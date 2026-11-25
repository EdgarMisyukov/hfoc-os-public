#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# lwIP source and license lock contracts (item 231).
bash scripts/verify-lwip-lock.sh
grep -Fq 'test-lwip-source-lock-host' Makefile || {
    echo "Makefile не содержит test-lwip-source-lock-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-lwip-source-lock-host' Makefile || {
    echo "test-host-suite не содержит test-lwip-source-lock-host" >&2;
    exit 1; }
grep -Fq 'lwip_lock_version=1' third_party/LOCKS.sha256 || {
    echo "LOCKS.sha256 не содержит lwip_lock_version=1" >&2; exit 1; }
grep -Fq 'STABLE-2_2_1_RELEASE' \
    third_party/LOCKS.sha256 third_party/LICENSES.md || {
    echo "lwIP release не зафиксирован в source lock/license inventory" >&2;
    exit 1; }
grep -Fq 'upstream_sha256=ce0b7461c0ad9602c376f0bf07c5eb7253b48c7bf66f011c6bf3e2a96731c539' \
    third_party/LOCKS.sha256 || {
    echo "LOCKS.sha256 не содержит upstream archive digest" >&2; exit 1; }
grep -Fq 'BSD-3-Clause' \
    third_party/LOCKS.sha256 third_party/LICENSES.md || {
    echo "lwIP license inventory не содержит BSD-3-Clause" >&2; exit 1; }
grep -Fq 'file_count=107' third_party/LICENSES.md || {
    echo "LICENSES.md не содержит точный file_count" >&2; exit 1; }
if grep -Eq '^C_SOURCES[^=]*=.*third_party/lwip' Makefile; then
    echo "Production C_SOURCES не должен компилировать lwIP source" >&2
    exit 1
fi

# mbedTLS source and license lock contracts (item 261).
bash scripts/verify-mbedtls-lock.sh
grep -Fq 'test-mbedtls-source-lock-host' Makefile || {
    echo "Makefile не содержит test-mbedtls-source-lock-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-mbedtls-source-lock-host' Makefile || {
    echo "test-host-suite не содержит test-mbedtls-source-lock-host" >&2;
    exit 1; }
grep -Fq 'mbedtls_lock_version=1' third_party/LOCKS.sha256 || {
    echo "LOCKS.sha256 не содержит mbedtls_lock_version=1" >&2; exit 1; }
grep -Fq 'mbedtls-3.6.7' \
    third_party/LOCKS.sha256 third_party/LICENSES.md || {
    echo "mbedTLS release не зафиксирован в source lock/license inventory" >&2;
    exit 1; }
grep -Fq 'upstream_sha256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6' \
    third_party/LOCKS.sha256 || {
    echo "LOCKS.sha256 не содержит mbedTLS upstream archive digest" >&2; exit 1; }
grep -Fq 'Apache-2.0 OR GPL-2.0-or-later' \
    third_party/LOCKS.sha256 third_party/LICENSES.md || {
    echo "mbedTLS license inventory не содержит dual license expression" >&2;
    exit 1; }
grep -Fq 'file_count=272' third_party/LICENSES.md || {
    echo "LICENSES.md не содержит точный mbedTLS file_count" >&2; exit 1; }
grep -Fq '9b405ef4c89342f5eae1dd828882f931747f71001cfba7d114801039b52ad09b  mbedtls/LICENSE' \
    third_party/LICENSE-DOCUMENTS.sha256 || {
    echo "LICENSE-DOCUMENTS.sha256 не содержит mbedTLS license text" >&2;
    exit 1; }
if grep -Eq '^C_SOURCES[^=]*=.*third_party/mbedtls' Makefile; then
    echo "Production C_SOURCES не должен компилировать mbedTLS source" >&2
    exit 1
fi
for mbedtls_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'mbedtls source and license lock|mbedTLS source and license lock' \
            "$mbedtls_doc"; then
        echo "$mbedtls_doc не документирует mbedTLS source and license lock" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **261. mbedTLS source and license lock' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 261 завершённым" >&2
    exit 1
}

