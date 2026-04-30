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
#define PORT 8080
#define MAX_PENDING 10
#define USERS_FILE "data/users.dat"
#define TRIP_HISTORY_FILE "data/trip_history.txt"

pthread_mutex_t session_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t driver_mutex = PTHREAD_MUTEX_INITIALIZER;
static SystemGridMap* grid_shm = NULL;
static sem_t* driver_pool_sem = NULL;

int user_sockets[2000] = {0};
volatile int user_responses[2000] = {0}; // 0 = waiting, 1 = accept, 2 = reject

int authenticate_user(const char* username, const char* password, UserRecord* out_user) {
    FILE *fp = fopen(USERS_FILE, "r");
    if (!fp) return 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        UserRecord temp; int role_int;
        if (sscanf(line, "%d %31s %63s %d %d", &temp.user_id, temp.username, temp.password, &role_int, &temp.is_banned) == 5) {
            temp.role = (UserRole)role_int;
            if (strcmp(temp.username, username) == 0 && strcmp(temp.password, password) == 0) {
                *out_user = temp; fclose(fp); return 1;
            }
        }
    }
    fclose(fp); return 0;
}

void log_trip(int rider_id, int driver_id, int sx, int sy, int dx, int dy, int fare) {
    int fd = open(TRIP_HISTORY_FILE, O_WRONLY | O_APPEND | O_CREAT, 0666);
    if (fd < 0) return;
    struct flock lock; memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK; lock.l_whence = SEEK_END;
    fcntl(fd, F_SETLKW, &lock);
    char entry[256];
    sprintf(entry, "TRIP %d %d %d %d %d %d %d\n", rider_id, driver_id, sx, sy, dx, dy, fare);
    write(fd, entry, strlen(entry));
    lock.l_type = F_UNLCK; fcntl(fd, F_SETLK, &lock); close(fd);
}

void update_driver_status(int driver_id, DriverStatus status, int x, int y) {
    pthread_mutex_lock(&driver_mutex);
    int driver_index = -1;
    for (int i = 0; i < MAX_DRIVERS; i++) {
        if (grid_shm->grid[i].driver_id == driver_id || grid_shm->grid[i].status == STATUS_OFFLINE) {
            driver_index = i; break;
        }
    }
    if (driver_index != -1) {
        DriverStatus old_status = grid_shm->grid[driver_index].status;
        grid_shm->grid[driver_index].driver_id = driver_id;
        grid_shm->grid[driver_index].status = status;
        grid_shm->grid[driver_index].current_loc.x = x;
        grid_shm->grid[driver_index].current_loc.y = y;
        if (old_status != STATUS_AVAILABLE && status == STATUS_AVAILABLE) sem_post(driver_pool_sem);
    }
    pthread_mutex_unlock(&driver_mutex);
}

int request_ride(int rider_id, int rider_x, int rider_y, int* exclude_list, int exclude_count) {
    printf("Searching for nearest driver for Rider %d...\n", rider_id);
    if (sem_trywait(driver_pool_sem) != 0) return -1;
    pthread_mutex_lock(&driver_mutex);
    int matched_driver = -1; int best_index = -1; double min_dist = 9999999.0;
    for (int i = 0; i < MAX_DRIVERS; i++) {
        if (grid_shm->grid[i].status == STATUS_AVAILABLE) {
            int is_excluded = 0;
            for (int j = 0; j < exclude_count; j++) {
                if (grid_shm->grid[i].driver_id == exclude_list[j]) { is_excluded = 1; break; }
            }
            if (is_excluded) continue;
            long long dx = (long long)grid_shm->grid[i].current_loc.x - (long long)rider_x;
            long long dy = (long long)grid_shm->grid[i].current_loc.y - (long long)rider_y;
            double dist = (double)(dx * dx) + (double)(dy * dy);
            if (dist < min_dist) { min_dist = dist; best_index = i; matched_driver = grid_shm->grid[i].driver_id; }
        }
    }
    if (best_index != -1) grid_shm->grid[best_index].status = STATUS_ON_TRIP;
    pthread_mutex_unlock(&driver_mutex);
    if (matched_driver == -1) sem_post(driver_pool_sem);
    return matched_driver;
}

