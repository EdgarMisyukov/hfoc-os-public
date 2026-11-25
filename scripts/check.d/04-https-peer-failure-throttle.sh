#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# HTTPS peer-failure throttle contracts (item 288).
grep -Fq 'test-https-peer-throttle-host' Makefile || {
    echo "Makefile не содержит test-https-peer-throttle-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-https-peer-throttle-host' Makefile || {
    echo "test-host-suite не содержит test-https-peer-throttle-host" >&2;
    exit 1; }
grep -Fq 'HTTPS_PEER_THROTTLE_VERSION' \
    kernel/include/https_peer_throttle.h || {
    echo "https_peer_throttle.h не содержит version" >&2; exit 1; }
grep -Fq 'HTTPS_PEER_THROTTLE_MAX_PEERS' \
    kernel/include/https_peer_throttle.h || {
    echo "https_peer_throttle.h не содержит maximum peer buckets" >&2;
    exit 1; }
grep -Fq 'HTTPS_PEER_THROTTLE_PEER_BLOCKED' \
    kernel/include/https_peer_throttle.h tests/https-peer-throttle-host.c || {
    echo "HTTPS peer throttle contract не проверяет blocked status" >&2;
    exit 1; }
grep -Fq 'struct https_peer_throttle_table' \
    kernel/include/https_peer_throttle.h || {
    echo "https_peer_throttle.h не содержит table descriptor" >&2; exit 1; }
for https_peer_throttle_symbol in \
        https_peer_throttle_init \
        https_peer_throttle_record_failure \
        https_peer_throttle_check \
        https_peer_throttle_reset \
        https_peer_throttle_local_allowed \
        https_peer_throttle_status_string; do
    grep -Fq "$https_peer_throttle_symbol" \
        kernel/include/https_peer_throttle.h kernel/src/https_peer_throttle.c || {
        echo "HTTPS peer throttle boundary не объявляет $https_peer_throttle_symbol" >&2
        exit 1; }
done
grep -Fq 'HTTPS peer-failure throttle host test: ALL OK' \
    tests/https-peer-throttle-host.c || {
    echo "https-peer-throttle-host.c не публикует host OK marker" >&2;
    exit 1; }
grep -Fq 'one abusive peer cannot block unrelated peers' \
    tests/https-peer-throttle-host.c || {
    echo "https-peer-throttle-host.c не проверяет unrelated peer isolation" >&2;
    exit 1; }
grep -Fq 'local management remains allowed while peer is blocked' \
    tests/https-peer-throttle-host.c || {
    echo "https-peer-throttle-host.c не проверяет local management guard" >&2;
    exit 1; }
grep -Fq 'blocked peer is allowed at exact expiry' \
    tests/https-peer-throttle-host.c || {
    echo "https-peer-throttle-host.c не проверяет cooldown expiry" >&2;
    exit 1; }
grep -Fq 'repeated blocked failures do not extend cooldown' \
    tests/https-peer-throttle-host.c || {
    echo "https-peer-throttle-host.c не проверяет no extension" >&2;
    exit 1; }
grep -Fq 'full table admits a new peer by replacing oldest bucket' \
    tests/https-peer-throttle-host.c || {
    echo "https-peer-throttle-host.c не проверяет bounded replacement" >&2;
    exit 1; }
grep -Fq 'HTTPS_PEER_THROTTLE_HOST_TEST' \
    kernel/src/https_peer_throttle.c || {
    echo "https_peer_throttle.c не помечен host-only boundary" >&2;
    exit 1; }
if grep -nE '\b(malloc|calloc|free|kmalloc|kfree|pmm_alloc|pmm_free|errno|printf|fprintf|snprintf|sprintf|sleep|delay|hlt|sti|cli|panic)\b|\b(serial_write|console_write)\b' \
        kernel/src/https_peer_throttle.c; then
    echo "https_peer_throttle.c выполняет запрещённое выделение, диагностику или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf 2>/dev/null |
        grep 'https_peer_throttle_'; then
    echo "Production kernel содержит HTTPS peer throttle symbol" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'HTTPS peer-failure throttle host test: ALL OK'; then
    echo "Production kernel содержит HTTPS peer throttle host marker" >&2
    exit 1
