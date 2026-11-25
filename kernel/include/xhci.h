#ifndef HFOC_XHCI_H
#define HFOC_XHCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "keyboard.h"
#include "usb_hid.h"
#include "xhci_trb.h"

struct block_device;

/*
 * ITEM-395: xHCI (USB 3) host controller bring-up and USB HID boot-protocol
 * keyboard support for the local console.
 *
 * The appliance boots on machines with no usable PS/2 path, and until now
 * console input existed only on the i8042 path (ITEM-392 made its absence
 * survivable but not replaceable). This driver enumerates every xHCI controller
 * the machine has, brings up the HID boot-protocol keyboard it finds on any of
 * their root ports and feeds the console the same events the PS/2 driver
 * produces; ITEM-448 added the pointer beside it, and ITEM-452 made the walk
 * visit every controller and every port even after both are bound, so a
 * keyboard and a mouse on two different controllers work at the same time and
 * every attached device names itself.
 *
 * Scope and shape, and why:
 *   * Polled, not interrupt-driven. The controller's interrupt path would need
 *     APIC/IOAPIC/IRQ-routing work, which is frozen scope, so the transfer
 *     completion is collected from the event ring when the ordinary-boot event
 *     loop services the driver (kernel/src/kernel.c), exactly like the other
 *     production poll paths. Nothing is masked, routed or enabled here.
 *   * Every controller and every root port, in PCI order (ITEM-406), with no
 *     early exit at all (ITEM-452). The acceptance host has three xHCI
 *     controllers: the keyboard is on one of them and the owner's Logitech
 *     receiver on another, and a controller the driver does not open cannot
 *     name itself. A controller whose devices are not the ones the machine
 *     needs costs a bounded number of attach attempts and is still published.
 *   * One live session per bound role (ITEM-452). A controller that binds the
 *     keyboard or the pointer keeps its own DMA pool, rings, event ring and
 *     controller running for the rest of the boot; the walk's current
 *     controller uses a third scratch session whose pool is reused. There are
 *     XHCI_SESSION_SLOTS sessions, each with its own pool claimed lazily from
 *     the physical page allocator (pmm_alloc_contiguous_pages), never the heap;
 *     a machine whose contiguous pages run out is refused with
 *     XHCI_STATUS_DMA_UNAVAILABLE and keeps booting.
 *   * The speed the port reports is used as it is (ITEM-406). A low-speed
 *     USB 1.1 device is addressed with its 8-byte control pipe, the interrupt
 *     endpoint context carries the descriptor's own wMaxPacketSize (8 for a
 *     boot keyboard, never an assumed 64) and the Interval field the speed
 *     implies, and a PORTSC read-modify-write never writes back PED, which
 *     would disable the very port it just reset.
 *   * Bounded everywhere. Every controller handshake, port reset, command and
 *     transfer has a bounded spin budget. A controller, port or device that
 *     never answers degrades to a published UNAVAILABLE reason; it cannot hang
 *     the boot.
 *   * Published per controller and per device (ITEM-452). Each visit emits one
 *     `[XHCI]` record naming the controller's PCI address, how many controllers
 *     the machine has and whether this one was taken; every port with a
 *     connected device emits its own `[USB ]` record with the port, the speed,
 *     the role it was bound as or refused for, the reason and - once its
 *     descriptors were read - its vid/pid and interface list.
 *
 * References used for the bring-up sequence and the register semantics:
 *   * xHCI specification revision 1.1: sections 4.2 (initialization), 4.3
 *     (power management / port reset), 4.6.6 (Configure Endpoint), 4.8 (slot
 *     and device context), 5.4.8 and table 5-27 (PORTSC), 6.2.3.6 and table 6-8
 *     (endpoint interval) and 6.4 (TRB/event definitions).
 *   * Linux drivers/usb/host/xhci.c, xhci-ring.c, xhci-mem.c for the bring-up
 *     ordering (halt-and-reset, DCBAAP, CRCR, ERST/ERDP, RS) and the
 *     Address Device / Configure Endpoint input-context shape, and xhci-hub.c
 *     (xhci_port_state_to_neutral, XHCI_PORT_RW1CS) for which PORTSC bits a
 *     read-modify-write may write: PED and the change bits are write-one-to-
 *     clear, so a read value must never be written back whole.
 *   * USB 2.0 specification section 5.5.3 (default control pipe size per speed)
 *     and section 9.6 (descriptor layout) for the low-speed path.
 *   * QEMU hw/usb/hcd-xhci.c as the behavioural reference the QEMU proof runs
 *     against: the controller model implements the same register set and
 *     command/transfer ring protocol - and it ignores writes to PED, which is
 *     why that defect can only be seen on a real controller.
 */

