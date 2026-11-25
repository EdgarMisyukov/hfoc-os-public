#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# HTTP deterministic response encoder production isolation.
# ITEM-386: the production serving path encodes every response with this
# encoder, so the symbols MUST be present.  The host-test marker below stays
# forbidden.
for http_response_symbol in \
    http_response_encode \
    http_response_encode_head \
    http_response_length; do
    nm build/hfoc-kernel.elf | grep -F " $http_response_symbol" >/dev/null || {
        echo "Production kernel не содержит $http_response_symbol" >&2
        exit 1
    }
done
if strings build/hfoc-kernel.elf | grep -F \
        'HTTP response encoder host tests'; then
    echo "Production kernel содержит HTTP response encoder host marker" >&2
    exit 1
fi

# HTTP error matrix production isolation.
# ITEM-386: the ordinary production boot serves this component itself,
# so its symbols MUST be present in the production ELF. The old
# prohibition is replaced by this positive contract; the host-test
# marker that follows stays forbidden.
for http_error_matrix_symbol in \
    http_error_matrix_map_parser \
    http_error_matrix_map_framing \
    http_error_matrix_map_route; do
    nm build/hfoc-kernel.elf | grep -F " $http_error_matrix_symbol" >/dev/null || {
        echo "Production kernel не содержит $http_error_matrix_symbol" >&2
        exit 1
    }
done
if strings build/hfoc-kernel.elf | grep -F \
        'HTTP error matrix host tests'; then
    echo "Production kernel содержит HTTP error matrix host marker" >&2
    exit 1
fi

# HTTP parser/encoder host acceptance production isolation.
if strings build/hfoc-kernel.elf | grep -F \
        'HTTP parser/encoder host acceptance: ALL OK'; then
    echo "Production kernel содержит HTTP parser/encoder acceptance marker" >&2
    exit 1
fi

# lwIP allocator production boundary.
for allocator_symbol in hfoc_lwip_alloc_init hfoc_lwip_alloc_shutdown \
                        hfoc_lwip_alloc hfoc_lwip_alloc_calloc \
                        hfoc_lwip_alloc_free hfoc_lwip_mem_malloc \
                        hfoc_lwip_mem_calloc hfoc_lwip_mem_free; do
    nm build/hfoc-kernel.elf | grep -F " $allocator_symbol" >/dev/null || {
        echo "Production kernel не содержит $allocator_symbol" >&2
        exit 1
    }
done
if nm -g build/hfoc-kernel.elf | grep 'hfoc_lwip_host_test_'; then
    echo "lwIP allocator host test symbols попали в kernel ELF" >&2
    exit 1
fi

# mbedTLS allocator production boundary.
for mbedtls_alloc_symbol in hfoc_mbedtls_alloc_init \
                            hfoc_mbedtls_alloc_shutdown \
                            hfoc_mbedtls_alloc \
                            hfoc_mbedtls_calloc \
                            hfoc_mbedtls_alloc_free \
                            hfoc_mbedtls_free \
                            hfoc_mbedtls_alloc_connection_open \
                            hfoc_mbedtls_alloc_connection_close; do
    nm build/hfoc-kernel.elf | grep -F " $mbedtls_alloc_symbol" >/dev/null || {
        echo "Production kernel не содержит $mbedtls_alloc_symbol" >&2
        exit 1
    }
done
if nm -g build/hfoc-kernel.elf | grep 'hfoc_mbedtls_alloc_host_test_'; then
    echo "mbedTLS allocator host test symbols попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'mbedTLS allocator boundary host tests: ALL OK'; then
    echo "Production kernel содержит mbedTLS allocator host marker" >&2
    exit 1
fi

# mbedTLS entropy/DRBG production boundary.
# ITEM-386: the ordinary production boot serves this component itself,
# so its symbols MUST be present in the production ELF. The old
# prohibition is replaced by this positive contract; the host-test
# marker that follows stays forbidden.
for mbedtls_drbg_symbol in \
    hfoc_mbedtls_drbg_publish \
    hfoc_mbedtls_drbg_generate; do
    nm build/hfoc-kernel.elf | grep -F " $mbedtls_drbg_symbol" >/dev/null || {
        echo "Production kernel не содержит $mbedtls_drbg_symbol" >&2
        exit 1
    }
