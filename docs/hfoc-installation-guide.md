# HFOC Installation Guide

**Release ID:** HFOC-APPLIANCE-V1
**Scope:** HFOC Appliance clean, automatic, dual-boot, and manual
installation

## Purpose

This guide describes the required HFOC installation workflow and safety
boundaries. It does not claim that a physical installation has passed.
Physical clean/dual-boot/manual installation remains `NOT TESTED` until the
Hardware Acceptance Report records dedicated-machine evidence.

## Safety Before Installation

1. Run HFOC installer media only on dedicated test hardware or a safe spare
   disk.
2. Back up every disk that may be touched.
3. Run read-only hardware and existing-OS discovery before selecting a
   destructive action.
4. Review the preview showing the selected disk, existing partitions, what
   will be removed, what will be preserved, and the final layout.
5. Confirm explicitly before any destructive or partition-changing
   operation. Without explicit confirmation, installation must not start.

Never use a production server, an HFOC runner, or any system containing
accounting or irreplaceable data for destructive installation.

## Boot Media And Firmware

The ISO is a hybrid boot medium. Both documented target modes install: UEFI
first-class, and legacy BIOS with the measured Limine MBR chain (ITEM-517).
The firmware the medium actually booted from selects the installed boot chain,
so the same medium installs either machine. Before installation:

- verify firmware mode and firmware version;
- verify that Secure Boot policy permits the selected boot path or record
  the required exception;
- preserve existing UEFI boot entries;
- treat the EFI System Partition as shared boot infrastructure on UEFI; a
  legacy-BIOS machine has none and does not need one.

### One medium, any machine (ITEM-500)

The medium carries two entries. The **operator** entry (`hfoc.install=console`)
shows the storage screen and installs the disk the person at the machine names,
so the same medium installs on any machine - the disk the medium was *declared*
for does not have to be present. When it is not, the screen says so
(`Declared disk <serial> is not on this machine - choose a disk`) and the run
proceeds with the operator's choice. The **unattended** entry
(`hfoc.install=unattended`) still writes only the disk the declaration names and
refuses with `TARGET_NOT_FOUND` on any other machine: nobody is standing there
to name one.

When the machine cannot install at all, the reason is shown on its own screen
before anything else happens:

- `FIRMWARE_UNSUPPORTED` - the firmware mode could not be determined at all
  (neither UEFI nor legacy BIOS); a detected legacy-BIOS boot installs
  (ITEM-517) and no longer refuses;
- `NO_DISK` - no usable disk was found;
- `REPORT_UNAVAILABLE` - the disk inventory could not be read;
- `NETWORK_MALFORMED` / `FORWARD_MALFORMED` and their `*_INCOMPLETE` forms - the
  declaration cannot be honoured.

The page names the reason, says that this medium cannot install on this machine
and waits for a key (five minutes, then the ordinary boot continues), so a
refused install is something the operator read rather than a machine that
seemed to do nothing.

### Legacy BIOS (no UEFI) - ITEM-517

A machine that boots the medium through legacy BIOS installs too. The
installer detects the firmware mode the medium was booted with and writes the
BIOS boot chain instead of the ESP one:

- the layout is the BIOS contract's five roles - a `Hah!IdontNeedEFI` BIOS
  boot partition for Limine's stage2, a FAT boot volume for the boot set, and
  the three HFOC roles (system, persistence, recovery). There is **no EFI
  System Partition** on this disk and none is required;
- the installer writes Limine's measured stage recipe into the target disk
  (stage1 at LBA0 with the stage2 byte offset patched in, stage2 at the start
  of the BIOS boot partition) and verifies it, the GPT and the boot volume by
  read-back before it reports the install. The restored partition table gets
  the active flag `limine bios-install` gives an MBR disk (partition 1,
  byte `446` of LBA0), because a firmware that consults the flag - the Toshiba
  L875's InsydeH2O does - otherwise skips the disk and never runs stage1. The
  stage's two BIOS calls are also made with `ECX` preserved: that firmware's
  AHCI `INT 13h` handler clobbers the register Limine counts stage2 sectors in,
  and without the repair the loop overruns and the machine reboots before stage2
  runs (the same image boots from a USB stick, whose path preserves `ECX`);
