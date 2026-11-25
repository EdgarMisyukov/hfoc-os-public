#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

make_cmd=${HFOC_MAKE_CMD:-make}
jobs=${HFOC_ACCEPTANCE_JOBS:-2}
artifact_dir=

# ITEM-421: the acceptance gate runs its own target list, so it needs the same
# rule the QEMU suite applies (ITEM-412): a proof that stopped because it did
# not see an expected marker inside its own deadline declares that class with
# one strict record, and the gate retries that target once, visibly, taking the
# verdict from the retry.  Everything else - a failed expectation, an in-guest
# `[TAG] failed:` record, a panic, a fail-closed guard - fails the gate at once
# and is never retried.  On 2026-09-15 the gate failed twice on
# `test-https-restart-soak-qemu`, whose log carried the class record, because
# this runner had no rule to read it.
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
# shellcheck source=/dev/null  # linted on its own, like the check.d modules
. "$script_dir/hfoc-qemu-failure-class.sh"
# ITEM-442: the acceptance gate starts the same proofs, so it makes the same
# accelerator decision and prints the same record.
# shellcheck source=/dev/null  # linted on its own, like the check.d modules
. "$script_dir/hfoc-qemu-accel.sh"
hfoc_qemu_accel_enable
hfoc_qemu_accel_record

timing_retry=${HFOC_QEMU_TIMING_RETRY:-1}
# ITEM-421: the fields the classifier assigns.  The CI static phase lints one
# file at a time and does not follow sourced files, so they are declared here as
# well; every path of the classifier assigns all four, which its drills in
# tests/test-qemu-suite-host.sh exercise.
hfoc_qemu_failure_class=
hfoc_qemu_failure_reason=
hfoc_qemu_failure_marker=
hfoc_qemu_failure_deadline=

fail_usage() {
    echo "run-acceptance-suite: $*" >&2
    exit 2
}

[[ $jobs =~ ^[1-9][0-9]*$ ]] ||
    fail_usage 'HFOC_ACCEPTANCE_JOBS must be a positive decimal integer'

# ITEM-421: target names are not numbers, so the set of already-retried targets
# is associative: an indexed array would evaluate `timing-once` as arithmetic
# and trip `set -u` on the first word of a target name.
declare -A retried_target=()
declare -a phase_kinds=()
declare -a phase_starts=()
declare -a phase_counts=()
declare -a targets=()
declare -a markers=()
declare -A pid_target=()
declare -A pid_marker=()
declare -A pid_log=()

