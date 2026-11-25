#!/usr/bin/env bash
# Test console command history proof on q35 / one vCPU / default CPU.
# Transport: serial log file + Unix HMP monitor; sendkey history sequence.
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

if [[ "${HFOC_CONSOLE_HISTORY_TEST_INNER:-0}" != 1 ]]; then
    # ITEM-426: the wrapper and the deadline inside it are one contract.  An
    # earlier attempt raised only the deadline, and the wrapper killed the
    # proof at 90 s - exit 124, no class record, and the phase could not even
    # retry it (418's run, 2026-09-16).  The wrapper is the deadline plus the
    # time the guest needs to boot and build.
    # ITEM-427: this proof spends three budgets inside the wrapper now - the
    # 45 s boot-prompt wait, the 90 s typing phase that recovers a lost
    # keystroke, and the 180 s validation window - plus the 5 s trace wait and
    # the time QEMU needs to start.  300 s no longer dominates that sum, so the
    # wrapper rises with the deadlines it protects: a give-up writes its
    # HFOC-QEMU-MARKER-TIMEOUT record instead of being killed at 124 with no
    # class the phase could retry.
    # ITEM-457: the scroll burst is a fourth budget - 120 s of typed long
    # output plus the 30 s settle window that reads the records it left - so the
    # wrapper rises with it by the same rule.
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_CONSOLE_HISTORY_TEST_TIMEOUT:-540}s" \
        env HFOC_CONSOLE_HISTORY_TEST_INNER=1 "$0" "$@"
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
QMP_SOCKET=$TEST_DIR/qmp.sock
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
    artifact_dir="build/test-artifacts/console-history-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-Console history test failed}" \
        >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$INTERRUPTS_LOG" \
        "$METADATA" "$FAILURE_REASON" "$SERIAL_OFFSET_FILE" \
        "$TRACE_OFFSET_FILE" "$artifact_dir/"
    echo "Console history test artifacts: $artifact_dir" >&2
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

# ITEM-461: the mode the proof runs in.  Empty keeps the firmware default; a
# value selects a VGA device with an EDID that asks for a mode, so the same
# image can be proven at 1920x1080 and 2560x1440 (the two the item names) and
# the `[FB]` record lets the proof assert which one it got.
qemu_vga_args=()
if [[ -n "${HFOC_CONSOLE_HISTORY_DISPLAY:-}" ]]; then
    qemu_vga_args=(-vga none -device "${HFOC_CONSOLE_HISTORY_DISPLAY}")
fi

qemu-system-x86_64 \
    "${qemu_vga_args[@]}" \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" \
    -display none -cdrom "$IMAGE" \
    -serial "file:$SERIAL_LOG" \
    -monitor "unix:$MONITOR_SOCKET,server,nowait" \
    -qmp "unix:$QMP_SOCKET,server,nowait" \
    -device qemu-xhci,id=xhci -device usb-mouse,bus=xhci.0 \
    -d int -D "$INTERRUPTS_LOG" \
    -no-reboot -no-shutdown >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

