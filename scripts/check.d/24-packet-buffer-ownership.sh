#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Packet buffer ownership contracts.
grep -Fq 'test-packet-buffer-host' Makefile || {
    echo "Makefile не содержит test-packet-buffer-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-packet-buffer-host' Makefile || {
    echo "test: target не содержит test-packet-buffer-host" >&2
    exit 1
}
grep -Fq 'PACKET_BUFFER_POOL_VERSION' \
    kernel/include/packet_buffer.h || {
    echo "packet_buffer.h не содержит PACKET_BUFFER_POOL_VERSION" >&2
    exit 1
}
grep -Fq 'PACKET_BUFFER_CAPACITY_MAX' \
    kernel/include/packet_buffer.h || {
    echo "packet_buffer.h не содержит PACKET_BUFFER_CAPACITY_MAX" >&2
    exit 1
}
grep -Fq 'PACKET_BUFFER_PAYLOAD_MAX' \
    kernel/include/packet_buffer.h || {
    echo "packet_buffer.h не содержит PACKET_BUFFER_PAYLOAD_MAX" >&2
    exit 1
}
grep -Fq 'struct packet_buffer_pool' \
    kernel/include/packet_buffer.h || {
    echo "packet_buffer.h не содержит struct packet_buffer_pool" >&2
    exit 1
}
for packet_buffer_symbol in packet_buffer_pool_init \
                            packet_buffer_acquire_rx \
                            packet_buffer_acquire_tx \
                            packet_buffer_handoff_tx \
                            packet_buffer_release \
                            packet_buffer_data \
                            packet_buffer_free_count \
                            packet_buffer_capacity \
                            packet_buffer_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $packet_buffer_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $packet_buffer_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/packet_buffer.c; then
    echo "packet_buffer.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'packet_buffer_(acquire_rx|acquire_tx|handoff_tx|release)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Packet buffer ownership transitions вызываются из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'packet_buffer_host_test_'; then
    echo "packet_buffer_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Packet buffer host tests: ALL OK'; then
    echo "Production kernel содержит packet buffer host marker" >&2
    exit 1
fi
for packet_buffer_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'packet buffer ownership|packet-buffer ownership' \
            "$packet_buffer_doc"; then
        echo "$packet_buffer_doc не документирует packet buffer ownership" >&2
        exit 1
    fi
done

# Network packet-pool descriptor contracts.
grep -Fq 'test-packet-pool-host' Makefile || {
    echo "Makefile не содержит test-packet-pool-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-packet-pool-host' Makefile || {
    echo "test: target не содержит test-packet-pool-host" >&2
    exit 1
}
grep -Fq 'PACKET_POOL_VERSION' \
    kernel/include/packet_pool.h || {
    echo "packet_pool.h не содержит PACKET_POOL_VERSION" >&2
    exit 1
}
grep -Fq 'PACKET_POOL_COUNT_MAX' \
    kernel/include/packet_pool.h || {
    echo "packet_pool.h не содержит PACKET_POOL_COUNT_MAX" >&2
    exit 1
}
grep -Fq 'PACKET_POOL_CAPACITY_MAX' \
    kernel/include/packet_pool.h || {
    echo "packet_pool.h не содержит PACKET_POOL_CAPACITY_MAX" >&2
    exit 1
}
grep -Fq 'PACKET_POOL_HEADROOM_MAX' \
    kernel/include/packet_pool.h || {
    echo "packet_pool.h не содержит PACKET_POOL_HEADROOM_MAX" >&2
    exit 1
}
grep -Fq 'struct packet_pool_descriptor' \
    kernel/include/packet_pool.h || {
    echo "packet_pool.h не содержит struct packet_pool_descriptor" >&2
    exit 1
}
grep -Fq 'struct packet_pool_slot_descriptor' \
    kernel/include/packet_pool.h || {
    echo "packet_pool.h не содержит struct packet_pool_slot_descriptor" >&2
    exit 1
}
for packet_pool_symbol in packet_pool_init \
                          packet_pool_is_published \
                          packet_pool_count \
                          packet_pool_capacity \
                          packet_pool_headroom \
                          packet_pool_length \
                          packet_pool_storage_size \
                          packet_pool_slot \
                          packet_pool_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $packet_pool_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $packet_pool_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/packet_pool.c; then
    echo "packet_pool.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'packet_pool_host_test_'; then
    echo "packet_pool_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Packet pool descriptor host tests: ALL OK'; then
    echo "Production kernel содержит packet pool host marker" >&2
    exit 1
