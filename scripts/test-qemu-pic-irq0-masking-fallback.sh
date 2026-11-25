#!/usr/bin/env bash
# Test Stage 3b PIC IRQ0 fallback proof on q35 with APIC and one vCPU.
# Profile: q35, default CPU, -smp 1.
# Transport: serial log file + Unix HMP monitor; forced route failure.
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

if [[ "${HFOC_PIC_IRQ0_MASKING_FALLBACK_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_PIC_IRQ0_MASKING_FALLBACK_TEST_TIMEOUT:-90}s" \
        env HFOC_PIC_IRQ0_MASKING_FALLBACK_TEST_INNER=1 "$0" "$@"
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
POST_TRACE_OFFSET_FILE=$TEST_DIR/post-trace-offset.txt
SERIAL_OFFSET_FILE=$TEST_DIR/serial-offset.txt
MONITOR_SOCKET=$TEST_DIR/monitor.sock
QEMU_PID=
FAILURE_MESSAGE=

: >"$SERIAL_LOG"
: >"$MONITOR_LOG"
: >"$QEMU_LOG"
: >"$INTERRUPTS_LOG"
: >"$POST_TRACE_OFFSET_FILE"
: >"$SERIAL_OFFSET_FILE"

printf '%s\n' \
    "machine=$QEMU_MACHINE" \
    "cpu=default" \
    "smp=$QEMU_SMP" \
    "image=$IMAGE" >"$METADATA"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/pic-irq0-masking-fallback-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-PIC IRQ0 masking fallback test failed}" >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$INTERRUPTS_LOG" \
        "$METADATA" "$FAILURE_REASON" "$TRACE_OFFSET_FILE" \
        "$POST_TRACE_OFFSET_FILE" "$SERIAL_OFFSET_FILE" "$artifact_dir/"
    echo "PIC IRQ0 masking fallback test artifacts: $artifact_dir" >&2
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
    "$INTERRUPTS_LOG" "$MONITOR_SOCKET" "$TRACE_OFFSET_FILE" \
    "$POST_TRACE_OFFSET_FILE" "$SERIAL_OFFSET_FILE" 2>&1 <<'PY'
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
post_trace_offset_path = pathlib.Path(sys.argv[6])
serial_offset_path = pathlib.Path(sys.argv[7])
deadline = time.monotonic() + 45.0
failure_marker = "[IOAPIC] test: forcing IRQ0 route failure"
success_route_marker = "[IOAPIC] route: irq=0 vector=0x40"
runtime_marker = "[LAPIC-TIMER] runtime: vector=0x42 hz=100; PIT sources masked"
trace_offset_path.write_text("0", encoding="ascii")

def serial_text():
    try:
        return serial_path.read_bytes().decode("ascii", errors="replace")
    except FileNotFoundError:
        return ""

def serial_suffix(offset):
    data = serial_path.read_bytes()
    if offset >= len(data):
        return ""
    if offset > 0 and data[offset - 1] not in (0x0a, 0x0d):
        newline = data.find(b"\n", offset)
        if newline < 0:
            return ""
        offset = newline + 1
    return data[offset:].decode("ascii", errors="replace")

def new_tick_value(offset):
    clean = serial_suffix(offset)
    # Interrupt markers can split "TICKS: (\d+)"; remove them before parsing.
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
    match = re.search(r"TICKS:\s*(\d+)\r?\n", clean)
    if match is None:
        return None
    return int(match.group(1))

pre_trace_offset = 0
while time.monotonic() < deadline:
    text = serial_text()
    if (failure_marker in text and
            "[PIC ] IRQ0 unmasked; legacy PIC timer retained." in text and
            runtime_marker not in text):
        trace_offset_path.write_text(str(pre_trace_offset), encoding="ascii")
        break
    if success_route_marker in text:
        raise SystemExit("Success route appeared in forced fallback build")
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic before validation")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="pre-transition fallback evidence" '
        'deadline=45s\n')
    sys.stderr.flush()
    raise SystemExit(
        "timed out waiting for pre-transition fallback evidence")

