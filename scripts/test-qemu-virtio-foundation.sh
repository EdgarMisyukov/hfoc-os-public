#!/usr/bin/env bash
# Prove the item 201-209 virtio foundation composition in one q35 QEMU boot:
# discovery, feature rejection, queue publication/completion, injected reset,
# exact rollback, DMA teardown, timer liveness, keyboard responsiveness, and
# clean shutdown.
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

if [[ "${HFOC_VIRTIO_FOUNDATION_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_VIRTIO_FOUNDATION_TEST_TIMEOUT:-90}s" \
        env HFOC_VIRTIO_FOUNDATION_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Virtio-foundation test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "Virtio-foundation test must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "Virtio-foundation test must not set QEMU_CPU" >&2
    exit 1
}
echo "QEMU virtio-foundation baseline: machine=q35 smp=1 cpu=default"

TEST_DIR=$(mktemp -d)
SERIAL_LOG=$TEST_DIR/com1.log
MONITOR_LOG=$TEST_DIR/monitor.log
QEMU_LOG=$TEST_DIR/qemu.log
INTERRUPTS_LOG=$TEST_DIR/interrupts.log
METADATA=$TEST_DIR/metadata.txt
FAILURE_REASON=$TEST_DIR/failure-reason.txt
MONITOR_SOCKET=$TEST_DIR/monitor.sock
QEMU_PID=
FAILURE_MESSAGE=
QEMU_WAIT_RESULT=

: >"$SERIAL_LOG"
: >"$MONITOR_LOG"
: >"$QEMU_LOG"
: >"$INTERRUPTS_LOG"
rm -f "$MONITOR_SOCKET"

printf '%s\n' \
    "machine=$QEMU_MACHINE" \
    "cpu=default" \
    "smp=$QEMU_SMP" \
    "image=$IMAGE" >"$METADATA"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/virtio-foundation-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-Virtio-foundation test failed}" \
        >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$INTERRUPTS_LOG" \
        "$METADATA" "$FAILURE_REASON" "$artifact_dir/"
    echo "Virtio-foundation artifacts: $artifact_dir" >&2
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

def fail(message):
    print(message, file=sys.stderr)
    raise SystemExit(1)

monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    monitor.settimeout(1.0)
    try:
        monitor.connect(sys.argv[1])
    except OSError as exc:
        fail(f"QEMU monitor did not accept quit: {exc}")
    prompt_deadline = time.monotonic() + 5.0
    prompt = b""
    while b"(qemu)" not in prompt:
        remaining = prompt_deadline - time.monotonic()
        if remaining <= 0.0:
            fail("QEMU HMP prompt timeout")
        monitor.settimeout(remaining)
        try:
            chunk = monitor.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            fail("QEMU HMP closed before prompt")
        prompt += chunk
    try:
        monitor.sendall(b"quit\n")
    except OSError as exc:
        fail(f"QEMU monitor did not accept quit: {exc}")
    monitor.settimeout(5.0)
    while True:
        try:
            chunk = monitor.recv(4096)
        except socket.timeout:
            fail("QEMU monitor EOF timeout after quit")
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
if ! validation_output=$(python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" \
    "$QEMU_PID" 2>&1 <<'PY'
import os
import pathlib
import socket
import sys
import time

serial_path = pathlib.Path(sys.argv[1])
monitor_path = pathlib.Path(sys.argv[2])
qemu_pid = int(sys.argv[3])
command_marker = "UNKNOWN COMMAND: vfoc. TYPE HELP."
timer_marker = "[LAPIC-TIMER] irq vector=0x42"


def serial_text():
    try:
        return serial_path.read_bytes().decode("ascii", errors="replace")
    except FileNotFoundError:
        return ""


def qemu_running():
    try:
        os.kill(qemu_pid, 0)
        return True
    except ProcessLookupError:
        return False


def wait_prompt(monitor):
    chunks = []
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        try:
            chunk = monitor.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            raise SystemExit("QEMU HMP closed before prompt")
        chunks.append(chunk)
        if b"(qemu)" in b"".join(chunks):
            return
    raise SystemExit("QEMU HMP prompt timeout")


deadline = time.monotonic() + 45.0
while time.monotonic() < deadline:
    text = serial_text()
    if "[VIRTIO-FOUNDATION] proof OK" in text:
        break
    if "[VIRTIO-FOUNDATION] failed" in text:
        raise SystemExit("Virtio-foundation proof reported failure")
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic before virtio-foundation proof")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="virtio-foundation proof OK" '
        'deadline=45s\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for virtio-foundation proof OK")

required = [
    "[VIRTIO-FOUNDATION] proof init",
    "[VIRTIO-FOUNDATION] discovery published",
    "[VIRTIO-FOUNDATION] feature failure rejected",
    "[VIRTIO-FOUNDATION] queue publication ok",
    "[VIRTIO-FOUNDATION] queue completion ok",
    "[VIRTIO-FOUNDATION] injected reset ok",
    "[VIRTIO-FOUNDATION] exact rollback ok",
    "[VIRTIO-FOUNDATION] DMA teardown ok",
    "[VIRTIO-FOUNDATION] clean shutdown",
    "[VIRTIO-FOUNDATION] proof OK",
]
text = serial_text()
for marker in required:
    if text.count(marker) != 1:
        raise SystemExit(
            f"Expected 1 marker, got {text.count(marker)}: {marker}")
if "[VIRTIO-FOUNDATION] failed" in text:
    raise SystemExit("Virtio-foundation proof reported failure")
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

monitor_deadline = time.monotonic() + 5.0
while not monitor_path.exists():
    if time.monotonic() >= monitor_deadline:
        raise SystemExit("timed out waiting for QEMU monitor socket")
    time.sleep(0.05)

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
    monitor.settimeout(1.0)
    try:
        monitor.connect(str(monitor_path))
    except OSError as exc:
        raise SystemExit(f"QEMU monitor did not accept connection: {exc}")

    wait_prompt(monitor)
    for key in ("v", "f", "o", "c", "ret"):
        monitor.sendall(f"sendkey {key}\n".encode("ascii"))
        wait_prompt(monitor)
        time.sleep(0.08)

command_deadline = time.monotonic() + 10.0
while time.monotonic() < command_deadline:
    text = serial_text()
    if text.count(command_marker) == 1:
        marker_index = text.rfind(command_marker)
        if text.find("hfoc>", marker_index) > marker_index:
            break
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic during keyboard responsiveness")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="one vfoc command result" '
        'deadline=' + str(int(command_deadline)) + 's\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for one vfoc command result")

text = serial_text()
if text.count(command_marker) != 1:
    raise SystemExit(f"Expected one vfoc result, got {text.count(command_marker)}")
if timer_marker not in text:
    raise SystemExit("Timer liveness evidence missing after keyboard input")
if not qemu_running():
    raise SystemExit("QEMU exited before monitor quit")

print("Virtio-foundation proof init: found")
print("Virtio-foundation proof OK: found")
print("Discovery/feature/queue/reset/rollback/DMA markers: 8")
print("Timer liveness: yes")
print("Keyboard command result: vfoc")
print("Kernel panic: none")
print("QEMU alive before quit: yes")
PY
); then
    FAILURE_MESSAGE=$validation_output
    printf '%s\n' "$validation_output" >&2
    exit 1
fi

printf '%s\n' "$validation_output"
if successful_shutdown; then
    QEMU_PID=
else
    printf '%s\n' "$FAILURE_MESSAGE" >&2
    exit 1
fi
echo "QEMU clean shutdown: yes"
QEMU_PID=
echo "QEMU virtio-foundation test: OK"
