#!/usr/bin/env bash
# ITEM-442: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The QEMU proofs run on the host's own virtualization when it is there.
#
# Every proof starts qemu-system-x86_64 itself and every one of them ran under
# TCG, which is the root of the timing class docs/ci-self-hosted-runner.md
# describes: a TCG guest sharing the host runs many times slower than wall
# clock, so a proof that polls for a marker expires on a deadline that is
# generous on an idle machine.  One decision file, one PATH shim and three
# adoption points give every proof the accelerator the phase resolved, without
# touching a hundred proof scripts, and each phase prints the record a reader
# uses to prove which one ran.
for qemu_accel_file in \
    scripts/hfoc-qemu-accel.sh \
    scripts/qemu-accel/qemu-system-x86_64 \
    tests/test-qemu-accel-host.sh; do
    [[ -f $qemu_accel_file ]] || {
        echo "ITEM-442: отсутствует $qemu_accel_file" >&2
        exit 1
    }
done

# The decision: three modes, one usable-device probe, a demanded kvm that fails
# loudly instead of falling back, and the record line.
# shellcheck disable=SC2016  # the compared text is literal, not expanded
for qemu_accel_decision in \
    'HFOC_QEMU_ACCEL=auto' \
    'HFOC_QEMU_ACCEL=kvm' \
    'HFOC_QEMU_ACCEL=tcg' \
    'hfoc_qemu_accel_usable()' \
    'hfoc_qemu_accel_resolve()' \
    'hfoc_qemu_accel_enable()' \
    'hfoc_qemu_accel_record()' \
    "hfoc_qemu_accel_args='-accel kvm'" \
    "hfoc_qemu_accel_args='-accel tcg'" \
    'not readable and writable by' \
    'HFOC_QEMU_ACCEL must be auto, kvm or tcg' \
    'local requested=${HFOC_QEMU_ACCEL:-tcg}'; do
    grep -Fq -- "$qemu_accel_decision" scripts/hfoc-qemu-accel.sh || {
        echo "ITEM-442: решение об ускорителе не содержит:" \
            "$qemu_accel_decision" >&2
        exit 1
    }
done

# The shim: it prepends the resolved arguments, never overrides a proof that
# chose its own accelerator, and execs the real emulator.
# shellcheck disable=SC2016  # the compared text is literal, not expanded
for qemu_accel_shim in \
    'HFOC_QEMU_ACCEL_ARGS' \
    'HFOC_QEMU_ACCEL_REAL' \
    '-accel | -accel=* | -enable-kvm)' \
    'exec "$real" "$@"' \
    'exec "$real" $args "$@"'; do
    grep -Fq -- "$qemu_accel_shim" scripts/qemu-accel/qemu-system-x86_64 || {
        echo "ITEM-442: шим ускорителя не содержит:" "$qemu_accel_shim" >&2
        exit 1
    }
done

# Every phase that starts proofs adopts it, and says so.
for qemu_accel_runner in \
    scripts/run-qemu-suite.sh \
    scripts/run-acceptance-suite.sh \
    scripts/ci-acceptance-qemu-phase.sh; do
    for qemu_accel_adoption in \
        'hfoc-qemu-accel.sh' \
        'hfoc_qemu_accel_enable' \
        'hfoc_qemu_accel_record'; do
        grep -Fq -- "$qemu_accel_adoption" "$qemu_accel_runner" || {
            echo "ITEM-442: $qemu_accel_runner не делает выбор ускорителя:" \
                "$qemu_accel_adoption" >&2
            exit 1
        }
    done
done

# The shim has no .sh suffix - it has to carry the emulator's name - so the CI
# lint names it explicitly instead of globbing past it.
grep -Fq 'scripts/qemu-accel/*' scripts/ci-static-host-phase.sh || {
    echo "ITEM-442: шим ускорителя не попадает в линт статической фазы" >&2
    exit 1
}

# The drill is registered in the canonical host suite (the recipe is what the
# manifest derives its target list from) and drives every branch on the host.
# shellcheck disable=SC2016  # the compared text is literal, not expanded
grep -Fq '	$(MAKE) test-qemu-accel-host' Makefile || {
    echo "ITEM-442: проба ускорителя не зарегистрирована в host suite" >&2
    exit 1
}
grep -Fq 'test-qemu-accel-host:' Makefile || {
    echo "ITEM-442: цели пробы ускорителя нет" >&2
    exit 1
}
for qemu_accel_drill in \
    'the default is tcg' \
    'auto with a usable device chooses kvm' \
    'auto without the device chooses tcg' \
    'a demanded kvm without the device fails' \
    'an unknown HFOC_QEMU_ACCEL is refused' \
    'the shim prepends the accelerator' \
    'an explicit -accel is not overridden' \
    'without resolved arguments the shim changes nothing'; do
    grep -Fq -- "$qemu_accel_drill" tests/test-qemu-accel-host.sh || {
        echo "ITEM-442: host-проба не проверяет:" "$qemu_accel_drill" >&2
        exit 1
    }
done

# The item is recorded where a maintainer looks, including the one host change
# that brings KVM to CI.
for qemu_accel_doc in \
    'HFOC_QEMU_ACCEL' \
    'hfoc-runner' \
    '/dev/kvm'; do
    grep -Fq -- "$qemu_accel_doc" docs/ci-self-hosted-runner.md || {
        echo "docs/ci-self-hosted-runner.md не фиксирует ITEM-442:" \
            "$qemu_accel_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-442' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-442" >&2
    exit 1
}