/* Why the USB HID input path is or is not usable. Every value is a reason an
   operator can read; only XHCI_STATUS_OK means a keyboard feeds the console. */
enum xhci_status {
    XHCI_STATUS_OK = 0,
    /* No PCI function with class 0x0C:0x03:0x30 in this machine. */
    XHCI_STATUS_CONTROLLER_ABSENT,
    /* The function exists but exposes no usable memory BAR. */
    XHCI_STATUS_NO_MEMORY_BAR,
    /* The register window could not be mapped. */
    XHCI_STATUS_MMIO_MAP_FAILED,
    /* No contiguous physical pages for the DMA structures. */
    XHCI_STATUS_DMA_UNAVAILABLE,
    /* The controller never left the halted state. */
    XHCI_STATUS_HALT_TIMEOUT,
    /* HCRST never completed, or CNR never cleared. */
    XHCI_STATUS_RESET_TIMEOUT,
    /* The controller started but never completed a No-Op command. */
    XHCI_STATUS_COMMAND_FAILED,
    /* The controller is up and no port has a connected device. */
    XHCI_STATUS_NO_DEVICE,
    /* A device is connected at a port speed this driver does not address (an
       unknown value of PORTSC's speed field; USB 3 SuperSpeed is addressable
       since ITEM-438), so it cannot be enumerated. */
    XHCI_STATUS_UNSUPPORTED_SPEED,
    /* Slot enable, address device or a control transfer failed. */
    XHCI_STATUS_ENUMERATION_FAILED,
    /* A device enumerated but does not offer a HID boot-protocol keyboard
       interface with an interrupt IN endpoint. */
    XHCI_STATUS_NOT_A_KEYBOARD,
    /* ITEM-578: the pointer's walk reached the same verdict - the descriptor
       or the interface walk names no HID mouse.  The two roles carry two
       statuses so a pointer refusal is never published as a keyboard one. */
    XHCI_STATUS_NOT_A_MOUSE,
    /* ITEM-452: devices were seen on this controller but not enumerated,
       because the machine already holds the role they would have been offered
       for.  The devices are still published, each with its own `[USB ]` line
       and this reason. */
    XHCI_STATUS_ROLE_TAKEN,
    /* The keyboard was ready and then its port or transfers failed. */
    XHCI_STATUS_KEYBOARD_LOST,
    XHCI_STATUS_UNKNOWN
};

/*
 * ITEM-406: the controller and port scan policy of the keyboard bring-up.
 *
 * The acceptance host has three xHCI controllers: the keyboard is on one of
 * them and the owner's wireless receiver on another, and the receiver's
 * pointer must be bound while the keyboard keeps working (ITEM-452). "First
 * controller, first device" therefore cannot be the rule. This is the pure
 * half of that rule, so the host proof can drive the machine shapes the
 * acceptance host has (a controller with a non-keyboard device, an empty
 * controller, the keyboard and the pointer on different controllers) without
 * hardware:
 *
 *   * the scan visits every controller and, inside one, every root port, and
 *     nothing it finds ends that walk (ITEM-452): a keyboard that is bound
 *     makes the machine-wide verdict OK, and the controllers behind it are
 *     still visited and still publish their own records;
 *   * one controller may spend a bounded number of device-attach attempts, so
 *     a controller full of other devices cannot cost unbounded enumeration
 *     work;
 *   * when the machine has no usable keyboard, what gets published is the most
 *     informative rejection - a device that answered and was described says
 *     more than a controller with nothing attached - and the first controller
 *     wins a tie, so the reason is deterministic.
 *
 * The struct owns the storage for the published detail and the policy copies
 * into it whenever an outcome wins: the detail a caller passes is usually the
 * driver's working buffer, which the very next port probe overwrites, so a kept
 * pointer would silently start naming a different port.
 */
