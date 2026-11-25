#!/usr/bin/env bash
# ITEM-513: contracted slice of the HFOC OS check suite. Sourced in pinned order
#           by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-513: the panel's language is a property of the browser.  The owner's
# decision (2026-10-10 03:05 UTC) keeps the shared `v1:system.language` record
# as the machine's default and adds one per-browser cookie, one nav click and
# no token.  The claims pinned here:
#
#   * the cookie is `hfoc_lang`, read and written by the same module and with
#     the same strict rules as the session cookie; only the two catalog codes
#     are accepted, and an empty, unknown, upper-case or duplicated value is a
#     named refusal, never a guess;
#   * the request's language is resolved once - cookie, then record, then the
#     default `en` - and that one language reaches the navigation, `<html
#     lang>`, the section pages, the licenses page, the ACME control page and
#     the Help renderer;
#   * the nav draws one POST form with two submit buttons (`EN`/`RU`, labels
#     from the catalog, values from `appliance_language_code()`) that submits
#     exactly one field to `/panel/language`; the target is a panel target
#     authorized through the same `PANEL` route and admitted to the HTTP body
#     framing, and it answers a 303 and one session-scoped cookie without any
#     token or mutation;
#   * the shared record is not written by the switch: no persistent-config
#     setter is called from the panel dispatch, and the QEMU phase asserts the
#     machine's `[PRODUCTION-SYSTEM-LANGUAGE]` record never appears;
#   * the System page draws one stage field per form, so the body a browser
#     submits is the body the stage accepts - the defect the item closes;
#   * the budgets and the ceiling moved with the measured 169-byte nav form,
#     and the host and QEMU proofs exist and are wired.
for item513_file in \
    kernel/include/appliance_panel_login.h \
    kernel/src/appliance_panel_login.c \
    kernel/src/appliance_management_panel.c \
    kernel/src/appliance_management_web_ui.c \
    kernel/include/appliance_management_web_ui.h \
    kernel/src/appliance_language.c \
    kernel/src/appliance_management_authorization.c \
    kernel/include/appliance_management_authorization.h \
    kernel/src/http_parser.c \
    kernel/include/http_parser.h \
    kernel/src/appliance_production_serving.c \
    tests/panel-login-form-host.c \
    tests/appliance-management-web-ui-host.c \
    tests/appliance-management-panel-host.c \
    scripts/test-qemu-production-service-https.sh \
    docs/hfoc-management-web-ui.md \
    docs/hfoc-language-surfaces.md \
    docs/ROADMAP.md \
    docs/ARCHITECTURE.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item513_file ]] || {
        echo "ITEM-513: отсутствует $item513_file" >&2
        exit 1
    }
done

# 1. The cookie: one name, one writer for the value, one reader, and the one
#    field parser.  The two codes come from the catalog, not from a second
#    table.
for item513_cookie in \
    '#define APPLIANCE_PANEL_LANGUAGE_COOKIE_NAME "hfoc_lang"' \
    'appliance_panel_login_language_cookie(' \
    'appliance_panel_login_language_cookie_set(' \
    'appliance_panel_login_language_cookie_clear(' \
    'appliance_panel_login_language_form_parse(' \
    '"; HttpOnly; Secure; SameSite=Strict; Path=/"' \
    '"; Max-Age=0"'; do
    grep -Fq -- "$item513_cookie" \
        kernel/include/appliance_panel_login.h \
        kernel/src/appliance_panel_login.c || {
        echo "ITEM-513: cookie не несёт: $item513_cookie" >&2
        exit 1
    }
done
grep -Fq 'appliance_language_parse(' kernel/src/appliance_panel_login.c || {
    echo "ITEM-513: cookie не разбирается парсером каталога" >&2
    exit 1
}

