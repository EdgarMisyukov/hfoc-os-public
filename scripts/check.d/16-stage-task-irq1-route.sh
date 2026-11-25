#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Stage 3a Task 5: IRQ1 route-state and boot-policy acceptance.
if ! grep -Fq 'st == IOAPIC_OK && ioapic_is_irq1_routed()' \
    tests/ioapic-host.c; then
    echo "tests/ioapic-host.c не проверяет normalized irq1_ready initializer" >&2
    exit 1
fi
normalized_count=$(grep -c 'st == IOAPIC_OK && ioapic_is_irq1_routed()' \
    tests/ioapic-host.c || true)
[[ "$normalized_count" -ge 5 ]] || {
    echo "tests/ioapic-host.c содержит только $normalized_count проверок irq1_ready (ожидалось >= 5)" >&2
    exit 1
}
for acceptance_label in \
    'successful IRQ1 route sets irq1_routed for policy' \
    'invalid vector route leaves irq1_routed false for fallback policy' \
    'invalid saved IRQ1 descriptor leaves irq1_routed false for fallback policy' \
    'IRQ0 route must not publish irq1_routed'; do
    grep -Fq "$acceptance_label" tests/ioapic-host.c || {
        echo "tests/ioapic-host.c не содержит $acceptance_label" >&2
        exit 1
    }
done
irq1_mismatch_block=$(extract_balanced_block \
    'static void test_route_readback_mismatch_paths' tests/ioapic-host.c)
if [[ -z "$irq1_mismatch_block" ]]; then
    echo "tests/ioapic-host.c не содержит readback mismatch test block" >&2
    exit 1
fi

strip_c_comments() {
    local block=$1
    printf '%s\n' "$block" |
        awk '
            BEGIN { in_block_comment = 0 }
            {
                line = ""
                n = length($0)
                for (i = 1; i <= n; i++) {
                    ch = substr($0, i, 1)
                    next_ch = (i < n ? substr($0, i + 1, 1) : "")
                    if (in_block_comment) {
                        if (ch == "*" && next_ch == "/") {
                            in_block_comment = 0
                            i++
                        }
                        continue
                    }
                    if (ch == "/" && next_ch == "/") {
                        break
                    }
                    if (ch == "/" && next_ch == "*") {
                        in_block_comment = 1
                        i++
                        continue
                    }
                    line = line ch
                }
                print line
            }
        '
}

