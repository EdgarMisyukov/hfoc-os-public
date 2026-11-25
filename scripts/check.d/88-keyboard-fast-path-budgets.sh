#!/usr/bin/env bash
# ITEM-482: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The keyboard's turn budget holds the loop, typing paints one cell, and the
# input batch drains once (СРОЧНО-31, round 20).
#
# The round-19 hardware run (`/srv/hfoc-example/queue/hw9-log-all.txt`) left the
# keyboard's queue drained whole on every turn, so continuous typing could
# starve the pointer's three batches, and every typed character paid a full
# `redraw_input_line()` - `"\r"`, the prompt and the whole line, each with its
# own framebuffer drain - so the echo cost grew with the line's length.  This
# item:
#
#   * appends a character at the end of the line with `console_putc()` and keeps
#     the full redraw only for edits that move text (mid-line insert, LEFT,
#     RIGHT, HOME, END, DELETE, history);
#   * gives the keyboard the named `LOCAL_INPUT_KEYBOARD_BUDGET` (8) events per
#     turn, so neither input source can make one turn unbounded, with the
#     unserved remainder staying queued for the next turn;
#   * drains the whole input batch once in `console_end_input_batch()` instead
#     of once per batch/report;
#   * publishes `kbd_events`/`kbd_budget`/`kbd_draw_us`/`kbd_sync_us` and
#     `ptr_events`/`ptr_draws`/`ptr_budget` in `[LOOP-PHASES]`.

for item482_file in \
    kernel/include/local_input.h \
    kernel/include/console.h \
    kernel/src/console.c \
    kernel/src/kernel.c \
    tests/console-host.c \
    Makefile \
    scripts/test-qemu-production-storage-boot.sh \
    docs/hfoc-appliance-boot-console.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item482_file ]] || {
        echo "ITEM-482: отсутствует $item482_file" >&2
        exit 1
    }
done

# ----------------------------------------- the named keyboard turn budget ----
grep -Fq '#define LOCAL_INPUT_KEYBOARD_BUDGET 8U' \
    kernel/include/local_input.h || {
    echo "ITEM-482: бюджет оборота клавиатуры не назван" >&2
    exit 1
}

# ------------------------------------------------ the bounded loop in kernel --
grep -Fq 'keyboard_served < LOCAL_INPUT_KEYBOARD_BUDGET &&' \
    kernel/src/kernel.c || {
    echo "ITEM-482: дренаж клавиатуры не ограничен бюджетом" >&2
    exit 1
}
grep -Fq 'kernel_loop_input_kbd_events += keyboard_served;' \
    kernel/src/kernel.c || {
    echo "ITEM-482: оборот не считает обслуженные события клавиатуры" >&2
    exit 1
}
grep -Fq 'if (mouse_batches[mouse_batch_index].dx != 0 ||' \
    kernel/src/kernel.c || {
    echo "ITEM-482: оборот не считает отрисовки указателя" >&2
    exit 1
}
grep -Fq 'console_end_input_batch();' kernel/src/kernel.c || {
    echo "ITEM-482: входная пачка не закрывается одним барьером" >&2
    exit 1
}
if grep -Fq 'while (local_input_pop_event(&event))' kernel/src/kernel.c; then
    echo "ITEM-482: клавиатура всё ещё дренируется безлимитно" >&2
    exit 1
fi

# ------------------------------------------------- the fast path and one drain --
grep -Fq 'void console_end_input_batch(void) {' kernel/src/console.c || {
    echo "ITEM-482: нет одного барьера входной пачки" >&2
    exit 1
}
grep -Fq 'uint64_t console_keyboard_sync_us(void) {' kernel/src/console.c || {
    echo "ITEM-482: цена барьера клавиатуры не считается" >&2
    exit 1
}
grep -Fq 'console_input_needs_sync = true;' kernel/src/console.c || {
    echo "ITEM-482: отрисовка указателя не отмечает отложенный барьер" >&2
    exit 1
}
for item482_fast in \
    'if (input_cursor == input_length) {' \
    'input_line[input_cursor] = event->character;' \
    'console_putc(event->character);' \
    "console_putc('\\b');"; do
    grep -Fq -- "$item482_fast" kernel/src/console.c || {
        echo "ITEM-482: быстрый путь набора не содержит:" "$item482_fast" >&2
        exit 1
    }
done
grep -Fq 'console_putc(event->character);' kernel/src/console.c || {
    echo "ITEM-482: символ не отдаётся painter'у одним знакоместом" >&2
    exit 1
}

# ------------------------------------------------------ the phase record ----
for item482_field in \
    '" kbd_events="' \
    '" kbd_budget="' \
    '" kbd_draw_us="' \
    '" kbd_sync_us="' \
    '" ptr_events="' \
    '" ptr_draws="' \
    '" ptr_budget="'; do
    grep -Fq -- "$item482_field" kernel/src/kernel.c || {
        echo "ITEM-482: [LOOP-PHASES] не публикует" "$item482_field" >&2
        exit 1
    }
done
grep -Fq 'kernel_loop_input_kbd_window_events' kernel/src/kernel.c || {
    echo "ITEM-482: окно не держит худший оборот клавиатуры" >&2
    exit 1
}
grep -Fq 'kernel_loop_input_ptr_window_draws' kernel/src/kernel.c || {
    echo "ITEM-482: окно не держит худший оборот указателя" >&2
    exit 1
}

