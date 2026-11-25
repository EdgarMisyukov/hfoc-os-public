#!/usr/bin/env bash
# Test verified IOAPIC IRQ3-7 routing with five real 16550 UART receive events.
# QEMU supports only four ISA serial ports, so each IRQ is proven in a separate
# QEMU run with one real UART event.
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

if [[ "${HFOC_IOAPIC_IRQ3_7_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_IOAPIC_IRQ3_7_TEST_TIMEOUT:-240}s" \
        env HFOC_IOAPIC_IRQ3_7_TEST_INNER=1 "$0" "$@"
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

TEST_DIR=
SERIAL_LOG=
MONITOR_LOG=
QEMU_LOG=
INTERRUPTS_LOG=
METADATA=
FAILURE_REASON=
SERIAL_OFFSET_FILE=
TRACE_OFFSET_FILE=
MONITOR_SOCKET=
CASE_SOCKET=
CASE_NAME=
QEMU_PID=
FAILURE_MESSAGE=
QEMU_WAIT_RESULT=

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/ioapic-irq3-7-${CASE_NAME:-all}-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-IOAPIC IRQ3-7 UART routing test failed}" \
        >"$FAILURE_REASON"
    for file in "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$INTERRUPTS_LOG" \
                "$METADATA" "$FAILURE_REASON" "$SERIAL_OFFSET_FILE" \
                "$TRACE_OFFSET_FILE"; do
        if [[ -f "$file" ]]; then
            cp -a "$file" "$artifact_dir/"
        fi
    done
    echo "IOAPIC IRQ3-7 UART routing test artifacts: $artifact_dir" >&2
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

monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    monitor.settimeout(1.0)
    connect_deadline = time.monotonic() + 5.0
    while True:
        try:
            monitor.connect(sys.argv[1])
            break
        except OSError as exc:
            if time.monotonic() >= connect_deadline:
                raise SystemExit(f"QEMU monitor did not accept quit: {exc}")
            time.sleep(0.05)

    prompt_deadline = time.monotonic() + 5.0
    prompt = b""
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

    try:
        monitor.sendall(b"quit\n")
    except OSError as exc:
        raise SystemExit(f"QEMU monitor did not accept quit: {exc}")

    monitor.settimeout(5.0)
    while True:
        try:
            chunk = monitor.recv(4096)
        except socket.timeout:
            raise SystemExit("QEMU monitor EOF timeout after quit")
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
    local rc=$?
    trap - EXIT INT TERM
    set +e
    if (( rc != 0 )); then
        best_effort_cleanup
        if [[ -n "$SERIAL_LOG" && -f "$SERIAL_LOG" ]]; then
            save_failure_artifacts
        fi
    fi
    if [[ -n "$TEST_DIR" ]]; then
        rm -rf "$TEST_DIR"
    fi
    exit "$rc"
}

trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

