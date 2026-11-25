#!/usr/bin/env bash
# ITEM-509 increment 12: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-509 increment 12 (509.7a-3) makes the panel's forms, its two-phase
# confirmations, the operator certificate import, the licenses page and the
# sign-in document speak the language the machine selected.  The prose comes
# from the one catalog - English verbatim, Russian beside it - while machine
# values (THROTTLED, SIGNED IN, SIGNED OUT, AVAILABLE, DEGRADED, UNAVAILABLE,
# the LICENSE_* refusal codes, field names, form targets and routes) stay
# English.  The served sign-in document is the rendered page; the static `/`
# shell of `https_handler` stays English and that boundary is named here.
for item509_inc12_file in \
    kernel/src/appliance_language.c \
    kernel/src/appliance_management_web_ui.c \
    kernel/include/appliance_management_web_ui.h \
    tests/appliance-management-web-ui-host.c \
    docs/hfoc-management-web-ui.md \
    docs/hfoc-language-surfaces.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc12_file ]] || {
        echo "ITEM-509 inc12: отсутствует $item509_inc12_file" >&2
        exit 1
    }
done

# 1. The form vocabulary is in the catalog, both languages, English verbatim.
for item509_inc12_key in \
    '{"panel.form.stage", "Stage", "Подготовить"}' \
    '{"panel.form.confirm", "Confirm", "Подтвердить"}' \
    '{"panel.form.cancel", "Cancel", "Отмена"}' \
    '{"panel.form.token", "token", "token"}' \
    '{"panel.confirm.title", "Two-phase confirmation",' \
    '{"panel.confirm.none", "No mutation confirmation is pending.",' \
    '{"panel.confirm.token", "Confirmation token: ", "Токен подтверждения: "}' \
    '{"panel.confirm.ms_left", " ms left; single-use.",' \
    '{"panel.security.title", "Import operator certificate",' \
    '{"panel.security.placeholder_certificate", "certificate PEM",' \
    '{"panel.network.mode_static", "Static", "Статический"}' \
    '{"panel.certificates.import", "Import", "Импорт"}' \
    '{"panel.routing.form_title", "Stage a route", "Подготовить маршрут"}' \
    '{"panel.licenses.title", "About and licenses", "О системе и лицензиях"}' \
    '{"panel.licenses.column_component", "Component", "Компонент"}' \
    '{"panel.acme_control.title", "ACME control", "Управление ACME"}' \
    '{"panel.acme_control.enable", "Enable", "Включить"}' \
    '{"panel.login.title", "Management sign-in", "Вход в управление"}' \
    '{"panel.login.sign_out", "Sign out", "Выйти"}' \
    '{"panel.login.session_prefix", "session ", "сессия "}'; do
    grep -Fq "$item509_inc12_key" kernel/src/appliance_language.c || {
        echo "ITEM-509 inc12: каталог не несёт: $item509_inc12_key" >&2
        exit 1
    }
done
# The ACME refusal keeps its exact double space: the English document has to
# stay byte for byte what the renderer printed before the increment.
grep -Fq '"; it does not invent one.  Nothing is enabled and no issuance is"' \
    kernel/src/appliance_language.c || {
    echo "ITEM-509 inc12: каталог потерял двойной пробел отказа ACME" >&2
    exit 1
}

# 2. The renderers read the catalog and the English prose they used to print is
#    gone, so the two cannot drift apart.
for item509_inc12_call in \
    '"panel.confirm.title"' \
    '"panel.form.stage"' \
    '"panel.form.confirm"' \
    '"panel.form.cancel"' \
    '"panel.security.placeholder_certificate"' \
    '"panel.network.mode_static"' \
    '"panel.certificates.import"' \
    '"panel.form.route_add"' \
    '"panel.form.route_remove"' \
    '"panel.form.stage_add"' \
    '"panel.form.stage_remove"' \
    '"panel.licenses.column_notice"' \
    '"panel.acme_control.two_phase_note"' \
    '"panel.login.requirements_prefix"' \
    '"panel.login.api_discovery"'; do
    grep -Fq -- "$item509_inc12_call" \
        kernel/src/appliance_management_web_ui.c || {
        echo "ITEM-509 inc12: рендерер не читает каталог: $item509_inc12_call" >&2
        exit 1
    }
