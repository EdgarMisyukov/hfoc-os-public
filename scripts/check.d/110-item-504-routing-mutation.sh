#!/usr/bin/env bash
# ITEM-504 part B: contracted slice of the HFOC OS check suite.  Sourced in
# pinned order by scripts/check.sh; lint it on its own with the linter.

# The route table reaches the machine through the existing two-phase mutation
# plane.  This increment wires the composition, not the HTTP target yet:
#
#   * `ROUTING_CONFIG` is a new mutation kind (command id 13) reachable at
#     `/api/v1/system/routes` and `/panel/routing`, POST only, admin +
#     configuration-write, exactly like every other mutation;
#   * `appliance_route_config` owns the candidate pair: a stage parses one
#     `application/x-www-form-urlencoded` body (`action=add|remove`, `name`,
#     `backend`, `port`, `tls=on|off`), applies exactly one validated add or
#     remove to a copy of the applied table, and holds the copy;
#   * the commit replaces the applied table whole; a refusal, a cancel or an
#     expired window drops the candidate and leaves the applied table exactly
#     as it was, and a refusal keeps the model's own name
#     (`DUPLICATE_NAME`, `TABLE_FULL`, `BAD_BACKEND`, `NOT_FOUND`, ...);
#   * the plane delegates the bytes to a bound routing executor and refuses an
#     unbound one as `UNSUPPORTED_TARGET`, never as applied;
#   * the body grammar is fail-closed: an empty body, a body over the framing
#     bound, a duplicate field, an unknown field, a missing `=` and an unknown
#     action or flag are all refused, and values are not percent-decoded.

for item504b_file in \
    kernel/include/appliance_route_config.h \
    kernel/src/appliance_route_config.c \
    tests/appliance-route-config-host.c; do
    test -f "$item504b_file" || {
        echo "ITEM-504B: отсутствует файл: $item504b_file" >&2
        exit 1
    }
done

# ------------------------------------------------------ the candidate pair ---
for item504b_api in \
    APPLIANCE_ROUTE_CONFIG_VERSION \
    APPLIANCE_ROUTE_CONFIG_CONTENT_TYPE \
    APPLIANCE_ROUTE_CONFIG_BODY_MAX \
    appliance_route_config_init \
    appliance_route_config_stage \
    appliance_route_config_commit \
    appliance_route_config_rollback \
    appliance_route_config_applied \
    appliance_route_config_staged \
    appliance_route_config_last_status \
    appliance_route_config_last_route_status \
    appliance_route_config_stage_count \
    appliance_route_config_commit_count \
    appliance_route_config_rollback_count \
    appliance_route_config_refused_count \
    appliance_route_config_status_string; do
    grep -Fq "$item504b_api" kernel/include/appliance_route_config.h || {
        echo "ITEM-504B: композиция маршрутов не публикует: $item504b_api" >&2
        exit 1
    }
done
for item504b_refusal in \
    APPLIANCE_ROUTE_CONFIG_BAD_CONTENT_TYPE \
    APPLIANCE_ROUTE_CONFIG_MALFORMED \
    APPLIANCE_ROUTE_CONFIG_MISSING_FIELD \
    APPLIANCE_ROUTE_CONFIG_BAD_ACTION \
    APPLIANCE_ROUTE_CONFIG_BAD_FLAG \
    APPLIANCE_ROUTE_CONFIG_NO_CANDIDATE \
    APPLIANCE_ROUTE_CONFIG_REFUSED; do
    grep -Fq "$item504b_refusal" kernel/include/appliance_route_config.h || {
        echo "ITEM-504B: отказ не объявлен: $item504b_refusal" >&2
        exit 1
    }
    grep -Fq "$item504b_refusal" kernel/src/appliance_route_config.c || {
        echo "ITEM-504B: отказ не назван в композиции: $item504b_refusal" >&2
        exit 1
    }
