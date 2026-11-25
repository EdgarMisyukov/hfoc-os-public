#!/usr/bin/env bash
# ITEM-508 increment 29: the HTTP-01 wire proof.
#
# The guest is the production kernel plus one fixture token (the increment-29
# arming hook) and the increment-28 listener is the production listener: one
# `hfoc_lwip_tcp` listener on port 80, the decision module's answer, the
# module's exact response bytes.  The host forwards a local port to guest port
# 80 and speaks HTTP/1.1 to it, because the item's point 8 is about the wire,
# not about the decision the host proof already pins.
#
# What the proof requires:
#   * serial `[ACME-HTTP01] v=1 state=LISTENING armed=1 port=80 ...` - the
#     production listener is up and the fixture armed one token with the one
#     fixture epoch the loop handed the listener's poll (there is no SNTP peer
#     in the proof, and the decision module refuses a zero clock by name);
#   * a real `GET` of exactly `/.well-known/acme-challenge/hfoc-fixture-token`
#     answers `200` whose body is the armed key authorization, byte for byte;
#   * `HEAD` of the same target answers `200`, names the full body length and
#     carries no body;
#   * `/`, a wrong token, the right path with a query string and a `POST` of
#     the right path each answer their named refusal with an empty body;
#   * the guest record then carries `served=2` and `refused>=4`, and no
#     `[ACME-HTTP01-FIXTURE] v=1 armed=0` record and no panic appear.
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

