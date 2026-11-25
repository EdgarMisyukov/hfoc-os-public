#!/usr/bin/env bash
# ITEM-509 increment 11: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-509 increment 11 (509.7a-2) makes the panel's read-side pages speak the
# language the machine selected.  The prose of the generic view, the section
# state, the routes, network, system, account, ACME and certificates readings
# and the Help block and refusal comes from the one catalog, English verbatim
# and Russian beside it; state values (AVAILABLE, UNAVAILABLE, DEGRADED), field
# names, commands and routes stay English machine words.  The budget proof
# renders every page in both languages and records the longer document.
for item509_inc11_file in \
    kernel/src/appliance_language.c \
    kernel/src/appliance_management_web_ui.c \
    tests/appliance-management-web-ui-host.c \
    docs/hfoc-management-web-ui.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc11_file ]] || {
        echo "ITEM-509 inc11: отсутствует $item509_inc11_file" >&2
        exit 1
    }
done

# 1. The read-page vocabulary is in the catalog, both languages, English
#    verbatim; the fragments around dynamic values are separate keys.
for item509_inc11_key in \
    '{"panel.view.hostname", "Hostname", "Имя узла"}' \
    '{"panel.routes.title", "Applied routes", "Применённые маршруты"}' \
    '{"panel.network.title", "Network configuration", "Настройка сети"}' \
    '{"panel.system.title", "System configuration", "Настройка системы"}' \
    '{"panel.certificates.active_names", "Active names=", "Действующие имена="}' \
    '{"panel.help.not_available", "Help is not available", "Справка недоступна"}'; do
    grep -Fq "$item509_inc11_key" kernel/src/appliance_language.c || {
        echo "ITEM-509 inc11: каталог не несёт: $item509_inc11_key" >&2
        exit 1
    }
done
grep -Fq 'panel.acme.control_link' kernel/src/appliance_language.c || {
    echo "ITEM-509 inc11: каталог не несёт текст ссылки ACME" >&2
    exit 1
}

# 2. The renderers read the catalog, and the literal English prose they used to
#    print is gone, so the two cannot drift apart.
for item509_inc11_call in \
    '"panel.view.hostname"' \
    '"panel.routes.none_published"' \
    '"panel.network.none_published"' \
    '"panel.system.language_active"' \
    '"panel.account.change_prefix"' \
    '"panel.acme.names_note"' \
    '"panel.certificates.none_published"' \
    '"panel.help.index_prefix"' \
    '"panel.state.unavailable"'; do
    grep -Fq -- "$item509_inc11_call" \
        kernel/src/appliance_management_web_ui.c || {
        echo "ITEM-509 inc11: рендерер не читает каталог: $item509_inc11_call" >&2
        exit 1
    }
done
for item509_inc11_gone in \
    '<dt>Hostname</dt>' \
    'No route table is published, so this page cannot list one' \
    'No network owner is published' \
    'No account owner is published' \
    'No ACME state is published' \
    'Help is not available' \
    'Help index, from the console'; do
    if grep -Fq -- "$item509_inc11_gone" \
        kernel/src/appliance_management_web_ui.c; then
        echo "ITEM-509 inc11: английская проза осталась литералом: $item509_inc11_gone" >&2
        exit 1
    fi
done

# 3. The language travels as the render's own parameter: the shared renderer
#    takes it, the dispatcher passes the one it already parsed, and the Help
#    pages pass theirs, so no renderer invents a language of its own.
grep -Fq 'appliance_management_web_ui_write_view(&buffer, view, language);' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-509 inc11: диспетчер не передаёт язык в view" >&2
    exit 1
}
grep -Fq 'buffer, language, "CATALOG_INCOMPLETE", missing_key);' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-509 inc11: Help-отказ не получает язык" >&2
    exit 1
}

# 4. The budget proof renders each page in Russian as well and records the
#    longer document; the Help row and constant move together.
grep -Fq 'production.view.language = APPLIANCE_LANGUAGE_RU;' \
    tests/appliance-management-web-ui-host.c || {
    echo "ITEM-509 inc11: бюджет не измерен по-русски" >&2
    exit 1
}
grep -Fq '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_HELP 3650U' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-509 inc11: бюджет Help не пере-измерен" >&2
    exit 1
}
grep -Fq '| help | 3650 | 3650 |' docs/hfoc-management-web-ui.md || {
    echo "ITEM-509 inc11: документ не называет измеренный бюджет Help" >&2
    exit 1
}
grep -Fq '| routing | 3532 | 3532 |' docs/hfoc-management-web-ui.md || {
    echo "ITEM-509 inc11: документ не называет измеренный бюджет Routing" >&2
    exit 1
}