#define XHCI_SCAN_DETAIL_BYTES 64U

struct xhci_scan {
    /* Controllers the machine has (from the PCI scan). */
    uint32_t controllers_total;
    /* Controllers whose bring-up was attempted so far. */
    uint32_t controllers_seen;
    /* Device-attach attempts spent on the current controller. */
    uint32_t attempts;
    /* Attempts one controller may spend (XHCI_MAX_ATTACH_ATTEMPTS). */
    uint32_t attempt_limit;
    /* A usable boot keyboard was addressed: the machine has local input.  The
       walk does not stop here (ITEM-452); this is the machine-wide verdict. */
    bool selected;
    /* The verdict published when nothing was selected, and the specific
       detail behind it. Never NULL after xhci_scan_note_controller(). */
    enum xhci_status status;
    const char *detail;
    /* Storage `detail` points into. */
    char detail_storage[XHCI_SCAN_DETAIL_BYTES];
};

/* Begin a scan over `controllers_total` controllers, each of which may spend
   at most `attempt_limit` device-attach attempts. `controllers_total` may be
   0: a machine without a controller publishes CONTROLLER_ABSENT. */
void xhci_scan_init(
    struct xhci_scan *scan,
    uint32_t controllers_total,
    uint32_t attempt_limit);

/* Start (or restart) the per-controller part of the scan: the attempt budget
   belongs to one controller, so a controller full of devices that are not
   keyboards cannot starve the ones behind it. */
void xhci_scan_begin_controller(struct xhci_scan *scan);

/* Spend one attach attempt of the current controller's budget. False when the
   budget is exhausted; the caller then stops probing that controller and the
   machine-wide scan goes on with the next one. */
bool xhci_scan_take_attempt(struct xhci_scan *scan);

/* Record one controller's outcome. `selected` is true only when a usable boot
   keyboard was bound there, which makes the machine-wide verdict OK. Returns
   true when a keyboard was found on this controller; ITEM-452: the machine-wide
   walk continues either way, so the caller never stops on this value. */
bool xhci_scan_note_controller(
    struct xhci_scan *scan,
    enum xhci_status status,
    const char *detail,
    bool selected);

/* How much a status says about the machine: deeper progress ranks higher, so
   the published reason names the most informative rejection. */
uint32_t xhci_scan_progress(enum xhci_status status);

/* True when `candidate` is a more informative rejection than `current`. A tie
   keeps `current`, so the earliest controller's evidence survives. */
bool xhci_scan_prefers(
    enum xhci_status candidate,
    enum xhci_status current);

/*
 * ITEM-395 + ITEM-406 + ITEM-452: bring up the xHCI controllers this machine
 * has and the HID devices behind them. Every controller is visited in PCI
 * order and every root port of each is probed; the walk does not stop at a
 * bound device, so the keyboard and the pointer may live on two different
 * controllers and both stay live. Each visit publishes its controller record
 * and each connected device its own record, so a device that is refused names
 * itself and the reason. Called once from the boot path; safe (and honest) on
 * a machine without a controller: it returns XHCI_STATUS_CONTROLLER_ABSENT and
 * publishes the reason, and the boot continues.
 */
enum xhci_status xhci_keyboard_init(uint64_t hhdm_offset);

/*
 * Service the controllers: collect completed events, decode finished reports
 * into console events and keep one interrupt IN transfer in flight per bound
 * device.  ITEM-452: every live session is serviced round-robin, each with its
 * own event ring and counters, so a keyboard on one controller and a pointer on
 * another both make progress.  Called from the ordinary-boot event loop; a
 * no-op until a device is ready.
 */
void xhci_keyboard_service(void);

/* Pop one decoded console event, false when none is pending. */
bool xhci_keyboard_pop_event(struct keyboard_event *event);

