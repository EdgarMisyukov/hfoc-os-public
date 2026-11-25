#!/usr/bin/env bash
# ITEM-505 increment 2: contracted slice of the HFOC OS check suite.  Sourced
# in pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The eight generic panel sections used to render the literal `unavailable` on
# a real appliance, because the only publisher of the ITEM-384 view was the
# QEMU fixture.  This increment composes the view from the owners the
# appliance already has:
#
#   * `appliance_production_panel_view` copies each owner value into bounded
#     caller-owned storage and publishes a missing or oversized value as
#     `unavailable` rather than inventing or cutting it;
#   * the ordinary-boot composition reads the loaded hostname (with the same
#     identity fallback the management snapshot uses), the production
#     network's own MAC reading, the one service the listener serves and the
#     serving path's state, and publishes the view before the panel exists as
#     well as on every loop turn;
#   * the serving path stores the borrowed struct and hands it to the panel the
#     moment the panel is initialized, so the first section page is live;
#   * the generic page budgets are re-baselined against the worst case the
#     production view can publish, not only the short fixture values;
#   * the QEMU production proof requires the live hostname, MAC, service,
#     serving state and serving line on a generic section.

for item505_view_file in \
    kernel/include/appliance_production_panel_view.h \
    kernel/src/appliance_production_panel_view.c \
    kernel/include/appliance_management_web_ui.h \
    kernel/include/appliance_production_serving.h \
    kernel/src/appliance_production_serving.c \
    kernel/src/kernel.c \
    kernel/src/appliance_management_web_ui.c \
    tests/appliance-production-panel-view-host.c \
    tests/appliance-management-web-ui-host.c \
    scripts/test-qemu-production-service-https.sh \
    docs/hfoc-management-web-ui.md; do
    test -f "$item505_view_file" || {
        echo "ITEM-505: отсутствует файл: $item505_view_file" >&2
        exit 1
    }
done

# ------------------------------------------------------- the composition ------
grep -Fq 'APPLIANCE_PRODUCTION_PANEL_VIEW_HOSTNAME_MAX 64U' \
    kernel/include/appliance_production_panel_view.h || {
    echo "ITEM-505: у представления панели нет границы имени хоста" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PRODUCTION_PANEL_VIEW_MAC_MAX 18U' \
    kernel/include/appliance_production_panel_view.h || {
    echo "ITEM-505: у представления панели нет границы MAC" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PRODUCTION_PANEL_VIEW_SERVICE_MAX 16U' \
    kernel/include/appliance_production_panel_view.h || {
    echo "ITEM-505: у представления панели нет границы службы" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PRODUCTION_PANEL_VIEW_DETAIL_MAX 96U' \
    kernel/include/appliance_production_panel_view.h || {
    echo "ITEM-505: у представления панели нет границы строки состояния" >&2
    exit 1
}
# The recent-log bound is the renderer's own per-value bound: a longer value
# would be cut silently by the page, so the production line must fit it.
grep -Fq 'APPLIANCE_PRODUCTION_PANEL_VIEW_LOG_MAX 96U' \
    kernel/include/appliance_production_panel_view.h || {
    echo "ITEM-505: граница recent log не совпадает с границей рендера" >&2
    exit 1
}
grep -Fq 'appliance_production_panel_view_compose' \
    kernel/include/appliance_production_panel_view.h || {
    echo "ITEM-505: представление панели не собирается одной функцией" >&2
    exit 1
}
# A value that does not fit its bound is refused, never cut: a truncated
# hostname or MAC would name a machine that does not exist.
grep -Fq 'length + 1U > capacity' \
    kernel/src/appliance_production_panel_view.c || {
    echo "ITEM-505: значение, не влезающее в границу, не отвергается" >&2
    exit 1
}
for item505_view_field in \
    'storage->view.hostname = storage->hostname;' \
    'storage->view.mac_address = storage->mac_address;' \
    'storage->view.service_name = storage->service_name;' \
    'storage->view.detail = storage->detail;' \
    'storage->view.log_line = storage->log_line;'; do
    grep -Fq "$item505_view_field" \
        kernel/src/appliance_production_panel_view.c || {
        echo "ITEM-505: поле представления не публикуется: $item505_view_field" >&2
        exit 1
    }
done

# --------------------------------------------------------------- kernel -------
grep -Fq 'publish_production_panel_view' kernel/src/kernel.c || {
    echo "ITEM-505: обычная загрузка не публикует представление панели" >&2
    exit 1
}
# Once before the listener exists and once per loop turn: the panel must have
# the view the moment it is published, and the values must move with the owners.
item505_view_calls=$(grep -c '^    publish_production_panel_view();$\|^            publish_production_panel_view();$' \
    kernel/src/kernel.c)
test "$item505_view_calls" -ge 2 || {
    echo "ITEM-505: представление панели публикуется не на загрузке и не в цикле" >&2
    exit 1
}
for item505_owner in \
    'production_panel_hostname' \
    'appliance_production_network_mac' \
    'HTTPS_SERVICE_NAME' \
    'appliance_production_serving_state_string' \
    'appliance_production_serving_reason' \
    'appliance_production_serving_render_panel_line'; do
    grep -Fq "$item505_owner" kernel/src/kernel.c || {
        echo "ITEM-505: представление панели не читает владельца: $item505_owner" >&2
        exit 1
    }
done

