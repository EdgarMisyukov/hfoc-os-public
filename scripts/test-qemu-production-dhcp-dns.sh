#!/usr/bin/env bash
# ITEM-362B ordinary-boot production DHCP/DNS wire proof: q35 QEMU boots the
# plain production ISO with one virtio-net device on a socket backend. The
# host side acts as the DHCP server, ARP responder, and DNS server; the guest
# acquires a lease, applies it, and resolves one A record through the
# production NIC/lwIP packet path. Physical network hardware is not exercised.
#
# ITEM-408 extends the same proof with what the installed appliance must do
# after the lease: the acknowledgement carries a short lease time, the host
# deliberately refuses the first renewal cycle, and the proof requires the
# appliance to keep the address applied (no FAILED record, no deactivation),
# to answer an ICMP echo request for its own address while the renewal is
# failing, and to return to ACTIVE on the same address once the server
# answers the re-acquisition. The panel itself is served over the real lwIP
# path by scripts/test-qemu-production-service-https.sh (ITEM-386).
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

if [[ "${HFOC_PRODUCTION_DHCP_DNS_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_PRODUCTION_DHCP_DNS_TEST_TIMEOUT:-300}s" \
        env HFOC_PRODUCTION_DHCP_DNS_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
# ITEM-404: the SNTP-synchronized boot now promotes the management TLS
# lifecycle, and the promotion needs the same hardware RNG the appliance
# certificate needs, so this proof runs the CPU model that advertises
# RDRAND/RDSEED (the ITEM-386 service proof does the same).
QEMU_CPU=${QEMU_CPU:-max}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}
GUEST_MAC='52:54:00:12:34:56'
# ITEM-494: the same proof on the second vendor's controller.  The peer, the
# lease, the name and the clock are identical; only the emulated part changes,
# so a run with HFOC_PRODUCTION_DHCP_DNS_NIC=e1000 proves the production packet
# path through the Intel 8254x driver instead of virtio-net, and the records
# are required to name that path (driver=E1000, state=AVAILABLE, backend=E1000).
NIC_KIND=${HFOC_PRODUCTION_DHCP_DNS_NIC:-virtio}
case $NIC_KIND in
    virtio|e1000) ;;
    *)
        echo "HFOC_PRODUCTION_DHCP_DNS_NIC must be virtio or e1000" >&2
        exit 1
        ;;
esac

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Production DHCP/DNS test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "Production DHCP/DNS test must set QEMU_SMP=1" >&2
    exit 1
}
test -f "$IMAGE" || {
    echo "Production DHCP/DNS test requires image: $IMAGE" >&2
    exit 1
}
echo "QEMU production DHCP/DNS baseline: machine=q35 smp=1 cpu=$QEMU_CPU nic=$NIC_KIND"

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
    artifact_dir="build/test-artifacts/production-dhcp-dns-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-Production DHCP/DNS test failed}" \
        >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" \
        "$FAILURE_REASON" "$artifact_dir/" 2>/dev/null || true
    echo "Production-DHCP/DNS artifacts: $artifact_dir" >&2
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

if [[ $NIC_KIND == e1000 ]]; then
    # ITEM-494: the Intel 82540EM QEMU emulates, alone on the segment.  No
    # virtio-net function is present, so the ordinary boot must select the
    # second vendor's driver and carry the whole proof on it.
    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" -cpu "$QEMU_CPU" \
        -display none -cdrom "$IMAGE" \
        -serial "file:$SERIAL_LOG" \
        -monitor "unix:$MONITOR_SOCKET,server,nowait" \
        -netdev socket,id=net0,listen=127.0.0.1:"$NET_PORT" \
        -device e1000,netdev=net0,mac="$GUEST_MAC" \
        -no-reboot -no-shutdown \
        >"$QEMU_LOG" 2>&1 &
else
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
fi
QEMU_PID=$!

