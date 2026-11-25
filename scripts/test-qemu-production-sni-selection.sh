#!/usr/bin/env bash
# ITEM-507 increment 6: two declared names are served two different
# certificates by the production SNI selection, on a live listener.
#
# Increment 4 proved the mutation and the refusal of an undeclared name on the
# ordinary production boot.  This proof answers the item's own evidence
# requirement - "two different SNI names receive different certificates" - on a
# fixture boot that carries two pairs and declares both names:
#
#   * the fixture writes the two test-only pairs through the same durable
#     writer an operator commit uses, declares `default=REFUSE`, and publishes
#     each entry's certificate SHA-256 prefix;
#   * the boot loads the record from the state volume, parses both pairs and
#     registers the SNI callback (the production path);
#   * a real TLS 1.3 client presents `alpha.example`, `ALPHA.example`,
#     `beta.example`, an undeclared name and no name at all, and the proof
#     requires the two declared names to receive the two distinct certificates
#     the fixture published, the undeclared name to be refused and the client
#     without SNI to receive the appliance's own identity;
#   * the serial log must carry one selection record per handshake, and the
#     second boot must read the same two certificates back from the committed
#     record, which is what makes the declaration durable rather than a
#     boot-time accident.
#
# The fixture is a build of its own (`HFOC_APPLIANCE_CERTIFICATE_NAME_FIXTURE_QEMU_TEST`):
# the item's import gate refuses a pair until the wall clock is validated, and
# the QEMU fixture cannot reach an SNTP server (UDP/123 is privileged on the
# test host), so a proof cannot declare a name through the operator API here.
# Everything the proof observes after the record is written is production code.
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

if [[ "${HFOC_PRODUCTION_SNI_SELECTION_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_PRODUCTION_SNI_SELECTION_TEST_TIMEOUT:-600}s" \
        env HFOC_PRODUCTION_SNI_SELECTION_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
QEMU_CPU=${QEMU_CPU:-max}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION)-sni-fixture.iso}
SNI_SELECTION_PORT=${HFOC_PRODUCTION_SNI_SELECTION_PORT:-$((30000 + ($$ % 20000)))}
SNI_DURABILITY_PORT=$((SNI_SELECTION_PORT + 1))
NETDEV_DEVICE='virtio-net-pci-non-transitional,netdev=net0,ioeventfd=off,mac=52:54:00:12:34:56,event_idx=off,indirect_desc=off,mq=off,mrg_rxbuf=off,ctrl_vq=off,ctrl_rx=off,ctrl_rx_extra=off,ctrl_vlan=off,ctrl_mac_addr=off,ctrl_guest_offloads=off,gso=off,guest_csum=off,csum=off,guest_tso4=off,guest_tso6=off,guest_ecn=off,guest_ufo=off,host_tso4=off,host_tso6=off,host_ecn=off,host_ufo=off,host_uso=off,guest_uso4=off,guest_uso6=off,guest_announce=off,hash=off,rss=off,guest_rsc_ext=off,vectors=0,x-disable-pcie=on'

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Production SNI selection test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "Production SNI selection test must set QEMU_SMP=1" >&2
    exit 1
}
test -s "$IMAGE" || {
    echo "Missing fixture image: $IMAGE" >&2
    exit 1
}

TEST_DIR=$(mktemp -d)
DISK=$TEST_DIR/hfoc-state.img
FAILURE_MESSAGE=
QEMU_PID=

