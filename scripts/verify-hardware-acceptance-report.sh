#!/usr/bin/env bash
# ITEM-381 strict verifier for the machine-readable hardware acceptance report.
#
# A hardware report is a hardware proof: it must quote the pre-write audit line,
# prove that the observed acceptance identity equals the declared identity the
# guard enforces, and carry one complete stage block per hardware acceptance
# stage. A stage may only be PASS when a human hardware observation backs it,
# and the gate may only be ACCEPT when no stage is left unobserved.
#
#   ACCEPT      every stage is PASS or DEGRADED
#   INCOMPLETE  no stage failed, but at least one is NOT_TESTED
#   REJECT      at least one stage is FAIL or UNOBSERVED
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

if (( $# < 1 || $# > 2 )); then
    echo "usage: $0 <report.txt> [matrix.txt]" >&2
    exit 2
fi
REPORT=$1
MATRIX=${2:-}
RELEASE_ID=${HFOC_RELEASE_PROFILE_ID:-HFOC-APPLIANCE-V1}

[[ -f "$REPORT" ]] || {
    echo "Missing hardware acceptance report: $REPORT" >&2
    exit 1
}

required_value() {
    local key=$1
    local value

    if [[ $(grep -c "^${key}=" "$REPORT") -ne 1 ]]; then
        echo "Hardware acceptance report is missing ${key}" >&2
        exit 1
    fi
    value=$(sed -n "s/^${key}=//p" "$REPORT")
    if [[ -z "$value" ]]; then
        echo "Hardware acceptance report has empty ${key}" >&2
        exit 1
    fi
    printf '%s' "$value"
}

report_version=$(required_value hfoc_hardware_acceptance_report_version)
[[ "$report_version" == "1" ]] || {
    echo "Unsupported hardware acceptance report version" >&2
    exit 1
}

release_id=$(required_value target_release_id)
[[ "$release_id" == "$RELEASE_ID" ]] || {
    echo "Hardware acceptance report release identity mismatch" >&2
    exit 1
}

source_revision=$(required_value source_revision)
[[ "$source_revision" =~ ^([0-9a-f]{7,40}|unknown)$ ]] || {
    echo "Hardware acceptance report has an invalid source revision" >&2
    exit 1
}

acceptance_device=$(required_value acceptance_device)
[[ "$acceptance_device" == /dev/sda ]] || {
    echo "Hardware acceptance report does not target the declared acceptance disk" >&2
    exit 1
}

declared_model=$(required_value declared_model)
declared_serial=$(required_value declared_serial)
declared_root=$(required_value declared_root)
observed_model=$(required_value observed_model)
observed_serial=$(required_value observed_serial)
observed_root=$(required_value observed_root)

# The identity the run actually saw must equal the declared identity, otherwise
# the guard was bypassed and the report describes a different disk.
observed_matches() {
    [[ "$observed_serial" == "$declared_serial" &&
       "$observed_root" == "$declared_root" ]] || return 1
    # shellcheck disable=SC2254
    case "$observed_model" in
        $declared_model) return 0 ;;
        *) return 1 ;;
    esac
}
if ! observed_matches; then
    echo "Hardware acceptance report observed identity does not match the declared identity" >&2
    exit 1
fi

image_sha256=$(required_value image_sha256)
[[ "$image_sha256" =~ ^[0-9a-f]{64}$ ]] || {
    echo "Hardware acceptance report has an invalid image digest" >&2
    exit 1
}

audit_line=$(required_value audit_line)
for fragment in 'initiator=' 'started_utc=' "device=${acceptance_device}" \
                'model=' 'serial=' 'declared_model=' 'declared_serial=' \
                'declared_root=' 'image_sha256='; do
    if [[ "$audit_line" != *"$fragment"* ]]; then
        echo "Hardware acceptance report audit line is missing ${fragment}" >&2
        exit 1
    fi
done
if [[ "$audit_line" != *"image_sha256=${image_sha256}"* ]]; then
    echo "Hardware acceptance report audit line does not quote the run image digest" >&2
    exit 1
fi

stage_count=$(required_value stage_count)
[[ "$stage_count" =~ ^[1-9][0-9]*$ ]] || {
    echo "Hardware acceptance report has an invalid stage count" >&2
    exit 1
}

run_started_utc=$(required_value run_started_utc)
run_finished_utc=$(required_value run_finished_utc)
for stamp in "$run_started_utc" "$run_finished_utc"; do
    [[ "$stamp" =~ ^[0-9]{8}T[0-9]{6}Z$ ]] || {
        echo "Hardware acceptance report has an invalid run timestamp" >&2
        exit 1
    }
done
if [[ "$run_finished_utc" < "$run_started_utc" ]]; then
    echo "Hardware acceptance report finished before it started" >&2
    exit 1
fi

gate_status=$(required_value gate_status)
[[ "$gate_status" == "ACCEPT" || "$gate_status" == "INCOMPLETE" ||
   "$gate_status" == "REJECT" ]] || {
    echo "Hardware acceptance report has an unsupported gate status" >&2
    exit 1
}
required_value gate_reason >/dev/null

declare -A seen_ids
passed=0
degraded=0
not_tested=0
failing=0

