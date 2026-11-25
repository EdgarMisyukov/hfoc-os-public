#!/usr/bin/env bash
# ITEM-509 increment 10: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-509 increment 10 is 509.7a-1: the panel framework speaks the chosen
# language.  The dispatcher parses the `v1:system.language` record once, the
# document's `lang` attribute, the navigation, the section heading and the
# footer read the one catalog through it, and the module's own section-name
# table is gone.  The claims pinned here:
#
#   * the language travels as a field of the renderer inputs (`view`,
#     `session`, `licenses`, `acme_control`), so no renderer signature moved and
#     the zero value stays English - the pre-increment document, byte for byte;
#   * the dispatcher reads the language from the same system record the System
#     page publishes and applies it to a copy, so the stored views stay the
#     owner's publication;
#   * the catalog is the only source of section names: `section_names[]` is
#     removed, the navigation and the page heading read `panel.section.*`, and
#     the Help section's new key is in the catalog;
#   * a key the catalog cannot answer in the requested language latches
#     `missing_translation` and the render refuses by name; the panel answers a
#     named 503 (`APPLIANCE_MANAGEMENT_PANEL_MISSING_TRANSLATION`) instead of
#     the zero status the old overflow path left behind;
#   * the English markers of the live proofs are untouched: the static sign-in
#     shell keeps `lang="en"`, and the QEMU fixtures still require the English
#     navigation line;
#   * the host proofs render both languages and refuse an unknown one.
for item509_inc10_file in \
    kernel/include/appliance_management_web_ui.h \
    kernel/src/appliance_management_web_ui.c \
    kernel/include/appliance_management_panel.h \
    kernel/src/appliance_management_panel.c \
    kernel/src/appliance_language.c \
    tests/appliance-management-web-ui-host.c \
    tests/appliance-management-panel-host.c \
    docs/hfoc-management-web-ui.md \
    docs/hfoc-language-surfaces.md \
    docs/ROADMAP.md \
    docs/ARCHITECTURE.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc10_file ]] || {
        echo "ITEM-509 inc10: отсутствует $item509_inc10_file" >&2
        exit 1
    }
done

# 1. The language is a field of the renderer inputs; a caller that does not
#    fill it renders English, and no renderer signature grew a parameter.
for item509_inc10_field in \
    'enum appliance_language language;' \
    'APPLIANCE_MANAGEMENT_WEB_UI_MISSING_TRANSLATION = 5,' ; do
    grep -Fq -- "$item509_inc10_field" \
        kernel/include/appliance_management_web_ui.h || {
        echo "ITEM-509 inc10: заголовок панели не несёт: $item509_inc10_field" >&2
        exit 1
    }
done
grep -Fq 'APPLIANCE_MANAGEMENT_PANEL_MISSING_TRANSLATION = 11,' \
    kernel/include/appliance_management_panel.h || {
    echo "ITEM-509 inc10: у панели нет именованного статуса отказа перевода" >&2
    exit 1
}

# 2. The head publishes the code the catalog answers for the requested
#    language, and the navigation and the heading read the catalog.
grep -Fq 'const char *code = appliance_language_code(language);' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-509 inc10: <html lang> берётся не из каталога" >&2
    exit 1
}
grep -Fq 'appliance_management_web_ui_buffer_language(' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-509 inc10: нет поиска строки каталога для страницы" >&2
    exit 1
}
grep -Fq 'appliance_language_string(language, key);' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-509 inc10: хелпер каталога не читает единственный поиск" >&2
    exit 1
}
grep -Fq 'appliance_management_web_ui_section_keys[index]);' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-509 inc10: навигация не читает имя раздела из каталога" >&2
    exit 1
}
if grep -Fq 'appliance_management_web_ui_section_names' \
    kernel/src/appliance_management_web_ui.c; then
    echo "ITEM-509 inc10: у имён разделов снова два источника" >&2
    exit 1
fi
grep -Fq 'return appliance_language_string(' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-509 inc10: имя раздела не сведено к каталогу" >&2
    exit 1
}

# 3. The catalog carries the Help section and the footer, in both languages.
for item509_inc10_catalog in \
    '{"panel.section.help", "Help", "Справка"}' \
    '{"panel.footer", "HFOC Appliance management panel",' ; do
    grep -Fq -- "$item509_inc10_catalog" kernel/src/appliance_language.c || {
        echo "ITEM-509 inc10: каталог не несёт: $item509_inc10_catalog" >&2
        exit 1
    }
done

# 4. A missing translation is a named refusal, not an English page.
grep -Fq 'bool missing_translation;' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-509 inc10: буфер не несёт признак отсутствующего перевода" >&2
    exit 1
}
grep -Fq 'buffer->missing_translation = true;' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-509 inc10: отсутствующий перевод не защёлкивается" >&2
    exit 1
}
count_missing=$(grep -c 'APPLIANCE_MANAGEMENT_WEB_UI_MISSING_TRANSLATION;' \
    kernel/src/appliance_management_web_ui.c || true)
[[ $count_missing -ge 4 ]] || {
    echo "ITEM-509 inc10: не все рендереры отвечают именованным отказом" >&2
    exit 1
}

