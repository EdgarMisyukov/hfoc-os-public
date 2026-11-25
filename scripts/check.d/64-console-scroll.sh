#!/usr/bin/env bash
# ITEM-457: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The console's scroll moves rows, and says what it cost.
#
# The 2026-09-19 hardware report was "when the output reaches the bottom of the
# screen, the refresh is very slow", and the same run showed the keyboard -
# polled from the console's own main loop - answering late while it happened.
# The cause was in framebuffer_scroll_up(): the body was copied one pixel at a
# time, so one scrolled text row on a 1920x1080 screen was about two million
# scalar reads and writes, once per printed line once the screen was full.
#
# The claims pinned here are what the screen does after the item:
#
#   * the move is a row move: whole rows are copied with 8-byte words where the
#     row's alignment allows it and 4-byte words otherwise, and the per-pixel
#     inner loop that caused the report is gone;
#   * the rows that came in from the bottom are filled with the same wide
#     access, and the rows above the scrolled region are never written - the
#     scroll is a move, not a repaint;
#   * framebuffer_scroll_up() returns the bytes it wrote, so the console can
#     publish a measured cost instead of a claimed one;
#   * the console measures the scroll with the machine's tick - one scroll ends
#     inside one tick, so the per-scroll reading would be "0 or 1"; the record
#     reports the average of the window and the worst single scroll - and
#     publishes `[CONSOLE] scroll rows=... bytes=... ms=... max_ms=...` as one
#     atomic write, because a proof waits for the whole line;
#   * the report is a summary - the first scroll, a scroll worse than any
#     reported before, or every sixteen rows - so publishing it cannot cost a
#     serial write per scrolled row, which is the cost the item is about;
#   * a line that still fits does not scroll at all, and the host proofs read
#     the pixels back instead of trusting the move: one drives framebuffer.c
#     against a host surface, the other drives the console and reads the
#     record it published;
#   * the QEMU proof types several screens of output into the guest and
#     requires the records to grow, to move the same row block every time, and
#     to stay inside the budget.
for console_scroll_file in \
    kernel/include/framebuffer.h \
    kernel/src/framebuffer.c \
    kernel/include/console.h \
    kernel/src/console.c \
    tests/framebuffer-scroll-host.c \
    tests/console-host.c \
    scripts/test-qemu-console-history.sh \
    docs/hfoc-appliance-boot-console.md; do
    [[ -f $console_scroll_file ]] || {
        echo "ITEM-457: отсутствует $console_scroll_file" >&2
        exit 1
    }
done

# The scroll returns the bytes it wrote, and both sides declare the same shape.
grep -Fq 'uint64_t framebuffer_scroll_up(uint64_t top, uint64_t pixels, uint32_t fill_colour);' \
    kernel/include/framebuffer.h || {
    echo "ITEM-457: framebuffer.h не объявляет измеряемую прокрутку" >&2
    exit 1
}
grep -Fq 'uint64_t framebuffer_scroll_up(uint64_t top, uint64_t pixels, uint32_t fill_colour) {' \
    kernel/src/framebuffer.c || {
    echo "ITEM-457: прокрутка не возвращает записанные байты" >&2
    exit 1
}

# The row move: wide stores first, and the per-pixel loop is gone.
grep -Fq 'static void framebuffer_copy_row(volatile uint8_t *destination,' \
    kernel/src/framebuffer.c || {
    echo "ITEM-457: нет построчного копирования" >&2
    exit 1
}
grep -Fq 'for (; offset + 8U <= bytes; offset += 8U) {' \
    kernel/src/framebuffer.c || {
    echo "ITEM-457: строка не копируется восьмибайтовыми словами" >&2
    exit 1
}
grep -Fq 'for (; offset + 4U <= bytes; offset += 4U) {' \
    kernel/src/framebuffer.c || {
    echo "ITEM-457: у построчного копирования нет четырёхбайтового пути" >&2
    exit 1
}
if grep -Fq 'address[y * pitch_words + x] = address[(y + pixels) * pitch_words + x];' \
    kernel/src/framebuffer.c; then
    echo "ITEM-457: прокрутка всё ещё копирует по пикселю" >&2
    exit 1
fi
# The rows that came in from the bottom are filled, not repainted pixel by
# pixel, and the copy still happens row by row above them.
grep -Fq 'static void framebuffer_fill_row(volatile uint8_t *destination, uint64_t bytes,' \
    kernel/src/framebuffer.c || {
    echo "ITEM-457: входящие строки не заливаются широкой записью" >&2
    exit 1
}
grep -Fq 'framebuffer_copy_row(base + y * pitch, base + (y + pixels) * pitch,' \
    kernel/src/framebuffer.c || {
    echo "ITEM-457: прокрутка не переносит строку целиком" >&2
    exit 1
}
grep -Fq 'for (uint64_t y = active_framebuffer->height - pixels;' \
    kernel/src/framebuffer.c || {
    echo "ITEM-457: прокрутка не заливает только входящие строки" >&2
    exit 1
}

