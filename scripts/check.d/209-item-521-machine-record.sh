#!/usr/bin/env bash
# ITEM-521 (increment 10): contracted slice of the HFOC OS check suite, sourced
#           in pinned order by scripts/check.sh and lintable on its own with
#           the shellcheck tool.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item521mr_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item521mr_root" || exit 1

# ITEM-521 increment 10: the journal names the machine the run happened on.
#
# Requirement 1 of the owner's item ("носитель ведёт журнал установки") lists
# what the medium's journal has to carry: the medium itself, the machine (its
# cores and its memory), the observed storage, the flow's decisions, every
# refusal with its reason and the panic tail.  The queue-side dry run of the
# prepared placement (2026-10-12, run-proof-wired-extent-draft.sh) read the
# *kernel's* journal off the emulated medium and found the machine half absent:
# `[MEM ] Usable memory: 474 MiB` went to serial only, the core count was
# written nowhere, and the 52 records on the medium named neither.
#
# This increment is fork-independent like increments 5, 6, 8 and 9: it renders
# one record - `[BOOT] machine cpus=<cores> memory=<mib>MiB` - and publishes it
# through the durable boot-log path, so it reaches the journal whichever
# ITEM-520 placement the owner chooses.  The slice pins:
#   * the renderer: one pure function, the exact format, both counter
#     boundaries, and the "too small means nothing, never a truncated record"
#     rule (requirement 5);
#   * the emission: exactly one call in kernel_main, at the memory line, inside
#     one serial sequence and one console journal pair, written to serial and
#     to the durable path with the same text;
#   * the witness and its registration, so the format cannot drift silently;
#   * the queue and the roadmap: this increment is current, and the placement
#     itself is still the owner's ITEM-520 fork (no placement token is chosen
#     here and the seam's body is untouched).

# 1. The module, the witness, the call site and the documents exist.
for item521mr_file in \
    kernel/include/machine_facts.h \
    kernel/src/machine_facts.c \
    kernel/src/kernel.c \
    tests/machine-facts-host.c \
    Makefile \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md \
    docs/ROADMAP.md; do
    [[ -f $item521mr_file ]] || {
        echo "ITEM-521: отсутствует $item521mr_file" >&2
        exit 1
    }
done

# 2. The header declares the renderer and its one refusal rule: a record that
#    does not fit is never handed out half-written.
grep -Fq 'size_t machine_facts_record(char *output, size_t capacity, uint64_t memory_mib,' \
    kernel/include/machine_facts.h || {
    echo "ITEM-521: заголовок не объявляет запись о машине" >&2
    exit 1
}
grep -Fq '#define MACHINE_FACTS_RECORD_MAX 64U' \
    kernel/include/machine_facts.h || {
    echo "ITEM-521: заголовок не объявляет границу записи" >&2
    exit 1
}
grep -Fq 'truncated record is never handed to a journal' \
    kernel/include/machine_facts.h || {
    echo "ITEM-521: заголовок не называет правило отказа по месту" >&2
    exit 1
}

# 3. The renderer is one pure function: the pinned format, the NUL before the
#    copy, and no serial, console or device call of its own.
for item521mr_pin in \
    '"[BOOT] machine cpus="' \
    '" memory="' \
    '"MiB"' \
    "record[length] = '\\0';" \
    'length + 1U > capacity'; do
    grep -Fq "$item521mr_pin" kernel/src/machine_facts.c || {        echo "ITEM-521: рендер записи о машине не содержит: $item521mr_pin" >&2
        exit 1
    }
done
if grep -Eq 'serial_write|console_|block_device' kernel/src/machine_facts.c; then
    echo "ITEM-521: рендер записи о машине сам пишет в serial, консоль или устройство" >&2
    exit 1
fi

