#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Dependency graph snapshot contracts.
grep -Fq 'test-service-graph-host' Makefile || {
    echo "Makefile не содержит test-service-graph-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-service-graph-host' Makefile || {
    echo "test: target не содержит test-service-graph-host" >&2
    exit 1
}
grep -Fq 'SERVICE_GRAPH_SNAPSHOT_VERSION' kernel/include/service.h || {
    echo "service.h не содержит SERVICE_GRAPH_SNAPSHOT_VERSION" >&2
    exit 1
}
grep -Fq 'struct service_graph_snapshot' kernel/include/service.h || {
    echo "service.h не содержит struct service_graph_snapshot" >&2
    exit 1
}
grep -Fq 'service_graph_snapshot_build' kernel/include/service.h || {
    echo "service.h не содержит service_graph_snapshot_build" >&2
    exit 1
}
for graph_symbol in service_graph_snapshot_build \
                    service_graph_snapshot_count \
                    service_graph_snapshot_descriptor \
                    service_graph_snapshot_start_id \
                    service_graph_snapshot_published \
                    service_graph_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $graph_symbol" >/dev/null || {
        echo "Production kernel не содержит $graph_symbol" >&2
        exit 1
    }
done
if nm -g build/hfoc-kernel.elf | grep 'service_graph_host_test_'; then
    echo "service_graph_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Service graph host tests: ALL OK'; then
    echo "Production kernel содержит service graph host marker" >&2
    exit 1
fi
for graph_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'dependency graph snapshot|topological.*snapshot' \
            "$graph_doc"; then
        echo "$graph_doc не документирует dependency graph snapshot" >&2
        exit 1
    fi
done

