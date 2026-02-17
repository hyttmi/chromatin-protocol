#!/bin/bash
# Orchestrates the 10-node integration test:
# 1. Generate configs
# 2. Build Docker image
# 3. Start bootstrap node first, then remaining nodes
# 4. Run integration test binary as 11th node
# 5. Tear down
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
COMPOSE_FILE="$SCRIPT_DIR/docker-compose.yml"

echo "=== Generating configs ==="
"$SCRIPT_DIR/generate-configs.sh"

echo "=== Building Docker image ==="
docker compose -f "$COMPOSE_FILE" build

echo "=== Starting bootstrap node (node0) ==="
docker compose -f "$COMPOSE_FILE" up -d node0
sleep 3

echo "=== Starting nodes 1-9 ==="
docker compose -f "$COMPOSE_FILE" up -d \
    node1 node2 node3 node4 \
    node5 node6 node7 node8 node9

echo "=== Waiting 15s for bootstrap + discovery ==="
sleep 15

echo "=== Running integration test ==="
EXIT_CODE=0
docker compose -f "$COMPOSE_FILE" run --rm test-runner || EXIT_CODE=$?

echo "=== Tearing down ==="
docker compose -f "$COMPOSE_FILE" down

if [ $EXIT_CODE -eq 0 ]; then
    echo "=== ALL TESTS PASSED ==="
else
    echo "=== TESTS FAILED (exit code: $EXIT_CODE) ==="
fi

exit $EXIT_CODE
