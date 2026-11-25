#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# IOAPIC — boot integration and read-only Stage 0 invariants.
grep -Fq '#include "ioapic.h"' kernel/src/kernel.c || {
    echo "kernel.c не включает ioapic.h" >&2
    exit 1
}

ioapic_init_line=$(grep -n 'ioapic_init(acpi_madt(), hhdm_request.response->offset)' kernel/src/kernel.c | head -n1 | cut -d: -f1 || true)
lapic_init_line=$(grep -n 'lapic_init(' kernel/src/kernel.c | head -n1 | cut -d: -f1 || true)
pic_init_line=$(grep -n 'pic_init();' kernel/src/kernel.c | head -n1 | cut -d: -f1 || true)
[[ -n "$ioapic_init_line" && -n "$lapic_init_line" && -n "$pic_init_line" ]] || {
    echo "kernel.c не содержит корректный вызов ioapic_init между lapic_init и pic_init" >&2
    exit 1
}
[[ "$ioapic_init_line" -gt "$lapic_init_line" ]] || {
    echo "ioapic_init должен вызываться после lapic_init" >&2
    exit 1
}
[[ "$ioapic_init_line" -lt "$pic_init_line" ]] || {
    echo "ioapic_init должен вызываться до pic_init" >&2
    exit 1
}

grep -Fq '[PIC ] IOAPIC pre: master=' kernel/src/kernel.c || {
    echo "kernel.c не содержит exact [PIC ] IOAPIC pre marker" >&2
    exit 1
}
grep -Fq '[PIC ] IOAPIC post: master=' kernel/src/kernel.c || {
    echo "kernel.c не содержит exact [PIC ] IOAPIC post marker" >&2
    exit 1
}
ioapic_pre_line=$(grep -nF '[PIC ] IOAPIC pre: master=' kernel/src/kernel.c | head -n1 | cut -d: -f1 || true)
ioapic_post_line=$(grep -nF '[PIC ] IOAPIC post: master=' kernel/src/kernel.c | head -n1 | cut -d: -f1 || true)
[[ -n "$ioapic_pre_line" && -n "$ioapic_post_line" ]] || {
    echo "kernel.c не содержит оба IOAPIC PIC mask markers" >&2
    exit 1
}
[[ "$ioapic_pre_line" -lt "$ioapic_init_line" &&
   "$ioapic_init_line" -lt "$ioapic_post_line" &&
   "$ioapic_post_line" -lt "$pic_init_line" ]] || {
    echo "IOAPIC PIC mask markers должны окружать вызов ioapic_init до pic_init" >&2
    exit 1
}
if sed -n "${ioapic_pre_line},${ioapic_post_line}p" kernel/src/kernel.c | grep -nE '\bpic_mask\(|\bpic_unmask\(|\bpic_init\('; then
    echo "IOAPIC PIC mask diagnostic выполняет PIC writes/init" >&2
    exit 1
fi

grep -Fq '\[PIC \] IOAPIC pre: master=' scripts/test-qemu-ioapic-probe.sh || {
    echo "test-qemu-ioapic-probe.sh не валидирует pre marker" >&2
    exit 1
}
grep -Fq '\[PIC \] IOAPIC post: master=' scripts/test-qemu-ioapic-probe.sh || {
    echo "test-qemu-ioapic-probe.sh не валидирует post marker" >&2
    exit 1
}
grep -Fq 'pre_master != post_master' scripts/test-qemu-ioapic-probe.sh || {
    echo "test-qemu-ioapic-probe.sh не сравнивает pre/post masks" >&2
    exit 1
}
grep -Fq 'PIC masks unchanged across IOAPIC probe: confirmed' scripts/test-qemu-ioapic-probe.sh || {
    echo "test-qemu-ioapic-probe.sh не подтверждает неизменность PIC masks" >&2
    exit 1
}

grep -Fq 'enum ioapic_status ioapic_route_irq(' kernel/src/ioapic.c || {
    echo "ioapic.c не определяет ioapic_route_irq" >&2
    exit 1
}
grep -Fq 'enum ioapic_status ioapic_route_irq(' kernel/include/ioapic.h || {
    echo "Отсутствует зарезервированное объявление ioapic_route_irq" >&2
    exit 1
}
grep -Fq '#include "lapic.h"' kernel/src/ioapic.c || {
    echo "ioapic.c не включает lapic.h" >&2
    exit 1
}
grep -Fq 'ISA IRQ3–7 use verified IOAPIC vectors 0x44–0x48; PIC IRQ3–7 lines are masked only after route publication and remain fallback otherwise.' \
    docs/ARCHITECTURE.md || {
    echo "ARCHITECTURE.md не фиксирует enabled ISA IRQ3-7 routing" >&2
    exit 1
}
grep -Fq 'enum ioapic_status ioapic_route_isa_irq(' \
    kernel/include/ioapic.h || {
    echo "ioapic.h не объявляет ioapic_route_isa_irq" >&2
    exit 1
}
grep -Fq 'bool ioapic_is_isa_irq_routed(uint8_t irq);' \
    kernel/include/ioapic.h || {
    echo "ioapic.h не объявляет ioapic_is_isa_irq_routed" >&2
    exit 1
}
grep -Fq '#define IOAPIC_ISA_IRQ_COUNT 16U' \
    kernel/include/ioapic.h || {
    echo "ioapic.h не фиксирует IOAPIC_ISA_IRQ_COUNT" >&2
    exit 1
}
grep -Fq '0x41U + irq' kernel/src/ioapic.c || {
    echo "ioapic.c не использует fixed IRQ3-15 vector mapping" >&2
    exit 1
}
grep -Fq 'isa_irq3_7_route_all();' kernel/src/kernel.c || {
    echo "kernel.c не вызывает isa_irq3_7_route_all" >&2
    exit 1
}
grep -Fq 'if (!lapic_is_enabled()) {' \
    kernel/src/isa_irq3_7.c || {
    echo "isa_irq3_7.c не gate-ит route policy на LAPIC" >&2
    exit 1
}
grep -Fq 'frame->vector >= 0x44U && frame->vector <= 0x48U' \
    kernel/src/arch/x86_64/interrupts.c || {
    echo "interrupts.c не диспатчит vectors 0x44-0x48" >&2
    exit 1
}
grep -Fq 'static const uint8_t irq3_7[] = {' \
    kernel/src/isa_irq3_7.c || {
    echo "isa_irq3_7.c не определяет route policy array" >&2
    exit 1
}
grep -A2 -F 'static const uint8_t irq3_7[] = {' \
    kernel/src/isa_irq3_7.c | grep -Fq '3U, 4U, 5U, 6U, 7U' || {
    echo "isa_irq3_7.c route policy содержит не только IRQ3-7" >&2
    exit 1
}
for isa_irq in 8 9 10 11 12 13 14 15; do
    if grep -Fq "ioapic_route_isa_irq(${isa_irq}U" \
        kernel/src/kernel.c kernel/src/arch/x86_64/interrupts.c; then
        echo "kernel содержит IRQ${isa_irq} IOAPIC route caller" >&2
        exit 1
    fi
done
grep -Fq 'ISA IRQ8–15 use verified IOAPIC vectors 0x49–0x50; PIC IRQ8–15 lines are masked only after route publication and remain fallback otherwise.' \
    docs/ARCHITECTURE.md || {
    echo "ARCHITECTURE.md не фиксирует enabled ISA IRQ8-15 routing" >&2
    exit 1
}
grep -Fq 'isa_irq8_15_route_all();' kernel/src/kernel.c || {
    echo "kernel.c не вызывает isa_irq8_15_route_all" >&2
    exit 1
}
grep -Fq 'frame->vector >= 0x49U && frame->vector <= 0x50U' \
    kernel/src/arch/x86_64/interrupts.c || {
    echo "interrupts.c не диспатчит vectors 0x49-0x50" >&2
    exit 1
}
grep -Fq 'static const uint8_t irq8_15[] = {' \
    kernel/src/isa_irq8_15.c || {
    echo "isa_irq8_15.c не определяет route policy array" >&2
    exit 1
}
grep -A2 -F 'static const uint8_t irq8_15[] = {' \
    kernel/src/isa_irq8_15.c | grep -Fq '8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U' || {
    echo "isa_irq8_15.c route policy содержит не только IRQ8-15" >&2
    exit 1
}
if grep -RInE 'ioapic_route_isa_irq\((1[6-9]U|[2-9][0-9]U)' \
    kernel/src --include='*.c'; then
    echo "kernel содержит IRQ16+ IOAPIC route caller" >&2
    exit 1
fi
if grep -nE '\bpic_' kernel/src/ioapic.c; then
    echo "ioapic.c ссылается на PIC API" >&2
    exit 1
fi
grep -Fq 'PIC IRQ2 is permanently reserved as the 8259 cascade line; direct pic_unmask(2U) is rejected and no IRQ2 endpoint exists.' \
    docs/ARCHITECTURE.md || {
    echo "ARCHITECTURE.md не фиксирует permanent PIC IRQ2 policy" >&2
    exit 1
}
grep -A 2 -F 'bool pic_unmask(uint8_t irq) {' kernel/src/arch/x86_64/pic.c | \
    grep -Fqx '    if (irq == 2U) {' || {
    echo "pic_unmask не отклоняет PIC IRQ2" >&2
    exit 1
}
grep -A 3 -F 'bool pic_unmask(uint8_t irq) {' kernel/src/arch/x86_64/pic.c | \
    grep -Fqx '        return false;' || {
    echo "pic_unmask не возвращает false для PIC IRQ2" >&2
    exit 1
}
if grep -RInE 'ioapic_route_isa_irq\(2U|pic_unmask\(2U' \
    kernel/src --include='*.c'; then
    echo "kernel содержит production IRQ2 endpoint caller" >&2
    exit 1
fi
if grep -nE 'case[[:space:]]+0x22U:|irq2_|set_idt_gate\(0x22U' \
    kernel/src/arch/x86_64/interrupts.c kernel/src/kernel.c; then
    echo "kernel содержит IRQ2 IDT endpoint" >&2
    exit 1
fi
if grep -nE 'test-(qemu-)?pic-irq2|pic-irq2' Makefile .github/workflows/ci.yml; then
    echo "build содержит IRQ2 QEMU endpoint target" >&2
    exit 1
fi
if grep -nE '\bpic_(mask|unmask|send_eoi)\b|\bpic_init[[:space:]]*\(' kernel/src/ioapic.c; then
    echo "ioapic.c содержит PIC calls" >&2
    exit 1
fi
if grep -nE 'map_lapic_2m|cr3|pml4|pdpt|\bpd\[|PS_BIT|0x83ULL|0x000FFFFFFFFFF000ULL' kernel/src/ioapic.c; then
    echo "IOAPIC manual page-table mapper detected" >&2
    exit 1
fi

nm -g build/hfoc-kernel.elf | grep -F ' ioapic_init' >/dev/null || {
    echo "В ELF отсутствует ioapic_init" >&2
    exit 1
}
nm -g build/hfoc-kernel.elf | grep -F ' ioapic_is_irq1_routed' >/dev/null || {
    echo "В ELF отсутствует ioapic_is_irq1_routed" >&2
    exit 1
}
nm -g build/hfoc-kernel.elf | grep -F ' ioapic_is_irq0_routed' >/dev/null || {
    echo "В ELF отсутствует ioapic_is_irq0_routed" >&2
    exit 1
}
nm -g build/hfoc-kernel.elf | grep -F ' ioapic_route_irq' >/dev/null || {
    echo "В ELF отсутствует ioapic_route_irq" >&2
    exit 1
}
if nm -g build/hfoc-kernel.elf | grep -E 'ioapic_test_|lapic_test_|keyboard_test_|dispatch_test_'; then
    echo "test-only symbols попали в kernel ELF" >&2
    exit 1
fi
if sed -n '/^enum ioapic_status ioapic_init(/,/^}/p' kernel/src/ioapic.c |
    grep -nE 'ioapic_write32[[:space:]]*\([[:space:]]*0x10'; then
    echo "IOAPIC probe writes IOWIN" >&2
    exit 1
fi

grep -Fq 'test-ioapic-host' Makefile || {
    echo "Отсутствует test-ioapic-host в Makefile" >&2; exit 1; }
grep -Fq 'test-ioapic' Makefile || {
    echo "Отсутствует test-ioapic в Makefile" >&2; exit 1; }
bash -n scripts/test-qemu-ioapic-probe.sh || {
    echo "Синтаксис test-qemu-ioapic-probe.sh некорректен" >&2; exit 1; }
grep -A2 '^test-ioapic:' Makefile | grep -Fq "QEMU_MACHINE='q35'" || {
    echo "test-ioapic должен использовать QEMU_MACHINE='q35'" >&2; exit 1; }
if grep -A2 '^test-ioapic:' Makefile | grep -Fq 'QEMU_CPU='; then
    echo "test-ioapic должен не задавать QEMU_CPU" >&2; exit 1
