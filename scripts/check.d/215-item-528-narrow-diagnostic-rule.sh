#!/usr/bin/env bash
# ITEM-528: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with the shellcheck tool.
#
# ITEM-526 introduced the shared normaliser with a rule that removed every line
# beginning with an interrupt prefix.  That form also deleted console records
# the proofs require, because those records happen to begin with the same
# prefixes:
#   * "[IRQ ] IF enabled; entering HLT event loop."  (test-qemu-irqs.sh,
#     test-qemu-integrated-appliance-boot-acceptance.sh)
#   * "[PIC ] Master mask: 0x00000000000000FF; slave mask: 0x..."
#     (test-qemu-ioapic-irq0.sh, test-qemu-ioapic-irq1.sh)
#   * "[PIC ] disabled; APIC runtime active."        (test-qemu-ioapic-irq3-7.sh,
#     test-qemu-ioapic-irq8-15.sh)
# A proof whose python half waited for one of them through the view would wait
# forever, and the two halves would disagree again - the defect ITEM-525/526/527
# closed, in the other direction.
#
# This slice pins the narrowed rule:
#   * only the periodic lines the kernel writes from interrupt context are
#     removed, and the witness derives that set from the kernel source;
#   * the shared renderer and the three embedded helpers carry the same literal;
#   * the records above survive the view and the proofs still require them;
#   * the witness, its registration and the runner document keep the contract.
#
# The pins below are literal source text, so `$(...)` must not expand and a
# trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item528_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item528_root" || exit 1

for item528_file in \
    scripts/hfoc-serial-view.py \
    scripts/test-qemu-production-service-https.sh \
    scripts/test-qemu-production-forward.sh \
    scripts/test-qemu-production-dhcp-dns.sh \
    kernel/src/arch/x86_64/interrupts.c \
    tests/serial-view-normalised-host.py \
    Makefile \
    docs/ci-self-hosted-runner.md; do
    [[ -f $item528_file ]] || {
        echo "ITEM-528: отсутствует $item528_file" >&2
        exit 1
    }
done

# 1. The one rule is now the periodic-line rule, and it is byte-identical in the
#    shared renderer and in every embedded helper.  The broad ITEM-526 form must
#    not come back anywhere.
item528_rule=$(cat <<'RULE'
    r"\[(?:IOAPIC|LAPIC-TIMER)\] irq[0-9]*"
    r"(?: vector=0x[0-9A-Fa-f]+| lapic_eoi)[^\[\r\n]*\r?\n")
RULE
)
for item528_holder in \
    scripts/hfoc-serial-view.py \
    scripts/test-qemu-production-service-https.sh \
    scripts/test-qemu-production-forward.sh \
    scripts/test-qemu-production-dhcp-dns.sh; do
    grep -Fq -- "$item528_rule" "$item528_holder" || {
        echo "ITEM-528: $item528_holder не несёт суженное правило" >&2
        exit 1
    }
done
if grep -Fq 'r"\[(?:IOAPIC|LAPIC|PIC|PIT|IRQ)[^\[\r\n]*\r?\n")' \
        scripts/hfoc-serial-view.py \
        scripts/test-qemu-production-service-https.sh \
        scripts/test-qemu-production-forward.sh \
        scripts/test-qemu-production-dhcp-dns.sh; then
    echo "ITEM-528: широкое правило ITEM-526 вернулось" >&2
    exit 1
fi

# 2. The records the narrowing was made for are still required by their proofs:
#    the rule may only change if those assertions change with it.
for item528_record in \
    'scripts/test-qemu-irqs.sh:[IRQ ] IF enabled; entering HLT event loop.' \
    'scripts/test-qemu-integrated-appliance-boot-acceptance.sh:[IRQ ] IF enabled; entering HLT event loop.' \
    'scripts/test-qemu-ioapic-irq0.sh:[PIC ] Master mask: 0x00000000000000FF; slave mask: 0x00000000000000FF' \
    'scripts/test-qemu-ioapic-irq1.sh:[PIC ] Master mask: 0x00000000000000FF; slave mask: 0x00000000000000FF' \
    'scripts/test-qemu-ioapic-irq3-7.sh:[PIC ] disabled; APIC runtime active.' \
    'scripts/test-qemu-ioapic-irq8-15.sh:[PIC ] disabled; APIC runtime active.' \
    'scripts/test-qemu-lapic.sh:[LAPIC] SVR configured: vector=239; legacy PIC retained' \
    'scripts/test-qemu-lapic-timer-oneshot.sh:[LAPIC-TIMER] oneshot armed: vector=0x43'; do
    item528_proof=${item528_record%%:*}
    item528_marker=${item528_record#*:}
    grep -Fq -- "$item528_marker" "$item528_proof" || {
        echo "ITEM-528: $item528_proof больше не требует запись:" \
            "$item528_marker" >&2
        exit 1
    }
done

# 3. The kernel still writes the periodic lines the rule covers, and the
#    witness still derives them from the kernel source rather than trusting a
#    copied list.
for item528_periodic in \
    '[IOAPIC] irq0 vector=0x40' \
    '[IOAPIC] irq0 lapic_eoi' \
    '[IOAPIC] irq1 vector=0x41' \
    '[IOAPIC] irq1 lapic_eoi' \
    '[LAPIC-TIMER] irq vector=0x42' \
    '[LAPIC-TIMER] irq lapic_eoi'; do
    grep -Fq -- "$item528_periodic" kernel/src/arch/x86_64/interrupts.c || {
        echo "ITEM-528: ядро больше не пишет периодическую строку:" \
            "$item528_periodic" >&2
        exit 1
    }
done
item528_host=tests/serial-view-normalised-host.py
for item528_control in \
    'PERIODIC_PREFIX' \
    'RECORD_MARKERS' \
    'SPLIT_FIXTURES' \
    'the rule removes every periodic line the kernel writes' \
    'the rule keeps every record a proof requires' \
    'the kernel writes the periodic IRQ lines from interrupt context' \
    'the shared renderer restores the recorded split'; do
    grep -Fq -- "$item528_control" "$item528_host" || {
        echo "ITEM-528: свидетель потерял контроль:" \
            "$item528_control" >&2
        exit 1
    }
done

# 4. The witness is registered in the canonical host suite under the same
#    target the earlier normalised-view items use.
for item528_witness in \
    'test-serial-view-normalised-host:' \
    'python3 tests/serial-view-normalised-host.py' \
    '$(MAKE) test-serial-view-normalised-host'; do
    grep -Fq -- "$item528_witness" Makefile || {
        echo "ITEM-528: свидетель не зарегистрирован:" \
            "$item528_witness" >&2
        exit 1
    }
done

# 5. The runner document owns the narrowing and the reason for it.
for item528_doc in \
    'ITEM-528' \
    'periodic' \
    '[IRQ ] IF enabled'; do
    grep -Fq -- "$item528_doc" docs/ci-self-hosted-runner.md || {
        echo "docs/ci-self-hosted-runner.md не фиксирует ITEM-528:" \
            "$item528_doc" >&2
        exit 1
    }
done
