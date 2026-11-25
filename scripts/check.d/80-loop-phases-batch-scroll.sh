#!/usr/bin/env bash
# ITEM-474: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The turn reports its phases, and the body moves once (СРОЧНО-24, round 13).
#
# The round-12 hardware run (`/srv/hfoc-example/queue/hw6-log-all.txt`) read a loop
# that turned 2.4 times a second (`iter_us=424 022`, `loops` 99 -> 195 over 42
# real seconds, `tick_hz=2`) whose only endpoint-ended turn was `cpu_hlt()`, and
# a scroll that moved the whole 3840x2160 body once per line
# (`[CONSOLE] scroll rows=1 bytes=31979520 us=7198`).  The owner saw it as
# "movement is jerky".  This item makes the loop publish where its time went and
# makes a turn move the body once for all the lines it printed:
#
#   * `[LOOP-PHASES]` once a second carries `turns= console_us= xhci_us=
#     input_us= net_us= other_us= work_us= hlt_us= turn_us=`; the console's own
#     TSC-measured cost is subtracted from every measured span, so the phases
#     are disjoint and `hlt_us` separates sleeping from working.
#   * `console_scroll_pending_cells`/`console_defer_buffer` defer the turn's
#     overflow, and `console_flush_deferred()` - called by the main loop before
#     `cpu_hlt()` - does one `framebuffer_scroll_up()` for the whole batch and
#     replays the buffered characters.
#   * `tests/console-host.c` proves eight lines move the body once; the QEMU
#     proof reads the full `[LOOP-PHASES]` field set with a bounded `turn_us`.

for item474_file in \
    kernel/include/console.h \
    kernel/src/console.c \
    kernel/src/kernel.c \
    tests/console-host.c \
    scripts/test-qemu-production-storage-boot.sh \
    docs/hfoc-appliance-boot-console.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item474_file ]] || {
        echo "ITEM-474: отсутствует $item474_file" >&2
        exit 1
    }
done

# ---------------------------------------------------------------------- B --
# A turn batches its overflow: the pending row count, the deferred paint
# buffer, one body move for the whole batch and the replay behind it.
#
# ITEM-509 increment 3 moved the replay from a byte loop to a decoder, because
# the batch now holds whole code points: the replay still walks the buffer in
# order and still paints through the console's own emitters - `console_emit()`
# for the bytes that are one cell, `console_emit_codepoint()` for a decoded
# sequence - so the pin below names both, and a replay that stopped decoding
# would fail here rather than paint half a letter.
for item474_batch_impl in \
    '#define CONSOLE_DEFER_MAX 4096U' \
    'static char console_defer_buffer[CONSOLE_DEFER_MAX];' \
    'static uint64_t console_scroll_pending_cells;' \
    'static void console_scroll_apply(uint64_t cells) {' \
    'framebuffer_scroll_up(BODY_TOP, cells * console_cell_height,' \
    'void console_flush_deferred(void) {' \
    'console_scroll_apply(cells);' \
    'console_emit((char)byte, false);' \
    'appliance_language_utf8_next(console_defer_buffer,' \
    'console_emit_codepoint(codepoint,' \
    'pointer_suspend();' \
    'static uint64_t console_scroll_batch_limit(void) {' \
    'if (console_scroll_pending_cells >= console_scroll_batch_limit()) {'; do
    grep -Fq -- "$item474_batch_impl" kernel/src/console.c || {
        echo "ITEM-474: пакетная прокрутка не содержит:" \
            "$item474_batch_impl" >&2
        exit 1
    }
done
for item474_batch_glue in \
    '++console_scroll_pending_cells;' \
    'console_deferring = true;' \
    'console_begin_turn_batching(void) {' \
    'console_defer_batching = true;' \
    'console_cost_us_total += elapsed_us;' \
    'uint64_t console_cost_us(void) {'; do
    grep -Fq -- "$item474_batch_glue" kernel/src/console.c || {
        echo "ITEM-474: состояние пакета не реализовано:" \
            "$item474_batch_glue" >&2
        exit 1
    }
done
for item474_console_api in \
    'uint64_t console_cost_us(void);' \
    'void console_begin_turn_batching(void);' \
    'void console_flush_deferred(void);'; do
    grep -Fq -- "$item474_console_api" kernel/include/console.h || {
        echo "ITEM-474: API консоли не объявлен:" "$item474_console_api" >&2
        exit 1
    }
done

# ---------------------------------------------------------------------- A --
# The loop's phase accounting and the once-a-second record.
for item474_phase_impl in \
    'enum kernel_loop_phase {' \
    'KERNEL_LOOP_PHASE_XHCI = 0,' \
    'KERNEL_LOOP_PHASE_INPUT,' \
    'KERNEL_LOOP_PHASE_NETWORK,' \
    'kernel_loop_phase_us[KERNEL_LOOP_PHASE_COUNT]' \
    'static void kernel_loop_phase_turn_begin(void) {' \
    'static void kernel_loop_phase_turn_end(void) {' \
    'console_spent = console_cost_us() - span.console_us;' \
    'static void log_production_loop_phases(void) {'; do
    grep -Fq -- "$item474_phase_impl" kernel/src/kernel.c || {
        echo "ITEM-474: фазовый учёт не реализован:" \
            "$item474_phase_impl" >&2
        exit 1
    }
