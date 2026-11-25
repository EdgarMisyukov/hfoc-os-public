#!/usr/bin/env bash
# ITEM-521 (increment 11): contracted slice of the HFOC OS check suite, sourced
#           in pinned order by scripts/check.sh and lintable on its own with
#           the shellcheck tool.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item521pr_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item521pr_root" || exit 1

# ITEM-521 increment 11: the panic names itself in the durable journal.
#
# Requirement 1 of the owner's item ("носитель ведёт журнал установки") names
# the panic among the records the medium's journal must carry, and requirement
# 2 asks the panic path to get its tail out.  Both halves were wired - the
# panic path flushed what was staged and the seam reaches the medium - but the
# panic itself was never a record: `panic_finish()` wrote it to the serial
# stream and the screen only, so the journal ended on whatever ran *before* the
# machine stopped.  Found by reading the journal off the emulated medium, the
# same way increment 10 found the machine record missing.
#
# This increment is fork-independent like increments 5, 6, 8, 9 and 10: it
# renders two records - `[PANIC] <message> vector=<decimal> error=0x<hex>
# rip=0x<hex>` and `[PANIC] detail=<detail>` - and stages them through the
# durable boot-log path, so they reach the journal whichever ITEM-520 placement
# the owner chooses.  The slice pins:
#   * the renderers: two pure functions, the exact format, the clamp that
#     *marks* an over-long message or detail instead of hiding it, and the
#     "too small means nothing, never a truncated record" rule
#     (requirement 5);
#   * the emission: exactly one call in `panic_finish`, between the store's
#     panic flush and the journal's own panic flush, so the tail the medium
#     receives names the panic and nothing can paint over the panic screen;
#   * that both records fit the bound the durable log and the journal share, so
#     neither of them clamps a panic on the way to the medium;
#   * the witness and its registration, so the format cannot drift silently;
#   * the queue and the roadmap: this increment is current, the placement
#     itself is still the owner's ITEM-520 fork (no placement token is chosen
#     here and the seam's body is untouched).

# 1. The module, the witness, the call site and the documents exist.
for item521pr_file in \
    kernel/include/panic_record.h \
    kernel/src/panic_record.c \
    kernel/src/panic.c \
    tests/panic-record-host.c \
    Makefile \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md \
    docs/ROADMAP.md; do
    [[ -f $item521pr_file ]] || {
        echo "ITEM-521: отсутствует $item521pr_file" >&2
        exit 1
    }
done

# 2. The header declares both renderers and their bounds.
for item521pr_pin in \
    'size_t panic_record_render(char *output, size_t capacity, const char *message,' \
    'size_t panic_record_detail(char *output, size_t capacity, const char *detail);' \
    '#define PANIC_RECORD_MESSAGE_MAX 160U' \
    '#define PANIC_RECORD_DETAIL_MAX 399U' \
    '#define PANIC_RECORD_MAX 256U' \
    '#define PANIC_RECORD_DETAIL_LINE_MAX 416U' \
    'nothing is ever handed out truncated'; do
    grep -Fq "$item521pr_pin" kernel/include/panic_record.h || {
        echo "ITEM-521: заголовок записи о панике не содержит: $item521pr_pin" >&2
        exit 1
    }
done

# 3. The renderers are pure: the pinned format, the NUL before the copy, the
#    refusal rule and the marked clamp - and no serial, console or device call
#    of their own.
for item521pr_pin in \
    '"[PANIC] "' \
    '" vector="' \
    '" error="' \
    '" rip="' \
    '"[PANIC] detail="' \
    '"0123456789ABCDEF"' \
    'memcpy(record + length + bound - 3U, "...", 3U);' \
    'length + 1U > capacity' \
    "record[length] = '\\0';" \
    'Unknown panic'; do
    grep -Fq "$item521pr_pin" kernel/src/panic_record.c || {
        echo "ITEM-521: рендер записи о панике не содержит: $item521pr_pin" >&2
        exit 1
    }
done
# Comments may name the serial renderer this format deliberately copies
# (`serial_write_hex`); the code must not call it.
if grep -Ev '^[[:space:]]*([*/]|//)' kernel/src/panic_record.c |
    grep -Eq 'serial_write|console_|block_device|framebuffer'; then
    echo "ITEM-521: рендер записи о панике сам пишет в serial, консоль или устройство" >&2
    exit 1
fi

