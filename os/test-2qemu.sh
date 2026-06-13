#!/bin/bash
# Two-QEMU IPv6 TCP + UDP echo validation
# Usage: make test-net-2qemu
#   or:  ./test-2qemu.sh [--foreground]

set -euo pipefail

KERNEL="build/kernel.elf"
TIMEOUT=180
TMPDIR=$(mktemp -d /tmp/qemu-2qemu-XXXXXX)
LISTENER_LOG="$TMPDIR/listener.log"
CONNECTOR_LOG="$TMPDIR/connector.log"
LISTENER_PID=""
CONNECTOR_PID=""
PASS=1

cleanup() {
    set +e
    [ -n "$LISTENER_PID" ] && kill "$LISTENER_PID" 2>/dev/null
    [ -n "$CONNECTOR_PID" ] && kill "$CONNECTOR_PID" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$TMPDIR"
}

trap cleanup EXIT INT TERM

# Find QEMU
QEMU=${QEMU:-$(which qemu-system-x86_64 2>/dev/null || echo "/usr/bin/qemu-system-x86_64")}
QEMU=$(command -v "$QEMU" 2>/dev/null || echo "$QEMU")
if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "FAIL: qemu-system-x86_64 not found"; exit 1
fi

# Build if needed
if [ ! -f "$KERNEL" ]; then
    echo "Building kernel..."
    make -C os -j4 all >/dev/null 2>&1
fi

echo "=== Two-QEMU TCP+UDP Echo Validation ==="
echo "Listener: default MAC (52:54:00:12:34:56)"
echo "Connector: alt  MAC (52:54:00:12:34:57)"
echo ""

# Start both QEMU instances simultaneously (side-by-side boot)
"$QEMU" -kernel "$KERNEL" -serial mon:stdio -m 512M \
    -no-reboot -no-shutdown \
    -netdev socket,id=n1,listen=:12345 -device e1000,netdev=n1 \
    > "$LISTENER_LOG" 2>&1 &
LISTENER_PID=$!
echo "Listener QEMU started (pid=$LISTENER_PID), listening on :12345"

"$QEMU" -kernel "$KERNEL" -serial mon:stdio -m 512M \
    -no-reboot -no-shutdown \
    -netdev socket,id=n1,connect=127.0.0.1:12345 \
    -device e1000,netdev=n1,mac=52:54:00:12:34:57 \
    > "$CONNECTOR_LOG" 2>&1 &
CONNECTOR_PID=$!
echo "Connector QEMU started (pid=$CONNECTOR_PID)"
echo ""

# Wait for connector to complete
CONNECTOR_DONE=0
for i in $(seq 1 $TIMEOUT); do
    if grep -qE '\[UDP\] PASS|\[UDP\] FAIL|\[NET\] .* exited|\[BOOT\] Starting shell' "$CONNECTOR_LOG" 2>/dev/null; then
        CONNECTOR_DONE=1
        break
    fi
    if grep -q "PAGE_FAULT\|DOUBLE-ALLOCATED\|KERNEL PANIC\|triple fault\|pmm_alloc_page: no free" "$CONNECTOR_LOG" 2>/dev/null; then
        echo "FAIL: Connector crashed!"
        PASS=0
        break
    fi
    sleep 1
done

if [ "$CONNECTOR_DONE" -ne 1 ] && [ "$PASS" -eq 1 ]; then
    echo "FAIL: Connector did not complete within ${TIMEOUT}s"
    PASS=0
fi

echo ""
echo "=== Results ==="

TCP_PASS=0
UDP_PASS=0

if grep -q '\[TCP\] PASS' "$CONNECTOR_LOG" 2>/dev/null; then
    TCP_PASS=1
    echo "  TCP echo: PASS"
else
    echo "  TCP echo: FAIL (check log)"
fi

if grep -q '\[UDP\] PASS' "$CONNECTOR_LOG" 2>/dev/null; then
    UDP_PASS=1
    echo "  UDP echo: PASS"
else
    echo "  UDP echo: FAIL (check log)"
fi

echo ""
if [ "$TCP_PASS" -eq 1 ] && [ "$UDP_PASS" -eq 1 ] && [ "$PASS" -eq 1 ]; then
    echo "OVERALL: PASS"
else
    echo "OVERALL: FAIL"
fi

# Print full logs
echo ""
echo "========== CONNECTOR LOG (FULL) =========="
cat "$CONNECTOR_LOG"
echo ""
echo "========== LISTENER LOG (FULL) =========="
cat "$LISTENER_LOG"

exit $(( ! (TCP_PASS && UDP_PASS && PASS) ))
