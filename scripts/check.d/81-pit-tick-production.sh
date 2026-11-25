#!/usr/bin/env bash
# ITEM-475: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The production tick stays on PIT (СРОЧНО-25, round 14).
#
# The owner's hardware runs (`/srv/hfoc-example/queue/hw6-log-all.txt`) showed a
# machine living at ~2.4 wakeups a second (`tick_hz=2`, `iter_us=424 022`) with
# nothing in the journal naming the tick source.  The cause was the first IRQ0:
# `lapic_timer_runtime_coordinate_irq0()` enabled the periodic Local APIC timer
# and `pic_disable_after_apic_runtime()` disabled the PIC, leaving the Local
# APIC timer as the only wakeup at ~2.4 Hz.  This item keeps the ordinary
# production build on the legacy 8259 PIC + PIT at 100 Hz, leaves the verified
# transition to the drill/QEMU builds that prove it, and publishes the choice:
#
#   * `HFOC_PRODUCTION_PIT_TICK` in `PRODUCTION_CFLAGS` makes
#     `lapic_timer_runtime_transition_allowed()` return false, so neither the
#     vector 0x20 nor the vector 0x40 IRQ0 path can transition in production.
#   * `lapic_timer_tick_source()` names `pit` or `lapic`, and the once-a-second
#     `[LOOP-PHASES]` record carries `tick_source=` and the measured `wake_hz=`.
#   * `$(LAPIC_RUNTIME_ISO)` is the drill/QEMU kernel that keeps the
#     transition; the APIC-focused QEMU proofs run there, and the QEMU
#     production proof requires `tick_source=pit` with a PIT-rate loop.

for item475_file in \
    kernel/include/lapic_timer.h \
    kernel/src/lapic_timer.c \
    kernel/src/arch/x86_64/interrupts.c \
    kernel/src/kernel.c \
    kernel/include/appliance_admin_auth.h \
    kernel/src/appliance_management_panel.c \
    tests/lapic-timer-host.c \
    tests/lapic-timer-runtime-host.c \
    scripts/test-qemu-production-storage-boot.sh \
    docs/hfoc-appliance-boot-console.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item475_file ]] || {
        echo "ITEM-475: отсутствует $item475_file" >&2
        exit 1
    }
done

# ---------------------------------------------------------------------- A --
# The production build keeps PIT, and the transition policy is one function.
grep -Fq -- '-DHFOC_PRODUCTION_PIT_TICK' Makefile || {
    echo "ITEM-475: PRODUCTION_CFLAGS не включает HFOC_PRODUCTION_PIT_TICK" >&2
    exit 1
}
for item475_policy in \
    '#ifdef HFOC_PRODUCTION_PIT_TICK' \
    'return false;' \
    'bool lapic_timer_runtime_transition_allowed(void) {' \
    'return runtime_active ? LAPIC_TIMER_TICK_SOURCE_LAPIC' \
    'lapic_timer_tick_source_for(bool runtime_active)' \
    '"[LAPIC-TIMER] tick policy: pit"' \
    '"[LAPIC-TIMER] tick policy: lapic-transition"'; do
    grep -Fq -- "$item475_policy" kernel/src/lapic_timer.c || {
        echo "ITEM-475: политика тика не реализована:" "$item475_policy" >&2
        exit 1
    }
done
for item475_api in \
    'enum lapic_timer_tick_source {' \
    'LAPIC_TIMER_TICK_SOURCE_PIT = 0,' \
    'LAPIC_TIMER_TICK_SOURCE_LAPIC = 1,' \
    'bool lapic_timer_runtime_transition_allowed(void);' \
    'enum lapic_timer_tick_source lapic_timer_tick_source_for(bool runtime_active);' \
    'enum lapic_timer_tick_source lapic_timer_tick_source(void);' \
    'const char *lapic_timer_tick_source_string('; do
    grep -Fq -- "$item475_api" kernel/include/lapic_timer.h || {
        echo "ITEM-475: API источника тика не объявлен:" "$item475_api" >&2
        exit 1
    }
done
# Both IRQ0 paths go through the policy function before the coordinator.
item475_coordinator_guards=$(
    grep -c 'lapic_timer_runtime_transition_allowed()' \
        kernel/src/arch/x86_64/interrupts.c
)
[[ $item475_coordinator_guards -eq 2 ]] || {
    echo "ITEM-475: ожидалось 2 стража перехода, найдено" \
        "$item475_coordinator_guards" >&2
    exit 1
}
item475_coordinator_calls=$(
    grep -c 'lapic_timer_runtime_coordinate_irq0();' \
        kernel/src/arch/x86_64/interrupts.c
)
[[ $item475_coordinator_calls -eq 2 ]] || {
    echo "ITEM-475: ожидалось 2 вызова координатора, найдено" \
        "$item475_coordinator_calls" >&2
    exit 1
}

# ---------------------------------------------------------------------- B --
# The tick source and the measured wake rate are published every second.
grep -Fq '" tick_source="' kernel/src/kernel.c || {
    echo "ITEM-475: [LOOP-PHASES] не публикует tick_source" >&2
    exit 1
}
grep -Fq '" wake_hz="' kernel/src/kernel.c || {
    echo "ITEM-475: [LOOP-PHASES] не публикует wake_hz" >&2
    exit 1
}
grep -Fq 'lapic_timer_tick_source_string(lapic_timer_tick_source())' \
    kernel/src/kernel.c || {
    echo "ITEM-475: tick_source не берётся у политики" >&2
    exit 1
}
grep -Fq 'NET_RECORD_U64(timer_measured_tick_hz());' kernel/src/kernel.c || {
    echo "ITEM-475: wake_hz не берётся у измеренной частоты" >&2
    exit 1
}