fi
grep -A2 '^test-ioapic:' Makefile | grep -Fq "QEMU_SMP='1'" || {
    echo "test-ioapic должен использовать QEMU_SMP='1'" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Eq '\$\(MAKE\) test-ioapic-host$' Makefile || {
    echo "test: target не содержит test-ioapic-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Eq '\$\(MAKE\) test-ioapic$' Makefile || {
    echo "test: target не содержит test-ioapic" >&2; exit 1; }
if grep -nE 'sendkey' scripts/test-qemu-ioapic-probe.sh; then
    echo "test-qemu-ioapic-probe.sh содержит HMP sendkey" >&2
    exit 1
fi

# IOAPIC Stage 1 IRQ1 boot/dispatcher invariants.
grep -Fq 'test-keyboard-host' Makefile || {
    echo "Отсутствует test-keyboard-host в Makefile" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-keyboard-host' Makefile || {
    echo "test: target не содержит test-keyboard-host" >&2; exit 1; }

# Keyboard line editing Task 4: QEMU proof and strict editing contract.
grep -Fq 'test-keyboard-line-editing' Makefile || {
    echo "Отсутствует test-keyboard-line-editing в Makefile" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-keyboard-line-editing' Makefile || {
    echo "test: target не содержит test-keyboard-line-editing" >&2; exit 1; }
grep -A2 '^test-keyboard-line-editing:' Makefile |
    grep -Fq "QEMU_MACHINE='q35'" || {
    echo "test-keyboard-line-editing должен использовать QEMU_MACHINE='q35'" >&2
    exit 1
}
if grep -A2 '^test-keyboard-line-editing:' Makefile |
    grep -Fq 'QEMU_CPU='; then
    echo "test-keyboard-line-editing должен не задавать QEMU_CPU" >&2
    exit 1
fi
grep -A2 '^test-keyboard-line-editing:' Makefile |
    grep -Fq "QEMU_SMP='1'" || {
    echo "test-keyboard-line-editing должен использовать QEMU_SMP='1'" >&2
    exit 1
}
bash -n scripts/test-qemu-keyboard-line-editing.sh || {
    echo "Синтаксис test-qemu-keyboard-line-editing.sh некорректен" >&2
    exit 1
}
for strict_symbol in mktemp timeout send_monitor_quit successful_shutdown \
                     best_effort_cleanup cleanup no-reboot no-shutdown; do
    grep -Fq "$strict_symbol" scripts/test-qemu-keyboard-line-editing.sh || {
        echo "test-qemu-keyboard-line-editing.sh не содержит $strict_symbol" >&2
        exit 1
    }
done
grep -Fq 'QEMU APIC baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-keyboard-line-editing.sh || {
    echo "test-qemu-keyboard-line-editing.sh не фиксирует baseline" >&2
    exit 1
}
for sendkey in 'sendkey a' 'sendkey c' 'sendkey left' 'sendkey b' \
               'sendkey end' 'sendkey backspace' 'sendkey ret'; do
    grep -Fq "$sendkey" scripts/test-qemu-keyboard-line-editing.sh || {
        echo "test-qemu-keyboard-line-editing.sh не содержит $sendkey" >&2
        exit 1
    }
done
grep -Fq 'UNKNOWN COMMAND: abc. TYPE HELP.' \
    scripts/test-qemu-keyboard-line-editing.sh || {
    echo "test-qemu-keyboard-line-editing.sh не проверяет final abc command" >&2
    exit 1
}
grep -Fq 'Servicing hardware INT=0x41' \
    scripts/test-qemu-keyboard-line-editing.sh || {
    echo "test-qemu-keyboard-line-editing.sh не требует INT=0x41" >&2
    exit 1
}
grep -Fq 'Servicing hardware INT=0x21' \
    scripts/test-qemu-keyboard-line-editing.sh || {
    echo "test-qemu-keyboard-line-editing.sh не запрещает INT=0x21" >&2
    exit 1
}
grep -Fq 'keyboard-line-editing-' \
    scripts/test-qemu-keyboard-line-editing.sh || {
    echo "test-qemu-keyboard-line-editing.sh не сохраняет артефакты" >&2
    exit 1
}

# Keyboard line editing Task 5: documentation contract.
for line_editing_doc in README.md docs/ARCHITECTURE.md; do
    grep -Fq 'PS/2 Set 1 decoder and console line editing' \
        "$line_editing_doc" || {
        echo "$line_editing_doc не документирует PS/2 line editing" >&2
        exit 1
    }
    # shellcheck disable=SC2016
    grep -Fq 'IRQ1 vector `0x41` ownership unchanged' \
        "$line_editing_doc" || {
        echo "$line_editing_doc не фиксирует неизменный IRQ1 ownership" >&2
        exit 1
    }
    grep -Fq 'No persistence, completion/search, ANSI, UTF-8, keypad, or heap' \
        "$line_editing_doc" || {
        echo "$line_editing_doc не фиксирует line editing boundaries" >&2
        exit 1
    }
done
grep -Fq 'line editing консоли' docs/ROADMAP.md || {
    echo "ROADMAP.md не отмечает line editing консоли" >&2
    exit 1
}

# Console history Task 4: QEMU proof and strict history contract.
grep -Fq 'test-console-history' Makefile || {
    echo "Отсутствует test-console-history в Makefile" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-console-history' Makefile || {
    echo "test: target не содержит test-console-history" >&2; exit 1; }
grep -A2 '^test-console-history:' Makefile |
    grep -Fq "QEMU_MACHINE='q35'" || {
    echo "test-console-history должен использовать QEMU_MACHINE='q35'" >&2
    exit 1
}
if grep -A2 '^test-console-history:' Makefile |
    grep -Fq 'QEMU_CPU='; then
    echo "test-console-history должен не задавать QEMU_CPU" >&2
    exit 1
fi
grep -A2 '^test-console-history:' Makefile |
    grep -Fq "QEMU_SMP='1'" || {
    echo "test-console-history должен использовать QEMU_SMP='1'" >&2
    exit 1
}
bash -n scripts/test-qemu-console-history.sh || {
    echo "Синтаксис test-qemu-console-history.sh некорректен" >&2
    exit 1
}
for strict_symbol in mktemp timeout send_monitor_quit successful_shutdown \
                     best_effort_cleanup cleanup no-reboot no-shutdown; do
    grep -Fq "$strict_symbol" scripts/test-qemu-console-history.sh || {
        echo "test-qemu-console-history.sh не содержит $strict_symbol" >&2
        exit 1
    }
done
grep -Fq 'QEMU APIC baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-console-history.sh || {
    echo "test-qemu-console-history.sh не фиксирует baseline" >&2
    exit 1
}
for history_key in 'sendkey ' '"sendkey " + key' 'send_key("up")' \
                   'send_key("down")' 'send_key("ret")' \
                   'type_text("help")' 'type_text("version")' \
                   'type_text("draft")'; do
    grep -Fq "$history_key" scripts/test-qemu-console-history.sh || {
        echo "test-qemu-console-history.sh не содержит $history_key" >&2
        exit 1
    }
done
grep -Fq 'Command result counts: help=1 version=2' \
    scripts/test-qemu-console-history.sh || {
    echo "test-qemu-console-history.sh не проверяет command counts" >&2
    exit 1
}
grep -Fq 'Draft restored before final Up: yes' \
    scripts/test-qemu-console-history.sh || {
    echo "test-qemu-console-history.sh не проверяет draft restore" >&2
    exit 1
}
grep -Fq 'UNKNOWN COMMAND: draft. TYPE HELP.' \
    scripts/test-qemu-console-history.sh || {
    echo "test-qemu-console-history.sh не проверяет отсутствие draft submit" >&2
    exit 1
}
grep -Fq 'Servicing hardware INT=0x41' \
    scripts/test-qemu-console-history.sh || {
    echo "test-qemu-console-history.sh не требует INT=0x41" >&2
    exit 1
}
grep -Fq 'Servicing hardware INT=0x21' \
    scripts/test-qemu-console-history.sh || {
    echo "test-qemu-console-history.sh не запрещает INT=0x21" >&2
    exit 1
}
grep -Fq 'console-history-' \
    scripts/test-qemu-console-history.sh || {
    echo "test-qemu-console-history.sh не сохраняет артефакты" >&2
    exit 1
}

# Console history Task 5: documentation contract.
for history_doc in README.md docs/ARCHITECTURE.md; do
    grep -Fq 'fixed 16-entry command history' "$history_doc" || {
        echo "$history_doc не документирует command history" >&2
        exit 1
    }
    grep -Fq 'non-empty newest-only deduplication' "$history_doc" || {
        echo "$history_doc не фиксирует newest-only deduplication" >&2
        exit 1
    }
    grep -Fq 'exact draft restore' "$history_doc" || {
        echo "$history_doc не фиксирует draft restore" >&2
        exit 1
    }
    grep -Fq 'No persistence, completion/search, ANSI, UTF-8, keypad, or heap' \
        "$history_doc" || {
        echo "$history_doc не фиксирует history boundaries" >&2
        exit 1
    }
done
grep -Fq 'fixed 16-entry ring' docs/ROADMAP.md || {
    echo "ROADMAP.md не отмечает fixed command history" >&2
    exit 1
}

grep -Fq 'ioapic_route_irq(1U, IOAPIC_IRQ1_VECTOR);' kernel/src/kernel.c || {
    echo "kernel.c не вызывает ioapic_route_irq для IRQ1" >&2
    exit 1
}
route_line=$(grep -n 'ioapic_route_irq(1U, IOAPIC_IRQ1_VECTOR);' kernel/src/kernel.c | head -n1 | cut -d: -f1 || true)
keyboard_init_line=$(grep -n 'keyboard_init(' kernel/src/kernel.c | head -n1 | cut -d: -f1 || true)
[[ -n "$route_line" && -n "$keyboard_init_line" &&
   "$route_line" -gt "$pic_init_line" &&
   "$route_line" -lt "$keyboard_init_line" ]] || {
    echo "ioapic_route_irq должен идти после pic_init и до keyboard_init" >&2
    exit 1
}
grep -Fq '#include "lapic.h"' kernel/src/arch/x86_64/interrupts.c || {
    echo "interrupts.c не включает lapic.h" >&2
    exit 1
}
grep -Fq '[IOAPIC] irq1 vector=0x41' kernel/src/arch/x86_64/interrupts.c || {
    echo "interrupts.c не содержит vector 0x41 entry marker" >&2
    exit 1
}
grep -Fq '[IOAPIC] irq1 lapic_eoi' kernel/src/arch/x86_64/interrupts.c || {
    echo "interrupts.c не содержит LAPIC EOI marker" >&2
    exit 1
}
grep -Fq '[IOAPIC] route: irq=1 vector=0x41 gsi=' kernel/src/ioapic.c || {
    echo "ioapic.c не содержит route success marker" >&2
    exit 1
}
grep -Fq '[IOAPIC] route failed: irq=' kernel/src/ioapic.c || {
    echo "ioapic.c не содержит generic route failure marker" >&2
    exit 1
}
if sed -n '/if (frame->vector == 0x41U)/,/^    }/p' kernel/src/arch/x86_64/interrupts.c |
    grep -nE '\bpic_(mask|unmask|send_eoi)\b|\bpic_init[[:space:]]*\('; then
    echo "vector 0x41 branch вызывает PIC API" >&2
    exit 1
fi
if ! sed -n '/if (frame->vector == 33U)/,/^    }/p' kernel/src/arch/x86_64/interrupts.c |
    grep -Fq 'pic_send_eoi(1);'; then
    echo "vector 0x21 branch не вызывает pic_send_eoi(1)" >&2
    exit 1
fi
if sed -n '/if (frame->vector == 33U)/,/^    }/p' kernel/src/arch/x86_64/interrupts.c |
    grep -Fq 'lapic_eoi'; then
    echo "vector 0x21 branch вызывает LAPIC EOI" >&2
    exit 1
fi

# IOAPIC Stage 2 IRQ0 dispatcher invariants.
grep -Fq '[IOAPIC] irq0 vector=0x40' kernel/src/arch/x86_64/interrupts.c || {
    echo "interrupts.c не содержит vector 0x40 entry marker" >&2
    exit 1
}
grep -Fq '[IOAPIC] irq0 lapic_eoi' kernel/src/arch/x86_64/interrupts.c || {
    echo "interrupts.c не содержит IRQ0 LAPIC EOI marker" >&2
    exit 1
}

# Extract one brace-balanced block from a source file. The target line may
# open the block, or the opening brace may follow on the next line.
extract_balanced_block() {
    awk -v needle="$1" '
        index($0, needle) > 0 { found = 1 }
        found {
            print
            opens = gsub(/{/, "{")
            closes = gsub(/}/, "}")
            if (saw_open) {
                depth += opens - closes
                if (depth <= 0) exit
            } else if (opens > 0) {
                saw_open = 1
                depth = opens - closes
                if (depth <= 0) exit
            }
        }
    ' "$2"
}

extract_vector_branch() {
    extract_balanced_block "$1" kernel/src/arch/x86_64/interrupts.c
}

extract_balanced_from_text() {
    printf '%s\n' "$2" | awk -v needle="$1" '
        index($0, needle) > 0 { found = 1 }
        found {
            if (saw_open && depth == 1 && index($0, "} else") > 0) exit
            print
            opens = gsub(/{/, "{")
            closes = gsub(/}/, "}")
            if (saw_open) {
                depth += opens - closes
                if (depth <= 0) exit
            } else if (opens > 0) {
                saw_open = 1
                depth = opens - closes
                if (depth <= 0) exit
            }
        }
    '
}

