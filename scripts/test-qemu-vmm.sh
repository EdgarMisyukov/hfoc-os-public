#!/usr/bin/env bash
# Test VMM MMIO mapper: send vmmmap via HMP sendkey, validate PTE.
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

if [[ "${HFOC_VMM_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM "${HFOC_VMM_TEST_TIMEOUT:-45}s" \
        env HFOC_VMM_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_CPU=${QEMU_CPU:-max,-apic}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

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
    "cpu=$QEMU_CPU" \
    "image=$IMAGE" \
    "command=vmmmap" >"$METADATA"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/vmm-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-VMM test failed}" >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$METADATA" "$FAILURE_REASON" "$artifact_dir/"
    echo "VMM test artifacts: $artifact_dir" >&2
}

wait_for_exit() {
    local deadline=$((SECONDS + 1))
    while kill -0 "$QEMU_PID" 2>/dev/null; do
        if (( SECONDS >= deadline )); then
            return 1
        fi
        sleep 0.1
    done
    wait "$QEMU_PID" 2>/dev/null || true
}

stop_qemu() {
    if [[ -z "$QEMU_PID" ]] || ! kill -0 "$QEMU_PID" 2>/dev/null; then
        wait "$QEMU_PID" 2>/dev/null || true
        return 0
    fi
    python3 - "$MONITOR_SOCKET" <<'PY' >/dev/null 2>&1 || true
import socket
import sys

try:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
        monitor.settimeout(0.5)
        monitor.connect(sys.argv[1])
        monitor.sendall(b"quit\n")
except OSError:
    pass
PY
    if wait_for_exit; then return 0; fi
    kill -TERM "$QEMU_PID" 2>/dev/null || true
    if wait_for_exit; then return 0; fi
    kill -KILL "$QEMU_PID" 2>/dev/null || true
    wait_for_exit
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    stop_qemu
    local qemu_status=$?
    if (( status == 0 )); then
        if (( qemu_status != 0 )); then
            FAILURE_MESSAGE="QEMU did not terminate after quit/TERM/KILL"
            save_failure_artifacts
            status=1
        fi
    else
        save_failure_artifacts
    fi
    rm -rf "$TEST_DIR"
    exit "$status"
}

trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -cpu "$QEMU_CPU" -smp 1 \
    -display none -cdrom "$IMAGE" \
    -serial "file:$SERIAL_LOG" \
    -monitor "unix:$MONITOR_SOCKET,server,nowait" \
    -d int,guest_errors -D "$QEMU_LOG" \
    -no-reboot -no-shutdown >/dev/null 2>&1 &
QEMU_PID=$!

# Phase 1: boot + send vmmmap via HMP sendkey
phase1_output=
if ! phase1_output=$(python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" "$MONITOR_LOG"     "$SERIAL_OFFSET_FILE" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import pathlib
import socket
import sys
import time

serial_path = pathlib.Path(sys.argv[1])
socket_path = pathlib.Path(sys.argv[2])
monitor_log = pathlib.Path(sys.argv[3])
offset_path = pathlib.Path(sys.argv[4])
deadline = time.monotonic() + 15.0
monitor_log.write_text("", encoding="utf-8")


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

while time.monotonic() < deadline:
    text = SERIAL_VIEW.normalise(
        serial_bytes().decode("ascii", errors="replace"))
    if "HFOC OS 1.0.0 INITIALISED" in text and "hfoc> " in text:
        break
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="initial boot and hfoc> prompt" '
        'deadline=15s\n')
    sys.stderr.flush()
    raise TimeoutError("timed out waiting for initial boot and hfoc> prompt")

while not socket_path.exists():
    if time.monotonic() >= deadline:
        raise TimeoutError("timed out waiting for QEMU monitor socket")
    time.sleep(0.05)

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
    monitor.settimeout(1.0)
    monitor.connect(str(socket_path))

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

    # Save serial offset before sending keys
    offset_path.write_text(str(len(serial_bytes())), encoding="ascii")

    # Send vmmmap via HMP sendkey
    for key in ['v', 'm', 'm', 'm', 'a', 'p', 'ret']:
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

