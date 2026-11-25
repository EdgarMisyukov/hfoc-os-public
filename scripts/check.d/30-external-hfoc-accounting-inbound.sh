#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# External HFOC accounting inbound configuration contract.
grep -Fq 'test-integration-config-intent-host' Makefile || {
    echo "Makefile не содержит test-integration-config-intent-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-integration-config-intent-host' Makefile || {
    echo "test: target не содержит test-integration-config-intent-host" >&2
    exit 1
}
grep -Fq 'INTEGRATION_CONFIG_INTENT_VERSION' \
    kernel/include/integration_config_intent.h || {
    echo "integration_config_intent.h не содержит INTEGRATION_CONFIG_INTENT_VERSION" >&2
    exit 1
}
grep -Fq 'INTEGRATION_CONFIG_INTENT_PEER_ID_MAX' \
    kernel/include/integration_config_intent.h || {
    echo "integration_config_intent.h не содержит peer ID maximum" >&2
    exit 1
}
grep -Fq 'INTEGRATION_CONFIG_INTENT_COUNT' \
    kernel/include/integration_config_intent.h || {
    echo "integration_config_intent.h не содержит intent count" >&2
    exit 1
}
grep -Fq 'struct integration_config_intent' \
    kernel/include/integration_config_intent.h || {
    echo "integration_config_intent.h не содержит intent descriptor" >&2
    exit 1
}
for integration_config_intent_symbol in \
        integration_config_intent_validate \
        integration_config_intent_apply \
        integration_config_intent_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $integration_config_intent_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $integration_config_intent_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b' \
        kernel/src/integration_config_intent.c; then
    echo "integration_config_intent.c выполняет запрещённое выделение, диагностику, блокирование или hardware mutation" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'integration_config_intent_host_test_'; then
    echo "integration_config_intent_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Integration config intent host tests: ALL OK'; then
    echo "Production kernel содержит integration config intent host marker" >&2
    exit 1
fi
if ! printf '#include "integration_config_intent.h"\nvoid hfoc_integration_config_intent_header(void) { (void)INTEGRATION_CONFIG_INTENT_VERSION; }\n' |
    gcc -std=c11 -Wall -Wextra -Werror -Wpedantic -ffreestanding \
        -Ikernel/include -x c -c - -o /dev/null; then
    echo "integration_config_intent.h не компилируется как self-contained header" >&2
    exit 1
fi
grep -Fq '## Inbound Configuration Contract' \
    docs/ACCOUNTING-INTEGRATION.md || {
    echo "ACCOUNTING-INTEGRATION.md не содержит inbound configuration contract section" >&2
    exit 1
}
for integration_config_intent_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'inbound configuration contract|configuration intent' \
            "$integration_config_intent_doc"; then
        echo "$integration_config_intent_doc не документирует inbound configuration contract" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **177. Inbound configuration contract' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 177 завершённым" >&2
    exit 1
}

# External HFOC accounting integration failure isolation contracts.
grep -Fq 'test-integration-failure-isolation-host' Makefile || {
    echo "Makefile не содержит test-integration-failure-isolation-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-integration-failure-isolation-host' Makefile || {
    echo "test: target не содержит test-integration-failure-isolation-host" >&2
    exit 1
}
grep -Fq 'INTEGRATION_FAILURE_ISOLATION_VERSION' \
    kernel/include/integration_failure_isolation.h || {
    echo "integration_failure_isolation.h не содержит INTEGRATION_FAILURE_ISOLATION_VERSION" >&2
    exit 1
}
grep -Fq 'INTEGRATION_FAILURE_ISOLATION_MAX_PEERS' \
    kernel/include/integration_failure_isolation.h || {
    echo "integration_failure_isolation.h не содержит maximum peers" >&2
    exit 1
}
grep -Fq 'INTEGRATION_FAILURE_ISOLATION_THRESHOLD' \
    kernel/include/integration_failure_isolation.h || {
    echo "integration_failure_isolation.h не содержит failure threshold" >&2
    exit 1
}
grep -Fq 'struct integration_failure_isolation_table' \
    kernel/include/integration_failure_isolation.h || {
    echo "integration_failure_isolation.h не содержит table descriptor" >&2
    exit 1
}
for integration_failure_isolation_symbol in \
        integration_failure_isolation_init \
        integration_failure_isolation_record \
        integration_failure_isolation_check \
        integration_failure_isolation_recover \
        integration_failure_isolation_local_allowed \
        integration_failure_isolation_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $integration_failure_isolation_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $integration_failure_isolation_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/integration_failure_isolation.c; then
    echo "integration_failure_isolation.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'integration_failure_isolation_host_test_'; then
    echo "integration_failure_isolation_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Integration failure isolation host tests: ALL OK'; then
    echo "Production kernel содержит integration failure isolation host marker" >&2
    exit 1
