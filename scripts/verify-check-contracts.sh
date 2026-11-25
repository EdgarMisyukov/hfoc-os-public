#!/usr/bin/env bash
# ITEM-387: mechanical proof that splitting scripts/check.sh into the pinned
# modules under scripts/check.d/ did not change what `make check` enforces.
#
# The suite is now a thin entry point plus 39 sourced modules. This proof
# flattens the split tree back into the ordered stream of enforced statements
# (scripts/check-contract-stream.sh) and requires that stream to match the
# frozen baseline byte for byte. The baseline is the pre-split stream extended
# only by reviewed contract additions (ITEM-388 added the paced production NIC
# ring-cycle and ICMP budget assertions, ITEM-390 added the PMM proofs above
# the legacy bitmap cap, ITEM-391 added the production installation layout
# contract, ITEM-392 added the degraded PS/2 boot policy and its proofs, and
# ITEM-393 added the firmware-map/PCI-hole PMM proof, the page-fault physical
# record, the RTL8168 MMIO/DMA base separation and the boot-path proof stack
# budget, ITEM-394 added the production install flow and the install-media
# staging, ITEM-395 added the USB HID and xHCI local-input contracts, and
# ITEM-397 added the installed boot payload set, the boot-entry mechanism
# record and the installed-disk boot proof, ITEM-399 added the two-kernel
# medium, the installed configuration's declared management scope and the
# production network chip identity, and ITEM-405 added the ESP volume-extent
# rule, the ESP role ownership rule, the console record mirror and the
# stale-ESP install proof, ITEM-411 added the platform reset path with its
# mechanism-selection proof and the reboot at the end of a completed install,
# ITEM-410 added the durable appliance boot log - its fixed extent on the
# ESP, its flush policy, the console binding and the host reader that reads it
# back out of a raw image - ITEM-427 added the bounded console-retype slice
# (`scripts/check.d/49-console-retype.sh`), which pins the retype helper, the
# three console-driven proofs' use of it and the unchanged exactness checks,
# and ITEM-418 added the platform power slice
# (`scripts/check.d/47-platform-power.sh`), which pins the ACPI S5 mechanism
# selection, the refusal that names the missing ingredient and the QEMU proof
# that the machine leaves on its own, ITEM-420 extended the xHCI slice with
# the Evaluate Context command the driver must send and the completion-code
# names of xHCI 1.1 table 6-90, ITEM-423 extended the install-medium slice
# with the storage evidence a menu record has to carry, ITEM-419 extended
# the acceptance-gate slice with the local menu's model, its console binding
# and the QEMU proof that the machine leaves through the menu, and ITEM-428
# added the keep-running slice (`scripts/check.d/50-keep-running.sh`), which
# pins the PCI bus-master enable the hardware run showed the NIC driver never
# performed - both call sites, the guards that keep a refused or unread virtio
# function from publishing a command register or a live frame path, and the
# named refusal itself - the DHCP wire view and the loop heartbeat the
# boot record now carries, the bounded publication of a refused transmit, the
# order of a transmit restart (stop the part, rewind the ring, re-arm the
# receive ring, then bring it up) with its attempt counted before the part is
# touched, and the republish of the network record when the backend's reason
# changes, and ITEM-429 extended the install-medium slice with the NVMe
# bring-up wait: the millisecond budget and its step, the delay seam on the
# MMIO provider and the kernel provider that owns a time base before
# `timer_init`, and the `nvme_step=`/`nvme_wait_ms=` evidence the next hardware
# run reads, and ITEM-430 extended the acceptance-gate slice with the menu the
# machine offers when nobody asks for it: the pure offer decision and its idle
# window, the console's paint/record/ENTER/cancel, both host proofs and the
# no-keyboard QEMU markers, and ITEM-431 extended the acceptance slice with
# the heavy lock's exclusive wait budget and the merge guard: the exclusive
# and shared bounds and their overrides, the guard's exit codes and marker,
# and both host drills, and ITEM-432 extended the acceptance slice with the
# warning-free kernel: the `_Noreturn` triple fault, the fresh production
# rebuild the acceptance phase captures, the gate script it hands the log to,
# and the host drill that drives the gate's reading, and ITEM-433 closed the
# window between the merge guard's verdict and the merge it guards, and
# ITEM-436 added the keyboard-binding slice
# (`scripts/check.d/51-keyboard-binding-record.sh`), which pins the
# classification of a completed interrupt IN transfer against the 8-byte boot
# report, the driver's refusal counter and remembered last length, the binding
# the driver selected, the `[KBD ]` and heartbeat fields that publish them, the
# console command's live counters and both QEMU proofs that require them,
# and ITEM-437 closed the boot-stack budget proof's own race: the disassembly
# is measured from a file so a reader that stops early cannot SIGPIPE its
# writer, the drill that proves the shape with a fake disassembler and its
# negative control, and the host-suite registration of that drill, and ITEM-438
# extended the keyboard-binding slice with the half that makes a keyboard type:
# the candidate scoring that accepts report-protocol and no-boot-subclass HID
# interfaces, the report-descriptor walk and the conversion of its reports into
# the tested boot decode, the SuperSpeed control pipe and interval table, the
# bounded proof window that keeps the walk from stopping at the first silent
# candidate, the [KBD ] NO_REPORT finding and the fields the binding record
# publishes about all of it, and ITEM-439 extended the keep-running slice with
# the receive path that must be armed before it is bound: the arming read the
# bring-up already made, the two refusal names it now produces, and the two
# proofs - the QEMU one that requires `rx_arm=1` and a delivered frame on a
# boot with no configuration, and the host one that drives the arming read with
# its wrong-address and I/O-error controls, and ITEM-440 extended the
# acceptance-gate slice with the menu painted as one IRQ-safe write: the
# sequence pair around the rendered menu, the absence of the character-by-
# character paint, and the console host proof that makes the sequence
# observable and asserts each menu marker atomic, and ITEM-441 added the
# queue-current-item slice (`scripts/check.d/52-queue-current-item.sh`), which
# keeps exactly one `## Current item` heading - the last section - and the
# `## Item ITEM-NNN` history the other slices refer to, and ITEM-442 added the
# QEMU accelerator slice (`scripts/check.d/53-qemu-accel.sh`), which pins the
# one-per-phase accelerator decision, the PATH shim every proof reaches through,
# the three runners that adopt it, the host drill and the one host change that
# brings KVM to CI, and the lint glob that names the shim
# (`scripts/qemu-accel/*`), which has no .sh suffix to be matched by, and
# ITEM-443 extended the acceptance-gate slice with the menu's own records made
# IRQ-safe (offer, open, closed, cancel and the composite action lines), which
# is the same class ITEM-440 fixed for the painted menu, and ITEM-444 added the
# framebuffer-lowercase slice
# (`scripts/check.d/54-framebuffer-lowercase.sh`), which pins the lowercase
# glyph table and its lookup, the absence of the case folding that painted every
# letter as a capital, the host proof that drives the real renderer and requires
# the two cases to differ, and that proof's registration in the canonical host
# suite, and ITEM-445 added the panel-login slice
# (`scripts/check.d/55-panel-login.sh`), which pins the browser sign-in form,
# the one `HttpOnly`/`Secure`/`SameSite=Strict`/`Path=/` session cookie with its
# short `Max-Age`, the sign-out cookie that expires it, the refusals a partial
# form or an untrustworthy cookie meets, the `form-action 'self'` policy that
# still refuses a script, and that proof's registration in the canonical host
# suite. ITEM-497 added the input-verdict slice
# (`scripts/check.d/102-item-497-input-verdict.sh`), which pins the per-boot
# `[INPUT-PATH]` verdict and its carried copies, the tail scan at attach, the
# heartbeat that restates the verdict set, and the repeat gate that names only
# the per-second snapshots - the watchdog and mouse-silence records stay flush
# markers outside it. ITEM-445 step 2 extends the same slice with the two POST targets, the
# bounded request body both receive loops consume before completing, the
# credential boundary the serving path binds to the ITEM-401 enrollment, the
# `Set-Cookie`/`Location` the encoder prints, and the host and wire proofs that
# drive the whole flow. ITEM-446 adds `scripts/check.d/56-install-network-address.sh`,
# which pins the declared network on the install medium: the four directives, the
# one IPv4 validator, the refusals before the first write, the installed entry
# that states the address, the boot that applies and stores it, and the build-time
# negative controls. ITEM-447 adds `scripts/check.d/57-menu-address.sh`, which
# pins the address the machine's own menu sets: the four bounded questions, the
# one IPv4 builder (moved into the owner by this item), the refusals that commit
# nothing, the two-word record binding the boot publishes, and the console and
# wire proofs that replay the operator's whole gesture. ITEM-448 adds
# `scripts/check.d/58-mouse-input.sh`, which pins the boot mouse report and its
# counters, the two HID roles one controller holds at once (the role-aware
# configuration walk, the per-role rings, report buffers and decoded queues, and
# the mouse re-bound beside a keyboard that only answers when it is touched),
# the pointer's arithmetic and hit test, the console's click that runs the entry
# under it through the keyboard's own path, the rectangles that follow the
# screen as the log scrolls it, the [MOUSE] record with its NO_REPORT finding,
# and the QEMU proof that binds usb-kbd and usb-mouse on one controller. The
# slice-51 pins that named the driver's single-device state moved onto the
# role's own device in the same commit, because the counter and the binding are
# the same contract on a different owner. ITEM-449 adds
# `scripts/check.d/59-forward-declaration.sh`, which pins the one forward
# grammar and its access rule, the refusals that keep an unhonourable
# declaration from being sealed or installed, the directive that carries the
# rule into the installed boot entry, and the `[PRODUCTION-FORWARD]` record the
# appliance publishes before anything is opened, and
# `scripts/check.d/60-forward-relay.sh`, which pins the relay that acts on the
# rule: the second listen PCB and the session PCBs, the admission that happens
# before `tcp_connect`, the whole-chain-or-nothing receive that cannot be
# acknowledged twice, the idle deadline, the session record with its verdict
# and counters, and the byte-level QEMU proof that exchanges a challenge
# through the relay and refuses a source the declaration does not name).
# ITEM-451 adds `scripts/check.d/61-network-names-itself.sh`, which pins the
# record that names the receive path: the `bound`/`proto_published`/
# `netif_live` flags in `[PRODUCTION-NET]` and their part in its change
# decision, the per-class frame counters (ARP requests and replies received
# and sent, ICMP echo and other ICMP, TCP SYN with the SYN+ACK and RST that
# answer it, other IPv4, and every non-ARP/IPv4 ethertype as a class rather
# than as `malformed`), the heartbeat that publishes them, the event record
# that names the first frame of each class and reaches the durable log, and the
# QEMU proof that opens the panel with a SYN from the same segment, refuses a
# closed port with an RST and then requires the machine's own records to name
# each class):
# ITEM-452 adds `scripts/check.d/62-two-controllers.sh`, which pins the driver
# that walks every xHCI controller and every port: the per-controller session
# with its own DMA pool, event ring and two role devices, the machine state
# that keeps one live session per bound role plus one scratch session, the walk
# with no early exit, the round-robin service pass, the controller record that
# names itself (`pci=`, `controllers=`, `taken=`) and the per-device `[USB ]`
# record with port, speed, role, state, reason and identity (including
# `reason=ROLE_TAKEN`), the durable-log marker, the host proof that drives the
# all-controller walk, the new QEMU proof with two controllers at fixed PCI
# addresses and its non-HID negative, and the four documents that record the
# item.
# ITEM-453 adds `scripts/check.d/63-port-bring-up.sh`, which pins the port that
# Linux sees before it is believed empty: the pure bring-up decision and the
# link states that cannot sample a connect, the RxDetect write that clears the
# link state field first, the driver's read-decide-write-wait sequence with the
# early exit on a connect and the controller's shared debounce window, the
# per-port `[XHCI-PORT]` record (raw PORTSC before and after, decoded power,
# connect and link state, the writes it made and the wait it spent), the
# `[XHCI-PROTO]` record with the Supported Protocol fields and the raw
# capability dwords, the relative Next Capability Pointer the extended
# capability walk follows, both durable-log markers, the host proof that drives
# the decision with the exact value the owner's machine published, and the QEMU
# proof that names every port, checks the protocol groups partition the ports
# and publishes a SuperSpeed device on a USB3 port.
# ITEM-454 extends `scripts/check.d/55-panel-login.sh` with the sign-in document
# that explains itself: the one sentence naming `admin-provision <SECRET>`, its
# escaped placeholder, the measured asset budget and every recorded copy of the
# served document with its Content-Length, and the case-folding comparison the
# console's own command names go through, so `help` prints what the parser
# accepts.  ITEM-455 extends `scripts/check.d/34-appliance-acceptance-gate.sh`
# with the offer that names the network key: the key list the menu publishes
# from its own table, the hint that the static address is set behind `n`, and
# the composed single-write shape both offer records kept when they grew.
# ITEM-456 extends `scripts/check.d/45-qemu-marker-timeout-retry.sh` with the
# installed-boot proof that declares its own timing class: the canonical record
# every marker wait writes, the load-tolerant window of the interactive menu
# phase, and the failure path that kills the guest it started.
# ITEM-457 adds `scripts/check.d/64-console-scroll.sh`: the console's scroll is
# a row move with 8- and 4-byte words and no per-pixel loop, the incoming rows
# are filled and the region above them is not written, the console publishes
# `[CONSOLE] scroll rows= bytes= ms= max_ms=` atomically and only as a summary,
# and both the host proofs and the QEMU burst stay registered.  ITEM-460 adds
# `scripts/check.d/67-console-journal.sh`: the console's own command and answer
# reach the durable journal as `[CONSOLE] cmd=`/`out=` through a journal-only
# path, a secret-bearing command is redacted, the enrollment record names its
# last refusal, and the menu and its offer are journaled too.  ITEM-461 adds
# `scripts/check.d/68-console-budget.sh`: the `[FB]` mode record, the journal-only
# `[CONSOLE] paint` and `[PRODUCTION-LOOP] iter_ms`/`iter_max_ms` costs, the
# served-record coalescing, the `repost_ms_max` re-arm measurement, and the
# two-mode QEMU load proof.  ITEM-462 adds
# `scripts/check.d/69-ci-artifacts.sh`: the bounded diagnostics collector (text
# evidence, a per-file tail, a byte budget, the named exclusions), the runner
# cleanup that removes old test-artifact directories and publishes the count
# and the bytes, the workflow wiring of both, the upload's repeated
# `!**/*.raw`/`!**/*.img`/`!build/hfoc-os-*.iso` exclusions under a bounded
# retention, and the host proof that drives every rule and rejection.  ITEM-463
# adds `scripts/check.d/70-console-4k.sh`: the `resolution: 1280x720` request,
# the `[FB] requested=/actual=/fallback=` record and its consistency rule, the
# cycle-counter microsecond clock and its measured frequency, the `us=`/`max_us=`
# paint, scroll and pointer costs, the heartbeat that republishes them every
# window, the 3840x2160 QEMU mode with its microsecond budgets, and the narrowed
# `Kernel panic ` fail-closed signature whose all-clear the slice's fixtures
# pin in both directions.  ITEM-466 moves the slice's console-scroll anchor to
# the runtime cell name the automatic font scale introduced
# (`console_cell_height`) and adds `scripts/check.d/72-console-shadow-scale.sh`:
# the RAM shadow that a scroll moves rows in before writing video, the physical
# allocation the boot binds and its `[FB] shadow` record, the pure font-scale
# rule with its documented floor and cap, the runtime scale the console takes
# from the actual mode, and both host proofs that pin them.  ITEM-466 (round 5)
# extends slice 70 to the automatic mode choice: no `resolution:` in either boot
# configuration or in `installer_boot_menu.c`, the `preferred=`/`available=`
# fields the pure `framebuffer_choose_preferred()` rule publishes, the
# three-EDID QEMU proof with the automatic scale 1/2/3, and the screen output
# budget whose suppressed count the heartbeat carries.  ITEM-468 (round 7) adds
# `scripts/check.d/74-xhci-diagnostics.sh`: the once-a-second diagnostic
# heartbeat the ordinary-boot loop calls, the three journal-only record shapes
# (`[XHCI-DIAG]`, `[INPUT-DIAG]`, `[LOOP-DIAG]`) and every field the owner
# asked for, the `XHCI`/`INPUT` console commands and their transcript, and the
# console record's own 320-byte line bound.  ITEM-469 (round 8) adds
# `scripts/check.d/75-xhci-trb-queue.sh`: the queue of armed interrupt
# transfers (`XHCI_ARM_DEPTH` in `XHCI_REPORT_SLOTS` per-role buffers), the
# in-handler top-up after every completion, every counted completion code with
# missed service and event ring full named, and the `armed=`/`mse=` and
# `kbd_armed=`/`kbd_mse=` fields the diagnostic and input records publish.
# ITEM-470 (round 9) updates that slice to the per-role depth and adds
# `scripts/check.d/76-pointer-speed-queue-honesty.sh`: the pointer scaled by
# the console's own glyph scale with its bounded soft acceleration, the
# sixteen-deep mouse queue in the sixteen-slot report pool with the deeper
# interrupt ring and the `no_trb` window counter, and the literal main-loop
# turn counter (`loops=`) published beside `iter_us`, together with the
# `[PRODUCTION-INPUT]`/`[PRODUCTION-INPUT-TAIL]` split and the QEMU proof that
# requires `mouse_armed>=16`, `mouse_mse=0`, `mouse_no_trb=0` and `loops>0`.
# ITEM-471 (round 10) adds `scripts/check.d/77-pointer-edges-calibration.sh`:
# the single-report `POINTER_MAX_STEP` cap, the `pointer_sprite_origin()` that
# keeps the arrow whole at the right/bottom edges, the `x=`/`y=`/`clamped=`
# fields of `[POINTER]`, the ten-tick cycle re-calibration and its pure
# `timer_window_average_us()`/`timer_calibration_rate()` helpers, and the
# `cal_hz=` the loop diagnostic publishes beside `loops=`/`iter_us=`.
# ITEM-472 (round 11) adds `scripts/check.d/78-pointer-telemetry-gain-time.sh`:
# the once-a-real-second `[POINTER]` record with the applied-delta sums
# (`dx=`/`dy=`), the largest single delta (`dmax=`) and the `PTR` gain handle,
# the PIT-hardware TSC calibration with `tick_hz=`/`second=` in `[LOOP-DIAG]`,
# and the host proofs behind them.
# ITEM-473 (round 12) adds `scripts/check.d/79-xhci-event-ring-arm-depth.sh`:
# the event ring as its whole 4 KiB page (`XHCI_EVENT_TRBS` 256) with the static
# assertions that every armed queue stays below its capacity, the `ARM` handle
# for the mouse depth, the Host Controller Event decode with `ringfull=`/
# `host_events=` and the per-role `[INPUT-CODE]` record (`last=`, `codes=`,
# `ep_state=`), and the QEMU proof that requires `mouse_arm_depth>=16` with
# `ringfull=0`.
# ITEM-474 (round 13) adds `scripts/check.d/80-loop-phases-batch-scroll.sh`:
# the `[LOOP-PHASES]` once-a-second budget (`turns=`, `console_us=`, `xhci_us=`,
# `input_us=`, `net_us=`, `other_us=`, `work_us=`, `hlt_us=`, `turn_us=`) with
# the console cost subtracted from every measured span, the per-turn scroll
# batch (`console_scroll_pending_cells`, `console_defer_buffer`,
# `console_flush_deferred()` and the main loop's flush before `cpu_hlt()`), the
# host proof that eight lines move the body once, and the QEMU proof that reads
# every field and bounds `turn_us`.
# ITEM-475 (round 14) adds `scripts/check.d/81-pit-tick-production.sh`: the
# `HFOC_PRODUCTION_PIT_TICK` production policy that keeps the legacy 8259
# PIC + PIT as the 100 Hz tick source with the two IRQ0 guards around the Local
# APIC runtime coordinator, the `lapic_timer_tick_source()`/`tick_source=`/
# `wake_hz=` publication, the one-shot `[IOAPIC] irq0` bring-up lines, the
# millisecond administrator/panel session lifetime the honest clock exposed,
# the `$(LAPIC_RUNTIME_ISO)` drill/QEMU build every APIC-focused proof now runs
# on, and the QEMU production proof that requires `tick_source=pit` with a
# PIT-rate loop.
# ITEM-478 (round 16) adds `scripts/check.d/84-screen-service-journal.sh`: the
# journal-only service funnel (`console_service_line()`), the boot-screen record
# `[CONSOLE] boot paint lines=<n> service_lines=<m>`, the banner/readiness split
# and the service call sites of `kernel.c`, `xhci.c`, the installer, power and
# reset, plus the host and QEMU proofs that read them.
# ITEM-479 (round 17) adds
# `scripts/check.d/85-input-subphases-watchdog.sh`: the input path's three
# measured sub-spans (`in_xhci_us=`, `in_kbd_us=`, `in_ptr_us=`), the window's
# worst turn and its named span (`worst_us=`, `worst_phase=`) with the stall
# count (`over50=`), and the stalled-endpoint watchdog that publishes
# `[INPUT-WATCHDOG] action=REARM` and re-arms the role, with the host proofs and
# the QEMU assertions behind them.
# ITEM-480 (round 18) adds `scripts/check.d/86-mouse-turn-budget.sh`: the
# pointer's per-turn draw budget (`LOCAL_INPUT_MOUSE_DRAW_BUDGET`), movement
# coalescing, the one-drain batch and the `[POINTER-BATCH]` record.
# ITEM-481 (round 19) adds `scripts/check.d/87-console-record-bounds.sh`: the
# journal record bound enforced by construction (`CONSOLE_RECORD_U64_WORST`,
# `CONSOLE_RECORD_HEX64_WORST`, the `-TAIL` spill and the splitter for pre-built
# records), the `[CONSOLE-BOUND]` guard, the host proof and the QEMU journal
# assertions.
# ITEM-482 (round 20) adds `scripts/check.d/88-keyboard-fast-path-budgets.sh`:
# the one-cell typing fast path, the keyboard's turn budget
# (`LOCAL_INPUT_KEYBOARD_BUDGET`), the single `console_end_input_batch()` drain
# and the `kbd_events`/`ptr_draws` budget fields in `[LOOP-PHASES]`.
# ITEM-483 (round 20 follow-up) extends the same slice with the output
# generation that invalidates the one-cell fast path after foreign output
# (`console_output_generation`, `console_input_line_generation`, the
# `console_mirror_open()` and `clear_body()` bumps) and the host proof that a
# painted and then a journal-only record each force the next keystroke to
# re-emit the prompt and the whole line.
# ITEM-484 (round 22) adds `scripts/check.d/89-console-input-batch.sh`: the
# pointer's readiness announcement (`console_pointer_ready()`) that centres and
# paints the arrow exactly once before any movement or click, the deferred
# echo's in-batch replay before the single `console_end_input_batch()` drain,
# the `console_note_echo_paint()` flag, and the three host proofs behind them.
# ITEM-485 adds `scripts/check.d/90-host-suite-fixture-isolation.sh`: the
# boot-log store/reader fixture pair that must not share a parallel phase, the
# serial gate scheduling that separates them, and the manifest proof that
# carries the same assertion.
# ITEM-486 (round 23) adds `scripts/check.d/91-other-us-subphases.sh`: the three
# named top-level spans that split the former `other_us` remainder (`svc_us`,
# `journal_us`, `store_us`), the residual accounting and the worst-phase names,
# with the host and QEMU proofs behind them.
# ITEM-486 (round 24) adds `scripts/check.d/92-input-latency-shadow-budget.sh`:
# the shadow-only framebuffer and its batched flush, the bounded network pass
# and the named mouse event path, with the framebuffer/network telemetry and
# the host proofs behind them.  The same slice now also pins the loop's own
# clocks (`loops`, `ticks`, `real_seconds`) in the record that carries the
# window they are read against, and the cumulative `[LOOP-SUMMARY]` record
# whose newest copy still describes the whole run after the journal's ring has
# reclaimed the records around it.
# ITEM-490 extends `scripts/check.d/97-item-489-installer-storage.sh` with the
# operator entry's mandatory screen: the session that waits with no deadline and
# hands back a copy of the chosen row, the model's choose-by-id, the flow's
# awaiting/choose/cancel entry points and its own copy of the media it was
# handed, the CANCELLED_BY_OPERATOR refusal, the console-first medium
# (`HFOC_INSTALL_MEDIA_ENTRY=console`), the boot-loop call site, and the QEMU
# proof that drives the whole operator gesture.
# ITEM-489 adds `scripts/check.d/97-item-489-installer-storage.sh`: the
# installer's storage screen, whose model may not include the block device, the
# ESP, the layout, the flow or a controller header and may not contain a write
# call at all, whose inventory never selects a disk on the operator's behalf,
# whose selectable state requires the identity and the geometry to have been
# read, whose keys are decided by a pure function (the arrows and the `w`/`s`
# the hint prints, with every other key doing nothing) and whose unattended flow
# only publishes the bounded `[INSTALLER-STORAGE]` reading instead of opening a
# waiting screen.
# ITEM-492 adds `scripts/check.d/98-item-492-nic-discovery.sh`: the machine's
# network controllers discovered by class and described by identity, a discovery
# walk that names no vendor, the one registry of driver claims that refuses an
# ambiguous claim, the bounded refusal of a table that cannot hold the machine,
# the capability reading that never claims an initialised device, the `[NIC]`
# reading published on every boot and the installer's `install=CONTINUES`, and
# the proof that "no controller" is driven on a machine that really has none.
# ITEM-493 extends `scripts/check.d/05-certificate-key-fail-closed.sh` with the
# compiled-in test certificate's remaining life: the check that it stays valid
# for at least a year, that its bytes match the digests the header declares,
# and its registration in the canonical host suite.
# ITEM-494 adds `scripts/check.d/99-item-494-intel-nic.sh`: the second NIC
# vendor claimed as one part (`8086:100E`) and not a vendor, the RTL8168 claim
# left untouched, a driver that masks interrupts and enables none (no MSI, no
# APIC/IOAPIC/SMP, no xHCI), the bound record carrying the MAC the part gave,
# the register-map bits the production proof corrected (`STATUS.LU` is bit 1,
# `RCTL.BAM` is bit 15), the PHY auto-negotiation restart that brings the link
# up, the production adapter's bind/transmit/poll wiring with its TDT/RDT
# doorbells, the ordinary-boot DHCP/DNS/SNTP proof on a machine whose only
# controller is the emulated Intel part, and the core proven by a host proof in
# the canonical suite because the acceptance host has no Intel controller.
# ITEM-496 adds `scripts/check.d/101-item-496-network-choice.sh`: the operator's
# DHCP/static choice answered as one IRQ-safe and durable write per question and
# per record, the current configuration published before the first question from
# the live wire (never from the record the menu is about to replace), the answer
# record naming both what was stored and what the interface still runs
# (`applies=reboot current=`), the Network entry bound only to a loaded
# persistent record so an unstorable answer is refused before the questions, and
# the QEMU proof that drives a refused answer, an accepted static address that
# answers the host's own ARP and ICMP, and the return to DHCP with a fresh lease.
# ITEM-498 added `scripts/check.d/103-item-498-honest-refusal-bodies.sh`, which
# pins the refusal body that names its status and its reason: the two pure
# builders, the family rule that gives a browser target the HTML document and
# everything else valid JSON, the serving path that no longer knows the frozen
# word, the host proof of both builders and the refusal of a body that does not
# fit, the eight QEMU refusal proofs, and the documents.
# ITEM-499 added `scripts/check.d/104-item-499-panel-navigation.sh`, which pins
# the panel menu that leads to the pages the machine serves: the href taken from
# the same section-path accessor the panel router matches, the writer that no
# longer concatenates the `/panel` prefix with a full path, the host proof that
# reads every rendered document and requires each published link and form target
# to be served, the QEMU proof that walks the served menu as a remote client,
# the UNAVAILABLE reason on the page, and the re-baselined page budgets.
# ITEM-501 (in progress) added `scripts/check.d/106-item-501-partition-layouts.sh`,
# which pins the discovery half of the item: the named partition layouts, the
# MBR extended container and its bounded EBR chain, the GPT LVM GUID and Btrfs
# superblock probe, the container-free overlap rule and the host proof of all of
# them.  The screen, the record, the free-space numbers and the release mode are
# deliberately not pinned there yet - they are not implemented.
# ITEM-500 added `scripts/check.d/105-item-500-operator-any-machine.sh`, which
# pins one medium for any machine: the declared disk staying the unattended
# entry's authorization and its `TARGET_NOT_FOUND`, the operator entry keeping
# `ACTION_CONSOLE` with nothing selected on a machine that does not carry the
# declared disk, the `declared=NOT_ON_THIS_MACHINE disk=NONE identity=NONE`
# record and the screen line that names the reason, the refusal page an
# operator entry paints when it cannot install at all, the host proofs of both
# sides, the QEMU proof on the machine the medium was not declared for, and the
# documents.
# ITEM-504 increment 8 added `scripts/check.d/115-item-504-facade-relay.sh`,
# which pins the facade relay: the one owner of the `Host` -> route decision
# with a refusal that zeroes the whole route, management keeping priority over
# `/panel/...` and `/api/...`, the declared `tls=off` route led to exactly its
# backend over the terminated TLS session, the answer handed to TLS before a
# segment is refused and immediately after one is staged (otherwise it would
# advance one segment per retransmit timer) in a client buffer that holds a
# whole receive window plus a segment (a delivered pbuf is not bounded by our
# own TCP_MSS: the QEMU link delivers 1440-byte pbufs), the named refusals (404
# for an undeclared name, 400 for one the table cannot carry, 503 for `tls=on`
# because the frozen profile has no TLS client), the release that forgets the
# target, and the QEMU proof that commits a route against a live backend on the
# test host and requires that backend's own multi-segment body back whole.
# Increment 9 extends that proof to two declared names committed against two
# different live backends, each name required to answer with its own backend's
# body, so a facade that ignores the name fails the phase.
# ITEM-505 increment 2 added `scripts/check.d/117-item-505-live-panel-view.sh`,
# which pins the live panel view on a real appliance: the compose module and its
# five bounds (the recent-log bound equal to the renderer's per-value bound), the
# refusal to cut a value that does not fit, the ordinary-boot owners (loaded
# hostname with the identity fallback, the production network's MAC, the
# service, the serving state and its bounded panel line), the serving path that
# stores the borrowed struct and applies a view published before the panel
# existed, the host proof and the re-measured generic page budgets (the worst
# case the live view can publish, not only the fixture values), and the QEMU
# proof that requires the live hostname, MAC, service, state and serving line on
# a generic section.
# ITEM-505 increment 3 added `scripts/check.d/118-item-505-system-hostname.sh`,
# which pins the System section configuring the machine's own hostname: the
# mutation plane's bound system executor (stage, commit, rollback, the failed
# write answered APPLY_FAILED, and no borrowed fallback), the one
# `SYSTEM_HOSTNAME` mutation behind both `POST /panel/system` and
# `POST /api/v1/system` with the GET body of the API target still read-only, the
# configuration model's hostname rule on the one-field stage, the durable
# `appliance_persistent_config_set_hostname()` write and the
# `[PRODUCTION-SYSTEM-SET] … applies=reboot current=…` record whose `current=`
# is the identity captured at serving publication, the System page with both
# readings and both forms and its measured budget, the host proofs, the QEMU
# proof that saves `node-505`, reads the applied record from serial and requires
# the saved name in `/api/v1/system` after the reboot, and the rewritten
# documents.
# ITEM-507 increment 1 added `scripts/check.d/126-item-507-sni-profile.sh`,
# which pins the frozen profile's version 2 and the enabled
# `MBEDTLS_SSL_SERVER_NAME_INDICATION` as a *presentation* choice (never trust),
# the verifier's positive SNI contract with ALPN, TLS 1.2, client mode and the
# other forbidden families still refused verbatim, the item 262 slice's moved
# version pin, the host proof that a profile without SNI or with the stale
# version is rejected, and the documents that record the owner's 2026-10-03
# decision and the measured price of the change.
# ITEM-507 increment 2 added
# `scripts/check.d/127-item-507-certificate-name-store.sh`, which pins the
# name -> certificate/key model (eight exact DNS names normalized by the route
# table's own rule, the ITEM-503 DER bounds, the explicit default-certificate
# declaration and the fail-closed lookup) and its durable record placed after
# the ITEM-503 certificate record in the same CERTIFICATE-namespace store: the
# format, the read-back before OK, the sealed-volume refusal, the empty table
# on every corrupt or foreign record, the key never rendered and the host
# proof that drives all of it against a memory block device.
# ITEM-507 increment 3 added `scripts/check.d/128-item-507-sni-selection.sh`,
# which pins the pure selection rule (EXACT / DEFAULT / REFUSED, a client
# without SNI getting the declared identity, an unknown name refused unless the
# record declares the default), the serving callback registered with
# `mbedtls_ssl_conf_sni` that parses the pairs once and never allocates, the
# honest boundary between an absent store (the pre-507 single certificate) and
# an untrusted record (nothing declared), the sanitized per-handshake record
# drained by the ordinary-boot loop, and the QEMU proof that requires the
# record on a real handshake.
# ITEM-507 increment 4 added
# `scripts/check.d/129-item-507-certificate-name-mutation.sh`, which pins the
# fifth mutation-plane executor and its `CERTIFICATE_NAME` authorization entry
# (command id 16, the matrix at fifteen entries, the scriptable target and the
# moved 504/505/506 matrix pins), the stage that decodes and validates the PEM
# pair through the ITEM-503 import gate against the declared name and zeroes
# the key candidate on every path, the commit that builds on the saved table
# and proves the durable write before APPLIED, the
# `[PRODUCTION-CERTIFICATE-NAME]` record drained by the loop, the
# route-consistency refusal `ROUTE_WITHOUT_CERTIFICATE`, and the QEMU proof
# that commits the declaration on a live listener and proves it in force after
# the reboot.
# ITEM-507 increment 5 added
# `scripts/check.d/130-item-507-certificates-panel.sh`, which pins the twelfth
# panel section (index 11, the section count at twelve, the page indices moved
# only for the licenses page that follows), the page that lists the active
# summary next to the saved bounded `name (bytes)` list and carries the import
# and delete/confirmation forms without ever rendering key material, the
# `/panel/certificates` target as the same `CERTIFICATE_NAME` mutation on POST
# and the rendered page on GET, the serving publication of the two renderings,
# the re-measured budgets of every page the new navigation link grew, and the
# fixtures (exact login body, canonical menu, panel probes) that moved with the
# measurement, and moved the licenses-page budget pin of ITEM-384's rendered
# browser slice (`scripts/check.d/35-rendered-management-panel-browser.sh`)
# from 1964 to the 1998 bytes the navigation link grew it to, in the same
# commit as the document it checks.
# ITEM-507 increment 6 added
# `scripts/check.d/131-item-507-two-certificate-selection.sh`, which pins the
# two-certificate proof: the fixture kernel that writes two test-only pairs
# through the durable writer a panel commit uses, the hook that runs it between
# the account binding and the serving init, the fixture ISO and the target in
# both QEMU suite lists, the proof that two declared names receive two
# different certificates (an undeclared name refused, a client without SNI
# served the appliance identity, the record durable across a reboot), and the
# clean list the fixture directory joined.
# ITEM-508 increment 1 added
# `scripts/check.d/132-item-508-tls-client-profile.sh`, which pins the profile
# version 3, the client role the ACME module will need, the server role it
# still requires, every family the profile keeps forbidden, the verifier's
# positive contract and its per-source client-symbol refusal, the host proof's
# rejection of a profile without the client, and the documents that say what
# the client role is for and what it is not.
# ITEM-508 increment 2 added
# `scripts/check.d/133-item-508-acme-trust.sh`, which pins the one named
# anchor (its DER length and digest in both the image data and the host proof),
# the trust module's named refusals and clock-first order, its no-network and
# no-file surface, the TLS-profile object and the host-suite registration, and
# the honest RSA finding: the image anchor cannot be parsed while RSA
# verification stays off, so the module says ANCHOR_UNAVAILABLE instead of
# pretending to verify, and the increment that first verifies a real chain has
# to move that deliberately.
# ITEM-508 increment 3 added
# `scripts/check.d/134-item-508-acme-message.sh`, which pins the bounded JSON
# reader (no allocation, no files, no network, unknown escapes and trailing
# bytes refused), the reply readers (absolute https URLs, base64url tokens,
# RFC 8555 statuses, exact DNS identifiers, bounded lists), the http-01-only
# authorization rule and the host proof that drives all of it on fixed data.
# ITEM-508 increment 4 added
# `scripts/check.d/135-item-508-acme-jws.sh`, which pins the ES256 account key
# (P-256 only, JWK and RFC 7638 thumbprint from the DER mbedTLS writes), the
# DER-to-raw signature conversion the JWS requires, the strict base64url pair,
# the flattened envelope, the TLS-profile object, the host-suite registration
# and the proof that verifies the signature through the reversible conversion.
# ITEM-508 increment 5 added
# `scripts/check.d/136-item-508-acme-flow.sh`, which pins the transport-free
# order spine: the clock refusal before anything is read, the account, order
# and authorization steps that build real JWS requests, the POST-as-GET read,
# the http-01 key authorization, the bounded records that carry no nonce or
# token, and the proof that verifies every produced request's signature.
# ITEM-508 increment 6 extended it with the challenge and finalize steps, the
# optional certificate member the reply reader now carries, the out-of-order
# refusal that leaves the flow usable, and the proof cases that drive the order
# to CERTIFICATE_READY and refuse an invalid challenge and a valid order
# without a certificate URL.
# ITEM-508 increment 7 added
# `scripts/check.d/137-item-508-acme-challenge.sh`, which pins the HTTP-01
# answer decision: the one request line the listener reads, the exact
# well-known path for the armed token, GET and HEAD as the only methods, the
# 900 s lifetime and the disarm, and that the module stays free of transport,
# console output and mbedTLS includes.
# ITEM-508 increment 8 added
# `scripts/check.d/138-item-508-acme-certificate.sh`, which pins the
# certificate step: the PEM chain decoded member by member, the chain
# verified against the named anchor and the declared name at the instant
# handed in, the refusals that fail the order against the caller errors
# that leave it usable, and the DER chain published for the store.
# ITEM-508 increment 9 added
# `scripts/check.d/139-item-508-acme-renewal.sh`, which pins the renewal
# policy: the expiry only a verified chain moves, the thirty-day window, the
# zero-clock and backwards-clock refusals, the refusal to renew without a
# certificate, one attempt in flight, the six-hour retry window, the bounded
# attempt budget with the operator's reset, the saturating retry instant and
# the bounded record that carries no material.
# ITEM-508 increment 10 added
# `scripts/check.d/140-item-508-acme-http.sh`, which pins the ACME client's
# HTTP exchange: the https-only URL split, the request that carries exactly the
# method, host, content type and length with `Connection: close` and is refused
# whole when it does not fit, and the reply read only when it arrived whole -
# the status line, the headers the item acts on, the named refusals for an
# obs-fold, a duplicate, a control byte, a chunked framing and a lying or
# over-bound content length, and the record that never echoes the body or a
# nonce.
# ITEM-508 increment 11 added
# `scripts/check.d/141-item-508-acme-transport.sh`, which pins the exchange
# driver over an injected link: the clock refusal before the link is touched,
# the two bounded windows, the request that leaves whole only after the
# handshake, the reply that is complete on its `Content-Length` or on the
# peer's close, the refusals named beside the HTTP module's own status, and the
# one record that carries no host, body, nonce or key.
# ITEM-508 increment 12 added
# `scripts/check.d/142-item-508-mbedtls-exception.sh`, which pins the checked
# per-source exception in the profile verifier: `--except=<symbol>:<source>`
# skips exactly one forbidden symbol in exactly one named source, the exception
# must name a checked source and a forbidden symbol, every other prohibition
# stays in place, and the host proof drives the permission and each refusal.
# ITEM-508 increment 34 added
# `scripts/check.d/161-item-508-acme-control.sh`, which pins the durable ACME
# control: the one-block `ACMC` record at block 44 of the CERTIFICATE store
# (after the certificate record and the name table) with its off default, its
# single request and its clear-on-disable rule, every corrupt-record refusal
# and the read-back before a save reports OK; the sixteenth authorization entry
# (command id 17, mutation `ACME_CONTROL`) and the two exact targets; the sixth
# mutation-plane executor and its unsupported-target refusal when unbound; the
# serving executor, the two HTTP form targets, the `[PRODUCTION-ACME-CONTROL]`
# record and the getter ordinary boot reads; the service state `DISABLED` that
# follows the committed value, holding a request as requested rather than
# executing it; and the `/panel/acme` page of its own with its 1985-byte budget
# and the Dashboard link that moved 1911 -> 1942.
#
# ITEM-508 increment 35 added
# `scripts/check.d/162-item-508-acme-order-path.sh`, which pins the order path
# ordinary boot runs: the driver with its named refusals, its bounded
# `[PRODUCTION-ACME-ORDER]` record and the two success consequences (the
# adopted ACME chain and the renewal policy moved to the verified leaf's own
# `notAfter`), the trust module's published leaf expiry that no refusal leaves
# behind, the boot composition with the production endpoint and the TLS-profile
# translation-unit membership, the durable spending of the operator's request,
# the ordinary loop that only publishes, ticks and mirrors, and the host proof
# that drives the whole order over a scripted link.
#
# ITEM-508 increment 36 added
# `scripts/check.d/163-item-508-acme-order-wire.sh`, which pins the wire proof
# of the production order path: the `acme-boot-order` family (production
# objects plus the fixture anchor and clock, its own port, the parallel QEMU
# phase membership), the stub URL branch and the fixture entry in the boot
# composition, the identity gate and the fixture identity the operator-less
# guest prepares through the production call, the harness that requires the
# production records and refuses a failure, a stub refusal or key material, and
# the extended guards of the two shared fixture objects.
#
# ITEM-508 increment 37 added
# `scripts/check.d/164-item-508-acme-order-panel.sh`, which pins the read-only
# ACME block's order value: the view field, the bounded `STATE/STEP` the boot
# composition renders from the driver's state and stage, the ordinary-boot
# wiring, the escaping and the Dashboard budget re-measured with it (1942 ->
# 1973) across the constant, the documentation and the moved pins of items
# 505/508.
#
# ITEM-509 increment 1 added
# `scripts/check.d/165-item-509-language-catalog.sh`, which pins the one string
# catalog of the interface: two languages with English the default, the single
# durable setting key `v1:system.language`, the fail-closed lookup that never
# falls back to English, the bounded UTF-8 reader, the sorted code-point
# inventory the console glyph proof consumes, the twelve panel section entries
# kept verbatim in English, the surface inventory document, and the host proof
# wired into both host suites.
#
# ITEM-509 increment 2 added
# `scripts/check.d/166-item-509-language-record.sh`, which pins the durable
# language inside the system record: the optional second `v1:system.language=`
# line validated through the catalog's own parser, the previous hostname-only
# format still read as English, the language line written only when it is not
# the default, the hostname change that keeps the chosen language, the named
# refusals without a write, the integrity catalogue key, the three host proofs
# and the boundary that no surface reads the setting before the glyphs exist.
#
# ITEM-509 increment 7 added
# `scripts/check.d/171-item-509-help-console.sh`, which pins 509.5: the help
# catalogue of nine topics with its four fields, the pure `help` classifier
# (list / topic / all / refusal), the console renderer whose every line is one
# IRQ-safe write, the named refusal of an incomplete catalogue, the retained
# `COMMANDS: HELP ...` vocabulary line, the re-measured code-point bound
# (70 -> 127, bound 96 -> 192) and the two proofs plus the live-console QEMU
# run that read the same strings in both languages.
#
# ITEM-509 increment 6 added
# `scripts/check.d/170-item-509-panel-language-switch.sh`, which pins the second
# half of 509.4: the one-field system stage that now accepts `hostname=` or
# `language=`, the catalog's own parser for the language, the commit that writes
# it through the same record the console command uses with its own
# `[PRODUCTION-SYSTEM-LANGUAGE]` audit line, the decision to reuse the existing
# authorization entry, the form field and the re-measured 1962-byte System page
# budget, and the recorded reason `<html lang>` waits for 509.7.
#
# ITEM-509 increment 5 added
# `scripts/check.d/169-item-509-panel-language.sh`, which pins the first half of
# 509.4: the System page's language pair and its view fields, the owner that
# reads the system record at render time and publishes UNAVAILABLE rather than
# guessing, the escaped rendering, the re-measured System page budget (1857 ->
# 1939 bytes) with the documented table and the ITEM-505 slices that pin both,
# the host proof, and the recorded reason the switch and `<html lang>` are the
# next increment (lang describes the content, which stays English until 509.7).
#
# ITEM-509 increment 4 added
# `scripts/check.d/168-item-509-console-language.sh`, which pins the console's
# `language` command: the pure classifier (`console_language_command.h`, the
# word folds and the value does not), the binding and the published language,
# the answers taken from the catalog as whole atomic lines, the one durable
# write plus the English `[CONSOLE] language=...` record with its refusal
# reasons, the production adapter in kernel.c, the host proof that drives the
# real console and the QEMU console proof that types both commands into a live
# guest and requires one of the two truthful answers and no missing glyph.
#
# ITEM-509 increment 3 added
# `scripts/check.d/167-item-509-cyrillic-glyphs.sh`, which pins the console's
# Russian: the Cyrillic tables in the same 5x7 cell as the untouched Latin ones,
# the code-point layer (`framebuffer_glyph_rows()`, `framebuffer_char_codepoint()`
# and the visible marker a code point with no glyph gets), the console's own
# strict UTF-8 decoding with code-point wrapping and byte-identical serial, the
# `[GLYPH-MISSING]` record, the deferred batch that holds whole code points, the
# coverage gate that walks the catalog's inventory, both host proofs and the
# documents that record the increment.
#
# ITEM-521 increment 1 added
# `scripts/check.d/203-item-521-journal-core.sh`, which pins the
# placement-independent core of the install journal: the backend interface that
# keeps the ITEM-520 fork out of the module, the version 1 slot layout with its
# CRC32, the bounded 64-record staging ring, the named OVERFLOW/TRUNCATED/FULL/
# UNAVAILABLE losses on the notify side with the funnel's re-entry guard, the
# slot reserved for the end marker, the single header of a resumed journal, and
# the host proof's registration in the compiled host suite.  The four ITEM-519
# slices moved their queue pin from `## Current item` to `## Item ITEM-519`
# because the current heading moved to ITEM-521, as ITEM-519 had moved it
# before them.
#
# ITEM-521 requirement 4 added
# `scripts/check.d/204-item-521-install-log-reader.sh`, which pins the reading
# half the owner asked for ("читается одной командой"): the `--disk /dev/sdb`
# invocation with no mount, the read-only open and the BLKGETSIZE64 sizing of a
# block device, the three exit codes (0 read, 1 malformed and named, 2 absent),
# the autodetected placement - MBR type 0xEF into the FAT12 chain, otherwise
# the raw extent at `blocks - 34 - 2048` - with both extent header layouts
# accepted, the torn/foreign/non-printable record that is named and never
# printed as text, the commit point of a growing and a preallocated file, and
# the host proof's registration in the compiled host suite.  Like increment 1,
# the slice chooses no placement: all three ITEM-520 forms are read.
#
# ITEM-521 increment 3 extended that slice with the machine-readable half: the
# reader's `json.dumps(journal, indent=2)` line and the witness cases that drive
# `--json` for a FAT12 file, a raw extent, a torn record and a medium with no
# journal, require one parsed document with the same placement/slots/capacity/
# lba/records/warnings as the text output, and require an empty stdout when the
# medium carries no journal.  The gap this closes was recorded by the previous
# window: the witness drove only the text half, so the branch an acceptance-host
# script uses was unchecked.
#
# ITEM-521 increment 4 (fork-independent; the ITEM-520 placement fork is still
# the owner's) extended it again with the reader's remaining documented paths:
# the `--offset` spelling (a FAT12 volume inside a whole disk at a non-zero
# offset, a bare volume dump at offset 0, and a bogus offset that must fail
# `malformed` rather than pass as "no journal") and `--file --json` for the
# mounted-file placement.  The slice now pins the offset branch itself
# (`ap.add_argument("--offset", ...)`, `if offset is not None:` /
# `return offset`), so the path cannot disappear silently; the witness is shown
# to bite by mutation outside the tree (12 failures without the offset branch,
# one on the new mounted-file placement).
#
# ITEM-521 increment 5 (fork-independent; the placement fork is still the
# owner's) added `scripts/check.d/205-item-521-journal-wiring.sh`, which pins
# the wiring that is the same for all three ITEM-520 answers: the journal as a
# second consumer of the store's *admitted* records (past the dump guard, the
# dedup and the periodic gate) rather than of the console funnel's repaints, the
# status path that reaches serial and the durable store directly and never
# re-enters the funnel, the flush hooks before the refusal page and on the panic
# path, the bind attempt hanging off the medium identity guard's MATCH verdict,
# the empty placement seam (comments stripped, its whole body is
# `return false;`), and the host witness registration.  No placement is chosen
# and no byte of any medium is written.
#
# ITEM-521 increment 6 (fork-independent) added
# `scripts/check.d/206-item-521-readonly-medium.sh`, which pins the medium that
# cannot carry the journal at all: the core's `install_journal_refuse()`
# (pre-placement only, named once through the existing notify path), the
# platform hand-off that touches no device, the kernel's optical decision
# (`media_type == INSTALL_MEDIUM_MEDIA_OPTICAL`) placed after the durable log is
# bound so the record reaches both the serial stream and the durable log, the
# host witness that proves the named refusal and the untouched placement, and
# the QEMU proof that boots the ISO as a CD and requires exactly one
# `[INSTALL-LOG] state=UNAVAILABLE reason=READ_ONLY`.  The placement fork stays
# the owner's: no placement is chosen and no byte of any medium is written.
#
# ITEM-521 increment 7 (fork-independent; the placement fork is still the
# owner's) added `scripts/check.d/207-item-521-placement-context.sh` and moved
# the two seam pins of slice 205 to the same contract.  Preparing the increment
# that follows the owner's answer found that the seam took no arguments, so no
# answer could reach the medium even though all three need the block view the
# medium was proven through and the guard's verdict (extent, type, disk id).
# The seam is now declared and defined as
# `install_journal_platform_bind_medium(struct block_device *device,
# const struct install_medium_verdict *verdict)`, and its only call site - the
# guard's MATCH branch - passes `&storage->block.device` and `&verdict`.  The
# body still binds nothing: with the comments stripped it is exactly
# `(void)device; (void)verdict; return false;`, so no placement module is named,
# no sector is read or written, and the install path is unchanged.  The slice
# was shown to bite before the source change: both updated pins fail on the old
# signature, and both pass after it.
#
# ITEM-521 increment 11 (fork-independent; the placement fork is still the
# owner's) added `scripts/check.d/210-item-521-panic-record.sh`, which pins the
# panic the medium's journal must carry: the two pure renderers
# (`[PANIC] <message> vector=<decimal> error=0x<hex> rip=0x<hex>` and
# `[PANIC] detail=<detail>`), the marked clamp that keeps an over-long message
# or detail visible instead of silent, the too-small-buffer refusal that is
# never a truncated record, the one emission inside `panic_finish()` between
# the store's panic flush and the journal's own panic flush, the host witness
# and its registration, and the queue and roadmap records.  The increment also
# moved the current-item pins of slices 207, 208 and 209 to increment 11, the
# way increment 10 moved 207 and 208.  No placement is chosen and no byte of any
# medium is written.
#
# ITEM-524 pinned ITEM-523's fix, which had shipped without a slice of its own:
# `scripts/check.d/211-item-524-paint-sample-progress.sh` holds the storage boot
# proof's paint-sample wait to what ITEM-523 made it - the progress loop, the
# 20 s stall bound and the 60 s cap checked against the record's own arrival
# time, the absence of the fixed 12 s window that lost run 38264136090's proof
# to a starved host, the unchanged assertion (three samples at least, at most
# one interval above two painted lines, the same `turns >= 5`/`wake_hz >= 50`
# filter), the canonical `HFOC-QEMU-MARKER-TIMEOUT` give-up that still names the
# bound and now the count, the runner document that records the flake, and that
# no such bound reached the kernel.
#
# ITEM-525 added `scripts/check.d/212-item-525-gate-serial-view.sh`, which pins
# the credential-durability gate proof's read of the console stream: the
# embedded python's `normalise` mode (and the untouched argument list of the
# monitor modes), the shell helper that renders a serial file through it, every
# assertion of that boot reading the normalised view - the listener, the three
# gate records and their order, the local session, the operator warning, the
# certificate-name record, the secret-leak check and the panic check - and no
# assertion of that boot reading the raw file, plus the behavioural witness
# `tests/https-gate-serial-view-host.py` and its registration in the canonical
# host suite.  The slice was shown to bite before the source change: the witness
# failed at `the shipped block accepts the interleaved record` on the recorded
# bytes of run 38264136090, where an `[IOAPIC] irq1` diagnostic split
# `[PRODUCTION-SECURITY] gate=ENABLED source=LOCAL_SECRET` and the shell greps
# missed a record the python half had already seen.
#
# ITEM-526 added `scripts/check.d/213-item-526-normalised-serial-reads.sh`,
# which pins the class ITEM-525 fixed one instance of: the shared renderer
# `scripts/hfoc-serial-view.py` and its one diagnostic rule (the same literal
# the embedded python helpers of the https, forward and dhcp-dns proofs carry),
# the `serial_view` helper of every covered proof rendering through it into a
# file, every assertion of the https proof's ten boots and of the sni-selection
# and four acme proofs reading that view (with the `>&2` failure diagnostics
# deliberately raw), and the behavioural witness
# `tests/serial-view-normalised-host.py` with its registration in the canonical
# host suite.  The witness was shown to bite before the source change: it failed
# 11 of 31 checks, naming every proof whose assertions still read the raw file,
# and with its ground-truth rule - a serial log is any variable a proof feeds
# to `-serial "file:..."`, so `$SERIAL_LOG` counts - it failed 1 of 52 on the
# previous commit and named the https proof's own first boot that a name-based
# inventory had missed.  CI run 38408474798 then showed the other half of that
# boot's problem: the https helper rendered through `$TEST_DIR/admin-enroll.py`,
# a file the proof writes only at line ~760 while the first boot asserts from
# ~440, so the view stayed empty and the boot timed out with the record in the
# serial log all along.  The helper now calls the shared renderer, and the
# witness runs every shipped helper with no `admin-enroll.py` present at all
# (`serial_view needs no late helper file`); the final witness is 58 checks, 0
# failures.  It runs each shipped helper against the recorded bytes of run
# 38264136090 and requires the split record back.
#
#
# ITEM-527 added `scripts/check.d/214-item-527-normalised-serial-rest.sh`, which
# extends the normalised-view rule to the ten remaining proofs that write a
# QEMU serial file (nine are converted; the two stdio-only EFI proofs are
# excluded in the witness comment).  It pins the shared serial_view helper of
# every one of them, the minimum number of view reads per proof, the raw forms
# the conversion removed, storage-boot's per-label logs in the witness
# (`EXTRA_SERIAL_NAMES`) and the runner document.  The witness is 130 checks, 0
# failures, and slices 207-210 now pin the queue's current item to ITEM-527.
#
# ITEM-528 added `scripts/check.d/215-item-528-narrow-diagnostic-rule.sh` and
# narrowed the shared rule: only the periodic lines the kernel writes from
# interrupt context are removed, because ITEM-526's broad form also deleted
# console records the proofs require ("[IRQ ] IF enabled; entering HLT event
# loop.", "[PIC ] Master mask: ...", "[PIC ] disabled; APIC runtime active.").
# Slice 215 pins the narrowed literal in the shared renderer and the three
# embedded helpers, the records the proofs still require, the kernel's periodic
# emitters, the witness controls that derive that set from the kernel source,
# and the runner document; slice 213 pins the new literal.  The witness is 133
# checks, 0 failures (5 failures before the change), and slices 207-210 pin
# ITEM-528.
#
# ITEM-529 added `scripts/check.d/216-item-529-python-halves.sh`, which converts
# the last five embedded python readers of the proofs the witness covers
# (test-qemu-irqs.sh and test-qemu-usb-two-controllers.sh): each is handed the
# shared renderer as its last argument and normalises every read through the
# same byte-identical `load_serial_view`.  The slice pins the loader, the reader
# counts, the unchanged assertions, the witness class control and the runner
# document; the witness is 152 checks, 0 failures (2 failures before the
# change), and slices 207-210 pin ITEM-529.
#
# ITEM-530 added `scripts/check.d/217-item-530-python-halves-panic.sh` for the
# panic/fault group (doublefault, nmi, pagefault, panic), whose shell halves have
# no serial assertion and so are listed in the witness's PYTHON_HALF_PROOFS:
# eight embedded readers now normalise the text they search while the byte
# bookkeeping stays raw.  The slice pins the loader, the read counts, the raw
# offset and suffix writes, the unchanged assertions, the witness list and the
# runner document; the witness is 168 checks, 0 failures (4 failures before the
# change), and slices 207-210 pin ITEM-530.
#
# ITEM-531 added `scripts/check.d/218-item-531-python-halves-input.sh` for the
# input/console group (keyboard-line-editing, local-control, network-foundation,
# nic-discovery, physical-sata-warm-reset): six embedded readers normalise the
# text they search while the byte bookkeeping stays raw and the QEMU `-d int`
# trace reader is deliberately left alone.  The slice pins the loader, the
# per-proof counts, the raw suffix slice, the unchanged assertions, the witness
# list and the runner document; the witness is 188 checks, 0 failures (5
# failures before the change), and slices 207-210 pin ITEM-531.
#
# ITEM-532 added `scripts/check.d/219-item-532-python-halves-https.sh` for the
# https matrix group (exhaustion, malformed-request, response, size-boundary):
# four embedded readers normalise their one serial read through the shared
# renderer.  The slice pins the loader, the removed raw read, the unchanged
# assertions, the witness list and the runner document; the witness is 204
# checks, 0 failures (4 failures before the change), and slices 207-210 pin
# ITEM-532.
#
# ITEM-533 added `scripts/check.d/220-item-533-python-halves-installer.sh` for
# the installer group (disk-screen, display, production-installer): four
# embedded readers normalise every read through the shared renderer.  The slice
# pins the loader, the per-proof counts, the unchanged assertions, the witness
# list and the runner document; the witness is 216 checks, 0 failures (3
# failures before the change), and slices 207-210 pin ITEM-533.
#
# ITEM-534 added `scripts/check.d/221-item-534-python-halves-drills.sh` for the
# https client, production-network and drill group (seven proofs, one embedded
# reader each): every reader normalises its one serial read through the shared
# renderer.  The slice pins the loader, the per-proof counts, the unchanged
# assertions, the witness list and the runner document; the witness is 244
# checks, 0 failures (7 failures before the change), and slices 207-210 pin
# ITEM-534.
#
# ITEM-535 added `scripts/check.d/222-item-535-python-halves-install-flow.sh`
# for the install-flow and physical-storage group (five proofs, fifteen embedded
# readers): every reader normalises its one serial read through the shared
# renderer, and the witness recognises the five literal log paths that the
# declared-serial-variable scan cannot see.  The slice pins the loader, the
# per-proof counts, the unchanged assertions, the witness list and the runner
# document; the witness is 264 checks, 0 failures (5 failures before the
# change), and slices 207-210 pin ITEM-535.
#
# ITEM-536 added `scripts/check.d/223-item-536-python-halves-heap-pmm-vmm.sh`
# for the heap, pmm and vmm group (four proofs, ten embedded readers): every
# reader normalises its one serial read through the shared renderer, and the
# witness recognises the two pmm serial variables the declared scan cannot see.
# The slice pins the loader, the per-proof counts, the shared rule and raw
# offsets, the unchanged assertions, the witness list and the runner document;
# the witness is 280 checks, 0 failures (4 failures before the change), and
# slices 207-210 pin ITEM-536.
#
# ITEM-537 added `scripts/check.d/224-item-537-python-halves-one-rule.sh` and
# made the python-half control honest: only a rendered view or the one
# `load_serial_view` counts as normalised.  Five covered proofs that still
# carried ITEM-526's wide rule (platform-power, production-storage-boot,
# usb-mouse-menu, usb-two-controllers, installed-boot) and the
# interrupt-controller group (acpi, ioapic-irq3-7, ioapic-irq8-15,
# ioapic-probe) had to convert fourteen readers in nine proofs.  The slice pins
# the loader, the per-proof counts, the absence of the wide literal, the raw
# byte offsets of the ioapic-uart proofs, the unchanged assertions, the witness
# predicate and the runner document; the witness is 296 checks, 0 failures
# (9 failures before the change), and slices 207-210 pin ITEM-537.
#
# ITEM-521 increment 12 filled the placement seam the whole series held open:
# the owner's answer to the ITEM-520 fork is variant (c) - the build reserves
# \HFOC\INSTALL.LOG in the medium's FAT12 partition and the kernel only reads
# that reservation and writes inside its clusters.  Slice 205 now pins the
# filled body (the reserved-file bind, the named refusal and the journal handed
# to the core) instead of the empty one, and bans the placements that were not
# chosen; slice 208 keeps the naming policy of a proven medium that got no
# placement and follows the increment-8 witness onto the RAM medium, where a
# reserved FAT12 file binds inside its own extent and a refused volume costs no
# write.  This finalization adds the closing slice 225 (224 was taken by
# ITEM-537): it pins the queue's answered ITEM-520 fork, the roadmap entry, the
# build-time reservation script and its host witness, the kernel module's
# absence of allocation, and the one-command reader the acceptance runbook now
# documents beside the boot-log reader.  The baseline, both anchors and the
# statement count move with it in the same commit.
#
#
# ITEM-538 added `scripts/check.d/226-item-538-python-halves-storage-config.sh`
# and converted the storage and configuration group: thirteen proofs
# (accounting-boundary, backup-restore, block-cache, block-device,
# boot-policy-regression, config-journal, data-region, persistent-config,
# persistent-config-reboot-acceptance, persistent-network-config,
# power-cycle-continuity-drill, storage-interruption, update-lifecycle) now hand
# the shared renderer to their python halves and read serial text only through
# `SERIAL_VIEW.normalise`, with the private ITEM-526 pattern deleted and the
# assertions unchanged.  The slice pins the loaders, the renderer arguments,
# the thirteen witness entries, the markers each reader asserts on and the
# runner document; the witness is 348 checks, 0 failures (13 failures before
# the change), and slices 207-210 pin ITEM-538.  The baseline, both anchors and
# the statement count move with it in the same commit.
#
# ITEM-539 added `scripts/check.d/227-item-539-python-halves-appliance-console.sh`
# and converted the last safe batch: twelve proofs (appliance-boot-console,
# appliance-console, install-notice, integrated-appliance-boot-acceptance,
# local-control-outage-drill, network-config, service-cascade-drill,
# supportability, syscall, trust-policy, user-init, user-iretq) now hand the
# shared renderer to their python halves and read serial text only through
# `SERIAL_VIEW.normalise`, with the private ITEM-526 pattern deleted and the
# assertions unchanged.  process-scheduler and scheduler-stress stay out: their
# assertions search the periodic lines the rule removes.  The slice pins the
# loaders, the renderer arguments, the twelve witness entries, the markers each
# reader asserts on and the runner document; the witness is 396 checks, 0
# failures (12 failures before the change), and slices 207-210 pin ITEM-539.
# The baseline, both anchors and the statement count move with it in the same
# commit.
#
# ITEM-540 added `scripts/check.d/228-item-540-pci-common-path.sh` and fixed the
# common PCI path the L875 finding exposed: `pci_device_identity()` separates
# identity/class from resource preparation, `pci_scan_class_ex()` filters by
# class before it probes any BAR and names the device a matched failure came
# from, I/O BAR decoding follows the specification's doubleword minimum and
# size alignment while the memory minimum and the bounds/overflow/64-bit
# refusals stay, and the storage probe now publishes `probe_attempted`, its
# stage, the primary PCI status and the failing device so the observed-storage
# report prints `state=PROBE_FAILED stage=... status=... bdf=...` instead of
# folding a failed search into `state=NOT_PROBED`.  The slice pins the split
# API, the scan's filter-before-probe order and named failure, the I/O BAR
# rules, the probe evidence, the report rendering and the host proofs; slices
# 207-210 pin ITEM-540 and 227 became a history pin.  The baseline, both
# anchors and the statement count move with it in the same commit.
#
# ITEM-542 added `scripts/check.d/230-item-542-refusal-page-qemu.sh` and proved
# the ITEM-541 refusal-page section on the machine: one shape-based probe reads
# both refusal shapes (no USB medium at all, and the operator medium booted from
# USB with its reserved journal frame damaged), bounds the ring tail by the
# machine's own count and keeps the way out on the page, and the in-CI
# two-controller proof now selects its `[INSTALL-LOG]`/`[INSTALL-MEDIUM]`
# records by shape so a page line cannot shadow them.  The same commit fixes the
# latent argv regression ITEM-533 left in the standalone proof (`extra` must
# stop before the shared renderer's path).  The slice pins the probe, both
# cases, the fixture and the hardening; slice 229 became a history pin.  The
# baseline, both anchors and the statement count move with it in the same
# commit.
#
# ITEM-543 added `scripts/check.d/231-item-543-ahci-legacy-bar.sh` and fixed
# the reason the owner's L875 refused NO_DISK (F-8): the Intel 7-series AHCI
# controller's legacy I/O BAR0 does not answer the size probe with a range, and
# that one BAR used to sink the whole matched device before AHCI could look at
# its memory ABAR.  A BAR whose probe explains no size is now reported unusable
# on itself (`present == false`, `decode_status`) instead of failing the read,
# while a structurally malformed register and a failed configuration access
# stay hard errors; `pci_scan_class_required()` lets a caller name the BARs it
# needs (the storage probe asks for AHCI BAR5 and NVMe BAR0) and still refuses
# a needed BAR by name, and `pci_scan_class_ex()` keeps the older "every BAR"
# contract.  The slice pins the mask, the tolerance, the preserved hard
# refusals, both storage call sites and the host proof; slices 207-210 were
# re-pinned to ITEM-543 and slice 230 became a history pin.  The baseline, both
# anchors and the statement count move with it in the same commit.
#
# ITEM-544 added `scripts/check.d/232-item-544-policy-wait-class.sh` and fixed
# the third `no-marker-timeout-record` failure of
# `test-production-service-https-qemu`: the after-boot policy proof awaited
# `[PRODUCTION-SECURITY] gate=PUBLISHED source=POLICY` in an inline loop that
# printed no canonical timeout record, so the suite - which infers the timing
# class from that record alone - read a guest that had not published it in time
# as an assertion and gave it no retry (the guest side of that run booted with
# `STATE_LOAD_FAILED storage=UNAVAILABLE`).  The wait now goes through
# `wait_for_record`, so the record's absence inside the deadline declares the
# canonical `HFOC-QEMU-MARKER-TIMEOUT` class and takes the documented one retry,
# while the assertion, the failure message, the panic message and the
# `[PRODUCTION-SECURITY]` dump stay exactly as they were.  The slice pins the
# wait, both messages, the dump, the absent inline deadline and the single bash
# spelling of the record; slices 207-210 were re-pinned to ITEM-544 and slice
# 231 became a history pin, while the ITEM-526 view-read bound in slice 213
# moved from 117 to 115 because the shared helper replaced the policy boot's
# two inline reads with one call.  The baseline, both anchors and the statement
# count move with it in the same commit.
#
# ITEM-545 added `scripts/check.d/233-item-545-superspeed-bot-storage.sh` and
# closed the two halves of the owner's 2026-10-17 SuperSpeed finding: the
# controller's post-reset window no longer ends because one port (the USB 2
# mouse) reported a connect while the USB 3 port beside it was still training
# its link - it is bounded by `XHCI_PORT_TRAINING_MS = 500`, the same grace
# Linux's `xhci_start()` gives, and ends only when `xhci_port_connect_pending()`
# finds no port left to wait for - and the SuperSpeed endpoint companion
# (descriptor type 0x30, USB 3.0 section 9.6.7) is read into
# `bulk_in_max_burst`/`bulk_out_max_burst`, with
# `xhci_endpoint_context_burst()` programming Max Burst Size in the endpoint
# context (xHCI 1.1 section 6.2.3.5, dword 4 bits 23:16) and the bound record
# naming `burst_in=`/`burst_out=`.  The slice pins both host proofs, the three
# companion parser cases, the QEMU phase E that forces a SuperSpeed BOT disk
# onto the USB3 half and requires its binding, its companion burst and its
# marked sector 0, and the paragraph recording that QEMU's own enumeration
# already worked before the fix.  The baseline, both anchors and the statement
# count move with it in the same commit.
#
# ITEM-552 added `scripts/check.d/236-item-552-diag-export.sh`: the session
# frame and its CRC, the journal's persisted session counter, ring and read-back,
# the one operator command with its SAVED and FAILED answers, the report's own
# sources through the xHCI and EHCI sinks and the build identity, the read-only
# Debian extractor with its identity/ambiguity/completeness rules, the host and
# QEMU proofs with their registration, and the confinement of the QEMU proof's
# fault injection to its own kernel variant.  The baseline, both anchors and the
# statement count move with it in the same commit.
#
# ITEM-551 added `scripts/check.d/235-item-551-ehci-usb2-storage.sh`: the EHCI
# function's PCI interface and the BAR0-only scan, the operational offsets being
# relative to CAPLENGTH, the interrupt mask staying zero and the driver never
# reaching the interrupt stack, the writable PORTSC subset and the speed policy
# that accepts an enabled port whose controller model leaves the speed field at
# zero, the descriptor encoders and the circular asynchronous list, the shared
# BOT core and block view, the existing boot-medium guard and the idempotent
# journal bind, the controller-named records, the host and QEMU proofs with
# their registration, and the documents that record the item.  The baseline,
# both anchors and the statement count move with it in the same commit.
#
# ITEM-550 added `scripts/check.d/234-item-550-xhci-command-snapshot.sh`: the
# vocabulary and the three record shapes of a command that never completed, every
# field the owner asked for (the submitted TRB and its ring slot and cycle, the
# controller registers, the event ring's consumer position and last entry, the
# events read, the unmatched completions and the last of them), the separation of
# an enqueue failure from a completion timeout, the absence of any print or
# journal write inside the wait loop, the machine-lifetime ownership of the
# captured copy, the No-Op-before-ENABLE_SLOT order that makes the `0x14.0`
# `ENUMERATION_FAILED` verdict honest, the host proofs on both halves and the
# documents that record the item.  The baseline, both anchors and the statement
# count move with it in the same commit.
#
# ITEM-549 added six more statements to the ITEM-517 slice: the two constants of
# the Stage1 register-preservation patch, the writer's call into it and the
# trampoline table it copies, and the two host-proof assertions that ECX is kept
# across both BIOS calls and that the redirects and the carry test survive.  The
# hardware finding behind it: the L875's legacy AHCI INT 13h handler does not
# preserve ECX, which Limine's Stage1 uses as its sector counter, so the stage2
# read loop overran and the machine rebooted before any output.  The baseline,
# both anchors and the statement count move with it in the same commit.
#
# ITEM-547 added six statements to the ITEM-517 slice: the two MBR constants of
# the active-entry repair (`PRODUCTION_INSTALLER_BIOS_MBR_FIRST_ENTRY_OFFSET`,
# `PRODUCTION_INSTALLER_BIOS_MBR_ACTIVE_FLAG`), the writer's rule
# (`if (!production_bios_table_has_active(lba0)) {` and the flag it sets), the
# host proof's reference recipe line and the QEMU proof's installed-LBA0
# assertion.  The current-item pins of slices 207, 208, 209, 210, 232 and 233
# follow the new item (ITEM-547) in the same change, and slice 233 pins its own
# `## Item ITEM-545` history section, which is four more statements.  The
# physical finding behind it: the L875's InsydeH2O never ran stage1 because the
# installed protective MBR carried no active entry, while every medium it does
# boot was written by `limine bios-install`, which sets partition 1 active.  The
# baseline, both anchors and the statement count move with it in the same
# commit.
# ITEM-564 moved the media writer's build-id gate into its own script, added
# slice 237 and re-pinned the historical ITEM-547 sections; the baseline, both
# anchors and the statement count move with it in the same commit.
# ITEM-565 pinned slice 237 to the historical `## Item ITEM-564` section, added
# the queue-advance rules to slice 52 and the fixture slice 238; the baseline,
# both anchors and the statement count move with it in the same commit.
# ITEM-566 extended the EHCI slice with the confirmed DMA-quiet rule: the
# CAPLENGTH accessor, the bounded ASE/ASS start-up window, the halt that makes
# the pool safe and the records that show it; the baseline, both anchors and
# the statement count move with it in the same commit.
#   * ITEM-567 moved the root port reset into `kernel/src/ehci_port.c` (so the
#     host proof drives the production sequence through the production MMIO
#     accessor), re-pinned the ITEM-551/ITEM-563 port-write and power checks to
#     the module, and added section 14: PR set with PE zero, the 50 ms hold, the
#     explicit PR-zero write, the specification's 2 ms completion sample, the
#     one re-clear of a latched PR, the PE window, the words kept on the
#     controller and the `[EHCI-PORT]` record in the report.  The baseline, both
#     anchors and the statement count move with it in the same commit.
#   * ITEM-568 moved the EHCI BIOS -> OS handoff to PCI configuration space
#     (EECP is a configuration-space byte offset, not BAR + EECP*4), added a
#     terminated periodic frame list so a stray PSE cannot fault, gave the port
#     reset a halted-controller verdict with USBCMD/USBSTS sampled at every
#     point, and registered the handoff and mask proofs.  The ITEM-556 pins for
#     the handoff follow the module; the ITEM-560 pin for the periodic list base
#     now requires the frame list instead of zero.  The baseline, both anchors
#     and the statement count move with it in the same commit.
#   * ITEM-569 gave the queue structures the 64-bit format (52-byte qTD and
#     68-byte QH with the extended buffer words), the strides that hold them,
#     a pool whose five regions are disjoint (the frame list no longer starts
#     where the control data buffer does), a qTD slot accessor, the [EHCI-DMA]
#     record with both halves of every address, and the format proof.  The
#     ITEM-560 descriptor pin now requires the slot accessor.  The baseline,
#     both anchors and the statement count move with it in the same commit.
#   * ITEM-570 fixed the hub's wPortStatus speed bits (low = bit 9, high =
#     bit 10, test = bit 11), kept the raw wPortStatus/wPortChange and the
#     verdict in the walk's port record and the [EHCI-PATH] line, and added
#     the raw-word classifier and high-speed walk proofs.  The baseline, both
#     anchors and the statement count move with it in the same commit.
#   * ITEM-572 gave the unsupported flush its own classification (exactly
#     opcode 0x35, a valid failed CSW, a completed REQUEST SENSE, a valid
#     05/20/00 sense), read the device's MODE SENSE caching page at bind (an
#     absent or unusable page is UNKNOWN, not WCE=0), made a confirmed WCE=0
#     device skip the command entirely, kept an enabled-cache refusal an error,
#     accepted the unknown-cache case only under the documented weaker
#     guarantee, and printed cache/wce/flush in the storage records.  The
#     baseline, both anchors and the statement count move with it in the same
#     commit.
#   * ITEM-573 kept what a refused xHCI controller read - every port's raw
#     PORTSC before and after the driver's preparation, its actions, the start
#     verdict, USBCMD/USBSTS, the Supported Protocol groups and each HID role's
#     identity and counters - in a durable store keyed by PCI location, took
#     that copy before the session's release zeroed it, rendered it from RAM in
#     `diag save` even when no session survived, and made the pointer verdict
#     upgrade to SEEN (naming the controller, the connected port and the
#     failure stage) when the driver could not identify the device.  The
#     baseline, both anchors and the statement count move with it in the same
#     commit.
#   * ITEM-571 separated the journal commit point's header write from its flush,
#     kept the block result of each, added the storage fault snapshot (opcode,
#     CSW status/residue and the REQUEST SENSE result with its own validity),
#     printed both through the diag refusal and the EHCI report, and made a
#     bound device clear the first candidate's controller status.  The baseline,
#     both anchors and the statement count move with it in the same commit.
#   * ITEM-574 added the Intel xHCI USB port-routing stage (the four Panther/
#     Lynx Point registers, the mask and device gates, the EHCI companion's
#     ports excluded from the switch, write read-back and restore), the
#     before/after routing snapshots and the BIOS/OS handoff result in the
#     exported file, the shared connect-window record, and both host proofs
#     (a substituted configuration space and the evidence records).  The
#     baseline, both anchors and the statement count move with it in the same
#     commit.
#   * ITEM-576 corrected the Intel routing rule to the chipset topology: the
#     xHCI at D20:F0, the boot-medium EHCI at D26:F0 (outside the routing
#     domain) and the routing EHCI at D29:F0, with four-bit 1E31 fields, the
#     masks written as the routing values, the N_PORTS guess and the root-CCS
#     check removed, a production-path host proof over the three functions and
#     an RMH regression.  The baseline, both anchors and the statement count
#     move with it in the same commit.
#   * ITEM-577 gave the controller the scratchpad buffers its HCSPARAMS2 asks
#     for (the pointer array in DCBAA[0], the pages written before USBCMD.RS),
#     laid every slot's 32-record Device Context area out at a run-time pool
#     offset, kept the first HSE/HCH snapshot before cleanup and gated every
#     command on it, exported the DMA geometry and added the production-path
#     host proof.  The baseline, both anchors and the statement count move with
#     it in the same commit.
#   * ITEM-581 made the interactive boot log paint its reading (screen plus
#     serial, never the journal) and say EMPTY/UNAVAILABLE explicitly, added
#     the one network/panel/entropy status composition shared by the console
#     `network` command, the Status menu and `diag save`, and gave the
#     entropy owner its CPUID/stage evidence and a pure CPU-profile selection.
#     The baseline, both anchors and the statement count move with it in the
#     same commit.
#   * ITEM-580 made the export take one live snapshot before composing (the
#     working fields of every live session and the pointer verdict refreshed;
#     the visit verdict, refusal reason, routing, handoff, ports, protocols,
#     DMA geometry, first fatal snapshot and released sessions' records kept)
#     and split the `[INPUT-DIAG]` role line into submitted= and completions=.
#     The baseline, both anchors and the statement count move with it in the
#     same commit.
#   * ITEM-579 gave the HID roles one physical device path (one slot, one USB
#     address, one EP0 and one configuration, with one interrupt endpoint and
#     one queue per role), made a role transition and the silent-candidate
#     retry reset the port only when no live path exists, repaired a Halted
#     endpoint with Reset Endpoint + Set TR Dequeue Pointer (or took the role
#     out of readiness), split the submitted/completed/decoded counters, and
#     carried the wheel through the pointer event.  The ITEM-448 decoder pins
#     moved to the wheel-decoding wording.  The baseline, both anchors and the
#     statement count move with it in the same commit.
#   * ITEM-592 added slice 247 and pinned the RTL8168evl cold start: the XID
#     table's row carries the confirmed generation, so the receive data-valid
#     gate and RX_EARLY_OFF are G/H/EP settings and TXCFG_AUTO_FIFO is the evl
#     one; the start runs reset, the EPHY table over EPHYAR, the clock/FIFO/
#     MCU/EEE programming over the 11-bit extended-register window, the MAC and
#     the PHY stage with named results and a first failure; the PHY stage
#     selects page 0, advertises 10/100 in register 4 and 1000BASE-T in
#     register 9, reads both back with verdicts and keeps the first error; the
#     vendor firmware dependency is named rather than imitated; and the
#     evidence records [NET-PHY-BOOT]/[NET-PHY-GIG]/[NET-PHY-SAMPLES] travel
#     with the journal and the report.  The cold-state model rejects an early
#     MAC enable, a generation-foreign register and a wrong-page PHY access.
#     The baseline, both anchors and the statement count move with it in the
#     same commit.
#   * ITEM-595 added slice 248 and pinned PHYAR reads as Linux's separate
#     completion check followed by a payload read.  Transaction.last remains
#     the completion sample, transaction.observed is the separate payload,
#     payload MMIO failure is named MMIO_READ_FAILED, the success/error settle
#     remains mandatory, and production cold-start models prove stale
#     completion data, page mismatch, payload refusal and page0 recovery.
#     The baseline, both anchors and the statement count move with it in the
#     same commit.
#   * ITEM-591 added slice 246 and pinned the PHYAR handshake's timing and the
#     advertisement read-back: the busy wait polls at the reference driver's
#     25 us interval for at most 20 polls (500 us), every completed operation
#     leaves the 20 us settle the hardware spec requires (on the failure path
#     too), the pause comes from the platform through the `delay_us` seam and
#     the production adapter measures it with the calibrated cycle counter,
#     a bus without a pause is refused, and MII register 4 is read back so the
#     record separates the written value, the answered value and the read's
#     verdict while CONFIGURED, the link state and the auto-negotiation result
#     stay separate.  The host model owns a microsecond clock and rejects a
#     command issued inside the settle window.  The baseline, both anchors and
#     the statement count move with it in the same commit.
#   * ITEM-598 added slice 251 and pinned the no-manual-hex path: the provisioning
#     tool reads only `os.getrandom()`, refuses repository output and prints
#     only the public header digest; the target/build-bound record is parsed by
#     the production owner and sealed under the ordinary administrator password
#     with PBKDF2/AES-CTR/HMAC in two durable slots; the installer asks for the
#     normal set-or-verify credential conversation and the installed boot
#     advances the epoch before publishing HOST_PROVISIONED.  Derived bytes are
#     not recredited after pool exhaustion.  A console or panel password change
#     re-seals the seed before replacing the account.  The UEFI QEMU proof pins
#     the no-hardware-RNG CPU, a password change, two installed boots, live
#     TLS/panel evidence, hidden passwords and the durable slot sequences
#     without typing `entropy seed`.  The baseline, both anchors and the
#     statement count move with it in the same commit.
#   * ITEM-598 wrong-password RCA added the staged host-seed login: account
#     verification produces a private one-use verifier without opening a gate
#     or session, the seed store is opened next, and only then may the account
#     be published before entropy.  The console reports `ACCOUNT`,
#     `ACCOUNT_PUBLISH`, `SEED` or `ENTROPY` by name, and a hidden credential
#     conversation prevents the idle local menu from painting over it.  The
#     QEMU proof exercises both an account denial and a corrupted seed HMAC,
#     requires the matching stage and proves the failed seed attempt does not
#     advance the durable sequence or start the listener.  The baseline, both
#     anchors and the statement count move with it in the same commit.
#   * ITEM-590 added slice 245 and pinned the installed journal's two
#     hardware findings: a journal that bound its reserved file and filled every
#     slot it declares is FULL, so the export asks `saveable()` (READY or FULL)
#     and recovers the room through the journal's own rewind instead of calling
#     a healthy medium missing; the failed placement keeps
#     `NO_MEDIUM`/`MEDIUM_PROBE`, a failed write is named `BEGIN_WRITE` and a
#     failed flush `FLUSH`; the medium record names the journal's own state; and
#     the `[NET-PHY]` record is opened and ended by its own emitter, whose one
#     composition both the boot journal and the exported report print, with the
#     device record finished before it starts.  The three states are proven
#     through the RTL8168 host model and read back with the operator's Debian
#     readers; the installed-system QEMU proof fills the journal before it saves
#     twice.  The baseline, both anchors and the statement count move with it in
#     the same commit.
#   * ITEM-589 added slice 244 and pinned the RTL8168 PHY access: the family
#     table row carries the access method, so the mask that accepts the
#     owner's 0x2C9 part can no longer be skipped by a second exact-XID list;
#     the E/F rows are PHYAR, the late G/H/EP rows stay OCP and the classic
#     parts keep no PHY claim; the driver records the access, the attempt, the
#     configuration result and the first error; `[NET-PHY]` is printed on skip
#     and refusal with the XID, the access method and the reason.  The
#     regression drives the raw 0x2C900000 TxConfig through the production
#     identify.  The baseline, both anchors and the statement count move with
#     it in the same commit.
#   * ITEM-588 added slice 243 and pinned the state-preserving update: a
#     loadable state is the condition, not a configured one, so a default-only
#     volume is updated instead of refused (the owner's Toshiba was refused
#     exactly that way); every outcome carries the current and planned
#     persistence ranges, the reader's verdict and the configuration/account
#     evidence, and the refusals still stop before the first write.  The QEMU
#     proof drives the four cases on a legacy-BIOS install.  The baseline, both
#     anchors and the statement count move with it in the same commit.
#   * CI-SPEEDUP-5 added slice 242 and pinned the installer proof's place in
#     the QEMU suite: the installer media became file targets with an
#     order-only `deps` (the fetch script would otherwise make every medium
#     permanently out of date), the phase's image step prepares them before the
#     queue, the standalone target keeps its build and its medium checks, and
#     the installer is the first entry of `QEMU_PARALLEL_PHASE_1` because the
#     acceptance journal named it the last target to finish.  The baseline,
#     both anchors and the statement count move with it in the same commit.
#   * CI-SPEEDUP-4 added slice 241 and pinned the credential KDF's prepared
#     HMAC key: the pads are absorbed once per derivation through
#     `sha256_absorb_block`, every round copies both states and the byte
#     counter, a long password is hashed once instead of per round, the
#     prepared states are zeroed, every PBKDF2 parameter and the record format
#     are unchanged, and the reference vectors across the block boundary, the
#     saved-record check and the compression budget are proven by the host
#     tests.  The baseline, both anchors and the statement count move with it
#     in the same commit.
#   * CI-SPEEDUP-3 added slice 240 and pinned the credential-durability boot's
#     expected refusal: the proof asks the embedded helper for the console's
#     own denied line instead of driving the whole sign-in conversation and
#     waiting out the 120-second session marker (121 s of the main HTTPS proof,
#     measured by PR #761), the answer is scoped to the attempt's serial
#     position, and the host witness that drives the stale marker, the issued
#     session, another refusal reason, a silent console and a hung-up monitor
#     is registered and run by the canonical suite.  The baseline, both anchors
#     and the statement count move with it in the same commit.
#   * ITEM-578 made the control transfer's Data Stage name the buffer its own
#     parser reads (the report descriptor is now transferred into the buffer
#     the report-descriptor walk classifies), separated a failed read from a
#     descriptor verdict, gave the pointer its own status (NOT_A_MOUSE) and its
#     own stage/reason/detail in the durable evidence, added the [XHCI-RDESC]
#     record with the read result, length and bounded bytes, forced a real port
#     reset between two role attempts on one device and on the silent-candidate
#     re-bind, made the SET_PROTOCOL fallback parse for the role it binds, and
#     stopped the pointer verdict from borrowing the controller's (keyboard's)
#     detail.  The baseline, both anchors and the statement count move with it
#     in the same commit.
#   * ITEM-614 added slice 256 and pinned the finished login trace's transfer
#     to the shared snapshot: the bounded scalar snapshot with no secret field,
#     the one fragment renderer for serial/journal/status, the NOT_ATTEMPTED
#     and IN_PROGRESS states that cannot present a stale success, the real
#     diag save/FAT12/extractor host proof, the completed record in the
#     host-provisioned QEMU snapshot, and the honest cold-boot record in the
#     installed-system export.  The baseline, both anchors and the statement
#     count move with it in the same commit.
#   * ITEM-623 made the installed root initramfs part of the boot payload set:
#     the `\boot\hfoc-rootfs.arc` path and its 1 MiB bound, the medium that
#     carries the same archive the installer booted from, the generated menu
#     that names it, and the installed-configuration module line.  The
#     payload-set bound moved from 6 to 7 entries so the sealed five, the
#     initramfs and the generated menu fit one set.  The baseline, both
#     anchors and the statement count move with it in the same commit.
#   * ITEM-615 added slice 257 and pinned the retained `diag save` refusal's
#     transfer to the shared snapshot: the bounded commit/storage receipts, the
#     one read-only `[DIAG-FAIL]` renderer, its honest NO_FAILURE/NOT_ATTEMPTED
#     state, the no-clear property, and the real save/FAT12/extractor proof.
#     The baseline, both anchors and the statement count move with it in the
#     same commit.
#   * ITEM-632 added slice 263 and pinned the native shell script language:
#     the whole-input AST parser and its named bounds, the positional and
#     command-substitution expansion, the test/trap/return/break/continue
#     builtins, the exit-time descriptor release that lets a pipe reader see
#     EOF, the RED/GREEN fixture copied into the target rootfs, and the QEMU
#     proof that compares the exact bytes, the real status and the exit-2
#     refusal for wrong syntax.  The baseline, both anchors and the statement
#     count move with it in the same commit.
#   * ITEM-633 added slice 264 and pinned job control: process groups and the
#     session-confined signal set, WAIT2's encoded status, the session-leader
#     TTY foreground gate, Ctrl-C/Ctrl-Z mapping, scheduler suspend/resume,
#     timeout expiry on the real PIT/IOAPIC tick, the shell's bounded job
#     table and jobs/fg/bg/wait, the native sleep utility and the stock-OpenSSH
#     pty proof that stops, continues, foregrounds and interrupts a real job.
#     The baseline, both anchors and the statement count move with it in the
#     same commit.
#   * ITEM-634 added slice 265 and pinned the production shell acceptance:
#     the file/program/pipeline/script/job matrix, the second owner shell
#     session, the copy/rename/delete and exit-status checks, the acceptance
#     runner and the honest OPEN persistent-workspace blocker.  The baseline,
#     both anchors and the statement count move with it in the same commit.
#   * ITEM-636 pinned the installed root: the production media declare the
#     staged native rootfs, the stage packs that tree and refuses a declared
#     root without /bin/sh, and the update proof extracts the archived root and
#     requires a real ELF /bin/sh.  The baseline, both anchors and the statement
#     count move with it in the same commit.
#   * ITEM-645 added slice 270 and pinned the runtime receive-event
#     acknowledge: the write-one-to-clear mask of the observed word, the
#     drained-ring rule that keeps RxFIFOOver|RxOverflow latched until a free
#     descriptor is proven, the all-ones dead-bus refusal, the adapter's
#     overflow/recovery history in [NET-RX] and both host proofs.  The
#     baseline, both anchors and the statement count move with it in the same
#     commit.
#   * ITEM-646 added slice 271 and pinned the negative update phase's freeze:
#     the marker waiter's --stop-pid/SIGSTOP, the 10 ms poll, the refused
#     phase passing the emulator pid, the SIGCONT that lets a stopped QEMU
#     flush and exit, the no-next-boot assertion, the whole-file SHA-256
#     fingerprint and the host regression's frozen-process case.  The
#     baseline, both anchors and the statement count move with it in the same
#     commit.
#   * ITEM-647 added slice 272 and pinned the localized form labels: the shared
#     confirmation term and hint, the field-name keys, the hidden fixed
#     actions, the separate route remove form, the nesting/language audit and
#     the old remove body's MALFORMED refusal.  The baseline, both anchors and
#     the statement count move with it in the same commit.
#   * the anchors below pin the stream of origin/main 962953d:scripts/check.sh
#     (23,332 lines, 1,022,223 bytes, sha256 ecd9b355...) and the committed
#     baseline copy of the current stream;
#   * a statement that is deleted, weakened, duplicated or reordered changes the
#     stream, so the anchors fail;
#   * the negative controls at the end re-run the extraction on deliberately
#     mutated copies of the tree and fail if a deletion, a weakening or a
#     reorder is NOT detected: the proof is shown to be able to fail.
#
# ITEM-659 adds slice 275 (`scripts/check.d/275-item-659-not-found-redirection.sh`),
# which pins the not-found path that performs its redirections, publishes
# status 127 and is read back from the production QEMU shell proof.  The
# baseline, both anchors and the statement count move with it in the same
# commit.
#
# An item that intentionally changes the enforced contract set must regenerate
# the baseline and move both anchors in the same commit, so the change is
# visible in review:
#
#   scripts/check-contract-stream.sh scripts/check.sh > scripts/check-contracts.baseline.txt
#
# ITEM-662 extends the installed-boot slice with the boot-menu labels: both
# `boot/hfoc-installed.conf` and `boot/limine.conf` must name the release in
# `VERSION`. The baseline, both anchors and the statement count move with it
# in the same commit.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

