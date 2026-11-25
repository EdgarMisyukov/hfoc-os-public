#!/usr/bin/env bash
# Test Stage 3a IOAPIC IRQ1 masking proof on q35 with APIC and one vCPU.
# Profile: q35, default CPU, -smp 1.
# Transport: serial log file + Unix HMP monitor; sendkey a/b/c.
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

if [[ "${HFOC_IOAPIC_IRQ1_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_IOAPIC_IRQ1_TEST_TIMEOUT:-90}s" \
        env HFOC_IOAPIC_IRQ1_TEST_INNER=1 "$0" "$@"
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
INTERRUPTS_LOG=$TEST_DIR/interrupts.log
METADATA=$TEST_DIR/metadata.txt
FAILURE_REASON=$TEST_DIR/failure-reason.txt
SERIAL_OFFSET_FILE=$TEST_DIR/serial-offset.txt
TRACE_OFFSET_FILE=$TEST_DIR/trace-offset.txt
MONITOR_SOCKET=$TEST_DIR/monitor.sock
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
    "image=$IMAGE" >"$METADATA"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/ioapic-irq1-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-IOAPIC IRQ1 routing test failed}" >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$INTERRUPTS_LOG" \
        "$METADATA" "$FAILURE_REASON" "$SERIAL_OFFSET_FILE" \
        "$TRACE_OFFSET_FILE" "$artifact_dir/"
    echo "IOAPIC IRQ1 routing test artifacts: $artifact_dir" >&2
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
    -d int -D "$INTERRUPTS_LOG" \
    -no-reboot -no-shutdown >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

# Phase 1: wait for boot and route, then send a, b, c and Enter through HMP.
phase1_output=
if ! phase1_output=$(python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" \
    "$MONITOR_LOG" "$SERIAL_OFFSET_FILE" "$INTERRUPTS_LOG" \
    "$TRACE_OFFSET_FILE" 2>&1 <<'PY'
import pathlib
import re
import socket
import sys
import time

serial_path = pathlib.Path(sys.argv[1])
socket_path = pathlib.Path(sys.argv[2])
monitor_log = pathlib.Path(sys.argv[3])
offset_path = pathlib.Path(sys.argv[4])
interrupts_path = pathlib.Path(sys.argv[5])
trace_offset_path = pathlib.Path(sys.argv[6])
deadline = time.monotonic() + 45.0

def serial_bytes():
    try:
        return serial_path.read_bytes()
    except FileNotFoundError:
        return b""

def serial_text():
    return serial_bytes().decode("ascii", errors="replace")

while time.monotonic() < deadline:
    text = serial_text()
    if ("hfoc> " in text and
            "[IOAPIC] route: irq=1 vector=0x41" in text and
            "[PIC ] IRQ1 masked; IOAPIC vector=0x41 active." in text):
        break
    if "[IOAPIC] route failed: irq=1" in text:
        raise SystemExit("IOAPIC route failed before sendkey")
    if "[PIC ] IRQ1 unmasked; legacy PIC fallback retained." in text:
        raise SystemExit("Stage 3a fallback marker before sendkey")
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic before sendkey")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="boot prompt and Stage 3a success marker" '
        'deadline=45s\n')
    sys.stderr.flush()
    raise SystemExit(
        "timed out waiting for boot prompt and Stage 3a success marker")

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

    def command(value):
        monitor.sendall((value + "\n").encode("ascii"))
        response = receive_prompt()
        record(response)
        return response

    record(receive_prompt())
    pic_response = command("info pic").lower()
    pic0_match = re.search(r"pic0:.*imr=(?:0x)?([0-9a-f]+)", pic_response)
    pic1_match = re.search(r"pic1:.*imr=(?:0x)?([0-9a-f]+)", pic_response)
    if not pic0_match or not pic1_match:
        raise SystemExit("QEMU info pic missing pic0/pic1 imr values")
    pic0_imr = pic0_match.group(1)
    pic1_imr = pic1_match.group(1)
    if pic0_imr != "ff" or pic1_imr != "ff":
        raise SystemExit(
            f"QEMU info pic imr mismatch: pic0={pic0_imr} pic1={pic1_imr}")

    offset_path.write_text(str(len(serial_bytes())), encoding="ascii")
    trace_offset_path.write_text(
        str(len(interrupts_path.read_bytes())), encoding="ascii")

    for command in ("sendkey a", "sendkey b", "sendkey c", "sendkey ret"):
        monitor.sendall((command + "\n").encode("ascii"))
        record(receive_prompt())
        time.sleep(0.08)

print("HMP sendkey sequence: a b c ret")
print("Serial offset saved before sendkey: yes")
print(f"QEMU info pic: pic0 imr={pic0_imr} pic1 imr={pic1_imr}")
print("Trace offset saved before sendkey: yes")
PY
); then
    FAILURE_MESSAGE=$phase1_output
    printf '%s\n' "$phase1_output" >&2
    exit 1
