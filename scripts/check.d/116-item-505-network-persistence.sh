#!/usr/bin/env bash
# ITEM-505 increment 1: contracted slice of the HFOC OS check suite.  Sourced
# in pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The owner's rule (2026-09-30) is that a panel section which configures must
# read the current value from HFOC OS state, accept the change through the
# existing two-phase mutation, write it to non-volatile state, and apply it so
# the next boot carries it.  ITEM-504 did that for the routes; this increment
# does it for the network section:
#
#   * the mutation plane can bind a network executor whose stage, commit and
#     rollback own the candidate and the durable record, with the borrowed
#     RAM-only transaction kept as the fallback for the compositions that still
#     stage through it;
#   * the panel form (`POST /panel/network`) and the scriptable target
#     (`POST /api/v1/network`) are the same `NETWORK_CONFIG` mutation, POST
#     only, reached under the authorization matrix's configuration-write
#     command;
#   * the commit writes `appliance_persistent_network_config` through the same
#     setter the console menu and the install declaration use and publishes
#     `[PRODUCTION-NET-SET] source=PANEL … persisted=OK applies=reboot
#     current=…`; the live interface is not rebound and the page shows both
#     readings;
#   * a commit whose write fails answers APPLY_FAILED and claims nothing;
#   * the QEMU proof signs in, stages, confirms, reboots, requires
#     `[PRODUCTION-NET-MODE] mode=STATIC` with the saved address, and serves a
#     request on the applied address.

for item505_file in \
    kernel/include/appliance_production_mutation_plane.h \
    kernel/src/appliance_production_mutation_plane.c \
    kernel/include/appliance_management_authorization.h \
    kernel/src/appliance_management_authorization.c \
    kernel/include/appliance_management_web_ui.h \
    kernel/src/appliance_management_web_ui.c \
    kernel/include/appliance_management_panel.h \
    kernel/src/appliance_management_panel.c \
    kernel/include/http_parser.h \
    kernel/src/http_parser.c \
    kernel/src/appliance_production_serving.c \
    kernel/include/appliance_production_serving.h \
    kernel/src/kernel.c \
    tests/appliance-production-mutation-plane-host.c \
    tests/appliance-management-web-ui-host.c \
    tests/appliance-management-panel-host.c \
    tests/http-policy-host.c \
    scripts/test-qemu-production-service-https.sh; do
    test -f "$item505_file" || {
        echo "ITEM-505: отсутствует файл: $item505_file" >&2
        exit 1
    }
done

# ------------------------------------------------------- the mutation plane ---
grep -Fq 'appliance_production_mutation_plane_network_executor' \
    kernel/include/appliance_production_mutation_plane.h || {
    echo "ITEM-505: у плоскости нет исполнителя сети" >&2
    exit 1
}
grep -Fq 'appliance_production_mutation_plane_bind_network_executor' \
    kernel/include/appliance_production_mutation_plane.h || {
    echo "ITEM-505: исполнитель сети не привязывается" >&2
    exit 1
}
grep -Fq 'appliance_production_mutation_plane_network_executor_ready' \
    kernel/src/appliance_production_mutation_plane.c || {
    echo "ITEM-505: плоскость не проверяет готовность исполнителя сети" >&2
    exit 1
}
for item505_plane_call in \
    'plane->network_executor.stage' \
    'plane->network_executor.commit' \
    'plane->network_executor.rollback'; do
    grep -Fq "$item505_plane_call" \
        kernel/src/appliance_production_mutation_plane.c || {
        echo "ITEM-505: исполнитель сети не вызывается: $item505_plane_call" >&2
        exit 1
    }
done
# The borrowed transaction stays the fallback only: the executor wins when the
# caller bound one.
grep -Fq 'network_config_transaction_commit(plane->network)' \
    kernel/src/appliance_production_mutation_plane.c || {
    echo "ITEM-505: RAM-транзакция сети потеряна как фолбэк" >&2
    exit 1
}

# --------------------------------------------------------- authorization ------
grep -Fq '#define APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_NETWORK_PATH \
    "/panel/network"' \
    kernel/include/appliance_management_authorization.h || {
    echo "ITEM-505: у панельной формы сети нет объявленной цели" >&2
    exit 1
}
item505_panel_network_block=$(awk \
    '/APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_NETWORK_PATH\)\) \{/,/^    \} else if/' \
    kernel/src/appliance_management_authorization.c)
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_NETWORK_CONFIG' \
    <<<"$item505_panel_network_block" || {
    echo "ITEM-505: панельная форма сети не ведёт к NETWORK_CONFIG" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_NETWORK_MUTATION' \
    <<<"$item505_panel_network_block" || {
    echo "ITEM-505: панельная форма сети не требует config-write" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_METHOD_NOT_ALLOWED' \
    <<<"$item505_panel_network_block" || {
    echo "ITEM-505: панельная форма сети принимает не только POST" >&2
    exit 1
}

