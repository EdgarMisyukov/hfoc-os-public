# HFOC Production Install Flow

**Item:** ITEM-394 of the HFOC Appliance Platform productization wave
**Depends on:** ITEM-351 (production installer controller), ITEM-391 (install
layout contract), ITEM-392 (boot without a PS/2 keyboard)
**Release contract:** HFOC-APPLIANCE-V1
**Status:** implemented; proven by a QEMU proof that boots the installer medium
on UEFI firmware with a real (virtual) disk, and by host proofs for the
declaration and decision logic. Since ITEM-411 a completed run also reboots the
machine (and the QEMU proof observes that reboot, and its absence on a refused
run). The physical run on the acceptance host stays **NOT TESTED** and
human-triggered, and which reset mechanism that host honours is what the next
hardware run states (`docs/hfoc-hardware-acceptance.md`).

## Why this item exists

The installer image existed, the controller existed and the layout contract
existed, but nothing on real media reached them: the only caller of
`production_installer_confirm()` was the QEMU fixture
(`production_installer_qemu_test_init()`), and the acceptance host has no PS/2
keyboard and no USB HID path yet (ITEM-395), so an interactive prompt was not
reachable on hardware. The `INSTALLER` acceptance stage was therefore
unreachable on a real machine.

This item adds the product entry point: booting the installer medium reaches a
real flow that displays the candidate disk, derives the ITEM-391 layout, shows
the preview with the published numbers, requires an explicit confirmation, and
only then writes — through the existing ITEM-351 controller, ITEM-340/341
planner and writer, and ITEM-371 signed-media gate.

## What the installer medium carries

`make` builds the installer media from the same pair of kernels:

| Media | Target | Declaration |
| --- | --- | --- |
| shipped installer ISO | `$(PRODUCTION_INSTALLER_ISO)` | none: an unattended boot refuses |
| authorised media | `make production-installer-authorised-media` | the confirmation token, the one disk serial, the management scope |
| whole-disk media | `make production-installer-whole-disk-media` | the same, with `HFOC-INSTALL-LAYOUT=whole-disk` |
| management media | `make production-installer-management-media` | the same, with `HFOC-INSTALL-MANAGEMENT=management-interface` |
| network media | `make production-installer-network-media` | the same, with a static network declared (ITEM-446) |
| language media | `make production-installer-language-media` | the same, with `HFOC-INSTALL-LANGUAGE=ru` (ITEM-509 509.7c-2) |
| host-seeded media | `make production-installer-host-seeded-media` | console entry, whole-disk/update layout, target/build-bound host seed (ITEM-598) |

All media are staged by `scripts/hfoc-install-media-stage.sh`, and all boot from
`boot/limine-installer.conf`:

```
/HFOC install (unattended, media-authorised)   cmdline: hfoc.install=unattended
/HFOC install (operator: screen + warning)     cmdline: hfoc.install=console
/HFOC appliance (no install)                   cmdline: hfoc.install=none
```

The unattended entry is the default after the boot menu timeout, which is what
makes the flow reachable on a machine with no local input. The entries are
**not** hidden by firmware type, and both firmware modes install (ITEM-517):
the installed boot chain is chosen from the mode the medium was booted with,
and a firmware mode that cannot be determined at all says so itself
(`refused reason=FIRMWARE_UNSUPPORTED`) instead of silently disappearing from
the menu.

### The medium carries two kernels (ITEM-399)

The medium used to carry one kernel and it was the installer: the same file was
the entry the ISO booted and the file the install staged into the ESP, so an
installed appliance ran an installer build - with the install flow, the QEMU
fixture and `-DHFOC_PRODUCTION_INSTALL_MEDIA` in it - and published
`[PRODUCTION-INSTALLER] … proof OK` on every boot. The layout is now explicit:

| File on the medium | What it is | Who reads it |
| --- | --- | --- |
| `boot/hfoc-installer-kernel.elf` | the installer kernel, `$(PRODUCTION_INSTALLER_KERNEL)` | the medium's own Limine entries (`path:`) |
| `boot/hfoc-kernel.elf` | the **installed** kernel, `$(KERNEL)` - the product appliance build | the install run: the boot stage writes it to `\boot\hfoc-kernel.elf`, the ITEM-371 media manifest seals it as its kernel component and the ITEM-372 boot chain seals it |

So the byte the installer stages is the byte the manifest sealed, and the byte
the installed disk runs is the product appliance. `macOS`-style aliasing is not
possible: the staging script hashes both kernels and refuses a medium whose
installer kernel and installed kernel are the same file, and the install flow
refuses a medium that carries no installed-kernel module
(`refused reason=CONTROLLER status=PRODUCTION_INSTALLER_SOURCE_UNAVAILABLE
detail=INSTALLED_KERNEL_MISSING writes=0`) before it plans anything.

`scripts/hfoc-install-media-stage.sh` takes both kernels:

```
hfoc-install-media-stage.sh ISO_ROOT INSTALLER_KERNEL_ELF INSTALLED_KERNEL_ELF \
    CONFIRM TARGET MANAGEMENT [LAYOUT [NETWORK [ADDRESS [GATEWAY [DNS]]]]]
```

The medium's installer payloads are Limine modules:

| File | Purpose |
| --- | --- |
| `boot/hfoc-install.token` | the media declaration (below) |
| `boot/hfoc-kernel.elf` | the installed kernel the run measures and stages (a module, never an entry) |
| `boot/hfoc-installed.conf` | the ITEM-397/ITEM-399 installed boot configuration, staged into the ESP as `\boot\limine\limine.conf` |
| `boot/hfoc-installed.chain` | the ITEM-397 sealed boot chain of the installed set (kernel + that configuration), staged as `\boot\hfoc-boot.man` |
| `EFI/BOOT/BOOTX64.EFI` | the signed Limine UEFI loader: the ESP boot payload and the loader on the installed disk's removable-media path |
| `boot/hfoc-install.payload` | the signed payload blob (loader + installed configuration + kernel) |
| `boot/hfoc-install.man` | the sealed manifest over the payload blob |
| `boot/hfoc-seed.bin` | optional ITEM-598 host CSPRNG record, present only on a private operator medium |
| `boot/hfoc-ssh-key.point` | optional ITEM-606 owner-confirmed SSH client public point, present only when the medium declares its fingerprint |

The ITEM-371 sealed media manifest keeps its declared component order (boot
payload, loader configuration, kernel); its loader configuration is the
*installed* configuration and its kernel is the *installed* kernel, so the bytes
the installer stages are exactly the bytes the medium sealed. The
`hfoc-installed.chain` manifest is sealed over the kernel and the installed
configuration, which is what the installed disk verifies at its own boot
(`[BOOT-CHAIN] verdict=ATTESTED`).

### Host-provisioned entropy on a CPU without hardware RNG (ITEM-598)

A normal medium carries no seed and keeps the fail-closed no-entropy profile.
For a target such as the Toshiba L875 (`rdrand=0 rdseed=0`), generate the
personal record outside the repository and build a private console medium:

```text
python3 scripts/hfoc-host-seed-provision.py \
    --output /secure/operator/hfoc-seed.bin \
    --target TESTNVME000001 --build-id 1

make production-installer-host-seeded-media \
    HFOC_INSTALL_MEDIA_HOST_SEED=/secure/operator/hfoc-seed.bin \
    HFOC_INSTALL_MEDIA_TARGET=TESTNVME000001 \
    HFOC_INSTALL_MEDIA_CONFIRM=ITEM-394-UNATTENDED-INSTALL \
    HFOC_INSTALL_MEDIA_MANAGEMENT=management-interface \
    HFOC_INSTALL_MEDIA_LAYOUT=whole-disk \
    HFOC_INSTALL_MEDIA_NETWORK=dhcp
```

The provisioning record is mode `0600`; the resulting ISO and checksum are also
mode `0600`. Neither may be uploaded to CI or shared as a public release. The
stage step verifies the target serial and release id, requires the console entry
and a `whole-disk` or `update` layout, injects `hfoc-seed.bin` into both install
entries, and adds `HFOC-INSTALL-HOST-SEED=required`.

### Owner SSH public key on the medium (ITEM-606)

A state-preserving update medium can carry one SSH client public key from the
trusted Debian machine, so the appliance never needs the 88-character point
typed at its console. The key and the fingerprint the owner confirmed are
declared together:

```text
make production-installer-update-media \
    HFOC_INSTALL_MEDIA_CONFIRM=ITEM-394-UNATTENDED-INSTALL \
    HFOC_INSTALL_MEDIA_TARGET=TESTNVME000001 \
    HFOC_INSTALL_MEDIA_LAYOUT=update \
    HFOC_INSTALL_MEDIA_MANAGEMENT=management-interface \
    HFOC_INSTALL_MEDIA_NETWORK=dhcp \
    HFOC_INSTALL_MEDIA_SSH_KEY=$HOME/.ssh/hfoc_diag.pub \
    HFOC_INSTALL_MEDIA_SSH_KEY_CONFIRM=$(ssh-keygen -lf $HOME/.ssh/hfoc_diag.pub | awk '{print $2}')
```

