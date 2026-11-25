#!/usr/bin/env bash
# ITEM-506 increment 4: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The installer sets the account.
#
# Increment 3 made the console and the panel sign in with the one account.
# This increment gives the operator install the same account: the screen after
# the accepted disk warning asks for the login and the password twice (no
# current password exists on a fresh machine), the flow derives the PBKDF2
# record through a salt seam, and the state step writes it into the installed
# system and proves it by reading it back before the run promises
# `admin=SET`.  An unattended (or declined) install writes no administrator
# record and says `admin=UNSET`.  The claims pinned here:
#
#   * the prompt module gained `MODE_SET` and `SUBMIT_SET`: the same policy,
#     the same refusals and the same zeroization as the console conversation,
#     without a current-password step;
#   * `installer_credential_screen.{h,c}` is the hidden line editor and the
#     machine screen: characters are never echoed, an over-long line is refused
#     instead of cut, a backspace clears the overflow, an empty line cancels,
#     and every buffer is zeroed;
#   * the installer kernel has no hardware entropy owner, so the salt is a
#     caller seam: `production_install_flow_set_credential_salt()` and the
#     owner's documented unique (not secret) derivation in `kernel.c` over the
#     domain, the chosen target disk's identity, the boot tick and a per-run
#     counter;
#   * `production_install_flow_set_operator_credential()` keeps only the
#     model's record - never the password - and
#     `production_installer_state_commit_admin()` writes it and reads it back
#     through the installed system's own reader, failing closed on a mismatch;
#   * `apply()` publishes `[PRODUCTION-INSTALL] admin=SET login=...
#     persisted=VERIFIED` or `admin=UNSET reason=UNATTENDED` /
#     `reason=OPERATOR_DECLINED`, and the QEMU proof types the account on the
#     real screen, requires the SET record and requires the password to be
#     absent from the serial log.
for item506_inc4_file in \
    kernel/include/installer_credential_screen.h \
    kernel/src/installer_credential_screen.c \
    kernel/include/appliance_admin_credentials_prompt.h \
    kernel/src/appliance_admin_credentials_prompt.c \
    kernel/include/production_install_flow.h \
    kernel/src/production_install_flow.c \
    kernel/include/production_installer_state.h \
    kernel/src/production_installer_state.c \
    kernel/src/kernel.c \
    tests/installer-credential-screen-host.c \
    tests/appliance-admin-credentials-prompt-host.c \
    tests/production-installer-orchestration-host.c \
    scripts/test-qemu-installer-disk-screen.sh; do
    [[ -f $item506_inc4_file ]] || {
        echo "ITEM-506 inc4: отсутствует $item506_inc4_file" >&2
        exit 1
    }
done

# ------------------------------------------------------------------ the model ---
for item506_inc4_prompt in \
    'APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_SET = 3' \
    'APPLIANCE_ADMIN_CREDENTIALS_PROMPT_ACTION_SUBMIT_SET = 5' \
    'prompt->step = APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_LOGIN;' \
    'APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MODE_SET' \
    'return "SET";' \
    'return "SUBMIT_SET";'; do
    grep -Fq -- "$item506_inc4_prompt" \
        kernel/include/appliance_admin_credentials_prompt.h \
        kernel/src/appliance_admin_credentials_prompt.c || {
        echo "ITEM-506 inc4: разговор не содержит:" "$item506_inc4_prompt" >&2
        exit 1
    }
done

# ------------------------------------------------------------- the screen -----
# ITEM-509 increment 20 moved the title and the hint to the one catalog, so the
# screen pins the keys and the catalog pins the English values verbatim.
for item506_inc4_screen in \
    'installer_credential_screen_key(' \
    'installer_credential_screen_run(' \
    'installer_credential_screen_zero_line(' \
    'screen->overflow = true;' \
    'screen->overflow = false;' \
    'INSTALLER_CREDENTIAL_SCREEN_ACTION_COMPLETE' \
    'INSTALLER_CREDENTIAL_SCREEN_ACTION_CANCELLED' \
    '"installer.credential.title"' \
    '"installer.credential.password_note"'; do
    grep -Fq -- "$item506_inc4_screen" \
        kernel/src/installer_credential_screen.c || {
        echo "ITEM-506 inc4: экран не содержит:" "$item506_inc4_screen" >&2
        exit 1
    }
