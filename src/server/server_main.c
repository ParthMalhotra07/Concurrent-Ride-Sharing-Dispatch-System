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

// These are our global variables for keeping track of the server state
// We use mutexes to make sure different threads don't mess with the same data at once
pthread_mutex_t session_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t driver_mutex = PTHREAD_MUTEX_INITIALIZER;
static SystemGridMap* grid_shm = NULL;
static sem_t* driver_pool_sem = NULL;

int user_sockets[2000] = {0};
volatile int user_responses[2000] = {0}; // 0 is waiting, 1 is accept, 2 is reject

/* 
   This function reads the users file and checks if the username and password match.
   It returns 1 if everything is okay and fills the user info, otherwise returns 0.
*/
int authenticate_user(const char* username, const char* password, UserRecord* out_user) {
    FILE *fp = fopen(USERS_FILE, "r");
    if (!fp) return 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        UserRecord temp; int role_int;
        if (sscanf(line, "%d %31s %63s %d %d", &temp.user_id, temp.username, temp.password, &role_int, &temp.is_banned) == 5) {
            temp.role = (UserRole)role_int;
            if (strcmp(temp.username, username) == 0 && strcmp(temp.password, password) == 0) {
                *out_user = temp; 
                fclose(fp); 
                return 1;
            }
        }
    }
    fclose(fp); 
    return 0;
}

/*
   We use this to save every finished trip into a text file.
   It uses fcntl locking to make sure two threads don't write at the same time.
*/
void log_trip(int rider_id, int driver_id, int sx, int sy, int dx, int dy, int fare) {
    int fd = open(TRIP_HISTORY_FILE, O_WRONLY | O_APPEND | O_CREAT, 0666);
    if (fd < 0) return;
    struct flock lock; 
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK; 
    lock.l_whence = SEEK_END;
    fcntl(fd, F_SETLKW, &lock);
    
    char entry[256];
    sprintf(entry, "TRIP %d %d %d %d %d %d %d\n", rider_id, driver_id, sx, sy, dx, dy, fare);
    write(fd, entry, strlen(entry));
    
    lock.l_type = F_UNLCK; 
    fcntl(fd, F_SETLK, &lock); 
    close(fd);
}

/*
   Updates where a driver is and what they are doing.
   If a driver becomes available, we increment the semaphore so riders can find them.
*/
void update_driver_status(int driver_id, DriverStatus status, int x, int y) {
    pthread_mutex_lock(&driver_mutex);
    int driver_index = -1;
    for (int i = 0; i < MAX_DRIVERS; i++) {
        if (grid_shm->grid[i].driver_id == driver_id || grid_shm->grid[i].status == STATUS_OFFLINE) {
            driver_index = i; 
            break;
        }
    }
    if (driver_index != -1) {
        DriverStatus old_status = grid_shm->grid[driver_index].status;
        grid_shm->grid[driver_index].driver_id = driver_id;
        grid_shm->grid[driver_index].status = status;
        grid_shm->grid[driver_index].current_loc.x = x;
        grid_shm->grid[driver_index].current_loc.y = y;
        if (old_status != STATUS_AVAILABLE && status == STATUS_AVAILABLE) {
            sem_post(driver_pool_sem);
        }
    }
    pthread_mutex_unlock(&driver_mutex);
}

/*
   This is the core matchmaking logic. It looks through the shared memory
   to find the closest driver who isn't on the exclude list.
*/
int request_ride(int rider_id, int rider_x, int rider_y, int* exclude_list, int exclude_count) {
    printf("Searching for nearest driver for Rider %d...\n", rider_id);
    if (sem_trywait(driver_pool_sem) != 0) return -1;
    
    pthread_mutex_lock(&driver_mutex);
    int matched_driver = -1; 
    int best_index = -1; 
    double min_dist = 9999999.0;
    
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
            
            if (dist < min_dist) { 
                min_dist = dist; 
                best_index = i; 
                matched_driver = grid_shm->grid[i].driver_id; 
            }
        }
    }
    if (best_index != -1) grid_shm->grid[best_index].status = STATUS_ON_TRIP;
    pthread_mutex_unlock(&driver_mutex);
    
    if (matched_driver == -1) sem_post(driver_pool_sem);
    return matched_driver;
}

