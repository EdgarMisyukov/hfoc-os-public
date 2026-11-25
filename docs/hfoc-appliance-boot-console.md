# HFOC Appliance Boot Console

**Date:** 2026-09-06
**Target:** ITEM-344 of
`docs/superpowers/plans/2026-09-06-hfoc-appliance-installer-and-management-ui.md`
**Base SHA:** `c6e4b3227965b2be717702818a89dd17c25d1b8a`
**Prerequisite boundaries:** ITEM-302 runtime identity; ITEM-303..304
configuration; ITEM-305 local appliance console; ITEM-326 appliance health;
ITEM-343 first boot wizard
**Status:** Completed by ITEM-344

## Purpose

This document is the authoritative ITEM-344 local appliance console boundary.
HFOC is a headless server appliance and must not receive a desktop
environment. ITEM-344 adds a deterministic, allocation-free operational text
screen and local administration menu to the existing boot console. It
preserves the existing `hfoc>` diagnostic console and adds the
`appliance-boot` command.

The normal boot still has no installed storage, network, service, or HTTPS
management loader. The command therefore publishes honest `UNAVAILABLE`
values for those domains and exposes a ready management URL only when a
caller publishes validated network and HTTPS state.

## Module

`appliance_boot_console` lives under `kernel/` and publishes:

- a bounded snapshot of product identity, architecture, platform, hostname,
  CPU cores, RAM bytes, health, storage, network, services, management state,
  and uptime ticks;
- a canonical operational screen with a bounded six-entry local menu;
- a bounded action vocabulary (Status, Network, Diagnostics, Reboot,
  Shutdown, Recovery);
- exact status and action strings;
- no allocation and no mutation.

The module never calls a block-device write callback, network API, service
API, reboot path, shutdown path, audit ring, or control-plane action.

## Canonical Screen

The unavailable production state renders:

```text
HFOC Appliance
--------------
Hostname:      hfoc-appliance
CPU:           1
Memory:        504 MiB
Storage:       UNAVAILABLE
Health:        OK
Network:       UNAVAILABLE
Services:      UNAVAILABLE
Management:    UNAVAILABLE

Local administration:
[1] Status
[2] Network
[3] Diagnostics
[4] Reboot
[5] Shutdown
[6] Recovery
```

When storage, network, services, and management are ready, their lines render
`OK` and the management line renders only the validated HTTPS URL. The
production q35/QEMU profile reports the actual usable RAM discovered by the
kernel, currently `504 MiB` for the `-m 512M` acceptance profile. The exact
integer can move by one MiB when the kernel image crosses a memory-map page
boundary; the QEMU test derives it from the kernel's `[MEM ]` boot report
instead of hard-coding a loader-layout-dependent value.

## Action Vocabulary

The local menu action vocabulary is bounded:

| Action | Command word |
| --- | --- |
| Status | `status` |
| Network | `network` |
| Diagnostics | `diag` or `diagnostics` |
| Reboot | `reboot` |
| Shutdown | `shutdown` |
| Recovery | `recovery` |

ITEM-447 makes the Network entry an action of its own: when the boot path has
bound the persistent network record, selecting it closes the menu, asks four
bounded questions (mode, then the address in `a.b.c.d/prefix` form, the gateway
and the DNS), validates the whole answer with the one IPv4 owner the tree
already has, and commits it through the same setter the install declaration
uses.

ITEM-455 makes that gesture visible before the menu is opened.  The owner
looked for where the address is set and could not find it: his run carried no
`[LOCAL-MENU]` record at all, and the offer the machine would have made named
only ENTER.  The offer record now carries the keys the menu answers to, taken
from the menu's own table:

```
[LOCAL-MENU] offered reason=IDLE entries=6 keys=s,n,d,r,p,y
PRESS ENTER FOR THE LOCAL MENU (N: NETWORK SETUP - SET THE ADDRESS THERE)
```

and the read-only offer (a machine with no local input) publishes the same
`keys=` list.  So the answer to "where do I set the address" is readable in the
record: press ENTER on the empty line, then `n` for Network, and the four
questions follow.  The list comes from `local_menu_key_list()`, so it cannot
claim a key the menu does not handle.  An empty answer cancels at every step, an unknown mode or an invalid
value is refused with the owner's own reason (and nothing is written), and the
record `[PRODUCTION-NET-SET] mode=STATIC address=… gateway=… dns=… persisted=OK
applies=reboot current=…` says what was committed and when it applies.  Without
a bound record owner the entry does not ask the questions at all: it publishes
`state=UNAVAILABLE reason=NO_RECORD_OWNER`, because four answers that cannot be
stored are four answers wasted.

ITEM-496 adds what the operator decides with and what a hardware run can read
back afterwards.  Before the first question the console publishes
`[PRODUCTION-NET-SET] state=OFFERED current=…`, rendered by the live wire
(`appliance_network_wire_active_config()`: `DHCP`, `STATIC address=a.b.c.d/p`,
`OFFLINE`, or `UNAVAILABLE`), so the operator sees the starting point rather
than a blank slate.  Every question and every record is one IRQ-safe write
(`console_write_atomic()`, the ITEM-440 rule the menu lines already followed)
and is mirrored into the durable journal as well as the screen: the 2026-09-29
reproduction showed `[IOAPIC] irq1` trace lines splitting `gateway: ` in the
serial capture, which is exactly the line an operator - or a proof - has to be
able to read.  And the answer record names both sides of the reboot: the mode
just stored (`mode=… persisted=…`) and the configuration still on the interface
(`current=…`).

The module maps command words to action enums and renders them; it executes
nothing itself, which keeps the screen renderer separate from the control
plane.  ITEM-419 is the half that does act: the console's local menu
(`menu` on the command line, `kernel/src/local_menu.c` for the model) selects an
entry with the arrow keys or its letter, runs it with ENTER, and owns the
sequencing an action needs - the record of what is about to happen, the boot-log
flush to the appliance's own disk, and then the action itself, through
`platform_reset` (ITEM-411) and `platform_power` (ITEM-418).  The menu is
keyboard-driven; the pointer is a separate item, because the xHCI driver brings
up one device per controller and a mouse needs a second slot, ring and report
path of its own.

## The Mouse (ITEM-448)

The USB path accepts a keyboard and a mouse at the same time.  The boot mouse
report - one button byte and two signed relative deltas, exactly three bytes -
is decoded by `usb_hid.c`, which counts the reports, the movements, the press
and release edges and the refused reports and publishes them individually; the
decoder holds no console, no framebuffer and no controller, because where the
pointer is and what a click hits belongs to the caller.  A report of another
length is refused instead of being truncated, so the pointer can only move by a
report the device actually sent.

The driver holds the two devices as two roles of their own: each bound device
has its own slot, port, interface, endpoint, transfer rings, report buffer,
counters and decoded queue, and only the controller's command ring, event ring
and device-context table are shared.  The configuration walk is asked for a role
(`usb_hid_parse_config_role`), so a boot mouse is no keyboard candidate and the
other way round; a mouse that declares no boot protocol is read through its own
report descriptor.  The walk stops for the keyboard when a keyboard answers, but
keeps visiting the remaining ports while the pointer is still missing, and a
mouse bound on a controller the walk has to leave is re-bound in the same
session when the machine ends up with no keyboard at all - or with a keyboard
that only answered after the whole machine had been visited.

The console owns the pointer.  `pointer.c` is the arithmetic - a movement clamps
at the screen edge, a held button is not a second press, and a click hits the
first rectangle containing it - and `console.c` aims it at the rectangles its
own last menu paint recorded, so a click can only run a line that is on the
screen.  The click runs the entry through the same `local_menu_run()` the
keyboard's ENTER uses and publishes `[POINTER] click entry=... action=... x=...
y=... moves=... presses=...`; the geometry a reader aims at is published as
`[POINTER] menu targets=... x=... y=... line=... width=...` whenever the rows
move, including when the boot log scrolls them up.  The arrow itself is pixels:
`framebuffer.c` saves what the sprite covers and puts it back, and the console
hides it around a text write so no record ends up with an arrow baked into it.

The pointer has its own record next to the keyboard's: `[MOUSE] state=READY
reason=OK port=... speed=... vid=... pid=... iface=... protocol=... ep=... dci=...
interval=... binterval=... max_packet=... mode=... reports=... transfers=...
errors=... refused=... moves=... presses=... releases=...`; a bound pointer that
never moved is published as `[MOUSE] state=READY finding=NO_REPORT ...` after the
same window the keyboard uses, and a refused or released device names its own
vid/pid and interface list instead of reading like a machine without a port.

## Secret Handling

The management URL validator accepts only a printable `https://` URL and
rejects control bytes, non-ASCII bytes, and credentials (`@`). The module
does not accept passwords, tokens, private keys, fingerprints, or any other
secret bytes. Status, screen, action, and error strings contain no secret
material.

## Production Console Command

Typing `appliance-boot` at the `hfoc>` prompt publishes:

- CPU cores from the Limine multiprocessor response;
- RAM bytes from the usable-memory value passed to `console_init()`;
- health `OK` with the current timer tick count;
- storage, network, services, and management as `UNAVAILABLE`.

The command reports `[APPLIANCE-BOOT] SNAPSHOT FAILED` or
`[APPLIANCE-BOOT] RENDER FAILED` only if the bounded renderer fails. It
performs no reboot, shutdown, recovery, storage, network, service, or
management mutation.

ITEM-454: the console's own command names are accepted in any mix of ASCII
case, because `help` prints them in upper case.

ITEM-506: the account vocabulary is one command family. `CREDENTIALS` asks the
current password, the new login and the new password twice, and writes the one
account record; `LOGIN` asks the login and the password and opens one bounded
local session. Neither command takes an argument: a password is only ever an
answer to a question, so it cannot reach the history, the saved draft or the
durable journal; the login is painted as typed and each accepted password
character is one framebuffer-only star (ITEM-610), and the line is zeroed after
every step. The old
`ADMIN`, `ADMIN-PROVISION`, `ADMIN-LOGIN` and `ADMIN-RESET` commands are
removed, not hidden; the break-glass reset stays as `CREDENTIALS RESET` and
`CREDENTIALS RESET CONFIRM`, the same two-line confirmation the recovery path
always required. The panel's sign-in document names the same command
(`credentials`), so the operator who reaches the browser page first knows what
to do at the machine.

## Host Proof

`make test-appliance-boot-console-host` and `make test-console-host` run
host fixtures that:

- render the exact unavailable screen;
- publish and render ready storage, network, services, and HTTPS management;
- reject non-HTTPS and credential-bearing management URLs without mutation;
- parse the complete action vocabulary;
- verify overflow/no-mutation behavior;
- verify a caller buffer must include space for the NUL terminator;
- exercise the production `appliance-boot` console command;
- ITEM-447: select Network in the menu, answer the four questions, assert the
  committed record and the two-word binding's own view of the answer, and
  assert that an unknown mode, an invalid prefix and an empty answer commit
  nothing.

## QEMU Proof

`make test-production-installer-qemu` (the installed-disk phases) drives the
whole ITEM-447 gesture over the QEMU monitor - `menu`, DOWN to Network, ENTER,
the four answers, then the menu's Reboot entry - and the boot after it asserts
`[PRODUCTION-NET-MODE] mode=STATIC address=…` for the address the medium did
*not* declare, which is what makes the menu's commit, and not the medium's
declaration, the record that boot read.

`make test-appliance-boot-console-qemu` boots the production ISO, sends
`appliance-boot` through the keyboard path, and verifies:

- the screen contains `HFOC Appliance`, hostname, CPU, memory, unavailable
  storage/network/services/management, and the six menu entries;
- no kernel panic or render failure marker appears;
- QEMU shuts down cleanly through HMP.

`make test-production-storage-boot-qemu` additionally boots the production ISO
on a machine with no PS/2 controller and a USB HID boot-protocol keyboard on an
emulated xHCI controller (ITEM-395, `-machine q35,i8042=off,usb=off` with
`-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0`), types `kbd` and `help`
through that keyboard, and requires the console to answer both
(`LOCAL INPUT: SOURCE=USB_HID …`, `USB HID: REPORTS=…`, `COMMANDS: HELP …`).
The same target's `keyboard_absent` phase requires the opposite: with neither
path the boot publishes `source=NONE`, `local_input=UNAVAILABLE` and still
reaches readiness.

`make test-production-storage-boot-qemu` also carries the ITEM-406 phase
`usb_hid_keyboard_second_controller`: two emulated xHCI controllers at fixed PCI
slots (`0:16.0`, `0:17.0`), a device that is not a boot keyboard (`usb-mouse`)
on the first one and the keyboard on the second. It requires the first
controller's rejection (`state=UNAVAILABLE reason=NOT_A_KEYBOARD
detail=P5_CONFIG_PARSE_NO_BOOT_KEYBOARD` - the controller's own port number,
QEMU numbers the four USB2 ports of qemu-xhci 5..8) *before* the second
controller's
`state=READY reason=OK`, then types `kbd` and `help` through the keyboard of the
second controller: the scan may not stop at the first controller that answers,
which is what the acceptance host's layout (receiver on controller 1, keyboard
on controller 2) needs.

`make test-production-storage-boot-qemu` also carries the ITEM-407 phase
`usb_hid_empty_controller`: one emulated xHCI controller with **no device at
all**, which is the shape the acceptance host published `reason=NO_DEVICE
detail=NO_CONNECTED_DEVICE` for while a keyboard was physically attached. The
phase requires the `[XHCI]` record to carry the port evidence - `usbcmd=`,
`usbsts=`, the first two `portsc=` values, the powered-port mask `pp=` and
`ppoff=` - with the controller running (`usbcmd` run/stop set, `usbsts`
host-controller-halt clear) and every published port exactly "powered, no device
connected". It also requires `ppoff=0`: QEMU's model always reports
`PORTSC.PP` set, so a scan that claimed port power by writing back what it read
would still pass, but a claim that *lost* the power bit could not.

ITEM-453 answers the question that phase left open - the 2026-09-19 hardware run
published two `portsc=` values out of the six ports of the controller Linux
enumerates the owner's receiver on, and nothing in the record said whether the
other four had been read. Every port of every controller now publishes its own
line:

```
[XHCI-PORT] pci=05:00.3 port=6 group=USB2 was=0x000202A0 now=0x000202A0 pp=1 ccs=0 pls=5 pon=0 kick=0 wait=0
```

`was=` is the raw `PORTSC` the port answered before the driver touched it and
`now=` the value the scan believed; `pp=`/`ccs=`/`pls=` are that value's power,
connect and link state; `pon=1`/`kick=1` say the driver asserted power or moved
the link back to RxDetect before it read; `wait=` is how many milliseconds the
port was given to show a connect. A port in `Disabled`, `Inactive`,
`Compliance` or `Test Mode` reports `ccs=0` whatever is plugged into it, so the
driver no longer reads one register and moves on: a port whose power was clear
is powered, a link state that cannot sample a connect is written back to
RxDetect with the link-state strobe, a port that was written to is re-read for
up to 100 ms (USB 2.0 `TATTDB`) with the window ending the moment `ccs` rises,
and a controller that has just been reset gets one shared connect-debounce
window before any of its ports is believed empty. A port the driver never
touched is not waited on - its `wait=0` is the honest value.

The controller's own port layout is published beside it, one line per Supported
Protocol capability (xHCI 1.1 section 7.2.1):

```
[XHCI-PROTO] pci=05:00.3 index=0 groups=2 major=2 minor=0 name=USB off=5 count=4 ports=5-8 psiv=NONE d0=0x02000402 d1=0x20425355 d2=0x00000405
```

`off=`/`count=` are the capability's raw fields, `ports=a-b` is the range they
declare (the offset is the group's first raw port number, valid values 1 to
`MaxPorts`), `psiv=` lists the Protocol Speed IDs the group declares, and
`d0=`/`d1=`/`d2=` are the three raw dwords it was parsed from - so a controller
whose capability this driver reads differently says so on the next run instead
of a port being mislabelled silently. `group=` on each `[XHCI-PORT]` line is
the label that mapping produced (`USB2`, `USB3`, or `NONE` for a port no
declared group owns).

Both records are flush moments of the durable boot log
(`kernel/src/appliance_boot_log.c`), so which ports a machine looked at, and
what each answered, is on the disk the machine came up on - not only in a
serial capture. What they cannot say is which of the three bring-up steps the
owner's `05:00.3` actually needs: QEMU's xHCI model always reports port power
set, never leaves RxDetect for an empty port and ignores writes to `PED`, so the
power and link-state writes are pinned by the host proof
(`make test-xhci-host`, which drives the decision with the exact `0x000002A0`
the hardware published) and the next hardware run is what decides the physics.

This is a software/QEMU proof, not physical-hardware local-console
acceptance. ITEM-395 also does not close physical USB HID acceptance: the QEMU
controller is qemu-xhci, so firmware handover, real port routing, a real
keyboard's report timing and a machine whose USB 3 ports only speak
SuperSpeed remain unproven on hardware. ITEM-406 narrows this for the low-speed
USB 1.1 case - the acceptance host's K120 is addressed with its port speed
field, its 8-byte endpoint packet and the interval that speed implies, all
proven on the host - but no emulated USB device offers low speed, and QEMU's
xHCI model ignores writes to `PED` and always powers every empty port, so both
the real low-speed transaction and the real PORTSC write semantics still need
the next hardware boot. ITEM-407 narrows the second of those further: the scan
now writes `PORTSC.PP` before it reads the connect status, and publishes the
port values it read, so the next hardware boot says whether a port that came up
unpowered was what hid the keyboard.

`make test-usb-two-controllers-qemu` (ITEM-452,
`scripts/test-qemu-usb-two-controllers.sh`) is the proof for the machine shape
the 2026-09-19 hardware run exposed: the driver visited only the first xHCI
controller, the one with the keyboard, and the Logitech receiver on a USB3 port
of a different controller was never enumerated (`[MOUSE] … seen=0`). The proof
boots the production ISO with **two** emulated xHCI controllers at fixed PCI
addresses (`-device qemu-xhci,id=x0,bus=pcie.0,addr=0x10` and the same at
`addr=0x11`, so the scan order is deterministic), the keyboard on the first and
the mouse on the second, and requires:

- both controllers to name themselves: two `[XHCI]` records with different
  `pci=` addresses, `controllers=2` and `taken=YES` on the record that ends the
  visit;
- both roles to be bound and published on different controllers: `[KBD ]
  state=READY`, `[MOUSE] state=READY reason=OK`, and per device a
  `[USB ] pci=… port=… speed=… role=KEYBOARD|MOUSE state=BOUND … vid=… pid=…
  ifaces=…` line with the keyboard's and the pointer's `pci=` different;
- the keyboard to prove itself *inside its walk window* (`candidates=1 silent=0
  proven=1 retry=0`). QEMU's usb-kbd does not answer the idle request on its
  own, so the proof wakes it with modifier-only report keys while the boot
  records are awaited; without that the first candidate is always silent and
  the proof would only exercise the release-and-re-bind path instead of the
  owner's (a keyboard that answers, after which the walk still has to reach the
  controller the pointer is on);
- both to work at the same time: the keyboard opens the menu
  (`[LOCAL-MENU] open`), the QEMU monitor moves the pointer and clicks the
  Status entry (`[POINTER] click entry=status action=RUN`,
  `[LOCAL-MENU] run action=status`), and the keyboard still answers afterwards
  (`kbd`, `LOCAL INPUT: SOURCE=USB_HID USB=READY`, `KEYS>0`);
- the negative: a second boot puts `usb-storage` (not HID at all) on the second
  controller, and the device must still be published - `[USB ] … state=REFUSED
  reason=… detail=… vid=… pid=…` - while its controller publishes `taken=NO`.
  A plugged-in device that no record names is the failure this phase exists to
  forbid.

What this cannot prove: QEMU has no SuperSpeed device model (every emulated USB
device is USB 2), its xHCI model ignores writes to `PORTSC.PED` and always
reports `PORTSC.PP` set, so the low-speed path and the AMD controllers' PORTSC
semantics stay hardware questions. The proof says so in its own comments rather
than asserting them.

### ITEM-420: the command the keyboard stopped at

That boot answered both of the questions above and named a third cause, in the
`[XHCI]` record of the 2026-09-15 acceptance run:

```
[XHCI] state=UNAVAILABLE reason=ENUMERATION_FAILED
       detail=P10_EVALUATE_CONTEXT_EVENT_RING_FULL bar=0xFCEA0000 ports=14
