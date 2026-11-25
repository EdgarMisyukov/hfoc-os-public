#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# HTTP error matrix contracts (item 279).
grep -Fq 'test-http-error-matrix-host' Makefile || {
    echo "Makefile не содержит test-http-error-matrix-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-http-error-matrix-host' Makefile || {
    echo "test-host-suite не содержит test-http-error-matrix-host" >&2;
    exit 1; }
grep -Fq 'HTTP_ERROR_MATRIX_VERSION 1U' \
    kernel/include/http_error_matrix.h || {
    echo "http_error_matrix.h не содержит HTTP_ERROR_MATRIX_VERSION 1U" >&2;
    exit 1; }
for error_matrix_status in HTTP_ERROR_MATRIX_OK \
                           HTTP_ERROR_MATRIX_NULL \
                           HTTP_ERROR_MATRIX_NOT_ERROR \
                           HTTP_ERROR_MATRIX_UNMAPPED; do
    grep -Fq "$error_matrix_status" \
        kernel/include/http_error_matrix.h || {
        echo "http_error_matrix.h не содержит $error_matrix_status" >&2;
        exit 1; }
done
for error_matrix_symbol in http_error_matrix_map_parser \
                           http_error_matrix_map_policy \
                           http_error_matrix_map_framing \
                           http_error_matrix_map_route \
                           http_error_matrix_status_string; do
    grep -Fq "$error_matrix_symbol" \
        kernel/include/http_error_matrix.h \
        kernel/src/http_error_matrix.c || {
        echo "HTTP error matrix не объявляет $error_matrix_symbol" >&2;
        exit 1; }
done
grep -Fq 'HTTP_ERROR_MATRIX_HOST_TEST' \
    kernel/src/http_error_matrix.c || {
    echo "HTTP error matrix не помечен HTTP_ERROR_MATRIX_HOST_TEST" >&2;
    exit 1; }
if grep -nE '\b(errno|socket|malloc|calloc|free|kmalloc|kfree|pmm_alloc|serial_printf|printf|fprintf|snprintf|sprintf|sleep|delay|hlt|sti|cli|panic)\b' \
        kernel/src/http_error_matrix.c; then
    echo "HTTP error matrix использует POSIX, heap, диагностику или блокировку" >&2
    exit 1
fi
for error_matrix_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'HTTP error matrix|error matrix' \
            "$error_matrix_doc"; then
        echo "$error_matrix_doc не документирует HTTP error matrix" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **279. HTTP error matrix' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 279 завершённым" >&2
    exit 1
}

# HTTP parser/encoder host acceptance contracts (item 280).
grep -Fq 'test-http-acceptance-host' Makefile || {
    echo "Makefile не содержит test-http-acceptance-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-http-acceptance-host' Makefile || {
    echo "test-host-suite не вызывает test-http-acceptance-host" >&2;
    exit 1; }
grep -Fq 'test-http-acceptance-sanitize-host' Makefile || {
    echo "Makefile не содержит test-http-acceptance-sanitize-host" >&2;
    exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-http-acceptance-sanitize-host' Makefile || {
    echo "test-host-suite не вызывает test-http-acceptance-sanitize-host" >&2;
    exit 1; }
grep -Fq 'HTTP parser/encoder host acceptance: ALL OK' \
    tests/http-acceptance-host.c || {
    echo "http-acceptance-host.c не публикует acceptance OK marker" >&2;
    exit 1; }
grep -Fq 'HTTP_PARSER_SLOT_COUNT' tests/http-acceptance-host.c || {
    echo "http-acceptance-host.c не использует fixed parser slot count" >&2;
    exit 1; }
grep -Fq '4096U' tests/http-acceptance-host.c || {
    echo "http-acceptance-host.c не содержит bounded parser fuzz iterations" >&2;
    exit 1; }
grep -Fq '1024U' tests/http-acceptance-host.c || {
    echo "http-acceptance-host.c не содержит bounded encoder fuzz iterations" >&2;
    exit 1; }