# lwIP minimal configuration contracts (item 232).
bash scripts/verify-lwip-config.sh
grep -Fq 'test-lwip-config-host' Makefile || {
    echo "Makefile не содержит test-lwip-config-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-lwip-config-host' Makefile || {
    echo "test-host-suite не содержит test-lwip-config-host" >&2;
    exit 1; }
grep -Fq '#define HFOC_LWIP_CONFIG_VERSION 1' \
    kernel/include/lwipopts.h || {
    echo "lwipopts.h не содержит HFOC_LWIP_CONFIG_VERSION 1" >&2; exit 1; }

# mbedTLS minimal configuration contracts (item 262).
# ITEM-508 increment 13: the one client-role call site is the ACME link
# binding; the verifier is given the pair explicitly and checks it both ways.
bash scripts/verify-mbedtls-config.sh \
    kernel/include/hfoc_mbedtls_config.h kernel/src/hfoc_mbedtls_alloc.c \
    kernel/src/hfoc_mbedtls_drbg.c kernel/src/hfoc_mbedtls_manifest.c \
    kernel/src/appliance_acme_transport_lwip.c \
    --except=mbedtls_ssl_handshake_step:kernel/src/appliance_acme_transport_lwip.c
grep -Fq 'test-mbedtls-config-host' Makefile || {
    echo "Makefile не содержит test-mbedtls-config-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-mbedtls-config-host' Makefile || {
    echo "test-host-suite не содержит test-mbedtls-config-host" >&2;
    exit 1; }
grep -Fq '#define HFOC_MBEDTLS_CONFIG_VERSION 5' \
    kernel/include/hfoc_mbedtls_config.h || {
    echo "hfoc_mbedtls_config.h не содержит HFOC_MBEDTLS_CONFIG_VERSION 5" >&2;
    exit 1; }
grep -Fq '#define MBEDTLS_SSL_PROTO_TLS1_3' \
    kernel/include/hfoc_mbedtls_config.h || {
    echo "hfoc_mbedtls_config.h не включает TLS 1.3" >&2; exit 1; }
grep -Fq '#define MBEDTLS_SSL_SRV_C' \
    kernel/include/hfoc_mbedtls_config.h || {
    echo "hfoc_mbedtls_config.h не включает server mode" >&2; exit 1; }
grep -Fq '#define MBEDTLS_SSL_CLI_C' \
    kernel/include/hfoc_mbedtls_config.h || {
    echo "hfoc_mbedtls_config.h не включает client mode (ITEM-508)" >&2;
    exit 1; }
grep -Fq '#define MBEDTLS_SSL_SERVER_NAME_INDICATION' \
    kernel/include/hfoc_mbedtls_config.h || {
    echo "hfoc_mbedtls_config.h не включает SNI" >&2; exit 1; }
for mbedtls_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'minimal mbedTLS TLS 1.3 configuration' \
            "$mbedtls_doc"; then
        echo "$mbedtls_doc не документирует minimal mbedTLS TLS 1.3 configuration" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **262. Minimal TLS 1.3 configuration' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 262 завершённым" >&2
    exit 1
}

# mbedTLS allocator boundary contracts (item 263).
grep -Fq 'test-mbedtls-alloc-host' Makefile || {
    echo "Makefile не содержит test-mbedtls-alloc-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-mbedtls-alloc-host' Makefile || {
    echo "test-host-suite не содержит test-mbedtls-alloc-host" >&2;
    exit 1; }
grep -Fq 'HFOC_MBEDTLS_ALLOC_VERSION 1U' \
    kernel/include/hfoc_mbedtls_alloc.h || {
    echo "hfoc_mbedtls_alloc.h не содержит HFOC_MBEDTLS_ALLOC_VERSION 1U" >&2;
    exit 1; }
grep -Fq 'HFOC_MBEDTLS_CONNECTION_COUNT 4U' \
    kernel/include/hfoc_mbedtls_alloc.h || {
    echo "hfoc_mbedtls_alloc.h не содержит connection count 4" >&2;
    exit 1; }
