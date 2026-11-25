#!/usr/bin/env bash
# ITEM-460: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The console's own conversation reaches the durable journal.
#
# The owner's 2026-09-20 report: "I am tired of photographing the screen after
# every run; cannot the screen output go to the log".  The record funnel already
# handed every *published record* to the boot log, but the console's own
# conversation - the typed command and the answer it produced - went through
# console_write() and reached the screen and the serial stream only.  On a
# machine whose only output is the framebuffer that conversation existed for a
# few seconds; the admin-provision refusal the owner needed was lost under a
# stream of timer records, and even a photograph could not recover it.
#
# The claims pinned here are what the console does after the item:
#
#   * every command is journaled as `[CONSOLE] cmd="..."`, and every line of its
#     answer as `[CONSOLE] out="..."`, through the same boot-log binding the
#     published records use - and through a path that does not paint, so the
#     transcript cannot scroll the screen it exists to make optional;
#   * a password never reaches the journal: ITEM-506's account commands take no
#     argument and the hidden answer is never a command, so the ITEM-460
#     redaction rule that named `admin-provision`/`admin-login` was removed with
#     those commands instead of lingering as a claim that does no work;
#   * the enrollment state is published as
#     `[ADMIN-ENROLLMENT] v=1 state=... gate=... provisioned=... sessions=...
#     denied=... last_refusal=...` whenever the console changes it;
#   * the menu the machine offers and the entries it runs are journaled too;
#   * the host proof drives the real console and requires the command, the
#     answer, a multi-line answer, the redaction and the menu markers - and
#     requires the secret to be absent from everything the log observed.
for console_journal_file in \
    kernel/include/console.h \
    kernel/src/console.c \
    kernel/src/appliance_production_admin_enrollment.c \
    tests/console-host.c \
    tests/appliance-production-admin-enrollment-host.c \
    docs/hfoc-appliance-boot-console.md; do
    [[ -f $console_journal_file ]] || {
        echo "ITEM-460: отсутствует $console_journal_file" >&2
        exit 1
    }
done

# One journal-only record path, and it is the boot log's own observer - not a
# second write path and not the painting funnel.
grep -Fq 'static void console_journal_record(const char *text, size_t length) {' \
    kernel/src/console.c || {
    echo "ITEM-460: нет отдельного пути записей журнала" >&2
    exit 1
}
grep -Fq 'boot_log_binding.observe(line, strlen(line));' kernel/src/console.c || {
    echo "ITEM-460: запись журнала не доходит до наблюдателя boot log" >&2
    exit 1
}
if grep -Fq 'boot_log_binding.observe(line, strlen(line));' kernel/src/console.c &&
    awk '/^static void console_journal_record\(/ { inside = 1 }
         inside { print }
         inside && /^}/ { exit }' kernel/src/console.c |
        grep -Fq 'console_publish_record'; then
    echo "ITEM-460: журнал консоли идёт через рисующий путь" >&2
    exit 1
fi

# The command and the answer, one output record per line, bounded by the same
# sanitising the painted mirror uses.
grep -Fq '"[CONSOLE] cmd=\""' kernel/src/console.c || {
    echo "ITEM-460: команда не попадает в журнал" >&2
    exit 1
}
grep -Fq '"[CONSOLE] out=\""' kernel/src/console.c || {
    echo "ITEM-460: ответ команды не попадает в журнал" >&2
    exit 1
}
grep -Fq 'static void console_transcript_capture(char value) {' \
    kernel/src/console.c || {
    echo "ITEM-460: нет захвата вывода консоли" >&2
    exit 1
}
grep -Fq 'console_transcript_command(input_line);' kernel/src/console.c || {
    echo "ITEM-460: execute_command не записывает команду" >&2
    exit 1
}
grep -Fq 'console_transcript_end();' kernel/src/console.c || {
    echo "ITEM-460: вывод команды не завершается записью" >&2
    exit 1
}
grep -Fq 'if (!boot_log_bound || !console_record_accept(text, length, line)) {' \
    kernel/src/console.c || {
    echo "ITEM-460: журнальная запись не санитизируется тем же правилом" >&2
    exit 1
}