# -------------------------------------------------------------- wire policy ---
grep -Fq '#define HTTP_POLICY_NETWORK_TARGET "/api/v1/network"' \
    kernel/include/http_parser.h || {
    echo "ITEM-505: у сетевой API-цели нет POST-разрешения" >&2
    exit 1
}
grep -Fq '#define HTTP_POLICY_PANEL_NETWORK_TARGET "/panel/network"' \
    kernel/include/http_parser.h || {
    echo "ITEM-505: у панельной формы сети нет POST-разрешения" >&2
    exit 1
}
# The two network targets and `/panel/routing` share a length, so the table
# form is what keeps the comparison exact; a length-first chain would answer
# for the wrong target.
grep -Fq '{HTTP_POLICY_PANEL_NETWORK_TARGET,' \
    kernel/src/http_parser.c || {
    echo "ITEM-505: панельная цель сети не в таблице form-целей" >&2
    exit 1
}
if grep -Fq 'return target_length == HTTP_POLICY_ROUTING_TARGET_LENGTH' \
    kernel/src/http_parser.c; then
    echo "ITEM-505: form-цели снова сравниваются цепочкой ранних return" >&2
    exit 1
fi

# ------------------------------------------------------------- the serving ----
grep -Fq 'appliance_production_serving_network_stage' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-505: у serving нет стадии сети" >&2
    exit 1
}
grep -Fq 'appliance_production_serving_network_commit' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-505: у serving нет коммита сети" >&2
    exit 1
}
grep -Fq 'appliance_persistent_network_config_set_dhcp' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-505: коммит сети не пишет DHCP в энергонезависимый record" >&2
    exit 1
}
grep -Fq 'appliance_persistent_network_config_set_static' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-505: коммит сети не пишет static в энергонезависимый record" >&2
    exit 1
}
grep -Fq '"[PRODUCTION-NET-SET] source=PANEL mode="' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-505: запись сети не называет источник PANEL и режим" >&2
    exit 1
}
grep -Fq 'persisted=OK' kernel/src/appliance_production_serving.c ||
    {
        echo "ITEM-505: запись сети не называет результат записи" >&2
        exit 1
    }
grep -Fq 'applies=reboot current=' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-505: запись сети не называет перезагрузку и текущий режим" >&2
    exit 1
}
grep -Fq 'appliance_production_serving_network_current_short' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-505: запись сети не читает действующий режим интерфейса" >&2
    exit 1
}
grep -Fq 'appliance_production_serving_take_network_record' \
    kernel/src/appliance_production_serving.c \
    kernel/include/appliance_production_serving.h || {
    echo "ITEM-505: запись сети не выдаётся циклу" >&2
    exit 1
}
grep -Fq 'log_production_network_set' kernel/src/kernel.c || {
    echo "ITEM-505: цикл не публикует запись сети" >&2
    exit 1
}
grep -Fq 'appliance_production_serving_publish_network(serving)' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-505: страница сети не получает оба значения" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_NETWORK_PATH' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-505: форма сети не возвращает 303 на свою страницу" >&2
    exit 1
}

# ----------------------------------------------------------------- the page ---
grep -Fq 'struct appliance_management_web_ui_network' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-505: у страницы сети нет модели значений" >&2
    exit 1
}
grep -Fq 'const char *active;' kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-505: страница сети не публикует действующее значение" >&2
    exit 1
}
grep -Fq 'const char *saved;' kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-505: страница сети не публикует сохранённое значение" >&2
    exit 1
}
grep -Fq 'const char *applies;' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-505: страница сети не публикует, когда применяется" >&2
    exit 1
}
for item505_field in \
    'name=\"mode\"' \
    'name=\"address\"' \
    'name=\"prefix\"' \
    'name=\"gateway\"' \
    'name=\"dns\"' \
    'name=\"token\"'; do
    grep -Fq "$item505_field" kernel/src/appliance_management_web_ui.c || {
        echo "ITEM-505: форма сети не несёт поле: $item505_field" >&2
        exit 1
    }
