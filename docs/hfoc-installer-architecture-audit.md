# HFOC Installer Architecture Audit

**Date:** 2026-09-06
**Target:** ITEM-338 of
`docs/superpowers/plans/2026-09-06-hfoc-appliance-installer-and-management-ui.md`
**Base SHA:** `938744beded596f4512f59a511cd6986305d818b`
**Status:** Completed by ITEM-338

## Purpose

This document is the authoritative ITEM-338 installer architecture and
storage-safety audit. It records what actually exists in the repository, what
is missing, and which boundaries later installer items must satisfy. The
audit is a specification result, not an implementation of an installer.

No QEMU, host, or static proof in this repository establishes that HFOC can
be installed on a physical machine. Real-hardware acceptance remains a
separate release requirement.

## Current State

The repository builds a read-only hybrid ISO and boots the freestanding HFOC
kernel with Limine. Storage primitives exist as bounded caller-owned modules
and are proven in host and macro-gated QEMU fixtures. The normal production
boot path does not discover, partition, mount, or install to a real disk.

There is no production installer subsystem.

## ISO Format And Boot Mode

The `iso` target in `Makefile` creates a hybrid ISO under `build/`. The
resulting image is an ISO 9660 CD-ROM with a protective MBR label and both
El Torito BIOS and UEFI boot records.

The current ISO contains:

- the HFOC kernel at `boot/hfoc-kernel.elf`;
- `boot/limine.conf`;
- Limine BIOS and UEFI CD payloads;
- Limine `BOOTX64.EFI` under `EFI/BOOT`;
- the licensed payload tree.

`xorriso` writes the BIOS El Torito image from
`boot/limine/limine-bios-cd.bin`, the UEFI El Torito image from
`boot/limine/limine-uefi-cd.bin`, and `limine bios-install` writes the BIOS
ISO boot record.

## UEFI

UEFI boot is already present at ISO level. The ISO exposes the Limine UEFI
CD image and the standard removable-media EFI binary path. `make run-uefi`
launches QEMU with an OVMF firmware file and the current ISO.

The repository does not manage persistent UEFI firmware variables, does not
create HFOC NVRAM entries, and does not preserve or augment an existing EFI
System Partition. UEFI boot works only through removable/optical media or
whatever external firmware path selects the ISO.

UEFI is first-class for the installer queue. Later installer items must keep
the existing ISO UEFI capability and add deterministic disk/EFI installation
behavior without weakening it.

## Legacy BIOS

Legacy BIOS/BIOS-CD boot is retained by the current ISO. Limine's BIOS CD
payload and `limine bios-install` are used. Existing legacy profiles run
QEMU with SeaBIOS and the current ISO.

Legacy disk installation is not implemented. There is no BIOS MBR stage
installation onto a disk, no legacy boot-menu generation, and no dual-boot
configuration.

Legacy BIOS may remain supported, but it must not block or weaken UEFI
first-class behavior.

## Installer Environment

No installer environment exists. The ISO is a normal runtime boot media, not
an install-to-disk workflow. The kernel has no:

- installer boot profile;
- installer console or keyboard-driven wizard;
- install action state machine;
- separate installer executable or task;
- first-boot handoff record;
- recovery-media workflow.

The update package architecture defines update stages and a future runtime
installer boundary for packages. It does not provide OS installation to a
physical disk.

## Hardware Probing

Current kernel probing includes:

- Limine memory map and HHDM;
- CPU and local APIC/IOAPIC state;
- ACPI RSDP/RSDT/MADT when available;
- PS/2 keyboard;
- virtio PCI devices used by proof/network/RNG paths;
- framebuffer and serial console.

There is no installer-oriented hardware inventory. There is no public
hardware probe API that returns CPU model, installed RAM capacity, disk
devices, network adapters, or PCI devices for a setup flow.

## Disk Discovery

No production disk discovery exists. The kernel does not:

