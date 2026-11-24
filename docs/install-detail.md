# Installing HFOC OS 1.0.0 and setting up dual boot

[Boot and installation overview](install.md) · English · [Русский](install-detail.ru.md)

This guide applies to HFOC OS `v1.0.0`, published on 25 November 2026 (UTC+07:00), source revision `933f7cd739a9437e86638fbfe30a738e22f2e9aa`. The installer uses release profile `HFOC-APPLIANCE-V1`.

## Choose the boot image

The release download `hfoc-os-1.0.0.iso` is a generic x86-64 boot image. It starts HFOC OS through Limine in UEFI or legacy BIOS mode. Use it to try the console in a virtual machine without attaching a disk. With the release assets in the same directory, check the download and boot it as described in the installation overview. [Boot and installation overview](install.md)

```sh
sha256sum -c SHA256SUMS.txt
qemu-system-x86_64 -machine q35 -m 512M -cpu max,-apic \
  -cdrom hfoc-os-1.0.0.iso -serial stdio -no-reboot
```

Disk installation and state-preserving updates use separately built installer media. The installer medium carries the installer kernel and the product kernel it stages on the selected disk. A declaration supplies the confirmation, target disk identity, management scope and any selected layout or network options. Treat media with a disk declaration, owner public key or machine-bound entropy record as private.

## Prepare the target

1. Use dedicated test hardware, a spare disk or an isolated VM disk image. Back up every disk that the installer might touch. Keep working recovery media for any operating system you intend to preserve.
2. Record the target disk model, installer-visible identity and capacity. Use the hardware-reported identity, such as the SATA serial, instead of a Linux device path. Use read-only disk and existing-OS discovery before choosing a layout. Check the reported disks, partitions, firmware mode, EFI System Partition (ESP), and any discovered systems.
3. Boot the installer in the firmware mode you intend to use for the installed disk. The booted mode selects the installed boot chain. Check that the firmware and Secure Boot policy permit the boot path before changing the disk. For dual boot, use the same UEFI mode as the existing system and keep its firmware boot entries.
4. Plan for local keyboard and display access when using the console entry. Review each screen and confirm only when the disk, partitions, firmware mode and proposed changes match your intent.

## Build and boot an operator installer

Use a clean source checkout matching the release, with the project build dependencies installed. When changing a declaration, build in a fresh checkout with no earlier installer output. An existing installer ISO can be up to date by timestamp even when the `make` variable values have changed.

Set `HFOC_DISK_ID` to the intended disk identity before running this example. The example accepts only letters, digits, `.`, `_`, `:`, or `-`, with a limit of 31 characters. Compare the identity with the installer inventory when booting. This example declares preservation as the initial layout and DHCP in the `management-interface` scope. The operator still chooses and confirms the final scheme on screen.

```sh
(
  LC_ALL=C
  export LC_ALL
  : "${HFOC_DISK_ID:?Set HFOC_DISK_ID to the intended disk identity}"
  case "$HFOC_DISK_ID" in *[!A-Za-z0-9._:-]*) exit 1 ;; esac
  [ "${#HFOC_DISK_ID}" -le 31 ] || exit 1
  make production-installer-console-media \
    HFOC_INSTALL_MEDIA_CONFIRM=ITEM-394-UNATTENDED-INSTALL \
    HFOC_INSTALL_MEDIA_TARGET="$HFOC_DISK_ID" \
    HFOC_INSTALL_MEDIA_MANAGEMENT=management-interface \
    HFOC_INSTALL_MEDIA_LAYOUT=preserve \
    HFOC_INSTALL_MEDIA_NETWORK=dhcp
)
```

Set `HFOC_INSTALL_MEDIA_LANGUAGE=ru` in the `make` invocation for Russian installer screens. The default is `en`.

The build creates `build/production-installer/hfoc-os-1.0.0-production-installer-console.iso` and a `.sha256` sidecar. Verify the image before using it. Invoke this command from the same source directory where you built the image.

```sh
sha256sum -c build/production-installer/hfoc-os-1.0.0-production-installer-console.iso.sha256
```

Attach the ISO to a VM as a virtual optical drive or write it to a verified removable medium with your operating system's image writer. Boot the entry whose command line is `hfoc.install=console`. With this build target, the console entry is first in the boot menu. The other entries are `hfoc.install=unattended` and `hfoc.install=none`. The latter starts the appliance without installing it.

