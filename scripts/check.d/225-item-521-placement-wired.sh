#!/usr/bin/env bash
# ITEM-521 (increment 12): contracted slice of the HFOC OS check suite, sourced
#           in pinned order by scripts/check.sh and lintable on its own with
#           the shellcheck tool.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item521pw_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item521pw_root" || exit 1

# ITEM-521 increment 12: the placement is wired as the owner's answer (c) and the
# medium's journal is read back with one command.
#
# The closing slice of the placement increment.  Increments 1-11 built the
# placement-independent half (their sections and the slices 203-210); increment
# 12 fills the seam with the owner's answer (c): the medium's build reserves
# \HFOC\INSTALL.LOG in the FAT12 partition, the kernel only reads what the build
# reserved and writes only inside those clusters - no allocation, no FAT write,
# no directory write.  Slices 205, 207-210 already pin the seam, the proven
# medium handed to it, the machine record and the panic record; this slice pins
# the end of the path the operator sees:
#   * the queue's current item is this increment: it names ITEM-521, the answer
#     (c) to the ITEM-520 fork, and it no longer sends the placement to the
#     owner;
#   * docs/ROADMAP.md records the increment and the answer;
#   * the build reserves the file: the preallocation script exists, refuses
#     rather than write outside the FAT12 partition, and every installer-media
#     recipe runs it before sealing the image;
#   * the kernel module allocates nothing: it contains no allocation, no FAT
#     entry write and no directory-entry write, and it names the two refusals
#     the reservation needs (NOT_RESERVED, BAD_HEADER);
#   * the operator reads the medium's journal with one command, documented in
#     docs/hfoc-hardware-acceptance.md next to the boot-log reader, and the
#     reader names the reserved placement.
#
# The behavioural proof is queue-side and is not part of the repository: the
# QEMU proofs of the wired medium (item-521-qemu-proof/) boot the installer on a
# writable emulated medium, read the kernel journal off it with
# scripts/hfoc-install-log-read.py and compare the records with requirement 1.

item521pw_queue=docs/superpowers/plans/2026-09-11-appliance-productization-queue.md
for item521pw_file in \
    scripts/hfoc-install-media-prealloc.py \
    tests/install-media-prealloc-host.py \
    scripts/hfoc-install-log-read.py \
    tests/install-fat12-journal-host.c \
    tests/install-journal-platform-fat12-host.c \
    kernel/include/install_fat12_journal.h \
    kernel/src/install_fat12_journal.c \
    docs/hfoc-hardware-acceptance.md \
    docs/ROADMAP.md \
    "$item521pw_queue"; do
    [[ -f $item521pw_file ]] || {
        echo "ITEM-521: отсутствует $item521pw_file" >&2
        exit 1
    }
done

item521pw_current=$(awk '/^## Item ITEM-521 \(инкремент 12\)/{inside = 1; next} inside && /^## /{exit} inside{print}' "$item521pw_queue")

# 1. The queue is on this increment and says the fork is answered with (c).
[[ -n $item521pw_current ]] || {
    echo "ITEM-521: очередь не несёт текущего раздела" >&2
    exit 1
}
for item521pw_pin in \
    'ITEM-521' \
    'инкремент 12' \
    'ITEM-520' \
    'вариант (в)'; do
    [[ $item521pw_current == *"$item521pw_pin"* ]] || {
        echo "ITEM-521: текущий раздел очереди не называет: $item521pw_pin" >&2
        exit 1
    }
done
for item521pw_open in \
    'ждёт ответа владельца' \
    'по-прежнему за владельцем' \
    'развилку выбирает владелец'; do
    if [[ $item521pw_current == *"$item521pw_open"* ]]; then
        echo "ITEM-521: текущий раздел всё ещё называет развилку открытой: $item521pw_open" >&2
        exit 1
    fi
done

