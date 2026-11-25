#!/usr/bin/env bash
# Prove item 291 in one q35 QEMU boot: standard curl and OpenSSL HTTPS client
# harness for hfoc.test:18443, hostname rejection, and untrusted-CA rejection.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in python3 qemu-system-x86_64 timeout mktemp openssl curl; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

if [[ "${HFOC_HTTPS_CLIENT_QEMU_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_HTTPS_CLIENT_QEMU_TEST_TIMEOUT:-300}s" \
        env HFOC_HTTPS_CLIENT_QEMU_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}
CLIENT_PORT=${HFOC_HTTPS_CLIENT_QEMU_TEST_PORT:-18443}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "HTTPS client QEMU test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "HTTPS client QEMU test must set QEMU_SMP=1" >&2
    exit 1
}
test -z "${QEMU_CPU:-}" || {
    echo "HTTPS client QEMU test must not set QEMU_CPU" >&2
    exit 1
}
if ! test "$CLIENT_PORT" -gt 0 || ! test "$CLIENT_PORT" -le 65535; then
    echo "HTTPS client QEMU test port out of range: $CLIENT_PORT" >&2
    exit 1
fi
echo "QEMU HTTPS client baseline: machine=q35 smp=1 cpu=default port=$CLIENT_PORT"

TEST_DIR=$(mktemp -d)
SERIAL_LOG=$TEST_DIR/com1.log
MONITOR_LOG=$TEST_DIR/monitor.log
QEMU_LOG=$TEST_DIR/qemu.log
INTERRUPTS_LOG=$TEST_DIR/interrupts.log
TRACE_LOG=$TEST_DIR/trace.log
METADATA=$TEST_DIR/metadata.txt
FAILURE_REASON=$TEST_DIR/failure-reason.txt
MONITOR_SOCKET=$TEST_DIR/monitor.sock
CERT_DER=$TEST_DIR/hfoc-test-cert.der
CERT_PEM=$TEST_DIR/hfoc-test-cert.pem
UNTRUSTED_PEM=$TEST_DIR/untrusted-ca.pem
UNTRUSTED_KEY=$TEST_DIR/untrusted-ca.key
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

if ! python3 - "$CLIENT_PORT" <<'PY'
import socket
import sys

port = int(sys.argv[1])
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.bind(("127.0.0.1", port))
PY
then
    echo "HTTPS client QEMU port is not available: $CLIENT_PORT" >&2
    exit 1
fi

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
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
    -keyout "$UNTRUSTED_KEY" -out "$UNTRUSTED_PEM" \
    -subj "/CN=untrusted.test" >/dev/null 2>&1

