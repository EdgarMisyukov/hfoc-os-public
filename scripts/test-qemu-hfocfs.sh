#!/usr/bin/env bash
# ITEM-630: prove HFOCFS persists across three real QEMU boots on one 16 MiB
# ide-hd.  Boot 1 formats and writes phase 1; boot 2 mounts, verifies phase 1
# and writes phase 2; boot 3 mounts and verifies both.  The resulting raw image
# is then decoded by an independent Python reader, not by hfocfs.c.
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

if [[ "${HFOC_HFOCFS_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_HFOCFS_TEST_TIMEOUT:-240}s" \
        env HFOC_HFOCFS_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "HFOCFS test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "HFOCFS test must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "HFOCFS test must not set QEMU_CPU" >&2
    exit 1
}
echo "QEMU HFOCFS baseline: machine=q35 smp=1 cpu=default"

TEST_DIR=$(mktemp -d)
DISK=$TEST_DIR/hfocfs-disk.raw
QEMU_LOG=$TEST_DIR/qemu.log
METADATA=$TEST_DIR/metadata.txt
QEMU_PID=

truncate -s 16M "$DISK"
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
        artifact_dir="build/test-artifacts/hfocfs-$timestamp"
        mkdir -p "$artifact_dir"
        cp -a "$DISK" "$QEMU_LOG" "$METADATA" "$artifact_dir/" 2>/dev/null || true
        echo "HFOCFS artifacts: $artifact_dir" >&2
    fi
    best_effort_cleanup
    rm -rf "$TEST_DIR"
    exit "$status"
}

trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

quit_monitor() {
    local socket=$1
    python3 - "$socket" <<'PY'
import socket
import sys
import time

path = sys.argv[1]
deadline = time.monotonic() + 10.0
while time.monotonic() < deadline:
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(1.0)
        sock.connect(path)
        sock.sendall(b"quit\n")
        sock.close()
        raise SystemExit(0)
    except (OSError, socket.timeout):
        time.sleep(0.05)
raise SystemExit("QEMU monitor did not accept quit")
PY
}

run_boot() {
    local label=$1
    local marker=$2
    local serial_log=$TEST_DIR/serial-$label.log
    local monitor_socket=$TEST_DIR/monitor-$label.sock

    : >"$serial_log"
    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" \
        -display none -cdrom "$IMAGE" \
        -drive "if=none,id=hfoc,file=$DISK,format=raw,cache=writeback,file.locking=off" \
        -device 'ide-hd,drive=hfoc,bus=ide.0' \
        -serial "file:$serial_log" \
        -monitor "unix:$monitor_socket,server,nowait" \
        -no-reboot -no-shutdown >>"$QEMU_LOG" 2>&1 &
    QEMU_PID=$!

    if ! python3 - "$serial_log" "$marker" <<'PY'
import pathlib
import sys
import time

path = pathlib.Path(sys.argv[1])
marker = sys.argv[2]
deadline = time.monotonic() + 75.0

def text():
    try:
        return path.read_bytes().decode("ascii", errors="replace")
    except FileNotFoundError:
        return ""

while time.monotonic() < deadline:
    value = text()
    if marker in value:
        raise SystemExit(0)
    if "[HFOCFS] failed" in value:
        raise SystemExit("HFOCFS proof reported failure")
    if "[AHCI] no QEMU SATA/AHCI disk" in value:
        raise SystemExit("AHCI disk was not found")
    if "*** HFOC KERNEL PANIC ***" in value:
        raise SystemExit("kernel panic before HFOCFS marker")
    time.sleep(0.05)
sys.stderr.write(
    'HFOC-QEMU-MARKER-TIMEOUT marker="' + marker + '" deadline=75s\n')
sys.stderr.flush()
raise SystemExit(f"timed out waiting for marker: {marker}")
PY
    then
        echo "HFOCFS $label boot failed; serial follows" >&2
        sed -n '1,200p' "$serial_log" >&2 || true
        return 1
    fi

    quit_monitor "$monitor_socket"
    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=
    cp "$serial_log" "$TEST_DIR/final-$label.log"
}

run_boot phase1 '[HFOCFS] phase1 wrote'
run_boot phase2 '[HFOCFS] phase2 wrote'
run_boot proof '[HFOCFS] proof OK'

python3 - "$DISK" <<'PY'
import struct
import sys
import zlib

data = open(sys.argv[1], "rb").read()
assert len(data) == 16 * 1024 * 1024, len(data)

def u32(off):
    return struct.unpack_from("<I", data, off)[0]

def u64(off):
    return struct.unpack_from("<Q", data, off)[0]

def slot_valid(off):
    if data[off:off + 7] != b"HFOCFS1":
        return False
    if u32(off + 0x0c) != 4096:
        return False
    return u32(off + 0x1f8) == zlib.crc32(data[off:off + 0x1f8]) & 0xffffffff

assert slot_valid(0) and slot_valid(4096), "both superblock slots invalid"
slot = 0 if u64(0 + 0x18) >= u64(4096 + 0x18) else 4096
generation = u64(slot + 0x18)
assert generation >= 3, generation
commit_block = u64(slot + 0x20)
commit = data[commit_block * 4096:(commit_block + 1) * 4096]
assert commit[:4] == b"BCFH"
assert struct.unpack_from("<H", commit, 4)[0] == 4
record = commit[64:]

def ru32(off):
    return struct.unpack_from("<I", record, off)[0]

def ru64(off):
    return struct.unpack_from("<Q", record, off)[0]

inode_blocks = ru32(8)
node_count = ru32(0x14)
assert inode_blocks >= 1 and node_count >= 3

def inode(ino):
    index = (ino - 1) // 22
    block = ru64(0x18 + index * 8)
    base = block * 4096 + 64 + ((ino - 1) % 22) * 176
    raw = data[base:base + 176]
    assert struct.unpack_from("<I", raw, 0)[0] == ino
    return raw

def lookup(dir_ino, name):
    raw = inode(dir_ino)
    for index in range(18):
        block = struct.unpack_from("<Q", raw, 0x18 + index * 8)[0]
        if not block:
            continue
        payload = data[block * 4096 + 64:block * 4096 + 4096]
        position = 0
        while position + 8 <= len(payload):
            ino, name_len, rec_len = struct.unpack_from("<IHH", payload, position)
            if ino == 0 and name_len == 0 and rec_len == 0:
                break
            assert rec_len >= 16 and rec_len % 8 == 0
            if payload[position + 8:position + 8 + name_len] == name:
                return ino
            position += rec_len
    raise AssertionError(f"missing {name}")

def file_bytes(path):
    ino = 1
    for part in path.strip("/").split("/"):
        ino = lookup(ino, part.encode())
    raw = inode(ino)
    size = struct.unpack_from("<I", raw, 8)[0]
    result = bytearray()
    remaining = size
    for index in range(18):
        if remaining == 0:
            break
        block = struct.unpack_from("<Q", raw, 0x18 + index * 8)[0]
        assert block != 0
        chunk = min(remaining, 4032)
        result += data[block * 4096 + 64:block * 4096 + 64 + chunk]
        remaining -= chunk
    return bytes(result)

assert file_bytes("/phase1") == b"phase-one-bytes"
assert file_bytes("/phase2") == b"phase-two-bytes"
print(f"HFOCFS independent decode: generation={generation} phase1+phase2 bytes OK")
PY

echo "QEMU HFOCFS reboot persistence test: OK"
