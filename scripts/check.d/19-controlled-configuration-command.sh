#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Controlled configuration command contracts.
grep -Fq 'test-controlled-configuration-host' Makefile || {
    echo "Makefile не содержит test-controlled-configuration-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-controlled-configuration-host' Makefile || {
    echo "test: target не содержит test-controlled-configuration-host" >&2
    exit 1
}
grep -Fq 'CONTROLLED_CONFIGURATION_VERSION' \
    kernel/include/controlled_configuration.h || {
    echo "controlled_configuration.h не содержит CONTROLLED_CONFIGURATION_VERSION" >&2
    exit 1
}
grep -Fq 'CONTROLLED_CONFIGURATION_COMMAND_ID' \
    kernel/include/controlled_configuration.h || {
    echo "controlled_configuration.h не содержит CONTROLLED_CONFIGURATION_COMMAND_ID" >&2
    exit 1
}
grep -Fq 'CONTROLLED_CONFIGURATION_REQUEST_MAX' \
    kernel/include/controlled_configuration.h || {
    echo "controlled_configuration.h не содержит CONTROLLED_CONFIGURATION_REQUEST_MAX" >&2
    exit 1
}
grep -Fq 'CONTROLLED_CONFIGURATION_AUDIT_OPERATION_ACTIVATE' \
    kernel/include/controlled_configuration.h || {
    echo "controlled_configuration.h не содержит audit operation" >&2
    exit 1
}
for controlled_symbol in controlled_configuration_apply \
                         controlled_configuration_command_descriptor \
                         controlled_configuration_authorization_entry \
                         controlled_configuration_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $controlled_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $controlled_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/controlled_configuration.c; then
    echo "controlled_configuration.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'controlled_configuration_host_test_'; then
    echo "controlled_configuration_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Controlled configuration host tests: ALL OK'; then
    echo "Production kernel содержит controlled configuration host marker" >&2
    exit 1
fi
for controlled_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'controlled configuration command|controlled-configuration' \
            "$controlled_doc"; then
        echo "$controlled_doc не документирует controlled configuration command" >&2
        exit 1
    fi
done

# Management audit correlation contracts.
grep -Fq 'test-audit-correlation-host' Makefile || {
    echo "Makefile не содержит test-audit-correlation-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-audit-correlation-host' Makefile || {
    echo "test: target не содержит test-audit-correlation-host" >&2
    exit 1
}
grep -Fq 'AUDIT_CORRELATION_VERSION' \
    kernel/include/audit_correlation.h || {
    echo "audit_correlation.h не содержит AUDIT_CORRELATION_VERSION" >&2
    exit 1
}
grep -Fq 'AUDIT_CORRELATION_ID_MIN' \
    kernel/include/audit_correlation.h || {
    echo "audit_correlation.h не содержит AUDIT_CORRELATION_ID_MIN" >&2
    exit 1
}
grep -Fq 'AUDIT_CORRELATION_RESULT_DENIED' \
    kernel/include/audit_correlation.h || {
    echo "audit_correlation.h не содержит denied result" >&2
    exit 1
}
grep -Fq 'struct audit_correlation' \
    kernel/include/audit_correlation.h || {
    echo "audit_correlation.h не содержит struct audit_correlation" >&2
    exit 1
}
grep -Fq 'AUDIT_RECORD_MANAGEMENT' \
    kernel/include/audit.h || {
    echo "audit.h не содержит AUDIT_RECORD_MANAGEMENT" >&2
    exit 1
}
grep -Fq 'session_id' kernel/include/audit.h || {
    echo "audit.h не содержит session_id correlation field" >&2
    exit 1
}
grep -Fq 'principal_id' kernel/include/audit.h || {
    echo "audit.h не содержит principal_id correlation field" >&2
    exit 1
}
grep -Fq 'command_id' kernel/include/audit.h || {
    echo "audit.h не содержит command_id correlation field" >&2
    exit 1
}
grep -Fq 'audit_record_init_management' \
    kernel/include/audit.h || {
    echo "audit.h не содержит audit_record_init_management" >&2
    exit 1
}
grep -Fq 'slot->data[2] = event->data[2];' \
    kernel/src/event_ring.c || {
    echo "event_ring.c не сохраняет третье событие data word" >&2
    exit 1
}
for correlation_symbol in audit_correlation_validate \
                          audit_correlation_append \
                          audit_correlation_status_string \
                          audit_record_init_management \
                          controlled_configuration_apply_correlated; do
    nm build/hfoc-kernel.elf | grep -F " $correlation_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $correlation_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/audit_correlation.c; then
    echo "audit_correlation.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'audit_correlation_host_test_'; then
    echo "audit_correlation_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Audit correlation host tests: ALL OK'; then
    echo "Production kernel содержит audit correlation host marker" >&2
    exit 1
