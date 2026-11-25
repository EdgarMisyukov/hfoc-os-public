#!/usr/bin/env bash
# ITEM-543: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with the shellcheck tool.
#
# The L875 finding F-8 (2026-10-16): the machine's Intel 7-series AHCI
# controller (8086:1e03) refuses with
#
#   storage probe failed state=PROBE_FAILED stage=AHCI_SCAN status=PCI_INVALID_BAR
#   failed at bdf=00:1f.2 vendor=8086 device=1e03 bar=0
#
# and the installer then reports NO_DISK.  The controller is not broken: its
# ABAR at BAR5 is an ordinary memory BAR and Linux reaches the disk through
# it.  BAR0 is a legacy I/O BAR whose size probe does not explain a range (a
# 16-bit I/O decode answers 0x0000FFF9, and 0xFFFF0008 is not a power of two),
# and one BAR the driver never touches was sinking the whole matched device
# before AHCI could look at the ABAR.
#
# This slice pins the fix:
#   * a BAR whose size probe explains no range is reported unusable on that
#     BAR (`present == false`, `decode_status` says why) and no longer fails
#     the resource read; a structurally malformed register (reserved bits or
#     type, I/O BAR5, a misaligned base, an overflowing range) and a failed
#     configuration access still do;
#   * pci_scan_class_required() names the BARs a caller needs; a matched
#     device whose needed BAR did not decode is the named refusal of that
#     device with the BAR index and the decode status, and
#     pci_scan_class_ex() keeps the older "every BAR" contract for callers
#     that do not choose;
#   * the storage probe asks for the AHCI ABAR (BAR5) and the NVMe BAR (BAR0),
#     so the L875's legacy BAR0 no longer hides the controller and a needed
#     BAR that does not decode still reaches the page as `status=... bar=...`;
#   * the host proof fixes the three probe shapes that explain no size and
#     keeps the malformed-register refusal.
#
# The pins below are literal source text, so `$(...)` must not expand and a
# trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item543_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item543_root" || exit 1

item543_files=(
    kernel/include/pci.h
    kernel/src/pci.c
    kernel/src/appliance_production_storage.c
    tests/pci-host.c
    docs/ROADMAP.md
    docs/hfoc-installer-storage.md
)
for item543_file in "${item543_files[@]}"; do
    [[ -f $item543_file ]] || {
        echo "ITEM-543: отсутствует $item543_file" >&2
        exit 1
    }
done

# 1. The header names the required-BAR mask and records why a BAR is unusable.
for item543_pin in \
    '#define PCI_BAR_REQUIRED_NONE 0U' \
    '#define PCI_BAR_REQUIRED(bar_index) (1U << (bar_index))' \
    '#define PCI_BAR_REQUIRED_ALL ((1U << PCI_BAR_COUNT) - 1U)' \
    'enum pci_status decode_status;' \
    'enum pci_status pci_scan_class_required('; do
    grep -Fq -- "$item543_pin" kernel/include/pci.h || {
        echo "ITEM-543: pci.h не несёт '$item543_pin'" >&2
        exit 1
    }
done

# 2. An unexplainable size is a BAR that cannot be used, not a dead device; the
#    required-BAR check keeps the named refusal.
for item543_pin in \
    'bar->decode_status = PCI_INVALID_BAR;' \
    'static enum pci_status pci_required_bars_status(' \
    'status = pci_required_bars_status(' \
    'return pci_scan_class_required(io, base_class, subclass, prog_if,'; do
    grep -Fq -- "$item543_pin" kernel/src/pci.c || {
        echo "ITEM-543: pci.c не несёт '$item543_pin'" >&2
        exit 1
    }
done
# A structurally malformed register is never traded for a green test: the
# reserved-bit, reserved-type and I/O-BAR5 refusals stay.
item543_hard_refusals=$(grep -Fc -- 'return PCI_INVALID_BAR;' kernel/src/pci.c)
[[ $item543_hard_refusals -ge 3 ]] || {
    echo "ITEM-543: pci.c оставил $item543_hard_refusals жёстких отказов BAR" >&2
    exit 1
}
for item543_pin in \
    'if ((raw0 & 2U) != 0U) {' \
    'return PCI_BAD_ALIGNMENT;' \
    'return PCI_OVERFLOW;'; do
    grep -Fq -- "$item543_pin" kernel/src/pci.c || {
        echo "ITEM-543: pci.c потерял проверку '$item543_pin'" >&2
        exit 1
    }
done

# 3. The storage probe asks for the BAR it is brought up through.
for item543_pin in \
    'PCI_BAR_REQUIRED(AHCI_MEMORY_BAR_INDEX)' \
    'PCI_BAR_REQUIRED(NVME_MEMORY_BAR_INDEX)' \
    'pci_scan_class_required('; do
    grep -Fq -- "$item543_pin" kernel/src/appliance_production_storage.c || {
        echo "ITEM-543: проба хранилища не несёт '$item543_pin'" >&2
        exit 1
    }
done

# 4. The host proof fixes the shapes that explain no size and the refusals
#    that stay.
for item543_case in \
    test_unexplained_bar_size_leaves_the_device_usable \
    test_required_bar_reaches_the_scan_and_names_its_own_failure; do
    grep -Fq -- "$item543_case" tests/pci-host.c || {
        echo "ITEM-543: tests/pci-host.c потерял случай $item543_case" >&2
        exit 1
    }
done
for item543_pin in \
    'the L875 legacy I/O BAR whose 16-bit probe names no range' \
    'a legacy I/O BAR whose size probe reads back zero' \
    'a read-only legacy I/O BAR answers with its own base' \
    'header.bars[0].decode_status == PCI_INVALID_BAR' \
    'a caller that requires every BAR still gets the named refusal' \
    'a needed BAR that explains no size is a named refusal of that device' \
    'the upper half of an unusable 64-bit BAR is not probed alone' \
    'a malformed register is still a hard refusal on any BAR'; do
    grep -Fq -- "$item543_pin" tests/pci-host.c || {
        echo "ITEM-543: свидетель PCI потерял утверждение '$item543_pin'" >&2
        exit 1
    }
done

# 5. The storage document states the rule the page now follows.
for item543_doc in 'ITEM-543' 'PCI_INVALID_BAR' 'BAR0' 'BAR5'; do
    grep -Fq -- "$item543_doc" docs/hfoc-installer-storage.md || {
        echo "docs/hfoc-installer-storage.md не фиксирует ITEM-543:" \
            "$item543_doc" >&2
        exit 1
    }
done

# 6. The queue keeps this item as history, the roadmap records it.  ITEM-544
#    owns the current-item pin now.
item543_queue=docs/superpowers/plans/2026-09-11-appliance-productization-queue.md
grep -Fq '## Item ITEM-543' "$item543_queue" || {
    echo "ITEM-543: очередь потеряла исторический раздел ITEM-543" >&2
    exit 1
}
grep -Fq '## Item ITEM-542' "$item543_queue" || {
    echo "ITEM-543: очередь потеряла исторический раздел ITEM-542" >&2
    exit 1
}
grep -Fq 'ITEM-543' docs/ROADMAP.md || {
    echo "ITEM-543: docs/ROADMAP.md не называет пункт" >&2
    exit 1
}
