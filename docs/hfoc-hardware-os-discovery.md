# HFOC Hardware And Existing OS Discovery

**Date:** 2026-09-06
**Target:** ITEM-339 of
`docs/superpowers/plans/2026-09-06-hfoc-appliance-installer-and-management-ui.md`
**Base SHA:** `938744beded596f4512f59a511cd6986305d818b`
**Prerequisite boundary:** ITEM-338 installer architecture and storage-safety
audit
**Status:** Completed by ITEM-339

## Purpose

This document is the authoritative ITEM-339 hardware and existing-OS
discovery boundary. ITEM-339 implements an allocation-free read-only report
and MBR/GPT scan path that an installer can use before any destructive
action. Discovery never writes a device, partition, filesystem, EFI
variable, or boot entry.

The module performs no SATA/NVMe/virtio-blk driver work, no generic PCI
configuration-space enumeration, and no EFI filesystem read. Those probe
drivers remain future hardware work. The discovery API accepts caller-owned
evidence from existing CPU/memory/PCI/network paths and scans generic
512-byte block devices for partition metadata.

## Module

`installer_hardware_discovery` lives under `kernel/` and publishes:

- firmware mode (`UNKNOWN`, `UEFI`, `BIOS_LEGACY`);
- CPU core-count evidence;
- usable RAM byte evidence;
- up to 4 disk entries;
- up to 64 partition entries;
- up to 4 existing-OS candidates;
- up to 4 NIC entries;
- up to 16 PCI entries.

The report is initialized `read_only = true`. No API can clear that flag or
perform a write.

## MBR Discovery

`installer_discovery_scan_block_device()` requires a validated 512-byte
`block_device` and reads only block-aligned sectors. MBR partition types map:

- `0xEE` protective entry selects GPT scan;
- `0xEF` is an EFI System Partition;
- `0x83` is a Linux root candidate;
- `0x82` is a Linux swap candidate;
- `0x07` is a Windows basic-data candidate;
- other nonzero types are recorded as `OTHER`.

Extended MBR partitions are not followed in this item. An unsupported or
ambiguous extended layout is not guessed.

## GPT Discovery

GPT scan validates the `EFI PART` header, header size, entry size, entry
LBA, and bounds, then reads partition-entry sectors. It recognizes:

- the EFI System Partition GUID;
- the Linux filesystem-data GUID;
- the Microsoft basic-data GUID;
- the Linux swap GUID;
- unknown nonzero GUIDs as `OTHER`.

A GPT with more than 64 nonzero partitions fails with
`TOO_MANY_PARTITIONS`. A truncated or partial scan is never reported as a
safe final layout.

## Existing OS Candidates

The report derives OS candidates only from unambiguous partition metadata:

- Linux root partitions produce a `LINUX` candidate;
- Windows basic-data partitions produce a `WINDOWS` candidate;
- an EFI System Partition alone does not name an OS;
- caller-supplied boot paths may publish `LINUX`, `WINDOWS`, or `OTHER`
  candidates.

No filesystem is mounted and no bootloader is executed.

## Firmware, CPU, RAM, NIC, PCI

The module stores caller-owned evidence for firmware mode, CPU core count,
RAM bytes, NIC snapshots, and PCI snapshots. Existing seams that can supply
evidence are Limine/PMU/ACPI/MP state, `network_device`, and virtio PCI
identity modules. The module itself does not execute new hardware probes in
this item.

## Safe Completion

`installer_discovery_finalize()` marks a report complete only after:

- firmware mode is known;
- CPU and RAM evidence are present;
- no scan returned an ambiguous or unsupported result;
- all partition records fit inside their published disks.

`installer_discovery_validate()` returns `OK` only for a read-only,
finalized report. An installer must call this validation before any
destructive preview or action.

## Host Proof

`make test-installer-hardware-discovery-host` runs in-memory fixtures that:

- parse an MBR with Linux and Windows partitions;
- parse a GPT with ESP, Linux, and Windows partitions;
- scan multiple disks with globally unique partition IDs;
- verify no write callback occurs during discovery;
- reject invalid MBR/GPT, overlapping partitions, oversized GPT tables, and
  unsupported block sizes;
- infer existing-OS ownership for manually published partitions;
- validate report publication and fail-closed behavior.

This is a host proof, not physical-hardware acceptance.

## Boundaries

- ITEM-338 owns the installer architecture audit.
- ITEM-339 (this item) owns read-only discovery and safe report semantics.
- ITEM-340 owns whole-disk/manual installation and must consume this report.
- ITEM-341 owns dual-boot and boot-menu generation.
- ITEM-342 owns bootloader/UEFI recovery.
- ITEM-346 owns integrated installer acceptance.

## Non-Goals

- No real disk-controller driver.
- No generic PCI enumerator.
- No EFI filesystem reader.
- No UEFI variable access or mutation.
- No destructive operation or confirmation UI.
- No bootloader entry generation.
- No physical-hardware acceptance.
- No VERSION or release-number change.