# 2. The resolved language is one value for every renderer, and a refused
#    cookie is named and cleared rather than guessed at.
for item513_priority in \
    'appliance_panel_login_language_cookie(' \
    'language_cookie_refused_count' \
    'appliance_panel_login_language_cookie_clear(' \
    'session.language = language;' \
    'view.language = language;' \
    'licenses.language = language;' \
    'control.language = language;' \
    'help.published = language_source;' \
    'language_source ? appliance_language_code(language) : NULL;'; do
    grep -Fq -- "$item513_priority" \
        kernel/src/appliance_management_panel.c || {
        echo "ITEM-513: приоритет языка не несёт: $item513_priority" >&2
        exit 1
    }
done
# The switch changes no machine state: the panel never calls the persistent
# record's writer.
if grep -Fq 'appliance_persistent_config_set_language' \
    kernel/src/appliance_management_panel.c; then
    echo "ITEM-513: переключатель пишет системную запись языка" >&2
    exit 1
fi

# 3. The nav: one POST form, two submit buttons, catalog labels, catalog codes.
for item513_nav in \
    'APPLIANCE_MANAGEMENT_WEB_UI_LANGUAGE_PATH "/panel/language"' \
    'appliance_management_web_ui_language_keys[' \
    'appliance_language_code((enum appliance_language)index)' \
    '\" style=\"display:inline\">"'; do
    grep -Fq -- "$item513_nav" \
        kernel/include/appliance_management_web_ui.h \
        kernel/src/appliance_management_web_ui.c || {
        echo "ITEM-513: nav не несёт: $item513_nav" >&2
        exit 1
    }
done
grep -Fq '"<button name=\"language\" value=\"' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-513: кнопка переключателя не подаёт поле language" >&2
    exit 1
}
for item513_label in \
    '{"panel.language.button_en", "EN", "EN"}' \
    '{"panel.language.button_ru", "RU", "RU"}'; do
    grep -Fq -- "$item513_label" kernel/src/appliance_language.c || {
        echo "ITEM-513: каталог не несёт подпись: $item513_label" >&2
        exit 1
    }
done

# 4. The target is a panel target, authorized through the panel route and
#    admitted to the body framing; a body that is not the drawn one is a named
#    refusal and nothing else.
for item513_target in \
    'APPLIANCE_MANAGEMENT_PANEL_TARGET_LANGUAGE' \
    'APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_LANGUAGE_PATH' \
    '#define APPLIANCE_MANAGEMENT_PANEL_SIGN_IN_LOCATION "/panel"' \
    'HTTP_POLICY_PANEL_LANGUAGE_TARGET "/panel/language"'; do
    grep -Fq -- "$item513_target" \
        kernel/src/appliance_management_panel.c \
        kernel/include/appliance_management_panel.h \
        kernel/include/appliance_management_authorization.h \
        kernel/src/appliance_management_authorization.c \
        kernel/include/http_parser.h \
        kernel/src/http_parser.c || {
        echo "ITEM-513: цель переключателя не заведена: $item513_target" >&2
        exit 1
    }
done
for item513_switch in \
    'appliance_management_panel_language_switch(' \
    '"PANEL_LANGUAGE_BAD_VALUE"' \
    'HTTP_STATUS_303_SEE_OTHER' \
    'APPLIANCE_MANAGEMENT_PANEL_SIGN_IN_LOCATION;'; do
    grep -Fq -- "$item513_switch" \
        kernel/src/appliance_management_panel.c || {
        echo "ITEM-513: переключатель не несёт: $item513_switch" >&2
        exit 1
    }
done
# The POST is authenticated with the browser's cookie on the serving path, not
# only the bearer header.
grep -Fq 'ITEM-513: the `Cookie` header is read for every panel request' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-513: POST-панель не читает cookie" >&2
    exit 1
}

# 5. The System page draws one stage field per form: the defect the item closes.
for item513_form in \
    '" <input name=\"hostname\"></label>"' \
    '" <input name=\"language\"></label>"'; do
    grep -Fq -- "$item513_form" \
        kernel/src/appliance_management_web_ui.c || {
        echo "ITEM-513: форма System не разделена: $item513_form" >&2
        exit 1
    }
