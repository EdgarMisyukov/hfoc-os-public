#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Bounded kernel heap contracts.
grep -Fq 'HFOC_HEAP_SIZE' kernel/include/heap.h || {
    echo "heap.h не содержит HFOC_HEAP_SIZE" >&2; exit 1; }
for heap_api in \
    'bool heap_init(uint64_t hhdm_offset);' \
    'bool heap_ready(void);' \
    'void *kmalloc(size_t size);' \
    'bool kfree(void *ptr);' \
    'size_t heap_capacity_bytes(void);' \
    'size_t heap_used_bytes(void);'; do
    grep -Fq "$heap_api" kernel/include/heap.h || {
        echo "heap.h не содержит API: $heap_api" >&2
        exit 1
    }
done
for heap_symbol in heap_init heap_ready kmalloc kfree \
                   heap_capacity_bytes heap_used_bytes; do
    nm build/hfoc-kernel.elf | grep -F " $heap_symbol" >/dev/null || {
        echo "В ELF отсутствует символ: $heap_symbol" >&2
        exit 1
    }
done
if nm -g build/hfoc-kernel.elf | grep -E 'heap_test_|host_arena'; then
    echo "HEAP host test symbols попали в kernel ELF" >&2
    exit 1
fi
! grep -Fq 'kernel_panic' kernel/src/heap.c || {
    echo "heap.c не должен вызывать kernel_panic" >&2
    exit 1
}
grep -Fq '#include "heap.h"' kernel/src/console.c || {
    echo "console.c не включает heap.h" >&2; exit 1; }
grep -Fq 'console_command_is(input_line, "heap")' kernel/src/console.c || {
    echo "console.c не содержит команду heap" >&2
    exit 1
}
grep -Fq '[HEAP] ready=' kernel/src/console.c || {
    echo "console.c не содержит HEAP диагностику ready" >&2
    exit 1
}
grep -Fq 'capacity=' kernel/src/console.c || {
    echo "console.c не содержит HEAP диагностику capacity" >&2
    exit 1
}
grep -Fq 'used=' kernel/src/console.c || {
    echo "console.c не содержит HEAP диагностику used" >&2
    exit 1
}
grep -Fq '#include "heap.h"' kernel/src/kernel.c || {
    echo "kernel.c не включает heap.h" >&2
    exit 1
}
grep -Fq 'heap_init(hhdm_request.response->offset)' \
    kernel/src/kernel.c || {
    echo "kernel.c не вызывает heap_init с HHDM offset" >&2
    exit 1
}
heap_init_line=$(grep -n 'heap_init(' kernel/src/kernel.c |
    head -n1 | cut -d: -f1 || true)
vmm_init_line=$(grep -n 'vmm_init(' kernel/src/kernel.c |
    head -n1 | cut -d: -f1 || true)
console_init_line=$(grep -n 'console_init(' kernel/src/kernel.c |
    head -n1 | cut -d: -f1 || true)
[[ -n "$heap_init_line" && -n "$vmm_init_line" &&
   -n "$console_init_line" &&
   "$vmm_init_line" -lt "$heap_init_line" &&
   "$heap_init_line" -lt "$console_init_line" ]] || {
    echo "heap_init должен вызываться после vmm_init и до console_init" >&2
    exit 1
}
grep -Fq 'test-heap-host' Makefile || {
    echo "Makefile не содержит test-heap-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-heap-host' Makefile || {
    echo "test: target не содержит test-heap-host" >&2; exit 1; }
grep -Fq 'test-heap' Makefile || {
    echo "Makefile не содержит test-heap" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-heap' Makefile || {
    echo "test: target не содержит test-heap" >&2; exit 1; }
if [[ "$(grep -c '^test-heap:' Makefile || true)" -ne 1 ]]; then
    echo "Ожидался ровно один test-heap target" >&2
    exit 1
fi
heap_target_block=$(sed -n '/^test-heap:/,/^[^[:space:]]/p' Makefile)
grep -Fq "QEMU_MACHINE='q35'" <<<"$heap_target_block" || {
    echo "test-heap должен использовать QEMU_MACHINE='q35'" >&2
    exit 1
}
grep -Fq "QEMU_SMP='1'" <<<"$heap_target_block" || {
    echo "test-heap должен использовать QEMU_SMP='1'" >&2
    exit 1
}
if grep -Eq 'QEMU_CPU=.*(max|-apic|-no-apic)|-cpu[[:space:]]+(max|-apic|-no-apic)' \
    <<<"$heap_target_block"; then
    echo "test-heap должен не задавать QEMU_CPU" >&2
    exit 1
fi
bash -n scripts/test-qemu-heap.sh || {
    echo "Синтаксис scripts/test-qemu-heap.sh некорректен" >&2
    exit 1
}
grep -Fq 'QEMU APIC baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-heap.sh || {
    echo "test-qemu-heap.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU HEAP test: OK' scripts/test-qemu-heap.sh || {
    echo "test-qemu-heap.sh не содержит success marker" >&2
    exit 1
}
for heap_strict_symbol in mktemp timeout send_monitor_quit stop_qemu cleanup \
                          no-reboot no-shutdown; do
    grep -Fq "$heap_strict_symbol" scripts/test-qemu-heap.sh || {
        echo "test-qemu-heap.sh не содержит $heap_strict_symbol" >&2
        exit 1
    }
