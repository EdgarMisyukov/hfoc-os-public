#!/usr/bin/env bash
# MOUSE-DIAG (owner task 2026-09-27): contracted slice of the HFOC OS check
#           suite.  Sourced in pinned order by scripts/check.sh; lint it on its
#           own with shellcheck.

# Distinguishing a silent USB mouse from a stalled xHCI event path.
#
# Two hardware runs (`hw11`, `hw12`) ended the same way: the pointer worked,
# then stopped for the rest of the run while the journal showed
# `mouse_armed=16`, the endpoint context Running, every completion code SUCCESS,
# `mse=0`, `no_trb=0` and `ringfull=0`.  The owner's diagnostic contract asks
# for the one observation that separates the two candidate causes - the USB
# device stopped sending reports, or the controller stopped delivering events -
# and for it to survive the journal's own drop behaviour.
#
# This slice pins the diagnostic-only implementation and, just as important,
# that nothing about the production behaviour it observes was changed:
#
#   * one bounded snapshot per silence episode, published at the watchdog's
#     first-detection point and never per loop turn, carrying the endpoint's
#     counters, the controller's command/status/ring/interrupter registers and
#     the PORTSC of the port the device sits on (or every port value the
#     controller read, with `identified=0`, when the port cannot be named);
#   * the same controller and port read again after the single re-arm, so
#     "the dequeue pointer did not move" is a number;
#   * `[ITER-MAX]`: a new boot maximum past the long-turn budget is published
#     once with its phase and its clock, so `iter_max_us` stops being a bare
#     number;
#   * the keyboard echo's split (fast path against whole-line redraw, the
#     characters a redraw repaints, its cost, and the ITEM-483 generation
#     invalidations) reaches the journal;
#   * the snapshot, the watchdog record and the iteration maximum are journal
#     flush markers, so the evidence the run exists for is on the disk even
#     though the pending ring holds 64 records at ~20-24 records per second;
#   * the watchdog's recovery is unchanged: re-arm, barrier, doorbell - no Stop
#     Endpoint, no Set TR Dequeue Pointer, no controller reset;
#   * the keyboard fast path's own condition is unchanged.

for moused_file in \
    kernel/include/xhci_trb.h \
    kernel/src/xhci_trb.c \
    kernel/src/xhci.c \
    kernel/src/kernel.c \
    kernel/src/console.c \
    kernel/include/console.h \
    kernel/include/loop_budget.h \
    kernel/src/loop_budget.c \
    kernel/src/appliance_boot_log.c \
    tests/xhci-host.c \
    tests/loop-budget-host.c \
    docs/hfoc-appliance-boot-console.md; do
    [[ -f $moused_file ]] || {
        echo "MOUSE-DIAG: отсутствует $moused_file" >&2
        exit 1
    }
done

# ------------------------------------------------------------ the pure records --
for moused_api in \
    'size_t xhci_format_silence_record(' \
    'size_t xhci_format_silence_controller_record(' \
    'size_t xhci_format_silence_port_record(' \
    'size_t xhci_format_silence_after_record(' \
    'size_t xhci_format_iter_max_record(' \
    'struct xhci_silence_controller {' \
    'struct xhci_silence_endpoint {'; do
    grep -Fq "$moused_api" kernel/include/xhci_trb.h || {
        echo "MOUSE-DIAG: в xhci_trb.h нет «$moused_api»" >&2
        exit 1
    }
done

for moused_field in \
    '[MOUSE-SILENCE] ticks=' \
    ' since_completion_ms=' \
    ' since_report_ms=' \
    ' last_completion_ticks=' \
    '[MOUSE-SILENCE-XHCI] pci=' \
    ' erdp=' \
    ' erstba=' \
    ' iman=' \
    ' imod=' \
    ' host_events=' \
    ' ringfull=' \
    '[MOUSE-SILENCE-PORT] pci=' \
    ' identified=' \
    ' portsc=' \
    ' ccs=' \
    ' ped=' \
    ' pr=' \
    ' pls=' \
    ' pp=' \
    ' oca=' \
    ' csc=' \
    ' pec=' \
    ' wrc=' \
    ' prc=' \
    ' plc=' \
    ' cec=' \
    ' wake=' \
    '[MOUSE-SILENCE-AFTER] ticks=' \
    ' erdp_before=' \
    ' erdp_after=' \
    ' erdp_moved=' \
    '[ITER-MAX] ticks=' \
    ' iter_max_us=' \
    ' phase='; do
    grep -Fq "$moused_field" kernel/src/xhci_trb.c || {
        echo "MOUSE-DIAG: форматтер не публикует «$moused_field»" >&2
        exit 1
    }
