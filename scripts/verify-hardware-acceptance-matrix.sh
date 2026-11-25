#!/usr/bin/env bash
# ITEM-381 strict verifier for the hardware acceptance matrix.
#
# The matrix is the machine-readable plan a hardware run follows: every
# product-path stage of the production end-to-end acceptance matrix observed on
# the acceptance host, plus the three hardware-only stages that have no CI
# equivalent. It must agree with the production matrix it mirrors, name the
# declared acceptance identity that the guard enforces, and carry a real
# software counterpart proof for every stage. The matrix is a plan, not a
# result: it carries no status fields, and the run-scoped report carries the
# observations.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

MATRIX=${1:-$ROOT/release/hfoc-hardware-acceptance-matrix.txt}
GUARD=scripts/hfoc-physical-sata-continue.sh
DOC=docs/hfoc-hardware-acceptance.md
RELEASE_ID=${HFOC_RELEASE_PROFILE_ID:-HFOC-APPLIANCE-V1}

hardware_only_stages=(DUAL_BOOT_UNINSTALL POWER_CYCLE FACTORY_RESET)
excluded_production_stages=(SBOM_RELEASE HARDWARE_ACCEPTANCE)

[[ -f "$MATRIX" ]] || {
    echo "Missing hardware acceptance matrix: $MATRIX" >&2
    exit 1
}

required_value() {
    local key=$1
    local value

    if [[ $(grep -c "^${key}=" "$MATRIX") -ne 1 ]]; then
        echo "Hardware acceptance matrix is missing ${key}" >&2
        exit 1
    fi
    value=$(sed -n "s/^${key}=//p" "$MATRIX")
    if [[ -z "$value" ]]; then
        echo "Hardware acceptance matrix has empty ${key}" >&2
        exit 1
    fi
    printf '%s' "$value"
}

matrix_version=$(required_value hfoc_hardware_acceptance_matrix_version)
[[ "$matrix_version" == "1" ]] || {
    echo "Unsupported hardware acceptance matrix version" >&2
    exit 1
}

release_id=$(required_value target_release_id)
[[ "$release_id" == "$RELEASE_ID" ]] || {
    echo "Hardware acceptance release identity mismatch" >&2
    exit 1
}

acceptance_device=$(required_value acceptance_device)
declared_model=$(required_value declared_model)
declared_serial=$(required_value declared_serial)
declared_root=$(required_value declared_root)
source_matrix=$(required_value source_matrix)
stage_count=$(required_value stage_count)

[[ "$acceptance_device" =~ ^/dev/[a-z]+$ ]] || {
    echo "Hardware acceptance matrix has an invalid acceptance device" >&2
    exit 1
}
[[ "$declared_model" =~ ^[A-Za-z0-9*?._[:space:]-]+$ ]] || {
    echo "Hardware acceptance matrix has an invalid declared model" >&2
    exit 1
}
[[ "$declared_serial" =~ ^[A-Za-z0-9]+$ ]] || {
    echo "Hardware acceptance matrix has an invalid declared serial" >&2
    exit 1
}
[[ "$declared_root" =~ ^/dev/[a-z0-9]+$ ]] || {
    echo "Hardware acceptance matrix has an invalid declared root" >&2
    exit 1
}

# The declared identity is not copied on trust: it must equal the constants the
# identity guard in scripts/hfoc-physical-sata-continue.sh enforces, so a matrix
# can never widen the standing permission to write the acceptance disk.
guard_device=$(sed -n 's/^HFOC_PHYSICAL_SATA_DISK=//p' "$GUARD")
guard_model=$(sed -n "s/^HFOC_PHYSICAL_SATA_EXPECTED_MODEL='\\(.*\\)'$/\\1/p" \
    "$GUARD")
guard_serial=$(sed -n "s/^HFOC_PHYSICAL_SATA_EXPECTED_SERIAL='\\(.*\\)'$/\\1/p" \
    "$GUARD")
guard_root=$(sed -n "s/^HFOC_PHYSICAL_SATA_EXPECTED_ROOT='\\(.*\\)'$/\\1/p" \
    "$GUARD")
for pair in "device:$acceptance_device:$guard_device" \
            "model:$declared_model:$guard_model" \
            "serial:$declared_serial:$guard_serial" \
            "root:$declared_root:$guard_root"; do
    IFS=: read -r label matrix_value guard_value <<<"$pair"
    if [[ -z "$guard_value" || "$matrix_value" != "$guard_value" ]]; then
        echo "Hardware acceptance matrix declared ${label} does not match the identity guard" >&2
        exit 1
    fi
done

[[ -f "$source_matrix" ]] || {
    echo "Hardware acceptance matrix names a missing source matrix: $source_matrix" >&2
    exit 1
}
source_count=$(sed -n 's/^stage_count=//p' "$source_matrix")
[[ "$source_count" =~ ^[1-9][0-9]*$ ]] || {
    echo "Hardware acceptance source matrix has an invalid stage count" >&2
    exit 1
}

expected_product_stages=()
for ((index = 1; index <= source_count; ++index)); do
    id=$(sed -n "s/^stage_${index}_id=//p" "$source_matrix")
    skip=0
    for excluded in "${excluded_production_stages[@]}"; do
        [[ "$id" == "$excluded" ]] && skip=1
    done
    (( skip == 1 )) && continue
    expected_product_stages+=("$index:$id")
