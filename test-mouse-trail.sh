#!/usr/bin/env bash
# Isolated CLI/config/IPC smoke tests. Never launch the overlay daemon.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="${1:-$SCRIPT_DIR/mouse-trail}"
for tool in python3 strace timeout; do
    command -v "$tool" >/dev/null || { echo "Missing test dependency: $tool" >&2; exit 1; }
done
[ -x "$BINARY" ] || { echo "Executable not found: $BINARY (pass its path as argument)" >&2; exit 1; }

# Keep all paths (including the fake control sockets) in our own directory.
WORK="$(mktemp -d "${XDG_RUNTIME_DIR:-${TMPDIR:-/tmp}}/mouse-trail-test.XXXXXXXX")"
SERVER_PID=
cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    /run/current-system/sw/bin/remove-without-permission -rf -- "$WORK"
}
trap cleanup EXIT

pass() { printf 'PASS: %s\n' "$1"; }
fail() { printf 'FAIL: %s\n' "$1" >&2; exit 1; }

"$BINARY" --help >"$WORK/help" 2>&1 || fail '--help exit status'
grep -q 'Usage:' "$WORK/help" && grep -q -- '--socket PATH' "$WORK/help" || fail '--help CLI options'
pass 'help and CLI options'
if "$BINARY" --not-a-real-option >"$WORK/invalid" 2>&1; then
    fail 'unknown option should fail'
fi
pass 'unknown option returns failure'

# --ctl does not start a daemon and must not fall back to the real runtime socket.
if timeout 5 "$BINARY" --socket "$WORK/missing.sock" --ctl show >"$WORK/missing.out" 2>&1; then
    fail 'missing isolated socket should fail'
else
    status=$?
    [ "$status" -ne 124 ] || fail 'missing-socket IPC timed out'
fi
pass 'missing isolated socket returns failure'

# Exercise the client against our own one-request UNIX server, never the user's service.
start_server() {
    local reply=$1 sock=$2
    python3 - "$sock" "$reply" "$WORK/ready" "$WORK/message" <<'PY' &
import socket
import sys

path, reply, ready, message = sys.argv[1:]
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
    server.bind(path)
    server.listen(1)
    open(ready, 'w').close()
    server.settimeout(5)
    client, _ = server.accept()
    with client:
        client.settimeout(5)
        data = client.recv(4096)
        with open(message, 'wb') as out:
            out.write(data)
        client.sendall((reply + '\n').encode())
PY
    SERVER_PID=$!
    for ((i=0; i<100; i++)); do
        [ -f "$WORK/ready" ] && return 0
        kill -0 "$SERVER_PID" 2>/dev/null || fail 'fake IPC server exited'
        sleep 0.05
    done
    fail 'fake IPC server did not become ready'
}

finish_server() {
    wait "$SERVER_PID" || fail 'fake IPC server failed'
    SERVER_PID=
    [ "$(<"$WORK/message")" = 'color 123abc' ] || fail 'IPC command bytes'
    /run/current-system/sw/bin/remove-without-permission -f -- "$WORK/ready" "$WORK/message"
}

start_server OK "$WORK/ok.sock"
timeout 5 "$BINARY" --socket "$WORK/ok.sock" --ctl 'color 123abc' >"$WORK/ok.out" 2>&1 || fail 'OK reply should succeed'
finish_server
pass 'IPC sends command and accepts OK'

start_server ERR "$WORK/err.sock"
if timeout 5 "$BINARY" --socket "$WORK/err.sock" --ctl 'color 123abc' >"$WORK/err.out" 2>&1; then
    fail 'ERR reply should fail'
else
    status=$?
    [ "$status" -ne 124 ] || fail 'ERR reply IPC timed out'
fi
finish_server
pass 'IPC rejects ERR'

# main.c parses import=... then restores --device from CLI. Trace only file
# syscalls with a deliberately nonexistent Wayland socket: even if this host
# has readable input devices, the compositor cannot be touched.
mkdir -p "$WORK/home/.config/mouse-trail"
printf 'device=%s\n' "$WORK/import-device" > "$WORK/import.conf"
printf 'import=%s\ndevice=%s\n' "$WORK/import.conf" "$WORK/config-device" > "$WORK/home/.config/mouse-trail/config"
trace_start() {
    local trace=$1; shift
    local status=0
    timeout 10 strace -qq -e trace=file -o "$trace" \
        env HOME="$WORK/home" WAYLAND_DISPLAY="mouse-trail-test-nonexistent-$$" \
        "$BINARY" --socket "$WORK/no-daemon.sock" "$@" \
        >"$WORK/run.out" 2>&1 || status=$?
    [ "$status" -ne 124 ] || fail 'isolated config test timed out'
    # Returning nonzero is expected without a real compositor / mouse.
}

trace_start "$WORK/config.trace"
grep -Fq "$WORK/import.conf" "$WORK/config.trace" || fail 'config import not read'
grep -Fq "$WORK/config-device" "$WORK/config.trace" || fail 'config device not applied'
pass 'default config and imported file are parsed'

trace_start "$WORK/cli.trace" --config "$WORK/home/.config/mouse-trail/config" --device "$WORK/cli-device"
grep -Fq "$WORK/cli-device" "$WORK/cli.trace" || fail '--device not applied'
if grep -Fq "$WORK/config-device" "$WORK/cli.trace"; then
    fail 'config device incorrectly overrides CLI device'
fi
pass 'CLI device overrides config device'

echo 'All isolated mouse-trail tests passed.'
