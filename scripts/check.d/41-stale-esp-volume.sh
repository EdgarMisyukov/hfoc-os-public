#!/usr/bin/env bash
# ITEM-405: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Installing onto a disk whose ESP region still carries a foreign filesystem.
# The acceptance run replaced the partition table of a disk that had held a
# Windows layout but left the old system's 100 MiB FAT32 inside the 33 MiB ESP
# role: the bind accepted it because it never read the volume's own total-sector
# count, so the role was never formatted and the staged boot set went to a
# geometry the volume does not have - and the refusal was published on serial
# only, on a machine with no serial capture.
for stale_esp_file in \
    kernel/include/efi_fat32_esp.h \
    kernel/src/efi_fat32_esp.c \
    kernel/include/production_installer_esp.h \
    kernel/src/production_installer_esp.c \
    kernel/src/production_installer_boot_stage.c \
    kernel/src/production_installer_layout.c \
    kernel/src/production_installer_preserve.c \
    kernel/include/console.h \
    kernel/src/console.c \
    tests/efi-fat32-esp-host.c \
    tests/production-installer-boot-stage-host.c \
    tests/console-host.c \
    scripts/test-qemu-production-install-flow.sh \
    scripts/test-qemu-hfoc-efi-installer.sh \
    docs/hfoc-installed-boot.md \
    docs/hfoc-installer-preservation.md \
    docs/hfoc-production-install-flow.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    test -f "$stale_esp_file" || {
        echo "Отсутствует ITEM-405 file: $stale_esp_file" >&2
        exit 1
    }
done

# The bind reads the volume's own declared extent (FAT32 offset 32, FAT16
# offset 19) and refuses a volume that does not fit the partition it was handed:
# zero extent, an extent larger than the partition, or a data region the FAT
# cannot describe inside it.
for stale_esp_bind in \
    'total_sectors16 = efi_fat32_load_le16(boot_sector + 19U)' \
    'total_sectors32 = efi_fat32_load_le32(boot_sector + 32U)' \
    'volume_sectors == 0U || volume_sectors > block_count' \
    'fat_region_blocks >= volume_sectors' \
    'EFI_FAT32_ESP_UNSUPPORTED_GEOMETRY'; do
    grep -Fq "$stale_esp_bind" kernel/src/efi_fat32_esp.c || {
        echo "ESP bind не проверяет объявленный объём тома: $stale_esp_bind" >&2
        exit 1
    }
done
# The cluster space comes from the declared volume, and the volume's extent is
# part of the bound geometry the caller can read back.
grep -Fq 'volume_sectors' kernel/include/efi_fat32_esp.h || {
    echo "ESP geometry не публикует объявленный объём тома" >&2
    exit 1
}
grep -Fq 'esp->cluster_count = cluster_count;' kernel/src/efi_fat32_esp.c || {
    echo "cluster space не выводится из объявленного объёма" >&2
    exit 1
}
# The module's bookkeeping is 32-bit: a sector count that does not fit it cannot
# be addressed correctly, so it is refused rather than truncated.
grep -Fq 'block_count > UINT32_MAX' kernel/src/efi_fat32_esp.c || {
    echo "ESP bind не отказывает объёму, не помещающемуся в 32-битную адресацию" >&2
    exit 1
}

# The role decides, not the bytes: an owned role is formatted before it is
# bound, a shared ESP is never formatted.
grep -Fq 'format_owned' kernel/include/production_installer_esp.h || {
    echo "ESP bind не называет владельца роли" >&2
    exit 1
}
for stale_esp_owned in \
    'if (!format_owned) {' \
    'production_esp_format(' \
    'production_installer_esp_bind('; do
    grep -Fq "$stale_esp_owned" kernel/src/production_installer_esp.c || {
        echo "installer ESP bind не различает владельца роли: $stale_esp_owned" >&2
        exit 1
    }
done
grep -Fq '!esp_target->reused' kernel/src/production_installer_boot_stage.c || {
    echo "boot stage не передаёт владельца ESP-роли" >&2
    exit 1
}
grep -Fq 'PRODUCTION_INSTALLER_ESP_UNSUITABLE' \
    kernel/src/production_installer_boot_stage.c || {
    echo "boot stage не отказывает общему ESP как ESP_UNSUITABLE" >&2
    exit 1
}

# The records an operator needs are mirrored to the framebuffer console, and the
# mirror never feeds the serial stream: it paints through console_emit(..., false).
grep -Fq 'CONSOLE_RECORD_MAX 320U' kernel/include/console.h || {
    echo "console mirror не ограничивает длину записи" >&2
    exit 1
}
grep -Fq 'console_emit(line[index], false)' kernel/src/console.c || {
    echo "console mirror пишет в serial" >&2
    exit 1
}
grep -Fq 'console_record_flush();' kernel/src/console.c || {
    echo "console mirror не выводит ранние записи после баннера" >&2
    exit 1
}
for stale_esp_mirror in \
    kernel/src/production_install_flow.c \
    kernel/src/production_installer_boot_stage.c \
    kernel/src/production_installer_layout.c \
    kernel/src/production_installer_preserve.c \
    kernel/src/kernel.c \
    kernel/src/xhci.c; do
    grep -Fq 'console_publish_journal' "$stale_esp_mirror" ||
        grep -Fq 'console_publish_record' "$stale_esp_mirror" ||
        grep -Fq 'console_record_' "$stale_esp_mirror" || {
        echo "$stale_esp_mirror не публикует свои записи" >&2
        exit 1
    }