// Signal handler for graceful shutdown.
// Ensures shared memory and semaphores are unlinked on exit.
void cleanup_and_exit(int sig) {
    (void)sig; // Silence unused warning
    printf("Shutting down. Cleaning up IPC resources...\n");
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

    // Semaphore to manage driver pool and prevent race conditions 
    driver_pool_sem = sem_open(SEM_POOL_NAME, O_CREAT, 0666, 0);
    if (driver_pool_sem == SEM_FAILED) {
        perror("sem_open failed");
        exit(1);
    }

    // 3. System initialized
    printf("IPC and Synchronization primitives initialized.\n");
    

}

// Thread function for handling client requests 
void* handle_client(void* arg) {
    int client_sock = *(int*)arg;
    free(arg);

    MessagePacket packet;
    int authenticated = 0;
    UserRecord current_user;

    printf("Client connected. Waiting for authentication... (Socket: %d)\n", client_sock);

    while (1) {
        int bytes_read = recv(client_sock, &packet, sizeof(MessagePacket), 0);
        if (bytes_read <= 0) {
            printf("Client on socket %d disconnected abruptly.\n", client_sock);
            
            // Resource cleanup for disconnected drivers
            if (authenticated && current_user.role == ROLE_DRIVER) {
                printf("Emergency Cleanup: Offlining Ghost Driver %d\n", current_user.user_id);
                update_driver_status(current_user.user_id, STATUS_OFFLINE, 0, 0);
            }
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
                        printf("Rejected banned user '%s'.\n", username);
                    } else {
                        pthread_mutex_lock(&session_mutex);
                        int is_logged_in = (user_sockets[current_user.user_id] != 0);
                        if (is_logged_in) {
                            pthread_mutex_unlock(&session_mutex);
                            // We check if this user is already logged in elsewhere.
                            // This prevents multiple people using the same account simultaneously.
                            packet.type = MSG_ERROR;
                            strcpy(packet.payload, "Already logged in from another session.");
                            send(client_sock, &packet, sizeof(MessagePacket), 0);
                            printf("Rejected duplicate login for '%s'.\n", username);
                        } else {
                            authenticated = 1;
                            user_sockets[current_user.user_id] = client_sock;
                            pthread_mutex_unlock(&session_mutex);
                            packet.type = MSG_AUTH_RES;
                            sprintf(packet.payload, "%d %d", current_user.user_id, current_user.role);
                            send(client_sock, &packet, sizeof(MessagePacket), 0);
                            printf("User '%s' (ID: %d, Role: %d) authenticated.\n", 
                                    current_user.username, current_user.user_id, current_user.role);
                        }
                    }
                } else {
                    packet.type = MSG_ERROR;
                    strcpy(packet.payload, "Invalid credentials.");
                    send(client_sock, &packet, sizeof(MessagePacket), 0);
                    printf("Failed login attempt for '%s'.\n", username);
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
                printf("User ID %d cleanly disconnected.\n", current_user.user_id);
                pthread_mutex_lock(&session_mutex);
                user_sockets[current_user.user_id] = 0;
                pthread_mutex_unlock(&session_mutex);
                if (current_user.role == ROLE_DRIVER) {
                    update_driver_status(current_user.user_id, STATUS_OFFLINE, 0, 0);
                }
                break;
            } 
            else if (packet.type == MSG_LOC_UPDATE && current_user.role == ROLE_DRIVER) {
                int status, x, y;
                if (sscanf(packet.payload, "%d %d %d", &status, &x, &y) == 3) {
                    update_driver_status(current_user.user_id, (DriverStatus)status, x, y);
                    printf("Driver %d updated status to %d at (%d,%d)\n", 
                            current_user.user_id, status, x, y);
                }
            }
            else if (packet.type == MSG_RIDE_ACCEPT && current_user.role == ROLE_DRIVER) {
                pthread_mutex_lock(&session_mutex);
                user_responses[current_user.user_id] = 1;
                pthread_mutex_unlock(&session_mutex);
                printf("Driver %d ACCEPTED the ride.\n", current_user.user_id);
            }
            else if (packet.type == MSG_RIDE_REJECT && current_user.role == ROLE_DRIVER) {
                pthread_mutex_lock(&session_mutex);
                user_responses[current_user.user_id] = 2;
                pthread_mutex_unlock(&session_mutex);
                printf("Driver %d REJECTED the ride.\n", current_user.user_id);
            }
            else if (packet.type == MSG_RIDE_REQ && current_user.role == ROLE_RIDER) {
                int sx, sy, dx, dy;
                // Parse the 4 geographic inputs from Rider
                if (sscanf(packet.payload, "%d %d %d %d", &sx, &sy, &dx, &dy) == 4) {
                    
                    int exclude_list[MAX_DRIVERS];
                    int exclude_count = 0;
                    int trip_started = 0;

                    // This is the matching loop. We keep searching for the nearest driver, 
                    // and if they reject or don't respond, we try the next one until a match is made.
                    while (!trip_started) {
                        // Match driver based on Start Location (sx, sy)
                        int driver_id = request_ride(current_user.user_id, sx, sy, exclude_list, exclude_count);
                        
                        if (driver_id != -1) {
                            pthread_mutex_lock(&session_mutex);
                            int driver_sock = user_sockets[driver_id];
                            pthread_mutex_unlock(&session_mutex);

                            // Send Push Notification OFFER to Driver
                            packet.type = MSG_RIDE_OFFER;
                            sprintf(packet.payload, "%d %d %d %d", sx, sy, dx, dy);
                            pthread_mutex_lock(&session_mutex);
                            user_responses[driver_id] = 0; // Reset response state
                            pthread_mutex_unlock(&session_mutex);
                            send(driver_sock, &packet, sizeof(MessagePacket), 0);
                            printf("Sent RIDE_OFFER to Driver %d. Waiting 10s for response...\n", driver_id);

                            int wait_time = 0;
                            int local_resp = 0;
                            while(wait_time < 10) {
                                pthread_mutex_lock(&session_mutex);
                                local_resp = user_responses[driver_id];
                                pthread_mutex_unlock(&session_mutex);
                                if (local_resp != 0) break;
                                sleep(1);
                                wait_time++;
                            }

                            if (local_resp == 1) {
                                trip_started = 1;
                                // Driver ACCEPTED!
                                packet.type = MSG_RIDE_MATCHED;
                                sprintf(packet.payload, "%d", driver_id);
                                send(client_sock, &packet, sizeof(MessagePacket), 0);
                                
                                printf("Driver %d ACCEPTED. Simulating trip from (%d,%d) to (%d,%d)...\n", 
                                        driver_id, sx, sy, dx, dy);
                                sleep(5); // Shorter simulated trip
                                
                                // 1. Calculate REALISTIC Base Fare (Manhattan Distance)
                                int dist_x = abs(dx - sx);
                                int dist_y = abs(dy - sy);
                                int distance_blocks = dist_x + dist_y;
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
                                printf("Trip finished! Rider %d reached destination.\n", current_user.user_id);
                                printf("         Distance: %d blocks | Base: $%d | Surge: %.1fx | FINAL CHARGE: $%d\n", 
                                        distance_blocks, base_fare, current_surge, final_fare);
                                
                                log_trip(current_user.user_id, driver_id, sx, sy, dx, dy, final_fare);
                                update_driver_status(driver_id, STATUS_AVAILABLE, dx, dy);
                            } else {
                                // Driver REJECTED or TIMEOUT
                                printf("Driver %d REJECTED or TIMEOUT. Trying next driver...\n", driver_id);
                                // Put driver back in pool so they can be matched again (or go offline)
                                update_driver_status(driver_id, STATUS_AVAILABLE, sx, sy);
                                exclude_list[exclude_count++] = driver_id;
                            }
                        } else {
                            packet.type = MSG_ERROR;
                            strcpy(packet.payload, "No drivers available at the moment.");
                            send(client_sock, &packet, sizeof(MessagePacket), 0);
                            break;
                        }
                    }
                }
            }
            else if (packet.type == MSG_ADMIN_ACTION) {
                if (current_user.role != ROLE_ADMIN) {
                    packet.type = MSG_ERROR;
                    strcpy(packet.payload, "Access denied: insufficient privileges.");
                    send(client_sock, &packet, sizeof(MessagePacket), 0);
                } else {
                    char target_user[32];
                    int new_status;
                    if (sscanf(packet.payload, "%31s %d", target_user, &new_status) == 2) {
                        int fd = open("data/users.dat", O_RDWR);
                        if (fd != -1) {
                            struct flock lock;
                            memset(&lock, 0, sizeof(lock));
                            lock.l_type = F_WRLCK;
                            lock.l_whence = SEEK_SET;
                            lock.l_start = 0;
                            lock.l_len = 0;
                            fcntl(fd, F_SETLKW, &lock);

                            FILE *fp = fdopen(fd, "r");
                            FILE *ftemp = fopen("data/users_temp.dat", "w");
                            if (fp && ftemp) {
                                char line[256];
                                int found = 0;
                                rewind(fp);
                                while (fgets(line, sizeof(line), fp)) {
                                    int id, role, banned;
                                    char uname[32], pword[64];
                                    if (sscanf(line, "%d %31s %63s %d %d", &id, uname, pword, &role, &banned) == 5) {
                                        if (strcmp(uname, target_user) == 0) {
                                            fprintf(ftemp, "%d %s %s %d %d\n", id, uname, pword, role, new_status);
                                            found = 1;
                                        } else {
                                            fputs(line, ftemp);
                                        }
                                    } else {
                                        fputs(line, ftemp);
                                    }
                                }
                                fclose(ftemp);
                                
                                if (found) {
                                    rename("data/users_temp.dat", "data/users.dat");
                                    packet.type = MSG_ADMIN_ACTION;
                                    sprintf(packet.payload, "Success: User '%s' status updated to %s.", target_user, new_status ? "LOCKED" : "ACTIVE");
                                } else {
                                    remove("data/users_temp.dat");
                                    packet.type = MSG_ERROR;
                                    sprintf(packet.payload, "Error: User '%s' not found.", target_user);
                                }
                            } else {
                                if (ftemp) fclose(ftemp);
                                packet.type = MSG_ERROR;
                                strcpy(packet.payload, "Error processing user database.");
                            }
                            
                            lock.l_type = F_UNLCK;
                            fcntl(fd, F_SETLK, &lock);
                            if (fp) fclose(fp); // This also closes fd
                            
                            send(client_sock, &packet, sizeof(MessagePacket), 0);
                        } else {
                            packet.type = MSG_ERROR;
                            strcpy(packet.payload, "Error accessing user database.");
                            send(client_sock, &packet, sizeof(MessagePacket), 0);
                        }
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

    // Standard TCP socket setup: create, bind to port 8080, and start listening for connections.
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

    printf("Ride-Sharing Dispatch Server started!\n");
    printf("Listening on port %d...\n", PORT);

    while (1) {
        // Main server loop 
        int* client_sock = malloc(sizeof(int));
        *client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);

        if (*client_sock < 0) {
            perror("Accept failed");
            free(client_sock);
            continue;
        }

        // Handle the client request in a separate thread 
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
