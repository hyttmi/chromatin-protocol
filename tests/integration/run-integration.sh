#!/usr/bin/env bash
# =============================================================================
# chromatindb Integration Test Runner
#
# Discovers and runs all test_*.sh scripts in tests/integration/.
# Reports per-test pass/fail and prints a summary.
#
# Usage:
#   bash tests/integration/run-integration.sh [--skip-build] [--filter PATTERN]
#
# Options:
#   --skip-build   Skip Docker image build (use existing chromatindb:test image)
#   --filter PAT   Run only tests matching shell glob pattern (e.g. "crypt01")
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# --- Argument Parsing --------------------------------------------------------

SKIP_BUILD=false
FILTER=""
FORWARD_ARGS=()

for arg in "$@"; do
    case "$arg" in
        --skip-build)
            SKIP_BUILD=true
            FORWARD_ARGS+=("--skip-build")
            ;;
        --filter)
            # Next arg is the pattern; handled below
            FILTER="__NEXT__"
            ;;
        *)
            if [[ "$FILTER" == "__NEXT__" ]]; then
                FILTER="$arg"
            else
                echo "Unknown argument: $arg" >&2
                exit 1
            fi
            ;;
    esac
done

# Reset FILTER if it was never filled
[[ "$FILTER" == "__NEXT__" ]] && FILTER=""

# --- Logging -----------------------------------------------------------------

log() {
    echo "[$(date +%H:%M:%S)] $*" >&2
}

# --- Discover Tests ----------------------------------------------------------

mapfile -t ALL_TESTS < <(find "$SCRIPT_DIR" -maxdepth 1 -name "test_*.sh" -type f | sort)

if [[ ${#ALL_TESTS[@]} -eq 0 ]]; then
    log "No test_*.sh scripts found in $SCRIPT_DIR"
    exit 0
fi

# Apply filter
TESTS=()
for t in "${ALL_TESTS[@]}"; do
    name="$(basename "$t")"
    if [[ -z "$FILTER" || "$name" == *"$FILTER"* ]]; then
        TESTS+=("$t")
    fi
done

if [[ ${#TESTS[@]} -eq 0 ]]; then
    log "No tests match filter: $FILTER"
    exit 0
fi

log "Found ${#TESTS[@]} test(s) to run"

# --- Build Image (once) ------------------------------------------------------

if [[ "$SKIP_BUILD" == false ]]; then
    REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
    log "Building chromatindb:test image..."
    docker build -t chromatindb:test "$REPO_ROOT"
    log "Image build complete"
fi

# --- Run Tests ---------------------------------------------------------------

PASSED=0
FAILED=0
FAILED_NAMES=()

for test_script in "${TESTS[@]}"; do
    name="$(basename "$test_script")"
    log "Running $name..."
    test_start=$(date +%s)

    set +e
    bash "$test_script" "${FORWARD_ARGS[@]}"
    rc=$?
    set -e

    test_end=$(date +%s)
    duration=$(( test_end - test_start ))

    if [[ $rc -eq 0 ]]; then
        log "PASS: $name (${duration}s)"
        PASSED=$((PASSED + 1))
    else
        log "FAIL: $name (${duration}s, exit $rc)"
        FAILED=$((FAILED + 1))
        FAILED_NAMES+=("$name")
    fi
done

# --- Summary -----------------------------------------------------------------

TOTAL=$((PASSED + FAILED))

echo ""
echo "=== Integration Test Summary ==="
echo "PASS: $PASSED/$TOTAL"
echo "FAIL: $FAILED/$TOTAL"

if [[ $FAILED -gt 0 ]]; then
    for fn in "${FAILED_NAMES[@]}"; do
        echo "  - $fn"
    done
    exit 1
fi

exit 0