extract_ownership_if_block() {
    awk '
        index($0, "if (irq0_ioapic_active)") > 0 { found = 1; depth = 1; print; next }
        found {
            if (index($0, "} else {") > 0) exit
            print
            opens = gsub(/{/, "{")
            closes = gsub(/}/, "}")
            depth += opens - closes
            if (depth <= 0) exit
        }
    '
}

extract_else_block() {
    awk '
        index($0, "} else {") > 0 { found = 1; depth = 1; print; next }
        found {
            print
            opens = gsub(/{/, "{")
            closes = gsub(/}/, "}")
            depth += opens - closes
            if (depth <= 0) exit
        }
    '
}

vector20_branch=$(extract_vector_branch 'if (frame->vector == 32U)')
vector40_branch=$(extract_vector_branch 'if (frame->vector == 0x40U)')
vector42_branch=$(extract_vector_branch 'if (frame->vector == 0x42U)')

runtime20_block=$(extract_balanced_from_text \
    'if (lapic_timer_runtime_active())' "$vector20_branch")
stray20_block=$(printf '%s\n' "$vector20_branch" | extract_ownership_if_block)
fallback20_block=$(printf '%s\n' "$vector20_branch" | extract_else_block)
runtime40_block=$(extract_balanced_from_text \
    'if (!lapic_timer_runtime_active() && irq0_ioapic_active)' \
    "$vector40_branch")
runtime42_block=$(extract_balanced_from_text \
    'if (lapic_timer_runtime_active())' "$vector42_branch")

if printf '%s\n' "$runtime20_block" | grep -Fq 'timer_handle_irq();'; then
    echo "runtime active 0x20 branch вызывает timer_handle_irq()" >&2
    exit 1
fi
if ! printf '%s\n' "$runtime20_block" | grep -Fq 'pic_send_eoi(0);'; then
    echo "runtime active 0x20 branch не вызывает pic_send_eoi(0)" >&2
    exit 1
fi
if printf '%s\n' "$stray20_block" | grep -Fq 'timer_handle_irq();'; then
    echo "active stray 0x20 branch вызывает timer_handle_irq()" >&2
    exit 1
fi
if ! printf '%s\n' "$stray20_block" | grep -Fq 'pic_send_eoi(0);'; then
    echo "active stray 0x20 branch не вызывает pic_send_eoi(0)" >&2
    exit 1
fi
if ! printf '%s\n' "$stray20_block" | grep -Fq 'irq0_stray_pic_count++;'; then
    echo "active stray 0x20 branch не увеличивает irq0_stray_pic_count" >&2
    exit 1
fi
if printf '%s\n' "$vector20_branch" | grep -Fq 'lapic_eoi'; then
    echo "vector 0x20 branch вызывает lapic_eoi()" >&2
    exit 1
fi
if ! printf '%s\n' "$fallback20_block" | grep -Fq 'timer_handle_irq();'; then
    echo "fallback 0x20 else branch не вызывает timer_handle_irq()" >&2
    exit 1
fi
if ! printf '%s\n' "$fallback20_block" | \
    grep -Fq 'lapic_timer_runtime_coordinate_irq0();'; then
    echo "fallback 0x20 else branch не вызывает runtime coordinator" >&2
    exit 1
fi
if ! printf '%s\n' "$fallback20_block" | grep -Fq 'pic_send_eoi(0);'; then
    echo "fallback 0x20 else branch не вызывает pic_send_eoi(0)" >&2
    exit 1
fi

if ! printf '%s\n' "$runtime40_block" | grep -Fq 'timer_handle_irq();'; then
    echo "non-runtime active 0x40 branch не вызывает timer_handle_irq()" >&2
    exit 1
fi
if ! printf '%s\n' "$runtime40_block" | \
    grep -Fq 'lapic_timer_runtime_coordinate_irq0();'; then
    echo "non-runtime active 0x40 branch не вызывает runtime coordinator" >&2
    exit 1
fi
if ! printf '%s\n' "$vector40_branch" | grep -Fq 'lapic_eoi();'; then
    echo "vector 0x40 branch не вызывает lapic_eoi()" >&2
    exit 1
fi
timer_total=$(printf '%s\n' "$vector40_branch" |
    grep -F -o 'timer_handle_irq();' | wc -l)
timer_runtime40=$(printf '%s\n' "$runtime40_block" |
    grep -F -o 'timer_handle_irq();' | wc -l)
if [[ "$timer_total" -ne "$timer_runtime40" ]]; then
    echo "timer_handle_irq() в vector 0x40 находится вне verified block" >&2
    exit 1
fi
if printf '%s\n' "$vector40_branch" |
    grep -nE '\bpic_(mask|unmask|send_eoi)\b|\bpic_init[[:space:]]*\('; then
    echo "vector 0x40 branch вызывает PIC API" >&2
    exit 1
fi

if ! grep -Fq 'timer_handle_irq();' <<<"$runtime42_block"; then
    echo "runtime 0x42 branch не вызывает timer_handle_irq()" >&2
    exit 1
fi
if ! grep -Fq 'lapic_eoi();' <<<"$runtime42_block"; then
    echo "runtime 0x42 branch не вызывает lapic_eoi()" >&2
    exit 1
fi
if printf '%s\n' "$vector42_branch" |
    grep -nE '\bpic_(mask|unmask|send_eoi)\b|\bpic_init[[:space:]]*\('; then
    echo "vector 0x42 branch вызывает PIC API" >&2
    exit 1
fi
if grep -Fq 'ioapic_is_irq0_routed' kernel/src/arch/x86_64/interrupts.c; then
    echo "interrupts.c перечитывает ioapic_is_irq0_routed()" >&2
    exit 1
fi

# Runtime LAPIC timer Task 6: strict QEMU proof and coordinator order.
grep -Fq 'test-lapic-timer-runtime-host' Makefile || {
    echo "Отсутствует test-lapic-timer-runtime-host в Makefile" >&2; exit 1; }
grep -Fq 'test-lapic-timer-runtime' Makefile || {
    echo "Отсутствует test-lapic-timer-runtime в Makefile" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-lapic-timer-runtime-host' Makefile || {
    echo "test: target не содержит test-lapic-timer-runtime-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-lapic-timer-runtime' Makefile || {
    echo "test: target не содержит test-lapic-timer-runtime" >&2; exit 1; }
grep -Fq 'test-pic-host' Makefile || {
    echo "Отсутствует test-pic-host в Makefile" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-pic-host' Makefile || {
    echo "test: target не содержит test-pic-host" >&2; exit 1; }
runtime_target_block=$(sed -n \
    '/^test-lapic-timer-runtime:/,/^[^[:space:]]/p' Makefile)
grep -Fq "QEMU_MACHINE='q35'" <<<"$runtime_target_block" || {
    echo "test-lapic-timer-runtime должен использовать QEMU_MACHINE='q35'" >&2; exit 1; }
if grep -Fq 'QEMU_CPU=' <<<"$runtime_target_block"; then
    echo "test-lapic-timer-runtime должен не задавать QEMU_CPU" >&2
    exit 1
fi
grep -Fq "QEMU_SMP='1'" <<<"$runtime_target_block" || {
    echo "test-lapic-timer-runtime должен использовать QEMU_SMP='1'" >&2; exit 1; }
if [[ "$(grep -c '^test-lapic-timer-runtime:' Makefile || true)" -ne 1 ]]; then
    echo "Ожидался ровно один test-lapic-timer-runtime target" >&2
    exit 1
fi
bash -n scripts/test-qemu-lapic-timer-runtime.sh || {
    echo "Синтаксис scripts/test-qemu-lapic-timer-runtime.sh некорректен" >&2; exit 1; }
for runtime_strict_symbol in mktemp timeout send_monitor_quit successful_shutdown \
                            best_effort_cleanup cleanup no-reboot no-shutdown; do
    grep -Fq "$runtime_strict_symbol" scripts/test-qemu-lapic-timer-runtime.sh || {
        echo "test-qemu-lapic-timer-runtime.sh не содержит $runtime_strict_symbol" >&2
        exit 1
    }
done
grep -Fq 'runtime_re = re.compile' scripts/test-qemu-lapic-timer-runtime.sh || {
    echo "test-qemu-lapic-timer-runtime.sh не валидирует runtime diagnostic" >&2; exit 1; }
grep -Fq 'Missing in suffix: Servicing hardware INT=0x42' \
    scripts/test-qemu-lapic-timer-runtime.sh || {
    echo "test-qemu-lapic-timer-runtime.sh не требует INT=0x42" >&2; exit 1; }
grep -Fq 'Post-transition suffix contains INT=0x40' \
    scripts/test-qemu-lapic-timer-runtime.sh || {
    echo "test-qemu-lapic-timer-runtime.sh не отклоняет post-transition INT=0x40" >&2
    exit 1
}
grep -Fq 'TICKS after > before: yes' \
    scripts/test-qemu-lapic-timer-runtime.sh || {
    echo "test-qemu-lapic-timer-runtime.sh не подтверждает рост ticks" >&2; exit 1; }
grep -Fq 'QEMU LAPIC timer runtime test: OK' \
    scripts/test-qemu-lapic-timer-runtime.sh || {
    echo "test-qemu-lapic-timer-runtime.sh не содержит success marker" >&2; exit 1; }
for runtime_host_contract in \
    'timer prepare ioapic-mask activate publish lapic_eoi' \
    'timer prepare pic-mask activate publish pic_eoi' \
    'timer prepare abort lapic_eoi' \
    'timer prepare ioapic-mask abort lapic_eoi' \
    'timer prepare pic-mask abort pic_eoi' \
    'timer prepare ioapic-mask activate abort ioapic-restore lapic_eoi' \
    'timer prepare pic-mask activate abort pic-restore pic_eoi'; do
    grep -Fq "$runtime_host_contract" tests/lapic-timer-runtime-host.c || {
        echo "tests/lapic-timer-runtime-host.c не содержит $runtime_host_contract" >&2
        exit 1
    }
done

coordinator_block=$(extract_balanced_block \
    'static void lapic_timer_runtime_coordinate_irq0(void)' \
    kernel/src/arch/x86_64/interrupts.c)
prepare_line=$(printf '%s\n' "$coordinator_block" |
    grep -n 'lapic_timer_runtime_prepare()' | head -n1 | cut -d: -f1 || true)
mask_line=$(printf '%s\n' "$coordinator_block" |
    grep -n 'runtime_mask_active_source()' | head -n1 | cut -d: -f1 || true)
activate_line=$(printf '%s\n' "$coordinator_block" |
    grep -n 'lapic_timer_runtime_activate()' | head -n1 | cut -d: -f1 || true)
[[ -n "$prepare_line" && -n "$mask_line" && -n "$activate_line" &&
   "$prepare_line" -lt "$mask_line" && "$mask_line" -lt "$activate_line" ]] || {
    echo "Runtime coordinator нарушает prepare -> source-mask -> activate" >&2
    exit 1
}
abort_count=$(printf '%s\n' "$coordinator_block" |
    grep -F -o 'lapic_timer_runtime_abort();' | wc -l)
restore_count=$(printf '%s\n' "$coordinator_block" |
    grep -F -o 'runtime_restore_source_masks();' | wc -l)
[[ "$abort_count" -ge 3 && "$restore_count" -ge 2 ]] || {
    echo "Runtime coordinator не вызывает abort/restore на всех failure branches" >&2
    exit 1
}
grep -Fq 'runtime_restore_source_masks();' kernel/src/arch/x86_64/interrupts.c || {
    echo "interrupts.c не содержит source-mask restore helpers" >&2; exit 1; }
grep -Fq 'ioapic_irq0_mask_save(&runtime_previous_rte)' \
    kernel/src/arch/x86_64/interrupts.c || {
    echo "interrupts.c не сохраняет IOAPIC IRQ0 RTE" >&2; exit 1; }
grep -Fq 'ioapic_irq0_restore(runtime_previous_rte)' \
    kernel/src/arch/x86_64/interrupts.c || {
    echo "interrupts.c не восстанавливает IOAPIC IRQ0 RTE" >&2; exit 1; }
grep -Fq 'pic_irq0_mask_save(&runtime_previous_pic_mask)' \
    kernel/src/arch/x86_64/interrupts.c || {
    echo "interrupts.c не использует transactional PIC IRQ0 mask save" >&2
    exit 1
}
grep -Fq 'pic_master_mask_restore(runtime_previous_pic_mask)' \
    kernel/src/arch/x86_64/interrupts.c || {
    echo "interrupts.c не использует exact PIC master mask restore" >&2
    exit 1
}
if grep -Fq 'pic_unmask(0U);' kernel/src/arch/x86_64/interrupts.c; then
    echo "interrupts.c использует pic_unmask(0U) как runtime rollback" >&2
    exit 1
fi

# Transactional PIC disable Task 4: API, coordinator, QEMU, and docs contract.
grep -Fq 'bool pic_disable_transactional(void);' \
    kernel/include/arch/x86_64/pic.h || {
    echo "pic.h не объявляет pic_disable_transactional" >&2; exit 1; }
grep -Fq 'pic_disable_after_apic_runtime' \
    kernel/src/arch/x86_64/interrupts.c || {
    echo "interrupts.c не содержит PIC disable coordinator" >&2; exit 1; }
grep -Fq 'ioapic_is_irq1_routed()' \
    kernel/src/arch/x86_64/interrupts.c || {
    echo "interrupts.c не проверяет verified IRQ1 route" >&2; exit 1; }
