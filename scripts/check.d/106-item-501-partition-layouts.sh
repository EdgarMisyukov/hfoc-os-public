#!/usr/bin/env bash
# ITEM-501: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The machine's disks are described as they are: containers, volumes, plain.
#
# The owner's laptop carries Fedora on LVM over MBR logical partitions.  The
# discovery read exactly four MBR primary entries and knew nothing about LVM or
# Btrfs, so the screen that asks the operator to choose a disk could only offer
# "one large Linux/unknown partition" - not enough to decide anything safely on.
#
# This slice pins the discovery half of the item, which is what the rest of the
# item is allowed to build on:
#
#   * every partition carries one named layout: PLAIN, LVM, BTRFS, EXTENDED or
#     LOGICAL, and the name is a stable string;
#   * the MBR extended container (0x05/0x0F/0x85) is published as a container -
#     a table, never a target - and its EBR chain is walked with a hard bound,
#     so every logical partition is listed with its own extent and an index of
#     5 or more;
#   * a GPT Linux LVM type GUID is named LVM, and a Linux filesystem whose
#     superblock carries the Btrfs magic is named BTRFS (read as a whole,
#     aligned block, because that is what the block layer serves);
#   * the overlap rule never counts the extended container against the logical
#     partitions that live inside its extent by construction.
#
# The remainder of the item is pinned in the second half of this slice: the
# screen and the record name the layout, free space is published beside what the
# appliance needs, and the mandatory "release the selected partition" mode
# exists from the operator screen to the GPT entry it removes.
# The second half of the item is pinned below as well: the screen that names
# what may be given up, the numbers the free-space path is decided by, the
# release planner and contract, the preserving writer that removes exactly the
# named entry, the flow record that says which partition the operator gave up,
# and the QEMU proof that runs the gesture and boots the disk it produced.

for item501_file in \
    kernel/include/installer_hardware_discovery.h \
    kernel/src/installer_hardware_discovery.c \
    kernel/include/installer_storage.h \
    kernel/src/installer_storage.c \
    kernel/include/installer_storage_screen.h \
    kernel/src/installer_storage_screen.c \
    kernel/include/installer_dual_boot.h \
    kernel/src/installer_dual_boot.c \
    kernel/include/installer_layout_contract.h \
    kernel/src/installer_layout_contract.c \
    kernel/include/installer_gpt_preserve.h \
    kernel/src/installer_gpt_preserve.c \
    kernel/src/production_install_flow.c \
    kernel/src/production_installer.c \
    kernel/src/production_installer_preserve.c \
    tests/installer-hardware-discovery-host.c; do
    test -f "$item501_file" || {
        echo "ITEM-501: отсутствует файл: $item501_file" >&2
        exit 1
    }
done

# ------------------------------------------------------- one named layout ---
for item501_layout in \
    INSTALLER_PARTITION_LAYOUT_PLAIN \
    INSTALLER_PARTITION_LAYOUT_LVM \
    INSTALLER_PARTITION_LAYOUT_BTRFS \
    INSTALLER_PARTITION_LAYOUT_EXTENDED \
    INSTALLER_PARTITION_LAYOUT_LOGICAL; do
    grep -Fq "$item501_layout" \
        kernel/include/installer_hardware_discovery.h || {
        echo "ITEM-501: layout не объявлен: $item501_layout" >&2
        exit 1
    }
done
for item501_name in \
    'return "PLAIN";' \
    'return "LVM";' \
    'return "BTRFS";' \
    'return "EXTENDED";' \
    'return "LOGICAL";'; do
    grep -Fq "$item501_name" kernel/src/installer_hardware_discovery.c || {
        echo "ITEM-501: layout не имеет имени: $item501_name" >&2
        exit 1
    }
done
grep -Fq 'installer_partition_layout_name(' \
    kernel/include/installer_hardware_discovery.h \
    kernel/src/installer_hardware_discovery.c || {
    echo "ITEM-501: имя layout не публикуется" >&2
    exit 1
}
grep -Fq 'enum installer_partition_layout layout;' \
    kernel/include/installer_hardware_discovery.h || {
    echo "ITEM-501: раздел не несёт свой layout" >&2
    exit 1
}
grep -Fq 'bool container;' kernel/include/installer_hardware_discovery.h || {
    echo "ITEM-501: контейнер не отличим от раздела-цели" >&2
    exit 1
}

