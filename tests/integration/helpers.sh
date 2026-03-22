#!/usr/bin/env bash
# =============================================================================
# chromatindb Integration Test Helpers
#
# Sourced by all test_*.sh scripts. Provides Docker orchestration primitives
# for building, starting, and inspecting chromatindb test nodes.
#
# Usage: source "$(dirname "${BASH_SOURCE[0]}")/helpers.sh"
# =============================================================================
set -euo pipefail

# --- Paths -------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
COMPOSE_FILE="$SCRIPT_DIR/docker-compose.test.yml"
COMPOSE="docker compose -f $COMPOSE_FILE -p chromatindb-test"
NETWORK="chromatindb-test_test-net"
IMAGE="chromatindb:test"

# --- Configuration -----------------------------------------------------------

SKIP_BUILD=false
HEALTH_TIMEOUT=60

# Parse common flags from caller's arguments (if any)
for arg in "$@"; do
    case "$arg" in
        --skip-build) SKIP_BUILD=true ;;
    esac
done

# --- Logging -----------------------------------------------------------------

log() {
    echo "[$(date +%H:%M:%S)] $*" >&2
}

fail() {
    log "FAIL: $*"
    exit 1
}

pass() {
    log "PASS: $*"
}

# --- Dependency Checks -------------------------------------------------------

check_deps() {
    local missing=()
    command -v docker >/dev/null 2>&1 || missing+=("docker")
    docker compose version >/dev/null 2>&1 || missing+=("docker compose")
    command -v jq >/dev/null 2>&1 || missing+=("jq")

    if [[ ${#missing[@]} -gt 0 ]]; then
        fail "Missing dependencies: ${missing[*]}"
    fi
    log "Dependencies OK: docker, docker compose, jq"
}

# --- Docker Image Build ------------------------------------------------------

build_image() {
    if [[ "$SKIP_BUILD" == true ]]; then
        log "Skipping image build (--skip-build)"
        return 0
    fi
    log "Building $IMAGE image..."
    docker build -t "$IMAGE" "$REPO_ROOT"
    log "Image build complete"
}

# --- Cleanup -----------------------------------------------------------------

cleanup() {
    log "Cleaning up test topology..."
    $COMPOSE down -v --remove-orphans 2>/dev/null || true
}

# --- Node Lifecycle ----------------------------------------------------------

start_nodes() {
    log "Starting test nodes..."
    $COMPOSE up -d "$@"
    wait_healthy chromatindb-test-node1
    wait_healthy chromatindb-test-node2
    log "All test nodes healthy"
}

wait_healthy() {
    local container="$1"
    local timeout="${2:-$HEALTH_TIMEOUT}"
    local start
    start=$(date +%s)

    while true; do
        local status
        status=$(docker inspect --format '{{.State.Health.Status}}' "$container" 2>/dev/null || echo "missing")
        if [[ "$status" == "healthy" ]]; then
            log "$container is healthy"
            return 0
        fi
        local elapsed=$(( $(date +%s) - start ))
        if [[ $elapsed -ge $timeout ]]; then
            log "ERROR: Timeout waiting for $container (status: $status after ${elapsed}s)"
            return 1
        fi
        sleep 1
    done
}

# --- Blob Count / Metrics ----------------------------------------------------

get_blob_count() {
    local container="$1"
    docker kill -s USR1 "$container" >/dev/null 2>&1 || true
    sleep 2
    local count
    # SIGUSR1 dumps: metrics line, then per-namespace stats (can be 1000+ lines).
    # Use --tail 5000 to ensure the metrics: line is captured even with many namespaces.
    count=$(docker logs --tail 5000 "$container" 2>&1 | grep "metrics:" | tail -1 | grep -oP 'blobs=\K[0-9]+' || echo "0")
    echo "$count"
}

wait_sync() {
    local container="$1"
    local expected="$2"
    local timeout="${3:-120}"
    local start
    start=$(date +%s)

    while true; do
        local count
        count=$(get_blob_count "$container")
        local elapsed=$(( $(date +%s) - start ))

        log "Sync wait $container: $count/$expected blobs (${elapsed}s)"

        if [[ "$count" -ge "$expected" ]]; then
            return 0
        fi

        if [[ $elapsed -ge $timeout ]]; then
            log "WARN: Sync timeout ($count/$expected after ${elapsed}s)"
            return 1
        fi

        sleep 3
    done
}

# --- Tool Wrappers -----------------------------------------------------------

run_loadgen() {
    local target_host="$1"
    shift
    docker run --rm --network "$NETWORK" \
        --entrypoint chromatindb_loadgen \
        "$IMAGE" \
        --target "${target_host}:4200" "$@" \
        2>/dev/null
}

run_verify() {
    local container="$1"
    shift
    docker exec -i "$container" chromatindb_verify "$@"
}
