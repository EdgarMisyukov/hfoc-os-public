#!/usr/bin/env bash
# Test PS/2 line editing proof on q35 / one vCPU / default CPU.
# Transport: serial log file + Unix HMP monitor; sendkey editing sequence.
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

if [[ "${HFOC_KEYBOARD_LINE_EDITING_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_KEYBOARD_LINE_EDITING_TEST_TIMEOUT:-90}s" \
        env HFOC_KEYBOARD_LINE_EDITING_TEST_INNER=1 "$0" "$@"
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
PARSER_MODULE=$TEST_DIR/keyboard-parser.py
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
cat >"$PARSER_MODULE" <<'PY'
import sys
import re


def normalise_suffix(suffix):
    return re.sub(
        r"\[LAPIC-TIMER\] irq (?:vector=0x42|lapic_eoi)\r?\n?", "",
        suffix)


def exact_command_result(suffix):
    return suffix.count("UNKNOWN COMMAND: abc. TYPE HELP.") == 1


def exact_command_result_with_prompt(suffix):
    marker = "UNKNOWN COMMAND: abc. TYPE HELP."
    if suffix.count(marker) != 1:
        return False
    marker_index = suffix.rfind(marker)
    return suffix.find("hfoc> ", marker_index) > marker_index


def run_parser_fixtures():
    fixture = ("UNKNOWN COMMAND: abc. TYPE HELP.\n"
               "[LAPIC-TIMER] irq vector=0x42\n"
               "[LAPIC-TIMER] irq lapic_eoi\n"
               "hfoc> \n")
    if not exact_command_result(fixture):
        raise SystemExit(
            "keyboard parser fixture split by periodic diagnostics")
    normalised = normalise_suffix(fixture)
    if not exact_command_result(normalised):
        raise SystemExit(
            "keyboard parser fixture split by periodic diagnostics")
    if not exact_command_result_with_prompt(normalised):
        raise SystemExit(
            "normalised keyboard suffix lacks one abc result and prompt")

    negative = ("[IOAPIC] route failed: irq=1 status=11\n"
                "*** HFOC KERNEL PANIC ***\n"
                "arbitrary line must survive\n")
    if normalise_suffix(negative) != negative:
        raise SystemExit(
            "keyboard normaliser removed a non-diagnostic line")

    split_marker = ("UNKNOWN COMMAND: abc[LAPIC-TIMER] irq vector=0x42\r\n"
                    "[LAPIC-TIMER] irq lapic_eoi\r\n. TYPE HELP.\r\n"
                    "hfoc> \r\n")
    if normalise_suffix(split_marker) != (
            "UNKNOWN COMMAND: abc. TYPE HELP.\r\nhfoc> \r\n"):
        raise SystemExit(
            "keyboard normaliser left diagnostic line endings")

    print("Keyboard line editing parser fixtures: OK")
    print("LAPIC-TIMER diagnostic lines removed: 2")
    print("Negative lines preserved: route failure, panic, arbitrary")


if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "run-fixtures":
    run_parser_fixtures()
PY

if ! parser_self_output=$(python3 "$PARSER_MODULE" run-fixtures 2>&1); then
    printf '%s\n' "$parser_self_output" >&2
    exit 1
fi
printf '%s\n' "$parser_self_output"

printf '%s\n' \
    "machine=$QEMU_MACHINE" \
    "cpu=default" \
    "smp=$QEMU_SMP" \
    "image=$IMAGE" >"$METADATA"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/keyboard-line-editing-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-Keyboard line editing test failed}" \
        >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$INTERRUPTS_LOG" \
        "$METADATA" "$FAILURE_REASON" "$SERIAL_OFFSET_FILE" \
        "$TRACE_OFFSET_FILE" "$artifact_dir/"
    echo "Keyboard line editing test artifacts: $artifact_dir" >&2
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

phase1_output=
if ! phase1_output=$(python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" \
    "$MONITOR_LOG" "$SERIAL_OFFSET_FILE" "$INTERRUPTS_LOG" \
    "$TRACE_OFFSET_FILE" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
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
interrupts_path = pathlib.Path(sys.argv[5])
trace_offset_path = pathlib.Path(sys.argv[6])
deadline = time.monotonic() + 45.0

def serial_bytes():
    try:
        return serial_path.read_bytes()
    except FileNotFoundError:
        return b""

def serial_text():
    return SERIAL_VIEW.normalise(
        serial_bytes().decode("ascii", errors="replace"))

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

    record(receive_prompt())
    offset_path.write_text(str(len(serial_bytes())), encoding="ascii")
    trace_offset_path.write_text(
        str(len(interrupts_path.read_bytes())), encoding="ascii")

    for command in ("sendkey a", "sendkey c", "sendkey left",
                    "sendkey b", "sendkey end", "sendkey backspace",
                    "sendkey c", "sendkey ret"):
        monitor.sendall((command + "\n").encode("ascii"))
        record(receive_prompt())
        time.sleep(0.08)

print("HMP sendkey editing sequence: a c left b end backspace c ret")
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
    "$INTERRUPTS_LOG" "$TRACE_OFFSET" "$PARSER_MODULE" \
    "$ROOT/scripts/hfoc-serial-view.py" \
    2>&1 <<'PY'
import os
import pathlib
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
qemu_pid = int(sys.argv[2])
offset = int(sys.argv[3])
interrupts_path = pathlib.Path(sys.argv[4])
trace_offset = int(sys.argv[5])
exec(open(sys.argv[6]).read())
deadline = time.monotonic() + 20.0
command_marker = "UNKNOWN COMMAND: abc. TYPE HELP."

def serial_bytes():
    try:
        return serial.read_bytes()
    except FileNotFoundError:
        return b""

def suffix_text_raw():
    data = serial_bytes()
    if offset >= len(data):
        return ""
    if offset > 0 and data[offset - 1] not in (0x0a, 0x0d):
        newline = data.find(b"\n", offset)
        if newline < 0:
            return ""
        return data[newline + 1:].decode("ascii", errors="replace")
    return data[offset:].decode("ascii", errors="replace")

def suffix_text():
    return SERIAL_VIEW.normalise(suffix_text_raw())

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
    suffix = normalise_suffix(suffix_text())
    if suffix.count(command_marker) == 1:
        marker_index = suffix.rfind(command_marker)
        if suffix.find("hfoc> ", marker_index) > marker_index:
            break
    if "*** HFOC KERNEL PANIC ***" in suffix_text():
        raise SystemExit("Kernel panic during line editing")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="exactly one abc command result and prompt" '
        'deadline=20s\n')
    sys.stderr.flush()
    raise SystemExit(
        "timed out waiting for exactly one abc command result and prompt")

suffix = normalise_suffix(suffix_text())
if suffix.count(command_marker) != 1:
    raise SystemExit(f"expected one abc command result, got "
                     f"{suffix.count(command_marker)}")
if ("UNKNOWN COMMAND: ac. TYPE HELP." in suffix or
        "UNKNOWN COMMAND: ab. TYPE HELP." in suffix):
    raise SystemExit("intermediate line was submitted as a command")
if "\x1b[" in suffix:
    raise SystemExit("console redraw emitted an ANSI escape")

trace_deadline = time.monotonic() + 5.0
while time.monotonic() < trace_deadline:
    trace_suffix = trace_suffix_text()
    if "Servicing hardware INT=0x21" in trace_suffix:
        raise SystemExit(
            "line editing trace contains Servicing hardware INT=0x21")
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

print("Final submitted command: abc")
print("Command result count: 1")
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

echo "QEMU keyboard line editing test: OK"
