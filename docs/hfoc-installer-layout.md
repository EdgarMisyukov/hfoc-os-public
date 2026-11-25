# HFOC Clean Installer Layout

**Date:** 2026-09-06
**Target:** ITEM-340 of
`docs/superpowers/plans/2026-09-06-hfoc-appliance-installer-and-management-ui.md`
**Base SHA:** `37db744438a8a0fd703cae07555cea64a235af0b`
**Prerequisite boundaries:** ITEM-338 installer architecture and
storage-safety audit; ITEM-339 read-only hardware and existing-OS discovery
**Status:** Completed by ITEM-340

## Purpose

This document is the authoritative ITEM-340 clean installer layout
boundary. ITEM-340 creates bounded whole-disk and manual HFOC installation
layout plans, publishes a destructive-operation preview, requires explicit
confirmation before any write, and executes partition-table mutation over a
generic 512-byte `block_device` with a pre-mutation snapshot and rollback.

Existing-OS dual-boot, boot-menu generation, UEFI NVRAM mutation, and
bootloader recovery remain ITEM-341 and ITEM-342 work. ITEM-340 does not
install or mutate an existing operating system.

## Module

`installer_layout` lives under `kernel/` and publishes:

- whole-disk clean GPT layout planning;
- manual clean GPT layout planning from aligned free ranges;
- bounded destructive and non-destructive previews;
- explicit confirmation state;
- checksummed GPT header/partition-array writes;
- pre-mutation sector snapshots and rollback;
- exact status strings.

The module accepts an ITEM-339 finalized discovery report, finds a selected
512-byte disk, and never writes before `installer_layout_confirm()`.

## HFOC Clean GPT Layout

Whole-disk planning places up to four roles in a deterministic order:

1. EFI System Partition.
2. HFOC system partition.
3. HFOC persistence partition.
4. HFOC recovery partition.

The layout writes a protective MBR, primary GPT header and partition array,
and backup GPT header/partition array at the end of the selected disk. It
uses bounded partition capacities and rejects layouts that would exceed the
GPT usable region.

Manual planning accepts aligned caller-owned free ranges for the same four
roles. It rejects overlapping, out-of-range, duplicated, or unsafe ranges.

## Destructive Preview And Confirmation

The plan records the selected disk identity, firmware mode, existing
partition count, destructive flag, mode, and final partition ranges. A
whole-disk plan is always marked destructive because a missing partition
table does not prove that a disk contains no data. The preview publishes
that information as bounded deterministic text.

`installer_layout_confirm()` marks a plan confirmed. Every execution path
rejects an unconfirmed plan and performs no block writes. A whole-disk plan
is always marked destructive and preserves the existing partition count in
the preview.

## Transaction And Rollback

`installer_layout_execute()` snapshots all sectors it will mutate before the
first write. Writes are applied in staged order. If any write fails, the
module restores the snapshot sectors. `installer_layout_rollback()` exposes
the same snapshot for recovery paths.

The snapshot is caller-owned in this item. A real recovery-media region and
EFI bootloader transaction remain ITEM-342 work.

## Host Proof

`make test-installer-layout-host` runs in-memory fixtures that:

- plan whole-disk and manual HFOC layouts without writes;
- verify an unconfirmed layout performs no block writes;
- execute confirmed layouts;
- re-scan the written GPT with ITEM-339 discovery;
- inject a write failure and verify original sectors are restored;
- reject overlapping manual ranges;
- mark every whole-disk plan destructive.

This is a host proof, not physical-hardware acceptance.

## Boundaries

- ITEM-338 owns the installer architecture audit.
- ITEM-339 owns read-only hardware and existing-OS discovery.
- ITEM-340 (this item) owns clean whole-disk/manual layout and transaction
  semantics.
- ITEM-341 owns dual-boot installation and boot-menu generation.
- ITEM-342 owns bootloader and UEFI-entry recovery.
- ITEM-346 owns integrated installer acceptance.