done

# Stage 3a Task 1 host-only boot-policy seam.
grep -Fq -- '-DHFOC_PIC_IRQ1_BOOT_POLICY_HOST_TEST' \
    scripts/test-pic-spurious.sh || {
    echo "test-pic-spurious.sh не определяет HFOC_PIC_IRQ1_BOOT_POLICY_HOST_TEST" >&2
    exit 1
}
grep -Fq '#ifdef HFOC_PIC_IRQ1_BOOT_POLICY_HOST_TEST' \
    kernel/src/arch/x86_64/pic.c || {
    echo "pic.c не содержит macro-gated host boot-policy seam" >&2
    exit 1
}
grep -Fq 'pic_irq1_boot_policy_host_test_apply' \
    kernel/src/arch/x86_64/pic.c || {
    echo "pic.c не содержит pic_irq1_boot_policy_host_test_apply" >&2
    exit 1
}
grep -Fq 'bool pic_irq1_boot_policy_host_test_apply(' \
    tests/pic-spurious-irqs.c || {
    echo "tests/pic-spurious-irqs.c не объявляет host boot-policy seam" >&2
    exit 1
}
for policy_name in 'run_irq1_boot_policy_case' 'test_irq1_boot_policy' \
                   'IRQ1 success transition' 'IRQ1 success idempotence' \
                   'IRQ1 fallback transition' \
                   'IRQ1 status OK but not routed' \
                   'IRQ1 route failure but routed true' \
                   'IRQ1 fallback idempotence'; do
    grep -Fq "$policy_name" tests/pic-spurious-irqs.c || {
        echo "tests/pic-spurious-irqs.c не содержит $policy_name" >&2
        exit 1
    }
done
if nm -g build/hfoc-kernel.elf | grep -F ' pic_irq1_boot_policy_host_test'; then
    echo "pic_irq1_boot_policy_host_test попал в kernel ELF" >&2
    exit 1
fi
grep -Fq 'bool pic_irq0_boot_policy_host_test_apply(' \
    kernel/src/arch/x86_64/pic.c || {
    echo "pic.c не содержит pic_irq0_boot_policy_host_test_apply" >&2
    exit 1
}
grep -Fq 'bool pic_irq01_boot_policy_host_test_apply(' \
    kernel/src/arch/x86_64/pic.c || {
    echo "pic.c не содержит pic_irq01_boot_policy_host_test_apply" >&2
    exit 1
}
grep -Fq 'bool pic_irq0_boot_policy_host_test_apply(' \
    tests/pic-spurious-irqs.c || {
    echo "tests/pic-spurious-irqs.c не объявляет pic_irq0_boot_policy_host_test_apply" >&2
    exit 1
}
grep -Fq 'bool pic_irq01_boot_policy_host_test_apply(' \
    tests/pic-spurious-irqs.c || {
    echo "tests/pic-spurious-irqs.c не объявляет pic_irq01_boot_policy_host_test_apply" >&2
    exit 1
}
for irq0_policy_name in 'test_irq0_boot_policy' 'test_irq01_boot_policy' \
                        'run_irq0_boot_policy_case' \
                        'run_irq01_boot_policy_case' \
                        'IRQ0 boot policy success' \
                        'IRQ0 boot policy status OK but not routed' \
                        'IRQ0 boot policy route failure' \
                        'IRQ01 boot policy OK OK true true' \
                        'IRQ01 boot policy OK OK true false' \
                        'IRQ01 boot policy OK fail true false' \
                        'IRQ01 boot policy fail OK false false' \
                        'IRQ01 boot policy fail OK false true' \
                        'IRQ01 boot policy OK OK false false' \
                        'IRQ01 boot policy fail fail false false'; do
    grep -Fq "$irq0_policy_name" tests/pic-spurious-irqs.c || {
        echo "tests/pic-spurious-irqs.c не содержит $irq0_policy_name" >&2
        exit 1
    }
done
if nm -g build/hfoc-kernel.elf |
    grep -E ' pic_irq0_boot_policy_host_test| pic_irq01_boot_policy_host_test'; then
    echo "Stage 3b boot-policy host seam попал в kernel ELF" >&2
    exit 1
fi

