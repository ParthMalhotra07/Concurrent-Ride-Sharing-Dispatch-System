#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../common/structs.h"

#define SERVER_IP "127.0.0.1"
#define PORT 8080

// Global variables for the Listener Thread
int global_sock;
int is_online = 0;
int current_x = 0;
int current_y = 0;

void* server_listener_thread(void* arg) {
    (void)arg;
    MessagePacket res;
    while(1) {
        int bytes = recv(global_sock, &res, sizeof(res), 0);
        if (bytes <= 0) {
            printf("\n[Disconnected from server]\n");
            exit(0);
        }
        if (res.type == MSG_RIDE_OFFER) {
            int sx, sy, dx, dy;
            sscanf(res.payload, "%d %d %d %d", &sx, &sy, &dx, &dy);
            printf("\n==========================================\n");
            printf("  🚨 RIDE OFFER RECEIVED! 🚨\n");
            printf("  Pickup: (%d, %d)  -->  Dropoff: (%d, %d)\n", sx, sy, dx, dy);
            printf("  (Type '5' to Accept, '6' to Reject in menu)\n");
            printf("==========================================\nChoice: ");
            fflush(stdout);
        }
    }
    return NULL;
}

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
        
        global_sock = sock;
        pthread_t listener_thread;
        
        pthread_create(&listener_thread, NULL, server_listener_thread, NULL);
        pthread_detach(listener_thread);

        while (1) {
            printf("\n--- DRIVER MENU ---\n");
            printf("1. Go Online (Available for rides)\n");
            printf("2. Go Offline\n");
            printf("3. Update My Location (x,y)\n");
            printf("4. Logout & Exit\n");
            printf("5. Accept Pending Ride Offer\n");
            printf("6. Reject Pending Ride Offer\n");
            printf("Choice: ");
            if (scanf("%d", &choice) != 1) break;

            if (choice == 4) {
                is_online = 0;
                packet.type = MSG_DISCONNECT;
                strcpy(packet.payload, "Bye");
                send(sock, &packet, sizeof(packet), 0);
                break;
            }

            switch (choice) {
                case 1:
                    is_online = 1;
                    packet.type = MSG_LOC_UPDATE;
                    sprintf(packet.payload, "%d %d %d", STATUS_AVAILABLE, current_x, current_y);
                    send(sock, &packet, sizeof(packet), 0);
                    printf("Status set to: AVAILABLE.\n");
                    break;
                case 2:
                    is_online = 0;
                    packet.type = MSG_LOC_UPDATE;
                    sprintf(packet.payload, "%d %d %d", STATUS_OFFLINE, current_x, current_y);
                    send(sock, &packet, sizeof(packet), 0);
                    printf("Status set to: OFFLINE.\n");
                    break;
                case 3:
                    printf("Enter new X Y (e.g. 5 10): ");
                    if (scanf("%d %d", &current_x, &current_y) != 2) {
                        printf("Invalid input! Please enter numbers only.\n");
                        int c; while ((c = getchar()) != '\n' && c != EOF); // Clear bad buffer
                        break;
                    }
                    packet.type = MSG_LOC_UPDATE;
                    sprintf(packet.payload, "%d %d %d", is_online ? STATUS_AVAILABLE : STATUS_OFFLINE, current_x, current_y);
                    send(sock, &packet, sizeof(packet), 0);
                    printf("Location updated to (%d, %d)\n", current_x, current_y);
                    break;
                case 5:
                    packet.type = MSG_RIDE_ACCEPT;
                    strcpy(packet.payload, "Accepted");
                    send(sock, &packet, sizeof(packet), 0);
                    printf("Ride Offer ACCEPTED!\n");
                    break;
                case 6:
                    packet.type = MSG_RIDE_REJECT;
                    strcpy(packet.payload, "Rejected");
                    send(sock, &packet, sizeof(packet), 0);
                    printf("Ride Offer REJECTED!\n");
                    break;
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
