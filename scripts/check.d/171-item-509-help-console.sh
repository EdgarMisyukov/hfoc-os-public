#!/usr/bin/env bash
# ITEM-509 increment 7: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-509 increment 7 is 509.5: the help content and the console renderer.
# One content, two renderers - `help` prints the table of contents, `help
# <topic>` one topic, `help all` every topic, and the strings live in the one
# language catalog so the panel can render the same four fields in increment 8.
# The claims pinned here:
#
#   * the help vocabulary is a pure classifier like the language one: the word
#     and the id fold, one space separates one argument, and an id no topic
#     carries is a named refusal, never a silent empty answer;
#   * the nine topics the item names exist, and each one has a title, a
#     summary, a body and its related commands and sections in both languages;
#   * the console renderer writes every line through the IRQ-safe atomic path,
#     so an IRQ trace record cannot split a help line, and a catalog that is
#     incomplete is a visible machine refusal, not half a topic;
#   * the command vocabulary line `COMMANDS: HELP ...` stays on the answer, so
#     every existing proof and operator reading survives;
#   * the code-point bound grew with the measured catalog (70 -> 127 code
#     points), because a help body is the first Russian prose in the catalog;
#     the glyph proof still fails on any code point it cannot draw.
for item509_inc7_file in \
    kernel/include/console_help_command.h \
    kernel/include/appliance_language.h \
    kernel/src/appliance_language.c \
    kernel/src/console.c \
    tests/console-host.c \
    tests/appliance-language-host.c \
    scripts/test-qemu-appliance-console.sh \
    docs/hfoc-appliance-boot-console.md \
    docs/hfoc-language-surfaces.md \
    docs/ROADMAP.md \
    docs/ARCHITECTURE.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc7_file ]] || {
        echo "ITEM-509 inc7: отсутствует $item509_inc7_file" >&2
        exit 1
    }
done

# 1. The classifier: one word, five answers, and the id is looked up in the one
#    catalogue rather than matched by a second table.
for item509_inc7_classifier in \
    '#define CONSOLE_HELP_COMMAND_WORD "help"' \
    '#define CONSOLE_HELP_COMMAND_ALL_WORD "all"' \
    '#define CONSOLE_HELP_LINE_MAX 192U' \
    'CONSOLE_HELP_COMMAND_NONE = 0' \
    'CONSOLE_HELP_COMMAND_LIST = 1' \
    'CONSOLE_HELP_COMMAND_TOPIC = 2' \
    'CONSOLE_HELP_COMMAND_ALL = 3' \
    'CONSOLE_HELP_COMMAND_REFUSE = 4' \
    'console_help_command_classify(const char *line, size_t length, size_t *topic)' \
    'appliance_help_topic_find(argument, argument_length)'; do
    grep -Fq -- "$item509_inc7_classifier" \
        kernel/include/console_help_command.h || {
        echo "ITEM-509 inc7: классификатор не несёт: $item509_inc7_classifier" >&2
        exit 1
    }
done

# 2. The catalogue: one field enum, one accessor set, and the nine topics with
#    their four key strings each.
for item509_inc7_header in \
    'APPLIANCE_HELP_FIELD_TITLE = 0' \
    'APPLIANCE_HELP_FIELD_SUMMARY = 1' \
    'APPLIANCE_HELP_FIELD_BODY = 2' \
    'APPLIANCE_HELP_FIELD_RELATED = 3' \
    'appliance_help_topic_find(const char *id, size_t length)' \
    'appliance_help_topic_string(size_t index,'; do
    grep -Fq -- "$item509_inc7_header" kernel/include/appliance_language.h || {
        echo "ITEM-509 inc7: каталог не объявляет: $item509_inc7_header" >&2
        exit 1
    }
done
for item509_inc7_topic in install network storage account certificates \
    names panel recovery records; do
    grep -Fq -- "\"$item509_inc7_topic\"," \
        kernel/src/appliance_language.c || {
        echo "ITEM-509 inc7: каталог потерял тему $item509_inc7_topic" >&2
        exit 1
    }
    for item509_inc7_field in title summary body related; do
        grep -Fq -- "\"help.topic.$item509_inc7_topic.$item509_inc7_field\"" \
            kernel/src/appliance_language.c || {
            echo "ITEM-509 inc7: у темы $item509_inc7_topic нет $item509_inc7_field" >&2
            exit 1
        }
    done
