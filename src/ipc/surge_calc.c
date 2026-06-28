#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "../common/structs.h"

/* 
   This process acts like a separate monitoring tool. It reads the shared memory 
   grid to see how many drivers are busy and then calculates a price multiplier.
*/
int main() {
    printf("Surge Pricing Calculator\n");
    printf("Monitoring Shared Memory Grid...\n");

    // Open the shared memory created by the server
    int shm_fd = shm_open(SHM_NAME, O_RDONLY, 0666);
    if (shm_fd == -1) {
        perror("shm_open failed! Make sure the server is already running.");
        return 1;
    }

    // Map the grid into my address space so I can read it
    SystemGridMap* grid_shm = mmap(NULL, SHM_SIZE, PROT_READ, MAP_SHARED, shm_fd, 0);
    if (grid_shm == MAP_FAILED) {
        perror("mmap failed");
        return 1;
    }

    // I also create another shared memory segment to send the multiplier back to the server
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
        
        // Loop through the whole grid to count the status of every driver
        for (int i = 0; i < MAX_DRIVERS; i++) {
            if (grid_shm->grid[i].status == STATUS_AVAILABLE) {
                available_drivers++;
            } else if (grid_shm->grid[i].status == STATUS_ON_TRIP) {
                on_trip_drivers++;
            } else if (grid_shm->grid[i].driver_id != 0) {
                offline_drivers++;
            }
        }

        /* 
           Simple supply-and-demand logic: if there are no drivers left, I spike 
           the price. If only a few are left, I raise it slightly to 1.5x.
        */
        float surge_multiplier = 1.0;
        if (available_drivers == 0 && on_trip_drivers > 0) {
            surge_multiplier = 2.5;
        } else if (available_drivers > 0 && available_drivers <= 2) {
            surge_multiplier = 1.5;
        } else {
            surge_multiplier = 1.0;
        }

        // Save the multiplier in the shared memory so the server can read it when a trip ends
        if (surge_shm != MAP_FAILED) {
            surge_shm->multiplier = (double)surge_multiplier;
        }

        // Clear the screen using ANSI escape codes instead of system("clear") for cross-platform compatibility
        printf("\033[H\033[J");
        
        printf("\033[1;36m====================================================\n");
        printf("         Surge Pricing & Demand Monitor\n");
        printf("====================================================\033[0m\n\n");
        
        printf("  Drivers Available : \033[1;32m%d\033[0m\n", available_drivers);
        printf("  Drivers On Trip   : \033[1;33m%d\033[0m\n", on_trip_drivers);
        printf("  Drivers Offline   : \033[1;31m%d\033[0m\n\n", offline_drivers);
        
        if (surge_multiplier >= 2.0) {
            printf("Current Surge: \033[1;31m%.1fx (High Demand!)\033[0m\n", surge_multiplier);
        } else if (surge_multiplier > 1.0) {
            printf("Current Surge: \033[1;33m%.1fx (Busy)\033[0m\n", surge_multiplier);
        } else {
            printf("Current Surge: \033[1;32m1.0x (Normal)\033[0m\n");
        }
        
        fflush(stdout);
        sleep(2);
    }

    return 0;
}