grep -Fq 'hfoc_mbedtls_alloc_connection_open' \
    kernel/include/hfoc_mbedtls_alloc.h kernel/src/hfoc_mbedtls_alloc.c || {
    echo "mbedTLS allocator boundary не объявляет connection open" >&2;
    exit 1; }
grep -Fq '#define MBEDTLS_PLATFORM_MEMORY' \
    kernel/include/hfoc_mbedtls_config.h || {
    echo "hfoc_mbedtls_config.h не включает platform memory" >&2;
    exit 1; }
grep -Fq '#define MBEDTLS_PLATFORM_CALLOC_MACRO hfoc_mbedtls_calloc' \
    kernel/include/hfoc_mbedtls_config.h || {
    echo "hfoc_mbedtls_config.h не включает HFOC calloc adapter" >&2;
    exit 1; }
grep -Fq '#define MBEDTLS_PLATFORM_FREE_MACRO hfoc_mbedtls_free' \
    kernel/include/hfoc_mbedtls_config.h || {
    echo "hfoc_mbedtls_config.h не включает HFOC free adapter" >&2;
    exit 1; }
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/hfoc_mbedtls_alloc.c; then
    echo "hfoc_mbedtls_alloc.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
for mbedtls_alloc_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'mbedtls allocator boundary|mbedTLS allocator boundary' \
            "$mbedtls_alloc_doc"; then
        echo "$mbedtls_alloc_doc не документирует mbedTLS allocator boundary" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **263. mbedTLS allocator boundary' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 263 завершённым" >&2
    exit 1
}

# mbedTLS entropy/DRBG adapter contracts (item 264).
grep -Fq 'test-mbedtls-drbg-host' Makefile || {
    echo "Makefile не содержит test-mbedtls-drbg-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-mbedtls-drbg-host' Makefile || {
    echo "test-host-suite не содержит test-mbedtls-drbg-host" >&2;
    exit 1; }
grep -Fq 'HFOC_MBEDTLS_DRBG_VERSION 1U' \
    kernel/include/hfoc_mbedtls_drbg.h || {
    echo "hfoc_mbedtls_drbg.h не содержит HFOC_MBEDTLS_DRBG_VERSION 1U" >&2;
    exit 1; }
grep -Fq 'HFOC_MBEDTLS_DRBG_PURPOSE_TLS_HANDSHAKE 1U' \
    kernel/include/hfoc_mbedtls_drbg.h || {
    echo "hfoc_mbedtls_drbg.h не содержит TLS handshake purpose" >&2;
    exit 1; }
grep -Fq 'HFOC_MBEDTLS_DRBG_ENTROPY_LENGTH 48U' \
    kernel/include/hfoc_mbedtls_drbg.h || {
    echo "hfoc_mbedtls_drbg.h не содержит 48-byte entropy length" >&2;
    exit 1; }
grep -Fq 'HFOC_MBEDTLS_DRBG_CONTEXT_STORAGE_BYTES 512U' \
    kernel/include/hfoc_mbedtls_drbg.h || {
    echo "hfoc_mbedtls_drbg.h не содержит DRBG context storage" >&2;
    exit 1; }
grep -Fq 'hfoc_mbedtls_drbg_publish' \
    kernel/include/hfoc_mbedtls_drbg.h kernel/src/hfoc_mbedtls_drbg.c || {
    echo "mbedTLS DRBG boundary не объявляет publish" >&2; exit 1; }
grep -Fq 'hfoc_mbedtls_drbg_invalidate' \
    kernel/include/hfoc_mbedtls_drbg.h kernel/src/hfoc_mbedtls_drbg.c || {
    echo "mbedTLS DRBG boundary не объявляет generation invalidation" >&2;
    exit 1; }
grep -Fq 'HFOC_MBEDTLS_DRBG_HOST_TEST' \
    kernel/src/hfoc_mbedtls_drbg.c || {
    echo "mbedTLS DRBG host adapter не помечен HFOC_MBEDTLS_DRBG_HOST_TEST" >&2;
    exit 1; }
grep -Fq 'mbedtls_ctr_drbg_seed' \
    kernel/src/hfoc_mbedtls_drbg.c || {
    echo "mbedTLS DRBG host adapter не вызывает mbedtls_ctr_drbg_seed" >&2;
    exit 1; }