# 4. The emission: one call, in kernel_main, at the memory line and before the
#    PMM line, inside one serial sequence and one console journal pair, with the
#    same text going to the serial stream and to the durable path.
item521mr_calls=$(grep -c 'machine_facts_record(' kernel/src/kernel.c)
[[ $item521mr_calls -eq 1 ]] || {
    echo "ITEM-521: запись о машине рендерится не ровно один раз ($item521mr_calls)" >&2
    exit 1
}
item521mr_block=$(awk '
    /const uint32_t machine_cores =/ { inside = 1 }
    inside { print }
    inside && /console_journal_end\(\);/ { exit }
' kernel/src/kernel.c)
[[ -n $item521mr_block ]] || {
    echo "ITEM-521: блок публикации записи о машине не найден" >&2
    exit 1
}
for item521mr_pin in \
    'machine_record, sizeof(machine_record), usable_mib, machine_cores);' \
    'console_journal_begin();' \
    'serial_write(machine_record);' \
    'console_record_text(machine_record);' \
    'serial_sequence_end(serial_flags);' \
    'console_journal_end();'; do
    [[ $item521mr_block == *"$item521mr_pin"* ]] || {
        echo "ITEM-521: блок записи о машине не содержит: $item521mr_pin" >&2
        exit 1
    }
done
item521mr_begin=$(printf '%s\n' "$item521mr_block" |
    grep -n 'console_journal_begin();' | head -n 1 | cut -d: -f1)
item521mr_serial=$(printf '%s\n' "$item521mr_block" |
    grep -n 'serial_write(machine_record);' | head -n 1 | cut -d: -f1)
item521mr_record=$(printf '%s\n' "$item521mr_block" |
    grep -n 'console_record_text(machine_record);' | head -n 1 | cut -d: -f1)
item521mr_sequence=$(printf '%s\n' "$item521mr_block" |
    grep -n 'serial_sequence_end(serial_flags);' | head -n 1 | cut -d: -f1)
item521mr_end=$(printf '%s\n' "$item521mr_block" |
    grep -n 'console_journal_end();' | head -n 1 | cut -d: -f1)
[[ -n $item521mr_begin && -n $item521mr_serial && -n $item521mr_record &&
   -n $item521mr_sequence && -n $item521mr_end ]] || {
    echo "ITEM-521: блок записи о машине неполон" >&2
    exit 1
}
((item521mr_begin < item521mr_serial &&
  item521mr_serial < item521mr_record &&
  item521mr_record < item521mr_sequence &&
  item521mr_sequence < item521mr_end)) || {
    echo "ITEM-521: запись о машине выходит из одной последовательности" >&2
    exit 1
}
item521mr_cores=$(printf '%s\n' "$item521mr_block" |
    grep -c 'mp_request.response->cpu_count != 0U')
[[ $item521mr_cores -eq 1 ]] || {
    echo "ITEM-521: запись о машине не берёт число ядер у загрузчика" >&2
    exit 1
}

# 5. The record is emitted where the memory is known: after the `[MEM ]` line
#    and before the `[PMM ]` line, so it exists before any medium can bind.
item521mr_mem=$(grep -n -F 'serial_write("[MEM ] Usable memory: ");' \
    kernel/src/kernel.c | head -n 1 | cut -d: -f1)
item521mr_call=$(grep -n 'machine_facts_record(' kernel/src/kernel.c |
    head -n 1 | cut -d: -f1)
item521mr_pmm=$(grep -n -F 'serial_write("[PMM ] First allocated page: ");' \
    kernel/src/kernel.c | head -n 1 | cut -d: -f1)
[[ -n $item521mr_mem && -n $item521mr_call && -n $item521mr_pmm ]] || {
    echo "ITEM-521: не найдены строки памяти и записи о машине" >&2
    exit 1
}
((item521mr_mem < item521mr_call && item521mr_call < item521mr_pmm)) || {
    echo "ITEM-521: запись о машине стоит не на строке памяти" >&2
    exit 1
}

# 6. The witness is registered in the host suite and carries the assertions this
#    increment is about; a witness that is not run proves nothing.
grep -Fq '.PHONY: test-machine-facts-host' Makefile || {
    echo "ITEM-521: host-проба записи о машине не объявлена" >&2
    exit 1
}
grep -Fq '	$(MAKE) test-machine-facts-host' Makefile || {
    echo "ITEM-521: host-проба записи о машине не входит в набор" >&2
    exit 1
}
for item521mr_pin in \
    '[BOOT] machine cpus=1 memory=474MiB' \
    '[BOOT] machine cpus=4294967295 ' \
    'the record renders the boundary values in full' \
    'zero memory and zero cores are rendered as values' \
    'a buffer one byte short is refused' \
    'the refused buffer is emptied, not left holding a fragment' \
    'a NULL buffer is refused' \
    'the record is NUL-terminated' \
    'the same facts render the same record'; do
    grep -Fq "$item521mr_pin" tests/machine-facts-host.c || {
        echo "ITEM-521: host-проба записи о машине не проверяет: $item521mr_pin" >&2
        exit 1
    }
done

# 7. The queue is on this increment and still sends the placement itself to the
#    owner's ITEM-520 answer; the roadmap names it too.  (The current-item pin
#    follows the item the queue is on: ITEM-564 moved it on from ITEM-547; this
#    slice's own record stays its increment 10 section in the roadmap.)
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
grep -Fq 'инкремент 10' docs/ROADMAP.md || {
    echo "ITEM-521: docs/ROADMAP.md не называет инкремент" >&2
    exit 1
}
