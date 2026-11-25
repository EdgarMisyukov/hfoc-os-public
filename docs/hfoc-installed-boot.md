# Booting the installed system from the disk (ITEM-397, ITEM-399, ITEM-405, ITEM-410)

**Item:** ITEM-397 of the HFOC Appliance Productization wave, extended by
ITEM-399 (the installed disk runs the product appliance kernel and states the
management scope its medium declared), by ITEM-405 (the ESP role the install
creates is formatted, and a volume that does not fit its partition is refused
instead of written into) and by ITEM-410 (the appliance writes its own boot log
into that ESP, so a hardware run can be read afterwards)
**Depends on:** ITEM-391 (production install layout contract), ITEM-394
(production install flow), ITEM-371 (signed media), ITEM-350 (ESP writer)
**Status:** implemented and proven by a host proof and a QEMU proof; the
firmware paths that only real hardware or a release signing key can close are
listed under "What this does not prove".

## Which kernel the installed disk runs (ITEM-399)

The installed boot payload set is written from the medium's *installed* kernel
module `boot/hfoc-kernel.elf`, which `scripts/hfoc-install-media-stage.sh`
copies from the product appliance build (`$(KERNEL)`), and never from the
installer kernel the medium itself boots
(`boot/hfoc-installer-kernel.elf`, `$(PRODUCTION_INSTALLER_KERNEL)`). Before
ITEM-399 the flow staged `executable_file_request` - the kernel that was
running, i.e. the installer - so `\boot\hfoc-kernel.elf` on the installed ESP
was an installer build with the install flow, the QEMU fixture and
`-DHFOC_PRODUCTION_INSTALL_MEDIA` / `-DHFOC_PRODUCTION_INSTALLER_QEMU_TEST` in
it, and an installed appliance printed `[PRODUCTION-INSTALLER] … proof OK` on
every boot and could install to a disk again.

An installed boot therefore publishes **no** `[PRODUCTION-INSTALLER]` record and
no QEMU-fixture record, and both QEMU proofs assert that absence.

## The installed configuration's command line (ITEM-399)

The configuration staged at `\boot\limine\limine.conf` is generated from the
sealed template `boot/hfoc-installed.conf` by
`scripts/hfoc-installed-config.py`, which adds exactly one line to the entry:

```
/HFOC OS 1.0.0
    protocol: limine
    path: boot():/boot/hfoc-kernel.elf
    cmdline: hfoc.management=local-only|management-interface
    module_path: boot():/boot/hfoc-boot.man
    module_path: boot():/boot/limine/limine.conf
```

ITEM-464 added a `resolution: 1024x768` line to the sealed template, but
ITEM-466 removed it: the installed entry asks for no mode of its own.  The
firmware/panel's preferred mode is what boots, and the kernel's `[FB]` record
names `preferred=`, the `available=` list, the mode the rule would choose if the
preference had been refused, the actual mode and the automatic font scale
(`scale=`/`cell_px=`/`cells=`).  `scripts/hfoc-installed-config.py` copies the
template verbatim, so a staged configuration that carries a `resolution:` line
again would be the hardcode the owner removed.

The scope is the operator's `HFOC-INSTALL-MANAGEMENT=` declaration on the
medium, written explicitly either way, so the installed appliance states its
management scope instead of relying on the kernel's deny-by-default value:

```
[PRODUCTION-MGMT-BIND] scope=LOCAL_ONLY source=boot-declaration
[PRODUCTION-MGMT-BIND] scope=MANAGEMENT_INTERFACE source=boot-declaration
```

When the ITEM-398 generated menu is the installed configuration, its appliance
entry carries the same `cmdline:` line, because the menu is what Limine reads
first.

## The gap this item closes

The ITEM-394 install flow writes the contracted layout, the ESP payload at
`\EFI\HFOC\BOOTX64.EFI` and the persistent state volume, and then published
the honest refusal:

```
[PRODUCTION-INSTALL] boot-entry state=UNAVAILABLE \
    reason=PRODUCTION_INSTALLER_EFI_UNAVAILABLE
```

The installed disk did not boot. Two things were missing:

1. the kernel and its boot configuration were never staged into the ESP: the
   ESP writer installed a single payload under a 512 KiB bound
   (`EFI_FAT32_ESP_PAYLOAD_MAX`) while the production kernel is about 8.4 MiB
   (8,395,368 bytes at the 2026-09-27 diagnostic build, most of it DWARF);
2. the running installer kernel has no EFI runtime, so it cannot create the
   `Boot####`/`BootOrder` entry a firmware needs for a fixed disk.

## The installed boot payload set

`efi_fat32_esp_install_payload_set()` stages a bounded set instead of one
payload, written by the production ESP formatter:

