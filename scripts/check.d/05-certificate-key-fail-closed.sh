#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Certificate/key fail-closed matrix contracts (item 297).
grep -Fq 'test-certificate-fail-closed-matrix-host' Makefile || {
    echo "Makefile не содержит test-certificate-fail-closed-matrix-host" >&2;
    exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-certificate-fail-closed-matrix-host' Makefile || {
    echo "Makefile не вызывает test-certificate-fail-closed-matrix-host" >&2;
    exit 1; }
test -x scripts/test-certificate-fail-closed-matrix.sh || {
    echo "scripts/test-certificate-fail-closed-matrix.sh не является исполняемым" >&2;
    exit 1; }
bash -n scripts/test-certificate-fail-closed-matrix.sh || {
    echo "Синтаксис scripts/test-certificate-fail-closed-matrix.sh некорректен" >&2;
    exit 1; }
grep -Fq 'missing certificate readiness prevents HTTPS start' \
    scripts/test-certificate-fail-closed-matrix.sh \
    tests/https-service-descriptor-host.c || {
    echo "Certificate/key fail-closed matrix не проверяет no-listener publication" >&2;
    exit 1; }
for marker in \
    'manifest parse rejects null bytes' \
    'manifest parse rejects truncated certificate DER' \
    'manifest parse rejects digest mismatch' \
    'manifest parse rejects zero purpose' \
    'manifest parse rejects wrong hostname bytes' \
    'manifest parse rejects zero algorithm' \
    'manifest parse rejects mismatched private key' \
    '[CERTIFICATE-FAIL-CLOSED] matrix ok'; do
    if ! grep -Fq "$marker" tests/mbedtls-manifest-host.c \
            scripts/test-certificate-fail-closed-matrix.sh; then
        echo "Certificate/key fail-closed matrix не публикует $marker" >&2
        exit 1
    fi
done
grep -Fq '[t]est-certificate-fail-closed-matrix' \
    .github/workflows/ci.yml || {
    echo "CI не содержит test-certificate-fail-closed-matrix leak detection" >&2;
    exit 1; }
for certificate_fail_closed_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'Certificate/key fail-closed matrix' \
            "$certificate_fail_closed_doc"; then
        echo "$certificate_fail_closed_doc не документирует certificate/key fail-closed matrix" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **297. Certificate/key fail-closed matrix' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 297 завершённым" >&2
    exit 1
}

# Production artifact secret gate contracts (item 298).
grep -Fq 'test-production-artifact-secret-gate-host' Makefile || {
    echo "Makefile не содержит test-production-artifact-secret-gate-host" >&2;
    exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-production-artifact-secret-gate-host' Makefile || {
    echo "Makefile не вызывает test-production-artifact-secret-gate-host" >&2;
    exit 1; }
test -x scripts/verify-production-artifact-secret-gate.sh || {
    echo "scripts/verify-production-artifact-secret-gate.sh не является исполняемым" >&2;
    exit 1; }
test -x scripts/test-production-artifact-secret-gate.sh || {
    echo "scripts/test-production-artifact-secret-gate.sh не является исполняемым" >&2;
    exit 1; }
test -f scripts/production-artifact-secret-gate-profile.txt || {
    echo "scripts/production-artifact-secret-gate-profile.txt отсутствует" >&2;
    exit 1; }
bash -n scripts/verify-production-artifact-secret-gate.sh || {
    echo "Синтаксис verify-production-artifact-secret-gate.sh некорректен" >&2;
    exit 1; }
bash -n scripts/test-production-artifact-secret-gate.sh || {
    echo "Синтаксис test-production-artifact-secret-gate.sh некорректен" >&2;
    exit 1; }
for artifact_type in \
    kernel iso release-manifest serial audit-snapshot metrics support-bundle; do
    if ! grep -Fq "$artifact_type" \
            scripts/verify-production-artifact-secret-gate.sh; then
        echo "Production artifact secret gate не поддерживает $artifact_type" >&2
        exit 1
    fi
done
for token in \
    'HFOC-TLS-QEMU-RESPONSE' \
    'HFOC-TLS-ENTROPY-DENY' \
    'HFOC-ENTROPY-QEMU-SECRET' \
    '[TLS-QEMU]' \
    '[HTTPS-QEMU]' \
    '[ENTROPY-FAIL-CLOSED]' \
    '[CERTIFICATE-FAIL-CLOSED]'; do
    if ! grep -Fq "$token" \
            scripts/production-artifact-secret-gate-profile.txt \
            scripts/verify-production-artifact-secret-gate.sh; then
        echo "Production artifact secret gate profile не содержит $token" >&2
        exit 1
    fi