# ------------------------------------------- the MBR extended partition ------
grep -Fq '#define INSTALLER_DISCOVERY_EBR_MAX 32U' \
    kernel/src/installer_hardware_discovery.c || {
    echo "ITEM-501: обход EBR не ограничен" >&2
    exit 1
}
for item501_extended in \
    'INSTALLER_DISCOVERY_MBR_EXTENDED_FIRST 0x05U' \
    'INSTALLER_DISCOVERY_MBR_EXTENDED_LBA 0x0FU' \
    'INSTALLER_DISCOVERY_MBR_EXTENDED_LINUX 0x85U'; do
    grep -Fq "#define $item501_extended" \
        kernel/src/installer_hardware_discovery.c || {
        echo "ITEM-501: extended-тип не назван: $item501_extended" >&2
        exit 1
    }
done
grep -Fq 'installer_discovery_parse_ebr_chain(' \
    kernel/src/installer_hardware_discovery.c || {
    echo "ITEM-501: цепочка EBR не читается" >&2
    exit 1
}
grep -Fq 'INSTALLER_PARTITION_LAYOUT_EXTENDED' \
    kernel/src/installer_hardware_discovery.c || {
    echo "ITEM-501: контейнер не помечается на чтении MBR" >&2
    exit 1
}
grep -Fq 'INSTALLER_PARTITION_LAYOUT_LOGICAL' \
    kernel/src/installer_hardware_discovery.c || {
    echo "ITEM-501: логический раздел не помечается" >&2
    exit 1
}
grep -Fq '5U + (uint32_t)logical_index' \
    kernel/src/installer_hardware_discovery.c || {
    echo "ITEM-501: логические разделы не нумеруются с 5" >&2
    exit 1
}

# ------------------------------------------------ GPT names LVM and Btrfs ----
grep -Fq 'installer_discovery_lvm_guid' \
    kernel/src/installer_hardware_discovery.c || {
    echo "ITEM-501: GUID Linux LVM не распознаётся" >&2
    exit 1
}
grep -Fq 'installer_discovery_probe_btrfs(' \
    kernel/src/installer_hardware_discovery.c || {
    echo "ITEM-501: Btrfs не распознаётся по суперблоку" >&2
    exit 1
}
grep -Fq "'_', 'B', 'H', 'R', 'f', 'S', '_', 'M'" \
    kernel/src/installer_hardware_discovery.c || {
    echo "ITEM-501: магия Btrfs не записана" >&2
    exit 1
}
grep -Fq 'INSTALLER_DISCOVERY_BTRFS_MAGIC_OFFSET /' \
    kernel/src/installer_hardware_discovery.c || {
    echo "ITEM-501: суперблок Btrfs читается не выровненным блоком" >&2
    exit 1
}

# --------------------------------------------- the container is not a claim --
grep -Fq 'partition->container ||' \
    kernel/src/installer_hardware_discovery.c || {
    echo "ITEM-501: правило перекрытия снова считает контейнер разделом" >&2
    exit 1
}

# ------------------------------------------------------------- the proof -----
for item501_proof in \
    'every partition layout has one stable name' \
    'the scan publishes the container and both logical partitions' \
    'the extended container is published as a container, not a target' \
    'the second EBR'"'"'s logical partition is reached through the chain' \
    'the LVM physical volume is named as LVM' \
    'the Btrfs filesystem is named by its superblock' \
    'a Linux partition without the Btrfs magic stays plain'; do
    grep -Fq "$item501_proof" \
        tests/installer-hardware-discovery-host.c || {
        echo "ITEM-501: host-проба не утверждает: $item501_proof" >&2
        exit 1
    }
done
grep -Fq '    test_mbr_logical_partitions();' \
    tests/installer-hardware-discovery-host.c || {
    echo "ITEM-501: проба логических разделов не вызывается" >&2
    exit 1
}
grep -Fq '    test_gpt_names_lvm_and_btrfs();' \
    tests/installer-hardware-discovery-host.c || {
    echo "ITEM-501: проба LVM/Btrfs не вызывается" >&2
    exit 1
}

# ------------------------------------------------- the screen names a target --
for item501_screen in \
    'struct installer_storage_partition_model' \
    'installer_storage_partitions_count(' \
    'installer_storage_partitions_free_lba(' \
    'installer_storage_partition_warning_lines(' \
    'installer_storage_partition_record('; do
    grep -Fq "$item501_screen" kernel/include/installer_storage.h || {
        echo "ITEM-501: экран разделов не публикует: $item501_screen" >&2
        exit 1
    }
