#!/usr/bin/env bash
# ITEM-461: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The console keeps up with the input, and says what it cost.
#
# The owner's 2026-09-20 report on build ee01b958 was that the keyboard lost
# characters, the mouse and the redraw were slow, and the resolution was the
# suspected cause - but nothing published the mode, the loop's cost or the
# console's own paint cost, and the screen was flooded by `[BOOTLOG]` and
# `[PRODUCTION-SERVING]` records.  The claims pinned here:
#
#   * the boot publishes the mode it selected as
#     `[FB] width= height= pitch= cells=WxH bpp=`, on the serial stream and in
#     the durable journal, so "the resolution may not fit" is a fact;
#   * `[BOOTLOG]` reaches the journal and is never painted: it is the log's own
#     bookkeeping, and printing one per flush was the flood that scrolled the
#     console continuously;
#   * `[PRODUCTION-SERVING]` still publishes a state change immediately and an
#     unchanged state at most once per interval, because its counters move
#     every tick;
#   * the costs reach the durable journal: `[CONSOLE] scroll` (as before) and
#     `[CONSOLE] paint lines= chars= ms= max_ms=`; the main loop's turn is
#     measured and published as `iter_ms=`/`iter_max_ms=` on the heartbeat;
#   * the interrupt transfer is re-armed at the end of the same service pass
#     that consumed the completion, and the longest wait is published as
#     `kbd_repost_ms_max=` on the heartbeat, beside the queue's own
#     `kbd_dropped=`;
#   * the QEMU proof drives 200+ typed characters and several screens of output
#     in the same guest, requires every load line to reach the console, requires
#     the queue's drop count to be zero, and bounds the paint and loop costs; it
#     runs at the firmware default and at 1920x1080 and 2560x1440 through an
#     EDID, with the `[FB]` record asserting the mode really changed.
for console_budget_file in \
    kernel/include/console.h \
    kernel/src/console.c \
    kernel/include/framebuffer.h \
    kernel/src/framebuffer.c \
    kernel/include/xhci.h \
    kernel/src/xhci.c \
    kernel/src/kernel.c \
    tests/console-host.c \
    scripts/test-qemu-console-history.sh \
    docs/hfoc-appliance-boot-console.md; do
    [[ -f $console_budget_file ]] || {
        echo "ITEM-461: отсутствует $console_budget_file" >&2
        exit 1
    }
done

# The public journal-only publication exists and is the static journal path -
# a cost record must not paint.
grep -Fq 'void console_publish_journal(const char *record, size_t length);' \
    kernel/include/console.h || {
    echo "ITEM-461: console.h не объявляет журнальную публикацию" >&2
    exit 1
}
grep -Fq 'void console_publish_journal(const char *record, size_t length) {' \
    kernel/src/console.c || {
    echo "ITEM-461: нет реализации журнальной публикации" >&2
    exit 1
}
if awk '/^void console_publish_journal\(/ { inside = 1 }
         inside { print }
         inside && /^}/ { exit }' kernel/src/console.c |
        grep -Fq 'console_publish_record'; then
    echo "ITEM-461: журнальная публикация рисует экран" >&2
    exit 1
fi

# The mode is published, serial included, with pitch and bpp getters.
for console_budget_display in \
    'void console_publish_display(uint64_t width, uint64_t height, uint64_t pitch,' \
    '"[FB] width="' \
    'uint64_t framebuffer_pitch(void);' \
    'uint32_t framebuffer_bpp(void);' \
    'uint64_t framebuffer_pitch(void) {' \
    'uint32_t framebuffer_bpp(void) {'; do
    grep -Fq -- "$console_budget_display" \
        kernel/include/console.h kernel/src/console.c \
        kernel/include/framebuffer.h kernel/src/framebuffer.c || {
        echo "ITEM-461: нет записи режима:" "$console_budget_display" >&2
        exit 1
    }
done
grep -Fq 'framebuffer_width(), framebuffer_height(), framebuffer_pitch(),' \
    kernel/src/kernel.c || {
    echo "ITEM-461: boot не публикует режим" >&2
    exit 1
}

# `[BOOTLOG]` goes to the journal, not the screen.
grep -Fq 'console_publish_journal(record, length);' kernel/src/kernel.c || {
    echo "ITEM-461: [BOOTLOG] всё ещё рисуется" >&2
    exit 1
}

