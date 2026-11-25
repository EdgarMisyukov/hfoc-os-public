#!/usr/bin/env bash
# ITEM-509 increment 18: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-509 increment 18 (509.7c-1b-4): the disk warning page and the two
# partition screens (the list and the warning page) read the one catalog in the
# caller's language, exactly as the disk list did in increment 16.  The English
# screens stay byte for byte what they were - the QEMU markers
# `WARNING: EVERYTHING ON THIS DISK WILL BE DELETED`,
# `Give up one existing partition` and
# `WARNING: THE DATA ON THIS PARTITION WILL BE DELETED` read the rendered screen,
# not this file - and the cursor, the device index, the capacity, the kind and
# layout names, the state words and the [INSTALLER-STORAGE] records stay machine
# values.  A key the catalog cannot answer is the named refusal, not half a
# screen.  Since increment 27 every caller passes the language the medium
# declared (a medium that declares none keeps the default).
for item509_inc18_file in \
    kernel/src/installer_storage.c \
    kernel/src/installer_storage_screen.c \
    kernel/include/installer_storage.h \
    kernel/include/installer_storage_screen.h \
    tests/installer-storage-host.c \
    docs/hfoc-language-surfaces.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc18_file ]] || {
        echo "ITEM-509 inc18: отсутствует $item509_inc18_file" >&2
        exit 1
    }
done

# 1. All three renderers read every prose key they print: the disk warning...
for item509_inc18_warning_key in \
    '"installer.storage.warning_no_disk"' \
    '"installer.storage.nothing_written"' \
    '"installer.storage.warning_title"' \
    '"installer.storage.disk_prefix"' \
    '"installer.storage.unidentified_disk"' \
    '"installer.storage.capacity_prefix"' \
    '"installer.storage.warning_transport_prefix"' \
    '"installer.storage.table_prefix"' \
    '"installer.storage.has_system_note"' \
    '"installer.storage.has_efi_note"' \
    '"installer.storage.no_system_note"' \
    '"installer.storage.erase_note"' \
    '"installer.storage.settings_note"' \
    '"installer.storage.settings_detail"' \
    '"installer.storage.not_written_yet"' \
    '"installer.storage.erase_footer"'; do
    grep -Fq -- "$item509_inc18_warning_key" kernel/src/installer_storage.c || {
        echo "ITEM-509 inc18: предупреждение о диске не читает каталог: $item509_inc18_warning_key" >&2
        exit 1
    }
done
# ...the partition list...
for item509_inc18_partition_key in \
    '"installer.storage.partition_title"' \
    '"installer.storage.free_space_prefix"' \
    '"installer.storage.mb_needed_middle"' \
    '"installer.storage.mb_suffix"' \
    '"installer.storage.partition_not_enough"' \
    '"installer.storage.partition_none"' \
    '"installer.storage.partition_unchanged"' \
    '"installer.storage.partition_prefix"' \
    '"installer.storage.partition_system_prefix"' \
    '"installer.storage.partition_move_hint"' \
    '"installer.storage.partition_keep_note"'; do
    grep -Fq -- "$item509_inc18_partition_key" kernel/src/installer_storage.c || {
        echo "ITEM-509 inc18: список разделов не читает каталог: $item509_inc18_partition_key" >&2
        exit 1
    }
done
# ...and the partition warning page.
for item509_inc18_partition_warning_key in \
    '"installer.storage.partition_warning_no_partition"' \
    '"installer.storage.partition_warning_title"' \
    '"installer.storage.partition_field_prefix"' \
    '"installer.storage.size_prefix"' \
    '"installer.storage.type_prefix"' \
    '"installer.storage.layout_prefix"' \
    '"installer.storage.system_prefix"' \
    '"installer.storage.disk_field_prefix"' \
    '"installer.storage.partition_removed_note"' \
    '"installer.storage.partition_created_note"' \
    '"installer.storage.partition_others_note"' \
    '"installer.storage.partition_others_detail"' \
    '"installer.storage.partition_footer"'; do
    grep -Fq -- "$item509_inc18_partition_warning_key" \
        kernel/src/installer_storage.c || {
        echo "ITEM-509 inc18: предупреждение о разделе не читает каталог: $item509_inc18_partition_warning_key" >&2
        exit 1
    }
done

# 2. The English prose the three renderers used to carry is gone from the
#    renderer, so a key and its printed sentence cannot drift apart.
for item509_inc18_gone in \
    '"WARNING: EVERYTHING ON THIS DISK WILL BE DELETED"' \
    '"No disk is selected"' \
    '"Nothing will be written."' \
    '"  Disk: "' \
    '"  Capacity: "' \
    '"UNIDENTIFIED DISK"' \
    '"  An existing system is installed on this disk."' \
    '"  This disk carries an EFI system partition."' \
    '"  The disk holds no "' \
    '"  Everything on it - partitions, "' \
    '"  The appliance'"'"'s own settings are erased too:"' \
    '"  network address, administrator credential, certificates."' \
    '"  Nothing has been written yet."' \
    '"ENTER erase this disk and install"' \
    '"Give up one existing partition"' \
    '"  Free space: "' \
    '" MB   Needed: "' \
    '"  Not enough free space: give up a partition, or Q "' \
    '"  This disk has no partition that can be given up"' \
    '"  Nothing was changed. Q continues with free space."' \
    '"Partition "' \
    '"     System: "' \
    '"W/S or arrow keys move   ENTER choose   "' \
    '"Q keeps every partition and continues with free space."' \
    '"No partition is selected"' \
    '"WARNING: THE DATA ON THIS PARTITION WILL BE DELETED"' \
    '"  Partition: "' \
    '"   Size: "' \
    '"   Type: "' \
    '"  Layout: "' \
    '"   System: "' \
    '"   Disk: "' \
    '"  This partition is removed and the appliance'"'"'s three"' \
    '"  partitions are created in the space it occupied."' \
    '"  Every other partition, the shared EFI System"' \
    '"  Partition and the systems on them stay untouched."' \
    '"ENTER give up this partition and install   Q go back"'; do
    if grep -Fq -- "$item509_inc18_gone" kernel/src/installer_storage.c; then
        echo "ITEM-509 inc18: английская проза осталась литералом: $item509_inc18_gone" >&2
        exit 1
    fi
