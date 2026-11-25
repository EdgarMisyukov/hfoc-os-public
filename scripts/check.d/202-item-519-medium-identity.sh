#!/usr/bin/env bash
# ITEM-519 increment 5: contracted slice of the HFOC OS check suite, sourced
#           in pinned order by scripts/check.sh and lintable on its own with
#           the shellcheck tool.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item519m_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item519m_root" || exit 1

# ITEM-519 increment 5: the boot-medium guard.
#   * the loader's own identity for the file it loaded (media type, partition
#     index, MBR disk id) is read once, in kernel.c, where the loader request
#     lives;
#   * the pure rule compares it with LBA 0 of a candidate device and refuses
#     every shape that is not that medium - another disk id, no table, no
#     loader id, an unusable partition entry, an extent past the device, the
#     protective GPT MBR - each with its own reason;
#   * xhci probes the bound stick through its block view at bind time, publishes
#     `[INSTALL-MEDIUM]`, and the console can ask again;
#   * the QEMU proof is two-sided: the stick attached to a CD boot must never be
#     called MATCH, and the stick the machine booted from must match with the
#     whole device as its scope.

# 1. The files the increment touches exist.
for item519m_file in \
    kernel/include/install_medium.h \
    kernel/src/install_medium.c \
    kernel/src/xhci.c \
    kernel/src/kernel.c \
    kernel/src/console.c \
    tests/install-medium-host.c \
    scripts/test-qemu-usb-two-controllers.sh \
    docs/ROADMAP.md; do
    [[ -f $item519m_file ]] || {
        echo "ITEM-519: отсутствует $item519m_file" >&2
        exit 1
    }
done

# 2. The pure rule and its named refusals.
for item519m_pin in \
    'INSTALL_MEDIUM_MBR_SIGNATURE_OFFSET 510U' \
    'INSTALL_MEDIUM_MBR_DISK_ID_OFFSET 440U' \
    'INSTALL_MEDIUM_MBR_TYPE_PROTECTIVE 0xEEU' \
    'struct install_medium_verdict install_medium_identify(' \
    'install_medium_result_string(' \
    'INSTALL_MEDIUM_NO_MATCH' \
    'INSTALL_MEDIUM_NO_TABLE' \
    'INSTALL_MEDIUM_NO_IDENTITY' \
    'INSTALL_MEDIUM_MALFORMED' \
    'INSTALL_MEDIUM_PROTECTIVE_MBR'; do
    grep -Fq "$item519m_pin" kernel/src/install_medium.c kernel/include/install_medium.h || {
        echo "ITEM-519: модуль идентичности не содержит: $item519m_pin" >&2
        exit 1
    }
done
grep -Fq 'verdict.disk_id = install_medium_le32(' kernel/src/install_medium.c || {
    echo "ITEM-519: вердикт не читает id самого устройства" >&2
    exit 1
}
grep -Fq 'verdict.disk_id != boot->mbr_disk_id' kernel/src/install_medium.c || {
    echo "ITEM-519: вердикт не сравнивает id загрузчика с id устройства" >&2
    exit 1
}

# 3. The loader identity is read where the loader request lives.
grep -Fq 'bool install_medium_boot_identity(struct install_medium_boot_identity *out)' \
    kernel/src/kernel.c || {
    echo "ITEM-519: ядро не публикует идентичность загрузчика" >&2
    exit 1
}
grep -Fq 'out->mbr_disk_id_valid = executable->mbr_disk_id != 0U;' \
    kernel/src/kernel.c || {
    echo "ITEM-519: идентичность не называет нулевой id отсутствующим" >&2
    exit 1
}

# 4. The driver probes through the block view, publishes the verdict and never
#    hardcodes MATCH; the session is not required, because the walk publishes
#    before it records the session.
for item519m_xhci in \
    '[INSTALL-MEDIUM]' \
    'xhci_storage_medium_publish(' \
    'xhci_storage_medium_probe(' \
    'install_medium_identify(&boot, sector,' \
    'install_medium_result_string(verdict.result)' \
    'xhci_storage_medium_probe(storage)' \
    'XHCI_PUBLISH(" scope=")'; do
    grep -Fq "$item519m_xhci" kernel/src/xhci.c || {
        echo "ITEM-519: драйвер не содержит: $item519m_xhci" >&2
        exit 1
    }
done
if grep -Fq 'state=MATCH' kernel/src/xhci.c; then
    echo "ITEM-519: драйвер публикует MATCH литералом, а не вердиктом" >&2
    exit 1
fi

# 5. The console command and the header declaration.
grep -Fq 'storage medium' kernel/src/console.c || {
    echo "ITEM-519: нет команды storage medium" >&2
    exit 1
}
grep -Fq 'STORAGE MEDIUM:' kernel/src/console.c || {
    echo "ITEM-519: команда не печатает вердикт" >&2
    exit 1
}
grep -Fq 'bool xhci_storage_medium_publish(void);' kernel/include/xhci.h || {
    echo "ITEM-519: xhci.h не объявляет пробу носителя" >&2
    exit 1
}
grep -Fq 'const char *xhci_storage_medium_result(void);' kernel/include/xhci.h || {
    echo "ITEM-519: xhci.h не объявляет имя вердикта" >&2
    exit 1
}

# 6. The host proof drives the rule's shapes and is registered in the suite.
for item519m_host in \
    'test_match_names_the_partition();' \
    'test_a_loader_without_a_partition_names_the_whole_device();' \
    'test_another_disk_is_refused();' \
    'test_no_loader_identity_is_refused();' \
    'test_a_sector_without_a_table_is_refused();' \
    'test_a_bad_partition_entry_is_refused();' \
    'test_an_extent_past_the_device_is_refused();' \
    'test_the_protective_mbr_is_not_a_layout();'; do
    grep -Fq "$item519m_host" tests/install-medium-host.c || {
        echo "ITEM-519: host-проба не проверяет: $item519m_host" >&2
        exit 1
    }
done
grep -Fq 'test-install-medium-host' Makefile || {
    echo "ITEM-519: host-проба не зарегистрирована" >&2
    exit 1
}

# 7. The QEMU proof is two-sided.
grep -Fq 'the attached stick was claimed as the boot medium' \
    scripts/test-qemu-usb-two-controllers.sh || {
    echo "ITEM-519: QEMU-проба не запрещает ложный MATCH" >&2
    exit 1
}
grep -Fq 'state=MATCH' scripts/test-qemu-usb-two-controllers.sh || {
    echo "ITEM-519: QEMU-проба не требует положительный MATCH" >&2
    exit 1
}
grep -Fq 'scope=WHOLE' scripts/test-qemu-usb-two-controllers.sh || {
    echo "ITEM-519: QEMU-проба не требует целый носитель" >&2
    exit 1
}

# 8. The queue says which item it is on, and the roadmap records the increment.
#    ITEM-521 moved the queue's `## Current item` heading on to its own item,
#    so this pin reads ITEM-519's own historical section.
grep -A 3 '^## Item ITEM-519$' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md |
    grep -Fq 'ITEM-519' || {
    echo "ITEM-519: текущий раздел очереди не называет ITEM-519" >&2
    exit 1
}
grep -Fq '[INSTALL-MEDIUM]' docs/ROADMAP.md || {
    echo "ITEM-519: docs/ROADMAP.md не записывает идентичность носителя" >&2
    exit 1
}