grep -Fq 'entropy_seed_fill_seed' \
    kernel/src/hfoc_mbedtls_drbg.c || {
    echo "mbedTLS DRBG host adapter не вызывает entropy_seed_fill_seed" >&2;
    exit 1; }
grep -Fq 'entropy_seed_fill_fill' \
    kernel/src/hfoc_mbedtls_drbg.c || {
    echo "mbedTLS DRBG host adapter не вызывает entropy_seed_fill_fill" >&2;
    exit 1; }
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/hfoc_mbedtls_drbg.c; then
    echo "hfoc_mbedtls_drbg.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
for mbedtls_drbg_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'mbedtls entropy/drbg adapter|mbedTLS entropy/DRBG adapter' \
            "$mbedtls_drbg_doc"; then
        echo "$mbedtls_drbg_doc не документирует mbedTLS entropy/DRBG adapter" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **264. mbedTLS entropy/DRBG adapter' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 264 завершённым" >&2
    exit 1
}

# mbedTLS certificate/key manifest parser contracts (item 265).
grep -Fq 'test-mbedtls-manifest-host' Makefile || {
    echo "Makefile не содержит test-mbedtls-manifest-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-mbedtls-manifest-host' Makefile || {
    echo "test-host-suite не содержит test-mbedtls-manifest-host" >&2;
    exit 1; }
grep -Fq 'HFOC_MBEDTLS_MANIFEST_VERSION 1U' \
    kernel/include/hfoc_mbedtls_manifest.h || {
    echo "hfoc_mbedtls_manifest.h не содержит HFOC_MBEDTLS_MANIFEST_VERSION 1U" >&2;
    exit 1; }
grep -Fq 'HFOC_MBEDTLS_MANIFEST_PURPOSE_TLS_HANDSHAKE 1U' \
    kernel/include/hfoc_mbedtls_manifest.h || {
    echo "hfoc_mbedtls_manifest.h не содержит TLS handshake purpose" >&2;
    exit 1; }
grep -Fq 'HFOC_MBEDTLS_MANIFEST_ALGORITHM_ECDSA_P256_SHA256 1U' \
    kernel/include/hfoc_mbedtls_manifest.h || {
    echo "hfoc_mbedtls_manifest.h не содержит ECDSA P-256 SHA-256" >&2;
    exit 1; }
grep -Fq 'HFOC_MBEDTLS_MANIFEST_HOSTNAME "hfoc.test"' \
    kernel/include/hfoc_mbedtls_manifest.h || {
    echo "hfoc_mbedtls_manifest.h не содержит hfoc.test hostname" >&2;
    exit 1; }
grep -Fq 'hfoc_mbedtls_manifest_parse' \
    kernel/include/hfoc_mbedtls_manifest.h \
    kernel/src/hfoc_mbedtls_manifest.c || {
    echo "mbedTLS manifest boundary не объявляет parse" >&2; exit 1; }
grep -Fq 'HFOC_MBEDTLS_MANIFEST_HOST_TEST' \
    kernel/src/hfoc_mbedtls_manifest.c || {
    echo "mbedTLS manifest parser не помечен HFOC_MBEDTLS_MANIFEST_HOST_TEST" >&2;
    exit 1; }
grep -Fq 'mbedtls_x509_crt_parse_der' \
    kernel/src/hfoc_mbedtls_manifest.c || {
    echo "mbedTLS manifest parser не вызывает mbedtls_x509_crt_parse_der" >&2;
    exit 1; }
grep -Fq 'mbedtls_pk_parse_key' \
    kernel/src/hfoc_mbedtls_manifest.c || {
    echo "mbedTLS manifest parser не вызывает mbedtls_pk_parse_key" >&2;
    exit 1; }
grep -Fq 'mbedtls_sha256' \
    kernel/src/hfoc_mbedtls_manifest.c || {
    echo "mbedTLS manifest parser не вызывает mbedtls_sha256" >&2;
    exit 1; }
