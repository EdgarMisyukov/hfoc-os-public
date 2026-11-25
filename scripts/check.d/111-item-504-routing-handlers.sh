#!/usr/bin/env bash
# ITEM-504 part C: contracted slice of the HFOC OS check suite.  Sourced in
# pinned order by scripts/check.sh; lint it on its own with the linter.

# The route-table mutation is reachable over HTTP through the same two-phase
# plane every other mutation uses:
#
#   * `/panel/routing` and `/api/v1/system/routes` are the two POST targets
#     this increment adds, declared once in `http_parser.h` with their exact
#     bytes and lengths and recognised by `http_policy_form_target`;
#   * the serving path binds the routing executor next to the certificate
#     executor and dispatches both targets through one handler that is reached
#     before the panel handler;
#   * the wire `Content-Type` is checked before the body reaches the executor,
#     and a panel cookie is the same bearer principal as an `Authorization`
#     header;
#   * the panel form is answered `303 See Other` back to `/panel/routing`, the
#     API target with the one-line state record and the confirmation token;
#   * an applied mutation is counted, and a refusal names the route
#     configuration's own status instead of a generic failure.

for item504c_file in \
    kernel/include/http_parser.h \
    kernel/src/http_parser.c \
    kernel/src/appliance_production_serving.c \
    tests/http-acceptance-host.c; do
    test -f "$item504c_file" || {
        echo "ITEM-504C: отсутствует файл: $item504c_file" >&2
        exit 1
    }
done

# ------------------------------------------------- the two POST targets ------
grep -Fq '#define HTTP_POLICY_ROUTING_TARGET "/panel/routing"' \
    kernel/include/http_parser.h || {
    echo "ITEM-504C: цель формы маршрутов не объявлена" >&2
    exit 1
}
grep -Fq '#define HTTP_POLICY_ROUTING_TARGET_LENGTH 14U' \
    kernel/include/http_parser.h || {
    echo "ITEM-504C: длина цели формы маршрутов не объявлена" >&2
    exit 1
}
grep -Fq '#define HTTP_POLICY_ROUTES_TARGET "/api/v1/system/routes"' \
    kernel/include/http_parser.h || {
    echo "ITEM-504C: цель API маршрутов не объявлена" >&2
    exit 1
}
grep -Fq '#define HTTP_POLICY_ROUTES_TARGET_LENGTH 21U' \
    kernel/include/http_parser.h || {
    echo "ITEM-504C: длина цели API маршрутов не объявлена" >&2
    exit 1
}
grep -Fq 'HTTP_POLICY_ROUTING_TARGET_LENGTH' kernel/src/http_parser.c || {
    echo "ITEM-504C: разборщик не признаёт цель формы маршрутов" >&2
    exit 1
}
grep -Fq 'HTTP_POLICY_ROUTES_TARGET_LENGTH' kernel/src/http_parser.c || {
    echo "ITEM-504C: разборщик не признаёт цель API маршрутов" >&2
    exit 1
}

# ------------------------------------------- the serving path and the plane --
grep -Fq 'appliance_production_mutation_plane_bind_routing' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504C: исполнитель маршрутов не привязан к плоскости" >&2
    exit 1
}
grep -Fq 'appliance_production_serving_handle_routing' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504C: обработчик целей маршрутов не вызывается" >&2
    exit 1
}
grep -Fq 'if (appliance_production_serving_handle_routing(' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504C: обработчик маршрутов не стоит в диспетчере" >&2
    exit 1
}
grep -Fq 'APPLIANCE_ROUTE_CONFIG_CONTENT_TYPE' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504C: Content-Type тела маршрутов не проверяется" >&2
    exit 1
}
grep -Fq 'appliance_panel_login_cookie_token' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504C: cookie панели не становится тем же principal" >&2
    exit 1
}
grep -Fq 'HTTP_STATUS_303_SEE_OTHER' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504C: форма панели не отвечает 303" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ROUTING_PATH' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504C: 303 не возвращает на страницу маршрутов" >&2
    exit 1
}
grep -Fq 'appliance_route_config_status_string' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504C: отказ не называет статус конфигурации маршрутов" >&2
    exit 1
}

# --------------------------------------------------- the grown corpus -------
grep -Fq '"POST /panel/routing HTTP/1.1\r\n"' \
    tests/http-acceptance-host.c || {
    echo "ITEM-504C: корпус POST-целей не называет форму маршрутов" >&2
    exit 1
}
grep -Fq '"POST /api/v1/system/routes HTTP/1.1\r\n"' \
    tests/http-acceptance-host.c || {
    echo "ITEM-504C: корпус POST-целей не называет API маршрутов" >&2
    exit 1
}
grep -Fq 'ITEM-504 adds the routing form' tests/http-acceptance-host.c || {
    echo "ITEM-504C: корпус POST-целей не называет пункт" >&2
    exit 1
}

# ------------------------------------------------------------- QEMU proof -----
grep -Fq 'serving-routing-client.py' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504C: QEMU-проба не запускает клиент маршрутов" >&2
    exit 1
}
grep -Fq 'the stage names its state and its token' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504C: QEMU-проба не утверждает стадию маршрута" >&2
    exit 1
}
grep -Fq 'the commit applies the staged route' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504C: QEMU-проба не утверждает применение маршрута" >&2
    exit 1
}
grep -Fq '303 back to the page' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504C: QEMU-проба не утверждает 303 формы панели" >&2
    exit 1
}
# The fixture's lwIP pool is spent by the anonymous and authenticated boots
# (eleven connections, then thirteen) and refuses the next one, so the proof
# runs on a boot of its own instead of competing for a pool with no room.
grep -Fq 'refuses the next connection' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504C: QEMU-проба не объясняет бюджет соединений" >&2
    exit 1
}

# -------------------------------------------------------------- documents ----
grep -Fq 'ITEM-504' docs/ROADMAP.md || {
    echo "ITEM-504C: ROADMAP не называет пункт" >&2
    exit 1
}
grep -Fq 'appliance_production_serving_handle_routing' docs/ARCHITECTURE.md || {
    echo "ITEM-504C: ARCHITECTURE не описывает HTTP-путь маршрутов" >&2
    exit 1
}