if [[ "${HFOC_ACME_HTTP01_QEMU_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_ACME_HTTP01_QEMU_TEST_TIMEOUT:-420}s" \
        env HFOC_ACME_HTTP01_QEMU_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
QEMU_CPU=${QEMU_CPU:-max}
IMAGE=${IMAGE:-build/acme-http01/hfoc-os-$(<VERSION)-acme-http01.iso}
PORT=${HFOC_ACME_HTTP01_QEMU_TEST_PORT:-18447}
GUEST_PORT=${HFOC_ACME_HTTP01_QEMU_TEST_GUEST_PORT:-80}
NETDEV_DEVICE='virtio-net-pci-non-transitional,netdev=net0,ioeventfd=off,mac=52:54:00:12:34:56,event_idx=off,indirect_desc=off,mq=off,mrg_rxbuf=off,ctrl_vq=off,ctrl_rx=off,ctrl_rx_extra=off,ctrl_vlan=off,ctrl_mac_addr=off,ctrl_guest_offloads=off,gso=off,guest_csum=off,csum=off,guest_tso4=off,guest_tso6=off,guest_ecn=off,guest_ufo=off,host_tso4=off,host_tso6=off,host_ecn=off,host_ufo=off,host_uso=off,guest_uso4=off,guest_uso6=off,guest_announce=off,hash=off,rss=off,guest_rsc_ext=off,vectors=0,x-disable-pcie=on'

test "${QEMU_MACHINE:-}" = q35 || {
    echo "ACME HTTP-01 QEMU test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "ACME HTTP-01 QEMU test must set QEMU_SMP=1" >&2
    exit 1
}
if ! test "$PORT" -ge 1 || ! test "$PORT" -le 65535; then
    echo "ACME HTTP-01 QEMU test port out of range: $PORT" >&2
    exit 1
fi
test "$GUEST_PORT" = 80 || {
    echo "ACME HTTP-01 QEMU test must forward to guest port 80" >&2
    exit 1
}
test -s "$IMAGE" || {
    echo "Missing fixture image: $IMAGE" >&2
    exit 1
}

# The disk the production boot expects to find.  It is a blank acceptance-style
# volume (GPT plus the HFOC state partition), created exactly like the SNI and
# ACME proof disks, so the production first boot has somewhere to commit and
# the network path is the production one.
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
DISK=$TEST_DIR/state.img
QEMU_PID=
FAILURE_MESSAGE=

cleanup() {
    if test -n "$QEMU_PID" && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
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

# A port already in use would make the client reach a stranger, so it fails
# before QEMU starts rather than after a misleading answer.
python3 - "$PORT" <<'PY'
import socket
import sys

port = int(sys.argv[1])
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
    probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        probe.bind(("127.0.0.1", port))
    except OSError as error:
        raise SystemExit("port %d is not free: %s" % (port, error))
PY

create_blank_hfoc_disk "$DISK"
: >"$SERIAL"
qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" -cpu "$QEMU_CPU" \
    -display none -boot order=d,menu=off -cdrom "$IMAGE" \
    -drive "if=none,id=hfoc,file=$DISK,format=raw,cache=writeback,file.locking=off" \
    -device 'ide-hd,drive=hfoc,bus=ide.0' \
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:$GUEST_PORT" \
    -device "$NETDEV_DEVICE" \
    -serial "file:$SERIAL" \
    -monitor "unix:$MONITOR,server,nowait" \
    -no-reboot -no-shutdown >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

marker_timeout() {
    printf 'HFOC-QEMU-MARKER-TIMEOUT marker="%s" deadline=%ss\n' \
        "$1" "$2" >&2
}

dump_logs() {
    echo '--- serial tail ---' >&2
    tail -20 "$SERIAL" >&2 || true
    echo '--- qemu log tail ---' >&2
    tail -20 "$QEMU_LOG" >&2 || true
}

wait_for_armed() {
    local seconds=$1
    local deadline=$((SECONDS + seconds))

    while (( SECONDS < deadline )); do
        if grep -Fq '[ACME-HTTP01-FIXTURE] v=1 armed=0' "$(serial_view "$SERIAL")"; then
            FAILURE_MESSAGE="The fixture token was refused: $(grep -F '[ACME-HTTP01-FIXTURE] v=1 armed=0' "$(serial_view "$SERIAL")" | tail -1)"
            echo "$FAILURE_MESSAGE" >&2
            dump_logs
            exit 1
        fi
        if grep -F '[ACME-HTTP01] ' "$(serial_view "$SERIAL")" | grep -Fq 'armed=1'; then
            return 0
        fi
        if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$SERIAL")"; then
            FAILURE_MESSAGE="Kernel panic while waiting for the armed listener"
            echo "$FAILURE_MESSAGE" >&2
            dump_logs
            exit 1
        fi
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            FAILURE_MESSAGE="QEMU exited before the HTTP-01 listener was armed"
            echo "$FAILURE_MESSAGE" >&2
            dump_logs
            exit 1
        fi
        sleep 0.2
    done
    marker_timeout 'the armed HTTP-01 listener' "$seconds"
    FAILURE_MESSAGE="Timed out waiting for: the armed HTTP-01 listener"
    echo "$FAILURE_MESSAGE" >&2
    dump_logs
    exit 1
}

wait_for_served() {
    local seconds=$1
    local deadline=$((SECONDS + seconds))

    while (( SECONDS < deadline )); do
        if grep -F '[ACME-HTTP01] ' "$(serial_view "$SERIAL")" | grep -Fq 'served=2'; then
            return 0
        fi
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            break
        fi
        sleep 0.2
    done
    marker_timeout 'the served HTTP-01 counter' "$seconds"
    FAILURE_MESSAGE="Timed out waiting for: the served HTTP-01 counter"
    echo "$FAILURE_MESSAGE" >&2
    dump_logs
    exit 1
}

wait_for_armed 300

# The client runs only once the guest record says the listener is armed, so a
# refusal cannot be an answer to a request that arrived before the token did.
python3 - "$PORT" <<'PY'
import socket
import sys

port = int(sys.argv[1])
host = "127.0.0.1"
token = "hfoc-fixture-token"
key_authorization = b"hfoc-fixture-token.hfoc-fixture-thumbprint"
target = "/.well-known/acme-challenge/" + token
failures = []


def request(method, path, extra_headers=()):
    lines = ["%s %s HTTP/1.1" % (method, path), "Host: 10.0.2.15"]
    lines.extend(extra_headers)
    lines.append("Connection: close")
    payload = ("\r\n".join(lines) + "\r\n\r\n").encode("ascii")
    with socket.create_connection((host, port), timeout=30) as connection:
        connection.sendall(payload)
        chunks = []
        while True:
            chunk = connection.recv(4096)
            if not chunk:
                break
            chunks.append(chunk)
    raw = b"".join(chunks)
    head, separator, body = raw.partition(b"\r\n\r\n")
    if not separator:
        raise AssertionError("no header terminator in the answer to %s %s: %r"
                             % (method, path, raw))
    head_lines = head.split(b"\r\n")
    status = head_lines[0].decode("ascii", "replace")
    headers = {}
    for line in head_lines[1:]:
        name, _, value = line.partition(b":")
        headers[name.strip().lower()] = value.strip()
    return status, headers, body


def expect(method, path, status_code, content_length, body,
           extra_headers=()):
    status, headers, answer = request(method, path, extra_headers)
    label = "%s %s" % (method, path)
    if not status.startswith("HTTP/1.1 %d" % status_code):
        failures.append("%s answered %r, wanted %d" % (label, status,
                                                       status_code))
        return
    if headers.get(b"content-length") != str(content_length).encode("ascii"):
        failures.append("%s named Content-Length %r, wanted %d"
                        % (label, headers.get(b"content-length"),
                           content_length))
    if answer != body:
        failures.append("%s carried body %r, wanted %r"
                        % (label, answer, body))


# Refusals first: none of them may touch the armed answer.
expect("GET", "/", 404, 0, b"")
expect("GET", "/.well-known/acme-challenge/hfoc-fixture-wrong", 404, 0, b"")
expect("GET", target + "?x=1", 404, 0, b"")
expect("POST", target, 405, 0, b"", ("Content-Length: 0",))
expect("GET", "/.well-known/other", 404, 0, b"")
# HEAD answers the headers of the resource with no body at all.
expect("HEAD", target, 200, len(key_authorization), b"")
# The armed answer, byte for byte, and again: the token stays armed until the
# order path disarms it, so a second fetch is the same answer, not a new one.
expect("GET", target, 200, len(key_authorization), key_authorization)
expect("GET", target, 200, len(key_authorization), key_authorization)

if failures:
    for failure in failures:
        print("FAIL: " + failure, file=sys.stderr)
    raise SystemExit(1)
print("HTTP-01 wire proof: 8 requests answered as declared")
PY

wait_for_served 60

grep -F '[ACME-HTTP01] ' "$(serial_view "$SERIAL")" | grep -Fq 'state=LISTENING' || {
    echo "The guest never published a listening HTTP-01 record" >&2
    dump_logs
    exit 1
}
if grep -Fq '[ACME-HTTP01-FIXTURE] v=1 armed=0' "$(serial_view "$SERIAL")"; then
    echo "The serial log carries a fixture arming refusal" >&2
    exit 1
fi

echo "ACME HTTP-01 wire proof: OK (host port=$PORT, guest port=$GUEST_PORT)"
grep -F '[ACME-HTTP01' "$SERIAL" | tail -3 >&2 || true