fi
for packet_pool_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'packet pool descriptor|packet-pool descriptor' \
            "$packet_pool_doc"; then
        echo "$packet_pool_doc не документирует packet pool descriptor" >&2
        exit 1
    fi
done

# Packet acquire/release lifecycle contracts.
grep -Fq 'test-packet-lifecycle-host' Makefile || {
    echo "Makefile не содержит test-packet-lifecycle-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-packet-lifecycle-host' Makefile || {
    echo "test: target не содержит test-packet-lifecycle-host" >&2
    exit 1
}
grep -Fq 'PACKET_POOL_OWNER_ACTIVE' \
    kernel/include/packet_pool.h || {
    echo "packet_pool.h не содержит PACKET_POOL_OWNER_ACTIVE" >&2
    exit 1
}
grep -Fq 'PACKET_POOL_EXHAUSTED' \
    kernel/include/packet_pool.h || {
    echo "packet_pool.h не содержит PACKET_POOL_EXHAUSTED" >&2
    exit 1
}
for packet_lifecycle_symbol in packet_pool_reserve \
                               packet_pool_commit \
                               packet_pool_transfer \
                               packet_pool_release \
                               packet_pool_free_count \
                               packet_pool_reserved_count \
                               packet_pool_active_count; do
    nm build/hfoc-kernel.elf | grep -F " $packet_lifecycle_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $packet_lifecycle_symbol" >&2
        exit 1
    }
done
if nm -g build/hfoc-kernel.elf | grep 'packet_lifecycle_host_test_'; then
    echo "packet_lifecycle_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Packet acquire/release lifecycle host tests: ALL OK'; then
    echo "Production kernel содержит packet lifecycle host marker" >&2
    exit 1
fi
for packet_lifecycle_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'packet.*lifecycle|packet-pool lifecycle' \
            "$packet_lifecycle_doc"; then
        echo "$packet_lifecycle_doc не документирует packet lifecycle" >&2
        exit 1
    fi
done

# Packet slice and cursor contracts.
grep -Fq 'test-packet-slice-host' Makefile || {
    echo "Makefile не содержит test-packet-slice-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-packet-slice-host' Makefile || {
    echo "test: target не содержит test-packet-slice-host" >&2
    exit 1
}
grep -Fq 'PACKET_SLICE_VERSION' \
    kernel/include/packet_slice.h || {
    echo "packet_slice.h не содержит PACKET_SLICE_VERSION" >&2
    exit 1
}
grep -Fq 'struct packet_slice' \
    kernel/include/packet_slice.h || {
    echo "packet_slice.h не содержит struct packet_slice" >&2
    exit 1
}
grep -Fq 'PACKET_SLICE_OVERLAP' \
    kernel/include/packet_slice.h || {
    echo "packet_slice.h не содержит PACKET_SLICE_OVERLAP" >&2
    exit 1
}
if grep -nE '\b(void|uint8_t|unsigned char)\s*\*\s*packet_slice_' \
        kernel/include/packet_slice.h; then
    echo "packet_slice.h возвращает указатель на storage" >&2
    exit 1
fi
if grep -Fq 'packet_slice_data' kernel/include/packet_slice.h; then
    echo "packet_slice.h содержит pointer-escape API" >&2
    exit 1