done
grep -Fq 'appliance_route_table_add(' kernel/src/appliance_route_config.c || {
    echo "ITEM-504B: кандидат не проходит грамматику модели" >&2
    exit 1
}
grep -Fq 'appliance_route_table_remove(' kernel/src/appliance_route_config.c || {
    echo "ITEM-504B: удаление не проходит грамматику модели" >&2
    exit 1
}
grep -Fq 'config->candidate = config->applied;' \
    kernel/src/appliance_route_config.c || {
    echo "ITEM-504B: кандидат не начинается с применённой таблицы" >&2
    exit 1
}
grep -Fq 'config->applied = config->candidate;' \
    kernel/src/appliance_route_config.c || {
    echo "ITEM-504B: коммит не заменяет применённую таблицу целиком" >&2
    exit 1
}
grep -Fq 'APPLIANCE_ROUTE_SOURCE_PANEL' kernel/src/appliance_route_config.c || {
    echo "ITEM-504B: маршрут панели не помечен источником PANEL" >&2
    exit 1
}

# ------------------------------------------------- the mutation plane -------
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ROUTING_CONFIG = 7,' \
    kernel/include/appliance_management_authorization.h || {
    echo "ITEM-504B: вид мутации маршрутов не объявлен" >&2
    exit 1
}
grep -Fq '#define APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_ROUTING_MUTATION 13U' \
    kernel/include/appliance_management_authorization.h || {
    echo "ITEM-504B: команда маршрутов не объявлена" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTES_PATH' \
    kernel/include/appliance_management_authorization.h || {
    echo "ITEM-504B: API-цель маршрутов не объявлена" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ROUTING_PATH' \
    kernel/include/appliance_management_authorization.h || {
    echo "ITEM-504B: цель панели не объявлена" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ROUTING_CONFIG;' \
    kernel/src/appliance_management_authorization.c || {
    echo "ITEM-504B: цель не маршрутизируется в мутацию" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_ROUTING_MUTATION,' \
    kernel/src/appliance_management_authorization.c || {
    echo "ITEM-504B: команда не попала в матрицу" >&2
    exit 1
}
grep -Fq 'return "ROUTING_CONFIG";' \
    kernel/src/appliance_management_authorization.c || {
    echo "ITEM-504B: запись не называет мутацию" >&2
    exit 1
}
grep -Fq 'appliance_production_mutation_plane_routing_executor' \
    kernel/include/appliance_production_mutation_plane.h || {
    echo "ITEM-504B: у плоскости нет исполнителя маршрутов" >&2
    exit 1
}
grep -Fq 'appliance_production_mutation_plane_bind_routing(' \
    kernel/include/appliance_production_mutation_plane.h \
    kernel/src/appliance_production_mutation_plane.c || {
    echo "ITEM-504B: исполнитель маршрутов не привязывается" >&2
    exit 1
}
grep -Fq 'appliance_production_mutation_plane_routing_ready(' \
    kernel/src/appliance_production_mutation_plane.c || {
    echo "ITEM-504B: непривязанный исполнитель не отвергается" >&2
    exit 1
}
grep -Fq 'plane->routing.commit(' \
    kernel/src/appliance_production_mutation_plane.c || {
    echo "ITEM-504B: подтверждение не доходит до коммита маршрутов" >&2
    exit 1
}
grep -Fq 'plane->routing.stage(' \
    kernel/src/appliance_production_mutation_plane.c || {
    echo "ITEM-504B: стадия не доходит до исполнителя маршрутов" >&2
    exit 1
}