/*
   Handles the login process when a client sends their username and password.
   It also checks if the user is already logged in or if they are banned.
*/
void handle_auth_req(int client_sock, MessagePacket* packet, int* authenticated, UserRecord* current_user) {
    char username[32]; 
    char password[64];
    if (sscanf(packet->payload, "%31s %63s", username, password) == 2) {
        if (authenticate_user(username, password, current_user)) {
            if (current_user->is_banned) {
                packet->type = MSG_ERROR; 
                strcpy(packet->payload, "Account banned.");
                send(client_sock, packet, sizeof(MessagePacket), 0);
                printf("Rejected banned user '%s'.\n", username);
            } else {
                pthread_mutex_lock(&session_mutex);
                if (user_sockets[current_user->user_id] != 0) {
                    pthread_mutex_unlock(&session_mutex);
                    packet->type = MSG_ERROR; 
                    strcpy(packet->payload, "Already logged in elsewhere.");
                    send(client_sock, packet, sizeof(MessagePacket), 0);
                } else {
                    *authenticated = 1;
                    user_sockets[current_user->user_id] = client_sock;
                    pthread_mutex_unlock(&session_mutex);
                    packet->type = MSG_AUTH_RES;
                    sprintf(packet->payload, "%d %d", current_user->user_id, current_user->role);
                    send(client_sock, packet, sizeof(MessagePacket), 0);
                    printf("User '%s' authenticated.\n", current_user->username);
                }
            }
        } else {
            packet->type = MSG_ERROR; 
            strcpy(packet->payload, "Invalid credentials.");
            send(client_sock, packet, sizeof(MessagePacket), 0);
        }
    }
}

/*
   This manages the whole ride request flow. It finds a driver, sends them an offer,
   waits for their response, and then calculates the fare if they accept.
*/
void handle_ride_request(int client_sock, MessagePacket* packet, UserRecord* current_user) {
    int sx, sy, dx, dy;
    if (sscanf(packet->payload, "%d %d %d %d", &sx, &sy, &dx, &dy) == 4) {
        int exclude_list[MAX_DRIVERS]; 
        int exclude_count = 0; 
        int trip_started = 0;
        
        while (!trip_started) {
            int driver_id = request_ride(current_user->user_id, sx, sy, exclude_list, exclude_count);
            if (driver_id == -1) {
                packet->type = MSG_ERROR; 
                strcpy(packet->payload, "No drivers available.");
                send(client_sock, packet, sizeof(MessagePacket), 0); 
                break;
            }
            
            pthread_mutex_lock(&session_mutex);
            int driver_sock = user_sockets[driver_id];
            user_responses[driver_id] = 0;
            pthread_mutex_unlock(&session_mutex);
            
            packet->type = MSG_RIDE_OFFER; 
            sprintf(packet->payload, "%d %d %d %d", sx, sy, dx, dy);
            send(driver_sock, packet, sizeof(MessagePacket), 0);
            
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
                packet->type = MSG_RIDE_MATCHED; 
                sprintf(packet->payload, "%d", driver_id);
                send(client_sock, packet, sizeof(MessagePacket), 0);
                
                sleep(5); // Simulate the trip happening
                int base_fare = 15 + ((abs(dx - sx) + abs(dy - sy)) * 5);
                double surge = 1.0;
                
                // Read the current surge price from shared memory
                int s_fd = shm_open(SURGE_SHM_NAME, O_RDONLY, 0666);
                if (s_fd != -1) {
                    SurgeState* s_shm = mmap(NULL, sizeof(SurgeState), PROT_READ, MAP_SHARED, s_fd, 0);
                    if (s_shm != MAP_FAILED) { 
                        surge = s_shm->multiplier; 
                        munmap(s_shm, sizeof(SurgeState)); 
                    }
                    close(s_fd);
                }
                
                int final_fare = (int)(base_fare * surge);
                log_trip(current_user->user_id, driver_id, sx, sy, dx, dy, final_fare);
                update_driver_status(driver_id, STATUS_AVAILABLE, dx, dy);
                printf("Trip finished for Rider %d. Fare: $%d\n", current_user->user_id, final_fare);
            } else {
                update_driver_status(driver_id, STATUS_AVAILABLE, sx, sy);
                exclude_list[exclude_count++] = driver_id;
            }
        }
    }
}

/*
   Special commands that only the Admin can run, like banning a user.
   It updates the users file safely using file locking.
*/
void handle_admin_action(int client_sock, MessagePacket* packet) {
    char target[32]; 
    int status;
    if (sscanf(packet->payload, "%31s %d", target, &status) == 2) {
        int fd = open("data/users.dat", O_RDWR);
        if (fd == -1) return;
        
        struct flock lock; 
        memset(&lock, 0, sizeof(lock));
        lock.l_type = F_WRLCK; 
        fcntl(fd, F_SETLKW, &lock);
        
        FILE *fp = fdopen(fd, "r"); 
        FILE *ftemp = fopen("data/users_temp.dat", "w");
        if (fp && ftemp) {
            char line[256]; 
            int found = 0;
            while (fgets(line, sizeof(line), fp)) {
                int id, r, b; 
                char u[32], p[64];
                if (sscanf(line, "%d %31s %63s %d %d", &id, u, p, &r, &b) == 5 && strcmp(u, target) == 0) {
                    fprintf(ftemp, "%d %s %s %d %d\n", id, u, p, r, status); 
                    found = 1;
                } else {
                    fputs(line, ftemp);
                }
            }
            fclose(ftemp);
            if (found) {
                rename("data/users_temp.dat", "data/users.dat");
                packet->type = MSG_ADMIN_ACTION; 
                sprintf(packet->payload, "User %s status updated successfully.", target);
            } else { 
                remove("data/users_temp.dat"); 
                packet->type = MSG_ERROR; 
                sprintf(packet->payload, "Could not find that user."); 
            }
        }
        lock.l_type = F_UNLCK; 
        fcntl(fd, F_SETLK, &lock); 
        fclose(fp);
        send(client_sock, packet, sizeof(MessagePacket), 0);
    }
}

