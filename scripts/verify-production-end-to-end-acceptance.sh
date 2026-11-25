#!/usr/bin/env bash
# Strict verifier for the ITEM-369 production end-to-end acceptance matrix.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

MATRIX=${1:-$ROOT/release/hfoc-production-end-to-end-acceptance.txt}
RELEASE_ID=${HFOC_RELEASE_PROFILE_ID:-HFOC-APPLIANCE-V1}
DOC=docs/hfoc-production-end-to-end-acceptance.md
HARDWARE_REPORT=release/hfoc-hardware-acceptance-report.md

required_stages=(
    MEDIA_DIGEST
    INSTALLER
    STORAGE_STATE
    FIRST_BOOT
    NETWORK_LINK
    DHCP_DNS
    HTTPS_SERVICE
    AUTH
    MANAGEMENT
    MUTATION
    UPDATE
    REBOOT_PERSISTENCE
    RECOVERY
    RECOVERY_EXECUTOR
    SIGNED_MEDIA
    SIGNED_BOOT
    STORAGE_ENCRYPTION
    TIME_SYNC
    TLS_LIFECYCLE
    UPDATE_ROLLBACK
    AUDIT_EXPORT
    SUPPORT_BUNDLE
    SBOM_RELEASE
    HARDWARE_ACCEPTANCE
    LICENSE_PAYLOAD
)

# Stages owned by the production integration and productization items. Every
# one of them is implemented, so the matrix may not quietly drop any of them to
# NOT_TESTED: a release decision reads this matrix, and a stage that disappears
# is a gap that must be argued, not an omission. Only SBOM_RELEASE (ITEM-380)
# and HARDWARE_ACCEPTANCE (ITEM-381) may stay NOT_TESTED in this wave.
required_executable_stages=(
    MANAGEMENT
    MUTATION
    UPDATE
    RECOVERY
    RECOVERY_EXECUTOR
    SIGNED_MEDIA
    SIGNED_BOOT
    STORAGE_ENCRYPTION
    TIME_SYNC
    TLS_LIFECYCLE
    UPDATE_ROLLBACK
    AUDIT_EXPORT
    SUPPORT_BUNDLE
    SBOM_RELEASE
    LICENSE_PAYLOAD
)
declare -A required_executable=()
for required_executable_stage in "${required_executable_stages[@]}"; do
    required_executable[$required_executable_stage]=1
done

[[ -f "$MATRIX" ]] || {
    echo "Missing production end-to-end acceptance matrix: $MATRIX" >&2
    exit 1
}

required_value() {
    local key=$1
    local value

    if [[ $(grep -c "^${key}=" "$MATRIX") -ne 1 ]]; then
        echo "Production end-to-end acceptance matrix is missing ${key}" >&2
        exit 1
    fi
    value=$(sed -n "s/^${key}=//p" "$MATRIX")
    if [[ -z "$value" ]]; then
        echo "Production end-to-end acceptance matrix has empty ${key}" >&2
        exit 1
    fi
    printf '%s' "$value"
}

require_positive() {
    local key=$1
    local value

    value=$(required_value "$key")
    if [[ ! "$value" =~ ^[1-9][0-9]*$ ]]; then
        echo "Production end-to-end acceptance matrix has invalid ${key}" >&2
        exit 1
    fi
    printf '%s' "$value"
}

matrix_version=$(require_positive hfoc_production_end_to_end_acceptance_version)
[[ "$matrix_version" == "2" ]] || {
    echo "Unsupported production end-to-end acceptance matrix version" >&2
    exit 1
}

release_id=$(required_value target_release_id)
[[ "$release_id" == "$RELEASE_ID" ]] || {
    echo "Production end-to-end acceptance release identity mismatch" >&2
    exit 1
}