grep -Fq 'test-pic-disable' Makefile || {
    echo "Отсутствует test-pic-disable в Makefile" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-pic-disable' Makefile || {
    echo "test: target не содержит test-pic-disable" >&2; exit 1; }
grep -Eq 'pic-disable' .github/workflows/ci.yml || {
    echo "CI pgrep pattern не содержит test-qemu-pic-disable.sh" >&2; exit 1; }
grep -A2 '^test-pic-disable:' Makefile | grep -Fq "QEMU_MACHINE='q35'" || {
    echo "test-pic-disable должен использовать QEMU_MACHINE='q35'" >&2; exit 1; }
if grep -A2 '^test-pic-disable:' Makefile | grep -Fq 'QEMU_CPU='; then
    echo "test-pic-disable должен не задавать QEMU_CPU" >&2
    exit 1
fi
grep -A2 '^test-pic-disable:' Makefile | grep -Fq "QEMU_SMP='1'" || {
    echo "test-pic-disable должен использовать QEMU_SMP='1'" >&2; exit 1; }
bash -n scripts/test-qemu-pic-disable.sh || {
    echo "Синтаксис scripts/test-qemu-pic-disable.sh некорректен" >&2; exit 1; }
for pic_disable_strict_symbol in mktemp timeout send_monitor_quit \
                                 successful_shutdown best_effort_cleanup \
                                 cleanup no-reboot no-shutdown; do
    grep -Fq "$pic_disable_strict_symbol" \
        scripts/test-qemu-pic-disable.sh || {
        echo "test-qemu-pic-disable.sh не содержит $pic_disable_strict_symbol" >&2
        exit 1
    }
done
grep -Fq 'QEMU APIC baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-pic-disable.sh || {
    echo "test-qemu-pic-disable.sh не фиксирует baseline" >&2; exit 1; }
for pic_disable_proof in 'PIC disable diagnostic: found' \
                         'Command result count: help=1' \
                         'Trace suffix: INT=0x41 and INT=0x42 found; INT=0x20 and INT=0x21 absent' \
                         'QEMU PIC disable test: OK' \
                         'pic-disable-'; do
    grep -Fq "$pic_disable_proof" scripts/test-qemu-pic-disable.sh || {
        echo "test-qemu-pic-disable.sh не содержит $pic_disable_proof" >&2
        exit 1
    }
done
grep -Fq 'clean_serial_suffix' scripts/test-qemu-pic-disable.sh || {
    echo "test-qemu-pic-disable.sh не очищает diagnostic markers" >&2
    exit 1
}
grep -Fq '[PIC ] disabled; APIC runtime active.' \
    kernel/src/arch/x86_64/interrupts.c || {
    echo "interrupts.c не содержит PIC disable success marker" >&2; exit 1; }
grep -Fq '[PIC ] retained; PIC disable failed.' \
    kernel/src/arch/x86_64/interrupts.c || {
    echo "interrupts.c не содержит PIC disable failure marker" >&2; exit 1; }
pic_disable_call_count=$(grep -F -o \
    'pic_disable_after_apic_runtime();' \
    kernel/src/arch/x86_64/interrupts.c | wc -l)
[[ "$pic_disable_call_count" -eq 1 ]] || {
    echo "PIC disable coordinator вызывается $pic_disable_call_count раз(а), ожидался 1" >&2
    exit 1
}
disable_line=$(printf '%s\n' "$coordinator_block" |
    grep -n 'pic_disable_after_apic_runtime();' | head -n1 | cut -d: -f1 || true)
[[ -n "$disable_line" && "$activate_line" -lt "$disable_line" ]] || {
    echo "PIC disable coordinator вызывается после lapic_timer_runtime_activate" >&2
    exit 1
}
for pic_disable_doc in README.md docs/ARCHITECTURE.md; do
    grep -Fq 'PIC controllers to FF/FF' \
        "$pic_disable_doc" || {
        echo "$pic_disable_doc не документирует PIC disable success" >&2
        exit 1
    }
    grep -Fq 'fallback' \
        "$pic_disable_doc" || {
        echo "$pic_disable_doc не фиксирует fallback policy" >&2
        exit 1
    }
    grep -Fq 'logical mask disable, not IMCR/hardware removal' \
        "$pic_disable_doc" || {
        echo "$pic_disable_doc не фиксирует logical mask disable" >&2
        exit 1
    }
    grep -Fq 'ISA IRQ8–15 use verified IOAPIC vectors 0x49–0x50; PIC IRQ8–15 lines are masked only after route publication and remain fallback otherwise.' \
        "$pic_disable_doc" || {
        echo "$pic_disable_doc не фиксирует enabled ISA IRQ8-15 routing" >&2
        exit 1
    }
done
grep -Fq 'transactional legacy PIC disable' docs/ROADMAP.md || {
    echo "ROADMAP.md не отмечает transactional PIC disable" >&2
    exit 1
}
if nm -g build/hfoc-kernel.elf |
    awk '$3 ~ /^(pic_disable_host_test_|host_)/ { print; found=1 }
         END { exit !found }'; then
    echo "PIC disable host-test symbols попали в kernel ELF" >&2
    exit 1
fi

for harness_symbol in 'static unsigned timer_calls;' \
                      'run_irq1_eoi_case' \
                      'run_irq0_eoi_case' \
                      'run_irq0_sequence_case' \
                      'run_irq1_sequence_case' \
                      'run_irq1_repeat_case' \
                      'run_irq0_repeat_case' \
                      'run_spurious_eoi_case' \
                      'test_irq0_eoi_paths' \
                      'test_spurious_eoi_paths'; do
    grep -Fq "$harness_symbol" tests/pic-spurious-irqs.c || {
        echo "tests/pic-spurious-irqs.c не содержит $harness_symbol" >&2
        exit 1
    }
done
for task2_harness_symbol in 'ioapic_is_irq0_routed' \
                            'run_irq0_ownership_single_case' \
                            'run_irq0_stray_repeat_case' \
                            'run_irq0_active_to_fallback_case' \
                            'run_irq0_no_ioapic_reread_case' \
                            'test_irq0_ownership_dispatch_paths' \
                            'IRQ0 fallback 0x20 timer owner' \
                            'IRQ0 fallback 0x40 shadow' \
                            'IRQ0 active 0x40 timer owner' \
                            'IRQ0 active stray 0x20' \
                            'IRQ0 stray repeat counter' \
                            'IRQ0 active to fallback transition' \
                            'IRQ0 dispatcher no IOAPIC reread'; do
    grep -Fq "$task2_harness_symbol" tests/pic-spurious-irqs.c || {
        echo "tests/pic-spurious-irqs.c не содержит $task2_harness_symbol" >&2
        exit 1
    }
done
for task3_harness_symbol in 'run_exact_negative_case' \
                            'run_full_sequence_case' \
                            'run_full_repeat_case' \
                            'run_irq0_false_to_true_case' \
                            'run_irq0_reset_seam_case' \
                            'test_irq0_full_dispatch_matrix' \
                            'irq0_stray_pic_count_host_test_value() != expected_stray' \
                            'expect_only_pic_eois(name, expected_pic1_eoi)' \
                            'expect_only_pic_eois(name, expected_pic_eoi)' \
                            'IRQ0 fallback repeat 0x20' \
                            'IRQ0 active repeat 0x40' \
                            'IRQ0 active repeat stray 0x20' \
                            'IRQ0 success sequence 0x20 then 0x40' \
                            'IRQ0 success sequence 0x40 then 0x20' \
                            'IRQ0 fallback sequence 0x20 then 0x40' \
                            'IRQ0 fallback sequence 0x40 then 0x20' \
                            'IRQ0 false to true 0x40 owner' \
                            'IRQ0 reset seam no leakage' \
                            'IRQ0 negative fallback 0x20' \
                            'IRQ0 negative active stray 0x20' \
                            'IRQ0 negative fallback 0x40' \
                            'IRQ0 negative active 0x40' \
                            'IRQ1 negative 0x21 fallback' \
                            'IRQ1 negative 0x21 active' \
                            'IRQ1 negative 0x41 fallback' \
                            'IRQ1 negative 0x41 active' \
                            'IRQ spurious negative fallback' \
                            'IRQ spurious negative active'; do
    grep -Fq "$task3_harness_symbol" tests/pic-spurious-irqs.c || {
        echo "tests/pic-spurious-irqs.c не содержит $task3_harness_symbol" >&2
        exit 1
    }
done

# IOAPIC Stage 1 IRQ1 QEMU regression test, CI, and documentation.
grep -Fq 'test-ioapic-irq1' Makefile || {
    echo "Отсутствует test-ioapic-irq1 в Makefile" >&2; exit 1; }
bash -n scripts/test-qemu-ioapic-irq1.sh || {
    echo "Синтаксис test-qemu-ioapic-irq1.sh некорректен" >&2; exit 1; }
grep -A2 '^test-ioapic-irq1:' Makefile | grep -Fq "QEMU_MACHINE='q35'" || {
    echo "test-ioapic-irq1 должен использовать QEMU_MACHINE='q35'" >&2; exit 1; }
if grep -A2 '^test-ioapic-irq1:' Makefile | grep -Fq 'QEMU_CPU='; then
    echo "test-ioapic-irq1 должен не задавать QEMU_CPU" >&2; exit 1
fi
grep -A2 '^test-ioapic-irq1:' Makefile | grep -Fq "QEMU_SMP='1'" || {
    echo "test-ioapic-irq1 должен использовать QEMU_SMP='1'" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-ioapic-irq1' Makefile || {
    echo "test: target не содержит test-ioapic-irq1" >&2; exit 1; }
for strict_symbol in mktemp timeout send_monitor_quit successful_shutdown \
                     best_effort_cleanup cleanup no-reboot no-shutdown; do
    grep -Fq "$strict_symbol" scripts/test-qemu-ioapic-irq1.sh || {
        echo "test-qemu-ioapic-irq1.sh не содержит $strict_symbol" >&2
        exit 1
    }
done
grep -Fq 'INTERRUPTS_LOG' scripts/test-qemu-ioapic-irq1.sh || {
    echo "test-qemu-ioapic-irq1.sh не сохраняет INTERRUPTS_LOG" >&2; exit 1; }
grep -Fq 'sendkey a' scripts/test-qemu-ioapic-irq1.sh || {
    echo "test-qemu-ioapic-irq1.sh не отправляет sendkey a" >&2; exit 1; }
grep -Fq 'sendkey b' scripts/test-qemu-ioapic-irq1.sh || {
    echo "test-qemu-ioapic-irq1.sh не отправляет sendkey b" >&2; exit 1; }
grep -Fq 'sendkey c' scripts/test-qemu-ioapic-irq1.sh || {
    echo "test-qemu-ioapic-irq1.sh не отправляет sendkey c" >&2; exit 1; }
grep -Fq '[IOAPIC] route: irq=1 vector=0x41' scripts/test-qemu-ioapic-irq1.sh || {
    echo "test-qemu-ioapic-irq1.sh не валидирует route marker" >&2; exit 1; }
grep -Fq '[IOAPIC] irq1 vector=0x41' scripts/test-qemu-ioapic-irq1.sh || {
    echo "test-qemu-ioapic-irq1.sh не валидирует vector 0x41" >&2; exit 1; }
grep -Fq '[IOAPIC] irq1 lapic_eoi' scripts/test-qemu-ioapic-irq1.sh || {
    echo "test-qemu-ioapic-irq1.sh не валидирует LAPIC EOI" >&2; exit 1; }
grep -Fq 'extract_console_input' scripts/test-qemu-ioapic-irq1.sh || {
    echo "test-qemu-ioapic-irq1.sh не содержит extract_console_input" >&2; exit 1; }
grep -Fq 'QEMU IOAPIC IRQ1 routing test: OK' scripts/test-qemu-ioapic-irq1.sh || {
    echo "test-qemu-ioapic-irq1.sh не содержит success marker" >&2; exit 1; }
if grep -Fq 'hfoc> abc' scripts/test-qemu-ioapic-irq1.sh; then
    echo "test-qemu-ioapic-irq1.sh требует literal contiguous hfoc> abc" >&2
    exit 1
fi
# IOAPIC Stage 2 IRQ0 boot integration and QEMU proof.
grep -Fq 'ioapic_route_irq(0U, IOAPIC_IRQ0_VECTOR);' kernel/src/kernel.c || {
    echo "kernel.c не вызывает ioapic_route_irq для IRQ0" >&2; exit 1; }
irq0_route_line=$(grep -n 'ioapic_route_irq(0U, IOAPIC_IRQ0_VECTOR);' kernel/src/kernel.c | head -n1 | cut -d: -f1 || true)
irq1_route_line=$(grep -n 'ioapic_route_irq(1U, IOAPIC_IRQ1_VECTOR);' kernel/src/kernel.c | head -n1 | cut -d: -f1 || true)
[[ -n "$irq0_route_line" && -n "$irq1_route_line" &&
   "$irq0_route_line" -gt "$irq1_route_line" &&
   "$irq0_route_line" -lt "$keyboard_init_line" ]] || {
    echo "ioapic_route_irq IRQ0 должен идти после IRQ1 и до keyboard_init" >&2
    exit 1
}

