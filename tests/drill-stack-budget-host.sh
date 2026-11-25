#!/usr/bin/env bash
# ITEM-393: the boot-path proofs must not spend the boot stack.
#
# Every *_qemu_test_init() the kernel calls from kmain runs before the scheduler
# exists, i.e. on the stack the bootloader handed over. The
# storage-interruption target used to keep its whole fixture set in one 63 KiB
# stack frame; the frame grew down into the page tables the bootloader keeps
# just below the stack, the kernel's text mapping disappeared, and the guest
# died with an instruction-fetch page fault, a faulting double-fault handler,
# a triple fault and a silent reboot - which the CI reported only as
# "timed out waiting for storage-interruption proof OK".
#
# This check compiles each proof translation unit with the same flags its QEMU
# kernel uses and measures the stack frame its entry point allocates, so a
# proof that starts spending the boot stack again fails here, in a host check,
# instead of inside a QEMU run.
#
# Usage: drill-stack-budget-host.sh <cc> <kernel-cflags> <budget-bytes>
#        drill-stack-budget-host.sh --makefile <budget-bytes>
#
# The --makefile form reads the kernel CFLAGS block straight out of the
# Makefile, so the check suite can run the same measurement without a make
# recursion.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

if [[ "${1:-}" == --makefile ]]; then
    cc=cc
    budget=${2:?usage: drill-stack-budget-host.sh --makefile <budget-bytes>}
    kernel_cflags=$(sed -n '/^CFLAGS :=/,/[^\\]$/p' Makefile |
        sed -e 's/^CFLAGS := //' -e 's/\\$//' | tr '\n' ' ')
else
    cc=${1:?usage: drill-stack-budget-host.sh <cc> <kernel-cflags> <budget-bytes>}
    kernel_cflags=${2:?usage: drill-stack-budget-host.sh <cc> <kernel-cflags> <budget-bytes>}
    budget=${3:?usage: drill-stack-budget-host.sh <cc> <kernel-cflags> <budget-bytes>}
fi

