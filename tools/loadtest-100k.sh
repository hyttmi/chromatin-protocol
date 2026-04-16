#!/usr/bin/env bash
# Put 100K random 1KB files into the database.
# Usage: ./tools/loadtest-100k.sh [cdb_path]
#
# One cdb invocation per batch (sequential, no parallelism).
# Each batch = one connection + handshake + N puts.

set -uo pipefail

CDB="${1:-$(dirname "$0")/../build/cli/cdb}"
CDB=$(realpath "$CDB")
COUNT=100000
SIZE=1024
BATCH=500

if [ ! -x "$CDB" ]; then
    echo "Error: cdb not found at $CDB"
    exit 1
fi

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

echo "Generating $COUNT x ${SIZE}B files..."
dd if=/dev/urandom bs=$SIZE count=$COUNT 2>/dev/null | \
    split -b $SIZE -d -a 6 - "$TMPDIR/f"

echo "Generated $(find "$TMPDIR" -type f | wc -l) files"
echo "Putting to node ($BATCH files/batch, sequential)..."
echo "Using: $CDB"
echo ""

start=$(date +%s)
ok=0
fail=0
sent=0

while IFS= read -r -d '' batch_files; do
    if "$CDB" -q put $batch_files >/dev/null 2>&1; then
        n=$(echo "$batch_files" | wc -w)
        ok=$((ok + n))
    else
        fail=$((fail + 1))
    fi
    sent=$((sent + BATCH))
    if ((sent > COUNT)); then sent=$COUNT; fi
    elapsed=$(( $(date +%s) - start ))
    rate=0
    if ((elapsed > 0)); then rate=$((ok / elapsed)); fi
    printf "\r  %d / %d  (ok=%d fail=%d  %d/s)  " "$sent" "$COUNT" "$ok" "$fail" "$rate"
done < <(find "$TMPDIR" -type f -print0 | xargs -0 -n "$BATCH" printf '%s\0')

elapsed=$(( $(date +%s) - start ))
echo ""
echo ""
echo "Done: $ok ok, $fail failed batches in ${elapsed}s"
if ((elapsed > 0)); then
    echo "Rate: $((ok / elapsed))/s"
fi
