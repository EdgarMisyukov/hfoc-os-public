#!/usr/bin/env bash
# ITEM-368 production recovery/factory-reset executor proof: q35 QEMU boots the
# recovery-executor composition against a real AHCI HFOC state partition. The
# in-kernel fixture runs an authorized recovery executor over the production
# storage path: a bounded dry run that must write nothing, a rejected wrong
# confirmation token, and a confirmed factory reset that rewrites the system
# configuration stores as empty data regions, clears service, certificate,
# secret and diagnostic state, and preserves the recovery-metadata foundation.
# The script then verifies the post-transaction disk byte for byte. Physical
# reset is not exercised.
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

if [[ "${HFOC_PRODUCTION_RECOVERY_RESET_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_PRODUCTION_RECOVERY_RESET_TIMEOUT:-180}s" \
        env HFOC_PRODUCTION_RECOVERY_RESET_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/recovery-executor/hfoc-os-$(<VERSION)-recovery-executor.iso}

test "$QEMU_MACHINE" = q35 || {
    echo "Production recovery reset must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "$QEMU_SMP" = 1 || {
    echo "Production recovery reset must set QEMU_SMP=1" >&2
    exit 1
}
test -f "$IMAGE" || {
    echo "Missing recovery-executor image: $IMAGE" >&2
    exit 1
}
echo "QEMU production recovery reset baseline: machine=q35 smp=1 cpu=default"

TEST_DIR=$(mktemp -d)
DISK=$TEST_DIR/hfoc-recovery.raw
DISK_BEFORE=$TEST_DIR/hfoc-recovery.before
SERIAL_LOG=$TEST_DIR/serial.log
QEMU_LOG=$TEST_DIR/qemu.log
MONITOR=$TEST_DIR/monitor.sock
QEMU_PID=

cleanup() {
    local result=$?
    trap - EXIT INT TERM
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    if (( result != 0 )); then
        local artifact_dir
        artifact_dir="build/test-artifacts/production-recovery-reset-$(date -u +%Y%m%dT%H%M%SZ)-$$"
        mkdir -p "$artifact_dir"
        cp -a "$TEST_DIR"/. "$artifact_dir"/ 2>/dev/null || true
        echo "Production recovery reset artifacts: $artifact_dir" >&2
    fi
    rm -rf "$TEST_DIR"
    exit "$result"
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

create_hfoc_disk() {
    python3 - "$1" <<'PY'
import hashlib
import pathlib
import struct
import sys
import zlib

path = pathlib.Path(sys.argv[1])
block_size = 512
block_count = (16 * 1024 * 1024) // block_size
first_lba = 34
partition_blocks = 512
store_blocks = 64
store_count = 8
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

for store in range(store_count):
    base = (first_lba + store * store_blocks) * block_size
    disk[base:base + 8] = b"HFOCDATA"
    disk[base + 8] = 1
    store64(base + 16, 1)
    store64(base + 24, store_blocks - 1)
    for body in range(1, store_blocks):
        offset = base + body * block_size
        disk[offset:offset + block_size] = bytes(
            [0x11 + store] * block_size)

record = b"v1:system.hostname=hfoc-production\n"
offset = (first_lba + 1) * block_size
disk[offset:offset + 8] = b"HFOCJRNL"
disk[offset + 8] = 2
store64(offset + 16, 1)
store32(offset + 24, len(record))
disk[offset + 64:offset + 64 + len(record)] = record
disk[offset + 448:offset + 456] = b"HFOCCOMM"
store64(offset + 456, 1)
disk[offset + 32:offset + 64] = hashlib.sha256(
    disk[offset:offset + 32] +
    disk[offset + 64:offset + 64 + len(record)] +
    disk[offset + 448:offset + 512]
).digest()
path.write_bytes(disk)
PY
}

verify_reset_disk() {
    python3 - "$DISK_BEFORE" "$DISK" <<'PY'
import pathlib
import struct
import sys

before = bytearray(pathlib.Path(sys.argv[1]).read_bytes())
after = bytearray(pathlib.Path(sys.argv[2]).read_bytes())
block_size = 512
first_lba = 34
store_blocks = 64
store_count = 8

if len(before) != len(after):
    raise SystemExit("disk size changed during the recovery proof")

expected = bytearray(before)
for store in range(store_count):
    base = (first_lba + store * store_blocks) * block_size
    if store <= 1 or store == 7:
        # REWRITE_FACTORY_DEFAULT: the SYSTEM_CONFIG stores (hostname, network
        # and, since ITEM-504 increment 6, the route table) keep their region
        # header and have their journal body emptied.
        for body in range(1, store_blocks):
            offset = base + body * block_size
            expected[offset:offset + block_size] = bytes(block_size)
    elif store <= 5:
        # CLEAR: the whole store, header included, is zeroed.
        expected[base:base + store_blocks * block_size] = bytes(
            store_blocks * block_size)
    # store 6 (recovery metadata) is preserved byte for byte.

if after != expected:
    for index in range(len(expected)):
        if after[index] != expected[index]:
            raise SystemExit(
                "recovery transaction mismatch at disk offset 0x%x: "
                "expected 0x%02x, found 0x%02x"
                % (index, expected[index], after[index]))
    raise SystemExit("recovery transaction mismatch")

for store in (0, 1, 7):
    base = (first_lba + store * store_blocks) * block_size
    header = after[base:base + 8]
    if header != b"HFOCDATA":
        raise SystemExit("system store %d lost its data region" % store)
for store in (2, 3, 4, 5):
    base = (first_lba + store * store_blocks) * block_size
    if any(after[base:base + store_blocks * block_size]):
        raise SystemExit("store %d was not cleared" % store)

recovery_base = (first_lba + 6 * store_blocks) * block_size
if after[recovery_base:recovery_base + store_blocks * block_size] != \
        before[recovery_base:recovery_base + store_blocks * block_size]:
    raise SystemExit("recovery metadata store changed")
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
    monitor.connect(sys.argv[1])
    deadline = time.monotonic() + 5.0
    prompt = b""
    while b"(qemu)" not in prompt:
        remaining = deadline - time.monotonic()
        if remaining <= 0.0:
            raise SystemExit("QEMU monitor prompt timeout")
        monitor.settimeout(remaining)
        try:
            prompt += monitor.recv(4096)
        except socket.timeout:
            continue
    monitor.sendall(b"quit\n")
finally:
    monitor.close()
PY
}

run_boot() {
    local deadline=$((SECONDS + 60))

    : >"$SERIAL_LOG"
    : >"$QEMU_LOG"
    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" \
        -display none -boot order=d,menu=off -cdrom "$IMAGE" \
        -drive "if=none,id=hfoc-recovery,file=$DISK,format=raw,cache=writeback,file.locking=off" \
        -device 'ide-hd,drive=hfoc-recovery,bus=ide.0' \
        -serial "file:$SERIAL_LOG" \
        -monitor "unix:$MONITOR,server,nowait" \
        -no-reboot -no-shutdown >"$QEMU_LOG" 2>&1 &
    QEMU_PID=$!

    while (( SECONDS < deadline )); do
        if grep -Fq '[PRODUCTION-RECOVERY] proof OK' "$(serial_view "$SERIAL_LOG")"; then
            break
        fi
        if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$SERIAL_LOG")"; then
            echo "recovery reset: kernel panic" >&2
            return 1
        fi
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            echo "recovery reset: QEMU exited before the proof marker" >&2
            return 1
        fi
        sleep 0.05
    done
    if ! grep -Fq '[PRODUCTION-RECOVERY] proof OK' "$(serial_view "$SERIAL_LOG")"; then
        echo "recovery reset: marker timeout" >&2
        return 1
    fi
    if ! send_monitor_quit "$MONITOR"; then
        echo "recovery reset: QEMU monitor quit failed" >&2
        return 1
    fi
    wait "$QEMU_PID"
    QEMU_PID=
    echo "recovery reset: boot PASS"
}

require_once() {
    local marker=$1
    local count

    count=$(grep -Fc "$marker" "$(serial_view "$SERIAL_LOG")" || true)
    if [[ "$count" != 1 ]]; then
        echo "recovery reset: marker '$marker' count is $count, expected 1" >&2
        return 1
    fi
}

create_hfoc_disk "$DISK"
cp "$DISK" "$DISK_BEFORE"

run_boot

for marker in \
    '[PRODUCTION-BOOT] status=APPLIANCE_PRODUCTION_BOOT_OK' \
    '[PRODUCTION-RECOVERY] proof init' \
    '[PRODUCTION-RECOVERY] executor=READY' \
    '[PRODUCTION-RECOVERY] dry_run=OK writes=0' \
    '[PRODUCTION-RECOVERY] wrong_token=REJECTED' \
    '[PRODUCTION-RECOVERY] apply=APPLIED stores=7 blocks=448 preserved=1' \
    '[PRODUCTION-RECOVERY] recovery_metadata=PRESERVED' \
    '[PRODUCTION-RECOVERY] store_map=VERIFIED' \
    '[PRODUCTION-RECOVERY] proof OK'; do
    require_once "$marker"
done
if grep -Fq '[PRODUCTION-RECOVERY] failed:' "$(serial_view "$SERIAL_LOG")"; then
    echo "recovery reset: fixture reported a failure" >&2
    exit 1
fi

verify_reset_disk
echo "Production recovery reset QEMU test: OK"
