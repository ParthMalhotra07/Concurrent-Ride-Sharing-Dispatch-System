#ifndef LEDGER_H
#define LEDGER_H

#include "../common/structs.h"

// Initialize the ledger file (create if not exists)
void init_ledger();

// Log a completed trip to the ledger file
// Uses fcntl advisory locks to ensure safe writing
void log_trip(int rider_id, int driver_id, int start_x, int start_y, int end_x, int end_y, int fare);

#endif