The stage requires both values, accepts only an ECDSA P-256 OpenSSH key,
refuses a mismatched fingerprint, writes the 88-character public point as
`boot/hfoc-ssh-key.point`, injects the module into both install entries and
adds `HFOC-INSTALL-SSH-KEY=<SHA256:...>` to the token. The installer refuses a
key module without that declaration or a declaration that does not match the
module, then writes the authorized record into the installed state, flushes it
and reads it back through the installed reader. The private key is never read,
copied or printed.

Before the first disk write the installer checks the record schema, target,
running release id and operator credentials. It then asks for `New login`,
`New password` and `Repeat new password`. The password derives separate
PBKDF2-HMAC-SHA256 encryption and authentication keys; the seed is stored only
as ciphertext in the two-slot persistence record and is read back, authenticated
and compared before installation continues. An update instead asks `Login` and
`Password`, verifies the existing account and preserves the existing seed store.

Installed boot asks the same ordinary `Login` and `Password`. The account
verifier decides whether the pair is valid, then the password unlocks the seed
slot. The owner advances the durable sequence, writes and verifies the companion
slot, derives one domain-separated `HOST_PROVISIONED` pool and only then lets TLS
start:

```text
[RNG] host seed status=OK stage=COMPLETE source=HOST_PROVISIONED state=READY credited=32
[PRODUCTION-SERVING] ... state=LISTENING ... entropy=HOST_PROVISIONED
certificate=persisted_self_signed ... panel=published
```

Changing the account password through the console or management panel re-seals
the current host seed under the replacement password before committing the new
account record. The preceding sealed slot remains available until that commit,
so power loss during the change still leaves a bootable old pair. A forgotten
password uses the documented break-glass account reset and then the explicit
`entropy seed` recovery, or a fresh host-provisioned update.

This is a normal login/password flow, not an automatic panel and not a hex
transfer. `entropy seed` remains available as an explicit recovery command.
One initial seed does not create infinite credited entropy: pool exhaustion
fails closed, and the next boot advances the durable epoch. A clone or complete
old disk snapshot remains undetectable without an external monotonic root of
trust. The installation medium still contains the seed and must be destroyed or
overwritten; secure erase of flash is not claimed.

### The installed configuration states the management scope (ITEM-399)

`scripts/hfoc-installed-config.py` generates the configuration the medium seals
from the template `boot/hfoc-installed.conf`: every template line verbatim - the
`path:` line that must name the staged kernel and the two `module_path:` lines
the installed disk's own chain needs - plus exactly one line

```
    cmdline: hfoc.management=local-only
    cmdline: hfoc.management=management-interface
```

chosen from the medium's `HFOC-INSTALL-MANAGEMENT=` declaration. The scope is
written either way, so an installed appliance states it instead of falling back
to the kernel's deny-by-default value.

### The installed configuration states the network too (ITEM-446)

The same generated line carries the network the operator declared on the
medium:

```
    cmdline: hfoc.management=local-only hfoc.network=dhcp
    cmdline: hfoc.management=management-interface hfoc.network=static hfoc.address=10.0.2.15/24 hfoc.gateway=10.0.2.2 hfoc.dns=10.0.2.3
```

ITEM-449: `HFOC-INSTALL-FORWARD=on` declares one TCP forward and requires
`HFOC-INSTALL-FORWARD-TARGET=` (a literal IPv4 host address),
`HFOC-INSTALL-FORWARD-TARGET-PORT=`, `HFOC-INSTALL-FORWARD-LISTEN-PORT=` (not
443, which the management listener owns) and
`HFOC-INSTALL-FORWARD-CLIENTS=` (one to four exact addresses, comma separated);
`HFOC-INSTALL-FORWARD=off` is the explicit no-forward and carries no values.
The rule travels into the installed boot entry as
`hfoc.forward=on hfoc.forward.target=… hfoc.forward.target-port=…
hfoc.forward.listen-port=… hfoc.forward.clients=…`, where the appliance
publishes `[PRODUCTION-FORWARD] state=DECLARED …` or the refusal, and the
client list is the whole access rule: a source that is not on it is denied
before any outbound connection exists.

`HFOC-INSTALL-NETWORK=dhcp` states the pre-item behaviour explicitly, and
`HFOC-INSTALL-NETWORK=static` also requires `HFOC-INSTALL-ADDRESS=`
(`a.b.c.d/prefix`), `HFOC-INSTALL-GATEWAY=` and `HFOC-INSTALL-DNS=`. The
declaration is validated three times by the same IPv4 owner
(`network_ipv4_config_parse`/`_validate`): at media build time (the staging
script refuses to seal a medium that would be refused), before the first write
(the plan refuses with `NETWORK_INCOMPLETE` or `NETWORK_MALFORMED` and the run
writes nothing), and at boot (the kernel refuses a command line it cannot
honour). The address the record publishes is the active configuration's own, so
a refused or overridden declaration cannot print an address the interface does
not have.
ITEM-654: `HFOC_INSTALL_MEDIA_SNTP=192.0.2.7` (environment variable, not a
positional argument) seals `HFOC-INSTALL-SNTP=192.0.2.7` and the installed
entry carries `hfoc.sntp=192.0.2.7`.  An absent variable keeps the gateway
as the time source.


At boot the declared network is applied and committed into the persistent
network record - the same record the local menu reads and writes - and a record
the operator already committed from the menu wins over the install-time
declaration, so changing the address later is not silently overwritten by the
medium.

### The interface language is a property of the medium (ITEM-509 509.7c-2)

The medium declares the language its installer speaks:

```
HFOC-INSTALL-LANGUAGE=en|ru
```

The value is one of the catalog's two codes, parsed by the same owner the
console command uses (`appliance_language_parse()`); an absent directive is
`en`, and a misspelled one refuses the whole declaration with `BAD_TEXT` before
the first write - `LAYOUT` and `NETWORK` follow the same fail-closed rule.  (A
medium carrying the new directive refuses as `TOKEN_MALFORMED` on a kernel that
does not know it, which is the reason the directive and its parser land in the
same commit and the reason the refusal, not a guess, is the contract.)

The directive is written by `scripts/hfoc-install-media-stage.sh` from the
environment variable `HFOC_INSTALL_MEDIA_LANGUAGE` (like the forward
declaration), so a medium that declares no language keeps its exact token bytes
and every medium built before the directive boots and installs byte for byte as
it did.  The declared language is the language of every installer screen (the
foundation screen, the disk list, the warning, release and refusal pages, the
account screen), of the final banner, and of the console on an install boot;
`language` from that console honestly answers `UNAVAILABLE`, because nothing on
the medium can be changed.  The machine's own record is not consulted on an
install boot: a machine that still carries a previous installation shows the
language of the medium being installed, not the old system's.

Right after `[PRODUCTION-INSTALL] state initialized=VERIFIED` the install
writes the declared language into the installed system's own record and reads it
back through the reader the installed system uses:

```
[PRODUCTION-INSTALL] language=ru persisted=VERIFIED
```

A read-back mismatch fails the install (`persisted=FAILED`, stage `language`)
instead of leaving a machine whose record disagrees with the medium.  The
default language writes no line at all: an English install keeps the
hostname-only record of the previous increments, byte for byte, and the
installed system reads `en` from that absence - the same reader, the same
grammar, no second setting.

#### Changing it later: the machine's own menu (ITEM-447, ITEM-496)

The local menu's `Network` entry is the operator's side of that record.  It
asks four bounded questions (mode `dhcp` or `static`, address
`a.b.c.d/prefix`, gateway, DNS) and stores the answer through the same setter
the install declaration uses; the boot that follows reads the record and
applies it.  ITEM-496 makes the gesture answerable and auditable:

* before the first question the machine publishes
  `[PRODUCTION-NET-SET] state=OFFERED current=…`, the live wire's own reading
  (`DHCP`, `STATIC address=a.b.c.d/p`, `OFFLINE`, `UNAVAILABLE`), so the
  operator is not choosing blind;
* every question and every record is one IRQ-safe write, and every record is
  mirrored into the durable boot log as well as the screen; the 2026-09-29
  reproduction showed `[IOAPIC] irq1` lines splitting `gateway: `, which is
  the class of marker ITEM-440 had already removed from the menu lines;
* the answer record names the mode, the addresses, `persisted=OK|FAILED`,
  `applies=reboot` and the configuration the interface still runs
  (`current=…`): a stored change and a live one are two different facts until
  the reboot, and the record says both.  A change therefore takes effect on
  reboot - never "eventually";
