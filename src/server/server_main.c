#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>
#include "../common/structs.h"
#include "auth.h"
#include "ledger.h"
#include "match_core.h"

#define PORT 8080
#define MAX_PENDING 10

static SystemGridMap* grid_shm = NULL;
static sem_t* driver_pool_sem = NULL;

void cleanup_and_exit(int sig) {
    (void)sig; // Silence unused warning
    printf("\n[SERVER] Shutting down. Cleaning up IPC resources...\n");
    if (grid_shm != MAP_FAILED && grid_shm != NULL) {
        munmap(grid_shm, SHM_SIZE);
    }
    shm_unlink(SHM_NAME);
    
    if (driver_pool_sem != SEM_FAILED && driver_pool_sem != NULL) {
        sem_close(driver_pool_sem);
    }
    sem_unlink(SEM_POOL_NAME);
    exit(0);
}

void setup_ipc() {
    // 1. Create/Open Shared Memory
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open failed");
        exit(1);
    }

    //allocate the size to the shared memory = shm_size 
    if (ftruncate(shm_fd, SHM_SIZE) == -1) {
        perror("ftruncate failed");
        exit(1);
    }

    //map the shared memory to server's adddress space 
    grid_shm = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (grid_shm == MAP_FAILED) {
        perror("mmap failed");
        exit(1);
    }
    
    // Initialize the grid state
    for(int i = 0; i < MAX_DRIVERS; i++) {
        grid_shm->grid[i].status = STATUS_OFFLINE;
        grid_shm->grid[i].driver_id = 0;
    }

    // 2. Initialize Named Semaphore (start at 0, goes up as drivers come online)
    sem_unlink(SEM_POOL_NAME); // Clean up from previous rough closures

    //semaphore helps me ensure there is no race condintion 
    driver_pool_sem = sem_open(SEM_POOL_NAME, O_CREAT, 0666, 0);
    if (driver_pool_sem == SEM_FAILED) {
        perror("sem_open failed");
        exit(1);
    }

    // 3. Hand pointers over to the matching core
    init_match_core(grid_shm, driver_pool_sem);
    init_ledger();
    
    printf("[SERVER] IPC and Synchronization primitives initialized.\n");
}