## Non-Goals

- No existing-OS dual-boot installation or preservation.
- No boot-menu or UEFI NVRAM mutation.
- No bootloader installation or recovery.
- No general-purpose writable filesystem.
- No real disk-controller driver.
- No physical-hardware acceptance.
- No VERSION or release-number change.

## Production Install Layout Contract (ITEM-391)

Verified on 2026-09-12: everything above is a *validator*. The module accepts
caller-supplied `requests[]` (role plus `length_lba`) and checks overlap,
alignment and the GPT usable region; `production_installer` executes a plan it
is handed. The only fixed numbers in the tree were the QEMU fixture's
2048-sector test shape, so the installed footprint of the appliance was
whatever the caller passed in, and nobody could answer "how much space does the
appliance take, and how much is left for the previous OS in a dual-boot
install" from the product.

ITEM-391 closes that with `kernel/include/installer_layout_contract.h`, which
declares the four role sizes as product constants, refuses a target smaller
than the declared layout instead of truncating it, derives the plan from the
constants through the existing planner, and publishes the numbers. The role
capacity grew from four to five with ITEM-517's BIOS roles
(`INSTALLER_LAYOUT_PARTITION_CAPACITY 5U`); the planner's validation and
execution semantics are otherwise unchanged, and the contract module adds no
second planner. A UEFI plan still fills exactly four roles
(`INSTALLER_LAYOUT_CONTRACT_CLEAN_ROLE_COUNT`).

### Declared sizes

| Role | Sectors (512 B) | Size | What the number has to hold |
| --- | --- | --- | --- |
| EFI System Partition | 67584 | 33 MiB | the installed boot payload set: the Limine loader on the removable-media path, the boot configuration, the sealed boot manifest and the kernel itself. ITEM-397 moved this role from 4096 sectors (2 MiB), which could not hold the kernel (about 8.4 MiB of ELF at this revision, most of it DWARF). The set is bounded by `EFI_FAT32_ESP_PAYLOAD_SET_BYTES_MAX` (16 MiB) with `EFI_FAT32_ESP_FILE_MAX` (16 MiB) per file, and the production ESP formatter now derives its FAT size from the volume under `EFI_FAT32_ESP_FAT_SECTORS_MAX` instead of writing a fixed 32-sector FAT. The role is one `INSTALLER_LAYOUT_CONTRACT_SYSTEM_SLOT_LBA`, so the ESP grows in the same unit as the system image it carries, and it leaves about 15 MiB of FAT32, directory and growth margin over the bounded set; the bounds were 8 MiB/10 MiB until the 2026-09-27 kernel crossed the per-file one and every install was refused with `PRODUCTION_INSTALLER_SOURCE_UNAVAILABLE`. |
| HFOC system | 131072 | 64 MiB | the signed system image with its growth margin: four 16 MiB slots (`INSTALLER_LAYOUT_CONTRACT_SYSTEM_SLOT_LBA`) for the A/B update pair, the staged boot payload and headroom. The production installer kernel image measures 735083 bytes of text, rodata, data and Limine requests at this revision (about 0.7 MiB), so a slot is more than 20x today's image. |
| HFOC persistence | 65536 | 32 MiB | the state volume (`appliance_state_layout`, bounded stores and journals), the audit chain and the support bundle. It is twice the 16 MiB state partition the recovery-executor QEMU proof already drives. |
| HFOC recovery | 65536 | 32 MiB | one complete recovery image slot plus the same again for recovery metadata and margin, so a recovery boot never depends on the system role. |
| **Declared footprint** | **329728** | **161 MiB** | all four roles, `INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA` |

