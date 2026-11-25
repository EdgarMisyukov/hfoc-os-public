#!/usr/bin/env bash
# ITEM-506 increment 3: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The console and the panel sign in with the one account.
#
# Increments 1 and 2 built the model, the record, the owner and the pure
# conversation, and deliberately left the user interfaces on the legacy path.
# This increment switches both of them, which is the moment the old commands
# have to disappear: an appliance whose panel still called the secret-only path
# would lock its operator out the moment a structured account existed.  The
# claims pinned here:
#
#   * one vocabulary (`kernel/include/console_credentials_command.h`):
#     `credentials` opens the change conversation, `login` opens the local
#     session, and the ITEM-465 reset stays as `credentials reset` /
#     `credentials reset confirm`; `admin`, `admin-provision`, `admin-login`
#     and `admin-reset` are removed, and the old classifier header is gone;
#   * the hidden input mode: no echo, no history, no saved draft, no
#     transcript, zeroed after every step, overflow refused as too long (and
#     forgotten as soon as a backspace brings the line back under the cap),
#     and `INPUT_MAX` raised to the policy's 128-character maximum;
#   * the removed `admin-provision <secret>` / `admin-login <secret>`
#     spellings still do not run, but the three places that would keep the
#     password a stale manual puts after them - the durable command record, the
#     `UNKNOWN COMMAND` echo and the input history - redact or skip it;
#   * a legacy fingerprint is not treated as an unset account: the change
#     refuses it and names the explicit reset, at the console and in the owner;
#   * the console verifies the current password through the verify-only entry
#     before it writes anything, and the enrollment owner shares one failure
#     counter and one lockout between the verify and the session-issuing entry;
#   * ordinary boot binds the hardware entropy owner as the salt source, so a
#     change on a fresh appliance derives a real salt;
#   * the panel passes the parsed `username` with the password and the serving
#     adapter calls `authenticate_credential()`;
#   * the documents name the one command and list the removed ones, and the
#     measured login budget (1593 bytes) moved with its sentence.
for item506_inc3_file in \
    kernel/include/console_credentials_command.h \
    kernel/include/console.h \
    kernel/src/console.c \
    kernel/include/appliance_admin_credentials_prompt.h \
    kernel/src/appliance_admin_credentials_prompt.c \
    kernel/include/appliance_production_admin_enrollment.h \
    kernel/src/appliance_production_admin_enrollment.c \
    kernel/include/appliance_management_panel.h \
    kernel/src/appliance_management_panel.c \
    kernel/src/appliance_production_serving.c \
    kernel/src/kernel.c \
    tests/console-host.c \
    tests/appliance-production-admin-enrollment-host.c \
    tests/appliance-management-panel-host.c \
    docs/hfoc-appliance-boot-console.md \
    docs/hfoc-production-admin-enrollment.md \
    docs/hfoc-hardware-acceptance.md \
    docs/hfoc-management-web-ui.md; do
    [[ -f $item506_inc3_file ]] || {
        echo "ITEM-506 inc3: отсутствует $item506_inc3_file" >&2
        exit 1
    }
done
[[ ! -f kernel/include/admin_reset_command.h ]] || {
    echo "ITEM-506 inc3: удалённый заголовок admin_reset_command.h всё ещё в дереве" >&2
    exit 1
}
[[ ! -f scripts/check.d/71-admin-reset.sh ]] || {
    echo "ITEM-506 inc3: срез 71 не переименован" >&2
    exit 1
}
[[ -f scripts/check.d/71-credentials-command.sh ]] || {
    echo "ITEM-506 inc3: срез 71 не назван словарём учётной записи" >&2
    exit 1
}

# ---------------------------------------------------------- the vocabulary -----
for item506_inc3_word in \
    '#define CONSOLE_CREDENTIALS_COMMAND_CHANGE_WORD "credentials"' \
    '#define CONSOLE_CREDENTIALS_COMMAND_LOGIN_WORD "login"' \
    '#define CONSOLE_CREDENTIALS_COMMAND_RESET_REQUEST_WORD "credentials reset"' \
    '#define CONSOLE_CREDENTIALS_COMMAND_RESET_CONFIRM_WORD "credentials reset confirm"' \
    'CONSOLE_CREDENTIALS_COMMAND_CHANGE' \
    'CONSOLE_CREDENTIALS_COMMAND_LOGIN' \
    'CONSOLE_CREDENTIALS_COMMAND_RESET_ARM' \
    'CONSOLE_CREDENTIALS_COMMAND_RESET_EXECUTE' \
    'CONSOLE_CREDENTIALS_COMMAND_RESET_UNCONFIRMED' \
    'console_credentials_command_classify(' \
    '*reset_pending = true;' \
    '*reset_pending = false;'; do
    grep -Fq -- "$item506_inc3_word" \
        kernel/include/console_credentials_command.h || {
        echo "ITEM-506 inc3: словарь не содержит:" "$item506_inc3_word" >&2
        exit 1
    }
