#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

MATRIX=${1:-$ROOT/release/hfoc-nvme-production-acceptance.txt}
RELEASE_ID=${HFOC_RELEASE_PROFILE_ID:-HFOC-APPLIANCE-V1}
DOC=docs/hfoc-integrated-installer-dual-boot-management-acceptance.md

required_categories=(
    NVME_HOST
    NVME_STATE_INTEGRATION
    PHYSICAL_STORAGE_QEMU
)

[[ -f "$MATRIX" ]] || {
    echo "Missing NVMe production acceptance matrix: $MATRIX" >&2
    exit 1
}

required_value() {
    local key=$1
    local value

    if [[ $(grep -c "^${key}=" "$MATRIX") -ne 1 ]]; then
        echo "NVMe production acceptance matrix is missing ${key}" >&2
        exit 1
    fi
    value=$(sed -n "s/^${key}=//p" "$MATRIX")
    if [[ -z "$value" ]]; then
        echo "NVMe production acceptance matrix has empty ${key}" >&2
        exit 1
    fi
    printf '%s' "$value"
}

require_positive() {
    local key=$1
    local value

    value=$(required_value "$key")
    if [[ ! "$value" =~ ^[1-9][0-9]*$ ]]; then
        echo "NVMe production acceptance matrix has invalid ${key}" >&2
        exit 1
    fi
    printf '%s' "$value"
}

matrix_version=$(require_positive hfoc_nvme_production_acceptance_version)
[[ "$matrix_version" == "1" ]] || {
    echo "Unsupported NVMe production acceptance matrix version" >&2
    exit 1
}

release_id=$(required_value target_release_id)
[[ "$release_id" == "$RELEASE_ID" ]] || {
    echo "NVMe production acceptance release identity mismatch" >&2
    exit 1
}

[[ "$(required_value acceptance_class)" == "SOFTWARE_PROOF" ]] || {
    echo "NVMe production acceptance must remain SOFTWARE_PROOF" >&2
    exit 1
}
[[ "$(required_value hardware_status)" == "NOT_TESTED" ]] || {
    echo "Physical NVMe hardware acceptance must remain NOT_TESTED" >&2
    exit 1
}
[[ "$(required_value supported_v1_namespace_count)" == "1" ]] || {
    echo "ITEM-348 V1 profile accepts exactly one supported namespace" >&2
    exit 1
}
[[ "$(required_value supported_v1_block_size)" == "512" ]] || {
    echo "ITEM-348 V1 HFOC state view requires 512-byte blocks" >&2
    exit 1
}
[[ "$(required_value gpt_mutation)" == "NONE" ]] || {
    echo "ITEM-348 must not mutate GPT structures" >&2
    exit 1
}
[[ "$(required_value efi_nvram_mutation)" == "NONE" ]] || {
    echo "ITEM-348 must not mutate EFI/NVRAM" >&2
    exit 1
}

check_count=$(require_positive check_count)
if (( check_count != 3 )); then
    echo "NVMe production acceptance matrix check count is not 3" >&2
    exit 1
fi

while IFS= read -r line; do
    if [[ "$line" =~ ^check_([0-9]+)_ ]]; then
        check_number=${BASH_REMATCH[1]}
        if (( check_number < 1 || check_number > check_count )); then
            echo "NVMe production acceptance matrix contains undeclared check ${check_number}" >&2
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
        echo "NVMe production acceptance matrix check ${check_index} has ${field_count} fields" >&2
        exit 1
    fi

    check_id=$(required_value "${prefix}id")
    category=$(required_value "${prefix}category")
    name=$(required_value "${prefix}name")
    target=$(required_value "${prefix}target")
    marker=$(required_value "${prefix}marker")

    [[ "$check_id" =~ ^[A-Z0-9][A-Z0-9_-]*$ ]] || {
        echo "NVMe production acceptance matrix check ${check_index} has invalid ID" >&2
        exit 1
    }
    [[ " ${required_categories[*]} " == *" $category "* ]] || {
        echo "NVMe production acceptance matrix check ${check_index} has unsupported category" >&2
        exit 1
    }
    expected_category=${required_categories[$((check_index - 1))]}
    if [[ "$category" != "$expected_category" ]]; then
        echo "NVMe production acceptance matrix check ${check_index} is out of order" >&2
        exit 1
    fi
    if [[ -n "${seen_categories[$category]+x}" ]]; then
        echo "NVMe production acceptance matrix has duplicate category ${category}" >&2
        exit 1
    fi
    [[ "$name" =~ ^[a-z0-9][a-z0-9-]*$ ]] || {
        echo "NVMe production acceptance matrix check ${check_index} has invalid name" >&2
        exit 1
    }
    [[ "$target" =~ ^test-[a-z0-9-]+$ ]] || {
        echo "NVMe production acceptance matrix check ${check_index} has invalid target" >&2
        exit 1
    }
    if ! grep -Eq '^[A-Za-z0-9][A-Za-z0-9 :_./-]+$' <<<"$marker"; then
        echo "NVMe production acceptance matrix check ${check_index} has invalid marker" >&2
        exit 1
    fi

    if [[ -n "${seen_ids[$check_id]+x}" ]]; then
        echo "NVMe production acceptance matrix has duplicate check ID ${check_id}" >&2
        exit 1
    fi
    if [[ -n "${seen_names[$name]+x}" ]]; then
        echo "NVMe production acceptance matrix has duplicate check name ${name}" >&2
        exit 1
    fi
    if [[ -n "${seen_targets[$target]+x}" ]]; then
        echo "NVMe production acceptance matrix has duplicate check target ${target}" >&2
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
        [[ "$file" != *test-hfoc-nvme-production-acceptance* ]] || continue
        if grep -Fq -- "$marker" "$file"; then
            return 0
        fi
    done
    return 1
}

[[ -f "$DOC" ]] || {
    echo "Missing integrated acceptance doc: $DOC" >&2
    exit 1
}
grep -Fq -- 'QEMU and host proofs are not physical-hardware acceptance' \
    "$DOC" || {
    echo "Integrated acceptance doc must state QEMU/host boundary" >&2
    exit 1
}
grep -Fq -- 'must remain NOT_TESTED' "$DOC" || {
    echo "Integrated acceptance doc must state physical NOT_TESTED boundary" >&2
    exit 1
}

for ((check_index = 1; check_index <= check_count; ++check_index)); do
    prefix="check_${check_index}_"
    target=$(required_value "${prefix}target")
    marker=$(required_value "${prefix}marker")

    grep -Eq "^${target}:" Makefile || {
        echo "Makefile does not declare NVMe production acceptance target ${target}" >&2
        exit 1
    }
    if ! find_proof_marker "$marker"; then
        echo "NVMe production acceptance marker is missing from proof artifacts: ${marker}" >&2
        exit 1
    fi
done

echo "HFOC NVMe production acceptance verification: OK"
