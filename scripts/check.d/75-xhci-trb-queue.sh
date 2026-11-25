#!/usr/bin/env bash
# ITEM-469: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The keyboard holds a queue of armed interrupt transfers, and a completion
# re-arms inside its own handler (СРОЧНО-19, round 8).
#
# The owner's 2026-09-22 run after ITEM-468: the pointer still jerked and the
# keyboard lost fast typing (`reports=622 errors=0 short=0`, mouse
# `reports=117 completed=118 errors=0`) exactly as before.  The mechanism was
# the single TRB: `xhci_rearm_devices()` kept one transfer in flight and ran at
# the END of the service pass, while the main loop sleeps in `cpu_hlt()` and
# wakes on the 100 Hz PIT - so between a completion and the next tick the
# endpoint had no TRB and the device dropped every report that arrived in the
# window.  A slow, >10 ms typing cadence fits inside the armed window; fast
# typing does not.
#
# This item keeps XHCI_ARM_DEPTH (2..4) transfers armed per role, each with its
# own report buffer, and tops the queue up inside the completion's own
# `xhci_apply_event()` - not at the end of the pass.  It also counts every
# completion code, names missed-service (23) and event-ring-full (21), and
# publishes the queue depth (`armed=`/`kbd_armed=`) and `mse=`.  This slice
# pins all of that, and the QEMU fast-typing proof that requires `kbd_mse=0`
# and `kbd_armed>=2`.
#
# ITEM-470 (СРОЧНО-20) raises the pool to XHCI_REPORT_SLOTS=16 and keeps the
# keyboard at XHCI_ARM_DEPTH=3 while the mouse arms XHCI_MOUSE_ARM_DEPTH=16, so
# the top-up compares against the role's own depth.  The queue slice's own
# assertions below are updated to that shape; the new depth and the unarmed
# window counter are pinned by `76-pointer-speed-queue-honesty.sh`.
for xhci_trb_queue_file in \
    kernel/include/xhci.h \
    kernel/include/console.h \
    kernel/src/xhci.c \
    kernel/src/kernel.c \
    scripts/test-qemu-production-storage-boot.sh \
    docs/hfoc-appliance-boot-console.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $xhci_trb_queue_file ]] || {
        echo "ITEM-469: отсутствует $xhci_trb_queue_file" >&2
        exit 1
    }
done

# 1. The queue exists: the depth is 2..4, the buffers are per-slot, and the
#    driver counts every completion code plus the two named errors.
grep -Fq '#define XHCI_ARM_DEPTH 3U' kernel/src/xhci.c || {
    echo "ITEM-469: глубина очереди TRB не объявлена" >&2
    exit 1
}
grep -Fq '#define XHCI_REPORT_SLOTS 16U' kernel/src/xhci.c || {
    echo "ITEM-469: слотов буферов отчёта не объявлено" >&2
    exit 1
}
for xhci_trb_queue_field in \
    'uint8_t armed_slots[XHCI_REPORT_SLOTS];' \
    'uint8_t armed_head;' \
    'uint8_t armed_count;' \
    'uint32_t mse_count;' \
    'uint32_t event_ring_full_count;' \
    'uint32_t completion_code_counts[XHCI_COMPLETION_CODE_COUNT];'; do
    grep -Fq -- "$xhci_trb_queue_field" kernel/src/xhci.c || {
        echo "ITEM-469: состояние очереди не ведёт поле:" \
            "$xhci_trb_queue_field" >&2
        exit 1
    }
done

# 2. The queue is filled to the depth, and the completion re-arms in place -
#    `xhci_top_up_device()` is called from `xhci_apply_event()`, not only from
#    the end-of-pass re-arm.
grep -Fq 'static void xhci_top_up_device(struct xhci_hid_device *device)' \
    kernel/src/xhci.c || {
    echo "ITEM-469: нет добора очереди до глубины" >&2
    exit 1
}
grep -Fq 'xhci_top_up_device(device);' kernel/src/xhci.c || {
    echo "ITEM-469: очередь не добирается" >&2
    exit 1
}
grep -Fq 'device->armed_count < xhci_role_arm_depth(device->role)' \
    kernel/src/xhci.c || {
    echo "ITEM-469: добор не держит глубину" >&2
    exit 1
}