fi

printf '%s\n' "$phase1_output"
SERIAL_OFFSET=$(<"$SERIAL_OFFSET_FILE")
TRACE_OFFSET=$(<"$TRACE_OFFSET_FILE")
printf '%s\n' "serial_offset=$SERIAL_OFFSET" >>"$METADATA"
printf '%s\n' "trace_offset=$TRACE_OFFSET" >>"$METADATA"

if ! kill -0 "$QEMU_PID" 2>/dev/null; then
    FAILURE_MESSAGE="QEMU exited before validation"
    printf '%s\n' "$FAILURE_MESSAGE" >&2
    exit 1
fi

# Phase 2: validate migration-window console input and IOAPIC proof.
validation_output=
if ! validation_output=$(python3 - "$SERIAL_LOG" "$QEMU_PID" "$SERIAL_OFFSET" \
    "$INTERRUPTS_LOG" "$TRACE_OFFSET" \
    2>&1 <<'PY'
import os
import pathlib
import re
import sys
import time

serial = pathlib.Path(sys.argv[1])
qemu_pid = int(sys.argv[2])
offset = int(sys.argv[3])
interrupts_path = pathlib.Path(sys.argv[4])
trace_offset = int(sys.argv[5])
deadline = time.monotonic() + 15.0
prompt = "hfoc> "
marker1 = "[IOAPIC] irq1 vector=0x41"
marker2 = "[IOAPIC] irq1 lapic_eoi"
marker_irq0_1 = "[IOAPIC] irq0 vector=0x40"
marker_irq0_2 = "[IOAPIC] irq0 lapic_eoi"
success_marker = "[PIC ] IRQ1 masked; IOAPIC vector=0x41 active."
fallback_marker = "[PIC ] IRQ1 unmasked; legacy PIC fallback retained."
exact_masks = "[PIC ] Master mask: 0x00000000000000FF; slave mask: 0x00000000000000FF"

def suffix_text(data):
    if offset >= len(data):
        return ""
    if offset > 0 and data[offset - 1] not in (0x0a, 0x0d):
        newline = data.find(b"\n", offset)
        if newline < 0:
            return ""
        return data[newline + 1:].decode("ascii", errors="replace")
    return data[offset:].decode("ascii", errors="replace")

def trace_suffix_text():
    data = interrupts_path.read_bytes()
    if trace_offset >= len(data):
        return ""
    return data[trace_offset:].decode("ascii", errors="replace")

def extract_console_input(text):
    if marker1 not in text:
        raise SystemExit("missing vector 0x41 handler marker")
    if marker2 not in text:
        raise SystemExit("missing LAPIC EOI marker")
    clean = (
        text.replace(marker1 + "\r\n", "")
            .replace(marker2 + "\r\n", "")
            .replace(marker1 + "\n", "")
            .replace(marker2 + "\n", "")
            .replace(marker1, "")
            .replace(marker2, "")
            .replace(marker_irq0_1 + "\r\n", "")
            .replace(marker_irq0_2 + "\r\n", "")
            .replace(marker_irq0_1 + "\n", "")
            .replace(marker_irq0_2 + "\n", "")
            .replace(marker_irq0_1, "")
            .replace(marker_irq0_2, "")
    )
    # Background diagnostic line; the calibration test validates its content.
    clean = re.sub(r"\[LAPIC-TIMER\] [^\r\n]*(?:\r?\n)?", "", clean)
    lines = clean.splitlines()
    nonempty_lines = [line.strip() for line in lines if line.strip()]
    abc_lines = [
        line for line in nonempty_lines
        if line == "UNKNOWN COMMAND: abc. TYPE HELP."
    ]
    if len(abc_lines) != 1:
        raise SystemExit(
            f"expected exactly one console input abc, got {len(abc_lines)}")
    payload = "abc"
    return payload, clean

def qemu_is_running():
    try:
        os.kill(qemu_pid, 0)
        state = pathlib.Path(
            f"/proc/{qemu_pid}/stat").read_text(encoding="ascii").split()[2]
        return state != "Z"
    except (OSError, IndexError, FileNotFoundError):
        return False

console_input = ""
while time.monotonic() < deadline:
    suffix = suffix_text(serial.read_bytes())
    try:
        console_input, clean = extract_console_input(suffix)
        if (clean.count("UNKNOWN COMMAND: abc. TYPE HELP.") == 1 and
                clean.rfind(prompt) > clean.find("UNKNOWN COMMAND: abc")):
            break
    except SystemExit:
        pass
    time.sleep(0.05)
else:
    extract_console_input(suffix_text(serial.read_bytes()))
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="exactly-once abc and new prompt" '
        'deadline=15s\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for exactly-once abc and new prompt")

full = serial.read_bytes().decode("ascii", errors="replace")

trace_deadline = time.monotonic() + 5.0
while time.monotonic() < trace_deadline:
    trace_suffix = trace_suffix_text()
    if "Servicing hardware INT=0x21" in trace_suffix:
        raise SystemExit(
            "Stage 3a sendkey trace contains Servicing hardware INT=0x21")
    if ("Servicing hardware INT=0x41" in trace_suffix and
            "Servicing hardware INT=0x42" in trace_suffix):
        break
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="Stage 3b sendkey INT=0x41/INT=0x42 trace" '
        'deadline=' + str(int(trace_deadline)) + 's\n')
    sys.stderr.flush()
    raise SystemExit(
        "timed out waiting for Stage 3b sendkey INT=0x41/INT=0x42 trace")

