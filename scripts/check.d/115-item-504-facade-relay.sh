#!/usr/bin/env bash
# ITEM-504 increment 8: contracted slice of the HFOC OS check suite.  Sourced in
# pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The facade leads a request to the backend of the name it was addressed by.
# The name is decided once, before any dispatch, and the relay is a byte pipe
# over the already terminated TLS session:
#
#   * management keeps priority: `/panel/...` and `/api/...` are management
#     even when no route claims them, so an unknown management path keeps its
#     own 404 and is never led to a backend;
#   * a declared route with `tls=off` is relayed to exactly that backend - the
#     raw request head and the bounded body the client wrote, then the answer
#     back through TLS; nothing re-resolves the name mid-session;
#   * `tls=on` is refused with 503, never silently downgraded: the frozen
#     profile has no TLS client, and the refusal names the state rather than
#     pretending the hop is protected;
#   * an undeclared name is a 404 and a name the table cannot carry is a 400;
#     a refused request leaves no target behind;
#   * the QEMU proof commits a route against a live backend on the test host
#     and requires that backend's own body back, plus the management 404 and
#     the 503 for `tls=on`.

for item504r_file in \
    kernel/include/appliance_route_table.h \
    kernel/src/appliance_route_table.c \
    kernel/include/appliance_route_config.h \
    kernel/src/appliance_production_serving.c \
    tests/appliance-route-table-host.c \
    scripts/test-qemu-production-service-https.sh \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    test -f "$item504r_file" || {
        echo "ITEM-504R: отсутствует файл: $item504r_file" >&2
        exit 1
    }
done

# ----------------------------------------------------------- the decision -----
grep -Fq 'appliance_route_table_resolve_host(' \
    kernel/include/appliance_route_table.h \
    kernel/src/appliance_route_table.c || {
    echo "ITEM-504R: у решения «Host -> маршрут» нет одного владельца" >&2
    exit 1
}
grep -Fq 'appliance_production_serving_resolve_route(connection)' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504R: путь обслуживания не решает имя запроса" >&2
    exit 1
}
# Management first: the facade must not claim a path the panel or the API owns.
grep -Fq 'appliance_production_serving_target_management(' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504R: управляющие пути не отделены от фасада" >&2
    exit 1
}
grep -Fq '"/panel"' kernel/src/appliance_production_serving.c || {
    echo "ITEM-504R: /panel не признан управляющим путём" >&2
    exit 1
}
grep -Fq '"/api/"' kernel/src/appliance_production_serving.c || {
    echo "ITEM-504R: /api/ не признан управляющим путём" >&2
    exit 1
}

# ------------------------------------------------------------- the refusals ---
grep -Fq 'appliance_production_serving_relay_start(connection)' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504R: объявленный маршрут не поднимает релей" >&2
    exit 1
}
grep -Fq 'connection->route.tls_to_backend' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504R: маршрут с TLS до бэкенда не отличается" >&2
    exit 1
}
grep -Fq 'HTTP_STATUS_503_SERVICE_UNAVAILABLE' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504R: отказ tls=on не назван статусом" >&2
    exit 1
}
grep -Fq 'HTTP_STATUS_404_NOT_FOUND' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504R: неизвестное имя не отвергается как 404" >&2
    exit 1
}
grep -Fq 'APPLIANCE_ROUTE_BAD_NAME' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504R: имя, которое таблица нести не может, не отличается" >&2
    exit 1
}

# ----------------------------------------------------------- the byte pipe ----
for item504r_relay in \
    'appliance_production_serving_relay_step(' \
    'appliance_production_serving_relay_stage_request(' \
    'appliance_production_serving_relay_drain_to_target(' \
    'appliance_production_serving_relay_flush_to_client(' \
    'appliance_production_serving_relay_forget_target(' \
    'appliance_production_serving_relay_recv(' \
    'appliance_production_serving_relay_connected(' \
    'appliance_production_serving_relay_error(' \
    'appliance_production_serving_relay_settle(' \
    'tcp_connect(' \
    'TCP_WRITE_FLAG_COPY' \
    'tcp_shutdown(' \
    'mbedtls_ssl_write('; do
    grep -Fq "$item504r_relay" kernel/src/appliance_production_serving.c || {
        echo "ITEM-504R: релей не содержит: $item504r_relay" >&2
        exit 1
    }