- enumerate PCI or platform block controllers;
- probe SATA, NVMe, USB storage, or virtio-blk;
- list whole disks and their capacities;
- identify an EFI System Partition;
- identify free space;
- identify a target disk by stable identity.

The `block_device` interface is a bounded seam, but all real uses in host and
QEMU fixtures are caller-supplied memory or RAM-backed devices. No driver
constructs a `block_device` from hardware.

## Partitioning Abstraction

No partitioning abstraction exists. The repository has no:

- GPT parser or writer;
- MBR parser or writer;
- partition-type registry;
- partition number/label/identity model;
- free-space calculator;
- partition transaction model;
- EFI System Partition model;
- Windows or Linux partition classifier.

Any code that appears to mention partition or disk layout is documentation or
host test fixture text, not a kernel partition API.

## Filesystem

HFOC deliberately has no general writable filesystem. The current storage
format consists of:

- `HFOCDATA`, a fixed 64-byte data-region header describing a block range;
- `HFOCJRNL`, a checksummed append-only configuration journal with atomic
  commit records;
- bounded configuration/KV, backup, restore, update, and state modules built
  above caller-owned storage.

The kernel does not mount ext4, FAT, NTFS, or any other existing-OS
filesystem. It cannot read Linux root filesystems or Windows filesystems for
dual-boot discovery. Later discovery work must add bounded metadata readers
for supported existing systems or explicitly classify unsupported systems.

The installer queue must not create a general-purpose desktop filesystem.

## Installation Target

No installation target exists. Normal boot does not write an installed HFOC
system to a disk. Persistent-state tests use caller-provided RAM/block
fixtures and do not install a bootable HFOC OS.

The future installed layout must separate:

- bootloader-owned files;
- HFOC system image;
- persistent configuration namespaces;
- recovery metadata.

ITEM-340 owns layout creation and installation. ITEM-338 only fixes the
boundaries that ITEM-340 must implement.

## Bootloader

Limine is the bootloader used by the ISO. The repository vendors Limine files
and invokes `limine bios-install` only for ISO creation. There is no disk
bootloader installation path. There is no UEFI NVRAM entry creation or
preservation. There is no UEFI application installation into an EFI System
Partition, boot menu generation for installed HFOC and existing systems,
chainload path for Linux or Windows, or pre-mutation boot-configuration
snapshot.

Limine is not an installer. Its files are reusable payloads for later
bootloader work, but actual disk/EFI installation and entry management must
be implemented and tested by ITEM-341 and ITEM-342.

## Persistence

Existing persistence is bounded configuration/journal state, not an installed
OS. Reusable pieces include:

- persistent storage namespace catalogue;
- configuration integrity classifier and recovery planner;
- data-region discovery;
- config journal;
- persistent config, network, and service modules;
- secret/certificate persistence policy;
- factory-reset state map.

These modules assume a caller supplies a device/region. They are the state
layer that a future installed HFOC boot can use after ITEM-340 creates the
layout.

## Rollback And Failure Handling

Current rollback/failure contracts cover:

- journal corruption and schema-invalid configuration recovery;
- update first-boot health and automatic rollback policy;
- service failure handling;
- recovery-mode decision policy;
- factory-reset boundary.

There is no installer transaction journal. There is no mechanism to record a
pre-installation disk/partition/boot state and restore it after a torn write,
failed partition change, or failed bootloader installation.

The future installer must treat disk and bootloader mutation as staged work:

1. Record exact pre-mutation disk state.
2. Present the destructive-operation plan.
3. Wait for explicit confirmation.
4. Apply each mutation with bounded steps and failure status.
5. On failure, return to the last safe state or emit recovery instructions
   backed by the recorded snapshot.
6. Never report success without post-mutation verification.

## Logging

Current logging is serial output plus bounded runtime log rings used by
proofs and diagnostics. There is no installer log contract.

The future installer must produce bounded, structured, deterministic logs
covering:

- selected firmware mode;
- discovered hardware;
- selected disk and partitions;
- destructive-action confirmation;
- every mutation and verification step;
- failure and recovery path.

Installer logs must never contain passwords, private keys, bearer tokens, or
other secret material.

## Recovery

Current recovery is policy-only:

- configuration integrity recovery planning;
- appliance recovery mode;
- update rollback;
- factory-reset evidence gating.

There is no bootloader recovery. There is no recovery media entry, no
repaired-bootloader acceptance proof, no lost-EFI-entry repair flow, and no
verification that an existing OS still boots after HFOC changes.

ITEM-342 owns bootloader recovery. It must test damaged HFOC bootloader,
lost EFI entry, failed bootloader update, HFOC boot restoration, and
preservation of existing operating systems.

## Required Installer Architecture

The following boundaries are fixed for later items:

1. **Media**: The existing Limine hybrid ISO is the installer/recovery media
   starting point. Installer and normal boot may be selected by a bounded
   build/profile or menu path, but must not require a custom UEFI loader.
2. **Firmware mode**: The installer must record whether it is running under
   UEFI or legacy BIOS before presenting disk choices.
3. **Disk model**: A bounded catalogue of whole disks with stable identity,
   size, block size, partition-table type, and selected/target state is
   required.
4. **Partition model**: A bounded catalogue of partitions with start, length,
   type, EFI system flag, and existing-OS classification is required.
5. **Read-only discovery**: Detection and preview must not write to any
   existing partition or EFI state.
6. **Confirmation**: Any destructive or partition-changing action requires an
   explicit confirmation step showing selected disk, existing partitions,
   deleted data, preserved data, and final layout.
7. **Transaction/rollback**: Disk and bootloader mutations need a persistent
   pre-mutation snapshot and restartable/fail-safe semantics.
8. **Bootloader safety**: Existing EFI entries, EFI System Partition contents,
   and existing OS boot paths must be preserved.
9. **Dual boot**: HFOC adds deterministic entries for itself and discovered
   systems. Ambiguous or unsafe layouts must stop for manual selection.
10. **Logging and recovery**: Installer actions are logged without secrets,
    and recovery media can repair HFOC boot state without damaging existing
    systems.

## Safety Principles

- Never run a destructive installation without explicit confirmation.
- Never format an existing OS partition in the automatic dual-boot path.
- Never overwrite an existing OS filesystem merely to install the HFOC
  bootloader.
- Never delete unrelated UEFI boot entries.
- Never guess an unsafe automatic layout.
- Never report partial mutation as success.
- Never claim physical-machine installation from QEMU/CI evidence.

## Dependency Ownership

- ITEM-338 (this item): installer architecture and storage-safety audit.
- ITEM-339: CPU, RAM, disk, partition, EFI, NIC, PCI, Linux, Windows, and
  bootable-system discovery.
- ITEM-340: whole-disk, automatic/clean, and manual installation layout.
- ITEM-341: dual-boot installation and boot-menu generation.
- ITEM-342: bootloader and UEFI-entry recovery.
- ITEM-343: first-boot wizard.
- ITEM-344: local appliance console.
- ITEM-345: HTTPS management panel.
- ITEM-346: integrated installer/dual-boot/management acceptance.
- ITEM-347: installation, administration, recovery, and hardware
  documentation.

## Non-Goals

- No installer implementation.
- No real disk driver.
- No GPT/MBR parser or writer.
- No existing-OS filesystem reader.
- No disk bootloader installation.
- No UEFI NVRAM entry management.
- No recovery-media bootloader repair.
- No QEMU installer acceptance.
- No real-hardware installer acceptance.
- No VERSION or release-number change.

## Acceptance

ITEM-338 is complete when the audit records the current state above, fixes
the required installer boundaries, the focused host test is part of the host
suite, the roadmap marks ITEM-338 complete and ITEM-339 incomplete, static
checks and branch CI pass, and no installer code is introduced.
