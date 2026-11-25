#!/usr/bin/env bash
# MOUSE-DIAG step 3 (owner task 2026-09-27): contracted slice of the HFOC OS
#           check suite.  Sourced in pinned order by scripts/check.sh; lint it on
#           its own with shellcheck.

# Telemetry only: the three records that close the unknowns `hw14` left.
#
# `hw14` proved that the multi-second `journal_us`/`xhci_us` spans are an
# accounting artifact - the same records show the ISR tick counter advancing
# exactly 100 ticks and the cumulative cycle clock advancing one second - but
# not what produces it; the endpoint's TR Dequeue Pointer and ring cycle were
# never published at all; and the journal flush published no split between
# publishing records and writing them to the ESP.
#
# This slice pins the diagnostic-only addition and, just as important, that
# nothing about the behaviour it observes changed:
#
#   * `[TIMING-CHECK]` + `[TIMING-CHECK-TAIL]`: raw start/end TSC, the elapsed
#     cycles, both clocks at both ends, `tick_hz`, `raw_elapsed_us`, and the
#     existing metrics beside a `timing_accounting_mismatch` flag computed by
#     `loop_budget_accounting_mismatch()`;
#   * `[EP-STATE]` + `[EP-STATE-TAIL]`: endpoint address, DCI, context state,
#     transfer ring base, TR Dequeue Pointer with its dequeue cycle, producer
#     enqueue position and cycle, plus the counters the driver already had;
#   * `[BOOTLOG-FLUSH]`: records, text bytes, slot bytes, the measured
#     ESP-write and commit spans, the total, and `publish=derived` beside
#     `unavailable=esp_blocks,clusters`, so a derived span is never read as a
#     measured one;
#   * the three records are periodic, never flush markers: a diagnostic record
#     must not trigger a flush of its own;
#   * the endpoint snapshot writes no register, rings no doorbell, re-arms
#     nothing and does not touch the endpoint context;
#   * the flush measurements wrap the existing calls without changing their
#     order, batching, frequency or commit semantics.

for hw15_file in \
    kernel/include/xhci_trb.h \
    kernel/src/xhci_trb.c \
    kernel/include/loop_budget.h \
    kernel/src/loop_budget.c \
    kernel/src/kernel.c \
    kernel/src/xhci.c \
    kernel/include/appliance_boot_log_store.h \
    kernel/src/appliance_boot_log_store.c \
    tests/xhci-host.c \
    tests/loop-budget-host.c; do
    [[ -f $hw15_file ]] || {
        echo "MOUSE-DIAG step 3: отсутствует $hw15_file" >&2
        exit 1
    }
done

# --------------------------------------------------------------- the records --
for hw15_api in \
    'size_t xhci_format_timing_check_record(' \
    'size_t xhci_format_timing_check_tail_record(' \
    'size_t xhci_format_ep_state_record(' \
    'size_t xhci_format_ep_state_tail_record(' \
    'size_t xhci_format_bootlog_flush_record(' \
    'bool xhci_endpoint_dequeue(' \
    'struct xhci_timing_check {' \
    'struct xhci_ep_state {' \
    'struct xhci_bootlog_flush {'; do
    grep -Fq "$hw15_api" kernel/include/xhci_trb.h || {
        echo "MOUSE-DIAG step 3: в xhci_trb.h нет «$hw15_api»" >&2
        exit 1
    }
done

for hw15_field in \
    '[TIMING-CHECK] start_tsc=' \
    ' end_tsc=' \
    ' elapsed_cycles=' \
    ' ticks_start=' \
    ' ticks_end=' \
    ' real_start=' \
    ' real_end=' \
    ' tick_hz=' \
    ' raw_elapsed_us=' \
    '[TIMING-CHECK-TAIL] sum_phase_us=' \
    ' work_us=' \
    ' turn_us=' \
    ' worst_us=' \
    ' iter_max_us=' \
    ' timing_accounting_mismatch=' \
    '[EP-STATE] role=' \
    ' ep=0x' \
    ' dci=' \
    ' state=' \
    ' armed=' \
    ' pending=' \
    ' reposted=' \
    ' code=' \
    ' last_ms=' \
    '[EP-STATE-TAIL] role=' \
    ' ring=' \
    ' dq=' \
    ' dc=' \
    ' enq=' \
    ' pc=' \
    '[BOOTLOG-FLUSH] records=' \
    ' text_bytes=' \
    ' slot_bytes=' \
    ' publish_us=' \
    ' publish=derived' \
    ' esp_write_us=' \
    ' commit_us=' \
    ' total_us=' \
    ' unavailable=esp_blocks,clusters'; do
    grep -Fq "$hw15_field" kernel/src/xhci_trb.c || {
        echo "MOUSE-DIAG step 3: форматтер не публикует «$hw15_field»" >&2
        exit 1
    }