| Entry | Path | Bound |
| --- | --- | --- |
| HFOC boot payload | `\EFI\HFOC\BOOTX64.EFI` | `PRODUCTION_INSTALLER_BOOT_PAYLOAD_MAX` (512 KiB) |
| Boot configuration | `\boot\limine\limine.conf` | 64 KiB |
| Kernel | `\boot\hfoc-kernel.elf` | `EFI_FAT32_ESP_FILE_MAX` (16 MiB) |
| Loader (removable media) | `\EFI\BOOT\BOOTX64.EFI` | 1 MiB |
| Sealed boot manifest | `\boot\hfoc-boot.man` | 4 KiB |
| Root initramfs | `\boot\hfoc-rootfs.arc` | 1 MiB (`INITRAMFS_MAX_SIZE`) |

ITEM-636: the root initramfs is packed from the same staged tree `make iso`
uses (`build/rootfs-stage`): `rootfs/` plus the native ELF programs built from
`user/`, including `/bin/sh`.  The production media declare that tree through
`HFOC_INSTALL_MEDIA_ROOTFS`; the stage refuses a declared root without
`/bin/sh` before it writes the first staged byte, so an installed appliance
cannot silently come up with an empty `/bin`.  The artefact-level gate lives in
`scripts/test-qemu-production-install-flow-update.sh`, which extracts
`boot/hfoc-rootfs.arc` from the install and update media and requires `/bin/sh`
to be a real ELF file.

Every bound is explicit and enforced before the first write:

- `EFI_FAT32_ESP_PAYLOAD_SET_MAX` (7) entries per set;
- `EFI_FAT32_ESP_FILE_MAX` (16 MiB) per file;
- `EFI_FAT32_ESP_PAYLOAD_SET_BYTES_MAX` (16 MiB) for the whole set;
- `EFI_FAT32_ESP_PATH_MAX` (64) per canonical path, with a derived and
  collision-checked 8.3 alias per element and VFAT long-name chains, because
  `hfoc-kernel.elf`, `limine.conf` and `hfoc-boot.man` do not fit the 8.3 form
  and Limine matches a name that does not fit 8.3 case sensitively;
- `EFI_FAT32_ESP_FAT_SECTORS_MAX` (1024) for the FAT the formatter writes, and
  `EFI_FAT32_ESP_FAT_SCAN_SECTORS_MAX` (1024) for the free-cluster precheck.

`efi_fat32_esp_payload_requirement()` is pure arithmetic over the declared
geometry, and `production_installer_boot_stage_requirements()` compares it with
the ESP role the layout plan declares. An ESP that cannot hold the set is
refused with `PRODUCTION_INSTALLER_ESP_TOO_SMALL` **before the first write**,
so a partial install that cannot boot is impossible. Chain allocation, data
writes and FAT updates are done a sector at a time, so a multi-megabyte kernel
costs no proportional stack.

### Why the ESP role moved to 33 MiB

A volume is FAT32 only when it carries at least 65525 clusters
(`EFI_FAT32_ESP_MIN_FAT32_CLUSTERS`), and the installer writes one sector per
cluster. The previous 4096-sector (2 MiB) role could not be a valid FAT32
volume at all, and firmware that validates the BIOS parameter block refused to
mount it: OVMF enumerated the partition and then reported
`failed to load Boot0001 "UEFI QEMU HARDDISK" ... Not Found`. The first
attempt at this item (32768 sectors = 16 MiB) still carried only 32222
clusters, which is FAT16 territory. The declared role is now 67584 sectors
(33 MiB): the smallest whole-MiB role that satisfies the rule, with the FAT
size the formatter writes, and about 16 MiB of growth margin over the bounded
payload set. The formatter also writes a complete FAT32 BIOS parameter block
(total sectors, media descriptor, FSInfo sector, backup boot sector, volume
label and `FAT32` type string) and marks the root directory cluster allocated,
without which the first directory the installer created was handed the root
cluster itself.

### The volume has to fit the partition it lives in (ITEM-405)

A role is the *partition*; a volume is what its boot sector says it is, and the
two are not the same number until somebody checks. `efi_fat32_esp_bind()` now
reads the volume's own total-sector count (FAT32 offset 32, FAT16 offset 19)
and refuses a volume with `EFI_FAT32_ESP_UNSUPPORTED_GEOMETRY` when

- the count is zero - a boot sector that declares no extent is not a volume
  this writer may use;
- the count exceeds the partition it was handed; or
- the data region the FAT describes (`reserved_sectors + fat_count *
  sectors_per_fat + cluster_count * sectors_per_cluster`) does not fit inside
  the declared volume.

`cluster_count` is derived from the **smaller** of the two numbers, never from
the partition alone, so every cluster LBA the writer computes stays inside the
volume it describes - and a partition larger than the volume it holds still
binds, with the cluster space taken from the declared volume.

