#!/usr/bin/env bash
# ITEM-399: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The installed appliance is the product appliance. The media separates the two
# kernels (the ISO boots the installer, the ESP receives the product kernel),
# the installed configuration states the management scope the operator declared,
# and the production network record names the silicon the driver identified.
for product_appliance_file in \
    scripts/hfoc-installed-config.py \
    scripts/hfoc-esp-read.py \
    boot/limine-installer.conf \
    boot/hfoc-installed.conf \
    kernel/src/installer_boot_menu.c \
    kernel/include/rtl8168.h \
    tests/rtl8168-host.c \
    docs/hfoc-production-install-flow.md \
    docs/hfoc-installed-boot.md; do
    test -f "$product_appliance_file" || {
        echo "Отсутствует ITEM-399 file: $product_appliance_file" >&2
        exit 1
    }
done

# The medium carries an installer kernel and an installed kernel, and the two
# are distinct roles in the staging script and in the medium's own entries.
for product_appliance_stage in \
    'INSTALLER_KERNEL_ELF' \
    'INSTALLED_KERNEL_ELF' \
    'boot/hfoc-installer-kernel.elf' \
    'boot/hfoc-kernel.elf' \
    'scripts/hfoc-installed-config.py'; do
    grep -Fq "$product_appliance_stage" \
        scripts/hfoc-install-media-stage.sh || {
        echo "medium staging не разделяет два ядра: $product_appliance_stage" >&2
        exit 1
    }
done
# shellcheck disable=SC2016  # the single-quoted text is what the script says
grep -Fq -- '--kernel "$ISO_ROOT/boot/hfoc-kernel.elf"' \
    scripts/hfoc-install-media-stage.sh || {
    echo "sealed media manifest не bind'ит installed kernel" >&2
    exit 1
}
if ! grep -Fq 'installer_kernel_sha' scripts/hfoc-install-media-stage.sh ||
    ! grep -Fq 'installed_kernel_sha' scripts/hfoc-install-media-stage.sh; then
    echo "medium staging не отказывает одинаковым ядрам" >&2
    exit 1
fi
grep -Fq 'path: boot():/boot/hfoc-installer-kernel.elf' \
    boot/limine-installer.conf || {
    echo "installer entries не грузят installer kernel" >&2
    exit 1
}
grep -Fq 'module_path: boot():/boot/hfoc-kernel.elf' \
    boot/limine-installer.conf || {
    echo "installer entries не передают installed kernel модулем" >&2
    exit 1
}
grep -Fq 'boot():/boot/hfoc-kernel.elf' kernel/src/kernel.c || {
    echo "kernel не берёт installed kernel с medium" >&2
    exit 1
}
if grep -Fq 'media.kernel = executable->address' kernel/src/kernel.c; then
    echo "kernel всё ещё ставит собственный installer kernel" >&2
    exit 1
fi
grep -Fq 'INSTALLED_KERNEL_MISSING' kernel/src/production_install_flow.c || {
    echo "install flow не отказывает medium без installed kernel" >&2
    exit 1
}

# The generated configuration: the template names the staged kernel, and the
# generator writes exactly one explicit management scope into it.
grep -Fq 'path: boot():/boot/hfoc-kernel.elf' boot/hfoc-installed.conf || {
    echo "installed configuration template не называет staged kernel" >&2
    exit 1
}
product_appliance_dir=$(mktemp -d)
trap 'rm -rf "$product_appliance_dir"' EXIT
for product_appliance_scope in local-only management-interface; do
    python3 scripts/hfoc-installed-config.py \
        --template boot/hfoc-installed.conf \
        --management "$product_appliance_scope" \
        --output "$product_appliance_dir/$product_appliance_scope.conf" \
        >/dev/null || {
        echo "installed-config generator не работает: $product_appliance_scope" >&2
        exit 1
    }
    grep -Fq "cmdline: hfoc.management=$product_appliance_scope" \
        "$product_appliance_dir/$product_appliance_scope.conf" || {
        echo "installed configuration не несёт scope: $product_appliance_scope" >&2
        exit 1
    }
    [[ "$(grep -c 'cmdline:' \
        "$product_appliance_dir/$product_appliance_scope.conf")" == 1 ]] || {
        echo "installed configuration несёт больше одного cmdline" >&2
        exit 1
    }
    grep -Fq 'path: boot():/boot/hfoc-kernel.elf' \
        "$product_appliance_dir/$product_appliance_scope.conf" || {
        echo "installed configuration потеряла kernel reference" >&2
        exit 1
    }
    grep -Fq 'module_path: boot():/boot/hfoc-boot.man' \
        "$product_appliance_dir/$product_appliance_scope.conf" || {
        echo "installed configuration потеряла boot-chain модули" >&2
        exit 1
    }
