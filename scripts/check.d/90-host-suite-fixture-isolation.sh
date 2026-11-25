#!/usr/bin/env bash
# ITEM-485: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Two proofs that share one file must never run in the same parallel phase.
#
# The boot-log reader (`tests/appliance-boot-log-reader-host.py`) runs the store
# proof as its prerequisite and then reads the image that proof leaves behind
# (`build/test-appliance-boot-log-host.img`).  On 2026-09-26 the CI run on
# `f2400e52` failed `test-appliance-boot-log-store-host` with 11 FAILURES: the
# standalone store target and the reader's own store run were both in the
# parallel phase and wrote that image at the same time, so each proof asserted
# against the other's interleaved file.  The class is "a proof that produces a
# fixture and a proof that consumes it must be separated in time"; this slice
# pins the scheduling that separates them:
#
#   * the reader is a serial gate target, run after the parallel phase;
#   * the store target stays in the parallel phase - a target put in the gate
#     group but not passed as `--serial` would never run at all;
#   * `test-host-suite` passes the reader as `--serial`, so the gate group
#     cannot silently stop naming it;
#   * the manifest proof carries the same assertion, so a later reshuffle is
#     caught where the phase groups are computed.

for item485_file in \
    Makefile \
    tests/test-host-suite-manifest.sh \
    tests/appliance-boot-log-store-host.c \
    tests/appliance-boot-log-reader-host.py \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item485_file ]] || {
        echo "ITEM-485: отсутствует $item485_file" >&2
        exit 1
    }
done

# --------------------------------------------------- the shared fixture file --
grep -Fq '#define STORE_IMAGE "build/test-appliance-boot-log-host.img"' \
    tests/appliance-boot-log-store-host.c || {
    echo "ITEM-485: проба store не называет общий образ" >&2
    exit 1
}
grep -Fq 'DEFAULT_IMAGE = os.path.join(ROOT, "build", "test-appliance-boot-log-host.img")' \
    tests/appliance-boot-log-reader-host.py || {
    echo "ITEM-485: читатель не называет общий образ" >&2
    exit 1
}

# ------------------------------------------ the phase that separates them ---
item485_gate_block=$(awk '
    /^HOST_GATE_TARGETS :=/ { inside = 1 }
    inside { print; if ($0 !~ /\\$/) exit }
' Makefile)
grep -Fq 'test-appliance-boot-log-reader-host' <<<"$item485_gate_block" || {
    echo "ITEM-485: читатель не в последовательной фазе" >&2
    exit 1
}
if grep -Fq 'test-appliance-boot-log-store-host' <<<"$item485_gate_block"; then
    echo "ITEM-485: проба store вынесена из параллельной фазы" >&2
    exit 1
fi
grep -Fq -- '--serial test-appliance-boot-log-reader-host' Makefile || {
    echo "ITEM-485: test-host-suite не запускает читателя последовательно" >&2
    exit 1
}

# -------------------------------------------------- the proof behind it -----
grep -Fq 'the boot-log reader must be scheduled in the serial gate phase' \
    tests/test-host-suite-manifest.sh || {
    echo "ITEM-485: манифест не проверяет разделение фаз" >&2
    exit 1
}
bash tests/test-host-suite-manifest.sh || {
    echo "ITEM-485: манифест host suite не подтверждает разделение" >&2
    exit 1
}