done
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'mbedTLS DRBG adapter host tests: ALL OK'; then
    echo "Production kernel содержит mbedTLS DRBG host marker" >&2
    exit 1
fi

# mbedTLS certificate/key manifest production boundary.
if nm -g build/hfoc-kernel.elf | grep -F 'hfoc_mbedtls_manifest_'; then
    echo "Production kernel не должен содержать host-only manifest parser symbols" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'mbedTLS manifest parser host tests: ALL OK'; then
    echo "Production kernel содержит mbedTLS manifest host marker" >&2
    exit 1
fi

# mbedTLS BIO transport production boundary.
# ITEM-386: the ordinary production boot serves this component itself,
# so its symbols MUST be present in the production ELF. The old
# prohibition is replaced by this positive contract; the host-test
# marker that follows stays forbidden.
for mbedtls_bio_symbol in \
    hfoc_mbedtls_bio_init \
    hfoc_mbedtls_bio_attach \
    hfoc_mbedtls_bio_send_cb; do
    nm build/hfoc-kernel.elf | grep -F " $mbedtls_bio_symbol" >/dev/null || {
        echo "Production kernel не содержит $mbedtls_bio_symbol" >&2
        exit 1
    }
done
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'mbedTLS BIO transport host tests: ALL OK'; then
    echo "Production kernel содержит mbedTLS BIO host marker" >&2
    exit 1
fi

# TLS handshake state machine production boundary.
if nm -g build/hfoc-kernel.elf | grep -F 'hfoc_tls_handshake_'; then
    echo "Production kernel не должен содержать host-only TLS handshake symbols" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'TLS handshake state machine host tests: ALL OK'; then
    echo "Production kernel содержит TLS handshake host marker" >&2
    exit 1
fi

# TLS close and error normalization production boundary.
if nm -g build/hfoc-kernel.elf | grep -F 'hfoc_tls_close_'; then
    echo "Production kernel не должен содержать host-only TLS close symbols" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'TLS close and error normalization host tests: ALL OK'; then
    echo "Production kernel содержит TLS close host marker" >&2
    exit 1
fi

# lwIP monotonic time production boundary.
for time_symbol in hfoc_lwip_time_init_default hfoc_lwip_time_init \
                   hfoc_lwip_time_now hfoc_lwip_time_arm \
                   hfoc_lwip_time_disarm \
                   hfoc_lwip_time_teardown_generation \
                   hfoc_lwip_time_poll hfoc_lwip_time_next_deadline_ms \
                   hfoc_lwip_time_snapshot sys_now; do
    nm build/hfoc-kernel.elf | grep -F " $time_symbol" >/dev/null || {
        echo "Production kernel не содержит $time_symbol" >&2
        exit 1
    }
done
if nm -g build/hfoc-kernel.elf | grep 'hfoc_lwip_time_host_test_'; then
    echo "lwIP monotonic time host test symbols попали в kernel ELF" >&2
    exit 1
fi

# lwIP pbuf bridge production boundary.
for pbuf_symbol in hfoc_lwip_pbuf_bridge_init \
                   hfoc_lwip_pbuf_bridge_shutdown \
                   hfoc_lwip_pbuf_bridge_published \
                   hfoc_lwip_pbuf_bridge_used_count \
                   hfoc_lwip_pbuf_bridge_free_count \
                   hfoc_lwip_pbuf_import_rx \
                   hfoc_lwip_pbuf_export_tx \
                   hfoc_lwip_pbuf_release_chain \
                   hfoc_lwip_pbuf_result_string \
                   hfoc_lwip_pbuf_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $pbuf_symbol" >/dev/null || {
        echo "Production kernel не содержит $pbuf_symbol" >&2
        exit 1
    }
