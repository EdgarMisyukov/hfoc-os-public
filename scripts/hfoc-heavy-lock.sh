#!/usr/bin/env bash
# ITEM-370A: host-wide heavy-work lock.
#
# One lock file serializes heavy work on the shared CI host:
#
#   * every heavy CI phase (static-host, acceptance-qemu) runs its work under
#     `ci-shared`, i.e. with a shared lock, so the two phases of one run stay
#     parallel — that pair is the measured-good configuration of ITEM-370;
#   * every local heavy build goes through `local`, i.e. an exclusive lock
#     with the host priority settings applied, so a local `make` run waits for
#     a live CI phase instead of overlapping it and provoking QEMU marker
#     timeouts.
#
# Both sides use this one tool, so the rule is enforced by tooling rather than
# by a prompt line. The wait is bounded and a timeout is an explicit
# diagnostic with a distinct exit code (75); an unusable lock path is a
# configuration error (78) and the command is NOT run unlocked.
#
# Usage:
#   hfoc-heavy-lock.sh ci-shared <phase> -- <command> [args...]
#   hfoc-heavy-lock.sh ci-exclusive <phase> -- <command> [args...]
#   hfoc-heavy-lock.sh local [--wait <seconds>] -- <command> [args...]
#
# Environment:
#   HFOC_HEAVY_LOCK        lock file (default /var/lock/hfoc-heavy.lock)
#   HFOC_HEAVY_CI_WAIT     shared-mode wait bound in seconds (default 1200)
#   HFOC_HEAVY_CI_EXCLUSIVE_WAIT
#                          exclusive-mode CI wait bound in seconds (default
#                          4500); ITEM-431: a shared holder waits for a moment
#                          of another phase, but the exclusive post-merge gate
#                          waits for a whole live phase - measured at 15-30.5
#                          minutes in 2026-09-16, and at 47 minutes on
#                          2026-11-01 - and 1200 s made it time out
#                          (EX_TEMPFAIL=75) and colour `main` twice on
#                          2026-09-16
#   HFOC_HEAVY_LOCAL_WAIT  local-build wait bound in seconds (default 2400)
#   HFOC_HEAVY_NICE        local niceness (default 10)
#   HFOC_HEAVY_IONICE_CLASS / HFOC_HEAVY_IONICE_LEVEL  (defaults 2 / 7)
set -Eeuo pipefail

readonly DEFAULT_LOCK_FILE=/var/lock/hfoc-heavy.lock
# ITEM-431: the post-merge gate waits for a whole live phase, not for a moment
# of one.  The longest measured pull-request phase is 30.5 minutes (the
# single-job band before the split) and the acceptance phase sits in the 15-30
# minute band, so 1200 s was shorter than the thing it waited for: on
# 2026-09-16 the post-merge static phase died with EX_TEMPFAIL=75 waiting for a
# live pull-request phase and took `main` red with it.  The exclusive budget is
# 75 minutes - the acceptance job's own timeout, so the gate waits for any
# phase that can still be alive - while a shared holder keeps the short bound:
# it coexists with the phase it waits beside rather than waiting for it to
# finish.  ITEM-584 raised this from 45 minutes after the 2026-11-01 post-merge
# run died with EX_TEMPFAIL=75 behind a 47-minute acceptance phase; the verify
# job timeout moved from 75 to 90 minutes so the wait plus the static phase
# still fits.
readonly DEFAULT_CI_WAIT=1200
readonly DEFAULT_CI_EXCLUSIVE_WAIT=4500
readonly DEFAULT_LOCAL_WAIT=2400
readonly DEFAULT_NICE=10
readonly DEFAULT_IONICE_CLASS=2
readonly DEFAULT_IONICE_LEVEL=7
readonly EX_TEMPFAIL=75
readonly EX_CONFIG=78