stage_count=$(require_positive stage_count)
if (( stage_count != ${#required_stages[@]} )); then
    echo "Production end-to-end acceptance matrix stage count is not ${#required_stages[@]}" >&2
    exit 1
fi

while IFS= read -r line; do
    if [[ "$line" =~ ^stage_([0-9]+)_ ]]; then
        stage_number=${BASH_REMATCH[1]}
        if (( stage_number < 1 || stage_number > stage_count )); then
            echo "Production end-to-end acceptance matrix contains undeclared stage ${stage_number}" >&2
            exit 1
        fi
    fi
done < "$MATRIX"

declare -A seen_ids
declare -A seen_names

for ((stage_index = 1; stage_index <= stage_count; ++stage_index)); do
    prefix="stage_${stage_index}_"
    stage_id=$(required_value "${prefix}id")
    name=$(required_value "${prefix}name")
    owner=$(required_value "${prefix}owner")
    state=$(required_value "${prefix}state")
    kind=$(required_value "${prefix}kind")

    expected_id=${required_stages[$((stage_index - 1))]}
    if [[ "$stage_id" != "$expected_id" ]]; then
        echo "Production end-to-end acceptance matrix stage ${stage_index} is out of order" >&2
        exit 1
    fi
    [[ "$stage_id" =~ ^[A-Z0-9][A-Z0-9_]*$ ]] || {
        echo "Production end-to-end acceptance matrix stage ${stage_index} has invalid ID" >&2
        exit 1
    }
    [[ "$name" =~ ^[a-z0-9][a-z0-9-]*$ ]] || {
        echo "Production end-to-end acceptance matrix stage ${stage_index} has invalid name" >&2
        exit 1
    }
    [[ "$owner" =~ ^ITEM-[0-9]+[A-Z]?$ ]] || {
        echo "Production end-to-end acceptance matrix stage ${stage_index} has invalid owner" >&2
        exit 1
    }
    [[ "$state" == "PROVEN" || "$state" == "DEGRADED" || "$state" == "NOT_TESTED" ]] || {
        echo "Production end-to-end acceptance matrix stage ${stage_index} has unsupported state" >&2
        exit 1
    }
    [[ "$kind" == "run" || "$kind" == "gate" || "$kind" == "none" ]] || {
        echo "Production end-to-end acceptance matrix stage ${stage_index} has unsupported kind" >&2
        exit 1
    }
    if [[ -n "${seen_ids[$stage_id]+x}" ]]; then
        echo "Production end-to-end acceptance matrix has duplicate stage ID ${stage_id}" >&2
        exit 1
    fi
    if [[ -n "${seen_names[$name]+x}" ]]; then
        echo "Production end-to-end acceptance matrix has duplicate stage name ${name}" >&2
        exit 1
    fi
    seen_ids[$stage_id]=1
    seen_names[$name]=1

    if [[ -n "${required_executable[$stage_id]+x}" && "$state" == "NOT_TESTED" ]]; then
        echo "Production end-to-end acceptance matrix stage ${stage_index} ${stage_id} is implemented and may not be skipped as NOT_TESTED" >&2
        exit 1
    fi
    if [[ "$state" == "NOT_TESTED" ]]; then
        field_count=$(grep -c "^${prefix}" "$MATRIX" || true)
        if [[ "$field_count" != "6" ]]; then
            echo "Production end-to-end acceptance matrix stage ${stage_index} has ${field_count} fields" >&2
            exit 1
        fi
        if [[ "$kind" != "none" ]]; then
            echo "Production end-to-end acceptance matrix stage ${stage_index} is not tested but executable" >&2
            exit 1
        fi
        if [[ $(grep -c "^${prefix}target=" "$MATRIX" || true) -ne 0 ||
              $(grep -c "^${prefix}marker=" "$MATRIX" || true) -ne 0 ]]; then
            echo "Production end-to-end acceptance matrix stage ${stage_index} publishes a target while not tested" >&2
            exit 1
        fi
        required_value "${prefix}reason" >/dev/null
    elif [[ "$state" == "DEGRADED" ]]; then
        field_count=$(grep -c "^${prefix}" "$MATRIX" || true)
        if [[ "$field_count" != "8" ]]; then
            echo "Production end-to-end acceptance matrix stage ${stage_index} has ${field_count} fields" >&2
            exit 1
        fi
        if [[ "$kind" == "none" ]]; then
            echo "Production end-to-end acceptance matrix stage ${stage_index} has no runner kind" >&2
            exit 1
        fi
        required_value "${prefix}reason" >/dev/null
    else
        field_count=$(grep -c "^${prefix}" "$MATRIX" || true)
        # ITEM-587: a stage may name the independent parts its run target
        # composes with `_targets=`; that is one extra field, and every part is
        # validated below with the same rules as the target itself.
        if [[ "$field_count" != "7" && "$field_count" != "8" ]]; then
            echo "Production end-to-end acceptance matrix stage ${stage_index} has ${field_count} fields" >&2
            exit 1
        fi
        if [[ "$kind" == "none" ]]; then
            echo "Production end-to-end acceptance matrix stage ${stage_index} has no runner kind" >&2
            exit 1
        fi
        target=$(required_value "${prefix}target")
        marker=$(required_value "${prefix}marker")
        [[ "$target" =~ ^test-[a-z0-9-]+$ ]] || {
            echo "Production end-to-end acceptance matrix stage ${stage_index} has invalid target" >&2
            exit 1
        }
        if ! grep -Eq '^[A-Za-z0-9][A-Za-z0-9 :_./-]+$' <<<"$marker"; then
            echo "Production end-to-end acceptance matrix stage ${stage_index} has invalid marker" >&2
            exit 1
        fi
        part_line=$(sed -n "s/^${prefix}targets=//p" "$MATRIX")
        if [[ -n $part_line ]]; then
            for part in $part_line; do
                [[ "$part" =~ ^test-[a-z0-9-]+$ ]] || {
                    echo "Production end-to-end acceptance matrix stage ${stage_index} has invalid part target" >&2
                    exit 1
                }
                grep -Eq "^${part}:" Makefile || {
                    echo "Makefile does not declare production acceptance part ${part}" >&2
                    exit 1
                }
                grep -Fq -- "$part" "$DOC" || {
                    echo "Production acceptance doc does not name part ${part}" >&2
                    exit 1
                }
            done
        fi
    fi
done

find_proof_marker() {
    local marker=$1
    local file

    for file in scripts/*.sh tests/test-*.sh tests/*.c; do
        [[ -f "$file" ]] || continue
        [[ "$file" != *production-end-to-end-acceptance* ]] || continue
        if grep -Fq -- "$marker" "$file"; then
            return 0
        fi
    done
    return 1
}

[[ -f "$DOC" ]] || {
    echo "Missing production end-to-end acceptance doc: $DOC" >&2
    exit 1
}

for ((stage_index = 1; stage_index <= stage_count; ++stage_index)); do
    prefix="stage_${stage_index}_"
    state=$(required_value "${prefix}state")

    if [[ "$state" == "NOT_TESTED" ]]; then
        continue
    fi
    target=$(required_value "${prefix}target")
    marker=$(required_value "${prefix}marker")

    grep -Eq "^${target}:" Makefile || {
        echo "Makefile does not declare production acceptance target ${target}" >&2
        exit 1
    }
    if ! find_proof_marker "$marker"; then
        echo "Production acceptance marker is missing from real proof artifacts: ${marker}" >&2
        exit 1
    fi
    grep -Fq -- "$target" "$DOC" || {
        echo "Production acceptance doc does not name target ${target}" >&2
        exit 1
    }
    grep -Fq -- "$marker" "$DOC" || {
        echo "Production acceptance doc does not record marker ${marker}" >&2
        exit 1
    }
done

[[ -f "$HARDWARE_REPORT" ]] || {
    echo "Missing hardware acceptance report: $HARDWARE_REPORT" >&2
    exit 1
}
hardware_state=$(required_value stage_24_state)
hardware_owner=$(required_value stage_24_owner)
if [[ "$hardware_state" != "NOT_TESTED" || "$hardware_owner" != "ITEM-381" ]]; then
    echo "Hardware acceptance must stay NOT_TESTED and owned by ITEM-381" >&2
    exit 1
fi
grep -Fq '**Status:** NOT TESTED' "$HARDWARE_REPORT" || {
    echo "Hardware acceptance report no longer records NOT TESTED" >&2
    exit 1
}
if grep -Eq '^\| PASS \|' "$HARDWARE_REPORT"; then
    echo "Hardware acceptance report fabricates a PASS row" >&2
    exit 1
fi

echo "Production end-to-end acceptance verification: OK"