verify_irq1_direct_state_assertion() {
    local label=$1
    local block=$2
    local source_without_comments

    source_without_comments=$(strip_c_comments "$block")
    printf '%s\n' "$source_without_comments" |
        awk -v label="$label" '
            {
                source = source $0 " "
            }
            END {
                gsub(/[[:space:]]+/, " ", source)
                search_from = 1
                while (match(substr(source, search_from),
                    /check[[:space:]]*\([[:space:]]*![[:space:]]*ioapic_is_irq1_routed[[:space:]]*\([[:space:]]*\)[[:space:]]*,/) != 0) {
                    call_start = search_from + RSTART - 1
                    call_end = index(substr(source, call_start), ";")
                    if (call_end > 0 &&
                        index(substr(source, call_start, call_end - 1), label) > 0) {
                        exit 0
                    }
                    search_from = call_start + RLENGTH
                }
                exit 1
            }
        '
}

for direct_state_label in \
    'masked readback mismatch does not publish IRQ1 route state' \
    'final readback mismatch does not publish IRQ1 route state'; do
    if ! verify_irq1_direct_state_assertion "$direct_state_label" \
        "$irq1_mismatch_block"; then
        echo "tests/ioapic-host.c не содержит direct-state assertion с label: $direct_state_label" >&2
        exit 1
    fi
done
for conjunction_label in \
    'masked readback mismatch conjunction is false' \
    'final readback mismatch conjunction is false'; do
    grep -Fq "$conjunction_label" tests/ioapic-host.c || {
        echo "tests/ioapic-host.c не содержит conjunction assertion: $conjunction_label" >&2
        exit 1
    }
done

# .PHONY must contain each of test-vmm-host and test-vmm exactly once.
phony_line=$(grep '^\.PHONY:' Makefile |
    sed 's/^\.PHONY:[[:space:]]*//' | tr '\n' ' ')
phony_count_vmm_host=$(echo "$phony_line" | grep -o 'test-vmm-host' | wc -l)
phony_count_vmm=$(echo "$phony_line" | grep -o 'test-vmm[^-]' | wc -l)
phony_count_vmm_user_host=$(echo "$phony_line" | grep -o 'test-vmm-user-host' | wc -l)
[[ "$phony_count_vmm_host" -eq 1 ]] || {
    echo ".PHONY содержит test-vmm-host $phony_count_vmm_host раз(а), ожидалось 1" >&2; exit 1; }
[[ "$phony_count_vmm_user_host" -eq 1 ]] || {
    echo ".PHONY содержит test-vmm-user-host $phony_count_vmm_user_host раз(а), ожидалось 1" >&2; exit 1; }
[[ "$phony_count_vmm" -eq 1 ]] || {
    echo ".PHONY содержит test-vmm $phony_count_vmm раз(а), ожидалось 1" >&2; exit 1; }
phony_count_lapic_host=$(echo "$phony_line" | grep -o 'test-lapic-host' | wc -l)
phony_count_lapic=$(echo "$phony_line" | grep -o 'test-lapic[^-]' | wc -l)
[[ "$phony_count_lapic_host" -eq 1 ]] || {
    echo ".PHONY содержит test-lapic-host $phony_count_lapic_host раз(а), ожидалось 1" >&2; exit 1; }
[[ "$phony_count_lapic" -eq 1 ]] || {
    echo ".PHONY содержит test-lapic $phony_count_lapic раз(а), ожидалось 1" >&2; exit 1; }
phony_count_ioapic_host=$(echo "$phony_line" | grep -o 'test-ioapic-host' | wc -l)
phony_count_ioapic=$(echo "$phony_line" | grep -o 'test-ioapic[^-]' | wc -l)
[[ "$phony_count_ioapic_host" -eq 1 ]] || {
    echo ".PHONY содержит test-ioapic-host $phony_count_ioapic_host раз(а), ожидалось 1" >&2; exit 1; }
[[ "$phony_count_ioapic" -eq 1 ]] || {
    echo ".PHONY содержит test-ioapic $phony_count_ioapic раз(а), ожидалось 1" >&2; exit 1; }
phony_count_keyboard_host=$(echo "$phony_line" | grep -o 'test-keyboard-host' | wc -l)
[[ "$phony_count_keyboard_host" -eq 1 ]] || {
    echo ".PHONY содержит test-keyboard-host $phony_count_keyboard_host раз(а), ожидалось 1" >&2; exit 1; }
phony_count_keyboard_line_editing=$(echo "$phony_line" |
    grep -o 'test-keyboard-line-editing' | wc -l)
[[ "$phony_count_keyboard_line_editing" -eq 1 ]] || {
    echo ".PHONY содержит test-keyboard-line-editing $phony_count_keyboard_line_editing раз(а), ожидалось 1" >&2
    exit 1
}
phony_count_console_history=$(echo "$phony_line" |
    grep -o 'test-console-history' | wc -l)
[[ "$phony_count_console_history" -eq 1 ]] || {
    echo ".PHONY содержит test-console-history $phony_count_console_history раз(а), ожидалось 1" >&2
    exit 1
}
phony_count_ioapic_irq1=$(echo "$phony_line" | grep -o 'test-ioapic-irq1' | wc -l)
[[ "$phony_count_ioapic_irq1" -eq 1 ]] || {
    echo ".PHONY содержит test-ioapic-irq1 $phony_count_ioapic_irq1 раз(а), ожидалось 1" >&2; exit 1; }
phony_count_ioapic_irq0=$(echo "$phony_line" | grep -o 'test-ioapic-irq0' | wc -l)
[[ "$phony_count_ioapic_irq0" -eq 1 ]] || {
    echo ".PHONY содержит test-ioapic-irq0 $phony_count_ioapic_irq0 раз(а), ожидалось 1" >&2; exit 1; }
phony_count_lapic_timer_host=$(echo "$phony_line" | grep -o 'test-lapic-timer-host' | wc -l)
[[ "$phony_count_lapic_timer_host" -eq 1 ]] || {
    echo ".PHONY содержит test-lapic-timer-host $phony_count_lapic_timer_host раз(а), ожидалось 1" >&2; exit 1; }
phony_count_lapic_timer_runtime_host=$(echo "$phony_line" | grep -o 'test-lapic-timer-runtime-host' | wc -l)
[[ "$phony_count_lapic_timer_runtime_host" -eq 1 ]] || {
    echo ".PHONY содержит test-lapic-timer-runtime-host $phony_count_lapic_timer_runtime_host раз(а), ожидалось 1" >&2; exit 1; }
phony_count_lapic_timer_runtime=$(echo "$phony_line" |
    grep -o 'test-lapic-timer-runtime[^-]' | wc -l)
[[ "$phony_count_lapic_timer_runtime" -eq 1 ]] || {
    echo ".PHONY содержит test-lapic-timer-runtime $phony_count_lapic_timer_runtime раз(а), ожидалось 1" >&2; exit 1; }
phony_count_lapic_timer_calibration=$(echo "$phony_line" | grep -o 'test-lapic-timer-calibration' | wc -l)
[[ "$phony_count_lapic_timer_calibration" -eq 1 ]] || {
    echo ".PHONY содержит test-lapic-timer-calibration $phony_count_lapic_timer_calibration раз(а), ожидалось 1" >&2; exit 1; }
phony_count_pic_disable=$(echo "$phony_line" |
    grep -o 'test-pic-disable' | wc -l)
[[ "$phony_count_pic_disable" -eq 1 ]] || {
    echo ".PHONY содержит test-pic-disable $phony_count_pic_disable раз(а), ожидалось 1" >&2
    exit 1
}
phony_count_heap_host=$(echo "$phony_line" | grep -o 'test-heap-host' | wc -l)
[[ "$phony_count_heap_host" -eq 1 ]] || {
    echo ".PHONY содержит test-heap-host $phony_count_heap_host раз(а), ожидалось 1" >&2
    exit 1
}
phony_count_heap=$(echo "$phony_line" | grep -o 'test-heap[^-]' | wc -l)
[[ "$phony_count_heap" -eq 1 ]] || {
    echo ".PHONY содержит test-heap $phony_count_heap раз(а), ожидалось 1" >&2
    exit 1
}
phony_count_dup=$(echo "$phony_line" | tr ' ' '\n' | sort | uniq -d | wc -l)
[[ "$phony_count_dup" -eq 0 ]] || {
    echo ".PHONY содержит дубликаты" >&2; exit 1; }

# LAPIC timer one-shot probe: static contracts, isolated proof, and docs.
oneshot_marker='[LAPIC-TIMER] oneshot armed: vector=0x43'
for oneshot_api in \
    'bool lapic_timer_oneshot_arm(uint32_t lapic_ticks);' \
    'void lapic_timer_oneshot_handle_irq(void);' \
    'bool lapic_timer_oneshot_active(void);'; do
    grep -Fq "$oneshot_api" kernel/include/lapic_timer.h || {
        echo "lapic_timer.h не содержит API: $oneshot_api" >&2
        exit 1
    }
done
oneshot_dispatch_block=$(sed -n \
    '/if (frame->vector == 0x43U) {/,/^    }/p' \
    kernel/src/arch/x86_64/interrupts.c)
grep -Fq 'lapic_timer_oneshot_handle_irq();' <<<"$oneshot_dispatch_block" || {
    echo "vector 0x43 block не вызывает lapic_timer_oneshot_handle_irq()" >&2
    exit 1
}
grep -Fq 'lapic_eoi();' <<<"$oneshot_dispatch_block" || {
    echo "vector 0x43 block не вызывает lapic_eoi()" >&2
    exit 1
}
if grep -Eq 'timer_handle_irq|pic_(mask|unmask|send_eoi)|ioapic_' \
    <<<"$oneshot_dispatch_block"; then
    echo "vector 0x43 block нарушает one-shot ownership contract" >&2
    exit 1
fi
grep -Fq 'HFOC_LAPIC_TIMER_ONESHOT_TEST' kernel/src/arch/x86_64/interrupts.c || {
    echo "interrupts.c не содержит one-shot test macro gate" >&2
    exit 1
}
grep -Fq 'HFOC_LAPIC_TIMER_ONESHOT_TEST' tests/lapic-timer-runtime-host.c || {
    echo "runtime host harness не содержит one-shot test macro gate" >&2
    exit 1
}
grep -Fq -- '-DHFOC_LAPIC_TIMER_ONESHOT_TEST' Makefile || {
    echo "Makefile не определяет HFOC_LAPIC_TIMER_ONESHOT_TEST" >&2
    exit 1
}
grep -Fq 'test-lapic-timer-oneshot' Makefile || {
    echo "Makefile не содержит test-lapic-timer-oneshot" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-lapic-timer-oneshot' Makefile || {
    echo "test: target не содержит test-lapic-timer-oneshot" >&2
    exit 1
}
phony_count_lapic_timer_oneshot=$(echo "$phony_line" |
    grep -o 'test-lapic-timer-oneshot[^-]' | wc -l)
[[ "$phony_count_lapic_timer_oneshot" -eq 1 ]] || {
    echo ".PHONY содержит test-lapic-timer-oneshot $phony_count_lapic_timer_oneshot раз(а), ожидалось 1" >&2
    exit 1
}
oneshot_target_block=$(sed -n \
    '/^test-lapic-timer-oneshot:/,/^[^[:space:]]/p' Makefile)
grep -Fq "QEMU_MACHINE='q35'" <<<"$oneshot_target_block" || {
    echo "test-lapic-timer-oneshot должен использовать QEMU_MACHINE='q35'" >&2
    exit 1
}
if grep -Eq 'QEMU_CPU=.*(max|-apic|-no-apic)|-cpu[[:space:]]+(max|-apic|-no-apic)' \
    <<<"$oneshot_target_block"; then
    echo "test-lapic-timer-oneshot должен не задавать QEMU_CPU" >&2
    exit 1
fi
grep -Fq "QEMU_SMP='1'" <<<"$oneshot_target_block" || {
    echo "test-lapic-timer-oneshot должен использовать QEMU_SMP='1'" >&2
    exit 1
}
bash -n scripts/test-qemu-lapic-timer-oneshot.sh || {
    echo "Синтаксис scripts/test-qemu-lapic-timer-oneshot.sh некорректен" >&2
    exit 1
}
for oneshot_strict_symbol in mktemp timeout send_monitor_quit successful_shutdown \
                            best_effort_cleanup cleanup no-reboot no-shutdown; do
    grep -Fq "$oneshot_strict_symbol" scripts/test-qemu-lapic-timer-oneshot.sh || {
        echo "test-qemu-lapic-timer-oneshot.sh не содержит $oneshot_strict_symbol" >&2
        exit 1
    }
done
grep -Fq 'QEMU APIC baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-lapic-timer-oneshot.sh || {
    echo "test-qemu-lapic-timer-oneshot.sh не фиксирует baseline" >&2
    exit 1
}
grep -Fq 'One-shot armed diagnostic: found' \
    scripts/test-qemu-lapic-timer-oneshot.sh || {
    echo "test-qemu-lapic-timer-oneshot.sh не проверяет arm diagnostic" >&2
    exit 1
}
grep -Fq 'One-shot complete diagnostic: found' \
    scripts/test-qemu-lapic-timer-oneshot.sh || {
    echo "test-qemu-lapic-timer-oneshot.sh не проверяет complete diagnostic" >&2
    exit 1
}
grep -Fq 'Servicing hardware INT=0x43 count: 1' \
    scripts/test-qemu-lapic-timer-oneshot.sh || {
    echo "test-qemu-lapic-timer-oneshot.sh не требует ровно один INT=0x43" >&2
    exit 1
}
grep -Fq 'Post-hook suffix: INT=0x42 found; INT=0x20 and INT=0x21 absent' \
    scripts/test-qemu-lapic-timer-oneshot.sh || {
    echo "test-qemu-lapic-timer-oneshot.sh не проверяет post-hook vectors" >&2
    exit 1
}
grep -Fq 'TICKS after > before: yes' \
    scripts/test-qemu-lapic-timer-oneshot.sh || {
    echo "test-qemu-lapic-timer-oneshot.sh не подтверждает рост ticks" >&2
    exit 1
}
grep -Fq 'QEMU info pic: pic0 imr=' \
    scripts/test-qemu-lapic-timer-oneshot.sh || {
    echo "test-qemu-lapic-timer-oneshot.sh не проверяет info pic" >&2
    exit 1
}
grep -Fq 'QEMU LAPIC timer one-shot test: OK' \
    scripts/test-qemu-lapic-timer-oneshot.sh || {
    echo "test-qemu-lapic-timer-oneshot.sh не содержит success marker" >&2
    exit 1
}
grep -Fq 'lapic-timer-oneshot\.sh' .github/workflows/ci.yml || {
    echo "CI pgrep pattern не содержит test-qemu-lapic-timer-oneshot.sh" >&2
    exit 1
}
for oneshot_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    grep -Fq 'one-shot' "$oneshot_doc" || {
        echo "$oneshot_doc не документирует one-shot probe" >&2
        exit 1
    }
    grep -Fq '0x43' "$oneshot_doc" || {
        echo "$oneshot_doc не документирует vector 0x43" >&2
        exit 1
    }
    grep -Fq 'periodic 0x42' "$oneshot_doc" || {
        echo "$oneshot_doc не документирует periodic 0x42" >&2
        exit 1
    }
