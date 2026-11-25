#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Entropy diagnostics privacy contracts (item 258).
grep -Fq 'test-entropy-diagnostics-host' Makefile || {
    echo "Makefile не содержит test-entropy-diagnostics-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-entropy-diagnostics-host' Makefile || {
    echo "test-host-suite не вызывает test-entropy-diagnostics-host" >&2
    exit 1
}
grep -Fq 'ENTROPY_DIAGNOSTICS_VERSION' \
    kernel/include/entropy_diagnostics.h || {
    echo "entropy_diagnostics.h не содержит version" >&2
    exit 1
}
grep -Fq 'ENTROPY_DIAGNOSTICS_POLICY_ID' \
    kernel/include/entropy_diagnostics.h || {
    echo "entropy_diagnostics.h не содержит policy ID" >&2
    exit 1
}
grep -Fq 'ENTROPY_DIAGNOSTICS_SERIAL_MAX' \
    kernel/include/entropy_diagnostics.h || {
    echo "entropy_diagnostics.h не содержит serial max" >&2
    exit 1
}
grep -Fq 'ENTROPY_DIAGNOSTICS_SUPPORT_SIZE' \
    kernel/include/entropy_diagnostics.h || {
    echo "entropy_diagnostics.h не содержит support size" >&2
    exit 1
}
grep -Fq 'ENTROPY_DIAGNOSTICS_STATE_UNAVAILABLE' \
    kernel/include/entropy_diagnostics.h || {
    echo "entropy_diagnostics.h не содержит unavailable state" >&2
    exit 1
}
grep -Fq 'ENTROPY_DIAGNOSTICS_STATE_DEGRADED' \
    kernel/include/entropy_diagnostics.h || {
    echo "entropy_diagnostics.h не содержит degraded state" >&2
    exit 1
}
grep -Fq 'ENTROPY_DIAGNOSTICS_STATE_READY' \
    kernel/include/entropy_diagnostics.h || {
    echo "entropy_diagnostics.h не содержит ready state" >&2
    exit 1
}
grep -Fq 'ENTROPY_DIAGNOSTICS_FAULT_FILL_EXHAUSTED' \
    kernel/include/entropy_diagnostics.h || {
    echo "entropy_diagnostics.h не содержит fill-exhausted fault" >&2
    exit 1
}
grep -Fq 'struct entropy_diagnostics_snapshot' \
    kernel/include/entropy_diagnostics.h || {
    echo "entropy_diagnostics.h не содержит snapshot" >&2
    exit 1
}
grep -Fq 'struct entropy_diagnostics_audit' \
    kernel/include/entropy_diagnostics.h || {
    echo "entropy_diagnostics.h не содержит audit payload" >&2
    exit 1
}
grep -Fq 'enum entropy_diagnostics_state' \
    kernel/include/entropy_diagnostics.h || {
    echo "entropy_diagnostics.h не содержит state enum" >&2
    exit 1
}
grep -Fq 'enum entropy_diagnostics_fault' \
    kernel/include/entropy_diagnostics.h || {
    echo "entropy_diagnostics.h не содержит fault enum" >&2
    exit 1
}
if grep -nE 'entropy_diagnostics_(read|bytes|seed|accumulator|address|pointer)\(' \
        kernel/include/entropy_diagnostics.h; then
    echo "entropy_diagnostics.h предоставляет публичное чтение seed/bytes" >&2
    exit 1
fi
for entropy_diagnostics_symbol in entropy_diagnostics_snapshot \
                                  entropy_diagnostics_serial \
                                  entropy_diagnostics_audit \
                                  entropy_diagnostics_support \
                                  entropy_diagnostics_state_string \
                                  entropy_diagnostics_fault_string \
                                  entropy_diagnostics_status_string \
                                  entropy_diagnostics_version \
                                  entropy_diagnostics_policy_id; do
    nm build/hfoc-kernel.elf | grep -F " $entropy_diagnostics_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $entropy_diagnostics_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/entropy_diagnostics.c; then
    echo "entropy_diagnostics.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'entropy_diagnostics_(snapshot|serial|audit|support)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Entropy diagnostics вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'entropy_diagnostics_host_test_'; then
    echo "entropy_diagnostics_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Entropy diagnostics host tests: ALL OK'; then
    echo "Production kernel содержит entropy diagnostics host marker" >&2
    exit 1
fi
for entropy_diagnostics_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'entropy[- ].*diagnostics|diagnostics[- ].*entropy' \
            "$entropy_diagnostics_doc"; then
        echo "$entropy_diagnostics_doc не документирует entropy diagnostics privacy" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **258. Entropy diagnostics privacy' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan не помечает item 258 завершённым" >&2
    exit 1
}

