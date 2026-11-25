#!/usr/bin/env bash
# Run explicitly supplied host Make targets in ordered serial/parallel phases.
set -Eeuo pipefail

make_cmd=${HFOC_MAKE_CMD:-make}
jobs=${HFOC_HOST_JOBS:-2}

fail_usage() {
    echo "run-host-suite: $*" >&2
    exit 2
}

[[ $jobs =~ ^[1-9][0-9]*$ ]] ||
    fail_usage "HFOC_HOST_JOBS must be a positive decimal integer"

declare -a phase_kinds=()
declare -a phase_targets=()

while (( $# )); do
    case $1 in
        --serial)
            shift
            if (( $# == 0 )) || [[ $1 == --* ]]; then
                fail_usage "--serial requires one target"
            fi
            phase_kinds+=(serial)
            phase_targets+=("$1")
            shift
            ;;
        --parallel)
            shift
            declare -a targets=()
            while (( $# )) && [[ $1 != --serial && $1 != --parallel ]]; do
                [[ $1 != --* ]] || fail_usage "unknown option: $1"
                targets+=("$1")
                shift
            done
            (( ${#targets[@]} )) ||
                fail_usage "--parallel requires at least one target"
            phase_kinds+=(parallel)
            phase_targets+=("${targets[*]}")
            ;;
        *)
            fail_usage "unknown option: $1"
            ;;
    esac
done

(( ${#phase_kinds[@]} )) || fail_usage "at least one phase is required"

log_dir=$(mktemp -d)
artifact_dir=${HFOC_HOST_SUITE_ARTIFACT_DIR:-}

# ITEM-585: per-target timing.  Durations are monotonic (/proc/uptime), the
# records live in a directory unique to this run attempt, and every append is
# one flock-protected line, so parallel targets cannot interleave records or
# read another run's file.
timing_root=${HFOC_CI_TIMINGS_DIR:-build/test-artifacts/ci-timings}
timing_job=${GITHUB_JOB:-local}
timing_run=${GITHUB_RUN_ID:-manual}
timing_attempt=${GITHUB_RUN_ATTEMPT:-1}
timing_sha=${GITHUB_SHA:-$(git rev-parse HEAD 2>/dev/null || echo unknown)}
timing_phase=${HFOC_TIMINGS_PHASE:-static-host}
timing_dir=$timing_root/"$timing_job-$timing_run-$timing_attempt"
timing_file=$timing_dir/host-suite.tsv
suite_started_ms=$(awk '{printf "%d", $1 * 1000}' /proc/uptime)
wrapper_recorded=0

now_ms() {
    awk '{printf "%d", $1 * 1000}' /proc/uptime
}

record_timing() {
    # kind name status rc ms retry started_ms
    local line
    line=$(printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$timing_job" "$timing_attempt" "$timing_sha" "$timing_phase" \
        "$1" "$2" "$3" "$4" "$5" "$6" "$7")
    mkdir -p "$timing_dir" 2>/dev/null || return 0
    if ! command -v flock >/dev/null 2>&1; then
        printf '%s\n' "$line" >>"$timing_file" 2>/dev/null || true
        return 0
    fi
    (
        flock 9
        printf '%s\n' "$line" >>"$timing_file"
    ) 9>>"$timing_file.lock" 2>/dev/null || true
}

record_wrapper() {
    local status=$1
    (( wrapper_recorded == 0 )) || return 0
    wrapper_recorded=1
    record_timing phase-wrapper host-suite \
        "$([[ $status == 0 ]] && echo PASS || echo FAIL)" "$status" \
        "$(($(now_ms) - suite_started_ms))" "" "$suite_started_ms"
}

declare -A pid_target=()
declare -A pid_log=()
declare -A pid_start=()

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
    record_wrapper "$status"
    rm -rf "$log_dir"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

print_failure() {
    local target=$1
    local log_path=$2

    printf 'HOST suite: FAIL %s\n' "$target" >&2
    printf '%s\n' "----- $target output -----" >&2
    cat "$log_path" >&2
    printf '%s\n' "----- end $target output -----" >&2
}

save_target_artifact() {
    local target=$1
    local log_path=$2
    local status=$3

    if [[ -z "$artifact_dir" ]]; then
        return 0
    fi
    mkdir -p "$artifact_dir"
    cp "$log_path" "$artifact_dir/$target.log" 2>/dev/null || true
    printf '%s %s\n' "$target" "$status" >>"$artifact_dir/suite-status.txt"
}

run_serial() {
    local target=$1
    local log_path="$log_dir/$target.log"
    local status
    local started_ms

    started_ms=$(now_ms)
    if "$make_cmd" "$target" >"$log_path" 2>&1; then
        save_target_artifact "$target" "$log_path" PASS
        record_timing host-target "$target" PASS 0 \
            "$(($(now_ms) - started_ms))" "" "$started_ms"
        printf 'HOST suite: PASS %s\n' "$target"
        return 0
    else
        status=$?
    fi

    save_target_artifact "$target" "$log_path" FAIL
    record_timing host-target "$target" FAIL "$status" \
        "$(($(now_ms) - started_ms))" "" "$started_ms"
    print_failure "$target" "$log_path"
    return "$status"
}

start_target() {
    local target=$1
    local log_path="$log_dir/$target.log"
    local pid

    pid_start_target=$(now_ms)
    "$make_cmd" "$target" >"$log_path" 2>&1 &
    pid=$!
    pid_target[$pid]=$target
    pid_log[$pid]=$log_path
    pid_start[$pid]=$pid_start_target
}

reap_one() {
    local finished_pid
    local status
    local target
    local log_path

    if wait -n -p finished_pid; then
        status=0
    else
        status=$?
    fi

    target=${pid_target[$finished_pid]:-}
    log_path=${pid_log[$finished_pid]:-}
    [[ -n $target && -n $log_path ]] ||
        fail_usage "received an unknown child completion"

    reaped_started=${pid_start[$finished_pid]:-$(now_ms)}
    unset 'pid_target[$finished_pid]'
    unset 'pid_log[$finished_pid]'
    unset 'pid_start[$finished_pid]'
    unset 'pid_kind[$finished_pid]'
    reaped_started=${reaped_started:-$(now_ms)}
    reaped_target=$target
    reaped_log=$log_path
    reaped_status=$status
}

run_parallel() {
    local -a targets=("$@")
    local next=0
    local running=0
    local failed_target=
    local failed_log=
    local failed_status=0

    while (( next < ${#targets[@]} || running > 0 )); do
        while (( next < ${#targets[@]} && running < jobs )); do
            start_target "${targets[$next]}"
            next=$((next + 1))
            running=$((running + 1))
        done

        (( running > 0 )) || continue
        reap_one
        running=$((running - 1))

        if (( reaped_status == 0 )); then
            save_target_artifact "$reaped_target" "$reaped_log" PASS
            record_timing host-target "$reaped_target" PASS 0 \
                "$(($(now_ms) - reaped_started))" "" "$reaped_started"
            printf 'HOST suite: PASS %s\n' "$reaped_target"
            continue
        fi

        save_target_artifact "$reaped_target" "$reaped_log" FAIL
        record_timing host-target "$reaped_target" FAIL "$reaped_status" \
            "$(($(now_ms) - reaped_started))" "" "$reaped_started"
        failed_target=$reaped_target
        failed_log=$reaped_log
        failed_status=$reaped_status
        while (( running > 0 )); do
            reap_one
            running=$((running - 1))
            save_target_artifact "$reaped_target" "$reaped_log" FAIL
        done
        print_failure "$failed_target" "$failed_log"
        return "$failed_status"
    done
}

for phase_index in "${!phase_kinds[@]}"; do
    case ${phase_kinds[$phase_index]} in
        serial)
            run_serial "${phase_targets[$phase_index]}" || exit $?
            ;;
        parallel)
            read -r -a targets <<<"${phase_targets[$phase_index]}"
            run_parallel "${targets[@]}" || exit $?
            ;;
    esac
done
