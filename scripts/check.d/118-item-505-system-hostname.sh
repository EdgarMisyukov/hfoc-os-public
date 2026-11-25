#!/usr/bin/env bash
# ITEM-505 increment 3: contracted slice of the HFOC OS check suite.  Sourced
# in pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The owner's rule (2026-09-30) is that a panel section which configures must
# read the current value from HFOC OS state, accept the change through the
# existing two-phase mutation, write it to non-volatile state, and apply it so
# the next boot carries it.  The network section does that since increment 1;
# this increment does it for the machine's own hostname:
#
#   * the mutation plane can bind a system executor whose stage, commit and
#     rollback own the candidate and the durable system record; without a
#     binding the mutation is refused as UNSUPPORTED_TARGET;
#   * the panel form (`POST /panel/system`) and the scriptable target
#     (`POST /api/v1/system`) are the same `SYSTEM_HOSTNAME` mutation, POST
#     only, reached under the authorization matrix's configuration-write
#     command; the GET of `/api/v1/system` keeps its read-only body;
#   * the stage accepts exactly one `hostname=…` field and validates it with
#     the configuration model's own rule; a refused name writes nothing;
#   * the commit writes `appliance_persistent_config` through the same
#     `appliance_persistent_config_set_hostname()` setter the first-boot wizard
#     uses and publishes `[PRODUCTION-SYSTEM-SET] source=PANEL hostname=…
#     persisted=OK|FAILED applies=reboot current=…`, where `current=` is the
#     identity captured at serving publication, never a rebound one;
#   * the System page shows both readings and both forms, and its budget is
#     re-measured;
#   * the QEMU proof stages `hostname=node-505`, confirms it, requires the
#     saved reading next to the unchanged active identity, reads the applied
#     record from serial, and requires the saved name in `/api/v1/system` after
#     the reboot.

for item505_system_file in \
    kernel/include/appliance_production_mutation_plane.h \
    kernel/src/appliance_production_mutation_plane.c \
    kernel/include/appliance_management_authorization.h \
    kernel/src/appliance_management_authorization.c \
    kernel/include/http_parser.h \
    kernel/src/http_parser.c \
    kernel/include/appliance_management_web_ui.h \
    kernel/src/appliance_management_web_ui.c \
    kernel/include/appliance_management_panel.h \
    kernel/src/appliance_management_panel.c \
    kernel/include/appliance_production_serving.h \
    kernel/src/appliance_production_serving.c \
    kernel/src/appliance_management_plane_qemu.c \
    kernel/src/kernel.c \
    tests/appliance-production-mutation-plane-host.c \
    tests/appliance-management-authorization-host.c \
    tests/appliance-management-web-ui-host.c \
    tests/appliance-management-panel-host.c \
    tests/http-policy-host.c \
    scripts/test-qemu-production-service-https.sh; do
    test -f "$item505_system_file" || {
        echo "ITEM-505: отсутствует файл: $item505_system_file" >&2
        exit 1
    }
done

# ------------------------------------------------------- the mutation plane ---
grep -Fq 'appliance_production_mutation_plane_system_executor' \
    kernel/include/appliance_production_mutation_plane.h || {
    echo "ITEM-505: у плоскости нет исполнителя системы" >&2
    exit 1
}
grep -Fq 'appliance_production_mutation_plane_bind_system' \
    kernel/include/appliance_production_mutation_plane.h \
    kernel/src/appliance_production_mutation_plane.c || {
    echo "ITEM-505: исполнитель системы не привязывается" >&2
    exit 1
}
grep -Fq 'appliance_production_mutation_plane_system_ready' \
    kernel/src/appliance_production_mutation_plane.c || {
    echo "ITEM-505: плоскость не проверяет готовность исполнителя системы" >&2
    exit 1
}
for item505_system_call in \
    'plane->system.stage' \
    'plane->system.commit' \
    'plane->system.rollback'; do
    grep -Fq "$item505_system_call" \
        kernel/src/appliance_production_mutation_plane.c || {
        echo "ITEM-505: исполнитель системы не вызывается: $item505_system_call" >&2
        exit 1
    }
