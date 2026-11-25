#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# TLS 1.3 QEMU proof contracts (item 270).
grep -Fq 'test-tls-qemu' Makefile || {
    echo "Makefile не содержит test-tls-qemu" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-tls-qemu' Makefile || {
    echo "test-serial не содержит test-tls-qemu" >&2;
    exit 1; }
grep -Fq 'MBEDTLS_TLS_QEMU_KERNEL' Makefile || {
    echo "Makefile не содержит MBEDTLS_TLS_QEMU_KERNEL" >&2; exit 1; }
grep -Fq 'MBEDTLS_TLS_QEMU_ISO' Makefile || {
    echo "Makefile не содержит MBEDTLS_TLS_QEMU_ISO" >&2; exit 1; }
grep -Fq 'MBEDTLS_TLS_QEMU_CFLAGS' Makefile || {
    echo "Makefile не содержит MBEDTLS_TLS_QEMU_CFLAGS" >&2; exit 1; }
grep -Fq 'HFOC_MBEDTLS_TLS_QEMU_TEST' \
    Makefile kernel/src/virtio_net_frame_qemu.c || {
    echo "TLS QEMU proof не определяет HFOC_MBEDTLS_TLS_QEMU_TEST" >&2;
    exit 1; }
grep -Fq 'HFOC_MBEDTLS_TEST_SECRET_QEMU_TEST' \
    Makefile kernel/src/hfoc_mbedtls_test_secret.c || {
    echo "TLS QEMU proof не включает test-secret build separation" >&2;
    exit 1; }
test -f tests/hfoc-tls-qemu-config.h || {
    echo "tests/hfoc-tls-qemu-config.h отсутствует" >&2; exit 1; }
grep -Fq '#define MBEDTLS_PSA_CRYPTO_C' \
    tests/hfoc-tls-qemu-config.h || {
    echo "TLS QEMU test config не включает PSA crypto" >&2; exit 1; }
grep -Fq '#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG' \
    tests/hfoc-tls-qemu-config.h || {
    echo "TLS QEMU test config не включает external RNG" >&2; exit 1; }
grep -Fq '#define MBEDTLS_USE_PSA_CRYPTO' \
    tests/hfoc-tls-qemu-config.h || {
    echo "TLS QEMU test config не включает PSA crypto use" >&2; exit 1; }
grep -Fq '#define MBEDTLS_HAVE_INT32' \
    tests/hfoc-tls-qemu-config.h || {
    echo "TLS QEMU test config не включает 32-bit bignum" >&2; exit 1; }
test -x scripts/test-qemu-tls.sh || {
    echo "scripts/test-qemu-tls.sh не является исполняемым" >&2; exit 1; }
bash -n scripts/test-qemu-tls.sh || {
    echo "Синтаксис scripts/test-qemu-tls.sh некорректен" >&2; exit 1; }
if grep -Eq '^C_SOURCES[^=]*=.*third_party/mbedtls' Makefile; then
    echo "Production C_SOURCES не должен компилировать mbedTLS source" >&2
    exit 1
fi
for tls_qemu_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'TLS 1.3 QEMU proof|tls 1.3 qemu proof' \
            "$tls_qemu_doc"; then
        echo "$tls_qemu_doc не документирует TLS 1.3 QEMU proof" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **270. TLS 1.3 QEMU proof' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 270 завершённым" >&2
    exit 1
}

# HTTP parser state descriptor contracts (item 271).
grep -Fq 'test-http-parser-state-host' Makefile || {
    echo "Makefile не содержит test-http-parser-state-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-http-parser-state-host' Makefile || {
    echo "test-host-suite не содержит test-http-parser-state-host" >&2;
    exit 1; }
grep -Fq 'HTTP_PARSER_VERSION 1U' \
    kernel/include/http_parser.h || {
    echo "http_parser.h не содержит HTTP_PARSER_VERSION 1U" >&2; exit 1; }
grep -Fq 'HTTP_PARSER_SLOT_COUNT 4U' \
    kernel/include/http_parser.h || {
    echo "http_parser.h не содержит HTTP_PARSER_SLOT_COUNT 4U" >&2;
    exit 1; }
for parser_state in HTTP_PARSER_STATE_EMPTY \
                    HTTP_PARSER_STATE_REQUEST_LINE \
                    HTTP_PARSER_STATE_HEADERS \
                    HTTP_PARSER_STATE_COMPLETE \
                    HTTP_PARSER_STATE_REJECTED; do
    grep -Fq "$parser_state" kernel/include/http_parser.h || {
        echo "http_parser.h не содержит $parser_state" >&2; exit 1; }
