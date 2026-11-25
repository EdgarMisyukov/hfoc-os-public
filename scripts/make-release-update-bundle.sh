#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
OUTPUT=${1:-$ROOT/build/hfoc-update.bundle}
KERNEL=${HFOC_KERNEL_ELF:-$ROOT/build/hfoc-kernel.elf}
SUPPORT_SCHEMA=${HFOC_SUPPORT_SCHEMA:-$ROOT/kernel/include/support_bundle_manifest.h}
COMPAT_VERSION=${HFOC_UPDATE_COMPATIBILITY_VERSION:-1}

[[ -f "$KERNEL" ]] || {
    echo "Missing release update bundle kernel: $KERNEL" >&2
    exit 1
}
[[ -f "$SUPPORT_SCHEMA" ]] || {
    echo "Missing release update bundle support schema: $SUPPORT_SCHEMA" >&2
    exit 1
}
[[ "$COMPAT_VERSION" =~ ^[1-9][0-9]*$ ]] || {
    echo "Invalid update compatibility version: $COMPAT_VERSION" >&2
    exit 1
}

revision=$(git -C "$ROOT" rev-parse --verify 'HEAD^{commit}')
if [[ ! "$revision" =~ ^[0-9a-f]{40}$ ]]; then
    echo "Invalid source revision: $revision" >&2
    exit 1
fi

kernel_digest=$(sha256sum "$KERNEL" | awk '{print $1}')
support_digest=$(sha256sum "$SUPPORT_SCHEMA" | awk '{print $1}')

mkdir -p "$(dirname "$OUTPUT")"
tmp="${OUTPUT}.tmp.$$"
trap 'rm -f "$tmp"' EXIT
{
    printf 'update_bundle_version=1\n'
    printf 'source_revision=%s\n' "$revision"
    printf 'kernel_digest=%s\n' "$kernel_digest"
    printf 'support_schema_digest=%s\n' "$support_digest"
    printf 'update_compatibility_version=%s\n' "$COMPAT_VERSION"
} >"$tmp"
mv "$tmp" "$OUTPUT"
trap - EXIT

echo "Release update bundle: $OUTPUT"
