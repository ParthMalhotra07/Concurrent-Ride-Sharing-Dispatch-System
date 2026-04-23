#ifndef STRUCTS_H
#define STRUCTS_H

#include <stdint.h>
#include <fcntl.h>
#include <semaphore.h>

// I have set the max limit of drivers at a time =100 
#define MAX_DRIVERS 100

#define SHM_NAME "/ride_share_shm"
#define SEM_POOL_NAME "/sem_driver_pool"
#define SHM_SIZE (sizeof(SystemGridMap))



//why do i need structs.h

// this acts as a shared memory
//It ensures that the Server, Clients, and IPC monitors all agree on the exact
//memory alignment and byte-offsets of the data flowing through Sockets and
//Shared Memory

// 1. Packet structure for Socket Communication
typedef enum {
   //0
    MSG_AUTH_REQ, 
    //1
    MSG_AUTH_RES, 
    //2
    MSG_LOC_UPDATE, 
    //3
    MSG_RIDE_REQ, 
    //4
    MSG_RIDE_MATCHED, 
    //5
    MSG_RIDE_END,
    //6
    MSG_ERROR,
    //7
    MSG_DISCONNECT
} MessageType;

typedef struct {
    MessageType type;
    int sender_id;      // Unique User ID
    char payload[256];  // CSV or JSON-like simple string, or raw bytes depending on operation
} MessagePacket;

// 2. Authentication Structures
typedef enum { ROLE_RIDER = 0, ROLE_DRIVER = 1, ROLE_ADMIN = 2 } UserRole;

typedef struct {
    int user_id;
    char username[32];
    char password[64]; // Plaintext for course simplicity
    UserRole role;
    int is_banned;
} UserRecord;

// 3. Application State Structures
typedef struct {
    int x;
    int y;
} Location;

typedef enum { STATUS_OFFLINE = 0, STATUS_AVAILABLE = 1, STATUS_ON_TRIP = 2 } DriverStatus;

// 4. Inter-Process Communication (Shared Memory Layout)
typedef struct {
    int driver_id;
    DriverStatus status;
    Location current_loc;
} SharedDriverState;

// Layout in the SHM segment:
typedef struct {
    SharedDriverState grid[MAX_DRIVERS]; 
} SystemGridMap;

#endif