run_case() {
    local irq=$1
    local iobase=$2
    local use_com1_socket=$3
    local char_value=$4
    local vector
    local pic_vector
    vector=$(printf '0x%02X' $((0x41 + irq)))
    pic_vector=$(printf '0x%02X' $((0x23 + irq)))

    CASE_NAME="irq${irq}"
    TEST_DIR=$(mktemp -d)
    SERIAL_LOG=$TEST_DIR/com1.log
    MONITOR_LOG=$TEST_DIR/monitor.log
    QEMU_LOG=$TEST_DIR/qemu.log
    INTERRUPTS_LOG=$TEST_DIR/interrupts.log
    METADATA=$TEST_DIR/metadata.txt
    FAILURE_REASON=$TEST_DIR/failure-reason.txt
    SERIAL_OFFSET_FILE=$TEST_DIR/serial-offset.txt
    TRACE_OFFSET_FILE=$TEST_DIR/trace-offset.txt
    MONITOR_SOCKET=$TEST_DIR/monitor.sock
    CASE_SOCKET=$TEST_DIR/case.sock
    QEMU_PID=
    FAILURE_MESSAGE=

    : >"$SERIAL_LOG"
    : >"$MONITOR_LOG"
    : >"$QEMU_LOG"
    : >"$INTERRUPTS_LOG"

    printf '%s\n' \
        "machine=$QEMU_MACHINE" \
        "cpu=default" \
        "smp=$QEMU_SMP" \
        "image=$IMAGE" \
        "case=IRQ${irq}" >"$METADATA"

    if [[ "$use_com1_socket" == yes ]]; then
        qemu-system-x86_64 \
            -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" \
            -display none -cdrom "$IMAGE" \
            -chardev socket,id=com1,path="$CASE_SOCKET",server=on,wait=off,logfile="$SERIAL_LOG",logappend=off \
            -serial chardev:com1 \
            -monitor "unix:$MONITOR_SOCKET,server,nowait" \
            -d int -D "$INTERRUPTS_LOG" \
            -no-reboot -no-shutdown >"$QEMU_LOG" 2>&1 &
    else
        qemu-system-x86_64 \
            -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" \
            -display none -cdrom "$IMAGE" \
            -serial "file:$SERIAL_LOG" \
            -chardev socket,id=case_sock,path="$CASE_SOCKET",server=on,wait=off \
            -device isa-serial,chardev=case_sock,iobase="$iobase",irq="$irq" \
            -monitor "unix:$MONITOR_SOCKET,server,nowait" \
            -d int -D "$INTERRUPTS_LOG" \
            -no-reboot -no-shutdown >"$QEMU_LOG" 2>&1 &
    fi
    QEMU_PID=$!

    local phase1_output
    if ! phase1_output=$(python3 - "$SERIAL_LOG" "$MONITOR_LOG" \
        "$MONITOR_SOCKET" "$SERIAL_OFFSET_FILE" "$TRACE_OFFSET_FILE" \
        "$CASE_SOCKET" \
        "$char_value" "$vector" "$ROOT/scripts/hfoc-serial-view.py" \
        2>&1 <<'PY'
import pathlib
import re
import socket
import sys
import time

serial_path = pathlib.Path(sys.argv[1])
monitor_log = pathlib.Path(sys.argv[2])
monitor_path = pathlib.Path(sys.argv[3])
offset_path = pathlib.Path(sys.argv[4])
trace_offset_path = pathlib.Path(sys.argv[5])
socket_path = pathlib.Path(sys.argv[6])
char_value = sys.argv[7]
vector = sys.argv[8]
deadline = time.monotonic() + 45.0

masked_markers = [
    "[ISA ] IRQ3 masked; IOAPIC vector=0x44 active.",
    "[ISA ] IRQ4 masked; IOAPIC vector=0x45 active.",
    "[ISA ] IRQ5 masked; IOAPIC vector=0x46 active.",
    "[ISA ] IRQ6 masked; IOAPIC vector=0x47 active.",
    "[ISA ] IRQ7 masked; IOAPIC vector=0x48 active.",
]

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

while time.monotonic() < deadline:
    text = serial_text()
    if (all(marker in text for marker in masked_markers)
            and "[ISA ] IRQ3-7 UART receive interrupts enabled." in text
            and "hfoc> " in text
            and "[PIC ] disabled; APIC runtime active." in text):
        break
    if any(f"[ISA ] IRQ{irq} unmasked; legacy PIC fallback retained." in text
           for irq in (3, 4, 5, 6, 7)):
        raise SystemExit("IRQ3-7 fallback marker detected before UART proof")
    if "[IOAPIC] route failed: irq=" in text:
        raise SystemExit("IOAPIC route failure before UART proof")
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic before UART proof")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="IRQ3-7 routing readiness" '
        'deadline=45s\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for IRQ3-7 routing readiness")

while not monitor_path.exists():
    if time.monotonic() >= deadline:
        raise SystemExit("timed out waiting for QEMU monitor socket")
    time.sleep(0.05)

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
    monitor.settimeout(1.0)

    def connect_with_retry(path):
        while True:
            try:
                monitor.connect(path)
                return
            except OSError as exc:
                if time.monotonic() >= deadline:
                    raise SystemExit(
                        f"QEMU monitor did not accept connection: {exc}")
                time.sleep(0.05)

    connect_with_retry(str(monitor_path))

    def receive_prompt():
        chunks = []
        prompt_deadline = time.monotonic() + 5.0
        while b"(qemu)" not in b"".join(chunks):
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
            chunks.append(chunk)
        return b"".join(chunks).decode("utf-8", errors="replace")

    def command(value):
        monitor.sendall((value + "\n").encode("ascii"))
        return receive_prompt()

    prompt = receive_prompt()
    pic_response = command("info pic").lower()
    with open(monitor_log, "a", encoding="utf-8") as output:
        output.write(prompt)
        output.write(pic_response)
    pic0_match = re.search(r"pic0:.*imr=(?:0x)?([0-9a-f]+)", pic_response)
    pic1_match = re.search(r"pic1:.*imr=(?:0x)?([0-9a-f]+)", pic_response)
    if not pic0_match or not pic1_match:
        raise SystemExit("QEMU info pic missing pic0/pic1 imr values")
    if pic0_match.group(1) != "ff" or pic1_match.group(1) != "ff":
        raise SystemExit(
            f"QEMU info pic imr mismatch: pic0={pic0_match.group(1)} "
            f"pic1={pic1_match.group(1)}")

while not socket_path.exists():
    if time.monotonic() >= deadline:
        raise SystemExit(f"timed out waiting for serial socket {socket_path}")
    time.sleep(0.05)

offset_path.write_text(str(len(serial_path.read_bytes())), encoding="ascii")
trace_offset_path.write_text(
    str(len(pathlib.Path(sys.argv[1]).parent.joinpath(
        "interrupts.log").read_bytes())),
    encoding="ascii")

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as uart:
    uart.settimeout(2.0)

    def connect_uart_with_retry(path):
        while True:
            try:
                uart.connect(path)
                return
            except OSError as exc:
                if time.monotonic() >= deadline:
                    raise SystemExit(
                        f"QEMU serial socket did not accept connection: {exc}")
                time.sleep(0.05)

    connect_uart_with_retry(str(socket_path))
    uart.sendall(char_value.encode("ascii"))
    time.sleep(0.2)

time.sleep(0.5)
print(f"UART socket sent: {char_value}")
print("Serial offset saved before UART proof: yes")
print("Trace offset saved before UART proof: yes")
print("QEMU info pic: pic0 imr=ff pic1 imr=ff")
PY
); then
        FAILURE_MESSAGE=$phase1_output
        printf '%s\n' "$phase1_output" >&2
        return 1
    fi

    printf '%s\n' "$phase1_output"
    local serial_offset
    local trace_offset
    serial_offset=$(<"$SERIAL_OFFSET_FILE")
    trace_offset=$(<"$TRACE_OFFSET_FILE")
    printf '%s\n' "serial_offset=$serial_offset" >>"$METADATA"
    printf '%s\n' "trace_offset=$trace_offset" >>"$METADATA"

    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        FAILURE_MESSAGE="QEMU exited before UART validation"
        printf '%s\n' "$FAILURE_MESSAGE" >&2
        return 1
    fi

    local validation_output
    if ! validation_output=$(python3 - "$SERIAL_LOG" "$INTERRUPTS_LOG" \
        "$serial_offset" "$trace_offset" "$char_value" "$vector" \
        "$pic_vector" "$QEMU_PID" "$ROOT/scripts/hfoc-serial-view.py" \
        2>&1 <<'PY'
import os
import pathlib
import sys
import time

serial_path = pathlib.Path(sys.argv[1])
interrupts_path = pathlib.Path(sys.argv[2])
serial_offset = int(sys.argv[3])
trace_offset = int(sys.argv[4])
char_value = sys.argv[5]
vector = sys.argv[6]
pic_vector = sys.argv[7]
qemu_pid = int(sys.argv[8])
deadline = time.monotonic() + 20.0
serial_marker = f"[UART] irq={int(vector, 16) - 0x44 + 3} " \
                f"vector={vector} char={char_value}"
trace_marker = f"Servicing hardware INT={vector}"

def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])

