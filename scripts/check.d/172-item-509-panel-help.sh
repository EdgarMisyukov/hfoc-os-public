#!/usr/bin/env bash
# ITEM-509 increment 8: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-509 increment 8 is 509.6: the second help renderer.  One content, two
# renderers - the console prints the catalog, the panel renders the same four
# fields as HTML, and the panel keeps no second topic table.  The Help section
# is the thirteenth entry in the one section model (name, path, owner, state,
# budget move together - the ITEM-499 trap), its index is `/panel/help` and its
# topic pages are `/panel/help/<id>` for the catalog's own ids.  The claims
# pinned here:
#
#   * the section model gained exactly one section and every parallel table
#     moved with it, so the menu, the router and the budgets cannot disagree;
#   * the renderer reads the catalog through `appliance_help_topic_*` only, and
#     a missing translation or an unknown id is a named refusal on the page,
#     never a silent English fallback and never an empty document;
#   * the language comes from the same system record the System page publishes,
#     so a machine whose record did not load says so;
#   * the wire body cap moved with the measured Help pages (3179 bytes) and the
#     JSON API documents keep their own smaller 2048-byte bound;
#   * the host proofs walk every topic in both renderers and every language,
#     and the QEMU document matrices know the new navigation.
for item509_inc8_file in \
    kernel/include/appliance_management_web_ui.h \
    kernel/src/appliance_management_web_ui.c \
    kernel/src/appliance_management_panel.c \
    kernel/src/appliance_management_authorization.c \
    kernel/include/http_response.h \
    tests/appliance-management-web-ui-host.c \
    tests/appliance-management-panel-host.c \
    docs/hfoc-management-web-ui.md \
    docs/hfoc-language-surfaces.md \
    docs/ROADMAP.md \
    docs/ARCHITECTURE.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc8_file ]] || {
        echo "ITEM-509 inc8: отсутствует $item509_inc8_file" >&2
        exit 1
    }
done

# 1. The one section model: the thirteenth entry and every parallel table.
for item509_inc8_section in \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_SECTION_COUNT 13U' \
    'APPLIANCE_MANAGEMENT_WEB_UI_SECTION_HELP = 12,' \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_HELP_PATH "/panel/help"' \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_HELP_PATH_LENGTH 11U' \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_HELP_TOPIC_PREFIX "/panel/help/"' \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_HELP_TOPIC_PREFIX_LENGTH 12U' \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_HELP_TOPIC_MAX 31U' \
    'APPLIANCE_MANAGEMENT_WEB_UI_PAGE_HELP = 13,' \
    'APPLIANCE_MANAGEMENT_WEB_UI_PAGE_COUNT = 17,' \
    'struct appliance_management_web_ui_help {'; do
    grep -Fq -- "$item509_inc8_section" \
        kernel/include/appliance_management_web_ui.h || {
        echo "ITEM-509 inc8: модель разделов не несёт: $item509_inc8_section" >&2
        exit 1
    }
done
# ITEM-509 increment 10: the module's name table became the catalog key table.
grep -Fq '"panel.section.help",' kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-509 inc8: таблица имён не несёт Help" >&2
    exit 1
}
grep -Fq '{"panel.section.help", "Help", "Справка"}' \
    kernel/src/appliance_language.c || {
    echo "ITEM-509 inc8: каталог не несёт имя раздела Help" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_WEB_UI_HELP_PATH,' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-509 inc8: таблица путей не несёт /panel/help" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_HELP,' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-509 inc8: таблица бюджетов не несёт Help" >&2
    exit 1
}
grep -Fq 'Certificates Help</nav>' kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-509 inc8: статический индекс меню не несёт Help" >&2
    exit 1
}

# 2. The renderer: one catalog accessor set, the named refusals, and the four
#    fields of a topic page.
for item509_inc8_render in \
    'static void appliance_management_web_ui_write_help(' \
    'static void appliance_management_web_ui_write_help_block(' \
    'appliance_management_web_ui_write_help_topic_page(' \
    'appliance_help_topic_find(' \
    'appliance_help_topic_string(' \
    '"NO_SUCH_HELP_TOPIC"' \
    '"CATALOG_INCOMPLETE"' \
    'appliance_language_parse('; do
    grep -Fq -- "$item509_inc8_render" \
        kernel/src/appliance_management_web_ui.c || {
        echo "ITEM-509 inc8: рендерер панели не несёт: $item509_inc8_render" >&2
        exit 1
    }
done
grep -Fq 'APPLIANCE_HELP_FIELD_RELATED' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-509 inc8: страница темы не печатает related" >&2
    exit 1
}
grep -Fq 'appliance_management_web_ui_write_help_block(&buffer, help);' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-509 inc8: Dashboard не несёт блок помощи" >&2
    exit 1
}