phase1_output=
if ! phase1_output=$(python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" \
    "$MONITOR_LOG" "$SERIAL_OFFSET_FILE" "$INTERRUPTS_LOG" \
    "$TRACE_OFFSET_FILE" 2>&1 <<'PY'
import pathlib
import re
import socket
import sys
import time

# ITEM-427: the bounded retype owns the lost-keystroke recovery; the markers,
# the exactness checks and the validation deadline stay this proof's own.
sys.path.insert(0, "scripts")
import hfoc_console_command as console_command

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

def normalised_serial_bytes():
    # ITEM-463: a `[CONSOLE] scroll` record is written from inside the paint of
    # the line it follows, so it can land in the middle of a typed echo or of a
    # command's own result and split the marker the proof waits for.  The
    # marker checks read this view, which removes the interleaved console and
    # interrupt diagnostics; the offset record and the scroll proof still read
    # the raw stream.
    data = serial_bytes().decode("ascii", errors="replace")
    data = re.sub(r"\[CONSOLE\] scroll[^\[\r\n]*\r?\n", "", data)
    data = re.sub(r"\[(?:IOAPIC|LAPIC|PIC|PIT|IRQ)[^\[\r\n]*\r?\n", "", data)
    return data.encode("ascii", errors="replace")

while time.monotonic() < deadline:
    text = serial_text()
    if "hfoc> " in text:
        break
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic before sendkey")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="boot prompt" '
        'deadline=45s\n')
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

    def send_key(key):
        monitor.sendall(("sendkey " + key + "\n").encode("ascii"))
        record(receive_prompt())
        time.sleep(0.08)

    def press_up():
        send_key("up")

    def press_down():
        send_key("down")

    def press_ret():
        send_key("ret")

    def type_text(text):
        # ITEM-427: a line the bounded input queue dropped is retyped while its
        # echo is missing; a retype cannot add a console history entry, because
        # only Enter does that and Enter is never sent from here.
        console_command.type_line(send_key, normalised_serial_bytes, text,
                                  typing_deadline)

    def one_result(marker):
        def probe(suffix):
            count = suffix.count(marker)
            if count > 1:
                # A duplicated result is an assertion, not a timing stop.
                raise SystemExit(
                    f"expected one {marker} command result, got {count}")
            return True if count == 1 else None
        return probe

    record(receive_prompt())
    offset_path.write_text(str(len(serial_bytes())), encoding="ascii")
    trace_offset_path.write_text(
        str(len(interrupts_path.read_bytes())), encoding="ascii")

    help_marker = ("COMMANDS: HELP CLEAR SCROLL VERSION UPTIME TICKS IRQ KBD MEM PMM HEAP "
                   "ABOUT APPLIANCE APPLIANCE-BOOT CREDENTIALS LOGIN LANGUAGE "
                   "REBOOT SHUTDOWN PANIC DIVZERO PAGEFAULT "
                   "DOUBLEFAULT VMMMAP")
    version_marker = "HFOC OS 1.0.0 X86-64"
    # ITEM-427: the typing phase gets its own bounded budget inside the
    # wrapper's 360 s, so a lost keystroke is recovered here instead of being
    # paid for out of the validation phase's 180 s marker window.  The number
    # is the record's deadline below: the give-up names the budget that ran
    # out, not the window the validation phase would have used.
    typing_deadline = time.monotonic() + 90.0

    try:
        type_text("help")
        console_command.submit(press_ret, normalised_serial_bytes,
                               one_result(help_marker), typing_deadline)
        type_text("version")
        console_command.submit(press_ret, normalised_serial_bytes,
                               one_result(version_marker), typing_deadline)
        type_text("draft")
        for press, expected_line in (
                (press_up, "version"), (press_up, "help"),
                (press_down, "version"), (press_down, "draft"),
                (press_up, "version")):
            console_command.press_key(press, normalised_serial_bytes, expected_line,
                                      typing_deadline)
        console_command.submit(press_ret, normalised_serial_bytes,
                               one_result(version_marker), typing_deadline)
    except console_command.LostKeystrokes:
        # ITEM-422: a proof that gives up on a marker says so.  ITEM-427: the
        # budget that ran out here is the typing phase's 90 s; the validation
        # phase writes the same marker with its own 180 s window below.
        sys.stderr.write(
            'HFOC-QEMU-MARKER-TIMEOUT marker="exact help/version counts and draft restore" '
            'deadline=90s\n')
        sys.stderr.flush()
        raise SystemExit(
            "timed out waiting for exact help/version counts and draft restore")

print("HMP sendkey history sequence: help version draft up up down down up ret")
print("Serial offset saved before sendkey: yes")
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
# ITEM-426: every guest in this suite runs under TCG - the plumbing never
# passes -enable-kvm, and /dev/kvm exists on the CI host - so a guest that
# shares the host with four others runs many times slower than wall clock.
# The console interactions below are the ones that expired twice in a row on
# 2026-09-16 (test-lapic-timer-oneshot, test-pic-disable,
# test-console-history), and the retry rule was spent on the first of them.
# The wait is the phase's interaction budget for an emulated guest; the
# markers themselves are exact and are not what changed.
# ITEM-424 raised 20 s to 45 s for the same proof; ITEM-426 is the measurement
# behind the number.
deadline = time.monotonic() + 180.0
help_marker = ("COMMANDS: HELP CLEAR SCROLL VERSION UPTIME TICKS IRQ KBD MEM PMM HEAP "
               "ABOUT APPLIANCE APPLIANCE-BOOT CREDENTIALS LOGIN LANGUAGE "
               "REBOOT SHUTDOWN PANIC DIVZERO PAGEFAULT "
               "DOUBLEFAULT VMMMAP")
version_marker = "HFOC OS 1.0.0 X86-64"
draft_marker = "\rhfoc> draft"
parser_fixture = ("COMMANDS: HELP CLEAR SCROLL VERSION UPTIME TICKS IRQ KBD MEM "
                  "PMM HEAP ABOUT APPLIANCE APPLIANCE-BOOT "
                  "CREDENTIALS LOGIN LANGUAGE REBOOT SHUTDOWN "
                  "PANIC DIVZERO PAGEFAULT DOUBLEFAULT "
                  "VMM[LAPIC-TIMER] irq vector=0x42\n"
                  "[LAPIC-TIMER] irq lapic_eoi\nMAP")

def normalise_suffix(suffix):
    return re.sub(
        r'\[(?:IOAPIC|LAPIC|PIC|PIT|IRQ|CONSOLE)[^\[\r\n]*(?:\r?\n)?',
        '', suffix)

normalised_fixture = normalise_suffix(parser_fixture)
if help_marker not in normalised_fixture:
    raise SystemExit(
        "history parser fixture: COMMANDS marker split by LAPIC timer diagnostics")
# ITEM-463: the same reader must restore a marker split by a `[CONSOLE]` cost
# record, which is written from inside the paint of the line it follows.
console_split_fixture = (
    help_marker[:40] +
    "[CONSOLE] scroll rows=7 bytes=1 us=1 max_us=1\n" +
    help_marker[40:])
if help_marker not in normalise_suffix(console_split_fixture):
    raise SystemExit(
        "history parser fixture: COMMANDS marker split by a console record")

def serial_bytes():
    try:
        return serial.read_bytes()
    except FileNotFoundError:
        return b""

