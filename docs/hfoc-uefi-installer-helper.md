# HFOC UEFI Installer Helper

**Date:** 2026-09-10
**Target:** ITEM-361 of the HFOC Appliance Platform production wave
**Release contract:** HFOC-APPLIANCE-V1
**Status:** Implemented and OVMF-proven; physical UEFI installation remains
`NOT_TESTED`

## Purpose

ITEM-361 adds a bounded pre-kernel UEFI installer helper on dedicated helper
media. Limine remains the kernel loader and bootloader. The helper is
chainloaded by the existing Limine configuration, installs the approved
Limine EFI payload on one uniquely selected HFOC target, and creates or
updates the HFOC UEFI boot entry.

Ordinary boot does not activate this helper. Normal HFOC kernels remain
loaded by the normal Limine path and do not acquire UEFI runtime-service
ownership.

## Implemented Boundary

`boot/efi/hfoc_efi_installer.c` is the portable target-selection and apply
orchestrator. It:

- validates every candidate `block_device`;
- uses `appliance_gpt_find_hfoc_partition()` to identify HFOC persistence
  partitions;
- requires exactly one valid HFOC disk and exactly one ESP on that disk;
- fails closed on ambiguous targets or ESPs and performs no write before
  selection succeeds;
- installs the approved payload through `efi_fat32_esp`;
- snapshots, plans, confirms, applies, and verifies only the HFOC UEFI boot
  entry through `efi_boot_variables`.

`boot/efi/hfoc_efi_installer_uefi.c` is the minimal UEFI provider. It uses
the existing `efi_boot_environment` callback seam for block and variable
access. Its firmware operations are limited to:

1. locating block and SimpleFileSystem handles;
2. reading the fixed helper payload
   `\EFI\HFOC\PAYLOAD.EFI` from exactly one helper volume;
3. writing the approved payload to
   `\EFI\HFOC\BOOTX64.EFI` on the selected ESP;
4. reading and writing the HFOC `Boot####` entry and `BootOrder`.

The helper does not format storage, modify partitions, write arbitrary UEFI
variables, delete unrelated boot entries, or provide a general writable
filesystem. It does not create a second EFI environment abstraction; the
portable installer consumes the existing `efi_boot_environment` interface.

The UEFI boot option contains the real caller-owned device path and the
`FilePath` to `\EFI\HFOC\BOOTX64.EFI`. Device paths are bounded and validated
without assuming four-byte length alignment.

## Build And Proof

`make test-hfoc-efi-installer-host` runs portable fixtures for no target,
single-target selection, enumeration-order independence, multiple disks,
multiple HFOC partitions, multiple ESPs, and missing ESP behavior.

`make test-hfoc-efi-installer-qemu` builds a dedicated helper ISO. Limine
chainloads the helper twice against a disposable OVMF target disk. The proof
requires two successful selections, identical boot-order and boot-entry
counts across launches, exactly one HFOC entry, and preservation of the
unrelated Ubuntu and Debian entries. It also compares the installed payload
and unrelated ESP files byte-for-byte.

OVMF QEMU evidence is software acceptance only. It does not prove a physical
motherboard firmware, NVRAM implementation, ESP, or boot path. Physical UEFI
installation remains `NOT_TESTED`.

## Non-Goals

- No replacement for Limine and no alternative bootloader.
- No custom UEFI loader or normal-boot EFI runtime client.
- No EFI/NVRAM decision for later providers.
- No arbitrary UEFI variable mutation.
- No storage mutation outside the selected ESP payload path.
- No general writable filesystem.
- No network, service, management, or physical acceptance activation.
- No `VERSION` or v1.0.0 change.
