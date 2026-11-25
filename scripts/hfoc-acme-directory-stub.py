#!/usr/bin/env python3
"""ITEM-508 increments 15, 17 and 22: the offline ACME stub.

The guest proofs (fixture kernels dialling `10.0.2.2:<port>` through the real
lwIP + mbedTLS binding of increment 13) need a peer that speaks TLS 1.3 only
and serves the ACME documents of a canned order over a chain the guest's
fixture anchor verifies for the literal address.  This stub is that peer.

Increment 15 needed one document: `GET /directory`.  Increment 17 adds the
endpoints of one complete order - `new-nonce`, `new-account`, `new-order`, the
authorization and its `http-01` challenge, `finalize` and the certificate
download - each with a real `Replay-Nonce` header, so the guest's order flow
can be driven end to end without a network.  Increment 22 makes `finalize`
check what RFC 8555 section 7.4 requires: the JWS payload must carry a
base64url PKCS#10, its DER must parse, its self-signature must verify under
`openssl req -verify` and its subject must be the ordered name.  Anything else
is a named refusal, never a silent acceptance.

The stub verifies nothing beyond that: it is the fixed counterpart of one
fixed order, not a certificate authority.

The bytes have one source of truth: `tests/appliance-acme-directory-fixtures.h`
(the increment-14 fixture chain, whose certificates the host proof pins by
SHA-256).  The DER arrays are parsed here, converted to PEM in memory and
written to a private temporary directory only because python's `ssl` API takes
file paths.  The private key never reaches a log.
"""

import argparse
import base64
import json
import os
import re
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIXTURES = os.path.join(ROOT, "tests", "appliance-acme-directory-fixtures.h")

DIRECTORY_PATH = "/directory"
REQUEST_MAX = 8192
# The one name this canned order is for; the CSR subject must agree with it.
ORDER_IDENTIFIER = "hfoc-appliance"

_ARRAY = re.compile(
    r"static\s+const\s+unsigned\s+char\s+(\w+)\s*\[\s*\d*U?\s*\]\s*=\s*\{(.*?)\};",
    re.DOTALL,
)
_BYTE = re.compile(r"0x([0-9A-Fa-f]{1,2})U?")


def read_arrays(path):
    """Return every `static const unsigned char name[] = { ... };` as bytes."""
    with open(path, "r", encoding="ascii") as handle:
        text = handle.read()
    arrays = {}
    for name, body in _ARRAY.findall(text):
        arrays[name] = bytes(int(value, 16) for value in _BYTE.findall(body))
    return arrays


def pem_block(der, label):
    encoded = base64.b64encode(der).decode("ascii")
    lines = [encoded[offset:offset + 64] for offset in range(0, len(encoded), 64)]
    return "-----BEGIN %s-----\n%s\n-----END %s-----\n" % (
        label,
        "\n".join(lines),
        label,
    )


def directory_body(port):
    return (
        '{"newNonce":"https://10.0.2.2:%d/acme/new-nonce",'
        '"newAccount":"https://10.0.2.2:%d/acme/new-account",'
        '"newOrder":"https://10.0.2.2:%d/acme/new-order"}'
    ) % (port, port, port)


def account_body():
    return '{"status":"valid","contact":[]}'


def order_body(port):
    return (
        '{"status":"pending",'
        '"identifiers":[{"type":"dns","value":"hfoc-appliance"}],'
        '"authorizations":["https://10.0.2.2:%d/acme/authz/1"],'
        '"finalize":"https://10.0.2.2:%d/acme/finalize/1"}'
    ) % (port, port)


def authorization_body(port):
    return (
        '{"status":"pending",'
        '"identifier":{"type":"dns","value":"hfoc-appliance"},'
        '"challenges":[{"type":"http-01",'
        '"url":"https://10.0.2.2:%d/acme/chall/1",'
        '"status":"pending","token":"tok3n-ABC_123"}]}'
    ) % port


def challenge_body(port, status):
    return (
        '{"type":"http-01","url":"https://10.0.2.2:%d/acme/chall/1",'
        '"status":"%s","token":"tok3n-ABC_123"}'
    ) % (port, status)


def finalized_order_body(port):
    return (
        '{"status":"valid",'
        '"identifiers":[{"type":"dns","value":"hfoc-appliance"}],'
        '"authorizations":["https://10.0.2.2:%d/acme/authz/1"],'
        '"finalize":"https://10.0.2.2:%d/acme/finalize/1",'
        '"certificate":"https://10.0.2.2:%d/acme/cert/1"}'
    ) % (port, port, port)


def response_bytes(status, reason, content_type, body, nonce, location):
    head = "HTTP/1.1 %d %s\r\n" % (status, reason)
    if content_type is not None:
        head += "Content-Type: %s\r\n" % content_type
    if location is not None:
        head += "Location: %s\r\n" % location
    if nonce is not None:
        head += "Replay-Nonce: %s\r\n" % nonce
    head += "Content-Length: %d\r\nConnection: close\r\n\r\n" % len(body)
    return head.encode("ascii") + body


