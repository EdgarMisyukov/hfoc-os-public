#!/usr/bin/env bash
# ITEM-509 increment 23: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-509 increment 23 (509.7d-2c): the console's remaining operator prose -
# the honest BOOTLOG: UNAVAILABLE answer, the `. TYPE HELP.` sentence after an
# unknown command, and the two range suffixes of PTR and ARM - reads the one
# catalog in the console's published language through the two helpers that
# already exist.  The journal records, the service lines, the diagnostic
# triggers and the machine values stay English on purpose; this slice pins both
# the translated prose and the named English exception list.
for item509_inc23_file in \
    kernel/src/console.c \
    kernel/src/appliance_language.c \
    tests/console-host.c \
    tests/appliance-language-host.c \
    scripts/check.d/43-appliance-boot-log.sh \
    docs/hfoc-language-surfaces.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc23_file ]] || {
        echo "ITEM-509 inc23: отсутствует $item509_inc23_file" >&2
        exit 1
    }
done

# 1. The catalog carries the four prose entries, English verbatim (the newlines
#    are part of the literal) and Russian beside them.
for item509_inc23_key in \
    '{"console.prose.bootlog_unavailable", "BOOTLOG: UNAVAILABLE\n",' \
    '{"console.prose.unknown_command_hint", ". TYPE HELP.\n",' \
    '{"console.prose.ptr_range_suffix", "> sets it)", "> задаёт его)"}' \
    '{"console.prose.arm_range_suffix", "> sets the mouse depth)",'; do
    grep -Fq -- "$item509_inc23_key" kernel/src/appliance_language.c || {
        echo "ITEM-509 inc23: каталог не несёт: $item509_inc23_key" >&2
        exit 1
    }
done
for item509_inc23_ru in \
    'ЖУРНАЛ ЗАГРУЗКИ: НЕДОСТУПЕН' \
    '. НАБЕРИТЕ HELP.' \
    '> задаёт его)' \
    '> задаёт глубину мыши)'; do
    grep -Fq -- "$item509_inc23_ru" kernel/src/appliance_language.c || {
        echo "ITEM-509 inc23: каталог не несёт русское значение: $item509_inc23_ru" >&2
        exit 1
    }
done

# 2. The console reads the prose through the two existing helpers in the
#    published language, and the English literals are gone from the console.
for item509_inc23_call in \
    'console_local_menu_text("console.prose.bootlog_unavailable")' \
    'console_local_menu_text("console.prose.unknown_command_hint")' \
    'console_label_text("console.prose.ptr_range_suffix")' \
    'console_label_text("console.prose.arm_range_suffix")'; do
    grep -Fq -- "$item509_inc23_call" kernel/src/console.c || {
        echo "ITEM-509 inc23: проза не читает каталог: $item509_inc23_call" >&2
        exit 1
    }
done
grep -Fq 'static const char *console_local_menu_text(const char *key)' \
    kernel/src/console.c || {
    echo "ITEM-509 inc23: нет помощника прозы меню" >&2
    exit 1
}
grep -Fq 'static const char *console_label_text(const char *key)' \
    kernel/src/console.c || {
    echo "ITEM-509 inc23: нет помощника подписей" >&2
    exit 1
}
for item509_inc23_gone in \
    '"BOOTLOG: UNAVAILABLE\n"' \
    '". TYPE HELP.\n"' \
    '"> sets it)"' \
    '"> sets the mouse depth)"'; do
    if grep -Fq -- "$item509_inc23_gone" kernel/src/console.c; then
        echo "ITEM-509 inc23: английская проза осталась литералом:" \
            "$item509_inc23_gone" >&2
        exit 1
    fi
done

# 3. The named English exception: the service lines are journal records, and
#    the diagnostic triggers are the test tool - both stay byte for byte.
for item509_inc23_service in \
    'GDT IDT LEGACY PIC PIT PS2: OK\n' \
    'GDT IDT LEGACY PIC PIT PS2: ' \
    'TYPE HELP FOR COMMANDS, MENU FOR THE LOCAL MENU\n' \
    'LOCAL INPUT: ENABLED (USB HID KEYBOARD)\n' \
    'LOCAL INPUT: DISABLED (NO PS/2 KEYBOARD)\n' \
    'USE SERIAL OR THE MANAGEMENT PLANE\n' \
    'DOUBLE FAULT IST ENABLED\n' \
    'TRIGGERING DOUBLE FAULT VIA #PF GATE NOT-PRESENT...\n' \
    'TRIGGERING DIVIDE ERROR...\n' \
    'TRIGGERING PAGE FAULT...\n'; do
    grep -Fq -- "$item509_inc23_service" kernel/src/console.c || {
        echo "ITEM-509 inc23: исключение потеряно: $item509_inc23_service" >&2
        exit 1
    }
done

# 4. The proof drives a Russian console through the prose and keeps the records
#    and the service lines English.
for item509_inc23_check in \
    'the unknown-command hint speaks the chosen language' \
    'the English unknown-command hint is gone from a Russian console' \
    'the pointer range suffix is translated and the record stays English' \
    'the arm range suffix is translated and the record stays English' \
    'the unavailable boot log answers in the chosen language' \
    'the English unavailable answer is gone from a Russian console' \
    'the service banner stays English in a Russian console' \
    'the service invitation stays English in a Russian console'; do
    grep -Fq -- "$item509_inc23_check" tests/console-host.c || {
        echo "ITEM-509 inc23: проба консоли не утверждает: $item509_inc23_check" >&2
        exit 1
    }
done
grep -Fq -- '. НАБЕРИТЕ HELP.' tests/console-host.c || {
    echo "ITEM-509 inc23: проба не печатает русскую подсказку" >&2
    exit 1
}
grep -Fq -- 'ЖУРНАЛ ЗАГРУЗКИ: НЕДОСТУПЕН' tests/console-host.c || {
    echo "ITEM-509 inc23: проба не печатает русский отказ журнала" >&2
    exit 1
}
grep -Fq '325 measured' tests/appliance-language-host.c || {
    echo "ITEM-509 inc23: проба каталога не называет измеренное число" >&2
    exit 1
}

# 5. The QEMU proofs that read the English serial lines are untouched: they are
#    the machine view, and the English column is byte for byte the old literal.
grep -Fq 'UNKNOWN COMMAND: abc. TYPE HELP.' \
    scripts/test-qemu-keyboard-line-editing.sh || {
    echo "ITEM-509 inc23: QEMU-проба клавиатуры потеряла английскую строку" >&2
    exit 1
}
grep -Fq 'console.prose.bootlog_unavailable' \
    scripts/check.d/43-appliance-boot-log.sh || {
    echo "ITEM-509 inc23: срез журнала не пере-пинен на каталог" >&2
    exit 1
}

# 6. The increment is recorded where the next window reads it.
grep -Fq 'ITEM-509 increment 23' docs/hfoc-language-surfaces.md || {
    echo "ITEM-509 inc23: поверхности языка не называют инкремент" >&2
    exit 1
}
grep -Fq '509.7d-2c' docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-509 inc23: очередь не называет кусок 509.7d" >&2
    exit 1
}