fi
for packet_slice_symbol in packet_slice_init \
                           packet_slice_is_valid \
                           packet_slice_prepend \
                           packet_slice_append \
                           packet_slice_trim \
                           packet_slice_read \
                           packet_slice_write \
                           packet_slice_seek \
                           packet_slice_start \
                           packet_slice_length \
                           packet_slice_cursor \
                           packet_slice_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $packet_slice_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $packet_slice_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/packet_slice.c; then
    echo "packet_slice.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'packet_slice_(init|append|prepend|trim|read|write|seek)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Packet slice operations вызываются из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'packet_slice_host_test_'; then
    echo "packet_slice_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Packet slice and cursor host tests: ALL OK'; then
    echo "Production kernel содержит packet slice host marker" >&2
    exit 1
fi
for packet_slice_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'packet.*slice|cursor api' "$packet_slice_doc"; then
        echo "$packet_slice_doc не документирует packet slice/cursor" >&2
        exit 1
    fi
done

# Ethernet frame validator contracts.
grep -Fq 'test-ethernet-frame-host' Makefile || {
    echo "Makefile не содержит test-ethernet-frame-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-ethernet-frame-host' Makefile || {
    echo "test: target не содержит test-ethernet-frame-host" >&2
    exit 1
}
grep -Fq 'ETHERNET_FRAME_VERSION' \
    kernel/include/ethernet_frame.h || {
    echo "ethernet_frame.h не содержит ETHERNET_FRAME_VERSION" >&2
    exit 1
}
grep -Fq 'ETHERNET_FRAME_MIN_LENGTH' \
    kernel/include/ethernet_frame.h || {
    echo "ethernet_frame.h не содержит ETHERNET_FRAME_MIN_LENGTH" >&2
    exit 1
}
grep -Fq 'ETHERNET_FRAME_MAX_LENGTH' \
    kernel/include/ethernet_frame.h || {
    echo "ethernet_frame.h не содержит ETHERNET_FRAME_MAX_LENGTH" >&2
    exit 1
}
grep -Fq 'ETHERNET_FRAME_PADDING_MAX' \
    kernel/include/ethernet_frame.h || {
    echo "ethernet_frame.h не содержит ETHERNET_FRAME_PADDING_MAX" >&2
    exit 1
}
grep -Fq 'ETHERNET_FRAME_ETHERTYPE_VLAN' \
    kernel/include/ethernet_frame.h || {
    echo "ethernet_frame.h не содержит ETHERNET_FRAME_ETHERTYPE_VLAN" >&2
    exit 1
}
grep -Fq 'struct ethernet_frame_policy' \
    kernel/include/ethernet_frame.h || {
    echo "ethernet_frame.h не содержит struct ethernet_frame_policy" >&2
    exit 1
}
grep -Fq 'struct ethernet_frame_view' \
    kernel/include/ethernet_frame.h || {
    echo "ethernet_frame.h не содержит struct ethernet_frame_view" >&2
    exit 1
}
grep -Fq 'ETHERNET_FRAME_DESTINATION_MULTICAST' \
    kernel/include/ethernet_frame.h || {
    echo "ethernet_frame.h не содержит multicast destination" >&2
    exit 1
}
if grep -nE '\b(void|uint8_t|unsigned char)\s*\*\s*ethernet_frame_' \
        kernel/include/ethernet_frame.h; then
    echo "ethernet_frame.h возвращает указатель на frame storage" >&2
    exit 1
fi
if grep -Fq 'ethernet_frame_data' kernel/include/ethernet_frame.h; then
    echo "ethernet_frame.h содержит pointer-escape API" >&2
    exit 1
fi
for ethernet_frame_symbol in ethernet_frame_validate \
                             ethernet_frame_status_string \
                             ethernet_frame_destination_string; do
    nm build/hfoc-kernel.elf | grep -F " $ethernet_frame_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $ethernet_frame_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/ethernet_frame.c; then
    echo "ethernet_frame.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'ethernet_frame_validate\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Ethernet frame validation вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'ethernet_frame_host_test_'; then
    echo "ethernet_frame_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Ethernet frame host tests: ALL OK'; then
    echo "Production kernel содержит Ethernet frame host marker" >&2
    exit 1