grep -Fq 'mbedtls_pk_write_pubkey_der' \
    kernel/src/hfoc_mbedtls_manifest.c || {
    echo "mbedTLS manifest parser не вызывает mbedtls_pk_write_pubkey_der" >&2;
    exit 1; }
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/hfoc_mbedtls_manifest.c; then
    echo "hfoc_mbedtls_manifest.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
for mbedtls_manifest_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'mbedtls certificate/key manifest parser|mbedTLS certificate/key manifest parser' \
            "$mbedtls_manifest_doc"; then
        echo "$mbedtls_manifest_doc не документирует mbedTLS certificate/key manifest parser" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **265. Certificate/key manifest parser' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 265 завершённым" >&2
    exit 1
}

# mbedTLS test-secret build separation contracts (item 266).
grep -Fq 'test-mbedtls-secret-host' Makefile || {
    echo "Makefile не содержит test-mbedtls-secret-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-mbedtls-secret-host' Makefile || {
    echo "test-host-suite не содержит test-mbedtls-secret-host" >&2;
    exit 1; }
grep -Fq 'test-mbedtls-secret-iso:' Makefile || {
    echo "Makefile не содержит test-mbedtls-secret-iso" >&2; exit 1; }
grep -Fq 'MBEDTLS_TEST_SECRET_ISO' Makefile || {
    echo "Makefile не содержит MBEDTLS_TEST_SECRET_ISO" >&2; exit 1; }
grep -Fq 'HFOC_MBEDTLS_TEST_SECRET_QEMU_TEST' \
    Makefile kernel/src/hfoc_mbedtls_test_secret.c || {
    echo "test-secret build не определяет HFOC_MBEDTLS_TEST_SECRET_QEMU_TEST" >&2;
    exit 1; }
grep -Fq 'HFOC_MBEDTLS_TEST_SECRET_VERSION 1U' \
    kernel/include/hfoc_mbedtls_test_secret.h || {
    echo "hfoc_mbedtls_test_secret.h не содержит test-secret version 1" >&2;
    exit 1; }
grep -Fq 'HFOC_MBEDTLS_TEST_SECRET_MARKER' \
    kernel/include/hfoc_mbedtls_test_secret.h || {
    echo "hfoc_mbedtls_test_secret.h не содержит test-secret marker" >&2;
    exit 1; }
grep -Fq 'HFOC_MBEDTLS_TEST_SECRET_CERT_DIGEST' \
    kernel/include/hfoc_mbedtls_test_secret.h || {
    echo "hfoc_mbedtls_test_secret.h не содержит certificate digest" >&2;
    exit 1; }
grep -Fq 'HFOC_MBEDTLS_TEST_SECRET_KEY_DIGEST' \
    kernel/include/hfoc_mbedtls_test_secret.h || {
    echo "hfoc_mbedtls_test_secret.h не содержит key digest" >&2;
    exit 1; }
grep -Fq 'hfoc_mbedtls_test_secret_certificate' \
    kernel/include/hfoc_mbedtls_test_secret.h \
    kernel/src/hfoc_mbedtls_test_secret.c || {
    echo "test-secret boundary не объявляет certificate accessor" >&2;
    exit 1; }
grep -Fq 'hfoc_mbedtls_test_secret_key' \
    kernel/include/hfoc_mbedtls_test_secret.h \
    kernel/src/hfoc_mbedtls_test_secret.c || {
    echo "test-secret boundary не объявляет key accessor" >&2;
    exit 1; }
grep -Fq 'hfoc_mbedtls_test_secret_cert_der' \
    kernel/src/hfoc_mbedtls_test_secret.c || {
    echo "test-secret source не содержит certificate DER" >&2;
    exit 1; }
grep -Fq 'hfoc_mbedtls_test_secret_key_der' \
    kernel/src/hfoc_mbedtls_test_secret.c || {
    echo "test-secret source не содержит private-key DER" >&2;
    exit 1; }
grep -Fq 'hfoc_mbedtls_test_secret_evidence' \
    kernel/src/hfoc_mbedtls_test_secret.c || {
    echo "test-secret source не содержит evidence marker" >&2;
    exit 1; }
