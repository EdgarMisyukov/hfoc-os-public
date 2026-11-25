#!/usr/bin/env bash
# ITEM-479: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The input path names its own phases, and a stalled endpoint recovers
# (СРОЧНО-28, round 17).
#
# The round-16 hardware run (`/srv/hfoc-example/queue/hw8-log-all.txt`) published
# one loop turn of `input_us=1213704` beside `xhci_us=99` in an otherwise
# healthy 10 ms loop, and ended with the pointer silent for the last 95 s while
# the driver held `armed=16`, the endpoint context read Running and every
# completion code was SUCCESS.  The phase budget could name "input" but not the
# work inside it, and no code noticed or recovered the silence.  This item:
#
#   * splits the input path into three measured sub-spans - the driver service
#     and doorbell, the keyboard's decoded events, the pointer's movement and
#     drawing - and publishes them plus the window's worst turn, the span it
#     spent (`worst_us=`, `worst_phase=`) and the count of turns past the 50 ms
#     stall bound (`over50=`);
#   * adds the stalled-endpoint watchdog: a ready role with armed transfers and
#     no completion past one second publishes `[INPUT-WATCHDOG]` once per
#     silence episode and re-arms and kicks the endpoint;
#   * proves the pure arithmetic on the host and drives the record in QEMU.

for item479_file in \
    kernel/include/loop_budget.h \
    kernel/src/loop_budget.c \
    kernel/src/kernel.c \
    kernel/src/xhci.c \
    kernel/src/xhci_trb.c \
    kernel/include/xhci.h \
    kernel/include/xhci_trb.h \
    tests/loop-budget-host.c \
    tests/xhci-host.c \
    Makefile \
    scripts/test-qemu-production-storage-boot.sh \
    docs/hfoc-appliance-boot-console.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item479_file ]] || {
        echo "ITEM-479: отсутствует $item479_file" >&2
        exit 1
    }
done

# ------------------------------------------- the pure phase arithmetic --
grep -Fq 'enum loop_budget_phase loop_budget_dominant(' \
    kernel/include/loop_budget.h || {
    echo "ITEM-479: нет чистой функции выбора фазы" >&2
    exit 1
}
grep -Fq 'const char *loop_budget_phase_name(enum loop_budget_phase phase);' \
    kernel/include/loop_budget.h || {
    echo "ITEM-479: нет имени фазы" >&2
    exit 1
}
grep -Fq 'bool loop_budget_is_long_turn(uint64_t period_us);' \
    kernel/include/loop_budget.h || {
    echo "ITEM-479: нет границы застойного оборота" >&2
    exit 1
}
grep -Fq '#define LOOP_BUDGET_LONG_TURN_US 50000U' \
    kernel/include/loop_budget.h || {
    echo "ITEM-479: граница застойного оборота не 50 мс" >&2
    exit 1
}
grep -Fq 'return period_us > (uint64_t)LOOP_BUDGET_LONG_TURN_US;' \
    kernel/src/loop_budget.c || {
    echo "ITEM-479: граница застойного оборота не строгая" >&2
    exit 1
}
for item479_budget_name in console xhci input net in_xhci in_kbd in_ptr other; do
    grep -Fq "return \"$item479_budget_name\";" kernel/src/loop_budget.c || {
        echo "ITEM-479: у фазы нет имени:" "$item479_budget_name" >&2
        exit 1
    }
done

# ----------------------------------------- the input sub-phases in kernel.c --
grep -Fq 'enum kernel_loop_input_phase {' kernel/src/kernel.c || {
    echo "ITEM-479: под-фазы ввода не объявлены" >&2
    exit 1
}
grep -Fq 'static void kernel_loop_phase_end_input(enum kernel_loop_input_phase phase,' \
    kernel/src/kernel.c || {
    echo "ITEM-479: под-фаза не измеряется" >&2
    exit 1
}
grep -Fq 'kernel_loop_input_window_us[index] += kernel_loop_input_us[index];' \
    kernel/src/kernel.c || {
    echo "ITEM-479: под-спаны не сворачиваются в окно" >&2
    exit 1
}
for item479_sub in XHCI KEYBOARD POINTER; do
    grep -Fq "kernel_loop_phase_end_input(KERNEL_LOOP_INPUT_$item479_sub," \
        kernel/src/kernel.c || {
        echo "ITEM-479: под-фаза не закрыта:" "$item479_sub" >&2
        exit 1
    }
