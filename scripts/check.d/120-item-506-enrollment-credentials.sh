#!/usr/bin/env bash
# ITEM-506 increment 2: contracted slice of the HFOC OS check suite.  Sourced
# in pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The owner wants one account with a login and a password, one command that
# changes it and a local login beside it.  This increment builds the owner the
# console and the panel will call, and the pure conversation they will drive;
# it deliberately does not switch any user interface yet, so the legacy
# secret-only commands keep working on a legacy record:
#
#   * the enrollment owner binds a salt source and refuses provisioning with
#     NO_RANDOM when none is bound;
#   * `provision_credential` validates the login and password with the model,
#     derives the salted verifier, commits it as the newest record, publishes
#     it into the request gate and is also the explicit migration of a legacy
#     fingerprint record;
#   * `authenticate_credential` reads the structured record, verifies the
#     login and password with the model, shares the one five-failure lockout
#     and issues the same bounded session;
#   * a legacy record refuses the credential path as LEGACY_RECORD (the
#     factory placeholder as UNPROVISIONED), and once a structured credential
#     exists the legacy secret-only path refuses as CREDENTIAL_REQUIRED;
#   * `appliance_admin_credentials_prompt` owns the console conversation as a
#     pure state machine: `credentials` asks current password, new login, new
#     password twice; `login` asks login and password; refusals name the rule,
#     a mismatch restarts the password pair, over-long lines are refused and
#     never cut, and cancel/reset zero every secret buffer.

for item506_credential_file in \
    kernel/include/appliance_admin_credentials_prompt.h \
    kernel/src/appliance_admin_credentials_prompt.c \
    tests/appliance-admin-credentials-prompt-host.c; do
    test -f "$item506_credential_file" || {
        echo "ITEM-506: отсутствует файл: $item506_credential_file" >&2
        exit 1
    }
done

# ------------------------------------------------------------- the owner ------
for item506_status in \
    'APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_LOGIN = 13' \
    'APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_WEAK_PASSWORD = 14' \
    'APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_LEGACY_RECORD = 15' \
    'APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_CREDENTIAL_REQUIRED = 16' \
    'APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NO_RANDOM = 17'; do
    grep -Fq "$item506_status" \
        kernel/include/appliance_production_admin_enrollment.h || {
        echo "ITEM-506: отказ не назван: $item506_status" >&2
        exit 1
    }
done
grep -Fq 'appliance_production_admin_enrollment_random_fn' \
    kernel/include/appliance_production_admin_enrollment.h || {
    echo "ITEM-506: соляной seam не объявлен" >&2
    exit 1
}
for item506_api in \
    'appliance_production_admin_enrollment_set_random' \
    'appliance_production_admin_enrollment_provision_credential' \
    'appliance_production_admin_enrollment_authenticate_credential' \
    'appliance_production_admin_enrollment_credential_set'; do
    grep -Fq "$item506_api" \
        kernel/include/appliance_production_admin_enrollment.h \
        kernel/src/appliance_production_admin_enrollment.c || {
        echo "ITEM-506: API не найден: $item506_api" >&2
        exit 1
    }
done
grep -Fq 'appliance_persistent_secret_config_admin_credential(' \
    kernel/src/appliance_production_admin_enrollment.c || {
    echo "ITEM-506: владелец не читает структурную запись" >&2
    exit 1
}
grep -Fq 'appliance_persistent_secret_config_commit_admin_credential(' \
    kernel/src/appliance_production_admin_enrollment.c || {
    echo "ITEM-506: владелец не пишет структурную запись" >&2
    exit 1
}
grep -Fq 'APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_DEFAULT' \
    kernel/src/appliance_production_admin_enrollment.c || {
    echo "ITEM-506: провижининг не берёт число итераций модели" >&2
    exit 1
}
# One failure counter and one session tail for both entries.
grep -Fq 'admin_enrollment_record_failure' \
    kernel/src/appliance_production_admin_enrollment.c || {
    echo "ITEM-506: общий счётчик неудач не найден" >&2
    exit 1
}
grep -Fq 'admin_enrollment_finish_session' \
    kernel/src/appliance_production_admin_enrollment.c || {
    echo "ITEM-506: общий хвост сессии не найден" >&2
    exit 1
}
# Both legacy entry points refuse once the account is structured.  The enum
# name alone is not enough: the status text names it twice, so a count of
# three would stay green with only one of the two refusals left.  Four is the
# floor (two refusals plus the two status-text lines), and the guard call is
# pinned at all four call sites (refresh, both legacy entry points, the
# public query), so a defined-but-uncalled regression fails here too.
item506_required_count=$(grep -c \
    'APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_CREDENTIAL_REQUIRED' \
    kernel/src/appliance_production_admin_enrollment.c)