done
if grep -Eq 'COMMAND_[A-Z_]*WORD "admin' \
        kernel/include/console_credentials_command.h; then
    echo "ITEM-506 inc3: словарь всё ещё называет удалённые команды" >&2
    exit 1
fi

# ------------------------------------------------------------- the console -----
grep -Fq '#define INPUT_MAX 128U' kernel/src/console.c || {
    echo "ITEM-506 inc3: граница строки консоли не равна максимуму пароля" >&2
    exit 1
}
for item506_inc3_console in \
    'console_credentials_command_classify(' \
    'console_admin_credentials_begin(' \
    'console_admin_credentials_enter(' \
    'console_admin_credentials_commit(' \
    'console_admin_credentials_login(' \
    'console_admin_credentials_reset(' \
    'if (admin_credentials_active) {' \
    'input_overflow = true;' \
    'input_overflow = false;' \
    'console_legacy_secret_command(' \
    'console_legacy_secret_redact(' \
    'if (input_length != 0U && !console_legacy_secret_command(input_line)) {' \
    'const char *echoed = console_legacy_secret_redact(input_line);' \
    'appliance_admin_credentials_prompt_submit(' \
    'appliance_admin_credentials_prompt_cancel(' \
    'admin_credentials_binding->verify(' \
    'admin_credentials_binding->provision(' \
    'admin_credentials_binding->authenticate(' \
    'console_admin_credentials_zero_line();' \
    'no verifiable account; this sets the first ' \
    'APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_LEGACY_RECORD' \
    'APPLIANCE_ADMIN_CREDENTIALS_PROMPT_LINE_MAX + 1U'; do
    grep -Fq -- "$item506_inc3_console" kernel/src/console.c || {
        echo "ITEM-506 inc3: консоль не содержит:" "$item506_inc3_console" >&2
        exit 1
    }
done
for item506_inc3_removed in \
    'ADMIN_PROVISION_KEYWORD' \
    'ADMIN_LOGIN_KEYWORD' \
    'console_admin_secret_command' \
    'console_transcript_redact' \
    'cmd_admin(' \
    'ADMIN-PROVISION'; do
    if grep -Fq -- "$item506_inc3_removed" kernel/src/console.c; then
        echo "ITEM-506 inc3: консоль всё ещё несёт удалённое:" \
            "$item506_inc3_removed" >&2
        exit 1
    fi
done
for item506_inc3_binding in \
    'struct console_admin_credentials_binding {' \
    'void console_bind_admin_credentials(' \
    'bool console_admin_credentials_input_active(void);'; do
    grep -Fq -- "$item506_inc3_binding" kernel/include/console.h || {
        echo "ITEM-506 inc3: console.h не объявляет:" "$item506_inc3_binding" >&2
        exit 1
    }
done
grep -Fq 'admin_credentials_production_binding.reset =' kernel/src/console.c || {
    echo "ITEM-506 inc3: production-привязка не несёт reset" >&2
    exit 1
}

# ------------------------------------------------------- the enrollment owner ---
grep -Fq 'appliance_production_admin_enrollment_verify_credential(' \
    kernel/include/appliance_production_admin_enrollment.h || {
    echo "ITEM-506 inc3: заголовок владельца не объявляет verify-only вход" >&2
    exit 1
}
for item506_inc3_owner in \
    'appliance_production_admin_enrollment_verify_credential(' \
    'admin_enrollment_credential_check(' \
    'admin_enrollment_record_failure(enrollment, now_tick)' \
    'login = credential->login;' \
    'appliance_admin_credential_verify('; do
    grep -Fq -- "$item506_inc3_owner" \
        kernel/src/appliance_production_admin_enrollment.c || {
        echo "ITEM-506 inc3: владелец не содержит:" "$item506_inc3_owner" >&2
        exit 1
    }
done
grep -Fq 'kernel_admin_credential_random' kernel/src/kernel.c || {
    echo "ITEM-506 inc3: загрузка не привязывает соляной источник" >&2
    exit 1
}
grep -Fq 'appliance_production_admin_enrollment_set_random(' kernel/src/kernel.c || {
    echo "ITEM-506 inc3: соляной источник не привязывается к владельцу" >&2
    exit 1
}

