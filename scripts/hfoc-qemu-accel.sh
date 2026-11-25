#!/usr/bin/env bash
# ITEM-442: run the QEMU proofs on the host's own virtualization when it is
#           there, and say which one ran.
#
# Every proof starts qemu-system-x86_64 itself, and every one of them has run
# under TCG so far - the plumbing never passed an accelerator, and /dev/kvm
# exists on this host.  A TCG guest that shares the machine with another guest
# runs many times slower than wall clock, which is the root of the timing class
# docs/ci-self-hosted-runner.md describes (a proof that did not observe its
# marker inside a deadline) and of the retry and timeout budgets built around
# it.  KVM removes that gap: the guest runs at host speed, so the same deadlines
# become generous instead of marginal.
#
# The switch is one decision, made here:
#
#   HFOC_QEMU_ACCEL=tcg   (default)  the emulator, which is what every proof
#                                    ran on before this file existed;
#   HFOC_QEMU_ACCEL=kvm              KVM, and a loud failure when it is not
#                                    usable - never a silent fallback;
#   HFOC_QEMU_ACCEL=auto             KVM when the device below is usable by
#                                    this user, TCG otherwise.
#
# TCG is the default because KVM was *measured* on this host and lost: the same
# proof (test-production-storage-boot-qemu, five guest boots) took 59 s under
# TCG and 98 s under KVM, reproducibly, interleaved.  This host is itself a
# virtual machine, so the guests are nested and every MMIO/PIO exit is a
# second-level exit; the proofs are I/O-bound boots, which is the worst case
# for nested virtualization and the best case for the emulator.  The switch is
# therefore here for a host where the measurement goes the other way - a
# physical machine, a different workload - and the number above is what a
# reader needs before flipping it.  docs/ci-self-hosted-runner.md carries the
# measurement and its context.
#
# Adoption is a PATH shim plus this file: a caller sources this file, calls
# hfoc_qemu_accel_enable, and every qemu-system-x86_64 its children start goes
# through scripts/qemu-accel/qemu-system-x86_64, which prepends the accelerator
# arguments and passes everything else through untouched.  A proof that passes
# its own -accel or -enable-kvm is left exactly as it wrote it.
#
# The device path is a variable so the host drill can exercise every branch
# without a virtual machine.

# shellcheck disable=SC2034  # the fields are read by the caller's record line
hfoc_qemu_accel_mode=
hfoc_qemu_accel_reason=
hfoc_qemu_accel_args=

hfoc_qemu_accel_device() {
    printf '%s' "${HFOC_QEMU_ACCEL_DEVICE:-/dev/kvm}"
}

# True when this user may ask the kernel for a virtual machine.
hfoc_qemu_accel_usable() {
    local device
    device=$(hfoc_qemu_accel_device)
    [[ -r $device && -w $device ]]
}

# Resolve the requested mode into hfoc_qemu_accel_mode/_reason/_args.  Returns
# non-zero only when KVM was demanded and cannot be used: a caller that asked
# for it must fail rather than quietly run the emulator.
hfoc_qemu_accel_resolve() {
    local requested=${HFOC_QEMU_ACCEL:-tcg}

    hfoc_qemu_accel_mode=
    hfoc_qemu_accel_reason=
    hfoc_qemu_accel_args=
    case $requested in
    tcg)
        hfoc_qemu_accel_mode=tcg
        hfoc_qemu_accel_reason=requested
        hfoc_qemu_accel_args='-accel tcg'
        ;;
    kvm)
        if ! hfoc_qemu_accel_usable; then
            hfoc_qemu_accel_mode=kvm
            hfoc_qemu_accel_reason="unusable:$(hfoc_qemu_accel_device)"
            echo "hfoc-qemu-accel: HFOC_QEMU_ACCEL=kvm but $(hfoc_qemu_accel_device) is not readable and writable by $(id -un)" >&2
            return 1
        fi
        hfoc_qemu_accel_mode=kvm
        hfoc_qemu_accel_reason=requested
        hfoc_qemu_accel_args='-accel kvm'
        ;;
    auto)
        if hfoc_qemu_accel_usable; then
            hfoc_qemu_accel_mode=kvm
            hfoc_qemu_accel_reason="available:$(hfoc_qemu_accel_device)"
            hfoc_qemu_accel_args='-accel kvm'
        else
            hfoc_qemu_accel_mode=tcg
            hfoc_qemu_accel_reason="no:$(hfoc_qemu_accel_device)"
            hfoc_qemu_accel_args='-accel tcg'
        fi
        ;;
    *)
        echo "hfoc-qemu-accel: HFOC_QEMU_ACCEL must be auto, kvm or tcg, not '$requested'" >&2
        return 2
        ;;
    esac
    return 0
}

# The one line a phase prints, so a reader can prove from the log which
# accelerator the guests of that phase ran on and why.
hfoc_qemu_accel_record() {
    printf 'HFOC_QEMU_ACCEL mode=%s reason=%s device=%s user=%s\n' \
        "$hfoc_qemu_accel_mode" "$hfoc_qemu_accel_reason" \
        "$(hfoc_qemu_accel_device)" "$(id -un)"
}

# Resolve, put the shim first on PATH and export the decision to the children.
hfoc_qemu_accel_enable() {
    local shim_dir
    shim_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)/qemu-accel

    hfoc_qemu_accel_resolve || return 1
    export HFOC_QEMU_ACCEL
    export HFOC_QEMU_ACCEL_MODE=$hfoc_qemu_accel_mode
    export HFOC_QEMU_ACCEL_ARGS=$hfoc_qemu_accel_args
    case :$PATH: in
    *:"$shim_dir":*) ;;
    *) PATH="$shim_dir:$PATH" ;;
    esac
    export PATH
    return 0
}