/* True once a boot-protocol keyboard is attached and its first transfer is
   in flight. */
bool xhci_keyboard_attached(void);

enum xhci_status xhci_keyboard_status(void);
const char *xhci_status_string(enum xhci_status status);
/* The specific evidence behind the status: the descriptor decode reason, the
   xHCI completion code, or "OK". Never NULL. */
const char *xhci_keyboard_detail(void);
const char *xhci_keyboard_transport_string(void);

/* Published detail for the boot record and the console. Zero when no keyboard
   is attached. */
uint8_t xhci_keyboard_speed(void);
uint16_t xhci_keyboard_vendor_id(void);
uint16_t xhci_keyboard_product_id(void);
uint8_t xhci_keyboard_port(void);
uint32_t xhci_keyboard_report_count(void);
uint64_t xhci_keyboard_transfer_count(void);
/* ITEM-579: submitted and completed are separate counters. */
uint64_t xhci_keyboard_completion_count(void);
uint32_t xhci_keyboard_error_count(void);
/* ITEM-458: the reports the bounded queue refused, and the longest wait
   between two reports the service pass delivered. */
uint32_t xhci_keyboard_dropped_count(void);
uint32_t xhci_keyboard_gap_ms_max(void);
/* ITEM-461: the longest wait between a completed report and the next interrupt
   transfer being armed, in milliseconds. */
uint32_t xhci_keyboard_repost_ms_max(void);
/* ITEM-467: the keyboard's in-flight transfer, its re-arm count and the time
   since its last completion, so a keyboard that never answered is not read as
   one nobody touched. */
bool xhci_keyboard_pending(void);
uint32_t xhci_keyboard_repost_count(void);
uint32_t xhci_keyboard_last_report_ms(void);
/* ITEM-469: the depth of the keyboard's armed-transfer queue and the missed
   service completions it has seen, so the durable input record can require
   `kbd_armed>=2` and `kbd_mse=0`. */
uint32_t xhci_keyboard_armed_count(void);
uint32_t xhci_keyboard_mse_count(void);
/* ITEM-470: the windows a keyboard/pointer completion left with no armed TRB. */
uint32_t xhci_keyboard_no_trb_count(void);
/*
 * ITEM-479 (СРОЧНО-28, round 17): the stalled-endpoint watchdog's findings for
 * each role - how many silence episodes were published and recovered, and the
 * longest wait that triggered one, in milliseconds.  They ride in the input
 * tail record beside the queue facts, so the next hardware run reads whether a
 * pointer that stopped completing was recovered inside the run.
 */
uint32_t xhci_keyboard_watchdog_count(void);
uint32_t xhci_keyboard_watchdog_silence_ms_max(void);
uint32_t xhci_mouse_watchdog_count(void);
uint32_t xhci_mouse_watchdog_silence_ms_max(void);
/*
 * ITEM-486 (round 24): the watchdog published its one recovery for the current
 * silence and will not try again while the endpoint stays armed - the
 * `WATCHDOG_GAVE_UP` outcome of the mouse event path.
 */
bool xhci_mouse_watchdog_gave_up(void);
/*
 * ITEM-482 (round 20): the watchdog's why and the keyboard's window.
 *
 * `xhci_keyboard_gap_window_ms_max()` is the longest wait between two keyboard
 * reports over the current window; `xhci_keyboard_gap_window_reset()` closes
 * the window, and the once-a-second input record calls both so `kbd_gap_ms_max`
 * describes the last second instead of the whole boot.  `xhci_*_report_rate()`
 * is the device's reports per second over the last window.  The mouse's
 * `active_count` is how many silences began while the pointer was being moved,
 * and `escalations` how many of those the pure repair rule wanted reset.
 */
uint32_t xhci_keyboard_gap_window_ms_max(void);
void xhci_keyboard_gap_window_reset(void);
uint32_t xhci_keyboard_report_rate(void);
uint32_t xhci_mouse_report_rate(void);
uint32_t xhci_mouse_watchdog_active_count(void);
uint32_t xhci_mouse_watchdog_escalations(void);
uint32_t xhci_mouse_watchdog_recover_us_max(void);