# Entropy failure and restart contracts (item 259).
grep -Fq 'test-entropy-restart-host' Makefile || {
    echo "Makefile не содержит test-entropy-restart-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-entropy-restart-host' Makefile || {
    echo "test-host-suite не вызывает test-entropy-restart-host" >&2
    exit 1
}
grep -Fq 'ENTROPY_RESTART_VERSION' \
    kernel/include/entropy_restart.h || {
    echo "entropy_restart.h не содержит version" >&2
    exit 1
}
grep -Fq 'ENTROPY_RESTART_GENERATION_MIN' \
    kernel/include/entropy_restart.h || {
    echo "entropy_restart.h не содержит generation min" >&2
    exit 1
}
grep -Fq 'ENTROPY_RESTART_SOURCE_ID_MIN' \
    kernel/include/entropy_restart.h || {
    echo "entropy_restart.h не содержит source identity min" >&2
    exit 1
}
grep -Fq 'ENTROPY_RESTART_RESTART_COUNT_MAX' \
    kernel/include/entropy_restart.h || {
    echo "entropy_restart.h не содержит restart count max" >&2
    exit 1
}
grep -Fq 'ENTROPY_RESTART_INTERVAL_MIN_TICKS' \
    kernel/include/entropy_restart.h || {
    echo "entropy_restart.h не содержит interval min" >&2
    exit 1
}
grep -Fq 'ENTROPY_RESTART_INTERVAL_MAX_TICKS' \
    kernel/include/entropy_restart.h || {
    echo "entropy_restart.h не содержит interval max" >&2
    exit 1
}
grep -Fq 'ENTROPY_RESTART_FAULT_MISSING_DEVICE' \
    kernel/include/entropy_restart.h || {
    echo "entropy_restart.h не содержит missing-device fault" >&2
    exit 1
}
grep -Fq 'ENTROPY_RESTART_FAULT_STALLED' \
    kernel/include/entropy_restart.h || {
    echo "entropy_restart.h не содержит stalled fault" >&2
    exit 1
}
grep -Fq 'ENTROPY_RESTART_FAULT_REPEATED_OUTPUT' \
    kernel/include/entropy_restart.h || {
    echo "entropy_restart.h не содержит repeated-output fault" >&2
    exit 1
}
grep -Fq 'ENTROPY_RESTART_FAULT_DEVICE_RESET' \
    kernel/include/entropy_restart.h || {
    echo "entropy_restart.h не содержит device-reset fault" >&2
    exit 1
}
grep -Fq 'ENTROPY_RESTART_STATE_UNAVAILABLE' \
    kernel/include/entropy_restart.h || {
    echo "entropy_restart.h не содержит unavailable state" >&2
    exit 1
}
grep -Fq 'ENTROPY_RESTART_STATE_READY' \
    kernel/include/entropy_restart.h || {
    echo "entropy_restart.h не содержит ready state" >&2
    exit 1
}
grep -Fq 'ENTROPY_RESTART_STATE_FAILED' \
    kernel/include/entropy_restart.h || {
    echo "entropy_restart.h не содержит failed state" >&2
    exit 1
}
grep -Fq 'ENTROPY_RESTART_STATE_STOPPED' \
    kernel/include/entropy_restart.h || {
    echo "entropy_restart.h не содержит stopped state" >&2
    exit 1
}
grep -Fq 'struct entropy_restart_config' \
    kernel/include/entropy_restart.h || {
    echo "entropy_restart.h не содержит config" >&2
    exit 1
}
grep -Fq 'struct entropy_restart' \
    kernel/include/entropy_restart.h || {
    echo "entropy_restart.h не содержит lifecycle" >&2
    exit 1
}
grep -Fq 'enum entropy_restart_state' \
    kernel/include/entropy_restart.h || {
    echo "entropy_restart.h не содержит state enum" >&2
    exit 1
}
grep -Fq 'enum entropy_restart_fault' \
    kernel/include/entropy_restart.h || {
    echo "entropy_restart.h не содержит fault enum" >&2
    exit 1
}
if grep -nE 'entropy_restart_(read|bytes|seed|accumulator|pointer)\(' \
        kernel/include/entropy_restart.h; then
    echo "entropy_restart.h предоставляет публичное чтение seed/bytes" >&2
    exit 1
