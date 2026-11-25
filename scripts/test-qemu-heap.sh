#!/usr/bin/env bash
# Test the heap console diagnostic on q35 / one vCPU / default CPU.
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

if [[ "${HFOC_HEAP_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_HEAP_TEST_TIMEOUT:-60}s" \
        env HFOC_HEAP_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "APIC baseline must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "APIC baseline must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "APIC baseline must not set QEMU_CPU" >&2
    exit 1
}
echo "QEMU APIC baseline: machine=q35 smp=1 cpu=default"

TEST_DIR=$(mktemp -d)
SERIAL_LOG=$TEST_DIR/serial.log
MONITOR_LOG=$TEST_DIR/monitor.log
QEMU_LOG=$TEST_DIR/qemu.log
METADATA=$TEST_DIR/metadata.txt
FAILURE_REASON=$TEST_DIR/failure-reason.txt
MONITOR_SOCKET=$TEST_DIR/monitor.sock
SERIAL_OFFSET_FILE=$TEST_DIR/serial-offset.txt
QEMU_PID=
FAILURE_MESSAGE=

: >"$SERIAL_LOG"
: >"$MONITOR_LOG"
: >"$QEMU_LOG"

printf '%s\n' \
    "machine=$QEMU_MACHINE" \
    "cpu=default" \
    "smp=$QEMU_SMP" \
    "image=$IMAGE" \
    "command=heap" >"$METADATA"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/heap-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-HEAP test failed}" \
        >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$METADATA" \
        "$FAILURE_REASON" "$SERIAL_OFFSET_FILE" "$artifact_dir/"
    echo "HEAP test artifacts: $artifact_dir" >&2
}

wait_for_exit() {
    local deadline=$((SECONDS + 5))
    while kill -0 "$QEMU_PID" 2>/dev/null; do
        if (( SECONDS >= deadline )); then
            return 1
        fi
        sleep 0.1
    done
    wait "$QEMU_PID" 2>/dev/null || true
}

send_monitor_quit() {
    python3 - "$MONITOR_SOCKET" <<'PY'
import socket
import sys

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

    prompt = b""
    deadline = 5.0
    while b"(qemu)" not in prompt:
        if deadline <= 0.0:
            fail("QEMU HMP prompt timeout")
        monitor.settimeout(deadline)
        try:
            chunk = monitor.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            fail("QEMU HMP closed before prompt")
        prompt += chunk

    monitor.sendall(b"quit\n")
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

stop_qemu() {
    if [[ -z "$QEMU_PID" ]] || ! kill -0 "$QEMU_PID" 2>/dev/null; then
        wait "$QEMU_PID" 2>/dev/null || true
        return 0
    fi
    if send_monitor_quit 2>/dev/null; then
        if wait_for_exit; then
            return 0
        fi
    fi
    kill -TERM "$QEMU_PID" 2>/dev/null || true
    if wait_for_exit; then
        return 0
    fi
    kill -KILL "$QEMU_PID" 2>/dev/null || true
    wait_for_exit
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    if (( status != 0 )); then
        stop_qemu
        save_failure_artifacts
    else
        stop_qemu
        if kill -0 "$QEMU_PID" 2>/dev/null; then
            FAILURE_MESSAGE="QEMU still running after stop"
            save_failure_artifacts
            status=1
        fi
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
    -d int,guest_errors -D "$QEMU_LOG" \
    -no-reboot -no-shutdown >/dev/null 2>&1 &
QEMU_PID=$!

phase1_output=
if ! phase1_output=$(python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" \
    "$MONITOR_LOG" "$SERIAL_OFFSET_FILE" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import pathlib
import socket
import sys
import time

serial_path = pathlib.Path(sys.argv[1])
socket_path = pathlib.Path(sys.argv[2])
monitor_log = pathlib.Path(sys.argv[3])
offset_path = pathlib.Path(sys.argv[4])
deadline = time.monotonic() + 30.0


def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])

def serial_bytes():
    try:
        return serial_path.read_bytes()
    except FileNotFoundError:
        return b""

def serial_text():
    return SERIAL_VIEW.normalise(
        serial_bytes().decode("ascii", errors="replace"))

while time.monotonic() < deadline:
    text = serial_text()
    if "HFOC OS 1.0.0 INITIALISED" in text and "hfoc> " in text:
        break
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic before HEAP command")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="boot prompt" '
        'deadline=30s\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for boot prompt")

while not socket_path.exists():
    if time.monotonic() >= deadline:
        raise SystemExit("timed out waiting for QEMU monitor socket")
    time.sleep(0.05)

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
    monitor.settimeout(1.0)
    try:
        monitor.connect(str(socket_path))
    except OSError as exc:
        raise SystemExit(f"QEMU monitor did not accept connection: {exc}")

    def receive_prompt():
        chunks = []
        while True:
            chunk = monitor.recv(4096)
            if not chunk:
                raise RuntimeError("QEMU monitor closed unexpectedly")
            chunks.append(chunk)
            if b"(qemu)" in b"".join(chunks):
                return b"".join(chunks).decode("utf-8", errors="replace")

    def record(value):
        with monitor_log.open("a", encoding="utf-8") as output:
            output.write(value)

    record(receive_prompt())
    offset_path.write_text(str(len(serial_bytes())), encoding="ascii")

    for key in ['h', 'e', 'a', 'p', 'ret']:
        monitor.sendall(f"sendkey {key}\n".encode())
        record(receive_prompt())
        time.sleep(0.06)
PY
); then
    FAILURE_MESSAGE=$phase1_output
    printf '%s\n' "$phase1_output" >&2
    exit 1
