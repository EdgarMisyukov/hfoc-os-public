#!/usr/bin/env bash
# ITEM-445: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The panel opens in the laptop's browser: one form, one HttpOnly cookie.
#
# Until this item the panel was reachable only by a client that could send
# `Authorization: Bearer <token>`, and the token was issued by the local
# administration console, so a browser could not sign in at all.  The owner's
# decision (2026-09-18, option 1) keeps the panel script-free and adds exactly
# two things: a form that posts the administrator secret to the same origin,
# and one session cookie that the section pages accept next to the bearer
# header.  The claims pinned here are the ones that make that safe:
#
#   * the cookie the panel sets is `HttpOnly`, `Secure`, `SameSite=Strict`,
#     `Path=/` and short lived, and the sign-out cookie is the same cookie with
#     an empty value and `Max-Age=0`;
#   * a form body that cannot be decoded honestly, or that is missing either
#     declared field, is refused rather than guessed at;
#   * a cookie value that is not exactly 64 hex digits, or two session cookies
#     in one header, is refused rather than resolved;
#   * the content security policy allows `form-action 'self'` and still carries
#     no `script-src` and no external resource;
#   * the host proof drives the real kernel module and is registered in the
#     canonical host suite, so a suite that loses it fails.
for panel_login_file in \
    kernel/include/appliance_panel_login.h \
    kernel/src/appliance_panel_login.c \
    tests/panel-login-form-host.c \
    Makefile \
    tests/test-host-suite-manifest.sh; do
    [[ -f $panel_login_file ]] || {
        echo "ITEM-445: отсутствует $panel_login_file" >&2
        exit 1
    }
done

# The cookie is one named value with every protective attribute spelled out in
# the one place that writes it.
grep -Fq '#define APPLIANCE_PANEL_LOGIN_COOKIE_NAME "hfoc_session"' \
    kernel/include/appliance_panel_login.h || {
    echo "ITEM-445: cookie не назван" >&2
    exit 1
}
grep -Fq '"; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age="' \
    kernel/src/appliance_panel_login.c || {
    echo "ITEM-445: cookie не несёт HttpOnly/Secure/SameSite/Path/Max-Age" >&2
    exit 1
}
# Sign-out expires the cookie: the empty value and Max-Age=0 come from the one
# writer, so the clear path cannot drift from the set path.
grep -Fq 'return appliance_panel_login_cookie_write(' \
    kernel/src/appliance_panel_login.c || {
    echo "ITEM-445: выход не пишет cookie" >&2
    exit 1
}
grep -Fq 'NULL, 0U, 0U, output, output_capacity, written_out);' \
    kernel/src/appliance_panel_login.c || {
    echo "ITEM-445: выход не обнуляет значение cookie" >&2
    exit 1
}
# A value the reader cannot trust is refused, not resolved: the length check is
# the 64 hex digits of a token, and a second session cookie is MALFORMED.
grep -Fq 'value_length != APPLIANCE_PANEL_LOGIN_COOKIE_VALUE_LENGTH' \
    kernel/src/appliance_panel_login.c || {
    echo "ITEM-445: cookie не проверяется по длине" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PANEL_LOGIN_COOKIE_NAME_LENGTH)) {' \
    kernel/src/appliance_panel_login.c || {
    echo "ITEM-445: cookie не ищется по имени" >&2
    exit 1
}
# The form is the declared content type, and both declared fields are required.
grep -Fq '#define APPLIANCE_PANEL_LOGIN_CONTENT_TYPE' \
    kernel/include/appliance_panel_login.h || {
    echo "ITEM-445: форма не объявляет content type" >&2
    exit 1
}
grep -Fq '"application/x-www-form-urlencoded"' \
    kernel/include/appliance_panel_login.h || {
    echo "ITEM-445: форма не объявляет url-encoded" >&2
    exit 1
}
grep -Fq 'return APPLIANCE_PANEL_LOGIN_MISSING_FIELD;' \
    kernel/src/appliance_panel_login.c || {
    echo "ITEM-445: неполная форма не отвергается" >&2
    exit 1
}
# The secret does not outlive the request.
grep -Fq 'void appliance_panel_login_form_zero(struct appliance_panel_login_form *form)' \
    kernel/src/appliance_panel_login.c || {
    echo "ITEM-445: разобранная форма не обнуляется" >&2
    exit 1
}
# The sign-in policy allows the form to post to its own origin and still
# refuses a script; a policy that kept `form-action 'none'` fails the proof.
grep -Fq "\"form-action 'self'\"" kernel/src/appliance_panel_login.c || {
    echo "ITEM-445: CSP не разрешает форму на свой origin" >&2
    exit 1
}
grep -Fq "\"script-src\"" kernel/src/appliance_panel_login.c || {
    echo "ITEM-445: CSP не отвергает script-src" >&2
    exit 1
}

