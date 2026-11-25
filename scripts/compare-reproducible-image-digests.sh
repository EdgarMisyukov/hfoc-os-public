#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

FIRST=${1:-}
SECOND=${2:-}
REPORT=${3:-}

if [[ -z "$FIRST" || -z "$SECOND" ]]; then
    echo "Usage: compare-reproducible-image-digests.sh FIRST SECOND [REPORT]" >&2
    exit 1
fi

[[ -f "$FIRST" ]] || {
    echo "Missing first digest manifest: $FIRST" >&2
    exit 1
}
[[ -f "$SECOND" ]] || {
    echo "Missing second digest manifest: $SECOND" >&2
    exit 1
}

required_value() {
    local file=$1
    local key=$2
    local value
    if ! grep -Eq "^${key}=" "$file"; then
        echo "Digest manifest is missing ${key}: $file" >&2
        exit 1
    fi
    value=$(grep -m1 "^${key}=" "$file" | cut -d= -f2-)
    if [[ -z "$value" ]]; then
        echo "Digest manifest has empty ${key}: $file" >&2
        exit 1
    fi
    printf '%s' "$value"
}

require_digest() {
    local file=$1
    local key=$2
    local value
    value=$(required_value "$file" "$key")
    if [[ ! "$value" =~ ^[0-9a-f]{64}$ ]]; then
        echo "Digest manifest has invalid ${key}: $file" >&2
        exit 1
    fi
    printf '%s' "$value"
}

version_one=$(required_value "$FIRST" comparison_version)
version_two=$(required_value "$SECOND" comparison_version)
[[ "$version_one" == "2" && "$version_two" == "2" ]] || {
    echo "Unsupported reproducible image comparison version" >&2
    exit 1
}

revision_one=$(required_value "$FIRST" source_revision)
revision_two=$(required_value "$SECOND" source_revision)
[[ "$revision_one" == "$revision_two" ]] || {
    echo "Reproducible image source revision mismatch" >&2
    exit 1
}

kernel_one=$(require_digest "$FIRST" artifact_hfoc_kernel_elf)
kernel_two=$(require_digest "$SECOND" artifact_hfoc_kernel_elf)
[[ "$kernel_one" == "$kernel_two" ]] || {
    echo "Reproducible image kernel digest mismatch" >&2
    exit 1
}

build_input_one=$(require_digest "$FIRST" artifact_hfoc_build_input)
build_input_two=$(require_digest "$SECOND" artifact_hfoc_build_input)
[[ "$build_input_one" == "$build_input_two" ]] || {
    echo "Reproducible image build input digest mismatch" >&2
    exit 1
}

image_one=$(require_digest "$FIRST" artifact_iso)
image_two=$(require_digest "$SECOND" artifact_iso)
[[ "$image_one" == "$image_two" ]] || {
    echo "Reproducible ISO digest mismatch" >&2
    exit 1
}

if [[ -n "$REPORT" ]]; then
    mkdir -p "$(dirname "$REPORT")"
    tmp="${REPORT}.tmp.$$"
    trap 'rm -f "$tmp"' EXIT
    {
        printf 'reproducibility_report_version=1\n'
        printf 'source_revision=%s\n' "$revision_one"
        printf 'artifact_hfoc_kernel_elf_first=%s\n' "$kernel_one"
        printf 'artifact_hfoc_kernel_elf_second=%s\n' "$kernel_two"
        printf 'artifact_hfoc_build_input_first=%s\n' "$build_input_one"
        printf 'artifact_hfoc_build_input_second=%s\n' "$build_input_two"
        printf 'artifact_iso_first=%s\n' "$image_one"
        printf 'artifact_iso_second=%s\n' "$image_two"
        printf 'verdict=REPRODUCIBLE\n'
    } >"$tmp"
    mv "$tmp" "$REPORT"
    trap - EXIT
    echo "Reproducible image comparison report: $REPORT"
fi

echo "Reproducible image comparison: OK"