fi

SERIAL_OFFSET=$(<"$SERIAL_OFFSET_FILE")
printf '%s\n' "serial_offset=$SERIAL_OFFSET" >>"$METADATA"

validation_output=
if ! validation_output=$(python3 - "$SERIAL_LOG" "$QEMU_LOG" "$QEMU_PID" \
    "$SERIAL_OFFSET" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import os
import pathlib
import re
import sys
import time

serial = pathlib.Path(sys.argv[1])
qemu_log = pathlib.Path(sys.argv[2])
qemu_pid = int(sys.argv[3])
offset = int(sys.argv[4])
deadline = time.monotonic() + 15.0


def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])

def normalise_suffix(text):
    return SERIAL_VIEW.normalise(text)

split_fixture = (
    "[HEAP] ready=y[LAPIC-TIMER] irq vector=0x42\r\n"
    "[LAPIC-TIMER] irq lapic_eoi\r\n"
    "es capacity=4194304 used=0\r\n"
    "hfoc> "
)
normalised_fixture = normalise_suffix(split_fixture)
if "[HEAP] ready=yes capacity=4194304 used=0" not in normalised_fixture:
    raise SystemExit("heap parser fixture split by periodic diagnostics")

def read_suffix():
    data = serial.read_bytes()
    return data[offset:].decode("ascii", errors="replace")

while time.monotonic() < deadline:
    suffix = normalise_suffix(read_suffix())
    if "[HEAP] ready=" in suffix and "capacity=" in suffix and \
            "used=" in suffix and "hfoc> " in suffix:
        break
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="HEAP diagnostic" '
        'deadline=15s\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for HEAP diagnostic")

suffix = normalise_suffix(read_suffix())
match = re.search(
    r"\[HEAP\] ready=(yes|no) capacity=([0-9]+) used=([0-9]+)", suffix)
if not match:
    raise SystemExit("Missing exact [HEAP] diagnostic")
ready = match.group(1)
capacity = int(match.group(2))
used = int(match.group(3))
if ready != "yes":
    raise SystemExit("HEAP must report ready=yes")
if capacity != 4194304:
    raise SystemExit(f"HEAP capacity mismatch: {capacity}")
if used != 0:
    raise SystemExit(f"HEAP used bytes must be zero after boot: {used}")

diagnostic_index = suffix.find("[HEAP] ready=")
if diagnostic_index < 0 or "hfoc> " not in suffix[diagnostic_index:]:
    raise SystemExit("No new hfoc> prompt after HEAP command")

full = SERIAL_VIEW.normalise(
    serial.read_bytes().decode("ascii", errors="replace"))
if "*** HFOC KERNEL PANIC ***" in full:
    raise SystemExit("Unexpected kernel panic")

def qemu_is_running():
    try:
        os.kill(qemu_pid, 0)
        state = pathlib.Path(
            f"/proc/{qemu_pid}/stat").read_text(encoding="ascii").split()[2]
        return state != "Z"
    except (OSError, IndexError, FileNotFoundError):
        return False

if not qemu_is_running():
    raise SystemExit("QEMU exited before validation completed")

time.sleep(0.5)
if not qemu_is_running():
    raise SystemExit("QEMU exited during observation interval")

print(f"ready={ready}")
print(f"capacity={capacity}")
print(f"used={used}")
print("Exact diagnostic: found")
print("New hfoc> after command: yes")
print("Kernel panic: none")
print("QEMU alive: yes")
PY
); then
    FAILURE_MESSAGE=$validation_output
    printf '%s\n' "$validation_output" >&2
    exit 1
fi

printf '%s\n' "$validation_output"
echo "QEMU HEAP test: OK"