done
for stale_esp_console_record in \
    '[PRODUCTION-NET] device=' \
    '[XHCI] state=' \
    '[KBD ] state='; do
    grep -Fq "$stale_esp_console_record" kernel/src/kernel.c \
        kernel/src/xhci.c || {
        echo "не найдена зеркалируемая запись: $stale_esp_console_record" >&2
        exit 1
    }
done

# The host proofs hold both halves: the bind rule and the ownership rule.
for stale_esp_bind_case in \
    'a volume declaring more sectors than its partition is refused' \
    'the refused volume publishes no geometry' \
    'a volume that declares no extent is refused' \
    'a volume that fits inside its partition binds' \
    'the cluster space is derived from the declared volume' \
    'nothing is written past the volume the partition holds'; do
    grep -Fq "$stale_esp_bind_case" tests/efi-fat32-esp-host.c || {
        echo "ESP host proof не проверяет: $stale_esp_bind_case" >&2
        exit 1
    }
done
for stale_esp_owned_case in \
    'the install stages into the role despite the stale volume' \
    "the ESP role carries the installer's own volume" \
    'the formatted role declares the partition it lives in' \
    'a shared ESP that does not fit its partition is refused' \
    'the shared-ESP refusal performs no write at all' \
    'the shared ESP keeps the foreign volume byte for byte' \
    'the shared-ESP refusal reaches the durable journal'; do
    grep -Fq "$stale_esp_owned_case" \
        tests/production-installer-boot-stage-host.c || {
        echo "boot stage proof не проверяет: $stale_esp_owned_case" >&2
        exit 1
    }
done
for stale_esp_console_case in \
    'a mirrored record is painted on the console' \
    'the mirror never writes to the serial stream' \
    'a mirrored record paints at most the bound' \
    'a republished record paints once, so the screen does not scroll' \
    'an early record is painted after the banner, once per boot' \
    'the incremental mirror paints the same record'; do
    grep -Fq "$stale_esp_console_case" tests/console-host.c || {
        echo "console proof не проверяет: $stale_esp_console_case" >&2
        exit 1
    }
done

# Host proof membership: the targets exist and the serial suite runs them.
for stale_esp_target in \
    test-efi-fat32-esp-host \
    test-production-installer-boot-stage-host \
    test-console-host; do
    grep -Fq "$stale_esp_target" Makefile || {
        echo "Makefile не содержит $stale_esp_target" >&2
        exit 1
    }
    # shellcheck disable=SC2016
    grep -Fq "\$(MAKE) $stale_esp_target" Makefile || {
        echo "test-host-suite не вызывает $stale_esp_target" >&2
        exit 1
    }
done

# The QEMU proof prepares the acceptance host's target shape and reads the
# result back out of the raw image, independently of the kernel.
for stale_esp_qemu in \
    'prepare_stale_esp_disk' \
    'assert_hfoc_esp_geometry' \
    'assert_stale_tree_unreachable' \
    'STALE_ESP_LENGTH=67584' \
    'total_sectors=204800' \
    "oem == b\"HFOC    \""; do
    grep -Fq "$stale_esp_qemu" \
        scripts/test-qemu-production-install-flow.sh || {
        echo "QEMU proof не проверяет: $stale_esp_qemu" >&2
        exit 1
    }
done
# shellcheck disable=SC2016  # the Make variable is the text the recipe carries
grep -Fq "WHOLE_DISK_IMAGE='\$(PRODUCTION_INSTALLER_WHOLE_DISK_ISO)'" \
    Makefile || {
    echo "install-flow QEMU proof не запускается объявленным whole-disk носителем" >&2
    exit 1
}
# The EFI-helper fixture formats its ESP to the size of the partition it has:
# mformat would otherwise size the volume to the whole image, which is exactly
# the volume that does not fit its partition.
# shellcheck disable=SC2016  # the single-quoted text is what the script says
grep -Fq 'mformat -i "$DISK"@@$ESP_OFFSET -F -T "$ESP_SECTORS" ::' \
    scripts/test-qemu-hfoc-efi-installer.sh || {
    echo "EFI-helper fixture не описывает том размером своего раздела" >&2
    exit 1
}

# Documented: the item, the bind rule and the console publication.
for stale_esp_doc in \
    docs/hfoc-installed-boot.md \
    docs/hfoc-installer-preservation.md \
    docs/hfoc-production-install-flow.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    grep -Fq 'ITEM-405' "$stale_esp_doc" || {
        echo "$stale_esp_doc не документирует ITEM-405" >&2
        exit 1
    }
done
grep -Fq 'PRODUCTION_INSTALLER_ESP_UNSUITABLE' \
    docs/hfoc-installer-preservation.md || {
    echo "preservation doc не фиксирует ESP_UNSUITABLE для общего ESP" >&2
    exit 1
}
grep -Fq 'console_publish_record' \
    docs/hfoc-production-install-flow.md || {
    echo "install-flow doc не фиксирует публикацию записей на консоль" >&2
    exit 1
}
