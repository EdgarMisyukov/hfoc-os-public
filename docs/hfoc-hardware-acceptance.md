# HFOC Hardware Acceptance

**Item:** ITEM-381 of the HFOC Appliance Platform productization wave
**Depends on:** ITEM-379 (production end-to-end acceptance report)
**Release contract:** HFOC-APPLIANCE-V1
**Status:** harness, matrix and report implemented and proven by a host fixture;
the physical run on the acceptance host is **NOT TESTED** and stays
human-triggered.

## Purpose

Every QEMU proof in this repository runs against virtual hardware it fully
controls. A release decision also needs the other half: the same product path
observed on a real machine, against a real disk, with the same honesty rules —
a stage that has not been observed is `NOT_TESTED`, never an implied pass.

ITEM-381 owns that half. It does **not** claim a hardware result: it defines the
plan, the guard, the audit line, the recording contract and the machine-readable
report, so that when a human runs the acceptance on the acceptance host the
result is reproducible, attributable and impossible to confuse with a QEMU
proof.

## Disk policy (AGENTS.md section 3a)

- `/dev/nvme0n1` is the host system disk. It is never written, partitioned,
  formatted, reset or touched by any acceptance path, test, script or agent.
- `/dev/sda` is the dedicated hardware-acceptance disk. It may be written only
  by a run that passed the identity guard.
- The harness itself never opens `/dev/sda` for writing. The physical procedure
  (install, uninstall, power cycle, factory reset) is performed by a human on
  the acceptance host; the unattended worker never starts it, and CI never runs
  it.

## Identity guard

The declared acceptance identity is the guard of
`scripts/hfoc-physical-sata-continue.sh`, the reference implementation:

| Constant | Value |
| --- | --- |
| Acceptance device | `/dev/sda` |
| Declared model | `HFOC Example SATA*` (case glob) |
| Declared serial | `DEADBEEF00000001` (exact) |
| Declared root device | `/dev/nvme0n1p2` (exact) |

`scripts/hfoc-hardware-acceptance.sh --guard` reads the model from sysfs, the
serial through `lsblk` and the running root through `findmnt`, and exits `90`
on any mismatch, before anything else happens. The hardware acceptance matrix
declares the same constants and the matrix verifier refuses a matrix whose
declared identity differs from the guard, so neither a matrix edit nor an
operator flag can widen the standing permission to write the disk. Swapping,
renaming or replacing the acceptance disk requires updating the constants and
the matrix in the same commit.

## Pre-write audit line

Before the human physical procedure starts, `--arm` appends one audit line to
the run log and quotes the same line in the report:

```
audit initiator=<who or what started it> started_utc=<UTC> device=/dev/sda
      model=<observed> serial=<observed> declared_model=<declared>
      declared_serial=<declared> declared_root=<declared>
      image_sha256=<digest of the image under test>
```

`--arm` requires an explicit human confirmation
(`HFOC_HARDWARE_ACCEPTANCE_CONFIRM=ITEM-381`), refuses inside CI, requires an
existing image to digest, and then exits `92` *awaiting the human physical
run*: arming never writes to the disk. The report verifier refuses a report
whose audit line is missing any of those fragments or does not quote the run
image digest.

## Matrix

`release/hfoc-hardware-acceptance-matrix.txt` is the machine-readable plan: the
26 stages observed on hardware.

- The first 23 mirror the product path of the production end-to-end acceptance
  matrix (everything except `SBOM_RELEASE` and `HARDWARE_ACCEPTANCE`), in the
  same order, with the same owner and the same software counterpart target and
  marker. Each one adds the hardware observation that must be made.
- The last three are hardware-only and have no CI equivalent:
  `DUAL_BOOT_UNINSTALL`, `POWER_CYCLE` and `FACTORY_RESET`. They still name
  their software counterpart proof (the QEMU or host proof that covers the
  logic) while the hardware observation remains the only proof of the physical
  behaviour.

`scripts/verify-hardware-acceptance-matrix.sh` validates the shape, the
agreement with the production matrix and with the guard constants, and that
every software counterpart target exists in the Makefile and its marker is
present in a real proof artifact. The matrix is a plan: it carries no status
fields, and the report carries the results.

### The real media and the real flow (ITEM-394)

The `INSTALLER` stage is an ordinary boot of the **installer medium**, not a
QEMU fixture: the operator boots the acceptance host from the authorised
installer stick and the product flow runs (`docs/hfoc-production-install-flow.md`).

Before the run, the medium must be built with the declared acceptance serial,
so the kernel may write that one disk and no other:

```
make production-installer-authorised-media \
    HFOC_INSTALL_MEDIA_CONFIRM=ITEM-394-UNATTENDED-INSTALL \
    HFOC_INSTALL_MEDIA_TARGET=DEADBEEF00000001 \
    HFOC_INSTALL_MEDIA_MANAGEMENT=local-only
```

ITEM-446 adds the network the installed appliance comes up on. Without a
declaration it discovers an address, which is the pre-item behaviour;
`HFOC_INSTALL_MEDIA_NETWORK=static` with `HFOC_INSTALL_MEDIA_ADDRESS`
(`a.b.c.d/prefix`), `HFOC_INSTALL_MEDIA_GATEWAY` and `HFOC_INSTALL_MEDIA_DNS`
makes it state that address instead, and `HFOC_INSTALL_MEDIA_NETWORK=dhcp`
states the discovery choice explicitly. The declaration is validated before the
first write and again at boot; the installed boot publishes
`[PRODUCTION-NET-MODE] mode=STATIC address=<address>` and runs no DHCP
discover.

