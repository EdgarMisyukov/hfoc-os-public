#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
OUTPUT=${1:-$ROOT/build/release/release-artifacts.txt}
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

[[ -n "$RELEASE_ID" ]] || {
    echo "Empty release ID" >&2
    exit 1
}
for protocol in \
    "$PROTOCOL_POLICY" \
    "$PROTOCOL_CONFIG" \
    "$PROTOCOL_STATE" \
    "$PROTOCOL_IMAGE" \
    "$PROTOCOL_UPDATE" \
    "$PROTOCOL_INTEGRATION_SCHEMA" \
    "$PROTOCOL_INTEGRATION_TRANSPORT"; do
    [[ "$protocol" =~ ^[1-9][0-9]*$ ]] || {
        echo "Invalid protocol version: $protocol" >&2
        exit 1
    }
done

for artifact in "$IMAGE" "$RECOVERY_IMAGE" "$UPDATE_BUNDLE" "$SUPPORT_SCHEMA" \
    "$LICENSE_MANIFEST"; do
    [[ -f "$artifact" ]] || {
        echo "Missing release artifact: $artifact" >&2
        exit 1
    }
done

revision=$(git -C "$ROOT" rev-parse --verify 'HEAD^{commit}')
if [[ ! "$revision" =~ ^[0-9a-f]{40}$ ]]; then
    echo "Invalid source revision: $revision" >&2
    exit 1
fi

image_digest=$(sha256sum "$IMAGE" | awk '{print $1}')
recovery_digest=$(sha256sum "$RECOVERY_IMAGE" | awk '{print $1}')
update_digest=$(sha256sum "$UPDATE_BUNDLE" | awk '{print $1}')
support_digest=$(sha256sum "$SUPPORT_SCHEMA" | awk '{print $1}')
license_manifest_digest=$(sha256sum "$LICENSE_MANIFEST" | awk '{print $1}')

mkdir -p "$(dirname "$OUTPUT")"
tmp="${OUTPUT}.tmp.$$"
trap 'rm -f "$tmp"' EXIT
{
    printf 'artifact_manifest_version=2\n'
    printf 'release_id=%s\n' "$RELEASE_ID"
    printf 'source_revision=%s\n' "$revision"
    printf 'protocol_policy_version=%s\n' "$PROTOCOL_POLICY"
    printf 'protocol_config_schema_version=%s\n' "$PROTOCOL_CONFIG"
    printf 'protocol_state_schema_version=%s\n' "$PROTOCOL_STATE"
    printf 'protocol_image_manifest_version=%s\n' "$PROTOCOL_IMAGE"
    printf 'protocol_update_compatibility_version=%s\n' "$PROTOCOL_UPDATE"
    printf 'protocol_integration_schema_version=%s\n' \
        "$PROTOCOL_INTEGRATION_SCHEMA"
    printf 'protocol_integration_transport_version=%s\n' \
        "$PROTOCOL_INTEGRATION_TRANSPORT"
    printf 'artifact_image=%s\n' "$image_digest"
    printf 'artifact_recovery_image=%s\n' "$recovery_digest"
    printf 'artifact_update_bundle=%s\n' "$update_digest"
    printf 'artifact_support_schema=%s\n' "$support_digest"
    printf 'artifact_license_manifest=%s\n' "$license_manifest_digest"
} >"$tmp"
mv "$tmp" "$OUTPUT"
trap - EXIT

echo "Release artifact manifest: $OUTPUT"
