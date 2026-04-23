#ifndef AUTH_H
#define AUTH_H

#include "../common/structs.h"

// Authenticate a user by checking the text file users.dat
// Returns 1 on success, 0 on failure. Fills `out_user` with the details.
int authenticate_user(const char* username, const char* password, UserRecord* out_user);

#endif