# 3. The completion retires the oldest buffer, counts its code and publishes the
#    two errors the owner asked to see separately.
grep -Fq 'slot = device->armed_slots[device->armed_head];' kernel/src/xhci.c || {
    echo "ITEM-469: завершение не снимает старейший буфер" >&2
    exit 1
}
grep -Fq '++device->completion_code_counts[event->completion_code];' \
    kernel/src/xhci.c || {
    echo "ITEM-469: код завершения не считается" >&2
    exit 1
}
grep -Fq '#define XHCI_COMPLETION_MSE 23U' kernel/src/xhci.c || {
    echo "ITEM-469: missed service не назван" >&2
    exit 1
}
grep -Fq '#define XHCI_COMPLETION_EVENT_RING_FULL 21U' kernel/src/xhci.c || {
    echo "ITEM-469: event ring full не назван" >&2
    exit 1
}

# 4. The diagnostics carry `armed=` and `mse=`, and the durable input record
#    carries `kbd_armed=`/`kbd_mse=` once a second.
for xhci_trb_queue_diag in \
    '" armed="' \
    '" mse="' \
    '" iter_window=1s iter_max_scope=boot"'; do
    grep -Fq -- "$xhci_trb_queue_diag" kernel/src/xhci.c || {
        echo "ITEM-469: [INPUT-DIAG]/[LOOP-DIAG] не несёт" \
            "$xhci_trb_queue_diag" >&2
        exit 1
    }
done
for xhci_trb_queue_record in \
    '" kbd_armed="' \
    '" kbd_mse="' \
    '" mouse_armed="' \
    '" mouse_mse="'; do
    grep -Fq -- "$xhci_trb_queue_record" kernel/src/kernel.c || {
        echo "ITEM-469: [PRODUCTION-INPUT] не несёт" \
            "$xhci_trb_queue_record" >&2
        exit 1
    }
done

# 5. The input record is published once a second, and the loop-cost window is
#    closed by the once-a-second heartbeat, not the ten-second record.
grep -Fq 'now - production_input_last_ticks < HFOC_TIMER_HZ' \
    kernel/src/kernel.c || {
    echo "ITEM-469: запись ввода не идёт раз в секунду" >&2
    exit 1
}
grep -Fq 'void kernel_loop_iter_window_reset(void)' kernel/src/kernel.c || {
    echo "ITEM-469: окно оборота цикла не закрывается heartbeat'ом" >&2
    exit 1
}
grep -Fq 'kernel_loop_iter_window_reset();' kernel/src/xhci.c || {
    echo "ITEM-469: heartbeat не закрывает окно оборота цикла" >&2
    exit 1
}

# 6. The accessors exist and the QEMU fast-typing proof requires the numbers.
for xhci_trb_queue_accessor in \
    'uint32_t xhci_keyboard_armed_count(void);' \
    'uint32_t xhci_keyboard_mse_count(void);' \
    'uint32_t xhci_mouse_armed_count(void);' \
    'uint32_t xhci_mouse_mse_count(void);' \
    'void kernel_loop_iter_window_reset(void);'; do
    grep -Fq -- "$xhci_trb_queue_accessor" kernel/include/xhci.h \
        kernel/include/console.h || {
        echo "ITEM-469: доступ к очереди не объявлен:" \
            "$xhci_trb_queue_accessor" >&2
        exit 1
    }
done
grep -Fq 'int(input_fields["kbd_armed"]) < 2' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-469: QEMU-проба не требует очередь" >&2
    exit 1
}
grep -Fq 'int(input_fields["kbd_mse"]) != 0' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-469: QEMU-проба не требует mse=0" >&2
    exit 1
}

# 7. The item is recorded where a maintainer looks.
for xhci_trb_queue_doc in \
    'ITEM-469' \
    'XHCI_ARM_DEPTH' \
    'armed=' \
    'mse='; do
    grep -Fq -- "$xhci_trb_queue_doc" \
        docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-469:" \
            "$xhci_trb_queue_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-469' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-469" >&2
    exit 1
}
grep -Fq 'ITEM-469' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-469" >&2
    exit 1
}
