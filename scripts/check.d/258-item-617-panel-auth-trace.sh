#!/usr/bin/env bash
# ITEM-617: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with shellcheck.
#
# The reproduced Toshiba incident lost ARP/ICMP/SSH after a browser sign-in
# while the local console stayed alive.  The only measured, code-supported
# first error is the synchronous account KDF inside the serving poll (~21 s in
# the owner's trace); the permanent receive loss is not proved and must not be
# "fixed" by a speculative NIC reset.  This slice pins the two pieces the next
# hardware snapshot needs: an honest panel-login attempt record (pending,
# finished, duration, result, no credential) and receive counters that follow
# the frame instead of the zero value of the lwIP input enum.
# shellcheck disable=SC2016,SC2041

item617_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item617_root" || exit 1

# 1. The trace keeps bounded scalars only and has an honest cold state.
for item617_header in \
    '#define APPLIANCE_PANEL_AUTH_TRACE_LINE_MAX 320U' \
    'APPLIANCE_PANEL_AUTH_NOT_ATTEMPTED' \
    'APPLIANCE_PANEL_AUTH_IN_PROGRESS' \
    'APPLIANCE_PANEL_AUTH_COMPLETED' \
    'uint32_t attempt;' \
    'uint32_t started_ms;' \
    'uint32_t duration_ms;' \
    'enum appliance_panel_auth_trace_result result;' \
    'uint32_t kdf_us;' \
    'uint32_t wait_us;' \
    'uint32_t slices;' \
    'uint32_t max_gap_us;' \
    'uint32_t rounds_done;' \
    'uint32_t rounds_total;' \
    'appliance_panel_auth_trace_begin(' \
    'appliance_panel_auth_trace_slice(' \
    'appliance_panel_auth_trace_commit(' \
    'appliance_panel_auth_trace_status_report('; do
    grep -Fq -- "$item617_header" kernel/include/appliance_panel_auth_trace.h || {
        echo "ITEM-617: заголовок trace не содержит:" "$item617_header" >&2
        exit 1
    }
done
if grep -Eiq 'password|passphrase|private|verifier|token|cookie|seed_bytes' \
    kernel/src/appliance_panel_auth_trace.c; then
    echo "ITEM-617: trace-модуль называет секретное поле" >&2
    exit 1
fi

# 2. One renderer publishes the pending/finished/result fields to the shared
#    status composition; a new attempt replaces the previous success.
for item617_render in \
    '"[PANEL-AUTH] v=1"' \
    'state=COMPLETED' \
    'state=IN_PROGRESS' \
    'state=NOT_ATTEMPTED' \
    '" result="' \
    '"attempt"' \
    '"started_ms"' \
    '"duration_ms"' \
    '"kdf_us"' \
    '"wait_us"' \
    '"slices"' \
    '"max_gap_us"' \
    '"rounds_done"' \
    '"rounds_total"' \
    'return "OK";' \
    'return "DENIED";' \
    'return "THROTTLED";' \
    'return "UNAVAILABLE"'; do
    grep -Fq -- "$item617_render" kernel/src/appliance_panel_auth_trace.c || {
        echo "ITEM-617: renderer не содержит:" "$item617_render" >&2
        exit 1
    }
done
for item617_hook in \
    'appliance_panel_auth_trace_begin(' \
    'appliance_panel_auth_trace_commit(' \
    'appliance_production_serving_panel_auth_now_ms())' \
    'appliance_production_serving_panel_auth_now_ms(void)' \
    'appliance_panel_auth_trace_slice(' \
    'timer_cycles_to_us(finished - started)'; do
    grep -Fq -- "$item617_hook" kernel/src/appliance_production_serving.c || {
        echo "ITEM-617: panel adapter не содержит:" "$item617_hook" >&2
        exit 1
    }
done
grep -Fq 'appliance_panel_auth_trace_status_report(sink, context);' \
    kernel/src/kernel.c || {
    echo "ITEM-617: production status не публикует [PANEL-AUTH]" >&2
    exit 1
}
grep -Fq 'appliance_panel_auth_trace_status_report(sink, context);' \
    kernel/src/kernel_status_report_profile.c || {
    echo "ITEM-617: profile fallback не публикует [PANEL-AUTH]" >&2
    exit 1
}

# 3. The receive counter follows the frame, not the enum's zero value; the old
#    `!wire && !input` inversion must not return to any backend.
grep -Fq 'appliance_production_network_rx_frame_received(' \
    kernel/include/appliance_production_network_rx_accounting.h || {
    echo "ITEM-617: правило счётчиков приёма не объявлено" >&2
    exit 1
}
item617_rx_sites=$(grep -Fc 'appliance_production_network_rx_frame_received(' \
    kernel/src/appliance_production_network_x86.c || true)
[[ $item617_rx_sites -eq 3 ]] || {
    echo "ITEM-617: правило приёма применено не к трём backend ($item617_rx_sites)" >&2
    exit 1
}
if grep -Fq '!appliance_production_network_input(' \
    kernel/src/appliance_production_network_x86.c; then
    echo "ITEM-617: старый инвертированный счётчик приёма вернулся" >&2
    exit 1
fi

# 4. Both host proofs are built and registered in the canonical suite.
for item617_proof in \
    'panel auth trace host: ALL OK' \
    'state=IN_PROGRESS' \
    'duration_ms=21000' \
    'result=DENIED' \
    'kdf_us=1700' \
    'max_gap_us=200'; do
    grep -Fq -- "$item617_proof" tests/panel-auth-trace-host.c || {
        echo "ITEM-617: panel trace proof не содержит:" "$item617_proof" >&2
        exit 1
    }
done
for item617_rx in \
    'a frame accepted by lwIP was not counted as received' \
    'a rejected frame was counted as received'; do
    grep -Fq -- "$item617_rx" tests/network-rx-accounting-host.c || {
        echo "ITEM-617: rx accounting proof не содержит:" "$item617_rx" >&2
        exit 1
    }
done
for item617_target in \
    'test-panel-auth-trace-host' \
    'test-network-rx-accounting-host'; do
    grep -Fq -- "$item617_target" Makefile || {
        echo "ITEM-617: Makefile не содержит:" "$item617_target" >&2
        exit 1
    }
    grep -Fq -- "\$(MAKE) $item617_target" Makefile || {
        echo "ITEM-617: suite не запускает:" "$item617_target" >&2
        exit 1
    }
done

# 5. The real save/extract proof carries the record in the saved session, and
#    the trace module is linked into that proof.
for item617_diag in \
    'appliance_panel_auth_trace_status_report(sink, context);' \
    '[PANEL-AUTH] v=1' \
    'state=COMPLETED' \
    'duration_ms=21000' \
    'state=END records=10 crc='; do
    grep -Fq -- "$item617_diag" tests/diag-export-full-host.c || {
        echo "ITEM-617: diag-export proof не содержит:" "$item617_diag" >&2
        exit 1
    }
done
grep -Fq 'kernel/src/appliance_panel_auth_trace.c' Makefile || {
    echo "ITEM-617: trace не собран в host proof" >&2
    exit 1
}