done
grep -Fq 'HTTP_PARSER_REQUEST_LINE_MAX 1024U' \
    kernel/include/http_parser.h || {
    echo "http_parser.h не содержит request-line max 1024" >&2; exit 1; }
grep -Fq 'HTTP_PARSER_HEADERS_MAX 4096U' \
    kernel/include/http_parser.h || {
    echo "http_parser.h не содержит header max 4096" >&2; exit 1; }
grep -Fq 'struct http_parser_token' \
    kernel/include/http_parser.h || {
    echo "http_parser.h не содержит token" >&2; exit 1; }
grep -Fq 'struct http_parser_progress' \
    kernel/include/http_parser.h || {
    echo "http_parser.h не содержит progress descriptor" >&2; exit 1; }
for parser_symbol in http_parser_init \
                     http_parser_start \
                     http_parser_advance \
                     http_parser_reject \
                     http_parser_release; do
    grep -Fq "$parser_symbol" \
        kernel/include/http_parser.h \
        kernel/src/http_parser.c || {
        echo "HTTP parser descriptor не объявляет $parser_symbol" >&2;
        exit 1; }
done
grep -Fq 'HTTP_PARSER_HOST_TEST' \
    kernel/src/http_parser.c || {
    echo "HTTP parser descriptor не помечен HTTP_PARSER_HOST_TEST" >&2;
    exit 1; }
if grep -nE '\b(errno|socket|malloc|calloc|free|kmalloc|kfree|pmm_alloc|serial_printf|printf|fprintf|sleep|delay|hlt|sti|cli|panic)\b' \
        kernel/src/http_parser.c; then
    echo "HTTP parser descriptor использует POSIX, heap, диагностику или блокировку" >&2
    exit 1
fi
for parser_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'HTTP parser state descriptor|http parser state descriptor' \
            "$parser_doc"; then
        echo "$parser_doc не документирует HTTP parser state descriptor" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **271. HTTP parser state descriptor' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 271 завершённым" >&2
    exit 1
}

# Incremental request-line parser contracts (item 272).
grep -Fq 'test-http-request-line-host' Makefile || {
    echo "Makefile не содержит test-http-request-line-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-http-request-line-host' Makefile || {
    echo "test-host-suite не содержит test-http-request-line-host" >&2;
    exit 1; }
grep -Fq 'HTTP_REQUEST_LINE_VERSION 1U' \
    kernel/include/http_parser.h || {
    echo "http_parser.h не содержит HTTP_REQUEST_LINE_VERSION 1U" >&2;
    exit 1; }
for request_line_status in HTTP_PARSER_REQUEST_LINE_INCOMPLETE \
                           HTTP_PARSER_REQUEST_LINE_INVALID_CHARACTER \
                           HTTP_PARSER_REQUEST_LINE_INVALID_SPACING \
                           HTTP_PARSER_REQUEST_LINE_INVALID_TARGET \
                           HTTP_PARSER_REQUEST_LINE_INVALID_VERSION; do
    grep -Fq "$request_line_status" kernel/include/http_parser.h || {
        echo "http_parser.h не содержит $request_line_status" >&2;
        exit 1; }
done
grep -Fq 'struct http_request_line' \
    kernel/include/http_parser.h || {
    echo "http_parser.h не содержит request-line output struct" >&2;
    exit 1; }
grep -Fq 'struct http_request_line_fields' \
    kernel/include/http_parser.h || {
    echo "http_parser.h не содержит request-line field descriptor" >&2;
    exit 1; }
grep -Fq 'request_line_data[HTTP_PARSER_REQUEST_LINE_MAX]' \
    kernel/include/http_parser.h || {
    echo "http_parser.h не содержит fixed request-line buffer" >&2;
    exit 1; }
for request_line_symbol in http_request_line_feed \
                           http_request_line_get \
                           http_request_line_error; do
    grep -Fq "$request_line_symbol" \
        kernel/include/http_parser.h \
        kernel/src/http_parser.c || {
        echo "HTTP request-line parser не объявляет $request_line_symbol" >&2;
        exit 1; }
done
grep -Fq 'HTTP_REQUEST_LINE_HOST_TEST' \
    kernel/src/http_parser.c || {
    echo "HTTP request-line parser не помечен HTTP_REQUEST_LINE_HOST_TEST" >&2;
    exit 1; }
