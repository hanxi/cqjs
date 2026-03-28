#!/bin/bash
# cqjs eval 测试脚本
cd "$(dirname "$0")"

# 先杀掉可能残留的 cqjs 进程
pkill -9 -f "./cqjs" 2>/dev/null || true
sleep 0.5

# 重新编译
echo "=== Building cqjs ==="
make 2>&1 | tail -5
echo ""

# 测试1: 基本 eval (使用 timeout 命令)
echo "=== Test 1: Basic eval ==="
echo '{"id":"1","type":"eval","code":"1+1"}' | timeout 5 ./cqjs > /tmp/cqjs_stdout.log 2>/tmp/cqjs_stderr.log
EXIT_CODE=$?
echo "Exit code: $EXIT_CODE"
echo "STDOUT:"
cat /tmp/cqjs_stdout.log
echo ""
echo "STDERR:"
cat /tmp/cqjs_stderr.log
echo ""

# 测试2: console.log
echo "=== Test 2: console.log ==="
echo '{"id":"2","type":"eval","code":"console.log(\"hello from cqjs\"); 42"}' | timeout 5 ./cqjs > /tmp/cqjs_stdout2.log 2>/tmp/cqjs_stderr2.log
EXIT_CODE=$?
echo "Exit code: $EXIT_CODE"
echo "STDOUT:"
cat /tmp/cqjs_stdout2.log
echo ""
echo "STDERR:"
cat /tmp/cqjs_stderr2.log
echo ""

# 测试3: 直接运行看是否 crash (无 stdin)
echo "=== Test 3: Crash check ==="
echo "" | timeout 3 ./cqjs > /tmp/cqjs_stdout3.log 2>/tmp/cqjs_stderr3.log
EXIT_CODE=$?
echo "Exit code: $EXIT_CODE"
echo "STDERR (last 20 lines):"
tail -20 /tmp/cqjs_stderr3.log
echo ""

echo "=== All tests done ==="
