#!/usr/bin/env bash
# Test read-only IOAPIC probe on q35 with APIC and a single vCPU.
# Profile: q35, default CPU, -smp 1.
# Transport: serial log file + monitor Unix socket, no keyboard input.
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

if [[ "${HFOC_IOAPIC_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM "${HFOC_IOAPIC_TEST_TIMEOUT:-60}s" \
        env HFOC_IOAPIC_TEST_INNER=1 "$0" "$@"
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
SERIAL_LOG=$TEST_DIR/com1.log
MONITOR_LOG=$TEST_DIR/monitor.log
QEMU_LOG=$TEST_DIR/qemu.log
METADATA=$TEST_DIR/metadata.txt
FAILURE_REASON=$TEST_DIR/failure-reason.txt
MONITOR_SOCKET=$TEST_DIR/monitor.sock
QEMU_PID=
FAILURE_MESSAGE=

: >"$SERIAL_LOG"
: >"$MONITOR_LOG"
: >"$QEMU_LOG"

printf '%s\n' \
    "machine=$QEMU_MACHINE" \
    "cpu=default" \
    "smp=$QEMU_SMP" \
    "image=$IMAGE" >"$METADATA"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/ioapic-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-IOAPIC test failed}" >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$METADATA" "$FAILURE_REASON" "$artifact_dir/"
    echo "IOAPIC test artifacts: $artifact_dir" >&2
}

QEMU_WAIT_RESULT=

wait_for_exit() {
    # Loaded GitHub runners can need several seconds to exit after monitor quit.
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

# Strict success shutdown: QEMU must present an HMP prompt, accept quit, and
# then close the monitor and exit 0 by itself. TERM/KILL are never success.
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

# Best-effort cleanup after an error. It preserves the original failure status.
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
    -no-reboot -no-shutdown >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

validation_output=
if ! validation_output=$(python3 - "$SERIAL_LOG" \
    "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import pathlib
import re
import sys
import time

serial = pathlib.Path(sys.argv[1])
deadline = time.monotonic() + 45.0

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
            serial.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""

while time.monotonic() < deadline:
    text = serial_text()
    if "hfoc>" in text:
        break
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="hfoc> boot prompt" '
        'deadline=45s\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for hfoc> boot prompt")

text = serial_text()
lower_text = text.lower()

probe_match = re.search(
    r"\[IOAPIC\] probe: id=(\d+) base=(0x[0-9A-Fa-f]+) "
    r"gsi_base=(\d+) version=(\d+) redirections=(\d+)",
    text)
if not probe_match:
    raise SystemExit("Missing: [IOAPIC] probe: line with parsed fields")

ioapic_id = int(probe_match.group(1))
base_text = probe_match.group(2)
version = int(probe_match.group(4))
redirections = int(probe_match.group(5))
base = int(base_text, 16)
if not (0 <= ioapic_id <= 255):
    raise SystemExit(f"IOAPIC ID out of range 0..255: {ioapic_id}")
if version == 0:
    raise SystemExit("IOAPIC version is zero")
if redirections == 0:
    raise SystemExit("IOAPIC redirections are zero")
if base == 0:
    raise SystemExit("IOAPIC MMIO base is zero")

if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("Kernel panic detected")
if "MMIO map failed" in text:
    raise SystemExit("IOAPIC MMIO map failure detected")

pre_pic_match = re.search(
    r"\[PIC \] IOAPIC pre: master=(0x[0-9A-Fa-f]+) slave=(0x[0-9A-Fa-f]+)",
    text)
post_pic_match = re.search(
    r"\[PIC \] IOAPIC post: master=(0x[0-9A-Fa-f]+) slave=(0x[0-9A-Fa-f]+)",
    text)
if not pre_pic_match or not post_pic_match:
    raise SystemExit("Missing: [PIC ] IOAPIC pre/post markers")

pre_master = int(pre_pic_match.group(1), 16)
pre_slave = int(pre_pic_match.group(2), 16)
post_master = int(post_pic_match.group(1), 16)
post_slave = int(post_pic_match.group(2), 16)
if pre_master != post_master or pre_slave != post_slave:
    raise SystemExit("PIC masks changed across IOAPIC probe")

if "master mask: 0x00000000000000ff" not in lower_text:
    raise SystemExit("Master PIC mask is not 0x00000000000000ff")
if "slave mask: 0x00000000000000ff" not in lower_text:
    raise SystemExit("Slave PIC mask is not 0x00000000000000ff")

print(f"IOAPIC ID: {ioapic_id}")
print(f"IOAPIC version: {version}")
print(f"IOAPIC redirections: {redirections}")
print(f"IOAPIC base: {base_text}")
print("Boot prompt: found")
print("Kernel panic: none")
print("MMIO map failure: none")
print(f"Pre-probe PIC: master={pre_pic_match.group(1)} slave={pre_pic_match.group(2)}")
print(f"Post-probe PIC: master={post_pic_match.group(1)} slave={post_pic_match.group(2)}")
print("PIC masks unchanged across IOAPIC probe: confirmed")
print("PIC masks retained: confirmed")
PY
); then
    FAILURE_MESSAGE=$validation_output
    printf '%s\n' "$validation_output" >&2
    exit 1
fi

printf '%s\n' "$validation_output"
if ! kill -0 "$QEMU_PID" 2>/dev/null; then
    FAILURE_MESSAGE="QEMU exited before monitor quit"
    printf '%s\n' "$FAILURE_MESSAGE" >&2
    exit 1
fi
echo "QEMU alive: yes"
if successful_shutdown; then
    QEMU_PID=
    echo "QEMU IOAPIC probe test: OK"
else
    printf '%s\n' "$FAILURE_MESSAGE" >&2
    exit 1
fi