done
grep -Fq 'HFOC_MBEDTLS_TEST_SECRET_MARKER' \
    scripts/verify-production-artifact-secret-gate.sh \
    kernel/include/hfoc_mbedtls_test_secret.h || {
    echo "Production artifact secret gate не связывает test-secret marker" >&2;
    exit 1; }
grep -Fq 'test_secret_cert_digest=' \
    scripts/verify-production-artifact-secret-gate.sh || {
    echo "Production artifact secret gate не проверяет release digest fields" >&2;
    exit 1; }
grep -Fq 'hfoc_mbedtls_test_secret_' \
    scripts/verify-production-artifact-secret-gate.sh || {
    echo "Production artifact secret gate не проверяет test-secret symbols" >&2;
    exit 1; }
grep -Fq 'Production artifact secret gate host test: ALL OK' \
    scripts/test-production-artifact-secret-gate.sh || {
    echo "Production artifact secret gate harness не публикует ALL OK" >&2;
    exit 1; }
grep -Fq '[t]est-production-artifact-secret-gate' \
    .github/workflows/ci.yml || {
    echo "CI не содержит test-production-artifact-secret-gate leak detection" >&2;
    exit 1; }
for negative in \
    'kernel scan rejects test marker' \
    'ISO scan rejects certificate digest' \
    'release manifest scan rejects test-secret digest fields' \
    'serial scan rejects TLS QEMU response' \
    'audit snapshot scan rejects entropy sentinel' \
    'metrics scan rejects TLS proof control' \
    'support bundle scan rejects entropy proof control'; do
    if ! grep -Fq "$negative" \
            scripts/test-production-artifact-secret-gate.sh; then
        echo "Production artifact secret gate harness не содержит $negative" >&2
        exit 1
    fi
done
for production_secret_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'Production artifact secret gate' \
            "$production_secret_doc"; then
        echo "$production_secret_doc не документирует production artifact secret gate" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **298. Production artifact secret gate' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 298 завершённым" >&2
    exit 1
}

# HTTPS restart/soak rehearsal contracts (item 299).
grep -Fq 'test-https-restart-soak-qemu' Makefile || {
    echo "Makefile не содержит test-https-restart-soak-qemu" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-https-restart-soak-qemu' Makefile || {
    echo "Makefile не вызывает test-https-restart-soak-qemu" >&2; exit 1; }
grep -Fq 'HTTPS_RESTART_SOAK_QEMU_KERNEL' Makefile || {
    echo "Makefile не содержит HTTPS_RESTART_SOAK_QEMU_KERNEL" >&2;
    exit 1; }
grep -Fq 'HTTPS_RESTART_SOAK_QEMU_ISO' Makefile || {
    echo "Makefile не содержит HTTPS_RESTART_SOAK_QEMU_ISO" >&2;
    exit 1; }
grep -Fq 'HTTPS_RESTART_SOAK_QEMU_CFLAGS' Makefile || {
    echo "Makefile не содержит HTTPS_RESTART_SOAK_QEMU_CFLAGS" >&2;
    exit 1; }
grep -Fq 'HFOC_HTTPS_RESTART_SOAK_QEMU_TEST' \
    Makefile kernel/src/virtio_net_frame_qemu.c || {
    echo "HTTPS restart/soak rehearsal не определяет HFOC_HTTPS_RESTART_SOAK_QEMU_TEST" >&2;
    exit 1; }
test -x scripts/test-qemu-https-restart-soak.sh || {
    echo "scripts/test-qemu-https-restart-soak.sh не является исполняемым" >&2;
    exit 1; }
bash -n scripts/test-qemu-https-restart-soak.sh || {
    echo "Синтаксис scripts/test-qemu-https-restart-soak.sh некорректен" >&2;
    exit 1; }
grep -Fq 'set_link net0 off' scripts/test-qemu-https-restart-soak.sh || {
    echo "HTTPS restart/soak rehearsal не выключает net0" >&2;
    exit 1; }
grep -Fq 'set_link net0 on' scripts/test-qemu-https-restart-soak.sh || {
    echo "HTTPS restart/soak rehearsal не включает net0" >&2;
    exit 1; }
