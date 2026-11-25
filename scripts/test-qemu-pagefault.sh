#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in python3 qemu-system-x86_64 timeout mktemp; do
    command -v "$command" >/dev/null || {
        echo "Не найдена команда: $command" >&2
        exit 1
    }
done

if [[ "${HFOC_PAGEFAULT_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM "${HFOC_PAGEFAULT_TEST_TIMEOUT:-30}s" \
        env HFOC_PAGEFAULT_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_CPU=${QEMU_CPU:-max,-apic}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}
EXPECTED_PF_VECTOR=${EXPECTED_PF_VECTOR:-14}
EXPECTED_PF_ERROR=${EXPECTED_PF_ERROR:-2}
EXPECTED_PF_CR2=${EXPECTED_PF_CR2:-0x0000400000000000}
[[ "$EXPECTED_PF_VECTOR" =~ ^[0-9]+$ ]] || { echo "Invalid EXPECTED_PF_VECTOR: $EXPECTED_PF_VECTOR" >&2; exit 1; }
[[ "$EXPECTED_PF_ERROR" =~ ^[0-9]+$ ]] || { echo "Invalid EXPECTED_PF_ERROR: $EXPECTED_PF_ERROR" >&2; exit 1; }
[[ "$EXPECTED_PF_CR2" =~ ^(0[xX][0-9a-fA-F]+|[0-9]+)$ ]] || { echo "Invalid EXPECTED_PF_CR2: $EXPECTED_PF_CR2" >&2; exit 1; }

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
    "expected_vector=$EXPECTED_PF_VECTOR" \
    "expected_error=$EXPECTED_PF_ERROR" \
    "expected_cr2=$EXPECTED_PF_CR2" \
    "command=pagefault" >"$METADATA"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/pagefault-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-page-fault test failed}" >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$METADATA" "$FAILURE_REASON" "$artifact_dir/"
    echo "Page-fault test artifacts: $artifact_dir" >&2
}

wait_for_exit() {
    local deadline=$((SECONDS + 1))
    while kill -0 "$QEMU_PID" 2>/dev/null; do
        (( SECONDS >= deadline )) && return 1
        sleep 0.1
    done
    wait "$QEMU_PID" 2>/dev/null || true
}