done
for item506_inc4_screen_text in \
    '{"installer.credential.title", "HFOC OS installer - account",' \
    '{"installer.credential.password_note",' \
    'The password is shown as asterisks as you type.'; do
    grep -Fq -- "$item506_inc4_screen_text" \
        kernel/src/appliance_language.c || {
        echo "ITEM-506 inc4: каталог не содержит:" \
             "$item506_inc4_screen_text" >&2
        exit 1
    }
done
# ITEM-509 increment 20: the machine loop is compiled into the host proof
# instead of being cut out of it - the loop's action handling is exactly where a
# regression escaped to the QEMU preserve proof - so the host test provides the
# display, local-input and serial stubs and drives `installer_credential_screen_run`.
grep -Fq 'installer_credential_screen_run(&screen,' \
    tests/installer-credential-screen-host.c || {
    echo "ITEM-506 inc4: экран не отделяет машинный цикл для host-пробы" >&2
    exit 1
}
grep -Fq 'bool local_input_pop_event(struct keyboard_event *event)' \
    tests/installer-credential-screen-host.c || {
    echo "ITEM-506 inc4: host-проба не подставляет очередь ввода" >&2
    exit 1
}
grep -Fq 'installer_credential_screen_reset(&screen);' kernel/src/kernel.c || {
    echo "ITEM-506 inc4: загрузка не обнуляет буферы экрана" >&2
    exit 1
}
# ITEM-506 inc4, the owner's salt decision (2026-10-03): the install salt is a
# derivation that names the chosen target disk's identity beside the boot tick
# and the per-run counter, and the screen is opened with that identity.
for item506_inc4_salt in \
    'kernel_installer_credential_salt(' \
    'kernel_installer_credential_salt_context' \
    'sha256_update(&sha, salt_context->identity, identity_length)' \
    'HFOC-INSTALL-ADMIN-SALT-V1:' \
    'kernel_installer_account_screen(chosen.identity);'; do
    grep -Fq -- "$item506_inc4_salt" kernel/src/kernel.c || {
        echo "ITEM-506 inc4: загрузка не привязывает соляной источник:" \
            "$item506_inc4_salt" >&2
        exit 1
    }
done

# --------------------------------------------------------------- the flow -----
for item506_inc4_flow in \
    'production_install_flow_set_credential_salt(' \
    'production_install_flow_set_operator_credential(' \
    'appliance_admin_credential_create(' \
    'PRODUCTION_INSTALL_FLOW_CREDENTIAL_NO_SALT' \
    'production_installer_state_commit_admin(' \
    '[PRODUCTION-INSTALL] admin=SET login=' \
    'persisted=VERIFIED' \
    'admin=UNSET reason=' \
    'UNATTENDED' \
    'OPERATOR_DECLINED'; do
    grep -Fq -- "$item506_inc4_flow" kernel/src/production_install_flow.c || {
        echo "ITEM-506 inc4: поток установки не содержит:" "$item506_inc4_flow" >&2
        exit 1
    }
done
grep -Fq 'production_install_flow_set_operator_credential(' \
    kernel/include/production_install_flow.h || {
    echo "ITEM-506 inc4: заголовок потока не объявляет учётную запись" >&2
    exit 1
}
grep -Fq 'production_installer_state_commit_admin(' \
    kernel/include/production_installer_state.h || {
    echo "ITEM-506 inc4: заголовок состояния не объявляет запись учётной записи" >&2
    exit 1
}
grep -Fq 'appliance_persistent_secret_config_admin_credential(' \
    kernel/src/production_installer_state.c || {
    echo "ITEM-506 inc4: запись не проверяется обратным чтением" >&2
    exit 1
}