printf '%s\n' \
    "machine=$QEMU_MACHINE" \
    "cpu=default" \
    "smp=$QEMU_SMP" \
    "image=$IMAGE" \
    "host_port=$CLIENT_PORT" \
    "guest_port=443" \
    "certificate_pem=$CERT_PEM" \
    "untrusted_ca_pem=$UNTRUSTED_PEM" >"$METADATA"

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/https-client-qemu-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-HTTPS client QEMU test failed}" \
        >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$MONITOR_LOG" "$QEMU_LOG" "$INTERRUPTS_LOG" \
        "$TRACE_LOG" "$TEST_DIR/trace-events.txt" \
        "$METADATA" "$FAILURE_REASON" "$CERT_DER" "$CERT_PEM" \
        "$UNTRUSTED_PEM" "$UNTRUSTED_KEY" \
        "$artifact_dir/"
    echo "HTTPS client QEMU artifacts: $artifact_dir" >&2
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
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:"$CLIENT_PORT"-:443 \
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
if ! validation_output=$(python3 - "$SERIAL_LOG" "$CLIENT_PORT" "$QEMU_PID" \
    "$CERT_PEM" "$UNTRUSTED_PEM" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import os
import pathlib
import subprocess
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
host_port = int(sys.argv[2])
qemu_pid = int(sys.argv[3])
certificate_pem = sys.argv[4]
untrusted_pem = sys.argv[5]
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
root_request = (
    b"GET / HTTP/1.1\r\n"
    b"Host: hfoc.test\r\n"
    b"Connection: close\r\n\r\n"
)
listener_marker = "[HTTPS-QEMU] listener live"


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
            raise SystemExit("Kernel panic during HTTPS client QEMU harness")
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


def run(label, command, input_bytes=b"", timeout=30):
    try:
        result = subprocess.run(
            command,
            input=input_bytes,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired:
        raise SystemExit(f"{label}: client command timed out")
    return result.returncode, result.stdout.decode("ascii", errors="replace")


def fail(label, output):
    raise SystemExit(f"{label}: unexpected result:\n{output}")


def curl_success():
    command = [
        "curl",
        "--noproxy", "*",
        "--tlsv1.3",
        "--cacert", certificate_pem,
        "--resolve", f"hfoc.test:{host_port}:127.0.0.1",
        "--silent", "--show-error", "--fail",
        "--max-time", "20",
        "--write-out",
        "\nHFOC_CURL_CODE=%{http_code}\nHFOC_CURL_SSL_VERIFY=%{ssl_verify_result}\n",
        f"https://hfoc.test:{host_port}/",
    ]
    rc, output = run("curl root", command)
    if rc != 0:
        fail("curl root", output)
    if root_body.decode("ascii") not in output:
        fail("curl root body", output)
    if "HFOC_CURL_CODE=200" not in output:
        fail("curl root status", output)
    if "HFOC_CURL_SSL_VERIFY=0" not in output:
        fail("curl root certificate verification", output)
    return output


def curl_hostname_failure():
    command = [
        "curl",
        "--noproxy", "*",
        "--tlsv1.3",
        "--cacert", certificate_pem,
        "--silent", "--show-error", "--fail",
        "--max-time", "20",
        f"https://127.0.0.1:{host_port}/",
    ]
    rc, output = run("curl hostname mismatch", command)
    if rc == 0:
        fail("curl hostname mismatch", output)
    lower = output.lower()
    if "certificate" not in lower or not any(
            token in lower for token in (
                "hostname mismatch",
                "ip address mismatch",
                "subject name",
                "does not match",
                "identity")):
        fail("curl hostname mismatch", output)
    return output


def curl_untrusted_failure():
    command = [
        "curl",
        "--noproxy", "*",
        "--tlsv1.3",
        "--cacert", untrusted_pem,
        "--resolve", f"hfoc.test:{host_port}:127.0.0.1",
        "--silent", "--show-error", "--fail",
        "--max-time", "20",
        f"https://hfoc.test:{host_port}/",
    ]
    rc, output = run("curl untrusted CA", command)
    if rc == 0:
        fail("curl untrusted CA", output)
    lower = output.lower()
    if not any(token in lower for token in (
            "self-signed",
            "unable to get local issuer certificate",
            "certificate verify failed",
            "untrusted")):
        fail("curl untrusted CA", output)
    return output


def openssl_success():
    command = [
        "openssl",
        "s_client",
        "-tls1_3",
        "-connect", f"127.0.0.1:{host_port}",
        "-servername", "hfoc.test",
        "-CAfile", certificate_pem,
        "-no-CAfile", "-no-CApath",
        "-verify_hostname", "hfoc.test",
        "-verify_return_error",
        "-ign_eof",
    ]
    rc, output = run("OpenSSL root", command, input_bytes=root_request)
    if rc != 0:
        fail("OpenSSL root", output)
    for marker in (
            "Verification: OK",
            "Verified peername: hfoc.test",
            "New, TLSv1.3",
            "HTTP/1.1 200",
            root_body.decode("ascii")):
        if marker not in output:
            fail(f"OpenSSL root marker {marker!r}", output)
    return output


def openssl_hostname_failure():
    command = [
        "openssl",
        "s_client",
        "-tls1_3",
        "-connect", f"127.0.0.1:{host_port}",
        "-servername", "hfoc.test",
        "-CAfile", certificate_pem,
        "-no-CAfile", "-no-CApath",
        "-verify_hostname", "127.0.0.1",
        "-verify_return_error",
        "-ign_eof",
    ]
    rc, output = run("OpenSSL hostname mismatch", command)
    if rc == 0:
        fail("OpenSSL hostname mismatch", output)
    lower = output.lower()
    if "verification" not in lower or not any(
            token in lower for token in (
                "failed",
                "error",
                "hostname mismatch",
                "ip address mismatch",
                "does not match")):
        fail("OpenSSL hostname mismatch", output)
    return output


def openssl_untrusted_failure():
    command = [
        "openssl",
        "s_client",
        "-tls1_3",
        "-connect", f"127.0.0.1:{host_port}",
        "-servername", "hfoc.test",
        "-CAfile", untrusted_pem,
        "-no-CAfile", "-no-CApath",
        "-verify_hostname", "hfoc.test",
        "-verify_return_error",
        "-ign_eof",
    ]
    rc, output = run("OpenSSL untrusted CA", command)
    if rc == 0:
        fail("OpenSSL untrusted CA", output)
    lower = output.lower()
    if not any(token in lower for token in (
            "self-signed",
            "unable to get local issuer certificate",
            "certificate verify failed",
            "untrusted")):
        fail("OpenSSL untrusted CA", output)
    return output


wait_serial(listener_marker, 90)

curl_success()
time.sleep(0.2)
curl_hostname_failure()
time.sleep(0.2)
curl_untrusted_failure()
time.sleep(0.2)
openssl_success()
time.sleep(0.2)
openssl_hostname_failure()
time.sleep(0.2)
openssl_untrusted_failure()

text = serial_text()
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("Kernel panic detected")

print("HTTPS client QEMU baseline: found")
print("curl TLS 1.3 root: found")
print("curl hostname mismatch rejection: found")
print("curl untrusted CA rejection: found")
print("OpenSSL TLS 1.3 verification: found")
print("OpenSSL hostname mismatch rejection: found")
print("OpenSSL untrusted CA rejection: found")
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

echo "QEMU HTTPS client test: OK"
