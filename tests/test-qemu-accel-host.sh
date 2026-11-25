#!/usr/bin/env bash
# ITEM-442: the QEMU accelerator plumbing, driven on the host.
#
# scripts/hfoc-qemu-accel.sh decides once per phase whether the proofs run on
# the host's virtualization, and scripts/qemu-accel/qemu-system-x86_64 is the
# shim every proof reaches through PATH.  Both are shell, so both are driven
# here without starting a single virtual machine: the device path is a
# variable, the real emulator is /bin/echo, and every branch - auto with and
# without the device, forced kvm, forced tcg, an unknown value, the shim's
# pass-through and its respect for a proof that chose for itself - is asserted.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C
# shellcheck disable=SC2154  # hfoc_qemu_accel_* are assigned by the sourced file

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
SHIM=$ROOT/scripts/qemu-accel/qemu-system-x86_64
# shellcheck source=/dev/null  # linted on its own, like the check.d modules
. "$ROOT/scripts/hfoc-qemu-accel.sh"
# The fields the sourced file assigns; declared here so a reader of this file -
# and ShellCheck, which lints one file at a time - sees them.
hfoc_qemu_accel_mode=
hfoc_qemu_accel_reason=
hfoc_qemu_accel_args=
# shellcheck disable=SC2153  # the exported name is the interface, not a typo
HFOC_QEMU_ACCEL_ARGS=${HFOC_QEMU_ACCEL_ARGS:-}

TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT

failures=0
check() {
    local what=$1 got=$2 want=$3

    if [[ $got == "$want" ]]; then
        echo "PASS: $what"
    else
        echo "FAIL: $what (got '$got', want '$want')" >&2
        failures=$((failures + 1))
    fi
}

# A device this user can read and write is what auto looks for; /dev/null is
# one on every machine, and a path that does not exist is the negative.
USABLE=/dev/null
UNUSABLE=$TEST_DIR/absent-kvm

# 0. the default - no HFOC_QEMU_ACCEL at all - is the emulator, because KVM
#    was measured slower on this host (59 s vs 98 s on the storage-boot proof).
unset HFOC_QEMU_ACCEL
hfoc_qemu_accel_resolve
check "the default is tcg" "$hfoc_qemu_accel_mode" tcg
check "and is recorded as requested" "$hfoc_qemu_accel_reason" requested

# 1. auto, device usable: KVM.
HFOC_QEMU_ACCEL=auto HFOC_QEMU_ACCEL_DEVICE=$USABLE hfoc_qemu_accel_resolve
check "auto with a usable device chooses kvm" "$hfoc_qemu_accel_mode" kvm
check "and asks QEMU for it" "$hfoc_qemu_accel_args" "-accel kvm"
check "and says why" "$hfoc_qemu_accel_reason" "available:$USABLE"

# 2. auto, device absent: TCG, with the same arguments a proof would have
#    written before this item.
HFOC_QEMU_ACCEL=auto HFOC_QEMU_ACCEL_DEVICE=$UNUSABLE hfoc_qemu_accel_resolve
check "auto without the device chooses tcg" "$hfoc_qemu_accel_mode" tcg
check "and says which device was missing" "$hfoc_qemu_accel_reason" "no:$UNUSABLE"

# 3. tcg requested explicitly.
HFOC_QEMU_ACCEL=tcg HFOC_QEMU_ACCEL_DEVICE=$USABLE hfoc_qemu_accel_resolve
check "tcg requested stays tcg" "$hfoc_qemu_accel_mode" tcg
check "and is recorded as requested" "$hfoc_qemu_accel_reason" requested

# 4. kvm demanded and unusable: a loud failure, never a silent fallback.
if HFOC_QEMU_ACCEL=kvm HFOC_QEMU_ACCEL_DEVICE=$UNUSABLE \
    hfoc_qemu_accel_resolve 2>"$TEST_DIR/kvm.err"; then
    echo "FAIL: a demanded kvm without the device must fail" >&2
    failures=$((failures + 1))
else
    echo "PASS: a demanded kvm without the device fails"
fi
if grep -q 'not readable and writable' "$TEST_DIR/kvm.err"; then
    echo "PASS: and the failure names the device and the user"
else
    echo "FAIL: the refusal does not say why" >&2
    failures=$((failures + 1))
fi

# 5. an unknown value is a configuration error, not a mode.
if HFOC_QEMU_ACCEL=sometimes hfoc_qemu_accel_resolve 2>/dev/null; then
    echo "FAIL: an unknown HFOC_QEMU_ACCEL must be refused" >&2
    failures=$((failures + 1))
else
    echo "PASS: an unknown HFOC_QEMU_ACCEL is refused"
fi

# 6. enable() resolves, exports the arguments and puts the shim first on PATH.
if (
    HFOC_QEMU_ACCEL=kvm HFOC_QEMU_ACCEL_DEVICE=$USABLE hfoc_qemu_accel_enable
    shim_dir=$(dirname -- "$SHIM")
    case $PATH in
    "$shim_dir":*) exit 0 ;;
    *) exit 1 ;;
    esac
); then
    echo "PASS: enable puts the shim first on PATH"
else
    echo "FAIL: enable did not prepend the shim" >&2
    failures=$((failures + 1))
fi
if (
    HFOC_QEMU_ACCEL=kvm HFOC_QEMU_ACCEL_DEVICE=$USABLE hfoc_qemu_accel_enable
    [[ $HFOC_QEMU_ACCEL_ARGS == '-accel kvm' ]]
); then
    echo "PASS: enable exports the resolved arguments"
else
    echo "FAIL: enable did not export the arguments" >&2
    failures=$((failures + 1))
fi

# 7. the shim prepends the resolved arguments and passes the rest through.
got=$(HFOC_QEMU_ACCEL_REAL=/bin/echo HFOC_QEMU_ACCEL_ARGS='-accel kvm' \
    "$SHIM" -machine q35 -m 512M -cdrom proof.iso)
check "the shim prepends the accelerator" "$got" \
    "-accel kvm -machine q35 -m 512M -cdrom proof.iso"

# 8. a proof that chose its own accelerator is left alone.
got=$(HFOC_QEMU_ACCEL_REAL=/bin/echo HFOC_QEMU_ACCEL_ARGS='-accel kvm' \
    "$SHIM" -accel tcg -machine q35)
check "an explicit -accel is not overridden" "$got" "-accel tcg -machine q35"
got=$(HFOC_QEMU_ACCEL_REAL=/bin/echo HFOC_QEMU_ACCEL_ARGS='-accel kvm' \
    "$SHIM" -enable-kvm -machine q35)
check "an explicit -enable-kvm is not overridden" "$got" \
    "-enable-kvm -machine q35"

# 9. nothing resolved: the command line is exactly the proof's own.
got=$(HFOC_QEMU_ACCEL_REAL=/bin/echo "$SHIM" -machine q35 -m 512M)
check "without resolved arguments the shim changes nothing" "$got" \
    "-machine q35 -m 512M"

if ((failures != 0)); then
    echo "QEMU accelerator host drill: FAIL ($failures)" >&2
    exit 1
fi
echo "QEMU accelerator host drill: OK"