done
# The system mutation has no borrowed fallback: without the bound executor it
# must be refused, not reported as persisted.
grep -Fq 'APPLIANCE_PRODUCTION_MUTATION_PLANE_SYSTEM_OK' \
    kernel/src/appliance_production_mutation_plane.c || {
    echo "ITEM-505: исход коммита системы не назван" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PRODUCTION_MUTATION_PLANE_SYSTEM_APPLY_FAILED' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-505: отказ записи системы не назван" >&2
    exit 1
}

# --------------------------------------------------------- the authorization --
grep -Fq '#define APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_SYSTEM_MUTATION 14U' \
    kernel/include/appliance_management_authorization.h || {
    echo "ITEM-505: у мутации системы нет своей команды" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_SYSTEM_HOSTNAME = 8' \
    kernel/include/appliance_management_authorization.h || {
    echo "ITEM-505: у мутации системы нет своего вида" >&2
    exit 1
}
grep -Fq '#define APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_SYSTEM_PATH "/panel/system"' \
    kernel/include/appliance_management_authorization.h || {
    echo "ITEM-505: панельная цель системы не названа" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_SYSTEM_MUTATION,' \
    kernel/src/appliance_management_authorization.c || {
    echo "ITEM-505: команда системы не входит в матрицу" >&2
    exit 1
}
# ITEM-506 increment 6 grew the same matrix to 14 entries (the credential
# change), ITEM-507 increment 4 to 15 (the certificate-name command) and
# ITEM-508 increment 34 to 16 (the ACME control command); the pins below still
# require the exact count and the exact arrays, so a silent entry cannot be
# added.
grep -Fq 'struct authorization_entry entries[17];' \
    kernel/src/appliance_management_authorization.c || {
    echo "ITEM-505/ITEM-506: матрица не выросла на запись системы и учётной записи" >&2
    exit 1
}
grep -Fq 'index < 17U' kernel/src/appliance_management_authorization.c || {
    echo "ITEM-505/ITEM-506: матрица не заполняет запись системы и учётной записи" >&2
    exit 1
}
# The QEMU management-plane fixture pins the exact matrix set too, and the
# HTTPS service proof fails before its listener if that pin goes stale.
grep -Fq 'policy.matrix.count == 17U' \
    kernel/src/appliance_management_plane_qemu.c || {
    echo "ITEM-505/ITEM-506: QEMU-фикстура не знает выросшую матрицу" >&2
    exit 1
}
# Both targets resolve to the same mutation and only POST is accepted; the GET
# of the API target keeps the system-status read route.
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_SYSTEM_PATH' \
    kernel/src/appliance_management_authorization.c || {
    echo "ITEM-505: панельная цель системы не разрешается в мутацию" >&2
    exit 1
}
grep -Fq 'return "SYSTEM_HOSTNAME";' \
    kernel/src/appliance_management_authorization.c || {
    echo "ITEM-505: вид мутации системы не назван строкой" >&2
    exit 1
}

# --------------------------------------------------------------- http policy --
for item505_target in \
    '#define HTTP_POLICY_SYSTEM_TARGET "/api/v1/system"' \
    '#define HTTP_POLICY_PANEL_SYSTEM_TARGET "/panel/system"'; do
    grep -Fq "$item505_target" kernel/include/http_parser.h || {
        echo "ITEM-505: цель POST системы не объявлена: $item505_target" >&2
        exit 1
    }
done
for item505_table in \
    '{HTTP_POLICY_SYSTEM_TARGET, HTTP_POLICY_SYSTEM_TARGET_LENGTH},' \
    '{HTTP_POLICY_PANEL_SYSTEM_TARGET,'; do
    grep -Fq "$item505_table" kernel/src/http_parser.c || {
        echo "ITEM-505: цель POST системы не разрешена таблицей: $item505_table" >&2
        exit 1
    }
done