fi
for correlation_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'audit correlation|audit-correlation' \
            "$correlation_doc"; then
        echo "$correlation_doc не документирует management audit correlation" >&2
        exit 1
    fi
done

# Local-control QEMU proof contracts.
grep -Fq 'test-local-control-qemu' Makefile || {
    echo "Makefile не содержит test-local-control-qemu" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-local-control-qemu' Makefile || {
    echo "test: target не содержит test-local-control-qemu" >&2
    exit 1
}
grep -Fq 'scripts/test-qemu-local-control.sh' Makefile || {
    echo "Makefile не вызывает local-control QEMU script" >&2
    exit 1
}
grep -Fq 'HFOC_LOCAL_CONTROL_QEMU_TEST' \
    kernel/src/local_control_qemu.c || {
    echo "local_control_qemu.c не имеет proof guard" >&2
    exit 1
}
grep -Fq 'QEMU local-control baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-local-control.sh || {
    echo "test-qemu-local-control.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU local-control test: OK' \
    scripts/test-qemu-local-control.sh || {
    echo "test-qemu-local-control.sh не содержит success marker" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[LOCAL-CONTROL]'; then
    echo "Production kernel содержит local-control proof marker" >&2
    exit 1
fi
local_control_iso_path="build/hfoc-os-$(<VERSION).iso"
if [[ -f "$local_control_iso_path" ]] &&
   bash scripts/hfoc-file-marker.sh "$local_control_iso_path" '[LOCAL-CONTROL]'; then
    echo "Production ISO содержит local-control proof marker" >&2
    exit 1
fi
for local_control_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'local-control QEMU proof|local-control qemu|local-control proof' \
            "$local_control_doc"; then
        echo "$local_control_doc не документирует local-control QEMU proof" >&2
        exit 1
    fi
done

# Network device descriptor contracts.
grep -Fq 'test-network-device-host' Makefile || {
    echo "Makefile не содержит test-network-device-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-network-device-host' Makefile || {
    echo "test: target не содержит test-network-device-host" >&2
    exit 1
}
grep -Fq 'NETWORK_DEVICE_SNAPSHOT_VERSION' \
    kernel/include/network_device.h || {
    echo "network_device.h не содержит NETWORK_DEVICE_SNAPSHOT_VERSION" >&2
    exit 1
}
grep -Fq 'NETWORK_DEVICE_ID_MIN' \
    kernel/include/network_device.h || {
    echo "network_device.h не содержит NETWORK_DEVICE_ID_MIN" >&2
    exit 1
}
grep -Fq 'NETWORK_DEVICE_ID_MAX' \
    kernel/include/network_device.h || {
    echo "network_device.h не содержит NETWORK_DEVICE_ID_MAX" >&2
    exit 1
}
grep -Fq 'NETWORK_DEVICE_CAPABILITY_MASK' \
    kernel/include/network_device.h || {
    echo "network_device.h не содержит NETWORK_DEVICE_CAPABILITY_MASK" >&2
    exit 1
}
grep -Fq 'struct network_device_selection' \
    kernel/include/network_device.h || {
    echo "network_device.h не содержит struct network_device_selection" >&2
    exit 1
}
for network_device_symbol in network_device_snapshot_validate \
                             network_device_snapshot_publish \
                             network_device_selection_init \
                             network_device_selection_validate_state \
                             network_device_selection_published \
                             network_device_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $network_device_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $network_device_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/network_device.c; then
    echo "network_device.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'network_device_(selection_init|selection_validate_state)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Network-device publication вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'network_device_host_test_'; then
    echo "network_device_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Network device host tests: ALL OK'; then
    echo "Production kernel содержит network device host marker" >&2
    exit 1