done
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf "$oneshot_marker"; then
    echo "normal kernel ELF содержит one-shot test hook marker" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf 'HFOC_LAPIC_TIMER_ONESHOT_TEST'; then
    echo "normal kernel ELF содержит one-shot test macro string" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
    awk '$3 ~ /(oneshot.*test|^host_)/ { print; found=1 }
         END { exit !found }'; then
    echo "normal kernel ELF содержит one-shot test/host symbol" >&2
    exit 1
fi
if [[ -f "build/hfoc-os-$(<VERSION).iso" ]] && \
    bash scripts/hfoc-file-marker.sh "build/hfoc-os-$(<VERSION).iso" "$oneshot_marker"; then
    echo "normal ISO содержит one-shot test hook marker" >&2
    exit 1
fi

# Bounded kernel event ring contracts.
grep -Fq 'test-event-ring-host' Makefile || {
    echo "Makefile не содержит test-event-ring-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-event-ring-host' Makefile || {
    echo "test: target не содержит test-event-ring-host" >&2; exit 1; }
grep -Fq 'KERNEL_EVENT_RING_CAPACITY' kernel/include/event_ring.h || {
    echo "event_ring.h не содержит KERNEL_EVENT_RING_CAPACITY" >&2; exit 1; }
grep -Fq 'kernel_event_ring_get' kernel/include/event_ring.h || {
    echo "event_ring.h не содержит kernel_event_ring_get" >&2; exit 1; }
