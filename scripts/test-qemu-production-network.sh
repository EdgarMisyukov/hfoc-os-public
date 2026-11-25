#!/usr/bin/env bash
# ITEM-362A ordinary-boot production network proof: q35 QEMU boots the plain
# production ISO with one virtio-net device, the production NIC/lwIP backend
# publishes a link and MAC on ordinary boot, and a real socket backend
# round-trips an ARP request and an ICMP echo through the production packet
# path. Physical network hardware is not exercised.
#
# ITEM-451: the same peer also opens the panel the way the owner's laptop does
# - a TCP SYN to 443 on the leased address, answered by a SYN+ACK, with a SYN
# to a closed port answered by an RST - and then requires the machine's own
# record to name each class it saw (`[PRODUCTION-LOOP] ... syn_rx= ...
# synack_tx= ... rst_tx= ... arp_req_rx= ... icmp_echo_rx= ...`) and to
# publish the three frame-path flags (`[PRODUCTION-NET] ... bound=1
# proto_published=1 netif_live=1`).  A machine that answers nothing and a
# machine that never saw the frame are different failures, and the record has
# to say which one happened.
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

if [[ "${HFOC_PRODUCTION_NETWORK_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_PRODUCTION_NETWORK_TEST_TIMEOUT:-210}s" \
        env HFOC_PRODUCTION_NETWORK_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
# ITEM-451: the panel half of this proof needs the hardware entropy owner the
# management listener waits for; `-cpu max` is the model that advertises
# RDRAND/RDSEED under TCG (the same model the production service proof runs),
# so the same segment now proves ARP, ICMP and the panel handshake together.
QEMU_CPU=${QEMU_CPU:-max}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}
GUEST_MAC='52:54:00:12:34:56'
# ITEM-388: the proof used to send exactly one ARP request and one ICMP echo, so
# a receive ring that stops after the buffers posted at activation still
# satisfied it. The paced cycles below require many exchanges:
#
#   * ARP is not rate limited by the ICMP reachability engine, so every one of
#     ARP_CYCLE_COUNT paced requests must be answered; a ring that stops after
#     eight frames cannot produce that many answers.
#   * ICMP echo is answered and rate limited by the ITEM-237 reachability
#     engine (HFOC_LWIP_PROTO_ICMP_BUDGET requests per
#     HFOC_LWIP_PROTO_ICMP_WINDOW_MS window), so the ICMP cycle paces one echo
#     per window and requires every reply, and the separate burst phase asserts
#     the documented budget instead of calling it a stall.
ARP_CYCLE_COUNT=${HFOC_PRODUCTION_NETWORK_ARP_CYCLE_COUNT:-24}
ICMP_CYCLE_COUNT=${HFOC_PRODUCTION_NETWORK_ICMP_CYCLE_COUNT:-8}
ICMP_BURST_COUNT=${HFOC_PRODUCTION_NETWORK_ICMP_BURST_COUNT:-6}
REPLY_DEADLINE_MS=${HFOC_PRODUCTION_NETWORK_REPLY_DEADLINE_MS:-2000}
REPLY_ATTEMPTS=${HFOC_PRODUCTION_NETWORK_REPLY_ATTEMPTS:-5}
ICMP_SPACING_MS=${HFOC_PRODUCTION_NETWORK_ICMP_SPACING_MS:-1200}
for value_name in ARP_CYCLE_COUNT ICMP_CYCLE_COUNT ICMP_BURST_COUNT \
    REPLY_DEADLINE_MS REPLY_ATTEMPTS ICMP_SPACING_MS; do
    case ${!value_name} in
        ''|*[!0-9]*)
            echo "$value_name must be a positive integer" >&2
            exit 1
            ;;
    esac
done
if (( ARP_CYCLE_COUNT < 1 || ARP_CYCLE_COUNT > 64 )); then
    echo "ARP_CYCLE_COUNT must be between 1 and 64" >&2
    exit 1
fi
if (( ICMP_CYCLE_COUNT < 1 || ICMP_CYCLE_COUNT > 64 )); then
    echo "ICMP_CYCLE_COUNT must be between 1 and 64" >&2
    exit 1
fi
if (( ICMP_BURST_COUNT < 1 || ICMP_BURST_COUNT > 16 )); then
    echo "ICMP_BURST_COUNT must be between 1 and 16" >&2
    exit 1
fi
if (( REPLY_ATTEMPTS < 1 || REPLY_ATTEMPTS > 16 )); then
    echo "REPLY_ATTEMPTS must be between 1 and 16" >&2
    exit 1
fi
if (( REPLY_DEADLINE_MS < 1 || ICMP_SPACING_MS < 1 )); then
    echo "reply deadline and ICMP spacing must be positive" >&2
    exit 1
fi
# The burst phase asserts the production reachability budget, so read the
# contract from the kernel header instead of duplicating the number.
ICMP_BUDGET=$(sed -n \
    's/^#define HFOC_LWIP_PROTO_ICMP_BUDGET \([0-9][0-9]*\)U$/\1/p' \
    kernel/include/hfoc_lwip_proto.h)
ICMP_WINDOW_MS=$(sed -n \
    's/^#define HFOC_LWIP_PROTO_ICMP_WINDOW_MS \([0-9][0-9]*\)U$/\1/p' \
    kernel/include/hfoc_lwip_proto.h)
for contract_value in "$ICMP_BUDGET" "$ICMP_WINDOW_MS"; do
    case $contract_value in
        ''|*[!0-9]*)
            echo "could not read the ICMP reachability contract" >&2
            exit 1
            ;;
    esac
done
if (( ICMP_BURST_COUNT <= ICMP_BUDGET )); then
    echo "ICMP_BURST_COUNT must exceed the reachability budget" >&2
    exit 1
