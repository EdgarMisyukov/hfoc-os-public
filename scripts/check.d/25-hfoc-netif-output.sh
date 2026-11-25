#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# HFOC netif output contract contracts.
grep -Fq 'test-hfoc-netif-output-host' Makefile || {
    echo "Makefile не содержит test-hfoc-netif-output-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-hfoc-netif-output-host' Makefile || {
    echo "host suite не содержит test-hfoc-netif-output-host" >&2
    exit 1
}
grep -Fq 'HFOC_NETIF_OUTPUT_VERSION' \
    kernel/include/hfoc_netif_output.h || {
    echo "hfoc_netif_output.h не содержит HFOC_NETIF_OUTPUT_VERSION" >&2
    exit 1
}
grep -Fq 'struct hfoc_netif_output' \
    kernel/include/hfoc_netif_output.h || {
    echo "hfoc_netif_output.h не содержит struct hfoc_netif_output" >&2
    exit 1
}
grep -Fq 'HFOC_NETIF_OUTPUT_ALREADY_SUBMITTED' \
    kernel/include/hfoc_netif_output.h || {
    echo "hfoc_netif_output.h не содержит already-submitted status" >&2
    exit 1
}
grep -Fq 'HFOC_NETIF_OUTPUT_INVALID_COMPLETION' \
    kernel/include/hfoc_netif_output.h || {
    echo "hfoc_netif_output.h не содержит invalid-completion status" >&2
    exit 1
}
if grep -nE '\b(void|uint8_t|unsigned char)\s*\*\s*hfoc_netif_' \
        kernel/include/hfoc_netif_output.h; then
    echo "hfoc_netif_output.h возвращает указатель на storage" >&2
    exit 1
fi
for hfoc_netif_output_symbol in hfoc_netif_output_init \
                               hfoc_netif_output_submit \
                               hfoc_netif_output_complete \
                               hfoc_netif_output_published \
                               hfoc_netif_output_pending_count \
                               hfoc_netif_output_result_string \
                               hfoc_netif_output_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $hfoc_netif_output_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $hfoc_netif_output_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/hfoc_netif_output.c; then
    echo "hfoc_netif_output.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'hfoc_netif_output_submit\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "HFOC netif output submit вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'hfoc_netif_output_host_test_'; then
    echo "hfoc_netif_output_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'HFOC netif output host tests: ALL OK'; then
    echo "Production kernel содержит netif output host marker" >&2
    exit 1
fi
for hfoc_netif_output_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'HFOC netif output contract|netif output contract' \
            "$hfoc_netif_output_doc"; then
        echo "$hfoc_netif_output_doc не документирует HFOC netif output contract" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **228. HFOC netif output contract' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan не помечает item 228 завершённым" >&2
    exit 1
}

# Packet pressure metrics and audit contracts.
grep -Fq 'test-packet-pressure-host' Makefile || {
    echo "Makefile не содержит test-packet-pressure-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-packet-pressure-host' Makefile || {
    echo "host suite не содержит test-packet-pressure-host" >&2
    exit 1
}
grep -Fq 'PACKET_PRESSURE_VERSION' \
    kernel/include/packet_pressure.h || {
    echo "packet_pressure.h не содержит PACKET_PRESSURE_VERSION" >&2
    exit 1
}
grep -Fq 'struct packet_pressure_snapshot' \
    kernel/include/packet_pressure.h || {
    echo "packet_pressure.h не содержит struct packet_pressure_snapshot" >&2
    exit 1
}
grep -Fq 'struct packet_pressure' \
    kernel/include/packet_pressure.h || {
    echo "packet_pressure.h не содержит struct packet_pressure" >&2
    exit 1
}
grep -Fq 'PACKET_PRESSURE_GENERATION_OVERFLOW' \
    kernel/include/packet_pressure.h || {
    echo "packet_pressure.h не содержит generation-overflow status" >&2
    exit 1
}
if sed -n '/struct packet_pressure_snapshot {/,/};/p' \
        kernel/include/packet_pressure.h | grep -q '\*'; then
    echo "packet_pressure_snapshot содержит указатель или адрес" >&2
    exit 1
fi
for packet_pressure_symbol in packet_pressure_init \
                              packet_pressure_begin_update \
                              packet_pressure_refresh_pool_counts \
                              packet_pressure_record_rx_drop \
                              packet_pressure_record_tx_backpressure \
                              packet_pressure_record_invalid_frame \
                              packet_pressure_commit \
                              packet_pressure_rollback \
                              packet_pressure_snapshot \
                              packet_pressure_published \
                              packet_pressure_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $packet_pressure_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $packet_pressure_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/packet_pressure.c; then
    echo "packet_pressure.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'packet_pressure_(refresh_pool_counts|record_rx_drop|record_tx_backpressure|record_invalid_frame|commit|rollback)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Packet pressure mutation вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'packet_pressure_host_test_'; then
    echo "packet_pressure_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Packet pressure host tests: ALL OK'; then
    echo "Production kernel содержит packet pressure host marker" >&2
    exit 1
