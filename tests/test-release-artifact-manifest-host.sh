#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT
mkdir -p "$TEST_DIR/build-a" "$TEST_DIR/build-b"

printf 'image-one' >"$TEST_DIR/build-a/hfoc-os.iso"
printf 'recovery-one' >"$TEST_DIR/build-a/hfoc-recovery.iso"
printf 'bundle-one' >"$TEST_DIR/build-a/hfoc-update.bundle"
printf 'schema-one' >"$TEST_DIR/build-a/support-schema.txt"
printf 'license-manifest-one' >"$TEST_DIR/build-a/licenses.sha256"
cp "$TEST_DIR/build-a/hfoc-os.iso" "$TEST_DIR/build-b/hfoc-os.iso"
cp "$TEST_DIR/build-a/hfoc-recovery.iso" "$TEST_DIR/build-b/hfoc-recovery.iso"
cp "$TEST_DIR/build-a/hfoc-update.bundle" "$TEST_DIR/build-b/hfoc-update.bundle"
cp "$TEST_DIR/build-a/support-schema.txt" "$TEST_DIR/build-b/support-schema.txt"
cp "$TEST_DIR/build-a/licenses.sha256" "$TEST_DIR/build-b/licenses.sha256"

export HFOC_RELEASE_ID='HFOC-APPLIANCE-V1'
export HFOC_ARTIFACT_IMAGE="$TEST_DIR/build-a/hfoc-os.iso"
export HFOC_ARTIFACT_RECOVERY_IMAGE="$TEST_DIR/build-a/hfoc-recovery.iso"
export HFOC_ARTIFACT_UPDATE_BUNDLE="$TEST_DIR/build-a/hfoc-update.bundle"
export HFOC_ARTIFACT_SUPPORT_SCHEMA="$TEST_DIR/build-a/support-schema.txt"
export HFOC_ARTIFACT_LICENSE_MANIFEST="$TEST_DIR/build-a/licenses.sha256"
export HFOC_PROTOCOL_POLICY_VERSION=1
export HFOC_PROTOCOL_CONFIG_SCHEMA_VERSION=1
export HFOC_PROTOCOL_STATE_SCHEMA_VERSION=1
export HFOC_PROTOCOL_IMAGE_MANIFEST_VERSION=1
export HFOC_PROTOCOL_UPDATE_COMPATIBILITY_VERSION=1
export HFOC_PROTOCOL_INTEGRATION_SCHEMA_VERSION=1
export HFOC_PROTOCOL_INTEGRATION_TRANSPORT_VERSION=1

bash scripts/generate-release-artifact-manifest.sh \
    "$TEST_DIR/release-a.manifest"

grep -Fq 'artifact_manifest_version=2' "$TEST_DIR/release-a.manifest"
grep -Fq 'release_id=HFOC-APPLIANCE-V1' "$TEST_DIR/release-a.manifest"
grep -Fq "source_revision=$(git rev-parse HEAD)" \
    "$TEST_DIR/release-a.manifest"
grep -Fq 'protocol_policy_version=1' "$TEST_DIR/release-a.manifest"
grep -Fq 'protocol_config_schema_version=1' \
    "$TEST_DIR/release-a.manifest"
grep -Fq 'protocol_state_schema_version=1' \
    "$TEST_DIR/release-a.manifest"
grep -Fq 'protocol_image_manifest_version=1' \
    "$TEST_DIR/release-a.manifest"
grep -Fq 'protocol_update_compatibility_version=1' \
    "$TEST_DIR/release-a.manifest"
grep -Fq 'protocol_integration_schema_version=1' \
    "$TEST_DIR/release-a.manifest"
grep -Fq 'protocol_integration_transport_version=1' \
    "$TEST_DIR/release-a.manifest"
grep -Eq '^artifact_image=[0-9a-f]{64}$' "$TEST_DIR/release-a.manifest"
grep -Eq '^artifact_recovery_image=[0-9a-f]{64}$' \
    "$TEST_DIR/release-a.manifest"
