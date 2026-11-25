#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Appliance v1 acceptance gate contracts.
grep -Fq 'test-release-appliance-v1-acceptance-host' Makefile || {
    echo "Makefile не содержит test-release-appliance-v1-acceptance-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-release-appliance-v1-acceptance-host' Makefile || {
    echo "test-host-suite не вызывает test-release-appliance-v1-acceptance-host" >&2
    exit 1
}
grep -Fq 'test-release-appliance-v1-acceptance:' Makefile || {
    echo "Makefile не содержит test-release-appliance-v1-acceptance target" >&2
    exit 1
}
grep -Eq '\$\(MAKE\) test-release-appliance-v1-acceptance[[:space:]]*$' Makefile || {
    echo "test не вызывает реальный test-release-appliance-v1-acceptance" >&2
    exit 1
}
grep -Fq 'scripts/test-release-appliance-v1-acceptance.sh' Makefile || {
    echo "Makefile не вызывает test-release-appliance-v1-acceptance.sh" >&2
    exit 1
}
bash -n scripts/verify-release-appliance-v1-acceptance.sh || {
    echo "Синтаксис verify-release-appliance-v1-acceptance.sh некорректен" >&2
    exit 1
}
bash -n scripts/test-release-appliance-v1-acceptance.sh || {
    echo "Синтаксис test-release-appliance-v1-acceptance.sh некорректен" >&2
    exit 1
}
bash -n tests/test-release-appliance-v1-acceptance-host.sh || {
    echo "Синтаксис test-release-appliance-v1-acceptance-host.sh некорректен" >&2
    exit 1
}
bash tests/test-release-appliance-v1-acceptance-host.sh
bash scripts/verify-release-appliance-v1-acceptance.sh \
    release/release-appliance-v1-acceptance.txt
for acceptance_target in \
    test-release-profile-host \
    test-build-input-manifest-host \
    test-reproducible-image-host \
    test-release-artifact-host \
    test-release-upgrade-matrix \
    test-release-backup-restore-matrix \
    test-release-security-baseline \
    test-release-operational-baseline \
    test-release-disaster-recovery-rehearsal; do
    grep -Fq "${acceptance_target}:" Makefile || {
        echo "Makefile не содержит canonical acceptance target ${acceptance_target}" >&2
        exit 1
    }
    grep -Eq "check_[0-9]+_target=${acceptance_target}" \
        release/release-appliance-v1-acceptance.txt || {
        echo "acceptance matrix не содержит ${acceptance_target}" >&2
        exit 1
    }
done
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Appliance v1 acceptance gate host test: OK'; then
    echo "Production kernel содержит appliance v1 acceptance host marker" >&2
    exit 1
fi
for release_acceptance_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'appliance v1 acceptance gate|appliance-v1-acceptance-gate' \
        "$release_acceptance_doc"; then
        echo "$release_acceptance_doc не документирует appliance v1 acceptance gate" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **200. Appliance v1 acceptance gate' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 200 завершённым" >&2
    exit 1
}

grep -Fq 'test-fetch-limine-host:' Makefile || {
    echo "Makefile не содержит test-fetch-limine-host" >&2
    exit 1
}
grep -Fq "\$(MAKE) test-fetch-limine-host" Makefile || {
    echo "test-host-suite не вызывает test-fetch-limine-host" >&2
    exit 1
}
grep -Fq -- '--retry 4 --retry-all-errors' scripts/fetch-limine.sh || {
    echo "fetch-limine не содержит bounded retry contract" >&2
    exit 1
}
grep -Fq 'HFOC_LIMINE_CACHE_DIR' scripts/fetch-limine.sh || {
    echo "fetch-limine не содержит persistent cache contract" >&2
    exit 1
}
grep -Fq '.hfoc-prepared' scripts/fetch-limine.sh || {
    echo "fetch-limine не содержит preparation stamp contract" >&2
    exit 1
}

bash scripts/verify-licensing-policy.sh
bash scripts/verify-third-party-license-docs.sh

grep -Fq 'def exchange(sock, phase,' scripts/test-qemu-network-resource.sh || {
    echo "network resource QEMU script lacks phase-labelled exchange" >&2
    exit 1
}
grep -Fq 'post-restart echo' scripts/test-qemu-network-resource.sh || {
    echo "network resource QEMU script lacks post-restart phase label" >&2
    exit 1
}
grep -Fq 'initial pool echo slot' scripts/test-qemu-network-resource.sh || {
    echo "network resource QEMU script lacks initial-pool phase label" >&2
    exit 1
}