fi
if ! printf '#include "integration_failure_isolation.h"\nvoid hfoc_integration_failure_isolation_header(void) { (void)INTEGRATION_FAILURE_ISOLATION_VERSION; }\n' |
    gcc -std=c11 -Wall -Wextra -Werror -Wpedantic -ffreestanding \
        -Ikernel/include -x c -c - -o /dev/null; then
    echo "integration_failure_isolation.h не компилируется как self-contained header" >&2
    exit 1
fi
grep -Fq '## Integration Failure Isolation' \
    docs/ACCOUNTING-INTEGRATION.md || {
    echo "ACCOUNTING-INTEGRATION.md не содержит integration failure isolation section" >&2
    exit 1
}
for integration_failure_isolation_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'integration failure isolation' "$integration_failure_isolation_doc"; then
        echo "$integration_failure_isolation_doc не документирует integration failure isolation" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **178. Integration failure isolation' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 178 завершённым" >&2
    exit 1
}

# External HFOC accounting integration compatibility policy contracts.
grep -Fq 'test-integration-compatibility-host' Makefile || {
    echo "Makefile не содержит test-integration-compatibility-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-integration-compatibility-host' Makefile || {
    echo "test: target не содержит test-integration-compatibility-host" >&2
    exit 1
}
grep -Fq 'INTEGRATION_COMPATIBILITY_VERSION' \
    kernel/include/integration_compatibility.h || {
    echo "integration_compatibility.h не содержит INTEGRATION_COMPATIBILITY_VERSION" >&2
    exit 1
}
grep -Fq 'INTEGRATION_COMPATIBILITY_SCHEMA_VERSION_MAX' \
    kernel/include/integration_compatibility.h || {
    echo "integration_compatibility.h не содержит schema version maximum" >&2
    exit 1
}
grep -Fq 'INTEGRATION_COMPATIBILITY_TRANSPORT_VERSION_MAX' \
    kernel/include/integration_compatibility.h || {
    echo "integration_compatibility.h не содержит transport version maximum" >&2
    exit 1
}
grep -Fq 'INTEGRATION_COMPATIBILITY_DEPRECATION_DEPRECATED' \
    kernel/include/integration_compatibility.h || {
    echo "integration_compatibility.h не содержит deprecated state" >&2
    exit 1
}
grep -Fq 'struct integration_compatibility_policy' \
    kernel/include/integration_compatibility.h || {
    echo "integration_compatibility.h не содержит policy descriptor" >&2
    exit 1
}
grep -Fq 'struct integration_compatibility_request' \
    kernel/include/integration_compatibility.h || {
    echo "integration_compatibility.h не содержит request descriptor" >&2
    exit 1
}
for integration_compatibility_symbol in \
        integration_compatibility_policy_init \
        integration_compatibility_policy_validate \
        integration_compatibility_policy_deprecate \
        integration_compatibility_check \
        integration_compatibility_session_validate \
        integration_compatibility_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $integration_compatibility_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $integration_compatibility_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/integration_compatibility.c; then
    echo "integration_compatibility.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'integration_compatibility_host_test_'; then
    echo "integration_compatibility_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Integration compatibility host tests: ALL OK'; then
    echo "Production kernel содержит integration compatibility host marker" >&2
    exit 1
