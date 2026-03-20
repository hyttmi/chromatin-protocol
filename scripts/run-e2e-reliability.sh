#!/usr/bin/env bash
# Run [daemon][e2e] tests N times and report reliability.
# Usage: ./scripts/run-e2e-reliability.sh <build_dir> [runs] [env_opts]
# Example: ./scripts/run-e2e-reliability.sh build-tsan 50 "TSAN_OPTIONS=suppressions=sanitizers/tsan.supp:halt_on_error=1"
set -euo pipefail

BUILD_DIR="${1:?Usage: $0 <build_dir> [runs] [env_opts]}"
RUNS="${2:-50}"
ENV_OPTS="${3:-}"

BINARY="$BUILD_DIR/db/chromatindb_tests"
if [[ ! -x "$BINARY" ]]; then
    # Try without db/ prefix (some build layouts differ)
    BINARY="$BUILD_DIR/chromatindb_tests"
    if [[ ! -x "$BINARY" ]]; then
        echo "ERROR: chromatindb_tests not found in $BUILD_DIR"
        exit 1
    fi
fi

TAG="[daemon][e2e]"
PASS=0
FAIL=0
FAILURES=()

echo "=== E2E Reliability Test ==="
echo "Binary: $BINARY"
echo "Tag filter: $TAG"
echo "Runs: $RUNS"
echo "Env: ${ENV_OPTS:-<none>}"
echo ""

for i in $(seq 1 "$RUNS"); do
    printf "Run %3d/%d ... " "$i" "$RUNS"
    if env $ENV_OPTS "$BINARY" "$TAG" --colour-mode none > /dev/null 2>&1; then
        echo "PASS"
        PASS=$((PASS + 1))
    else
        echo "FAIL"
        FAIL=$((FAIL + 1))
        FAILURES+=("$i")
    fi
done

echo ""
echo "=== Results ==="
echo "Passed: $PASS/$RUNS"
echo "Failed: $FAIL/$RUNS"
if [[ ${#FAILURES[@]} -gt 0 ]]; then
    echo "Failed runs: ${FAILURES[*]}"
    exit 1
fi
echo "All runs passed."
exit 0