- **Operator (`hfoc.install=console`).** The storage screen waits for a disk selection, including when discovery finds a single selectable disk. If the declaration's disk is absent, the installer displays that fact and lets the operator choose from the disks it finds. A disk with unreadable identity or geometry is unavailable for selection.
- **Unattended (`hfoc.install=unattended`).** The declaration authorizes writing to exactly one disk without local confirmation. Build dedicated media with `production-installer-authorised-media` and explicitly set the target identity, confirmation, management scope, layout and network settings. A disk identity absent from the inventory gives `TARGET_NOT_FOUND`, and a duplicate match gives `TARGET_AMBIGUOUS`. Keep unattended media away from disks unless you intend the declared operation to start automatically.

The installer build emits an authorization declaration only when both confirmation and target are supplied. An install entry without a declaration gives `TOKEN_MISSING` and leaves the disk untouched. Static networking requires an IPv4 address and prefix, gateway and DNS. Use a consistent declaration even when you intend to choose the disk at the console. The installer checks it again before planning the write. The declaration limit is 1024 bytes.

## Select a scheme and review the plan

On the disk screen, use the arrow keys or `W` and `S` to move, and `Enter` to choose. On the scheme screen, read the displayed disk and available and required space. Select the scheme that matches the intention for that disk.

| Scheme | Operation | New partition space |
| --- | --- | --- |
| Whole-disk erase, UEFI | Replaces the selected disk's layout with an ESP and three HFOC partitions | 161 MiB |
| Whole-disk erase, legacy BIOS | Replaces the selected disk's layout with a BIOS boot partition, a FAT32 boot volume and three HFOC partitions | 162 MiB |
| Alongside or free space | Adds three HFOC partitions in aligned, contiguous free space and reuses the existing ESP | 128 MiB |
| Manual, give up one partition | Removes the selected GPT partition and adds the three HFOC partitions in its extent, with the same shared ESP | 128 MiB |

The three HFOC partitions are system (64 MiB), persistence (32 MiB) and recovery (32 MiB). The clean UEFI ESP is 33 MiB. The legacy BIOS boot partition and FAT32 boot volume are 1 MiB and 33 MiB, respectively. For 512-byte logical sectors, the clean target must provide at least 331,810 sectors in UEFI mode or 333,858 in legacy BIOS mode, including alignment and GPT metadata. The preserving modes allocate 262,144 sectors to HFOC on a compatible disk, using its existing ESP.

For whole-disk erase, read the explicit warning before proceeding to the final summary. For the manual scheme, check the exact partition on the separate warning. The summary shows what will be erased, what will be created, whether the ESP is reused and which boot entries will be kept. Press `Enter` on the summary only after verifying the displayed plan. `Q` returns from the whole-disk warning or the final summary to the scheme list. `Q` on that list cancels the install and records `CANCELLED_BY_OPERATOR` with `writes=0`.

In legacy BIOS mode, Limine stage 1 goes at `LBA0`, stage 2 starts in the `Hah!IdontNeedEFI` BIOS boot partition, and the FAT32 boot volume holds Limine, the generated menu, kernel and sealed boot files. The installer verifies the stages, GPT and boot volume by read-back. The BIOS menu boots HFOC OS, and reports discovered EFI systems as `NO_ESP` when it cannot chainload them. Attempting to preserve an existing system in legacy BIOS mode gives `PRODUCTION_INSTALLER_BIOS_PRESERVE_UNSUPPORTED`.

## Install alongside Linux or Windows

Use **UEFI, GPT and exactly one suitable existing ESP** on the target disk. The ESP needs room for the HFOC boot payload. The installer reuses the ESP without formatting it, preserves vendor boot files and adds the HFOC menu. It may replace the standard UEFI fallback paths on that ESP with HFOC files. Where those paths already held files, the installer saves their bytes as `\EFI\HFOC\BOOTX64.PRE` and `\EFI\HFOC\LIMINE.PRE`. Confirm the plan shows `esp_reused=1` when checking serial records.

### Use free space