/*
 * ITEM-436: the binding the driver selected (the interface, its descriptor
 * protocol, the interrupt IN endpoint address, the interval the endpoint
 * context was programmed with and the packet size it declares) and the two
 * traffic counters that name a keyboard which is bound but not answering:
 * completed transfers that did not carry a whole 8-byte report, and the byte
 * count of the last completed transfer. The binding readers answer zero while
 * no keyboard is ready; the counters answer the driver's own totals.
 *
 * ITEM-438 adds the two halves of the binding that were missing: the DCI the
 * reports arrive on, the descriptor's own bInterval (the number the programmed
 * Interval field is the exponent of, xHCI 1.2 section 6.2.3.6), and the keys
 * the whole reports carried - so a keyboard that answered with an idle report
 * and one that never answered at all are different records.
 */
uint8_t xhci_keyboard_interface(void);
uint8_t xhci_keyboard_protocol(void);
uint8_t xhci_keyboard_endpoint_address(void);
uint8_t xhci_keyboard_dci(void);
uint8_t xhci_keyboard_endpoint_interval(void);
uint8_t xhci_keyboard_endpoint_binterval(void);
uint16_t xhci_keyboard_endpoint_max_packet(void);
uint32_t xhci_keyboard_refused_count(void);
uint32_t xhci_keyboard_last_report_bytes(void);
uint32_t xhci_keyboard_key_count(void);

/*
 * ITEM-438: the report-protocol binding.  A device whose interface descriptor
 * did not declare the boot keyboard protocol is bound only after its own HID
 * report descriptor named a keyboard layout, and its reports are converted
 * into the boot report the decoder already covers.  `mode` is REPORT exactly
 * then; the layout readers publish where the modifier byte and the key array
 * were found, and `idle` is 1 when the device refused SET_IDLE with a
 * duration - the request that makes an untouched keyboard prove it is alive.
 */
bool xhci_keyboard_report_mode(void);
uint8_t xhci_keyboard_report_id(void);
uint8_t xhci_keyboard_report_length(void);
uint8_t xhci_keyboard_key_offset(void);
uint8_t xhci_keyboard_key_slots(void);
bool xhci_keyboard_idle_refused(void);

/*
 * ITEM-438: how the walk ended.  `bound` counts devices that parsed as a
 * keyboard and were bound, `silent` those that then never answered inside their
 * window, `proven` those that did; `retry` is true when the binding that
 * stands is the first silent candidate re-bound after every controller was
 * visited - the state a machine with one keyboard that ignores SET_IDLE ends
 * in, and the difference between that and a keyboard that answered at once.
 */
uint32_t xhci_keyboard_candidates_bound(void);
uint32_t xhci_keyboard_candidates_silent(void);
uint32_t xhci_keyboard_candidates_proven(void);
bool xhci_keyboard_silent_retry(void);

/* Controller facts, published whether or not a keyboard was found. */
const char *xhci_controller_state_string(void);
uint32_t xhci_controller_max_ports(void);
uint32_t xhci_controller_max_slots(void);
uint64_t xhci_controller_bar(void);
uint16_t xhci_controller_vendor_id(void);
uint16_t xhci_controller_device_id(void);

/*
 * ITEM-448: the pointer, bound next to the keyboard.
 *
 * The driver holds up to two HID devices at once - a keyboard and a mouse -
 * with their own slots, endpoints, transfer rings, report buffers and
 * counters; one is no longer mistaken for the other, and the second no longer
 * displaces the first.  ITEM-452 gives each controller its own session, so a
 * pointer bound on a different controller than the keyboard is *kept*: both
 * controllers stay up and both devices are serviced.  The `seen`/`released`
 * readers below are what remains of the older shape - a pointer the machine
 * could not keep - and they answer zero while every binding is held; the
 * [MOUSE] record keeps printing them so its fields do not move.
 *
 * `xhci_mouse_pop_event()` hands the console one decoded movement or button
 * edge at a time; where the pointer is and what a click hits stays with the
 * console, exactly as the character a key produces stays with the keyboard
 * decoder.  The readers below answer the same binding and traffic facts the
 * keyboard's readers answer, so the [MOUSE] record carries the evidence the
 * [KBD ] record carries, including the silence a bound pointer may show when
 * nobody has moved it.
 */