fi
if ! printf '#include "https_peer_throttle.h"\nvoid hfoc_https_peer_throttle_header(void) { (void)HTTPS_PEER_THROTTLE_VERSION; }\n' |
    gcc -std=c11 -Wall -Wextra -Werror -Wpedantic -ffreestanding \
        -Ikernel/include -x c -c - -o /dev/null; then
    echo "https_peer_throttle.h не компилируется как self-contained header" >&2
    exit 1
fi
for https_peer_throttle_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'HTTPS peer-failure throttle' "$https_peer_throttle_doc"; then
        echo "$https_peer_throttle_doc не документирует HTTPS peer-failure throttle" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **288. Peer-failure throttle' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 288 завершённым" >&2
    exit 1
}

# HTTPS graceful stop and exact restart contracts (item 289).
grep -Fq 'test-https-service-lifecycle-host' Makefile || {
    echo "Makefile не содержит test-https-service-lifecycle-host" >&2;
    exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-https-service-lifecycle-host' Makefile || {
    echo "test-host-suite не содержит test-https-service-lifecycle-host" >&2;
    exit 1; }
grep -Fq 'HTTPS_SERVICE_LIFECYCLE_VERSION' \
    kernel/include/https_service_lifecycle.h || {
    echo "https_service_lifecycle.h не содержит version" >&2; exit 1; }
grep -Fq 'HTTPS_SERVICE_LIFECYCLE_PHASE_COUNT' \
    kernel/include/https_service_lifecycle.h || {
    echo "https_service_lifecycle.h не содержит phase count" >&2; exit 1; }
grep -Fq 'HTTPS_SERVICE_LIFECYCLE_DRAIN_TIMEOUT_MAX' \
    kernel/include/https_service_lifecycle.h || {
    echo "https_service_lifecycle.h не содержит bounded drain timeout" >&2;
    exit 1; }
grep -Fq 'HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY' \
    kernel/include/https_service_lifecycle.h || {
    echo "https_service_lifecycle.h не содержит fixed connection capacity" >&2;
    exit 1; }
grep -Fq 'struct https_service_lifecycle' \
    kernel/include/https_service_lifecycle.h || {
    echo "https_service_lifecycle.h не содержит lifecycle descriptor" >&2;
    exit 1; }
grep -Fq 'struct https_service_lifecycle_ops' \
    kernel/include/https_service_lifecycle.h || {
    echo "https_service_lifecycle.h не содержит callback ops" >&2;
    exit 1; }
for https_service_lifecycle_symbol in \
        https_service_lifecycle_init \
        https_service_lifecycle_start \
        https_service_lifecycle_stop_admission \
        https_service_lifecycle_step \
        https_service_lifecycle_finish_drain \
        https_service_lifecycle_restart \
        https_service_lifecycle_snapshot \
        https_service_lifecycle_phase_string \
        https_service_lifecycle_status_string; do
    grep -Fq "$https_service_lifecycle_symbol" \
        kernel/include/https_service_lifecycle.h kernel/src/https_service_lifecycle.c || {
        echo "HTTPS lifecycle boundary не объявляет $https_service_lifecycle_symbol" >&2
        exit 1; }
done
grep -Fq 'HTTPS graceful stop and restart host test: ALL OK' \
    tests/https-service-lifecycle-host.c || {
    echo "https-service-lifecycle-host.c не публикует host OK marker" >&2;
    exit 1; }
grep -Fq 'HTTPS lifecycle deadline aborts remaining slots exactly' \
    tests/https-service-lifecycle-host.c || {
    echo "https-service-lifecycle-host.c не проверяет deadline abort" >&2;
    exit 1; }
grep -Fq 'HTTPS lifecycle finish zeroizes TLS and releases HTTP/packet' \
    tests/https-service-lifecycle-host.c || {
    echo "https-service-lifecycle-host.c не проверяет exact cleanup" >&2;
    exit 1; }
grep -Fq 'HTTPS lifecycle restart publishes exact initial counters' \
    tests/https-service-lifecycle-host.c || {
    echo "https-service-lifecycle-host.c не проверяет exact restart" >&2;
    exit 1; }
grep -Fq 'HTTPS lifecycle finish before empty is rejected without cleanup' \
    tests/https-service-lifecycle-host.c || {
    echo "https-service-lifecycle-host.c не проверяет unfinished drain" >&2;
    exit 1; }
