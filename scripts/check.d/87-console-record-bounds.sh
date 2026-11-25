#!/usr/bin/env bash
# ITEM-481: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The durable-journal record bound, by construction (СРОЧНО-30, round 19).
#
# The 2026-09-25 hardware journal
# (`/srv/hfoc-example/queue/hw8-log-all.txt`) showed the `[PRODUCTION-LOOP]`
# heartbeat ending at `malformed...`: the console mirror assembles one line and
# `console_record_accept()` cuts anything past CONSOLE_RECORD_MAX (320) with a
# silent "...".  A counter that grows, or one more field, loses the tail of the
# record - and with it the evidence the record exists to carry.
#
# This item fixes the class, not one record:
#
#   * the console holds the assembled record whole - a buffer sized to the
#     worst case of the longest builder, with an explicit overflow flag - and
#     the CONSOLE_RECORD_MAX bound is enforced while it is assembled: a fragment
#     that would cross it records a break, so every part's worst case is inside
#     the bound.  Nothing is published until `console_mirror_finish()`, after
#     the serial sequence has ended, so the log's own `[BOOTLOG]` serial record
#     cannot split the line being written;
#   * the head keeps the record's own name and every continuation carries
#     `[<name>-TAIL]`, published in the same tick, so no field is dropped and no
#     journal line exceeds the bound;
#   * `console_publish_journal()` applies the same bound to a pre-built record
#     (the console transcript, a service line);
#   * the console counts a `[CONSOLE-BOUND]` violation if an assembled record
#     should ever outgrow its own worst-case buffer, so a future field cannot be
#     lost silently;
#   * the host proof drives the assembly and the pre-built path, and the QEMU
#     proof reads the durable journal for bounded parts, both sides of the
#     split, and the absence of `[CONSOLE-BOUND]`.

for item481_file in \
    kernel/include/console.h \
    kernel/src/console.c \
    kernel/src/xhci.c \
    kernel/src/appliance_boot_log_store.c \
    tests/console-host.c \
    scripts/test-qemu-production-storage-boot.sh \
    docs/hfoc-appliance-boot-console.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item481_file ]] || {
        echo "ITEM-481: отсутствует $item481_file" >&2
        exit 1
    }
done

# --------------------------------------------------- the named bound and state --
grep -Fq '#define CONSOLE_RECORD_U64_WORST 20U' kernel/src/console.c || {
    echo "ITEM-481: worst case десятичного счётчика не назван" >&2
    exit 1
}
grep -Fq '#define CONSOLE_RECORD_HEX64_WORST 18U' kernel/src/console.c || {
    echo "ITEM-481: worst case шестнадцатеричного счётчика не назван" >&2
    exit 1
}
grep -Fq 'static size_t mirror_worst_peak;' kernel/src/console.c || {
    echo "ITEM-481: пик worst-case не хранится" >&2
    exit 1
}
grep -Fq 'static size_t mirror_bound_violations;' kernel/src/console.c || {
    echo "ITEM-481: нарушения границы не считаются" >&2
    exit 1
}

# ---------------------------------------- the assembly and the bounded funnel --
grep -Fq '#define CONSOLE_RECORD_ASSEMBLY_MAX 2560U' kernel/src/console.c || {
    echo "ITEM-481: буфер сборки не покрывает worst-case самой длинной записи" >&2
    exit 1
}
grep -Fq 'static bool mirror_overflow;' kernel/src/console.c || {
    echo "ITEM-481: переполнение буфера сборки не отмечается" >&2
    exit 1
}
grep -Fq 'mirror_overflow = true;' kernel/src/console.c || {
    echo "ITEM-481: переполнение буфера сборки не выставляется" >&2
    exit 1
}
grep -Fq 'console_mirror_publish_journal();' kernel/src/console.c || {
    echo "ITEM-481: собранная запись не уходит в ограниченную воронку" >&2
    exit 1
}
grep -Fq 'static void console_mirror_break(void)' kernel/src/console.c || {
    echo "ITEM-481: точка разрыва записи не отмечается" >&2
    exit 1
}
grep -Fq 'mirror_worst = CONSOLE_RECORD_TAIL_WORST;' kernel/src/console.c || {
    echo "ITEM-481: worst-case продолжения не сбрасывается" >&2
    exit 1
}
grep -Fq 'static size_t console_record_split_parts;' kernel/src/console.c || {
    echo "ITEM-481: разнесение записи не считается" >&2
    exit 1
}
grep -Fq '++console_record_split_parts;' kernel/src/console.c || {
    echo "ITEM-481: хвост не считается при разнесении" >&2
    exit 1
}
grep -Fq 'console_publish_journal(record, length);' kernel/src/console.c || {
    echo "ITEM-481: служебная строка обходит ограниченную воронку" >&2
    exit 1
}

# ---------------------------------------------- the pre-built record splitter --
grep -Fq 'void console_publish_journal(const char *record, size_t length) {' \
    kernel/src/console.c || {
    echo "ITEM-481: прямая публикация не ограничена" >&2
    exit 1
}
grep -Fq 'if (effective <= CONSOLE_RECORD_MAX) {' kernel/src/console.c || {
    echo "ITEM-481: короткая прямая запись не проходит как есть" >&2
    exit 1
}
grep -Fq 'console_journal_record(record, CONSOLE_RECORD_MAX);' \
    kernel/src/console.c || {
    echo "ITEM-481: длинная прямая запись не режется на части" >&2
    exit 1
}

