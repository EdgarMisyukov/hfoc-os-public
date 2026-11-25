#!/usr/bin/env bash
# ITEM-480: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The pointer's turn budget and movement coalescing (СРОЧНО-29, round 18).
#
# The round-17 hardware run (`/srv/hfoc-example/queue/hw8-log-all.txt`) left the
# pointer's queue drained in an unbounded `while` loop - up to a whole queue of
# reports, each one a full cursor drawing - and every movement painted the
# arrow.  This item:
#
#   * serves the pointer a bounded number of draw-worthy batches per turn
#     (`LOCAL_INPUT_MOUSE_DRAW_BUDGET`), the keyboard is still drained whole
#     first, and the unserved remainder stays queued for the next turn;
#   * sums consecutive movements into one batch (`dx`/`dy`), while a button
#     edge breaks the run and is carried alone, in order;
#   * measures `framebuffer_cursor_show()` and `framebuffer_sync()` separately
#     and drains the write-combining stores once per batch, not once per report;
#   * publishes `[POINTER-BATCH]` with the draws, the reports consumed, the
#     budget and the two cost averages.

for item480_file in \
    kernel/include/local_input.h \
    kernel/src/local_input.c \
    kernel/include/usb_hid.h \
    kernel/include/console.h \
    kernel/src/console.c \
    kernel/src/kernel.c \
    tests/local-input-host.c \
    tests/console-host.c \
    Makefile \
    scripts/test-qemu-production-storage-boot.sh \
    docs/hfoc-appliance-boot-console.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item480_file ]] || {
        echo "ITEM-480: отсутствует $item480_file" >&2
        exit 1
    }
done

# ------------------------------------------- the named budget and the batch --
grep -Fq '#define LOCAL_INPUT_MOUSE_DRAW_BUDGET 3U' \
    kernel/include/local_input.h || {
    echo "ITEM-480: бюджет оборота мыши не назван" >&2
    exit 1
}
grep -Fq '#define LOCAL_INPUT_MOUSE_BATCH_EVENTS 16U' \
    kernel/include/local_input.h || {
    echo "ITEM-480: граница одной пачки не названа" >&2
    exit 1
}
grep -Fq 'uint32_t local_input_pop_mouse_batches(struct usb_hid_mouse_batch *batches,' \
    kernel/include/local_input.h || {
    echo "ITEM-480: нет функции бюджета пачек" >&2
    exit 1
}
grep -Fq 'struct usb_hid_mouse_batch {' kernel/include/usb_hid.h || {
    echo "ITEM-480: нет структуры пачки" >&2
    exit 1
}
for item480_field in 'int32_t dx;' 'int32_t dy;' 'uint16_t events;'; do
    grep -Fq -- "$item480_field" kernel/include/usb_hid.h || {
        echo "ITEM-480: у пачки нет поля:" "$item480_field" >&2
        exit 1
    }
done

# --------------------------------------------- the coalescing in local_input --
grep -Fq 'static bool local_input_mouse_peek(' kernel/src/local_input.c || {
    echo "ITEM-480: пачка не читает следующий отчёт без потребления" >&2
    exit 1
}
grep -Fq 'static void local_input_mouse_drop(void) {' kernel/src/local_input.c || {
    echo "ITEM-480: нет потребления удержанного отчёта" >&2
    exit 1
}
grep -Fq 'bool local_input_pop_mouse_batch(struct usb_hid_mouse_batch *batch) {' \
    kernel/src/local_input.c || {
    echo "ITEM-480: батч не собирается" >&2
    exit 1
}
grep -Fq 'batch->events = (uint16_t)spent;' kernel/src/local_input.c || {
    echo "ITEM-480: пачка не считает потреблённые отчёты" >&2
    exit 1
}
grep -Fq 'if (local_input_mouse_has_button(&event) || event.wheel != 0) {' \
    kernel/src/local_input.c || {
    echo "ITEM-480: кнопка (или колесо) не разрывает серию движений" >&2
    exit 1
}
grep -Fq 'for (spent = 0U; spent < LOCAL_INPUT_MOUSE_BATCH_EVENTS; ++spent) {' \
    kernel/src/local_input.c || {
    echo "ITEM-480: серия движений не ограничена батчем" >&2
    exit 1
}
grep -Fq 'while (count < budget && local_input_pop_mouse_batch(&batches[count])) {' \
    kernel/src/local_input.c || {
    echo "ITEM-480: дренаж пачек не ограничен бюджетом" >&2
    exit 1
}

# ------------------------------------------------ the bounded loop in kernel --
grep -Fq 'struct usb_hid_mouse_batch mouse_batches' kernel/src/kernel.c || {
    echo "ITEM-480: цикл не держит буфер пачек" >&2
    exit 1
}
grep -Fq 'local_input_pop_mouse_batches(' kernel/src/kernel.c || {
    echo "ITEM-480: цикл не зовёт бюджетный дренаж" >&2
    exit 1
}
grep -Fq 'mouse_batches, LOCAL_INPUT_MOUSE_DRAW_BUDGET);' kernel/src/kernel.c || {
    echo "ITEM-480: цикл не передаёт именованный бюджет" >&2
    exit 1
}
grep -Fq 'console_process_mouse_batch(' kernel/src/kernel.c || {
    echo "ITEM-480: цикл не обрабатывает пачку" >&2
    exit 1
}
if grep -Fq 'while (local_input_pop_mouse_event' kernel/src/kernel.c; then
    echo "ITEM-480: цикл всё ещё дренирует мышь неограниченно" >&2
    exit 1
fi

