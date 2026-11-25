#!/usr/bin/env bash
# Prove ITEM-335 persistent configuration reboot acceptance with three fresh
# QEMU boots. The host loads and saves a 16 KiB RAM storage region so no
# production disk driver is required.
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

if [[ "${HFOC_PERSISTENT_CONFIG_REBOOT_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_PERSISTENT_CONFIG_REBOOT_TEST_TIMEOUT:-300}s" \
        env HFOC_PERSISTENT_CONFIG_REBOOT_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION)-persistent-config.iso}
STORAGE_BASE=0x1f000000

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Persistent-config reboot acceptance must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "Persistent-config reboot acceptance must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "Persistent-config reboot acceptance must not set QEMU_CPU" >&2
    exit 1
}
test -f "$IMAGE" || {
    echo "Missing persistent-config ISO: $IMAGE" >&2
    exit 1
}
echo "Persistent-config reboot baseline: machine=q35 smp=1 cpu=default"

TEST_DIR=$(mktemp -d)
STORAGE_1=$TEST_DIR/storage-phase1.img
STORAGE_2=$TEST_DIR/storage-phase2.img
STORAGE_3=$TEST_DIR/storage-phase3.img
SERIAL_1=$TEST_DIR/com1-phase1.log
SERIAL_2=$TEST_DIR/com1-phase2.log
SERIAL_3=$TEST_DIR/com1-phase3.log
QEMU_LOG=$TEST_DIR/qemu.log
FAILURE_REASON=$TEST_DIR/failure-reason.txt
MONITOR_1=$TEST_DIR/monitor-phase1.sock
MONITOR_2=$TEST_DIR/monitor-phase2.sock
MONITOR_3=$TEST_DIR/monitor-phase3.sock
QEMU_PID=
FAILURE_MESSAGE=

: >"$QEMU_LOG"

python3 - "$STORAGE_1" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
size = 16384
path.write_bytes(b"\0" * size)
data = bytearray(path.read_bytes())
data[0:8] = b"HFOCREBT"
data[8] = 1
path.write_bytes(data)
PY

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
        timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
        artifact_dir="build/test-artifacts/persistent-config-reboot-acceptance-$timestamp"
        mkdir -p "$artifact_dir"
        printf '%s\n' \
            "${FAILURE_MESSAGE:-Persistent-config reboot acceptance failed}" \
            >"$FAILURE_REASON"
        cp -a "$STORAGE_1" "$STORAGE_2" "$STORAGE_3" \
            "$SERIAL_1" "$SERIAL_2" "$SERIAL_3" \
            "$QEMU_LOG" "$FAILURE_REASON" "$artifact_dir/" 2>/dev/null || true
        echo "Persistent-config reboot artifacts: $artifact_dir" >&2
    fi
    best_effort_cleanup
    rm -rf "$TEST_DIR"
    exit "$status"
}

trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

run_phase() {
    local phase_label=$1
    local input_storage=$2
    local output_storage=$3
    local serial_log=$4
    local monitor_socket=$5
    local required_marker=$6

    : >"$serial_log"
    rm -f "$monitor_socket"

    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" \
        -display none -cdrom "$IMAGE" \
        -serial "file:$serial_log" \
        -monitor "unix:$monitor_socket,server,nowait" \
        -device "loader,file=$input_storage,addr=$STORAGE_BASE,force-raw=on" \
        -no-reboot -no-shutdown >"$QEMU_LOG" 2>&1 &
    QEMU_PID=$!

    local phase_output
    if ! phase_output=$(python3 - "$serial_log" "$monitor_socket" \
        "$QEMU_PID" "$output_storage" "$required_marker" \
        "$phase_label" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import pathlib
import re
import socket
import sys
import time
import os

serial_path = pathlib.Path(sys.argv[1])
monitor_socket = pathlib.Path(sys.argv[2])
qemu_pid = int(sys.argv[3])
output_storage = pathlib.Path(sys.argv[4])
required_marker = sys.argv[5]
phase_label = sys.argv[6]
deadline = time.monotonic() + 90.0

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

def monitor_prompt(monitor, timeout):
    prompt = b""
    prompt_deadline = time.monotonic() + timeout
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
    return prompt

while time.monotonic() < deadline:
    text = serial_text()
    if required_marker in text:
        break
    if "[REBOOT-CONFIG] failed: " in text:
        raise SystemExit(f"{phase_label} reported reboot failure")
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit(f"Kernel panic during {phase_label}")
    try:
        os.kill(qemu_pid, 0)
    except OSError:
        raise SystemExit(f"QEMU exited before {phase_label} marker")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="' +
        str(f"timed out waiting for {phase_label} marker") + '" '
        'deadline=90s\n')
    sys.stderr.flush()
    raise SystemExit(f"timed out waiting for {phase_label} marker")

if not monitor_socket.exists():
    raise SystemExit("timed out waiting for QEMU monitor socket")

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
    monitor.settimeout(2.0)
    try:
        monitor.connect(str(monitor_socket))
    except OSError as exc:
        raise SystemExit(f"QEMU monitor did not accept connection: {exc}")
    monitor_prompt(monitor, 5.0)
    monitor.sendall(
        f'pmemsave 520093696 16384 "{output_storage}"\n'.encode("ascii"))
    response = monitor_prompt(monitor, 5.0).decode("utf-8", errors="replace")
    if "Error" in response or "error" in response:
        raise SystemExit(f"QEMU pmemsave failed for {phase_label}")
    if not output_storage.exists() or output_storage.stat().st_size != 16384:
        raise SystemExit(f"pmemsave produced invalid storage for {phase_label}")
    monitor.sendall(b"quit\n")

try:
    os.kill(qemu_pid, 0)
except OSError:
    raise SystemExit(f"QEMU exited before {phase_label} quit")

exit_deadline = time.monotonic() + 10.0
while time.monotonic() < exit_deadline:
    try:
        os.kill(qemu_pid, 0)
    except OSError:
        break
    time.sleep(0.05)
else:
    raise SystemExit(f"QEMU did not exit after {phase_label} quit")

print(f"{phase_label}: exact")
print("QEMU HMP save and quit: clean")
PY
); then
        FAILURE_MESSAGE=$phase_output
        printf '%s\n' "$phase_output" >&2
        return 1
    fi

    printf '%s\n' "$phase_output"
    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=
}

run_phase "Boot 1" \
    "$STORAGE_1" "$STORAGE_2" "$SERIAL_1" "$MONITOR_1" \
    "[REBOOT-CONFIG] phase1 committed"
run_phase "Boot 2" \
    "$STORAGE_2" "$STORAGE_3" "$SERIAL_2" "$MONITOR_2" \
    "[REBOOT-CONFIG] phase2 invalid committed"
run_phase "Boot 3" \
    "$STORAGE_3" "$STORAGE_3" "$SERIAL_3" "$MONITOR_3" \
    "[REBOOT-CONFIG] phase3 recovery OK"

echo "Persistent configuration reboot acceptance: OK"