grep -Fq 'test-ioapic-irq0' Makefile || {
    echo "Отсутствует test-ioapic-irq0 в Makefile" >&2; exit 1; }
bash -n scripts/test-qemu-ioapic-irq0.sh || {
    echo "Синтаксис test-qemu-ioapic-irq0.sh некорректен" >&2; exit 1; }
grep -A2 '^test-ioapic-irq0:' Makefile | grep -Fq "QEMU_MACHINE='q35'" || {
    echo "test-ioapic-irq0 должен использовать QEMU_MACHINE='q35'" >&2; exit 1; }
if grep -A2 '^test-ioapic-irq0:' Makefile | grep -Fq 'QEMU_CPU='; then
    echo "test-ioapic-irq0 должен не задавать QEMU_CPU" >&2; exit 1
fi
grep -A2 '^test-ioapic-irq0:' Makefile | grep -Fq "QEMU_SMP='1'" || {
    echo "test-ioapic-irq0 должен использовать QEMU_SMP='1'" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-ioapic-irq0' Makefile || {
    echo "test: target не содержит test-ioapic-irq0" >&2; exit 1; }
for strict_symbol in mktemp timeout send_monitor_quit successful_shutdown \
                     best_effort_cleanup cleanup no-reboot no-shutdown; do
    grep -Fq "$strict_symbol" scripts/test-qemu-ioapic-irq0.sh || {
        echo "test-qemu-ioapic-irq0.sh не содержит $strict_symbol" >&2
        exit 1
    }
done
grep -Fq 'INTERRUPTS_LOG' scripts/test-qemu-ioapic-irq0.sh || {
    echo "test-qemu-ioapic-irq0.sh не сохраняет INTERRUPTS_LOG" >&2; exit 1; }
grep -Fq 'Servicing hardware INT=0x42' scripts/test-qemu-ioapic-irq0.sh || {
    echo "test-qemu-ioapic-irq0.sh не требует INT=0x42" >&2; exit 1; }
grep -Fq '"Servicing hardware INT=0x40" in trace_suffix' \
    scripts/test-qemu-ioapic-irq0.sh || {
    echo "test-qemu-ioapic-irq0.sh не отклоняет INT=0x40 в suffix" >&2; exit 1; }
grep -Fq '"Servicing hardware INT=0x20" in trace_suffix' \
    scripts/test-qemu-ioapic-irq0.sh || {
    echo "test-qemu-ioapic-irq0.sh не отклоняет INT=0x20 в suffix" >&2; exit 1; }
grep -Fq 'TICKS: (\d+)' scripts/test-qemu-ioapic-irq0.sh || {
    echo "test-qemu-ioapic-irq0.sh не парсит TICKS:" >&2; exit 1; }
grep -Fq 'info pic' scripts/test-qemu-ioapic-irq0.sh || {
    echo "test-qemu-ioapic-irq0.sh не проверяет info pic" >&2; exit 1; }
grep -Fq '[IOAPIC] route: irq=0 vector=0x40' scripts/test-qemu-ioapic-irq0.sh || {
    echo "test-qemu-ioapic-irq0.sh не валидирует route marker" >&2; exit 1; }
grep -Fq '[IOAPIC] route failed: irq=0' scripts/test-qemu-ioapic-irq0.sh || {
    echo "test-qemu-ioapic-irq0.sh не проверяет route failed" >&2; exit 1; }
grep -Fq '[IOAPIC] irq0 vector=0x40' scripts/test-qemu-ioapic-irq0.sh || {
    echo "test-qemu-ioapic-irq0.sh не валидирует vector 0x40" >&2; exit 1; }
grep -Fq '[IOAPIC] irq0 lapic_eoi' scripts/test-qemu-ioapic-irq0.sh || {
    echo "test-qemu-ioapic-irq0.sh не валидирует LAPIC EOI" >&2; exit 1; }
grep -Fq 'TICKS after > before: yes' scripts/test-qemu-ioapic-irq0.sh || {
    echo "test-qemu-ioapic-irq0.sh не подтверждает рост ticks" >&2; exit 1; }
grep -Fq 'QEMU IOAPIC IRQ0 routing test: OK' scripts/test-qemu-ioapic-irq0.sh || {
    echo "test-qemu-ioapic-irq0.sh не содержит success marker" >&2; exit 1; }
grep -Fq '[IOAPIC] route: irq=0 vector=0x40 gsi=' kernel/src/ioapic.c || {
    echo "ioapic.c не содержит exact IRQ0 route success marker" >&2; exit 1; }
grep -Fq '[IOAPIC] route failed: irq=' kernel/src/ioapic.c || {
    echo "ioapic.c не содержит exact IRQ route failure prefix" >&2; exit 1; }
grep -Fq 'serial_write(" status=")' kernel/src/ioapic.c || {
    echo "ioapic.c не содержит IRQ route failure status prefix" >&2; exit 1; }

# IOAPIC Stage 2 static and documentation integration.
grep -Fq 'Stage 2 IOAPIC IRQ0 shadow route' README.md || {
    echo "README.md не содержит Stage 2 IOAPIC IRQ0 shadow route" >&2; exit 1; }
grep -Fq 'make test-ioapic-irq0' README.md || {
    echo "README.md не содержит make test-ioapic-irq0" >&2; exit 1; }
grep -Fq 'shadow route, не владеет ticks' README.md || {
    echo "README.md не фиксирует shadow semantics vector 0x40" >&2; exit 1; }
grep -Fq 'authoritative owner времени остаётся legacy PIC vector 0x20' README.md || {
    echo "README.md не фиксирует authoritative PIC timer owner" >&2; exit 1; }
grep -Fq 'IOAPIC Stage 2 IRQ0 shadow routing' docs/ARCHITECTURE.md || {
    echo "ARCHITECTURE.md не содержит Stage 2 IRQ0 shadow routing" >&2; exit 1; }
grep -Fq 'ioapic_is_irq0_routed' docs/ARCHITECTURE.md || {
    echo "ARCHITECTURE.md не содержит ioapic_is_irq0_routed" >&2; exit 1; }
grep -Fq 'ioapic_route_irq(0, 0x40)' docs/ARCHITECTURE.md || {
    echo "ARCHITECTURE.md не содержит boot call ioapic_route_irq(0, 0x40)" >&2; exit 1; }
grep -Fq 'PIC vector 0x20' docs/ARCHITECTURE.md || {
    echo "ARCHITECTURE.md не фиксирует PIC vector 0x20" >&2; exit 1; }
grep -Fq 'authoritative timer owner' docs/ARCHITECTURE.md || {
    echo "ARCHITECTURE.md не фиксирует authoritative PIC timer owner" >&2; exit 1; }
grep -Fq 'IOAPIC IRQ0 shadow route' docs/ROADMAP.md || {
    echo "ROADMAP.md не отмечает IOAPIC IRQ0 shadow route" >&2; exit 1; }
grep -Fq 'Stage 2, реализован в текущей ветке' docs/ROADMAP.md || {
    echo "ROADMAP.md не отмечает Stage 2 как реализованный" >&2; exit 1; }
grep -Fq "Stage 3b PIC IRQ0 masking (реализован в \`main\`; при success PIC IRQ0 маскируется, authoritative PIT ticks идут через vector \`0x40\`, fallback сохраняет PIC vector \`0x20\`);" docs/ROADMAP.md || {
    echo "ROADMAP.md не фиксирует post-merge Stage 3b формулировку" >&2; exit 1; }

# Stage 3a CI/static/docs integration: QEMU proof contract and docs.
for qemu_contract in \
    'trace_offset_path.write_text' \
    'trace_suffix_text' \
    '"Servicing hardware INT=0x21" in trace_suffix' \
    'if ("Servicing hardware INT=0x41" in trace_suffix and' \
    '"Servicing hardware INT=0x42" in trace_suffix)' \
    'len(abc_lines) != 1' \
    'pic0_imr != "ff" or pic1_imr != "ff"' \
    'exact_masks = "[PIC ] Master mask: 0x00000000000000FF; slave mask: 0x00000000000000FF"'; do
    grep -Fq "$qemu_contract" scripts/test-qemu-ioapic-irq1.sh || {
        echo "test-qemu-ioapic-irq1.sh не содержит $qemu_contract" >&2
        exit 1
    }
done

for stage3b_validator in \
    scripts/test-qemu-lapic.sh \
    scripts/test-qemu-ioapic-irq0.sh; do
    grep -Fq 'stage3b_mask_line = "[PIC ] Master mask: 0x00000000000000FF; slave mask: 0x00000000000000FF"' "$stage3b_validator" || {
        echo "$stage3b_validator не сохраняет exact uppercase Stage 3b COM1 masks" >&2
        exit 1
    }
    grep -Fq 'if pic0_imr != "ff" or pic1_imr != "ff":' "$stage3b_validator" || {
        echo "$stage3b_validator не отклоняет HMP imr отличный от ff/ff" >&2
        exit 1
    }
done

grep -Fq '"master mask: 0x00000000000000ff" not in lower_text' \
    scripts/test-qemu-ioapic-probe.sh || {
    echo "test-qemu-ioapic-probe.sh не сохраняет Stage 3b master FF expectation" >&2
    exit 1
}

# shellcheck disable=SC2016
for legacy_contract in \
    'QEMU_CPU=${QEMU_CPU:-max,-apic}' \
    'EXPECTED_PIC0_IMR=${EXPECTED_PIC0_IMR:-fc}' \
    'first_master == 252 && second_master == 252'; do
    grep -Fq "$legacy_contract" scripts/test-qemu-irqs.sh || {
        echo "test-qemu-irqs.sh не сохраняет legacy profile/FC contract: $legacy_contract" >&2
        exit 1
    }
done

for stage3a_doc in README.md docs/ARCHITECTURE.md; do
    grep -Fq 'Stage 3a PIC IRQ1 masking' "$stage3a_doc" || {
        echo "$stage3a_doc не описывает Stage 3a PIC IRQ1 masking" >&2
        exit 1
    }
    grep -Fq '[PIC ] IRQ1 masked; IOAPIC vector=0x41 active.' "$stage3a_doc" || {
        echo "$stage3a_doc не фиксирует exact success marker" >&2
        exit 1
    }
    grep -Fq '[PIC ] Master mask: 0x00000000000000FE; slave mask: 0x00000000000000FF' "$stage3a_doc" || {
        echo "$stage3a_doc не фиксирует exact uppercase FE/FF masks" >&2
        exit 1
    }
done

# Stage 3b Task 6: CI, static checks and documentation integration.
for stage3b_doc in README.md docs/ARCHITECTURE.md; do
    grep -Fq 'Stage 3b PIC IRQ0 masking' "$stage3b_doc" || {
        echo "$stage3b_doc не описывает Stage 3b PIC IRQ0 masking" >&2
        exit 1
    }
    grep -Fq "Stage 3b PIC IRQ0 masking реализован в \`main\`." \
        "$stage3b_doc" || {
        echo "$stage3b_doc не фиксирует Stage 3b как реализованный в main" >&2
        exit 1
    }
    if grep -nE 'Stage 3b PIC IRQ0 masking.*(PR #19|current branch / Draft|not merged)' \
        "$stage3b_doc"; then
        echo "$stage3b_doc содержит stale Draft/not merged формулировку Stage 3b" >&2
        exit 1
    fi
    grep -Fq 'verified IOAPIC route makes vector 0x40 authoritative PIT timer owner' \
        "$stage3b_doc" || {
        echo "$stage3b_doc не фиксирует success ownership vector 0x40" >&2
        exit 1
    }
    grep -Fq 'fallback preserves PIC vector 0x20 timer ownership' \
        "$stage3b_doc" || {
        echo "$stage3b_doc не фиксирует fallback ownership vector 0x20" >&2
        exit 1
    }
    grep -Fq 'stray PIC EOI only' "$stage3b_doc" || {
        echo "$stage3b_doc не описывает stray 0x20 disposition" >&2
        exit 1
    }
    grep -Fq '[PIC ] Master mask: 0x00000000000000FF; slave mask: 0x00000000000000FF' \
        "$stage3b_doc" || {
        echo "$stage3b_doc не фиксирует exact Stage 3b success FF/FF masks" >&2
        exit 1
    }
    grep -Fq '[PIC ] Master mask: 0x00000000000000FE; slave mask: 0x00000000000000FF' \
        "$stage3b_doc" || {
        echo "$stage3b_doc не фиксирует exact Stage 3b fallback FE/FF masks" >&2
        exit 1
    }
    grep -Fq '[PIC ] Master mask: 0x00000000000000FC; slave mask: 0x00000000000000FF' \
        "$stage3b_doc" || {
        echo "$stage3b_doc не отличает legacy FC/FF masks" >&2
        exit 1
    }
done

grep -Fq 'make test-pic-irq0-masking' README.md || {
    echo "README.md не документирует make test-pic-irq0-masking" >&2; exit 1; }
grep -Fq 'make test-pic-irq0-masking-fallback' README.md || {
    echo "README.md не документирует make test-pic-irq0-masking-fallback" >&2; exit 1; }
grep -Fq 'PIC IRQ0 is masked only in success' README.md || {
    echo "README.md не фиксирует success-only PIC IRQ0 masking" >&2; exit 1; }
grep -Fq 'Stage 3a IRQ1 remains independent' README.md || {
    echo "README.md не фиксирует Stage 3a IRQ1 independence" >&2; exit 1; }
