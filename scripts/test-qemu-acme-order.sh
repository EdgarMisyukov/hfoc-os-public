#!/usr/bin/env bash
# ITEM-508 increment 17: the offline ACME order exchange over the real
# binding.
#
# The guest is the production kernel plus one fixture anchor (the increment-14
# ECDSA CA, because the production anchor is RSA and the frozen profile cannot
# parse it) and one driver.  The driver dials the fixed port 18446 through the
# increment-13 lwIP + mbedTLS binding and runs one complete ACME order; the
# host serves the increment-17 stub (`scripts/hfoc-acme-directory-stub.py`) on
# the same fixed port, because QEMU user networking presents the host as the
# gateway literal `10.0.2.2`.
#
# What the proof requires, in the serial log:
#   * `[ACME-ORDER] proof init` - the driver started on a configured
#     network with a published lifecycle;
#   * `[ACME-ORDER] csr bytes=... subject=hfoc-appliance` - the finalize
#     carried a PKCS#10 the driver wrote from the account key (increment 22),
#     and the stub verified its signature and subject with openssl;
#   * `[ACME-ORDER] account thumbprint=<43>` - the account key was bound
#     through the production binding point from the identity view, and the
#     record names the public RFC 7638 thumbprint (increment 25);
#   * `[ACME-ORDER] order ok state=CERTIFICATE_VERIFIED members=2` - every
#     step of the order ran over verified TLS 1.3 and the downloaded chain
#     verified against the fixture anchor;
#   * no `[ACME-ORDER] failed:` record, no `ACME-STUB refusal:` and no panic.
# The stub's chain is not trusted by this script: the guest's own pinned
# fixture anchor is the only trust input, which is the point of the exchange.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in python3 openssl qemu-system-x86_64 timeout mktemp; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

