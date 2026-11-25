#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
MANIFEST=${1:-build/hfoc-build-input.txt}
CC=${HFOC_CC:-gcc}
LD=${HFOC_LD:-ld}
CFLAGS=${HFOC_CFLAGS:-}
ASFLAGS=${HFOC_ASFLAGS:-}
INPUT_FILES=${HFOC_INPUT_FILES:-}

required_value() {
    local key=$1
    local value
    if ! grep -Eq "^${key}=" "$MANIFEST"; then
        echo "Manifest is missing ${key}" >&2
        exit 1
    fi
    value=$(grep -m1 "^${key}=" "$MANIFEST" | cut -d= -f2-)
    if [[ -z "$value" ]]; then
        echo "Manifest has empty ${key}" >&2
        exit 1
    fi
    printf '%s' "$value"
}

[[ -f "$MANIFEST" ]] || {
    echo "Missing build input manifest: $MANIFEST" >&2
    exit 1
}

version=$(required_value manifest_version)
[[ "$version" == "1" ]] || {
    echo "Unsupported build input manifest version: $version" >&2
    exit 1
}

revision=$(git -C "$ROOT" rev-parse --verify 'HEAD^{commit}')
if [[ "$(required_value source_revision)" != "$revision" ]]; then
    echo "Build input manifest source revision mismatch" >&2
    exit 1
fi

cc_id=$("$CC" --version 2>/dev/null | head -n 1 || true)
ld_id=$("$LD" --version 2>/dev/null | head -n 1 || true)
if [[ -z "$cc_id" || -z "$ld_id" ]]; then
    echo "Unable to read toolchain identifiers" >&2
    exit 1
fi
[[ "$(required_value toolchain_cc)" == "$cc_id" ]] || {
    echo "Build input manifest compiler mismatch" >&2
    exit 1
}
[[ "$(required_value toolchain_ld)" == "$ld_id" ]] || {
    echo "Build input manifest linker mismatch" >&2
    exit 1
}
[[ "$(required_value build_flags_c)" == "$CFLAGS" ]] || {
    echo "Build input manifest C flags mismatch" >&2
    exit 1
}
[[ "$(required_value build_flags_as)" == "$ASFLAGS" ]] || {
    echo "Build input manifest assembler flags mismatch" >&2
    exit 1
}

if [[ -n "$INPUT_FILES" ]]; then
    read -r -a input_paths <<<"$INPUT_FILES"
    for input_path in "${input_paths[@]}"; do
        if [[ ! -f "$input_path" ]]; then
            echo "Missing generated input: $input_path" >&2
            exit 1
        fi
        key=$(printf '%s' "$input_path" | tr -c '[:alnum:]' '_')
        expected=$(sha256sum "$input_path" | awk '{print $1}')
        actual=$(required_value "generated_input_${key}")
        if [[ "$actual" != "$expected" ]]; then
            echo "Build input manifest digest mismatch for ${input_path}" >&2
            exit 1
        fi
    done
fi

echo "Build input manifest verification: OK"
