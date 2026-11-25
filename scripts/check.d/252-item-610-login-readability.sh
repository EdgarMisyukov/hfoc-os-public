#!/usr/bin/env bash
# ITEM-610: contracted slice of the HFOC OS check suite. Sourced in pinned order
#           by scripts/check.sh; lint it on its own with shellcheck.
#
# The owner's 2026-11-15 report: the local login is invisible while typing, the
# password gives no feedback, and the text is too small.  The contract is:
#
#   * the installed console paints the login exactly as it is typed and one `*`
#     per accepted password character; the echo is framebuffer-only, backspace
#     removes one cell and never the question, and the over-long character is
#     refused without a mask cell;
#   * ENTER on a credential line paints a localised, honest checking line and
#     presents the frame before the synchronous verification starts;
#   * the installer account screen paints the same login/mask on its display and
#     the serial transcript still carries no typed credential;
#   * the automatic font floor is raised to 80 columns so the common 1080p panel
#     gets the 18x24-pixel cell, with the 80-column grid still supported.

for item610_file in \
    kernel/src/console.c \
    kernel/src/installer_credential_screen.c \
    kernel/src/appliance_language.c \
    kernel/include/framebuffer.h \
    tests/console-host.c \
    tests/installer-credential-screen-host.c \
    tests/framebuffer-scroll-host.c \
    scripts/test-qemu-host-provisioned-entropy.sh; do
    [[ -f $item610_file ]] || {
        echo "ITEM-610: отсутствует $item610_file" >&2
        exit 1
    }
done

# 1. The installed console knows which steps are secret and paints the mask.
for item610_console in \
    'static bool console_admin_credentials_step_is_secret(void)' \
    'APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_CURRENT:' \
    'APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_NEW_PASSWORD:' \
    'APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_CONFIRM:' \
    'APPLIANCE_ADMIN_CREDENTIALS_PROMPT_STEP_LOGIN_PASSWORD:' \
    "console_emit(console_admin_credentials_step_is_secret() ? '*' : value," \
    'static void console_admin_credentials_echo_backspace(void)' \
    'console_admin_credentials_echo_character(' \
    'console_admin_credentials_checking();' \
    'console_local_menu_text("console.prose.credentials_checking")' \
    'framebuffer_flush();'; do
    grep -Fq -- "$item610_console" kernel/src/console.c || {
        echo "ITEM-610: консоль не содержит:" "$item610_console" >&2
        exit 1
    }
done
# The echo is screen-only: the two password/login echo calls must pass false.
grep -Fq -- '"console.prose.credentials_checking"' \
    kernel/src/appliance_language.c || {
    echo "ITEM-610: каталог не несёт строку проверки" >&2
    exit 1
}

# 2. The installer screen paints the same feedback from the same prompt state.
for item610_installer in \
    'static bool installer_credential_screen_step_is_secret(' \
    'static void installer_credential_screen_echo(' \
    "? '*'" \
    'installer_credential_screen_step_is_secret(screen)' \
    'installer_credential_screen_refresh(screen, text);' \
    'static void installer_credential_screen_checking(' \
    'installer_credential_screen_checking(screen);' \
    '"installer.credential.checking"'; do
    grep -Fq -- "$item610_installer" \
        kernel/src/installer_credential_screen.c || {
        echo "ITEM-610: экран установщика не содержит:" \
            "$item610_installer" >&2
        exit 1
    }
done
grep -Fq '{"installer.credential.password_note",' \
    kernel/src/appliance_language.c || {
    echo "ITEM-610: подсказка установщика потеряна" >&2
    exit 1
}
grep -Fq 'The password is shown as asterisks as you type.' \
    kernel/src/appliance_language.c || {
    echo "ITEM-610: подсказка установщика всё ещё говорит, что пароля не видно" >&2
    exit 1
}