done
grep -Fq 'INSTALLER TRANSLATION MISSING' kernel/src/installer_storage.c || {
    echo "ITEM-509 inc18: экраны не называют отказ перевода" >&2
    exit 1
}

# 3. The machine values the screens print stay machine values, and the screen
#    only reads the catalog: it never parses or writes the setting.
grep -Fq 'return "EFI system";' kernel/src/installer_storage.c || {
    echo "ITEM-509 inc18: имя раздела EFI перестало быть машинным" >&2
    exit 1
}
grep -Fq 'return "Linux";' kernel/src/installer_storage.c || {
    echo "ITEM-509 inc18: имя системы Linux перестало быть машинным" >&2
    exit 1
}
if grep -Fq 'appliance_language_parse' kernel/src/installer_storage.c ||
    grep -Fq 'appliance_persistent_config_set_language' \
        kernel/src/installer_storage.c; then
    echo "ITEM-509 inc18: экран предупреждений пишет язык, а он только читает" >&2
    exit 1
fi

# 4. The language travels as the render's own parameter through the screen
#    session, and the boot passes the default until the medium can declare one.
grep -Fq 'enum appliance_language language,' kernel/include/installer_storage.h || {
    echo "ITEM-509 inc18: рендереры не принимают язык" >&2
    exit 1
}
# ITEM-514: the disk warning is now the erase scheme's page, and the model
# composes it (the ITEM-495 page plus the unknown-contents notice), so the
# exact line array is host-proven; the language still travels as the render's
# parameter.
grep -Fq 'installer_storage_warning_lines(&model->disk, language,' \
    kernel/src/installer_storage.c || {
    echo "ITEM-509 inc18: экран не передаёт язык предупреждению о диске" >&2
    exit 1
}
grep -Fq 'installer_storage_partition_lines(model, language, lines)' \
    kernel/src/installer_storage_screen.c || {
    echo "ITEM-509 inc18: экран не передаёт язык списку разделов" >&2
    exit 1
}
grep -Fq 'installer_storage_partition_warning_lines(partition, model, language,' \
    kernel/src/installer_storage_screen.c || {
    echo "ITEM-509 inc18: экран не передаёт язык предупреждению о разделе" >&2
    exit 1
}
grep -Fq 'installer_storage_show_partitions(&model, language)' \
    kernel/src/installer_storage_screen.c || {
    echo "ITEM-509 inc18: сессия разделов не передаёт язык" >&2
    exit 1
}
grep -Fq 'enum appliance_language language);' \
    kernel/include/installer_storage_screen.h || {
    echo "ITEM-509 inc18: сессия разделов не принимает язык" >&2
    exit 1
}
grep -Fq 'required_lba, kernel_installer_declared_language()) ==' \
    kernel/src/kernel.c || {
    echo "ITEM-509 inc18: загрузка не передаёт язык экрану разделов" >&2
    exit 1
}

# 5. The proof renders the Russian screens and the named refusal.
grep -Fq 'installer_storage_warning_lines(disk, APPLIANCE_LANGUAGE_RU, lines)' \
    tests/installer-storage-host.c || {
    echo "ITEM-509 inc18: проба не рисует русское предупреждение о диске" >&2
    exit 1
}
grep -Fq 'ITEM-509: the disk warning comes from the Russian catalog column' \
    tests/installer-storage-host.c || {
    echo "ITEM-509 inc18: проба не называет русское предупреждение о диске" >&2
    exit 1
}
grep -Fq 'ITEM-509: the partition list comes from the Russian catalog column' \
    tests/installer-storage-host.c || {
    echo "ITEM-509 inc18: проба не называет русский список разделов" >&2
    exit 1
}
grep -Fq 'ITEM-509: the partition warning comes from the Russian catalog ' \
    tests/installer-storage-host.c || {
    echo "ITEM-509 inc18: проба не называет русское предупреждение о разделе" >&2
    exit 1
}
grep -Fq 'ITEM-509: an unknown language renders the named refusal on the disk ' \
    tests/installer-storage-host.c || {
    echo "ITEM-509 inc18: проба не требует именованного отказа" >&2
    exit 1
}
grep -Fq 'kernel/src/appliance_language.c' Makefile || {
    echo "ITEM-509 inc18: проба не линкует каталог" >&2
    exit 1
}

# 6. The increment is recorded where the next window reads it.
grep -Fq 'ITEM-509 increment 18' docs/hfoc-language-surfaces.md || {
    echo "ITEM-509 inc18: поверхности языка не называют инкремент" >&2
    exit 1
}
grep -Fq '509.7c-1b-4' docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-509 inc18: очередь не называет половину инкремента" >&2
    exit 1
}