//function for individually handling each request 
void* handle_client(void* arg) {
    int client_sock = *(int*)arg;
    free(arg);

    MessagePacket packet;
    int authenticated = 0;
    UserRecord current_user;

    printf("[SERVER] Client connected. Waiting for authentication... (Socket: %d)\n", client_sock);

    while (1) {
        int bytes_read = recv(client_sock, &packet, sizeof(MessagePacket), 0);
        if (bytes_read <= 0) {
            printf("[SERVER] Client on socket %d disconnected abruptly.\n", client_sock);
            break;
        }

        if (packet.type == MSG_AUTH_REQ) {
            char username[32];
            char password[64];
            if (sscanf(packet.payload, "%31s %63s", username, password) == 2) {
                if (authenticate_user(username, password, &current_user)) {
                    if (current_user.is_banned) {
                        packet.type = MSG_ERROR;
                        strcpy(packet.payload, "Account banned.");
                        send(client_sock, &packet, sizeof(MessagePacket), 0);
                        printf("[SERVER] Rejected banned user '%s'.\n", username);
                    } else {
                        authenticated = 1;
                        packet.type = MSG_AUTH_RES;
                        sprintf(packet.payload, "%d %d", current_user.user_id, current_user.role);
                        send(client_sock, &packet, sizeof(MessagePacket), 0);
                        printf("[SERVER] User '%s' (ID: %d, Role: %d) authenticated.\n", 
                                current_user.username, current_user.user_id, current_user.role);
                    }
                } else {
                    packet.type = MSG_ERROR;
                    strcpy(packet.payload, "Invalid credentials.");
                    send(client_sock, &packet, sizeof(MessagePacket), 0);
                    printf("[SERVER] Failed login attempt for '%s'.\n", username);
                }
            }
        } 
        else if (!authenticated) {
            packet.type = MSG_ERROR;
            strcpy(packet.payload, "Please authenticate first.");
            send(client_sock, &packet, sizeof(MessagePacket), 0);
        }
        else {
            if (packet.type == MSG_DISCONNECT) {
                printf("[SERVER] User ID %d cleanly disconnected.\n", current_user.user_id);
                if (current_user.role == ROLE_DRIVER) {
                    update_driver_status(current_user.user_id, STATUS_OFFLINE, 0, 0);
                }
                break;
            } 
            else if (packet.type == MSG_LOC_UPDATE && current_user.role == ROLE_DRIVER) {
                int status, x, y;
                if (sscanf(packet.payload, "%d %d %d", &status, &x, &y) == 3) {
                    update_driver_status(current_user.user_id, (DriverStatus)status, x, y);
                    printf("[SERVER] Driver %d updated status to %d at (%d,%d)\n", 
                            current_user.user_id, status, x, y);
                }
            }
            else if (packet.type == MSG_RIDE_REQ && current_user.role == ROLE_RIDER) {
                int sx, sy, dx, dy;
                // Parse the 4 geographic inputs from Rider
                if (sscanf(packet.payload, "%d %d %d %d", &sx, &sy, &dx, &dy) == 4) {
                    // Match driver based on Start Location (sx, sy)
                    int driver_id = request_ride(current_user.user_id, sx, sy);
                    
                    if (driver_id != -1) {
                        packet.type = MSG_RIDE_MATCHED;
                        sprintf(packet.payload, "%d", driver_id);
                        send(client_sock, &packet, sizeof(MessagePacket), 0);
                        
                        // Simulate a longer trip
                        printf("[SERVER] Driver %d is taking Rider %d from (%d,%d) to (%d,%d)...\n", 
                                driver_id, current_user.user_id, sx, sy, dx, dy);
                        sleep(15);
                        
                        // 1. Calculate REALISTIC Base Fare (Manhattan Distance)
                        // Distance = |x2 - x1| + |y2 - y1|
                        int dist_x = abs(dx - sx);
                        int dist_y = abs(dy - sy);
                        int distance_blocks = dist_x + dist_y;
                        
                        // $15 base pickup + $5 per block of distance
                        int base_fare = 15 + (distance_blocks * 5);
                        
                        // 2. Read Surge Pricing from TWO-WAY SHM
                        double current_surge = 1.0;
                        int surge_fd = shm_open(SURGE_SHM_NAME, O_RDONLY, 0666);
                        if (surge_fd != -1) {
                            SurgeState* surge_shm = mmap(NULL, sizeof(SurgeState), PROT_READ, MAP_SHARED, surge_fd, 0);
                            if (surge_shm != MAP_FAILED) {
                                current_surge = surge_shm->multiplier;
                                munmap(surge_shm, sizeof(SurgeState));
                            }
                            close(surge_fd);
                        }

                        int final_fare = (int)(base_fare * current_surge);
                        printf("[SERVER] Trip finished! Rider %d reached destination.\n", current_user.user_id);
                        printf("         Distance: %d blocks | Base: $%d | Surge: %.1fx | FINAL CHARGE: $%d\n", 
                                distance_blocks, base_fare, current_surge, final_fare);
                        
                        // Write exact geographic trip to ledger
                        log_trip(current_user.user_id, driver_id, sx, sy, dx, dy, final_fare);

                        // Reset driver to available at the NEW DROP-OFF location
                        update_driver_status(driver_id, STATUS_AVAILABLE, dx, dy);
                        printf("[SERVER] Driver %d is AVAILABLE again at new location (%d,%d).\n", 
                                driver_id, dx, dy);
                    } else {
                        packet.type = MSG_ERROR;
                        strcpy(packet.payload, "No drivers available near your location.");
                        send(client_sock, &packet, sizeof(MessagePacket), 0);
                    }
                }
            }
        }
    }

    close(client_sock);
    pthread_exit(NULL);
}

int main() {
    signal(SIGINT, cleanup_and_exit);
    signal(SIGTERM, cleanup_and_exit);

    setup_ipc();

    int server_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    if ((server_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation failed");
        cleanup_and_exit(0);
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; //listen to all network interfaces 
    server_addr.sin_port = htons(PORT);  //host to network short - helps to attach to the port 8080

    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_sock, MAX_PENDING) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    printf("==========================================\n");
    printf(" Ride-Sharing Dispatch Server started!\n");
    printf(" Listening on port %d...\n", PORT);
    printf("==========================================\n");

    while (1) {
        //infinte loop waiting for rider or driver to connectwith the server via message received 
        int* client_sock = malloc(sizeof(int));
        *client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);

        if (*client_sock < 0) {
            perror("Accept failed");
            free(client_sock);
            continue;
        }

        //creating a new thread to handle the current request so that if any other user tries to access
        //or send message the update is not lost so the new thread handles the current request and the original
        //proccess continues to be in the while(1) loop and waiting for new requests 
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, client_sock) != 0) {
            perror("Failed to create thread");
            free(client_sock);
        }
        pthread_detach(thread_id);
    }

    close(server_sock);
    return 0;
}