fi
for packet_pressure_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'packet pressure metrics|packet pressure.*audit|packet-pressure' \
            "$packet_pressure_doc"; then
        echo "$packet_pressure_doc не документирует packet pressure metrics" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **229. Packet pressure metrics and audit' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan не помечает item 229 завершённым" >&2
    exit 1
}

# Minimal ICMP reachability contracts.
grep -Fq 'test-icmp-reachability-host' Makefile || {
    echo "Makefile не содержит test-icmp-reachability-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-icmp-reachability-host' Makefile || {
    echo "test: target не содержит test-icmp-reachability-host" >&2
    exit 1
}
grep -Fq 'ICMP_REACHABILITY_MESSAGE_MAX' \
    kernel/include/icmp_reachability.h || {
    echo "icmp_reachability.h не содержит ICMP_REACHABILITY_MESSAGE_MAX" >&2
    exit 1
}
grep -Fq 'ICMP_REACHABILITY_RATE_BUDGET_MAX' \
    kernel/include/icmp_reachability.h || {
    echo "icmp_reachability.h не содержит ICMP_REACHABILITY_RATE_BUDGET_MAX" >&2
    exit 1
}
grep -Fq 'ICMP_REACHABILITY_WINDOW_MAX' \
    kernel/include/icmp_reachability.h || {
    echo "icmp_reachability.h не содержит ICMP_REACHABILITY_WINDOW_MAX" >&2
    exit 1
}
grep -Fq 'struct icmp_reachability_engine' \
    kernel/include/icmp_reachability.h || {
    echo "icmp_reachability.h не содержит struct icmp_reachability_engine" >&2
    exit 1
}
for icmp_reachability_symbol in icmp_reachability_engine_init \
                                icmp_reachability_handle_request \
                                icmp_reachability_published \
                                icmp_reachability_window_count \
                                icmp_reachability_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $icmp_reachability_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $icmp_reachability_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/icmp_reachability.c; then
    echo "icmp_reachability.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'icmp_reachability_handle_request\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "ICMP reachability handling вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'icmp_reachability_host_test_'; then
    echo "icmp_reachability_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'ICMP reachability host tests: ALL OK'; then
    echo "Production kernel содержит ICMP reachability host marker" >&2
    exit 1
fi
for icmp_reachability_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'ICMP reachability|icmp reachability' \
            "$icmp_reachability_doc"; then
        echo "$icmp_reachability_doc не документирует ICMP reachability" >&2
        exit 1
    fi
done

# Management transport framing contracts.
grep -Fq 'test-management-transport-host' Makefile || {
    echo "Makefile не содержит test-management-transport-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-management-transport-host' Makefile || {
    echo "test: target не содержит test-management-transport-host" >&2
    exit 1
}
grep -Fq 'MANAGEMENT_TRANSPORT_VERSION' \
    kernel/include/management_transport.h || {
    echo "management_transport.h не содержит MANAGEMENT_TRANSPORT_VERSION" >&2
    exit 1
}
grep -Fq 'MANAGEMENT_TRANSPORT_HEADER_SIZE' \
    kernel/include/management_transport.h || {
    echo "management_transport.h не содержит MANAGEMENT_TRANSPORT_HEADER_SIZE" >&2
    exit 1
}
grep -Fq 'MANAGEMENT_TRANSPORT_PAYLOAD_MAX' \
    kernel/include/management_transport.h || {
    echo "management_transport.h не содержит MANAGEMENT_TRANSPORT_PAYLOAD_MAX" >&2
    exit 1
}
grep -Fq 'MANAGEMENT_TRANSPORT_FRAME_MAX' \
    kernel/include/management_transport.h || {
    echo "management_transport.h не содержит MANAGEMENT_TRANSPORT_FRAME_MAX" >&2
    exit 1
}
grep -Fq 'MANAGEMENT_TRANSPORT_FLAG_RESERVED_MASK' \
    kernel/include/management_transport.h || {
    echo "management_transport.h не содержит MANAGEMENT_TRANSPORT_FLAG_RESERVED_MASK" >&2
    exit 1
}
grep -Fq 'struct management_transport_policy' \
    kernel/include/management_transport.h || {
    echo "management_transport.h не содержит struct management_transport_policy" >&2
    exit 1
}
for management_transport_symbol in management_transport_validate_frame \
                                   management_transport_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $management_transport_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $management_transport_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/management_transport.c; then
    echo "management_transport.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'management_transport_validate_frame\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Management transport validation вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'management_transport_host_test_'; then
    echo "management_transport_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Management transport host tests: ALL OK'; then
    echo "Production kernel содержит management transport host marker" >&2
    exit 1
