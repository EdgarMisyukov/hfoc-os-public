#!/usr/bin/env bash
# ITEM-517: contracted slice of the HFOC OS check suite. Sourced in pinned order
#           by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source, Makefile and document text, so `$(...)`
# must not expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The owner's requirement of 2026-10-10 (queue item "УСТАНОВКА БЕЗ UEFI /
# LEGACY BIOS"): HFOC must install on machines without UEFI - the Toshiba L875 -
# and the installed system must boot without an ESP.  The item has five
# contracted halves and this slice holds each of them:
#
#   * the firmware gate admits a detected legacy-BIOS boot and still refuses an
#     unknown firmware; the supported-hardware policy admits BIOS by name
#     (`allow_bios_legacy`) and never admits `UNKNOWN`;
#   * the declared legacy-BIOS layout: roles 5 and 6, a five-role capacity, the
#     BIOS-only planner entry points and the named preserve refusal;
#   * the generated menu without an ESP: every disk is `NO_ESP`, no EFI
#     chainload entry is produced, and the BIOS install publishes the
#     `firmware=bios-legacy` record;
#   * the measured Limine recipe: the 22,516-byte blob, the 0x1a4 field, the
#     22,004-byte stage2, the two pinned digests, the 64-sector minimum, the
#     active-entry repair the tool performs on a table without one,
#     `limine-bios.sys` on `\boot\limine\`, and the two-installer-only blob
#     transport;
#   * the proofs: the host targets and their registration, the QEMU BIOS
#     install/boot assertions and `boot_installed_bios_disk()`, the BIOS
#     operator walk and the honest refusal, and the documents.
for item517_file in \
    kernel/include/installer_hardware_discovery.h \
    kernel/src/production_install_media.c \
    kernel/include/production_install_media.h \
    kernel/include/installer_supported_hardware.h \
    kernel/src/installer_supported_hardware.c \
    kernel/include/installer_layout.h \
    kernel/src/installer_layout.c \
    kernel/include/installer_layout_contract.h \
    kernel/src/installer_layout_contract.c \
    kernel/src/installer_storage_summary.c \
    kernel/include/production_installer.h \
    kernel/src/production_installer.c \
    kernel/src/production_install_flow.c \
    kernel/src/installer_boot_menu.c \
    kernel/include/installer_boot_menu.h \
    kernel/include/production_installer_bios_boot.h \
    kernel/src/production_installer_bios_boot.c \
    kernel/src/production_installer_bios_blob.c \
    tests/production-install-media-host.c \
    tests/installer-hardening-host.c \
    tests/production-installer-host.c \
    tests/installer-layout-contract-host.c \
    tests/production-installer-bios-boot-host.c \
    tests/installer-boot-menu-host.c \
    tests/test-host-suite-manifest.sh \
    scripts/fetch-limine.sh \
    scripts/test-qemu-production-install-flow.sh \
    scripts/test-qemu-installer-disk-screen.sh \
    docs/ROADMAP.md \
    docs/ARCHITECTURE.md \
    docs/hfoc-installation-guide.md \
    docs/hfoc-production-install-flow.md \
    docs/hfoc-installer-media-hardening.md \
    docs/hfoc-installer-storage.md \
    docs/hfoc-installer-layout.md \
    docs/hfoc-installer-preservation.md \
    docs/hfoc-hardware-acceptance.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item517_file ]] || {
        echo "ITEM-517: отсутствует $item517_file" >&2
        exit 1
    }
done

# ------------------------------------------------ 1. the firmware gate -------
grep -Fq 'INSTALLER_FIRMWARE_BIOS_LEGACY = 2,' \
    kernel/include/installer_hardware_discovery.h || {
    echo "ITEM-517: режим BIOS_LEGACY не объявлен" >&2
    exit 1
}
grep -Fq 'if (firmware_mode == INSTALLER_FIRMWARE_UNKNOWN) {' \
    kernel/src/production_install_media.c || {
    echo "ITEM-517: план установки отказывает не только UNKNOWN" >&2
    exit 1
}
grep -Fq 'PRODUCTION_INSTALL_MEDIA_REFUSAL_FIRMWARE_UNSUPPORTED;' \
    kernel/src/production_install_media.c || {
    echo "ITEM-517: отказ FIRMWARE_UNSUPPORTED потерян" >&2
    exit 1
}
for item517_gate in \
    'report->firmware_mode == INSTALLER_FIRMWARE_BIOS_LEGACY' \
    'installer_layout_contract_plan_clean_bios(' \
    'return PRODUCTION_INSTALLER_BIOS_PRESERVE_UNSUPPORTED;'; do
    grep -Fq -- "$item517_gate" kernel/src/production_installer.c || {
        echo "ITEM-517: контроллер не знает BIOS: $item517_gate" >&2
        exit 1
    }
