#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

HEADER="$ROOT/kernel/include/hfoc_mbedtls_test_secret.h"
[[ -f "$HEADER" ]] || {
    echo "Missing mbedTLS test-secret header: $HEADER" >&2
    exit 1
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
    echo "mbedTLS test-secret header has invalid scanner constants" >&2
    exit 1
}

usage() {
    echo "Usage: verify-mbedtls-test-secret.sh MODE ARTIFACT..." >&2
    echo "MODE: --production | --test | --release-manifest" >&2
    exit 2
}

fail() {
    echo "mbedTLS test-secret scan: $*" >&2
    exit 1
}

is_elf() {
    file "$1" | grep -q 'ELF'
}

has_elf_symbols() {
    nm -a "$1" 2>/dev/null | grep -F 'hfoc_mbedtls_test_secret_' \
        >/dev/null
}

mode=
artifacts=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --production|--test|--release-manifest)
            mode=$1
            ;;
        -*)
            usage
            ;;
        *)
            artifacts+=("$1")
            ;;
    esac
    shift
done

[[ -n "$mode" && ${#artifacts[@]} -gt 0 ]] || usage

case "$mode" in
    --production)
        for artifact in "${artifacts[@]}"; do
            [[ -f "$artifact" ]] || fail "missing production artifact: $artifact"
            if grep -aF -e "$marker" -e "$cert_digest" -e "$key_digest" \
                    "$artifact" >/dev/null 2>&1; then
                fail "production artifact contains test-secret evidence: $artifact"
            fi
            if is_elf "$artifact" && has_elf_symbols "$artifact"; then
                fail "production ELF contains test-secret symbols: $artifact"
            fi
        done
        echo "Production mbedTLS test-secret scan: OK"
        ;;
    --test)
        for artifact in "${artifacts[@]}"; do
            [[ -f "$artifact" ]] || fail "missing test artifact: $artifact"
            grep -aF -e "$marker" "$artifact" >/dev/null 2>&1 ||
                fail "test artifact lacks test-secret marker: $artifact"
            grep -aF -e "$cert_digest" -e "$key_digest" \
                "$artifact" >/dev/null 2>&1 ||
                fail "test artifact lacks test-secret digests: $artifact"
            if is_elf "$artifact" && ! has_elf_symbols "$artifact"; then
                fail "test ELF lacks test-secret symbols: $artifact"
            fi
        done
        echo "Test mbedTLS test-secret image scan: OK"
        ;;
    --release-manifest)
        for artifact in "${artifacts[@]}"; do
            [[ -f "$artifact" ]] || fail "missing release manifest: $artifact"
            if grep -aF -e "$marker" -e "$cert_digest" -e "$key_digest" \
                    -e 'test_secret_cert_digest=' \
                    -e 'test_secret_key_digest=' \
                    "$artifact" >/dev/null 2>&1; then
                fail "release manifest contains test-secret evidence: $artifact"
            fi
        done
        echo "Release manifest mbedTLS test-secret scan: OK"
        ;;
esac
