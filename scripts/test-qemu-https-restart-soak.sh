#!/usr/bin/env bash
# Prove item 299 in one q35 QEMU boot: repeated TLS connect/request/close,
# malformed traffic, TCP reset, three HTTPS lifecycle restarts, QEMU user-net
# link toggle recovery, final success, exact counts, no stale callback,
# timer/console liveness, and clean HMP shutdown.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in python3 qemu-system-x86_64 timeout mktemp openssl; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

if [[ "${HFOC_HTTPS_RESTART_SOAK_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_HTTPS_RESTART_SOAK_TEST_TIMEOUT:-600}s" \
        env HFOC_HTTPS_RESTART_SOAK_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "HTTPS restart/soak QEMU test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "HTTPS restart/soak QEMU test must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "HTTPS restart/soak QEMU test must not set QEMU_CPU" >&2
    exit 1
}
echo "QEMU HTTPS restart/soak baseline: machine=q35 smp=1 cpu=default"

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
CERT_DER=$TEST_DIR/hfoc-test-cert.der
CERT_PEM=$TEST_DIR/hfoc-test-cert.pem
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

python3 - "$CERT_DER" <<'PY'
import re
import sys
from pathlib import Path

source = Path("kernel/src/hfoc_mbedtls_test_secret.c").read_text(
    encoding="ascii")
start = source.index("hfoc_mbedtls_test_secret_cert_der")
body = source[start:source.index("};", start)]
values = [int(value, 16) for value in re.findall(r"0x[0-9a-fA-F]{2}", body)]
if not values:
    raise SystemExit("could not extract HTTPS test certificate DER")
Path(sys.argv[1]).write_bytes(bytes(values))
PY
openssl x509 -inform DER -in "$CERT_DER" -out "$CERT_PEM"
openssl x509 -in "$CERT_PEM" -noout -subject >/dev/null

printf '%s\n' \
    "machine=$QEMU_MACHINE" \
    "cpu=default" \
    "smp=$QEMU_SMP" \
    "image=$IMAGE" \
    "host_port=$NET_PORT" \
    "guest_port=443" \
    "certificate_pem=$CERT_PEM" >"$METADATA"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/https-restart-soak-qemu-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-HTTPS restart/soak QEMU test failed}" \
        >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$INTERRUPTS_LOG" \
        "$TRACE_LOG" "$TEST_DIR/trace-events.txt" \
        "$METADATA" "$FAILURE_REASON" "$CERT_DER" "$CERT_PEM" \
        "$artifact_dir/"
    echo "HTTPS restart/soak QEMU artifacts: $artifact_dir" >&2
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
    -no-reboot -no-shutdown \
    -trace "events=$TEST_DIR/trace-events.txt,file=$TRACE_LOG" \
    >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