# -------------------------------------------------------------- serving -------
grep -Fq 'appliance_production_serving_publish_panel_view' \
    kernel/include/appliance_production_serving.h \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-505: serving не публикует представление панели" >&2
    exit 1
}
grep -Fq 'bool panel_view_published;' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-505: serving не хранит признак опубликованного представления" >&2
    exit 1
}
grep -Fq 'appliance_management_panel_set_view(' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-505: serving не передаёт представление панели" >&2
    exit 1
}
grep -Fq 'appliance_production_serving_apply_view' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-505: представление, опубликованное до панели, не применяется" >&2
    exit 1
}
# The panel line carries state and listener only: it must fit the renderer's
# per-value bound for every state the serving path has.
grep -Fq 'APPLIANCE_PRODUCTION_SERVING_PANEL_LINE_MAX 96U' \
    kernel/include/appliance_production_serving.h || {
    echo "ITEM-505: у панельной строки serving нет границы" >&2
    exit 1
}
grep -Fq 'appliance_production_serving_render_panel_line' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-505: serving не рендерит панельную строку" >&2
    exit 1
}

# ------------------------------------------------------------------ host ------
grep -Fq 'test-appliance-production-panel-view-host' Makefile || {
    echo "ITEM-505: host-проба представления панели не зарегистрирована" >&2
    exit 1
}
# shellcheck disable=SC2016  # the single-quoted text is what the Makefile says
grep -Fq '$(MAKE) test-appliance-production-panel-view-host' Makefile || {
    echo "ITEM-505: host-проба представления панели не входит в прогон" >&2
    exit 1
}
grep -Fq 'appliance_production_panel_view_compose' \
    tests/appliance-production-panel-view-host.c || {
    echo "ITEM-505: host-проба не собирает представление" >&2
    exit 1
}
grep -Fq '52:54:00:12:34:56' \
    tests/appliance-production-panel-view-host.c || {
    echo "ITEM-505: host-проба не проверяет форму MAC" >&2
    exit 1
}
grep -Fq 'a hostname that does not fit is unavailable, not cut' \
    tests/appliance-production-panel-view-host.c || {
    echo "ITEM-505: host-проба не проверяет отказ от обрезки" >&2
    exit 1
}
# The page budget is now measured against the worst case the live view can
# publish, not only against the short QEMU fixture values.
grep -Fq 'fill_worst_case_view' \
    tests/appliance-management-web-ui-host.c || {
    echo "ITEM-505: бюджет страниц не меряется по живому представлению" >&2
    exit 1
}
grep -Fq 'appliance_production_panel_view_compose' \
    tests/appliance-management-web-ui-host.c || {
    echo "ITEM-505: бюджет страниц не собирает представление модулем" >&2
    exit 1
}

# --------------------------------------------------------------- budgets ------
# ITEM-508 increment 32 re-measured the Dashboard (1621 -> 1911) when the
# read-only ACME state block moved onto it, increment 34 re-measured it again
# (1911 -> 1942) when the block grew the link to the ACME control page, and
# ITEM-509 increment 11 re-measured every row with the longer of the English
# and the Russian rendering the read pages now serve.
for item505_budget in \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_DASHBOARD 2759U' \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_SERVICES 2544U' \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_STORAGE 2240U' \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_SYSTEM 2917U' \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_DIAGNOSTICS 2244U' \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_UPDATES 2643U' \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_RECOVERY 2653U'; do
    grep -Fq "$item505_budget" \
        kernel/include/appliance_management_web_ui.h || {
        echo "ITEM-505: бюджет страницы не перебазирован: $item505_budget" >&2
        exit 1
    }
done
for item505_row in \
    '| dashboard | 2759 | 2759 |' \
    '| services | 2544 | 2544 |' \
    '| storage | 2240 | 2240 |' \
    '| system | 2917 | 2917 |' \
    '| diagnostics | 2244 | 2244 |' \
    '| updates | 2643 | 2643 |' \
    '| recovery | 2653 | 2653 |'; do
    grep -Fq "$item505_row" docs/hfoc-management-web-ui.md || {
        echo "ITEM-505: документ бюджетов не называет строку: $item505_row" >&2
        exit 1
    }
done

# ------------------------------------------------------------- the QEMU proof -
for item505_qemu in \
    '<dt>Hostname</dt><dd>hfoc-appliance</dd>' \
    '<dt>Management MAC</dt>' \
    '<dd>52:54:00:12:34:56</dd>' \
    '<dt>Service</dt><dd>https</dd>' \
    'state=LISTENING reason=' \
    '[PRODUCTION-SERVING] state=LISTENING listener=live' \
    'the live view names the machine instead of'; do
    grep -Fq "$item505_qemu" \
        scripts/test-qemu-production-service-https.sh || {
        echo "ITEM-505: QEMU-проба не утверждает живое значение: $item505_qemu" >&2
        exit 1
    }
done

# ------------------------------------------------------------- the documents --
QUEUE_DOC=docs/superpowers/plans/2026-09-11-appliance-productization-queue.md
grep -A 3 '^## Item ITEM-505$' "$QUEUE_DOC" | grep -Fq 'ITEM-505' || {
    echo "ITEM-505: очередь не называет раздел пункта" >&2
    exit 1
}
grep -Fq 'ITEM-505 increment 2' docs/ROADMAP.md || {
    echo "ITEM-505: ROADMAP не называет второй инкремент" >&2
    exit 1
}
grep -Fq 'ITEM-505 increment 2' docs/ARCHITECTURE.md || {
    echo "ITEM-505: ARCHITECTURE не описывает живое представление панели" >&2
    exit 1
}
grep -Fq 'ITEM-505 increment 2' docs/hfoc-management-web-ui.md || {
    echo "ITEM-505: документ панели не описывает живое представление" >&2
    exit 1
}