fi
for network_device_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'network device descriptor|network-device descriptor' \
            "$network_device_doc"; then
        echo "$network_device_doc не документирует network-device descriptor" >&2
        exit 1
    fi
done

# Link-state model contracts.
grep -Fq 'test-link-state-host' Makefile || {
    echo "Makefile не содержит test-link-state-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-link-state-host' Makefile || {
    echo "test: target не содержит test-link-state-host" >&2
    exit 1
}
grep -Fq 'NETWORK_LINK_STATE_VERSION' \
    kernel/include/link_state.h || {
    echo "link_state.h не содержит NETWORK_LINK_STATE_VERSION" >&2
    exit 1
}
grep -Fq 'NETWORK_LINK_SELECTION_VERSION' \
    kernel/include/link_state.h || {
    echo "link_state.h не содержит NETWORK_LINK_SELECTION_VERSION" >&2
    exit 1
}
grep -Fq 'NETWORK_LINK_DOWN' \
    kernel/include/link_state.h || {
    echo "link_state.h не содержит NETWORK_LINK_DOWN" >&2
    exit 1
}
grep -Fq 'NETWORK_LINK_INITIALIZING' \
    kernel/include/link_state.h || {
    echo "link_state.h не содержит NETWORK_LINK_INITIALIZING" >&2
    exit 1
}
grep -Fq 'NETWORK_LINK_UP' \
    kernel/include/link_state.h || {
    echo "link_state.h не содержит NETWORK_LINK_UP" >&2
    exit 1
}
grep -Fq 'NETWORK_LINK_FAILED' \
    kernel/include/link_state.h || {
    echo "link_state.h не содержит NETWORK_LINK_FAILED" >&2
    exit 1
}
grep -Fq 'struct network_link_selection' \
    kernel/include/link_state.h || {
    echo "link_state.h не содержит struct network_link_selection" >&2
    exit 1
}
for link_state_symbol in network_link_state_validate \
                         network_link_selection_init \
                         network_link_selection_published \
                         network_link_path_status \
                         network_link_path_enabled \
                         network_link_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $link_state_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $link_state_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/link_state.c; then
    echo "link_state.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'network_link_(selection_init|path_status|path_enabled)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Link-state publication/admission вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'link_state_host_test_'; then
    echo "link_state_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Link state host tests: ALL OK'; then
    echo "Production kernel содержит link state host marker" >&2
    exit 1
fi
for link_state_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'link-state model|link state model' \
            "$link_state_doc"; then
        echo "$link_state_doc не документирует link-state model" >&2
        exit 1
    fi
done

# Virtio PCI identity contract contracts.
grep -Fq 'test-virtio-pci-identity-host' Makefile || {
    echo "Makefile не содержит test-virtio-pci-identity-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-virtio-pci-identity-host' Makefile || {
    echo "test-host-suite не вызывает test-virtio-pci-identity-host" >&2
    exit 1
}
grep -Fq 'VIRTIO_PCI_VENDOR_ID' \
    kernel/include/virtio_pci_identity.h || {
    echo "virtio_pci_identity.h не содержит VIRTIO_PCI_VENDOR_ID" >&2
    exit 1
}
grep -Fq 'VIRTIO_PCI_DEVICE_ID_NET_TRANSITIONAL' \
    kernel/include/virtio_pci_identity.h || {
    echo "virtio_pci_identity.h не содержит transitional net device ID" >&2
    exit 1
}
grep -Fq 'VIRTIO_PCI_DEVICE_ID_RNG_MODERN' \
    kernel/include/virtio_pci_identity.h || {
    echo "virtio_pci_identity.h не содержит modern rng device ID" >&2
    exit 1
}
grep -Fq 'VIRTIO_PCI_CAPABILITY_MASK' \
    kernel/include/virtio_pci_identity.h || {
    echo "virtio_pci_identity.h не содержит VIRTIO_PCI_CAPABILITY_MASK" >&2
    exit 1
}
grep -Fq 'struct virtio_pci_selection' \
    kernel/include/virtio_pci_identity.h || {
    echo "virtio_pci_identity.h не содержит struct virtio_pci_selection" >&2
    exit 1
}
for virtio_pci_identity_symbol in virtio_pci_identity_validate \
                                  virtio_pci_capability_snapshot_validate \
                                  virtio_pci_selection_init \
                                  virtio_pci_selection_validate_state \
                                  virtio_pci_selection_published \
                                  virtio_pci_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $virtio_pci_identity_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $virtio_pci_identity_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/virtio_pci_identity.c; then
    echo "virtio_pci_identity.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'virtio_pci_(selection_init|selection_validate_state)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Virtio PCI identity publication вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'virtio_pci_identity_host_test_'; then
    echo "virtio_pci_identity_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Virtio PCI identity host tests: ALL OK'; then
    echo "Production kernel содержит virtio PCI identity host marker" >&2
    exit 1