grep -Fq 'TLS connection unexpectedly succeeded while net0 off' \
    scripts/test-qemu-https-restart-soak.sh || {
    echo "HTTPS restart/soak rehearsal не проверяет link-off failure" >&2;
    exit 1; }
for marker in \
    '[HTTPS-RESTART-SOAK] proof init' \
    '[HTTPS-RESTART-SOAK] listener live' \
    '[HTTPS-RESTART-SOAK] round 1 requests ok' \
    '[HTTPS-RESTART-SOAK] round 2 requests ok' \
    '[HTTPS-RESTART-SOAK] round 3 requests ok' \
    '[HTTPS-RESTART-SOAK] malformed ok' \
    '[HTTPS-RESTART-SOAK] reset ok' \
    '[HTTPS-RESTART-SOAK] restart 1 ok' \
    '[HTTPS-RESTART-SOAK] restart 2 ok' \
    '[HTTPS-RESTART-SOAK] restart 3 ok' \
    '[HTTPS-RESTART-SOAK] link recovery ok' \
    '[HTTPS-RESTART-SOAK] final success' \
    '[HTTPS-RESTART-SOAK] no stale callback' \
    '[HTTPS-RESTART-SOAK] balanced counts' \
    '[HTTPS-RESTART-SOAK] clean shutdown' \
    '[HTTPS-RESTART-SOAK] proof OK'; do
    if ! grep -Fq "$marker" kernel/src/virtio_net_frame_qemu.c \
            scripts/test-qemu-https-restart-soak.sh; then
        echo "HTTPS restart/soak rehearsal не публикует $marker" >&2
        exit 1
    fi
done
grep -Fq '[t]est-qemu-https-restart-soak' \
    .github/workflows/ci.yml || {
    echo "CI не содержит test-qemu-https-restart-soak leak detection" >&2;
    exit 1; }
grep -Fq '[HTTPS-RESTART-SOAK]' \
    scripts/production-artifact-secret-gate-profile.txt \
    scripts/verify-production-artifact-secret-gate.sh || {
    echo "Production artifact secret gate profile не содержит HTTPS restart/soak marker" >&2;
    exit 1; }
for restart_soak_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'HTTPS restart/soak rehearsal' "$restart_soak_doc"; then
        echo "$restart_soak_doc не документирует HTTPS restart/soak rehearsal" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **299. HTTPS restart/soak rehearsal' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 299 завершённым" >&2
    exit 1
}

# HTTPS vertical-slice acceptance gate contracts (item 300).
test -f release/https-vertical-slice-acceptance.txt || {
    echo "release/https-vertical-slice-acceptance.txt отсутствует" >&2
    exit 1
}
grep -Fq 'https_vertical_slice_acceptance_version=1' \
    release/https-vertical-slice-acceptance.txt || {
    echo "HTTPS acceptance matrix не содержит версию 1" >&2
    exit 1
}
grep -Fq 'target_slice_id=HFOC-HTTPS-VERTICAL-SLICE' \
    release/https-vertical-slice-acceptance.txt || {
    echo "HTTPS acceptance matrix не содержит slice identity" >&2
    exit 1
}
grep -Fq 'check_count=9' release/https-vertical-slice-acceptance.txt || {
    echo "HTTPS acceptance matrix не содержит ровно девять проверок" >&2
    exit 1
}
test -x scripts/verify-https-vertical-slice-acceptance.sh || {
    echo "scripts/verify-https-vertical-slice-acceptance.sh не является исполняемым" >&2
    exit 1
}
test -x scripts/test-https-vertical-slice-acceptance.sh || {
    echo "scripts/test-https-vertical-slice-acceptance.sh не является исполняемым" >&2
    exit 1
}
test -x tests/test-https-vertical-slice-acceptance-host.sh || {
    echo "tests/test-https-vertical-slice-acceptance-host.sh не является исполняемым" >&2
    exit 1
}
bash -n scripts/verify-https-vertical-slice-acceptance.sh
bash -n scripts/test-https-vertical-slice-acceptance.sh
bash -n tests/test-https-vertical-slice-acceptance-host.sh
bash scripts/verify-https-vertical-slice-acceptance.sh \
    release/https-vertical-slice-acceptance.txt >/dev/null
