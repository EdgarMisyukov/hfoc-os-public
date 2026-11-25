#!/usr/bin/env bash
# ITEM-412: the failure class of one QEMU suite target.
#
# A proof stops for one of two very different reasons, and the suite must be able
# to tell them apart from the log alone:
#
#   * marker-timeout - the proof did not observe an expected marker inside its
#     own deadline. The guest may still be perfectly healthy: it is starved, its
#     time lags wall time, and the proof ran out of patience. A proof declares
#     this class itself, by writing exactly one record immediately before it
#     exits:
#
#         HFOC-QEMU-MARKER-TIMEOUT marker="<marker>" deadline=<seconds>s
#
#     The record carries the marker and the deadline, so a reader sees what was
#     awaited and for how long without opening the artifact.
#   * assertion - everything else: a failed expectation, an in-guest proof
#     failure (`[TAG] failed: ...`), a kernel panic, a missing marker in a
#     required set, a fail-closed guard, or a nonzero exit that carries no
#     record at all.
#
# The classification is fail-closed by construction:
#
#   * only an explicit, well-formed record makes a target timing-class; the
#     record must carry both `marker="..."` and `deadline=<n>s` in that order,
#     and nothing else on the line;
#   * a log that also contains any fail-closed signature (a panic, an in-guest
#     `failed:` record, a `... reported failure` line) is never timing-class,
#     even when it carries a record - a proof that reports a real failure is
#     never retried;
#   * an unreadable or empty log is never timing-class.
#
# Only the timing class is retryable, and only once per target per suite run
# (`scripts/run-qemu-suite.sh`). See `docs/ci-self-hosted-runner.md` for the
# rule and the measured evidence behind it.
#
# Usage (after sourcing):
#
#     hfoc_qemu_failure_classify "$log_path"
#     printf '%s %s\n' "$hfoc_qemu_failure_class" "$hfoc_qemu_failure_marker"

# The canonical record. `scripts/check.d/45-qemu-marker-timeout-retry.sh` pins
# the prefix, the strict form and the fail-closed signatures below.
# shellcheck disable=SC2034  # consumed by proofs and by the check slice
HFOC_QEMU_MARKER_TIMEOUT_RECORD='HFOC-QEMU-MARKER-TIMEOUT'

# Signatures of a real failure. Any one of them suppresses the timing class.
#
# ITEM-463: the `Kernel panic` signature carries a trailing space on purpose.
# A healthy guest prints `Kernel panic: none` as its own all-clear, and the
# unanchored substring swallowed that all-clear: every marker-timeout log that
# also printed it was classified as an assertion, never retried, and failed the
# phase on a starved-but-healthy guest (measured twice on 2026-09-20, run
# 35507821604 attempts 1 and 2).  Every real panic this suite produces is
# either the kernel's `*** HFOC KERNEL PANIC ***` or a proof's own
# `Kernel panic <word>` line, so the trailing space keeps the failures and
# drops the colon-form all-clear.  The slice's fixtures pin both directions.
HFOC_QEMU_FAIL_CLOSED_SIGNATURES=(
    '*** HFOC KERNEL PANIC ***'
    'Kernel panic '
    'reported failure'
    ' failed: '
)

# Result of hfoc_qemu_failure_classify; read by the sourcing suite.
# shellcheck disable=SC2034  # read by scripts/run-qemu-suite.sh
hfoc_qemu_failure_class=assertion
hfoc_qemu_failure_marker=
hfoc_qemu_failure_deadline=
hfoc_qemu_failure_reason=not-classified

hfoc_qemu_failure_classify() {
    local log_path=$1
    local line
    local signature
    local record_seen=0
    local marker=
    local deadline=

    hfoc_qemu_failure_class=assertion
    hfoc_qemu_failure_marker=
    hfoc_qemu_failure_deadline=
    hfoc_qemu_failure_reason=no-marker-timeout-record

    if [[ ! -r $log_path ]]; then
        hfoc_qemu_failure_reason=unreadable-log
        return 0
    fi

    while IFS= read -r line; do
        if [[ $line =~ ^HFOC-QEMU-MARKER-TIMEOUT[[:space:]]marker=\"([^\"]+)\"[[:space:]]deadline=([0-9]+([.][0-9]+)?)s$ ]]; then
            record_seen=1
            marker=${BASH_REMATCH[1]}
            deadline=${BASH_REMATCH[2]}s
        fi
    done <"$log_path"

    if (( record_seen == 0 )); then
        return 0
    fi

    for signature in "${HFOC_QEMU_FAIL_CLOSED_SIGNATURES[@]}"; do
        if grep -Fq -- "$signature" "$log_path"; then
            hfoc_qemu_failure_reason="fail-closed-signature:$signature"
            return 0
        fi
    done

    hfoc_qemu_failure_class='marker-timeout'
    hfoc_qemu_failure_marker=$marker
    hfoc_qemu_failure_deadline=$deadline
    hfoc_qemu_failure_reason='marker-timeout'
    return 0
}

# 0 when the last classification is the retryable timing class.
hfoc_qemu_failure_class_is_timing() {
    [[ $hfoc_qemu_failure_class == marker-timeout ]]
}