fi
for ethernet_frame_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'ethernet.*frame|frame validator' \
            "$ethernet_frame_doc"; then
        echo "$ethernet_frame_doc не документирует Ethernet frame validator" >&2
        exit 1
    fi
done

# Virtio net MAC identity publication contracts.
grep -Fq 'test-virtio-net-mac-identity-host' Makefile || {
    echo "Makefile не содержит test-virtio-net-mac-identity-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-virtio-net-mac-identity-host' Makefile || {
    echo "test: target не содержит test-virtio-net-mac-identity-host" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_MAC_IDENTITY_VERSION' \
    kernel/include/virtio_net_mac_identity.h || {
    echo "virtio_net_mac_identity.h не содержит VIRTIO_NET_MAC_IDENTITY_VERSION" >&2
    exit 1
}
grep -Fq 'struct virtio_net_mac_identity' \
    kernel/include/virtio_net_mac_identity.h || {
    echo "virtio_net_mac_identity.h не содержит struct virtio_net_mac_identity" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_MAC_IDENTITY_MULTICAST_MAC' \
    kernel/include/virtio_net_mac_identity.h || {
    echo "virtio_net_mac_identity.h не содержит multicast status" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_MAC_IDENTITY_DEVICE_MISMATCH' \
    kernel/include/virtio_net_mac_identity.h || {
    echo "virtio_net_mac_identity.h не содержит device mismatch status" >&2
    exit 1
}
grep -Fq 'VIRTIO_NET_MAC_IDENTITY_CHANGED' \
    kernel/include/virtio_net_mac_identity.h || {
    echo "virtio_net_mac_identity.h не содержит changed status" >&2
    exit 1
}
for mac_identity_symbol in virtio_net_mac_identity_bind \
                           virtio_net_mac_identity_validate_state \
                           virtio_net_mac_identity_published \
                           virtio_net_mac_identity_device_selection \
                           virtio_net_mac_identity_profile \
                           virtio_net_mac_identity_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $mac_identity_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $mac_identity_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/virtio_net_mac_identity.c; then
    echo "virtio_net_mac_identity.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'virtio_net_mac_identity_bind\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "MAC identity publication вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'virtio_net_mac_identity_host_test_'; then
    echo "virtio_net_mac_identity_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Virtio net MAC identity host tests: ALL OK'; then
    echo "Production kernel содержит MAC identity host marker" >&2
    exit 1
fi
for mac_identity_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'mac.*identity|MAC identity' \
            "$mac_identity_doc"; then
        echo "$mac_identity_doc не документирует MAC identity publication" >&2
        exit 1
    fi
done

for ipv4_profile_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq \
            'Static QEMU IPv4 profile binding|static QEMU IPv4 profile binding|IPv4 profile binding' \
            "$ipv4_profile_doc"; then
        echo "$ipv4_profile_doc не документирует static QEMU IPv4 profile" >&2
        exit 1
    fi
done