The serial is the declared acceptance serial of AGENTS.md section 3a and of the
matrix header; a medium built for another serial refuses with
`TARGET_NOT_FOUND` and writes nothing. The run needs a **supported firmware
mode** - UEFI, or legacy BIOS with the BIOS boot chain (ITEM-517; the two blobs
live only in the installer kernel) - and at least 2 GiB of usable memory (the
ITEM-371 supported-hardware policy), and it publishes the observed figure as
`[PRODUCTION-INSTALL] machine cpu_cores=... ram_bytes=...` before it decides
anything. A firmware mode that cannot be determined at all still refuses with
`FIRMWARE_UNSUPPORTED`.

The acceptance disk's own content is expendable (owner decision 2026-09-13), so
the acceptance medium declares the destructive layout explicitly with
`HFOC_INSTALL_MEDIA_LAYOUT=whole-disk` (or `HFOC-INSTALL-LAYOUT=whole-disk` in
the token). Without that declaration the run preserves a disk that still carries
a system - which is the customer path, and the one ITEM-398 proves.

`HFOC_INSTALL_MEDIA_MANAGEMENT` is the scope the **installed** appliance will
publish. ITEM-399 writes it into the installed configuration as an explicit
`cmdline: hfoc.management=…`, so a run built with
`HFOC_INSTALL_MEDIA_MANAGEMENT=management-interface` boots an installed
appliance that publishes `[PRODUCTION-MGMT-BIND] scope=MANAGEMENT_INTERFACE
source=boot-declaration` and answers on its printed URL, while `local-only`
publishes `scope=LOCAL_ONLY source=boot-declaration`. The medium also carries
two kernels: the installer one it boots, and the product appliance kernel
(`$(KERNEL)`) it stages into the ESP, so the installed machine runs the product
image and its boot carries no `[PRODUCTION-INSTALLER]` record.

#### The operator entry: the person confirms on the screen (ITEM-495)

The run above boots the **unattended** entry, where the medium's own declaration
is the confirmation and nobody is asked anything.  The medium a person boots is
the operator one, and it is built with the same declaration plus the operator
entry first:

```
make production-installer-console-media \
    HFOC_INSTALL_MEDIA_CONFIRM=ITEM-394-UNATTENDED-INSTALL \
    HFOC_INSTALL_MEDIA_TARGET=DEADBEEF00000001 \
    HFOC_INSTALL_MEDIA_MANAGEMENT=management-interface \
    HFOC_INSTALL_MEDIA_LAYOUT=whole-disk
```

The target is what the medium declares and what the plan is built from; the
**disk the operator names is the disk that is written**.  What he sees, and what
he does:

```
> HFOC Example SATA Fixture  DEADBEEF00000001  240.0 GB     ← the name and the
  Transport: SATA   Table: GPT                            identity the hardware
  State: SELECTABLE   Existing system: NO                 reported
        ↓ ENTER on the row
Choose the installation scheme                           ← ITEM-514
> Erase the whole disk and install HFOC   162 MB
  DESTRUCTIVE: deletes every partition and all data.
        ↓ ENTER on the destructive scheme
WARNING: EVERYTHING ON THIS DISK WILL BE DELETED
  Disk: HFOC Example SATA Fixture  DEADBEEF00000001
  Capacity: 240.0 GB   Transport: SATA   Table: GPT
  Nothing has been written yet.
ENTER erase this disk and install   Q go back
        ↓ ENTER on the warning page
Confirm the installation plan                            ← ITEM-514
  Will be deleted: everything on this disk
  Will be created: EFI system partition, HFOC system, ...
  Nothing has been written yet.
ENTER confirm this plan   Q go back to the schemes
        ↓ ENTER on the summary page
```

ENTER on the summary page is the confirmation - the disk the operator named,
the scheme the operator chose (the record names `chosen=`) and the plan the
same contract planner built for the writer.  `Q` on the erase warning goes
back to the schemes with nothing confirmed; `Q` on the scheme list or on the
disk list refuses the run (`CANCELLED_BY_OPERATOR`, zero writes).  **There is
no console step**: ITEM-495 removed the `INSTALL CONFIRM <token>` command, so
the operator never types anything - the person who read the plan and pressed
ENTER is the confirmation, and the record says so:

```
[PRODUCTION-INSTALL] pending source=console entry=console token=... target=DEADBEEF00000001 disk=2 identity=... writes=0
[PRODUCTION-INSTALL] confirm source=operator-screen entry=console token=... target=DEADBEEF00000001 disk=2 identity=... management=... when_uptime_s=...
[PRODUCTION-INSTALL] complete disk=2 identity=...
```

A run that is never confirmed - nobody at the machine, or `Q` - publishes
`[PRODUCTION-INSTALL] refused reason=CANCELLED_BY_OPERATOR writes=0` and touches
neither disk.  The gesture is proven in QEMU by
`make test-installer-disk-screen-qemu`; a person confirming on real hardware is
the physical stage, and its evidence is the serial capture below, exactly like
the unattended run's.

The stage evidence is the serial record of the run, recorded verbatim in the
report:

