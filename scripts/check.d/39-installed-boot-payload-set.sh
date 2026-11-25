#!/usr/bin/env bash
# ITEM-397: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Booting the installed system from the disk. The install flow stages a bounded
# boot payload set into the ESP, refuses an ESP that cannot hold it before the
# first write, publishes which mechanism makes the firmware find the disk, and
# proves the installed disk boots in QEMU.
for installed_boot_file in \
    kernel/include/efi_fat32_esp.h \
    kernel/src/efi_fat32_esp.c \
    kernel/include/production_installer_boot_stage.h \
    kernel/src/production_installer_boot_stage.c \
    tests/efi-fat32-esp-payload-set-host.c \
    tests/production-installer-boot-stage-host.c \
    tests/installed-boot-disk-host.c \
    scripts/test-qemu-installed-boot.sh \
    docs/hfoc-installed-boot.md; do
    test -f "$installed_boot_file" || {
        echo "Отсутствует ITEM-397 file: $installed_boot_file" >&2
        exit 1
    }
done

# The payload-set bounds are explicit and single-sourced in the header.
for installed_boot_bound in \
    'EFI_FAT32_ESP_PAYLOAD_SET_MAX 7U' \
    'EFI_FAT32_ESP_FILE_MAX 16777216U' \
    'EFI_FAT32_ESP_PAYLOAD_SET_BYTES_MAX 16777216U' \
    'EFI_FAT32_ESP_PATH_MAX 64U' \
    'EFI_FAT32_ESP_FAT_SECTORS_MAX 1024U' \
    'EFI_FAT32_ESP_MIN_FAT32_CLUSTERS 65525U' \
    'EFI_FAT32_ESP_PATH_CONFLICT' \
    'EFI_FAT32_ESP_SET_TOO_LARGE'; do
    grep -Fq "$installed_boot_bound" kernel/include/efi_fat32_esp.h || {
        echo "ESP payload set не фиксирует bound: $installed_boot_bound" >&2
        exit 1
    }
done

# The installed paths and the mechanism record are contract text: the
# configuration must reference the kernel the installer staged, and the record
# must name the mechanism that makes the firmware find the disk.
for installed_boot_record in \
    'PRODUCTION_INSTALLER_BOOT_LOADER_PATH "\\EFI\\BOOT\\BOOTX64.EFI"' \
    'PRODUCTION_INSTALLER_BOOT_KERNEL_PATH "\\boot\\hfoc-kernel.elf"' \
    'PRODUCTION_INSTALLER_BOOT_CONFIG_PATH "\\boot\\limine\\limine.conf"' \
    'PRODUCTION_INSTALLER_BOOT_MANIFEST_PATH "\\boot\\hfoc-boot.man"' \
    'PRODUCTION_INSTALLER_BOOT_ROOTFS_PATH "\\boot\\hfoc-rootfs.arc"' \
    'PRODUCTION_INSTALLER_BOOT_CONFIG_REFERENCE' \
    'PRODUCTION_INSTALLER_BOOT_MECHANISM_REMOVABLE_MEDIA_PATH' \
    'PRODUCTION_INSTALLER_BOOT_MECHANISM_EFI_BOOT_VARIABLE'; do
    grep -Fq "$installed_boot_record" \
        kernel/include/production_installer_boot_stage.h || {
        echo "installed boot stage не фиксирует: $installed_boot_record" >&2
        exit 1
    }
done
grep -Fq 'PRODUCTION_INSTALLER_ESP_TOO_SMALL' \
    kernel/include/production_installer.h || {
    echo "controller не фиксирует ESP-too-small отказ" >&2
    exit 1
}
grep -Fq 'PRODUCTION_INSTALLER_ESP_TOO_SMALL' \
    kernel/src/production_installer.c || {
    echo "controller не публикует ESP-too-small статус" >&2
    exit 1
}
grep -Fq '"REMOVABLE_MEDIA_PATH"' \
    kernel/src/production_installer_boot_stage.c || {
    echo "boot stage не публикует removable-media механизм" >&2
    exit 1
}
for installed_boot_sequence in \
    'serial_sequence_begin' \
    'serial_sequence_end' \
    '[PRODUCTION-INSTALL] boot-entry '; do
    grep -Fq "$installed_boot_sequence" \
        kernel/src/production_installer_boot_stage.c || {
        echo "boot-entry record не публикуется одной serial sequence: $installed_boot_sequence" >&2
        exit 1
    }
done

