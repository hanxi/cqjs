#!/bin/bash
# Test loading 洛雪音乐源 script via eval_file
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CQJS="$SCRIPT_DIR/cqjs"
LX_SCRIPT="$SCRIPT_DIR/../data/plugins_data/lxmusic/scripts/洛雪音乐源.js"

echo "=== Building cqjs ==="
make -C "$SCRIPT_DIR" -j4

if [ ! -f "$LX_SCRIPT" ]; then
    echo "ERROR: 洛雪音乐源.js not found at: $LX_SCRIPT"
    exit 1
fi

echo ""
echo "=== Test: Load 洛雪音乐源 script via eval_file ==="
echo "Script: $LX_SCRIPT ($(wc -c < "$LX_SCRIPT") bytes)"
echo ""

# Send eval_file command via pipe (stdin EOF after command)
STDOUT_FILE=$(mktemp)
STDERR_FILE=$(mktemp)

printf '{"id":"1","type":"eval_file","path":"%s"}\n' "$LX_SCRIPT" \
    | timeout 15 "$CQJS" > "$STDOUT_FILE" 2> "$STDERR_FILE"
EXIT_CODE=$?

echo "Exit code: $EXIT_CODE"
echo ""
echo "STDOUT:"
cat "$STDOUT_FILE"
echo ""
echo ""
echo "STDERR (last 30 lines):"
tail -30 "$STDERR_FILE"
echo ""

# Check results
if grep -q '"type":"result"' "$STDOUT_FILE"; then
    echo "=== eval_file: SUCCESS (script loaded) ==="
else
    echo "=== eval_file: FAILED ==="
fi

if grep -q '"type":"event"' "$STDOUT_FILE"; then
    echo "=== lx.send events detected ==="
    grep '"type":"event"' "$STDOUT_FILE"
else
    echo "=== No lx.send events (may need dispatch to trigger inited) ==="
fi

rm -f "$STDOUT_FILE" "$STDERR_FILE"
echo ""
echo "=== Test done ==="