done
if nm -g build/hfoc-kernel.elf | grep 'hfoc_lwip_pbuf_host_test_'; then
    echo "lwIP pbuf bridge host test symbols попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'lwIP pbuf/packet ownership bridge host tests: ALL OK'; then
    echo "Production kernel содержит pbuf bridge host marker" >&2
    exit 1
fi
for pbuf_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'lwip pbuf/packet ownership bridge|pbuf/packet ownership bridge' \
            "$pbuf_doc"; then
        echo "$pbuf_doc не документирует lwIP pbuf/packet ownership bridge" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **235. lwIP pbuf/packet ownership bridge' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 235 завершённым" >&2
    exit 1
}

# lwIP netif lifecycle production boundary.
for netif_symbol in hfoc_lwip_netif_init \
                    hfoc_lwip_netif_stage \
                    hfoc_lwip_netif_activate \
                    hfoc_lwip_netif_remove \
                    hfoc_lwip_netif_shutdown \
                    hfoc_lwip_netif_published \
                    hfoc_lwip_netif_live \
                    hfoc_lwip_netif_staged \
                    hfoc_lwip_netif_generation \
                    hfoc_lwip_netif_config \
                    hfoc_lwip_netif_lwip_netif \
                    hfoc_lwip_netif_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $netif_symbol" >/dev/null || {
        echo "Production kernel не содержит $netif_symbol" >&2
        exit 1
    }
done
if nm -g build/hfoc-kernel.elf | grep 'hfoc_lwip_netif_host_test_'; then
    echo "lwIP netif lifecycle host test symbols попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'lwIP netif lifecycle host tests: ALL OK'; then
    echo "Production kernel содержит netif lifecycle host marker" >&2
    exit 1
fi
for netif_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'lwip netif lifecycle|netif lifecycle' \
            "$netif_doc"; then
        echo "$netif_doc не документирует lwIP netif lifecycle" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **236. lwIP netif lifecycle' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 236 завершённым" >&2
    exit 1
}

# lwIP ARP/ICMP protocol integration contracts (item 237).
grep -Fq 'test-lwip-arp-icmp-host' Makefile || {
    echo "Makefile не содержит test-lwip-arp-icmp-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-lwip-arp-icmp-host' Makefile || {
    echo "test-host-suite не содержит test-lwip-arp-icmp-host" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_PROTO_VERSION 1U' \
    kernel/include/hfoc_lwip_proto.h || {
    echo "hfoc_lwip_proto.h не содержит HFOC_LWIP_PROTO_VERSION 1U" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_PROTO_FRAME_MAX 1518U' \
    kernel/include/hfoc_lwip_proto.h || {
    echo "hfoc_lwip_proto.h не содержит HFOC_LWIP_PROTO_FRAME_MAX 1518U" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_PROTO_ICMP_BUDGET 4U' \
    kernel/include/hfoc_lwip_proto.h || {
    echo "hfoc_lwip_proto.h не содержит HFOC_LWIP_PROTO_ICMP_BUDGET 4U" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_PROTO_ICMP_WINDOW_MS 1000U' \
    kernel/include/hfoc_lwip_proto.h || {
    echo "hfoc_lwip_proto.h не содержит HFOC_LWIP_PROTO_ICMP_WINDOW_MS 1000U" >&2;
    exit 1; }
grep -Fq 'hfoc_lwip_proto_init' kernel/src/hfoc_lwip_proto.c || {
    echo "hfoc_lwip_proto.c не реализует init" >&2; exit 1; }
grep -Fq 'hfoc_lwip_proto_input' kernel/src/hfoc_lwip_proto.c || {
    echo "hfoc_lwip_proto.c не реализует input" >&2; exit 1; }
grep -Fq 'hfoc_lwip_proto_lookup' kernel/src/hfoc_lwip_proto.c || {
    echo "hfoc_lwip_proto.c не реализует ARP lookup" >&2; exit 1; }
grep -Fq 'hfoc_lwip_proto_shutdown' kernel/src/hfoc_lwip_proto.c || {
    echo "hfoc_lwip_proto.c не реализует shutdown" >&2; exit 1; }
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/hfoc_lwip_proto.c; then
    echo "hfoc_lwip_proto.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