fi
for virtio_pci_identity_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'virtio pci identity contract|virtio-pci-identity' \
            "$virtio_pci_identity_doc"; then
        echo "$virtio_pci_identity_doc не документирует virtio PCI identity" >&2
        exit 1
    fi
done

# Virtio PCI capability parser contracts.
grep -Fq 'test-virtio-pci-capability-host' Makefile || {
    echo "Makefile не содержит test-virtio-pci-capability-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-virtio-pci-capability-host' Makefile || {
    echo "test-host-suite не вызывает test-virtio-pci-capability-host" >&2
    exit 1
}
grep -Fq 'VIRTIO_PCI_CAPABILITY_POINTER_OFFSET' \
    kernel/include/virtio_pci_capability.h || {
    echo "virtio_pci_capability.h не содержит capability pointer offset" >&2
    exit 1
}
grep -Fq 'VIRTIO_PCI_CAPABILITY_ID_VENDOR' \
    kernel/include/virtio_pci_capability.h || {
    echo "virtio_pci_capability.h не содержит vendor capability ID" >&2
    exit 1
}
grep -Fq 'struct virtio_pci_bar_range' \
    kernel/include/virtio_pci_capability.h || {
    echo "virtio_pci_capability.h не содержит struct virtio_pci_bar_range" >&2
    exit 1
}
grep -Fq 'struct virtio_pci_capability_parse_result' \
    kernel/include/virtio_pci_capability.h || {
    echo "virtio_pci_capability.h не содержит parse result" >&2
    exit 1
}
for virtio_pci_capability_symbol in virtio_pci_capability_parse \
                                    virtio_pci_capability_result_validate \
                                    virtio_pci_capability_result_published \
                                    virtio_pci_capability_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $virtio_pci_capability_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $virtio_pci_capability_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/virtio_pci_capability.c; then
    echo "virtio_pci_capability.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'virtio_pci_capability_parse\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Virtio PCI capability parsing вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'virtio_pci_capability_host_test_'; then
    echo "virtio_pci_capability_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Virtio PCI capability host tests: ALL OK'; then
    echo "Production kernel содержит virtio PCI capability host marker" >&2
    exit 1
fi
for virtio_pci_capability_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'virtio pci capability parser|virtio-pci-capability' \
            "$virtio_pci_capability_doc"; then
        echo "$virtio_pci_capability_doc не документирует virtio PCI capability parser" >&2
        exit 1
    fi
done