* a refused answer (an unknown mode, a malformed address, a prefix outside
  1..32, a gateway off-subnet, a broken DNS) is published as
  `state=REFUSED reason=…` and stores nothing; the next boot shows the address
  the machine had;
* the entry is offered only when the persistent record exists and is loaded.
  A machine whose state volume is unusable publishes `state=UNAVAILABLE
  reason=NO_RECORD_OWNER` instead of taking four answers it cannot store.

`scripts/test-qemu-production-install-flow.sh` drives all of it on a real
installed disk: the refused answer, the static answer (whose address must
answer the host's own ARP and ICMP on the wire, not merely appear in a record),
and the answer that goes back to `dhcp` (whose boot must publish the lease
again and no static address).

*Why the sealed configuration and not the generated menu*: the generated menu
(ITEM-398) exists only when the machine has a system to chainload - the common
case for an install is a machine with nothing else to offer, where the sealed
configuration is what boots - and generating it in every mode would replace an
attested, sealed configuration with a generated one for no gain. Injecting the
line at media-staging time instead keeps one property the delivery contract
needs: the bytes the medium sealed are the bytes the installer stages are the
bytes the installed disk boots, so the boot stage's "the staged configuration
names the staged kernel" check and the ITEM-371/ITEM-372 seals all still hold on
the same bytes. The generated menu carries the same `cmdline:` line on its
appliance entry (planned from the same declaration), so both paths state the
scope.

An installed appliance therefore publishes, on its own boot:

```
[PRODUCTION-MGMT-BIND] scope=LOCAL_ONLY source=boot-declaration
[PRODUCTION-MGMT-BIND] scope=MANAGEMENT_INTERFACE source=boot-declaration
```

`source=boot-declaration` for both, because both really are declarations: before
this item an explicit `hfoc.management=local-only` was reported as
`source=default`, which was not true.

## The two declarations

An install run needs **both**, and neither one alone can start a write:

1. **The boot entry** selects the mode: `hfoc.install=unattended` (confirm from
   the media) or `hfoc.install=console` (wait for a local confirmation). An
   entry without a recognised directive boots the appliance and never installs;
   an unknown `hfoc.install=` value is refused, not read as "no install".
2. **The media declaration** names the confirmation source and the one disk the
   operator authorised:

```
HFOC-INSTALL-CONFIRM=ITEM-394-UNATTENDED-INSTALL
HFOC-INSTALL-TARGET=DEADBEEF00000001
HFOC-INSTALL-MANAGEMENT=local-only
```

`HFOC-INSTALL-TARGET` is the disk **serial** the SATA identity publishes; the
NVMe path synthesises a stable address-derived identifier until the Identify
Controller serial is parsed. A medium without a declaration, or with a
declaration that does not name exactly one present disk, refuses. The
declaration is a finite text file: the parser accepts at most 1024 bytes, which
covers every standard directive at its maximum (the full owner medium is 543
bytes), and refuses one byte over it by name instead of calling it missing. The
boot command line is a different input and keeps its own 512-byte bound:

| Refusal | Reason |
| --- | --- |
| `TOKEN_MISSING` | no declaration (the shipped medium's default) |
| `TOKEN_MALFORMED` | a directive with an unusable value |
| `TOKEN_TOO_LARGE` | the declaration file is larger than the finite 1024-byte format bound; the boot command line keeps its separate 512-byte bound |
| `NETWORK_INCOMPLETE` | `HFOC-INSTALL-NETWORK=static` without its address, gateway or DNS (ITEM-446) |
| `FORWARD_INCOMPLETE` | `HFOC-INSTALL-FORWARD=on` without its target, ports or clients, or an `off` carrying values (ITEM-449) |
| `FORWARD_MALFORMED` | a forward target that is not a literal host address, an out-of-range port, the serving port 443 as the listen port, a repeated client or more than four of them (ITEM-449) |
| `NETWORK_MALFORMED` | an address without a prefix, a prefix outside 1..32, a gateway outside the declared subnet, a malformed DNS, or a static value declared with `dhcp` (ITEM-446) |
| `CONFIRM_MISSING` / `TARGET_MISSING` | an incomplete declaration |
| `FIRMWARE_UNSUPPORTED` | the firmware mode could not be determined at all; a detected legacy-BIOS boot installs the BIOS chain (ITEM-517) |
| `NO_DISK` / `TARGET_NOT_FOUND` / `TARGET_AMBIGUOUS` | the declared disk is not present, or more than one disk answers to it |
| `CONTROLLER status=PRODUCTION_INSTALLER_MEDIA_TOO_SMALL` | the declared layout does not fit (below `INSTALLER_LAYOUT_CONTRACT_MINIMUM_MEDIA_LBA`) |
| `CONTROLLER status=PRODUCTION_INSTALLER_MEDIA_UNVERIFIED` | the sealed media manifest does not verify |
| `CONTROLLER status=PRODUCTION_INSTALLER_SOURCE_UNAVAILABLE detail=INSTALLED_KERNEL_MISSING` | the medium carries no installed-kernel module: there is nothing to stage into the ESP (ITEM-399) |

Every refusal publishes `writes=0`; the proof checks the disk image is
byte-identical after each one.

### The operator entry asks instead of refusing (ITEM-500)

The table above is the whole truth for the **unattended** entry. The
**operator** entry (`hfoc.install=console`) has a person at the machine, so the
declared disk being absent is the reason to ask, not a reason to refuse: the
plan keeps `ACTION_CONSOLE` with nothing selected and the storage screen opens
on the disks this machine actually has. The record published before the screen
names the declaration it could not honour:

```text
[PRODUCTION-INSTALL] pending source=console entry=console token=<token>
    target=<declared> declared=NOT_ON_THIS_MACHINE disk=NONE identity=NONE writes=0
```

The operator's choice then becomes the declared target of the prepared plan -
the same path ITEM-490 proved - and the run installs the disk the person read on
the screen. `TARGET_NOT_FOUND` therefore stays a refusal only where nobody can
answer it, and the operator entry can never widen an unattended run: the
unattended entry never reaches the screen.

An operator entry that refuses *before* the screen can open - `NO_DISK`,
`FIRMWARE_UNSUPPORTED`, `REPORT_UNAVAILABLE`, an unhonourable network or forward
declaration - paints the reason on the machine's own screen and waits for a key
(five minutes, then the ordinary boot continues) instead of leaving the machine
to boot from the medium in silence. The operator leaving the storage screen on
purpose (`CANCELLED_BY_OPERATOR`) is a decision made on a page the person was
reading, so it keeps its record and shows no second page.

## The run, in order

1. the boot entry's mode and the media declaration are parsed;
2. every probed disk is published as a `candidate` record with its controller,
   serial (the guard identity), model, block count and size, and exactly one is
   marked `selected=1`;
3. the disk the declaration names becomes the controller's target, with the
   serial as the identity string;
4. the ITEM-371 media gate verifies the sealed manifest against the declared
   trust anchors, the signed payload digest and the source payloads;
5. the layout mode is selected and published (see "Which layout the run takes"
   below) before anything is written;
6. the ITEM-391 contract derives the role sizes for the target disk and refuses
   a disk below the minimum media instead of truncating - in preserve mode it is
   the dual-boot contract, which sizes the three roles the appliance adds and
   leaves the existing system's space alone;
7. the boot menu and its inventory are planned in both modes - a menu plan is
   accepted only while the layout plan the contract just built is the
   controller's current state - and in preserve mode the
   `[INSTALLER-PRESERVE]` plan is published with them. The preview is published
   after that planning, so what the operator confirms is the contracted numbers
   together with what is preserved, what is added and what the menu offers. All
   of it precedes the confirmation and the first write;
8. the confirmation source publishes who or what confirmed, the token, the
   entry, the disk and its identity, and the uptime of the confirmation;
9. the ITEM-397 pre-write gate refuses an ESP that cannot hold the installed
   boot set *before* the writer runs, and the ITEM-340 writer then writes the
   GPT and the roles (whole-disk), or the ITEM-396 preservation writer rewrites
   the table with every foreign entry preserved and the appliance's roles added
   in the free region (preserve);
10. the ITEM-397 boot stage stages the installed boot set into the ESP (the
   Limine loader on the removable-media path, the installed boot
   configuration, the sealed boot chain and the kernel), verifies it against
   the sealed media source digest and against the configuration's own kernel
   reference, preserves the two paths the set takes over, and publishes exactly
   one boot-entry record naming the mechanism the firmware uses;
11. ITEM-502: the appliance's persistent state is **erased** as an explicit
   named step - every store rebuilt as an empty data region, the start and tail
   of the appliance's own roles zeroed (never the shared ESP, never a foreign
   partition), read back raw and through the loader, and published as
   `[PRODUCTION-INSTALL] state erase stores=<n> blocks=<n> verify=CLEAN`.  A
   run whose erase does not verify refuses here, before `applied`; the step is
   before the boot-log flush so its record is durable on the installed ESP;
12. the persistent state volume is initialised (only a run whose erase verified
   may do this), the layout is verified, and the `[INSTALLER-LAYOUT]` record the
   acceptance `INSTALLER` stage quotes is published;
13. the `applied` record closes the write path;
14. ITEM-411: the `complete` record states that the installation is finished,
   that the **installation medium must be removed**, which mechanism reboots the
   machine and how many seconds that takes, and the machine then resets itself.
   Nothing before step 14 reboots: a run that refused, failed or is still
   waiting at the operator's storage screen keeps its behaviour and leaves the machine
   running.

## The reboot at the end of a successful install (ITEM-411)

The flow used to end at step 12, and on the acceptance host that was the whole
problem: the install completed, the machine carried on with the **ordinary boot
of the installation medium** (the firmware prefers the removable medium), the
operator saw the appliance from the stick instead of the system that had just
been installed, and the only way on was to power-cycle by hand.

A completed run now reboots the machine. The last record of the run is:

```
[PRODUCTION-INSTALL] complete disk=1 identity=DEADBEEF00000001 medium=REMOVE reset=PCI in=5s
```

| field | meaning |
| --- | --- |
| `disk`, `identity` | the disk that was installed, i.e. the target the media declaration named |
| `medium=REMOVE` | the installation medium has to come out of the machine: the firmware prefers the removable medium, so a machine left with the stick in boots the installer again |
| `reset=` | the mechanism the reset plan tries first (`ACPI`, `PCI`, `I8042` or `FAULT`) |
| `in=` | the bounded delay before the reset, so the operator can read the line and pull the stick out |

The record is mirrored on the local console (ITEM-405), so a machine with no
serial capture shows it too, and no keypress is asked for: the local keyboard
may be unavailable (ITEM-392/395), so the delay is a delay, never a prompt.

### The screen the operator reads (ITEM-417)

Mirroring the record is not enough on the machine that mattered. The flow runs
**before the kernel's console exists**, so its records are held back from the
display: the 2026-09-15 acceptance run rebooted with nothing on the screen
saying the install had finished. A completed install now paints the machine's
own framebuffer with a banner (`HFOC OS INSTALLED`), a header band, a countdown
in the middle of the screen and the line an operator needs at the bottom
(`REMOVE THE INSTALLATION MEDIUM WHILE THE MACHINE RESTARTS`), in the colours
the appliance console already uses.

The completion record says which of the two outcomes the operator got:

```
[PRODUCTION-INSTALL] complete disk=1 identity=DEADBEEF00000001 medium=REMOVE reset=ACPI in=5s screen=BANNER
```

| value | meaning |
| --- | --- |
| `screen=BANNER` | the banner and its countdown are on the machine's screen |
| `notice screen=NONE reason=NO_FRAMEBUFFER` | there is no framebuffer to paint, so the wait happens with the display untouched |
| `notice screen=NONE reason=BANNER_FAILED` | the framebuffer is there and the paint failed; published as its own finding |

`in=` is a promise about time, and it is measured rather than assumed. The
delay used to be measured on "the PIT channel the timer programmed", which the
installer path never programmed: a stopped counter reads the same value twice,
every delta was zero, the sampling bound ended the wait after 200 000 useless
port reads, and a countdown promised as five seconds lasted **0.318 s**
(measured in QEMU, 2026-09-15: record published, banner painted at +0.168 s,
machine gone at +0.318 s). `platform_reset_delay_ms()` now puts channel 0 into
a counting state itself - rate generator, mode 2, full 16-bit reload at the
counter's base frequency - accumulates the ticks a delay of that length lasts,
and ends the wait on a counter that stops moving instead of parking a caller on
it. The same measurement after the change: banner at +0.194 s, countdown moving,
machine gone at **+5.065 s**.

`scripts/test-qemu-install-notice.sh` is what keeps both halves honest: it runs
the installer with a monitor attached, waits for the completion record, reads
the guest's own frame while the machine is up - the banner has to cover at least
half of it, and the number in the middle has to change, because a fixed picture
is not a countdown - and then requires the machine to leave no earlier than the
seconds its record promised.

### Which mechanism reboots the machine (ITEM-411)

`kernel/src/platform_reset.c` tries the mechanisms in a documented order and
falls through to the next one when the platform ignores a step. Every step is a
bounded write followed by a bounded settle delay, so no step can hang the
machine:

| # | mechanism | when it is tried | what it does |
| --- | --- | --- | --- |
| 1 | `ACPI` | the FADT declares a reset register (`RESET_REG_SUP`, `RESET_REG`, `RESET_VALUE`) in SystemIO or SystemMemory | writes `RESET_VALUE` to `RESET_REG`: byte-wise for a port, at the declared width through the direct map for memory |
| 2 | `PCI` | a PCI host bridge answered the configuration mechanism | the documented write sequence to the reset control register at port `0xCF9`: the current value with the reset bits cleared, then `SYS_RST`, then `SYS_RST\|RST_CPU`, preserving the bits the kernel does not own |
| 3 | `I8042` | the i8042 controller answered at ports `0x60`/`0x64` during boot | command `0xFE` on port `0x64`. It is **not** tried on a machine with no controller - the acceptance host publishes `PS2: UNAVAILABLE` (`KEYBOARD_INIT_CONTROLLER_ABSENT`), where that write goes nowhere |
| 4 | `FAULT` | always, as the last step | an IDT with no gates and a breakpoint: the processor double-faults, finds no gate again and resets itself |

The plan and every attempt are published, so a hardware run states which
mechanism the platform actually honoured - a mechanism that resets the machine
never returns to publish the next one, so the **last** `[PLATFORM-RESET]` record
before the machine goes down is the mechanism that worked:

```
[PLATFORM-RESET] plan reset=PCI steps=PCI>I8042>FAULT acpi_declared=0 pci=1 i8042=1
[PLATFORM-RESET] attempt=1/3 reset=PCI port=0xCF9 value=0x06
[PLATFORM-RESET] attempt=2/3 reset=I8042 port=0x64 command=0xFE
[PLATFORM-RESET] attempt=3/3 reset=FAULT detail=triple-fault
```

Which of the four the **acceptance host** honours is exactly what only a
hardware run can confirm: the host's shape - no i8042, and a FADT that declares
no reset register - selects `PCI` (`make test-platform-reset-host` pins that
decision), and the boot record of the next hardware run states whether the
chipset obeyed it or whether the machine fell through to the fault.

## Which layout the run takes

The flow used to hardcode the destructive clean whole-disk mode. The layout is
now a declaration, and it is decided and published **before the first write**:

```
[PRODUCTION-INSTALL] layout mode=preserve|whole-disk|release reason=… declared=0|1 [released_index=N] chosen=none|alongside|free-space|erase|manual
```

| `reason` | meaning |
| --- | --- |
| `declared-whole-disk` | the operator declared `HFOC-INSTALL-LAYOUT=whole-disk`; the target disk's content is expendable |
| `declared-preserve` | the operator declared `HFOC-INSTALL-LAYOUT=preserve` |
| `existing-system` | nothing was declared, no operator chose, and the target disk carries a partition that is not an ESP, or a system the discovery named |
| `foreign-loader` | nothing was declared, no operator chose, and the target disk's ESP carries a loader that is not ours, even though the discovery could not name a system |
| `no-existing-system` | nothing was declared, no operator chose, and the target disk carries nothing to keep - the whole-disk path |
| `operator-alongside` | the operator chose the alongside scheme on the scheme screen |
| `operator-free-space` | the operator chose the free-space scheme |
| `operator-erase` | the operator chose the destructive erase scheme (allowed even when the medium declared preserve - the declaration is a default, not a lock) |
| `operator-release` | the operator chose the manual scheme and gave up a partition (ITEM-501) |

ITEM-514 puts the mode on a screen: `declared=` and `chosen=` name both sources,
so the acceptance report reads what decided the mode.  The scheme screen itself
publishes `[PRODUCTION-INSTALL] scheme declared=… chosen=… source=operator
writes=0` before the confirmation, and the summary that follows renders the plan
the installer's own `installer_layout_contract_*` planner builds - the same
calls the writer executes - with `[INSTALLER-STORAGE] summary … writes=0`.
`RELEASE_CONFLICT` is judged against the mode the run will use, not against the
medium's declaration: giving up a partition and erasing the whole disk still
contradict each other, while giving one up on a medium that declared whole-disk
is exactly the operator's decision to make.

`HFOC-INSTALL-LAYOUT` is one more directive of the install token, so it is one
more argument of the medium staging script (`scripts/hfoc-install-media-stage.sh
ISO_ROOT INSTALLER_KERNEL_ELF INSTALLED_KERNEL_ELF CONFIRM TARGET MANAGEMENT
[LAYOUT]`) and one more `Makefile`
variable (`HFOC_INSTALL_MEDIA_LAYOUT`). A misspelled value is refused (`BAD_TEXT`)
rather than guessed, and the `media token=…` record publishes
`mode=whole-disk|preserve|update|default`.  ITEM-586 adds the third value:
`update` is an explicit state-preserving reinstall of an appliance that is
already installed, while `whole-disk` still erases the previous run's state.

## The legacy-BIOS layout and boot chain (ITEM-517)

The firmware mode the medium was booted with selects the installed boot chain
(`media->firmware_mode`, Limine's firmware type). A `BIOS_LEGACY` run installs
the BIOS contract layout and never constructs an EFI environment:

```
[INSTALLER-LAYOUT] plan mode=clean-whole-disk-bios disk=1 … first_lba=2048
                   footprint_lba=331776 footprint_mib=162
[INSTALLER-LAYOUT] role=bios-boot        first_lba=2048   length_lba=2048
[INSTALLER-LAYOUT] role=boot-volume      first_lba=4096   length_lba=67584
[INSTALLER-LAYOUT] role=hfoc-system      first_lba=71680  length_lba=131072
[INSTALLER-LAYOUT] role=hfoc-persistence first_lba=202752 length_lba=65536
[INSTALLER-LAYOUT] role=hfoc-recovery    first_lba=268288 length_lba=65536
```

There is no EFI System Partition in this plan. The BIOS boot partition is typed
`Hah!IdontNeedEFI` (the measured Limine minimum is 64 sectors; the declared
role is one 1 MiB alignment unit), and the FAT boot volume plays the ESP's
part for the payload set. A BIOS preserving install is refused by name
(`PRODUCTION_INSTALLER_BIOS_PRESERVE_UNSUPPORTED`) before any plan exists,
because `installer_dual_boot` reuses the previous system's ESP.

The stage (`production_installer_bios_boot.c`) writes the measured Limine
12.3.2 recipe and verifies it by read-back:

- stage1 - the first 512 bytes of the pinned 22,516-byte stage image - at LBA0,
  with the LE64 at byte offset `0x1a4` patched to `bios_boot_first_lba * 512`
  and bytes `[218:224)` and `[440:510)` restored from the original MBR, so the
  protective MBR entry and therefore the GPT survive;
- the restored table is then marked active exactly as `limine bios-install`
  marks an MBR disk whose table has no active entry: partition 1's flag byte
  (`446`) becomes `0x80`, and every other byte of the layout's table stays as
  it was. Firmware that consults the flag before it runs the boot sector - the
  Toshiba L875's InsydeH2O is one - skips a disk without it and never reaches
  stage1; the boot media, which `limine bios-install` wrote, carry the flag,
  and the installed disk now does too;
- the stage's two BIOS calls are made with `ECX` preserved (ITEM-549). The L875's
  legacy AHCI `INT 13h` handler does not keep `ECX`, and Limine's Stage1 counts
  its stage2 sectors in that register (`loop .loop` in `stage1/hdd/disk.asm`), so
  the loop ran about 65,536 times, the disk address packet's buffer offset
  wrapped inside the `0x7000` segment and the runaway reads overwrote low memory:
  the machine rebooted before the protected-mode handoff, with no stage1 output
  at all. The same image boots from a stick because that firmware's USB path does
  preserve `ECX`. The writer redirects the `AH=48h` parameters call (`0xfc`) and
  the `AH=42h` read call (`0x13a`) into two trampolines placed in the stage's own
  zero padding (`0xad..0xda`), each of which does `push ecx` / `int 13h` /
  `pop ecx`, and turns the carry test at `0x8b` into a jump to the tail of the
  second trampoline, which performs the same test and returns. Every other byte
  of the pinned image stays as pinned, and a stage image whose sites or padding
  do not match the measured recipe is refused before LBA0 is written;
- stage2 - the remaining 22,004 bytes - at the first sector of the BIOS boot
  partition, with the trailing 12 bytes of the partial last sector left as they
  were; exactly 44 sectors change;
- the payload set on the FAT boot volume: the generated menu
  `\boot\limine\limine.conf`, the sealed chain configuration
  `\boot\hfoc\limine.conf`, `\boot\hfoc-kernel.elf`, `\boot\hfoc-boot.man` and
  `\boot\limine\limine-bios.sys` (290,964 bytes).

The two configuration files are deliberate. Limine BIOS reads
`\boot\limine\limine.conf` itself, so that file is the generated menu; the
menu's appliance entry names the *sealed* configuration as the module
`boot():/boot/hfoc/limine.conf`. The kernel's boot chain collects the module
whose path ends in `limine.conf` and verifies its digest against the signed
`hfoc-boot.man`, so a single file for both roles would hand it the generated
menu, which the manifest does not seal. On UEFI the same split is
`\EFI\BOOT\limine.conf` (read by the firmware) against
`\boot\limine\limine.conf` (the module).

The stage publishes one boot-entry record in the same vocabulary as the UEFI
path:

```
[PRODUCTION-INSTALL] boot-entry state=AVAILABLE mechanism=BIOS_MBR
    path=\boot\limine\limine.conf kernel=\boot\hfoc-kernel.elf
    config=\boot\hfoc\limine.conf manifest=\boot\hfoc-boot.man
    loader=\boot\limine\limine-bios.sys entries=1 timeout=10s source=GENERATED
    payload_set=6 … stage1_lba=0 stage2_first_lba=2048 stage2_bytes=22004
    stage2_offset=1048576 stage2_field=420 stage_writes=44
    limine_bios_sys_bytes=290964 blob_digest=<sha256> verify=READBACK gpt=VALID
```

`path=` is the file Limine reads (the menu), `config=` the file the kernel
receives as the chain component. The flow also publishes
`[PRODUCTION-INSTALL] bios payload=VERIFIED … mechanism=BIOS_MBR …` and, in
BIOS mode, the same `[INSTALLER-BOOT-MENU] menu firmware=bios-legacy …` record
the preserving UEFI install publishes.

The BIOS boot chain's two blobs live only in the installer kernel
(`HFOC_PRODUCTION_INSTALL_MEDIA`): the stage image is the
`binary_limine_hdd_bin_data` array of `deps/limine/limine-bios-hdd.h`, and
`deps/limine/limine-bios.sys` is linked by a `$(LD) -r -b binary` rule whose
object is a member of `PRODUCTION_INSTALLER_OBJS` only. The product kernel
links neither, and `production_installer_bios_blob()` returns an empty blob
there, which the pre-write gate refuses with `BIOS_STAGE_UNAVAILABLE`.

**Proven where.** `scripts/test-qemu-production-install-flow.sh` installs in
BIOS mode with SeaBIOS (QEMU's default, no OVMF), reads the raw image
independently (LBA0 recipe, protective MBR, GPT CRCs, stage2 region, five role
extents, and the FAT boot volume through `hfoc-esp-read.py
--partition-index 1`), and then boots the installed disk alone with
`boot_installed_bios_disk()`: it must publish `[BOOT-CHAIN] verdict=ATTESTED`,
the production boot verdict, exactly one readiness record and no installer
record. `scripts/test-qemu-installer-disk-screen.sh` walks the BIOS operator
screens to the summary (which names the BIOS roles and promises no ESP) and
proves the honest `NO_DISK` refusal on a machine with no disk. Physical
acceptance on the owner's Toshiba L875 is the owner's step and is not part of
CI.

## The boot menu, in both modes

The generated menu is planned by `production_installer_plan_preserve()` in both
layout modes, because the system that has to stay selectable may not be on the
target disk at all (owner Case B: Debian on the machine's other disk, HFOC on the
dedicated one):

* a **preserving** install keeps what is on the target disk, so the target's own
  systems are offered through `boot()` and the other disks' systems through
  `guid(<GPT partition GUID>)`;
* a **whole-disk** install erases the target disk, so offering a system that
  lives there would offer exactly what the run destroys.  The target contributes
  no loader and only the *other* disks' systems are offered - and a machine with
  nothing else to offer produces a menu with just the appliance entry, which is
  installed all the same (see below).

Other disks are only ever read: each has its own ESP, the scan binds it, lists
its loaders and writes nothing.

A loader is chosen by the vendor directory it lives in and by name, and the name
order is the order a firmware boots: the **shim** first (`SHIMX64.EFI`), then
`GRUBX64.EFI`, then the removable-media fallback `BOOTX64.EFI`, then the Windows
boot manager.  A real Debian `\EFI\debian` carries both the shim and the GRUB
behind it, and the shim is the signed entry point: an entry that named GRUB would
be an entry for a system a Secure Boot machine refuses.

### When the menu is the installed configuration

Always (ITEM-409).  The owner's rule is that the installed appliance boots
through a menu: the appliance entry - first, so it is Limine's default - with its
`module_path:` lines and its declared `cmdline:` scope, plus one `protocol: efi`
chainload entry per system the discovery found.  A machine that carries nothing
but this appliance still gets a menu that shows this appliance; a machine that
carries another system gets a menu that offers both.

The sealed configuration is staged either way - the appliance entry references it
as a module and the signed chain attests it - and it is what the loader reads
only when no menu could be built at all.  That case is a refusal in preserve mode
(a preserving install that cannot keep the existing system selectable must not
claim it does); in whole-disk mode the run installs the sealed configuration and
says so: `source=SEALED menu_reason=MENU_UNAVAILABLE` in the boot-entry record,
and `state=SEALED` in the `boot-menu` record below.

The menu declares `timeout: 10` - ten seconds, so an operator can read it and
choose - and the appliance entry is the first entry, so a machine nobody touches
still boots this appliance.  The timeout is rendered from
`INSTALLER_BOOT_MENU_TIMEOUT_SECONDS`, published in the boot-entry record and
pinned by the product-flow proofs, so the file, the record and the proof cannot
disagree.

Limine reads `\EFI\BOOT\limine.conf` - the configuration next to the EFI
application it was booted from - before the sealed `\boot\limine\limine.conf`,
which is why the menu is the configuration an operator sees.  That is a measured
claim, not a reading of the manual: patching only `\EFI\BOOT\limine.conf` on an
installed disk with a different management scope makes the installed system
publish that scope as `source=boot-declaration` on its next boot (the probe and
its output are recorded in `docs/hfoc-installed-boot.md`).

### The menu record, and why nothing is silent

A menu that ends up offering nothing but this appliance is exactly what cost the
2026-09-14 hardware iteration: the installed configuration carried one entry, and
nothing in the run said which disk or which stage had decided that.  One bounded
record, published before the first write in **both** layout modes, states it:

```
[PRODUCTION-INSTALL] boot-menu state=GENERATED reason=chainload-entry
  stage=chainload-entry entries=2 loaders=1 missing=0 unreadable=0 disks=2
  target=1 d1=NONE/esp0/NO_ESP/0 d2=GPT/esp1/OK/1
```

* `state=GENERATED|SEALED` - which configuration is installed;
* `reason=chainload-entry|appliance-only|menu-unavailable` - whether the menu
  offers a system besides this appliance, and if not, that the menu could not be
  built at all;
* `stage=` - the earliest stage of the chain that decided, when the menu offers
  nothing else: `discovery-read` (the disk's partition table could not be read),
  `partition-table` (read, no EFI System Partition), `esp-read` (the ESP is there
  and could not be read), `loader-inventory` (read, no loader we recognise),
  `device-lookup`, `entry-capacity`, `target-erased` (the only system on the
  machine is on the disk a whole-disk install erases) and `no-other-disk`;
* `entries=`, `loaders=`, `missing=`, `unreadable=`, `disks=`, `target=` - what
  the menu holds.  `missing=` counts the systems that *were* found and could not
  be offered (no recognisable loader, or a menu that ran out of room);
  `unreadable=` counts the disks **besides the target** whose ESP the read-only
  scan could not read - the target's own scan state is in its `d<id>=` token, and
  a whole-disk install drops that disk's device on purpose (`NO_DEVICE`), which
  is not an unreadable disk;
* `d<id>=<table>/esp<0|1>/<scan>/<loaders>` per disk - what the discovery found
  on it and what the read-only scan then made of it, with `,discovery=<status>`
  appended when the discovery's own scan of that disk did not return OK.

The per-disk scan tokens are the ITEM-396 vocabulary: `OK`, `NO_DEVICE`,
`NO_ESP`, `UNREADABLE`, `CORRUPT`, `UNSUPPORTED`, `NO_LOADER`.  The record is one
line, it is mirrored to the framebuffer by ITEM-405, and the acceptance run
quotes it - so the next run that cannot offer a system on the machine names the
disk and the stage instead of publishing a single-entry menu.

## Which volume the ESP writer is allowed to touch (ITEM-405)

A partition is a role; a volume is what its boot sector says it is. The two are
the same number only when something checks, and until ITEM-405 nothing did: the
bind compared the boot sector with the partition length it was handed and never
read the volume's own total-sector count (FAT32 offset 32, FAT16 offset 19).

The bind now refuses a volume with `EFI_FAT32_ESP_UNSUPPORTED_GEOMETRY` when the
declared total is zero, when it exceeds the partition, or when the data region
the FAT describes does not fit inside it, and it derives the cluster count from
the **smaller** of the partition and the declared volume, so every cluster LBA
stays inside the volume the writer is actually addressing. This is the shape the
hardware acceptance run hit: an install onto a disk that had carried a Windows
layout left the 33 MiB ESP role holding the old system's 100 MiB FAT32
(`total_sectors=204800`, `reserved_sectors=6654`, `sectors_per_fat=769`,
`sectors_per_cluster=2`), which the old bind accepted, so the role was never
formatted and the staged set went to a volume with a different geometry.

Who the role belongs to decides what happens next, and the flow tells the writer
with `format_owned = !esp_target->reused`:

* a **whole-disk** install created the role from its own layout, so the role is
  formatted and then bound - whatever the range held before (a zeroed area, a
  formatted volume, or the stale foreign filesystem the replaced table left
  behind) is not a volume anybody owns;
* a **shared** ESP in preserve mode belongs to the system already on the
  machine, so it is bound as it is and is *never* formatted; a volume that does
  not fit its partition, or that this module cannot use for any other reason, is
  refused - `PRODUCTION_INSTALLER_ESP_UNSUITABLE` - with zero writes.

## The shared ESP, and the paths the install takes over
In preserve mode the ESP belongs to the system that is already there. It is
bound as it is and **never formatted**, and the installed boot set is staged into
it by the one ESP writer. A shared volume whose boot sector describes an extent
its partition does not have is refused before the first byte (ITEM-405). Before
that writer's first byte:

* the free clusters are counted (read-only) and compared with the set's
  requirement, so a volume without room for the set is refused with
  `PRODUCTION_INSTALLER_ESP_TOO_SMALL` and zero writes instead of receiving a
  partial set that cannot boot;
* `\EFI\BOOT\BOOTX64.EFI` - the removable-media fallback path a firmware tries
  when no boot entry matches, and therefore frequently the other system's own
  fallback loader - is probed. When it is occupied it is renamed to
  `\EFI\HFOC\BOOTX64.PRE`: the directory entry moves, the clusters, the FAT chain
  and every byte of the file stay where they were, so the original is
  recoverable;
* `\EFI\BOOT\limine.conf` - the first configuration a Limine reads, which would
  otherwise shadow the generated menu - is probed the same way and renamed to
  `\EFI\HFOC\LIMINE.PRE`.

The boot-entry record states both, so an operator can put somebody else's loader
back exactly as it was:

```
[PRODUCTION-INSTALL] boot-entry state=AVAILABLE mechanism=REMOVABLE_MEDIA_PATH
  path=\EFI\BOOT\BOOTX64.EFI kernel=\boot\hfoc-kernel.elf
  config=\boot\limine\limine.conf
  menu=\EFI\BOOT\limine.conf entries=… timeout=10s source=GENERATED
  payload_set=7 required_sectors=… esp_first_lba=… esp_sectors=…
  sealed_bytes=… sealed_digest=…
  existing_loader=FREE|PRESERVED [existing_loader_bytes=… existing_loader_kept_at=\EFI\HFOC\BOOTX64.PRE]
  existing_config=FREE|PRESERVED [existing_config_bytes=… existing_config_kept_at=\EFI\HFOC\LIMINE.PRE]
  verify=SEALED_MEDIA variable=UNAVAILABLE|CREATED|FAILED boot_entry=NONE|CREATED
```

`menu=` names the configuration the loader reads, `entries=` how many entries it
offers and `timeout=` the timeout it declares; `source=GENERATED` is the menu this
run built.  The sealed fallback is published instead of those fields when no menu
could be built at all:

```
  config=\boot\limine\limine.conf menu=\boot\limine\limine.conf entries=1
  source=SEALED menu_reason=MENU_UNAVAILABLE payload_set=6 …
```

`payload_set` counts the files this run staged: the sealed five, plus the
generated menu - which is one more file for every install that has a menu, i.e.
for every install but the fallback above.

Example run (`docs/hfoc-hardware-acceptance.md` records the same records as the
stage evidence):

```
[PRODUCTION-INSTALL] media token=PRESENT confirm=ITEM-394-UNATTENDED-INSTALL target=DEADBEEF00000001 management=local-only mode=default entry=unattended
[PRODUCTION-INSTALL] candidate disk=1 controller=SATA identity=DEADBEEF00000001 model=KINGSTON SA400S37480G blocks=234375000 size_mib=114440 selected=1
[PRODUCTION-INSTALL] confirm source=media-token entry=unattended token=ITEM-394-UNATTENDED-INSTALL target=DEADBEEF00000001 disk=1 identity=DEADBEEF00000001 management=local-only when_uptime_s=0
[PRODUCTION-INSTALL] layout mode=whole-disk reason=no-existing-system declared=0 chosen=none
[PRODUCTION-INSTALL] boot-menu state=GENERATED reason=appliance-only stage=no-other-disk entries=1 loaders=0 missing=0 unreadable=0 disks=1 target=1 d1=NONE/esp0/NO_ESP/0
[PRODUCTION-INSTALL] preview HFOC INSTALL preview mode=clean-whole-disk disk=1 ...
[INSTALLER-LAYOUT] plan mode=clean-whole-disk disk=1 media_lba=... footprint_lba=329728 footprint_mib=161 ...
[INSTALLER-LAYOUT] role=efi-system first_lba=2048 length_lba=67584 length_mib=33 reused=0
...
[PRODUCTION-INSTALL] esp payload=VERIFIED first_lba=2048 length_lba=67584 payload_bytes=360448 mode=whole-disk staged_bytes=7206963 mechanism=REMOVABLE_MEDIA_PATH
[PRODUCTION-INSTALL] boot-entry state=AVAILABLE mechanism=REMOVABLE_MEDIA_PATH path=\EFI\BOOT\BOOTX64.EFI kernel=\boot\hfoc-kernel.elf config=\boot\limine\limine.conf menu=\EFI\BOOT\limine.conf entries=1 timeout=10s source=GENERATED payload_set=7 required_sectors=... esp_first_lba=2048 esp_sectors=67584 sealed_bytes=... sealed_digest=... existing_loader=FREE existing_config=FREE verify=SEALED_MEDIA variable=UNAVAILABLE boot_entry=NONE
[PRODUCTION-INSTALL] state erase stores=7 blocks=832 verify=CLEAN
[PRODUCTION-INSTALL] state initialized=VERIFIED ready=1
[PRODUCTION-INSTALL] admin=SET login=installer1 persisted=VERIFIED
[PRODUCTION-INSTALL] applied disk=1 identity=DEADBEEF00000001 controller=SATA management=local-only when_uptime_s=0 layout=VERIFIED
[PRODUCTION-INSTALL] complete disk=1 identity=DEADBEEF00000001 medium=REMOVE reset=PCI in=5s
[PLATFORM-RESET] plan reset=PCI steps=PCI>I8042>FAULT acpi_declared=0 pci=1 i8042=1
[PLATFORM-RESET] attempt=1/3 reset=PCI port=0xCF9 value=0x06
```

The last two lines are ITEM-411: the medium has to come out, and the machine
reboots through the mechanism the platform states it honoured.

`admin=SET` is ITEM-506 increment 4.  The operator entry opens the account
screen after the accepted disk warning: it asks for the login and the password
twice, hides every typed character, refuses a line past the policy maximum
instead of cutting it, and hands the pair to the flow, which derives the
PBKDF2-HMAC-SHA256 record and keeps only that record.  The state step then
writes it into the installed system and reads it back through the installed
system's own reader; a mismatch fails the install closed.  An install with no
operator account writes no administrator record at all and says so instead:
`admin=UNSET reason=UNATTENDED` on the unattended entry, or
`admin=UNSET reason=OPERATOR_DECLINED` when the operator cancelled the screen.
The installer kernel has no hardware entropy owner, so its salt is a unique
(not secret) derivation over a domain string, the chosen target disk's
identity, the boot-relative tick count and a per-run counter; the console keeps
the hardware entropy owner.

On a machine that already carries a system the same run reports the preserving
path instead:

```
[PRODUCTION-INSTALL] layout mode=preserve reason=existing-system declared=0 chosen=none
[PRODUCTION-INSTALL] boot-menu state=GENERATED reason=chainload-entry stage=chainload-entry entries=3 loaders=2 missing=0 unreadable=0 disks=1 target=1 d1=GPT/esp1/OK/2
[INSTALLER-PRESERVE] plan mode=dual-boot disk=1 table=gpt entry_count=128 preserved=4 added=3 esp_reused=1 ...
[INSTALLER-BOOT-MENU] menu firmware=uefi entries=3 loaders=1 missing=0 unreadable_disks=0
[INSTALLER-BOOT-MENU] config-begin
...
[INSTALLER-BOOT-MENU] config-end
[PRODUCTION-INSTALL] esp payload=VERIFIED first_lba=2048 length_lba=<the reused ESP> payload_bytes=... mode=preserve staged_bytes=... mechanism=REMOVABLE_MEDIA_PATH
[PRODUCTION-INSTALL] boot-entry state=AVAILABLE mechanism=REMOVABLE_MEDIA_PATH path=\EFI\BOOT\BOOTX64.EFI kernel=\boot\hfoc-kernel.elf config=\boot\limine\limine.conf menu=\EFI\BOOT\limine.conf entries=3 timeout=10s source=GENERATED payload_set=7 required_sectors=... esp_first_lba=2048 esp_sectors=... sealed_bytes=... sealed_digest=... existing_loader=FREE|PRESERVED ... existing_config=FREE|PRESERVED ... verify=SEALED_MEDIA variable=UNAVAILABLE boot_entry=NONE
```


## The confirmation source, and why

The acceptance host has no PS/2 keyboard, and the serial console is a
publication channel rather than an input path, so the confirmation cannot be a
prompt. The confirmation source is therefore the **media itself**: the operator
builds an authorised medium that names the confirmation token and the one disk
serial, and the kernel publishes exactly that as the confirmation record
(`source=media-token`). The shipped installer ISO carries no declaration, so it
refuses — an install never happens silently, and a medium that was not
deliberately authorised cannot write anything.

The console path exists too, for machines that do have local input, and since
ITEM-495 it is confirmed **on the screens** rather than by a typed token.
ITEM-514 makes that order the ordinary installers' one - disk, then scheme,
then the plan:

```
Select installation disk          ← the operator picks a row with ENTER
Choose the installation scheme    ← then a named scheme
  Disk: HFOC Example SATA Fixture  DEADBEEF00000001  240.0 GB  GPT
  Free space: 228845 MB   Needed: 128 MB
> Install alongside the existing system
  ...
  Erase the whole disk and install HFOC
  Manual: give up one existing partition

WARNING: EVERYTHING ON THIS DISK WILL BE DELETED   ← the erase scheme only
  Disk: HFOC Example SATA Fixture  DEADBEEF00000001
  Capacity: 240.0 GB   Transport: SATA   Table: GPT
  Nothing has been written yet.
ENTER erase this disk and install   Q go back

Confirm the installation plan      ← the summary: the plan the writer executes
  Scheme: Erase the whole disk and install HFOC   Declared: none   Chosen: erase
  Will be deleted: everything on this disk
  Will be created:
    EFI system partition  33 MB
    HFOC system  64 MB
    ...
  Nothing has been written yet.
ENTER confirm this plan   Q go back to the schemes
```

ENTER on the summary is the confirmation and publishes `source=operator-screen`:
the disk is the one the operator chose, the scheme is the one the operator
selected (the record names `chosen=`), the plan is the one the same contract
planner built for the writer, and a decision made at the screen cannot widen the
write. `Q` on the erase warning page goes back to the schemes with nothing
confirmed; `Q` on the scheme list refuses the run with `CANCELLED_BY_OPERATOR`
and zero writes. The `INSTALL CONFIRM <token>` command this document used to
describe is gone - the person who read the plan and pressed ENTER is the
confirmation. ITEM-395 makes the operator path usable on the acceptance host.

### Building the authorised medium

```
make production-installer-authorised-media \
    HFOC_INSTALL_MEDIA_CONFIRM=ITEM-394-UNATTENDED-INSTALL \
    HFOC_INSTALL_MEDIA_TARGET=DEADBEEF00000001 \
    HFOC_INSTALL_MEDIA_MANAGEMENT=local-only \
    HFOC_INSTALL_MEDIA_NETWORK=static \
    HFOC_INSTALL_MEDIA_ADDRESS=10.0.2.15/24 \
    HFOC_INSTALL_MEDIA_GATEWAY=10.0.2.2 \
    HFOC_INSTALL_MEDIA_DNS=10.0.2.3
```

`HFOC_INSTALL_MEDIA_TARGET` is the serial of the disk that may be written; it
must be the declared acceptance serial from `AGENTS.md` section 3a and
`release/hfoc-hardware-acceptance-matrix.txt`. Writing this ISO to a stick and
booting the acceptance host from it is the `INSTALLER` stage of the runbook.

## What the operator sees on the local console (ITEM-405)

The records this flow publishes went to the serial stream only. The acceptance
host has no serial capture, so a run that refused - or an appliance whose
network path never came up - left the operator with a banner and nothing else.
Every record is now mirrored to the framebuffer console through
`console_publish_record()`:

* every `[PRODUCTION-INSTALL]` record the flow publishes: the declaration, the
  candidates, the layout decision, the preview, the confirmation, the measured
  source sizes, the sealed-media verification, the machine, the preserve and
  boot-menu records, the layout record and the boot-entry record (available or
  refused);
* the `[PRODUCTION-NET]` record (device, readiness, backend, PCI identity, chip,
  link, status, stage, MAC, reason), `[PRODUCTION-NET-MODE]`,
  `[PRODUCTION-DHCP]` and `[PRODUCTION-DNS]`, so a machine with no serial
  capture can tell whether the NIC came up and where the network path stopped;
* `[XHCI] state=… reason=… detail=…` and `[KBD ] state=… source=… ps2=… usb=…`,
  so a USB keyboard that did not enumerate says why.

The mirror is additive: it paints the framebuffer and never writes to the serial
stream, which stays byte for byte what it was. One call paints one bounded line
(`CONSOLE_RECORD_MAX`), a record identical to a recent one is not repainted (a
poll loop cannot scroll the screen), and a record published before the banner is
flushed immediately after it - before the first prompt - so nothing published
during early boot is lost. `docs/hfoc-installed-boot.md` documents the same
property from the installed-boot side, and `make test-console-host` proves the
four properties (paint-only, bounded, once, survives the banner).

## Reaching the panel without a serial console

The management address used to reach only the serial record
(`[PRODUCTION-DHCP] ... address=...`), so a machine without a serial capture
could not learn the URL. The local console now publishes it as soon as the
network has an address:

```
MANAGEMENT: https://192.0.2.10/ reachability=LOCAL_ONLY cert=persisted_self_signed
```

and the same record is published on serial as `[PRODUCTION-MGMT] url=...`. The
scope is the published management binding:

- `LOCAL_ONLY` (the default) admits no network ingress: the panel is reachable
  from the appliance itself, and the appliance cannot be reached from the
  operator's network until the operator says so;
- `MANAGEMENT_INTERFACE` is the explicit opt-in, published only when the boot
  entry declares `hfoc.management=management-interface`
  (`[PRODUCTION-MGMT-BIND] scope=MANAGEMENT_INTERFACE source=boot-declaration`).
  Then the panel is reachable at the printed URL.

The listener serves a per-appliance **self-signed** certificate (persisted in
the state volume when one exists, per-boot otherwise), so the browser shows a
certificate warning: the certificate has no public CA behind it, and the
warning is expected until a validated certificate is imported through the
management TLS lifecycle (ITEM-375). `cert=` on the console line states which
of the two states is in force.

## Proof

- `make test-production-installer-qemu` keeps proving the controller in-process
  and then runs the product flow for real:
  `scripts/test-qemu-production-install-flow.sh` boots the authorised media on
  OVMF with an AHCI disk and boots the unauthorised media, a mismatched serial,
  an undersized disk and — ITEM-517 — the authorised media again on SeaBIOS
  (no OVMF) to install and then boot the BIOS disk. The successful UEFI run must
  publish
  the ITEM-391 numbers and the GPT parsed from the raw disk image — outside the
  kernel — must reproduce all four roles; every refusal must leave the image
  byte-identical. The BIOS run must publish its own five-role plan, the
  `mechanism=BIOS_MBR` records and the `firmware=bios-legacy` menu record, the
  raw image must reproduce the measured stage1 recipe, the intact protective
  MBR and GPT CRCs, the stage2 region and the FAT boot volume's five files, and
  the installed disk must then boot alone to `[BOOT-CHAIN] verdict=ATTESTED`
  and the readiness record. ITEM-399 adds the two installed boots: the
  `local-only` and
  the `management-interface` medium each install a disk, the disk is booted
  alone, and the proof requires no `[PRODUCTION-INSTALLER]` record at all (the
  installed kernel is the product build), the sealed chain attestation, exactly
  one readiness record, the `[PRODUCTION-MGMT-BIND]` scope the medium declared
  with `source=boot-declaration`, and - read out of the raw image by
  `scripts/hfoc-esp-read.py` and compared with `cmp` - the ESP's
  `\boot\hfoc-kernel.elf` byte for byte equal to the medium's product kernel
  (with no installer proof marker in it) and its `\boot\limine\limine.conf`
  byte for byte equal to the medium's sealed configuration with exactly one
  `cmdline: hfoc.management=…` line. ITEM-411: QEMU runs the guest with
  `-no-reboot` and **without** `-no-shutdown`, so the reboot the flow performs
  after a completed install makes QEMU **exit by itself with status 0** - every
  successful run in the proof waits for that exit and fails if the machine is
  still running, asserts one `complete` record with `medium=REMOVE`, a `reset=`
  mechanism from the declared set and a `in=<n>s` countdown, and requires the
  completion record and the plan record to name the same first mechanism; every
  refusal must still be running after its record, with no completion record and
  no reset plan at all.  The flag pair matters and is not cosmetic: QEMU maps
  `-no-reboot` to `reboot=shutdown` and `-no-shutdown` to `shutdown=pause`, and
  with both a guest reset moves the machine to `RUN_STATE_SHUTDOWN` and pauses
  it - the first run of this proof did exactly that, and the reset looked like a
  hang.  The installed-disk phase keeps both flags, because it must not reset.
- `make test-platform-reset-host` proves the mechanism-selection decision for
  the shapes that matter: the acceptance host (no i8042, no ACPI reset
  register → `PCI`), a declared ACPI register (`ACPI` first, then `PCI`, then
  `I8042`, then the fault), an i8042 on a machine with no PCI bridge, and a
  platform that declares nothing at all (`FAULT` alone). It also pins that a
  declared register in an address space this kernel cannot write is never
  claimed, that a FADT without `RESET_REG_SUP` contributes no step, and the
  ACPI register width the table described.
- `make test-production-install-media-host` proves the declaration parser, the
  one-disk selection, the management opt-in (and, since ITEM-399, the
  difference between a declared `local-only` scope and no declaration at all)
  and the bounded records, including the ambiguous/absent/malformed cases.

## Honest limits

- **The platform boot entry.** The kernel has no EFI runtime, so it cannot
  write `Boot####`/`BootOrder`; the mechanism that makes the firmware find the
  installed disk is the removable-media path (`\EFI\BOOT\BOOTX64.EFI`), and
  the record says so instead of claiming a boot entry. The installed disk boots
  from that path (ITEM-397/398, proven in QEMU with the installer medium
  detached); a component with EFI runtime access that creates the boot entry is
  still the next step.
- **Cross-disk booting.** The installed configuration carries the chainload
  entry for a system on another disk (`guid(<GPT partition GUID>)`) and that disk
  is proven to be read and never written, but a firmware following that entry on
  real hardware is unaudited until the two-disk acceptance run
  (`docs/hfoc-installed-boot.md`).
- The serial record is the acceptance evidence; the framebuffer line is the
  operator's copy of the same facts.
- The management scope is a boot-entry declaration today. Since ITEM-399 the
  media declaration is written into the installed configuration, so an installed
  appliance publishes the scope the operator declared; it is still not persisted
  into the state volume, so a later edit of the boot entry is what changes it,
  and the reachability policy itself is ITEM-385's.
- The chip identity ITEM-399 publishes is proven on the host for the string and
  the family mapping, and its presence in the record is pinned statically; the
  `[PRODUCTION-NET]` record itself is only produced by a boot with a Realtek NIC
  in it, so the acceptance host's run is the first execution that prints it
  (`docs/hfoc-hardware-acceptance.md`).
- **The reset mechanism the acceptance host honours.** ITEM-411's QEMU proof
  shows that a completed install reboots the machine (QEMU exits) and that a
  refused run does not, and `make test-platform-reset-host` pins which mechanism
  the host's shape *selects* - `PCI` on a machine with no i8042 and no ACPI reset
  register. Whether that chipset obeys `0xCF9`, or whether the machine falls
  through to the i8042 or to the triple fault, is observable only on the
  hardware: the next acceptance run quotes the `[PLATFORM-RESET]` records and
  the `reset=` field of the completion record, and the last attempt line names
  the mechanism that worked. The reboot also ends the `INSTALLER` stage's boot
  log: the run's records must be captured before the machine goes down (the
  serial capture, or the local console), and a boot-log flush to the
  appliance's own disk belongs immediately before the reset.
- `VERSION` is not changed by this item.
