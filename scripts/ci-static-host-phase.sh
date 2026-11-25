#!/usr/bin/env bash
# ITEM-370A: heavy body of the CI `static-host` verification phase.
#
# This script is started by scripts/hfoc-heavy-lock.sh in shared mode, so the
# whole heavy section of the phase holds the host-wide heavy lock: a local
# heavy build waits for it instead of overlapping it. The body lives here
# rather than in the workflow so that the lock covers one continuous section
# per phase and scripts/check.sh can assert the command set phase by phase.
set -Eeuo pipefail

event_name=${GITHUB_EVENT_NAME:-}

# ITEM-387: the check suite is split into bounded scripts/check.d/ modules, so
# no single ShellCheck invocation needs gigabytes any more, but the modules must
# be linted on purpose: `scripts/*.sh` does not match `scripts/check.d/*.sh`.
# Every invocation still takes the host-wide shellcheck lock in addition to the
# heavy lock. The two locks differ, so this cannot deadlock with the wrapper.
lock=/var/lock/hfoc-shellcheck.lock
touch "$lock" 2>/dev/null || lock="${RUNNER_TEMP:-/tmp}/hfoc-shellcheck.lock"
failed=0
# ITEM-442: scripts/qemu-accel/qemu-system-x86_64 is the PATH shim the QEMU
# phases put in front of the emulator; it has no .sh suffix because it has to
# carry the binary's name, so it is named here rather than left unlinted.
for script in scripts/*.sh scripts/check.d/*.sh scripts/qemu-accel/*; do
    echo "ShellCheck: $script"
    if ! flock "$lock" shellcheck "$script"; then
        failed=1
    fi
done
if (( failed != 0 )); then
    echo "ShellCheck failed"
    exit 1
fi
echo "ShellCheck passed"

# ITEM-387: prove that the split kept every enforced statement of the check
# suite, in the same order, against the frozen pre-split contract stream.
bash scripts/verify-check-contracts.sh

(
    # shellcheck disable=SC2034  # read by the EXIT trap
    started_at=$SECONDS
    trap 'echo "HFOC static checks elapsed: $((SECONDS - started_at))s"' EXIT
    make check
)

# The host suite is the expensive half of the phase and runs for pull requests
# and manual dispatches only; a main push stays the bounded ~2.5 min check.
if [[ "$event_name" == 'pull_request' || "$event_name" == 'workflow_dispatch' ]]; then
    (
        # shellcheck disable=SC2034  # read by the EXIT trap
    started_at=$SECONDS
        trap 'echo "HFOC host suite elapsed: $((SECONDS - started_at))s"' EXIT
        # ITEM-582: `make check` above is the same target the suite lists as
        # its first serial step; skipping that step here removes the duplicate
        # without touching the suite's standalone composition.
        make test-host-suite HOST_SUITE_CHECK=0
    )
fi