done
# The fold and the worst-turn snapshot live inside the turn boundary, before
# the in-flight spans are cleared, so the worst phase describes that turn.
item479_turn_body=$(
    awk '/^static void kernel_loop_phase_turn_begin\(void\) \{/,/^\}/' \
        kernel/src/kernel.c
)
grep -Fq 'turn.in_ptr_us = kernel_loop_input_us[KERNEL_LOOP_INPUT_POINTER];' \
    <<<"$item479_turn_body" || {
    echo "ITEM-479: снимок худшего оборота не снят" >&2
    exit 1
}
grep -Fq 'kernel_loop_phase_worst_name = loop_budget_dominant(&turn);' \
    <<<"$item479_turn_body" || {
    echo "ITEM-479: фаза худшего оборота не названа" >&2
    exit 1
}
grep -Fq 'if (loop_budget_is_long_turn(period)) {' <<<"$item479_turn_body" || {
    echo "ITEM-479: застойные обороты не считаются" >&2
    exit 1
}
item479_reset_body=$(
    awk '/^static void kernel_loop_phase_window_reset\(void\) \{/,/^\}/' \
        kernel/src/kernel.c
)
grep -Fq 'kernel_loop_input_window_us[index] = 0U;' <<<"$item479_reset_body" || {
    echo "ITEM-479: окно под-спанов не сбрасывается" >&2
    exit 1
}
grep -Fq 'kernel_loop_phase_worst_name = LOOP_BUDGET_OTHER;' \
    <<<"$item479_reset_body" || {
    echo "ITEM-479: худший оборот не сбрасывается" >&2
    exit 1
}
# The record carries the sub-spans and the worst-turn facts beside the ones
# ITEM-474/ITEM-476 already publish.
for item479_field in in_xhci_us in_kbd_us in_ptr_us worst_us worst_phase over50; do
    grep -Fq "NET_RECORD_WRITE(\" $item479_field=\")" kernel/src/kernel.c || {
        echo "ITEM-479: запись фаз не публикует" "$item479_field" >&2
        exit 1
    }
done
grep -Fq 'NET_RECORD_WRITE(" kbd_watchdog=");' kernel/src/kernel.c || {
    echo "ITEM-479: хвост ввода не публикует счётчик клавиатуры" >&2
    exit 1
}
grep -Fq 'NET_RECORD_WRITE(" mouse_watchdog=");' kernel/src/kernel.c || {
    echo "ITEM-479: хвост ввода не публикует счётчик мыши" >&2
    exit 1
}

# ------------------------------------------------- the endpoint watchdog --
grep -Fq '#define XHCI_WATCHDOG_SILENCE_MS 1000U' kernel/src/xhci.c || {
    echo "ITEM-479: порог сторожа не 1 с" >&2
    exit 1
}
grep -Fq 'uint64_t watchdog_progress_ticks;' kernel/src/xhci.c || {
    echo "ITEM-479: у роли нет часов тишины" >&2
    exit 1
}
grep -Fq 'static void xhci_watchdog_device(struct xhci_hid_device *device) {' \
    kernel/src/xhci.c || {
    echo "ITEM-479: сторож роли не реализован" >&2
    exit 1
}
grep -Fq 'static void xhci_watchdog_publish(const struct xhci_hid_device *device,' \
    kernel/src/xhci.c || {
    echo "ITEM-479: запись сторожа не реализована" >&2
    exit 1
}
grep -Fq 'serial_write("[INPUT-WATCHDOG] role=");' kernel/src/xhci.c || {
    echo "ITEM-479: запись сторожа не названа" >&2
    exit 1
}
grep -Fq 'serial_write(" action=REARM");' kernel/src/xhci.c || {
    echo "ITEM-479: запись сторожа не называет действие" >&2
    exit 1
}
# ITEM-482 (round 20): the watchdog record keeps naming REARM and now also
# publishes the escalation decision beside it.
grep -Fq 'serial_write(" escalate=");' kernel/src/xhci.c || {
    echo "ITEM-482: запись сторожа не публикует эскалацию" >&2
    exit 1
}
grep -Fq 'static void xhci_endpoint_recover(struct xhci_hid_device *device) {' \
    kernel/src/xhci.c || {
    echo "ITEM-479: восстановление эндпоинта не реализовано" >&2
    exit 1
}
item479_recover_body=$(
    awk '/^static void xhci_endpoint_recover\(struct xhci_hid_device \*device\) \{/,/^\}/' \
        kernel/src/xhci.c
)
grep -Fq 'xhci_top_up_device(device);' <<<"$item479_recover_body" || {
    echo "ITEM-479: восстановление не добирает очередь" >&2
    exit 1
}
grep -Fq 'xhci_write32(xhci_doorbell(device->slot_id), 0U, device->dci);' \
    <<<"$item479_recover_body" || {
    echo "ITEM-479: восстановление не звонит в двербель" >&2
    exit 1
}
grep -Fq 'xhci_watchdog_devices();' kernel/src/xhci.c || {
    echo "ITEM-479: сторож не вызывается" >&2
    exit 1
}
grep -Fq 'device->watchdog_progress_ticks = timer_ticks();' kernel/src/xhci.c || {
    echo "ITEM-479: свежее вооружение не отмечает прогресс" >&2
    exit 1
}
grep -Fq 'device->watchdog_reported = false;' kernel/src/xhci.c || {
    echo "ITEM-479: завершение не заканчивает эпизод тишины" >&2
    exit 1
}
for item479_accessor in \
    'uint32_t xhci_keyboard_watchdog_count(void);' \
    'uint32_t xhci_keyboard_watchdog_silence_ms_max(void);' \
    'uint32_t xhci_mouse_watchdog_count(void);' \
    'uint32_t xhci_mouse_watchdog_silence_ms_max(void);'; do
    grep -Fq "$item479_accessor" kernel/include/xhci.h || {
        echo "ITEM-479: нет доступа к счётчикам сторожа:" \
            "$item479_accessor" >&2
        exit 1
    }