# The costs: the scroll record reaches the journal; the paint record is
# journal-only and counted; the loop turn is measured and published.
grep -Fq 'console_publish_journal(scroll_record, length);' kernel/src/console.c || {
    echo "ITEM-461: стоимость прокрутки не в журнале" >&2
    exit 1
}
grep -Fq '"[CONSOLE] paint lines="' kernel/src/console.c || {
    echo "ITEM-461: нет записи стоимости отрисовки" >&2
    exit 1
}
grep -Fq 'static void console_publish_paint(void) {' kernel/src/console.c || {
    echo "ITEM-461: нет публикации стоимости отрисовки" >&2
    exit 1
}
grep -Fq 'console_publish_journal(paint_record, length);' kernel/src/console.c || {
    echo "ITEM-461: запись стоимости отрисовки не в журнале" >&2
    exit 1
}
grep -Fq 'static void log_production_iteration(void)' kernel/src/kernel.c || {
    echo "ITEM-461: нет измерения итерации главного цикла" >&2
    exit 1
}
grep -Fq 'log_production_iteration();' kernel/src/kernel.c || {
    echo "ITEM-461: главный цикл не измеряется" >&2
    exit 1
}
grep -Fq 'NET_RECORD_WRITE(" iter_us=");' kernel/src/kernel.c || {
    echo "ITEM-461/463: heartbeat не публикует iter_us" >&2
    exit 1
}
grep -Fq 'NET_RECORD_WRITE(" iter_max_us=");' kernel/src/kernel.c || {
    echo "ITEM-461/463: heartbeat не публикует iter_max_us" >&2
    exit 1
}

# The serving record is coalesced when the state is unchanged.
grep -Fq '#define PRODUCTION_SERVING_PUBLISH_INTERVAL_MS 2000U' \
    kernel/src/kernel.c || {
    echo "ITEM-461: [PRODUCTION-SERVING] не объединён по времени" >&2
    exit 1
}

# The re-arm wait is measured from the completion and published.
grep -Fq 'uint32_t repost_ms_max;' kernel/src/xhci.c || {
    echo "ITEM-461: нет счётчика переподстановки" >&2
    exit 1
}
grep -Fq 'device->transfer_complete_ticks = timer_ticks();' kernel/src/xhci.c || {
    echo "ITEM-461: завершение передачи не запоминает тик" >&2
    exit 1
}
grep -Fq 'if (elapsed_ms > device->repost_ms_max) {' kernel/src/xhci.c || {
    echo "ITEM-461: задержка переподстановки не измеряется" >&2
    exit 1
}
grep -Fq 'uint32_t xhci_keyboard_repost_ms_max(void);' kernel/include/xhci.h || {
    echo "ITEM-461: нет геттера переподстановки" >&2
    exit 1
}
grep -Fq 'NET_RECORD_WRITE(" kbd_repost_ms_max=");' kernel/src/kernel.c || {
    echo "ITEM-461: heartbeat не публикует переподстановку" >&2
    exit 1
}

# The host proof drives the journal-only path and the display record.
for console_budget_host in \
    'a journal-only record reaches the durable log' \
    'a journal-only record is never painted' \
    'the display geometry reaches the durable log' \
    'the display geometry is not painted for the operator' \
    'kbd command prints the live USB HID counters'; do
    grep -Fq -- "$console_budget_host" tests/console-host.c || {
        echo "ITEM-461: host-проба не требует:" "$console_budget_host" >&2
        exit 1
    }
done

# The QEMU proof types 200+ characters and bounds both costs, in two modes.
for console_budget_qemu in \
    'LOAD_LINE_LENGTH = 80' \
    'LOAD_LINE_COUNT = 3' \
    'PAINT_US_BUDGET = 50000' \
    'ITER_US_BUDGET = 500000' \
    'HFOC_CONSOLE_HISTORY_DISPLAY' \
    'HFOC_CONSOLE_HISTORY_EXPECT_DISPLAY' \
    'the requested display' \
    'the console queue dropped' \
    'the kbd counters carried no [CONSOLE] paint line' \
    'the kbd counters carried no [PRODUCTION-LOOP] cost line'; do
    grep -Fq -- "$console_budget_qemu" scripts/test-qemu-console-history.sh || {
        echo "ITEM-461: QEMU-проба не требует:" "$console_budget_qemu" >&2
        exit 1
    }
done
# shellcheck disable=SC2016  # the Makefile text is what the search must match
for console_budget_mode in \
    "HFOC_CONSOLE_HISTORY_DISPLAY='VGA,edid=on,xres=1920,yres=1080'" \
    "HFOC_CONSOLE_HISTORY_DISPLAY='VGA,edid=on,xres=3840,yres=2160'"; do
    grep -Fq -- "$console_budget_mode" Makefile || {
        echo "ITEM-461: Makefile не прогоняет режим:" "$console_budget_mode" >&2
        exit 1
    }
done

# The item is recorded where a maintainer looks.
for console_budget_doc in \
    'ITEM-461' \
    '[FB] width=' \
    '[CONSOLE] paint' \
    'repost_ms_max'; do
    grep -Fq -- "$console_budget_doc" docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-461:" \
            "$console_budget_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-461' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-461" >&2
    exit 1
}
grep -Fq 'ITEM-461' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-461" >&2
    exit 1
}