# The proof drives the kernel module rather than a copy of it, and it is
# registered where the canonical host suite reads its targets.
grep -Fq 'kernel/src/appliance_panel_login.c' Makefile || {
    echo "ITEM-445: проба не линкует модуль панели" >&2
    exit 1
}
# shellcheck disable=SC2016  # the compared text is literal, not expanded
grep -Fq '$(MAKE) test-panel-login-host' Makefile || {
    echo "ITEM-445: проба не зарегистрирована в host suite" >&2
    exit 1
}
grep -Fq 'test-panel-login-host; do' tests/test-host-suite-manifest.sh || {
    echo "ITEM-445: проба не закреплена в манифесте" >&2
    exit 1
}
# The proof has to be able to fail: the drill asserts the attributes, the
# refusals and the absence of the secret in what the module produces.
for panel_login_claim in \
    'the cookie is HttpOnly' \
    'the cookie is Secure' \
    'the cookie is SameSite=Strict' \
    'sign-out expires the cookie immediately' \
    'a short cookie value is refused' \
    'two session cookies are refused, not resolved by guessing' \
    'the secret never reaches the cookie' \
    'the parse buffer is zeroed after the proof is derived'; do
    grep -Fq "$panel_login_claim" tests/panel-login-form-host.c || {
        echo "ITEM-445: проба не утверждает: $panel_login_claim" >&2
        exit 1
    }
done

# ITEM-445 step 2: the form is served, the body reaches the panel, the answer
# carries the cookie, and the form consults the one credential owner the
# appliance already has.
for panel_browser_symbol in \
    appliance_management_panel_set_credential \
    appliance_management_panel_sign_in_count \
    appliance_management_panel_sign_out_count \
    APPLIANCE_MANAGEMENT_PANEL_TARGET_SIGN_IN \
    APPLIANCE_MANAGEMENT_PANEL_TARGET_SIGN_OUT \
    APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_OK \
    APPLIANCE_MANAGEMENT_PANEL_CREDENTIAL_THROTTLED; do
    grep -Fq "$panel_browser_symbol" \
        kernel/include/appliance_management_panel.h \
        kernel/src/appliance_management_panel.c || {
        echo "ITEM-445: панель не публикует $panel_browser_symbol" >&2
        exit 1
    }
done
# The form posts to `/`, signs out at `/panel/logout`, and the panel turns the
# session cookie back into the bearer line the existing gate already reads.
grep -Fq 'appliance_panel_login_form_parse(' \
    kernel/src/appliance_management_panel.c || {
    echo "ITEM-445: форма не разбирается панелью" >&2
    exit 1
}
grep -Fq 'appliance_panel_login_cookie_token(' \
    kernel/src/appliance_management_panel.c || {
    echo "ITEM-445: cookie не читается панелью" >&2
    exit 1
}
grep -Fq 'appliance_panel_login_cookie_set(' \
    kernel/src/appliance_management_panel.c || {
    echo "ITEM-445: вход не выпускает cookie" >&2
    exit 1
}
grep -Fq 'appliance_panel_login_cookie_clear(' \
    kernel/src/appliance_management_panel.c || {
    echo "ITEM-445: выход не гасит cookie" >&2
    exit 1
}
# One credential owner: the panel calls the boundary it was given and derives
# no verifier of its own.
grep -Fq 'panel->credential.authenticate(' \
    kernel/src/appliance_management_panel.c || {
    echo "ITEM-445: панель не зовёт привязанную границу учётных данных" >&2
    exit 1
}
if grep -nE '\b(sha256_|appliance_production_admin_enrollment_)\b' \
        kernel/src/appliance_management_panel.c; then
    echo "ITEM-445: панель выводит verifier вместо привязанной границы" >&2
    exit 1
