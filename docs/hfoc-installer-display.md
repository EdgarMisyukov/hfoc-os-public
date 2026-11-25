# HFOC installer display foundation

ITEM-488.  What the installer draws on, what it refuses to depend on, and what
happens on a machine whose firmware gives it nothing to draw on.

## Why the installer carries no GPU driver

The installer has to run on as wide a set of UEFI x86-64 machines as possible,
and it has to do that before it knows anything about the machine it is running
on.  Writing a driver for every GPU would be a project of its own, would need
firmware blobs for some of them, and would fail on exactly the machines an
installer is most needed on - the ones whose vendor driver the owner does not
have at hand.

The firmware has already solved that problem.  UEFI's **Graphics Output
Protocol (GOP)** hands an operating system a framebuffer - a region of memory,
a width, a height, a pitch and a pixel format - that the firmware itself set up
for the device it found.  HFOC's boot chain already consumes it:

```text
UEFI firmware
    │
    ├── GOP framebuffer ──────────────┐
    │                                 │
    └── text/serial fallback          │
                                      ▼
                              HFOC Installer
                                      │
                              Display abstraction
                                      │
                         ┌────────────┴────────────┐
                         │                         │
                    framebuffer              text console
```

Limine takes the GOP framebuffer and passes it to this kernel through the boot
protocol.  From that point on the installer needs no driver, no acceleration and
no knowledge of the vendor: it writes pixels into memory the firmware mapped,
and the firmware's own modesetting put them on the screen.

**This item adds none of:** amdgpu, an Intel GPU driver, an NVIDIA GPU driver, a
DRM/KMS stack, GPU acceleration, Mesa, X11, Wayland or any desktop graphics
stack.  None of those is a prerequisite for booting the installer, installing
HFOC, the first boot, recovery, factory reset, the local console UI or the HTTPS
management plane.

## The layers, and the boundary between them

```text
UEFI GOP backend            kernel/src/display_firmware.c, kernel/include/display_firmware.h
        │                   (the only file that knows the boot protocol's type)
        ▼
HFOC display abstraction    kernel/src/display.c, kernel/include/display.h
        │                   (capabilities, validation, the one record, drawing)
        ▼
Installer UI                kernel/src/installer_ui.c, kernel/include/installer_ui.h
                            (the model and the one line renderer)
```

The installer never reads a GOP structure, never writes a pixel address and
never assumes a resolution.  Adding a second backend later - a UEFI text
protocol, a serial-only terminal, a different firmware interface - means writing
another file behind the same `display.h` contract; the installer UI does not
change, because it only ever sees the abstraction.

`display.c` is deliberately a value machine: it takes a plain
`struct display_framebuffer` (address, dimensions, pitch, depth, masks, memory
model) and never touches the memory it describes.  That is what makes the whole
layer provable on a host, case by case, including every case that must *not*
pass.

## The GOP contract

At boot the kernel asks the abstraction to probe the framebuffer the boot
protocol handed over, and the abstraction checks, in this order:

1. **is there a framebuffer at all** - `present`, and a non-null address;
2. **are the dimensions sane** - non-zero, and inside
   `DISPLAY_MAX_DIMENSION` (16384), the bound the address arithmetic is computed
   in;
3. **is the pixel format one this kernel writes** - the RGB memory model, 32 bits
   per pixel, and masks that name either `BGRX8888` or `RGBX8888`;
4. **can the pitch hold a scanline** - at least `width * bytes_per_pixel`, and a
   whole number of pixels.

Only a framebuffer that passes all four is `AVAILABLE`.  Anything else is
`DEGRADED` or `UNAVAILABLE`, the text path is bound, and `framebuffer_release()`
unbinds the surface so the console cannot paint into a framebuffer the
abstraction refused.

The reading is published once, to the serial stream and to the durable journal:

```text
[DISPLAY] backend=GOP status=AVAILABLE resolution=1920x1080 pitch=7680 bpp=32 format=BGRX8888 fallback=TEXT reason=OK
[DISPLAY] backend=TEXT status=UNAVAILABLE resolution=NONE pitch=0 bpp=0 format=UNKNOWN fallback=SERIAL reason=NO_FRAMEBUFFER
```

Finding a GOP is reported as `backend=GOP`; being able to *use* it is
`status=AVAILABLE`.  The two are not the same claim, and the record never
confuses them.

## The fallback

When the probe refuses a framebuffer the installer does not panic, does not
hang, does not treat the missing driver as an installation failure, and does not
go looking for a GPU to load a driver for.  It binds the text path:

```text
GOP unavailable
      ↓
text console
      ↓
serial console, when one is present
```

The screen says what happened and what it is using instead:

```text
Display
  Backend: TEXT
  Resolution: NONE
  Pixel format: UNKNOWN
  Status: UNAVAILABLE
  Fallback: SERIAL
```

Nothing in that message tells an operator that HFOC does not support their
computer.  A machine with no GOP is a machine this installer installs on with
the console, and the record names the reason (`NO_FRAMEBUFFER`, `BAD_ADDRESS`,
`INVALID_DIMENSIONS`, `INVALID_PITCH`, `UNSUPPORTED_PIXEL_FORMAT`) so the
question "why is this machine in text mode" has an answer on the disk.

## The installer UI foundation

This item does not install anything.  It establishes the shape the installer
grows into, and the rule that shape obeys: **one renderer, two backends**.

`installer_ui.c` builds a model - a title, the rows the installer owns, and for
each row a value and a state - and renders it into bounded text lines.  The
kernel paints those lines on the framebuffer through the display abstraction
when there is one, and prints the same lines to the console and the serial
stream when there is not.  The installer's own code never branches on the
backend, and the screen an operator sees is the same screen either way.

```text
HFOC Installer

> Display
  Backend: GOP
  Resolution: 1920x1080
  Pixel format: BGRX8888
  Status: AVAILABLE
  Fallback: TEXT

  Storage
  Status: NOT_IMPLEMENTED

  Network
  Status: NOT_IMPLEMENTED

  Continue
  Status: AVAILABLE

W/S move   ENTER select   Q quit
```

Every capability that is not built yet is `NOT_IMPLEMENTED`, in the UI and in
the records.  Storage discovery, the partition planner, the installation
executor and the boot configuration are the following items; until they exist
this screen says so, and nothing here reports a success it did not perform.

## Input

The foundation screen reads the same hardware-independent input layer the rest
of the product uses: the PS/2 keyboard and the USB HID keyboard that the xHCI
driver already binds, decoded into characters.  No new USB controller, transport
or HID stack is added by this item, and the screen's keys are named once, in
`installer_ui_key_name()`, so a record can say what a key did.

## What the following items add

```text
Installer UI
    │
    ├── Display            this item
    │
    ├── Input              this item (the existing keyboard paths)
    │
    ├── Storage discovery  next: identity, model, serial, size, transport,
    │                      partition table, existing-OS detection
    │
    ├── Partition planner  next
    │
    ├── Installation executor
    │
    ├── Network
    │
    └── Boot configuration
```

Storage work selects a disk by **identity**, never by a device name: model,
serial, size, transport, partition table and what already lives on it.  A
production installer must not assume the machine's system disk is called
`/dev/nvme0n1` - that name belongs to one test machine - and it must not inherit
the physical acceptance environment's guard as its only selection mechanism.
The destructive-confirmation flow (whole disk, manual partitioning, preserve an
existing system, and a typed `CONFIRM` before anything is written) belongs to
those items, not to this one.

## Boundaries

* The installer draws on the firmware's framebuffer or on text.  It does not
  initialise a GPU, set a mode, or program a display controller.
* The boot chain is unchanged: UEFI → Limine → HFOC.  No new bootloader, no
  replacement for Limine, and the bounded pre-kernel UEFI installer helper keeps
  its existing contract.
* APIC, IOAPIC, SMP and the xHCI architecture are untouched.
* The appliance's production display path is the same abstraction with the same
  reading; no GPU acceleration is added to it by this item.

## QEMU/OVMF proof, and what it is not

The QEMU/OVMF proof exercises the installer through the existing UEFI path and
reads the serial stream: it asserts the `[DISPLAY]` reading (backend, status,
resolution, pitch, format) and the foundation screen's lines, with a GOP
framebuffer present, and it asserts the text fallback when the machine has no
display device at all, where the same flow must appear with
`status=UNAVAILABLE` and no panic.

```text
QEMU/OVMF proof != physical hardware acceptance
```

OVMF's GOP is one firmware implementing one virtual device.  Real machines ship
different firmware, different GOP revisions and different pixel formats, and
some of them offer no GOP at all.  **Physical GPU acceptance remains
`NOT TESTED`** for this item: the QEMU proof shows the installer does not depend
on a GPU-specific driver, and it cannot show how any particular physical machine
behaves.  That is a separate acceptance stage, on real hardware, and this
document says so rather than implying the proof covers it.