for event_ring_symbol in event_ring_init event_ring_destroy event_ring_push \
                         event_ring_pop event_ring_snapshot event_ring_count \
                         event_ring_capacity event_ring_write_cursor \
                         event_ring_read_cursor event_ring_is_published \
                         event_ring_status_string kernel_event_ring_get; do
    nm build/hfoc-kernel.elf | grep -F " $event_ring_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $event_ring_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b' \
        kernel/src/event_ring.c; then
    echo "event_ring.c выполняет запрещённое выделение" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Event ring host tests: ALL OK'; then
    echo "Production kernel содержит event ring host marker" >&2
    exit 1
fi
for event_ring_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'event ring|event-ring' "$event_ring_doc"; then
        echo "$event_ring_doc не документирует event ring" >&2
        exit 1
    fi
done

# Structured audit record contracts.
grep -Fq 'test-audit-host' Makefile || {
    echo "Makefile не содержит test-audit-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-audit-host' Makefile || {
    echo "test: target не содержит test-audit-host" >&2; exit 1; }
grep -Fq 'struct audit_record' kernel/include/audit.h || {
    echo "audit.h не содержит struct audit_record" >&2; exit 1; }
grep -Fq 'AUDIT_EVENT_FLAG_STRUCTURED' kernel/include/audit.h || {
    echo "audit.h не содержит AUDIT_EVENT_FLAG_STRUCTURED" >&2; exit 1; }