done
grep -Fq 'PRODUCTION_INSTALLER_BIOS_PRESERVE_UNSUPPORTED = 24,' \
    kernel/include/production_installer.h || {
    echo "ITEM-517: именованный отказ BIOS preserve не объявлен" >&2
    exit 1
}
grep -Fq 'a legacy-BIOS boot selects the declared disk' \
    tests/production-install-media-host.c || {
    echo "ITEM-517: host-проба не требует установки в BIOS-режиме" >&2
    exit 1
}

# ------------------------------------------------- 2. the hardware policy ----
grep -Fq 'bool allow_bios_legacy;' \
    kernel/include/installer_supported_hardware.h || {
    echo "ITEM-517: политика не объявляет allow_bios_legacy" >&2
    exit 1
}
grep -Fq 'policy->allow_bios_legacy = true;' \
    kernel/src/installer_supported_hardware.c || {
    echo "ITEM-517: BIOS не допущен политикой по умолчанию" >&2
    exit 1
}
grep -Fq 'policy->allow_bios_legacy &&' \
    kernel/src/installer_supported_hardware.c || {
    echo "ITEM-517: исключение политики не проверяется" >&2
    exit 1
}
for item517_policy in \
    'legacy firmware is supported when the policy allows it' \
    'a policy that forbids legacy firmware still fails closed' \
    'an unknown firmware still fails closed'; do
    grep -Fq -- "$item517_policy" tests/installer-hardening-host.c || {
        echo "ITEM-517: проба политики не несёт: $item517_policy" >&2
        exit 1
    }
done

# ------------------------------------------- 3. the legacy-BIOS layout -------
grep -Fq '#define INSTALLER_LAYOUT_PARTITION_CAPACITY 5U' \
    kernel/include/installer_layout.h || {
    echo "ITEM-517: ёмкость ролей не выросла до пяти" >&2
    exit 1
}
grep -Fq 'INSTALLER_LAYOUT_ROLE_BIOS_BOOT = 5,' \
    kernel/include/installer_layout.h || {
    echo "ITEM-517: роль BIOS_BOOT не объявлена" >&2
    exit 1
}
grep -Fq 'INSTALLER_LAYOUT_ROLE_BOOT_VOLUME = 6,' \
    kernel/include/installer_layout.h || {
    echo "ITEM-517: роль BOOT_VOLUME не объявлена" >&2
    exit 1
}
grep -Fq '#define INSTALLER_LAYOUT_BIOS_BOOT_MINIMUM_SECTORS 64U' \
    kernel/include/installer_layout.h || {
    echo "ITEM-517: измеренный минимум BIOS-раздела не записан" >&2
    exit 1
}
for item517_contract in \
    '#define INSTALLER_LAYOUT_CONTRACT_BIOS_ROLE_COUNT 5U' \
    'INSTALLER_LAYOUT_CONTRACT_BIOS_BOOT_LENGTH_LBA \' \
    'INSTALLER_LAYOUT_CONTRACT_BIOS_BOOT_VOLUME_LENGTH_LBA UINT64_C(67584)' \
    'INSTALLER_LAYOUT_CONTRACT_BIOS_FOOTPRINT_LBA \' \
    'INSTALLER_LAYOUT_CONTRACT_BIOS_MINIMUM_MEDIA_LBA \'; do
    grep -Fq -- "$item517_contract" \
        kernel/include/installer_layout_contract.h || {
        echo "ITEM-517: контракт BIOS-раскладки не несёт: $item517_contract" >&2
        exit 1
    }
done
grep -Fq 'INSTALLER_LAYOUT_CONTRACT_MODE_CLEAN_WHOLE_DISK_BIOS = 4,' \
    kernel/include/installer_layout_contract.h || {
    echo "ITEM-517: режим BIOS-раскладки не объявлен" >&2
    exit 1
}
for item517_planner in \
    'installer_layout_plan_whole_disk_bios(' \
    'installer_layout_assign_whole_disk_bios('; do
    grep -Fq -- "$item517_planner" kernel/src/installer_layout.c || {
        echo "ITEM-517: планировщик BIOS не написан: $item517_planner" >&2
        exit 1
    }