# ------------------------------------------------------------------ the page --
grep -Fq 'struct appliance_management_web_ui_system {' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-505: у страницы системы нет структуры значений" >&2
    exit 1
}
grep -Fq 'appliance_management_web_ui_write_system_form' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-505: страница системы не несёт форму" >&2
    exit 1
}
grep -Fq 'No system owner is published' \
    tests/appliance-management-web-ui-host.c \
    tests/appliance-management-panel-host.c || {
    echo "ITEM-505: страница системы не называет отсутствующего владельца" >&2
    exit 1
}
grep -Fq 'action=\"/panel/system\"' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-505: форма системы не публикует свою цель" >&2
    exit 1
}
grep -Fq 'appliance_management_web_ui_section_confirmable' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-505: страница системы не участвует в подтверждении" >&2
    exit 1
}
# The System section is a configuring section now, so its page is no longer the
# generic value dump and its budget is the measured 1810 (ITEM-506 increment 6
# re-measured it with the Account link and the real longest mutation name).
grep -Fq 'APPLIANCE_MANAGEMENT_WEB_UI_SECTION_SYSTEM) {' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-505: страница системы не имеет своей ветки" >&2
    exit 1
}
grep -Fq '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_SYSTEM 2917U' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-505: бюджет страницы системы не перемерен" >&2
    exit 1
}
grep -Fq '| system | 2917 | 2917 |' docs/hfoc-management-web-ui.md || {
    echo "ITEM-505: документ бюджетов не называет страницу системы" >&2
    exit 1
}

# ----------------------------------------------------------------- the panel --
grep -Fq 'appliance_management_panel_set_system' \
    kernel/include/appliance_management_panel.h \
    kernel/src/appliance_management_panel.c || {
    echo "ITEM-505: панель не принимает значения системы" >&2
    exit 1
}
grep -Fq 'appliance_management_panel_clear_system' \
    kernel/src/appliance_management_panel.c || {
    echo "ITEM-505: панель не снимает значения системы" >&2
    exit 1
}
grep -Fq 'panel->system_published ? &panel->system : NULL' \
    kernel/src/appliance_management_panel.c || {
    echo "ITEM-505: значения системы не доходят до рендера" >&2
    exit 1
}

# --------------------------------------------------------------- the serving --
grep -Fq 'appliance_production_serving_handle_system' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-505: serving не обслуживает мутацию системы" >&2
    exit 1
}
grep -Fq 'appliance_production_serving_system_stage' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-505: serving не разбирает форму имени хоста" >&2
    exit 1
}
grep -Fq 'appliance_production_serving_system_capture_active' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-505: serving не захватывает работающую личность" >&2
    exit 1
}
grep -Fq 'appliance_config_validation_validate_hostname' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-505: имя хоста не проверяется правилом модели" >&2
    exit 1
}
grep -Fq 'appliance_persistent_config_set_hostname' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-505: коммит не пишет durable record имени хоста" >&2
    exit 1
}
grep -Fq '"[PRODUCTION-SYSTEM-SET] source=PANEL hostname="' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-505: запись [PRODUCTION-SYSTEM-SET] не собирается" >&2
    exit 1
}
grep -Fq 'appliance_production_serving_publish_system' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-505: serving не публикует значения системы панели" >&2
    exit 1
}
grep -Fq 'appliance_production_mutation_plane_bind_system(' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-505: serving не привязывает исполнителя системы" >&2
    exit 1
}
grep -Fq '#define APPLIANCE_PRODUCTION_SERVING_SYSTEM_RECORD_MAX 224U' \
    kernel/include/appliance_production_serving.h || {
    echo "ITEM-505: у записи системы нет границы" >&2
    exit 1
}
grep -Fq 'appliance_production_serving_take_system_record' \
    kernel/include/appliance_production_serving.h \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-505: запись системы не передаётся циклу" >&2
    exit 1
}
grep -Fq 'log_production_system_set' kernel/src/kernel.c || {
    echo "ITEM-505: цикл не публикует запись системы" >&2
    exit 1
}
# The dispatch order is explicit: the mutation handler is consulted before the
# panel handler, so a POST to `/panel/system` never reaches the read dispatch.
item505_handle_line=$(grep -n 'appliance_production_serving_handle_system(' \
    kernel/src/appliance_production_serving.c | tail -n 1 | cut -d: -f1)
item505_panel_line=$(grep -n 'appliance_production_serving_handle_panel(' \
    kernel/src/appliance_production_serving.c | tail -n 1 | cut -d: -f1)
if [[ -z $item505_handle_line || -z $item505_panel_line ||
      $item505_handle_line -ge $item505_panel_line ]]; then
    echo "ITEM-505: обработчик системы не раньше обработчика панели" >&2
    exit 1
