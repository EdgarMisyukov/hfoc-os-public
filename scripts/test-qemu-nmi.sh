#!/usr/bin/env bash
# Test that a real NMI is delivered via IST2 and triggers a diagnostic
# kernel panic.
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

if [[ "${HFOC_NMI_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM "${HFOC_NMI_TEST_TIMEOUT:-30}s" \
        env HFOC_NMI_TEST_INNER=1 "$0" "$@"
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
SERIAL_OFFSET_FILE=$TEST_DIR/serial-offset.txt
SUFFIX_LOG=$TEST_DIR/serial-suffix.log
MONITOR_SOCKET=$TEST_DIR/monitor.sock
QEMU_PID=
FAILURE_MESSAGE=

: >"$SERIAL_LOG"
: >"$MONITOR_LOG"
: >"$QEMU_LOG"

printf '%s\n' \
    "machine=$QEMU_MACHINE" \
    "cpu=$QEMU_CPU" \
    "image=$IMAGE" \
    "command=nmi" >"$METADATA"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/nmi-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-NMI test failed}" >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$METADATA" "$FAILURE_REASON" "$artifact_dir/"
    echo "NMI test artifacts: $artifact_dir" >&2
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
        else
            :
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
    -machine "$QEMU_MACHINE" -m 512M -cpu "$QEMU_CPU" \
    -display none -cdrom "$IMAGE" \
    -serial "file:$SERIAL_LOG" \
    -monitor "unix:$MONITOR_SOCKET,server,nowait" \
    -d int,guest_errors -D "$QEMU_LOG" \
    -no-reboot -no-shutdown >/dev/null 2>&1 &
QEMU_PID=$!

# Phase 1: boot + send NMI via monitor
monitor_output=
if ! monitor_output=$(python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" "$MONITOR_LOG" \
    "$SERIAL_OFFSET_FILE" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
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
socket_path = pathlib.Path(sys.argv[2])
monitor_log = pathlib.Path(sys.argv[3])
offset_path = pathlib.Path(sys.argv[4])
deadline = time.monotonic() + 10.0
monitor_log.write_text("", encoding="utf-8")

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
        'deadline=10s\n')
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

    # Verify TR selector via QEMU monitor
    monitor.sendall(b"info registers\n")
    record(receive_prompt())

    offset_path.write_text(str(len(serial_bytes())), encoding="ascii")

    # Send NMI
    monitor.sendall(b"nmi\n")
    record(receive_prompt())
PY
); then
    FAILURE_MESSAGE=$monitor_output
    printf '%s\n' "$monitor_output" >&2
    exit 1
fi

SERIAL_OFFSET=$(<"$SERIAL_OFFSET_FILE")
printf '%s\n' "serial_offset=$SERIAL_OFFSET" >>"$METADATA"

# Phase 2: validation
validation_output=
if ! validation_output=$(python3 - "$SERIAL_LOG" "$SUFFIX_LOG" "$QEMU_LOG" \
    "$SERIAL_OFFSET" "$QEMU_PID" "$MONITOR_LOG" \
    "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import os
import pathlib
import re
import sys
import time


def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])
serial = pathlib.Path(sys.argv[1])
suffix_path = pathlib.Path(sys.argv[2])
qemu_log = pathlib.Path(sys.argv[3])
offset = int(sys.argv[4])
qemu_pid = int(sys.argv[5])
monitor_log = pathlib.Path(sys.argv[6])
deadline = time.monotonic() + 10.0

frame_re = re.compile(
    r"(?m)^\s*vector=2\s+error=0x0+\s+rip=0x([0-9a-fA-F]+)"
)
nmi_re = re.compile(
    r"(?m)nmi:\s+ist=2\s+stack=ok"
)

def read_suffix():
    data = serial.read_bytes()
    suffix = data[offset:]
    suffix_path.write_bytes(suffix)
    return SERIAL_VIEW.normalise(suffix.decode("ascii", errors="replace"))

while time.monotonic() < deadline:
    suffix = read_suffix()
    frame_match = frame_re.search(suffix)
    nmi_match = nmi_re.search(suffix)
    if ("*** HFOC KERNEL PANIC ***" in suffix
            and "Non-maskable interrupt" in suffix
            and frame_match
            and nmi_match):
        break
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="structured NMI panic" '
        'deadline=10s\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for structured NMI panic")

rip_text = frame_match.group(1)
rip = int(rip_text, 16)
if rip == 0:
    raise SystemExit("Panic RIP must be non-zero")

full_data = serial.read_bytes()
full = full_data.decode("ascii", errors="replace")
if len(full_data) < offset:
    raise SystemExit("Serial log was truncated after NMI")

# Verify kernel COM1 TR selector string from str instruction.
tr_str_match = re.search(r"(?m)^\[TSS \] TR selector: (0x[0-9a-fA-F]+)", full)
if not tr_str_match:
    raise SystemExit("Kernel COM1 TR selector line not found")
tr_str_val = int(tr_str_match.group(1), 16)
if tr_str_val != 0x28:
    raise SystemExit(
        f"Kernel COM1 TR selector mismatch: expected 0x28, got {tr_str_val:#x}")