if "[IOAPIC] route failed: irq=1" in full:
    raise SystemExit("IOAPIC route failed")
if "*** HFOC KERNEL PANIC ***" in full:
    raise SystemExit("Kernel panic detected")
if fallback_marker in full:
    raise SystemExit("Stage 3a fallback marker detected")
if success_marker not in full:
    raise SystemExit("Missing Stage 3a success marker")

route_match = re.search(
    r"\[IOAPIC\] route: irq=1 vector=0x41 gsi=(\d+) dest=(\d+)", full)
if not route_match:
    raise SystemExit("Missing: [IOAPIC] route: irq=1 vector=0x41 line")
gsi = int(route_match.group(1))
dest = int(route_match.group(2))
if not (0 <= dest <= 255):
    raise SystemExit(f"IOAPIC destination out of range 0..255: {dest}")

mask_match = re.search(
    r"\[PIC \] Master mask: (0x[0-9A-Fa-f]+); slave mask: (0x[0-9A-Fa-f]+)",
    full)
if not mask_match:
    raise SystemExit("Missing: [PIC ] Master mask line")
master_mask = int(mask_match.group(1), 16)
slave_mask = int(mask_match.group(2), 16)
if master_mask != 0xff or slave_mask != 0xff:
    raise SystemExit(
        f"Unexpected PIC masks: master={master_mask:#x} slave={slave_mask:#x}")
if (master_mask & 0x02) == 0:
    raise SystemExit("PIC IRQ1 must be masked")
if exact_masks not in full:
    raise SystemExit("PIC mask line is not exact uppercase Stage 3b FF/FF")

vector_count = suffix.count(marker1)
eoi_count = suffix.count(marker2)
if vector_count < 1 or eoi_count < 1:
    raise SystemExit("IOAPIC vector or LAPIC EOI marker count is zero")
if clean.find("UNKNOWN COMMAND: abc. TYPE HELP.") < 0:
    raise SystemExit("Missing unknown-command output after abc")
if clean.rfind(prompt) < 0:
    raise SystemExit("Missing new hfoc> prompt")

if not qemu_is_running():
    raise SystemExit("QEMU exited before validation completed")
time.sleep(0.3)
if not qemu_is_running():
    raise SystemExit("QEMU exited during observation interval")

print(f"IOAPIC GSI: {gsi}")
print(f"IOAPIC destination: {dest}")
print(f"Vector: 0x41")
print(f"IOAPIC vector markers: {vector_count}")
print(f"LAPIC EOI markers: {eoi_count}")
print("Console input exactly once: abc")
print("Trace suffix: INT=0x41 found; INT=0x42 found; INT=0x21 absent")
print(f"PIC masks after init: master={master_mask:#x} slave={slave_mask:#x}")
print("PIC IRQ1 masked as Stage 3a success: yes")
print("Kernel panic: none")
print("IOAPIC route failure: none")
print("QEMU alive: yes")
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
echo "QEMU alive before quit: yes"

if successful_shutdown; then
    QEMU_PID=
else
    printf '%s\n' "$FAILURE_MESSAGE" >&2
    exit 1
fi

echo "QEMU IOAPIC IRQ1 routing test: OK"