def suffix_text():
    data = serial_bytes()
    if offset >= len(data):
        return ""
    if offset > 0 and data[offset - 1] not in (0x0a, 0x0d):
        newline = data.find(b"\n", offset)
        if newline < 0:
            return ""
        return normalise_suffix(
            data[newline + 1:].decode("ascii", errors="replace"))
    return normalise_suffix(
        data[offset:].decode("ascii", errors="replace"))

def trace_suffix_text():
    data = interrupts_path.read_bytes()
    if trace_offset >= len(data):
        return ""
    return data[trace_offset:].decode("ascii", errors="replace")

def qemu_is_running():
    try:
        os.kill(qemu_pid, 0)
        state = pathlib.Path(
            f"/proc/{qemu_pid}/stat").read_text(encoding="ascii").split()[2]
        return state != "Z"
    except (OSError, IndexError, FileNotFoundError):
        return False

while time.monotonic() < deadline:
    suffix = suffix_text()
    if (suffix.count(version_marker) == 2 and
            suffix.count(help_marker) == 1 and draft_marker in suffix):
        marker_index = suffix.rfind(version_marker)
        if suffix.find("hfoc> ", marker_index) > marker_index:
            break
    if "*** HFOC KERNEL PANIC ***" in suffix:
        raise SystemExit("Kernel panic during history navigation")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="exact help/version counts and draft restore" '
        'deadline=180s\n')
    sys.stderr.flush()
    raise SystemExit(
        "timed out waiting for exact help/version counts and draft restore")

suffix = suffix_text()
if suffix.count(help_marker) != 1:
    raise SystemExit(f"expected one help command result, got "
                     f"{suffix.count(help_marker)}")
if suffix.count(version_marker) != 2:
    raise SystemExit(f"expected two version command results, got "
                     f"{suffix.count(version_marker)}")
if "UNKNOWN COMMAND: draft. TYPE HELP." in suffix:
    raise SystemExit("draft was submitted as a command")
if "\x1b[" in suffix:
    raise SystemExit("console redraw emitted an ANSI escape")

trace_deadline = time.monotonic() + 5.0
while time.monotonic() < trace_deadline:
    trace_suffix = trace_suffix_text()
    if "Servicing hardware INT=0x21" in trace_suffix:
        raise SystemExit(
            "history trace contains Servicing hardware INT=0x21")
    if "Servicing hardware INT=0x41" in trace_suffix:
        break
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="Servicing hardware INT=0x41" '
        'deadline=' + str(int(trace_deadline)) + 's\n')
    sys.stderr.flush()
    raise SystemExit(
        "timed out waiting for Servicing hardware INT=0x41")

full = serial_bytes().decode("ascii", errors="replace")
if "*** HFOC KERNEL PANIC ***" in full:
    raise SystemExit("Kernel panic detected")
if "hfoc> " not in full:
    raise SystemExit("boot prompt missing")

if not qemu_is_running():
    raise SystemExit("QEMU exited before validation completed")
time.sleep(0.3)
if not qemu_is_running():
    raise SystemExit("QEMU exited during observation interval")

print("Final submitted command: version")
print("Command result counts: help=1 version=2")
print("Draft restored before final Up: yes")
print("No ANSI escapes in redraw: yes")
print("Trace suffix: INT=0x41 found; INT=0x21 absent")
print("Kernel panic: none")
print("QEMU alive: yes")
PY
); then
    FAILURE_MESSAGE=$validation_output
    printf '%s\n' "$validation_output" >&2
    exit 1
fi

printf '%s\n' "$validation_output"

