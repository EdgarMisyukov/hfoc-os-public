#!/usr/bin/env bash
# Prove item 237 in one q35 QEMU boot: lwIP ARP resolution/cache, ICMP echo,
# malformed rejection, rate limiting, timer liveness, and clean HMP shutdown.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in python3 qemu-system-x86_64 timeout mktemp; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

if [[ "${HFOC_ARP_ICMP_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_ARP_ICMP_TEST_TIMEOUT:-90}s" \
        env HFOC_ARP_ICMP_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "ARP/ICMP test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "ARP/ICMP test must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "ARP/ICMP test must not set QEMU_CPU" >&2
    exit 1
}
echo "QEMU ARP/ICMP baseline: machine=q35 smp=1 cpu=default"

TEST_DIR=$(mktemp -d)
SERIAL_LOG=$TEST_DIR/com1.log
MONITOR_LOG=$TEST_DIR/monitor.log
QEMU_LOG=$TEST_DIR/qemu.log
INTERRUPTS_LOG=$TEST_DIR/interrupts.log
FAILURE_REASON=$TEST_DIR/failure-reason.txt
MONITOR_SOCKET=$TEST_DIR/monitor.sock
QEMU_PID=
FAILURE_MESSAGE=

: >"$SERIAL_LOG"
: >"$MONITOR_LOG"
: >"$QEMU_LOG"
: >"$INTERRUPTS_LOG"
rm -f "$MONITOR_SOCKET"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/arp-icmp-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-ARP/ICMP test failed}" \
        >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$INTERRUPTS_LOG" \
        "$FAILURE_REASON" "$artifact_dir/" 2>/dev/null || true
    echo "ARP/ICMP artifacts: $artifact_dir" >&2
}

QEMU_WAIT_RESULT=

wait_for_exit() {
    local deadline=$((SECONDS + 5))
    QEMU_WAIT_RESULT=0
    while kill -0 "$QEMU_PID" 2>/dev/null; do
        if (( SECONDS >= deadline )); then
            QEMU_WAIT_RESULT=timeout
            return 1
        fi
        sleep 0.1
    done
    wait "$QEMU_PID" 2>/dev/null
    QEMU_WAIT_RESULT=$?
    return "$QEMU_WAIT_RESULT"
}

send_monitor_quit() {
    python3 - "$MONITOR_SOCKET" <<'PY'
import socket
import sys
import time

monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    monitor.settimeout(1.0)
    try:
        monitor.connect(sys.argv[1])
    except OSError as exc:
        raise SystemExit(f"QEMU monitor did not accept quit: {exc}")
    deadline = time.monotonic() + 5.0
    prompt = b""
    while b"(qemu)" not in prompt:
        remaining = deadline - time.monotonic()
        if remaining <= 0.0:
            raise SystemExit("QEMU HMP prompt timeout")
        monitor.settimeout(remaining)
        try:
            chunk = monitor.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            raise SystemExit("QEMU HMP closed before prompt")
        prompt += chunk
    monitor.sendall(b"quit\n")
    monitor.settimeout(5.0)
    while True:
        try:
            chunk = monitor.recv(4096)
        except socket.timeout:
            raise SystemExit("QEMU monitor EOF timeout after quit")
        if not chunk:
            break
finally:
    monitor.close()
PY
}

successful_shutdown() {
    local quit_error
    if ! quit_error=$(send_monitor_quit 2>&1); then
        FAILURE_MESSAGE="${quit_error:-QEMU monitor quit failed}"
        return 1
    fi
    if ! wait_for_exit; then
        if [[ "$QEMU_WAIT_RESULT" == timeout ]]; then
            FAILURE_MESSAGE="QEMU did not exit after monitor quit"
        else
            FAILURE_MESSAGE="QEMU exited with status $QEMU_WAIT_RESULT after monitor quit"
        fi
        return 1
    fi
    return 0
}

