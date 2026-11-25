# HFOC Dual Boot

**Date:** 2026-09-06
**Target:** ITEM-341 of
`docs/superpowers/plans/2026-09-06-hfoc-appliance-installer-and-management-ui.md`
**Base SHA:** `758ee0ae265427b2604e4396ce8668e8be669029`
**Status:** Completed by ITEM-341

## Purpose

This document is the authoritative ITEM-341 dual-boot boundary. ITEM-341
implements bounded dual-boot planning and boot-menu record generation on top
of the ITEM-339 read-only discovery report. It proves safe selection of a
UEFI/GPT target, preservation of existing partitions and the existing EFI
System Partition, deterministic HFOC/existing-OS menu records, and
fail-closed handling of ambiguous or unknown boot state.

Actual UEFI NVRAM entry creation, EFI bootloader files, disk bootloader
installation, and recovery remain ITEM-342 work.

## Module

`installer_dual_boot` lives under `kernel/` and publishes:

- automatic and selected-target dual-boot plans;
- bounded existing-OS and ESP eligibility checks;
- aligned free-space selection for HFOC system, persistence, and recovery;
- deterministic preview and confirmation state;
- boot-menu records for HFOC, Linux, Windows, other bootable systems, and
  recovery;
- bounded ASCII menu rendering;
- exact status strings.

The module consumes an ITEM-339 finalized discovery report and never invokes a block-device write callback.

## UEFI/GPT Target Rules

Automatic dual-boot installation is planned only when:

1. firmware mode is UEFI;
2. the target disk is GPT;
3. the target disk has exactly one EFI System Partition;
4. the target disk contains at least one discovered existing OS;
5. enough contiguous aligned free space exists for the requested HFOC
   partitions.

The module preserves all existing partitions on the selected disk and reuses
the existing ESP. It does not create a second ESP. Legacy BIOS, MBR, missing
ESP, multiple ESPs, no existing OS, insufficient free space, and unknown
existing-OS boot paths fail closed.

## ITEM-501: The Layouts The Discovery Used To Hide

A Fedora installation is not one partition with one filesystem: it is commonly
a GPT disk whose Linux partition carries an LVM volume group (or a Btrfs
filesystem), and an MBR disk can carry its systems in logical partitions inside
an extended container. The discovery used to describe all of that as one
"Linux/unknown" partition, so the operator could not tell a plain partition
from a volume group. ITEM-501 names what is there:

- `installer_partition_layout` is `PLAIN`, `LVM`, `BTRFS`, `EXTENDED` or
  `LOGICAL`; the screen prints the name (`Layout: LVM`) and the partition
  record carries it (`layout=LVM`);
- the MBR extended container is published as a container - a table, not a
  target - and the EBR chain is walked within a bounded depth (`EBR_MAX` 32), so
  the logical partitions get their own extents and their own indices from 5;
- on GPT, the Linux LVM type GUID is recognised, and the Btrfs superblock magic
  (`_BHRfS_M` at 0x10040) is probed when the entry is a Linux filesystem;
- a partition whose layout is not `PLAIN` is named, but is not offered as a
  free-space target: HFOC cannot install *into* an LVM volume group or a Btrfs
  filesystem, and it says so instead of pretending the partition is empty.

## ITEM-501: Giving Up One Partition

Free contiguous space is the first way to install beside a system, and it stays
the default. A machine whose disk is full - the common Fedora laptop - has no
free space at all, and refusing there would leave the owner with no path. The
second, explicit path is the one the owner asked for: **give up the selected
partition**.

- the operator screen lists the partitions that may be given up (never the
  shared ESP, never a container) with the device index, capacity, kind, owning
  system and volume layout;
- the warning page names the same values and says what dies - the data of that
  one partition - and what stays: every other partition, the shared EFI System
  Partition and the systems on them;
- on confirmation, `installer_gpt_preserve_write()` removes exactly that GPT
  entry (writes zero bytes into its slot) and places the appliance's three roles
  (`HFOC_SYSTEM`, `HFOC_PERSISTENCE`, `HFOC_RECOVERY`) in the extent it
  occupied, aligned; the protective MBR and every other entry stay
  byte-for-byte;
- a partition that cannot hold the three roles is refused with the numbers
  (available versus required) before the first write, and nothing is written;
- the record of the run names the decision: `[PRODUCTION-INSTALL] layout
  mode=release reason=operator-release released_index=N chosen=manual` (the
  manual scheme of ITEM-514 makes the same path an explicit operator choice),
  the layout record
  carries `released_index`, `released_first_lba`, `released_lba`,
  `available_mib` and `required_mib`, and the preservation record says
  `mode=release` with the slot it released.

Both paths are QEMU-proven on a synthetic foreign GPT carrying a valid FAT32
ESP and a Linux system (`scripts/test-qemu-production-install-flow-preserve.sh`,
GREEN and RELEASE runs): the foreign bytes are compared before and after, and
the installed disk boots alone. Physical dual boot, existing-OS boot and
boot-menu selection remain `NOT TESTED` until a hardware run records them.

## Preview And Confirmation

The plan records the selected disk, firmware mode, existing ESP, preserved
partition count, existing OS count, and final HFOC ranges. A bounded preview
is published before confirmation. `installer_dual_boot_confirm()` changes
only confirmation state. The ITEM-341 module has no write path.

## Boot Menu Records

The deterministic menu order is:

1. HFOC OS.
2. Discovered existing systems.
3. Recovery.

Every existing OS in a menu must have either a preserved EFI entry or a
non-empty caller-owned boot path. Missing or unknown paths return an
`UNKNOWN_BOOT_PATH` status instead of publishing a guessed entry.

## Host Proof

`make test-installer-dual-boot-host` runs in-memory fixtures that:

- plan Linux-only and Linux + Windows UEFI/GPT dual boot without writes;
- verify existing partitions and the existing ESP are preserved;
- render an HFOC/Linux/Windows/Recovery menu;
- reject multiple automatic targets as ambiguous;
- reject no existing OS, MBR, missing ESP, and unknown boot paths;
- reject insufficient free space;
- verify status strings are exact.

ITEM-501 adds to the same proof the layout names (`LVM`, `BTRFS`, `EXTENDED`,
`LOGICAL`), the MBR EBR chain, and the release planner: a named LVM partition
is given up, its extent holds the three roles, the shared ESP is refused as a
target, a container is refused, and a partition smaller than the three roles is
refused; `make test-installer-gpt-preserve-host` proves the writer removes
exactly the named slot and keeps every other entry byte-for-byte, and
`make test-installer-layout-contract-host` proves the release summary and its
numbers.

This is a host proof, not physical-machine or QEMU dual-boot acceptance.

## Boundaries

- ITEM-338 owns the installer architecture audit.
- ITEM-339 owns read-only hardware and existing-OS discovery.
- ITEM-340 owns clean whole-disk/manual layout and transaction semantics.
- ITEM-341 (this item) owns dual-boot planning and boot-menu records.
- ITEM-342 owns bootloader and UEFI-entry recovery.
- ITEM-346 owns integrated installer/dual-boot acceptance.

## Non-Goals

- No UEFI boot entry mutation.
- No EFI System Partition filesystem writes.
- No bootloader installation or recovery.
- No legacy automatic dual-boot path.
- No booting of an installed HFOC or existing OS in this item.
- No real disk-controller driver.
- No physical-hardware acceptance.
- No VERSION or release-number change.