grep -Fq 'AUDIT_EMPTY' kernel/include/audit.h || {
    echo "audit.h не содержит AUDIT_EMPTY" >&2; exit 1; }
for audit_symbol in audit_record_init audit_record_append \
                    audit_record_to_event audit_record_from_event \
                    audit_snapshot audit_record_pop \
                    audit_status_string audit_record_type_string; do
    nm build/hfoc-kernel.elf | grep -F " $audit_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $audit_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b' \
        kernel/src/audit.c; then
    echo "audit.c выполняет запрещённое выделение" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Audit record host tests: ALL OK'; then
    echo "Production kernel содержит audit host marker" >&2
    exit 1
fi
grep -Fq 'test-audit-snapshot-host' Makefile || {
    echo "Makefile не содержит test-audit-snapshot-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-audit-snapshot-host' Makefile || {
    echo "test: target не содержит test-audit-snapshot-host" >&2; exit 1; }
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Audit/event snapshot host tests: ALL OK'; then
    echo "Production kernel содержит audit/event snapshot host marker" >&2
    exit 1
fi
grep -Fq 'test-audit-observability-host' Makefile || {
    echo "Makefile не содержит test-audit-observability-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-audit-observability-host' Makefile || {
    echo "test: target не содержит test-audit-observability-host" >&2; exit 1; }
