#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <fcntl.h>
#include "../common/structs.h"

#define SERVER_IP "127.0.0.1"
#define PORT 8080

// ANSI Color Codes
#define C_RESET   "\033[0m"
#define C_GREEN   "\033[1;32m"
#define C_BLUE    "\033[1;34m"
#define C_CYAN    "\033[1;36m"
#define C_YELLOW  "\033[1;33m"
#define C_RED     "\033[1;31m"

int main() {
    int sock;
    struct sockaddr_in server_addr;

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Socket creation failed");
        return -1;
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    
    if (inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr) <= 0) {
        perror("Invalid address");
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection Failed");
        return -1;
    }

    printf(C_RED "====================================================\n");
    printf("   Administrative Dashboard (Root Access)\n");
    printf("====================================================" C_RESET "\n");
    char username[32], password[64];
    printf("Username: ");
    scanf("%31s", username);
    printf("Password: ");
    scanf("%63s", password);

    MessagePacket packet;
    packet.type = MSG_AUTH_REQ;
    snprintf(packet.payload, sizeof(packet.payload), "%s %s", username, password);
    send(sock, &packet, sizeof(packet), 0);

    MessagePacket res;
    recv(sock, &res, sizeof(res), 0);

    if (res.type == MSG_AUTH_RES) {
        printf(C_GREEN "Access Granted: Admin logged in." C_RESET "\n");
        
        int choice;
        while (1) {
            printf(C_CYAN "\nAdmin Menu:" C_RESET "\n");
            printf("1. View Live Driver Grid (IPC)\n");
            printf("2. View All Trip Records (Socket Stream)\n");
            printf("3. Calculate Total Earnings (Socket Request)\n");
            printf("4. Lock/Unlock a User Account\n");
            printf("5. Exit\n");
            printf("Choice: ");
            if (scanf("%d", &choice) != 1) break;

            if (choice == 5) {
                packet.type = MSG_DISCONNECT;
                strncpy(packet.payload, "Bye", 255);
                send(sock, &packet, sizeof(packet), 0);
                break;
            }

            switch (choice) {
                case 1: {
                    int shm_fd = shm_open(SHM_NAME, O_RDONLY, 0666);
                    if (shm_fd == -1) {
                        printf(C_RED "Error: Could not open shared memory." C_RESET "\n");
                        break;
                    }
                    SystemGridMap* grid_shm = mmap(NULL, SHM_SIZE, PROT_READ, MAP_SHARED, shm_fd, 0);
                    if (grid_shm != MAP_FAILED) {
                        printf(C_CYAN "\n--- Global Driver Grid ---" C_RESET "\n");
                        int count = 0;
                        for (int i = 0; i < MAX_DRIVERS; i++) {
                            if (grid_shm->grid[i].driver_id != 0) {
                                printf(" ID " C_YELLOW "%d" C_RESET " | Status: %d | At: (%d, %d)\n", 
                                        grid_shm->grid[i].driver_id, 
                                        grid_shm->grid[i].status, 
                                        grid_shm->grid[i].current_loc.x, 
                                        grid_shm->grid[i].current_loc.y);
                                count++;
                            }
                        }
                        if (count == 0) printf("    Grid is currently empty.\n");
                        munmap(grid_shm, SHM_SIZE);
                    }
                    close(shm_fd);
                    break;
                }
                case 2: {
                    packet.type = MSG_TRIP_HISTORY_REQ;
                    packet.payload[0] = '\0';
                    send(sock, &packet, sizeof(packet), 0);
                    
                    printf(C_CYAN "\n--- Full System History ---" C_RESET "\n");
                    int count = 0;
                    while (1) {
                        if (recv(sock, &res, sizeof(res), 0) <= 0) break;
                        if (res.type == MSG_TRIP_HISTORY_END) break;
                        
                        if (res.type == MSG_TRIP_HISTORY_RES) {
                            int r_id, d_id, sx, sy, ex, ey, fare;
                            if (sscanf(res.payload, "%d %d %d %d %d %d %d", &r_id, &d_id, &sx, &sy, &ex, &ey, &fare) == 7) {
                                printf(" Rider: %d | Driver: %d | From (%d,%d) to (%d,%d) | Fare: " C_GREEN "$%d" C_RESET "\n", 
                                        r_id, d_id, sx, sy, ex, ey, fare);
                                count++;
                            }
                        }
                    }
                    if (count == 0) printf("    No records found in database.\n");
                    break;
                }
                case 3: {
                    packet.type = MSG_REVENUE_REPORT_REQ;
                    packet.payload[0] = '\0';
                    send(sock, &packet, sizeof(packet), 0);
                    
                    recv(sock, &res, sizeof(res), 0);
                    if (res.type == MSG_REVENUE_REPORT_RES) {
                        int trips;
                        long rev;
                        sscanf(res.payload, "%d %ld", &trips, &rev);
                        printf(C_CYAN "\n--- Finance Report ---" C_RESET "\n");
                        printf(" Total System Trips: %d\n", trips);
                        printf(" Total System Revenue: " C_GREEN "$%ld" C_RESET "\n", rev);
                    } else {
                        printf(C_RED "Failed to retrieve report." C_RESET "\n");
                    }
                    break;
                }
                case 4: {
                    char target_user[32];
                    int new_status;
                    printf("Target Username: ");
                    scanf("%31s", target_user);
                    printf("Set Status (0=Active, 1=Banned): ");
                    scanf("%d", &new_status);
                    
                    packet.type = MSG_ADMIN_ACTION;
                    snprintf(packet.payload, sizeof(packet.payload), "%s %d", target_user, new_status);
                    send(sock, &packet, sizeof(packet), 0);
                    
                    MessagePacket action_res;
                    recv(sock, &action_res, sizeof(action_res), 0);
                    printf(C_YELLOW "Server result: %s" C_RESET "\n", action_res.payload);
                    break;
                }
                default:
                    printf(C_RED "Invalid option." C_RESET "\n");
            }
        }
    } else {
        printf(C_RED "Access Denied: %s" C_RESET "\n", res.payload);
    }

    close(sock);
    return 0;
}
