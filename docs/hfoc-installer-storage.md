# HFOC installer storage: the operator names the disk

ITEM-489.  What the installer knows about the machine's disks, how it shows
them, and the two rules that keep an operator - not a default - in charge of
which disk the installer will later write to.

## What already existed, and what this item adds

The storage layer already probes the machine: `installer_hardware_discovery`
holds every disk a storage owner enumerated, with the controller, the partition
table, the partitions, the EFI system partition flag and the identity the
hardware reported, and the install flow already builds that report at boot.

What did not exist is the screen an operator reads, and the rule that decides
which disk may be chosen.  This item adds both:

```text
discovery report (existing)
        │
        ▼
installer_storage.c        the model: rows, states, the choice, the schemes
        │                  (pure, host-proven)
        ▼
installer_storage_screen.c the screen: draw through the display abstraction,
        │                  read the keys, publish the reading
        ▼
installer_storage_summary.c the plan the summary shows, built by the same
        │                  installer_layout_contract_* the writer calls
        ▼
[INSTALLER-STORAGE]        what was found, who chose and what the plan is
```

## The install schemes (ITEM-514)

Choosing the disk names the target; it does not decide what happens to it.
ITEM-514 puts that decision on its own screen, between the disk and the write,
in the shape the ordinary installers use: a list of named schemes, each with one
line about what it does and the space it needs beside the space the disk has.

```text
Choose the installation scheme
  Disk: KINGSTON SA400S3 DEADBEE  240.0 GB  GPT
  Free space: 228845 MB   Needed: 128 MB
> Install alongside the existing system
  Uses free space; existing partitions and systems stay.
  Use the free space
  Uses the largest free region; nothing else is touched.
  Erase the whole disk and install HFOC
  DESTRUCTIVE: deletes every partition and all data.
  Manual: give up one existing partition
  No full manual partitioning, no shrink, no LVM or RAID.
```

The four rows never disappear, because a scheme that silently disappears is a
scheme the operator is wrong about.  A row that cannot be used carries its
stable refusal name beside it (`NO_EXISTING_SYSTEM`, `NOT_ENOUGH_FREE_SPACE`,
`NO_EFI_SYSTEM_PARTITION`, `MEDIA_TOO_SMALL`, `NO_RELEASABLE_PARTITION`), and
choosing it draws that refusal instead of quietly selecting another scheme.

The default is the medium's declaration when it made one, otherwise the disk's
own suggestion: alongside when a system is here, erase when it is not.  It is a
cursor position, not a decision - the model selects nothing until an operator's
ENTER - and it is not a lock either: a person may choose a more destructive
scheme than the medium declared, through the erase scheme's own warning page.

The last page before the write is the summary.  It renders the plan the
installer's own `installer_layout_contract_*` planner builds - the same calls
`production_installer_plan_layout()` makes with the same disk and the same
released partition - so what the operator confirms is the plan the writer
executes: what is deleted, the roles and their sizes, whether the EFI system
partition is reused, how many systems the menu keeps and which entry it adds.
A plan the contract refuses is the named refusal on that page.

The strings come from the one catalog (`installer.scheme.*`), in the language
the medium declared, like every other installer screen; the scheme and summary
readings are `[INSTALLER-STORAGE] scheme ...` and `[INSTALLER-STORAGE] summary
...`, both `writes=0`.

## The rules

**A disk is chosen by identity, never by a name.**  This kernel has no
`/dev/sdX` and no `/dev/nvmeX`; the installer must not invent one, because a
name is exactly what changes between machines, between controllers and between
boots.  The row an operator reads is the identity the hardware reported, with
the capacity, the transport and the partition table beside it:

```text
Select installation disk
> KINGSTON SA400S3 DEADBEE  240.0 GB
  Transport: SATA   Table: GPT
  State: SELECTABLE
```

**Nothing is chosen automatically.**  `installer_storage_inventory()` leaves the
choice at `NONE` whatever it found - including on a machine with exactly one
disk - and `installer_storage_can_continue()` stays false until an operator
chooses.  A machine whose disk layout is guessed is a machine whose data can be
destroyed by a default.