test -f scripts/verify-mbedtls-test-secret.sh || {
    echo "scripts/verify-mbedtls-test-secret.sh отсутствует" >&2; exit 1; }
bash -n scripts/verify-mbedtls-test-secret.sh || {
    echo "Синтаксис verify-mbedtls-test-secret.sh некорректен" >&2; exit 1; }
bash -n tests/test-mbedtls-secret-host.sh || {
    echo "Синтаксис test-mbedtls-secret-host.sh некорректен" >&2; exit 1; }
for mbedtls_secret_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'test-secret build separation|mbedTLS test-secret build separation' \
            "$mbedtls_secret_doc"; then
        echo "$mbedtls_secret_doc не документирует mbedTLS test-secret build separation" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **266. Test-secret build separation' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 266 завершённым" >&2
    exit 1
}

# mbedTLS BIO transport adapter contracts (item 267).
grep -Fq 'test-mbedtls-bio-host' Makefile || {
    echo "Makefile не содержит test-mbedtls-bio-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-mbedtls-bio-host' Makefile || {
    echo "test-host-suite не содержит test-mbedtls-bio-host" >&2;
    exit 1; }
grep -Fq 'HFOC_MBEDTLS_BIO_VERSION 1U' \
    kernel/include/hfoc_mbedtls_bio.h || {
    echo "hfoc_mbedtls_bio.h не содержит HFOC_MBEDTLS_BIO_VERSION 1U" >&2;
    exit 1; }
grep -Fq 'HFOC_MBEDTLS_BIO_SLOT_COUNT 4U' \
    kernel/include/hfoc_mbedtls_bio.h || {
    echo "hfoc_mbedtls_bio.h не содержит BIO slot count 4" >&2;
    exit 1; }
grep -Fq 'HFOC_MBEDTLS_BIO_MAX_SEND_BYTES' \
    kernel/include/hfoc_mbedtls_bio.h || {
    echo "hfoc_mbedtls_bio.h не содержит max send bytes" >&2;
    exit 1; }
grep -Fq 'HFOC_MBEDTLS_BIO_MAX_RECV_BYTES' \
    kernel/include/hfoc_mbedtls_bio.h || {
    echo "hfoc_mbedtls_bio.h не содержит max recv bytes" >&2;
    exit 1; }
grep -Fq 'hfoc_mbedtls_bio_init' \
    kernel/include/hfoc_mbedtls_bio.h kernel/src/hfoc_mbedtls_bio.c || {
    echo "mbedTLS BIO boundary не объявляет init" >&2;
    exit 1; }
grep -Fq 'hfoc_mbedtls_bio_attach' \
    kernel/include/hfoc_mbedtls_bio.h kernel/src/hfoc_mbedtls_bio.c || {
    echo "mbedTLS BIO boundary не объявляет attach" >&2;
    exit 1; }
grep -Fq 'hfoc_mbedtls_bio_detach' \
    kernel/include/hfoc_mbedtls_bio.h kernel/src/hfoc_mbedtls_bio.c || {
    echo "mbedTLS BIO boundary не объявляет detach" >&2;
    exit 1; }
grep -Fq 'hfoc_mbedtls_bio_send_cb' \
    kernel/include/hfoc_mbedtls_bio.h kernel/src/hfoc_mbedtls_bio.c || {
    echo "mbedTLS BIO boundary не объявляет send callback" >&2;
    exit 1; }
grep -Fq 'hfoc_mbedtls_bio_recv_cb' \
    kernel/include/hfoc_mbedtls_bio.h kernel/src/hfoc_mbedtls_bio.c || {
    echo "mbedTLS BIO boundary не объявляет recv callback" >&2;
    exit 1; }
grep -Fq 'HFOC_MBEDTLS_BIO_HOST_TEST' \
    kernel/src/hfoc_mbedtls_bio.c || {
    echo "mbedTLS BIO adapter не помечен HFOC_MBEDTLS_BIO_HOST_TEST" >&2;
    exit 1; }