# Stage 3b Task 1 host-only ownership state scaffold.
grep -Fq -- '-DHFOC_IRQ0_OWNERSHIP_HOST_TEST' \
    scripts/test-pic-spurious.sh || {
    echo "test-pic-spurious.sh не определяет HFOC_IRQ0_OWNERSHIP_HOST_TEST" >&2
    exit 1
}
grep -A1 '^#define HFOC_ARCH_X86_64_INTERRUPTS_H$' \
    kernel/include/arch/x86_64/interrupts.h | grep -Fq '#include <stdbool.h>' || {
    echo "interrupts.h не содержит self-contained <stdbool.h> после include guard" >&2
    exit 1
}
grep -Fq 'void irq0_ioapic_active_publish(bool active);' \
    kernel/include/arch/x86_64/interrupts.h || {
    echo "interrupts.h не содержит irq0_ioapic_active_publish" >&2
    exit 1
}
grep -Fq '#ifdef HFOC_IRQ0_OWNERSHIP_HOST_TEST' \
    kernel/src/arch/x86_64/interrupts.c || {
    echo "interrupts.c не содержит macro-gated host ownership seam" >&2
    exit 1
}
for ownership_seam in 'irq0_ioapic_active_host_test_reset' \
                      'irq0_ioapic_active_host_test_is_active' \
                      'irq0_stray_pic_count_host_test_value'; do
    grep -Fq "$ownership_seam" kernel/src/arch/x86_64/interrupts.c || {
        echo "interrupts.c не содержит $ownership_seam" >&2
        exit 1
    }
done
for ownership_test in 'void irq0_ioapic_active_publish(bool active);' \
                      'void irq0_ioapic_active_host_test_reset(void);' \
                      'bool irq0_ioapic_active_host_test_is_active(void);' \
                      'uint64_t irq0_stray_pic_count_host_test_value(void);' \
                      'test_irq0_ownership_host_test' \
                      'IRQ0 ownership initial false' \
                      'IRQ0 ownership publish true' \
                      'IRQ0 ownership publish false' \
                      'IRQ0 ownership host reset'; do
    grep -Fq "$ownership_test" tests/pic-spurious-irqs.c || {
        echo "tests/pic-spurious-irqs.c не содержит $ownership_test" >&2
        exit 1
    }
done
if ! printf '#include "arch/x86_64/interrupts.h"\nvoid hfoc_self_contained_interrupts_header(void) { irq0_ioapic_active_publish(false); }\n' |
    gcc -std=c11 -Wall -Wextra -Werror -Wpedantic -ffreestanding \
        -Ikernel/include -x c -c - -o /dev/null; then
    echo "interrupts.h не компилируется как self-contained header" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep -F ' irq0_ioapic_active_host_test_'; then
    echo "irq0_ioapic_active_host_test_ попал в kernel ELF" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep -F ' irq0_stray_pic_count_host_test_value'; then
    echo "irq0_stray_pic_count_host_test_value попал в kernel ELF" >&2
    exit 1
fi

elf_info=$(file build/hfoc-kernel.elf)
[[ "$elf_info" == *"ELF 64-bit LSB executable, x86-64"* ]] || {
    echo "Ядро не является ELF64 x86-64: $elf_info" >&2
    exit 1
}

readelf -h build/hfoc-kernel.elf | grep -Fq 'Entry point address:'
readelf -SW build/hfoc-kernel.elf | grep -Fq '.limine_requests'

symbols=$(nm -g build/hfoc-kernel.elf)
for symbol in kmain gdt_init gdt_init_for gdt_load_for gdt_load_tr_for \
              interrupts_init interrupt_dispatch isr_stub_table \
              timer_handle_irq keyboard_handle_irq kernel_panic \
              console_process_input pic_master_mask pic_slave_mask \
              tss_init tss_init_for tss_ist1_stack \
              tss_ist2_stack tss_ist2_stack_size tss_set_rsp0 tss_rsp0 \
              user_iret_frame_init user_iret_frame_valid user_enter_iretq \
              cpu_init_bsp cpu_init_ap cpu_current cpu_by_index cpu_gs_base_set \
              smp_init smp_ap_entry smp_online smp_online_ap_count \
              smp_ap_idle smp_status_string lapic_init_ap \
              interrupts_make_pf_gate_not_present; do
    grep -Eq "[[:space:]]${symbol}$" <<<"$symbols" || {
        echo "В ELF отсутствует символ: $symbol" >&2
        exit 1
    }
done

stub_count=$(grep -c '^isr_stub_[0-9]\+:$' kernel/src/arch/x86_64/interrupt_stubs.S)
[[ "$stub_count" -eq 256 ]] || {
    echo "Ожидалось 256 ISR-заглушек, найдено: $stub_count" >&2
    exit 1
}

grep -Fq 'pic_unmask(0U);' kernel/src/kernel.c
grep -Fq 'pic_unmask(1U);' kernel/src/kernel.c
grep -Fq 'cpu_sti()' kernel/src/kernel.c
grep -Fq 'cpu_hlt();' kernel/src/kernel.c
grep -Fq 'PS2_CFG_IRQ1 | PS2_CFG_TRANSLATION' kernel/src/keyboard.c
grep -Fq 'KBD_CMD_ENABLE_SCAN' kernel/src/keyboard.c
! grep -Fq 'keyboard_poll()' kernel/src/kernel.c || {
    echo "0.2.6 не должна использовать polling-костыль в основном цикле" >&2
    exit 1
}
! grep -Eq 'keyboard_poll|keyboard_poll_count|poll_count' kernel/src/keyboard.c kernel/include/keyboard.h || {
    echo "В 0.2.6 остался код polling-клавиатуры" >&2
    exit 1
}

