# HFOC Installer: Preserving Existing Systems

**Date:** 2026-09-12
**Target:** ITEM-396 of
`docs/superpowers/plans/2026-09-11-appliance-productization-queue.md`
**Base SHA:** `579a1d42e0923c7f3c3e301115b68e5b0570e54b`
**Prerequisites:** ITEM-341 dual-boot planning, ITEM-351 production installer,
ITEM-391 production install layout contract
**Status:** Implemented by ITEM-396

## Purpose

Before the first physical install the owner asked directly: *will the installer
wipe Debian's boot record?* The verified answer at the time was **yes**. This
document is the authoritative ITEM-396 boundary for the answer being **no**: an
install on a disk that already carries an operating system keeps that system's
partition entries, its EFI System Partition contents and its boot record intact,
adds the appliance's four roles next to them, and writes a Limine menu that
still lists the system that was there.

## The bug this closes (RED evidence)

The only executable partition-table writer before this item was
`installer_layout_execute()` (`kernel/src/installer_layout.c`). It builds the
table *from the plan*: `installer_layout_build_partition_array()` zeroes the
entry array and writes only `plan->partitions[]`, at most
`INSTALLER_LAYOUT_PARTITION_CAPACITY` entries (four for a UEFI plan, five for a
legacy-BIOS plan since ITEM-517), and
`installer_layout_build_mbr()` zeroes the MBR before writing a fresh protective
entry. Foreign entries cannot pass through it, and neither can the bytes in
MBR sectors 0..445.

The reproduction used a synthetic disk carrying a Debian table (shared ESP at
LBA 2048, Debian root at LBA 4096, a second Debian entry at LBA 5000000) and ran
the ITEM-391 production path over it - the same calls
`production_installer_execute_layout()` makes:

```
BEFORE install (foreign Debian GPT):
  before slot=0 first_lba=2048 last_lba=6143 type=28732ac1
  before slot=1 first_lba=4096 last_lba=1004095 type=af3dc60f
  before slot=2 first_lba=5000000 last_lba=5099999 type=af3dc60f
plan_clean: INSTALLER_LAYOUT_CONTRACT_OK
confirm: INSTALLER_LAYOUT_OK
execute: INSTALLER_LAYOUT_OK
AFTER install (the production path):
  after slot=0 first_lba=2048 last_lba=6143 type=28732ac1
  after slot=1 first_lba=6144 last_lba=137215 type=8d0b19a2
  after slot=2 first_lba=137216 last_lba=202751 type=e144014c
  after slot=3 first_lba=202752 last_lba=268287 type=2be5716d
MBR bytes 0..445 changed: YES (boot record wiped)
```

The Debian root entry (`af3dc60f`, Linux filesystem data) is replaced by the
appliance's `hfoc-system` role, the second Debian entry is deleted outright, and
the boot code and disk signature in MBR bytes 0..445 are zeroed. The ESP entry
only survives because the clean plan happens to put its own ESP at the same LBA.

## What ITEM-396 adds

### `installer_gpt_preserve` - the writer that preserves

`kernel/include/installer_gpt_preserve.h`, `kernel/src/installer_gpt_preserve.c`.

* `installer_gpt_preserve_read()` validates a target's protective MBR, primary
  header and array, backup header and array: signatures, header and array CRCs,
  `my_lba`/`alternate_lba`/array geometry, the usable range, the protective MBR
  entry, every present entry's extent, and that the primary array is
  byte-identical to the backup array. An I/O failure is
  `UNREADABLE_TABLE`, a malformed table is `CORRUPT_TABLE`, and a table that is
  not 128 entries of 128 bytes is `UNSUPPORTED_TABLE` - the writer's documented
  preserved-entry bound, the GPT's own. It never "assumes empty".
* A slot is **empty** only when all 128 of its bytes are zero. Everything else
  is a foreign entry and is preserved byte-for-byte: the writer only ever
  writes slots it read as all-zero. This is what makes byte-identity mechanical
  rather than best-effort, and it is why a caller cannot lose an entry it did
  not know about.
