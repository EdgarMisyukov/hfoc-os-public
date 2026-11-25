#!/usr/bin/env bash
# Exact target-set regression for the bounded host suite runner.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT

awk '
    /^test-host-suite-serial:/ { in_serial = 1; next }
    in_serial && /^[^[:space:]#]/ { exit }
    in_serial && /^[[:space:]]*[$][(]MAKE[)]/ {
        sub(/^[[:space:]]*[$][(]MAKE[)] /, "")
        print
    }
' Makefile >"$TEST_DIR/serial-targets.txt"

make -s host-suite-phase-list >"$TEST_DIR/runner-targets.txt"

serial_count=$(wc -l <"$TEST_DIR/serial-targets.txt")
runner_count=$(wc -l <"$TEST_DIR/runner-targets.txt")

# ITEM-390: the canonical size of the host suite is re-derived from the serial
# recipe itself instead of being guessed here, so adding a host proof cannot
# desync this manifest. The protection against a silently shrinking suite is the
# pinned floor below plus the explicit per-target membership checks that follow:
# a suite that loses a proof fails the floor, and a suite that loses one of the
# pinned proofs fails its membership check.
HOST_SUITE_TARGET_FLOOR=319
[[ "$serial_count" -ge "$HOST_SUITE_TARGET_FLOOR" ]] || {
    echo "canonical host serial manifest contains $serial_count targets, below the pinned floor of $HOST_SUITE_TARGET_FLOOR" >&2
    exit 1
}
[[ "$runner_count" == "$serial_count" ]] || {
    echo "runner host manifest contains $runner_count targets, expected the canonical $serial_count" >&2
    exit 1
}

if ! diff -u <(sort "$TEST_DIR/serial-targets.txt") \
    <(sort "$TEST_DIR/runner-targets.txt"); then
    echo "host runner manifest does not match canonical test-host-suite serial manifest" >&2
    exit 1
fi

for pinned in \
    test-pmm-host \
    test-pmm-limited-host \
    test-pmm-highmem-host \
    test-pmm-pci-hole-host \
    test-drill-stack-budget-host \
    test-drill-stack-budget-race-host \
    test-qemu-marker-wait-host \
    test-keyboard-init-host \
    test-usb-hid-host \
    test-xhci-host \
    test-xhci-evidence-host \
    test-xhci-route-host \
    test-xhci-dma-host \
    test-xhci-handover-host \
    test-xhci-composite-host \
    test-pointer-host \
    test-local-input-host \
    test-local-menu-host \
    test-local-network-setup-host \
    test-loop-heartbeat-host \
    test-installer-layout-contract-host \
    test-installer-gpt-preserve-host \
    test-efi-fat32-esp-payload-set-host \
    test-production-installer-boot-stage-host \
    test-production-installer-bios-boot-host \
    test-installed-boot-disk-host \
    test-rtl8168-host \
    test-production-install-media-host \
    test-merge-guard-host \
    test-build-warning-gate-host \
    test-framebuffer-glyph-host \
    test-framebuffer-scroll-host \
    test-console-host \
    test-panel-login-host; do
    grep -Fxq "$pinned" "$TEST_DIR/serial-targets.txt" || {
        echo "canonical host serial manifest missing pinned target: $pinned" >&2
        exit 1
    }
    grep -Fxq "$pinned" "$TEST_DIR/runner-targets.txt" || {
        echo "host runner manifest missing pinned target: $pinned" >&2
        exit 1
    }
done

for gate in \
    test-certificate-fail-closed-matrix-host \
    test-production-artifact-secret-gate-host \
    test-mbedtls-secret-host \
    test-appliance-boot-log-reader-host; do
    grep -Fxq "$gate" "$TEST_DIR/runner-targets.txt" || {
        echo "host runner manifest missing gate target: $gate" >&2
        exit 1
    }
done

# ITEM-485: two proofs that share one file must never run in the same phase.
#
# The boot-log reader runs the store proof as its prerequisite and reads the
# image that proof leaves behind, so it is scheduled in the serial gate phase
# that follows the parallel phase: with both in the parallel phase, the reader's
# own store run and the standalone store target would write
# `build/test-appliance-boot-log-host.img` at the same time, and the run's
# assertions failed on the interleaved image (11 FAILURES in run 36230645539's
# first attempt).  The store target stays in the parallel phase, because a gate
# target that is not passed as `--serial` is never run at all.
gate_block=$(awk '
    /^HOST_GATE_TARGETS :=/ { inside = 1 }
    inside { print; if ($0 !~ /\\$/) exit }
' Makefile)
grep -Fq 'test-appliance-boot-log-reader-host' <<<"$gate_block" || {
    echo "the boot-log reader must be scheduled in the serial gate phase" >&2
    exit 1
}
if grep -Fq 'test-appliance-boot-log-store-host' <<<"$gate_block"; then
    echo "the boot-log store proof must stay in the parallel phase" >&2
    exit 1
fi

echo 'Host suite manifest regression: OK'