# ITEM-392: a machine with no usable i8042 boots. Отказ клавиатуры больше не
# фатален, политика загрузки одна для всех сборок, а тестовая лазейка
# HFOC_PHYSICAL_STORAGE_QEMU_TEST больше не обходит инициализацию клавиатуры.
grep -Fq 'keyboard_init_status_boots(keyboard_status)' kernel/src/kernel.c || {
    echo "kernel.c не публикует политику загрузки без клавиатуры" >&2
    exit 1
}
if grep -Fq 'kernel_panic("PS/2 keyboard initialisation failed")' \
    kernel/src/kernel.c; then
    echo "kernel.c всё ещё считает отказ клавиатуры фатальным без diagnostic" >&2
    exit 1
fi
grep -Fq 'kernel_panic_detail("PS/2 keyboard initialisation failed",' \
    kernel/src/kernel.c || {
    echo "kernel.c не сохраняет диагностику отказа клавиатуры" >&2
    exit 1
}
if grep -Fq 'keyboard_ready' kernel/src/kernel.c; then
    echo "kernel.c содержит прежний флаг keyboard_ready" >&2
    exit 1
fi
if grep -Fq 'continuing storage proof without keyboard input' \
    kernel/src/kernel.c; then
    echo "kernel.c содержит тестовую лазейку обхода клавиатуры" >&2
    exit 1
fi
grep -Fq 'serial_write("[KBD ] state=");' kernel/src/kernel.c || {
    echo "kernel.c не публикует состояние клавиатуры" >&2
    exit 1
}
grep -Fq 'serial_write(" local_input=");' kernel/src/kernel.c || {
    echo "запись APPLIANCE-BOOT не публикует локальный ввод" >&2
    exit 1
}
grep -Fq 'console_publish_local_input(local_input_available);' \
    kernel/src/kernel.c || {
    echo "kernel.c не публикует локальный ввод в консоль" >&2
    exit 1
}
for keyboard_failure in \
    KEYBOARD_INIT_CONTROLLER_ABSENT \
    KEYBOARD_INIT_CONTROLLER_STUCK \
    KEYBOARD_INIT_CONFIG_REJECTED \
    KEYBOARD_INIT_KEYBOARD_ABSENT; do
    grep -Fq "$keyboard_failure" kernel/include/keyboard.h || {
        echo "keyboard.h не содержит статус $keyboard_failure" >&2
        exit 1
    }
    grep -Fq "$keyboard_failure" kernel/src/keyboard.c || {
        echo "keyboard.c не сообщает статус $keyboard_failure" >&2
        exit 1
    }
done
grep -Fq 'keyboard_init_status_boots' kernel/include/keyboard.h || {
    echo "keyboard.h не публикует политику загрузки без клавиатуры" >&2
    exit 1
}
grep -Fq 'i8042=off' scripts/test-qemu-production-storage-boot.sh || {
    echo "QEMU-доказательство не убирает контроллер PS/2" >&2
    exit 1
}
grep -Fq 'local_input=UNAVAILABLE' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "QEMU-доказательство не требует local_input=UNAVAILABLE" >&2
    exit 1
}
test -f tests/keyboard-init-host.c || {
    echo "Отсутствует tests/keyboard-init-host.c" >&2
    exit 1
}
grep -Fq 'test-keyboard-init-host' Makefile || {
    echo "Makefile не содержит test-keyboard-init-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-keyboard-init-host' Makefile || {
    echo "test-host-suite-serial не вызывает test-keyboard-init-host" >&2
    exit 1
}
grep -Fq 'test-keyboard-init-host' tests/test-host-suite-manifest.sh || {
    echo "манифест host suite не содержит test-keyboard-init-host" >&2
    exit 1
}

# ITEM-395: машина без PS/2 получает локальный ввод с USB HID клавиатуры за
# xHCI. Контроллер поднимается опросом из обычного цикла загрузки (APIC/IOAPIC
# и маршрутизация IRQ не тронуты), ввод обязан публиковаться честно, а два
# источника не имеют права мешать друг другу.
grep -Fq 'xhci_keyboard_init(hhdm_request.response->offset)' kernel/src/kernel.c || {
    echo "kernel.c не поднимает xHCI/USB HID путь ввода" >&2
    exit 1
}
grep -Fq 'local_input_service();' kernel/src/kernel.c || {
    echo "kernel.c не обслуживает объединённый локальный ввод" >&2
    exit 1
}
grep -Fq 'console_publish_input_source(local_input_source_now);' \
    kernel/src/kernel.c || {
    echo "kernel.c не публикует источник локального ввода в консоль" >&2
    exit 1
}
grep -Fq 'serial_write(" source=");' kernel/src/kernel.c || {
    echo "запись [KBD ] не публикует источник локального ввода" >&2
    exit 1
}
grep -Fq 'serial_write(" input_source=");' kernel/src/kernel.c || {
    echo "запись APPLIANCE-BOOT не публикует источник локального ввода" >&2
    exit 1
}
grep -Fq 'LOCAL INPUT: ENABLED (USB HID KEYBOARD)' kernel/src/console.c || {
    echo "консоль не сообщает о работе через USB HID клавиатуру" >&2
    exit 1
}
grep -Fq 'xHCI (USB 3)' kernel/include/xhci.h || {
    echo "xhci.h не описывает область драйвера" >&2
    exit 1
}
if grep -Eq 'keyboard_poll|xhci_poll_count' kernel/src/xhci.c \
    kernel/src/local_input.c kernel/include/xhci.h; then
    echo "драйвер xHCI содержит polling-счётчик вместо опроса по циклу" >&2
    exit 1