This is not a hypothetical: the hardware acceptance run installed onto a disk
that had carried a Windows layout. The partition table was replaced by ours, but
the 33 MiB ESP role still held the old system's FAT32 volume - OEM `MSDOS5.0`,
`reserved_sectors=6654`, two FATs, `sectors_per_fat=769`,
`sectors_per_cluster=2` and `total_sectors=204800` (the 100 MiB volume the
previous table had) - inside 67584 sectors. The old bind compared the boot
sector only with the partition length it was given, accepted that volume, never
formatted it, and staged the installed boot set against a geometry the volume
does not have. Linux could not even mount the result (`Seek to 104857088:
Invalid argument`), because the declared volume ran past the partition.

The rule is enforced at the bind, and the whole-disk path adds the other half:
see [which volume the install owns](#the-install-formats-the-role-it-creates-item-405)
below.

### The install formats the role it creates (ITEM-405)

`production_installer_esp_bind(..., format_owned)` says **who the role belongs
to**, not what the bytes in it happen to look like:

- `format_owned = true` - a whole-disk install created that role from the
  layout it planned. The role is formatted and then bound, whatever was in the
  range before: a zeroed area, a formatted volume, or the stale foreign FAT32 a
  replaced partition table left behind. Before this item the format happened
  only when the bind *failed*, which is how a stale volume that bound
  successfully was never formatted at all.
- `format_owned = false` - a shared ESP that belongs to a system already on the
  machine. The volume is bound as it is and is never formatted, whatever state
  it is in; a volume that does not fit its partition, or that this module cannot
  otherwise use, is refused before the first write.

## The product flow stages it

`production_install_flow.c` (ITEM-394) is the ordinary-boot entry point on the
installer medium, and it is the product path that stages the set:

- the medium carries the installed system's own boot set as Limine modules -
  `boot/hfoc-installed.conf` (staged as `\boot\limine\limine.conf`, whose
  `module_path` lines are the installed ESP paths `boot():/boot/hfoc-kernel.elf`,
  `boot():/boot/hfoc-boot.man` and `boot():/boot/limine/limine.conf`) and
  `boot/hfoc-installed.chain` (the sealed ITEM-372 chain of that kernel and
  configuration, staged as `\boot\hfoc-boot.man`). The ITEM-371 sealed media
  manifest keeps its declared component order - boot payload, loader
  configuration, kernel - with the *installed* configuration as its loader
  configuration, so the bytes the installer stages are the bytes the medium
  sealed;
- the flow calls `production_installer_boot_stage_requirements()` straight
  after the confirmation, before the layout write, so a disk whose ESP cannot
  hold the set is refused with zero writes;
- after the layout write it calls one `production_installer_boot_stage_run()`,
  which publishes the single boot-entry record (with the mechanism, the ESP
  geometry, the staged byte count and the sealed-media verification) and then
  initialises the persistent state volume.

### The generated menu is the installed configuration (ITEM-398, ITEM-409)

The boot stage takes the ESP explicitly - the role the plan declares for a
whole-disk install, the reused range for a preserving one - so a shared ESP is
bound as it is and never formatted. The flow passes a **generated menu**, and
that menu is what the loader reads: it is staged at `\EFI\BOOT\limine.conf`, the
configuration next to the EFI application Limine was booted from, which wins over
the sealed `\boot\limine\limine.conf` (staged verbatim beside it).

That "wins over" is measured, not assumed. Patching **only**
`\EFI\BOOT\limine.conf` on an installed disk - with an appliance entry that
declares `cmdline: hfoc.management=management-interface` where the sealed
configuration declares `local-only` - and booting that disk alone makes the
installed system publish `[PRODUCTION-MGMT-BIND] scope=MANAGEMENT_INTERFACE
source=boot-declaration`: the scope can only have come from the file the loader
read, so the loader read `\EFI\BOOT\limine.conf`.

The generated menu's appliance entry carries the same two `module_path` lines the
sealed installed configuration declares (`boot():/boot/hfoc-boot.man` and
`boot():/boot/limine/limine.conf`), which is what keeps the installed disk's own
chain verifiable - ITEM-372's `[BOOT-CHAIN] verdict=ATTESTED` still holds on the
installed disk - and, since ITEM-399, the same `cmdline:` line naming the
management scope the medium declared. ITEM-464 added a
`resolution: 1024x768` line to the Limine entry; ITEM-466 removed it again,
because the owner's rule is "the mode is the monitor's native/preferred one,
with a reasonable fallback to the largest available".  `kernel/src/installer_boot_menu.c`
no longer renders a mode line at all, so the generated menu and the sealed
template both leave the choice to the firmware and the kernel's own `[FB]`
record names what was chosen.

ITEM-409 makes the menu the installed configuration for **every** install, not
only for one that found a system to chainload:

* the appliance entry is written first, so it is Limine's default entry and a
  machine nobody touches boots this appliance;
* the configuration declares `timeout: 10` - ten seconds, so a person can read
  the menu and choose; the value is `INSTALLER_BOOT_MENU_TIMEOUT_SECONDS`, it is
  published in the boot-entry record as `timeout=10s` and pinned by the QEMU
  proofs, and a machine that carries only this appliance gets a menu with one
  entry (`entries=1`) rather than no menu at all;
* one `protocol: efi` entry per system the discovery found is added after it,
  chainloaded through the loader that is really on that system's ESP.  Where a
  vendor directory carries both a shim and a GRUB, the **shim** is chosen: it is
  the signed entry point the firmware boots, and a Secure Boot machine refuses
  the GRUB behind it;
* the sealed configuration is the fallback only for a menu that could not be
  built at all.  That case is a refusal in preserve mode; in whole-disk mode the
  run installs the sealed configuration and the boot-entry record says
  `source=SEALED menu_reason=MENU_UNAVAILABLE` instead of claiming a menu.

The flow publishes what the menu holds - and, when it holds nothing but this
appliance, which disk and which stage decided that - in one bounded record
before the first write, in both layout modes:

```
[PRODUCTION-INSTALL] boot-menu state=GENERATED reason=chainload-entry
  stage=chainload-entry entries=2 loaders=1 missing=0 unreadable=0 disks=2
  target=1 d1=NONE/esp0/NO_ESP/0 d2=GPT/esp1/OK/1
```

`docs/hfoc-production-install-flow.md` documents the record field by field.

Two paths a shared ESP usually already carries are taken over without losing
them, before the stage's first write: `\EFI\BOOT\BOOTX64.EFI` (the other
system's removable-media fallback loader) and `\EFI\BOOT\limine.conf` (the
foreign configuration that would shadow ours) are renamed to
`\EFI\HFOC\BOOTX64.PRE` / `\EFI\HFOC\LIMINE.PRE` - same clusters, same bytes -
and the record publishes `existing_loader=…`, `existing_loader_bytes=…`,
`existing_loader_kept_at=…`, `existing_config=…` and the same for the
configuration. `payload_set` counts what this run staged: six with the initramfs, or seven
with the generated menu - which every install but the sealed fallback has.
The initramfs is appended after the sealed five, so the sealed prefix and its
digest keep their declared order; the installed kernel mounts it as `/`
(ITEM-623).