grep -Fq 'test-arp-icmp-qemu' Makefile || {
    echo "Makefile не содержит test-arp-icmp-qemu" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-arp-icmp-qemu' Makefile || {
    echo "test-serial/test не содержит test-arp-icmp-qemu" >&2;
    exit 1; }
grep -Fq 'scripts/test-qemu-arp-icmp.sh' Makefile || {
    echo "Makefile не содержит ARP/ICMP QEMU script" >&2; exit 1; }
grep -Fq 'HFOC_ARP_ICMP_QEMU_TEST' \
    kernel/src/arp_icmp_qemu.c || {
    echo "arp_icmp_qemu.c не содержит HFOC_ARP_ICMP_QEMU_TEST gate" >&2;
    exit 1; }
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[ARP-ICMP]'; then
    echo "Production kernel содержит ARP/ICMP marker" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
    grep -E ' (arp_icmp_qemu_test_init)'; then
    echo "Production kernel содержит ARP/ICMP proof symbol" >&2
    exit 1
fi
# ITEM-362A promotes the frozen lwIP ARP/ICMP protocol boundary into the
# ordinary-boot production NIC backend. The production kernel must publish
# hfoc_lwip_proto_* and the production network owner, but it must not publish
# the QEMU ARP/ICMP proof initializer.
for production_network_symbol in \
    hfoc_lwip_proto_init \
    hfoc_lwip_proto_input \
    hfoc_lwip_proto_lookup \
    hfoc_lwip_proto_shutdown \
    appliance_production_network_init \
    appliance_production_network_bind \
    appliance_production_network_input \
    appliance_production_network_boot_init; do
    nm build/hfoc-kernel.elf | grep -F " $production_network_symbol" >/dev/null || {
        echo "Production kernel не содержит $production_network_symbol" >&2
        exit 1
    }
done
if nm build/hfoc-kernel.elf | grep 'appliance_production_network_host_test_'; then
    echo "Production kernel содержит production-network host-only symbols" >&2
    exit 1
fi

# ITEM-362A ordinary-boot production NIC/lwIP backend contracts.
grep -Fq 'test-appliance-production-network-host' Makefile || {
    echo "Makefile не содержит test-appliance-production-network-host" >&2
    exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-appliance-production-network-host' Makefile || {
    echo "test-host-suite не содержит test-appliance-production-network-host" >&2
    exit 1; }
grep -Fq 'test-production-network-qemu' Makefile || {
    echo "Makefile не содержит test-production-network-qemu" >&2
    exit 1; }
grep -Fq 'scripts/test-qemu-production-network.sh' Makefile || {
    echo "Makefile не содержит production-network QEMU script" >&2
    exit 1; }
grep -Fq 'APPLIANCE_PRODUCTION_NETWORK_VERSION 1U' \
    kernel/include/appliance_production_network.h || {
    echo "appliance_production_network.h не содержит версию 1U" >&2
    exit 1; }
for production_network_contract in \
    appliance_production_network_bind \
    appliance_production_network_input; do
    grep -Fq "$production_network_contract" \
        kernel/src/appliance_production_network.c || {
        echo "appliance_production_network.c не реализует $production_network_contract" >&2
        exit 1
    }
done
grep -Fq 'HFOC_PRODUCTION_LWIP' kernel/src/hfoc_lwip_proto.c || {
    echo "hfoc_lwip_proto.c не включает production gate" >&2
    exit 1; }
grep -Fq 'HFOC_PRODUCTION_NETWORK' \
    kernel/src/appliance_production_network_x86.c || {
    echo "appliance_production_network_x86.c не включает production gate" >&2
    exit 1; }
# ITEM-393: the RTL8168 backend owns two bases and they are not
# interchangeable. The mapped MMIO register window is reachable only through the
# apn_rtl_read*/write* accessors; every descriptor ring and frame buffer is
# addressed from the DMA ring allocation. On the acceptance host a transmit
# buffer was derived from the MMIO window, and the memset that zeroes it faulted
# on physical 0xFC000000, inside the machine's PCI aperture.
grep -Fq 'apn.rtl.dma_ring = base;' \
    kernel/src/appliance_production_network_x86.c || {
    echo "appliance_production_network_x86.c не публикует базу DMA-кольца" >&2
    exit 1; }
