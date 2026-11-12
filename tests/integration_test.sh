#!/usr/bin/env bash
# End-to-end tests: transfer files of awkward sizes through heavy in-process
# loss / duplication / reordering (applied on BOTH directions) and check that
# the reassembled file is byte-identical.
#
# usage: tests/integration_test.sh [build-dir]
set -euo pipefail

BUILD=${1:-build}
BUILD=$(cd "$BUILD" && pwd)
WORK=$(mktemp -d)
PORT=$((20000 + RANDOM % 20000))
SERVER_PID=

cleanup() {
    [[ -n $SERVER_PID ]] && kill "$SERVER_PID" 2>/dev/null && wait "$SERVER_PID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT

start_server() {
    [[ -n $SERVER_PID ]] && kill "$SERVER_PID" 2>/dev/null && wait "$SERVER_PID" 2>/dev/null || true
    "$BUILD/udp_server" --port "$PORT" --out-dir "$WORK/out" "$@" 2>"$WORK/server.log" &
    SERVER_PID=$!
    sleep 0.2
}

fail=0
run_case() {  # name size client-args...
    local name=$1 size=$2
    shift 2
    head -c "$size" /dev/urandom >"$WORK/$name"
    if "$BUILD/udp_client" "$WORK/$name" --port "$PORT" --json "$@" >"$WORK/$name.json" 2>"$WORK/$name.err" &&
        cmp -s "$WORK/$name" "$WORK/out/received_$name"; then
        printf '  PASS  %-28s %s\n' "$name" "$(grep -o '"retransmissions":[0-9]*' "$WORK/$name.json")"
    else
        printf '  FAIL  %s\n' "$name"
        cat "$WORK/$name.err"
        fail=1
    fi
}

echo "clean channel"
start_server
run_case empty.bin 0
run_case one_byte.bin 1
run_case exact_chunk.bin 1400
run_case exact_multi.bin $((1400 * 64))
run_case medium.bin 3000000 --window 128

echo "client->server: 20% loss, 10% dup, 20% reorder | server->client: 20% loss, 10% dup"
start_server --loss 0.2 --dup 0.1 --seed 7
run_case lossy_small.bin 5000 --loss 0.2 --dup 0.1 --reorder 0.2 --seed 1
run_case lossy_big.bin 2000000 --loss 0.2 --dup 0.1 --reorder 0.2 --seed 2 --window 32
run_case lossy_window1.bin 100000 --loss 0.2 --window 1 --seed 3
run_case lossy_tiny_chunks.bin 50000 --chunk 10 --loss 0.2 --reorder 0.3 --seed 4

echo "rejects bad requests"
start_server
if "$BUILD/udp_client" /nonexistent --port "$PORT" 2>/dev/null; then
    echo "  FAIL  missing file accepted"; fail=1
else
    echo "  PASS  missing file reported"
fi
kill "$SERVER_PID"; wait "$SERVER_PID" 2>/dev/null || true; SERVER_PID=
head -c 10 /dev/urandom >"$WORK/noserver.bin"
if timeout 20 "$BUILD/udp_client" "$WORK/noserver.bin" --port "$PORT" --max-retries 3 2>/dev/null; then
    echo "  FAIL  transfer to dead server succeeded"; fail=1
else
    echo "  PASS  gives up when server is down"
fi

[[ $fail == 0 ]] && echo "All integration tests passed"
exit $fail
