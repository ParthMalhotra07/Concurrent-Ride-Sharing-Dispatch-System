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
            printf("2. Logout & Exit\n");
            printf("Choice: ");
            if (scanf("%d", &choice) != 1) break;

            if (choice == 2) {
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
                    
                    // Wait for server response!
                    recv(sock, &res, sizeof(res), 0);
                    if (res.type == MSG_RIDE_MATCHED) {
                        printf("\n[MATCH FOUND!] Driver ID %s is on the way!\n", res.payload);
                    } else {
                        printf("\n[ERROR] %s\n", res.payload);
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