fi
# The ordinary boot binds the ITEM-401 enrollment to that boundary, so the
# browser adds no second credential path.
grep -Fq 'appliance_production_serving_bind_admin_enrollment' \
    kernel/include/appliance_production_serving.h \
    kernel/src/appliance_production_serving.c \
    kernel/src/kernel.c || {
    echo "ITEM-445: enrollment не привязан к форме входа" >&2
    exit 1
}
# ITEM-619: the same owner through the cooperative job - the begin and finish
# halves are both pinned, so a caller cannot start a check and skip the session
# or failure accounting.
grep -Fq 'appliance_production_admin_enrollment_begin_credential_check' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-445: форма входа не начинает проверку через enrollment" >&2
    exit 1
}
grep -Fq 'appliance_production_admin_enrollment_finish_credential_check' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-445: форма входа не завершает проверку через enrollment" >&2
    exit 1
}
# The body is consumed before the request completes in both receive loops.
grep -Fq 'body_pending' kernel/src/appliance_production_serving.c || {
    echo "ITEM-445: production-приём не копит тело" >&2
    exit 1
}
grep -Fq 'body_pending' kernel/src/virtio_net_frame_qemu.c || {
    echo "ITEM-445: QEMU-приём не копит тело" >&2
    exit 1
}
grep -Fq 'HTTP_FRAMING_BODY_MAX' kernel/include/http_parser.h || {
    echo "ITEM-445: кадрирование не публикует границу тела" >&2
    exit 1
}
# POST exists, and only the two form targets accept it.
grep -Fq 'HTTP_METHOD_POST' kernel/include/http_parser.h || {
    echo "ITEM-445: метод POST не объявлен" >&2
    exit 1
}
grep -Fq 'http_policy_form_target' kernel/src/http_parser.c || {
    echo "ITEM-445: политика не ограничивает POST двумя целями" >&2
    exit 1
}
# The encoder prints the two headers, and the policy allows the one form.
grep -Fq 'http_response_set_cookie_prefix' kernel/src/http_response.c || {
    echo "ITEM-445: кодировщик не печатает Set-Cookie" >&2
    exit 1
}
grep -Fq 'http_response_location_prefix' kernel/src/http_response.c || {
    echo "ITEM-445: кодировщик не печатает Location" >&2
    exit 1
}
grep -Fq 'HTTP_RESPONSE_SET_COOKIE_MAX' kernel/include/http_response.h || {
    echo "ITEM-445: кодировщик не ограничивает Set-Cookie" >&2
    exit 1
}
grep -Fq 'HTTP_RESPONSE_LOCATION_MAX' kernel/include/http_response.h || {
    echo "ITEM-445: кодировщик не ограничивает Location" >&2
    exit 1
}
grep -Fq "form-action 'self'" kernel/include/http_response.h || {
    echo "ITEM-445: CSP не разрешает форму на свой origin" >&2
    exit 1
}
# The page carries the two forms and still no script.
grep -Fq '<form method=\"post\" action=\"/\">' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-445: страница входа не несёт форму" >&2
    exit 1
}
grep -Fq '<form method=\"post\" action=\"/panel/logout\">' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-445: страница не несёт выход" >&2
    exit 1
}
if grep -nE '<(script|iframe|object|embed|link)\b' \
        kernel/src/appliance_management_web_ui.c; then
    echo "ITEM-445: страница входа несёт скрипт или внешний ресурс" >&2
    exit 1