fi
for management_transport_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'management transport framing|management-transport framing' \
            "$management_transport_doc"; then
        echo "$management_transport_doc не документирует management transport framing" >&2
        exit 1
    fi
done

# Network admission policy contracts.
grep -Fq 'test-network-admission-host' Makefile || {
    echo "Makefile не содержит test-network-admission-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-network-admission-host' Makefile || {
    echo "test: target не содержит test-network-admission-host" >&2
    exit 1
}
grep -Fq 'NETWORK_ADMISSION_VERSION' \
    kernel/include/network_admission.h || {
    echo "network_admission.h не содержит NETWORK_ADMISSION_VERSION" >&2
    exit 1
}
grep -Fq 'NETWORK_ADMISSION_MODE_DENY_ALL' \
    kernel/include/network_admission.h || {
    echo "network_admission.h не содержит NETWORK_ADMISSION_MODE_DENY_ALL" >&2
    exit 1
}
grep -Fq 'NETWORK_ADMISSION_MODE_ROLE_SESSION' \
    kernel/include/network_admission.h || {
    echo "network_admission.h не содержит NETWORK_ADMISSION_MODE_ROLE_SESSION" >&2
    exit 1
}
grep -Fq 'struct network_admission_selection' \
    kernel/include/network_admission.h || {
    echo "network_admission.h не содержит struct network_admission_selection" >&2
    exit 1
}
grep -Fq 'struct network_admission_peer' \
    kernel/include/network_admission.h || {
    echo "network_admission.h не содержит struct network_admission_peer" >&2
    exit 1
}
for network_admission_symbol in network_admission_policy_validate \
                               network_admission_peer_validate \
                               network_admission_selection_init \
                               network_admission_selection_published \
                               network_admission_check_request \
                               network_admission_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $network_admission_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $network_admission_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/network_admission.c; then
    echo "network_admission.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'network_admission_check_request\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Network admission request check вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'network_admission_host_test_'; then
    echo "network_admission_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Network admission host tests: ALL OK'; then
    echo "Production kernel содержит network admission host marker" >&2
    exit 1
fi
for network_admission_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'network admission policy|network-admission policy' \
            "$network_admission_doc"; then
        echo "$network_admission_doc не документирует network admission policy" >&2
        exit 1
    fi
done

# Network audit/health event contracts.
grep -Fq 'test-network-event-host' Makefile || {
    echo "Makefile не содержит test-network-event-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-network-event-host' Makefile || {
    echo "test: target не содержит test-network-event-host" >&2
    exit 1
}
grep -Fq 'AUDIT_RECORD_NETWORK' kernel/include/audit.h || {
    echo "audit.h не содержит AUDIT_RECORD_NETWORK" >&2
    exit 1
}
grep -Fq 'NETWORK_EVENT_VERSION' kernel/include/network_event.h || {
    echo "network_event.h не содержит NETWORK_EVENT_VERSION" >&2
    exit 1
}
grep -Fq 'NETWORK_EVENT_TYPE_LINK' kernel/include/network_event.h || {
    echo "network_event.h не содержит NETWORK_EVENT_TYPE_LINK" >&2
    exit 1
}
grep -Fq 'NETWORK_EVENT_TYPE_ADDRESS' kernel/include/network_event.h || {
    echo "network_event.h не содержит NETWORK_EVENT_TYPE_ADDRESS" >&2
    exit 1
}
grep -Fq 'NETWORK_EVENT_TYPE_ADMISSION' kernel/include/network_event.h || {
    echo "network_event.h не содержит NETWORK_EVENT_TYPE_ADMISSION" >&2
    exit 1
}
grep -Fq 'NETWORK_EVENT_TYPE_TRANSPORT' kernel/include/network_event.h || {
    echo "network_event.h не содержит NETWORK_EVENT_TYPE_TRANSPORT" >&2
    exit 1
}
grep -Fq 'struct network_event' kernel/include/network_event.h || {
    echo "network_event.h не содержит struct network_event" >&2
    exit 1
}
for network_event_symbol in network_event_validate network_event_append \
                           network_event_from_event \
                           network_event_from_audit \
                           network_event_snapshot network_event_pop \
                           network_event_type_string \
                           network_event_result_string \
                           network_event_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $network_event_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $network_event_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/network_event.c; then
    echo "network_event.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'network_event_(append|pop|snapshot)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Network event emission/consumption вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'network_event_host_test_'; then
    echo "network_event_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Network event host tests: ALL OK'; then
    echo "Production kernel содержит network event host marker" >&2
    exit 1