while (( $# )); do
    case $1 in
        --artifact-dir)
            (( $# >= 2 )) || fail_usage '--artifact-dir requires a directory'
            [[ -z $artifact_dir ]] || fail_usage '--artifact-dir may be supplied once'
            artifact_dir=$2
            shift 2
            ;;
        --serial)
            (( $# >= 3 )) || fail_usage '--serial requires a target and marker'
            [[ $2 != --* && $3 != --* ]] ||
                fail_usage '--serial requires a target and marker'
            phase_kinds+=(serial)
            phase_starts+=(${#targets[@]})
            phase_counts+=(1)
            targets+=("$2")
            markers+=("$3")
            shift 3
            ;;
        --parallel)
            shift
            start=${#targets[@]}
            count=0
            while (( $# )) && [[ $1 != --* ]]; do
                (( $# >= 2 )) ||
                    fail_usage '--parallel requires target and marker pairs'
                [[ $2 != --* ]] ||
                    fail_usage '--parallel requires target and marker pairs'
                targets+=("$1")
                markers+=("$2")
                count=$((count + 1))
                shift 2
            done
            (( count > 0 )) || fail_usage '--parallel requires at least one pair'
            phase_kinds+=(parallel)
            phase_starts+=("$start")
            phase_counts+=("$count")
            ;;
        *)
            fail_usage "unknown option: $1"
            ;;
    esac
done

(( ${#phase_kinds[@]} > 0 )) || fail_usage 'at least one phase is required'

for target_index in "${!targets[@]}"; do
    [[ -n ${targets[$target_index]} ]] ||
        fail_usage 'empty target'
    [[ -n ${markers[$target_index]} ]] ||
        fail_usage "empty marker for ${targets[$target_index]}"
    for prior_index in $(seq 0 "$((target_index - 1))"); do
        [[ ${targets[$target_index]} != "${targets[$prior_index]}" ]] ||
            fail_usage "duplicate target: ${targets[$target_index]}"
    done
done

if [[ -z $artifact_dir ]]; then
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/acceptance-suite-$timestamp"
fi
mkdir -p "$artifact_dir"
manifest="$artifact_dir/manifest.txt"
: >"$manifest"

cleanup() {
    local status=$?
    local pid

    trap - EXIT INT TERM
    set +e
    for pid in "${!pid_target[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    for pid in "${!pid_target[@]}"; do
        wait "$pid" 2>/dev/null || true
    done
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

print_failure() {
    local target=$1
    local log_path=$2
    local reason=$3

    printf 'ACCEPTANCE suite: FAIL %s (%s)\n' "$target" "$reason" >&2
    printf '%s\n' "----- $target output -----" >&2
    cat "$log_path" >&2
    printf '%s\n' "----- end $target output -----" >&2
}

record_result() {
    local target=$1
    local log_path=$2
    local marker=$3
    local status=$4

    reaped_target=$target
    reaped_log=$log_path
    reaped_status=$status
    reaped_marker=$marker
    reaped_reason=

    if (( status == 0 )) && grep -Fq "$marker" "$log_path"; then
        printf 'PASS %s\n' "$target" >>"$manifest"
        printf 'ACCEPTANCE suite: PASS %s\n' "$target"
        return 0
    fi

    if (( status == 0 )); then
        reaped_status=1
        reaped_reason=missing-marker
    else
        reaped_status=$status
        reaped_reason="exit-$status"
    fi
    printf 'FAIL %s\n' "$target" >>"$manifest"
    if (( status == 0 )); then
        printf 'FAILURE missing-marker\n' >>"$manifest"
    else
        printf 'FAILURE %s\n' "$status" >>"$manifest"
    fi
    return 1
}

# retry_marker_timeout TARGET LOG_PATH: one visible second attempt.
#
# Returns 0 when the target stopped on a marker timeout, had not been retried
# yet, and passed on the retry; 1 in every other case, with the reason printed
# so a reader learns why the gate is not going to try again.
retry_marker_timeout() {
    local target=$1
    local log_path=$2
    local marker=$3
    local retry_log="$artifact_dir/$target.retry.log"
    local status
    local line

    hfoc_qemu_failure_classify "$log_path"
    if [[ $hfoc_qemu_failure_class != marker-timeout ]]; then
        return 1
    fi
    if [[ -n ${retried_target[$target]:-} ]]; then
        printf 'ACCEPTANCE suite: NO-RETRY %s class=%s reason=retry-exhausted class_reason=%s\n' \
            "$target" "$hfoc_qemu_failure_class" "$hfoc_qemu_failure_reason" >&2
        return 1
    fi
    if (( timing_retry == 0 )); then
        printf 'ACCEPTANCE suite: NO-RETRY %s class=%s reason=retry-disabled class_reason=%s\n' \
            "$target" "$hfoc_qemu_failure_class" "$hfoc_qemu_failure_reason" >&2
        return 1
    fi
    retried_target[target]=1
    line=$(printf 'ACCEPTANCE suite: RETRY %s reason=marker-timeout attempt=2 marker="%s" deadline=%s' \
        "$target" "$hfoc_qemu_failure_marker" "$hfoc_qemu_failure_deadline")
    printf '%s\n' "$line" >&2
    printf '%s\n' "$line" >>"$artifact_dir/retries.txt"
    if "$make_cmd" "$target" >"$retry_log" 2>&1; then
        status=0
    else
        status=$?
    fi
    record_result "$target" "$retry_log" "${marker:-}" "$status" || true
    if (( status == 0 )); then
        # The manifest keeps the whole story - the first attempt's FAIL line and
        # the retry that decided the verdict - and this line names the verdict.
        printf 'RETRY-PASS %s reason=marker-timeout attempt=2\n' "$target" \
            >>"$manifest"
    fi
    (( status == 0 ))
}

run_serial() {
    local target=$1
    local marker=$2
    local log_path="$artifact_dir/$target.log"
    local status

    if "$make_cmd" "$target" >"$log_path" 2>&1; then
        status=0
    else
        status=$?
    fi

    if record_result "$target" "$log_path" "$marker" "$status"; then
        return 0
    fi
    if retry_marker_timeout "$reaped_target" "$reaped_log" "$marker"; then
        printf 'ACCEPTANCE suite: RETRY-PASS %s\n' "$reaped_target"
        return 0
    fi
    print_failure "$reaped_target" "$reaped_log" "$reaped_reason"
    return "$reaped_status"
}

start_target() {
    local target=$1
    local marker=$2
    local log_path="$artifact_dir/$target.log"
    local pid

    "$make_cmd" "$target" >"$log_path" 2>&1 &
    pid=$!
    pid_target[$pid]=$target
    pid_marker[$pid]=$marker
    pid_log[$pid]=$log_path
}

reap_one() {
    local finished_pid
    local status
    local target
    local marker
    local log_path

    if wait -n -p finished_pid; then
        status=0
    else
        status=$?
    fi

    target=${pid_target[$finished_pid]:-}
    marker=${pid_marker[$finished_pid]:-}
    log_path=${pid_log[$finished_pid]:-}
    [[ -n $target && -n $marker && -n $log_path ]] ||
        fail_usage 'received an unknown child completion'

    unset 'pid_target[$finished_pid]'
    unset 'pid_marker[$finished_pid]'
    unset 'pid_log[$finished_pid]'
    record_result "$target" "$log_path" "$marker" "$status" || true
}

run_parallel() {
    local start=$1
    local count=$2
    local next=$start
    local end=$((start + count))
    local running=0
    local failed_status=0
    local -a failed_targets=()
    local -a failed_logs=()
    local -a failed_reasons=()

    while (( next < end || running > 0 )); do
        while (( next < end && running < jobs )); do
            start_target "${targets[$next]}" "${markers[$next]}"
            next=$((next + 1))
            running=$((running + 1))
        done

        reap_one
        running=$((running - 1))
        if (( reaped_status != 0 )); then
            if retry_marker_timeout "$reaped_target" "$reaped_log" \
                    "$reaped_marker"; then
                printf 'ACCEPTANCE suite: RETRY-PASS %s\n' "$reaped_target"
                continue
            fi
            failed_status=$reaped_status
            failed_targets+=("$reaped_target")
            failed_logs+=("$reaped_log")
            failed_reasons+=("$reaped_reason")
            break
        fi
    done

    if (( failed_status == 0 )); then
        return 0
    fi

    while (( running > 0 )); do
        reap_one
        running=$((running - 1))
        if (( reaped_status != 0 )); then
            failed_targets+=("$reaped_target")
            failed_logs+=("$reaped_log")
            failed_reasons+=("$reaped_reason")
        fi
    done

    for failed_index in "${!failed_targets[@]}"; do
        print_failure "${failed_targets[$failed_index]}" \
            "${failed_logs[$failed_index]}" "${failed_reasons[$failed_index]}"
    done
    return "$failed_status"
}

for phase_index in "${!phase_kinds[@]}"; do
    case ${phase_kinds[$phase_index]} in
        serial)
            target_index=${phase_starts[$phase_index]}
            run_serial "${targets[$target_index]}" "${markers[$target_index]}" || exit $?
            ;;
        parallel)
            run_parallel "${phase_starts[$phase_index]}" \
                "${phase_counts[$phase_index]}" || exit $?
            ;;
    esac
done

printf 'ACCEPTANCE SUITE OK\n' >>"$manifest"
echo "Acceptance suite artifacts: $artifact_dir"
