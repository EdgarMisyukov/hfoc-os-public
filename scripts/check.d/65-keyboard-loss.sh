#!/usr/bin/env bash
# ITEM-458: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The keyboard loses no character, and says how long it waited.
#
# The 2026-09-19 hardware report was that the keyboard "answers with delays and
# sometimes skips characters" while the screen scrolled.  ITEM-457 removed the
# stall that made the main loop miss polls (the console copied the screen one
# pixel at a time); this item removes what was left in the receive path:
#
#   * the xHCI service pass took one whole report and returned, so a keystroke
#     that arrived during a busy pass waited for the next main-loop turn - one
#     report per session per pass, with a sixteen-TRB event ring that the
#     controller stops publishing into once it is full;
#   * the bounded queue between the driver and the console refused a report
#     when it was full and said nothing about it, so a lost character was
#     indistinguishable from a key nobody pressed.
#
# What is pinned here:
#
#   * one service pass drains the session's event ring, bounded by
#     XHCI_SERVICE_EVENTS_MAX ring events and XHCI_SERVICE_REPORTS_MAX whole
#     reports, so the round-robin still gives the other controller its turn;
#   * a full local queue counts the refusal (`dropped_report_count`) instead of
#     dropping the newest report silently, and the decision itself is the pure
#     helper `xhci_local_queue_push`, which the host proof drives;
#   * the wait between the two most recent reports a device delivered is kept
#     as a running maximum by the pure helper `xhci_service_gap_ms`, so a
#     report that waited in a full ring is a number rather than a silence;
#   * both numbers are published: `kbd_dropped=` and `kbd_gap_ms_max=` on the
#     boot record's `[PRODUCTION-LOOP]` heartbeat and `[KBD ]` finding, and
#     `DROPPED=` / `GAP_MS_MAX=` on the console's own `kbd` command;
#   * the host proof drives both helpers against their edges (full queue,
#     capacity zero, no earlier report, a clock that went backwards, an unknown
#     tick rate, a longer and a shorter gap);
#   * the QEMU proof queues a run of long reports and types 308 characters
#     through the same USB keyboard in three lines, requires every line to be
#     echoed whole and in order, and then requires `DROPPED=0` and
#     `GAP_MS_MAX` inside the budget.
for keyboard_loss_file in \
    kernel/include/xhci_trb.h \
    kernel/src/xhci_trb.c \
    kernel/include/xhci.h \
    kernel/src/xhci.c \
    kernel/src/kernel.c \
    kernel/src/console.c \
    tests/xhci-host.c \
    tests/console-host.c \
    scripts/test-qemu-production-storage-boot.sh \
    docs/hfoc-appliance-boot-console.md; do
    [[ -f $keyboard_loss_file ]] || {
        echo "ITEM-458: отсутствует $keyboard_loss_file" >&2
        exit 1
    }
done

# The two pure decisions exist and are the ones the driver calls.
grep -Fq 'bool xhci_local_queue_push(uint32_t head, uint32_t tail, uint32_t capacity,' \
    kernel/include/xhci_trb.h || {
    echo "ITEM-458: решение очереди не объявлено чистым" >&2
    exit 1
}
grep -Fq 'uint32_t xhci_service_gap_ms(uint32_t maximum_ms, uint64_t last_ticks,' \
    kernel/include/xhci_trb.h || {
    echo "ITEM-458: измерение промежутка не объявлено чистым" >&2
    exit 1
}
grep -Fq 'if (!xhci_local_queue_push(device->event_head, device->event_tail,' \
    kernel/src/xhci.c || {
    echo "ITEM-458: драйвер не спрашивает чистое решение очереди" >&2
    exit 1
}
grep -Fq 'device->service_gap_ms_max = xhci_service_gap_ms(' \
    kernel/src/xhci.c || {
    echo "ITEM-458: драйвер не измеряет промежуток чистым помощником" >&2
    exit 1
}

# The refusal is counted, not silent.
grep -Fq '++device->dropped_report_count;' kernel/src/xhci.c || {
    echo "ITEM-458: отказ очереди не считается" >&2
    exit 1
}
if grep -Fq '/* The console is not draining; dropping the newest event keeps the' \
    kernel/src/xhci.c && ! grep -Fq 'refusal is counted' kernel/src/xhci.c; then
    echo "ITEM-458: отказ очереди остался молчаливым" >&2
    exit 1
fi

