CC = gcc
CFLAGS = -Wall -Wextra -pthread -g
LDFLAGS = -lrt

# Directories
BIN_DIR = bin
SERVER_SRC = src/server
CLIENT_SRC = src/client

IPC_SRC = src/ipc

# Targets
SERVER_BIN = $(BIN_DIR)/server
DRIVER_BIN = $(BIN_DIR)/driver
RIDER_BIN  = $(BIN_DIR)/rider
ADMIN_BIN  = $(BIN_DIR)/admin
SURGE_BIN  = $(BIN_DIR)/surge_calc

all: dirs server driver rider admin surge_calc

dirs:
	mkdir -p $(BIN_DIR) || type nul >> nul

server: $(SERVER_SRC)/server_main.c $(SERVER_SRC)/auth.c $(SERVER_SRC)/ledger.c $(SERVER_SRC)/match_core.c
	$(CC) $(CFLAGS) $^ -o $(SERVER_BIN) $(LDFLAGS)

driver: $(CLIENT_SRC)/driver.c
	$(CC) $(CFLAGS) $^ -o $(DRIVER_BIN) $(LDFLAGS)

rider: $(CLIENT_SRC)/rider.c
	$(CC) $(CFLAGS) $^ -o $(RIDER_BIN) $(LDFLAGS)

admin: $(CLIENT_SRC)/admin.c
	$(CC) $(CFLAGS) $^ -o $(ADMIN_BIN) $(LDFLAGS)

surge_calc: $(IPC_SRC)/surge_calc.c
	$(CC) $(CFLAGS) $^ -o $(SURGE_BIN) $(LDFLAGS)

clean:
	rm -rf $(BIN_DIR)