runtime_seen = False
while time.monotonic() < deadline:
    text = serial_text()
    if (not runtime_seen and
            "hfoc> " in text and runtime_marker in text):
        runtime_seen = True
    if runtime_seen:
        interrupts_bytes = interrupts_path.read_bytes()
        post_trace_offset = interrupts_bytes.find(
            b"Servicing hardware INT=0x42")
        if post_trace_offset >= 0:
            post_trace_offset_path.write_text(
                str(post_trace_offset), encoding="ascii")
            break
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic before runtime validation")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="boot prompt and runtime transition" '
        'deadline=45s\n')
    sys.stderr.flush()
    raise SystemExit(
        "timed out waiting for boot prompt and runtime transition")

text = serial_text()
if failure_marker not in text:
    raise SystemExit("Missing forced IOAPIC IRQ0 route failure marker")
if "[IOAPIC] route failed: irq=0" not in text:
    raise SystemExit("Missing IOAPIC IRQ0 route failed marker")
if success_route_marker in text:
    raise SystemExit("Success route appeared in forced fallback build")
if runtime_marker not in text:
    raise SystemExit("Missing runtime LAPIC transition diagnostic")
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("Kernel panic detected")
if "[LAPIC] LINT0 failed: status=" in text:
    raise SystemExit("LINT0 failure marker detected")
if "[PIC ] IRQ1 masked; IOAPIC vector=0x41 active." not in text:
    raise SystemExit("Missing Stage 3a IRQ1 success marker")
if "[PIC ] IRQ0 masked; IOAPIC vector=0x40 active." in text:
    raise SystemExit("Stage 3b success marker detected in fallback build")
if "[PIC ] IRQ0 unmasked; legacy PIC timer retained." not in text:
    raise SystemExit("Missing Stage 3b fallback marker")

mask_match = re.search(
    r"\[PIC \] Master mask: (0x[0-9A-Fa-f]+); slave mask: (0x[0-9A-Fa-f]+)",
    text)
if not mask_match:
    raise SystemExit("Missing: [PIC ] Master mask line")
master_mask = int(mask_match.group(1), 16)
slave_mask = int(mask_match.group(2), 16)
if master_mask != 0xfe or slave_mask != 0xff:
    raise SystemExit(
        f"Unexpected PIC masks: master={master_mask:#x} slave={slave_mask:#x}")
