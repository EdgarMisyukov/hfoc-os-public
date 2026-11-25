#!/usr/bin/env bash
# ITEM-509 increment 20: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-509 increment 20 (509.7d-1): the credential conversation the console's
# `credentials`/`login` commands and the installer's account screen share reads
# every question and refusal from the one catalog in the conversation's
# language.  The English column is byte for byte the literals the module
# carried, so the console output, the QEMU transcripts and the
# [ADMIN-ENROLLMENT] records do not move; the conversation's language is the
# console's published setting on one surface and APPLIANCE_LANGUAGE_DEFAULT on
# the installer (509.7c-2 waits for the owner).  A language the catalog cannot
# answer is the named refusal `CREDENTIAL TRANSLATION MISSING` with status
# MISSING_TRANSLATION, never a silent English question.
for item509_inc20_file in \
    kernel/include/appliance_admin_credentials_prompt.h \
    kernel/src/appliance_admin_credentials_prompt.c \
    kernel/include/installer_credential_screen.h \
    kernel/src/installer_credential_screen.c \
    kernel/src/console.c \
    kernel/src/kernel.c \
    tests/appliance-admin-credentials-prompt-host.c \
    tests/installer-credential-screen-host.c \
    docs/hfoc-language-surfaces.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc20_file ]] || {
        echo "ITEM-509 inc20: отсутствует $item509_inc20_file" >&2
        exit 1
    }
done

# 1. The catalog carries the conversations's fourteen strings, English verbatim
#    and Russian beside it.
for item509_inc20_key in \
    '{"credential.prompt.current", "Current password: ",' \
    '{"credential.prompt.new_login", "New login: ",' \
    '{"credential.prompt.new_password", "New password: ",' \
    '{"credential.prompt.confirm", "Repeat new password: ",' \
    '{"credential.prompt.login", "Login: ",' \
    '{"credential.prompt.password", "Password: ",' \
    '{"credential.refusal.too_long",' \
    '{"credential.refusal.current_empty",' \
    '{"credential.refusal.login_policy",' \
    '{"credential.refusal.password_policy",' \
    '{"credential.refusal.weak_password",' \
    '{"credential.refusal.mismatch", "The passwords do not match. ",' \
    '{"credential.refusal.password_empty", "The password cannot be empty. ",' \
    '{"credential.cancelled", "Credential entry cancelled.",'; do
    grep -Fq -- "$item509_inc20_key" kernel/src/appliance_language.c || {
        echo "ITEM-509 inc20: каталог не несёт: $item509_inc20_key" >&2
        exit 1
    }
done

# 2. The module reads the keys and holds no English copy any more.
for item509_inc20_module_key in \
    '"credential.prompt.current"' \
    '"credential.prompt.new_login"' \
    '"credential.prompt.new_password"' \
    '"credential.prompt.confirm"' \
    '"credential.prompt.login"' \
    '"credential.prompt.password"' \
    '"credential.refusal.too_long"' \
    '"credential.refusal.current_empty"' \
    '"credential.refusal.login_policy"' \
    '"credential.refusal.password_policy"' \
    '"credential.refusal.weak_password"' \
    '"credential.refusal.mismatch"' \
    '"credential.refusal.password_empty"' \
    '"credential.cancelled"'; do
    grep -Fq -- "$item509_inc20_module_key" \
        kernel/src/appliance_admin_credentials_prompt.c || {
        echo "ITEM-509 inc20: разговор не читает каталог: $item509_inc20_module_key" >&2
        exit 1
    }
done
for item509_inc20_gone in \
    '"Current password: "' \
    '"New login: "' \
    '"New password: "' \
    '"Repeat new password: "' \
    '"Login: "' \
    '"Password: "' \
    '"The passwords do not match. "' \
    '"Credential entry cancelled."'; do
    if grep -Fq -- "$item509_inc20_gone" \
        kernel/src/appliance_admin_credentials_prompt.c; then
        echo "ITEM-509 inc20: английская проза осталась литералом: $item509_inc20_gone" >&2
        exit 1
    fi
done
grep -Fq 'CREDENTIAL TRANSLATION MISSING' \
    kernel/src/appliance_admin_credentials_prompt.c || {
    echo "ITEM-509 inc20: нет именованного отказа перевода" >&2
    exit 1
}
grep -Fq 'APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MISSING_TRANSLATION = 4,' \
    kernel/include/appliance_admin_credentials_prompt.h || {
    echo "ITEM-509 inc20: статус отказа перевода не заведён" >&2
    exit 1
}
grep -Fq 'enum appliance_language language;' \
    kernel/include/appliance_admin_credentials_prompt.h || {
    echo "ITEM-509 inc20: разговор не несёт язык" >&2
    exit 1
}
grep -Fq 'text_out, text_size, text_length_out, language);' \
    kernel/src/installer_credential_screen.c || {
    echo "ITEM-509 inc20: экран не передаёт язык разговору" >&2
    exit 1
}