best_effort_cleanup() {
    if [[ -z "$QEMU_PID" ]]; then
        return 0
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        wait "$QEMU_PID" 2>/dev/null || true
        QEMU_PID=
        return 0
    fi
    kill -TERM "$QEMU_PID" 2>/dev/null || true
    local deadline=$((SECONDS + 2))
    while kill -0 "$QEMU_PID" 2>/dev/null; do
        if (( SECONDS >= deadline )); then
            break
        fi
        sleep 0.1
    done
    if kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -KILL "$QEMU_PID" 2>/dev/null || true
    fi
    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    if (( status != 0 )); then
        best_effort_cleanup
        save_failure_artifacts
    fi
    rm -rf "$TEST_DIR"
    exit "$status"
}

trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" \
    -display none -cdrom "$IMAGE" \
    -serial "file:$SERIAL_LOG" \
    -monitor "unix:$MONITOR_SOCKET,server,nowait" \
    -d int -D "$INTERRUPTS_LOG" -no-reboot -no-shutdown \
    >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

validation_output=
if ! validation_output=$(python3 - "$SERIAL_LOG" "$QEMU_PID" 2>&1 <<'PY'
import pathlib
import sys
import time

serial_path = pathlib.Path(sys.argv[1])
qemu_pid = int(sys.argv[2])
timer_marker = "[LAPIC-TIMER] irq vector=0x42"

def serial_text():
    try:
        return serial_path.read_bytes().decode("ascii", errors="replace")
    except FileNotFoundError:
        return ""

deadline = time.monotonic() + 45.0
while time.monotonic() < deadline:
    text = serial_text()
    if "[ARP-ICMP] proof init" in text:
        break
    if "[ARP-ICMP] failed" in text:
        raise SystemExit("ARP/ICMP proof reported failure before proof OK")
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic before ARP/ICMP proof")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="ARP/ICMP proof init" '
        'deadline=45s\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for ARP/ICMP proof init")

deadline = time.monotonic() + 45.0
while time.monotonic() < deadline:
    text = serial_text()
    if "[ARP-ICMP] proof OK" in text:
        break
    if "[ARP-ICMP] failed" in text:
        raise SystemExit("ARP/ICMP proof reported failure")
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic during ARP/ICMP proof")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="ARP/ICMP proof OK" '
        'deadline=45s\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for ARP/ICMP proof OK")

text = serial_text()
required = [
    "[ARP-ICMP] proof init",
    "[ARP-ICMP] arp ok",
    "[ARP-ICMP] icmp ok",
    "[ARP-ICMP] rate limit ok",
    "[ARP-ICMP] proof OK",
]
for marker in required:
    if text.count(marker) != 1:
        raise SystemExit(
            f"Expected 1 marker, got {text.count(marker)}: {marker}")
if "[ARP-ICMP] failed" in text:
    raise SystemExit("ARP/ICMP proof reported failure")
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("Kernel panic detected")

timer_deadline = time.monotonic() + 10.0
while time.monotonic() < timer_deadline:
    if timer_marker in serial_text():
        break
    if "*** HFOC KERNEL PANIC ***" in serial_text():
        raise SystemExit("Kernel panic during timer liveness")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="LAPIC timer liveness" '
        'deadline=' + str(int(timer_deadline)) + 's\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for LAPIC timer liveness")

try:
    import os
    os.kill(qemu_pid, 0)
except OSError:
    raise SystemExit("QEMU exited before validation completed")

print("ARP/ICMP proof init: found")
print("ARP resolution and cache: found")
print("ICMP echo: found")
print("Malformed and rate limiting: found")
print("Proof OK: found")
print("Timer liveness: found")
print("Kernel panic: none")
print("QEMU alive before quit: yes")
PY
); then
    FAILURE_MESSAGE=$validation_output
    printf '%s\n' "$validation_output" >&2
    exit 1
fi

printf '%s\n' "$validation_output"
if ! kill -0 "$QEMU_PID" 2>/dev/null; then
    FAILURE_MESSAGE="QEMU exited before validation completed"
    printf '%s\n' "$FAILURE_MESSAGE" >&2
    exit 1
fi

if successful_shutdown; then
    QEMU_PID=
else
    printf '%s\n' "$FAILURE_MESSAGE" >&2
    exit 1
fi

echo "QEMU ARP/ICMP test: OK"
