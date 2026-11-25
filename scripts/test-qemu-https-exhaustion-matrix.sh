#!/usr/bin/env bash
# Prove item 295 in one q35 QEMU boot: four live TLS 1.3 HTTPS connections,
# fifth denial before TLS allocation, bounded packet/TX pressure, one-slot
# release, subsequent recovery success, and balanced counts.
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

if [[ "${HFOC_HTTPS_EXHAUSTION_MATRIX_QEMU_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_HTTPS_EXHAUSTION_MATRIX_QEMU_TEST_TIMEOUT:-600}s" \
        env HFOC_HTTPS_EXHAUSTION_MATRIX_QEMU_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "HTTPS exhaustion matrix QEMU test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "HTTPS exhaustion matrix QEMU test must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "HTTPS exhaustion matrix QEMU test must not set QEMU_CPU" >&2
    exit 1
}
echo "QEMU HTTPS exhaustion baseline: machine=q35 smp=1 cpu=default"

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

if [[ -n "${HFOC_HTTPS_EXHAUSTION_MATRIX_QEMU_TEST_PORT:-}" ]]; then
    NET_PORT=$HFOC_HTTPS_EXHAUSTION_MATRIX_QEMU_TEST_PORT
    if ! [[ "$NET_PORT" =~ ^[0-9]+$ ]] ||
            (( NET_PORT <= 0 || NET_PORT > 65535 )); then
        echo "HTTPS exhaustion matrix QEMU test port out of range: $NET_PORT" >&2
        exit 1
    fi
else
    NET_PORT=$(python3 - <<'PY'
import socket
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
PY
)
fi
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
    artifact_dir="build/test-artifacts/https-exhaustion-matrix-qemu-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-HTTPS exhaustion matrix QEMU test failed}" \
        >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$INTERRUPTS_LOG" \
        "$TRACE_LOG" "$TEST_DIR/trace-events.txt" \
        "$METADATA" "$FAILURE_REASON" "$CERT_DER" "$CERT_PEM" \
        "$artifact_dir/"
    echo "HTTPS exhaustion matrix QEMU artifacts: $artifact_dir" >&2
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
            fail("QEMU HMP EOF timeout after quit")
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
if ! validation_output=$(python3 - "$SERIAL_LOG" "$NET_PORT" "$QEMU_PID" \
    "$CERT_PEM" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import os
import pathlib
import socket
import ssl
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

def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])

serial_path = pathlib.Path(sys.argv[1])
host_port = int(sys.argv[2])
qemu_pid = int(sys.argv[3])
certificate_pem = sys.argv[4]
ready_marker = "[HTTPS-EXHAUSTION-MATRIX] harness ready"
listener_marker = "[HTTPS-QEMU] listener live"
four_marker = "[HTTPS-QEMU] four held"
fifth_marker = "[HTTPS-QEMU] fifth denial ok"
malformed_marker = "[HTTPS-QEMU] malformed request ok"
peer_marker = "[HTTPS-QEMU] peer throttle ok"
malformed_drained_marker = "[HTTPS-EXHAUSTION-MATRIX] malformed drained"
pressure_marker = "[HTTPS-QEMU] packet pressure ok"
release_marker = "[HTTPS-EXHAUSTION-MATRIX] release ok"
recovery_marker = "[HTTPS-QEMU] exhaustion recovery ok"
root_get_marker = "[HTTPS-QEMU] root get ok"
root_head_marker = "[HTTPS-QEMU] root head ok"
health_get_marker = "[HTTPS-QEMU] health get ok"
health_head_marker = "[HTTPS-QEMU] health head ok"
restart_ok_marker = "[HTTPS-QEMU] restart ok"
restart_live_marker = "[HTTPS-QEMU] restart live"
proof_ready_marker = "[HTTPS-QEMU] proof ready"
balanced_pool_marker = "[HTTPS-QEMU] balanced pool"
balanced_counts_marker = "[HTTPS-EXHAUSTION-MATRIX] balanced counts"
clean_shutdown_marker = "[HTTPS-QEMU] clean shutdown"
proof_ok_marker = "[HTTPS-QEMU] proof OK"
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


def wait_serial(marker, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = serial_text()
        if marker in text:
            return
        if "[HTTPS-QEMU] failed" in text:
            raise SystemExit("HTTPS service QEMU proof reported failure")
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit("Kernel panic during HTTPS exhaustion matrix")
        if not qemu_running():
            raise SystemExit("QEMU exited before validation completed")
        time.sleep(0.05)
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="' +
        str(f"timed out waiting for {marker}") + '" '
        'deadline=' + str(int(timeout)) + 's\n')
    sys.stderr.flush()
    raise SystemExit(f"timed out waiting for {marker}")


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
    return context.wrap_socket(raw, server_hostname="hfoc.test")