# Restart policy descriptor contracts.
grep -Fq 'test-service-restart-policy-host' Makefile || {
    echo "Makefile не содержит test-service-restart-policy-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-service-restart-policy-host' Makefile || {
    echo "test: target не содержит test-service-restart-policy-host" >&2
    exit 1
}
grep -Fq 'SERVICE_RESTART_POLICY_VERSION' \
    kernel/include/service_restart_policy.h || {
    echo "service_restart_policy.h не содержит SERVICE_RESTART_POLICY_VERSION" >&2
    exit 1
}
grep -Fq 'SERVICE_RESTART_BUDGET_MAX' \
    kernel/include/service_restart_policy.h || {
    echo "service_restart_policy.h не содержит SERVICE_RESTART_BUDGET_MAX" >&2
    exit 1
}
grep -Fq 'SERVICE_RESTART_INTERVAL_MAX_TICKS' \
    kernel/include/service_restart_policy.h || {
    echo "service_restart_policy.h не содержит restart interval bound" >&2
    exit 1
}
grep -Fq 'struct service_restart_policy' \
    kernel/include/service_restart_policy.h || {
    echo "service_restart_policy.h не содержит struct service_restart_policy" >&2
    exit 1
}
for restart_symbol in service_restart_policy_validate \
                      service_restart_policy_init \
                      service_restart_policy_can_restart \
                      service_restart_policy_record_restart \
                      service_restart_policy_reset \
                      service_restart_policy_restart_count \
                      service_restart_policy_state \
                      service_restart_policy_status_string \
                      service_restart_policy_state_string; do
    nm build/hfoc-kernel.elf | grep -F " $restart_symbol" >/dev/null || {
        echo "Production kernel не содержит $restart_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b' \
        kernel/src/service_restart_policy.c; then
    echo "service_restart_policy.c выполняет запрещённое выделение" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Restart policy host tests: ALL OK'; then
    echo "Production kernel содержит restart policy host marker" >&2
    exit 1
fi
for restart_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'restart policy descriptor|restart.*policy' \
            "$restart_doc"; then
        echo "$restart_doc не документирует restart policy descriptor" >&2
        exit 1
    fi
done

for service_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'service descriptor|validated service descriptor' \
            "$service_doc"; then
        echo "$service_doc не документирует validated service descriptors" >&2
        exit 1
    fi
done
for resource_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'service resource envelope|resource envelope' \
            "$resource_doc"; then
        echo "$resource_doc не документирует service resource envelope" >&2
        exit 1
    fi
done

# BSP service supervisor lifecycle contracts.
grep -Fq 'test-service-supervisor-host' Makefile || {
    echo "Makefile не содержит test-service-supervisor-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-service-supervisor-host' Makefile || {
    echo "test: target не содержит test-service-supervisor-host" >&2
    exit 1
}
grep -Fq 'SERVICE_SUPERVISOR_CAPACITY' kernel/include/service_supervisor.h || {
    echo "service_supervisor.h не содержит SERVICE_SUPERVISOR_CAPACITY" >&2
    exit 1
}
grep -Fq 'SERVICE_SUPERVISOR_ENTRY_MAX' \
    kernel/include/service_supervisor.h || {
    echo "service_supervisor.h не содержит SERVICE_SUPERVISOR_ENTRY_MAX" >&2
    exit 1
}
grep -Fq 'struct service_supervisor' kernel/include/service_supervisor.h || {
    echo "service_supervisor.h не содержит struct service_supervisor" >&2
    exit 1
}
grep -Fq 'struct service_supervisor_entry' \
    kernel/include/service_supervisor.h || {
    echo "service_supervisor.h не содержит struct service_supervisor_entry" >&2
    exit 1
}
grep -Fq 'service_supervisor_init' kernel/include/service_supervisor.h || {
    echo "service_supervisor.h не содержит service_supervisor_init" >&2
    exit 1
}
if ! grep -Fq 'service_supervisor_start_ordered' \
        kernel/include/service_supervisor.h ||
   ! grep -Fq 'service_supervisor_start_ordered' \
        kernel/src/service_supervisor.c; then
    echo "ordered service start не объявлен в header и не реализован" >&2
    exit 1
fi
grep -Fq 'service_supervisor_stop_ordered' \
    kernel/include/service_supervisor.h || {
    echo "service_supervisor.h не содержит service_supervisor_stop_ordered" >&2
    exit 1
}
grep -Fq 'service_supervisor_stop_ordered' \
    kernel/src/service_supervisor.c || {
    echo "service_supervisor.c не реализует service_supervisor_stop_ordered" >&2
    exit 1
}
grep -Fq 'service_supervisor_stop_slot' \
    kernel/src/service_supervisor.c || {
    echo "service_supervisor.c не содержит service_supervisor_stop_slot" >&2
    exit 1
}
for supervisor_symbol in service_supervisor_init service_supervisor_start \
                        service_supervisor_start_ordered \
                        service_supervisor_stop_ordered \
                        service_supervisor_configure_restart_policy \
                        service_supervisor_reap_contained \
                        service_supervisor_restart_contained \
                        service_supervisor_exit service_supervisor_reap \
                        service_supervisor_restart_count \
                        service_supervisor_state \
                        service_supervisor_report_ready \
                        service_supervisor_report_liveness \
                        service_supervisor_check_liveness \
                        service_supervisor_set_readiness_deadline \
                        service_supervisor_check_readiness \
                        service_supervisor_reap_at \
                        service_supervisor_health_state \
                        service_supervisor_liveness_generation \
                        service_supervisor_readiness_deadline_tick \
                        service_supervisor_snapshot \
                        service_supervisor_snapshot_published \
                        service_supervisor_snapshot_count \
                        service_supervisor_snapshot_entry \
                        service_supervisor_status_string \
                        service_supervisor_state_string \
                        service_supervisor_health_state_string; do
    nm build/hfoc-kernel.elf | grep -F " $supervisor_symbol" >/dev/null || {
        echo "Production kernel не содержит $supervisor_symbol" >&2
        exit 1
    }
done
grep -Fq 'SERVICE_HEALTH_PENDING' kernel/include/service_supervisor.h || {
    echo "service_supervisor.h не содержит SERVICE_HEALTH_PENDING" >&2
    exit 1
}
grep -Fq 'SERVICE_HEALTH_READY' kernel/include/service_supervisor.h || {
    echo "service_supervisor.h не содержит SERVICE_HEALTH_READY" >&2
    exit 1
}
grep -Fq 'SERVICE_HEALTH_LIVENESS_FAILED' \
    kernel/include/service_supervisor.h || {
    echo "service_supervisor.h не содержит SERVICE_HEALTH_LIVENESS_FAILED" >&2
    exit 1
}
grep -Fq 'SERVICE_HEALTH_READINESS_TIMEOUT' \
    kernel/include/service_supervisor.h || {
    echo "service_supervisor.h не содержит SERVICE_HEALTH_READINESS_TIMEOUT" >&2
    exit 1
}
grep -Fq 'SERVICE_LIVENESS_TIMEOUT_MAX_TICKS' \
    kernel/include/service_supervisor.h || {
    echo "service_supervisor.h не содержит SERVICE_LIVENESS_TIMEOUT_MAX_TICKS" >&2
    exit 1
}
grep -Fq 'SERVICE_READINESS_TIMEOUT_MAX_TICKS' \
    kernel/include/service_supervisor.h || {
    echo "service_supervisor.h не содержит SERVICE_READINESS_TIMEOUT_MAX_TICKS" >&2
    exit 1
}
grep -Fq 'SERVICE_SUPERVISOR_SNAPSHOT_VERSION' \
    kernel/include/service_supervisor.h || {
    echo "service_supervisor.h не содержит snapshot version" >&2
    exit 1
}
grep -Fq 'struct service_supervisor_snapshot' \
    kernel/include/service_supervisor.h || {
    echo "service_supervisor.h не содержит service state snapshot" >&2
    exit 1
}
grep -Fq 'SERVICE_SUPERVISOR_RESTART_INTERVAL_DEFAULT_TICKS' \
    kernel/include/service_supervisor.h || {
    echo "service_supervisor.h не содержит restart interval default" >&2
    exit 1
}
grep -Fq 'SERVICE_SUPERVISOR_DEPENDENCY_BLOCKED' \
    kernel/include/service_supervisor.h || {
    echo "service_supervisor.h не содержит dependency blocked status" >&2
    exit 1
}
grep -Fq 'SERVICE_SUPERVISOR_READINESS_TIMEOUT' \
    kernel/include/service_supervisor.h || {
    echo "service_supervisor.h не содержит readiness timeout status" >&2
    exit 1
}
grep -Fq 'SERVICE_SUPERVISOR_BAD_READINESS' \
    kernel/include/service_supervisor.h || {
    echo "service_supervisor.h не содержит bad readiness status" >&2
    exit 1
}
grep -Fq 'SERVICE_SUPERVISOR_READINESS_OVERFLOW' \
    kernel/include/service_supervisor.h || {
    echo "service_supervisor.h не содержит readiness overflow status" >&2
    exit 1
}
grep -Fq 'SERVICE_SUPERVISOR_SNAPSHOT_INVALID' \
    kernel/include/service_supervisor.h || {
    echo "service_supervisor.h не содержит snapshot invalid status" >&2
    exit 1
}
grep -Fq 'service_supervisor_health_reset' \
    kernel/src/service_supervisor.c || {
    echo "service_supervisor.c не сбрасывает readiness/liveness state" >&2
    exit 1
}
grep -Fq 'service_supervisor_start_slot_at' \
    kernel/src/service_supervisor.c || {
    echo "service_supervisor.c не перевооружает readiness deadline" >&2
    exit 1
}
grep -Fq 'service_supervisor_snapshot_sort_entries' \
    kernel/src/service_supervisor.c || {
    echo "service_supervisor.c не сортирует service state snapshot" >&2
    exit 1
}
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages|vmm_create_address_space|vmm_destroy_address_space)\b' \
        kernel/src/service_supervisor.c; then
    echo "service_supervisor.c выполняет запрещённое прямое выделение" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Service supervisor host tests: ALL OK'; then
    echo "Production kernel содержит service supervisor host marker" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'service_supervisor_host_test_'; then
    echo "Service supervisor host symbols попали в kernel ELF" >&2
    exit 1