done
grep -Fq 'installer_storage_partition_releasable' \
    kernel/src/installer_storage.c || {
    echo "ITEM-501: правило «что можно отдать» не названо" >&2
    exit 1
}
# ITEM-509 increment 18 moved the three sentences to the one catalog: the pins
# follow them - key in the renderer, English value verbatim in the catalog.
grep -Fq '"installer.storage.partition_warning_title"' \
    kernel/src/installer_storage.c || {
    echo "ITEM-501: страница предупреждения не читает каталог" >&2
    exit 1
}
grep -Fq '"WARNING: THE DATA ON THIS PARTITION WILL BE DELETED"' \
    kernel/src/appliance_language.c || {
    echo "ITEM-501: каталог не говорит, что погибнет" >&2
    exit 1
}
grep -Fq '"installer.storage.free_space_prefix"' \
    kernel/src/installer_storage.c || {
    echo "ITEM-501: экран не читает свободное место из каталога" >&2
    exit 1
}
grep -Fq '"  Free space: "' kernel/src/appliance_language.c || {
    echo "ITEM-501: каталог потерял строку свободного места" >&2
    exit 1
}
grep -Fq '"installer.storage.partition_not_enough"' \
    kernel/src/installer_storage.c || {
    echo "ITEM-501: экран не читает нехватку места из каталога" >&2
    exit 1
}
grep -Fq 'Not enough free space: give up a partition, or Q ' \
    kernel/src/appliance_language.c || {
    echo "ITEM-501: каталог потерял строку нехватки места" >&2
    exit 1
}
grep -Fq 'installer_storage_session_release(' \
    kernel/include/installer_storage_screen.h \
    kernel/src/installer_storage_screen.c || {
    echo "ITEM-501: второй обязательный экран не объявлен или не написан" >&2
    exit 1
}
grep -Fq 'installer_storage_session_release(' kernel/src/kernel.c || {
    echo "ITEM-501: загрузочный цикл не открывает экран разделов" >&2
    exit 1
}
grep -Fq 'production_install_flow_release_partition(' kernel/src/kernel.c || {
    echo "ITEM-501: выбор оператора не доходит до потока установки" >&2
    exit 1
}

# ------------------------------------------------ the release planner -------
grep -Fq 'installer_dual_boot_plan_release(' \
    kernel/include/installer_dual_boot.h \
    kernel/src/installer_dual_boot.c || {
    echo "ITEM-501: планировщик «отдать раздел» не объявлен" >&2
    exit 1
}
grep -Fq 'INSTALLER_DUAL_BOOT_PARTITION_UNSUPPORTED' \
    kernel/include/installer_dual_boot.h \
    kernel/src/installer_dual_boot.c || {
    echo "ITEM-501: у отказа «нельзя отдать этот раздел» нет статуса" >&2
    exit 1
}
grep -Fq 'releases_partition' kernel/include/installer_dual_boot.h || {
    echo "ITEM-501: план не несёт признак отданного раздела" >&2
    exit 1
}

# --------------------------------------------- the contract's release mode ---
grep -Fq 'INSTALLER_LAYOUT_CONTRACT_MODE_RELEASE' \
    kernel/include/installer_layout_contract.h \
    kernel/src/installer_layout_contract.c || {
    echo "ITEM-501: у контракта нет режима release" >&2
    exit 1
}
grep -Fq 'installer_layout_contract_plan_release(' \
    kernel/include/installer_layout_contract.h \
    kernel/src/installer_layout_contract.c || {
    echo "ITEM-501: контракт не планирует release" >&2
    exit 1
}
grep -Fq 'available_lba' kernel/include/installer_layout_contract.h || {
    echo "ITEM-501: сводка не несёт «сколько есть»" >&2
    exit 1
}
grep -Fq 'required_lba' kernel/include/installer_layout_contract.h || {
    echo "ITEM-501: сводка не несёт «сколько нужно»" >&2
    exit 1
}
grep -Fq 'released_index=' kernel/src/installer_layout_contract.c || {
    echo "ITEM-501: запись контракта не называет отданный раздел" >&2
    exit 1
}
grep -Fq 'available_mib=' kernel/src/installer_layout_contract.c || {
    echo "ITEM-501: запись контракта не несёт числа" >&2
    exit 1
}

# ------------------------------------- the writer removes exactly one entry ---
grep -Fq 'INSTALLER_GPT_PRESERVE_SLOT_RELEASED' \
    kernel/include/installer_gpt_preserve.h \
    kernel/src/installer_gpt_preserve.c || {
    echo "ITEM-501: у писателя нет удаляемой записи" >&2
    exit 1
}
grep -Fq 'released_index' kernel/include/installer_gpt_preserve.h || {
    echo "ITEM-501: писатель не помнит слот, который удалил" >&2
    exit 1
}
grep -Fq 'memset(target, 0, INSTALLER_GPT_PRESERVE_ENTRY_SIZE);' \
    kernel/src/installer_gpt_preserve.c || {
    echo "ITEM-501: писатель не обнуляет отданную запись" >&2
    exit 1
}
grep -Fq '? "release"' kernel/src/installer_gpt_preserve.c || {
    echo "ITEM-501: запись сохранения не называет режим release" >&2
    exit 1
}