def open_raw(timeout=20):
    raw = socket.create_connection(
        ("127.0.0.1", host_port), timeout=timeout)
    raw.settimeout(timeout)
    return raw


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
            print(f"{method} {target}: TLS timeout on attempt {attempt}; retrying")
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


def fifth_rejected():
    raw = socket.create_connection(("127.0.0.1", host_port), timeout=3)
    try:
        raw.settimeout(3)
        try:
            data = raw.recv(1)
        except socket.timeout:
            return False
        except OSError:
            return True
        return data == b""
    except OSError:
        return True
    finally:
        raw.close()


wait_serial(ready_marker, 30)
wait_serial(listener_marker, 90)

malformed_request()
wait_serial(malformed_marker, 20)
wait_serial(peer_marker, 20)
wait_serial(malformed_drained_marker, 20)

held = []
held.append(open_tls())
for index in range(3):
    held.append(open_raw())
wait_serial(four_marker, 30)

if not fifth_rejected():
    raise SystemExit("fifth connection unexpectedly succeeded")
wait_serial(fifth_marker, 20)
wait_serial(pressure_marker, 30)

held[0].close()
held = held[1:]
wait_serial(release_marker, 30)

recovery = open_tls()
try:
    recovery.sendall(
        b"GET / HTTP/1.1\r\n"
        b"Host: hfoc.test\r\n"
        b"Connection: close\r\n\r\n")
    data = read_tls(recovery)
finally:
    recovery.close()
if b"\r\n\r\n" not in data:
    raise SystemExit("recovery root response missing header terminator")
head, body = data.split(b"\r\n\r\n", 1)
if head != root_head or body != root_body:
    raise SystemExit(
        "recovery root response mismatch\n"
        f"expected={root_head + b'\\r\\n\\r\\n' + root_body!r}\n"
        f"actual={data!r}")
wait_serial(root_get_marker, 20)
wait_serial(recovery_marker, 20)

for sock in held:
    sock.close()
held.clear()

assert_response("HEAD", "/", root_head, b"")
wait_serial(root_head_marker, 20)
assert_response("GET", "/health", health_head, health_body)
wait_serial(health_get_marker, 20)
assert_response("HEAD", "/health", health_head, b"")
wait_serial(health_head_marker, 20)

wait_serial(restart_ok_marker, 60)

post_restart = open_tls()
try:
    post_restart.sendall(
        b"GET / HTTP/1.1\r\n"
        b"Host: hfoc.test\r\n"
        b"Connection: close\r\n\r\n")
    data = read_tls(post_restart)
finally:
    post_restart.close()
if b"\r\n\r\n" not in data:
    raise SystemExit("post-restart root response missing header terminator")
head, body = data.split(b"\r\n\r\n", 1)
if head != root_head or body != root_body:
    raise SystemExit(
        "post-restart root response mismatch\n"
        f"expected={root_head + b'\\r\\n\\r\\n' + root_body!r}\n"
        f"actual={data!r}")
wait_serial(restart_live_marker, 20)
wait_serial(proof_ready_marker, 60)
wait_serial(balanced_pool_marker, 120)
wait_serial(balanced_counts_marker, 120)
wait_serial(clean_shutdown_marker, 120)
wait_serial(proof_ok_marker, 120)

text = serial_text()
required = [
    ready_marker,
    listener_marker,
    malformed_marker,
    peer_marker,
    malformed_drained_marker,
    four_marker,
    fifth_marker,
    pressure_marker,
    release_marker,
    root_get_marker,
    recovery_marker,
    root_head_marker,
    health_get_marker,
    health_head_marker,
    restart_ok_marker,
    restart_live_marker,
    proof_ready_marker,
    balanced_pool_marker,
    balanced_counts_marker,
    clean_shutdown_marker,
    proof_ok_marker,
]
for marker in required:
    if text.count(marker) != 1:
        raise SystemExit(
            f"Expected 1 marker, got {text.count(marker)}: {marker}")
if "[HTTPS-QEMU] failed" in text:
    raise SystemExit("HTTPS service QEMU proof reported failure")
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("Kernel panic detected")

print("QEMU HTTPS exhaustion baseline: found")
print("Four held connections including TLS handshake: found")
print("Fifth denial before TLS allocation: found")
print("Packet/TX pressure high-water: found")
print("Exact slot release after one close: found")
print("Recovery TLS connection and root response: found")
print("Route and lifecycle completion: found")
print("Balanced packet/pool/allocator counts: found")
print("Clean HMP shutdown: pending")
print("Kernel panic: none")
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

echo "QEMU HTTPS exhaustion matrix test: OK"