fi
if (( ICMP_SPACING_MS < ICMP_WINDOW_MS )); then
    echo "ICMP_SPACING_MS must be at least the reachability window" >&2
    exit 1
fi

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Production network test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "Production network test must set QEMU_SMP=1" >&2
    exit 1
}
test -f "$IMAGE" || {
    echo "Production network test requires image: $IMAGE" >&2
    exit 1
}
echo "QEMU production-network baseline: machine=q35 smp=1 cpu=$QEMU_CPU"

TEST_DIR=$(mktemp -d)
SERIAL_LOG=$TEST_DIR/com1.log
MONITOR_LOG=$TEST_DIR/monitor.log
QEMU_LOG=$TEST_DIR/qemu.log
FAILURE_REASON=$TEST_DIR/failure-reason.txt
MONITOR_SOCKET=$TEST_DIR/monitor.sock
NET_PORT_FILE=$TEST_DIR/net-port.txt
QEMU_PID=
FAILURE_MESSAGE=

: >"$SERIAL_LOG"
: >"$MONITOR_LOG"
: >"$QEMU_LOG"
rm -f "$MONITOR_SOCKET"

NET_PORT=$(python3 - <<'PY'
import socket
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
PY
)
printf '%s\n' "$NET_PORT" >"$NET_PORT_FILE"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/production-network-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-Production network test failed}" \
        >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" \
        "$FAILURE_REASON" "$artifact_dir/" 2>/dev/null || true
    echo "Production-network artifacts: $artifact_dir" >&2
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

wait_for_exit() {
    local deadline=$((SECONDS + 5))
    local result=0
    while kill -0 "$QEMU_PID" 2>/dev/null; do
        if (( SECONDS >= deadline )); then
            return 1
        fi
        sleep 0.1
    done
    wait "$QEMU_PID" 2>/dev/null
    result=$?
    return "$result"
}