# The pass drains the ring under both budgets.
grep -Fq '#define XHCI_SERVICE_EVENTS_MAX 64U' kernel/src/xhci.c || {
    echo "ITEM-458: у прохода нет бюджета по событиям кольца" >&2
    exit 1
}
grep -Fq 'while (events < XHCI_SERVICE_EVENTS_MAX &&' kernel/src/xhci.c || {
    echo "ITEM-458: проход не вычитывает кольцо" >&2
    exit 1
}
grep -Fq 'reports < XHCI_SERVICE_REPORTS_MAX) {' kernel/src/xhci.c || {
    echo "ITEM-458: у прохода нет бюджета по отчётам" >&2
    exit 1
}
if grep -Fq 'One whole report per session per pass' kernel/src/xhci.c; then
    echo "ITEM-458: проход всё ещё обрывается на первом отчёте" >&2
    exit 1
fi

# Both numbers have accessors and both are published where a reader looks.
grep -Fq 'uint32_t xhci_keyboard_dropped_count(void);' kernel/include/xhci.h || {
    echo "ITEM-458: нет доступа к счётчику отказов" >&2
    exit 1
}
grep -Fq 'uint32_t xhci_keyboard_gap_ms_max(void);' kernel/include/xhci.h || {
    echo "ITEM-458: нет доступа к максимальному промежутку" >&2
    exit 1
}
grep -Fq 'NET_RECORD_WRITE(" kbd_dropped=");' kernel/src/kernel.c || {
    echo "ITEM-458: heartbeat не публикует отказы очереди" >&2
    exit 1
}
grep -Fq 'NET_RECORD_WRITE(" kbd_gap_ms_max=");' kernel/src/kernel.c || {
    echo "ITEM-458: heartbeat не публикует максимальный промежуток" >&2
    exit 1
}
grep -Fq 'console_write(" DROPPED=");' kernel/src/console.c || {
    echo "ITEM-458: команда kbd не публикует отказы очереди" >&2
    exit 1
}
grep -Fq 'console_write(" GAP_MS_MAX=");' kernel/src/console.c || {
    echo "ITEM-458: команда kbd не публикует максимальный промежуток" >&2
    exit 1
}

# The host proof drives both helpers, including the edges.
for keyboard_loss_proof in \
    '"a queue whose next head would reach the tail is full"' \
    '"a refused push writes no next head"' \
    '"a queue of no slots refuses everything"' \
    '"the first report of a boot has no gap to measure"' \
    '"one tick is ten milliseconds"' \
    '"a longer gap raises it"' \
    '"a clock that went backwards leaves the maximum alone"' \
    '"an unknown tick rate leaves the maximum alone"'; do
    grep -Fq -- "$keyboard_loss_proof" tests/xhci-host.c || {
        echo "ITEM-458: host-проба не требует:" "$keyboard_loss_proof" >&2
        exit 1
    }
done
grep -Fq '" KEYS=5 DROPPED=0 GAP_MS_MAX=30\n"' tests/console-host.c || {
    echo "ITEM-458: host-проба консоли не требует обеих величин" >&2
    exit 1
}
grep -Fq 'test_report_queue_and_service_gap();' tests/xhci-host.c || {
    echo "ITEM-458: host-проба не запускается" >&2
    exit 1
}

# The QEMU proof types the stream through the USB keyboard and requires the
# echo and the counters.
# ITEM-459 extended the exact-echo signer with an optional key-name map for
# punctuation; the per-character echo it exists for is unchanged, so the pin
# moves with the signature rather than disappearing.
# shellcheck disable=SC2016  # the proof's own text is what the search matches
for keyboard_loss_qemu in \
    'local loss_phase=$4' \
    '"abcdefghijklmnopqrstuvwxyz" * 4,' \
    '"0123456789" * 10,' \
    '"qwertyuiopasdfghjklzxcvbnm" * 4,' \
    'def type_line_exact(line, offset, key_names=None):' \
    '("sendkey " + key + " 1\n")' \
    '("hfoc> " + prefix) in text' \
    'the USB keyboard was never echoed' \
    'HFOC_PRODUCTION_STORAGE_BOOT_TIMEOUT:-300}s' \
    "grep -Eq 'KEYS=[0-9]+ DROPPED=0 GAP_MS_MAX=[0-9]+'" \
    'usb_gap_ms_max > 500' \
    'usb_reports_printed < 6'; do
    grep -Fq -- "$keyboard_loss_qemu" \
        scripts/test-qemu-production-storage-boot.sh || {
        echo "ITEM-458: QEMU-проба не требует:" "$keyboard_loss_qemu" >&2
        exit 1
    }
done

# The item is recorded where a maintainer looks.
for keyboard_loss_doc in \
    'ITEM-458' \
    'kbd_gap_ms_max'; do
    grep -Fq -- "$keyboard_loss_doc" docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-458:" \
            "$keyboard_loss_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-458' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-458" >&2
    exit 1
}
grep -Fq 'ITEM-458' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-458" >&2
    exit 1
}