# ------------------------------------------------------------- the flow ------
grep -Fq 'release_chosen' kernel/src/production_install_flow.c || {
    echo "ITEM-501: поток не помнит отданный раздел" >&2
    exit 1
}
grep -Fq 'operator-release' kernel/src/production_install_flow.c || {
    echo "ITEM-501: решение оператора не называется в записи" >&2
    exit 1
}
grep -Fq 'PRODUCTION_INSTALL_MEDIA_REFUSAL_RELEASE_CONFLICT' \
    kernel/include/production_install_media.h \
    kernel/src/production_install_media.c || {
    echo "ITEM-501: конфликт «отдать раздел» и whole-disk не назван" >&2
    exit 1
}
grep -Fq 'release_partition_index' kernel/include/production_installer.h || {
    echo "ITEM-501: контроллер не принимает отданный раздел" >&2
    exit 1
}
grep -Fq 'installer_layout_contract_plan_release(' \
    kernel/src/production_installer.c || {
    echo "ITEM-501: контроллер не планирует release" >&2
    exit 1
}

# ------------------------------------------------------- the host proofs -----
for item501_host in \
    'a named LVM partition can be given up' \
    'the release plan names the partition and keeps the rest' \
    'the EFI System Partition is refused as a release target' \
    'an extended container is refused as a release target' \
    'a partition smaller than the three roles is refused'; do
    grep -Fq "$item501_host" tests/installer-dual-boot-host.c || {
        echo "ITEM-501: host-проба не утверждает: $item501_host" >&2
        exit 1
    }
done
for item501_contract in \
    'the named partition is given up and the roles fit inside it' \
    'the summary carries the room available and the room needed' \
    'the record names the released partition and the two numbers' \
    'the EFI System Partition cannot be released' \
    'a partition too small for the roles is refused'; do
    grep -Fq "$item501_contract" tests/installer-layout-contract-host.c || {
        echo "ITEM-501: host-проба контракта не утверждает: $item501_contract" >&2
        exit 1
    }
done
for item501_writer in \
    'the release plan fits in the extent it gives up' \
    'the given-up entry is zero bytes after the write' \
    'every entry that was not given up stays byte-identical' \
    'the record names the release and its slot' \
    'a released extent that is not on the disk is refused' \
    'a role outside the given-up extent is refused'; do
    grep -Fq "$item501_writer" tests/installer-gpt-preserve-host.c || {
        echo "ITEM-501: host-проба писателя не утверждает: $item501_writer" >&2
        exit 1
    }
done
for item501_model in \
    'the ESP and the container are not offered as release targets' \
    'the free-space number is the largest region the disk still has' \
    'the warning names what dies and what stays' \
    'the reading names the partition and says nothing was written'; do
    grep -Fq "$item501_model" tests/installer-storage-host.c || {
        echo "ITEM-501: host-проба экрана не утверждает: $item501_model" >&2
        exit 1
    }
done

# --------------------------------------------------------- the QEMU proof ----
for item501_qemu in \
    'run_release()' \
    'assert_release()' \
    'WARNING: THE DATA ON THIS PARTITION WILL BE DELETED' \
    'the operator gave up partition 2 on the' \
    'only slot 1 (the given-up partition) changed' \
    'the released extent carries the three' \
    'the disk with one given-up partition boots the'; do
    grep -Fq "$item501_qemu" \
        scripts/test-qemu-production-install-flow-preserve.sh || {
        echo "ITEM-501: QEMU-проба не проверяет: $item501_qemu" >&2
        exit 1
    }
done
grep -Fq 'CONSOLE_IMAGE=' scripts/test-qemu-production-install-flow-preserve.sh || {
    echo "ITEM-501: QEMU-проба не получает операторскую медиа" >&2
    exit 1
}
grep -Fq 'production-installer-console-media' Makefile || {
    echo "ITEM-501: цель QEMU-пробы не собирает операторскую медиа" >&2
    exit 1
}

# -------------------------------------------------------------- documents ----
grep -Fq 'give up the selected' docs/hfoc-dual-boot.md || {
    echo "ITEM-501: документ двойной загрузки не описывает отказ от раздела" >&2
    exit 1
}
grep -Fq 'Give up the selected partition' docs/hfoc-dual-boot-guide.md || {
    echo "ITEM-501: руководство не описывает отказ от раздела" >&2
    exit 1
}
grep -Fq 'LVM' docs/hfoc-dual-boot-guide.md || {
    echo "ITEM-501: руководство не называет реальность LVM" >&2
    exit 1
}
grep -Fq 'gparted' docs/hfoc-dual-boot-guide.md || {
    echo "ITEM-501: руководство не описывает сжатие из Fedora" >&2
    exit 1
}
