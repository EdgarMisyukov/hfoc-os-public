#!/usr/bin/env bash
# ITEM-509 increment 21: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-509 increment 21 (509.7d-2a): the console's offer and availability lines
# around the local menu read the one catalog in the console's published
# language.  The English column is byte for byte the literals, including the
# trailing newline each atomic write carries, so the QEMU `keyboard_absent`
# phase and the host proof that wait for those bytes do not move.  The machine
# records (`[LOCAL-MENU] offered ...`) and the menu itself are not touched.
for item509_inc21_file in \
    kernel/src/console.c \
    kernel/src/appliance_language.c \
    tests/console-host.c \
    docs/hfoc-language-surfaces.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc21_file ]] || {
        echo "ITEM-509 inc21: отсутствует $item509_inc21_file" >&2
        exit 1
    }
done

# 1. The catalog carries the four lines, English verbatim (newline included) and
#    Russian beside them.
for item509_inc21_key in \
    '{"console.menu.render_failed", "LOCAL MENU: RENDER FAILED\n",' \
    '{"console.menu.no_local_input",' \
    '{"console.menu.offer_idle",' \
    '{"console.menu.offered_reading",' \
    '"LOCAL INPUT UNAVAILABLE: THE MENU IS SHOWN FOR READING\n",' \
    '"PRESS ENTER FOR THE LOCAL MENU (N: NETWORK SETUP - SET THE ADDRESS "' \
    '"THE LOCAL MENU IS OFFERED BELOW FOR READING\n",' \
    '"ЛОКАЛЬНОЕ МЕНЮ: ОШИБКА ОТРИСОВКИ\n"' \
    '"ЛОКАЛЬНЫЙ ВВОД НЕДОСТУПЕН: МЕНЮ ПОКАЗАНО ДЛЯ ЧТЕНИЯ\n"' \
    '"ENTER — ЛОКАЛЬНОЕ МЕНЮ (N: НАСТРОЙКА СЕТИ — ЗАДАТЬ АДРЕС)\n"' \
    '"ЛОКАЛЬНОЕ МЕНЮ ПРЕДЛОЖЕНО НИЖЕ ДЛЯ ЧТЕНИЯ\n"'; do
    grep -Fq -- "$item509_inc21_key" kernel/src/appliance_language.c || {
        echo "ITEM-509 inc21: каталог не несёт: $item509_inc21_key" >&2
        exit 1
    }
done

# 2. The console reads them through one helper in the published language, and
#    the literals are gone from the console, so key and line cannot drift.
grep -Fq 'static const char *console_local_menu_text(const char *key)' \
    kernel/src/console.c || {
    echo "ITEM-509 inc21: нет помощника строк предложения меню" >&2
    exit 1
}
grep -Fq 'appliance_language_string(console_active_language(), key)' \
    kernel/src/console.c || {
    echo "ITEM-509 inc21: помощник не читает действующий язык" >&2
    exit 1
}
grep -Fq 'CONSOLE TRANSLATION MISSING' kernel/src/console.c || {
    echo "ITEM-509 inc21: нет именованного отказа перевода" >&2
    exit 1
}
for item509_inc21_call in \
    'console_local_menu_text("console.menu.render_failed")' \
    'console_local_menu_text("console.menu.no_local_input")' \
    'console_local_menu_text("console.menu.offer_idle")' \
    'console_local_menu_text("console.menu.offered_reading")'; do
    grep -Fq -- "$item509_inc21_call" kernel/src/console.c || {
        echo "ITEM-509 inc21: строка не читает каталог: $item509_inc21_call" >&2
        exit 1
    }
done
for item509_inc21_gone in \
    '"LOCAL MENU: RENDER FAILED\n"' \
    '"LOCAL INPUT UNAVAILABLE: THE MENU IS SHOWN FOR READING\n"' \
    '"THE LOCAL MENU IS OFFERED BELOW FOR READING\n"' \
    '"PRESS ENTER FOR THE LOCAL MENU'; do
    if grep -Fq -- "$item509_inc21_gone" kernel/src/console.c; then
        echo "ITEM-509 inc21: английская проза осталась литералом: $item509_inc21_gone" >&2
        exit 1
    fi
done
grep -Fq '"[LOCAL-MENU] offered reason=IDLE entries=6 keys="' \
    kernel/src/console.c || {
    echo "ITEM-509 inc21: машинная запись предложения меню потеряна" >&2
    exit 1
}
grep -Fq '"[LOCAL-MENU] offered reason=NO_LOCAL_INPUT entries=6 keys="' \
    kernel/src/console.c || {
    echo "ITEM-509 inc21: машинная запись read-only предложения потеряна" >&2
    exit 1
}

# 3. The proof paints both Russian lines atomically and keeps the English record.
for item509_inc21_check in \
    'the offer prompt speaks the published language, atomically' \
    'the read-only reason speaks the published language, atomically' \
    'the offer record stays English'; do
    grep -Fq -- "$item509_inc21_check" tests/console-host.c || {
        echo "ITEM-509 inc21: проба консоли не утверждает: $item509_inc21_check" >&2
        exit 1
    }
done
grep -Fq 'ENTER — ЛОКАЛЬНОЕ МЕНЮ (N: НАСТРОЙКА СЕТИ — ЗАДАТЬ АДРЕС)' \
    tests/console-host.c || {
    echo "ITEM-509 inc21: проба не печатает русскую подсказку" >&2
    exit 1
}
grep -Fq 'ЛОКАЛЬНЫЙ ВВОД НЕДОСТУПЕН: МЕНЮ ПОКАЗАНО ДЛЯ ЧТЕНИЯ' \
    tests/console-host.c || {
    echo "ITEM-509 inc21: проба не печатает русскую причину read-only" >&2
    exit 1
}

# 4. The increment is recorded where the next window reads it.
grep -Fq 'ITEM-509 increment 21' docs/hfoc-language-surfaces.md || {
    echo "ITEM-509 inc21: поверхности языка не называют инкремент" >&2
    exit 1
}
grep -Fq '509.7d-2a' docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-509 inc21: очередь не называет кусок 509.7d" >&2
    exit 1
}