readonly EXTRACTOR=scripts/check-contract-stream.sh
readonly CHECK=scripts/check.sh
readonly BASELINE=scripts/check-contracts.baseline.txt

# Anchors. PRE_SPLIT_* describe the origin of the baseline; BASELINE_* pin the
# stream and the file, so neither the tree nor the baseline can drift silently.
readonly PRE_SPLIT_COMMIT=962953db703d419d5e16bb23236dccbd5337bf1d
readonly PRE_SPLIT_SHA256=ecd9b35506577ff3ae16385de95150fb2169339cca075f1954bbbc4809372cfd
readonly BASELINE_STREAM_SHA256=9e7363cb60550e53705bf9e65006bb79aaeca15b11a0c1d2cd59b579fdea42af
readonly BASELINE_FILE_SHA256=9e7363cb60550e53705bf9e65006bb79aaeca15b11a0c1d2cd59b579fdea42af
readonly EXPECTED_STATEMENTS=61545

fail() {
    echo "check contracts: $*" >&2
    exit 1
}

for required in "$EXTRACTOR" "$CHECK" "$BASELINE"; do
    [[ -f $required ]] || fail "missing $required"
done

file_sha=$(sha256sum "$BASELINE" | awk '{print $1}')
[[ $file_sha == "$BASELINE_FILE_SHA256" ]] || fail \
    "the frozen baseline was modified (sha256 $file_sha); regenerate it from ${PRE_SPLIT_COMMIT}:scripts/check.sh and move the anchors"