bool xhci_mouse_pop_event(struct usb_hid_mouse_event *event);
bool xhci_mouse_attached(void);
uint8_t xhci_mouse_speed(void);
uint8_t xhci_mouse_port(void);
uint16_t xhci_mouse_vendor_id(void);
uint16_t xhci_mouse_product_id(void);
uint8_t xhci_mouse_interface(void);
uint8_t xhci_mouse_protocol(void);
uint8_t xhci_mouse_endpoint_address(void);
uint8_t xhci_mouse_dci(void);
uint8_t xhci_mouse_endpoint_interval(void);
uint8_t xhci_mouse_endpoint_binterval(void);
uint16_t xhci_mouse_endpoint_max_packet(void);
bool xhci_mouse_report_mode(void);
uint32_t xhci_mouse_report_count(void);
uint32_t xhci_mouse_transfer_count(void);
/* ITEM-579: transfers the controller completed and reports that carried a
   wheel movement, beside the submitted count above. */
uint32_t xhci_mouse_completion_count(void);
uint32_t xhci_mouse_wheel_count(void);
uint32_t xhci_mouse_endpoint_restarts(void);
uint32_t xhci_mouse_error_count(void);
uint32_t xhci_mouse_refused_count(void);
uint32_t xhci_mouse_last_report_bytes(void);
uint32_t xhci_mouse_move_count(void);
uint32_t xhci_mouse_press_count(void);
uint32_t xhci_mouse_release_count(void);
/* ITEM-467: the pointer's in-flight transfer, its re-arm count, the time since
   its last completion, and the one-word reason it has delivered no report. */
bool xhci_mouse_pending(void);
uint32_t xhci_mouse_repost_count(void);
uint32_t xhci_mouse_last_report_ms(void);
const char *xhci_mouse_no_report_reason(void);
/* ITEM-469: the pointer's queue depth and missed service count. */
uint32_t xhci_mouse_armed_count(void);
uint32_t xhci_mouse_mse_count(void);
/* ITEM-470: the unarmed windows the sixteen-deep pointer queue must keep at 0. */
uint32_t xhci_mouse_no_trb_count(void);
/*
 * ITEM-473 (СРОЧНО-23): the run-time mouse arm depth the console's `ARM`
 * command sets and shows, and the controller-level Event Ring Full (21) and
 * Host Controller Event counts the input record publishes as `ringfull=` and
 * the QEMU proof requires to stay zero at the sixteen-deep queue.
 */
uint32_t xhci_mouse_arm_depth(void);
void xhci_set_mouse_arm_depth(uint32_t requested);
uint32_t xhci_event_ring_full_count(void);
uint32_t xhci_controller_event_count(void);
/* The mouse role's verdict when no pointer is bound, the port/stage detail
   behind it, and the identity and interface list of the device it refused. */
const char *xhci_mouse_status_string(void);
const char *xhci_mouse_detail(void);
bool xhci_mouse_device_seen(void);
const char *xhci_mouse_interface_summary(void);
uint16_t xhci_mouse_refused_vendor_id(void);
uint16_t xhci_mouse_refused_product_id(void);
/* A pointer was bound on a controller the machine could not keep (the state
   ITEM-452 removes for the controller-per-session case): the record names the
   controller, the port and the speed instead of reporting no mouse.  Answers
   false while every bound pointer is held, which is the normal state. */
bool xhci_mouse_seen(void);
uint8_t xhci_mouse_seen_port(void);
uint8_t xhci_mouse_seen_speed(void);
const char *xhci_mouse_seen_controller(void);
uint32_t xhci_mouse_candidates_bound(void);
uint32_t xhci_mouse_candidates_released(void);