done
# The raw head is captured while the request is parsed, because the parser
# publishes fields, not bytes; the body is forwarded from its own bounded store.
grep -Fq 'connection->request_raw_length' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504R: сырые байты запроса не сохраняются" >&2
    exit 1
}
grep -Fq 'sizeof(connection->request_raw)' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504R: буфер сырого запроса не ограничен" >&2
    exit 1
}
# A released connection owns no target, and the target's callbacks are cleared
# before the abort so the release cannot recurse through the error handler.
item504r_forget_line=$(grep -n 'appliance_production_serving_relay_forget_target(connection);' \
    kernel/src/appliance_production_serving.c | head -1 | cut -d: -f1)
item504r_release_line=$(grep -n '^static void appliance_production_serving_release(' \
    kernel/src/appliance_production_serving.c | head -1 | cut -d: -f1)
if (( item504r_forget_line < item504r_release_line )); then
    echo "ITEM-504R: цель не забывается при освобождении соединения" >&2
    exit 1
fi
# The answer streams instead of trickling: staging hands what it already holds
# to the TLS session before it refuses a segment, and hands the segment it just
# staged over immediately.  Without the first flush a staged segment refuses
# the segment behind it; without the second the answer waits for the next poll
# tick.  Either way the answer would advance one segment per lwIP retransmit
# timer, which a single-segment proof body cannot see.
item504r_recv_start=$(grep -n '^static err_t appliance_production_serving_relay_recv(' \
    kernel/src/appliance_production_serving.c | head -1 | cut -d: -f1)
item504r_recv_end=$(awk -v start="$item504r_recv_start" \
    'NR > start && /^}/ { print NR; exit }' \
    kernel/src/appliance_production_serving.c)
item504r_recv_body=$(sed -n "${item504r_recv_start},${item504r_recv_end}p" \
    kernel/src/appliance_production_serving.c)
item504r_flush_lines=$(printf '%s\n' "$item504r_recv_body" | \
    grep -n 'appliance_production_serving_relay_flush_to_client(connection);' | \
    cut -d: -f1)
item504r_refusal_line=$(printf '%s\n' "$item504r_recv_body" | \
    grep -n 'return ERR_MEM;' | head -1 | cut -d: -f1)
item504r_recved_line=$(printf '%s\n' "$item504r_recv_body" | \
    grep -n 'tcp_recved(pcb, total);' | head -1 | cut -d: -f1)
item504r_flush_before=0
item504r_flush_after=0
if [[ -n "$item504r_flush_lines" && -n "$item504r_refusal_line" &&
      -n "$item504r_recved_line" ]]; then
    item504r_flush_before=$(printf '%s\n' "$item504r_flush_lines" | \
        awk -v limit="$item504r_refusal_line" '$1 < limit' | wc -l)
    item504r_flush_after=$(printf '%s\n' "$item504r_flush_lines" | \
        awk -v start="$item504r_recved_line" '$1 > start' | wc -l)
fi
if (( item504r_flush_before == 0 || item504r_flush_after == 0 )); then
    echo "ITEM-504R: ответ не отдаётся в TLS до отказа и сразу после приёма" >&2
    exit 1
fi
# A delivered pbuf is not bounded by our own TCP_MSS: the proof backend's
# answer arrives as 1440-byte pbufs into a stack that advertises 536, and a
# staging buffer smaller than that refuses the first segment forever (measured
# before this bound existed).  The client buffer therefore holds a whole
# receive window plus one segment, and it is the buffer the relay copies into.
grep -Fq '#define APPLIANCE_PRODUCTION_SERVING_RELAY_CLIENT_BUFFER_MAX (TCP_WND + TCP_MSS)' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504R: буфер ответа не покрывает доставленный pbuf" >&2
    exit 1
}
grep -Fq 'to_client[APPLIANCE_PRODUCTION_SERVING_RELAY_CLIENT_BUFFER_MAX]' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504R: буфер ответа не связан с окном приёма" >&2
    exit 1
}

