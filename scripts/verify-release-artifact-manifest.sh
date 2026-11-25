#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
MANIFEST=${1:-}
VERSION=$(cat "$ROOT/VERSION")
RELEASE_ID=${HFOC_RELEASE_ID:-HFOC-APPLIANCE-V1}
IMAGE=${HFOC_ARTIFACT_IMAGE:-$ROOT/build/hfoc-os-$VERSION.iso}
RECOVERY_IMAGE=${HFOC_ARTIFACT_RECOVERY_IMAGE:-$ROOT/build/fallback/hfoc-os-$VERSION-fallback.iso}
UPDATE_BUNDLE=${HFOC_ARTIFACT_UPDATE_BUNDLE:-$ROOT/build/hfoc-update.bundle}
SUPPORT_SCHEMA=${HFOC_ARTIFACT_SUPPORT_SCHEMA:-$ROOT/kernel/include/support_bundle_manifest.h}
LICENSE_MANIFEST=${HFOC_ARTIFACT_LICENSE_MANIFEST:-$ROOT/build/release/licenses/LICENSES.sha256}
PROTOCOL_POLICY=${HFOC_PROTOCOL_POLICY_VERSION:-1}
PROTOCOL_CONFIG=${HFOC_PROTOCOL_CONFIG_SCHEMA_VERSION:-1}
PROTOCOL_STATE=${HFOC_PROTOCOL_STATE_SCHEMA_VERSION:-1}
PROTOCOL_IMAGE=${HFOC_PROTOCOL_IMAGE_MANIFEST_VERSION:-1}
PROTOCOL_UPDATE=${HFOC_PROTOCOL_UPDATE_COMPATIBILITY_VERSION:-1}
PROTOCOL_INTEGRATION_SCHEMA=${HFOC_PROTOCOL_INTEGRATION_SCHEMA_VERSION:-1}
PROTOCOL_INTEGRATION_TRANSPORT=${HFOC_PROTOCOL_INTEGRATION_TRANSPORT_VERSION:-1}

if [[ -z "$MANIFEST" ]]; then
    echo "Usage: verify-release-artifact-manifest.sh MANIFEST" >&2
    exit 1
fi
[[ -f "$MANIFEST" ]] || {
    echo "Missing release artifact manifest: $MANIFEST" >&2
    exit 1
}

required_value() {
    local key=$1
    local value
    if [[ $(grep -c "^${key}=" "$MANIFEST") -ne 1 ]]; then
        echo "Release artifact manifest is missing ${key}" >&2
        exit 1
    fi
    value=$(sed -n "s/^${key}=//p" "$MANIFEST")
    if [[ -z "$value" ]]; then
        echo "Release artifact manifest has empty ${key}" >&2
        exit 1
    fi
    printf '%s' "$value"
}

require_digest() {
    local key=$1
    local value
    value=$(required_value "$key")
    if [[ ! "$value" =~ ^[0-9a-f]{64}$ ]]; then
        echo "Release artifact manifest has invalid ${key}" >&2
        exit 1
    fi
    printf '%s' "$value"
}

require_version() {
    local key=$1
    local value
    value=$(required_value "$key")
    if [[ ! "$value" =~ ^[1-9][0-9]*$ ]]; then
        echo "Release artifact manifest has invalid ${key}" >&2
        exit 1
    fi
    printf '%s' "$value"
}

manifest_version=$(required_value artifact_manifest_version)
[[ "$manifest_version" == "2" ]] || {
    echo "Unsupported release artifact manifest version" >&2
    exit 1
}

manifest_release_id=$(required_value release_id)
[[ "$manifest_release_id" == "$RELEASE_ID" ]] || {
    echo "Release artifact manifest release ID mismatch" >&2
    exit 1
}

manifest_revision=$(required_value source_revision)
[[ "$manifest_revision" =~ ^[0-9a-f]{40}$ ]] || {
    echo "Release artifact manifest has invalid source revision" >&2
    exit 1
}
revision=$(git -C "$ROOT" rev-parse --verify 'HEAD^{commit}')
[[ "$manifest_revision" == "$revision" ]] || {
    echo "Release artifact manifest source revision mismatch" >&2
    exit 1
}

compare_version() {
    local key=$1
    local expected=$2
    local actual
    actual=$(require_version "$key")
    [[ "$actual" == "$expected" ]] || {
        echo "Release artifact manifest ${key} mismatch" >&2
        exit 1
    }
}

compare_version protocol_policy_version "$PROTOCOL_POLICY"
compare_version protocol_config_schema_version "$PROTOCOL_CONFIG"
compare_version protocol_state_schema_version "$PROTOCOL_STATE"
compare_version protocol_image_manifest_version "$PROTOCOL_IMAGE"
compare_version protocol_update_compatibility_version "$PROTOCOL_UPDATE"
compare_version protocol_integration_schema_version \
    "$PROTOCOL_INTEGRATION_SCHEMA"
compare_version protocol_integration_transport_version \
    "$PROTOCOL_INTEGRATION_TRANSPORT"

verify_artifact() {
    local key=$1
    local path=$2
    local expected
    local actual
    expected=$(require_digest "$key")
    [[ -f "$path" ]] || {
        echo "Missing release artifact for ${key}: $path" >&2
        exit 1
    }
    actual=$(sha256sum "$path" | awk '{print $1}')
    [[ "$actual" == "$expected" ]] || {
        echo "Release artifact ${key} digest mismatch" >&2
        exit 1
    }
}

verify_artifact artifact_image "$IMAGE"
verify_artifact artifact_recovery_image "$RECOVERY_IMAGE"
verify_artifact artifact_update_bundle "$UPDATE_BUNDLE"
verify_artifact artifact_support_schema "$SUPPORT_SCHEMA"
verify_artifact artifact_license_manifest "$LICENSE_MANIFEST"

echo "Release artifact manifest verification: OK"