grep -Fq 'buffer = apn.rtl.dma_ring + layout.tx_buffer_offset' \
    kernel/src/appliance_production_network_x86.c || {
    echo "передающий буфер не адресуется из DMA-кольца" >&2
    exit 1; }
grep -Fq 'apn.rtl.dma_ring + layout.rx_buffer_offset' \
    kernel/src/appliance_production_network_x86.c || {
    echo "приёмный буфер не адресуется из DMA-кольца" >&2
    exit 1; }
for rtl_retired_base in \
    '(uint8_t *)apn.rtl.region + layout.tx_buffer_offset' \
    '(const uint8_t *)apn.rtl.region + layout.rx_buffer_offset'; do
    if grep -Fq "$rtl_retired_base" \
            kernel/src/appliance_production_network_x86.c; then
        echo "буфер кольца адресуется из окна MMIO: $rtl_retired_base" >&2
        exit 1
    fi
done
if grep -Eq '\b(dhcp_|dns_|tcp_|udp_|mbedtls_)\b' \
        kernel/src/appliance_production_network.c; then
    echo "appliance_production_network.c расширяет scope beyond NIC/lwIP" >&2
    exit 1
fi
[[ -f docs/hfoc-production-nic-lwip-backend.md ]] || {
    echo "missing ITEM-362A production NIC/lwIP document" >&2
    exit 1; }
for production_network_marker in \
    '[PRODUCTION-NET]' \
    'QEMU production network test: OK'; do
    if [[ "$production_network_marker" == '[PRODUCTION-NET]' ]]; then
        grep -Fq "$production_network_marker" kernel/src/kernel.c || {
            echo "kernel.c lacks $production_network_marker" >&2
            exit 1
        }
    else
        grep -Fq "$production_network_marker" \
            scripts/test-qemu-production-network.sh || {
            echo "test-qemu-production-network.sh lacks $production_network_marker" >&2
            exit 1
        }
    fi
done
for production_network_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'production NIC/lwIP|production network backend|production nic' \
            "$production_network_doc"; then
        echo "$production_network_doc не документирует production NIC/lwIP backend" >&2
        exit 1
    fi
done
if ! grep -Fq 'qemu-production-network' .github/workflows/ci.yml; then
    echo "CI pgrep guard не содержит production-network script" >&2
    exit 1
fi
if ! strings build/hfoc-kernel.elf | grep -F '[PRODUCTION-NET]' \
        >/dev/null 2>&1; then
    echo "Production kernel не содержит production-network marker" >&2
    exit 1
fi

# ITEM-388 production NIC ring-cycle proof and ICMP budget closure. The proof
# markers and the header-derived budget are the enforced evidence that the
# paced cycle and the reachability policy stay asserted.
grep -Fq 'Production ARP ring cycle:' \
    scripts/test-qemu-production-network.sh || {
    echo "test-qemu-production-network.sh не содержит paced ARP ring cycle" >&2
    exit 1; }
grep -Fq 'Production ICMP paced cycle:' \
    scripts/test-qemu-production-network.sh || {
    echo "test-qemu-production-network.sh не содержит paced ICMP cycle" >&2
    exit 1; }
grep -Fq 'Production ICMP burst budget:' \
    scripts/test-qemu-production-network.sh || {
    echo "test-qemu-production-network.sh не содержит ICMP budget burst" >&2
    exit 1; }
grep -Fq 'HFOC_PRODUCTION_NETWORK_ARP_CYCLE_COUNT' \
    scripts/test-qemu-production-network.sh || {
    echo "test-qemu-production-network.sh не параметризует ARP ring cycle" >&2
    exit 1; }
grep -Fq 'HFOC_LWIP_PROTO_ICMP_BUDGET' \
    scripts/test-qemu-production-network.sh || {
    echo "test-qemu-production-network.sh не выводит ICMP budget из заголовка" >&2
    exit 1; }
