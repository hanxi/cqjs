#!/bin/bash
# Test script for callMusicUrl via cqjs
# This script:
# 1. Builds cqjs
# 2. Loads the 洛雪音乐源.js
# 3. Waits for 'inited' event
# 4. Sends a callMusicUrl request and waits for result

set -e

CQJS_DIR="$(cd "$(dirname "$0")" && pwd)"
SCRIPT_PATH="$CQJS_DIR/../data/plugins_data/lxmusic/scripts/洛雪音乐源.js"
CQJS_BIN="$CQJS_DIR/cqjs"
STDOUT_FILE="$CQJS_DIR/test_callMusicUrl_stdout.log"
STDERR_FILE="$CQJS_DIR/test_callMusicUrl_stderr.log"

echo "=== Building cqjs ==="
cd "$CQJS_DIR"
make

echo ""
echo "=== Test: callMusicUrl end-to-end ==="

if [ ! -f "$SCRIPT_PATH" ]; then
    echo "ERROR: Script not found: $SCRIPT_PATH"
    exit 1
fi

echo "Script: $SCRIPT_PATH ($(wc -c < "$SCRIPT_PATH") bytes)"

# Create a FIFO for communication
FIFO=$(mktemp -u)
mkfifo "$FIFO"

# Start cqjs in background, reading from FIFO
"$CQJS_BIN" < "$FIFO" > "$STDOUT_FILE" 2>"$STDERR_FILE" &
CQJS_PID=$!

# Open FIFO for writing
exec 3>"$FIFO"

echo "=== Step 1: Loading 洛雪音乐源.js ==="
echo '{"id":"1","type":"eval_file","path":"'"$SCRIPT_PATH"'"}' >&3

# Wait for script to initialize
sleep 3

echo "=== Output after loading script ==="
cat "$STDOUT_FILE"

echo ""
echo "=== Step 2: Sending callMusicUrl request ==="
echo "Source: kw (酷我音乐), Quality: 128k"
# Use kw source with a simple songInfo
echo '{"id":"2","type":"callMusicUrl","source":"kw","songInfo":"{\"name\":\"晴天\",\"singer\":\"周杰伦\",\"songmid\":\"228908\",\"rid\":\"228908\"}","quality":"128k"}' >&3

# Wait for response (up to 30 seconds)
echo "Waiting for response (up to 30s)..."
sleep 30

echo ""
echo "=== Final stdout ==="
cat "$STDOUT_FILE"

echo ""
echo "=== Final stderr ==="
cat "$STDERR_FILE"

# Cleanup
exec 3>&-
kill $CQJS_PID 2>/dev/null || true
wait $CQJS_PID 2>/dev/null || true
rm -f "$FIFO"

echo ""
echo "=== Test complete ==="