# -------------------------------------------------------------- the records ---
grep -Fq 'name=b.example backend=10.0.2.2:' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504R: QEMU-проба не коммитит маршрут до живого бэкенда" >&2
    exit 1
}
grep -Fq 'ROUTING_RECORD_RELAY=' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504R: запись о применённом маршруте релея не проверяется" >&2
    exit 1
}
# ITEM-504 increment 9: the second declared name has its own applied record
# and its own live backend, so the name - not the first table entry - selects
# the backend.
grep -Fq 'ROUTING_RECORD_RELAY2=' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504R: запись о втором применённом маршруте не проверяется" >&2
    exit 1
}
grep -Fq 'name=c.example backend=10.0.2.2:' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504R: второй маршрут не назван своим бэкендом" >&2
    exit 1
}
# ITEM-504 increment 10: a commit that must fail is refused by name, its
# candidate is dropped by an explicit cancel, and the record says CANCELLED -
# the applied table the restore record carries is never touched.
grep -Fq 'ROUTING_RECORD_CANCELLED=' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504R: запись об отменённом кандидате не проверяется" >&2
    exit 1
}
grep -Fq 'result=CANCELLED' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504R: отмена не названа результатом" >&2
    exit 1
}
grep -Fq 'cancel=1&token=' scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504R: кандидат снимается не явной отменой" >&2
    exit 1
}

# -------------------------------------------------------------- the QEMU proof -
grep -Fq 'facade-backend.py' scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504R: у пробы нет живого бэкенда" >&2
    exit 1
}
grep -Fq 'FACADE_BACKEND2_PORT' scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504R: у пробы нет второго живого бэкенда" >&2
    exit 1
}
grep -Fq 'serving-facade-client.py' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504R: у пробы нет клиента фасада" >&2
    exit 1
}
grep -Fq 'SERVING_REBOOT_PORT' scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504R: запросы фасада не идут на свежий пул соединений" >&2
    exit 1
}
for item504r_qemu in \
    'FACADE-PROOF-BACKEND:' \
    'the facade relay did not carry the backend body' \
    'the second name did not reach its own backend' \
    'both names reached the same backend' \
    'a cancelled route is listed as applied' \
    'BAD_CONFIRMATION' \
    'the failed commit is refused by name' \
    'the failed commit changed no applied route' \
    'an undeclared name is refused, not guessed' \
    'tls=on is refused, never silently downgraded' \
    'the tls=on route reached a backend in the clear'; do
    grep -Fq "$item504r_qemu" \
        scripts/test-qemu-production-service-https.sh || {
        echo "ITEM-504R: QEMU-проба не утверждает: $item504r_qemu" >&2
        exit 1
    }
done
grep -Fq 'source=COMMITTED routes=3; name=a.example' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504R: перезагрузка не несёт все применённые записи" >&2
    exit 1
}
# The proof body is larger than one TCP segment and both sides build the same
# 4096 bytes: the backend answers with it and the client requires it whole, so
# a relay that loses, truncates or stalls the multi-segment answer fails the
# phase instead of passing on a tiny marker.  Each declared name is compared
# with the body of *its own* backend.
item504r_body_sides=$(grep -Fc 'range(4096 - len(marker) - 1)' \
    scripts/test-qemu-production-service-https.sh)
if (( item504r_body_sides != 2 )); then
    echo "ITEM-504R: бэкенд и клиент не строят одно многосегментное тело" >&2
    exit 1
fi
grep -Fq 'body != first_expected' scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504R: клиент не требует ответ первого имени целиком" >&2
    exit 1
}
grep -Fq 'body != second_expected' scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504R: клиент не требует ответ второго имени целиком" >&2
    exit 1
}

# --------------------------------------------------------------- documents ----
grep -Fq 'ITEM-504' docs/ARCHITECTURE.md || {
    echo "ITEM-504R: ARCHITECTURE не описывает пункт" >&2
    exit 1
}
grep -Fq 'релей' docs/ARCHITECTURE.md || {
    echo "ITEM-504R: ARCHITECTURE не описывает релей фасада" >&2
    exit 1
}
grep -Fq 'ITEM-504' docs/ROADMAP.md || {
    echo "ITEM-504R: ROADMAP не называет пункт" >&2
    exit 1
}
