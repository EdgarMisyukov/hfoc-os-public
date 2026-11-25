#!/usr/bin/env bash
# Run explicitly supplied QEMU Make targets in ordered serial/parallel phases.
#
# ITEM-412: a target that stopped because it did not observe an expected marker
# inside its own deadline (the timing class) is retried once, visibly, and the
# phase verdict is taken from the retry. Every other failure - a failed
# assertion, an in-guest `[TAG] failed:` record, a kernel panic, a fail-closed
# guard - fails the phase immediately and is never retried. The class, the
# awaited marker and the deadline are printed for a reader, and the retry is
# recorded in the job summary and in the suite artifact directory. The rule and
# the measurement behind it live in docs/ci-self-hosted-runner.md; the
# classifier lives in scripts/hfoc-qemu-failure-class.sh.
set -Eeuo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
# ITEM-412: the classifier assigns the four fields this suite reports.  The CI
# static phase lints one file at a time and does not follow sourced files, so
# the fields are declared here as well - every path of the classifier assigns
# all four, which its drills in tests/test-qemu-suite-host.sh exercise - and the
# file is named for a human reader instead.
hfoc_qemu_failure_class=
hfoc_qemu_failure_reason=
hfoc_qemu_failure_marker=
hfoc_qemu_failure_deadline=
# shellcheck source=/dev/null  # linted on its own, like the check.d modules
. "$script_dir/hfoc-qemu-failure-class.sh"
# ITEM-442: the guests of this suite run on the host's own virtualization when
# this user may open it, and on the emulator otherwise.  The decision, the
# arguments and the shim are one file; the record line below is what a reader
# uses to prove which accelerator the suite ran on.
# shellcheck source=/dev/null  # linted on its own, like the check.d modules
. "$script_dir/hfoc-qemu-accel.sh"
hfoc_qemu_accel_enable
hfoc_qemu_accel_record

make_cmd=${HFOC_MAKE_CMD:-make}
jobs=${HFOC_QEMU_JOBS:-2}
timing_retry=${HFOC_QEMU_TIMING_RETRY:-1}
retry_limit=${HFOC_QEMU_RETRY_LIMIT:-3}
step_summary=${GITHUB_STEP_SUMMARY:-}

fail_usage() {
    echo "run-qemu-suite: $*" >&2
    exit 2
}

[[ $jobs =~ ^[1-9][0-9]*$ ]] ||
    fail_usage "HFOC_QEMU_JOBS must be a positive decimal integer"
[[ $timing_retry =~ ^[01]$ ]] ||
    fail_usage "HFOC_QEMU_TIMING_RETRY must be 0 or 1"
[[ $retry_limit =~ ^[0-9]+$ ]] ||
    fail_usage "HFOC_QEMU_RETRY_LIMIT must be a non-negative decimal integer"

declare -a phase_kinds=()
declare -a phase_targets=()

