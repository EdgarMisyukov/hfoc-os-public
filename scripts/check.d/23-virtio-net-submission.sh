#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Virtio-net TX submission contracts.
grep -Fq 'test-virtio-net-tx-host' Makefile || {
    echo "Makefile не содержит test-virtio-net-tx-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-virtio-net-tx-host' Makefile || {
    echo "test-host-suite не вызывает test-virtio-net-tx-host" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_TX_VERSION' \
    kernel/include/virtio_net_tx.h || {
    echo "virtio_net_tx.h не содержит version" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_TX_CAPACITY_MAX' \
    kernel/include/virtio_net_tx.h || {
    echo "virtio_net_tx.h не содержит capacity max" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_TX_FRAME_LENGTH_MAX' \
    kernel/include/virtio_net_tx.h || {
    echo "virtio_net_tx.h не содержит frame max" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_TX_DMA_DIRECTION' \
    kernel/include/virtio_net_tx.h || {
    echo "virtio_net_tx.h не содержит DMA direction" >&2
    exit 1
}
grep -Fq 'struct virtio_net_tx_config' \
    kernel/include/virtio_net_tx.h || {
    echo "virtio_net_tx.h не содержит config" >&2
    exit 1
}
grep -Fq 'struct virtio_net_tx_frame' \
    kernel/include/virtio_net_tx.h || {
    echo "virtio_net_tx.h не содержит frame" >&2
    exit 1
}
grep -Fq 'struct virtio_net_tx_submission' \
    kernel/include/virtio_net_tx.h || {
    echo "virtio_net_tx.h не содержит submission" >&2
    exit 1
}
grep -Fq 'struct virtio_net_tx' \
    kernel/include/virtio_net_tx.h || {
    echo "virtio_net_tx.h не содержит controller" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_TX_NOTIFY_FAILED' \
    kernel/include/virtio_net_tx.h || {
    echo "virtio_net_tx.h не содержит notify failure status" >&2
    exit 1
}
for virtio_net_tx_symbol in virtio_net_tx_activate \
                            virtio_net_tx_teardown \
                            virtio_net_tx_submit \
                            virtio_net_tx_published \
                            virtio_net_tx_generation \
                            virtio_net_tx_capacity \
                            virtio_net_tx_submitted_count \
                            virtio_net_tx_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $virtio_net_tx_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $virtio_net_tx_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/virtio_net_tx.c; then
    echo "virtio_net_tx.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'virtio_net_tx_(activate|teardown|submit)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Virtio-net TX submission вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'virtio_net_tx_host_test_'; then
    echo "virtio_net_tx_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Virtio net TX host tests: ALL OK'; then
    echo "Production kernel содержит virtio net TX host marker" >&2
    exit 1
fi
for virtio_net_tx_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'virtio[- ].*net[- ].*TX|virtio net TX submission' \
            "$virtio_net_tx_doc"; then
        echo "$virtio_net_tx_doc не документирует virtio-net TX submission" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **215. Virtio-net TX submission' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan не помечает item 215 завершённым" >&2
    exit 1
}