# ----------------------------------------- the console batch and its split --
grep -Fq 'void console_process_mouse_batch(const struct usb_hid_mouse_batch *batch);' \
    kernel/include/console.h || {
    echo "ITEM-480: консоль не объявляет обработку пачки" >&2
    exit 1
}
grep -Fq 'void console_process_mouse_batch(const struct usb_hid_mouse_batch *event) {' \
    kernel/src/console.c || {
    echo "ITEM-480: консоль не обрабатывает пачку" >&2
    exit 1
}
grep -Fq 'draw_us = timer_cycles_to_us(timer_cycles() - started);' \
    kernel/src/console.c || {
    echo "ITEM-480: цена отрисовки не измерена отдельно" >&2
    exit 1
}
# ITEM-482 (round 20): the drain moved out of the per-batch path into the single
# `console_end_input_batch()`; the batch marks the deferred drain and the one
# close charges it to the pointer's total.
grep -Fq 'console_input_needs_sync = true;' kernel/src/console.c || {
    echo "ITEM-480: отрисовка не отмечает отложенный барьер" >&2
    exit 1
}
grep -Fq 'void console_end_input_batch(void) {' kernel/src/console.c || {
    echo "ITEM-480: один барьер входной пачки не реализован" >&2
    exit 1
}
grep -Fq 'console_pointer_draw_us_total += draw_us;' kernel/src/console.c || {
    echo "ITEM-480: сумма цены отрисовки не копится" >&2
    exit 1
}
grep -Fq 'console_pointer_sync_us_total +=' kernel/src/console.c || {
    echo "ITEM-480: сумма цены барьера не копится" >&2
    exit 1
}
grep -Fq 'console_pointer_batch_events += event->events;' kernel/src/console.c || {
    echo "ITEM-480: потреблённые отчёты не считаются" >&2
    exit 1
}

# ------------------------------------------------------- the batch record --
grep -Fq '"[POINTER-BATCH] draws="' kernel/src/console.c || {
    echo "ITEM-480: запись пачек не названа" >&2
    exit 1
}
for item480_record in ' events=' ' batches=' ' draw_us=' ' sync_us=' ' budget='; do
    grep -Fq "\"$item480_record\"" kernel/src/console.c || {
        echo "ITEM-480: запись пачек не публикует" "$item480_record" >&2
        exit 1
    }
done
grep -Fq 'console_service_line(batch_record, batch_length);' \
    kernel/src/console.c || {
    echo "ITEM-480: запись пачек не уходит в serial и журнал" >&2
    exit 1
}

# ------------------------------------------------------------- host proofs --
grep -Fq 'static void test_mouse_turn_budget_and_coalescing(void) {' \
    tests/local-input-host.c || {
    echo "ITEM-480: нет host-пробы бюджета и coalescing" >&2
    exit 1
}
for item480_claim in \
    'the budget bounds how many batches one turn may draw' \
    'a run of movements is summed into one batch' \
    'a press breaks the run and is its own batch' \
    'the unserved remainder is served by the next call' \
    'no release is lost to the budget' \
    'every queued report was served exactly once'; do
    grep -Fq -- "$item480_claim" tests/local-input-host.c || {
        echo "ITEM-480: host-проба не утверждает:" "$item480_claim" >&2
        exit 1
    }
done
grep -Fq 'static void test_mouse_zero_move_is_not_a_movement(void) {' \
    tests/local-input-host.c || {
    echo "ITEM-480: нет host-пробы пустого движения" >&2
    exit 1
}
grep -Fq 'static void test_keyboard_does_not_wait_for_the_mouse(void) {' \
    tests/local-input-host.c || {
    echo "ITEM-480: нет host-пробы приоритета клавиатуры" >&2
    exit 1
}
grep -Fq 'the keyboard queue is drained whole before the pointer is served' \
    tests/local-input-host.c || {
    echo "ITEM-480: host-проба не держит приоритет клавиатуры" >&2
    exit 1
}
grep -Fq 'test_mouse_turn_budget_and_coalescing();' tests/local-input-host.c || {
    echo "ITEM-480: host-проба бюджета не вызывается" >&2
    exit 1
}
grep -Fq 'static void test_mouse_batch_coalesces_and_drains_once(void) {' \
    tests/console-host.c || {
    echo "ITEM-480: нет host-пробы батча консоли" >&2
    exit 1
}
grep -Fq 'a coalesced movement run draws the arrow once' tests/console-host.c || {
    echo "ITEM-480: host-проба консоли не считает отрисовки" >&2
    exit 1
}
grep -Fq 'the input batch drains the write-combining stores exactly once' \
    tests/console-host.c || {
    echo "ITEM-480: host-проба консоли не считает барьер" >&2
    exit 1
}
grep -Fq 'test_mouse_batch_coalesces_and_drains_once();' tests/console-host.c || {
    echo "ITEM-480: host-проба консоли не вызывается" >&2
    exit 1
}

# --------------------------------------------------------------- QEMU proof --
grep -Fq 'r"\[POINTER-BATCH\]([^\r\n]*)"' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-480: QEMU-проба не ловит запись пачек" >&2
    exit 1
}
grep -Fq 'batch_fields["budget"]' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-480: QEMU-проба не читает бюджет" >&2
    exit 1
}
grep -Fq "the pointer's turn budget is not the named three batches" \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-480: QEMU-проба не требует именованный бюджет" >&2
    exit 1
}
grep -Fq 'the pointer drew as many batches as reports it consumed' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-480: QEMU-проба не требует отрисовок меньше отчётов" >&2
    exit 1
}

# ------------------------------------------------------------- the documents --
for item480_doc in 'ITEM-480' 'POINTER-BATCH' 'LOCAL_INPUT_MOUSE_DRAW_BUDGET'; do
    grep -Fq -- "$item480_doc" docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-480:" \
            "$item480_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-480' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-480" >&2
    exit 1
}
grep -Fq 'ITEM-480' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-480" >&2
    exit 1
}
