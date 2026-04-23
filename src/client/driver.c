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

    printf("=== DRIVER CLIENT ===\n");
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
        printf("Successfully authenticated as Driver (ID: %d)!\n", my_id);

        int choice;
        while (1) {
            printf("\n--- DRIVER MENU ---\n");
            printf("1. Go Online (Available for rides)\n");
            printf("2. Go Offline\n");
            printf("3. Update My Location (x,y)\n");
            printf("4. Logout & Exit\n");
            printf("Choice: ");
            if (scanf("%d", &choice) != 1) break;

            if (choice == 4) {
                packet.type = MSG_DISCONNECT;
                strcpy(packet.payload, "Bye");
                send(sock, &packet, sizeof(packet), 0);
                break;
            }

            int x = 0, y = 0;
            switch (choice) {
                case 1:
                    packet.type = MSG_LOC_UPDATE;
                    sprintf(packet.payload, "%d %d %d", STATUS_AVAILABLE, 0, 0);
                    send(sock, &packet, sizeof(packet), 0);
                    printf("Status set to: AVAILABLE\n");
                    break;
                case 2:
                    packet.type = MSG_LOC_UPDATE;
                    sprintf(packet.payload, "%d %d %d", STATUS_OFFLINE, 0, 0);
                    send(sock, &packet, sizeof(packet), 0);
                    printf("Status set to: OFFLINE\n");
                    break;
                case 3:
                    printf("Enter new X Y (e.g. 5 10): ");
                    if (scanf("%d %d", &x, &y) != 2) {
                        printf("Invalid input! Please enter numbers only.\n");
                        int c; while ((c = getchar()) != '\n' && c != EOF); // Clear bad buffer
                        break;
                    }
                    packet.type = MSG_LOC_UPDATE;
                    sprintf(packet.payload, "%d %d %d", STATUS_AVAILABLE, x, y);
                    send(sock, &packet, sizeof(packet), 0);
                    printf("Location updated to (%d, %d)\n", x, y);
                    break;
                default:
                    printf("Invalid choice.\n");
            }
            
            // Listen for any immediate feedback from server (e.g. You are matched!)
            // For now, we'll keep it simple and just send. 
            // In a real app we'd have a listener thread.
        }
    } else {
        printf("Authentication failed: %s\n", res.payload);
    }

    close(sock);
    return 0;
}