grep -Fq -- '-fsanitize=address,undefined' Makefile || {
    echo "Makefile не содержит HTTP acceptance sanitizer flags" >&2;
    exit 1; }
for http_acceptance_source in kernel/src/http_parser.c \
                              kernel/src/http_response.c \
                              kernel/src/http_error_matrix.c; do
    if grep -nE '\b(malloc|calloc|free|kmalloc|kfree|pmm_alloc|pmm_free|errno|printf|fprintf|snprintf|sprintf|sleep|delay|hlt|sti|cli|panic)\b' \
            "$http_acceptance_source"; then
        echo "$http_acceptance_source использует запрещённое выделение, POSIX, диагностику или блокировку" >&2
        exit 1
    fi
done
for acceptance_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'HTTP parser/encoder host acceptance|parser/encoder host acceptance' \
            "$acceptance_doc"; then
        echo "$acceptance_doc не документирует HTTP parser/encoder host acceptance" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **280. HTTP parser/encoder host acceptance' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 280 завершённым" >&2
    exit 1
}

# HTTPS service descriptor contracts (item 281).
grep -Fq 'test-https-service-descriptor-host' Makefile || {
    echo "Makefile не содержит test-https-service-descriptor-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-https-service-descriptor-host' Makefile || {
    echo "test-host-suite не вызывает test-https-service-descriptor-host" >&2;
    exit 1; }
grep -Fq 'HTTPS_SERVICE_REQUIRED_DEPENDENCY_MASK' \
    kernel/include/https_service.h || {
    echo "https_service.h не содержит required dependency mask" >&2; exit 1; }
grep -Fq 'HTTPS_SERVICE_CONNECTION_CAPACITY' \
    kernel/include/https_service.h || {
    echo "https_service.h не содержит connection capacity" >&2; exit 1; }
grep -Fq 'HTTPS_SERVICE_TLS_CONTEXT_CAPACITY' \
    kernel/include/https_service.h || {
    echo "https_service.h не содержит TLS context capacity" >&2; exit 1; }
grep -Fq 'HTTPS_SERVICE_HTTP_PARSER_CAPACITY' \
    kernel/include/https_service.h || {
    echo "https_service.h не содержит HTTP parser capacity" >&2; exit 1; }
grep -Fq 'HTTPS_SERVICE_READINESS_DEADLINE_DEFAULT_MS' \
    kernel/include/https_service.h || {
    echo "https_service.h не содержит readiness deadline" >&2; exit 1; }
grep -Fq 'https_service_descriptor_validate' \
    kernel/include/https_service.h kernel/src/https_service.c || {
    echo "HTTPS descriptor boundary не объявляет validate" >&2; exit 1; }
grep -Fq 'https_service_descriptor_init' \
    kernel/include/https_service.h kernel/src/https_service.c || {
    echo "HTTPS descriptor boundary не объявляет init" >&2; exit 1; }
grep -Fq 'https_service_start_check' \
    kernel/include/https_service.h kernel/src/https_service.c || {
    echo "HTTPS descriptor boundary не объявляет start check" >&2; exit 1; }
grep -Fq 'HTTPS service descriptor host test: ALL OK' \
    tests/https-service-descriptor-host.c || {
    echo "https-service-descriptor-host.c не публикует host OK marker" >&2;
    exit 1; }
grep -Fq 'HTTPS_SERVICE_DEPENDENCY_UNAVAILABLE' \
    tests/https-service-descriptor-host.c || {
    echo "https-service-descriptor-host.c не проверяет missing dependency" >&2;
    exit 1; }
if grep -nE '\b(malloc|calloc|free|kmalloc|kfree|pmm_alloc|pmm_free|errno|printf|fprintf|snprintf|sprintf|sleep|delay|hlt|sti|cli|panic)\b|\b(serial_write|console_write|event_ring_|audit_)\b' \
        kernel/src/https_service.c; then
    echo "https_service.c выполняет запрещённое выделение, диагностику, аудит или блокировку" >&2
    exit 1
fi
for https_descriptor_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'HTTPS service descriptor' "$https_descriptor_doc"; then
        echo "$https_descriptor_doc не документирует HTTPS service descriptor" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **281. HTTPS service descriptor' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 281 завершённым" >&2
    exit 1
}

