#!/usr/bin/env bash
# ITEM-509 increment 9: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-509 increment 9 is 509.7b: the menus speak the chosen language.  The
# local menu the machine paints and the boot console screen take the language
# as an explicit parameter - not a global the caller can forget - and every
# human label comes from the one catalog, so the same key cannot be translated
# on one surface and English on another.  The claims pinned here:
#
#   * both renderers carry the language in their signatures, and the console
#     hands them the language it already published (`console_active_language()`),
#     so a console that switched to `ru` cannot keep painting English;
#   * every label is a catalog lookup (`menu.local.*`, `menu.bootconsole.*`),
#     the English values are the wording that stood in the renderers before, and
#     an incomplete catalog is a visible refusal rather than half a menu;
#   * the machine words the console runs (`status`, `reboot`, ...), the offer
#     records, the state values and the product name stay English, which is the
#     item's boundary;
#   * the host proofs render both languages, and the Russian menu is exact - a
#     half-translated screen fails;
#   * the live QEMU proofs keep their English markers, because the default
#     language is `en` and the catalog's English values are byte-for-byte.
for item509_inc9_file in \
    kernel/include/local_menu.h \
    kernel/src/local_menu.c \
    kernel/include/appliance_boot_console.h \
    kernel/src/appliance_boot_console.c \
    kernel/src/appliance_language.c \
    kernel/src/console.c \
    tests/local-menu-host.c \
    tests/appliance-boot-console-host.c \
    tests/console-host.c \
    docs/hfoc-appliance-boot-console.md \
    docs/hfoc-language-surfaces.md \
    docs/ROADMAP.md \
    docs/ARCHITECTURE.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc9_file ]] || {
        echo "ITEM-509 inc9: отсутствует $item509_inc9_file" >&2
        exit 1
    }
done

# 1. The language is an explicit parameter of both renderers, and the label
#    lookup answers in the requested language.
for item509_inc9_signature in \
    'const char *local_menu_entry_label(enum local_menu_entry entry,' \
    'enum appliance_language language);' \
    'size_t local_menu_render(' \
    'enum appliance_language language,' \
    'enum appliance_boot_console_status' \
    'appliance_boot_console_render(' ; do
    grep -Fq -- "$item509_inc9_signature" \
        kernel/include/local_menu.h kernel/include/appliance_boot_console.h || {
        echo "ITEM-509 inc9: заголовок меню не несёт: $item509_inc9_signature" >&2
        exit 1
    }
done
grep -Fq 'appliance_language_string(language, text->label_key)' \
    kernel/src/local_menu.c || {
    echo "ITEM-509 inc9: метка меню берётся не из каталога" >&2
    exit 1
}

# 2. The console hands the renderers the language it published, at both call
#    sites, instead of keeping a second copy.
grep -Fq 'local_menu_render(' kernel/src/console.c || {
    echo "ITEM-509 inc9: консоль не рисует меню" >&2
    exit 1
}
grep -Fq '&menu_state, console_active_language(), rendered,' \
    kernel/src/console.c || {
    echo "ITEM-509 inc9: консоль не передаёт язык в локальное меню" >&2
    exit 1
}
grep -Fq '&snapshot, console_active_language(), output, sizeof(output),' \
    kernel/src/console.c || {
    echo "ITEM-509 inc9: консоль не передаёт язык в boot-консоль" >&2
    exit 1
}

# 3. The labels live in the one catalog, in both languages, and the renderers
#    no longer carry English labels of their own.
for item509_inc9_catalog in \
    '{"menu.local.title", "LOCAL MENU", "ЛОКАЛЬНОЕ МЕНЮ"}' \
    '{"menu.local.hint", "UP/DOWN select, ENTER run, Q leave",' \
    '{"menu.local.shutdown", "Shut down", "Выключение"}' \
    '{"menu.local.unknown", "UNKNOWN", "НЕИЗВЕСТНО"}' \
    '{"menu.bootconsole.hostname", "Hostname:      ", "Имя узла:      "}' \
    '{"menu.bootconsole.local_admin", "Local administration:",' \
    '{"menu.bootconsole.entry.recovery", "[6] Recovery",' ; do
    grep -Fq -- "$item509_inc9_catalog" kernel/src/appliance_language.c || {
        echo "ITEM-509 inc9: каталог не несёт: $item509_inc9_catalog" >&2
        exit 1
    }
done
for item509_inc9_literal in '"Status"' '"Hostname:      "' \
    '"LOCAL MENU\n"' '"UP/DOWN select, ENTER run, Q leave\n"'; do
    if grep -Fq -- "$item509_inc9_literal" \
        kernel/src/local_menu.c kernel/src/appliance_boot_console.c; then
        echo "ITEM-509 inc9: рендерер всё ещё несёт литерал $item509_inc9_literal" >&2
        exit 1
    fi