grep -Fq 'HFOC_LWIP_PROTO_ICMP_WINDOW_MS' docs/ARCHITECTURE.md || {
    echo "ARCHITECTURE.md не документирует ICMP rate-limit окно" >&2
    exit 1; }
grep -Fq 'ITEM-388' docs/ROADMAP.md || {
    echo "ROADMAP.md не закрывает ITEM-388 ring-cycle вопрос" >&2
    exit 1; }

# ITEM-364 ordinary-boot production service/HTTPS activation contracts.
for production_service_symbol in \
    appliance_production_service_activation_init \
    appliance_production_service_activation_evaluate \
    appliance_production_service_activation_poll \
    appliance_production_service_activation_restart \
    appliance_production_service_boot_init; do
    nm build/hfoc-kernel.elf | grep -F " $production_service_symbol" >/dev/null || {
        echo "Production kernel не содержит $production_service_symbol" >&2
        exit 1
    }
done
if nm build/hfoc-kernel.elf | grep 'appliance_production_service_activation_host_test_'; then
    echo "Production kernel содержит production-service host-only symbols" >&2
    exit 1
fi
grep -Fq 'test-appliance-production-service-activation-host' Makefile || {
    echo "Makefile не содержит test-appliance-production-service-activation-host" >&2
    exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-appliance-production-service-activation-host' Makefile || {
    echo "test-host-suite не содержит test-appliance-production-service-activation-host" >&2
    exit 1; }
grep -Fq 'test-production-service-https-qemu' Makefile || {
    echo "Makefile не содержит test-production-service-https-qemu" >&2
    exit 1; }
grep -Fq 'scripts/test-qemu-production-service-https.sh' Makefile || {
    echo "Makefile не содержит production service QEMU script" >&2
    exit 1; }
grep -Fq 'APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_VERSION 1U' \
    kernel/include/appliance_production_service_activation.h || {
    echo "appliance_production_service_activation.h не содержит версию 1U" >&2
    exit 1; }
for production_service_contract in \
    appliance_production_service_activation_configure \
    appliance_production_service_activation_evaluate \
    appliance_production_service_activation_poll; do
    grep -Fq "$production_service_contract" \
        kernel/src/appliance_production_service_activation.c || {
        echo "appliance_production_service_activation.c не реализует $production_service_contract" >&2
        exit 1
    }
done
grep -Fq 'HFOC_PRODUCTION_SERVICES' Makefile || {
    echo "PRODUCTION_CFLAGS не включает production service gate" >&2
    exit 1; }
grep -Fq 'HFOC_PRODUCTION_SERVICES' \
    kernel/src/appliance_production_service_boot.c || {
    echo "appliance_production_service_boot.c не включает production gate" >&2
    exit 1; }
if grep -Eq '\b(mbedtls_|https_handler|management_command)\b' \
        kernel/src/appliance_production_service_activation.c; then
    echo "appliance_production_service_activation.c расширяет scope beyond activation" >&2
    exit 1
fi
[[ -f docs/hfoc-production-service-https-activation.md ]] || {
    echo "missing ITEM-364 production service/HTTPS activation document" >&2
    exit 1; }
grep -Fq '[PRODUCTION-SERVICE]' kernel/src/kernel.c || {
    echo "kernel.c lacks [PRODUCTION-SERVICE] marker" >&2
    exit 1; }
# ITEM-587: the canonical marker belongs to the aggregate target, which prints
# it through the summary script only after both scheduling parts succeeded.
grep -Fq 'QEMU production service activation test: OK' \
    scripts/test-production-service-https-summary.sh || {
    echo "test-production-service-https-summary.sh lacks success marker" >&2
    exit 1; }
if ! grep -Fq 'qemu-production-service-https' .github/workflows/ci.yml; then
    echo "CI pgrep guard не содержит production service script" >&2
    exit 1
fi
for production_service_doc in docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'production service.*activation|service and HTTPS activation' \
            "$production_service_doc"; then
        echo "$production_service_doc не документирует production service activation" >&2
        exit 1
    fi
