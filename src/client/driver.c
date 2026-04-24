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

// This thread runs in the background to listen for messages from the server.
// It's mainly here to catch "Ride Offers" that the server pushes to us 
// even while we are sitting in the main menu.
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
            printf("4. View My Earnings\n");
            printf("5. Accept Pending Ride Offer\n");
            printf("6. Reject Pending Ride Offer\n");
            printf("7. Logout & Exit\n");
            printf("Choice: ");
            if (scanf("%d", &choice) != 1) break;

            if (choice == 7) {
                is_online = 0;
                packet.type = MSG_DISCONNECT;
                strcpy(packet.payload, "Bye");
                send(sock, &packet, sizeof(packet), 0);
                break;
            }

            switch (choice) {
                case 1:
                    // When the driver goes online, we notify the server so we 
                    // can start receiving ride requests.
                    is_online = 1;
                    packet.type = MSG_LOC_UPDATE;
                    sprintf(packet.payload, "%d %d %d", STATUS_AVAILABLE, current_x, current_y);
                    send(sock, &packet, sizeof(packet), 0);
                    printf("Status set to: AVAILABLE.\n");
                    break;
                case 2:
                    // Going offline tells the server to stop sending us rides.
                    is_online = 0;
                    packet.type = MSG_LOC_UPDATE;
                    sprintf(packet.payload, "%d %d %d", STATUS_OFFLINE, current_x, current_y);
                    send(sock, &packet, sizeof(packet), 0);
                    printf("Status set to: OFFLINE.\n");
                    break;
                case 3:
                    // We send our new coordinates to the server so it can 
                    // calculate the distance when a rider makes a request.
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
                case 4: {
                    // This reads the trip ledger file and filters for our driver ID
                    // to calculate how many trips we've done and how much we've made.
                    FILE *fp = fopen("data/ledger.txt", "r");
                    if (!fp) {
                        printf("No earnings history available yet.\n");
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
                    printf("\n--- MY EARNINGS REPORT ---\n");
                    char line[256];
                    int total_money = 0;
                    int trip_count = 0;
                    while (fgets(line, sizeof(line), fp)) {
                        int r_id, d_id, sx, sy, ex, ey, fare;
                        if (sscanf(line, "TRIP %d %d %d %d %d %d %d", &r_id, &d_id, &sx, &sy, &ex, &ey, &fare) == 7) {
                            if (d_id == my_id) {
                                printf(" -> Trip: From (%d,%d) to (%d,%d) | Fare: $%d\n", sx, sy, ex, ey, fare);
                                total_money += fare;
                                trip_count++;
                            }
                        }
                    }
                    printf("---------------------------\n");
                    printf(" TOTAL TRIPS: %d\n", trip_count);
                    printf(" TOTAL EARNED: $%d\n", total_money);
                    lock.l_type = F_UNLCK;
                    fcntl(fd, F_SETLK, &lock);
                    fclose(fp);
                    break;
                }
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