# ---------------------------------------------------------------- the panel -----
grep -Fq 'const uint8_t *user,' kernel/include/appliance_management_panel.h || {
    echo "ITEM-506 inc3: граница панели не принимает логин" >&2
    exit 1
}
grep -Fq 'form.user,' kernel/src/appliance_management_panel.c || {
    echo "ITEM-506 inc3: панель не передаёт разобранный логин" >&2
    exit 1
}
# ITEM-619: the adapter uses the cooperative begin/finish halves of the same
# structural entry; both are required so a caller cannot skip the session or
# the failure accounting.
grep -Fq 'appliance_production_admin_enrollment_begin_credential_check(' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-506 inc3: адаптер serving не начинает структурный вход" >&2
    exit 1
}
grep -Fq 'appliance_production_admin_enrollment_finish_credential_check(' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-506 inc3: адаптер serving не завершает структурный вход" >&2
    exit 1
}
grep -Fq '#define VNF_HTTPS_QEMU_PANEL_LOGIN "operator"' \
    kernel/src/virtio_net_frame_qemu.c || {
    echo "ITEM-506 inc3: QEMU-фикстура не фиксирует логин" >&2
    exit 1
}

# --------------------------------------------------------------- the proofs -----
for item506_inc3_test in \
    'static void test_credentials_command_classify(void)' \
    'static void test_credentials_conversation(void)' \
    'the typed current password is never echoed' \
    'the password was proven, the seed re-sealed, then the account set' \
    'an appliance with no verifiable account sets the first one' \
    'a legacy record is not migrated by an unproven change' \
    'the legacy refusal names the explicit reset step' \
    'a line shortened back under the cap submits, not stays refused' \
    'an over-long line is refused as too long' \
    'the confirmation performs the reset' \
    'test_console_journal_hidden_input' \
    'the typed password is never written to the durable journal' \
    'the hidden password never becomes a history entry' \
    'test_console_journal_removed_secret_command' \
    'the durable command record redacts the removed secret argument' \
    'the removed spelling is not recallable from the history' \
    'the refusal echo does not repeat the secret' \
    'the prefix helper never rewrites the argument it examines'; do
    grep -Fq -- "$item506_inc3_test" tests/console-host.c || {
        echo "ITEM-506 inc3: console-host не утверждает:" "$item506_inc3_test" >&2
        exit 1
    }
done
for item506_inc3_test in \
    'static void test_verify_credential(void)' \
    'the verify step opens no session' \
    'the verify step shares the five-failure lockout' \
    'the locked account refuses the verify step too' \
    'the verify step refuses a legacy record by name'; do
    grep -Fq -- "$item506_inc3_test" \
        tests/appliance-production-admin-enrollment-host.c || {
        echo "ITEM-506 inc3: enrollment-host не утверждает:" "$item506_inc3_test" >&2
        exit 1
    }
done
grep -Fq '#define PANEL_BROWSER_LOGIN "admin"' \
    tests/appliance-management-panel-host.c || {
    echo "ITEM-506 inc3: host-проба панели не фиксирует логин" >&2
    exit 1
}
grep -Fq 'test-console-host' Makefile || {
    echo "ITEM-506 inc3: цель консольной пробы не зарегистрирована" >&2
    exit 1
}
grep -Fq 'kernel/src/appliance_admin_credentials_prompt.c' Makefile || {
    echo "ITEM-506 inc3: консольная проба не линкует модуль разговора" >&2
    exit 1
}

# ------------------------------------------------------------- the documents ----
for item506_inc3_doc in \
    docs/hfoc-appliance-boot-console.md \
    docs/hfoc-production-admin-enrollment.md \
    docs/hfoc-hardware-acceptance.md; do
    grep -Fq -- 'CREDENTIALS' "$item506_inc3_doc" || {
        echo "ITEM-506 inc3: $item506_inc3_doc не называет команду" >&2
        exit 1
    }
done
grep -Fq 'CREDENTIALS RESET CONFIRM' docs/hfoc-production-admin-enrollment.md || {
    echo "ITEM-506 inc3: документ владельца не описывает путь восстановления" >&2
    exit 1
}
grep -Fq 'LOGIN' docs/hfoc-hardware-acceptance.md || {
    echo "ITEM-506 inc3: стадия AUTH не описывает локальный вход" >&2
    exit 1
}
grep -Fq 'removed, not hidden' docs/hfoc-management-web-ui.md || {
    echo "ITEM-506 inc3: документ панели не называет удалённые команды" >&2
    exit 1
}
if grep -Fq '<code>admin-provision' docs/hfoc-management-web-ui.md; then
    echo "ITEM-506 inc3: документ панели всё ещё учит удалённой команде" >&2
    exit 1
fi
grep -Fq 'ITEM-506 increment 3' docs/ARCHITECTURE.md || {
    echo "ITEM-506 inc3: ARCHITECTURE не описывает инкремент" >&2
    exit 1
}
grep -Fq 'ITEM-506 increment 3' docs/ROADMAP.md || {
    echo "ITEM-506 inc3: ROADMAP не называет инкремент" >&2
    exit 1
}
grep -Fq 'Инкремент 3 (этот PR)' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-506 inc3: очередь не описывает третий инкремент" >&2
    exit 1
}