```
[PRODUCTION-INSTALL] media token=PRESENT confirm=... target=DEADBEEF00000001 management=local-only mode=whole-disk|preserve|default entry=unattended
[PRODUCTION-INSTALL] candidate disk=1 controller=SATA identity=DEADBEEF00000001 model=... blocks=... size_mib=... selected=1
[PRODUCTION-INSTALL] confirm source=media-token entry=unattended token=... target=DEADBEEF00000001 disk=1 identity=DEADBEEF00000001 management=local-only when_uptime_s=...
[PRODUCTION-INSTALL] source boot_payload=... limine_config=... kernel=... envelope=... payload_blob=...
[PRODUCTION-INSTALL] media sealed=VERIFIED release=1 signer=1 payload_bytes=...
[PRODUCTION-INSTALL] machine cpu_cores=... ram_bytes=...
[PRODUCTION-INSTALL] layout mode=whole-disk|preserve|release reason=... declared=0|1 [released_index=...] chosen=none|alongside|free-space|erase|manual
[PRODUCTION-INSTALL] boot-menu state=GENERATED|SEALED reason=chainload-entry|appliance-only|menu-unavailable stage=... entries=... loaders=... missing=... unreadable=... disks=... target=... pci_ahci=... pci_nvme=... ahci=... nvme=... nvme_status=... nvme_step=... nvme_wait_ms=... d1=... d2=...
[PRODUCTION-INSTALL] preview ... footprint_lba=329728 footprint_mib=161 ...
[INSTALLER-LAYOUT] plan ... / [INSTALLER-LAYOUT] role=... (one per role)
[PRODUCTION-INSTALL] esp payload=VERIFIED first_lba=2048 length_lba=67584 payload_bytes=... mode=whole-disk|preserve staged_bytes=... mechanism=REMOVABLE_MEDIA_PATH
[PRODUCTION-INSTALL] boot-entry state=AVAILABLE mechanism=REMOVABLE_MEDIA_PATH path=\EFI\BOOT\BOOTX64.EFI kernel=\boot\hfoc-kernel.elf config=\boot\limine\limine.conf menu=\EFI\BOOT\limine.conf entries=... timeout=10s source=GENERATED payload_set=7|source=SEALED [menu_reason=MENU_UNAVAILABLE] payload_set=6 required_sectors=... esp_first_lba=2048 esp_sectors=67584 sealed_bytes=... sealed_digest=... existing_loader=FREE|PRESERVED ... existing_config=FREE|PRESERVED ... verify=SEALED_MEDIA variable=UNAVAILABLE boot_entry=NONE
[PRODUCTION-INSTALL] state initialized=VERIFIED ready=1
[PRODUCTION-INSTALL] applied disk=1 identity=DEADBEEF00000001 controller=SATA management=local-only when_uptime_s=... layout=VERIFIED
[PRODUCTION-INSTALL] complete disk=1 identity=DEADBEEF00000001 medium=REMOVE reset=ACPI|PCI|I8042|FAULT in=5s
[PLATFORM-RESET] plan reset=... steps=...>...>FAULT acpi_declared=0|1 pci=0|1 i8042=0|1
[PLATFORM-RESET] attempt=1/... reset=... ...
```

The `boot-menu` record's probe fields are what a menu that offers fewer
systems than the machine carries is read with: `pci_ahci=`/`pci_nvme=` are the
controllers the class scan matched, `ahci=`/`nvme=` the disks they produced,
and `nvme_status=` the bring-up verdict of the last NVMe controller.  ITEM-429
adds `nvme_step=` - the call that stopped (`RESET`, `QUEUE`, `ADMIN_QUEUE`,
`ENABLE`, `IDENTIFY`, `NAMESPACE`, `IO_QUEUE`, `IO_QUEUE_CONFIGURE`,
`BLOCK_DEVICE`, or `READY`) - and `nvme_wait_ms=`, the milliseconds the last
controller-ready wait spent.  A `nvme_status=NVME_TIMEOUT` with `nvme_step=RESET`
or `nvme_step=ENABLE` and `nvme_wait_ms=1000` is a controller that never
reported `CSTS.RDY` in a full second; the same timeout with a small
`nvme_wait_ms=` would mean the driver stopped waiting too early, which is the
defect this item closes.

On a disk that already carries a system the same run publishes the preserving
path instead: `[INSTALLER-PRESERVE] plan mode=dual-boot … preserved=… added=…
esp_reused=1` and the `[INSTALLER-BOOT-MENU]` records, both before the first
write.

On a machine whose local input is unavailable - the acceptance host with no
PS/2 controller and no USB keyboard - the console offers its own menu: it
paints the entries and publishes `[LOCAL-MENU] offered reason=NO_LOCAL_INPUT
entries=6` with `LOCAL INPUT UNAVAILABLE: THE MENU IS SHOWN FOR READING`, so
the operator sees what the appliance offers even though nothing on that machine
can select it.  On a machine with a keyboard the menu is offered once after
30 s of an untouched keyboard (`reason=IDLE`), opened with ENTER on the empty
line, and any other keystroke cancels the offer and is typed as usual.

#### The operator sets the address (ITEM-496)

The appliance's DHCP-or-static choice is made at this console, and a hardware
run records it as the `NETWORK_CHOICE` stage.  With a keyboard and the
installed system running, the operator waits for the offer (or types `menu`),
presses `n` for Network and reads what the machine says it has before answering
anything:

```
[PRODUCTION-NET-SET] state=OFFERED current=DHCP
network mode [dhcp|static] (empty cancels):
```

Answer `static` and the four questions (address as `a.b.c.d/prefix`, gateway,
DNS) or `dhcp` alone.  Each question is one IRQ-safe line, so the keyboard's own
interrupt records cannot split the text the operator is answering.  The answer
is stored, not applied: the record says both what was stored and what the
interface keeps until the next boot, e.g.

```
[PRODUCTION-NET-SET] mode=STATIC address=10.0.2.16/24 gateway=10.0.2.2 dns=10.0.2.3 persisted=OK applies=reboot current=DHCP
```

Reboot through the menu's own `Reboot` entry (or the operator's reset), and read
the boot that follows: `[PRODUCTION-NET-MODE] mode=STATIC address=10.0.2.16/24
clock=…` with no `[PRODUCTION-DHCP] … discover=[1-9]` line.  Going back to
`dhcp` produces the opposite: `[PRODUCTION-NET-MODE] mode=DHCP` and
`[PRODUCTION-DHCP] state=ACTIVE lease=ACQUIRED address=…`.  A mistyped answer
is refused with its reason (`[PRODUCTION-NET-SET] state=REFUSED
reason=NETWORK_IPV4_INVALID_PREFIX`) and changes nothing - the next boot still
shows the previous address.  A machine whose state volume is not usable says
`[PRODUCTION-NET-SET] state=UNAVAILABLE reason=NO_RECORD_OWNER` at the entry,
before asking anything.  Both questions and records are in the boot log now
(ITEM-496), so the stage can be quoted from the `/dev/sda1` log rather than
from the screen alone.  The QEMU proof (`scripts/test-qemu-production-install-flow.sh`)
drives all four gestures and additionally requires the static address to answer
the host's own ARP and ICMP; a physical run records what the operator saw and
whether the address answered from another machine.