ITEM-397 then moved the EFI System Partition from 4096 to 67584 sectors: the
installed system boots from the ESP, so the ESP carries the bounded payload set
(the loader the firmware finds without a boot entry, the configuration, the
sealed boot manifest and the kernel) and the role has to hold it. The other
three roles are unchanged, and the declared footprint moves from 266240 to
294912 sectors with the move. Every pin that names the old numbers - this
document, the `[INSTALLER-LAYOUT]` record in the QEMU proof, the contract
host proof, the static check module and the kernel/controller records - moved
in the same commit as the constants.

Alignment and reserved space are part of the contract: every role size is a
whole multiple of the planner's 1 MiB alignment
(`INSTALLER_LAYOUT_CONTRACT_MIB_LBA`), the first partition starts at
`INSTALLER_LAYOUT_CONTRACT_FIRST_LBA` (the planner's 2048), and the plan leaves
`INSTALLER_LAYOUT_CONTRACT_GPT_TAIL_LBA` (34) sectors for the backup GPT.

### The legacy-BIOS layout (ITEM-517)

A machine without UEFI has no ESP to boot from, so its whole-disk install is a
second declared layout with its own roles and its own floor
(`installer_layout_contract_plan_clean_bios()`,
`INSTALLER_LAYOUT_CONTRACT_MODE_CLEAN_WHOLE_DISK_BIOS`):

| Role | Sectors (512 B) | Size | What the number has to hold |
| --- | --- | --- | --- |
| BIOS boot partition | 2048 | 1 MiB | Limine's stage2 (22,004 bytes). The measured minimum is 64 sectors (32 KiB); the declared role is one planner alignment unit so every following role start stays 1 MiB aligned. The type GUID is `Hah!IdontNeedEFI`. |
| FAT boot volume | 67584 | 33 MiB | the BIOS payload set: the generated menu, the sealed chain configuration, the kernel, the sealed boot manifest and `limine-bios.sys`. Same FAT32-geometry and payload-capacity reasons as the ESP; it is a plain FAT32 volume, not an ESP. |
| HFOC system | 131072 | 64 MiB | as in the UEFI layout |
| HFOC persistence | 65536 | 32 MiB | as in the UEFI layout |
| HFOC recovery | 65536 | 32 MiB | as in the UEFI layout |
| **Declared footprint** | **331776** | **162 MiB** | all five roles, `INSTALLER_LAYOUT_CONTRACT_BIOS_FOOTPRINT_LBA`; the floor is `INSTALLER_LAYOUT_CONTRACT_BIOS_MINIMUM_MEDIA_LBA` = 2048 + 331776 + 34 = **333858 sectors (163 MiB at the MiB-truncating record)**. |

Role values 1–4 keep their numbers (the role byte is part of the GPT unique
GUID); `INSTALLER_LAYOUT_ROLE_BIOS_BOOT = 5` and
`INSTALLER_LAYOUT_ROLE_BOOT_VOLUME = 6` are appended. The whole-disk BIOS plan
places the roles in the order of the table and contains no ESP. A BIOS
**preserving** install is refused by name
(`PRODUCTION_INSTALLER_BIOS_PRESERVE_UNSUPPORTED`), because the preserving
planner reuses the previous system's ESP.

### Minimum media

`INSTALLER_LAYOUT_CONTRACT_MINIMUM_MEDIA_LBA` is the exact smallest target the
declared clean layout fits on: first usable LBA + declared footprint + GPT tail
reserve = 2048 + 329728 + 34 = **331810 sectors (>= 162 MiB)**. A disk one
sector below it is refused with
`INSTALLER_LAYOUT_CONTRACT_MEDIA_TOO_SMALL` (surfaced by the controller as
`PRODUCTION_INSTALLER_MEDIA_TOO_SMALL`) before any plan exists, so an
undersized appliance disk can never be silently truncated.

### Dual-boot reservation

A dual-boot install does not consume the whole disk and does not create a
second ESP:

- the existing EFI System Partition is reused and the existing partitions are
  preserved (the ITEM-341 `installer_dual_boot` planner owns that decision);
- the three HFOC roles are sized from the same product constants through
  `installer_layout_contract_dual_boot_requirements()` and placed in the
  detected free region, so the footprint they allocate is
  `INSTALLER_LAYOUT_CONTRACT_DUAL_BOOT_FOOTPRINT_LBA` (262144 sectors =
  128 MiB);
- the summary reports the previous system's span
  (`existing_partitions`, `existing_first_lba`, `existing_lba`) and the space
  left after the reservation (`free_first_lba`, `free_lba`);
- a plan that would leave the previous system no space is refused with
  `INSTALLER_LAYOUT_CONTRACT_NO_RESERVE`, and any summary whose HFOC roles
  overlap a discovered partition is refused with
  `INSTALLER_LAYOUT_CONTRACT_EXISTING_SYSTEM_OVERLAP`. The product never
  overwrites the previous OS by arithmetic accident.

Boundary: the frozen GPT writer carries four entries, so an executable
dual-boot layout that preserves unrelated partition *entries* is still
ITEM-341/ITEM-342 scope. ITEM-391 owns the sizes, the reservation arithmetic
and the refusal. Which split a dual-boot install should choose — reuse the
previous system's free space after its partitions, as ITEM-341 plans today —
remains an open product decision.

### Publication

The plan preview carries the same numbers it will write: `mode`,
`footprint_lba`/`footprint_mib`, `minimum_media_lba`, `free_first_lba`,
`free_lba`/`free_mib` and one `role=<name>:<first_lba>+<length_lba>=<mib>mib`
entry per role. The boot/serial record publishes them as single-line markers:

```
[INSTALLER-LAYOUT] plan mode=clean-whole-disk disk=1 media_lba=... media_mib=... \
minimum_media_lba=331810 minimum_media_mib=162 declared_lba=329728 declared_mib=161 \
first_lba=2048 footprint_lba=329728 footprint_mib=161 free_first_lba=331776 \
free_lba=... free_mib=... existing_partitions=0 existing_first_lba=0 existing_lba=0 esp_reused=0
[INSTALLER-LAYOUT] role=efi-system first_lba=2048 length_lba=67584 length_mib=33 reused=0
[INSTALLER-LAYOUT] role=hfoc-system first_lba=69632 length_lba=131072 length_mib=64 reused=0
[INSTALLER-LAYOUT] role=hfoc-persistence first_lba=200704 length_lba=65536 length_mib=32 reused=0
[INSTALLER-LAYOUT] role=hfoc-recovery first_lba=266240 length_lba=65536 length_mib=32 reused=0
```

`production_installer_publish_layout_record()` writes the record inside one
`serial_sequence_begin()`/`serial_sequence_end()` pair, so the hardware
acceptance `INSTALLER` stage and the runbook can quote the installed footprint
instead of a guess.

### Proofs

`make test-installer-layout-contract-host` proves the declared arithmetic and
its pins, the exact minimum-media boundary (accepted at 331810 sectors, refused
one sector below with no partial plan), role alignment, non-overlap and GPT
bounds, bounded arithmetic on hostile summaries, the dual-boot reservation and
its refusals, and that the published record, the plan and the executed GPT
carry the same numbers. `make test-production-installer-qemu` executes the
production entry point on the acceptance disk's size class, publishes the
record, and compares it with the GPT the execution wrote.

### Open product questions

- How much margin the system and state roles should carry: the declared numbers
  are slot multiples (system 4 x 16 MiB, persistence 2 x 16 MiB, recovery
  2 x 16 MiB) chosen from the measured image and the proven state volume, not
  from a storage budget the product has yet to state.
- How a dual-boot install decides the split between the previous system and
  the appliance: ITEM-391 reserves the free region and publishes it, but the
  product has not decided whether an operator may shrink the previous system
  or must reinstall it.
- The ESP is capped at 4190 sectors by the fixed FAT geometry of
  `production_installer_esp`; growing the role needs that formatter to size
  `sectors_per_fat` from the volume first.