# Static QEMU IPv4 profile binding contracts.
grep -Fq 'test-network-ipv4-profile-host' Makefile || {
    echo "Makefile не содержит test-network-ipv4-profile-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-network-ipv4-profile-host' Makefile || {
    echo "host suite не содержит test-network-ipv4-profile-host" >&2
    exit 1
}
grep -Fq 'NETWORK_IPV4_PROFILE_VERSION' \
    kernel/include/network_ipv4_profile.h || {
    echo "network_ipv4_profile.h не содержит NETWORK_IPV4_PROFILE_VERSION" >&2
    exit 1
}
grep -Fq 'struct network_ipv4_profile' \
    kernel/include/network_ipv4_profile.h || {
    echo "network_ipv4_profile.h не содержит struct network_ipv4_profile" >&2
    exit 1
}
grep -Fq 'NETWORK_IPV4_PROFILE_QEMU_PORT' \
    kernel/include/network_ipv4_profile.h || {
    echo "network_ipv4_profile.h не содержит QEMU port constant" >&2
    exit 1
}
grep -Fq 'NETWORK_IPV4_PROFILE_IDENTITY_UNPUBLISHED' \
    kernel/include/network_ipv4_profile.h || {
    echo "network_ipv4_profile.h не содержит identity status" >&2
    exit 1
}
for ipv4_profile_symbol in network_ipv4_port_policy_validate \
                           network_ipv4_profile_init \
                           network_ipv4_profile_stage \
                           network_ipv4_profile_commit \
                           network_ipv4_profile_rollback \
                           network_ipv4_profile_validate_state \
                           network_ipv4_profile_published \
                           network_ipv4_profile_active_config \
                           network_ipv4_profile_identity \
                           network_ipv4_profile_port_policy \
                           network_ipv4_profile_has_pending \
                           network_ipv4_profile_pending_config \
                           network_ipv4_profile_pending_identity \
                           network_ipv4_profile_pending_port_policy \
                           network_ipv4_profile_qemu_fill \
                           network_ipv4_profile_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $ipv4_profile_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $ipv4_profile_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/network_ipv4_profile.c; then
    echo "network_ipv4_profile.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'network_ipv4_profile_(init|stage|commit|rollback)\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "Static IPv4 profile binding вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'network_ipv4_profile_host_test_'; then
    echo "network_ipv4_profile_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Static QEMU IPv4 profile host tests: ALL OK'; then
    echo "Production kernel содержит IPv4 profile host marker" >&2
    exit 1
fi

# HFOC netif input contract contracts.
grep -Fq 'test-hfoc-netif-input-host' Makefile || {
    echo "Makefile не содержит test-hfoc-netif-input-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-hfoc-netif-input-host' Makefile || {
    echo "host suite не содержит test-hfoc-netif-input-host" >&2
    exit 1
}
grep -Fq 'HFOC_NETIF_INPUT_VERSION' \
    kernel/include/hfoc_netif_input.h || {
    echo "hfoc_netif_input.h не содержит HFOC_NETIF_INPUT_VERSION" >&2
    exit 1
}
grep -Fq 'struct hfoc_netif_input' \
    kernel/include/hfoc_netif_input.h || {
    echo "hfoc_netif_input.h не содержит struct hfoc_netif_input" >&2
    exit 1
}
grep -Fq 'HFOC_NETIF_INPUT_RELEASE_FAILED' \
    kernel/include/hfoc_netif_input.h || {
    echo "hfoc_netif_input.h не содержит release-failed status" >&2
    exit 1
}
if grep -nE '\b(void|uint8_t|unsigned char)\s*\*\s*hfoc_netif_' \
        kernel/include/hfoc_netif_input.h; then
    echo "hfoc_netif_input.h возвращает указатель на storage" >&2
    exit 1
fi
for hfoc_netif_input_symbol in hfoc_netif_input_init \
                               hfoc_netif_input_transfer \
                               hfoc_netif_input_published \
                               hfoc_netif_input_result_string \
                               hfoc_netif_input_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $hfoc_netif_input_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $hfoc_netif_input_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/hfoc_netif_input.c; then
    echo "hfoc_netif_input.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if grep -nE 'hfoc_netif_input_transfer\(' \
        kernel/src/arch/x86_64/interrupts.c; then
    echo "HFOC netif input transfer вызывается из interrupt path" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'hfoc_netif_input_host_test_'; then
    echo "hfoc_netif_input_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'HFOC netif input host tests: ALL OK'; then
    echo "Production kernel содержит netif input host marker" >&2
    exit 1
fi
for hfoc_netif_input_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'HFOC netif input contract|netif input contract' \
            "$hfoc_netif_input_doc"; then
        echo "$hfoc_netif_input_doc не документирует HFOC netif input contract" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **227. HFOC netif input contract' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "HTTPS queue plan не помечает item 227 завершённым" >&2
    exit 1
}