done
if python3 scripts/hfoc-installed-config.py \
    --template boot/hfoc-installed.conf --management bogus \
    --output "$product_appliance_dir/bogus.conf" >/dev/null 2>&1; then
    echo "installed-config generator принимает неизвестный scope" >&2
    exit 1
fi

# The generated menu is the installed configuration whenever a system has to
# stay selectable, so its appliance entry carries the same scope.
grep -Fq 'INSTALLER_BOOT_MENU_CMDLINE_LENGTH' \
    kernel/include/installer_boot_menu.h || {
    echo "boot menu не фиксирует cmdline bound" >&2
    exit 1
}
grep -Fq 'char cmdline[INSTALLER_BOOT_MENU_CMDLINE_LENGTH]' \
    kernel/include/installer_boot_menu.h || {
    echo "boot menu entry не несёт cmdline" >&2
    exit 1
}
grep -Fq 'const char *cmdline;' kernel/include/installer_boot_menu.h || {
    echo "boot menu entry source не принимает cmdline" >&2
    exit 1
}
grep -Fq '"    cmdline: "' kernel/src/installer_boot_menu.c || {
    echo "boot menu не рендерит cmdline" >&2
    exit 1
}
grep -Fq '" cmdline="' kernel/src/installer_boot_menu.c || {
    echo "boot menu record не публикует cmdline" >&2
    exit 1
}
# ITEM-446: the entry command line the flow writes is composed by the one
# owner of the declaration, so the management scope and the declared network
# cannot drift apart between the sealed configuration and the generated menu.
if ! grep -Fq 'PRODUCTION_INSTALL_MEDIA_CMDLINE_MANAGEMENT_LOCAL' \
        kernel/src/production_install_media.c ||
    ! grep -Fq 'PRODUCTION_INSTALL_MEDIA_CMDLINE_MANAGEMENT_INTERFACE' \
        kernel/src/production_install_media.c ||
    ! grep -Fq 'production_install_media_installed_cmdline' \
        kernel/src/production_install_flow.c; then
    echo "install flow не ставит scope в appliance entry" >&2
    exit 1
fi
if ! grep -Fq 'production_install_media_management_declared' \
        kernel/src/kernel.c ||
    ! grep -Fq 'source=boot-declaration' kernel/src/kernel.c; then
    echo "kernel не публикует источник management scope" >&2
    exit 1
fi
grep -Fq 'production_install_media_management_declared' \
    tests/production-install-media-host.c || {
    echo "media host proof не проверяет объявленный scope" >&2
    exit 1
}

# The production network record names the silicon the Realtek backend
# identified, in the same single-line record and the same serial sequence.
if ! grep -Fq 'rtl8168_chip_render_identity' kernel/include/rtl8168.h ||
    ! grep -Fq 'rtl8168_chip_family_string' kernel/include/rtl8168.h; then
    echo "rtl8168 не публикует chip identity" >&2
    exit 1
fi
grep -Fq '" xid=0x' kernel/src/rtl8168.c || {
    echo "rtl8168 identity не несёт xid=" >&2
    exit 1
}
grep -Fq '" family="' kernel/src/rtl8168.c || {
    echo "rtl8168 identity не несёт family=" >&2
    exit 1
}
if ! grep -Fq 'appliance_production_network_boot_chip_record' \
        kernel/src/appliance_production_network_x86.c ||
    ! grep -Fq 'rtl8168_chip_identify' \
        kernel/src/appliance_production_network_x86.c; then
    echo "production network backend не публикует chip" >&2
    exit 1
fi
grep -Fq 'appliance_production_network_boot_chip_record' \
    kernel/src/kernel.c || {
    echo "kernel не публикует chip в [PRODUCTION-NET]" >&2
    exit 1
}
for product_appliance_net_sequence in \
    'serial_sequence_begin' \
    'serial_sequence_end' \
    'appliance_production_network_boot_chip_record'; do
    grep -Fq "$product_appliance_net_sequence" kernel/src/kernel.c || {
        echo "production network record потерял: $product_appliance_net_sequence" >&2
        exit 1
    }