/*
   Cleans up everything before the server closes.
   We make sure to unlink shared memory so it doesn't leak.
*/
void cleanup_and_exit(int sig) {
    (void)sig; 
    printf("Shutting down. Cleaning up IPC resources...\n");
    if (grid_shm) munmap(grid_shm, SHM_SIZE);
    shm_unlink(SHM_NAME); 
    sem_unlink(SEM_POOL_NAME); 
    exit(0);
}

/*
   Sets up our shared memory and semaphores when the server starts.
*/
void setup_ipc() {
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    ftruncate(shm_fd, SHM_SIZE);
    grid_shm = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    for(int i = 0; i < MAX_DRIVERS; i++) { 
        grid_shm->grid[i].status = STATUS_OFFLINE; 
        grid_shm->grid[i].driver_id = 0; 
    }
    sem_unlink(SEM_POOL_NAME);
    driver_pool_sem = sem_open(SEM_POOL_NAME, O_CREAT, 0666, 0);
    printf("IPC and Synchronization primitives initialized.\n");
}

/*
   Each connected user gets their own thread running this function.
   It waits for messages and calls the right helper function to handle them.
*/
void* handle_client(void* arg) {
    int client_sock = *(int*)arg; 
    free(arg);
    MessagePacket packet; 
    int authenticated = 0; 
    UserRecord current_user;
    printf("New client connected on Socket %d.\n", client_sock);

    while (1) {
        if (recv(client_sock, &packet, sizeof(MessagePacket), 0) <= 0) {
            if (authenticated && current_user.role == ROLE_DRIVER) {
                update_driver_status(current_user.user_id, STATUS_OFFLINE, 0, 0);
            }
            break;
        }

        if (packet.type == MSG_AUTH_REQ) {
            handle_auth_req(client_sock, &packet, &authenticated, &current_user);
        } else if (!authenticated) {
            packet.type = MSG_ERROR; 
            strcpy(packet.payload, "Authenticate first.");
            send(client_sock, &packet, sizeof(MessagePacket), 0);
        } else {
            switch(packet.type) {
                case MSG_DISCONNECT:
                    pthread_mutex_lock(&session_mutex); 
                    user_sockets[current_user.user_id] = 0; 
                    pthread_mutex_unlock(&session_mutex);
                    if (current_user.role == ROLE_DRIVER) {
                        update_driver_status(current_user.user_id, STATUS_OFFLINE, 0, 0);
                    }
                    goto cleanup;
                case MSG_LOC_UPDATE:
                    if (current_user.role == ROLE_DRIVER) {
                        int s, x, y; 
                        if (sscanf(packet.payload, "%d %d %d", &s, &x, &y) == 3) {
                            update_driver_status(current_user.user_id, (DriverStatus)s, x, y);
                        }
                    }
                    break;
                case MSG_RIDE_ACCEPT:
                    if (current_user.role == ROLE_DRIVER) { 
                        pthread_mutex_lock(&session_mutex); 
                        user_responses[current_user.user_id] = 1; 
                        pthread_mutex_unlock(&session_mutex); 
                    }
                    break;
                case MSG_RIDE_REJECT:
                    if (current_user.role == ROLE_DRIVER) { 
                        pthread_mutex_lock(&session_mutex); 
                        user_responses[current_user.user_id] = 2; 
                        pthread_mutex_unlock(&session_mutex); 
                    }
                    break;
                case MSG_RIDE_REQ:
                    if (current_user.role == ROLE_RIDER) handle_ride_request(client_sock, &packet, &current_user);
                    break;
                case MSG_ADMIN_ACTION:
                    if (current_user.role == ROLE_ADMIN) handle_admin_action(client_sock, &packet);
                    break;
                default:
                    break;
            }
        }
    }
cleanup:
    close(client_sock); 
    return NULL;
}

/*
   Starts the server, sets up IPC, and listens for new incoming connections.
*/
int main() {
    signal(SIGINT, cleanup_and_exit); 
    setup_ipc();
    
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; 
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr; 
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; 
    addr.sin_addr.s_addr = INADDR_ANY; 
    addr.sin_port = htons(PORT);
    
    bind(server_sock, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_sock, MAX_PENDING);
    
    printf("Server is up and running on port %d.\n", PORT);
    while (1) {
        int* csock = malloc(sizeof(int));
        *csock = accept(server_sock, NULL, NULL);
        pthread_t tid; 
        pthread_create(&tid, NULL, handle_client, csock); 
        pthread_detach(tid);
    }
    return 0;
}
