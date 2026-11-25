#!/usr/bin/env bash
# ITEM-508 increment 36: the production order path over the real binding.
#
# The guest is the production kernel plus the fixture anchor and the shared
# fixture clock; the ordinary-boot composition inside it
# (`appliance_production_acme_order_boot.c`) dials the offline stub through the
# lwIP + mbedTLS binding and runs one complete order, and the fixture entry
# asks for exactly that one order because the proof has no SNTP peer and no
# operator request to spend.
#
# What the proof requires, in the serial log:
#   * `[PRODUCTION-ACME-ORDER] state=RUNNING step=DIRECTORY names=1` - the
#     production driver started on the fixture clock;
#   * `[PRODUCTION-ACME-ORDER] state=DONE step=CERTIFICATE members=2 der=...
#     expires=...` - every step ran over verified TLS 1.3, the downloaded
#     chain verified against the fixture anchor, the production certificate
#     owner adopted it and the renewal policy moved to the leaf's expiry;
#   * `[ACME-HTTP01] ... armed=1` - the production listener was armed from the
#     parsed challenge, so the answer the CA would fetch was really served;
#   * no `[PRODUCTION-ACME-ORDER] state=FAILED`, no `ACME-STUB refusal:`, no
#     panic and no private key material in either log.
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

if [[ "${HFOC_ACME_BOOT_ORDER_QEMU_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_ACME_BOOT_ORDER_QEMU_TEST_TIMEOUT:-420}s" \
        env HFOC_ACME_BOOT_ORDER_QEMU_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
QEMU_CPU=${QEMU_CPU:-max}
IMAGE=${IMAGE:-build/acme-boot-order/hfoc-os-$(<VERSION)-acme-boot-order.iso}
PORT=${HFOC_ACME_BOOT_ORDER_QEMU_TEST_PORT:-18449}
NETDEV_DEVICE='virtio-net-pci-non-transitional,netdev=net0,ioeventfd=off,mac=52:54:00:12:34:56,event_idx=off,indirect_desc=off,mq=off,mrg_rxbuf=off,ctrl_vq=off,ctrl_rx=off,ctrl_rx_extra=off,ctrl_vlan=off,ctrl_mac_addr=off,ctrl_guest_offloads=off,gso=off,guest_csum=off,csum=off,guest_tso4=off,guest_tso6=off,guest_ecn=off,guest_ufo=off,host_tso4=off,host_tso6=off,host_ecn=off,host_ufo=off,host_uso=off,guest_uso4=off,guest_uso6=off,guest_announce=off,hash=off,rss=off,guest_rsc_ext=off,vectors=0,x-disable-pcie=on'

test "${QEMU_MACHINE:-}" = q35 || {
    echo "ACME boot-order QEMU test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "ACME boot-order QEMU test must set QEMU_SMP=1" >&2
    exit 1
}
if ! test "$PORT" -ge 1 || ! test "$PORT" -le 65535; then
    echo "ACME boot-order QEMU test port out of range: $PORT" >&2
    exit 1
fi
test -s "$IMAGE" || {
    echo "Missing fixture image: $IMAGE" >&2
    exit 1
}

# The disk the production boot expects to find: a blank acceptance-style volume
# (GPT plus the HFOC state partition), so the production first boot has
# somewhere to commit the certificate and the network path is the production
# one.
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

for store_offset in (0, 64, 128, 256):
    offset = (first_lba + store_offset) * block_size
    disk[offset:offset + 8] = b"HFOCDATA"
    disk[offset + 8] = 1
    store64(offset + 16, 1)
    store64(offset + 24, 16)

path.write_bytes(disk)
PY
}

TEST_DIR=$(mktemp -d)
SERIAL=$TEST_DIR/com1.log
MONITOR=$TEST_DIR/monitor.sock
QEMU_LOG=$TEST_DIR/qemu.log
STUB_LOG=$TEST_DIR/stub.log
DISK=$TEST_DIR/state.img
QEMU_PID=
STUB_PID=

cleanup() {
    if test -n "$QEMU_PID" && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    if test -n "$STUB_PID" && kill -0 "$STUB_PID" 2>/dev/null; then
        kill "$STUB_PID" 2>/dev/null || true
        wait "$STUB_PID" 2>/dev/null || true
    fi
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

# ITEM-526: the console prints one character at a time and an interrupt-context
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

python3 - "$PORT" <<'PY'
import socket
import sys

port = int(sys.argv[1])
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
    probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        probe.bind(("0.0.0.0", port))
    except OSError as error:
        raise SystemExit("port %d is not free: %s" % (port, error))
PY

python3 scripts/hfoc-acme-directory-stub.py --port "$PORT" \
    >"$STUB_LOG" 2>&1 &
STUB_PID=$!

readiness="ACME-STUB listening port=$PORT"
deadline=$((SECONDS + 30))
while ! grep -Fq "$readiness" "$STUB_LOG" 2>/dev/null; do
    if ! kill -0 "$STUB_PID" 2>/dev/null; then
        echo "The ACME boot-order stub died before listening" >&2
        cat "$STUB_LOG" >&2 || true
        exit 1
    fi
    if test "$SECONDS" -ge "$deadline"; then
        echo "Timed out waiting for: $readiness" >&2
        cat "$STUB_LOG" >&2 || true
        exit 1
    fi
    sleep 0.2
done
echo "ACME boot-order stub ready on the host at port $PORT"

create_blank_hfoc_disk "$DISK"
: >"$SERIAL"
qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" -cpu "$QEMU_CPU" \
    -display none -boot order=d,menu=off -cdrom "$IMAGE" \
    -drive "if=none,id=hfoc,file=$DISK,format=raw,cache=writeback,file.locking=off" \
    -device 'ide-hd,drive=hfoc,bus=ide.0' \
    -netdev "user,id=net0" \
    -device "$NETDEV_DEVICE" \
    -serial "file:$SERIAL" \
    -monitor "unix:$MONITOR,server,nowait" \
    -no-reboot -no-shutdown >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

dump_logs() {
    echo '--- serial tail ---' >&2
    tail -30 "$SERIAL" >&2 || true
    echo '--- qemu log tail ---' >&2
    tail -20 "$QEMU_LOG" >&2 || true
    echo '--- stub log tail ---' >&2
    tail -20 "$STUB_LOG" >&2 || true
}

wait_for_exchange() {
    local seconds=$1
    local deadline=$((SECONDS + seconds))

    while (( SECONDS < deadline )); do
        if grep -Fq '[PRODUCTION-ACME-ORDER] state=DONE step=CERTIFICATE' \
                "$(serial_view "$SERIAL")"; then
            return 0
        fi
        if grep -Fq '[PRODUCTION-ACME-ORDER] state=FAILED' "$(serial_view "$SERIAL")"; then
            echo "The production order path failed:" >&2
            grep -F '[PRODUCTION-ACME-ORDER] state=FAILED' "$SERIAL" | tail -1 \
                >&2
            dump_logs
            exit 1
        fi
        if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$SERIAL")"; then
            echo "Kernel panic while waiting for the order exchange" >&2
            dump_logs
            exit 1
        fi
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            echo "QEMU exited before the order exchange" >&2
            dump_logs
            exit 1
        fi
        sleep 0.2
    done
    printf 'HFOC-QEMU-MARKER-TIMEOUT marker="the production order exchange" deadline=%ss\n' \
        "$seconds" >&2
    dump_logs
    exit 1
}

wait_for_exchange 300

grep -Eq '\[PRODUCTION-ACME-ORDER\] state=RUNNING step=DIRECTORY names=1' \
    "$(serial_view "$SERIAL")" || {
    echo "The production driver never announced its start" >&2
    dump_logs
    exit 1
}
grep -Eq '\[PRODUCTION-ACME-ORDER\] state=DONE step=CERTIFICATE members=2 der=[0-9]+ expires=[0-9]+' \
    "$(serial_view "$SERIAL")" || {
    echo "The order record does not name the verified chain and its expiry" >&2
    dump_logs
    exit 1
}
grep -F '[ACME-HTTP01] ' "$(serial_view "$SERIAL")" | grep -Fq 'armed=1' || {
    echo "The production listener was never armed from the challenge" >&2
    dump_logs
    exit 1
}
if grep -Fq 'ACME-STUB refusal:' "$STUB_LOG"; then
    echo "The stub refused a request" >&2
    dump_logs
    exit 1
fi
if grep -Fq '[PRODUCTION-ACME-ORDER] state=FAILED' "$(serial_view "$SERIAL")"; then
    echo "The serial log carries an order failure record" >&2
    exit 1
fi
for log in "$SERIAL" "$STUB_LOG"; do
    if grep -Fq 'PRIVATE KEY' "$log"; then
        echo "A proof log carries private key material: $log" >&2
        exit 1
    fi
done

echo "Production order exchange: OK (port=$PORT)"
grep -F '[PRODUCTION-ACME-ORDER]' "$SERIAL" | tail -3 >&2 || true