# HTTPS admission transaction contracts (item 282).
grep -Fq 'test-https-admission-host' Makefile || {
    echo "Makefile не содержит test-https-admission-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-https-admission-host' Makefile || {
    echo "test-host-suite не вызывает test-https-admission-host" >&2;
    exit 1; }
grep -Fq 'HTTPS_ADMISSION_QEMU_ADDRESS_STRING' \
    kernel/include/https_admission.h || {
    echo "https_admission.h не содержит fixed QEMU address" >&2;
    exit 1; }
grep -Fq 'HTTPS_ADMISSION_QEMU_PORT' \
    kernel/include/https_admission.h || {
    echo "https_admission.h не содержит fixed QEMU port" >&2;
    exit 1; }
grep -Fq 'HTTPS_ADMISSION_GENERATION_MAX' \
    kernel/include/https_admission.h || {
    echo "https_admission.h не содержит bounded generation" >&2;
    exit 1; }
grep -Fq 'https_admission_transaction_init' \
    kernel/include/https_admission.h kernel/src/https_admission.c || {
    echo "HTTPS admission boundary не объявляет init" >&2;
    exit 1; }
grep -Fq 'https_admission_transaction_stage' \
    kernel/include/https_admission.h kernel/src/https_admission.c || {
    echo "HTTPS admission boundary не объявляет stage" >&2;
    exit 1; }
grep -Fq 'https_admission_transaction_commit' \
    kernel/include/https_admission.h kernel/src/https_admission.c || {
    echo "HTTPS admission boundary не объявляет commit" >&2;
    exit 1; }
grep -Fq 'https_admission_transaction_validate_state' \
    kernel/include/https_admission.h kernel/src/https_admission.c || {
    echo "HTTPS admission boundary не объявляет validate state" >&2;
    exit 1; }
grep -Fq 'HTTPS admission host test: ALL OK' \
    tests/https-admission-host.c || {
    echo "https-admission-host.c не публикует host OK marker" >&2;
    exit 1; }
grep -Fq 'invalid network generation stage leaves no pending' \
    tests/https-admission-host.c || {
    echo "https-admission-host.c не проверяет partial bind" >&2;
    exit 1; }
grep -Fq 'tampered pending commit clears pending and preserves active' \
    tests/https-admission-host.c || {
    echo "https-admission-host.c не проверяет atomic commit" >&2;
    exit 1; }
grep -Fq 'HTTPS_ADMISSION_CHANGED' \
    tests/https-admission-host.c || {
    echo "https-admission-host.c не проверяет observed-state mismatch" >&2;
    exit 1; }
if grep -nE '\b(malloc|calloc|free|kmalloc|kfree|pmm_alloc|pmm_free|errno|printf|fprintf|snprintf|sprintf|sleep|delay|hlt|sti|cli|panic)\b|\b(serial_write|console_write|event_ring_|audit_)\b' \
        kernel/src/https_admission.c; then
    echo "https_admission.c выполняет запрещённое выделение, диагностику, аудит или блокировку" >&2
    exit 1
fi
for https_admission_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'HTTPS admission transaction' "$https_admission_doc"; then
        echo "$https_admission_doc не документирует HTTPS admission transaction" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **282. HTTPS admission transaction' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 282 завершённым" >&2
    exit 1
}

# HTTPS connection composition contracts (item 283).
grep -Fq 'test-https-connection-host' Makefile || {
    echo "Makefile не содержит test-https-connection-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-https-connection-host' Makefile || {
    echo "test-host-suite не вызывает test-https-connection-host" >&2;
    exit 1; }
grep -Fq 'HTTPS_CONNECTION_VERSION' \
    kernel/include/https_connection.h || {
    echo "https_connection.h не содержит version" >&2; exit 1; }
grep -Fq 'HTTPS_CONNECTION_RESPONSE_CAPACITY' \
    kernel/include/https_connection.h || {
    echo "https_connection.h не содержит fixed response capacity" >&2;
    exit 1; }