done

# The record bound: a diagnostic record longer than the console's line budget is
# cut, and a cut record is worse than none.  Both the budget and the host proof
# that pins it stay in the tree.
grep -Fq '#define XHCI_SILENCE_RECORD_BYTES 320U' kernel/include/xhci_trb.h || {
    echo "MOUSE-DIAG: снимок стойла не ограничен бюджетом записи" >&2
    exit 1
}
grep -Fq 'test_silence_records();' tests/xhci-host.c || {
    echo "MOUSE-DIAG: host-проба снимка стойла не подключена" >&2
    exit 1
}
grep -Fq "the widest silence record fits the journal's line budget" \
    tests/xhci-host.c || {
    echo "MOUSE-DIAG: host-проба не проверяет ширину записей" >&2
    exit 1
}
grep -Fq 'an unnamed port publishes every port value it read instead' \
    tests/xhci-host.c || {
    echo "MOUSE-DIAG: host-проба не проверяет запасной путь без имени порта" >&2
    exit 1
}

# ---------------------------------------------------- publication at the stall --
grep -Fq 'silence_erdp = xhci_silence_publish(device, silence_ms);' \
    kernel/src/xhci.c || {
    echo "MOUSE-DIAG: снимок не публикуется в точке первого обнаружения" >&2
    exit 1
}
grep -Fq 'xhci_silence_publish_after(device, silence_ms, silence_erdp);' \
    kernel/src/xhci.c || {
    echo "MOUSE-DIAG: нет записи о состоянии после восстановления" >&2
    exit 1
}
grep -Fq 'if (device->role == XHCI_HID_ROLE_MOUSE) {' kernel/src/xhci.c || {
    echo "MOUSE-DIAG: снимок не ограничен ролью мыши" >&2
    exit 1
}
# Read-only: the snapshot must not write a single controller register.  The
# readers go through the operational reads and the doorbell is not among them.
python3 - "$PWD/kernel/src/xhci.c" <<'PY' || {
import re
import sys

text = open(sys.argv[1], encoding='utf-8').read()
start = text.index('static void xhci_silence_read_controller(')
end = text.index('static void xhci_watchdog_device(')
block = text[start:end]
write = re.search(r'xhci_write32\(|xhci_write64\(|doorbell', block)
sys.exit(1 if write else 0)
PY
    echo "MOUSE-DIAG: снимок стойла пишет в контроллер" >&2
    exit 1
}
# The watchdog's recovery is the same re-arm and kick it always was.
python3 - "$PWD/kernel/src/xhci.c" <<'PY' || {
import sys

text = open(sys.argv[1], encoding='utf-8').read()
start = text.index('static void xhci_endpoint_recover(')
end = text.index('static uint64_t xhci_silence_publish(')
block = text[start:end]
ok = ('xhci_top_up_device(device);' in block and
      'xhci_write32(xhci_doorbell(device->slot_id), 0U, device->dci);' in block)
banned = ('XHCI_TRB_STOP_ENDPOINT' in block or
          'SET_TR_DEQUEUE' in block or
          'xhci_reset' in block)
sys.exit(0 if ok and not banned else 1)
PY
    echo "MOUSE-DIAG: поведение watchdog/re-arm изменилось" >&2
    exit 1
}