if [[ $item506_required_count -lt 4 ]]; then
    echo "ITEM-506: прежний путь не отказывается от структурной записи" >&2
    exit 1
fi
item506_guard_count=$(grep -c \
    'admin_enrollment_credential_set_internal(enrollment)' \
    kernel/src/appliance_production_admin_enrollment.c)
if [[ $item506_guard_count -lt 4 ]]; then
    echo "ITEM-506: проверка структурной записи вызывается не на всех путях" >&2
    exit 1
fi
grep -Fq 'APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NO_RANDOM' \
    kernel/src/appliance_production_admin_enrollment.c || {
    echo "ITEM-506: отказ без соляного источника не найден" >&2
    exit 1
}

# ------------------------------------------------------- the conversation -----
for item506_prompt_api in \
    'appliance_admin_credentials_prompt_start' \
    'appliance_admin_credentials_prompt_submit' \
    'appliance_admin_credentials_prompt_cancel' \
    'appliance_admin_credentials_prompt_reset'; do
    grep -Fq "$item506_prompt_api" \
        kernel/include/appliance_admin_credentials_prompt.h \
        kernel/src/appliance_admin_credentials_prompt.c || {
        echo "ITEM-506: шаг разговора не найден: $item506_prompt_api" >&2
        exit 1
    }
done
for item506_step in \
    'APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_CURRENT' \
    'APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_LOGIN' \
    'APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_PASSWORD' \
    'APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_CONFIRM' \
    'APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_LOGIN_NAME' \
    'APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_LOGIN_PASSWORD'; do
    grep -Fq "$item506_step" \
        kernel/include/appliance_admin_credentials_prompt.h || {
        echo "ITEM-506: шаг разговора не назван: $item506_step" >&2
        exit 1
    }
done
grep -A 1 '#define APPLIANCE_ADMIN_CREDENTIALS_PROMPT_LINE_MAX' \
    kernel/include/appliance_admin_credentials_prompt.h | \
    grep -Fq 'APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX' || {
    echo "ITEM-506: граница строки разговора не равна границе пароля" >&2
    exit 1
}
# ITEM-509 increment 20: the questions and refusals moved to the one catalog, so
# the pins follow them - the key in the conversation module and the English
# value verbatim in the catalog.
for item506_question in \
    '"credential.prompt.current"' \
    '"credential.prompt.new_login"' \
    '"credential.prompt.new_password"' \
    '"credential.prompt.confirm"' \
    '"credential.prompt.login"' \
    '"credential.prompt.password"'; do
    grep -Fq "$item506_question" \
        kernel/src/appliance_admin_credentials_prompt.c || {
        echo "ITEM-506: вопрос не читает каталог: $item506_question" >&2
        exit 1
    }
done
for item506_question in \
    '"Current password: "' \
    '"New login: "' \
    '"New password: "' \
    '"Repeat new password: "' \
    '"Login: "' \
    '"Password: "'; do
    grep -Fq "$item506_question" \
        kernel/src/appliance_language.c || {
        echo "ITEM-506: каталог потерял вопрос: $item506_question" >&2
        exit 1
    }
done
for item506_refusal in \
    '"credential.refusal.too_long"' \
    '"credential.refusal.current_empty"' \
    '"credential.refusal.login_policy"' \
    '"credential.refusal.password_policy"' \
    '"credential.refusal.weak_password"' \
    '"credential.refusal.mismatch"' \
    '"credential.refusal.password_empty"' \
    '"credential.cancelled"'; do
    grep -Fq "$item506_refusal" \
        kernel/src/appliance_admin_credentials_prompt.c || {
        echo "ITEM-506: отказ не читает каталог: $item506_refusal" >&2
        exit 1
    }