# Virtio-net TX reclaim contracts.
grep -Fq 'virtio_net_tx_reclaim' \
    kernel/include/virtio_net_tx.h || {
    echo "virtio_net_tx.h не содержит reclaim API" >&2
    exit 1
}
grep -Fq 'struct virtio_net_tx_completion' \
    kernel/include/virtio_net_tx.h || {
    echo "virtio_net_tx.h не содержит completion" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_TX_USED_EMPTY' \
    kernel/include/virtio_net_tx.h || {
    echo "virtio_net_tx.h не содержит used empty status" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_TX_INVALID_HEAD' \
    kernel/include/virtio_net_tx.h || {
    echo "virtio_net_tx.h не содержит invalid head status" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_TX_INVALID_LENGTH' \
    kernel/include/virtio_net_tx.h || {
    echo "virtio_net_tx.h не содержит invalid length status" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_TX_STALE_COMPLETION' \
    kernel/include/virtio_net_tx.h || {
    echo "virtio_net_tx.h не содержит stale completion status" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_TX_RELEASE_FAILED' \
    kernel/include/virtio_net_tx.h || {
    echo "virtio_net_tx.h не содержит release failed status" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_TX_USED_INVALID' \
    kernel/include/virtio_net_tx.h || {
    echo "virtio_net_tx.h не содержит used invalid status" >&2
    exit 1
}
grep -Fq 'virtio_net_tx_reclaim' \
    tests/virtio-net-tx-host.c || {
    echo "TX reclaim host fixture не вызывает reclaim API" >&2
    exit 1
}
for virtio_net_tx_reclaim_symbol in virtio_net_tx_reclaim \
                                    virtio_net_tx_reclaimed_count \
                                    virtio_net_tx_invalid_head_count \
                                    virtio_net_tx_invalid_length_count \
                                    virtio_net_tx_stale_completion_count; do
    nm build/hfoc-kernel.elf | grep -F " $virtio_net_tx_reclaim_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $virtio_net_tx_reclaim_symbol" >&2
        exit 1
    }
done
if grep -nE 'virtio_net_tx_reclaim\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Virtio-net TX reclaim вызывается из interrupt path" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'TX reclaim success/wraparound allocates nothing'; then
    echo "Production kernel содержит virtio net TX reclaim host marker" >&2
    exit 1
fi
for virtio_net_tx_reclaim_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'virtio[- ].*net[- ].*TX.*reclaim|virtio net TX reclaim' \
            "$virtio_net_tx_reclaim_doc"; then
        echo "$virtio_net_tx_reclaim_doc не документирует virtio-net TX reclaim" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **216. Virtio-net TX reclaim' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan не помечает item 216 завершённым" >&2
    exit 1
}

# Virtio-net interrupt path contracts.
grep -Fq 'test-virtio-net-interrupt-host' Makefile || {
    echo "Makefile не содержит test-virtio-net-interrupt-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-virtio-net-interrupt-host' Makefile || {
    echo "test-host-suite не вызывает test-virtio-net-interrupt-host" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_INTERRUPT_VERSION' \
    kernel/include/virtio_net_interrupt.h || {
    echo "virtio_net_interrupt.h не содержит version" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_INTERRUPT_GENERATION_MIN' \
    kernel/include/virtio_net_interrupt.h || {
    echo "virtio_net_interrupt.h не содержит generation min" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_INTERRUPT_ISR_STATUS_USED_RING' \
    kernel/include/virtio_net_interrupt.h || {
    echo "virtio_net_interrupt.h не содержит used-ring ISR status" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_INTERRUPT_ISR_STATUS_CONFIG' \
    kernel/include/virtio_net_interrupt.h || {
    echo "virtio_net_interrupt.h не содержит config ISR status" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_INTERRUPT_ISR_STATUS_MASK' \
    kernel/include/virtio_net_interrupt.h || {
    echo "virtio_net_interrupt.h не содержит ISR status mask" >&2
    exit 1
}
grep -Fq 'struct virtio_net_interrupt_work' \
    kernel/include/virtio_net_interrupt.h || {
    echo "virtio_net_interrupt.h не содержит work summary" >&2
    exit 1
}
grep -Fq 'struct virtio_net_interrupt_io' \
    kernel/include/virtio_net_interrupt.h || {
    echo "virtio_net_interrupt.h не содержит IO seam" >&2
    exit 1
}
grep -Fq 'struct virtio_net_interrupt_direction' \
    kernel/include/virtio_net_interrupt.h || {
    echo "virtio_net_interrupt.h не содержит direction seam" >&2
    exit 1
}
grep -Fq 'struct virtio_net_interrupt_publication' \
    kernel/include/virtio_net_interrupt.h || {
    echo "virtio_net_interrupt.h не содержит publication seam" >&2
    exit 1
}
grep -Fq 'struct virtio_net_interrupt' \
    kernel/include/virtio_net_interrupt.h || {
    echo "virtio_net_interrupt.h не содержит controller" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_INTERRUPT_SPURIOUS' \
    kernel/include/virtio_net_interrupt.h || {
    echo "virtio_net_interrupt.h не содержит spurious status" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_INTERRUPT_EMPTY_WORK' \
    kernel/include/virtio_net_interrupt.h || {
    echo "virtio_net_interrupt.h не содержит empty work status" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_INTERRUPT_ACK_FAILED' \
    kernel/include/virtio_net_interrupt.h || {
    echo "virtio_net_interrupt.h не содержит ack failed status" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_INTERRUPT_INVALID_STATUS' \
    kernel/include/virtio_net_interrupt.h || {
    echo "virtio_net_interrupt.h не содержит invalid status" >&2
    exit 1
}
for virtio_net_interrupt_symbol in \
        virtio_net_interrupt_activate \
        virtio_net_interrupt_handle \
        virtio_net_interrupt_published \
        virtio_net_interrupt_generation \
        virtio_net_interrupt_isr_ack_count \
        virtio_net_interrupt_used_ring_count \
        virtio_net_interrupt_config_count \
        virtio_net_interrupt_empty_work_count \
        virtio_net_interrupt_spurious_count \
        virtio_net_interrupt_published_work_count \
        virtio_net_interrupt_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $virtio_net_interrupt_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $virtio_net_interrupt_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/virtio_net_interrupt.c; then
    echo "virtio_net_interrupt.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'virtio_net_interrupt_activate\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Virtio-net interrupt activation вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'virtio_net_interrupt_host_test_'; then
    echo "virtio_net_interrupt_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Virtio net interrupt host tests: ALL OK'; then
    echo "Production kernel содержит virtio net interrupt host marker" >&2
    exit 1