done
grep -Fq 'installer_layout_contract_plan_clean_bios(' \
    kernel/src/installer_layout_contract.c || {
    echo "ITEM-517: контрактная точка входа BIOS не написана" >&2
    exit 1
}
grep -Fq '"clean-whole-disk-bios"' \
    kernel/src/installer_layout_contract.c || {
    echo "ITEM-517: режим BIOS не называется в записи" >&2
    exit 1
}
if ! grep -Fq '"bios-boot"' kernel/src/installer_layout_contract.c ||
    ! grep -Fq '"boot-volume"' kernel/src/installer_layout_contract.c; then
    echo "ITEM-517: имена BIOS-ролей не записаны" >&2
    exit 1
fi
grep -Fq 'installer_layout_contract_plan_clean_bios(' \
    kernel/src/installer_storage_summary.c || {
    echo "ITEM-517: сводка экрана не строит BIOS-план" >&2
    exit 1
}
for item517_layout in \
    'the BIOS plan is a five-role whole-disk layout' \
    'the BIOS plan has no EFI System Partition role' \
    'the BIOS-boot role carries the Hah!IdontNeedEFI GPT type' \
    'the BIOS minimum media is the pinned exact layout floor' \
    'the BIOS record names the BIOS layout and no ESP' \
    'a disk that fits the UEFI layout is still below the BIOS floor'; do
    grep -Fq -- "$item517_layout" \
        tests/installer-layout-contract-host.c || {
        echo "ITEM-517: проба раскладки не несёт: $item517_layout" >&2
        exit 1
    }
done
grep -Fq 'the BIOS clean plan carries the BIOS boot role and the volume' \
    tests/production-installer-bios-boot-host.c || {
    echo "ITEM-517: проба загрузки не проверяет BIOS-план" >&2
    exit 1
}

# ------------------------------------------------- 4. the menu without ESP ---
grep -Fq 'INSTALLER_BOOT_MENU_SCAN_NO_ESP' \
    kernel/src/installer_boot_menu.c || {
    echo "ITEM-517: меню не различает NO_ESP" >&2
    exit 1
}
grep -Fq '"bios-legacy"' kernel/src/installer_boot_menu.c || {
    echo "ITEM-517: запись меню не называет bios-legacy" >&2
    exit 1
}
grep -Fq 'report->firmware_mode == INSTALLER_FIRMWARE_BIOS_LEGACY' \
    kernel/src/installer_boot_menu.c || {
    echo "ITEM-517: сборщик меню не различает BIOS" >&2
    exit 1
}
for item517_menu in \
    'the UEFI configuration is byte for byte the pre-item vector' \
    'the UEFI record is byte for byte the pre-item vector' \
    'firmware=bios-legacy' \
    'reason=NO_ESP' \
    'the BIOS configuration offers no EFI chainload entry' \
    'the BIOS appliance entry names the chain configuration module'; do
    grep -Fq -- "$item517_menu" tests/installer-boot-menu-host.c || {
        echo "ITEM-517: проба меню не несёт: $item517_menu" >&2
        exit 1
    }
done
grep -Fq 'production_install_flow_publish_bios_menu' \
    kernel/src/production_install_flow.c || {
    echo "ITEM-517: BIOS-установка не публикует запись меню" >&2
    exit 1
}
grep -Fq 'PRODUCTION_INSTALLER_BIOS_CHAIN_CONFIG_REFERENCE;' \
    kernel/src/production_install_flow.c || {
    echo "ITEM-517: меню не называет модуль запечатанной конфигурации" >&2
    exit 1
}

# ------------------------------------------ 5. the measured Limine recipe ----
for item517_recipe in \
    '#define PRODUCTION_INSTALLER_BIOS_STAGE1_SIZE 512U' \
    '#define PRODUCTION_INSTALLER_BIOS_STAGE2_SIZE 22004U' \
    '#define PRODUCTION_INSTALLER_BIOS_STAGE2_OFFSET_FIELD 0x1A4U' \
    '#define PRODUCTION_INSTALLER_BIOS_LIMINE_SYS_SIZE 290964U' \
    '#define PRODUCTION_INSTALLER_BIOS_STAGE_SECTOR_WRITES 44U' \
    '#define PRODUCTION_INSTALLER_BIOS_MBR_FIRST_ENTRY_OFFSET 446U' \
    '#define PRODUCTION_INSTALLER_BIOS_MBR_ACTIVE_FLAG 0x80U' \
    '#define PRODUCTION_INSTALLER_BIOS_ECX_PADDING 0xADU' \
    '#define PRODUCTION_INSTALLER_BIOS_ECX_TRAMPOLINES_SIZE 32U' \
    '#define PRODUCTION_INSTALLER_BIOS_SET_COUNT 5U' \
    '#define PRODUCTION_INSTALLER_BIOS_CONFIG_PATH "\\boot\\limine\\limine.conf"' \
    '#define PRODUCTION_INSTALLER_BIOS_CHAIN_CONFIG_PATH \' \
    '"\\boot\\hfoc\\limine.conf"' \
    '"\\boot\\limine\\limine-bios.sys"' \
    '"boot():/boot/hfoc/limine.conf"'; do
    grep -Fq -- "$item517_recipe" \
        kernel/include/production_installer_bios_boot.h || {
        echo "ITEM-517: контракт BIOS-загрузки не несёт: $item517_recipe" >&2
        exit 1
    }