# ---------------------------- the builders that compose into a local buffer --
# A pre-built record is only split by the funnel if its own buffer holds the
# worst case; the three named shapes are sized to it and the xHCI diagnostic
# line marks an overflow instead of dropping the field silently.
grep -Fq '#define XHCI_DIAG_LINE_MAX 512U' kernel/src/xhci.c || {
    echo "ITEM-481: буфер диагностики xHCI не покрывает worst-case" >&2
    exit 1
}
grep -Fq 'bool overflow;' kernel/src/xhci.c || {
    echo "ITEM-481: переполнение диагностики xHCI не отмечается" >&2
    exit 1
}
grep -Fq 'line->overflow = true;' kernel/src/xhci.c || {
    echo "ITEM-481: переполнение диагностики xHCI не выставляется" >&2
    exit 1
}
grep -Fq '"[XHCI-DIAG] overflow=1 truncated=1"' kernel/src/xhci.c || {
    echo "ITEM-481: переполнение диагностики xHCI не публикуется" >&2
    exit 1
}
grep -Fq 'char record[1024U];' kernel/src/console.c || {
    echo "ITEM-481: буфер записи [FB] не покрывает worst-case" >&2
    exit 1
}
grep -Fq '#define APPLIANCE_BOOT_LOG_STORE_LINE_MAX 640U' \
    kernel/src/appliance_boot_log_store.c || {
    echo "ITEM-481: буфер записи [BOOTLOG] не покрывает worst-case" >&2
    exit 1
}

# ------------------------------------------------------- the runtime guard --
grep -Fq '"[CONSOLE-BOUND] worst="' kernel/src/console.c || {
    echo "ITEM-481: нет записи о нарушении границы" >&2
    exit 1
}
grep -Fq 'size_t console_record_bound_violations(void);' \
    kernel/include/console.h || {
    echo "ITEM-481: счётчик нарушений не объявлен" >&2
    exit 1
}
grep -Fq 'size_t console_record_worst_length(void);' kernel/include/console.h || {
    echo "ITEM-481: worst-case не объявлен для пробы" >&2
    exit 1
}
grep -Fq 'size_t console_record_spill_count(void);' kernel/include/console.h || {
    echo "ITEM-481: счётчик разнесений не объявлен" >&2
    exit 1
}

# ----------------------------------------- the journal path is always bounded --
# The painted funnel (`console_publish_record`) keeps its own documented cut and
# mark rule; the durable journal reaches `boot_log_binding.observe()` only
# through `console_journal_record()`, and that function is called with a part
# already at or below the bound.
grep -Fq 'static void console_journal_record(const char *text, size_t length) {' \
    kernel/src/console.c || {
    echo "ITEM-481: нет единственной функции публикации в журнал" >&2
    exit 1
}
if [[ $(grep -Fc 'boot_log_binding.observe(' kernel/src/console.c) -ne 2 ]]; then
    echo "ITEM-481: путь в журнал изменился неожиданно" >&2
    exit 1
fi

# ----------------------------------------------------------- the host proofs --
grep -Fq 'static void test_record_bound_spills_into_bounded_tails(void) {' \
    tests/console-host.c || {
    echo "ITEM-481: нет host-пробы границы журнала" >&2
    exit 1
}
for item481_claim in \
    'an over-bound durable record is split, not cut' \
    'the head keeps the record'\''s own name' \
    'every continuation is named [<name>-TAIL]' \
    'no field is lost across the tail split' \
    'the assembled worst case is inside the bound' \
    'the by-construction bound never had to fire' \
    'a pre-built over-bound record is split too' \
    'every byte of a pre-built record survives the split'; do
    grep -Fq -- "$item481_claim" tests/console-host.c || {
        echo "ITEM-481: host-проба не утверждает:" "$item481_claim" >&2
        exit 1
    }
done
grep -Fq 'test_record_bound_spills_into_bounded_tails();' tests/console-host.c || {
    echo "ITEM-481: host-проба границы не вызывается" >&2
    exit 1
}

# --------------------------------------------------------------- QEMU proof --
grep -Fq 'r"#\d+ \[LOOP-PHASES-TAIL\]"' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-481: QEMU-проба не ловит хвост фаз" >&2
    exit 1
}
# ITEM-518: the input diagnostics record is short enough never to need a tail -
# the old proof's `[INPUT-DIAG-TAIL]` requirement was satisfied only by the
# runaway dump splitting its own nested echoes, which is the defect ITEM-518
# fixed.  The tail half is pinned on the families that really do split and that
# the proof requires by name (`[LOOP-PHASES-TAIL]` above,
# `[PRODUCTION-INPUT-TAIL]` below), while the bound itself stays pinned by the
# 320-byte check and the `[CONSOLE-BOUND]` absence check underneath.
grep -Fq '"[PRODUCTION-INPUT-TAIL]"' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-481: QEMU-проба не ловит хвост записи ввода" >&2
    exit 1
}
grep -Fq 'if any("[CONSOLE-BOUND]" in line for line in journal_records):' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-481: QEMU-проба не требует отсутствия нарушений границы" >&2
    exit 1
}
grep -Fq 'if any(len(line) > 320 for line in journal_records):' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-481: QEMU-проба не мерит длину записей журнала" >&2
    exit 1
}
grep -Fq 'the durable journal is missing the record part ' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-481: QEMU-проба не требует обе части разнесённой записи" >&2
    exit 1
}

# ------------------------------------------------------------- the documents --
for item481_doc in 'ITEM-481' 'CONSOLE_RECORD_MAX' 'CONSOLE-BOUND' \
    'TAIL'; do
    grep -Fq -- "$item481_doc" docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-481:" \
            "$item481_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-481' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-481" >&2
    exit 1
}
grep -Fq 'ITEM-481' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-481" >&2
    exit 1
}