done
grep -Fq '{"help.topic.install.title", "Installation and disk choice",' \
    kernel/src/appliance_language.c || {
    echo "ITEM-509 inc7: заголовок темы установки не в каталоге" >&2
    exit 1
}
grep -Fq '"Установка и выбор диска"' kernel/src/appliance_language.c || {
    echo "ITEM-509 inc7: русский заголовок темы установки не в каталоге" >&2
    exit 1
}
# The bound is the measured catalog, not a guess: the comment names both
# measurements next to the constant.
grep -Fq '#define APPLIANCE_LANGUAGE_CODEPOINT_MAX 192U' \
    kernel/include/appliance_language.h || {
    echo "ITEM-509 inc7: потолок инвентаря кодпойнтов не пере-измерен" >&2
    exit 1
}
grep -Fq '70 code points before the help topics and' \
    kernel/include/appliance_language.h || {
    echo "ITEM-509 inc7: потолок не называет измерение" >&2
    exit 1
}

# 3. The console renderer: full lines through the atomic path, the named
#    refusal, and the dispatch that replaces the old one-line `help`.
for item509_inc7_render in \
    'static void console_help_command(enum console_help_command_kind kind,' \
    'console_write_atomic(line);' \
    'missing = appliance_language_string(console_language,' \
    '"help.topic_missing"' \
    'usage = appliance_language_string(console_language, "help.usage");' \
    '"[CONSOLE] help unavailable reason=CATALOG_INCOMPLETE"' \
    'console_help_command(help_kind, help_topic);' \
    'help_kind = console_help_command_classify(input_line, input_length,'; do
    grep -Fq -- "$item509_inc7_render" kernel/src/console.c || {
        echo "ITEM-509 inc7: рендерер не несёт: $item509_inc7_render" >&2
        exit 1
    }
done
if grep -Fq 'strcmp(input_line, "help") == 0' kernel/src/console.c; then
    echo "ITEM-509 inc7: старая однострочная команда help осталась" >&2
    exit 1
fi
grep -Fq 'COMMANDS: HELP CLEAR SCROLL VERSION UPTIME TICKS IRQ KBD' \
    kernel/src/console.c || {
    echo "ITEM-509 inc7: строка словаря команд потеряна" >&2
    exit 1
}

# 4. The proofs: the console drives all four answers in both languages, and the
#    catalogue proof measures the nine topics.
grep -Fq 'static void test_help_command(void)' tests/console-host.c || {
    echo "ITEM-509 inc7: host-проба консоли не несёт проверку help" >&2
    exit 1
}
grep -Fq 'test_help_command();' tests/console-host.c || {
    echo "ITEM-509 inc7: проверка help не вызвана" >&2
    exit 1
}
grep -Fq 'static void check_help_topics(void)' tests/appliance-language-host.c || {
    echo "ITEM-509 inc7: проба каталога не несёт проверку тем" >&2
    exit 1
}
grep -Fq 'check_help_topics();' tests/appliance-language-host.c || {
    echo "ITEM-509 inc7: проверка тем не вызвана" >&2
    exit 1
}
grep -Fq '"install", "network", "storage", "account", "certificates",' \
    tests/appliance-language-host.c || {
    echo "ITEM-509 inc7: проба каталога не называет обязательные темы" >&2
    exit 1
}

# 5. The live-console proof types both help spellings and reads the answer in
#    whichever language the machine ended up in.
for item509_inc7_qemu in \
    'for character in "help":' \
    'for character in "help install":' \
    'HFOC-QEMU-MARKER-TIMEOUT marker="help table of contents"' \
    'HFOC-QEMU-MARKER-TIMEOUT marker="help topic output"' \
    '"Installation and disk choice"' \
    'the help table of contents lost topic' \
    'HFOC_APPLIANCE_CONSOLE_TEST_TIMEOUT:-150'; do
    grep -Fq -- "$item509_inc7_qemu" \
        scripts/test-qemu-appliance-console.sh || {
        echo "ITEM-509 inc7: QEMU-проба не несёт: $item509_inc7_qemu" >&2
        exit 1
    }
done

# 6. The documents record the increment, and the surface inventory keeps the
#    order: console renderer now, panel renderer next.
for item509_inc7_doc in docs/ROADMAP.md docs/ARCHITECTURE.md \
    docs/hfoc-language-surfaces.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md \
    docs/hfoc-appliance-boot-console.md; do
    grep -Fq 'ITEM-509 increment 7' "$item509_inc7_doc" || {
        echo "ITEM-509 inc7: $item509_inc7_doc не называет инкремент" >&2
        exit 1
    }
done
grep -Fq '509.6' docs/hfoc-language-surfaces.md || {
    echo "ITEM-509 inc7: инвентарь поверхностей не называет следующий шаг" >&2
    exit 1
}
