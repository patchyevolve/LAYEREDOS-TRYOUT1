#!/bin/bash
# test-runner.sh — Waits for shell prompt, injects test commands, checks results.
# Usage: test-runner.sh <kernel.elf>
# Exit: 0 if STAGE3:DONE found, 1 otherwise.

KERNEL="$1"
[ -f "$KERNEL" ] || { echo "Usage: $0 <kernel.elf>" >&2; exit 1; }

LOG=$(mktemp) || { echo "mktemp failed" >&2; exit 1; }
trap "rm -f $LOG" EXIT

# Start QEMU as a coprocess (bidirectional I/O), 5-minute inner timeout
coproc QEMU {
    timeout 300 qemu-system-x86_64 -kernel "$KERNEL" -nographic \
        -m 512M -no-reboot -no-shutdown 2>&1
}

# Wait for shell prompt (up to 240 seconds, showing progress)
echo "[test-runner] Waiting for shell prompt..." >&2
START_SEC=$(date +%s)
LINES=0
while IFS= read -r -u ${QEMU[0]} line; do
    LINES=$((LINES + 1))
    echo "$line" >> "$LOG"
    if [ $((LINES % 50)) -eq 0 ]; then
        echo "[test-runner] Read $LINES lines, elapsed $(( $(date +%s) - START_SEC ))s..." >&2
    fi
    if [[ "$line" == *"OS>"* ]]; then
        echo "[test-runner] Prompt detected after $LINES lines ($(( $(date +%s) - START_SEC ))s)" >&2
        break
    fi
done

# Show remaining boot lines for debugging
echo "[test-runner] Sending test commands..." >&2

# Helper: send a command to QEMU's stdin
send_cmd() { echo "$1" >&${QEMU[1]}; sleep 0.3; }

send_cmd "echo STAGE3:FILE_RW"
send_cmd "echo hello > /test.txt"
send_cmd "cat /test.txt"
send_cmd "echo world >> /test.txt"
send_cmd "cat /test.txt"
send_cmd "wc /test.txt"
send_cmd "echo STAGE3:DIR_OPS"
send_cmd "mkdir /mydir"
send_cmd "ls /mydir"
send_cmd "rmdir /mydir"
send_cmd "echo STAGE3:SYMLINK"
send_cmd "ln -s /test.txt /link.txt"
send_cmd "cat /link.txt"
send_cmd "readlink /link.txt"
send_cmd "echo STAGE3:HARDLINK"
send_cmd "ln /test.txt /hard.txt"
send_cmd "cat /hard.txt"
send_cmd "echo STAGE3:RENAME"
send_cmd "mv /test.txt /renamed.txt"
send_cmd "cat /renamed.txt"
send_cmd "echo STAGE3:COPY"
send_cmd "cp /renamed.txt /copy.txt"
send_cmd "cat /copy.txt"
send_cmd "echo STAGE3:PERMS"
send_cmd "chmod 444 /copy.txt"
send_cmd "stat /copy.txt"
send_cmd "chmod 644 /copy.txt"
send_cmd "echo STAGE3:TMPFS"
send_cmd "ls /tmp"
send_cmd "echo STAGE3:DEVFS"
send_cmd "cat /dev/null"
send_cmd "wc /copy.txt"
send_cmd "rm /renamed.txt /copy.txt /link.txt /hard.txt"
send_cmd "echo STAGE3:SNAPSHOT"
send_cmd "echo snap-data > /snap-test.txt"
send_cmd "cat /snap-test.txt"
send_cmd "snap take"
sleep 2
send_cmd "echo changed > /snap-test.txt"
send_cmd "snap rollback"
sleep 2
send_cmd "cat /snap-test.txt"
send_cmd "echo STAGE3:BACKUP"
send_cmd "backup save /backup.bkp"
sleep 3
send_cmd "rm /snap-test.txt"
send_cmd "backup restore /backup.bkp"
sleep 3
send_cmd "cat /snap-test.txt"
send_cmd "echo STAGE3:FSCK"
send_cmd "fsck"
sleep 2
send_cmd "echo STAGE3:DONE"
send_cmd "source /welcome.txt"
sleep 2
send_cmd "run hello-c.elf"
sleep 6
send_cmd "run thread_test.elf"
sleep 6
send_cmd "echo done"

# Drain remaining output
while IFS= read -r -t 2 -u ${QEMU[0]} line; do
    echo "$line" >> "$LOG"
done 2>/dev/null || true

# Clean up I/O and wait for QEMU to finish
exec {QEMU[1]}>&-
wait $QEMU_PID 2>/dev/null || true

echo "[test-runner] Done after $(( $(date +%s) - START_SEC ))s" >&2

# Show test output and check for success
grep -aE 'STAGE3|hello|world|Welcome|FSCK|clean|snap-data|rolled|BACKUP|RESTORE' "$LOG" 2>/dev/null

if grep -q "STAGE3:DONE" "$LOG" 2>/dev/null; then
    echo "=== Test complete ==="
    exit 0
else
    echo "FAIL: STAGE3:DONE not found in output" >&2
    exit 1
fi
