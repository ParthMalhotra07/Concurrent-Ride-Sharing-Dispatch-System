#include "match_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>

static SystemGridMap* grid_shm = NULL;
static sem_t* driver_pool_sem = NULL;

// Mutex to protect iterating over and editing the grid map to prevent double booking.
static pthread_mutex_t driver_mutex = PTHREAD_MUTEX_INITIALIZER;

void init_match_core(SystemGridMap* shm_ptr, sem_t* pool_sem) {
    grid_shm = shm_ptr;
    driver_pool_sem = pool_sem;
}

void update_driver_status(int driver_id, DriverStatus status, int x, int y) {
    if (!grid_shm) return;

    pthread_mutex_lock(&driver_mutex);
    
    int driver_index = -1;
    // Find driver or an empty slot
    for (int i = 0; i < MAX_DRIVERS; i++) {
        if (grid_shm->grid[i].driver_id == driver_id || grid_shm->grid[i].status == STATUS_OFFLINE) {
            driver_index = i;
            break;
        }
    }

    if (driver_index != -1) {
        DriverStatus old_status = grid_shm->grid[driver_index].status;
        
        grid_shm->grid[driver_index].driver_id = driver_id;
        grid_shm->grid[driver_index].status = status;
        grid_shm->grid[driver_index].current_loc.x = x;
        grid_shm->grid[driver_index].current_loc.y = y;

        // Manage semaphore if status changed regarding AVAILABILITY
        if (old_status != STATUS_AVAILABLE && status == STATUS_AVAILABLE) {
            sem_post(driver_pool_sem);
        } else if (old_status == STATUS_AVAILABLE && status != STATUS_AVAILABLE) {
            sem_trywait(driver_pool_sem); // Consume one available slot silently
        }
    }

    pthread_mutex_unlock(&driver_mutex);
}

int request_ride(int rider_id, int rider_x, int rider_y, int* exclude_list, int exclude_count) {
    if (!grid_shm) return -1;

    printf("[MATCH] Rider %d at (%d,%d) is requesting a ride...\n", rider_id, rider_x, rider_y);

    // Concurrency check: wait for an available driver in the pool
    if (sem_trywait(driver_pool_sem) != 0) {
        printf("[MATCH] No available drivers in pool for Rider %d.\n", rider_id);
        return -1;
    }

    pthread_mutex_lock(&driver_mutex);
    // CRITICAL SECTION: Safe from double booking
    
    int matched_driver = -1;
    int best_index = -1;
    double min_dist = 9999999.0;

    for (int i = 0; i < MAX_DRIVERS; i++) {
        if (grid_shm->grid[i].status == STATUS_AVAILABLE) {
             // Check exclude list
             int is_excluded = 0;
             for (int j = 0; j < exclude_count; j++) {
                 if (grid_shm->grid[i].driver_id == exclude_list[j]) {
                     is_excluded = 1;
                     break;
                 }
             }
             if (is_excluded) continue;

             long long dx = (long long)grid_shm->grid[i].current_loc.x - (long long)rider_x;
             long long dy = (long long)grid_shm->grid[i].current_loc.y - (long long)rider_y;
             double dist = (double)(dx * dx) + (double)(dy * dy); // Squared geographical distance

             if (dist < min_dist) {
                 min_dist = dist;
                 best_index = i;
                 matched_driver = grid_shm->grid[i].driver_id;
             }
        }
    }

    if (best_index != -1) {
        grid_shm->grid[best_index].status = STATUS_ON_TRIP; // Mark closest driver as taken
        printf("[MATCH] Success! Rider %d matched with mathematically closest Driver %d (Squared Dist: %.0f).\n", 
                rider_id, matched_driver, min_dist);
    }

    pthread_mutex_unlock(&driver_mutex);

    if (matched_driver == -1) {
        // Edge case: State drifted (should not happen with mutex but just in case)
        sem_post(driver_pool_sem);
    }
    
    return matched_driver;
}
