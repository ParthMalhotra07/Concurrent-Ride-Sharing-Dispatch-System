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

/* 
   This is the client program for Riders. It connects to the server, 
   authenticates, and then allows the user to request rides or view history.
*/
int main() {
    int sock;
    struct sockaddr_in server_addr;

    // Standard socket creation and connection to localhost port 8080
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
        perror("Connection Failed! Is the server running?");
        return -1;
    }

    printf("--- Welcome to the Ride-Sharing System (Rider) ---\n");
    char username[32], password[64];
    printf("Username: ");
    scanf("%31s", username);
    printf("Password: ");
    scanf("%63s", password);

    // Prepare and send the login request packet
    MessagePacket packet;
    packet.type = MSG_AUTH_REQ;
    sprintf(packet.payload, "%s %s", username, password);
    send(sock, &packet, sizeof(packet), 0);

    // Wait for the server to tell us if we are logged in or not
    MessagePacket res;
    recv(sock, &res, sizeof(res), 0);

    if (res.type == MSG_AUTH_RES) {
        int my_id;
        sscanf(res.payload, "%d", &my_id);
        printf("Successfully logged in! Your ID is %d\n", my_id);

        int choice;
        while (1) {
            printf("\nMain Menu:\n");
            printf("1. Book a Ride\n");
            printf("2. View My History\n");
            printf("3. Logout\n");
            printf("Enter choice: ");
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
                    printf("Enter Your Current X and Y: ");
                    scanf("%d %d", &sx, &sy);
                    printf("Enter Your Destination X and Y: ");
                    scanf("%d %d", &dx, &dy);

                    packet.type = MSG_RIDE_REQ;
                    sprintf(packet.payload, "%d %d %d %d", sx, sy, dx, dy);
                    printf("Looking for a driver nearby...\n");
                    send(sock, &packet, sizeof(packet), 0);
                    
                    // The server will block here until a driver accepts or we time out
                    recv(sock, &res, sizeof(res), 0);
                    if (res.type == MSG_RIDE_MATCHED) {
                        printf("Matched with Driver ID %s! They are coming to pick you up.\n", res.payload);
                    } else {
                        printf("Server says: %s\n", res.payload);
                    }
                    break;
                }
                case 2: {
                    /* 
                       We read the trip_history file directly to show the user their rides.
                       We use a read-lock (F_RDLCK) so we don't read half-written data.
                    */
                    FILE *fp = fopen("data/trip_history.txt", "r");
                    if (!fp) {
                        printf("No trips recorded yet.\n");
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
                    
                    printf("\n--- Your Rides ---\n");
                    char line[256];
                    int count = 0;
                    while (fgets(line, sizeof(line), fp)) {
                        int r_id, d_id, sx, sy, ex, ey, fare;
                        if (sscanf(line, "TRIP %d %d %d %d %d %d %d", &r_id, &d_id, &sx, &sy, &ex, &ey, &fare) == 7) {
                            if (r_id == my_id) {
                                printf(" -> Trip to (%d,%d) | Fare: $%d\n", ex, ey, fare);
                                count++;
                            }
                        }
                    }
                    if (count == 0) printf("    No trips found in history.\n");
                    
                    lock.l_type = F_UNLCK;
                    fcntl(fd, F_SETLK, &lock);
                    fclose(fp);
                    break;
                }
                default:
                    printf("Invalid input.\n");
            }
        }
    } else {
        printf("Login failed: %s\n", res.payload);
    }

    close(sock);
    return 0;
}