grep -Fq 'https_connection_compose' \
    kernel/include/https_connection.h kernel/src/https_connection.c || {
    echo "HTTPS connection boundary не объявляет compose" >&2; exit 1; }
grep -Fq 'https_connection_release' \
    kernel/include/https_connection.h kernel/src/https_connection.c || {
    echo "HTTPS connection boundary не объявляет release" >&2; exit 1; }
grep -Fq 'https_connection_response_advance' \
    kernel/include/https_connection.h kernel/src/https_connection.c || {
    echo "HTTPS connection boundary не объявляет response advance" >&2;
    exit 1; }
grep -Fq 'HTTPS_CONNECTION_TCP_RESERVE_FAILED' \
    kernel/include/https_connection.h tests/https-connection-host.c || {
    echo "HTTPS connection contract не проверяет TCP stage failure" >&2;
    exit 1; }
grep -Fq 'HTTPS_CONNECTION_TLS_RESERVE_FAILED' \
    kernel/include/https_connection.h tests/https-connection-host.c || {
    echo "HTTPS connection contract не проверяет TLS stage failure" >&2;
    exit 1; }
grep -Fq 'HTTPS_CONNECTION_HTTP_RESERVE_FAILED' \
    kernel/include/https_connection.h tests/https-connection-host.c || {
    echo "HTTPS connection contract не проверяет HTTP stage failure" >&2;
    exit 1; }
grep -Fq 'HTTPS_CONNECTION_RESPONSE_BAD_CAPACITY' \
    kernel/include/https_connection.h tests/https-connection-host.c || {
    echo "HTTPS connection contract не проверяет response stage failure" >&2;
    exit 1; }
grep -Fq 'HTTPS connection composition host test: ALL OK' \
    tests/https-connection-host.c || {
    echo "https-connection-host.c не публикует host OK marker" >&2;
    exit 1; }
grep -Fq 'TLS stage failure releases reserved TCP slot' \
    tests/https-connection-host.c || {
    echo "https-connection-host.c не проверяет TLS reverse rollback" >&2;
    exit 1; }
grep -Fq 'HTTP stage failure releases TLS then TCP' \
    tests/https-connection-host.c || {
    echo "https-connection-host.c не проверяет HTTP reverse rollback" >&2;
    exit 1; }
grep -Fq 'response stage failure releases parser, TLS, then TCP' \
    tests/https-connection-host.c || {
    echo "https-connection-host.c не проверяет response reverse rollback" >&2;
    exit 1; }
grep -Fq 'HTTPS_CONNECTION_HOST_TEST' \
    kernel/src/https_connection.c || {
    echo "https_connection.c не помечен host-only boundary" >&2; exit 1; }
if grep -nE '\b(malloc|calloc|free|kmalloc|kfree|pmm_alloc|pmm_free|errno|printf|fprintf|snprintf|sprintf|sleep|delay|hlt|sti|cli|panic)\b|\b(serial_write|console_write|event_ring_|audit_)\b' \
        kernel/src/https_connection.c; then
    echo "https_connection.c выполняет запрещённое выделение, диагностику, аудит или блокировку" >&2
    exit 1
fi
for https_connection_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'HTTPS connection composition' "$https_connection_doc"; then
        echo "$https_connection_doc не документирует HTTPS connection composition" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **283. HTTPS connection composition' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 283 завершённым" >&2
    exit 1
}

# HTTPS TLS-to-HTTP data flow contracts (item 284).
grep -Fq 'test-https-data-flow-host' Makefile || {
    echo "Makefile не содержит test-https-data-flow-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-https-data-flow-host' Makefile || {
    echo "test-host-suite не вызывает test-https-data-flow-host" >&2;
    exit 1; }
grep -Fq 'HTTPS_DATA_FLOW_VERSION' \
    kernel/include/https_data_flow.h || {
    echo "https_data_flow.h не содержит version" >&2; exit 1; }
grep -Fq 'HTTPS_DATA_FLOW_MAX_REQUEST_BYTES' \
    kernel/include/https_data_flow.h || {
    echo "https_data_flow.h не содержит bounded request bytes" >&2;
    exit 1; }
