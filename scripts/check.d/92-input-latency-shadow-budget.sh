#!/usr/bin/env bash
# ITEM-486: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The blocking local-input delay: the shadow-only framebuffer, the batched
# flush, the bounded network pass and the named mouse event path.
#
# The owner's 2026-09-26 contract (`/srv/hfoc-example/queue/current.md`, round 24)
# names the architectural causes: a glyph was a burst of volatile
# write-combining stores, the network phase could hold the loop for hundreds of
# milliseconds, and a pointer that stopped completing could not say where the
# event was lost.  This slice pins the fixes and their evidence:
#
#   * the RAM shadow is the primary surface: `pixel()` writes it and marks a
#     dirty rectangle, and the existing `framebuffer_sync()` barrier performs
#     the one batched `framebuffer_flush()`, so every old call site keeps its
#     meaning;
#   * the panic and installer-notice screens flush the shadow they draw;
#   * the receive pass stops at a named microsecond budget as well as the frame
#     count and returns to the loop; the frames it did not take stay in the
#     ring;
#   * the mouse event path classifies the window as NO_COMPLETION, NO_REPORT,
#     NO_LOCAL_INPUT, QUEUE_BACKLOG, WATCHDOG_REARM, WATCHDOG_GAVE_UP,
#     POINTER_PROCESSING or FRAMEBUFFER_RENDER, and the framebuffer's own
#     `fb_shadow_draw_us`/`fb_dirty_flush_us`/`fb_dirty_bytes`/
#     `fb_dirty_rects`/`fb_flush_count` and the network's `net_budget_us`/
#     `net_budget_hits` reach the loop record.

for item486b_file in \
    kernel/include/framebuffer.h \
    kernel/src/framebuffer.c \
    kernel/src/panic.c \
    kernel/src/installer_notice.c \
    kernel/include/appliance_production_network_boot.h \
    kernel/src/appliance_production_network_x86.c \
    kernel/include/input_path.h \
    kernel/src/input_path.c \
    kernel/src/xhci.c \
    kernel/src/kernel.c \
    tests/framebuffer-scroll-host.c \
    tests/input-path-host.c \
    scripts/test-qemu-production-storage-boot.sh \
    docs/hfoc-appliance-boot-console.md \
    docs/ROADMAP.md; do
    [[ -f $item486b_file ]] || {
        echo "ITEM-486: отсутствует $item486b_file" >&2
        exit 1
    }
done

# ------------------------------------------------- the shadow-only framebuffer --
for item486b_fb_api in \
    'void framebuffer_flush(void);' \
    'void framebuffer_set_clock(framebuffer_clock_fn clock);' \
    'uint64_t framebuffer_shadow_draw_us(void);' \
    'uint64_t framebuffer_dirty_flush_us(void);' \
    'uint64_t framebuffer_dirty_bytes(void);' \
    'uint64_t framebuffer_dirty_rects(void);' \
    'uint64_t framebuffer_flush_count(void);'; do
    grep -Fq "$item486b_fb_api" kernel/include/framebuffer.h || {
        echo "ITEM-486: framebuffer.h не объявляет $item486b_fb_api" >&2
        exit 1
    }
done
grep -Fq 'dirty_mark(x, y);' kernel/src/framebuffer.c || {
    echo "ITEM-486: pixel() не отмечает dirty-прямоугольник" >&2
    exit 1
}
grep -Fq 'framebuffer_flush();' kernel/src/framebuffer.c || {
    echo "ITEM-486: framebuffer.c не выполняет flush" >&2
    exit 1
}
grep -Fq 'framebuffer_sync();' kernel/src/panic.c || {
    echo "ITEM-486: паника не сбрасывает shadow на устройство" >&2
    exit 1
}
grep -Fq 'framebuffer_sync();' kernel/src/installer_notice.c || {
    echo "ITEM-486: экран installer notice не сбрасывает shadow" >&2
    exit 1
}
grep -Fq 'test_shadow_paint_waits_for_flush' \
    tests/framebuffer-scroll-host.c || {
    echo "ITEM-486: host-проба не проверяет отложенный flush" >&2
    exit 1
}
for item486b_fb_field in \
    '" fb_shadow_draw_us="' '" fb_dirty_flush_us="' \
    '" fb_dirty_bytes="' '" fb_dirty_rects="' '" fb_flush_count="'; do
    grep -Fq "$item486b_fb_field" kernel/src/kernel.c || {
        echo "ITEM-486: [LOOP-PHASES] не публикует $item486b_fb_field" >&2
        exit 1
    }