grep -Fq 'HTTPS lifecycle repeated cycles restore exact initial counters' \
    tests/https-service-lifecycle-host.c || {
    echo "https-service-lifecycle-host.c не проверяет repeat cycles" >&2;
    exit 1; }
grep -Fq 'HTTPS_SERVICE_LIFECYCLE_HOST_TEST' \
    kernel/src/https_service_lifecycle.c || {
    echo "https_service_lifecycle.c не помечен host-only boundary" >&2;
    exit 1; }
if grep -nE '\b(malloc|calloc|free|kmalloc|kfree|pmm_alloc|pmm_free|errno|printf|fprintf|snprintf|sprintf|sleep|delay|hlt|sti|cli|panic)\b|\b(serial_write|console_write)\b' \
        kernel/src/https_service_lifecycle.c; then
    echo "https_service_lifecycle.c выполняет запрещённое выделение, диагностику или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf 2>/dev/null |
        grep 'https_service_lifecycle_'; then
    echo "Production kernel содержит HTTPS service lifecycle symbol" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'HTTPS graceful stop and restart host test: ALL OK'; then
    echo "Production kernel содержит HTTPS lifecycle host marker" >&2
    exit 1
fi
if ! printf '#include "https_service_lifecycle.h"\nvoid hfoc_https_service_lifecycle_header(void) { (void)HTTPS_SERVICE_LIFECYCLE_VERSION; }\n' |
    gcc -std=c11 -Wall -Wextra -Werror -Wpedantic -ffreestanding \
        -Ikernel/include -x c -c - -o /dev/null; then
    echo "https_service_lifecycle.h не компилируется как self-contained header" >&2
    exit 1
fi
for https_service_lifecycle_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'HTTPS graceful stop and exact restart' "$https_service_lifecycle_doc"; then
        echo "$https_service_lifecycle_doc не документирует HTTPS graceful stop and exact restart" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **289. Graceful stop and exact restart' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 289 завершённым" >&2
    exit 1
}

# HTTPS service QEMU proof contracts (item 290).
grep -Fq 'test-https-service-qemu' Makefile || {
    echo "Makefile не содержит test-https-service-qemu" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-https-service-qemu' Makefile || {
    echo "Makefile не вызывает test-https-service-qemu" >&2; exit 1; }
grep -Fq 'HTTPS_SERVICE_QEMU_KERNEL' Makefile || {
    echo "Makefile не содержит HTTPS_SERVICE_QEMU_KERNEL" >&2; exit 1; }
grep -Fq 'HTTPS_SERVICE_QEMU_ISO' Makefile || {
    echo "Makefile не содержит HTTPS_SERVICE_QEMU_ISO" >&2; exit 1; }
grep -Fq 'HTTPS_SERVICE_QEMU_CFLAGS' Makefile || {
    echo "Makefile не содержит HTTPS_SERVICE_QEMU_CFLAGS" >&2; exit 1; }
grep -Fq 'HFOC_HTTPS_SERVICE_QEMU_TEST' \
    Makefile kernel/src/virtio_net_frame_qemu.c || {
    echo "HTTPS service QEMU proof не определяет HFOC_HTTPS_SERVICE_QEMU_TEST" >&2;
    exit 1; }
test -x scripts/test-qemu-https-service.sh || {
    echo "scripts/test-qemu-https-service.sh не является исполняемым" >&2;
    exit 1; }
bash -n scripts/test-qemu-https-service.sh || {
    echo "Синтаксис scripts/test-qemu-https-service.sh некорректен" >&2;
    exit 1; }
grep -Fqi 'fifth denial before TLS allocation' \
    scripts/test-qemu-https-service.sh || {
    echo "HTTPS QEMU script не проверяет fifth denial before TLS allocation" >&2;
    exit 1; }
grep -Fq '[HTTPS-QEMU] proof OK' \
    kernel/src/virtio_net_frame_qemu.c scripts/test-qemu-https-service.sh || {
    echo "HTTPS QEMU proof не публикует proof OK marker" >&2; exit 1; }
for https_qemu_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'HTTPS service QEMU proof' "$https_qemu_doc"; then
        echo "$https_qemu_doc не документирует HTTPS service QEMU proof" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **290. HTTPS service QEMU proof' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 290 завершённым" >&2
    exit 1
}