```

Two defects, both this driver's:

* **the command was not the command it claimed to be.** Evaluate Context is its
  own command, TRB type 13. The driver sent an Address Device TRB (type 11)
  with the Block Set Address bit set, on the reading that "the shape is the
  same". To a controller those are different commands, and for a slot that is
  already addressed an Address Device is not one it may run: the host answered
  completion code 19, a Context State Error, and the keyboard never reached its
  configuration - so the local console stayed read-only.
  `xhci_trb_evaluate_context()` builds the real command now, and
  `xhci_evaluate_ep0()` calls it; the host proof pins the type field, the slot
  id, the input-context address and the fact that BSR is clear.
* **the name of that answer was wrong.** The completion-code table was shifted
  two codes from 16 upwards, so code 19 was published as `EVENT_RING_FULL` -
  pointing the investigation at an event ring that was healthy - and 21, 24 and
  the rest were wrong the same way. The table is now xHCI 1.1 table 6-90, and
  the host test pins the names of the codes a controller can return for these
  commands, so the next record says what the controller actually said.

QEMU does not show the first defect: its xHCI model tolerates the mislabelled
command, which is why `test-production-storage-boot-qemu`'s keyboard phase
passed while the hardware failed. The evidence for this item is the host proof
of the TRB and of the code table, plus the hardware run.

### ITEM-436: the input record names the interface it bound and the traffic it carried

The 2026-09-17 acceptance run enumerated the keyboard and stopped there:

```
[XHCI] state=READY reason=OK detail=OK ... hid=BOOT_KEYBOARD port=10
       speed=FULL vid=0x046D pid=0xC53F max_packet=12
[KBD ] state=READY reason=USB_HID port=0x...0017 config=0x...0067
       source=USB_HID ps2=KEYBOARD_ABSENT usb=OK speed=FULL
       vid=0x046D pid=0xC53F
```

The device is a Logitech wireless receiver (`0xC53F`) that answers with more
than one HID interface, and `max_packet=12` alone did not say *which* interface
the driver selected, on which endpoint, or whether any report ever arrived.
The driver decoded a completed interrupt transfer only when it carried exactly
eight bytes and dropped every other length without counting it, so a keyboard
that was bound and answered with short reports and a keyboard that answered
nothing at all were the same silence.

Two halves are published now:

* **the binding.** The `[KBD ]` record carries the selected interface's own
  numbers - `iface=` (bInterfaceNumber), `protocol=` (bInterfaceProtocol),
  `ep=0x` (bEndpointAddress), `interval=` (the interval the endpoint context
  was programmed with) and `max_packet=` (wMaxPacketSize) - so a receiver with
  several collections is read as "interface N, endpoint 0x81" instead of being
  inferred from a packet size. `xhci_keyboard_interface()` and its siblings
  answer zero once the keyboard is gone, so a lost device does not keep
  publishing its numbers.
* **the traffic.** Every completed transfer is classified against the 8-byte
  boot report by `usb_hid_boot_report_classify()` - whole, short, or a residual
  the 8-byte buffer cannot produce - and the driver counts the ones that were
  not whole and remembers the byte count of the last completion. The heartbeat
  publishes them as `kbd_reports=` (whole reports decoded), `kbd_xfer=`
  (completed transfers), `kbd_err=`, `kbd_ref=` (completed but not a whole
  report) and `kbd_last=` (bytes in the most recent completion); the `kbd`
  console command prints the same live numbers as `REFUSED=` and `LAST=` next
  to the report count.

`make test-usb-hid-host` owns the classification (whole, short, empty and
impossible residuals, and a null out-pointer); `test-production-storage-boot-qemu`'s
keyboard phase requires the binding fields on the record and `REFUSED=0 LAST=8`
from the live counters after it types through the emulated xHCI keyboard;
`test-production-dhcp-dns-qemu` requires the heartbeat's input fields. What
remains a hardware question is the receiver's own descriptors: QEMU's usb-kbd
declares one boot-keyboard interface, so the multi-interface selection is
proven on the host and read on the acceptance host from the next run.

### ITEM-438: the keyboard is polled when it asked to be, and its silence is a finding

The next run of the same machine answered the question ITEM-436 had opened and
raised a harder one. The binding was published:

```
[KBD ] state=READY reason=USB_HID ... iface=0 protocol=1 ep=0x81
       interval=8 max_packet=12
[PRODUCTION-LOOP] state=RUNNING ticks=3001 ... kbd_reports=1 kbd_xfer=2
       kbd_err=0 kbd_ref=0 kbd_last=8
```

One whole 8-byte report in three thousand seconds, from a receiver whose
keyboard was being typed on, and no character on the console. Two defects sat
behind that, both this driver's:

* **the endpoint was polled at the wrong period.** xHCI 1.2 section 6.2.3.6
  gives a low- or full-speed interrupt endpoint the period
  `2^Interval x 125 us` with the field in 3..10, so the descriptor's
  `bInterval` in milliseconds becomes an *exponent*, not a microframe count.
  The driver programmed `bInterval * 8`: the receiver's 1 ms endpoint was
  programmed as 8, which a controller reads as `2^8 x 125 us = 32 ms`, and the
  K120's 10 ms endpoint as 80, which is outside the field's range altogether.
  Linux's `xhci_parse_frame_interval()`, Haiku's endpoint-context code and
  QEMU's own model (`1 << Interval`) agree on the exponent and the range.
  QEMU could not show the defect: its model only exponentiates whatever it is
  handed, and every proof's budget is far longer than either period, so the
  table is pinned on the host instead - the receiver's 1 ms is now `3`, the
  K120's 10 ms is `6` (8 ms, rounded down), and 255 ms clamps to `10`.
* **silence had no name.** `kbd_reports` counts whole reports, so a device
  that answers every poll with an idle report and a device that answers
  nothing at all were the same `kbd_reports` value, and a bound keyboard that
  had never answered was published exactly like one nobody had touched yet.

The record and the heartbeat now separate the three states, because the next
run has to read which one it got from one boot log:

| state | record |
| --- | --- |
| never answered | `kbd_reports=0 kbd_keys=0 kbd_silent=1` and one `[KBD ] state=READY finding=NO_REPORT ...` line |
| answered and said nothing | `kbd_reports>0 kbd_keys=0` |
| answered and typed | `kbd_keys>0` |

* the `[KBD ]` record carries `dci=` (the doorbell target and endpoint-context
  index the reports arrive on) and `binterval=` (the descriptor's own number)
  next to `interval=` (the field the endpoint context was built from), so the
  conversion above is readable on one line instead of inferred from a packet
  size;
* `kbd_keys=` counts the keys those whole reports produced, on the heartbeat
  and in the `kbd` command's live `KEYS=`; `kbd_silent=` is 1 once the silence
  has outlasted `USB_HID_NO_REPORT_WINDOW_SECONDS` (30 s, the console's own
  idle window, measured on the loop's clock);
* the one-shot `[KBD ] state=READY finding=NO_REPORT reports=0 keys=0
  transfers=N waited=Ns window=30s iface=... dci=... interval=... binterval=...
  max_packet=...` line is what an operator gets without reading a heartbeat,
  and `usb_hid_no_report_due()` is the pure decision behind it: bound, no
  report, past the window - a machine with no keyboard is `state=UNAVAILABLE`
  and never a silence, and a keyboard that answered once is never reported as
  never answering.

`make test-xhci-host` owns the interval table and the window decision (including
the negative controls: inside the window, answered once, nothing bound);
`test-production-storage-boot-qemu`'s keyboard phase requires `dci=`,
`binterval=` and `KEYS>0` after it types through the emulated xHCI keyboard;
`test-production-dhcp-dns-qemu` requires `kbd_keys=`/`kbd_silent=` on the
heartbeat. What remains a hardware question is the receiver itself: whether the
report that arrived was an idle one, a boot report the decoder dropped, or the
last report a mis-scheduled endpoint ever delivered is what the next acceptance
run's counters say.

#### The other half: the driver accepts what that run refused

The same hardware session showed the driver refusing devices the machine had,
with two verdicts that were about the *driver*, not the keyboard:

```
[XHCI] state=UNAVAILABLE reason=NOT_A_KEYBOARD
       detail=P5_CONFIG_PARSE_NO_BOOT_KEYBOARD
[XHCI] state=UNAVAILABLE reason=UNSUPPORTED_SPEED detail=P2_SPEED_SUPER
```

* **only a declared boot keyboard was accepted.** The configuration walk
  required HID class 3, boot subclass 1 *and* keyboard protocol 1. A keyboard
  that declares report protocol (`bInterfaceProtocol` 0) or no boot subclass
  was refused, and no record said which of the three fields failed. The walk
  now scores candidates - a declared boot keyboard (3), a boot-capable
  interface in report protocol (2), a HID interface with no boot subclass (1) -
  and picks the best one wherever it sits in the configuration;
* **an interface descriptor cannot tell a keyboard from a mouse**, which is why
  the two weaker candidates are confirmed by the interface's own HID report
  descriptor: `usb_hid_parse_report_descriptor()` walks the short items, finds
  the Generic Desktop keyboard application collection (usage page `0x01`, usage
  `0x06`), its 1-bit modifier field (usages `0xE0..0xE7`) and its 8-bit key
  array, report IDs included. The driver reads that descriptor with
  `GET_DESCRIPTOR(REPORT)` (`0x22`, the interface in `wIndex`), and a mouse, a
  consumer-control collection or a truncated descriptor is refused by name:
  `detail=P5_REPORT_DESCRIPTOR_NOT_KEYBOARD`. A confirmed keyboard's reports
  are converted into the 8-byte boot report the tested decoder already covers,
  so the report path is one decoder, not two;
* **`SET_PROTOCOL(boot)` is sent only to an interface that promised it**
  (subclass 1), and a refusal falls back to the report descriptor instead of
  losing the device; `SET_IDLE` is now sent with a duration
  (`USB_HID_IDLE_PERIOD`, one second) so a live keyboard re-sends its report
  without being touched, and a device that refuses idle is published as
  `idle=REFUSED` and still kept;
* **a SuperSpeed port is addressable.** USB 3.0 section 9.6.1 gives such a
  device a 512-byte default control pipe, and the port register's speed ID (4,
  or 5 for SuperSpeedPlus) is the one the slot context needs; the endpoint's
  `bInterval` is programmed as the field's own value. `reason=UNSUPPORTED_SPEED`
  now means only a speed the controller did not report;
* **the walk no longer stops at the first device that parses as a keyboard.**
  A candidate is bound, given `XHCI_CANDIDATE_PROVE_MS` (1200 ms, one idle
  period plus enumeration and event-ring latency) to answer, and only a proven
  binding ends the scan. A silent candidate is published as
  `[KBD ] state=UNAVAILABLE reason=NO_REPORT detail=P<n>_SILENT port=... speed=...
  vid=... pid=... ifaces=0:03/00/00,... iface=... protocol=... reports=0
  transfers=... errors=0 waited=1200ms`, released, and the ports and controllers
  behind it are still visited. The first silent candidate is re-bound after the
  walk (`retry=1`), so a machine whose only keyboard ignores `SET_IDLE` - QEMU's
  emulated keyboard is one - still ends with that keyboard bound.

The binding record now reads, in full:

```
[KBD ] state=READY ... iface=0 protocol=1 ep=0x81 dci=3 interval=7 binterval=7
       max_packet=8 mode=BOOT idle=OK candidates=1 silent=1 proven=0 retry=1