fi
for entropy_restart_symbol in entropy_restart_init \
                              entropy_restart_activate \
                              entropy_restart_check \
                              entropy_restart_stop \
                              entropy_restart_restart \
                              entropy_restart_initialized \
                              entropy_restart_ready \
                              entropy_restart_stopped \
                              entropy_restart_state \
                              entropy_restart_fault \
                              entropy_restart_generation \
                              entropy_restart_last_generation \
                              entropy_restart_restart_count \
                              entropy_restart_last_restart_tick \
                              entropy_restart_status_string \
                              entropy_restart_state_string \
                              entropy_restart_fault_string; do
    nm build/hfoc-kernel.elf | grep -F " $entropy_restart_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $entropy_restart_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/entropy_restart.c; then
    echo "entropy_restart.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'entropy_restart_(init|activate|check|stop|restart)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Entropy restart вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'entropy_restart_host_test_'; then
    echo "entropy_restart_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Entropy restart host tests: ALL OK'; then
    echo "Production kernel содержит entropy restart host marker" >&2
    exit 1
fi
for entropy_restart_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'entropy[- ].*restart|restart[- ].*entropy' \
            "$entropy_restart_doc"; then
        echo "$entropy_restart_doc не документирует entropy failure/restart" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **259. Entropy failure and restart' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan не помечает item 259 завершённым" >&2
    exit 1
}

# Entropy QEMU proof (item 260).
grep -Fq 'test-entropy-qemu' Makefile || {
    echo "Makefile не содержит test-entropy-qemu" >&2
    exit 1
}
grep -Fq '[t]est-qemu-entropy' .github/workflows/ci.yml || {
    echo "CI не содержит test-qemu-entropy leak detection" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(ENTROPY_ISO)' Makefile || {
    echo "Makefile не включает entropy ISO в QEMU suite" >&2
    exit 1
}
grep -Fq 'HFOC_ENTROPY_QEMU_TEST' Makefile || {
    echo "Makefile не содержит HFOC_ENTROPY_QEMU_TEST" >&2
    exit 1
}
grep -Fq 'entropy_qemu_test_init' kernel/src/kernel.c || {
    echo "kernel.c не содержит entropy QEMU hook" >&2
    exit 1
}
grep -Fq 'scripts/test-qemu-entropy.sh' Makefile || {
    echo "Makefile не вызывает entropy QEMU harness" >&2
    exit 1
}
grep -Fq 'HFOC-ENTROPY-QEMU-SECRET' kernel/src/entropy_qemu.c || {
    echo "entropy_qemu.c не содержит serial-leak sentinel" >&2
    exit 1
}
for entropy_qemu_marker in \
        '[ENTROPY] proof init' \
        '[ENTROPY] missing device denial' \
        '[ENTROPY] injected rejection' \
        '[ENTROPY] reset recovery' \
        '[ENTROPY] proof OK'; do
    grep -Fq "$entropy_qemu_marker" kernel/src/entropy_qemu.c || {
        echo "entropy_qemu.c не содержит marker $entropy_qemu_marker" >&2
        exit 1
    }
done
if nm -g build/hfoc-kernel.elf | grep ' entropy_qemu_test_init'; then
    echo "Production kernel содержит entropy QEMU proof symbol" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[ENTROPY]'; then
    echo "Production kernel содержит entropy QEMU marker" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf 'HFOC-ENTROPY-QEMU-SECRET'; then
    echo "Production kernel содержит entropy serial-leak sentinel" >&2
    exit 1
fi
version=$(<VERSION)
production_image="build/hfoc-os-${version}.iso"
make iso
if bash scripts/hfoc-file-marker.sh "$production_image" '[ENTROPY]'; then
    echo "Production ISO содержит entropy QEMU marker" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh "$production_image" 'HFOC-ENTROPY-QEMU-SECRET'; then
    echo "Production ISO содержит entropy serial-leak sentinel" >&2
    exit 1
fi
grep -Fq -- '- [x] **260. Entropy QEMU proof' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan не помечает item 260 завершённым" >&2
    exit 1
}
for entropy_qemu_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'entropy[- ].*qemu|qemu[- ].*entropy' \
            "$entropy_qemu_doc"; then
        echo "$entropy_qemu_doc не документирует entropy QEMU proof" >&2
        exit 1
    fi
done

# Virtio-net queue composition contracts.
grep -Fq 'test-virtio-net-queue-composition-host' Makefile || {
    echo "Makefile не содержит test-virtio-net-queue-composition-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-virtio-net-queue-composition-host' Makefile || {
    echo "test-host-suite не вызывает test-virtio-net-queue-composition-host" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_QUEUE_COMPOSITION_VERSION' \
    kernel/include/virtio_net_queue_composition.h || {
    echo "virtio_net_queue_composition.h не содержит version" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_SIZE_MAX' \
    kernel/include/virtio_net_queue_composition.h || {
    echo "virtio_net_queue_composition.h не содержит queue size max" >&2
    exit 1
}
grep -Fq 'struct virtio_net_queue_config' \
    kernel/include/virtio_net_queue_composition.h || {
    echo "virtio_net_queue_composition.h не содержит queue config" >&2
    exit 1
}
grep -Fq 'struct virtio_net_queue_composition' \
    kernel/include/virtio_net_queue_composition.h || {
    echo "virtio_net_queue_composition.h не содержит composition" >&2
    exit 1
}
for virtio_net_queue_symbol in virtio_net_queue_composition_activate \
                               virtio_net_queue_composition_teardown \
                               virtio_net_queue_composition_published \
                               virtio_net_queue_composition_generation \
                               virtio_net_queue_composition_receive \
                               virtio_net_queue_composition_transmit \
                               virtio_net_queue_composition_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $virtio_net_queue_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $virtio_net_queue_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/virtio_net_queue_composition.c; then
    echo "virtio_net_queue_composition.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'virtio_net_queue_composition_(activate|teardown)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Virtio-net queue composition вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'virtio_net_queue_composition_host_test_'; then
    echo "virtio_net_queue_composition_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Virtio net queue composition host tests: ALL OK'; then
    echo "Production kernel содержит virtio net queue composition host marker" >&2
    exit 1
