#!/usr/bin/env bash
# ITEM-509 increment 4: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-509 increment 4 gives the console the command that chooses the language.
# Increment 2 made the language durable and increment 3 made it drawable; this
# is the first surface that reads the setting, and the claims pinned here are
# the three the item asks for:
#
#   * one understandable command - `language` shows, `language en|ru` sets, and
#     a value the catalog does not name is refused with the usage line instead
#     of being treated as English;
#   * the answer is in the language that is active after the command, so a
#     successful switch confirms itself in the new language;
#   * a change is one durable write plus one English machine record, and a
#     machine whose record cannot be written says so and stays where it was.
#
# The classifier is pure (`console_language_command.h`), so the whole table of
# spellings is provable without a console; the command is then driven through
# the real console by the host proof, and through a live guest by the QEMU
# console proof.
for item509_inc4_file in \
    kernel/include/console_language_command.h \
    kernel/include/console.h \
    kernel/src/console.c \
    kernel/src/kernel.c \
    tests/console-host.c \
    tests/test-console-retype-host.sh \
    scripts/test-qemu-appliance-console.sh \
    docs/hfoc-appliance-boot-console.md \
    docs/hfoc-language-surfaces.md \
    docs/ROADMAP.md \
    docs/ARCHITECTURE.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc4_file ]] || {
        echo "ITEM-509 inc4: отсутствует $item509_inc4_file" >&2
        exit 1
    }
done

# 1. The classifier: one word, four answers, the word folds and the value does
#    not, and the value is the catalog's own parser - so `EN` and `de` are the
#    same refusal and `languages` is not this command.
for item509_inc4_classifier in \
    '#define CONSOLE_LANGUAGE_COMMAND_WORD "language"' \
    'CONSOLE_LANGUAGE_COMMAND_NONE = 0' \
    'CONSOLE_LANGUAGE_COMMAND_SHOW = 1' \
    'CONSOLE_LANGUAGE_COMMAND_SET = 2' \
    'CONSOLE_LANGUAGE_COMMAND_REFUSE = 3' \
    'static inline char console_language_command_fold(char value)' \
    'console_language_command_classify(const char *line, size_t length,' \
    'appliance_language_parse(argument, argument_length, &language) !=' \
    'return CONSOLE_LANGUAGE_COMMAND_SHOW;' \
    'return CONSOLE_LANGUAGE_COMMAND_REFUSE;' \
    'return CONSOLE_LANGUAGE_COMMAND_SET;'; do
    grep -Fq -- "$item509_inc4_classifier" \
        kernel/include/console_language_command.h || {
        echo "ITEM-509 inc4: классификатор не несёт:" \
            "$item509_inc4_classifier" >&2
        exit 1
    }
done

# 2. The console: the binding, the published state, the dispatch and the
#    answers from the catalog (no new literals for the operator's text).
for item509_inc4_console in \
    '#include "console_language_command.h"' \
    'static const struct console_language_binding *language_binding;' \
    'void console_bind_language(const struct console_language_binding *binding)' \
    'void console_publish_language(enum appliance_language language, bool durable)' \
    'enum appliance_language console_active_language(void)' \
    'bool console_language_is_durable(void)' \
    'static void console_language_command(enum console_language_command_kind kind,' \
    'language_kind = console_language_command_classify(' \
    '} else if (language_kind != CONSOLE_LANGUAGE_COMMAND_NONE) {' \
    'console_language_write_show();' \
    'console_language_write_key("console.language.refused");' \
    'console_language_write_key("console.language.usage");' \
    'console_language_write_key("console.language.unavailable");' \
    'console_language_write_key("console.language.saved");' \
    'static void console_language_write_line(const char *text) {' \
    'console_write_atomic(line);' \
    'static void console_language_write_show(void) {'; do
    grep -Fq -- "$item509_inc4_console" kernel/src/console.c || {
        echo "ITEM-509 inc4: консоль не несёт:" "$item509_inc4_console" >&2
        exit 1
    }
done
grep -Fq 'struct console_language_binding {' kernel/include/console.h || {
    echo "ITEM-509 inc4: console.h не объявляет привязку языка" >&2
    exit 1
}
grep -Fq 'bool (*write)(void *context, enum appliance_language language);' \
    kernel/include/console.h || {
    echo "ITEM-509 inc4: привязка не пишет запись" >&2
    exit 1
}
grep -Fq 'void console_publish_language(enum appliance_language language,' \
    kernel/include/console.h || {
    echo "ITEM-509 inc4: console.h не публикует язык" >&2
    exit 1
}

# 3. The records: one English machine line per outcome, with the reason named
#    when nothing was written.
for item509_inc4_record in \
    'static const char prefix[] = "[CONSOLE] language=";' \
    'static const char refused[] = " refused reason=";' \
    'console_language_record_code(requested, "UNAVAILABLE");' \
    'console_language_record_code(requested, "WRITE_FAILED");' \
    'console_language_record(value, value_length, "UNKNOWN_LANGUAGE");' \
    'console_language_record_code(requested, NULL);'; do
    grep -Fq -- "$item509_inc4_record" kernel/src/console.c || {
        echo "ITEM-509 inc4: запись команды не полна:" \
            "$item509_inc4_record" >&2
        exit 1
    }