done

# ---------------------------------------------------- the bounded network pass --
grep -Fq '#define APN_SERVICE_BUDGET_US 2000U' \
    kernel/src/appliance_production_network_x86.c || {
    echo "ITEM-486: сетевой бюджет не назван" >&2
    exit 1
}
grep -Fq 'apn_service_budget_spent()' \
    kernel/src/appliance_production_network_x86.c || {
    echo "ITEM-486: приёмный проход не проверяет бюджет" >&2
    exit 1
}
grep -Fq 'apn.service_budget_hits' \
    kernel/src/appliance_production_network_x86.c || {
    echo "ITEM-486: срабатывания бюджета не считаются" >&2
    exit 1
}
# The budget covers the *receive* pass, not the whole poll that contains it.
# The first version opened the deadline at the top of
# `appliance_production_network_boot_poll()` - ahead of the transmit path and
# the link work - so in a slow (TCG) build the budget was already spent when
# the first frame was offered and the receive path took no frame at all; the
# DHCP/DNS proof caught it as "the receive path accepted and refused no frame
# at all on a machine that answered DHCP".  Two things keep that fixed: the
# deadline is opened where the ring is drained, once per receive path, and the
# check starts at the second frame, so a pass that finds work always takes one.
grep -Fq 'static void apn_service_budget_open(void)' \
    kernel/src/appliance_production_network_x86.c || {
    echo "ITEM-486: бюджет не открывается отдельной функцией" >&2
    exit 1
}
# ITEM-494: the Intel backend is the third receive path, and it opens its own
# budget and guarantees the same minimum progress, so all three paths are named
# and the counts are three.
for item486b_rx_path in apn_rtl_poll_rx apn_poll_rx apn_e1000_poll_rx; do
    if ! sed -n "/^static void $item486b_rx_path(/,/^}/p" \
            kernel/src/appliance_production_network_x86.c | \
            grep -Fq 'apn_service_budget_open();'; then
        echo "ITEM-486: $item486b_rx_path не открывает свой бюджет" >&2
        exit 1
    fi
done
if [[ $(grep -c 'apn_service_budget_open();' \
        kernel/src/appliance_production_network_x86.c) -ne 3 ]]; then
    echo "ITEM-486/494: бюджет открывается не ровно в трёх приёмных путях" >&2
    exit 1
fi
if [[ $(grep -c 'budget >= APN_SERVICE_MIN_FRAMES && apn_service_budget_spent()' \
        kernel/src/appliance_production_network_x86.c) -ne 3 ]]; then
    echo "ITEM-486/494: проход не гарантирует минимальный прогресс во всех трёх путях" >&2
    exit 1
fi
# A bound expressed only in measured microseconds is not the same amount of work
# in an emulated build as on the part: the same frame costs tens of times more
# microseconds there, so a pass of "2000 us" ran at one frame per turn and the
# DHCP/DNS proof caught it a second time - its boot-time DNS probe exhausted its
# attempt budget, `reason=DNS_PROBE_TIMEOUT` took `hold=1`, and the marker the
# renewal drill is written around became `reason=REBIND_TIMEOUT hold=2`.  The
# floor makes progress structural instead of a property of the host's speed.
grep -Fq '#define APN_SERVICE_MIN_FRAMES 4U' \
    kernel/src/appliance_production_network_x86.c || {
    echo "ITEM-486: минимальный прогресс приёмного прохода не назван" >&2
    exit 1
}
grep -Fq 'appliance_production_network_boot_service_budget_hits' \
    kernel/include/appliance_production_network_boot.h || {
    echo "ITEM-486: бюджет не объявлен в заголовке" >&2
    exit 1
}
for item486b_net_field in '" net_budget_us="' '" net_budget_hits="'; do
    grep -Fq "$item486b_net_field" kernel/src/kernel.c || {
        echo "ITEM-486: [LOOP-PHASES] не публикует $item486b_net_field" >&2
        exit 1
    }