grep -Fq 'https_data_flow_rx' \
    kernel/include/https_data_flow.h kernel/src/https_data_flow.c || {
    echo "HTTPS data-flow boundary не объявляет rx" >&2; exit 1; }
grep -Fq 'https_data_flow_tx' \
    kernel/include/https_data_flow.h kernel/src/https_data_flow.c || {
    echo "HTTPS data-flow boundary не объявляет tx" >&2; exit 1; }
grep -Fq 'https_data_flow_release' \
    kernel/include/https_data_flow.h kernel/src/https_data_flow.c || {
    echo "HTTPS data-flow boundary не объявляет release" >&2; exit 1; }
grep -Fq 'HTTPS_DATA_FLOW_STALE_GENERATION' \
    kernel/include/https_data_flow.h tests/https-data-flow-host.c || {
    echo "HTTPS data-flow contract не проверяет stale generation" >&2;
    exit 1; }
grep -Fq 'HTTPS_DATA_FLOW_TLS_RETAINED' \
    kernel/include/https_data_flow.h tests/https-data-flow-host.c || {
    echo "HTTPS data-flow contract не проверяет TLS retained" >&2;
    exit 1; }
grep -Fq 'HTTPS TLS-to-HTTP data flow host test: ALL OK' \
    tests/https-data-flow-host.c || {
    echo "https-data-flow-host.c не публикует host OK marker" >&2;
    exit 1; }
grep -Fq 'split-at-every-byte request reaches parser complete' \
    tests/https-data-flow-host.c || {
    echo "https-data-flow-host.c не проверяет incremental parser feed" >&2;
    exit 1; }
grep -Fq 'TLS short progress sends exact prefix without duplication' \
    tests/https-data-flow-host.c || {
    echo "https-data-flow-host.c не проверяет short progress" >&2;
    exit 1; }
grep -Fq 'TLS retained backpressure leaves response_sent unchanged' \
    tests/https-data-flow-host.c || {
    echo "https-data-flow-host.c не проверяет retained backpressure" >&2;
    exit 1; }
grep -Fq 'stale connection generation rejects old flow without parser mutation' \
    tests/https-data-flow-host.c || {
    echo "https-data-flow-host.c не проверяет stale generation isolation" >&2;
    exit 1; }
grep -Fq 'HTTPS_DATA_FLOW_HOST_TEST' \
    kernel/src/https_data_flow.c || {
    echo "https_data_flow.c не помечен host-only boundary" >&2; exit 1; }
if grep -nE '\b(malloc|calloc|free|kmalloc|kfree|pmm_alloc|pmm_free|errno|printf|fprintf|snprintf|sprintf|sleep|delay|hlt|sti|cli|panic)\b|\b(serial_write|console_write|event_ring_|audit_)\b' \
        kernel/src/https_data_flow.c; then
    echo "https_data_flow.c выполняет запрещённое выделение, диагностику, аудит или блокировку" >&2
    exit 1
fi
for https_data_flow_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'HTTPS TLS-to-HTTP data flow' "$https_data_flow_doc"; then
        echo "$https_data_flow_doc не документирует HTTPS TLS-to-HTTP data flow" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **284. TLS-to-HTTP data flow' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 284 завершённым" >&2
    exit 1
}

# HTTPS root and health handler contracts (item 285).
grep -Fq 'test-https-handler-host' Makefile || {
    echo "Makefile не содержит test-https-handler-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-https-handler-host' Makefile || {
    echo "host test suite не вызывает test-https-handler-host" >&2;
    exit 1; }
grep -Fq 'HTTPS_HANDLER_VERSION' \
    kernel/include/https_handler.h || {
    echo "https_handler.h не содержит version" >&2; exit 1; }
grep -Fq 'HTTPS_HANDLER_ROOT_BODY_MAX' \
    kernel/include/https_handler.h || {
    echo "https_handler.h не содержит bounded root body" >&2; exit 1; }