# ----------------------------------------------------------- iteration maximum --
grep -Fq 'bool loop_budget_iter_max_should_publish(uint64_t period_us,' \
    kernel/include/loop_budget.h || {
    echo "MOUSE-DIAG: нет чистого правила публикации максимума оборота" >&2
    exit 1
}
grep -Fq 'return loop_budget_is_long_turn(period_us) && period_us > previous_max_us;' \
    kernel/src/loop_budget.c || {
    echo "MOUSE-DIAG: правило публикации максимума не то, что заявлено" >&2
    exit 1
}
grep -Fq 'test_iter_max_publish_policy();' tests/loop-budget-host.c || {
    echo "MOUSE-DIAG: host-проба правила публикации не подключена" >&2
    exit 1
}
grep -Fq 'if (loop_budget_iter_max_should_publish(micros, previous_max)) {' \
    kernel/src/kernel.c || {
    echo "MOUSE-DIAG: максимум оборота не публикуется по правилу" >&2
    exit 1
}
grep -Fq 'production_iteration_max_phase = loop_budget_phase_name(' \
    kernel/src/kernel.c || {
    echo "MOUSE-DIAG: фаза худшего оборота не запоминается" >&2
    exit 1
}

# --------------------------------------------------------------- keyboard echo --
for moused_console_api in \
    'uint64_t console_input_echo_fast_count(void);' \
    'uint64_t console_input_echo_redraw_count(void);' \
    'uint64_t console_input_echo_redraw_chars(void);' \
    'uint64_t console_input_echo_redraw_us_total(void);' \
    'uint64_t console_input_echo_redraw_us_max(void);' \
    'uint64_t console_input_echo_generation_invalidations(void);'; do
    grep -Fq "$moused_console_api" kernel/include/console.h || {
        echo "MOUSE-DIAG: в console.h нет «$moused_console_api»" >&2
        exit 1
    }
done
grep -Fq '++console_kbd_echo_fast_count;' kernel/src/console.c || {
    echo "MOUSE-DIAG: fast path не считается" >&2
    exit 1
}
grep -Fq '++console_kbd_echo_generation_invalidations;' kernel/src/console.c || {
    echo "MOUSE-DIAG: инвалидации поколения не считаются" >&2
    exit 1
}
grep -Fq '++console_kbd_echo_redraw_count;' kernel/src/console.c || {
    echo "MOUSE-DIAG: полные перерисовки не считаются" >&2
    exit 1
}
grep -Fq '[KBD-ECHO] fast=' kernel/src/kernel.c || {
    echo "MOUSE-DIAG: разбивка эха клавиатуры не публикуется" >&2
    exit 1
}
# The production fast path still decides exactly as ITEM-482/483 left it.
if ! grep -Fq 'if (console_input_line_generation ==' kernel/src/console.c ||
    ! grep -Fq 'previous_rendered_length + 1U == input_length) {' \
        kernel/src/console.c; then
    echo "MOUSE-DIAG: условие fast path изменилось" >&2
    exit 1
fi

# ------------------------------------------------------------ surviving the ring --
for moused_marker in '"[MOUSE-SILENCE"' '"[INPUT-WATCHDOG]"' '"[ITER-MAX]"'; do
    grep -Fq "$moused_marker" kernel/src/appliance_boot_log.c || {
        echo "MOUSE-DIAG: «$moused_marker» не является flush-marker" >&2
        exit 1
    }
done
grep -Fq '"[PRODUCTION-LOOP]",' kernel/src/appliance_boot_log.c || {
    echo "MOUSE-DIAG: производственные маркеры журнала не тронуты" >&2
    exit 1
}

# ------------------------------------------- MOUSE-DIAG step 2: the EP0 probe --
# `hw13` left one question open: with the port connected and enabled and the
# controller running, did the device stop answering at all, or did its
# interrupt pipe alone go quiet?  One read-only GET_STATUS on the same slot's
# EP0 answers it - and this slice pins both the record set and the fact that
# the probe touches nothing it is forbidden to touch.
for moused_ep0_api in \
    'size_t xhci_format_ep0_probe_record(' \
    'size_t xhci_format_ep0_state_record(' \
    'uint32_t xhci_slot_state(' \
    'const char *xhci_slot_state_string(uint32_t state);' \
    '#define XHCI_EP0_NOT_SUBMITTED' \
    '#define XHCI_EP0_NO_COMPLETION'; do
    grep -Fq "$moused_ep0_api" kernel/include/xhci_trb.h || {
        echo "MOUSE-DIAG: в xhci_trb.h нет «$moused_ep0_api»" >&2
        exit 1
    }
