#!/usr/bin/env bash
# ITEM-380: the boot-chain manifest signature must be deterministic (RFC 6979)
# so that two builds of the same inputs produce identical manifest bytes.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

SCRIPT="$ROOT/scripts/hfoc-boot-chain-manifest.py"
KEY="$ROOT/boot/keys/hfoc-development-signing-key.pem"

TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT

fail() {
    echo "boot-chain determinism: FAIL: $*" >&2
    exit 1
}

command -v openssl >/dev/null 2>&1 || fail "openssl is required"
test -f "$SCRIPT" || fail "missing $SCRIPT"
test -f "$KEY" || fail "missing $KEY"

printf 'fixture kernel payload\n' >"$TEST_DIR/hfoc-kernel.elf"
printf 'fixture limine configuration\n' >"$TEST_DIR/limine.conf"

sign() {
    PYTHONHASHSEED=0 python3 "$SCRIPT" \
        --kernel "$TEST_DIR/hfoc-kernel.elf" \
        --config "$TEST_DIR/limine.conf" \
        --release-id 1 --signer-id 1 \
        --key "$1" --output "$2"
}

# 1. Two signings of the same inputs must be byte-identical.
sign "$KEY" "$TEST_DIR/first.bin" >/dev/null
sign "$KEY" "$TEST_DIR/second.bin" >/dev/null
cmp -s "$TEST_DIR/first.bin" "$TEST_DIR/second.bin" || \
    fail "two signings of identical inputs differ"
[[ "$(stat -c %s "$TEST_DIR/first.bin")" == 352 ]] || \
    fail "envelope is not the expected 352 bytes"

# 2. The PKCS#8 encoding of the same key must produce the same signature.
openssl pkcs8 -topk8 -nocrypt -in "$KEY" -out "$TEST_DIR/key-pkcs8.pem"
sign "$TEST_DIR/key-pkcs8.pem" "$TEST_DIR/pkcs8.bin" >/dev/null
cmp -s "$TEST_DIR/first.bin" "$TEST_DIR/pkcs8.bin" || \
    fail "PKCS#8 key produced a different signature"

# Extract the manifest body [32, 256) and the raw r || s signature [256, 320).
python3 - "$TEST_DIR/first.bin" "$TEST_DIR/body.bin" \
    "$TEST_DIR/signature.raw" <<'PY'
import sys

envelope = open(sys.argv[1], "rb").read()
assert len(envelope) == 352, len(envelope)
open(sys.argv[2], "wb").write(envelope[32:256])
open(sys.argv[3], "wb").write(envelope[256:320])
PY

# Convert the raw r || s pair into the DER SEQUENCE of two INTEGERs that
# openssl expects.
python3 - "$TEST_DIR/signature.raw" "$TEST_DIR/signature.der" <<'PY'
import sys

raw = open(sys.argv[1], "rb").read()
assert len(raw) == 64, len(raw)


def der_integer(value):
    value = value.lstrip(b"\x00") or b"\x00"
    if value[0] & 0x80:
        value = b"\x00" + value
    return b"\x02" + bytes([len(value)]) + value


body = der_integer(raw[:32]) + der_integer(raw[32:])
open(sys.argv[2], "wb").write(b"\x30" + bytes([len(body)]) + body)
PY

# 3. openssl must accept the signature over the manifest body.
openssl pkey -in "$KEY" -pubout -out "$TEST_DIR/public.pem" 2>/dev/null
openssl dgst -sha256 -verify "$TEST_DIR/public.pem" \
    -signature "$TEST_DIR/signature.der" "$TEST_DIR/body.bin" \
    >"$TEST_DIR/verify.out" 2>&1 || fail "openssl rejected the signature"
grep -Fq 'Verified OK' "$TEST_DIR/verify.out" || \
    fail "unexpected openssl verification output"

# 4. A tampered body must fail verification.
python3 - "$TEST_DIR/body.bin" "$TEST_DIR/body-tampered.bin" <<'PY'
import sys

body = bytearray(open(sys.argv[1], "rb").read())
body[12] ^= 0x01
open(sys.argv[2], "wb").write(bytes(body))
PY
if openssl dgst -sha256 -verify "$TEST_DIR/public.pem" \
    -signature "$TEST_DIR/signature.der" "$TEST_DIR/body-tampered.bin" \
    >/dev/null 2>&1; then
    fail "tampered manifest body verified"
fi

# 5. RFC 6979 P-256/SHA-256 known-answer test (RFC 6979 appendix A.2.5).
python3 - "$SCRIPT" <<'PY'
import hashlib
import importlib.util
import sys

spec = importlib.util.spec_from_file_location(
    "hfoc_boot_chain_manifest", sys.argv[1])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

private_key = 0xC9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721
vectors = (
    (b"sample",
     "efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716"
     "f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8"),
    (b"test",
     "f1abb023518351cd71d881567b1ea663ed3efcf6c5132b354f28d3b0b7d38367"
     "019f4113742a2b14bd25926b49c649155f267e60d3814b4c0cc84250e46f0083"),
)
for message, expected in vectors:
    actual = module.ecdsa_sign(
        private_key, hashlib.sha256(message).digest()).hex()
    assert actual == expected, (
        "RFC 6979 vector mismatch for %r: %s" % (message, actual))
PY

# 6. A malformed key must fail closed, never fall back to random signing.
printf -- '-----BEGIN EC PRIVATE KEY-----\nnot-a-key\n-----END EC PRIVATE KEY-----\n' \
    >"$TEST_DIR/bad-key.pem"
if sign "$TEST_DIR/bad-key.pem" "$TEST_DIR/bad.bin" \
    >"$TEST_DIR/bad.out" 2>&1; then
    fail "malformed private key was accepted"
fi
grep -Fq 'cannot parse private key' "$TEST_DIR/bad.out" || \
    fail "malformed private key error is not clear"

echo "boot-chain determinism: OK"
