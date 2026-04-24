#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
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

    printf("=== RIDER CLIENT ===\n");
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
        int my_id;
        sscanf(res.payload, "%d", &my_id);
        printf("Successfully authenticated as Rider (ID: %d)!\n", my_id);

        int choice;
        while (1) {
            printf("\n--- RIDER MENU ---\n");
            printf("1. Request a Ride\n");
            printf("2. View My Trip History\n");
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
                    int sx, sy, dx, dy;
                    printf("Enter Your Start Location (X Y) and Drop-off Location (X Y) [e.g. 10 20 40 50]: ");
                    if (scanf("%d %d %d %d", &sx, &sy, &dx, &dy) != 4) {
                        printf("Invalid input! Please enter four numbers.\n");
                        int c; while ((c = getchar()) != '\n' && c != EOF); // Clear bad buffer
                        break;
                    }
                    packet.type = MSG_RIDE_REQ;
                    sprintf(packet.payload, "%d %d %d %d", sx, sy, dx, dy);
                    printf("Searching for drivers to take you to (%d, %d)...\n", dx, dy);
                    send(sock, &packet, sizeof(packet), 0);
                    
                    // We wait here for the server to find a driver and send back a match.
                    recv(sock, &res, sizeof(res), 0);
                    if (res.type == MSG_RIDE_MATCHED) {
                        printf("\n[MATCH FOUND!] Driver ID %s is on the way!\n", res.payload);
                    } else {
                        printf("\n[ERROR] %s\n", res.payload);
                    }
                    break;
                }
                case 2: {
                    // This function opens the shared ledger file to show the rider 
                    // a list of all their successfully completed trips.
                    FILE *fp = fopen("data/ledger.txt", "r");
                    if (!fp) {
                        printf("No trip history available yet.\n");
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
                    printf("\n--- MY TRIP HISTORY ---\n");
                    char line[256];
                    int count = 0;
                    while (fgets(line, sizeof(line), fp)) {
                        int r_id, d_id, sx, sy, ex, ey, fare;
                        if (sscanf(line, "TRIP %d %d %d %d %d %d %d", &r_id, &d_id, &sx, &sy, &ex, &ey, &fare) == 7) {
                            if (r_id == my_id) {
                                printf(" -> Ride to (%d,%d) | Driver ID: %d | Fare: $%d\n", ex, ey, d_id, fare);
                                count++;
                            }
                        }
                    }
                    if (count == 0) printf("    No trips found.\n");
                    lock.l_type = F_UNLCK;
                    fcntl(fd, F_SETLK, &lock);
                    fclose(fp);
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
