#!/usr/bin/env bash
# Prove the BSP keeps interrupt/timer ownership while application processors
# idle with masked LVTs, SVR enabled, and IF=0.
# Profile: q35, -smp 2, -cpu max.
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

if [[ "${HFOC_SMP_QEMU_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_SMP_QEMU_TEST_TIMEOUT:-120}s" \
        env HFOC_SMP_QEMU_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-2}
QEMU_CPU=${QEMU_CPU:-max}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "SMP proof must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 2 || {
    echo "SMP proof must set QEMU_SMP=2" >&2
    exit 1
}
test "${QEMU_CPU:-}" = max || {
    echo "SMP proof must set QEMU_CPU=max" >&2
    exit 1
}
echo "QEMU SMP profile: machine=q35 smp=2 cpu=max"

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
    "cpu=$QEMU_CPU" \
    "smp=$QEMU_SMP" \
    "image=$IMAGE" >"$METADATA"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/smp-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-SMP QEMU proof failed}" \
        >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$INTERRUPTS_LOG" \
        "$METADATA" "$FAILURE_REASON" "$SERIAL_OFFSET_FILE" \
        "$TRACE_OFFSET_FILE" "$artifact_dir/"
    echo "SMP proof artifacts: $artifact_dir" >&2
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
    -machine "$QEMU_MACHINE" -cpu "$QEMU_CPU" -m 512M -smp "$QEMU_SMP" \
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

serial_path = pathlib.Path(sys.argv[1])
monitor_log = pathlib.Path(sys.argv[2])
interrupts_path = pathlib.Path(sys.argv[3])
monitor_socket = pathlib.Path(sys.argv[4])
serial_offset_path = pathlib.Path(sys.argv[5])
trace_offset_path = pathlib.Path(sys.argv[6])
deadline = time.monotonic() + 60.0

bsp_marker = "[SMP ] BSP lapic="
ap_online_re = re.compile(rb"\[SMP \] AP [0-9]+ online: lapic=")
abc_marker = "UNKNOWN COMMAND: abc. TYPE HELP."
ff_mask_line = (
    "[PIC ] Master mask: 0x00000000000000FF; "
    "slave mask: 0x00000000000000FF"
)

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

def clean_serial_suffix(offset):
    clean = serial_suffix(offset)
    clean = re.sub(
        r"\[(?:IOAPIC|LAPIC-TIMER|PIC|SMP|LAPIC)\][^\r\n]*(?:\r?\n)?",
        "",
        clean,
    )
    return clean.replace("\r\n", "\n")

def new_tick_value(offset):
    clean = clean_serial_suffix(offset)
    match = re.search(r"TICKS:\s*(\d+)\r?\n", clean)
    if match is None:
        match = re.search(r"TICKS:\s*(\d+)\n", clean)
    if match is None:
        return None
    return int(match.group(1))

while time.monotonic() < deadline:
    data = serial_bytes()
    text = data.decode("ascii", errors="replace")
    if ("hfoc> " in text and bsp_marker in text and
            ap_online_re.search(data) is not None and
            ff_mask_line in text):
        break
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic before SMP validation")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="boot prompt, SMP online markers, and FF/FF PIC masks" '
        'deadline=60s\n')
    sys.stderr.flush()
    raise SystemExit(
        "timed out waiting for boot prompt, SMP online markers, and FF/FF "
        "PIC masks")

text = serial_text()
data = serial_bytes()
if bsp_marker not in text:
    raise SystemExit("Missing: [SMP ] BSP lapic marker")
if ap_online_re.search(data) is None:
    raise SystemExit("Missing: [SMP ] AP n online marker")
if ff_mask_line not in text:
    raise SystemExit("Missing exact FF/FF COM1 PIC mask line")