fi
if ! printf '#include "integration_compatibility.h"\nvoid hfoc_integration_compatibility_header(void) { (void)INTEGRATION_COMPATIBILITY_VERSION; }\n' |
    gcc -std=c11 -Wall -Wextra -Werror -Wpedantic -ffreestanding \
        -Ikernel/include -x c -c - -o /dev/null; then
    echo "integration_compatibility.h не компилируется как self-contained header" >&2
    exit 1
fi
grep -Fq '## Integration Compatibility Policy' \
    docs/ACCOUNTING-INTEGRATION.md || {
    echo "ACCOUNTING-INTEGRATION.md не содержит integration compatibility policy section" >&2
    exit 1
}
for integration_compatibility_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'integration compatibility policy' "$integration_compatibility_doc"; then
        echo "$integration_compatibility_doc не документирует integration compatibility policy" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **179. Integration compatibility policy' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 179 завершённым" >&2
    exit 1
}

# Accounting-boundary QEMU proof contracts.
grep -Fq 'test-accounting-boundary-qemu' Makefile || {
    echo "Makefile не содержит test-accounting-boundary-qemu" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-accounting-boundary-qemu' Makefile || {
    echo "test: target не содержит test-accounting-boundary-qemu" >&2
    exit 1
}
grep -Fq 'scripts/test-qemu-accounting-boundary.sh' Makefile || {
    echo "Makefile не содержит accounting-boundary QEMU script" >&2
    exit 1
}
grep -Fq 'HFOC_ACCOUNTING_BOUNDARY_QEMU_TEST' \
    kernel/src/accounting_boundary_qemu.c || {
    echo "accounting_boundary_qemu.c не имеет proof guard" >&2
    exit 1
}
grep -Fq '[ACCOUNTING-BOUNDARY] proof init' \
    kernel/src/accounting_boundary_qemu.c || {
    echo "accounting_boundary_qemu.c не содержит proof init marker" >&2
    exit 1
}
grep -Fq 'accounting_boundary_qemu_test_init' kernel/src/kernel.c || {
    echo "kernel.c не вызывает accounting-boundary proof" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[ACCOUNTING-BOUNDARY]'; then
    echo "Production kernel содержит accounting-boundary marker" >&2
    exit 1
fi
if grep -Fq 'qemu-accounting-boundary' .github/workflows/ci.yml; then
    :
else
    echo "CI pgrep guard не содержит accounting-boundary script" >&2
    exit 1
fi
grep -Fq 'QEMU accounting-boundary baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-accounting-boundary.sh || {
    echo "test-qemu-accounting-boundary.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU accounting-boundary test: OK' \
    scripts/test-qemu-accounting-boundary.sh || {
    echo "test-qemu-accounting-boundary.sh не содержит success marker" >&2
    exit 1
}
grep -Fq '## Accounting-boundary QEMU proof' \
    docs/ACCOUNTING-INTEGRATION.md || {
    echo "ACCOUNTING-INTEGRATION.md не содержит accounting-boundary QEMU proof section" >&2
    exit 1
}
for accounting_boundary_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'accounting-boundary QEMU proof|accounting boundary QEMU proof' \
            "$accounting_boundary_doc"; then
        echo "$accounting_boundary_doc не документирует accounting-boundary QEMU proof" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **180. Accounting-boundary QEMU proof' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 180 завершённым" >&2
    exit 1
}

# Fault-injection catalogue contracts.
grep -Fq 'test-fault-injection-host' Makefile || {
    echo "Makefile не содержит test-fault-injection-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-fault-injection-host' Makefile || {
    echo "test: target не содержит test-fault-injection-host" >&2
    exit 1
}
grep -Fq 'FAULT_INJECTION_VERSION' kernel/include/fault_injection.h || {
    echo "fault_injection.h не содержит FAULT_INJECTION_VERSION" >&2
    exit 1
}
grep -Fq 'FAULT_INJECTION_CATALOGUE_CAPACITY' \
    kernel/include/fault_injection.h || {
    echo "fault_injection.h не содержит catalogue capacity" >&2
    exit 1
}
grep -Fq 'struct fault_injection_site' \
    kernel/include/fault_injection.h || {
    echo "fault_injection.h не содержит site descriptor" >&2
    exit 1
}
grep -Fq 'struct fault_injection_activation' \
    kernel/include/fault_injection.h || {
    echo "fault_injection.h не содержит activation descriptor" >&2
    exit 1
}
for fault_injection_symbol in \
        fault_injection_catalogue_count \
        fault_injection_catalogue_at \
        fault_injection_site_by_id \
        fault_injection_subsystem_string \
        fault_injection_status_string \
        fault_injection_id_string; do
    nm build/hfoc-kernel.elf | grep -F " $fault_injection_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $fault_injection_symbol" >&2
        exit 1
    }
