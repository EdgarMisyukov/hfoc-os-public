#!/usr/bin/env bash
# Prove the item-344 appliance-boot console command through the normal boot
# console: keyboard sends appliance-boot, canonical bounded state is rendered,
# and HMP quits cleanly.
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

if [[ "${HFOC_APPLIANCE_BOOT_CONSOLE_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_APPLIANCE_BOOT_CONSOLE_TEST_TIMEOUT:-60}s" \
        env HFOC_APPLIANCE_BOOT_CONSOLE_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Appliance boot console test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "Appliance boot console test must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "Appliance boot console test must not set QEMU_CPU" >&2
    exit 1
}
echo "QEMU appliance boot console baseline: machine=q35 smp=1 cpu=default"

TEST_DIR=$(mktemp -d)
SERIAL_LOG=$TEST_DIR/com1.log
MONITOR_LOG=$TEST_DIR/monitor.log
QEMU_LOG=$TEST_DIR/qemu.log
METADATA=$TEST_DIR/metadata.txt
FAILURE_REASON=$TEST_DIR/failure-reason.txt
MONITOR_SOCKET=$TEST_DIR/monitor.sock
QEMU_PID=
FAILURE_MESSAGE=

: >"$SERIAL_LOG"
: >"$MONITOR_LOG"
: >"$QEMU_LOG"

printf '%s\n' \
    "machine=$QEMU_MACHINE" \
    "cpu=default" \
    "smp=$QEMU_SMP" \
    "image=$IMAGE" >"$METADATA"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/appliance-boot-console-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-Appliance boot console test failed}" \
        >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" \
        "$METADATA" "$FAILURE_REASON" "$artifact_dir/"
    echo "Appliance boot console test artifacts: $artifact_dir" >&2
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

QEMU_ARGS=(
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" \
    -display none -cdrom "$IMAGE" \
    -serial "file:$SERIAL_LOG" \
    -monitor "unix:$MONITOR_SOCKET,server,nowait" \
    -no-reboot -no-shutdown
)
qemu-system-x86_64 "${QEMU_ARGS[@]}" >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

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
    python3 - "$MONITOR_SOCKET"   <<'PY'
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

phase1_output=
if ! phase1_output=$(python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" \
    "$MONITOR_LOG" 2>&1 "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import pathlib
import re
import socket
import sys
import time

serial_path = pathlib.Path(sys.argv[1])
socket_path = pathlib.Path(sys.argv[2])
monitor_log = pathlib.Path(sys.argv[3])
deadline = time.monotonic() + 45.0

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
    if "hfoc> " in serial_text():
        break
    if "*** HFOC KERNEL PANIC ***" in serial_text():
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

    def send_key(key):
        monitor.sendall(("sendkey " + key + "\n").encode("ascii"))
        with monitor_log.open("a", encoding="utf-8") as output:
            output.write(receive_prompt())
        time.sleep(0.05)

    receive_prompt()
    key_map = {"-": "minus"}
    for character in "appliance-boot":
        send_key(key_map.get(character, character))
    send_key("ret")

required = [
    "HFOC Appliance\n",
    "Hostname:      hfoc-appliance\n",
    "CPU:           1\n",
    "Storage:       UNAVAILABLE\n",
    "Health:        OK\n",
    "Network:       UNAVAILABLE\n",
    "Services:      UNAVAILABLE\n",
    "Management:    UNAVAILABLE\n",
    "[1] Status\n",
    "[2] Network\n",
    "[3] Diagnostics\n",
    "[4] Reboot\n",
    "[5] Shutdown\n",
    "[6] Recovery\n",
]

command_deadline = time.monotonic() + 30.0
while time.monotonic() < command_deadline:
    text = serial_text()
    if all(marker in text for marker in required):
        break
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic during appliance-boot command")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="appliance-boot command output" '
        'deadline=' + str(int(command_deadline)) + 's\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for appliance-boot command output")

text = serial_text()
memory_line = re.search(r"Memory:        ([0-9]+) MiB\n", text)
boot_memory_line = re.search(
    r"\[MEM \] Usable memory: ([0-9]+) MiB\r?\n", text)
if memory_line is None or boot_memory_line is None:
    raise SystemExit("missing appliance or boot memory report")
if memory_line.group(1) != boot_memory_line.group(1):
    raise SystemExit(
        "appliance memory does not match boot memory: "
        f"appliance={memory_line.group(1)} MiB "
        f"boot={boot_memory_line.group(1)} MiB")
if text.count(memory_line.group(0)) != 1 or \
        text.count(boot_memory_line.group(0)) != 1:
    raise SystemExit("expected one appliance and boot memory report")
for marker in required:
    if text.count(marker) != 1:
        raise SystemExit(
            f"Expected 1 marker, got {text.count(marker)}: {marker}")
if "[APPLIANCE-BOOT] SNAPSHOT FAILED" in text or \
        "[APPLIANCE-BOOT] RENDER FAILED" in text:
    raise SystemExit("appliance-boot command reported a render failure")
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("Kernel panic detected")

print("Appliance boot console output: exact")
print("Kernel panic: none")
print("QEMU alive: yes")
PY
); then
    FAILURE_MESSAGE=$phase1_output
    printf '%s\n' "$phase1_output" >&2
    exit 1
fi

printf '%s\n' "$phase1_output"
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

echo "QEMU appliance boot console test: OK"
