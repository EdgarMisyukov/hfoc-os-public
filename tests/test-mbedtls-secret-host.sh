#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

VERSION=$(cat "$ROOT/VERSION")
PRODUCTION_KERNEL="$ROOT/build/hfoc-kernel.elf"
PRODUCTION_IMAGE="$ROOT/build/hfoc-os-${VERSION}.iso"
TEST_KERNEL="$ROOT/build/mbedtls-test-secret/hfoc-kernel.elf"
TEST_IMAGE="$ROOT/build/mbedtls-test-secret/hfoc-os-${VERSION}-mbedtls-test-secret.iso"
VERIFY="$ROOT/scripts/verify-mbedtls-test-secret.sh"
HEADER="$ROOT/kernel/include/hfoc_mbedtls_test_secret.h"
TEST_DIR=$(mktemp -d)

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    rm -rf "$TEST_DIR"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

for artifact in \
    "$PRODUCTION_KERNEL" \
    "$PRODUCTION_IMAGE" \
    "$TEST_KERNEL" \
    "$TEST_IMAGE"; do
    [[ -f "$artifact" ]] || {
        echo "Missing test-secret artifact: $artifact" >&2
        exit 1
    }
done

expect_fail() {
    local label=$1
    shift
    if "$@" >/dev/null 2>&1; then
        echo "$label unexpectedly succeeded" >&2
        exit 1
    fi
}

bash "$VERIFY" --production "$PRODUCTION_KERNEL" "$PRODUCTION_IMAGE"
bash "$VERIFY" --test "$TEST_KERNEL" "$TEST_IMAGE"

mkdir -p "$TEST_DIR/artifacts"
printf 'test-update\n' >"$TEST_DIR/artifacts/update.bundle"
printf 'test-schema\n' >"$TEST_DIR/artifacts/support-schema.txt"
printf 'test-licenses\n' >"$TEST_DIR/artifacts/licenses.sha256"
printf 'test-recovery\n' >"$TEST_DIR/artifacts/recovery.iso"

export HFOC_RELEASE_ID=HFOC-APPLIANCE-V1
export HFOC_ARTIFACT_IMAGE="$PRODUCTION_IMAGE"
export HFOC_ARTIFACT_RECOVERY_IMAGE="$TEST_DIR/artifacts/recovery.iso"
export HFOC_ARTIFACT_UPDATE_BUNDLE="$TEST_DIR/artifacts/update.bundle"
export HFOC_ARTIFACT_SUPPORT_SCHEMA="$TEST_DIR/artifacts/support-schema.txt"
export HFOC_ARTIFACT_LICENSE_MANIFEST="$TEST_DIR/artifacts/licenses.sha256"
export HFOC_PROTOCOL_POLICY_VERSION=1
export HFOC_PROTOCOL_CONFIG_SCHEMA_VERSION=1
export HFOC_PROTOCOL_STATE_SCHEMA_VERSION=1
export HFOC_PROTOCOL_IMAGE_MANIFEST_VERSION=1
export HFOC_PROTOCOL_UPDATE_COMPATIBILITY_VERSION=1
export HFOC_PROTOCOL_INTEGRATION_SCHEMA_VERSION=1
export HFOC_PROTOCOL_INTEGRATION_TRANSPORT_VERSION=1

bash "$ROOT/scripts/generate-release-artifact-manifest.sh" \
    "$TEST_DIR/release-artifacts.txt"
bash "$VERIFY" --release-manifest "$TEST_DIR/release-artifacts.txt"

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
    echo "mbedTLS test-secret header is missing scanner constants" >&2
    exit 1
}

PROD_MARKER_NEGATIVE="$TEST_DIR/production-marker-negative.elf"
printf '%s\n' "$marker" >"$PROD_MARKER_NEGATIVE"
expect_fail 'production scan rejects marker' \
    bash "$VERIFY" --production "$PROD_MARKER_NEGATIVE"

PROD_DIGEST_NEGATIVE="$TEST_DIR/production-digest-negative.elf"
printf '%s\n' "$cert_digest" >"$PROD_DIGEST_NEGATIVE"
expect_fail 'production scan rejects certificate digest' \
    bash "$VERIFY" --production "$PROD_DIGEST_NEGATIVE"

TEST_MISSING_NEGATIVE="$TEST_DIR/test-missing-negative.elf"
printf 'ordinary test input\n' >"$TEST_MISSING_NEGATIVE"
expect_fail 'test scan rejects missing evidence' \
    bash "$VERIFY" --test "$TEST_MISSING_NEGATIVE"

RELEASE_MARKER_NEGATIVE="$TEST_DIR/release-marker-negative.txt"
cp "$TEST_DIR/release-artifacts.txt" "$RELEASE_MARKER_NEGATIVE"
printf '%s\n' "$marker" >>"$RELEASE_MARKER_NEGATIVE"
expect_fail 'release scan rejects marker' \
    bash "$VERIFY" --release-manifest "$RELEASE_MARKER_NEGATIVE"

RELEASE_DIGEST_NEGATIVE="$TEST_DIR/release-digest-negative.txt"
cp "$TEST_DIR/release-artifacts.txt" "$RELEASE_DIGEST_NEGATIVE"
printf 'test_secret_cert_digest=%s\n' "$cert_digest" \
    >>"$RELEASE_DIGEST_NEGATIVE"
printf 'test_secret_key_digest=%s\n' "$key_digest" \
    >>"$RELEASE_DIGEST_NEGATIVE"
expect_fail 'release scan rejects digest fields' \
    bash "$VERIFY" --release-manifest "$RELEASE_DIGEST_NEGATIVE"

echo 'mbedTLS test-secret build separation host tests: ALL OK'