def suffix(data, offset):
    if offset >= len(data):
        return ""
    if offset > 0 and data[offset - 1] not in (0x0a, 0x0d):
        newline = data.find(b"\n", offset)
        if newline < 0:
            return ""
        return data[newline + 1:].decode("ascii", errors="replace")
    return data[offset:].decode("ascii", errors="replace")

# The byte offset stays raw (ITEM-527); only the text cut at it is normalised.
def serial_suffix_text():
    return SERIAL_VIEW.normalise(
        suffix(serial_path.read_bytes(), serial_offset))

def trace_suffix():
    return suffix(interrupts_path.read_bytes(), trace_offset)

def qemu_is_running():
    try:
        os.kill(qemu_pid, 0)
        state = pathlib.Path(
            f"/proc/{qemu_pid}/stat").read_text(encoding="ascii").split()[2]
        return state != "Z"
    except (OSError, IndexError, FileNotFoundError):
        return False

while time.monotonic() < deadline:
    text = serial_suffix_text()
    trace = trace_suffix()
    if serial_marker in text and trace_marker in trace:
        break
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="' +
        str(f"timed out waiting for {serial_marker} and {trace_marker}") + '" '
        'deadline=20s\n')
    sys.stderr.flush()
    raise SystemExit(
        f"timed out waiting for {serial_marker} and {trace_marker}")