stop_qemu() {
    local deadline
    if [[ -z "$QEMU_PID" ]] || ! kill -0 "$QEMU_PID" 2>/dev/null; then
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
    deadline=$((SECONDS + 1))
    while kill -0 "$QEMU_PID" 2>/dev/null && (( SECONDS < deadline )); do sleep 0.1; done
    if kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
    fi
    deadline=$((SECONDS + 1))
    while kill -0 "$QEMU_PID" 2>/dev/null && (( SECONDS < deadline )); do sleep 0.1; done
    if kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -KILL "$QEMU_PID" 2>/dev/null || true
    fi
    wait_for_exit || true
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    stop_qemu
    if (( status == 0 )); then
        rm -rf "$TEST_DIR"
    else
        save_failure_artifacts
        rm -rf "$TEST_DIR"
    fi
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

monitor_output=
if ! monitor_output=$(python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" "$MONITOR_LOG" "$SERIAL_OFFSET_FILE" \
    "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
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
    offset_path.write_text(str(len(serial_bytes())), encoding="ascii")
    for key in "pagefault":
        monitor.sendall(f"sendkey {key}\n".encode("ascii"))
        record(receive_prompt())
        time.sleep(0.06)
    monitor.sendall(b"sendkey ret\n")
    record(receive_prompt())
PY
); then
    FAILURE_MESSAGE=$monitor_output
    printf '%s\n' "$monitor_output" >&2
    exit 1
fi

SERIAL_OFFSET=$(<"$SERIAL_OFFSET_FILE")
printf '%s\n' "serial_offset=$SERIAL_OFFSET" >>"$METADATA"
validation_output=
if ! validation_output=$(python3 - "$SERIAL_LOG" "$SUFFIX_LOG" "$QEMU_LOG" "$SERIAL_OFFSET" "$QEMU_PID" \
    "$EXPECTED_PF_VECTOR" "$EXPECTED_PF_ERROR" "$EXPECTED_PF_CR2" \
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
expected_vector = int(sys.argv[6])
expected_error = int(sys.argv[7])
expected_cr2 = int(sys.argv[8], 0)
deadline = time.monotonic() + 10.0
frame_re = re.compile(
    r"(?m)^\s*vector=(\d+)\s+error=0x([0-9a-fA-F]+)\s+"
    r"rip=0x([0-9a-fA-F]+)\s+cr2=0x([0-9a-fA-F]+)"
)
decode_re = re.compile(
    r"(?m)^\s*page-fault: cause=(\S+) access=(\S+) mode=(\S+) "
    r"reserved=([01]) instruction=([01])\r?$"
)

def read_suffix():
    data = serial.read_bytes()
    suffix = data[offset:]
    suffix_path.write_bytes(suffix)
    return SERIAL_VIEW.normalise(suffix.decode("ascii", errors="replace"))

while time.monotonic() < deadline:
    suffix = read_suffix()
    frame_match = frame_re.search(suffix)
    decode_match = decode_re.search(suffix)
    # ITEM-482: the one-character-at-a-time echo can be split by a periodic
    # record; strip whole records before looking for the typed command name.
    typed = re.sub(r"\[[A-Z][A-Z0-9 _-]*\][^\r\n]*\r?\n", "", suffix)
    if ("pagefault" in typed and "TRIGGERING PAGE FAULT..." in suffix
            and "*** HFOC KERNEL PANIC ***" in suffix
            and "Page fault" in suffix and frame_match and decode_match):
        break
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="structured page-fault panic" '
        'deadline=10s\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for structured page-fault panic")

vector = int(frame_match.group(1), 10)
error = int(frame_match.group(2), 16)
rip_text = frame_match.group(3)
rip = int(rip_text, 16)
cr2_text = frame_match.group(4)
cr2 = int(cr2_text, 16)
if vector != expected_vector:
    raise SystemExit(f"Panic vector mismatch: expected {expected_vector}, got {vector}")
if error != expected_error:
    raise SystemExit(f"Panic error mismatch: expected {expected_error}, got {error}")
if rip == 0:
    raise SystemExit("Panic RIP must be non-zero")
if cr2 != expected_cr2:
    raise SystemExit(f"Panic CR2 mismatch: expected {expected_cr2:#x}, got {cr2:#x}")

cause, access, mode, reserved, instruction = decode_match.groups()
expected_decode = (
    "protection-violation" if error & 0x01 else "not-present",
    "write" if error & 0x02 else "read",
    "user" if error & 0x04 else "supervisor",
    "1" if error & 0x08 else "0",
    "1" if error & 0x10 else "0",
)
actual_decode = (cause, access, mode, reserved, instruction)
if actual_decode != expected_decode:
    raise SystemExit(
        "Page-fault decode mismatch: "
        f"expected {expected_decode}, got {actual_decode}"
    )

full_data = serial.read_bytes()
full = SERIAL_VIEW.normalise(full_data.decode("ascii", errors="replace"))
if len(full_data) < offset:
    raise SystemExit("Serial log was truncated after pagefault")
if full.count("HFOC OS 1.0.0 INITIALISED") != 1:
    raise SystemExit("Expected exactly one HFOC boot signature")
panic_index = suffix.find("*** HFOC KERNEL PANIC ***")
if panic_index >= 0 and "hfoc>" in suffix[panic_index:]:
    raise SystemExit("New hfoc> prompt appeared after pagefault")

time.sleep(0.75)
suffix_after_wait = read_suffix()
serial_size_after_wait = serial.stat().st_size

def qemu_is_running():
    try:
        os.kill(qemu_pid, 0)
        state = pathlib.Path(f"/proc/{qemu_pid}/stat").read_text(encoding="ascii").split()[2]
        return state != "Z"
    except (OSError, IndexError, FileNotFoundError):
        return False

if not qemu_is_running():
    raise SystemExit("QEMU exited before the observation interval completed")
if suffix_after_wait.count("HFOC OS 1.0.0 INITIALISED") != 0:
    raise SystemExit("A second HFOC boot signature appeared after panic")
panic_after_wait = suffix_after_wait.find("*** HFOC KERNEL PANIC ***")
if panic_after_wait >= 0 and "hfoc>" in suffix_after_wait[panic_after_wait:]:
    raise SystemExit("New hfoc> prompt appeared during the observation interval")

trace = qemu_log.read_text(encoding="utf-8", errors="replace")
if not re.search(r"(?:\bv=0e\b|new 0xe\b)", trace):
    raise SystemExit("QEMU interrupt trace does not contain CPU page fault vector 0x0e")

print("Found command: pagefault")
print("Found trigger: TRIGGERING PAGE FAULT...")
print("Found panic banner: *** HFOC KERNEL PANIC ***")
print("Exception: Page fault")
print(f"Panic vector: {vector}")
print(f"Panic error: {error}")
print(f"Panic RIP: 0x{rip_text}")
print(f"Panic CR2: 0x{cr2_text}")
print(f"Page-fault cause: {cause}")
print(f"Page-fault access: {access}")
print(f"Page-fault mode: {mode}")
print(f"Page-fault reserved violation: {'yes' if reserved == '1' else 'no'}")
print(f"Page-fault instruction fetch: {'yes' if instruction == '1' else 'no'}")
print("QEMU #PF trace: vector 0x0e")
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
stop_qemu
QEMU_PID=
echo "QEMU page-fault panic test: OK"
