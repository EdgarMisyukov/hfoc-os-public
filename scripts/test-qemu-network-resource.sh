#!/usr/bin/env bash
# Prove item 250 in one q35 QEMU boot: four live connections, fifth pool
# denial, backlog/packet pressure, timeout, reset, drain/restart, timer and
# console liveness, exact ownership balance, and clean HMP shutdown.
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

if [[ "${HFOC_NETWORK_RESOURCE_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_NETWORK_RESOURCE_TEST_TIMEOUT:-240}s" \
        env HFOC_NETWORK_RESOURCE_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Network resource test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "Network resource test must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "Network resource test must not set QEMU_CPU" >&2
    exit 1
}
echo "QEMU network resource baseline: machine=q35 smp=1 cpu=default"

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
    "host_port=$NET_PORT" \
    "guest_port=443" >"$METADATA"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/network-resource-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-Network resource test failed}" \
        >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$INTERRUPTS_LOG" \
        "$TRACE_LOG" "$TEST_DIR/trace-events.txt" \
        "$METADATA" "$FAILURE_REASON" "$artifact_dir/"
    echo "Network resource artifacts: $artifact_dir" >&2
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
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:"$NET_PORT"-:443 \
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
import errno
import select
import socket
import struct
import sys
import time

serial_path = pathlib.Path(sys.argv[1])
monitor_path = pathlib.Path(sys.argv[2])
host_port = int(sys.argv[3])
qemu_pid = int(sys.argv[4])
payload = b"HFOC-NETRES-PAYLOAD"
keepalive = b"HFOC-NETRES-KEEP"
response = b"HFOC-NETRES-ECHO:" + payload
keepalive_response = b"HFOC-NETRES-ECHO:" + keepalive
timer_marker = "[LAPIC-TIMER] irq vector=0x42"
command_marker = "UNKNOWN COMMAND: vnfc. TYPE HELP."


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


def connect_backlog_burst():
    connected = []
    pending = []
    deadline = time.monotonic() + 10.0

    def fail(message):
        for sock in connected + pending:
            sock.close()
        raise SystemExit(message)

    for _ in range(4):
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setblocking(False)
        try:
            result = sock.connect_ex(("127.0.0.1", host_port))
        except OSError as exc:
            sock.close()
            fail(f"backlog burst connect failed: {exc}")
        if result == 0:
            sock.setblocking(True)
            connected.append(sock)
            continue
        if result in (errno.EINPROGRESS, errno.EWOULDBLOCK,
                      errno.EALREADY):
            pending.append(sock)
            continue
        if result == errno.EISCONN:
            sock.setblocking(True)
            connected.append(sock)
            continue
        sock.close()
        fail(f"backlog burst connect failed with error {result}")

    while pending:
        remaining = deadline - time.monotonic()
        if remaining <= 0.0:
            fail("timed out waiting for network resource backlog burst")
        try:
            _, writable, _ = select.select([], pending, [], remaining)
        except OSError as exc:
            fail(f"backlog burst select failed: {exc}")
        for sock in writable:
            pending.remove(sock)
            connect_error = sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
            if connect_error != 0:
                sock.close()
                fail(
                    "backlog burst connect failed with error "
                    f"{connect_error}")
            sock.setblocking(True)
            connected.append(sock)

    return connected


def marker_timeout(marker, timeout):
    """ITEM-412: the proof stopped without seeing a marker inside its window.

    The record is what makes this a timing class rather than an assertion:
    scripts/run-qemu-suite.sh reads it and retries the target once, visibly
    (docs/ci-self-hosted-runner.md). A `[NET-RESOURCE] failed` record, a panic
    or a wrong response is never reported this way.
    """
    sys.stderr.write(
        f'HFOC-QEMU-MARKER-TIMEOUT marker="{marker}" deadline={timeout:g}s\n')
    sys.stderr.flush()
    raise SystemExit(f"timed out waiting for {marker}")


def wait_serial(marker, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = serial_text()
        if marker in text:
            return
        if "[NET-RESOURCE] failed" in text:
            raise SystemExit("Network resource proof reported failure")
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit("Kernel panic during network resource proof")
        if not qemu_running():
            raise SystemExit("QEMU exited before validation completed")
        time.sleep(0.05)
    marker_timeout(marker, timeout)


def recv_exact(sock, length, phase):
    chunks = []
    remaining = length
    while remaining:
        chunk = sock.recv(remaining)
        if not chunk:
            raise SystemExit(
                f"{phase}: TCP peer closed before exact response: "
                f"received {b''.join(chunks)!r}")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def exchange(sock, phase, request=payload, expected=response):
    sock.sendall(request)
    received = recv_exact(sock, len(expected), phase)
    if received != expected:
        raise SystemExit(
            f"unexpected network resource response: {received!r} "
            f"expected {expected!r}")


def connect_echo():
    with socket.create_connection(("127.0.0.1", host_port), timeout=10) as sock:
        exchange(sock, "initial echo")
    return None


def reset_connection(sock):
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                    struct.pack("ii", 1, 0))
    sock.close()