grep -Fq 'test-https-vertical-slice-acceptance-host' Makefile || {
    echo "Makefile не содержит test-https-vertical-slice-acceptance-host" >&2
    exit 1
}
grep -Fq 'test-https-vertical-slice-acceptance' Makefile || {
    echo "Makefile не содержит test-https-vertical-slice-acceptance" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-https-vertical-slice-acceptance-host' Makefile || {
    echo "test-host-suite не содержит HTTPS acceptance host target" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-https-vertical-slice-acceptance' Makefile || {
    echo "make test не содержит HTTPS acceptance gate" >&2
    exit 1
}
grep -Fq 'HTTPS vertical-slice acceptance gate: ACCEPT' \
    scripts/test-https-vertical-slice-acceptance.sh || {
    echo "HTTPS acceptance runner не публикует ACCEPT" >&2
    exit 1
}
grep -Fq 'HTTPS vertical-slice acceptance gate: REJECT' \
    scripts/test-https-vertical-slice-acceptance.sh || {
    echo "HTTPS acceptance runner не публикует REJECT" >&2
    exit 1
}
for acceptance_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'HTTPS vertical-slice acceptance gate' "$acceptance_doc"; then
        echo "$acceptance_doc не документирует HTTPS vertical-slice acceptance gate" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **300. HTTPS vertical-slice acceptance gate' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 300 завершённым" >&2
    exit 1
}

# lwIP allocator boundary contracts (item 233).
grep -Fq 'test-lwip-alloc-host' Makefile || {
    echo "Makefile не содержит test-lwip-alloc-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-lwip-alloc-host' Makefile || {
    echo "test-host-suite не содержит test-lwip-alloc-host" >&2;
    exit 1; }
grep -Fq '#define MEM_CUSTOM_ALLOCATOR 1' \
    kernel/include/lwipopts.h || {
    echo "lwipopts.h не содержит MEM_CUSTOM_ALLOCATOR 1" >&2; exit 1; }
grep -Fq '#define MEM_CUSTOM_MALLOC hfoc_lwip_mem_malloc' \
    kernel/include/lwipopts.h || {
    echo "lwipopts.h не содержит HFOC lwIP malloc adapter" >&2; exit 1; }
grep -Fq '#define MEM_CUSTOM_FREE hfoc_lwip_mem_free' \
    kernel/include/lwipopts.h || {
    echo "lwipopts.h не содержит HFOC lwIP free adapter" >&2; exit 1; }
grep -Fq '#define MEM_CUSTOM_CALLOC hfoc_lwip_mem_calloc' \
    kernel/include/lwipopts.h || {
    echo "lwipopts.h не содержит HFOC lwIP calloc adapter" >&2; exit 1; }
grep -Fq '#define MEMP_MEM_MALLOC 1' \
    kernel/include/lwipopts.h || {
    echo "lwipopts.h не содержит MEMP_MEM_MALLOC 1" >&2; exit 1; }
grep -Fq 'hfoc_lwip_alloc_init' kernel/include/hfoc_lwip.h || {
    echo "hfoc_lwip.h не объявляет allocator init" >&2; exit 1; }
grep -Fq 'hfoc_lwip_mem_malloc' kernel/include/hfoc_lwip.h || {
    echo "hfoc_lwip.h не объявляет lwIP malloc adapter" >&2; exit 1; }

# lwIP monotonic time adapter contracts (item 234).
grep -Fq 'test-lwip-time-host' Makefile || {
    echo "Makefile не содержит test-lwip-time-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-lwip-time-host' Makefile || {
    echo "test-host-suite не содержит test-lwip-time-host" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_TIME_VERSION 1U' \
    kernel/include/hfoc_lwip_time.h || {
    echo "hfoc_lwip_time.h не содержит HFOC_LWIP_TIME_VERSION 1U" >&2;
    exit 1; }
grep -Fq 'hfoc_lwip_time_init_default' \
    kernel/include/hfoc_lwip_time.h || {
    echo "hfoc_lwip_time.h не объявляет default init" >&2; exit 1; }
grep -Fq 'hfoc_lwip_time_init_default' \
    kernel/src/kernel.c || {
    echo "kernel.c не инициализирует monotonic time adapter" >&2;
    exit 1; }
grep -Fq '(void)hfoc_lwip_time_poll();' \
    kernel/src/kernel.c || {
    echo "kernel.c не вызывает monotonic time poll" >&2; exit 1; }