text = serial_suffix_text()
trace = trace_suffix()
if serial_marker not in text:
    raise SystemExit(f"Missing serial marker: {serial_marker}")
if trace_marker not in trace:
    raise SystemExit(f"Missing trace marker: {trace_marker}")
if f"Servicing hardware INT={pic_vector}" in trace:
    raise SystemExit(f"PIC trace marker present in UART proof: {pic_vector}")
full = SERIAL_VIEW.normalise(
    serial_path.read_bytes().decode("ascii", errors="replace"))
if "*** HFOC KERNEL PANIC ***" in full:
    raise SystemExit("Kernel panic detected")
if "[IOAPIC] route failed: irq=" in full:
    raise SystemExit("IOAPIC route failure detected")
if any(f"[ISA ] IRQ{irq} unmasked; legacy PIC fallback retained." in full
       for irq in (3, 4, 5, 6, 7)):
    raise SystemExit("IRQ3-7 fallback marker detected")
if not qemu_is_running():
    raise SystemExit("QEMU exited before validation completed")

print(f"UART serial marker: {serial_marker}")
print(f"Trace marker: {trace_marker} found; {pic_vector} absent")
print("Kernel panic: none")
print("IOAPIC route failure: none")
print("IRQ3-7 fallback marker: none")
print("QEMU alive: yes")
PY
); then
        FAILURE_MESSAGE=$validation_output
        printf '%s\n' "$validation_output" >&2
        return 1
    fi

    printf '%s\n' "$validation_output"
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        FAILURE_MESSAGE="QEMU exited before validation completed"
        printf '%s\n' "$FAILURE_MESSAGE" >&2
        return 1
    fi
    echo "QEMU alive before quit: yes"

    if ! successful_shutdown; then
        printf '%s\n' "$FAILURE_MESSAGE" >&2
        return 1
    fi
    QEMU_PID=
    rm -rf "$TEST_DIR"
    TEST_DIR=
    SERIAL_LOG=
    MONITOR_LOG=
    QEMU_LOG=
    INTERRUPTS_LOG=
    METADATA=
    FAILURE_REASON=
    SERIAL_OFFSET_FILE=
    TRACE_OFFSET_FILE=
    MONITOR_SOCKET=
    CASE_SOCKET=
    echo "QEMU IOAPIC IRQ${irq} UART routing case: OK"
}

run_case 3 0x2f8 no A
run_case 4 0x3f8 yes B
run_case 5 0x3e8 no C
run_case 6 0x2e8 no D
run_case 7 0x278 no E

echo "QEMU IOAPIC IRQ3-7 UART routing test: OK"