# 3. The console speaks the published language and shows the named refusal; the
#    installer passes the language the medium declared (the default when it
#    declares none).
grep -Fq '&text_length, console_active_language());' kernel/src/console.c || {
    echo "ITEM-509 inc20: консоль не передаёт действующий язык разговору" >&2
    exit 1
}
grep -Fq 'APPLIANCE_ADMIN_CREDENTIALS_PROMPT_MISSING_TRANSLATION &&' \
    kernel/src/console.c || {
    echo "ITEM-509 inc20: консоль не называет отказ перевода" >&2
    exit 1
}
grep -Fq 'installer_credential_screen_run_mode(' kernel/src/kernel.c || {
    echo "ITEM-509 inc20: установка не открывает экран учётной записи" >&2
    exit 1
}
grep -Fq 'kernel_installer_declared_language()) ==' kernel/src/kernel.c || {
    echo "ITEM-509 inc20: установка не передаёт язык экрану учётной записи" >&2
    exit 1
}
grep -Fq 'const char *installer_credential_screen_title(enum appliance_language language);' \
    kernel/include/installer_credential_screen.h || {
    echo "ITEM-509 inc20: заголовок экрана не читает каталог" >&2
    exit 1
}
grep -Fq '"installer.credential.title"' kernel/src/installer_credential_screen.c || {
    echo "ITEM-509 inc20: экран не читает заголовок из каталога" >&2
    exit 1
}
grep -Fq '"installer.credential.password_note"' \
    kernel/src/installer_credential_screen.c || {
    echo "ITEM-509 inc20: экран не читает подсказку из каталога" >&2
    exit 1
}
for item509_inc20_screen_gone in \
    '"HFOC OS installer - account"' \
    '"The password is not shown while you type."'; do
    if grep -Fq -- "$item509_inc20_screen_gone" \
        kernel/src/installer_credential_screen.c; then
        echo "ITEM-509 inc20: экран оставил английский литерал: $item509_inc20_screen_gone" >&2
        exit 1
    fi
done

# 4. The proofs pin the Russian conversation, the refusal and the catalog link.
for item509_inc20_check in \
    'the Russian change conversation starts' \
    'the first question is the Russian catalog question' \
    'the Russian refusal precedes the repeated question and fits' \
    'the Russian conversation submits like the English one' \
    'the Russian cancellation notice comes from the catalog' \
    'an unknown language starts no conversation and names the refusal' \
    'a language the catalog cannot answer later is named, not ignored'; do
    grep -Fq -- "$item509_inc20_check" \
        tests/appliance-admin-credentials-prompt-host.c || {
        echo "ITEM-509 inc20: проба разговора не утверждает: $item509_inc20_check" >&2
        exit 1
    }
done
for item509_inc20_check in \
    'the Russian title and hint come from the catalog column' \
    'the Russian account screen starts with the Russian question' \
    'an unknown language starts no conversation and names the refusal' \
    'the machine loop completes the whole account conversation' \
    'an ignored key does not end the conversation before its questions' \
    'the machine loop leaves when the catalog cannot answer' \
    'the machine loop paints the named refusal, not a blank screen'; do
    grep -Fq -- "$item509_inc20_check" \
        tests/installer-credential-screen-host.c || {
        echo "ITEM-509 inc20: проба экрана не утверждает: $item509_inc20_check" >&2
        exit 1
    }
done
# The machine loop is compiled into the host proof, not only into the QEMU
# proof: the regression that sent the preserve proof red lived exactly in the
# loop's action handling, and the host proof must keep driving it.
grep -Fq 'installer_credential_screen_run(&screen,' \
    tests/installer-credential-screen-host.c || {
    echo "ITEM-509 inc20: машинный цикл не доказывается на хосте" >&2
    exit 1
}
grep -Fq 'kernel/src/appliance_language.c' Makefile || {
    echo "ITEM-509 inc20: пробы не линкуют каталог" >&2
    exit 1
}

# 5. The increment is recorded where the next window reads it.
grep -Fq 'ITEM-509 increment 20' docs/hfoc-language-surfaces.md || {
    echo "ITEM-509 inc20: поверхности языка не называют инкремент" >&2
    exit 1
}
grep -Fq '509.7d-1' docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-509 inc20: очередь не называет кусок 509.7d" >&2
    exit 1
}