done

# 6. The measured price: the ceiling and the worst page the budgets carry.
grep -Fq '#define APPLIANCE_MANAGEMENT_WEB_UI_BODY_MAX 3840U' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-513: потолок страниц панели не поднят" >&2
    exit 1
}
grep -Fq '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_HELP 3650U' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-513: бюджет Help не пере-измерен" >&2
    exit 1
}
grep -Fq '#define HTTP_RESPONSE_BODY_MAX 3840U' \
    kernel/include/http_response.h || {
    echo "ITEM-513: потолок тела ответа не поднят" >&2
    exit 1
}
grep -Fq '#define HTTP_RESPONSE_BUFFER_MAX 4512U' \
    kernel/include/http_response.h || {
    echo "ITEM-513: буфер ответа не поднят" >&2
    exit 1
}
grep -Fq '| help | 3650 | 3650 |' docs/hfoc-management-web-ui.md || {
    echo "ITEM-513: документ не называет измеренный бюджет Help" >&2
    exit 1
}

# 7. The host proofs drive the real modules.
for item513_host in \
    'static void test_language_control(void)' \
    'appliance_panel_login_language_cookie(' \
    'appliance_panel_login_language_form_parse('; do
    grep -Fq -- "$item513_host" tests/panel-login-form-host.c || {
        echo "ITEM-513: host-проба cookie не несёт: $item513_host" >&2
        exit 1
    }
done
grep -Fq 'static void test_language_switch(void)' \
    tests/appliance-management-web-ui-host.c || {
    echo "ITEM-513: host-проба nav не несёт переключатель" >&2
    exit 1
}
grep -Fq 'static void test_system_form_bodies(void)' \
    tests/appliance-management-web-ui-host.c || {
    echo "ITEM-513: host-проба не разбирает тела форм System" >&2
    exit 1
}
grep -Fq 'static void test_language_switch_and_cookie(void)' \
    tests/appliance-management-panel-host.c || {
    echo "ITEM-513: host-проба панели не несёт приоритет cookie" >&2
    exit 1
}
grep -Fq 'test_language_switch_and_cookie();' \
    tests/appliance-management-panel-host.c || {
    echo "ITEM-513: проба приоритета cookie не вызвана" >&2
    exit 1
}

# 8. The QEMU proof: a fresh boot, the browser path, and the untouched record.
for item513_qemu in \
    'SERVING_LANGUAGE_PORT=' \
    'serving-language-client.py' \
    'LANGUAGE_TOKEN=$(python3 "$TEST_DIR/admin-enroll.py" login' \
    'TLS-PROOF language: one click answers 303 and one hfoc_lang cookie' \
    'TLS-PROOF language: browser A serves Russian while the record stays en' \
    'The language switch proof wrote the machine language record'; do
    grep -Fq -- "$item513_qemu" \
        scripts/test-qemu-production-service-https.sh || {
        echo "ITEM-513: QEMU-проба не несёт: $item513_qemu" >&2
        exit 1
    }
done

# 9. The item is recorded where the next window reads it.
for item513_doc in docs/ROADMAP.md docs/ARCHITECTURE.md \
    docs/hfoc-management-web-ui.md docs/hfoc-language-surfaces.md; do
    grep -Fq 'ITEM-513' "$item513_doc" || {
        echo "ITEM-513: $item513_doc не называет пункт" >&2
        exit 1
    }
done
# ITEM-514 moved the queue's `## Current item` heading on to its own item, so
# this pin reads ITEM-513's own historical section; the heading shape itself is
# held by `scripts/check.d/52-queue-current-item.sh`.
grep -A 3 '^## Item ITEM-513$' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md |
    grep -Fq 'ITEM-513' || {
    echo "ITEM-513: раздел очереди не называет ITEM-513" >&2
    exit 1
}
