# Boot and installation

[Home](../README.md) · English · [Русский](install.ru.md)

HFOC OS starts from a BIOS or UEFI boot image on an x86-64 machine. Download the generic ISO and `SHA256SUMS.txt` from [release v1.0.0](https://github.com/EdgarMisyukov/hfoc-os-public/releases/tag/v1.0.0). Check the ISO against the corresponding entry before booting.

## Try the release ISO in QEMU

Run this from the directory containing the verified ISO. It opens the generic boot image without attaching a hard disk. Disk installation uses separately generated installer media with a disk and layout declaration.

```sh
qemu-system-x86_64 -machine q35 -m 512M -cpu max,-apic \
  -cdrom hfoc-os-1.0.0.iso -serial stdio -no-reboot
```

## Select an installation mode

| Mode | What it does | Space reserved by HFOC |
| --- | --- | --- |
| Whole disk, UEFI | Replaces the selected disk layout with the HFOC layout | 161 MiB |
| Whole disk, legacy BIOS | Replaces the selected disk layout with the HFOC layout | 162 MiB |
| Preserve an existing system | Installs in unallocated space on supported GPT/UEFI systems and uses the existing EFI System Partition | 128 MiB |
| Update | Updates the installed HFOC system and preserves the owner workspace and settings | Uses the existing layout |

These reservations describe the layout itself. On media with 512-byte logical sectors, the whole-disk minimum including metadata and alignment is 331,810 sectors in UEFI mode and 333,858 in legacy BIOS mode. The installer checks the declared disk identity and layout before writing. Back up data and confirm the exact target before a whole-disk install.

For dual boot, prepare contiguous aligned unallocated space on the GPT disk and retain its single existing EFI System Partition. Before confirming, check that the installer recognizes the existing system and offers a preserving plan. The boot menu can show the existing system when discovery recognizes a supported UEFI entry.

After installation, boot the installed disk and follow the [first-boot and management guide](operate.md).

See the [detailed installation and dual-boot guide](install-detail.md) for preparing an installer, reviewing the disk plan and verifying the result.