grep -Fq 'u32_t sys_now(void)' \
    kernel/src/hfoc_lwip_time.c || {
    echo "hfoc_lwip_time.c не определяет sys_now" >&2; exit 1; }
grep -Fq 'timer_ticks() * 10U' \
    kernel/src/hfoc_lwip_time.c || {
    echo "hfoc_lwip_time.c не использует существующий monotonic tick" >&2;
    exit 1; }
if grep -Eq '\b(clock_gettime|gettimeofday|localtime|mktime|time_t)\b' \
    kernel/src/hfoc_lwip_time.c kernel/include/hfoc_lwip_time.h; then
    echo "lwIP time adapter зависит от wall-clock API" >&2
    exit 1
fi

# lwIP pbuf/packet ownership bridge contracts (item 235).
grep -Fq 'test-lwip-pbuf-host' Makefile || {
    echo "Makefile не содержит test-lwip-pbuf-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-lwip-pbuf-host' Makefile || {
    echo "test-host-suite не содержит test-lwip-pbuf-host" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_PBUF_VERSION 1U' \
    kernel/include/hfoc_lwip_pbuf.h || {
    echo "hfoc_lwip_pbuf.h не содержит HFOC_LWIP_PBUF_VERSION 1U" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_PBUF_SLOT_COUNT 8U' \
    kernel/include/hfoc_lwip_pbuf.h || {
    echo "hfoc_lwip_pbuf.h не содержит HFOC_LWIP_PBUF_SLOT_COUNT 8U" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_PBUF_CHAIN_MAX 4U' \
    kernel/include/hfoc_lwip_pbuf.h || {
    echo "hfoc_lwip_pbuf.h не содержит HFOC_LWIP_PBUF_CHAIN_MAX 4U" >&2;
    exit 1; }
grep -Fq 'hfoc_lwip_pbuf_import_rx' \
    kernel/include/hfoc_lwip_pbuf.h || {
    echo "hfoc_lwip_pbuf.h не объявляет RX import" >&2; exit 1; }
grep -Fq 'hfoc_lwip_pbuf_export_tx' \
    kernel/include/hfoc_lwip_pbuf.h || {
    echo "hfoc_lwip_pbuf.h не объявляет TX export" >&2; exit 1; }
grep -Fq '#define LWIP_SUPPORT_CUSTOM_PBUF 1' \
    kernel/include/lwipopts.h || {
    echo "lwipopts.h не включает custom pbuf support" >&2; exit 1; }
grep -Fq 'hfoc_lwip_pbuf_bridge_init' \
    kernel/src/hfoc_lwip_pbuf.c || {
    echo "hfoc_lwip_pbuf.c не реализует bridge init" >&2; exit 1; }
if grep -Eq '\b(pbuf_alloc|pbuf_free|mem_malloc|mem_free|memp_malloc|memp_free)\b' \
        kernel/src/hfoc_lwip_pbuf.c; then
    echo "hfoc_lwip_pbuf.c использует lwIP allocator/pbuf ownership API" >&2
    exit 1
fi
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/hfoc_lwip_pbuf.c; then
    echo "hfoc_lwip_pbuf.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi

# lwIP netif lifecycle contracts (item 236).
grep -Fq 'test-lwip-netif-host' Makefile || {
    echo "Makefile не содержит test-lwip-netif-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-lwip-netif-host' Makefile || {
    echo "test-host-suite не содержит test-lwip-netif-host" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_NETIF_VERSION 1U' \
    kernel/include/hfoc_lwip_netif.h || {
    echo "hfoc_lwip_netif.h не содержит HFOC_LWIP_NETIF_VERSION 1U" >&2;
    exit 1; }
grep -Fq 'HFOC_LWIP_NETIF_MTU 1500U' \
    kernel/include/hfoc_lwip_netif.h || {
    echo "hfoc_lwip_netif.h не содержит HFOC_LWIP_NETIF_MTU 1500U" >&2;
    exit 1; }
grep -Fq 'hfoc_lwip_netif_activate' \
    kernel/include/hfoc_lwip_netif.h || {
    echo "hfoc_lwip_netif.h не объявляет activate" >&2; exit 1; }
grep -Fq 'hfoc_lwip_netif_remove' \
    kernel/include/hfoc_lwip_netif.h || {
    echo "hfoc_lwip_netif.h не объявляет remove" >&2; exit 1; }