# ITEM-457: the console's own scroll, measured.
#
# The 2026-09-19 hardware report was that output reaching the bottom of the
# screen crawled and the keyboard answered late while it happened: the body was
# copied one pixel at a time.  The move is a row move now, and this phase makes
# the guest scroll several screens and reads the records the console publishes
# about it:
#
#   [CONSOLE] scroll rows=<total> bytes=<total> us=<average> max_us=<worst>
#
# What is required is the contract the item names: the burst really did scroll
# several screens, every record is well formed, the totals only grow, every
# byte step is a whole number of row moves (the scroll writes the same row
# block every time), the measured cost stays inside a budget, and the worst
# cost does not grow from the first half of the burst to the second.  ITEM-463
# moved the clock to the cycle counter: a scroll ends inside one 100 Hz tick,
# so the millisecond reading was always zero and the budget bounded nothing.
# The fix is proven on the host (tests/framebuffer-scroll-host.c reads the
# pixels back) and pinned by scripts/check.d/64-console-scroll.sh.
scroll_output=
if ! scroll_output=$(python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" "$MONITOR_LOG" \
    2>&1 <<'PY'
import pathlib
import re
import socket
import sys
import time

sys.path.insert(0, "scripts")
import hfoc_console_command as console_command

serial_path = pathlib.Path(sys.argv[1])
socket_path = pathlib.Path(sys.argv[2])
monitor_log = pathlib.Path(sys.argv[3])

# ITEM-463: the costs are microseconds from the cycle counter.  Measured on
# an idle TCG guest a row-move scroll of the console body took tens of
# milliseconds at 1080p and more at 4K, so the budget bounds the pathology
# (a scroll that redraws the screen instead of moving it), not the fix; the
# growth check across the burst is the part that catches a regression.  The
# paint budget is smaller because the burst's own scrolls are subtracted from
# it and charged to `[CONSOLE] scroll`.
SCROLL_US_BUDGET = 250000
# A 20 ms floor plus a factor of five: emulation noise under a loaded runner is
# multiplicative, and the fixed 10 ms slack the millisecond reading used is far
# below it now that the cost is real microseconds.
SCROLL_US_SLACK = 20000
SCROLL_US_GROWTH_FACTOR = 5
# ITEM-478 (round 16): the boot screen paints only the product banner now, so
# the burst starts several rows higher and 24 reports left the total just under
# the sixth report's threshold (measured: 5 reports).  The burst is longer - the
# same per-report budget and the same six-report floor - so it still crosses six
# whole-body moves rather than lowering the floor.
SCROLL_COMMANDS = 30
SCROLL_MIN_ROWS = 96
SCROLL_MIN_REPORTS = 6
SCROLL_REPORT = re.compile(
    r"^\[CONSOLE\] scroll rows=(\d+) bytes=(\d+) us=(\d+) max_us=(\d+)$",
    re.M)

def serial_bytes():
    try:
        return serial_path.read_bytes()
    except FileNotFoundError:
        return b""

def normalised_serial_bytes():
    # ITEM-463: the typed echo must survive a cost record landing inside it.
    data = serial_bytes().decode("ascii", errors="replace")
    data = re.sub(r"\[CONSOLE\] scroll[^\[\r\n]*\r?\n", "", data)
    data = re.sub(r"\[(?:IOAPIC|LAPIC|PIC|PIT|IRQ)[^\[\r\n]*\r?\n", "", data)
    return data.encode("ascii", errors="replace")

deadline = time.monotonic() + 120.0

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

    def send_key(key):
        monitor.sendall(("sendkey " + key + "\n").encode("ascii"))
        record(receive_prompt())
        time.sleep(0.03)

    record(receive_prompt())
    # The burst starts on a line boundary: a record cut in half by the offset
    # is not a record this phase reads.
    data = serial_bytes()
    offset = data.rfind(b"\n") + 1

    try:
        for _ in range(SCROLL_COMMANDS):
            # `appliance` is the longest report a plain boot can ask for with
            # keys the harness can send (no shifted characters), and each
            # report is several screens' worth of lines.
            console_command.type_line(send_key, normalised_serial_bytes,
                                      "appliance", deadline)
            console_command.submit(lambda: send_key("ret"),
                                   normalised_serial_bytes,
                                   lambda suffix: True, deadline)
    except console_command.LostKeystrokes as exc:
        sys.stderr.write(
            'HFOC-QEMU-MARKER-TIMEOUT marker="console scroll burst" '
            'deadline=120s\n')
        sys.stderr.flush()
        raise SystemExit(f"timed out typing the scroll burst: {exc}")

    reports = []
    settle_deadline = time.monotonic() + 30.0
    while time.monotonic() < settle_deadline:
        text = serial_bytes()[offset:].decode("ascii", errors="replace")
        reports = SCROLL_REPORT.findall(text)
        if len(reports) >= SCROLL_MIN_REPORTS:
            break
        time.sleep(0.2)

if len(reports) < SCROLL_MIN_REPORTS:
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="[CONSOLE] scroll records" '
        'deadline=30s\n')
    sys.stderr.flush()
    raise SystemExit(
        f"expected at least {SCROLL_MIN_REPORTS} scroll records, got "
        f"{len(reports)}")

rows = [int(match[0]) for match in reports]
byte_totals = [int(match[1]) for match in reports]
averages = [int(match[2]) for match in reports]
worsts = [int(match[3]) for match in reports]

for index in range(1, len(reports)):
    if rows[index] <= rows[index - 1]:
        raise SystemExit("a scroll report did not advance its row total")
    if byte_totals[index] <= byte_totals[index - 1]:
        raise SystemExit("a scroll report did not advance its byte total")
# ITEM-474: a turn batches the rows it scrolled into one body move, so one
# report may cover several whole-body writes.  What ITEM-457 pinned still has
# to hold: the move writes whole screen rows, never pixels, and the bytes one
# screen row moved is the framebuffer's own row stride.  The stride comes from
# the `[FB]` record the machine published, so the check follows the mode the
# firmware actually gave instead of the requested one.  Every byte step is a
# whole number of screen rows; the per-text-row block is no longer constant
# because a batch moves many rows in one whole-body write.
SCREEN_ROW_BYTES = 1024 * 4
fb_match = re.search(
    r"\[FB\] width=\d+ height=\d+ pitch=(\d+)",
    serial_bytes().decode("ascii", errors="replace"))
if fb_match:
    SCREEN_ROW_BYTES = int(fb_match.group(1))
else:
    fb_width = re.search(
        r"\[FB\] width=(\d+)",
        serial_bytes().decode("ascii", errors="replace"))
    if fb_width:
        SCREEN_ROW_BYTES = int(fb_width.group(1)) * 4