grep -Fq 'irq0_ioapic_active' docs/ARCHITECTURE.md || {
    echo "ARCHITECTURE.md не описывает irq0_ioapic_active ownership state" >&2; exit 1; }
grep -Fq 'IF=0' docs/ARCHITECTURE.md || {
    echo "ARCHITECTURE.md не фиксирует IF=0 publication boundary" >&2; exit 1; }
grep -Fq 'no dispatcher IOAPIC reread' docs/ARCHITECTURE.md || {
    echo "ARCHITECTURE.md не фиксирует no dispatcher IOAPIC reread" >&2; exit 1; }

grep -Fq 'make test-ioapic-irq1' README.md || {
    echo "README.md не содержит make test-ioapic-irq1" >&2; exit 1; }
grep -Fq 'authoritative owner времени остаётся legacy PIC vector 0x20' README.md || {
    echo "README.md не фиксирует authoritative PIC timer owner" >&2; exit 1; }
grep -Fq 'max,-apic' README.md || {
    echo "README.md не документирует legacy profile max,-apic" >&2; exit 1; }
grep -Fq '0xFC' README.md || {
    echo "README.md не документирует legacy master mask 0xFC" >&2; exit 1; }
if grep -Fq '[PIC ] Master mask: 0x00000000000000fc; slave mask: 0x00000000000000ff' \
    README.md; then
    echo "README.md содержит lowercase COM1 mask example" >&2
    exit 1
fi
grep -Fq '[PIC ] Master mask: 0x00000000000000FC; slave mask: 0x00000000000000FF' \
    README.md || {
    echo "README.md не содержит uppercase legacy COM1 mask example" >&2
    exit 1
}

grep -Fq 'IOAPIC Stage 3a PIC IRQ1 masking' docs/ARCHITECTURE.md || {
    echo "ARCHITECTURE.md не содержит Stage 3a section" >&2; exit 1; }
grep -Fq 'irq1_ready' docs/ARCHITECTURE.md || {
    echo "ARCHITECTURE.md не описывает irq1_ready conjunction" >&2; exit 1; }
grep -Fq 'trace_offset' docs/ARCHITECTURE.md || {
    echo "ARCHITECTURE.md не описывает trace_offset contract" >&2; exit 1; }
grep -Fq 'INT=0x21 absent' docs/ARCHITECTURE.md || {
    echo "ARCHITECTURE.md не фиксирует suffix-only INT=0x21 absence" >&2; exit 1; }

grep -Fq "Stage 3a PIC IRQ1 masking (реализован в \`main\`; PIC IRQ1 маскируется после verified IOAPIC IRQ1 route);" docs/ROADMAP.md || {
    echo "ROADMAP.md не фиксирует post-merge Stage 3a формулировку" >&2; exit 1; }
grep -Fq "Stage 3b PIC IRQ0 masking (реализован в \`main\`; при success PIC IRQ0 маскируется, authoritative PIT ticks идут через vector \`0x40\`, fallback сохраняет PIC vector \`0x20\`);" docs/ROADMAP.md || {
    echo "ROADMAP.md не фиксирует post-merge Stage 3b формулировку" >&2; exit 1; }
if grep -nE 'Stage 3[ab] PIC IRQ[01] masking.*(PR #18|PR #19|current branch / Draft|not merged)' docs/ROADMAP.md; then
    echo "ROADMAP.md содержит stale Draft/not merged формулировку в Stage 3a/3b" >&2
    exit 1
fi

# Stage 3b Task 5: QEMU success/fallback proofs and isolated fallback build.
for stage3b_target in test-pic-irq0-masking test-pic-irq0-masking-fallback; do
    grep -Eq "^\.PHONY:.*${stage3b_target}[[:space:]]" Makefile || {
        echo "Makefile .PHONY не содержит $stage3b_target" >&2
        exit 1
    }
    grep -A15 "^$stage3b_target:" Makefile | grep -Fq "QEMU_MACHINE='q35'" || {
        echo "$stage3b_target должен использовать QEMU_MACHINE='q35'" >&2
        exit 1
    }
    if grep -A15 "^$stage3b_target:" Makefile | grep -Fq 'QEMU_CPU='; then
        echo "$stage3b_target должен не задавать QEMU_CPU" >&2
        exit 1
    fi
    grep -A15 "^$stage3b_target:" Makefile | grep -Fq "QEMU_SMP='1'" || {
        echo "$stage3b_target должен использовать QEMU_SMP='1'" >&2
        exit 1
    }
done

make_test_block=$(sed -n '/^test-serial:$/,/^[^[:space:]]/p' Makefile)
fallback_test_line=$(printf '%s\n' "$make_test_block" |
    grep -n 'test-pic-irq0-masking-fallback$' | head -n1 |
    cut -d: -f1 || true)
success_test_line=$(printf '%s\n' "$make_test_block" |
    grep -n 'test-pic-irq0-masking$' | head -n1 |
    cut -d: -f1 || true)
[[ -n "$fallback_test_line" && -n "$success_test_line" &&
   "$fallback_test_line" -lt "$success_test_line" ]] || {
    echo "make test должен запускать fallback до production success target" >&2
    exit 1
}

for stage3b_script in scripts/test-qemu-pic-irq0-masking.sh \
                      scripts/test-qemu-pic-irq0-masking-fallback.sh; do
    [[ -f "$stage3b_script" ]] || {
        echo "Отсутствует $stage3b_script" >&2
        exit 1
    }
    bash -n "$stage3b_script" || {
        echo "Синтаксис $stage3b_script некорректен" >&2
        exit 1
    }
    for strict_symbol in mktemp timeout send_monitor_quit successful_shutdown \
                         best_effort_cleanup cleanup no-reboot no-shutdown; do
        grep -Fq "$strict_symbol" "$stage3b_script" || {
            echo "$stage3b_script не содержит $strict_symbol" >&2
            exit 1
        }
    done
    grep -Fq 'INTERRUPTS_LOG' "$stage3b_script" || {
        echo "$stage3b_script не сохраняет INTERRUPTS_LOG" >&2
        exit 1
    }
    grep -Fq 'ticks_request_offset = len(serial_path.read_bytes())' \
        "$stage3b_script" || {
        echo "$stage3b_script не сохраняет ticks_request_offset перед sendkey" >&2
        exit 1
    }
    grep -Fq 'def new_tick_value(offset):' "$stage3b_script" || {
        echo "$stage3b_script не содержит offset-aware TICKS parser" >&2
        exit 1
    }
    grep -Fq 'clean = serial_suffix(offset)' "$stage3b_script" || {
        echo "$stage3b_script не парсит TICKS только из serial suffix" >&2
        exit 1
    }
    grep -Fq 'if offset > 0 and data[offset - 1] not in (0x0a, 0x0d):' \
        "$stage3b_script" || {
        echo "$stage3b_script не обрабатывает partial serial line на offset" >&2
        exit 1
    }
    grep -Fq 'TICKS:\s*(\d+)\r?\n' "$stage3b_script" || {
        echo "$stage3b_script не требует completed TICKS line" >&2
        exit 1
    }
    grep -Fq 'timed out waiting for new TICKS snapshot' "$stage3b_script" || {
        echo "$stage3b_script не отклоняет отсутствие нового TICKS line" >&2
        exit 1
    }
done

# shellcheck disable=SC2016
if ! grep -A20 '^test-pic-irq0-masking-fallback:' Makefile | \
    grep -Fq 'strings $(FALLBACK_KERNEL)'; then
    echo "fallback target не проверяет marker в fallback kernel" >&2
    exit 1
fi
# shellcheck disable=SC2016
if ! grep -A20 '^test-pic-irq0-masking-fallback:' Makefile | \
    grep -Fq 'strings $(FALLBACK_ISO)'; then
    echo "fallback target не проверяет marker в fallback ISO" >&2
    exit 1
fi
# shellcheck disable=SC2016
if ! grep -A20 '^test-pic-irq0-masking-fallback:' Makefile | \
    grep -Fq 'strings $(KERNEL)'; then
    echo "fallback target не проверяет отсутствие marker в production kernel" >&2
    exit 1
fi
# shellcheck disable=SC2016
if ! grep -A20 '^test-pic-irq0-masking-fallback:' Makefile | \
    grep -Fq 'strings $(IMAGE)'; then
    echo "fallback target не проверяет отсутствие marker в production ISO" >&2
    exit 1
fi
# shellcheck disable=SC2016
if ! grep -A20 '^test-pic-irq0-masking-fallback:' Makefile | \
    grep -Fq '$(MAKE) test-pic-irq0-masking'; then
    echo "fallback target не запускает production success proof" >&2
    exit 1
fi

grep -Fq 'QEMU PIC IRQ0 masking test: OK' scripts/test-qemu-pic-irq0-masking.sh || {
    echo "test-qemu-pic-irq0-masking.sh не содержит success marker" >&2
    exit 1
}
grep -Fq 'trace_offset_path.write_text' scripts/test-qemu-pic-irq0-masking.sh || {
    echo "test-qemu-pic-irq0-masking.sh не сохраняет trace_offset" >&2
    exit 1
}
grep -Fq 'if "Servicing hardware INT=0x20" in trace_suffix:' \
    scripts/test-qemu-pic-irq0-masking.sh || {
    echo "test-qemu-pic-irq0-masking.sh не отклоняет INT=0x20 в suffix" >&2
    exit 1
}
grep -Fq 'if "Servicing hardware INT=0x40" not in trace_suffix:' \
    scripts/test-qemu-pic-irq0-masking.sh || {
    echo "test-qemu-pic-irq0-masking.sh не требует INT=0x40 в suffix" >&2
    exit 1
}
grep -Fq 'if "Servicing hardware INT=0x42" not in trace_suffix:' \
    scripts/test-qemu-pic-irq0-masking.sh || {
    echo "test-qemu-pic-irq0-masking.sh не требует INT=0x42 в suffix" >&2
    exit 1
}
grep -Fq 'TICKS after > before: yes' scripts/test-qemu-pic-irq0-masking.sh || {
    echo "test-qemu-pic-irq0-masking.sh не подтверждает рост ticks" >&2
    exit 1
}
grep -Fq '[PIC ] IRQ0 masked; IOAPIC vector=0x40 active.' \
    scripts/test-qemu-pic-irq0-masking.sh || {
    echo "test-qemu-pic-irq0-masking.sh не требует Stage 3b success marker" >&2
    exit 1
}
grep -Fq '[PIC ] IRQ1 masked; IOAPIC vector=0x41 active.' \
    scripts/test-qemu-pic-irq0-masking.sh || {
    echo "test-qemu-pic-irq0-masking.sh не требует Stage 3a IRQ1 marker" >&2
    exit 1
}
grep -Fq '[PIC ] IRQ0 unmasked; legacy PIC timer retained.' \
    scripts/test-qemu-pic-irq0-masking.sh || {
    echo "test-qemu-pic-irq0-masking.sh не отклоняет fallback marker" >&2
    exit 1
}
grep -Fq '[PIC ] Master mask: 0x00000000000000FF; slave mask: 0x00000000000000FF' \
    scripts/test-qemu-pic-irq0-masking.sh || {
    echo "test-qemu-pic-irq0-masking.sh не требует exact FF/FF COM1 masks" >&2
    exit 1
}
grep -Fq 'if pic0_imr != "ff" or pic1_imr != "ff":' \
    scripts/test-qemu-pic-irq0-masking.sh || {
    echo "test-qemu-pic-irq0-masking.sh не требует HMP imr ff/ff" >&2
    exit 1
}
grep -Fq 'if "Servicing hardware INT=0x41" not in irq1_trace_suffix:' \
    scripts/test-qemu-pic-irq0-masking.sh || {
    echo "test-qemu-pic-irq0-masking.sh не требует IRQ1 INT=0x41 suffix" >&2
    exit 1
}
grep -Fq 'if "Servicing hardware INT=0x21" in irq1_trace_suffix:' \
    scripts/test-qemu-pic-irq0-masking.sh || {
    echo "test-qemu-pic-irq0-masking.sh не отклоняет IRQ1 INT=0x21" >&2
    exit 1
}

