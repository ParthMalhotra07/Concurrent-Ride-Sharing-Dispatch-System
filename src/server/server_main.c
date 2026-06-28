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
#define MAX_ACTIVE_OFFERS 100

// These are my global variables for keeping track of the server state
pthread_mutex_t session_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t driver_mutex = PTHREAD_MUTEX_INITIALIZER;
static SystemGridMap* grid_shm = NULL;
static sem_t* driver_pool_sem = NULL;

// Bounded arrays to prevent buffer overflows
int user_sockets[MAX_USERS] = {0};

// Condition variables and structure for tracking active ride offers
typedef struct {
    int driver_id;
    int rider_id;
    int status; // 0 = pending, 1 = accepted, 2 = rejected
    int in_use;
    pthread_cond_t cond;
    pthread_mutex_t mutex;
} ActiveOffer;

ActiveOffer active_offers[MAX_ACTIVE_OFFERS];
pthread_mutex_t offers_mutex = PTHREAD_MUTEX_INITIALIZER;

void init_offers() {
    for (int i = 0; i < MAX_ACTIVE_OFFERS; i++) {
        active_offers[i].in_use = 0;
        pthread_cond_init(&active_offers[i].cond, NULL);
        pthread_mutex_init(&active_offers[i].mutex, NULL);
    }
}

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

void log_trip(int rider_id, int driver_id, int sx, int sy, int dx, int dy, int fare) {
    int fd = open(TRIP_HISTORY_FILE, O_WRONLY | O_APPEND | O_CREAT, 0666);
    if (fd < 0) return;
    struct flock lock; 
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK; 
    lock.l_whence = SEEK_END;
    fcntl(fd, F_SETLKW, &lock);
    
    char entry[256];
    snprintf(entry, sizeof(entry), "TRIP %d %d %d %d %d %d %d\n", rider_id, driver_id, sx, sy, dx, dy, fare);
    write(fd, entry, strlen(entry));
    
    lock.l_type = F_UNLCK; 
    fcntl(fd, F_SETLK, &lock); 
    close(fd);
}

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

