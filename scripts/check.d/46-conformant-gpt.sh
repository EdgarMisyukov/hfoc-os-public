#!/usr/bin/env bash
# ITEM-414: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The partition table this installer writes is one other implementations have to
# accept.  A GPT reserves a partition entry array of 16 KiB - 128 entries of 128
# bytes - and the header carries both that count and the CRC of those bytes.
# This writer declared four entries over a single block, so every disk it
# produced described itself as a table a reader may refuse: this project's own
# reader refuses it (`installer_gpt_preserve.c` requires 128 entries), which is
# why the generated menu could not offer a system installed on a disk by this
# installer, and a firmware is free to refuse the same table.  FreeBSD's mkimg
# was fixed for exactly this.
#
# The slice pins the shape (the count, the array length the CRC covers, both
# headers naming their own array, and the fixture that stands in for an
# installed disk carrying the same shape) and the host proof that reads the
# table back out of the raw storage with its own CRC-32.
for gpt_shape in \
    'INSTALLER_LAYOUT_GPT_ENTRY_COUNT 128U' \
    'INSTALLER_LAYOUT_GPT_ARRAY_BLOCKS' \
    'installer_layout_store_le64(header + 72U, array_lba);' \
    'installer_layout_crc32_array'; do
    grep -Fq -- "$gpt_shape" kernel/src/installer_layout.c || {
        echo "писатель таблицы не описывает конформный GPT: $gpt_shape" >&2
        exit 1
    }
done
grep -Fq 'PHYSICAL_STORAGE_QEMU_GPT_ENTRY_COUNT 128U' \
    kernel/src/physical_storage_qemu.c || {
    echo "фикстура установленного диска несёт не ту таблицу" >&2
    exit 1
}
# shellcheck disable=SC2016  # the single-quoted text is what the proof says
for gpt_proof in \
    'the header declares 128 entries of 128 bytes' \
    'the array CRC is the CRC of the 16 KiB the disk carries' \
    'the backup header names the backup array, not the primary one' \
    'the backup table carries the same array and the same CRC'; do
    grep -Fq -- "$gpt_proof" tests/installer-layout-host.c || {
        echo "хостовое доказательство не проверяет поле таблицы: $gpt_proof" >&2
        exit 1
    }
done
# The install proof reads the table back out of the finished disk, so it must
# not accept a table shorter than the format's own array.
grep -Fq 'entry_count >= 4 and entry_size >= 128' \
    scripts/test-qemu-production-install-flow.sh || {
    echo "product-flow proof не читает таблицу записанного диска" >&2
    exit 1
}