done
if ! strings build/hfoc-kernel.elf | grep -F '[PRODUCTION-SERVICE]' \
        >/dev/null 2>&1; then
    echo "Production kernel не содержит production service marker" >&2
    exit 1
fi

# ITEM-365 production auth/authz request integration contracts.
for production_auth_symbol in \
    appliance_production_auth_init \
    appliance_production_auth_publish_credential \
    appliance_production_auth_authorize_request \
    appliance_production_auth_available; do
    nm build/hfoc-kernel.elf | grep -F " $production_auth_symbol" >/dev/null || {
        echo "Production kernel не содержит $production_auth_symbol" >&2
        exit 1
    }
done
grep -Fq 'APPLIANCE_PRODUCTION_AUTH_VERSION 1U' \
    kernel/include/appliance_production_auth.h || {
    echo "appliance_production_auth.h не содержит версию 1U" >&2
    exit 1; }
for production_auth_contract in \
    appliance_production_auth_init \
    appliance_production_auth_publish_credential \
    appliance_production_auth_authorize_request; do
    grep -Fq "$production_auth_contract" \
        kernel/src/appliance_production_auth.c || {
        echo "appliance_production_auth.c не реализует $production_auth_contract" >&2
        exit 1
    }
done
grep -Fq 'appliance_production_auth_authorize_request' \
    kernel/src/appliance_production_management_qemu.c || {
    echo "production management QEMU proof не использует production auth" >&2
    exit 1; }
grep -Fq 'authorization=AVAILABLE' \
    kernel/src/appliance_production_management_qemu.c || {
    echo "production management QEMU proof не публикует authorization=AVAILABLE" >&2
    exit 1; }
grep -Fq 'test-appliance-production-auth-host' Makefile || {
    echo "Makefile не содержит test-appliance-production-auth-host" >&2
    exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-appliance-production-auth-host' Makefile || {
    echo "test-host-suite не содержит test-appliance-production-auth-host" >&2
    exit 1; }
[[ -f tests/appliance-production-auth-host.c ]] || {
    echo "missing tests/appliance-production-auth-host.c" >&2
    exit 1; }
[[ -f docs/hfoc-production-auth-authz-integration.md ]] || {
    echo "missing ITEM-365 production auth/authz document" >&2
    exit 1; }
for production_auth_doc in docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'production auth|auth/authz request integration' \
            "$production_auth_doc"; then
        echo "$production_auth_doc не документирует production auth integration" >&2
        exit 1
    fi
done

if grep -Fq 'qemu-arp-icmp' .github/workflows/ci.yml; then
    :
else
    echo "CI pgrep guard не содержит ARP/ICMP script" >&2
    exit 1
fi
grep -Fq 'QEMU ARP/ICMP baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-arp-icmp.sh || {
    echo "test-qemu-arp-icmp.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU ARP/ICMP test: OK' \
    scripts/test-qemu-arp-icmp.sh || {
    echo "test-qemu-arp-icmp.sh не содержит success marker" >&2
    exit 1
}
for arp_icmp_marker in \
    '[ARP-ICMP] proof init' \
    '[ARP-ICMP] arp ok' \
    '[ARP-ICMP] icmp ok' \
    '[ARP-ICMP] rate limit ok' \
    '[ARP-ICMP] proof OK'; do
    grep -Fq "$arp_icmp_marker" \
        kernel/src/arp_icmp_qemu.c || {
        echo "arp_icmp_qemu.c lacks $arp_icmp_marker" >&2
        exit 1
    }
    grep -Fq "$arp_icmp_marker" \
        scripts/test-qemu-arp-icmp.sh || {
        echo "test-qemu-arp-icmp.sh lacks $arp_icmp_marker" >&2
        exit 1
    }
done
for arp_icmp_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'arp/icmp.*qemu|arp-icmp.*qemu|arp and icmp integration' \
            "$arp_icmp_doc"; then
        echo "$arp_icmp_doc не документирует ARP/ICMP integration" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **237. ARP and ICMP integration' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 237 завершённым" >&2
    exit 1
}
