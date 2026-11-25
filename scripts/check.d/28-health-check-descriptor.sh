#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Health-check descriptor contracts.
grep -Fq 'test-health-check-host' Makefile || {
    echo "Makefile не содержит test-health-check-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-health-check-host' Makefile || {
    echo "test: target не содержит test-health-check-host" >&2
    exit 1
}
grep -Fq 'HEALTH_CHECK_VERSION' \
    kernel/include/health_check.h || {
    echo "health_check.h не содержит HEALTH_CHECK_VERSION" >&2
    exit 1
}
grep -Fq 'struct health_check_store' \
    kernel/include/health_check.h || {
    echo "health_check.h не содержит struct health_check_store" >&2
    exit 1
}
grep -Fq 'health_check_publish' \
    kernel/include/health_check.h || {
    echo "health_check.h не содержит health_check_publish" >&2
    exit 1
}
for health_check_symbol in health_check_init \
                           health_check_publish \
                           health_check_result \
                           health_check_lookup \
                           health_check_snapshot \
                           health_check_status_string \
                           health_check_severity_string \
                           health_check_result_string; do
    nm build/hfoc-kernel.elf | grep -F " $health_check_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $health_check_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/health_check.c; then
    echo "health_check.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'health_check_host_test_'; then
    echo "health_check_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Health-check host tests: ALL OK'; then
    echo "Production kernel содержит health-check host marker" >&2
    exit 1
fi
for health_check_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'health-check descriptor|health check descriptor' \
            "$health_check_doc"; then
        echo "$health_check_doc не документирует health-check descriptor" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **161. Health-check descriptor' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 161 завершённым" >&2
    exit 1
}

# Storage health check contracts.
grep -Fq 'test-storage-health-host' Makefile || {
    echo "Makefile не содержит test-storage-health-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-storage-health-host' Makefile || {
    echo "test: target не содержит test-storage-health-host" >&2
    exit 1
}
grep -Fq 'STORAGE_HEALTH_VERSION' \
    kernel/include/storage_health.h || {
    echo "storage_health.h не содержит STORAGE_HEALTH_VERSION" >&2
    exit 1
}
grep -Fq 'struct storage_health_input' \
    kernel/include/storage_health.h || {
    echo "storage_health.h не содержит struct storage_health_input" >&2
    exit 1
}
grep -Fq 'storage_health_evaluate' \
    kernel/include/storage_health.h || {
    echo "storage_health.h не содержит storage_health_evaluate" >&2
    exit 1
}
for storage_health_symbol in storage_health_evaluate \
                             storage_health_status_string \
                             storage_health_fault_string; do
    nm build/hfoc-kernel.elf | grep -F " $storage_health_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $storage_health_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/storage_health.c; then
    echo "storage_health.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'storage_health_host_test_'; then
    echo "storage_health_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Storage health host tests: ALL OK'; then
    echo "Production kernel содержит storage health host marker" >&2
    exit 1
fi
for storage_health_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'storage health check' \
            "$storage_health_doc"; then
        echo "$storage_health_doc не документирует storage health check" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **162. Storage health check' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 162 завершённым" >&2
    exit 1
}

# Service health aggregation contracts.
grep -Fq 'test-service-health-host' Makefile || {
    echo "Makefile не содержит test-service-health-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-service-health-host' Makefile || {
    echo "test: target не содержит test-service-health-host" >&2
    exit 1
}
grep -Fq 'SERVICE_HEALTH_VERSION' \
    kernel/include/service_health.h || {
    echo "service_health.h не содержит SERVICE_HEALTH_VERSION" >&2
    exit 1
}
grep -Fq 'struct service_health_input' \
    kernel/include/service_health.h || {
    echo "service_health.h не содержит struct service_health_input" >&2
    exit 1
}
grep -Fq 'service_health_evaluate' \
    kernel/include/service_health.h || {
    echo "service_health.h не содержит service_health_evaluate" >&2
    exit 1
}
for service_health_symbol in service_health_evaluate \
                             service_health_status_string \
                             service_health_fault_string; do
    nm build/hfoc-kernel.elf | grep -F " $service_health_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $service_health_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/service_health.c; then
    echo "service_health.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'service_health_host_test_'; then
    echo "service_health_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Service health host tests: ALL OK'; then
    echo "Production kernel содержит service health host marker" >&2
    exit 1