grep -Eq '^artifact_update_bundle=[0-9a-f]{64}$' \
    "$TEST_DIR/release-a.manifest"
grep -Eq '^artifact_support_schema=[0-9a-f]{64}$' \
    "$TEST_DIR/release-a.manifest"
grep -Eq '^artifact_license_manifest=[0-9a-f]{64}$' \
    "$TEST_DIR/release-a.manifest"

bash scripts/verify-release-artifact-manifest.sh \
    "$TEST_DIR/release-a.manifest"

printf 'image-two' >"$TEST_DIR/build-a/hfoc-os.iso"
if bash scripts/verify-release-artifact-manifest.sh \
    "$TEST_DIR/release-a.manifest" >/dev/null 2>&1; then
    echo 'release artifact verification accepted an image digest mismatch' >&2
    exit 1
fi
printf 'image-one' >"$TEST_DIR/build-a/hfoc-os.iso"

printf 'recovery-two' >"$TEST_DIR/build-b/hfoc-recovery.iso"
if HFOC_ARTIFACT_IMAGE="$TEST_DIR/build-b/hfoc-os.iso" \
    HFOC_ARTIFACT_RECOVERY_IMAGE="$TEST_DIR/build-b/hfoc-recovery.iso" \
    HFOC_ARTIFACT_UPDATE_BUNDLE="$TEST_DIR/build-b/hfoc-update.bundle" \
    HFOC_ARTIFACT_SUPPORT_SCHEMA="$TEST_DIR/build-b/support-schema.txt" \
    bash scripts/verify-release-artifact-manifest.sh \
    "$TEST_DIR/release-a.manifest" >/dev/null 2>&1; then
    echo 'release artifact verification accepted a recovery-image mismatch' >&2
    exit 1
fi
printf 'recovery-one' >"$TEST_DIR/build-b/hfoc-recovery.iso"

printf 'bundle-two' >"$TEST_DIR/build-b/hfoc-update.bundle"
if HFOC_ARTIFACT_IMAGE="$TEST_DIR/build-b/hfoc-os.iso" \
    HFOC_ARTIFACT_RECOVERY_IMAGE="$TEST_DIR/build-b/hfoc-recovery.iso" \
    HFOC_ARTIFACT_UPDATE_BUNDLE="$TEST_DIR/build-b/hfoc-update.bundle" \
    HFOC_ARTIFACT_SUPPORT_SCHEMA="$TEST_DIR/build-b/support-schema.txt" \
    bash scripts/verify-release-artifact-manifest.sh \
    "$TEST_DIR/release-a.manifest" >/dev/null 2>&1; then
    echo 'release artifact verification accepted an update-bundle mismatch' >&2
    exit 1
fi
printf 'bundle-one' >"$TEST_DIR/build-b/hfoc-update.bundle"

printf 'schema-two' >"$TEST_DIR/build-b/support-schema.txt"
if HFOC_ARTIFACT_IMAGE="$TEST_DIR/build-b/hfoc-os.iso" \
    HFOC_ARTIFACT_RECOVERY_IMAGE="$TEST_DIR/build-b/hfoc-recovery.iso" \
    HFOC_ARTIFACT_UPDATE_BUNDLE="$TEST_DIR/build-b/hfoc-update.bundle" \
    HFOC_ARTIFACT_SUPPORT_SCHEMA="$TEST_DIR/build-b/support-schema.txt" \
    bash scripts/verify-release-artifact-manifest.sh \
    "$TEST_DIR/release-a.manifest" >/dev/null 2>&1; then
    echo 'release artifact verification accepted a support-schema mismatch' >&2
    exit 1
fi

printf 'license-manifest-two' >"$TEST_DIR/build-a/licenses.sha256"
if bash scripts/verify-release-artifact-manifest.sh \
    "$TEST_DIR/release-a.manifest" >/dev/null 2>&1; then
    echo 'release artifact verification accepted a license-manifest mismatch' >&2
    exit 1