# Virtio PCI device status lifecycle contracts.
grep -Fq 'test-virtio-pci-device-status-host' Makefile || {
    echo "Makefile не содержит test-virtio-pci-device-status-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-virtio-pci-device-status-host' Makefile || {
    echo "test-host-suite не вызывает test-virtio-pci-device-status-host" >&2
    exit 1
}
grep -Fq 'VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE' \
    kernel/include/virtio_pci_device_status.h || {
    echo "virtio_pci_device_status.h не содержит ACKNOWLEDGE bit" >&2
    exit 1
}
grep -Fq 'struct virtio_pci_device_status_io' \
    kernel/include/virtio_pci_device_status.h || {
    echo "virtio_pci_device_status.h не содержит struct virtio_pci_device_status_io" >&2
    exit 1
}
grep -Fq 'struct virtio_pci_device_status_controller' \
    kernel/include/virtio_pci_device_status.h || {
    echo "virtio_pci_device_status.h не содержит struct virtio_pci_device_status_controller" >&2
    exit 1
}
for virtio_pci_device_status_symbol in \
        virtio_pci_device_status_init \
        virtio_pci_device_status_reset \
        virtio_pci_device_status_acknowledge \
        virtio_pci_device_status_driver \
        virtio_pci_device_status_features_ok \
        virtio_pci_device_status_driver_ok \
        virtio_pci_device_status_failed \
        virtio_pci_device_status_observe \
        virtio_pci_device_status_live \
        virtio_pci_device_status_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $virtio_pci_device_status_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $virtio_pci_device_status_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/virtio_pci_device_status.c; then
    echo "virtio_pci_device_status.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'virtio_pci_device_status_(init|reset|acknowledge|driver|features_ok|driver_ok|failed|observe|live)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Virtio PCI device status lifecycle вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'virtio_pci_device_status_host_test_'; then
    echo "virtio_pci_device_status_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Virtio device status host tests: ALL OK'; then
    echo "Production kernel содержит virtio device status host marker" >&2
    exit 1
fi
for virtio_pci_device_status_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'virtio device status lifecycle|virtio-device-status' \
            "$virtio_pci_device_status_doc"; then
        echo "$virtio_pci_device_status_doc не документирует virtio device status lifecycle" >&2
        exit 1
    fi
done

# Virtio PCI feature negotiation contracts.
grep -Fq 'test-virtio-pci-feature-host' Makefile || {
    echo "Makefile не содержит test-virtio-pci-feature-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-virtio-pci-feature-host' Makefile || {
    echo "test-host-suite не вызывает test-virtio-pci-feature-host" >&2
    exit 1
}
grep -Fq 'struct virtio_pci_feature_bitset' \
    kernel/include/virtio_pci_feature.h || {
    echo "virtio_pci_feature.h не содержит feature bitset" >&2
    exit 1
}
grep -Fq 'struct virtio_pci_feature_policy' \
    kernel/include/virtio_pci_feature.h || {
    echo "virtio_pci_feature.h не содержит feature policy" >&2
    exit 1
}
grep -Fq 'struct virtio_pci_feature_io' \
    kernel/include/virtio_pci_feature.h || {
    echo "virtio_pci_feature.h не содержит feature I/O seam" >&2
    exit 1
}
grep -Fq 'struct virtio_pci_feature_controller' \
    kernel/include/virtio_pci_feature.h || {
    echo "virtio_pci_feature.h не содержит feature controller" >&2
    exit 1
}
grep -Fq 'VIRTIO_PCI_FEATURE_KNOWN_HI_MASK' \
    kernel/include/virtio_pci_feature.h || {
    echo "virtio_pci_feature.h не содержит known high mask" >&2
    exit 1
}
for virtio_pci_feature_symbol in virtio_pci_feature_init \
                                 virtio_pci_feature_negotiate \
                                 virtio_pci_feature_observe \
                                 virtio_pci_feature_published \
                                 virtio_pci_feature_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $virtio_pci_feature_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $virtio_pci_feature_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/virtio_pci_feature.c; then
    echo "virtio_pci_feature.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'virtio_pci_feature_(init|negotiate|observe|published)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Virtio PCI feature negotiation вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'virtio_pci_feature_host_test_'; then
    echo "virtio_pci_feature_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Virtio PCI feature host tests: ALL OK'; then
    echo "Production kernel содержит virtio PCI feature host marker" >&2
    exit 1
fi
for virtio_pci_feature_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'virtio.*feature negotiation|virtio-pci-feature' \
            "$virtio_pci_feature_doc"; then
        echo "$virtio_pci_feature_doc не документирует virtio feature negotiation" >&2
        exit 1
    fi
done
