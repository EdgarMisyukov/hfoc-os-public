#!/usr/bin/env bash
# ITEM-509 increment 22: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-509 increment 22 (509.7d-2b): the console's field labels read the one
# catalog in the console's published language.  The English column is byte for
# byte the literals, leading and trailing spaces included, so the QEMU proofs
# that grep the English serial lines (`test-qemu-irqs.sh`, the exactness tests)
# do not move.  The values beside the labels stay English machine text.
for item509_inc22_file in \
    kernel/src/console.c \
    kernel/src/appliance_language.c \
    tests/console-host.c \
    tests/appliance-language-host.c \
    docs/hfoc-language-surfaces.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc22_file ]] || {
        echo "ITEM-509 inc22: отсутствует $item509_inc22_file" >&2
        exit 1
    }
done

# 1. The catalog carries the thirteen labels, English verbatim (the spaces are
#    part of the literal) and Russian beside them.
for item509_inc22_key in \
    '{"console.label.uptime", "UPTIME: ", "ВРЕМЯ РАБОТЫ: "}' \
    '{"console.label.ticks", "TICKS: ", "ТИКИ: "}' \
    '{"console.label.if", "IF: ", "ФЛАГ ПРЕРЫВАНИЙ: "}' \
    '"console.label.pic_master_mask", " PIC MASTER MASK: "' \
    '" МАСКА PIC MASTER: "' \
    '{"console.label.pic_slave_mask", " SLAVE MASK: ", " МАСКА PIC SLAVE: "}' \
    '{"console.label.timer_irq", " TIMER IRQ: ", " IRQ ТАЙМЕРА: "}' \
    '{"console.label.irq_keyboard", " KEYBOARD IRQ: ", " IRQ КЛАВИАТУРЫ: "}' \
    '{"console.label.keyboard_irq", "KEYBOARD IRQ: ", "IRQ КЛАВИАТУРЫ: "}' \
    '"console.label.last_scancode", " LAST SCANCODE: "' \
    '" ПОСЛЕДНИЙ СКАНКОД: "' \
    '{"console.label.usable_memory", "USABLE MEMORY: ", "ДОСТУПНАЯ ПАМЯТЬ: "}' \
    '{"console.label.power", "POWER: ", "ПИТАНИЕ: "}' \
    '"console.label.unknown_command", "UNKNOWN COMMAND: "' \
    '"НЕИЗВЕСТНАЯ КОМАНДА: "' \
    '{"console.label.management", "MANAGEMENT: ", "УПРАВЛЕНИЕ: "}'; do
    grep -Fq -- "$item509_inc22_key" kernel/src/appliance_language.c || {
        echo "ITEM-509 inc22: каталог не несёт: $item509_inc22_key" >&2
        exit 1
    }
done

# 2. The console reads them through one helper in the published language, and
#    the English literals are gone from the console, so key and label cannot
#    drift.
grep -Fq 'static const char *console_label_text(const char *key)' \
    kernel/src/console.c || {
    echo "ITEM-509 inc22: нет помощника подписей консоли" >&2
    exit 1
}
grep -Fq 'appliance_language_string(console_active_language(), key)' \
    kernel/src/console.c || {
    echo "ITEM-509 inc22: помощник не читает действующий язык" >&2
    exit 1
}
grep -Fq '"CONSOLE TRANSLATION MISSING"' kernel/src/console.c || {
    echo "ITEM-509 inc22: нет именованного отказа перевода" >&2
    exit 1
}
for item509_inc22_call in \
    'console_label_text("console.label.uptime")' \
    'console_label_text("console.label.ticks")' \
    'console_label_text("console.label.if")' \
    'console_label_text("console.label.pic_master_mask")' \
    'console_label_text("console.label.pic_slave_mask")' \
    'console_label_text("console.label.timer_irq")' \
    'console_label_text("console.label.irq_keyboard")' \
    'console_label_text("console.label.keyboard_irq")' \
    'console_label_text("console.label.last_scancode")' \
    'console_label_text("console.label.usable_memory")' \
    'console_label_text("console.label.power")' \
    'console_label_text("console.label.unknown_command")' \
    'console_label_text("console.label.management")'; do
    grep -Fq -- "$item509_inc22_call" kernel/src/console.c || {
        echo "ITEM-509 inc22: подпись не читает каталог: $item509_inc22_call" >&2
        exit 1
    }
done
for item509_inc22_gone in \
    '"UPTIME: "' \
    '"TICKS: "' \
    '"IF: "' \
    '" PIC MASTER MASK: "' \
    '" SLAVE MASK: "' \
    '" TIMER IRQ: "' \
    '" KEYBOARD IRQ: "' \
    '"KEYBOARD IRQ: "' \
    '" LAST SCANCODE: "' \
    '"USABLE MEMORY: "' \
    '"POWER: "' \
    '"UNKNOWN COMMAND: "' \
    '"MANAGEMENT: "'; do
    if grep -Fq -- "$item509_inc22_gone" kernel/src/console.c; then
        echo "ITEM-509 inc22: английская подпись осталась литералом:" \
            "$item509_inc22_gone" >&2
        exit 1
    fi
done

# 3. The proof drives a Russian console through the labels and keeps the values
#    English.
for item509_inc22_check in \
    'the uptime label speaks the chosen language' \
    'the value beside the translated label stays English' \
    'the English label is gone from a Russian console' \
    'the memory label speaks the chosen language' \
    'the unknown-command label speaks the chosen language' \
    'the English unknown-command label is gone too'; do
    grep -Fq -- "$item509_inc22_check" tests/console-host.c || {
        echo "ITEM-509 inc22: проба консоли не утверждает: $item509_inc22_check" >&2
        exit 1
    }
done
grep -Fq 'ВРЕМЯ РАБОТЫ: ' tests/console-host.c || {
    echo "ITEM-509 inc22: проба не печатает русскую подпись" >&2
    exit 1
}
grep -Fq 'НЕИЗВЕСТНАЯ КОМАНДА: nosuchthing' tests/console-host.c || {
    echo "ITEM-509 inc22: проба не печатает русский отказ команды" >&2
    exit 1
}
grep -Fq '321 measured' tests/appliance-language-host.c || {
    echo "ITEM-509 inc22: проба каталога не называет измеренное число" >&2
    exit 1
}

# 4. The QEMU proofs that read the English serial lines are untouched: they are
#    the machine view, and the English column is byte for byte the old literal.
grep -Fq 'IF: (ON|OFF) PIC MASTER MASK: ' scripts/test-qemu-irqs.sh || {
    echo "ITEM-509 inc22: QEMU-проба IRQ потеряла английскую строку" >&2
    exit 1
}

# 5. The increment is recorded where the next window reads it.
grep -Fq 'ITEM-509 increment 22' docs/hfoc-language-surfaces.md || {
    echo "ITEM-509 inc22: поверхности языка не называют инкремент" >&2
    exit 1
}
grep -Fq '509.7d-2b' docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-509 inc22: очередь не называет кусок 509.7d" >&2
    exit 1
}
