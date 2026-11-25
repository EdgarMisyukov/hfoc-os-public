#!/usr/bin/env bash
# ITEM-477: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The periodic records reach the journal, not the screen (СРОЧНО-26, round 15).
#
# The owner's 2026-09-24 hardware run on `ce6765a3` read `tick_source=pit` and
# `wake_hz=100` with a working pointer, and reported "служебные символы"
# constantly arriving on the screen.  The tick fix turned the once-a-second
# input and phase records and the ten-second loop heartbeat from a line every
# forty seconds into the flood the screen budget (ITEM-466, at most twelve
# painted records a second) then spent itself repainting; the operator's own
# view scrolled away.  The records exist for the serial stream and the durable
# journal, not for the text console, so they now open with a journal-only
# mirror: the same fragments, the same bound, the same log, no paint.
#
# The four periodic records are the input heartbeat, its tail, the loop's phase
# budget and the ten-second [PRODUCTION-LOOP]; a one-time state change (boot,
# DHCP, serving, install, menu, power, error) and the operator's own command
# still paint, because that is what the screen is for.  The phase record now
# also carries `paint_lines=`, the console's own rendered-line counter, so the
# QEMU proof can require the screen to stay put while the heartbeat publishes
# from the serial stream, and `bootlog` shows the periodic records in the
# durable journal with the dump's own `#<sequence> ` prefix.

for item477_file in \
    kernel/include/console.h \
    kernel/src/console.c \
    kernel/src/kernel.c \
    tests/console-host.c \
    scripts/test-qemu-production-storage-boot.sh \
    docs/hfoc-appliance-boot-console.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item477_file ]] || {
        echo "ITEM-477: отсутствует $item477_file" >&2
        exit 1
    }
done

# ------------------------------------------------- the journal-only opener --
for item477_api in \
    'void console_journal_begin(void);' \
    'void console_journal_end(void);'; do
    grep -Fq -- "$item477_api" kernel/include/console.h || {
        echo "ITEM-477: журнальное зеркало не объявлено:" "$item477_api" >&2
        exit 1
    }
done
for item477_impl in \
    'void console_journal_begin(void)' \
    'static void console_mirror_finish(void)' \
    'const bool to_journal = mirror_to_journal;' \
    'console_mirror_publish_journal();' \
    'console_publish_record(mirror_text, mirror_length);' \
    'void console_journal_end(void)' \
    'void console_record_end(void)'; do
    grep -Fq -- "$item477_impl" kernel/src/console.c || {
        echo "ITEM-477: журнальное зеркало не реализовано:" \
            "$item477_impl" >&2
        exit 1
    }
done

# ------------------------------------------- the periodic four and the rest --
# The fragment writer is unchanged - the periodic records stay on the serial
# stream and in the journal - and only the opener chooses the screen off.
grep -Fq 'NET_RECORD_WRITE("[PRODUCTION-INPUT] kbd_reports=");' \
    kernel/src/kernel.c || {
    echo "ITEM-477: запись ввода потеряла поля" >&2
    exit 1
}
grep -Fq 'NET_RECORD_WRITE("[LOOP-PHASES] turns=");' kernel/src/kernel.c || {
    echo "ITEM-477: запись фаз потеряла поля" >&2
    exit 1
}
for item477_periodic_fn in \
    log_production_input_record \
    log_production_loop_phases \
    log_production_loop_heartbeat; do
    item477_body=$(
        awk "/^static void $item477_periodic_fn\(/{f=1} f{print} f&&/^}/{exit}" \
            kernel/src/kernel.c
    )
    grep -Fq 'console_journal_begin();' <<<"$item477_body" || {
        echo "ITEM-477: $item477_periodic_fn не открывает журнальное зеркало" >&2
        exit 1
    }
    grep -Fq 'console_journal_end();' <<<"$item477_body" || {
        echo "ITEM-477: $item477_periodic_fn не закрывает журнальное зеркало" >&2
        exit 1
    }
    if grep -Fq 'console_record_begin();' <<<"$item477_body"; then
        echo "ITEM-477: $item477_periodic_fn всё ещё рисуется" >&2
        exit 1
    fi
done
# ITEM-478 (round 16): every service record now opens the journal-only mirror,
# so the one-time network record keeps the journal and loses the screen too.
# The painted mirror stays an API the host proof still exercises; no production
# record uses it.
item477_network_body=$(
    awk '/^static void log_production_network\(void\)/{f=1} f{print} f&&/^}/{exit}' \
        kernel/src/kernel.c
)
grep -Fq 'console_journal_begin();' <<<"$item477_network_body" || {
    echo "ITEM-478: разовая сетевая запись не уходит в журнал" >&2
    exit 1
}
if grep -Fq 'console_record_begin();' <<<"$item477_network_body"; then
    echo "ITEM-478: разовая сетевая запись всё ещё рисуется" >&2
    exit 1
fi

# The phase record names the painted lines, so the QEMU proof reads the
# screen's growth from the serial stream without touching the framebuffer.
grep -Fq 'NET_RECORD_WRITE(" paint_lines=");' kernel/src/kernel.c || {
    echo "ITEM-477: запись фаз не несёт число нарисованных строк" >&2
    exit 1
}
grep -Fq 'NET_RECORD_U64(console_render_line_count());' kernel/src/kernel.c || {
    echo "ITEM-477: запись фаз не читает счётчик рисования" >&2
    exit 1
}

# ------------------------------------------------------------- host proofs --
for item477_claim in \
    'a periodic record reaches the durable journal' \
    'a periodic record is never painted on the screen' \
    'a state-change record still reaches the durable journal' \
    'a state-change record still paints the screen' \
    "the operator's command still paints its answer" \
    "the operator's command still reaches the durable journal"; do
    grep -Fq -- "$item477_claim" tests/console-host.c || {
        echo "ITEM-477: host-проба не утверждает:" "$item477_claim" >&2
        exit 1
    }
done
grep -Fq 'static void test_periodic_records_are_journal_only(void) {' \
    tests/console-host.c || {
    echo "ITEM-477: host-пробы периодики нет" >&2
    exit 1
}
grep -Fq 'test_periodic_records_are_journal_only();' tests/console-host.c || {
    echo "ITEM-477: host-проба периодики не вызывается" >&2
    exit 1
}

# --------------------------------------------------------------- QEMU proof --
grep -Fq 'paint_lines' scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-477: QEMU-проба не читает число нарисованных строк" >&2
    exit 1
}
grep -Fq 'the periodic records keep painting the screen' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-477: QEMU-проба не ловит возврат периодики на экран" >&2
    exit 1
}
grep -Fq 'r"#\d+ \[LOOP-PHASES\]"' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-477: QEMU-проба не сверяет durable-журнал" >&2
    exit 1
}
grep -Fq 'the durable journal did not carry the periodic records' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-477: QEMU-проба не требует периодику в журнале" >&2
    exit 1
}

# ------------------------------------------------------------- the documents --
for item477_doc in \
    'ITEM-477' \
    'console_journal_begin' \
    'paint_lines'; do
    grep -Fq -- "$item477_doc" docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-477:" \
            "$item477_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-477' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-477" >&2
    exit 1
}
grep -Fq 'ITEM-477' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-477" >&2
    exit 1
}