# Standard HTTPS client harness contracts (item 291).
grep -Fq 'test-https-client-qemu' Makefile || {
    echo "Makefile не содержит test-https-client-qemu" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-https-client-qemu' Makefile || {
    echo "Makefile не вызывает test-https-client-qemu" >&2; exit 1; }
grep -Fq 'HTTPS_SERVICE_QEMU_ISO' Makefile scripts/test-qemu-https-client.sh \
    || {
    echo "HTTPS client harness не переиспользует HTTPS_SERVICE_QEMU_ISO" >&2;
    exit 1; }
test -x scripts/test-qemu-https-client.sh || {
    echo "scripts/test-qemu-https-client.sh не является исполняемым" >&2;
    exit 1; }
bash -n scripts/test-qemu-https-client.sh || {
    echo "Синтаксис scripts/test-qemu-https-client.sh некорректен" >&2;
    exit 1; }
grep -Fq 'curl' scripts/test-qemu-https-client.sh || {
    echo "HTTPS client harness не использует curl" >&2; exit 1; }
grep -Fq 'openssl' scripts/test-qemu-https-client.sh || {
    echo "HTTPS client harness не использует openssl" >&2; exit 1; }
grep -Fq 'hfoc.test:18443' scripts/test-qemu-https-client.sh || {
    echo "HTTPS client harness не фиксирует hfoc.test:18443" >&2; exit 1; }
grep -Fqi 'hostname mismatch' scripts/test-qemu-https-client.sh || {
    echo "HTTPS client harness не проверяет hostname mismatch" >&2; exit 1; }
grep -Fqi 'untrusted CA' scripts/test-qemu-https-client.sh || {
    echo "HTTPS client harness не проверяет untrusted CA" >&2; exit 1; }
for https_client_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'Standard HTTPS client harness' "$https_client_doc"; then
        echo "$https_client_doc не документирует Standard HTTPS client harness" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **291. Standard HTTPS client harness' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 291 завершённым" >&2
    exit 1
}

# Exact HTTPS successful response matrix contracts (item 292).
grep -Fq 'test-https-response-matrix-qemu' Makefile || {
    echo "Makefile не содержит test-https-response-matrix-qemu" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-https-response-matrix-qemu' Makefile || {
    echo "Makefile не вызывает test-https-response-matrix-qemu" >&2;
    exit 1; }
grep -Fq 'HTTPS_SERVICE_QEMU_ISO' Makefile \
    scripts/test-qemu-https-response-matrix.sh || {
    echo "HTTPS response matrix harness не переиспользует HTTPS_SERVICE_QEMU_ISO" >&2;
    exit 1; }
test -x scripts/test-qemu-https-response-matrix.sh || {
    echo "scripts/test-qemu-https-response-matrix.sh не является исполняемым" >&2;
    exit 1; }
bash -n scripts/test-qemu-https-response-matrix.sh || {
    echo "Синтаксис scripts/test-qemu-https-response-matrix.sh некорректен" >&2;
    exit 1; }
grep -Fq 'Content-Length: 1611' scripts/test-qemu-https-response-matrix.sh || {
    echo "HTTPS response matrix не проверяет rendered root Content-Length 1593" >&2;
    exit 1; }
grep -Fq 'Content-Length: 32' scripts/test-qemu-https-response-matrix.sh || {
    echo "HTTPS response matrix не проверяет health Content-Length 32" >&2;
    exit 1; }
grep -Fq 'HEAD /' scripts/test-qemu-https-response-matrix.sh || {
    echo "HTTPS response matrix не проверяет HEAD zero-body" >&2;
    exit 1; }
grep -Fq 'Health JSON schema' scripts/test-qemu-https-response-matrix.sh || {
    echo "HTTPS response matrix не проверяет health schema" >&2;
    exit 1; }
for response_matrix_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'Exact HTTPS successful response matrix' \
            "$response_matrix_doc"; then
        echo "$response_matrix_doc не документирует exact HTTPS response matrix" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **292. Exact successful-response matrix' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 292 завершённым" >&2
    exit 1
}

# Encrypted HTTPS malformed-request matrix contracts (item 293).
grep -Fq 'test-https-malformed-request-matrix-qemu' Makefile || {
    echo "Makefile не содержит test-https-malformed-request-matrix-qemu" >&2;
    exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-https-malformed-request-matrix-qemu' Makefile || {
    echo "Makefile не вызывает test-https-malformed-request-matrix-qemu" >&2;
    exit 1; }