# Phase 2: validation
validation_output=
if ! validation_output=$(python3 - "$SERIAL_LOG" "$QEMU_LOG" "$QEMU_PID" "$MONITOR_LOG"     "$SERIAL_OFFSET" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import os
import pathlib
import re
import sys
import time

serial = pathlib.Path(sys.argv[1])
qemu_log = pathlib.Path(sys.argv[2])
qemu_pid = int(sys.argv[3])
monitor_log = pathlib.Path(sys.argv[4])
offset = int(sys.argv[5])
deadline = time.monotonic() + 10.0

def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])

def read_suffix():
    data = serial.read_bytes()
    return SERIAL_VIEW.normalise(
        data[offset:].decode("ascii", errors="replace"))

while time.monotonic() < deadline:
    suffix = read_suffix()
    if "[VMM] MMIO page mapped successfully" in suffix and "hfoc> " in suffix:
        break
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="VMM success markers and new hfoc>" '
        'deadline=10s\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for VMM success markers and new hfoc>")

suffix = read_suffix()

# 1. Mapping line (in suffix only)
if "[VMM] Mapping LAPIC page phys=0x00000000FEE00000" not in suffix:
    raise SystemExit("Missing: [VMM] Mapping LAPIC page phys=0x00000000FEE00000")

# 2. virtual address
virt_match = re.search(r"\[VMM\] virtual=(0x[0-9A-F]+)", suffix)
if not virt_match:
    raise SystemExit("Missing: [VMM] virtual= line")
virt_val = int(virt_match.group(1), 16)
if virt_val == 0:
    raise SystemExit("Virtual address is zero")

# 3. PTE line
pte_match = re.search(r"\[VMM\] PTE=(0x[0-9A-F]+) P=1 RW=1 PWT=1 PCD=1 PAT=0", suffix)
if not pte_match:
    raise SystemExit("Missing: [VMM] PTE= line with expected flags")
pte_val = int(pte_match.group(1), 16)
pte_phys = pte_val & 0x000FFFFFFFFFF000
if pte_phys != 0xFEE00000:
    raise SystemExit(f"PTE physical mismatch: expected 0xFEE00000, got {pte_phys:#018x}")
if not (pte_val & 1):
    raise SystemExit("PTE Present bit not set")
if not (pte_val & 2):
    raise SystemExit("PTE RW bit not set")
if not (pte_val & 8):
    raise SystemExit("PTE PWT bit not set")
if not (pte_val & 16):
    raise SystemExit("PTE PCD bit not set")
if pte_val & (1 << 7):
    raise SystemExit("PTE PAT bit set (expected 0)")

# 4. Success marker
if "[VMM] MMIO page mapped successfully" not in suffix:
    raise SystemExit("Missing: [VMM] MMIO page mapped successfully")

# 5. No panic (check full log — panic should NEVER appear)
full = SERIAL_VIEW.normalise(
    serial.read_bytes().decode("ascii", errors="replace"))
if "*** HFOC KERNEL PANIC ***" in full:
    raise SystemExit("Unexpected kernel panic")

# 6. New prompt after command (in suffix only)
success_index = suffix.find("[VMM] MMIO page mapped successfully")
if success_index < 0 or "hfoc> " not in suffix[success_index:]:
    raise SystemExit("No new hfoc> prompt after vmmmap")

# 7. QEMU alive check
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

print(f"serial_offset={offset}")
print(f"PTE: {pte_match.group(1)}")
print(f"Virtual: {virt_match.group(1)}")
print(f"PTE physical: {pte_phys:#018x}")
print("Flags: P=1 RW=1 PWT=1 PCD=1 PAT=0")
print("Success marker: found")
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
if stop_qemu; then
    QEMU_PID=
    echo "QEMU VMM MMIO mapper test: OK"
else
    FAILURE_MESSAGE="QEMU did not terminate after quit/TERM/KILL"
    printf '%s\n' "$FAILURE_MESSAGE" >&2
    exit 1
fi