A shared ESP that passes the declared-size gate but has no room for the set is
refused by the read-only free-cluster pre-flight, with zero writes.

Two QEMU proofs cover this, with a stated division:

| Proof | What it proves |
| --- | --- |
| `test-production-installer-qemu` -> `scripts/test-qemu-production-install-flow.sh` | the **product** path: the flow stages the set on a real (virtual) disk from the authorised medium, publishes exactly one boot-entry record with `mechanism=REMOVABLE_MEDIA_PATH` and `verify=SEALED_MEDIA`, and then **boots the disk the flow installed** with no installer medium attached, requiring the boot-chain attestation and `[APPLIANCE-BOOT] ... storage=READY` |
| `test-production-installer-qemu` -> `scripts/test-qemu-production-install-flow-preserve.sh` | the **preserving** path: the RED run (the declared whole-disk mode, which is the hardcoded mode the flow used to take) destroys a synthetic foreign machine through the flow, and the GREEN run keeps every foreign GPT entry, the MBR and the foreign ESP files byte for byte, stages exactly one installed set into the shared ESP, preserves the two occupied paths under their documented names and then boots the installed disk alone to `[BOOT-CHAIN] verdict=ATTESTED` and `[APPLIANCE-BOOT] ... storage=READY` |
| `test-production-installer-qemu` -> `scripts/test-qemu-installed-boot.sh` | the **writer and harness** path: the same entry points on a file-backed block device built from the release kernel and the sealed ISO boot set, including the ESP-too-small refusal with `writes=0` and a boot from that image alone |

## How the firmware finds the disk

`production_installer_boot_stage_run()` selects and publishes exactly one
mechanism:

- `mechanism=REMOVABLE_MEDIA_PATH` - the Limine loader is staged at
  `\EFI\BOOT\BOOTX64.EFI`, the path the UEFI specification requires a firmware
  to try when no boot entry matches. This is what the running installer kernel
  can guarantee: it has no EFI runtime and cannot write `Boot####`.
- `mechanism=EFI_BOOT_VARIABLE` - a writable EFI variable environment with a
  boot device path is bound, so the Boot#### entry and BootOrder are created
  through the ITEM-350 seam and verified.

Both are reported in one bounded record, so the recorded boot-entry state is
never confused with the state of the disk:

```
[PRODUCTION-INSTALL] boot-entry state=AVAILABLE mechanism=REMOVABLE_MEDIA_PATH \
path=\EFI\BOOT\BOOTX64.EFI kernel=\boot\hfoc-kernel.elf config=\boot\limine\limine.conf \
menu=\EFI\BOOT\limine.conf entries=1 timeout=10s source=GENERATED \
payload_set=6 required_sectors=15047 esp_first_lba=2048 esp_sectors=67584 \
sealed_bytes=7204027 sealed_digest=bbcd293f6ee9487b91c816798f2a14b6 \
existing_loader=FREE existing_config=FREE \
verify=SEALED_MEDIA variable=UNAVAILABLE boot_entry=NONE
```

A refusal publishes `state=UNAVAILABLE reason=<status>` with the requirement and
the declared ESP.

## What is verified

- The staged set is re-read from the finished ESP, compared byte for byte with
  the declared component bytes, and the SHA-256 over the sealed ITEM-371 source
  prefix (boot payload, loader configuration, kernel, in that order) is
  compared with the sealed media source digest the controller stored when it
  verified the media (`PRODUCTION_INSTALLER_MEDIA_UNVERIFIED` otherwise: the
  stage fails closed on a media that was never verified).
- The staged configuration is read back and must name the staged kernel by its
  real path (`path: boot():/boot/hfoc-kernel.elf`), or the stage refuses with
  `PRODUCTION_INSTALLER_BOOT_CONFIG_INVALID`. That configuration is the medium's
  sealed one - ITEM-399 generates it there, `cmdline:` line included - so the
  bytes the boot stage verifies are the bytes the ITEM-371 manifest and the
  ITEM-372 chain sealed, and the QEMU proof compares the configuration read out
  of the finished ESP with the medium's file byte for byte.

## Proofs

- `make test-efi-fat32-esp-payload-set-host` proves the requirement arithmetic
  at the installed kernel bound, the refusal before the first write, the
  collision and bound refusals, the staged bytes and long-name chains as raw
  directory bytes, re-install replacement and failure isolation.
- `make test-production-installer-boot-stage-host` proves the pre-write gate
  with no writes at all, the staged-and-verified set, the removable-media
  publication, the configuration reference check, the media bounds and the
  tampered-source and unverified-media refusals.
- `make test-installed-boot-disk-host` runs the production install entry points
  (contracted layout, persistent state volume, installed boot set) against a
  file-backed block device, re-verifies the staged set on the finished image
  and proves the small-ESP refusal with zero writes.
- `make test-efi-fat32-esp-host` proves the ITEM-405 bind rule directly: the
  acceptance run's stale boot sector (`total_sectors=204800` inside a
  67584-sector role) is refused with `EFI_FAT32_ESP_UNSUPPORTED_GEOMETRY`, a
  boot sector that declares no extent is refused, a volume whose FAT cannot
  address its declared data region is corrupt, and a volume that declares less
  than its partition binds with `cluster_count` derived from the declared
  volume - the payload is then installed and read back through it, and the
  sectors past the declared volume are untouched.
- `make test-production-installer-boot-stage-host` also proves both halves of
  the ownership rule: a whole-disk install stages into a role that carried the
  stale foreign volume and leaves its own `HFOC` geometry behind, while the same
  volume on a shared ESP is refused with `PRODUCTION_INSTALLER_ESP_UNSUITABLE`
  **with zero device writes**, keeps the foreign volume byte for byte, and
  publishes that refusal on serial and on the console mirror.
- `make test-production-installer-qemu` additionally runs the product flow
  proof (`scripts/test-qemu-production-install-flow.sh`, which installs through
  `production_install_flow` and then boots that disk) and
  `scripts/test-qemu-installed-boot.sh`: the latter builds the installed disk
  from the release kernel and the sealed ISO boot chain, boots QEMU from that
  image alone under OVMF (no installer media) and requires

  ```
  [BOOT] Bootloader: Limine 12.3.2
  [BOOT-CHAIN] verdict=ATTESTED reason=NONE release=1 signer=1 components=2
  [PRODUCTION-BOOT] status=APPLIANCE_PRODUCTION_BOOT_OK discovered=1 valid=1 \
      selected=AHCI storage=READY mode=NORMAL state=APPLIANCE_STATE_LOADER_OK
  [PMM ] status=OK ...
  [KBD ] state=READY reason=OK ...
  [PRODUCTION-MGMT-BIND] scope=LOCAL_ONLY
  [PRODUCTION-SERVING] v=1 state=...
  [UPDATE-ATOMICITY] update_lifecycle v=1 state=IDLE ...
  [AUDIT-INTEGRITY] audit_chain v=1 state=READY ...
  [APPLIANCE-BOOT] runtime=READY identity=OK config=DEFAULT_MISSING \
      local_management=READY storage=READY local_input=AVAILABLE
  ```

  and no kernel panic, plus the refusal record with `writes=0`.

  The product flow proof also prepares the acceptance host's target shape - the
  stale foreign FAT32 in the region the whole-disk layout declares as its ESP
  role - runs the declared whole-disk install over it, and then requires, out of
  the raw image and independently of the kernel, that the role carries the
  installer's own volume (OEM `HFOC`, `total_sectors` equal to the role length,
  32 reserved sectors, two FATs, one sector per cluster) and that the old tree
  is not reachable through it, followed by the same staged-set comparison and
  the same standalone boot as every other installed disk.

  The product flow proof boots the disks it installed from the two authorised
  media and requires, for each: no `[PRODUCTION-INSTALLER]` record at all (the
  installed kernel is the product appliance, ITEM-399), `[BOOT-CHAIN]
  verdict=ATTESTED`, `[PRODUCTION-BOOT] … storage=READY`, exactly one
  `[APPLIANCE-BOOT] runtime=READY … storage=READY`, the `[PRODUCTION-MGMT-BIND]`
  scope the medium declared with `source=boot-declaration`, and both files the
  ESP carries read out of the raw image and compared with the medium's own: the
  staged kernel is the medium's product kernel byte for byte and carries no
  installer proof marker, and the staged configuration is the medium's sealed
  configuration byte for byte.

  An unattended install leaves the account unset, so that boot withholds the
  management listener: its `[PRODUCTION-SERVING]` record reads
  `state=AWAITING_CREDENTIAL reason=admin_credential_unset listener=down` until
  the operator sets the account on the console, and the listener comes up on
  the next poll after `[ADMIN-ENROLLMENT] credentials changed` (ITEM-506
  increment 5).  A medium installed with the operator's account reaches
  `state=LISTENING` directly.

