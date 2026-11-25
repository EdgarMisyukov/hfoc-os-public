#!/usr/bin/env bash
# Prove item 298: production artifacts reject test-secret, TLS, and proof
# controls through the reusable production artifact secret gate.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

VERSION=$(cat "$ROOT/VERSION")
PRODUCTION_KERNEL="$ROOT/build/hfoc-kernel.elf"
PRODUCTION_IMAGE="$ROOT/build/hfoc-os-${VERSION}.iso"
VERIFY="$ROOT/scripts/verify-production-artifact-secret-gate.sh"
PROFILE="$ROOT/scripts/production-artifact-secret-gate-profile.txt"
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

for command in bash grep make file nm; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

[[ -f "$VERIFY" ]] || {
    echo "Missing production artifact secret gate scanner: $VERIFY" >&2
    exit 1
}
[[ -f "$PROFILE" ]] || {
    echo "Missing production artifact secret gate profile: $PROFILE" >&2
    exit 1
}
for artifact in "$PRODUCTION_KERNEL" "$PRODUCTION_IMAGE"; do
    [[ -f "$artifact" ]] || {
        echo "Missing production artifact: $artifact" >&2
        exit 1
    }
done

mkdir -p "$TEST_DIR/artifacts"
printf 'production-update\n' >"$TEST_DIR/artifacts/update.bundle"
printf 'production-support-schema\n' \
    >"$TEST_DIR/artifacts/support-schema.txt"
printf 'production-license-manifest\n' \
    >"$TEST_DIR/artifacts/licenses.sha256"
printf 'production-recovery\n' >"$TEST_DIR/artifacts/recovery.iso"

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

printf 'HFOC OS production serial output\n' >"$TEST_DIR/serial.log"
printf 'audit_snapshot_version=1\n' >"$TEST_DIR/audit-snapshot.bin"
printf 'metrics_schema_version=1\n' >"$TEST_DIR/metrics.bin"
printf 'HFOCSBEX production support bundle\n' \
    >"$TEST_DIR/support-bundle.bin"

bash "$VERIFY" kernel "$PRODUCTION_KERNEL"
bash "$VERIFY" iso "$PRODUCTION_IMAGE"
bash "$VERIFY" release-manifest "$TEST_DIR/release-artifacts.txt"
bash "$VERIFY" serial "$TEST_DIR/serial.log"
bash "$VERIFY" audit-snapshot "$TEST_DIR/audit-snapshot.bin"
bash "$VERIFY" metrics "$TEST_DIR/metrics.bin"
bash "$VERIFY" support-bundle "$TEST_DIR/support-bundle.bin"

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

expect_fail() {
    local label=$1
    shift
    if "$@" >/dev/null 2>&1; then
        echo "$label unexpectedly succeeded" >&2
        exit 1
    fi
}

NEGATIVE_DIR="$TEST_DIR/negatives"
mkdir -p "$NEGATIVE_DIR"

printf '%s\n' "$marker" >"$NEGATIVE_DIR/kernel-marker.elf"
expect_fail 'kernel scan rejects test marker' \
    bash "$VERIFY" kernel "$NEGATIVE_DIR/kernel-marker.elf"

printf '%s\n' "$cert_digest" >"$NEGATIVE_DIR/iso-cert-digest.iso"
expect_fail 'ISO scan rejects certificate digest' \
    bash "$VERIFY" iso "$NEGATIVE_DIR/iso-cert-digest.iso"

printf 'test_secret_cert_digest=%s\n' "$cert_digest" \
    >"$NEGATIVE_DIR/release-manifest.txt"
printf 'test_secret_key_digest=%s\n' "$key_digest" \
    >>"$NEGATIVE_DIR/release-manifest.txt"
expect_fail 'release manifest scan rejects test-secret digest fields' \
    bash "$VERIFY" release-manifest "$NEGATIVE_DIR/release-manifest.txt"

printf '%s\n' 'HFOC-TLS-QEMU-RESPONSE' >"$NEGATIVE_DIR/serial-tls.log"
expect_fail 'serial scan rejects TLS QEMU response' \
    bash "$VERIFY" serial "$NEGATIVE_DIR/serial-tls.log"

printf '%s\n' 'HFOC-ENTROPY-QEMU-SECRET' \
    >"$NEGATIVE_DIR/audit-entropy.bin"
expect_fail 'audit snapshot scan rejects entropy sentinel' \
    bash "$VERIFY" audit-snapshot "$NEGATIVE_DIR/audit-entropy.bin"

printf '%s\n' '[TLS-QEMU]' >"$NEGATIVE_DIR/metrics-tls.bin"
expect_fail 'metrics scan rejects TLS proof control' \
    bash "$VERIFY" metrics "$NEGATIVE_DIR/metrics-tls.bin"

printf '%s\n' '[ENTROPY-FAIL-CLOSED]' \
    >"$NEGATIVE_DIR/support-entropy.bin"
expect_fail 'support bundle scan rejects entropy proof control' \
    bash "$VERIFY" support-bundle "$NEGATIVE_DIR/support-entropy.bin"

profile_index=0
while IFS= read -r line; do
    case "$line" in
        ''|\#*)
            continue
            ;;
    esac
    profile_index=$((profile_index + 1))
    printf '%s\n' "$line" >"$NEGATIVE_DIR/profile-${profile_index}.log"
    expect_fail "profile token scan rejects $line" \
        bash "$VERIFY" serial "$NEGATIVE_DIR/profile-${profile_index}.log"
done <"$PROFILE"

echo 'Production kernel scan: OK'
echo 'Production ISO scan: OK'
echo 'Production release manifest scan: OK'
echo 'Production serial scan: OK'
echo 'Production audit snapshot scan: OK'
echo 'Production metrics scan: OK'
echo 'Production support bundle scan: OK'
echo 'Test key/certificate digest negative: found'
echo 'TLS secret negative: found'
echo 'Proof control negative: found'
echo 'Production artifact secret gate host test: ALL OK'
