#!/usr/bin/env bash
# ITEM-369/ITEM-379 production end-to-end acceptance runner.
#
# Verifies the canonical stage matrix, harvests evidence already produced in
# this run, executes every remaining stage independently through the shared
# acceptance-suite runner, and publishes a machine-readable per-stage report.
#
# ITEM-379 hardened the report into the artifact a release decision reads:
#
#   - every stage carries the verdict the matrix declares: a DEGRADED stage is
#     reported as DEGRADED, never as a pass, and a NOT_TESTED stage carries its
#     reason;
#   - stages run independently, so a failing stage cannot hide the verdict of
#     the stages after it; every stage of the path gets a verdict;
#   - the gate reason names the first failing or unobserved stage instead of
#     the last one;
#   - the report carries the run window and a per-stage reason, and the runner
#     validates its own report with the report verifier before it prints ACCEPT.
#
# A stage is never rendered as a pass unless its own target printed its own
# marker.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

MATRIX=${HFOC_PRODUCTION_ACCEPTANCE_MATRIX:-$ROOT/release/hfoc-production-end-to-end-acceptance.txt}
MAKE_CMD=${HFOC_MAKE_CMD:-make}
ARTIFACT_DIR=${HFOC_PRODUCTION_ACCEPTANCE_ARTIFACT_DIR:-}
HARVEST_DIR=${HFOC_PRODUCTION_ACCEPTANCE_HARVEST_DIR:-}
HARVEST_ONLY=${HFOC_PRODUCTION_ACCEPTANCE_HARVEST_ONLY:-0}

[[ -f "$MATRIX" ]] || {
    echo "Missing production end-to-end acceptance matrix: $MATRIX" >&2
    exit 1
}

bash "$ROOT/scripts/verify-production-end-to-end-acceptance.sh" "$MATRIX"

stage_count=$(sed -n 's/^stage_count=//p' "$MATRIX")
if [[ ! "$stage_count" =~ ^[1-9][0-9]*$ ]]; then
    echo "Production acceptance matrix has invalid stage count" >&2
    exit 1
fi

if [[ -z "$ARTIFACT_DIR" ]]; then
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    ARTIFACT_DIR="$ROOT/build/test-artifacts/production-end-to-end-acceptance-$timestamp"
fi
mkdir -p "$ARTIFACT_DIR"
report=$ARTIFACT_DIR/report.txt
suite_log=$ARTIFACT_DIR/acceptance-suite.log
run_started_utc=$(date -u +%Y%m%dT%H%M%SZ)

declare -a stage_ids=()
declare -a stage_names=()
declare -a stage_owners=()
declare -a stage_states=()
declare -a stage_kinds=()
declare -a stage_targets=()
declare -a stage_markers=()
declare -a stage_part_sets=()
declare -a stage_reasons=()