- The `[KBD ]` record of that boot is fed by whichever local keyboard the
  machine has, and since ITEM-406/ITEM-452 the USB path enumerates every xHCI
  controller the machine carries and every root port of each instead of the
  first one that answers: each visited controller publishes its own `[XHCI]`
  record (`state=`, `reason=`, `detail=`, `pci=bb:dd.f`, `controllers=N`,
  `taken=YES|NO`, then bar/ports/slots/ctx/vendor/device, the port evidence
  when no role is bound there, and `hid=…` when one is), the rejection `detail`
  names the controller, the port and the stage, and nothing ends the walk
  before the last controller - the pointer and the keyboard may sit on two
  different controllers and both stay bound at the same time. Every connected
  device also gets its own `[USB ] pci=… port=… speed=… role=… state=… reason=…
  [vid=… pid=… ifaces=…]` line, so a device that is refused still names itself.
  An installed system booting on the acceptance host therefore records which
  controller and port each device came from instead of publishing
  `local_input=UNAVAILABLE` (or `[MOUSE] … seen=0`) with the hardware plugged
  in.

## The appliance writes its own boot log (ITEM-410)

Every hardware iteration used to end with photographs of a 16-line screen. The
records the kernel publishes about an install, the network, the local input path
and its own readiness already reach the serial stream - which the acceptance host
cannot capture - and the framebuffer console, where one record scrolling away
costs an iteration. The appliance owns a disk, so the log now survives the boot
on it.

### Where the log lives, and why there

`\EFI\HFOC\BOOTLOG.BIN` on the EFI System Partition, next to the boot payload
and the sealed boot manifest this install already owns.

The alternative - a dedicated region inside the appliance's own persistence
partition - was rejected deliberately. That volume is the state volume: it can be
sealed by ITEM-373, it is not a filesystem a host can mount, and reading it needs
HFOC software and possibly a key. A log whose whole purpose is to explain a
machine that did not come up must be readable by a person with a USB stick and a
laptop, and a file on the ESP is readable from any Linux with `mount`, and from a
raw image with `scripts/hfoc-esp-read.py`.

Writing into a shared ESP is only acceptable because the log never touches bytes
that are not ours:

- it attaches to an ESP only after a **read-only probe** finds this appliance's
  own `\boot\hfoc-boot.man` there. An ESP without it belongs to somebody else
  and is never written - a Windows ESP on the same machine is probed and left
  exactly as it is;
- on the installer medium the log is **not attached at boot at all**: the medium
  is not the declared target. The install flow attaches it to the disk it just
  installed to, after the boot stage, so the install records land on the disk the
  operator will boot from;
- the file is **created once and preallocated** to its whole extent
  (`efi_fat32_esp_create_fixed_file()`). Every later flush overwrites slots
  inside that extent: the file allocation table, the directory and every other
  file on the volume are never touched again. The host proof asserts exactly that
  - after the creation, a flush changes no byte of the FAT, no byte of a
  directory and no byte outside the log's own clusters;
- a file that already has the declared size and chain is **reused without a
  single write**, which is what makes the second boot append instead of starting
  over.

### Format

The file is exactly 262144 bytes (256 KiB, a whole number of clusters at any FAT
cluster size): 512 slots of 512 bytes. Slot 0 is the header, slots 1..511 are the
records. Every slot is one ASCII line closed by `\n` and padded with NUL bytes,
so a plain `cat` of the file on a mounted ESP already shows the sequence numbers
and the text.