**A disk nobody can name is a disk nobody may write to.**  A disk whose identity
or geometry was not read is shown, marked `UNREADABLE`, with the reason, and
`installer_storage_choose()` refuses it.  Fail closed: the screen says what it
could not read instead of offering it.

**This item writes nothing.**  No partitioning, no formatting, no payload, no
boot entry, no boot order.  The module does not include the block device, ESP,
installer or flow headers, which is what the slice pins: the only way to write a
disk from here would be to add one of them.

## The reading

Once the discovery report is complete the installer publishes one bounded
record, on the serial stream and in the durable journal:

```text
[INSTALLER-STORAGE] disks=2 selectable=1 selected=NONE state=NONE table=NONE existing_os=0
[INSTALLER-STORAGE] disks=2 selectable=2 selected=4 state=SELECTABLE table=GPT existing_os=0
```

`selected=NONE` is the honest reading of a screen nobody has chosen on, and it
is the reading a machine that was never touched by an operator shows.  When a
disk is chosen, the record names it by the discovery's own id - not by a device
name - with its partition table and whether a system already lives on it, so
"which disk did the operator choose, and what was on it" is on the disk the
machine came up on.

On a real boot the reading appears beside the flow's own records, and the two do
not contradict each other:

```text
[INSTALLER-STORAGE] disks=1 selectable=1 selected=NONE state=NONE table=NONE existing_os=0
[PRODUCTION-INSTALL] candidate disk=1 controller=SATA identity=QM00001 model=QEMU HARDDISK blocks=8388608 size_mib=4096 selected=1
```

The flow's `selected=1` is the media declaration's choice - the token named that
disk before anybody was asked - and it is why an unattended run works with no
screen at all.  This screen's model is `NONE` until a person chooses, and neither
overwrites the other: the record above is what the screen would show an operator
standing at the machine, and `state=NONE table=NONE` says plainly that no human
has picked anything yet.

## The screen

The screen is drawn through the display abstraction (ITEM-488): on a machine
with a firmware framebuffer it is painted, and on a machine without one the same
lines go to the console and the serial stream.  It reads the
hardware-independent input layer, so the PS/2 keyboard and the USB HID keyboard
the xHCI driver already binds are the same keys to it:

```text
W/S      move between the disks
ENTER    choose the disk under the cursor
Q        leave the screen with whatever is chosen (nothing, if nothing is)
```

The arrow keys move as well: `w`/`s` are the letters the screen's own hint
prints, and an operator who reads the screen must be able to drive it with the
hand they have on the keyboard.  What a key means is a pure function -
`installer_storage_key_action()` - so the keys this screen advertises are proven
on the host, including the ones it does **not** define, which do nothing at all.

The session is bounded by the caller's timeout, so a machine nobody is standing
at carries on with no choice made rather than waiting forever.

## ITEM-490: the operator entry is mandatory

The screen existed but nothing opened it, so an operator never saw it.  This
item gives the medium a second entry and makes that entry's screen mandatory:

```text
/HFOC install (unattended, media-authorised)   hfoc.install=unattended   ← automated runs
/HFOC install (operator: screen + warning)     hfoc.install=console      ← a person
```

The medium a human boots is built so the **operator entry is the first entry**,
which is what `HFOC_INSTALL_MEDIA_ENTRY=console` does (the entries are the same
file either way - only their order changes, and the reorder refuses a config in
which the requested entry is not declared exactly once).  Every automated medium
keeps `unattended` first, because there is nobody at its keyboard:

```text
make production-installer-console-media HFOC_INSTALL_MEDIA_TARGET=...   # a person
make production-installer-authorised-media                              # a machine
```

On that entry the flow does **not** install by itself.  It discovers the disks,
publishes what it found, and waits:

```text
discovery → plan for the declared disk → pending record → storage screen (no deadline)
    ├── ENTER on a disk  → the warning page for *that* disk (nothing written yet)
    │      ├── ENTER     → it becomes the declared target and is written;
    │      │               the record says source=operator-screen
    │      └── Q         → back to the list, nothing confirmed
    ├── Q                → [PRODUCTION-INSTALL] refused reason=CANCELLED_BY_OPERATOR writes=0
    └── nothing          → the machine waits; a mandatory screen has no timeout
```