An operator records the `MEDIA_DIGEST` stage for the image on the stick, then
records the `INSTALLER` stage from those records. Which steps are reachable on
the acceptance host today, and which still need the next item, is stated under
"Honest limits" below and in `docs/hfoc-production-install-flow.md`.

### Quoting the installed footprint (ITEM-391)

The `INSTALLER` stage is observed against the declared production layout, not
against whatever ranges an operator supplies. ITEM-391 fixes the four role
sizes (EFI System Partition 33 MiB, HFOC system 64 MiB, HFOC persistence 32 MiB,
HFOC recovery 32 MiB; declared footprint 161 MiB = 329728 sectors) and refuses
a target below the exact minimum media (331810 sectors, >= 162 MiB) as
`PRODUCTION_INSTALLER_MEDIA_TOO_SMALL`
(`docs/hfoc-installer-layout.md`). The installer publishes the numbers it
planned as the single-line `[INSTALLER-LAYOUT]` serial record - one `plan`
line plus one `role=` line per role - so the physical run can record that
record verbatim as the stage evidence and the report can quote the footprint
and the space left for the previous OS without recomputing anything:
`media_lba`, `minimum_media_lba`, `footprint_lba`/`footprint_mib`,
`free_first_lba`, `free_lba`/`free_mib`, `existing_partitions` and the per-role
`first_lba`/`length_lba`/`length_mib`. A dual-boot run additionally quotes
`existing_first_lba`/`existing_lba`, the span the previous system keeps.

### Booting the installed disk (ITEM-397, ITEM-399)

The `INSTALLER` stage writes the ESP and the persistent state volume, and the
next stage is the first boot of the machine from *that disk*, not from the
installer media. ITEM-397 makes that boot real and proves it:

- the installer stages a bounded boot payload set into the ESP - the Limine
  loader on the removable-media path `\EFI\BOOT\BOOTX64.EFI`, the boot
  configuration, the sealed boot manifest and the kernel - and refuses an ESP
  that cannot hold it with `PRODUCTION_INSTALLER_ESP_TOO_SMALL` *before the
  first write*, so a partial install that cannot boot is impossible;
- it verifies the staged set against the sealed ITEM-371 media source digest
  and requires the staged configuration to name the staged kernel by its real
  path;
- it publishes which mechanism makes the firmware find the disk -
  `mechanism=REMOVABLE_MEDIA_PATH` where no EFI runtime exists to write a
  `Boot####` entry, `mechanism=EFI_BOOT_VARIABLE` where one does - in the
  `[PRODUCTION-INSTALL] boot-entry state=...` record, so the recorded boot
  entry state is never confused with the state of the disk;
- ITEM-399: the kernel it stages is the medium's **product appliance** kernel
  (`boot/hfoc-kernel.elf`, `$(KERNEL)`), never the installer kernel that is
  running, and the configuration it stages carries the declared management
  scope as `cmdline: hfoc.management=…`. The first boot of the installed disk
  is therefore the product image and publishes no `[PRODUCTION-INSTALLER]`
  record and no QEMU-fixture record - an installed machine must not carry test
  code - and it publishes the scope the medium declared:
  `[PRODUCTION-MGMT-BIND] scope=LOCAL_ONLY|MANAGEMENT_INTERFACE
  source=boot-declaration`;
- `make test-production-installer-qemu` builds the installed disk with the
  production entry points on a file-backed block device, re-verifies the
  staged set on the finished image, boots QEMU from that image alone under
  OVMF and requires the ordinary `[APPLIANCE-BOOT] ... storage=READY` record,
  the `[PMM ]` and `[KBD ]` records, the boot-chain attestation and the
  management publication (`QEMU installed-disk boot test: OK`). Its product-flow
  proof installs from both the `local-only` and the `management-interface`
  medium, boots each installed disk alone, requires the absence of every
  `[PRODUCTION-INSTALLER]` record, exactly one `[APPLIANCE-BOOT]
  runtime=READY … storage=READY`, the scope that medium declared, and compares
  the configuration read out of the finished ESP with the medium's sealed
  configuration byte for byte.

The `STORAGE_STATE` and `FIRST_BOOT` stages therefore observe the installed
disk's own first boot. Since ITEM-411 the machine performs that reboot itself:
the completed install publishes
`[PRODUCTION-INSTALL] complete disk=1 identity=DEADBEEF00000001 medium=REMOVE
reset=… in=5s`, waits the bounded few seconds it names, and resets the machine
through the first mechanism of its reset plan (`docs/hfoc-production-install-flow.md`).
The operator's step changes with it: **pull the installation medium out while
the countdown runs** - the firmware prefers the removable medium, so a machine
left with the stick in boots the installer again - and then record the readiness
record the installed system prints. No keypress is involved, so the local
keyboard being unavailable (ITEM-392/395) does not matter here.

The `INSTALLER` observation records two more lines than before:

```
[PRODUCTION-INSTALL] complete disk=1 identity=DEADBEEF00000001 medium=REMOVE reset=PCI in=5s
[PLATFORM-RESET] plan reset=PCI steps=PCI>I8042>FAULT acpi_declared=0 pci=1 i8042=1
[PLATFORM-RESET] attempt=1/3 reset=PCI port=0xCF9 value=0x06
```

The plan line states which mechanisms this machine will try, in order, and the
attempt lines state which one was actually written; the **last** attempt line
before the machine reboots is the mechanism the platform honoured, so a run
that falls through ACPI and PCI to the i8042 or to the triple fault says so
instead of claiming the first one worked. The `reset=` field of the completion
record is the mechanism the plan tries first, i.e. the prediction the attempt
lines then confirm or correct.