fi
grep -Fq 'qemu-xhci,id=xhci' scripts/test-qemu-production-storage-boot.sh || {
    echo "QEMU-доказательство не добавляет xHCI контроллер" >&2
    exit 1
}
grep -Fq 'usb-kbd,bus=xhci.0' scripts/test-qemu-production-storage-boot.sh || {
    echo "QEMU-доказательство не привязывает клавиатуру к шине xHCI" >&2
    exit 1
}
grep -Fq 'source=NONE ps2=CONTROLLER_ABSENT usb=CONTROLLER_ABSENT' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "QEMU-доказательство не требует source=NONE без клавиатуры" >&2
    exit 1
}
grep -Fq 'COMMANDS: HELP CLEAR SCROLL VERSION UPTIME TICKS IRQ KBD' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "QEMU-доказательство не проверяет команду, набранную с USB клавиатуры" >&2
    exit 1
}
for usb_input_proof in test-usb-hid-host test-xhci-host test-local-input-host; do
    grep -Fq "${usb_input_proof}:" Makefile || {
        echo "Makefile не содержит ${usb_input_proof}" >&2
        exit 1
    }
    grep -Fq "\$(MAKE) ${usb_input_proof}" Makefile || {
        echo "test-host-suite-serial не вызывает ${usb_input_proof}" >&2
        exit 1
    }
    grep -Fq "${usb_input_proof}" tests/test-host-suite-manifest.sh || {
        echo "манифест host suite не содержит ${usb_input_proof}" >&2
        exit 1
    }
done
test -f tests/usb-hid-host.c || {
    echo "Отсутствует tests/usb-hid-host.c" >&2
    exit 1
}
test -f tests/xhci-host.c || {
    echo "Отсутствует tests/xhci-host.c" >&2
    exit 1
}
test -f tests/local-input-host.c || {
    echo "Отсутствует tests/local-input-host.c" >&2
    exit 1
}

grep -Fq 'QEMU_MACHINE ?= q35' Makefile
grep -Fq 'QEMU_CPU ?= max,-apic' Makefile
grep -Fq 'grab-on-hover=on' Makefile
# The literal ${VERSION} is the text that run-uefi.sh must contain.
# shellcheck disable=SC2016
grep -Fq 'build/hfoc-os-${VERSION}.iso' scripts/run-uefi.sh
grep -Fq -- '-cpu max,-apic' scripts/run-uefi.sh

# QEMU xAPIC baseline: APIC proofs must run on q35 / one vCPU with QEMU's
# default xAPIC-capable CPU configuration. The guard is scoped to APIC proof
# entry points and deliberately ignores legacy-only interactive/test profiles.
apic_qemu_proof_scripts=(
    scripts/test-qemu-lapic.sh
    scripts/test-qemu-ioapic-probe.sh
    scripts/test-qemu-ioapic-irq0.sh
    scripts/test-qemu-ioapic-irq1.sh
    scripts/test-qemu-lapic-timer-calibration.sh
    scripts/test-qemu-lapic-timer-runtime.sh
    scripts/test-qemu-pic-irq0-masking.sh
    scripts/test-qemu-pic-irq0-masking-fallback.sh
    scripts/test-qemu-keyboard-line-editing.sh
    scripts/test-qemu-pic-disable.sh
)
for apic_script in "${apic_qemu_proof_scripts[@]}"; do
    if grep -vE '^[[:space:]]*#' "$apic_script" | grep -Eq -- \
        '-cpu[[:space:]]+(max|max,-apic|-apic|-no-apic)' \
        ; then
        echo "FAIL: APIC QEMU proof still selects forbidden CPU override" >&2
        exit 1
    fi
done
for apic_target in \
    test-lapic test-ioapic test-ioapic-irq0 test-ioapic-irq1 \
    test-lapic-timer-calibration test-lapic-timer-runtime \
    test-pic-irq0-masking test-pic-irq0-masking-fallback \
    test-keyboard-line-editing test-pic-disable; do
    apic_target_block=$(sed -n "/^${apic_target}:/,/^[^[:space:]]/p" Makefile)
    if grep -Eq -- \
        'QEMU_CPU=.*(max|-apic|-no-apic)|-cpu[[:space:]]+(max|-apic|-no-apic)' \
        <<<"$apic_target_block"; then
        echo "FAIL: APIC QEMU proof still selects forbidden CPU override" >&2
        exit 1
    fi
done

apic_profile_contract='Portable QEMU xAPIC baseline: q35, one vCPU, QEMU default xAPIC-capable CPU; -cpu max,-apic is not required.'
for apic_profile_doc in README.md docs/ARCHITECTURE.md; do
    grep -Fq "$apic_profile_contract" "$apic_profile_doc" || {
        echo "$apic_profile_doc не документирует portable QEMU xAPIC baseline" >&2
        exit 1
    }