grep -Fq 'MBEDTLS_ERR_SSL_WANT_WRITE' \
    kernel/src/hfoc_mbedtls_bio.c || {
    echo "mbedTLS BIO adapter не отображает retained send в WANT_WRITE" >&2;
    exit 1; }
grep -Fq 'MBEDTLS_ERR_SSL_WANT_READ' \
    kernel/src/hfoc_mbedtls_bio.c || {
    echo "mbedTLS BIO adapter не отображает retained recv в WANT_READ" >&2;
    exit 1; }
grep -Fq 'MBEDTLS_ERR_SSL_BAD_INPUT_DATA' \
    kernel/src/hfoc_mbedtls_bio.c || {
    echo "mbedTLS BIO adapter не возвращает стабильную ошибку" >&2;
    exit 1; }
if grep -nE '\b(errno|socket|malloc|calloc|free|kmalloc|kfree|pmm_alloc|serial_printf|printf|fprintf|sleep|delay|hlt|sti|cli|panic)\b' \
        kernel/src/hfoc_mbedtls_bio.c; then
    echo "mbedTLS BIO adapter использует POSIX, heap, диагностику или блокировку" >&2
    exit 1
fi
for mbedtls_bio_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'mbedTLS BIO transport adapter|BIO transport adapter' \
            "$mbedtls_bio_doc"; then
        echo "$mbedtls_bio_doc не документирует mbedTLS BIO transport adapter" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **267. TLS BIO transport adapter' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 267 завершённым" >&2
    exit 1
}

# TLS handshake state machine contracts (item 268).
grep -Fq 'test-tls-handshake-host' Makefile || {
    echo "Makefile не содержит test-tls-handshake-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-tls-handshake-host' Makefile || {
    echo "test-host-suite не содержит test-tls-handshake-host" >&2;
    exit 1; }
grep -Fq 'HFOC_TLS_HANDSHAKE_VERSION 1U' \
    kernel/include/hfoc_tls_handshake.h || {
    echo "hfoc_tls_handshake.h не содержит HFOC_TLS_HANDSHAKE_VERSION 1U" >&2;
    exit 1; }
grep -Fq 'HFOC_TLS_HANDSHAKE_SLOT_COUNT 4U' \
    kernel/include/hfoc_tls_handshake.h || {
    echo "hfoc_tls_handshake.h не содержит handshake slot count 4" >&2;
    exit 1; }
grep -Fq 'HFOC_TLS_HANDSHAKE_STATE_SUCCESS' \
    kernel/include/hfoc_tls_handshake.h || {
    echo "hfoc_tls_handshake.h не содержит success state" >&2;
    exit 1; }
grep -Fq 'HFOC_TLS_HANDSHAKE_STEP_COMPLETE' \
    kernel/include/hfoc_tls_handshake.h || {
    echo "hfoc_tls_handshake.h не содержит complete step result" >&2;
    exit 1; }
grep -Fq 'HFOC_TLS_HANDSHAKE_FAULT_DEADLINE_EXPIRED' \
    kernel/include/hfoc_tls_handshake.h || {
    echo "hfoc_tls_handshake.h не содержит deadline fault" >&2;
    exit 1; }
for handshake_symbol in hfoc_tls_handshake_init \
                        hfoc_tls_handshake_reserve \
                        hfoc_tls_handshake_configure \
                        hfoc_tls_handshake_progress \
                        hfoc_tls_handshake_release; do
    grep -Fq "$handshake_symbol" \
        kernel/include/hfoc_tls_handshake.h \
        kernel/src/hfoc_tls_handshake.c || {
        echo "TLS handshake boundary не объявляет $handshake_symbol" >&2;
        exit 1; }
done
grep -Fq 'HFOC_TLS_HANDSHAKE_HOST_TEST' \
    kernel/src/hfoc_tls_handshake.c || {
    echo "TLS handshake adapter не помечен HFOC_TLS_HANDSHAKE_HOST_TEST" >&2;
    exit 1; }
if grep -nE '\b(errno|socket|malloc|calloc|free|kmalloc|kfree|pmm_alloc|serial_printf|printf|fprintf|sleep|delay|hlt|sti|cli|panic)\b' \
        kernel/src/hfoc_tls_handshake.c; then
    echo "TLS handshake adapter использует POSIX, heap, диагностику или блокировку" >&2
    exit 1