fi
for virtio_net_interrupt_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'virtio[- ].*net[- ].*interrupt|virtio net interrupt path' \
            "$virtio_net_interrupt_doc"; then
        echo "$virtio_net_interrupt_doc не документирует virtio-net interrupt path" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **217. Virtio-net interrupt path' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan не помечает item 217 завершённым" >&2
    exit 1
}

# Virtio-net link-state binding contracts.
grep -Fq 'test-virtio-net-link-state-host' Makefile || {
    echo "Makefile не содержит test-virtio-net-link-state-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-virtio-net-link-state-host' Makefile || {
    echo "test-host-suite/test не вызывает test-virtio-net-link-state-host" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_LINK_STATE_VERSION' \
    kernel/include/virtio_net_link_state.h || {
    echo "virtio_net_link_state.h не содержит version" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_LINK_STATE_GENERATION_MIN' \
    kernel/include/virtio_net_link_state.h || {
    echo "virtio_net_link_state.h не содержит generation min" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_LINK_CAPABILITY_MASK' \
    kernel/include/virtio_net_link_state.h || {
    echo "virtio_net_link_state.h не содержит capability mask" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_LINK_STATUS_MASK' \
    kernel/include/virtio_net_link_state.h || {
    echo "virtio_net_link_state.h не содержит status mask" >&2
    exit 1
}
grep -Fq 'struct virtio_net_link_io' \
    kernel/include/virtio_net_link_state.h || {
    echo "virtio_net_link_state.h не содержит IO seam" >&2
    exit 1
}
grep -Fq 'struct virtio_net_link_readiness' \
    kernel/include/virtio_net_link_state.h || {
    echo "virtio_net_link_state.h не содержит readiness" >&2
    exit 1
}
grep -Fq 'struct virtio_net_link_state' \
    kernel/include/virtio_net_link_state.h || {
    echo "virtio_net_link_state.h не содержит binding" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_LINK_QUEUE_READINESS_MISMATCH' \
    kernel/include/virtio_net_link_state.h || {
    echo "virtio_net_link_state.h не содержит readiness status" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_LINK_INVALID_DEVICE_STATUS' \
    kernel/include/virtio_net_link_state.h || {
    echo "virtio_net_link_state.h не содержит device status" >&2
    exit 1
}
for virtio_net_link_symbol in virtio_net_link_state_activate \
                              virtio_net_link_state_transition \
                              virtio_net_link_state_published \
                              virtio_net_link_state_selection \
                              virtio_net_link_state_path_enabled \
                              virtio_net_link_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $virtio_net_link_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $virtio_net_link_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/virtio_net_link_state.c; then
    echo "virtio_net_link_state.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'virtio_net_link_state_(activate|transition)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Virtio-net link-state binding вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'virtio_net_link_state_host_test_'; then
    echo "virtio_net_link_state_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Virtio net link-state binding host tests: ALL OK'; then
    echo "Production kernel содержит virtio net link-state binding host marker" >&2
    exit 1
