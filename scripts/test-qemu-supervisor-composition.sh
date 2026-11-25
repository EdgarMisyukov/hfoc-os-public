#!/usr/bin/env bash
# Prove the BSP service supervisor, appliance service manager, appliance
# HTTP/HTTPS service lifecycle, persistent service enablement, and service
# failure-handler composition in one QEMU session: dependency order, readiness
# timeout, contained restart, manager health snapshot, HTTPS lifecycle,
# service enablement persistence, automatic failure recovery, audit, PS/2
# keyboard response, and clean shutdown.
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

if [[ "${HFOC_SUPERVISOR_COMPOSITION_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_SUPERVISOR_COMPOSITION_TEST_TIMEOUT:-90}s" \
        env HFOC_SUPERVISOR_COMPOSITION_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Supervisor composition test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "Supervisor composition test must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "Supervisor composition test must not set QEMU_CPU" >&2
    exit 1
}
echo "QEMU supervisor baseline: machine=q35 smp=1 cpu=default"

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
    artifact_dir="build/test-artifacts/supervisor-composition-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-Supervisor composition test failed}" \
        >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$INTERRUPTS_LOG" \
        "$FAILURE_REASON" "$artifact_dir/" 2>/dev/null || true
    echo "Supervisor composition artifacts: $artifact_dir" >&2
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
    -d int -D "$INTERRUPTS_LOG" -no-reboot -no-shutdown \
    >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

validation_output=
if ! validation_output=$(python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" \
    "$QEMU_PID" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import pathlib
import socket
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
monitor_socket = pathlib.Path(sys.argv[2])
qemu_pid = int(sys.argv[3])

def serial_text():
    try:
        return SERIAL_VIEW.normalise(
            serial_path.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""

def monitor_prompt(monitor, timeout):
    prompt = b""
    deadline = time.monotonic() + timeout
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

deadline = time.monotonic() + 45.0
while time.monotonic() < deadline:
    text = serial_text()
    if "[SUPERVISOR] composition init" in text:
        break
    if "[SUPERVISOR] failed" in text:
        raise SystemExit("Supervisor proof reported failure before sendkey")
    if "[APP-SVC] failed" in text:
        raise SystemExit("Appliance service manager proof reported failure before sendkey")
    if "[APP-HTTPS] failed" in text:
        raise SystemExit("Appliance HTTPS proof reported failure before sendkey")
    if "[SVC-PERSIST] failed" in text:
        raise SystemExit("Persistent service proof reported failure before sendkey")
    if "[FAIL-HANDLER] failed" in text:
        raise SystemExit("Service failure handler proof reported failure before sendkey")
    if "[PROD-SVC] failed" in text:
        raise SystemExit("Production service HTTPS proof reported failure before sendkey")
    if "[PROD-MGMT] failed" in text:
        raise SystemExit("Production management proof reported failure before sendkey")
    if "[FIRST-BOOT] failed" in text:
        raise SystemExit("First boot integration proof reported failure before sendkey")
    if "[RECOVERY-RESET] failed" in text:
        raise SystemExit("Recovery factory reset integration proof reported failure before sendkey")
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic before sendkey")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="supervisor composition init" '
        'deadline=45s\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for supervisor composition init")

while not monitor_socket.exists():
    if time.monotonic() >= deadline:
        raise SystemExit("timed out waiting for QEMU monitor socket")
    time.sleep(0.05)

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
    monitor.settimeout(2.0)
    try:
        monitor.connect(str(monitor_socket))
    except OSError as exc:
        raise SystemExit(f"QEMU monitor did not accept connection: {exc}")
    monitor_prompt(monitor, 5.0)
    try:
        monitor.sendall(b"sendkey a\n")
    except OSError as exc:
        raise SystemExit(f"QEMU monitor did not accept sendkey: {exc}")
    monitor_prompt(monitor, 5.0)

deadline = time.monotonic() + 45.0
while time.monotonic() < deadline:
    text = serial_text()
    if "[SUPERVISOR] proof OK" in text and "[PROD-SVC] proof OK" in text and "[PROD-MGMT] proof OK" in text and "[FIRST-BOOT] proof OK" in text and "[RECOVERY-RESET] proof OK" in text:
        break
    if "[SUPERVISOR] failed" in text:
        raise SystemExit("Supervisor proof reported failure")
    if "[APP-SVC] failed" in text:
        raise SystemExit("Appliance service manager proof reported failure")
    if "[APP-HTTPS] failed" in text:
        raise SystemExit("Appliance HTTPS proof reported failure")
    if "[SVC-PERSIST] failed" in text:
        raise SystemExit("Persistent service proof reported failure")
    if "[FAIL-HANDLER] failed" in text:
        raise SystemExit("Service failure handler proof reported failure")
    if "[PROD-SVC] failed" in text:
        raise SystemExit("Production service HTTPS proof reported failure")
    if "[PROD-MGMT] failed" in text:
        raise SystemExit("Production management proof reported failure")
    if "[FIRST-BOOT] failed" in text:
        raise SystemExit("First boot integration proof reported failure")
    if "[RECOVERY-RESET] failed" in text:
        raise SystemExit("Recovery factory reset integration proof reported failure")
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic during proof")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="supervisor proof OK" '
        'deadline=45s\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for supervisor proof OK")

text = serial_text()
required = [
    "[SUPERVISOR] dependency order 1 2 3",
    "[SUPERVISOR] composition init",
    "[SUPERVISOR] readiness published 1 2",
    "[SUPERVISOR] timeout published service=3",
    "[SUPERVISOR] late ready rejected",
    "[SUPERVISOR] contained restart service=3 only",
    "[SUPERVISOR] restarted readiness service=3",
    "[SUPERVISOR] audit record verified",
    "[SUPERVISOR] clean shutdown",
    "[SUPERVISOR] keyboard responsive",
    "[SUPERVISOR] proof OK",
    "[APP-SVC] manager init",
    "[APP-SVC] dependency order 1 2 3",
    "[APP-SVC] healthy snapshot",
    "[APP-SVC] contained restart service=3",
    "[APP-SVC] clean shutdown",
    "[APP-SVC] proof OK",
    "[APP-HTTPS] dependency-gated start",
    "[APP-HTTPS] ready listening",
    "[APP-HTTPS] healthy snapshot",
    "[APP-HTTPS] contained restart lifecycle",
    "[APP-HTTPS] clean shutdown",
    "[APP-HTTPS] proof OK",
    "[SVC-PERSIST] proof init",
    "[SVC-PERSIST] default apply and startup",
    "[SVC-PERSIST] disabled service committed",
    "[SVC-PERSIST] fresh load restores enablement",
    "[SVC-PERSIST] restored startup with disabled service",
    "[SVC-PERSIST] clean shutdown",
    "[SVC-PERSIST] proof OK",
    "[FAIL-HANDLER] proof init",
    "[FAIL-HANDLER] healthy no failure",
    "[FAIL-HANDLER] automatic recovery service=3",
    "[FAIL-HANDLER] recovered readiness",
    "[FAIL-HANDLER] clean shutdown",
    "[FAIL-HANDLER] proof OK",
    "[PROD-SVC] proof init",
    "[PROD-SVC] persistent evidence committed",
    "[PROD-SVC] missing readiness blocks https",
    "[PROD-SVC] service manager started",
    "[PROD-SVC] https running",
    "[PROD-SVC] canonical snapshot",
    "[PROD-SVC] link loss fails closed",
    "[PROD-SVC] clean shutdown",
    "[PROD-SVC] proof OK",
    "[PROD-MGMT] proof init",
    "[PROD-MGMT] service https evidence active",
    "[PROD-MGMT] missing evidence blocks publication",
    "[PROD-MGMT] canonical snapshot",
    "[PROD-MGMT] audit integrity view",
    "[PROD-MGMT] audit tamper view",
    "[PROD-MGMT] degraded health published",
    "[PROD-MGMT] failure fails closed",
    "[PROD-MGMT] clean shutdown",
    "[PROD-MGMT] proof OK",
    "[FIRST-BOOT] proof init",
    "[FIRST-BOOT] service https evidence active",
    "[FIRST-BOOT] production management evidence active",
    "[FIRST-BOOT] missing wizard blocks ready",
    "[FIRST-BOOT] missing management blocks ready",
    "[FIRST-BOOT] canonical snapshot",
    "[FIRST-BOOT] failure fails closed",
    "[FIRST-BOOT] completion confirmed",
    "[FIRST-BOOT] clean shutdown",
    "[FIRST-BOOT] proof OK",
    "[RECOVERY-RESET] proof init",
    "[RECOVERY-RESET] no operation blocks ready",
    "[RECOVERY-RESET] missing foundation blocks ready",
    "[RECOVERY-RESET] recovery snapshot",
    "[RECOVERY-RESET] factory reset snapshot",
    "[RECOVERY-RESET] stale evidence rejected",
    "[RECOVERY-RESET] completion confirmed",
    "[RECOVERY-RESET] clean shutdown",
    "[RECOVERY-RESET] proof OK",
]
for marker in required:
    if text.count(marker) != 1:
        raise SystemExit(
            f"Expected 1 marker, got {text.count(marker)}: {marker}")
if "[SUPERVISOR] failed" in text:
    raise SystemExit("Supervisor proof reported failure")
if "[APP-SVC] failed" in text:
    raise SystemExit("Appliance service manager proof reported failure")
if "[APP-HTTPS] failed" in text:
    raise SystemExit("Appliance HTTPS proof reported failure")
if "[SVC-PERSIST] failed" in text:
    raise SystemExit("Persistent service proof reported failure")
if "[FAIL-HANDLER] failed" in text:
    raise SystemExit("Service failure handler proof reported failure")
if "[PROD-SVC] failed" in text:
    raise SystemExit("Production service HTTPS proof reported failure")
if "[PROD-MGMT] failed" in text:
    raise SystemExit("Production management proof reported failure")
if "[FIRST-BOOT] failed" in text:
    raise SystemExit("First boot integration proof reported failure")
if "[RECOVERY-RESET] failed" in text:
    raise SystemExit("Recovery factory reset integration proof reported failure")
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("Kernel panic detected")

try:
    import os
    os.kill(qemu_pid, 0)
except OSError:
    raise SystemExit("QEMU exited before validation completed")

print("Supervisor composition init: found")
print("Supervisor dependency order: 1 2 3")
print("Supervisor timeout and contained restart: found")
print("Supervisor audit record: verified")
print("Supervisor keyboard response: found")
print("Supervisor clean shutdown: found")
print("Supervisor proof OK: found")
print("Appliance service manager health and restart: found")
print("Appliance HTTPS service lifecycle proof: found")
print("Persistent service enablement proof: found")
print("Service failure handler automatic recovery: found")
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
echo "QEMU alive before quit: yes"

if successful_shutdown; then
    QEMU_PID=
else
    printf '%s\n' "$FAILURE_MESSAGE" >&2
    exit 1
fi

echo "QEMU supervisor composition test: OK"