done

# The mismatch rule is pure and host-proven.
grep -Fq 'bool loop_budget_accounting_mismatch(uint64_t sum_phase_us,' \
    kernel/include/loop_budget.h || {
    echo "MOUSE-DIAG step 3: нет правила timing_accounting_mismatch" >&2
    exit 1
}
grep -Fq 'return sum_phase_us > raw_elapsed_us + slack_us;' \
    kernel/src/loop_budget.c || {
    echo "MOUSE-DIAG step 3: правило несогласованности не то, что заявлено" >&2
    exit 1
}
grep -Fq 'test_ep_state_and_timing_records();' tests/xhci-host.c || {
    echo "MOUSE-DIAG step 3: host-проба записей не подключена" >&2
    exit 1
}
grep -Fq 'test_accounting_mismatch();' tests/loop-budget-host.c || {
    echo "MOUSE-DIAG step 3: host-проба правила несогласованности не подключена" >&2
    exit 1
}

# ------------------------------------------------- bound and never a marker --
grep -Fq '#define XHCI_TIMING_CHECK_RECORD_BYTES 320U' kernel/include/xhci_trb.h || {
    echo "MOUSE-DIAG step 3: запись времени не ограничена бюджетом" >&2
    exit 1
}
grep -Fq '#define XHCI_EP_STATE_RECORD_BYTES 320U' kernel/include/xhci_trb.h || {
    echo "MOUSE-DIAG step 3: снимок эндпойнта не ограничен бюджетом" >&2
    exit 1
}
grep -Fq "the widest timing record fits it too" tests/xhci-host.c || {
    echo "MOUSE-DIAG step 3: host-проба не проверяет ширину записи времени" >&2
    exit 1
}
for hw15_marker in '"[TIMING-CHECK' '"[EP-STATE' '"[BOOTLOG-FLUSH'; do
    if grep -Fq "$hw15_marker" kernel/src/appliance_boot_log.c; then
        echo "MOUSE-DIAG step 3: «$hw15_marker» стал flush-маркером" >&2
        exit 1
    fi
done

# ---------------------------------------------------- behaviour unchanged ----
python3 - "$PWD/kernel/src/xhci.c" <<'PY' || {
import sys

text = open(sys.argv[1], encoding='utf-8').read()
start = text.index('static void xhci_ep_state_publish(')
end = text.index('static uint64_t xhci_silence_publish(')
block = text[start:end]
banned = ('xhci_write32', 'xhci_write64', 'doorbell', 'xhci_reset',
          'xhci_port_reset', 'xhci_top_up_device', 'xhci_endpoint_recover',
          'xhci_disable_slot', 'xhci_address_device',
          'xhci_configure_endpoint')
sys.exit(1 if any(name in block for name in banned) else 0)
PY
    echo "MOUSE-DIAG step 3: снимок эндпойнта меняет состояние" >&2
    exit 1
}
# ITEM-487 (merged after this diagnostic branch) writes a run of consecutive
# slots with one range write instead of one call per slot, so the call this
# slice pinned by name is now the batched one.  The claim it protects is
# unchanged - the flush's records still leave through the store's own range
# write and nothing in these diagnostics grew a second write path - so it pins
# the call site that exists, and fails if the store stops writing the run.
grep -Fq 'store_flush_batch, batch_bytes' \
    kernel/src/appliance_boot_log_store.c || {
    echo "MOUSE-DIAG step 3: путь записи журнала изменён" >&2
    exit 1
}
grep -Fq 'appliance_boot_log_store_write_header();' \
    kernel/src/appliance_boot_log_store.c || {
    echo "MOUSE-DIAG step 3: путь commit журнала изменён" >&2
    exit 1
}
grep -Fq 'store_flush_stats.count += 1U;' kernel/src/appliance_boot_log_store.c || {
    echo "MOUSE-DIAG step 3: статистика flush'а не считается" >&2
    exit 1
}
