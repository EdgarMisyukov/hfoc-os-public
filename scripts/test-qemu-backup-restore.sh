#!/usr/bin/env bash
# Prove backup/restore acceptance in one QEMU session. The first proof runs,
# HMP system_reset reboots the machine, and the same serial log must show a
# second complete proof before a strict HMP shutdown.
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

if [[ "${HFOC_BACKUP_RESTORE_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_BACKUP_RESTORE_TEST_TIMEOUT:-90}s" \
        env HFOC_BACKUP_RESTORE_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Backup-restore test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "Backup-restore test must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "Backup-restore test must not set QEMU_CPU" >&2
    exit 1
}
echo "QEMU backup-restore baseline: machine=q35 smp=1 cpu=default"

TEST_DIR=$(mktemp -d)
SERIAL_LOG=$TEST_DIR/com1.log
INTERRUPTS_LOG=$TEST_DIR/interrupts.log
MONITOR_SOCKET=$TEST_DIR/monitor.sock
QEMU_LOG=$TEST_DIR/qemu.log
FAILURE_REASON=$TEST_DIR/failure-reason.txt
QEMU_PID=
FAILURE_MESSAGE=

: >"$SERIAL_LOG"
: >"$INTERRUPTS_LOG"
: >"$QEMU_LOG"
rm -f "$MONITOR_SOCKET"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/backup-restore-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-Backup-restore QEMU test failed}" \
        >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$INTERRUPTS_LOG" "$MONITOR_SOCKET" \
        "$QEMU_LOG" "$FAILURE_REASON" "$artifact_dir/" 2>/dev/null || true
    echo "Backup-restore artifacts: $artifact_dir" >&2
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

# Strict success shutdown: QEMU must accept quit, close the monitor, and exit
# by itself. TERM/KILL are never success.
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
    -d int -D "$INTERRUPTS_LOG" -no-shutdown \
    >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

validation_output=
if ! validation_output=$(python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" \
    "$QEMU_PID" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import pathlib
import re
import socket
import sys
import time
import os

serial_path = pathlib.Path(sys.argv[1])
monitor_socket = pathlib.Path(sys.argv[2])
qemu_pid = int(sys.argv[3])

def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])


def serial_text():
    try:
        return SERIAL_VIEW.normalise(
            serial_path.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""

def monitor_prompt(monitor, timeout):
    prompt = b""
    prompt_deadline = time.monotonic() + timeout
    while b"(qemu)" not in prompt:
        remaining = prompt_deadline - time.monotonic()
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
    return prompt

first_deadline = time.monotonic() + 40.0
while time.monotonic() < first_deadline:
    text = serial_text()
    if text.count("[BACKUP-RESTORE] proof OK") >= 1:
        break
    if "[BACKUP-RESTORE] failed" in text:
        raise SystemExit("Backup-restore proof 1 reported failure")
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic during proof 1")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="backup-restore proof 1" '
        'deadline=' + str(int(first_deadline)) + 's\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for backup-restore proof 1")

if not monitor_socket.exists():
    raise SystemExit("timed out waiting for QEMU monitor socket")

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
    monitor.settimeout(2.0)
    try:
        monitor.connect(str(monitor_socket))
    except OSError as exc:
        raise SystemExit(f"QEMU monitor did not accept connection: {exc}")
    monitor_prompt(monitor, 5.0)
    try:
        monitor.sendall(b"system_reset\n")
    except OSError as exc:
        raise SystemExit(f"QEMU monitor did not accept system_reset: {exc}")
    monitor_prompt(monitor, 5.0)

try:
    os.kill(qemu_pid, 0)
except OSError:
    raise SystemExit("QEMU exited before system_reset")

second_deadline = time.monotonic() + 40.0
while time.monotonic() < second_deadline:
    text = serial_text()
    if text.count("[BACKUP-RESTORE] proof OK") >= 2:
        break
    if "[BACKUP-RESTORE] failed" in text:
        raise SystemExit("Backup-restore proof 2 reported failure")
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic during proof 2")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="backup-restore proof 2" '
        'deadline=' + str(int(second_deadline)) + 's\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for backup-restore proof 2")

required = [
    "[BACKUP-RESTORE] proof init",
    "[BACKUP-RESTORE] active state initialized",
    "[BACKUP-RESTORE] stream exported",
    "[BACKUP-RESTORE] dry-run plan validated",
    "[BACKUP-RESTORE] active state changed",
    "[BACKUP-RESTORE] values restored",
    "[BACKUP-RESTORE] audit continuity preserved",
    "[BACKUP-RESTORE] clean reboot",
    "[BACKUP-RESTORE] proof OK",
]
text = serial_text()
for marker in required:
    if text.count(marker) != 2:
        raise SystemExit(
            f"Expected 2 markers across reboot, got {text.count(marker)}: {marker}")
if "[BACKUP-RESTORE] failed" in text:
    raise SystemExit("Backup-restore proof reported failure")
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("Kernel panic detected")

print("Backup-restore proof 1: OK")
print("HMP system_reset: issued")
print("Backup-restore proof 2: OK")
print("Kernel panic: none")
print("QEMU alive before reset: yes")
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
echo "QEMU alive before quit: yes"

if successful_shutdown; then
    QEMU_PID=
else
    printf '%s\n' "$FAILURE_MESSAGE" >&2
    exit 1
fi

echo "QEMU backup-restore test: OK"