done
for item506_refusal in \
    'The current password cannot be empty. ' \
    'Login must be 1-32 characters' \
    'Password must be 12-128 printable characters' \
    'That password is in the weak list. ' \
    'The passwords do not match. ' \
    'That line is longer than the policy allows. ' \
    'Credential entry cancelled.'; do
    grep -Fq "$item506_refusal" \
        kernel/src/appliance_language.c || {
        echo "ITEM-506: каталог потерял отказ: $item506_refusal" >&2
        exit 1
    }
done

# --------------------------------------------------------------- the tests -----
for item506_owner_claim in \
    'provisioning without a salt source is refused' \
    'a weak password is refused by name' \
    'the salt comes from the bound source' \
    'the right login and password open a session' \
    'a wrong login is denied' \
    'the fifth failure locks the account' \
    'a locked account refuses even the right password' \
    'the legacy authenticate refuses a structured credential' \
    'a legacy record refuses the credential path by name' \
    'the legacy path still serves the legacy record' \
    'an explicit provisioning migrates the legacy record' \
    'the legacy secret is refused after the migration' \
    'a source that fills nothing is refused as NO_RANDOM' \
    'every salt byte comes from the bound source' \
    'an unbound source refuses the change' \
    'the lockout survives the legacy trip' \
    'the old legacy secret is denied after the migration'; do
    grep -Fq "$item506_owner_claim" \
        tests/appliance-production-admin-enrollment-host.c || {
        echo "ITEM-506: проба владельца не утверждает: $item506_owner_claim" >&2
        exit 1
    }
done
for item506_prompt_claim in \
    'the first question asks the current password' \
    'an empty current password is refused and re-asked' \
    'a short password is refused with the rule' \
    'a weak password is refused by name' \
    'a mismatch restarts the password pair and drops the first value' \
    'the matching pair submits the change' \
    'reset zeroes every secret buffer' \
    'a non-empty password submits the login' \
    'an over-long line is refused, not truncated' \
    'a 128-character password is accepted' \
    'the terminal action text never repeats the password' \
    'reset zeroes the secret bytes, not only the lengths' \
    'a mismatch refusal repeats no secret' \
    'a one-character login is accepted' \
    'a 33-character login is refused and re-asked'; do
    grep -Fq "$item506_prompt_claim" \
        tests/appliance-admin-credentials-prompt-host.c || {
        echo "ITEM-506: проба разговора не утверждает: $item506_prompt_claim" >&2
        exit 1
    }
done
grep -Fq 'test-appliance-admin-credentials-prompt-host' Makefile || {
    echo "ITEM-506: цель пробы разговора не зарегистрирована" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-appliance-admin-credentials-prompt-host' Makefile || {
    echo "ITEM-506: проба разговора не входит в test-host-suite" >&2
    exit 1
}
# ITEM-506 increment 3 switched the console; the transitional pin that required
# the legacy commands to survive is replaced by the requirement that they are
# gone and that the one account vocabulary is present.
if grep -Fq 'ADMIN_PROVISION_KEYWORD' kernel/src/console.c; then
    echo "ITEM-506: удалённое имя admin-provision всё ещё в консоли" >&2
    exit 1
fi
grep -Fq 'console_credentials_command_classify(' kernel/src/console.c || {
    echo "ITEM-506: консоль не использует словарь учётной записи" >&2
    exit 1
}

# ------------------------------------------------------------- the documents ---
QUEUE_DOC=docs/superpowers/plans/2026-09-11-appliance-productization-queue.md
grep -Fq 'Инкремент 2 (влит) — владелец учётной записи' "$QUEUE_DOC" || {
    echo "ITEM-506: очередь не описывает второй инкремент" >&2
    exit 1
}
grep -Fq 'ITEM-506 increment 2' docs/ARCHITECTURE.md || {
    echo "ITEM-506: ARCHITECTURE не описывает владельца учётной записи" >&2
    exit 1
}
grep -Fq 'ITEM-506 increment 2' docs/ROADMAP.md || {
    echo "ITEM-506: ROADMAP не называет второй инкремент" >&2
    exit 1
}
