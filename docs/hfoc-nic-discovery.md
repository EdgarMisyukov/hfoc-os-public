# HFOC NIC discovery: the machine's network controllers, described

ITEM-492.  What the product knows about the network controllers a machine has,
why "is there a Realtek part?" was the wrong question, and the three readings
that used to be one.

## What already existed, and what this item changes

The production path asked one question - whether the RTL8168 driver could find
its own part - and answered it inside that driver's file.  A machine whose NIC
was anything else, and a machine with no NIC at all, produced the same reading,
and the only driver in the image was the only network device the product could
describe.  Each driver also carried its own copy of the PCI configuration-space
access (0xCF8/0xCFC), and the PCI filter could not express "class 0x02, any
subclass".

This item makes the description a property of the machine and the driver list a
property of the image:

```text
PCI bus (class 0x02, any subclass)
        │
        ▼
nic_discovery.c      walk, one device at a time, independently
        │            refuse the whole list rather than truncate it
        ▼
nic_registry.c       value machine: a backend claims an id, a device
        │            carries the identity and the state
        ▼
[NIC] bdf=… vendor=… device=… driver=… mac=… state=… reason=…
[NIC] devices=N supported=N available=N unsupported=N error=N absent=N
```

`nic_backends.c` is the single list of claims this image carries
(virtio-net transitional and modern, RTL8168), and `pci_io.c` is the one
configuration-space provider every driver now shares.

## The rules

**A device is described by the hardware, not by the driver list.**  Discovery
walks the PCI bus for the network class and reports the address and the ids the
hardware itself reports; the registry then answers which driver, if any, claims
that part.  Nothing in `nic_discovery.c` names a vendor.

**A machine with no NIC, a NIC nobody wrote a driver for, and a driver that
refused are three readings.**  They are `absent=1`, `state=UNSUPPORTED
reason=NO_DRIVER` and `state=ERROR`, and the record states which one happened
instead of leaving it to be inferred from silence.

| state | meaning |
| --- | --- |
| `SUPPORTED` | discovered, and a registered driver claims it; nothing has been initialised yet |
| `AVAILABLE` | discovered, and its driver initialised it: the machine can use it |
| `UNSUPPORTED` | discovered, and no registered driver claims it |
| `ERROR` | discovered, and the driver that claims it refused or failed |
| `ABSENT` | no device was found at all - a reading, not a failure |

**A capability reading never claims a working device.**  The installer kernel
carries no network stack, so its scan runs without a bind callback and every
claimed device stays `SUPPORTED`; `available=` can only become non-zero where a
driver actually initialised the device.  A driver in the image is not a device
that works, and no record pretends otherwise.

**The bind result lands on the record it belongs to.**  The production network
path scans the same bus into its own table, and each physical bind point - the
Realtek path and the virtio path - publishes the outcome for the controller it
drove: `AVAILABLE reason=OK` when the frame path exists, `ERROR` with the
driver's own reason (`RX_NOT_ARMED`, `PCI_BUS_MASTER_REFUSED`, `LINK_DOWN`, ...)
when it refused.  The record is reprinted only when the bind changed the
reading, so a link that is retried while the cable is out leaves one refusal
behind instead of one per attempt.

**One device does not hide another.**  Every controller is classified
independently, so a machine with a supported NIC and an unsupported one reports
both.

**A truncated list is a machine misdescribed.**  More controllers than the
device table holds is `NIC_DISCOVERY_CAPACITY` and no records, never the first
`NIC_DEVICES_MAX` of them.

**An ambiguous claim is refused.**  Two backends claiming the same ids is a
registration failure, not a decision made by table order.

**The network is not a precondition of installing.**  The install flow publishes
its reading and states the rule in the record:

```text
[INSTALLER-NIC] devices=N network=<state> install=CONTINUES
```

## What was deliberately not changed

* The RTL8168 driver, its claim (10EC:8168) and the hardware it drives.  This
  item moves the claim into the registry; it removes no driver.
* The installer's target selection, the install flow's behaviour and the boot
  order.  The `[NIC]` reading is published next to the existing records.
* `VERSION`, `third_party/**`, APIC/IOAPIC/SMP and the existing test gates.

## Evidence

* `tests/nic-registry-host.c` and `tests/nic-discovery-host.c` drive the value
  machine and the walk over a fake configuration space, including the refusal
  of a duplicate claim, the refusal of an overfull table, and the difference
  between an unclaimed device and a failed bind.
* `scripts/test-qemu-nic-discovery.sh` boots the real install medium in
  QEMU/OVMF on machines that differ only in their network hardware: an Intel
  e1000 (`driver=E1000 ... SUPPORTED reason=DRIVER_PRESENT`, still
  `available=0`), an e1000 plus a virtio-net function (`devices=2 supported=2
  available=0`), and a machine with no controller at all (`devices=0 ...
  absent=1`).  In all three the install reaches its own records with
  `install=CONTINUES`.
* `scripts/check.d/98-item-492-nic-discovery.sh` pins the contract in the
  canonical check suite.