for ((stage_index = 1; stage_index <= stage_count; ++stage_index)); do
    prefix="stage_${stage_index}_"
    stage_ids+=("$(sed -n "s/^${prefix}id=//p" "$MATRIX")")
    stage_names+=("$(sed -n "s/^${prefix}name=//p" "$MATRIX")")
    stage_owners+=("$(sed -n "s/^${prefix}owner=//p" "$MATRIX")")
    stage_states+=("$(sed -n "s/^${prefix}state=//p" "$MATRIX")")
    stage_kinds+=("$(sed -n "s/^${prefix}kind=//p" "$MATRIX")")
    stage_targets+=("$(sed -n "s/^${prefix}target=//p" "$MATRIX")")
    stage_markers+=("$(sed -n "s/^${prefix}marker=//p" "$MATRIX")")
    stage_part_sets+=("$(sed -n "s/^${prefix}targets=//p" "$MATRIX")")
    stage_reasons+=("$(sed -n "s/^${prefix}reason=//p" "$MATRIX")")
done

declare -A harvested=()
if [[ -n "$HARVEST_DIR" && -d "$HARVEST_DIR" ]]; then
    while IFS= read -r line; do
        target=${line%% *}
        status=${line##* }
        if [[ "$target" =~ ^test-[a-z0-9-]+$ && "$status" == "PASS" ]]; then
            harvested[$target]=1
        fi
    done < <(find "$HARVEST_DIR" -name suite-status.txt -type f \
        -exec cat {} + 2>/dev/null || true)
    while IFS= read -r line; do
        if [[ "$line" == PASS\ * ]]; then
            target=${line#PASS }
            if [[ "$target" =~ ^test-[a-z0-9-]+$ ]]; then
                harvested[$target]=1
            fi
        fi
    done < <(find "$HARVEST_DIR" -name manifest.txt -type f \
        -exec cat {} + 2>/dev/null || true)
fi

if [[ "$HARVEST_ONLY" != "1" ]]; then
    for ((stage_index = 0; stage_index < stage_count; ++stage_index)); do
        [[ "${stage_states[$stage_index]}" == "NOT_TESTED" ]] && continue
        target=${stage_targets[$stage_index]}
        parts=${stage_part_sets[$stage_index]}
        if [[ -n $parts ]]; then
            part_harvested=0
            part_missing=0
            for part in $parts; do
                if [[ -n "${harvested[$part]:-}" ]]; then
                    part_harvested=$((part_harvested + 1))
                else
                    part_missing=$((part_missing + 1))
                fi
            done
            # Split scheduling (CI): the parts ran in the suite and the current
            # harvest must carry every one of them; a partial harvest is a
            # failure, not a reason to run the aggregate again outside it.
            if (( part_harvested > 0 )); then
                continue
            fi
        fi
        [[ -n "${harvested[$target]:-}" ]] && continue
        if ! HFOC_MAKE_CMD="$MAKE_CMD" \
                bash "$ROOT/scripts/run-acceptance-suite.sh" \
                --artifact-dir "$ARTIFACT_DIR" \
                --serial "$target" "${stage_markers[$stage_index]}" \
                >>"$suite_log" 2>&1; then
            echo "production acceptance: stage $((stage_index + 1)) ${stage_ids[$stage_index]} target failed" >&2
        fi
    done
fi

declare -a stage_status=()
declare -a stage_evidence=()
declare -a stage_details=()
declare -a stage_report_reasons=()
gate_status=ACCEPT
gate_reason="every executed stage was observed"

for ((stage_index = 0; stage_index < stage_count; ++stage_index)); do
    state=${stage_states[$stage_index]}
    target=${stage_targets[$stage_index]}
    marker=${stage_markers[$stage_index]}
    reason=${stage_reasons[$stage_index]}

    if [[ "$state" == "NOT_TESTED" ]]; then
        stage_status+=(NOT_TESTED)
        stage_evidence+=(none)
        stage_details+=("owner=${stage_owners[$stage_index]} ${reason}")
        stage_report_reasons+=("$reason")
        continue
    fi
    if [[ "$state" == "DEGRADED" ]]; then
        observed_status=DEGRADED
    else
        observed_status=PASS
    fi
    parts=${stage_part_sets[$stage_index]}
    if [[ -n $parts ]]; then
        part_count=0
        part_harvested=0
        for part in $parts; do
            part_count=$((part_count + 1))
            if [[ -n "${harvested[$part]:-}" ]]; then
                part_harvested=$((part_harvested + 1))
            fi
        done
        if (( part_harvested == part_count )); then
            stage_status+=("$observed_status")
            stage_evidence+=(harvest)
            stage_details+=("harvested evidence for all ${part_count} parts: ${parts}")
            stage_report_reasons+=("$reason")
            continue
        fi
        if (( part_harvested > 0 )); then
            stage_status+=(FAIL)
            stage_evidence+=(harvest)
            stage_details+=("harvest carries ${part_harvested} of ${part_count} parts")
            stage_report_reasons+=("the split HTTPS parts must all pass: ${parts}")
            if [[ "$gate_status" == "ACCEPT" ]]; then
                gate_status=REJECT
                gate_reason="stage $((stage_index + 1)) ${stage_ids[$stage_index]} is missing part evidence"
            fi
            continue
        fi
    fi
    if [[ -n "${harvested[$target]:-}" ]]; then
        stage_status+=("$observed_status")
        stage_evidence+=(harvest)
        stage_details+=("harvested evidence for ${target}")
        stage_report_reasons+=("$reason")
        continue
    fi
    target_log=$ARTIFACT_DIR/$target.log
    if [[ -f "$target_log" ]] && grep -Fq -- "$marker" "$target_log"; then
        stage_status+=("$observed_status")
        stage_evidence+=("${stage_kinds[$stage_index]}")
        stage_details+=("observed ${marker}")
        stage_report_reasons+=("$reason")
    elif [[ -f "$target_log" ]]; then
        stage_status+=(FAIL)
        stage_evidence+=("${stage_kinds[$stage_index]}")
        stage_details+=("missing marker for ${target}")
        stage_report_reasons+=("${target} did not print its declared marker")
        if [[ "$gate_status" == "ACCEPT" ]]; then
            gate_status=REJECT
            gate_reason="stage $((stage_index + 1)) ${stage_ids[$stage_index]} failed"
        fi
    else
        stage_status+=(UNOBSERVED)
        stage_evidence+=(none)
        stage_details+=("no evidence for ${target}")
        stage_report_reasons+=("the ${target} proof produced no evidence")
        if [[ "$gate_status" == "ACCEPT" ]]; then
            gate_status=REJECT
            gate_reason="stage $((stage_index + 1)) ${stage_ids[$stage_index]} was not observed"
        fi
    fi
done

source_revision=$(git rev-parse HEAD 2>/dev/null || echo unknown)
run_finished_utc=$(date -u +%Y%m%dT%H%M%SZ)
{
    printf 'hfoc_production_end_to_end_acceptance_report_version=2\n'
    printf 'target_release_id=HFOC-APPLIANCE-V1\n'
    printf 'source_revision=%s\n' "$source_revision"
    printf 'stage_count=%s\n' "$stage_count"
    printf 'artifact_dir=%s\n' "$ARTIFACT_DIR"
    printf 'run_started_utc=%s\n' "$run_started_utc"
    printf 'run_finished_utc=%s\n' "$run_finished_utc"
    for ((stage_index = 0; stage_index < stage_count; ++stage_index)); do
        number=$((stage_index + 1))
        printf 'stage_%s_id=%s\n' "$number" "${stage_ids[$stage_index]}"
        printf 'stage_%s_name=%s\n' "$number" "${stage_names[$stage_index]}"
        printf 'stage_%s_owner=%s\n' "$number" "${stage_owners[$stage_index]}"
        printf 'stage_%s_expected=%s\n' "$number" "${stage_states[$stage_index]}"
        printf 'stage_%s_status=%s\n' "$number" "${stage_status[$stage_index]}"
        printf 'stage_%s_evidence=%s\n' "$number" "${stage_evidence[$stage_index]}"
        printf 'stage_%s_target=%s\n' "$number" "${stage_targets[$stage_index]}"
        printf 'stage_%s_marker=%s\n' "$number" "${stage_markers[$stage_index]}"
        printf 'stage_%s_detail=%s\n' "$number" "${stage_details[$stage_index]}"
        printf 'stage_%s_reason=%s\n' "$number" "${stage_report_reasons[$stage_index]}"
    done
    printf 'gate_status=%s\n' "$gate_status"
    printf 'gate_reason=%s\n' "$gate_reason"
} >"$report"

if ! bash "$ROOT/scripts/verify-production-end-to-end-acceptance-report.sh" \
        "$report" "$MATRIX" >/dev/null; then
    echo "production acceptance: the published report is malformed" >&2
    exit 1
fi

echo "Production end-to-end acceptance report: $report"
cat "$report"

if [[ "$gate_status" != "ACCEPT" ]]; then
    echo "PRODUCTION END-TO-END ACCEPTANCE: REJECT" >&2
    exit 1
fi
echo "PRODUCTION END-TO-END ACCEPTANCE: ACCEPT"