grep -Fq 'HTTPS_HANDLER_HEALTH_BODY_MAX' \
    kernel/include/https_handler.h || {
    echo "https_handler.h не содержит bounded health body" >&2; exit 1; }
grep -Fq 'https_handler_dispatch' \
    kernel/include/https_handler.h kernel/src/https_handler.c || {
    echo "HTTPS handler boundary не объявляет dispatch" >&2; exit 1; }
grep -Fq 'https_handler_encode' \
    kernel/include/https_handler.h kernel/src/https_handler.c || {
    echo "HTTPS handler boundary не объявляет encode" >&2; exit 1; }
grep -Fq 'HTTPS_HANDLER_FRAMING_DENIED' \
    kernel/include/https_handler.h tests/https-handler-host.c || {
    echo "HTTPS handler contract не проверяет framing denial" >&2;
    exit 1; }
grep -Fq 'HTTPS_HANDLER_POLICY_DENIED' \
    kernel/include/https_handler.h tests/https-handler-host.c || {
    echo "HTTPS handler contract не проверяет policy denial" >&2;
    exit 1; }
grep -Fq 'HTTPS_HANDLER_UNROUTED' \
    kernel/include/https_handler.h tests/https-handler-host.c || {
    echo "HTTPS handler contract не проверяет unknown route" >&2;
    exit 1; }
grep -Fq 'HTTPS root/health handler host test: ALL OK' \
    tests/https-handler-host.c || {
    echo "https-handler-host.c не публикует host OK marker" >&2;
    exit 1; }
grep -Fq 'valid dispatch publishes fixed route, method, and body' \
    tests/https-handler-host.c || {
    echo "https-handler-host.c не проверяет fixed dispatch" >&2;
    exit 1; }
grep -Fq 'body-bearing request is denied before handler dispatch' \
    tests/https-handler-host.c || {
    echo "https-handler-host.c не проверяет framing denial" >&2;
    exit 1; }
grep -Fq 'short handler output is rejected without mutation' \
    tests/https-handler-host.c || {
    echo "https-handler-host.c не проверяет response overflow" >&2;
    exit 1; }
grep -Fq 'HTTPS_HANDLER_HOST_TEST' \
    kernel/src/https_handler.c || {
    echo "https_handler.c не помечен host-only boundary" >&2; exit 1; }
if grep -nE '\b(malloc|calloc|free|kmalloc|kfree|pmm_alloc|pmm_free|errno|printf|fprintf|snprintf|sprintf|sleep|delay|hlt|sti|cli|panic)\b|\b(serial_write|console_write|event_ring_|audit_)\b' \
        kernel/src/https_handler.c; then
    echo "https_handler.c выполняет запрещённое выделение, диагностику, аудит или блокировку" >&2
    exit 1
fi
for https_handler_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'HTTPS root and health handlers' "$https_handler_doc"; then
        echo "$https_handler_doc не документирует HTTPS root and health handlers" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **285. Root and health handlers' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 285 завершённым" >&2
    exit 1
}

# HTTPS health/audit/metrics contracts (item 286).
grep -Fq 'test-https-health-host' Makefile || {
    echo "Makefile не содержит test-https-health-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-https-health-host' Makefile || {
    echo "host test suite не вызывает test-https-health-host" >&2;
    exit 1; }
grep -Fq 'HTTPS_HEALTH_VERSION' \
    kernel/include/https_health.h || {
    echo "https_health.h не содержит version" >&2; exit 1; }
grep -Fq 'HTTPS_HEALTH_CONNECTION_CAPACITY' \
    kernel/include/https_health.h || {
    echo "https_health.h не содержит connection capacity" >&2; exit 1; }
grep -Fq 'HTTPS_HEALTH_FAILURE_CATEGORY_COUNT' \
    kernel/include/https_health.h || {
    echo "https_health.h не содержит failure category count" >&2; exit 1; }
grep -Fq 'HTTPS_HEALTH_AUDIT_OPERATION' \
    kernel/include/https_health.h || {
    echo "https_health.h не содержит audit operation" >&2; exit 1; }