done
for item517_digest in \
    '0x83U, 0xA2U, 0x05U, 0xABU, 0x84U, 0xC1U, 0x88U, 0x8AU,' \
    '0x7DU, 0x3AU, 0xD7U, 0x41U, 0x8CU, 0x1AU, 0x95U, 0x4BU,'; do
    grep -Fq -- "$item517_digest" \
        kernel/src/production_installer_bios_boot.c || {
        echo "ITEM-517: прибитый дайджест потерян: $item517_digest" >&2
        exit 1
    }
done
for item517_stage in \
    'PRODUCTION_INSTALLER_BIOS_STAGE2_OFFSET_FIELD,' \
    'memcpy(lba0 + 218U, original + 218U, 6U);' \
    'memcpy(lba0 + 440U, original + 440U, 70U);' \
    'if (!production_bios_table_has_active(lba0)) {' \
    'PRODUCTION_INSTALLER_BIOS_MBR_ACTIVE_FLAG;' \
    'production_bios_preserve_ecx(lba0)' \
    'production_bios_ecx_trampolines' \
    'PRODUCTION_INSTALLER_BOOT_MECHANISM_BIOS_MBR;' \
    'PRODUCTION_INSTALLER_BIOS_CHAIN_CONFIG_PATH'; do
    grep -Fq -- "$item517_stage" \
        kernel/src/production_installer_bios_boot.c || {
        echo "ITEM-517: писатель BIOS-стадии не несёт: $item517_stage" >&2
        exit 1
    }
done
for item517_host_boot in \
    'LBA0 is the measured stage1 recipe byte for byte' \
    'the saved LBA0 ranges are restored' \
    'the protective entry is marked active for the firmware' \
    'ECX is kept across both BIOS calls of the stage' \
    'the three call sites are redirected and the carry test survives' \
    'the stage writer changes exactly 44 sectors' \
    'the partial tail sector keeps the bytes stage2 does not cover' \
    'the primary and backup GPT CRCs still validate' \
    'limine-bios.sys is the measured Limine 12.3.2 loader' \
    'the sealed chain configuration the menu names is staged' \
    'mechanism=BIOS_MBR' \
    'path=\\boot\\limine\\limine.conf' \
    'config=\\boot\\hfoc\\limine.conf' \
    'loader=\\boot\\limine\\limine-bios.sys'; do
    grep -Fq -- "$item517_host_boot" \
        tests/production-installer-bios-boot-host.c || {
        echo "ITEM-517: host-проба загрузки не несёт: $item517_host_boot" >&2
        exit 1
    }
done

# ------------------------------ 6. the installer-only blob transport --------
grep -Fq 'PRODUCTION_INSTALLER_LIMINE_BIOS_SYS_OBJ := \' Makefile || {
    echo "ITEM-517: объект limine-bios.sys не объявлен" >&2
    exit 1
}
grep -Fq '$(LD) -r -b binary deps/limine/limine-bios.sys -o $@' Makefile || {
    echo "ITEM-517: правило -b binary потеряно" >&2
    exit 1
}
grep -Fq '$(PRODUCTION_INSTALLER_LIMINE_BIOS_SYS_OBJ): deps/limine/limine-bios.sys' \
    Makefile || {
    echo "ITEM-517: правило сборки блоба потеряло зависимость" >&2
    exit 1
}
grep -Fq 'OBJECTS := $(C_OBJECTS) $(ASM_OBJECTS)' Makefile || {
    echo "ITEM-517: ядро продукта изменило список объектов" >&2
    exit 1
}
if grep -Eq '^(OBJECTS|C_OBJECTS|ASM_OBJECTS) .*LIMINE_BIOS_SYS' Makefile; then
    echo "ITEM-517: блоб попал в ядро продукта" >&2
    exit 1
