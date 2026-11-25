#!/usr/bin/env bash
# ITEM-540: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with the shellcheck tool.
#
# The L875 finding: the common PCI path read every present type-0 device with
# the combined identity-and-BARs call, so a class scan wrote BAR probe values
# to devices it was not looking for and returned the first resource error it
# met - an error on any unrelated device aborted the whole search and the
# machine reported no SATA disk and no xHCI controller at once.
#
# This slice pins the fix:
#   * identity/class and resource preparation are separate calls, and
#     pci_scan_class_ex() filters by class before it decodes any BAR;
#   * I/O BARs are read per the specification (bit 1 reserved, bit 2 part of
#     the base, one-doubleword minimum and size alignment), while the
#     memory minimum, overflow and 64-bit checks stay;
#   * the storage probe records its own state and the primary error, the
#     observed-storage report prints `state=PROBE_FAILED stage=... status=...`
#     with the failing device instead of folding it into `state=NOT_PROBED`,
#     and the machine record carries the same fields;
#   * the host proofs keep the RED fixtures and the named scan failure.
#
# The pins below are literal source text, so `$(...)` must not expand and a
# trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item540_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item540_root" || exit 1

item540_files=(
    kernel/include/pci.h
    kernel/src/pci.c
    kernel/include/appliance_production_storage.h
    kernel/src/appliance_production_storage.c
    kernel/include/appliance_storage_report.h
    kernel/src/appliance_storage_report.c
    kernel/src/appliance_language.c
    tests/pci-host.c
    tests/installer-storage-host.c
)
for item540_file in "${item540_files[@]}"; do
    [[ -f $item540_file ]] || {
        echo "ITEM-540: отсутствует $item540_file" >&2
        exit 1
    }
done

# 1. The common path publishes identity, resources and a named scan failure as
#    three separate things.
for item540_pin in \
    'enum pci_status pci_device_identity(' \
    'enum pci_status pci_device_resources_read(' \
    'enum pci_status pci_scan_class_ex(' \
    'struct pci_scan_failure {' \
    '#define PCI_BAR_IO_ALIGNMENT 4U' \
    '#define PCI_BAR_INDEX_NONE 0xFFU'; do
    grep -Fq -- "$item540_pin" kernel/include/pci.h || {
        echo "ITEM-540: pci.h не несёт '$item540_pin'" >&2
        exit 1
    }
done

# 2. The scan filters on identity before it probes, and a probe failure names
#    the device it belongs to.
for item540_pin in \
    'status = pci_device_identity(io, &location, &header);' \
    'status = pci_device_decode_bars(' \
    'failure->vendor_id = header.vendor_id;' \
    'failure->bar_index = failed_bar;' \
    'return pci_scan_class_ex(io, base_class, subclass, prog_if,'; do
    grep -Fq -- "$item540_pin" kernel/src/pci.c || {
        echo "ITEM-540: pci.c не несёт '$item540_pin'" >&2
        exit 1
    }
done
# The scan must not go back to reading a whole device before it knows whether
# the class is the one it was asked for.
if grep -Fq -- 'pci_device_read(io, &location, &header)' kernel/src/pci.c; then
    echo "ITEM-540: скан снова зондирует каждое устройство целиком" >&2
    exit 1
fi

# 3. I/O BAR decoding follows the specification; the memory minimum and the
#    bound/overflow refusals stay.
for item540_pin in \
    'if ((raw0 & 2U) != 0U) {' \
    'io_space ? PCI_BAR_IO_ALIGNMENT : PCI_BAR_ALIGNMENT' \
    'base % length != 0U' \
    'if (base > UINT64_MAX - length) {' \
    'if (!is_64bit && !io_space &&' \
    'return PCI_INVALID_BAR;'; do
    grep -Fq -- "$item540_pin" kernel/src/pci.c || {
        echo "ITEM-540: pci.c потерял проверку '$item540_pin'" >&2
        exit 1
    }
done

# 4. The storage probe keeps the primary error instead of one NOT_PROBED.
for item540_pin in \
    'bool probe_attempted;' \
    'enum pci_status probe_status;' \
    'struct pci_scan_failure probe_failure;'; do
    grep -Fq -- "$item540_pin" kernel/include/appliance_production_storage.h || {
        echo "ITEM-540: свидетельство пробы не несёт '$item540_pin'" >&2
        exit 1
    }