if grep -nE '\b(errno|socket|malloc|calloc|free|kmalloc|kfree|pmm_alloc|serial_printf|printf|fprintf|sleep|delay|hlt|sti|cli|panic)\b' \
        kernel/src/http_parser.c; then
    echo "HTTP request-line parser использует POSIX, heap, диагностику или блокировку" >&2
    exit 1
fi
for request_line_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'incremental request-line parser|incremental request line parser' \
            "$request_line_doc"; then
        echo "$request_line_doc не документирует incremental request-line parser" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **272. Incremental request-line parser' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 272 завершённым" >&2
    exit 1
}

# Incremental header parser contracts (item 273).
grep -Fq 'test-http-header-host' Makefile || {
    echo "Makefile не содержит test-http-header-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-http-header-host' Makefile || {
    echo "test-host-suite не содержит test-http-header-host" >&2;
    exit 1; }
grep -Fq 'HTTP_HEADER_VERSION 1U' \
    kernel/include/http_parser.h || {
    echo "http_parser.h не содержит HTTP_HEADER_VERSION 1U" >&2;
    exit 1; }
for header_status in HTTP_PARSER_HEADER_INCOMPLETE \
                     HTTP_PARSER_HEADER_INVALID_NAME \
                     HTTP_PARSER_HEADER_INVALID_VALUE \
                     HTTP_PARSER_HEADER_FOLDING \
                     HTTP_PARSER_HEADER_INCOMPLETE_TERMINATION \
                     HTTP_PARSER_HEADER_OUT_OF_RANGE; do
    grep -Fq "$header_status" kernel/include/http_parser.h || {
        echo "http_parser.h не содержит $header_status" >&2;
        exit 1; }
done
grep -Fq 'struct http_header_field' \
    kernel/include/http_parser.h || {
    echo "http_parser.h не содержит header field output struct" >&2;
    exit 1; }
grep -Fq 'header_data[HTTP_PARSER_HEADERS_MAX]' \
    kernel/include/http_parser.h || {
    echo "http_parser.h не содержит fixed header buffer" >&2;
    exit 1; }
for header_symbol in http_header_feed \
                     http_header_count \
                     http_header_field \
                     http_header_error; do
    grep -Fq "$header_symbol" \
        kernel/include/http_parser.h \
        kernel/src/http_parser.c || {
        echo "HTTP header parser не объявляет $header_symbol" >&2;
        exit 1; }
done
grep -Fq 'HTTP_HEADER_HOST_TEST' \
    kernel/src/http_parser.c || {
    echo "HTTP header parser не помечен HTTP_HEADER_HOST_TEST" >&2;
    exit 1; }
if grep -nE '\b(errno|socket|malloc|calloc|free|kmalloc|kfree|pmm_alloc|serial_printf|printf|fprintf|sleep|delay|hlt|sti|cli|panic)\b' \
        kernel/src/http_parser.c; then
    echo "HTTP header parser использует POSIX, heap, диагностику или блокировку" >&2
    exit 1
fi
for header_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'incremental header parser|incremental header parser' \
            "$header_doc"; then
        echo "$header_doc не документирует incremental header parser" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **273. Incremental header parser' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 273 завершённым" >&2
    exit 1
}

# HTTP framing and body rejection contracts (item 274).
grep -Fq 'test-http-framing-host' Makefile || {
    echo "Makefile не содержит test-http-framing-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-http-framing-host' Makefile || {
    echo "test-host-suite не содержит test-http-framing-host" >&2;
    exit 1; }
grep -Fq 'HTTP_FRAMING_VERSION 1U' \
    kernel/include/http_parser.h || {
    echo "http_parser.h не содержит HTTP_FRAMING_VERSION 1U" >&2;
    exit 1; }
for framing_kind in HTTP_FRAMING_NONE \
                    HTTP_FRAMING_CONTENT_LENGTH \
                    HTTP_FRAMING_TRANSFER_ENCODING \
                    HTTP_FRAMING_EXPECT \
                    HTTP_FRAMING_UPGRADE \
                    HTTP_FRAMING_DUPLICATE \
                    HTTP_FRAMING_CONFLICT \
                    HTTP_FRAMING_INVALID; do
    grep -Fq "$framing_kind" kernel/include/http_parser.h || {
        echo "http_parser.h не содержит $framing_kind" >&2;
        exit 1; }