Two details make that work rather than merely look right:

* the screen runs in the boot's own loop, not in the flow.  The install flow
  runs before the machine has a console and before interrupts are enabled, so a
  screen there could never see a key: it publishes what it prepared and returns,
  and the loop - where local input is live - opens the screen;
* the flow keeps its **own copy** of the media it was handed.  The boot function
  that builds that struct returns before the operator answers, and a re-plan
  built on the dangling stack read refused a healthy machine with
  `FIRMWARE_UNSUPPORTED` - the defect the QEMU proof caught.

The unattended entry is unchanged: no screen, no key, the declared disk, and the
same records it always published.

## ITEM-500: the screen explains why it is asking

The screen is also the place the flow explains itself when the disk the medium
was declared for is not on this machine. The caller hands the model one bounded
line (`installer_storage_set_note()`), the screen prints it above the disks, and
the operator reads which declared serial was not found:

```text
Select installation disk
Declared disk DEADBEEF00000001 is not on this machine - choose a disk
> QEMU HARDDISK  DEADBEEF00000002  4.0 GB
  Transport: SATA   Table: GPT
  State: SELECTABLE
```

The note is the caller's, never the model's: a screen with nothing to explain
prints no note, an empty note is no note, and clearing it removes the line
again. The declaration that could not be honoured is still what the run
publishes - `declared=NOT_ON_THIS_MACHINE disk=NONE identity=NONE writes=0` -
and the operator's choice is what becomes the target.

A refused operator entry that never reaches this screen (no disk, an unknown
firmware, an unreadable report, an unhonourable declaration) gets its own page
through `installer_storage_refusal_show()`: it names the refusal token the
records carry, says the medium cannot install on this machine, and waits for a
key - five minutes, then the ordinary boot continues - so a machine that cannot
install says so on its own screen instead of booting from the medium in silence.
A legacy-BIOS machine is not refused any more (ITEM-517): the screen opens on
it, and the scheme list and the summary it draws are the BIOS plan - BIOS boot
partition plus FAT boot volume, no ESP.

ITEM-518 prints the observed storage between the refusal and the way out, so
`NO_DISK` is not the only thing the person at the machine can act on.  The page
renders one bounded model (`appliance_storage_report.{h,c}`):

```text
Observed storage
  controllers: class_ahci=1 class_nvme=0 brought_up=0 failed=1 reason=BRINGUP_FAILED
  disks: found=0 accepted=0
  disk 1: controller=AHCI identity=KINGSTON-SA400
    blocks=488397168 bs=512 reason=OK
```

`class_ahci`/`class_nvme` are the controllers the class scan matched,
`brought_up` the disks those controllers produced, and `failed` the difference.
Every disk the probe brought up gets a row, and the row's `reason` names the one
finding that matters: `EMPTY_IDENTITY`, `ZERO_GEOMETRY`,
`UNSUPPORTED_BLOCK_SIZE`, `INVALID_DEVICE`, or the two annotations that do not
reject a disk - `IO_ERROR` and `INVALID_PARTITION_TABLE`, for a table the
discovery could not read, which the erase scheme exists for.  Labels come from
the language catalog (`storage.report.*`); counts, identities and reason tokens
stay machine text, and a catalog that cannot answer the labels renders the named
`STORAGE TRANSLATION MISSING` line instead of half a page.  The same facts are
published as one `[STORAGE-REPORT]` serial record, and the console `storage`
command renders the same model on a machine that has no state volume at all
(see `docs/hfoc-appliance-boot-console.md`).

## ITEM-542: the page's own facts are proven on the machine

The medium verdict and the journal state are not printed only by the machine
records: ITEM-541 puts them on the refusal page too, because on a machine with
no serial the page is the only channel left.  The page section reads

```text
[INSTALL-MEDIUM] state=MATCH reason=OK
[INSTALL-LOG] state=UNAVAILABLE reason=HEADER pending=12 dropped=0 refused=0
[INSTALL-LOG-TAIL] shown=12
... up to twelve records, oldest first ...
Press any key to continue.
```

