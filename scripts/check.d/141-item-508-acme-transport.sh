#!/usr/bin/env bash
# ITEM-508 increment 11: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The ACME client's exchange driver: one HTTP exchange over an injected link.
#
# The order module produces one request at a time and consumes one reply at a
# time; `appliance_acme_http` owns the bytes and this module owns the exchange
# between them.  This slice pins it:
#
#   * nothing starts on an unset clock, and a clock that runs backwards is a
#     refusal: `begin` refuses `now_ms == 0` before the link is touched;
#   * the connection and the TLS handshake share one bounded window and the
#     reply has its own; a window that closed is `TIMEOUT` with the record;
#   * the request is built by the HTTP module and leaves whole, never before
#     the link reports the handshake complete, and a short send is continued;
#   * the reply is parsed only when the module has it whole: a body framed by
#     `Content-Length` completes on its bytes, a body framed by the connection
#     waits for the close, and a truncated or malformed reply is a refusal by
#     name with the HTTP module's own status kept beside it;
#   * one bounded record names the outcome, the status and the two sizes and
#     carries no host, no body, no nonce and no key.
for item508_inc11_file in \
    kernel/include/appliance_acme_transport.h \
    kernel/src/appliance_acme_transport.c \
    tests/appliance-acme-transport-host.c \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item508_inc11_file ]] || {
        echo "ITEM-508 inc11: отсутствует $item508_inc11_file" >&2
        exit 1
    }
done

# The published states, statuses, bounds and entry points.
for item508_inc11_name in \
    'APPLIANCE_ACME_TRANSPORT_CONNECT_DEADLINE_MS' \
    'APPLIANCE_ACME_TRANSPORT_REPLY_DEADLINE_MS' \
    'APPLIANCE_ACME_TRANSPORT_RECORD_MAX' \
    'APPLIANCE_ACME_TRANSPORT_STATE_CONNECTING' \
    'APPLIANCE_ACME_TRANSPORT_TIMEOUT' \
    'APPLIANCE_ACME_TRANSPORT_BAD_REPLY' \
    'APPLIANCE_ACME_TRANSPORT_BAD_CLOCK' \
    'appliance_acme_transport_init' \
    'appliance_acme_transport_begin' \
    'appliance_acme_transport_advance' \
    'appliance_acme_transport_take_response' \
    'appliance_acme_transport_close'; do
    grep -Fq -- "$item508_inc11_name" kernel/include/appliance_acme_transport.h || {
        echo "ITEM-508 inc11: заголовок не публикует $item508_inc11_name" >&2
        exit 1
    }
done

# The bytes belong to the HTTP module; the driver decides only when and how
# much.  Both directions go through the increment-10 entry points.
for item508_inc11_http in \
    'appliance_acme_http_parse_url' \
    'appliance_acme_http_build_request' \
    'appliance_acme_http_read_response' \
    'has_content_length'; do
    grep -Fq -- "$item508_inc11_http" kernel/src/appliance_acme_transport.c || {
        echo "ITEM-508 inc11: драйвер не зовёт $item508_inc11_http" >&2
        exit 1
    }
done
# The pure driver knows neither lwIP nor mbedTLS: the binding is the next
# increment and the client symbol stays forbidden here.
if grep -Eq 'mbedtls_|tcp_new|tcp_connect|pbuf_' \
        kernel/src/appliance_acme_transport.c; then
    echo "ITEM-508 inc11: чистый драйвер знает транспорт" >&2
    exit 1
fi
# The one record.
grep -Fq '"[ACME-TRANSPORT] outcome="' \
    kernel/src/appliance_acme_transport.c || {
    echo "ITEM-508 inc11: драйвер не публикует запись" >&2
    exit 1
}
grep -Fq 'appliance_acme_transport_text_append(&text, " reply=")' \
    kernel/src/appliance_acme_transport.c || {
    echo "ITEM-508 inc11: запись не называет размер ответа" >&2
    exit 1
}

# The proof: the exchange is driven on a scripted link in both host aggregates.
grep -Fq 'test-appliance-acme-transport-host' Makefile || {
    echo "ITEM-508 inc11: проба не подключена к Makefile" >&2
    exit 1
}
item508_inc11_make_targets=$(awk \
    '/[$][(]MAKE[)] test-appliance-acme-transport-host/ { count += 1 } \
     END { print count + 0 }' Makefile)
[[ $item508_inc11_make_targets -eq 2 ]] || {
    echo "ITEM-508 inc11: проба не подключена к обоим хост-наборам" >&2
    exit 1
}
for item508_inc11_case in \
    'an unset clock is refused before the link' \
    'no request byte leaves before the handshake completes' \
    'each turn accepts the bounded number of bytes the link takes' \
    'the bytes the link received are exactly the HTTP request' \
    'a reply that arrives in fragments is read whole' \
    'a reply framed by the connection waits while it is open' \
    'a reply shorter than it promised is truncated, not handed over' \
    'a declared body over the bound is refused by its HTTP name' \
    'the record names the outcome and carries neither nonce, host nor body' \
    'closing twice releases the link once'; do
    grep -Fq -- "$item508_inc11_case" tests/appliance-acme-transport-host.c || {
        echo "ITEM-508 inc11: проба не проверяет «$item508_inc11_case»" >&2
        exit 1
    }
done

# The documents name the increment and its bound.
for item508_inc11_doc in docs/ARCHITECTURE.md docs/ROADMAP.md; do
    grep -Fq 'ITEM-508 increment 11' "$item508_inc11_doc" || {
        echo "ITEM-508 inc11: $item508_inc11_doc не называет инкремент" >&2
        exit 1
    }
done
