#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
MATRIX=${1:-$ROOT/release/https-vertical-slice-acceptance.txt}
TARGET_SLICE_ID=${HFOC_HTTPS_VERTICAL_SLICE_ID:-HFOC-HTTPS-VERTICAL-SLICE}

required_categories=(
    CLIENT
    RESPONSE
    MALFORMED
    SIZE
    EXHAUSTION
    ENTROPY
    CERTIFICATE
    SECRET
    RESTART
)

[[ -f "$MATRIX" ]] || {
    echo "Missing HTTPS vertical-slice acceptance matrix: $MATRIX" >&2
    exit 1
}

required_value() {
    local key=$1
    local value

    if [[ $(grep -c "^${key}=" "$MATRIX") -ne 1 ]]; then
        echo "HTTPS vertical-slice acceptance matrix is missing ${key}" >&2
        exit 1
    fi
    value=$(sed -n "s/^${key}=//p" "$MATRIX")
    if [[ -z "$value" ]]; then
        echo "HTTPS vertical-slice acceptance matrix has empty ${key}" >&2
        exit 1
    fi
    printf '%s' "$value"
}

require_positive() {
    local key=$1
    local value

    value=$(required_value "$key")
    if [[ ! "$value" =~ ^[1-9][0-9]*$ ]]; then
        echo "HTTPS vertical-slice acceptance matrix has invalid ${key}" >&2
        exit 1
    fi
    printf '%s' "$value"
}

matrix_version=$(require_positive https_vertical_slice_acceptance_version)
[[ "$matrix_version" == "1" ]] || {
    echo "Unsupported HTTPS vertical-slice acceptance matrix version" >&2
    exit 1
}

slice_id=$(required_value target_slice_id)
[[ "$slice_id" == "$TARGET_SLICE_ID" ]] || {
    echo "HTTPS vertical-slice acceptance matrix slice identity mismatch" >&2
    exit 1
}

check_count=$(require_positive check_count)
if (( check_count != 9 )); then
    echo "HTTPS vertical-slice acceptance matrix check count is not 9" >&2
    exit 1
fi

while IFS= read -r line; do
    if [[ "$line" =~ ^check_([0-9]+)_ ]]; then
        check_number=${BASH_REMATCH[1]}
        if (( check_number < 1 || check_number > check_count )); then
            echo "HTTPS vertical-slice acceptance matrix contains undeclared check ${check_number}" >&2
            exit 1
        fi
    fi
done < "$MATRIX"

declare -A seen_ids
declare -A seen_names
declare -A seen_targets
declare -A seen_categories

for ((check_index = 1; check_index <= check_count; ++check_index)); do
    prefix="check_${check_index}_"
    field_count=$(grep -c "^${prefix}" "$MATRIX" || true)
    if [[ "$field_count" != "5" ]]; then
        echo "HTTPS vertical-slice acceptance matrix check ${check_index} has ${field_count} fields" >&2
        exit 1
    fi

    check_id=$(required_value "${prefix}id")
    category=$(required_value "${prefix}category")
    name=$(required_value "${prefix}name")
    target=$(required_value "${prefix}target")
    marker=$(required_value "${prefix}marker")

    [[ "$check_id" =~ ^[A-Z0-9_][A-Z0-9_-]*$ ]] || {
        echo "HTTPS vertical-slice acceptance matrix check ${check_index} has invalid ID" >&2
        exit 1
    }
    [[ " ${required_categories[*]} " == *" $category "* ]] || {
        echo "HTTPS vertical-slice acceptance matrix check ${check_index} has unsupported category" >&2
        exit 1
    }
    expected_category=${required_categories[$((check_index - 1))]}
    if [[ "$category" != "$expected_category" ]]; then
        echo "HTTPS vertical-slice acceptance matrix check ${check_index} is out of order" >&2
        exit 1
    fi
    if [[ -n "${seen_categories[$category]+x}" ]]; then
        echo "HTTPS vertical-slice acceptance matrix has duplicate category ${category}" >&2
        exit 1
    fi
    [[ "$name" =~ ^[a-z0-9][a-z0-9-]*$ ]] || {
        echo "HTTPS vertical-slice acceptance matrix check ${check_index} has invalid name" >&2
        exit 1
    }
    [[ "$target" =~ ^test-[a-z0-9-]+$ ]] || {
        echo "HTTPS vertical-slice acceptance matrix check ${check_index} has invalid target" >&2
        exit 1
    }
    if ! grep -Eq '^[A-Za-z0-9][A-Za-z0-9 :_./-]+$' <<<"$marker"; then
        echo "HTTPS vertical-slice acceptance matrix check ${check_index} has invalid marker" >&2
        exit 1
    fi

    if [[ -n "${seen_ids[$check_id]+x}" ]]; then
        echo "HTTPS vertical-slice acceptance matrix has duplicate check ID ${check_id}" >&2
        exit 1
    fi
    if [[ -n "${seen_names[$name]+x}" ]]; then
        echo "HTTPS vertical-slice acceptance matrix has duplicate check name ${name}" >&2
        exit 1
    fi
    if [[ -n "${seen_targets[$target]+x}" ]]; then
        echo "HTTPS vertical-slice acceptance matrix has duplicate check target ${target}" >&2
        exit 1
    fi
    seen_ids[$check_id]=1
    seen_names[$name]=1
    seen_targets[$target]=1
    seen_categories[$category]=1
done

for category in "${required_categories[@]}"; do
    if [[ -z "${seen_categories[$category]+x}" ]]; then
        echo "HTTPS vertical-slice acceptance matrix is missing category ${category}" >&2
        exit 1
    fi
done

echo "HTTPS vertical-slice acceptance gate verification: OK"