# Installer architecture audit contracts (ITEM-338).
grep -Fq 'test-item-338-installer-architecture-audit-host' Makefile || {
    echo "Makefile не содержит test-item-338-installer-architecture-audit-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-item-338-installer-architecture-audit-host' Makefile || {
    echo "test-host-suite не вызывает test-item-338-installer-architecture-audit-host" >&2
    exit 1
}
test -f docs/hfoc-installer-architecture-audit.md || {
    echo "Отсутствует docs/hfoc-installer-architecture-audit.md" >&2
    exit 1
}
test -f docs/superpowers/specs/2026-09-06-item-338-installer-architecture-audit-design.md || {
    echo "Отсутствует ITEM-338 design spec" >&2
    exit 1
}
test -f docs/superpowers/plans/2026-09-06-item-338-installer-architecture-audit-implementation.md || {
    echo "Отсутствует ITEM-338 implementation plan" >&2
    exit 1
}
test -f tests/test-item-338-installer-architecture-audit.sh || {
    echo "Отсутствует tests/test-item-338-installer-architecture-audit.sh" >&2
    exit 1
}
bash -n tests/test-item-338-installer-architecture-audit.sh || {
    echo "Синтаксис tests/test-item-338-installer-architecture-audit.sh некорректен" >&2
    exit 1
}
bash tests/test-item-338-installer-architecture-audit.sh
grep -Fq -- '- [x] **ITEM-338**' \
    docs/superpowers/plans/2026-09-06-hfoc-appliance-installer-and-management-ui.md || {
    echo "installer plan не помечает ITEM-338 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **ITEM-339**' \
    docs/superpowers/plans/2026-09-06-hfoc-appliance-installer-and-management-ui.md || {
    echo "installer plan не помечает ITEM-339 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **ITEM-340**' \
    docs/superpowers/plans/2026-09-06-hfoc-appliance-installer-and-management-ui.md || {
    echo "installer plan не помечает ITEM-340 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **ITEM-341**' \
    docs/superpowers/plans/2026-09-06-hfoc-appliance-installer-and-management-ui.md || {
    echo "installer plan не помечает ITEM-341 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **ITEM-342**' \
    docs/superpowers/plans/2026-09-06-hfoc-appliance-installer-and-management-ui.md || {
    echo "installer plan не помечает ITEM-342 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **ITEM-343**' \
    docs/superpowers/plans/2026-09-06-hfoc-appliance-installer-and-management-ui.md || {
    echo "installer plan не помечает ITEM-343 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **ITEM-344**' \
    docs/superpowers/plans/2026-09-06-hfoc-appliance-installer-and-management-ui.md || {
    echo "installer plan не помечает ITEM-344 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **ITEM-345**' \
    docs/superpowers/plans/2026-09-06-hfoc-appliance-installer-and-management-ui.md || {
    echo "installer plan не помечает ITEM-345 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **ITEM-346**' \
    docs/superpowers/plans/2026-09-06-hfoc-appliance-installer-and-management-ui.md || {
    echo "installer plan не помечает ITEM-346 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **ITEM-347**' \
    docs/superpowers/plans/2026-09-06-hfoc-appliance-installer-and-management-ui.md || {
    echo "installer plan не помечает ITEM-347 завершённым" >&2
    exit 1
}

# Hardware and existing OS discovery contracts (ITEM-339).
grep -Fq 'test-installer-hardware-discovery-host' Makefile || {
    echo "Makefile не содержит test-installer-hardware-discovery-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-installer-hardware-discovery-host' Makefile || {
    echo "test-host-suite не вызывает test-installer-hardware-discovery-host" >&2
    exit 1
}
test -f kernel/include/installer_hardware_discovery.h || {
    echo "Отсутствует kernel/include/installer_hardware_discovery.h" >&2
    exit 1
}
test -f kernel/src/installer_hardware_discovery.c || {
    echo "Отсутствует kernel/src/installer_hardware_discovery.c" >&2
    exit 1
}
test -f tests/installer-hardware-discovery-host.c || {
    echo "Отсутствует tests/installer-hardware-discovery-host.c" >&2
    exit 1
}
test -f docs/hfoc-hardware-os-discovery.md || {
    echo "Отсутствует docs/hfoc-hardware-os-discovery.md" >&2
    exit 1
}
test -f docs/superpowers/specs/2026-09-06-item-339-hardware-os-discovery-design.md || {
    echo "Отсутствует ITEM-339 design spec" >&2
    exit 1
}
test -f docs/superpowers/plans/2026-09-06-item-339-hardware-os-discovery-implementation.md || {
    echo "Отсутствует ITEM-339 implementation plan" >&2
    exit 1
}
grep -Fq 'never writes a device' docs/hfoc-hardware-os-discovery.md || {
    echo "ITEM-339 authoritative doc не заявляет read-only boundary" >&2
    exit 1
}
for installer_metadata_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    grep -Eiq 'hardware and existing OS discovery|hardware-os-discovery|hardware discovery' \
        "$installer_metadata_doc" || {
        echo "$installer_metadata_doc не документирует hardware and OS discovery" >&2
        exit 1
    }
done