row_blocks = []
for index in range(1, len(reports)):
    row_step = rows[index] - rows[index - 1]
    byte_step = byte_totals[index] - byte_totals[index - 1]
    if row_step <= 0 or byte_step <= 0:
        raise SystemExit("a scroll report did not advance its totals")
    if byte_step % SCREEN_ROW_BYTES != 0:
        raise SystemExit(
            "a scroll report's byte step is not a whole number of screen rows")
    row_blocks.append(byte_step)
if row_blocks[0] == 0:
    raise SystemExit("a scroll report claims a row move of no bytes")
# The greatest common divisor of the steps is the byte block one body move
# writes when the window holds a single move; it must be at least one screenful
# of rows, or the byte step would be a fraction of the body and the "row move"
# claim would be an artefact of the arithmetic.
from math import gcd
block = 0
for step in row_blocks:
    block = gcd(block, step)
if block < SCREEN_ROW_BYTES * SCROLL_MIN_ROWS:
    raise SystemExit("the bytes one scrolled row moves changed across the burst")
if rows[-1] - rows[0] < SCROLL_MIN_ROWS:
    raise SystemExit(
        f"the burst scrolled {rows[-1] - rows[0]} rows, less than the "
        f"{SCROLL_MIN_ROWS} several screens require")
if max(averages) > SCROLL_US_BUDGET or max(worsts) > SCROLL_US_BUDGET:
    raise SystemExit(
        f"a scroll took longer than the {SCROLL_US_BUDGET} us budget: "
        f"us={averages} max_us={worsts}")
half = len(reports) // 2
# ITEM-463: the two halves must not degrade into a repaint.  A scroll moves the
# same row block every time, but an emulated guest shares the runner with the
# other QEMU targets, so the multiplier tolerates the load's own variation
# while still failing a cost that grows by several times across the burst.
first_worst = max(worsts[:half])
if max(worsts[half:]) > first_worst * SCROLL_US_GROWTH_FACTOR + SCROLL_US_SLACK:
    raise SystemExit("the worst scroll grew across the burst")

full = serial_bytes().decode("ascii", errors="replace")
if "*** HFOC KERNEL PANIC ***" in full:
    raise SystemExit("Kernel panic during the scroll burst")

print(f"Scroll records: {len(reports)}")
print(f"Scroll rows moved by the burst: {rows[-1] - rows[0]}")
print(f"Scroll bytes at the end: {byte_totals[-1]}")
print(f"Bytes one whole-body move writes: {block}")
print(f"Scroll average us: min={min(averages)} max={max(averages)}")
print(f"Scroll worst us: first_half={max(worsts[:half])} "
      f"second_half={max(worsts[half:])} budget={SCROLL_US_BUDGET}")
print("Every report moves whole screen rows, never pixels: yes")
PY
); then
    FAILURE_MESSAGE=$scroll_output
    printf '%s\n' "$scroll_output" >&2
    exit 1
fi

printf '%s\n' "$scroll_output"

# ITEM-621: the decoded wheel opens and moves the retained history.
#
# The scroll burst above put far more text into the bounded ring than one
# screen holds.  A real USB mouse is attached to an xHCI controller; the wheel
# notch is injected as a QEMU input event, so the proof exercises the same
# HID decode -> local_input batch -> console viewport path a physical wheel
# uses, not a keyboard command.  The live and wheel screenshots must differ,
# the production loop must report a wheel report, and the run must have no
# panic.
wheel_output=
if ! wheel_output=$(python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" \
    "$QMP_SOCKET" "$TEST_DIR/wheel-live.ppm" "$TEST_DIR/wheel-page.ppm" \
    2>&1 <<'PY'
import json
import pathlib
import socket
import sys
import time

serial_path = pathlib.Path(sys.argv[1])
monitor_path = pathlib.Path(sys.argv[2])
qmp_path = pathlib.Path(sys.argv[3])
live_path = pathlib.Path(sys.argv[4])
page_path = pathlib.Path(sys.argv[5])


def serial_text():
    return serial_path.read_bytes().decode("ascii", errors="replace")


def wait_serial(predicate, timeout, what):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = serial_text()
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit("panic while waiting for " + what)
        if predicate(text):
            return text
        time.sleep(0.05)
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="%s" deadline=%ds\n'
        % (what, int(timeout)))
    sys.stderr.flush()
    raise SystemExit("timed out waiting for " + what)


def hmp(command):
    monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    monitor.settimeout(5.0)
    monitor.connect(str(monitor_path))
    received = b""
    while b"(qemu)" not in received:
        received += monitor.recv(4096)
    monitor.sendall((command + "\n").encode("ascii"))
    received = b""
    while b"(qemu)" not in received:
        chunk = monitor.recv(4096)
        if not chunk:
            break
        received += chunk
    monitor.close()
    return received


def screendump(path):
    try:
        path.unlink()
    except FileNotFoundError:
        pass
    hmp("screendump " + str(path))
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        if path.exists() and path.stat().st_size > 32:
            return path.read_bytes()
        time.sleep(0.05)
    raise SystemExit("screendump " + str(path) + " did not appear")


