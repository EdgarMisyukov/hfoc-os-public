#!/usr/bin/env bash
# Prove item 230 in one q35 QEMU boot: real virtio-net PCI discovery, exact
# RX/TX Ethernet frames through a socket backend, packet-boundary ownership,
# destination filtering, static profile activation, pressure, link reset,
# balanced pool counts, timer/keyboard liveness, and clean HMP shutdown.
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

if [[ "${HFOC_VIRTIO_NET_FRAME_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_VIRTIO_NET_FRAME_TEST_TIMEOUT:-120}s" \
        env HFOC_VIRTIO_NET_FRAME_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Packet-boundary test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "Packet-boundary test must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "Packet-boundary test must not set QEMU_CPU" >&2
    exit 1
}
echo "QEMU packet-boundary baseline: machine=q35 smp=1 cpu=default"

TEST_DIR=$(mktemp -d)
SERIAL_LOG=$TEST_DIR/com1.log
MONITOR_LOG=$TEST_DIR/monitor.log
QEMU_LOG=$TEST_DIR/qemu.log
INTERRUPTS_LOG=$TEST_DIR/interrupts.log
TRACE_LOG=$TEST_DIR/trace.log
METADATA=$TEST_DIR/metadata.txt
FAILURE_REASON=$TEST_DIR/failure-reason.txt
MONITOR_SOCKET=$TEST_DIR/monitor.sock
NET_PORT_FILE=$TEST_DIR/net-port.txt
NET_PORT=
QEMU_PID=
FAILURE_MESSAGE=
QEMU_WAIT_RESULT=

: >"$SERIAL_LOG"
: >"$MONITOR_LOG"
: >"$QEMU_LOG"
: >"$INTERRUPTS_LOG"
: >"$TRACE_LOG"
rm -f "$MONITOR_SOCKET"

cat >"$TEST_DIR/trace-events.txt" <<'EOF'
virtio_queue_notify
virtio_pci_notify
virtio_pci_notify_write
virtio_pci_notify_write_pio
virtio_set_status
virtio_notify
virtqueue_pop
virtqueue_alloc_element
EOF

NET_PORT=$(python3 - <<'PY'
import socket
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
PY
)
printf '%s\n' "$NET_PORT" >"$NET_PORT_FILE"

printf '%s\n' \
    "machine=$QEMU_MACHINE" \
    "cpu=default" \
    "smp=$QEMU_SMP" \
    "image=$IMAGE" \
    "net_port=$NET_PORT" >"$METADATA"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/packet-boundary-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-Packet-boundary test failed}" \
        >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$INTERRUPTS_LOG" \
        "$TRACE_LOG" "$TEST_DIR/trace-events.txt" \
        "$METADATA" "$FAILURE_REASON" "$artifact_dir/"
    echo "Packet-boundary artifacts: $artifact_dir" >&2
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
    QEMU_WAIT_RESULT=0
    while kill -0 "$QEMU_PID" 2>/dev/null; do
        if (( SECONDS >= deadline )); then
            QEMU_WAIT_RESULT=timeout
            return 1
        fi
        sleep 0.1
    done
    wait "$QEMU_PID" 2>/dev/null
    QEMU_WAIT_RESULT=$?
    return "$QEMU_WAIT_RESULT"
}

send_monitor_quit() {
    python3 - "$MONITOR_SOCKET" <<'PY'
import socket
import sys
import time

def fail(message):
    print(message, file=sys.stderr)
    raise SystemExit(1)

monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    monitor.settimeout(1.0)
    try:
        monitor.connect(sys.argv[1])
    except OSError as exc:
        fail(f"QEMU monitor did not accept quit: {exc}")
    prompt_deadline = time.monotonic() + 5.0
    prompt = b""
    while b"(qemu)" not in prompt:
        remaining = prompt_deadline - time.monotonic()
        if remaining <= 0.0:
            fail("QEMU HMP prompt timeout")
        monitor.settimeout(remaining)
        try:
            chunk = monitor.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            fail("QEMU HMP closed before prompt")
        prompt += chunk
    try:
        monitor.sendall(b"quit\n")
    except OSError as exc:
        fail(f"QEMU monitor did not accept quit: {exc}")
    monitor.settimeout(5.0)
    while True:
        try:
            chunk = monitor.recv(4096)
        except socket.timeout:
            fail("QEMU monitor EOF timeout after quit")
        if not chunk:
            break
finally:
    monitor.close()
PY
}