done
if nm -g build/hfoc-kernel.elf |
        grep 'fault_injection_activation_validate'; then
    echo "Production kernel содержит test-only activation validator" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Fault injection catalogue host tests: ALL OK'; then
    echo "Production kernel содержит fault-injection host marker" >&2
    exit 1
fi
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/fault_injection.c; then
    echo "fault_injection.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if ! printf '#include "fault_injection.h"\nvoid hfoc_fault_injection_header(void) { (void)FAULT_INJECTION_VERSION; }\n' |
    gcc -std=c11 -Wall -Wextra -Werror -Wpedantic -ffreestanding \
        -Ikernel/include -x c -c - -o /dev/null; then
    echo "fault_injection.h не компилируется как self-contained header" >&2
    exit 1
fi
for fault_injection_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'fault-injection catalogue|fault injection catalogue' \
            "$fault_injection_doc"; then
        echo "$fault_injection_doc не документирует fault-injection catalogue" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **181. Fault-injection catalogue' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 181 завершённым" >&2
    exit 1
}

# Storage interruption drill contracts.
grep -Fq 'test-storage-interruption-host' Makefile || {
    echo "Makefile не содержит test-storage-interruption-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-storage-interruption-host' Makefile || {
    echo "test: target не содержит test-storage-interruption-host" >&2
    exit 1
}
grep -Fq 'test-storage-interruption-qemu' Makefile || {
    echo "Makefile не содержит test-storage-interruption-qemu" >&2
    exit 1
}
if nm -g build/hfoc-kernel.elf |
        grep 'storage_interruption_qemu_test_init'; then
    echo "Production kernel содержит storage-interruption QEMU symbol" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[STORAGE-INTERRUPTION]'; then
    echo "Production kernel содержит storage-interruption proof marker" >&2
    exit 1
fi
for storage_interruption_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'storage interruption drill|storage-interruption drill' \
            "$storage_interruption_doc"; then
        echo "$storage_interruption_doc не документирует storage interruption drill" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **182. Storage interruption drill' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 182 завершённым" >&2
    exit 1
}

# Service cascade drill contracts.
grep -Fq 'test-service-cascade-drill-host' Makefile || {
    echo "Makefile не содержит test-service-cascade-drill-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-service-cascade-drill-host' Makefile || {
    echo "test: target не содержит test-service-cascade-drill-host" >&2
    exit 1
}
grep -Fq 'test-service-cascade-drill-qemu' Makefile || {
    echo "Makefile не содержит test-service-cascade-drill-qemu" >&2
    exit 1
}
if nm -g build/hfoc-kernel.elf |
        grep 'service_cascade_drill_qemu_test_init'; then
    echo "Production kernel содержит service-cascade-drill QEMU symbol" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[SERVICE-CASCADE-DRILL]'; then
    echo "Production kernel содержит service-cascade-drill proof marker" >&2
    exit 1
fi
grep -Fq 'scripts/test-qemu-service-cascade-drill.sh' Makefile || {
    echo "Makefile не вызывает service-cascade-drill QEMU script" >&2
    exit 1
}
grep -Fq 'QEMU service cascade baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-service-cascade-drill.sh || {
    echo "test-qemu-service-cascade-drill.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU service cascade drill test: OK' \
    scripts/test-qemu-service-cascade-drill.sh || {
    echo "test-qemu-service-cascade-drill.sh не содержит success marker" >&2
    exit 1
}
if grep -Fq 'qemu-service-cascade-drill' .github/workflows/ci.yml; then
    :