done
grep -Fq 'portable QEMU xAPIC baseline' docs/ROADMAP.md || {
    echo "ROADMAP.md не документирует portable QEMU xAPIC baseline" >&2
    exit 1
}

gdt_symbol_type=$(nm -n build/hfoc-kernel.elf | awk '$3 == "cpus" {print $2}')
[[ "$gdt_symbol_type" == "b" || "$gdt_symbol_type" == "B" ]] || {
    echo "Per-CPU GDT storage (cpus) должна находиться в записываемой секции .bss, тип символа: ${gdt_symbol_type:-не найден}" >&2
    exit 1
}

grep -Fq '0x00af9b000000ffffULL' kernel/src/arch/x86_64/gdt.c
grep -Fq '0x00cf93000000ffffULL' kernel/src/arch/x86_64/gdt.c

# Verify TSS selector constant and TR read-back.
grep -Fq 'GDT_TSS_SELECTOR' kernel/include/arch/x86_64/gdt.h
grep -Fq 'gdt_read_tr' kernel/include/arch/x86_64/gdt.h

# TSS and IST checks.
grep -Fq '_Static_assert(sizeof(struct tss64) == 104U' kernel/src/arch/x86_64/tss.c
grep -Fq 'offsetof(struct tss64, io_map_base) == 102U' kernel/src/arch/x86_64/tss.c
grep -Fq 'sizeof(struct tss64)' kernel/src/arch/x86_64/gdt.c
grep -Fq '0x28' kernel/src/arch/x86_64/gdt.c
grep -Fq 'ist1_stack[IST_STACK_SIZE]' kernel/include/cpu.h
grep -Eq '#define[[:space:]]+IST_STACK_SIZE[[:space:]]+16384' kernel/include/cpu.h
grep -Fq 'cpu->tss.io_map_base = (uint16_t)sizeof(struct tss64)' \
    kernel/src/arch/x86_64/tss.c
grep -Fq 'gdt_load_tr' kernel/src/arch/x86_64/gdt.c
grep -Fq 'ltr' kernel/src/arch/x86_64/gdt.c
grep -Fq 'str' kernel/src/arch/x86_64/gdt.c
grep -Fq 'TR selector mismatch' kernel/src/arch/x86_64/gdt.c
grep -Fq 'ist = (vector == 2U) ? 2U : (vector == 8U) ? 1U : 0U' kernel/src/arch/x86_64/interrupts.c || {
    echo "Отсутствует IST mapping vector 2->2, 8->1" >&2; exit 1; }
grep -Fq '"nmi: ist=2 stack=ok\n"' kernel/src/arch/x86_64/interrupts.c || {
    echo "Отсутствует nmi: ist=2 stack=ok" >&2; exit 1; }
grep -Fq '"nmi: ist=2 stack=bad\n"' kernel/src/arch/x86_64/interrupts.c || {
    echo "Отсутствует nmi: ist=2 stack=bad" >&2; exit 1; }
grep -Fq 'stack_in_range' kernel/src/arch/x86_64/interrupts.c || {
    echo "Отсутствует stack_in_range" >&2; exit 1; }
grep -Fq 'double-fault: ist=' kernel/src/arch/x86_64/interrupts.c
grep -Fq 'interrupts_make_pf_gate_not_present' kernel/src/console.c
grep -Fq 'DOUBLE FAULT IST ENABLED' kernel/src/console.c

# Verify TSS descriptor occupies two 8-byte entries at GDT index 5.
grep -Fq 'uint64_t gdt[7];' kernel/include/cpu.h
grep -Fq '&cpu->gdt[5]' kernel/src/arch/x86_64/gdt.c

# Per-CPU state and host test contracts.
grep -Fq 'test-cpu-host' Makefile || {
    echo "Отсутствует test-cpu-host в Makefile" >&2; exit 1; }
grep -Fq 'HFOC_CPU_HOST_TEST' Makefile || {
    echo "Makefile не определяет HFOC_CPU_HOST_TEST" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-cpu-host' Makefile || {
    echo "test: target не содержит test-cpu-host" >&2; exit 1; }
grep -Fq 'cpu_init_bsp' kernel/include/cpu.h || {
    echo "cpu.h не содержит cpu_init_bsp" >&2; exit 1; }
grep -Fq 'cpu_init_ap' kernel/include/cpu.h || {
    echo "cpu.h не содержит cpu_init_ap" >&2; exit 1; }
grep -Fq 'cpu_current' kernel/src/cpu.c || {
    echo "cpu.c не содержит cpu_current" >&2; exit 1; }
grep -Fq 'cpu_gs_base_set' kernel/src/cpu.c || {
    echo "cpu.c не содержит cpu_gs_base_set" >&2; exit 1; }
grep -Fq 'gdt_init_for' kernel/include/arch/x86_64/gdt.h || {
    echo "gdt.h не содержит gdt_init_for" >&2; exit 1; }
grep -Fq 'gdt_load_for' kernel/include/arch/x86_64/gdt.h || {
    echo "gdt.h не содержит gdt_load_for" >&2; exit 1; }
