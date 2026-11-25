# HFOC Known Limitations

**Release ID:** HFOC-APPLIANCE-V1

## Installer And Storage

- Production installer, EFI filesystem, UEFI NVRAM, physical storage, and
  recovery/factory-reset controllers are software boundaries for an approved
  installer or recovery environment.
- Normal boot has no general writable filesystem and does not activate
  destructive installer, UEFI, recovery, or factory-reset mutation.
- Physical clean, dual-boot, and manual installation remain NOT TESTED.
- Physical SATA/NVMe, PCI, firmware, and UEFI NVRAM acceptance remain NOT
  TESTED.

## Boot And Recovery

- Existing boot entries are modeled and preserved through deterministic
  software boundaries and OVMF QEMU evidence.
- No physical UEFI filesystem/NVRAM or bootloader repair has been recorded.
- Physical bootloader recovery and existing-OS boot remain NOT TESTED.
- Physical recovery mode and factory reset remain NOT TESTED.
- A machine with no usable PS/2 controller boots (ITEM-392) and, when it has
  an xHCI (USB 3) controller with a HID boot-protocol keyboard on a root port,
  drives its local console from that keyboard (ITEM-395): the boot publishes
  `[KBD ] state=READY reason=USB_HID … source=USB_HID` and
  `local_input=AVAILABLE input_source=USB_HID`. Every xHCI controller the
  machine has is enumerated in PCI order and every root port of each, and the
  scan stops only at a usable boot keyboard (ITEM-406): a controller whose
  devices are not keyboards - the acceptance host's first controller carries a
  wireless receiver - costs a bounded number of attach attempts and the scan
  goes on to the controller that has the keyboard. Each attempted controller
  publishes its own `[XHCI]` record, and the rejection names the controller,
  the port and the stage (`detail=CTRL_05:00.3_…`, `P7_CONFIG_PARSE_…`). The
  USB input path is polled from the ordinary-boot event loop rather than
  interrupt driven, so keyboard latency follows the loop (about 10 ms);
  APIC/IOAPIC/IRQ routing is untouched.
- A machine with neither PS/2 nor a USB HID keyboard boots, publishes
  `source=NONE` and `local_input=UNAVAILABLE`, and stays manageable through the
  network management plane; the local console is then a display only, and the
  serial console is a publication channel rather than an input path.
- The USB HID input path is limited to what a boot-protocol keyboard and a
  pointer need and says so instead of guessing: only xHCI controllers are
  probed (a machine with only UHCI/EHCI/OHCI publishes `source=NONE`), only
  devices directly on a root port are enumerated (a keyboard behind a hub is
  not found), low/full/high/SuperSpeed devices are addressed (ITEM-438 gave a
  SuperSpeed device the 512-byte control pipe and its own interval rule; an
  unknown speed field is still published as `UNSUPPORTED_SPEED`), and one
  keyboard and one pointer are held at the same time (ITEM-448). The device LED
  state is not set (Caps Lock is tracked by the host, as the PS/2 path tracks
  it), and no interrupt-report path or hub support exists.
- Since ITEM-452 the walk visits every xHCI controller the machine has and
  every port of each, and holds one live session per bound role (a keyboard and
  a pointer may sit on two different controllers); a visit that binds nothing
  releases its session. That needs at most three 14-page DMA pools
  (`XHCI_SESSION_SLOTS`, about 168 KiB) claimed lazily from the physical page
  allocator - never the heap. A machine whose contiguous physical pages run out
  publishes `DMA_UNAVAILABLE` on that controller and keeps booting, so a
  keyboard elsewhere on the machine still works; whether the acceptance host
  has enough contiguous pages for three pools is answered by its next log, not
  assumed here.
- Every connected device is published, but the QEMU proof cannot reproduce the
  hardware's USB3 port behaviour: QEMU has no SuperSpeed device model (every
  emulated USB device is USB 2), its xHCI model ignores writes to
  `PORTSC.PED` and always reports `PORTSC.PP` set on an empty port. Whether the
  Logitech receiver `046d:c53f` on the owner's USB3 port trains as FULL or
  SUPER, and whether that port needs the USB3 warm reset / PLS wait this driver
  does not implement, stays a hardware question.