done
for item509_inc12_gone in \
    '<h3>Two-phase confirmation</h3>' \
    '<p>No mutation confirmation is pending.</p>' \
    '<p>Confirmation token: ' \
    '<h3>Import operator certificate</h3>' \
    'placeholder="certificate PEM"' \
    '<option value="static">Static</option>' \
    '<button>Stage</button>' \
    '<h3>Stage a route</h3>' \
    '<h2>About and licenses</h2>' \
    '<h2>ACME control</h2>' \
    '<p>Throttle: ' \
    '<p>mbedTLS election <code>' \
    '<p>Reason: ' \
    '<p>Payload <code>'; do
    if grep -Fq -- "$item509_inc12_gone" \
        kernel/src/appliance_management_web_ui.c; then
        echo "ITEM-509 inc12: английская проза осталась литералом: $item509_inc12_gone" >&2
        exit 1
    fi
done
# The static `/` shell stays English by decision (increment 10): `https_handler`
# serves it by pointer and has no access to the system record, so its two
# strings remain in the file, inside the shell array, and the boundary is
# recorded rather than forgotten.
if ! grep -Fq '"<h2>Management sign-in</h2>\n"' \
        kernel/src/appliance_management_web_ui.c ||
   ! grep -Fq '"<p>Session: none</p>\n"' \
        kernel/src/appliance_management_web_ui.c; then
    echo "ITEM-509 inc12: статическая оболочка входа изменилась" >&2
    exit 1
fi

# 3. Machine values stay English literals in the renderer: the state words the
#    proof markers and the QEMU matrix quote, the refusal codes and the record
#    fields the serving path reads.
for item509_inc12_machine in \
    '"THROTTLED"' \
    '"SIGNED IN"' \
    '"SIGNED OUT"' \
    '"AVAILABLE"' \
    '"DEGRADED"' \
    '"UNAVAILABLE"' \
    '"LICENSE_INVENTORY_UNPUBLISHED"' \
    '"LICENSE_PAYLOAD_INCOMPLETE"' \
    '", applies="' \
    '</code> source='; do
    grep -Fq -- "$item509_inc12_machine" \
        kernel/src/appliance_management_web_ui.c || {
        echo "ITEM-509 inc12: машинное значение потеряно: $item509_inc12_machine" >&2
        exit 1
    }
done

# 4. The language travels as the render's own parameter, never as a hidden
#    global: every rewritten static renderer names the parameter, the dispatcher
#    passes the language it already parsed and the ACME control page passes its
#    own, so a form cannot print one language's prose inside another document.
for item509_inc12_signature in \
    'appliance_management_web_ui_write_confirmation(' \
    'appliance_management_web_ui_write_security_form(' \
    'appliance_management_web_ui_write_security_confirmation(' \
    'appliance_management_web_ui_write_network_form(' \
    'appliance_management_web_ui_write_system_form(' \
    'appliance_management_web_ui_write_account_form(' \
    'appliance_management_web_ui_write_certificates_form(' \
    'appliance_management_web_ui_write_routing_form(' \
    'appliance_management_web_ui_write_mutation_confirmation(' \
    'appliance_management_web_ui_write_acme_control('; do
    grep -Fq -- "$item509_inc12_signature" \
        kernel/src/appliance_management_web_ui.c || {
        echo "ITEM-509 inc12: форма потеряла язык: $item509_inc12_signature" >&2
        exit 1
    }
done
for item509_inc12_call_site in \
    'appliance_management_web_ui_write_security_form(&buffer, language);' \
    'appliance_management_web_ui_write_network_form(&buffer, language);' \
    'appliance_management_web_ui_write_routing_form(&buffer, language);' \
    'appliance_management_web_ui_write_account_form(&buffer, language);' \
    'appliance_management_web_ui_write_certificates_form(&buffer, language);' \
    'appliance_management_web_ui_write_system_form(&buffer, language);' \
    'appliance_management_web_ui_write_acme_control(&buffer, language, control);' \
    '                &buffer, language, confirmation);'; do
    grep -Fq -- "$item509_inc12_call_site" \
        kernel/src/appliance_management_web_ui.c || {
        echo "ITEM-509 inc12: диспетчер не передаёт язык: $item509_inc12_call_site" >&2
        exit 1
    }