grep -Fq 'gdt_load_tr_for' kernel/include/arch/x86_64/gdt.h || {
    echo "gdt.h не содержит gdt_load_tr_for" >&2; exit 1; }
grep -Fq 'tss_init_for' kernel/include/arch/x86_64/tss.h || {
    echo "tss.h не содержит tss_init_for" >&2; exit 1; }
grep -Fq 'void tss_set_rsp0(uint64_t rsp0);' \
    kernel/include/arch/x86_64/tss.h || {
    echo "tss.h не содержит tss_set_rsp0" >&2; exit 1; }
grep -Fq 'uint64_t tss_rsp0(void);' \
    kernel/include/arch/x86_64/tss.h || {
    echo "tss.h не содержит tss_rsp0" >&2; exit 1; }
grep -Fq 'cpu_gs_base_set' kernel/src/kernel.c || {
    echo "kernel.c не вызывает cpu_gs_base_set" >&2; exit 1; }
grep -Fq 'write_gs_base((uintptr_t)&cpu->self);' kernel/src/cpu.c || {
    echo "cpu.c не публикует cpu->self через GS offset 0" >&2; exit 1; }
gdt_reload_line=$(grep -n 'gdt_reload_segments();' kernel/src/kernel.c |
    head -n1 | cut -d: -f1 || true)
gs_base_line=$(grep -n 'cpu_gs_base_set(bsp);' kernel/src/kernel.c |
    head -n1 | cut -d: -f1 || true)
[[ -n "$gdt_reload_line" && -n "$gs_base_line" &&
   "$gs_base_line" -gt "$gdt_reload_line" ]] || {
    echo "cpu_gs_base_set должен вызываться после gdt_reload_segments" >&2
    exit 1
}
if nm -g build/hfoc-kernel.elf | grep -E 'cpu_host_test_|host_cpu'; then
    echo "CPU host test symbols попали в kernel ELF" >&2
    exit 1
fi

# SMP MP request and AP startup contracts.
grep -Fq 'LIMINE_MP_REQUEST_ID' kernel/include/limine_min.h || {
    echo "limine_min.h не содержит LIMINE_MP_REQUEST_ID" >&2; exit 1; }
grep -Fq 'struct limine_mp_info' kernel/include/limine_min.h || {
    echo "limine_min.h не содержит struct limine_mp_info" >&2; exit 1; }
grep -Fq 'struct limine_mp_response' kernel/include/limine_min.h || {
    echo "limine_min.h не содержит struct limine_mp_response" >&2; exit 1; }
grep -Fq 'mp_request' kernel/src/kernel.c || {
    echo "kernel.c не содержит Limine MP request" >&2; exit 1; }
mp_request_block=$(sed -n \
    '/static volatile struct limine_mp_request mp_request/,/^};/p' \
    kernel/src/kernel.c)
grep -Fq '.flags = 0' <<<"$mp_request_block" || {
    echo "Limine MP request должен использовать flags = 0" >&2; exit 1; }
grep -Fq 'smp_init(' kernel/src/kernel.c || {
    echo "kernel.c не вызывает smp_init" >&2; exit 1; }
lapic_init_line=$(grep -n 'lapic_init(' kernel/src/kernel.c |
    head -n1 | cut -d: -f1 || true)
smp_init_line=$(grep -n 'smp_init(' kernel/src/kernel.c |
    head -n1 | cut -d: -f1 || true)
pic_init_line=$(grep -n 'pic_init();' kernel/src/kernel.c |
    head -n1 | cut -d: -f1 || true)
[[ -n "$lapic_init_line" && -n "$smp_init_line" &&
   -n "$pic_init_line" &&
   "$smp_init_line" -gt "$lapic_init_line" &&
   "$smp_init_line" -lt "$pic_init_line" ]] || {
    echo "smp_init должен вызываться после lapic_init и до pic_init" >&2
    exit 1
}
grep -Fq 'test-smp-host' Makefile || {
    echo "Отсутствует test-smp-host в Makefile" >&2; exit 1; }
grep -Fq 'HFOC_SMP_HOST_TEST' Makefile || {
    echo "Makefile не определяет HFOC_SMP_HOST_TEST" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-smp-host' Makefile || {
    echo "test: target не содержит test-smp-host" >&2; exit 1; }
grep -Fq 'smp_ap_entry' kernel/src/smp.c || {
    echo "smp.c не содержит smp_ap_entry" >&2; exit 1; }
grep -Fq 'void smp_ap_idle(void);' kernel/include/smp.h || {
    echo "smp.h не содержит smp_ap_idle" >&2; exit 1; }
grep -Fq 'smp_ap_idle();' kernel/src/smp.c || {
    echo "smp.c не вызывает smp_ap_idle после online publication" >&2; exit 1; }
ap_idle_block=$(sed -n '/^void smp_ap_idle(void) {/,/^}/p' \
    kernel/src/smp.c)
if grep -Eq '\b(sti|stgi)\b' <<<"$ap_idle_block"; then
    echo "smp_ap_idle не должен включать interrupts" >&2
    exit 1
fi
ap_vector_guard=$(sed -n \
    '/current != NULL && current->index != 0U/,/^    }/p' \
    kernel/src/arch/x86_64/interrupts.c)