void handle_auth_req(int client_sock, MessagePacket* packet, int* authenticated, UserRecord* current_user) {
    char username[32]; 
    char password[64];
    if (sscanf(packet->payload, "%31s %63s", username, password) == 2) {
        if (authenticate_user(username, password, current_user)) {
            if (current_user->user_id < 0 || current_user->user_id >= MAX_USERS) {
                packet->type = MSG_ERROR; 
                strncpy(packet->payload, "Internal server error: Invalid User ID.", 255);
                send(client_sock, packet, sizeof(MessagePacket), 0);
                return;
            }
            
            if (current_user->is_banned) {
                packet->type = MSG_ERROR; 
                strncpy(packet->payload, "Account banned.", 255);
                send(client_sock, packet, sizeof(MessagePacket), 0);
                printf("Rejected banned user: %s\n", username);
            } else {
                pthread_mutex_lock(&session_mutex);
                if (user_sockets[current_user->user_id] != 0) {
                    pthread_mutex_unlock(&session_mutex);
                    packet->type = MSG_ERROR; 
                    strncpy(packet->payload, "Already logged in elsewhere.", 255);
                    send(client_sock, packet, sizeof(MessagePacket), 0);
                } else {
                    *authenticated = 1;
                    user_sockets[current_user->user_id] = client_sock;
                    pthread_mutex_unlock(&session_mutex);
                    packet->type = MSG_AUTH_RES;
                    snprintf(packet->payload, sizeof(packet->payload), "%d %d", current_user->user_id, current_user->role);
                    send(client_sock, packet, sizeof(MessagePacket), 0);
                    printf("User '%s' authenticated.\n", current_user->username);
                }
            }
        } else {
            packet->type = MSG_ERROR; 
            strncpy(packet->payload, "Invalid credentials.", 255);
            send(client_sock, packet, sizeof(MessagePacket), 0);
        }
    }
}

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
                strncpy(packet->payload, "No drivers available.", 255);
                send(client_sock, packet, sizeof(MessagePacket), 0); 
                break;
            }
            
            pthread_mutex_lock(&session_mutex);
            int driver_sock = user_sockets[driver_id];
            pthread_mutex_unlock(&session_mutex);
            
            // Create a pending offer and wait for driver response using Condition Variables
            pthread_mutex_lock(&offers_mutex);
            int offer_slot = -1;
            for (int i = 0; i < MAX_ACTIVE_OFFERS; i++) {
                if (!active_offers[i].in_use) {
                    offer_slot = i;
                    active_offers[i].in_use = 1;
                    active_offers[i].driver_id = driver_id;
                    active_offers[i].rider_id = current_user->user_id;
                    active_offers[i].status = 0;
                    break;
                }
            }
            pthread_mutex_unlock(&offers_mutex);
            
            if (offer_slot == -1) {
                packet->type = MSG_ERROR; 
                strncpy(packet->payload, "Server too busy handling offers.", 255);
                send(client_sock, packet, sizeof(MessagePacket), 0); 
                break;
            }

            packet->type = MSG_RIDE_OFFER; 
            snprintf(packet->payload, sizeof(packet->payload), "%d %d %d %d", sx, sy, dx, dy);
            send(driver_sock, packet, sizeof(MessagePacket), 0);
            
            // Wait for response using CV instead of CPU-wasting spinlock
            pthread_mutex_lock(&active_offers[offer_slot].mutex);
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 10; // 10 seconds timeout
            
            int cv_res = 0;
            while (active_offers[offer_slot].status == 0 && cv_res == 0) {
                cv_res = pthread_cond_timedwait(&active_offers[offer_slot].cond, &active_offers[offer_slot].mutex, &ts);
            }
            int driver_response = active_offers[offer_slot].status;
            pthread_mutex_unlock(&active_offers[offer_slot].mutex);
            
            // Cleanup offer slot
            pthread_mutex_lock(&offers_mutex);
            active_offers[offer_slot].in_use = 0;
            pthread_mutex_unlock(&offers_mutex);

            if (driver_response == 1) {
                trip_started = 1; 
                packet->type = MSG_RIDE_MATCHED; 
                snprintf(packet->payload, sizeof(packet->payload), "%d", driver_id);
                send(client_sock, packet, sizeof(MessagePacket), 0);
                
                sleep(3); // Simulate trip (shortened for demonstration)
                int base_fare = 15 + ((abs(dx - sx) + abs(dy - sy)) * 5);
                double surge = 1.0;
                
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
                printf("Trip complete for Rider %d. Fare: $%d\n", current_user->user_id, final_fare);
            } else {
                update_driver_status(driver_id, STATUS_AVAILABLE, sx, sy);
                exclude_list[exclude_count++] = driver_id;
            }
        }
    }
}

void process_driver_response(int driver_id, int status) {
    pthread_mutex_lock(&offers_mutex);
    for (int i = 0; i < MAX_ACTIVE_OFFERS; i++) {
        if (active_offers[i].in_use && active_offers[i].driver_id == driver_id) {
            pthread_mutex_lock(&active_offers[i].mutex);
            active_offers[i].status = status; // 1 = accept, 2 = reject
            pthread_cond_signal(&active_offers[i].cond);
            pthread_mutex_unlock(&active_offers[i].mutex);
            break;
        }
    }
    pthread_mutex_unlock(&offers_mutex);
}

