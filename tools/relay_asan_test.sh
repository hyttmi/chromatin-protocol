#!/usr/bin/env bash
# tools/relay_asan_test.sh
#
# ASAN test harness for chromatindb_relay.
# Builds node + relay with AddressSanitizer, runs benchmark at 1/10/100
# concurrency, verifies SIGHUP config reload and SIGTERM graceful shutdown,
# and parses ASAN stderr for memory safety errors.
#
# Exit 0 = all tests pass.  Exit 1 = one or more failures.
#
# Usage: bash tools/relay_asan_test.sh
#        bash tools/relay_asan_test.sh --skip-build   # reuse existing build-asan

set -euo pipefail

# =============================================================================
# Setup
# =============================================================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build-asan"
WORK_DIR=""
NODE_PID=""
RELAY_PID=""
SKIP_BUILD=false

# Parse script arguments
for arg in "$@"; do
    case "$arg" in
        --skip-build) SKIP_BUILD=true ;;
    esac
done

# Result tracking
RESULTS=()
OVERALL_PASS=true

log()  { echo ">>> $*"; }
pass() { log "PASS: $*"; RESULTS+=("PASS: $*"); }
fail() { log "FAIL: $*"; RESULTS+=("FAIL: $*"); OVERALL_PASS=false; }

cleanup() {
    log "Cleaning up..."
    # Kill relay if running
    if [ -n "$RELAY_PID" ] && kill -0 "$RELAY_PID" 2>/dev/null; then
        kill -TERM "$RELAY_PID" 2>/dev/null || true
        sleep 1
        kill -9 "$RELAY_PID" 2>/dev/null || true
        wait "$RELAY_PID" 2>/dev/null || true
    fi
    # Kill node if running
    if [ -n "$NODE_PID" ] && kill -0 "$NODE_PID" 2>/dev/null; then
        kill -TERM "$NODE_PID" 2>/dev/null || true
        sleep 1
        kill -9 "$NODE_PID" 2>/dev/null || true
        wait "$NODE_PID" 2>/dev/null || true
    fi
    # Remove temp dir
    if [ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ]; then
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

WORK_DIR=$(mktemp -d /tmp/relay_asan_test.XXXXXX)
log "Work directory: $WORK_DIR"
log "Repo root: $REPO_ROOT"

# =============================================================================
# Build (ASAN-instrumented)
# =============================================================================
if [ "$SKIP_BUILD" = true ] && [ -x "$BUILD_DIR/chromatindb" ] && [ -x "$BUILD_DIR/chromatindb_relay" ]; then
    log "Skipping build (--skip-build, binaries exist)"
    pass "Build (skipped, reusing existing)"
else
    log "Building with -DSANITIZER=asan ..."
    if cmake -B "$BUILD_DIR" -S "$REPO_ROOT" \
        -DSANITIZER=asan \
        -DCMAKE_BUILD_TYPE=Debug \
        -DBUILD_TESTING=OFF 2>&1 | tail -5; then
        log "CMake configure OK"
    else
        fail "Build: cmake configure failed"
        exit 1
    fi

    if cmake --build "$BUILD_DIR" --target chromatindb chromatindb_relay -j"$(nproc)" 2>&1 | tail -10; then
        pass "Build (ASAN-instrumented chromatindb + chromatindb_relay)"
    else
        fail "Build: compilation failed"
        exit 1
    fi
fi

# Verify binaries exist
if [ ! -x "$BUILD_DIR/chromatindb" ] || [ ! -x "$BUILD_DIR/chromatindb_relay" ]; then
    fail "Build: binaries not found in $BUILD_DIR"
    exit 1
fi

# =============================================================================
# Generate test configs
# =============================================================================
log "Generating test configs..."

# Node config (ephemeral port, UDS in temp dir)
cat > "$WORK_DIR/node.json" <<NODEEOF
{
  "bind_address": "127.0.0.1:0",
  "uds_path": "$WORK_DIR/chromatindb.sock",
  "log_level": "info"
}
NODEEOF

# Relay config (permissive settings for stress testing)
cat > "$WORK_DIR/relay.json" <<RELAYEOF
{
  "bind_address": "127.0.0.1",
  "bind_port": 4291,
  "uds_path": "$WORK_DIR/chromatindb.sock",
  "identity_key_path": "$WORK_DIR/relay.key",
  "log_level": "info",
  "rate_limit_messages_per_sec": 0,
  "request_timeout_seconds": 0,
  "max_blob_size_bytes": 0,
  "max_connections": 1024
}
RELAYEOF

log "Node config: $WORK_DIR/node.json"
log "Relay config: $WORK_DIR/relay.json"

# =============================================================================
# Start node
# =============================================================================
log "Starting chromatindb node..."

export ASAN_OPTIONS="detect_leaks=1:halt_on_error=0:print_stats=0:log_path=/dev/stderr"
export LSAN_OPTIONS="suppressions=$REPO_ROOT/sanitizers/lsan.supp"

"$BUILD_DIR/chromatindb" run \
    --config "$WORK_DIR/node.json" \
    --data-dir "$WORK_DIR/node_data" \
    2>"$WORK_DIR/node_stderr.log" &
NODE_PID=$!
log "Node PID: $NODE_PID"

# Wait for UDS socket to appear (max 30s)
TIMEOUT=30
while [ ! -S "$WORK_DIR/chromatindb.sock" ] && [ "$TIMEOUT" -gt 0 ]; do
    sleep 1
    TIMEOUT=$((TIMEOUT - 1))
    # Check if node crashed
    if ! kill -0 "$NODE_PID" 2>/dev/null; then
        log "Node process exited unexpectedly. Stderr:"
        cat "$WORK_DIR/node_stderr.log" 2>/dev/null || true
        fail "Node startup: process exited"
        exit 1
    fi
done

if [ ! -S "$WORK_DIR/chromatindb.sock" ]; then
    log "Node stderr:"
    cat "$WORK_DIR/node_stderr.log" 2>/dev/null || true
    fail "Node startup: UDS socket not found after 30s"
    exit 1
fi
log "Node UDS socket ready: $WORK_DIR/chromatindb.sock"

# =============================================================================
# Start relay
# =============================================================================
log "Starting chromatindb_relay..."

export ASAN_OPTIONS="detect_leaks=1:halt_on_error=0:print_stats=0:log_path=/dev/stderr"
export LSAN_OPTIONS="suppressions=$REPO_ROOT/relay/lsan_suppressions.txt"

"$BUILD_DIR/chromatindb_relay" \
    --config "$WORK_DIR/relay.json" \
    2>"$WORK_DIR/relay_stderr.log" &
RELAY_PID=$!
log "Relay PID: $RELAY_PID"

# Wait for relay to accept connections (max 15s)
# Try a simple TCP check on the configured port.
RELAY_PORT=4291
TIMEOUT=15
while ! bash -c "echo > /dev/tcp/127.0.0.1/$RELAY_PORT" 2>/dev/null && [ "$TIMEOUT" -gt 0 ]; do
    sleep 1
    TIMEOUT=$((TIMEOUT - 1))
    # Check if relay crashed
    if ! kill -0 "$RELAY_PID" 2>/dev/null; then
        log "Relay process exited unexpectedly. Stderr:"
        cat "$WORK_DIR/relay_stderr.log" 2>/dev/null || true
        fail "Relay startup: process exited"
        exit 1
    fi
done

if ! bash -c "echo > /dev/tcp/127.0.0.1/$RELAY_PORT" 2>/dev/null; then
    log "Relay stderr:"
    cat "$WORK_DIR/relay_stderr.log" 2>/dev/null || true
    fail "Relay startup: not accepting connections after 15s"
    exit 1
fi
log "Relay accepting connections on port $RELAY_PORT"

# =============================================================================
# Run benchmark at concurrency 1, 10, 100
# =============================================================================
log "Running relay benchmark at concurrency 1, 10, 100..."
log "(Reduced iterations for ASAN: 20 per client, no large blobs)"

BENCHMARK_EXIT=0
python3 "$REPO_ROOT/tools/relay_benchmark.py" \
    --relay-url "http://127.0.0.1:$RELAY_PORT" \
    --concurrency-levels "1,10,100" \
    --iterations 20 \
    --warmup 3 \
    --blob-sizes "" \
    --output /dev/null \
    --mixed-duration 10 \
    2>&1 | tee "$WORK_DIR/benchmark_output.log" || BENCHMARK_EXIT=$?

if [ "$BENCHMARK_EXIT" -eq 0 ]; then
    pass "Benchmark (concurrency 1, 10, 100 -- exit code 0)"
else
    log "Benchmark exited with code $BENCHMARK_EXIT (may be ASAN slowdown)"
    fail "Benchmark (exit code $BENCHMARK_EXIT)"
fi

# Check relay and node are still alive after benchmark
if ! kill -0 "$RELAY_PID" 2>/dev/null; then
    log "Relay crashed during benchmark! Stderr:"
    tail -50 "$WORK_DIR/relay_stderr.log" 2>/dev/null || true
    fail "Relay survived benchmark (crashed)"
else
    pass "Relay survived benchmark (still running)"
fi

if ! kill -0 "$NODE_PID" 2>/dev/null; then
    log "Node crashed during benchmark! Stderr:"
    tail -50 "$WORK_DIR/node_stderr.log" 2>/dev/null || true
    fail "Node survived benchmark (crashed)"
fi

# =============================================================================
# SIGHUP signal test
# =============================================================================
log "Testing SIGHUP config reload..."

# Write modified config with rate_limit_messages_per_sec=999 using atomic mv
python3 -c "
import json, sys
with open(sys.argv[1]) as f:
    cfg = json.load(f)
cfg['rate_limit_messages_per_sec'] = 999
with open(sys.argv[2], 'w') as f:
    json.dump(cfg, f, indent=2)
" "$WORK_DIR/relay.json" "$WORK_DIR/relay.json.tmp"
mv "$WORK_DIR/relay.json.tmp" "$WORK_DIR/relay.json"

# Send SIGHUP
kill -HUP "$RELAY_PID"
sleep 2

# Check relay is still running (didn't crash on SIGHUP)
if ! kill -0 "$RELAY_PID" 2>/dev/null; then
    log "Relay crashed after SIGHUP! Stderr:"
    tail -50 "$WORK_DIR/relay_stderr.log" 2>/dev/null || true
    fail "SIGHUP: relay crashed"
else
    # Check for rate_limit reload confirmation in stderr
    if grep -q "rate_limit reloaded: 999" "$WORK_DIR/relay_stderr.log"; then
        pass "SIGHUP: rate_limit reloaded to 999"
    else
        log "Relay stderr (last 20 lines):"
        tail -20 "$WORK_DIR/relay_stderr.log" 2>/dev/null || true
        fail "SIGHUP: rate_limit reload confirmation not found"
    fi

    # Check for config reload failure
    if grep -q "SIGHUP config reload failed" "$WORK_DIR/relay_stderr.log"; then
        fail "SIGHUP: config reload reported failure"
    fi
fi

# =============================================================================
# SIGTERM graceful shutdown test
# =============================================================================
log "Testing SIGTERM graceful shutdown..."

kill -TERM "$RELAY_PID"

# Wait up to 10 seconds for relay to exit
TIMEOUT=10
while kill -0 "$RELAY_PID" 2>/dev/null && [ "$TIMEOUT" -gt 0 ]; do
    sleep 1
    TIMEOUT=$((TIMEOUT - 1))
done

if kill -0 "$RELAY_PID" 2>/dev/null; then
    fail "SIGTERM: relay did not exit within 10 seconds"
    kill -9 "$RELAY_PID" 2>/dev/null || true
    wait "$RELAY_PID" 2>/dev/null || true
else
    wait "$RELAY_PID" 2>/dev/null || true
    pass "SIGTERM: relay exited cleanly"
fi

# Check for "relay stopped" confirmation
if grep -q "relay stopped" "$WORK_DIR/relay_stderr.log"; then
    pass "SIGTERM: 'relay stopped' log message confirmed"
else
    fail "SIGTERM: 'relay stopped' log message not found"
fi

# Mark relay as already stopped (cleanup won't try again)
RELAY_PID=""

# =============================================================================
# Stop node and collect ASAN output
# =============================================================================
log "Stopping node..."
if [ -n "$NODE_PID" ] && kill -0 "$NODE_PID" 2>/dev/null; then
    kill -TERM "$NODE_PID" 2>/dev/null || true
    TIMEOUT=10
    while kill -0 "$NODE_PID" 2>/dev/null && [ "$TIMEOUT" -gt 0 ]; do
        sleep 1
        TIMEOUT=$((TIMEOUT - 1))
    done
    if kill -0 "$NODE_PID" 2>/dev/null; then
        kill -9 "$NODE_PID" 2>/dev/null || true
    fi
    wait "$NODE_PID" 2>/dev/null || true
fi
NODE_PID=""

# =============================================================================
# Parse ASAN output
# =============================================================================
log "Parsing ASAN output..."

ASAN_ERRORS=0

# Check relay stderr for AddressSanitizer errors
if grep -qE 'ERROR: AddressSanitizer' "$WORK_DIR/relay_stderr.log" 2>/dev/null; then
    log "AddressSanitizer errors in RELAY stderr:"
    grep -A 20 'ERROR: AddressSanitizer' "$WORK_DIR/relay_stderr.log" || true
    ASAN_ERRORS=1
fi

# Check node stderr for AddressSanitizer errors
if grep -qE 'ERROR: AddressSanitizer' "$WORK_DIR/node_stderr.log" 2>/dev/null; then
    log "AddressSanitizer errors in NODE stderr:"
    grep -A 20 'ERROR: AddressSanitizer' "$WORK_DIR/node_stderr.log" || true
    ASAN_ERRORS=1
fi

# Check relay stderr for LeakSanitizer errors (un-suppressed leaks)
if grep -qE 'ERROR: LeakSanitizer' "$WORK_DIR/relay_stderr.log" 2>/dev/null; then
    # LSAN fires when there are un-suppressed leaks even after applying suppressions
    log "LeakSanitizer report in RELAY stderr:"
    grep -A 30 'ERROR: LeakSanitizer' "$WORK_DIR/relay_stderr.log" || true
    ASAN_ERRORS=1
fi

# Check node stderr for LeakSanitizer errors (un-suppressed leaks)
if grep -qE 'ERROR: LeakSanitizer' "$WORK_DIR/node_stderr.log" 2>/dev/null; then
    log "LeakSanitizer report in NODE stderr:"
    grep -A 30 'ERROR: LeakSanitizer' "$WORK_DIR/node_stderr.log" || true
    ASAN_ERRORS=1
fi

if [ "$ASAN_ERRORS" -eq 0 ]; then
    pass "ASAN-clean: no AddressSanitizer or LeakSanitizer errors"
else
    fail "ASAN-clean: memory safety issues detected (see above)"
fi

# =============================================================================
# Summary
# =============================================================================
echo ""
echo "============================================================"
echo "  ASAN Test Results"
echo "============================================================"
for result in "${RESULTS[@]}"; do
    echo "  $result"
done
echo "============================================================"

if [ "$OVERALL_PASS" = true ]; then
    echo ""
    echo "ALL TESTS PASSED"
    exit 0
else
    echo ""
    echo "SOME TESTS FAILED"
    exit 1
fi