done
grep -Fq 'APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_NETWORK' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-505: у страницы сети нет записанного бюджета" >&2
    exit 1
}
grep -Fq 'appliance_management_panel_set_network' \
    kernel/src/appliance_management_panel.c || {
    echo "ITEM-505: панель не публикует значения сети" >&2
    exit 1
}
grep -Fq 'panel->network_published ? &panel->network : NULL' \
    kernel/src/appliance_management_panel.c || {
    echo "ITEM-505: панель не передаёт значения сети на страницу" >&2
    exit 1
}

# ------------------------------------------------------------- the QEMU proof -
grep -Fq 'serving-network-client.py' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-505: у пробы нет клиента формы сети" >&2
    exit 1
}
grep -Fq 'SERVING_NETWORK_APPLY_PORT' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-505: у пробы нет отдельного порта применённой сети" >&2
    exit 1
}
grep -Fq "mode=static&address=10.0.2.15&prefix=24" \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-505: проба не ставит static-кандидата" >&2
    exit 1
}
grep -Fq 'NETWORK_SET_RECORD=' scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-505: проба не пинит запись [PRODUCTION-NET-SET]" >&2
    exit 1
}
grep -Fq 'persisted=OK applies=reboot current=DHCP' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-505: проба не требует честного current= в записи" >&2
    exit 1
}
grep -Fq 'NETWORK_APPLY_RECORD=' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-505: проба не пинит режим применённой сети" >&2
    exit 1
}
for item505_qemu in \
    'Active <code>mode=DHCP</code>' \
    'Saved <code>mode=STATIC address=10.0.2.15/24' \
    'source=COMMITTED' \
    'the commit rebound the live interface instead of ' \
    'TLS-PROOF network apply: the appliance serves on the saved '; do
    grep -Fq "$item505_qemu" \
        scripts/test-qemu-production-service-https.sh || {
        echo "ITEM-505: QEMU-проба не утверждает: $item505_qemu" >&2
        exit 1
    }
done
# The HTTPS response-matrix fixture serves the same page without a network
# owner, so its assertion must name the new page, not the removed generic
# value dump.
for item505_matrix in \
    'No network owner is published' \
    'name="mode"' \
    'name="token"'; do
    grep -Fq "$item505_matrix" \
        scripts/test-qemu-https-response-matrix.sh || {
        echo "ITEM-505: матрица ответов не знает новую страницу сети: $item505_matrix" >&2
        exit 1
    }
done

# --------------------------------------------------------------- the tests ----
grep -Fq 'test_network_executor' \
    tests/appliance-production-mutation-plane-host.c || {
    echo "ITEM-505: нет host-пробы исполнителя сети" >&2
    exit 1
}
grep -Fq 'test_network_page' tests/appliance-management-web-ui-host.c || {
    echo "ITEM-505: нет host-пробы страницы сети" >&2
    exit 1
}
grep -Fq 'appliance_management_panel_clear_network' \
    tests/appliance-management-panel-host.c || {
    echo "ITEM-505: панельная проба не проверяет отсутствующее значение сети" >&2
    exit 1
}
grep -Fq 'POST /panel/network HTTP/1.1' tests/http-policy-host.c || {
    echo "ITEM-505: нет пробы POST панельной формы сети" >&2
    exit 1
}
grep -Fq 'POST /api/v1/network HTTP/1.1' tests/http-policy-host.c || {
    echo "ITEM-505: нет пробы POST сетевой API-цели" >&2
    exit 1
}

# ------------------------------------------------------------- the documents --
QUEUE_DOC=docs/superpowers/plans/2026-09-11-appliance-productization-queue.md
grep -A 3 '^## Item ITEM-505$' "$QUEUE_DOC" | grep -Fq 'ITEM-505' || {
    echo "ITEM-505: очередь не называет раздел пункта" >&2
    exit 1
}
grep -Fq 'ITEM-505 removes the read-only property from the network target' \
    docs/ARCHITECTURE.md || {
    echo "ITEM-505: ARCHITECTURE не переписывает утверждение read-only" >&2
    exit 1
}
grep -Fq 'ITEM-505 update: the network target is no longer read-only' \
    docs/hfoc-production-readonly-management-api.md || {
    echo "ITEM-505: документ read-only API не называет сетевую мутацию" >&2
    exit 1
}
grep -Fq 'The Network section configures the appliance' \
    docs/hfoc-administrator-guide.md || {
    echo "ITEM-505: руководство администратора не описывает раздел сети" >&2
    exit 1
}
grep -Fq 'ITEM-505 — всё, что настроено в панели' docs/ROADMAP.md || {
    echo "ITEM-505: ROADMAP не называет пункт" >&2
    exit 1
}