# 3. The readable floor is the pure rule's own constants and the host proof
#    pins the modes the owner named.
grep -Fq '#define FRAMEBUFFER_FONT_MIN_COLUMNS 80U' \
    kernel/include/framebuffer.h || {
    echo "ITEM-610: минимум колонок не поднят до 80" >&2
    exit 1
}
grep -Fq '#define FRAMEBUFFER_FONT_MIN_ROWS 30U' \
    kernel/include/framebuffer.h || {
    echo "ITEM-610: минимум строк изменился неосознанно" >&2
    exit 1
}
for item610_scale in \
    '1024x768 gets a 1x cell' \
    '1366x768 gets a 1x cell' \
    '1920x1080 gets a 2x cell' \
    '3840x2160 gets a 3x cell'; do
    grep -Fq -- "$item610_scale" tests/framebuffer-scroll-host.c || {
        echo "ITEM-610: проба шкалы не утверждает:" "$item610_scale" >&2
        exit 1
    }
done

# 4. The host proofs drive the real prompt and the machine loop.
for item610_proof in \
    'each accepted current-password character paints one star' \
    'backspace paints no new star' \
    'the login is visible as it is typed' \
    'ENTER on the final password announces the check' \
    'the refused character paints no star' \
    'no password byte reaches the serial transcript'; do
    grep -Fq -- "$item610_proof" tests/console-host.c || {
        echo "ITEM-610: проба консоли не утверждает:" "$item610_proof" >&2
        exit 1
    }
done
for item610_screen_proof in \
    'the installer login is visible as it is typed' \
    'the installer password paints one star per character' \
    'the installer typed answer stays off the serial stream' \
    'the completed account paints the check line before the caller runs it'; do
    grep -Fq -- "$item610_screen_proof" \
        tests/installer-credential-screen-host.c || {
        echo "ITEM-610: проба экрана не утверждает:" \
            "$item610_screen_proof" >&2
        exit 1
    }
done

# The two installed-boot QEMU proofs name the same 1920x1080 scale as the
# console-history proof; the 80-column floor makes it 3, not 2.
for item610_installed_scale in \
    scripts/test-qemu-installed-boot.sh \
    scripts/test-qemu-production-install-flow.sh; do
    grep -Fq 'if scale != 2:' "$item610_installed_scale" || {
        echo "ITEM-610: $item610_installed_scale ждёт старую шкалу" >&2
        exit 1
    }
    grep -Fq 'not 2 at 1920x1080' "$item610_installed_scale" || {
        echo "ITEM-610: $item610_installed_scale не называет новую шкалу" >&2
        exit 1
    }
done

# The menu pointer proofs divide the pixel aim by the published cell scale:
# their old accelerated walk overshoots the first row on the 3x 1080p cell.
for item610_pointer in \
    scripts/test-qemu-usb-mouse-menu.sh \
    scripts/test-qemu-usb-two-controllers.sh; do
    grep -Fq 'pointer_scale = geometry[3] // 8' "$item610_pointer" || {
        echo "ITEM-610: $item610_pointer не выводит шкалу указателя" >&2
        exit 1
    }
    grep -Fq 'walk_slow(entry_x // pointer_scale, entry_y // pointer_scale)' \
        "$item610_pointer" || {
        echo "ITEM-610: $item610_pointer не компенсирует шкалу" >&2
        exit 1
    }
done

# 5. The production QEMU proof reads the rendered cells where serial has no
#    credential answer.
for item610_qemu in \
    'def wait_echo(monitor, label, value, secret):' \
    'screendump' \
    'cells are not one repeated asterisk glyph' \
    'wait_echo(monitor, "login", login, False)' \
    'wait_echo(monitor, "password", password, True)' \
    'wait_echo(monitor, "new-login", login, False)' \
    'wait_echo(monitor, "new-password", updated_password, True)'; do
    grep -Fq -- "$item610_qemu" \
        scripts/test-qemu-host-provisioned-entropy.sh || {
        echo "ITEM-610: QEMU-проба не содержит:" "$item610_qemu" >&2
        exit 1
    }
done
