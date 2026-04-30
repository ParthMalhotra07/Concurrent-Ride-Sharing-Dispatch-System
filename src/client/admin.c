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

/* 
   This is the admin program. It has special powers like viewing the 
   global driver grid through shared memory and locking/unlocking user accounts.
*/
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

    printf("--- Administrative Dashboard ---\n");
    char username[32], password[64];
    printf("Username: ");
    scanf("%31s", username);
    printf("Password: ");
    scanf("%63s", password);

    MessagePacket packet;
    packet.type = MSG_AUTH_REQ;
    sprintf(packet.payload, "%s %s", username, password);
    send(sock, &packet, sizeof(packet), 0);

    MessagePacket res;
    recv(sock, &res, sizeof(res), 0);

    if (res.type == MSG_AUTH_RES) {
        printf("Access Granted: Admin logged in.\n");
        
        int choice;
        while (1) {
            printf("\nAdmin Menu:\n");
            printf("1. View Live Driver Grid\n");
            printf("2. View All Trip Records\n");
            printf("3. Calculate Total Earnings\n");
            printf("4. Lock/Unlock a User Account\n");
            printf("5. Exit\n");
            printf("Choice: ");
            if (scanf("%d", &choice) != 1) break;

            if (choice == 5) {
                packet.type = MSG_DISCONNECT;
                strcpy(packet.payload, "Bye");
                send(sock, &packet, sizeof(packet), 0);
                break;
            }

            switch (choice) {
                case 1: {
                    /* 
                       Admins can "spy" on the shared memory grid to see every 
                       driver's status and location in real-time.
                    */
                    int shm_fd = shm_open(SHM_NAME, O_RDONLY, 0666);
                    if (shm_fd == -1) {
                        printf("Error: Could not open shared memory.\n");
                        break;
                    }
                    SystemGridMap* grid_shm = mmap(NULL, SHM_SIZE, PROT_READ, MAP_SHARED, shm_fd, 0);
                    if (grid_shm != MAP_FAILED) {
                        printf("\n--- Global Driver Grid ---\n");
                        int count = 0;
                        for (int i = 0; i < MAX_DRIVERS; i++) {
                            if (grid_shm->grid[i].driver_id != 0) {
                                printf(" ID %d | Status: %d | At: (%d, %d)\n", 
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
                    // Show every single line in the trip history text file
                    FILE *fp = fopen("data/trip_history.txt", "r");
                    if (!fp) {
                        printf("History file is missing.\n");
                        break;
                    }
                    printf("\n--- Full System History ---\n");
                    char line[256];
                    while (fgets(line, sizeof(line), fp)) {
                        printf(" %s", line);
                    }
                    fclose(fp);
                    break;
                }
                case 3: {
                    // Summarize all earnings from all drivers in the system
                    FILE *fp = fopen("data/trip_history.txt", "r");
                    if (!fp) {
                        printf("No financial data found.\n");
                        break;
                    }
                    long total_revenue = 0;
                    int total_trips = 0;
                    char line[256];
                    while (fgets(line, sizeof(line), fp)) {
                        int r_id, d_id, sx, sy, ex, ey, fare;
                        if (sscanf(line, "TRIP %d %d %d %d %d %d %d", &r_id, &d_id, &sx, &sy, &ex, &ey, &fare) == 7) {
                            total_revenue += fare;
                            total_trips++;
                        }
                    }
                    printf("\n--- Finance Report ---\n");
                    printf(" Total Trips: %d\n", total_trips);
                    printf(" Total Revenue: $%ld\n", total_revenue);
                    fclose(fp);
                    break;
                }
                case 4: {
                    /* 
                       Tell the server to modify the users.dat file to ban 
                       or unban a specific username.
                    */
                    char target_user[32];
                    int new_status;
                    printf("Target Username: ");
                    scanf("%31s", target_user);
                    printf("Set Status (0=Active, 1=Banned): ");
                    scanf("%d", &new_status);
                    
                    packet.type = MSG_ADMIN_ACTION;
                    sprintf(packet.payload, "%s %d", target_user, new_status);
                    send(sock, &packet, sizeof(packet), 0);
                    
                    MessagePacket action_res;
                    recv(sock, &action_res, sizeof(action_res), 0);
                    printf("Server result: %s\n", action_res.payload);
                    break;
                }
                default:
                    printf("Invalid option.\n");
            }
        }
    } else {
        printf("Access Denied: %s\n", res.payload);
    }

    close(sock);
    return 0;
}
