#!/usr/bin/env bash
# ITEM-509 increment 13: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-509 increment 13 (509.7c-1a): the installer-screen prose enters the one
# catalog.  This first half is add-only on purpose: nothing reads the new keys
# yet, so no rendered screen, no installer record and no QEMU marker can move.
# The English values are the literals the renderers print today, concatenated
# exactly as the compiler concatenates adjacent literals; the second half
# (509.7c-1b) threads the language parameter through the renderers, and that
# increment updates this slice when it makes the screens read these keys.
for item509_inc13_file in \
    kernel/src/appliance_language.c \
    tests/appliance-language-host.c \
    docs/hfoc-language-surfaces.md \
    docs/ROADMAP.md \
    docs/ARCHITECTURE.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc13_file ]] || {
        echo "ITEM-509 inc13: отсутствует $item509_inc13_file" >&2
        exit 1
    }
done

# 1. The catalog carries the installer vocabulary, English verbatim and Russian
#    beside it.  The move hints keep their exact runs of spaces, and the two
#    warning sentences that the screen writes as two lines stay two keys.
for item509_inc13_key in \
    '{"installer.foundation.title", "HFOC Installer", "Установщик HFOC"}' \
    '{"installer.foundation.footer", "W/S move   ENTER select   Q quit",' \
    '{"installer.foundation.row_storage", "Storage", "Хранилище"}' \
    '{"installer.storage.select_disk", "Select installation disk",' \
    '{"installer.storage.unidentified_disk", "UNIDENTIFIED DISK",' \
    '{"installer.storage.status_prefix", "  Status: ", "  Состояние: "}' \
    '{"installer.storage.existing_system", "   Existing system: YES",' \
    '{"installer.storage.efi_partition", "   EFI system partition: YES",' \
    '{"installer.storage.warning_title",' \
    '{"installer.storage.settings_note",' \
    '{"installer.storage.settings_detail",' \
    '{"installer.storage.erase_footer",' \
    '{"installer.storage.partition_title", "Give up one existing partition",' \
    '{"installer.storage.partition_warning_title",' \
    '{"installer.storage.partition_field_prefix", "  Partition: ",' \
    '{"installer.storage.partition_footer",' \
    '{"installer.refusal.title", "Installation refused",' \
    '{"installer.refusal.reason_prefix", "  Reason: ", "  Причина: "}' \
    '{"installer.credential.title", "HFOC OS installer - account",' \
    '{"installer.credential.password_note",' \
    '{"installer.notice.remove_medium",' \
    '{"installer.media.declared_disk_prefix", "Declared disk ",' \
    '{"installer.media.not_on_machine_suffix",'; do
    grep -Fq "$item509_inc13_key" kernel/src/appliance_language.c || {
        echo "ITEM-509 inc13: каталог не несёт: $item509_inc13_key" >&2
        exit 1
    }
done
grep -Fq '"W/S or arrow keys move   ENTER choose   Q leave"' \
    kernel/src/appliance_language.c || {
    echo "ITEM-509 inc13: подсказка списка дисков потеряла пробелы" >&2
    exit 1
}
grep -Fq '"ENTER erase this disk and install   Q go back"' \
    kernel/src/appliance_language.c || {
    echo "ITEM-509 inc13: подвал стирания диска потерял пробелы" >&2
    exit 1
}
for item509_inc13_split in \
    '{"installer.storage.partition_removed_note",' \
    '{"installer.storage.partition_created_note",' \
    '{"installer.storage.partition_others_note",' \
    '{"installer.storage.partition_others_detail",'; do
    grep -Fq "$item509_inc13_split" kernel/src/appliance_language.c || {
        echo "ITEM-509 inc13: двухстрочные предупреждения не разделены" >&2
        exit 1
    }
done

# 2. ITEM-509 increment 14 wired the foundation screen: `installer_ui.c` reads
#    the foundation and field-label keys, while the remaining installer surfaces
#    are still add-only until 509.7c-1b-2/3.  The module the console shares with
#    the installer stays out of the increment (509.7d).
for item509_inc13_wired in \
    '"installer.foundation.title"' \
    '"installer.foundation.footer"' \
    '"installer.foundation.row_display"' \
    '"installer.foundation.storage_detail"' \
    '"installer.ui.backend_prefix"' \
    '"installer.ui.fallback_prefix"'; do
    grep -Fq -- "$item509_inc13_wired" kernel/src/installer_ui.c || {
        echo "ITEM-509 inc14: фундаментный экран не читает каталог: $item509_inc13_wired" >&2
        exit 1
    }
done
# ITEM-509 increments 15 and 16 wired the refusal page and the declared-disk
# note, then the disk list (installer_storage.c); increment 18 wired the disk
# warning and the two partition screens; increment 19 wired the install banner
# (installer_notice.c); increment 20 wired the credential conversation the
# installer account screen shares with the console.  Every installer surface of
# 509.7c now reads the catalog, so the add-only guard of 509.7c-1a is gone.
for item509_inc20_screen_key in \
    '"installer.credential.title"' \
    '"installer.credential.password_note"'; do
    grep -Fq -- "$item509_inc20_screen_key" \
        kernel/src/installer_credential_screen.c || {
        echo "ITEM-509 inc20: экран учётной записи не читает каталог: $item509_inc20_screen_key" >&2
        exit 1
    }