# --------------------------------------------------------------- the proofs ----
for item506_inc4_test in \
    'static void test_set_conversation(void)' \
    'the matching pair submits the set, not a change' \
    'the installer conversation never asks for a current password' \
    'the installer enforces the same password policy'; do
    grep -Fq -- "$item506_inc4_test" \
        tests/appliance-admin-credentials-prompt-host.c || {
        echo "ITEM-506 inc4: prompt-host не утверждает:" "$item506_inc4_test" >&2
        exit 1
    }
done
for item506_inc4_test in \
    'static void test_set_conversation(void)' \
    'a typed character is kept hidden and paints nothing' \
    'an over-long line is refused as too long' \
    'one backspace brings the line back under the cap' \
    'an empty line cancels and says so' \
    'reset zeroes the whole screen'; do
    grep -Fq -- "$item506_inc4_test" \
        tests/installer-credential-screen-host.c || {
        echo "ITEM-506 inc4: screen-host не утверждает:" "$item506_inc4_test" >&2
        exit 1
    }
done
grep -Fq 'test-installer-credential-screen-host' Makefile || {
    echo "ITEM-506 inc4: цель host-пробы экрана не зарегистрирована" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-installer-credential-screen-host' Makefile || {
    echo "ITEM-506 inc4: host-проба экрана не входит в test-host-suite" >&2
    exit 1
}
for item506_inc4_test in \
    'static void test_state_commits_the_operator_account(void)' \
    'the account fixture builds a real PBKDF2 record' \
    'the account record commits into the installed state' \
    'the committed record is byte-for-byte the account that was set'; do
    grep -Fq -- "$item506_inc4_test" \
        tests/production-installer-orchestration-host.c || {
        echo "ITEM-506 inc4: orchestration-host не утверждает:" \
            "$item506_inc4_test" >&2
        exit 1
    }
done
for item506_inc4_qemu in \
    'New login:' \
    'New password:' \
    'Repeat new password:' \
    'admin=SET login=' \
    'persisted=VERIFIED' \
    'the hidden account password reached the serial log'; do
    grep -Fq -- "$item506_inc4_qemu" \
        scripts/test-qemu-installer-disk-screen.sh || {
        echo "ITEM-506 inc4: QEMU-проба не требует:" "$item506_inc4_qemu" >&2
            exit 1
        }
done
# The account screen sits between the accepted warning and the install on
# every operator path, so the release proof (ITEM-398/501) answers it too: the
# screen marker, the hidden password and the committed record are pinned here,
# and a proof that stops waiting for a record the screen now precedes would be
# caught by this slice instead of by a red CI phase.
for item506_inc4_qemu in \
    'New login:' \
    'New password:' \
    'Repeat new password:' \
    'admin=SET login=' \
    'persisted=VERIFIED' \
    'the hidden account password reached the serial log'; do
    grep -Fq -- "$item506_inc4_qemu" \
        scripts/test-qemu-production-install-flow-preserve.sh || {
        echo "ITEM-506 inc4: release-проба не отвечает на экран:" \
            "$item506_inc4_qemu" >&2
        exit 1
    }
done

# ------------------------------------------------------------- the documents ---
for item506_inc4_doc in \
    docs/hfoc-production-install-flow.md \
    docs/hfoc-production-admin-enrollment.md \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    grep -Fq -- 'admin=SET' "$item506_inc4_doc" || {
        echo "ITEM-506 inc4: $item506_inc4_doc не описывает установку учётной записи" >&2
        exit 1
    }
done
grep -Fq 'ITEM-506 increment 4' docs/ARCHITECTURE.md || {
    echo "ITEM-506 inc4: ARCHITECTURE не описывает инкремент" >&2
    exit 1
}
grep -Fq 'Инкремент 4 (этот PR)' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-506 inc4: очередь не описывает четвёртый инкремент" >&2
    exit 1
}