done
expected_stage_count=$(( ${#expected_product_stages[@]} + ${#hardware_only_stages[@]} ))
if [[ "$stage_count" != "$expected_stage_count" ]]; then
    echo "Hardware acceptance matrix declares ${stage_count} stages, expected ${expected_stage_count}" >&2
    exit 1
fi

find_proof_marker() {
    local marker=$1
    local file

    for file in scripts/*.sh tests/test-*.sh tests/*.c; do
        [[ -f "$file" ]] || continue
        [[ "$file" != *hardware-acceptance* ]] || continue
        if grep -Fq -- "$marker" "$file"; then
            return 0
        fi
    done
    return 1
}

declare -A seen_ids
for ((stage_index = 1; stage_index <= stage_count; ++stage_index)); do
    prefix="stage_${stage_index}_"
    field_count=$(grep -c "^${prefix}" "$MATRIX" || true)
    if [[ "$field_count" != "7" ]]; then
        echo "Hardware acceptance matrix stage ${stage_index} has ${field_count} fields" >&2
        exit 1
    fi
    if [[ $(grep -c "^${prefix}status=" "$MATRIX" || true) -ne 0 ]]; then
        echo "Hardware acceptance matrix stage ${stage_index} carries a result" >&2
        exit 1
    fi
    stage_id=$(required_value "${prefix}id")
    name=$(required_value "${prefix}name")
    owner=$(required_value "${prefix}owner")
    kind=$(required_value "${prefix}kind")
    software_target=$(required_value "${prefix}software_target")
    software_marker=$(sed -n "s/^${prefix}software_marker=//p" "$MATRIX")
    observation=$(required_value "${prefix}hardware_observation")

    [[ "$stage_id" =~ ^[A-Z0-9][A-Z0-9_]*$ ]] || {
        echo "Hardware acceptance matrix stage ${stage_index} has an invalid ID" >&2
        exit 1
    }
    [[ "$name" =~ ^[a-z0-9][a-z0-9-]*$ ]] || {
        echo "Hardware acceptance matrix stage ${stage_index} has an invalid name" >&2
        exit 1
    }
    [[ "$owner" =~ ^ITEM-[0-9]+[A-Z]?$ ]] || {
        echo "Hardware acceptance matrix stage ${stage_index} has an invalid owner" >&2
        exit 1
    }
    if (( ${#observation} < 8 )); then
        echo "Hardware acceptance matrix stage ${stage_index} has an uninformative hardware observation" >&2
        exit 1
    fi
    # ITEM-401/ITEM-506: the AUTH stage is only actionable when the plan
    # carries the local account steps the ordinary production path needs, and
    # the MANAGEMENT stage must state the authenticated read-only targets.
    if [[ "$stage_id" == "AUTH" ]]; then
        [[ "$observation" == *"CREDENTIALS"* &&
           "$observation" == *"LOGIN"* &&
           "$observation" == *"Authorization: Bearer"* ]] || {
            echo "Hardware acceptance AUTH stage lost the ITEM-506 account steps" >&2
            exit 1
        }
    fi
    if [[ "$stage_id" == "MANAGEMENT" ]]; then
        [[ "$observation" == *"/api/v1/health"* ]] || {
            echo "Hardware acceptance MANAGEMENT stage lost the authenticated read-only targets" >&2
            exit 1
        }
    fi
    [[ "$kind" == "both" || "$kind" == "hardware" ]] || {
        echo "Hardware acceptance matrix stage ${stage_index} has an unsupported kind" >&2
        exit 1
    }
    [[ -n "${seen_ids[$stage_id]+x}" ]] && {
        echo "Hardware acceptance matrix has duplicate stage ID ${stage_id}" >&2
        exit 1
    }
    seen_ids[$stage_id]=1

    [[ "$software_target" =~ ^test-[a-z0-9-]+$ ]] || {
        echo "Hardware acceptance matrix stage ${stage_index} has an invalid software target" >&2
        exit 1
    }
    grep -Eq "^${software_target}:" Makefile || {
        echo "Hardware acceptance matrix stage ${stage_index} names a missing target ${software_target}" >&2
        exit 1
    }
    [[ -n "$software_marker" ]] || {
        echo "Hardware acceptance matrix stage ${stage_index} has no software marker" >&2
        exit 1
    }
    if ! find_proof_marker "$software_marker"; then
        echo "Hardware acceptance software marker is missing from real proof artifacts: ${software_marker}" >&2
        exit 1
    fi

    if (( stage_index <= ${#expected_product_stages[@]} )); then
        production_index=${expected_product_stages[$((stage_index - 1))]%%:*}
        production_id=${expected_product_stages[$((stage_index - 1))]##*:}
        if [[ "$stage_id" != "$production_id" ]]; then
            echo "Hardware acceptance matrix stage ${stage_index} is out of order" >&2
            exit 1
        fi
        production_owner=$(sed -n "s/^stage_${production_index}_owner=//p" \
            "$source_matrix")
        production_target=$(sed -n "s/^stage_${production_index}_target=//p" \
            "$source_matrix")
        production_marker=$(sed -n "s/^stage_${production_index}_marker=//p" \
            "$source_matrix")
        if [[ "$owner" != "$production_owner" ||
              "$software_target" != "$production_target" ||
              "$software_marker" != "$production_marker" ]]; then
            echo "Hardware acceptance matrix stage ${stage_index} does not match the production matrix" >&2
            exit 1
        fi
    else
        hardware_index=$((stage_index - ${#expected_product_stages[@]} - 1))
        expected_id=${hardware_only_stages[$hardware_index]}
        if [[ "$stage_id" != "$expected_id" || "$kind" != "hardware" ]]; then
            echo "Hardware acceptance matrix stage ${stage_index} is not the declared hardware-only stage ${expected_id}" >&2
            exit 1
        fi
    fi
done

[[ -f "$DOC" ]] || {
    echo "Missing hardware acceptance doc: $DOC" >&2
    exit 1
}
grep -Fq -- "$acceptance_device" "$DOC" || {
    echo "Hardware acceptance doc does not name the acceptance device" >&2
    exit 1
}

echo "Hardware acceptance matrix verification: OK"
