#!/usr/bin/env bash
# ITEM-478: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The screen keeps only what the operator asked for (СРОЧНО-27, round 16).
#
# The owner's 2026-09-25 review asked that all service information leave the
# screen: the product banner and the prompt, the operator's own typing and the
# answers to it, the local menu and an input error are the only things a fresh
# screen shows.  ITEM-477 had moved the four periodic records to the
# journal-only mirror; this item moves every remaining service record - boot,
# BOOT-CHAIN, PRODUCTION-*, APPLIANCE-*, install, power, KBD, MOUSE,
# USB/XHCI-PORT, FB, the LOCAL-MENU markers and the readiness banner - to the
# same funnel.  The serial stream and the durable journal keep every line; the
# framebuffer keeps the banner's one row.  `[CONSOLE] boot paint lines=<n>
# service_lines=<m>` is published once at the first prompt, so the QEMU proof
# reads the boot screen's own count before a keystroke can add to it.

for item478_file in \
    kernel/src/console.c \
    kernel/src/kernel.c \
    kernel/src/xhci.c \
    kernel/src/production_installer_layout.c \
    kernel/src/production_installer_boot_stage.c \
    kernel/src/production_install_flow.c \
    kernel/src/production_installer_preserve.c \
    kernel/src/platform_power.c \
    kernel/src/platform_reset.c \
    tests/console-host.c \
    scripts/test-qemu-production-storage-boot.sh \
    docs/hfoc-appliance-boot-console.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item478_file ]] || {
        echo "ITEM-478: отсутствует $item478_file" >&2
        exit 1
    }
done

# ------------------------------------------------- the service-line funnel --
grep -Fq 'static void console_service_line(const char *record, size_t length);' \
    kernel/src/console.c || {
    echo "ITEM-478: служебная воронка не объявлена" >&2
    exit 1
}
grep -Fq 'static void console_service_line(const char *record, size_t length) {' \
    kernel/src/console.c || {
    echo "ITEM-478: служебная воронка не реализована" >&2
    exit 1
}
item478_service_body=$(
    awk '/^static void console_service_line\(const char \*record, size_t length\) \{/{f=1} f{print} f&&/^}/{exit}' \
        kernel/src/console.c
)
for item478_call in \
    'serial_sequence_begin();' \
    'serial_putc(record[index]);' \
    'console_publish_journal(record, length);'; do
    grep -Fq -- "$item478_call" <<<"$item478_service_body" || {
        echo "ITEM-478: служебная воронка не пишет:" "$item478_call" >&2
        exit 1
    }
done
if grep -Fq 'console_write' <<<"$item478_service_body"; then
    echo "ITEM-478: служебная воронка всё ещё рисует" >&2
    exit 1
fi

# --------------------------------------------- the boot screen's own count --
grep -Fq '"[CONSOLE] boot paint lines="' kernel/src/console.c || {
    echo "ITEM-478: нет записи о числе нарисованных строк загрузки" >&2
    exit 1
}
grep -Fq '" service_lines="' kernel/src/console.c || {
    echo "ITEM-478: запись загрузки не называет служебные строки" >&2
    exit 1
}
# The banner is the one painted machine-origin line; the readiness lines are
# stored, not painted.
grep -Fq 'console_write("HFOC OS 1.0.0 INITIALISED\n");' \
    kernel/src/console.c || {
    echo "ITEM-478: баннер продукта потерян" >&2
    exit 1
}
grep -Fq 'console_service_line("GDT IDT LEGACY PIC PIT PS2: OK\n",' \
    kernel/src/console.c || {
    echo "ITEM-478: строка готовности всё ещё рисуется" >&2
    exit 1
}
grep -Fq 'console_service_line("DOUBLE FAULT IST ENABLED\n",' \
    kernel/src/console.c || {
    echo "ITEM-478: строка готовности прерываний всё ещё рисуется" >&2
    exit 1
}
# The local menu's own offer stays the UI the machine may show on its own.
grep -Fq '"console.menu.offer_idle"' kernel/src/console.c || {
    echo "ITEM-478: предложение меню потеряно" >&2
    exit 1
}
grep -Fq 'console_service_line("[LOCAL-MENU] open\n",' \
    kernel/src/console.c || {
    echo "ITEM-478: маркер меню всё ещё рисуется" >&2
    exit 1
}

# ------------------------------------- every service record uses the journal --
if grep -Fq 'console_record_begin();' kernel/src/kernel.c kernel/src/xhci.c; then
    echo "ITEM-478: служебная запись ядра или драйвера всё ещё рисуется" >&2
    exit 1
fi
for item478_service_file in \
    kernel/src/production_installer_layout.c \
    kernel/src/production_installer_boot_stage.c \
    kernel/src/production_install_flow.c \
    kernel/src/production_installer_preserve.c \
    kernel/src/platform_power.c \
    kernel/src/platform_reset.c; do
    if grep -Fq 'console_publish_record(' "$item478_service_file"; then
        echo "ITEM-478: $item478_service_file всё ещё рисует служебную запись" >&2
        exit 1
    fi
done
grep -Fq 'console_publish_journal(record, length);' kernel/src/console.c || {
    echo "ITEM-478: [FB] не уходит в журнал" >&2
    exit 1
}

# ------------------------------------------------------------- host proofs --
for item478_claim in \
    'the product banner is painted' \
    'the readiness banner is not painted' \
    'the readiness banner still reaches the serial stream' \
    'the readiness banner still reaches the durable journal' \
    'the boot screen painted only the banner' \
    'the menu offer marker is not painted' \
    "the menu's own offer is still painted as the UI" \
    "the operator's command still paints its answer" \
    'the display geometry is not painted for the operator'; do
    grep -Fq -- "$item478_claim" tests/console-host.c || {
        echo "ITEM-478: host-проба не утверждает:" "$item478_claim" >&2
        exit 1
    }
done
grep -Fq 'static void test_service_lines_stay_off_the_screen(void) {' \
    tests/console-host.c || {
    echo "ITEM-478: host-пробы экрана нет" >&2
    exit 1
}
grep -Fq 'test_service_lines_stay_off_the_screen();' tests/console-host.c || {
    echo "ITEM-478: host-проба экрана не вызывается" >&2
    exit 1
}

# --------------------------------------------------------------- QEMU proof --
grep -Fq 'r"\[CONSOLE\] boot paint lines=(\d+) service_lines=(\d+)"' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-478: QEMU-проба не читает запись загрузки" >&2
    exit 1
}
grep -Fq 'a service line was painted during boot' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-478: QEMU-проба не ловит возврат служебной строки на экран" >&2
    exit 1
}
grep -Fq 'service lines kept off the screen' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-478: QEMU-проба не публикует число служебных строк" >&2
    exit 1
}

# ------------------------------------------------------------- the documents --
for item478_doc in 'ITEM-478' 'console_service_line' 'paint lines='; do
    grep -Fq -- "$item478_doc" docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-478:" \
            "$item478_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-478' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-478" >&2
    exit 1
}
grep -Fq 'ITEM-478' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-478" >&2
    exit 1
}