done
grep -Fq '"installer.storage.select_disk"' kernel/src/installer_storage.c || {
    echo "ITEM-509 inc16: список дисков не читает каталог" >&2
    exit 1
}
# ITEM-509 increment 18: the three installer_storage.c screens that were still
# add-only now read the same installer.storage.* keys.
for item509_inc18_key in \
    '"installer.storage.warning_title"' \
    '"installer.storage.partition_title"' \
    '"installer.storage.partition_warning_title"'; do
    grep -Fq -- "$item509_inc18_key" kernel/src/installer_storage.c || {
        echo "ITEM-509 inc18: экран не читает каталог: $item509_inc18_key" >&2
        exit 1
    }
done
# ITEM-509 increment 19: the banner's title and footer come from the catalog
# too, so the last installer surface of 509.7c reads the same table.
for item509_inc19_key in \
    '"installer.notice.title"' \
    '"installer.notice.remove_medium"'; do
    grep -Fq -- "$item509_inc19_key" kernel/src/installer_notice.c || {
        echo "ITEM-509 inc19: баннер не читает каталог: $item509_inc19_key" >&2
        exit 1
    }
done
grep -Fq 'INSTALLER TRANSLATION MISSING' kernel/src/installer_ui.c || {
    echo "ITEM-509 inc14: нет именованного отказа перевода" >&2
    exit 1
}
grep -Fq 'APPLIANCE_LANGUAGE_EN, installer_ui_row_key(row->id)' \
    kernel/src/installer_ui.c || {
    echo "ITEM-509 inc14: запись установщика следует за языком экрана" >&2
    exit 1
}
# ITEM-509 increment 20: the shared credential conversation entered the catalog,
# so the add-only guard that kept it out is gone; the keys are pinned by slice
# 120 and the new slice 183.
grep -Fq '"credential.prompt.new_login"' \
    kernel/src/appliance_admin_credentials_prompt.c || {
    echo "ITEM-509 inc20: разговор учётной записи не читает каталог" >&2
    exit 1
}

# 3. The proof bound moves with the measured count: 214 entries before the
#    increment, 289 after (75 installer keys), so the ceiling is 384 and the
#    comment records why.  ITEM-509 increments 20-25 raised it to 384 entries
#    measured; ITEM-516 raised it to 416 with 401 measured (the gate's console
#    vocabulary, its warning and its panel page); ITEM-647 raised it to 448
#    with 432 measured (one label for every visible form control).
grep -Fq 'CHECK(count <= 448U, "catalog stays a small table (%zu)", count);' \
    tests/appliance-language-host.c || {
    echo "ITEM-509 inc13: потолок каталога не поднят осознанно" >&2
    exit 1
}
grep -Fq 'ITEM-509 increment 13 raised the ceiling from 256 to 384' \
    tests/appliance-language-host.c || {
    echo "ITEM-509 inc13: причина подъёма потолка не записана" >&2
    exit 1
}

# 4. The increment is recorded where the next window reads it.
grep -Fq 'ITEM-509 increment 13' docs/hfoc-language-surfaces.md || {
    echo "ITEM-509 inc13: поверхности языка не называют инкремент" >&2
    exit 1
}
grep -Fq '509.7c-1a' docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-509 inc13: очередь не называет половину инкремента" >&2
    exit 1
}

# 5. ITEM-509 increment 14 (509.7c-1b-1): the foundation screen takes the
#    language as an explicit parameter, the boot passes the default until the
#    medium can declare one, the proof renders Russian and the named refusal,
#    and the record stays English in a Russian screen.
grep -Fq 'void installer_ui_foundation(struct installer_ui_model *model,' \
    kernel/include/installer_ui.h || {
    echo "ITEM-509 inc14: фундаментный экран не принимает язык" >&2
    exit 1
}
grep -Fq 'enum appliance_language language,' kernel/include/installer_ui.h || {
    echo "ITEM-509 inc14: заголовок не называет параметр языка" >&2
    exit 1
}
grep -Fq 'void display_show_installer_foundation(enum appliance_language language);' \
    kernel/include/display_firmware.h || {
    echo "ITEM-509 inc14: экран дисплея не принимает язык" >&2
    exit 1
}
grep -Fq 'installer_ui_foundation(&model, language);' \
    kernel/src/display_firmware.c || {
    echo "ITEM-509 inc14: текстовый путь не передаёт язык" >&2
    exit 1
}
grep -Fq 'installer_ui_foundation(&model, APPLIANCE_LANGUAGE_RU);' \
    tests/installer-ui-host.c || {
    echo "ITEM-509 inc14: проба не рисует русский экран" >&2
    exit 1
}
grep -Fq 'installer_ui_foundation(&model, (enum appliance_language)7);' \
    tests/installer-ui-host.c || {
    echo "ITEM-509 inc14: проба не требует именованного отказа" >&2
    exit 1
}
grep -Fq 'kernel/src/appliance_language.c' Makefile || {
    echo "ITEM-509 inc14: проба не линкует каталог" >&2
    exit 1
}
