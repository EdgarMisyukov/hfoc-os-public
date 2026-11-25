#!/usr/bin/env bash
# Prove item 297: host-only certificate/key fail-closed matrix through the
# pinned mbedTLS manifest parser and the HTTPS service start gate.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in bash grep make; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

MANIFEST_TEST=tests/mbedtls-manifest-host.c
SERVICE_TEST=tests/https-service-descriptor-host.c

require_fixture() {
    local file=$1
    local pattern=$2

    if ! grep -Fq -- "$pattern" "$file"; then
        echo "Certificate/key fail-closed matrix missing fixture: $pattern" >&2
        exit 1
    fi
}

require_fixture "$MANIFEST_TEST" \
    "manifest parse rejects null bytes"
require_fixture "$MANIFEST_TEST" \
    "manifest parse rejects zero-length blob"
require_fixture "$MANIFEST_TEST" \
    "manifest parse rejects truncated certificate DER"
require_fixture "$MANIFEST_TEST" \
    "manifest parse rejects truncated key DER"
require_fixture "$MANIFEST_TEST" \
    "manifest parse rejects digest mismatch"
require_fixture "$MANIFEST_TEST" \
    "manifest parse rejects zero purpose"
require_fixture "$MANIFEST_TEST" \
    "manifest parse rejects wrong hostname bytes"
require_fixture "$MANIFEST_TEST" \
    "manifest parse rejects certificate without hfoc.test SAN"
require_fixture "$MANIFEST_TEST" \
    "manifest parse rejects zero algorithm"
require_fixture "$MANIFEST_TEST" \
    "manifest parse rejects mismatched private key"
require_fixture "$SERVICE_TEST" \
    "missing certificate readiness prevents HTTPS start"

make -s test-mbedtls-manifest-host
make -s test-https-service-descriptor-host

manifest_output=$(build/test-mbedtls-manifest-host)
service_output=$(build/test-https-service-descriptor-host)

if ! grep -Fq "mbedTLS manifest parser host tests: ALL OK" \
        <<<"$manifest_output"; then
    echo "Certificate/key fail-closed matrix: manifest parser test failed" >&2
    exit 1
fi
if ! grep -Fq "[CERTIFICATE-FAIL-CLOSED] matrix ok" \
        <<<"$manifest_output"; then
    echo "Certificate/key fail-closed matrix: matrix ok marker missing" >&2
    exit 1
fi
if ! grep -Fq "HTTPS service descriptor host test: ALL OK" \
        <<<"$service_output"; then
    echo "Certificate/key fail-closed matrix: HTTPS start gate test failed" >&2
    exit 1
fi

echo "Absent certificate/key fixture: found"
echo "Malformed DER fixture: found"
echo "Digest-mismatch fixture: found"
echo "Wrong-purpose fixture: found"
echo "Hostname-mismatch fixture: found"
echo "Algorithm-incompatible fixture: found"
echo "Key-mismatch fixture: found"
echo "HTTPS start gate denies missing certificate readiness: found"
echo "Valid manifest and complete readiness allow HTTPS start: found"
echo "Certificate/key fail-closed matrix: OK"
