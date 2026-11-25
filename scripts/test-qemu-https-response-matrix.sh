#!/usr/bin/env bash
# Prove item 292 in one q35 QEMU boot: exact HTTPS successful response matrix
# for GET/HEAD root and health routes, including exact headers and schema.
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

if [[ "${HFOC_HTTPS_RESPONSE_MATRIX_QEMU_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_HTTPS_RESPONSE_MATRIX_QEMU_TEST_TIMEOUT:-300}s" \
        env HFOC_HTTPS_RESPONSE_MATRIX_QEMU_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "HTTPS response matrix QEMU test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "HTTPS response matrix QEMU test must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "HTTPS response matrix QEMU test must not set QEMU_CPU" >&2
    exit 1
}
echo "QEMU HTTPS response matrix baseline: machine=q35 smp=1 cpu=default"

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

if [[ -n "${HFOC_HTTPS_RESPONSE_MATRIX_QEMU_TEST_PORT:-}" ]]; then
    NET_PORT=$HFOC_HTTPS_RESPONSE_MATRIX_QEMU_TEST_PORT
    if ! [[ "$NET_PORT" =~ ^[0-9]+$ ]] ||
            (( NET_PORT <= 0 || NET_PORT > 65535 )); then
        echo "HTTPS response matrix QEMU test port out of range: $NET_PORT" >&2
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
    artifact_dir="build/test-artifacts/https-response-matrix-qemu-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-HTTPS response matrix QEMU test failed}" \
        >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$INTERRUPTS_LOG" \
        "$TRACE_LOG" "$TEST_DIR/trace-events.txt" \
        "$METADATA" "$FAILURE_REASON" "$CERT_DER" "$CERT_PEM" \
        "$artifact_dir/"
    echo "HTTPS response matrix QEMU artifacts: $artifact_dir" >&2
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
    "$CERT_PEM" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import json
import os
import pathlib
import re
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
listener_marker = "[HTTPS-QEMU] listener live"
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
    b"X-Frame-Options: DENY"
)
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
    b"X-Frame-Options: DENY"
)


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
            raise SystemExit("Kernel panic during HTTPS response matrix harness")
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


def exchange(method, target, authorization=None, timeout=15):
    context = make_tls_context()
    with socket.create_connection(
            ("127.0.0.1", host_port), timeout=timeout) as raw:
        with context.wrap_socket(
                raw, server_hostname="hfoc.test") as tls:
            payload = (
                f"{method} {target} HTTP/1.1\r\n"
                "Host: hfoc.test\r\n").encode("ascii")
            if authorization is not None:
                payload += authorization + b"\r\n"
            payload += b"Connection: close\r\n\r\n"
            tls.sendall(payload)
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
    return head.split(b"\r\n", 1)[0], head, body


def request(method, target, timeout=15):
    status_line, head, body = exchange(method, target, None, timeout)
    if status_line != b"HTTP/1.1 200 OK":
        raise SystemExit(
            f"{method} {target}: unexpected status line {status_line!r}")
    return head, body


# ITEM-384: the panel session token is the deterministic fixture token the
# QEMU kernel issues through the existing administrator authentication
# boundary. The panel adds no authentication factor of its own.
panel_authorization = b"Authorization: Bearer " + b"B" * 32

required_hardening_headers = (
    b"Cache-Control: no-store",
    b"Content-Security-Policy: default-src 'none'; "
    b"style-src 'unsafe-inline'; base-uri 'none'; "
    b"form-action 'self'; frame-ancestors 'none'",
    b"Strict-Transport-Security: max-age=31536000",
    b"Referrer-Policy: no-referrer",
    b"X-Content-Type-Options: nosniff",
    b"X-Frame-Options: DENY",
)


def assert_hardening_headers(method, target, head):
    lines = head.split(b"\r\n")
    for header in required_hardening_headers:
        if header not in lines:
            raise SystemExit(
                f"{method} {target}: missing hardening header {header!r}")
    for line in lines:
        if line.startswith(b"Content-Length: "):
            return int(line.split(b": ", 1)[1])
    raise SystemExit(f"{method} {target}: missing Content-Length")


def assert_panel_document(method, target, head, body, needles):
    length = assert_hardening_headers(method, target, head)
    if length != len(body):
        raise SystemExit(
            f"{method} {target}: Content-Length {length} "
            f"does not match body length {len(body)}")
    for needle in needles:
        if needle not in body:
            raise SystemExit(
                f"{method} {target}: body does not contain {needle!r}")


