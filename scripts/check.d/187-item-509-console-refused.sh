#!/usr/bin/env bash
# ITEM-509 increment 24: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-509 increment 24 (509.7d-3a): the last two console sentences that were
# still English literals - the refusal the power command prints when the
# platform declares no mechanism, and the sentence the account conversation
# prints when one is already open - read the one catalog in the published
# language.  The `POWER:` label, the action name and the `[ADMIN-ENROLLMENT]`
# record marker stay English machine text, and the English column is byte for
# byte the old output.  The two branches cannot be reached by the host build as
# it stands (the platform-power guard compiles the first out, a live
# conversation captures the keyboard that reaches the second), so the slice
# also pins the two host hooks that call the same functions the kernel calls.
for item509_inc24_file in \
    kernel/src/console.c \
    kernel/include/console.h \
    kernel/src/appliance_language.c \
    tests/console-host.c \
    tests/appliance-language-host.c \
    docs/hfoc-language-surfaces.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc24_file ]] || {
        echo "ITEM-509 inc24: отсутствует $item509_inc24_file" >&2
        exit 1
    }
done

# 1. The catalog carries the two sentences, English verbatim (the leading space
#    and the trailing newline are part of the power literal) and Russian beside
#    them.
for item509_inc24_key in \
    '{"console.prose.power_refused",' \
    '" REFUSED: this platform declares no mechanism\n",' \
    '{"console.prose.credentials_busy",' \
    '"a credentials conversation is already open; press ENTER on an empty line "' \
    '"to cancel it\n",'; do
    grep -Fq -- "$item509_inc24_key" kernel/src/appliance_language.c || {
        echo "ITEM-509 inc24: каталог не несёт: $item509_inc24_key" >&2
        exit 1
    }
done
for item509_inc24_ru in \
    'ОТКАЗ: эта платформа не объявляет механизм' \
    'разговор об учётной записи уже открыт'; do
    grep -Fq -- "$item509_inc24_ru" kernel/src/appliance_language.c || {
        echo "ITEM-509 inc24: каталог не несёт русское значение: $item509_inc24_ru" >&2
        exit 1
    }
done

# 2. The console reads both sentences through the existing helper in the
#    published language, and the English literals are gone from the console.
grep -Fq 'console_local_menu_text("console.prose.power_refused")' \
    kernel/src/console.c || {
    echo "ITEM-509 inc24: отказ платформы не читает каталог" >&2
    exit 1
}
grep -Fq 'console_local_menu_text("console.prose.credentials_busy")' \
    kernel/src/console.c || {
    echo "ITEM-509 inc24: занятый разговор не читает каталог" >&2
    exit 1
}
grep -Fq 'static void console_power_refusal(const char *action)' \
    kernel/src/console.c || {
    echo "ITEM-509 inc24: нет общей функции отказа платформы" >&2
    exit 1
}
grep -Fq 'console_power_refusal(' kernel/src/console.c || {
    echo "ITEM-509 inc24: ветка отказа не вызывает общую функцию" >&2
    exit 1
}
for item509_inc24_gone in \
    '" REFUSED: this platform declares no mechanism\n"' \
    'a credentials conversation is already open'; do
    if grep -Fq -- "$item509_inc24_gone" kernel/src/console.c; then
        echo "ITEM-509 inc24: английская проза осталась литералом:" \
            "$item509_inc24_gone" >&2
        exit 1
    fi
done

# 3. The English machine text beside the prose is still there byte for byte:
#    the record marker in the console and the two catalog label/action pieces.
grep -Fq '"[ADMIN-ENROLLMENT] "' kernel/src/console.c || {
    echo "ITEM-509 inc24: маркер записи потерян" >&2
    exit 1
}
grep -Fq '{"console.label.power", "POWER: ", "ПИТАНИЕ: "}' \
    kernel/src/appliance_language.c || {
    echo "ITEM-509 inc24: английская подпись POWER потеряна" >&2
    exit 1
}

# 4. The two host hooks call the same functions the kernel calls, and the proof
#    drives the exact lines in both languages.
for item509_inc24_hook in \
    'void console_test_power_refusal(const char *action);' \
    'void console_test_admin_credentials_reenter(void);'; do
    grep -Fq -- "$item509_inc24_hook" kernel/include/console.h || {
        echo "ITEM-509 inc24: нет host-крючка: $item509_inc24_hook" >&2
        exit 1
    }
done
grep -Fq 'void console_test_power_refusal(const char *action) {' \
    kernel/src/console.c || {
    echo "ITEM-509 inc24: крючок отказа не вызывает общую функцию" >&2
    exit 1
}
grep -Fq 'void console_test_admin_credentials_reenter(void) {' \
    kernel/src/console.c || {
    echo "ITEM-509 inc24: крючок занятого разговора не заведён" >&2
    exit 1
}
for item509_inc24_check in \
    'the power refusal speaks the chosen language' \
    'the English power refusal is gone from a Russian console' \
    'the English power refusal is byte for byte the old line' \
    'the account conversation is open before the guard is exercised' \
    'the busy account conversation speaks the chosen language' \
    'the English busy sentence is gone from a Russian console'; do
    grep -Fq -- "$item509_inc24_check" tests/console-host.c || {
        echo "ITEM-509 inc24: проба консоли не утверждает: $item509_inc24_check" >&2
        exit 1
    }
done
grep -Fq -- 'ПИТАНИЕ: REBOOT ОТКАЗ: эта платформа не объявляет ' \
    tests/console-host.c || {
    echo "ITEM-509 inc24: проба не печатает русский отказ платформы" >&2
    exit 1
}
grep -Fq -- 'разговор об учётной записи уже открыт' tests/console-host.c || {
    echo "ITEM-509 inc24: проба не печатает русский занятый разговор" >&2
    exit 1
}
grep -Fq '330 measured' tests/appliance-language-host.c || {
    echo "ITEM-509 inc24: проба каталога не называет измеренное число" >&2
    exit 1
}

# 5. The increment is recorded where the next window reads it.
grep -Fq 'ITEM-509 increment 24' docs/hfoc-language-surfaces.md || {
    echo "ITEM-509 inc24: поверхности языка не называют инкремент" >&2
    exit 1
}
grep -Fq '509.7d-3a' docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-509 inc24: очередь не называет кусок 509.7d" >&2
    exit 1
}
