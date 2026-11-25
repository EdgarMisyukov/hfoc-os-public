#!/usr/bin/env bash
# Prove item 336 in one q35 QEMU boot: administrative authentication and
# authorization fixtures run before the real TLS 1.3 HTTPS management plane
# route/failure proof, including malformed isolation, fifth denial, peer
# throttle, lifecycle restart, health/audit evidence, timer liveness, and
# clean shutdown.
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

if [[ "${HFOC_MANAGEMENT_PLANE_QEMU_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_MANAGEMENT_PLANE_QEMU_TEST_TIMEOUT:-300}s" \
        env HFOC_MANAGEMENT_PLANE_QEMU_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Management plane QEMU test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "Management plane QEMU test must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "Management plane QEMU test must not set QEMU_CPU" >&2
    exit 1
}
echo "QEMU management plane baseline: machine=q35 smp=1 cpu=default"

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
    artifact_dir="build/test-artifacts/management-plane-qemu-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-Management plane QEMU test failed}" \
        >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$INTERRUPTS_LOG" \
        "$TRACE_LOG" "$TEST_DIR/trace-events.txt" \
        "$METADATA" "$FAILURE_REASON" "$CERT_DER" "$CERT_PEM" \
        "$artifact_dir/"
    echo "Management plane QEMU artifacts: $artifact_dir" >&2
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
if ! validation_output=$(python3 - "$SERIAL_LOG" "$NET_PORT" "$QEMU_PID" \
    "$CERT_PEM" 2>&1 <<'PY'
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


serial_path = pathlib.Path(sys.argv[1])
host_port = int(sys.argv[2])
qemu_pid = int(sys.argv[3])
certificate_pem = sys.argv[4]
timer_marker = "[LAPIC-TIMER] irq vector=0x42"
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
# ITEM-498: a malformed request line never publishes a target, so that refusal
# is the JSON body for 400. The unknown-route and reachability refusals are
# answered before the target is classified, so they carry the JSON 404 body.
malformed_body = refusal_json(400, "bad_request")
not_found_body = refusal_json(404, "not_found")
api_body = (
    b'{"api":"hfoc","version":1,"name":"management",'
    b'"status":"ok","auth":"unavailable","mutating":false}'
)
api_not_found_body = (
    b'{"api":"hfoc","version":1,"error":"not_found",'
    b'"auth":"unavailable","mutating":false}'
)
api_method_body = (
    b'{"api":"hfoc","version":1,"error":"method_not_allowed",'
    b'"auth":"unavailable","mutating":false}'
)
system_body = (
    b'{"api":"hfoc","version":1,"name":"system","status":"ok",'
    b'"auth":"unavailable","mutating":false,"machine_class":'
    b'"hfoc-appliance","product":"HFOC-APPLIANCE-V1",'
    b'"architecture":"x86_64","platform":"q35",'
    b'"hostname":"hfoc-appliance","instance":"default",'
    b'"uptime_ticks":316,"health":"ok","config_state":"ok",'
    b'"config_source":"committed","config_hostname":"node-316",'
    b'"recovery_mode":0,"active_process_count":0}'
)
system_method_body = (
    b'{"api":"hfoc","version":1,"name":"system",'
    b'"error":"method_not_allowed","auth":"unavailable",'
    b'"mutating":false}'
)
network_body = (
    b'{"api":"hfoc","version":1,"name":"network","status":"ok",'
    b'"auth":"unavailable","mutating":false,"config_state":"ok",'
    b'"config_source":"committed","interface":"eth0","mode":"static",'
    b'"ipv4_address":"10.0.2.15","ipv4_prefix":24,'
    b'"ipv4_gateway":"10.0.2.2","ipv4_dns":"10.0.2.3"}'
)
network_status_body = (
    b'{"api":"hfoc","version":1,"name":"network-status",'
    b'"status":"ok","auth":"unavailable","mutating":false,'
    b'"config_state":"ok","diagnostics":"available",'
    b'"interface":"eth0","mode":"static","active":true,'
    b'"ipv4_address":"10.0.2.15","ipv4_prefix":24,'
    b'"ipv4_gateway":"10.0.2.2","ipv4_dns":"10.0.2.3",'
    b'"device_id":1,"link":"up",'
    b'"gateway_reachability":"reachable",'
    b'"dns_reachability":"reachable","health":"pass",'
    b'"fault":"none"}'
)
network_method_body = (
    b'{"api":"hfoc","version":1,"name":"network",'
    b'"error":"method_not_allowed","auth":"unavailable",'
    b'"mutating":false}'
)
service_body = (
    b'{"api":"hfoc","version":1,"name":"service","status":"ok",'
    b'"auth":"unavailable","mutating":false,"state":"running",'
    b'"policy":"rollback","generation":1,"configured_count":1,'
    b'"active_count":1,"ready_count":1,"pending_count":0,'
    b'"failed_count":0,"exhausted_count":0,"health":"pass",'
    b'"fault":"none"}'
)
service_method_body = (
    b'{"api":"hfoc","version":1,"name":"service",'
    b'"error":"method_not_allowed","auth":"unavailable",'
    b'"mutating":false}'
)

api_health_body = (
    b'{"api":"hfoc","version":1,"name":"health","status":"ok",'
    b'"auth":"unavailable","mutating":false,"state":"OK",'
    b'"healthy":true,"critical":false,"first_fault_domain":"NONE",'
    b'"first_fault":"NONE"}'
)
api_health_method_body = (
    b'{"api":"hfoc","version":1,"name":"health",'
    b'"error":"method_not_allowed","auth":"unavailable",'
    b'"mutating":false}'
)


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


def wait_serial(marker, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = serial_text()
        if marker in text:
            return
        if "[MGMT-PLANE] failed" in text or "[HTTPS-QEMU] failed" in text:
            raise SystemExit(
                "Management plane QEMU proof reported a fixture failure")
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit(
                "Kernel panic during management plane QEMU proof")
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


def request(method, target, timeout=15):
    context = make_tls_context()
    with socket.create_connection(
            ("127.0.0.1", host_port), timeout=timeout) as raw:
        with context.wrap_socket(
                raw, server_hostname="hfoc.test") as tls:
            tls.sendall(
                f"{method} {target} HTTP/1.1\r\n"
                "Host: hfoc.test\r\n"
                "Connection: close\r\n\r\n".encode("ascii"))
            data = b""
            while True:
                try:
                    chunk = tls.recv(4096)
                except ssl.SSLEOFError:
                    break
                if not chunk:
                    break
                data += chunk
    if b"\r\n\r\n" not in data:
        raise SystemExit(
            f"{method} {target}: response missing header terminator")
    head, body = data.split(b"\r\n\r\n", 1)
    lines = head.split(b"\r\n")
    try:
        status = int(lines[0].split()[1])
    except (IndexError, ValueError):
        raise SystemExit(
            f"{method} {target}: malformed status line {lines[0]!r}")
    return status, head, body


def open_held_tls(timeout=10):
    return socket.create_connection(
        ("127.0.0.1", host_port), timeout=timeout)


def assert_request(method, target, status, body, content_length):
    actual_status, head, actual_body = request(method, target)
    if actual_status != status:
        raise SystemExit(
            f"{method} {target}: status {actual_status}, expected {status}")
    if actual_body != body:
        raise SystemExit(
            f"{method} {target}: body {actual_body!r}, expected {body!r}")
    marker = f"Content-Length: {content_length}".encode("ascii")
    if marker not in head:
        raise SystemExit(
            f"{method} {target}: header missing {marker!r}")
    return actual_body


def malformed_request(timeout=15):
    context = make_tls_context()
    with socket.create_connection(
            ("127.0.0.1", host_port), timeout=timeout) as raw:
        with context.wrap_socket(
                raw, server_hostname="hfoc.test") as tls:
            tls.sendall(
                b"GET /\x01 HTTP/1.1\r\n"
                b"Host: hfoc.test\r\n"
                b"Connection: close\r\n\r\n")
            data = b""
            while True:
                try:
                    chunk = tls.recv(4096)
                except ssl.SSLEOFError:
                    break
                if not chunk:
                    break
                data += chunk
    if b"\r\n\r\n" not in data:
        raise SystemExit("malformed request: response missing terminator")
    head, body = data.split(b"\r\n\r\n", 1)
    if not head.startswith(b"HTTP/1.1 400 "):
        raise SystemExit(
            f"malformed request: unexpected status {head.splitlines()[0]!r}")
    if body != malformed_body:
        raise SystemExit(
            f"malformed request: body {body!r}, expected {malformed_body!r}")


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


wait_serial("[MGMT-PLANE] proof OK", 60)
wait_serial("[HTTPS-QEMU] listener live", 60)

assert_request("GET", "/", 200, root_body, len(root_body))
wait_serial("[HTTPS-QEMU] root get ok", 20)

assert_request("HEAD", "/", 200, b"", len(root_body))
wait_serial("[HTTPS-QEMU] root head ok", 20)

assert_request("GET", "/health", 200, health_body, 32)
wait_serial("[HTTPS-QEMU] health get ok", 20)

assert_request("HEAD", "/health", 200, b"", 32)
wait_serial("[HTTPS-QEMU] health head ok", 20)

assert_request("GET", "/api/v1", 200, api_body, len(api_body))
wait_serial("[MGMT-API] version get ok", 20)

assert_request("HEAD", "/api/v1", 200, b"", len(api_body))
wait_serial("[MGMT-API] version head ok", 20)

assert_request(
    "POST", "/api/v1", 405, api_method_body, len(api_method_body))
wait_serial("[MGMT-API] method denied ok", 20)

assert_request(
    "GET", "/api/v1/status", 404,
    api_not_found_body, len(api_not_found_body))
wait_serial("[MGMT-API] not found ok", 20)

assert_request("GET", "/api/v1/system", 200, system_body, len(system_body))
wait_serial("[SYSTEM-API] system get ok", 20)

assert_request(
    "HEAD", "/api/v1/system", 200, b"", len(system_body))
wait_serial("[SYSTEM-API] system head ok", 20)

assert_request(
    "POST", "/api/v1/system", 405,
    system_method_body, len(system_method_body))
wait_serial("[SYSTEM-API] method denied ok", 20)

assert_request("GET", "/api/v1/network", 200, network_body, len(network_body))
wait_serial("[NETWORK-API] config get ok", 20)

assert_request(
    "HEAD", "/api/v1/network", 200, b"", len(network_body))
wait_serial("[NETWORK-API] config head ok", 20)

assert_request(
    "GET", "/api/v1/network/status", 200,
    network_status_body, len(network_status_body))
wait_serial("[NETWORK-API] status get ok", 20)

assert_request(
    "HEAD", "/api/v1/network/status", 200, b"",
    len(network_status_body))
wait_serial("[NETWORK-API] status head ok", 20)

assert_request(
    "POST", "/api/v1/network", 405,
    network_method_body, len(network_method_body))
wait_serial("[NETWORK-API] method denied ok", 20)

assert_request("GET", "/api/v1/service", 200, service_body, len(service_body))
wait_serial("[SERVICE-API] service get ok", 20)

assert_request(
    "HEAD", "/api/v1/service", 200, b"", len(service_body))
wait_serial("[SERVICE-API] service head ok", 20)

assert_request(
    "POST", "/api/v1/service", 405,
    service_method_body, len(service_method_body))
wait_serial("[SERVICE-API] method denied ok", 20)

assert_request("GET", "/api/v1/health", 200, api_health_body, len(api_health_body))
wait_serial("[HEALTH-API] health get ok", 20)

assert_request(
    "HEAD", "/api/v1/health", 200, b"", len(api_health_body))
wait_serial("[HEALTH-API] health head ok", 20)

assert_request(
    "POST", "/api/v1/health", 405,
    api_health_method_body, len(api_health_method_body))
wait_serial("[HEALTH-API] method denied ok", 20)

# ITEM-385: ask the appliance to publish the console-only reachability policy,
# then prove over the wire that the live peer is refused and served again once
# the policy returns. The trigger target is unknown to every route, so the
# request itself is answered like any other unknown route.
assert_request(
    "GET", "/mgmt-reach-proof", 404,
    not_found_body, len(not_found_body))
wait_serial("[MGMT-REACH] console-only published ok", 20)
assert_request("GET", "/", 404, not_found_body, len(not_found_body))
wait_serial("[MGMT-REACH] live peer denied ok", 20)
assert_request("GET", "/", 200, root_body, len(root_body))
wait_serial("[MGMT-REACH] console-only served again ok", 20)

malformed_request()
wait_serial("[HTTPS-QEMU] malformed request ok", 20)
wait_serial("[HTTPS-QEMU] peer throttle ok", 20)

held = []
for _ in range(4):
    held.append(open_held_tls())
wait_serial("[HTTPS-QEMU] four held", 20)
if not fifth_rejected():
    raise SystemExit("fifth connection unexpectedly succeeded")
wait_serial("[HTTPS-QEMU] fifth denial ok", 20)
for sock in held:
    sock.close()
held.clear()

wait_serial("[HTTPS-QEMU] restart ok", 60)
assert_request("GET", "/", 200, root_body, len(root_body))
wait_serial("[HTTPS-QEMU] restart live", 20)
wait_serial("[HTTPS-QEMU] proof ready", 20)

wait_serial("[HTTPS-QEMU] health ok", 20)
wait_serial("[HTTPS-QEMU] audit ok", 20)
wait_serial("[HTTPS-QEMU] proof OK", 60)

text = serial_text()
required = [
    "[MGMT-PLANE] proof init",
    "[MGMT-PLANE] auth absent ok",
    "[MGMT-PLANE] credential active ok",
    "[MGMT-PLANE] wrong proof denied ok",
    "[MGMT-PLANE] valid session ok",
    "[MGMT-PLANE] valid bearer ok",
    "[MGMT-PLANE] missing bearer denied ok",
    "[MGMT-PLANE] malformed bearer denied ok",
    "[MGMT-PLANE] unknown bearer denied ok",
    "[MGMT-PLANE] disable revocation ok",
    "[MGMT-PLANE] reset recovery ok",
    "[MGMT-PLANE] throttle denial ok",
    "[MGMT-PLANE] authorization policy ok",
    "[MGMT-PLANE] discovery public ok",
    "[MGMT-PLANE] protected deny closed ok",
    "[MGMT-PLANE] protected read authorized ok",
    "[MGMT-PLANE] method denied ok",
    "[MGMT-PLANE] unknown denied ok",
    "[MGMT-PLANE] production route boundary ok",
    "[MGMT-PLANE] proof OK",
    "[MGMT-REACH] proof init",
    "[MGMT-REACH] console allowed ok",
    "[MGMT-REACH] management interface served ok",
    "[MGMT-REACH] outer interface denied ok",
    "[MGMT-REACH] client certificate denied ok",
    "[MGMT-REACH] remote opt-in admitted ok",
    "[MGMT-REACH] remote opt-in denied ok",
    "[MGMT-REACH] readiness degraded ok",
    "[MGMT-REACH] proof OK",
    "[MGMT-REACH] live peer admitted ok",
    "[MGMT-REACH] console-only published ok",
    "[MGMT-REACH] live peer denied ok",
    "[MGMT-REACH] console-only served again ok",
    "[HTTPS-QEMU] proof init",
    "[HTTPS-QEMU] listener live",
    "[HTTPS-QEMU] root get ok",
    "[HTTPS-QEMU] root head ok",
    "[HTTPS-QEMU] health get ok",
    "[HTTPS-QEMU] health head ok",
    "[MGMT-API] version get ok",
    "[MGMT-API] version head ok",
    "[MGMT-API] method denied ok",
    "[MGMT-API] not found ok",
    "[SYSTEM-API] system get ok",
    "[SYSTEM-API] system head ok",
    "[SYSTEM-API] method denied ok",
    "[NETWORK-API] config get ok",
    "[NETWORK-API] config head ok",
    "[NETWORK-API] status get ok",
    "[NETWORK-API] status head ok",
    "[NETWORK-API] method denied ok",
    "[SERVICE-API] service get ok",
    "[SERVICE-API] service head ok",
    "[SERVICE-API] method denied ok",
    "[HEALTH-API] health get ok",
    "[HEALTH-API] health head ok",
    "[HEALTH-API] method denied ok",
    "[HTTPS-QEMU] malformed request ok",
    "[HTTPS-QEMU] four held",
    "[HTTPS-QEMU] fifth denial ok",
    "[HTTPS-QEMU] peer throttle ok",
    "[HTTPS-QEMU] restart ok",
    "[HTTPS-QEMU] restart live",
    "[HTTPS-QEMU] proof ready",
    "[HTTPS-QEMU] health ok",
    "[HTTPS-QEMU] audit ok",
    "[HTTPS-QEMU] balanced pool",
    "[HTTPS-QEMU] clean shutdown",
    "[HTTPS-QEMU] proof OK",
]
for marker in required:
    if text.count(marker) != 1:
        raise SystemExit(
            f"Expected 1 marker, got {text.count(marker)}: {marker}")
if "[MGMT-PLANE] failed" in text or "[HTTPS-QEMU] failed" in text:
    raise SystemExit("Management plane QEMU proof reported failure")
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

print("Management plane proof init: found")
print("Admin auth fixture: found")
print("Management authorization fixture: found")
print("Management reachability policy: found")
print("Management reachability outer interface denial: found")
print("Management reachability management interface service: found")
print("Management reachability live peer admitted: found")
print("Management reachability console-only denial on the wire: found")
print("Management reachability service restored on the wire: found")
print("HTTPS proof init: found")
print("TLS 1.3 listener live: found")
print("Root GET: found")
print("Root HEAD zero-body: found")
print("Health GET: found")
print("Health HEAD zero-body: found")
print("Management API version GET: found")
print("Management API version HEAD zero-body: found")
print("Management API method denial: found")
print("Management API unknown route 404: found")
print("System API GET: found")
print("System API HEAD zero-body: found")
print("System API method denial: found")
print("Service API GET: found")
print("Service API HEAD zero-body: found")
print("Service API method denial: found")

print("Health API GET: found")
print("Health API HEAD zero-body: found")
print("Health API method denial: found")
print("Malformed request isolation: found")
print("Four held connections: found")
print("Fifth denial before TLS allocation: found")
print("Peer throttle isolation: found")
print("Lifecycle restart: found")
print("Restart live route: found")
print("Proof ready: found")
print("Health publication: found")
print("Audit evidence: found")
print("Balanced allocator pools: found")
print("Clean shutdown: found")
print("Proof OK: found")
print("Timer liveness: found")
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

echo "QEMU management plane acceptance test: OK"
