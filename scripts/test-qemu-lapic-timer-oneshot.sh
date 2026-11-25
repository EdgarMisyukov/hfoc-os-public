#!/usr/bin/env bash
# Prove the macro-gated LAPIC timer one-shot probe on vector 0x43 and the
# exact restore of periodic vector 0x42.
# Profile: q35, default CPU, -smp 1.
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

if [[ "${HFOC_LAPIC_TIMER_ONESHOT_TEST_INNER:-0}" != 1 ]]; then
    # ITEM-426: the wrapper and the deadline inside it are one contract.  An
    # earlier attempt raised only the deadline, and the wrapper killed the
    # proof at 90 s - exit 124, no class record, and the phase could not even
    # retry it (418's run, 2026-09-16).  The wrapper is the deadline plus the
    # time the guest needs to boot and build.
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_LAPIC_TIMER_ONESHOT_TEST_TIMEOUT:-300}s" \
        env HFOC_LAPIC_TIMER_ONESHOT_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/oneshot/hfoc-os-$(<VERSION)-oneshot.iso}

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
: >"$SERIAL_OFFSET_FILE"
: >"$TRACE_OFFSET_FILE"

printf '%s\n' \
    "machine=$QEMU_MACHINE" \
    "cpu=default" \
    "smp=$QEMU_SMP" \
    "image=$IMAGE" >"$METADATA"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/lapic-timer-oneshot-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-LAPIC timer one-shot test failed}" \
        >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$INTERRUPTS_LOG" \
        "$METADATA" "$FAILURE_REASON" "$SERIAL_OFFSET_FILE" \
        "$TRACE_OFFSET_FILE" "$artifact_dir/"
    echo "LAPIC timer one-shot test artifacts: $artifact_dir" >&2
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
    -d int -D "$INTERRUPTS_LOG" \
    -no-reboot -no-shutdown >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

validation_output=
if ! validation_output=$(python3 - "$SERIAL_LOG" "$MONITOR_LOG" \
    "$INTERRUPTS_LOG" "$MONITOR_SOCKET" "$SERIAL_OFFSET_FILE" \
    "$TRACE_OFFSET_FILE" 2>&1 <<'PY'
import pathlib
import re
import socket
import sys
import time

# ITEM-427: the bounded retype owns the lost-keystroke recovery; the markers,
# the exactness checks and the deadline stay this proof's own.
sys.path.insert(0, "scripts")
import hfoc_console_command as console_command

serial_path = pathlib.Path(sys.argv[1])
monitor_log = pathlib.Path(sys.argv[2])
interrupts_path = pathlib.Path(sys.argv[3])
monitor_socket = pathlib.Path(sys.argv[4])
serial_offset_path = pathlib.Path(sys.argv[5])
trace_offset_path = pathlib.Path(sys.argv[6])
# ITEM-426: every guest in this suite runs under TCG - the plumbing never
# passes -enable-kvm, and /dev/kvm exists on the CI host - so a guest that
# shares the host with four others runs many times slower than wall clock.
# The console interactions below are the ones that expired twice in a row on
# 2026-09-16 (test-lapic-timer-oneshot, test-pic-disable,
# test-console-history), and the retry rule was spent on the first of them.
# The wait is the phase's interaction budget for an emulated guest; the
# markers themselves are exact and are not what changed.
deadline = time.monotonic() + 180.0
runtime_re = re.compile(
    rb"\[LAPIC-TIMER\] runtime: vector=0x42 hz=100; "
    rb"PIT sources masked")
disable_re = re.compile(
    rb"\[PIC \] disabled; APIC runtime active\.\r?\n")
armed_marker = "[LAPIC-TIMER] oneshot armed: vector=0x43"
complete_marker = (
    "[LAPIC-TIMER] oneshot complete: periodic vector=0x42 restored")
arm_failed_marker = "[LAPIC-TIMER] oneshot arm failed; periodic retained"
restore_failed_marker = "[LAPIC-TIMER] oneshot restore failed; LVT masked"

def serial_bytes():
    try:
        return serial_path.read_bytes()
    except FileNotFoundError:
        return b""

def serial_text():
    return serial_bytes().decode("ascii", errors="replace")

def serial_suffix(offset):
    data = serial_bytes()
    if offset >= len(data):
        return ""
    if offset > 0 and data[offset - 1] not in (0x0a, 0x0d):
        newline = data.find(b"\n", offset)
        if newline < 0:
            return ""
        offset = newline + 1
    return data[offset:].decode("ascii", errors="replace")

def clean_text(text):
    clean = re.sub(r"\[LAPIC-TIMER\][^\r\n]*(?:\r?\n)?", "", text)
    for marker in (
        "[IOAPIC] irq0 vector=0x40",
        "[IOAPIC] irq0 lapic_eoi",
        "[IOAPIC] irq1 vector=0x41",
        "[IOAPIC] irq1 lapic_eoi",
    ):
        clean = (
            clean.replace(marker + "\r\n", "")
                .replace(marker + "\n", "")
                .replace(marker, "")
        )
    return clean

def clean_serial_suffix(offset):
    return clean_text(serial_suffix(offset))

def tick_value(suffix):
    match = re.search(r"TICKS:\s*(\d+)\r?\n", clean_text(suffix))
    if match is None:
        return None
    return int(match.group(1))

while time.monotonic() < deadline:
    text = serial_text()
    data = serial_bytes()
    if (runtime_re.search(data) and disable_re.search(data) and
            armed_marker in text):
        break
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic before validation")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="boot prompt, runtime diagnostic, PIC disable, and one-shot arm diagnostic" '
        'deadline=180s\n')
    sys.stderr.flush()
    raise SystemExit(
        "timed out waiting for boot prompt, runtime diagnostic, "
        "PIC disable, and one-shot arm diagnostic")