```
HFOC-BOOTLOG v=1 slot=512 slots=512 records=511 gen=0000000001 next=00000000000000000042 cursor=0000000007 dropped=0000000000 written=00000000000000000036 crc=856FD014
@00000000000000000041 g0000000000 t00000000000000123456 l047 cEE568417 [PRODUCTION-DHCP] state=BOUND lease=ACQUIRED
```

- `gen` is the wrap generation, `next` the next sequence number to assign,
  `cursor` the next record slot, `written` how many records ever reached the
  extent and `dropped` how many the bounded staging ring lost before a flush.
- A record carries its own sequence number, generation, tick stamp, text length
  and a CRC-32 over the fields *and* the text, so a torn write is detected and is
  never printed as text.
- **Rotation** is a bounded wrap: record N goes to slot `1 + (cursor % 511)`, the
  cursor advances per record, and when it passes the last slot it returns to the
  first and `gen` increments. The generation is what separates the newest pass
  from the leftovers of the previous one.

### Flushing

Records are staged in a bounded 64-entry RAM ring as they are published, and the
ring is written at the moments that matter, never once per record:

- the end of boot (the `[APPLIANCE-BOOT]` readiness record);
- after DHCP and after DNS resolve (`[PRODUCTION-DHCP]`, `[PRODUCTION-DNS]`);
- after the serving path publishes (`[PRODUCTION-SERVING]`) and after the
  management binding (`[PRODUCTION-MGMT-BIND]`), the TLS lifecycle, the audit
  chain and the install records;
- on every record that announces a refusal or a failure (`state=UNAVAILABLE`,
  `state=REFUSED`, `state=FAILED`, `state=ERROR`, `verdict=REFUSED`), so a machine
  that dies right after a refusal still has the reason on disk;
- on the **panic path**, before the machine stops - a panic whose last records
  are missing is the least useful case there is.

Every flush is best effort. A log that cannot be written publishes

```
[BOOTLOG] state=UNAVAILABLE reason=<status>
```

once and the boot continues; the records stay staged and the next flush tries
again. Nothing in the log path can fail, delay past one bounded flush or refuse a
boot.

The state record of a working log is:

```
[BOOTLOG] state=READY path=\EFI\HFOC\BOOTLOG.BIN esp_first_lba=2048 esp_sectors=67584 file_bytes=262144 slot=512 slots=512 records=511 reused=1 gen=0 next=12 written=12 dropped=0 cursor=12 flushed=4 reason=readiness
```

`reused=1` says the extent already existed and was appended to; `reused=0` says
this boot created it. The record is written to the serial stream and mirrored on
the console, like every other record this kernel owns.

### Reading it

Three ways, in increasing order of convenience:

```
# 1. a raw image or the acceptance disk, with sequence numbers (no mount needed)
python3 scripts/hfoc-esp-read.py /dev/sda --bootlog

# 2. a mounted ESP - the two-line recipe
mount /dev/sda1 /mnt
python3 scripts/hfoc-esp-read.py --bootlog-file /mnt/EFI/HFOC/BOOTLOG.BIN

# 3. the raw bytes, if all you have is a shell
tr -d '\000' </mnt/EFI/HFOC/BOOTLOG.BIN | sed -n '1,40p'
```

The reader prints a summary line (`bytes=`, `slots=`, `records=`, `header=`,
`gen=`, `wrapped=`, `oldest=`, `newest=`, `shown=`, `dropped=`, `written=`)
followed by one line per record, oldest first:

```
boot-log: path=\EFI\HFOC\BOOTLOG.BIN bytes=262144 slot=512 slots=512 records=511 header=OK gen=1 wrapped=YES oldest=73 newest=583 shown=511 dropped=0 written=584
#00000000000000000073 gen=0 ticks=525 [PRODUCTION-INSTALL] boot-entry state=AVAILABLE
#00000000000000000074 gen=0 ticks=532 [BOOT-CHAIN] verdict=ATTESTED reason=NONE
...
```

It never crashes and never prints garbage: a truncated tail, a torn slot, a log
whose header claims more than the file holds, a foreign file and random bytes are
all reported honestly (warnings on stderr, exit 2 for "not an HFOC boot log"),
and the interactive console prints the staged records from RAM - the command that
explains why a run refused does not depend on the write path that refused:

```
hfoc> BOOTLOG
```

The local console also hands every record it accepts to the log as it is
published, so the log holds exactly the bounded, printable lines the operator
read.

A healthy log that has wrapped prints warnings, and they are expected rather than
defects: the header's `written` counts every record the extent ever received, so
on a wrapped log it is larger than the 511 slots that carry data, and the records
the previous pass left behind are counted as "outside the shown run". The
summary's `wrapped=YES` and the per-record `gen=` are what separate the newest
pass from those leftovers; neither warning changes the exit status.