done
# ITEM-543 moved the storage probe to pci_scan_class_required(), which names
# the BARs it needs; the probe still publishes the same named failure here.
for item540_pin in \
    'static void production_storage_note_probe_failure(' \
    'pci_status = pci_scan_class_required(' \
    'production_storage.probe_stage = "AHCI_SCAN";' \
    'production_storage.probe_stage = "NVME_SCAN";' \
    'production_storage.probe_stage = "READY";' \
    'out->probe_attempted = production_storage.probe_attempted;' \
    'out->probe_status = pci_status_string(evidence.probe_status);' \
    'out->failure_known = evidence.probe_failure.status != PCI_OK;'; do
    grep -Fq -- "$item540_pin" kernel/src/appliance_production_storage.c || {
        echo "ITEM-540: проба не несёт '$item540_pin'" >&2
        exit 1
    }
done

# 5. The report renders the failed search and the machine record carries it.
grep -Fq -- '#define APPLIANCE_STORAGE_REPORT_BAR_NONE 0xFFU' \
    kernel/include/appliance_storage_report.h || {
    echo "ITEM-540: отчёт не объявляет BAR_NONE" >&2
    exit 1
}
for item540_pin in \
    'state=PROBE_FAILED stage=' \
    'failed at bdf=' \
    'probe_attempted=' \
    'failed_bdf='; do
    grep -Fq -- "$item540_pin" kernel/src/appliance_storage_report.c || {
        echo "ITEM-540: отчёт не несёт '$item540_pin'" >&2
        exit 1
    }
done
if [[ $(grep -Fc -- 'state=NOT_PROBED' kernel/src/appliance_storage_report.c) != 1 ]]; then
    echo "ITEM-540: state=NOT_PROBED больше не один (слияние вернулось)" >&2
    exit 1
fi
for item540_pin in \
    '"storage.report.probe_failed", "storage probe failed",' \
    '"опрос хранилища завершился ошибкой"'; do
    grep -Fq -- "$item540_pin" kernel/src/appliance_language.c || {
        echo "ITEM-540: каталог не несёт '$item540_pin'" >&2
        exit 1
    }
done

# 6. The host proofs keep the RED fixtures and the named failure.
for item540_case in \
    test_io_bar_small_ranges_decode \
    test_scan_survives_unrelated_device_resource \
    test_scan_probes_only_matched_devices \
    test_identity_read_skips_resource_probe \
    test_scan_failure_names_the_device; do
    grep -Fq -- "$item540_case" tests/pci-host.c || {
        echo "ITEM-540: tests/pci-host.c потерял случай $item540_case" >&2
        exit 1
    }
done
for item540_pin in \
    '4-byte I/O BAR decodes to its own size and alignment' \
    "an unrelated device's malformed BAR does not abort the scan" \
    'a scan performs no BAR probe writes on an unmatched device' \
    'a failed scan names the device, its identity and its BAR'; do
    grep -Fq -- "$item540_pin" tests/pci-host.c || {
        echo "ITEM-540: свидетель PCI потерял утверждение '$item540_pin'" >&2
        exit 1
    }
done
for item540_pin in \
    'test_the_observed_report_names_a_failed_probe' \
    'state=PROBE_FAILED stage=NVME_SCAN' \
    'failed at bdf=02:1f.1 vendor=8086 ' \
    'device=1e03 bar=5' \
    'probe_attempted=1 probe_stage=NVME_SCAN' \
    'failed_bdf=02:1f.1 vendor=8086 '; do
    grep -Fq -- "$item540_pin" tests/installer-storage-host.c || {
        echo "ITEM-540: свидетель отчёта потерял '$item540_pin'" >&2
        exit 1
    }
done

# 7. This item is history now that the later items own the current section; the
#    document has to keep recording it, and the roadmap records the item.
item540_queue=docs/superpowers/plans/2026-09-11-appliance-productization-queue.md
grep -Fq '## Item ITEM-540' "$item540_queue" || {
    echo "ITEM-540: очередь потеряла исторический раздел" >&2
    exit 1
}
grep -Fq '## Item ITEM-539' "$item540_queue" || {
    echo "ITEM-540: очередь потеряла исторический раздел ITEM-539" >&2
    exit 1
}
grep -Fq 'ITEM-540' docs/ROADMAP.md || {
    echo "ITEM-540: docs/ROADMAP.md не называет пункт" >&2
    exit 1
}