done
grep -Fq 'struct http_framing_result' \
    kernel/include/http_parser.h || {
    echo "http_parser.h не содержит framing result struct" >&2;
    exit 1; }
for framing_symbol in http_framing_check \
                      http_framing_kind_string; do
    grep -Fq "$framing_symbol" \
        kernel/include/http_parser.h \
        kernel/src/http_parser.c || {
        echo "HTTP framing gate не объявляет $framing_symbol" >&2;
        exit 1; }
done
grep -Fq 'HTTP_FRAMING_HOST_TEST' \
    kernel/src/http_parser.c || {
    echo "HTTP framing gate не помечен HTTP_FRAMING_HOST_TEST" >&2;
    exit 1; }
if grep -nE '\b(errno|socket|malloc|calloc|free|kmalloc|kfree|pmm_alloc|serial_printf|printf|fprintf|sleep|delay|hlt|sti|cli|panic)\b' \
        kernel/src/http_parser.c; then
    echo "HTTP framing gate использует POSIX, heap, диагностику или блокировку" >&2
    exit 1
fi
for framing_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'framing and body rejection|framing body rejection' \
            "$framing_doc"; then
        echo "$framing_doc не документирует HTTP framing and body rejection" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **274. Framing and body rejection' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 274 завершённым" >&2
    exit 1
}

# HTTP method/version and Host policy contracts (item 275).
grep -Fq 'test-http-policy-host' Makefile || {
    echo "Makefile не содержит test-http-policy-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-http-policy-host' Makefile || {
    echo "test-host-suite не содержит test-http-policy-host" >&2;
    exit 1; }
grep -Fq 'HTTP_METHOD_VERSION_POLICY_VERSION 1U' \
    kernel/include/http_parser.h || {
    echo "http_parser.h не содержит HTTP_METHOD_VERSION_POLICY_VERSION 1U" >&2;
    exit 1; }
for method_kind in HTTP_METHOD_NONE \
                   HTTP_METHOD_GET \
                   HTTP_METHOD_HEAD \
                   HTTP_METHOD_UNSUPPORTED; do
    grep -Fq "$method_kind" kernel/include/http_parser.h || {
        echo "http_parser.h не содержит $method_kind" >&2; exit 1; }
done
for policy_reason in HTTP_POLICY_NONE \
                     HTTP_POLICY_METHOD \
                     HTTP_POLICY_VERSION \
                     HTTP_POLICY_MISSING_HOST \
                     HTTP_POLICY_EMPTY_HOST \
                     HTTP_POLICY_DUPLICATE_HOST \
                     HTTP_POLICY_INVALID_HOST; do
    grep -Fq "$policy_reason" kernel/include/http_parser.h || {
        echo "http_parser.h не содержит $policy_reason" >&2; exit 1; }
done
grep -Fq 'struct http_policy_result' \
    kernel/include/http_parser.h || {
    echo "http_parser.h не содержит policy result struct" >&2; exit 1; }
for policy_symbol in http_policy_check \
                     http_method_kind_string \
                     http_policy_reason_string; do
    grep -Fq "$policy_symbol" \
        kernel/include/http_parser.h \
        kernel/src/http_parser.c || {
        echo "HTTP policy gate не объявляет $policy_symbol" >&2;
        exit 1; }
done
grep -Fq 'HTTP_POLICY_HOST_TEST' \
    kernel/src/http_parser.c || {
    echo "HTTP policy gate не помечен HTTP_POLICY_HOST_TEST" >&2;
    exit 1; }
if grep -nE '\b(errno|socket|malloc|calloc|free|kmalloc|kfree|pmm_alloc|serial_printf|printf|fprintf|sleep|delay|hlt|sti|cli|panic)\b' \
        kernel/src/http_parser.c; then
    echo "HTTP policy gate использует POSIX, heap, диагностику или блокировку" >&2
    exit 1
fi
for policy_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'HTTP method/version and Host policy|method/version and host policy' \
            "$policy_doc"; then
        echo "$policy_doc не документирует HTTP method/version and Host policy" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **275. Method/version policy' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 275 завершённым" >&2
    exit 1
}

# HTTP fixed route table contracts (item 276).
grep -Fq 'test-http-route-host' Makefile || {
    echo "Makefile не содержит test-http-route-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-http-route-host' Makefile || {
    echo "test-host-suite не содержит test-http-route-host" >&2;
    exit 1; }