# 4. The emission: one call in panic.c, both records staged through the durable
#    boot-log path, and the order store-panic-flush -> panic records ->
#    journal-panic-flush that keeps the panic screen and the medium's tail.
item521pr_calls=$(grep -c 'publish_panic_record(message, context);' \
    kernel/src/panic.c)
[[ $item521pr_calls -eq 1 ]] || {
    echo "ITEM-521: панические записи публикуются не ровно один раз ($item521pr_calls)" >&2
    exit 1
}
item521pr_helper=$(awk '
    /^static void publish_panic_record\(/ { inside = 1 }
    inside { print }
    inside && /^}/ { exit }
' kernel/src/panic.c)
[[ -n $item521pr_helper ]] || {
    echo "ITEM-521: помощник публикации панических записей не найден" >&2
    exit 1
}
for item521pr_pin in \
    'panic_record_render(record, sizeof(record), message,' \
    'panic_record_detail(detail, sizeof(detail),' \
    'appliance_boot_log_store_observe(record, length);' \
    'appliance_boot_log_store_observe(detail, length);'; do
    [[ $item521pr_helper == *"$item521pr_pin"* ]] || {
        echo "ITEM-521: помощник публикации не содержит: $item521pr_pin" >&2
        exit 1
    }
done
if grep -Eq 'serial_write|framebuffer|console_write' <<<"$item521pr_helper"; then
    echo "ITEM-521: паническая запись сама рисует экран или serial вместо долговременного пути" >&2
    exit 1
fi
item521pr_store=$(grep -n 'appliance_boot_log_store_panic_flush();' \
    kernel/src/panic.c | head -n 1 | cut -d: -f1)
item521pr_publish=$(grep -n 'publish_panic_record(message, context);' \
    kernel/src/panic.c | head -n 1 | cut -d: -f1)
item521pr_journal=$(grep -n 'install_journal_platform_panic_flush();' \
    kernel/src/panic.c | head -n 1 | cut -d: -f1)
[[ -n $item521pr_store && -n $item521pr_publish && -n $item521pr_journal ]] || {
    echo "ITEM-521: панический путь не найден целиком" >&2
    exit 1
}
((item521pr_store < item521pr_publish && item521pr_publish < item521pr_journal)) || {
    echo "ITEM-521: панические записи стоят не между двумя сбросами" >&2
    exit 1
}

# 5. The witness is registered in the host suite and carries the assertions this
#    increment is about; a witness that is not run proves nothing.
grep -Fq '.PHONY: test-panic-record-host' Makefile || {
    echo "ITEM-521: host-проба записи о панике не объявлена" >&2
    exit 1
}
grep -Fq '	$(MAKE) test-panic-record-host' Makefile || {
    echo "ITEM-521: host-проба записи о панике не входит в набор" >&2
    exit 1
}
for item521pr_pin in \
    'the panic record names the message and the frame' \
    'a frameless panic record carries the message alone' \
    'a null message renders as Unknown panic' \
    'the frame fields render the boundary values in full' \
    'a message at the bound is rendered whole' \
    'an over-long message is clamped to the bound' \
    'the clamp is marked with a visible tail' \
    'the detail record names the panic detail' \
    'a null detail is refused with an emptied buffer' \
    'an empty detail is refused with an emptied buffer' \
    'a detail at the bound is rendered whole' \
    'an over-long detail is clamped to the bound' \
    'a buffer one byte short is refused' \
    'the refused record writes nothing past the buffer' \
    'a detail record one byte short is refused' \
    'a null message buffer is refused' \
    'the record is NUL-terminated at its length' \
    'the same panic renders the same record' \
    "both panic records fit the durable log's record bound" \
    "both panic records fit the journal's record bound"; do
    grep -Fq "$item521pr_pin" tests/panic-record-host.c || {
        echo "ITEM-521: host-проба записи о панике не проверяет: $item521pr_pin" >&2
        exit 1
    }
done

# 6. The queue is on this increment and still sends the placement itself to the
#    owner's ITEM-520 answer; the roadmap names it too.  (The current-item pin
#    follows the item the queue is on: ITEM-564 moved it on from ITEM-547.)
grep -A 3 '^## Item ITEM-547$' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md |
    grep -Fq 'ITEM-547' || {
    echo "ITEM-521: очередь потеряла исторический раздел ITEM-547" >&2
    exit 1
}
grep -Fq 'ITEM-520' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-521: очередь не называет развилку владельца" >&2
    exit 1
}
grep -Fq 'инкремент 11' docs/ROADMAP.md || {
    echo "ITEM-521: docs/ROADMAP.md не называет инкремент" >&2
    exit 1
}
