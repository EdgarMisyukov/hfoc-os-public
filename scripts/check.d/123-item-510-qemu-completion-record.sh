#!/usr/bin/env bash
# ITEM-510: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# A QEMU phase must not lose a completion to the shell's job bookkeeping.
#
# On 2026-10-03 run 37116056572 turned a healthy static phase red after 6 m:
# bash 5.2.37 reaped a suite child and `wait -n -p` answered 127 with the
# variable left unset, so `set -u` killed scripts/run-qemu-suite.sh on the next
# reference and the phase lost the verdict it had already earned.  The verdict
# therefore comes from a record the child writes itself before it exits, and a
# lost completion with no record at all is a loud usage stop, not a crash.
for completion_record_file in \
    scripts/run-qemu-suite.sh \
    tests/test-qemu-suite-host.sh \
    docs/ci-self-hosted-runner.md; do
    [[ -f $completion_record_file ]] || {
        echo "ITEM-510: отсутствует $completion_record_file" >&2
        exit 1
    }
done
# The child publishes its own exit status before it exits; the rename publishes
# the record atomically, so the parent can never read a half-written status.
# shellcheck disable=SC2016  # the compared text is literal source
for completion_record_contract in \
    'completion_dir=$log_dir/completions' \
    '"$completion_dir/$BASHPID.done.tmp"' \
    '"$completion_dir/$BASHPID.done"' \
    'child_status=$?'; do
    grep -Fq -- "$completion_record_contract" scripts/run-qemu-suite.sh || {
        echo "ITEM-510: нет записи о завершении: $completion_record_contract" >&2
        exit 1
    }
done
# The reap path must not take the verdict from `wait -n -p` alone, and a lost
# completion without a record must stop the phase instead of crashing.
# shellcheck disable=SC2016  # the compared text is literal source
for completion_record_recovery in \
    'a child completion was lost and no record was found' \
    'the completion record of pid' \
    '"$completion_dir/$finished_pid.done"'; do
    grep -Fq -- "$completion_record_recovery" scripts/run-qemu-suite.sh || {
        echo "ITEM-510: нет восстановления потерянного завершения:" \
            "$completion_record_recovery" >&2
        exit 1
    }
done
# The drill is deterministic: an exported `wait` reaps one child, returns 127
# and names nothing - the measured shape - and the phase must keep the child's
# verdict.  A recordless loss stops with the usage status.
for completion_record_case in \
    'HFOC_QEMU_TEST_LOST_COMPLETION' \
    'export -f wait' \
    'builtin wait -n' \
    'lost_completion_run' \
    'a lost completion did not classify the assertion' \
    'a child completion was lost and no record was found'; do
    grep -Fq -- "$completion_record_case" tests/test-qemu-suite-host.sh || {
        echo "ITEM-510: host-проба не проверяет потерянное завершение:" \
            "$completion_record_case" >&2
        exit 1
    }
done
# The retry contract of ITEM-412 stays as it is: the completion record is a
# second source of the verdict, never a second retry rule.
for completion_record_retry in \
    'QEMU suite: FAIL-CLASS %s class=%s reason=%s seconds=%s' \
    'QEMU suite: RETRY %s reason=marker-timeout attempt=2' \
    'QEMU suite: NO-RETRY %s class=%s reason=%s'; do
    grep -Fq -- "$completion_record_retry" scripts/run-qemu-suite.sh || {
        echo "ITEM-510: контракт повтора ITEM-412 потерян:" \
            "$completion_record_retry" >&2
        exit 1
    }
done
# A maintainer reads the rule where the runner is documented.
for completion_record_doc in \
    'ITEM-510' \
    'lost completion' \
    'completions/<pid>.done'; do
    grep -Fq -- "$completion_record_doc" docs/ci-self-hosted-runner.md || {
        echo "docs/ci-self-hosted-runner.md не фиксирует ITEM-510:" \
            "$completion_record_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-510' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-510" >&2
    exit 1
}