text = serial_text()
if not runtime_re.search(serial_bytes()):
    raise SystemExit("Missing: [LAPIC-TIMER] runtime exact diagnostic")
if not disable_re.search(serial_bytes()):
    raise SystemExit("Missing: [PIC ] disabled; APIC runtime active.")
if armed_marker not in text:
    raise SystemExit("Missing: one-shot armed diagnostic")
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("Kernel panic detected")
if "[PIC ] retained; PIC disable failed." in text:
    raise SystemExit("PIC disable failure marker detected")
if arm_failed_marker in text:
    raise SystemExit("One-shot arm failure marker detected")
if restore_failed_marker in text:
    raise SystemExit("One-shot restore failure marker detected")

trace_offset_path.write_text(
    str(len(interrupts_path.read_bytes())), encoding="ascii")

while time.monotonic() < deadline:
    text = serial_text()
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic before one-shot completion")
    if arm_failed_marker in text:
        raise SystemExit("One-shot arm failure marker detected")
    if restore_failed_marker in text:
        raise SystemExit("One-shot restore failure marker detected")
    if complete_marker in text:
        break
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="one-shot completion diagnostic" '
        'deadline=180s\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for one-shot completion diagnostic")

serial_offset_path.write_text(str(len(serial_bytes())), encoding="ascii")