* `tests/e1000-host.c` drives the 8254x core over a fake register file -
  identity and BAR selection, the MAC read and its validation, the reset
  handshake, both legacy rings, the descriptor state machines and the ring
  arithmetic - and `scripts/check.d/99-item-494-intel-nic.sh` pins the second
  vendor's contract: one claimed part rather than a vendor, the RTL8168 claim
  untouched, interrupts masked and never enabled, the address on the record
  coming from the part, and this document separating the QEMU/OVMF proof from
  physical acceptance.
* `make test-production-dhcp-dns-e1000-qemu` is the item's main proof: the
  ordinary-boot DHCP/DNS/SNTP stand with one emulated Intel 82540EM and no
  virtio-net function, so the lease, the name and the clock can only come from
  the Intel path.  It requires the machine's own records to name that path -
  `[NIC] ... driver=E1000 mac=52:54:00:12:34:56 state=AVAILABLE` and
  `[PRODUCTION-NET] ... backend=E1000` - which is what distinguishes "the
  driver came up" from "the production architecture does not depend on one
  vendor".

## The second vendor: Intel 8254x (e1000)

The reading above is only worth something if a second vendor can be added to it
without rewriting it.  ITEM-494 adds the Intel 8254x family, starting with the
82540EM (`8086:100E`) - the part QEMU's `e1000` device emulates - and the price
is worth stating plainly.

**What a vendor shares.**  Discovery by class, the one registry of claims, the
`[NIC]` record shape, the state names and the rule that a bound record carries
`mac=` only from an address the part really gave.  None of that is touched by
the second vendor: it registers a claim like every other backend.

**What a vendor owns.**  Everything that is silicon: identity and BAR, the
reset handshake, the MAC registers, the multicast table, the two legacy
descriptor rings and their base/length/head/tail registers, the descriptor
format (16 bytes, `DD`/`EOP`/error bits), the receive-control enable with its
read-back, and the link bit (`STATUS.LU`).  That is `kernel/src/e1000.c`, proven
by `tests/e1000-host.c` over a fake register file.  A vendor is therefore not a
row in a table - the row is the cheap part.

**What a vendor must not do.**  Change the interrupt architecture: the 8254x
driver masks interrupts (`IMC`, then `IMS` left empty) and is polled, like the
RTL8168 driver and the rest of the appliance (ITEM-389).  No MSI, no APIC, no
IOAPIC, no SMP, no xHCI.  A second NIC vendor is a driver, not an interrupt
change.

**What an `AVAILABLE` record must never say.**  A zero address is a valid MAC,
so "the part gave no address" is spelled `mac=NONE`, never `00:00:00:00:00:00`:
the RTL8168 path refuses to bind when neither the extended registers nor the
IDR yield an address that passes `rtl8168_mac_valid()`, and the 8254x path
applies the same rule with `e1000_mac_valid()`.

**The reading on a machine with an Intel controller**, as far as it is proven
today - the record `scripts/test-qemu-nic-discovery.sh` asserts from the
QEMU/OVMF machine whose only controller is the emulated 82540EM:

```text
[NIC] bdf=00:02.00 vendor=8086 device=100E driver=E1000 mac=NONE state=SUPPORTED reason=DRIVER_PRESENT
[NIC] devices=1 supported=1 available=0 unsupported=0 error=0 absent=0
```

`available=0` is the honest part: a driver in the image is not a device that
works.  The production bring-up path binds the Intel part on the same
composition the Realtek path uses, so the record becomes `AVAILABLE` only when
the frame path really exists, and it then carries the address the driver read:

```text
[NIC] bdf=00:02.00 vendor=8086 device=100E driver=E1000 mac=52:54:00:12:34:56 state=AVAILABLE reason=OK
[PRODUCTION-NET] device=PRESENT readiness=READY backend=E1000 ... mac=52:54:00:12:34:56
```

That pair is what `make test-production-dhcp-dns-e1000-qemu` requires from the
emulated Intel machine, and `available=1` in the summary is checked with it.

**Where the proof comes from.**  The acceptance host carries exactly one network
controller, a Realtek RTL8111/8168 (`10ec:8168`), and no Intel part, so a
physical Intel run is deferred until a machine with an Intel controller runs it
- the physical run stays human-triggered (AGENTS.md 3a).  Until then the 8254x
is proven by the host value machine and by QEMU/OVMF, and it is not called
hardware-proven.

**What that proof caught before it passed.**  The first full runs of the Intel
stand failed on the driver's own register map, not on the emulation: the link
was read as `STATUS` bit 2 (which is `TXOFF`) instead of bit 1 (`LU`), so a
machine whose link was up read down for a whole boot; and the receiver enabled
`RCTL.SBP` ("store bad packets") instead of `RCTL.BAM` ("broadcast accept"), so
every broadcast DHCP offer was dropped while the driver reported a bound
receiver with the ring armed.  Both mistakes are now pinned by
`tests/e1000-host.c` and `scripts/check.d/99-item-494-intel-nic.sh`.  They are
also why this item's main proof is the whole DHCP/DNS/SNTP stand: "the driver
came up" would have passed both times.

## Boundaries

QEMU/OVMF proof is not physical hardware acceptance.  QEMU cannot emulate the
acceptance host's RTL8168 (0x10EC:0x8168); that part is covered by the
registry's matching proof on the host and by the physical acceptance run.  The
installer kernel's reading is a capability reading: the network stack of the
installed appliance is a separate item.