fallback_mask_line = "[PIC ] Master mask: 0x00000000000000FE; slave mask: 0x00000000000000FF"
if fallback_mask_line not in text:
    raise SystemExit("Missing exact uppercase Stage 3b fallback COM1 mask line")

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
        ticks_request_offset = len(serial_path.read_bytes())
        send_keys(("t", "i", "c", "k", "s", "ret"))
        while time.monotonic() < deadline:
            value = new_tick_value(ticks_request_offset)
            if value is not None:
                ticks.append(value)
                break
            time.sleep(0.05)
        else:
            # ITEM-422: a proof that gives up on a marker says so.
            sys.stderr.write(
                'HFOC-QEMU-MARKER-TIMEOUT marker="' +
                str(f"timed out waiting for new TICKS snapshot {snapshot} " f"after offset {ticks_request_offset}") + '" '
                'deadline=45s\n')
            sys.stderr.flush()
            raise SystemExit(
                f"timed out waiting for new TICKS snapshot {snapshot} "
                f"after offset {ticks_request_offset}")
        if snapshot == 1:
            time.sleep(2.0)

    if ticks[1] <= ticks[0]:
        raise SystemExit(f"fallback ticks did not grow: {ticks[0]} -> {ticks[1]}")

    try:
        interrupts_bytes = interrupts_path.read_bytes()
    except FileNotFoundError:
        raise SystemExit("Missing QEMU interrupt trace")
    pre_trace_offset = int(trace_offset_path.read_text(encoding="ascii"))
    post_trace_offset = int(
        post_trace_offset_path.read_text(encoding="ascii"))
    pre_trace_suffix = interrupts_bytes[
        pre_trace_offset:post_trace_offset].decode(
        encoding="ascii", errors="replace")
    post_trace_suffix = interrupts_bytes[post_trace_offset:].decode(
        encoding="ascii", errors="replace")
    if "Servicing hardware INT=0x20" not in pre_trace_suffix:
        raise SystemExit("Missing pre-transition: Servicing hardware INT=0x20")
    if "Servicing hardware INT=0x42" in pre_trace_suffix:
        raise SystemExit(
            "Pre-transition suffix contains Servicing hardware INT=0x42")
    if "Servicing hardware INT=0x42" not in post_trace_suffix:
        raise SystemExit(
            "Missing post-transition: Servicing hardware INT=0x42")
    if "Servicing hardware INT=0x20" in post_trace_suffix:
        raise SystemExit(
            "Post-transition suffix contains Servicing hardware INT=0x20")

    serial_offset_path.write_text(str(len(serial_path.read_bytes())), encoding="ascii")
    irq1_trace_offset = len(interrupts_path.read_bytes())
    for key in ("a", "b", "c", "ret"):
        command(f"sendkey {key}")
        time.sleep(0.08)

    irq1_abc = ""
    irq1_deadline = time.monotonic() + 15.0
    while time.monotonic() < irq1_deadline:
        suffix = serial_suffix(int(serial_offset_path.read_text(encoding="ascii")))
        clean = (
            suffix.replace("[IOAPIC] irq0 vector=0x40\r\n", "")
                .replace("[IOAPIC] irq0 lapic_eoi\r\n", "")
                .replace("[IOAPIC] irq1 vector=0x41\r\n", "")
                .replace("[IOAPIC] irq1 lapic_eoi\r\n", "")
                .replace("[LAPIC-TIMER] irq vector=0x42\r\n", "")
                .replace("[LAPIC-TIMER] irq lapic_eoi\r\n", "")
                .replace("[IOAPIC] irq0 vector=0x40\n", "")
                .replace("[IOAPIC] irq0 lapic_eoi\n", "")
                .replace("[IOAPIC] irq1 vector=0x41\n", "")
                .replace("[IOAPIC] irq1 lapic_eoi\n", "")
                .replace("[LAPIC-TIMER] irq vector=0x42\n", "")
                .replace("[LAPIC-TIMER] irq lapic_eoi\n", "")
        )
        abc_lines = [
            line.strip() for line in clean.splitlines()
            if line.strip() == "UNKNOWN COMMAND: abc. TYPE HELP."
        ]
        if len(abc_lines) == 1 and "[IOAPIC] irq1 vector=0x41" in suffix and \
                "[IOAPIC] irq1 lapic_eoi" in suffix:
            irq1_abc = abc_lines[0]
            break
        time.sleep(0.05)
    else:
        # ITEM-422: a proof that gives up on a marker says so.
        sys.stderr.write(
            'HFOC-QEMU-MARKER-TIMEOUT marker="exactly-once abc and IRQ1 markers" '
            'deadline=' + str(int(irq1_deadline)) + 's\n')
        sys.stderr.flush()
        raise SystemExit("timed out waiting for exactly-once abc and IRQ1 markers")

    irq1_trace_suffix = interrupts_path.read_bytes()[irq1_trace_offset:].decode(
        encoding="ascii", errors="replace")
    if "Servicing hardware INT=0x41" not in irq1_trace_suffix:
        raise SystemExit("Missing in IRQ1 suffix: Servicing hardware INT=0x41")
    if "Servicing hardware INT=0x21" in irq1_trace_suffix:
        raise SystemExit("IRQ1 suffix unexpectedly contains Servicing hardware INT=0x21")

with monitor_log.open("a", encoding="utf-8") as output:
    output.write("validation complete\n")

print("Forced route failure marker: found")
print("Fallback marker: IRQ0 unmasked; legacy PIC timer retained")
print("Pre-transition trace: INT=0x20 found; INT=0x42 absent")
print("Post-transition trace: INT=0x42 found; INT=0x20 absent")
print("Console input exactly once: abc")
print("IRQ1 suffix: INT=0x41 found; INT=0x21 absent")
print(f"fallback_ticks_before: {ticks[0]}")
print(f"fallback_ticks_after: {ticks[1]}")
print("fallback_ticks_after > fallback_ticks_before: yes")
print(f"PIC masks: master={master_mask:#x} slave={slave_mask:#x}")
print(f"QEMU info pic: pic0 imr={pic0_imr} pic1 imr={pic1_imr}")
print("Kernel panic: none")
print("IOAPIC route failure: forced")
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

echo "QEMU PIC IRQ0 masking fallback test: OK"
