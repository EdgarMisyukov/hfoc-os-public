#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
OUTPUT=${1:-build/reproducible/reproducible-image.digests}
KERNEL=${HFOC_KERNEL_ELF:-build/hfoc-kernel.elf}
BUILD_INPUT=${HFOC_BUILD_INPUT:-build/hfoc-build-input.txt}
IMAGE=${HFOC_IMAGE:-build/hfoc-os-$(cat "$ROOT/VERSION").iso}

[[ -f "$KERNEL" ]] || {
    echo "Missing reproducible kernel artifact: $KERNEL" >&2
    exit 1
}
[[ -f "$BUILD_INPUT" ]] || {
    echo "Missing reproducible build input manifest: $BUILD_INPUT" >&2
    exit 1
}
[[ -f "$IMAGE" ]] || {
    echo "Missing reproducible image artifact: $IMAGE" >&2
    exit 1
}

revision=$(git -C "$ROOT" rev-parse --verify 'HEAD^{commit}')
if [[ ! "$revision" =~ ^[0-9a-f]{40}$ ]]; then
    echo "Invalid source revision: $revision" >&2
    exit 1
fi

kernel_digest=$(sha256sum "$KERNEL" | awk '{print $1}')
build_input_digest=$(sha256sum "$BUILD_INPUT" | awk '{print $1}')
image_digest=$(sha256sum "$IMAGE" | awk '{print $1}')

mkdir -p "$(dirname "$OUTPUT")"
tmp="${OUTPUT}.tmp.$$"
trap 'rm -f "$tmp"' EXIT
{
    printf 'comparison_version=2\n'
    printf 'source_revision=%s\n' "$revision"
    printf 'artifact_hfoc_kernel_elf=%s\n' "$kernel_digest"
    printf 'artifact_hfoc_build_input=%s\n' "$build_input_digest"
    printf 'artifact_iso=%s\n' "$image_digest"
} >"$tmp"
mv "$tmp" "$OUTPUT"
trap - EXIT

echo "Reproducible image digests: $OUTPUT"
