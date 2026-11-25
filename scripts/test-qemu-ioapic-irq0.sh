#!/usr/bin/env bash
# Test Stage 2 IOAPIC IRQ0 shadow routing on q35 with APIC and one vCPU.
# Profile: q35, default CPU, -smp 1.
# Transport: serial log file + Unix HMP monitor; sendkey ticks twice.
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

if [[ "${HFOC_IOAPIC_IRQ0_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_IOAPIC_IRQ0_TEST_TIMEOUT:-90}s" \
        env HFOC_IOAPIC_IRQ0_TEST_INNER=1 "$0" "$@"
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
    artifact_dir="build/test-artifacts/ioapic-irq0-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-IOAPIC IRQ0 routing test failed}" >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$INTERRUPTS_LOG" \
        "$METADATA" "$FAILURE_REASON" "$TRACE_OFFSET_FILE" "$artifact_dir/"
    echo "IOAPIC IRQ0 routing test artifacts: $artifact_dir" >&2
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

validation_output=
if ! validation_output=$(python3 - "$SERIAL_LOG" "$MONITOR_LOG" \
    "$INTERRUPTS_LOG" "$MONITOR_SOCKET" "$TRACE_OFFSET_FILE" 2>&1 <<'PY'
import pathlib
import re
import socket
import sys
import time

serial_path = pathlib.Path(sys.argv[1])
monitor_log = pathlib.Path(sys.argv[2])
interrupts_path = pathlib.Path(sys.argv[3])
monitor_socket = pathlib.Path(sys.argv[4])
trace_offset_path = pathlib.Path(sys.argv[5])
deadline = time.monotonic() + 45.0
route_re = re.compile(
    r"\[IOAPIC\] route: irq=0 vector=0x40 gsi=(\d+) dest=(\d+)")

marker_vector = "[IOAPIC] irq0 vector=0x40"
marker_eoi = "[IOAPIC] irq0 lapic_eoi"

def readiness_complete(text):
    return (
        "hfoc> " in text
        and route_re.search(text) is not None
        and marker_vector in text
        and marker_eoi in text
    )

fixture_prefix = (
    "hfoc> \n"
    "[IOAPIC] route: irq=0 vector=0x40 gsi=0 dest=0\n"
)
assert not readiness_complete(fixture_prefix)
assert not readiness_complete(fixture_prefix + marker_vector + "\n")
assert not readiness_complete(fixture_prefix + marker_eoi + "\n")
assert readiness_complete(
    fixture_prefix + marker_vector + "\n" + marker_eoi + "\n")

def serial_text():
    try:
        return serial_path.read_bytes().decode("ascii", errors="replace")
    except FileNotFoundError:
        return ""

def tick_values(text):
    # Interrupt markers can split "TICKS: (\d+)"; remove them before parsing.
    clean = text
    for marker in (
        "[IOAPIC] irq0 vector=0x40",
        "[IOAPIC] irq0 lapic_eoi",
        "[IOAPIC] irq1 vector=0x41",
        "[IOAPIC] irq1 lapic_eoi",
        "[LAPIC-TIMER] irq vector=0x42",
        "[LAPIC-TIMER] irq lapic_eoi",
    ):
        clean = (
            clean.replace(marker + "\r\n", "")
                .replace(marker + "\n", "")
                .replace(marker, "")
        )
    return [int(value) for value in re.findall(r"TICKS:\s*(\d+)", clean)]

while time.monotonic() < deadline:
    text = serial_text()
    if readiness_complete(text):
        break
    if "[IOAPIC] route failed: irq=0" in text:
        raise SystemExit("IOAPIC route failed before validation")
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic before validation")
    if "[LAPIC] LINT0 failed: status=" in text:
        raise SystemExit("LINT0 failure before validation")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="boot prompt and IOAPIC IRQ0 route" '
        'deadline=45s\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for boot prompt and IOAPIC IRQ0 route")

text = serial_text()
route_match = route_re.search(text)
if not route_match:
    raise SystemExit("Missing: [IOAPIC] route: irq=0 vector=0x40 line")
gsi = int(route_match.group(1))
dest = int(route_match.group(2))
if not (0 <= dest <= 255):
    raise SystemExit(f"IOAPIC destination out of range 0..255: {dest}")

if "[IOAPIC] route failed: irq=0" in text:
    raise SystemExit("IOAPIC route failed marker detected")
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("Kernel panic detected")
if "[LAPIC] LINT0 failed: status=" in text:
    raise SystemExit("LINT0 failure marker detected")

marker_vector = "[IOAPIC] irq0 vector=0x40"
marker_eoi = "[IOAPIC] irq0 lapic_eoi"
if marker_vector not in text:
    raise SystemExit("Missing: [IOAPIC] irq0 vector=0x40")
if marker_eoi not in text:
    raise SystemExit("Missing: [IOAPIC] irq0 lapic_eoi")

mask_match = re.search(
    r"\[PIC \] Master mask: (0x[0-9A-Fa-f]+); slave mask: (0x[0-9A-Fa-f]+)",
    text)
if not mask_match:
    raise SystemExit("Missing: [PIC ] Master mask line")
master_mask = int(mask_match.group(1), 16)
slave_mask = int(mask_match.group(2), 16)
if master_mask != 0xff or slave_mask != 0xff:
    raise SystemExit(
        f"Unexpected PIC masks: master={master_mask:#x} slave={slave_mask:#x}")
stage3b_mask_line = "[PIC ] Master mask: 0x00000000000000FF; slave mask: 0x00000000000000FF"
if stage3b_mask_line not in text:
    raise SystemExit("Missing exact uppercase Stage 3b COM1 mask line")

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

    def record(value):
        with monitor_log.open("a", encoding="utf-8") as output:
            output.write(value)

    def command(value):
        monitor.sendall((value + "\n").encode("ascii"))
        response = receive_prompt()
        record(response)
        return response

    def send_keys(keys):
        for key in keys:
            command(f"sendkey {key}")
            time.sleep(0.08)

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

    ticks = []
    for snapshot in (1, 2):
        send_keys(("t", "i", "c", "k", "s", "ret"))
        while time.monotonic() < deadline:
            matches = tick_values(serial_text())
            if len(matches) >= snapshot:
                ticks.append(int(matches[snapshot - 1]))
                break
            time.sleep(0.05)
        else:
            # ITEM-422: a proof that gives up on a marker says so.
            sys.stderr.write(
                'HFOC-QEMU-MARKER-TIMEOUT marker="' +
                str(f"timed out waiting for TICKS snapshot {snapshot}") + '" '
                'deadline=45s\n')
            sys.stderr.flush()
            raise SystemExit(f"timed out waiting for TICKS snapshot {snapshot}")
        if snapshot == 1:
            trace_offset_path.write_text(
                str(len(interrupts_path.read_bytes())), encoding="ascii")
            time.sleep(2.0)

if ticks[1] <= ticks[0]:
    raise SystemExit(f"Runtime ticks did not grow: {ticks[0]} -> {ticks[1]}")

try:
    interrupts_bytes = interrupts_path.read_bytes()
except FileNotFoundError:
    raise SystemExit("Missing QEMU interrupt trace")
trace_offset = int(trace_offset_path.read_text(encoding="ascii"))
trace_suffix = interrupts_bytes[trace_offset:].decode(
    encoding="ascii", errors="replace")
if "Servicing hardware INT=0x42" not in trace_suffix:
    raise SystemExit("Missing in suffix: Servicing hardware INT=0x42")
if "Servicing hardware INT=0x40" in trace_suffix:
    raise SystemExit("Suffix unexpectedly contains Servicing hardware INT=0x40")
if "Servicing hardware INT=0x20" in trace_suffix:
    raise SystemExit("Suffix unexpectedly contains Servicing hardware INT=0x20")

with monitor_log.open("a", encoding="utf-8") as output:
    output.write("validation complete\n")

print(f"IOAPIC GSI: {gsi}")
print(f"IOAPIC destination: {dest}")
print("Route marker: [IOAPIC] route: irq=0 vector=0x40")
print("IRQ0 handler markers: found")
print("Trace suffix: INT=0x42 found; INT=0x40 and INT=0x20 absent")
print(f"TICKS before: {ticks[0]}")
print(f"TICKS after: {ticks[1]}")
print("TICKS after > before: yes")
print(f"PIC masks: master={master_mask:#x} slave={slave_mask:#x}")
print(f"QEMU info pic: pic0 imr={pic0_imr} pic1 imr={pic1_imr}")
print("Kernel panic: none")
print("IOAPIC route failure: none")
print("LINT0 failure: none")
print("Boot prompt: found")
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

echo "QEMU IOAPIC IRQ0 routing test: OK"