fi
for service_health_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'service health aggregation' \
            "$service_health_doc"; then
        echo "$service_health_doc не документирует service health aggregation" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **163. Service health aggregation' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 163 завершённым" >&2
    exit 1
}

# Network health aggregation contracts.
grep -Fq 'test-network-health-host' Makefile || {
    echo "Makefile не содержит test-network-health-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-network-health-host' Makefile || {
    echo "test: target не содержит test-network-health-host" >&2
    exit 1
}
grep -Fq 'NETWORK_HEALTH_VERSION' \
    kernel/include/network_health.h || {
    echo "network_health.h не содержит NETWORK_HEALTH_VERSION" >&2
    exit 1
}
grep -Fq 'struct network_health_input' \
    kernel/include/network_health.h || {
    echo "network_health.h не содержит struct network_health_input" >&2
    exit 1
}
grep -Fq 'network_health_evaluate' \
    kernel/include/network_health.h || {
    echo "network_health.h не содержит network_health_evaluate" >&2
    exit 1
}
for network_health_symbol in network_health_evaluate \
                             network_health_status_string \
                             network_health_fault_string; do
    nm build/hfoc-kernel.elf | grep -F " $network_health_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $network_health_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/network_health.c; then
    echo "network_health.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'network_health_host_test_'; then
    echo "network_health_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Network health host tests: ALL OK'; then
    echo "Production kernel содержит network health host marker" >&2
    exit 1
fi
for network_health_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'network health aggregation' \
            "$network_health_doc"; then
        echo "$network_health_doc не документирует network health aggregation" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **164. Network health aggregation' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 164 завершённым" >&2
    exit 1
}

# Audit-ring pressure health contracts.
grep -Fq 'test-audit-ring-health-host' Makefile || {
    echo "Makefile не содержит test-audit-ring-health-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-audit-ring-health-host' Makefile || {
    echo "test: target не содержит test-audit-ring-health-host" >&2
    exit 1
}
grep -Fq 'AUDIT_RING_HEALTH_VERSION' \
    kernel/include/audit_ring_health.h || {
    echo "audit_ring_health.h не содержит AUDIT_RING_HEALTH_VERSION" >&2
    exit 1
}
grep -Fq 'struct audit_ring_health_input' \
    kernel/include/audit_ring_health.h || {
    echo "audit_ring_health.h не содержит struct audit_ring_health_input" >&2
    exit 1
}
grep -Fq 'audit_ring_health_evaluate' \
    kernel/include/audit_ring_health.h || {
    echo "audit_ring_health.h не содержит audit_ring_health_evaluate" >&2
    exit 1
}
for audit_ring_health_symbol in audit_ring_health_evaluate \
                                audit_ring_health_status_string \
                                audit_ring_health_fault_string; do
    nm build/hfoc-kernel.elf | grep -F " $audit_ring_health_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $audit_ring_health_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/audit_ring_health.c; then
    echo "audit_ring_health.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'audit_ring_health_host_test_'; then
    echo "audit_ring_health_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Audit-ring health host tests: ALL OK'; then
    echo "Production kernel содержит audit-ring health host marker" >&2
    exit 1
fi
for audit_ring_health_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'audit-ring pressure health' \
            "$audit_ring_health_doc"; then
        echo "$audit_ring_health_doc не документирует audit-ring pressure health" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **165. Audit-ring pressure health' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 165 завершённым" >&2
    exit 1
}