# Clean installer layout contracts (ITEM-340).
grep -Fq 'test-installer-layout-host' Makefile || {
    echo "Makefile не содержит test-installer-layout-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-installer-layout-host' Makefile || {
    echo "test-host-suite не вызывает test-installer-layout-host" >&2
    exit 1
}
test -f kernel/include/installer_layout.h || {
    echo "Отсутствует kernel/include/installer_layout.h" >&2
    exit 1
}
test -f kernel/src/installer_layout.c || {
    echo "Отсутствует kernel/src/installer_layout.c" >&2
    exit 1
}
test -f tests/installer-layout-host.c || {
    echo "Отсутствует tests/installer-layout-host.c" >&2
    exit 1
}
test -f docs/hfoc-installer-layout.md || {
    echo "Отсутствует docs/hfoc-installer-layout.md" >&2
    exit 1
}
test -f docs/superpowers/specs/2026-09-06-item-340-installer-layout-design.md || {
    echo "Отсутствует ITEM-340 design spec" >&2
    exit 1
}
test -f docs/superpowers/plans/2026-09-06-item-340-installer-layout-implementation.md || {
    echo "Отсутствует ITEM-340 implementation plan" >&2
    exit 1
}
grep -Fq 'never writes before' docs/hfoc-installer-layout.md || {
    echo "ITEM-340 authoritative doc не заявляет confirmation boundary" >&2
    exit 1
}
for installer_layout_metadata_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    grep -Eiq 'clean installer layout|installer layout|installer-layout' \
        "$installer_layout_metadata_doc" || {
        echo "$installer_layout_metadata_doc не документирует installer layout" >&2
        exit 1
    }
done

# Dual-boot planning and menu contracts (ITEM-341).
grep -Fq 'test-installer-dual-boot-host' Makefile || {
    echo "Makefile не содержит test-installer-dual-boot-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-installer-dual-boot-host' Makefile || {
    echo "test-host-suite не вызывает test-installer-dual-boot-host" >&2
    exit 1
}
test -f kernel/include/installer_dual_boot.h || {
    echo "Отсутствует kernel/include/installer_dual_boot.h" >&2
    exit 1
}
test -f kernel/src/installer_dual_boot.c || {
    echo "Отсутствует kernel/src/installer_dual_boot.c" >&2
    exit 1
}
test -f tests/installer-dual-boot-host.c || {
    echo "Отсутствует tests/installer-dual-boot-host.c" >&2
    exit 1
}
test -f docs/hfoc-dual-boot.md || {
    echo "Отсутствует docs/hfoc-dual-boot.md" >&2
    exit 1
}
test -f docs/superpowers/specs/2026-09-06-item-341-dual-boot-design.md || {
    echo "Отсутствует ITEM-341 design spec" >&2
    exit 1
}
test -f docs/superpowers/plans/2026-09-06-item-341-dual-boot-implementation.md || {
    echo "Отсутствует ITEM-341 implementation plan" >&2
    exit 1
}
grep -Fq 'never invokes a block-device write callback' \
    docs/hfoc-dual-boot.md || {
    echo "ITEM-341 authoritative doc не заявляет read-only boundary" >&2
    exit 1
}
for installer_dual_boot_metadata_doc in \
    README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    grep -Eiq 'dual.?boot|dual boot' \
        "$installer_dual_boot_metadata_doc" || {
        echo "$installer_dual_boot_metadata_doc не документирует dual boot" >&2
        exit 1
    }
done

# Bootloader recovery contracts (ITEM-342).
grep -Fq 'test-installer-boot-recovery-host' Makefile || {
    echo "Makefile не содержит test-installer-boot-recovery-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-installer-boot-recovery-host' Makefile || {
    echo "test-host-suite не вызывает test-installer-boot-recovery-host" >&2
    exit 1
}
test -f kernel/include/installer_boot_recovery.h || {
    echo "Отсутствует kernel/include/installer_boot_recovery.h" >&2
    exit 1
}
test -f kernel/src/installer_boot_recovery.c || {
    echo "Отсутствует kernel/src/installer_boot_recovery.c" >&2
    exit 1
}
test -f tests/installer-boot-recovery-host.c || {
    echo "Отсутствует tests/installer-boot-recovery-host.c" >&2
    exit 1
}
test -f docs/hfoc-bootloader-recovery.md || {
    echo "Отсутствует docs/hfoc-bootloader-recovery.md" >&2
    exit 1
}
test -f docs/superpowers/specs/2026-09-06-item-342-bootloader-recovery-design.md || {
    echo "Отсутствует ITEM-342 design spec" >&2
    exit 1
}
test -f docs/superpowers/plans/2026-09-06-item-342-bootloader-recovery-implementation.md || {
    echo "Отсутствует ITEM-342 implementation plan" >&2
    exit 1
}
grep -Fq 'No API writes EFI files, UEFI variables, block devices' \
    docs/hfoc-bootloader-recovery.md || {
    echo "ITEM-342 authoritative doc не заявляет no-mutation boundary" >&2
    exit 1
}
for installer_boot_recovery_metadata_doc in \
    README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    grep -Eiq 'bootloader recovery|boot recovery' \
        "$installer_boot_recovery_metadata_doc" || {
        echo "$installer_boot_recovery_metadata_doc не документирует boot recovery" >&2
        exit 1
    }
done

