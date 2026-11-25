#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in cc mktemp; do
    command -v "$command" >/dev/null || {
        echo "Не найдена команда: $command" >&2
        exit 1
    }
done

TEST_BINARY=$(mktemp)
cleanup() {
    rm -f "$TEST_BINARY"
}
trap cleanup EXIT

cc -std=c11 -Wall -Wextra -Werror -Wpedantic \
    -DHFOC_DISPATCH_HOST_TEST \
    -DHFOC_PIC_IRQ1_BOOT_POLICY_HOST_TEST \
    -DHFOC_IRQ0_OWNERSHIP_HOST_TEST \
    -DHFOC_CPU_HOST_TEST \
    -Itests/include -Ikernel/include \
    tests/pic-spurious-irqs.c \
    kernel/src/arch/x86_64/pic.c \
    kernel/src/cpu.c \
    kernel/src/arch/x86_64/interrupts.c \
    -Wl,--wrap=interrupt_dispatch \
    -o "$TEST_BINARY"
"$TEST_BINARY"