1. Boot the existing system and back it up. Create a contiguous, aligned unallocated region on the same GPT disk. The HFOC layout requires 128 MiB. Leaving at least 256 MiB unallocated when preparing a Fedora disk allows room for alignment. Free space inside an existing filesystem is separate from unallocated disk space.
2. If you need to shrink an existing system, use its supported storage tools and consult the filesystem and volume-manager procedure first. Shrink the filesystem before its containing partition where the stack supports it. When LVM is involved, handle the logical volumes and physical-volume layout before reducing the enclosing partition. Leave the existing ESP in place.
3. Boot the operator installer in UEFI mode. Select the disk, check that it found the existing operating system and select the alongside or free-space scheme. If the required contiguous range is missing, the installer reports the available and required sizes and refuses before writing.
4. Check in the summary that existing system and data partitions are preserved and the ESP is reused. Confirm only the plan you expected.

The installer identifies LVM and Btrfs partitions. It does not allocate HFOC filesystems inside LVM volume groups or Btrfs subvolumes. Discovery can list logical partitions inside an MBR extended container by their own extents and indices starting at 5. That discovery support does not make MBR a preserving install target.

### Give up one partition

On a full UEFI/GPT disk, the manual option can replace one operator-selected partition. This irreversibly erases the data on that partition. Choose it only if you have verified its contents are expendable and no operating system or storage volume you intend to keep depends on it. The installer shows the partition index, size, kind, owning system and layout, then asks you to confirm that partition. `Q` on the warning returns to the partition list. `Q` on the partition list returns to the scheme list without choosing a partition. Installation starts only after final plan confirmation. The writer replaces exactly that GPT entry and creates the three HFOC partitions in the freed extent. The shared ESP and other partition entries are retained. If the extent is too small or unsafe, the installer refuses before writing. The installer never chooses a partition to give up on its own.

### Check both boot paths

For Linux, verify that the existing Linux loader is still in the firmware or generated menu. For Windows, verify that the Windows Boot Manager and its existing EFI files and entry are present. After installation, remove the installer medium and boot the installed disk. Check the HFOC OS entry, then explicitly boot the preserved system and verify its data. The generated menu lists the systems with supported EFI loaders that discovery recognized. If detection is ambiguous or a known system cannot be accounted for in the plan, cancel and investigate before proceeding.

## Update and verify the installed system

The `update` declaration updates an existing HFOC installation in place using its existing layout. Specify it when building the update medium. Its preflight verifies the existing persistence range and state before writing. Use the same disk identity, the intended management scope and appropriate firmware mode. `production-installer-update-media` builds a disk-bound unattended update image, so review the declaration and use it only when ready for the update. An appliance installed with a private host entropy record requires the corresponding documented operator procedure. Keep personalized media private. The tag's `docs/hfoc-production-install-flow.md` describes standard and host-seeded media.

After an install or update, verify that the installer reported completion, boot from the installed disk without the installer attached, and look for `[BOOT-CHAIN] verdict=ATTESTED` and the normal readiness record. If enrollment was deferred, enroll the owner locally. Verify the management address, HTTPS and SSH access as applicable, and check that a file in `/home/owner` still has the expected contents after update and reboot. [First boot and management](operate.md)

If the installer refuses before presenting a disk selection, read the named reason on screen or in the serial record. Typical reasons include `FIRMWARE_UNSUPPORTED` for an unrecognized firmware mode, `NO_DISK`, `REPORT_UNAVAILABLE`, and `NETWORK_MALFORMED` or `FORWARD_MALFORMED` with their `*_INCOMPLETE` variants. Early operator refusals can remain on screen until a key is pressed or five minutes pass. Keep the reason and the evidence of any `writes=0` result when reporting the problem.

## What was verified

- Host and QEMU software proofs cover the layout arithmetic, read-only discovery, explicit confirmation, and the UEFI and SeaBIOS whole-disk paths. The preserving and partition-release QEMU cases use a synthetic foreign GPT disk, check the preserved partition and ESP data, and boot the installed HFOC disk without the installer. Linux and Windows menu records have software coverage.
- The installed-image QEMU tests verify the owner account, key identities and workspace persistence across reboot and a repeated state-preserving update. The release's exact-build Toshiba L875 check verifies installed boot, management, SSH, an HTTPS route, and file persistence across an ordinary reboot. That hardware run did not repeat the installation cycle. Physical dual boot, booting the preserved OS through the menu, and physical recovery have no acceptance claim here. [Release and hardware verification](release-1.0.0.md)
- A physical installation acceptance record needs the machine model, firmware version, CPU, RAM, disk and NIC, the test date, the HFOC revision or release ID, and the observed outcome. Use the template at `release/hfoc-hardware-acceptance-report.md` in the source tree.
