#!/usr/bin/env bash
# ITEM-509 increment 16: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-509 increment 16 (509.7c-1b-3): the disk list reads the one catalog.  The
# renderer takes the language as an explicit parameter and the screen session
# carries it from the caller; since increment 27 the boot passes the language the
# medium declared (a medium that declares none keeps the default).  The English
# list is byte for byte what it was, so the QEMU pins and
# the [INSTALLER-STORAGE] records do not move; the cursor, the indents, the
# disk's own name and identity, the size and the state words stay machine
# values.
for item509_inc16_file in \
    kernel/src/installer_storage.c \
    kernel/src/installer_storage_screen.c \
    kernel/include/installer_storage.h \
    kernel/include/installer_storage_screen.h \
    tests/installer-storage-host.c \
    docs/hfoc-language-surfaces.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc16_file ]] || {
        echo "ITEM-509 inc16: отсутствует $item509_inc16_file" >&2
        exit 1
    }
done

# 1. The disk list reads every prose key it prints, and the English literals it
#    used to print are gone, so the two cannot drift apart.
for item509_inc16_key in \
    '"installer.storage.select_disk"' \
    '"installer.storage.no_disk"' \
    '"installer.storage.status_prefix"' \
    '"installer.storage.unidentified_disk"' \
    '"installer.storage.transport_prefix"' \
    '"installer.storage.table_prefix"' \
    '"installer.storage.state_prefix"' \
    '"installer.storage.existing_system"' \
    '"installer.storage.efi_partition"' \
    '"installer.storage.confirmed_hint"' \
    '"installer.storage.selected_hint"' \
    '"installer.storage.move_hint"' \
    '"installer.storage.chosen_note"' \
    '"installer.storage.not_chosen_note"'; do
    grep -Fq -- "$item509_inc16_key" kernel/src/installer_storage.c || {
        echo "ITEM-509 inc16: список дисков не читает каталог: $item509_inc16_key" >&2
        exit 1
    }
done
for item509_inc16_gone in \
    '"Select installation disk"' \
    '"  No disk was found"' \
    '"  Status: UNAVAILABLE"' \
    '"   Existing system: YES"' \
    '"   EFI system partition: YES"' \
    '"  Confirmed. ENTER writes it, Q goes back."' \
    '"  Selected. ENTER shows what will be erased."' \
    '"Disk chosen by the operator.' \
    '"No disk is chosen.'; do
    if grep -Fq -- "$item509_inc16_gone" kernel/src/installer_storage.c; then
        echo "ITEM-509 inc16: английская проза осталась литералом: $item509_inc16_gone" >&2
        exit 1
    fi
done
grep -Fq 'INSTALLER TRANSLATION MISSING' kernel/src/installer_storage.c || {
    echo "ITEM-509 inc16: список дисков не называет отказ перевода" >&2
    exit 1
}

# 2. The language travels as the render's own parameter through the screen
#    session, and the boot passes the default until the medium can declare one.
grep -Fq 'enum appliance_language language,' kernel/include/installer_storage.h || {
    echo "ITEM-509 inc16: список дисков не принимает язык" >&2
    exit 1
}
grep -Fq 'installer_storage_lines(model, language, lines)' \
    kernel/src/installer_storage_screen.c || {
    echo "ITEM-509 inc16: экран не передаёт язык списку" >&2
    exit 1
}
grep -Fq 'installer_storage_show(&model, language);' \
    kernel/src/installer_storage_screen.c || {
    echo "ITEM-509 inc16: сессия не передаёт язык экрану" >&2
    exit 1
}
grep -Fq 'enum appliance_language language);' \
    kernel/include/installer_storage_screen.h || {
    echo "ITEM-509 inc16: сессия не принимает язык" >&2
    exit 1
}
grep -Fq 'kernel_installer_declared_language()) !=' kernel/src/kernel.c || {
    echo "ITEM-509 inc16: загрузка не передаёт язык экрану дисков" >&2
    exit 1
}

# 3. The proof renders the Russian list and the named refusal, and the target
#    links the catalog.
grep -Fq 'installer_storage_lines(&model, APPLIANCE_LANGUAGE_RU, lines)' \
    tests/installer-storage-host.c || {
    echo "ITEM-509 inc16: проба не рисует русский список" >&2
    exit 1
}
grep -Fq 'ITEM-509: the disk list comes from the Russian catalog column' \
    tests/installer-storage-host.c || {
    echo "ITEM-509 inc16: проба не называет русский список" >&2
    exit 1
}
grep -Fq 'ITEM-509: an unknown language renders the named refusal, not half ' \
    tests/installer-storage-host.c || {
    echo "ITEM-509 inc16: проба не требует именованного отказа" >&2
    exit 1
}
grep -Fq 'kernel/src/appliance_language.c' Makefile || {
    echo "ITEM-509 inc16: проба не линкует каталог" >&2
    exit 1
}

# 4. The increment is recorded where the next window reads it.
grep -Fq 'ITEM-509 increment 16' docs/hfoc-language-surfaces.md || {
    echo "ITEM-509 inc16: поверхности языка не называют инкремент" >&2
    exit 1
}
grep -Fq '509.7c-1b-3' docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-509 inc16: очередь не называет половину инкремента" >&2
    exit 1
}
