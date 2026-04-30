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

// Global variables for the background listener thread
int global_sock;
int is_online = 0;
int current_x = 0;
int current_y = 0;

/* 
   This thread runs in the background. It's needed because the driver might 
   be sitting at the menu when the server suddenly sends a "Ride Offer".
   It handles catching those incoming push messages.
*/
void* server_listener_thread(void* arg) {
    (void)arg;
    MessagePacket res;
    while(1) {
        int bytes = recv(global_sock, &res, sizeof(res), 0);
        if (bytes <= 0) {
            printf("\n[Disconnected from server]\n");
            exit(0);
        }
        // If the server sends a ride offer, I print it to the screen
        if (res.type == MSG_RIDE_OFFER) {
            int sx, sy, dx, dy;
            sscanf(res.payload, "%d %d %d %d", &sx, &sy, &dx, &dy);
            printf("\n!!! New Ride Request !!!\n");
            printf(" From: (%d, %d)  To: (%d, %d)\n", sx, sy, dx, dy);
            printf(" Go to menu and choose '5' to Accept or '6' to Reject.\n");
            printf("Choice: ");
            fflush(stdout);
        }
    }
    return NULL;
}

/* 
   Main driver program. It handles the UI, login, and sending status updates
   to the server like my current location or if I are online/offline.
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

    printf("--- Welcome to the Ride-Sharing System (Driver) ---\n");
    char username[32], password[64];
    printf("Username: ");
    scanf("%31s", username);
    printf("Password: ");
    scanf("%63s", password);

    // Send auth request to the server
    MessagePacket packet;
    packet.type = MSG_AUTH_REQ;
    sprintf(packet.payload, "%s %s", username, password);
    send(sock, &packet, sizeof(packet), 0);

    MessagePacket res;
    recv(sock, &res, sizeof(res), 0);

    if (res.type == MSG_AUTH_RES) {
        int my_id;
        sscanf(res.payload, "%d", &my_id);
        printf("Logged in as Driver ID: %d\n", my_id);

        global_sock = sock;
        pthread_t listener_thread;
        
        // Start the thread that listens for "Push" notifications from the server
        pthread_create(&listener_thread, NULL, server_listener_thread, NULL);
        pthread_detach(listener_thread);

        int choice;
        while (1) {
            printf("\nDriver Menu:\n");
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
                    printf("Status: ONLINE (Available for rides)\n");
                    break;
                case 2:
                    is_online = 0;
                    packet.type = MSG_LOC_UPDATE;
                    sprintf(packet.payload, "%d %d %d", STATUS_OFFLINE, current_x, current_y);
                    send(sock, &packet, sizeof(packet), 0);
                    printf("Status: OFFLINE\n");
                    break;
                case 3:
                    printf("Enter new X and Y: ");
                    scanf("%d %d", &current_x, &current_y);
                    packet.type = MSG_LOC_UPDATE;
                    sprintf(packet.payload, "%d %d %d", is_online ? STATUS_AVAILABLE : STATUS_OFFLINE, current_x, current_y);
                    send(sock, &packet, sizeof(packet), 0);
                    printf("New Location: (%d, %d)\n", current_x, current_y);
                    break;
                case 4: {
                    /* 
                       Read the common trip history file and calculate how much money 
                       this specific driver has earned.
                    */
                    FILE *fp = fopen("data/trip_history.txt", "r");
                    if (!fp) {
                        printf("No trip history found.\n");
                        break;
                    }
                    int fd = fileno(fp);
                    struct flock lock;
                    memset(&lock, 0, sizeof(lock));
                    lock.l_type = F_RDLCK;
                    fcntl(fd, F_SETLKW, &lock);
                    
                    printf("\n--- My Earnings ---\n");
                    char line[256];
                    int total = 0;
                    while (fgets(line, sizeof(line), fp)) {
                        int r_id, d_id, sx, sy, ex, ey, fare;
                        if (sscanf(line, "TRIP %d %d %d %d %d %d %d", &r_id, &d_id, &sx, &sy, &ex, &ey, &fare) == 7) {
                            if (d_id == my_id) {
                                printf(" -> Trip From (%d,%d) | Earned: $%d\n", sx, sy, fare);
                                total += fare;
                            }
                        }
                    }
                    printf(" TOTAL EARNINGS: $%d\n", total);
                    lock.l_type = F_UNLCK;
                    fcntl(fd, F_SETLK, &lock);
                    fclose(fp);
                    break;
                }
                case 5:
                    packet.type = MSG_RIDE_ACCEPT;
                    strcpy(packet.payload, "Accepted");
                    send(sock, &packet, sizeof(packet), 0);
                    printf("You accepted the ride!\n");
                    break;
                case 6:
                    packet.type = MSG_RIDE_REJECT;
                    strcpy(packet.payload, "Rejected");
                    send(sock, &packet, sizeof(packet), 0);
                    printf("You rejected the ride.\n");
                    break;
                default:
                    printf("Invalid option.\n");
            }
        }
    } else {
        printf("Auth failed: %s\n", res.payload);
    }

    close(sock);
    return 0;
}