def fifth_rejected():
    try:
        sock = socket.create_connection(("127.0.0.1", host_port), timeout=2)
    except OSError:
        return True
    try:
        sock.sendall(payload)
        received = b""
        while len(received) < len(response):
            chunk = sock.recv(len(response) - len(received))
            if not chunk:
                return True
            received += chunk
    except OSError:
        return True
    finally:
        sock.close()
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


wait_serial("[NET-RESOURCE] listener live", 60)

sockets = connect_backlog_burst()

wait_serial("[NET-RESOURCE] backlog pressure", 30)
wait_serial("[NET-RESOURCE] backlog ok", 30)
for index, sock in enumerate(sockets):
    exchange(sock, f"initial pool echo slot {index}")
wait_serial("[NET-RESOURCE] pool full", 30)
wait_serial("[NET-RESOURCE] packet pressure ok", 30)

if not fifth_rejected():
    raise SystemExit("fifth connection unexpectedly obtained a live slot")

reset_connection(sockets[0])
sockets.pop(0)
wait_serial("[NET-RESOURCE] reset ok", 30)

keepalive_window = 45.0
keepalive_deadline = time.monotonic() + keepalive_window
while "[NET-RESOURCE] timeout ok" not in serial_text():
    if time.monotonic() >= keepalive_deadline:
        marker_timeout("[NET-RESOURCE] timeout ok", keepalive_window)
    if "[NET-RESOURCE] failed" in serial_text():
        raise SystemExit("Network resource proof reported failure")
    if "*** HFOC KERNEL PANIC ***" in serial_text():
        raise SystemExit("Kernel panic during timeout evidence")
    if not qemu_running():
        raise SystemExit("QEMU exited before timeout evidence")
    for sock in sockets[:-1]:
        keepalive_closed = False
        try:
            sock.sendall(keepalive)
            received = recv_exact(sock, len(keepalive_response),
                                  "pre-restart keepalive")
            if received != keepalive_response:
                raise SystemExit(
                    f"unexpected keepalive response: {received!r}")
        except (BrokenPipeError, ConnectionResetError) as exc:
            sys.stderr.write(f"pre-restart keepalive: {exc!r}\n")
            keepalive_closed = True
        except SystemExit as exc:
            if "TCP peer closed before exact response" not in str(exc):
                raise
            sys.stderr.write(f"{exc}\n")
            keepalive_closed = True
        if not keepalive_closed:
            continue
        # ITEM-412: the guest closes an idle connection when its own timeout
        # fires, and a starved guest can win that race against the probe even
        # though it is doing exactly what the proof waits for (measured
        # 2026-09-14, run 34861611960: the guest went on to print
        # "[NET-RESOURCE] timeout ok" and "[NET-RESOURCE] restart ok" after the
        # proof had stopped here). The wait is what failed, so report the wait
        # with the marker it was awaiting; a wrong echo above still fails as
        # the assertion it is.
        if "[NET-RESOURCE] timeout ok" in serial_text():
            break
        marker_timeout("[NET-RESOURCE] timeout ok", keepalive_window)
    time.sleep(0.05)

wait_serial("[NET-RESOURCE] restart ok", 30)
for sock in sockets:
    sock.close()

# The restarted listener must echo again inside a bounded latency budget: a
# listener that only answers after minutes is a regression even though it
# eventually answers. A single accepted connection can still be slow under CI
# load, so the probe retries short socket attempts inside a diagnostic tail
# (budget + 60s) and then reports the measured latency; the successful echo
# itself must land inside the budget.
try:
    restart_echo_budget = float(
        os.environ.get("HFOC_NET_RESOURCE_RESTART_ECHO_BUDGET", "30"))
except ValueError:
    raise SystemExit("HFOC_NET_RESOURCE_RESTART_ECHO_BUDGET must be a number")
if restart_echo_budget <= 0:
    raise SystemExit("HFOC_NET_RESOURCE_RESTART_ECHO_BUDGET must be positive")

