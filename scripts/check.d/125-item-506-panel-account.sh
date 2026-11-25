#!/usr/bin/env bash
# ITEM-506 increment 6: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The account changes from the panel, and the previous sessions die.
#
# The one account (login + PBKDF2 verifier) exists and the panel signs in with
# it; this increment makes it changeable from the Account page through the
# ITEM-505 two-phase mutation plane, and pins the owner's invalidation: the
# commit calls the single setter, which republishes the verifier and closes
# every issued session - including the session that confirmed the change.  The
# claims pinned here:
#
#   * the authorization matrix grew to fourteen entries with
#     `ACCOUNT_CREDENTIAL` and command id 15; the panel form and the scriptable
#     target are the same Post-only mutation;
#   * the mutation plane has one more bound executor with stage/commit/
#     rollback; an unbound executor refuses the change as an unsupported
#     target;
#   * the stage proves the current password through the enrollment owner's
#     verify-only entry, validates the new pair with the credential model's own
#     policy and holds the candidate; the commit calls `provision_credential`,
#     which is the only writer and the only invalidation;
#   * the serving record is `[PRODUCTION-CREDENTIAL-SET]` with the login and
#     never the password, and the kernel loop drains it once;
#   * the Account page renders the stored login, the four-field stage form and
#     the confirmation form, posts to `/panel/account`, and its measured budget
#     and docs row move together with the navigation link of the new section;
#   * the HTTPS proof changes the account on its own boot, presents the
#     pre-change cookie and bearer again (both must answer 401) and reboots to
#     prove the new pair signs in and the old one does not.
for item506_inc6_file in \
    kernel/include/appliance_management_authorization.h \
    kernel/src/appliance_management_authorization.c \
    kernel/include/http_parser.h \
    kernel/src/http_parser.c \
    kernel/include/appliance_production_mutation_plane.h \
    kernel/src/appliance_production_mutation_plane.c \
    kernel/include/appliance_production_serving.h \
    kernel/src/appliance_production_serving.c \
    kernel/include/appliance_management_web_ui.h \
    kernel/src/appliance_management_web_ui.c \
    kernel/include/appliance_management_panel.h \
    kernel/src/appliance_management_panel.c \
    kernel/include/appliance_panel_login.h \
    kernel/src/appliance_panel_login.c \
    kernel/src/kernel.c \
    tests/appliance-management-authorization-host.c \
    tests/http-policy-host.c \
    tests/appliance-production-mutation-plane-host.c \
    tests/appliance-production-admin-enrollment-host.c \
    tests/appliance-management-web-ui-host.c \
    tests/appliance-management-panel-host.c \
    tests/panel-login-form-host.c \
    scripts/test-qemu-production-service-https.sh \
    docs/hfoc-management-web-ui.md \
    docs/hfoc-production-admin-enrollment.md \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item506_inc6_file ]] || {
        echo "ITEM-506 inc6: отсутствует $item506_inc6_file" >&2
        exit 1
    }
done

# The mutation, the command id and both targets, POST only.
for item506_inc6_auth in \
    'APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ACCOUNT_CREDENTIAL = 9,' \
    '#define APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_ACCOUNT_MUTATION 15U' \
    '#define APPLIANCE_MANAGEMENT_AUTHORIZATION_ACCOUNT_PATH' \
    '#define APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ACCOUNT_PATH' \
    '"/api/v1/system/account"' \
    '"/panel/account"'; do
    grep -Fq -- "$item506_inc6_auth" \
        kernel/include/appliance_management_authorization.h || {
        echo "ITEM-506 inc6: авторизация не называет смену учётной записи:" \
            "$item506_inc6_auth" >&2
        exit 1
    }
done
for item506_inc6_matrix in \
    'struct authorization_entry entries[17];' \
    'index < 17U' \
    'APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_ACCOUNT_MUTATION,' \
    'return "ACCOUNT_CREDENTIAL";'; do
    grep -Fq -- "$item506_inc6_matrix" \
        kernel/src/appliance_management_authorization.c || {
        echo "ITEM-506 inc6: матрица не выросла на смену учётной записи:" \
            "$item506_inc6_matrix" >&2
        exit 1
    }