grep -Fq 'HTTP_ROUTE_TABLE_VERSION 1U' \
    kernel/include/http_parser.h || {
    echo "http_parser.h не содержит HTTP_ROUTE_TABLE_VERSION 1U" >&2;
    exit 1; }
# ITEM-503: the public certificate document is the third fixed route.
grep -Fq 'HTTP_ROUTE_COUNT 3U' \
    kernel/include/http_parser.h || {
    echo "http_parser.h не содержит HTTP_ROUTE_COUNT 3U" >&2;
    exit 1; }
grep -Fq 'HTTP_ROUTE_ROOT_LENGTH 1U' \
    kernel/include/http_parser.h || {
    echo "http_parser.h не содержит HTTP_ROUTE_ROOT_LENGTH 1U" >&2;
    exit 1; }
grep -Fq 'HTTP_ROUTE_HEALTH_LENGTH 7U' \
    kernel/include/http_parser.h || {
    echo "http_parser.h не содержит HTTP_ROUTE_HEALTH_LENGTH 7U" >&2;
    exit 1; }
for route_id in HTTP_ROUTE_NONE \
                HTTP_ROUTE_ROOT \
                HTTP_ROUTE_HEALTH \
                HTTP_ROUTE_CERTIFICATE; do
    grep -Fq "$route_id" kernel/include/http_parser.h || {
        echo "http_parser.h не содержит $route_id" >&2; exit 1; }
done
for route_status in HTTP_ROUTE_OK HTTP_ROUTE_NULL; do
    grep -Fq "$route_status" kernel/include/http_parser.h || {
        echo "http_parser.h не содержит $route_status" >&2; exit 1; }
done
grep -Fq 'struct http_route_descriptor' \
    kernel/include/http_parser.h || {
    echo "http_parser.h не содержит route descriptor struct" >&2;
    exit 1; }
for route_symbol in http_route_table \
                    http_route_match \
                    http_route_match_line \
                    http_route_id_string; do
    grep -Fq "$route_symbol" \
        kernel/include/http_parser.h \
        kernel/src/http_parser.c || {
        echo "HTTP fixed route table не объявляет $route_symbol" >&2;
        exit 1; }
done
grep -Fq 'HTTP_ROUTES_HOST_TEST' \
    kernel/src/http_parser.c || {
    echo "HTTP fixed route table не помечен HTTP_ROUTES_HOST_TEST" >&2;
    exit 1; }
if grep -Fq 'http_route_register' \
        kernel/include/http_parser.h kernel/src/http_parser.c; then
    echo "HTTP fixed route table не должен регистрировать маршруты" >&2
    exit 1
fi
if grep -nE '\b(errno|socket|malloc|calloc|free|kmalloc|kfree|pmm_alloc|serial_printf|printf|fprintf|sleep|delay|hlt|sti|cli|panic)\b' \
        kernel/src/http_parser.c; then
    echo "HTTP fixed route table использует POSIX, heap, диагностику или блокировку" >&2
    exit 1
fi
for route_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'HTTP fixed route table|fixed route table' \
            "$route_doc"; then
        echo "$route_doc не документирует HTTP fixed route table" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **276. Fixed route table' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 276 завершённым" >&2
    exit 1
}

# HTTP deterministic response encoder contracts (item 277).
grep -Fq 'test-http-response-host' Makefile || {
    echo "Makefile не содержит test-http-response-host" >&2; exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-http-response-host' Makefile || {
    echo "test-host-suite не содержит test-http-response-host" >&2;
    exit 1; }
grep -Fq 'HTTP_RESPONSE_VERSION 1U' \
    kernel/include/http_response.h || {
    echo "http_response.h не содержит HTTP_RESPONSE_VERSION 1U" >&2;
    exit 1; }
grep -Fq 'HTTP_RESPONSE_BUFFER_MAX 4512U' \
    kernel/include/http_response.h || {
    echo "http_response.h не содержит HTTP_RESPONSE_BUFFER_MAX 4512U" >&2;
    exit 1; }
grep -Fq 'HTTP_RESPONSE_BODY_MAX 3840U' \
    kernel/include/http_response.h || {
    echo "http_response.h не содержит HTTP_RESPONSE_BODY_MAX 3840U" >&2;
    exit 1; }
# ITEM-509 increment 8: the JSON API documents keep their own smaller bound;
# the wire cap above covers the panel's Help pages and does not grow them.
grep -Fq 'HTTP_RESPONSE_API_BODY_MAX 2048U' \
    kernel/include/http_response.h || {
    echo "http_response.h не содержит HTTP_RESPONSE_API_BODY_MAX 2048U" >&2;
    exit 1; }