else
    echo "CI pgrep guard не содержит service-cascade-drill script" >&2
    exit 1
fi
for service_cascade_drill_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'service cascade drill|service-cascade drill' \
            "$service_cascade_drill_doc"; then
        echo "$service_cascade_drill_doc не документирует service cascade drill" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **183. Service cascade drill' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 183 завершённым" >&2
    exit 1
}

# Local-control outage drill contracts.
grep -Fq 'test-local-control-outage-drill-host' Makefile || {
    echo "Makefile не содержит test-local-control-outage-drill-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-local-control-outage-drill-host' Makefile || {
    echo "test: target не содержит test-local-control-outage-drill-host" >&2
    exit 1
}
grep -Fq 'test-local-control-outage-drill-qemu' Makefile || {
    echo "Makefile не содержит test-local-control-outage-drill-qemu" >&2
    exit 1
}
if nm -g build/hfoc-kernel.elf |
        grep 'local_control_outage_drill_qemu_test_init'; then
    echo "Production kernel содержит local-control-outage-drill QEMU symbol" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        '[LOCAL-CONTROL-OUTAGE-DRILL]'; then
    echo "Production kernel содержит local-control-outage-drill proof marker" >&2
    exit 1
fi
grep -Fq 'scripts/test-qemu-local-control-outage-drill.sh' Makefile || {
    echo "Makefile не вызывает local-control-outage-drill QEMU script" >&2
    exit 1
}
grep -Fq 'QEMU local-control outage baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-local-control-outage-drill.sh || {
    echo "test-qemu-local-control-outage-drill.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU local-control outage drill test: OK' \
    scripts/test-qemu-local-control-outage-drill.sh || {
    echo "test-qemu-local-control-outage-drill.sh не содержит success marker" >&2
    exit 1
}
if grep -Fq 'qemu-local-control-outage-drill' .github/workflows/ci.yml; then
    :
else
    echo "CI pgrep guard не содержит local-control-outage-drill script" >&2
    exit 1
fi
for local_control_outage_drill_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'local-control outage drill|local-control-outage drill' \
            "$local_control_outage_drill_doc"; then
        echo "$local_control_outage_drill_doc не документирует local-control outage drill" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **184. Local-control outage drill' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 184 завершённым" >&2
    exit 1
}

# Network partition drill contracts.
grep -Fq 'test-network-partition-drill-host' Makefile || {
    echo "Makefile не содержит test-network-partition-drill-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-network-partition-drill-host' Makefile || {
    echo "test: target не содержит test-network-partition-drill-host" >&2
    exit 1
}
grep -Fq 'test-network-partition-drill-qemu' Makefile || {
    echo "Makefile не содержит test-network-partition-drill-qemu" >&2
    exit 1
}
if nm -g build/hfoc-kernel.elf |
        grep 'network_partition_drill_qemu_test_init'; then
    echo "Production kernel содержит network-partition-drill QEMU symbol" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        '[NETWORK-PARTITION-DRILL]'; then
    echo "Production kernel содержит network-partition-drill proof marker" >&2
    exit 1
fi
grep -Fq 'scripts/test-qemu-network-partition-drill.sh' Makefile || {
    echo "Makefile не вызывает network-partition-drill QEMU script" >&2
    exit 1
}
grep -Fq 'QEMU network partition baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-network-partition-drill.sh || {
    echo "test-qemu-network-partition-drill.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU network partition drill test: OK' \
    scripts/test-qemu-network-partition-drill.sh || {
    echo "test-qemu-network-partition-drill.sh не содержит success marker" >&2
    exit 1
}
if grep -Fq 'qemu-network-partition-drill' .github/workflows/ci.yml; then
    :
else
    echo "CI pgrep guard не содержит network-partition-drill script" >&2
    exit 1
fi
for network_partition_drill_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'network partition drill|network-partition drill' \
            "$network_partition_drill_doc"; then
        echo "$network_partition_drill_doc не документирует network partition drill" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **185. Network partition drill' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 185 завершённым" >&2
    exit 1
}
