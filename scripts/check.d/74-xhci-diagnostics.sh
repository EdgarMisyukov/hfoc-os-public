#!/usr/bin/env bash
# ITEM-468: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The machine tells its own story about xHCI and input (СРОЧНО-18, round 7).
#
# The owner's 2026-09-21 hardware run left the input record written exactly once
# (tick 10) because it is published "on change", and the controller's registers
# only when it was bound.  The pointer stayed `mouse_reason=INTERRUPT_PENDING`
# with `mouse_reports=0` and nothing on the machine said whether the controller
# was still running, whether its event ring moved, whether the interrupt
# transfer was armed with ISP, or how long the loop took between two passes.
#
# This item is measurement, not a fix.  This slice pins:
#
#   * the once-a-second diagnostic heartbeat the loop calls
#     (`xhci_service_diagnostics()`), and that it publishes through the
#     journal-only path ITEM-461 exposes - not the painting record path;
#   * the three bounded record shapes and every field the owner asked for:
#     the controller registers, the per-role binding/traffic state and the loop
#     window;
#   * the `XHCI` and `INPUT` console commands that print the same facts on
#     demand, and the help line that names them;
#   * that the line bound is the console's own 320 byte journal bound, so a
#     field cannot be cut off the journal (ITEM-481 raised the local buffer to
#     the worst case and made the console funnel split a longer line into
#     `-TAIL` parts; the overflow flag makes a buffer that ever fills loud);
#   * that the item is recorded where a maintainer looks.
for xhci_diagnostics_file in \
    kernel/include/xhci.h \
    kernel/src/xhci.c \
    kernel/src/kernel.c \
    kernel/src/console.c \
    docs/hfoc-appliance-boot-console.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $xhci_diagnostics_file ]] || {
        echo "ITEM-468: отсутствует $xhci_diagnostics_file" >&2
        exit 1
    }
done

# 1. The periodic heartbeat exists, is declared and is called by the loop.
grep -Fq 'void xhci_service_diagnostics(' kernel/include/xhci.h || {
    echo "ITEM-468: диагностический heartbeat не объявлен" >&2
    exit 1
}
grep -Fq 'void xhci_diagnostics_print_xhci(void);' kernel/include/xhci.h || {
    echo "ITEM-468: команда XHCI не объявлена" >&2
    exit 1
}
grep -Fq 'void xhci_diagnostics_print_input(void);' kernel/include/xhci.h || {
    echo "ITEM-468: команда INPUT не объявлена" >&2
    exit 1
}
grep -Fq 'xhci_service_diagnostics(' kernel/src/kernel.c || {
    echo "ITEM-468: цикл не публикует диагностику" >&2
    exit 1
}

# 2. The records are journal-only (ITEM-461's path), so the heartbeat does not
#    repaint the screen every second, and the line bound is the console's own.
grep -Fq 'console_publish_journal(line->text, line->length);' kernel/src/xhci.c || {
    echo "ITEM-468: диагностика не идёт в durable-журнал" >&2
    exit 1
}
# ITEM-481: the local buffer now holds the worst case of the longest shape and
# the console funnel (`console_publish_journal`) bounds the published record at
# 320, splitting a longer one into `[<name>-TAIL]` parts.  The buffer must still
# be bounded, and an overflow must be loud rather than a silent drop.
grep -Fq '#define XHCI_DIAG_LINE_MAX 512U' kernel/src/xhci.c || {
    echo "ITEM-468: запись не ограничена бюджетом строки журнала" >&2
    exit 1
}
grep -Fq 'line->overflow = true;' kernel/src/xhci.c || {
    echo "ITEM-468: переполнение записи не отмечается" >&2
    exit 1
}

# 3. The controller record carries every register the owner asked for.
for xhci_diagnostics_controller_field in \
    '"[XHCI-DIAG] pci="' \
    '" usbcmd="' \
    '" usbsts="' \
    '" crcr="' \
    '" erdp="' \
    '" erstba="' \
    '" interrupter_enabled="' \
    '" running="' \
    '" halted="' \
    '" error="'; do
    grep -Fq -- "$xhci_diagnostics_controller_field" kernel/src/xhci.c || {
        echo "ITEM-468: запись контроллера не несёт" \
            "$xhci_diagnostics_controller_field" >&2
        exit 1
    }
done

# 4. The per-role record carries the binding, the traffic and the ISP/short
#    evidence that separates "never armed" from "armed and silent".
for xhci_diagnostics_role_field in \
    '"[INPUT-DIAG] role="' \
    '" slot="' \
    '" ep=0x"' \
    '" dci="' \
    '" state="' \
    '" pending="' \
    '" reposted="' \
    '" submitted="' \
    '" completions="' \
    '" reports="' \
    '" errors="' \
    '" isp="' \
    '" short="' \
    '" last_completion_ms="' \
    '" reason="'; do
    grep -Fq -- "$xhci_diagnostics_role_field" kernel/src/xhci.c || {
        echo "ITEM-468: запись роли не несёт" \
            "$xhci_diagnostics_role_field" >&2
        exit 1
    }
done

# 5. The loop-window record carries the same-window cost and frames.
for xhci_diagnostics_pass_field in \
    '"[LOOP-DIAG] events="' \
    '" reports="' \
    '" passes="' \
    '" iter_us="' \
    '" iter_max_us="' \
    '" frames="'; do
    grep -Fq -- "$xhci_diagnostics_pass_field" kernel/src/xhci.c || {
        echo "ITEM-468: запись прохода не несёт" \
            "$xhci_diagnostics_pass_field" >&2
        exit 1
    }
done

# 6. The short-packet completion is counted, so "completed" and "a whole
#    report" are different numbers.
grep -Fq 'device->short_packet_count' kernel/src/xhci.c || {
    echo "ITEM-468: короткий пакет не считается" >&2
    exit 1
}
grep -Fq 'device->last_transfer_isp = true;' kernel/src/xhci.c || {
    echo "ITEM-468: ISP вооружённой передачи не публикуется" >&2
    exit 1
}

# 7. The console commands read the same state on demand, and help names them.
grep -Fq 'console_command_is(input_line, "xhci")' kernel/src/console.c || {
    echo "ITEM-468: команда XHCI не подключена" >&2
    exit 1
}
grep -Fq 'console_command_is(input_line, "input")' kernel/src/console.c || {
    echo "ITEM-468: команда INPUT не подключена" >&2
    exit 1
}
grep -Fq 'XHCI INPUT' kernel/src/console.c || {
    echo "ITEM-468: справка не называет XHCI и INPUT" >&2
    exit 1
}

# 8. The item is recorded where a maintainer looks.
for xhci_diagnostics_doc in \
    'ITEM-468' \
    '[XHCI-DIAG]' \
    '[INPUT-DIAG]' \
    '[LOOP-DIAG]'; do
    grep -Fq -- "$xhci_diagnostics_doc" \
        docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-468:" \
            "$xhci_diagnostics_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-468' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-468" >&2
    exit 1
}
grep -Fq 'ITEM-468' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-468" >&2
    exit 1
}