# First boot wizard contracts (ITEM-343).
grep -Fq 'test-installer-first-boot-wizard-host' Makefile || {
    echo "Makefile не содержит test-installer-first-boot-wizard-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-installer-first-boot-wizard-host' Makefile || {
    echo "test-host-suite не вызывает test-installer-first-boot-wizard-host" >&2
    exit 1
}
test -f kernel/include/installer_first_boot_wizard.h || {
    echo "Отсутствует kernel/include/installer_first_boot_wizard.h" >&2
    exit 1
}
test -f kernel/src/installer_first_boot_wizard.c || {
    echo "Отсутствует kernel/src/installer_first_boot_wizard.c" >&2
    exit 1
}
test -f tests/installer-first-boot-wizard-host.c || {
    echo "Отсутствует tests/installer-first-boot-wizard-host.c" >&2
    exit 1
}
test -f docs/hfoc-first-boot-wizard.md || {
    echo "Отсутствует docs/hfoc-first-boot-wizard.md" >&2
    exit 1
}
test -f docs/superpowers/specs/2026-09-06-item-343-first-boot-wizard-design.md || {
    echo "Отсутствует ITEM-343 design spec" >&2
    exit 1
}
test -f docs/superpowers/plans/2026-09-06-item-343-first-boot-wizard-implementation.md || {
    echo "Отсутствует ITEM-343 implementation plan" >&2
    exit 1
}
grep -Fq 'No plaintext password, raw credential, bearer token, or secret material is an' \
    docs/hfoc-first-boot-wizard.md || {
    echo "ITEM-343 authoritative doc не заявляет secret boundary" >&2
    exit 1
}
for installer_first_boot_metadata_doc in \
    README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    grep -Eiq 'first.?boot wizard|first-boot wizard|first boot wizard' \
        "$installer_first_boot_metadata_doc" || {
        echo "$installer_first_boot_metadata_doc не документирует first boot wizard" >&2
        exit 1
    }
done

# Local appliance console contracts (ITEM-344).
grep -Fq 'test-appliance-boot-console-host' Makefile || {
    echo "Makefile не содержит test-appliance-boot-console-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-appliance-boot-console-host' Makefile || {
    echo "test-host-suite не вызывает test-appliance-boot-console-host" >&2
    exit 1
}
grep -Fq 'test-appliance-boot-console-qemu' Makefile || {
    echo "Makefile не содержит test-appliance-boot-console-qemu" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-appliance-boot-console-qemu' Makefile || {
    echo "test-serial не вызывает test-appliance-boot-console-qemu" >&2
    exit 1
}
test -f kernel/include/appliance_boot_console.h || {
    echo "Отсутствует kernel/include/appliance_boot_console.h" >&2
    exit 1
}
test -f kernel/src/appliance_boot_console.c || {
    echo "Отсутствует kernel/src/appliance_boot_console.c" >&2
    exit 1
}
test -f tests/appliance-boot-console-host.c || {
    echo "Отсутствует tests/appliance-boot-console-host.c" >&2
    exit 1
}
test -f docs/hfoc-appliance-boot-console.md || {
    echo "Отсутствует docs/hfoc-appliance-boot-console.md" >&2
    exit 1
}
test -f docs/superpowers/specs/2026-09-06-item-344-local-console-design.md || {
    echo "Отсутствует ITEM-344 design spec" >&2
    exit 1
}
test -f docs/superpowers/plans/2026-09-06-item-344-local-console-implementation.md || {
    echo "Отсутствует ITEM-344 implementation plan" >&2
    exit 1
}
grep -Fq 'No desktop environment or full-screen graphical UI' \
    docs/hfoc-appliance-boot-console.md || {
    echo "ITEM-344 authoritative doc не заявляет no-desktop boundary" >&2
    exit 1
}
grep -Fq 'The local menu reboots and powers the machine off (ITEM-418/419)' \
    docs/hfoc-appliance-boot-console.md || {
    echo "ITEM-344 authoritative doc не называет границу действий меню" >&2
    exit 1
}
grep -Fq 'No pointer, no touch input' \
    docs/hfoc-appliance-boot-console.md || {
    echo "ITEM-344 authoritative doc не заявляет no-pointer boundary" >&2
    exit 1
}
grep -Fq 'appliance-boot' kernel/src/console.c || {
    echo "console.c не содержит команду appliance-boot" >&2
    exit 1
}
if grep -nE '\b(malloc|calloc|free|kmalloc|kfree|pmm_alloc|pmm_free|serial_printf|printf|fprintf|snprintf|sprintf|serial_write|console_write|event_ring_|audit_|block_device_read|block_device_write)\b' \
        kernel/src/appliance_boot_console.c; then
    echo "appliance_boot_console.c использует запрещённое выделение, диагностику, аудит, I/O или блокирование" >&2
    exit 1
fi
grep -Fq 'key_map.get(character, character)' \
    scripts/test-qemu-appliance-boot-console.sh || {
    echo "ITEM-344 QEMU script не отправляет appliance-boot" >&2
    exit 1
}
grep -Fq 'QEMU appliance boot console test: OK' \
    scripts/test-qemu-appliance-boot-console.sh || {
    echo "ITEM-344 QEMU script не содержит success marker" >&2
    exit 1
}
for installer_local_console_metadata_doc in \
    README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    grep -Eiq 'local appliance console|appliance boot console|appliance-boot' \
        "$installer_local_console_metadata_doc" || {
        echo "$installer_local_console_metadata_doc не документирует appliance boot console" >&2
        exit 1
    }
done