successful_shutdown() {
    local quit_error
    if ! quit_error=$(send_monitor_quit 2>&1); then
        FAILURE_MESSAGE="${quit_error:-QEMU monitor quit failed}"
        return 1
    fi
    if ! wait_for_exit; then
        if [[ "$QEMU_WAIT_RESULT" == timeout ]]; then
            FAILURE_MESSAGE="QEMU did not exit after monitor quit"
        else
            FAILURE_MESSAGE="QEMU exited with status $QEMU_WAIT_RESULT after monitor quit"
        fi
        return 1
    fi
    return 0
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    if (( status != 0 )) && [[ -d "$TEST_DIR" ]]; then
        best_effort_cleanup
        save_failure_artifacts
    fi
    if [[ -d "$TEST_DIR" ]]; then
        rm -rf "$TEST_DIR"
    fi
    exit "$status"
}

trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" \
    -display none -cdrom "$IMAGE" \
    -serial "file:$SERIAL_LOG" \
    -monitor "unix:$MONITOR_SOCKET,server,nowait" \
    -netdev socket,id=net0,listen=127.0.0.1:"$NET_PORT" \
    -device virtio-net-pci-non-transitional,netdev=net0,ioeventfd=off,\
mac=52:54:00:12:34:56,\
event_idx=off,indirect_desc=off,\
mq=off,mrg_rxbuf=off,ctrl_vq=off,ctrl_rx=off,ctrl_rx_extra=off,\
ctrl_vlan=off,ctrl_mac_addr=off,ctrl_guest_offloads=off,gso=off,\
guest_csum=off,csum=off,guest_tso4=off,guest_tso6=off,guest_ecn=off,\
guest_ufo=off,host_tso4=off,host_tso6=off,host_ecn=off,host_ufo=off,\
host_uso=off,guest_uso4=off,guest_uso6=off,guest_announce=off,\
hash=off,rss=off,guest_rsc_ext=off,vectors=0,x-disable-pcie=on \
    -d int -D "$INTERRUPTS_LOG" -no-reboot -no-shutdown \
    -trace "events=$TEST_DIR/trace-events.txt,file=$TRACE_LOG" \
    >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

