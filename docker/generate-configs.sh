#!/bin/bash
# Generate JSON config files for the 10-node Docker cluster + test runner.
# Each node binds to its static IP so it advertises a routable address.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONFIGS_DIR="$SCRIPT_DIR/configs"

mkdir -p "$CONFIGS_DIR"

# node0 — bootstrap node (no bootstrap peers)
cat > "$CONFIGS_DIR/node0.json" <<'EOF'
{
  "data_dir": "/data",
  "bind": "172.20.0.10",
  "tcp_port": 4000,
  "ws_port": 4001,
  "bootstrap": []
}
EOF

# node1–node9 — bootstrap to node0
for i in $(seq 1 9); do
cat > "$CONFIGS_DIR/node${i}.json" <<EOF
{
  "data_dir": "/data",
  "bind": "172.20.0.$((10 + i))",
  "tcp_port": 4000,
  "ws_port": 4001,
  "bootstrap": ["172.20.0.10:4000"]
}
EOF
done

# test-runner — bootstraps to node0
cat > "$CONFIGS_DIR/test-runner.json" <<'EOF'
{
  "data_dir": "/data",
  "bind": "172.20.0.100",
  "tcp_port": 4000,
  "ws_port": 4001,
  "bootstrap": ["172.20.0.10:4000"]
}
EOF

echo "Generated configs in $CONFIGS_DIR:"
ls -1 "$CONFIGS_DIR"