done
# The chip identity is pinned by a host proof, at the two parts that matter and
# at the value an unidentified part publishes.
for product_appliance_chip in \
    'xid=0x2C8 family=RTL8168-EXTENDED' \
    'xid=0x541 family=RTL8168-EXTENDED' \
    'xid=0x300 family=RTL8168-CLASSIC' \
    'xid=0x7CF family=UNKNOWN'; do
    grep -Fq "$product_appliance_chip" tests/rtl8168-host.c || {
        echo "rtl8168 host proof не пинует: $product_appliance_chip" >&2
        exit 1
    }
done
for product_appliance_target in \
    test-rtl8168-host \
    test-production-install-media-host; do
    grep -Fq "$product_appliance_target" Makefile || {
        echo "Makefile не содержит $product_appliance_target" >&2
        exit 1
    }
    grep -Fq "$product_appliance_target" \
        tests/test-host-suite-manifest.sh || {
        echo "host suite manifest не пинует $product_appliance_target" >&2
        exit 1
    }
done

# The proofs: the installed disk runs the product kernel, carries the sealed
# configuration with its scope, and publishes the declared scope.
for product_appliance_proof in \
    '[PRODUCTION-INSTALLER]' \
    'assert_installed_configuration' \
    'scripts/hfoc-esp-read.py' \
    'cmdline: hfoc.management=' \
    '[PRODUCTION-MGMT-BIND] scope={bind_scope} ' \
    'source=boot-declaration' \
    'MANAGEMENT_INTERFACE' \
    'boot\hfoc-kernel.elf' \
    "the medium's installed (product) kernel"; do
    grep -Fq "$product_appliance_proof" \
        scripts/test-qemu-production-install-flow.sh || {
        echo "install flow proof не проверяет: $product_appliance_proof" >&2
        exit 1
    }
done
for product_appliance_proof in \
    '[PRODUCTION-INSTALLER]' \
    'assert_installed_menu' \
    'cmdline: hfoc.management=local-only' \
    '[PRODUCTION-MGMT-BIND] scope=LOCAL_ONLY '; do
    grep -Fq "$product_appliance_proof" \
        scripts/test-qemu-production-install-flow-preserve.sh || {
        echo "preserve proof не проверяет: $product_appliance_proof" >&2
        exit 1
    }
done
grep -Fq 'production-installer-management-media' Makefile || {
    echo "Makefile не собирает medium с management scope" >&2
    exit 1
}
grep -Fq 'PRODUCTION_INSTALLER_MANAGEMENT_ISO' Makefile || {
    echo "Makefile не несёт management medium" >&2
    exit 1
}
# shellcheck disable=SC2016  # the single-quoted text is a Makefile recipe
grep -Fq "AUTHORISED_ISO_ROOT='\$(PRODUCTION_INSTALLER_AUTHORISED_ISO_ROOT)'" \
    Makefile || {
    echo "install-flow proof не получает medium ISO root" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq "MANAGEMENT_IMAGE='\$(PRODUCTION_INSTALLER_MANAGEMENT_ISO)'" \
    Makefile || {
    echo "install-flow proof не получает management medium" >&2
    exit 1
}

# The medium the proof installs from is built from two distinct kernels, and
# the installed one is the product build.
grep -Fq 'scripts/hfoc-installed-config.py' Makefile || {
    echo "Makefile не собирает installed configuration генератором" >&2
    exit 1
}

# Documented contract: the two kernels, the installed configuration and the
# scope are the product documents' text, not only the code's.
for product_appliance_documented in \
    'hfoc-installer-kernel.elf' \
    'hfoc-installed-config.py' \
    'cmdline'; do
    grep -Fq "$product_appliance_documented" \
        docs/hfoc-production-install-flow.md || {
        echo "hfoc-production-install-flow.md не фиксирует: $product_appliance_documented" >&2
        exit 1
    }
done
for product_appliance_documented in \
    'hfoc-installer-kernel.elf' \
    'cmdline'; do
    grep -Fq "$product_appliance_documented" docs/hfoc-installed-boot.md || {
        echo "hfoc-installed-boot.md не фиксирует: $product_appliance_documented" >&2
        exit 1
    }
done
grep -Fq 'ITEM-399' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не документирует ITEM-399" >&2
    exit 1
}
grep -Fq 'xid=' docs/hfoc-hardware-acceptance.md || {
    echo "hardware acceptance doc не публикует chip identity" >&2
    exit 1
}

# ITEM-423: a menu that offers fewer systems than the machine carries says why.
#
# The 2026-09-15 hardware run ended its menu record at `disks=1` on a machine
# with two disks.  One candidate cannot distinguish "the controller was never
# matched at the PCI level" from "it was matched and the NVMe bring-up stopped"
# from "it was brought up and produced no namespace" - three defects with three
# different fixes - so the record now carries what the probe saw.
for inventory_evidence in \
    'appliance_production_storage_evidence' \
    'pci_ahci' \
    'pci_nvme' \
    'nvme_status'; do
    grep -Fq -- "$inventory_evidence" \
        kernel/include/appliance_production_storage.h || {
        echo "публикация инвентаря дисков не содержит: $inventory_evidence" >&2
        exit 1
    }
