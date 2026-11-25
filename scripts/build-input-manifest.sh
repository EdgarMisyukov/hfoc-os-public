#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
OUTPUT=${1:-build/hfoc-build-input.txt}
CC=${HFOC_CC:-gcc}
LD=${HFOC_LD:-ld}
CFLAGS=${HFOC_CFLAGS:-}
ASFLAGS=${HFOC_ASFLAGS:-}
INPUT_FILES=${HFOC_INPUT_FILES:-}

revision=$(git -C "$ROOT" rev-parse --verify 'HEAD^{commit}')
if [[ ! "$revision" =~ ^[0-9a-f]{40}$ ]]; then
    echo "Invalid source revision: $revision" >&2
    exit 1
fi

cc_id=$("$CC" --version 2>/dev/null | head -n 1 || true)
ld_id=$("$LD" --version 2>/dev/null | head -n 1 || true)
if [[ -z "$cc_id" || -z "$ld_id" ]]; then
    echo "Unable to read toolchain identifiers" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUTPUT")"
{
    printf 'manifest_version=1\n'
    printf 'source_revision=%s\n' "$revision"
    printf 'toolchain_cc=%s\n' "$cc_id"
    printf 'toolchain_ld=%s\n' "$ld_id"
    printf 'build_flags_c=%s\n' "$CFLAGS"
    printf 'build_flags_as=%s\n' "$ASFLAGS"
} >"$OUTPUT"

if [[ -n "$INPUT_FILES" ]]; then
    read -r -a input_paths <<<"$INPUT_FILES"
    for input_path in "${input_paths[@]}"; do
        if [[ ! -f "$input_path" ]]; then
            echo "Missing generated input: $input_path" >&2
            exit 1
        fi
        digest=$(sha256sum "$input_path" | awk '{print $1}')
        key=$(printf '%s' "$input_path" | tr -c '[:alnum:]' '_')
        printf 'generated_input_%s=%s\n' "$key" "$digest" >>"$OUTPUT"
    done
fi

echo "Build input manifest: $OUTPUT"
