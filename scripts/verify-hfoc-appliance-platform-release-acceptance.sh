#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

MATRIX=${1:-$ROOT/release/hfoc-appliance-platform-release-acceptance.txt}
RELEASE_ID=${HFOC_RELEASE_PROFILE_ID:-HFOC-APPLIANCE-V1}
DOC=docs/hfoc-appliance-platform-release-acceptance.md

required_categories=(
    BOOT
    PERSISTENCE
    MANAGEMENT
    INSTALLER
    NETWORK
    SERVICES
    HTTPS
    SECURITY
    RECOVERY
    UPDATE
    RESET
    RELEASE
)

[[ -f "$MATRIX" ]] || {
    echo "Missing HFOC Appliance Platform release acceptance matrix: $MATRIX" >&2
    exit 1
}

required_value() {
    local key=$1
    local value

    if [[ $(grep -c "^${key}=" "$MATRIX") -ne 1 ]]; then
        echo "HFOC Appliance Platform release acceptance matrix is missing ${key}" >&2
        exit 1
    fi
    value=$(sed -n "s/^${key}=//p" "$MATRIX")
    if [[ -z "$value" ]]; then
        echo "HFOC Appliance Platform release acceptance matrix has empty ${key}" >&2
        exit 1
    fi
    printf '%s' "$value"
}

require_positive() {
    local key=$1
    local value

    value=$(required_value "$key")
    if [[ ! "$value" =~ ^[1-9][0-9]*$ ]]; then
        echo "HFOC Appliance Platform release acceptance matrix has invalid ${key}" >&2
        exit 1
    fi
    printf '%s' "$value"
}

matrix_version=$(require_positive hfoc_appliance_platform_release_acceptance_version)
[[ "$matrix_version" == "1" ]] || {
    echo "Unsupported HFOC Appliance Platform release acceptance matrix version" >&2
    exit 1
}

release_id=$(required_value target_release_id)
[[ "$release_id" == "$RELEASE_ID" ]] || {
    echo "HFOC Appliance Platform release acceptance release identity mismatch" >&2
    exit 1
}

check_count=$(require_positive check_count)
if (( check_count != 12 )); then
    echo "HFOC Appliance Platform release acceptance matrix check count is not 12" >&2
    exit 1
fi

while IFS= read -r line; do
    if [[ "$line" =~ ^check_([0-9]+)_ ]]; then
        check_number=${BASH_REMATCH[1]}
        if (( check_number < 1 || check_number > check_count )); then
            echo "HFOC Appliance Platform release acceptance matrix contains undeclared check ${check_number}" >&2
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
        echo "HFOC Appliance Platform release acceptance matrix check ${check_index} has ${field_count} fields" >&2
        exit 1
    fi

    check_id=$(required_value "${prefix}id")
    category=$(required_value "${prefix}category")
    name=$(required_value "${prefix}name")
    target=$(required_value "${prefix}target")
    marker=$(required_value "${prefix}marker")

    [[ "$check_id" =~ ^[A-Z0-9][A-Z0-9_-]*$ ]] || {
        echo "HFOC Appliance Platform release acceptance matrix check ${check_index} has invalid ID" >&2
        exit 1
    }
    [[ " ${required_categories[*]} " == *" $category "* ]] || {
        echo "HFOC Appliance Platform release acceptance matrix check ${check_index} has unsupported category" >&2
        exit 1
    }
    expected_category=${required_categories[$((check_index - 1))]}
    if [[ "$category" != "$expected_category" ]]; then
        echo "HFOC Appliance Platform release acceptance matrix check ${check_index} is out of order" >&2
        exit 1
    fi
    if [[ -n "${seen_categories[$category]+x}" ]]; then
        echo "HFOC Appliance Platform release acceptance matrix has duplicate category ${category}" >&2
        exit 1
    fi
    [[ "$name" =~ ^[a-z0-9][a-z0-9-]*$ ]] || {
        echo "HFOC Appliance Platform release acceptance matrix check ${check_index} has invalid name" >&2
        exit 1
    }
    [[ "$target" =~ ^test-[a-z0-9-]+$ ]] || {
        echo "HFOC Appliance Platform release acceptance matrix check ${check_index} has invalid target" >&2
        exit 1
    }
    if ! grep -Eq '^[A-Za-z0-9][A-Za-z0-9 :_./-]+$' <<<"$marker"; then
        echo "HFOC Appliance Platform release acceptance matrix check ${check_index} has invalid marker" >&2
        exit 1
    fi

    if [[ -n "${seen_ids[$check_id]+x}" ]]; then
        echo "HFOC Appliance Platform release acceptance matrix has duplicate check ID ${check_id}" >&2
        exit 1
    fi
    if [[ -n "${seen_names[$name]+x}" ]]; then
        echo "HFOC Appliance Platform release acceptance matrix has duplicate check name ${name}" >&2
        exit 1
    fi
    if [[ -n "${seen_targets[$target]+x}" ]]; then
        echo "HFOC Appliance Platform release acceptance matrix has duplicate check target ${target}" >&2
        exit 1
    fi
    seen_ids[$check_id]=1
    seen_names[$name]=1
    seen_targets[$target]=1
    seen_categories[$category]=1
done

find_proof_marker() {
    local marker=$1
    local file

    for file in scripts/test-*.sh tests/test-*.sh tests/*.c; do
        [[ -f "$file" ]] || continue
        [[ "$file" != *hfoc-appliance-platform-release-acceptance* ]] || continue
        if grep -Fq -- "$marker" "$file"; then
            return 0
        fi
    done
    return 1
}

[[ -f "$DOC" ]] || {
    echo "Missing HFOC Appliance Platform release acceptance doc: $DOC" >&2
    exit 1
}

for ((check_index = 1; check_index <= check_count; ++check_index)); do
    prefix="check_${check_index}_"
    target=$(required_value "${prefix}target")
    marker=$(required_value "${prefix}marker")

    grep -Eq "^${target}:" Makefile || {
        echo "Makefile does not declare platform release target ${target}" >&2
        exit 1
    }
    if ! find_proof_marker "$marker"; then
        echo "Platform release marker is missing from real proof artifacts: ${marker}" >&2
        exit 1
    fi
    grep -Fq -- "$target" "$DOC" || {
        echo "Platform release doc does not name target ${target}" >&2
        exit 1
    }
    grep -Fq -- "$marker" "$DOC" || {
        echo "Platform release doc does not record marker ${marker}" >&2
        exit 1
    }
done

echo "HFOC Appliance Platform release acceptance verification: OK"