done
grep -Fq 'nvme_status_string' kernel/src/production_install_flow.c || {
    echo "запись меню не называет шаг NVMe, на котором остановился пробник" >&2
    exit 1
}
for inventory_record in \
    'pci_ahci=' \
    'pci_nvme=' \
    'nvme_status='; do
    grep -Fq -- "$inventory_record" kernel/src/production_install_flow.c || {
        echo "запись меню не публикует: $inventory_record" >&2
        exit 1
    }
    grep -Fq -- "$inventory_record" scripts/test-qemu-production-install-flow.sh || {
        echo "proof не требует поля инвентаря: $inventory_record" >&2
        exit 1
    }
done
grep -Fq 'ITEM-423' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-423" >&2
    exit 1
}

# ITEM-429: the NVMe bring-up waits on the clock, and the record says how long.
#
# The 2026-09-16 hardware run's NVMe probe ended in `NVME_TIMEOUT` while QEMU
# brings the same controller up instantly.  The driver waited on a loop count -
# a hundred MMIO reads, microseconds - against a specification that allows the
# controller 500 ms, so "this part needs a few hundred milliseconds" and "this
# part never answers" were the same result.  The wait is bounded in
# milliseconds now, and `nvme_step=`/`nvme_wait_ms=` separate the two findings
# on the next run.
for nvme_wait_contract in \
    'NVME_CONTROLLER_READY_BUDGET_MS 1000U' \
    'NVME_CONTROLLER_READY_STEP_MS 20U' \
    'nvme_mmio_delay_ms_fn' \
    'delay_ms'; do
    grep -Fq -- "$nvme_wait_contract" kernel/include/nvme.h || {
        echo "ITEM-429: бюджет ожидания NVMe не объявлен:" \
            "$nvme_wait_contract" >&2
        exit 1
    }
done
# The bound itself, not only the constants: the budget break is what makes the
# loop terminate when the controller never answers.
for nvme_wait_caller in \
    'nvme_wait_controller_ready(' \
    'if (waited_ms >= NVME_CONTROLLER_READY_BUDGET_MS) {' \
    'io->delay_ms(io->context, NVME_CONTROLLER_READY_STEP_MS);' \
    'controller->ready_wait_ms = waited_ms;'; do
    grep -Fq -- "$nvme_wait_caller" kernel/src/nvme.c || {
        echo "ITEM-429: ожидание готовности NVMe не ограничено часами:" \
            "$nvme_wait_caller" >&2
        exit 1
    }
done
# The kernel provider hands the PIT back after the wait: a delay that leaves
# channel 0 in its own rate mode slows every clock in the machine.
grep -Fq 'slot->mmio.delay_ms = production_storage_nvme_delay_ms;' \
    kernel/src/appliance_production_storage.c || {
    echo "ITEM-429: провайдер NVMe не даёт драйверу задержку" >&2
    exit 1
}
grep -Fq 'timer_delay_ms(milliseconds);' \
    kernel/src/appliance_production_storage.c || {
    echo "ITEM-429: задержка NVMe не возвращает PIT таймеру" >&2
    exit 1
}
grep -Fq 'void timer_delay_ms(uint32_t milliseconds);' \
    kernel/include/arch/x86_64/timer.h || {
    echo "ITEM-429: timer_delay_ms не объявлен" >&2
    exit 1
}
grep -Fq 'void timer_delay_ms(uint32_t milliseconds) {' \
    kernel/src/arch/x86_64/timer.c || {
    echo "ITEM-429: timer_delay_ms не реализован" >&2
    exit 1
}
grep -Fq 'timer_init(timer_frequency);' kernel/src/arch/x86_64/timer.c || {
    echo "ITEM-429: timer_delay_ms не восстанавливает делитель таймера" >&2
    exit 1
}
# The fields themselves and their wiring, not only the words in a comment.
for nvme_evidence_field in \
    'const char *nvme_step;' \
    'uint32_t nvme_wait_ms;'; do
    grep -Fq -- "$nvme_evidence_field" \
        kernel/include/appliance_production_storage.h || {
        echo "ITEM-429: свидетельство NVMe не содержит поле:" \
            "$nvme_evidence_field" >&2
        exit 1
    }