/*
 * ITEM-468 (СРОЧНО-18): the machine tells its own story about the controllers
 * and the input path, without a debugger and without guessing.
 *
 * The owner's 2026-09-21 hardware run published the input record exactly once
 * (it is written "on change"), so the only evidence was a single snapshot at
 * tick 10; the controller's `usbcmd`/`usbsts` were published only when it was
 * bound, so a link that degraded later left no trace.  This is a tool, not a
 * behaviour change: at most once a second the ordinary-boot loop asks the
 * driver to publish one short journal record per live controller, one per HID
 * role and one for the loop turn; `XHCI` and `INPUT` print the same facts on
 * demand, through the console transcript ITEM-460 already writes to the
 * durable journal.  Nothing here binds, arms, re-arms or reorders anything.
 */
void xhci_service_diagnostics(uint64_t frames);
/*
 * ITEM-519 increment 3: the write half of the mass-storage path, proven on the
 * medium itself rather than promised.  The proof reads the last sector of the
 * bound stick, writes a deterministic pattern over it, synchronizes the cache,
 * reads the pattern back, restores the original sector and reads that back too,
 * and publishes one `[USB-STORAGE] state=WRITE_PROOF` record naming every
 * verdict.  The medium is left as it was found.  False means no stick is bound,
 * the declared geometry cannot carry the proof, or a step failed - the record
 * names which step, and no failure is reported as success.
 *
 * ITEM-519 increment 4: the proof drives the medium through the stick's
 * `block_device`, and that view is what the driver publishes for the rest of
 * the kernel.
 */
bool xhci_storage_write_proof(void);
/*
 * ITEM-519 increment 4: the `block_device` of the bound USB stick, or NULL
 * when no stick is bound or its block view was refused.  The journal of
 * ITEM-521 reads, writes and flushes through this handle; it is valid for the
 * boot because the driver holds the session that owns the backend.
 */
struct block_device *xhci_storage_block_device(void);
/*
 * ITEM-519 increment 5: read the bound stick's LBA 0 through its block view and
 * publish one `[INSTALL-MEDIUM]` record answering whether it is the medium the
 * machine booted from; True means MATCH.  The probe never writes.  A device
 * with no block view, a loader with no identity, or a read that fails is
 * UNAVAILABLE with the step named - never a silent MATCH.
 */
bool xhci_storage_medium_publish(void);
/*
 * The name of the last medium probe's result ("MATCH", "NO_MATCH", ...), or
 * "UNCHECKED" before the first probe or with no device at all.
 */
const char *xhci_storage_medium_result(void);
/*
 * ITEM-541: the medium verdict and the reason that decided it, both borrowed
 * and never NULL, for the refusal page.  "UNAVAILABLE reason=NO_DEVICE" is a
 * machine with no storage session at all; "UNCHECKED reason=NOT_PROBED" is a
 * session that exists and was never asked - two states that used to read as the
 * same `UNCHECKED`.  True only for MATCH.
 */
bool xhci_storage_medium_status(const char **state, const char **reason);
/* ITEM-571: the bound device's last failed command, as one bounded line. */
bool xhci_storage_fault_text(char *buffer, size_t size);
/*
 * ITEM-497: publish the boot's input verdict now, at the end of boot and
 * before the ordinary loop starts.  The verdict answers "why did this boot come
 * up without input", so it belongs to the boot's own first flush - a short boot
 * that is switched off (or, in QEMU, killed) before the loop's second flush
 * would otherwise leave the disk with no verdict at all.  The loop keeps
 * publishing it on every state change; this is the boot's first copy.
 */
void xhci_input_verdict_publish_now(void);
void xhci_diagnostics_print_xhci(void);
/*
 * ITEM-552: the same answers, to a report instead of the screen.  Every line
 * the `xhci` and `input` commands would print - the controllers', the roles',
 * the loop's and the kept command failure's - is handed to `sink` one text line
 * at a time; the console is not touched.  `diag save` installs its own sink for
 * the duration of the call.
 */
typedef void (*xhci_report_sink_fn)(void *context, const char *text);
void xhci_diagnostics_report(xhci_report_sink_fn sink, void *context);
void xhci_diagnostics_print_input(void);

#endif