grep -Fq 'QEMU PIC IRQ0 masking fallback test: OK' \
    scripts/test-qemu-pic-irq0-masking-fallback.sh || {
    echo "test-qemu-pic-irq0-masking-fallback.sh не содержит success marker" >&2
    exit 1
}
grep -Fq 'fallback_ticks_before' scripts/test-qemu-pic-irq0-masking-fallback.sh || {
    echo "test-qemu-pic-irq0-masking-fallback.sh не парсит fallback_ticks_before" >&2
    exit 1
}
grep -Fq 'fallback_ticks_after' scripts/test-qemu-pic-irq0-masking-fallback.sh || {
    echo "test-qemu-pic-irq0-masking-fallback.sh не парсит fallback_ticks_after" >&2
    exit 1
}
grep -Fq 'fallback_ticks_after > fallback_ticks_before: yes' \
    scripts/test-qemu-pic-irq0-masking-fallback.sh || {
    echo "test-qemu-pic-irq0-masking-fallback.sh не подтверждает рост fallback ticks" >&2
    exit 1
}
grep -Fq '[IOAPIC] test: forcing IRQ0 route failure' \
    scripts/test-qemu-pic-irq0-masking-fallback.sh || {
    echo "test-qemu-pic-irq0-masking-fallback.sh не требует failure marker" >&2
    exit 1
}
grep -Fq '[IOAPIC] route failed: irq=0' \
    scripts/test-qemu-pic-irq0-masking-fallback.sh || {
    echo "test-qemu-pic-irq0-masking-fallback.sh не требует route failed marker" >&2
    exit 1
}
grep -Fq '[PIC ] IRQ0 unmasked; legacy PIC timer retained.' \
    scripts/test-qemu-pic-irq0-masking-fallback.sh || {
    echo "test-qemu-pic-irq0-masking-fallback.sh не требует fallback marker" >&2
    exit 1
}
grep -Fq '[PIC ] IRQ0 masked; IOAPIC vector=0x40 active.' \
    scripts/test-qemu-pic-irq0-masking-fallback.sh || {
    echo "test-qemu-pic-irq0-masking-fallback.sh не отклоняет success marker" >&2
    exit 1
}
grep -Fq '[PIC ] Master mask: 0x00000000000000FE; slave mask: 0x00000000000000FF' \
    scripts/test-qemu-pic-irq0-masking-fallback.sh || {
    echo "test-qemu-pic-irq0-masking-fallback.sh не требует exact FE/FF COM1 masks" >&2
    exit 1
}
grep -Fq 'if pic0_imr != "ff" or pic1_imr != "ff":' \
    scripts/test-qemu-pic-irq0-masking-fallback.sh || {
    echo "test-qemu-pic-irq0-masking-fallback.sh не требует HMP imr ff/ff" >&2
    exit 1
}
grep -Fq 'POST_TRACE_OFFSET_FILE=' \
    scripts/test-qemu-pic-irq0-masking-fallback.sh || {
    echo "test-qemu-pic-irq0-masking-fallback.sh не сохраняет post-transition offset" >&2
    exit 1
}
grep -Fq 'if "Servicing hardware INT=0x20" not in pre_trace_suffix:' \
    scripts/test-qemu-pic-irq0-masking-fallback.sh || {
    echo "test-qemu-pic-irq0-masking-fallback.sh не требует pre-transition INT=0x20" >&2
    exit 1
}
grep -Fq 'if "Servicing hardware INT=0x42" in pre_trace_suffix:' \
    scripts/test-qemu-pic-irq0-masking-fallback.sh || {
    echo "test-qemu-pic-irq0-masking-fallback.sh не запрещает pre-transition INT=0x42" >&2
    exit 1
}
grep -Fq 'if "Servicing hardware INT=0x42" not in post_trace_suffix:' \
    scripts/test-qemu-pic-irq0-masking-fallback.sh || {
    echo "test-qemu-pic-irq0-masking-fallback.sh не требует post-transition INT=0x42" >&2
    exit 1
}
grep -Fq 'if "Servicing hardware INT=0x20" in post_trace_suffix:' \
    scripts/test-qemu-pic-irq0-masking-fallback.sh || {
    echo "test-qemu-pic-irq0-masking-fallback.sh не запрещает post-transition INT=0x20" >&2
    exit 1
}
if grep -Fq 'Servicing hardware INT=0x40' \
    scripts/test-qemu-pic-irq0-masking-fallback.sh; then
    echo "test-qemu-pic-irq0-masking-fallback.sh заявляет INT=0x40 как fallback proof" >&2
    exit 1
fi
grep -Fq 'if "Servicing hardware INT=0x41" not in irq1_trace_suffix:' \
    scripts/test-qemu-pic-irq0-masking-fallback.sh || {
    echo "test-qemu-pic-irq0-masking-fallback.sh не требует IRQ1 INT=0x41 suffix" >&2
    exit 1
}
grep -Fq 'if "Servicing hardware INT=0x21" in irq1_trace_suffix:' \
    scripts/test-qemu-pic-irq0-masking-fallback.sh || {
    echo "test-qemu-pic-irq0-masking-fallback.sh не отклоняет IRQ1 INT=0x21" >&2
    exit 1
}

# shellcheck disable=SC2016
for fallback_contract in \
    'FALLBACK_OBJ_DIR := build/fallback/obj' \
    'FALLBACK_C_OBJS := $(patsubst kernel/src/%.c,$(FALLBACK_OBJ_DIR)/%.o,$(C_SOURCES))' \
    'FALLBACK_ASM_OBJS := $(patsubst kernel/src/%.S,$(FALLBACK_OBJ_DIR)/%.o,$(ASM_SOURCES))' \
    'FALLBACK_OBJS := $(FALLBACK_C_OBJS) $(FALLBACK_ASM_OBJS)' \
    'FALLBACK_DEPS := $(patsubst kernel/src/%.c,$(FALLBACK_OBJ_DIR)/%.d,$(C_SOURCES)) $(patsubst kernel/src/%.S,$(FALLBACK_OBJ_DIR)/%.d,$(ASM_SOURCES))' \
    'FALLBACK_KERNEL := build/fallback/hfoc-kernel.elf' \
    'FALLBACK_ISO := build/fallback/hfoc-os-$(VERSION)-fallback.iso' \
    'FALLBACK_ISO_ROOT := build/fallback/iso_root' \
    'FALLBACK_CFLAGS := $(CFLAGS) -DHFOC_IRQ0_ROUTE_FAILURE_TEST' \
    '$(FALLBACK_OBJ_DIR)/%.o: kernel/src/%.c' \
    '$(FALLBACK_OBJ_DIR)/%.o: kernel/src/%.S' \
    '$(CC) $(FALLBACK_CFLAGS) -MMD -MP -c $< -o $@' \
    '$(CC) $(ASFLAGS) -MMD -MP -c $< -o $@' \
    '-include $(FALLBACK_DEPS)' \
    '$(FALLBACK_KERNEL): $(FALLBACK_OBJS) kernel/linker.ld'; do
    grep -Fq -- "$fallback_contract" Makefile || {
        echo "Makefile не содержит $fallback_contract" >&2
        exit 1
    }
done

# shellcheck disable=SC2016
if grep -A20 -F '$(FALLBACK_ISO):' Makefile | grep -Fq 'build/iso_root'; then
    echo "fallback ISO packaging использует production build/iso_root" >&2
    exit 1
fi
# shellcheck disable=SC2016
if grep -A2 -F '$(FALLBACK_OBJ_DIR)/%.o:' Makefile | grep -Fq 'build/obj'; then
    echo "fallback object rules используют production build/obj" >&2
    exit 1
fi
fallback_cflags_count=$(grep -F -o 'FALLBACK_CFLAGS' Makefile | wc -l)
[[ "$fallback_cflags_count" -eq 2 ]] || {
    echo "FALLBACK_CFLAGS используется вне fallback graph" >&2
    exit 1
}
fallback_macro_count=$(grep -F -o 'HFOC_IRQ0_ROUTE_FAILURE_TEST' Makefile | wc -l)
[[ "$fallback_macro_count" -eq 1 ]] || {
    echo "production Makefile graph получает HFOC_IRQ0_ROUTE_FAILURE_TEST" >&2
    exit 1
}

grep -Fq '#ifdef HFOC_IRQ0_ROUTE_FAILURE_TEST' kernel/src/ioapic.c || {
    echo "ioapic.c не содержит failure-injection macro guard" >&2
    exit 1
}
grep -Fq '[IOAPIC] test: forcing IRQ0 route failure' kernel/src/ioapic.c || {
    echo "ioapic.c не содержит forced failure marker" >&2
    exit 1
}
grep -Fq 'return ioapic_route_fail(irq, IOAPIC_ROUTE_FAILED);' \
    kernel/src/ioapic.c || {
    echo "ioapic.c не возвращает IOAPIC_ROUTE_FAILED в failure seam" >&2
    exit 1
}

# Stage 3a Task 3 / Stage 3b Task 4 boot policy and brace-aware static checks.
extract_policy_branch() {
    local mode=$1
    local needle=$2
    local file=$3
    awk -v mode="$mode" -v needle="$needle" '
        function brace_delta(s,    i, ch, opens, closes) {
            opens = 0
            closes = 0
            for (i = 1; i <= length(s); i++) {
                ch = substr(s, i, 1)
                if (ch == "{") opens++
                else if (ch == "}") closes++
            }
            return opens - closes
        }
        function closing_count(s,    i, ch, count) {
            count = 0
            for (i = 1; i <= length(s); i++) {
                ch = substr(s, i, 1)
                if (ch == "}") count++
            }
            return count
        }
        function begin_else(line, idx,    tail, delta) {
            tail = substr(line, idx + 4)
            delta = brace_delta(tail)
            if (delta > 0) {
                phase = 5
                depth = delta
                if (depth <= 0) exit
            } else {
                phase = 4
            }
        }
        index($0, needle) > 0 { found = 1; phase = 1 }
        found {
            if (phase == 1) {
                if (mode == "if") print
                delta = brace_delta($0)
                if (delta > 0) {
                    phase = 2
                    depth = delta
                    if (depth <= 0) exit
                }
            } else if (phase == 2) {
                if (mode == "if") {
                    if (index($0, "else") > 0) {
                        else_idx = index($0, "else")
                        depth -= closing_count(substr($0, 1, else_idx - 1))
                        if (depth <= 0) {
                            print
                            exit
                        }
                        print
                        depth += brace_delta($0)
                    } else {
                        print
                        depth += brace_delta($0)
                        if (depth <= 0) exit
                    }
                } else {
                    if (index($0, "else") > 0) {
                        else_idx = index($0, "else")
                        depth -= closing_count(substr($0, 1, else_idx - 1))
                        if (depth <= 0) {
                            print
                            begin_else($0, else_idx)
                        } else {
                            depth += brace_delta($0)
                        }
                    } else {
                        depth += brace_delta($0)
                        if (depth <= 0) phase = 3
                    }
                }
            } else if (phase == 3) {
                if (index($0, "else") > 0) {
                    print
                    begin_else($0, index($0, "else"))
                }
            } else if (phase == 4) {
                print
                delta = brace_delta($0)
                if (delta > 0) {
                    phase = 5
                    depth = delta
                    if (depth <= 0) exit
                }
            } else if (phase == 5) {
                print
                depth += brace_delta($0)
                if (depth <= 0) exit
            }
        }
    ' "$file"
}

extract_irq1_branch() {
    extract_policy_branch "$1" 'if (irq1_ready)' "$2"
}

extract_irq0_branch() {
    extract_policy_branch "$1" 'if (irq0_ready)' "$2"
}

extract_irq1_initializer() {
    awk '
        index($0, "const bool irq1_ready =") > 0 { found = 1 }
        found {
            print
            if (index($0, ";") > 0) exit
        }
    ' "$1" | tr '\n' ' ' | tr -s '[:space:]' ' '
}

extract_irq0_initializer() {
    awk '
        index($0, "const bool irq0_ready =") > 0 { found = 1 }
        found {
            print
            if (index($0, ";") > 0) exit
        }
    ' "$1" | tr '\n' ' ' | tr -s '[:space:]' ' '
}

verify_unconditional_pic_unmask() {
    awk '
        function is_keyword_at(s, i, kw,    n, prev, nextc) {
            n = length(kw)
            if (substr(s, i, n) != kw) return 0
            prev = (i == 1) ? "" : substr(s, i - 1, 1)
            nextc = substr(s, i + n, 1)
            if (prev != "" && prev ~ /[[:alnum:]_]/) return 0
            if (nextc != "" && nextc ~ /[[:alnum:]_]/) return 0
            return 1
        }
        function push_brace(kind) {
            stack_size++
            stack[stack_size] = kind
            if (kind == "guard") guard_depth++
        }
        function pop_brace(    kind) {
            if (stack_size > 0) {
                kind = stack[stack_size]
                stack_size--
                if (kind == "guard") guard_depth--
            }
        }
        function scan_line(line,    i, ch) {
            for (i = 1; i <= length(line); i++) {
                ch = substr(line, i, 1)
                if (is_keyword_at(line, i, "if") ||
                    is_keyword_at(line, i, "else") ||
                    is_keyword_at(line, i, "for") ||
                    is_keyword_at(line, i, "while") ||
                    is_keyword_at(line, i, "switch")) {
                    pending_guard = 1
                } else if (ch == "{") {
                    if (pending_guard) {
                        push_brace("guard")
                        pending_guard = 0
                    } else {
                        push_brace("plain")
                    }
                } else if (ch == "}") {
                    pop_brace()
                }
                if (index(substr(line, i), "pic_unmask(0U);") == 1 &&
                    (guard_depth > 0 || pending_guard == 1)) {
                    call_in_guard = 1
                }
            }
        }
        {
            if (index($0, "pic_unmask(0U);") > 0) {
                call_count++
                call_line = NR
            }
            if (index($0, "timer_init(HFOC_TIMER_HZ);") > 0) {
                timer_line = NR
                in_segment = 1
                guard_depth = 0
                stack_size = 0
                pending_guard = 0
            }
            if (in_segment && !seen_end) {
                scan_line($0)
            }
            if (in_segment && index($0, "log_pic_masks();") > 0) {
                log_line = NR
                seen_end = 1
            }
        }
        END {
            if (call_count != 1) {
                printf "FAIL expected exactly one pic_unmask(0U); call, found %d\n",
                    call_count
                exit 1
            }
            if (timer_line <= 0 || call_line <= timer_line) {
                printf "FAIL pic_unmask(0U); call line %d is not after timer_init line %d\n",
                    call_line, timer_line
                exit 1
            }
            if (log_line <= 0 || call_line >= log_line) {
                printf "FAIL pic_unmask(0U); call line %d is not before log_pic_masks line %d\n",
                    call_line, log_line
                exit 1
            }
            if (call_in_guard == 1) {
                printf "FAIL pic_unmask(0U); call is inside conditional/loop block at line %d\n",
                    call_line
                exit 1
            }
        }
    ' "$1"
}