done

# ----------------------------------- the pure watchdog decision in the TRB half --
grep -Fq 'uint32_t xhci_watchdog_silence_ms(uint64_t last_ticks, uint64_t now_ticks,' \
    kernel/include/xhci_trb.h || {
    echo "ITEM-479: нет чистой функции тишины" >&2
    exit 1
}
grep -Fq 'bool xhci_watchdog_due(bool ready, uint32_t armed, uint32_t silence_ms,' \
    kernel/include/xhci_trb.h || {
    echo "ITEM-479: нет чистой функции срабатывания" >&2
    exit 1
}
grep -Fq 'return ready && armed != 0U && silence_ms > threshold_ms;' \
    kernel/src/xhci_trb.c || {
    echo "ITEM-479: срабатывание не строгое и не ограничено armed>0" >&2
    exit 1
}
grep -Fq 'if (last_ticks == 0U || now_ticks < last_ticks || tick_hz == 0U) {' \
    kernel/src/xhci_trb.c || {
    echo "ITEM-479: немеримая тишина не отсечена" >&2
    exit 1
}

# ------------------------------------------------------------- host proofs --
grep -Fq 'static void test_pointer_spends_the_turn(void)' \
    tests/loop-budget-host.c || {
    echo "ITEM-479: нет host-пробы под-фаз" >&2
    exit 1
}
for item479_claim in \
    "the pointer's movement is named when it spends the turn" \
    "the keyboard's decoded events are named when they spend the turn" \
    'the driver service and doorbell are named when they dominate' \
    'a tie between the console and the pointer keeps the console' \
    "the owner's 1.2-second input turn is a stall turn"; do
    grep -Fq -- "$item479_claim" tests/loop-budget-host.c || {
        echo "ITEM-479: host-проба фаз не утверждает:" "$item479_claim" >&2
        exit 1
    }
done
grep -Fq 'static void test_watchdog_decision(void) {' tests/xhci-host.c || {
    echo "ITEM-479: нет host-пробы сторожа" >&2
    exit 1
}
for item479_claim in \
    'a ready role with armed transfers past the bound is due' \
    'a role exactly at the bound is not due yet' \
    'a role with nothing armed is not due' \
    'a role that is not ready is not due'; do
    grep -Fq -- "$item479_claim" tests/xhci-host.c || {
        echo "ITEM-479: host-проба сторожа не утверждает:" "$item479_claim" >&2
        exit 1
    }
done
grep -Fq 'test_watchdog_decision();' tests/xhci-host.c || {
    echo "ITEM-479: host-проба сторожа не вызывается" >&2
    exit 1
}
grep -Fq 'test-loop-budget-host' Makefile || {
    echo "ITEM-479: host-проба фаз не зарегистрирована" >&2
    exit 1
}

# --------------------------------------------------------------- QEMU proof --
grep -Fq '"in_xhci_us", "in_kbd_us", "in_ptr_us",' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-479: QEMU-проба не читает под-спаны" >&2
    exit 1
}
grep -Fq '"worst_us", "worst_phase", "over50",' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-479: QEMU-проба не читает худший оборот" >&2
    exit 1
}
grep -Fq 'the driver service sub-span exceeds the driver span' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-479: QEMU-проба не сверяет под-спаны с родителями" >&2
    exit 1
}
grep -Fq 'r"\[INPUT-WATCHDOG\]([^\r\n]*)"' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-479: QEMU-проба не ловит запись сторожа" >&2
    exit 1
}
grep -Fq 'the stalled-endpoint watchdog never published its record' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-479: QEMU-проба не требует срабатывания сторожа" >&2
    exit 1
}

# ------------------------------------------------------------- the documents --
for item479_doc in \
    'ITEM-479' \
    'in_kbd_us' \
    'worst_phase' \
    'INPUT-WATCHDOG'; do
    grep -Fq -- "$item479_doc" docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-479:" \
            "$item479_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-479' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-479" >&2
    exit 1
}
grep -Fq 'ITEM-479' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-479" >&2
    exit 1
}