fi
for virtio_net_link_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'virtio[- ].*net[- ].*link[- ]state|virtio net link-state binding' \
            "$virtio_net_link_doc"; then
        echo "$virtio_net_link_doc не документирует virtio-net link-state binding" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **218. Virtio-net link-state binding' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan не помечает item 218 завершённым" >&2
    exit 1
}

# Virtio-net reset and shutdown contracts.
grep -Fq 'test-virtio-net-lifecycle-host' Makefile || {
    echo "Makefile не содержит test-virtio-net-lifecycle-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-virtio-net-lifecycle-host' Makefile || {
    echo "test-host-suite/test не вызывает test-virtio-net-lifecycle-host" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_LIFECYCLE_VERSION' \
    kernel/include/virtio_net_lifecycle.h || {
    echo "virtio_net_lifecycle.h не содержит version" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_LIFECYCLE_GENERATION_MIN' \
    kernel/include/virtio_net_lifecycle.h || {
    echo "virtio_net_lifecycle.h не содержит generation min" >&2
    exit 1
}
grep -Fq 'struct virtio_net_notification_io' \
    kernel/include/virtio_net_lifecycle.h || {
    echo "virtio_net_lifecycle.h не содержит notification IO" >&2
    exit 1
}
grep -Fq 'struct virtio_net_lifecycle' \
    kernel/include/virtio_net_lifecycle.h || {
    echo "virtio_net_lifecycle.h не содержит lifecycle controller" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_LIFECYCLE_NOTIFICATIONS_FAILED' \
    kernel/include/virtio_net_lifecycle.h || {
    echo "virtio_net_lifecycle.h не содержит quiesce failure status" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_LIFECYCLE_ADMISSION_STOP_FAILED' \
    kernel/include/virtio_net_lifecycle.h || {
    echo "virtio_net_lifecycle.h не содержит admission failure status" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_LIFECYCLE_DEVICE_NOT_LIVE' \
    kernel/include/virtio_net_lifecycle.h || {
    echo "virtio_net_lifecycle.h не содержит device-not-live status" >&2
    exit 1
}
for virtio_net_lifecycle_symbol in virtio_net_lifecycle_activate \
                                  virtio_net_lifecycle_stop \
                                  virtio_net_lifecycle_published \
                                  virtio_net_lifecycle_stopped \
                                  virtio_net_lifecycle_generation \
                                  virtio_net_lifecycle_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $virtio_net_lifecycle_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $virtio_net_lifecycle_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/virtio_net_lifecycle.c; then
    echo "virtio_net_lifecycle.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'virtio_net_lifecycle_(activate|stop)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Virtio-net lifecycle controller вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'virtio_net_lifecycle_host_test_'; then
    echo "virtio_net_lifecycle_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Virtio net reset and shutdown host tests: ALL OK'; then
    echo "Production kernel содержит virtio net reset and shutdown host marker" >&2
    exit 1
