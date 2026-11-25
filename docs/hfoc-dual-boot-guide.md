# HFOC Dual-Boot Guide

**Release ID:** HFOC-APPLIANCE-V1
**Scope:** Preserving existing Linux and Windows installations

## Purpose

HFOC supports installing alongside an existing operating system without
deleting its system or data partitions. This guide describes the safety
requirements for Linux dual boot, Windows dual boot, shared ESP use, and the
HFOC boot menu.

Physical dual-boot installation and existing-OS boot remain `NOT TESTED`
until a Hardware Acceptance Report records evidence on dedicated hardware.

## Linux Dual Boot

For an existing Linux installation:

1. Detect the Linux installation without modifying it.
2. Keep its system and data partitions untouched.
3. Preserve the existing EFI System Partition when suitable.
4. Add an HFOC boot record without deleting the Linux boot entry.
5. Validate that the Linux boot path remains available.

### What the installer can and cannot see (ITEM-501)

The installer now names what is really in a partition instead of calling it
"Linux/unknown":

- **LVM** (the Fedora default) and **Btrfs** are recognised and shown as such
  (`Layout: LVM`). The volumes inside them are *not* listed: HFOC cannot
  install into an LVM volume group or a Btrfs subvolume, and it does not offer
  such a partition as a target for its own filesystems.
- **MBR logical partitions** inside an extended container are listed with their
  own extents and indices from 5; the extended container itself is a table, not
  a target.
- A dual-boot install needs **UEFI + GPT + exactly one shared ESP** and room
  for three partitions (`HFOC_SYSTEM`, `HFOC_PERSISTENCE`, `HFOC_RECOVERY`,
  about 128 MiB together, contiguous and aligned).

### Two ways to install beside a system

1. **Free space (the default).** If the disk has a contiguous aligned free
   region large enough, the installer uses it and keeps every partition. The
   screen shows how much free space there is and how much is needed; a disk
   that does not fit is refused with both numbers, and nothing is written.
2. **Give up the selected partition (ITEM-501).** On a full disk - a Fedora
   laptop, for example - the operator may choose one existing partition to give
   up. The screen names the partition, its size, its kind, its owning system and
   its layout; the warning page says that the data of *that* partition will be
   erased while every other partition, the shared ESP and the systems on them
   stay. On confirmation the installer removes exactly that GPT entry and
   creates the three HFOC partitions in the space it occupied. A partition too
   small for the three roles is refused with the numbers, and no partial layout
   is written.

Giving up a partition is a deliberate, irreversible operator action. The
installer never chooses a partition by itself, and it never removes the shared
ESP.

### Preparing a free region from Fedora

A Fedora installation usually fills the disk. To keep Fedora and shrink it
instead of giving it up:

1. Boot Fedora and back up anything irreplaceable.
2. Use `gparted`, GNOME Disks or `blivet-gui` to shrink the filesystem and then
   the partition (an LVM physical volume must first have its logical volumes
   and the volume group shrunk - the tools do this in that order).
3. Leave at least 256 MiB of *unallocated* space on the same GPT disk, and keep
   the existing ESP in place.
4. Boot the HFOC medium and choose the disk: the free-space path should now fit
   and the Fedora entry stays in the boot menu.

If the disk cannot be shrunk, use the "give up the selected partition" path
above and choose a partition whose contents are expendable.

## Windows Dual Boot

For an existing Windows installation:

1. Detect the Windows Boot Manager without modifying Windows partitions.
2. Preserve Windows boot files and EFI entries.
3. Add HFOC as an additional entry, not a replacement for Windows Boot
   Manager.
4. Ensure Windows remains selectable from the boot menu.

## Shared ESP

The EFI System Partition is shared boot infrastructure. HFOC must not
reformat or claim an ESP as HFOC-private state. If the existing ESP is
suitable, reuse it without deleting existing OS boot files.

## Boot Menu

The intended conceptual menu is:

```text
HFOC APPLIANCE
> HFOC OS
  Existing Linux
  Windows Boot Manager
  Recovery
```

Existing discovered systems must not be hidden. The actual boot
implementation and physical menu remain subject to Hardware Acceptance
Report evidence.

## Failure Handling

- Ambiguous disk or boot configurations must require manual selection.
- Unknown boot paths must fail closed.
- Existing OS preservation must be validated after every mutation.

## Acceptance

Software dual-boot planning proofs cover Linux/Windows menu records and
existing partition preservation. Physical dual-boot boot, existing OS boot,
and boot-menu selection remain NOT TESTED.

## Non-Goals

- No formatting of existing OS partitions.
- No claim of physical dual-boot acceptance from QEMU/host proof.
- No release version change.