scratch=$(mktemp -d "${TMPDIR:-/tmp}/hfoc-contracts.XXXXXX")
trap 'rm -rf "$scratch"' EXIT

stream_sha256() {
    sha256sum "$1" | awk '{print $1}'
}

# The contract stream of the tree as it stands.
bash "$EXTRACTOR" "$CHECK" >"$scratch/stream.txt" ||
    fail 'cannot extract the contract stream (module list or structure is broken)'
statements=$(wc -l <"$scratch/stream.txt")
actual_sha=$(stream_sha256 "$scratch/stream.txt")
if [[ $actual_sha != "$BASELINE_STREAM_SHA256" ]]; then
    echo 'check contracts: the enforced statement stream changed' >&2
    echo "  expected (${PRE_SPLIT_COMMIT:0:12}:scripts/check.sh): $BASELINE_STREAM_SHA256" >&2
    echo "  actual   (split tree):                             $actual_sha" >&2
    diff -u "$BASELINE" "$scratch/stream.txt" | head -n 80 >&2 || true
    exit 1
fi
[[ $statements -eq $EXPECTED_STATEMENTS ]] || fail \
    "stream has $statements statements, expected $EXPECTED_STATEMENTS"

# Where the pre-split script is available, verify the frozen anchor against it.
if pre_split_sha=$(git show "${PRE_SPLIT_COMMIT}:scripts/check.sh" 2>/dev/null | sha256sum | awk '{print $1}') &&
    [[ -n $pre_split_sha ]]; then
    [[ $pre_split_sha == "$PRE_SPLIT_SHA256" ]] || fail \
        "pre-split ${PRE_SPLIT_COMMIT}:scripts/check.sh has sha256 $pre_split_sha, expected $PRE_SPLIT_SHA256"
    pre_split_note="origin verified against ${PRE_SPLIT_COMMIT:0:12}"