# ------------------------------------------------------------- the proofs ----
grep -Fq 'test-appliance-route-config-host' Makefile || {
    echo "ITEM-504B: Makefile не содержит пробы композиции" >&2
    exit 1
}
# shellcheck disable=SC2016  # the literal Makefile text is the contract
grep -Fq '$(MAKE) test-appliance-route-config-host' Makefile || {
    echo "ITEM-504B: host-набор не запускает пробу композиции" >&2
    exit 1
}
grep -Fq 'kernel/src/appliance_route_config.c' Makefile || {
    echo "ITEM-504B: проба плоскости не собирает композицию" >&2
    exit 1
}
for item504b_proof in \
    'the applied table starts empty' \
    'a complete add stages' \
    'staging changes nothing in the applied table' \
    'the staged candidate commits' \
    'a rolled-back stage never reaches the applied table' \
    'only the last staged candidate is applied' \
    'a duplicate name is refused by the model' \
    'the refusal names DUPLICATE_NAME' \
    'the applied table stays at the limit' \
    'a body over the framing bound is refused, not truncated' \
    'no malformed body reached the applied table'; do
    grep -Fq "$item504b_proof" tests/appliance-route-config-host.c || {
        echo "ITEM-504B: host-проба не утверждает: $item504b_proof" >&2
        exit 1
    }
done
for item504b_plane_proof in \
    'routing executor: an unbound executor is refused' \
    'routing executor: an authorized route stages' \
    'routing executor: staging changes nothing applied' \
    'routing executor: a wrong token applies nothing' \
    'routing executor: the confirmed route is applied' \
    'routing executor: the applied route is audited as ROUTING_CONFIG' \
    'routing executor: a duplicate route fails the stage' \
    'routing executor: the explicit cancel discards the remove' \
    'routing executor: a cancelled remove keeps the applied table'; do
    grep -Fq "$item504b_plane_proof" \
        tests/appliance-production-mutation-plane-host.c || {
        echo "ITEM-504B: проба плоскости не утверждает: $item504b_plane_proof" >&2
        exit 1
    }
done
# ITEM-505 increment 3 grew the matrix to 13 entries (the hostname command),
# ITEM-506 increment 6 to 14 (the credential change), ITEM-507 increment 4 to 15
# (the certificate-name command) and ITEM-508 increment 34 to 16 (the ACME
# control command); the pin still requires the exact count, so a silent entry
# cannot be added.
grep -Fq 'policy.matrix.count == 17U' \
    tests/appliance-management-authorization-host.c || {
    echo "ITEM-504B: проба авторизации не пинит число записей матрицы" >&2
    exit 1
}
grep -Fq 'ITEM-504 routing' tests/appliance-management-authorization-host.c || {
    echo "ITEM-504B: проба авторизации не называет новую запись" >&2
    exit 1
}

# ITEM-504: the confirmation window is milliseconds, and a person confirms on
# the panel page that printed the token.  The constant was named `..._TICKS`
# and read as 300 of them while `now_tick` is `hfoc_lwip_time_now()`, so the
# token expired 300 ms after it was issued and no HTTP client could confirm.
grep -Fq \
    '#define APPLIANCE_PRODUCTION_MUTATION_PLANE_CONFIRMATION_WINDOW_MS 300000U' \
    kernel/include/appliance_production_mutation_plane.h || {
    echo "ITEM-504B: окно подтверждения не названо в миллисекундах" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PRODUCTION_MUTATION_PLANE_CONFIRMATION_WINDOW_MS' \
    kernel/src/appliance_production_mutation_plane.c || {
    echo "ITEM-504B: плоскость не берёт окно подтверждения" >&2
    exit 1
}
grep -Fq 'the confirmation window leaves a person time to confirm' \
    tests/appliance-production-mutation-plane-host.c || {
    echo "ITEM-504B: проба плоскости не требует человеческого окна" >&2
    exit 1
}

# -------------------------------------------------------------- documents ----
grep -Fq 'ITEM-504' docs/ROADMAP.md || {
    echo "ITEM-504B: ROADMAP не называет пункт" >&2
    exit 1
}
grep -Fq 'appliance_route_config' docs/ARCHITECTURE.md || {
    echo "ITEM-504B: ARCHITECTURE не описывает кандидата маршрутов" >&2
    exit 1
}
grep -Fq 'ROUTING_CONFIG' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-504B: очередь не называет вид мутации" >&2
    exit 1
}
