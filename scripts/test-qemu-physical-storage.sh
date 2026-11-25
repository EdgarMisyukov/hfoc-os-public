#!/usr/bin/env bash
# Prove deterministic physical-style storage, HFOC GPT discovery, checksummed
# journal persistence, and a real QEMU SATA/AHCI block path in macro-gated
# boots. Two boots share one raw SATA disk to verify explicit flush survives
# QEMU restart.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in python3 qemu-system-x86_64 timeout mktemp truncate; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

if [[ "${HFOC_PHYSICAL_STORAGE_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_PHYSICAL_STORAGE_TEST_TIMEOUT:-180}s" \
        env HFOC_PHYSICAL_STORAGE_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Physical-storage test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "Physical-storage test must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "Physical-storage test must not set QEMU_CPU" >&2
    exit 1
}
echo "QEMU physical-storage baseline: machine=q35 smp=1 cpu=default"

TEST_DIR=$(mktemp -d)
AHCI_DISK=$TEST_DIR/ahci-disk.raw
SERIAL_1=$TEST_DIR/com1-phase1.log
SERIAL_2=$TEST_DIR/com1-phase2.log
MONITOR_1=$TEST_DIR/monitor-phase1.sock
MONITOR_2=$TEST_DIR/monitor-phase2.sock
QEMU_LOG=$TEST_DIR/qemu.log
METADATA=$TEST_DIR/metadata.txt
FAILURE_REASON=$TEST_DIR/failure-reason.txt
QEMU_PID=
FAILURE_MESSAGE=

truncate -s 16M "$AHCI_DISK"
: >"$QEMU_LOG"
printf '%s\n' \
    "machine=$QEMU_MACHINE" \
    "cpu=default" \
    "smp=$QEMU_SMP" \
    "image=$IMAGE" \
    "sata=q35-ich9-ahci" \
    "device=ide-hd,bus=ide.0" \
    "disk=16M raw" >"$METADATA"

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
        local timestamp artifact_dir
        timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
        artifact_dir="build/test-artifacts/physical-storage-$timestamp"
        mkdir -p "$artifact_dir"
        printf '%s\n' "${FAILURE_MESSAGE:-Physical-storage test failed}" \
            >"$FAILURE_REASON"
        cp -a "$SERIAL_1" "$SERIAL_2" "$AHCI_DISK" \
            "$QEMU_LOG" "$METADATA" "$FAILURE_REASON" \
            "$artifact_dir/" 2>/dev/null || true
        echo "Physical-storage artifacts: $artifact_dir" >&2
    fi
    best_effort_cleanup
    rm -rf "$TEST_DIR"
    exit "$status"
}

trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

run_boot() {
    local phase_label=$1
    local serial_log=$2
    local monitor_socket=$3
    local disk_mode=$4
    local phase_output

    : >"$serial_log"
    rm -f "$monitor_socket"

    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" \
        -display none -boot order=d,menu=off -cdrom "$IMAGE" \
        -drive "if=none,id=ahci-disk,file=$AHCI_DISK,format=raw,cache=writeback,file.locking=off" \
        -device "ide-hd,drive=ahci-disk,bus=ide.0" \
        -serial "file:$serial_log" \
        -monitor "unix:$monitor_socket,server,nowait" \
        -no-reboot -no-shutdown >"$QEMU_LOG" 2>&1 &
    QEMU_PID=$!

    if ! phase_output=$(python3 - "$serial_log" "$monitor_socket" \
        "$QEMU_PID" "$phase_label" "$disk_mode" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import os
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
monitor_socket = pathlib.Path(sys.argv[2])
qemu_pid = int(sys.argv[3])
phase_label = sys.argv[4]
disk_mode = sys.argv[5]
deadline = time.monotonic() + 80.0

physical_required = [
    "[PHYSICAL-STORAGE] proof init",
    "[PHYSICAL-STORAGE] backend read write flush OK",
    "[PHYSICAL-STORAGE] bounds and malformed rejected",
    "[PHYSICAL-STORAGE] HFOC partition discovered",
    "[PHYSICAL-STORAGE] corrupt GPT rejected",
    "[PHYSICAL-STORAGE] journal replay OK",
    "[PHYSICAL-STORAGE] corrupt journal fails closed",
    "[PHYSICAL-STORAGE] corrupt HFOC header fails closed",
    "[PHYSICAL-STORAGE] clean shutdown",
    "[PHYSICAL-STORAGE] proof OK",
]
state_required = [
    "[STATE-LOADER] proof init",
    "[STATE-LOADER] fresh defaults and flush OK",
    "[STATE-LOADER] committed journal replay OK",
    "[STATE-LOADER] corruption fails closed",
    "[STATE-LOADER] clean shutdown",
    "[STATE-LOADER] proof OK",
]
ahci_required = [
    "[AHCI] proof init",
    "[AHCI] SATA/AHCI controller selected",
    "[AHCI] port DMA start OK",
    "[AHCI] IDENTIFY OK",
    "[AHCI] write flush OK",
    "[AHCI] post-write readback OK",
    "[AHCI] balanced ownership",
    "[AHCI] clean shutdown",
    "[AHCI] proof OK",
]

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
    if "[PHYSICAL-STORAGE] proof OK" in text and \
       "[STATE-LOADER] proof OK" in text and \
       "[AHCI] proof OK" in text:
        break
    if "[PHYSICAL-STORAGE] failed" in text:
        raise SystemExit("Physical-storage proof reported failure")
    if "[STATE-LOADER] failed" in text:
        raise SystemExit("State-loader proof reported failure")
    if "[AHCI] failed" in text:
        raise SystemExit("AHCI proof reported failure")
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic before storage/AHCI proof")
    try:
        os.kill(qemu_pid, 0)
    except OSError:
        raise SystemExit(f"QEMU exited before {phase_label} marker")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="' +
        str(f"timed out waiting for storage/AHCI proof in {phase_label}") + '" '
        'deadline=80s\n')
    sys.stderr.flush()
    raise SystemExit(
        f"timed out waiting for storage/AHCI proof in {phase_label}")

required = physical_required + state_required + ahci_required
if disk_mode == "fresh":
    required.append("[AHCI] fresh disk detected")
elif disk_mode == "persistent":
    required.append("[AHCI] persistent readback OK")
else:
    raise SystemExit(f"invalid AHCI disk mode {disk_mode}")

for marker in required:
    if text.count(marker) != 1:
        raise SystemExit(
            f"Expected 1 marker, got {text.count(marker)}: {marker}")
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("Kernel panic detected")

if not monitor_socket.exists():
    raise SystemExit("timed out waiting for QEMU monitor socket")

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
    monitor.settimeout(2.0)
    try:
        monitor.connect(str(monitor_socket))
    except OSError as exc:
        raise SystemExit(f"QEMU monitor did not accept connection: {exc}")
    monitor_prompt(monitor, 5.0)
    monitor.sendall(b"quit\n")

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
print("Physical-storage proof init: found")
print("Physical-storage proof OK: found")
print("State-loader proof init: found")
print("State-loader proof OK: found")
print("AHCI proof init: found")
print("AHCI proof OK: found")
print(f"AHCI {disk_mode} disk marker: found")
print("Kernel panic: none")
print("QEMU HMP quit: clean")
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

run_boot "Boot 1" "$SERIAL_1" "$MONITOR_1" fresh
run_boot "Boot 2" "$SERIAL_2" "$MONITOR_2" persistent

echo "QEMU physical-storage test: OK"