def assert_status(method, target, authorization, expected, needles=()):
    status_line, head, body = exchange(method, target, authorization)
    if status_line != expected:
        raise SystemExit(
            f"{method} {target}: expected {expected!r}, got {status_line!r}")
    if method == "HEAD":
        if body != b"":
            raise SystemExit(f"HEAD {target}: body is not empty")
        assert_hardening_headers(method, target, head)
    else:
        assert_panel_document(method, target, head, body, needles)
    return head, body


def assert_response(method, target, expected_head, expected_body):
    head, body = request(method, target)
    if head != expected_head:
        raise SystemExit(
            f"{method} {target}: headers do not match\n"
            f"expected={expected_head!r}\nactual={head!r}")
    if body != expected_body:
        raise SystemExit(
            f"{method} {target}: body does not match\n"
            f"expected={expected_body!r}\nactual={body!r}")
    return body


def assert_health_schema(body):
    try:
        parsed = json.loads(body.decode("ascii"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise SystemExit(f"health schema is not valid JSON: {exc}")
    if parsed != {"status": "ok", "route": "health"}:
        raise SystemExit(
            f"health schema does not match: {parsed!r}")


wait_serial(listener_marker, 90)

assert_response("GET", "/", root_head, root_body)
assert_response("HEAD", "/", root_head, b"")
health = assert_response("GET", "/health", health_head, health_body)
assert_health_schema(health)
assert_response("HEAD", "/health", health_head, b"")

# ITEM-384 rendered panel over the wire.
assert_status(
    "GET", "/panel", None, b"HTTP/1.1 401 Unauthorized",
    (b"<h2>Management sign-in</h2>", b"SIGNED OUT"))
dashboard_head, dashboard_body = assert_status(
    "GET", "/panel", panel_authorization, b"HTTP/1.1 200 OK",
    (b"<h2>Dashboard</h2>", b"node-316", b"52:54:00:12:34:56", b"AVAILABLE"))
assert_status(
    "HEAD", "/panel", panel_authorization, b"HTTP/1.1 200 OK")
if assert_hardening_headers("HEAD", "/panel", dashboard_head) != \
        len(dashboard_body):
    raise SystemExit("HEAD /panel: Content-Length differs from GET /panel")
assert_status(
    "GET", "/panel/network", panel_authorization, b"HTTP/1.1 200 OK",
    # ITEM-505: the Network page is a configuring section now.  This fixture
    # binds no network owner, so the page must say so and still publish its
    # mode form and token form instead of the old generic value dump.
    (b"<h2>Network</h2>", b"/api/v1/network",
     b"No network owner is published", b'action="/panel/network"',
     b'name="mode"', b'name="token"'))
assert_status(
    "GET", "/panel/storage", panel_authorization, b"HTTP/1.1 200 OK",
    (b"<h2>Storage</h2>", b"UNAVAILABLE", b"No management API owner"))
assert_status(
    "GET", "/panel/updates", panel_authorization, b"HTTP/1.1 200 OK",
    (b"<h2>Updates</h2>", b"DEGRADED", b"/api/v1/system/update"))
# ITEM-382 about/licenses page over the wire: the shipped declared inventory
# with the mbedTLS Apache-2.0 election.
assert_status(
    "GET", "/panel/about/licenses", panel_authorization, b"HTTP/1.1 200 OK",
    (b"<h2>About and licenses</h2>", b"AVAILABLE",
     b"AGPL-3.0-only OR LicenseRef-HFOC-Commercial",
     b"mbedTLS election <code>Apache-2.0</code>",
     b"Apache-2.0 OR GPL-2.0-or-later",
     b"notices/mbedtls-Apache-2.0-election.txt",
     b"third_party/lwip/COPYING", b"third_party/limine/COPYING"))
assert_status(
    "GET", "/panel/about/licenses", None, b"HTTP/1.1 401 Unauthorized",
    (b"<h2>Management sign-in</h2>", b"SIGNED OUT"))
# ITEM-499: walk every navigation link the rendered panel publishes, as the
# remote client sees it.  The menu used to print the `/panel` prefix and then
# the section's own absolute path, so every entry but Dashboard answered 404
# `not_found`; the owner opened the panel, clicked a menu entry and got the
# refusal document.  The proof reads the links out of the served document
# instead of naming them a second time: the published set must be exactly the
# canonical targets, in order, and each one must answer the sign-in document
# anonymously (401, never 404) and its own section with the session.
canonical_nav_targets = [
    "/panel",
    "/panel/network",
    "/panel/services",
    "/panel/storage",
    "/panel/system",
    "/panel/security",
    "/panel/diagnostics",
    "/panel/updates",
    "/panel/recovery",
    # ITEM-504: the first mutating section joins the published menu.
    "/panel/routing",
    # ITEM-506 increment 6: the Account section joins the published menu.
    "/panel/account",
    # ITEM-507 increment 5: the Certificates section joins the published menu.
    "/panel/certificates",
    # ITEM-509 increment 8: the Help section joins the published menu.
    "/panel/help",
    "/panel/about/licenses",
]
_, nav_body = assert_status(
    "GET", "/panel", panel_authorization, b"HTTP/1.1 200 OK")
published_nav_targets = [
    target.decode("ascii")
    for target in re.findall(rb'href="([^"]+)"', nav_body)
]
if published_nav_targets != canonical_nav_targets:
    raise SystemExit(
        "GET /panel: the published menu does not name the served targets\n"
        f"expected={canonical_nav_targets!r}\n"
        f"actual={published_nav_targets!r}")
for nav_target in published_nav_targets:
    if "//" in nav_target:
        raise SystemExit(
            f"GET /panel: the menu joins a target twice: {nav_target!r}")
    assert_status(
        "GET", nav_target, None, b"HTTP/1.1 401 Unauthorized",
        (b"<h2>Management sign-in</h2>", b"SIGNED OUT"))
    assert_status(
        "GET", nav_target, panel_authorization, b"HTTP/1.1 200 OK",
        (b"<h1>HFOC Appliance</h1>",))
# ITEM-498: a refusal to a browser target is the HTML document, which names
# the status and links to the panel sign-in page.
_, panel_attack_body = assert_status(
    "GET", "/panel/attack", panel_authorization, b"HTTP/1.1 404 Not Found")
expected_panel_attack_body = refusal_html("404 Not Found")
if panel_attack_body != expected_panel_attack_body:
    raise SystemExit(
        "GET /panel/attack: body does not match the refusal document\n"
        f"expected={expected_panel_attack_body!r}\nactual={panel_attack_body!r}")
if b"rejected" in panel_attack_body:
    raise SystemExit("GET /panel/attack: body still carries the old word")
assert_status(
    "POST", "/panel", panel_authorization,
    b"HTTP/1.1 405 Method Not Allowed")

# ITEM-498: an API target is refused with JSON. `/api/v1/system/health` is not
# a published route in this build, so the management API's own not-found answer
# is served: it is valid JSON and it names the refusal in its `error` field.
_, api_unknown_body = assert_status(
    "GET", "/api/v1/system/health", None, b"HTTP/1.1 404 Not Found")
try:
    api_unknown = json.loads(api_unknown_body.decode("ascii"))
except (UnicodeDecodeError, json.JSONDecodeError) as exc:
    raise SystemExit(
        f"GET /api/v1/system/health: body is not JSON: {exc}") from exc
if api_unknown.get("error") != "not_found":
    raise SystemExit(
        "GET /api/v1/system/health: JSON does not name not_found\n"
        f"actual={api_unknown_body!r}")

_, unknown_body = assert_status(
    "GET", "/nope", None, b"HTTP/1.1 404 Not Found")
expected_unknown_body = refusal_json(404, "not_found")
if unknown_body != expected_unknown_body:
    raise SystemExit(
        "GET /nope: body does not match the refusal JSON\n"
        f"expected={expected_unknown_body!r}\nactual={unknown_body!r}")

text = serial_text()
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("Kernel panic detected")
for marker in (
        "[PANEL] section get ok",
        "[PANEL] section head ok",
        "[PANEL] denied ok",
        "[PANEL] method denied ok"):
    if marker not in text:
        raise SystemExit(f"missing panel serial marker {marker!r}")

print("GET / status/headers/body: found")
print("HEAD / zero body and matching headers: found")
print("Panel 401 without a session, 200 with the fixture session: found")
print("Panel section documents, UNAVAILABLE/DEGRADED sections: found")
print("Panel about/licenses page with the mbedTLS election: found")
print("Panel route deny (404) and method deny (405): found")
print("Hardening headers on every panel response: found")
print("GET /health status/headers/body: found")
print("HEAD /health zero body and matching headers: found")
print("Health JSON schema: found")
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
else
    printf '%s\n' "$FAILURE_MESSAGE" >&2
    exit 1
fi

echo "QEMU HTTPS response matrix test: OK"
