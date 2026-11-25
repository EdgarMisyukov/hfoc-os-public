#!/usr/bin/env bash
# Prove the Wave S integration replay/outage drill in one QEMU session:
# duplicate idempotency, malformed and incompatible rejection/isolation,
# unavailable peer isolation without local block, and clean HMP shutdown.
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

if [[ "${HFOC_INTEGRATION_REPLAY_OUTAGE_DRILL_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_INTEGRATION_REPLAY_OUTAGE_DRILL_TEST_TIMEOUT:-90}s" \
        env HFOC_INTEGRATION_REPLAY_OUTAGE_DRILL_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Integration replay drill must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "Integration replay drill must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "Integration replay drill must not set QEMU_CPU" >&2
    exit 1
}
echo "QEMU integration replay baseline: machine=q35 smp=1 cpu=default"

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
    artifact_dir="build/test-artifacts/integration-replay-outage-drill-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-Integration replay drill failed}" \
        >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$INTERRUPTS_LOG" "$MONITOR_SOCKET" \
        "$QEMU_LOG" "$FAILURE_REASON" "$artifact_dir/" 2>/dev/null || true
    echo "Integration replay drill artifacts: $artifact_dir" >&2
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
import os
import pathlib
import sys
import time

def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])

serial_path = pathlib.Path(sys.argv[1])
qemu_pid = int(sys.argv[3])

def serial_text():
    try:
        return SERIAL_VIEW.normalise(
            serial_path.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""

deadline = time.monotonic() + 60.0
while time.monotonic() < deadline:
    text = serial_text()
    if "[INTEGRATION-REPLAY-OUTAGE-DRILL] proof OK" in text:
        break
    if "[INTEGRATION-REPLAY-OUTAGE-DRILL] failed: " in text:
        raise SystemExit("Integration replay drill reported failure")
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic during integration replay drill")
    time.sleep(0.05)
else:
    raise SystemExit(
        "timed out waiting for integration replay drill proof OK")

required = [
    "[INTEGRATION-REPLAY-OUTAGE-DRILL] proof init",
    "[INTEGRATION-REPLAY-OUTAGE-DRILL] catalogue integration fault points exact",
    "[INTEGRATION-REPLAY-OUTAGE-DRILL] duplicate request idempotent",
    "[INTEGRATION-REPLAY-OUTAGE-DRILL] malformed request rejected",
    "[INTEGRATION-REPLAY-OUTAGE-DRILL] malformed peer isolated",
    "[INTEGRATION-REPLAY-OUTAGE-DRILL] incompatible request rejected",
    "[INTEGRATION-REPLAY-OUTAGE-DRILL] incompatible peer isolated",
    "[INTEGRATION-REPLAY-OUTAGE-DRILL] unavailable peer isolated without local block",
    "[INTEGRATION-REPLAY-OUTAGE-DRILL] local paths unaffected",
    "[INTEGRATION-REPLAY-OUTAGE-DRILL] proof OK",
]
text = serial_text()
for marker in required:
    if text.count(marker) != 1:
        raise SystemExit(
            f"Expected exactly 1 marker, got {text.count(marker)}: {marker}")
if "[INTEGRATION-REPLAY-OUTAGE-DRILL] failed: " in text:
    raise SystemExit("Integration replay drill reported failure")
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("Kernel panic detected")
try:
    os.kill(qemu_pid, 0)
except OSError:
    raise SystemExit("QEMU exited before validation completed")

print("Integration replay drill proof: OK")
print("Proof markers: exact")
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

echo "QEMU integration replay drill test: OK"
