#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "../common/structs.h"

int main() {
    printf("Surge Pricing Calculator\n");
    printf("Monitoring Shared Memory Grid...\n");

    // This process doesn't talk to the server via sockets. 
    // Instead, it "spies" on the shared memory grid to see how many drivers 
    // are available and calculates the surge price accordingly.
    int shm_fd = shm_open(SHM_NAME, O_RDONLY, 0666);
    if (shm_fd == -1) {
        perror("shm_open failed! Is the server running?");
        return 1;
    }

    SystemGridMap* grid_shm = mmap(NULL, SHM_SIZE, PROT_READ, MAP_SHARED, shm_fd, 0);
    if (grid_shm == MAP_FAILED) {
        perror("mmap failed");
        return 1;
    }

    // Initialize the two-way street for the Server
    int surge_fd = shm_open(SURGE_SHM_NAME, O_CREAT | O_RDWR, 0666);
    ftruncate(surge_fd, sizeof(SurgeState));
    SurgeState* surge_shm = mmap(NULL, sizeof(SurgeState), PROT_READ | PROT_WRITE, MAP_SHARED, surge_fd, 0);
    if (surge_shm != MAP_FAILED) {
        surge_shm->multiplier = 1.0;
    }

    while (1) {
        int available_drivers = 0;
        int on_trip_drivers = 0;
        int offline_drivers = 0;
        
        for (int i = 0; i < MAX_DRIVERS; i++) {
            if (grid_shm->grid[i].status == STATUS_AVAILABLE) {
                available_drivers++;
            } else if (grid_shm->grid[i].status == STATUS_ON_TRIP) {
                on_trip_drivers++;
            } else if (grid_shm->grid[i].driver_id != 0) {
                offline_drivers++;
            }
        }

        // Simple supply-and-demand logic: if there are no drivers left, 
        // we spike the price. If only a few are left, we raise it slightly.
        float surge_multiplier = 1.0;
        if (available_drivers == 0 && on_trip_drivers > 0) {
            surge_multiplier = 2.5;
        } else if (available_drivers > 0 && available_drivers <= 2) {
            surge_multiplier = 1.5;
        } else {
            surge_multiplier = 1.0;
        }

        // Write to Shared Memory for the Server to see!
        if (surge_shm != MAP_FAILED) {
            surge_shm->multiplier = (double)surge_multiplier;
        }

        // Use standard clear screen for a dashboard feel
        system("clear");
        
        printf("Live Metrics:\n");
        printf("  Drivers Available : %d\n", available_drivers);
        printf("  Drivers On Trip   : %d\n", on_trip_drivers);
        printf("  Drivers Offline   : %d\n\n", offline_drivers);
        
        if (surge_multiplier >= 2.0) {
            printf("Current Surge: %.1fx (Critical Shortage)\n", surge_multiplier);
        } else if (surge_multiplier > 1.0) {
            printf("Current Surge: %.1fx (Elevated Demand)\n", surge_multiplier);
        } else {
            printf("Current Surge: 1.0x (Normal Conditions)\n");
        }
        printf("Monitoring Shared Memory Grid instantly...\n");
        fflush(stdout);

        sleep(2);
    }

    // Cleanup (unreachable in infinite loop but good practice)
    munmap(grid_shm, SHM_SIZE);
    close(shm_fd);
    return 0;
}