# Versioned metrics snapshot contracts.
grep -Fq 'test-metrics-host' Makefile || {
    echo "Makefile не содержит test-metrics-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-metrics-host' Makefile || {
    echo "test: target не содержит test-metrics-host" >&2
    exit 1
}
grep -Fq 'METRICS_VERSION' \
    kernel/include/metrics.h || {
    echo "metrics.h не содержит METRICS_VERSION" >&2
    exit 1
}
grep -Fq 'METRICS_SCHEMA_ID' \
    kernel/include/metrics.h || {
    echo "metrics.h не содержит METRICS_SCHEMA_ID" >&2
    exit 1
}
grep -Fq 'struct metrics_store' \
    kernel/include/metrics.h || {
    echo "metrics.h не содержит struct metrics_store" >&2
    exit 1
}
grep -Fq 'metrics_begin_update' \
    kernel/include/metrics.h || {
    echo "metrics.h не содержит metrics_begin_update" >&2
    exit 1
}
for metrics_symbol in metrics_init metrics_begin_update \
                      metrics_set_health metrics_set_state \
                      metrics_set_service metrics_set_update \
                      metrics_commit metrics_rollback \
                      metrics_snapshot metrics_status_string \
                      metrics_schema_id; do
    nm build/hfoc-kernel.elf | grep -F " $metrics_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $metrics_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/metrics.c; then
    echo "metrics.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'metrics_host_test_'; then
    echo "metrics_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Metrics host tests: ALL OK'; then
    echo "Production kernel содержит metrics host marker" >&2
    exit 1
fi
for metrics_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'versioned metrics snapshot' \
            "$metrics_doc"; then
        echo "$metrics_doc не документирует versioned metrics snapshot" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **166. Versioned metrics snapshot' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 166 завершённым" >&2
    exit 1
}

# Support bundle manifest contracts.
grep -Fq 'test-support-bundle-manifest-host' Makefile || {
    echo "Makefile не содержит test-support-bundle-manifest-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-support-bundle-manifest-host' Makefile || {
    echo "test: target не содержит test-support-bundle-manifest-host" >&2
    exit 1
}
grep -Fq 'SUPPORT_BUNDLE_MANIFEST_VERSION' \
    kernel/include/support_bundle_manifest.h || {
    echo "support_bundle_manifest.h не содержит SUPPORT_BUNDLE_MANIFEST_VERSION" >&2
    exit 1
}
grep -Fq 'SUPPORT_BUNDLE_MANIFEST_SCHEMA_ID' \
    kernel/include/support_bundle_manifest.h || {
    echo "support_bundle_manifest.h не содержит SUPPORT_BUNDLE_MANIFEST_SCHEMA_ID" >&2
    exit 1
}
grep -Fq 'struct support_bundle_manifest_source' \
    kernel/include/support_bundle_manifest.h || {
    echo "support_bundle_manifest.h не содержит struct support_bundle_manifest_source" >&2
    exit 1
}
grep -Fq 'support_bundle_manifest_export' \
    kernel/include/support_bundle_manifest.h || {
    echo "support_bundle_manifest.h не содержит support_bundle_manifest_export" >&2
    exit 1
}
for support_bundle_symbol in support_bundle_manifest_export \
                             support_bundle_manifest_status_string \
                             support_bundle_manifest_entry_type_string \
                             support_bundle_manifest_schema_id; do
    nm build/hfoc-kernel.elf | grep -F " $support_bundle_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $support_bundle_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_push|event_ring_pop|event_ring_snapshot|audit_snapshot|audit_record_append|audit_record_pop)\b' \
        kernel/src/support_bundle_manifest.c; then
    echo "support_bundle_manifest.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'support_bundle_manifest_host_test_'; then
    echo "support_bundle_manifest_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Support bundle manifest host tests: ALL OK'; then
    echo "Production kernel содержит support bundle manifest host marker" >&2
    exit 1
fi
for support_bundle_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'support-bundle manifest' \
            "$support_bundle_doc"; then
        echo "$support_bundle_doc не документирует support-bundle manifest" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **167. Support-bundle manifest' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 167 завершённым" >&2
    exit 1
}