else
    pre_split_note="pre-split blob not present in this checkout; anchors stand on their own"
fi

# ---------------------------------------------------------------- negatives --
# Mutate a copy of the tree and require the extraction to notice. The target is
# derived from the pinned list, so the controls survive a rename of any module.
first_module=$(awk '
    /^CHECK_MODULES=\(/ { inside = 1; next }
    inside && /^\)/ { exit }
    inside { gsub(/[[:space:]]/, ""); if (length($0)) { print; exit } }
' "$CHECK")
[[ -n $first_module ]] || fail 'cannot read the first pinned module from the entry point'
first_module_path=scripts/check.d/$first_module
[[ -f $first_module_path ]] || fail "first pinned module is missing: $first_module_path"
statement_line=$(awk '
    /^[[:space:]]*#/ { next }
    /^[[:space:]]*$/ { next }
    { print NR; exit }
' "$first_module_path")
[[ -n $statement_line ]] || fail "no statement found in $first_module_path"

copy_tree() {
    local dir=$1
    mkdir -p "$dir/scripts"
    cp "$CHECK" "$dir/scripts/check.sh"
    cp -r scripts/check.d "$dir/scripts/check.d"
}

expect_detected() {
    local dir=$1
    local label=$2
    local mutated_sha
    if ! bash "$EXTRACTOR" "$dir/scripts/check.sh" >"$dir/stream.txt" 2>/dev/null; then
        echo "  negative control OK: $label (extraction refuses the mutated tree)"
        return 0
    fi
    mutated_sha=$(stream_sha256 "$dir/stream.txt")
    if [[ $mutated_sha == "$BASELINE_STREAM_SHA256" ]]; then
        fail "negative control FAILED: $label was not detected"
    fi
    echo "  negative control OK: $label"
}

# 1. a deleted statement
copy_tree "$scratch/deleted"
sed -i "${statement_line}d" "$scratch/deleted/scripts/check.d/$first_module"
expect_detected "$scratch/deleted" "deleted statement (${first_module}:${statement_line})"

# 2. a weakened early exit
copy_tree "$scratch/weakened"
sed -i '0,/exit 1/s//exit 0/' "$scratch/weakened/scripts/check.d/$first_module"
expect_detected "$scratch/weakened" "weakened guard exit 1 -> exit 0 (${first_module})"

# 3. two adjacent statements swapped
copy_tree "$scratch/reordered"
awk -v n="$statement_line" '
    NR == n { held = $0; next }
    NR == n + 1 { print; print held; next }
    { print }
' "$scratch/reordered/scripts/check.d/$first_module" >"$scratch/reordered/module.tmp"
mv "$scratch/reordered/module.tmp" "$scratch/reordered/scripts/check.d/$first_module"
expect_detected "$scratch/reordered" "reordered statements (${first_module}:${statement_line})"

echo "check contracts: OK ($statements enforced statements match the frozen baseline)"
echo "  $pre_split_note"
