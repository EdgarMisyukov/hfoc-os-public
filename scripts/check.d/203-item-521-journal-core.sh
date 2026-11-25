#!/usr/bin/env bash
# ITEM-521 (increment 1): contracted slice of the HFOC OS check suite, sourced
#           in pinned order by scripts/check.sh and lintable on its own with
#           the shellcheck tool.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item521_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item521_root" || exit 1

# ITEM-521 increment 1: the placement-independent core of the install journal.
# The owner's medium must carry the install run and every refusal; the item is
# split so that the placement (ITEM-520: FAT12 file, raw extent or preallocated
# file) can be chosen without rewriting the journal itself.  This slice holds
# the first increment's contract:
#   * the core is placement-independent: the medium is reached only through
#     `struct install_journal_backend`, so no placement is chosen here;
#   * a record published before the medium exists is staged in a bounded ring
#     and replayed in order at bind, so the machine announcement and the
#     observed storage are not lost (requirement 1);
#   * every loss is named: OVERFLOW, TRUNCATED, FULL, UNAVAILABLE - and the
#     status never re-enters the funnel that feeds the journal;
#   * one slot is reserved so the end of the journal can be named (requirement
#     5), a backend refusal turns the journal off, not the run (requirement 3),
#     and the capacity a placement declares is a real bound: no write path -
#     live, replayed or panic - hands it a slot past that bound (increment 9);
#   * the host proof drives all of that and is registered in the host suite.

# 1. The module, its proof and its record exist where the contract says.
for item521_file in \
    kernel/include/install_journal_core.h \
    kernel/src/install_journal_core.c \
    tests/install-journal-core-host.c \
    docs/ROADMAP.md; do
    [[ -f $item521_file ]] || {
        echo "ITEM-521: отсутствует $item521_file" >&2
        exit 1
    }
done

# 2. The core owns no placement: it reaches the medium only through the four
#    backend callbacks, and it does not include a placement module.
grep -Fq 'struct install_journal_backend {' \
    kernel/include/install_journal_core.h || {
    echo "ITEM-521: ядро не описывает интерфейс размещения" >&2
    exit 1
}
for item521_callback in \
    'uint32_t (*committed_slots)(void *context);' \
    'uint32_t (*capacity_slots)(void *context);' \
    'bool (*write_slot)(void *context,' \
    'bool (*flush)(void *context);'; do
    grep -Fq "$item521_callback" kernel/include/install_journal_core.h || {
        echo "ITEM-521: размещение не описывает колбэк: $item521_callback" >&2
        exit 1
    }
done
if grep -Eq '#include "(install_fat12|install_extent)' \
    kernel/src/install_journal_core.c kernel/include/install_journal_core.h; then
    echo "ITEM-521: ядро выбрало размещение (развилка ITEM-520 открыта)" >&2
    exit 1
fi

# 3. The slot format is pinned byte for byte, so the reader and every future
#    placement agree on it.
grep -Fq '#define INSTALL_JOURNAL_SLOT_BYTES 512U' \
    kernel/include/install_journal_core.h || {
    echo "ITEM-521: слот не зафиксирован на 512 байт" >&2
    exit 1
}
grep -Fq '#define INSTALL_JOURNAL_TEXT_MAX 440U' \
    kernel/include/install_journal_core.h || {
    echo "ITEM-521: предел текста записи не зафиксирован" >&2
    exit 1
}
grep -Fq '#define INSTALL_JOURNAL_PENDING_CAPACITY 64U' \
    kernel/include/install_journal_core.h || {
    echo "ITEM-521: кольцо предбуфера не ограничено" >&2
    exit 1
}
grep -Fq '#define INSTALL_JOURNAL_MAGIC "HFOC-ILOG"' \
    kernel/include/install_journal_core.h || {
    echo "ITEM-521: magic журнала не зафиксирован" >&2
    exit 1
}
grep -Fq '#define INSTALL_JOURNAL_VERSION 1U' \
    kernel/include/install_journal_core.h || {
    echo "ITEM-521: версия формата не зафиксирована" >&2
    exit 1
}
for item521_layout in \
    'le32_write(&slot[9], INSTALL_JOURNAL_VERSION);' \
    'le32_write(&slot[13], INSTALL_JOURNAL_SLOT_BYTES);' \
    'le32_write(&slot[17], journal->backend.capacity_slots != NULL' \
    'le32_write(&slot[21], journal->written);' \
    'le32_write(&slot[36], install_journal_crc32(slot, 36U));' \
    'le32_write(&slot[0], (uint32_t)length);' \
    'le32_write(&slot[4],' \
    'memcpy(&slot[8], text, length);'; do
    grep -Fq "$item521_layout" kernel/src/install_journal_core.c || {
        echo "ITEM-521: раскладка слота не содержит: $item521_layout" >&2
        exit 1
    }
done