# Management web UI contracts (ITEM-345).
grep -Fq 'test-appliance-management-web-ui-host' Makefile || {
    echo "Makefile не содержит test-appliance-management-web-ui-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-appliance-management-web-ui-host' Makefile || {
    echo "test-host-suite не вызывает test-appliance-management-web-ui-host" >&2
    exit 1
}
test -f kernel/include/appliance_management_web_ui.h || {
    echo "Отсутствует kernel/include/appliance_management_web_ui.h" >&2
    exit 1
}
test -f kernel/src/appliance_management_web_ui.c || {
    echo "Отсутствует kernel/src/appliance_management_web_ui.c" >&2
    exit 1
}
test -f tests/appliance-management-web-ui-host.c || {
    echo "Отсутствует tests/appliance-management-web-ui-host.c" >&2
    exit 1
}
test -f docs/hfoc-management-web-ui.md || {
    echo "Отсутствует docs/hfoc-management-web-ui.md" >&2
    exit 1
}
test -f docs/superpowers/specs/2026-09-06-item-345-management-web-ui-design.md || {
    echo "Отсутствует ITEM-345 design spec" >&2
    exit 1
}
test -f docs/superpowers/plans/2026-09-06-item-345-management-web-ui-implementation.md || {
    echo "Отсутствует ITEM-345 implementation plan" >&2
    exit 1
}
# ITEM-504: the panel gained its first mutating section, so the section model
# is ten sections and not nine.  The nine read-side sections keep their
# indices; Routing is appended and the ITEM-382 licenses page stays outside
# the model.  ITEM-506 increment 6 appends Account the same way, so the model
# is eleven sections and every earlier index is untouched.
grep -Fq 'APPLIANCE_MANAGEMENT_WEB_UI_SECTION_COUNT 13U' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-506 section model не содержит двенадцать разделов" >&2
    exit 1
}
grep -Fq 'appliance_management_web_ui_shell_body' \
    kernel/src/https_handler.c || {
    echo "HTTPS handler не публикует management web UI shell" >&2
    exit 1
}
grep -Fq 'No browser JavaScript' README.md || {
    echo "README не заявляет ITEM-345 no-browser boundary" >&2
    exit 1
}
grep -Fq 'remote credential form' README.md || {
    echo "README не заявляет ITEM-345 no-credential boundary" >&2
    exit 1
}
grep -Fq 'Dashboard Network Services Storage System Security Diagnostics Updates Recovery Routing Account' \
    docs/hfoc-management-web-ui.md kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-345/ITEM-506 authoritative doc/module не содержит все разделы" >&2
    exit 1
}
if grep -nE '\b(malloc|calloc|free|kmalloc|kfree|pmm_alloc|pmm_free|serial_printf|printf|fprintf|snprintf|sprintf|serial_write|console_write|event_ring_|audit_|block_device_read|block_device_write)\b' \
        kernel/src/appliance_management_web_ui.c; then
    echo "appliance_management_web_ui.c использует запрещённое выделение, диагностику, аудит, I/O или блокирование" >&2
    exit 1
fi
for installer_web_ui_metadata_doc in \
    README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    grep -Eiq 'management web UI|web management' \
        "$installer_web_ui_metadata_doc" || {
        echo "$installer_web_ui_metadata_doc не документирует management web UI" >&2
        exit 1
    }
done

# ITEM-419: the local menu is real - it selects, it runs, and the actions that
# take the machine down are sequenced.
#
# The appliance rendered `[4] Reboot` / `[5] Shutdown` on its own screen since
# ITEM-344 and nothing could select one, so the menu the operator at a machine
# with no serial port and no network was offered did not respond.  The model is
# pure and proven on the host; the console owns the screen, the keyboard and the
# order of a power transition.
for local_menu_file in \
    kernel/include/local_menu.h \
    kernel/src/local_menu.c \
    tests/local-menu-host.c \
    kernel/src/console.c; do
    test -f "$local_menu_file" || {
        echo "Отсутствует ITEM-419 file: $local_menu_file" >&2
        exit 1
    }
done
grep -Fq 'test-local-menu-host:' Makefile || {
    echo "Makefile не содержит test-local-menu-host" >&2
    exit 1
}
# shellcheck disable=SC2016  # the single-quoted text is what the Makefile says
grep -Fq '$(MAKE) test-local-menu-host' Makefile || {
    echo "test-host-suite-serial не вызывает test-local-menu-host" >&2
    exit 1
}
grep -Fq 'test-local-menu-host' tests/test-host-suite-manifest.sh || {
    echo "манифест host suite не содержит test-local-menu-host" >&2
    exit 1
}
for local_menu_console in \
    'cmd_local_menu' \
    'local_menu_handle_event' \
    'LOCAL-MENU] open' \
    'LOCAL-MENU] run action=' \
    'appliance_boot_log_store_flush'; do
    grep -Fq -- "$local_menu_console" kernel/src/console.c || {
        echo "консоль не содержит часть меню: $local_menu_console" >&2
        exit 1
    }