validation_output=
if ! validation_output=$(python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" \
    "$NET_PORT" "$QEMU_PID" 2>&1 <<'PY'
import os
import pathlib
import socket
import struct
import sys
import time

serial_path = pathlib.Path(sys.argv[1])
monitor_path = pathlib.Path(sys.argv[2])
net_port = int(sys.argv[3])
qemu_pid = int(sys.argv[4])
direct_frame_count = 7
frame_count = 9
frame_length = 60
tx_frame_length = 64
guest_mac = bytes([0x52, 0x54, 0x00, 0x12, 0x34, 0x56])
host_mac = bytes([0x02, 0x00, 0x00, 0x00, 0x00, 0x01])
rx_ethertype = bytes([0x88, 0xB5])
tx_ethertype = bytes([0x88, 0xB6])
packet_tx_ethertype = bytes([0x88, 0xB8])
command_marker = "UNKNOWN COMMAND: vnfc. TYPE HELP."
timer_marker = "[LAPIC-TIMER] irq vector=0x42"


def serial_text():
    try:
        return serial_path.read_bytes().decode("ascii", errors="replace")
    except FileNotFoundError:
        return ""


def qemu_running():
    try:
        os.kill(qemu_pid, 0)
        return True
    except ProcessLookupError:
        return False


def wait_prompt(monitor):
    chunks = []
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        try:
            chunk = monitor.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            raise SystemExit("QEMU HMP closed before prompt")
        chunks.append(chunk)
        if b"(qemu)" in b"".join(chunks):
            return
    raise SystemExit("QEMU HMP prompt timeout")


def wait_serial_marker(marker, timeout=60.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = serial_text()
        if marker in text:
            return
        if "[VIRTIO-NET-FRAME] failed" in text or \
                "[PACKET-BOUNDARY] failed" in text:
            raise SystemExit("Packet-boundary proof reported failure")
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit("Kernel panic during packet-boundary proof")
        if not qemu_running():
            raise SystemExit("QEMU exited before proof completed")
        time.sleep(0.05)
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="' +
        str(f"timed out waiting for marker: {marker}") + '" '
        'deadline=' + str(int(timeout)) + 's\n')
    sys.stderr.flush()
    raise SystemExit(f"timed out waiting for marker: {marker}")


def build_rx_frame(index):
    if index < direct_frame_count:
        payload = b"HFOC-RX-%02d" % index
        destination = guest_mac
        source = host_mac
        ethertype = rx_ethertype
    elif index == direct_frame_count:
        destination = bytes([0x01, 0x00, 0x5E, 0x00, 0x00, 0x01])
        source = host_mac
        ethertype = rx_ethertype
        payload = b"HFOC-MCAST"
    else:
        destination = guest_mac
        source = host_mac
        ethertype = bytes([0x88, 0xB7])
        payload = b"HFOC-BADTYPE"
    body = ethertype + payload
    return destination + source + body + b"\x00" * (
        frame_length - len(destination) - len(source) - len(body))


def expected_tx_frame(index):
    if index < direct_frame_count:
        payload = b"HFOC-TX-%02d" % index
        ethertype = tx_ethertype
    else:
        payload = b"HFOC-PKT-%02d" % index
        ethertype = packet_tx_ethertype
    body = ethertype + payload
    return host_mac + guest_mac + body + b"\x00" * (
        tx_frame_length - len(host_mac) - len(guest_mac) - len(body))


def recv_exact(connection, length, timeout=30.0):
    data = b""
    deadline = time.monotonic() + timeout
    connection.settimeout(0.5)
    while len(data) < length:
        if time.monotonic() >= deadline:
            raise SystemExit(
                f"timed out reading TX frames: got {len(data)} of {length}")
        try:
            chunk = connection.recv(length - len(data))
        except socket.timeout:
            if not qemu_running():
                raise SystemExit("QEMU exited while reading TX frames")
            continue
        if not chunk:
            raise SystemExit(
                f"QEMU network socket closed: got {len(data)} of {length}")
        data += chunk
    return data


def recv_qemu_frame(connection, timeout=30.0):
    header = recv_exact(connection, 4, timeout)
    frame_length = struct.unpack("!I", header)[0]
    return recv_exact(connection, frame_length, timeout)


with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as network:
    network.settimeout(5.0)
    deadline = time.monotonic() + 10.0
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

    wait_serial_marker("[VIRTIO-NET-FRAME] rx ready")
    for index in range(frame_count):
        frame = build_rx_frame(index)
        network.sendall(struct.pack("!I", len(frame)) + frame)
        time.sleep(0.05)

    wait_serial_marker("[VIRTIO-NET-FRAME] tx ready")
    for index in range(frame_count):
        frame = recv_qemu_frame(network)
        if frame != expected_tx_frame(index):
            raise SystemExit(
                f"TX frame {index} did not match exact expected bytes: "
                f"got {len(frame)} bytes")

wait_serial_marker("[VIRTIO-NET-FRAME] proof OK")
required = [
    "[VIRTIO-NET-FRAME] proof init",
    "[VIRTIO-NET-FRAME] device live",
    "[VIRTIO-NET-FRAME] rx ready",
    "[VIRTIO-NET-FRAME] rx ok",
    "[VIRTIO-NET-FRAME] tx ready",
    "[VIRTIO-NET-FRAME] tx ok",
    "[VIRTIO-NET-FRAME] reset shutdown",
    "[VIRTIO-NET-FRAME] balanced ownership",
    "[VIRTIO-NET-FRAME] clean shutdown",
    "[VIRTIO-NET-FRAME] proof OK",
    "[PACKET-BOUNDARY] proof init",
    "[PACKET-BOUNDARY] pool",
    "[PACKET-BOUNDARY] identity/profile",
    "[PACKET-BOUNDARY] rx ready",
    "[PACKET-BOUNDARY] rx ok",
    "[PACKET-BOUNDARY] tx ready",
    "[PACKET-BOUNDARY] tx ok",
    "[PACKET-BOUNDARY] pressure",
    "[PACKET-BOUNDARY] reset shutdown",
    "[PACKET-BOUNDARY] balanced pool",
    "[PACKET-BOUNDARY] clean shutdown",
    "[PACKET-BOUNDARY] proof OK",
]
text = serial_text()
for marker in required:
    if text.count(marker) != 1:
        raise SystemExit(
            f"Expected 1 marker, got {text.count(marker)}: {marker}")
if "[VIRTIO-NET-FRAME] failed" in text or \
        "[PACKET-BOUNDARY] failed" in text:
    raise SystemExit("Packet-boundary proof reported failure")
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("Kernel panic detected")

timer_deadline = time.monotonic() + 10.0
while time.monotonic() < timer_deadline:
    if timer_marker in serial_text():
        break
    if "*** HFOC KERNEL PANIC ***" in serial_text():
        raise SystemExit("Kernel panic during timer liveness")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="LAPIC timer liveness" '
        'deadline=' + str(int(timer_deadline)) + 's\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for LAPIC timer liveness")

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

    wait_prompt(monitor)
    for key in ("v", "n", "f", "c", "ret"):
        monitor.sendall(f"sendkey {key}\n".encode("ascii"))
        wait_prompt(monitor)
        time.sleep(0.08)

command_deadline = time.monotonic() + 10.0
while time.monotonic() < command_deadline:
    if command_marker in serial_text():
        break
    if "*** HFOC KERNEL PANIC ***" in serial_text():
        raise SystemExit("Kernel panic during keyboard responsiveness")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="keyboard responsiveness" '
        'deadline=' + str(int(command_deadline)) + 's\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for keyboard responsiveness")

PY
); then
    FAILURE_MESSAGE="${validation_output:-Virtio-net-frame validation failed}"
    echo "$FAILURE_MESSAGE" >&2
    best_effort_cleanup
    exit 1
fi

if ! successful_shutdown; then
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi

echo "QEMU packet-boundary test: OK"