ITEM-542 proves those lines in QEMU, in the two shapes the L875 finding left
indistinguishable:

* no USB medium at all (the boot medium is the optical disc): the page says
  `[INSTALL-MEDIUM] state=UNAVAILABLE reason=NO_DEVICE` beside
  `[INSTALL-LOG] state=UNAVAILABLE reason=READ_ONLY`;
* the operator medium itself booted from a USB stick whose reserved journal
  frame is damaged: the page says `state=MATCH reason=OK` for the medium - the
  record beside it compares the loader's disk id with the device's and names
  `scope=WHOLE` - and `reason=HEADER` for the journal the kernel could not bind.

One shape-based probe reads both: the application selects a machine record by
the fields only a record carries (`pci=`, `scope=`, `loader_id=`) and a page
line by its own shape - the bare medium verdict, and the journal line with its
`pending=`/`dropped=`/`refused=` counters.  The tail is bounded on the machine
itself (`shown=N`, N <= 12), exactly N records stand between the tail header and
the way out, and the way out is still on the page.  The in-CI two-controller
proof selects its `[INSTALL-LOG]`/`[INSTALL-MEDIUM]` records by the same shape,
so a page line can never stand in for the record it asserts.

The USB fixture damages the frame by its documented slot-0 layout (magic,
version, slot size and the header CRC), never by FAT walking, and refuses to
edit a medium that does not carry exactly one valid frame.

**QEMU is not hardware**: the physical acceptance of these lines is the owner's
Toshiba run, and the L875 root cause stays `NOT TESTED` until that run happens.

## ITEM-543: a BAR that explains no size does not sink the controller

The L875 evidence F-8 (2026-10-16) named the reason `NO_DISK`:

```text
storage probe failed state=PROBE_FAILED stage=AHCI_SCAN status=PCI_INVALID_BAR
failed at bdf=00:1f.2 vendor=8086 device=1e03 bar=0
```

The controller is the laptop's working Intel 7-series AHCI device, and the BAR
that failed is not the one AHCI uses.  Its **BAR0** is a legacy I/O BAR whose
size probe does not explain a range (a 16-bit I/O decode answers `0x0000FFF9`,
and `0xFFFF0008` is not a power of two); its **BAR5** is the ordinary memory
ABAR the driver is brought up through.  The decoder treated one unexplainable
BAR as a failure of the whole matched device, so the ABAR was never reached -
and the page the operator saw named the controller, not the disk.

The rule is now:

* a BAR whose size probe cannot explain a range is reported unusable on that
  BAR (`present = false`, with the reason kept in `decode_status`), and the
  device is still decoded;
* a structurally malformed register (reserved I/O bit 1, reserved memory type,
  I/O BAR5, a base that is not aligned to its own range, an overflowing range)
  and a failed configuration access are still hard errors - the validator was
  not weakened to make the L875 green;
* a scan names the BARs it needs (`pci_scan_class_required()`).  The storage
  probe asks for the AHCI ABAR (BAR5) and the NVMe BAR (BAR0); a needed BAR
  that did not decode is the named refusal of that device, with its BDF,
  vendor/device, BAR index and status on the page - never a silent `NO_DISK`;
* the older entry points (`pci_scan_class()`, `pci_scan_class_ex()`) keep
  asking for every BAR the matched device implements.

The host proof in `tests/pci-host.c` fixes the three shapes that explain no
size - the L875's 16-bit decode, a probe read back as zero, and a read-only BAR
that answers with its own base - and keeps the malformed-register refusal and
the needed-BAR refusal by name.  **This is a host fixture, not the laptop**: the
physical proof that the L875 now finds its disk is the owner's hardware run.

## What this item does not do to the unattended install

The install flow on the medium selects its target through the media declaration:
the token names the disk, the flow reads it and records the choice, and an
unattended run must keep behaving exactly as it did - a machine standing in a
rack has nobody to press ENTER.  This item therefore does **not** insert a
waiting screen into that flow.  What it inserts is one non-blocking call, made
after the discovery report is complete:

```text
installer_discovery_finalize()  →  installer_storage_publish()
                                   (one record; no key is read, nothing waits)
```

The record that call publishes is the honest reading of the flow's own state,
and it reads `selected=NONE`: the installer did not choose a disk, because the
declaration - not this screen - is what tells an unattended run which disk it
owns.  Opening the interactive session belongs to the item that owns the
operator's install session, together with the partition planner and the
executor, because only that item can say what a choice is allowed to do next.

## What the following items add

```text
Display, Input, the storage screen          ITEM-488 and this item
        │
        ├── Partition planner     choose the layout: whole disk, manual, preserve
        │                         an existing system
        │
        ├── Destructive confirmation
        │                         the typed CONFIRM before anything is written
        │
        └── Installation executor hand the chosen disk and layout to the flow that
                                  writes; the media declaration's audited path
                                  stays for unattended runs
```

Those items own every write.  Until they exist this screen selects and reports,
and the record says `Partitioning and writing are NOT_IMPLEMENTED`.

## Evidence

* `tests/installer-storage-host.c` proves the model on the host: an empty
  machine, a single disk that is still not chosen for the operator, an
  unreadable disk that cannot be chosen, navigation that stops at the ends, a
  disk with an existing system named as such, the model's bounds, the keys the
  screen advertises and the keys it refuses to invent, and the record's exact
  text and refusal to truncate;
* `scripts/check.d/97-item-489-installer-storage.sh` pins the no-write rule
  (the module may not include the block device, ESP, installer or flow headers
  and must contain no write call), the no-automatic-selection rule, the
  fail-closed identity rule, the screen's integration and the record's fields;
* the reading is published by the install flow once its discovery report is
  complete, which is what a QEMU proof and a hardware run read: the authorised
  medium booted under OVMF with an empty machine publishes
  `disks=0 selectable=0 selected=NONE`, and the same medium with one disk
  publishes `disks=1 selectable=1 selected=NONE` - the installer found the disk
  and still chose nothing by itself, and the flow refused before writing
  anything (`writes=0`) once the machine did not meet the supported-hardware
  contract;
* the foundation screen's `Storage` row still reads `NOT_IMPLEMENTED`: that row
  belongs to ITEM-488's model, and wiring the discovered storage into it belongs
  to the item that also owns what a choice is allowed to do next.

**Physical acceptance of the interactive screen is `NOT TESTED`**: the QEMU and
hardware proofs read the records, and an operator choosing a disk on real
hardware is part of the acceptance stage that the planner and the executor will
need anyway.

`scripts/test-qemu-installer-disk-screen.sh` (target
`test-installer-disk-screen-qemu`) boots the operator medium under OVMF with two
real (virtual) disks and drives the operator's whole gesture through the
emulated keyboard:

* the medium's first entry is the operator entry, read out of the ISO rather
  than assumed;
* the screen lists both disks by the name and the identity the hardware
  reported, with the capacity and the transport beside them;
* DOWN + ENTER chooses the **second** disk - the one the medium did not declare
  - and the reading says so (`selected=2 state=SELECTABLE`);
* that ENTER opens the scheme screen; the blank chosen disk defaults to the
  destructive scheme, whose ENTER draws the warning page that names the disk and
  says everything on it will be erased, and writes nothing (`writes=0`);
* ENTER on the summary page that follows installs, the records name the
  operator's scheme (`scheme declared=none chosen=erase source=operator`) and
  the plan (`layout mode=whole-disk reason=operator-erase declared=0
  chosen=erase`, then `confirm source=operator-screen disk=2 identity=...`,
  `complete disk=2 identity=...`), the disk the medium **did** declare is
  byte-identical afterwards, and the chosen one carries the install;
* `Q` on the erase warning page goes back to the schemes with nothing
  confirmed, `Q` on the list refuses the run with
  `CANCELLED_BY_OPERATOR writes=0`, and neither disk is touched.

```text
QEMU/OVMF proof != physical hardware acceptance
```

The proof drives a virtual keyboard through a virtual machine.  Whether a
particular laptop's keyboard, firmware and disks behave the same way is the
physical acceptance stage, and this document says so rather than implying the
proof covers it.
