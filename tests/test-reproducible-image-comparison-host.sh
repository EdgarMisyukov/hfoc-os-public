#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT
mkdir -p "$TEST_DIR/build-a" "$TEST_DIR/build-b"

printf 'kernel-one' >"$TEST_DIR/build-a/hfoc-kernel.elf"
printf 'manifest-one' >"$TEST_DIR/build-a/hfoc-build-input.txt"
printf 'image-one' >"$TEST_DIR/build-a/hfoc-os.iso"
printf 'kernel-one' >"$TEST_DIR/build-b/hfoc-kernel.elf"
printf 'manifest-one' >"$TEST_DIR/build-b/hfoc-build-input.txt"
printf 'image-one' >"$TEST_DIR/build-b/hfoc-os.iso"

export HFOC_KERNEL_ELF="$TEST_DIR/build-a/hfoc-kernel.elf"
export HFOC_BUILD_INPUT="$TEST_DIR/build-a/hfoc-build-input.txt"
export HFOC_IMAGE="$TEST_DIR/build-a/hfoc-os.iso"
bash scripts/capture-reproducible-image-digests.sh \
    "$TEST_DIR/build-a.digests"

grep -Fq 'comparison_version=2' "$TEST_DIR/build-a.digests"
grep -Fq "source_revision=$(git rev-parse HEAD)" "$TEST_DIR/build-a.digests"
grep -Eq '^artifact_hfoc_kernel_elf=[0-9a-f]{64}$' \
    "$TEST_DIR/build-a.digests"
grep -Eq '^artifact_hfoc_build_input=[0-9a-f]{64}$' \
    "$TEST_DIR/build-a.digests"
grep -Eq '^artifact_iso=[0-9a-f]{64}$' "$TEST_DIR/build-a.digests"

export HFOC_KERNEL_ELF="$TEST_DIR/build-b/hfoc-kernel.elf"
export HFOC_BUILD_INPUT="$TEST_DIR/build-b/hfoc-build-input.txt"
export HFOC_IMAGE="$TEST_DIR/build-b/hfoc-os.iso"
bash scripts/capture-reproducible-image-digests.sh \
    "$TEST_DIR/build-b.digests"
bash scripts/compare-reproducible-image-digests.sh \
    "$TEST_DIR/build-a.digests" "$TEST_DIR/build-b.digests"

printf 'kernel-two' >"$TEST_DIR/build-a/hfoc-kernel.elf"
export HFOC_KERNEL_ELF="$TEST_DIR/build-a/hfoc-kernel.elf"
export HFOC_BUILD_INPUT="$TEST_DIR/build-a/hfoc-build-input.txt"
bash scripts/capture-reproducible-image-digests.sh \
    "$TEST_DIR/build-a-changed.digests"
if bash scripts/compare-reproducible-image-digests.sh \
    "$TEST_DIR/build-a.digests" "$TEST_DIR/build-a-changed.digests" \
    >/dev/null 2>&1; then
    echo 'reproducible image comparison accepted a kernel digest mismatch' >&2
    exit 1
fi

printf 'manifest-two' >"$TEST_DIR/build-b/hfoc-build-input.txt"
export HFOC_KERNEL_ELF="$TEST_DIR/build-b/hfoc-kernel.elf"
export HFOC_BUILD_INPUT="$TEST_DIR/build-b/hfoc-build-input.txt"
export HFOC_IMAGE="$TEST_DIR/build-b/hfoc-os.iso"
bash scripts/capture-reproducible-image-digests.sh \
    "$TEST_DIR/build-b-changed.digests"
if bash scripts/compare-reproducible-image-digests.sh \
    "$TEST_DIR/build-a.digests" "$TEST_DIR/build-b-changed.digests" \
    >/dev/null 2>&1; then
    echo 'reproducible image comparison accepted a build-input mismatch' >&2
    exit 1
fi

printf 'image-two' >"$TEST_DIR/build-b/hfoc-os.iso"
bash scripts/capture-reproducible-image-digests.sh \
    "$TEST_DIR/build-b-image.digests"
if bash scripts/compare-reproducible-image-digests.sh \
    "$TEST_DIR/build-a.digests" "$TEST_DIR/build-b-image.digests" \
    >/dev/null 2>&1; then
    echo 'reproducible image comparison accepted an ISO digest mismatch' >&2
    exit 1
fi

if HFOC_KERNEL_ELF="$TEST_DIR/build-a/missing.elf" \
    bash scripts/capture-reproducible-image-digests.sh \
    "$TEST_DIR/missing.digests" >/dev/null 2>&1; then
    echo 'reproducible image capture accepted a missing artifact' >&2
    exit 1
fi

sed -i '/^source_revision=/d' "$TEST_DIR/build-a.digests"
if bash scripts/compare-reproducible-image-digests.sh \
    "$TEST_DIR/build-a.digests" "$TEST_DIR/build-b.digests" \
    >/dev/null 2>&1; then
    echo 'reproducible image comparison accepted a missing field' >&2
    exit 1
fi

sed -i 's/^artifact_hfoc_kernel_elf=.*/artifact_hfoc_kernel_elf=bad/' \
    "$TEST_DIR/build-b.digests"
if bash scripts/compare-reproducible-image-digests.sh \
    "$TEST_DIR/build-b.digests" "$TEST_DIR/build-b.digests" \
    >/dev/null 2>&1; then
    echo 'reproducible image comparison accepted an invalid digest' >&2
    exit 1
fi

echo 'Reproducible image comparison host test: OK'