```

with `mode=REPORT report_id=0 keys_at=2 keys=6` in place of `mode=BOOT` when the
binding came from the report descriptor, and the `[XHCI]` record naming such a
keyboard `hid=REPORT_KEYBOARD` (a declared boot keyboard keeps
`hid=BOOT_KEYBOARD`). `candidates=`/`silent=`/`proven=` count the walk itself,
so one line says whether the keyboard that stands was the first device tried or
the one that answered after the receiver did not.

`make test-usb-hid-host` owns the new walks and their negative shapes (a mouse
report descriptor, a consumer-control collection, a truncated descriptor, a
report of another report ID); `make test-xhci-host` owns the SuperSpeed tables,
the candidate scoring and the third record shape inside the same 320-byte
budget. What still needs the acceptance host is the device itself: which
interfaces the wired K120 and the receiver declare is exactly what the refusal
record's `vid=`/`pid=`/`ifaces=` fields now answer.

### ITEM-444: the screen draws lowercase letters as lowercase

The 2026-09-18 hardware run closed the input question and opened a rendering
one: `help` was executed and `1234567890` arrived as digits, but every letter on
the screen - in the boot records and in the echo of the typed command - was a
capital. The input path was already right; `glyph_rows()` in
`kernel/src/framebuffer.c` folded the case before it looked a glyph up:

```c
if (character >= 'a' && character <= 'z') {
    character = (char)(character - 'a' + 'A');
}
```

The font carried the 26 capitals and nothing else, so no path - record or echo -
could paint a lowercase letter. The font now carries 26 lowercase glyphs in the
same 5x7 cell (x-height on rows 2..6, ascenders `b d f h k l t` reaching row 0,
descenders `g j p q y` using row 6), and `glyph_rows()` selects
`lowercase[character - 'a']`; the capital and digit tables are untouched, and the
input path is not part of this change.

`make test-framebuffer-glyph-host` is the evidence, and it drives the real
renderer rather than a copy of the table: `kernel/src/framebuffer.c` is linked
into the drill, `framebuffer_init()` points it at a host surface, and the cell is
read back from what `framebuffer_char()` painted. It requires every lowercase
letter to be painted, to differ from its capital and from every other lowercase
letter, to stay inside the 5x7 cell the 6-pixel advance depends on, and it
requires a string (`hfoc os 123`, `help`) to fingerprint differently from the
same string in upper case. No QEMU proof can read the machine's screen, so the
host drill is the rendered proof and
`scripts/check.d/54-framebuffer-lowercase.sh` keeps it registered in the
canonical host suite (`make test-host-suite`); the slice also fails if the case
folding ever returns.

### ITEM-457: the scroll moves rows, and publishes what it cost

The 2026-09-19 hardware report was "when the output reaches the bottom of the
screen, the refresh is very slow", and the same run showed the keyboard - polled
from the console's own main loop - answering late while it happened. Both
symptoms had one cause in `framebuffer_scroll_up()`: the body was copied one
*pixel* at a time,

```c
for (uint64_t y = top; y + pixels < active_framebuffer->height; ++y) {
    for (uint64_t x = 0; x < active_framebuffer->width; ++x) {
        address[y * pitch_words + x] = address[(y + pixels) * pitch_words + x];
    }
}
```

so one scrolled text row on a 1920x1080 screen was about two million scalar
reads and writes - once per printed line once the screen was full.

The move is a row move now. `framebuffer_copy_row()` copies the visible bytes of
a row with 8-byte words where the row's alignment allows it and 4-byte words
otherwise, checking the alignment once per row rather than once per word, and
`framebuffer_fill_row()` fills the rows that come in from the bottom with the
same wide access. The rows above the scrolled region are never written: the
scroll is a move, not a repaint. `framebuffer_scroll_up()` returns the bytes it
wrote, so the console publishes a measured cost rather than a claimed one.

The console's measurement is its own record:

```
[CONSOLE] scroll rows=<total> bytes=<total> ms=<average> max_ms=<worst>
```

`rows` is how many text-row scrolls the boot has done, `bytes` how much
framebuffer those moves wrote, `ms` the average cost of the scrolls since the
previous report and `max_ms` the worst single one. The clock is the machine's
100 Hz tick: one scroll ends inside one tick, so a per-scroll millisecond
reading would be "0 or 1" for the row move and for the per-pixel move it
replaced alike (both measured 0-10 ms on an idle TCG guest); the average over
the window is what the tick can measure. The record is one atomic write, because
a proof waits for the whole line. A report is published for the first scroll,
for a scroll worse than any reported before, or every sixteen rows - publishing
a record costs a serial write, and a serial write per scrolled row is the cost
this item is about. A report caused by a report is suppressed and its cost
counted for the next one.

`make test-framebuffer-scroll-host` is the pixel-level evidence and drives the
real `kernel/src/framebuffer.c` against a host surface: the surviving rows must
carry exactly the pixels of the row that was below them, the rows that came in
must carry the fill colour, a row above the scrolled region must be untouched,
row padding must not be written, and both alignments a 32-bpp row can have are
covered. `make test-console-host` drives the console to the last row of its
screen and requires that a line that still fits scrolls nothing and publishes
nothing, that the line past it publishes exactly one scroll with exactly the
bytes the framebuffer returned, and that a long burst stays a summary rather
than one record per scrolled row. `make test-console-history` types several
screens of output into the guest and requires the records to grow, to move the
same row block every time, to stay inside the 50 ms budget and not to get worse
across the burst. `scripts/check.d/64-console-scroll.sh` keeps all of it
registered and fails if the per-pixel loop comes back.

### ITEM-458: the keyboard loses no character, and names what it refused

The 2026-09-19 hardware report was that the keyboard "answers with delays and
sometimes skips characters" while the screen scrolled. ITEM-457 removed the
stall that made the console miss polls; two things were left in the receive
path, and both made a lost character invisible:

* `xhci_service_session()` took one whole report and returned. A keystroke that
  arrived during a pass that was busy printing waited for the next main-loop
  turn, and the event ring it waited in is sixteen TRBs deep - once full, the
  controller stops publishing into it, so the report is gone rather than late;
* the bounded queue between the driver and the console refused a report when it
  was full and said nothing about it, so a lost character and a key nobody
  pressed were the same silence.

One service pass now drains the session's event ring, bounded by
`XHCI_SERVICE_EVENTS_MAX` ring events and `XHCI_SERVICE_REPORTS_MAX` whole
reports; the round-robin in `xhci_keyboard_service()` still gives the other
controller the next turn, so a busy keyboard cannot starve the pointer. A
refused report is counted (`dropped_report_count`), and the wait between the
two most recent reports a device delivered is kept as a running maximum
(`service_gap_ms_max`).

Both decisions live in the pure half of the driver, so the host proof drives
the same arithmetic the kernel runs: `xhci_local_queue_push()` answers whether
the queue accepts a report (a full queue refuses rather than overwriting an
unread one) and `xhci_service_gap_ms()` folds one wait into the maximum,
leaving it alone for the first report of a boot, a clock that went backwards
and an unknown tick rate. `make test-xhci-host` requires every one of those
edges, and `make test-console-host` requires the `kbd` command's new line:

```
USB HID: REPORTS=... TRANSFERS=... ERRORS=... REFUSED=... LAST=...
         KEYS=... DROPPED=0 GAP_MS_MAX=30
```

The boot records carry the same two numbers: `kbd_dropped=` and
`kbd_gap_ms_max=` on the `[PRODUCTION-LOOP]` heartbeat (where `kbd_reports`,
`kbd_xfer`, `kbd_err`, `kbd_ref`, `kbd_last`, `kbd_keys` and `kbd_silent`
already are) and on the `[KBD ] ... finding=NO_REPORT` record.

`make test-production-storage-boot-qemu` is the end-to-end evidence. Its USB
HID phase queues twelve `appliance` reports on a machine with no PS/2
controller and a `usb-kbd` on an emulated xHCI bus, then types 308 characters
through that keyboard in three lines at the contract's own ~10 ms per
character, requires every line to be echoed whole and in order before it is
submitted, and then requires the counters to agree: `DROPPED=0`, the worst
service gap inside 500 ms (measured 10-40 ms on an idle TCG guest), and at
least eight of the queued reports really printed. The proof's wrapper rises
180 to 300 s with the new burst by the ITEM-427 rule.

### ITEM-459: every printable usage has a character, and a dead key names itself

The 2026-09-19 hardware run pressed the key left of Enter on an ISO keyboard -
`Keyboard Non-US \ and |`, HID usage 0x64 - and got `*` where `|` was
expected. The USB HID character tables were filled only up to usage 0x38 while
the arrays themselves had 0x65 entries, so everything above the US letter rows
was `'\0'`: the numeric keypad and 0x64 alike produced no character, and the
decoder skipped them in silence. A dead key left no trace anywhere.

The tables now carry the printable usages of HID 1.11 Appendix B.1.2 up to
0x67, and `USB_HID_USAGE_MAX` / `USB_HID_USAGE_TABLE_SIZE` move with them:

| usage | key | unshifted | shifted |
| --- | --- | --- | --- |
| 0x64 | Keyboard Non-US \ and | | `\` | `|` |
| 0x31 | Keyboard \ and | (US) | `\` | `|` |
| 0x54..0x58 | Keypad / * - + Enter | `/ * - +` and the newline | same |
| 0x59..0x62 | Keypad 1..9 and 0 | the digits | same |
| 0x63 | Keypad . | `.` | `.` |
| 0x67 | Keypad = | `=` | `+` |

A usage that still produces no character is a finding, not a silence: the
decoder keeps the pair in its state and `usb_hid_take_unmapped()` hands it to
the caller once per press, and the driver publishes

```
[KBD ] unmapped usage=0x0000000000000068 modifier=0x0000000000000000
```

once per distinct usage - a held key is one record, not one per report. The
keys the decoder handles by name (Enter, Escape, Backspace, Tab, Space, Caps
Lock, the arrows, Home, Delete, End), the protocol's reserved usages (0x00..
0x03, the modifiers, the function keys and the keys up to 0x53) are excluded:
an arrow key is an answer, not a finding. The `[KBD ]` marker is the one the
keyboard's other findings use, because `[XHCI] state=` records are
one-per-controller evidence the proofs count exactly.

`make test-usb-hid-host` walks every printable US-ASCII character and requires
it to be reachable from some usage, states the usages this item is about
(0x64 both ways, 0x56, 0x67, the keypad digits), and drives the finding: taken
once per press, cleared by the call, carrying the modifiers that were down, and
absent for Caps Lock and the arrows. `make test-production-storage-boot-qemu`
types a line of every sign HMP can name - `\ | { } [ ] ~ ` : ; " ' < > , . / ?
_ - + = * ( )` - through the same USB keyboard with a per-character echo,
requires the completed repaint, and then presses F13 (HID 0x68, past the
tables) and requires exactly one `[KBD ] unmapped` record. QEMU cannot send
usage 0x64 at all (its key-name table has no entry for it), so the 0x64 half is
the host proof's subject; this is stated here rather than papered over.

### ITEM-460: the console's own conversation reaches the durable journal

The owner's 2026-09-20 report was direct: "I am tired of photographing the
screen after every run; cannot the screen output go to the log, so you can read
it afterwards instead of guessing". The record funnel (ITEM-405/410) already
handed every *published record* to the boot log, but the console's own
conversation - the command the operator typed and the answer it produced - went
through `console_write()` and reached the screen and the serial stream only. On
a machine whose only output is the framebuffer, that conversation existed for a
few seconds and then scrolled away; the enrollment refusal the owner
needed was lost under a stream of timer records, and even a photograph could
not recover it.

The console now keeps a bounded transcript of that conversation. Each typed or
menu-run command is journaled as

```
[CONSOLE] cmd="version"
```

and each line of its answer as

```
[CONSOLE] out="HFOC OS 1.0.0 X86-64"
```

The transcript is assembled from the same characters `console_write()` emits
(it is captured inside the one character painter, `console_emit()`) into a
bounded 2 KiB buffer, and published when the command ends, one bounded record
per line - so a multi-line answer (`kbd`, `bootlog`, a menu action) becomes
several records instead of one truncated one, and an answer too long for the
buffer ends with one `[CONSOLE] out="<truncated>"` record. The path is
journal-only: unlike `console_publish_record()`, it does not paint the
framebuffer, so adding it cannot scroll the screen it exists to make optional.

The publication is deliberately deferred to the end of the command. Journaling
from inside the character painter - where the answer is produced - re-entered
the console's own paint and scroll state: the 2026-09-20
`test-usb-mouse-menu-qemu` run showed the scroll report landing between the
pointer's geometry record and its own newline, so the menu moved a row and the
click hit the entry below the one the geometry named. For the same reason a
scroll report is not published while a pointer geometry record is being written
(`pointer_publishing`); the write that follows publishes it and republishes the
corrected geometry like any other record.

A password never reaches the journal, and ITEM-506 makes that structural rather
than a redaction rule. The one account command family takes no argument:
`CREDENTIALS` and `LOGIN` ask their questions, and the typed answer is read in a
hidden input mode that does not echo, does not enter the history or the saved
draft, and is not a command - so there is nothing to quote. The transcript still
carries the change record

```
[CONSOLE] cmd="credentials"
[ADMIN-ENROLLMENT] credentials changed login=operator
```

and the ITEM-460 redaction rule that named the removed `admin-provision` and
`admin-login` keywords survives in one narrow form: those names do not run any
more, but a shipped manual can still make an operator type
`admin-provision <secret>`, so the durable command record, the `UNKNOWN COMMAND`
echo and the input history still strip the argument they carry (as
`admin-provision <redacted>`) instead of keeping a password that the command no
longer consumes. The hidden input line is zeroed after every step, and an
ordinary unknown line is echoed whole because a password can no longer be one.

Whenever the console changes the administrator account - a change
conversation, a login attempt, or the `credentials` state report - it publishes
the structured state as its own record, and the record now carries the last
refusal:

```
[ADMIN-ENROLLMENT] v=1 state=... gate=... provisioned=... sessions=...
    denied=... last_refusal=... throttled=... unavailable=...
```

`last_refusal=NONE` means nothing has been refused yet. The machine's own menu
and the offer that announces it are journaled too: `[LOCAL-MENU] offered ...`,
`[LOCAL-MENU] open`, `[LOCAL-MENU] closed`, `[LOCAL-MENU] offer cancelled` and
`[LOCAL-MENU] run action=...`, so what was shown and what was chosen survive the
boot as well.

The transcript lives in the same bounded ring the boot log already owns
(`APPLIANCE_BOOT_LOG_PENDING_CAPACITY` records staged, 511 slots on the ESP),
so it rotates instead of growing without limit, and the `[BOOTLOG] state=READY
... dropped=...` record publishes how many records the ring lost. No new
storage, format or bound is introduced.

`make test-console-host` drives the real console with the boot log bound and
requires the command, its answer, a multi-line answer, the menu markers and the
redaction - and requires the secret to be absent from everything the log
observed. `make test-appliance-production-admin-enrollment-host` requires the
`last_refusal=` field and that a fresh owner renders `last_refusal=NONE`.

### ITEM-461: the console keeps up with the input, and says what it cost

The owner's 2026-09-20 report on build `ee01b958` was that the keyboard lost
characters, the mouse lagged, the redraw was slow, and the chosen resolution was
the suspected cause. Nothing published the mode or the costs, and the screen was
being flooded: a `[BOOTLOG] state=READY ...` line after every record and a
`[PRODUCTION-SERVING]` line every couple of ticks, both scrolling the console
continuously.

The mode is now a fact rather than a guess. Once the framebuffer is
initialized the boot publishes, to the serial stream, the journal and (once,
after the banner) the screen:

```
[FB] width=1920 height=1080 pitch=7680 cells=160x60 bpp=32
```

The flood is bounded. `[BOOTLOG]` is the log's own bookkeeping and now reaches
the journal **without being painted**; `[PRODUCTION-SERVING]` still publishes a
state change immediately but an unchanged state at most once per two seconds,
because its counters move every tick.

The costs are published instead of guessed. `[CONSOLE] scroll rows= bytes= ms=
max_ms=` now also reaches the durable journal (it was already on the serial
stream and the screen), and the console publishes what painting one line costs:

```
[CONSOLE] paint lines=<total> chars=<total> ms=<average> max_ms=<worst>
```

which is journal-only, because a cost record that painted itself would pay the
cost it exists to measure. The main loop measures its own turn and adds
`iter_ms=<average since the heartbeat> iter_max_ms=<worst of the boot>` (plus
the console's `paint_lines`/`paint_chars`/`paint_max_ms`) to the
`[PRODUCTION-LOOP]` heartbeat, which is on the serial stream the QEMU proof
reads.

The keyboard's report path was already re-armed at the end of the same service
pass that consumed the completion, before the loop paints; the driver now
measures that wait. `kbd_repost_ms_max=` is the longest time between a completed
report and the next interrupt transfer being armed, published on the
`[PRODUCTION-LOOP]` heartbeat next to the queue's own `kbd_dropped=`. A bound
keyboard whose reports wait for the screen therefore shows it as a number
instead of only as a character that arrived late.

`make test-console-history` drives the same guest through several screens of
output and 240 typed characters in three lines, requires every load line to
reach the console whole, requires `DROPPED=0`, and bounds `paint_max_ms` and
`iter_max_ms`. It runs three times: at the firmware default and at 1920x1080 and
2560x1440 through a VGA EDID, with the `[FB]` record asserting that the requested
mode really changed.

## ITEM-463/464/466: a console that chooses its own mode and scale

The owner's 2026-09-20 durable journal carried the mode the console had been
running in:

    [FB] width=3840 height=2160 pitch=15360 cells=320x120 bpp=32
    [CONSOLE] paint lines=1 chars=25 ms=0 max_ms=0

The console ran at the firmware's maximum, and the costs were milliseconds on
the 100 Hz tick, so a paint read as zero and the budget bounded nothing.

ITEM-463 asked Limine for a bounded mode and measured in microseconds, but the
2026-09-21 hardware journal showed the request had not taken:

    [FB] width=3840 height=2160 ... requested=1280x720 actual=3840x2160 fallback=1

The cause was the *placement*: `resolution` is a Limine menu-entry option, and
the line sat at the top of the file as a global option, so Limine ignored it.
The installed configuration carried no such line at all. ITEM-464 moved the line
onto the entry, but that fixed *one* mode: on a 4K panel the owner saw a
1024x768 image the monitor stretched 3.75x ("the characters became huge"), and
the mode was still a hardcoded number that could not fit another machine.

**ITEM-466 removes the hardcode.** Neither `boot/limine.conf` nor the installed
`boot/hfoc-installed.conf` asks for a resolution any more, and the installer's
generated `\EFI\BOOT\limine.conf` entry carries none either. The mode is the
monitor's own preferred one: Limine boots the firmware's first mode, and the
kernel names the preference, the modes on offer, the mode its rule would use if
the preference had been refused, and the automatic font scale:

    [FB] width=3840 height=2160 pitch=15360 cells=213x80 bpp=32 modes=13
         chosen=3840x2160 requested=3840x2160 actual=3840x2160 fallback=0
         scale=3 cell_px=18x27 preferred=3840x2160 available_count=13
         available=1024x768,1280x720,...,3840x2160

`preferred=` is the first 32-bpp entry in Limine's list (the protocol orders
`modes[]` from most to least preferred). `requested=` is kept as the legacy name
of that preference, so older proofs still parse. `chosen=` is the rule's answer:
the preferred mode when it is what booted, otherwise the largest 32-bpp mode the
firmware offered - a refused preference is `fallback=1`. `available_count=` and
`available=` (a bounded, `...`-marked list) are the modes the firmware actually
listed; `scale=`, `cell_px=` and `cells=` are the automatic font geometry.