fi
# The host proof drives the real form, the cookie round trip and the refusals.
for panel_browser_claim in \
    'management panel posts the form and answers 303 with a cookie' \
    'management panel sets one HttpOnly Secure SameSite cookie' \
    'management panel accepts the session cookie for a section' \
    'management panel refuses a short cookie without guessing' \
    'management panel refuses two session cookies in one header' \
    'management panel denies a wrong secret without a cookie' \
    'management panel refuses a partial form' \
    'management panel sign-out expires the cookie immediately' \
    'management panel publishes the existing lockout on the form' \
    'management panel leaves the public root document to the router' \
    'management panel answers UNAVAILABLE without a credential owner'; do
    grep -Fq "$panel_browser_claim" \
        tests/appliance-management-panel-host.c || {
        echo "ITEM-445: проба панели не утверждает: $panel_browser_claim" >&2
        exit 1
    }
done
# The wire proof drives the same flow over TLS on the production image.
for panel_browser_proof in \
    'TLS-PROOF browser sign-in: 303 with one HttpOnly Secure SameSite cookie' \
    'TLS-PROOF panel section: 200 to the session cookie alone' \
    'TLS-PROOF browser sign-out: 303 with the cookie expired'; do
    grep -Fq "$panel_browser_proof" \
        scripts/test-qemu-production-service-https.sh || {
        echo "ITEM-445: QEMU-проба не утверждает: $panel_browser_proof" >&2
        exit 1
    }
done

# ITEM-454/ITEM-506: the sign-in document explains itself, and the console
# names the one account command.
#
# The owner reached the page in a browser and could not sign in: nothing on it
# said where the account is set, and the console's `help` printed the names in
# upper case while the parser accepted only the lower-case spelling.  ITEM-454
# fixed the case rule; ITEM-506 replaced the account and the commands.  This
# block pins the current state: the one sentence the page carries, the measured
# asset budget that moved with it, every recorded copy of the served document
# and its Content-Length, and the one-command vocabulary the console uses.
panel_login_page_sentence='<p>Set the account on the machine console with <code>credentials</code>, then sign in here with the same login and password.</p>'
# ITEM-509 increment 12 moved the rendered sentence into the catalog: the
# renderer names `panel.login.set_account_prefix` and the catalog carries the
# English words, so the C file keeps exactly the one copy the static `/` shell
# prints.  The whole-sentence pin below still runs against the QEMU copies,
# which hold it in one byte string each.
page_copies=$(grep -Fc -- '<p>Set the account on the machine console with' \
    kernel/src/appliance_management_web_ui.c || true)
[[ $page_copies == 1 ]] || {
    echo "ITEM-506: страница входа несёт строку об учётной записи $page_copies раз, а не один" >&2
    exit 1
}
grep -Fq -- '"panel.login.set_account_prefix"' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-506: страница входа не называет ключ каталога" >&2
    exit 1
}
grep -Fq -- 'Set the account on the machine console with ' \
    kernel/src/appliance_language.c || {
    echo "ITEM-506: каталог не несёт английскую строку об учётной записи" >&2
    exit 1
}
for panel_login_body in \
    kernel/include/appliance_management_web_ui.h \
    tests/appliance-management-web-ui-host.c \
    scripts/test-qemu-entropy-fail-closed-matrix.sh \
    scripts/test-qemu-https-client.sh \
    scripts/test-qemu-https-exhaustion-matrix.sh \
    scripts/test-qemu-https-malformed-request-matrix.sh \
    scripts/test-qemu-https-response-matrix.sh \
    scripts/test-qemu-https-restart-soak.sh \
    scripts/test-qemu-https-service.sh \
    scripts/test-qemu-https-size-boundary-matrix.sh \
    scripts/test-qemu-management-plane-acceptance.sh \
    scripts/check.d/04-https-peer-failure-throttle.sh \
    docs/hfoc-management-web-ui.md \
    tests/https-handler-host.c \
    tests/console-host.c \
    kernel/include/console.h \
    kernel/src/console.c; do
    [[ -f $panel_login_body ]] || {
        echo "ITEM-506: отсутствует $panel_login_body" >&2
        exit 1
    }