validation_output=
if ! validation_output=$(python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" \
    "$NET_PORT" "$QEMU_PID" "$CERT_PEM" 2>&1 <<'PY'
import os
import pathlib
import socket
import ssl
import struct
import sys
import time

# ITEM-498: the refusal body names its status and its reason.  A browser
# target gets the HTML document; a refusal with no parsed target gets JSON.
_REFUSAL_REASONS = {
    400: "bad_request",
    401: "unauthorized",
    403: "forbidden",
    404: "not_found",
    405: "method_not_allowed",
    413: "payload_too_large",
    414: "uri_too_long",
    429: "too_many_requests",
    431: "header_fields_too_large",
    503: "unavailable",
}


def refusal_json(status, reason):
    return f'{{"status":{status},"error":"{reason}"}}'.encode("ascii")


def refusal_html(phrase):
    status = int(phrase.split(" ", 1)[0])
    reason = _REFUSAL_REASONS[status]
    return (
        b'<!doctype html>\n'
        b'<html lang="en"><head><meta charset="utf-8">'
        b'<title>HFOC Appliance management - ' +
        phrase.encode("ascii") +
        b'</title></head>\n'
        b'<body><h1>' + phrase.encode("ascii") +
        b'</h1><p>The appliance refused this request: ' +
        reason.encode("ascii") +
        b'.</p><p><a href="/panel">Management sign-in</a></p>'
        b'</body></html>\n'
    )


serial_path = pathlib.Path(sys.argv[1])
monitor_path = pathlib.Path(sys.argv[2])
host_port = int(sys.argv[3])
qemu_pid = int(sys.argv[4])
certificate_pem = sys.argv[5]
proof_init_marker = "[HTTPS-RESTART-SOAK] proof init"
listener_marker = "[HTTPS-RESTART-SOAK] listener live"
round_requests_markers = [
    "[HTTPS-RESTART-SOAK] round 1 requests ok",
    "[HTTPS-RESTART-SOAK] round 2 requests ok",
    "[HTTPS-RESTART-SOAK] round 3 requests ok",
]
malformed_marker = "[HTTPS-RESTART-SOAK] malformed ok"
reset_marker = "[HTTPS-RESTART-SOAK] reset ok"
restart_markers = [
    "[HTTPS-RESTART-SOAK] restart 1 ok",
    "[HTTPS-RESTART-SOAK] restart 2 ok",
    "[HTTPS-RESTART-SOAK] restart 3 ok",
]
link_recovery_marker = "[HTTPS-RESTART-SOAK] link recovery ok"
final_success_marker = "[HTTPS-RESTART-SOAK] final success"
no_stale_marker = "[HTTPS-RESTART-SOAK] no stale callback"
balanced_counts_marker = "[HTTPS-RESTART-SOAK] balanced counts"
clean_shutdown_marker = "[HTTPS-RESTART-SOAK] clean shutdown"
proof_ok_marker = "[HTTPS-RESTART-SOAK] proof OK"
timer_marker = "[LAPIC-TIMER] irq vector=0x42"
console_marker = "UNKNOWN COMMAND: vnfc. TYPE HELP."
root_body = (
    b'<!doctype html>\n'
    b'<html lang="en">\n'
    b'<head>\n'
    b'<meta charset="utf-8">\n'
    b'<meta name="viewport" content="width=device-width,initial-scale=1">\n'
    b'<title>HFOC Appliance</title>\n'
    b'<style>body{font-family:system-ui,sans-serif;margin:2rem;max-width:60rem;line-height:1.45}nav a{margin-right:.6rem}code{background:#eee;padding:0 .2rem}.state{font-weight:700}.notice{color:#a00;font-weight:700}footer{margin-top:2rem;color:#555;font-size:.9rem}</style>\n'
    b'</head>\n'
    b'<body>\n'
    b'<h1>HFOC Appliance</h1>\n'
    b'<nav>Dashboard Network Services Storage System Security Diagnostics Updates Recovery Routing Account Certificates Help</nav>\n'
    b'<main>\n'
    b'<h2>Management sign-in</h2>\n'
    b'<p class="state">SIGNED OUT</p>\n'
    b'<form method="post" action="/">\n'
    b'<p><label>Login <input type="text" name="username" autocomplete="username"></label></p>\n'
    b'<p><label>Password <input type="password" name="password" autocomplete="current-password"></label></p>\n'
    b'<p><button type="submit">Sign in</button></p>\n'
    b'</form>\n'
    b'<p>Set the account on the machine console with <code>credentials</code>, then sign in here with the same login and password.</p>\n'
    b'<p>The panel requires an administrator session. Sign in above with the login and password set on the local console, or present the issued token in the <code>Authorization: Bearer</code> request header. A sign-in answers one <code>HttpOnly</code>, <code>Secure</code>, <code>SameSite=Strict</code> session cookie with a short lifetime. The panel loads no browser script and no external asset.</p>\n'
    b'<p>Session: none</p>\n'
    b'<p>Management API discovery: <code>/api/v1</code></p>\n'
    b'</main>\n'
    b'<footer>HFOC Appliance management panel</footer>\n'
    b'</body>\n'
    b'</html>\n'
)
health_body = b'{"status":"ok","route":"health"}'
root_head = (
    b"HTTP/1.1 200 OK\r\n"
    b"Content-Type: text/html; charset=utf-8\r\n"
    b"Content-Length: 1611\r\n"
    b"Connection: close\r\n"
    b"Cache-Control: no-store\r\n"

    b"Strict-Transport-Security: max-age=31536000\r\n"

    b"Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'\r\n"

    b"Referrer-Policy: no-referrer\r\n"
    b"X-Content-Type-Options: nosniff\r\n"
    b"X-Frame-Options: DENY")
health_head = (
    b"HTTP/1.1 200 OK\r\n"
    b"Content-Type: application/json\r\n"
    b"Content-Length: 32\r\n"
    b"Connection: close\r\n"
    b"Cache-Control: no-store\r\n"

    b"Strict-Transport-Security: max-age=31536000\r\n"

    b"Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'\r\n"

    b"Referrer-Policy: no-referrer\r\n"
    b"X-Content-Type-Options: nosniff\r\n"
    b"X-Frame-Options: DENY")
route_requests = [
    ("GET", "/"),
    ("HEAD", "/"),
    ("GET", "/health"),
    ("HEAD", "/health"),
    ("GET", "/"),
    ("HEAD", "/"),
    ("GET", "/health"),
    ("HEAD", "/health"),
]


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


def marker_timeout(marker, timeout):
    """ITEM-412: the proof stopped without seeing a marker inside its window.

    The record is what makes this a timing class rather than an assertion:
    scripts/run-qemu-suite.sh reads it and retries the target once, visibly
    (docs/ci-self-hosted-runner.md). A `[HTTPS-RESTART-SOAK] failed` record or
    a panic is never reported this way.
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
        if "[HTTPS-RESTART-SOAK] failed" in text:
            raise SystemExit("HTTPS restart/soak proof reported failure")
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit("Kernel panic during HTTPS restart/soak")
        if not qemu_running():
            raise SystemExit("QEMU exited before validation completed")
        time.sleep(0.05)
    marker_timeout(marker, timeout)


def make_tls_context():
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    context.maximum_version = ssl.TLSVersion.TLSv1_3
    context.check_hostname = True
    context.verify_mode = ssl.CERT_REQUIRED
    context.load_verify_locations(cafile=certificate_pem)
    return context


def open_tls(timeout=20):
    context = make_tls_context()
    raw = socket.create_connection(
        ("127.0.0.1", host_port), timeout=timeout)
    tls = context.wrap_socket(
        raw, server_hostname="hfoc.test", do_handshake_on_connect=False)
    try:
        tls.do_handshake()
    except Exception:
        try:
            tls.setsockopt(
                socket.SOL_SOCKET,
                socket.SO_LINGER,
                struct.pack("ii", 1, 0))
        except OSError:
            pass
        try:
            tls.close()
        except OSError:
            pass
        raise
    return tls


def read_tls(tls, timeout=30):
    tls.settimeout(timeout)
    data = b""
    while True:
        try:
            chunk = tls.recv(4096)
        except (ssl.SSLEOFError, BrokenPipeError, ConnectionResetError):
            break
        except TimeoutError as exc:
            raise TimeoutError(
                f"TLS read timed out after {timeout}s") from exc
        if not chunk:
            break
        data += chunk
    return data


def request(method, target, timeout=30):
    tls = open_tls(timeout=timeout)
    try:
        tls.sendall(
            f"{method} {target} HTTP/1.1\r\n"
            "Host: hfoc.test\r\n"
            "Connection: close\r\n\r\n".encode("ascii"))
        return read_tls(tls, timeout=timeout)
    finally:
        tls.close()


def assert_response(method, target, expected_head, expected_body):
    last_error = None
    for attempt in range(1, 4):
        try:
            data = request(method, target)
        except TimeoutError as exc:
            last_error = exc
            print(
                f"{method} {target}: TLS timeout on attempt {attempt}; "
                "retrying")
            time.sleep(1.0)
            continue
        if b"\r\n\r\n" not in data:
            raise SystemExit(
                f"{method} {target}: response missing header terminator")
        head, body = data.split(b"\r\n\r\n", 1)
        if head != expected_head:
            raise SystemExit(
                f"{method} {target}: headers do not match\n"
                f"expected={expected_head!r}\nactual={head!r}")
        if body != expected_body:
            raise SystemExit(
                f"{method} {target}: body does not match\n"
                f"expected={expected_body!r}\nactual={body!r}")
        return
    raise SystemExit(
        f"{method} {target}: TLS timeout after 3 attempts: {last_error}")


def malformed_request(timeout=30):
    tls = open_tls(timeout=timeout)
    try:
        tls.sendall(
            b"GET /\x01 HTTP/1.1\r\n"
            b"Host: hfoc.test\r\n"
            b"Connection: close\r\n\r\n")
        data = read_tls(tls, timeout=timeout)
    finally:
        tls.close()
    if not data.startswith(b"HTTP/1.1 400 "):
        raise SystemExit(
            f"malformed request: unexpected response {data!r}")
    if b"\r\n\r\n" not in data:
        raise SystemExit("malformed request: response missing terminator")
    head, body = data.split(b"\r\n\r\n", 1)
    expected_body = refusal_json(400, "bad_request")
    if body != expected_body:
        raise SystemExit(
            f"malformed request: body {body!r}, expected {expected_body!r}")


def reset_tls():
    context = make_tls_context()
    raw = socket.create_connection(
        ("127.0.0.1", host_port), timeout=20)
    try:
        tls = context.wrap_socket(
            raw,
            server_hostname="hfoc.test",
            do_handshake_on_connect=False)
        tls.do_handshake()
        tls.setsockopt(
            socket.SOL_SOCKET,
            socket.SO_LINGER,
            struct.pack("ii", 1, 0))
        tls.close()
    except Exception:
        try:
            raw.close()
        except OSError:
            pass
        raise


def wait_prompt(monitor):
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


def hmp_command(command):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
        monitor.settimeout(1.0)
        try:
            monitor.connect(str(monitor_path))
        except OSError as exc:
            raise SystemExit(
                f"QEMU monitor did not accept command: {exc}")
        wait_prompt(monitor)
        monitor.sendall(f"{command}\n".encode("ascii"))
        wait_prompt(monitor)


wait_serial(proof_init_marker, 60)
wait_serial(listener_marker, 90)

for round_index in range(3):
    for method, target in route_requests:
        expected_head = root_head
        expected_body = root_body
        if target == "/health":
            expected_head = health_head
            expected_body = health_body
        if method == "HEAD":
            expected_body = b""
        assert_response(method, target, expected_head, expected_body)
    wait_serial(round_requests_markers[round_index], 60)
    malformed_request()
    if round_index == 0:
        wait_serial(malformed_marker, 30)
    reset_tls()
    if round_index == 0:
        wait_serial(reset_marker, 30)
    wait_serial(restart_markers[round_index], 60)

print("HTTPS restart/soak link off: issuing")
hmp_command("set_link net0 off")
print("HTTPS restart/soak link off: found")
time.sleep(1.0)
try:
    link_probe = open_tls(timeout=3)
except (OSError, TimeoutError, ssl.SSLError):
    pass
else:
    link_probe.close()
    raise SystemExit(
        "TLS connection unexpectedly succeeded while net0 off")

# The user-net forwarder can still deliver the off-link probe to the guest.
# Give the kernel time to release that rejected connection before enabling
# the link, so recovery starts from a clean allocator state.
print("HTTPS restart/soak link-off drain: waiting")
time.sleep(10.0)

print("HTTPS restart/soak link on: issuing")
hmp_command("set_link net0 on")
print("HTTPS restart/soak link on: found")

assert_response("GET", "/", root_head, root_body)
wait_serial(link_recovery_marker, 30)

for method, target in [
        ("HEAD", "/"),
        ("GET", "/health"),
        ("HEAD", "/health")]:
    expected_head = root_head if target == "/" else health_head
    expected_body = b"" if method == "HEAD" else (
        root_body if target == "/" else health_body)
    assert_response(method, target, expected_head, expected_body)

wait_serial(final_success_marker, 30)
wait_serial(no_stale_marker, 30)
wait_serial(balanced_counts_marker, 60)
wait_serial(clean_shutdown_marker, 60)
wait_serial(proof_ok_marker, 60)

text = serial_text()
required = [
    proof_init_marker,
    listener_marker,
] + round_requests_markers + [
    malformed_marker,
    reset_marker,
] + restart_markers + [
    link_recovery_marker,
    final_success_marker,
    no_stale_marker,
    balanced_counts_marker,
    clean_shutdown_marker,
    proof_ok_marker,
]
for marker in required:
    if text.count(marker) != 1:
        raise SystemExit(
            f"Expected 1 marker, got {text.count(marker)}: {marker}")
if "[HTTPS-RESTART-SOAK] failed" in text:
    raise SystemExit("HTTPS restart/soak proof reported failure")
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
        raise SystemExit(
            f"QEMU monitor did not accept console check: {exc}")
    wait_prompt(monitor)
    for key in ("v", "n", "f", "c", "ret"):
        monitor.sendall(f"sendkey {key}\n".encode("ascii"))
        wait_prompt(monitor)
        time.sleep(0.08)

console_deadline = time.monotonic() + 10.0
while time.monotonic() < console_deadline:
    if console_marker in serial_text():
        break
    if "*** HFOC KERNEL PANIC ***" in serial_text():
        raise SystemExit("Kernel panic during console liveness")
    time.sleep(0.05)
else:
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="console responsiveness" '
        'deadline=' + str(int(console_deadline)) + 's\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for console responsiveness")

print("QEMU HTTPS restart/soak proof init: found")
print("Repeated TLS connect/request/close rounds: 3")
print("Malformed traffic isolation: found")
print("TCP reset isolation: found")
print("HTTPS lifecycle restarts: 3")
print("QEMU user-net link off/on recovery: found")
print("Final success: found")
print("No stale callback: found")
print("Balanced pool/lifecycle counts: found")
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
    echo "Clean HMP shutdown: found"
else
    printf '%s\n' "$FAILURE_MESSAGE" >&2
    exit 1
fi

echo "QEMU HTTPS restart/soak test: OK"
