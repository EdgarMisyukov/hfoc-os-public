#!/usr/bin/env bash
# ITEM-370A: heavy body of the CI `acceptance-qemu` verification phase.
#
# This script is started by scripts/hfoc-heavy-lock.sh in shared mode, so the
# whole heavy section of the phase holds the host-wide heavy lock: a local
# heavy build waits for it instead of overlapping it and provoking QEMU marker
# timeouts. It must never carry the ~9 GiB ShellCheck or a second kernel
# build; scripts/check.sh asserts that.
set -Eeuo pipefail

# ITEM-442: every guest this phase starts goes through the accelerator decision
# in scripts/hfoc-qemu-accel.sh - the PATH shim it installs is inherited by the
# proof scripts the make targets below run.  With HFOC_QEMU_ACCEL=auto (the
# default) a host this user may open /dev/kvm on runs KVM and a host without it
# runs TCG, so the phase behaves the same in CI as it did before this item until
# the runner is granted access, and says which one it used.
# ITEM-585: per-step and per-QEMU-target timing.  Durations are monotonic
# (/proc/uptime), one flock-protected line per finished unit, and the records
# are scoped to this job/run/attempt so a retry can never read the previous
# attempt's file.
timing_root=${HFOC_CI_TIMINGS_DIR:-build/test-artifacts/ci-timings}
timing_job=${GITHUB_JOB:-local}
timing_run=${GITHUB_RUN_ID:-manual}
timing_attempt=${GITHUB_RUN_ATTEMPT:-1}
timing_sha=${GITHUB_SHA:-$(git rev-parse HEAD 2>/dev/null || echo unknown)}
timing_phase=acceptance-qemu
timing_dir=$timing_root/"$timing_job-$timing_run-$timing_attempt"
timing_file=$timing_dir/acceptance-qemu.tsv
phase_started_ms=$(awk '{printf "%d", $1 * 1000}' /proc/uptime)
wrapper_recorded=0

now_ms() {
    awk '{printf "%d", $1 * 1000}' /proc/uptime
}

