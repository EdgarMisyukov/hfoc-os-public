#!/usr/bin/env bash
# ITEM-379 strict verifier for the production end-to-end acceptance report.
#
# The report is the single artifact a release decision reads, so it is checked
# against its own contract before it can be believed:
#
#   - schema version, release identity, source revision, stage count and the
#     UTC run window are present exactly once;
#   - every declared stage publishes a complete, ordered block whose status is
#     the verdict the canonical matrix declares: a PROVEN stage may be PASS, a
#     DEGRADED stage may be DEGRADED, a NOT_TESTED stage stays NOT_TESTED, and
#     a failing or unobserved stage is reported as FAIL or UNOBSERVED;
#   - DEGRADED and NOT_TESTED stages carry a non-empty reason, so the report
#     never hides why a stage is not a full pass;
#   - the gate is consistent: ACCEPT forbids FAIL and UNOBSERVED, REJECT
#     requires at least one of them, and the gate reason is never empty;
#   - with an optional matrix path, every stage identity, owner, expected
#     state, target and marker must match the canonical matrix it claims.
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
    echo "Missing production end-to-end acceptance report: $REPORT" >&2
    exit 1
}

required_value() {
    local key=$1
    local value

    if [[ $(grep -c "^${key}=" "$REPORT") -ne 1 ]]; then
        echo "Production acceptance report is missing ${key}" >&2
        exit 1
    fi
    value=$(sed -n "s/^${key}=//p" "$REPORT")
    if [[ -z "$value" ]]; then
        echo "Production acceptance report has empty ${key}" >&2
        exit 1
    fi
    printf '%s' "$value"
}

report_version=$(required_value \
    hfoc_production_end_to_end_acceptance_report_version)
[[ "$report_version" == "2" ]] || {
    echo "Unsupported production acceptance report version" >&2
    exit 1
}

release_id=$(required_value target_release_id)
[[ "$release_id" == "$RELEASE_ID" ]] || {
    echo "Production acceptance report release identity mismatch" >&2
    exit 1
}

source_revision=$(required_value source_revision)
[[ "$source_revision" =~ ^([0-9a-f]{7,40}|unknown)$ ]] || {
    echo "Production acceptance report has an invalid source revision" >&2
    exit 1
}

stage_count=$(required_value stage_count)
[[ "$stage_count" =~ ^[1-9][0-9]*$ ]] || {
    echo "Production acceptance report has an invalid stage count" >&2
    exit 1
}

required_value artifact_dir >/dev/null
run_started_utc=$(required_value run_started_utc)
run_finished_utc=$(required_value run_finished_utc)
for stamp in "$run_started_utc" "$run_finished_utc"; do
    [[ "$stamp" =~ ^[0-9]{8}T[0-9]{6}Z$ ]] || {
        echo "Production acceptance report has an invalid run timestamp" >&2
        exit 1
    }
done
if [[ "$run_finished_utc" < "$run_started_utc" ]]; then
    echo "Production acceptance report finished before it started" >&2
    exit 1
fi

gate_status=$(required_value gate_status)
[[ "$gate_status" == "ACCEPT" || "$gate_status" == "REJECT" ]] || {
    echo "Production acceptance report has an unsupported gate status" >&2
    exit 1
}
required_value gate_reason >/dev/null

declare -A seen_ids
declare -A seen_names
failures=0
not_tested=0
degraded=0
proven=0