done
grep -Fq -- '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_LOGIN 2461U' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-506: бюджет страницы входа не равен измеренному" >&2
    exit 1
}
grep -Fq -- '| login | 2461 | 2461 |' docs/hfoc-management-web-ui.md || {
    echo "ITEM-506: таблица бюджетов не называет измеренную страницу входа" >&2
    exit 1
}
grep -Fq -- 'ITEM-506' docs/hfoc-management-web-ui.md || {
    echo "ITEM-506: документ панели не описывает пункт" >&2
    exit 1
}
grep -Fq -- 'Content-Length: 1611' \
    scripts/check.d/04-https-peer-failure-throttle.sh || {
    echo "ITEM-506: срез 04 не ждёт новую длину страницы" >&2
    exit 1
}
for panel_login_claim in \
    'tells the operator how to set the' \
    'no longer names the removed command' \
    'says the form takes that account'; do
    grep -Fq -- "$panel_login_claim" \
        tests/appliance-management-web-ui-host.c || {
        echo "ITEM-506: host-проба страницы не утверждает: $panel_login_claim" >&2
        exit 1
    }
done
for panel_login_copy in \
    scripts/test-qemu-entropy-fail-closed-matrix.sh \
    scripts/test-qemu-https-client.sh \
    scripts/test-qemu-https-exhaustion-matrix.sh \
    scripts/test-qemu-https-malformed-request-matrix.sh \
    scripts/test-qemu-https-response-matrix.sh \
    scripts/test-qemu-https-restart-soak.sh \
    scripts/test-qemu-https-service.sh \
    scripts/test-qemu-https-size-boundary-matrix.sh \
    scripts/test-qemu-management-plane-acceptance.sh; do
    grep -Fq -- "$panel_login_page_sentence" "$panel_login_copy" || {
        echo "ITEM-506: QEMU-проба не несёт новую страницу входа: $panel_login_copy" >&2
        exit 1
    }
done
for panel_login_length in \
    scripts/test-qemu-https-exhaustion-matrix.sh \
    scripts/test-qemu-https-malformed-request-matrix.sh \
    scripts/test-qemu-https-response-matrix.sh \
    scripts/test-qemu-https-restart-soak.sh \
    scripts/test-qemu-https-size-boundary-matrix.sh; do
    grep -Fq -- 'Content-Length: 1611' "$panel_login_length" || {
        echo "ITEM-506: QEMU-проба не ждёт новую длину страницы: $panel_login_length" >&2
        exit 1
    }
done
for panel_login_console in \
    'bool console_command_is(const char *line, const char *command)' \
    'bool console_command_prefix(const char *line, const char *prefix)' \
    'console_ascii_fold' \
    'console_credentials_command_classify(' \
    'CONSOLE_CREDENTIALS_COMMAND_CHANGE' \
    'CONSOLE_CREDENTIALS_COMMAND_LOGIN' \
    'CONSOLE_CREDENTIALS_COMMAND_RESET_EXECUTE'; do
    grep -Fq -- "$panel_login_console" kernel/src/console.c || {
        echo "ITEM-506: консоль не несёт словарь учётной записи: $panel_login_console" >&2
        exit 1
    }
done
for panel_login_decl in \
    'bool console_command_is(const char *line, const char *command);' \
    'bool console_command_prefix(const char *line, const char *prefix);'; do
    grep -Fq -- "$panel_login_decl" kernel/include/console.h || {
        echo "ITEM-506: заголовок консоли не объявляет: $panel_login_decl" >&2
        exit 1
    }
done
for panel_login_case in \
    'static void test_command_case(void)' \
    'test_command_case();' \
    'console_command_is("CREDENTIALS", "credentials")' \
    'static void test_credentials_command_classify(void)' \
    'the removed admin-* names are not commands any more' \
    'the confirmation line executes and clears the arm'; do
    grep -Fq -- "$panel_login_case" tests/console-host.c || {
        echo "ITEM-506: host-проба не утверждает: $panel_login_case" >&2
        exit 1
    }
done
if grep -Fq -- 'ADMIN_PROVISION_KEYWORD' kernel/src/console.c; then
    echo "ITEM-506: удалённое имя admin-provision всё ещё в консоли" >&2
    exit 1
fi