grep -Fq 'hfoc_lwip_netif_init' \
    kernel/src/hfoc_lwip_netif.c || {
    echo "hfoc_lwip_netif.c не реализует init" >&2; exit 1; }
if grep -Eq '\b(netif_add|netif_remove|netif_set_addr|netif_set_up|netif_set_link_up|pbuf_alloc|pbuf_free|mem_malloc|mem_free|memp_malloc|memp_free)\b' \
        kernel/src/hfoc_lwip_netif.c; then
    echo "hfoc_lwip_netif.c использует lwIP netif/protocol ownership API" >&2
    exit 1
fi
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/hfoc_lwip_netif.c; then
    echo "hfoc_lwip_netif.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi

find scripts -type f -name '*.sh' -print0 | xargs -0 -n1 bash -n
./scripts/test-pic-spurious.sh
make kernel
bash scripts/verify-mbedtls-test-secret.sh --production build/hfoc-kernel.elf

# HTTPS TLS-to-HTTP data flow production isolation.
if nm -g build/hfoc-kernel.elf | grep -E \
        ' https_data_flow_(rx|tx|release|init|published|generation|request_bytes|response_sent|retained_count|rejected_count|closed_count|status_string|tls_status_string)'; then
    echo "Production kernel содержит HTTPS data-flow host symbols" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'HTTPS TLS-to-HTTP data flow host test: ALL OK'; then
    echo "Production kernel содержит HTTPS data-flow host marker" >&2
    exit 1
fi

# HTTPS root and health handler production contract.
#
# ITEM-386 lifted the old prohibition here. The ordinary production boot now
# serves the root and health handlers itself, so the handler symbols MUST be
# present in the production ELF. The gate is re-pointed to a positive contract;
# no protection is dropped, because the host-test marker below stays forbidden
# and only the handler code moved into the production image.
for https_handler_symbol in \
    https_handler_dispatch \
    https_handler_encode \
    https_handler_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $https_handler_symbol" >/dev/null || {
        echo "Production kernel не содержит $https_handler_symbol" >&2
        exit 1
    }
done
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'HTTPS root/health handler host test: ALL OK'; then
    echo "Production kernel содержит HTTPS handler host marker" >&2
    exit 1
fi

# HTTPS service descriptor production isolation.
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'HTTPS service descriptor host test: ALL OK'; then
    echo "Production kernel содержит HTTPS descriptor host marker" >&2
    exit 1
fi

# HTTP parser state descriptor production isolation.
# ITEM-386: the ordinary production boot serves this component itself,
# so its symbols MUST be present in the production ELF. The old
# prohibition is replaced by this positive contract; the host-test
# marker that follows stays forbidden.
for http_parser_symbol in \
    http_parser_init \
    http_parser_start \
    http_parser_release; do
    nm build/hfoc-kernel.elf | grep -F " $http_parser_symbol" >/dev/null || {
        echo "Production kernel не содержит $http_parser_symbol" >&2
        exit 1
    }
done
if strings build/hfoc-kernel.elf | grep -F 'HTTP parser state host tests'; then
    echo "Production kernel содержит HTTP parser host marker" >&2
    exit 1
fi

# Incremental request-line parser production isolation.
# ITEM-386: the ordinary production boot serves this component itself,
# so its symbols MUST be present in the production ELF. The old
# prohibition is replaced by this positive contract; the host-test
# marker that follows stays forbidden.
for http_request_line_symbol in \
    http_request_line_feed \
    http_request_line_get; do
    nm build/hfoc-kernel.elf | grep -F " $http_request_line_symbol" >/dev/null || {
        echo "Production kernel не содержит $http_request_line_symbol" >&2
        exit 1
    }
done
if strings build/hfoc-kernel.elf | grep -F 'HTTP request line host tests'; then
    echo "Production kernel содержит HTTP request-line host marker" >&2
    exit 1
fi

# Incremental header parser production isolation.
# ITEM-386: the ordinary production boot serves this component itself,
# so its symbols MUST be present in the production ELF. The old
# prohibition is replaced by this positive contract; the host-test
# marker that follows stays forbidden.
for http_header_symbol in \
    http_header_feed \
    http_header_count \
    http_header_field; do
    nm build/hfoc-kernel.elf | grep -F " $http_header_symbol" >/dev/null || {
        echo "Production kernel не содержит $http_header_symbol" >&2
        exit 1
    }