done
for nvme_evidence_wire in \
    'out->nvme_step =' \
    'out->nvme_wait_ms =' \
    'production_storage.nvme_step = "RESET";' \
    'production_storage.nvme_step = "READY";' \
    'production_storage.nvme_status = (int)NVME_OK;' \
    'production_storage.nvme_step = "BIND";' \
    'production_storage.nvme_step = "MAP";' \
    'production_storage.nvme_step = "DMA";' \
    'production_storage.nvme_wait_ms = 0U;' \
    'production_storage.nvme_status = (int)bind_status;'; do
    grep -Fq -- "$nvme_evidence_wire" \
        kernel/src/appliance_production_storage.c || {
        echo "ITEM-429: свидетельство NVMe не заполняется:" \
            "$nvme_evidence_wire" >&2
        exit 1
    }
done
# The fallback proof is part of the item, not an extra: the call is pinned so
# the no-seam path cannot lose its test with every check still green.
grep -Fq 'test_controller_ready_wait_without_a_seam();' tests/nvme-host.c || {
    echo "ITEM-429: host-проба не проверяет путь без шва задержки" >&2
    exit 1
}
for nvme_wait_evidence in \
    'nvme_step' \
    'nvme_wait_ms'; do
    grep -Fq -- "\" $nvme_wait_evidence=\"" \
        kernel/src/production_install_flow.c || {
        echo "ITEM-429: запись меню не публикует: $nvme_wait_evidence" >&2
        exit 1
    }
    grep -Fq -- "$nvme_wait_evidence=" \
        scripts/test-qemu-production-install-flow.sh || {
        echo "ITEM-429: proof не требует поля: $nvme_wait_evidence" >&2
        exit 1
    }
done
# The proof of the wait itself is the host suite's: the target is pinned here
# so it cannot be dropped from the phase that runs it.
grep -Fq 'test-nvme-host:' Makefile || {
    echo "Makefile не содержит test-nvme-host" >&2
    exit 1
}
# shellcheck disable=SC2016  # the single-quoted text is what the Makefile says
grep -Fq '$(MAKE) test-nvme-host' Makefile || {
    echo "test-host-suite-serial не вызывает test-nvme-host" >&2
    exit 1
}
grep -Fq 'ready_delay_ms' tests/nvme-host.c || {
    echo "ITEM-429: host-проба не ждёт контроллер на часах" >&2
    exit 1
}
grep -Fq 'ITEM-429' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-429" >&2
    exit 1
}
# ITEM-636: the production media carry the native userland, not the bare
# source tree with an empty /bin.  `make iso` already packed the staged rootfs
# (rootfs plus the real ELF programs and /bin/sh); the install media now name
# that same tree and refuse a declared rootfs without a shell.  The pins below
# keep the declaration, the export and the one packer call together.
# shellcheck disable=SC2016  # the single-quoted text is what the files say
grep -Fq 'HFOC_INSTALL_MEDIA_ROOTFS ?= $(USER_ROOTFS_STAGE)' Makefile || {
    echo "ITEM-636: install media не объявляют staged rootfs" >&2
    exit 1
}
grep -Fq 'export HFOC_INSTALL_MEDIA_ROOTFS' Makefile || {
    echo "ITEM-636: staged rootfs не экспортируется в stage" >&2
    exit 1
}
# shellcheck disable=SC2016  # see above
grep -Fq 'ROOTFS_SOURCE=${HFOC_INSTALL_MEDIA_ROOTFS:-rootfs}' \
    scripts/hfoc-install-media-stage.sh || {
    echo "ITEM-636: stage не читает объявленный rootfs" >&2
    exit 1
}
# shellcheck disable=SC2016  # the single-quoted text is what the script says
grep -Fq 'python3 scripts/hfoc-rootfs-pack.py "$ROOTFS_SOURCE"' \
    scripts/hfoc-install-media-stage.sh || {
    echo "ITEM-636: stage пакует не объявленный rootfs" >&2
    exit 1
}
grep -Fq 'The declared root initramfs has no /bin/sh' \
    scripts/hfoc-install-media-stage.sh || {
    echo "ITEM-636: stage не отказывает rootfs без shell" >&2
    exit 1
}
# The update proof reads the archived rootfs beside the kernel and requires
# /bin/sh inside it, so the artifact gate is not only a source grep.
grep -Fq 'rootfs_archive_has_shell' \
    scripts/test-qemu-production-install-flow-update.sh || {
    echo "ITEM-636: update proof не проверяет /bin/sh в архиве" >&2
    exit 1
}