# The PIT now delivers the bring-up IOAPIC IRQ0 at 100 Hz, so its two serial
# lines are one-shot: a per-delivery print would be 200 lines a second and
# would interleave the console's own records (which the production QEMU proof
# reads as contiguous markers).
for item475_ioapic in \
    'static bool irq0_ioapic_irq_published;' \
    'static bool irq0_ioapic_eoi_published;' \
    'if (!irq0_ioapic_irq_published) {' \
    'if (!irq0_ioapic_eoi_published) {'; do
    grep -Fq -- "$item475_ioapic" kernel/src/arch/x86_64/interrupts.c || {
        echo "ITEM-475: одноразовые записи IOAPIC не реализованы:" \
            "$item475_ioapic" >&2
        exit 1
    }
done

# The honest 100 Hz clock exposed the session lifetime's unit: the boundary
# compares its lifetime against `hfoc_lwip_time_now()` (milliseconds), so the
# console lifetime is one hour in milliseconds and the panel converts its
# cookie's seconds before opening the session.  Both are pinned so a later
# edit cannot silently shorten a session to a few seconds.
grep -Fq '#define APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX (3600U * 1000U)' \
    kernel/include/appliance_admin_auth.h || {
    echo "ITEM-475: lifetime сессии администратора не в миллисекундах" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PANEL_LOGIN_SESSION_SECONDS * 1000U,' \
    kernel/src/appliance_management_panel.c || {
    echo "ITEM-475: панель не переводит секунды cookie в миллисекунды" >&2
    exit 1
}

# ------------------------------------------------------- the drill/QEMU build --
# shellcheck disable=SC2016  # the single-quoted Makefile variables are literals
for item475_build in \
    'LAPIC_RUNTIME_OBJ_DIR := build/lapic-runtime/obj' \
    'LAPIC_RUNTIME_KERNEL := build/lapic-runtime/hfoc-kernel.elf' \
    'LAPIC_RUNTIME_ISO := build/lapic-runtime/hfoc-os-$(VERSION)-lapic-runtime.iso' \
    'LAPIC_RUNTIME_CFLAGS := $(CFLAGS)' \
    '$(LAPIC_RUNTIME_ISO): deps $(LAPIC_RUNTIME_KERNEL)' \
    '-include $(LAPIC_RUNTIME_DEPS)'; do
    grep -Fq -- "$item475_build" Makefile || {
        echo "ITEM-475: drill/QEMU-сборка не содержит:" "$item475_build" >&2
        exit 1
    }
done
# Every APIC-focused proof that asserts the runtime transition runs on the
# drill/QEMU ISO rather than the production one.
# shellcheck disable=SC2016  # the single-quoted Makefile variables are literals
for item475_target in \
    test-smp \
    test-lapic \
    test-ioapic-irq0 \
    test-ioapic-irq1 \
    test-lapic-timer-calibration \
    test-lapic-timer-runtime \
    test-pic-disable \
    test-pic-irq0-masking; do
    grep -Fq "$item475_target: \$(LAPIC_RUNTIME_ISO) iso" Makefile || {
        echo "ITEM-475: $item475_target не переведён на LAPIC_RUNTIME_ISO" >&2
        exit 1
    }
done
awk '/^QEMU_TEST_IMAGES :=/,/^$/' Makefile | grep -Fq 'LAPIC_RUNTIME_ISO' || {
    echo "ITEM-475: LAPIC_RUNTIME_ISO не в QEMU_TEST_IMAGES" >&2
    exit 1
}

# ------------------------------------------------------------- host proofs --
for item475_claim in \
    'an inactive runtime names PIT as the tick source' \
    'an active runtime names the LAPIC timer as the tick source' \
    "the PIT tick source string is 'pit'" \
    "the LAPIC tick source string is 'lapic'" \
    'the production build publishes the PIT tick policy' \
    'the drill/QEMU build publishes the LAPIC-transition tick policy'; do
    grep -Fq -- "$item475_claim" tests/lapic-timer-host.c || {
        echo "ITEM-475: host-проба не утверждает:" "$item475_claim" >&2
        exit 1
    }
done
grep -Fq 'bool lapic_timer_runtime_transition_allowed(void) {' \
    tests/lapic-timer-runtime-host.c || {
    echo "ITEM-475: host-заглушка политики не объявлена" >&2
    exit 1
}

# --------------------------------------------------------------- QEMU proof --
for item475_qemu in \
    '"tick_source", "wake_hz",' \
    'int(fields["wake_hz"]) >= 50' \
    'int(fields["turns"]) >= 20'; do
    grep -Fq -- "$item475_qemu" scripts/test-qemu-production-storage-boot.sh || {
        echo "ITEM-475: QEMU-проба не требует:" "$item475_qemu" >&2
        exit 1
    }
done
grep -Fq 'the production build must tick on PIT' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-475: QEMU-проба не фиксирует PIT-политику" >&2
    exit 1
}

# ------------------------------------------------------------- the documents --
for item475_doc in \
    'ITEM-475' \
    'tick_source' \
    'wake_hz' \
    'HFOC_PRODUCTION_PIT_TICK'; do
    grep -Fq -- "$item475_doc" docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-475:" \
            "$item475_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-475' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-475" >&2
    exit 1
}
grep -Fq 'ITEM-475' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-475" >&2
    exit 1
}