fi
if grep -Eq '\b(mbedtls_ssl_|mbedtls_ssl_setup|mbedtls_ssl_handshake)\b' \
        kernel/src/hfoc_tls_handshake.c; then
    echo "TLS handshake adapter не должен компилировать mbedTLS SSL state" >&2
    exit 1
fi
for tls_handshake_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'TLS handshake state machine|tls handshake state machine' \
            "$tls_handshake_doc"; then
        echo "$tls_handshake_doc не документирует TLS handshake state machine" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **268. TLS handshake state machine' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 268 завершённым" >&2
    exit 1
}

# TLS close and error normalization contracts (item 269).
grep -Fq 'test-tls-close-host' Makefile || {
    echo "Makefile не содержит test-tls-close-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-tls-close-host' Makefile || {
    echo "test-host-suite не содержит test-tls-close-host" >&2;
    exit 1; }
grep -Fq 'HFOC_TLS_CLOSE_VERSION 1U' \
    kernel/include/hfoc_tls_close.h || {
    echo "hfoc_tls_close.h не содержит HFOC_TLS_CLOSE_VERSION 1U" >&2;
    exit 1; }
grep -Fq 'HFOC_TLS_CLOSE_SLOT_COUNT 4U' \
    kernel/include/hfoc_tls_close.h || {
    echo "hfoc_tls_close.h не содержит close slot count 4" >&2;
    exit 1; }
grep -Fq 'HFOC_TLS_CLOSE_STATE_CLOSE_NOTIFY_SENT' \
    kernel/include/hfoc_tls_close.h || {
    echo "hfoc_tls_close.h не содержит close-notify state" >&2;
    exit 1; }
grep -Fq 'HFOC_TLS_CLOSE_OUTCOME_TRANSPORT_REJECTED' \
    kernel/include/hfoc_tls_close.h || {
    echo "hfoc_tls_close.h не содержит transport-rejected outcome" >&2;
    exit 1; }
grep -Fq 'HFOC_TLS_CLOSE_ERROR_UNKNOWN' \
    kernel/include/hfoc_tls_close.h || {
    echo "hfoc_tls_close.h не содержит unknown error category" >&2;
    exit 1; }
for close_symbol in hfoc_tls_close_init \
                    hfoc_tls_close_reserve \
                    hfoc_tls_close_configure \
                    hfoc_tls_close_progress \
                    hfoc_tls_close_abort \
                    hfoc_tls_close_release \
                    hfoc_tls_close_error_normalize; do
    grep -Fq "$close_symbol" \
        kernel/include/hfoc_tls_close.h \
        kernel/src/hfoc_tls_close.c || {
        echo "TLS close boundary не объявляет $close_symbol" >&2;
        exit 1; }
done
grep -Fq 'HFOC_TLS_CLOSE_HOST_TEST' \
    kernel/src/hfoc_tls_close.c || {
    echo "TLS close adapter не помечен HFOC_TLS_CLOSE_HOST_TEST" >&2;
    exit 1; }
if grep -nE '\b(errno|socket|malloc|calloc|free|kmalloc|kfree|pmm_alloc|serial_printf|printf|fprintf|sleep|delay|hlt|sti|cli|panic)\b' \
        kernel/src/hfoc_tls_close.c; then
    echo "TLS close adapter использует POSIX, heap, диагностику или блокировку" >&2
    exit 1
fi
if grep -Eq '\b(mbedtls_ssl_|mbedtls_ssl_setup|mbedtls_ssl_handshake|mbedtls_ssl_close_notify)\b' \
        kernel/src/hfoc_tls_close.c; then
    echo "TLS close adapter не должен компилировать mbedTLS SSL state" >&2
    exit 1
fi
for tls_close_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'TLS close and error normalization|tls close and error normalization' \
            "$tls_close_doc"; then
        echo "$tls_close_doc не документирует TLS close and error normalization" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **269. TLS close and error normalization' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 269 завершённым" >&2
    exit 1
}