# ------------------------------------------------------------- host proofs --
grep -Fq 'static void test_typing_appends_without_a_full_redraw(void) {' \
    tests/console-host.c || {
    echo "ITEM-482: нет host-пробы быстрого пути набора" >&2
    exit 1
}
for item482_claim in \
    'appending a character paints one cell, not a whole-line redraw' \
    'erasing the last character paints one cell, not a whole-line redraw'; do
    grep -Fq -- "$item482_claim" tests/console-host.c || {
        echo "ITEM-482: host-проба не утверждает:" "$item482_claim" >&2
        exit 1
    }
done
grep -Fq 'test_typing_appends_without_a_full_redraw();' tests/console-host.c || {
    echo "ITEM-482: host-проба быстрого пути не вызывается" >&2
    exit 1
}
grep -Fq 'the input batch drains the write-combining stores exactly once' \
    tests/console-host.c || {
    echo "ITEM-482: host-проба не держит один барьер" >&2
    exit 1
}

# --------------------------------------------------------------- QEMU proof --
for item482_qemu in \
    '"kbd_events", "kbd_budget", "kbd_draw_us", "kbd_sync_us",' \
    '"ptr_events", "ptr_draws", "ptr_budget",' \
    'a turn served more keyboard events than the keyboard budget' \
    'a turn drew the pointer more times than the pointer budget'; do
    grep -Fq -- "$item482_qemu" \
        scripts/test-qemu-production-storage-boot.sh || {
        echo "ITEM-482: QEMU-проба не требует:" "$item482_qemu" >&2
        exit 1
    }
done
# The one-cell fast path changes the echo shape the console proofs witness: the
# typed characters, not a full redraw, confirm delivery.  The helper and the
# load proof must carry the record-stripped, contiguous witness so a lost
# character still breaks the run.
grep -Fq 'def delivery_view(text):' scripts/hfoc_console_command.py || {
    echo "ITEM-482: helper не несёт witness быстрого пути" >&2
    exit 1
}
# ITEM-511 re-pinned the expression: the witness transform of the type_line
# option (default identity) wraps both views, so the contiguous-run check is
# still there, now on the normalised line.
grep -Fq 'wanted_line in confirm(delivery_view(' scripts/hfoc_console_command.py || {
    echo "ITEM-482: type_line не подтверждает строку по символам" >&2
    exit 1
}
grep -Fq 'endswith(prefix)' scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-482: load-проба не сверяет набранный префикс" >&2
    exit 1
}

# ------------------------------------------------------------- the documents --
for item482_doc in \
    'ITEM-482' \
    'LOCAL_INPUT_KEYBOARD_BUDGET' \
    'console_end_input_batch' \
    'kbd_events'; do
    grep -Fq -- "$item482_doc" docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-482:" \
            "$item482_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-482' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-482" >&2
    exit 1
}
grep -Fq 'ITEM-482' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-482" >&2
    exit 1
}

# ------------------------------------ ITEM-483: foreign output invalidates ----
# The 2026-09-26 load proof lost `e`/`f` of `abcdef`: journal-only records landed
# between the typed characters on the serial wire (one of them, `[PRODUCTION-INPUT]`,
# right after `abcd`), so the typed prefix stopped being contiguous.  A painted
# record has the same effect through the framebuffer.  The fast path now paints
# one cell only while the output generation the line was rendered against is
# unchanged; every foreign emit, every record opened and `clear_body()` bump it,
# and the next keystroke does the full redraw that re-emits the prompt and the
# whole line to both surfaces.
for item483_generation in \
    'static uint64_t console_output_generation;' \
    'static uint64_t console_input_line_generation;' \
    'console_input_line_generation = console_output_generation;' \
    '++console_output_generation;'; do
    grep -Fq -- "$item483_generation" kernel/src/console.c || {
        echo "ITEM-483: поколение вывода не содержит:" \
            "$item483_generation" >&2
        exit 1
    }
done
grep -Fq 'if (!console_replaying && !console_keyboard_echo_active) {' \
    kernel/src/console.c || {
    echo "ITEM-483: чужой вывод не поднимает поколение" >&2
    exit 1
}
grep -Fq 'ITEM-483: assembling a record is foreign output even when it never paints' \
    kernel/src/console.c || {
    echo "ITEM-483: открытие записи не инвалидирует быстрый путь" >&2
    exit 1
}
grep -Fq 'console_input_line_generation ==' kernel/src/console.c || {
    echo "ITEM-483: быстрый путь набора не проверяет поколение вывода" >&2
    exit 1
}
grep -Fq 'test_a_foreign_record_forces_the_typing_redraw' tests/console-host.c || {
    echo "ITEM-483: нет host-пробы инвалидации после чужой записи" >&2
    exit 1
}
grep -Fq 'the first keystroke after a painted record re-emits the prompt' \
    tests/console-host.c || {
    echo "ITEM-483: host-проба не утверждает возврат промпта и строки" >&2
    exit 1
}
grep -Fq 'a journal-only record forces the prompt and the whole line back' \
    tests/console-host.c || {
    echo "ITEM-483: host-проба не покрывает journal-only запись" >&2
    exit 1
}
grep -Fq 'test_a_foreign_record_forces_the_typing_redraw();' \
    tests/console-host.c || {
    echo "ITEM-483: host-проба инвалидации не вызывается" >&2
    exit 1
}
grep -Fq 'the fast path resumes for the character after the forced redraw' \
    tests/console-host.c || {
    echo "ITEM-483: host-проба не требует возврата быстрого пути" >&2
    exit 1
}
for item483_doc in \
    'ITEM-483' \
    'console_output_generation'; do
    grep -Fq -- "$item483_doc" docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-483:" \
            "$item483_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-483' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-483" >&2
    exit 1
}
grep -Fq 'ITEM-483' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-483" >&2
    exit 1
}