* `installer_gpt_preserve_check()` places the appliance's roles in the lowest
  all-zero slots (or back into the slots this install wrote before, recognised
  by their type and unique GUID, which makes a re-run idempotent), and refuses,
  before any write: `EXISTING_SYSTEM_OVERLAP` when a role touches a preserved
  entry or when the reused ESP range is not the entry the plan believed it was,
  `NO_FREE_SPACE` when the declared layout does not fit one free region,
  `OUT_OF_RANGE` outside the usable region, `CAPACITY` when the table cannot
  hold the entries, `BAD_STATE` for the destructive clean plan (that path keeps
  its own writer) and for a table that was not completely read.
* `installer_gpt_preserve_write()` writes the backup array and header first, then
  the primary array and header, then the validated protective MBR - each sector
  read back and compared. A failure therefore always leaves at least one
  complete, self-consistent table that still contains every foreign entry, and
  the operation can be re-run to completion.
* The appliance's entries carry the identity `installer_layout` itself writes
  (`installer_layout_role_type_guid()`, `installer_layout_role_unique_guid()`),
  so the two writers cannot drift apart.
* The preserved-entry capacity is the GPT's 128 entries;
  `INSTALLER_LAYOUT_PARTITION_CAPACITY` (5 since ITEM-517: four UEFI roles or
  the five legacy-BIOS roles) stays the bound of *our* roles.  The
  appliance's own reader (`appliance_gpt_find_hfoc_partition()`) reads such a
  table: the entry count is the array's slot count, bounded by the GPT maximum,
  while the number of *present* partitions is what its ranges array bounds - a
  disk written by any other tool carries the 128-slot array, and the appliance
  has to find its persistence volume on it.

### `installer_boot_menu` - the menu that keeps the old system selectable

`kernel/include/installer_boot_menu.h`, `kernel/src/installer_boot_menu.c`.

The menu is built from the discovery report, the shared ESP's loader inventory
and the appliance's own paths, and renders the Limine configuration:

```
timeout: 5

/HFOC OS
    protocol: limine
    path: boot():/boot/hfoc-kernel.elf

/Debian GNU/Linux
    protocol: efi
    path: boot():/EFI/DEBIAN/GRUBX64.EFI

/HFOC OS (recovery)
    protocol: limine
    path: boot():/boot/hfoc-recovery.elf
```

A discovered system is chainloaded through `protocol: efi` and the path of the
loader that is really on the ESP.