done
# The console changes its own language only after the owner's write said yes.
grep -Fq 'console_language = requested;' kernel/src/console.c || {
    echo "ITEM-509 inc4: консоль не запоминает записанный язык" >&2
    exit 1
}
if grep -Fq 'appliance_persistent_config_set_language' kernel/src/console.c; then
    echo "ITEM-509 inc4: консоль сама пишет системную запись" >&2
    exit 1
fi

# 4. The production binding and the publication: the boot hands the console the
#    record's language and the one write, and a machine without a loaded record
#    publishes no binding.
grep -Fq 'static bool console_language_write(void *context,' \
    kernel/src/kernel.c || {
    echo "ITEM-509 inc4: kernel.c не несёт адаптер записи" >&2
    exit 1
}
grep -Fq 'publish_console_language();' kernel/src/kernel.c || {
    echo "ITEM-509 inc4: boot не публикует язык" >&2
    exit 1
}
grep -Fq 'console_bind_language(NULL);' kernel/src/kernel.c || {
    echo "ITEM-509 inc4: boot не снимает привязку без записи" >&2
    exit 1
}
grep -Fq 'console_bind_language(&console_language_binding);' \
    kernel/src/kernel.c || {
    echo "ITEM-509 inc4: boot не публикует привязку записи" >&2
    exit 1
}

# 5. The help line names the command, and the retype proof's own copy of that
#    line does not drift from it.
grep -Fq 'CREDENTIALS LOGIN LANGUAGE REBOOT' kernel/src/console.c || {
    echo "ITEM-509 inc4: строка справки не называет LANGUAGE" >&2
    exit 1
}
grep -Fq 'CREDENTIALS LOGIN LANGUAGE' tests/test-console-retype-host.sh || {
    echo "ITEM-509 inc4: копия строки справки в retype-пробе разъехалась" >&2
    exit 1
}

# 6. The host proof drives the real console: the classifier table, the show
#    answer, the switch and its record, the refusal, the unavailable machine and
#    the failed write.
for item509_inc4_proof in \
    'console_language_command_classify(table[index].line, length,' \
    'the language command shows the active language' \
    'a language switch writes the record exactly once' \
    'the confirmation is printed in the language just chosen' \
    'the switch leaves one record naming the language' \
    'a refused language writes nothing' \
    'the refusal names the rule instead of falling back to English' \
    'the refusal carries the usage line' \
    'the console says the setting is unavailable instead of pretending' \
    'a failed write does not move the console'"'"'s language'; do
    grep -Fq -- "$item509_inc4_proof" tests/console-host.c || {
        echo "ITEM-509 inc4: host-проба не требует:" \
            "$item509_inc4_proof" >&2
        exit 1
    }
done
for item509_inc4_record_proof in \
    '[CONSOLE] language=ru' \
    '[CONSOLE] language=de refused reason=UNKNOWN_LANGUAGE' \
    '[CONSOLE] language=ru refused reason=UNAVAILABLE' \
    '[CONSOLE] language=ru refused reason=WRITE_FAILED'; do
    grep -Fq -- "$item509_inc4_record_proof" tests/console-host.c || {
        echo "ITEM-509 inc4: проба не требует записи:" \
            "$item509_inc4_record_proof" >&2
        exit 1
    }
done

# 7. The QEMU console proof types the command on a live guest, accepts exactly
#    the two truthful answers and requires that no glyph was missing while the
#    console printed Russian.
for item509_inc4_qemu in \
    'for character in "language":' \
    'LANGUAGE: EN' \
    'LANGUAGE IS NOT AVAILABLE' \
    'switch_outcome = "saved"' \
    'if "[GLYPH-MISSING]" in serial_text():'; do
    grep -Fq -- "$item509_inc4_qemu" \
        scripts/test-qemu-appliance-console.sh || {
        echo "ITEM-509 inc4: QEMU-проба не требует:" \
            "$item509_inc4_qemu" >&2
        exit 1
    }
done
grep -Fq 'u0421\u041e\u0425\u0420\u0410\u041d\u0401\u041d' \
    scripts/test-qemu-appliance-console.sh || {
    echo "ITEM-509 inc4: QEMU-проба не ждёт русского подтверждения" >&2
    exit 1
}

# 8. The documents record the increment and its boundary.
for item509_inc4_doc in docs/ROADMAP.md docs/ARCHITECTURE.md \
    docs/hfoc-language-surfaces.md docs/hfoc-appliance-boot-console.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    grep -Fq 'ITEM-509 increment 4' "$item509_inc4_doc" || {
        echo "ITEM-509 inc4: $item509_inc4_doc не называет инкремент" >&2
        exit 1
    }
done
grep -Fq 'UNKNOWN LANGUAGE: use en or ru' docs/hfoc-appliance-boot-console.md || {
    echo "docs/hfoc-appliance-boot-console.md не фиксирует отказ" >&2
    exit 1
}
grep -Fq 'LANGUAGE IS NOT AVAILABLE' docs/hfoc-appliance-boot-console.md || {
    echo "docs/hfoc-appliance-boot-console.md не фиксирует недоступность" >&2
    exit 1
}