done
# The actions that take the machine down go through the paths ITEM-411 and
# ITEM-418 proved, and neither is called before the log is flushed.
grep -Fq 'platform_reset_execute(&plan)' kernel/src/console.c || {
    echo "меню не перезагружает машину через план ITEM-411" >&2
    exit 1
}
grep -Fq 'platform_power_execute(&plan)' kernel/src/console.c || {
    echo "меню не выключает машину через план ITEM-418" >&2
    exit 1
}
grep -Fq 'MENU' kernel/src/console.c || {
    echo "консоль не публикует команду menu" >&2
    exit 1
}
# The proof that the menu acts has to be the phase's own evidence: a target
# that exists but is absent from the phase's list never runs, and the item
# would merge on a green phase that never opened the menu.  ITEM-422 made the
# list the phase reads authoritative, so membership is pinned here.
test -f scripts/test-qemu-local-menu.sh || {
    echo "нет QEMU-доказательства локального меню" >&2
    exit 1
}
grep -Fq 'test-local-menu-qemu:' Makefile || {
    echo "Makefile не содержит цель test-local-menu-qemu" >&2
    exit 1
}
awk '/^QEMU_PARALLEL_PHASE_1 :=/,/^$/' Makefile |
    grep -Fq 'test-local-menu-qemu' || {
    echo "QEMU_PARALLEL_PHASE_1 не запускает доказательство локального меню" >&2
    exit 1
}
# ITEM-426/427: the wrapper dominates the deadlines the proof declares, so a
# starved guest writes its class record instead of dying at exit 124.
grep -Fq 'HFOC_LOCAL_MENU_TEST_TIMEOUT:-360}s' \
    scripts/test-qemu-local-menu.sh || {
    echo "обёртка доказательства меню не покрывает его дедлайны" >&2
    exit 1
}
grep -Fq 'MARKER_DEADLINE = 50.0' scripts/test-qemu-local-menu.sh || {
    echo "доказательство меню потеряло свой дедлайн маркера" >&2
    exit 1
}
grep -Fq "'HFOC-QEMU-MARKER-TIMEOUT '" scripts/test-qemu-local-menu.sh || {
    echo "отказ доказательства меню не объявляет класс" >&2
    exit 1
}

# ITEM-430: the machine offers its own menu when nobody asks for it.
#
# The menu is otherwise reachable only by typing `menu`, which assumes an
# operator who knows the word and a keyboard to type it on; on a machine with
# no local input at all it was unreachable in principle, and the 2026-09-16
# hardware run's screen is exactly that machine.  The decision is pure and
# host-tested, the console owns the paint and the one key that opens it, and
# the no-keyboard QEMU boot is the proof that a real machine without input
# really sees the menu.
for local_menu_offer in \
    'LOCAL_MENU_OFFER_MS 30000U' \
    'local_menu_offer_due('; do
    grep -Fq -- "$local_menu_offer" kernel/include/local_menu.h || {
        echo "ITEM-430: решение о предложении меню не объявлено:" \
            "$local_menu_offer" >&2
        exit 1
    }
done
grep -Fq 'local_menu_offer_due(' kernel/src/local_menu.c || {
    echo "ITEM-430: решение о предложении меню не реализовано" >&2
    exit 1
}
for local_menu_offer_console in \
    'void console_offer_local_menu(uint32_t now_ms);' \
    'console_offer_local_menu(hfoc_lwip_time_now());' \
    'LOCAL-MENU] offered reason=NO_LOCAL_INPUT entries=6' \
    'LOCAL-MENU] offered reason=IDLE entries=6' \
    'LOCAL-MENU] offer cancelled' \
    'console_input_seen = true;' \
    'MENU FOR THE LOCAL MENU' \
    '"console.menu.offered_reading"'; do
    grep -Fq -- "$local_menu_offer_console" kernel/include/console.h \
        kernel/src/console.c kernel/src/kernel.c || {
        echo "ITEM-430: консоль не содержит часть предложения:" \
            "$local_menu_offer_console" >&2
        exit 1
    }
done
# The offer opens on the empty-line ENTER only, so a command being typed is
# never hijacked; the console host test drives both halves.
for local_menu_offer_test in \
    'test_local_menu_offer();' \
    'ENTER on the empty line opens the offered menu' \
    'a keystroke cancels the painted offer' \
    'an operator who has typed is not offered a menu' \
    'a machine with no local input is offered the menu at once'; do
    grep -Fq -- "$local_menu_offer_test" tests/console-host.c || {
        echo "ITEM-430: host-проба консоли не содержит: " \
            "$local_menu_offer_test" >&2
        exit 1
    }
done
grep -Fq 'test_offer_decision();' tests/local-menu-host.c || {
    echo "ITEM-430: host-проба решения о предложении меню потеряна" >&2
    exit 1
}
# The machine without local input is where the offer is the only way the
# choices exist, so its QEMU boot has to require the offer and the painted
# menu - not only the decision's host test.
for local_menu_offer_proof in \
    '[LOCAL-MENU] offered reason=NO_LOCAL_INPUT entries=6' \
    'LOCAL INPUT UNAVAILABLE: THE MENU IS SHOWN FOR READING' \
    '[1] Status' \
    '[6] Recovery' \
    '[LOCAL-MENU] offer cancelled'; do
    grep -Fq -- "$local_menu_offer_proof" \
        scripts/test-qemu-production-storage-boot.sh || {
        echo "ITEM-430: QEMU-проба машины без клавиатуры не требует:" \
            "$local_menu_offer_proof" >&2
        exit 1
    }
