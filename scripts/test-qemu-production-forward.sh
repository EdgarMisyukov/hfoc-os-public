#!/usr/bin/env bash
# ITEM-449 HTTPS proof for the declared forward.
#
# The declaration half proves that the appliance reads, validates and publishes
# the rule.  This proof is the other half: traffic to the declared server really
# goes through the machine.
#
# It installs the forward medium on a disk (the same OVMF/AHCI shape the
# install-flow proof uses), boots the installed disk on the QEMU user-mode
# network with one forwarded port, and then, from the host:
#
#   0. the link is restored after the kernel reads the forward declaration.
#      The unattended install leaves the account unset, so the first-run
#      conversation on the local console sets it (ITEM-506 increment 5) before
#      anything is expected to serve; the typed password must not reach the
#      serial log; SSH, ACME HTTP-01, the panel and the relay all listen;
#   1. a host HTTPS target server listens on the port the declaration names as
#      the target - the guest reaches it as the user-mode gateway 10.0.2.2;
#   2. a host client connects to the forwarded port, which the appliance
#      serves on the declared listen port, verifies the target certificate
#      and sends a challenge;
#   3. the target answers, the answer travels back through the relay, and the
#      client checks every byte both ways;
#   4. the console publishes `[PRODUCTION-FORWARD] state=SESSION verdict=OK`
#      with nonzero byte counters in both directions;
#   5. a second medium declares a client list the source is not on: the
#      connection is refused, the target is never dialled, and the console
#      publishes `state=REFUSED reason=SOURCE_NOT_ALLOWED src=10.0.2.2`.
#
# The access rule is therefore proven where it matters: a source that is not
# declared cannot make the appliance dial the target, and the bytes of a
# declared source make the round trip.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in openssl python3 qemu-system-x86_64 timeout mktemp truncate; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

