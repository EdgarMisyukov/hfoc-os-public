#!/usr/bin/env bash
# ITEM-512: contracted slice of the HFOC OS check suite. Sourced in pinned order
#           by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-512: the console's command vocabulary is case-insensitive for the whole
# chain, not only for the commands that happened to call `console_command_is`.
# The gap surfaced through the two-controller QEMU proof: its keyboard wake sends
# modifier-only `sendkey shift` reports, and a dropped release delivers `MENU`,
# which the chain refused as `UNKNOWN COMMAND` - a definite assertion instead of
# a retryable timeout.  A person with Caps Lock on types the same line, so the
# proof and the operator want the same rule: every command name folds ASCII
# case, every argument keeps its own case (a language value is still exactly
# `en` or `ru`).
for item512_file in \
    kernel/src/console.c \
    kernel/include/console_language_command.h \
    tests/console-host.c \
    scripts/test-qemu-usb-two-controllers.sh \
    scripts/hfoc_console_command.py \
    scripts/check.d/12-deterministic-backup-stream.sh \
    scripts/check.d/13-bounded-kernel-heap.sh \
    scripts/check.d/43-appliance-boot-log.sh \
    scripts/check.d/47-platform-power.sh \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item512_file ]] || {
        echo "ITEM-512: отсутствует $item512_file" >&2
        exit 1
    }
done

# 1. No command name is compared case-sensitively any more: the only `strcmp`s
#    left in console.c compare history and journal records, never `input_line`.
if grep -Eq 'strcmp\(input_line|strncmp\(input_line' kernel/src/console.c; then
    echo "ITEM-512: имя команды сравнивается с учётом регистра" >&2
    exit 1
fi
for item512_command in \
    'console_command_is(input_line, "menu")' \
    'console_command_is(input_line, "clear")' \
    'console_command_is(input_line, "version")' \
    'console_command_is(input_line, "uptime")' \
    'console_command_is(input_line, "ticks")' \
    'console_command_is(input_line, "irq")' \
    'console_command_is(input_line, "kbd")' \
    'console_command_is(input_line, "mem")' \
    'console_command_is(input_line, "pmm")' \
    'console_command_is(input_line, "heap")' \
    'console_command_is(input_line, "about")' \
    'console_command_is(input_line, "appliance")' \
    'console_command_is(input_line, "bootlog")' \
    'console_command_is(input_line, "reboot")' \
    'console_command_is(input_line, "shutdown")' \
    'console_command_is(input_line, "appliance-boot")' \
    'console_command_is(input_line, "panic")' \
    'console_command_is(input_line, "divzero")' \
    'console_command_is(input_line, "pagefault")' \
    'console_command_is(input_line, "doublefault")' \
    'console_command_is(input_line, "vmmmap")'; do
    grep -Fq -- "$item512_command" kernel/src/console.c || {
        echo "ITEM-512: команда не свёрнута по регистру: $item512_command" >&2
        exit 1
    }
done

# 2. Only the name folds.  The helper folds ASCII case and nothing else, the
#    language value still goes through the catalog's exact parser, and the
#    command is still spelled out where the operator reads it.
grep -Fq 'static char console_ascii_fold(char value)' kernel/src/console.c || {
    echo "ITEM-512: свёртка регистра пропала" >&2
    exit 1
}
grep -Fq 'console_ascii_fold(*line) != console_ascii_fold(*command)' \
    kernel/src/console.c || {
    echo "ITEM-512: имя команды сравнивается не свёрткой" >&2
    exit 1
}
grep -Fq 'appliance_language_parse(argument, argument_length, &language)' \
    kernel/include/console_language_command.h || {
    echo "ITEM-512: значение языка перестало быть точным" >&2
    exit 1
}
grep -Fq 'ITEM-512: the rule is the whole chain' kernel/src/console.c || {
    echo "ITEM-512: правило цепочки не записано" >&2
    exit 1
}

# 3. The host proof drives the real chain in upper and mixed case and pins the
#    answers, not only the helper.
grep -Fq 'static void test_commands_match_whatever_case(void)' \
    tests/console-host.c || {
    echo "ITEM-512: проба регистра не заведена" >&2
    exit 1
}
grep -Fq 'test_commands_match_whatever_case();' tests/console-host.c || {
    echo "ITEM-512: проба регистра не запускается" >&2
    exit 1
}
for item512_proof in \
    'VERSION answers like version' \
    'VERSION is not refused' \
    'KBD answers like kbd' \
    'KBD is not refused' \
    'mixed-case TICKS answers like ticks' \
    'MENU opens the local menu like menu' \
    'MENU is not refused' \
    'the upper-case menu closes like the typed one'; do
    grep -Fq -- "$item512_proof" tests/console-host.c || {
        echo "ITEM-512: проба потеряла утверждение: $item512_proof" >&2
        exit 1
    }
done

# 4. The two-controller proof keeps its case-folding witness, and the premise it
#    documents - the console vocabulary is case-insensitive - is now true for the
#    two commands it types.
grep -Fq 'witness=str.lower' scripts/test-qemu-usb-two-controllers.sh || {
    echo "ITEM-512: проба двух контроллеров потеряла свидетеля" >&2
    exit 1
}
grep -Fq 'console_command_is' scripts/hfoc_console_command.py || {
    echo "ITEM-512: помощник не объясняет свёртку регистра" >&2
    exit 1
}

# 5. The slices that pinned the old case-sensitive spellings pin the folded ones.
grep -Fq 'console_command_is(input_line, "pmm")' \
    scripts/check.d/12-deterministic-backup-stream.sh || {
    echo "ITEM-512: срез 12 не пере-пинен" >&2
    exit 1
}
grep -Fq 'console_command_is(input_line, "heap")' \
    scripts/check.d/13-bounded-kernel-heap.sh || {
    echo "ITEM-512: срез 13 не пере-пинен" >&2
    exit 1
}
grep -Fq 'console_command_is(input_line, "bootlog")' \
    scripts/check.d/43-appliance-boot-log.sh || {
    echo "ITEM-512: срез 43 не пере-пинен" >&2
    exit 1
}
grep -Fq 'console_command_is(input_line, "reboot")' \
    scripts/check.d/47-platform-power.sh || {
    echo "ITEM-512: срез 47 не пере-пинен" >&2
    exit 1
}

# 6. The item is recorded where the next window reads it.
grep -Fq 'ITEM-512' docs/ARCHITECTURE.md || {
    echo "ITEM-512: ARCHITECTURE не называет пункт" >&2
    exit 1
}
grep -Fq 'ITEM-512' docs/ROADMAP.md || {
    echo "ITEM-512: ROADMAP не называет пункт" >&2
    exit 1
}
# ITEM-513 moved the queue's `## Current item` heading on to its own item, so
# this pin names the ITEM-512 section the queue keeps instead of the heading,
# which belongs to whichever item is running.
grep -A 3 '^## Item ITEM-512$' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md |
    grep -Fq 'ITEM-512' || {
    echo "ITEM-512: раздел очереди не называет ITEM-512" >&2
    exit 1
}