done
grep -Fq 'ITEM-430' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-430" >&2
    exit 1
}

# ---------------------------------------------------------------------------
# ITEM-440: the menu is painted inside one serial sequence.
#
# The `keyboard_absent` phase of test-qemu-production-storage-boot.sh waits for
# the painted menu markers (`[1] Status`, `[6] Recovery`), and the paint wrote
# the rendered menu character by character through console_write(), so an IRQ
# trace line could land inside a menu line: observed on 2026-09-17 as
# `  [1] Stat` + `[IOAPIC] irq0 ...` + `us  (s)`, which the proof reads as a
# marker timeout even though the guest painted the menu.  The records already
# solve this class with serial_sequence_begin()/serial_sequence_end()
# (docs/ci-self-hosted-runner.md, "a split marker line is not a timeout"); the
# paint now does the same through one helper, and the console host proof makes
# the sequence observable.
for menu_atomic_paint in \
    'static void console_write_atomic(const char *text) {' \
    'const uint64_t flags = serial_sequence_begin();' \
    'console_write_atomic(rendered);' \
    'console_local_menu_text("console.menu.render_failed")'; do
    grep -Fq -- "$menu_atomic_paint" kernel/src/console.c || {
        echo "ITEM-440: меню не рисуется одной IRQ-безопасной записью:" \
            "$menu_atomic_paint" >&2
        exit 1
    }
done
if grep -Fq 'console_write(rendered);' kernel/src/console.c; then
    echo "ITEM-440: меню всё ещё рисуется посимвольно" >&2
    exit 1
fi
for menu_atomic_proof in \
    'serial_contains_atomic("[1] Status")' \
    'serial_contains_atomic("[6] Recovery")' \
    'serial_contains_atomic("UP/DOWN select, ENTER run, Q leave")' \
    'an unsequenced write is readable but not atomic' \
    'a sequenced write is atomic'; do
    grep -Fq -- "$menu_atomic_proof" tests/console-host.c || {
        echo "ITEM-440: host-проба консоли не проверяет атомарность:" \
            "$menu_atomic_proof" >&2
        exit 1
    }
done
grep -Fq 'ITEM-440' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-440" >&2
    exit 1
}

# ITEM-443: the menu's own records are IRQ-safe too.
#
# ITEM-440 fixed the painted menu and stopped there; the offer, the lines that
# explain it, the open/close markers, the action line and the refusal lines are
# written through console_write() one character at a time as well, and the
# `keyboard_absent` phase of test-qemu-production-storage-boot.sh waits for
# `[LOCAL-MENU] offered reason=NO_LOCAL_INPUT entries=6` - which CI then failed
# to find on 2026-09-17 (marker timeout at 19:09Z) exactly as it had failed to
# find the painted `[1] Status` before ITEM-440.
for menu_record_atomic in \
    '"[LOCAL-MENU] closed' \
    '"[LOCAL-MENU] offered reason=NO_LOCAL_INPUT entries=6' \
    '"console.menu.no_local_input"' \
    '"[LOCAL-MENU] offered reason=IDLE entries=6' \
    '"console.menu.offer_idle"' \
    '"[LOCAL-MENU] open' \
    '"[LOCAL-MENU] offer cancelled' \
    '"console.menu.offered_reading"'; do
    grep -Fq -- "$menu_record_atomic" kernel/src/console.c || {
        echo "ITEM-443: маркер меню не пишется одной IRQ-безопасной записью:" \
            "$menu_record_atomic" >&2
        exit 1
    }
done
# ITEM-449: the read-only offer's two lines are atomic as well.  The pin above
# matched the string literal, so a plain console_write() carrying it passed the
# check while a timer record could land inside the line - and the
# `keyboard_absent` phase of the storage-boot proof waits for exactly that line.
# Reproduced twice in the ITEM-449 acceptance phase (2026-09-18), so the call
# itself is pinned now, not only the text.
#
# ITEM-455: the offer record now carries the menu's keys, so it is composed in
# a buffer before it is written.  The guarantee is the same one - the whole
# marker is one write - and what is pinned is that shape: the marker literal is
# composed into `record` and `record` is written with one atomic call.  A
# rewrite back to a plain console_write() fails the pin.
grep -Fq -- '"[LOCAL-MENU] offered reason=NO_LOCAL_INPUT entries=6 keys="' \
    kernel/src/console.c || {
    echo "ITEM-449/455: маркер read-only предложения меню потерян" >&2
    exit 1
}
grep -Fq -- 'console_local_menu_text("console.menu.no_local_input")' \
    kernel/src/console.c || {
    echo "ITEM-449: вторая строка read-only предложения меню не пишется" \
        "атомарно" >&2
    exit 1
}
# ITEM-478 (round 16): the offer markers are service lines now, so they are
# written by the journal-only funnel.  The atomicity guarantee is unchanged -
# the funnel wraps the whole record in one `serial_sequence_begin()` critical
# section - and what is pinned is that shape: both composed offer records go
# through `console_service_line()`.
readonly_atomic_calls=$(grep -c -F 'console_service_line(record, strlen(record));' \
    kernel/src/console.c || true)
