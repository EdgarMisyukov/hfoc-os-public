#!/usr/bin/env bash
# ITEM-588: the update keeps a default-only state and names its refusals.
#           Sourced in pinned order by scripts/check.sh; run ShellCheck on its
#           own.

# The owner's Toshiba refused the first hardware update with
# `PRODUCTION_INSTALLER_STATE_NOT_PRESERVABLE writes=0` although the target
# disk carried a loadable state: its stores held only the defaults (the account
# could not be set on a CPU without an entropy source).  The claims pinned
# here:
#
#   * a loadable state is the condition, not a configured one: the preflight
#     accepts a default-only volume and keeps it;
#   * every outcome carries the evidence the console and the journal name -
#     the current and the planned persistence ranges, the reader's verdict and
#     whether a configuration and an account were present;
#   * the refusal still happens before the first write, for a plan that moves
#     the range and for a volume the reader cannot load;
#   * the update medium declares its own layout, and the QEMU proof drives the
#     four cases on a legacy-BIOS install.
state_source=kernel/src/production_installer_state.c
flow_source=kernel/src/production_install_flow.c

grep -Fq 'PRODUCTION_INSTALLER_STATE_PRESERVE_REFUSAL_RANGE_MISMATCH' \
    kernel/include/production_installer_state.h || {
    echo "ITEM-588: the refusal has no named range-mismatch reason" >&2
    exit 1
}
grep -Fq 'production_installer_state_preserve_refusal_string' "$state_source" || {
    echo "ITEM-588: the refusal reason has no stable name" >&2
    exit 1
}
grep -Fq 'evidence->current_first_lba' "$state_source" || {
    echo "ITEM-588: the current persistence range is not recorded" >&2
    exit 1
}
grep -Fq 'evidence->plan_first_lba' "$state_source" || {
    echo "ITEM-588: the planned persistence range is not recorded" >&2
    exit 1
}
grep -Fq 'evidence->loader_ready' "$state_source" || {
    echo "ITEM-588: the state reader's verdict is not recorded" >&2
    exit 1
}
if grep -Fq 'if (!evidence->config_preserved) {' "$state_source"; then
    echo "ITEM-588: a default-only state is still a refusal condition" >&2
    exit 1
fi
grep -Fq 'state preserve refused=' "$flow_source" || {
    echo "ITEM-588: the refusal publishes no evidence record" >&2
    exit 1
}
grep -Fq '" writes=0\n"' "$flow_source" || {
    echo "ITEM-588: the refusal record does not state writes=0" >&2
    exit 1
}
grep -Fq 'loader=READY' "$flow_source" || {
    echo "ITEM-588: the verified record names no reader verdict" >&2
    exit 1
}
grep -Fq 'a loadable default-only volume is updated, not refused' \
    tests/production-installer-orchestration-host.c || {
    echo "ITEM-588: the host proof does not pin the default-only update" >&2
    exit 1
}
grep -Fq 'a moved persistence range is refused with both ranges named' \
    tests/production-installer-orchestration-host.c || {
    echo "ITEM-588: the host proof does not pin the refusal evidence" >&2
    exit 1
}
grep -Fq 'production-installer-update-media' Makefile || {
    echo "ITEM-588: no update medium target" >&2
    exit 1
}
grep -Fq 'test-production-installer-update-qemu' Makefile || {
    echo "ITEM-588: the canonical suite does not schedule the update proof" >&2
    exit 1
}
grep -Fq 'state preserve refused=RANGE_MISMATCH' \
    scripts/test-qemu-production-install-flow-update.sh || {
    echo "ITEM-588: the QEMU proof does not drive the range refusal" >&2
    exit 1
}
grep -Fq 'ITEM-588' docs/ROADMAP.md || {
    echo "ITEM-588: ROADMAP does not record the update fix" >&2
    exit 1
}