grep -Fq 'BSP-owned handler not entered' <<<"$ap_vector_guard" || {
    echo "interrupts.c не отклоняет AP-owned vectors" >&2; exit 1; }
if grep -Eq 'timer_handle_irq|pic_(mask|unmask|send_eoi)|ioapic_' \
    <<<"$ap_vector_guard"; then
    echo "AP vector guard вызывает BSP-owned handlers" >&2
    exit 1
fi
grep -Fq 'goto_address' kernel/src/smp.c || {
    echo "smp.c не публикует goto_address" >&2; exit 1; }
grep -Fq 'reserved = (uint64_t)cpu->kernel_stack_top' kernel/src/smp.c || {
    echo "smp.c не публикует AP stack top в reserved" >&2; exit 1; }
grep -Fq 'lapic_init_ap' kernel/src/lapic.c || {
    echo "lapic.c не содержит lapic_init_ap" >&2; exit 1; }
if nm -g build/hfoc-kernel.elf | grep -E 'smp_test_|host_smp'; then
    echo "SMP host test symbols попали в kernel ELF" >&2
    exit 1
fi
grep -Fq 'test-smp' Makefile || {
    echo "Makefile не содержит test-smp" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-smp' Makefile || {
    echo "test: target не содержит test-smp" >&2; exit 1; }
smp_target_block=$(sed -n '/^test-smp:/,/^[^[:space:]]/p' Makefile)
grep -Fq "QEMU_MACHINE='q35'" <<<"$smp_target_block" || {
    echo "test-smp должен использовать QEMU_MACHINE='q35'" >&2; exit 1; }
grep -Fq "QEMU_SMP='2'" <<<"$smp_target_block" || {
    echo "test-smp должен использовать QEMU_SMP='2'" >&2; exit 1; }
grep -Fq "QEMU_CPU='max'" <<<"$smp_target_block" || {
    echo "test-smp должен использовать QEMU_CPU='max'" >&2; exit 1; }
if [[ "$(grep -c '^test-smp:' Makefile || true)" -ne 1 ]]; then
    echo "Ожидался ровно один test-smp target" >&2; exit 1
fi
grep -Fq 'QEMU SMP profile: machine=q35 smp=2 cpu=max' \
    scripts/test-qemu-smp.sh || {
    echo "test-qemu-smp.sh не фиксирует SMP profile" >&2; exit 1; }
grep -Fq 'QEMU SMP test: OK' scripts/test-qemu-smp.sh || {
    echo "test-qemu-smp.sh не содержит success marker" >&2; exit 1; }
bash -n scripts/test-qemu-smp.sh || {
    echo "Синтаксис scripts/test-qemu-smp.sh некорректен" >&2; exit 1; }
if grep -RInE 'apic_write32\(0x300|mmio_write32\(0x300|x2apic_write32\(0x300' \
    kernel/src; then
    echo "Kernel не должен писать LAPIC ICR напрямую" >&2
    exit 1
fi

# ITEM-420: the boot keyboard enumerated with the wrong command, and the record
# named the controller's answer with the wrong name.
#
# The acceptance host's K120 stopped at Evaluate Context, which this driver sent
# as an Address Device TRB with the Block Set Address bit set - a command the
# specification does not define - and the controller answered 19, a Context
# State Error.  The completion-code table was shifted two codes above 16, so the
# record published that 19 as `EVENT_RING_FULL` and sent the investigation after
# an event ring that was fine.  Both are pinned here: the driver asks for its own
# Evaluate Context command and never builds one out of Address Device plus BSR,
# and the names of the codes a controller returns are the ones xHCI 1.1 table
# 6-90 gives.
grep -Fq 'xhci_trb_evaluate_context(' kernel/src/xhci.c || {
    echo "драйвер не отправляет команду Evaluate Context" >&2
    exit 1
}
if grep -Fq 'XHCI_TRB_BSR' kernel/src/xhci.c; then
    echo "драйвер всё ещё собирает Evaluate Context из Address Device + BSR" >&2
    exit 1
fi
grep -Fq 'trb = xhci_trb_evaluate_context(1U' kernel/src/xhci.c || {
    echo "путь перечисления не использует новую команду" >&2
    exit 1
}
grep -Fq 'xhci_trb_type(&evaluate) == XHCI_TRB_EVALUATE_CONTEXT' \
    tests/xhci-host.c || {
    echo "host-тест не фиксирует тип TRB команды Evaluate Context" >&2
    exit 1
}
for xhci_completion_name in \
    '"CONTEXT_STATE_ERROR"' \
    '"EVENT_RING_FULL_ERROR"' \
    '"COMMAND_RING_STOPPED"'; do
    grep -Fq -- "$xhci_completion_name" kernel/src/xhci_trb.c || {
        echo "имя кода завершения не зафиксировано: $xhci_completion_name" >&2
        exit 1
    }
done
grep -Fq 'xhci_completion_code_string(19U), "CONTEXT_STATE_ERROR"' \
    tests/xhci-host.c || {
    echo "host-тест не фиксирует имя кода 19" >&2
    exit 1
}