[[ $readonly_atomic_calls == 2 ]] || {
    echo "ITEM-449/455/478: предложение меню пишется не одной атомарной" \
        "записью ($readonly_atomic_calls вызовов)" >&2
    exit 1
}
grep -Fq 'serial_putc(record[index]);' kernel/src/console.c || {
    echo "ITEM-478: служебная воронка не пишет строку целиком" >&2
    exit 1
}

# The action line and the shutdown refusal are assembled from several writes
# and composed into one buffer before the funnel writes them, so the whole
# marker is one IRQ-safe sequence.
grep -Fq 'static const char marker[] = "[LOCAL-MENU] run action=";' \
    kernel/src/console.c || {
    echo "ITEM-443/478: строка действия меню потеряна" >&2
    exit 1
}
grep -Fq 'console_service_line(action_record, action_length);' \
    kernel/src/console.c || {
    echo "ITEM-478: строка действия меню пишется не одной записью" >&2
    exit 1
}
grep -Fq 'static const char refusal[] = "[LOCAL-MENU] shutdown refused: ";' \
    kernel/src/console.c || {
    echo "ITEM-443/478: строка отказа выключения потеряна" >&2
    exit 1
}
grep -Fq 'console_service_line(refusal_record, refusal_length);' \
    kernel/src/console.c || {
    echo "ITEM-478: строка отказа выключения пишется не одной записью" >&2
    exit 1
}
# ITEM-455: the two offer markers grew the key list and the network hint, so the
# strings the host proof asserts atomically are the new ones; the guarantee is
# the one ITEM-443 pinned.
for menu_record_proof in \
    'serial_contains_atomic(' \
    '"[LOCAL-MENU] offered reason=IDLE entries=6 keys=s,n,d,r,p,y"' \
    '"PRESS ENTER FOR THE LOCAL MENU (N: NETWORK SETUP - SET THE "' \
    'serial_contains_atomic("[LOCAL-MENU] open")' \
    'serial_contains_atomic("[LOCAL-MENU] closed")' \
    'serial_contains_atomic("[LOCAL-MENU] offer cancelled")'; do
    grep -Fq -- "$menu_record_proof" tests/console-host.c || {
        echo "ITEM-443: host-проба не требует атомарности маркера:" \
            "$menu_record_proof" >&2
        exit 1
    }
done
grep -Fq 'ITEM-443' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-443" >&2
    exit 1
}

# ITEM-455: the offer names the key that opens the network editor, and the
# record publishes the keys the menu answers to.
#
# The owner never found where the static address is set: the menu has the
# editor (ITEM-447: Network, key `n`), but the offer he would have read named
# only ENTER, and his run carried no `[LOCAL-MENU]` record at all.  The record
# now publishes `keys=` from the menu's own table, and the idle offer line
# names the network key and says the address is set there.
for menu_key_file in \
    kernel/src/console.c \
    kernel/include/console.h \
    kernel/src/local_menu.c \
    kernel/include/local_menu.h \
    tests/console-host.c \
    tests/local-menu-host.c; do
    [[ -f $menu_key_file ]] || {
        echo "ITEM-455: отсутствует $menu_key_file" >&2
        exit 1
    }
done
grep -Fq -- 'local_menu_key_list(char *output, size_t capacity)' \
    kernel/src/local_menu.c || {
    echo "ITEM-455: у меню нет аксессора клавиш" >&2
    exit 1
}
grep -Fq -- 'size_t local_menu_key_list(char *output, size_t capacity);' \
    kernel/include/local_menu.h || {
    echo "ITEM-455: заголовок меню не объявляет аксессор клавиш" >&2
    exit 1
}
grep -Fq -- '"keys="' kernel/src/console.c && {
    echo "ITEM-455: запись оффера собирается не из одной строки" >&2
    exit 1
}
for menu_key_record in \
    'reason=NO_LOCAL_INPUT entries=6 keys=' \
    'reason=IDLE entries=6 keys=' \
    '"console.menu.offer_idle"' \
    'local_menu_key_list(keys, sizeof(keys))' \
    'console_offer_record_append'; do
    grep -Fq -- "$menu_key_record" kernel/src/console.c || {
        echo "ITEM-455: запись оффера не называет: $menu_key_record" >&2
        exit 1
    }
done
grep -Fq -- 'keys=s,n,d,r,p,y' kernel/include/console.h || {
    echo "ITEM-455: форма записи оффера не задокументирована" >&2
    exit 1
}
for menu_key_host in \
    'static void test_key_list(void)' \
    'test_key_list();' \
    'strcmp(list, "s,n,d,r,p,y") == 0' \
    'a buffer too small for the list writes nothing' ; do
    grep -Fq -- "$menu_key_host" tests/local-menu-host.c || {
        echo "ITEM-455: host-проба меню не утверждает: $menu_key_host" >&2
        exit 1
    }
done
for menu_key_console in \
    'keys=s,n,d,r,p,y' \
    '(N: NETWORK SETUP - SET THE ADDRESS THERE)' \
    'and the record publishes the keys the menu answers to' ; do
    grep -Fq -- "$menu_key_console" tests/console-host.c || {
        echo "ITEM-455: host-проба консоли не утверждает: $menu_key_console" >&2
        exit 1
    }
done
