#!/usr/bin/env bash
# ITEM-493: the compiled-in test certificate must not rot into an expired state.
#
# Every HTTPS QEMU proof curls the appliance with the certificate embedded in
# `kernel/src/hfoc_mbedtls_test_secret.c`, and curl verifies it against the
# current time.  The certificate that expired on 2026-09-28 07:42 UTC did not
# break one proof: it turned every PR whose acceptance phase runs those proofs
# red, hours after the fact, with the cause buried in a guest log
# (`curl: (60) SSL certificate problem: certificate has expired`).
#
# This check reads the same bytes the kernel carries and requires them to stay
# valid for the whole window below, so a renewal that is too short - or a
# forgotten one - fails here, in the host suite, where the cause is obvious.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

# One year, in seconds: the certificate must outlive any plausible release cycle
# and every CI run in it.
MIN_SECONDS=${HFOC_TEST_SECRET_MIN_SECONDS:-31536000}

for command in python3 openssl; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT

python3 - "$TEST_DIR" <<'PY'
import pathlib
import re
import sys

source = pathlib.Path("kernel/src/hfoc_mbedtls_test_secret.c").read_text(
    encoding="ascii")
directory = pathlib.Path(sys.argv[1])
for name, out in (("cert_der", directory / "cert.der"),
                  ("key_der", directory / "key.der")):
    marker = "hfoc_mbedtls_test_secret_" + name
    if marker not in source:
        raise SystemExit(f"the kernel does not carry {marker}")
    start = source.index(marker)
    body = source[start:source.index("};", start)]
    values = [int(value, 16) for value in re.findall(r"0x[0-9a-fA-F]{2}", body)]
    if not values:
        raise SystemExit(f"{marker} carries no bytes")
    out.write_bytes(bytes(values))
PY

openssl x509 -inform DER -in "$TEST_DIR/cert.der" -out "$TEST_DIR/cert.pem" \
    || {
    echo "the embedded test certificate is not a certificate" >&2
    exit 1
}
subject=$(openssl x509 -in "$TEST_DIR/cert.pem" -noout -subject)
case "$subject" in
*CN*=hfoc.test*) ;;
*)
    echo "the embedded test certificate is not the hfoc.test certificate: $subject" >&2
    exit 1
    ;;
esac

if ! openssl x509 -in "$TEST_DIR/cert.pem" -noout -checkend "$MIN_SECONDS" \
    >/dev/null; then
    echo "the embedded test certificate expires within $MIN_SECONDS seconds:" >&2
    openssl x509 -in "$TEST_DIR/cert.pem" -noout -subject -dates >&2
    echo "renew it (ITEM-493) and update the digests in" \
        "kernel/include/hfoc_mbedtls_test_secret.h in the same commit" >&2
    exit 1
fi

cert_der=$(python3 - "$TEST_DIR/cert.der" <<'PY'
import hashlib
import pathlib
import sys
print(hashlib.sha256(pathlib.Path(sys.argv[1]).read_bytes()).hexdigest())
PY
)
key_der=$(python3 - "$TEST_DIR/key.der" <<'PY'
import hashlib
import pathlib
import sys
print(hashlib.sha256(pathlib.Path(sys.argv[1]).read_bytes()).hexdigest())
PY
)
header=$(sed -n 's/^#define HFOC_MBEDTLS_TEST_SECRET_CERT_DIGEST "\(.*\)"/\1/p' \
    kernel/include/hfoc_mbedtls_test_secret.h)
key_header=$(sed -n 's/^#define HFOC_MBEDTLS_TEST_SECRET_KEY_DIGEST "\(.*\)"/\1/p' \
    kernel/include/hfoc_mbedtls_test_secret.h)
if [[ "$cert_der" != "$header" || "$key_der" != "$key_header" ]]; then
    echo "the embedded secret does not match the digests the header declares:" >&2
    echo "  certificate: kernel=$cert_der header=$header" >&2
    echo "  key:         kernel=$key_der header=$key_header" >&2
    exit 1
fi

echo "mbedTLS test certificate: valid for more than $MIN_SECONDS seconds," \
    "digests match the header"
openssl x509 -in "$TEST_DIR/cert.pem" -noout -subject -dates