for ((stage_index = 1; stage_index <= stage_count; ++stage_index)); do
    prefix="stage_${stage_index}_"
    field_count=$(grep -c "^${prefix}" "$REPORT" || true)
    if [[ "$field_count" != "10" ]]; then
        echo "Production acceptance report stage ${stage_index} has ${field_count} fields" >&2
        exit 1
    fi
    stage_id=$(required_value "${prefix}id")
    name=$(required_value "${prefix}name")
    owner=$(required_value "${prefix}owner")
    expected=$(required_value "${prefix}expected")
    status=$(required_value "${prefix}status")
    evidence=$(required_value "${prefix}evidence")
    target=$(sed -n "s/^${prefix}target=//p" "$REPORT")
    marker=$(sed -n "s/^${prefix}marker=//p" "$REPORT")
    reason=$(sed -n "s/^${prefix}reason=//p" "$REPORT")
    required_value "${prefix}detail" >/dev/null

    [[ -n "${seen_ids[$stage_id]+x}" ]] && {
        echo "Production acceptance report has duplicate stage ID ${stage_id}" >&2
        exit 1
    }
    [[ -n "${seen_names[$name]+x}" ]] && {
        echo "Production acceptance report has duplicate stage name ${name}" >&2
        exit 1
    }
    seen_ids[$stage_id]=1
    seen_names[$name]=1

    case "$expected" in
        PROVEN)
            [[ "$status" == "PASS" || "$status" == "FAIL" ||
               "$status" == "UNOBSERVED" ]] || {
                echo "Production acceptance report stage ${stage_index} renders a PROVEN stage as ${status}" >&2
                exit 1
            }
            [[ "$status" == "PASS" ]] && proven=$((proven + 1))
            ;;
        DEGRADED)
            [[ "$status" == "DEGRADED" || "$status" == "FAIL" ||
               "$status" == "UNOBSERVED" ]] || {
                echo "Production acceptance report stage ${stage_index} renders a DEGRADED stage as ${status}" >&2
                exit 1
            }
            [[ -n "$reason" ]] || {
                echo "Production acceptance report stage ${stage_index} hides its degraded reason" >&2
                exit 1
            }
            [[ "$status" == "DEGRADED" ]] && degraded=$((degraded + 1))
            ;;
        NOT_TESTED)
            [[ "$status" == "NOT_TESTED" ]] || {
                echo "Production acceptance report stage ${stage_index} renders a NOT_TESTED stage as ${status}" >&2
                exit 1
            }
            [[ -n "$reason" ]] || {
                echo "Production acceptance report stage ${stage_index} hides its not-tested reason" >&2
                exit 1
            }
            not_tested=$((not_tested + 1))
            ;;
        *)
            echo "Production acceptance report stage ${stage_index} has an unsupported expected state" >&2
            exit 1
            ;;
    esac

    if [[ "$status" == "FAIL" || "$status" == "UNOBSERVED" ]]; then
        failures=$((failures + 1))
    fi
    if [[ "$status" != "NOT_TESTED" ]]; then
        [[ "$target" =~ ^test-[a-z0-9-]+$ ]] || {
            echo "Production acceptance report stage ${stage_index} has an invalid target" >&2
            exit 1
        }
        [[ -n "$marker" ]] || {
            echo "Production acceptance report stage ${stage_index} has no marker" >&2
            exit 1
        }
    fi
    [[ "$evidence" =~ ^(run|gate|harvest|none)$ ]] || {
        echo "Production acceptance report stage ${stage_index} has an unsupported evidence kind" >&2
        exit 1
    }

    if [[ -n "$MATRIX" ]]; then
        [[ -f "$MATRIX" ]] || {
            echo "Missing production acceptance matrix: $MATRIX" >&2
            exit 1
        }
        matrix_state=$(sed -n "s/^${prefix}state=//p" "$MATRIX")
        matrix_owner=$(sed -n "s/^${prefix}owner=//p" "$MATRIX")
        matrix_target=$(sed -n "s/^${prefix}target=//p" "$MATRIX")
        matrix_marker=$(sed -n "s/^${prefix}marker=//p" "$MATRIX")
        if [[ "$stage_id" != "$(sed -n "s/^${prefix}id=//p" "$MATRIX")" ||
              "$owner" != "$matrix_owner" ||
              "$expected" != "$matrix_state" ||
              "$target" != "$matrix_target" ||
              "$marker" != "$matrix_marker" ]]; then
            echo "Production acceptance report stage ${stage_index} does not match the canonical matrix" >&2
            exit 1
        fi
    fi
done

undeclared=$(grep -cE '^stage_[0-9]+_' "$REPORT" || true)
declared=$((stage_count * 10))
if [[ "$undeclared" != "$declared" ]]; then
    echo "Production acceptance report carries undeclared stage fields" >&2
    exit 1
fi

if [[ "$gate_status" == "ACCEPT" ]]; then
    if (( failures != 0 )); then
        echo "Production acceptance report accepts a gate over ${failures} failing stages" >&2
        exit 1
    fi
else
    if (( failures == 0 )); then
        echo "Production acceptance report rejects a gate without a failing stage" >&2
        exit 1
    fi
fi

printf 'Production end-to-end acceptance report verification: OK (%s proven, %s degraded, %s not tested, %s failing)\n' \
    "$proven" "$degraded" "$not_tested" "$failures"
