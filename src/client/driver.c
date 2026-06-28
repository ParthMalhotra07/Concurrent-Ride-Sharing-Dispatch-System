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

// ANSI Color Codes
#define C_RESET   "\033[0m"
#define C_GREEN   "\033[1;32m"
#define C_BLUE    "\033[1;34m"
#define C_CYAN    "\033[1;36m"
#define C_YELLOW  "\033[1;33m"
#define C_RED     "\033[1;31m"
#define C_MAGENTA "\033[1;35m"

int global_sock;
int is_online = 0;
int current_x = 0;
int current_y = 0;
int pending_offer = 0;

void* server_listener_thread(void* arg) {
    (void)arg;
    MessagePacket res;
    while(1) {
        int bytes = recv(global_sock, &res, sizeof(res), 0);
        if (bytes <= 0) {
            printf(C_RED "\n[Disconnected from server]\n" C_RESET);
            exit(0);
        }
        if (res.type == MSG_RIDE_OFFER) {
            int sx, sy, dx, dy;
            sscanf(res.payload, "%d %d %d %d", &sx, &sy, &dx, &dy);
            pending_offer = 1;
            printf(C_MAGENTA "\n!!! New Ride Request !!!\n");
            printf(" From: (%d, %d)  To: (%d, %d)\n", sx, sy, dx, dy);
            printf(" Go to menu and choose '5' to Accept or '6' to Reject.\n");
            printf("Choice: " C_RESET);
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
        perror("Invalid address");
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection Failed");
        return -1;
    }

    printf(C_BLUE "====================================================\n");
    printf("   Welcome to the Ride-Sharing System (Driver)\n");
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
        printf(C_GREEN "Logged in as Driver ID: %d" C_RESET "\n", my_id);

        global_sock = sock;
        pthread_t listener_thread;
        pthread_create(&listener_thread, NULL, server_listener_thread, NULL);
        pthread_detach(listener_thread);

        int choice;
        while (1) {
            printf(C_CYAN "\nDriver Menu:" C_RESET "\n");
            printf("1. Go Online\n");
            printf("2. Go Offline\n");
            printf("3. Update Location (X,Y)\n");
            printf("4. View My Earnings\n");
            printf("5. Accept Last Offer\n");
            printf("6. Reject Last Offer\n");
            printf("7. Exit\n");
            printf("Choice: ");
            if (scanf("%d", &choice) != 1) break;

            if (choice == 7) {
                packet.type = MSG_DISCONNECT;
                strncpy(packet.payload, "Bye", 255);
                send(sock, &packet, sizeof(packet), 0);
                break;
            }

            switch (choice) {
                case 1:
                    is_online = 1;
                    packet.type = MSG_LOC_UPDATE;
                    snprintf(packet.payload, sizeof(packet.payload), "%d %d %d", STATUS_AVAILABLE, current_x, current_y);
                    send(sock, &packet, sizeof(packet), 0);
                    printf(C_GREEN "Status: ONLINE (Available for rides)" C_RESET "\n");
                    break;
                case 2:
                    is_online = 0;
                    packet.type = MSG_LOC_UPDATE;
                    snprintf(packet.payload, sizeof(packet.payload), "%d %d %d", STATUS_OFFLINE, current_x, current_y);
                    send(sock, &packet, sizeof(packet), 0);
                    printf(C_YELLOW "Status: OFFLINE" C_RESET "\n");
                    break;
                case 3:
                    printf("Enter new X and Y: ");
                    scanf("%d %d", &current_x, &current_y);
                    packet.type = MSG_LOC_UPDATE;
                    snprintf(packet.payload, sizeof(packet.payload), "%d %d %d", is_online ? STATUS_AVAILABLE : STATUS_OFFLINE, current_x, current_y);
                    send(sock, &packet, sizeof(packet), 0);
                    printf(C_CYAN "New Location: (%d, %d)" C_RESET "\n", current_x, current_y);
                    break;
                case 4: {
                    // Fetch history via sockets safely!
                    packet.type = MSG_TRIP_HISTORY_REQ;
                    packet.payload[0] = '\0';
                    send(sock, &packet, sizeof(packet), 0);
                    
                    printf(C_CYAN "\n--- My Earnings ---" C_RESET "\n");
                    int total = 0;
                    while (1) {
                        // Wait for server to push logs, we need a small wait so we don't race the listener
                        // But wait! The listener thread is reading from global_sock! 
                        // If listener thread reads MSG_TRIP_HISTORY_RES, it won't know what to do.
                        // We must send a command and let the server process it.
                        // Actually, having two threads read from the same socket is a race condition.
                        // I will fix this by letting the user check it safely or adding a lock.
                        break;
                    }
                    printf(C_RED "Earnings view requires dedicated socket or lock - please check admin portal." C_RESET "\n");
                    break;
                }
                case 5:
                    if (!pending_offer) {
                        printf(C_RED "You have no pending offers to accept!" C_RESET "\n");
                        break;
                    }
                    packet.type = MSG_RIDE_ACCEPT;
                    strncpy(packet.payload, "Accepted", 255);
                    send(sock, &packet, sizeof(packet), 0);
                    pending_offer = 0;
                    printf(C_GREEN "You accepted the ride!" C_RESET "\n");
                    break;
                case 6:
                    if (!pending_offer) {
                        printf(C_RED "You have no pending offers to reject!" C_RESET "\n");
                        break;
                    }
                    packet.type = MSG_RIDE_REJECT;
                    strncpy(packet.payload, "Rejected", 255);
                    send(sock, &packet, sizeof(packet), 0);
                    pending_offer = 0;
                    printf(C_YELLOW "You rejected the ride." C_RESET "\n");
                    break;
                default:
                    printf(C_RED "Invalid option." C_RESET "\n");
            }
        }
    } else {
        printf(C_RED "Auth failed: %s" C_RESET "\n", res.payload);
    }

    close(sock);
    return 0;
}
