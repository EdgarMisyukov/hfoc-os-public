#!/usr/bin/env bash
# Prove item 260 in two q35 QEMU boots: one with a real virtio-rng device
# proving healthy seeding, injected rejection, reset/recovery, diagnostics
# privacy, ownership balance, liveness, and clean shutdown; one without the
# device proving missing-device denial and clean shutdown.
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

if [[ "${HFOC_ENTROPY_QEMU_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_ENTROPY_QEMU_TEST_TIMEOUT:-240}s" \
        env HFOC_ENTROPY_QEMU_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Entropy QEMU proof must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "Entropy QEMU proof must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "Entropy QEMU proof must not set QEMU_CPU" >&2
    exit 1
}
echo "QEMU entropy proof baseline: machine=q35 smp=1 cpu=default"

TEST_DIR=$(mktemp -d)
CASE_NAME=
SERIAL_LOG=
MONITOR_LOG=
QEMU_LOG=
INTERRUPTS_LOG=
MONITOR_SOCKET=
QEMU_PID=
FAILURE_MESSAGE=
QEMU_WAIT_RESULT=

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/entropy-${CASE_NAME:-unknown}-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-Entropy QEMU proof failed}" \
        >"$artifact_dir/failure-reason.txt"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$INTERRUPTS_LOG" \
        "$artifact_dir/" 2>/dev/null || true
    echo "Entropy artifacts: $artifact_dir" >&2
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
    if (( status != 0 )) && [[ -d "$TEST_DIR" ]]; then
        best_effort_cleanup
        save_failure_artifacts
    fi
    if [[ -d "$TEST_DIR" ]]; then
        rm -rf "$TEST_DIR"
    fi
    exit "$status"
}

trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

cat >"$TEST_DIR/validate.py" <<'PY'
import os
import pathlib
import socket
import sys
import time

mode = sys.argv[1]
serial_path = pathlib.Path(sys.argv[2])
monitor_path = pathlib.Path(sys.argv[3])
qemu_pid = int(sys.argv[4])
timer_marker = "[LAPIC-TIMER] irq vector=0x42"
command_marker = "UNKNOWN COMMAND: liveness. TYPE HELP."
secret_sentinel = "HFOC-ENTROPY-QEMU-SECRET"

if mode == "healthy":
    required = [
        "[ENTROPY] proof init",
        "[ENTROPY] device live",
        "[ENTROPY] healthy seeding",
        "[ENTROPY] diagnostics privacy",
        "[ENTROPY] injected rejection",
        "[ENTROPY] reset recovery",
        "[ENTROPY] balanced ownership",
        "[ENTROPY] clean shutdown",
        "[ENTROPY] proof OK",
    ]
else:
    required = [
        "[ENTROPY] proof init",
        "[ENTROPY] missing device denial",
        "[ENTROPY] clean shutdown",
        "[ENTROPY] proof OK",
    ]

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

def wait_serial(marker, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = serial_text()
        if marker in text:
            return
        if "[ENTROPY] failed" in text:
            raise SystemExit("Entropy proof reported failure")
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit("Kernel panic during entropy proof")
        if not qemu_running():
            raise SystemExit("QEMU exited before validation completed")
        time.sleep(0.05)
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="' +
        str(f"timed out waiting for {marker}") + '" '
        'deadline=' + str(int(timeout)) + 's\n')
    sys.stderr.flush()
    raise SystemExit(f"timed out waiting for {marker}")

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

wait_serial("[ENTROPY] proof OK", 60)
text = serial_text()
for marker in required:
    if text.count(marker) != 1:
        raise SystemExit(
            f"Expected 1 marker, got {text.count(marker)}: {marker}")
if "[ENTROPY] failed" in text:
    raise SystemExit("Entropy proof reported failure")
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("Kernel panic detected")
if secret_sentinel in text:
    raise SystemExit("Entropy proof serial log leaked the test sentinel")

if mode == "missing":
    for marker in [
        "[ENTROPY] device live",
        "[ENTROPY] healthy seeding",
        "[ENTROPY] injected rejection",
    ]:
        if marker in text:
            raise SystemExit(f"Missing-device boot unexpectedly contains {marker}")
    print("Entropy missing-device proof: found")
else:
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
        # ITEM-585: `entropy` is a real command now, so the console-liveness
        # probe types a word that is still unknown and pins that answer.
        for key in ("l", "i", "v", "e", "n", "e", "s", "s", "ret"):
            monitor.sendall(f"sendkey {key}\n".encode("ascii"))
            wait_prompt(monitor)
            time.sleep(0.08)

    command_deadline = time.monotonic() + 10.0
    while time.monotonic() < command_deadline:
        if command_marker in serial_text():
            break
        if "*** HFOC KERNEL PANIC ***" in serial_text():
            raise SystemExit("Kernel panic during console liveness")
        time.sleep(0.05)
    else:
        # ITEM-422: a proof that gives up on a marker says so.
        sys.stderr.write(
            'HFOC-QEMU-MARKER-TIMEOUT marker="console responsiveness" '
            'deadline=' + str(int(command_deadline)) + 's\n')
        sys.stderr.flush()
        raise SystemExit("timed out waiting for console responsiveness")

    print("Entropy healthy-device proof: found")
    print("LAPIC timer liveness: found")
    print("Console responsiveness: found")
    print("Secret-negative serial scan: passed")
PY

run_case() {
    local mode=$1
    local device_arg=$2
    local -a device_args=()

    CASE_NAME=$mode
    SERIAL_LOG=$TEST_DIR/$mode.serial
    MONITOR_LOG=$TEST_DIR/$mode.monitor
    QEMU_LOG=$TEST_DIR/$mode.qemu
    INTERRUPTS_LOG=$TEST_DIR/$mode.interrupts
    MONITOR_SOCKET=$TEST_DIR/$mode.socket
    QEMU_PID=
    FAILURE_MESSAGE=

    : >"$SERIAL_LOG"
    : >"$MONITOR_LOG"
    : >"$QEMU_LOG"
    : >"$INTERRUPTS_LOG"
    rm -f "$MONITOR_SOCKET"

    if [[ -n "$device_arg" ]]; then
        device_args=(-device "$device_arg")
    fi

    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" \
        -display none -cdrom "$IMAGE" \
        -serial "file:$SERIAL_LOG" \
        -monitor "unix:$MONITOR_SOCKET,server,nowait" \
        "${device_args[@]}" \
        -d int -D "$INTERRUPTS_LOG" -no-reboot -no-shutdown \
        >"$QEMU_LOG" 2>&1 &
    QEMU_PID=$!

    local validation_output
    if ! validation_output=$(python3 "$TEST_DIR/validate.py" \
            "$mode" "$SERIAL_LOG" "$MONITOR_SOCKET" "$QEMU_PID" 2>&1); then
        FAILURE_MESSAGE="${mode} validation failed: $validation_output"
        best_effort_cleanup
        return 1
    fi
    printf '%s\n' "$validation_output"

    if ! successful_shutdown; then
        best_effort_cleanup
        return 1
    fi
    QEMU_PID=
}

if ! run_case healthy \
        "virtio-rng-pci-non-transitional,ioeventfd=off,vectors=0,x-disable-pcie=on"; then
    exit 1
fi

if ! run_case missing ""; then
    exit 1
fi

echo "QEMU entropy healthy-device proof: passed"
echo "QEMU entropy missing-device proof: passed"
echo "QEMU entropy proof: OK"
