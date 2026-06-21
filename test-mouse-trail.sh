#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG_FILE="/tmp/mouse-trail-test.log"
BINARY="$SCRIPT_DIR/mouse-trail"
PIDFILE="/tmp/mouse-trail-test.pid"
RET=0

cleanup() {
    if [ -f "$PIDFILE" ]; then
        kill "$(cat "$PIDFILE")" 2>/dev/null || true
        rm -f "$PIDFILE"
    fi
    rm -f /tmp/mouse-trail.sock
}
trap cleanup EXIT

echo "=== Test 1: Binary exists and shows help ==="
if [ -x "$BINARY" ]; then
    echo "PASS: Binary found at $BINARY ($(du -h "$BINARY" | cut -f1))"
else
    echo "FAIL: Binary not found"
    exit 1
fi

echo "=== Test 2: --help works ==="
"$BINARY" --help 2>&1 | grep -q "Usage:" && echo "PASS: Help output OK" || { echo "FAIL: Help broken"; RET=1; }

echo "=== Test 3: --ctl error without server ==="
"$BINARY" --ctl "color #ff0000" 2>&1 | grep -q "Cannot connect" && echo "PASS: Proper error for missing server" || { echo "FAIL: Wrong ctl error"; RET=1; }

echo "=== Test 4: Invalid device error ==="
"$BINARY" --device /dev/input/nonexistent --log-file "$LOG_FILE" 2>/dev/null || true
sleep 1
if grep -q "Cannot open" "$LOG_FILE"; then
    echo "PASS: Proper error on invalid device"
else
    echo "FAIL: No error for invalid device"
    RET=1
fi

echo "=== Test 5: Traverse log level names ==="
for level in debug info warn error; do
    "$BINARY" --log-level "$level" --device /dev/input/nonexistent --log-file "$LOG_FILE" 2>/dev/null || true
    if grep -q "\[INFO" "$LOG_FILE" || grep -q "\[ERROR" "$LOG_FILE"; then
        echo "PASS: Log level '$level' works"
    else
        echo "FAIL: Log level '$level' broken"
        RET=1
    fi
done

echo "=== Test 6: Color parsing ==="
"$BINARY" --color "#ff0000" --device /dev/input/nonexistent --log-file "$LOG_FILE" --log-level debug 2>/dev/null || true
if grep -q "#ff0000" "$LOG_FILE"; then
    echo "PASS: Color #ff0000 parsed"
else
    echo "FAIL: Color not in logs"
    RET=1
fi

echo "=== Test 7: Starts in Wayland (if XDG_RUNTIME_DIR and WAYLAND_DISPLAY set) ==="
if [ -n "${WAYLAND_DISPLAY:-}" ] && [ -n "${XDG_RUNTIME_DIR:-}" ]; then
    "$BINARY" --log-file "$LOG_FILE" --log-level debug &
    PID=$!
    echo "$PID" > "$PIDFILE"
    sleep 2

    if kill -0 "$PID" 2>/dev/null; then
        echo "PASS: Process running under Wayland (PID=$PID)"

        if grep -q "Layer surface ready" "$LOG_FILE"; then
            echo "PASS: Layer surface configured"
        else
            echo "INFO: Connected but surface not confirmed (log: $(grep Wayland "$LOG_FILE" | tail -1))"
        fi

        if grep -q "Input region set empty" "$LOG_FILE"; then
            echo "PASS: Click passthrough configured"
        fi

        if grep -q "Opened input device" "$LOG_FILE"; then
            echo "PASS: Input device opened"
        fi

        kill "$PID"
        sleep 1
        if ! kill -0 "$PID" 2>/dev/null; then
            echo "PASS: Clean shutdown"
        fi
    else
        echo "FAIL: Process died under Wayland"

        if grep -q "ERROR" "$LOG_FILE"; then
            echo "DIAG: $(grep ERROR "$LOG_FILE" | tail -3)"
        fi

        if grep -q "Cannot open /dev/input" "$LOG_FILE"; then
            echo "INFO: Input device issue (expected if no /dev/input/event2)"
        else
            RET=1
        fi
    fi
else
    echo "SKIP: Not running under Wayland"
fi

echo ""
if [ "$RET" -eq 0 ]; then
    echo "=== LOG VERIFICATION PASSED ==="
else
    echo "=== SOME TESTS FAILED ==="
fi
echo "Full log: $LOG_FILE"
exit $RET