grep -Fq 'HTTPS_SERVICE_QEMU_ISO' Makefile \
    scripts/test-qemu-https-malformed-request-matrix.sh || {
    echo "HTTPS malformed-request matrix harness не переиспользует HTTPS_SERVICE_QEMU_ISO" >&2;
    exit 1; }
test -x scripts/test-qemu-https-malformed-request-matrix.sh || {
    echo "scripts/test-qemu-https-malformed-request-matrix.sh не является исполняемым" >&2;
    exit 1; }
bash -n scripts/test-qemu-https-malformed-request-matrix.sh || {
    echo "Синтаксис scripts/test-qemu-https-malformed-request-matrix.sh некорректен" >&2;
    exit 1; }
grep -Fq 'Split valid root exact response' \
    scripts/test-qemu-https-malformed-request-matrix.sh || {
    echo "HTTPS malformed-request matrix не проверяет split valid root" >&2;
    exit 1; }
grep -Fq 'Split control-character rejection' \
    scripts/test-qemu-https-malformed-request-matrix.sh || {
    echo "HTTPS malformed-request matrix не проверяет split control-character" >&2;
    exit 1; }
grep -Fq 'Invalid Host rejection' \
    scripts/test-qemu-https-malformed-request-matrix.sh || {
    echo "HTTPS malformed-request matrix не проверяет invalid Host" >&2;
    exit 1; }
grep -Fq 'Unsupported version rejection' \
    scripts/test-qemu-https-malformed-request-matrix.sh || {
    echo "HTTPS malformed-request matrix не проверяет unsupported version" >&2;
    exit 1; }
grep -Fq 'Unsupported method rejection' \
    scripts/test-qemu-https-malformed-request-matrix.sh || {
    echo "HTTPS malformed-request matrix не проверяет unsupported method" >&2;
    exit 1; }
grep -Fq 'Framing denial' \
    scripts/test-qemu-https-malformed-request-matrix.sh || {
    echo "HTTPS malformed-request matrix не проверяет framing denial" >&2;
    exit 1; }
grep -Fq 'Upgrade denial' \
    scripts/test-qemu-https-malformed-request-matrix.sh || {
    echo "HTTPS malformed-request matrix не проверяет upgrade denial" >&2;
    exit 1; }
grep -Fq 'Unknown route rejection' \
    scripts/test-qemu-https-malformed-request-matrix.sh || {
    echo "HTTPS malformed-request matrix не проверяет unknown route" >&2;
    exit 1; }
for malformed_matrix_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'Encrypted HTTPS malformed-request matrix' \
            "$malformed_matrix_doc"; then
        echo "$malformed_matrix_doc не документирует encrypted HTTPS malformed-request matrix" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **293. Encrypted malformed-request matrix' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 293 завершённым" >&2
    exit 1
}

# Encrypted HTTPS size-boundary matrix contracts (item 294).
grep -Fq 'test-https-size-boundary-matrix-qemu' Makefile || {
    echo "Makefile не содержит test-https-size-boundary-matrix-qemu" >&2;
    exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-https-size-boundary-matrix-qemu' Makefile || {
    echo "Makefile не вызывает test-https-size-boundary-matrix-qemu" >&2;
    exit 1; }
grep -Fq 'HTTPS_SERVICE_QEMU_ISO' Makefile \
    scripts/test-qemu-https-size-boundary-matrix.sh || {
    echo "HTTPS size-boundary matrix harness не переиспользует HTTPS_SERVICE_QEMU_ISO" >&2;
    exit 1; }
test -x scripts/test-qemu-https-size-boundary-matrix.sh || {
    echo "scripts/test-qemu-https-size-boundary-matrix.sh не является исполняемым" >&2;
    exit 1; }
bash -n scripts/test-qemu-https-size-boundary-matrix.sh || {
    echo "Синтаксис scripts/test-qemu-https-size-boundary-matrix.sh некорректен" >&2;
    exit 1; }
grep -Fq '1023-byte request line accepted' \
    scripts/test-qemu-https-size-boundary-matrix.sh || {
    echo "HTTPS size-boundary matrix не проверяет request line below" >&2;
    exit 1; }