# The console publishes the measurement, atomically, and only from a cursor
# that really left the screen.
grep -Fq '"[CONSOLE] scroll rows="' kernel/src/console.c || {
    echo "ITEM-457: консоль не публикует стоимость прокрутки" >&2
    exit 1
}
grep -Fq 'console_write_atomic(scroll_record);' kernel/src/console.c || {
    echo "ITEM-457: запись о прокрутке не атомарна" >&2
    exit 1
}
grep -Fq 'const uint64_t started = timer_cycles();' kernel/src/console.c || {
    echo "ITEM-457: прокрутка не измеряется часами машины" >&2
    exit 1
}
grep -Fq 'console_scroll_window_us += elapsed_us;' kernel/src/console.c || {
    echo "ITEM-457: измеренное время прокрутки не накапливается" >&2
    exit 1
}
grep -Fq 'static uint64_t console_scroll_average_us(void) {' kernel/src/console.c || {
    echo "ITEM-457: у записи нет средней стоимости прокрутки" >&2
    exit 1
}
grep -Fq '#define CONSOLE_SCROLL_REPORT_ROWS 16U' kernel/src/console.c || {
    echo "ITEM-457: запись о прокрутке не ограничена по частоте" >&2
    exit 1
}
grep -Fq 'if (cursor_y + console_cell_height > framebuffer_height()) {' kernel/src/console.c || {
    echo "ITEM-457: прокрутка не привязана к нижней границе экрана" >&2
    exit 1
}

# The host proofs: framebuffer.c is driven against a host surface and the
# pixels are read back; the console proof reads the record it published.
for console_scroll_proof in \
    'a scrolled row carries the pixels of the row below it' \
    'a row that came in from the bottom carries the fill colour' \
    'a row above the scrolled region is untouched' \
    'row padding is not screen content and is not written' \
    'a 4-byte-aligned row move still copies the row' \
    'the scroll returns the bytes it wrote'; do
    grep -Fq -- "$console_scroll_proof" tests/framebuffer-scroll-host.c || {
        echo "ITEM-457: host-проба framebuffer не требует:" \
            "$console_scroll_proof" >&2
        exit 1
    }
done
for console_scroll_console_proof in \
    'a line that still fits does not scroll the screen' \
    'the scroll publishes its rows, bytes and time as one atomic' \
    'the report is a summary, not one record per scrolled row'; do
    grep -Fq -- "$console_scroll_console_proof" tests/console-host.c || {
        echo "ITEM-457: host-проба консоли не требует:" \
            "$console_scroll_console_proof" >&2
        exit 1
    }
done

# Both proofs are part of the canonical host suite: a drill nobody runs is not
# evidence.
# shellcheck disable=SC2016  # the Makefile text is what the search must match
for console_scroll_registration in \
    'test-framebuffer-scroll-host:' \
    '$(MAKE) test-framebuffer-scroll-host'; do
    grep -Fq -- "$console_scroll_registration" Makefile || {
        echo "ITEM-457: host-проба не зарегистрирована в Makefile:" \
            "$console_scroll_registration" >&2
        exit 1
    }
done
grep -Fq 'test-framebuffer-scroll-host' tests/test-host-suite-manifest.sh || {
    echo "ITEM-457: манифест host suite не содержит test-framebuffer-scroll-host" >&2
    exit 1
}

# The QEMU proof scrolls several screens in the guest and requires the record:
# a budget, the same row block in every report, and a worst cost that does not
# grow across the burst.
for console_scroll_qemu in \
    'SCROLL_US_BUDGET = 250000' \
    'SCROLL_COMMANDS = 30' \
    'SCROLL_MIN_ROWS = 96' \
    'console_command.type_line(send_key, normalised_serial_bytes,' \
    'the bytes one scrolled row moves changed across the burst' \
    'HFOC_CONSOLE_HISTORY_TEST_TIMEOUT:-540'; do
    grep -Fq -- "$console_scroll_qemu" scripts/test-qemu-console-history.sh || {
        echo "ITEM-457: QEMU-проба не требует:" "$console_scroll_qemu" >&2
        exit 1
    }
done

# The item is recorded where a maintainer looks: the console document owns what
# the screen shows and what it publishes, and the queue owns the item itself.
for console_scroll_doc in \
    'ITEM-457' \
    '[CONSOLE] scroll'; do
    grep -Fq -- "$console_scroll_doc" docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-457:" \
            "$console_scroll_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-457' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-457" >&2
    exit 1
}
grep -Fq 'ITEM-457' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-457" >&2
    exit 1
}
