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
            printf("2. Manage Users (Stub)\n");
            printf("3. Logout & Exit\n");
            printf("Choice: ");
            if (scanf("%d", &choice) != 1) break;

            if (choice == 3) {
                packet.type = MSG_DISCONNECT;
                strcpy(packet.payload, "Bye");
                send(sock, &packet, sizeof(packet), 0);
                break;
            }

            switch (choice) {
                case 1: {
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
                    printf("\n[User Management] Ban a User\n");
                    char target_user[32];
                    printf("Enter username to lock out: ");
                    scanf("%31s", target_user);
                    
                    // Simple file rewrite to lock out a user (removes them from users.dat)
                    FILE *fp = fopen("data/users.dat", "r");
                    FILE *ftemp = fopen("data/users_temp.dat", "w");
                    if (fp == NULL || ftemp == NULL) {
                        printf("Error accessing user database.\n");
                        if(fp) fclose(fp);
                        if(ftemp) fclose(ftemp);
                        break;
                    }
                    
                    char line[128];
                    int found = 0;
                    while (fgets(line, sizeof(line), fp)) {
                        char u_name[32];
                        sscanf(line, "%*d %31s", u_name);
                        if (strcmp(u_name, target_user) == 0) {
                            found = 1;
                            // Optionally, we could write them back with a locked out role, but omitting them effectively bans them.
                            continue; 
                        }
                        fputs(line, ftemp);
                    }
                    fclose(fp);
                    fclose(ftemp);
                    
                    if (found) {
                        remove("data/users.dat");
                        rename("data/users_temp.dat", "data/users.dat");
                        printf("Success: User '%s' has been locked out of the system.\n", target_user);
                    } else {
                        remove("data/users_temp.dat");
                        printf("Error: User '%s' not found in database.\n", target_user);
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