### Proofs

- `make test-appliance-boot-log-host` proves the format, the checksum, the
  cursor and wrap arithmetic, the staging ring and its overflow accounting, the
  flush policy, the newest-contiguous-run reconstruction a reader performs, and
  that a torn slot is never text.
- `make test-appliance-boot-log-store-host` builds a real GPT + FAT32 image, runs
  the store against a file-backed block device and proves: the extent is created
  once; a later flush changes no byte of the FAT, of a directory or of another
  file; the second boot reuses the extent and continues the sequence; more
  records than the extent holds wrap it, keep the sequence counting and mark a new
  generation; an ESP that is not ours and a disk with no ESP are never written;
  and a device that refuses a write publishes `state=UNAVAILABLE` while the next
  flush still writes what was staged.
- `make test-appliance-boot-log-reader-host` proves the host reader against
  synthetic wrapped, truncated, over-claiming, torn and foreign logs, and reads
  the log the C proof wrote out of the real image.
- `make test-production-installer-qemu` reads the log out of the raw image after
  the disk has booted and requires the records in one sequence: its product-flow
  phase needs the install records and the boot's own records
  (`[PRODUCTION-INSTALL] boot-entry state=AVAILABLE …`,
  `[BOOT-CHAIN] verdict=ATTESTED`, `[PRODUCTION-BOOT] … storage=READY`,
  `[APPLIANCE-BOOT] runtime=READY …`), and its installed-disk phase
  (`scripts/test-qemu-installed-boot.sh`) needs the same for an image installed
  without the medium.  Both wait for the log's own completion record - which is
  published after the device flush - before they stop QEMU, so the read-back is
  deterministic rather than a race with the kill.

### What a hardware run must confirm

- that the log's ESP probe finds the acceptance disk's ESP (the QEMU proof runs
  one disk; the acceptance host runs the disk the install declared, and the
  identity guard is what keeps that the right disk);
- that a real firmware boot of a real ESP leaves the file where the reader
  expects it - QEMU's OVMF and the kernel's own FAT32 writer are the same code
  path, but a foreign ESP that a real machine already carries is only exercised
  by the hardware run;
- the wall-clock cost of the first creation (512 zeroed clusters) on real SATA
  hardware, and of a flush after a DHCP lease. Both are bounded and best effort
  by construction; the hardware run is where the numbers become facts.

## What this does not prove

- **Real firmware and Secure Boot.** The proof runs one firmware (OVMF from
  Debian's EDK II). Firmware that ignores the removable-media path on a fixed
  disk, that requires the boot entry (which the running kernel cannot create),
  or that enforces Secure Boot will not boot this disk until a release-signed
  loader and the `EFI_BOOT_VARIABLE` mechanism (the ITEM-350 helper, running
  under real firmware) are used. The install record publishes which mechanism
  was used, so a hardware run can tell the two cases apart.
- **BIOS/CSM boot.** The ESP is prepared for UEFI only.
- **The management plane over the wire from an installed disk.** The boot
  publishes the management records, but this QEMU configuration leaves the
  virtio-net link down with a user-mode netdev, so the appliance honestly
  reports `state=AWAITING_NETWORK`. The on-the-wire management path is proven
  by `test-production-network-qemu` and the management-plane acceptance from
  installer media, not from this image.
- **A physical run on the acceptance host.** `FIRST_BOOT` and `STORAGE_STATE`
  become observable on hardware with this item: the acceptance matrix now names
  this proof in both stages' observations, and the physical run reboots the
  machine from `/dev/sda` after the install step and records the readiness
  record it prints. The hardware observation itself stays human-triggered
  (`docs/hfoc-hardware-acceptance.md`).
- **Cross-disk booting (ITEM-398, owner Case B).** The installed configuration
  is proven to *carry* a chainload entry for a system that lives on the other
  disk's ESP, named with Limine's `guid(<GPT partition GUID>)` resource, and the
  other disk is proven to be read and never written (its write counter is
  unchanged after the install). That a firmware actually follows that entry into
  the other disk's loader stays unproven until the hardware acceptance run on the
  two-disk machine; QEMU here provides no second bootable system to follow it
  into.

ITEM-449: the same installed entry also carries the forward the medium declared
(`hfoc.forward=on hfoc.forward.target=… hfoc.forward.target-port=…
hfoc.forward.listen-port=… hfoc.forward.clients=…`, or `hfoc.forward=off`).
The appliance validates it at boot against the one forward owner and publishes
`[PRODUCTION-FORWARD] state=DECLARED …`, the refusal that names what was wrong,
or `state=UNAVAILABLE reason=NO_DECLARATION` when the entry declares nothing;
the client list is the access rule, and the declaration itself opens no port.
