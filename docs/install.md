# Boot and installation

[Home](../README.md) · English · [Русский](install.ru.md)

HFOC OS starts from a BIOS or UEFI boot image on an x86-64 machine. Start in a virtual machine to check the console and networking with an isolated disk image. The public source and image package is still being prepared.

## Select an installation mode

| Mode | What it does | Space reserved by HFOC |
| --- | --- | --- |
| Whole disk, UEFI | Replaces the selected disk layout with the HFOC layout | 161 MiB |
| Whole disk, legacy BIOS | Replaces the selected disk layout with the HFOC layout | 162 MiB |
| Preserve an existing system | Installs in unallocated space on supported GPT/UEFI systems and uses the existing EFI System Partition | 128 MiB |
| Update | Updates the installed HFOC system and preserves the owner workspace and settings | Uses the existing layout |

These reservations describe the layout itself. A disk also needs space for its partition metadata and alignment. The installer checks the declared disk identity and layout before writing. Back up data and confirm the exact target before a whole-disk install.

For dual boot, prepare unallocated space and retain the existing EFI System Partition. Check that the installer recognizes the existing system and offers a preserving plan before you confirm. The boot menu can show the existing system when discovery recognizes its supported UEFI entry.

After installation, boot the installed disk and follow the [first-boot and management guide](operate.md).