restart_echo_diagnostic_tail = 60.0
restart_echo_deadline = (
    time.monotonic() + restart_echo_budget + restart_echo_diagnostic_tail)
restart_echo_started = time.monotonic()
restart_echo_attempts = 0
while True:
    try:
        sock = socket.create_connection(
            ("127.0.0.1", host_port), timeout=10)
    except OSError:
        restart_echo_attempts += 1
        if time.monotonic() >= restart_echo_deadline:
            raise SystemExit(
                "post-restart connection did not become available within "
                f"{restart_echo_budget + restart_echo_diagnostic_tail:.0f}s")
        time.sleep(0.05)
        continue
    try:
        exchange(sock, "post-restart echo")
    except (SystemExit, OSError) as exc:
        # Retry only a slow/closed first connection; a wrong response still
        # fails immediately.
        restart_echo_attempts += 1
        sock.close()
        if time.monotonic() < restart_echo_deadline and (
                not isinstance(exc, SystemExit) or
                "TCP peer closed before exact response" in str(exc)):
            time.sleep(0.05)
            continue
        raise
    sock.close()
    break

restart_echo_latency = time.monotonic() - restart_echo_started
latency_line = (
    f"network-resource post-restart echo latency "
    f"{restart_echo_latency:.2f}s retries={restart_echo_attempts} "
    f"budget={restart_echo_budget:.0f}s")
print("Network resource post-restart echo latency: "
      f"{restart_echo_latency:.2f}s after {restart_echo_attempts} retry(ies), "
      f"budget {restart_echo_budget:.0f}s")
try:
    latency_dir = pathlib.Path("build/test-artifacts")
    latency_dir.mkdir(parents=True, exist_ok=True)
    (latency_dir / "network-resource-restart-echo-latency.txt").write_text(
        latency_line + "\n")
except OSError as exc:
    print(f"Network resource latency summary not written: {exc}")
if restart_echo_latency > restart_echo_budget:
    raise SystemExit(
        f"post-restart echo took {restart_echo_latency:.2f}s, over the "
        f"{restart_echo_budget:.0f}s latency budget")

wait_serial("[NET-RESOURCE] restart live", 30)
wait_serial("[NET-RESOURCE] restarted echo ok", 30)
wait_serial("[NET-RESOURCE] proof OK", 60)

text = serial_text()
required = [
    "[NET-RESOURCE] proof init",
    "[NET-RESOURCE] listener live",
    "[NET-RESOURCE] backlog pressure",
    "[NET-RESOURCE] backlog ok",
    "[NET-RESOURCE] pool full",
    "[NET-RESOURCE] packet pressure ok",
    "[NET-RESOURCE] reset ok",
    "[NET-RESOURCE] timeout ok",
    "[NET-RESOURCE] restart ok",
    "[NET-RESOURCE] restarted echo ok",
    "[NET-RESOURCE] balanced pool",
    "[NET-RESOURCE] clean shutdown",
    "[NET-RESOURCE] proof OK",
]
for marker in required:
    if text.count(marker) != 1:
        raise SystemExit(
            f"Expected 1 marker, got {text.count(marker)}: {marker}")
if "[NET-RESOURCE] failed" in text:
    raise SystemExit("Network resource proof reported failure")
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
        raise SystemExit("Kernel panic during console liveness")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="console responsiveness" '
        'deadline=' + str(int(command_deadline)) + 's\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for console responsiveness")

print("Network resource proof init: found")
print("Host-forwarded listener live: found")
print("Four live connections: found")
print("Backlog pressure and drain: found")
print("Fifth pool denial: found")
print("Packet pressure evidence: found")
print("Reset isolation: found")
print("Deadline timeout: found")
print("Listener drain and restart: found")
print("Post-restart echo: found")
print("Balanced ownership: found")
print("Clean shutdown: found")
print("Proof OK: found")
print("Timer liveness: found")
print("Console liveness: found")
print("Kernel panic: none")
print("QEMU alive before quit: yes")
PY
); then
    FAILURE_MESSAGE=$validation_output
    printf '%s\n' "$validation_output" >&2
    exit 1
fi

printf '%s\n' "$validation_output"
if ! kill -0 "$QEMU_PID" 2>/dev/null; then
    FAILURE_MESSAGE="QEMU exited before validation completed"
    printf '%s\n' "$FAILURE_MESSAGE" >&2
    exit 1
fi

if successful_shutdown; then
    QEMU_PID=
else
    printf '%s\n' "$FAILURE_MESSAGE" >&2
    exit 1
fi

echo "QEMU network resource test: OK"
