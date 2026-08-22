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