cleanup() {
    if [[ -n "${QEMU_PID:-}" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
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

# The same initialized-but-empty HFOC state volume the production proof uses:
# the appliance's own first boot finds it ready, so the fixture's durable write
# and the account it sets both land on a volume the ordinary boot accepts.
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

send_monitor_quit() {
    python3 - "$1" <<'PY'
import socket
import sys
import time

path = sys.argv[1]
for _ in range(100):
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(2)
            sock.connect(path)
            sock.sendall(b"quit\n")
        break
    except OSError:
        time.sleep(0.1)
else:
    raise SystemExit("QEMU monitor did not accept quit")
PY
}

# ITEM-421/ITEM-422: a proof that gives up says which marker it awaited and for
# how long, so the suite can classify a starved-but-healthy guest instead of
# reading a bare timeout.
marker_timeout() {
    printf 'HFOC-QEMU-MARKER-TIMEOUT marker="%s" deadline=%ss\n' \
        "$1" "$2" >&2
}

wait_for_pattern() {
    local serial=$1
    local pattern=$2
    local marker=$3
    local seconds=$4
    local deadline=$((SECONDS + seconds))

    while (( SECONDS < deadline )); do
        if grep -Eq "$pattern" "$(serial_view "$serial")"; then
            return 0
        fi
        if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$serial")"; then
            FAILURE_MESSAGE="Kernel panic while waiting for $marker"
            echo "$FAILURE_MESSAGE" >&2
            exit 1
        fi
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            FAILURE_MESSAGE="QEMU exited before $marker"
            echo "$FAILURE_MESSAGE" >&2
            exit 1
        fi
        sleep 0.2
    done
    marker_timeout "$marker" "$seconds"
    FAILURE_MESSAGE="Timed out waiting for: $marker"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
}

# A refusal's record is awaited with a bound, but unlike the affirmative wait
# above a timeout here is not fatal by itself: the caller repeats the refusal
# handshake, because a connection the guest closes before it has read the
# ClientHello looks exactly like a refusal to the client and publishes no
# decision at all.  A panic or a dead guest is still fatal.
refusal_record_present() {
    local deadline=$((SECONDS + 15))

    while (( SECONDS < deadline )); do
        if grep -Fq 'name=unknown.example result=REFUSED cert=none reason=NOT_FOUND' \
                "$(serial_view "$SELECTION_SERIAL")"; then
            return 0
        fi
        if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$SELECTION_SERIAL")"; then
            FAILURE_MESSAGE="Kernel panic while waiting for the refusal record"
            echo "$FAILURE_MESSAGE" >&2
            exit 1
        fi
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            FAILURE_MESSAGE="QEMU exited before the refusal record"
            echo "$FAILURE_MESSAGE" >&2
            exit 1
        fi
        sleep 0.2
    done
    return 1
}

# Extract the certificate fingerprint the fixture published for one name.  The
# guest writes CRLF, so the record's own text is matched without anchoring the
# end of the line.
fixture_fingerprint() {
    local serial=$1
    local name=$2

    sed -n "s/^\[CERTIFICATE-FIXTURE\] name=$name index=[0-9]* certificate_sha256=\([0-9a-f]*\) key=present.*$/\1/p" \
        "$(serial_view "$serial")" | tail -n 1
}

create_blank_hfoc_disk "$DISK"

# ---------------------------------------------------------------- first boot
SELECTION_SERIAL=$TEST_DIR/selection-boot.log
SELECTION_QEMU_LOG=$TEST_DIR/selection-qemu.log
SELECTION_MONITOR=$TEST_DIR/selection-monitor.sock
: >"$SELECTION_SERIAL"
qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" -cpu "$QEMU_CPU" \
    -display none -boot order=d,menu=off -cdrom "$IMAGE" \
    -drive "if=none,id=hfoc,file=$DISK,format=raw,cache=writeback,file.locking=off" \
    -device 'ide-hd,drive=hfoc,bus=ide.0' \
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$SNI_SELECTION_PORT-:443" \
    -device "$NETDEV_DEVICE" \
    -serial "file:$SELECTION_SERIAL" \
    -monitor "unix:$SELECTION_MONITOR,server,nowait" \
    -no-reboot -no-shutdown >"$SELECTION_QEMU_LOG" 2>&1 &
QEMU_PID=$!

wait_for_pattern "$SELECTION_SERIAL" \
    '\[CERTIFICATE-FIXTURE\] seeded=YES persisted=OK source=COMMITTED count=2' \
    'the fixture record was committed' 180
wait_for_pattern "$SELECTION_SERIAL" \
    '\[CERTIFICATE-FIXTURE\] name=beta.example index=1 certificate_sha256=[0-9a-f]{32} key=present' \
    'the fixture published both declared names' 60
wait_for_pattern "$SELECTION_SERIAL" \
    '\[PRODUCTION-SERVING\] .*listener=live' \
    'the management listener came up' 180

if ! grep -Eq \
        '\[CERTIFICATE-FIXTURE\] account=PROVISIONED' "$(serial_view "$SELECTION_SERIAL")"; then
    FAILURE_MESSAGE="The fixture did not set the test account"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
SELECTION_ALPHA=$(fixture_fingerprint "$SELECTION_SERIAL" alpha.example)
SELECTION_BETA=$(fixture_fingerprint "$SELECTION_SERIAL" beta.example)
if [[ -z "$SELECTION_ALPHA" || -z "$SELECTION_BETA" ]]; then
    FAILURE_MESSAGE="The fixture published no certificate fingerprint"
    echo "$FAILURE_MESSAGE" >&2
    grep -F '[CERTIFICATE-FIXTURE]' "$SELECTION_SERIAL" >&2 || true
    exit 1
fi
if [[ "$SELECTION_ALPHA" == "$SELECTION_BETA" ]]; then
    FAILURE_MESSAGE="The fixture declared two identical certificates"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi

cat >"$TEST_DIR/sni-selection-client.py" <<'CLIENTEOF'
import hashlib
import socket
import ssl
import sys
import time

port = int(sys.argv[1])
alpha = sys.argv[2]
beta = sys.argv[3]
mode = sys.argv[4]

context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
context.check_hostname = False
context.verify_mode = ssl.CERT_NONE
context.minimum_version = ssl.TLSVersion.TLSv1_3
context.maximum_version = ssl.TLSVersion.TLSv1_3


def handshake_once(name):
    with socket.create_connection(("127.0.0.1", port), timeout=90) as sock:
        if name is None:
            with context.wrap_socket(sock) as tls:
                version = tls.version()
                der = tls.getpeercert(binary_form=True)
        else:
            with context.wrap_socket(sock, server_hostname=name) as tls:
                version = tls.version()
                der = tls.getpeercert(binary_form=True)
    if version != "TLSv1.3":
        raise SystemExit("unexpected TLS version: %s" % version)
    if not der:
        raise SystemExit("the handshake carried no peer certificate")
    return hashlib.sha256(der).hexdigest()[:32]


def handshake(name):
    """
    One affirmative handshake, retried while the listener closes the
    connection instead of answering it.

    The fixture's listener serves from one lwIP pool and releases a finished
    connection on a later loop turn; when the guest is starved (the CI QEMU
    suite runs several proofs at once), the next connection can arrive before
    the previous one is released and the listener closes it.  The retry does
    not weaken the assertion - the handshake still has to complete and carry
    the certificate the proof compares - it only waits for the guest to be
    ready for it, which is what a real client does too.  A *refusal* is never
    retried: the callers that expect one call `handshake_once` directly.
    """
    last = None
    for attempt in range(4):
        try:
            return handshake_once(name)
        except (ssl.SSLError, OSError) as error:
            last = error
            time.sleep(0.5 * (attempt + 1))
    raise SystemExit("handshake for %r failed after 4 attempts: %s"
                     % (name, last))


if mode == "selection":
    alpha_upper = handshake("ALPHA.example")
    alpha_exact = handshake("alpha.example")
    beta_exact = handshake("beta.example")
    if alpha_exact != alpha:
        raise SystemExit("alpha.example was served %s, not the declared %s"
                         % (alpha_exact, alpha))
    if alpha_upper != alpha:
        raise SystemExit("ALPHA.example was served %s, not the declared %s"
                         % (alpha_upper, alpha))
    if beta_exact != beta:
        raise SystemExit("beta.example was served %s, not the declared %s"
                         % (beta_exact, beta))
    if alpha_exact == beta_exact:
        raise SystemExit("both names were served the same certificate")
    print("TLS-PROOF SNI selection: alpha.example and ALPHA.example received "
          "the declared alpha certificate and beta.example the declared beta "
          "certificate")
elif mode == "refuse":
    try:
        handshake_once("unknown.example")
    except (ssl.SSLError, OSError):
        pass
    else:
        raise SystemExit("an undeclared SNI name was served")
    print("TLS-PROOF SNI refusal: unknown.example was refused")
elif mode == "default":
    default = handshake(None)
    if default in (alpha, beta):
        raise SystemExit("a client without SNI was served a declared name's "
                         "certificate")
    print("TLS-PROOF SNI default: a client without SNI received the "
          "appliance's own identity")
else:
    raise SystemExit("unknown SNI selection client mode: %s" % mode)
CLIENTEOF

python3 "$TEST_DIR/sni-selection-client.py" \
    "$SNI_SELECTION_PORT" "$SELECTION_ALPHA" "$SELECTION_BETA" selection

# The callback publishes a present name's record inside the handshake, so the
# selection handshakes' records are read once the client has returned.
if ! grep -Eq 'name=(alpha|ALPHA)\.example result=EXACT cert=0' \
        "$(serial_view "$SELECTION_SERIAL")"; then
    FAILURE_MESSAGE="The selection record did not name alpha.example"
    echo "$FAILURE_MESSAGE" >&2
    grep -F '[PRODUCTION-TLS-SNI]' "$SELECTION_SERIAL" >&2 || true
    exit 1
fi
if [[ "$(grep -Ec 'name=(alpha|ALPHA)\.example result=EXACT cert=0' \
        "$(serial_view "$SELECTION_SERIAL")")" -lt 2 ]]; then
    FAILURE_MESSAGE="Both alpha.example handshakes published no selection record"
    echo "$FAILURE_MESSAGE" >&2
    grep -F '[PRODUCTION-TLS-SNI]' "$SELECTION_SERIAL" >&2 || true
    exit 1
fi
if ! grep -Fq 'name=beta.example result=EXACT cert=1' "$(serial_view "$SELECTION_SERIAL")"; then
    FAILURE_MESSAGE="The selection record did not name beta.example"
    echo "$FAILURE_MESSAGE" >&2
    grep -F '[PRODUCTION-TLS-SNI]' "$SELECTION_SERIAL" >&2 || true
    exit 1
fi

# The refusal.  A connection the guest closes before it has read the
# ClientHello looks like a refusal to the client but publishes no decision, so
# the attempt is repeated until the record appears.  The assertion is not
# weakened by the repetition: a handshake that *succeeds* for the undeclared
# name fails the proof at once and is never retried.
refused=0
for attempt in 1 2 3; do
    if ! python3 "$TEST_DIR/sni-selection-client.py" \
            "$SNI_SELECTION_PORT" "$SELECTION_ALPHA" "$SELECTION_BETA" refuse
    then
        FAILURE_MESSAGE="An undeclared SNI name was served"
        echo "$FAILURE_MESSAGE" >&2
        grep -F '[PRODUCTION-TLS-SNI]' "$SELECTION_SERIAL" >&2 || true
        exit 1
    fi
    if refusal_record_present; then
        refused=1
        break
    fi
    echo "TLS-PROOF SNI refusal: attempt $attempt published no record; retrying"
done
if (( refused == 0 )); then
    FAILURE_MESSAGE="The refused name published no selection record"
    echo "$FAILURE_MESSAGE" >&2
    grep -F '[PRODUCTION-TLS-SNI]' "$SELECTION_SERIAL" >&2 || true
    exit 1
fi

# A client that presents no name is recorded when the server's own handshake
# completes, which can trail the client's return by one guest turn: the record
# is awaited with a bound instead of read once.
python3 "$TEST_DIR/sni-selection-client.py" \
    "$SNI_SELECTION_PORT" "$SELECTION_ALPHA" "$SELECTION_BETA" default
wait_for_pattern "$SELECTION_SERIAL" \
    'name=- result=DEFAULT cert=default' \
    'the client without SNI published its selection record' 20
if ! grep -Fq 'name=- result=DEFAULT cert=default' "$(serial_view "$SELECTION_SERIAL")"; then
    FAILURE_MESSAGE="The client without SNI published no selection record"
    echo "$FAILURE_MESSAGE" >&2
    grep -F '[PRODUCTION-TLS-SNI]' "$SELECTION_SERIAL" >&2 || true
    exit 1
fi
# The fixture's own test credential must never reach the serial log, and no
# selection or fixture record may render key material.
if grep -Fq 'Hfoc-507-fixture-secret' "$(serial_view "$SELECTION_SERIAL")"; then
    FAILURE_MESSAGE="The fixture credential leaked into the serial log"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if grep -Eq '\[CERTIFICATE-FIXTURE\].*(PRIVATE KEY|BEGIN )' \
        "$(serial_view "$SELECTION_SERIAL")"; then
    FAILURE_MESSAGE="The fixture record leaked key material"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
echo "TLS-PROOF SNI selection: two declared names, one declared refusal and one no-SNI handshake match their records"

if ! send_monitor_quit "$SELECTION_MONITOR" >/dev/null 2>&1; then
    FAILURE_MESSAGE="QEMU monitor quit failed after the selection boot"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=

# ----------------------------------------------------------- durability boot
# The record the first boot wrote is the one the second boot reads: the fixture
# writes nothing here, and the two certificates the client receives must be the
# same two certificates, byte for byte, that the first boot declared.
DURABILITY_SERIAL=$TEST_DIR/durability-boot.log
DURABILITY_QEMU_LOG=$TEST_DIR/durability-qemu.log
DURABILITY_MONITOR=$TEST_DIR/durability-monitor.sock
: >"$DURABILITY_SERIAL"
qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" -cpu "$QEMU_CPU" \
    -display none -boot order=d,menu=off -cdrom "$IMAGE" \
    -drive "if=none,id=hfoc,file=$DISK,format=raw,cache=writeback,file.locking=off" \
    -device 'ide-hd,drive=hfoc,bus=ide.0' \
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$SNI_DURABILITY_PORT-:443" \
    -device "$NETDEV_DEVICE" \
    -serial "file:$DURABILITY_SERIAL" \
    -monitor "unix:$DURABILITY_MONITOR,server,nowait" \
    -no-reboot -no-shutdown >"$DURABILITY_QEMU_LOG" 2>&1 &
QEMU_PID=$!

wait_for_pattern "$DURABILITY_SERIAL" \
    '\[CERTIFICATE-FIXTURE\] seeded=NO persisted=NOT_WRITTEN source=COMMITTED count=2' \
    'the committed record was read back' 180
wait_for_pattern "$DURABILITY_SERIAL" \
    '\[CERTIFICATE-FIXTURE\] name=beta.example index=1 certificate_sha256=[0-9a-f]{32} key=present' \
    'the durability boot published both declared names' 60
wait_for_pattern "$DURABILITY_SERIAL" \
    '\[PRODUCTION-SERVING\] .*listener=live' \
    'the management listener came up again' 180

DURABILITY_ALPHA=$(fixture_fingerprint "$DURABILITY_SERIAL" alpha.example)
DURABILITY_BETA=$(fixture_fingerprint "$DURABILITY_SERIAL" beta.example)
if [[ "$DURABILITY_ALPHA" != "$SELECTION_ALPHA" ||
        "$DURABILITY_BETA" != "$SELECTION_BETA" ]]; then
    FAILURE_MESSAGE="The committed record did not carry the same certificates back"
    echo "$FAILURE_MESSAGE" >&2
    grep -F '[CERTIFICATE-FIXTURE]' "$DURABILITY_SERIAL" >&2 || true
    exit 1
fi

python3 "$TEST_DIR/sni-selection-client.py" \
    "$SNI_DURABILITY_PORT" "$DURABILITY_ALPHA" "$DURABILITY_BETA" selection

python3 "$TEST_DIR/sni-selection-client.py" \
    "$SNI_DURABILITY_PORT" "$DURABILITY_ALPHA" "$DURABILITY_BETA" default

wait_for_pattern "$DURABILITY_SERIAL" \
    'name=- result=DEFAULT cert=default' \
    'the durability boot recorded the client without SNI' 20

if ! grep -Fq 'name=beta.example result=EXACT cert=1' "$(serial_view "$DURABILITY_SERIAL")"; then
    FAILURE_MESSAGE="The durability boot published no selection record"
    echo "$FAILURE_MESSAGE" >&2
    grep -F '[PRODUCTION-TLS-SNI]' "$DURABILITY_SERIAL" >&2 || true
    exit 1
fi
if grep -Eq '\[PRODUCTION-SERVING\].*(PRIVATE KEY|BEGIN |secret=|password=)' \
        "$(serial_view "$DURABILITY_SERIAL")"; then
    FAILURE_MESSAGE="The durability boot leaked secret material"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if ! send_monitor_quit "$DURABILITY_MONITOR" >/dev/null 2>&1; then
    FAILURE_MESSAGE="QEMU monitor quit failed after the durability boot"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=

echo "TLS-PROOF SNI durability: the same two certificates came back from the committed record"
echo "QEMU production SNI selection test: OK"