def qmp_event(event):
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(5.0)
    client.connect(str(qmp_path))
    greeting = b""
    while b"\n" not in greeting:
        greeting += client.recv(4096)
    client.sendall(b'{"execute":"qmp_capabilities"}\n')
    reply = b""
    while b"\n" not in reply:
        reply += client.recv(4096)
    client.sendall((json.dumps({"execute": "input-send-event",
                                "arguments": {"events": [event]}}) +
                    "\n").encode("ascii"))
    reply = b""
    while b"\n" not in reply:
        reply += client.recv(4096)
    client.close()
    if b'"error"' in reply:
        raise SystemExit("QMP refused the wheel event: " + reply.decode())


# Wait until the scroll burst has reached a prompt again.
wait_serial(lambda text: "hfoc> " in text, 20.0, "the live prompt")
live = screendump(live_path)
qmp_event({"type": "btn", "data": {"button": "wheel-up", "down": True}})
time.sleep(0.5)
page = screendump(page_path)
changed = sum(1 for a, b in zip(live, page) if a != b)
if changed < 4096:
    raise SystemExit(
        "the wheel changed no screen pixels (%d bytes)" % changed)
body = wait_serial(
    lambda text: "mouse_wheels=" in text and
        int(text.rsplit("mouse_wheels=", 1)[1].split()[0]) > 0,
    20.0, "a decoded wheel report")
count = int(body.rsplit("mouse_wheels=", 1)[1].split()[0])
# Return to the live prompt so the next phase starts at a known screen.
for _ in range(64):
    qmp_event({"type": "btn", "data": {"button": "wheel-down", "down": True}})
# The wheel-up/down pair is proven; make sure the next phase starts at the
# live prompt even if a down event was coalesced away by the device model.
time.sleep(0.5)
hmp("sendkey ret")
print("Wheel reports: %d" % count)
print("Wheel up changed the painted page: %d bytes" % changed)
print("Wheel event reached the production HID batch: yes")
PY
); then
    FAILURE_MESSAGE=$wheel_output
    printf '%s\n' "$wheel_output" >&2
    exit 1
fi

printf '%s\n' "$wheel_output"