done
if strings build/hfoc-kernel.elf | grep -F 'HTTP header host tests'; then
    echo "Production kernel содержит HTTP header host marker" >&2
    exit 1
fi

# HTTP framing and body rejection production isolation.
# ITEM-386: the ordinary production boot serves this component itself,
# so its symbols MUST be present in the production ELF. The old
# prohibition is replaced by this positive contract; the host-test
# marker that follows stays forbidden.
for http_framing_symbol in \
    http_framing_check \
    http_framing_kind_string; do
    nm build/hfoc-kernel.elf | grep -F " $http_framing_symbol" >/dev/null || {
        echo "Production kernel не содержит $http_framing_symbol" >&2
        exit 1
    }
done
if strings build/hfoc-kernel.elf | grep -F 'HTTP framing host tests'; then
    echo "Production kernel содержит HTTP framing host marker" >&2
    exit 1
fi

# HTTP method/version and Host policy production isolation.
# ITEM-386: the ordinary production boot serves this component itself,
# so its symbols MUST be present in the production ELF. The old
# prohibition is replaced by this positive contract; the host-test
# marker that follows stays forbidden.
for http_policy_symbol in \
    http_policy_check \
    http_method_kind_string \
    http_policy_reason_string; do
    nm build/hfoc-kernel.elf | grep -F " $http_policy_symbol" >/dev/null || {
        echo "Production kernel не содержит $http_policy_symbol" >&2
        exit 1
    }
done
if strings build/hfoc-kernel.elf | grep -F 'HTTP policy host tests'; then
    echo "Production kernel содержит HTTP policy host marker" >&2
    exit 1
fi

# HTTP fixed route table production isolation.
# ITEM-386: the ordinary production boot serves this component itself,
# so its symbols MUST be present in the production ELF. The old
# prohibition is replaced by this positive contract; the host-test
# marker that follows stays forbidden.
for http_route_symbol in \
    http_route_match_line \
    http_route_match \
    http_route_id_string; do
    nm build/hfoc-kernel.elf | grep -F " $http_route_symbol" >/dev/null || {
        echo "Production kernel не содержит $http_route_symbol" >&2
        exit 1
    }
done
if strings build/hfoc-kernel.elf | grep -F 'HTTP route host tests'; then
    echo "Production kernel содержит HTTP route host marker" >&2
    exit 1
fi

# ------------------------------------------------- ITEM-493: сертификат жив --
# Прошитый в ядро тестовый сертификат `hfoc.test` проверяется curl'ом по
# текущему времени, поэтому истёкший сертификат красит acceptance-фазу каждого
# PR (2026-09-28: `curl: (60) SSL certificate problem: certificate has expired`
# через часы после истечения, причина видна только в логе гостя).  Контракт:
# сертификат обязан быть действителен ещё как минимум год, а его байты обязаны
# совпадать с дайджестами, которые объявляет заголовок; проверка стоит в
# каноническом host-suite, где причина видна сразу.
for mbedtls_validity in \
    tests/test-mbedtls-secret-validity.sh \
    kernel/src/hfoc_mbedtls_test_secret.c \
    kernel/include/hfoc_mbedtls_test_secret.h; do
    test -f "$mbedtls_validity" || {
        echo "ITEM-493: отсутствует файл: $mbedtls_validity" >&2
        exit 1
    }
done
# shellcheck disable=SC2016  # the pinned text is literal script, not expanded
for mbedtls_validity_pin in \
    'MIN_SECONDS=${HFOC_TEST_SECRET_MIN_SECONDS:-31536000}' \
    '-checkend "$MIN_SECONDS"' \
    'the embedded test certificate expires within' \
    'the embedded secret does not match the digests the header declares'; do
    grep -Fq -- "$mbedtls_validity_pin" \
        tests/test-mbedtls-secret-validity.sh || {
        echo "ITEM-493: проверка сертификата не содержит: $mbedtls_validity_pin" >&2
        exit 1
    }
done
# shellcheck disable=SC2016  # the Makefile text is literal, not expanded
grep -Fq '$(MAKE) test-mbedtls-secret-validity-host' Makefile || {
    echo "ITEM-493: проверка срока сертификата не в каноническом host-suite" >&2
    exit 1
}
grep -Fq 'test-mbedtls-secret-validity-host' Makefile || {
    echo "ITEM-493: цели проверки сертификата нет" >&2
    exit 1
}