fi
for virtio_net_queue_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'virtio[- ].*net[- ].*queue[- ].*composition' \
            "$virtio_net_queue_doc"; then
        echo "$virtio_net_queue_doc не документирует virtio-net queue composition" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **212. Virtio-net queue composition' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan не помечает item 212 завершённым" >&2
    exit 1
}

# Virtio-net RX buffer posting contracts.
grep -Fq 'test-virtio-net-rx-host' Makefile || {
    echo "Makefile не содержит test-virtio-net-rx-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-virtio-net-rx-host' Makefile || {
    echo "test-host-suite не вызывает test-virtio-net-rx-host" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_RX_VERSION' \
    kernel/include/virtio_net_rx.h || {
    echo "virtio_net_rx.h не содержит version" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_RX_CAPACITY_MAX' \
    kernel/include/virtio_net_rx.h || {
    echo "virtio_net_rx.h не содержит capacity max" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_RX_MINIMUM_FRAME_CAPACITY_MIN' \
    kernel/include/virtio_net_rx.h || {
    echo "virtio_net_rx.h не содержит minimum frame capacity" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_RX_DMA_DIRECTION' \
    kernel/include/virtio_net_rx.h || {
    echo "virtio_net_rx.h не содержит DMA direction" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_RX_ETHERNET_FRAME_MAX' \
    kernel/include/virtio_net_rx.h || {
    echo "virtio_net_rx.h не содержит Ethernet frame max" >&2
    exit 1
}
grep -Fq 'struct virtio_net_rx_config' \
    kernel/include/virtio_net_rx.h || {
    echo "virtio_net_rx.h не содержит config" >&2
    exit 1
}
grep -Fq 'struct virtio_net_rx_buffer' \
    kernel/include/virtio_net_rx.h || {
    echo "virtio_net_rx.h не содержит buffer" >&2
    exit 1
}
grep -Fq 'struct virtio_net_rx' \
    kernel/include/virtio_net_rx.h || {
    echo "virtio_net_rx.h не содержит controller" >&2
    exit 1
}
grep -Fq 'struct virtio_net_rx_frame' \
    kernel/include/virtio_net_rx.h || {
    echo "virtio_net_rx.h не содержит frame" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_RX_BUFFER_ALIAS' \
    kernel/include/virtio_net_rx.h || {
    echo "virtio_net_rx.h не содержит buffer alias status" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_RX_USED_EMPTY' \
    kernel/include/virtio_net_rx.h || {
    echo "virtio_net_rx.h не содержит used empty status" >&2
    exit 1
}
for virtio_net_rx_symbol in virtio_net_rx_activate \
                            virtio_net_rx_teardown \
                            virtio_net_rx_complete \
                            virtio_net_rx_published \
                            virtio_net_rx_generation \
                            virtio_net_rx_capacity \
                            virtio_net_rx_posted_count \
                            virtio_net_rx_buffer_at \
                            virtio_net_rx_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $virtio_net_rx_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $virtio_net_rx_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/virtio_net_rx.c; then
    echo "virtio_net_rx.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'virtio_net_rx_(activate|teardown|complete)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Virtio-net RX buffer posting/completion вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'virtio_net_rx_host_test_'; then
    echo "virtio_net_rx_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Virtio net RX host tests: ALL OK'; then
    echo "Production kernel содержит virtio net RX host marker" >&2
    exit 1
fi
for virtio_net_rx_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'virtio[- ].*net[- ].*RX|virtio net RX buffer posting|virtio net RX completion' \
            "$virtio_net_rx_doc"; then
        echo "$virtio_net_rx_doc не документирует virtio-net RX buffer posting" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **213. Virtio-net RX buffer posting' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan не помечает item 213 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **214. Virtio-net RX completion' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan не помечает item 214 завершённым" >&2
    exit 1
}