[[ -n ${kernel_cflags// /} ]] || {
    echo "Cannot read the kernel CFLAGS block; pass them explicitly" >&2
    exit 1
}
[[ "$kernel_cflags" == *-Ikernel/include* ]] || {
    echo "Kernel CFLAGS block looks incomplete: $kernel_cflags" >&2
    exit 1
}

for command in "$cc" objdump awk; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done
[[ "$budget" =~ ^[0-9]+$ ]] || {
    echo "Budget must be a byte count: $budget" >&2
    exit 1
}

TEST_DIR=$(mktemp -d "${TMPDIR:-/tmp}/hfoc-drill-stack.XXXXXX")
trap 'rm -rf "$TEST_DIR"' EXIT

# define:translation-unit:entry-point, ordered by the boot-path proof they
# belong to. The list is the set of entry points that allocate more than a few
# hundred bytes when it was written (the drills of the resilience matrix and
# the proofs whose fixtures were the largest); it is deliberately a list of
# compiled proofs, not a grep, so the measurement is the real frame.
readonly PROOFS=(
    "HFOC_STORAGE_INTERRUPTION_QEMU_TEST:kernel/src/storage_interruption_qemu.c:storage_interruption_qemu_test_init"
    "HFOC_SERVICE_CASCADE_DRILL_QEMU_TEST:kernel/src/service_cascade_drill_qemu.c:service_cascade_drill_qemu_test_init"
    "HFOC_LOCAL_CONTROL_OUTAGE_DRILL_QEMU_TEST:kernel/src/local_control_outage_drill_qemu.c:local_control_outage_drill_qemu_test_init"
    "HFOC_NETWORK_PARTITION_DRILL_QEMU_TEST:kernel/src/network_partition_drill_qemu.c:network_partition_drill_qemu_test_init"
    "HFOC_TRUST_FAILURE_DRILL_QEMU_TEST:kernel/src/trust_failure_drill_qemu.c:trust_failure_drill_qemu_test_init"
    "HFOC_UPDATE_FAILURE_DRILL_QEMU_TEST:kernel/src/update_failure_drill_qemu.c:update_failure_drill_qemu_test_init"
    "HFOC_INTEGRATION_REPLAY_OUTAGE_DRILL_QEMU_TEST:kernel/src/integration_replay_outage_drill_qemu.c:integration_replay_outage_drill_qemu_test_init"
    "HFOC_POWER_CYCLE_CONTINUITY_DRILL_QEMU_TEST:kernel/src/power_cycle_continuity_drill_qemu.c:power_cycle_continuity_drill_qemu_test_init"
    "HFOC_SUPPORTABILITY_QEMU_TEST:kernel/src/supportability_qemu.c:supportability_qemu_test_init"
    "HFOC_ACCOUNTING_BOUNDARY_QEMU_TEST:kernel/src/accounting_boundary_qemu.c:accounting_boundary_qemu_test_init"
    "HFOC_UPDATE_LIFECYCLE_QEMU_TEST:kernel/src/update_lifecycle_qemu.c:update_lifecycle_qemu_test_init"
    "HFOC_BACKUP_RESTORE_QEMU_TEST:kernel/src/backup_restore_qemu.c:backup_restore_qemu_test_init"
    "HFOC_TRUST_POLICY_QEMU_TEST:kernel/src/trust_policy_qemu.c:trust_policy_qemu_test_init"
    "HFOC_USER_INIT_QEMU_TEST:kernel/src/user_init_qemu.c:user_init_qemu_test_init"
    "HFOC_APPLIANCE_PRODUCTION_MANAGEMENT_QEMU_TEST:kernel/src/appliance_production_management_qemu.c:appliance_production_management_qemu_test_init"
    "HFOC_VIRTIO_FOUNDATION_QEMU_TEST:kernel/src/virtio_foundation_qemu.c:virtio_foundation_qemu_test_init"
    "HFOC_APPLIANCE_PRODUCTION_SERVICE_HTTPS_QEMU_TEST:kernel/src/appliance_production_service_https_qemu.c:appliance_production_service_https_qemu_test_init"
)

failures=0
for entry in "${PROOFS[@]}"; do
    define=${entry%%:*}
    rest=${entry#*:}
    source=${rest%%:*}
    symbol=${rest##*:}
    object=$TEST_DIR/$(basename "${source%.c}").o

    disassembly=$TEST_DIR/$(basename "${source%.c}").disasm

    "$cc" $kernel_cflags "-D${define}" -c "$source" -o "$object" || {
        echo "FAIL: cannot compile $source with -D$define" >&2
        exit 1
    }
    # ITEM-437: the disassembly is written to a file and measured from there.
    # The frame instruction is found early in the output, so an awk reading
    # straight from the pipe exits with most of the disassembly still being
    # written: objdump takes SIGPIPE, `set -o pipefail` turns that into the
    # status of the assignment, and `set -e` ends the measurement - which made
    # this check pass under CI load and fail on an idle host, the opposite of
    # what a budget check should do.  A file has no reader to close, and
    # objdump's own status is checked instead of being masked by the pipe.
    if ! objdump -d "$object" >"$disassembly"; then
        printf 'FAIL: cannot disassemble %s\n' "$object" >&2
        failures=$((failures + 1))
        continue
    fi
    frame=$(awk -v target="$symbol" '
        /^[0-9a-f]+ <.*>:$/ {
            name = $2
            gsub(/[<>:]/, "", name)
            next
        }
        name == target {
            if ($0 ~ /sub[[:space:]]+\$0x[0-9a-f]+,%rsp/) {
                line = $0
                sub(/.*\$0x/, "", line)
                sub(/,%rsp.*/, "", line)
                printf "0x%s\n", line
                found = 1
                exit
            }
        }
        END {
            if (!found) {
                print "0x0"
            }
        }
    ' "$disassembly")
    if [[ -z "$frame" ]]; then
        printf 'FAIL: %s allocates no measurable frame (entry point renamed?)\n' \
            "$symbol" >&2
        failures=$((failures + 1))
        continue
    fi
    frame_bytes=$((frame))
    if (( frame_bytes > budget )); then
        printf 'FAIL: %s allocates a %d-byte boot stack frame, budget is %d bytes\n' \
            "$symbol" "$frame_bytes" "$budget" >&2
        failures=$((failures + 1))
        continue
    fi
    printf 'PASS: %-52s frame=%d bytes\n' "$symbol" "$frame_bytes"
done

if (( failures != 0 )); then
    printf '\n%d FAILURES\n' "$failures"
    exit 1
fi
echo 'Boot-path proof stack budget: OK'