done
grep -Fq 'policy.matrix.count == 17U' \
    kernel/src/appliance_management_plane_qemu.c || {
    echo "ITEM-506 inc6: QEMU-фикстура не знает выросшую матрицу" >&2
    exit 1
}
for item506_inc6_policy in \
    'HTTP_POLICY_ACCOUNT_TARGET "/api/v1/system/account"' \
    'HTTP_POLICY_PANEL_ACCOUNT_TARGET "/panel/account"' \
    '{HTTP_POLICY_ACCOUNT_TARGET, HTTP_POLICY_ACCOUNT_TARGET_LENGTH},' \
    '{HTTP_POLICY_PANEL_ACCOUNT_TARGET,'; do
    grep -Fq -- "$item506_inc6_policy" \
        kernel/include/http_parser.h kernel/src/http_parser.c || {
        echo "ITEM-506 inc6: HTTP-политика не знает форму смены:" \
            "$item506_inc6_policy" >&2
        exit 1
    }
done

# The plane: one more executor, bound, refused when unbound, and dispatched in
# all three phases.
for item506_inc6_plane in \
    'APPLIANCE_PRODUCTION_MUTATION_PLANE_ACCOUNT_OK = 0,' \
    'struct appliance_production_mutation_plane_account_executor {' \
    'struct appliance_production_mutation_plane_account_executor account;' \
    'bool account_bound;' \
    'appliance_production_mutation_plane_bind_account(' \
    'appliance_production_mutation_plane_account_ready(' \
    'plane->account.stage(' \
    'plane->account.commit(' \
    'plane->account.rollback('; do
    grep -Fq -- "$item506_inc6_plane" \
        kernel/include/appliance_production_mutation_plane.h \
        kernel/src/appliance_production_mutation_plane.c || {
        echo "ITEM-506 inc6: плоскость мутаций не знает смену учётной записи:" \
            "$item506_inc6_plane" >&2
        exit 1
    }
done

# The serving composition: the verify-only stage, the single-setter commit and
# the record that never carries the password.
for item506_inc6_serving in \
    'appliance_production_serving_account_stage;' \
    'appliance_production_serving_account_commit;' \
    'appliance_production_serving_account_rollback;' \
    'appliance_production_admin_enrollment_verify_credential(' \
    'appliance_production_admin_enrollment_provision_credential(' \
    '[PRODUCTION-CREDENTIAL-SET] source=PANEL login=' \
    'conflict=CHANGED' \
    'appliance_production_serving_account_unchanged(' \
    'appliance_production_serving_take_account_record(' \
    'appliance_production_serving_publish_account(' \
    'appliance_production_serving_handle_account('; do
    grep -Fq -- "$item506_inc6_serving" \
        kernel/src/appliance_production_serving.c || {
        echo "ITEM-506 inc6: композиция не несёт смену учётной записи:" \
            "$item506_inc6_serving" >&2
        exit 1
    }
done
grep -Fq 'APPLIANCE_PRODUCTION_SERVING_ACCOUNT_RECORD_MAX' \
    kernel/include/appliance_production_serving.h || {
    echo "ITEM-506 inc6: запись смены не объявлена" >&2
    exit 1
}
grep -Fq 'log_production_account_set();' kernel/src/kernel.c || {
    echo "ITEM-506 inc6: цикл ядра не забирает запись смены" >&2
    exit 1
}
if grep -Fq 'password=' kernel/src/appliance_production_serving.c; then
    echo "ITEM-506 inc6: запись смены называет пароль" >&2
    exit 1
fi

# The owner-side invalidation is pinned by the host proof, not assumed.
for item506_inc6_host in \
    'test_change_revokes_sessions' \
    'the previous session stops authorizing after the change' \
    'the old password no longer proves the account' \
    'the new pair opens a session' \
    'the change drew a fresh salt instead of reusing the old one' \
    'the raw store carries no copy of the new password'; do
    grep -Fq -- "$item506_inc6_host" \
        tests/appliance-production-admin-enrollment-host.c || {
        echo "ITEM-506 inc6: host-проба не проверяет:" \
            "$item506_inc6_host" >&2
        exit 1
    }
done

# The panel page, its parser, the pending token and the recorded budget.
for item506_inc6_page in \
    'APPLIANCE_MANAGEMENT_WEB_UI_SECTION_ACCOUNT = 10,' \
    'APPLIANCE_MANAGEMENT_WEB_UI_PAGE_ACCOUNT = 11,' \
    'struct appliance_management_web_ui_account {' \
    '"/panel/account",' \
    'action=\"/panel/account\"' \
    'name=\"current\"' \
    'name=\"repeat\"' \
    'Recovery Routing Account Certificates Help</nav>' \
    'APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_ACCOUNT 3073U'; do
    grep -Fq -- "$item506_inc6_page" \
        kernel/include/appliance_management_web_ui.h \
        kernel/src/appliance_management_web_ui.c || {
        echo "ITEM-506 inc6: страница учётной записи не несёт:" \
            "$item506_inc6_page" >&2
        exit 1
    }