grep -Fq 'audit_record_pop' kernel/src/audit.c || {
    echo "audit.c не содержит audit_record_pop" >&2; exit 1; }
grep -Fq 'audit_record_pop(ring, &records[index])' \
    kernel/src/user_init_qemu.c || {
    echo "user_init_qemu.c не потребляет audit records" >&2; exit 1; }
grep -Fq 'audit_record_append(ring, &record, NULL)' \
    kernel/src/syscall.c || {
    echo "syscall.c не публикует syscall audit records" >&2; exit 1; }
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Audit observability host tests: ALL OK'; then
    echo "Production kernel содержит audit observability host marker" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[AUDIT-OBS]'; then
    echo "Production kernel содержит audit observability proof marker" >&2
    exit 1
fi
for audit_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'audit record|audit-records|structured audit' "$audit_doc"; then
        echo "$audit_doc не документирует structured audit records" >&2
        exit 1
    fi
    if ! grep -Eiq 'audit/event snapshot|consistent.*snapshot' "$audit_doc"; then
        echo "$audit_doc не документирует consistent audit/event snapshots" >&2
        exit 1
    fi
    if ! grep -Eiq 'audit observability|observability' "$audit_doc"; then
        echo "$audit_doc не документирует audit observability" >&2
        exit 1
    fi
done