# 3. The panel dispatch: the topic page resolves to the Help section, and the
#    language travels from the request's effective language.  ITEM-513 changed
#    the source of that language - the browser's cookie, then the record, then
#    the catalog default - and kept this refusal for the one case where none of
#    the two declarations exists; a valid cookie now counts as a declaration.
for item509_inc8_panel in \
    'static bool appliance_management_panel_help_topic(' \
    'APPLIANCE_MANAGEMENT_WEB_UI_HELP_TOPIC_PREFIX_LENGTH' \
    'help.published = language_source;' \
    'language_source ? appliance_language_code(language) : NULL;' \
    '"SYSTEM_LANGUAGE_UNAVAILABLE"'; do
    grep -Fq -- "$item509_inc8_panel" \
        kernel/src/appliance_management_panel.c || {
        echo "ITEM-509 inc8: диспетчер панели не несёт: $item509_inc8_panel" >&2
        exit 1
    }
done

# 4. Authorization: the Help index and its topic pages are the same panel
#    route; nothing else under /panel became reachable.
for item509_inc8_auth in \
    'APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_HELP_PATH "/panel/help"' \
    'appliance_management_authorization_panel_help_topic(' \
    'return appliance_management_authorization_panel_help_topic('; do
    grep -Fq -- "$item509_inc8_auth" \
        kernel/include/appliance_management_authorization.h \
        kernel/src/appliance_management_authorization.c || {
        echo "ITEM-509 inc8: авторизация не несёт: $item509_inc8_auth" >&2
        exit 1
    }
done

# 5. The measured ceiling: the Help budget, the raised wire body cap and the
#    API bound that deliberately did not grow with it.  ITEM-509 increment 10
#    consciously moved all three measured values again (3350, 3584), because the
#    Help page now carries the Russian navigation, and increment 11 moved the
#    Help budget once more (3460) when the read pages began serving Russian;
#    increment 25 re-pinned it at 3481 when the Help index sentence and the way
#    back were translated; the API bound still stays.
grep -Fq '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_HELP 3650U' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-509 inc8: бюджет Help не пере-измерен" >&2
    exit 1
}
grep -Fq '| help | 3650 | 3650 |' docs/hfoc-management-web-ui.md || {
    echo "ITEM-509 inc8: документ не называет измеренный бюджет Help" >&2
    exit 1
}
grep -Fq '#define HTTP_RESPONSE_BODY_MAX 3840U' \
    kernel/include/http_response.h || {
    echo "ITEM-509 inc8: потолок тела ответа не поднят" >&2
    exit 1
}
grep -Fq '#define HTTP_RESPONSE_API_BODY_MAX 2048U' \
    kernel/include/http_response.h || {
    echo "ITEM-509 inc8: у JSON-API не осталось своего потолка" >&2
    exit 1
}

# 6. The proofs: every topic on the panel index and on its own page, and the
#    panel dispatch proof.
grep -Fq 'static void test_help_page(void)' tests/appliance-management-web-ui-host.c || {
    echo "ITEM-509 inc8: host-проба панели не несёт Help" >&2
    exit 1
}
grep -Fq 'test_help_page();' tests/appliance-management-web-ui-host.c || {
    echo "ITEM-509 inc8: проверка Help не вызвана" >&2
    exit 1
}
grep -Fq 'the Help topic page carries every body line' \
    tests/appliance-management-web-ui-host.c || {
    echo "ITEM-509 inc8: проба не требует тело темы" >&2
    exit 1
}
grep -Fq 'management panel resolves the Help index and its topics' \
    tests/appliance-management-panel-host.c || {
    echo "ITEM-509 inc8: проба панели не разбирает темы Help" >&2
    exit 1
}

# 7. The live document matrices know the new navigation.
grep -Fq 'Help</nav>' scripts/test-qemu-https-response-matrix.sh || {
    echo "ITEM-509 inc8: QEMU-матрица входа не знает Help" >&2
    exit 1
}
grep -Fq '"Account", "Certificates", "Help")' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-509 inc8: QEMU-проба HTTPS не знает раздел Help" >&2
    exit 1
}
grep -Fq '    "/panel/help",' scripts/test-qemu-https-response-matrix.sh || {
    echo "ITEM-509 inc8: QEMU-матрица меню не знает /panel/help" >&2
    exit 1
}

# 8. The documents record the increment and close 509.6.
for item509_inc8_doc in docs/ROADMAP.md docs/ARCHITECTURE.md \
    docs/hfoc-language-surfaces.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    grep -Fq 'ITEM-509 increment 8' "$item509_inc8_doc" || {
        echo "ITEM-509 inc8: $item509_inc8_doc не называет инкремент" >&2
        exit 1
    }
done