irq1_ready_init=$(extract_irq1_initializer kernel/src/kernel.c)
if [[ -z "$irq1_ready_init" ]]; then
    echo "kernel.c не содержит const bool irq1_ready =" >&2
    exit 1
fi
if ! printf '%s' "$irq1_ready_init" |
    grep -Fq 'irq1_route_status == IOAPIC_OK && ioapic_is_irq1_routed()'; then
    echo "irq1_ready не содержит exact route/routed conjunction" >&2
    exit 1
fi

irq1_if_block=$(extract_irq1_branch if kernel/src/kernel.c)
irq1_else_block=$(extract_irq1_branch else kernel/src/kernel.c)
if [[ -z "$irq1_if_block" || -z "$irq1_else_block" ]]; then
    echo "kernel.c не содержит полный if (irq1_ready)/else блок" >&2
    exit 1
fi
if ! printf '%s' "$irq1_if_block" | grep -Fq 'pic_mask(1U)'; then
    echo "success branch не вызывает pic_mask(1U)" >&2
    exit 1
fi
if printf '%s' "$irq1_if_block" | grep -Fq 'pic_unmask(1U)'; then
    echo "success branch вызывает pic_unmask(1U)" >&2
    exit 1
fi
if ! printf '%s' "$irq1_else_block" | grep -Fq 'pic_unmask(1U)'; then
    echo "fallback branch не вызывает pic_unmask(1U)" >&2
    exit 1
fi
if printf '%s' "$irq1_else_block" | grep -Fq 'pic_mask(1U)'; then
    echo "fallback branch вызывает pic_mask(1U)" >&2
    exit 1
fi

grep -Fq 'const enum ioapic_status irq1_route_status =' kernel/src/kernel.c || {
    echo "kernel.c не сохраняет ioapic_route_irq status для IRQ1" >&2
    exit 1
}
grep -Fq 'const enum ioapic_status irq0_route_status =' \
    kernel/src/kernel.c || {
    echo "kernel.c не сохраняет ioapic_route_irq status для IRQ0" >&2
    exit 1
}
irq0_ready_init=$(extract_irq0_initializer kernel/src/kernel.c)
if [[ -z "$irq0_ready_init" ]]; then
    echo "kernel.c не содержит const bool irq0_ready =" >&2
    exit 1
fi
if ! printf '%s' "$irq0_ready_init" |
    grep -Fq 'irq0_route_status == IOAPIC_OK && ioapic_is_irq0_routed()'; then
    echo "irq0_ready не содержит exact route/routed conjunction" >&2
    exit 1
fi

irq0_if_block=$(extract_irq0_branch if kernel/src/kernel.c)
irq0_else_block=$(extract_irq0_branch else kernel/src/kernel.c)
if [[ -z "$irq0_if_block" || -z "$irq0_else_block" ]]; then
    echo "kernel.c не содержит полный if (irq0_ready)/else блок" >&2
    exit 1
fi
if ! printf '%s' "$irq0_if_block" | grep -Fq 'pic_mask(0U);'; then
    echo "success branch не вызывает pic_mask(0U)" >&2
    exit 1
fi
if printf '%s' "$irq0_if_block" | grep -Fq 'pic_unmask(0U);'; then
    echo "success branch вызывает pic_unmask(0U)" >&2
    exit 1
fi
if ! printf '%s' "$irq0_else_block" | grep -Fq 'pic_unmask(0U);'; then
    echo "fallback branch не вызывает pic_unmask(0U)" >&2
    exit 1
fi
if printf '%s' "$irq0_else_block" | grep -Fq 'pic_mask(0U);'; then
    echo "fallback branch вызывает pic_mask(0U)" >&2
    exit 1
fi
irq0_unmask_count=$(grep -F -o 'pic_unmask(0U);' kernel/src/kernel.c | wc -l)
[[ "$irq0_unmask_count" -eq 1 ]] || {
    echo "kernel.c содержит $irq0_unmask_count вызовов pic_unmask(0U), ожидался 1 в fallback branch" >&2
    exit 1
}
irq0_mask_count=$(grep -F -o 'pic_mask(0U);' kernel/src/kernel.c | wc -l)
[[ "$irq0_mask_count" -eq 1 ]] || {
    echo "kernel.c содержит $irq0_mask_count вызовов pic_mask(0U), ожидался 1 в success branch" >&2
    exit 1
}

grep -Fq '[PIC ] IRQ0 masked; IOAPIC vector=0x40 active.' \
    kernel/src/kernel.c || {
    echo "kernel.c не содержит exact IRQ0 success marker" >&2
    exit 1
}
grep -Fq '[PIC ] IRQ0 unmasked; legacy PIC timer retained.' \
    kernel/src/kernel.c || {
    echo "kernel.c не содержит exact IRQ0 fallback marker" >&2
    exit 1
}

irq0_route_line=$(grep -n 'ioapic_route_irq(0U, IOAPIC_IRQ0_VECTOR);' \
    kernel/src/kernel.c | head -n1 | cut -d: -f1 || true)
publish_line=$(grep -n 'irq0_ioapic_active_publish(irq0_ready);' \
    kernel/src/kernel.c | head -n1 | cut -d: -f1 || true)
publish_count=$(grep -F -o 'irq0_ioapic_active_publish(irq0_ready);' \
    kernel/src/kernel.c | wc -l)
cpu_cli_line=$(grep -n 'cpu_cli();' kernel/src/kernel.c | head -n1 |
    cut -d: -f1 || true)
cpu_sti_line=$(grep -n 'cpu_sti();' kernel/src/kernel.c | head -n1 |
    cut -d: -f1 || true)
timer_init_line=$(grep -n 'timer_init(HFOC_TIMER_HZ);' kernel/src/kernel.c |
    head -n1 | cut -d: -f1 || true)
log_pic_masks_line=$(grep -n 'log_pic_masks();' kernel/src/kernel.c |
    head -n1 | cut -d: -f1 || true)
[[ -n "$irq0_route_line" && -n "$publish_line" && -n "$cpu_cli_line" &&
   -n "$cpu_sti_line" && -n "$timer_init_line" &&
   -n "$log_pic_masks_line" ]] || {
    echo "kernel.c не содержит все требуемые Stage 3b boot policy anchor lines" >&2
    exit 1
}
[[ "$publish_count" -eq 1 ]] || {
    echo "irq0_ioapic_active_publish(irq0_ready) должен вызываться ровно один раз" >&2
    exit 1
}
[[ "$cpu_cli_line" -lt "$irq0_route_line" &&
   "$irq1_route_line" -lt "$irq0_route_line" &&
   "$irq0_route_line" -lt "$keyboard_init_line" &&
   "$irq0_route_line" -lt "$timer_init_line" &&
   "$irq0_route_line" -lt "$publish_line" &&
   "$publish_line" -lt "$log_pic_masks_line" &&
   "$log_pic_masks_line" -lt "$cpu_sti_line" ]] || {
    echo "Stage 3b boot ordering нарушен: cpu_cli -> IRQ1 route -> IRQ0 route -> timer_init -> publish -> log -> cpu_sti" >&2
    exit 1
}
grep -Fq '[PIC ] IRQ1 masked; IOAPIC vector=0x41 active.' kernel/src/kernel.c || {
    echo "kernel.c не содержит exact success marker" >&2
    exit 1
}
grep -Fq '[PIC ] IRQ1 unmasked; legacy PIC fallback retained.' kernel/src/kernel.c || {
    echo "kernel.c не содержит exact fallback marker" >&2
    exit 1
}

pic_init_count=$(grep -F -o 'pic_init();' kernel/src/kernel.c | wc -l)
[[ "$pic_init_count" -eq 1 ]] || {
    echo "kernel.c вызывает pic_init() $pic_init_count раз(а), ожидался 1" >&2
    exit 1
}
if grep -nE '\bpic_(mask|unmask)\([2-9]U\)|\bpic_(mask|unmask)\(0x[2-9A-Fa-f]U\)' \
    kernel/src/kernel.c; then
    echo "kernel.c меняет PIC IRQ2+ маски" >&2
    exit 1
fi
if grep -nE '\b(pic_reprogram|pic_remap)\b|\bIMCR\b' \
    kernel/src/kernel.c kernel/src/arch/x86_64/interrupts.c kernel/src/ioapic.c; then
    echo "production code содержит PIC reprogram/remap/IMCR API" >&2
    exit 1
fi

FIXTURE_DIR=build/check-irq1-policy-fixtures
rm -rf "$FIXTURE_DIR"
mkdir -p "$FIXTURE_DIR"

cat >"$FIXTURE_DIR/production-good.c" <<'EOF'
void production_style(void) {
    const bool irq1_ready =
        irq1_route_status == IOAPIC_OK &&
        ioapic_is_irq1_routed();

    if (irq1_ready) {
        if (true) {
            pic_mask(1U);
        }
    } else {
        if (true) {
            pic_unmask(1U);
        }
    }
}
EOF

cat >"$FIXTURE_DIR/production-bad.c" <<'EOF'
void production_bad(void) {
    const bool irq1_ready =
        irq1_route_status == IOAPIC_OK &&
        ioapic_is_irq1_routed();

    if (irq1_ready) {
        if (true) {
            pic_mask(1U);
        }
        pic_unmask(1U);
    } else {
        if (true) {
            pic_unmask(1U);
        }
        pic_mask(1U);
    }
}
EOF

cat >"$FIXTURE_DIR/multiline-good.c" <<'EOF'
void multiline_style(void) {
    const bool irq1_ready =
        irq1_route_status == IOAPIC_OK &&
        ioapic_is_irq1_routed();

    if (irq1_ready)
    {
        if (true)
        {
            pic_mask(1U);
        }
    }
    else
    {
        if (true)
        {
            pic_unmask(1U);
        }
    }
}
EOF

cat >"$FIXTURE_DIR/multiline-bad.c" <<'EOF'
void multiline_bad(void) {
    const bool irq1_ready =
        irq1_route_status == IOAPIC_OK &&
        ioapic_is_irq1_routed();

    if (irq1_ready)
    {
        if (true)
        {
            pic_mask(1U);
        }
        pic_unmask(1U);
    }
    else
    {
        if (true)
        {
            pic_unmask(1U);
        }
        pic_mask(1U);
    }
}
EOF

check_irq1_policy_fixture() {
    local fixture_file=$1
    local label=$2
    local initializer
    local if_block
    local else_block

    initializer=$(extract_irq1_initializer "$fixture_file")
    if_block=$(extract_irq1_branch if "$fixture_file")
    else_block=$(extract_irq1_branch else "$fixture_file")
    if [[ -z "$initializer" || -z "$if_block" || -z "$else_block" ]]; then
        echo "$label: fixture extractor вернул пустой результат" >&2
        exit 1
    fi
    if ! printf '%s' "$initializer" |
        grep -Fq 'irq1_route_status == IOAPIC_OK && ioapic_is_irq1_routed()'; then
        echo "$label: fixture initializer conjunction не найден" >&2
        exit 1
    fi
    if ! printf '%s' "$if_block" | grep -Fq 'pic_mask(1U)' ||
        printf '%s' "$if_block" | grep -Fq 'pic_unmask(1U)'; then
        echo "$label: success branch fixture не корректен" >&2
        exit 1
    fi
    if ! printf '%s' "$else_block" | grep -Fq 'pic_unmask(1U)' ||
        printf '%s' "$else_block" | grep -Fq 'pic_mask(1U)'; then
        echo "$label: fallback branch fixture не корректен" >&2
        exit 1
    fi
    printf 'IRQ1 policy fixture %s: OK\n' "$label"
}

check_irq1_policy_forbidden() {
    local fixture_file=$1
    local label=$2
    local if_block
    local else_block

    if_block=$(extract_irq1_branch if "$fixture_file")
    else_block=$(extract_irq1_branch else "$fixture_file")
    if ! printf '%s' "$if_block" | grep -Fq 'pic_unmask(1U)'; then
        echo "$label: extractor не обнаружил forbidden pic_unmask(1U)" >&2
        exit 1
    fi
    if ! printf '%s' "$else_block" | grep -Fq 'pic_mask(1U)'; then
        echo "$label: extractor не обнаружил forbidden pic_mask(1U)" >&2
        exit 1
    fi
    printf 'IRQ1 policy fixture %s forbidden detection: OK\n' "$label"
}

check_irq1_policy_fixture "$FIXTURE_DIR/production-good.c" "production-style"
check_irq1_policy_fixture "$FIXTURE_DIR/multiline-good.c" "multiline else"
check_irq1_policy_forbidden "$FIXTURE_DIR/production-bad.c" "production-style"
check_irq1_policy_forbidden "$FIXTURE_DIR/multiline-bad.c" "multiline else"
rm -rf "$FIXTURE_DIR"