fi
grep -Fq -- '-DHFOC_PRODUCTION_INSTALL_MEDIA -Ideps/limine' Makefile || {
    echo "ITEM-517: установщик не компилирует заголовок блоба" >&2
    exit 1
}
grep -Fq 'limine-bios-hdd.h' scripts/fetch-limine.sh || {
    echo "ITEM-517: fetch-limine не требует limine-bios-hdd.h" >&2
    exit 1
}
for item517_blob in \
    '#ifdef HFOC_PRODUCTION_INSTALL_MEDIA' \
    '#include "limine-bios-hdd.h"' \
    '_binary_deps_limine_limine_bios_sys_start'; do
    grep -Fq -- "$item517_blob" \
        kernel/src/production_installer_bios_blob.c || {
        echo "ITEM-517: транспорт блобов не несёт: $item517_blob" >&2
        exit 1
    }
done

# ------------------------------------------------ 7. the host proofs ---------
for item517_target in \
    '.PHONY: test-production-installer-bios-boot-host' \
    '.PHONY: test-installer-boot-menu-host' \
    '$(MAKE) test-production-installer-bios-boot-host' \
    '$(MAKE) test-installer-boot-menu-host'; do
    grep -Fq -- "$item517_target" Makefile || {
        echo "ITEM-517: цель пробы не подключена: $item517_target" >&2
        exit 1
    }
done
grep -Fq 'test-production-installer-bios-boot-host' \
    tests/test-host-suite-manifest.sh || {
    echo "ITEM-517: проба BIOS-загрузки не в манифесте набора" >&2
    exit 1
}

# ------------------------------------------------ 8. the QEMU proofs ---------
for item517_qemu in \
    'boot_installed_bios_disk' \
    'assert_bios_boot_volume' \
    'run_boot "bios" bios' \
    'mode=clean-whole-disk-bios' \
    'mechanism=BIOS_MBR' \
    'firmware=bios-legacy' \
    'Hah!IdontNeedEFI' \
    '0x1A4' \
    'limine_stage_blob' \
    '--partition-index 1' \
    'installed LBA0: partition 1 active' \
    '[BOOT-CHAIN] verdict=ATTESTED' \
    'declare_marker_timeout'; do
    grep -Fq -- "$item517_qemu" \
        scripts/test-qemu-production-install-flow.sh || {
        echo "ITEM-517: QEMU-проба установки не несёт: $item517_qemu" >&2
        exit 1
    }
done
for item517_screen in \
    'CASE_FIRMWARE' \
    'bios-walk' \
    'run_refusal_case refusal NO_DISK' \
    'BIOS boot partition' \
    'FAT boot volume'; do
    grep -Fq -- "$item517_screen" \
        scripts/test-qemu-installer-disk-screen.sh || {
        echo "ITEM-517: QEMU-проба экрана не несёт: $item517_screen" >&2
        exit 1
    }
done

# ------------------------------------------------ 9. the documents -----------
for item517_doc in \
    docs/ROADMAP.md \
    docs/ARCHITECTURE.md \
    docs/hfoc-installation-guide.md \
    docs/hfoc-production-install-flow.md \
    docs/hfoc-installer-media-hardening.md \
    docs/hfoc-installer-storage.md \
    docs/hfoc-installer-layout.md \
    docs/hfoc-installer-preservation.md \
    docs/hfoc-hardware-acceptance.md; do
    grep -Fq 'ITEM-517' "$item517_doc" || {
        echo "ITEM-517: $item517_doc не называет пункт" >&2
        exit 1
    }
done
# ITEM-518 advanced the queue's one current item; ITEM-517's own section is the
# historical one now (the heading shape itself is held by
# `scripts/check.d/52-queue-current-item.sh`).
grep -A 3 '^## Item ITEM-517$' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md |
    grep -Fq 'ITEM-517' || {
    echo "ITEM-517: исторический раздел ITEM-517 потерян" >&2
    exit 1
}
# And the past section is still the historical one slice 196 reads.
grep -A 3 '^## Item ITEM-516$' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md |
    grep -Fq 'ITEM-516' || {
    echo "ITEM-517: исторический раздел ITEM-516 потерян" >&2
    exit 1
}
# Physical acceptance is named as the owner's step, never claimed as CI's.
if ! grep -Fq 'Toshiba L875' docs/hfoc-installation-guide.md ||
    ! grep -Fq 'Toshiba L875' docs/ROADMAP.md; then
    echo "ITEM-517: документы не называют физическую приёмку владельца" >&2
    exit 1
fi
