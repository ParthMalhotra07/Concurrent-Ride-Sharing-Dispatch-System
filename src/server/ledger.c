#include "ledger.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#define LEDGER_FILE "data/ledger.txt"

void init_ledger() {
    int fd = open(LEDGER_FILE, O_CREAT | O_WRONLY | O_APPEND, 0666);
    if (fd < 0) {
        perror("Failed to create ledger file");
        exit(1);
    }
    close(fd);
}

void log_trip(int rider_id, int driver_id, int start_x, int start_y, int end_x, int end_y, int fare) {
    int fd = open(LEDGER_FILE, O_WRONLY | O_APPEND);
    if (fd < 0) {
        perror("Failed to open ledger file");
        return;
    }

    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK; // Exclusive write lock
    lock.l_whence = SEEK_END;
    lock.l_start = 0;
    lock.l_len = 0; // Lock to end of file

    // Block until lock is acquired
    if (fcntl(fd, F_SETLKW, &lock) == -1) {
        perror("Failed to acquire log lock");
        close(fd);
        return;
    }

    // Write trip details safely
    char entry[256];
    sprintf(entry, "TRIP %d %d %d %d %d %d %d\n", 
            rider_id, driver_id, start_x, start_y, end_x, end_y, fare);
    
    write(fd, entry, strlen(entry));

    // Release lock
    lock.l_type = F_UNLCK;
    if (fcntl(fd, F_SETLK, &lock) == -1) {
        perror("Failed to release lock");
    }

    close(fd);
}
