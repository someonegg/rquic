#!/usr/bin/env sh
set -eu

server_bin=$1
client_bin=$2

tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/rqc-e2e.XXXXXX")
server_log="$tmp_dir/server.log"
client_log="$tmp_dir/client.log"
file_out="$tmp_dir/file.out"
www_root="$tmp_dir/www"
server_pid=

cleanup() {
    status=$?
    if [ "$status" -ne 0 ]; then
        echo "demo e2e failed; server log:" >&2
        cat "$server_log" >&2 2>/dev/null || true
        echo "demo e2e failed; client log:" >&2
        cat "$client_log" >&2 2>/dev/null || true
    fi
    if [ "${server_pid:-}" ]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -rf "$tmp_dir"
}
trap cleanup EXIT INT TERM

mkdir -p "$www_root"
printf 'rquic file payload\nline 2\n' > "$www_root/payload.txt"
dd if=/dev/zero bs=16384 count=3 >> "$www_root/payload.txt" 2>/dev/null
: > "$www_root/empty.txt"

start_server() {
    port=$1
    www_arg=${2:-}
    : >"$server_log"
    if [ -n "$www_arg" ]; then
        "$server_bin" --host 127.0.0.1 --port "$port" --www-root "$www_arg" >"$server_log" 2>&1 &
    else
        "$server_bin" --host 127.0.0.1 --port "$port" >"$server_log" 2>&1 &
    fi

    server_pid=$!
    i=0
    while [ "$i" -lt 50 ]; do
        if grep -q "demo_server listening" "$server_log"; then
            break
        fi
        if ! kill -0 "$server_pid" 2>/dev/null; then
            echo "demo_server exited before listening" >&2
            cat "$server_log" >&2
            exit 1
        fi
        i=$((i + 1))
        sleep 0.1
    done

    if ! grep -q "demo_server listening" "$server_log"; then
        echo "demo_server did not become ready before timeout" >&2
        cat "$server_log" >&2
        exit 1
    fi

    if ! kill -0 "$server_pid" 2>/dev/null; then
        echo "demo_server exited before client run" >&2
        cat "$server_log" >&2
        exit 1
    fi
}

stop_server() {
    if [ "${server_pid:-}" ]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
        server_pid=
    fi
}

port=$((20000 + ($$ % 20000)))
start_server "$port"
"$client_bin" --host 127.0.0.1 --port "$port" --path /hello >"$client_log" 2>&1
grep -q "rquic demo response" "$client_log"
grep -q "resource: /hello" "$client_log"
grep -q "OK bytes=" "$client_log"
stop_server

port=$((port + 1))
start_server "$port" "$www_root"
"$client_bin" --host 127.0.0.1 --port "$port" --path /payload.txt --output "$file_out" >"$client_log" 2>&1
grep -q "OK bytes=" "$client_log"
cmp "$www_root/payload.txt" "$file_out"
stop_server

port=$((port + 1))
start_server "$port" "$www_root"
"$client_bin" --host 127.0.0.1 --port "$port" --path /empty.txt --output "$file_out" >"$client_log" 2>&1
grep -q "OK bytes=" "$client_log"
cmp "$www_root/empty.txt" "$file_out"

stop_server
port=$((port + 1))
start_server "$port"
"$client_bin" --host 127.0.0.1 --port "$port" --datagram 'hello datagram' >"$client_log" 2>&1
grep -qx 'OK datagram bytes=14' "$client_log"
"$client_bin" --host 127.0.0.1 --port "$port" --datagram '' >"$client_log" 2>&1
grep -qx 'OK datagram bytes=0' "$client_log"

expect_failure() {
    if "$client_bin" "$@" >"$client_log" 2>&1; then
        echo "expected client failure: $*" >&2
        exit 1
    fi
}

large_text=$(awk 'BEGIN { for (i = 0; i < 2000; i++) printf "x" }')
expect_failure --host 127.0.0.1 --port "$port" --datagram "$large_text"
grep -q 'datagram too large: bytes=2000 MSS=' "$client_log"
expect_failure --datagram x --path /hello
grep -q 'conflicts' "$client_log"
expect_failure --path /hello --datagram x
grep -q 'conflicts' "$client_log"
expect_failure --datagram x --output "$file_out"
grep -q 'conflicts' "$client_log"
expect_failure --output "$file_out" --datagram x
grep -q 'conflicts' "$client_log"
expect_failure --datagram '' --datagram x
grep -q 'duplicate --datagram' "$client_log"
stop_server

# Bind a real UDP socket without responding, so there is no ICMP shortcut.
# The outer watchdog bounds hangs independently of the demo application timer.
python3 - "$client_bin" "$port" "$client_log" <<'PY'
import socket
import subprocess
import sys
import time
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
    sock.bind(('127.0.0.1', int(sys.argv[2])))
    start = time.monotonic()
    with open(sys.argv[3], 'w') as log:
        result = subprocess.run([sys.argv[1], '--host', '127.0.0.1',
                                 '--port', sys.argv[2], '--datagram', 'timeout'],
                                stdout=log, stderr=log, timeout=8)
    elapsed = time.monotonic() - start
    assert result.returncode != 0, 'client unexpectedly succeeded'
    assert elapsed < 6.5, f'client exceeded application deadline: {elapsed}'
with open(sys.argv[3]) as log:
    assert 'datagram timeout after 5 seconds' in log.read()
PY
