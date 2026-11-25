#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT
mkdir -p "$TEST_DIR/inputs"
printf 'abc' >"$TEST_DIR/inputs/a.bin"
printf 'def' >"$TEST_DIR/inputs/b.bin"

INPUT_FILES="$TEST_DIR/inputs/a.bin $TEST_DIR/inputs/b.bin"
CC=$(command -v gcc)
LD=$(command -v ld)
CFLAGS='-std=gnu11 -O2'
ASFLAGS='-g'
MANIFEST="$TEST_DIR/manifest.txt"

export HFOC_CC="$CC"
export HFOC_LD="$LD"
export HFOC_CFLAGS="$CFLAGS"
export HFOC_ASFLAGS="$ASFLAGS"
export HFOC_INPUT_FILES="$INPUT_FILES"

bash scripts/build-input-manifest.sh "$MANIFEST"
bash scripts/verify-build-input-manifest.sh "$MANIFEST"

grep -Fq 'manifest_version=1' "$MANIFEST"
grep -Fq "source_revision=$(git rev-parse HEAD)" "$MANIFEST"
grep -Fq "build_flags_c=$CFLAGS" "$MANIFEST"
grep -Fq "build_flags_as=$ASFLAGS" "$MANIFEST"
grep -Fq 'generated_input_' "$MANIFEST"

if HFOC_INPUT_FILES="$INPUT_FILES $TEST_DIR/inputs/missing.bin" \
    bash scripts/build-input-manifest.sh "$TEST_DIR/missing.txt" \
    >/dev/null 2>&1; then
    echo 'build input manifest accepted a missing input' >&2
    exit 1
fi

printf 'changed' >"$TEST_DIR/inputs/a.bin"
if bash scripts/verify-build-input-manifest.sh "$MANIFEST" \
    >/dev/null 2>&1; then
    echo 'build input manifest accepted a mismatched digest' >&2
    exit 1
fi

printf 'abc' >"$TEST_DIR/inputs/a.bin"
bash scripts/verify-build-input-manifest.sh "$MANIFEST"

sed -i '/^source_revision=/d' "$MANIFEST"
if bash scripts/verify-build-input-manifest.sh "$MANIFEST" \
    >/dev/null 2>&1; then
    echo 'build input manifest accepted a missing field' >&2
    exit 1
fi

sed -i 's/^source_revision=.*/source_revision=0000000000000000000000000000000000000000/' \
    "$MANIFEST"
if bash scripts/verify-build-input-manifest.sh "$MANIFEST" \
    >/dev/null 2>&1; then
    echo 'build input manifest accepted a mismatched revision' >&2
    exit 1
fi

echo 'Build input manifest host test: OK'
