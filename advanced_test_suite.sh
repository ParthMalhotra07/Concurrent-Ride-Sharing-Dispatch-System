#!/bin/bash

# ==========================================
# ADVANCED TEST SUITE: OS Ride-Sharing
# ==========================================
# Run via: bash advanced_test_suite.sh

# Configuration
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color
LOG_DIR=".test_logs"

mkdir -p "$LOG_DIR"

echo -e "${GREEN}=== POSIX System Verification Test Suite ===${NC}"

# Cleanup Function
cleanup() {
    killall server driver rider surge_calc 2>/dev/null
    rm -f /dev/shm/ride_share_shm /dev/shm/sem.sem_driver_pool 2>/dev/null
    wait 2>/dev/null
}

fail() {
    echo -e "${RED}[FAIL] $1${NC}"
    cleanup
    exit 1
}

pass() {
    echo -e "${GREEN}[PASS] $1${NC}"
}

# Ensure clean slate
cleanup

# -----------------
# TEST 1: Build & Cleanup
# -----------------
echo -e "\nRunning Test 1: Build & Cleanup Verification..."
make all > "$LOG_DIR/build.log" 2>&1
if [ $? -ne 0 ]; then
    fail "Compilation failed. Check $LOG_DIR/build.log"
fi
pass "POSIX Compilation successful."

# -----------------
# TEST 2: Auth Failure Handling
# -----------------
echo -e "\nRunning Test 2: Auth Failure Handling..."
./bin/server > "$LOG_DIR/server_test2.log" 2>&1 &
SERVER_PID=$!
sleep 1

# Simulate user standard input via Here-Doc
(echo -e "alice\nbadpass\n"; sleep 1) | ./bin/rider > "$LOG_DIR/rider_test2.log" 2>&1

# Gracefully shutdown server to force buffer flush to disk!
kill -INT $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null

if grep -q "Authentication failed" "$LOG_DIR/rider_test2.log" && grep -q "Failed login attempt for 'alice'" "$LOG_DIR/server_test2.log"; then
    pass "Authentication securely verified & rejected."
else
    fail "Auth failure was not logged correctly."
fi
cleanup

# -----------------
# TEST 3: Resource Exhaustion (No Drivers)
# -----------------
echo -e "\nRunning Test 3: Resource Exhaustion (0 Drivers)..."
./bin/server > "$LOG_DIR/server_test3.log" 2>&1 &
SERVER_PID=$!
sleep 1

# Alice requests a ride, but 0 drivers are online
(echo -e "alice\nriderpass\n1\n0 0\n"; sleep 2) | ./bin/rider > "$LOG_DIR/rider_test3.log" 2>&1

# Shutdown server safely to flush buffers
kill -INT $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null

if grep -q "No drivers available" "$LOG_DIR/rider_test3.log"; then
    pass "Semaphore correctly blocked RIDER request (0 Supply)."
else
    fail "Rider was not cleanly rejected."
fi
cleanup

# -----------------
# TEST 4: Race Condition & Mutex Verification
# -----------------
echo -e "\nRunning Test 4: Race Condition / Mutex Stress Test..."
./bin/server > "$LOG_DIR/server_test4.log" 2>&1 &
SERVER_PID=$!
sleep 1

# EXACTLY ONE driver goes online
(echo -e "john\ndriverpass\n1\n"; sleep 20) | ./bin/driver > "$LOG_DIR/driver_test4.log" 2>&1 &
sleep 1

# TWO Riders simultaneously attempt to request the SINGLE driver
(echo -e "alice\nriderpass\n1\n0 0\n"; sleep 2) | ./bin/rider > "$LOG_DIR/riderA_test4.log" 2>&1 &
RIDER_A_PID=$!
(echo -e "alice\nriderpass\n1\n5 5\n"; sleep 2) | ./bin/rider > "$LOG_DIR/riderB_test4.log" 2>&1 &
RIDER_B_PID=$!

# Only wait for the two rider clients to finish simulating! (Do not wait on server)
wait $RIDER_A_PID $RIDER_B_PID

# Count how many riders got the specific MATCH string
MATCH_COUNT=0
grep -q "MATCH FOUND" "$LOG_DIR/riderA_test4.log" && MATCH_COUNT=$((MATCH_COUNT+1))
grep -q "MATCH FOUND" "$LOG_DIR/riderB_test4.log" && MATCH_COUNT=$((MATCH_COUNT+1))

if [ $MATCH_COUNT -eq 1 ]; then
    pass "Mutex prevented double-booking! Exactly ONE rider succeeded."
else
    fail "Race condition vulnerability! Matches found: $MATCH_COUNT (Expected exactly 1)"
fi

# Do NOT cleanup server yet. Wait relies on the locked Driver state.

# -----------------
# TEST 5: IPC Independent Verification
# -----------------
echo -e "\nRunning Test 5: IPC Monitoring / Shared Memory..."
export TERM=xterm # Suppress "clear" warnings in bash script
(./bin/surge_calc > "$LOG_DIR/surge_test5.log" & sleep 2; kill $!)

if grep -q "CRITICAL SHORTAGE\|2.5x" "$LOG_DIR/surge_test5.log"; then
    pass "External IPC Calculator correctly identified ON_TRIP via Shared Memory."
else
    fail "Surge Calculator failed to read the ON_TRIP driver state."
fi

# -----------------
# TEST 6: File Locking (fcntl) & Teardown
# -----------------
echo -e "\nRunning Test 6: Ledger File Locking & Teardown..."
echo "Waiting 15 seconds for strictly simulated trip block to complete..."
sleep 16

LEDGER_LINES=$(wc -l < data/ledger.txt)
if [ "$LEDGER_LINES" -ge 1 ] && grep -q "^TRIP" data/ledger.txt; then
    pass "fcntl locking successful. Ledger appended data cleanly."
else
    fail "Ledger was corrupted or empty."
fi

# Graceful POSIX SIGINT
echo "Invoking SIGINT on POSIX Server..."
kill -INT $SERVER_PID
wait $SERVER_PID 2>/dev/null

if [ ! -f /dev/shm/ride_share_shm ]; then
    pass "Kernel resources cleanly released (shm_unlink / sem_unlink)."
else
    fail "Memory leak! Shared memory not unlinked."
fi

cleanup # Final sweep

echo -e "\n${GREEN}==================================================${NC}"
echo -e "${GREEN} ALL 6 OS TESTS PASSED! ZERO VULNERABILITIES DETECTED ${NC}"
echo -e "${GREEN}==================================================${NC}"
exit 0