# ITEM-461/463: the console under load.
#
# The owner's 2026-09-20 report was that the keyboard, the mouse and the redraw
# were all slow and that characters were lost.  This phase drives the same
# guest with a 210-character line (the console's own line editor, one HMP
# keystroke at a time), requires the whole line to reach the console, and reads
# the machine's own numbers: the console queue's drop counter (zero is "no
# character was lost"), the worst line paint, and the worst main-loop turn.
# The `[FB]` record names the requested and actual modes and whether the
# bounded mode was refused (`fallback=1`), so the same proof run with different
# EDIDs is the item's modes and its fallback case.
load_output=
if ! load_output=$(python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" "$MONITOR_LOG" \
    "${HFOC_CONSOLE_HISTORY_EXPECT_DISPLAY:-}" \
    "${HFOC_CONSOLE_HISTORY_EXPECT_FALLBACK:-}" \
    "${HFOC_CONSOLE_HISTORY_EXPECT_SCALE:-}" \
    "${HFOC_CONSOLE_HISTORY_EXPECT_PREFERRED:-}" 2>&1 <<'PY'
import pathlib
import re
import socket
import sys
import time

sys.path.insert(0, "scripts")
import hfoc_console_command as console_command

serial_path = pathlib.Path(sys.argv[1])
socket_path = pathlib.Path(sys.argv[2])
monitor_log = pathlib.Path(sys.argv[3])
expected_display = sys.argv[4] if len(sys.argv) > 4 else ""
expected_fallback = sys.argv[5] if len(sys.argv) > 5 else ""
expected_scale = sys.argv[6] if len(sys.argv) > 6 else ""
expected_preferred = sys.argv[7] if len(sys.argv) > 7 else ""

# ITEM-466: the documented automatic font-scale rule (largest scale in [1,3]
# whose 6x9 grid keeps at least 80 columns and 30 rows).  A run that does not
# pin a scale still gets it checked against the actual mode, so a 4K EDID that
# QEMU hands back as a smaller mode is checked on that mode's own scale.
def expected_scale_for(width, height):
    for candidate in (3, 2, 1):
        if (width // (8 * candidate) >= 80 and
                height // (16 * candidate) >= 30):
            return candidate
    return 1

# ITEM-463: the costs are microseconds from the cycle counter.  A paint of one
# line and a turn of the loop are fractions of a tick on an idle guest, so the
# millisecond reading was zero; the budgets bound the pathology (seconds-long
# stalls) in microseconds and leave room for a loaded runner.  The loop budget
# includes a scroll that a record's own newline caused, so it is larger than
# the paint budget, from which the scroll is subtracted.
PAINT_US_BUDGET = 50000
ITER_US_BUDGET = 500000
# The console's own line editor bounds one line at 128 characters (ITEM-506
# raised it from 127 to the account policy's maximum), so the 200+ character
# stream is several full lines, each submitted and each named by its own
# `UNKNOWN COMMAND` echo.
LOAD_LINE_LENGTH = 80
LOAD_LINE_COUNT = 3
LOAD_LINES = [
    ("q%d" % index) +
    ("abcdefghijklmnopqrstuvwxyz" * 4)[:LOAD_LINE_LENGTH - 2]
    for index in range(LOAD_LINE_COUNT)
]
LOAD_CHARS = LOAD_LINE_LENGTH * LOAD_LINE_COUNT


def serial_text():
    try:
        raw = serial_path.read_bytes().decode("ascii", errors="replace")
    except FileNotFoundError:
        return ""
    # Interrupt-context diagnostics and the console's own scroll records can
    # land in the middle of a console line, so they are removed before a whole
    # marker is searched for.  `[CONSOLE] paint` is kept: the proof reads it.
    text = raw.replace("\r", "")
    text = re.sub(r"\[CONSOLE\] scroll[^\[\r\n]*\r?\n", "", text)
    return re.sub(r"\[(?:IOAPIC|LAPIC|PIC|PIT|IRQ)[^\[\r\n]*\r?\n", "", text)


deadline = time.monotonic() + 120.0
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

    def send_key(key):
        monitor.sendall(("sendkey " + key + "\n").encode("ascii"))
        record(receive_prompt())
        time.sleep(0.03)

    def serial_bytes():
        try:
            return serial_path.read_bytes()
        except FileNotFoundError:
            return b""

    def normalised_serial_bytes():
        # ITEM-463: the echo search must not be split by a console cost record.
        # The `\r` the console's redraw writes is kept: `type_line()` matches
        # the exact `\rhfoc> ` echo, so a view that strips carriage returns
        # would never confirm a typed line.
        data = serial_bytes()
        data = re.sub(rb"\[CONSOLE\] scroll[^\[\r\n]*\r?\n", b"", data)
        data = re.sub(rb"\[(?:IOAPIC|LAPIC|PIC|PIT|IRQ)[^\[\r\n]*\r?\n", b"",
                      data)
        return data

    record(receive_prompt())

    try:
        for line in LOAD_LINES:
            marker = "UNKNOWN COMMAND: " + line

            console_command.type_line(send_key, normalised_serial_bytes, line,
                                      deadline)
            # The answer is not written inside a serial sequence, so an IRQ
            # diagnostic can land inside it; `serial_text()` removes those, and
            # a repeated Enter only ever submits an empty line.
            answered = False
            for _ in range(6):
                send_key("ret")
                window_end = time.monotonic() + 10.0
                while time.monotonic() < window_end:
                    if marker in serial_text():
                        answered = True
                        break
                    time.sleep(0.05)
                if answered:
                    break
            if not answered:
                raise SystemExit(
                    f"the load line was never answered: {line!r}")
    except console_command.LostKeystrokes as exc:
        sys.stderr.write(
            'HFOC-QEMU-MARKER-TIMEOUT marker="console load lines" '
            'deadline=120s\n')
        sys.stderr.flush()
        raise SystemExit(f"timed out typing the load lines: {exc}")

    # The console's own counters: the drop count is the kernel's answer to
    # "was a character lost", not the echo the helper retyped until it matched.
    try:
        console_command.type_line(send_key, normalised_serial_bytes, "kbd",
                                  deadline)
        console_command.submit(
            lambda: send_key("ret"), normalised_serial_bytes,
            # The cost lines follow the counters, so wait for the last one.
            lambda suffix: (True if "[PRODUCTION-LOOP] iter_us=" in serial_text()
                            else None),
            deadline)
    except console_command.LostKeystrokes as exc:
        sys.stderr.write(
            'HFOC-QEMU-MARKER-TIMEOUT marker="kbd counters" deadline=120s\n')
        sys.stderr.flush()
        raise SystemExit(f"timed out reading the kbd counters: {exc}")

text = serial_text()

for line in LOAD_LINES:
    if ("UNKNOWN COMMAND: " + line) not in text:
        raise SystemExit(
            f"a load line did not reach the console whole: {line!r}")

geometry = re.findall(r"^\[FB\] width=(\d+) height=(\d+) pitch=(\d+) "
                      r"cells=(\d+)x(\d+) bpp=(\d+) modes=(\d+) "
                      r"chosen=(\d+)x(\d+) requested=(\d+)x(\d+) "
                      r"actual=(\d+)x(\d+) fallback=(\d+) "
                      r"scale=(\d+) cell_px=(\d+)x(\d+) "
                      r"preferred=(\d+)x(\d+) available_count=(\d+) "
                      r"available=(\S*)$", text, re.M)
if not geometry:
    raise SystemExit("the boot published no [FB] geometry record")
width, height = int(geometry[-1][0]), int(geometry[-1][1])
mode_count = int(geometry[-1][6])
chosen_width, chosen_height = int(geometry[-1][7]), int(geometry[-1][8])
requested_width, requested_height = int(geometry[-1][9]), int(geometry[-1][10])
actual_width, actual_height = int(geometry[-1][11]), int(geometry[-1][12])
fallback = int(geometry[-1][13])
scale = int(geometry[-1][14])
cell_width, cell_height = int(geometry[-1][15]), int(geometry[-1][16])
preferred_width, preferred_height = int(geometry[-1][17]), int(geometry[-1][18])
available_count = int(geometry[-1][19])
available = geometry[-1][20]
if (actual_width, actual_height) != (width, height):
    raise SystemExit(
        f"the [FB] actual mode {actual_width}x{actual_height} is not the "
        f"mode it was initialised with ({width}x{height})")
# ITEM-466 (round 5): the firmware's own preference, not a hardcoded request.
# `requested=` is kept as the legacy name of the preferred mode.
if mode_count == 0:
    raise SystemExit("the boot published no firmware mode count")
if chosen_width == 0 or chosen_height == 0:
    raise SystemExit("the boot published no chosen mode")
if (requested_width, requested_height) != (preferred_width, preferred_height):
    raise SystemExit(
        f"requested={requested_width}x{requested_height} is not the "
        f"preferred mode {preferred_width}x{preferred_height}")
if available_count == 0:
    raise SystemExit("the boot published no available mode")
if not available:
    raise SystemExit("the boot published an empty available= list")
# The fallback flag is derived from the preference, not from a fixed number: a
# mode that is the native one is not a fallback, and one that is not is.
native = (actual_width, actual_height) == (preferred_width, preferred_height)
if fallback != (0 if native else 1):
    raise SystemExit(
        f"fallback={fallback} contradicts the preferred "
        f"{preferred_width}x{preferred_height} and the actual {width}x{height}")
# The automatic font scale is the largest integer scale whose grid still keeps
# the documented minimum of columns and rows; the cell and the grid must follow
# it, so `[FB]` cannot report a scale it did not apply.
if cell_width != 8 * scale or cell_height != 16 * scale:
    raise SystemExit(
        f"the cell {cell_width}x{cell_height} does not follow scale={scale}")
if (width // cell_width) != int(geometry[-1][3]) or \
        (height // cell_height) != int(geometry[-1][4]):
    raise SystemExit(
        f"cells={geometry[-1][3]}x{geometry[-1][4]} does not follow "
        f"cell_px={cell_width}x{cell_height}")
if native and (chosen_width, chosen_height) != (width, height):
    raise SystemExit(
        f"the native actual {width}x{height} is not the chosen "
        f"{chosen_width}x{chosen_height}")
if expected_display:
    want_w, want_h = (int(part) for part in expected_display.split("x"))
    if (width, height) != (want_w, want_h):
        raise SystemExit(
            f"the requested display {expected_display} became {width}x{height}")
if expected_fallback:
    if fallback != int(expected_fallback):
        raise SystemExit(
            f"the display fallback flag is {fallback}, expected "
            f"{expected_fallback}")
if expected_preferred:
    want_pw, want_ph = (int(part) for part in expected_preferred.split("x"))
    if (preferred_width, preferred_height) != (want_pw, want_ph):
        raise SystemExit(
            f"the firmware preference {preferred_width}x{preferred_height} "
            f"is not {expected_preferred}")
# The automatic scale is checked against the actual mode whatever it is: a
# firmware that cannot hand back the EDID preference still has to scale the mode
# it did hand back correctly.
computed_scale = expected_scale_for(width, height)
if scale != computed_scale:
    raise SystemExit(
        f"the font scale is {scale}, but the rule gives {computed_scale} "
        f"for {width}x{height}")
if expected_scale:
    if scale != int(expected_scale):
        raise SystemExit(
            f"the font scale is {scale}, expected {expected_scale}")

drops = re.findall(r"DROPPED=(\d+)", text)
if not drops:
    raise SystemExit("the kbd counters carried no DROPPED=")
if int(drops[-1]) != 0:
    raise SystemExit(f"the console queue dropped {drops[-1]} reports")

# The costs the `kbd` command repeated after the load, which are the same
# numbers the journal keeps as `[CONSOLE] paint` and `[PRODUCTION-LOOP]`.
paints = re.findall(
    r"^\[CONSOLE\] paint lines=(\d+) chars=(\d+) us=(\d+) max_us=(\d+)$",
    text, re.M)
if not paints:
    raise SystemExit("the kbd counters carried no [CONSOLE] paint line")
paint_worst = max(int(entry[3]) for entry in paints)

loops = re.findall(
    r"^\[PRODUCTION-LOOP\] iter_us=(\d+) iter_max_us=(\d+)$", text, re.M)
if not loops:
    raise SystemExit("the kbd counters carried no [PRODUCTION-LOOP] cost line")
iter_worst = max(int(entry[1]) for entry in loops)
if paint_worst > PAINT_US_BUDGET:
    raise SystemExit(
        f"a painted line took {paint_worst} us, over the {PAINT_US_BUDGET} us "
        f"budget")
if iter_worst > ITER_US_BUDGET:
    raise SystemExit(
        f"a main-loop turn took {iter_worst} us, over the {ITER_US_BUDGET} us "
        f"budget")

print(f"Display mode: {width}x{height} preferred={preferred_width}x"
      f"{preferred_height} modes={mode_count} "
      f"chosen={chosen_width}x{chosen_height} fallback={fallback} scale={scale} "
      f"cell_px={cell_width}x{cell_height} available_count={available_count}")
print(f"Load characters: {LOAD_CHARS} in {LOAD_LINE_COUNT} lines")
print(f"Console queue drops: {drops[-1]}")
print(f"Paint worst: {paint_worst}us budget={PAINT_US_BUDGET}us")
print(f"Loop worst: {iter_worst}us budget={ITER_US_BUDGET}us")
print("Every load line reached the console whole: yes")
PY
); then
    FAILURE_MESSAGE=$load_output
    printf '%s\n' "$load_output" >&2
    exit 1
fi

printf '%s\n' "$load_output"

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

echo "QEMU console history test: OK"
