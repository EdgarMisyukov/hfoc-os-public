#!/usr/bin/env bash
# Scan production artifacts for test-secret, TLS, and proof-control evidence.
# [HTTPS-RESTART-SOAK] proof controls are forbidden in production artifacts.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

HEADER="$ROOT/kernel/include/hfoc_mbedtls_test_secret.h"
PROFILE="$ROOT/scripts/production-artifact-secret-gate-profile.txt"

for command in grep file nm; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

usage() {
    echo "Usage: verify-production-artifact-secret-gate.sh TYPE ARTIFACT..." >&2
    echo "TYPE: kernel|iso|release-manifest|serial|audit-snapshot|metrics|support-bundle" >&2
    exit 2
}

fail() {
    echo "Production artifact secret gate: $*" >&2
    exit 1
}

is_elf() {
    file "$1" | grep -q 'ELF'
}

has_test_secret_symbols() {
    nm -a "$1" 2>/dev/null | grep -F 'hfoc_mbedtls_test_secret_' \
        >/dev/null
}

[[ -f "$HEADER" ]] || {
    fail "missing mbedTLS test-secret header: $HEADER"
}
[[ -f "$PROFILE" ]] || {
    fail "missing production artifact secret gate profile: $PROFILE"
}

marker=$(sed -n \
    's/^#define HFOC_MBEDTLS_TEST_SECRET_MARKER "\(.*\)"/\1/p' \
    "$HEADER")
cert_digest=$(sed -n \
    's/^#define HFOC_MBEDTLS_TEST_SECRET_CERT_DIGEST "\(.*\)"/\1/p' \
    "$HEADER")
key_digest=$(sed -n \
    's/^#define HFOC_MBEDTLS_TEST_SECRET_KEY_DIGEST "\(.*\)"/\1/p' \
    "$HEADER")
[[ -n "$marker" && -n "$cert_digest" && -n "$key_digest" ]] || {
    fail "mbedTLS test-secret header has invalid scanner constants"
}

patterns=()
while IFS= read -r line; do
    case "$line" in
        ''|\#*)
            continue
            ;;
    esac
    patterns+=("$line")
done <"$PROFILE"
patterns+=(
    "$marker"
    "$cert_digest"
    "$key_digest"
    'test_secret_cert_digest='
    'test_secret_key_digest='
)

artifact_type=${1:-}
shift || usage
case "$artifact_type" in
    kernel|iso|release-manifest|serial|audit-snapshot|metrics|support-bundle)
        ;;
    *)
        usage
        ;;
esac
[[ $# -gt 0 ]] || usage

for artifact in "$@"; do
    [[ -f "$artifact" ]] || {
        fail "missing artifact: $artifact"
    }
    grep_args=()
    for pattern in "${patterns[@]}"; do
        grep_args+=(-e "$pattern")
    done
    if grep -aF "${grep_args[@]}" -- "$artifact" >/dev/null 2>&1; then
        fail "artifact contains forbidden production evidence: $artifact"
    fi
    if [[ "$artifact_type" == "kernel" ]] && is_elf "$artifact" \
            && has_test_secret_symbols "$artifact"; then
        fail "kernel artifact contains test-secret symbols: $artifact"
    fi
done

echo "Production artifact secret gate [$artifact_type] scan: OK"
echo "Production artifact secret gate: OK"