fi
grep -Fq 'test-service-supervisor-qemu' Makefile || {
    echo "Makefile не содержит test-service-supervisor-qemu" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-service-supervisor-qemu' Makefile || {
    echo "test: target не содержит test-service-supervisor-qemu" >&2
    exit 1
}
grep -Fq 'scripts/test-qemu-supervisor-composition.sh' Makefile || {
    echo "Makefile не вызывает supervisor composition QEMU script" >&2
    exit 1
}
grep -Fq 'HFOC_SERVICE_SUPERVISOR_QEMU_TEST' \
    kernel/src/service_supervisor_qemu.c || {
    echo "service_supervisor_qemu.c не имеет proof guard" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[SUPERVISOR]'; then
    echo "Production kernel содержит supervisor composition marker" >&2
    exit 1
fi
for supervisor_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'service supervisor|supervisor lifecycle' \
            "$supervisor_doc"; then
        echo "$supervisor_doc не документирует BSP service supervisor" >&2
        exit 1
    fi
    if ! grep -Eiq 'readiness.*liveness|liveness.*readiness' \
            "$supervisor_doc"; then
        echo "$supervisor_doc не документирует readiness/liveness" >&2
        exit 1
    fi
    if ! grep -Eiq 'readiness deadline|readiness.*deadline' \
            "$supervisor_doc"; then
        echo "$supervisor_doc не документирует readiness deadline" >&2
        exit 1
    fi
    if ! grep -Eiq 'status snapshot|state snapshot|service snapshot' \
            "$supervisor_doc"; then
        echo "$supervisor_doc не документирует service state snapshot" >&2
        exit 1
    fi
    if ! grep -Eiq 'ordered service start|start.*dependency order' \
            "$supervisor_doc"; then
        echo "$supervisor_doc не документирует ordered service start" >&2
        exit 1
    fi
    if ! grep -Eiq 'ordered service stop|reverse dependency order' \
            "$supervisor_doc"; then
        echo "$supervisor_doc не документирует ordered service stop" >&2
        exit 1
    fi
    if ! grep -Eiq 'restart containment|contained restart' \
            "$supervisor_doc"; then
        echo "$supervisor_doc не документирует restart containment" >&2
        exit 1
    fi
    if ! grep -Eiq 'supervisor composition|composition QEMU' \
            "$supervisor_doc"; then
        echo "$supervisor_doc не документирует supervisor composition QEMU" >&2
        exit 1
    fi
done

# Credential verifier contract contracts.
grep -Fq 'test-credential-verifier-host' Makefile || {
    echo "Makefile не содержит test-credential-verifier-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-credential-verifier-host' Makefile || {
    echo "test: target не содержит test-credential-verifier-host" >&2
    exit 1
}
grep -Fq 'CREDENTIAL_VERIFIER_VERSION' \
    kernel/include/credential_verifier.h || {
    echo "credential_verifier.h не содержит CREDENTIAL_VERIFIER_VERSION" >&2
    exit 1
}
grep -Fq 'CREDENTIAL_VERIFIER_SECRET_LENGTH' \
    kernel/include/credential_verifier.h || {
    echo "credential_verifier.h не содержит CREDENTIAL_VERIFIER_SECRET_LENGTH" >&2
    exit 1
}
grep -Fq 'struct credential_verifier' \
    kernel/include/credential_verifier.h || {
    echo "credential_verifier.h не содержит struct credential_verifier" >&2
    exit 1
}
for verifier_symbol in credential_verifier_validate_verifier \
                       credential_verifier_validate_input \
                       credential_verifier_verify \
                       credential_verifier_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $verifier_symbol" >/dev/null || {
        echo "Production kernel не содержит $verifier_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b' \
        kernel/src/credential_verifier.c; then
    echo "credential_verifier.c выполняет запрещённое выделение или диагностику" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'credential_verifier_host_test_'; then
    echo "credential_verifier_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Credential verifier host tests: ALL OK'; then
    echo "Production kernel содержит credential verifier host marker" >&2
    exit 1
fi
for verifier_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'credential verifier|credential-verifier' \
            "$verifier_doc"; then
        echo "$verifier_doc не документирует credential verifier contract" >&2
        exit 1
    fi
done

# Local principal descriptor contracts.
grep -Fq 'test-local-principal-host' Makefile || {
    echo "Makefile не содержит test-local-principal-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-local-principal-host' Makefile || {
    echo "test: target не содержит test-local-principal-host" >&2
    exit 1
}
grep -Fq 'LOCAL_PRINCIPAL_DESCRIPTOR_VERSION' \
    kernel/include/local_principal.h || {
    echo "local_principal.h не содержит LOCAL_PRINCIPAL_DESCRIPTOR_VERSION" >&2
    exit 1
}
grep -Fq 'LOCAL_PRINCIPAL_ROLE_MAX' \
    kernel/include/local_principal.h || {
    echo "local_principal.h не содержит LOCAL_PRINCIPAL_ROLE_MAX" >&2
    exit 1
}
grep -Fq 'LOCAL_PRINCIPAL_SET_MAX' \
    kernel/include/local_principal.h || {
    echo "local_principal.h не содержит LOCAL_PRINCIPAL_SET_MAX" >&2
    exit 1
}
grep -Fq 'struct local_principal' \
    kernel/include/local_principal.h || {
    echo "local_principal.h не содержит struct local_principal" >&2
    exit 1
}
for principal_symbol in local_principal_validate \
                       local_principal_init \
                       local_principal_status_string \
                       local_principal_set_validate \
                       local_principal_set_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $principal_symbol" >/dev/null || {
        echo "Production kernel не содержит $principal_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b' \
        kernel/src/local_principal.c; then
    echo "local_principal.c выполняет запрещённое выделение" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'local_principal_host_test_'; then
    echo "local_principal_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Local principal host tests: ALL OK'; then
    echo "Production kernel содержит local principal host marker" >&2
    exit 1
fi
for principal_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'local principal|principal descriptor' \
            "$principal_doc"; then
        echo "$principal_doc не документирует local principal descriptors" >&2
        exit 1
    fi
done

# Trust-anchor descriptor contracts.
grep -Fq 'test-trust-anchor-host' Makefile || {
    echo "Makefile не содержит test-trust-anchor-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-trust-anchor-host' Makefile || {
    echo "test: target не содержит test-trust-anchor-host" >&2
    exit 1
}
grep -Fq 'TRUST_ANCHOR_DESCRIPTOR_VERSION' \
    kernel/include/trust_anchor.h || {
    echo "trust_anchor.h не содержит TRUST_ANCHOR_DESCRIPTOR_VERSION" >&2
    exit 1
}
grep -Fq 'TRUST_ANCHOR_ID_MIN' \
    kernel/include/trust_anchor.h || {
    echo "trust_anchor.h не содержит TRUST_ANCHOR_ID_MIN" >&2
    exit 1
}
grep -Fq 'TRUST_ANCHOR_ID_MAX' \
    kernel/include/trust_anchor.h || {
    echo "trust_anchor.h не содержит TRUST_ANCHOR_ID_MAX" >&2
    exit 1
}
grep -Fq 'TRUST_ANCHOR_ALGORITHM_ID_MAX' \
    kernel/include/trust_anchor.h || {
    echo "trust_anchor.h не содержит TRUST_ANCHOR_ALGORITHM_ID_MAX" >&2
    exit 1
}
grep -Fq 'TRUST_ANCHOR_SET_MAX' \
    kernel/include/trust_anchor.h || {
    echo "trust_anchor.h не содержит TRUST_ANCHOR_SET_MAX" >&2
    exit 1
}
grep -Fq 'TRUST_ANCHOR_STATE_REVOKED' \
    kernel/include/trust_anchor.h || {
    echo "trust_anchor.h не содержит TRUST_ANCHOR_STATE_REVOKED" >&2
    exit 1
}
grep -Fq 'struct trust_anchor' \
    kernel/include/trust_anchor.h || {
    echo "trust_anchor.h не содержит struct trust_anchor" >&2
    exit 1
}
grep -Fq 'accepted_purposes' \
    kernel/include/trust_anchor.h || {
    echo "trust_anchor.h не содержит accepted_purposes" >&2
    exit 1
}
grep -Fq 'TRUST_ANCHOR_INVALID_PURPOSES' \
    kernel/include/trust_anchor.h || {
    echo "trust_anchor.h не содержит TRUST_ANCHOR_INVALID_PURPOSES" >&2
    exit 1
}
for anchor_symbol in trust_anchor_validate \
                     trust_anchor_init \
                     trust_anchor_status_string \
                     trust_anchor_set_validate \
                     trust_anchor_set_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $anchor_symbol" >/dev/null || {
        echo "Production kernel не содержит $anchor_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(block_device_read|block_device_write)\b' \
        kernel/src/trust_anchor.c; then
    echo "trust_anchor.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'trust_anchor_host_test_'; then
    echo "trust_anchor_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Trust anchor host tests: ALL OK'; then
    echo "Production kernel содержит trust anchor host marker" >&2
    exit 1
fi
for anchor_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'trust anchor|trust-anchor' "$anchor_doc"; then
        echo "$anchor_doc не документирует trust anchor descriptors" >&2
        exit 1
    fi
done