fi

# --------------------------------------------------------------- the tests ----
for item505_system_test in \
    'system executor: an unbound hostname mutation is refused' \
    'system executor: a failed durable write is APPLY_FAILED' \
    'system executor: the unbound hostname mutation is refused again'; do
    grep -Fq "$item505_system_test" \
        tests/appliance-production-mutation-plane-host.c || {
        echo "ITEM-505: нет host-пробы плоскости системы: $item505_system_test" >&2
        exit 1
    }
done
grep -Fq 'test_system_page' tests/appliance-management-web-ui-host.c || {
    echo "ITEM-505: нет host-пробы страницы системы" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PERSISTENT_CONFIG_HOSTNAME_MAX' \
    tests/appliance-management-web-ui-host.c || {
    echo "ITEM-505: бюджет системы не меряется по границе модели" >&2
    exit 1
}
grep -Fq 'appliance_management_panel_clear_system' \
    tests/appliance-management-panel-host.c || {
    echo "ITEM-505: панельная проба не проверяет отсутствующее значение системы" >&2
    exit 1
}
grep -Fq 'MUTATION_SYSTEM_HOSTNAME' \
    tests/appliance-management-authorization-host.c || {
    echo "ITEM-505: проба авторизации не знает мутацию системы" >&2
    exit 1
}
grep -Fq 'POST /panel/system HTTP/1.1' tests/http-policy-host.c || {
    echo "ITEM-505: нет пробы POST панельной формы системы" >&2
    exit 1
}
grep -Fq 'POST /api/v1/system HTTP/1.1' tests/http-policy-host.c || {
    echo "ITEM-505: нет пробы POST системной API-цели" >&2
    exit 1
}

# ------------------------------------------------------------- the QEMU proof -
for item505_system_qemu in \
    'TLS-PROOF system page: the running identity and the saved hostname are' \
    'TLS-PROOF system stage: the pending hostname and its token are served' \
    'TLS-PROOF system confirmation: the hostname is applied and durable' \
    'TLS-PROOF system apply: the saved hostname is the running' \
    'hostname=node-505' \
    '"token":' \
    "[PRODUCTION-SYSTEM-SET] source=PANEL hostname=node-505 persisted=OK applies=reboot current=hfoc-appliance"; do
    grep -Fq "$item505_system_qemu" \
        scripts/test-qemu-production-service-https.sh || {
        echo "ITEM-505: QEMU-проба не утверждает систему: $item505_system_qemu" >&2
            exit 1
        }
done
# The generic live view is still proven, but on a section that stayed generic.
grep -Fq 'get(b"/panel/storage", cookie=cookie_pair)' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-505: живое представление больше не проверяется на обычном разделе" >&2
    exit 1
}

# ------------------------------------------------------------- the documents --
QUEUE_DOC=docs/superpowers/plans/2026-09-11-appliance-productization-queue.md
grep -A 3 '^## Item ITEM-505$' "$QUEUE_DOC" | grep -Fq 'ITEM-505' || {
    echo "ITEM-505: очередь не называет раздел пункта" >&2
    exit 1
}
grep -Fq 'Инкремент 3 (этот PR) — раздел System настраивает имя хоста' \
    "$QUEUE_DOC" || {
    echo "ITEM-505: очередь не описывает третий инкремент" >&2
    exit 1
}
grep -Fq 'ITEM-505 increment 3' docs/ROADMAP.md || {
    echo "ITEM-505: ROADMAP не называет третий инкремент" >&2
    exit 1
}
grep -Fq 'ITEM-505 increment 3' docs/ARCHITECTURE.md || {
    echo "ITEM-505: ARCHITECTURE не описывает мутацию имени хоста" >&2
    exit 1
}
grep -Fq 'ITEM-505 increment 3: the system hostname target is no longer read-only' \
    docs/hfoc-production-readonly-management-api.md || {
    echo "ITEM-505: документ read-only API не называет системную мутацию" >&2
    exit 1
}
grep -Fq "The System section configures the machine's own hostname" \
    docs/hfoc-administrator-guide.md || {
    echo "ITEM-505: руководство администратора не описывает раздел системы" >&2
    exit 1
}