done

# ------------------------------------------------------ the mouse event path --
for item486b_outcome in \
    INPUT_PATH_NO_COMPLETION INPUT_PATH_NO_REPORT INPUT_PATH_NO_LOCAL_INPUT \
    INPUT_PATH_QUEUE_BACKLOG INPUT_PATH_WATCHDOG_REARM \
    INPUT_PATH_WATCHDOG_GAVE_UP INPUT_PATH_POINTER_PROCESSING \
    INPUT_PATH_FRAMEBUFFER_RENDER; do
    grep -Fq "$item486b_outcome" kernel/include/input_path.h || {
        echo "ITEM-486: input_path.h не называет $item486b_outcome" >&2
        exit 1
    }
done
grep -Fq 'xhci_mouse_watchdog_gave_up' kernel/src/xhci.c || {
    echo "ITEM-486: сторож не различает WATCHDOG_GAVE_UP" >&2
    exit 1
}
grep -Fq '" mouse_path="' kernel/src/kernel.c || {
    echo "ITEM-486: input record не публикует mouse_path" >&2
    exit 1
}
grep -Fq '"mouse_path"' scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-486: QEMU-проба не читает mouse_path" >&2
    exit 1
}

# --------------------------------------- the clocks and the surviving state --
# The window record carries the loop's own counters beside the window they are
# read against: `loops`/`ticks`/`real_seconds` next to `turns`/`turn_us`/
# `worst_us`/`over50`, which is what makes the ~1.55x reading of `hw9`-`hw11`
# decidable from one record instead of two.
for item486b_clock_field in '" loops="' '" ticks="' '" real_seconds="'; do
    grep -Fq "$item486b_clock_field" kernel/src/kernel.c || {
        echo "ITEM-486: [LOOP-PHASES] не публикует $item486b_clock_field" >&2
        exit 1
    }
done
# The run summary: every field cumulative, so the newest summary describes the
# whole run even though the 511-record ring only holds ~75 seconds of it.
grep -Fq 'log_production_loop_summary' kernel/src/kernel.c || {
    echo "ITEM-486: итоговая запись о состоянии прогона не публикуется" >&2
    exit 1
}
grep -Fq '#define KERNEL_LOOP_SUMMARY_EVERY' kernel/src/kernel.c || {
    echo "ITEM-486: частота итоговой записи не названа" >&2
    exit 1
}
for item486b_summary_field in \
    '"[LOOP-SUMMARY] loops="' '" iter_max_us="' '" mouse_completed="' \
    '" mouse_silent_max_ms="' '" mouse_gave_up="' '" fb_shadow_draw_us="' \
    '" net_budget_hits="'; do
    grep -Fq "$item486b_summary_field" kernel/src/kernel.c || {
        echo "ITEM-486: [LOOP-SUMMARY] не публикует $item486b_summary_field" >&2
        exit 1
    }
done
grep -Fq 'LOOP-SUMMARY' scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-486: QEMU-проба не читает итоговую запись" >&2
    exit 1
}
grep -Fq 'test_watchdog_gave_up_wins' tests/input-path-host.c || {
    echo "ITEM-486: host-проба не проверяет WATCHDOG_GAVE_UP" >&2
    exit 1
}
grep -Fq 'test-input-path-host' Makefile || {
    echo "ITEM-486: канонический host-suite не содержит test-input-path-host" >&2
    exit 1
}

# ------------------------------------------------------------- the documents --
for item486b_doc_word in ITEM-486 fb_shadow_draw_us fb_dirty_flush_us \
    net_budget_us mouse_path LOOP-SUMMARY real_seconds; do
    grep -Fq -- "$item486b_doc_word" docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-486:" \
            "$item486b_doc_word" >&2
        exit 1
    }
done