fi
for virtio_net_lifecycle_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'virtio[- ].*net[- ].*reset|virtio net reset and shutdown' \
            "$virtio_net_lifecycle_doc"; then
        echo "$virtio_net_lifecycle_doc не документирует virtio-net reset and shutdown" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **219. Virtio-net reset and shutdown' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan не помечает item 219 завершённым" >&2
    exit 1
}

# Fixed IPv4 configuration parser contracts.
grep -Fq 'test-ipv4-config-host' Makefile || {
    echo "Makefile не содержит test-ipv4-config-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-ipv4-config-host' Makefile || {
    echo "test: target не содержит test-ipv4-config-host" >&2
    exit 1
}
grep -Fq 'NETWORK_IPV4_CONFIG_VERSION' \
    kernel/include/ipv4_config.h || {
    echo "ipv4_config.h не содержит NETWORK_IPV4_CONFIG_VERSION" >&2
    exit 1
}
grep -Fq 'NETWORK_IPV4_PREFIX_MIN' \
    kernel/include/ipv4_config.h || {
    echo "ipv4_config.h не содержит NETWORK_IPV4_PREFIX_MIN" >&2
    exit 1
}
grep -Fq 'NETWORK_IPV4_PREFIX_MAX' \
    kernel/include/ipv4_config.h || {
    echo "ipv4_config.h не содержит NETWORK_IPV4_PREFIX_MAX" >&2
    exit 1
}
grep -Fq 'struct network_ipv4_config' \
    kernel/include/ipv4_config.h || {
    echo "ipv4_config.h не содержит struct network_ipv4_config" >&2
    exit 1
}
for ipv4_config_symbol in network_ipv4_config_parse \
                          network_ipv4_config_validate \
                          network_ipv4_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $ipv4_config_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $ipv4_config_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/ipv4_config.c; then
    echo "ipv4_config.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'network_ipv4_config_parse\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "IPv4 configuration parsing вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'ipv4_config_host_test_'; then
    echo "ipv4_config_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'IPv4 config host tests: ALL OK'; then
    echo "Production kernel содержит IPv4 config host marker" >&2
    exit 1
fi
for ipv4_config_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'IPv4 configuration parser|ipv4 configuration' \
            "$ipv4_config_doc"; then
        echo "$ipv4_config_doc не документирует fixed IPv4 configuration parser" >&2
        exit 1
    fi
done

# Network configuration transaction contracts.
grep -Fq 'test-network-config-transaction-host' Makefile || {
    echo "Makefile не содержит test-network-config-transaction-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-network-config-transaction-host' Makefile || {
    echo "test: target не содержит test-network-config-transaction-host" >&2
    exit 1
}
grep -Fq 'NETWORK_CONFIG_TRANSACTION_VERSION' \
    kernel/include/network_config_transaction.h || {
    echo "network_config_transaction.h не содержит NETWORK_CONFIG_TRANSACTION_VERSION" >&2
    exit 1
}
grep -Fq 'struct network_config_transaction' \
    kernel/include/network_config_transaction.h || {
    echo "network_config_transaction.h не содержит struct network_config_transaction" >&2
    exit 1
}
for network_config_transaction_symbol in \
        network_config_transaction_init \
        network_config_transaction_stage \
        network_config_transaction_commit \
        network_config_transaction_rollback \
        network_config_transaction_active \
        network_config_transaction_has_pending \
        network_config_transaction_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $network_config_transaction_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $network_config_transaction_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/network_config_transaction.c; then
    echo "network_config_transaction.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'network_config_transaction_(stage|commit|rollback)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Network configuration transaction вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'network_config_transaction_host_test_'; then
    echo "network_config_transaction_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Network configuration transaction host tests: ALL OK'; then
    echo "Production kernel содержит network configuration transaction host marker" >&2
    exit 1
fi
for network_config_transaction_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'network configuration transaction|network-config transaction' \
            "$network_config_transaction_doc"; then
        echo "$network_config_transaction_doc не документирует network configuration transaction" >&2
        exit 1
    fi
done