if [[ "${HFOC_ACME_ORDER_QEMU_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_ACME_ORDER_QEMU_TEST_TIMEOUT:-420}s" \
        env HFOC_ACME_ORDER_QEMU_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
QEMU_CPU=${QEMU_CPU:-max}
IMAGE=${IMAGE:-build/acme-order/hfoc-os-$(<VERSION)-acme-order.iso}
PORT=${HFOC_ACME_ORDER_QEMU_TEST_PORT:-18446}
# ITEM-508 increment 31: the host fetches the armed `http-01` answer from guest
# port 80 while the order runs.  The wire proof already forwards guest 80 on
# host 18447 and both proofs run in the same parallel QEMU phase, so this proof
# forwards it on its own third port.
HTTP01_PORT=${HFOC_ACME_ORDER_QEMU_HTTP01_PORT:-18448}
NETDEV_DEVICE='virtio-net-pci-non-transitional,netdev=net0,ioeventfd=off,mac=52:54:00:12:34:56,event_idx=off,indirect_desc=off,mq=off,mrg_rxbuf=off,ctrl_vq=off,ctrl_rx=off,ctrl_rx_extra=off,ctrl_vlan=off,ctrl_mac_addr=off,ctrl_guest_offloads=off,gso=off,guest_csum=off,csum=off,guest_tso4=off,guest_tso6=off,guest_ecn=off,guest_ufo=off,host_tso4=off,host_tso6=off,host_ecn=off,host_ufo=off,host_uso=off,guest_uso4=off,guest_uso6=off,guest_announce=off,hash=off,rss=off,guest_rsc_ext=off,vectors=0,x-disable-pcie=on'

test "${QEMU_MACHINE:-}" = q35 || {
    echo "ACME order QEMU test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "ACME order QEMU test must set QEMU_SMP=1" >&2
    exit 1
}
if ! test "$PORT" -ge 1 || ! test "$PORT" -le 65535; then
    echo "ACME order QEMU test port out of range: $PORT" >&2
    exit 1
fi
if ! test "$HTTP01_PORT" -ge 1 || ! test "$HTTP01_PORT" -le 65535; then
    echo "ACME order QEMU test HTTP-01 port out of range: $HTTP01_PORT" >&2
    exit 1
fi
test "$HTTP01_PORT" != "$PORT" || {
    echo "ACME order QEMU test HTTP-01 port collides with the stub port" >&2
    exit 1
}
test -s "$IMAGE" || {
    echo "Missing fixture image: $IMAGE" >&2
    exit 1
}

# The disk the production boot expects to find.  It is a blank acceptance-style
# volume (GPT plus the HFOC state partition), created exactly like the SNI
# proof's, so the production first boot has somewhere to commit and the network
# path is the production one.
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
FAILURE_MESSAGE=

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

# A port already in use would make the guest reach a stranger, so it fails
# before the stub starts rather than after a misleading handshake.
python3 - "$PORT" "$HTTP01_PORT" <<'PY'
import socket
import sys

for raw in sys.argv[1:]:
    port = int(raw)
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
        echo "The ACME order stub died before listening" >&2
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
echo "ACME order stub ready on the host at port $PORT"

create_blank_hfoc_disk "$DISK"
: >"$SERIAL"
qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" -cpu "$QEMU_CPU" \
    -display none -boot order=d,menu=off -cdrom "$IMAGE" \
    -drive "if=none,id=hfoc,file=$DISK,format=raw,cache=writeback,file.locking=off" \
    -device 'ide-hd,drive=hfoc,bus=ide.0' \
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$HTTP01_PORT-:80" \
    -device "$NETDEV_DEVICE" \
    -serial "file:$SERIAL" \
    -monitor "unix:$MONITOR,server,nowait" \
    -no-reboot -no-shutdown >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

marker_timeout() {
    printf 'HFOC-QEMU-MARKER-TIMEOUT marker="%s" deadline=%ss\n' \
        "$1" "$2" >&2
}

# A failure that says only "handshake failed" cannot be told from a failure
# that never reached the stub; both ends of the exchange are printed.
dump_logs() {
    echo '--- serial tail ---' >&2
    tail -20 "$SERIAL" >&2 || true
    echo '--- stub log tail ---' >&2
    tail -20 "$STUB_LOG" >&2 || true
}

wait_for_exchange() {
    local seconds=$1
    local deadline=$((SECONDS + seconds))

    while (( SECONDS < deadline )); do
        if grep -Fq '[ACME-ORDER] order ok state=CERTIFICATE_VERIFIED' \
                "$(serial_view "$SERIAL")"; then
            return 0
        fi
        if grep -Fq '[ACME-ORDER] failed:' "$(serial_view "$SERIAL")"; then
            FAILURE_MESSAGE=$(grep -F '[ACME-ORDER] failed:' "$(serial_view "$SERIAL")" | tail -1)
            echo "$FAILURE_MESSAGE" >&2
            dump_logs
            exit 1
        fi
        if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$SERIAL")"; then
            FAILURE_MESSAGE="Kernel panic while waiting for the order exchange"
            echo "$FAILURE_MESSAGE" >&2
            dump_logs
            exit 1
        fi
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            FAILURE_MESSAGE="QEMU exited before the order exchange"
            echo "$FAILURE_MESSAGE" >&2
            dump_logs
            exit 1
        fi
        sleep 0.2
    done
    marker_timeout 'the order exchange' "$seconds"
    FAILURE_MESSAGE="Timed out waiting for: the order exchange"
    echo "$FAILURE_MESSAGE" >&2
    dump_logs
    exit 1
}

wait_for_exchange 300

grep -Fq '[ACME-ORDER] proof init' "$(serial_view "$SERIAL")" || {
    echo "The order driver never announced its start" >&2
    exit 1
}
# ITEM-508 increment 22: the finalize must carry a written PKCS#10, and the
# record names its size and subject before the request is sent.
grep -Eq '\[ACME-ORDER\] csr bytes=[0-9]+ subject=hfoc-appliance' "$(serial_view "$SERIAL")" || {
    echo "The serial log carries no CSR record" >&2
    dump_logs
    exit 1
}
# ITEM-508 increment 25: the account key is bound through the account module
# from the identity view, and the record names the public RFC 7638 thumbprint
# (43 unpadded base64url characters) before the account request is sent.  The
# serial line ends with CRLF, so the pattern is not end-anchored.
grep -Eq '\[ACME-ORDER\] account thumbprint=[A-Za-z0-9_-]{43}' "$(serial_view "$SERIAL")" || {
    echo "The serial log carries no account record" >&2
    dump_logs
    exit 1
}
if grep -Fq 'ACME-STUB refusal:' "$STUB_LOG"; then
    echo "The stub refused a request" >&2
    dump_logs
    exit 1
fi
if grep -Fq '[ACME-ORDER] failed:' "$(serial_view "$SERIAL")"; then
    echo "The serial log carries an order failure record" >&2
    exit 1
fi
for log in "$SERIAL" "$STUB_LOG"; do
    if grep -Fq 'PRIVATE KEY' "$log"; then
        echo "A proof log carries private key material: $log" >&2
        exit 1
    fi
done

# ITEM-508 increment 31: the listener was armed from the flow the order driver
# is really driving, so the answer on the wire is the flow's own key
# authorization.  The host reads the public thumbprint the driver published and
# fetches the stub's fixed token from guest port 80, comparing the body byte
# for byte.  The driver keeps the token armed until the listener's own served
# counter moves, so this fetch cannot miss its window.
grep -Fq '[ACME-ORDER] challenge armed=1' "$(serial_view "$SERIAL")" || {
    echo "The order driver never armed the HTTP-01 listener" >&2
    dump_logs
    exit 1
}
THUMBPRINT=$(grep -Eo '\[ACME-ORDER\] account thumbprint=[A-Za-z0-9_-]{43}' \
    "$(serial_view "$SERIAL")" | head -1 | sed 's/.*thumbprint=//')
test -n "$THUMBPRINT" || {
    echo "The account thumbprint could not be read from the serial log" >&2
    dump_logs
    exit 1
}
python3 - "$HTTP01_PORT" "$THUMBPRINT" <<'PY' || {
import socket
import sys

port = int(sys.argv[1])
thumbprint = sys.argv[2]
token = "tok3n-ABC_123"
answer = ("%s.%s" % (token, thumbprint)).encode("ascii")
target = "/.well-known/acme-challenge/" + token
failures = []


def request(path):
    lines = ["GET %s HTTP/1.1" % path, "Host: 10.0.2.15", "Connection: close"]
    payload = ("\r\n".join(lines) + "\r\n\r\n").encode("ascii")
    with socket.create_connection(("127.0.0.1", port), timeout=30) as connection:
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
        raise AssertionError("no header terminator in the answer to %s: %r"
                             % (path, raw))
    return head.split(b"\r\n")[0].decode("ascii", "replace"), body


status, body = request(target)
if not status.startswith("HTTP/1.1 200"):
    failures.append("the armed path answered %r, wanted 200" % status)
if body != answer:
    failures.append("the armed path carried %r, wanted %r" % (body, answer))
status, body = request("/")
if not status.startswith("HTTP/1.1 404") or body != b"":
    failures.append("the root answered %r with body %r, wanted an empty 404"
                    % (status, body))
status, body = request("/.well-known/acme-challenge/wrong-token")
if not status.startswith("HTTP/1.1 404") or body != b"":
    failures.append("a foreign token answered %r with body %r, wanted an "
                    "empty 404" % (status, body))
if failures:
    for failure in failures:
        print("FAIL: " + failure, file=sys.stderr)
    raise SystemExit(1)
print("HTTP-01 answer from the order fixture: served byte for byte")
PY
    echo "The armed HTTP-01 answer was not served to the host" >&2
    dump_logs
    exit 1
}
if grep -Fq '[ACME-HTTP01-FIXTURE]' "$(serial_view "$SERIAL")"; then
    echo "The order kernel carries the HTTP-01 fixture record" >&2
    exit 1
fi
deadline=$((SECONDS + 30))
while ! grep -Fq '[ACME-ORDER] challenge armed=0 served=1' "$(serial_view "$SERIAL")"; do
    if test "$SECONDS" -ge "$deadline"; then
        echo "Timed out waiting for: the armed token to be disarmed" >&2
        dump_logs
        exit 1
    fi
    sleep 0.2
done
grep -F '[ACME-HTTP01] ' "$(serial_view "$SERIAL")" | grep -Fq 'served=1' || {
    echo "The listener record never counted the served answer" >&2
    dump_logs
    exit 1
}

echo "ACME order exchange: OK (port=$PORT, HTTP-01 port=$HTTP01_PORT)"
grep -F '[ACME-ORDER]' "$SERIAL" | tail -3 >&2 || true
