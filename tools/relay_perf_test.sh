#!/usr/bin/env bash
# tools/relay_perf_test.sh
#
# Performance benchmark harness for chromatindb_relay.
# Builds node + relay in Release mode, runs all 4 PERF workloads via
# relay_benchmark.py, and captures the report with environment metadata.
#
# Usage: bash tools/relay_perf_test.sh
#        bash tools/relay_perf_test.sh --skip-build   # reuse existing build-release

set -euo pipefail

# =============================================================================
# Setup
# =============================================================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build-release"
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

log() { echo ">>> $*"; }

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

WORK_DIR=$(mktemp -d /tmp/relay_perf_test.XXXXXX)
log "Work directory: $WORK_DIR"
log "Repo root: $REPO_ROOT"

# =============================================================================
# Dependency check
# =============================================================================
python3 -c "import httpx, flatbuffers, oqs" 2>/dev/null || {
    echo "ERROR: Missing Python dependencies (httpx, flatbuffers, oqs)"
    exit 1
}

# =============================================================================
# Build (Release)
# =============================================================================
if [ "$SKIP_BUILD" = true ] && [ -x "$BUILD_DIR/chromatindb" ] && [ -x "$BUILD_DIR/chromatindb_relay" ]; then
    log "Skipping build (--skip-build, binaries exist)"
else
    log "Building Release binaries..."
    cmake -B "$BUILD_DIR" -S "$REPO_ROOT" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF 2>&1 | tail -5
    cmake --build "$BUILD_DIR" --target chromatindb chromatindb_relay -j"$(nproc)" 2>&1 | tail -10
fi

# Verify binaries exist
if [ ! -x "$BUILD_DIR/chromatindb" ] || [ ! -x "$BUILD_DIR/chromatindb_relay" ]; then
    echo "ERROR: Binaries not found in $BUILD_DIR"
    exit 1
fi

# =============================================================================
# Generate test configs
# =============================================================================
log "Generating test configs..."

# Node config (port 4280 for parallel safety with ASAN script)
cat > "$WORK_DIR/node.json" <<NODEEOF
{
  "bind_address": "127.0.0.1:4280",
  "uds_path": "$WORK_DIR/chromatindb.sock",
  "log_level": "info"
}
NODEEOF

# Relay config (permissive settings for benchmarking)
cat > "$WORK_DIR/relay.json" <<RELAYEOF
{
  "bind_address": "127.0.0.1",
  "bind_port": 4281,
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

"$BUILD_DIR/chromatindb" run \
    --config "$WORK_DIR/node.json" \
    --data-dir "$WORK_DIR/node_data" \
    >"$WORK_DIR/node.log" 2>&1 &
NODE_PID=$!
log "Node PID: $NODE_PID"

# Wait for UDS socket to appear (max 30s)
TIMEOUT=30
while [ ! -S "$WORK_DIR/chromatindb.sock" ] && [ "$TIMEOUT" -gt 0 ]; do
    sleep 1
    TIMEOUT=$((TIMEOUT - 1))
    # Check if node crashed
    if ! kill -0 "$NODE_PID" 2>/dev/null; then
        log "Node process exited unexpectedly. Log:"
        cat "$WORK_DIR/node.log" 2>/dev/null || true
        echo "ERROR: Node startup failed"
        exit 1
    fi
done

if [ ! -S "$WORK_DIR/chromatindb.sock" ]; then
    log "Node log:"
    cat "$WORK_DIR/node.log" 2>/dev/null || true
    echo "ERROR: Node UDS socket not found after 30s"
    exit 1
fi
log "Node UDS socket ready: $WORK_DIR/chromatindb.sock"

# =============================================================================
# Start relay
# =============================================================================
log "Starting chromatindb_relay..."

"$BUILD_DIR/chromatindb_relay" \
    --config "$WORK_DIR/relay.json" \
    >"$WORK_DIR/relay.log" 2>&1 &
RELAY_PID=$!
log "Relay PID: $RELAY_PID"

# Wait for relay to accept connections (max 15s)
RELAY_PORT=4281
TIMEOUT=15
while ! bash -c "echo > /dev/tcp/127.0.0.1/$RELAY_PORT" 2>/dev/null && [ "$TIMEOUT" -gt 0 ]; do
    sleep 1
    TIMEOUT=$((TIMEOUT - 1))
    # Check if relay crashed
    if ! kill -0 "$RELAY_PID" 2>/dev/null; then
        log "Relay process exited unexpectedly. Log:"
        cat "$WORK_DIR/relay.log" 2>/dev/null || true
        echo "ERROR: Relay startup failed"
        exit 1
    fi
done

if ! bash -c "echo > /dev/tcp/127.0.0.1/$RELAY_PORT" 2>/dev/null; then
    log "Relay log:"
    cat "$WORK_DIR/relay.log" 2>/dev/null || true
    echo "ERROR: Relay not accepting connections after 15s"
    exit 1
fi
log "Relay accepting connections on port $RELAY_PORT"

# =============================================================================
# Run benchmark (all 4 PERF workloads)
# =============================================================================
REPORT_PATH="$REPO_ROOT/tools/benchmark_report.md"
log "Running all 4 benchmark workloads (PERF-01 through PERF-04)..."
log "This will take approximately 10-20 minutes."

python3 "$REPO_ROOT/tools/relay_benchmark.py" \
    --relay-url "http://127.0.0.1:$RELAY_PORT" \
    --output "$REPORT_PATH"

# =============================================================================
# Append environment metadata
# =============================================================================
GIT_HASH=$(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo "unknown")
CPU_MODEL=$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | xargs || uname -m)
OS_INFO=$(uname -srm)

{
    echo ""
    echo "---"
    echo ""
    echo "## Environment"
    echo ""
    echo "| Field | Value |"
    echo "|-------|-------|"
    echo "| Git hash | \`$GIT_HASH\` |"
    echo "| Build type | Release |"
    echo "| CPU | $CPU_MODEL |"
    echo "| OS | $OS_INFO |"
    echo "| Date | $(date '+%Y-%m-%d %H:%M:%S %Z') |"
} >> "$REPORT_PATH"

# =============================================================================
# Summary
# =============================================================================
echo ""
echo "============================================================"
echo "  Performance Benchmark Complete"
echo "============================================================"
echo "  Report: $REPORT_PATH"
echo "  Git hash: $GIT_HASH"
echo "  Build type: Release"
echo "============================================================"
