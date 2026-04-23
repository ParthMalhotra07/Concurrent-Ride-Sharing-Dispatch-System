#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "../common/structs.h"

int main() {
    printf("=== SURGE PRICING CALCULATOR ===\n");
    printf("Monitoring Shared Memory Grid...\n");

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

        float surge_multiplier = 1.0;
        if (available_drivers == 0 && on_trip_drivers > 0) {
            surge_multiplier = 2.5; // High demand, no supply
        } else if (available_drivers > 0 && available_drivers <= 2) {
            surge_multiplier = 1.5; // Low supply
        } else {
            surge_multiplier = 1.0; // Normal
        }

        // Use standard clear screen for a dashboard feel
        system("clear");
        printf("==========================================\n");
        printf("        SURGE PRICING DASHBOARD           \n");
        printf("==========================================\n\n");
        
        printf(" LIVE METRICS:\n");
        printf("  [-] Drivers Available : %d\n", available_drivers);
        printf("  [-] Drivers On Trip   : %d\n", on_trip_drivers);
        printf("  [-] Drivers Offline   : %d\n\n", offline_drivers);
        
        printf("==========================================\n");
        if (surge_multiplier >= 2.0) {
            printf(" CURRENT SURGE: %.1fx (CRITICAL SHORTAGE!)\n", surge_multiplier);
        } else if (surge_multiplier > 1.0) {
            printf(" CURRENT SURGE: %.1fx (ELEVATED DEMAND)\n", surge_multiplier);
        } else {
            printf(" CURRENT SURGE: 1.0x (NORMAL CONDITIONS)\n");
        }
        printf("==========================================\n");
        printf("Monitoring Shared Memory Grid instantly...\n");
        fflush(stdout);

        sleep(2);
    }

    // Cleanup (unreachable in infinite loop but good practice)
    munmap(grid_shm, SHM_SIZE);
    close(shm_fd);
    return 0;
}