def read_request(connection):
    received = b""
    while b"\r\n\r\n" not in received and len(received) < REQUEST_MAX:
        chunk = connection.recv(1024)
        if not chunk:
            break
        received += chunk
    head, separator, body = received.partition(b"\r\n\r\n")
    if not separator:
        return received
    length = 0
    for line in head.split(b"\r\n")[1:]:
        name, _, value = line.partition(b":")
        if name.strip().lower() == b"content-length":
            try:
                length = int(value.strip())
            except ValueError:
                length = 0
    while len(body) < length and len(received) < REQUEST_MAX:
        chunk = connection.recv(1024)
        if not chunk:
            break
        received += chunk
        body = received.partition(b"\r\n\r\n")[2]
    return received


def decode_base64url(text):
    """Unpadded base64url, the JWS alphabet; anything else raises."""
    if not isinstance(text, str) or text == "":
        raise ValueError("empty base64url value")
    return base64.urlsafe_b64decode(text + "=" * (-len(text) % 4))


def finalize_csr_refusal(request_body, scratch_dir):
    """Return None when the finalize carries a verifiable PKCS#10.

    The JWS envelope is not signature-checked here (the host proof owns the
    account key); what is checked is what RFC 8555 section 7.4 requires of the
    payload: a base64url PKCS#10 whose DER parses, whose self-signature
    verifies and whose subject is the ordered name.
    """
    try:
        envelope = json.loads(request_body.decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        return "the finalize body is not a JWS"
    payload = envelope.get("payload")
    if not isinstance(payload, str):
        return "the JWS carries no payload"
    try:
        document = json.loads(decode_base64url(payload).decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        return "the JWS payload is not base64url JSON"
    csr = document.get("csr")
    if not isinstance(csr, str) or csr == "":
        return "the finalize payload carries no csr"
    try:
        der = decode_base64url(csr)
    except ValueError:
        return "the csr is not base64url"
    if len(der) < 2 or der[0] != 0x30:
        return "the csr is not a DER SEQUENCE"
    path = os.path.join(scratch_dir, "finalize-csr.der")
    with open(path, "wb") as handle:
        handle.write(der)
    try:
        result = subprocess.run(
            [
                "openssl",
                "req",
                "-inform",
                "DER",
                "-in",
                path,
                "-noout",
                "-verify",
                "-subject",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=10,
            check=False,
        )
    except (OSError, subprocess.SubprocessError) as error:
        return "the csr could not be verified: %s" % error
    if result.returncode != 0:
        return "the csr signature does not verify"
    subject = result.stdout.decode("ascii", "replace")
    if "CN" not in subject or ORDER_IDENTIFIER not in subject:
        return "the csr subject is not the ordered name"
    return None


class StubState:
    """One fixed order and the monotone nonce every reply carries."""

    def __init__(self, port, chain_pem, scratch_dir):
        self.port = port
        self.chain_pem = chain_pem
        self.scratch_dir = scratch_dir
        self.replies = 0
        self.challenge_answers = 0
        self.refusals = []

    def next_nonce(self):
        self.replies += 1
        return "fixture-nonce-%d" % self.replies

    def route(self, method, target, body=b""):
        """Return (status, reason, content_type, body, location)."""
        if method == "GET" and target == DIRECTORY_PATH:
            return (
                200,
                "OK",
                "application/json",
                directory_body(self.port).encode("ascii"),
                None,
            )
        if method == "POST" and target == "/acme/new-nonce":
            return 200, "OK", "application/json", b"", None
        if method == "POST" and target == "/acme/new-account":
            return (
                201,
                "Created",
                "application/json",
                account_body().encode("ascii"),
                "https://10.0.2.2:%d/acct/1" % self.port,
            )
        if method == "POST" and target == "/acme/new-order":
            # ITEM-508 increment 19: the 201 names the order URL, which the
            # finalize poll then reads with POST-as-GET (RFC 8555 section 7.4).
            return (
                201,
                "Created",
                "application/json",
                order_body(self.port).encode("ascii"),
                "https://10.0.2.2:%d/acme/order/1" % self.port,
            )
        if method == "POST" and target == "/acme/authz/1":
            return (
                200,
                "OK",
                "application/json",
                authorization_body(self.port).encode("ascii"),
                None,
            )
        if method == "POST" and target == "/acme/chall/1":
            # ITEM-508 increment 18: the first answer is `processing` so the
            # guest's poll path is exercised; every later answer is `valid`.
            self.challenge_answers += 1
            status = "processing" if self.challenge_answers == 1 else "valid"
            return (
                200,
                "OK",
                "application/json",
                challenge_body(self.port, status).encode("ascii"),
                None,
            )
        if method == "POST" and target == "/acme/finalize/1":
            # ITEM-508 increment 22: the payload must carry the PKCS#10 the CA
            # validates; a body without one is refused by name, never accepted.
            refusal = finalize_csr_refusal(body, self.scratch_dir)
            if refusal is not None:
                self.refusals.append(refusal)
                print(
                    "ACME-STUB refusal: %s" % refusal,
                    file=sys.stderr,
                    flush=True,
                )
                return (
                    400,
                    "Bad Request",
                    "application/json",
                    (
                        '{"type":"urn:ietf:params:acme:error:malformed",'
                        '"detail":"%s"}' % refusal
                    ).encode("ascii"),
                    None,
                )
            # ITEM-508 increment 19: the CA accepted the finalize but is still
            # issuing, so the answer is the still-pending order; the guest
            # polls the order URL below for the finalized one.
            return (
                200,
                "OK",
                "application/json",
                order_body(self.port).encode("ascii"),
                None,
            )
        if method == "POST" and target == "/acme/order/1":
            # The order URL from the newOrder Location: the poll reads it and
            # finds the finalized order with its certificate URL.
            return (
                200,
                "OK",
                "application/json",
                finalized_order_body(self.port).encode("ascii"),
                None,
            )
        if method == "POST" and target == "/acme/cert/1":
            return (
                200,
                "OK",
                "application/pem-certificate-chain",
                self.chain_pem,
                None,
            )
        return 404, "Not Found", "text/plain", b"", None


def serve_connection(connection, state):
    request = read_request(connection)
    head, _, request_body = request.partition(b"\r\n\r\n")
    first_line = head.split(b"\r\n", 1)[0]
    parts = first_line.split(b" ")
    if len(parts) == 3:
        method, target, version = (
            part.decode("ascii", "replace") for part in parts
        )
    else:
        method, target, version = "", "", ""
    nonce = state.next_nonce()
    if version not in ("HTTP/1.0", "HTTP/1.1"):
        status, reason, content_type, body, location = (
            400,
            "Bad Request",
            "text/plain",
            b"",
            None,
        )
    else:
        status, reason, content_type, body, location = state.route(
            method, target, request_body
        )
    connection.sendall(
        response_bytes(status, reason, content_type, body, nonce, location)
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument(
        "--requests",
        type=int,
        default=0,
        help="stop after this many connections (0 serves until killed)",
    )
    arguments = parser.parse_args()
    if not 1 <= arguments.port <= 65535:
        parser.error("--port out of range")

    arrays = read_arrays(FIXTURES)
    try:
        ca = arrays["directory_fixture_ca"]
        leaf = arrays["directory_fixture_leaf"]
        key = arrays["directory_fixture_leaf_key"]
    except KeyError as missing:
        print("Missing fixture array: %s" % missing, file=sys.stderr)
        return 1
    chain_pem = (
        pem_block(leaf, "CERTIFICATE") + pem_block(ca, "CERTIFICATE")
    ).encode("ascii")
    private = tempfile.mkdtemp(prefix="hfoc-acme-stub-")
    csr_scratch = os.path.join(private, "csr")
    os.mkdir(csr_scratch)
    state = StubState(arguments.port, chain_pem, csr_scratch)

    try:
        chain_path = os.path.join(private, "chain.pem")
        key_path = os.path.join(private, "leaf-key.pem")
        with open(chain_path, "w", encoding="ascii") as handle:
            handle.write(chain_pem.decode("ascii"))
        with open(key_path, "w", encoding="ascii") as handle:
            handle.write(pem_block(key, "EC PRIVATE KEY"))
        os.chmod(key_path, 0o600)

        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        # The frozen guest profile carries TLS 1.3 only, so offering anything
        # else would prove a handshake the appliance cannot perform.
        context.minimum_version = ssl.TLSVersion.TLSv1_3
        context.maximum_version = ssl.TLSVersion.TLSv1_3
        context.load_cert_chain(chain_path, key_path)

        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("0.0.0.0", arguments.port))
        listener.listen(4)
        print("ACME-STUB listening port=%d" % arguments.port, flush=True)

        served = 0
        while arguments.requests == 0 or served < arguments.requests:
            try:
                connection, _ = listener.accept()
            except OSError:
                break
            try:
                with context.wrap_socket(connection, server_side=True) as tls:
                    serve_connection(tls, state)
                    try:
                        tls.unwrap()
                    except (ssl.SSLError, OSError):
                        pass
            except (ssl.SSLError, OSError) as error:
                print("ACME-STUB connection failed: %s" % error, file=sys.stderr)
            served += 1
        listener.close()
    finally:
        shutil.rmtree(private, ignore_errors=True)
    return 0


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    sys.exit(main())