validation_output=
if ! validation_output=$(python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" \
    "$NET_PORT" "$QEMU_PID" "$NIC_KIND" 2>&1 <<'PY'
import os
import pathlib
import re
import socket
import struct
import sys
import time

serial_path = pathlib.Path(sys.argv[1])
monitor_path = pathlib.Path(sys.argv[2])
net_port = int(sys.argv[3])
qemu_pid = int(sys.argv[4])
nic_kind = sys.argv[5]


def marker_timeout(marker, timeout):
    """ITEM-412: the proof stopped without seeing a marker inside its window.

    The record is what makes this a timing class rather than an assertion:
    scripts/run-qemu-suite.sh reads it and retries the target once, visibly
    (docs/ci-self-hosted-runner.md). A `state=FAILED` record, a panic or a lost
    lease is never reported this way.
    """
    sys.stderr.write(
        f'HFOC-QEMU-MARKER-TIMEOUT marker="{marker}" deadline={timeout:g}s\n')
    sys.stderr.flush()
    raise SystemExit(f"timed out waiting for {marker}")


guest_mac = bytes([0x52, 0x54, 0x00, 0x12, 0x34, 0x56])
host_mac = bytes([0x02, 0x00, 0x00, 0x00, 0x00, 0x02])
dns_mac = bytes([0x02, 0x00, 0x00, 0x00, 0x00, 0x03])
guest_ip = bytes([10, 0, 2, 15])
server_ip = bytes([10, 0, 2, 2])
dns_ip = bytes([10, 0, 2, 3])
broadcast_ip = bytes([255, 255, 255, 255])
broadcast_mac = b"\xff" * 6
dhcp_xid = 0x48464F43
# ITEM-408: a lease the proof can outlive.  The appliance renews at half of it
# (with a five-second floor), so a ten-second lease puts the first renewal five
# seconds into the run - early enough that the whole proof observes the refused
# cycle and the recovery even when the guest is starved by the parallel QEMU
# phases on this host, which is exactly how the first CI run of this proof
# missed the renewal entirely (the serial log carried no renewal record).
dhcp_lease_seconds = 10
# ITEM-408: the first renewal cycle is refused on purpose.  The appliance
# sends a unicast renew request and then a broadcast rebind request before its
# bounded budget runs out; answering nothing for exactly those two frames is
# what makes the "a working lease survives a transient failure" claim a real
# observation instead of a code reading.
renew_requests_to_refuse = 2
ready_marker = "[PRODUCTION-NET] device=PRESENT readiness=READY"
dhcp_marker = "[PRODUCTION-DHCP] state=ACTIVE"
dns_marker = "[PRODUCTION-DNS] state=RESOLVED answer=10.0.2.15"
time_marker = "[PRODUCTION-NET-MODE] mode=DHCP clock=READY"
# ITEM-404: the honest pre-clock lifecycle record and the promoted one.
tls_awaiting_marker = \
    "[PRODUCTION-TLS] management_tls v=1 state=AWAITING_CERTIFICATE"
tls_active_marker = "[PRODUCTION-TLS] management_tls v=1 state=ACTIVE"
# ITEM-404: a release-era wall clock inside the sealed certificate's
# documented fallback validity window (2026-01-01 .. 2036-01-01).  The
# appliance generates that window on a first boot with no validated clock, so
# a fixture clock before the window would keep the lifecycle honestly
# awaiting a valid certificate; the release-era clock lets the proof observe
# the promotion the ordinary boot performs once SNTP has validated the time.
sntp_epoch = 1789000000
sntp_port = 50001
sntp_ntp_offset = 2208988800


# ITEM-528: the periodic IRQ lines are the ones written from interrupt context;
# records that begin with the same prefixes ("[IRQ ] IF enabled; entering HLT
# event loop.", "[PIC ] Master mask: ...") are console output and must survive.
IRQ_DIAGNOSTIC = re.compile(
    r"\[(?:IOAPIC|LAPIC-TIMER)\] irq[0-9]*"
    r"(?: vector=0x[0-9A-Fa-f]+| lapic_eoi)[^\[\r\n]*\r?\n")


def normalise(text):
    # Interrupt-context diagnostics can land in the middle of a console line,
    # which the guest prints one character at a time, so they are removed
    # before any marker is searched for.
    return IRQ_DIAGNOSTIC.sub("", text)


def serial_text():
    try:
        return normalise(
            serial_path.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""


def qemu_running():
    try:
        os.kill(qemu_pid, 0)
        return True
    except ProcessLookupError:
        return False


def checksum(data):
    if len(data) % 2:
        data += b"\x00"
    total = 0
    for index in range(0, len(data), 2):
        total += (data[index] << 8) | data[index + 1]
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


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
    payload += bytes([3, 4]) + server_ip
    payload += bytes([6, 4]) + dns_ip
    payload += bytes([54, 4]) + server_ip
    payload += bytes([51, 4]) + struct.pack("!I", dhcp_lease_seconds)
    payload += bytes([255])
    if len(payload) < 300:
        payload += b"\x00" * (300 - len(payload))
    return bytes(payload)


def build_udp_frame(dst_mac, src_mac, src_ip, dst_ip, src_port, dst_port,
                    payload):
    udp_length = 8 + len(payload)
    udp = bytearray()
    udp += struct.pack("!HHHH", src_port, dst_port, udp_length, 0)
    udp += payload
    pseudo = src_ip + dst_ip + b"\x00\x11" + struct.pack("!H", udp_length)
    udp[6:8] = struct.pack("!H", checksum(pseudo + bytes(udp)))
    ip = bytearray()
    ip += b"\x45\x00"
    ip += struct.pack("!H", 20 + len(udp))
    ip += b"\x22\x33"
    ip += b"\x00\x00"
    ip += b"\x40\x11"
    ip += b"\x00\x00"
    ip += src_ip
    ip += dst_ip
    ip[10:12] = struct.pack("!H", checksum(bytes(ip)))
    frame = bytearray()
    frame += dst_mac
    frame += src_mac
    frame += b"\x08\x00"
    frame += ip
    frame += udp
    if len(frame) < 60:
        frame += b"\x00" * (60 - len(frame))
    return bytes(frame)


def build_dhcp_reply(message_type, xid):
    payload = build_dhcp_payload(message_type, xid)
    return build_udp_frame(broadcast_mac, host_mac, server_ip,
                           broadcast_ip, 67, 68, payload)


def build_arp_reply(target_mac, sender_mac=dns_mac, sender_ip=dns_ip):
    frame = bytearray()
    frame += target_mac
    frame += sender_mac
    frame += b"\x08\x06"
    frame += b"\x00\x01"
    frame += b"\x08\x00"
    frame += b"\x06\x04"
    frame += b"\x00\x02"
    frame += sender_mac
    frame += sender_ip
    frame += target_mac
    frame += guest_ip
    frame += b"\x00" * (60 - len(frame))
    return bytes(frame)


def build_icmp_echo_request(identifier, sequence):
    message = bytearray()
    message += b"\x08\x00"
    message += b"\x00\x00"
    message += struct.pack("!HH", identifier, sequence)
    message += b"hfoc-item-408"
    message[2:4] = struct.pack("!H", checksum(bytes(message)))
    ip = bytearray()
    ip += b"\x45\x00"
    ip += struct.pack("!H", 20 + len(message))
    ip += b"\x40\x08"
    ip += b"\x00\x00"
    ip += b"\x40\x01"
    ip += b"\x00\x00"
    ip += server_ip
    ip += guest_ip
    ip[10:12] = struct.pack("!H", checksum(bytes(ip)))
    frame = bytearray()
    frame += guest_mac
    frame += host_mac
    frame += b"\x08\x00"
    frame += ip
    frame += message
    if len(frame) < 60:
        frame += b"\x00" * (60 - len(frame))
    return bytes(frame)


def parse_icmp_echo_reply(frame, identifier, sequence):
    if len(frame) < 14 + 20 + 8 or frame[12:14] != b"\x08\x00":
        return False
    if frame[14] != 0x45 or frame[23] != 1:
        return False
    ihl = (frame[14] & 0x0F) * 4
    total = struct.unpack("!H", frame[16:18])[0]
    icmp = 14 + ihl
    if total < ihl + 8 or len(frame) < icmp + 8:
        return False
    if frame[icmp] != 0 or frame[icmp + 1] != 0:
        return False
    if struct.unpack("!HH", frame[icmp + 4:icmp + 8]) != (identifier, sequence):
        return False
    return checksum(frame[icmp:icmp + (total - ihl)]) == 0


def build_sntp_response(request_payload):
    response = bytearray(48)
    response[0] = 0x24
    response[1] = 2
    response[2] = 4
    response[3] = 0xEC
    if len(request_payload) >= 48:
        response[24:32] = request_payload[40:48]
    stamp = struct.pack("!II", sntp_epoch + sntp_ntp_offset, 0)
    response[32:40] = stamp
    response[40:48] = stamp
    return bytes(response)


def build_dns_response(query_payload):
    transaction_id = struct.unpack("!H", query_payload[0:2])[0]
    # Skip the question name and copy it verbatim.
    position = 12
    while query_payload[position] != 0:
        position += 1 + query_payload[position]
    position += 1
    question = query_payload[12:position + 4]
    payload = bytearray()
    payload += struct.pack("!H", transaction_id)
    payload += struct.pack("!H", 0x8180)
    payload += struct.pack("!H", 1)
    payload += struct.pack("!H", 1)
    payload += struct.pack("!H", 0)
    payload += struct.pack("!H", 0)
    payload += question
    payload += b"\xC0\x0C"
    payload += struct.pack("!H", 1)
    payload += struct.pack("!H", 1)
    payload += struct.pack("!I", 60)
    payload += struct.pack("!H", 4)
    payload += guest_ip
    return build_udp_frame(guest_mac, dns_mac, dns_ip, guest_ip, 53, 49152,
                           bytes(payload))


def parse_udp(frame):
    if len(frame) < 42 or frame[12:14] != b"\x08\x00":
        return None
    if frame[14] != 0x45 or frame[23] != 17:
        return None
    ihl = (frame[14] & 0x0F) * 4
    udp = 14 + ihl
    if len(frame) < udp + 8:
        return None
    src_port, dst_port, udp_length = struct.unpack(
        "!HHH", frame[udp:udp + 6])
    payload = frame[udp + 8:udp + udp_length]
    return (src_port, dst_port, payload)


def parse_arp(frame):
    if len(frame) < 42 or frame[12:14] != b"\x08\x06":
        return None
    if struct.unpack("!H", frame[20:22])[0] != 1:
        return None
    return frame[38:42]


served = {
    "offer": 0, "ack": 0, "arp": 0, "dns": 0, "sntp": 0,
    "renew": 0, "icmp": 0, "echo_sent": 0,
}
echo_identifier = 0x0408
echo_sequence = 1


def serve(frame):
    # ITEM-408: the echo reply is counted wherever it arrives, not only in the
    # phase that asked for it, so the answer to the request cannot be read as
    # an unrelated frame by an earlier wait loop.
    if parse_icmp_echo_reply(frame, echo_identifier, echo_sequence):
        served["icmp"] += 1
        return
    udp = parse_udp(frame)
    if udp is not None:
        src_port, dst_port, payload = udp
        if src_port == 68 and dst_port == 67 and len(payload) >= 240:
            message_type = None
            position = 240
            while position + 1 < len(payload):
                code = payload[position]
                if code == 255:
                    break
                if code == 0:
                    position += 1
                    continue
                length = payload[position + 1]
                if code == 53 and length == 1:
                    message_type = payload[position + 2]
                    break
                position += 2 + length
            xid = struct.unpack("!I", payload[4:8])[0]
            ciaddr = struct.unpack("!I", payload[12:16])[0]
            if message_type == 1 and xid == dhcp_xid:
                connection.sendall(struct.pack("!I", len(build_dhcp_reply(2, xid))) +
                                   build_dhcp_reply(2, xid))
                served["offer"] += 1
            elif message_type == 3 and xid == dhcp_xid:
                if ciaddr != 0:
                    # ITEM-408: this is the appliance renewing the address it
                    # already holds.  The first cycle is refused on purpose so
                    # the proof can see what the appliance does when a renewal
                    # the server never answers.
                    served["renew"] += 1
                    if served["renew"] <= renew_requests_to_refuse:
                        if served["echo_sent"] == 0:
                            served["echo_sent"] = 1
                            request = build_icmp_echo_request(
                                echo_identifier, echo_sequence)
                            connection.sendall(
                                struct.pack("!I", len(request)) + request)
                        return
                connection.sendall(struct.pack("!I", len(build_dhcp_reply(5, xid))) +
                                   build_dhcp_reply(5, xid))
                served["ack"] += 1
            return
        if src_port == 49152 and dst_port == 53:
            response = build_dns_response(payload)
            connection.sendall(struct.pack("!I", len(response)) + response)
            served["dns"] += 1
            return
        if src_port == sntp_port and dst_port == 123:
            response = build_udp_frame(
                frame[6:12], dns_mac, server_ip, guest_ip, 123, sntp_port,
                build_sntp_response(payload))
            connection.sendall(struct.pack("!I", len(response)) + response)
            served["sntp"] += 1
            return
    arp_target = parse_arp(frame)
    if arp_target == dns_ip:
        reply = build_arp_reply(frame[6:12])
        connection.sendall(struct.pack("!I", len(reply)) + reply)
        served["arp"] += 1
    elif arp_target == server_ip:
        reply = build_arp_reply(frame[6:12], dns_mac, server_ip)
        connection.sendall(struct.pack("!I", len(reply)) + reply)
        served["arp"] += 1


buffer = b""


def next_frame(timeout):
    global buffer
    deadline = time.monotonic() + timeout
    while len(buffer) < 4:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return None
        connection.settimeout(remaining)
        try:
            chunk = connection.recv(65536)
        except socket.timeout:
            return None
        if not chunk:
            raise SystemExit("QEMU network socket closed")
        buffer += chunk
    length = struct.unpack("!I", buffer[:4])[0]
    while len(buffer) < 4 + length:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return None
        connection.settimeout(remaining)
        try:
            chunk = connection.recv(65536)
        except socket.timeout:
            return None
        if not chunk:
            raise SystemExit("QEMU network socket closed")
        buffer += chunk
    frame = buffer[4:4 + length]
    buffer = buffer[4 + length:]
    return frame


with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as connection:
    connection.settimeout(5.0)
    deadline = time.monotonic() + 15.0
    while True:
        try:
            connection.connect(("127.0.0.1", net_port))
            break
        except OSError as exc:
            if time.monotonic() >= deadline:
                raise SystemExit(f"QEMU socket backend did not accept: {exc}")
            if not qemu_running():
                raise SystemExit("QEMU exited before network connection")
            time.sleep(0.05)

    deadline = time.monotonic() + 90.0
    dhcp_seen = False
    dns_seen = False
    time_seen = False
    while time.monotonic() < deadline:
        text = serial_text()
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit("Kernel panic during production DHCP/DNS proof")
        if dhcp_marker in text:
            dhcp_seen = True
        if dns_marker in text:
            dns_seen = True
        if time_marker in text:
            time_seen = True
        if dhcp_seen and dns_seen and time_seen:
            break
        if not qemu_running():
            raise SystemExit("QEMU exited before DHCP/DNS proof completed")
        frame = next_frame(0.2)
        if frame is not None:
            serve(frame)

    text = serial_text()
    if ready_marker not in text:
        raise SystemExit("production network did not report an ordinary-boot link")
    if nic_kind == "e1000":
        # ITEM-494: the second vendor is not proven by the driver coming up
        # but by the whole production path running on it.  The record must
        # name the Intel driver, the state that means the machine can use the
        # part, and the address the driver read from the part - a record with
        # `mac=NONE` or `state=SUPPORTED` would mean the proof ran on
        # discovery, not on the bound frame path.
        if not re.search(
                r"\[NIC\] bdf=[0-9A-Fa-f:.]+ vendor=8086 device=100E "
                r"driver=E1000 mac=52:54:00:12:34:56 state=AVAILABLE", text):
            raise SystemExit(
                "the production path did not publish an AVAILABLE Intel "
                "record with the address the driver read")
        # The summary the operator reads on an unfamiliar server must count
        # the bound part as available, not merely as present.  It is written
        # with the record above, before the DHCP exchange this proof waits for,
        # so it is part of the same reading.
        if not re.search(
                r"\[NIC\] devices=1 supported=[0-9]+ available=1 "
                r"unsupported=0 error=0 absent=0", text):
            raise SystemExit(
                "the NIC summary did not count the bound Intel part as "
                "available")
        if not re.search(r"\[PRODUCTION-NET\][^\n]*backend=E1000", text):
            raise SystemExit(
                "the production network record did not name the Intel backend")
    if not dhcp_seen:
        raise SystemExit("DHCP lease was not acquired on ordinary boot")
    if not dns_seen:
        raise SystemExit("DNS A record was not resolved on ordinary boot")
    if not time_seen:
        raise SystemExit("SNTP clock was not synchronized on ordinary boot")
    if served["offer"] < 1 or served["ack"] < 1:
        raise SystemExit("host DHCP server did not observe discover/request")
    if served["arp"] < 1:
        raise SystemExit("guest did not resolve the DNS next hop via ARP")
    if served["dns"] < 1:
        raise SystemExit("guest did not send a DNS query")
    if served["sntp"] < 1:
        raise SystemExit("guest did not send an SNTP request")
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic during production DHCP/DNS proof")
    if "device=ABSENT" in text:
        raise SystemExit("production network reported an absent NIC")

    # ITEM-404: the SNTP-validated clock must reach the ITEM-375 management TLS
    # lifecycle.  Before the clock is validated the record stays honestly
    # AWAITING_CERTIFICATE with the certificate unavailable; after it, the boot
    # certificate is imported and the lifecycle becomes ACTIVE with
    # certificate=READY and no rejection, so a machine with real network time
    # no longer reports a lifecycle that can never leave the awaited state.
    if tls_awaiting_marker not in text:
        raise SystemExit(
            "the management TLS lifecycle did not publish the awaited state "
            "before the clock was validated")
    tls_deadline = time.monotonic() + 60.0
    while time.monotonic() < tls_deadline:
        text = serial_text()
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit("Kernel panic during the TLS lifecycle proof")
        if tls_active_marker in text:
            break
        frame = next_frame(0.2)
        if frame is not None:
            serve(frame)
        if not qemu_running():
            break
    if tls_active_marker not in text:
        raise SystemExit(
            "the management TLS lifecycle did not become ACTIVE after the "
            "SNTP clock was validated")
    if not re.search(
            r"\[PRODUCTION-TLS\].*state=ACTIVE.*certificate=READY"
            r".*staged=NONE.*import=1.*reject=0", text):
        raise SystemExit(
            "the active management TLS record is missing the imported "
            "certificate or reports a rejection")

    # ITEM-394: once the lease is active the appliance publishes the management
    # address and URL where an operator without a serial capture can read them.
    # The console mirrors to serial, so the console line is asserted here, and
    # the scope is the default deny-by-default one because this boot entry
    # declares no management opt-in.  The loop above stops as soon as the
    # DHCP/DNS/time markers appear, and the event loop publishes those markers
    # in the same iteration *before* the management record, so the record is
    # read with a bounded wait instead of racing the guest's write.
    management_url = "https://10.0.2.15/"
    management_line = (f"MANAGEMENT: {management_url} "
                       "reachability=LOCAL_ONLY cert=")
    management_record = (f"[PRODUCTION-MGMT] url={management_url} "
                         "reachability=LOCAL_ONLY cert=")
    management_deadline = time.monotonic() + 30.0
    while time.monotonic() < management_deadline:
        text = serial_text()
        if management_line in text and management_record in text:
            break
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit("Kernel panic during production DHCP/DNS proof")
        frame = next_frame(0.2)
        if frame is not None:
            serve(frame)
        if not qemu_running():
            break
    if management_line not in text:
        raise SystemExit(
            "the console did not publish the management address and URL")
    if management_record not in text:
        raise SystemExit(
            "the management record did not publish the address and scope")
    if "[PRODUCTION-MGMT-BIND] scope=MANAGEMENT_INTERFACE" in text:
        raise SystemExit(
            "the default boot must not expose the management interface")

    # ITEM-408: the lease must survive the renewal the server refused.
    #
    # The acknowledgement above carried a short lease, so the appliance renews
    # it at half of that; the first renewal cycle is deliberately unanswered.
    # The proof requires the appliance to keep the address applied, to keep
    # answering for it (the echo request sent when the renewal was refused
    # must come back as a reply), to name the failure in the record instead of
    # reporting FAILED, and to return to ACTIVE on the same address once the
    # server answers the re-acquisition.
    # Guest time lags wall time when the parallel QEMU phases starve this
    # guest, so the window is generous: the appliance's own records decide the
    # outcome, not how fast the host happens to be.
    resilience_deadline = time.monotonic() + 150.0
    hold_seen = False
    recovered = False
    hold_marker = "reason=REBIND_TIMEOUT hold=1"
    recover_marker = ("[PRODUCTION-DHCP] state=ACTIVE lease=ACQUIRED "
                      "address=10.0.2.15 reason=NONE hold=0")
    while time.monotonic() < resilience_deadline:
        text = serial_text()
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit("Kernel panic during the lease-resilience proof")
        if "[PRODUCTION-DHCP] state=FAILED" in text:
            raise SystemExit(
                "a refused renewal tore the live lease down")
        if hold_marker in text:
            hold_seen = True
            # The recovery is only the record that follows the hold: the
            # ordinary-boot ACTIVE record is also in the log, and matching it
            # would prove nothing about what happened after the failure.
            tail = text.split(hold_marker, 1)[1]
            if recover_marker in tail:
                recovered = True
        if hold_seen and recovered and served["icmp"] >= 1:
            break
        frame = next_frame(0.2)
        if frame is not None:
            serve(frame)
        if not qemu_running():
            break
    text = serial_text()
    if served["echo_sent"] != 1:
        if time.monotonic() >= resilience_deadline and qemu_running():
            # ITEM-412: the window closed while the guest was still running and
            # the renewal had never been observed. That is a starved wait - the
            # guest's time lags wall time under load (ITEM-408) - not a claim
            # that the appliance is wrong, so it is reported as the timing class
            # and may be retried once, visibly. A guest that died, a FAILED
            # record or a panic above keeps failing as the assertion it is.
            sys.stderr.write(
                "the appliance never renewed the lease it was granted\n")
            marker_timeout(
                "[PRODUCTION-DHCP] renewal of the granted lease", 150.0)
        raise SystemExit(
            "the appliance never renewed the lease it was granted")
    if served["renew"] < renew_requests_to_refuse:
        raise SystemExit(
            "the appliance did not retry the refused renewal")
    if served["icmp"] < 1:
        raise SystemExit(
            "the appliance did not answer an echo request for its own "
            "address while the renewal was failing")
    if not hold_seen:
        raise SystemExit(
            "the refused renewal was not published with its reason and the "
            "address it kept")
    if not recovered:
        raise SystemExit(
            "the appliance did not return to ACTIVE after the re-acquisition")
    # The address the appliance was granted is never lost: once a record says
    # `lease=ACQUIRED`, no later record may say `lease=NONE`.  The check is
    # anchored on the grant rather than on the count of pre-grant records,
    # because a machine whose link comes up after the boot - the Intel path -
    # publishes an honest `lease=NONE` record while it waits for the link, and
    # that record is not a lost lease.
    granted_at = text.find("lease=ACQUIRED")
    if granted_at < 0:
        raise SystemExit("the appliance never recorded the granted lease")
    if "lease=NONE" in text[granted_at:]:
        raise SystemExit(
            "the appliance published a lost lease after it had been granted "
            "one")
    if served["offer"] < 2 or served["ack"] < 2:
        raise SystemExit(
            "the appliance did not re-acquire the held address")

    monitor_deadline = time.monotonic() + 5.0
    while not monitor_path.exists():
        if time.monotonic() >= monitor_deadline:
            raise SystemExit("timed out waiting for QEMU monitor socket")
        time.sleep(0.05)

    # ITEM-428: the appliance says it is alive, and the addressing record says
    # what the wire did.
    #
    # The 2026-09-16 hardware run's boot log ended on
    # `[PRODUCTION-DHCP] ... attempt=11/40` and nothing after it, so a machine
    # that was up with nothing to report and a machine that had stopped looked
    # the same.  Two things are required here: the periodic liveness record
    # reaches the serial log with its counters, and every addressing record
    # carries the wire view (discover/request/offer/ack/refused), so "no lease"
    # can never again be three causes at once.  The guest runs the virtio
    # backend, so the counters are the virtio frame path's - the shape is the
    # contract, the Realtek numbers are what the hardware run reads.
    #
    # The heartbeat is required once, not twice.  Its cadence is ten seconds of
    # the guest's own clock, and this guest is emulated under TCG beside the
    # phase's other guests: how long a second heartbeat takes in *wall* time is
    # a property of the host's load, not of the appliance, and ITEM-421/426/427
    # spent three items on exactly that confusion.  The cadence itself -
    # intervals, a clock that stands still or wraps, the interval index - is
    # proven against the real decision function by `test-loop-heartbeat-host`,
    # and what this proof owes is the integration: the record is rendered,
    # mirrored to serial and carries the live frame-path counters.
    heartbeat_lines = [line for line in text.splitlines()
                       if "[PRODUCTION-LOOP] state=RUNNING" in line]
    if not heartbeat_lines:
        # The heartbeat is published on the loop's first turn, long before this
        # check runs, so its absence is not a wait that ran out: it is the
        # record missing from the boot, and it fails closed without a retry.
        raise SystemExit(
            "the appliance published no [PRODUCTION-LOOP] heartbeat record")

    # ITEM-494: the receive counters are read from a heartbeat published after
    # the lease, not from the first one.  The first heartbeat is written on the
    # loop's very first turn, which on a machine whose PHY negotiates (the
    # Intel path) is before its link exists, and the honest `rx_ok=0` of that
    # line says nothing about the receive path.  Wait, with a bound, for a
    # heartbeat that carries a frame the DHCP exchange already proved arrived:
    # a starved wait is reported as the timing class and may be retried once,
    # while a machine that answers DHCP and never receives still fails the
    # assertion below.
    def heartbeat_frame_seen(lines):
        for line in lines:
            rx_ok = re.search(r"rx_ok=([0-9]+)", line)
            rx_drop = re.search(r"rx_drop=([0-9]+)", line)
            if rx_ok is not None and rx_drop is not None and \
                    int(rx_ok.group(1)) + int(rx_drop.group(1)) > 0:
                return True
        return False

    receive_deadline = time.monotonic() + 90.0
    while not heartbeat_frame_seen(heartbeat_lines) and \
            time.monotonic() < receive_deadline:
        if not qemu_running():
            break
        time.sleep(1.0)
        text = serial_text()
        heartbeat_lines = [line for line in text.splitlines()
                           if "[PRODUCTION-LOOP] state=RUNNING" in line]
    if not heartbeat_frame_seen(heartbeat_lines) and qemu_running():
        sys.stderr.write(
            "the appliance never published a heartbeat carrying a received "
            "frame\n")
        marker_timeout(
            "[PRODUCTION-LOOP] heartbeat with rx_ok+rx_drop > 0", 90.0)
    heartbeat_ticks = []
    for line in heartbeat_lines:
        match = re.search(r"ticks=([0-9]+)", line)
        if match is None:
            raise SystemExit(
                "the loop heartbeat does not name the tick it was "
                "published at")
        heartbeat_ticks.append(int(match.group(1)))
    if len(heartbeat_ticks) >= 2 and (
            heartbeat_ticks != sorted(heartbeat_ticks) or
            heartbeat_ticks[-1] <= heartbeat_ticks[0]):
        raise SystemExit(
            "the loop heartbeat tick count does not advance")
    for interval_index, line in enumerate(heartbeat_lines, start=1):
        match = re.search(r"interval=([0-9]+)", line)
        if match is None or int(match.group(1)) != interval_index:
            raise SystemExit(
                "the loop heartbeat interval index is not the count of "
                "published heartbeats")
    if not re.search(
            r"\[PRODUCTION-LOOP\].*tx_sub=([1-9][0-9]*).*tx_recl=[0-9]+"
            r".*tx_full=[0-9]+.*tx_rst=[0-9]+.*rx_ok=[0-9]+"
            r".*rx_drop=[0-9]+.*rx_err=[0-9]+.*rx_arm=[0-9]+",
            "\n".join(heartbeat_lines)):
        raise SystemExit(
            "the loop heartbeat does not carry the frame-path counters, or "
            "no frame was ever submitted")
    # ITEM-439: a machine that received a DHCP lease received frames, so its
    # own record has to say so.  The heartbeat used to accept `rx_ok=0` - the
    # shape the 2026-09-17 hardware bisect read as "no receive at all" - and
    # never required `rx_drop` or `rx_arm`, so a proof of the receive path
    # could pass on a machine whose receiver was not armed.  `rx_arm=1` is the
    # backend's own answer to "do you hold the receive ring", `rx_ok` counts
    # the frames the stack accepted and `rx_drop` the ones it refused, and
    # between them a broadcast frame that arrived is at least one of the two.
    #
    # The whole run is read, not one line: the heartbeats start before the
    # lease does, so the first line carrying the counters has every right to
    # read zero.  What must hold is that some line shows the ring armed and
    # some line shows a frame the stack accepted or refused.
    receive_counters_seen = False
    receive_armed_seen = False
    receive_frames_seen = False
    for line in heartbeat_lines:
        rx_arm = re.search(r"rx_arm=([0-9]+)", line)
        rx_ok = re.search(r"rx_ok=([0-9]+)", line)
        rx_drop = re.search(r"rx_drop=([0-9]+)", line)
        if rx_arm is None or rx_ok is None or rx_drop is None:
            continue
        receive_counters_seen = True
        if int(rx_arm.group(1)) == 1:
            receive_armed_seen = True
        if int(rx_ok.group(1)) + int(rx_drop.group(1)) > 0:
            receive_frames_seen = True
    if not receive_counters_seen:
        raise SystemExit(
            "no heartbeat line carries the receive-path counters")
    if not receive_armed_seen:
        raise SystemExit(
            "the receive ring is not armed on a machine that answered DHCP")
    if not receive_frames_seen:
        raise SystemExit(
            "the receive path accepted and refused no frame at all on a "
            "machine that answered DHCP")
    # ITEM-436: the same heartbeat carries the input path's traffic, so a
    # machine whose console and menu never answer is read from its own record:
    # `kbd_reports` counts the completions that carried a whole 8-byte boot
    # report, `kbd_ref` the ones that did not, and `kbd_last` the byte count of
    # the most recent completion. This proof boots without a USB keyboard, so
    # the fields are present and zero - their names and their shape are what it
    # owes; the movement is the USB keyboard proof's and the host proof's.
    #
    # ITEM-438: `kbd_keys` (what those reports carried) and `kbd_silent` (the
    # window reached with no report at all) are on the same line. Both are 0
    # here for the same reason - no keyboard is bound - and that is the shape
    # the acceptance host's next run compares itself against.
    if not re.search(
            r"\[PRODUCTION-LOOP\].*kbd_reports=[0-9]+.*kbd_xfer=[0-9]+"
            r".*kbd_err=[0-9]+.*kbd_ref=[0-9]+.*kbd_last=[0-9]+"
            r".*kbd_keys=[0-9]+.*kbd_silent=[0-9]+",
            "\n".join(heartbeat_lines)):
        raise SystemExit(
            "the loop heartbeat does not carry the keyboard counters")

    dhcp_lines = [line for line in text.splitlines()
                  if line.startswith("[PRODUCTION-DHCP]")]
    if not dhcp_lines:
        raise SystemExit("the appliance published no addressing record")
    wire_view = None
    for line in dhcp_lines:
        match = re.search(
            r"discover=([0-9]+) request=([0-9]+) offer=([0-9]+) "
            r"ack=([0-9]+) refused=([0-9]+)", line)
        if match is not None:
            wire_view = tuple(int(value) for value in match.groups())
    if wire_view is None:
        raise SystemExit(
            "no addressing record carries the DHCP wire view "
            "(discover/request/offer/ack/refused)")
    if wire_view[0] < 1 or wire_view[2] < 1 or wire_view[3] < 1:
        raise SystemExit(
            "the wire view does not show a discover, an offer and an "
            "acknowledgement")

    # ITEM-428: the live backend runs with memory space and bus mastering
    # enabled.  The 2026-09-16 hardware run's Realtek part answered every
    # register read while its transmit ring never returned a descriptor, and
    # bit 2 of this register is the one bit that separates those two facts.
    pci_command_match = re.search(
        r"\[PRODUCTION-NET\].*pci_cmd=0x([0-9A-Fa-f]+)", text)
    if pci_command_match is None:
        raise SystemExit(
            "the network record does not publish the PCI command register")
    if (int(pci_command_match.group(1), 16) & 0x0004) == 0:
        raise SystemExit(
            "the live backend runs without PCI bus mastering enabled")

print("Production link on ordinary boot: found")
print(f"DHCP discover/request/ack: {served['offer']}/{served['ack']}")
print("DHCP lease publication: found")
print("DNS next-hop ARP resolution: found")
print("DNS query and A-record resolution: found")
print("SNTP request and clock synchronization: found")
print("Management TLS lifecycle activation after validated clock: found")
print(f"Refused renewal cycles survived with the lease held: "
      f"renew={served['renew']}")
print("ICMP echo reply for the appliance address: found")
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

echo "QEMU production DHCP/DNS test: OK"
echo "QEMU production network/time test: OK"