# The payload-set writer keeps the whole-set precheck before the first write,
# and the formatter writes a mountable FAT32 volume.
grep -Fq 'EFI_FAT32_ESP_CAPACITY' kernel/src/efi_fat32_esp.c || {
    echo "payload set не отказывает по capacity" >&2
    exit 1
}
grep -Fq 'efi_fat32_esp_payload_requirement' \
    kernel/include/efi_fat32_esp.h || {
    echo "payload set не публикует требование к ESP" >&2
    exit 1
}
grep -Fq 'efi_fat32_esp_payload_requirement' \
    kernel/src/production_installer_boot_stage.c || {
    echo "boot stage не сравнивает требование с ESP роли" >&2
    exit 1
}
grep -Fq 'production_esp_sectors_per_fat' \
    kernel/src/production_installer_esp.c || {
    echo "ESP formatter не выводит размер FAT из объёма" >&2
    exit 1
}
for installed_boot_bpb in \
    '0x41615252U' \
    'FAT32   '; do
    grep -Fq "$installed_boot_bpb" \
        kernel/src/production_installer_esp.c || {
        echo "ESP formatter не пишет полный FAT32 BPB: $installed_boot_bpb" >&2
        exit 1
    }
done

# The declared ESP role must be a spec-valid FAT32 volume: at least
# EFI_FAT32_ESP_MIN_FAT32_CLUSTERS data sectors after the reserved region and
# two FAT copies. The check re-derives it from the two headers.
installed_boot_dir=$(mktemp -d)
trap 'rm -rf "$installed_boot_dir"' EXIT
cat >"$installed_boot_dir/values.c" <<'EOF'
#include <stdio.h>

#include "efi_fat32_esp.h"
#include "installer_layout_contract.h"

int main(void)
{
    printf("%llu %llu %llu\n",
           (unsigned long long)
               INSTALLER_LAYOUT_CONTRACT_EFI_SYSTEM_LENGTH_LBA,
           (unsigned long long)EFI_FAT32_ESP_MIN_FAT32_CLUSTERS,
           (unsigned long long)EFI_FAT32_ESP_FAT_SECTORS_MAX);
    return 0;
}
EOF
gcc -std=gnu11 -Wall -Wextra -Werror -Wpedantic -Ikernel/include \
    "$installed_boot_dir/values.c" -o "$installed_boot_dir/values" || {
    echo "installed boot headers не компилируются отдельно" >&2
    exit 1
}
read -r installed_boot_esp installed_boot_clusters installed_boot_fat_max \
    < <("$installed_boot_dir/values")
installed_boot_fat=$(( (2 + installed_boot_esp + 127) / 128 ))
installed_boot_data=$(( installed_boot_esp - 32 - 2 * installed_boot_fat ))
[[ "$installed_boot_fat" -le "$installed_boot_fat_max" ]] || {
    echo "ESP role требует FAT больше bound: $installed_boot_fat" >&2
    exit 1
}
[[ "$installed_boot_data" -ge "$installed_boot_clusters" ]] || {
    echo "ESP role не является FAT32 объёмом: $installed_boot_data кластеров" >&2
    exit 1
}

# Host proof membership: the targets exist and the serial suite runs them.
for installed_boot_target in \
    test-efi-fat32-esp-payload-set-host \
    test-production-installer-boot-stage-host \
    test-installed-boot-disk-host; do
    grep -Fq "$installed_boot_target" Makefile || {
        echo "Makefile не содержит $installed_boot_target" >&2
        exit 1
    }
    # shellcheck disable=SC2016
    grep -Fq "\$(MAKE) $installed_boot_target" Makefile || {
        echo "test-host-suite не вызывает $installed_boot_target" >&2
        exit 1
    }
    grep -Fq "$installed_boot_target" \
        tests/test-host-suite-manifest.sh || {
        echo "host suite manifest не пинует $installed_boot_target" >&2
        exit 1
    }
done

# The product install flow stages the set itself: the pre-write gate runs
# before the layout write, the boot stage replaces the single-payload install,
# and the boot-entry record is published once, by the boot stage.
grep -Fq 'production_installer_boot_stage_requirements' \
    kernel/src/production_install_flow.c || {
    echo "install flow не вызывает pre-write gate" >&2
    exit 1
}
grep -Fq 'production_installer_boot_stage_run' \
    kernel/src/production_install_flow.c || {
    echo "install flow не ставит installed boot set" >&2
    exit 1
}
if grep -Fq 'production_installer_esp_install(' \
        kernel/src/production_install_flow.c; then
    echo "install flow всё ещё ставит один payload вместо набора" >&2
    exit 1
fi
if grep -Fq 'boot-entry state=VERIFIED' \
        kernel/src/production_install_flow.c; then
    echo "install flow публикует устаревшую запись boot-entry" >&2
    exit 1
fi
grep -Fq 'production_installer_boot_stage_publish' \
    kernel/src/production_install_flow.c || {
    echo "install flow не публикует отказ boot stage" >&2
    exit 1
}