Owner refinement (2026-09-13): the menu offers systems on **other disks** too.
`installer_boot_menu_scan()` reads every discovered disk's ESP - read-only, from
end to end - and names it with Limine's `guid(<GPT partition GUID>)` resource
([Limine CONFIG.md, "Paths"](https://github.com/Limine-Bootloader/Limine/blob/v12.x/CONFIG.md)),
because a firmware drive number is not stable; the target disk's own ESP keeps
`boot()`, since that is where the configuration lives. An ESP that cannot be
read, holds no recognised loader, or belongs to a disk this run has no device
for is recorded with its scan status (`NO_DEVICE`, `NO_ESP`, `UNREADABLE`,
`CORRUPT`, `UNSUPPORTED`, `NO_LOADER`) and published per disk; the menu still
comes up and carries a comment naming what it could not offer, so the existing
system is never dropped in silence. A BIOS-legacy target no longer refuses:
ITEM-517 installs the BIOS chain (BIOS boot partition plus FAT boot volume,
no ESP) and the BIOS menu records every system and disk it cannot offer as
`NO_ESP`, because BIOS Limine cannot chainload an EFI application. A BIOS
*preserving* install is refused by name
(`PRODUCTION_INSTALLER_BIOS_PRESERVE_UNSUPPORTED`), because this planner
reuses the previous system's ESP.

Owner refinement (2026-09-14, ITEM-409): the menu is the installed configuration
for **every** install, and the run states what it offers on the console.

* The generated menu used to be installed only when it carried a `protocol: efi`
  entry.  A machine that carried nothing but this appliance therefore booted the
  sealed single-entry configuration - no menu at all - and the 2026-09-14
  hardware run showed exactly that (`menu=NONE`, one entry) with nothing in the
  run saying which disk or which stage had decided it.  The menu is installed
  always now: the appliance entry first (Limine's default), `timeout: 10` so a
  person can read it and choose, and one chainload entry per discovered system
  after it.  The sealed configuration is the fallback only for a menu that could
  not be built at all, and the record then says `source=SEALED
  menu_reason=MENU_UNAVAILABLE`.
* A loader is chosen by the vendor directory and by name in the order a firmware
  boots: the **shim** (`SHIMX64.EFI`) before `GRUBX64.EFI`, then the
  removable-media fallback, then the Windows boot manager.  A real
  `\EFI\debian` carries both files, and the shim is the signed entry point; an
  entry that named the GRUB behind it is an entry a Secure Boot machine refuses.
* One bounded `[PRODUCTION-INSTALL] boot-menu` record, published before the first
  write in both layout modes and mirrored to the framebuffer, states the state
  (`GENERATED|SEALED`), the reason (`chainload-entry|appliance-only|
  menu-unavailable`), the stage that decided when the menu offers nothing but
  this appliance (`discovery-read`, `partition-table`, `esp-read`,
  `loader-inventory`, `device-lookup`, `entry-capacity`, `target-erased`,
  `no-other-disk`), the counts, and - per disk - the partition table the
  discovery found, whether an ESP was classified, what the read-only scan made
  of it, how many loaders it holds, and the discovery's own status for a disk
  whose partition table could not be read.  Field-by-field:
  `docs/hfoc-production-install-flow.md`.
* The discovery's per-disk status is no longer discarded by the flow
  (`production_install_flow_report_build()` used to `(void)` it).  A disk the
  storage layer enumerated whose sectors do not read is now published as such,
  next to the disk, instead of appearing as a candidate with no partitions.

The two cases that define "done" (owner, 2026-09-13):

* **Case A** - Fedora already on the disk, HFOC installed beside it: Fedora's
  partitions and its `EFI/fedora` (or shim) files stay intact, the shared ESP is
  reused, and the menu lists both.
* **Case B** - Debian on the host's `nvme0n1`, HFOC on the dedicated `/dev/sda`:
  one menu lists both, Debian's loader is chainloaded from the other disk through
  `guid(...)`, and `nvme0n1` is never written - it is read, and only read. A
  firmware boot entry, if one is created, lives in NVRAM and never on that disk.

The acceptance disk's own prior content is expendable (owner decision
2026-09-13), so preserving it is a customer-facing feature rather than a
prerequisite for our own hardware run.

### ESP reuse without deleting foreign files

`efi_fat32_esp` gains three read-only/one-write entry points:

* `efi_fat32_esp_list_loaders()` - bounded inventory of the `\EFI\<vendor>`
  directories and their UEFI loaders, excluding the appliance's own `\EFI\HFOC`;
* `efi_fat32_esp_free_clusters()` - bounded free-cluster count (FAT scan capped
  at `EFI_FAT32_ESP_FREE_SCAN_MAX_SECTORS`);
* `efi_fat32_esp_install_config()` / `_verify_config()` / `_read_config()` -
  the Limine configuration as a second file in `\EFI\HFOC`, written through the
  same allocate/write/rollback path as the boot payload.

`production_installer_esp_install_shared()` binds the existing volume and
**never formats it**: an ESP this module cannot use is
`PRODUCTION_INSTALLER_ESP_UNSUITABLE`, and the free-cluster count is read first,
so an ESP that cannot hold both files is `PRODUCTION_INSTALLER_CAPACITY` before
the first write. Only `\EFI\HFOC\BOOTX64.EFI` and `\EFI\HFOC\LIMINE.CONF` are
written; a foreign `\EFI\<vendor>` directory is never touched.

ITEM-405 keeps that rule and states who the rule is about. The boot stage passes
`format_owned = !esp_target->reused` to `production_installer_esp_bind()`: a
shared ESP - the volume of the system already on the machine - is bound exactly
as it is and is never formatted, whatever state it is in, so a shared ESP whose
boot sector describes an extent its partition does not have (the stale-volume
shape a replaced partition table leaves behind) is refused with
`PRODUCTION_INSTALLER_ESP_UNSUITABLE` **before the first write**, and the volume
keeps its bytes. The role a whole-disk install creates is the other case: it
belongs to this install, so it is formatted before it is bound and the writer
can never inherit a foreign geometry.

### Production entry points

`kernel/include/production_installer_preserve.h`,
`kernel/src/production_installer_preserve.c`:

1. `production_installer_plan_preserve()` - menu and preservation summary, after
   `production_installer_plan_layout()` in `PRODUCTION_INSTALLER_MODE_DUAL_BOOT`;
2. `production_installer_publish_preserve_record()` - the machine-readable plan
   and the Limine configuration, published inside one serial sequence before
   the first write;
3. `production_installer_execute_layout_preserving()` - the preserving write
   through the shared controller state machine;
4. the installed boot set is staged into the reused ESP by the ITEM-397 boot
   stage (`production_installer_boot_stage_run()` with the reused range), which
   is the only writer of those files. `production_installer_esp_install_shared()`
   no longer exists: one ESP writer serves both install modes.

`production_installer_plan_layout()` in dual-boot mode now also fills the
executable `installer_layout_plan`: the three roles the appliance adds, with the
reused ESP deliberately absent from it and `destructive = false`.

### Which path the product flow takes (ITEM-398)

`kernel/src/production_install_flow.c` no longer hardcodes the destructive mode.
The layout is a declaration (`HFOC-INSTALL-LAYOUT=whole-disk|preserve|update`,
the seventh argument of `scripts/hfoc-install-media-stage.sh` - ITEM-399 inserted the
medium's two kernels in front of it - and the
`HFOC_INSTALL_MEDIA_LAYOUT` Makefile variable), and without one the run decides
from the target disk and publishes the decision before the first write:

```
[PRODUCTION-INSTALL] layout mode=preserve|whole-disk|update|release reason=… declared=0|1 [released_index=N] chosen=none|alongside|free-space|erase|manual
```

`reason=existing-system` (a partition that is not an ESP, or a system the
discovery named), `reason=foreign-loader` (a loader that is not ours on the
target ESP), `reason=declared-preserve`, `reason=operator-alongside` and
`reason=operator-free-space` take the preserving path; `reason=no-existing-system`,
`reason=declared-whole-disk` and `reason=operator-erase` take the whole-disk
path; `reason=operator-release` (ITEM-501, chosen as the manual scheme since
ITEM-514) is the third mode, `mode=release`.  ITEM-514 puts the mode on the
scheme screen and names both sources: `chosen=none` means the declaration or
the disk decided, `chosen=alongside|free-space|erase|manual` means the operator
did.  Nothing is written before that record.

On the preserving path the flow runs the entry points above in the product
order - dual-boot plan, menu and inventory, reuse request, read-only GPT read,
`[INSTALLER-PRESERVE]`/`[INSTALLER-BOOT-MENU]` publication - before the
confirmation and before the first write. The ~12 KiB table, the menu and the
inventory are file-scope statics in that translation unit, so the 16 KiB boot
stack does not grow.

The menu is planned in both layout modes, because the system that has to stay
selectable may not be on the target disk at all. A preserving install offers the
target disk's systems through `boot()` and the other disks' through
`guid(<GPT partition GUID>)`; a whole-disk install erases its target, so the
target contributes no loader and only the other disks' systems are offered -
offering what the run destroys would be worse than offering nothing. Other disks
are only ever read (`production_installer_plan_preserve()`'s scan binds each ESP,
lists its loaders and writes nothing), which is what the owner's two-disk case
requires.

The paths the installed set takes over are handled by the boot stage before its
first byte: `\EFI\BOOT\BOOTX64.EFI` and `\EFI\BOOT\limine.conf` are renamed to
`\EFI\HFOC\BOOTX64.PRE` / `\EFI\HFOC\LIMINE.PRE` (same clusters, same bytes) and
the boot-entry record names the size and the new name of each. The long-name
entries that spell those names move with the entry they belong to: a reader binds
such a run to the short entry that follows it, so a run left behind would name
whatever entry took the path - a firmware looking for `limine.conf` would read
this appliance's own loader as its configuration. The path the
install takes over on the target disk is not offered as a menu entry, because
after the install it holds this appliance's own loader; the same path on another
disk is left alone and stays selectable.

### Publication

```
[INSTALLER-PRESERVE] plan mode=dual-boot disk=1 table=gpt entry_count=128
  preserved=4 added=3 esp_reused=1 esp_index=0 preserved_first_lba=2048
  preserved_lba=... free_first_lba=... free_lba=... menu_entries=3
[INSTALLER-PRESERVE] slot index=0 origin=preserved role=efi-system reused=1
  first_lba=2048 length_lba=4096
[INSTALLER-PRESERVE] slot index=4 origin=added role=hfoc-system reused=0
  first_lba=... length_lba=...
[INSTALLER-BOOT-MENU] menu firmware=uefi entries=3 loaders=1
[INSTALLER-BOOT-MENU] entry index=1 protocol=efi label=Debian GNU/Linux
  path=boot():/EFI/DEBIAN/GRUBX64.EFI
[INSTALLER-BOOT-MENU] entry index=0 protocol=limine label=HFOC OS
  path=boot():/boot/hfoc-kernel.elf modules=2 cmdline=hfoc.management=local-only
[INSTALLER-BOOT-MENU] config-begin
timeout: 5
...
[INSTALLER-BOOT-MENU] config-end
```

ITEM-399 gives the appliance entry a `cmdline:` line, and the record publishes it
as `cmdline=…`: the menu is the installed configuration whenever a system has to
stay selectable, and the installed appliance states the management scope its
medium declared instead of falling back to the kernel's default. The scope is
`hfoc.management=management-interface` when the operator declared the management
interface on the medium and `hfoc.management=local-only` otherwise - explicit
either way.

The hardware acceptance `INSTALLER` and `DUAL_BOOT_UNINSTALL` stages record these
lines; the plan preview additionally carries
`preserve preserved=4 added=3 esp_reused=1 free_first_lba=... free_lba=...
menu_entries=3`.

New exact statuses: `PRODUCTION_INSTALLER_TABLE_UNREADABLE`,
`PRODUCTION_INSTALLER_TABLE_CORRUPT`, `PRODUCTION_INSTALLER_NO_FREE_SPACE`,
`PRODUCTION_INSTALLER_ESP_UNSUITABLE`,
`PRODUCTION_INSTALLER_MENU_UNAVAILABLE`.

## What the operator sees (ITEM-405)

Every `[PRODUCTION-INSTALL]` record - the declaration, the candidates, the
layout decision, the preview, the preservation and boot-menu records, the
layout record and the boot-entry record - is mirrored to the framebuffer
console through `console_publish_record()`, as is the `[PRODUCTION-NET]` /
`[PRODUCTION-DHCP]` / `[PRODUCTION-DNS]` / `[PRODUCTION-NET-MODE]` family and
the `[XHCI]` and `[KBD ]` local-input records. The acceptance host has no
serial capture: the run that installed onto a disk whose ESP role still carried
a foreign filesystem refused on serial alone, and the operator saw nothing on
screen. The mirror is additive - it paints the framebuffer and never writes to
serial, so the serial stream stays byte for byte what it was - bounded to one
line per record, deduplicated so a poll loop cannot scroll the screen, and a
record published before the banner is painted immediately after it, before the
first prompt.

## Proofs

* **Host**, `tests/installer-gpt-preserve-host.c`
  (`make test-installer-gpt-preserve-host`): a synthetic foreign GPT with four
  entries - the shared ESP next to the appliance's free region, a Debian root, a
  Debian entry above 4 GiB and an entry immediately above the free region - is
  preserved: every foreign entry comes back byte-identical, every untouched
  array sector and the whole MBR (boot code included) come back unchanged, the
  three roles are added, the backup table is complete and the record renders.
  Refusals proven with no write: corrupt primary array, primary/backup
  mismatch, bad header CRC, absent protective MBR, entry outside the usable
  region, overlapping foreign entries, unsupported entry count, unreadable
  table (injected I/O failure), no free region, overlap, out-of-range and
  unaligned requests, a reused ESP the disk does not carry, and the destructive
  plan. Re-running the install adds nothing twice, and a write failure leaves
  the primary array byte-identical and is completed by re-running.
* **QEMU**, `kernel/src/production_installer_qemu.c`
  (`make test-production-installer-qemu`, markers asserted by
  `scripts/test-qemu-production-installer.sh`): the fixture formats a foreign
  ESP holding `\EFI\DEBIAN\GRUBX64.EFI` and `\EFI\DEBIAN\GRUB.CFG`, builds a
  foreign MBR and GPT, and then runs the production path - discovery scan of the
  real synthetic disk, evidence, dual-boot plan, menu, preview, publication,
  confirm, preserving write, shared-ESP reuse. It asserts the foreign entries
  are byte-identical, the foreign directory and both foreign files' clusters are
  byte-identical, the appliance's roles are added in the free slots, the foreign
  loader is still the only foreign loader, the ESP carries exactly the published
  Limine configuration, the menu lists both systems, the corrupt-table and
  overlapping-request refusals write nothing, and an ESP that cannot host our
  loader is refused without being formatted. ITEM-399 adds the appliance
  entry's declared management scope to both the configuration and the record.
* **QEMU, the product flow**,
  `scripts/test-qemu-production-install-flow-preserve.sh`
  (`make test-production-installer-qemu`): the RED/GREEN runs above, and then the
  installed disk is booted alone. ITEM-399 asserts that this boot is the
  *installed* kernel - no `[PRODUCTION-INSTALLER]` record at all - that it
  publishes `[PRODUCTION-MGMT-BIND] scope=LOCAL_ONLY source=boot-declaration`
  because the installed configuration declares the scope, and that the
  configuration the loader reads (`\EFI\BOOT\limine.conf`, the generated menu) is
  read out of the finished image and carries the appliance entry's
  `cmdline: hfoc.management=local-only` next to the sealed chain's module paths.

## Non-goals

Migrating or shrinking an existing system automatically, booting an existing
system from the appliance's recovery environment, chainloading an existing
system through a BIOS-legacy path (ITEM-517 installs the BIOS appliance but the
BIOS menu offers no chainload entries), and any change to the frozen
APIC/IOAPIC/SMP scope.

## Open items

* The `guid(...)` cross-disk entry is proven at the record and configuration
  level and by the read-only discipline; booting it on real firmware (the
  acceptance server's Debian through the appliance's Limine) is a hardware
  acceptance observation, not a QEMU proof.
* The appliance's own kernel path in the generated menu is the `boot():/...`
  convention `boot/limine.conf` uses in this tree; the installed-volume
  specifier for the HFOC system partition (a `guid(...)`/`uuid(...)` volume, or
  the kernel staged on the ESP) belongs to the install flow (ITEM-394) and the
  signed boot chain, and is not proven here.
* The preserved-entry write path is proven on the synthetic QEMU disk; the first
  physical dual-boot install is the hardware acceptance `INSTALLER` stage on
  `/dev/sda`.