done

# 5. The budget proof measures the signed-in sign-in document and the Russian
#    licenses and ACME pages, and the constants and the recorded table move
#    with the measurement.
grep -Fq 'session.lockout_remaining_ticks = UINT64_MAX;' \
    tests/appliance-management-web-ui-host.c || {
    echo "ITEM-509 inc12: бюджет входа не измерен на вошедшей форме" >&2
    exit 1
}
grep -Fq 'licenses.language = APPLIANCE_LANGUAGE_RU;' \
    tests/appliance-management-web-ui-host.c || {
    echo "ITEM-509 inc12: бюджет licenses не измерен по-русски" >&2
    exit 1
}
grep -Fq 'control.language = APPLIANCE_LANGUAGE_RU;' \
    tests/appliance-management-web-ui-host.c || {
    echo "ITEM-509 inc12: бюджет ACME не измерен по-русски" >&2
    exit 1
}
for item509_inc12_budget in \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_LOGIN 2461U' \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_NETWORK 2922U' \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_SERVICES 2544U' \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_SYSTEM 2917U' \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_SECURITY 3209U' \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_UPDATES 2643U' \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_RECOVERY 2653U' \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_ROUTING 3532U' \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_ACCOUNT 3073U' \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_CERTIFICATES 3123U' \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_LICENSES 2736U' \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_ACME 2797U' \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_MAX 3840U'; do
    grep -Fq -- "$item509_inc12_budget" \
        kernel/include/appliance_management_web_ui.h || {
        echo "ITEM-509 inc12: бюджет не пере-измерен: $item509_inc12_budget" >&2
        exit 1
    }
done
for item509_inc12_row in \
    '| login | 2461 | 2461 |' \
    '| network | 2922 | 2922 |' \
    '| services | 2544 | 2544 |' \
    '| system | 2917 | 2917 |' \
    '| security | 3209 | 3209 |' \
    '| updates | 2643 | 2643 |' \
    '| recovery | 2653 | 2653 |' \
    '| routing | 3532 | 3532 |' \
    '| account | 3073 | 3073 |' \
    '| certificates | 3123 | 3123 |' \
    '| licenses | 2736 | 2736 |' \
    '| acme | 2797 | 2797 |'; do
    grep -Fq -- "$item509_inc12_row" docs/hfoc-management-web-ui.md || {
        echo "ITEM-509 inc12: документ не называет замер: $item509_inc12_row" >&2
        exit 1
    }
done

# 6. The proof renders the Russian form prose and the missing-translation
#    refusal by name, so a page cannot fall back to English silently.
grep -Fq 'ITEM-509: the Russian network form and confirmation come from the ' \
    tests/appliance-management-web-ui-host.c || {
    echo "ITEM-509 inc12: проба не требует русской формы" >&2
    exit 1
}
grep -Fq 'ITEM-509: the Russian sign-in document comes from the catalog' \
    tests/appliance-management-web-ui-host.c || {
    echo "ITEM-509 inc12: проба не требует русского входа" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_WEB_UI_MISSING_TRANSLATION' \
    tests/appliance-management-web-ui-host.c || {
    echo "ITEM-509 inc12: проба не требует именованного отказа" >&2
    exit 1
}

# 7. The boundary is named, not forgotten: the static `/` shell of
#    `https_handler` stays the English sign-in document, and the increment's
#    own record says so, so 509.7d cannot count it as translated.
grep -Fq 'appliance_management_web_ui_login_body' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-509 inc12: статическая оболочка входа исчезла" >&2
    exit 1
}
grep -Fq 'ITEM-509 increment 12' docs/hfoc-language-surfaces.md || {
    echo "ITEM-509 inc12: поверхности языка не называют инкремент" >&2
    exit 1
}