for response_status in HTTP_STATUS_200_OK \
                      HTTP_STATUS_303_SEE_OTHER \
                      HTTP_STATUS_400_BAD_REQUEST \
                      HTTP_STATUS_404_NOT_FOUND \
                      HTTP_STATUS_405_METHOD_NOT_ALLOWED \
                      HTTP_STATUS_413_PAYLOAD_TOO_LARGE \
                      HTTP_STATUS_414_URI_TOO_LONG \
                      HTTP_STATUS_431_HEADER_FIELDS_TOO_LARGE; do
    grep -Fq "$response_status" kernel/include/http_response.h || {
        echo "http_response.h не содержит $response_status" >&2;
        exit 1; }
done
for response_content_type in HTTP_CONTENT_TYPE_HTML_UTF8 \
                             HTTP_CONTENT_TYPE_JSON; do
    grep -Fq "$response_content_type" kernel/include/http_response.h || {
        echo "http_response.h не содержит $response_content_type" >&2;
        exit 1; }
done
for response_error in HTTP_RESPONSE_OK \
                      HTTP_RESPONSE_NULL \
                      HTTP_RESPONSE_BAD_STATUS \
                      HTTP_RESPONSE_BAD_CONTENT_TYPE \
                      HTTP_RESPONSE_BODY_TOO_LARGE \
                      HTTP_RESPONSE_BUFFER_TOO_SMALL; do
    grep -Fq "$response_error" kernel/include/http_response.h || {
        echo "http_response.h не содержит $response_error" >&2;
        exit 1; }
done
grep -Fq 'struct http_response' \
    kernel/include/http_response.h || {
    echo "http_response.h не содержит response descriptor struct" >&2;
    exit 1; }
for response_symbol in http_response_length \
                       http_response_head_length \
                       http_response_encode \
                       http_response_encode_head \
                       http_status_code_string \
                       http_content_type_string \
                       http_response_status_string; do
    grep -Fq "$response_symbol" \
        kernel/include/http_response.h \
        kernel/src/http_response.c || {
        echo "HTTP deterministic response encoder не объявляет $response_symbol" >&2;
        exit 1; }
done
grep -Fq 'HTTP_RESPONSE_HOST_TEST' \
    kernel/src/http_response.c || {
    echo "HTTP deterministic response encoder не помечен HTTP_RESPONSE_HOST_TEST" >&2;
    exit 1; }
if grep -Fq 'http_response_register' \
        kernel/include/http_response.h kernel/src/http_response.c; then
    echo "HTTP deterministic response encoder не должен регистрировать ответы" >&2
    exit 1
fi
if grep -nE '\b(errno|socket|malloc|calloc|free|kmalloc|kfree|pmm_alloc|serial_printf|printf|fprintf|snprintf|sprintf|sleep|delay|hlt|sti|cli|panic)\b' \
        kernel/src/http_response.c; then
    echo "HTTP deterministic response encoder использует POSIX, heap, диагностику или блокировку" >&2
    exit 1
fi
for response_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'HTTP deterministic response encoder|deterministic response encoder' \
            "$response_doc"; then
        echo "$response_doc не документирует HTTP deterministic response encoder" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **277. Deterministic response encoder' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 277 завершённым" >&2
    exit 1
}

# HTTP HEAD response semantics contracts (item 278).
grep -Fq 'HTTP_RESPONSE_HEAD_VERSION 1U' \
    kernel/include/http_response.h || {
    echo "http_response.h не содержит HTTP_RESPONSE_HEAD_VERSION 1U" >&2;
    exit 1; }
for head_symbol in http_response_head_length \
                   http_response_encode_head; do
    grep -Fq "$head_symbol" \
        kernel/include/http_response.h \
        kernel/src/http_response.c || {
        echo "HTTP HEAD response semantics не объявляет $head_symbol" >&2;
        exit 1; }
done
for head_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'HTTP HEAD response semantics|HEAD response semantics' \
            "$head_doc"; then
        echo "$head_doc не документирует HTTP HEAD response semantics" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **278. HEAD response semantics' \
    docs/superpowers/plans/2026-08-22-https-vertical-slice-items-201-300.md || {
    echo "https queue plan не помечает item 278 завершённым" >&2
    exit 1
}