**The font scale is automatic.** `framebuffer_font_scale()` in
`kernel/src/framebuffer.c` returns the largest integer scale in `[1,3]` whose
**8x16**-pixel cell still keeps at least **80 columns and 30 rows**
(`FRAMEBUFFER_FONT_MIN_COLUMNS`/`FRAMEBUFFER_FONT_MIN_ROWS`); the cap stops a 4K
screen from becoming a dozen giant glyphs. ITEM-620 made the cell the Debian
console-setup `Fixed 8x16` picture (the face `/etc/default/console-setup`
names) and keeps the row floor at 30, so the owner's 1024x768 console uses the
native 8x16 picture and a 768-line mode is never enlarged to a 16x32 cell. The
rule yields 1024x768 -> 1, 1280x800 -> 1, 1366x768 -> 1, 1920x1080 -> 2 and
3840x2160 -> 3.

**ITEM-466 makes a scroll read RAM, not video.** `kernel/src/framebuffer.c`
keeps a caller-owned shadow of the whole surface in cacheable memory; every
console write lands in both copies, `framebuffer_scroll_up()` moves rows inside
the shadow and only then writes video memory, and the software pointer's
save/restore (`pixel_read()`) reads the shadow too. The boot allocates the
shadow from the physical allocator after `pmm_init()` and publishes
`[FB] shadow bytes= pages= physical= bound=0|1`; a refused allocation is not
fatal and leaves the previous read-back path. The 2026-09-21 journal's
`scroll max_us=16315` was exactly the uncached read this removes.

**The costs are microseconds, drained.** The 100 Hz tick cannot resolve a paint
or a scroll - both end inside one tick - so the console reads the processor's
cycle counter (`rdtsc`) in `kernel/src/arch/x86_64/timer.c`; its frequency is
measured against the PIT tick over at least two ticks after interrupts are
enabled, so it is the machine's own rate. The framebuffer is write-combining,
so the processor can retire a paint whose stores are still queued;
`framebuffer_sync()` issues `sfence` and reads a pixel, which drains the queue,
before the console reads the end of the interval. The records are

    [CONSOLE] paint lines=<total> chars=<total> us=<average> max_us=<worst>
    [CONSOLE] scroll rows=<total> bytes=<total> us=<average> max_us=<worst>

and the pointer's arrow is counted as `moves=`/`max_us=` in the heartbeat,
which republishes every cost every window even when the state did not change:

    [PRODUCTION-LOOP] ... iter_us= iter_max_us= paint_us= paint_max_us=
    scroll_us= scroll_max_us= ptr_moves= ptr_max_us= kbd_dropped= kbd_gap_ms_max=

The drawing per character is still one changed cell (erase the cell, paint the
character), so the cost of a record is a function of the characters it carries
and not of the mode; the scroll is a whole-row move, not a repaint.

**The screen has its own output budget.** Every record still reaches the durable
journal, but a text console gains nothing from repainting an unbounded stream of
routine records - each new line shifts the whole body. At most
`CONSOLE_SCREEN_PAINT_PER_SECOND` (12) records are painted per one-second tick
window; the rest are counted and the count is published, so "the screen is
quieter" cannot be mistaken for "the machine stopped talking":

    [CONSOLE] screen suppressed=<total>            (kbd command)
    [PRODUCTION-LOOP] ... screen_suppressed=<n>    (heartbeat + journal)

A suppressed record is already in the durable journal (the log path is taken
before the budget gate), and `make test-console-host`'s
`test_console_screen_budget` pins the budget, the counter and that boundary.