done
grep -Fq 'LOCAL_MENU_TRANSLATION_REFUSAL "MENU TRANSLATION MISSING\n"' \
    kernel/src/local_menu.c || {
    echo "ITEM-509 inc9: у неполного каталога нет видимого отказа" >&2
    exit 1
}

# 4. The boundary: the words the console runs, the records and the state
#    values stay English at every language.
for item509_inc9_boundary in \
    '{"menu.local.shutdown", "Shut down", "Выключение"}' \
    '"[LOCAL-MENU] offered reason=IDLE entries=6 keys="' \
    'appliance_console_state_string(snapshot->health_state)' ; do
    grep -Fq -- "$item509_inc9_boundary" \
        kernel/src/appliance_language.c kernel/src/console.c \
        kernel/src/appliance_boot_console.c || {
        echo "ITEM-509 inc9: граница языка не соблюдена: $item509_inc9_boundary" >&2
        exit 1
    }
done
grep -Fq '"shutdown",' kernel/src/appliance_boot_console.c || {
    echo "ITEM-509 inc9: машинное слово shutdown пропало" >&2
    exit 1
}

# 5. The proofs render both languages, and the Russian menu is exact rather
#    than "something Cyrillic appeared".
grep -Fq 'static void test_render_russian(void)' tests/local-menu-host.c || {
    echo "ITEM-509 inc9: проба меню не несёт русский рендер" >&2
    exit 1
}
grep -Fq 'test_render_russian();' tests/local-menu-host.c || {
    echo "ITEM-509 inc9: русский рендер меню не вызван" >&2
    exit 1
}
grep -Fq '"ЛОКАЛЬНОЕ МЕНЮ"' tests/local-menu-host.c || {
    echo "ITEM-509 inc9: проба меню не требует русский заголовок" >&2
    exit 1
}
grep -Fq 'static void test_russian_render(void)' tests/appliance-boot-console-host.c || {
    echo "ITEM-509 inc9: проба boot-консоли не несёт русский экран" >&2
    exit 1
}
grep -Fq 'test_russian_render();' tests/appliance-boot-console-host.c || {
    echo "ITEM-509 inc9: русский экран boot-консоли не вызван" >&2
    exit 1
}
grep -Fq '"Локальное администрирование:\n"' tests/appliance-boot-console-host.c || {
    echo "ITEM-509 inc9: проба boot-консоли не требует русский экран" >&2
    exit 1
}
grep -Fq 'static void test_local_menu_language(void)' tests/console-host.c || {
    echo "ITEM-509 inc9: проба консоли не несёт язык меню" >&2
    exit 1
}
grep -Fq 'test_local_menu_language();' tests/console-host.c || {
    echo "ITEM-509 inc9: проверка языка меню в консоли не вызвана" >&2
    exit 1
}
grep -Fq '"the painted menu speaks the published language"' tests/console-host.c || {
    echo "ITEM-509 inc9: консольная проба не требует язык меню" >&2
    exit 1
}
grep -Fq '"[GLYPH-MISSING]"' tests/console-host.c || {
    echo "ITEM-509 inc9: проба не проверяет отсутствие маркера глифа" >&2
    exit 1
}

# 6. The live QEMU proofs keep their English markers: the default language is
#    `en`, so the catalog's English values have to be the old bytes.
grep -Fq '"[1] Status", "[4] Reboot", "[5] Shut down",' \
    scripts/test-qemu-local-menu.sh || {
    echo "ITEM-509 inc9: QEMU-проба локального меню потеряла английский маркер" >&2
    exit 1
}
grep -Fq '"[1] Status\n"' scripts/test-qemu-appliance-boot-console.sh || {
    echo "ITEM-509 inc9: QEMU-проба boot-консоли потеряла английский маркер" >&2
    exit 1
}
grep -Fq 'item509_inc9' scripts/check.d/173-item-509-menu-language.sh || {
    echo "ITEM-509 inc9: срез не называет себя" >&2
    exit 1
}

# 7. The documents record the increment and name the next step.
for item509_inc9_doc in docs/ROADMAP.md docs/ARCHITECTURE.md \
    docs/hfoc-language-surfaces.md docs/hfoc-appliance-boot-console.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    grep -Fq 'ITEM-509 increment 9' "$item509_inc9_doc" || {
        echo "ITEM-509 inc9: $item509_inc9_doc не называет инкремент" >&2
        exit 1
    }
done
grep -Fq '509.7c' docs/hfoc-language-surfaces.md || {
    echo "ITEM-509 inc9: инвентарь поверхностей не называет следующий шаг" >&2
    exit 1
}
