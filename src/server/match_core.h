#ifndef MATCH_CORE_H
#define MATCH_CORE_H

#include "../common/structs.h"

// Initialize matching structs (pointers to shm, mutexes)
void init_match_core(SystemGridMap* shm_ptr, sem_t* pool_sem);

// Update a driver's status and location
void update_driver_status(int driver_id, DriverStatus status, int x, int y);

// Attempt to match a rider with an available driver
// Returns driver_id if successful, -1 otherwise.
int request_ride(int rider_id, int rider_x, int rider_y);

#endif