done
for item474_phase_field in \
    '"[LOOP-PHASES] turns="' \
    '" console_us="' \
    '" xhci_us="' \
    '" input_us="' \
    '" net_us="' \
    '" other_us="' \
    '" work_us="' \
    '" hlt_us="' \
    '" turn_us="'; do
    grep -Fq -- "$item474_phase_field" kernel/src/kernel.c || {
        echo "ITEM-474: запись фаз не несёт:" "$item474_phase_field" >&2
        exit 1
    }
done
grep -Fq 'kernel_loop_phase_end(KERNEL_LOOP_PHASE_XHCI, phase_span);' \
    kernel/src/kernel.c || {
    echo "ITEM-474: xhci-фаза не измеряется" >&2
    exit 1
}
grep -Fq 'kernel_loop_phase_end(KERNEL_LOOP_PHASE_INPUT, phase_span);' \
    kernel/src/kernel.c || {
    echo "ITEM-474: фаза ввода не измеряется" >&2
    exit 1
}
grep -Fq 'kernel_loop_phase_end(KERNEL_LOOP_PHASE_NETWORK, phase_span);' \
    kernel/src/kernel.c || {
    echo "ITEM-474: сетевая фаза не измеряется" >&2
    exit 1
}
grep -Fq 'log_production_loop_phases();' kernel/src/kernel.c || {
    echo "ITEM-474: запись фаз не публикуется" >&2
    exit 1
}
grep -Fq 'kernel_loop_phase_turn_begin();' kernel/src/kernel.c || {
    echo "ITEM-474: окно оборота не открывается" >&2
    exit 1
}
grep -Fq 'kernel_loop_phase_turn_end();' kernel/src/kernel.c || {
    echo "ITEM-474: окно оборота не закрывается" >&2
    exit 1
}
grep -Fq 'console_flush_deferred();' kernel/src/kernel.c || {
    echo "ITEM-474: главный цикл не сбрасывает пакет" >&2
    exit 1
}

# ------------------------------------------------------------- host proofs --
for item474_claim in \
    "the turn's overflowing lines do not move the body one by one" \
    'no deferred character is painted before the flush' \
    'the flush replays the deferred lines onto the screen' \
    'the whole turn'"'"'s overflow moves the body exactly once' \
    'the batch reports all eight rows as one atomic record' \
    'a wrapped deferred line counts every row in the batch' \
    'a wrapped deferred line still moves the body once' \
    'the batched menu publishes the row the target is on' \
    'the batched menu publishes the rectangle'"'"'s column and width'; do
    grep -Fq -- "$item474_claim" tests/console-host.c || {
        echo "ITEM-474: host-проба не утверждает:" "$item474_claim" >&2
        exit 1
    }
done
grep -Fq 'void test_console_scroll_batch(void) {' tests/console-host.c || {
    echo "ITEM-474: host-пробы пакетной прокрутки нет" >&2
    exit 1
}
grep -Fq 'uint64_t console_test_scroll_rows(void);' kernel/include/console.h || {
    echo "ITEM-474: host-доступ к числу прокрученных строк не объявлен" >&2
    exit 1
}
grep -Fq 'uint64_t console_test_scroll_rows(void) {' kernel/src/console.c || {
    echo "ITEM-474: host-доступ к числу прокрученных строк не реализован" >&2
    exit 1
}

# --------------------------------------------------------------- QEMU proof --
grep -Fq '[LOOP-PHASES]' scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-474: QEMU-проба не читает запись фаз" >&2
    exit 1
}
grep -Fq 'phases_required = (' scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-474: QEMU-проба не требует поля записи фаз" >&2
    exit 1
}
grep -Fq 'the loop'"'"'s phases exceed the turn'"'"'s work' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-474: QEMU-проба не сверяет фазы с работой" >&2
    exit 1
}
grep -Fq 'int(phases_fields["turn_us"]) > 500000' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-474: QEMU-проба не держит бюджет оборота" >&2
    exit 1
}

# ------------------------------------------------------------- the documents --
for item474_doc in \
    'ITEM-474' \
    'LOOP-PHASES' \
    'console_scroll_pending_cells' \
    'console_flush_deferred' \
    'console_defer_buffer' \
    'turn_us'; do
    grep -Fq -- "$item474_doc" docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-474:" \
            "$item474_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-474' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-474" >&2
    exit 1
}
grep -Fq 'ITEM-474' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-474" >&2
    exit 1
}
