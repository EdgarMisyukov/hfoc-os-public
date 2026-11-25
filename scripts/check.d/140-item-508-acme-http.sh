#!/usr/bin/env bash
# ITEM-508 increment 10: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The ACME client's HTTP exchange: the request it builds and the reply it reads.
#
# The order module produces one request at a time and consumes one reply at a
# time; between them sits an HTTP exchange that must not guess.  This slice pins
# it:
#
#   * only an absolute `https://` URL is accepted, split into host, port and
#     target; the port defaults to 443 and an explicit one is kept for the
#     offline stub, a literal address is marked so the transport can skip the
#     resolver, and userinfo, an IPv6 literal, a fragment and a bad port are
#     refusals by name;
#   * the request carries exactly the method, the host, the content type and
#     the length the item needs, plus `Connection: close`; GET and HEAD carry
#     no body, an ACME POST is not empty, methods are case-sensitive, and a
#     request that does not fit the caller's buffer is refused whole;
#   * the reply is read only when it arrived whole: the status line must be
#     HTTP/1.x with three digits, the headers the item acts on are captured
#     (and a duplicate refused), an obs-fold and a control byte are refusals,
#     `Transfer-Encoding` is refused by name rather than parsed as payload,
#     and a content length that does not match the body, or that crosses the
#     body bound while it is read, is a refusal;
#   * the published record names the status and the size and never echoes the
#     body or a nonce.
for item508_inc10_file in \
    kernel/include/appliance_acme_http.h \
    kernel/src/appliance_acme_http.c \
    tests/appliance-acme-http-host.c \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item508_inc10_file ]] || {
        echo "ITEM-508 inc10: отсутствует $item508_inc10_file" >&2
        exit 1
    }
done

# The published bounds, statuses and entry points.
for item508_inc10_name in \
    'APPLIANCE_ACME_HTTP_PORT_DEFAULT 443U' \
    'APPLIANCE_ACME_HTTP_REQUEST_MAX' \
    'APPLIANCE_ACME_HTTP_RESPONSE_MAX' \
    'APPLIANCE_ACME_HTTP_TRANSFER_ENCODING_UNSUPPORTED' \
    'APPLIANCE_ACME_HTTP_DUPLICATE_HEADER' \
    'appliance_acme_http_parse_url' \
    'appliance_acme_http_build_request' \
    'appliance_acme_http_read_response' \
    'appliance_acme_http_status_is_success'; do
    grep -Fq -- "$item508_inc10_name" kernel/include/appliance_acme_http.h || {
        echo "ITEM-508 inc10: заголовок не публикует $item508_inc10_name" >&2
        exit 1
    }
done

# Only https, and only the three methods the item sends - exactly spelled.
grep -Fq '"https://"' kernel/src/appliance_acme_http.c || {
    echo "ITEM-508 inc10: модуль не требует https" >&2
    exit 1
}
grep -Fq 'appliance_acme_http_method_is' kernel/src/appliance_acme_http.c || {
    echo "ITEM-508 inc10: метод не сравнивается точно" >&2
    exit 1
}
for item508_inc10_method in '"POST"' '"GET"' '"HEAD"'; do
    grep -Fq -- "$item508_inc10_method" kernel/src/appliance_acme_http.c || {
        echo "ITEM-508 inc10: модуль не знает метод $item508_inc10_method" >&2
        exit 1
    }
done
# The request closes the connection and names its framing.
grep -Fq 'Connection: close' kernel/src/appliance_acme_http.c || {
    echo "ITEM-508 inc10: запрос не закрывает соединение" >&2
    exit 1
}
grep -Fq 'Content-Length: ' kernel/src/appliance_acme_http.c || {
    echo "ITEM-508 inc10: запрос не называет длину тела" >&2
    exit 1
}
# The framing this module refuses instead of parsing.
grep -Fq '"transfer-encoding"' kernel/src/appliance_acme_http.c || {
    echo "ITEM-508 inc10: модуль не распознаёт transfer-encoding" >&2
    exit 1
}
# The published record names the status and the size, and nothing else.
grep -Fq '"[ACME-HTTP] response status="' kernel/src/appliance_acme_http.c || {
    echo "ITEM-508 inc10: ответ не публикует запись" >&2
    exit 1
}
grep -Fq 'appliance_acme_http_text_append(&text, " bytes=")' \
    kernel/src/appliance_acme_http.c || {
    echo "ITEM-508 inc10: запись не называет размер тела" >&2
    exit 1
}

# The proof: the exchange is driven on fixed bytes in both host aggregates.
grep -Fq 'test-appliance-acme-http-host' Makefile || {
    echo "ITEM-508 inc10: проба не подключена к Makefile" >&2
    exit 1
}
item508_inc10_make_targets=$(awk \
    '/[$][(]MAKE[)] test-appliance-acme-http-host/ { count += 1 } \
     END { print count + 0 }' Makefile)
[[ $item508_inc10_make_targets -eq 2 ]] || {
    echo "ITEM-508 inc10: проба не подключена к обоим хост-наборам" >&2
    exit 1
}
for item508_inc10_case in \
    'a plaintext URL is refused by name' \
    'a dotted quad is marked as a literal and keeps its port' \
    'the POST is exactly the bytes the item sends' \
    'methods are case-sensitive: post is not POST' \
    'a refused request leaves nothing behind' \
    'the replay nonce is captured' \
    'a chunked framing is refused by name, not parsed as a body' \
    'a header the item acts on is not answered twice' \
    'a body longer than promised lies about its length' \
    'a ten-digit content length is refused by the bound, not wrapped' \
    'the record carries neither body nor nonce'; do
    grep -Fq -- "$item508_inc10_case" tests/appliance-acme-http-host.c || {
        echo "ITEM-508 inc10: проба не проверяет «$item508_inc10_case»" >&2
        exit 1
    }
done
