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

// ANSI Color Codes for polished UI
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
        perror("Connection Failed! Is the server running?");
        return -1;
    }

    printf(C_BLUE "====================================================\n");
    printf("   Welcome to the Ride-Sharing System (Rider)\n");
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
        int my_id;
        sscanf(res.payload, "%d", &my_id);
        printf(C_GREEN "Successfully logged in! Your ID is %d" C_RESET "\n", my_id);

        int choice;
        while (1) {
            printf(C_CYAN "\nMain Menu:" C_RESET "\n");
            printf("1. Book a Ride\n");
            printf("2. View My History\n");
            printf("3. Logout\n");
            printf("Enter choice: ");
            if (scanf("%d", &choice) != 1) break;

            if (choice == 3) {
                packet.type = MSG_DISCONNECT;
                strncpy(packet.payload, "Bye", 255);
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
                    snprintf(packet.payload, sizeof(packet.payload), "%d %d %d %d", sx, sy, dx, dy);
                    printf(C_YELLOW "Looking for a driver nearby..." C_RESET "\n");
                    send(sock, &packet, sizeof(packet), 0);
                    
                    recv(sock, &res, sizeof(res), 0);
                    if (res.type == MSG_RIDE_MATCHED) {
                        printf(C_GREEN "Matched with Driver ID %s! They are coming to pick you up." C_RESET "\n", res.payload);
                    } else {
                        printf(C_RED "Server says: %s" C_RESET "\n", res.payload);
                    }
                    break;
                }
                case 2: {
                    packet.type = MSG_TRIP_HISTORY_REQ;
                    packet.payload[0] = '\0';
                    send(sock, &packet, sizeof(packet), 0);
                    
                    printf(C_CYAN "\n--- Your Rides ---" C_RESET "\n");
                    int count = 0;
                    while (1) {
                        if (recv(sock, &res, sizeof(res), 0) <= 0) break;
                        if (res.type == MSG_TRIP_HISTORY_END) break;
                        
                        if (res.type == MSG_TRIP_HISTORY_RES) {
                            int r_id, d_id, sx, sy, ex, ey, fare;
                            if (sscanf(res.payload, "%d %d %d %d %d %d %d", &r_id, &d_id, &sx, &sy, &ex, &ey, &fare) == 7) {
                                printf(" -> Trip from (%d,%d) to (%d,%d) | Driver: %d | Fare: " C_GREEN "$%d" C_RESET "\n", 
                                        sx, sy, ex, ey, d_id, fare);
                                count++;
                            }
                        }
                    }
                    if (count == 0) printf("    No trips found in history.\n");
                    break;
                }
                default:
                    printf(C_RED "Invalid input." C_RESET "\n");
            }
        }
    } else {
        printf(C_RED "Login failed: %s" C_RESET "\n", res.payload);
    }

    close(sock);
    return 0;
}
