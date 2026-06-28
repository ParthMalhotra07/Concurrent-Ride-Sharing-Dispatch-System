#ifndef STRUCTS_H
#define STRUCTS_H

#include <stdint.h>
#include <fcntl.h>
#include <semaphore.h>

/* 
   Constants defining shared memory identifiers, sizes, and driver limits.
*/
#define MAX_DRIVERS 100
#define MAX_USERS 2000
#define SHM_NAME "/ride_share_shm"
#define SEM_POOL_NAME "/sem_driver_pool"
#define SURGE_SHM_NAME "/surge_multiplier_shm"
#define SHM_SIZE (sizeof(SystemGridMap))

/* 
   Message types sent between the clients and the central server to categorize payload data.
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
    MSG_ADMIN_ACTION,
    MSG_TRIP_HISTORY_REQ,
    MSG_TRIP_HISTORY_RES,
    MSG_TRIP_HISTORY_END,
    MSG_REVENUE_REPORT_REQ,
    MSG_REVENUE_REPORT_RES,
    MSG_TRIP_COMPLETED
} MessageType;

/* 
   Socket packet structure containing a type field and a fixed-size payload buffer.
*/
typedef struct {
    MessageType type;
    char payload[256];  
} MessagePacket;

/* 
   User roles specifying rider, driver, or admin privileges.
*/
typedef enum { ROLE_RIDER = 0, ROLE_DRIVER = 1, ROLE_ADMIN = 2 } UserRole;

/* 
   User record format parsed from database files.
*/
typedef struct {
    int user_id;
    char username[32];
    char password[64]; 
    UserRole role;
    int is_banned;
} UserRecord;

/* 
   Coordinates representing a position on the system grid.
*/
typedef struct {
    int x;
    int y;
} Location;

/* 
   Status indicating driver availability.
*/
typedef enum { STATUS_OFFLINE = 0, STATUS_AVAILABLE = 1, STATUS_ON_TRIP = 2 } DriverStatus;

/* 
   State representation of a driver tracked inside the shared memory grid.
*/
typedef struct {
    int driver_id;
    DriverStatus status;
    Location current_loc;
} SharedDriverState;

/* 
   Grid mapping containing location and status information for all registered drivers.
*/
typedef struct {
    SharedDriverState grid[MAX_DRIVERS]; 
} SystemGridMap;

/* 
   State representation of the dynamic demand-based surge price multiplier.
*/
typedef struct {
    double multiplier;
} SurgeState;

#endif
