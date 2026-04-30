#ifndef STRUCTS_H
#define STRUCTS_H

#include <stdint.h>
#include <fcntl.h>
#include <semaphore.h>

/* 
   We define some constants here for the shared memory names and the max 
   number of drivers we want to handle in the grid.
*/
#define MAX_DRIVERS 100
#define SHM_NAME "/ride_share_shm"
#define SEM_POOL_NAME "/sem_driver_pool"
#define SURGE_SHM_NAME "/surge_multiplier_shm"
#define SHM_SIZE (sizeof(SystemGridMap))

/* 
   The different types of messages that the clients and server send to each other.
   This helps us know what to do with the data in the payload.
*/
typedef enum {
    MSG_AUTH_REQ, 
    MSG_AUTH_RES, 
    MSG_LOC_UPDATE, 
    MSG_RIDE_REQ, 
    MSG_RIDE_MATCHED, 
    MSG_RIDE_OFFER,
    MSG_RIDE_ACCEPT,
    MSG_RIDE_REJECT,
    MSG_ERROR,
    MSG_DISCONNECT,
    MSG_ADMIN_ACTION
} MessageType;

/* 
   The packet structure for every socket message. 
   We just use a fixed size payload to keep things simple for this project.
*/
typedef struct {
    MessageType type;
    char payload[256];  
} MessagePacket;

/* 
   Roles for different types of users in our system.
*/
typedef enum { ROLE_RIDER = 0, ROLE_DRIVER = 1, ROLE_ADMIN = 2 } UserRole;

/* 
   Basic info for a user that we read from the database.
*/
typedef struct {
    int user_id;
    char username[32];
    char password[64]; 
    UserRole role;
    int is_banned;
} UserRecord;

/* 
   Simple x,y coordinates for locations on the grid.
*/
typedef struct {
    int x;
    int y;
} Location;

/* 
   The current state of a driver (if they are online, busy, etc.)
*/
typedef enum { STATUS_OFFLINE = 0, STATUS_AVAILABLE = 1, STATUS_ON_TRIP = 2 } DriverStatus;

/* 
   This is what we store in the shared memory grid for each driver.
*/
typedef struct {
    int driver_id;
    DriverStatus status;
    Location current_loc;
} SharedDriverState;

/* 
   The full map of all drivers stored in the shared memory segment.
*/
typedef struct {
    SharedDriverState grid[MAX_DRIVERS]; 
} SystemGridMap;

/* 
   Used for communicating the surge price multiplier between processes.
*/
typedef struct {
    double multiplier;
} SurgeState;

#endif