# 4. Every loss has a name an operator can read, and the journal's own status
#    travels on the notify side, never back through `observe`.
for item521_status in \
    '[INSTALL-LOG] state=OVERFLOW dropped=' \
    '[INSTALL-LOG] state=TRUNCATED limit=440' \
    '[INSTALL-LOG] state=FULL reason=CAPACITY' \
    '[INSTALL-LOG] state=UNAVAILABLE reason=' \
    'reason=PANIC_UNBOUND' \
    'go_unavailable(journal, "WRITE")' \
    'go_unavailable(journal, "FLUSH")' \
    'go_unavailable(journal, "HEADER")'; do
    grep -Fq "$item521_status" kernel/src/install_journal_core.c || {
        echo "ITEM-521: потеря не названа: $item521_status" >&2
        exit 1
    }
done
grep -Fq 'install_journal_notify_fn notify' \
    kernel/include/install_journal_core.h || {
    echo "ITEM-521: статус журнала не отделён от воронки" >&2
    exit 1
}
grep -Fq 'bool publishing; /* re-entrancy guard for the funnel */' \
    kernel/include/install_journal_core.h || {
    echo "ITEM-521: воронка не защищена от повторного входа" >&2
    exit 1
}

# 5. The end of the journal is nameable and the declared capacity is a real
#    bound: one slot is reserved for the end marker, the header of a resumed
#    journal is not written a second time into the record stream, and every
#    write path asks for room before it hands a slot over (increment 9 - the
#    replay at bind used to write until the placement refused, which named a
#    full journal as a write failure and left the rest of the ring uncounted).
grep -Fq 'journal->written + slots <= journal->capacity' \
    kernel/src/install_journal_core.c || {
    echo "ITEM-521: предел размещения не проверяется перед записью" >&2
    exit 1
}
grep -Fq 'static void go_full(struct install_journal *journal)' \
    kernel/src/install_journal_core.c || {
    echo "ITEM-521: переход в FULL не один на все пути" >&2
    exit 1
}
item521_room_calls=$(grep -c 'room_for(journal, 2U)' \
    kernel/src/install_journal_core.c)
[[ $item521_room_calls -ge 3 ]] || {
    echo "ITEM-521: не все пути записи спрашивают предел размещения" >&2
    exit 1
}
grep -Fq 'if (room_for(journal, 1U)) {' \
    kernel/src/install_journal_core.c || {
    echo "ITEM-521: метка конца пишется за пределом размещения" >&2
    exit 1
}
grep -Fq 'journal->refused += journal->pending_count;' \
    kernel/src/install_journal_core.c || {
    echo "ITEM-521: не поместившиеся записи кольца не посчитаны" >&2
    exit 1
}
grep -Fq 'if (journal->state == INSTALL_JOURNAL_FULL &&' \
    kernel/src/install_journal_core.c || {
    echo "ITEM-521: записи кольца считаются потерянными и при отказе записи" >&2
    exit 1
}
grep -Fq 'ring_unpop(journal);' kernel/src/install_journal_core.c || {
    echo "ITEM-521: отказанная размещением запись не возвращается в кольцо" >&2
    exit 1
}
grep -Fq 'if (committed == 0U) {' kernel/src/install_journal_core.c || {
    echo "ITEM-521: повторная привязка не отделена от свежей" >&2
    exit 1
}

# 6. The host proof drives the staging ring, the named losses, the re-entrancy
#    guard and both flush paths, and the target is registered in the compiled
#    host suite (the manifest check re-derives the count from the serial
#    recipe).
for item521_proof_pin in \
    'records stage before bind' \
    'bind replays the ring' \
    'six oldest records were dropped and counted' \
    'a refused write turns the journal off, not the run' \
    'FULL is named on the notify side' \
    'an unwritten panic tail is named, not faked' \
    'no header is written into the middle (F6)' \
    'a re-entrant backend' \
    'no write path hands the placement a slot past its capacity' \
    'the journal stops exactly at the capacity it was given' \
    'the reason is CAPACITY, not WRITE' \
    'a journal with no room writes nothing past the end' \
    'and the panic replay writes nothing past the capacity either' \
    'no staged record is counted away by one refused write' \
    'the kept records reach the medium on the resumed bind' \
    'and the ring keeps its order across the failure' \
    'a changed header field breaks the CRC'; do
    grep -Fq "$item521_proof_pin" tests/install-journal-core-host.c || {
        echo "ITEM-521: host-проба не проверяет: $item521_proof_pin" >&2
        exit 1
    }
done
grep -Fq 'test-install-journal-core-host:' Makefile || {
    echo "ITEM-521: цель host-пробы не определена" >&2
    exit 1
}
grep -Fq '$(MAKE) test-install-journal-core-host' Makefile || {
    echo "ITEM-521: host-проба не зарегистрирована в host-suite" >&2
    exit 1
}

# 7. The queue records this item (ITEM-526 moved the `## Current item` heading on
#    to its own item, so this pin reads ITEM-521's own historical section), and
#    the roadmap records the increment.
grep -A 3 '^## Item ITEM-521$' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md |
    grep -Fq 'ITEM-521' || {
    echo "ITEM-521: раздел очереди не называет ITEM-521" >&2
    exit 1
}
grep -Fq 'ITEM-521' docs/ROADMAP.md || {
    echo "ITEM-521: docs/ROADMAP.md не называет пункт" >&2
    exit 1
}