for ((stage_index = 1; stage_index <= stage_count; ++stage_index)); do
    prefix="stage_${stage_index}_"
    field_count=$(grep -c "^${prefix}" "$REPORT" || true)
    if [[ "$field_count" != "8" ]]; then
        echo "Hardware acceptance report stage ${stage_index} has ${field_count} fields" >&2
        exit 1
    fi
    stage_id=$(required_value "${prefix}id")
    name=$(required_value "${prefix}name")
    owner=$(required_value "${prefix}owner")
    kind=$(required_value "${prefix}kind")
    status=$(required_value "${prefix}status")
    evidence=$(required_value "${prefix}evidence")
    reason=$(sed -n "s/^${prefix}reason=//p" "$REPORT")
    hardware_evidence=$(sed -n "s/^${prefix}hardware_evidence=//p" "$REPORT")

    [[ -n "${seen_ids[$stage_id]+x}" ]] && {
        echo "Hardware acceptance report has duplicate stage ID ${stage_id}" >&2
        exit 1
    }
    seen_ids[$stage_id]=1
    [[ "$kind" == "both" || "$kind" == "hardware" ]] || {
        echo "Hardware acceptance report stage ${stage_index} has an unsupported kind" >&2
        exit 1
    }
    [[ "$evidence" =~ ^(hardware|none)$ ]] || {
        echo "Hardware acceptance report stage ${stage_index} has an unsupported evidence kind" >&2
        exit 1
    }

    case "$status" in
        PASS)
            [[ -n "$hardware_evidence" ]] || {
                echo "Hardware acceptance report stage ${stage_index} claims a pass without hardware evidence" >&2
                exit 1
            }
            passed=$((passed + 1))
            ;;
        DEGRADED)
            [[ -n "$reason" ]] || {
                echo "Hardware acceptance report stage ${stage_index} hides its degraded reason" >&2
                exit 1
            }
            [[ -n "$hardware_evidence" ]] || {
                echo "Hardware acceptance report stage ${stage_index} claims a degraded pass without hardware evidence" >&2
                exit 1
            }
            degraded=$((degraded + 1))
            ;;
        NOT_TESTED)
            [[ -n "$reason" ]] || {
                echo "Hardware acceptance report stage ${stage_index} hides its not-tested reason" >&2
                exit 1
            }
            not_tested=$((not_tested + 1))
            ;;
        FAIL|UNOBSERVED)
            failing=$((failing + 1))
            ;;
        *)
            echo "Hardware acceptance report stage ${stage_index} has an unsupported status" >&2
            exit 1
            ;;
    esac

    if [[ -n "$MATRIX" ]]; then
        [[ -f "$MATRIX" ]] || {
            echo "Missing hardware acceptance matrix: $MATRIX" >&2
            exit 1
        }
        matrix_id=$(sed -n "s/^${prefix}id=//p" "$MATRIX")
        matrix_name=$(sed -n "s/^${prefix}name=//p" "$MATRIX")
        matrix_owner=$(sed -n "s/^${prefix}owner=//p" "$MATRIX")
        matrix_kind=$(sed -n "s/^${prefix}kind=//p" "$MATRIX")
        if [[ -z "$matrix_id" || "$stage_id" != "$matrix_id" ||
              "$name" != "$matrix_name" || "$owner" != "$matrix_owner" ||
              "$kind" != "$matrix_kind" ]]; then
            echo "Hardware acceptance report stage ${stage_index} does not match the hardware acceptance matrix" >&2
            exit 1
        fi
    fi
done

undeclared=$(grep -cE '^stage_[0-9]+_' "$REPORT" || true)
declared=$((stage_count * 8))
if [[ "$undeclared" != "$declared" ]]; then
    echo "Hardware acceptance report carries undeclared stage fields" >&2
    exit 1
fi

physical_run=$(required_value physical_run)
[[ "$physical_run" == "YES" || "$physical_run" == "NO" ]] || {
    echo "Hardware acceptance report has an invalid physical_run flag" >&2
    exit 1
}

case "$gate_status" in
    ACCEPT)
        if (( failing != 0 || not_tested != 0 )); then
            echo "Hardware acceptance report accepts a gate with unobserved or failing stages" >&2
            exit 1
        fi
        [[ "$physical_run" == "YES" ]] || {
            echo "Hardware acceptance report accepts a gate that did not run on hardware" >&2
            exit 1
        }
        ;;
    INCOMPLETE)
        if (( failing != 0 )); then
            echo "Hardware acceptance report is incomplete but carries a failing stage" >&2
            exit 1
        fi
        if (( not_tested == 0 )); then
            echo "Hardware acceptance report is incomplete without an unobserved stage" >&2
            exit 1
        fi
        [[ "$physical_run" == "NO" ]] || {
            echo "Hardware acceptance report claims a completed physical run while incomplete" >&2
            exit 1
        }
        ;;
    REJECT)
        if (( failing == 0 )); then
            echo "Hardware acceptance report rejects a gate without a failing stage" >&2
            exit 1
        fi
        [[ "$physical_run" == "NO" ]] || {
            echo "Hardware acceptance report claims a completed physical run while rejecting" >&2
            exit 1
        }
        ;;
esac

printf 'Hardware acceptance report verification: OK (%s passed, %s degraded, %s not tested, %s failing)\n' \
    "$passed" "$degraded" "$not_tested" "$failing"