grep -Fq '1024-byte request line accepted' \
    scripts/test-qemu-https-size-boundary-matrix.sh || {
    echo "HTTPS size-boundary matrix не проверяет request line exact" >&2;
    exit 1; }
grep -Fq '1025-byte request line overflow' \
    scripts/test-qemu-https-size-boundary-matrix.sh || {
    echo "HTTPS size-boundary matrix не проверяет request line above" >&2;
    exit 1; }
grep -Fq '4095-byte header section accepted' \
    scripts/test-qemu-https-size-boundary-matrix.sh || {
    echo "HTTPS size-boundary matrix не проверяет header section below" >&2;
    exit 1; }
grep -Fq '4096-byte header section accepted' \
    scripts/test-qemu-https-size-boundary-matrix.sh || {
    echo "HTTPS size-boundary matrix не проверяет header section exact" >&2;
    exit 1; }
grep -Fq '4097-byte header section overflow' \
    scripts/test-qemu-https-size-boundary-matrix.sh || {
    echo "HTTPS size-boundary matrix не проверяет header section above" >&2;
    exit 1; }
grep -Fq 'Parser recovery after each boundary' \
    scripts/test-qemu-https-size-boundary-matrix.sh || {
    echo "HTTPS size-boundary matrix не проверяет parser recovery" >&2;
    exit 1; }
for size_matrix_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'Encrypted HTTPS size-boundary matrix' \
            "$size_matrix_doc"; then
        echo "$size_matrix_doc не документирует encrypted HTTPS size-boundary matrix" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **294. Encrypted size-boundary matrix' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 294 завершённым" >&2
    exit 1
}

# Concurrent HTTPS exhaustion matrix contracts (item 295).
grep -Fq 'test-https-exhaustion-matrix-qemu' Makefile || {
    echo "Makefile не содержит test-https-exhaustion-matrix-qemu" >&2;
    exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-https-exhaustion-matrix-qemu' Makefile || {
    echo "Makefile не вызывает test-https-exhaustion-matrix-qemu" >&2;
    exit 1; }
grep -Fq 'HTTPS_SERVICE_QEMU_ISO' Makefile \
    scripts/test-qemu-https-exhaustion-matrix.sh || {
    echo "HTTPS exhaustion matrix harness не переиспользует HTTPS_SERVICE_QEMU_ISO" >&2;
    exit 1; }
test -x scripts/test-qemu-https-exhaustion-matrix.sh || {
    echo "scripts/test-qemu-https-exhaustion-matrix.sh не является исполняемым" >&2;
    exit 1; }
bash -n scripts/test-qemu-https-exhaustion-matrix.sh || {
    echo "Синтаксис scripts/test-qemu-https-exhaustion-matrix.sh некорректен" >&2;
    exit 1; }
grep -Fq 'Four held connections including TLS handshake' \
    scripts/test-qemu-https-exhaustion-matrix.sh || {
    echo "HTTPS exhaustion matrix не проверяет four held connections" >&2;
    exit 1; }
grep -Fq 'Fifth denial before TLS allocation' \
    scripts/test-qemu-https-exhaustion-matrix.sh || {
    echo "HTTPS exhaustion matrix не проверяет fifth denial" >&2;
    exit 1; }
grep -Fq 'Packet/TX pressure high-water' \
    scripts/test-qemu-https-exhaustion-matrix.sh || {
    echo "HTTPS exhaustion matrix не проверяет packet/TX pressure" >&2;
    exit 1; }
grep -Fq 'Exact slot release after one close' \
    scripts/test-qemu-https-exhaustion-matrix.sh || {
    echo "HTTPS exhaustion matrix не проверяет exact slot release" >&2;
    exit 1; }
grep -Fq 'Recovery TLS connection and root response' \
    scripts/test-qemu-https-exhaustion-matrix.sh || {
    echo "HTTPS exhaustion matrix не проверяет recovery root response" >&2;
    exit 1; }
grep -Fq 'Balanced packet/pool/allocator counts' \
    scripts/test-qemu-https-exhaustion-matrix.sh || {
    echo "HTTPS exhaustion matrix не проверяет balanced counts" >&2;
    exit 1; }
