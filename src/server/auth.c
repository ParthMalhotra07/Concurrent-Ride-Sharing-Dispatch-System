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
    //my authenticate fn works in O(n) time complexity right now 
    //can be improved but would work fine for current model 

    while (fgets(line, sizeof(line), fp)) {
        UserRecord temp;
        // Parse: id username password role is_banned
        // We use %d %31s %63s %d %d
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
