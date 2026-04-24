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
        perror("Invalid address/ Address not supported");
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection Failed");
        return -1;
    }

    printf("=== ADMIN CLIENT ===\n");
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
        printf("Successfully authenticated as Administrator!\n");
        
        int choice;
        while (1) {
            printf("\n--- ADMIN DASHBOARD ---\n");
            printf("1. System Status Summary\n");
            printf("2. View Full Trip Ledger\n");
            printf("3. Financial Summary\n");
            printf("4. Manage User Access (Lock/Unlock)\n");
            printf("5. Logout & Exit\n");
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
                    // We access the global driver grid directly through Shared Memory.
                    // This is faster than network sockets because it avoids kernel overhead.
                    printf("\n[System View] Fetching Live Grid from Shared Memory...\n");
                    int shm_fd = shm_open(SHM_NAME, O_RDONLY, 0666);
                    if (shm_fd == -1) {
                        perror("Could not access Shared Memory (Is server running?)");
                        break;
                    }
                    SystemGridMap* grid_shm = mmap(NULL, SHM_SIZE, PROT_READ, MAP_SHARED, shm_fd, 0);
                    if (grid_shm != MAP_FAILED) {
                        printf("\n--- CURRENT DRIVER GRID (LIVE) ---\n");
                        int count = 0;
                        for (int i = 0; i < MAX_DRIVERS; i++) {
                            if (grid_shm->grid[i].driver_id != 0) {
                                printf(" -> Driver ID %d | Status: %d | Location: (%d, %d)\n", 
                                        grid_shm->grid[i].driver_id, 
                                        grid_shm->grid[i].status, 
                                        grid_shm->grid[i].current_loc.x, 
                                        grid_shm->grid[i].current_loc.y);
                                count++;
                            }
                        }
                        if (count == 0) printf("    No drivers currently online.\n");
                        printf("----------------------------------\n");
                        munmap(grid_shm, SHM_SIZE);
                    }
                    close(shm_fd);
                    break;
                }
                case 2: {
                    FILE *fp = fopen("data/ledger.txt", "r");
                    if (!fp) {
                        printf("Ledger file not found.\n");
                        break;
                    }
                    int fd = fileno(fp);
                    struct flock lock;
                    memset(&lock, 0, sizeof(lock));
                    lock.l_type = F_RDLCK;
                    lock.l_whence = SEEK_SET;
                    lock.l_start = 0;
                    lock.l_len = 0;
                    if (fcntl(fd, F_SETLKW, &lock) == -1) {
                        perror("Failed to lock ledger");
                        fclose(fp);
                        break;
                    }
                    printf("\n--- COMPLETE SYSTEM LEDGER ---\n");
                    char line[256];
                    while (fgets(line, sizeof(line), fp)) {
                        printf(" %s", line);
                    }
                    lock.l_type = F_UNLCK;
                    fcntl(fd, F_SETLK, &lock);
                    fclose(fp);
                    break;
                }
                case 3: {
                    FILE *fp = fopen("data/ledger.txt", "r");
                    if (!fp) {
                        printf("No financial data available.\n");
                        break;
                    }
                    int fd = fileno(fp);
                    struct flock lock;
                    memset(&lock, 0, sizeof(lock));
                    lock.l_type = F_RDLCK;
                    lock.l_whence = SEEK_SET;
                    lock.l_start = 0;
                    lock.l_len = 0;
                    fcntl(fd, F_SETLKW, &lock);

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
                    printf("\n--- SYSTEM FINANCIAL OVERVIEW ---\n");
                    printf(" TOTAL TRIPS COMPLETED : %d\n", total_trips);
                    printf(" TOTAL SYSTEM REVENUE  : $%ld\n", total_revenue);
                    if (total_trips > 0)
                        printf(" AVERAGE FARE PER TRIP : $%.2f\n", (double)total_revenue / total_trips);
                    printf("----------------------------------\n");

                    lock.l_type = F_UNLCK;
                    fcntl(fd, F_SETLK, &lock);
                    fclose(fp);
                    break;
                }
                case 4: {
                    // The Admin can modify user permissions live. This demonstrates 
                    // how the system handles role-based access control.
                    printf("\n--- MANAGE USER ACCESS ---\n");
                    char target_user[32];
                    int new_status;
                    printf("Enter username to modify: ");
                    scanf("%31s", target_user);
                    printf("Set Status (0 = ACTIVE, 1 = LOCKED/BANNED): ");
                    if (scanf("%d", &new_status) != 1) break;
                    
                    FILE *fp = fopen("data/users.dat", "r");
                    FILE *ftemp = fopen("data/users_temp.dat", "w");
                    if (!fp || !ftemp) {
                        printf("Error accessing user database.\n");
                        if (fp) fclose(fp); 
                        if (ftemp) fclose(ftemp);
                        break;
                    }
                    
                    char line[256];
                    int found = 0;
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
                    fclose(fp);
                    fclose(ftemp);
                    
                    if (found) {
                        remove("data/users.dat");
                        rename("data/users_temp.dat", "data/users.dat");
                        printf("Success: User '%s' status updated to %s.\n", target_user, new_status ? "LOCKED" : "ACTIVE");
                    } else {
                        remove("data/users_temp.dat");
                        printf("Error: User '%s' not found.\n", target_user);
                    }
                    break;
                }
                default:
                    printf("Invalid choice.\n");
            }
        }
    } else {
        printf("Authentication failed: %s\n", res.payload);
    }

    close(sock);
    return 0;
}