done
grep -Fq '| account | 3073 | 3073 |' docs/hfoc-management-web-ui.md || {
    echo "ITEM-506 inc6: измеренный бюджет страницы не записан" >&2
    exit 1
}
for item506_inc6_parser in \
    'appliance_panel_account_form_parse(' \
    'appliance_panel_account_form_zero(' \
    'APPLIANCE_PANEL_ACCOUNT_BODY_MAX 2048U' \
    'APPLIANCE_PANEL_LOGIN_FIELD_MAX + 1U'; do
    grep -Fq -- "$item506_inc6_parser" \
        kernel/include/appliance_panel_login.h \
        kernel/src/appliance_panel_login.c || {
        echo "ITEM-506 inc6: разбор формы смены не несёт:" \
            "$item506_inc6_parser" >&2
            exit 1
        }
done

# The mutation executor has its own host proof and the panel host proves the
# rendered page.
for item506_inc6_plane_host in \
    'test_account_executor' \
    'an unbound credential change is refused' \
    'the executor received the exact four-field body' \
    'a refused change is APPLY_FAILED' \
    'the token confirms nothing on another target' \
    'a new stage discards the candidate it replaces'; do
    grep -Fq -- "$item506_inc6_plane_host" \
        tests/appliance-production-mutation-plane-host.c || {
        echo "ITEM-506 inc6: проба плоскости не проверяет:" \
            "$item506_inc6_plane_host" >&2
        exit 1
    }
done
for item506_inc6_panel_host in \
    'management panel Account page shows the login and both forms' \
    'management panel names a missing account reading'; do
    grep -Fq -- "$item506_inc6_panel_host" \
        tests/appliance-management-panel-host.c || {
        echo "ITEM-506 inc6: панельная проба не проверяет:" \
            "$item506_inc6_panel_host" >&2
        exit 1
    }
done

# The end-to-end proof: its own change boot and its own durability boot, both
# on fresh forwarded ports, and the two pre-change credentials answering 401.
# shellcheck disable=SC2016  # the record pattern is literal source text
for item506_inc6_https in \
    'serving-account-client.py' \
    'TLS-PROOF account change: the pre-change cookie serves the Account page' \
    'TLS-PROOF account change: a wrong current password is refused' \
    'TLS-PROOF account change: the previous cookie is revoked' \
    'TLS-PROOF account change: the previous bearer is revoked' \
    'TLS-PROOF account durability: the old credential is refused after the reboot' \
    'TLS-PROOF account durability: the Account page names the new login' \
    'ACCOUNT_SET_RECORD="[PRODUCTION-CREDENTIAL-SET] source=PANEL login=$ACCOUNT_NEW_LOGIN changed=LOGIN,PASSWORD sessions=REVOKED persisted=OK"' \
    'The new password reached the serial log' \
    'TLS-PROOF account durability: the retired password is refused by name for the new login'; do
    grep -Fq -- "$item506_inc6_https" \
        scripts/test-qemu-production-service-https.sh || {
        echo "ITEM-506 inc6: QEMU-проба сервиса не проверяет:" \
            "$item506_inc6_https" >&2
        exit 1
    }
done
if ! grep -Fq 'SERVING_ACCOUNT_CHANGE_PORT=' \
    scripts/test-qemu-production-service-https.sh ||
    ! grep -Fq 'SERVING_ACCOUNT_DURABILITY_PORT=' \
        scripts/test-qemu-production-service-https.sh; then
    echo "ITEM-506 inc6: у проб смены и durability нет своих портов" >&2
    exit 1
fi

# The operator documentation states the invalidation and the console path.
for item506_inc6_doc in \
    'ITEM-506' \
    'sessions=REVOKED'; do
    grep -Fq -- "$item506_inc6_doc" \
        docs/hfoc-production-admin-enrollment.md || {
        echo "ITEM-506 inc6: документ enrollment не фиксирует:" \
            "$item506_inc6_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-506 increment 6' docs/ARCHITECTURE.md || {
    echo "docs/ARCHITECTURE.md не фиксирует ITEM-506 inc6" >&2
    exit 1
}
grep -Fq 'ITEM-506' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-506" >&2
    exit 1
}