# 2. The roadmap records the increment and the answer.
grep -Fq 'ITEM-521' docs/ROADMAP.md || {
    echo "ITEM-521: docs/ROADMAP.md не называет item" >&2
    exit 1
}
grep -Fq '## ITEM-521 — журнал установки пишется на носитель (инкремент 12, 13.10)' \
    docs/ROADMAP.md || {
    echo "ITEM-521: docs/ROADMAP.md не несёт запись текущего инкремента" >&2
    exit 1
}
grep -Fq 'вариант (в)' docs/ROADMAP.md || {
    echo "ITEM-521: docs/ROADMAP.md не называет ответ владельца" >&2
    exit 1
}

# 3. The build reserves the file and refuses to write outside its partition.
grep -Fq 'HFOC-ILOG' scripts/hfoc-install-media-prealloc.py || {
    echo "ITEM-521: скрипт предвыделения не пишет заголовок журнала" >&2
    exit 1
}
grep -Fq 'write_slot' scripts/hfoc-install-media-prealloc.py || {
    echo "ITEM-521: скрипт предвыделения не пишет точку фиксации" >&2
    exit 1
}
grep -Fq 'partition' scripts/hfoc-install-media-prealloc.py || {
    echo "ITEM-521: скрипт предвыделения не называет раздел" >&2
    exit 1
}
item521pw_recipes=$(grep -c 'scripts/hfoc-install-media-prealloc.py' Makefile)
[[ $item521pw_recipes -ge 9 ]] || {
    echo "ITEM-521: предвыделение стоит не во всех рецептах носителей: $item521pw_recipes" >&2
    exit 1
}
grep -Fq 'test-install-media-prealloc-host' Makefile || {
    echo "ITEM-521: host-проба предвыделения не зарегистрирована" >&2
    exit 1
}

# 4. The kernel allocates nothing: no allocation, no FAT entry write and no
#    directory-entry write survive in the module, and the reservation's own
#    refusals are named.
for item521pw_absent in \
    allocate_clusters \
    fat_entry_write \
    entry_set_name \
    ensure_directory \
    root_free_slot; do
    if grep -Fq "$item521pw_absent" kernel/src/install_fat12_journal.c; then
        echo "ITEM-521: модуль ядра всё ещё умеет аллокацию: $item521pw_absent" >&2
        exit 1
    fi
done
for item521pw_present in \
    FAT12_JOURNAL_REFUSED_NOT_RESERVED \
    FAT12_JOURNAL_REFUSED_BAD_HEADER \
    'fat12_journal_bind(' \
    'install_journal_crc32('; do
    if ! grep -Fq "$item521pw_present" \
        kernel/src/install_fat12_journal.c kernel/include/install_fat12_journal.h; then
        echo "ITEM-521: модуль ядра не несёт: $item521pw_present" >&2
        exit 1
    fi
done

# 5. The operator line is documented beside the boot-log reader, and the reader
#    names the reserved placement.
grep -Fq '### Reading the install journal the medium carries (ITEM-521)' \
    docs/hfoc-hardware-acceptance.md || {
    echo "ITEM-521: runbook не несёт раздела читалки журнала установки" >&2
    exit 1
}
grep -Fq 'python3 scripts/hfoc-install-log-read.py --disk /dev/sdb' \
    docs/hfoc-hardware-acceptance.md || {
    echo "ITEM-521: runbook не несёт строку читалки журнала установки" >&2
    exit 1
}
grep -Fq '\HFOC\INSTALL.LOG' docs/hfoc-hardware-acceptance.md || {
    echo "ITEM-521: runbook не называет размещение журнала" >&2
    exit 1
}
for item521pw_reader_pin in \
    '"fat12-preallocated"' 'def read_fat12_journal('; do
    grep -Fq "$item521pw_reader_pin" scripts/hfoc-install-log-read.py || {
        echo "ITEM-521: читалка не понимает размещение: $item521pw_reader_pin" >&2
        exit 1
    }
done