- The low-speed USB 1.1 path (ITEM-406) is proven on the host - the port speed
  field, the 8-byte endpoint packet and the interrupt interval a low-speed
  device implies - but not in QEMU: no emulated USB device offers low speed, so
  the acceptance host's Logitech K120 (`046d:c31c`, 1.5 Mbit/s) is the first
  real low-speed device this driver will address. The same applies to the
  PORTSC write semantics of the acceptance host's AMD controllers
  (`1022:43ec`, `1022:1639`): QEMU ignores writes to `PED` and always reports
  `PORTSC.PP` set on an empty port, so neither the defect that disabled a
  freshly reset port nor the port that had to be powered before its connect
  status could appear in the QEMU proof. A hardware run records which
  controller, port and speed the keyboard was found on.
- The controller bounds are the driver's, not the hardware's, and the record
  says which is which (ITEM-407): a controller may advertise up to 255 device
  slots and is accepted with any of them, but this driver enables at most
  `XHCI_DRIVER_SLOTS` (64) at once - one per device it addresses, and it
  addresses one device at a time - and the `[XHCI]` record publishes both
  numbers (`slots=` for the controller, `slot_en=` for what was enabled).
  Scratchpad buffers are not allocated: a controller that advertises them
  (`HCSPARAMS2`) is used with `DCBAA[0]` empty, which is correct only while the
  controller does not consume a scratchpad buffer. That is a stated limit
  rather than a silent fallback, and it is not the reason the acceptance host's
  controllers reported no device.
- A controller that finds no device publishes bounded port evidence in its own
  `[XHCI]` record (ITEM-407): `usbcmd=`/`usbsts=` as the scan left them, the
  first two `portsc=` values, the powered-port mask `pp=` and `ppoff=`, the
  number of root ports that came up unpowered. The list is capped so the whole
  line fits the console mirror's one-painted-line budget - the worst case of
  either record shape is 314 of those 320 bytes, and `make test-xhci-host` pins
  it - which is why the register fields inside the evidence are written
  compactly (eight digits, no `0x`) while the record's leading fields keep the
  established `key=0x…` shape. `ports=` still names the controller's real port count, so
  evidence that stops early is not read as a controller with three ports. A
  READY record publishes the keyboard's own facts instead, because both do not
  fit one line. What those values say on the acceptance host's AMD controllers
  is exactly what the next hardware boot decides.
- ITEM-453 widens that evidence from two ports to all of them: each port
  publishes its own `[XHCI-PORT]` line (raw `PORTSC` before and after the
  bring-up, the decoded power/connect/link state, whether the driver powered or
  kicked it, and how long it waited) and each declared Supported Protocol group
  its own `[XHCI-PROTO]` line, both flush moments of the durable log. The driver
  powers a port whose `PORTSC.PP` is clear, moves a link state that cannot
  sample a connect (`Disabled`, `Inactive`, `Compliance`, `Test Mode`) back to
  RxDetect, and gives a port it wrote to up to 100 ms for its connect to rise.
  What is still a limit is the hardware itself: QEMU's model always powers its
  ports, never leaves RxDetect for an empty one and ignores writes to `PED`, so
  whether the owner's `05:00.3` needs power, a link kick or more time is a
  question the *next* hardware run's `[XHCI-PORT]` lines answer - this driver
  makes that answer readable, it does not presume it.

## Management

- Normal boot has no installed first-boot wizard or management loader.
- The HTTPS management web UI is a deterministic shell, not a production
  browser UI.
- Administrative authentication/authorization and production composition
  boundaries are not activated as a complete management plane.
- Production browser management operation remains NOT TESTED.

## Network And Hardware

- QEMU VirtIO networking and real-TLS fixtures are the deterministic
  software acceptance environments.
- Real physical Ethernet, SATA/NVMe, PCI, DHCP, static IPv4, gateway, DNS,
  power-cycle, and production browser operation remain NOT TESTED.

## Release

HFOC-APPLIANCE-V1 cannot be declared complete without a Hardware Acceptance
Report recording PASS evidence for the required physical checks. QEMU/CI
success alone is not sufficient.
