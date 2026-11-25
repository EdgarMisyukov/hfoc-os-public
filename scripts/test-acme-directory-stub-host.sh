#!/usr/bin/env bash
# ITEM-508 increment 15: the offline ACME directory stub is proven from the
# outside, by a client that shares no code with it.
#
# The guest proof (a fixture kernel dialling 10.0.2.2:18445 through the real
# lwIP + mbedTLS binding) comes later in this increment.  What this proof
# establishes now is the other half of the exchange: scripts/hfoc-acme-
# directory-stub.py serves exactly one directory document over TLS 1.3, from
# the fixture chain of tests/appliance-acme-directory-fixtures.h, with a
# certificate that an independent client verifies for the literal address the
# guest will dial - and with no private key bytes in any log.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in python3 openssl mktemp timeout; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

STUB=scripts/hfoc-acme-directory-stub.py
FIXTURES=tests/appliance-acme-directory-fixtures.h
test -f "$STUB" || { echo "Missing stub: $STUB" >&2; exit 1; }
test -f "$FIXTURES" || { echo "Missing fixtures: $FIXTURES" >&2; exit 1; }

TEST_DIR=$(mktemp -d)
STUB_PID=
cleanup() {
    if test -n "$STUB_PID" && kill -0 "$STUB_PID" 2>/dev/null; then
        kill "$STUB_PID" 2>/dev/null || true
        wait "$STUB_PID" 2>/dev/null || true
    fi
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

STUB_LOG=$TEST_DIR/stub.log
CA_PEM=$TEST_DIR/ca.pem
CHAIN_PEM=$TEST_DIR/chain.pem

# An ephemeral port, chosen by the same stack the stub binds, so two proofs on
# one host cannot collide.
PORT=$(python3 -c 'import socket
s = socket.socket()
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()')
case "$PORT" in
    ''|*[!0-9]*) echo "Failed to choose a port" >&2; exit 1 ;;
esac
if ! test "$PORT" -ge 1024 || ! test "$PORT" -le 65535; then
    echo "Chosen port out of range: $PORT" >&2
    exit 1
fi

python3 "$STUB" --port "$PORT" >"$STUB_LOG" 2>&1 &
STUB_PID=$!

LISTEN_MARK="ACME-STUB listening port=$PORT"
deadline=$((SECONDS + 30))
while ! grep -Fq "$LISTEN_MARK" "$STUB_LOG" 2>/dev/null; do
    if ! kill -0 "$STUB_PID" 2>/dev/null; then
        echo "ACME directory stub died before listening" >&2
        cat "$STUB_LOG" >&2 || true
        exit 1
    fi
    if test "$SECONDS" -ge "$deadline"; then
        echo "Timed out waiting for: $LISTEN_MARK" >&2
        cat "$STUB_LOG" >&2 || true
        exit 1
    fi
    sleep 0.2
done
echo "ACME directory stub listening on 127.0.0.1:$PORT"

# The chain the stub serves: leaf first, issuer second.  The last certificate
# of -showcerts is the CA, which is the only trust input the guest kernel will
# carry, so it is the only one this proof trusts as well.
timeout --kill-after=5s 20s openssl s_client \
    -connect "127.0.0.1:$PORT" -tls1_3 -servername 10.0.2.2 \
    -showcerts </dev/null >"$CHAIN_PEM" 2>"$TEST_DIR/s_client.err" || {
        echo "TLS 1.3 handshake with the stub failed" >&2
        cat "$TEST_DIR/s_client.err" >&2 || true
        exit 1
    }
grep -Fq 'BEGIN CERTIFICATE' "$CHAIN_PEM" || {
    echo "The stub served no certificate" >&2
    cat "$CHAIN_PEM" >&2
    exit 1
}
# s_client repeats the leaf after the chain section, so the certificates are
# extracted in order and the issuer is the second distinct one.
python3 - "$CHAIN_PEM" "$CA_PEM" <<'PY'
import re
import sys

raw = open(sys.argv[1], "r", encoding="ascii", errors="replace").read()
blocks = re.findall(
    r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----",
    raw,
    re.DOTALL,
)
if len(blocks) < 2:
    print(
        "The stub served %d certificate(s); the fixture CA is not in the chain"
        % len(blocks),
        file=sys.stderr,
    )
    sys.exit(1)
with open(sys.argv[2], "w", encoding="ascii") as handle:
    handle.write(blocks[1].strip() + "\n")
PY
# The CA must actually verify the leaf, for the literal the guest dials.
timeout --kill-after=5s 20s openssl s_client \
    -connect "127.0.0.1:$PORT" -tls1_3 -servername 10.0.2.2 \
    -CAfile "$CA_PEM" -verify_ip 10.0.2.2 -verify_return_error \
    </dev/null >"$TEST_DIR/verified.out" 2>"$TEST_DIR/verified.err" || {
        echo "Independent client did not accept the stub chain for 10.0.2.2" >&2
        cat "$TEST_DIR/verified.err" >&2 || true
        exit 1
    }
grep -Fq 'Verify return code: 0 (ok)' "$TEST_DIR/verified.out" || {
    echo "Independent client returned a non-zero verify result" >&2
    grep -F 'Verify return code' "$TEST_DIR/verified.out" >&2 || true
    exit 1
}

# One request over TLS 1.3, with the same pinned chain and literal the guest
# will use.  The stub does not verify the JWS signature, so two fixed base64url
# fields are enough; the guest driver is the one that signs the real requests.
stub_request() {
    local method=$1
    local target=$2
    local body=$3
    local out=$4
    local length=${#body}

    {
        printf '%s %s HTTP/1.1\r\nHost: 10.0.2.2\r\nConnection: close\r\n' \
            "$method" "$target"
        if [[ $method == POST ]]; then
            printf 'Content-Type: application/jose+json\r\n'
            printf 'Content-Length: %d\r\n' "$length"
        fi
        printf '\r\n%s' "$body"
    } | timeout --kill-after=5s 20s openssl s_client \
            -quiet -connect "127.0.0.1:$PORT" -tls1_3 -servername 10.0.2.2 \
            -CAfile "$CA_PEM" -verify_ip 10.0.2.2 -verify_return_error \
            >"$out" 2>"$TEST_DIR/request.err" || {
        echo "The $method $target request failed" >&2
        cat "$TEST_DIR/request.err" >&2 || true
        return 1
    }
}

# ITEM-508 increment 22: the finalize body carries a real PKCS#10, because the
# stub now refuses a payload without one.  The key is generated outside the
# tree; the stub checks the request's own signature and subject, not the
# account key (the guest proof owns the account key).
openssl ecparam -name prime256v1 -genkey -noout \
    -out "$TEST_DIR/csr-key.pem" 2>"$TEST_DIR/csr-key.err" || {
    echo "Failed to generate the CSR key" >&2
    cat "$TEST_DIR/csr-key.err" >&2 || true
    exit 1
}
openssl req -new -key "$TEST_DIR/csr-key.pem" -subj '/CN=hfoc-appliance' \
    -outform DER -out "$TEST_DIR/csr.der" 2>"$TEST_DIR/csr.err" || {
    echo "Failed to build the CSR" >&2
    cat "$TEST_DIR/csr.err" >&2 || true
    exit 1
}
FINALIZE_BODY=$(python3 - "$TEST_DIR/csr.der" <<'PY'
import base64
import json
import sys

der = open(sys.argv[1], "rb").read()
csr = base64.urlsafe_b64encode(der).decode("ascii").rstrip("=")
payload = base64.urlsafe_b64encode(
    json.dumps({"csr": csr}, separators=(",", ":")).encode("utf-8")
).decode("ascii").rstrip("=")
print(json.dumps(
    {"protected": "e30", "payload": payload, "signature": "AA"},
    separators=(",", ":"),
))
PY
)
EMPTY_CSR_BODY=$(python3 - <<'PY'
import base64
import json

payload = base64.urlsafe_b64encode(b"{}").decode("ascii").rstrip("=")
print(json.dumps(
    {"protected": "e30", "payload": payload, "signature": "AA"},
    separators=(",", ":"),
))
PY
)

stub_request GET /directory "" "$TEST_DIR/r_directory"
stub_request POST /acme/new-nonce "" "$TEST_DIR/r_nonce"
stub_request POST /acme/new-account "{}" "$TEST_DIR/r_account"
stub_request POST /acme/new-order "{}" "$TEST_DIR/r_order"
stub_request POST /acme/authz/1 "{}" "$TEST_DIR/r_authz"
stub_request POST /acme/chall/1 "{}" "$TEST_DIR/r_chall"
# The first answer is `processing`; the poll that follows is `valid`.
stub_request POST /acme/chall/1 "{}" "$TEST_DIR/r_chall_poll"
stub_request POST /acme/finalize/1 "$FINALIZE_BODY" "$TEST_DIR/r_finalize"
# A finalize without a CSR is a named refusal, never a silent acceptance.
stub_request POST /acme/finalize/1 "$EMPTY_CSR_BODY" \
    "$TEST_DIR/r_finalize_no_csr"
# Finalize answers the still-pending order; the order URL poll is finalized.
stub_request POST /acme/order/1 "{}" "$TEST_DIR/r_order_poll"
stub_request POST /acme/cert/1 "{}" "$TEST_DIR/r_cert"
stub_request GET /elsewhere "" "$TEST_DIR/r_missing"

# The documents themselves, byte for byte, plus the headers the guest flow
# reads: `Location` on the account, `Replay-Nonce` on every reply, the
# processing-then-valid challenge pair, the pending finalize and the finalized order poll,
# the named refusal of a finalize without a CSR, and the certificate body
# carrying the fixture chain the anchor will verify.
python3 - "$PORT" "$CHAIN_PEM" \
    "$TEST_DIR/r_directory" "$TEST_DIR/r_nonce" "$TEST_DIR/r_account" \
    "$TEST_DIR/r_order" "$TEST_DIR/r_authz" "$TEST_DIR/r_chall" \
    "$TEST_DIR/r_chall_poll" "$TEST_DIR/r_finalize" \
    "$TEST_DIR/r_finalize_no_csr" \
    "$TEST_DIR/r_order_poll" "$TEST_DIR/r_cert" \
    "$TEST_DIR/r_missing" <<'PY'
import re
import sys

port = int(sys.argv[1])
chain_dump = open(sys.argv[2], "r", encoding="ascii", errors="replace").read()
paths = sys.argv[3:]
names = [
    "GET /directory",
    "POST /acme/new-nonce",
    "POST /acme/new-account",
    "POST /acme/new-order",
    "POST /acme/authz/1",
    "POST /acme/chall/1 (first)",
    "POST /acme/chall/1 (poll)",
    "POST /acme/finalize/1",
    "POST /acme/finalize/1 (no csr)",
    "POST /acme/order/1 (poll)",
    "POST /acme/cert/1",
    "GET /elsewhere",
]

expected_directory = (
    '{"newNonce":"https://10.0.2.2:%d/acme/new-nonce",'
    '"newAccount":"https://10.0.2.2:%d/acme/new-account",'
    '"newOrder":"https://10.0.2.2:%d/acme/new-order"}'
) % (port, port, port)
expected_account = '{"status":"valid","contact":[]}'
expected_order = (
    '{"status":"pending",'
    '"identifiers":[{"type":"dns","value":"hfoc-appliance"}],'
    '"authorizations":["https://10.0.2.2:%d/acme/authz/1"],'
    '"finalize":"https://10.0.2.2:%d/acme/finalize/1"}'
) % (port, port)
expected_authz = (
    '{"status":"pending",'
    '"identifier":{"type":"dns","value":"hfoc-appliance"},'
    '"challenges":[{"type":"http-01",'
    '"url":"https://10.0.2.2:%d/acme/chall/1",'
    '"status":"pending","token":"tok3n-ABC_123"}]}'
) % port
expected_challenge = (
    '{"type":"http-01","url":"https://10.0.2.2:%d/acme/chall/1",'
    '"status":"processing","token":"tok3n-ABC_123"}'
) % port
expected_challenge_poll = (
    '{"type":"http-01","url":"https://10.0.2.2:%d/acme/chall/1",'
    '"status":"valid","token":"tok3n-ABC_123"}'
) % port
expected_finalize = (
    '{"status":"valid",'
    '"identifiers":[{"type":"dns","value":"hfoc-appliance"}],'
    '"authorizations":["https://10.0.2.2:%d/acme/authz/1"],'
    '"finalize":"https://10.0.2.2:%d/acme/finalize/1",'
    '"certificate":"https://10.0.2.2:%d/acme/cert/1"}'
) % (port, port, port)

expected = {
    0: (200, expected_directory),
    1: (200, ""),
    2: (201, expected_account),
    3: (201, expected_order),
    4: (200, expected_authz),
    5: (200, expected_challenge),
    6: (200, expected_challenge_poll),
    7: (200, expected_order),
    8: (400, None),
    9: (200, expected_finalize),
    10: (200, None),
    11: (404, ""),
}

failures = 0


def fail(message):
    global failures
    failures += 1
    print("FAIL: %s" % message, file=sys.stderr)


def read_response(path):
    raw = open(path, "rb").read()
    head, separator, body = raw.partition(b"\r\n\r\n")
    if not separator:
        fail("%s: no header terminator" % path)
        return None, {}, b""
    lines = head.split(b"\r\n")
    headers = {}
    for line in lines[1:]:
        name, _, value = line.partition(b":")
        headers[name.strip().lower().decode("ascii", "replace")] = (
            value.strip().decode("ascii", "replace")
        )
    return lines[0].decode("ascii", "replace"), headers, body


blocks = re.findall(
    r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----",
    chain_dump,
    re.DOTALL,
)
expected_chain = ""
if len(blocks) >= 2:
    expected_chain = "\n".join(block.strip() for block in blocks[:2]) + "\n"

for index, (name, path) in enumerate(zip(names, paths)):
    status_line, headers, body = read_response(path)
    want_status, want_body = expected[index]
    if not status_line.startswith("HTTP/1.1 %d " % want_status):
        fail("%s: status %r, expected %d" % (name, status_line, want_status))
    if "replay-nonce" not in headers or not headers["replay-nonce"]:
        fail("%s: no Replay-Nonce header" % name)
    if "content-length" not in headers or int(headers["content-length"]) != len(body):
        fail("%s: Content-Length does not match the body" % name)
    text = body.decode("ascii", "replace")
    if want_body is not None and text != want_body:
        fail("%s: body differs from the declared document" % name)
        print("expected: %r" % want_body, file=sys.stderr)
        print("received: %r" % text, file=sys.stderr)

_, account_headers, _ = read_response(paths[2])
if account_headers.get("location") != (
    "https://10.0.2.2:%d/acct/1" % port
):
    fail("POST /acme/new-account: Location %r" % account_headers.get("location"))

_, order_headers, _ = read_response(paths[3])
if order_headers.get("location") != (
    "https://10.0.2.2:%d/acme/order/1" % port
):
    fail("POST /acme/new-order: Location %r" % order_headers.get("location"))

_, cert_headers, cert_body = read_response(paths[10])
if cert_headers.get("content-type") != "application/pem-certificate-chain":
    fail("POST /acme/cert/1: Content-Type %r" % cert_headers.get("content-type"))
if expected_chain == "":
    fail("the pinned chain has fewer than two certificates")
elif cert_body.decode("ascii", "replace") != expected_chain:
    fail("POST /acme/cert/1: body is not the fixture chain")

# ITEM-508 increment 22: the refusal names what the finalize lacked.
_, _, refusal_body = read_response(paths[8])
if b"csr" not in refusal_body:
    fail("POST /acme/finalize/1 (no csr): the refusal does not name the csr")
    print("received: %r" % refusal_body, file=sys.stderr)

if failures:
    sys.exit(1)
PY

# The stub's own log names the refusal reason, so a silent 400 cannot pass.
grep -Fq 'ACME-STUB refusal: the finalize payload carries no csr' \
    "$STUB_LOG" || {
    echo "The stub did not name the missing-CSR refusal" >&2
    cat "$STUB_LOG" >&2 || true
    exit 1
}

# No proof may print key material.
if grep -Fq 'PRIVATE KEY' "$STUB_LOG"; then
    echo "The stub log carries private key material" >&2
    exit 1
fi

echo "ACME directory stub host proof: OK (port=$PORT)"