- the boot volume carries the generated menu, the sealed chain configuration,
  the product kernel, the sealed boot manifest and Limine's
  `limine-bios.sys`. The menu is generated for the BIOS machine: the appliance
  entry plus an honest `NO_ESP` comment for every system it cannot chainload,
  because BIOS Limine cannot boot an EFI application;
- the disk then boots on its own with no medium attached, through the sealed
  chain attestation (`[BOOT-CHAIN] verdict=ATTESTED`) and the ordinary
  readiness record.

What is proven where: the install and the installed-disk boot are proven in
QEMU with SeaBIOS (no OVMF) by `scripts/test-qemu-production-install-flow.sh`,
and the operator screens by `scripts/test-qemu-installer-disk-screen.sh`.
Physical acceptance on the owner's Toshiba L875 is the owner's step; it is not
run by CI and is not claimed by these proofs.

A legacy-BIOS **preserving** install (alongside an existing system) is refused
by name (`PRODUCTION_INSTALLER_BIOS_PRESERVE_UNSUPPORTED`): the preserving
planner reuses the previous system's ESP, which a BIOS machine does not have.
The owner's machine takes the whole-disk path.

Building a medium that is not tied to one machine:

- for a machine that will be installed by a person, build the console medium
  (`HFOC_INSTALL_MEDIA_ENTRY=console`) and either omit the target or declare any
  serial - the operator's on-screen choice is what the run writes;
- for a fleet machine that must install without a person, declare the exact
  `HFOC_INSTALL_MEDIA_TARGET` serial of that machine; that declaration is the
  authorization the unattended run writes by and it never widens to another
  disk.

A medium with no declaration at all (`HFOC_INSTALL_MEDIA_CONFIRM` unset) seals
nothing: the unattended entry refuses `TOKEN_MISSING` and the installer boots as
an ordinary console. The current generator writes a declaration only when the
confirmation and the target are both given.

## Installation Choices

### Clean Installation

Use whole-disk clean installation only after explicit confirmation. The
installer must preview that the entire selected disk will be reorganized.

### Install Alongside Existing OS

Use this path to preserve an existing Linux or Windows installation. The
installer must:

- detect and preserve existing system/data partitions;
- preserve the existing ESP when suitable;
- add HFOC without deleting unrelated UEFI entries;
- generate a deterministic boot menu with HFOC and discovered existing
  systems.

### Manual Partitioning

Use manual partitioning only when automatic layout is ambiguous or unsafe.
The final partition table must reserve space for HFOC system, persistent
configuration, recovery, and existing OS boot state.

## Expected Steps

1. Boot installer media.
2. Detect firmware, CPU, RAM, disks, partitions, ESP, NIC, PCI, and existing
   OS evidence.
3. Choose a safe layout and review the exact mutation plan.
4. Confirm destructive operations.
5. Install HFOC files and persistent state layout.
6. Install/configure the bootloader without deleting existing boot paths.
7. Preserve existing EFI entries and add HFOC entries deterministically.
8. Run post-install validation.
9. Reboot into the installed HFOC system or recovery path.

## Validation And Acceptance

Software/QEMU boundary proofs exist for installer layout and discovery, but
they are not physical installation acceptance. A physical installation is
accepted only when the Hardware Acceptance Report records:

- hardware model;
- firmware/BIOS version;
- CPU, RAM, disk, and NIC;
- test date;
- HFOC commit/release ID;
- observed result.

## Non-Goals

- No claim that QEMU installation proof equals physical installation.
- No destructive installation without explicit confirmation.
- No release version change.