# The medium carries the installed system's boot set, and the sealed media
# manifest keeps its declared component order with the installed configuration
# as its loader configuration.
grep -Fq 'boot/hfoc-installed.conf' \
    scripts/hfoc-install-media-stage.sh || {
    echo "medium staging не несёт installed configuration" >&2
    exit 1
}
grep -Fq 'boot/hfoc-installed.chain' \
    scripts/hfoc-install-media-stage.sh || {
    echo "medium staging не несёт sealed installed chain" >&2
    exit 1
}
# shellcheck disable=SC2016  # the single-quoted text is what the script says
grep -Fq -- '--limine-config "$ISO_ROOT/boot/hfoc-installed.conf"' \
    scripts/hfoc-install-media-stage.sh || {
    echo "sealed media manifest не bind'ит installed configuration" >&2
    exit 1
}
for installed_boot_module in \
    'hfoc-installed.conf' \
    'hfoc-installed.chain' \
    'hfoc-rootfs.arc'; do
    grep -Fq "$installed_boot_module" boot/limine-installer.conf || {
        echo "installer entries не передают installed boot set модулями: $installed_boot_module" >&2
        exit 1
    }
done
for installed_boot_collected in \
    'hfoc-installed.conf' \
    'hfoc-installed.chain'; do
    grep -Fq "$installed_boot_collected" kernel/src/kernel.c || {
        echo "kernel не собирает installed boot set с medium: $installed_boot_collected" >&2
        exit 1
    }
done
installed_boot_reference=$(sed -n \
    's/^    "\(path: .*\)"$/\1/p' \
    kernel/include/production_installer_boot_stage.h)
[[ -n "$installed_boot_reference" ]] || {
    echo "boot stage не публикует kernel reference" >&2
    exit 1
}
grep -Fq "$installed_boot_reference" boot/hfoc-installed.conf || {
    echo "installed configuration не называет staged kernel" >&2
    exit 1
}
grep -Fq 'module_path: boot():/boot/hfoc-rootfs.arc' \
    boot/hfoc-installed.conf || {
    echo "installed configuration не называет root initramfs" >&2
    exit 1
}

# ITEM-662: the boot menu a person sees before the kernel starts names the
# release the medium carries, not the label of an earlier product.
installed_menu_version=$(sed -n 's|^/HFOC OS ||p' boot/hfoc-installed.conf | head -1)
appliance_menu_version=$(sed -n 's|^/HFOC OS ||p' boot/limine.conf | head -1)
[[ $installed_menu_version == "$(<VERSION)" ]] || {
    echo "installed menu называет версию '$installed_menu_version', а не $(<VERSION)" >&2
    exit 1
}
[[ $appliance_menu_version == "$(<VERSION)" ]] || {
    echo "appliance menu называет версию '$appliance_menu_version', а не $(<VERSION)" >&2
    exit 1
}

# The product flow proof asserts the staged set, the mechanism record and the
# boot of the disk the flow installed.
for installed_boot_flow_marker in \
    'boot-entry state=AVAILABLE' \
    'mechanism=REMOVABLE_MEDIA_PATH' \
    'verify=SEALED_MEDIA' \
    'boot_installed_disk'; do
    grep -Fq "$installed_boot_flow_marker" \
        scripts/test-qemu-production-install-flow.sh || {
        echo "install flow proof не проверяет: $installed_boot_flow_marker" >&2
        exit 1
    }
done

# The QEMU proof is wired into the existing installer target and proves the
# records the acceptance stages name.
grep -Fq 'test-qemu-installed-boot.sh' Makefile || {
    echo "Makefile не запускает installed-disk QEMU proof" >&2
    exit 1
}
for installed_boot_marker in \
    'QEMU installed-disk boot test: OK' \
    '\[APPLIANCE-BOOT\] runtime=READY identity=OK config=' \
    '\[PRODUCTION-BOOT\] status=APPLIANCE_PRODUCTION_BOOT_OK' \
    '\[BOOT-CHAIN\] verdict=ATTESTED' \
    'writes=0 result=OK' \
    'reason=PRODUCTION_INSTALLER_ESP_TOO_SMALL'; do
    grep -Fq "$installed_boot_marker" \
        scripts/test-qemu-installed-boot.sh || {
        echo "QEMU proof не проверяет: $installed_boot_marker" >&2
        exit 1
    }
done

# Documented contract and acceptance wiring: the stages that were unreachable
# name the new proof, and the product documents carry the item.
for installed_boot_documented in \
    'ITEM-397' \
    'REMOVABLE_MEDIA_PATH' \
    '67584'; do
    grep -Fq "$installed_boot_documented" \
        docs/hfoc-installed-boot.md || {
        echo "hfoc-installed-boot.md не фиксирует: $installed_boot_documented" >&2
        exit 1
    }
done
for installed_boot_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    grep -Fq 'ITEM-397' "$installed_boot_doc" || {
        echo "$installed_boot_doc не документирует ITEM-397" >&2
        exit 1
    }
done
grep -Fq 'QEMU installed-disk boot test: OK' \
    release/hfoc-hardware-acceptance-matrix.txt || {
    echo "acceptance matrix не называет installed-disk proof" >&2
    exit 1
}
grep -Fq 'installed-disk boot' \
    docs/hfoc-hardware-acceptance.md || {
    echo "hardware acceptance doc не называет installed-disk proof" >&2
    exit 1
}