for failure_marker in (
    "[SMP ] AP bring-up failed:",
    "[SMP ] unexpected AP vector",
    "[IOAPIC] route failed: irq=",
    "[LAPIC] LINT0 failed: status=",
    "[LAPIC] AP readback failed",
    "[LAPIC] MMIO map failed",
    "[LAPIC] enable failed",
    "[LAPIC] bad version",
    "[PIC ] retained; PIC disable failed.",
):
    if failure_marker in text:
        raise SystemExit(f"SMP failure marker detected: {failure_marker}")
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("Kernel panic detected")

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
        ticks_request_offset = len(serial_bytes())
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
                str(f"timed out waiting for TICKS snapshot {snapshot} " f"after offset {ticks_request_offset}") + '" '
                'deadline=60s\n')
            sys.stderr.flush()
            raise SystemExit(
                f"timed out waiting for TICKS snapshot {snapshot} "
                f"after offset {ticks_request_offset}")
        if snapshot == 1:
            time.sleep(2.0)

    if ticks[1] <= ticks[0]:
        raise SystemExit(
            f"TICKS did not grow with AP online: {ticks[0]} -> {ticks[1]}")

    serial_offset_path.write_text(
        str(len(serial_bytes())), encoding="ascii")
    trace_offset_path.write_text(
        str(len(interrupts_path.read_bytes())), encoding="ascii")

    abc_request_offset = len(serial_bytes())
    for key in ("a", "b", "c", "ret"):
        command(f"sendkey {key}")
        time.sleep(0.08)

    while time.monotonic() < deadline:
        clean = clean_serial_suffix(abc_request_offset)
        if (clean.count(abc_marker) == 1 and
                clean.rfind("hfoc> ") > clean.find(abc_marker)):
            break
        if "*** HFOC KERNEL PANIC ***" in clean:
            raise SystemExit("Kernel panic during console input")
        time.sleep(0.05)
    else:
        # ITEM-422: a proof that gives up on a marker says so.
        sys.stderr.write(
            'HFOC-QEMU-MARKER-TIMEOUT marker="exactly one console input abc" '
            'deadline=60s\n')
        sys.stderr.flush()
        raise SystemExit(
            "timed out waiting for exactly one console input abc")

    clean = clean_serial_suffix(abc_request_offset)
    if clean.count(abc_marker) != 1:
        raise SystemExit(
            f"expected one console input abc, got {clean.count(abc_marker)}")

    trace_deadline = time.monotonic() + 5.0
    trace_offset = int(trace_offset_path.read_text(encoding="ascii"))
    while time.monotonic() < trace_deadline:
        trace_data = interrupts_path.read_bytes()
        trace_suffix = trace_data[trace_offset:].decode(
            encoding="ascii", errors="replace")
        if ("Servicing hardware INT=0x41" in trace_suffix and
                "Servicing hardware INT=0x42" in trace_suffix):
            break
        time.sleep(0.05)
    else:
        # ITEM-422: a proof that gives up on a marker says so.
        sys.stderr.write(
            'HFOC-QEMU-MARKER-TIMEOUT marker="INT=0x41/INT=0x42 in post-boot trace" '
            'deadline=' + str(int(trace_deadline)) + 's\n')
        sys.stderr.flush()
        raise SystemExit(
            "timed out waiting for INT=0x41/INT=0x42 in post-boot trace")

    trace_data = interrupts_path.read_bytes()
    trace_suffix = trace_data[trace_offset:].decode(
        encoding="ascii", errors="replace")
    if "Servicing hardware INT=0x21" in trace_suffix:
        raise SystemExit("Post-boot trace contains INT=0x21")
    if "Servicing hardware INT=0x20" in trace_suffix:
        raise SystemExit("Post-boot trace contains INT=0x20")
    if "Servicing hardware INT=0x40" in trace_suffix:
        raise SystemExit("Post-boot trace contains INT=0x40")

with monitor_log.open("a", encoding="utf-8") as output:
    output.write("validation complete\n")

print("SMP profile: q35 / smp 2 / cpu max")
print("BSP marker: found")
print("AP online marker: found")
print("Console input exactly once: abc")
print("Trace suffix: INT=0x41 and INT=0x42 found; "
      "INT=0x21, 0x20, and 0x40 absent")
print(f"TICKS before: {ticks[0]}")
print(f"TICKS after: {ticks[1]}")
print("TICKS after > before: yes")
print("PIC masks: master=0xff slave=0xff")
print(f"QEMU info pic: pic0 imr={pic0_imr} pic1 imr={pic1_imr}")
print("Unexpected AP vector: none")
print("AP bring-up failure: none")
print("IOAPIC route failure: none")
print("LAPIC failure: none")
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

echo "QEMU SMP test: OK"