name=${0##*/}

# Marker lines go to stderr so the wrapped command's stdout stays clean and
# parseable; CI merges both streams into the job log.
marker() {
    # shellcheck disable=SC2059  # every call site passes a literal format
    printf "$@" >&2
}

usage() {
    sed -n '2,36p' "$0" | sed 's/^# \{0,1\}//'
}

config_error() {
    printf '%s: %s\n' "$name" "$*" >&2
    exit "$EX_CONFIG"
}

mode=
phase=
wait_seconds=

case ${1:-} in
    ci-shared)
        [[ $# -ge 3 ]] || config_error 'ci-shared needs <phase> -- <command>'
        phase=$2
        shift 2
        [[ ${1:-} == '--' ]] || config_error 'ci-shared needs -- before the command'
        shift
        mode=shared
        wait_seconds=${HFOC_HEAVY_CI_WAIT:-$DEFAULT_CI_WAIT}
        ;;
    ci-exclusive)
        # ITEM-425: the post-merge gate is heavy work like any other, and it
        # used to be a shared holder beside a live pull-request phase.  The
        # measurement that closed that door: on 2026-09-16 two acceptance
        # phases of one pull request failed on marker timeouts while a
        # post-merge `make check` was live on the same host
        # (`test-lapic-timer-oneshot`, `test-pic-disable` and
        # `test-console-history` all expired inside their own deadlines, and
        # the retry rule was spent).  Shared holders coexist by design, so the
        # post-merge phase asks for the lock exclusively instead: it waits for
        # a live phase to finish, and a pull-request phase waits for it.
        [[ $# -ge 3 ]] || config_error 'ci-exclusive needs <phase> -- <command>'
        phase=$2
        shift 2
        [[ ${1:-} == '--' ]] || config_error 'ci-exclusive needs -- before the command'
        shift
        mode=exclusive
        wait_seconds=${HFOC_HEAVY_CI_EXCLUSIVE_WAIT:-${HFOC_HEAVY_CI_WAIT:-$DEFAULT_CI_EXCLUSIVE_WAIT}}
        ;;
    local)
        shift
        while [[ ${1:-} == '--wait' ]]; do
            [[ $# -ge 2 ]] || config_error '--wait needs a value'
            wait_seconds=$2
            shift 2
        done
        if [[ ${1:-} == '--' ]]; then
            shift
        fi
        mode=exclusive
        phase=local
        wait_seconds=${wait_seconds:-${HFOC_HEAVY_LOCAL_WAIT:-$DEFAULT_LOCAL_WAIT}}
        ;;
    -h | --help | help)
        usage
        exit 0
        ;;
    *)
        usage >&2
        exit 64
        ;;
esac

[[ $# -gt 0 ]] || config_error 'no command given'
[[ $wait_seconds =~ ^[0-9]+$ ]] || config_error "wait bound is not a number: $wait_seconds"
[[ $phase =~ ^[a-z0-9-]+$ ]] || config_error "invalid phase name: $phase"

lock_file=${HFOC_HEAVY_LOCK:-$DEFAULT_LOCK_FILE}
holders_dir=$lock_file.holders
holder_file=
started_epoch=$(date +%s)
waited_seconds=0

apply_priority() {
    local nice_level
    local ionice_class
    local ionice_level
    nice_level=${HFOC_HEAVY_NICE:-$DEFAULT_NICE}
    ionice_class=${HFOC_HEAVY_IONICE_CLASS:-$DEFAULT_IONICE_CLASS}
    ionice_level=${HFOC_HEAVY_IONICE_LEVEL:-$DEFAULT_IONICE_LEVEL}
    [[ $nice_level =~ ^[0-9]+$ ]] || config_error "HFOC_HEAVY_NICE is not a number: $nice_level"
    marker 'HFOC_HEAVY_RUN state=start mode=%s phase=%s nice=%s ionice=%s:%s wait_s=%s\n' \
        "$mode" "$phase" "$nice_level" "$ionice_class" "$ionice_level" "$waited_seconds"
    if command -v ionice >/dev/null 2>&1; then
        nice -n "$nice_level" ionice -c "$ionice_class" -n "$ionice_level" -- "$@" 9>&-
    else
        nice -n "$nice_level" -- "$@" 9>&-
    fi
}

register_holder() {
    local tmp
    mkdir -p "$holders_dir" 2>/dev/null || return 0
    chmod 1777 "$holders_dir" 2>/dev/null || true
    tmp=$holders_dir/.$$.$(date +%s%N)
    holder_file=$holders_dir/$$.$(date +%s%N)
    if printf 'pid=%s mode=%s phase=%s user=%s started_epoch=%s\n' \
        "$$" "$mode" "$phase" "${USER:-unknown}" "$started_epoch" >"$tmp" 2>/dev/null &&
        mv -f "$tmp" "$holder_file" 2>/dev/null; then
        return 0
    fi
    rm -f "$tmp" 2>/dev/null || true
    holder_file=
    return 0
}

# A holder killed before its EXIT trap ran (a cancelled CI job) leaves a stale
# diagnostics file: the kernel already released the lock, only the file
# remains. Prune those so the registry names live holders only.
prune_holders() {
    local file pid
    [[ -d $holders_dir ]] || return 0
    for file in "$holders_dir"/*; do
        [[ -e $file ]] || continue
        pid=$(sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' "$file")
        if [[ -z $pid || ! -d /proc/$pid ]]; then
            rm -f "$file" 2>/dev/null || true
        fi
    done
}

report_holders() {
    local file pid started running_for
    local found=0
    [[ -d $holders_dir ]] || return 0
    prune_holders
    for file in "$holders_dir"/*; do
        [[ -e $file ]] || continue
        pid=$(sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' "$file")
        if [[ -n $pid && -d /proc/$pid ]]; then
            started=$(sed -n 's/.*started_epoch=\([0-9][0-9]*\).*/\1/p' "$file")
            running_for=0
            if [[ -n $started ]]; then
                running_for=$(($(date +%s) - started))
            fi
            printf '%s: holder: %s running_for=%ss\n' "$name" "$(cat "$file")" "$running_for" >&2
            found=1
        else
            # A dead holder leaves a stale diagnostics file; the lock itself
            # was already released by the kernel, so only the file is stale.
            rm -f "$file" 2>/dev/null || true
        fi
    done
    if (( found == 0 )); then
        printf '%s: holder: unknown (no live holder entry in %s)\n' "$name" "$holders_dir" >&2
    fi
}

# shellcheck disable=SC2317  # invoked through the EXIT trap
finish() {
    local status=$?
    trap - EXIT
    if [[ -n $holder_file ]]; then
        rm -f "$holder_file" 2>/dev/null || true
    fi
    if [[ -n ${lock_fd_open:-} ]]; then
        marker 'HFOC_HEAVY_LOCK state=released mode=%s phase=%s lock=%s elapsed_s=%s exit=%s\n' \
            "$mode" "$phase" "$lock_file" "$(($(date +%s) - started_epoch))" "$status"
        flock -u 9 2>/dev/null || true
        { exec 9>&-; } 2>/dev/null || true
    fi
    if [[ $mode == exclusive ]]; then
        marker 'HFOC_HEAVY_RUN state=done mode=%s phase=%s exit=%s elapsed_s=%s\n' \
            "$mode" "$phase" "$status" "$(($(date +%s) - started_epoch))"
    fi
    exit "$status"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# A nested invocation (a local build that itself calls a heavy wrapper) must
# not deadlock on the exclusive lock its ancestor already holds.
if [[ ${HFOC_HEAVY_LOCK_HELD:-} == "$lock_file" ]]; then
    marker 'HFOC_HEAVY_LOCK state=reentrant mode=%s phase=%s lock=%s\n' \
        "$mode" "$phase" "$lock_file"
    export HFOC_HEAVY_MODE=$mode
    export HFOC_HEAVY_PHASE=$phase
    if [[ $mode == exclusive ]]; then
        status=0
        apply_priority "$@" || status=$?
        exit "$status"
    fi
    status=0
    "$@" 9>&- || status=$?
    exit "$status"
fi

if [[ ! -e $lock_file ]]; then
    # 0666 so that the CI runner user and the worker user can share one lock;
    # /var/lock is world-writable with the sticky bit, like the shellcheck lock.
    (umask 000; : >>"$lock_file") 2>/dev/null || true
fi

if ! { exec 9<>"$lock_file"; } 2>/dev/null; then
    # A read-only descriptor is enough for flock(2); accept a pre-created
    # lock file that the other user owns.
    if ! { exec 9<"$lock_file"; } 2>/dev/null; then
        config_error "cannot open lock file '$lock_file' for reading or writing; \
create it world-writable (for example: sudo install -m 0666 /dev/null '$lock_file') \
or point HFOC_HEAVY_LOCK at a shared path"
    fi
fi
lock_fd_open=1
export HFOC_HEAVY_LOCK_HELD=$lock_file
export HFOC_HEAVY_MODE=$mode
export HFOC_HEAVY_PHASE=$phase

flock_mode=-x
[[ $mode == shared ]] && flock_mode=-s

marker 'HFOC_HEAVY_LOCK state=waiting mode=%s phase=%s lock=%s wait_s=%s\n' \
    "$mode" "$phase" "$lock_file" "$wait_seconds"
if ! flock -w "$wait_seconds" "$flock_mode" 9; then
    marker 'HFOC_HEAVY_LOCK state=timeout mode=%s phase=%s lock=%s wait_s=%s\n' \
        "$mode" "$phase" "$lock_file" "$wait_seconds"
    printf '%s: timed out after %ss waiting for %s access to %s\n' \
        "$name" "$wait_seconds" "$mode" "$lock_file" >&2
    report_holders
    exit "$EX_TEMPFAIL"
fi
waited_seconds=$(($(date +%s) - started_epoch))

marker 'HFOC_HEAVY_LOCK state=acquired mode=%s phase=%s lock=%s wait_s=%s\n' \
    "$mode" "$phase" "$lock_file" "$waited_seconds"
prune_holders
register_holder

status=0
if [[ $mode == exclusive ]]; then
    apply_priority "$@" || status=$?
else
    "$@" 9>&- || status=$?
fi
exit "$status"