void handle_trip_history_req(int client_sock, MessagePacket* packet, UserRecord* current_user) {
    FILE *fp = fopen("data/trip_history.txt", "r");
    if (!fp) {
        packet->type = MSG_TRIP_HISTORY_END;
        send(client_sock, packet, sizeof(MessagePacket), 0);
        return;
    }
    
    // Acquire Advisory Read Lock
    int fd = fileno(fp);
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_RDLCK;
    fcntl(fd, F_SETLKW, &lock);
    
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        int r_id, d_id, sx, sy, ex, ey, fare;
        if (sscanf(line, "TRIP %d %d %d %d %d %d %d", &r_id, &d_id, &sx, &sy, &ex, &ey, &fare) == 7) {
            // Filter based on role
            int should_send = 0;
            if (current_user->role == ROLE_ADMIN) should_send = 1;
            else if (current_user->role == ROLE_RIDER && r_id == current_user->user_id) should_send = 1;
            else if (current_user->role == ROLE_DRIVER && d_id == current_user->user_id) should_send = 1;
            
            if (should_send) {
                packet->type = MSG_TRIP_HISTORY_RES;
                snprintf(packet->payload, sizeof(packet->payload), "%d %d %d %d %d %d %d", r_id, d_id, sx, sy, ex, ey, fare);
                send(client_sock, packet, sizeof(MessagePacket), 0);
            }
        }
    }
    
    // Release Lock
    lock.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &lock);
    fclose(fp);
    
    packet->type = MSG_TRIP_HISTORY_END;
    send(client_sock, packet, sizeof(MessagePacket), 0);
}

void handle_revenue_req(int client_sock, MessagePacket* packet) {
    FILE *fp = fopen("data/trip_history.txt", "r");
    long total_revenue = 0;
    int total_trips = 0;
    
    if (fp) {
        int fd = fileno(fp);
        struct flock lock;
        memset(&lock, 0, sizeof(lock));
        lock.l_type = F_RDLCK;
        fcntl(fd, F_SETLKW, &lock);
        
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            int r_id, d_id, sx, sy, ex, ey, fare;
            if (sscanf(line, "TRIP %d %d %d %d %d %d %d", &r_id, &d_id, &sx, &sy, &ex, &ey, &fare) == 7) {
                total_revenue += fare;
                total_trips++;
            }
        }
        lock.l_type = F_UNLCK;
        fcntl(fd, F_SETLK, &lock);
        fclose(fp);
    }
    
    packet->type = MSG_REVENUE_REPORT_RES;
    snprintf(packet->payload, sizeof(packet->payload), "%d %ld", total_trips, total_revenue);
    send(client_sock, packet, sizeof(MessagePacket), 0);
}

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
                snprintf(packet->payload, sizeof(packet->payload), "User %s status updated successfully.", target);
            } else { 
                remove("data/users_temp.dat"); 
                packet->type = MSG_ERROR; 
                strncpy(packet->payload, "Could not find that user.", 255); 
            }
        }
        lock.l_type = F_UNLCK; 
        fcntl(fd, F_SETLK, &lock); 
        fclose(fp);
        send(client_sock, packet, sizeof(MessagePacket), 0);
    }
}

void cleanup_and_exit(int sig) {
    (void)sig; 
    printf("\nStopping server and cleaning up IPC resources...\n");
    if (grid_shm) munmap(grid_shm, SHM_SIZE);
    shm_unlink(SHM_NAME); 
    sem_unlink(SEM_POOL_NAME); 
    exit(0);
}

void setup_ipc() {
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) { perror("shm_open failed"); exit(1); }
    if (ftruncate(shm_fd, SHM_SIZE) == -1) { perror("ftruncate failed"); exit(1); }
    grid_shm = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (grid_shm == MAP_FAILED) { perror("mmap failed"); exit(1); }
    
    for(int i = 0; i < MAX_DRIVERS; i++) { 
        grid_shm->grid[i].status = STATUS_OFFLINE; 
        grid_shm->grid[i].driver_id = 0; 
    }
    sem_unlink(SEM_POOL_NAME);
    driver_pool_sem = sem_open(SEM_POOL_NAME, O_CREAT, 0666, 0);
    if (driver_pool_sem == SEM_FAILED) { perror("sem_open failed"); exit(1); }
    printf("System memory and locks ready.\n");
    init_offers();
}

