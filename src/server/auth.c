#include "auth.h"
#include <stdio.h>
#include <string.h>

#define USERS_FILE "data/users.dat"

int authenticate_user(const char* username, const char* password, UserRecord* out_user) {
    FILE *fp = fopen(USERS_FILE, "r");
    if (!fp) {
        perror("Error opening users.dat");
        return 0;
    }

    char line[256];
    // We loop through the users file line by line to find a matching username 
    // and password. It's a simple flat-file database approach.
    while (fgets(line, sizeof(line), fp)) {
        UserRecord temp;
        // The file format is: ID USERNAME PASSWORD ROLE BANNED_STATUS
        int role_int;
        if (sscanf(line, "%d %31s %63s %d %d", 
                &temp.user_id, temp.username, temp.password, 
                &role_int, &temp.is_banned) == 5) {
            
            temp.role = (UserRole)role_int;

            if (strcmp(temp.username, username) == 0 && strcmp(temp.password, password) == 0) {
                *out_user = temp;
                fclose(fp);
                return 1; // Success
            }
        }
    }

    fclose(fp);
    return 0; // Not found / auth failed
}