fi
printf 'license-manifest-one' >"$TEST_DIR/build-a/licenses.sha256"

cp "$TEST_DIR/release-a.manifest" "$TEST_DIR/release-missing-license.manifest"
sed -i '/^artifact_license_manifest=/d' \
    "$TEST_DIR/release-missing-license.manifest"
if bash scripts/verify-release-artifact-manifest.sh \
    "$TEST_DIR/release-missing-license.manifest" >/dev/null 2>&1; then
    echo 'release artifact verification accepted a missing license field' >&2
    exit 1
fi

cp "$TEST_DIR/release-a.manifest" "$TEST_DIR/release-invalid-license.manifest"
sed -i 's/^artifact_license_manifest=.*/artifact_license_manifest=bad/' \
    "$TEST_DIR/release-invalid-license.manifest"
if bash scripts/verify-release-artifact-manifest.sh \
    "$TEST_DIR/release-invalid-license.manifest" >/dev/null 2>&1; then
    echo 'release artifact verification accepted an invalid license digest' >&2
    exit 1
fi

if HFOC_ARTIFACT_LICENSE_MANIFEST="$TEST_DIR/build-a/missing.sha256" \
    bash scripts/generate-release-artifact-manifest.sh \
    "$TEST_DIR/release-missing-license-input.manifest" >/dev/null 2>&1; then
    echo 'release artifact generation accepted a missing license manifest' >&2
    exit 1
fi

if HFOC_ARTIFACT_IMAGE="$TEST_DIR/build-a/missing.iso" \
    bash scripts/generate-release-artifact-manifest.sh \
    "$TEST_DIR/release-missing.manifest" >/dev/null 2>&1; then
    echo 'release artifact generation accepted a missing artifact' >&2
    exit 1
fi

cp "$TEST_DIR/release-a.manifest" "$TEST_DIR/release-missing-field.manifest"
sed -i '/^protocol_policy_version=/d' \
    "$TEST_DIR/release-missing-field.manifest"
if bash scripts/verify-release-artifact-manifest.sh \
    "$TEST_DIR/release-missing-field.manifest" >/dev/null 2>&1; then
    echo 'release artifact verification accepted a missing field' >&2
    exit 1
fi

cp "$TEST_DIR/release-a.manifest" "$TEST_DIR/release-invalid.manifest"
sed -i 's/^artifact_image=.*/artifact_image=bad/' \
    "$TEST_DIR/release-invalid.manifest"
if bash scripts/verify-release-artifact-manifest.sh \
    "$TEST_DIR/release-invalid.manifest" >/dev/null 2>&1; then
    echo 'release artifact verification accepted an invalid digest' >&2
    exit 1
fi

if HFOC_PROTOCOL_POLICY_VERSION=2 \
    bash scripts/verify-release-artifact-manifest.sh \
    "$TEST_DIR/release-a.manifest" >/dev/null 2>&1; then
    echo 'release artifact verification accepted a protocol mismatch' >&2
    exit 1
fi

cp "$TEST_DIR/release-a.manifest" "$TEST_DIR/release-revision.manifest"
sed -i 's/^source_revision=.*/source_revision=0000000000000000000000000000000000000000/' \
    "$TEST_DIR/release-revision.manifest"
if bash scripts/verify-release-artifact-manifest.sh \
    "$TEST_DIR/release-revision.manifest" >/dev/null 2>&1; then
    echo 'release artifact verification accepted a revision mismatch' >&2
    exit 1
fi

cp "$TEST_DIR/release-a.manifest" "$TEST_DIR/release-identity.manifest"
sed -i 's/^release_id=.*/release_id=OTHER/' \
    "$TEST_DIR/release-identity.manifest"
if bash scripts/verify-release-artifact-manifest.sh \
    "$TEST_DIR/release-identity.manifest" >/dev/null 2>&1; then
    echo 'release artifact verification accepted an identity mismatch' >&2
    exit 1
fi

echo 'Release artifact manifest host test: OK'