record_timing() {
    # kind name status rc ms retry started_ms
    local line
    line=$(printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s' \
        "$timing_job" "$timing_attempt" "$timing_sha" "$timing_phase" \
        "$1" "$2" "$3" "$4" "$5" "$6" "$7")
    mkdir -p "$timing_dir" 2>/dev/null || return 0
    (
        flock 9
        printf '%s\n' "$line" >>"$timing_file"
    ) 9>>"$timing_file.lock" 2>/dev/null || printf '%s\n' "$line" >>"$timing_file" 2>/dev/null || true
}

record_wrapper() {
    local status=$1
    (( wrapper_recorded == 0 )) || return 0
    wrapper_recorded=1
    record_timing phase-wrapper acceptance-qemu \
        "$([[ $status == 0 ]] && echo PASS || echo FAIL)" "$status" \
        "$(($(now_ms) - phase_started_ms))" "" "$phase_started_ms"
}

run_step() {
    local name=$1
    shift
    local started status
    started=$(now_ms)
    if "$@"; then
        status=0
    else
        status=$?
    fi
    record_timing phase-step "$name" \
        "$([[ $status == 0 ]] && echo PASS || echo FAIL)" "$status" \
        "$(($(now_ms) - started))" "" "$started"
    return "$status"
}

# The QEMU suite prints one `seconds=` line per target (PASS or FAIL); those
# durations join the same record file as the host targets, so one summary
# reads both runners.
record_qemu_durations() {
    local log=$1
    local line status name secs
    local -A fail_seconds=()
    local -A retry_attempt=()
    [[ -f $log ]] || return 0
    # First pass: the class record carries the duration of a failed target and
    # the RETRY record carries the attempt number the target was rerun at.
    while IFS= read -r line; do
        if [[ $line =~ ^QEMU\ suite:\ FAIL-CLASS\ ([^ ]+)\ .*seconds=([0-9]+) ]]; then
            fail_seconds[${BASH_REMATCH[1]}]=${BASH_REMATCH[2]}
        elif [[ $line =~ ^QEMU\ suite:\ RETRY\ ([^ ]+)\ .*attempt=([0-9]+) ]]; then
            retry_attempt[${BASH_REMATCH[1]}]=${BASH_REMATCH[2]}
        fi
    done <"$log"
    # Second pass: one record per target, failures included, with the retry
    # number when the suite gave that target a second attempt.
    while IFS= read -r line; do
        if [[ $line =~ ^QEMU\ suite:\ PASS\ ([^ ]+)\ seconds=([0-9]+)$ ]]; then
            name=${BASH_REMATCH[1]}
            secs=${BASH_REMATCH[2]}
            record_timing qemu-target "$name" PASS 0 \
                "$((secs * 1000))" "${retry_attempt[$name]:-}" ""
        elif [[ $line =~ ^QEMU\ suite:\ FAIL\ ([^ ]+)$ ]]; then
            name=${BASH_REMATCH[1]}
            secs=${fail_seconds[$name]:-0}
            record_timing qemu-target "$name" FAIL 1 \
                "$((secs * 1000))" "${retry_attempt[$name]:-}" ""
        fi
    done <"$log"
}

trap 'status=$?; record_wrapper "$status"; exit "$status"' EXIT

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
# shellcheck source=/dev/null  # linted on its own, like the check.d modules
. "$script_dir/hfoc-qemu-accel.sh"
hfoc_qemu_accel_enable
hfoc_qemu_accel_record

(
    # shellcheck disable=SC2034  # read by the EXIT trap
    started_at=$SECONDS
    trap 'echo "HFOC acceptance elapsed: $((SECONDS - started_at))s"' EXIT
    run_step release-appliance-v1 make test-release-appliance-v1-acceptance
    run_step https-vertical-slice make test-https-vertical-slice-acceptance
    run_step appliance-platform-release make test-hfoc-appliance-platform-release-acceptance
    run_step installer-dual-boot-management make test-hfoc-integrated-installer-dual-boot-management-acceptance
    run_step nvme-production make test-nvme-production-acceptance
    run_step release-documentation make test-hfoc-release-documentation
)

(
    # shellcheck disable=SC2034  # read by the EXIT trap
    started_at=$SECONDS
    # ITEM-432: this is where the kernel and the test images are compiled, so
    # this is where the warning gate belongs.  A warning is not a style opinion
    # here: the production build carried exactly two - a dead declaration and a
    # function that says it cannot return but does - for several items, and
    # nothing failed on them because no phase read the build it produced.  The
    # build's own output is captured and scanned before any proof runs, so a
    # new warning fails the phase that produced it instead of being read past.
    build_log=$(mktemp "${TMPDIR:-/tmp}/hfoc-acceptance-build.XXXXXX")
    # shellcheck disable=SC2034  # read by the EXIT trap
    trap 'rm -f "$build_log"; echo "HFOC qemu elapsed: $((SECONDS - started_at))s"' EXIT
    # The production end-to-end acceptance gate harvests the QEMU evidence
    # produced by this same job, so the suite directory is reset first and a
    # stale run can never satisfy a stage.
    rm -rf build/test-artifacts/qemu-suite
    # The acceptance step built the production kernel with its output
    # redirected into that step's artifact logs, so the objects are current and
    # the compiler would say nothing here.  `kernel-rebuild` makes it compile
    # the production set again - the one the item is about, macros and all -
    # and `qemu-test-images` then packages the images the suite runs.
    run_step kernel-rebuild bash -c "set -o pipefail; make kernel-rebuild 2>&1 | tee -a '$build_log'"
    # The reading itself lives in scripts/hfoc-build-warning-gate.sh, so the
    # host suite can drive every shape of it on captured logs; a non-zero
    # verdict aborts the phase here, and the trap removes the log.  The gate
    # covers the production kernel, which is the build the item is about; the
    # QEMU-proof variant kernels are separate configurations whose own
    # pre-existing warnings are recorded as a follow-up, not silently blessed.
    bash scripts/hfoc-build-warning-gate.sh "$build_log"
    run_step qemu-test-images bash -c "set -o pipefail; make qemu-test-images 2>&1 | tee -a '$build_log'"
    qemu_log=$timing_dir/qemu-suite.log
    mkdir -p "$timing_dir"
    # Parse the suite's own `seconds=` records even when it fails: the
    # artifact must include failed targets, not only the successful run.
    qemu_status=0
    run_step qemu-suite bash -c "set -o pipefail; make test-qemu-parallel-after-acceptance 2>&1 | tee -a '$qemu_log'" || qemu_status=$?
    record_qemu_durations "$qemu_log"
    (( qemu_status == 0 )) || exit "$qemu_status"
)

(
    # shellcheck disable=SC2034  # read by the EXIT trap
    started_at=$SECONDS
    trap 'echo "HFOC production acceptance elapsed: $((SECONDS - started_at))s"' EXIT
    run_step production-end-to-end make test-production-end-to-end-acceptance
)