The `NETWORK_LINK` stage records the `[PRODUCTION-NET]` line as it is printed:
since ITEM-399 it carries `xid=0x… family=…` for the silicon the Realtek backend
identified (`pci=10EC:8168` alone names a dozen parts of that family), and
since ITEM-408 it also carries `lifecycle_reason=… lifecycle_hold=…`, the
addressing lifecycle's own reason for having left - or held - `ACTIVE`. Since
ITEM-451 it also carries the three flags a frame has to pass before lwIP sees
it - `bound=`, `proto_published=`, `netif_live=` - and the record is re-emitted
when one of them changes, so a run whose panel answered nothing says whether
the receive path was open at all. The
`[PRODUCTION-DHCP]` line carries the same decision next to the address it is
about:

```
[PRODUCTION-DHCP] state=ACTIVE lease=ACQUIRED address=<address> reason=NONE hold=0 lease_time=<n>s
```

The `NETWORK_LINK` stage is a PASS only when the last `[PRODUCTION-DHCP]` line
of the run shows `state=ACTIVE lease=ACQUIRED address=<the address the printed
URL names> reason=NONE`, and when nothing after it reports `state=FAILED
lease=NONE`. A non-`NONE` reason with the address still present
(`RENEW_TIMEOUT`, `REBIND_TIMEOUT`, `DNS_PROBE_TIMEOUT`, `LINK_DOWN_SAMPLE`, …)
is the documented *holding* state - the appliance keeps the address, keeps
serving and keeps retrying - and the run records the reason verbatim, because
that reason is what the next iteration has to act on. `state=FAILED
lease=NONE` is a real loss and a DEGRADED stage.

### The machine keeps running and says what the wire did (ITEM-428)

The 2026-09-16 run's log ended ten seconds in on
`[PRODUCTION-DHCP] state=ADDRESSING lease=NONE reason=NONE attempt=11/40` and
every record that lives in the main loop stopped with it. Three fields and one
record now make that shape impossible to read ambiguously, and a run states
them:

* `pci_cmd=` on the `[PRODUCTION-NET]` line is the live backend's PCI command
  register after the driver enabled memory space and **bus mastering**
  (`pci_cmd=0x…`, bit `0x4` set). A function that answers every MMIO register
  read while its transmit ring never returns a descriptor - which is what the
  2026-09-16 run's Realtek part did - is exactly a function without bus
  mastering, and this is the one field that says which of the two it was. If
  the bit cannot be set the driver refuses the device and publishes
  `reason=PCI_BUS_MASTER_REFUSED` instead of driving a NIC that cannot reach
  memory.
* the `[PRODUCTION-DHCP]` line carries the wire view:

  ```
  [PRODUCTION-DHCP] state=ADDRESSING lease=NONE reason=NONE hold=0 attempt=12/40 discover=12 request=0 offer=0 ack=0 refused=3
  ```

  `discover=`/`request=` are the DHCP frames the transmit path accepted,
  `offer=`/`ack=` the replies the server sent, and `refused=` the frames the
  transmit path would not take. "No lease" is therefore never again three
  causes at once: a machine that never transmitted shows
  `discover=0 refused=<n>`, one that transmitted and was ignored shows
  `discover=<n> offer=0`, and one whose answer was dropped shows `offer=<n>`.
  A refused frame also sets `reason=DHCP_TRANSPORT`, and the bounded attempt
  budget still ends the acquisition in a published verdict.
