#!/usr/bin/env bash
# ITEM-508 increment 28: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The HTTP-01 listener.
#
# ITEM-508 point 8 allows exactly one inbound surface for the challenge: the
# well-known path, for the armed token, GET/HEAD, one answer, no redirect, no
# second path.  Increment 7 owns that decision; this increment owns the socket,
# and this slice pins that the socket adds nothing of its own: every request
# line goes through the decision module, every byte of the answer comes from
# the bounded response builder, the listener is published through the same
# `hfoc_lwip_tcp` wrapper (and its port-80 policy) the management listener
# uses, and the ordinary boot comes up unarmed.
for item508_inc28_file in \
    kernel/include/appliance_production_acme_http01.h \
    kernel/src/appliance_production_acme_http01.c \
    tests/appliance-acme-http01-host.c \
    kernel/src/kernel.c \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item508_inc28_file ]] || {
        echo "ITEM-508 inc28: отсутствует $item508_inc28_file" >&2
        exit 1
    }
done

# 1. The module owns exactly the declared port, the two-slot pool bound and the
#    bounded buffers; the port is never configurable at runtime.
for item508_inc28_header_pin in \
    '#define APPLIANCE_PRODUCTION_ACME_HTTP01_PORT 80U' \
    '#define APPLIANCE_PRODUCTION_ACME_HTTP01_CONNECTION_COUNT 2U' \
    '#define APPLIANCE_PRODUCTION_ACME_HTTP01_LINE_MAX 640U' \
    '#define APPLIANCE_PRODUCTION_ACME_HTTP01_RESPONSE_MAX 768U' \
    '#define APPLIANCE_PRODUCTION_ACME_HTTP01_IDLE_TIMEOUT_MS 10000U' \
    '#define APPLIANCE_PRODUCTION_ACME_HTTP01_RECORD_MAX 192U' \
    'appliance_production_acme_http01_build_response(' \
    'appliance_production_acme_http01_arm(' \
    'appliance_production_acme_http01_disarm('; do
    grep -Fq -- "$item508_inc28_header_pin" \
        kernel/include/appliance_production_acme_http01.h || {
        echo "ITEM-508 inc28: заголовок не содержит «$item508_inc28_header_pin»" \
            >&2
        exit 1
    }
done

# 2. The decision is not duplicated: the listener asks the increment-7 module
#    and builds its answer only through the bounded response builder.
for item508_inc28_source_pin in \
    'appliance_acme_challenge_read_request_line(' \
    'appliance_acme_challenge_decide_line(' \
    'appliance_acme_challenge_arm_token(' \
    'appliance_acme_challenge_disarm(' \
    'appliance_production_acme_http01_build_response(' \
    '"HTTP/1.1 200 OK\r\n"' \
    '"HTTP/1.1 400 Bad Request\r\n"' \
    '"HTTP/1.1 404 Not Found\r\n"' \
    '"HTTP/1.1 405 Method Not Allowed\r\n"' \
    '"Content-Type: text/plain\r\nConnection: close\r\n"' \
    'tcp_sndbuf(' \
    'tcp_write(' \
    'tcp_output(' \
    'tcp_close(' \
    'tcp_abort(' \
    'tcp_recved(' \
    'tcp_recv(' \
    'tcp_err(' \
    'tcp_poll('; do
    grep -Fq -- "$item508_inc28_source_pin" \
        kernel/src/appliance_production_acme_http01.c || {
        echo "ITEM-508 inc28: модуль не содержит «$item508_inc28_source_pin»" \
            >&2
        exit 1
    }
done

# 3. No listener path bypasses the port-policy wrapper: raw lwIP listener
#    creation would open a port the port policy never authorized.  The regex
#    requires the raw call to stand on its own, so `hfoc_lwip_tcp_bind(` and
#    `hfoc_lwip_tcp_listen(` do not count as bypasses.
for item508_inc28_forbidden in tcp_new tcp_bind tcp_listen tcp_accept; do
    if grep -Eq "(^|[^_[:alnum:]])${item508_inc28_forbidden}\\(" \
        kernel/src/appliance_production_acme_http01.c; then
        echo "ITEM-508 inc28: модуль обходит порт-политику через «${item508_inc28_forbidden}(»" \
            >&2
        exit 1
    fi
done
for item508_inc28_wrapper_pin in \
    'hfoc_lwip_tcp_init(' \
    'hfoc_lwip_tcp_bind(' \
    'hfoc_lwip_tcp_listen('; do
    grep -Fq -- "$item508_inc28_wrapper_pin" \
        kernel/src/appliance_production_acme_http01.c || {
        echo "ITEM-508 inc28: модуль не использует «$item508_inc28_wrapper_pin»" \
            >&2
        exit 1
    }
done

# 4. The ordinary boot starts the listener unarmed and publishes one bounded
#    record per state; nothing on this path arms a token.
for item508_inc28_kernel_pin in \
    '#include "appliance_production_acme_http01.h"' \
    'static struct appliance_production_acme_http01 production_acme_http01;' \
    'appliance_production_acme_http01_init(' \
    'appliance_production_acme_http01_poll(' \
    'log_production_acme_http01();'; do
    grep -Fq -- "$item508_inc28_kernel_pin" kernel/src/kernel.c || {
        echo "ITEM-508 inc28: kernel.c не содержит «$item508_inc28_kernel_pin»" \
            >&2
        exit 1
    }
done
if grep -Fq 'appliance_production_acme_http01_arm(' kernel/src/kernel.c; then
    echo "ITEM-508 inc28: обычная загрузка вооружает токен челленджа" >&2
    exit 1
fi

# 5. The host proof pins the bytes and the unarmed production state.
for item508_inc28_test_pin in \
    'static void test_response_bytes(void)' \
    'static void test_init_and_lifecycle(void)' \
    'GET 200 carries exactly the key authorization' \
    'HEAD 200 names the body length and carries no body' \
    'an unarmed challenge is an empty 404' \
    'a POST is an empty 405' \
    'a malformed request line is an empty 400' \
    'a response that does not fit is refused, not truncated' \
    'the production listener comes up unarmed' \
    'the unarmed production record names state, arming and port' \
    'the order path can arm exactly one token' \
    'disarm closes the challenge'; do
    grep -Fq -- "$item508_inc28_test_pin" tests/appliance-acme-http01-host.c || {
        echo "ITEM-508 inc28: проба не содержит «$item508_inc28_test_pin»" >&2
        exit 1
    }
done

# 6. The increment is registered in both host-proof sets and the documents
#    name it.
grep -Fq "\$(MAKE) test-appliance-acme-http01-host" Makefile || {
    echo "ITEM-508 inc28: проба не подключена к хост-наборам" >&2
    exit 1
}
for item508_inc28_doc in docs/ROADMAP.md docs/ARCHITECTURE.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    grep -Fq 'ITEM-508 increment 28' "$item508_inc28_doc" || {
        echo "ITEM-508 inc28: $item508_inc28_doc не называет инкремент" >&2
        exit 1
    }
done
