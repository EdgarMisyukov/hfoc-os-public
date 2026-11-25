#!/usr/bin/env bash
# ITEM-504 part D: contracted slice of the HFOC OS check suite.  Sourced in
# pinned order by scripts/check.sh; lint it on its own with the linter.

# The Routing page is the panel's first mutating section.  It is served like
# every other section - the path comes from the one section table the router
# matches - and it renders what its owner published:
#
#   * `SECTION_COUNT` is ten and `..._SECTION_ROUTING` is appended, so the nine
#     read-side sections keep their indices and `page = section + 1`;
#   * the page lists the applied table the route configuration published
#     (`appliance_management_web_ui_routes`), names an unpublished table and an
#     empty one instead of inventing a list, counts a table too large for the
#     page instead of cutting it, and escapes every byte it renders;
#   * the stage form posts `action`, `name`, `backend`, `port` and `tls` to
#     `/panel/routing`, and the confirmation form returns `token` with
#     `confirm` or `cancel`; the page states the pending candidate's mutation,
#     token and remaining window in milliseconds, the unit the plane compares;
#   * the serving path fills the bounded copy from the applied table before it
#     dispatches the page, and the panel stores the copy it renders.

for item504d_file in \
    kernel/include/appliance_management_web_ui.h \
    kernel/src/appliance_management_web_ui.c \
    kernel/include/appliance_management_panel.h \
    kernel/src/appliance_management_panel.c \
    kernel/src/appliance_production_serving.c \
    tests/appliance-management-web-ui-host.c \
    docs/hfoc-management-web-ui.md; do
    test -f "$item504d_file" || {
        echo "ITEM-504D: отсутствует файл: $item504d_file" >&2
        exit 1
    }
done

# ---------------------------------------------------------- section model ----
# ITEM-506 increment 6 appends Account after Routing, so the model is eleven
# sections; Routing's own index (9) is unchanged and pinned below.
grep -Fq '#define APPLIANCE_MANAGEMENT_WEB_UI_SECTION_COUNT 13U' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-504D/ITEM-506: модель панели не содержит двенадцать разделов" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ROUTING = 9' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-504D: раздел маршрутизации не объявлен" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_WEB_UI_PAGE_ROUTING = 10' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-504D: страница маршрутизации не объявлена" >&2
    exit 1
}
grep -Fq '"/panel/routing",' kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-504D: путь раздела маршрутизации не в таблице секций" >&2
    exit 1
}
# ITEM-509 increment 10 removed the module's own name table: the label now
# comes from the catalog through the section key table, so the pin moved with
# it rather than being dropped.
grep -Fq '"panel.section.routing",' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-504D: раздел маршрутизации не назван" >&2
    exit 1
}
grep -Fq '{"panel.section.routing", "Routing", "Маршрутизация"}' \
    kernel/src/appliance_language.c || {
    echo "ITEM-504D: каталог не несёт имя раздела маршрутизации" >&2
    exit 1
}

# ------------------------------------------------------- the routes value ----
grep -Fq 'struct appliance_management_web_ui_routes {' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-504D: применённая таблица не описана как значение" >&2
    exit 1
}
grep -Fq '#define APPLIANCE_MANAGEMENT_WEB_UI_ROUTES_TEXT_MAX ' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-504D: у применённой таблицы нет границы" >&2
    exit 1
}
grep -Fq 'appliance_management_panel_set_routes' \
    kernel/include/appliance_management_panel.h \
    kernel/src/appliance_management_panel.c \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504D: владелец не публикует применённую таблицу" >&2
    exit 1
}
grep -Fq 'appliance_production_serving_publish_routes' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504D: serving не собирает применённую таблицу" >&2
    exit 1
}
grep -Fq 'appliance_route_render' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504D: строка маршрута не берётся у записи маршрута" >&2
    exit 1
}
grep -Fq 'panel->routes_published ? &panel->routes : NULL' \
    kernel/src/appliance_management_panel.c || {
    echo "ITEM-504D: панель не передаёт таблицу на страницу" >&2
    exit 1
}
grep -Fq '"panel.routes.none_published"' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-504D: страница не называет неопубликованную таблицу" >&2
    exit 1
}
grep -Fq '"panel.routes.none_applied"' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-504D: страница не объясняет пустую таблицу" >&2
    exit 1
}
grep -Fq '"panel.routes.truncated"' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-504D: страница не считает не поместившиеся маршруты" >&2
    exit 1
}

# ------------------------------------------------------------ the forms ------
grep -Fq 'action=\"/panel/routing\"' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-504D: форма не публикует цель маршрутов" >&2
    exit 1
}
for item504d_field in action name backend port tls token confirm cancel; do
    grep -Fq 'name=\"'"$item504d_field"'\"' \
        kernel/src/appliance_management_web_ui.c || {
        echo "ITEM-504D: форма не публикует поле: $item504d_field" >&2
        exit 1
    }
done
# ITEM-509 increment 12 moved the confirmation prose into the catalog; the
# renderer names the key and the catalog carries the milliseconds sentence.
grep -Fq '"panel.confirm.ms_left"' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-504D: подтверждение не называет ключ окна в миллисекундах" >&2
    exit 1
}
grep -Fq 'ms left; single-use' kernel/src/appliance_language.c || {
    echo "ITEM-504D: подтверждение не называет окно в миллисекундах" >&2
    exit 1
}
grep -Fq 'case APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ROUTING_CONFIG:' \
    kernel/src/appliance_management_authorization.c || {
    echo "ITEM-504D: страница не показывает кандидата маршрутов" >&2
    exit 1
}
grep -Fq 'appliance_management_authorization_mutation_has_panel_confirmation(' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504D: страница не спрашивает общий список подтверждения" >&2
    exit 1
}

# ------------------------------------------------------------ the budget -----
grep -Fq '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_ROUTING ' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-504D: бюджет страницы маршрутизации не записан" >&2
    exit 1
}
grep -Fq '| routing |' docs/hfoc-management-web-ui.md || {
    echo "ITEM-504D: документ не повторяет бюджет страницы" >&2
    exit 1
}

# ---------------------------------------------------------- host proofs ------
for item504d_proof in \
    'management web UI Routing page lists the applied table and both ' \
    'management web UI Routing page names an unpublished table' \
    'management web UI Routing page publishes an empty applied table ' \
    'management web UI Routing page counts a table it cannot list ' \
    'management web UI Routing page escapes the table it lists' \
    'management web UI Routing page publishes the pending token, its ' \
    'management web UI Routing section publishes the applied '; do
    grep -Fq "$item504d_proof" \
        tests/appliance-management-web-ui-host.c || {
        echo "ITEM-504D: host-проба не утверждает: $item504d_proof" >&2
        exit 1
    }
done

# ------------------------------------------------------------ QEMU proof -----
grep -Fq 'the applied route and the form are served' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504D: QEMU-проба не читает страницу маршрутов" >&2
    exit 1
}
grep -Fq '"/panel/routing",' scripts/test-qemu-https-response-matrix.sh || {
    echo "ITEM-504D: матрица ответов не обходит страницу маршрутов" >&2
    exit 1
}

# -------------------------------------------------------------- documents ----
grep -Fq 'ITEM-504' docs/ROADMAP.md || {
    echo "ITEM-504D: ROADMAP не называет пункт" >&2
    exit 1
}
grep -Fq 'страницу Routing' docs/ARCHITECTURE.md || {
    echo "ITEM-504D: ARCHITECTURE не описывает страницу маршрутов" >&2
    exit 1
}