# Validated service descriptor contracts.
grep -Fq 'test-service-descriptor-host' Makefile || {
    echo "Makefile не содержит test-service-descriptor-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-service-descriptor-host' Makefile || {
    echo "test: target не содержит test-service-descriptor-host" >&2
    exit 1
}
grep -Fq 'SERVICE_DESCRIPTOR_VERSION' kernel/include/service.h || {
    echo "service.h не содержит SERVICE_DESCRIPTOR_VERSION" >&2
    exit 1
}
grep -Fq 'struct service_descriptor' kernel/include/service.h || {
    echo "service.h не содержит struct service_descriptor" >&2
    exit 1
}
grep -Fq 'SERVICE_CPU_TICKS_LIMIT_MAX' kernel/include/service.h || {
    echo "service.h не содержит SERVICE_CPU_TICKS_LIMIT_MAX" >&2
    exit 1
}
grep -Fq 'SERVICE_USER_MEMORY_LIMIT_MAX_PAGES' \
    kernel/include/service.h || {
    echo "service.h не содержит SERVICE_USER_MEMORY_LIMIT_MAX_PAGES" >&2
    exit 1
}
grep -Fq 'cpu_ticks_limit' kernel/include/service.h || {
    echo "service.h не содержит cpu_ticks_limit" >&2
    exit 1
}
grep -Fq 'user_memory_limit_pages' kernel/include/service.h || {
    echo "service.h не содержит user_memory_limit_pages" >&2
    exit 1
}
grep -Eq 'process_limit_set_cpu_ticks\(' kernel/src/service_supervisor.c || {
    echo "service_supervisor.c не применяет descriptor CPU limit" >&2
    exit 1
}
grep -Fq 'slot->descriptor.cpu_ticks_limit' \
    kernel/src/service_supervisor.c || {
    echo "service_supervisor.c не публикует descriptor CPU limit" >&2
    exit 1
}
grep -Eq 'process_limit_set_user_memory_pages\(' \
    kernel/src/service_supervisor.c || {
    echo "service_supervisor.c не применяет descriptor user-memory limit" >&2
    exit 1
}
grep -Fq 'slot->descriptor.user_memory_limit_pages' \
    kernel/src/service_supervisor.c || {
    echo "service_supervisor.c не публикует descriptor memory limit" >&2
    exit 1
}
if ! grep -Fq 'process_limit_cpu_ticks(process) ==' \
        kernel/src/service_supervisor.c ||
   ! grep -Fq 'service->descriptor.cpu_ticks_limit' \
        kernel/src/service_supervisor.c; then
    echo "service_supervisor.c не проверяет exact CPU limit ownership" >&2
    exit 1
fi
if ! grep -Fq 'process_limit_user_memory_limit_pages(process) ==' \
        kernel/src/service_supervisor.c ||
   ! grep -Fq 'service->descriptor.user_memory_limit_pages' \
        kernel/src/service_supervisor.c; then
    echo "service_supervisor.c не проверяет exact memory limit ownership" >&2
    exit 1
fi
grep -Fq 'service_supervisor_sync_exited' \
    kernel/src/service_supervisor.c || {
    echo "service_supervisor.c не синхронизирует direct process exit" >&2
    exit 1
}
grep -Fq 'service_descriptor_validate' kernel/include/service.h || {
    echo "service.h не содержит service_descriptor_validate" >&2
    exit 1
}
for service_symbol in service_descriptor_validate \
                      service_descriptor_init \
                      service_descriptor_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $service_symbol" >/dev/null || {
        echo "Production kernel не содержит $service_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b' \
        kernel/src/service.c; then
    echo "service.c выполняет запрещённое выделение" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Service descriptor host tests: ALL OK'; then
    echo "Production kernel содержит service descriptor host marker" >&2
    exit 1
fi

# Service dependency descriptor contracts.
grep -Fq 'test-service-dependency-host' Makefile || {
    echo "Makefile не содержит test-service-dependency-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-service-dependency-host' Makefile || {
    echo "test: target не содержит test-service-dependency-host" >&2
    exit 1
}
grep -Fq 'SERVICE_DEPENDENCY_MAX' kernel/include/service.h || {
    echo "service.h не содержит SERVICE_DEPENDENCY_MAX" >&2
    exit 1
}
grep -Fq 'SERVICE_DEPENDENCY_SET_MAX' kernel/include/service.h || {
    echo "service.h не содержит SERVICE_DEPENDENCY_SET_MAX" >&2
    exit 1
}
grep -Fq 'service_dependency_set_validate' kernel/include/service.h || {
    echo "service.h не содержит service_dependency_set_validate" >&2
    exit 1
}
grep -Fq 'service_dependency_status_string' kernel/include/service.h || {
    echo "service.h не содержит service_dependency_status_string" >&2
    exit 1
}
for dependency_symbol in service_dependency_set_validate \
                         service_dependency_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $dependency_symbol" >/dev/null || {
        echo "Production kernel не содержит $dependency_symbol" >&2
        exit 1
    }
done
if nm -g build/hfoc-kernel.elf | grep 'service_dependency_host_test_'; then
    echo "service_dependency_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Service dependency host tests: ALL OK'; then
    echo "Production kernel содержит service dependency host marker" >&2
    exit 1
fi
for dependency_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'service dependency|dependency descriptor' \
            "$dependency_doc"; then
        echo "$dependency_doc не документирует service dependencies" >&2
        exit 1
    fi
done