grep -Fq 'https_health_init' \
    kernel/include/https_health.h kernel/src/https_health.c || {
    echo "HTTPS health boundary не объявляет init" >&2; exit 1; }
grep -Fq 'https_health_publish' \
    kernel/include/https_health.h kernel/src/https_health.c || {
    echo "HTTPS health boundary не объявляет publish" >&2; exit 1; }
grep -Fq 'https_health_snapshot' \
    kernel/include/https_health.h kernel/src/https_health.c || {
    echo "HTTPS health boundary не объявляет snapshot" >&2; exit 1; }
grep -Fq 'HTTPS health/audit/metrics host test: ALL OK' \
    tests/https-health-host.c || {
    echo "https-health-host.c не публикует host OK marker" >&2;
    exit 1; }
grep -Fq 'fifth connection count is rejected' \
    tests/https-health-host.c || {
    echo "https-health-host.c не проверяет connection capacity" >&2;
    exit 1; }
grep -Fq 'audit record contains exact network metadata' \
    tests/https-health-host.c || {
    echo "https-health-host.c не проверяет audit metadata" >&2;
    exit 1; }
grep -Fq 'full ring failure leaves snapshot unchanged' \
    tests/https-health-host.c || {
    echo "https-health-host.c не проверяет full-ring isolation" >&2;
    exit 1; }
grep -Fq 'HTTPS_HEALTH_HOST_TEST' \
    kernel/src/https_health.c || {
    echo "https_health.c не помечен host-only boundary" >&2; exit 1; }
if grep -nE '\b(malloc|calloc|free|kmalloc|kfree|pmm_alloc|pmm_free|errno|printf|fprintf|snprintf|sprintf|sleep|delay|hlt|sti|cli|panic)\b|\b(serial_write|console_write)\b' \
        kernel/src/https_health.c; then
    echo "https_health.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
for https_health_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'HTTPS health/audit/metrics' "$https_health_doc"; then
        echo "$https_health_doc не документирует HTTPS health/audit/metrics" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **286. HTTPS health/audit/metrics' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 286 завершённым" >&2
    exit 1
}

# HTTPS fifth-connection denial contracts (item 287).
grep -Fq 'HTTPS_CONNECTION_CAPACITY' \
    kernel/include/https_connection.h || {
    echo "https_connection.h не содержит fixed connection capacity" >&2;
    exit 1; }
grep -Fq 'HTTPS_CONNECTION_CAPACITY_FULL' \
    kernel/include/https_connection.h tests/https-connection-host.c || {
    echo "HTTPS connection contract не проверяет fifth-connection denial" >&2;
    exit 1; }
grep -Fq 'https_connection_capacity' \
    kernel/include/https_connection.h kernel/src/https_connection.c || {
    echo "HTTPS connection boundary не объявляет capacity accessor" >&2;
    exit 1; }
grep -Fq 'https_connection_available' \
    kernel/include/https_connection.h kernel/src/https_connection.c || {
    echo "HTTPS connection boundary не объявляет available accessor" >&2;
    exit 1; }
grep -Fq 'https_connection_capacity_available' \
    kernel/include/https_connection.h kernel/src/https_connection.c || {
    echo "HTTPS connection boundary не объявляет capacity predicate" >&2;
    exit 1; }
grep -Fq 'fifth connection is rejected before any TLS allocation' \
    tests/https-connection-host.c || {
    echo "https-connection-host.c не проверяет pre-TLS fifth denial" >&2;
    exit 1; }
grep -Fq 'one cleanup restores exactly one admission slot' \
    tests/https-connection-host.c || {
    echo "https-connection-host.c не проверяет recovery after cleanup" >&2;
    exit 1; }
grep -Fq 'fifth denial and recovery restore exact initial counts' \
    tests/https-connection-host.c || {
    echo "https-connection-host.c не проверяет exact final counts" >&2;
    exit 1; }
for https_denial_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'HTTPS fifth-connection denial' "$https_denial_doc"; then
        echo "$https_denial_doc не документирует HTTPS fifth-connection denial" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **287. Fifth-connection denial' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 287 завершённым" >&2
    exit 1
}