while (( $# )); do
    case $1 in
        --serial)
            shift
            if (( $# == 0 )) || [[ $1 == --* ]]; then
                fail_usage "--serial requires at least one target"
            fi
            while (( $# )) && [[ $1 != --serial && $1 != --parallel ]]; do
                [[ $1 != --* ]] || fail_usage "unknown option: $1"
                phase_kinds+=(serial)
                phase_targets+=("$1")
                shift
            done
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
# ITEM-510: every started target leaves one completion record here, written by
# the child itself before it exits; reap_one() falls back to it when the shell's
# own job bookkeeping loses a completion.
completion_dir=$log_dir/completions
mkdir -p "$completion_dir"
artifact_dir=${HFOC_QEMU_SUITE_ARTIFACT_DIR:-}
declare -A pid_target=()
declare -A pid_log=()
declare -A pid_started=()
declare -A retried_target=()
retries_used=0

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
    rm -rf "$log_dir"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# Whole seconds for the per-target durations reported below. The kernel drops
# the sub-second part, so two adjacent targets can report 0s.
now_seconds() {
    awk '{printf "%d\n", $1}' /proc/uptime
}

print_failure() {
    local target=$1
    local log_path=$2

    printf 'QEMU suite: FAIL %s\n' "$target" >&2
    printf '%s\n' "----- $target output -----" >&2
    cat "$log_path" >&2
    printf '%s\n' "----- end $target output -----" >&2
}

# ITEM-412: what kind of stop was this? A reader sees the class, the awaited
# marker and the deadline next to the target that failed, so a timing stop can
# never be mistaken for a failed assertion without opening the artifact.
report_failure_class() {
    local target=$1
    local log_path=$2
    local seconds=$3

    hfoc_qemu_failure_classify "$log_path"
    printf 'QEMU suite: FAIL-CLASS %s class=%s reason=%s seconds=%s' \
        "$target" "$hfoc_qemu_failure_class" "$hfoc_qemu_failure_reason" \
        "$seconds" >&2
    if [[ $hfoc_qemu_failure_class == marker-timeout ]]; then
        printf ' marker="%s" deadline=%s' \
            "$hfoc_qemu_failure_marker" "$hfoc_qemu_failure_deadline" >&2
    fi
    printf '\n' >&2
}

# The retry record goes to the job log, to the job summary and to the suite
# artifact directory, so a reader of any of the three sees the same line.
record_retry() {
    local target=$1
    local line

    line=$(printf 'QEMU suite: RETRY %s reason=marker-timeout attempt=2 marker="%s" deadline=%s' \
        "$target" "$hfoc_qemu_failure_marker" "$hfoc_qemu_failure_deadline")
    printf '%s\n' "$line"
    if [[ -n $artifact_dir ]]; then
        mkdir -p "$artifact_dir"
        printf '%s\n' "$line" >>"$artifact_dir/retries.txt"
    fi
    if [[ -n $step_summary ]]; then
        printf '%s\n' "$line" >>"$step_summary"
    fi
}

# Why the phase is not going to give this target a second attempt.
no_retry_reason() {
    local target=$1

    if [[ $hfoc_qemu_failure_class != marker-timeout ]]; then
        printf '%s' "$hfoc_qemu_failure_reason"
    elif [[ -n ${retried_target[$target]:-} ]]; then
        printf 'retry-exhausted'
    elif (( timing_retry == 0 )); then
        printf 'retry-disabled'
    else
        printf 'retry-limit:%s' "$retry_limit"
    fi
}

record_no_retry() {
    local target=$1
    local line

    line=$(printf 'QEMU suite: NO-RETRY %s class=%s reason=%s' \
        "$target" "$hfoc_qemu_failure_class" "$(no_retry_reason "$target")")
    printf '%s\n' "$line" >&2
    if [[ -n $step_summary ]]; then
        printf '%s\n' "$line" >>"$step_summary"
    fi
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

# Keep the log of a failed first attempt: the retry overwrites the target's own
# log, and the artifact must still show what the timing class looked like.
keep_first_attempt() {
    local target=$1
    local log_path=$2

    if [[ -z "$artifact_dir" ]]; then
        return 0
    fi
    mkdir -p "$artifact_dir"
    cp "$log_path" "$artifact_dir/$target.attempt1.log" 2>/dev/null || true
}

# One make invocation of one target; its exit status is returned unchanged.
run_target_once() {
    local target=$1
    local log_path=$2
    local status

    if "$make_cmd" -o deps -o iso "$target" >"$log_path" 2>&1; then
        return 0
    else
        status=$?
    fi
    return "$status"
}

# The retry may be granted only to the timing class, only once per target, and
# only while the phase has not spent its retry budget.
may_retry() {
    local target=$1

    hfoc_qemu_failure_class_is_timing &&
        [[ -z ${retried_target[$target]:-} ]] &&
        (( timing_retry == 1 )) &&
        (( retries_used < retry_limit ))
}

run_serial() {
    local target=$1
    local log_path="$log_dir/$target.log"
    local status
    local started

    started=$(now_seconds)
    if run_target_once "$target" "$log_path"; then
        save_target_artifact "$target" "$log_path" PASS
        printf 'QEMU suite: PASS %s seconds=%s\n' \
            "$target" "$(( $(now_seconds) - started ))"
        return 0
    else
        status=$?
    fi

    save_target_artifact "$target" "$log_path" FAIL
    report_failure_class "$target" "$log_path" "$(( $(now_seconds) - started ))"
    if may_retry "$target"; then
        keep_first_attempt "$target" "$log_path"
        retried_target[$target]=1
        retries_used=$((retries_used + 1))
        record_retry "$target"
        started=$(now_seconds)
        if run_target_once "$target" "$log_dir/$target.attempt2.log"; then
            save_target_artifact "$target" "$log_dir/$target.attempt2.log" PASS
            printf 'QEMU suite: PASS %s attempt=2 seconds=%s\n' \
                "$target" "$(( $(now_seconds) - started ))"
            return 0
        else
            status=$?
        fi
        save_target_artifact "$target" "$log_dir/$target.attempt2.log" FAIL
        print_failure "$target" "$log_dir/$target.attempt2.log"
        return "$status"
    fi

    record_no_retry "$target"
    print_failure "$target" "$log_path"
    return "$status"
}

start_target() {
    local target=$1
    local log_path="$log_dir/$target.log"
    local pid

    if [[ -n ${retried_target[$target]:-} ]]; then
        log_path="$log_dir/$target.attempt2.log"
    fi
    # ITEM-510: the child writes its own completion record before it exits, so
    # the phase never depends on the shell's job table alone.  Bash 5.2 can
    # reap a child and still answer `wait -n -p` without naming it (measured:
    # return 127, variable left unset), which killed a healthy phase under
    # `set -u`.  `$BASHPID` inside the subshell is the pid the parent sees as
    # `$!`, and the rename publishes the record atomically.
    #
    # The make invocation stays a child of this subshell, so the subshell
    # forwards TERM/INT to it: cleanup() kills the pid it knows - this subshell
    # - and a killed phase must not leave its make (and its guests) behind.
    (
        target_make_pid=
        # shellcheck disable=SC2317  # invoked by the traps below
        forward_signal() {
            [[ -n $target_make_pid ]] || return 0
            kill -TERM "$target_make_pid" 2>/dev/null || true
        }
        trap forward_signal TERM INT
        "$make_cmd" -o deps -o iso "$target" >"$log_path" 2>&1 &
        target_make_pid=$!
        child_status=0
        # `builtin`: this wait is the wrapper's own and must not be confused
        # with a `wait` shell function the environment may carry.
        builtin wait "$target_make_pid" || child_status=$?
        trap - TERM INT
        printf '%s\n' "$child_status" >"$completion_dir/$BASHPID.done.tmp" &&
            mv "$completion_dir/$BASHPID.done.tmp" \
                "$completion_dir/$BASHPID.done"
    ) &
    pid=$!
    pid_target[$pid]=$target
    pid_log[$pid]=$log_path
    pid_started[$pid]=$(now_seconds)
}

reap_one() {
    local finished_pid=
    local status
    local target
    local log_path
    local pid

    if wait -n -p finished_pid; then
        status=0
    else
        status=$?
    fi

    # ITEM-510: `wait -n -p` can lose a completion.  When it does, the record
    # the child wrote before exiting is the authority.  A completion with no
    # record at all is a harness fault: say so and stop, instead of
    # dereferencing an unset name under `set -u`.
    if [[ -z ${finished_pid:-} || -z ${pid_target[$finished_pid]:-} ]]; then
        finished_pid=
        for pid in "${!pid_target[@]}"; do
            [[ -f "$completion_dir/$pid.done" ]] || continue
            finished_pid=$pid
            break
        done
        [[ -n $finished_pid ]] || fail_usage \
            "a child completion was lost and no record was found"
    fi
    if [[ -f "$completion_dir/$finished_pid.done" ]]; then
        status=$(<"$completion_dir/$finished_pid.done")
        [[ $status =~ ^[0-9]+$ ]] || fail_usage \
            "the completion record of pid $finished_pid is not an exit status"
    fi
    rm -f "$completion_dir/$finished_pid.done"

    target=${pid_target[$finished_pid]:-}
    log_path=${pid_log[$finished_pid]:-}
    [[ -n $target && -n $log_path ]] ||
        fail_usage "received an unknown child completion"

    reaped_seconds=$(( $(now_seconds) - ${pid_started[$finished_pid]:-0} ))
    unset 'pid_target[$finished_pid]'
    unset 'pid_log[$finished_pid]'
    unset 'pid_started[$finished_pid]'
    reaped_target=$target
    reaped_log=$log_path
    reaped_status=$status
}

run_parallel() {
    local -a queue=("$@")
    local next=0
    local running=0
    local failed_target=
    local failed_log=
    local failed_status=0

    while (( next < ${#queue[@]} || running > 0 )); do
        while (( next < ${#queue[@]} && running < jobs )); do
            start_target "${queue[$next]}"
            next=$((next + 1))
            running=$((running + 1))
        done

        (( running > 0 )) || continue
        reap_one
        running=$((running - 1))

        if (( reaped_status == 0 )); then
            save_target_artifact "$reaped_target" "$reaped_log" PASS
            if [[ -n ${retried_target[$reaped_target]:-} ]]; then
                printf 'QEMU suite: PASS %s attempt=2 seconds=%s\n' \
                    "$reaped_target" "$reaped_seconds"
            else
                printf 'QEMU suite: PASS %s seconds=%s\n' \
                    "$reaped_target" "$reaped_seconds"
            fi
            continue
        fi

        save_target_artifact "$reaped_target" "$reaped_log" FAIL
        report_failure_class "$reaped_target" "$reaped_log" "$reaped_seconds"

        if may_retry "$reaped_target"; then
            keep_first_attempt "$reaped_target" "$reaped_log"
            retried_target[$reaped_target]=1
            retries_used=$((retries_used + 1))
            record_retry "$reaped_target"
            # The retry re-runs the same target with the same expectations. It
            # is queued behind the targets that are still running, so the phase
            # keeps its shape and the verdict comes from the retry.
            queue+=("$reaped_target")
            continue
        fi

        record_no_retry "$reaped_target"
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