# Parse IST1 range from COM1
ist1_match = re.search(
    r"(?m)^\[TSS \] IST1 stack: (0x[0-9a-fA-F]+) \.\. (0x[0-9a-fA-F]+)", full)
if not ist1_match:
    raise SystemExit("Kernel COM1 IST1 stack range not found")
ist1_start = int(ist1_match.group(1), 16)
ist1_end = int(ist1_match.group(2), 16)
if ist1_start >= ist1_end:
    raise SystemExit("IST1 range invalid: start >= end")
if ist1_end - ist1_start != 16384:
    raise SystemExit(
        f"IST1 size mismatch: expected 16384, got {ist1_end - ist1_start}")

# Parse IST2 range from COM1
ist2_match = re.search(
    r"(?m)^\[TSS \] IST2 stack: (0x[0-9a-fA-F]+) \.\. (0x[0-9a-fA-F]+)", full)
if not ist2_match:
    raise SystemExit("Kernel COM1 IST2 stack range not found")
ist2_start = int(ist2_match.group(1), 16)
ist2_end = int(ist2_match.group(2), 16)
if ist2_start >= ist2_end:
    raise SystemExit("IST2 range invalid: start >= end")
if ist2_end - ist2_start != 16384:
    raise SystemExit(
        f"IST2 size mismatch: expected 16384, got {ist2_end - ist2_start}")

# Verify IST1 and IST2 ranges do not overlap
if not (ist1_end <= ist2_start or ist2_end <= ist1_start):
    raise SystemExit("IST1 and IST2 stack ranges overlap")

if full.count("HFOC OS 1.0.0 INITIALISED") != 1:
    raise SystemExit("Expected exactly one HFOC boot signature")
panic_index = suffix.find("*** HFOC KERNEL PANIC ***")
if panic_index >= 0 and "hfoc>" in suffix[panic_index:]:
    raise SystemExit("New hfoc> prompt appeared after NMI")

time.sleep(0.75)
suffix_after_wait = read_suffix()
serial_size_after_wait = serial.stat().st_size

def qemu_is_running():
    try:
        os.kill(qemu_pid, 0)
        state = pathlib.Path(
            f"/proc/{qemu_pid}/stat").read_text(encoding="ascii").split()[2]
        return state != "Z"
    except (OSError, IndexError, FileNotFoundError):
        return False

if not qemu_is_running():
    raise SystemExit("QEMU exited before the observation interval completed")
if suffix_after_wait.count("HFOC OS 1.0.0 INITIALISED") != 0:
    raise SystemExit("A second HFOC boot signature appeared after NMI panic")
panic_after_wait = suffix_after_wait.find("*** HFOC KERNEL PANIC ***")
if panic_after_wait >= 0 and "hfoc>" in suffix_after_wait[panic_after_wait:]:
    raise SystemExit("New hfoc> prompt appeared during the observation interval")

# Verify QEMU interrupt trace contains vector 0x02 (NMI)
trace = qemu_log.read_text(encoding="utf-8", errors="replace")
if not re.search(r"(?:\bv=02\b|new 0x2\b)", trace):
    raise SystemExit("QEMU interrupt trace does not contain NMI vector 0x02")

# Check TR selector 0x28 in QEMU monitor output
mon = monitor_log.read_text(encoding="utf-8", errors="replace")
tr_match = re.search(r"TR\s*=\s*([0-9a-fA-F]+)", mon)
if tr_match:
    tr_val = int(tr_match.group(1), 16)
    if tr_val != 0x28:
        raise SystemExit(
            f"TR selector mismatch: expected 0x28, got {tr_val:#x}")
else:
    raise SystemExit("Could not find TR selector in QEMU monitor output")

print("Found command: nmi")
print("Found panic banner: *** HFOC KERNEL PANIC ***")
print("Exception: Non-maskable interrupt")
print("NMI IST stack: ist=2 stack=ok")
print(f"Kernel COM1 TR selector: 0x{tr_str_val:02x}")
print(f"IST1 range: 0x{ist1_start:x} .. 0x{ist1_end:x} ({ist1_end - ist1_start})")
print(f"IST2 range: 0x{ist2_start:x} .. 0x{ist2_end:x} ({ist2_end - ist2_start})")
print(f"Panic RIP: 0x{rip_text}")
print("Panic vector: 2")
print("Panic error: 0")
print("TR selector: 0x28")
print("QEMU NMI trace: vector 0x02")
print("Boot signatures: 1")
print(f"Serial bytes: {offset} -> {serial_size_after_wait}")
print("New hfoc> prompts after command: 0")
print("QEMU alive after 0.75s observation: yes")
PY
); then
    FAILURE_MESSAGE=$validation_output
    printf '%s\n' "$validation_output" >&2
    exit 1
fi

printf '%s\n' "$validation_output"
if stop_qemu; then
    QEMU_PID=
    echo "QEMU NMI test: OK"
else
    FAILURE_MESSAGE="QEMU did not terminate after quit/TERM/KILL"
    printf '%s\n' "$FAILURE_MESSAGE" >&2
    exit 1
fi