# Support bundle exporter contracts.
grep -Fq 'test-support-bundle-exporter-host' Makefile || {
    echo "Makefile не содержит test-support-bundle-exporter-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-support-bundle-exporter-host' Makefile || {
    echo "test: target не содержит test-support-bundle-exporter-host" >&2
    exit 1
}
grep -Fq 'SUPPORT_BUNDLE_EXPORTER_VERSION' \
    kernel/include/support_bundle_exporter.h || {
    echo "support_bundle_exporter.h не содержит SUPPORT_BUNDLE_EXPORTER_VERSION" >&2
    exit 1
}
grep -Fq 'SUPPORT_BUNDLE_EXPORTER_SCHEMA_ID' \
    kernel/include/support_bundle_exporter.h || {
    echo "support_bundle_exporter.h не содержит SUPPORT_BUNDLE_EXPORTER_SCHEMA_ID" >&2
    exit 1
}
grep -Fq 'struct support_bundle_exporter' \
    kernel/include/support_bundle_exporter.h || {
    echo "support_bundle_exporter.h не содержит struct support_bundle_exporter" >&2
    exit 1
}
grep -Fq 'support_bundle_exporter_init' \
    kernel/include/support_bundle_exporter.h || {
    echo "support_bundle_exporter.h не содержит support_bundle_exporter_init" >&2
    exit 1
}
for support_bundle_exporter_symbol in support_bundle_exporter_init \
                                      support_bundle_exporter_next \
                                      support_bundle_exporter_close \
                                      support_bundle_exporter_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $support_bundle_exporter_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $support_bundle_exporter_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/support_bundle_exporter.c; then
    echo "support_bundle_exporter.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'support_bundle_exporter_host_test_'; then
    echo "support_bundle_exporter_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Support bundle exporter host tests: ALL OK'; then
    echo "Production kernel содержит support bundle exporter host marker" >&2
    exit 1
fi
for support_bundle_exporter_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'support-bundle exporter' \
            "$support_bundle_exporter_doc"; then
        echo "$support_bundle_exporter_doc не документирует support-bundle exporter" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **168. Support-bundle exporter' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 168 завершённым" >&2
    exit 1
}

# Diagnostic privacy policy contracts.
grep -Fq 'test-diagnostic-privacy-host' Makefile || {
    echo "Makefile не содержит test-diagnostic-privacy-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-diagnostic-privacy-host' Makefile || {
    echo "test: target не содержит test-diagnostic-privacy-host" >&2
    exit 1
}
grep -Fq 'DIAGNOSTIC_PRIVACY_VERSION' \
    kernel/include/diagnostic_privacy.h || {
    echo "diagnostic_privacy.h не содержит DIAGNOSTIC_PRIVACY_VERSION" >&2
    exit 1
}
grep -Fq 'DIAGNOSTIC_PRIVACY_POLICY_ID' \
    kernel/include/diagnostic_privacy.h || {
    echo "diagnostic_privacy.h не содержит DIAGNOSTIC_PRIVACY_POLICY_ID" >&2
    exit 1
}
grep -Fq 'diagnostic_privacy_classify_key' \
    kernel/include/diagnostic_privacy.h || {
    echo "diagnostic_privacy.h не содержит diagnostic_privacy_classify_key" >&2
    exit 1
}
grep -Fq 'diagnostic_privacy_redact' \
    kernel/include/diagnostic_privacy.h || {
    echo "diagnostic_privacy.h не содержит diagnostic_privacy_redact" >&2
    exit 1
}
for diagnostic_privacy_symbol in diagnostic_privacy_classify_key \
                                 diagnostic_privacy_classify_verifier \
                                 diagnostic_privacy_classify_capability \
                                 diagnostic_privacy_classify_address \
                                 diagnostic_privacy_redact \
                                 diagnostic_privacy_category_string \
                                 diagnostic_privacy_status_string \
                                 diagnostic_privacy_version \
                                 diagnostic_privacy_policy_id; do
    nm build/hfoc-kernel.elf | grep -F " $diagnostic_privacy_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $diagnostic_privacy_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/diagnostic_privacy.c; then
    echo "diagnostic_privacy.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if ! grep -Fq 'diagnostic_privacy_classify_key' \
        kernel/src/support_bundle_manifest.c ||
        ! grep -Fq 'diagnostic_privacy_classify_key' \
            kernel/src/support_bundle_exporter.c; then
    echo "support-bundle manifest/exporter не используют shared diagnostic privacy policy" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'diagnostic_privacy_host_test_'; then
    echo "diagnostic_privacy_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Diagnostic privacy host tests: ALL OK'; then
    echo "Production kernel содержит diagnostic privacy host marker" >&2
    exit 1
fi
for diagnostic_privacy_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'diagnostic privacy' \
            "$diagnostic_privacy_doc"; then
        echo "$diagnostic_privacy_doc не документирует diagnostic privacy policy" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **169. Diagnostic privacy policy' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 169 завершённым" >&2
    exit 1
}