`make test-console-history` runs the same image at three EDID preferences -
1024x768, 1920x1080 and the owner's 3840x2160.  The first two require the native
actual mode, `fallback=0` and the automatic scale (1 and 2).  The 4K run boots
with the 4K EDID, but QEMU's VGA device has too little video memory for a 33 MB
3840x2160 surface (ITEM-463's self-review recorded that), so it cannot require a
native 4K actual: it requires the fallback flag and the automatic scale to
follow whatever actual mode came back, and the pure 3840x2160 -> 3 rule is pinned
by the host proof `tests/framebuffer-scroll-host.c`.  All three runs require
`DROPPED=0` and the paint and loop costs inside their microsecond budgets.

The installed system has its own QEMU proof:
`scripts/test-qemu-installed-boot.sh` boots a disk built by the install path
under an EDID that prefers 1920x1080 and requires the native `actual=1920x1080`,
`chosen=1920x1080`, `fallback=0`, `scale=3` and a loop inside the microsecond
budget; the production install flow proof reads the sealed
`hfoc-installed.conf` back out of the ESP, requires it to carry no
`resolution:` line, and boots it under the same display with the same
requirements. The same item narrows the QEMU suite's fail-closed `Kernel panic`
signature to `Kernel panic ` (trailing space), because a healthy guest's
`Kernel panic: none` all-clear was being read as a panic and suppressed the
retry of the very marker-timeout class this proof can hit.

## ITEM-466: the console's own copy of the surface, and a scale from the mode

The 2026-09-21 hardware journal (`4df5c4b9`) measured
`[CONSOLE] scroll rows=17 bytes=48046080 us=2171 max_us=16315`: one scrolled
line read the console body back out of uncached video memory, and the main
loop - which also polls the keyboard - waited for it ("the keyboard
practically does not type").

* `framebuffer.c` keeps a caller-owned shadow of the whole surface in
  cacheable RAM.  Every console write lands in both copies;
  `framebuffer_scroll_up()` moves the rows inside the shadow and then streams
  them to video, so no video memory is ever read back.  The software pointer's
  save/restore reads the shadow too.  `kernel.c` allocates the shadow from the
  physical allocator after `pmm_init()` and publishes
  `[FB] shadow bytes=<n> pages=<n> physical=<hex> bound=0|1`; a refused
  allocation is not fatal and leaves the previous read-back path.
* The glyph scale is a property of the mode.  `framebuffer_font_scale()` is
  the largest integer scale in `[1,3]` whose 8x16 cell keeps at least 80
  columns and 30 rows; it yields 1024x768 -> 1, 1280x800 -> 1, 1366x768 -> 1,
  1920x1080 -> 2 and 3840x2160 -> 3, so text keeps its physical size instead of
  the monitor stretching a small mode.  The `[FB]` record gains
  ` scale=N cell_px=WxH` after `fallback=`.
* Proofs: `make test-framebuffer-scroll-host` poisons the video rows the old
  code would have read and requires the painted colours from the shadow, and
  pins the four modes above; `make test-console-host` stays green.

ITEM-466 then completed the remaining contract on the merged branch: the
native/preferred mode comes from the firmware's own `modes[]` list (no
hardcoded `resolution:`), `[FB]` publishes `preferred=`/`available=`/
`chosen=`/`actual=`/`scale=`, the screen keeps at most
`CONSOLE_SCREEN_PAINT_PER_SECOND` records while the durable journal keeps every
one, and `make test-console-history` runs the three EDID modes.

## ITEM-467: the input survives the wire, and a silent pointer names why

The owner's 2026-09-21 hardware run on `ec6eb9db` showed the keyboard losing
characters while the wire carried a stream of ARP requests, the bound Logitech
receiver's mouse at `reports=0 transfers=1 moves=0 presses=0`, and none of the
input numbers reachable in the durable journal because they rode at the end of
the ~500-character heartbeat, past the journal's line bound.

* The interrupt transfer is armed with `xhci_trb_interrupt_in()`, a Normal TRB
  that carries IOC **and** ISP.  IOC produces the completion event; ISP extends
  it to a short packet.  The receiver's boot mouse answers with three bytes
  against the driver's four-byte request, and without ISP the controller waited
  for a fourth byte that never came: the transfer was never completed, never
  re-armed and the pointer never moved.  A keyboard is unaffected because its
  eight-byte boot report fills the buffer.
* A silent device's reason is one pure word, `xhci_no_report_reason()`:
  `INTERRUPT_PENDING` (the transfer is still owned by the controller),
  `COMPLETION_WITHOUT_REPORT` (it completed, was re-armed and carried nothing)
  or `NOT_ARMED`.  A report outranks a pending flag.
* The receive path processes at most `APN_RX_BUDGET_PER_PASS` (16) frames per
  loop turn in both the RTL8168 and virtio backends.  The pass used to drain
  the ring to empty, so a peer that sent faster than the loop could classify
  kept the loop inside the receive path while the keyboard waited.
* The input counters get their own short, journal-safe records.  ITEM-470
  split them so the journal's line bound can never cut a proof field:
  `[PRODUCTION-INPUT] kbd_reports= kbd_dropped= kbd_gap_ms_max= kbd_armed=
  kbd_mse= mouse_reports= mouse_moves= mouse_presses= mouse_armed= mouse_mse=
  mouse_no_trb= loops= iter_max_us=`, followed by `[PRODUCTION-INPUT-TAIL]
  kbd_pending= kbd_reposted= mouse_pending= mouse_reposted= mouse_last_ms=
  mouse_reason=`, both emitted immediately after the heartbeat on the same
  cadence.  Every required field is now inside the journal's line bound even
  when every counter has grown ten digits.
* Proofs: `make test-xhci-host` drives the short-packet TRB and all four reason
  shapes; the `usb_hid_keyboard` phase of
  `make test-production-storage-boot-qemu` floods the production receive path
  with broadcast ARP requests (`scripts/hfoc-qemu-netload-peer.py`) while 300+
  characters are typed through the USB keyboard and the pointer is moved and
  clicked on the same controller, and requires `kbd_dropped=0`, a service gap
  and loop turn inside budget, `mouse_reports>0`, `mouse_moves>0`,
  `mouse_presses>0`, and the machine's own `[PRODUCTION-FRAME]
  class=ARP_REQUEST`.


## ITEM-468: the machine tells its own story about xHCI and input

The owner's 2026-09-21 round-7 report on `a0e14097` was the opposite of
ITEM-467's QEMU result: on the real machine `mouse_reports=0` stayed with
`mouse_reason=INTERRUPT_PENDING`, the keyboard arrived late, and the input
record was published exactly once (tick 10) because it is written "on change".
The controller's `usbcmd`/`usbsts` were published only when it was bound, so a
link that degraded later left no trace.  There was no way to tell a controller
that stopped running from an event ring that stopped moving from a transfer that
was never re-armed.

This item is instrumentation only - it changes no binding, no ISP flag and no
poll order.  At most once a second, driven from the ordinary-boot loop, the
driver publishes three short record shapes through the journal-only path of
ITEM-461 (`console_publish_journal()`, no paint, no serial), each bounded by the
console record's own 320-byte line bound:

* `[XHCI-DIAG] pci=bb:dd.f usbcmd= usbsts= crcr= erdp= erstba=
  interrupter_enabled= running= halted= error=` - one per live controller,
  with the operational registers read live and `error=` the USBSTS HSE/HCE
  latches;
* `[INPUT-DIAG] role=keyboard|mouse ready= slot= ep= dci= state= pending=
  reposted= completed= reports= errors= isp= short= last_completion_ms=
  reason=` - one per HID role, where `isp=` states that the armed TRB asked
  for a short-packet event, `short=` counts the completions that arrived as
  short packets, and `reason=` is the same `xhci_no_report_reason()` word
  `[PRODUCTION-INPUT]` carries;
* `[LOOP-DIAG] events= reports= passes= iter_us= iter_max_us= frames=` - the
  service pass's share of the window, the loop's own cost and the wire frames
  of the same window.

The `XHCI` and `INPUT` console commands print the same facts on demand, so a
machine with a silent pointer can be read where it stands; the console
transcript of ITEM-460 puts their answer in the durable journal too.  Proof:
`scripts/check.d/74-xhci-diagnostics.sh` pins the heartbeat, both commands,
every field of the three records and the 320-byte line bound, and the existing
`make test-xhci-host` and QEMU input proofs stay green.


## ITEM-469: the keyboard holds a queue of armed interrupt transfers

The owner's 2026-09-22 run after ITEM-468 showed the same two symptoms with a
clean record: the pointer jerked and fast typing stopped echoing
(`reports=622 errors=0 short=0`).  The diagnosis is the single TRB.  The main
loop sleeps in `cpu_hlt()` and wakes on the 100 Hz PIT, and
`xhci_rearm_devices()` armed exactly one transfer per role at the END of the
service pass - so between a completion and the next tick the endpoint held no
TRB and the device dropped every report that arrived in that window.  A slow,
>10 ms typing cadence fits the armed window; fast typing does not.

ITEM-469 keeps `XHCI_ARM_DEPTH` (3, inside the contracted 2..4) transfers armed
per role in `XHCI_REPORT_SLOTS` (4) distinct report buffers, and replaces the
retired buffer inside the completion's own `xhci_apply_event()` rather than at
the end of the pass.  The binding, the poll order, ISP and the ITEM-458/ITEM-467
budgets are unchanged; xHCI interrupts are still off.

The same item makes the queue visible.  The driver counts every completion code
(`completion_code_counts[]`), names missed-service (23) and event-ring-full (21)
separately, and `[INPUT-DIAG]` gains `armed=` (the queue depth) and `mse=`.
`[LOOP-DIAG]` states `iter_window=1s` and `iter_max_scope=boot`, and the
iteration window is now closed by the once-a-second diagnostic heartbeat, not by
the ten-second `[PRODUCTION-LOOP]` record (which is why the owner's seven second
run read 83 ms as "the loop's turn").  `[PRODUCTION-INPUT]` is published once a
second and carries `kbd_armed=`, `kbd_mse=`, `mouse_armed=` and `mouse_mse=`.
The QEMU fast-typing proof reads that record and requires `kbd_mse=0`,
`kbd_armed>=2`, no dropped report and an in-budget gap; the contract lives in
`scripts/check.d/75-xhci-trb-queue.sh`.


## ITEM-470: the pointer's speed, the mouse queue and the loop counter

The owner's 2026-09-22 run after ITEM-469 confirmed the keyboard (`kbd_mse=0`,
`kbd_dropped=0`, `kbd_armed=3`) and named the pointer: "the mouse is slow".
Three facts were measured on the machine and in the code, and this item fixes
all three.

* **The pointer's speed.**  `pointer_move()` added a report's delta 1:1 in
  pixels.  On the owner's 3840x2160 screen the automatic glyph scale is 3, so
  the interface is three times bigger while the arrow travels the same pixels
  and crawls relative to what the operator aims at.  The console now passes its
  own `console_scale` - the number the `[FB]` record publishes - to
  `pointer_move_scaled()`, whose pure kernel is `pointer_scale_delta()`: at
  scale 1 it is the identity (the small modes keep exactly the old arithmetic),
  above it the delta is multiplied by the scale, and a fast report (at least
  `POINTER_ACCEL_MIN_DELTA` long) gets a gain that grows to
  `POINTER_ACCEL_MAX_GAIN` so a flick crosses the screen while slow aiming
  stays precise.  The 64-bit product is saturated before the edge clamp, and
  the host proof in `tests/pointer-host.c` drives scales 1 and 3, the
  acceleration and the edge.
* **The mouse queue.**  The pointer's endpoint is 1 ms (`binterval=1`) but the
  role kept the keyboard's three armed transfers, so a burst faster than about
  300 reports/s exhausted the queue and the device's NAKs were dropped
  silently: `mouse_mse` is a controller state and cannot see a device's own
  NAK.  `XHCI_REPORT_SLOTS` is now 16, the keyboard keeps `XHCI_ARM_DEPTH` (3),
  the mouse arms `XHCI_MOUSE_ARM_DEPTH` (16) and `xhci_role_arm_depth()`
  chooses between them.  The report pool costs one more page per role in the
  DMA pool and the HID interrupt ring is `XHCI_INTERRUPT_RING_TRBS` (64) so
  sixteen outstanding transfers fit.  A new `no_trb_count` rises whenever a
  completion leaves the role with no armed TRB at all - the exact window the
  device drops a report into - and `[INPUT-DIAG]` carries `no_trb=` while the
  core input record carries `mouse_no_trb=`.
* **The honest loop frequency.**  `passes` (about 99/s, the service passes) and
  `iter_us` (about 82 ms, i.e. about 12/s) disagreed eight-fold in the same
  window, and nothing said which described the main loop.  `kernel.c` now
  increments `production_iteration_count` once per turn in
  `log_production_iteration()`, publishes it as `kernel_loop_iteration_count()`,
  and `[LOOP-DIAG]` carries `loops=` beside `iter_us=`; the core input record
  carries `loops=` too, so the next QEMU and hardware runs settle the question
  with a number instead of a calibration.  Nothing about the rates is
  "corrected" blind.

The QEMU input proof reads the once-a-second record and requires
`mouse_armed>=16`, `mouse_mse=0`, `mouse_no_trb=0` and `loops>0` beside the
ITEM-469 keyboard requirements; the contract lives in
`scripts/check.d/76-pointer-speed-queue-honesty.sh`.

## ITEM-471 (СРОЧНО-21, раунд 10): the arrow stays visible and the clocks agree

The owner's round-9 run delivered the mouse reports (`mouse_armed=16`,
`mouse_mse=0`, `mouse_no_trb=0`, `mouse_moves=32`) and the cursor still did not
appear.  Four things were wrong or unreadable:

* a single report could move the pointer by up to ~1140 px (scale 3 times a
  gain of 3), so a flick put it in a corner in one step;
* at the right and bottom edges `framebuffer_cursor_show()` broke out of the
  sprite as soon as a column or row crossed the edge, leaving one column of the
  arrow;
* `[POINTER]` published moves and costs but never the position or the clamp
  count, so "it moved" and "it is pinned at the edge" read the same;
* `loops` (~98 per 100-tick window) and `iter_us` (~80 000 us) disagreed ~8x and
  one of the two clocks had to be lying.

This item caps one report at `POINTER_MAX_STEP` (128 px) inside
`pointer_scale_delta()`, with scale 1 still the exact identity; draws the sprite
from `pointer_sprite_origin()`, which shifts it left/up so the whole arrow stays
on the screen; publishes `x=`, `y=` and `clamped=` in `[POINTER]`; re-measures
the cycle rate over a ten-tick (`TIMER_CYCLE_CALIBRATION_TICKS`) window instead
of freezing the first two-tick sample; publishes the calibrated rate as
`cal_hz=` beside `loops`/`iter_us`; and pins the loop-average and calibration
arithmetic as the pure `timer_window_average_us()` and
`timer_calibration_rate()` helpers, driven by `tests/pointer-host.c`.  The next
hardware run reads `cal_hz`, `iter_us` and `loops` on one line and decides
which clock lost real time instead of assuming one of them.

### ITEM-472 (СРОЧНО-22): the pointer's deltas are numbers, and the clock is honest

The round-10 hardware run still could not say whether the pointer received
small deltas or lost reports: `[POINTER]` went out once per 64 movements and
carried only `moves=`/costs/`x=`/`y=`/`clamped=`, with no sum of the applied
deltas.  It also showed `cal_hz=17 222 012 730` beside a measured TSC of
3.893 GHz - the cycle clock was calibrated by dividing by the *counted* IRQ
ticks and assuming 100 of them a second, but the loop turn runs for tens of
milliseconds with interrupts held off through a paint, so many edges are never
counted and every `us=` was scaled by the same mistake.

* `[POINTER]` is published once per real second (the first movement still
  publishes at once) by `console_pointer_tick()` on the main loop, and it names
  `dx=`/`dy=` (the applied deltas summed over the window), `dmax=` (the largest
  single applied delta) and `gain=`.
* `PTR` shows and `PTR <1..64>` sets the operator's gain, a multiplier over the
  automatic glyph scale applied before the single-report cap.  The default is
  1, which is the ITEM-471 arithmetic bit for bit.
* `timer_cycle_clock_start()` calibrates the TSC against the PIT's own hardware
  counter: `platform_reset_delay_ms()` owns channel 0 at the PIT base frequency
  for `TIMER_CYCLE_CALIBRATION_MS` (100 ms), so the sample is wall time no
  matter how many IRQs the CPU took, and the 100 Hz tick is restored at once.
  `timer_real_seconds()` is that clock's uptime; `timer_measured_tick_hz()` is
  the counted IRQ ticks per real second.
* `[LOOP-DIAG]` publishes `tick_hz=` and `second=` beside `cal_hz=`, so a
  `tick_hz=` well under 100 names exactly how much real time the tick counter
  lost.

### ITEM-473 (СРОЧНО-23): the event ring absorbs the burst, and the depth has a handle

The round-11 hardware run showed `mouse_reports=24` and `completed=40` frozen
from t=110 to t=211 while `armed=16`, `mse=0`, `no_trb=0`, `state=READY` and the
controller still reported `running=1 halted=0 error=0`: the endpoint simply
stopped receiving completions.  The cause is in the driver: `XHCI_EVENT_TRBS`
was sixteen, exactly the mouse's `XHCI_MOUSE_ARM_DEPTH`, so one burst of
completions filled the event ring and the controller raised Event Ring Full
(21).  Event Ring Full is a Host Controller Event TRB (type 37) the decoder did
not know, so the driver neither counted nor published the one error that
explained the silence.

* `XHCI_EVENT_TRBS` is 256 - the whole 4 KiB page between `XHCI_OFF_EVENT_RING`
  and `XHCI_OFF_ERST` - and static assertions in `kernel/src/xhci.c` pin that
  the ring fits its page and that every armed queue (`XHCI_MOUSE_ARM_DEPTH`,
  `XHCI_REPORT_SLOTS`, `XHCI_ARM_DEPTH`) stays below its capacity.
* The Host Controller Event TRB decodes (`XHCI_TRB_HOST_CONTROLLER_EVENT`), the
  session counts every one and the Event Ring Full (21) separately, and
  `[XHCI-DIAG]` publishes `host_events=` and `ringfull=`.
* `ARM` shows and `ARM <1..16>` sets the mouse's armed-transfer depth, clamped
  by `xhci_arm_depth_clamp()`; the compiled sixteen stays the default.  The
  answer names `mouse_arm_depth=`, `mouse_armed=` and `event_ring=`, so the
  owner can try the ITEM-469 depth of three in one run.
* A per-role `[INPUT-CODE]` record publishes `ringfull=`, the last completion
  code (`last=`) beside a count of every code the role has seen (`codes=`), and
  the endpoint's `ep_state=` (`Running`/`Halted`/`Stopped`/`Disabled`/`Error`)
  read from the device context, so "the endpoint went silent" is a fact read
  from the context, not an inference.
* The short `[PRODUCTION-INPUT-TAIL]` carries `mouse_arm_depth=` and
  `ringfull=`; the QEMU proof requires `mouse_arm_depth>=16` with `ringfull=0`,
  i.e. the sixteen-deep queue no longer fills the event ring.

### ITEM-474 (СРОЧНО-24): the turn reports its phases, and the body moves once

The round-12 hardware run (`/srv/hfoc-example/queue/hw6-log-all.txt`) reported good
pointer transport (`ringfull=0`, `ep_state=Running`, `codes=1:2886`, `armed=16`,
`no_trb=0`) and a loop that turned 2.4 times a second: `iter_us=424 022`,
`loops` 99 -> 195 over 42 real seconds, with the counted IRQ rate at `tick_hz=2`.
The loop ends in `cpu_hlt()`, so `iter_us` alone cannot say whether the turn was
working or sleeping, and one scroll of the whole 3840x2160 body cost
`us=7198` while moving `bytes=31979520` - paid once per printed line.

* The loop publishes `[LOOP-PHASES]` once a second: `turns= console_us= xhci_us=
  input_us= net_us= other_us= work_us= hlt_us= turn_us=` (ITEM-474), plus the
  input sub-spans and the worst-turn facts of ITEM-479 below.  The console's own
  TSC-measured paint+scroll is subtracted from every measured span, so the
  numbers are disjoint and `hlt_us` separates "sleeping" from "working".
* A turn batches its scrolls.  `new_line()` no longer moves the body: the first
  line that would leave the screen increments `console_scroll_pending_cells` and
  keeps the logical cursor on the last row.  Characters printed after that are
  serialized immediately but painted into `console_defer_buffer`;
  `console_flush_deferred()` - called by the main loop before `cpu_hlt()` -
  performs one `framebuffer_scroll_up(BODY_TOP, cells * cell_height)` and
  replays the buffer through the ordinary painter.  `console_write()` outside
  the loop still flushes its own burst, so boot messages and the host proofs are
  unchanged.  A batch never grows past one screenful
  (`console_scroll_batch_limit()`), so the replay always starts on a row inside
  the body; a turn that prints more flushes in whole screenfuls, each still one
  body move.
* The pointer geometry record is published synchronously: its own line may
  scroll the body, and that move is applied while `pointer_publishing` still
  suppresses the recursive publish, so `[POINTER] menu targets=` names the row
  the entry will have after the write.
* `tests/console-host.c` drives eight overflowing lines with the batch open and
  requires no framebuffer move until `console_flush_deferred()`, then exactly
  one move and one `[CONSOLE] scroll rows=8 ...` record.  The QEMU proof reads
  `[LOOP-PHASES]` and requires the full field set and a bounded `turn_us`.
* The console-history proof still reads `[CONSOLE] scroll` and still requires
  the move to be whole screen rows, never pixels (`byte_step % pitch == 0`,
  with the stride read from the `[FB]` record) and the burst to cover several
  screens; because one report may now cover several whole-body writes, the
  per-text-row byte block is no longer required to be constant.  The prompt is
  flushed before it is written, so a report the flush publishes keeps its own
  serial line and the command prompt stays on its own line.

### ITEM-475 (СРОЧНО-25): the production tick stays on PIT

The round-13 hardware run showed the machine living at ~2.4 wakeups a second
(`tick_hz=2`, `iter_us=424 022`, `loops` 99 -> 195 over 42 real seconds) and the
journal named neither the tick source nor its measured rate.  The cause was the
first IRQ0: the runtime coordinator enabled the periodic Local APIC timer and
`pic_disable_after_apic_runtime()` disabled the PIC, leaving the Local APIC
timer as the only wakeup.  AGENTS.md's target is the legacy 8259 PIC + PIT, so
the ordinary production build now keeps PIT:

* `PRODUCTION_CFLAGS` defines `HFOC_PRODUCTION_PIT_TICK`, which compiles
  `lapic_timer_runtime_transition_allowed()` to `false`.  Both IRQ0 paths -
  the PIC vector `0x20` fallback and the IOAPIC vector `0x40` success path -
  call the runtime coordinator only when that policy allows it, so production
  never masks the PIT source and never disables the PIC.
* The drill/QEMU builds keep the verified transition: `$(LAPIC_RUNTIME_ISO)`
  is a kernel without the production macro, and the APIC-focused proofs
  (`test-smp`, `test-lapic`, `test-ioapic-irq0`, `test-ioapic-irq1`,
  `test-lapic-timer-calibration`, `test-lapic-timer-runtime`,
  `test-pic-disable`, `test-pic-irq0-masking`) run on it.  The one-shot, x2apic,
  fallback and scheduler proof kernels are non-production too and keep it.
* `lapic_timer_tick_source()` names `pit` or `lapic`, and the once-a-second
  `[LOOP-PHASES]` record now carries `tick_source=` and the measured `wake_hz=`
  (`timer_measured_tick_hz()`), so a journal reader can tell a starved machine
  from a busy one without inferring the clock.  Calibration also publishes
  `[LAPIC-TIMER] tick policy: pit` (production) or
  `[LAPIC-TIMER] tick policy: lapic-transition` (drill/QEMU).
* The two `[IOAPIC] irq0 vector=0x40` / `[IOAPIC] irq0 lapic_eoi` bring-up
  lines are published once, on the first delivery.  With PIT as the production
  source on the IOAPIC vector they would otherwise print 200 lines a second and
  interleave the console's own records; the interrupt trace (`Servicing
  hardware INT=0x40`) is unaffected.
* The honest clock exposed the unit of the administrator session lifetime: the
  boundary compares its `lifetime_ticks` against `hfoc_lwip_time_now()`, which
  is milliseconds, so `APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX` is now the
  one hour in milliseconds (`3600U * 1000U`) and the panel converts its
  cookie's seconds (`APPLIANCE_PANEL_LOGIN_SESSION_SECONDS`) before opening the
  session.  Under the ~2.4 Hz tick the bare `3600` read as an hour; under the
  100 Hz PIT it read as 3.6 seconds and the authenticated proof expired in the
  middle of the client.
* The QEMU production proof requires `tick_source=pit`, `wake_hz>=50` and at
  least 20 loop turns in the window, i.e. the 100 Hz PIT source is what drives
  the loop; `tests/lapic-timer-host.c` proves the pure source mapping and the
  policy text, and `scripts/check.d/81-pit-tick-production.sh` pins the wiring.

### ITEM-476 (round 14 follow-up): the phases describe the turns they are published beside

The first local production run after ITEM-475
(`production-storage-boot-20260923T190005Z-561658`) published
`[LOOP-PHASES] turns=74 console_us=10032 xhci_us=11211 input_us=23152
net_us=226474 other_us=0 work_us=264132`: the parts summed to 270869 us and
exceeded the turn's work, so the QEMU proof's "the loop's phases exceed the
turn's work" fired.  The cause was ordering, not measurement: a phase span was
added to the window the moment it ended, while `work_us` and `console_us` were
folded only at the next turn boundary.  A record published in the middle of a
turn therefore described N completed turns of work beside N completed turns
plus one unfinished turn's spans, and whether that showed in a run depended on
where the once-a-second boundary fell.

* `kernel_loop_phase_us[]` now holds only the turn in flight.
  `kernel_loop_phase_turn_begin()` folds that turn into
  `kernel_loop_phase_window_us[]` in the same step that folds its work and
  console cost, and clears the in-flight counters; the published window
  therefore holds completed turns only, and `other_us` stays the remainder
  `work_us - accounted`, never negative by construction.
* `kernel_loop_phase_window_reset()` resets the completed-turn window and
  leaves the in-flight spans alone, so no span is counted twice or dropped
  across the once-a-second publish.
* The QEMU production proof checks the invariant in every qualifying
  `[LOOP-PHASES]` record, not only in the one it prints, so timing luck cannot
  hide a regression; `scripts/check.d/82-loop-phase-alignment.sh` pins the
  fold, the reset and the publish.

### ITEM-477 (СРОЧНО-26, round 15): the periodic records reach the journal, not the screen

The owner's 2026-09-24 hardware run on `ce6765a3` read `tick_source=pit` and
`wake_hz=100` with a working pointer, and reported that "служебные символы"
kept arriving on the screen.  The tick fix had made the once-a-second
`[PRODUCTION-INPUT]`, `[PRODUCTION-INPUT-TAIL]` and `[LOOP-PHASES]` records and
the ten-second `[PRODUCTION-LOOP]` heartbeat publish at their real cadence, and
the screen budget (ITEM-466, at most twelve painted records a second) spent
itself repainting "still running" while the operator's own view scrolled away.

The records exist for the serial stream and the durable journal - they are what
makes a quiet machine readable from a log - and the text console gains nothing
from a line whose only news is that the loop is alive.  They now use a
journal-only builder:

* `console_journal_begin()` and `console_journal_end()` are the journal-only
  counterpart of the painted mirror.  They share the mirror's buffer, bound and
  rendering - the record is still assembled with `console_record_text()`/
  `_u64()`/`_hex64()`, so the line is byte for byte the one the serial calls
  wrote - and `console_journal_end()` publishes it through
  `console_publish_journal()` (the funnel ITEM-460 and ITEM-461 use), so it
  reaches the boot log and never the framebuffer.
* `console_record_end()` keeps publishing through `console_publish_record()`,
  the painted mirror API the host proofs still exercise.  ITEM-478 superseded
  its production use: no service record opens the painted mirror any more, so
  the operator's own typing, the menu and the banner are the only things the
  screen gains on its own.
* The four periodic records in `kernel.c` open with
  `console_journal_begin()` instead of `console_record_begin()`; their
  `NET_RECORD_WRITE()`/`NET_RECORD_U64()` fragments are unchanged, so the
  serial stream is exactly what it was and every earlier slice that pins those
  field names still holds.
* `[LOOP-PHASES]` carries `paint_lines=`, the console's rendered-line counter
  (ITEM-461), so the QEMU production proof can require the screen to stay put
  while the heartbeat keeps publishing, and `bootlog` can show the periodic
  records in the durable journal with the dump's own `#<sequence> ` prefix.

`scripts/check.d/83-screen-periodics.sh` pins the two funnels, the four
periodic call sites and the two proofs; `tests/console-host.c` proves
deterministically that a periodic record reaches the bound log and is never
painted while a typed command still paints.

### ITEM-478 (СРОЧНО-27, round 16): the screen keeps only the banner, the prompt and what the operator asked for

The owner's 2026-09-25 review asked that all service information leave the
screen: the product banner and the prompt, the operator's own typing and the
answers it produced, the local menu and an input error are the only things a
fresh screen shows.  ITEM-477 had moved the four periodic records; every
remaining service record still painted.

Every service line now goes through one journal-only path:

* `console_journal_begin()`/`console_journal_end()` for the 26 records in
  `kernel.c` and the 6 in `xhci.c` (`[BOOT]`, `[BOOT-CHAIN]`,
  `[PRODUCTION-*]`, `[APPLIANCE-*]`, `[KBD ]`, `[MOUSE]`, `[USB ]`,
  `[XHCI]` and the rest).  The `serial_write()`/`console_record_text()`
  fragments are unchanged, so the serial stream and the durable journal keep
  every byte; only the funnel that used to paint changed.
* `console_publish_journal()` for the records the console publishes directly:
  the `[FB]` display geometry, the installer and preserve records, the
  platform power and reset lines, and the serving and audit records.
* `console_service_line()` in `console.c` for the lines the console builds
  itself: the readiness banner (`GDT IDT LEGACY PIC PIT PS2: OK`,
  `DOUBLE FAULT IST ENABLED`), the `[LOCAL-MENU]` markers (`offered`, `open`,
  `closed`, `run`, `reboot`, `power off`, `recovery`, `offer cancelled`) and
  the pointer's `[POINTER] menu targets=`/`[POINTER] click` records.  The
  helper writes the serial stream inside one `serial_sequence_begin()` critical
  section, hands the same bounded line to `console_journal_record()` and never
  touches the framebuffer; it counts what it sent in
  `console_service_lines`.
* The product banner is the one machine-origin line that still paints:
  `console_write("HFOC OS 1.0.0 INITIALISED\n")`.  The prompt, the typed line,
  the command's answer, the rendered local menu and `UNKNOWN COMMAND: …` keep
  using `console_write()`, which is exactly the operator's own channel.
* `console_init()` publishes `[CONSOLE] boot paint lines=<n> service_lines=<m>`
  once, at the first prompt, where `n` is the console's painted-line counter
  (ITEM-461) and `m` the service lines it kept off the screen.  A service
  record that painted would push `n` past the banner's one.

`scripts/check.d/84-screen-service-journal.sh` pins the funnel, the boot
record, the banner/readiness split and the journal call sites, and the host
proof `test_service_lines_stay_off_the_screen` and QEMU proof
`test-production-storage-boot` read `paint lines=1` and `service_lines>=1`.

### ITEM-479 (СРОЧНО-28, round 17): the input path names its own phases, and a stalled endpoint recovers

The round-16 hardware run (`/srv/hfoc-example/queue/hw8-log-all.txt`) was healthy in
aggregate (`turn_us=9998`, `wake_hz=100`, 99.7% of the loop halted) and wrong in
the two ways this item addresses: one second published `input_us=1213704` with
`console_us=0`, and the run ended with the pointer silent for 95 s at
`armed=16`, `mse=0`, `no_trb=0`, `ringfull=0`, the endpoint context still
Running and every completion code SUCCESS.  `input_us` was one number for three
different places, and nothing noticed or recovered the silence.

* The input path is measured in three sub-spans, published beside the fields
  ITEM-474 already carried: `in_xhci_us` (the driver service and doorbell),
  `in_kbd_us` (the keyboard's decoded events) and `in_ptr_us` (the pointer's
  movement and drawing).  Each is measured inside the top-level span it belongs
  to and subtracts the console cost charged while it ran, so a sub-span never
  claims paint time; the top-level `xhci_us`/`input_us` keep their names and
  meanings.
* `[LOOP-PHASES]` also carries `worst_us=` (the worst full turn of the window),
  `worst_phase=` (the span that spent it, chosen by the pure
  `loop_budget_dominant()`) and `over50=` (turns longer than
  `LOOP_BUDGET_LONG_TURN_US`, 50 ms).  The snapshot is taken at the turn
  boundary before the in-flight spans are folded and cleared, so the name
  describes exactly the turn whose period it publishes.
* The stalled-endpoint watchdog lives in `xhci.c`.  Progress is the tick of the
  last completion or the last fresh arm; when a role is ready, holds at least
  one armed transfer and has waited longer than `XHCI_WATCHDOG_SILENCE_MS`
  (1000 ms), one `[INPUT-WATCHDOG]` record is published per silence episode
  (`role= slot= ep= armed= silent_ms= count= action=REARM`) and
  `xhci_endpoint_recover()` tops the depth up and rings the endpoint's doorbell.
  The recovery deliberately does not issue a Stop Endpoint / Set TR Dequeue
  Pointer reset: the motivating run showed the endpoint Running with its
  transfers armed, which is the state a doorbell re-kick addresses, and
  rebuilding a live ring under a running controller risks the report loss the
  item exists to prevent.  An idle device completes nothing legitimately, so the
  `watchdog_reported` flag makes the cost one record per episode, not one per
  pass.
* The per-role findings ride `[PRODUCTION-INPUT-TAIL]` as `kbd_watchdog=` and
  `mouse_watchdog=`.  The pure halves are `xhci_watchdog_silence_ms()` and
  `xhci_watchdog_due()` in `xhci_trb.c`.

`make test-loop-budget-host` drives the dominant-phase arithmetic and the stall
bound; `make test-xhci-host` (`test_watchdog_decision`) drives both sides of the
watchdog bound; `scripts/check.d/85-input-subphases-watchdog.sh` pins all of it,
and the QEMU proof `test-production-storage-boot` requires each sub-span inside
its parent, `worst_us>=turn_us`, a known `worst_phase`, `over50<=turns` and an
`[INPUT-WATCHDOG] action=REARM` record with `silent_ms>1000`.

### ITEM-480 (СРОЧНО-29, round 18): the pointer's turn budget and movement coalescing

The round-17 hardware run left the pointer's queue drained by an unbounded
`while (local_input_pop_mouse_event(...))` in the loop, one full cursor drawing
per decoded report.  ITEM-480 bounds the work and sums the movement:

* `local_input.c` keeps the pointer's queue behind a one-report hold, so the
  batch path can look at the next report without consuming it.  A run of
  consecutive movements is summed (`dx`/`dy`) into one draw-worthy batch; a
  `PRESS`/`RELEASE` edge breaks the run and is carried by a batch of its own
  (with the movement decoded in the same report), so no press and no release is
  merged away or reordered.  A report with `dx=dy=0` and no edge is not a
  movement and is consumed without a batch.  `LOCAL_INPUT_MOUSE_BATCH_EVENTS`
  (16) bounds even the coalescing run.
* The loop calls `local_input_pop_mouse_batches(mouse_batches,
  LOCAL_INPUT_MOUSE_DRAW_BUDGET)`: the keyboard's queue is still drained whole
  first, the pointer serves at most `LOCAL_INPUT_MOUSE_DRAW_BUDGET` (3) batches
  per turn, and the unserved remainder waits in the queue for the next turn.
  No input source may make one loop turn unbounded.
* `console_process_mouse_batch()` paints the arrow once for a movement batch and
  calls `framebuffer_sync()` once per batch, not once per report.  The drawing
  (`framebuffer_cursor_show`) and the barrier are measured separately and their
  running averages published as `draw_us=` and `sync_us=`.
* `[POINTER-BATCH] draws= events= batches= draw_us= sync_us= budget=` is
  published beside `[POINTER]` once a second, so a run where the movement
  coalesced shows `draws` far below `events`.

`make test-local-input-host` drives the budget, the coalescing, the button
ordering, the empty movement and the keyboard priority; `make test-console-host`
(`test_mouse_batch_coalesces_and_drains_once`) drives the one-drawing /
one-drain batch.  `scripts/check.d/86-mouse-turn-budget.sh` pins all of it, and
the QEMU proof `test-production-storage-boot` requires `budget=3`, at least one
drawing and `events >= draws` under the keyboard and pointer flood.

### ITEM-481 (СРОЧНО-30, round 19): the durable record is bounded by construction, and no field is cut

The 2026-09-25 hardware journal
(`/srv/hfoc-example/queue/hw8-log-all.txt`) ended its `[PRODUCTION-LOOP]` heartbeat
at `malformed...`: the mirror assembled one line and
`console_record_accept()` cut anything past `CONSOLE_RECORD_MAX` (320) with a
silent `...`.  A counter that grows, or one more field, loses exactly the tail
the record was extended to carry.  ITEM-481 fixes the class rather than one
record:

* The console holds the assembled journal record whole in a buffer sized to the
  worst case of the longest builder (`CONSOLE_RECORD_ASSEMBLY_MAX`, 2560: the
  `[PRODUCTION-LOOP]` heartbeat's fifty-four counters at twenty digits plus its
  literals), with an explicit overflow flag that a future builder cannot defeat.
* The bound `CONSOLE_RECORD_MAX` (320) is enforced while the record is
  assembled: every fragment reserves its worst case (a text fragment its length,
  `console_record_u64()` its full twenty digits, `console_record_hex64()` `0x`
  plus sixteen) and a fragment that would cross the bound records a break at
  that point, so each part's worst case is at or below the bound and no field is
  dropped.  A fragment that ends a field name (`...=`) reserves its value too,
  so a break falls between fields rather than between `name=` and its number.
* Nothing is published during assembly: `console_mirror_finish()` turns the
  recorded breaks into the parts after the serial sequence has ended, so the
  log's own `[BOOTLOG]` serial line cannot split the line the sequence is still
  writing.  The head keeps the record's own name; every continuation carries
  `[<name>-TAIL]`, published in the same tick so a reader associates them by
  their neighbouring sequences.
* `console_publish_journal()` is the same bound for a record a caller built
  itself - the console transcript, a service line - and splits it the same way.
* The assembly records the worst case each part reached.  If a record should
  ever outgrow the assembly buffer anyway, `console_mirror_finish()` publishes a
  bounded `[CONSOLE-BOUND] worst=<n> max=2560` record instead of letting a
  truncation stand silently, and `console_record_bound_violations()` counts it.
  A future field therefore cannot quietly lose a tail.
* The `[PRODUCTION-LOOP]` record, `[LOOP-PHASES]`,
  `[PRODUCTION-INPUT]`/`[PRODUCTION-INPUT-TAIL]`, `[KBD ]`, `[MOUSE]`,
  `[INPUT-DIAG]`, `[PRODUCTION-NET]`, `[PRODUCTION-DHCP]`, `[CONSOLE]` and the
  rest all publish through this one funnel: the main record keeps the leading
  fields and the `-TAIL` records keep the rest, whole.
* The builders that compose into a local buffer before the funnel are sized to
  their own worst case so nothing is dropped before the funnel can split it:
  `XHCI_DIAG_LINE_MAX` (512, the `[INPUT-DIAG]`/`[XHCI-DIAG]` shapes) and its
  overflow flag, which publishes `[XHCI-DIAG] overflow=1 truncated=1` instead of
  a silent drop; the `[FB]` record's 1024-byte buffer; and
  `APPLIANCE_BOOT_LOG_STORE_LINE_MAX` (640, the `[BOOTLOG] state=READY` shape).
  `[XHCI-PORT]` keeps its explicitly bounded 128-byte buffer, whose host proof
  already pins the worst case.

`tests/console-host.c` (`test_record_bound_spills_into_bounded_tails`) drives
both paths: an over-bound assembled record publishes a named head and
`-TAIL` continuations whose concatenation is byte-for-byte the assembled record,
and a pre-built record longer than the bound keeps every byte across its parts.  The QEMU proof
`test-production-storage-boot` reads the durable journal, requires every line
to be at most 320 bytes, requires both sides of `[LOOP-PHASES]` /
`[LOOP-PHASES-TAIL]`, `[PRODUCTION-INPUT]` / `[PRODUCTION-INPUT-TAIL]` and
`[INPUT-DIAG]` / `[INPUT-DIAG-TAIL]`, and fails on any `[CONSOLE-BOUND]`.
`scripts/check.d/87-console-record-bounds.sh` pins the mechanism, the splitter,
the guard, the host proof and the QEMU assertions.

### ITEM-482 (СРОЧНО-31, round 20): typing paints one cell, the keyboard has a turn budget, the input batch drains once

The 2026-09-26 hardware run (`/srv/hfoc-example/queue/hw9-log-all.txt`) published
`kbd_gap_ms_max=4110`, seven one-second mouse silences at `armed=16` with clean
completion codes, and the owner's report that the arrow stays on the screen
after the console fills and refreshes.  ITEM-482 fixes the three input-path
causes and the cursor artefact:

* Normal typing takes a fast path: a character appended at the end of the line
  is `input_line += character` and `console_putc(character)`, so one cell is
  painted instead of a full `redraw_input_line()` that writes `"\r"`, the prompt
  and the whole line (each with its own framebuffer drain) and whose cost grew
  with the line's length.  A backspace at the end erases one cell the same way.
  The full redraw stays for edits that move text: a mid-line insertion,
  LEFT/RIGHT/HOME/END/DELETE, and history UP/DOWN.
* The keyboard's queue is bounded to `LOCAL_INPUT_KEYBOARD_BUDGET` (8) decoded
  events per main-loop turn; the unserved remainder stays queued and is served
  next turn, in order, so no input source can make one turn unbounded.  The
  pointer keeps its own `LOCAL_INPUT_MOUSE_DRAW_BUDGET` (3) coalesced batches.
* `console_end_input_batch()` closes the whole input batch with one
  `framebuffer_sync()` after the keyboard and the bounded mouse batches, instead
  of a drain per batch or per report.  The fast typing path defers its paint to
  the turn flush and pays no drain there.
* The scroll and the body repaint hide the pointer before the body moves and
  draw it again after; `framebuffer_cursor_invalidate()` forgets the saved block
  when a move would otherwise restore stale pixels, so no ghost arrow remains.
* The mouse's silence record now says whether the pointer was in use
  (`active=`), its report rate, the recovery cost and whether a repeat active
  silence called for the stronger reset (`escalate=`); `[PRODUCTION-INPUT-WINDOW]`
  publishes `kbd_last_ms`, `kbd_rate`, `kbd_echo_us_max`, `mouse_rate`,
  `mouse_active_silences`, `mouse_reset_due` and `mouse_recover_us_max`, while
  `mouse_no_trb` stays the loss measure.
* `[LOOP-PHASES]` publishes `kbd_events`, `kbd_budget`, `kbd_draw_us`,
  `kbd_sync_us`, `ptr_events`, `ptr_draws` and `ptr_budget`.  The two count
  fields are the window's worst turn, so `kbd_events <= kbd_budget` and
  `ptr_draws <= ptr_budget` are the proof that no turn was unbounded.

`tests/console-host.c` proves the fast path (`test_typing_appends_without_a_full_redraw`),
the cursor ordering (`test_the_cursor_is_hidden_before_the_body_moves`), the
invalidation (`test_cursor_invalidate_forgets_the_saved_block` in
`tests/framebuffer-scroll-host.c`), the single drain in
`test_mouse_batch_coalesces_and_drains_once`, and the watchdog classification
(`test_watchdog_meaning_and_escalation` in `tests/xhci-host.c`).  The QEMU proof
`test-production-storage-boot` requires the budget fields and the two budget
comparisons.  `scripts/check.d/88-keyboard-fast-path-budgets.sh` pins the
mechanism, the record fields and the proofs.

### ITEM-483 (round 20 follow-up): foreign output forces the typing redraw back to the whole line

The 2026-09-26 post-merge run of `test-production-storage-boot-qemu` on
`c2936841` failed the exact-echo witness with
`HFOC-QEMU-MARKER-TIMEOUT marker="USB keyboard echo of abcdef"`, and the
artifact
(`build/test-artifacts/production-storage-boot-20260926T011050Z-2428944`)
shows the typed `e`/`f` landing on their own serial lines after a
`[PRODUCTION-INPUT]` record.  The input records are journal-only - they paint
nothing - but they are written to the serial stream around the typed characters,
so the ITEM-482 one-cell fast path (which only emits the character, without the
prompt) left the typed prefix non-contiguous on the wire.

`console_output_generation` now counts every writer that is foreign to the line
editor: every `console_emit()` that is not the input echo (and not the deferred
echo's replay), every record opened through `console_mirror_open()` - painted or
journal-only - and `clear_body()`, which paints the framebuffer directly.
`console_input_line_generation` remembers the generation the line was last
rendered against, set by `redraw_input_line()` and by a successful fast-path
cell.  The fast path paints one cell only while the two are equal and the
rendered length still matches; otherwise it calls `redraw_input_line()`, which
re-emits `"\r"`, the prompt and the whole line to both surfaces.  The character
after the forced redraw takes the fast path again.

`tests/console-host.c` proves it
(`test_a_foreign_record_forces_the_typing_redraw`): a mirrored record and then a
journal-only record between keystrokes each make the next keystroke re-emit
`hfoc> ` and the whole line, and the character after a forced redraw paints one
cell again.
`scripts/check.d/88-keyboard-fast-path-budgets.sh` pins the generation, the
fast-path guard and the proof.

### ITEM-484 (round 22): the arrow appears on mouse readiness, and the deferred echo replays inside the input batch

Two residual delays remained after ITEM-482/483.

The first is the pointer's first appearance.  `console_process_mouse_batch()`
set `pointer_ready` but called `framebuffer_cursor_show()` only inside its
movement branch (`if (event->dx != 0 || event->dy != 0)`), so a bound mouse
whose first report was a zero report - or whose first event was a bare click -
left the screen without an arrow until the operator moved the mouse.
`console_pointer_ready()` is the readiness announcement: the main loop calls it
every turn while `local_input_mouse_ready()` is true; the first call centres the
arrow, marks `pointer_ready` and paints `framebuffer_cursor_show()` exactly
once, and later calls are no-ops.  A zero report neither hides nor repaints the
visible arrow, and a click is never the mechanism that makes it appear.

The second is the deferred echo.  When the body is full, `new_line()` opens the
batched scroll (`console_deferring = true`) and every character after the
overflow is buffered instead of painted; the replay is
`console_flush_deferred()`.  The main loop used to call that only at the very
end of the turn, after the network, the heartbeat and the diagnostics, while the
single `console_end_input_batch()` barrier stood earlier - so a character was
processed but visibly waited the whole turn.  The loop now replays the deferred
batch immediately after the keyboard and the bounded mouse batches and before
the one barrier: keyboard budget 8 -> echo -> mouse budget 3 -> deferred replay
-> one `framebuffer_sync()`, and only then the network, the heartbeat and the
diagnostics.  `console_note_echo_paint()` raises the batch drain flag when an
echo cell (the fast path's own or the deferred replay's) is painted; the
end-of-turn `console_flush_deferred()` stays for records and diagnostics.  The
budgets (8/3), the queues, the xHCI transport, the watchdog, the gain and the
batch scroll are unchanged, and the full redraw is not brought back.

`tests/console-host.c` proves all three
(`test_the_pointer_appears_on_mouse_readiness`:
a click without movement does not create the cursor, readiness paints it once
centred, a zero report leaves it alone and the first movement moves it;
`test_many_batches_close_with_one_drain`: three coalesced batches draw three
times and close with a single drain; and
`test_the_deferred_echo_replays_inside_the_batch`: a typed character on a
wrapped, full screen is buffered, then replayed by the in-batch flush, and the
replay is what the batch's one drain makes visible).
`scripts/check.d/89-console-input-batch.sh` pins the mechanism, the loop order
and the proofs, and the QEMU proof `test-production-storage-boot` keeps
requiring `kbd_events <= kbd_budget`, `ptr_draws <= ptr_budget`, the contiguous
exact echo, `kbd_gap_ms_max <= 500` and unchanged loss counters.

### ITEM-486 (round 23): `other_us` names its parts

The owner's 2026-09-26 journal (`/srv/hfoc-example/queue/hw9-log-all.txt`) read one
loop window of `other_us=3 108 092` beside `work_us=3 108 657`, `turn_us=40 992`
and near-zero `console_us=0`, `xhci_us=101`, `input_us=63`, `net_us=401`: a
three-second freeze lived in the unnamed remainder, and neither the journal nor
the operator could tell which subsystem spent it.  `[LOOP-PHASES]` now names the
remainder's parts:

* `svc_us` - services and application state: the clock-driven console upkeep,
  the pointer's record and the local menu offer, the management publication, and
  the service/TLS/serving poll with its records;
* `journal_us` - the once-a-second records and the durable boot log's disk flush
  inside them (heartbeat, input record, loop phases, frame classes, keyboard and
  mouse silence, first-boot and service records);
* `store_us` - storage and queued writes: the deferred first-boot configuration
  commit;
* `other_us` stays the residual after them, so the accounted parts still cannot
  exceed `work_us`, and `worst_phase` can now be `svc`, `journal` or `store`.

The spans are measured outside the phase spans that already existed, folded into
the window with them, and published beside them.  The host proof
`test_remainder_names_its_parts` in `tests/loop-budget-host.c` drives the
arithmetic; the QEMU proof `test-production-storage-boot` reads the three
fields, keeps the existing field set, and adds them to its parts-versus-work
accounting.  `scripts/check.d/91-other-us-subphases.sh` pins the instrumentation.
Behaviour is unchanged: this item only measures.

### ITEM-486 (round 24): the shadow is the surface, the network pass is bounded, the mouse path is named

The same contract fixes the blocking delay the owner measured on hardware
(`kbd_draw_us` in the tens of milliseconds while `kbd_sync_us` was zero, and
`net_us` up to three quarters of a second).  Three mechanisms change, and each
is measured rather than asserted:

* **the RAM shadow is the primary surface.**  `pixel()` now writes the shadow
  and marks the rectangle it widened; it does not store to write-combining video
  memory per pixel.  `framebuffer_flush()` copies the marked rectangles and the
  existing `framebuffer_sync()` barrier calls it before it drains, so every call
  site that already meant "this batch is done" now also writes the device once
  per batch.  A pool of up to eight rectangles keeps a status-bar update and a
  typed character from becoming a full-screen copy.  The panic screen and the
  installer notice flush explicitly because they own their surface.
  `[LOOP-PHASES]` publishes `fb_shadow_draw_us` (the time the primitives spent
  filling the shadow), `fb_dirty_flush_us` (the time the batched device copies
  spent), `fb_dirty_bytes`, `fb_dirty_rects` and `fb_flush_count`, so a reader
  divides the first by the glyph count and sees whether one glyph is
  microseconds or milliseconds.
* **the network receive pass is bounded in time as well as in frames.**  The
  pass already fed the stack at most sixteen frames (`APN_RX_BUDGET_PER_PASS`);
  it now also stops at `APN_SERVICE_BUDGET_US` (2000 us) at a frame boundary and
  returns to the loop.  The frames it did not take stay in the device ring and
  are serviced next turn, so nothing dequeued is dropped.  `[LOOP-PHASES]`
  publishes `net_budget_us` and `net_budget_hits`, the cap and how many passes
  the time bound - not an empty ring - ended, so `net_us` can be read against a
  stated bound instead of guessed from a quiet link.  The deadline is opened
  where the ring is drained, once per receive path, and the time bound may not
  shrink a pass below `APN_SERVICE_MIN_FRAMES` (4): the budget covers the frames
  a pass chooses, not the transmit and link work that ran before it, and a pass
  that finds work keeps a floor of progress.  Both halves are regressions the
  DHCP/DNS proof caught.  Opening the deadline at the top of the whole poll spent
  the budget before the first frame was offered and starved the receive path
  completely in a slow build - "the receive path accepted and refused no frame at
  all on a machine that answered DHCP".  A bound expressed only in measured
  microseconds is also not the same amount of work in an emulated build as on the
  part, because one frame costs tens of times more microseconds there: without
  the floor the path ran at one frame per turn and the boot-time DNS probe
  exhausted its attempt budget, so `reason=DNS_PROBE_TIMEOUT` took `hold=1` and
  the marker the renewal drill is written around became
  `reason=REBIND_TIMEOUT hold=2`.  The placement and the floor are both pinned by
  `scripts/check.d/92-input-latency-shadow-budget.sh`.
* **the mouse event path is named.**  `input_path_classify()` takes the window's
  deltas - armed, completions, decoded reports, local events, drawings, watchdog
  recoveries and framebuffer flushes - and returns one of `NO_COMPLETION`,
  `NO_REPORT`, `NO_LOCAL_INPUT`, `QUEUE_BACKLOG`, `WATCHDOG_REARM`,
  `WATCHDOG_GAVE_UP`, `POINTER_PROCESSING`, `FRAMEBUFFER_RENDER` or `OK`; the
  input record publishes it as `mouse_path`.  `WATCHDOG_GAVE_UP` is the state
  the `hw11` run ended in: the stalled-endpoint watchdog published its recovery
  and, because a fresh arm does not clear the reported flag, never tried again
  while the endpoint stayed armed.  The accessor names that state without
  changing the xHCI transport, ring, arm depth or event-ring architecture.
* **the loop's clocks sit in the record that names the window they disagree
  about.**  `[LOOP-PHASES]` publishes `loops` (the cumulative iteration count),
  `ticks` (the PIT count the loop is paced by) and `real_seconds` beside the
  window's `turns`, `turn_us`, `worst_us` and `over50`.  On `hw9`, `hw10` and
  `hw11` the iteration counter and the window arithmetic read ~1.55x apart, and
  a reader who saw only one of them twice mistook the gap for a factor of eight;
  one record now carries both sides, so the comparison needs no second record.
* **the run's state survives the journal's wrap.**  The durable journal is a
  511-record ring - about seventy-five seconds at these cadences - so the
  motivating 4.7-minute run had already reclaimed the record that named its
  pointer's death by the time the log was read.  `[LOOP-SUMMARY]` is a compact
  record published every fifth phase window whose every field is *cumulative*:
  `loops`, `iter_max_us` (the boot's worst turn), `ticks`, `real_seconds`,
  `mouse_path`, `mouse_completed` (the driver's completion count - the field
  that answers "`mouse_armed>0` and no report: did a physical completion
  happen?"), `mouse_reports`, `mouse_armed`, `mouse_watchdog`,
  `mouse_silent_max_ms`, `mouse_gave_up`, `fb_shadow_draw_us`, `fb_flush_count`,
  `net_budget_us` and `net_budget_hits`.  Because none of them is a window
  delta, the newest summary describes the whole run no matter how many older
  records the ring has overwritten.  It costs about one record in five seconds.

The host proofs link the real modules: `test_shadow_paint_waits_for_flush`,
`test_sync_is_the_batch_flush`, `test_scattered_paints_flush_completely` and
`test_cursor_round_trip_over_shadow` in `tests/framebuffer-scroll-host.c`, and
every outcome in `tests/input-path-host.c`.
`scripts/check.d/92-input-latency-shadow-budget.sh` pins the mechanism and the
record fields, and the QEMU proof `test-production-storage-boot` reads
`net_budget_us=2000`, the framebuffer fields, a known `mouse_path`, the three
clock fields and the `[LOOP-SUMMARY]` record with its cumulative invariants.

## MOUSE-DIAG (2026-09-27): separating device silence from an event-path stall

Two hardware runs ended with a working pointer that stopped for the rest of the
run.  The journal showed the same picture both times: `mouse_armed=16`, the
endpoint context `Running`, every completion code `SUCCESS`, `mse=0`,
`no_trb=0`, `ringfull=0`, `running=1`, `halted=0`, `error=0x0` - and no report.
Nothing on the disk separated the two candidate causes:

- **A - device silence**: the USB mouse stopped sending reports;
- **B - event failure**: the xHCI controller stopped delivering events for that
  device (or stopped consuming its event ring).

The same runs also showed why the evidence was missing.  The durable journal's
pending ring holds 64 records, the loop publishes about 20-24 durable records
per second, and only named moments flush it, so a busy run dropped 2234 of its
3886 records (~58%).  The stall window, the watchdog's own record and the worst
turn's window were among the dropped ones, and the ring keeps ~75 s of what
survives at all.

### What is now published

One set of bounded records per silence episode, written at the watchdog's
**first detection** of that episode and never per loop turn:

| Record | Content |
| --- | --- |
| `[MOUSE-SILENCE]` | `ticks`, `since_completion_ms`, `since_report_ms`, the endpoint's `slot`/`dci`/`ep_state`, `armed`, `pending`, `reposted`, `completed`, `reports`, `last_code`, `last_completion_ticks` |
| `[MOUSE-SILENCE-XHCI]` | the controller's `pci`, `usbcmd`, `usbsts`, `crcr`, `erdp`, `erstba`, `iman`, `imod`, decoded `running`/`halted`/`error`, `host_events`, `ringfull` |
| `[MOUSE-SILENCE-PORT]` | `identified`, `port` and the port's `portsc` with `ccs`, `ped`, `pr`, `pls`, `pp`, `oca`, `csc`, `pec`, `wrc`, `prc`, `plc`, `cec`, `wake`; when the port the device sits on cannot be named, `identified=0` and the raw value of every port the controller read |
| `[MOUSE-SILENCE-AFTER]` | the same controller and port read again after the single re-arm and kick: `erdp_before`, `erdp_after`, `erdp_moved`, `usbcmd`, `usbsts`, `portsc` |

The readers only read: no register is written, no endpoint is reset and no
controller is re-initialised for the sake of the diagnostic.  The watchdog's
recovery stays exactly the re-arm, barrier and doorbell it was; the records
observe it.

### Classification

| Case | Evidence |
| --- | --- |
| A - device silence | `identified=1`, PORTSC keeps `ccs=1`, `ped=1`, `pp=1`, a link state other than `Inactive`, no change bit set - and `erdp` does not move after the re-arm |
| B - event failure | the controller reports `halted=1`, an `error` bit, `ringfull`/missed-service growth, or `erdp_moved=0` while the port stays connected and the device keeps a live link |
| C - both look healthy | PORTSC and the controller healthy, `erdp` frozen, no new reports: the evidence names the leg (device to controller) but not which end stopped |
| D - correlated controller change | a status, error or event-ring change whose `ticks` coincide with the stall |
| E - not enough data | the records are absent or unreadable: the honest answer, never a verdict |

### The EP0 probe: does the device still answer at all?

`hw13` narrowed the stall to the transfer leg - the port stayed connected and
enabled (`PORTSC ccs=1 ped=1 pls=U0`, every change bit clear), the controller
stayed running, the endpoint stayed `Running` with sixteen transfers armed - and
no completion arrived.  That leaves two mechanisms: a device whose *interrupt
pipe* went quiet, or a slot/device that stopped answering altogether.

At the first detection of a silence episode the driver therefore issues **one**
standard `GET_STATUS` control transfer on the same slot's EP0 (`[MOUSE-EP0]`,
`[MOUSE-EP0-STATE]`), with the state on both sides of it:

| Field | Meaning |
| --- | --- |
| `submitted=`, `code=`, `elapsed_us=` | whether the driver could place the transfer, its status-stage completion code and what the driver's own bounded transfer budget cost. `code=255` (`XHCI_EP0_NOT_SUBMITTED`) means nothing was observed; `code=254` (`XHCI_EP0_NO_COMPLETION`) means the status stage never completed |
| `ep_before=`/`ep_after=`, `slot_before=`/`slot_after=` | the endpoint and slot context states around the probe |
| `portsc_before=`/`portsc_after=`, `usbsts_before=`/`usbsts_after=`, `erdp_before=`/`erdp_after=`, `erdp_moved=` | the port, the controller status and the event-ring dequeue pointer around it |

Classification: a `SUCCESS` status stage is `INTERRUPT_ENDPOINT_OR_DEVICE_REPORT_PATH`
(the control plane answers; the interrupt pipe is what went quiet); no completion
inside the budget is `DEVICE_OR_SLOT_UNRESPONSIVE`; a transfer the driver could
not place at all is `NOT_TESTED`.

The probe issues one control transfer, writes no register of its own, performs
no port reset, no controller reset, no Stop Endpoint, no Set TR Dequeue Pointer,
no address change and no re-enumeration, and it is bounded by the driver's
existing transfer spin budget - it observes the stall, it does not repair it.
`[MOUSE-EP0` is a journal flush marker like the snapshot itself.

### The two telemetry fixes from `hw13`

- `[MOUSE-SILENCE] since_completion_ms=` / `last_completion_ticks=` read
  `last_report_ticks` now.  They had read `transfer_complete_ticks`, which the
  re-arm clears, so the record showed `0` while the device had answered 13471
  times.
- The worst-turn maximum no longer depends on the ring: `[LOOP-SUMMARY]`
  carries `iter_max_phase=`, `iter_max_ticks=` and `iter_max_second=` beside
  `iter_max_us=`, so the last maximum survives to the shutdown even when the
  once-per-maximum `[ITER-MAX]` record has been recycled (as it was in `hw13`,
  where `iter_max_us=3809337` had no record left).

### The input verdict, and the three boots it survives (ITEM-497)

The 2026-09-30 `hw20` run came up once with no keyboard and no pointer, and the
next boot of the same machine came up with both.  The durable ring holds 511
records while one boot publishes thousands, so the boot that failed had been
overwritten before the machine was switched off - the failing boot was not
merely unreadable, it was gone.

`[INPUT-PATH]` is the record that answers it, one line per boot (or per state
change), written as a flush marker:

```
[INPUT-PATH] boot=17 verdict=ARMED keyboard=ARMED mouse=ARMED arm_ms=812
    first_report_ms=1340 ports=2 reports=1823
```

`verdict=` is the worst of the roles that are present: `ARMED` (bound, armed and
reporting), `NO_REPORTS` (armed and silent), `NOT_ARMED` (bound and never
armed), `ABSENT` (no HID device on that role) and `NO_DEVICE` (neither role).
`arm_ms` and `first_report_ms` are milliseconds from boot to the first role that
became ready and to the first report; `0` means it never happened.

A ring cannot keep that line by itself: the next boot writes over it.  The
record is therefore carried.  At attach the log reads the tail of its own extent
back (`appliance_boot_log_scan_input_verdicts`), keeps the newest verdict of
each boot number - a `[INPUT-PATH-CARRY]` copy is the same boot's own body, not
a second entry - and re-states the current verdict plus the two boots before it
on the loop heartbeat (`[PRODUCTION-LOOP]`, at most once per ten seconds).  The
carried line is the earlier boot's own body with the `-CARRY` marker, so a
reader sees which boot failed, and its verdict, three boots later.

The periodic record families are bounded by a repeat gate, by two rules.  The
log's own bookkeeping - `[BOOTLOG] state=READY`, `[BOOTLOG-FLUSH` - carries
counters that change with every flush, so it is admitted at most once per 30
seconds.  The per-second snapshots - `[INPUT-DIAG]`, `[LOOP-DIAG]`,
`[XHCI-DIAG]` - are their content: a record identical to the last admitted one
of its family is a repeat of an unchanged state and is held back, while a record
that changed (a counter moved, a role became ready) is published at once, and a
family that stays unchanged is still restated once per 10 seconds.  A live
controller therefore keeps its per-second evidence, and a controller with no
device stops repeating the same line.  An `[INPUT-PATH]` verdict,
`[INPUT-WATCHDOG]`, `[MOUSE-SILENCE*]`, `[ITER-MAX]`, a refusal and the
boot/install/serving records are not gated at all: a state change is written the
moment it happens.

Read back: `scripts/hfoc-esp-read.py --bootlog` prints the records in sequence
order, so `[INPUT-PATH]` and `[INPUT-PATH-CARRY]` lines for several boots appear
in one file; `BOOTLOG` on the console shows the staged records of the boot in
progress.

ITEM-518 makes that console reading finite.  The dump publishes each staged
record through the same funnel the boot uses, the console journal hands the
record back to the log's `observe`, and `observe` appended it to the ring - so
the `next_sequence` the loop was comparing against moved with every line the
command printed, and `bootlog` on the owner's laptop never returned.  The dump
now captures the end once, ignores its own output while it runs
(`store.dumping`), reads only the staging window
(`APPLIANCE_BOOT_LOG_PENDING_CAPACITY` records), publishes a
`BOOTLOG: progress=N of=M` line every 16 records, and ends with
`BOOTLOG: shown=N of=M done`: one command is a bounded amount of work, and a
screen that is still printing says so.

### The `storage` command

`appliance-boot` publishes storage only when a persistent volume was published,
so on the machine booted from the installation medium it says nothing about
storage at all - exactly the machine whose installer refused `NO_DISK`.  The
`storage` command (ITEM-518) renders the observed storage report the boot
published instead, and works whether or not a state volume exists:

```text
Observed storage
  controllers: class_ahci=0 class_nvme=0 brought_up=0 failed=0 reason=NO_CONTROLLER
  disks: found=0 accepted=0
```

It is the same model the installer's refusal page prints and the same
`[STORAGE-REPORT]` record the flow publishes, so the screen a person reads and
the command he types say the same numbers.  A machine whose probe never ran
answers `storage was not probed state=NOT_PROBED` instead of zeroes that would
read like a finding.  Labels follow the console language like every other
console label; counts, identities and reason tokens stay machine text.

### The journal keeps them

`[MOUSE-SILENCE` (all four records), `[INPUT-WATCHDOG]` and `[ITER-MAX]` are
journal flush markers: writing one flushes the pending ring, so the snapshot of
a once-per-episode event reaches the disk instead of being dropped like an
ordinary periodic record.  Production flush semantics are otherwise unchanged.

### The iteration maximum and the keyboard echo

`iter_max_us` was published as a bare boot maximum (`3259516` on the
2026-09-27 run) with no phase and no clock.  Each new maximum past the
long-turn budget (`LOOP_BUDGET_LONG_TURN_US`, 50 ms) now writes one
`[ITER-MAX] ticks= second= loops= iter_max_us= phase=` record, so the number
can be attributed or dismissed as an artifact.

`kbd_draw_us` said how long a keystroke's echo took but not what it did.  The
phase window now also publishes `[KBD-ECHO] fast= redraw= redraw_chars=
redraw_us= redraw_us_max= gen_invalid=`: the ITEM-482 one-cell paints, the
whole-line redraws, the characters those repainted, their cost, and the
ITEM-483 fallbacks where another writer had moved the screen.  The echo's own
decision is unchanged.

The pure halves are proven by `tests/xhci-host.c` (`test_silence_records`, which
also pins every record's worst case inside the console's 320-byte line budget
and the `identified=0` fallback) and `tests/loop-budget-host.c`
(`test_iter_max_publish_policy`); `scripts/check.d/94-mouse-silence-diagnostics.sh`
pins the records, the markers, the counters and the unchanged watchdog recovery.

### ITEM-487: the durable log's flush writes a run at a time, and says what it cost

`hw15` measured the durable boot log's flush on the acceptance machine: about
42 ms for every 512-byte record, 2.7-3.2 s of blocked main loop for a 64-record
flush, and the keyboard's last completion 3.45 s behind the flush that blocked
it.  The cost was structural, not incidental: the flush wrote one slot per
`efi_fat32_esp_write_file_range()` call, and every call resolved
`\EFI\HFOC\BOOTLOG.BIN` again and walked the file's FAT cluster chain from its
first cluster to the offset - with the production ESP's one sector per cluster,
that is one sector read per slot already written, paid again for every record.

The fix is the write path, and nothing else:

* `appliance_boot_log_ring_peek()` reads the `index`-th pending record **without
  retiring it**, so a flush can render a whole run before it writes anything;
* the flush collects consecutive slots into a bounded 64-slot buffer, hands them
  to the range writer in **one** call - the writer already handles a range that
  crosses clusters, a range that ends at the extent's last byte and a range that
  is not sector aligned, and it still refuses one that would grow the file - and
  retires the records only after the bytes are on the device, so a refused write
  leaves the whole run staged and the ring's `written_sequence` stays the honest
  count of what the extent holds;
* the run ends where the extent's own ring wraps (the next slot is not the next
  index) and never spans it, so one flush is at most two range writes;
* the record format, the extent, the flush moments and the drop policy are
  unchanged: this is the same bytes, written in the same order, retired under
  the same rule, and no record is hidden to make a flush cheaper.

The flush's own reading grew the fields that name the cost.  `[BOOTLOG-FLUSH]`
carries `records`, `text_bytes`, `slot_bytes`, the measured `esp_write_us`,
`commit_us` and `total_us`, and its `[BOOTLOG-FLUSH-TAIL]` continuation carries
`publish_us` (named `publish=derived` because the plan walk and the slot write
share one loop), `lookup_us` and `fat_traversal_us` from the ESP writer's own
probe, `batch_count` - how many range writes carried the records - and
`unavailable=esp_blocks,clusters` for what this layer cannot read.  Both halves
are published once per new flush, to the serial stream and to the durable
journal, and a half that would not fit its 320-byte bound is refused rather than
truncated.  The probe is a reading, never a policy: no path in the writer
branches on it, and the cycle counter it measures with is reached through weak
symbols, so a host proof without a timer reads zeroes instead of failing to
link.

`tests/appliance-boot-log-store-host.c` reads the accounting for flushes of 1,
2, 29 and 64 records and requires one range write for the whole run, one path
lookup and one chain walk for it, two range writes for the flush that crosses
the extent's ring, and nothing retired by a run whose write was refused;
`tests/efi-fat32-esp-host.c` writes an eight-sector run across eight clusters
and the extent's last sector, and pins the probe's counts.
`scripts/check.d/93-item-487-bootlog-flush-batch.sh` pins the mechanism and the
record fields.

## ITEM-509 increment 7: the console help is the catalogue

`help` was one line of command names. It is now the console renderer of the
help catalogue, and the item's one content has two renderers - the panel
renders the same four strings in increment 8:

* `help` prints the table of contents: every topic id with its one-line
  summary, the usage line, and the command vocabulary line (`COMMANDS: HELP
  ...`) that existing operators and proofs read;
* `help <topic>` prints that topic's title, summary, body and related commands
  and sections; `help all` prints every topic;
* the word and the topic id fold by ASCII case, one space separates one
  argument, and an id no topic carries is refused with `NO SUCH HELP TOPIC`
  plus the usage - help never answers an unknown question with silence;
* the nine topics are `install`, `network`, `storage`, `account`,
  `certificates`, `names`, `panel`, `recovery` and `records`; every sentence
  lives in the one language catalog, so `help` follows the active language and
  the Russian bodies are covered by the glyph proof like any other Russian
  sentence;
* a rendered line is one IRQ-safe write (`console_write_atomic()`), so an IRQ
  trace record cannot land inside a topic title; a catalog that is incomplete
  answers `HELP IS NOT AVAILABLE` and records
  `[CONSOLE] help unavailable reason=CATALOG_INCOMPLETE` instead of printing
  half a topic.

The catalogue grew the measured code-point inventory from 70 to 127, so
`APPLIANCE_LANGUAGE_CODEPOINT_MAX` is 192: a caller bound with headroom, not a
glyph rule - the coverage proof still fails on any code point the font cannot
draw. The help content is checked against the code it describes: statements
the tree does not support (a Storage read side, a working recovery entry, a
Diagnostics log owner) are not in it.

## Boundaries

- ITEM-343 owns the first-boot wizard control boundary.
- ITEM-344 (this item) owns local appliance console presentation.
- ITEM-345 owns HTTPS management and web-panel presentation.
- ITEM-346 owns integrated installer/dual-boot/management acceptance.

## Non-Goals

- No desktop environment or full-screen graphical UI.
- No remote serial/network console.
- No pointer, no touch input: the local menu is driven by the keyboard.
- The local menu reboots and powers the machine off (ITEM-418/419) and runs the
  read-only status, network and diagnostic commands; recovery, storage,
  network, service and management *mutation* stay outside it.
- No replacement for the existing diagnostic `appliance` command.
- No actual physical-hardware acceptance.
- No VERSION or release-number change.

## ITEM-509 increment 3 — кириллица и UTF-8 на экране

Экран печатает русский текст.  Шрифт `kernel/src/framebuffer.c` несёт
прописные и строчные `А..Я`, `Ё`/`ё` и знаки `«» — … №` в той же клетке 5x7,
что и латиница (строчные — по конвенции ITEM-444: x-height в строках 2..6,
выносные до 0, нисходящие до 6); латинские таблицы и `glyph_rows()` не
тронуты.  Печать идёт по кодпойнтам: `framebuffer_glyph_rows()` отвечает, есть
ли глиф, `framebuffer_char_codepoint()` рисует клетку, а кодпойнт без глифа
получает видимый маркер (рамка с точкой), счётчик и запись
`[GLYPH-MISSING] codepoint=U+XXXX[ byte=0xXX] count=N` в журнал.  `console.c`
декодирует вывод строгим UTF-8 каталога
(`appliance_language_utf8_next()`): валидная последовательность — одна клетка,
перенос и обрезка считаются по кодпойнтам, а не по байтам; serial и транскрипт
получают исходные байты, поэтому записи, маркеры и пробы, читающие serial, не
сдвинулись.  Отложенная пачка (`console_defer_buffer`) хранит кодпойнт целиком,
чтобы разрез последовательности на границе 4 КиБ не превращался в маркер плюс
чужой байт.  `framebuffer_char()`/`framebuffer_text()` сохраняют прежний
побайтовый контракт: экраны установщика, паники и заставки не меняются.
Доказательства — `make test-framebuffer-glyph-host` (гейт покрытия по
инвентарю каталога) и `make test-console-host` (одна клетка на кодпойнт,
перенос, маркер и запись), срез
`scripts/check.d/167-item-509-cyrillic-glyphs.sh`.

## ITEM-509 increment 4 — команда `language`

Консоль понимает `language` (показать: `LANGUAGE: EN` / `ЯЗЫК: RU`) и
`language en|ru` (задать).  Слово команды сворачивается по регистру, значение —
нет: `language EN`, `language de`, `language  en`, `language en extra` и
`language ` — один и тот же отказ `UNKNOWN LANGUAGE: use en or ru` с подсказкой
`USAGE: language en|ru`; `languages` и `lang` командой не являются.  Ответы
печатаются на действующем языке (подтверждение — уже на новом: `language ru`
отвечает `ЯЗЫК СОХРАНЁН`), а меняется язык только после успешной записи
системной записи: владелец пишет её через `console_language_binding`, консоль
без записи или при отказе записи отвечает `LANGUAGE IS NOT AVAILABLE` и
остаётся на прежнем языке.  Каждый исход оставляет английскую машинную запись
`[CONSOLE] language=<код>` или `[CONSOLE] language=<значение> refused
reason=UNKNOWN_LANGUAGE|UNAVAILABLE|WRITE_FAILED`.  `LANGUAGE` добавлена в
строку справки `COMMANDS: ...`.  Доказательства — `make test-console-host` и
QEMU `test-appliance-console-qemu` (живая консоль печатает обе команды, и в
серийном журнале нет `[GLYPH-MISSING]`), срез
`scripts/check.d/168-item-509-console-language.sh`.

## ITEM-509 increment 9 — экран boot-консоли говорит на выбранном языке

Экран, который печатает `appliance-boot`, получил язык явным параметром:
`appliance_boot_console_render(snapshot, language, output, capacity,
output_length)`.  Подписи полей, строка `Local administration:` и шесть строк
меню — из общего каталога (`menu.bootconsole.*`), английские значения дословно
прежние, поэтому все прежние пины и QEMU-маркеры целы, а русские подписи держат
ту же 15-клеточную колонку (например, `Hostname:      ` → `Имя узла:      `).
Отсутствующий в каталоге ключ — `APPLIANCE_BOOT_CONSOLE_BAD_STATE_VALUE` (консоль
печатает `[APPLIANCE-BOOT] RENDER FAILED`), а не половина экрана.

Границы: имя продукта `HFOC Appliance`, значения состояний (`OK`,
`UNAVAILABLE`) и машинные слова за строками меню (`status`, `reboot`, …)
остаются английскими — это значения и команды, а не подписи.  Язык приходит из
`console_active_language()`, той же опубликованной настройки, что читает команда
`language`; второй копии нет.  Доказательство: `make
test-appliance-boot-console-host` (русский экран посимвольно, английский —
байт в байт), `make test-console-host` (`appliance-boot` под опубликованным
`en`), QEMU `test-appliance-boot-console-qemu` без сдвига маркеров; срез
`scripts/check.d/173-item-509-menu-language.sh`.

## ITEM-550: why the command never completed is a record, not a detail

The owner's 2026-10-25 run on the Toshiba L875 (the diagnostic ISO built from
`cae07a5d`, booted from the only USB 2 port) read the controller as
`[XHCI-DIAG] UNAVAILABLE last_probe=1 pci=00:14.0 caplength=0x80
mmio_bytes=65536 reason=ENUMERATION_FAILED detail=P1_ENABLE_SLOT_TIMEOUT`.
That answer is already an advance over ITEM-548's run: the Intel 7 Series
capability block is accepted, the controller is halted, reset and started, and
its command ring completes the No-Op command `xhci_controller_start()` submits
before any port is touched.  What the record could not say is *which* command
never completed and *what the controller looked like* when it did not: the
submitted TRB, its place in the command ring, the ring's producer cycle, CRCR,
MaxSlotsEn, the device context array, the event ring's segment table and dequeue
pointer, the interrupter, and the events the wait actually dequeued were all
local to `xhci_submit_command()`, in a session that the failed probe releases
before the `xhci` command runs.

This item is diagnostic only: it changes no timeout, no enumeration step, no
port order and no interrupt routing.  Inside the command wait it counts what it
sees, and when the wait gives up it captures one bounded snapshot - with reads,
never writes - into the machine's own RAM (`struct xhci_machine`, beside
ITEM-548's `last_probe`), publishes the same three records to the durable
journal through `console_publish_journal()`, and returns.  Nothing is printed
inside the wait loop, so a controller that is merely slow cannot be made slower
by its own diagnostic, and the `xhci` command prints the kept copy afterwards
without touching MMIO - the copy is a value, so releasing the session (which
zeroes it) cannot take the record away.

The three records, bounded by `XHCI_COMMAND_RECORD_BYTES` (320 bytes, the
console's own journal line bound):

* `[XHCI-CMD] result=SUCCESS|ENQUEUE_FAILED|TIMEOUT pci=bb:dd.f type= index=
  cycle= phys= trb=` - the outcome, the controller, the submitted TRB's type,
  the ring slot it landed in, the ring's producer cycle, its physical address
  and all four dwords the ring holds;
* `[XHCI-CMD-REG] usbcmd= usbsts= crcr= config= dcbaap= erstsz= erstba= erdp=
  iman=` - the registers the cause depends on, read at the failure point
  (USBCMD.RS and USBSTS.HCH say whether the controller still ran, CONFIG holds
  MaxSlotsEn, CRCR holds the ring base and its consumer cycle state);
* `[XHCI-CMD-EVT] index= cycle= event= events= unmatched= last=addr=,type=,
  code=,slot=` - the event-ring consumer position and producer cycle, the four
  dwords of the last entry the wait read (`event=none` when it read none), how
  many events it consumed, how many Command Completions named a *different*
  TRB, and that last foreign completion's address, type, completion code and
  slot.

`result=` is the distinction the old single `false` return could not make: an
`ENQUEUE_FAILED` command never reached the controller (the ring refused the
TRB), a `TIMEOUT` command was accepted and its completion never came, and a
wait that only ever saw `unmatched=` completions is a third case - the
controller is producing events, just not for this command.

Proof.  `make test-xhci-host` drives the pure half: a successful completion
records nothing, an accepted-but-unanswered command is a `TIMEOUT`, a refused
TRB is an `ENQUEUE_FAILED`, a completion for another address is counted and its
address/type/code/slot are kept, and the record formats all three lines after
the capture buffer is wiped.  `make test-xhci-mmio-host` drives the real
`xhci_submit_command()` against RAM-backed MMIO: the driver takes its own
completion and records no failure, an empty event ring produces the timeout
snapshot in the machine record and in the journal, and the session release that
follows leaves the `xhci` command able to print all three records - and the
snapshot is required to have written no operational register (only the
submission's own doorbell moved).  `scripts/check.d/234-item-550-xhci-command-snapshot.sh`
pins the vocabulary, every field, the absence of any print inside the wait loop,
the machine-record ownership and the copy, the No-Op-before-ENABLE_SLOT order
that makes `ENUMERATION_FAILED` the honest verdict, the host proofs and these
documents.

What the next hardware run answers, field by field: `config=` with
MaxSlotsEn = 0 makes ENABLE_SLOT unanswerable; `usbcmd=` without RS or
`usbsts=` with HCH says the controller stopped running between the No-Op and
the slot command (a port reset is the candidate); a `crcr=` that is not the
command ring's address with RCS says the ring the driver wrote is not the ring
the controller reads; `erdp=`/`index=`/`cycle=` frozen with `events=0` says the
event ring never produced anything; and `unmatched=1` with a foreign `last=`
says the controller is alive but answering a command the driver is not waiting
for.  Until the mass-storage path works the journal copy may not reach the
medium, so the console output of `xhci` is the primary evidence of that run.