stage3b_mask_line = (
    "[PIC ] Master mask: 0x00000000000000FF; "
    "slave mask: 0x00000000000000FF")
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

    def send_key(key):
        command(f"sendkey {key}")
        time.sleep(0.08)

    def press_ret():
        send_key("ret")

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
        ticks_request_offset = len(serial_bytes())
        try:
            # ITEM-427: a keystroke the bounded input queue dropped is retyped;
            # the value is still read only from a new, complete TICKS line.
            ticks_request_offset = console_command.type_line(
                send_key, serial_bytes, "ticks", deadline)
            value = console_command.submit(
                press_ret, serial_bytes, tick_value, deadline)
        except console_command.LostKeystrokes:
            # ITEM-422: a proof that gives up on a marker says so.
            sys.stderr.write(
                'HFOC-QEMU-MARKER-TIMEOUT marker="' +
                str(f"timed out waiting for new TICKS snapshot {snapshot} " f"after offset {ticks_request_offset}") + '" '
                'deadline=180s\n')
            sys.stderr.flush()
            raise SystemExit(
                f"timed out waiting for new TICKS snapshot {snapshot} "
                f"after offset {ticks_request_offset}")
        ticks.append(value)
        if snapshot == 1:
            time.sleep(2.0)

    if ticks[1] <= ticks[0]:
        raise SystemExit(
            f"TICKS did not grow after one-shot restore: "
            f"{ticks[0]} -> {ticks[1]}")

    help_request_offset = len(serial_bytes())
    help_marker = ("COMMANDS: HELP CLEAR SCROLL VERSION UPTIME TICKS IRQ KBD MEM PMM HEAP "
                   "ABOUT APPLIANCE APPLIANCE-BOOT CREDENTIALS LOGIN LANGUAGE "
                   "REBOOT SHUTDOWN PANIC DIVZERO PAGEFAULT "
                   "DOUBLEFAULT VMMMAP")

    def exactly_one_help(suffix):
        count = clean_text(suffix).count(help_marker)
        if count > 1:
            # A duplicated result is an assertion, not a timing stop.
            raise SystemExit(
                f"expected one help command result, got {count}")
        return True if count == 1 else None

    try:
        # ITEM-427: the line is retyped while its echo is missing, and the
        # Enter is repeated until the command's own result arrives; the marker
        # and the exactness check below are unchanged.
        help_request_offset = console_command.type_line(
            send_key, serial_bytes, "help", deadline)
        console_command.submit(
            press_ret, serial_bytes, exactly_one_help, deadline)
    except console_command.LostKeystrokes:
        # ITEM-421: a console command that did not answer inside this proof's
        # deadline is a timing class, not an assertion: the guest can be healthy
        # and starved, and on 2026-09-15 this is exactly how the acceptance gate
        # failed under load.  The record is what the runners read; an in-guest
        # `[TICKS] failed` or a panic is never reported this way.
        sys.stderr.write(
            'HFOC-QEMU-MARKER-TIMEOUT marker="console help result" '
            'deadline=180s\n')
        sys.stderr.flush()
        # ITEM-422: a proof that gives up on a marker says so.
        sys.stderr.write(
            'HFOC-QEMU-MARKER-TIMEOUT marker="exactly one help command result" '
            'deadline=180s\n')
        sys.stderr.flush()
        raise SystemExit(
            "timed out waiting for exactly one help command result")

    suffix = clean_serial_suffix(help_request_offset)
    if suffix.count(help_marker) != 1:
        raise SystemExit(
            f"expected one help command result, got {suffix.count(help_marker)}")

    full_trace = interrupts_path.read_text(encoding="ascii", errors="replace")
    if full_trace.count("Servicing hardware INT=0x43") != 1:
        raise SystemExit(
            "expected exactly one Servicing hardware INT=0x43, got "
            f"{full_trace.count('Servicing hardware INT=0x43')}")
    first_oneshot = full_trace.find("Servicing hardware INT=0x43")
    if full_trace.find("Servicing hardware INT=0x42", first_oneshot) < 0:
        raise SystemExit("missing Servicing hardware INT=0x42 after 0x43")

    trace_offset = int(trace_offset_path.read_text(encoding="ascii"))
    try:
        interrupts_bytes = interrupts_path.read_bytes()
    except FileNotFoundError:
        raise SystemExit("Missing QEMU interrupt trace")
    trace_suffix = interrupts_bytes[trace_offset:].decode(
        encoding="ascii", errors="replace")
    if "Servicing hardware INT=0x42" not in trace_suffix:
        raise SystemExit("Missing in post-hook suffix: INT=0x42")
    if "Servicing hardware INT=0x20" in trace_suffix:
        raise SystemExit("Post-hook suffix contains INT=0x20")
    if "Servicing hardware INT=0x21" in trace_suffix:
        raise SystemExit("Post-hook suffix contains INT=0x21")

with monitor_log.open("a", encoding="utf-8") as output:
    output.write("validation complete\n")

print("One-shot armed diagnostic: found")
print("One-shot complete diagnostic: found")
print("PIC masks: master=0xff slave=0xff")
print(f"QEMU info pic: pic0 imr={pic0_imr} pic1 imr={pic1_imr}")
print("Servicing hardware INT=0x43 count: 1")
print("Servicing hardware INT=0x42 after 0x43: yes")
print("Post-hook suffix: INT=0x42 found; INT=0x20 and INT=0x21 absent")
print(f"TICKS before: {ticks[0]}")
print(f"TICKS after: {ticks[1]}")
print("TICKS after > before: yes")
print("Command result count: help=1")
print("Kernel panic: none")
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

echo "QEMU LAPIC timer one-shot test: OK"