fi
for network_event_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'network audit/health events|network-audit-health events|network event' \
            "$network_event_doc"; then
        echo "$network_event_doc не документирует network audit/health events" >&2
        exit 1
    fi
done

# Network-foundation QEMU proof contracts.
grep -Fq 'test-network-foundation-qemu' Makefile || {
    echo "Makefile не содержит test-network-foundation-qemu" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-network-foundation-qemu' Makefile || {
    echo "test: target не содержит test-network-foundation-qemu" >&2
    exit 1
}
grep -Fq 'scripts/test-qemu-network-foundation.sh' Makefile || {
    echo "Makefile не содержит network-foundation QEMU script" >&2
    exit 1
}
grep -Fq '[NETWORK-FOUNDATION] proof init' \
    kernel/src/network_foundation_qemu.c || {
    echo "network_foundation_qemu.c не содержит proof init marker" >&2
    exit 1
}
grep -Fq 'network_foundation_qemu_test_init' kernel/src/kernel.c || {
    echo "kernel.c не вызывает network-foundation proof" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[NETWORK-FOUNDATION]'; then
    echo "Production kernel содержит network-foundation marker" >&2
    exit 1
fi
if grep -Fq 'qemu-network-foundation' .github/workflows/ci.yml; then
    :
else
    echo "CI pgrep guard не содержит network-foundation script" >&2
    exit 1
fi
grep -Fq 'QEMU network-foundation baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-network-foundation.sh || {
    echo "test-qemu-network-foundation.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU network-foundation test: OK' \
    scripts/test-qemu-network-foundation.sh || {
    echo "test-qemu-network-foundation.sh не содержит success marker" >&2
    exit 1
}

# Virtio foundation QEMU proof contracts.
grep -Fq 'test-virtio-foundation-qemu' Makefile || {
    echo "Makefile does not contain test-virtio-foundation-qemu" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-virtio-foundation-qemu' Makefile || {
    echo "test: target does not contain test-virtio-foundation-qemu" >&2
    exit 1
}
grep -Fq 'scripts/test-qemu-virtio-foundation.sh' Makefile || {
    echo "Makefile does not contain virtio-foundation QEMU script" >&2
    exit 1
}
grep -Fq '[VIRTIO-FOUNDATION] proof init' \
    kernel/src/virtio_foundation_qemu.c || {
    echo "virtio_foundation_qemu.c does not contain proof init marker" >&2
    exit 1
}
grep -Fq 'virtio_foundation_qemu_test_init' kernel/src/kernel.c || {
    echo "kernel.c does not call virtio-foundation proof" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[VIRTIO-FOUNDATION]'; then
    echo "Production kernel contains virtio-foundation marker" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
    grep -F ' virtio_foundation_qemu_test_init'; then
    echo "Production kernel contains virtio-foundation proof symbol" >&2
    exit 1
fi
if grep -Fq 'qemu-virtio-foundation' .github/workflows/ci.yml; then
    :
else
    echo "CI pgrep guard does not contain virtio-foundation script" >&2
    exit 1
fi
grep -Fq 'QEMU virtio-foundation baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-virtio-foundation.sh || {
    echo "test-qemu-virtio-foundation.sh does not fix portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU virtio-foundation test: OK' \
    scripts/test-qemu-virtio-foundation.sh || {
    echo "test-qemu-virtio-foundation.sh does not contain success marker" >&2
    exit 1
}
for virtio_foundation_marker in \
    '[VIRTIO-FOUNDATION] proof init' \
    '[VIRTIO-FOUNDATION] discovery published' \
    '[VIRTIO-FOUNDATION] feature failure rejected' \
    '[VIRTIO-FOUNDATION] queue publication ok' \
    '[VIRTIO-FOUNDATION] queue completion ok' \
    '[VIRTIO-FOUNDATION] injected reset ok' \
    '[VIRTIO-FOUNDATION] exact rollback ok' \
    '[VIRTIO-FOUNDATION] DMA teardown ok' \
    '[VIRTIO-FOUNDATION] clean shutdown' \
    '[VIRTIO-FOUNDATION] proof OK'; do
    grep -Fq "$virtio_foundation_marker" \
        kernel/src/virtio_foundation_qemu.c || {
        echo "virtio_foundation_qemu.c lacks $virtio_foundation_marker" >&2
        exit 1
    }
    grep -Fq "$virtio_foundation_marker" \
        scripts/test-qemu-virtio-foundation.sh || {
        echo "virtio-foundation script lacks $virtio_foundation_marker" >&2
        exit 1
    }
done
for virtio_foundation_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'virtio foundation qemu proof|virtio-foundation-qemu' \
            "$virtio_foundation_doc"; then
        echo "$virtio_foundation_doc does not document virtio-foundation proof" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **210. Virtio foundation QEMU proof' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan does not mark item 210 completed" >&2
    exit 1
}