if [[ "${HFOC_FORWARD_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_FORWARD_TEST_TIMEOUT:-600}s" \
        env HFOC_FORWARD_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
QEMU_MEMORY=${QEMU_MEMORY:-3072M}
FORWARD_IMAGE=${FORWARD_IMAGE:-build/production-installer/hfoc-os-$(<VERSION)-production-installer-forward-ssh-test.iso}
FORWARD_DENIED_IMAGE=${FORWARD_DENIED_IMAGE:-build/production-installer/hfoc-os-$(<VERSION)-production-installer-forward-denied.iso}
FORWARD_TARGET=${HFOC_INSTALL_MEDIA_FORWARD_PROOF_TARGET:-10.0.2.2}
FORWARD_TARGET_PORT=${HFOC_INSTALL_MEDIA_FORWARD_PROOF_TARGET_PORT:-18443}
FORWARD_LISTEN_PORT=${HFOC_INSTALL_MEDIA_FORWARD_PROOF_LISTEN_PORT:-19443}
FORWARD_CLIENTS=${HFOC_INSTALL_MEDIA_FORWARD_PROOF_CLIENTS:-10.0.2.2}
FORWARD_DENIED_CLIENTS=${HFOC_INSTALL_MEDIA_FORWARD_DENIED_CLIENTS:-10.0.2.9}
FORWARD_CLIENT_COUNT=$(awk -F, '{print NF}' <<<"$FORWARD_CLIENTS")
FORWARD_DENIED_CLIENT_COUNT=$(awk -F, '{print NF}' <<<"$FORWARD_DENIED_CLIENTS")
OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
# The one virtio-net profile the product driver binds (ITEM-389), the same one
# the serving proof uses.
NETDEV_DEVICE='virtio-net-pci-non-transitional,netdev=net0,ioeventfd=off,mac=52:54:00:12:34:56,event_idx=off,indirect_desc=off,mq=off,mrg_rxbuf=off,ctrl_vq=off,ctrl_rx=off,ctrl_rx_extra=off,ctrl_vlan=off,ctrl_mac_addr=off,ctrl_guest_offloads=off,gso=off,guest_csum=off,csum=off,guest_tso4=off,guest_tso6=off,guest_ecn=off,guest_ufo=off,host_tso4=off,host_tso6=off,host_ecn=off,host_ufo=off,host_uso=off,guest_uso4=off,guest_uso6=off,guest_announce=off,hash=off,rss=off,guest_rsc_ext=off,vectors=0,x-disable-pcie=on'
# The host side of the exchange: the target server's port is the port the
# declaration names as the target, and the client's port is the forwarded
# listen port on 127.0.0.1.
TARGET_PORT=$FORWARD_TARGET_PORT
LISTEN_PORT=$FORWARD_LISTEN_PORT
CLIENT_PORT=$FORWARD_LISTEN_PORT

for image in "$FORWARD_IMAGE" "$FORWARD_DENIED_IMAGE"; do
    [[ -f "$image" ]] || {
        echo "forward proof requires image: $image" >&2
        exit 1
    }
done

TEST_DIR=$(mktemp -d)

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

QEMU_LOG=$TEST_DIR/qemu.log
FAILURE_MESSAGE=
QEMU_PID=
DISK_BYTES=$((16 * 1024 * 1024 * 1024))
# ITEM-506 increment 5: the unattended install leaves the account unset, so the
# first-run console conversation sets one before the installed appliance is
# expected to serve anything.
ADMIN_SECRET=${HFOC_FORWARD_TEST_ADMIN_SECRET:-hfocforward449}

cleanup() {
    if [[ -n "${QEMU_PID:-}" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    if [[ -n "${TARGET_PID:-}" ]] && kill -0 "$TARGET_PID" 2>/dev/null; then
        kill "$TARGET_PID" 2>/dev/null || true
        wait "$TARGET_PID" 2>/dev/null || true
    fi
}
trap 'cleanup; rm -rf "$TEST_DIR"' EXIT

fail() {
    FAILURE_MESSAGE=$1
    echo "$FAILURE_MESSAGE" >&2
    exit 1
}

monitor_command() {
    local socket=$1
    shift

    python3 - "$socket" "$@" <<'PY'
import socket
import sys
import time

deadline = time.monotonic() + 10.0
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
    monitor.settimeout(1.0)
    while True:
        try:
            monitor.connect(sys.argv[1])
            break
        except (FileNotFoundError, ConnectionRefusedError, OSError):
            if time.monotonic() > deadline:
                raise SystemExit(1)
            time.sleep(0.1)
    monitor.sendall(("\n".join(sys.argv[2:]) + "\n").encode("ascii"))
    time.sleep(0.2)
PY
}

monitor_quit() {
    monitor_command "$1" quit || true
}

# wait_marker LOG MARKER SECONDS
wait_marker() {
    local log=$1
    local marker=$2
    local seconds=$3
    local deadline=$((SECONDS + seconds))

    while (( SECONDS < deadline )); do
        if grep -Fq -- "$marker" "$log"; then
            return 0
        fi
        if [[ -n "${QEMU_PID:-}" ]] && ! kill -0 "$QEMU_PID" 2>/dev/null; then
            break
        fi
        sleep 0.2
    done
    grep -Fq -- "$marker" "$(serial_view "$log")"
}

# install_medium LABEL IMAGE DISK
#
# Boot the installer medium under OVMF with the target AHCI disk attached and
# wait for the install to apply.  The disk must change: an "applied" record on
# a disk that did not move would prove nothing.
install_medium() {
    local label=$1
    local image=$2
    local disk=$3
    local serial_log=$TEST_DIR/$label-install.log
    local monitor_socket=$TEST_DIR/$label-install.monitor
    local vars=$TEST_DIR/$label-install-vars.fd
    local before after

    truncate -s "$DISK_BYTES" "$disk"
    cp "$OVMF_VARS" "$vars"
    before=$(sha256sum "$disk" | awk '{print $1}')
    : >"$serial_log"
    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m "$QEMU_MEMORY" -smp "$QEMU_SMP" \
        -display none \
        -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
        -drive "if=pflash,format=raw,file=$vars" \
        -drive "if=none,id=installed,file=$disk,format=raw,cache=writeback,file.locking=off" \
        -device "ide-hd,drive=installed,bus=ide.0" \
        -cdrom "$image" \
        -serial "file:$serial_log" \
        -monitor "unix:$monitor_socket,server,nowait" \
        -no-reboot -no-shutdown >"$QEMU_LOG" 2>&1 &
    QEMU_PID=$!
    if ! wait_marker "$serial_log" "[PRODUCTION-INSTALL] applied" 300; then
        fail "$label: the install did not apply; the guest said: $(tail -c 600 "$serial_log")"
    fi
    monitor_quit "$monitor_socket"
    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=
    after=$(sha256sum "$disk" | awk '{print $1}')
    [[ "$before" != "$after" ]] || fail "$label: the install wrote nothing"
    echo "$label: installed"
}

# ITEM-506 increment 5: set the one account on the installed appliance's local
# console, the same conversation the serving proof drives.  The install medium
# is unattended, so no account exists on the installed disk; the console is the
# first-run path the contract names.  The helper is an inline proof tool, not
# production code.
cat >"$TEST_DIR/forward-enroll.py" <<'ENROLLEOF'
import pathlib
import re
import socket
import sys
import time

monitor_path = pathlib.Path(sys.argv[1])
serial_path = pathlib.Path(sys.argv[2])
secret = sys.argv[3]

# The periodic IRQ lines are written from interrupt context and can land inside
# a console line; they are complete lines, so removing them restores the
# stream.  ITEM-528 narrowed the rule to those periodic lines: records that
# begin with the same prefixes (e.g. "[IRQ ] IF enabled; entering HLT event
# loop.") are console output and must survive.
DIAGNOSTIC_LINE = re.compile(
    r"\[(?:IOAPIC|LAPIC-TIMER)\] irq[0-9]*"
    r"(?: vector=0x[0-9A-Fa-f]+| lapic_eoi)[^\[\r\n]*\r?\n")


def serial_text():
    try:
        return DIAGNOSTIC_LINE.sub(
            "", serial_path.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""


def wait_for_new(marker, timeout, baseline):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = serial_text()
        if text.count(marker) > baseline:
            return text
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit("Kernel panic during forward enrollment")
        time.sleep(0.05)
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="' +
        str("timed out waiting for: %s" % marker) + '" '
        'deadline=' + str(int(timeout)) + 's\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for: %s" % marker)


def wait_for(marker, timeout):
    return wait_for_new(marker, timeout, 0)


wait_for("[PRODUCTION-FIRST-BOOT]", 90.0)

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
    monitor.settimeout(1.0)
    monitor.connect(str(monitor_path))

    def receive_prompt():
        chunks = []
        while True:
            chunk = monitor.recv(4096)
            if not chunk:
                raise RuntimeError("QEMU monitor closed unexpectedly")
            chunks.append(chunk)
            if b"(qemu)" in b"".join(chunks):
                return

    def send_key(key):
        monitor.sendall(("sendkey " + key + "\n").encode("ascii"))
        receive_prompt()
        time.sleep(0.05)

    def type_line(line):
        for character in line:
            if character == "-":
                send_key("minus")
            elif character == " ":
                send_key("spc")
            else:
                send_key(character)
        time.sleep(0.3)
        send_key("ret")
        time.sleep(0.1)

    def type_until(marker, line, timeout, sensitive=False):
        """
        Type one answer and wait for the console's next marker.

        The retry exists for a partially delivered line.  It is not used for a
        line that carries the account secret: when the guest is starved (the CI
        QEMU suite runs several proofs at once) the marker can arrive after the
        deadline, and retyping the secret then lands it on a plain command
        prompt, which echoes it into the serial log.  A sensitive answer is
        typed once: a missed marker fails the proof (as a marker timeout, which
        the suite retries) instead of leaking the secret.
        """
        baseline = serial_text().count(marker)
        for attempt in range(2):
            type_line(line)
            try:
                return wait_for_new(marker, timeout, baseline)
            except SystemExit:
                if attempt == 1 or sensitive:
                    raise
                send_key("ret")
                time.sleep(0.5)
                baseline = serial_text().count(marker)
        raise SystemExit("unreachable")

    receive_prompt()
    type_until("Current password:", "credentials", 120.0)
    type_until("New login:", secret, 120.0, sensitive=True)
    type_until("New password:", "admin", 120.0)
    type_until("Repeat new password:", secret, 120.0, sensitive=True)
    type_until("[ADMIN-ENROLLMENT] credentials changed", secret, 120.0,
               sensitive=True)
print("forward: the first-run account is set")
ENROLLEOF

# boot_installed LABEL DISK
#
# Boot the installed disk on the user-mode network with the declared listen
# port forwarded, set the account on the local console (the unattended install
# leaves it unset, and the management plane withholds its listener until it
# exists), and wait for the relay's own LISTENING record.
boot_installed() {
    local label=$1
    local disk=$2
    local serial_log=$TEST_DIR/$label-boot.log
    local monitor_socket=$TEST_DIR/$label-boot.monitor
    local vars=$TEST_DIR/$label-boot-vars.fd
    local netdev=$NETDEV_DEVICE
    local start_options=()

    if [[ $label == forward && ${HFOC_FORWARD_TEST_DELAYED_LINK:-1} == 1 ]]; then
        netdev='e1000,netdev=net0,mac=52:54:00:12:34:56'
        start_options=(-S)
    fi

    cp "$OVMF_VARS" "$vars"
    : >"$serial_log"
    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m "$QEMU_MEMORY" -smp "$QEMU_SMP" \
        -cpu "${QEMU_CPU:-max}" \
        -display none \
        -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
        -drive "if=pflash,format=raw,file=$vars" \
        -drive "if=none,id=installed,file=$disk,format=raw,cache=writeback,file.locking=off" \
        -device "ide-hd,drive=installed,bus=ide.0" \
        -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$CLIENT_PORT-:$LISTEN_PORT" \
        -device "$netdev" "${start_options[@]}" \
        -serial "file:$serial_log" \
        -monitor "unix:$monitor_socket,server,nowait" \
        -no-reboot -no-shutdown >"$QEMU_LOG" 2>&1 &
    QEMU_PID=$!
    if (( ${#start_options[@]} != 0 )); then
        monitor_command "$monitor_socket" 'set_link net0 off' cont
        if ! wait_marker "$serial_log" '[PRODUCTION-FORWARD] state=DECLARED' 90 ||
                ! grep -Eq '\[PRODUCTION-NET\].*backend=E1000 .*link=DOWN.*netif_live=0' \
                    "$(serial_view "$serial_log")"; then
            listener_records "$serial_log"
            fail "$label: the relay declaration was not read with the link down"
        fi
        if grep -Fq '[PRODUCTION-FORWARD] state=LISTENING' "$(serial_view "$serial_log")"; then
            fail "$label: relay listened before the link was restored"
        fi
        monitor_command "$monitor_socket" 'set_link net0 on'
        echo "$label: restored the link after the boot read the declaration"
    fi
    # `-cpu max` advertises RDRAND/RDSEED: the console path refuses a change
    # with NO_RANDOM without a hardware entropy owner, exactly as the serving
    # proof documents.
    if ! python3 "$TEST_DIR/forward-enroll.py" "$monitor_socket" "$serial_log" \
            "$ADMIN_SECRET"; then
        fail "$label: the first-run account conversation failed"
    fi
    if grep -Fq "$ADMIN_SECRET" "$(serial_view "$serial_log")"; then
        fail "$label: the hidden console input leaked the password"
    fi
    if ! wait_marker "$serial_log" \
        "[PRODUCTION-FORWARD] state=LISTENING target=$FORWARD_TARGET:$FORWARD_TARGET_PORT listen=$FORWARD_LISTEN_PORT clients=$EXPECTED_CLIENTS" 240; then
        listener_records "$serial_log"
        fail "$label: the relay never listened"
    fi
    SERIAL_LOG=$serial_log
    MONITOR_SOCKET=$monitor_socket
    echo "$label: account set, relay listening"
}

listener_records() {
    grep -E '\[(ACME-HTTP01|PRODUCTION-NET|PRODUCTION-SERVING|PRODUCTION-SSH|PRODUCTION-FORWARD)\]' \
        "$(serial_view "$1")" | tail -n 24 >&2 || true
}

# ------------------------------------------------------------- allowed flow ---
install_medium forward "$FORWARD_IMAGE" "$TEST_DIR/forward.raw"
EXPECTED_CLIENTS=$FORWARD_CLIENT_COUNT
boot_installed forward "$TEST_DIR/forward.raw"
forward_log=$SERIAL_LOG

for marker in '[PRODUCTION-SSH] state=LISTENING' \
        '[ACME-HTTP01] v=1 state=LISTENING' \
        '[PRODUCTION-SERVING] v=1 state=LISTENING'; do
    if ! wait_marker "$forward_log" "$marker" 60; then
        listener_records "$forward_log"
        fail "forward: missing concurrent listener: $marker"
    fi
done
echo "forward: SSH, ACME HTTP-01, panel and relay are listening"

openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
    -subj /CN=forward.test -addext 'subjectAltName=DNS:forward.test' -days 1 \
    -keyout "$TEST_DIR/target.key" -out "$TEST_DIR/target.crt"
if ! python3 - "$TARGET_PORT" "$CLIENT_PORT" "$TEST_DIR/target.crt" "$TEST_DIR/target.key" <<'PY'
from http.client import HTTPResponse
import socket
import ssl
import sys
import threading

target_port = int(sys.argv[1])
client_port = int(sys.argv[2])
certificate, private_key = sys.argv[3:]
payload = bytes(range(256)) * 24
challenge = (
    b"POST /HFOC-FORWARD-CLIENT-HELLO HTTP/1.1\r\n"
    b"Host: forward.test\r\nConnection: close\r\n"
    b"Content-Length: %d\r\n\r\n" % len(payload)
) + payload
answer = b"HFOC-FORWARD-TARGET-ANSWER\n" + payload
server_tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
server_tls.load_cert_chain(certificate, private_key)
client_tls = ssl.create_default_context(cafile=certificate)
received = []
accepted = []
ready = threading.Event()


def serve():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(("127.0.0.1", target_port))
        server.listen(1)
        server.settimeout(90)
        ready.set()
        connection, _ = server.accept()
        accepted.append(True)
        connection.settimeout(90)
        with connection, server_tls.wrap_socket(connection, server_side=True) as tls:
            data = b""
            while len(data) < len(challenge):
                chunk = tls.recv(4096)
                if not chunk:
                    break
                data += chunk
            received.append(data)
            tls.sendall(
                b"HTTP/1.1 200 OK\r\nConnection: close\r\n"
                b"Content-Length: %d\r\n\r\n" % len(answer) + answer
            )


server_thread = threading.Thread(target=serve)
server_thread.start()
# The guest dials the target as soon as it accepts the client, so the target
# must be listening before the client connects.  A fixed sleep was enough on an
# idle host and lost the race on a loaded CI runner (the guest got a refused
# dial and closed the client with no bytes).
if not ready.wait(timeout=30):
    raise SystemExit("the target server did not start listening")
with socket.create_connection(("127.0.0.1", client_port), timeout=90) as connection, \
        client_tls.wrap_socket(connection, server_hostname="forward.test") as client:
    client.sendall(challenge)
    with HTTPResponse(client) as response:
        response.begin()
        if response.status != 200:
            raise SystemExit(f"the target returned HTTP {response.status}")
        reply = response.read()
server_thread.join(timeout=10)
if reply != answer:
    raise SystemExit(f"the client response differed ({len(reply)}/{len(answer)} bytes)")
if not accepted:
    raise SystemExit("the target server was never dialled")
if received != [challenge]:
    raise SystemExit("the target request differed from the expected bytes")
print("forward relay: verified HTTPS round trip to the declared target")
PY
then
    listener_records "$forward_log"
    fail "forward: the HTTPS exchange failed"
fi
if ! wait_marker "$forward_log" \
    "[PRODUCTION-FORWARD] state=SESSION verdict=OK" 60; then
    fail "forward: the relay published no completed session"
fi
if ! grep -Fq "bytes_to_target=" "$forward_log"; then
    fail "forward: the session record carries no byte counters"
fi
python3 - "$forward_log" <<'PY' || fail "forward: the session counters are zero"
import pathlib
import re
import sys

text = pathlib.Path(sys.argv[1]).read_text(errors="replace")
matches = re.findall(
    r"\[PRODUCTION-FORWARD\] state=SESSION verdict=OK .*?"
    r"bytes_to_target=(\d+) bytes_to_client=(\d+)", text)
if not matches:
    raise SystemExit("no completed session record with counters")
to_target, to_client = (int(value) for value in matches[-1])
if to_target <= 0 or to_client <= 0:
    raise SystemExit(f"counters are zero: {to_target}/{to_client}")
print(f"forward relay: session bytes {to_target} to target, {to_client} to client")
PY
monitor_quit "$MONITOR_SOCKET"
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=

# -------------------------------------------------------------- denied flow ---
install_medium forward-denied "$FORWARD_DENIED_IMAGE" "$TEST_DIR/forward-denied.raw"
EXPECTED_CLIENTS=$FORWARD_DENIED_CLIENT_COUNT
boot_installed forward-denied "$TEST_DIR/forward-denied.raw"
denied_log=$SERIAL_LOG

python3 - "$TARGET_PORT" "$CLIENT_PORT" <<'PY' || fail "forward-denied: the refusal exchange failed"
import socket
import sys

target_port = int(sys.argv[1])
client_port = int(sys.argv[2])

# Keep the target available before, during, and after the refusal. A bad
# bind or another host failure must fail the proof.
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as target:
    target.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    target.bind(("127.0.0.1", target_port))
    target.listen(1)
    with socket.create_connection(("127.0.0.1", client_port), timeout=30) as client:
        client.settimeout(10)
        try:
            client.sendall(b"HFOC-FORWARD-DENIED\n")
            if client.recv(1):
                raise SystemExit("the refused client received upstream data")
        except (BrokenPipeError, ConnectionResetError):
            pass
    target.settimeout(8)
    try:
        connection, _ = target.accept()
    except TimeoutError:
        pass
    else:
        connection.close()
        raise SystemExit("the target was dialled for a refused source")
print("forward denied: the connection was refused before the target")
PY

grep -Fq "[PRODUCTION-FORWARD] state=REFUSED reason=SOURCE_NOT_ALLOWED src=$FORWARD_TARGET" \
    "$denied_log" || \
    fail "forward-denied: the relay published no source refusal"
if grep -Fq "state=SESSION" "$denied_log"; then
    fail "forward-denied: a refused source still produced a session"
fi
monitor_quit "$MONITOR_SOCKET"
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=

echo "Forward relay QEMU proof: OK"