for marker in \
    '[HTTPS-EXHAUSTION-MATRIX] harness ready' \
    '[HTTPS-QEMU] packet pressure ok' \
    '[HTTPS-EXHAUSTION-MATRIX] release ok' \
    '[HTTPS-QEMU] exhaustion recovery ok' \
    '[HTTPS-EXHAUSTION-MATRIX] balanced counts'; do
    if ! grep -Fq "$marker" kernel/src/virtio_net_frame_qemu.c \
            scripts/test-qemu-https-exhaustion-matrix.sh; then
        echo "HTTPS exhaustion matrix не публикует $marker" >&2
        exit 1
    fi
done
for exhaustion_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'Concurrent HTTPS exhaustion matrix' "$exhaustion_doc"; then
        echo "$exhaustion_doc не документирует concurrent HTTPS exhaustion matrix" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **295. Concurrent exhaustion matrix' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 295 завершённым" >&2
    exit 1
}

# Entropy fail-closed matrix contracts (item 296).
grep -Fq 'test-entropy-fail-closed-matrix-qemu' Makefile || {
    echo "Makefile не содержит test-entropy-fail-closed-matrix-qemu" >&2;
    exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-entropy-fail-closed-matrix-qemu' Makefile || {
    echo "Makefile не вызывает test-entropy-fail-closed-matrix-qemu" >&2;
    exit 1; }
grep -Fq 'HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO' Makefile || {
    echo "Makefile не содержит HTTPS_ENTROPY_FAIL_CLOSED_QEMU_ISO" >&2;
    exit 1; }
grep -Fq 'HFOC_HTTPS_ENTROPY_FAIL_CLOSED_QEMU_TEST' \
    Makefile kernel/src/virtio_net_frame_qemu.c || {
    echo "Entropy fail-closed matrix не определяет HFOC_HTTPS_ENTROPY_FAIL_CLOSED_QEMU_TEST" >&2;
    exit 1; }
test -x scripts/test-qemu-entropy-fail-closed-matrix.sh || {
    echo "scripts/test-qemu-entropy-fail-closed-matrix.sh не является исполняемым" >&2;
    exit 1; }
bash -n scripts/test-qemu-entropy-fail-closed-matrix.sh || {
    echo "Синтаксис scripts/test-qemu-entropy-fail-closed-matrix.sh некорректен" >&2;
    exit 1; }
grep -Fq 'virtio_rng=absent' \
    scripts/test-qemu-entropy-fail-closed-matrix.sh || {
    echo "Entropy fail-closed matrix не запускается без virtio-rng" >&2;
    exit 1; }
grep -Fq 'listener_closed' \
    scripts/test-qemu-entropy-fail-closed-matrix.sh || {
    echo "Entropy fail-closed matrix не проверяет закрытый listener" >&2;
    exit 1; }
for marker in \
    '[ENTROPY-FAIL-CLOSED] proof init' \
    '[ENTROPY-FAIL-CLOSED] missing device denial' \
    '[ENTROPY-FAIL-CLOSED] rejection ok' \
    '[ENTROPY-FAIL-CLOSED] stall ok' \
    '[ENTROPY-FAIL-CLOSED] reset ok' \
    '[ENTROPY-FAIL-CLOSED] stale generation denied' \
    '[ENTROPY-FAIL-CLOSED] listener closed' \
    '[ENTROPY-FAIL-CLOSED] local operation healthy' \
    '[ENTROPY-FAIL-CLOSED] evidence bounded' \
    '[ENTROPY-FAIL-CLOSED] recovery generation ok' \
    '[ENTROPY-FAIL-CLOSED] matrix ok'; do
    if ! grep -Fq "$marker" kernel/src/virtio_net_frame_qemu.c \
            scripts/test-qemu-entropy-fail-closed-matrix.sh; then
        echo "Entropy fail-closed matrix не публикует $marker" >&2
        exit 1
    fi
done
grep -Fq '[t]est-qemu-entropy-fail-closed-matrix' \
    .github/workflows/ci.yml || {
    echo "CI не содержит test-qemu-entropy-fail-closed-matrix leak detection" >&2;
    exit 1; }
for entropy_fail_closed_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'Entropy fail-closed matrix' "$entropy_fail_closed_doc"; then
        echo "$entropy_fail_closed_doc не документирует entropy fail-closed matrix" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **296. Entropy fail-closed matrix' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 296 завершённым" >&2
    exit 1
}