done
for moused_ep0_field in \
    '[MOUSE-EP0] ticks=' \
    ' submitted=' \
    ' code=' \
    ' elapsed_us=' \
    ' ep_before=' \
    ' ep_after=' \
    ' slot_before=' \
    ' slot_after=' \
    '[MOUSE-EP0-STATE] portsc_before=' \
    ' portsc_after=' \
    ' usbsts_before=' \
    ' usbsts_after=' \
    ' erdp_before=' \
    ' erdp_after=' \
    ' erdp_moved='; do
    grep -Fq "$moused_ep0_field" kernel/src/xhci_trb.c || {
        echo "MOUSE-DIAG: EP0-проба не публикует «$moused_ep0_field»" >&2
        exit 1
    }
done
grep -Fq 'test_ep0_probe_records();' tests/xhci-host.c || {
    echo "MOUSE-DIAG: host-проба EP0 не подключена" >&2
    exit 1
}
grep -Fq 'test_slot_state();' tests/xhci-host.c || {
    echo "MOUSE-DIAG: host-проба состояния слота не подключена" >&2
    exit 1
}
grep -Fq '"[MOUSE-EP0"' kernel/src/appliance_boot_log.c || {
    echo "MOUSE-DIAG: результат EP0-пробы не является flush-marker" >&2
    exit 1
}
# The probe is issued from the silence snapshot's own first-detection point, so
# one episode costs at most one control transfer.
grep -Fq 'xhci_ep0_probe_publish(device);' kernel/src/xhci.c || {
    echo "MOUSE-DIAG: EP0-проба не вызывается в точке стойла" >&2
    exit 1
}
python3 - "$PWD/kernel/src/xhci.c" <<'PY' || {
import re
import sys

text = open(sys.argv[1], encoding='utf-8').read()
start = text.index('static void xhci_ep0_probe_publish(')
end = text.index('static uint64_t xhci_silence_publish(')
block = text[start:end]
# exactly one control transfer, and none of the forbidden state changes
if block.count('xhci_control_transfer(') != 1:
    sys.exit(1)
banned = ('XHCI_TRB_STOP_ENDPOINT', 'SET_TR_DEQUEUE', 'xhci_reset',
          'xhci_port_reset', 'xhci_disable_slot', 'xhci_address_device',
          'xhci_configure_endpoint', 'xhci_endpoint_recover')
sys.exit(1 if any(name in block for name in banned) else 0)
PY
    echo "MOUSE-DIAG: EP0-проба делает больше одного transfer или меняет состояние" >&2
    exit 1
}

# --------------------------------------------------- the two telemetry fixes --
# (A) the silence snapshot's completion clock must be the one a re-arm does not
#     clear: `hw13` read `since_completion_ms=0 last_completion_ticks=0` while
#     the device had answered 13471 times.
grep -Fq 'xhci_service_gap_ms(0U, device->last_report_ticks,' kernel/src/xhci.c || {
    echo "MOUSE-DIAG: снимок стойла снова берёт обнуляемый transfer_complete_ticks" >&2
    exit 1
}
grep -Fq 'endpoint.last_completion_ticks = device->last_report_ticks;' \
    kernel/src/xhci.c || {
    echo "MOUSE-DIAG: last_completion_ticks снова обнуляемый" >&2
    exit 1
}
# (B) the worst-turn maximum must survive the journal's ring: its phase and its
#     clock ride the cumulative summary, not only the once-per-maximum record.
for moused_iter_field in ' iter_max_phase=' ' iter_max_ticks=' ' iter_max_second='; do
    grep -Fq "$moused_iter_field" kernel/src/kernel.c || {
        echo "MOUSE-DIAG: [LOOP-SUMMARY] не публикует «$moused_iter_field»" >&2
        exit 1
    }
done
grep -Fq 'production_iteration_max_ticks = timer_ticks();' kernel/src/kernel.c || {
    echo "MOUSE-DIAG: часы максимума оборота не запоминаются" >&2
    exit 1
}