# 5. The dispatcher resolves the request's language once, copies the stored
#    views and answers the refusal with a named 503.  ITEM-513 made the
#    resolved language the source for every renderer (the browser's cookie,
#    then the record, then the default), so the four call sites now take that
#    one value instead of asking the record again per page.
grep -Fq 'static enum appliance_language appliance_management_panel_language(' \
    kernel/src/appliance_management_panel.c || {
    echo "ITEM-509 inc10: у панели нет единственного разбора языка" >&2
    exit 1
}
grep -Fq 'appliance_language_parse(' kernel/src/appliance_management_panel.c || {
    echo "ITEM-509 inc10: панель не разбирает запись языка" >&2
    exit 1
}
grep -Fq 'session.language = language;' \
    kernel/src/appliance_management_panel.c || {
    echo "ITEM-509 inc10: страница входа не получает язык записи" >&2
    exit 1
}
grep -Fq 'view.language = language;' \
    kernel/src/appliance_management_panel.c || {
    echo "ITEM-509 inc10: страницы разделов не получают язык записи" >&2
    exit 1
}
grep -Fq 'licenses.language = language;' \
    kernel/src/appliance_management_panel.c || {
    echo "ITEM-509 inc10: страница лицензий не получает язык записи" >&2
    exit 1
}
grep -Fq 'control.language = language;' \
    kernel/src/appliance_management_panel.c || {
    echo "ITEM-509 inc10: страница ACME не получает язык записи" >&2
    exit 1
}
grep -Fq 'appliance_management_panel_render_refusal(' \
    kernel/src/appliance_management_panel.c || {
    echo "ITEM-509 inc10: отказ перевода не превращается в именованный ответ" >&2
    exit 1
}
grep -Fq 'return APPLIANCE_MANAGEMENT_PANEL_MISSING_TRANSLATION;' \
    kernel/src/appliance_management_panel.c || {
    echo "ITEM-509 inc10: панель не публикует именованный статус отказа" >&2
    exit 1
}

# 6. The proofs render both languages and refuse an unknown one.
grep -Fq 'static void test_language_framework(void)' \
    tests/appliance-management-web-ui-host.c || {
    echo "ITEM-509 inc10: host-проба панели не несёт языковой каркас" >&2
    exit 1
}
grep -Fq 'test_language_framework();' tests/appliance-management-web-ui-host.c || {
    echo "ITEM-509 inc10: языковой каркас панели не вызван" >&2
    exit 1
}
grep -Fq '"ITEM-509: the Russian view publishes the Russian navigation, "' \
    tests/appliance-management-web-ui-host.c || {
    echo "ITEM-509 inc10: проба не требует русскую навигацию" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_WEB_UI_MISSING_TRANSLATION' \
    tests/appliance-management-web-ui-host.c || {
    echo "ITEM-509 inc10: проба не требует именованный отказ перевода" >&2
    exit 1
}
grep -Fq 'static void test_language_record(void)' \
    tests/appliance-management-panel-host.c || {
    echo "ITEM-509 inc10: проба диспетчера не несёт язык записи" >&2
    exit 1
}
grep -Fq 'test_language_record();' tests/appliance-management-panel-host.c || {
    echo "ITEM-509 inc10: проверка языка записи не вызвана" >&2
    exit 1
}
grep -Fq '"ITEM-509: the panel renders the language the system record holds"' \
    tests/appliance-management-panel-host.c || {
    echo "ITEM-509 inc10: проба диспетчера не требует язык записи" >&2
    exit 1
}

# 7. The English path is untouched: the static sign-in shell keeps its honest
#    `lang="en"`, and the live QEMU fixtures still require the English
#    navigation line.
grep -Fq '"<html lang=\"en\">\n"' kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-509 inc10: статическая оболочка входа потеряла lang=en" >&2
    exit 1
}
grep -Fq '<nav>Dashboard Network Services Storage System Security Diagnostics Updates Recovery Routing Account Certificates Help</nav>' \
    scripts/test-qemu-https-service.sh || {
    echo "ITEM-509 inc10: QEMU-проба потеряла английскую навигацию" >&2
    exit 1
}
grep -Fq '"<nav>Dashboard Network Services Storage System Security Diagnostics Updates Recovery Routing Account Certificates Help</nav>\n"' \
    tests/https-handler-host.c || {
    echo "ITEM-509 inc10: host-проба транспорта потеряла английскую навигацию" >&2
    exit 1
}

# 8. The documents record the increment and the moved budgets.
for item509_inc10_doc in docs/ROADMAP.md docs/ARCHITECTURE.md \
    docs/hfoc-language-surfaces.md docs/hfoc-management-web-ui.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    grep -Fq 'ITEM-509 increment 10' "$item509_inc10_doc" || {
        echo "ITEM-509 inc10: $item509_inc10_doc не называет инкремент" >&2
        exit 1
    }
done
# ITEM-513 raised the ceiling again with the nav's language switch; this pin
# states the current value and the item that moved it.
grep -Fq '#define APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX 3840U' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-509 inc10: потолок страниц не поднят под русскую навигацию" >&2
    exit 1
}
grep -Fq '#define HTTP_RESPONSE_BODY_MAX 3840U' kernel/include/http_response.h || {
    echo "ITEM-509 inc10: транспортный потолок не поднят вместе со страницами" >&2
    exit 1
}
grep -Fq '| help | 3650 | 3650 |' docs/hfoc-management-web-ui.md || {
    echo "ITEM-509 inc10: таблица бюджетов не несёт новый замер help" >&2
    exit 1
}
grep -Fq 'item509_inc10' scripts/check.d/174-item-509-panel-language.sh || {
    echo "ITEM-509 inc10: срез не называет себя" >&2
    exit 1
}