send_monitor_quit() {
    python3 - "$MONITOR_SOCKET" <<'PY'
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

qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" -cpu "$QEMU_CPU" \
    -display none -cdrom "$IMAGE" \
    -serial "file:$SERIAL_LOG" \
    -monitor "unix:$MONITOR_SOCKET,server,nowait" \
    -netdev socket,id=net0,listen=127.0.0.1:"$NET_PORT" \
    -device virtio-net-pci-non-transitional,netdev=net0,ioeventfd=off,\
mac="$GUEST_MAC",\
event_idx=off,indirect_desc=off,\
mq=off,mrg_rxbuf=off,ctrl_vq=off,ctrl_rx=off,ctrl_rx_extra=off,\
ctrl_vlan=off,ctrl_mac_addr=off,ctrl_guest_offloads=off,gso=off,\
guest_csum=off,csum=off,guest_tso4=off,guest_tso6=off,guest_ecn=off,\
guest_ufo=off,host_tso4=off,host_tso6=off,host_ecn=off,host_ufo=off,\
host_uso=off,guest_uso4=off,guest_uso6=off,guest_announce=off,\
hash=off,rss=off,guest_rsc_ext=off,vectors=0,x-disable-pcie=on \
    -no-reboot -no-shutdown \
    >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

validation_output=
if ! validation_output=$(python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" \
    "$NET_PORT" "$QEMU_PID" "$ARP_CYCLE_COUNT" "$ICMP_CYCLE_COUNT" \
    "$ICMP_BURST_COUNT" "$REPLY_DEADLINE_MS" "$ICMP_SPACING_MS" \
    "$ICMP_BUDGET" "$REPLY_ATTEMPTS" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import os
import pathlib
import socket
import struct
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
monitor_path = pathlib.Path(sys.argv[2])
net_port = int(sys.argv[3])
qemu_pid = int(sys.argv[4])
arp_cycle_count = int(sys.argv[5])
icmp_cycle_count = int(sys.argv[6])
icmp_burst_count = int(sys.argv[7])
reply_deadline_s = int(sys.argv[8]) / 1000.0
icmp_spacing_s = int(sys.argv[9]) / 1000.0
icmp_budget = int(sys.argv[10])
reply_attempts = int(sys.argv[11])

guest_mac = bytes([0x52, 0x54, 0x00, 0x12, 0x34, 0x56])
host_mac = bytes([0x02, 0x00, 0x00, 0x00, 0x00, 0x02])
guest_ip = bytes([10, 0, 2, 15])
host_ip = bytes([10, 0, 2, 2])
ready_marker = "[PRODUCTION-NET] device=PRESENT readiness=READY"
mac_marker = "mac=52:54:00:12:34:56"


def serial_text():
    try:
        return SERIAL_VIEW.normalise(
            serial_path.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""


def qemu_running():
    try:
        os.kill(qemu_pid, 0)
        return True
    except ProcessLookupError:
        return False


def wait_serial_marker(marker, timeout=90.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = serial_text()
        if marker in text:
            return
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit("Kernel panic before production network ready")
        if not qemu_running():
            raise SystemExit("QEMU exited before production network ready")
        time.sleep(0.05)
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="' +
        str(f"timed out waiting for marker: {marker}") + '" '
        'deadline=' + str(int(timeout)) + 's\n')
    sys.stderr.flush()
    raise SystemExit(f"timed out waiting for marker: {marker}")


def checksum(data):
    if len(data) % 2:
        data += b"\x00"
    total = 0
    for index in range(0, len(data), 2):
        total += (data[index] << 8) | data[index + 1]
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


# ITEM-416: the appliance holds an address it was given, so this proof gives it
# one.  It used to borrow the QEMU fixture's 10.0.2.15 without asking, which is
# the defect the hardware run found: the machine answered on an address nobody
# had assigned.  The socket backend has no DHCP server of its own, so the peer
# side of this proof is the server - DISCOVER is offered the address the ARP
# and ICMP phases then use, and REQUEST is acknowledged with a long lease so the
# exchange happens once.
dhcp_lease_seconds = 3600


def build_dhcp_payload(message_type, xid):
    payload = bytearray(240)
    payload[0] = 2
    payload[1] = 1
    payload[2] = 6
    struct.pack_into("!I", payload, 4, xid)
    payload[10] = 0x80
    payload[11] = 0x00
    payload[16:20] = guest_ip
    payload[28:34] = guest_mac
    struct.pack_into("!I", payload, 236, 0x63825363)
    payload += bytes([53, 1, message_type])
    payload += bytes([1, 4]) + bytes([255, 255, 255, 0])
    payload += bytes([3, 4]) + host_ip
    payload += bytes([6, 4]) + host_ip
    payload += bytes([54, 4]) + host_ip
    payload += bytes([51, 4]) + struct.pack("!I", dhcp_lease_seconds)
    payload += bytes([255])
    if len(payload) < 300:
        payload += b"\x00" * (300 - len(payload))
    return bytes(payload)


def build_dhcp_reply(message_type, xid):
    payload = build_dhcp_payload(message_type, xid)
    udp_length = 8 + len(payload)
    udp = bytearray()
    udp += struct.pack("!HHHH", 67, 68, udp_length, 0)
    udp += payload
    pseudo = host_ip + b"\xff\xff\xff\xff" + b"\x00\x11" + \
        struct.pack("!H", udp_length)
    udp[6:8] = struct.pack("!H", checksum(pseudo + bytes(udp)))
    ip = bytearray()
    ip += b"\x45\x00"
    ip += struct.pack("!H", 20 + len(udp))
    ip += b"\x22\x33"
    ip += b"\x00\x00"
    ip += b"\x40\x11"
    ip += b"\x00\x00"
    ip += host_ip
    ip += b"\xff\xff\xff\xff"
    ip[10:12] = struct.pack("!H", checksum(bytes(ip)))
    frame = bytearray()
    frame += b"\xff\xff\xff\xff\xff\xff"
    frame += host_mac
    frame += b"\x08\x00"
    frame += ip
    frame += udp
    if len(frame) < 60:
        frame += b"\x00" * (60 - len(frame))
    return bytes(frame)


def dhcp_request(frame):
    """(message type, xid) of a DHCP request from the guest, or None."""
    if len(frame) < 42 + 240 or frame[12:14] != b"\x08\x00":
        return None
    if frame[23] != 17 or frame[30:34] != b"\xff\xff\xff\xff":
        return None
    if frame[34:36] != struct.pack("!H", 68) or \
            frame[36:38] != struct.pack("!H", 67):
        return None
    payload = frame[42:]
    if payload[236:240] != b"\x63\x82\x53\x63":
        return None
    if payload[28:34] != guest_mac:
        return None
    index = 240
    while index < len(payload):
        option = payload[index]
        if option == 255:
            break
        if option == 0:
            index += 1
            continue
        if index + 1 >= len(payload):
            break
        length = payload[index + 1]
        if option == 53 and length == 1 and index + 2 < len(payload):
            return payload[index + 2], struct.unpack_from("!I", payload, 4)[0]
        index += 2 + length
    return None


def give_address(network, deadline=30.0):
    """Answer the guest's DHCP exchange, and say whether it was answered."""
    limit = time.monotonic() + deadline
    while time.monotonic() < limit:
        frame = try_recv_frame(network, 0.2)
        if frame is None:
            if not qemu_running():
                return False
            continue
        request = dhcp_request(frame)
        if request is None:
            continue
        message_type, xid = request
        if message_type == 1:
            reply = build_dhcp_reply(2, xid)
        elif message_type == 3:
            reply = build_dhcp_reply(5, xid)
        else:
            continue
        network.sendall(struct.pack("!I", len(reply)) + reply)
        if message_type == 3:
            return True
    return False


def build_arp_request():
    frame = bytearray()
    frame += b"\xff" * 6
    frame += host_mac
    frame += b"\x08\x06"
    frame += b"\x00\x01"
    frame += b"\x08\x00"
    frame += b"\x06\x04"
    frame += b"\x00\x01"
    frame += host_mac
    frame += host_ip
    frame += b"\x00" * 6
    frame += guest_ip
    frame += b"\x00" * (60 - len(frame))
    return bytes(frame)


def build_icmp_echo(sequence=1):
    payload = b"HFOC-PING-%02d" % sequence
    icmp = bytearray()
    icmp += b"\x08\x00"
    icmp += b"\x00\x00"
    icmp += b"\xAB\xCD"
    icmp += struct.pack("!H", sequence)
    icmp += payload
    icmp[2:4] = struct.pack("!H", checksum(bytes(icmp)))
    ip = bytearray()
    ip += b"\x45\x00"
    ip += struct.pack("!H", 20 + len(icmp))
    ip += b"\x12\x00"
    ip += b"\x40\x00"
    ip += b"\x40\x01"
    ip += b"\x00\x00"
    ip += host_ip
    ip += guest_ip
    ip[10:12] = struct.pack("!H", checksum(bytes(ip)))
    frame = bytearray()
    frame += guest_mac
    frame += host_mac
    frame += b"\x08\x00"
    frame += ip
    frame += icmp
    frame += b"\x00" * (60 - len(frame))
    return bytes(frame)


def recv_exact(connection, length, timeout=20.0):
    data = b""
    deadline = time.monotonic() + timeout
    connection.settimeout(0.5)
    while len(data) < length:
        if time.monotonic() >= deadline:
            raise SystemExit("timed out reading QEMU network frame")
        try:
            chunk = connection.recv(length - len(data))
        except socket.timeout:
            if not qemu_running():
                raise SystemExit("QEMU exited while reading frame")
            continue
        if not chunk:
            raise SystemExit("QEMU network socket closed")
        data += chunk
    return data


def recv_frame(connection):
    header = recv_exact(connection, 4)
    length = struct.unpack("!I", header)[0]
    return recv_exact(connection, length)


def try_recv_frame(connection, timeout):
    """Read one length-prefixed frame, or None when the deadline expires.

    The paced ring-cycle proof must be able to give up on one exchange and
    continue with the next one, so a per-exchange deadline cannot raise the
    fatal timeout the baseline helpers use.
    """
    deadline = time.monotonic() + timeout
    header = b""
    connection.settimeout(0.2)
    while len(header) < 4:
        if time.monotonic() >= deadline:
            return None
        try:
            chunk = connection.recv(4 - len(header))
        except socket.timeout:
            if not qemu_running():
                raise SystemExit("QEMU exited while reading frames")
            continue
        if not chunk:
            raise SystemExit("QEMU network socket closed")
        header += chunk
    length = struct.unpack("!I", header)[0]
    body = b""
    while len(body) < length:
        if time.monotonic() >= deadline:
            return None
        try:
            chunk = connection.recv(length - len(body))
        except socket.timeout:
            if not qemu_running():
                raise SystemExit("QEMU exited while reading frames")
            continue
        if not chunk:
            raise SystemExit("QEMU network socket closed")
        body += chunk
    return body


def recv_matching(connection, predicate, description, timeout=30.0):
    # ITEM-362B ordinary boot also drives the DHCP/DNS wire, so the socket
    # backend may observe DHCP broadcasts interleaved with the ARP/ICMP frames
    # this proof owns. Skip them without weakening the exact ARP/ICMP match.
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        frame = recv_frame(connection)
        if predicate(frame):
            return frame
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="' +
        str("timed out waiting for " + description) + '" '
        'deadline=' + str(int(timeout)) + 's\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for " + description)


def arp_reply_ok(frame):
    return (len(frame) >= 42 and
            frame[0:6] == host_mac and
            frame[6:12] == guest_mac and
            frame[12:14] == b"\x08\x06" and
            frame[20:22] == b"\x00\x02" and
            frame[22:28] == guest_mac and
            frame[28:32] == guest_ip and
            frame[32:38] == host_mac and
            frame[38:42] == host_ip)


def icmp_reply(frame, sequence):
    """Return True for the exact echo reply of `sequence`, else False."""
    if len(frame) < 60:
        return False
    if (frame[0:6] != host_mac or frame[6:12] != guest_mac or
            frame[12:14] != b"\x08\x00"):
        return False
    ip = frame[14:34]
    if ip[0] != 0x45 or ip[9] != 1 or ip[12:16] != guest_ip or \
            ip[16:20] != host_ip or checksum(ip) != 0:
        return False
    icmp_length = struct.unpack("!H", ip[2:4])[0] - 20
    icmp = frame[34:34 + icmp_length]
    if len(icmp) < 8 + 12 or icmp[0] != 0 or checksum(icmp) != 0:
        return False
    return (struct.unpack("!H", icmp[6:8])[0] == sequence and
            bytes(icmp[8:]) == b"HFOC-PING-%02d" % sequence)


def icmp_reply_ok(frame):
    return icmp_reply(frame, 1)


# ITEM-451: the panel being opened from another machine on the same segment is
# a TCP handshake, not an echo.  This builds the SYN the owner's laptop sends
# and reads the answer, so "the panel does not open" fails here by name.
def build_tcp_segment(destination_port, flags, sequence, source_port=40000):
    tcp = bytearray()
    tcp += struct.pack("!HH", source_port, destination_port)
    tcp += struct.pack("!I", sequence)
    tcp += struct.pack("!I", 0)
    tcp += b"\x50" + bytes([flags])
    tcp += struct.pack("!H", 0x2000)
    tcp += b"\x00\x00"
    tcp += b"\x00\x00"
    pseudo = host_ip + guest_ip + b"\x00\x06" + struct.pack("!H", len(tcp))
    tcp[16:18] = struct.pack("!H", checksum(pseudo + bytes(tcp)))
    ip = bytearray()
    ip += b"\x45\x00"
    ip += struct.pack("!H", 20 + len(tcp))
    ip += b"\x51\x00"
    ip += b"\x40\x00"
    ip += b"\x40\x06"
    ip += b"\x00\x00"
    ip += host_ip
    ip += guest_ip
    ip[10:12] = struct.pack("!H", checksum(bytes(ip)))
    frame = bytearray()
    frame += guest_mac
    frame += host_mac
    frame += b"\x08\x00"
    frame += ip
    frame += tcp
    frame += b"\x00" * (60 - len(frame))
    return bytes(frame)


def tcp_reply(frame, source_port, destination_port, flags):
    """True for the guest's TCP answer with exactly `flags` set on the way."""
    if len(frame) < 54 or frame[0:6] != host_mac or frame[6:12] != guest_mac:
        return False
    if frame[12:14] != b"\x08\x00":
        return False
    ip = frame[14:34]
    if ip[0] != 0x45 or ip[9] != 6 or ip[12:16] != guest_ip or \
            ip[16:20] != host_ip or checksum(ip) != 0:
        return False
    ihl = (ip[0] & 0x0F) * 4
    total = struct.unpack("!H", ip[2:4])[0]
    tcp = frame[14 + ihl:14 + total]
    if len(tcp) < 20:
        return False
    if struct.unpack("!HH", tcp[0:4]) != (source_port, destination_port):
        return False
    if tcp[13] & flags != flags:
        return False
    pseudo = guest_ip + host_ip + b"\x00\x06" + struct.pack("!H", len(tcp))
    return checksum(pseudo + bytes(tcp)) == 0


def wait_tcp_reply(network, source_port, destination_port, flags, timeout):
    """Read frames until the named TCP answer arrives, or the deadline ends."""
    deadline = time.monotonic() + timeout
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0.0:
            return None
        frame = try_recv_frame(network, remaining)
        if frame is None:
            return None
        if tcp_reply(frame, source_port, destination_port, flags):
            return frame


def production_record_fields(prefix):
    """The last record with `prefix`, as a {key: value} mapping."""
    fields = {}
    for line in serial_text().splitlines():
        if line.startswith(prefix):
            fields = {}
            for token in line.split()[1:]:
                if "=" in token:
                    key, _, value = token.partition("=")
                    fields[key] = value
    return fields


def production_frame_classes():
    """{class name: {field: value}} for every [PRODUCTION-FRAME] record."""
    records = {}
    for line in serial_text().splitlines():
        if not line.startswith("[PRODUCTION-FRAME]"):
            continue
        fields = {}
        for token in line.split()[1:]:
            if "=" in token:
                key, _, value = token.partition("=")
                fields[key] = value
        name = fields.pop("class", None)
        if name is not None:
            records[name] = fields
    return records


def wait_frame_class(name, predicate, description, timeout=20.0):
    """Wait for the record that names a class, or fail naming it.

    The heartbeat carries the same counters but its clock is the PIT, so on a
    loaded runner ten guest seconds can be a minute of wall time; these records
    are event-driven, so the wait is a bound on the guest's own loop, not on
    its clock.
    """
    deadline = time.monotonic() + timeout
    while True:
        records = production_frame_classes()
        fields = records.get(name)
        if fields and predicate(fields):
            return fields
        if time.monotonic() >= deadline:
            seen = ", ".join(sorted(records)) if records else "none"
            raise SystemExit(
                "the appliance never named %s ([PRODUCTION-FRAME] class=%s); "
                "frame classes named: %s" % (description, name, seen))
        time.sleep(0.1)


def wait_record_fields(prefix, predicate, description, timeout):
    """Wait for a record that names the traffic, or fail naming the class."""
    deadline = time.monotonic() + timeout
    while True:
        fields = production_record_fields(prefix)
        if fields and predicate(fields):
            return fields
        if time.monotonic() >= deadline:
            raise SystemExit(
                "the appliance never named %s in %s (last: %s)" %
                (description, prefix, fields if fields else "no record"))
        time.sleep(0.25)


with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as network:
    network.settimeout(5.0)
    deadline = time.monotonic() + 15.0
    while True:
        try:
            network.connect(("127.0.0.1", net_port))
            break
        except OSError as exc:
            if time.monotonic() >= deadline:
                raise SystemExit(f"QEMU socket backend did not accept: {exc}")
            if not qemu_running():
                raise SystemExit("QEMU exited before network connection")
            time.sleep(0.05)

    wait_serial_marker(ready_marker)
    # The address is a field of the same `[PRODUCTION-NET]` record, written a
    # few fields after the readiness marker.  It is waited for like every other
    # marker instead of sampled once: the 2026-09-29 branch run read the serial
    # file inside that window and failed a machine whose record carried the
    # address.  A machine that never writes it still fails, now through the
    # canonical timeout record.
    wait_serial_marker(mac_marker)

    # ITEM-416: give the machine the address this proof then talks to.  The
    # serial record proves the address it holds is the one the server handed
    # out, and it is the address the ARP/ICMP phases use.
    if not give_address(network):
        raise SystemExit(
            "the appliance never completed a DHCP exchange with this proof")
    wait_serial_marker("[PRODUCTION-DHCP] state=ACTIVE")

    network.sendall(struct.pack("!I", 60) + build_arp_request())
    arp_reply = recv_matching(
        network, arp_reply_ok, "production ARP reply")
    if not arp_reply_ok(arp_reply):
        raise SystemExit(
            "ARP reply did not match the production packet path")

    network.sendall(struct.pack("!I", len(build_icmp_echo())) +
                    build_icmp_echo())
    icmp_frame = recv_matching(
        network, icmp_reply_ok, "production ICMP reply")
    if not icmp_reply_ok(icmp_frame):
        raise SystemExit(
            "ICMP reply did not match the production packet path")

    # ITEM-388 ARP ring cycle: ARP is not rate limited, so every paced request
    # must be answered. A ring that stops after its activation window answers
    # only the first few and this phase fails on the first missing reply.
    # A request is retried across REPLY_ATTEMPTS deadlines so a boot-time busy
    # window cannot be mistaken for a ring that stopped answering.
    arp_cycle_answered = 0
    for _ in range(arp_cycle_count):
        replied = False
        for _ in range(reply_attempts):
            network.sendall(struct.pack("!I", 60) + build_arp_request())
            deadline = time.monotonic() + reply_deadline_s
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0.0:
                    break
                frame = try_recv_frame(network, remaining)
                if frame is None:
                    break
                if arp_reply_ok(frame):
                    replied = True
                    break
            if replied:
                break
        if not replied:
            raise SystemExit(
                "production ARP ring cycle lost reply %d of %d"
                % (arp_cycle_answered + 1, arp_cycle_count))
        arp_cycle_answered += 1

    # ITEM-388 ICMP paced cycle: the reachability engine answers
    # HFOC_LWIP_PROTO_ICMP_BUDGET echoes per
    # HFOC_LWIP_PROTO_ICMP_WINDOW_MS window and rate limits the rest, so one
    # echo per window must always be answered and every reply is required.
    # A rate-limited or delayed request is retried on the next window.
    icmp_cycle_answered = 0
    for sequence in range(1, icmp_cycle_count + 1):
        request = build_icmp_echo(sequence)
        replied = False
        for _ in range(reply_attempts):
            network.sendall(struct.pack("!I", len(request)) + request)
            deadline = time.monotonic() + reply_deadline_s
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0.0:
                    break
                frame = try_recv_frame(network, remaining)
                if frame is None:
                    break
                if icmp_reply(frame, sequence):
                    replied = True
                    break
            if replied:
                break
        if not replied:
            raise SystemExit(
                "production ICMP paced cycle lost reply %d of %d"
                % (sequence, icmp_cycle_count))
        icmp_cycle_answered += 1
        time.sleep(icmp_spacing_s)

    # ITEM-388 ICMP burst budget: more echoes than the window budget in one
    # window are rate limited by the reachability engine, which is a policy,
    # not a ring stall. Assert the policy so a future reader can tell them
    # apart instead of rediscovering the scratch 8-of-12 signature. A loaded
    # host runs the guest slower than wall time, so the phase waits three full
    # spacings before the burst; a cold window answers exactly the budget and a
    # window that is already full answers none, but the burst is always bounded
    # by the budget and the paced cycle above already proves the interface
    # answers. The printed count is the measured evidence.
    time.sleep(3.0 * icmp_spacing_s)
    burst_sequences = list(range(100, 100 + icmp_burst_count))
    for sequence in burst_sequences:
        request = build_icmp_echo(sequence)
        network.sendall(struct.pack("!I", len(request)) + request)
    burst_answered = set()
    deadline = time.monotonic() + reply_deadline_s
    while time.monotonic() < deadline:
        frame = try_recv_frame(network, deadline - time.monotonic())
        if frame is None:
            break
        for sequence in burst_sequences:
            if icmp_reply(frame, sequence):
                burst_answered.add(sequence)
                break
    if len(burst_answered) > icmp_budget:
        raise SystemExit(
            "production ICMP burst answered %d of %d, above the %d-request "
            "reachability budget" %
            (len(burst_answered), icmp_burst_count, icmp_budget))

    # ITEM-451: the owner's laptop could not open the panel or ping the
    # machine, and the 2026-09-19 record could not say which of the two the
    # machine had even seen.  This phase opens the panel the way the laptop
    # does - a TCP SYN to 443 on the leased address, answered by a SYN+ACK -
    # and then requires the machine's own record to name what arrived and what
    # it answered.  The listener waits for the entropy owner, so the proof
    # gives it a bounded window and names the wait if it never opens.
    wait_serial_marker("state=LISTENING", timeout=120.0)
    panel_syn = build_tcp_segment(443, 0x02, 0x10203040)
    panel_reply = None
    for _ in range(3):
        network.sendall(struct.pack("!I", len(panel_syn)) + panel_syn)
        panel_reply = wait_tcp_reply(
            network, 443, 40000, 0x12, reply_deadline_s)
        if panel_reply is not None:
            break
    if panel_reply is None:
        refused = wait_tcp_reply(network, 443, 40000, 0x04, 0.5)
        if refused is not None:
            raise SystemExit(
                "the panel refused the panel SYN with a TCP RST "
                "(the management listener is not answering 443)")
        raise SystemExit(
            "the appliance never answered the panel SYN to 443: the panel "
            "does not open from this segment")

    closed_syn = build_tcp_segment(444, 0x02, 0x50607080)
    network.sendall(struct.pack("!I", len(closed_syn)) + closed_syn)
    closed_reply = wait_tcp_reply(network, 444, 40000, 0x04, reply_deadline_s)
    if closed_reply is None:
        raise SystemExit(
            "the appliance never refused a SYN to the closed port 444: a "
            "refusal has to name itself")

    loop_fields = wait_record_fields(
        "[PRODUCTION-LOOP]",
        lambda fields: ("arp_req_rx" in fields and "arp_rep_tx" in fields and
                        "icmp_echo_rx" in fields and "icmp_rep_tx" in fields and
                        "syn_rx" in fields and "synack_tx" in fields and
                        "rst_tx" in fields and "nonipv4_rx" in fields and
                        "malformed_rx" in fields),
        "the frame-path classes on the heartbeat",
        10.0)
    arp_frame = wait_frame_class(
        "ARP_REQUEST",
        lambda fields: (int(fields.get("arp_req_rx", "0")) >= 1 and
                        int(fields.get("arp_rep_tx", "0")) >= 1),
        "the ARP request it answered")
    icmp_frame = wait_frame_class(
        "ICMP_ECHO",
        lambda fields: (int(fields.get("icmp_echo_rx", "0")) >= 1 and
                        int(fields.get("icmp_rep_tx", "0")) >= 1),
        "the ICMP echo it answered")
    syn_frame = wait_frame_class(
        "TCP_SYN",
        lambda fields: (int(fields.get("syn_rx", "0")) >= 1 and
                        int(fields.get("synack_tx", "0")) >= 1),
        "the panel SYN it accepted")
    refused_frame = wait_frame_class(
        "TCP_REFUSED",
        lambda fields: int(fields.get("rst_tx", "0")) >= 1,
        "the connection it refused")
    net_fields = wait_record_fields(
        "[PRODUCTION-NET]",
        lambda fields: (fields.get("bound") == "1" and
                        fields.get("proto_published") == "1" and
                        fields.get("netif_live") == "1"),
        "the open frame path (bound/proto_published/netif_live)",
        10.0)

    # ITEM-581: the installed Toshiba showed no address and no panel, and no
    # command answered "what does the network look like right now".  This phase
    # types the console `network` command on the same boot whose DHCP lease,
    # panel listener and entropy owner are already proven above, and requires
    # the answer on the screen (the console mirrors every painted character to
    # the serial transcript) to name the leased address, the panel gate, the
    # CPUID entropy profile and the administrator gate.  The command is typed
    # only, so nothing here changes the machine's network state.
    monitor_deadline = time.monotonic() + 5.0
    while not monitor_path.exists():
        if time.monotonic() >= monitor_deadline:
            raise SystemExit("timed out waiting for QEMU monitor socket")
        time.sleep(0.05)

    console_mark = len(serial_text())
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
        monitor.settimeout(1.0)
        try:
            monitor.connect(str(monitor_path))
        except OSError as exc:
            raise SystemExit(f"QEMU monitor did not accept connection: {exc}")

        def drain_monitor():
            try:
                while monitor.recv(4096):
                    pass
            except socket.timeout:
                return

        def send_key(key):
            monitor.sendall(("sendkey " + key + "\n").encode("ascii"))
            drain_monitor()
            time.sleep(0.05)

        drain_monitor()
        for character in "network":
            send_key(character)
        send_key("ret")

    def wait_console_record(prefix, timeout=30.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            text = serial_text()
            for line in text[console_mark:].splitlines():
                if line.startswith(prefix):
                    return line
            if "*** HFOC KERNEL PANIC ***" in text:
                raise SystemExit("Kernel panic while the `network` command ran")
            time.sleep(0.05)
        # ITEM-422: a proof that gives up on a marker says so.
        sys.stderr.write(
            'HFOC-QEMU-MARKER-TIMEOUT marker="console ' + prefix + '" '
            'deadline=30s\n')
        sys.stderr.flush()
        raise SystemExit(
            "the console `network` command printed no " + prefix + " record")

    def type_console_line(line):
        """Type one console command through the QEMU monitor (sendkey)."""
        monitor_deadline = time.monotonic() + 5.0
        while not monitor_path.exists():
            if time.monotonic() >= monitor_deadline:
                raise SystemExit("timed out waiting for QEMU monitor socket")
            time.sleep(0.05)
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
            monitor.settimeout(1.0)
            try:
                monitor.connect(str(monitor_path))
            except OSError as exc:
                raise SystemExit(f"QEMU monitor did not accept connection: {exc}")

            def drain():
                try:
                    while monitor.recv(4096):
                        pass
                except socket.timeout:
                    return

            def key(name):
                monitor.sendall(("sendkey " + name + "\n").encode("ascii"))
                drain()
                time.sleep(0.05)

            drain()
            for character in line:
                # QEMU's sendkey names the space key `spc`; a bare space is
                # not a key name and is dropped, which typed `diagsave`.
                key("spc" if character == " " else character)
            key("ret")

    status_line = wait_console_record("[NET-STATUS]")
    for field in ("device=PRESENT", "readiness=READY", "link=UP",
                  "mode=DHCP", " stage="):
        if field not in status_line:
            raise SystemExit(
                "the console [NET-STATUS] line lacks " + field + ": " +
                status_line)
    address_line = wait_console_record("[NET-ADDR]")
    # The lease this proof handed out above is 10.0.2.15/24 with gateway
    # 10.0.2.2 and DNS 10.0.2.2; the console must show that address while the
    # panel is serving, which is exactly what the Toshiba never displayed.
    for field in ("state=ACTIVE", "lease=ACQUIRED", "address=10.0.2.15/24",
                  "gateway=10.0.2.2", "dns=10.0.2.2", " mac=", "attempt=",
                  "discover=", "request=", "offer=", "ack=", "refused="):
        if field not in address_line:
            raise SystemExit(
                "the console [NET-ADDR] line lacks " + field + ": " +
                address_line)
    # ITEM-612: the live frame path must be visible where the operator looks.
    # The Toshiba export had link=UP and a lease but no ring or per-class
    # counters, so it could not say whether the peer's ARP/ICMP/TCP arrived.
    io_line = wait_console_record("[NET-IO]")
    for field in ("available=1", "tx_sub=", "tx_recl=", "tx_lost=",
                  "tx_full=", "tx_rst=", "tx_err=", "rx_ok=", "rx_drop=",
                  "rx_err=", "rx_arm="):
        if field not in io_line:
            raise SystemExit(
                "the console [NET-IO] line lacks " + field + ": " + io_line)
    frame_line = wait_console_record("[NET-FRAME]")
    for field in ("available=1", "arp_req_rx=", "arp_rep_rx=", "arp_req_tx=",
                  "arp_rep_tx=", "icmp_echo_rx=", "icmp_other_rx=",
                  "icmp_rep_tx=", "syn_rx=", "synack_tx=", "rst_tx=",
                  "ipv4_other_rx=", "nonipv4_rx=", "malformed_rx=",
                  "limited_rx=", "accepted_rx="):
        if field not in frame_line:
            raise SystemExit(
                "the console [NET-FRAME] line lacks " + field + ": " +
                frame_line)
    panel_line = wait_console_record("[PANEL]")
    for field in ("v=1 state=", "listener=", " entropy=", "certificate=",
                  " bind="):
        if field not in panel_line:
            raise SystemExit(
                "the console [PANEL] line lacks " + field + ": " + panel_line)
    rng_line = wait_console_record("[RNG]")
    for field in ("source=", "state=", "stage=", "cpuid_max=", "rdrand=",
                  "rdseed="):
        if field not in rng_line:
            raise SystemExit(
                "the console [RNG] line lacks " + field + ": " + rng_line)
    admin_line = wait_console_record("[ADMIN]")
    for field in ("state=", "last="):
        if field not in admin_line:
            raise SystemExit(
                "the console [ADMIN] line lacks " + field + ": " + admin_line)

    # ITEM-582: a machine with no writable medium must still answer with its
    # evidence.  `diag save` fails at MEDIUM_PROBE on this boot (the proof boots
    # the optical medium, with no journal), and the failure must print the RAM
    # snapshot with the network/panel/entropy records instead of returning
    # before they are composed; `diag snapshot` must print it again.
    diag_mark = len(serial_text())
    type_console_line("diag save")
    snapshot_deadline = time.monotonic() + 30.0
    while time.monotonic() < snapshot_deadline:
        text = serial_text()[diag_mark:]
        if "DIAG SNAPSHOT records=" in text:
            break
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit("Kernel panic during the diag snapshot")
        time.sleep(0.05)
    else:
        sys.stderr.write(
            'HFOC-QEMU-MARKER-TIMEOUT marker="DIAG SNAPSHOT records=" '
            'deadline=30s\n')
        sys.stderr.flush()
        raise SystemExit("a failed `diag save` printed no RAM snapshot")
    snapshot_text = serial_text()[diag_mark:]
    for record in ("[NET-STATUS]", "[NET-ADDR]", "[NET-IO]", "[NET-FRAME]",
                   "[PANEL]", "[RNG]", "[ADMIN]"):
        if record not in snapshot_text:
            raise SystemExit(
                "the RAM snapshot after a failed `diag save` lacks " + record)
    for field in ("tx_sub=", "rx_arm=", "arp_req_rx=", "icmp_echo_rx=",
                  "syn_rx=", "accepted_rx="):
        if field not in snapshot_text:
            raise SystemExit(
                "the RAM snapshot lacks the frame-path field " + field)
    type_console_line("diag snapshot")
    second_deadline = time.monotonic() + 30.0
    while time.monotonic() < second_deadline:
        if serial_text().count("DIAG SNAPSHOT records=") >= 2:
            break
        if "*** HFOC KERNEL PANIC ***" in serial_text():
            raise SystemExit("Kernel panic during `diag snapshot`")
        time.sleep(0.05)
    else:
        sys.stderr.write(
            'HFOC-QEMU-MARKER-TIMEOUT marker="second DIAG SNAPSHOT records=" '
            'deadline=30s\n')
        sys.stderr.flush()
        raise SystemExit("`diag snapshot` did not print the snapshot again")

    text = serial_text()
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("Kernel panic during production network proof")
if "device=ABSENT" in text:
    raise SystemExit("production network reported an absent NIC")

monitor_deadline = time.monotonic() + 5.0
while not monitor_path.exists():
    if time.monotonic() >= monitor_deadline:
        raise SystemExit("timed out waiting for QEMU monitor socket")
    time.sleep(0.05)

print("Production network ready on ordinary boot: found")
print("Production MAC publication: found")
print("Production ARP packet path: found")
print("Production ICMP packet path: found")
print("Production ARP ring cycle: %d of %d replies"
      % (arp_cycle_answered, arp_cycle_count))
print("Production ICMP paced cycle: %d of %d replies"
      % (icmp_cycle_answered, icmp_cycle_count))
print("Production ICMP burst budget: %d of %d answered (budget %d)"
      % (len(burst_answered), icmp_burst_count, icmp_budget))
print("Production panel handshake from the same segment: SYN+ACK")
print("Production closed-port refusal: RST")
print("Production frame path by class: "
      "ARP_REQUEST arp_req_rx=%s arp_rep_tx=%s; "
      "ICMP_ECHO icmp_echo_rx=%s icmp_rep_tx=%s; "
      "TCP_SYN syn_rx=%s synack_tx=%s; TCP_REFUSED rst_tx=%s; "
      "heartbeat nonipv4_rx=%s malformed_rx=%s" %
      (arp_frame.get("arp_req_rx"), arp_frame.get("arp_rep_tx"),
       icmp_frame.get("icmp_echo_rx"), icmp_frame.get("icmp_rep_tx"),
       syn_frame.get("syn_rx"), syn_frame.get("synack_tx"),
       refused_frame.get("rst_tx"), loop_fields.get("nonipv4_rx"),
       loop_fields.get("malformed_rx")))
print("Production frame path flags: bound=%s proto_published=%s netif_live=%s"
      % (net_fields.get("bound"), net_fields.get("proto_published"),
         net_fields.get("netif_live")))
print("Console `network` records: [NET-STATUS] [NET-ADDR] [NET-IO] "
      "[NET-FRAME] [PANEL] [RNG] [ADMIN]")
print("Console `network` address: %s"
      % address_line.split(" address=")[1].split(" ")[0])
print("Console `network` panel: %s"
      % panel_line.split(" state=")[1].split(" ")[0])
print("Console `network` entropy source: %s"
      % rng_line.split(" source=")[1].split(" ")[0])
print("Kernel panic: none")
PY
); then
    FAILURE_MESSAGE=$validation_output
    printf '%s\n' "$validation_output" >&2
    exit 1
fi

printf '%s\n' "$validation_output"

if ! send_monitor_quit >/dev/null 2>&1; then
    FAILURE_MESSAGE="QEMU monitor quit failed"
    printf '%s\n' "$FAILURE_MESSAGE" >&2
    exit 1
fi
if ! wait_for_exit; then
    FAILURE_MESSAGE="QEMU did not exit cleanly after monitor quit"
    printf '%s\n' "$FAILURE_MESSAGE" >&2
    exit 1
fi
QEMU_PID=

echo "QEMU production network test: OK"
