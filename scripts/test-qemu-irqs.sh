#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in python3 qemu-system-x86_64 timeout; do
    command -v "$command" >/dev/null || {
        echo "Не найдена команда: $command" >&2
        exit 1
    }
done

if [[ "${HFOC_IRQ_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM "${HFOC_IRQ_TEST_TIMEOUT:-30}s" \
        env HFOC_IRQ_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_CPU=${QEMU_CPU:-max,-apic}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}
EXPECTED_PIC0_IMR=${EXPECTED_PIC0_IMR:-fc}
EXPECTED_PIC1_IMR=${EXPECTED_PIC1_IMR:-ff}
[[ "$EXPECTED_PIC0_IMR" =~ ^[[:xdigit:]]{2}$ ]] || { echo "Invalid EXPECTED_PIC0_IMR: $EXPECTED_PIC0_IMR" >&2; exit 1; }
[[ "$EXPECTED_PIC1_IMR" =~ ^[[:xdigit:]]{2}$ ]] || { echo "Invalid EXPECTED_PIC1_IMR: $EXPECTED_PIC1_IMR" >&2; exit 1; }
EXPECTED_PIC0_IMR=${EXPECTED_PIC0_IMR,,}
EXPECTED_PIC1_IMR=${EXPECTED_PIC1_IMR,,}
TEST_DIR=$(mktemp -d)
SERIAL_LOG=$TEST_DIR/serial.log
MONITOR_LOG=$TEST_DIR/monitor.log
INTERRUPTS_LOG=$TEST_DIR/interrupts.log
MONITOR_SOCKET=$TEST_DIR/monitor.sock
QEMU_PID=

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/irq-$timestamp"
    mkdir -p "$artifact_dir"
    cp -a "$TEST_DIR"/. "$artifact_dir"/
    echo "IRQ test artifacts: $artifact_dir" >&2
}

stop_qemu() {
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        python3 - "$MONITOR_SOCKET" <<'PY' 2>/dev/null || true
import socket
import sys

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
    monitor.settimeout(1.0)
    monitor.connect(sys.argv[1])
    monitor.sendall(b"quit\n")
PY
        for _ in 1 2 3 4 5; do
            kill -0 "$QEMU_PID" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "$QEMU_PID" 2>/dev/null; then
            kill "$QEMU_PID" 2>/dev/null || true
        fi
        for _ in 1 2 3 4 5; do
            kill -0 "$QEMU_PID" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "$QEMU_PID" 2>/dev/null; then
            kill -KILL "$QEMU_PID" 2>/dev/null || true
        fi
        wait "$QEMU_PID" 2>/dev/null || true
    fi
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

# ITEM-527: the console prints one character at a time and an interrupt-context
# diagnostic can land inside a record it is printing, so a shell assertion that
# greps the raw serial file can miss a record the proof's python half saw.  The
# assertions grep the diagnostics-stripped stream, rendered into a file so a
# reader that stops early cannot SIGPIPE the renderer (ITEM-437).  The rule
# lives in scripts/hfoc-serial-view.py - the same rule the embedded python
# helpers carry - so the two halves of the proof cannot disagree.
serial_view() {
    local view=$1.view

    python3 "$ROOT/scripts/hfoc-serial-view.py" "$1" >"$view"
    printf '%s\n' "$view"
}

trap 'exit 124' TERM
trap 'exit 130' INT

qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -cpu "$QEMU_CPU" \
    -display none -cdrom "$IMAGE" \
    -serial "file:$SERIAL_LOG" \
    -monitor "unix:$MONITOR_SOCKET,server,nowait" \
    -d int -D "$INTERRUPTS_LOG" \
    -no-reboot -no-shutdown &
QEMU_PID=$!

python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" "$MONITOR_LOG" \
    "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
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
deadline = time.monotonic() + 24.0
monitor_log.write_text("", encoding="utf-8")

def serial_text():
    try:
        return SERIAL_VIEW.normalise(
            serial_path.read_text(encoding="ascii", errors="replace"))
    except FileNotFoundError:
        return ""

def wait_for(predicate, description):
    while time.monotonic() < deadline:
        text = serial_text()
        if predicate(text):
            return text
        time.sleep(0.05)
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="' +
        str(f"timed out waiting for {description}") + '" '
        'deadline=24s\n')
    sys.stderr.flush()
    raise TimeoutError(f"timed out waiting for {description}")

wait_for(
    lambda text: "[IRQ ] IF enabled; entering HLT event loop." in text
    and "hfoc> " in text,
    "completed boot and hfoc> prompt",
)

while not socket_path.exists():
    if time.monotonic() >= deadline:
        raise TimeoutError("timed out waiting for QEMU monitor socket")
    time.sleep(0.05)

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
    monitor.settimeout(2.0)
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

    def command(value):
        monitor.sendall(value.encode("ascii") + b"\n")
        record(receive_prompt())

    def send_keys(keys):
        for key in keys:
            command(f"sendkey {key}")
            time.sleep(0.06)

    command("info pic")
    send_keys(("i", "r", "q", "ret"))
    wait_for(lambda text: text.count("IF: ") >= 1, "first irq snapshot")

    send_keys(("a",))
    wait_for(lambda text: "hfoc> a" in text, "keyboard character in COM1")
    send_keys(("backspace", "i", "r", "q", "ret"))
    wait_for(lambda text: text.count("IF: ") >= 2, "second irq snapshot")

PY

stop_qemu
QEMU_PID=

mapfile -t irq_lines < <(grep -E 'IF: (ON|OFF) PIC MASTER MASK: [0-9]+ SLAVE MASK: [0-9]+ TIMER IRQ: [0-9]+ KEYBOARD IRQ: [0-9]+' "$(serial_view "$SERIAL_LOG")")
if (( ${#irq_lines[@]} < 2 )); then
    echo "Expected two numeric IRQ snapshots, found ${#irq_lines[@]}" >&2
    exit 1
fi

first=${irq_lines[0]}
second=${irq_lines[1]}
read -r first_if first_master first_slave first_irq0 first_irq1 < <(
    sed -E 's/.*IF: (ON|OFF) PIC MASTER MASK: ([0-9]+) SLAVE MASK: ([0-9]+) TIMER IRQ: ([0-9]+) KEYBOARD IRQ: ([0-9]+).*/\1 \2 \3 \4 \5/' <<<"$first"
)
read -r second_if second_master second_slave second_irq0 second_irq1 < <(
    sed -E 's/.*IF: (ON|OFF) PIC MASTER MASK: ([0-9]+) SLAVE MASK: ([0-9]+) TIMER IRQ: ([0-9]+) KEYBOARD IRQ: ([0-9]+).*/\1 \2 \3 \4 \5/' <<<"$second"
)

[[ "$first_if" == ON && "$second_if" == ON ]] || { echo "IF is not ON in both snapshots" >&2; exit 1; }
(( first_master == 252 && second_master == 252 )) || { echo "Master PIC mask is not 0xFC" >&2; exit 1; }
(( first_slave == 255 && second_slave == 255 )) || { echo "Slave PIC mask is not 0xFF" >&2; exit 1; }
(( second_irq0 > first_irq0 )) || { echo "IRQ0 did not increase: $first_irq0 -> $second_irq0" >&2; exit 1; }
(( second_irq1 > first_irq1 )) || { echo "IRQ1 did not increase: $first_irq1 -> $second_irq1" >&2; exit 1; }
grep -Fq 'hfoc> a' "$SERIAL_LOG" || { echo "Injected character a is absent from COM1" >&2; exit 1; }

int20_count=$(grep -c 'Servicing hardware INT=0x20' "$INTERRUPTS_LOG" || true)
int21_count=$(grep -c 'Servicing hardware INT=0x21' "$INTERRUPTS_LOG" || true)
(( int20_count >= 2 )) || { echo "Expected repeated INT=0x20, found $int20_count" >&2; exit 1; }
(( int21_count >= 1 )) || { echo "Expected at least one INT=0x21, found $int21_count" >&2; exit 1; }

mapfile -t pic0_masks < <(sed -nE 's/^[[:space:]]*pic0:.*[[:space:]]imr=([[:xdigit:]]+).*/\1/p' "$MONITOR_LOG")
mapfile -t pic1_masks < <(sed -nE 's/^[[:space:]]*pic1:.*[[:space:]]imr=([[:xdigit:]]+).*/\1/p' "$MONITOR_LOG")
(( ${#pic0_masks[@]} == 1 )) || { echo "Expected one pic0 IMR in info pic, found ${#pic0_masks[@]}" >&2; exit 1; }
(( ${#pic1_masks[@]} == 1 )) || { echo "Expected one pic1 IMR in info pic, found ${#pic1_masks[@]}" >&2; exit 1; }
[[ "${pic0_masks[0],,}" == "$EXPECTED_PIC0_IMR" ]] || {
    echo "QEMU pic0 IMR mismatch: expected $EXPECTED_PIC0_IMR, got ${pic0_masks[0]}" >&2
    exit 1
}
[[ "${pic1_masks[0],,}" == "$EXPECTED_PIC1_IMR" ]] || {
    echo "QEMU pic1 IMR mismatch: expected $EXPECTED_PIC1_IMR, got ${pic1_masks[0]}" >&2
    exit 1
}

echo "IRQ snapshot before input: IRQ0=$first_irq0 IRQ1=$first_irq1"
echo "IRQ snapshot after input:  IRQ0=$second_irq0 IRQ1=$second_irq1"
echo "Hardware interrupt trace: INT=0x20 $int20_count; INT=0x21 $int21_count"
echo "QEMU info pic:"
sed -nE '/^[[:space:]]*pic[01]:/p' "$MONITOR_LOG"
echo "QEMU IRQ0/IRQ1 smoke test: OK"
