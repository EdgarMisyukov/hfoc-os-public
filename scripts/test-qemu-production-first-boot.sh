#!/usr/bin/env bash
# ITEM-363 ordinary-boot first-boot persistence proof: q35 QEMU boots the
# plain production ISO twice against one writable AHCI HFOC partition. The
# first boot provisions hostname, admin fingerprint, and network and commits
# them; the second boot reloads them without re-running the wizard. QEMU user
# networking supplies the DHCP lease that gates first-boot provisioning.
# Physical first-boot acceptance is not exercised.
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

if [[ "${HFOC_PRODUCTION_FIRST_BOOT_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_PRODUCTION_FIRST_BOOT_TEST_TIMEOUT:-240}s" \
        env HFOC_PRODUCTION_FIRST_BOOT_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Production first-boot test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "Production first-boot test must set QEMU_SMP=1" >&2
    exit 1
}
test -f "$IMAGE" || {
    echo "Production first-boot test requires image: $IMAGE" >&2
    exit 1
}
echo "QEMU production first-boot baseline: machine=q35 smp=1 cpu=default"

TEST_DIR=$(mktemp -d)
DISK=$TEST_DIR/hfoc-first-boot.raw
QEMU_LOG=$TEST_DIR/qemu.log
FAILURE_REASON=$TEST_DIR/failure-reason.txt
FAILURE_MESSAGE=
QEMU_PID=

: >"$QEMU_LOG"

create_blank_hfoc_disk() {
    python3 - "$1" <<'PY'
import pathlib
import struct
import sys
import zlib

path = pathlib.Path(sys.argv[1])
block_size = 512
block_count = (16 * 1024 * 1024) // block_size
first_lba = 34
partition_blocks = 512
disk = bytearray(block_size * block_count)

def store32(offset, value):
    disk[offset:offset + 4] = struct.pack("<I", value)

def store64(offset, value):
    disk[offset:offset + 8] = struct.pack("<Q", value)

header = block_size
disk[header:header + 8] = b"EFI PART"
store32(header + 8, 0x00010000)
store32(header + 12, 92)
store64(header + 24, 1)
store64(header + 32, block_count - 1)
store64(header + 40, first_lba)
store64(header + 48, block_count - 1)
store64(header + 72, 2)
store32(header + 80, 4)
store32(header + 84, 128)

entry = 2 * block_size + 128
disk[entry:entry + 16] = bytes([
    0xE1, 0x44, 0x01, 0x4C, 0x0D, 0x6E, 0x7A, 0x4B,
    0xB4, 0x9D, 0x22, 0x10, 0x33, 0x3D, 0x41, 0x31,
])
store64(entry + 16, 2)
store64(entry + 32, first_lba)
store64(entry + 40, first_lba + partition_blocks - 1)
store32(header + 88, zlib.crc32(disk[2 * block_size:3 * block_size]))
store32(header + 16, 0)
store32(header + 16, zlib.crc32(disk[header:header + 92]))

# Empty data regions for system hostname, system network, service, and secret.
for store_offset in (0, 64, 128, 256):
    offset = (first_lba + store_offset) * block_size
    disk[offset:offset + 8] = b"HFOCDATA"
    disk[offset + 8] = 1
    store64(offset + 16, 1)
    store64(offset + 24, 16)

path.write_bytes(disk)
PY
}

send_monitor_quit() {
    python3 - "$1" <<'PY'
import socket
import sys
import time

monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    monitor.settimeout(1.0)
    try:
        monitor.connect(sys.argv[1])
    except OSError as exc:
        raise SystemExit(f"QEMU monitor did not accept quit: {exc}")
    deadline = time.monotonic() + 5.0
    prompt = b""
    while b"(qemu)" not in prompt:
        remaining = deadline - time.monotonic()
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
    monitor.sendall(b"quit\n")
    monitor.settimeout(5.0)
    while True:
        try:
            chunk = monitor.recv(4096)
        except socket.timeout:
            raise SystemExit("QEMU monitor EOF timeout after quit")
        if not chunk:
            break
finally:
    monitor.close()
PY
}

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/production-first-boot-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-Production first-boot test failed}" \
        >"$FAILURE_REASON"
    cp -a "$TEST_DIR"/*.log "$FAILURE_REASON" "$artifact_dir/" \
        2>/dev/null || true
    echo "Production first-boot artifacts: $artifact_dir" >&2
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    if (( status != 0 )); then
        save_failure_artifacts
    fi
    rm -rf "$TEST_DIR"
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

run_boot() {
    local label=$1
    local expected=$2
    local serial_log=$TEST_DIR/$label.log
    local monitor_socket=$TEST_DIR/$label.monitor
    local deadline=$((SECONDS + 90))

    : >"$serial_log"
    : >"$QEMU_LOG"
    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" \
        -display none -boot order=d,menu=off -cdrom "$IMAGE" \
        -drive "if=none,id=hfoc,file=$DISK,format=raw,cache=writeback,file.locking=off" \
        -device 'ide-hd,drive=hfoc,bus=ide.0' \
        -netdev user,id=net0 \
        -device virtio-net-pci-non-transitional,netdev=net0,ioeventfd=off,\
mac=52:54:00:12:34:56,\
event_idx=off,indirect_desc=off,\
mq=off,mrg_rxbuf=off,ctrl_vq=off,ctrl_rx=off,ctrl_rx_extra=off,\
ctrl_vlan=off,ctrl_mac_addr=off,ctrl_guest_offloads=off,gso=off,\
guest_csum=off,csum=off,guest_tso4=off,guest_tso6=off,guest_ecn=off,\
guest_ufo=off,host_tso4=off,host_tso6=off,host_ecn=off,host_ufo=off,\
host_uso=off,guest_uso4=off,guest_uso6=off,guest_announce=off,\
hash=off,rss=off,guest_rsc_ext=off,vectors=0,x-disable-pcie=on \
        -serial "file:$serial_log" \
        -monitor "unix:$monitor_socket,server,nowait" \
        -no-reboot -no-shutdown >"$QEMU_LOG" 2>&1 &
    QEMU_PID=$!

    while (( SECONDS < deadline )); do
        if grep -Fq "$expected" "$(serial_view "$serial_log")"; then
            break
        fi
        if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$serial_log")"; then
            echo "$label: kernel panic" >&2
            return 1
        fi
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            echo "$label: QEMU exited before expected marker" >&2
            return 1
        fi
        sleep 0.2
    done
    if ! grep -Fq "$expected" "$(serial_view "$serial_log")"; then
        echo "$label: timed out waiting for: $expected" >&2
        return 1
    fi
    if ! send_monitor_quit "$monitor_socket" >/dev/null 2>&1; then
        echo "$label: QEMU monitor quit failed" >&2
        return 1
    fi
    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=
    return 0
}

create_blank_hfoc_disk "$DISK"

if ! run_boot boot1 '[PRODUCTION-FIRST-BOOT] first_boot=provisioned'; then
    FAILURE_MESSAGE="First boot did not provision"
    exit 1
fi
if ! grep -Fq 'hostname=hfoc-appliance' "$TEST_DIR/boot1.log"; then
    FAILURE_MESSAGE="First boot did not publish the committed hostname"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if ! run_boot boot2 '[PRODUCTION-FIRST-BOOT] first_boot=reloaded'; then
    FAILURE_MESSAGE="Second boot did not reload"
    exit 1
fi
if grep -Fq '[PRODUCTION-FIRST-BOOT] first_boot=provisioned' "$TEST_DIR/boot2.log"; then
    FAILURE_MESSAGE="Second boot re-ran the first-boot wizard"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if grep -Fq '*** HFOC KERNEL PANIC ***' "$TEST_DIR/boot1.log" \
        "$TEST_DIR/boot2.log"; then
    FAILURE_MESSAGE="Kernel panic during first-boot proof"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi

echo "First boot provisioning: found"
echo "First boot hostname publication: found"
echo "Second boot reload without wizard: found"
echo "Kernel panic: none"
echo "QEMU production first-boot test: OK"