void* handle_client(void* arg) {
    int client_sock = *(int*)arg; 
    free(arg);
    MessagePacket packet; 
    int authenticated = 0; 
    UserRecord current_user;
    printf("New client connected on socket %d.\n", client_sock);

    while (1) {
        if (recv(client_sock, &packet, sizeof(MessagePacket), 0) <= 0) {
            // FIX: Session Leakage Fix! We must clear the socket from the array on disconnect
            if (authenticated) {
                printf("User %s (ID: %d) connection lost.\n", current_user.username, current_user.user_id);
                pthread_mutex_lock(&session_mutex);
                user_sockets[current_user.user_id] = 0;
                pthread_mutex_unlock(&session_mutex);
                if (current_user.role == ROLE_DRIVER) {
                    update_driver_status(current_user.user_id, STATUS_OFFLINE, 0, 0);
                }
            } else {
                printf("Client on socket %d disconnected.\n", client_sock);
            }
            break;
        }

        if (packet.type == MSG_AUTH_REQ) {
            handle_auth_req(client_sock, &packet, &authenticated, &current_user);
        } else if (!authenticated) {
            packet.type = MSG_ERROR; 
            strncpy(packet.payload, "Authenticate first.", 255);
            send(client_sock, &packet, sizeof(MessagePacket), 0);
        } else {
            switch(packet.type) {
                case MSG_DISCONNECT:
                    printf("User %s (ID: %d) requested logout.\n", current_user.username, current_user.user_id);
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
                            printf("Location update from Driver %d: (%d, %d)\n", current_user.user_id, x, y);
                        }
                    }
                    break;
                case MSG_RIDE_ACCEPT:
                    if (current_user.role == ROLE_DRIVER) { 
                        process_driver_response(current_user.user_id, 1);
                        printf("Driver %d accepted the ride.\n", current_user.user_id);
                    }
                    break;
                case MSG_RIDE_REJECT:
                    if (current_user.role == ROLE_DRIVER) { 
                        process_driver_response(current_user.user_id, 2);
                        printf("Driver %d rejected the ride.\n", current_user.user_id);
                    }
                    break;
                case MSG_RIDE_REQ:
                    if (current_user.role == ROLE_RIDER) {
                        printf("Ride request received from Rider %d.\n", current_user.user_id);
                        handle_ride_request(client_sock, &packet, &current_user);
                    }
                    break;
                case MSG_ADMIN_ACTION:
                    if (current_user.role == ROLE_ADMIN) {
                        printf("Admin action received from %s.\n", current_user.username);
                        handle_admin_action(client_sock, &packet);
                    }
                    break;
                case MSG_TRIP_HISTORY_REQ:
                    handle_trip_history_req(client_sock, &packet, &current_user);
                    break;
                case MSG_REVENUE_REPORT_REQ:
                    if (current_user.role == ROLE_ADMIN) {
                        handle_revenue_req(client_sock, &packet);
                    }
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

int main() {
    signal(SIGINT, cleanup_and_exit); 
    setup_ipc();
    
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) { perror("Socket failed"); return 1; }
    
    int opt = 1; 
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr; 
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; 
    addr.sin_addr.s_addr = INADDR_ANY; 
    addr.sin_port = htons(PORT);
    
    if (bind(server_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Bind failed"); return 1;
    }
    if (listen(server_sock, MAX_PENDING) < 0) {
        perror("Listen failed"); return 1;
    }
    
    printf("Server listening on port %d...\n", PORT);
    while (1) {
        int* csock = malloc(sizeof(int));
        if (!csock) continue;
        *csock = accept(server_sock, NULL, NULL);
        if (*csock < 0) { free(csock); continue; }
        
        pthread_t tid; 
        if (pthread_create(&tid, NULL, handle_client, csock) != 0) {
            free(csock);
        } else {
            pthread_detach(tid);
        }
    }
    return 0;
}