* the periodic `[PRODUCTION-LOOP]` record is the loop's own liveness, and it
  reaches the appliance's own log because it is a flush trigger:

  ```
  [PRODUCTION-LOOP] state=RUNNING ticks=3000 interval=2 stage=100 tx_sub=16 tx_recl=0 tx_lost=16 tx_full=3 tx_rst=1 rx_ok=0 rx_err=0
  ```

  `tx_sub=` is what the driver handed to the device, `tx_recl=` what the
  device handed back by clearing a descriptor's OWN bit, `tx_lost=` the frames
  a bounded transmit restart had to abandon, `tx_full=` the frames the ring
  refused because nothing came back, `tx_rst=` the restart attempts themselves
  - counted before the part is touched, so a bring-up that fails still spends
  one - and `rx_ok=`/`rx_err=` the received frames the stack took or refused.
  A restart that reached the ring's rewind abandons what the device never
  completed (the ring's full sixteen at the moment it triggered), so
  `tx_rst=1 tx_lost=16` is the ordinary recovery; `tx_rst=1 tx_lost=0` says
  the attempt stopped before the rewind - the part did not answer its own
  reset - and `tx_rst=4` with `tx_recl=` still standing still and the reason
  `TX_RING_FULL_EXHAUSTED` says the driver has stopped trying. The record
  is published on the loop's own ten-second cadence whatever else changes, so
  a log whose last line is a heartbeat is a machine that was alive at that
  tick and stopped inside the loop, and a stopped loop is never again
  indistinguishable from a quiet one.

  Since ITEM-451 the same record splits the receive path into the classes a
  peer's traffic falls into, because `rx_ok=5 rx_drop=407` was one number for
  three different failures:

  ```
  [PRODUCTION-LOOP] state=RUNNING ticks=1001 interval=1 stage=100 ... rx_ok=5 rx_drop=407 rx_err=0 rx_arm=1 arp_req_rx=2 arp_rep_rx=0 arp_rep_tx=2 arp_req_tx=1 icmp_echo_rx=1 icmp_other_rx=0 icmp_rep_tx=1 syn_rx=1 synack_tx=1 rst_tx=0 ipv4_other_rx=0 nonipv4_rx=407 malformed_rx=0 limited_rx=0
  ```

  `arp_req_rx`/`arp_rep_tx` are the ARP requests a peer sent and the replies
  this machine sent; `arp_rep_rx`/`arp_req_tx` are the ARP traffic this
  machine received an answer to and the questions it asked itself;
  `icmp_echo_rx`/`icmp_rep_tx` are the echo requests and the echo replies
  (`icmp_other_rx` is ICMP that is not an echo request, which the reachability
  engine does not answer); `syn_rx` is every TCP SYN that arrived and
  `synack_tx`/`rst_tx` are the panel accepting or refusing it. `nonipv4_rx`
  counts every frame that is not ARP or IPv4 - a busy segment's IPv6 router
  advertisements, LLDP, PTP - and `malformed_rx` only what could not be parsed
  at all; the two are no longer the same number. A run that cannot ping the
  machine therefore reads as one of: frames never arrived (`arp_req_rx=0`),
  they arrived and were answered (`arp_req_rx>0 arp_rep_tx>0`, so the cause is
  on the path), the panel's SYN arrived and was refused (`syn_rx>0
  synack_tx=0`), or the path was not open at all (`[PRODUCTION-NET]` with a
  zero flag).

  The heartbeat's clock is the PIT and its cadence is ten seconds, so the same
  counters are also published **when a class first appears**, once per class
  per boot, on the loop turn that sees it:

  ```
  [PRODUCTION-FRAME] class=ARP_REQUEST arp_req_rx=2 arp_rep_tx=2
  [PRODUCTION-FRAME] class=ICMP_ECHO icmp_echo_rx=1 icmp_rep_tx=1
  [PRODUCTION-FRAME] class=TCP_SYN syn_rx=1 synack_tx=1
  [PRODUCTION-FRAME] class=TCP_REFUSED rst_tx=1 syn_rx=2
  [PRODUCTION-FRAME] class=NON_IPV4 nonipv4_rx=407
  ```

  These records are in the durable-log flush table, so a machine that is
  switched off right after a failed ping still has the evidence on its disk:
  the first ARP request (and whether it was answered), the first echo, the
  first panel SYN, the first refusal, and the first frame that was never
  addressed to this machine at all. A `class=` that never appears is itself a
  reading: the machine never saw a frame of that class.

A `NETWORK_LINK` stage that fails quotes the last `[PRODUCTION-LOOP]` record
next to the last `[PRODUCTION-DHCP]` one: the pair says whether the appliance
was still running and which side of the wire stopped first.

### Reaching the panel after the run (ITEM-394)

The acceptance host has no serial capture, so the management address is also
painted on the local console:

```
MANAGEMENT: https://<address>/ reachability=LOCAL_ONLY cert=persisted_self_signed
```

`LOCAL_ONLY` is the default and admits no network ingress. To observe the
`MANAGEMENT_REACHABILITY` stage from another machine, build the acceptance
medium with `HFOC_INSTALL_MEDIA_MANAGEMENT=management-interface`: since ITEM-399
that declaration is written into the installed configuration, so the installed
appliance publishes
`[PRODUCTION-MGMT-BIND] scope=MANAGEMENT_INTERFACE source=boot-declaration` on
its own boot and the printed URL answers. A medium built with `local-only` (the
default) publishes `scope=LOCAL_ONLY source=boot-declaration`. The listener
serves a per-appliance self-signed certificate, so the browser shows a
certificate warning: it is expected, and
`cert=` on the console line says whether the certificate is persisted in the
state volume or per-boot.

### The account and the authenticated management plane (ITEM-401, ITEM-506)

The ordinary first-boot flow leaves the account unset (an unattended install
records `admin=UNSET reason=UNATTENDED`), and the ITEM-365 request gate starts
every boot without a verifier, so the read-only management targets answer `503`
until an operator sets the account on the local console. The `AUTH` stage is
observed with this sequence:

1. On the appliance console run `CREDENTIALS` (ITEM-454: command names are
   accepted in any case, exactly as `help` prints them). The account
   conversation first publishes
   `[ADMIN-ENROLLMENT] v=1 state=UNPROVISIONED gate=CLOSED provisioned=0` and
   asks `Current password:`. On an appliance with no verifiable account the
   answer cannot be checked, so it is ignored; on a provisioned appliance it
   must match, or the change is refused with
   `[ADMIN-ENROLLMENT] current password refused: <STATUS>`.
2. Answer the remaining questions: `New login:`, `New password:` and
   `Repeat new password:`. Nothing is echoed while typing. The record must
   answer `[ADMIN-ENROLLMENT] credentials changed login=<login>` and then
   `state=ACTIVE gate=PUBLISHED provisioned=1`. The typed password never
   appears on the serial stream or in the durable journal.
3. Open a bounded session with `LOGIN`. The console asks `Login:` and
   `Password:` and prints
   `ADMIN SESSION: Authorization: Bearer <32-character token>`; the token is
   valid for the configured session lifetime of that boot only.
4. From another machine:
   - `curl -k https://<address>/api/v1/system` answers `503` before the account
     is set and `401` after it without a session;
   - `curl -k -H "Authorization: Bearer <token>" https://<address>/api/v1/system`
     answers `200` with the live system body, and the same holds for
     `/api/v1/network`, `/api/v1/network/status`, `/api/v1/service` and
     `/api/v1/health`;
   - an unknown bearer token answers `401`, and the anonymous request never
     receives a management body.
   - the browser sign-in form takes the same login and password (ITEM-506 made
     the `username` field real): a wrong login is refused exactly like a wrong
     password.
5. After a reboot the gate starts closed again (the session is bounded per
   boot): repeat step 3 for the `REBOOT_PERSISTENCE` observation, and record
   that the account did not need to be set again.
6. The forgotten-password path stays local: `CREDENTIALS RESET` arms the
   break-glass gesture and `CREDENTIALS RESET CONFIRM` on the next line
   performs it, publishing `[ADMIN-ENROLLMENT] reset; state=UNPROVISIONED`.
   The old `ADMIN`, `ADMIN-PROVISION`, `ADMIN-LOGIN` and `ADMIN-RESET` names
   are removed and answer `UNKNOWN COMMAND`.

Evidence redaction: record the `[ADMIN-ENROLLMENT]` state/gate fields, the HTTP
status codes and the fact that a token was issued. Never record a password, and
mask the token as `Bearer <redacted>` in the acceptance log.

### Reading the appliance's own boot log (ITEM-410)

Every record this kernel publishes is written to the appliance's own disk as it
is produced: the install flow, the boot chain, the storage verdict, DHCP and DNS,
the serving path, the readiness line, and every refusal or failure. A hardware
run therefore does not have to be photographed - it can be *read back*, on the
acceptance host, with no serial capture and no HFOC software beyond one script.

The log lives in `\EFI\HFOC\BOOTLOG.BIN` on the ESP of the disk the install
declared (format, rotation and the full recipe: `docs/hfoc-installed-boot.md`).
After the machine has rebooted into Linux, the whole run is one command:

```
python3 scripts/hfoc-esp-read.py /dev/sda --bootlog
```

and from a mounted ESP it is two lines:

```
mount /dev/sda1 /mnt
python3 scripts/hfoc-esp-read.py --bootlog-file /mnt/EFI/HFOC/BOOTLOG.BIN
```

Both print a summary line and then one line per record with its sequence number,
oldest first, so the install and the boot that followed it appear in one
sequence. `/dev/sda` is only ever *read* here.

The `ITEM-357` host continuation captures the same file automatically when the
machine comes back from the acceptance disk: `scripts/hfoc-physical-sata-continue.sh`
runs the reader after the identity guard has passed and appends
`APPLIANCE_BOOT_LOG=<file>` and `APPLIANCE_BOOT_LOG_RECORDS=<n>` to the evidence
file (`build/hfoc-item410-boot-log.txt` holds the log itself).

Which stages quote it:

| Stage | The log line the stage quotes |
| --- | --- |
| `INSTALLER` | `[PRODUCTION-INSTALL] boot-entry state=AVAILABLE mechanism=…`, `[PRODUCTION-INSTALL] esp payload=VERIFIED …`, `[INSTALLER-LAYOUT] …` |
| `FIRST_BOOT` | `[BOOT-CHAIN] verdict=ATTESTED …`, `[PRODUCTION-BOOT] status=APPLIANCE_PRODUCTION_BOOT_OK … storage=READY`, `[APPLIANCE-BOOT] runtime=READY identity=OK … storage=READY`, `[PRODUCTION-FIRST-BOOT] first_boot=…` |
| `NETWORK_LINK` | `[PRODUCTION-NET] device=… mac=… link=…` |
| `DHCP_DNS` | `[PRODUCTION-NET-MODE] mode=DHCP clock=…`, `[PRODUCTION-DHCP] state=BOUND lease=ACQUIRED address=…`, `[PRODUCTION-DNS] state=RESOLVED answer=…` |
| `NETWORK_CHOICE` | `[PRODUCTION-NET-SET] state=OFFERED current=…`, `[PRODUCTION-NET-SET] mode=… persisted=OK applies=reboot current=…`, and the `[PRODUCTION-NET-MODE]` / `[PRODUCTION-DHCP]` line of the boot that followed |
| `AUTH` | `[ADMIN-ENROLLMENT] v=1 state=… gate=…`, `[PRODUCTION-SERVING] v=1 state=…` — an unattended install reads `state=AWAITING_CREDENTIAL … listener=down` until the console sets the account, and the session this stage issues is what publishes the verifier and lets the listener rise — the `[BOOTLOG] state=READY …` line of the boot that issued the session |
| `MANAGEMENT` | `[PRODUCTION-MGMT-BIND] scope=… source=boot-declaration`, `[PRODUCTION-MGMT] url=… reachability=… cert=…`, `[PRODUCTION-TLS] … state=…` |

Record the summary line and the records a stage names; the log's own
`[BOOTLOG] state=READY … reused=…` line states whether this boot appended to the
log the install started (`reused=1`) or created it (`reused=0`), and
`[BOOTLOG] state=UNAVAILABLE reason=…` is the honest answer when the appliance
could not write its log at all - a stage that quotes a log must quote that line
rather than an empty screen.

The reader warns on stderr about a torn or truncated tail and about the records a
wrap left behind; those warnings are evidence too, and the stage must record them
verbatim rather than dropping them.

### Reading the install journal the medium carries (ITEM-521)

The installer also keeps its journal on the medium the machine booted from, so
a run that never reaches a disk - or that stops on a refusal or a panic - still
leaves the record behind instead of leaving nothing.  The placement is the
answer to the ITEM-520 fork (option c): the medium's build reserves
`\HFOC\INSTALL.LOG` in the FAT12 partition, the volume the firmware boots
from, and the installer only reads that reservation and writes the journal
inside the clusters the build already assigned to the file.  The kernel
allocates nothing: a medium built without the reservation simply has no journal
and the run continues unchanged.  The journal is written while the run proceeds
and flushed before the refusal page is painted and on the panic path, so what
the medium carries is the run as it happened, including the record that ends
it.  A medium that cannot carry the journal - an optical boot, a medium without
the reserved file, a volume whose FAT does not back the reservation - is named
in the ordinary records (`[INSTALL-LOG] state=UNAVAILABLE reason=...`) and
never fails the run.

On the acceptance host the whole journal is one command:

```
python3 scripts/hfoc-install-log-read.py --disk /dev/sdb
```

and from a captured image it is the same command with the image file:

```
python3 scripts/hfoc-install-log-read.py build/hfoc-medium.img
```

The reader states the placement it found and then one line per record, oldest
first; it exits 2 when the medium carries no journal, and warns on stderr about
a torn or truncated tail.  It only ever reads: the medium is not mounted and
nothing is written back.  Where the placement is the reserved FAT12 file, the
reader reports it as `fat12-preallocated`, and `--file` reads it straight from a
mounted partition with the same output.

## Run and report

```
scripts/hfoc-hardware-acceptance.sh --plan      # read-only plan and observed identity
scripts/hfoc-hardware-acceptance.sh --guard     # identity guard only, exit 90 on mismatch
HFOC_HARDWARE_ACCEPTANCE_IMAGE=<iso> \
HFOC_HARDWARE_ACCEPTANCE_CONFIRM=ITEM-381 \
    scripts/hfoc-hardware-acceptance.sh --arm   # guard + audit line + manifest, exit 92
scripts/hfoc-hardware-acceptance.sh --record <STAGE> <PASS|DEGRADED|NOT_TESTED> "<evidence>"
scripts/hfoc-hardware-acceptance.sh --report    # render and verify the report
```

Every stage of the path is recorded by a human observation; a `PASS` or
`DEGRADED` record requires non-empty evidence, and the report verifier refuses
a pass without hardware evidence. `--report` publishes
`build/test-artifacts/hardware-acceptance-<UTC>-<pid>/report.txt` and validates
it before printing it:

| Gate | Meaning |
| --- | --- |
| `ACCEPT` | every stage is `PASS` or `DEGRADED`, `physical_run=YES` |
| `INCOMPLETE` | nothing failed, but at least one stage is `NOT_TESTED`, `physical_run=NO` |
| `REJECT` | at least one stage is `FAIL` or `UNOBSERVED`, `physical_run=NO` |

`scripts/verify-hardware-acceptance-report.sh` enforces the schema, the release
identity, the source revision, the declared and observed identity (they must be
equal), the image digest, the audit line, the UTC run window, one complete
eight-field block per stage, the status domains, the gate consistency and — with
the matrix — that every stage identity, name, owner and kind matches the plan.

## Published report

`release/hfoc-hardware-acceptance-report.md` stays **NOT TESTED** until a human
completes a physical run. The QEMU-era verifiers pin that: the production
end-to-end acceptance verifier requires `stage_24_state=NOT_TESTED` owned by
ITEM-381 and `**Status:** NOT TESTED`, and the release documentation verifier
refuses any `| ... | PASS |` row in the published report. A hardware result is
recorded first as the run-scoped machine-readable report; the published
markdown is updated by a human afterwards, and it is a hardware proof kept
separate from the QEMU proofs.

## Proof

`make test-hardware-acceptance-host` runs the host fixture. It never touches
`/dev/sda`: the identity comes from a fixture sysfs root and the recorded fake
writers (`dd`, `efibootmgr`, `systemctl`, `partx`, `sgdisk`, `wipefs`,
`mkfs.ext4`) fail the test if any of them is ever invoked. The fixture proves:

- the matrix verifier accepts the canonical matrix and rejects a wrong version,
  a missing hardware observation, an unknown software target, a drifted stage
  order and a missing implemented stage;
- the identity guard accepts the declared identity and exits `90` on a model, a
  serial or a root mismatch, with zero writer invocations;
- `--arm` refuses without the confirmation token and inside CI, and with both
  present it guards, appends the audit line (initiator, timestamp, device,
  model, serial, declared constants, image digest) and exits `92` without
  touching the disk;
- `--record` refuses a `PASS` without evidence and an unknown stage, and
  `--report` publishes an `INCOMPLETE` report with `physical_run=NO` that the
  report verifier accepts;
- the report verifier rejects a `PASS` without hardware evidence, an `ACCEPT`
  gate over an unobserved stage, a missing audit line, an observed-identity
  mismatch, a stage that does not match the matrix, and a `REJECT` gate without
  a failure;
- the published markdown report still says `NOT TESTED` with no `PASS` row.

## Honest limits

- The physical run is **NOT TESTED** in this repository state: it requires the
  acceptance host with the declared disk attached, and it stays human-triggered
  until the first complete successful run.
- ITEM-394 makes the `INSTALLER` stage reachable on hardware, but the install
  does not yet produce a bootable disk: the kernel, its boot configuration and
  the platform boot entry are not staged (`boot-entry state=UNAVAILABLE`), so
  the following `FIRST_BOOT` and `STORAGE_STATE` stages still need the next
  item (`docs/hfoc-production-install-flow.md`, "Honest limits"). ITEM-397 and
  ITEM-399 close the kernel, the configuration and the staged boot set on the
  QEMU side; the platform boot entry and the two-disk chainload still need a
  component with an EFI runtime, and the `xid=`/`family=` fields are observed
  on hardware for the first time on the acceptance host's own part.
- **Which reset mechanism the acceptance host honours (ITEM-411).** The QEMU
  proof observes that a completed install reboots the machine (QEMU runs the
  guest with `-no-reboot`, so the guest reset makes it exit) and that a refused
  run does not, and `make test-platform-reset-host` pins the *selection* for the
  host's shape - no i8042 and no ACPI reset register declares `PCI`, i.e. the
  write sequence to the chipset's reset control register at `0xCF9`. Whether
  this particular chipset obeys `0xCF9`, or whether the machine falls through to
  the i8042 or to the triple fault, is exactly what only the hardware run can
  confirm, and the acceptance log records it from the last `[PLATFORM-RESET]`
  attempt line. Two consequences for the runbook: the boot log of the
  `INSTALLER` boot ends at the reset, so the evidence has to be captured from
  the serial capture or the local console (a boot-log flush to the appliance's
  own disk belongs immediately before the reset and is ITEM-410's facility), and
  the countdown is the operator's window to pull the stick out.
- The report is a record of human observations, not an automated measurement:
  the harness proves the guard, the audit line and the report contract, and the
  stage evidence is what the operator recorded.
- QEMU proofs and hardware proofs stay separate: the matrix names the software
  counterpart for every stage so a reader can tell which claim rests on which
  evidence.
- `VERSION` is not changed by this item.