# A password never reaches the journal, and now by construction rather than by
# redaction: the account commands take no argument, the hidden answer never
# becomes a command, and the ITEM-460 redaction rule that named the removed
# `admin-provision`/`admin-login` keywords is gone with them.
if grep -Fq 'console_transcript_redact' kernel/src/console.c; then
    echo "ITEM-506: правило редактирования пережило удалённые команды" >&2
    exit 1
fi
grep -Fq 'test_console_journal_hidden_input' tests/console-host.c || {
    echo "ITEM-506: host-проба не проверяет скрытый ввод в журнале" >&2
    exit 1
}
grep -Fq 'the typed password is never written to the durable journal' \
    tests/console-host.c || {
    echo "ITEM-506: host-проба не требует отсутствия пароля в журнале" >&2
    exit 1
}

# The transcript is journaled only after the command ends, and a scroll report
# never lands between a pointer geometry record and its own newline: both were
# the 2026-09-20 `test-usb-mouse-menu-qemu` stale-geometry failure.
grep -Fq 'static void console_transcript_end(void) {' kernel/src/console.c || {
    echo "ITEM-460: нет отложенной публикации транскрипта" >&2
    exit 1
}
grep -Fq 'console_scroll_publishing || pointer_publishing ||' \
    kernel/src/console.c || {
    echo "ITEM-460: запись о прокрутке может сдвинуть геометрию указателя" >&2
    exit 1
}

# The enrollment state record gains the last refusal and is journaled by the
# console whenever the console changes it.
grep -Fq '" last_refusal="' kernel/src/appliance_production_admin_enrollment.c || {
    echo "ITEM-460: запись enrollment не называет последний отказ" >&2
    exit 1
}
grep -Fq 'console_journal_record(record, record_length);' kernel/src/console.c || {
    echo "ITEM-460: состояние enrollment не публикуется в журнал" >&2
    exit 1
}
grep -Fq '"[ADMIN-ENROLLMENT] state=UNAVAILABLE"' kernel/src/console.c || {
    echo "ITEM-460: недоступный enrollment не называет себя в журнале" >&2
    exit 1
}

# The menu and the offer are part of the durable conversation.  ITEM-478 moved
# the offer markers to the journal-only service funnel, which hands the same
# bounded line to `console_journal_record()`, so the journal shape is pinned
# through the funnel.
for console_journal_menu in \
    'console_service_line(record, strlen(record));' \
    'console_journal_record(record, length);' \
    '[LOCAL-MENU] open' \
    '[LOCAL-MENU] closed' \
    '[LOCAL-MENU] offer cancelled' \
    '[LOCAL-MENU] run action='; do
    grep -Fq -- "$console_journal_menu" kernel/src/console.c || {
        echo "ITEM-460: журнал не содержит меню/оффер:" \
            "$console_journal_menu" >&2
        exit 1
    }
done

# The host proof drives the real console and requires the whole contract,
# including that the password never reached the log.
for console_journal_proof in \
    'the typed command reaches the durable journal' \
    'the answer to the command reaches the durable journal too' \
    'the last line of a multi-line answer is journaled as its own' \
    'the durable command record redacts the removed secret argument' \
    'not even the first word of the password is journaled' \
    'the account change reaches the durable journal' \
    'the typed password is never written to the durable journal' \
    'the offer the machine made is journaled' \
    'opening the offered menu is journaled'; do
    grep -Fq -- "$console_journal_proof" tests/console-host.c || {
        echo "ITEM-460: host-проба не требует:" "$console_journal_proof" >&2
        exit 1
    }
done
grep -Fq 'the record says when nothing has been refused yet' \
    tests/appliance-production-admin-enrollment-host.c || {
    echo "ITEM-460: host-проба enrollment не требует last_refusal" >&2
    exit 1
}

# The proof is part of the canonical host suite: a drill nobody runs is not
# evidence.
# shellcheck disable=SC2016  # the Makefile text is what the search must match
grep -Fq '$(MAKE) test-console-host' Makefile || {
    echo "ITEM-460: host-проба консоли не зарегистрирована в Makefile" >&2
    exit 1
}

# The item is recorded where a maintainer looks.
for console_journal_doc in \
    'ITEM-460' \
    '[CONSOLE] cmd=' \
    '[ADMIN-ENROLLMENT]'; do
    grep -Fq -- "$console_journal_doc" docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-460:" \
            "$console_journal_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-460' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-460" >&2
    exit 1
}
grep -Fq 'ITEM-460' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-460" >&2
    exit 1
}
