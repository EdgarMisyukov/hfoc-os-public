#include "xhci.h"

#include "appliance_boot_log_store.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/timer.h"
#include "console.h"
#include "install_medium.h"
#include "install_journal_platform.h"
#include "kstring.h"
#include "pci.h"
#include "pmm.h"
#include "serial.h"
#include "usb_hid.h"
#include "usb_storage.h"
#include "vmm.h"
#include "xhci_evidence.h"
#include "xhci_route.h"
#include "xhci_trb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* xHCI is a serial-bus controller: class 0x0C, subclass 0x03, prog-if 0x30. */
#define XHCI_PCI_CLASS_SERIAL_BUS 0x0CU
#define XHCI_PCI_SUBCLASS_USB 0x03U
#define XHCI_PCI_PROGIF_XHCI 0x30U

#define XHCI_PCI_CONFIG_ADDRESS 0xCF8U
#define XHCI_PCI_CONFIG_DATA 0xCFCU
#define XHCI_PCI_CONFIG_ENABLE 0x80000000U
#define XHCI_PCI_COMMAND 0x04U
#define XHCI_PCI_COMMAND_MEMORY 0x0002U
#define XHCI_PCI_COMMAND_BUS_MASTER 0x0004U

/* One fixed pool for every structure the controller reads or writes: the
   command ring, the event ring, the event ring segment table, the device
   context table, the input context, one transfer ring pair per HID device and
   the transfer buffers, plus the device context base address array whose size
   follows the controller (ITEM-407). Claimed once for the whole machine
   (pmm_alloc_contiguous_pages), never the heap.

   ITEM-448: the pool carries two devices now - a keyboard and a mouse - so the
   endpoint-0 ring, the interrupt ring and the transfer buffers exist twice. The
   controller-level structures (command ring, event ring, ERST, device contexts,
   input context) stay single: one controller is up at a time and its command
   and event paths are shared by every slot it owns. */
#define XHCI_DMA_FIXED_BYTES 0x10000U
#define XHCI_OFF_COMMAND_RING 0x1000U
#define XHCI_OFF_EVENT_RING 0x2000U
#define XHCI_OFF_ERST 0x3000U
#define XHCI_OFF_INPUT_CONTEXT 0x5000U
/* One ring page per device: [0] the keyboard, [1] the mouse. */
#define XHCI_OFF_DEVICE_EP0_RING 0x6000U
#define XHCI_OFF_DEVICE_INTERRUPT_RING 0x8000U
#define XHCI_OFF_DEVICE_DATA 0xA000U
/*
 * ITEM-519 increment 2: the mass-storage device's own rings and its bounce
 * page.  The DCBAA keeps its place at XHCI_DMA_FIXED_BYTES (at most 0x800
 * bytes for the widest controller), so storage starts on the next free page
 * boundary and ends before the pool does.
 */
#define XHCI_OFF_STORAGE_EP0_RING 0x12000U
#define XHCI_OFF_STORAGE_BULK_IN_RING 0x13000U
#define XHCI_OFF_STORAGE_BULK_OUT_RING 0x14000U
#define XHCI_OFF_STORAGE_DATA 0x15000U
#define XHCI_STORAGE_BULK_TRBS 256U
/*
 * ITEM-470 (СРОЧНО-20): the data region is three pages per role now, because
 * the pointer's queue is sixteen transfers deep (see below).  The keyboard's
 * one-page shape is unchanged bar the stride; the mouse's queue occupies the
 * last two pages of its region.
 */
#define XHCI_DEVICE_DATA_STRIDE 0x3000U
/* Inside one device's data region. */
#define XHCI_DATA_DESCRIPTOR 0x0000U
#define XHCI_DATA_DESCRIPTOR_MAX 512U
/* ITEM-438: the HID report descriptor a report-protocol interface is asked
   for, next to the report buffer and inside the same data region. */
#define XHCI_DATA_REPORT_DESCRIPTOR 0x0400U
/*
 * ITEM-469 (СРОЧНО-19): the interrupt-transfer queue.
 *
 * Until this item the driver kept exactly one interrupt transfer in flight per
 * role and re-armed it at the end of the service pass, so between a completion
 * and the next PIT tick (up to 10 ms at 100 Hz) the endpoint had no TRB at all
 * and a report that arrived in that window was dropped by the device - the
 * owner's fast typing stopped echoing while a slow, >10 ms cadence worked.
 *
 * Each role now holds transfers in the ring at once, each with its own report
 * buffer, and a completion is followed by an immediate re-arm inside the same
 * event handler.  The buffers live above the report descriptor: slot `n` is
 * XHCI_DATA_REPORT_QUEUE + n * XHCI_REPORT_SLOT_BYTES.
 *
 * ITEM-470 (СРОЧНО-20): the owner's pointer runs at binterval 1 (1 ms) and the
 * keyboard's old depth of three left the mouse short of one loop turn, so the
 * pointer NAKed bursts the controller then dropped silently (`mouse_mse=0`
 * cannot see a device's own NAK).  The pool is sixteen 512-byte slots, the
 * keyboard arms the same three it always did (`XHCI_ARM_DEPTH`), and the mouse
 * arms sixteen (`XHCI_MOUSE_ARM_DEPTH`) - a full turn's worth at 1 ms, which
 * the deeper interrupt ring backs.  The cost is one extra page per role in the
 * DMA pool; the `no_trb` counter below makes any window that is still unarmed
 * a number instead of a silent loss.
 */
#define XHCI_DATA_REPORT_QUEUE 0x0600U
#define XHCI_REPORT_SLOT_BYTES 512U
#define XHCI_REPORT_SLOTS 16U
#define XHCI_ARM_DEPTH 3U
#define XHCI_MOUSE_ARM_DEPTH 16U
/*
 * ITEM-479 (СРОЧНО-28, round 17): how long a role may hold armed transfers with
 * no completion before the watchdog publishes and re-arms it.  The owner's run
 * showed the pointer silent for the last 95 s with `armed=16` and the endpoint
 * Running; one second is far past the 1 ms-10 ms endpoints these devices use,
 * and short enough that a stalled pointer is recovered inside the run.
 */
#define XHCI_WATCHDOG_SILENCE_MS 1000U
/*
 * ITEM-482 (round 20): how recent a report has to be for a silence to count as
 * "the device froze while it was in use" rather than "nobody touched it".  Two
 * seconds is the item's window and comfortably covers the 1 ms mouse endpoint.
 */
#define XHCI_WATCHDOG_ACTIVE_MS 2000U
/*
 * ITEM-470: sixteen outstanding interrupt transfers do not fit the sixteen-TRB
 * ring the keyboard used (one slot must always separate producer and
 * consumer), so the HID interrupt rings are four times deeper.  The ring still
 * lives inside its own 4 KiB page, and the command and endpoint-0 rings keep
 * the original XHCI_RING_TRBS.
 */
#define XHCI_INTERRUPT_RING_TRBS 64U
/* xHCI 1.1 table 6-90 spans completion codes 1..35; the array is indexed by the
   raw code so every event the controller raises gets counted, not only the two
   the record publishes. */
#define XHCI_COMPLETION_CODE_COUNT 36U
/* Missed Service Error (23) and Event Ring Full Error (21) in table 6-90. */
#define XHCI_COMPLETION_MSE 23U
#define XHCI_COMPLETION_EVENT_RING_FULL 21U

/*
 * ITEM-577: one DMA pool, sized at run time from what the controller and the
 * slots this driver enables actually need.
 *
 * The fixed regions (rings, the input context, the per-role data, the storage
 * bounce page and the DCBAA) keep their offsets.  Every region whose size
 * follows the controller lives after `XHCI_DMA_FIXED_END`, in this order:
 *
 *   device context slot areas   slots_enabled x (32 contexts)
 *   scratchpad pointer array    scratchpad_buffers x 8 bytes
 *   scratchpad pages            scratchpad_buffers x the controller page size
 *
 * A slot's Device Context is 32 context records (the slot context plus the 31
 * endpoint contexts, xHCI 1.1 section 6.2.1); `context_bytes` is the size of
 * one record, not of the slot.  The input context is 33 records and stays in
 * its fixed page (XHCI_INPUT_CONTEXT_MAX_BYTES is the largest it can be).
 * The pool is allocated with those sizes, never with a fixed page count, so a
 * controller that needs no scratchpads and a controller that needs many get
 * the same disjoint layout.
 *
 * The static assertions below pin the fixed regions; the run-time layout is
 * checked by xhci_dma_setup() before a single register is published.
 */
#define XHCI_DMA_FIXED_BYTES 0x10000U
#define XHCI_DMA_FIXED_END (XHCI_OFF_STORAGE_DATA + 0x1000U)
/* A pool larger than this is a misread HCSPARAMS2, not a machine to cover:
   the widest legal controller (255 slots, 64-byte contexts, 1023 scratchpad
   pages) stays far below it. */
#define XHCI_DMA_MAX_BYTES (4U * 1024U * 1024U)

_Static_assert(
    XHCI_DMA_FIXED_BYTES + (size_t)(XHCI_MAX_SLOTS + 1U) * sizeof(uint64_t) <=
        XHCI_OFF_STORAGE_EP0_RING,
    "the DCBAA must stay below the storage rings");
_Static_assert((size_t)XHCI_INPUT_CONTEXT_MAX_BYTES <= 4096U,
               "the input context must stay inside its fixed page");
_Static_assert(XHCI_DMA_FIXED_END <= XHCI_DMA_MAX_BYTES,
               "the fixed regions must fit the pool bound");
/* ITEM-470: the per-role data region must hold the deepest report queue, and
   the interrupt ring must stay inside its own page. */
_Static_assert(
    (size_t)XHCI_DATA_REPORT_QUEUE +
            (size_t)XHCI_REPORT_SLOTS * XHCI_REPORT_SLOT_BYTES <=
        XHCI_DEVICE_DATA_STRIDE,
    "one role's report queue must stay inside its data region");
_Static_assert(
    (size_t)XHCI_INTERRUPT_RING_TRBS * XHCI_TRB_BYTES <= 4096U,
    "the interrupt ring must stay inside its page");
_Static_assert(
    XHCI_MOUSE_ARM_DEPTH < XHCI_INTERRUPT_RING_TRBS,
    "the mouse queue must leave the interrupt ring room to move");
_Static_assert(
    XHCI_PORT_EVIDENCE_BYTES > XHCI_PORT_EVIDENCE_MAX * 10U,
    "the port evidence buffer must hold every value and its separator");
/*
 * ITEM-473 (СРОЧНО-23): the event ring is its whole page, and every queue that
 * feeds it must stay below its capacity.  The previous ring held sixteen TRBs -
 * the mouse's own armed depth - so one burst of completions filled it, the
 * controller raised Event Ring Full (21) and, per the specification, stopped
 * publishing events until the software moved the dequeue pointer; the round-11
 * run read that as a pointer that went silent with `armed=16` still set.  The
 * static assertions make the page fit and the depth relation compile-time
 * facts instead of arithmetic a reader has to reproduce.
 */
_Static_assert(
    (size_t)XHCI_EVENT_TRBS * XHCI_TRB_BYTES <=
        XHCI_OFF_ERST - XHCI_OFF_EVENT_RING,
    "the event ring must stay inside its page");
_Static_assert(
    XHCI_MOUSE_ARM_DEPTH < XHCI_EVENT_TRBS,
    "the mouse queue must leave the event ring room");
_Static_assert(
    XHCI_REPORT_SLOTS < XHCI_EVENT_TRBS,
    "the report pool must leave the event ring room");
_Static_assert(
    XHCI_ARM_DEPTH < XHCI_EVENT_TRBS,
    "the keyboard queue must leave the event ring room");
_Static_assert(
    XHCI_ARM_DEPTH_MAX == XHCI_MOUSE_ARM_DEPTH,
    "the run-time depth's maximum must be the compiled mouse depth");

/* Register window: the BAR is at least 4 KiB and covers the capability,
   operational, runtime and doorbell register spaces inside 64 KiB. */
#define XHCI_MMIO_MAX_PAGES 16U
#define XHCI_MMIO_MIN_BYTES 0x1000U

/* Bounded spin budgets. The PIT is 100 Hz and interrupts are still masked
   while the controller comes up, so every wait is a pause loop with a hard
   iteration bound rather than a time source; the loops leave as soon as the
   condition they wait for is true. */
#define XHCI_SPIN_COMMAND 4000000U
#define XHCI_SPIN_TRANSFER 4000000U
#define XHCI_SPIN_RESET 20000000U
#define XHCI_SPIN_HALT 20000000U
#define XHCI_SPIN_PORT_RESET 5000000U
#define XHCI_SPIN_LEGACY 2000000U

/* Bounds on the work one service call and one bring-up may do. */
#define XHCI_EVENT_DRAIN_MAX 64U
/*
 * ITEM-458: one service pass drains the session's event ring, so these are the
 * budgets that keep a busy controller from starving the rest of the machine
 * rather than the "one report per pass" cap ITEM-448 introduced.  Sixteen TRBs
 * are the ring; 32 reports and 64 ring events leave room for a wrapped ring
 * and for a report repeated across control transfers, and the round-robin in
 * xhci_keyboard_service() still gives the other session its turn next.
 */
#define XHCI_SERVICE_REPORTS_MAX 32U
#define XHCI_SERVICE_EVENTS_MAX 64U
#define XHCI_MAX_ATTACH_ATTEMPTS 8U
#define XHCI_MAX_CONSECUTIVE_ERRORS 8U
#define XHCI_LOCAL_EVENTS 32U

/*
 * ITEM-406: how many xHCI functions the machine may have before the driver
 * stops looking. The PCI scan fills this array in bus/device/function order and
 * the driver visits every entry, which is what puts the acceptance host's
 * keyboard (second controller) in reach; eight is far above the three
 * controllers that machine has, and a machine with more is reported honestly
 * instead of being silently reduced to its first controller.
 *
 * ITEM-452: the walk visits every one of them and does not stop at the first
 * bound device, so a controller that holds a needed role keeps its session -
 * and its DMA pool, its rings and its event ring - alive for the rest of the
 * boot. Two roles can be live at once (the keyboard and the pointer, which the
 * owner's machine has on two different controllers) and the walk needs one
 * more session for the controller it is currently surveying: that is the three
 * slots below. A visit that binds nothing releases its slot.
 */
#define XHCI_MAX_CONTROLLERS 8U
/* ITEM-519 increment 2: a third role is now persistent - the keyboard, the
   pointer, the mass-storage device - so the walk needs a fourth session as
   its scratch. */
#define XHCI_SESSION_SLOTS 4U

/* Room for the specific rejection detail published in the boot record:
   "CTRL_05:00.3" / "P2_SET_PROTOCOL_STALL" and their stages fit with room to
   spare, and a longer one is truncated rather than overflowing. */
#define XHCI_DETAIL_BYTES 80U

/* A controller is halted before the driver leaves it for the next one, so it
   stops writing the shared DMA pool. Bounded like every other wait. */
#define XHCI_SPIN_LEAVE 2000000U

/* USBLEGSUP capability id and bits, xHCI 1.1 section 7.1. */
#define XHCI_EXT_CAP_USBLEGSUP 1U
#define XHCI_USBLEGSUP_BIOS_OWNED 0x00010000U
#define XHCI_USBLEGSUP_OS_OWNED 0x01000000U
#define XHCI_EXT_CAP_NEXT_SHIFT 8U

/*
 * ITEM-448: the two HID devices this driver holds at the same time.
 *
 * Until this item the controller state *was* the keyboard state: one slot, one
 * interrupt ring, one report buffer, one set of counters.  The owner's contract
 * is that a keyboard and a mouse work together - a mouse (or the mouse half of
 * a wireless receiver) must no longer be either mistaken for the keyboard or
 * skipped, and the second device must not displace the first - so everything
 * that describes one bound device lives in this struct and the controller state
 * carries two of them, indexed by role.
 *
 * The two are independent in every respect that matters: their own slot, port,
 * endpoint, DCI, report layout, transfer ring, report buffer, counters and
 * decoded-event queues.  Only the controller-level structures (command ring,
 * event ring, device context table, input context) are shared, which is
 * correct because one controller is up at a time and its command and event
 * paths are per-controller, not per-device.
 */
#define XHCI_HID_ROLE_KEYBOARD 0U
#define XHCI_HID_ROLE_MOUSE 1U
#define XHCI_HID_DEVICES 2U

/* One device's decoded events waiting for the console. Bounded like the
   keyboard's queue: a console that is not draining loses the newest event
   instead of overwriting an unread one. */
#define XHCI_LOCAL_MOUSE_EVENTS 16U

struct xhci_hid_device {
    uint8_t role;
    bool ready;
    uint8_t slot_id;
    uint8_t port;
    uint8_t speed;
    uint8_t dci;
    uint8_t interface_number;
    /* ITEM-436: the rest of the binding the record publishes, so an operator
       reads which interface of a multi-interface device was selected, on
       which endpoint, with which descriptor values. */
    uint8_t interface_protocol;
    uint8_t endpoint_address;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t endpoint_max_packet;
    /* ITEM-438: the descriptor's own bInterval next to the Interval field the
       endpoint context was programmed with (xHCI 1.2 section 6.2.3.6). */
    uint8_t endpoint_binterval;
    uint8_t endpoint_interval;
    bool transfer_pending;
    uint32_t report_count;
    uint32_t transfer_count;
    uint32_t error_count;
    /* ITEM-436: completed transfers that did not carry a whole report, and the
       byte count of the last completion. */
    uint32_t refused_report_count;
    uint32_t last_report_bytes;
    /* ITEM-438: how many keys the decoded reports produced. */
    uint32_t key_count;
    /*
     * ITEM-458: what the bounded queue had to refuse, and the longest wait
     * between two reports this device's service pass delivered.  A count of
     * zero is the proof that no character was lost; anything else is a number
     * a reader can act on instead of a silent gap in the echo.
     */
    uint32_t dropped_report_count;
    uint32_t service_gap_ms_max;
    uint64_t service_last_ticks;
    /*
     * ITEM-461: the completion tick of the last transfer and the longest wait
     * between a completion and the next interrupt transfer being armed.  The
     * re-arm already happens at the end of the service pass, before the loop
     * paints anything; this is the number that proves it, and it is published
     * as `repost_ms_max=` beside the report counters.
     */
    uint32_t repost_ms_max;
    uint64_t transfer_complete_ticks;
    /*
     * ITEM-467: how many completions were followed by a re-arm (`reposted`)
     * and the uptime of the last completion, so a device with no report reads
     * as "the interrupt transfer is still pending" or "the transfer completed
     * and carried nothing" instead of one silence.
     */
    uint32_t repost_count;
    uint64_t last_report_ticks;
    /*
     * ITEM-468 (СРОЧНО-18): the diagnostics read on a machine whose pointer is
     * silent - how many completions arrived as a short packet (the ISP
     * completion a three-byte boot-mouse answer produces), and whether the
     * transfer currently armed asked for one.
     */
    uint32_t short_packet_count;
    bool last_transfer_isp;
    /*
     * ITEM-469 (СРОЧНО-19): the queue of interrupt transfers this role holds in
     * the ring.  `armed_slots` is a FIFO of the report-buffer slots in arming
     * order, so a completion pops the oldest buffer and the record's `armed=`
     * is how many TRBs stood between the device and a lost report.  Every
     * completion code is counted; `mse_count` (23) and `event_ring_full_count`
     * (21) are the two the owner asked to see separately - a missed service is
     * the exact failure the single-TRB window produced.
     */
    uint8_t armed_slots[XHCI_REPORT_SLOTS];
    uint8_t armed_head;
    uint8_t armed_count;
    uint32_t mse_count;
    uint32_t event_ring_full_count;
    uint32_t completion_code_counts[XHCI_COMPLETION_CODE_COUNT];
    /* ITEM-473 (СРОЧНО-23): the last completion code this role saw, so the
       per-code counts have a "most recent" reading beside them. */
    uint8_t last_completion_code;
    /*
     * ITEM-470 (СРОЧНО-20): how many times a completion emptied the queue
     * before the in-handler top-up ran - the exact window in which the device
     * had no TRB to hand a report to.  Zero is the proof the mouse depth covers
     * a loop turn; a rising number is the device's silent NAK loss made
     * visible, which `mse_count` cannot see because a missed service is the
     * controller's own state.
     */
    uint32_t no_trb_count;
    /*
     * ITEM-479 (СРОЧНО-28, round 17): the stalled-endpoint watchdog.  Progress
     * is the tick of the last completion or the last fresh arm; `reported`
     * makes one silence episode cost one record and one recovery rather than a
     * stream, and `count`/`silence_ms_max` are what the input tail publishes.
     */
    uint64_t watchdog_progress_ticks;
    uint32_t watchdog_count;
    uint32_t watchdog_silence_ms_max;
    bool watchdog_reported;
    /*
     * ITEM-482 (round 20): why the silence happened and what the recovery
     * cost.  `active_count` counts the silences that began while the device was
     * in use, `escalations` the ones the pure repair rule said a plain re-arm
     * had already failed to fix, and `recover_us_max` the worst re-arm cost.
     * The rate window turns the report counter into reports per second, and
     * `service_gap_window_ms_max` is the keyboard's gap over the current
     * one-second window rather than the whole boot.
     */
    uint32_t watchdog_active_count;
    uint32_t watchdog_escalations;
    uint32_t watchdog_recover_us_max;
    uint64_t rate_window_ticks;
    uint32_t rate_window_reports;
    uint32_t rate_per_second;
    uint32_t service_gap_window_ms_max;
    uint64_t service_gap_window_ticks;
    /*
     * ITEM-459: the usages this keyboard pressed that produced no character,
     * one bit each, so a held key is one finding rather than one per report.
     */
    uint64_t unmapped_seen[4];
    uint32_t consecutive_errors;
    /* ITEM-438: the refused device's own identity, so a refusal record names
       the device and the interfaces it declared. */
    bool device_seen;
    char interface_summary[USB_HID_INTERFACE_SUMMARY_BYTES];
    /*
     * ITEM-578: the role's own attempt.  A refusal that never reached the
     * device's identity still names the stage that stopped it, so the pointer's
     * record cannot inherit the keyboard's reason, and a report-descriptor
     * transfer that failed at the wire is published as a failed read instead of
     * a classification verdict.
     */
    bool attempt_made;
    enum xhci_status attempt_status;
    const char *attempt_stage;
    char attempt_detail[XHCI_DETAIL_BYTES];
    /*
     * ITEM-579: the physical device path this role is attached through.  A
     * composite device's roles share one path (one slot, one USB address, one
     * EP0 and one configuration) and differ only in their interface, their
     * interrupt endpoint and their queue.
     */
    struct xhci_device_path *path;
    /* The three counters a stalled pointer needs kept apart: TRBs submitted to
       the interrupt endpoint, transfer events the controller completed, and
       reports the decoder accepted. */
    uint32_t completion_count;
    uint32_t halted_count;
    uint32_t endpoint_restarts;
    /* The endpoint state as the completion path last read it (1 Running,
       2 Halted, ...), so a record can name the state and not only the code. */
    uint8_t endpoint_state;
    /* The report descriptor the read returned: result, length and the bounded
       copy of the bytes the parser classified. */
    uint8_t report_descriptor_read;
    uint8_t report_descriptor_parse;
    uint16_t report_descriptor_length;
    uint8_t report_descriptor_bytes[XHCI_EVIDENCE_DESCRIPTOR_BYTES];
    /* ITEM-438: the report-protocol keyboard layout; ITEM-448: the pointer's
       own layout for a mouse bound through its report descriptor. */
    struct usb_hid_report_layout layout;
    struct usb_hid_mouse_layout mouse_layout;
    bool report_mode;
    uint16_t report_length;
    bool idle_refused;
    /* ITEM-448: the two decoders, each with its own state and counters. */
    struct usb_hid_state hid;
    struct usb_hid_mouse_state mouse;
    struct keyboard_event events[XHCI_LOCAL_EVENTS];
    uint32_t event_head;
    uint32_t event_tail;
    struct usb_hid_mouse_event mouse_events[XHCI_LOCAL_MOUSE_EVENTS];
    uint32_t mouse_event_head;
    uint32_t mouse_event_tail;
};

/*
 * ITEM-519 increment 2: one USB mass-storage device.
 *
 * The BOT/SCSI core keeps its own command buffers; the controller owns the
 * rings and the bounce page.  `session` is where the callbacks find the
 * registers, rings and event ring they must use - a runtime caller (the
 * journal of ITEM-521) must not depend on the walk's global cursor pointing
 * at the right controller.
 */
struct xhci_storage_device {
    struct usb_storage usb;
    struct xhci_session *session;
    bool bound;
    uint8_t slot_id;
    uint8_t port;
    uint8_t speed;
    uint8_t interface_number;
    uint8_t bulk_in_dci;
    uint8_t bulk_out_dci;
    /* ITEM-545: the bursts the endpoint companions declared, kept so the bound
       record can name what the Configure Endpoint context carried. */
    uint8_t bulk_in_max_burst;
    uint8_t bulk_out_max_burst;
    uint16_t vendor_id;
    uint16_t product_id;
    uint32_t transfer_error_count;
    /* The Input Control Context as it was built when Configure Endpoint was
       refused, so the record names what the controller rejected instead of
       only the completion code. */
    uint32_t configure_diag[4];
    bool configure_diag_valid;
    struct xhci_ring ep0_ring;
    struct xhci_ring bulk_in_ring;
    struct xhci_ring bulk_out_ring;
    uint8_t sector0[4];
    bool sector0_valid;
    /*
     * ITEM-519 increment 4: the same bound stick addressed through the block
     * layer, so the journal of ITEM-521 (and anything else that owns a
     * `struct block_device`) can carry its bytes without knowing xHCI, BOT or
     * SCSI.  `block_bound` is true only when the adapter accepted the device;
     * otherwise `block_status` names the refusal, and no reader ever gets a
     * view the device never granted.
     */
    struct usb_storage_block_device block;
    bool block_bound;
    enum block_device_status block_status;
    /*
     * ITEM-519 increment 5: whether this device is the medium the machine
     * booted from, and the verdict's own name so the console answers with the
     * last probe's result instead of guessing.  `medium_probed` separates "not
     * checked yet" from "checked and not the medium".
     */
    enum install_medium_result medium_result;
    /* ITEM-541: the name the probe itself used, so the refusal page answers
       with the verdict's reason instead of re-deriving one. */
    const char *medium_reason;
    bool medium_probed;
};

struct xhci_session;

/*
 * ITEM-452: one live controller session.
 *
 * Until this item the driver held one anonymous singleton for the whole
 * machine and re-zeroed its single DMA pool for every controller it opened, so
 * a binding could not survive a move to the next controller: the keyboard on
 * the first controller and the pointer on the second were mutually exclusive
 * by construction, and the walk stopped at the first controller that had the
 * keyboard anyway.  Everything that describes *one* controller - its
 * registers, its rings, its event ring, its own DMA pool, its two HID role
 * devices and the record it publishes - now lives in a session, the machine
 * holds a small array of them, and `xhci` points at the session being worked
 * on, so every existing `xhci->field` reference names the current session and
 * nothing else.
 *
 * The pool is claimed lazily and stays with the slot for the machine's
 * lifetime: a visit that binds a needed role keeps the session live and its
 * controller running, a visit that binds nothing releases the session and the
 * next visit re-zeroes and reuses that slot's pool (pmm_alloc_contiguous_pages,
 * never the heap).  At most two pools are live at once - the keyboard's and
 * the pointer's - plus the one scratch slot the walk is using; that is the
 * bound the machine pays for holding two controllers at the same time.
 */
/*
 * ITEM-579: one physical USB device, not one role.
 *
 * The driver used to bring up each HID role as an independent device: its own
 * slot, its own Address Device, its own SET_CONFIGURATION and its own EP0.
 * That works for two devices on two ports, and it is wrong for one composite
 * device with a pointer interface and a keyboard interface: the second role
 * re-addressed a device the first role was using, and the silent-candidate
 * retry reset the port out from under a live pointer (the 10231684 report:
 * the pointer's endpoint Halted after a USB transaction error, `reports=0`).
 *
 * A path is the physical device: one slot, one USB address, one EP0 ring and
 * one configuration.  Roles attach to it by interface, each with its own
 * interrupt endpoint (DCI) and its own transfer queue; `roles` is the bitmask
 * of roles currently attached.  A path with no roles is released, and only
 * then may a new attach reset the port and build a new one.
 */
struct xhci_device_path {
    bool valid;
    bool configured;
    uint8_t port;
    uint8_t speed;
    uint8_t slot_id;
    uint8_t config_value;
    /* The highest device context index configured on this slot: the slot
       context's Context Entries field, so adding a second role's endpoint does
       not drop the first one. */
    uint8_t max_dci;
    /* The roles attached through this path: bit 0 keyboard, bit 1 mouse. */
    uint8_t roles;
    bool device_seen;
    uint16_t vendor_id;
    uint16_t product_id;
    struct xhci_ring ep0_ring;
    uint16_t config_length;
    uint8_t config[USB_HID_CONFIG_DESCRIPTOR_MAX];
    char interface_summary[USB_HID_INTERFACE_SUMMARY_BYTES];
};

struct xhci_session {
    /* The slot is describing a controller: between acquire and release. */
    bool in_use;
    /* The controller the record on the wire describes. */
    struct pci_location location;
    struct pci_device_header header;
    uint16_t pci_vendor;
    uint16_t pci_device;
    uint64_t bar_physical;
    uint64_t bar_length;
    /* ITEM-453: the bytes of the register window that were actually mapped,
       which is the BAR size clamped to what this driver maps.  The extended
       capability walks are bounded by it: a controller whose capability
       pointer is bogus would otherwise be read past the end of its own window
       and fault the boot. */
    uint32_t mmio_bytes;
    volatile uint8_t *regs;
    uint8_t caplength;
    uint8_t hci_version_major;
    uint32_t max_ports;
    uint32_t max_slots;
    uint32_t page_size;
    uint32_t db_offset;
    uint32_t rt_offset;
    size_t context_bytes;
    bool context_size_64;
    uint64_t dma_physical;
    uint8_t *dma;
    struct xhci_ring command_ring;
    struct xhci_trb *event_ring;
    uint64_t event_ring_physical;
    uint32_t event_index;
    uint8_t event_cycle;
    /*
     * ITEM-473 (СРОЧНО-23): the controller-level events the session's event
     * ring carried - every Host Controller Event TRB, and separately the Event
     * Ring Full (21) ones.  They belong to the controller, not to a slot, so
     * they live on the session; the previous build could not decode them at
     * all and the one error that explained the silent pointer was invisible.
     */
    uint32_t controller_event_count;
    uint32_t event_ring_full_events;
    uint64_t *dcbaa;
    uint64_t dcbaa_physical;
    /* Offset of the DCBAA inside the fixed pool, claimed at the end so a
       controller's own MaxSlots decides its size (ITEM-407). */
    uint32_t dcbaa_offset;
    uint32_t dcbaa_bytes;
    /* Slots this driver enables on the controller (CONFIG.MaxSlotsEn), which
       is what the device context table and its DCBAA entries are sized for. */
    uint32_t slots_enabled;
    /* ITEM-407: the evidence a controller publishes with its record when its
       register window is mapped - the operational command and status registers
       as the scan left them, the PORTSC values of the first
       XHCI_PORT_EVIDENCE_MAX ports, which of those had PORTSC.PP set, and how
       many ports were powered off before the scan claimed them. */
    uint32_t usbcmd;
    uint32_t usbsts;
    uint32_t portsc[XHCI_PORT_EVIDENCE_MAX];
    uint32_t portsc_powered_mask;
    uint32_t portsc_ports;
    uint32_t portsc_powered_off;
    /* ITEM-573: every port's raw PORTSC before and after the driver's own
       preparation, and what was done to it, kept for the durable copy that
       survives this session's release. */
    uint32_t ports_seen;
    uint32_t port_evidence_count;
    struct xhci_evidence_port port_evidence[XHCI_EVIDENCE_PORTS];
    /* ITEM-574: the Intel USB port-routing registers as this visit found and
       left them, the routing stage's verdict and the BIOS/OS handoff result,
       all kept in the durable evidence so the export can answer what the
       firmware left and what the OS changed. */
    struct xhci_evidence_route route_before;
    struct xhci_evidence_route route_after;
    char route_outcome[XHCI_EVIDENCE_STATUS];
    char route_detail[XHCI_EVIDENCE_DETAIL];
    uint32_t route_target_usb2;
    uint32_t route_target_usb3;
    uint32_t route_usb2_field;
    uint8_t route_wrote_usb2;
    uint8_t route_wrote_usb3;
    uint8_t route_restored;
    char legacy[XHCI_EVIDENCE_DETAIL];
    uint32_t settle_ms;
    uint32_t settle_bound_ms;
    uint8_t settle_pending;
    /* ITEM-577: the DMA geometry the controller asked for and the first fatal
       controller snapshot, captured before any cleanup can overwrite it. */
    uint32_t hcs_params2;
    uint32_t scratchpad_count;
    uint32_t scratchpad_page_bytes;
    uint32_t slot_area_bytes;
    uint32_t device_contexts_offset;
    uint32_t device_contexts_bytes;
    uint32_t scratchpad_array_offset;
    uint32_t scratchpad_array_bytes;
    uint32_t scratchpad_pages_offset;
    uint32_t dma_pages;
    uint32_t pool_bytes;
    uint64_t scratchpad_array_physical;
    uint64_t dcbaa0;
    bool fatal_valid;
    char fatal[XHCI_EVIDENCE_DETAIL];
    char fatal_cause[XHCI_EVIDENCE_STATUS];
    /* ITEM-453: the protocol groups the controller declares in its Supported
       Protocol capabilities (xHCI 1.1 section 7.2.1) and how many of them were
       read.  They are what maps a raw port number to the USB2 or the USB3 half
       of the controller, and they are read once, before the reset. */
    struct xhci_protocol_group protocols[XHCI_PROTOCOL_GROUPS_MAX];
    uint32_t protocol_count;
    uint64_t *erst;
    uint64_t erst_physical;
    uint8_t *device_contexts;
    uint64_t device_contexts_physical;
    uint8_t *input_context;
    uint64_t input_context_physical;
    /* One endpoint-0 ring and one interrupt ring per role (ITEM-448). */
    struct xhci_ring ep0_rings[XHCI_HID_DEVICES];
    struct xhci_ring interrupt_rings[XHCI_HID_DEVICES];
    /* ITEM-579: the physical paths of this controller.  At most one device per
       role, and a composite device is one path carrying both roles. */
    struct xhci_device_path paths[XHCI_HID_DEVICES];
    /* The two HID devices this driver binds at the same time: [0] keyboard,
       [1] mouse. */
    struct xhci_hid_device devices[XHCI_HID_DEVICES];
    /* ITEM-519 increment 2: the mass-storage device this session carries. */
    struct xhci_storage_device storage;
    /* The session's own published verdict - the [XHCI] record this controller
       emits names it, and it is never another controller's. */
    enum xhci_status status;
    const char *detail;
    /* The detail being composed (rewritten by every probe). */
    char detail_buffer[XHCI_DETAIL_BYTES];
    /* A composed detail that outlives the probe that produced it. */
    char kept_detail[XHCI_DETAIL_BYTES];
    const char *state;
};

/*
 * ITEM-452: the machine-wide half of the driver - how many controllers the
 * machine has and how many the walk visited, which session holds the keyboard
 * and which holds the pointer, the walk's own counters, and the verdict the
 * console records publish when a role is missing.  The sessions outlive a
 * visit; this state outlives the walk.
 */
struct xhci_machine {
    bool initialized;
    uint64_t hhdm_offset;
    /* Controllers the PCI scan found. */
    uint32_t controllers_total;
    enum xhci_status verdict_status;
    const char *verdict_detail;
    char verdict_detail_storage[XHCI_DETAIL_BYTES];
    /* Sessions are zeroed on release. Keep plain probe data for an XHCI
       command issued after every controller was refused or released. */
    struct {
        bool valid;
        struct pci_location location;
        enum xhci_status status;
        uint8_t caplength;
        uint32_t mmio_bytes;
        char detail[XHCI_DETAIL_BYTES];
    } last_probe;
    /*
     * ITEM-550: the last command that did not complete, captured inside the
     * wait that gave up on it.  It lives here, in the machine's RAM, and not in
     * a session: a probe that fails this way is usually followed by the release
     * of the session that owns the controller, and the `xhci` command has to be
     * able to print the failure after that release.
     */
    struct xhci_command_snapshot command;
    /* The session each role is bound in, machine-wide, or NULL. */
    struct xhci_session *keyboard_session;
    struct xhci_session *mouse_session;
    /* ITEM-519 increment 2: the session that carries the mass-storage
       device, so the walk keeps it live and never re-zeroes its pool. */
    struct xhci_session *storage_session;
    /* ITEM-582: the last survey session, kept alive when it bound nothing so a
       stick plugged in after boot can be enumerated without re-opening every
       controller (the boot survey releases sessions that hold no role). */
    struct xhci_session *survey_session;
    uint32_t candidates_bound;
    uint32_t candidates_silent;
    uint32_t candidates_proven;
    uint32_t mouse_candidates_bound;
    uint32_t storage_candidates_bound;
    uint32_t mouse_candidates_released;
    bool silent_retry;
    /*
     * ITEM-438: the walk does not stop at the first device that *parses* as a
     * keyboard.  A candidate is bound, then given a bounded window to prove it
     * answers (SET_IDLE with a duration makes a live keyboard re-send its
     * report once a second); a candidate that stays silent past that window is
     * released, published as `reason=NO_REPORT`, and the walk continues with
     * the ports and controllers it had not reached.  The first silent
     * candidate is remembered, so a machine whose only keyboard simply ignored
     * the idle request still ends the walk with that keyboard bound.
     */
    struct {
        struct pci_device_header header;
        uint8_t port;
        uint8_t speed;
        bool valid;
    } silent_candidate;
    /* ITEM-448: how the mouse role ended on the machine - the last device that
       was reached and refused, and whether a pointer was bound on a controller
       the machine could not keep (ITEM-452: with a session per controller it
       keeps it, so this is the degraded case). */
    enum usb_hid_status mouse_status;
    char mouse_detail[XHCI_DETAIL_BYTES];
    bool mouse_device_seen;
    char mouse_interface_summary[USB_HID_INTERFACE_SUMMARY_BYTES];
    uint16_t mouse_vendor_id;
    uint16_t mouse_product_id;
    uint8_t mouse_seen_port;
    uint8_t mouse_seen_speed;
    char mouse_seen_controller[XHCI_DETAIL_BYTES];
    bool mouse_seen;
};

static struct xhci_session xhci_sessions[XHCI_SESSION_SLOTS];
/* The session every `xhci->field` reference names. */
static struct xhci_session *xhci = &xhci_sessions[0];
static struct xhci_machine xhci_machine;
/* ITEM-452: where the round-robin service pass starts (fairness between the
   keyboard's session and the pointer's). */
static uint32_t xhci_service_cursor;

/*
 * ITEM-468 (СРОЧНО-18): the window between two once-a-second diagnostic
 * publications - the events and whole reports the service pass applied, and how
 * many turns it took.  They are the "input starves while the wire is busy"
 * numbers; the loop turn's own cost rides in from kernel.c with them.
 */
static uint64_t xhci_diag_last_ticks;
/* MOUSE-DIAG step 3: counts the once-a-second diagnostic publications, so the
   endpoint snapshots ride every fifth of them. */
static uint64_t xhci_ep_state_publications;
static uint32_t xhci_diag_window_events;
static uint32_t xhci_diag_window_reports;
static uint32_t xhci_diag_window_passes;

static struct xhci_hid_device *xhci_device(uint8_t role) {
    return &xhci->devices[role < XHCI_HID_DEVICES ? role
                                                  : XHCI_HID_ROLE_KEYBOARD];
}

/* ITEM-452: the session a role is bound in, machine-wide, or NULL. */
static struct xhci_session *xhci_role_session(uint8_t role) {
    return role == XHCI_HID_ROLE_MOUSE ? xhci_machine.mouse_session
                                       : xhci_machine.keyboard_session;
}

/*
 * ITEM-452: the role's device in whichever session holds it.  The accessors
 * below answer zero when no session does, so the fallback slot's zeroed device
 * is exactly the "no such device" record - never another session's live one.
 */
static struct xhci_hid_device *xhci_role_device(uint8_t role) {
    struct xhci_session *session = xhci_role_session(role);

    return session != NULL ? &session->devices[role]
                           : &xhci_sessions[0].devices[role];
}

/*
 * ITEM-579: the slot's own EP0 ring.  A physical device has one slot, one
 * address and one EP0, so a composite device's two roles issue their control
 * transfers on the same ring (serialized, since every transfer is polled to
 * completion).  The per-role ring is the fallback for a session that has no
 * path yet - the focused host proofs use it directly.
 */
static struct xhci_ring *xhci_device_ep0_ring(struct xhci_hid_device *device) {
    if (device->path != NULL) {
        return &device->path->ep0_ring;
    }
    return &xhci->ep0_rings[device->role];
}

static uint32_t xhci_device_interrupt_ring_offset(uint8_t role) {
    return XHCI_OFF_DEVICE_INTERRUPT_RING + (uint32_t)role * 0x1000U;
}

static uint32_t xhci_device_data_offset(uint8_t role) {
    return XHCI_OFF_DEVICE_DATA + (uint32_t)role * XHCI_DEVICE_DATA_STRIDE;
}

/*
 * ITEM-578: the buffer a control transfer's Data Stage must name.
 *
 * The generic descriptor buffer holds the device, configuration and string
 * descriptors; the HID report descriptor has its own buffer next to it, and the
 * report-descriptor parser reads that one.  The Data Stage TRB has to name the
 * same address the parser reads - until this item it named the generic buffer
 * for every request, so a report descriptor was transferred into one buffer and
 * classified from another (which is why the a853d322 run refused a device whose
 * descriptor had been read without error).
 */
static uint32_t xhci_device_descriptor_offset(uint8_t role) {
    return xhci_device_data_offset(role) + XHCI_DATA_DESCRIPTOR;
}

static uint32_t xhci_device_report_descriptor_offset(uint8_t role) {
    return xhci_device_data_offset(role) + XHCI_DATA_REPORT_DESCRIPTOR;
}

/*
 * ITEM-469: one queued interrupt transfer's report buffer.  Each armed TRB
 * points at its own slot, so the controller never writes a second report over
 * the bytes a just-completed one is being decoded from.
 */
static uint32_t xhci_report_buffer_offset(
    const struct xhci_hid_device *device,
    uint8_t slot) {
    if (slot >= XHCI_REPORT_SLOTS) {
        slot = 0U;
    }
    return xhci_device_data_offset(device->role) + XHCI_DATA_REPORT_QUEUE +
           (uint32_t)slot * XHCI_REPORT_SLOT_BYTES;
}

static void xhci_outl(uint16_t port, uint32_t value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static uint32_t xhci_inl(uint16_t port) {
    uint32_t value;

    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint32_t xhci_pci_read32(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint16_t offset) {
    const uint32_t address = XHCI_PCI_CONFIG_ENABLE |
                             ((uint32_t)bus << 16U) |
                             ((uint32_t)device << 11U) |
                             ((uint32_t)function << 8U) |
                             ((uint32_t)offset & 0xFCU);

    xhci_outl(XHCI_PCI_CONFIG_ADDRESS, address);
    return xhci_inl(XHCI_PCI_CONFIG_DATA);
}

static void xhci_pci_write32(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint16_t offset,
    uint32_t value) {
    const uint32_t address = XHCI_PCI_CONFIG_ENABLE |
                             ((uint32_t)bus << 16U) |
                             ((uint32_t)device << 11U) |
                             ((uint32_t)function << 8U) |
                             ((uint32_t)offset & 0xFCU);

    xhci_outl(XHCI_PCI_CONFIG_ADDRESS, address);
    xhci_outl(XHCI_PCI_CONFIG_DATA, value);
}

static bool xhci_pci_read(
    void *context,
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint16_t offset,
    uint32_t *value) {
    (void)context;
    if (value == NULL || (offset % 4U) != 0U) {
        return false;
    }
    *value = xhci_pci_read32(bus, device, function, offset);
    return true;
}

static bool xhci_pci_write(
    void *context,
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint16_t offset,
    uint32_t value) {
    (void)context;
    if ((offset % 4U) != 0U) {
        return false;
    }
    xhci_pci_write32(bus, device, function, offset, value);
    return true;
}

#ifdef HFOC_XHCI_MMIO_HOST_TEST
/*
 * ITEM-577: the host proof drives the production start/command/enumeration
 * path against a register model, so the model must see every register access,
 * not a RAM mirror (HCRST/CNR/RS and the doorbell are write-and-observe
 * handshakes).  The seam exists only in this test build; the shipped kernel
 * below uses the plain accesses.
 */
uint32_t xhci_host_mmio_read32(const volatile uint8_t *base, uint32_t offset);
void xhci_host_mmio_write32(volatile uint8_t *base, uint32_t offset,
                            uint32_t value);

static uint32_t xhci_read32(const volatile uint8_t *base, uint32_t offset) {
    return xhci_host_mmio_read32(base, offset);
}

static void xhci_write32(
    volatile uint8_t *base,
    uint32_t offset,
    uint32_t value) {
    xhci_host_mmio_write32(base, offset, value);
}
#else
static uint32_t xhci_read32(const volatile uint8_t *base, uint32_t offset) {
    return *(volatile uint32_t *)(base + offset);
}

static void xhci_write32(
    volatile uint8_t *base,
    uint32_t offset,
    uint32_t value) {
    *(volatile uint32_t *)(base + offset) = value;
}
#endif

static void xhci_write64(volatile uint8_t *reg, uint64_t value) {
    xhci_write32(reg, 0U, (uint32_t)(value & 0xFFFFFFFFULL));
    xhci_write32(reg, 4U, (uint32_t)(value >> 32));
}

static volatile uint8_t *xhci_capability(uint32_t offset) {
    return xhci->regs + offset;
}

static volatile uint8_t *xhci_operational(uint32_t offset) {
    return xhci->regs + (uint32_t)xhci->caplength + offset;
}

static volatile uint8_t *xhci_runtime(uint32_t offset) {
    return xhci->regs + xhci->rt_offset + offset;
}

static volatile uint8_t *xhci_doorbell(uint32_t index) {
    return xhci->regs + xhci->db_offset + index * 4U;
}

static volatile uint8_t *xhci_portsc(uint32_t port) {
    return xhci_operational(XHCI_OP_PORTSC_BASE +
                            (port - 1U) * XHCI_OP_PORTSC_STRIDE);
}

/* Serialize MMIO writes against the DMA structures the controller reads.
   x86 keeps the write order, and the fence keeps the compiler from moving the
   ring writes past the doorbell. */
static void xhci_barrier(void) {
    __asm__ volatile ("mfence" : : : "memory");
}

static uint8_t *xhci_dma_at(uint32_t offset) {
    return xhci->dma + offset;
}

static uint64_t xhci_dma_physical(uint32_t offset) {
    return xhci->dma_physical + (uint64_t)offset;
}

/* ITEM-577: the small arithmetic the runtime pool layout needs. */
static uint32_t xhci_align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

/* HCSPARAMS2, xHCI 1.2 section 5.3.4: Max Scratchpad Buffers is 10 bits, the
   high five in 25:21 and the low five in 31:27 (Linux's HCS_MAX_SCRATCHPAD). */
static uint32_t xhci_scratchpad_count(uint32_t hcsparams2) {
    return ((hcsparams2 >> 21U) & 0x1FU) << 5U |
           ((hcsparams2 >> 27U) & 0x1FU);
}

static void xhci_set_status(enum xhci_status status, const char *detail) {
    xhci->status = status;
    xhci->detail = detail != NULL ? detail : "UNKNOWN";
    xhci->state = status == XHCI_STATUS_OK ? "READY" : "UNAVAILABLE";
}

/* Defined further down; the session helpers below are what the walk uses to
   stop a controller it is leaving and to keep a detail across a probe. */
static void xhci_controller_stop(void);
static void xhci_detail_copy(char *target, size_t size, const char *source);
static void xhci_evidence_keep_session(struct xhci_session *session,
                                       bool released);
/* ITEM-580: the machine-wide pointer record is refreshed with the sessions. */
static void xhci_evidence_keep_mouse(void);
/* ITEM-578: the role's own refusal, published by the report-descriptor walks
   before the refusal helper itself is defined below. */
static void xhci_attach_refuse(struct xhci_hid_device *device,
                               enum xhci_status status, const char *stage,
                               const char *detail);
/* ITEM-579: the endpoint state the repair path reads from the Device Context. */
static uint32_t xhci_device_endpoint_state(
    const struct xhci_hid_device *device);

/*
 * ITEM-452: the session a visit works in.
 *
 * A free slot is re-zeroed and marked in use; the DMA pool it already owns (a
 * released visit keeps its pool so the next visit reuses the pages instead of
 * asking the physical allocator for fourteen more) is carried across the
 * reset.  Returns NULL when every slot is held by a live role session, which
 * is the one case where a controller cannot be surveyed; the caller publishes
 * that instead of writing into a live session's pool.
 */
static struct xhci_session *xhci_session_acquire(void) {
    for (uint32_t index = 0U; index < XHCI_SESSION_SLOTS; ++index) {
        struct xhci_session *session = &xhci_sessions[index];
        const uint64_t dma_physical = session->dma_physical;
        uint8_t *const dma = session->dma;

        if (session->in_use) {
            continue;
        }
        memset(session, 0, sizeof(*session));
        session->dma_physical = dma_physical;
        session->dma = dma;
        session->in_use = true;
        xhci = session;
        return session;
    }
    return NULL;
}

/*
 * Release a session that bound nothing.  The controller is halted first, so it
 * cannot write the pool after the visit; the pool itself stays with the slot.
 */
static void xhci_session_release(struct xhci_session *session) {
    if (session == NULL) {
        return;
    }
    const uint64_t dma_physical = session->dma_physical;
    uint8_t *const dma = session->dma;

    /* ITEM-573: the durable copy of this controller, taken while the session
       still names its registers and its roles, before the stop and the
       memset below. */
    xhci_evidence_keep_session(session, true);
    /* Copy before stop/reset: neither transient strings nor MMIO pointers
       from this session may outlive its release. */
    xhci_machine.last_probe.valid = true;
    xhci_machine.last_probe.location = session->location;
    xhci_machine.last_probe.status = session->status;
    xhci_machine.last_probe.caplength = session->caplength;
    xhci_machine.last_probe.mmio_bytes = session->mmio_bytes;
    xhci_detail_copy(xhci_machine.last_probe.detail,
                     sizeof(xhci_machine.last_probe.detail), session->detail);
    xhci = session;
    if (session->regs != NULL) {
        xhci_controller_stop();
    }
    memset(session, 0, sizeof(*session));
    session->dma_physical = dma_physical;
    session->dma = dma;
}

/* The role's device in one named session; used where the session is not the
   one the machine-wide accessors resolve (the walk, the re-bind). */
static struct xhci_hid_device *xhci_session_device(
    struct xhci_session *session,
    uint8_t role) {
    return &session->devices[role < XHCI_HID_DEVICES ? role
                                                     : XHCI_HID_ROLE_KEYBOARD];
}

/*
 * ITEM-573: copy one session into the machine's durable controller evidence.
 *
 * The copy happens before the session is stopped or zeroed (a released
 * session) and again for a session that stays live, so the report has the same
 * facts either way.  It reads session values only: no register is touched, a
 * transient detail pointer is copied into a bounded array, and the per-port
 * array is a value copy - which is what lets the export run after every
 * session was released without any MMIO access.
 */
static void xhci_evidence_collect_session(
    struct xhci_session *session, bool released,
    struct xhci_evidence_controller *controller) {
    /* PCI bus 0 is a real bus (the QEMU controller and the laptop's both sit
       on it), so the validity signal is the slot being in use, not the bus
       number. */
    if (session == NULL || !session->in_use || controller == NULL) {
        return;
    }
    memset(controller, 0, sizeof(*controller));
    controller->valid = true;
    controller->released = released;
    controller->location = session->location;
    controller->pci_vendor = session->pci_vendor;
    controller->pci_device = session->pci_device;
    controller->bar_physical = session->bar_physical;
    controller->mmio_bytes = session->mmio_bytes;
    controller->caplength = session->caplength;
    controller->max_ports = session->max_ports;
    controller->max_slots = session->max_slots;
    controller->slots_enabled = session->slots_enabled;
    controller->usbcmd = session->usbcmd;
    controller->usbsts = session->usbsts;
    controller->ports_seen = session->ports_seen;
    controller->port_count = session->port_evidence_count;
    for (uint32_t index = 0U;
         index < session->port_evidence_count &&
         index < XHCI_EVIDENCE_PORTS;
         ++index) {
        controller->ports[index] = session->port_evidence[index];
    }
    xhci_detail_copy(controller->status, sizeof(controller->status),
                     xhci_status_string(session->status));
    xhci_detail_copy(controller->detail, sizeof(controller->detail),
                     session->detail != NULL ? session->detail : "UNKNOWN");
    for (uint32_t index = 0U;
         index < session->protocol_count && index < XHCI_EVIDENCE_PROTOCOLS;
         ++index) {
        const struct xhci_protocol_group *group = &session->protocols[index];
        struct xhci_evidence_protocol *kept = &controller->protocols[index];

        kept->major = group->major;
        kept->minor = group->minor;
        kept->port_offset = group->port_offset;
        kept->port_count = group->port_count;
        for (uint32_t letter = 0U; letter < XHCI_EVIDENCE_NAME; ++letter) {
            kept->name[letter] = group->name[letter];
        }
        controller->protocol_count += 1U;
    }
    for (uint32_t role = 0U; role < XHCI_HID_DEVICES; ++role) {
        const struct xhci_hid_device *device = &session->devices[role];
        struct xhci_evidence_role *kept = &controller->roles[role];

        kept->role = device->role;
        kept->seen = device->device_seen ? 1U : 0U;
        kept->bound = device->ready ? 1U : 0U;
        kept->port = device->port;
        kept->speed = device->speed;
        kept->slot_id = device->slot_id;
        kept->dci = device->dci;
        kept->interface_number = device->interface_number;
        kept->interface_protocol = device->interface_protocol;
        kept->endpoint_address = device->endpoint_address;
        kept->vendor_id = device->vendor_id;
        kept->product_id = device->product_id;
        kept->reports = device->report_count;
        kept->transfers = device->transfer_count;
        kept->errors = device->error_count;
        kept->completions = device->completion_count;
        kept->restarts = device->endpoint_restarts;
        kept->endpoint_state = device->endpoint_state;
        kept->refused = device->refused_report_count;
        kept->dropped = device->dropped_report_count;
        kept->short_packets = device->short_packet_count;
        kept->mse = device->mse_count;
        kept->event_ring_full = device->event_ring_full_count;
        kept->no_trb = device->no_trb_count;
        kept->last_completion_code = device->last_completion_code;
        xhci_detail_copy(kept->interface_summary,
                         sizeof(kept->interface_summary),
                         device->interface_summary);
        /* ITEM-578: the role's own attempt - its stage, reason and detail -
           and the report descriptor the read returned. */
        kept->attempted = device->attempt_made ? 1U : 0U;
        xhci_detail_copy(kept->stage, sizeof(kept->stage),
                         device->attempt_stage != NULL ? device->attempt_stage
                                                       : "NONE");
        xhci_detail_copy(kept->reason, sizeof(kept->reason),
                         xhci_status_string(device->attempt_status));
        xhci_detail_copy(kept->detail, sizeof(kept->detail),
                         device->attempt_detail);
        kept->report_descriptor_read = device->report_descriptor_read;
        kept->report_descriptor_parse = device->report_descriptor_parse;
        kept->report_descriptor_length = device->report_descriptor_length;
        memcpy(kept->report_descriptor_bytes, device->report_descriptor_bytes,
               sizeof(kept->report_descriptor_bytes));
        controller->role_count = role + 1U;
    }
    /* ITEM-574: the routing registers (before and after this visit), the
       routing stage's verdict, the handoff result and the shared connect
       window are values, so they survive the release like the port words. */
    controller->route_before = session->route_before;
    controller->route_after = session->route_after;
    xhci_detail_copy(controller->route_outcome, sizeof(controller->route_outcome),
                     session->route_outcome[0] != '\0'
                         ? session->route_outcome
                         : "UNKNOWN");
    xhci_detail_copy(controller->route_detail, sizeof(controller->route_detail),
                     session->route_detail);
    controller->route_target_usb2 = session->route_target_usb2;
    controller->route_target_usb3 = session->route_target_usb3;
    controller->route_usb2_field = session->route_usb2_field;
    controller->route_wrote_usb2 = session->route_wrote_usb2;
    controller->route_wrote_usb3 = session->route_wrote_usb3;
    controller->route_restored = session->route_restored;
    xhci_detail_copy(controller->legacy, sizeof(controller->legacy),
                     session->legacy);
    controller->settle_ms = session->settle_ms;
    controller->settle_bound_ms = session->settle_bound_ms;
    controller->settle_pending = session->settle_pending;
    controller->hcs_params2 = session->hcs_params2;
    controller->scratchpad_count = session->scratchpad_count;
    controller->scratchpad_page_bytes = session->scratchpad_page_bytes;
    controller->context_bytes = (uint32_t)session->context_bytes;
    controller->slot_area_bytes = session->slot_area_bytes;
    controller->pool_bytes = session->pool_bytes;
    controller->dcbaa_physical = session->dcbaa_physical;
    controller->dcbaa0 = session->dcbaa0;
    xhci_detail_copy(controller->fatal, sizeof(controller->fatal),
                     session->fatal);
}

static void xhci_evidence_keep_session(struct xhci_session *session,
                                       bool released) {
    struct xhci_evidence_controller controller;

    if (session == NULL || !session->in_use) {
        return;
    }
    xhci_evidence_collect_session(session, released, &controller);
    xhci_evidence_controller_keep(&controller);
}

/*
 * ITEM-580: take the export's one snapshot of the live machine.
 *
 * The kept controller evidence is the visit's copy: it says what the walk
 * found and how it ended.  The counters in it are the ones the roles had when
 * the visit finished - for a pointer that has since decoded thousands of
 * reports they read `completions=0 reports=0 epstate=DISABLED`, which is
 * exactly the contradiction the 10231684 report showed next to a working
 * `[INPUT-DIAG]`.  This refresh updates only the values a running machine
 * changes (the operational registers and every role's live binding, counters
 * and endpoint state); the visit's evidence - its verdict and refusal reason,
 * the routing and handoff snapshots, the ports, the protocol groups, the DMA
 * geometry and the first fatal snapshot - stays as the visit recorded it, and
 * a session that is no longer in use is not touched at all.
 *
 * It is called once, before the report is composed, so `[INPUT-DIAG]`,
 * `[XHCI-INPUT]` and `[XHCI-MOUSE]` in one export describe one moment.  It
 * reads no register writable path, submits no command and never resets or
 * re-enumerates anything: the export must not change the machine it reports.
 */
static void xhci_evidence_refresh_live(void) {
    for (uint32_t slot = 0U; slot < XHCI_SESSION_SLOTS; ++slot) {
        struct xhci_session *session = &xhci_sessions[slot];
        struct xhci_evidence_controller live;

        if (!session->in_use) {
            continue;
        }
        xhci_evidence_collect_session(session, false, &live);
        xhci_evidence_controller_refresh(&live);
    }
    xhci_evidence_keep_mouse();
}

/*
 * ITEM-573: the machine-wide pointer verdict, filled from what the walk left in
 * the machine - the same fields the `[MOUSE]` console record uses.  It is kept
 * separately from the controller evidence so the export always answers "was
 * the pointer detected, and where did it stop", even when no session survived.
 */
static void xhci_evidence_keep_mouse(void) {
    struct xhci_evidence_mouse mouse;

    memset(&mouse, 0, sizeof(mouse));
    mouse.valid = true;
    mouse.wheel_offset = USB_HID_REPORT_OFFSET_NONE;
    mouse.seen = xhci_machine.mouse_seen || xhci_machine.mouse_device_seen;
    mouse.vendor_id = xhci_machine.mouse_vendor_id;
    mouse.product_id = xhci_machine.mouse_product_id;
    mouse.port = xhci_machine.mouse_seen_port;
    mouse.speed = xhci_machine.mouse_seen_speed;
    if (xhci_machine.mouse_session != NULL) {
        const struct xhci_hid_device *device =
            &xhci_machine.mouse_session->devices[XHCI_HID_ROLE_MOUSE];

        if (device->ready) {
            mouse.bound = true;
            mouse.seen = true;
            xhci_detail_copy(mouse.verdict, sizeof(mouse.verdict), "BOUND");
            xhci_detail_copy(mouse.detail, sizeof(mouse.detail),
                             xhci_status_string(XHCI_STATUS_OK));
            xhci_detail_copy(mouse.controller, sizeof(mouse.controller),
                             xhci_machine.mouse_seen_controller);
            mouse.vendor_id = device->vendor_id;
            mouse.product_id = device->product_id;
            mouse.port = device->port;
            mouse.speed = device->speed;
            mouse.interface_number = device->interface_number;
            mouse.endpoint_address = device->endpoint_address;
            mouse.reports = device->report_count;
            mouse.transfers = device->transfer_count;
            mouse.errors = device->error_count;
            mouse.completions = device->completion_count;
            mouse.wheels = device->mouse.wheels;
            /*
             * ITEM-639: a boot pointer has no layout; publish `none` rather
             * than the zeroed offset, and keep the actual last transfer length
             * so three-byte boot reports are distinguishable in the diag.
             */
            mouse.report_mode = device->report_mode;
            mouse.wheel_offset = device->report_mode
                                     ? device->mouse_layout.wheel_offset
                                     : USB_HID_REPORT_OFFSET_NONE;
            mouse.report_length =
                device->report_mode ? device->mouse_layout.report_length : 0U;
            mouse.last_report_bytes = device->last_report_bytes;
            xhci_detail_copy(mouse.interface_summary,
                             sizeof(mouse.interface_summary),
                             device->interface_summary);
        }
    }
    if (!mouse.bound) {
        xhci_detail_copy(mouse.verdict, sizeof(mouse.verdict),
                         mouse.seen ? "REFUSED" : "NOT_DETECTED");
        xhci_detail_copy(mouse.detail, sizeof(mouse.detail),
                         xhci_machine.mouse_detail[0] != '\0'
                             ? xhci_machine.mouse_detail
                             : usb_hid_status_string(
                                   xhci_machine.mouse_status));
        xhci_detail_copy(mouse.controller, sizeof(mouse.controller),
                         xhci_machine.mouse_seen_controller);
    }
    /*
     * ITEM-573: the store upgrades a NOT_DETECTED pointer to SEEN when a kept
     * controller reports a connect on one of its ports - the device was read
     * before the enumeration stopped, so the report names that port and the
     * controller's own stage instead of claiming nothing was there.
     */
    xhci_evidence_mouse_keep(&mouse);
}

/*
 * ITEM-452: the machine-wide verdict the [KBD ] record and the console
 * publish.  It is OK exactly when a keyboard is bound somewhere, and the most
 * informative refusal otherwise - a controller that failed after the keyboard
 * was found does not turn the machine's answer into that failure.
 */
static void xhci_machine_set_verdict(enum xhci_status status,
                                     const char *detail) {
    xhci_machine.verdict_status = status;
    xhci_detail_copy(xhci_machine.verdict_detail_storage,
                     sizeof(xhci_machine.verdict_detail_storage),
                     detail != NULL ? detail : "UNKNOWN");
    xhci_machine.verdict_detail = xhci_machine.verdict_detail_storage;
}

/*
 * ITEM-452: a keyboard lost at runtime is the machine's loss exactly when the
 * session that lost it is the one holding the keyboard - the pointer's
 * controller failing must not rewrite the machine's input verdict, and the
 * keyboard's own controller failing must not leave it reading READY.
 */
static void xhci_note_keyboard_lost(enum xhci_status status,
                                    const char *detail) {
    xhci_set_status(status, detail);
    if (xhci_machine.keyboard_session == NULL ||
        xhci_machine.keyboard_session == xhci) {
        xhci_machine_set_verdict(status, detail);
    }
}

/*
 * ITEM-406: the detail a rejection publishes has to name the stage *and* the
 * place, because the acceptance host has three controllers, several root ports
 * per controller and more than one device on the machine. These compose
 * "P2_GET_DESCRIPTOR_STALL" or "CTRL_05:00.3_BAR0_NOT_MEMORY" into one bounded
 * buffer, and they copy the outcome first: it may be the very buffer they are
 * about to write.
 */
static char *xhci_detail_append(char *cursor, const char *end,
                                const char *text) {
    if (text == NULL) {
        return cursor;
    }
    while (*text != '\0' && cursor < end) {
        *cursor++ = *text++;
    }
    return cursor;
}

static char *xhci_detail_append_hex2(char *cursor, const char *end,
                                     uint8_t value) {
    static const char digits[] = "0123456789ABCDEF";

    if (cursor + 2 <= end) {
        *cursor++ = digits[(value >> 4) & 0xFU];
        *cursor++ = digits[value & 0xFU];
    }
    return cursor;
}

static char *xhci_detail_append_hex32(char *cursor, const char *end,
                                     uint32_t value) {
    static const char digits[] = "0123456789ABCDEF";

    if (cursor + 10 > end) {
        return cursor;
    }
    *cursor++ = '0';
    *cursor++ = 'x';
    for (int shift = 28; shift >= 0; shift -= 4) {
        *cursor++ = digits[(value >> (unsigned int)shift) & 0x0FU];
    }
    return cursor;
}

static char *xhci_detail_append_hex64(char *cursor, const char *end,
                                     uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";

    if (cursor + 18 > end) {
        return cursor;
    }
    *cursor++ = '0';
    *cursor++ = 'x';
    for (int shift = 60; shift >= 0; shift -= 4) {
        *cursor++ = digits[(value >> (unsigned int)shift) & 0x0FU];
    }
    return cursor;
}

static char *xhci_detail_append_port(char *cursor, const char *end,
                                     uint32_t port) {
    char digits[11];
    size_t length = 0U;

    if (cursor >= end) {
        return cursor;
    }
    *cursor++ = 'P';
    if (port == 0U) {
        if (cursor < end) {
            *cursor++ = '0';
        }
        return cursor;
    }
    while (port != 0U && length < sizeof(digits)) {
        digits[length++] = (char)('0' + (port % 10U));
        port /= 10U;
    }
    while (length != 0U && cursor < end) {
        *cursor++ = digits[--length];
    }
    return cursor;
}

static const char *xhci_detail_port(uint32_t port, const char *stage,
                                    const char *outcome) {
    char copied[32];
    size_t length = 0U;
    char *cursor = xhci->detail_buffer;
    const char *end = xhci->detail_buffer + sizeof(xhci->detail_buffer) - 1U;

    if (outcome != NULL) {
        while (outcome[length] != '\0' && length + 1U < sizeof(copied)) {
            copied[length] = outcome[length];
            ++length;
        }
    }
    copied[length] = '\0';
    cursor = xhci_detail_append_port(cursor, end, port);
    cursor = xhci_detail_append(cursor, end, "_");
    cursor = xhci_detail_append(cursor, end, stage);
    if (length != 0U) {
        cursor = xhci_detail_append(cursor, end, "_");
        cursor = xhci_detail_append(cursor, end, copied);
    }
    *cursor = '\0';
    return xhci->detail_buffer;
}

static const char *xhci_detail_controller(const char *stage,
                                          const char *outcome) {
    char copied[32];
    size_t length = 0U;
    char *cursor = xhci->detail_buffer;
    const char *end = xhci->detail_buffer + sizeof(xhci->detail_buffer) - 1U;

    if (outcome != NULL) {
        while (outcome[length] != '\0' && length + 1U < sizeof(copied)) {
            copied[length] = outcome[length];
            ++length;
        }
    }
    copied[length] = '\0';
    cursor = xhci_detail_append(cursor, end, "CTRL_");
    cursor = xhci_detail_append_hex2(cursor, end, xhci->location.bus);
    cursor = xhci_detail_append(cursor, end, ":");
    cursor = xhci_detail_append_hex2(cursor, end, xhci->location.device);
    cursor = xhci_detail_append(cursor, end, ".");
    cursor = xhci_detail_append_hex2(cursor, end, xhci->location.function);
    cursor = xhci_detail_append(cursor, end, "_");
    cursor = xhci_detail_append(cursor, end, stage);
    if (length != 0U) {
        cursor = xhci_detail_append(cursor, end, "_");
        cursor = xhci_detail_append(cursor, end, copied);
    }
    *cursor = '\0';
    return xhci->detail_buffer;
}

/*
 * A detail that has to outlive the probe that produced it is copied into
 * `xhci->kept_detail`: `xhci->detail_buffer` is rewritten by the next port or
 * controller, and a published pointer into it would start naming a different
 * port while the status stayed the same.
 */
static void xhci_detail_copy(char *target, size_t size, const char *source) {
    size_t index = 0U;

    if (target == NULL || size == 0U) {
        return;
    }
    if (source != NULL) {
        while (source[index] != '\0' && index + 1U < size) {
            target[index] = source[index];
            ++index;
        }
    }
    target[index] = '\0';
}

static const char *xhci_detail_keep(const char *source) {
    xhci_detail_copy(xhci->kept_detail, sizeof(xhci->kept_detail), source);
    return xhci->kept_detail;
}

/* The same, into a caller-owned scratch buffer, for a selection that is still
   being made (the port loop keeps the most informative rejection of the ports
   it visited). */
static const char *xhci_detail_hold(char *target, size_t size,
                                    const char *source) {
    xhci_detail_copy(target, size, source);
    return target;
}

static bool xhci_wait_status(uint32_t mask, uint32_t expected, uint32_t spins) {
    for (uint32_t index = 0U; index < spins; ++index) {
        const uint32_t status =
            xhci_read32(xhci_operational(XHCI_OP_USBSTS), 0U);

        if ((status & mask) == expected) {
            return true;
        }
        __asm__ volatile ("pause");
    }
    return false;
}

static bool xhci_next_event(struct xhci_event *event) {
    const volatile uint32_t *source;
    struct xhci_trb raw;
    uint32_t index;

    if (event == NULL || xhci->event_ring == NULL) {
        return false;
    }
    source = (const volatile uint32_t *)&xhci->event_ring[xhci->event_index];
    /*
     * The producer's cycle state alternates once per lap around the segment,
     * so the entry is published only when its cycle bit equals the cycle state
     * this consumer is waiting for. Comparing against a constant instead would
     * either stop consuming after the first wrap (the controller then reports
     * an event ring full and stops reporting transfers) or re-read a retired
     * entry as if it were new.
     */
    if (((source[3] & XHCI_TRB_CYCLE) != 0U ? 1U : 0U) !=
        (uint32_t)xhci->event_cycle) {
        return false;
    }
    for (index = 0U; index < 4U; ++index) {
        raw.dword[index] = source[index];
    }
    {
        const bool decoded = xhci_event_decode(&raw, event);

        if (!decoded) {
            /* A TRB this driver does not consume still has to be retired. */
            event->type = XHCI_TRB_NORMAL;
            event->completion_code = 0U;
        }
        ++xhci->event_index;
        if (xhci->event_index >= XHCI_EVENT_TRBS) {
            xhci->event_index = 0U;
            xhci->event_cycle = (uint8_t)(xhci->event_cycle ^ 1U);
        }
        /* Hand the consumed entries back and clear the Event Handler Busy. */
        xhci_write64(xhci_runtime(XHCI_RT_IR0 + XHCI_IR_ERDP),
                     xhci->event_ring_physical +
                         (uint64_t)xhci->event_index * XHCI_TRB_BYTES +
                         XHCI_ERDP_EHB);
        return decoded;
    }
}


/* Defined with the service pass; the control and command waits below apply
   the events that are not their own through it. */
static bool xhci_apply_event(const struct xhci_event *event);

/*
 * ITEM-550: the 64-bit register readers the command snapshot uses live with the
 * silence snapshot far below; the wait that needs them is here.
 */
static uint64_t xhci_diag_read64(volatile uint8_t *base);

/*
 * ITEM-550: one command that did not complete, captured where it failed.
 *
 * The owner's L875 run could say `ENUMERATION_FAILED` and
 * `P1_ENABLE_SLOT_TIMEOUT` and nothing else.  This reads - never writes - the
 * controller state that separates the possible causes: whether the ring
 * accepted the TRB, where it landed and with which cycle state, what CRCR says
 * about the ring base and the consumer cycle the controller holds, whether the
 * controller was running
 * (USBCMD.RS, USBSTS.HCH), what MaxSlotsEn the CONFIG register actually holds,
 * where the device context array and the event ring segment table point, and
 * how far the event consumer had moved.  The record is copied into the machine
 * and published to the durable journal once, after the wait has given up: no
 * print and no journal write happens inside the wait loop, so a controller that
 * is merely slow cannot be made slower by its own diagnostic.
 */
static void xhci_command_capture(
    struct xhci_command_snapshot *snapshot,
    enum xhci_command_result result,
    const struct xhci_trb *trb,
    uint64_t physical,
    uint32_t index,
    bool event_seen,
    const struct xhci_event *last_event,
    const struct xhci_command_wait *wait) {
    char record[XHCI_COMMAND_RECORD_BYTES];
    size_t length;

    for (size_t byte = 0U; byte < sizeof(*snapshot); ++byte) {
        ((uint8_t *)snapshot)[byte] = 0U;
    }
    snapshot->result = result;
    snapshot->physical = physical;
    snapshot->ring_index = index;
    snapshot->ring_cycle = xhci->command_ring.cycle;
    snapshot->bus = xhci->location.bus;
    snapshot->device = xhci->location.device;
    snapshot->function = xhci->location.function;
    if (trb != NULL) {
        snapshot->type = (uint8_t)xhci_trb_type(trb);
        for (uint32_t dword = 0U; dword < 4U; ++dword) {
            snapshot->trb[dword] = trb->dword[dword];
        }
    }
    if (wait != NULL) {
        snapshot->wait = *wait;
    }
    snapshot->usbcmd = xhci_read32(xhci_operational(XHCI_OP_USBCMD), 0U);
    snapshot->usbsts = xhci_read32(xhci_operational(XHCI_OP_USBSTS), 0U);
    snapshot->config = xhci_read32(xhci_operational(XHCI_OP_CONFIG), 0U);
    snapshot->crcr = xhci_diag_read64(xhci_operational(XHCI_OP_CRCR));
    snapshot->dcbaap = xhci_diag_read64(xhci_operational(XHCI_OP_DCBAAP));
    snapshot->erstsz =
        xhci_read32(xhci_runtime(XHCI_RT_IR0 + XHCI_IR_ERSTSZ), 0U);
    snapshot->erstba =
        xhci_diag_read64(xhci_runtime(XHCI_RT_IR0 + XHCI_IR_ERSTBA));
    snapshot->erdp = xhci_diag_read64(xhci_runtime(XHCI_RT_IR0 + XHCI_IR_ERDP));
    snapshot->iman = xhci_read32(xhci_runtime(XHCI_RT_IR0 + XHCI_IR_IMAN), 0U);
    snapshot->event_index = xhci->event_index;
    snapshot->event_cycle = xhci->event_cycle;
    if (event_seen && last_event != NULL) {
        snapshot->event_valid = true;
        for (uint32_t dword = 0U; dword < 4U; ++dword) {
            snapshot->event[dword] = last_event->raw[dword];
        }
    }
    snapshot->valid = true;
    xhci_command_snapshot_keep(&xhci_machine.command, snapshot);

    /* The durable journal gets the same three records the `xhci` command
       prints.  If no medium is writable they simply do not survive the boot;
       that is why the console copy is the primary one. */
    length = xhci_format_command_record(record, sizeof(record), snapshot);
    if (length > 0U) {
        console_publish_journal(record, length);
    }
    length = xhci_format_command_register_record(record, sizeof(record),
                                                 snapshot);
    if (length > 0U) {
        console_publish_journal(record, length);
    }
    length = xhci_format_command_event_record(record, sizeof(record),
                                              snapshot);
    if (length > 0U) {
        console_publish_journal(record, length);
    }
}

/*
 * ITEM-577: the first fatal controller snapshot.
 *
 * The 1ad25c25 run reported ENABLE_SLOT_TIMEOUT while USBSTS already said HSE
 * and USBCMD.RS was 0: the timeout was the symptom, the controller's first
 * fatal condition was the cause.  This captures that condition the moment it
 * is seen - before any stop, reset or release can overwrite the registers -
 * and only the first one: a later command's error must not displace it.
 *
 * `cause` is "HSE" or "HCH"; the record carries the command/status/config
 * registers and both ring/array pointers, so the export answers which fatal
 * condition the controller raised and where the command ring and DCBAA were.
 */
static void xhci_fatal_capture(const char *cause) {
    char *cursor = xhci->fatal;
    const char *end = xhci->fatal + sizeof(xhci->fatal) - 1U;
    const uint32_t usbcmd =
        xhci_read32(xhci_operational(XHCI_OP_USBCMD), 0U);
    const uint32_t usbsts =
        xhci_read32(xhci_operational(XHCI_OP_USBSTS), 0U);
    const uint32_t config =
        xhci_read32(xhci_operational(XHCI_OP_CONFIG), 0U);

    if (xhci->fatal_valid) {
        return;
    }
    cursor = xhci_detail_append(cursor, end, "cause=");
    cursor = xhci_detail_append(cursor, end, cause);
    cursor = xhci_detail_append(cursor, end, " usbcmd=");
    cursor = xhci_detail_append_hex32(cursor, end, usbcmd);
    cursor = xhci_detail_append(cursor, end, " usbsts=");
    cursor = xhci_detail_append_hex32(cursor, end, usbsts);
    cursor = xhci_detail_append(cursor, end, " config=");
    cursor = xhci_detail_append_hex32(cursor, end, config);
    cursor = xhci_detail_append(cursor, end, " crcr=");
    cursor = xhci_detail_append_hex64(
        cursor, end, xhci_diag_read64(xhci_operational(XHCI_OP_CRCR)));
    cursor = xhci_detail_append(cursor, end, " dcbaap=");
    cursor = xhci_detail_append_hex64(
        cursor, end, xhci_diag_read64(xhci_operational(XHCI_OP_DCBAAP)));
    *cursor = '\0';
    xhci_detail_copy(xhci->fatal_cause, sizeof(xhci->fatal_cause),
                     cause[0] == 'H' && cause[1] == 'S'
                         ? "CONTROLLER_FATAL_HSE"
                         : "CONTROLLER_FATAL_HCH");
    xhci->fatal_valid = true;
}

/* ITEM-577: the controller's first fatal condition, checked before a command
   is placed on the ring and again while the wait runs.  HSE is the controller's
   own fatal error; HCH while the driver expects it to be running is the halt
   that error leaves behind.  Once true, no further command is submitted to
   this controller until it is re-initialized. */
static bool xhci_fatal_stop(void) {
    uint32_t usbsts;

    if (xhci->fatal_valid) {
        return true;
    }
    usbsts = xhci_read32(xhci_operational(XHCI_OP_USBSTS), 0U);
    if ((usbsts & (XHCI_USBSTS_HSE | XHCI_USBSTS_HCH)) != 0U) {
        xhci_fatal_capture((usbsts & XHCI_USBSTS_HSE) != 0U ? "HSE" : "HCH");
        return true;
    }
    return false;
}

static bool xhci_submit_command(
    const struct xhci_trb *trb,
    struct xhci_event *completion) {
    struct xhci_command_snapshot snapshot;
    struct xhci_command_wait wait;
    struct xhci_event event;
    bool last_seen = false;
    bool completed = false;
    uint32_t index = 0U;
    uint64_t physical;

    /* ITEM-577: a controller whose first fatal condition was already seen is
       not given another command; the same check catches the condition before
       this command is placed on the ring. */
    if (xhci->fatal_valid) {
        return false;
    }
    if (xhci_fatal_stop()) {
        return false;
    }
    xhci_command_wait_init(&wait);
    if (!xhci_ring_push_index(&xhci->command_ring, trb, &index)) {
        xhci_command_capture(&snapshot, xhci_command_result_of(false, false),
                             trb, 0U, index, false, NULL, &wait);
        return false;
    }
    physical = xhci->command_ring.phys + (uint64_t)index * XHCI_TRB_BYTES;
    xhci_barrier();
#ifdef HFOC_XHCI_TIMEOUT_QEMU_TEST
    /*
     * ITEM-552 proof-only fault injection.  The command is placed in the ring
     * exactly as a real submission is, but the doorbell is not rung for
     * ENABLE_SLOT: the controller never sees it, the wait below runs its real
     * budget and the failure capture is the driver's own real path.  The switch
     * is compiled only into the diagnostic-export proof's kernel variant
     * (`make diag-export-qemu-media`); no shipped kernel carries it.
     */
    if (xhci_trb_type(trb) != XHCI_TRB_ENABLE_SLOT) {
        xhci_write32(xhci_doorbell(0U), 0U, 0U);
    }
#else
    xhci_write32(xhci_doorbell(0U), 0U, 0U);
#endif

    for (uint32_t spins = 0U; spins < XHCI_SPIN_COMMAND; ++spins) {
        enum xhci_command_event_kind kind;

        /* The fatal condition is checked while the wait runs, not only when
           it gives up, so the first snapshot is taken before any cleanup. */
        if ((spins & 0x3FU) == 0U && xhci_fatal_stop()) {
            break;
        }
        if (!xhci_next_event(&event)) {
            __asm__ volatile ("pause");
            continue;
        }
        kind = xhci_command_event_kind_of(&event, physical);
        xhci_command_wait_note(&wait, &event, kind);
        last_seen = true;
        if (kind == XHCI_COMMAND_EVENT_OWN) {
            if (completion != NULL) {
                *completion = event;
            }
            completed = true;
            break;
        }
        if (kind == XHCI_COMMAND_EVENT_OTHER) {
            /*
             * ITEM-452: not this wait's event - apply it to the session's bound
             * devices instead of consuming and dropping it.
             */
            (void)xhci_apply_event(&event);
        }
        /* A completion for another TRB is the wait's evidence, not its result:
           it is counted and kept, and the wait keeps looking. */
    }
    if (completed) {
        return true;
    }
    xhci_command_capture(&snapshot, xhci_command_result_of(true, false), trb,
                         physical, index, last_seen, last_seen ? &event : NULL,
                         &wait);
    return false;
}

static bool xhci_command_ok(const struct xhci_trb *trb, const char **detail) {
    struct xhci_event completion;

    if (!xhci_submit_command(trb, &completion)) {
        if (detail != NULL) {
            *detail = xhci->fatal_valid ? xhci->fatal_cause
                                        : "COMMAND_TIMEOUT";
        }
        return false;
    }
    if (completion.completion_code != XHCI_COMPLETION_SUCCESS) {
        if (detail != NULL) {
            *detail = xhci_completion_code_string(completion.completion_code);
        }
        return false;
    }
    return true;
}

/*
 * ITEM-578: reset the port even when PORTSC.PED already says it is enabled.
 *
 * A port reset is what returns a device to the Default state (xHCI 1.1
 * section 4.3), and only a device in that state answers the Address Device
 * command on address 0.  The old rule skipped the reset exactly when PED was
 * set - i.e. when the device had already been addressed by a previous role
 * attempt - so offering the same device to the second role could only fail.
 * `force` is the difference: the scan's first reset needs none, a role
 * transition and a re-bind do.
 */
static bool xhci_port_reset(uint32_t port, uint8_t *speed, bool force) {
    volatile uint8_t *portsc = xhci_portsc(port);
    uint32_t value = xhci_read32(portsc, 0U);

    if ((value & XHCI_PORTSC_CCS) == 0U) {
        return false;
    }
    /*
     * ITEM-406: every write to PORTSC carries only the bits software may write
     * (xhci_portsc_neutral) plus the bits this step actually intends, and the
     * port is kept powered. A plain read-modify-write would carry PED back:
     * bit 1 is write-one-to-clear, so writing back the 1 the controller set
     * when the reset completed *disables* the port, and every device on it
     * then fails to enumerate. QEMU's model ignores writes to PED, which is
     * why only a real controller can show this, and Linux keeps the same
     * distinction (drivers/usb/host/xhci-hub.c: xhci_port_state_to_neutral).
     */
    xhci_write32(portsc, 0U,
                 xhci_portsc_neutral(value) | XHCI_PORTSC_PP |
                     XHCI_PORTSC_RW1C);
    if (force || (value & XHCI_PORTSC_PED) == 0U) {
        xhci_write32(portsc, 0U,
                     xhci_portsc_neutral(value) | XHCI_PORTSC_PP |
                         XHCI_PORTSC_PR);
        for (uint32_t spins = 0U; spins < XHCI_SPIN_PORT_RESET; ++spins) {
            const uint32_t current = xhci_read32(portsc, 0U);

            if ((current & XHCI_PORTSC_PRC) != 0U &&
                (current & XHCI_PORTSC_PR) == 0U) {
                break;
            }
            __asm__ volatile ("pause");
        }
        value = xhci_read32(portsc, 0U);
        if ((value & XHCI_PORTSC_PRC) == 0U) {
            return false;
        }
        /* Retire the reset change bit; PED stays out of the write, so a port
           the controller enabled during the reset stays enabled. */
        xhci_write32(portsc, 0U,
                     xhci_portsc_neutral(value) | XHCI_PORTSC_PP |
                         XHCI_PORTSC_PRC);
        value = xhci_read32(portsc, 0U);
    }
    if ((value & XHCI_PORTSC_CCS) == 0U) {
        return false;
    }
    *speed = (uint8_t)((value & XHCI_PORTSC_SPEED_MASK) >>
                       XHCI_PORTSC_SPEED_SHIFT);
    return *speed != 0U;
}

/* Hand the controller over from firmware legacy support (xHCI 1.1 section
   7.1). Best effort and bounded: a machine whose firmware never releases the
   controller still gets the reset attempt below, which is what actually
   decides whether this driver can use it.

   ITEM-453: the walk follows the specification's Next Capability Pointer - an
   offset in dwords from the entry that carries it, with zero ending the list -
   instead of treating it as an absolute offset.  On QEMU's controller the two
   readings differ (its first capability at offset 8 dwords points at 4), and
   the absolute reading sent this walk into the capability registers
   themselves, where HCCPARAMS1 (0x00080001) has exactly the legacy-support
   capability ID in its low byte.  A machine whose firmware owned the
   controller could then have been "claimed" by writing a register that is not
   the legacy capability at all. */
static void xhci_claim_from_firmware(void) {
    const uint32_t hccparams = xhci_read32(xhci_capability(XHCI_CAP_HCCPARAMS1),
                                           0U);
    uint32_t offset = (hccparams >> 16U) & 0xFFFFU;

    xhci_detail_copy(xhci->legacy, sizeof(xhci->legacy),
                     "cap=0x00000000 result=ABSENT");
    for (uint32_t entries = 0U;
         entries < XHCI_EXT_CAP_ENTRIES_MAX && offset != 0U; ++entries) {
        const uint32_t base = offset * 4U;
        uint32_t capability;
        uint32_t next;

        /* ITEM-453: the walk stays inside the mapped register window. */
        if (xhci->mmio_bytes != 0U && base + 4U > xhci->mmio_bytes) {
            return;
        }
        capability = xhci_read32(xhci_capability(base), 0U);
        next = (capability >> XHCI_EXT_CAP_NEXT_SHIFT) & 0xFFU;

        if ((capability & 0xFFU) == XHCI_EXT_CAP_USBLEGSUP) {
            /* ITEM-574: the handoff's own result, kept for the export.  It is
               the firmware half of the routing question: a controller the
               firmware still owns is not one this driver may program. */
            const bool bios_owned =
                (capability & XHCI_USBLEGSUP_BIOS_OWNED) != 0U;
            bool released = !bios_owned;
            char *cursor = xhci->legacy;
            const char *end = xhci->legacy + sizeof(xhci->legacy) - 1U;

            if (bios_owned) {
                xhci_write32(xhci_capability(base), 0U,
                             capability | XHCI_USBLEGSUP_OS_OWNED);
                released = false;
                for (uint32_t spins = 0U; spins < XHCI_SPIN_LEGACY; ++spins) {
                    const uint32_t current =
                        xhci_read32(xhci_capability(base), 0U);

                    if ((current & XHCI_USBLEGSUP_BIOS_OWNED) == 0U) {
                        released = true;
                        break;
                    }
                    __asm__ volatile ("pause");
                }
            }
            cursor = xhci_detail_append(cursor, end, "cap=");
            cursor = xhci_detail_append_hex32(cursor, end, base);
            cursor = xhci_detail_append(cursor, end, " bios=");
            cursor = xhci_detail_append_hex32(cursor, end,
                                              bios_owned ? 1U : 0U);
            cursor = xhci_detail_append(cursor, end, " result=");
            cursor = xhci_detail_append(
                cursor, end,
                !bios_owned ? "NOT_OWNED" : (released ? "TAKEN" : "TIMEOUT"));
            *cursor = '\0';
            return;
        }
        if (next == 0U) {
            return;
        }
        offset += next;
    }
}

/* ITEM-574: where the Intel xHCI routing registers live (see xhci_route.h).
   The snapshot reads the firmware's state and the state a visit leaves; it is
   a pure configuration-space read, so it cannot change the original failure. */
static void xhci_route_snapshot(struct xhci_session *session,
                                struct xhci_evidence_route *out) {
    const uint8_t bus = session->location.bus;
    const uint8_t device = session->location.device;
    const uint8_t function = session->location.function;

    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->xusb2pr =
        xhci_pci_read32(bus, device, function, XHCI_ROUTE_XUSB2PR);
    out->usb2prm =
        xhci_pci_read32(bus, device, function, XHCI_ROUTE_USB2PRM);
    out->usb3_pssen =
        xhci_pci_read32(bus, device, function, XHCI_ROUTE_USB3_PSSEN);
    out->usb3prm =
        xhci_pci_read32(bus, device, function, XHCI_ROUTE_USB3PRM);
    out->read_ok = out->xusb2pr != 0xFFFFFFFFU &&
                   out->usb2prm != 0xFFFFFFFFU &&
                   out->usb3_pssen != 0xFFFFFFFFU &&
                   out->usb3prm != 0xFFFFFFFFU;
}

#ifndef HFOC_XHCI_MMIO_HOST_TEST
/* ITEM-576: the PCI configuration seam the routing decision speaks through.
   The decision reads the documented participants (00:14.0, 00:1A.0, 00:1D.0)
   by address, so the seam carries the address, not just a register offset. */
static bool xhci_route_pci_read(void *context, uint8_t bus, uint8_t device,
                                uint8_t function, uint16_t offset,
                                uint32_t *value) {
    if (value == NULL) {
        return false;
    }
    *value = xhci_pci_read32(bus, device, function, offset);
    (void)context;
    return true;
}

static bool xhci_route_pci_write(void *context, uint8_t bus, uint8_t device,
                                 uint8_t function, uint16_t offset,
                                 uint32_t value) {
    xhci_pci_write32(bus, device, function, offset, value);
    (void)context;
    return true;
}

/*
 * ITEM-574/576: the Intel routing stage, run after the BIOS/OS handoff and
 * before the controller is halted and reset - the place Linux's early quirk
 * uses, so no device has been probed by this or the EHCI driver yet.  The
 * decision, the participant verification, the write/read-back/restore and the
 * evidence text live in xhci_route.c; this function supplies the xHCI's own
 * address and keeps the result in the session for the durable evidence.
 *
 * The boot medium's safety is the documented topology: it is on 00:1A.0
 * (D26:F0), a controller the routing registers do not move.  The driver does
 * not read a root PORTSC bit as proof of the downstream medium - the RMH
 * keeps the root CCS set when the child is gone - and does not restore on such
 * a hint; the exported EHCI records and the successful diag save are the
 * medium's own proof.
 */
static void xhci_route_apply(void) {
    const struct xhci_route_ops ops = {xhci_route_pci_read,
                                       xhci_route_pci_write};
    struct xhci_route_result result;

    xhci_route_ports(&ops, NULL, xhci->location.bus, xhci->location.device,
                     xhci->location.function, xhci->pci_vendor,
                     xhci->pci_device, &result);
    xhci_detail_copy(xhci->route_outcome, sizeof(xhci->route_outcome),
                     xhci_route_outcome_string(result.outcome));
    xhci_detail_copy(xhci->route_detail, sizeof(xhci->route_detail),
                     result.detail);
    xhci->route_target_usb2 = result.target_usb2;
    xhci->route_target_usb3 = result.target_usb3;
    xhci->route_usb2_field = result.usb2_field;
    xhci->route_wrote_usb2 = result.wrote_usb2 ? 1U : 0U;
    xhci->route_wrote_usb3 = result.wrote_usb3 ? 1U : 0U;
    xhci->route_restored = result.restored ? 1U : 0U;
}

#endif

/*
 * ITEM-453: read the controller's Supported Protocol capabilities (xHCI 1.1
 * section 7.2.1) before it is reset.
 *
 * The list is reached through HCCPARAMS1's extended-capability pointer and is
 * a linked list in the capability registers, so it survives the controller
 * reset that follows and only has to be walked once per visit.  Each entry with
 * capability ID 2 declares one protocol group: the raw port range it owns and
 * the Protocol Speed IDs its ports report.  A controller with a USB2 half and
 * a USB3 half - the owner's built-in `05:00.3` is exactly that - says here
 * which raw port number belongs to which half, which is the question the
 * 2026-09-19 port evidence could not answer.
 *
 * The walk is bounded (XHCI_EXT_CAP_ENTRIES_MAX) because a broken controller
 * could make the list cyclic, and every entry's fields go through
 * xhci_protocol_capability(), which refuses what it cannot use.  A controller
 * that declares nothing parses to zero groups; its ports are then published
 * with `group=NONE` rather than with an invented label.
 */
static void xhci_read_protocol_groups(void) {
    const uint32_t hccparams =
        xhci_read32(xhci_capability(XHCI_CAP_HCCPARAMS1), 0U);
    uint32_t offset = (hccparams >> 16U) & 0xFFFFU;

    xhci->protocol_count = 0U;
    for (uint32_t entries = 0U;
         entries < XHCI_EXT_CAP_ENTRIES_MAX && offset != 0U; ++entries) {
        const uint32_t base = offset * 4U;
        uint32_t dword0;
        uint32_t next;

        /* ITEM-453: never read past the window that was mapped.  A controller
           whose pointer is bogus is refused by the bound, not by a fault. */
        if (xhci->mmio_bytes != 0U && base + 12U > xhci->mmio_bytes) {
            break;
        }
        dword0 = xhci_read32(xhci_capability(base), 0U);
        next = (dword0 & XHCI_EXT_CAP_NEXT_MASK) >> XHCI_EXT_CAP_NEXT_SHIFT;

        if ((dword0 & XHCI_EXT_CAP_ID_MASK) == XHCI_EXT_CAP_PROTOCOL) {
            const uint32_t dword1 = xhci_read32(xhci_capability(base + 4U), 0U);
            const uint32_t dword2 = xhci_read32(xhci_capability(base + 8U), 0U);
            const uint32_t psic =
                (dword2 & XHCI_EXT_PORT_PSIC_MASK) >> XHCI_EXT_PORT_PSIC_SHIFT;
            uint32_t psi[XHCI_PROTOCOL_PSI_MAX];
            uint32_t psi_count = 0U;

            for (uint32_t index = 0U;
                 index < psic && index < XHCI_PROTOCOL_PSI_MAX; ++index) {
                /* The PSI dwords follow the capability's three dwords, so the
                   bound covers each of them too: a group whose declared PSI
                   count runs past the mapped window is read for as long as the
                   window allows and no further. */
                if (xhci->mmio_bytes != 0U &&
                    base + 12U + index * 4U + 4U > xhci->mmio_bytes) {
                    break;
                }
                psi[index] = xhci_read32(
                    xhci_capability(base + 12U + index * 4U), 0U);
                psi_count = index + 1U;
            }
            /*
             * The declared PSI count may exceed what this driver stores; the
             * group is still parsed with the ones that were read, and the
             * record publishes the declared count beside them.
             */
            if (xhci->protocol_count < XHCI_PROTOCOL_GROUPS_MAX &&
                xhci_protocol_capability(dword0, dword1, dword2,
                                         psi_count == 0U ? NULL : psi,
                                         psi_count,
                                         &xhci->protocols[xhci->protocol_count])) {
                ++xhci->protocol_count;
            }
        }
        /*
         * ITEM-453: the Next Capability Pointer is an offset in dwords from
         * the entry it sits in (Linux's xhci_find_next_ext_cap() adds it the
         * same way), and a pointer of zero ends the list.  Reading it as an
         * absolute offset - which the firmware-claim walk below used to do -
         * makes the last entry repeat for as long as the loop is allowed to
         * run, and QEMU's two-capability list published its USB3 group four
         * times before this was fixed.
         */
        if (next == 0U) {
            break;
        }
        offset += next;
    }
}

/* Claim the session's pool once and zero it for the controller that is coming
   up. The size the controller needs is known only here, because the device
   context base address array follows MaxSlots: `dcbaa_bytes` is
   xhci_dcbaa_bytes(), i.e. the (MaxSlots+1) pointers of xHCI 1.1 section 6.1,
   and it is placed at the end of the fixed layout. A controller whose array
   does not fit the pool is refused here rather than allowed to run off the end
   of it.

   ITEM-452: the pool belongs to the session, not to the machine. A visit that
   binds a keyboard or a pointer keeps its pool (and its controller) alive
   while the walk goes on to the next controller, which is what lets the K120
   on one controller and the receiver on another be bound at the same time. A
   released visit reuses its slot's existing pages. */
static bool xhci_dma_setup(uint32_t max_slots) {
    const size_t dcbaa_bytes = xhci_dcbaa_bytes(max_slots);
    const uint32_t slot_area =
        XHCI_DEVICE_CONTEXT_ENTRIES * (uint32_t)xhci->context_bytes;
    uint32_t offset;
    uint32_t pages;
    size_t pool_bytes;

    if (dcbaa_bytes == 0U || slot_area == 0U ||
        xhci->slots_enabled == 0U ||
        xhci->slots_enabled > XHCI_DRIVER_SLOTS) {
        return false;
    }
    if (xhci->scratchpad_count != 0U && xhci->scratchpad_page_bytes == 0U) {
        return false;
    }
    /*
     * ITEM-577: one slot's area is 32 context records (the slot context plus
     * the 31 endpoint contexts, xHCI 1.1 section 6.2.1), not one record.  The
     * areas are laid out first, then the scratchpad pointer array and the
     * scratchpad pages, each aligned so no two regions can share a byte.
     */
    xhci->slot_area_bytes = slot_area;
    offset = xhci_align_up(XHCI_DMA_FIXED_END, 64U);
    xhci->device_contexts_offset = offset;
    xhci->device_contexts_bytes = xhci->slots_enabled * slot_area;
    offset += xhci->device_contexts_bytes;
    offset = xhci_align_up(offset, 64U);
    xhci->scratchpad_array_offset = offset;
    xhci->scratchpad_array_bytes = xhci->scratchpad_count * 8U;
    offset += xhci->scratchpad_array_bytes;
    offset = xhci_align_up(offset, xhci->scratchpad_page_bytes != 0U
                                      ? xhci->scratchpad_page_bytes
                                      : 4096U);
    xhci->scratchpad_pages_offset = offset;
    offset += xhci->scratchpad_count * xhci->scratchpad_page_bytes;
    pool_bytes = xhci_align_up(offset, 4096U);
    if (pool_bytes > XHCI_DMA_MAX_BYTES) {
        return false;
    }
    pages = (uint32_t)(pool_bytes / 4096U);
    if (xhci->dma_physical != 0U && xhci->dma_pages < pages) {
        /* The reused pool is too small for this controller; give the pages
           back instead of running off the end of them. */
        (void)pmm_free_contiguous_pages(xhci->dma_physical, xhci->dma_pages);
        xhci->dma_physical = 0U;
        xhci->dma = NULL;
        xhci->dma_pages = 0U;
    }
    if (xhci->dma_physical == 0U) {
        xhci->dma_physical = pmm_alloc_contiguous_pages(pages);
        if (xhci->dma_physical == 0U) {
            return false;
        }
        xhci->dma = (uint8_t *)(uintptr_t)(xhci->dma_physical +
                                          xhci_machine.hhdm_offset);
        xhci->dma_pages = pages;
    }
    xhci->dcbaa_offset = XHCI_DMA_FIXED_BYTES;
    xhci->dcbaa_bytes = (uint32_t)dcbaa_bytes;
    xhci->pool_bytes = (uint32_t)pool_bytes;
    memset(xhci->dma, 0, pool_bytes);
    return true;
}

static bool xhci_controller_start(void) {
    uint32_t command;
    uint32_t hcsparams1;

    xhci->caplength =
        (uint8_t)xhci_read32(xhci_capability(XHCI_CAP_CAPLENGTH), 0U);
    /* Intel 7 Series uses 0x80. Check the field, alignment and mapped span,
       rather than treating a smaller capability block as the maximum. */
    if (!xhci_caplength_valid(xhci->caplength, xhci->mmio_bytes)) {
        xhci_set_status(XHCI_STATUS_MMIO_MAP_FAILED, "BAD_CAPLENGTH");
        /* Release must not access unvalidated operational registers. */
        xhci->regs = NULL;
        return false;
    }
    hcsparams1 = xhci_read32(xhci_capability(XHCI_CAP_HCSPARAMS1), 0U);
    xhci->max_slots = hcsparams1 & 0xFFU;
    xhci->max_ports = (hcsparams1 >> 24U) & 0xFFU;
    xhci->hcs_params2 = xhci_read32(xhci_capability(XHCI_CAP_HCSPARAMS2), 0U);
    xhci->scratchpad_count = xhci_scratchpad_count(xhci->hcs_params2);
    xhci->db_offset = xhci_read32(xhci_capability(XHCI_CAP_DBOFF), 0U) & ~0x3U;
    xhci->rt_offset = xhci_read32(xhci_capability(XHCI_CAP_RTSOFF), 0U) & ~0x1FU;
    xhci->context_size_64 =
        (xhci_read32(xhci_capability(XHCI_CAP_HCCPARAMS1), 0U) & 0x4U) != 0U;
    xhci->context_bytes =
        xhci->context_size_64 ? XHCI_CONTEXT_64 : XHCI_CONTEXT_32;
    xhci->page_size = xhci_read32(xhci_operational(XHCI_OP_PAGESIZE), 0U);
    /*
     * ITEM-407: the controller's own field limits decide this, not the size of
     * our table. HCSPARAMS1.MaxSlots and .MaxPorts are eight-bit fields, so a
     * controller that advertises 127 slots (the acceptance host's A520 xHCI)
     * or 14 ports is a conforming, usable controller: what it may not be is a
     * register window that answers with a value the field cannot hold, which is
     * a bad mapping or a bad read and is refused as such. The number of slots
     * this driver *enables* is bounded separately, by xhci_slots_enabled().
     */
    if (!xhci_max_slots_valid(xhci->max_slots) ||
        !xhci_max_ports_valid(xhci->max_ports)) {
        xhci_set_status(XHCI_STATUS_MMIO_MAP_FAILED, "BAD_HCSPARAMS");
        return false;
    }
    xhci->slots_enabled = xhci_slots_enabled(xhci->max_slots);
    /*
     * ITEM-577: the scratchpad pages must be pages the controller supports
     * (PAGESIZE, xHCI 1.1 section 5.4.3: bit n means 2^(n+12) bytes).  The
     * pool is 4 KiB pages, so a controller whose smallest supported page is
     * larger is refused by name instead of given pages it cannot use.  A
     * register of zero is invalid; Linux falls back to 4 KiB and so does this
     * driver.
     */
    xhci->scratchpad_page_bytes = 0U;
    if (xhci->scratchpad_count != 0U) {
        const uint32_t pagesize = xhci->page_size & 0xFFFFU;

        if (pagesize == 0U || (pagesize & 1U) != 0U) {
            xhci->scratchpad_page_bytes = 4096U;
        } else {
            xhci_set_status(XHCI_STATUS_DMA_UNAVAILABLE,
                            "SCRATCHPAD_PAGE_UNSUPPORTED");
            return false;
        }
    }
    xhci_read_protocol_groups();

    xhci_claim_from_firmware();

    /* ITEM-574: the Intel USB port routing, before the controller is halted
       and reset (Linux's early quirk does it at the same point).  It reads the
       firmware's four routing registers, writes only the switchable ports the
       EHCI companion does not expose, and verifies every write; see
       xhci_route.c. */
#ifndef HFOC_XHCI_MMIO_HOST_TEST
    xhci_route_apply();
#endif

    /* Halt, then reset, exactly as section 4.2 of the specification orders
       it. Both waits are bounded: a controller that never answers must not
       hold the boot. */
    command = xhci_read32(xhci_operational(XHCI_OP_USBCMD), 0U);
    xhci_write32(xhci_operational(XHCI_OP_USBCMD), 0U,
                 command & ~(XHCI_USBCMD_RS | XHCI_USBCMD_INTE));
    if (!xhci_wait_status(XHCI_USBSTS_HCH, XHCI_USBSTS_HCH,
                          XHCI_SPIN_HALT)) {
        xhci_set_status(XHCI_STATUS_HALT_TIMEOUT, "HCH_SET_TIMEOUT");
        return false;
    }
    if (!xhci_wait_status(XHCI_USBSTS_CNR, 0U, XHCI_SPIN_RESET)) {
        xhci_set_status(XHCI_STATUS_RESET_TIMEOUT, "CNR_TIMEOUT");
        return false;
    }
    command = xhci_read32(xhci_operational(XHCI_OP_USBCMD), 0U);
    xhci_write32(xhci_operational(XHCI_OP_USBCMD), 0U,
                 command | XHCI_USBCMD_HCRST);
    for (uint32_t spins = 0U; spins < XHCI_SPIN_RESET; ++spins) {
        const uint32_t current =
            xhci_read32(xhci_operational(XHCI_OP_USBCMD), 0U);

        if ((current & XHCI_USBCMD_HCRST) == 0U) {
            break;
        }
        __asm__ volatile ("pause");
    }
    if ((xhci_read32(xhci_operational(XHCI_OP_USBCMD), 0U) &
         XHCI_USBCMD_HCRST) != 0U) {
        xhci_set_status(XHCI_STATUS_RESET_TIMEOUT, "HCRST_TIMEOUT");
        return false;
    }
    if (!xhci_wait_status(XHCI_USBSTS_CNR, 0U, XHCI_SPIN_RESET)) {
        xhci_set_status(XHCI_STATUS_RESET_TIMEOUT, "CNR_AFTER_RESET_TIMEOUT");
        return false;
    }

    if (!xhci_dma_setup(xhci->max_slots)) {
        xhci_set_status(XHCI_STATUS_DMA_UNAVAILABLE, "NO_CONTIGUOUS_PAGES");
        return false;
    }

    /* Device context base address array, sized for the controller, and the
       device context table, sized for the slots this driver enables. Every
       entry the controller may use has to point at a device context before the
       array is published (xHCI 1.1 section 6.1), so the loop covers the enabled
       slots and stops there: the entries above MaxSlotsEn belong to slots the
       controller will never allocate. */
    xhci->dcbaa = (uint64_t *)xhci_dma_at(xhci->dcbaa_offset);
    xhci->dcbaa_physical = xhci_dma_physical(xhci->dcbaa_offset);
    xhci->device_contexts = xhci_dma_at(xhci->device_contexts_offset);
    xhci->device_contexts_physical =
        xhci_dma_physical(xhci->device_contexts_offset);
    xhci->input_context = xhci_dma_at(XHCI_OFF_INPUT_CONTEXT);
    xhci->input_context_physical = xhci_dma_physical(XHCI_OFF_INPUT_CONTEXT);
    for (uint32_t slot = 1U; slot <= xhci->slots_enabled; ++slot) {
        xhci->dcbaa[slot] = xhci->device_contexts_physical +
                           (uint64_t)(slot - 1U) * xhci->slot_area_bytes;
    }
    /*
     * ITEM-577: the scratchpad pointer array lives in DCBAA[0] and the pages
     * it names must exist before the controller is started (xHCI 1.1 section
     * 6.1, Linux's scratchpad_alloc()).  A controller that asks for zero
     * buffers leaves DCBAA[0] zero and this driver allocates nothing.
     */
    xhci->dcbaa0 = 0U;
    if (xhci->scratchpad_count != 0U) {
        uint64_t *array = (uint64_t *)xhci_dma_at(
            xhci->scratchpad_array_offset);
        const uint32_t page_bytes = xhci->scratchpad_page_bytes;

        xhci->scratchpad_array_physical =
            xhci_dma_physical(xhci->scratchpad_array_offset);
        for (uint32_t index = 0U; index < xhci->scratchpad_count; ++index) {
            const uint32_t page_offset = xhci->scratchpad_pages_offset +
                                         index * page_bytes;
            const uint64_t page_physical = xhci_dma_physical(page_offset);

            array[index] = page_physical;
            memset(xhci_dma_at(page_offset), 0, page_bytes);
        }
        xhci->dcbaa[0] = xhci->scratchpad_array_physical;
        xhci->dcbaa0 = xhci->scratchpad_array_physical;
    }

    /* Command ring, event ring and the segment table that describes it. */
    xhci_ring_init(&xhci->command_ring,
                   (struct xhci_trb *)xhci_dma_at(XHCI_OFF_COMMAND_RING),
                   xhci_dma_physical(XHCI_OFF_COMMAND_RING),
                   XHCI_RING_TRBS, 1U);
    xhci->event_ring =
        (struct xhci_trb *)xhci_dma_at(XHCI_OFF_EVENT_RING);
    xhci->event_ring_physical = xhci_dma_physical(XHCI_OFF_EVENT_RING);
    xhci->event_index = 0U;
    xhci->event_cycle = 1U;
    xhci->erst = (uint64_t *)xhci_dma_at(XHCI_OFF_ERST);
    xhci->erst_physical = xhci_dma_physical(XHCI_OFF_ERST);
    xhci->erst[0] = xhci->event_ring_physical;
    xhci->erst[1] = XHCI_EVENT_TRBS;

    xhci_write64(xhci_operational(XHCI_OP_DCBAAP), xhci->dcbaa_physical);
    xhci_write64(xhci_operational(XHCI_OP_CRCR),
                 xhci->command_ring.phys | XHCI_CRCR_RCS);
    /* ITEM-407: CONFIG.MaxSlotsEn is the number of slots this driver enables,
       never the controller's MaxSlots - that is the field whose size follows
       the number of slots actually used. */
    xhci_write32(xhci_operational(XHCI_OP_CONFIG), 0U, xhci->slots_enabled);

    /* Interrupter 0 is used as a polled event ring only: the interrupt is
       left disabled and nothing is routed (frozen IRQ scope). */
    xhci_write32(xhci_runtime(XHCI_RT_IR0 + XHCI_IR_IMAN), 0U, 0U);
    xhci_write32(xhci_runtime(XHCI_RT_IR0 + XHCI_IR_IMOD), 0U, 0U);
    xhci_write32(xhci_runtime(XHCI_RT_IR0 + XHCI_IR_ERSTSZ), 0U, 1U);
    xhci_write64(xhci_runtime(XHCI_RT_IR0 + XHCI_IR_ERSTBA),
                 xhci->erst_physical);
    xhci_write64(xhci_runtime(XHCI_RT_IR0 + XHCI_IR_ERDP),
                 xhci->event_ring_physical | XHCI_ERDP_EHB);

    command = xhci_read32(xhci_operational(XHCI_OP_USBCMD), 0U);
    xhci_write32(xhci_operational(XHCI_OP_USBCMD), 0U,
                 command | XHCI_USBCMD_RS);
    /*
     * ITEM-407: the run/stop bit is written and then read back, because
     * everything the port scan does depends on the controller actually running:
     * a halted controller keeps its port view but does not update it, so
     * believing a PORTSC read on one is exactly the "we are reading the wrong
     * thing" case the [XHCI] record has to be able to rule out. Both the
     * readback of USBCMD.RS and USBSTS.HCH are required, and both are
     * published in the record.
     */
    xhci->usbcmd = xhci_read32(xhci_operational(XHCI_OP_USBCMD), 0U);
    if ((xhci->usbcmd & XHCI_USBCMD_RS) == 0U) {
        xhci_set_status(XHCI_STATUS_HALT_TIMEOUT, "RS_NOT_SET");
        return false;
    }
    if (!xhci_wait_status(XHCI_USBSTS_HCH, 0U, XHCI_SPIN_RESET)) {
        xhci->usbsts = xhci_read32(xhci_operational(XHCI_OP_USBSTS), 0U);
        xhci_set_status(XHCI_STATUS_HALT_TIMEOUT, "RS_TIMEOUT");
        return false;
    }
    xhci->usbsts = xhci_read32(xhci_operational(XHCI_OP_USBSTS), 0U);
    if ((xhci->usbsts & XHCI_USBSTS_HCH) != 0U) {
        xhci_set_status(XHCI_STATUS_HALT_TIMEOUT, "HCH_SET_AFTER_RS");
        return false;
    }

    /* A No-Op command proves the command ring and the event ring work before
       any device work starts. */
    {
        const struct xhci_trb noop = xhci_trb_noop_command(1U);

        if (!xhci_command_ok(&noop, &xhci->detail)) {
            xhci_set_status(XHCI_STATUS_COMMAND_FAILED, xhci->detail);
            return false;
        }
    }
    return true;
}

/* The context addressing rule (which Add flag bit addresses which entry) lives
   in kernel/src/xhci_trb.c, where the host proof pins it. */
static size_t xhci_context_index(uint8_t dci) {
    return xhci_context_entry_index(dci);
}

static bool xhci_address_device_ring(struct xhci_ring *ep0_ring,
                                     uint8_t slot_id, uint8_t port,
                                     uint8_t speed,
                                     uint16_t ep0_max_packet) {
    uint8_t *input = xhci->input_context;
    const char *detail = NULL;
    struct xhci_trb trb;

    xhci_context_clear(input, XHCI_INPUT_CONTEXT_MAX_BYTES);
    xhci_input_control_context(
        input, xhci->context_bytes,
        xhci_context_flag(0U) | xhci_context_flag(1U), 0U);
    xhci_slot_context(
        input + xhci_context_offset(xhci->context_bytes,
                                    xhci_context_index(0U)),
        speed, 1U, port);
    xhci_endpoint_context(
        input + xhci_context_offset(xhci->context_bytes,
                                    xhci_context_index(1U)),
        XHCI_EP_TYPE_CONTROL, ep0_max_packet, 0U,
        ep0_ring->phys, 8U);
    trb = xhci_trb_address_device(1U, xhci->input_context_physical, slot_id);
    if (!xhci_command_ok(&trb, &detail)) {
        xhci_set_status(XHCI_STATUS_ENUMERATION_FAILED,
                        xhci_detail_port(port, "ADDRESS_DEVICE", detail));
        return false;
    }
    return true;
}

static bool xhci_evaluate_ep0(struct xhci_hid_device *device,
                              uint8_t slot_id, uint8_t port, uint8_t speed,
                              uint16_t ep0_max_packet) {
    uint8_t *input = xhci->input_context;
    struct xhci_trb trb;
    const char *detail = NULL;

    xhci_context_clear(input, XHCI_INPUT_CONTEXT_MAX_BYTES);
    xhci_input_control_context(
        input, xhci->context_bytes,
        xhci_context_flag(0U) | xhci_context_flag(1U), 0U);
    xhci_slot_context(
        input + xhci_context_offset(xhci->context_bytes,
                                    xhci_context_index(0U)),
        speed, 1U, port);
    xhci_endpoint_context(
        input + xhci_context_offset(xhci->context_bytes,
                                    xhci_context_index(1U)),
        XHCI_EP_TYPE_CONTROL, ep0_max_packet, 0U,
        xhci_device_ep0_ring(device)->phys, 8U);
    /*
     * ITEM-420: Evaluate Context is its own command (TRB type 13).  It is the
     * same TRB shape as Address Device, and the driver used to send exactly
     * that - an Address Device with the Block Set Address bit set, which the
     * specification does not define as Evaluate Context at all.  The acceptance
     * host answered it with completion code 19, a Context State Error: the
     * command was illegal for a slot that was already addressed.  The keyboard
     * then never reached its configuration and the local console stayed
     * read-only.
     */
    trb = xhci_trb_evaluate_context(1U, xhci->input_context_physical, slot_id);
    if (!xhci_command_ok(&trb, &detail)) {
        xhci_set_status(XHCI_STATUS_ENUMERATION_FAILED,
                        xhci_detail_port(port, "EVALUATE_CONTEXT", detail));
        return false;
    }
    return true;
}

static bool xhci_control_transfer_ring(
    struct xhci_ring *ep0_ring,
    uint32_t data_buffer,
    uint8_t slot_id,
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    uint16_t length,
    bool data_in,
    uint32_t *transferred,
    uint8_t *completion_code) {
    struct xhci_trb trb;
    uint32_t trb_index = 0U;
    uint64_t data_trb_physical = 0U;
    uint64_t status_trb_physical = 0U;
    uint8_t transfer_type;
    bool status_reached = false;
    uint8_t status_code = 0U;
    uint32_t received = 0U;
    bool data_seen = false;

    if (transferred != NULL) {
        *transferred = 0U;
    }
    /*
     * MOUSE-DIAG (2026-09-27): the optional outcome.  The callers that only
     * care whether the transfer worked pass NULL; the EP0 probe needs the
     * status-stage code itself, and the two sentinels tell "the driver could
     * not place the transfer" from "the device never answered it".
     */
    if (completion_code != NULL) {
        *completion_code = XHCI_EP0_NOT_SUBMITTED;
    }
    transfer_type = length == 0U
                        ? XHCI_TRT_NO_DATA
                        : (data_in ? XHCI_TRT_IN : XHCI_TRT_OUT);

    trb = xhci_trb_setup(1U, request_type, request, value, index, length,
                         transfer_type);
    if (!xhci_ring_push_index(ep0_ring, &trb,
                              &trb_index)) {
        return false;
    }
    if (length != 0U) {
        /*
         * ITEM-578: the caller names the buffer, and the parser that reads the
         * result reads that same buffer.  A Data Stage that writes where the
         * reader does not look makes a successful transfer look like a device
         * with an unusable descriptor.
         */
        trb = xhci_trb_data(1U, xhci_dma_physical(data_buffer), length,
                            data_in, true);
        if (!xhci_ring_push_index(ep0_ring, &trb,
                                  &trb_index)) {
            return false;
        }
        data_trb_physical =
            ep0_ring->phys +
            (uint64_t)trb_index * XHCI_TRB_BYTES;
    }
    /*
     * The Status Stage runs opposite to the Data Stage: a device-to-host data
     * stage is acknowledged by an OUT status stage, a host-to-device data
     * stage by an IN one, and a request with no data stage by the stage
     * opposite to its own direction.
     */
    trb = xhci_trb_status(1U,
                          length == 0U ? (request_type & 0x80U) == 0U
                                       : !data_in);
    if (!xhci_ring_push_index(ep0_ring, &trb,
                              &trb_index)) {
        return false;
    }
    status_trb_physical =
        ep0_ring->phys +
        (uint64_t)trb_index * XHCI_TRB_BYTES;

    xhci_barrier();
    xhci_write32(xhci_doorbell(slot_id), 0U, 1U);
    if (completion_code != NULL) {
        *completion_code = XHCI_EP0_NO_COMPLETION;
    }

    for (uint32_t spins = 0U; spins < XHCI_SPIN_TRANSFER; ++spins) {
        struct xhci_event event;

        if (!xhci_next_event(&event)) {
            __asm__ volatile ("pause");
            continue;
        }
        if (event.type != XHCI_TRB_TRANSFER_EVENT ||
            event.slot_id != slot_id || event.endpoint_id != 1U) {
            /*
             * ITEM-452: an event for another device that shares this
             * controller's ring - the other HID role - is decoded and its
             * device re-armed, not dropped: a dropped completion leaves
             * `transfer_pending` set and that device stays silent for the rest
             * of the boot.
             */
            (void)xhci_apply_event(&event);
            continue;
        }
        if (event.parameter == data_trb_physical &&
            data_trb_physical != 0U) {
            if (event.completion_code != XHCI_COMPLETION_SUCCESS &&
                event.completion_code != XHCI_COMPLETION_SHORT_PACKET) {
                xhci->detail =
                    xhci_completion_code_string(event.completion_code);
                if (completion_code != NULL) {
                    *completion_code = event.completion_code;
                }
                return false;
            }
            data_seen = true;
            received = length >= event.transfer_length
                           ? length - event.transfer_length
                           : 0U;
            continue;
        }
        if (event.parameter == status_trb_physical) {
            status_reached = true;
            status_code = event.completion_code;
            if (completion_code != NULL) {
                *completion_code = event.completion_code;
            }
            break;
        }
    }
    if (!status_reached) {
        xhci->detail = "TRANSFER_TIMEOUT";
        return false;
    }
    if (status_code != XHCI_COMPLETION_SUCCESS) {
        xhci->detail = xhci_completion_code_string(status_code);
        return false;
    }
    if (transferred != NULL) {
        *transferred = data_seen ? received : 0U;
    }
    return true;
}

/* ITEM-519 increment 2: the HID roles pass their own ring and data region;
   the storage device passes the storage ring and its bounce page. */
static bool xhci_control_transfer(
    struct xhci_hid_device *device,
    uint32_t data_buffer,
    uint8_t slot_id,
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    uint16_t length,
    bool data_in,
    uint32_t *transferred,
    uint8_t *completion_code) {
    return xhci_control_transfer_ring(
        xhci_device_ep0_ring(device), data_buffer, slot_id, request_type,
        request, value, index, length, data_in, transferred,
        completion_code);
}


static bool xhci_get_descriptor(
    struct xhci_hid_device *device,
    uint8_t slot_id,
    uint8_t descriptor_type,
    uint8_t descriptor_index,
    uint16_t length,
    uint32_t *received) {
    return xhci_control_transfer(
        device, xhci_device_descriptor_offset(device->role), slot_id, 0x80U,
        USB_REQUEST_GET_DESCRIPTOR,
        (uint16_t)(((uint16_t)descriptor_type << 8) | descriptor_index),
        0U, length, true, received, NULL);
}

static bool xhci_configure_endpoint(struct xhci_hid_device *device,
                                    uint8_t slot_id, uint8_t port, uint8_t dci,
                                    uint16_t max_packet, uint8_t interval) {
    uint8_t *input = xhci->input_context;
    const char *detail = NULL;
    /*
     * Add the slot context and the new interrupt endpoint. Endpoint 0 keeps
     * its Add flag clear: its transfer ring has already advanced, and adding
     * it again would push the controller's dequeue pointer back to the ring
     * base, where the retired entries carry the wrong cycle state.
     */
    /*
     * The Add Context flag for an endpoint is 1 << its device context index.
     * The context entry itself lives one slot further along, because index 0
     * of the input context is the Input Control Context and Endpoint 0 sits
     * between the slot context and the first data endpoint: the flag bit is
     * the DCI, the array index is the DCI plus one. QEMU's xhci model and the
     * specification agree on both halves.
     */
    const uint32_t add_flags =
        xhci_context_flag(0U) | xhci_context_flag(dci);
    struct xhci_trb trb;

    /*
     * ITEM-579: adding a second role's endpoint must not drop the first one -
     * the slot context publishes the highest configured device context index,
     * and both roles' endpoints are addressed by their own DCI.
     */
    if (device->path != NULL && dci > device->path->max_dci) {
        device->path->max_dci = dci;
    }
    xhci_context_clear(input, XHCI_INPUT_CONTEXT_MAX_BYTES);
    xhci_input_control_context(input, xhci->context_bytes, add_flags, 0U);
    xhci_slot_context(
        input + xhci_context_offset(xhci->context_bytes,
                                    xhci_context_index(0U)),
        device->speed,
        device->path != NULL ? device->path->max_dci : dci, device->port);
    xhci_endpoint_context(
        input + xhci_context_offset(xhci->context_bytes,
                                    xhci_context_index(1U)),
        XHCI_EP_TYPE_CONTROL, 8U, 0U,
        xhci_device_ep0_ring(device)->phys, 8U);
    xhci_endpoint_context(
        input + xhci_context_offset(xhci->context_bytes,
                                    xhci_context_index(dci)),
        XHCI_EP_TYPE_INTERRUPT_IN, max_packet, interval,
        xhci->interrupt_rings[device->role].phys, max_packet);
    trb = xhci_trb_configure_endpoint(1U, xhci->input_context_physical,
                                      slot_id);
    if (!xhci_command_ok(&trb, &detail)) {
        xhci_set_status(XHCI_STATUS_ENUMERATION_FAILED,
                        xhci_detail_port(port, "CONFIGURE_ENDPOINT", detail));
        return false;
    }
    return true;
}

static bool xhci_disable_slot(uint8_t slot_id) {
    const struct xhci_trb disable = xhci_trb_disable_slot(1U, slot_id);
    const char *detail = NULL;

    return xhci_command_ok(&disable, &detail);
}

static bool xhci_submit_interrupt_transfer(struct xhci_hid_device *device) {
    struct xhci_trb trb;
    uint8_t slot;

    /*
     * ITEM-469: the queue is bounded by the buffer page, not by the ring.  A
     * full queue is not an error - the caller tops the depth up, it does not
     * insist on a fixed number - so a saturated device simply stops here.
     */
    if (device->armed_count >= XHCI_REPORT_SLOTS) {
        return false;
    }
    slot = (uint8_t)((device->armed_head + device->armed_count) %
                     XHCI_REPORT_SLOTS);
    /*
     * ITEM-438: a boot keyboard answers with the 8-byte boot report; a
     * report-protocol keyboard answers with its own report, whose length is
     * what its report descriptor declared.  Either way the transfer asks for
     * one whole report, so a completion is classified against the same number.
     * ITEM-448: the same rule for the pointer, whose own report length came
     * from the boot protocol or from its report descriptor.
     */
    trb = xhci_trb_interrupt_in(1U,
                                xhci_dma_physical(
                                    xhci_report_buffer_offset(device, slot)),
                                device->report_length);
    if (!xhci_ring_push(&xhci->interrupt_rings[device->role], &trb)) {
        return false;
    }
    device->armed_slots[(device->armed_head + device->armed_count) %
                        XHCI_REPORT_SLOTS] = slot;
    ++device->armed_count;
    device->transfer_pending = true;
    /*
     * ITEM-461: how long the completed report waited before this transfer was
     * armed again.  ITEM-469: the re-arm now runs inside the completion's own
     * event handler, so this measures the handler's distance from the
     * completion, not a whole service pass.
     * ITEM-467: a re-arm that follows a completion is counted as `reposted`, so
     * "the completion was handled and the transfer was put back in flight" is a
     * number the record publishes instead of an assumption.
     */
    if (device->transfer_complete_ticks != 0U) {
        const uint64_t elapsed_ms =
            (timer_ticks() - device->transfer_complete_ticks) * 1000U /
            HFOC_TIMER_HZ;

        if (elapsed_ms > device->repost_ms_max) {
            device->repost_ms_max = (uint32_t)elapsed_ms;
        }
        device->transfer_complete_ticks = 0U;
        ++device->repost_count;
    }
    /* ITEM-468: the TRB this driver just armed asks for a short-packet event
       (`xhci_trb_interrupt_in()` sets ISP); the diagnostic record states it as
       a fact instead of leaving a reader to read the encoder. */
    device->last_transfer_isp = true;
    ++device->transfer_count;
    /* ITEM-479: a fresh arm is progress; the watchdog measures from it. */
    device->watchdog_progress_ticks = timer_ticks();
    xhci_barrier();
    xhci_write32(xhci_doorbell(device->slot_id), 0U, device->dci);
    return true;
}

/*
 * ITEM-469: keep the role's depth in flight.  Called after every completion
 * (so the endpoint never waits for the pass to end) and once more at the end of
 * the pass, which only tops up what the pass did not see.
 *
 * ITEM-470: the depth is per role.  The keyboard keeps three; the mouse, whose
 * 1 ms endpoint can hand over a report per millisecond, keeps sixteen - enough
 * for a whole loop turn and backed by the sixteen-slot report pool and the
 * deeper interrupt ring.
 *
 * ITEM-473 (СРОЧНО-23): `ARM <n>` can override the mouse depth for one run
 * (the compiled sixteen stays the default, and the clamp keeps the request
 * inside the interrupt ring).  The keyboard's three are fixed: it never showed
 * the burst the pointer's 1 ms endpoint does.
 */
static uint8_t xhci_mouse_arm_depth_override;
static uint8_t xhci_role_arm_depth(uint8_t role) {
    if (role == XHCI_HID_ROLE_MOUSE && xhci_mouse_arm_depth_override != 0U) {
        return xhci_mouse_arm_depth_override;
    }
    return role == XHCI_HID_ROLE_MOUSE ? XHCI_MOUSE_ARM_DEPTH
                                       : XHCI_ARM_DEPTH;
}

/* ITEM-473: the console's `ARM` handle.  A zero request is clamped to the
   minimum rather than left as "no override", so `ARM 0` still sets a depth. */
void xhci_set_mouse_arm_depth(uint32_t requested) {
    xhci_mouse_arm_depth_override = xhci_arm_depth_clamp(requested);
}

uint32_t xhci_mouse_arm_depth(void) {
    return xhci_role_arm_depth(XHCI_HID_ROLE_MOUSE);
}

static void xhci_top_up_device(struct xhci_hid_device *device) {
    while (device->ready &&
           device->armed_count < xhci_role_arm_depth(device->role)) {
        if (!xhci_submit_interrupt_transfer(device)) {
            ++device->error_count;
            break;
        }
    }
}

/*
 * ITEM-479 (СРОЧНО-28, round 17): the stalled-endpoint watchdog.
 *
 * The round-16 hardware run ended with the pointer silent for the last 95 s
 * (`mouse_last_ms=95410`) while the driver held `armed=16`, the endpoint
 * context read Running, every completion code was SUCCESS and
 * `no_trb=0`/`mse=0`/`ringfull=0`.  The machine had no way to tell a pointer
 * nobody moved from one whose transfers stopped completing, and nothing
 * recovered the latter.  This watchdog is that missing piece:
 *
 *   * progress is the tick of the last completion or the last fresh arm; the
 *     run-time state lives in `watchdog_progress_ticks`;
 *   * `xhci_watchdog_due()` (the pure half, host-proven) fires only when the
 *     role is ready, holds at least one armed transfer and has waited longer
 *     than XHCI_WATCHDOG_SILENCE_MS;
 *   * one record per silence episode (`[INPUT-WATCHDOG]`) names the role, the
 *     slot, the endpoint, the depth and the wait, and the recovery re-arms and
 *     kicks the endpoint, so a stalled pointer is not dead for the rest of the
 *     run.
 *
 * An idle device legitimately completes nothing, so the watchdog costs one
 * record and one re-arm per silence episode, not one per pass.  That is the
 * trade the owner asked for: a record that names the silence is better than a
 * pointer that can only be read as "somebody stopped moving it".
 */
static void xhci_watchdog_publish(const struct xhci_hid_device *device,
                                  uint32_t silence_ms, bool active,
                                  bool escalated, uint32_t recover_us) {
    const uint64_t flags = serial_sequence_begin();

    console_journal_begin();
    serial_write("[INPUT-WATCHDOG] role=");
    console_record_text("[INPUT-WATCHDOG] role=");
    if (device->role == XHCI_HID_ROLE_MOUSE) {
        serial_write("mouse");
        console_record_text("mouse");
    } else {
        serial_write("keyboard");
        console_record_text("keyboard");
    }
    serial_write(" slot=");
    console_record_text(" slot=");
    serial_write_u64(device->slot_id);
    console_record_u64(device->slot_id);
    serial_write(" ep=");
    console_record_text(" ep=");
    serial_write_u64(device->dci);
    console_record_u64(device->dci);
    serial_write(" armed=");
    console_record_text(" armed=");
    serial_write_u64(device->armed_count);
    console_record_u64(device->armed_count);
    serial_write(" silent_ms=");
    console_record_text(" silent_ms=");
    serial_write_u64(silence_ms);
    console_record_u64(silence_ms);
    serial_write(" count=");
    console_record_text(" count=");
    serial_write_u64(device->watchdog_count);
    console_record_u64(device->watchdog_count);
    /*
     * ITEM-482: was the device in use when it went quiet, how many reports a
     * second was it producing, what did the recovery cost, and did the pure
     * rule call for the stronger reset?  `active` is what separates "nobody
     * moved the mouse" from "the mouse froze while it was being moved"; the
     * rest makes the recovery the same number as the silence.
     */
    serial_write(" active=");
    console_record_text(" active=");
    serial_write_u64(active ? 1U : 0U);
    console_record_u64(active ? 1U : 0U);
    serial_write(" rate=");
    console_record_text(" rate=");
    serial_write_u64(device->rate_per_second);
    console_record_u64(device->rate_per_second);
    serial_write(" recover_us=");
    console_record_text(" recover_us=");
    serial_write_u64(recover_us);
    console_record_u64(recover_us);
    serial_write(" action=REARM");
    console_record_text(" action=REARM");
    serial_write(" escalate=");
    console_record_text(" escalate=");
    serial_write_u64(escalated ? 1U : 0U);
    console_record_u64(escalated ? 1U : 0U);
    serial_write("\n");
    console_record_text("\n");
    serial_sequence_end(flags);
    console_journal_end();
}

/*
 * ITEM-579: what a Halted endpoint means and how it is repaired.
 *
 * A transfer that completes with a USB transaction error (or any code that
 * stops the endpoint) leaves the endpoint in the Halted state: the controller
 * will not execute another TRB for it, so arming more TRBs and ringing the
 * doorbell - what the watchdog and the error path used to do - changes nothing.
 * The repair is the two commands xHCI 1.1 defines for it: Reset Endpoint moves
 * the endpoint to Stopped, and Set TR Dequeue Pointer republishes the ring
 * position software is about to re-arm.  The old TRBs are discarded (the halted
 * endpoint never executed them), so the ring starts over at cycle 1 and the
 * command points the controller at its base.  The first fatal case is kept: the
 * completion code and the endpoint state the error path read.
 */
#define XHCI_EP_STATE_RUNNING 1U
#define XHCI_EP_STATE_HALTED 2U

static void xhci_publish_endpoint_action(const struct xhci_hid_device *device,
                                         const char *action,
                                         const char *detail) {
    const uint64_t flags = serial_sequence_begin();

    console_journal_begin();
    serial_write("[XHCI-EP] role=");
    console_record_text("[XHCI-EP] role=");
    if (device->role == XHCI_HID_ROLE_MOUSE) {
        serial_write("MOUSE");
        console_record_text("MOUSE");
    } else {
        serial_write("KEYBOARD");
        console_record_text("KEYBOARD");
    }
    serial_write(" slot=");
    console_record_text(" slot=");
    serial_write_u64(device->slot_id);
    console_record_u64(device->slot_id);
    serial_write(" dci=");
    console_record_text(" dci=");
    serial_write_u64(device->dci);
    console_record_u64(device->dci);
    serial_write(" action=");
    console_record_text(" action=");
    serial_write(action);
    console_record_text(action);
    serial_write(" last=");
    console_record_text(" last=");
    serial_write_u64(device->last_completion_code);
    console_record_u64(device->last_completion_code);
    serial_write(" epstate=");
    console_record_text(" epstate=");
    serial_write(xhci_endpoint_state_string(device->endpoint_state));
    console_record_text(xhci_endpoint_state_string(device->endpoint_state));
    serial_write(" halted=");
    console_record_text(" halted=");
    serial_write_u64(device->halted_count);
    console_record_u64(device->halted_count);
    serial_write(" restarts=");
    console_record_text(" restarts=");
    serial_write_u64(device->endpoint_restarts);
    console_record_u64(device->endpoint_restarts);
    if (detail != NULL && detail[0] != '\0') {
        serial_write(" detail=");
        console_record_text(" detail=");
        serial_write(detail);
        console_record_text(detail);
    }
    serial_write("\n");
    console_record_text("\n");
    serial_sequence_end(flags);
    console_journal_end();
}

/*
 * Rebuild the role's interrupt ring and republish its position.  Returns false
 * when the controller refused either command - the caller then takes the role
 * out of readiness instead of re-arming an endpoint that cannot run.
 */
static bool xhci_endpoint_restart(struct xhci_hid_device *device) {
    struct xhci_ring *ring;
    struct xhci_trb reset;
    struct xhci_trb dequeue;
    const char *detail = NULL;

    if (device == NULL || device->slot_id == 0U || device->dci < 2U) {
        return false;
    }
    ring = &xhci->interrupt_rings[device->role];
    xhci_ring_init(ring,
                   (struct xhci_trb *)xhci_dma_at(
                       xhci_device_interrupt_ring_offset(device->role)),
                   xhci_dma_physical(
                       xhci_device_interrupt_ring_offset(device->role)),
                   XHCI_INTERRUPT_RING_TRBS, 1U);
    device->armed_count = 0U;
    device->armed_head = 0U;
    device->transfer_pending = false;
    reset = xhci_trb_reset_endpoint(1U, device->slot_id, device->dci);
    if (!xhci_command_ok(&reset, &detail)) {
        xhci_set_status(XHCI_STATUS_ENUMERATION_FAILED,
                        xhci_detail_port(device->port, "RESET_ENDPOINT",
                                         detail));
        return false;
    }
    dequeue = xhci_trb_set_tr_dequeue(1U, device->slot_id, device->dci,
                                      ring->phys);
    if (!xhci_command_ok(&dequeue, &detail)) {
        xhci_set_status(XHCI_STATUS_ENUMERATION_FAILED,
                        xhci_detail_port(device->port, "SET_TR_DEQUEUE",
                                         detail));
        return false;
    }
    device->endpoint_state = XHCI_EP_STATE_RUNNING;
    device->consecutive_errors = 0U;
    ++device->endpoint_restarts;
    return true;
}

static void xhci_endpoint_recover(struct xhci_hid_device *device) {
    /*
     * ITEM-579: the repair depends on the endpoint state.  A Running endpoint
     * with armed transfers is topped up and kicked - the case the motivating
     * stalls published.  A Halted endpoint cannot execute anything, so the
     * watchdog issues the two repair commands and only re-arms after they
     * succeeded; when they did not, the role stops pretending to be alive.
     */
    device->endpoint_state = (uint8_t)xhci_device_endpoint_state(device);
    if (device->endpoint_state == XHCI_EP_STATE_HALTED) {
        ++device->halted_count;
        if (!xhci_endpoint_restart(device)) {
            device->ready = false;
            device->transfer_pending = false;
            xhci_publish_endpoint_action(device, "LOST", xhci->detail);
            if (device->role == XHCI_HID_ROLE_KEYBOARD) {
                xhci_note_keyboard_lost(XHCI_STATUS_KEYBOARD_LOST,
                                        "ENDPOINT_LOST");
            }
            return;
        }
        xhci_publish_endpoint_action(device, "RESET", NULL);
    }
    xhci_top_up_device(device);
    xhci_barrier();
    xhci_write32(xhci_doorbell(device->slot_id), 0U, device->dci);
}

/* The two masks the controller/role diagnostics decode: the status bits that
   mean the host controller reported an error, and the interrupter's enable
   bit.  They live here because the stall snapshot reads the same registers. */
#define XHCI_DIAG_USBSTS_ERROR (0x00000004U | 0x00001000U)
#define XHCI_DIAG_IMAN_IE 0x00000002U

/*
 * MOUSE-DIAG (owner task 2026-09-27): the stall snapshot.
 *
 * The two hardware runs that ended with a silent pointer left the same hole:
 * `mouse_armed=16`, the endpoint context Running, every completion code
 * SUCCESS, `mse=0`, `no_trb=0`, `ringfull=0` - and no report for the rest of
 * the run.  The journal could not say whether the device stopped sending (A)
 * or the controller stopped delivering events (B), because none of the three
 * places that separate the cases was on the disk at the moment of the stall:
 * the controller's event-ring dequeue pointer and interrupter registers, the
 * endpoint's own counters, and the PORTSC of the port the device sits on.
 *
 * These readers take those values, and `xhci_silence_publish()` writes them as
 * bounded records through the journal-only funnel.  They are read once per
 * silence episode - the watchdog's own first-detection point - and never per
 * loop turn, and nothing here writes a register: the diagnostic must not be
 * able to change the state it is describing.
 */
static uint32_t xhci_device_endpoint_state(
    const struct xhci_hid_device *device);

static struct xhci_session *xhci_session_for_device(
    const struct xhci_hid_device *device) {    for (uint32_t slot = 0U; slot < XHCI_SESSION_SLOTS; ++slot) {
        struct xhci_session *session = &xhci_sessions[slot];

        if (!session->in_use) {
            continue;
        }
        for (uint32_t role = 0U; role < XHCI_HID_DEVICES; ++role) {
            if (&session->devices[role] == device) {
                return session;
            }
        }
    }
    return NULL;
}

static void xhci_silence_read_controller(
    struct xhci_session *session,
    struct xhci_silence_controller *controller) {
    memset(controller, 0, sizeof(*controller));
    if (session == NULL || session->regs == NULL) {
        return;
    }
    controller->bus = session->location.bus;
    controller->device = session->location.device;
    controller->function = session->location.function;
    controller->usbcmd = xhci_read32(xhci_operational(XHCI_OP_USBCMD), 0U);
    controller->usbsts = xhci_read32(xhci_operational(XHCI_OP_USBSTS), 0U);
    controller->crcr = xhci_diag_read64(xhci_operational(XHCI_OP_CRCR));
    controller->erdp =
        xhci_diag_read64(xhci_runtime(XHCI_RT_IR0 + XHCI_IR_ERDP));
    controller->erstba =
        xhci_diag_read64(xhci_runtime(XHCI_RT_IR0 + XHCI_IR_ERSTBA));
    controller->iman = xhci_read32(xhci_runtime(XHCI_RT_IR0 + XHCI_IR_IMAN), 0U);
    controller->imod = xhci_read32(xhci_runtime(XHCI_RT_IR0 + XHCI_IR_IMOD), 0U);
    controller->error = controller->usbsts & XHCI_DIAG_USBSTS_ERROR;
    controller->host_events = session->controller_event_count;
    controller->ringfull = session->event_ring_full_events;
}

/* Every port the controller declared, or as many as the evidence buffer holds.
   The mouse's own port is read from the device that is bound to it; a device
   the driver cannot place leaves `port == 0` and the record says so. */
static size_t xhci_silence_read_ports(
    struct xhci_session *session,
    uint32_t *ports,
    size_t capacity) {
    size_t count = 0U;

    if (session == NULL || session->regs == NULL || ports == NULL) {
        return 0U;
    }
    if (session->max_ports < capacity) {
        capacity = session->max_ports;
    }
    for (size_t index = 0U; index < capacity; ++index) {
        ports[index] =
            xhci_read32(xhci_portsc((uint32_t)(index + 1U)), 0U);
        if (ports[index] == 0U) {
            /* A port that reads as zero was not powered or not present; the
               raw value is still published, so the count stays the real one. */
        }
        ++count;
    }
    return count;
}

/*
 * MOUSE-DIAG (owner task 2026-09-27, second step): one read-only EP0 probe per
 * silence episode.
 *
 * `hw13` left one question open: with the port connected and enabled and the
 * controller running, did the *device* stop answering, or did its interrupt
 * pipe alone go quiet?  A single standard GET_STATUS on the same slot's EP0
 * answers it without touching anything the contract protects: the probe issues
 * one control transfer, writes no register of its own, resets nothing, changes
 * no address, re-arms nothing and is bounded by the driver's own transfer spin
 * budget.  The state before and after is published beside the result, so the
 * record shows what the probe observed and not only what it asked.
 */
static uint32_t xhci_device_slot_state(const struct xhci_hid_device *device) {
    if (device == NULL || !device->ready || device->slot_id == 0U ||
        xhci->device_contexts == NULL || xhci->context_bytes == 0U) {
        return 0U;
    }
    return xhci_slot_state(
        xhci->device_contexts +
            (size_t)(device->slot_id - 1U) * xhci->slot_area_bytes,
        xhci->context_bytes);
}

static uint32_t xhci_probe_portsc(struct xhci_session *session,
                                  const struct xhci_hid_device *device) {
    if (session == NULL || device == NULL || device->port == 0U ||
        device->port > session->max_ports) {
        return 0U;
    }
    return xhci_read32(xhci_portsc(device->port), 0U);
}

static uint64_t xhci_probe_erdp(void) {
    return xhci_diag_read64(xhci_runtime(XHCI_RT_IR0 + XHCI_IR_ERDP));
}

static void xhci_ep0_probe_publish(struct xhci_hid_device *device) {
    struct xhci_session *session = xhci_session_for_device(device);
    struct xhci_session *const caller = xhci;
    struct xhci_ep0_probe probe;
    char line[XHCI_EP0_PROBE_RECORD_BYTES];
    size_t length;
    uint32_t transferred = 0U;
    uint8_t control_code = XHCI_EP0_NOT_SUBMITTED;
    uint64_t started;

    if (session == NULL || device == NULL || !device->ready) {
        return;
    }
    xhci = session;
    memset(&probe, 0, sizeof(probe));
    probe.ticks = timer_ticks();
    probe.slot = device->slot_id;
    probe.dci = device->dci;
    /* bmRequestType 0x80 (device-to-host, standard, device), bRequest 0
       (GET_STATUS), wValue 0, wIndex 0, two bytes of device status. */
    probe.request = 0x00U;
    probe.endpoint_state_before = xhci_device_endpoint_state(device);
    probe.slot_state_before = xhci_device_slot_state(device);
    probe.portsc_before = xhci_probe_portsc(session, device);
    probe.usbsts_before = xhci_read32(xhci_operational(XHCI_OP_USBSTS), 0U);
    probe.erdp_before = xhci_probe_erdp();

    started = timer_cycles();
    (void)xhci_control_transfer(device,
                                xhci_device_descriptor_offset(device->role),
                                device->slot_id, 0x80U, 0x00U, 0U, 0U, 2U,
                                true, &transferred, &control_code);
    probe.elapsed_us = timer_cycles_to_us(timer_cycles() - started);
    probe.completion_code = control_code;
    probe.submitted =
        control_code == XHCI_EP0_NOT_SUBMITTED ? 0U : 1U;

    probe.endpoint_state_after = xhci_device_endpoint_state(device);
    probe.slot_state_after = xhci_device_slot_state(device);
    probe.portsc_after = xhci_probe_portsc(session, device);
    probe.usbsts_after = xhci_read32(xhci_operational(XHCI_OP_USBSTS), 0U);
    probe.erdp_after = xhci_probe_erdp();

    length = xhci_format_ep0_probe_record(line, sizeof(line), &probe);
    if (length > 0U) {
        console_publish_journal(line, length);
    }
    length = xhci_format_ep0_state_record(line, sizeof(line), &probe);
    if (length > 0U) {
        console_publish_journal(line, length);
    }
    xhci = caller;
}

/*
 * MOUSE-DIAG step 3 (owner task 2026-09-27): the endpoint and ring state no
 * record carried.
 *
 * The stall evidence - ERDP frozen, EP0 `code=254`, PORTSC `0x00000603`, an
 * endpoint context that reads Running with sixteen transfers armed - says the
 * controller stopped delivering completions, but not whether it stopped
 * *fetching* the armed TRBs.  The endpoint context's TR Dequeue Pointer and
 * its cycle state, the transfer ring's base, the producer's enqueue position
 * and its cycle are what separate those two cases, and none of them was ever
 * published.  This snapshot reads them and writes one bounded record; it does
 * not write a register, kick the endpoint, re-arm anything or touch the
 * context.
 */
static void xhci_ep_state_publish(uint8_t role) {
    struct xhci_hid_device *device = xhci_device(role);
    struct xhci_session *session = xhci_session_for_device(device);
    struct xhci_session *const caller = xhci;
    struct xhci_ep_state state;
    char record[XHCI_EP_STATE_RECORD_BYTES];
    size_t length;

    if (device == NULL || session == NULL || !device->ready) {
        return;
    }
    xhci = session;
    memset(&state, 0, sizeof(state));
    state.role = role;
    state.endpoint_address = device->endpoint_address;
    state.dci = device->dci;
    state.endpoint_state = xhci_device_endpoint_state(device);
    state.ring_base = xhci->interrupt_rings[role].phys;
    state.enqueue = xhci->interrupt_rings[role].enqueue;
    state.ring_trbs = xhci->interrupt_rings[role].trb_count;
    state.producer_cycle = xhci->interrupt_rings[role].cycle;
    if (device->slot_id != 0U && xhci->device_contexts != NULL &&
        xhci->context_bytes != 0U) {
        uint64_t dequeue = 0U;
        uint32_t dequeue_cycle = 0U;

        if (xhci_endpoint_dequeue(
                xhci->device_contexts +
                    (size_t)(device->slot_id - 1U) * xhci->slot_area_bytes,
                xhci->context_bytes, device->dci, &dequeue, &dequeue_cycle)) {
            state.dequeue_pointer = dequeue;
            state.dequeue_cycle = dequeue_cycle;
        }
    }
    state.armed = device->armed_count;
    state.pending = device->transfer_pending ? 1U : 0U;
    state.reposted = device->repost_count;
    state.last_completion_code = device->last_completion_code;
    state.last_completion_ms =
        xhci_service_gap_ms(0U, device->last_report_ticks, timer_ticks(),
                            HFOC_TIMER_HZ);
    length = xhci_format_ep_state_record(record, sizeof(record), &state);
    if (length > 0U) {
        console_publish_journal(record, length);
    }
    length = xhci_format_ep_state_tail_record(record, sizeof(record), &state);
    if (length > 0U) {
        console_publish_journal(record, length);
    }
    xhci = caller;
}

static uint64_t xhci_silence_publish(
    struct xhci_hid_device *device,
    uint32_t silence_ms) {
    struct xhci_session *session = xhci_session_for_device(device);
    struct xhci_session *const caller = xhci;
    char line[XHCI_SILENCE_RECORD_BYTES];
    size_t length;
    uint64_t erdp_before = 0U;

    if (session == NULL) {
        return 0U;
    }
    xhci = session;
    {
        struct xhci_silence_time time;
        struct xhci_silence_endpoint endpoint;
        struct xhci_silence_controller controller;

        memset(&time, 0, sizeof(time));
        memset(&endpoint, 0, sizeof(endpoint));
        time.ticks = timer_ticks();
        /*
         * MOUSE-DIAG (2026-09-27, fix): `last_report_ticks`, not
         * `transfer_complete_ticks`.  The latter measures the completion-to-
         * re-arm wait and is cleared by the re-arm itself, so at a snapshot
         * taken a second after the last report it is always zero - the `hw13`
         * journal read `since_completion_ms=0 last_completion_ticks=0` while
         * the device had answered 13471 times.  `last_report_ticks` is the
         * uptime of the last completion the device answered with and is never
         * cleared, which is the clock this record means.
         */
        time.since_completion_ms =
            xhci_service_gap_ms(0U, device->last_report_ticks,
                                timer_ticks(), HFOC_TIMER_HZ);
        time.since_report_ms = silence_ms;
        endpoint.slot = device->slot_id;
        endpoint.dci = device->dci;
        endpoint.endpoint_state = xhci_device_endpoint_state(device);
        endpoint.armed = device->armed_count;
        endpoint.pending = device->transfer_pending ? 1U : 0U;
        endpoint.reposted = device->repost_count;
        endpoint.completed = device->transfer_count;
        endpoint.reports = device->report_count;
        endpoint.last_completion_code = device->last_completion_code;
        endpoint.last_completion_ticks = device->last_report_ticks;
        length = xhci_format_silence_record(line, sizeof(line), &time, &endpoint);
        if (length > 0U) {
            console_publish_journal(line, length);
        }
        xhci_silence_read_controller(session, &controller);
        length = xhci_format_silence_controller_record(line, sizeof(line),
                                                       &controller);
        if (length > 0U) {
            console_publish_journal(line, length);
        }
        {
            uint32_t ports[XHCI_PORT_EVIDENCE_MAX];
            size_t count = xhci_silence_read_ports(session, ports,
                                                   XHCI_PORT_EVIDENCE_MAX);

            length = xhci_format_silence_port_record(
                line, sizeof(line), session->location.bus,
                session->location.device, session->location.function,
                device->port, ports, count);
            if (length > 0U) {
                console_publish_journal(line, length);
            }
        }
        erdp_before = xhci_diag_read64(
            xhci_runtime(XHCI_RT_IR0 + XHCI_IR_ERDP));
    }
    xhci = caller;
    return erdp_before;
}

/* The same controller and port, read once after the watchdog's single re-arm
   and kick: the record says whether the dequeue pointer moved, so "the
   recovery did not restart the events" is a number and not an inference. */
static void xhci_silence_publish_after(
    struct xhci_hid_device *device,
    uint32_t silence_ms,
    uint64_t erdp_before) {
    struct xhci_session *session = xhci_session_for_device(device);
    struct xhci_session *const caller = xhci;
    char line[XHCI_SILENCE_RECORD_BYTES];
    size_t length;

    if (session == NULL) {
        return;
    }
    xhci = session;
    {
        const uint64_t erdp_after =
            xhci_diag_read64(xhci_runtime(XHCI_RT_IR0 + XHCI_IR_ERDP));
        const uint32_t usbcmd =
            xhci_read32(xhci_operational(XHCI_OP_USBCMD), 0U);
        const uint32_t usbsts =
            xhci_read32(xhci_operational(XHCI_OP_USBSTS), 0U);
        const uint32_t port = device->port;
        const uint32_t portsc =
            port != 0U && port <= session->max_ports
                ? xhci_read32(xhci_portsc(port), 0U)
                : 0U;

        length = xhci_format_silence_after_record(
            line, sizeof(line), timer_ticks(), silence_ms, erdp_before,
            erdp_after, usbcmd, usbsts, portsc);
        if (length > 0U) {
            console_publish_journal(line, length);
        }
    }
    xhci = caller;
}

static void xhci_watchdog_device(struct xhci_hid_device *device) {
    const uint64_t now = timer_ticks();
    uint32_t silence_ms;
    bool active;
    bool escalated;
    uint64_t silence_erdp = 0U;

    if (device == NULL) {
        return;
    }
    /*
     * ITEM-482: the report rate over a one-second window.  The window closes
     * even while the device is silent, so a rate of zero is a measured zero,
     * not an absent number.
     */
    if (device->rate_window_ticks == 0U) {
        device->rate_window_ticks = now;
        device->rate_window_reports = device->report_count;
    } else if (now >= device->rate_window_ticks &&
               now - device->rate_window_ticks >= HFOC_TIMER_HZ) {
        device->rate_per_second = xhci_report_rate_per_second(
            device->report_count - device->rate_window_reports,
            (now - device->rate_window_ticks) * 1000U / HFOC_TIMER_HZ);
        device->rate_window_ticks = now;
        device->rate_window_reports = device->report_count;
    }

    if (!device->ready || device->armed_count == 0U) {
        device->watchdog_reported = false;
        device->watchdog_progress_ticks = now;
        return;
    }
    silence_ms = xhci_watchdog_silence_ms(device->watchdog_progress_ticks,
                                          now, HFOC_TIMER_HZ);
    if (!xhci_watchdog_due(true, device->armed_count, silence_ms,
                           XHCI_WATCHDOG_SILENCE_MS) ||
        device->watchdog_reported) {
        return;
    }
    device->watchdog_reported = true;
    ++device->watchdog_count;
    if (silence_ms > device->watchdog_silence_ms_max) {
        device->watchdog_silence_ms_max = silence_ms;
    }
    /*
     * MOUSE-DIAG (2026-09-27): this is the first moment a silence episode is
     * known, so it is where the pointer's snapshot belongs - one set of records
     * per episode, before the recovery below touches anything, and only for the
     * pointer (a keyboard silence has its own counters and is not the case
     * these records exist to separate).
     */
    if (device->role == XHCI_HID_ROLE_MOUSE) {
        silence_erdp = xhci_silence_publish(device, silence_ms);
        /*
         * MOUSE-DIAG (2026-09-27): one read-only control-plane probe per
         * episode, after the snapshot and before the recovery below - so the
         * record says whether the device still answers at all, and the
         * watchdog's own re-arm stays exactly what it was.
         */
        xhci_ep0_probe_publish(device);
        /* The endpoint/ring state at the stall itself; it reaches the extent
           with the watchdog record's own flush below. */
        xhci_ep_state_publish(XHCI_HID_ROLE_MOUSE);
    }
    /*
     * ITEM-482: was the device in use when it went quiet?  The silence clock
     * restarted at `watchdog_progress_ticks`; a report within the active window
     * of that instant is what separates "nobody moved the mouse" from "the
     * mouse froze while it was being moved".  `active_silences` counts the
     * latter, and a second one is the point the pure rule says a plain re-arm
     * has already failed to fix.
     */
    active = xhci_watchdog_active_before(
        device->last_report_ticks, device->watchdog_progress_ticks,
        (uint64_t)XHCI_WATCHDOG_ACTIVE_MS * HFOC_TIMER_HZ / 1000U);
    if (active) {
        ++device->watchdog_active_count;
    }
    escalated = xhci_watchdog_escalate(active, device->watchdog_active_count);
    if (escalated) {
        ++device->watchdog_escalations;
    }
    {
        const uint64_t started = timer_cycles();

        xhci_endpoint_recover(device);
        if (device->role == XHCI_HID_ROLE_MOUSE) {
            xhci_silence_publish_after(device, silence_ms, silence_erdp);
        }
        {
            const uint32_t recover_us = (uint32_t)timer_cycles_to_us(
                timer_cycles() - started);

            if (recover_us > device->watchdog_recover_us_max) {
                device->watchdog_recover_us_max = recover_us;
            }
            xhci_watchdog_publish(device, silence_ms, active, escalated,
                                  recover_us);
        }
    }
}

static void xhci_watchdog_devices(void) {
    for (uint32_t role = 0U; role < XHCI_HID_DEVICES; ++role) {
        xhci_watchdog_device(xhci_device((uint8_t)role));
    }
}

/*
 * ITEM-438: ask a HID interface for its report descriptor and decide, from the
 * descriptor alone, whether it is a keyboard.
 *
 * This is the read the 2026-09-17 hardware run never made: an interface
 * declaring report protocol or no boot subclass was refused by its interface
 * descriptor, so a keyboard whose collection is only spelled out in the report
 * descriptor could not type.  The request is the standard GET_DESCRIPTOR with
 * the report type (0x22), the interface number in wIndex and the descriptor's
 * own length in wLength - the HID descriptor already published that length,
 * which is why the longest read this driver makes is bounded by
 * USB_HID_REPORT_DESCRIPTOR_MAX.
 *
 * A descriptor that is not a keyboard (a mouse, a consumer-control collection,
 * an empty or truncated one) is refused here with the device facts already
 * captured, so the record names what was rejected and why.
 *
 * ITEM-448: the read is the same for both roles - the descriptor bytes do not
 * depend on what the walk is looking for - so it is shared, and only the parse
 * and the refusal detail differ.
 */
static bool xhci_get_report_descriptor(struct xhci_hid_device *device,
                                       uint8_t interface_number,
                                       uint16_t length, uint32_t *received) {
    return xhci_control_transfer(
        device, xhci_device_report_descriptor_offset(device->role),
        device->slot_id, 0x81U, USB_REQUEST_GET_DESCRIPTOR,
        (uint16_t)((uint16_t)USB_DESCRIPTOR_HID_REPORT << 8), interface_number,
        length, true, received, NULL);
}

/*
 * ITEM-578: one role attempt's own record.
 *
 * A role that refused before the device's identity was read still has a stage
 * and a reason, and the two roles keep them apart: the pointer's outcome is
 * never the keyboard's outcome with a different name.  The record lives on the
 * device and is copied into the durable evidence when the controller's visit
 * is kept.
 */
static void xhci_role_attempt(struct xhci_hid_device *device,
                              enum xhci_status status, const char *stage,
                              const char *detail) {
    device->attempt_made = true;
    device->attempt_status = status;
    device->attempt_stage = stage != NULL ? stage : "ATTACH";
    xhci_detail_copy(device->attempt_detail, sizeof(device->attempt_detail),
                     detail != NULL ? detail : "UNKNOWN");
}

/*
 * ITEM-578: read the report descriptor into the buffer its own parser reads,
 * and keep what the read returned - its result, its length and a bounded copy
 * of the bytes.  The read failing and the descriptor being refused by the walk
 * are two different answers, and the record names them separately.
 */
static bool xhci_read_report_descriptor(struct xhci_hid_device *device,
                                        uint8_t interface_number,
                                        uint32_t *received) {
    const uint32_t buffer = xhci_device_report_descriptor_offset(device->role);
    uint8_t *descriptor = xhci_dma_at(buffer);

    memset(descriptor, 0, USB_HID_REPORT_DESCRIPTOR_MAX);
    device->report_descriptor_read = XHCI_EVIDENCE_RDESC_NONE;
    device->report_descriptor_parse = XHCI_EVIDENCE_RDESC_NONE;
    device->report_descriptor_length = 0U;
    memset(device->report_descriptor_bytes, 0,
           sizeof(device->report_descriptor_bytes));
    if (!xhci_get_report_descriptor(device, interface_number,
                                    USB_HID_REPORT_DESCRIPTOR_MAX,
                                    received) ||
        *received == 0U) {
        device->report_descriptor_read = XHCI_EVIDENCE_RDESC_READ_FAILED;
        return false;
    }
    device->report_descriptor_read = XHCI_EVIDENCE_RDESC_READ_OK;
    device->report_descriptor_length =
        *received > 0xFFFFU ? 0xFFFFU : (uint16_t)*received;
    {
        const uint32_t bytes =
            *received > sizeof(device->report_descriptor_bytes)
                ? (uint32_t)sizeof(device->report_descriptor_bytes)
                : *received;

        if (bytes != 0U) {
            memcpy(device->report_descriptor_bytes, descriptor, bytes);
        }
    }
    return true;
}

/*
 * ITEM-578: a report-descriptor transfer that failed is published as a failed
 * read (XHCI_STATUS_ENUMERATION_FAILED with the stage and the transport
 * reason), never as "not a keyboard": no parser ever saw the bytes, so no
 * verdict about the descriptor was reached.
 */
static bool xhci_parse_report_layout(struct xhci_hid_device *device,
                                     uint8_t port,
                                     uint8_t interface_number) {
    uint8_t *descriptor = xhci_dma_at(
        xhci_device_report_descriptor_offset(device->role));
    uint32_t received = 0U;

    if (!xhci_read_report_descriptor(device, interface_number, &received)) {
        xhci_attach_refuse(device, XHCI_STATUS_ENUMERATION_FAILED,
                           "REPORT_DESCRIPTOR",
                           xhci_detail_port(port, "REPORT_DESCRIPTOR_READ",
                                            xhci->detail));
        return false;
    }
    if (!usb_hid_parse_report_descriptor(descriptor, received,
                                         &device->layout)) {
        device->report_descriptor_parse = XHCI_EVIDENCE_RDESC_PARSE_REFUSED;
        xhci_attach_refuse(device, XHCI_STATUS_NOT_A_KEYBOARD,
                           "REPORT_DESCRIPTOR",
                           xhci_detail_port(port, "REPORT_DESCRIPTOR",
                                            "NOT_KEYBOARD"));
        return false;
    }
    device->report_descriptor_parse = XHCI_EVIDENCE_RDESC_PARSE_OK;
    return true;
}

/*
 * ITEM-448: the same read and the same question for the pointer: does this
 * interface's own report descriptor name a mouse?  The answer is the pointer's
 * layout - buttons, X and Y - or a refusal that names the port and the stage.
 *
 * ITEM-578: the pointer's classification is its own status and detail, and a
 * failed read is a failed read here too.
 */
static bool xhci_parse_mouse_report_layout(struct xhci_hid_device *device,
                                           uint8_t port,
                                           uint8_t interface_number) {
    uint8_t *descriptor = xhci_dma_at(
        xhci_device_report_descriptor_offset(device->role));
    uint32_t received = 0U;

    if (!xhci_read_report_descriptor(device, interface_number, &received)) {
        xhci_attach_refuse(device, XHCI_STATUS_ENUMERATION_FAILED,
                           "REPORT_DESCRIPTOR",
                           xhci_detail_port(port, "REPORT_DESCRIPTOR_READ",
                                            xhci->detail));
        return false;
    }
    if (!usb_hid_parse_mouse_report_descriptor(descriptor, received,
                                               &device->mouse_layout)) {
        device->report_descriptor_parse = XHCI_EVIDENCE_RDESC_PARSE_REFUSED;
        xhci_attach_refuse(device, XHCI_STATUS_NOT_A_MOUSE,
                           "REPORT_DESCRIPTOR",
                           xhci_detail_port(port, "REPORT_DESCRIPTOR",
                                            "NOT_MOUSE"));
        return false;
    }
    device->report_descriptor_parse = XHCI_EVIDENCE_RDESC_PARSE_OK;
    return true;
}

/*
 * ITEM-639: a boot mouse report has no wheel field.
 *
 * The physical 04D9:A09F run on 2026-11-21 was bound as a boot mouse and sent
 * three-byte reports (`short=24`, `wheels=0`), so the fourth byte the decoder
 * knows how to read never arrived.  Its report descriptor does declare a
 * wheel.  Probe that descriptor without making the read mandatory, and ask for
 * report protocol only when the descriptor names a wheel and the device
 * accepts the request; a mouse with no wheel, a failed read or a refused
 * SET_PROTOCOL stays on the boot protocol it declared.
 */
static bool xhci_mouse_enable_wheel_report_protocol(
    struct xhci_hid_device *device,
    uint8_t interface_number) {
    uint8_t *descriptor = xhci_dma_at(
        xhci_device_report_descriptor_offset(device->role));
    struct usb_hid_mouse_layout layout;
    uint32_t received = 0U;

    if (!xhci_read_report_descriptor(device, interface_number, &received) ||
        received == 0U) {
        return false;
    }
    if (!usb_hid_parse_mouse_report_descriptor(descriptor, received, &layout)) {
        device->report_descriptor_parse = XHCI_EVIDENCE_RDESC_PARSE_REFUSED;
        return false;
    }
    device->report_descriptor_parse = XHCI_EVIDENCE_RDESC_PARSE_OK;
    if (layout.wheel_offset == USB_HID_REPORT_OFFSET_NONE) {
        return false;
    }
    if (!xhci_control_transfer(device,
                               xhci_device_descriptor_offset(device->role),
                               device->slot_id, 0x21U,
                               USB_HID_REQUEST_SET_PROTOCOL,
                               USB_HID_REPORT_PROTOCOL_VALUE,
                               interface_number, 0U, false, NULL, NULL)) {
        return false;
    }
    device->mouse_layout = layout;
    device->report_mode = true;
    return true;
}

static void xhci_push_event(const struct keyboard_event *event) {
    struct xhci_hid_device *device = xhci_device(XHCI_HID_ROLE_KEYBOARD);
    uint32_t next = 0U;

    if (!xhci_local_queue_push(device->event_head, device->event_tail,
                               XHCI_LOCAL_EVENTS, &next)) {
        /* ITEM-458: the console is not draining; dropping the newest event
           keeps the queue bounded and never overwrites an unread one - and the
           refusal is counted, so a lost character is a published number rather
           than a silence. */
        ++device->dropped_report_count;
        return;
    }
    device->events[device->event_head] = *event;
    device->event_head = next;
}

/* ITEM-448: the pointer's own queue, under the same bounded rule. */
static void xhci_push_mouse_event(const struct usb_hid_mouse_event *event) {
    struct xhci_hid_device *device = xhci_device(XHCI_HID_ROLE_MOUSE);
    const uint32_t next =
        (device->mouse_event_head + 1U) % XHCI_LOCAL_MOUSE_EVENTS;

    if (next == device->mouse_event_tail) {
        return;
    }
    device->mouse_events[device->mouse_event_head] = *event;
    device->mouse_event_head = next;
}

/* ITEM-573: "bus:device.function" as text, so the durable mouse evidence can
   name the controller without borrowing a live session. */
static void xhci_location_text(char *target, size_t size,
                               const struct pci_location *location) {
    static const char digits[] = "0123456789ABCDEF";
    size_t length = 0U;

    if (target == NULL || size == 0U || location == NULL) {
        return;
    }
    if (size > length + 1U) {
        target[length++] = digits[(location->bus >> 4) & 0x0FU];
    }
    if (size > length + 1U) {
        target[length++] = digits[location->bus & 0x0FU];
    }
    if (size > length + 1U) {
        target[length++] = ':';
    }
    if (size > length + 1U) {
        target[length++] = digits[(location->device >> 4) & 0x0FU];
    }
    if (size > length + 1U) {
        target[length++] = digits[location->device & 0x0FU];
    }
    if (size > length + 1U) {
        target[length++] = '.';
    }
    if (size > length + 1U) {
        target[length++] = (char)('0' + (location->function % 10U));
    }
    target[length] = '\0';
}

/*
 * ITEM-448: the same refusal, published under the role that earned it.
 *
 * A keyboard refusal is the machine-wide verdict the boot record already
 * carries (`[XHCI] state=UNAVAILABLE reason=...`, `[KBD ]`).  A mouse refusal
 * is kept beside it, so a machine whose pointer was refused says so without
 * overwriting the keyboard's reason - and a machine with a keyboard but no
 * mouse does not publish the keyboard's refusal as the mouse's.
 */
static void xhci_attach_refuse(struct xhci_hid_device *device,
                               enum xhci_status status, const char *stage,
                               const char *detail) {
    /* ITEM-578: every refusal names its own role, stage and reason, including
       the ones that stopped before the device's identity was read. */
    xhci_role_attempt(device, status, stage, detail);
    if (device->role == XHCI_HID_ROLE_MOUSE) {
        xhci_detail_copy(xhci_machine.mouse_detail,
                         sizeof(xhci_machine.mouse_detail),
                         detail != NULL ? detail : "UNKNOWN");
        xhci_machine.mouse_device_seen = device->device_seen;
        xhci_machine.mouse_seen = device->device_seen;
        xhci_machine.mouse_seen_port = device->port;
        xhci_machine.mouse_seen_speed = device->speed;
        xhci_location_text(xhci_machine.mouse_seen_controller,
                           sizeof(xhci_machine.mouse_seen_controller),
                           &xhci->location);
        xhci_machine.mouse_vendor_id = device->vendor_id;
        xhci_machine.mouse_product_id = device->product_id;
        xhci_detail_copy(xhci_machine.mouse_interface_summary,
                         sizeof(xhci_machine.mouse_interface_summary),
                         device->interface_summary);
        return;
    }
    xhci_set_status(status, detail);
}

/* The same refusal when the interface walk itself reached a verdict - "no HID
   mouse in this configuration" - which the [MOUSE] record publishes. */
static void xhci_attach_refuse_verdict(struct xhci_hid_device *device,
                                       enum xhci_status status,
                                       const char *stage,
                                       enum usb_hid_status mouse_status,
                                       const char *detail) {
    if (device->role == XHCI_HID_ROLE_MOUSE) {
        xhci_machine.mouse_status = mouse_status;
    }
    xhci_attach_refuse(device, status, stage, detail);
}

/* ------------------------------------------------------------------------ *
 * ITEM-519 increment 2: the xHCI half of USB mass storage.
 *
 * The BOT/SCSI core (kernel/src/usb_storage.c) reaches the wire through three
 * callbacks; this block supplies them from one device context: an endpoint-0
 * ring, a ring per bulk endpoint, and one bounce page the controller reads and
 * writes.  The device lives in a session of its own, exactly like the keyboard
 * and the pointer, so its binding survives the walk to the next controller and
 * its rings are never the ones a HID role uses.
 *
 * Storage is offered only to a port no HID role bound, and it spends a budget
 * of its own: the HID walk's attempt budget is what keeps a controller full of
 * other devices from starving the keyboard behind it, and offering every port
 * to storage would spend that budget on devices that are not storage at all.
 * ------------------------------------------------------------------------ */

/* Endpoint context types live in kernel/include/xhci_trb.h (xHCI 1.1 table
   6-7: 2 = Bulk OUT, 6 = Bulk IN). */
/* The bounce page: control data (descriptors) at its base, bulk data above it. */
#define XHCI_STORAGE_BULK_OFFSET 0x200U
#define XHCI_STORAGE_BULK_BYTES 0x0E00U
#define XHCI_STORAGE_DATA_BYTES 0x1000U
#define XHCI_STORAGE_SECTOR_BYTES 512U
/* Storage gets its own per-controller attempt budget (see above). */
#define XHCI_STORAGE_MAX_ATTACH_ATTEMPTS 8U

static void xhci_log_storage_record(
    const struct xhci_storage_device *storage,
    const char *state,
    const char *reason,
    const char *detail,
    bool bound);

/* ITEM-519 increment 4: the record that names whether the bound stick also got
   a block_device view, and why not when it did not. */
static void xhci_log_storage_block_record(
    const struct xhci_storage_device *storage);

/* ITEM-519 increment 5: the record that answers whether this device is the
   medium the machine booted from; `boot` and `verdict` may be NULL when the
   probe could not read or identify anything, and `state`/`reason` then name
   what stopped it. */
static void xhci_log_medium_record(
    const struct xhci_storage_device *storage,
    const struct install_medium_boot_identity *boot,
    const struct install_medium_verdict *verdict,
    const char *state,
    const char *reason);

/* ITEM-519 increment 5: the read-only boot-medium probe for one device.  The
   walk calls it directly because the machine does not hold the session yet. */
static bool xhci_storage_medium_probe(struct xhci_storage_device *storage);

/*
 * One bulk transfer: a Normal TRB with IOC on the endpoint's own ring, the
 * doorbell with that endpoint's device context index, and the Transfer Event
 * that names the TRB.  A short packet completes the transfer - that is normal
 * for bulk - and the residual length is the caller's byte count.  The
 * direction lives in the endpoint, not in the TRB.
 */
static bool xhci_bulk_transfer(
    struct xhci_ring *ring,
    uint8_t slot_id,
    uint8_t dci,
    uint64_t buffer_physical,
    uint32_t length,
    uint32_t *transferred) {
    struct xhci_trb trb;
    uint32_t index = 0U;
    uint64_t trb_physical;

    if (transferred != NULL) {
        *transferred = 0U;
    }
    trb = xhci_trb_normal(ring->cycle, buffer_physical, length, true);
    if (!xhci_ring_push_index(ring, &trb, &index)) {
        return false;
    }
    trb_physical = ring->phys + (uint64_t)index * XHCI_TRB_BYTES;
    xhci_barrier();
    xhci_write32(xhci_doorbell(slot_id), 0U, dci);
    for (uint32_t spins = 0U; spins < XHCI_SPIN_TRANSFER; ++spins) {
        struct xhci_event event;

        if (!xhci_next_event(&event)) {
            __asm__ volatile ("pause");
            continue;
        }
        if (event.type != XHCI_TRB_TRANSFER_EVENT ||
            event.slot_id != slot_id || event.endpoint_id != dci ||
            event.parameter != trb_physical) {
            /*
             * A completion for another device that shares the controller's
             * event ring is applied, not dropped - the same rule the control
             * and interrupt waits follow.
             */
            (void)xhci_apply_event(&event);
            continue;
        }
        if (event.completion_code != XHCI_COMPLETION_SUCCESS &&
            event.completion_code != XHCI_COMPLETION_SHORT_PACKET) {
            xhci->detail = xhci_completion_code_string(event.completion_code);
            return false;
        }
        if (transferred != NULL) {
            *transferred = length >= event.transfer_length
                               ? length - event.transfer_length
                               : 0U;
        }
        return true;
    }
    xhci->detail = "TRANSFER_TIMEOUT";
    return false;
}

static bool xhci_storage_control(
    void *context,
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    uint16_t length,
    bool data_in,
    void *data,
    uint32_t *transferred) {
    struct xhci_storage_device *storage =
        (struct xhci_storage_device *)context;
    struct xhci_session *const caller = xhci;
    uint8_t *bounce;
    uint32_t received = 0U;
    bool ok;

    if (storage == NULL || storage->session == NULL ||
        length > XHCI_DATA_DESCRIPTOR_MAX) {
        return false;
    }
    xhci = storage->session;
    bounce = xhci_dma_at(XHCI_OFF_STORAGE_DATA);
    if (!data_in && data != NULL && length != 0U) {
        memcpy(bounce, data, length);
    }
    ok = xhci_control_transfer_ring(
        &storage->ep0_ring, XHCI_OFF_STORAGE_DATA, storage->slot_id,
        request_type, request, value, index, length, data_in, &received, NULL);
    if (ok && data_in && data != NULL && length != 0U) {
        memcpy(data, bounce, length);
    }
    xhci = caller;
    if (transferred != NULL) {
        *transferred = ok ? received : 0U;
    }
    return ok;
}

static bool xhci_storage_bulk(
    void *context,
    bool direction_in,
    void *buffer,
    uint32_t length,
    uint32_t *transferred) {
    struct xhci_storage_device *storage =
        (struct xhci_storage_device *)context;
    struct xhci_session *const caller = xhci;
    uint32_t done = 0U;

    if (storage == NULL || storage->session == NULL || buffer == NULL) {
        return false;
    }
    if (transferred != NULL) {
        *transferred = 0U;
    }
    xhci = storage->session;
    while (done < length) {
        const uint32_t chunk =
            length - done < XHCI_STORAGE_BULK_BYTES
                ? length - done
                : XHCI_STORAGE_BULK_BYTES;
        uint8_t *bounce =
            xhci_dma_at(XHCI_OFF_STORAGE_DATA + XHCI_STORAGE_BULK_OFFSET);
        uint32_t moved = 0U;

        if (!direction_in) {
            memcpy(bounce, (const uint8_t *)buffer + done, chunk);
        }
        if (!xhci_bulk_transfer(
                direction_in ? &storage->bulk_in_ring
                             : &storage->bulk_out_ring,
                storage->slot_id,
                direction_in ? storage->bulk_in_dci
                             : storage->bulk_out_dci,
                xhci_dma_physical(XHCI_OFF_STORAGE_DATA +
                                  XHCI_STORAGE_BULK_OFFSET),
                chunk, &moved) ||
            moved != chunk) {
            ++storage->transfer_error_count;
            xhci = caller;
            return false;
        }
        if (direction_in) {
            memcpy((uint8_t *)buffer + done, bounce, chunk);
        }
        done += chunk;
    }
    xhci = caller;
    if (transferred != NULL) {
        *transferred = length;
    }
    return true;
}

static bool xhci_storage_bulk_in(void *context, void *buffer, uint32_t length,
                                 uint32_t *transferred) {
    return xhci_storage_bulk(context, true, buffer, length, transferred);
}

static bool xhci_storage_bulk_out(void *context, const void *buffer,
                                  uint32_t length, uint32_t *transferred) {
    return xhci_storage_bulk(context, false, (void *)buffer, length,
                             transferred);
}

static bool xhci_storage_get_descriptor(
    struct xhci_storage_device *storage,
    uint8_t descriptor_type,
    uint16_t length,
    uint32_t *received) {
    return xhci_control_transfer_ring(
        &storage->ep0_ring, XHCI_OFF_STORAGE_DATA, storage->slot_id, 0x80U,
        USB_REQUEST_GET_DESCRIPTOR,
        (uint16_t)((uint16_t)descriptor_type << 8U), 0U, length, true,
        received, NULL);
}

static bool xhci_storage_configure(
    struct xhci_storage_device *storage,
    const struct usb_storage_config *config) {
    uint8_t *input = xhci->input_context;
    const uint8_t dci_in = xhci_endpoint_dci(
        (uint8_t)(config->bulk_in_endpoint & 0x0FU), true);
    const uint8_t dci_out = xhci_endpoint_dci(
        (uint8_t)(config->bulk_out_endpoint & 0x0FU), false);
    const uint8_t max_dci = dci_in > dci_out ? dci_in : dci_out;
    struct xhci_trb trb;
    const char *detail = NULL;

    xhci_context_clear(input, XHCI_INPUT_CONTEXT_MAX_BYTES);
    xhci_input_control_context(
        input, xhci->context_bytes,
        xhci_context_flag(0U) | xhci_context_flag(dci_in) |
            xhci_context_flag(dci_out),
        0U);
    xhci_slot_context(
        input + xhci_context_offset(xhci->context_bytes,
                                    xhci_context_index(0U)),
        storage->speed, max_dci, storage->port);
    xhci_endpoint_context(
        input + xhci_context_offset(xhci->context_bytes, 1U),
        XHCI_EP_TYPE_CONTROL, xhci_speed_ep0_max_packet(storage->speed), 0U,
        storage->ep0_ring.phys, 8U);
    /* The Add flag carries the DCI, the context entry is one further along
       (see xhci_context_index); using the DCI as the entry index writes the
       bulk contexts where the controller does not read them, and QEMU answers
       the command with TRB Error. */
    xhci_endpoint_context_burst(
        input + xhci_context_offset(xhci->context_bytes,
                                    xhci_context_index(dci_in)),
        XHCI_EP_TYPE_BULK_IN, config->bulk_in_max_packet, 0U,
        storage->bulk_in_ring.phys, config->bulk_in_max_packet,
        config->bulk_in_max_burst);
    xhci_endpoint_context_burst(
        input + xhci_context_offset(xhci->context_bytes,
                                    xhci_context_index(dci_out)),
        XHCI_EP_TYPE_BULK_OUT, config->bulk_out_max_packet, 0U,
        storage->bulk_out_ring.phys, config->bulk_out_max_packet,
        config->bulk_out_max_burst);
    trb = xhci_trb_configure_endpoint(1U, xhci->input_context_physical,
                                      storage->slot_id);
    if (!xhci_command_ok(&trb, &detail)) {
        const uint32_t *icc = (const uint32_t *)input;

        storage->configure_diag[0] = icc[0];
        storage->configure_diag[1] = icc[1];
        storage->configure_diag[2] =
            ((uint32_t)dci_in << 8) | (uint32_t)dci_out;
        storage->configure_diag[3] = storage->slot_id;
        storage->configure_diag_valid = true;
        xhci_set_status(XHCI_STATUS_ENUMERATION_FAILED,
                        xhci_detail_port(storage->port, "STORAGE_CONFIGURE",
                                         detail));
        return false;
    }
    storage->bulk_in_dci = dci_in;
    storage->bulk_out_dci = dci_out;
    return true;
}

static bool xhci_storage_attach(
    struct xhci_storage_device *storage,
    uint32_t port,
    uint8_t speed) {
    const uint16_t ep0_max_packet = xhci_speed_ep0_max_packet(speed);
    uint8_t *descriptor;
    uint32_t received = 0U;
    uint16_t config_total = 0U;
    struct usb_storage_config config;
    struct usb_storage_transport transport;
    struct xhci_event completion;
    uint8_t slot_id = 0U;
    enum usb_storage_status status;
    bool parsed = false;

    memset(storage, 0, sizeof(*storage));
    storage->port = (uint8_t)port;
    storage->speed = speed;
    if (ep0_max_packet == 0U) {
        xhci_log_storage_record(storage, "REFUSED", "SPEED", NULL, false);
        return false;
    }
    xhci_ring_init(&storage->ep0_ring,
                   (struct xhci_trb *)xhci_dma_at(XHCI_OFF_STORAGE_EP0_RING),
                   xhci_dma_physical(XHCI_OFF_STORAGE_EP0_RING),
                   XHCI_RING_TRBS, 1U);
    xhci_ring_init(&storage->bulk_in_ring,
                   (struct xhci_trb *)xhci_dma_at(
                       XHCI_OFF_STORAGE_BULK_IN_RING),
                   xhci_dma_physical(XHCI_OFF_STORAGE_BULK_IN_RING),
                   XHCI_STORAGE_BULK_TRBS, 1U);
    xhci_ring_init(&storage->bulk_out_ring,
                   (struct xhci_trb *)xhci_dma_at(
                       XHCI_OFF_STORAGE_BULK_OUT_RING),
                   xhci_dma_physical(XHCI_OFF_STORAGE_BULK_OUT_RING),
                   XHCI_STORAGE_BULK_TRBS, 1U);
    {
        const struct xhci_trb enable = xhci_trb_enable_slot(1U);

        if (!xhci_submit_command(&enable, &completion) ||
            completion.completion_code != XHCI_COMPLETION_SUCCESS) {
            xhci_log_storage_record(storage, "REFUSED", "ENABLE_SLOT", NULL, false);
            return false;
        }
        slot_id = (uint8_t)completion.parameter_low;
        if (slot_id == 0U) {
            slot_id = completion.slot_id;
        }
        if (slot_id == 0U || slot_id > xhci->max_slots) {
            xhci_log_storage_record(storage, "REFUSED", "BAD_SLOT", NULL, false);
            return false;
        }
    }
    storage->slot_id = slot_id;
    if (!xhci_address_device_ring(&storage->ep0_ring, slot_id,
                                  (uint8_t)port, speed, ep0_max_packet)) {
        xhci_log_storage_record(storage, "REFUSED", "ADDRESS_DEVICE", NULL, false);
        (void)xhci_disable_slot(slot_id);
        return false;
    }
    descriptor = xhci_dma_at(XHCI_OFF_STORAGE_DATA);
    memset(descriptor, 0, 18U);
    if (!xhci_storage_get_descriptor(storage, USB_DESCRIPTOR_DEVICE, 18U,
                                     &received) ||
        received < 18U) {
        xhci_log_storage_record(storage, "REFUSED", "DEVICE_DESCRIPTOR", NULL,
                                false);
        (void)xhci_disable_slot(slot_id);
        return false;
    }
    storage->vendor_id = (uint16_t)((uint16_t)descriptor[8] |
                                    ((uint16_t)descriptor[9] << 8U));
    storage->product_id = (uint16_t)((uint16_t)descriptor[10] |
                                     ((uint16_t)descriptor[11] << 8U));
    memset(descriptor, 0, USB_CONFIG_DESCRIPTOR_PREFIX);
    if (!xhci_storage_get_descriptor(storage, USB_DESCRIPTOR_CONFIGURATION,
                                     USB_CONFIG_DESCRIPTOR_PREFIX,
                                     &received) ||
        received < USB_CONFIG_DESCRIPTOR_PREFIX) {
        xhci_log_storage_record(storage, "REFUSED", "CONFIG_DESCRIPTOR", NULL,
                                false);
        (void)xhci_disable_slot(slot_id);
        return false;
    }
    config_total = usb_hid_config_total_length(descriptor,
                                               USB_CONFIG_DESCRIPTOR_PREFIX);
    if (config_total == 0U ||
        config_total > USB_HID_CONFIG_DESCRIPTOR_MAX) {
        xhci_log_storage_record(storage, "REFUSED", "CONFIG_TOTAL", NULL, false);
        (void)xhci_disable_slot(slot_id);
        return false;
    }
    memset(descriptor, 0, config_total);
    if (!xhci_storage_get_descriptor(storage, USB_DESCRIPTOR_CONFIGURATION,
                                     config_total, &received) ||
        received < 9U) {
        xhci_log_storage_record(storage, "REFUSED", "CONFIG_DESCRIPTOR_FULL",
                                NULL, false);
        (void)xhci_disable_slot(slot_id);
        return false;
    }
    status = usb_storage_parse_config(descriptor, received, &config);
    if (status == USB_STORAGE_OK) {
        parsed = true;
    }
    if (!parsed) {
        xhci_log_storage_record(storage, "REFUSED",
                                usb_storage_status_string(status), NULL, false);
        (void)xhci_disable_slot(slot_id);
        return false;
    }
    if (!xhci_control_transfer_ring(
            &storage->ep0_ring, XHCI_OFF_STORAGE_DATA, slot_id,
            USB_REQUEST_TYPE_STANDARD, USB_REQUEST_SET_CONFIGURATION,
            config.configuration_value, 0U, 0U, false, NULL, NULL)) {
        xhci_log_storage_record(storage, "REFUSED", "SET_CONFIGURATION",
                                NULL, false);
        (void)xhci_disable_slot(slot_id);
        return false;
    }
    if (!xhci_storage_configure(storage, &config)) {
        xhci_log_storage_record(storage, "REFUSED", "CONFIGURE_ENDPOINT",
                                xhci->detail, false);
        (void)xhci_disable_slot(slot_id);
        return false;
    }
    transport.context = storage;
    transport.control = xhci_storage_control;
    transport.bulk_in = xhci_storage_bulk_in;
    transport.bulk_out = xhci_storage_bulk_out;
    storage->interface_number = config.interface_number;
    /* ITEM-545: the Configure Endpoint above programmed these; the record
       names them so the next hardware run shows whether the device declared
       a SuperSpeed burst and the context carried it. */
    storage->bulk_in_max_burst = config.bulk_in_max_burst;
    storage->bulk_out_max_burst = config.bulk_out_max_burst;
    storage->session = xhci;
    status = usb_storage_bind(&storage->usb, &transport, &config);
    if (status != USB_STORAGE_OK) {
        xhci_log_storage_record(storage, "REFUSED",
                                usb_storage_status_string(status), NULL, false);
        (void)xhci_disable_slot(slot_id);
        return false;
    }
    storage->bound = true;
    /*
     * ITEM-519 increment 4: give the bound stick its block_device view in the
     * same breath as the transport binding, and publish the verdict either way.
     * The journal's transport is the block layer; a stick that carries the
     * transport but no block view must be named as such, not discovered later
     * by the first writer.
     */
    storage->block_status =
        usb_storage_block_device_bind(&storage->usb, &storage->block);
    storage->block_bound = storage->block_status == BLOCK_DEVICE_OK;
    if (storage->usb.block_size == XHCI_STORAGE_SECTOR_BYTES &&
        storage->usb.block_count != 0U) {
        uint8_t sector[XHCI_STORAGE_SECTOR_BYTES];

        if (usb_storage_read(&storage->usb, 0U, 1U, sector) ==
            USB_STORAGE_OK) {
            memcpy(storage->sector0, sector, sizeof(storage->sector0));
            storage->sector0_valid = true;
        }
    }
    xhci_log_storage_block_record(storage);
    xhci_log_storage_record(storage, "BOUND", "OK", NULL, true);
    /*
     * ITEM-519 increment 5: name whether this device is the medium the machine
     * booted from.  The probe only reads LBA 0 and compares; it never writes,
     * and its verdict is what a journal guard must see before it may write.
     */
    (void)xhci_storage_medium_probe(storage);
    return true;
}

/*
 * ITEM-519 increment 3: the write half, proven on the medium.
 *
 * Increment 2 read sector 0 through the bulk IN path and published its bytes;
 * the same wire has a bulk OUT half that nothing had exercised end to end.
 * This proof drives it on the last sector of the bound stick: read it, write a
 * deterministic pattern over it, synchronize the cache, read the pattern back,
 * restore the sector and read the restored bytes back.  The verdicts are
 * published as one `[USB-STORAGE] state=WRITE_PROOF` record, and the medium is
 * left exactly as it was found.  The last sector is used because it is one
 * sector nothing else on the stick points at, and every write is preceded by a
 * read of the same sector, so a device that lies about its capacity is caught
 * before anything is overwritten.
 *
 * ITEM-519 increment 4: the proof reads, writes and flushes through the stick's
 * `block_device` view - the same interface the journal of ITEM-521 will own -
 * rather than through the BOT/SCSI object directly.  The bytes on the medium
 * are proof of the transport; this path is proof that the block layer above it
 * carries those bytes, and a stick that got no block view is refused by name
 * (`reason=NO_BLOCK_DEVICE`) instead of being reported as a write that never
 * happened.
 */
#define XHCI_STORAGE_PROOF_BYTES XHCI_STORAGE_SECTOR_BYTES

static void xhci_log_storage_proof_record(
    const struct xhci_storage_device *storage,
    uint64_t lba,
    const char *reason,
    bool write_ok,
    bool verify_ok,
    bool restore_ok);

bool xhci_storage_write_proof(void) {
    struct xhci_storage_device *storage;
    struct xhci_session *const caller = xhci;
    struct block_device *block;
    static uint8_t original[XHCI_STORAGE_PROOF_BYTES];
    static uint8_t pattern[XHCI_STORAGE_PROOF_BYTES];
    static uint8_t readback[XHCI_STORAGE_PROOF_BYTES];
    uint64_t lba = 0U;
    uint64_t offset = 0U;
    const char *reason = "OK";
    bool write_ok = false;
    bool verify_ok = false;
    bool restore_ok = false;
    bool sync_ok = true;

    if (xhci_machine.storage_session == NULL ||
        !xhci_machine.storage_session->storage.bound) {
        const uint64_t flags = serial_sequence_begin();

        console_journal_begin();
        serial_write("[USB-STORAGE] state=WRITE_PROOF reason=NO_DEVICE\n");
        console_journal_end();
        serial_sequence_end(flags);
        return false;
    }
    storage = &xhci_machine.storage_session->storage;
    block = xhci_storage_block_device();
    if (block == NULL) {
        reason = "NO_BLOCK_DEVICE";
    } else if (storage->usb.block_size != XHCI_STORAGE_PROOF_BYTES ||
               storage->usb.block_count < 2U ||
               block->block_size != XHCI_STORAGE_PROOF_BYTES) {
        reason = "BAD_GEOMETRY";
    } else {
        lba = (uint64_t)storage->usb.block_count - 1U;
        offset = lba * XHCI_STORAGE_PROOF_BYTES;
        for (uint32_t index = 0U; index < XHCI_STORAGE_PROOF_BYTES;
             ++index) {
            pattern[index] = (uint8_t)(0xA5U ^ (uint8_t)index);
        }
        if (block_device_read(block, offset, original,
                              XHCI_STORAGE_PROOF_BYTES) != BLOCK_DEVICE_OK) {
            reason = "READ_ORIGINAL";
        } else if (block_device_write(block, offset, pattern,
                                      XHCI_STORAGE_PROOF_BYTES) !=
                   BLOCK_DEVICE_OK) {
            /*
             * The write itself was refused, so the medium still carries the
             * sector this call just read; there is nothing to restore and the
             * record says only that the write half failed.
             */
            reason = "WRITE";
        } else {
            write_ok = true;
            if (block_device_flush(block) != BLOCK_DEVICE_OK) {
                reason = "SYNCHRONIZE";
                sync_ok = false;
            }
            if (block_device_read(block, offset, readback,
                                  XHCI_STORAGE_PROOF_BYTES) !=
                BLOCK_DEVICE_OK) {
                reason = "READ_BACK";
            } else if (memcmp(readback, pattern,
                              XHCI_STORAGE_PROOF_BYTES) != 0) {
                reason = "VERIFY";
            } else {
                verify_ok = true;
            }
            /*
             * Restore whatever the write may have left behind, even when the
             * pattern never verified: the original sector was already read, so
             * restoring it is the one repair this proof can always attempt.
             * The first failure keeps the record's reason name; a later verdict
             * is still published in its own field.
             */
            if (block_device_write(block, offset, original,
                                   XHCI_STORAGE_PROOF_BYTES) !=
                BLOCK_DEVICE_OK) {
                if (verify_ok) {
                    reason = "RESTORE";
                }
            } else {
                if (block_device_flush(block) != BLOCK_DEVICE_OK) {
                    sync_ok = false;
                    if (verify_ok) {
                        reason = "RESTORE_SYNCHRONIZE";
                    }
                }
                if (block_device_read(block, offset, readback,
                                      XHCI_STORAGE_PROOF_BYTES) !=
                    BLOCK_DEVICE_OK) {
                    if (verify_ok) {
                        reason = "RESTORE_READ";
                    }
                } else {
                    restore_ok = memcmp(readback, original,
                                        XHCI_STORAGE_PROOF_BYTES) == 0;
                    if (!restore_ok && verify_ok) {
                        reason = "RESTORE";
                    }
                }
            }
        }
    }
    xhci = storage->session;
    xhci_log_storage_proof_record(storage, lba, reason, write_ok, verify_ok,
                                  restore_ok);
    xhci = caller;
    return write_ok && verify_ok && restore_ok && sync_ok;
}


/*
 * ITEM-579: the physical device a role attaches through.
 *
 * `xhci_session_path()` finds the path that already carries a port, so a second
 * role joins the device instead of re-addressing it.  `xhci_path_new()` claims a
 * free path slot, `xhci_path_release()` gives the slot back, and
 * `xhci_attach_abort()` detaches one role from its path (releasing the path only
 * when no role is left on it).
 */
static struct xhci_device_path *xhci_session_path(struct xhci_session *session,
                                                  uint8_t port) {
    if (session == NULL) {
        return NULL;
    }
    for (uint32_t index = 0U; index < XHCI_HID_DEVICES; ++index) {
        struct xhci_device_path *path = &session->paths[index];

        if (path->valid && path->port == port && path->slot_id != 0U) {
            return path;
        }
    }
    return NULL;
}

static struct xhci_device_path *xhci_path_new(struct xhci_session *session) {
    if (session == NULL) {
        return NULL;
    }
    for (uint32_t index = 0U; index < XHCI_HID_DEVICES; ++index) {
        struct xhci_device_path *path = &session->paths[index];

        if (!path->valid) {
            return path;
        }
    }
    return NULL;
}

static void xhci_path_release(struct xhci_device_path *path) {
    if (path == NULL) {
        return;
    }
    if (path->valid && path->slot_id != 0U) {
        (void)xhci_disable_slot(path->slot_id);
    }
    memset(path, 0, sizeof(*path));
}

/* Does this path still carry a live role?  Only when it does not may the port
   be reset and the device re-addressed. */
static bool xhci_path_live(const struct xhci_device_path *path) {
    return path != NULL && path->valid && path->configured &&
           path->slot_id != 0U && path->roles != 0U;
}

/*
 * ITEM-579: detach one role from its path.  The path - and with it the slot,
 * the USB address and every other role's endpoint - survives while another role
 * is attached; it is released only when the last role leaves.
 */
static void xhci_attach_abort(struct xhci_hid_device *device) {
    struct xhci_device_path *path = device->path;

    device->ready = false;
    device->transfer_pending = false;
    device->armed_count = 0U;
    device->armed_head = 0U;
    if (path != NULL) {
        path->roles &= (uint8_t)~(1U << device->role);
        if (path->roles == 0U) {
            xhci_path_release(path);
        }
    }
    device->path = NULL;
    device->slot_id = 0U;
}

/*
 * ITEM-579: establish a new physical path - Enable Slot, Address Device, the
 * device descriptor, the configuration descriptor (read once and kept on the
 * path) and SET_CONFIGURATION.  Everything after this is per interface: the
 * role's report descriptor, its endpoint and its queue are added to the same
 * slot by xhci_attach_device().
 */
static bool xhci_path_establish(struct xhci_hid_device *device,
                                struct xhci_device_path *path, uint32_t port,
                                uint8_t speed) {
    const uint32_t path_index = (uint32_t)(path - xhci->paths);
    /* The descriptor buffer belongs to the role doing the enumeration: every
       control transfer writes into that role's data region, so the path's
       temporary reads must look there too (the two coincide only for the
       keyboard). */
    uint8_t *descriptor = xhci_dma_at(
        xhci_device_descriptor_offset(device->role));
    uint16_t ep0_max_packet = xhci_speed_ep0_max_packet(speed);
    uint32_t received = 0U;
    uint16_t config_total = 0U;
    struct xhci_event completion;
    uint8_t slot_id = 0U;

    memset(path, 0, sizeof(*path));
    path->valid = true;
    path->port = (uint8_t)port;
    path->speed = speed;
    path->max_dci = 1U;
    xhci_ring_init(&path->ep0_ring,
                   (struct xhci_trb *)xhci_dma_at(
                       XHCI_OFF_DEVICE_EP0_RING + path_index * 0x1000U),
                   xhci_dma_physical(XHCI_OFF_DEVICE_EP0_RING +
                                     path_index * 0x1000U),
                   XHCI_RING_TRBS, 1U);
    device->path = path;
    device->slot_id = 0U;
    device->port = (uint8_t)port;
    device->speed = speed;

    {
        const struct xhci_trb enable = xhci_trb_enable_slot(1U);

        if (!xhci_submit_command(&enable, &completion)) {
            xhci_attach_refuse(device, XHCI_STATUS_ENUMERATION_FAILED,
                               "ENABLE_SLOT",
                               xhci_detail_port(port, "ENABLE_SLOT",
                                                "TIMEOUT"));
            goto fail;
        }
        if (completion.completion_code != XHCI_COMPLETION_SUCCESS) {
            xhci_attach_refuse(
                device, XHCI_STATUS_ENUMERATION_FAILED, "ENABLE_SLOT",
                xhci_detail_port(port, "ENABLE_SLOT",
                                 xhci_completion_code_string(
                                     completion.completion_code)));
            goto fail;
        }
        slot_id = (uint8_t)completion.parameter_low;
        if (slot_id == 0U) {
            slot_id = completion.slot_id;
        }
        if (slot_id == 0U || slot_id > xhci->max_slots) {
            xhci_attach_refuse(device, XHCI_STATUS_ENUMERATION_FAILED,
                               "ENABLE_SLOT",
                               xhci_detail_port(port, "ENABLE_SLOT",
                                                "BAD_SLOT_ID"));
            goto fail;
        }
    }
    path->slot_id = slot_id;
    device->slot_id = slot_id;

    if (!xhci_address_device_ring(&path->ep0_ring, slot_id, (uint8_t)port,
                                  speed, ep0_max_packet)) {
        xhci_attach_refuse(device, xhci->status, "ADDRESS_DEVICE",
                           xhci->detail);
        goto fail;
    }
    memset(descriptor, 0, XHCI_DATA_DESCRIPTOR_MAX);
    if (!xhci_get_descriptor(device, slot_id, USB_DESCRIPTOR_DEVICE, 0U, 8U,
                             &received) ||
        received < 8U) {
        xhci_attach_refuse(device, XHCI_STATUS_ENUMERATION_FAILED,
                           "DEVICE_DESCRIPTOR",
                           xhci_detail_port(port, "DEVICE_DESCRIPTOR_PREFIX",
                                            xhci->detail));
        goto fail;
    }
    if (descriptor[7] != 0U && descriptor[7] != (uint8_t)ep0_max_packet &&
        (descriptor[7] == 8U || descriptor[7] == 16U ||
         descriptor[7] == 32U || descriptor[7] == 64U)) {
        /* The device declares a different control-pipe packet size. */
        ep0_max_packet = descriptor[7];
        if (!xhci_evaluate_ep0(device, slot_id, (uint8_t)port, speed,
                               ep0_max_packet)) {
            xhci_attach_refuse(device, xhci->status, "EVALUATE_CONTEXT",
                               xhci->detail);
            goto fail;
        }
    }
    memset(descriptor, 0, XHCI_DATA_DESCRIPTOR_MAX);
    if (!xhci_get_descriptor(device, slot_id, USB_DESCRIPTOR_DEVICE, 0U, 18U,
                             &received) ||
        received < 8U) {
        xhci_attach_refuse(device, XHCI_STATUS_ENUMERATION_FAILED,
                           "DEVICE_DESCRIPTOR",
                           xhci_detail_port(port, "DEVICE_DESCRIPTOR",
                                            xhci->detail));
        goto fail;
    }
    path->vendor_id = (uint16_t)((uint16_t)descriptor[8] |
                                 ((uint16_t)descriptor[9] << 8));
    path->product_id = (uint16_t)((uint16_t)descriptor[10] |
                                  ((uint16_t)descriptor[11] << 8));
    path->device_seen = true;
    device->vendor_id = path->vendor_id;
    device->product_id = path->product_id;
    device->device_seen = true;

    memset(descriptor, 0, XHCI_DATA_DESCRIPTOR_MAX);
    if (!xhci_get_descriptor(device, slot_id, USB_DESCRIPTOR_CONFIGURATION, 0U,
                             USB_CONFIG_DESCRIPTOR_PREFIX, &received) ||
        received < USB_CONFIG_DESCRIPTOR_PREFIX) {
        xhci_attach_refuse(device, XHCI_STATUS_ENUMERATION_FAILED,
                           "CONFIG_DESCRIPTOR",
                           xhci_detail_port(port, "CONFIG_DESCRIPTOR_PREFIX",
                                            xhci->detail));
        goto fail;
    }
    config_total = usb_hid_config_total_length(descriptor,
                                               USB_CONFIG_DESCRIPTOR_PREFIX);
    if (config_total == 0U || config_total > USB_HID_CONFIG_DESCRIPTOR_MAX) {
        xhci_attach_refuse(device, XHCI_STATUS_NOT_A_KEYBOARD,
                           "CONFIG_DESCRIPTOR",
                           xhci_detail_port(port, "CONFIG_DESCRIPTOR",
                                            "CONFIG_TOO_LARGE"));
        goto fail;
    }
    memset(descriptor, 0, XHCI_DATA_DESCRIPTOR_MAX);
    if (!xhci_get_descriptor(device, slot_id, USB_DESCRIPTOR_CONFIGURATION, 0U,
                             config_total, &received) ||
        received < USB_CONFIG_DESCRIPTOR_PREFIX) {
        xhci_attach_refuse(device, XHCI_STATUS_ENUMERATION_FAILED,
                           "CONFIG_DESCRIPTOR",
                           xhci_detail_port(port, "CONFIG_DESCRIPTOR",
                                            xhci->detail));
        goto fail;
    }
    /* The configuration is the physical device's, so it is read once and kept
       on the path: every role's walk runs over these same bytes. */
    path->config_length = (uint16_t)(received > sizeof(path->config)
                                         ? sizeof(path->config)
                                         : received);
    memcpy(path->config, descriptor, path->config_length);
    (void)usb_hid_describe_interfaces(descriptor, received,
                                      path->interface_summary,
                                      sizeof(path->interface_summary));
    xhci_detail_copy(device->interface_summary, sizeof(device->interface_summary),
                     path->interface_summary);
    path->config_value = path->config[5];
    if (!xhci_control_transfer(device,
                               xhci_device_descriptor_offset(device->role),
                               slot_id, USB_REQUEST_TYPE_STANDARD,
                               USB_REQUEST_SET_CONFIGURATION,
                               path->config_value, 0U, 0U, false, NULL,
                               NULL)) {
        xhci_attach_refuse(device, XHCI_STATUS_ENUMERATION_FAILED,
                           "SET_CONFIGURATION",
                           xhci_detail_port(port, "SET_CONFIGURATION",
                                            xhci->detail));
        goto fail;
    }
    path->configured = true;
    return true;

fail:
    xhci_path_release(path);
    device->path = NULL;
    device->slot_id = 0U;
    return false;
}

static bool xhci_attach_device(struct xhci_hid_device *device,
                               uint32_t port, uint8_t speed) {
    const bool direction_in =
        true;
    const enum usb_hid_role role = device->role == XHCI_HID_ROLE_MOUSE
                                       ? USB_HID_ROLE_MOUSE
                                       : USB_HID_ROLE_KEYBOARD;
    const uint16_t ep0_max_packet = xhci_speed_ep0_max_packet(speed);
    struct usb_hid_config config;
    struct xhci_device_path *path;
    uint8_t slot_id = 0U;

    /* ITEM-578: this attempt's own record starts empty; every refusal below
       fills it, and a binding marks it OK. */
    device->attempt_made = true;
    device->attempt_status = XHCI_STATUS_UNKNOWN;
    device->attempt_stage = "ATTACH";
    device->attempt_detail[0] = '\0';
    device->report_descriptor_read = XHCI_EVIDENCE_RDESC_NONE;
    device->report_descriptor_parse = XHCI_EVIDENCE_RDESC_NONE;
    device->report_descriptor_length = 0U;
    memset(device->report_descriptor_bytes, 0,
           sizeof(device->report_descriptor_bytes));

    if (ep0_max_packet == 0U) {
        xhci_attach_refuse(device, XHCI_STATUS_UNSUPPORTED_SPEED, "SPEED",
                           xhci_detail_port(port, "SPEED",
                                            xhci_speed_string(speed)));
        return false;
    }

    /*
     * ITEM-579: one physical device is one path.  The first role on a port
     * establishes it (slot, address, EP0, configuration); the second role joins
     * the same path by interface - no port reset, no second Address Device, no
     * second SET_CONFIGURATION - because both interfaces are functions of one
     * device and the pointer may already be using it.
     */
    path = xhci_session_path(xhci, (uint8_t)port);
    if (path == NULL) {
        path = xhci_path_new(xhci);
        if (path == NULL) {
            xhci_attach_refuse(device, XHCI_STATUS_ENUMERATION_FAILED, "PATH",
                               xhci_detail_port(port, "PATH",
                                                "NO_FREE_PATH"));
            return false;
        }
        if (!xhci_path_establish(device, path, port, speed)) {
            return false;
        }
    }
    device->path = path;
    device->slot_id = path->slot_id;
    device->port = (uint8_t)port;
    device->speed = path->speed;
    device->vendor_id = path->vendor_id;
    device->product_id = path->product_id;
    device->device_seen = path->device_seen;
    xhci_detail_copy(device->interface_summary,
                     sizeof(device->interface_summary),
                     path->interface_summary);
    slot_id = path->slot_id;

    /*
     * The configuration descriptor was read once for the physical device; each
     * role's walk over the same bytes selects its own interface.
     */
    config = usb_hid_parse_config_role(path->config, path->config_length, role);
    if (config.status != USB_HID_STATUS_OK) {
        /*
         * ITEM-406/438: the interface was selected by its descriptor wherever
         * it sits in the configuration; an interface that declares no boot
         * protocol is judged by its report descriptor below, so this refusal
         * names the interface shape, not the device.
         */
        xhci_attach_refuse_verdict(
            device,
            device->role == XHCI_HID_ROLE_MOUSE ? XHCI_STATUS_NOT_A_MOUSE
                                                : XHCI_STATUS_NOT_A_KEYBOARD,
            "CONFIG_PARSE", config.status,
            xhci_detail_port(port, "CONFIG_PARSE",
                             usb_hid_status_string(config.status)));
        xhci_attach_abort(device);
        return false;
    }

    /*
     * ITEM-438: a report-protocol candidate is confirmed by its own report
     * descriptor before anything is bound.  The boot keyboard path is
     * unchanged and needs no read; the shapes the 2026-09-17 hardware run
     * refused (protocol 0, or no boot subclass at all) are exactly the ones
     * that reach this block, and a mouse, a consumer-control collection or a
     * broken descriptor is refused here by name.
     */
    if (config.report_descriptor_needed) {
        /*
         * ITEM-448: the same descriptor is asked the role's own question - a
         * keyboard's parse answers NOT_KEYBOARD, a pointer's answers NOT_MOUSE
         * - so a combined receiver's mouse collection cannot be bound by the
         * keyboard walk and the other way round.
         */
        const bool parsed = device->role == XHCI_HID_ROLE_MOUSE
                                ? xhci_parse_mouse_report_layout(
                                      device, (uint8_t)port,
                                      config.interface_number)
                                : xhci_parse_report_layout(
                                      device, (uint8_t)port,
                                      config.interface_number);

        if (!parsed) {
            xhci_attach_abort(device);
            return false;
        }
        device->report_mode = true;
    }
    /*
     * ITEM-448/579: each role owns its interrupt ring and its report queue; the
     * endpoint those transfers target is the role's own DCI on the shared
     * path's slot, so a second role on one device never touches this one.  The
     * ring must exist before Configure Endpoint publishes its address to the
     * controller.
     */
    xhci_ring_init(&xhci->interrupt_rings[device->role],
                   (struct xhci_trb *)xhci_dma_at(
                       xhci_device_interrupt_ring_offset(device->role)),
                   xhci_dma_physical(
                       xhci_device_interrupt_ring_offset(device->role)),
                   XHCI_INTERRUPT_RING_TRBS, 1U);
    device->dci = xhci_endpoint_dci(config.endpoint_number, direction_in);
    /*
     * ITEM-406: the endpoint context carries the descriptor's own
     * wMaxPacketSize (8 for a boot keyboard, and 8 for the acceptance host's
     * low-speed K120 - never an assumed 64) and the Interval field the
     * *speed* implies, not the raw bInterval.  ITEM-438 keeps the descriptor's
     * number as well, because for a low- or full-speed endpoint the field is
     * the exponent of the period the device asked for and the record has to
     * show both halves of that conversion.
     */
    device->endpoint_binterval = config.endpoint_interval;
    device->endpoint_interval =
        xhci_endpoint_interval(speed, config.endpoint_interval);
    if (device->endpoint_interval == 0U ||
        !xhci_configure_endpoint(device, slot_id, (uint8_t)port, device->dci,
                                 config.endpoint_max_packet,
                                 device->endpoint_interval)) {
        if (device->endpoint_interval == 0U) {
            xhci_attach_refuse(device, XHCI_STATUS_ENUMERATION_FAILED,
                               "ENDPOINT_INTERVAL",
                               xhci_detail_port(port, "ENDPOINT_INTERVAL",
                                                xhci_speed_string(speed)));
        } else {
            xhci_attach_refuse(device, xhci->status, "CONFIGURE_ENDPOINT",
                               xhci->detail);
        }
        xhci_attach_abort(device);
        return false;
    }
    /*
     * ITEM-438: boot protocol when the interface promised it.  A device that
     * declares the boot subclass (protocol 1 or 0) is asked for boot reports;
     * if it refuses, its report descriptor is read as the fallback rather than
     * losing the device - a keyboard that answers only report protocol is
     * still a keyboard.  An interface with no boot subclass is never asked:
     * SET_PROTOCOL is not defined for it, and the report descriptor was
     * already read above.
     *
     * ITEM-639: a boot mouse's three-byte report has no wheel, so a boot mouse
     * whose own descriptor declares one is first asked for report protocol.
     * Only a successful request changes the binding; every other answer keeps
     * the boot protocol the interface declared.
     */
    if (config.interface_subclass == USB_HID_SUBCLASS_BOOT &&
        !device->report_mode) {
        const bool wheel_report =
            device->role == XHCI_HID_ROLE_MOUSE &&
            xhci_mouse_enable_wheel_report_protocol(
                device, config.interface_number);

        if (!wheel_report &&
            !xhci_control_transfer(device,
                                   xhci_device_descriptor_offset(device->role),
                                   slot_id, 0x21U,
                                   USB_HID_REQUEST_SET_PROTOCOL,
                                   USB_HID_BOOT_PROTOCOL_VALUE,
                                   config.interface_number, 0U, false,
                                   NULL, NULL)) {
            /*
             * ITEM-578: the fallback reads the descriptor for the role being
             * bound.  A boot mouse that refuses SET_PROTOCOL must not be
             * classified by the keyboard walk - the parse helper publishes the
             * role's own stage, reason and read result.
             */
            const bool fallback_parsed =
                device->role == XHCI_HID_ROLE_MOUSE
                    ? xhci_parse_mouse_report_layout(device, (uint8_t)port,
                                                     config.interface_number)
                    : xhci_parse_report_layout(device, (uint8_t)port,
                                               config.interface_number);

            if (!fallback_parsed) {
                xhci_attach_abort(device);
                return false;
            }
            device->report_mode = true;
        }
    }
    /*
     * ITEM-438: idle with a duration, not zero.  A keyboard that honours it
     * re-sends its report once a second even when nothing changed, which is
     * what makes "this binding is alive" an observation instead of an
     * assumption; the request is still optional (a device may refuse idle
     * entirely), so a refusal is published and the keyboard is kept.
     */
    if (device->role == XHCI_HID_ROLE_MOUSE) {
        /*
         * ITEM-448: a pointer is asked for idle rate zero - "report only when
         * the state changes" (HID 1.11 section 7.2.4).  A boot mouse report
         * carries *relative* deltas, so an idle re-send of an unchanged report
         * would repeat the last movement and walk the pointer across the
         * screen on its own; silence is therefore normal for a mouse and is
         * published by the [MOUSE] finding, not proved away here.
         */
        (void)xhci_control_transfer(device,
                                    xhci_device_descriptor_offset(device->role),
                                    slot_id, 0x21U,
                                    USB_HID_REQUEST_SET_IDLE, 0U,
                                    config.interface_number, 0U, false, NULL,
                                    NULL);
    } else if (!xhci_control_transfer(device,
                                      xhci_device_descriptor_offset(
                                          device->role),
                                      slot_id, 0x21U,
                                      USB_HID_REQUEST_SET_IDLE,
                                      USB_HID_IDLE_PERIOD,
                                      config.interface_number, 0U, false,
                                      NULL, NULL)) {
        device->idle_refused = true;
        (void)xhci_control_transfer(device,
                                    xhci_device_descriptor_offset(device->role),
                                    slot_id, 0x21U,
                                    USB_HID_REQUEST_SET_IDLE, 0U,
                                    config.interface_number, 0U, false, NULL,
                                    NULL);
    }

    device->endpoint_state = XHCI_EP_STATE_RUNNING;
    device->interface_number = config.interface_number;
    device->interface_protocol = config.interface_protocol;
    device->endpoint_address = config.endpoint_address;
    device->endpoint_max_packet = config.endpoint_max_packet;
    /*
     * ITEM-438: a report-protocol transfer may carry the endpoint's whole
     * packet, and the layout says how much of it is a report.  ITEM-448: a
     * boot mouse asks for four bytes - the three boot fields and the wheel
     * byte the report is commonly extended with - and for no more than its
     * endpoint declares; a report-protocol mouse asks for its endpoint packet,
     * which is what its layout's bytes arrive in.
     */
    if (device->role == XHCI_HID_ROLE_MOUSE) {
        /*
         * ITEM-640: a report-protocol pointer's descriptor may declare a
         * report longer than one endpoint packet (the physical 04D9:A09F
         * declares seven bytes).  An interrupt transfer can span packets, so
         * request the whole layout even when the endpoint's packet is
         * smaller; a shorter request would truncate the axes or the wheel
         * before the decoder ever saw them.
         */
        const uint16_t report_type_length =
            device->report_mode &&
                    device->mouse_layout.report_length >
                        config.endpoint_max_packet
                ? device->mouse_layout.report_length
                : config.endpoint_max_packet;

        device->report_length =
            device->report_mode
                ? report_type_length
                : (config.endpoint_max_packet <
                           USB_HID_BOOT_MOUSE_TRANSFER_LENGTH
                       ? config.endpoint_max_packet
                       : USB_HID_BOOT_MOUSE_TRANSFER_LENGTH);
    } else {
        device->report_length = device->report_mode
                                    ? config.endpoint_max_packet
                                    : USB_HID_BOOT_REPORT_LENGTH;
    }
    usb_hid_state_reset(&device->hid);
    usb_hid_mouse_state_reset(&device->mouse);
    device->transfer_pending = false;
    device->consecutive_errors = 0U;
    /* ITEM-436: the traffic counters describe this keyboard, so they start
       where its binding does. */
    device->report_count = 0U;
    device->transfer_count = 0U;
    device->error_count = 0U;
    /* ITEM-579: submitted, completed and decoded are separate questions. */
    device->completion_count = 0U;
    device->halted_count = 0U;
    device->endpoint_restarts = 0U;
    device->endpoint_state = 0U;
    device->refused_report_count = 0U;
    device->last_report_bytes = 0U;
    /* ITEM-438: and how many keys those reports carried. */
    device->key_count = 0U;
    /* ITEM-467: the re-arm/completion evidence belongs to this binding too. */
    device->repost_count = 0U;
    device->last_report_ticks = 0U;
    /* ITEM-468: so does the short-packet completion count. */
    device->short_packet_count = 0U;
    device->last_transfer_isp = false;
    /* ITEM-469: the transfer queue and the completion-code counts belong to
       this binding; the queue is filled to the armed depth before the binding
       is declared. */
    device->armed_count = 0U;
    device->armed_head = 0U;
    device->mse_count = 0U;
    device->event_ring_full_count = 0U;
    for (uint32_t code = 0U; code < XHCI_COMPLETION_CODE_COUNT; ++code) {
        device->completion_code_counts[code] = 0U;
    }
    /* ITEM-479: a fresh binding starts a fresh silence clock. */
    device->watchdog_progress_ticks = 0U;
    device->watchdog_count = 0U;
    device->watchdog_silence_ms_max = 0U;
    device->watchdog_reported = false;
    /* ITEM-482: and fresh silence/recovery evidence. */
    device->watchdog_active_count = 0U;
    device->watchdog_escalations = 0U;
    device->watchdog_recover_us_max = 0U;
    device->rate_window_ticks = 0U;
    device->rate_window_reports = 0U;
    device->rate_per_second = 0U;
    device->service_gap_window_ms_max = 0U;
    device->service_gap_window_ticks = 0U;
    device->ready = true;
    xhci_top_up_device(device);
    if (device->armed_count == 0U) {
        device->ready = false;
        xhci_attach_refuse(device, XHCI_STATUS_ENUMERATION_FAILED,
                           "INTERRUPT_TRANSFER",
                           xhci_detail_port(port, "INTERRUPT_TRANSFER",
                                            "SUBMIT_FAILED"));
        xhci_attach_abort(device);
        return false;
    }
    /* ITEM-579: the role is attached to the physical path; from here a re-bind
       of the other role reuses this slot instead of resetting the device. */
    path->roles |= (uint8_t)(1U << device->role);
    xhci_attach_refuse(device, XHCI_STATUS_OK, "BOUND", "OK");
    return true;
}

/*
 * ITEM-579: a physical reset that has to happen while roles are live on the
 * device is a path-wide operation.
 *
 * A per-endpoint repair (Reset Endpoint + Set TR Dequeue Pointer) touches one
 * endpoint and leaves the other role's endpoint alone, so it is the first
 * answer.  When that repair fails and the device has to be reset at the wire
 * instead, every role of the device is stopped first, the port is reset, and
 * every role that was bound is brought back on the new path - no role may be
 * left holding a slot that no longer answers.  The caller is the session's
 * current `xhci`.
 */
static bool xhci_path_restore(struct xhci_session *session,
                              struct xhci_device_path *path) {
    bool was_ready[XHCI_HID_DEVICES];
    uint8_t port;
    uint8_t speed;
    bool restored = true;

    if (session == NULL || path == NULL || !path->valid) {
        return false;
    }
    port = path->port;
    speed = path->speed;
    for (uint32_t role = 0U; role < XHCI_HID_DEVICES; ++role) {
        was_ready[role] = session->devices[role].ready &&
                          session->devices[role].path == path;
    }
    for (uint32_t role = 0U; role < XHCI_HID_DEVICES; ++role) {
        struct xhci_hid_device *device = &session->devices[role];

        if (!was_ready[role]) {
            continue;
        }
        device->ready = false;
        device->transfer_pending = false;
        device->armed_count = 0U;
        device->armed_head = 0U;
    }
    xhci_path_release(path);
    {
        uint8_t reopened = speed;

        if (!xhci_port_reset(port, &reopened, true) || reopened == 0U) {
            restored = false;
        } else {
            for (uint32_t role = 0U; role < XHCI_HID_DEVICES; ++role) {
                if (!was_ready[role]) {
                    continue;
                }
                if (!xhci_attach_device(&session->devices[role], port,
                                        reopened)) {
                    restored = false;
                }
            }
        }
    }
    return restored;
}

/*
 * ITEM-407: what the scan saw, kept for the record.
 *
 * The port values are the ones the scan read after claiming each port, and the
 * two operational registers are read here, when the scan is over, so the record
 * says whether the controller was still running while its ports were read.
 */
static void xhci_probe_note_evidence(
    const uint32_t *portsc,
    uint32_t ports,
    uint32_t powered_off) {
    if (xhci->regs == NULL) {
        return;
    }
    for (uint32_t index = 0U; index < XHCI_PORT_EVIDENCE_MAX; ++index) {
        xhci->portsc[index] = index < ports ? portsc[index] : 0U;
    }
    xhci->portsc_ports = ports;
    xhci->portsc_powered_mask = xhci_ports_powered_mask(portsc, ports);
    xhci->portsc_powered_off = powered_off;
    xhci->usbcmd = xhci_read32(xhci_operational(XHCI_OP_USBCMD), 0U);
    xhci->usbsts = xhci_read32(xhci_operational(XHCI_OP_USBSTS), 0U);
}

/*
 * ITEM-407: claim the root port for the scan before its connect status is
 * believed, and keep the PORTSC value the scan saw.
 *
 * The driver used to write PORTSC.PP only inside xhci_port_reset(), i.e. only
 * for a port it had *already* decided to reset - and the scan decides that from
 * PORTSC.CCS. On a controller whose ports start unpowered (PORTSC.PP reads 0
 * after the reset, which is a state the hardware may come up in and which
 * Linux's xhci-hub never leaves to chance: it powers every root port through
 * USB_PORT_FEAT_POWER, "turn on ports, even if there isn't per-port
 * switching") such a port reports no connect and would never be powered,
 * because the only code that powers a port is behind a connect it cannot see.
 * That is a closed loop the driver cannot escape, and it is exactly the shape
 * the acceptance host's second controller showed: a keyboard on root port 2 and
 * `NO_CONNECTED_DEVICE` for every port.
 *
 * Power is therefore asserted before the status is read, exactly as the ITEM-406
 * fix made every other PORTSC write: xhci_portsc_neutral() carries only the
 * bits software may write back, so PED and the change bits stay out of it, and
 * PORTSC.PP is added explicitly. The read is the port's answer, and it is the
 * value the `[XHCI]` record publishes, so the next hardware boot shows both the
 * power state the port came up in and whether CCS rose with it.
 */
static uint32_t xhci_port_claim(uint32_t port, uint32_t *power_was_off) {
    volatile uint8_t *portsc = xhci_portsc(port);
    const uint32_t before = xhci_read32(portsc, 0U);

    if ((before & XHCI_PORTSC_PP) == 0U) {
        if (power_was_off != NULL) {
            *power_was_off += 1U;
        }
        xhci_write32(portsc, 0U, xhci_portsc_claim(before));
    }
    return xhci_read32(portsc, 0U);
}

/* ITEM-453: how often the settle windows below re-read a port.  Ten steps a
   window keeps the total sleep of a fully empty controller bounded by the
   window itself, and the first step is a read, so a port that is already
   connected costs one read and no sleep at all. */
#define XHCI_PORT_SETTLE_STEP_MS 10U

/*
 * ITEM-453: one root port, brought up before its answer is believed.
 *
 * ITEM-407 taught the scan to assert PORTSC.PP before it reads CCS, which is
 * what a port that came up unpowered needs to report anything.  The 2026-09-19
 * hardware run showed that half is not enough: the owner's receiver sits on
 * `05:00.3` - the controller Linux enumerates it on - and the driver published
 * `pp=3 ppoff=0` and `0x000002A0` for that controller's six ports, i.e. power
 * was already on, the link was in RxDetect, and every port answered "nothing
 * connected".  A port in Disabled, Inactive, Compliance or Test Mode answers
 * CCS = 0 no matter what is plugged into it, and a port that has just been
 * powered or moved back to RxDetect needs up to TATTDB (100 ms, USB 2.0
 * section 7.1.7.3) before its connect status means anything.
 *
 * The sequence, all of it bounded:
 *
 *   1. read PORTSC and let xhci_port_bring_up_plan() decide (pure, host-pinned);
 *   2. assert power when PP was clear;
 *   3. move a link that cannot sample a connect back to RxDetect with LWS;
 *   4. when the port was written to, re-read it for up to XHCI_PORT_SETTLE_MS
 *      in XHCI_PORT_SETTLE_STEP_MS steps, and stop the window the moment CCS
 *      rises - the wait is for a connect that has not appeared yet, never a
 *      fixed delay;
 *   5. return the value the scan then believes, which is also what the port's
 *      own `[XHCI-PORT]` line publishes.
 *
 * A controller that has just been reset gets one shared window before any of
 * its ports is believed empty (xhci_ports_settle_after_reset); this per-port
 * window is only for the ports the driver actually changed.
 */
static uint32_t xhci_port_bring_up(
    uint32_t port,
    uint32_t *power_was_off,
    struct xhci_port_bring_up *plan,
    uint32_t *waited_ms) {
    volatile uint8_t *portsc = xhci_portsc(port);
    const uint32_t first = xhci_read32(portsc, 0U);
    uint32_t waited = 0U;

    xhci_port_bring_up_plan(first, plan);
    if (plan->power_on) {
        if (power_was_off != NULL) {
            *power_was_off += 1U;
        }
        xhci_write32(portsc, 0U, xhci_portsc_claim(first));
    }
    if (plan->link_kick) {
        xhci_write32(portsc, 0U, xhci_portsc_link_kick(first));
    }
    if (plan->settle_ms != 0U) {
        while (waited < plan->settle_ms) {
            if ((xhci_read32(portsc, 0U) & XHCI_PORTSC_CCS) != 0U) {
                break;
            }
            timer_delay_ms(XHCI_PORT_SETTLE_STEP_MS);
            waited += XHCI_PORT_SETTLE_STEP_MS;
        }
    }
    if (waited_ms != NULL) {
        *waited_ms = waited;
    }
    return xhci_read32(portsc, 0U);
}

/*
 * ITEM-453: the controller's own connect-debounce window.
 *
 * A host controller reset resets every port state machine, and a device that
 * was attached all along is re-detected by the controller after it comes back
 * up - not necessarily before the first PORTSC read.  The scan therefore waits
 * once per controller, before any port is believed empty.
 *
 * ITEM-545: the window may not end because *one* port answered.  A SuperSpeed
 * link is only visible after it finishes training, and the USB 2 port beside it
 * reports its full-speed mouse within milliseconds - the 2026-10-17 hardware
 * run is the case in point: the mouse on the USB 2 half enumerated, the
 * SuperSpeed stick on the USB 3 half of the same controller did not, and a
 * window that returned on the first connect never gave the USB 3 port its
 * training time.  Linux's xhci_start() polls the roothubs for the same reason
 * ("SS devices are only visible to roothub after link training completes",
 * 500 ms of run_graceperiod), so the wait here is bounded by
 * XHCI_PORT_TRAINING_MS and ends the moment no port is left that can still
 * report a connect.  QEMU's model answers instantly; a real controller is what
 * this window is for.
 */
static void xhci_ports_settle_after_reset(void) {
    uint32_t elapsed = 0U;
    bool quiet = false;

    for (uint32_t waited = 0U; waited < XHCI_PORT_TRAINING_MS;
         waited += XHCI_PORT_SETTLE_STEP_MS) {
        bool pending = false;

        elapsed = waited;
        for (uint32_t port = 1U; port <= xhci->max_ports; ++port) {
            if (xhci_port_connect_pending(
                    xhci_read32(xhci_portsc(port), 0U))) {
                pending = true;
            }
        }
        if (!pending) {
            quiet = true;
            break;
        }
        timer_delay_ms(XHCI_PORT_SETTLE_STEP_MS);
    }
    /* ITEM-574: a per-port `wait_ms=0` does not prove the machine did not
       wait: this shared window runs once per controller and is recorded here,
       so the export distinguishes "no port ever asked for a connect" from "the
       whole window was spent waiting for one". */
    xhci->settle_ms = quiet ? elapsed : XHCI_PORT_TRAINING_MS;
    xhci->settle_bound_ms = XHCI_PORT_TRAINING_MS;
    xhci->settle_pending = quiet ? 0U : 1U;
}

/* ITEM-453: one `[XHCI-PORT]` line per port, defined next to the other record
   composers below (the publication macros live there). */
static void xhci_log_port_record(
    uint32_t port,
    const struct xhci_port_bring_up *plan,
    uint32_t now,
    uint32_t waited_ms);
static void xhci_log_protocol_records(void);

/*
 * ITEM-438: how long a freshly bound candidate is given to prove it answers.
 *
 * A live keyboard was asked for an idle report every USB_HID_IDLE_PERIOD x 4 ms
 * (one second), so 1200 ms covers a whole idle period plus the enumeration and
 * event-ring latency around it.  A device that never answers is not given the
 * driver's whole 30 s NO_REPORT window here: the window is what the heartbeat
 * publishes, while the walk needs a bound that is short enough to try the next
 * candidate - the 2026-09-17 machine had one silent receiver and one wired
 * keyboard, and the wired keyboard's port was never reached because the first
 * device that parsed was treated as the answer.
 */
#define XHCI_CANDIDATE_PROVE_MS 1200U
#define XHCI_CANDIDATE_PROVE_STEP_MS 10U

/*
 * ITEM-438: publish the candidate that was bound and did not answer.
 *
 * One line, on both surfaces, naming the device (vid/pid/ifaces), the binding
 * (port/speed/iface/protocol) and the counters that were zero when the window
 * closed - so the next hardware run reads *why* the walk moved on instead of
 * guessing from a READY record that never typed.
 *
 * The marker is `[KBD ]`, not `[XHCI] state=`: this is a statement about a
 * keyboard candidate, and the `[XHCI] state=` records are one-per-controller
 * evidence the proofs count exactly (ITEM-406/407).  An extra line under that
 * marker would read as a controller the scan never visited.
 */
static void xhci_log_candidate_rejection(struct xhci_hid_device *device,
                                         uint32_t port, uint8_t speed) {
    const uint64_t flags = serial_sequence_begin();

    console_journal_begin();
    serial_write(device->role == XHCI_HID_ROLE_MOUSE
                     ? "[MOUSE] state=UNAVAILABLE reason=NO_REPORT detail=P"
                     : "[KBD ] state=UNAVAILABLE reason=NO_REPORT detail=P");
    console_record_text(device->role == XHCI_HID_ROLE_MOUSE
                            ? "[MOUSE] state=UNAVAILABLE reason=NO_REPORT detail=P"
                            : "[KBD ] state=UNAVAILABLE reason=NO_REPORT detail=P");
    serial_write_u64(port);
    console_record_u64(port);
    serial_write("_SILENT port=");
    console_record_text("_SILENT port=");
    serial_write_u64(port);
    console_record_u64(port);
    serial_write(" speed=");
    console_record_text(" speed=");
    serial_write(xhci_speed_string(speed));
    console_record_text(xhci_speed_string(speed));
    /*
     * ITEM-452: the shared hexadecimal writer emits the "0x" prefix itself, so
     * the fragment names the key only.  The record used to print "0x" twice
     * ("vid=0x0x0000000000000627"), which no other record in this kernel does
     * and which a reader parsing the identity had to know about.
     */
    serial_write(" vid=");
    console_record_text(" vid=");
    serial_write_hex(device->vendor_id);
    console_record_hex64(device->vendor_id);
    serial_write(" pid=");
    console_record_text(" pid=");
    serial_write_hex(device->product_id);
    console_record_hex64(device->product_id);
    serial_write(" ifaces=");
    console_record_text(" ifaces=");
    serial_write(device->interface_summary[0] != '\0'
                     ? device->interface_summary
                     : "NONE");
    console_record_text(device->interface_summary[0] != '\0'
                            ? device->interface_summary
                            : "NONE");
    serial_write(" iface=");
    console_record_text(" iface=");
    serial_write_u64(device->interface_number);
    console_record_u64(device->interface_number);
    serial_write(" protocol=");
    console_record_text(" protocol=");
    serial_write_u64(device->interface_protocol);
    console_record_u64(device->interface_protocol);
    serial_write(" reports=");
    console_record_text(" reports=");
    serial_write_u64(device->report_count);
    console_record_u64(device->report_count);
    serial_write(" transfers=");
    console_record_text(" transfers=");
    serial_write_u64(device->transfer_count);
    console_record_u64(device->transfer_count);
    serial_write(" errors=");
    console_record_text(" errors=");
    serial_write_u64(device->error_count);
    console_record_u64(device->error_count);
    serial_write(" waited=");
    console_record_text(" waited=");
    serial_write_u64(XHCI_CANDIDATE_PROVE_MS);
    console_record_u64(XHCI_CANDIDATE_PROVE_MS);
    serial_write("ms\n");
    console_record_text("ms");
    serial_sequence_end(flags);
    console_journal_end();
}

/*
 * ITEM-459: a key the machine cannot name names itself.
 *
 * The 2026-09-19 hardware run pressed the key left of Enter on an ISO keyboard
 * and got nothing: its usage (0x64, `Keyboard Non-US \ and |`) was outside the
 * character table, so the decoder returned no character and the caller skipped
 * the key in silence.  The tables reach it now; this record is for the keys
 * that still have no character, so the next dead key is a code in the log
 * rather than a mystery.  It is published once per distinct usage - a held key
 * would otherwise repeat it on every report - under the `[KBD ]` marker the
 * keyboard's other findings use, because `[XHCI] state=` records are
 * one-per-controller evidence the proofs count exactly.
 */
static void xhci_log_unmapped_usage(uint8_t usage, uint8_t modifiers) {
    const uint64_t flags = serial_sequence_begin();

    console_journal_begin();
    serial_write("[KBD ] unmapped usage=");
    console_record_text("[KBD ] unmapped usage=");
    serial_write_hex(usage);
    console_record_hex64(usage);
    serial_write(" modifier=");
    console_record_text(" modifier=");
    serial_write_hex(modifiers);
    console_record_hex64(modifiers);
    serial_write("\n");
    console_record_text("\n");
    serial_sequence_end(flags);
    console_journal_end();
}

static void xhci_note_unmapped_usage(struct xhci_hid_device *device) {
    uint8_t usage = 0U;
    uint8_t modifiers = 0U;
    uint32_t word;
    uint64_t bit;

    if (device == NULL ||
        !usb_hid_take_unmapped(&device->hid, &usage, &modifiers)) {
        return;
    }
    word = (uint32_t)usage >> 6U;
    bit = 1ULL << (usage & 63U);
    if (word >= 4U || (device->unmapped_seen[word] & bit) != 0U) {
        return;
    }
    device->unmapped_seen[word] |= bit;
    xhci_log_unmapped_usage(usage, modifiers);
}

/*
 * ITEM-438: give the just-bound candidate its window, servicing the event ring
 * the whole time.  Returns true as soon as one whole report has been decoded -
 * which, with the idle request, a live keyboard produces without being touched.
 */
static bool xhci_candidate_prove(struct xhci_hid_device *device) {
    uint32_t waited = 0U;

    while (waited <= XHCI_CANDIDATE_PROVE_MS) {
        xhci_keyboard_service();
        if (!device->ready) {
            return false;
        }
        if (device->report_count > 0U) {
            return true;
        }
        timer_delay_ms(XHCI_CANDIDATE_PROVE_STEP_MS);
        waited += XHCI_CANDIDATE_PROVE_STEP_MS;
    }
    xhci_keyboard_service();
    return device->ready && device->report_count > 0U;
}

/* ITEM-438: give a silent candidate's slot back so the walk can go on.  The
   counters and the binding stay in the state for the record that was just
   published; only the binding itself is released. */
static void xhci_release_candidate(struct xhci_hid_device *device) {
    /* ITEM-579: the role leaves the physical path; the path (and with it the
       other role's slot, address and endpoint) survives while a role remains
       on it. */
    xhci_attach_abort(device);
}


/*
 * ITEM-406: visit every root port of the controller that is up.  Every port
 * costs one PORTSC read; a port with nothing attached costs exactly that; a
 * port whose device is a mouse, a receiver or anything else that is not a boot
 * keyboard costs one bounded attach attempt and the probe goes on.  What the
 * controller publishes when nothing on it is a keyboard is the most
 * informative outcome of its ports, with the port and the stage in the detail.
 *
 * ITEM-407: every port's PORTSC value is kept (the first
 * XHCI_PORT_EVIDENCE_MAX of them) and published with the controller's own
 * registers in the `[XHCI]` record, so a controller that finds no device says
 * which ports it looked at and what they answered.
 *
 * ITEM-438: a device that parses as a keyboard is not the end of the walk by
 * itself.  It is bound, given XHCI_CANDIDATE_PROVE_MS to answer, and only a
 * proven binding is the machine's keyboard; a silent one is published as
 * `reason=NO_REPORT`, released, and the remaining ports and controllers are
 * still visited.  The first silent candidate is remembered for the caller, so
 * a machine whose only keyboard ignores the idle request still ends with that
 * keyboard bound.
 *
 * ITEM-448: the walk binds two devices, and they are searched for
 * independently.  Every port that carries a usable device is offered to each
 * role the machine is still missing, in role order: the device is claimed by
 * the first role whose descriptor walk accepts it, so a keyboard is never
 * mistaken for a mouse and a mouse never costs the machine its keyboard.
 *
 * ITEM-452: the walk no longer stops anywhere.  It visits every controller the
 * PCI scan found and every port of each, because the owner's machine has the
 * keyboard and the pointer on two different controllers and "plugged in but
 * seen=0" is not an acceptable record.  A role that is already bound
 * machine-wide is not offered a later device - that device is still published,
 * as REFUSED with reason=ROLE_TAKEN - and every device that is reached gets
 * its own `[USB ]` line with its port, speed, role, identity and reason.
 */
static void xhci_log_device_record(
    uint32_t port,
    uint8_t speed,
    const char *role,
    const char *state,
    const char *reason,
    const char *detail,
    const struct xhci_hid_device *identity);
static bool xhci_role_ready(uint8_t role) {
    return xhci_role_session(role) != NULL;
}

/* What one port's role attempts produced, kept for the device record. */
struct xhci_port_attempt {
    /* The last device that was attempted, for its identity, or NULL. */
    struct xhci_hid_device *device;
    /* The role that bound the device, or the last one that was offered it. */
    const char *role;
    const char *reason;
    const char *state;
    /* The detail of the attempt, copied out of the driver's working buffer. */
    char detail[XHCI_DETAIL_BYTES];
    bool has_detail;
    bool tried;
};

/*
 * ITEM-452: the walk visits every controller the PCI scan found and every port
 * of each.  What one port's role attempts produced is kept for the device
 * record, so a device that was reached is named even when neither role could
 * bind it.
 */
static void xhci_probe_ports(struct xhci_scan *scan) {
    bool saw_device = false;
    bool keyboard_proven = xhci_machine.keyboard_session != NULL;
    enum xhci_status best_status = XHCI_STATUS_NO_DEVICE;
    /* The most informative rejection of the ports visited, kept in this
       function's own storage: xhci_detail_port() rewrites its buffer on every
       call, so a pointer into it would name the wrong port by the next one. */
    char best_scratch[XHCI_DETAIL_BYTES];
    const char *best_detail = "NO_CONNECTED_DEVICE";
    const uint32_t evidence_ports = xhci_port_evidence_count(xhci->max_ports);
    uint32_t evidence[XHCI_PORT_EVIDENCE_MAX];
    uint32_t powered_off = 0U;
    bool role_bound_here = false;
    /* ITEM-452: whether the keyboard role was actually offered to a device on
       this controller.  When it was not, the controller's verdict is not a
       refusal it never made - it is that the machine already has a keyboard. */
    bool keyboard_attempted = false;
    /* ITEM-519 increment 2: storage's own attempt budget for this
       controller, spent only on ports no HID role bound. */
    uint32_t storage_attempts = 0U;

    for (uint32_t index = 0U; index < XHCI_PORT_EVIDENCE_MAX; ++index) {
        evidence[index] = 0U;
    }

    /*
     * ITEM-453: the controller's protocol groups and its own connect-debounce
     * window come before any port is read: the groups label the port records,
     * and the window is what makes the first "nothing connected" of a
     * just-reset controller mean something.
     */
    xhci_log_protocol_records();
    xhci->ports_seen = xhci->max_ports;
    xhci_ports_settle_after_reset();

    for (uint32_t port = 1U; port <= xhci->max_ports; ++port) {
        struct xhci_port_bring_up bring;

        /* ITEM-577: after the controller's first fatal condition no further
           port or role is offered a command; the snapshot already names it. */
        if (xhci->fatal_valid) {
            break;
        }
        uint32_t waited = 0U;
        const uint32_t portsc =
            xhci_port_bring_up(port, &powered_off, &bring, &waited);
        uint8_t speed = (uint8_t)((portsc & XHCI_PORTSC_SPEED_MASK) >>
                                  XHCI_PORTSC_SPEED_SHIFT);
        struct xhci_port_attempt attempt;

        memset(&attempt, 0, sizeof(attempt));
        attempt.role = "NONE";
        attempt.state = "REFUSED";
        /* ITEM-573: keep the port's own two words and the actions taken, for
           the evidence that outlives the session. */
        if (port <= XHCI_EVIDENCE_PORTS) {
            struct xhci_evidence_port *kept =
                &xhci->port_evidence[port - 1U];

            kept->was = bring.was;
            kept->now = portsc;
            kept->waited_ms = (uint16_t)waited;
            kept->powered = bring.power_on ? 1U : 0U;
            kept->kicked = bring.link_kick ? 1U : 0U;
            kept->connected = (portsc & XHCI_PORTSC_CCS) != 0U ? 1U : 0U;
            if (xhci->port_evidence_count < port) {
                xhci->port_evidence_count = port;
            }
        }
        /* ITEM-453: every port names itself, connected or not, before the walk
           decides anything about the device that may be on it. */
        xhci_log_port_record(port, &bring, portsc, waited);
        if (port <= evidence_ports) {
            evidence[port - 1U] = portsc;
        }
        if ((portsc & XHCI_PORTSC_CCS) == 0U) {
            continue;
        }
        saw_device = true;
        if ((portsc & XHCI_PORTSC_PED) == 0U || speed == 0U) {
            if (!xhci_port_reset(port, &speed, false)) {
                if (xhci_scan_prefers(XHCI_STATUS_UNSUPPORTED_SPEED,
                                      best_status)) {
                    best_status = XHCI_STATUS_UNSUPPORTED_SPEED;
                    best_detail = xhci_detail_hold(
                        best_scratch, sizeof(best_scratch),
                        xhci_detail_port(port, "PORT_RESET", "TIMEOUT"));
                }
                attempt.reason = "UNSUPPORTED_SPEED";
                xhci_detail_copy(attempt.detail, sizeof(attempt.detail),
                                 xhci_detail_port(port, "PORT_RESET",
                                                  "TIMEOUT"));
                attempt.has_detail = true;
                xhci_log_device_record(port, speed, attempt.role,
                                       attempt.state, attempt.reason,
                                       attempt.detail, NULL);
                continue;
            }
        }
        if (xhci_speed_ep0_max_packet(speed) == 0U) {
            /*
             * ITEM-406: the port speed field is what a low-speed USB 1.1
             * device reports (2, XHCI_SPEED_LOW) and it is accepted here;
             * only a speed this driver cannot address at all is rejected, and
             * the record names the port, the stage and the speed.
             */
            if (xhci_scan_prefers(XHCI_STATUS_UNSUPPORTED_SPEED, best_status)) {
                best_status = XHCI_STATUS_UNSUPPORTED_SPEED;
                best_detail = xhci_detail_hold(
                    best_scratch, sizeof(best_scratch),
                    xhci_detail_port(port, "SPEED",
                                     xhci_speed_string(speed)));
            }
            attempt.reason = "UNSUPPORTED_SPEED";
            xhci_detail_copy(attempt.detail, sizeof(attempt.detail),
                             xhci_detail_port(port, "SPEED",
                                              xhci_speed_string(speed)));
            attempt.has_detail = true;
            xhci_log_device_record(port, speed, attempt.role, attempt.state,
                                   attempt.reason, attempt.detail, NULL);
            continue;
        }

        /*
         * ITEM-452: a device the machine did not need is REFUSED with
         * reason=ROLE_TAKEN rather than silently skipped: both roles are bound
         * elsewhere, so no attempt is spent on it and no descriptor is read -
         * but the port still names the device it carries.
         */
        attempt.reason = "ROLE_TAKEN";
        for (uint8_t role = 0U; role < XHCI_HID_DEVICES; ++role) {
            struct xhci_hid_device *device = xhci_device(role);
            const bool keyboard = role == XHCI_HID_ROLE_KEYBOARD;

            if (device->ready) {
                continue;
            }
            /*
             * ITEM-448: once a keyboard has answered, the remaining ports are
             * visited for the pointer only.  Offering them to the keyboard
             * role again would spend the attach budget on a device the machine
             * already has, and a later refusal would overwrite the proven
             * binding's status with a rejection.  ITEM-452 extends the rule
             * across controllers: a role the machine has bound anywhere is not
             * offered another device, and the device is still published as
             * REFUSED with reason=ROLE_TAKEN.
             */
            if (role == XHCI_HID_ROLE_KEYBOARD && keyboard_proven) {
                continue;
            }
            if (!keyboard && xhci_machine.mouse_session != NULL) {
                continue;
            }
            attempt.role = keyboard ? "KEYBOARD" : "MOUSE";
            /*
             * ITEM-578: the previous role addressed this device, so it answers
             * only the address it was given and the next Address Device would
             * fail at the wire (the a853d322 run's pointer never got past it).
             * A real USB reset returns the device to the Default state before
             * the port is offered to the next role; the port is still enabled,
             * which is why xhci_port_reset() has to be forced here.
             */
            if (attempt.tried &&
                xhci_session_path(xhci, (uint8_t)port) == NULL) {
                uint8_t reopened = speed;

                if (!xhci_port_reset(port, &reopened, true) ||
                    reopened == 0U) {
                    attempt.state = "REFUSED";
                    attempt.reason = "ENUMERATION_FAILED";
                    xhci_detail_copy(attempt.detail, sizeof(attempt.detail),
                                     xhci_detail_port(port, "PORT_RESET",
                                                      "TIMEOUT"));
                    attempt.has_detail = true;
                    if (xhci_scan_prefers(XHCI_STATUS_UNSUPPORTED_SPEED,
                                          best_status)) {
                        best_status = XHCI_STATUS_UNSUPPORTED_SPEED;
                        best_detail = xhci_detail_hold(
                            best_scratch, sizeof(best_scratch),
                            xhci_detail_port(port, "PORT_RESET", "TIMEOUT"));
                    }
                    break;
                }
                speed = reopened;
            }
            if (!xhci_scan_take_attempt(scan)) {
                /*
                 * This controller has spent its whole attach budget on devices
                 * that are not the ones it is looking for.  The controller's
                 * verdict says so - and ITEM-452 keeps walking its remaining
                 * ports anyway: every connected device still has to name
                 * itself, and each one is published as REFUSED with this
                 * reason without spending an attempt it does not have.
                 */
                attempt.tried = true;
                attempt.reason = "ENUMERATION_FAILED";
                xhci_detail_copy(attempt.detail, sizeof(attempt.detail),
                                 xhci_detail_port(port, "ATTACH_BUDGET",
                                                  "EXHAUSTED"));
                attempt.has_detail = true;
                if (xhci_scan_prefers(XHCI_STATUS_ENUMERATION_FAILED,
                                      best_status)) {
                    best_status = XHCI_STATUS_ENUMERATION_FAILED;
                    best_detail = xhci_detail_hold(
                        best_scratch, sizeof(best_scratch),
                        xhci_detail_port(port, "ATTACH_BUDGET", "EXHAUSTED"));
                }
                break;
            }
            attempt.tried = true;
            attempt.device = device;
            if (keyboard) {
                keyboard_attempted = true;
            }
            if (xhci_attach_device(device, port, speed)) {
                attempt.state = "BOUND";
                attempt.reason = "OK";
                /*
                 * ITEM-452: the device may have been refused by the other role
                 * first (a mouse is offered to the keyboard role before it is
                 * offered to its own), and that refusal's detail is still in
                 * the scratch buffer.  A bound device publishes its binding,
                 * not a stage it passed through.
                 */
                attempt.detail[0] = '\0';
                attempt.has_detail = false;
                if (!keyboard) {
                    /*
                     * ITEM-448/452: a bound mouse is kept - no proof window,
                     * no release - and its session stays live next to the
                     * keyboard's, on whichever controller it was found.
                     */
                    ++xhci_machine.mouse_candidates_bound;
                    xhci_machine.mouse_session = xhci;
                    xhci_machine.mouse_seen = true;
                    xhci_machine.mouse_device_seen = true;
                    xhci_machine.mouse_seen_port = device->port;
                    xhci_machine.mouse_seen_speed = device->speed;
                    xhci_machine.mouse_vendor_id = device->vendor_id;
                    xhci_machine.mouse_product_id = device->product_id;
                    xhci_location_text(xhci_machine.mouse_seen_controller,
                                       sizeof(xhci_machine.mouse_seen_controller),
                                       &xhci->location);
                    role_bound_here = true;
                    break;
                }
                ++xhci_machine.candidates_bound;
                if (xhci_candidate_prove(device)) {
                    /*
                     * ITEM-438: the device answered inside its window, so it is
                     * the machine's keyboard - but the walk still visits every
                     * remaining port and controller, for the pointer and for
                     * the record of what else is attached.
                     */
                    ++xhci_machine.candidates_proven;
                    keyboard_proven = true;
                    xhci_machine.keyboard_session = xhci;
                    role_bound_here = true;
                    break;
                }
                /*
                 * ITEM-438: bound, silent, and now published as such.  The walk
                 * keeps the first such candidate in case nothing better
                 * answers, releases this slot, and goes on with the ports the
                 * 2026-09-17 run never reached.  ITEM-448 then offers the same
                 * port to the mouse role: a wireless receiver that parses as a
                 * keyboard and never types is exactly the device whose other
                 * half is the pointer.
                 */
                ++xhci_machine.candidates_silent;
                xhci_log_candidate_rejection(device, port, speed);
                if (xhci_scan_prefers(XHCI_STATUS_KEYBOARD_LOST, best_status)) {
                    best_status = XHCI_STATUS_KEYBOARD_LOST;
                    best_detail = xhci_detail_hold(
                        best_scratch, sizeof(best_scratch),
                        xhci_detail_port(port, "NO_REPORT", "SILENT"));
                }
                if (!xhci_machine.silent_candidate.valid) {
                    xhci_machine.silent_candidate.header = xhci->header;
                    xhci_machine.silent_candidate.port = (uint8_t)port;
                    xhci_machine.silent_candidate.speed = speed;
                    xhci_machine.silent_candidate.valid = true;
                }
                xhci_release_candidate(device);
                attempt.state = "REFUSED";
                attempt.reason = "NO_REPORT";
                xhci_detail_copy(attempt.detail, sizeof(attempt.detail),
                                 xhci_detail_port(port, "NO_REPORT", "SILENT"));
                attempt.has_detail = true;
                continue;
            }
            /*
             * The refusal detail is copied out before the next role probes:
             * both xhci->detail and the machine's mouse detail are working
             * buffers, and the device record has to name the stage that
             * actually refused this device.
             */
            if (keyboard) {
                attempt.reason = xhci_status_string(xhci->status);
                xhci_detail_copy(attempt.detail, sizeof(attempt.detail),
                                 xhci->detail);
                if (xhci_scan_prefers(xhci->status, best_status)) {
                    best_status = xhci->status;
                    best_detail = xhci_detail_hold(best_scratch,
                                                   sizeof(best_scratch),
                                                   xhci->detail);
                }
            } else if (device->attempt_made) {
                /* ITEM-578: the pointer's own attempt record names the stage
                   and reason, so the device line cannot read as the keyboard's
                   refusal with the roles swapped. */
                attempt.reason = xhci_status_string(device->attempt_status);
                xhci_detail_copy(attempt.detail, sizeof(attempt.detail),
                                 device->attempt_detail);
            } else {
                attempt.reason =
                    usb_hid_status_string(xhci_machine.mouse_status);
                xhci_detail_copy(attempt.detail, sizeof(attempt.detail),
                                 xhci_machine.mouse_detail);
            }
            attempt.has_detail = attempt.detail[0] != '\0';
        }
        /*
         * ITEM-519 increment 2: a port no HID role bound is offered to USB
         * mass storage.  The budget is storage's own: the HID walk's budget
         * is what keeps a controller full of other devices from starving
         * the keyboard behind it, and it must not be spent on devices that
         * are not storage.
         */
        if (xhci_machine.storage_session == NULL &&
            storage_attempts < XHCI_STORAGE_MAX_ATTACH_ATTEMPTS) {
            bool hid_bound_here = false;

            for (uint8_t role = 0U; role < XHCI_HID_DEVICES; ++role) {
                const struct xhci_hid_device *bound = xhci_device(role);

                if (bound->ready && bound->port == port) {
                    hid_bound_here = true;
                    break;
                }
            }
            if (!hid_bound_here) {
                ++storage_attempts;
                if (xhci_storage_attach(&xhci->storage, port, speed)) {
                    attempt.role = "STORAGE";
                    attempt.state = "BOUND";
                    attempt.reason = "OK";
                    attempt.detail[0] = '\0';
                    attempt.has_detail = false;
                    attempt.tried = true;
                    attempt.device = NULL;
                    xhci_machine.storage_session = xhci;
                    ++xhci_machine.storage_candidates_bound;
                }
            }
        }
        xhci_log_device_record(port, speed, attempt.role, attempt.state,
                               attempt.reason,
                               attempt.has_detail ? attempt.detail : NULL,
                               attempt.tried ? attempt.device : NULL);
    }
    /*
     * ITEM-452: the record's state/reason is the keyboard role's verdict on
     * this controller.
     *
     *   * the keyboard is bound here: READY, and the status its own attach set
     *     stands even if a later device was refused for the other role;
     *   * nothing was connected: NO_DEVICE with the controller's own evidence;
     *   * the keyboard role was offered and refused: the most informative
     *     refusal, in this function's own storage (the scratch buffer dies
     *     with it, and xhci->detail is read long after);
     *   * the keyboard role was never offered because the machine already has
     *     a keyboard: ROLE_TAKEN, so the record does not invent a refusal the
     *     controller never made.  Its devices are named by their own
     *     `[USB ]` lines with the same reason.
     */
    if (role_bound_here && xhci_machine.keyboard_session == xhci) {
        xhci_set_status(XHCI_STATUS_OK, "OK");
    } else if (!saw_device) {
        xhci_set_status(XHCI_STATUS_NO_DEVICE, "NO_CONNECTED_DEVICE");
    } else if (keyboard_attempted) {
        xhci_set_status(best_status, xhci_detail_keep(best_detail));
    } else if (role_bound_here) {
        xhci_set_status(XHCI_STATUS_ROLE_TAKEN, "KEYBOARD_ALREADY_BOUND");
    } else {
        xhci_set_status(XHCI_STATUS_ROLE_TAKEN, "ROLES_ALREADY_BOUND");
    }
    xhci_probe_note_evidence(evidence, evidence_ports, powered_off);
    xhci_scan_note_controller(scan, xhci->status, xhci->detail,
                              xhci_machine.keyboard_session == xhci);
}

/*
 * ITEM-405: one fragment, two surfaces.
 *
 * The serial calls below are exactly what they were; the console mirror is fed
 * the same fragment so the line an operator reads on screen is the line the
 * serial record carries.  Appending only buffers - the framebuffer is painted
 * once, by console_record_end() after the serial sequence has ended.
 */
#define XHCI_PUBLISH(text) do { \
        serial_write(text); \
        console_record_text(text); \
    } while (0)
#define XHCI_PUBLISH_U64(value) do { \
        serial_write_u64(value); \
        console_record_u64(value); \
    } while (0)
#define XHCI_PUBLISH_HEX(value) do { \
        serial_write_hex(value); \
        console_record_hex64(value); \
    } while (0)
/* A hexadecimal field the record composes itself, so the serial stream and the
   console mirror still receive the same bytes. */
#define XHCI_PUBLISH_TEXT(value) do { \
        serial_write(value); \
        console_record_text(value); \
    } while (0)

/*
 * ITEM-407: the compact hexadecimal form the evidence region uses: eight digits
 * for a value that fits a 32-bit register, its own digits for a wider one, no
 * "0x" prefix. The register fields the evidence names (PORTSC, USBCMD, USBSTS,
 * the powered-port mask) are 32 bits wide, so eight digits is exact rather than
 * a truncation, and a value that does not fit them is written whole.
 *
 * Returns the number of characters written, or -1 when `size` cannot hold the
 * value at that width - which is what the static assertion below turns into a
 * build failure if a caller reserves too little for the budget it claims.
 */
#define XHCI_EVIDENCE_HEX_DIGITS 8U
#define XHCI_EVIDENCE_HEX_MAX 16U

static char xhci_hex_digit(uint32_t nibble) {
    return (char)(nibble < 10U ? ('0' + nibble) : ('A' + (nibble - 10U)));
}

static int xhci_format_hex32(char *buffer, size_t size, uint32_t value) {
    size_t digits = XHCI_EVIDENCE_HEX_DIGITS;
    size_t index = 0U;

    while (digits > 1U && (value >> ((digits - 1U) * 4U)) == 0U) {
        --digits;
    }
    if (digits + 1U > size) {
        return -1;
    }
    for (index = 0U; index < digits; ++index) {
        const uint32_t shift = (uint32_t)(digits - 1U - index) * 4U;

        buffer[index] = xhci_hex_digit((value >> shift) & 0xFU);
    }
    buffer[index] = '\0';
    return (int)digits;
}

/*
 * ITEM-407: the record has to stay inside the console mirror's one-painted-line
 * budget (CONSOLE_RECORD_MAX, 320 bytes). A longer one is cut and closed with
 * "...", which would hide the port evidence this item exists to publish - and
 * the port evidence is the longest part of the line.
 *
 * The budget therefore decides how the evidence is written and how much of it
 * there is:
 *
 *   * a hexadecimal *register* field inside the evidence region is written
 *     compactly - eight digits for a 32-bit register, the full value for
 *     anything wider, no "0x" prefix - because PORTSC and USBCMD/USBSTS are
 *     exactly 32 bits wide and the shared serial helpers cost eighteen bytes
 *     per value. The record's leading fields (state/reason/detail/bar/...)
 *     keep the established `key=0x…` shape and the shared renderers;
 *   * the evidence is published for a controller that did **not** find a
 *     keyboard, which is the case that has to be explained and whose record
 *     carries no `hid=` block; a READY record publishes the keyboard's own
 *     facts instead (the two together do not fit);
 *   * at most XHCI_PORT_EVIDENCE_MAX ports are named. Two ports answer the
 *     question this exists for - "no connect on every port", "power was never
 *     on", "the register does not answer" - and `ports=` at the end of the
 *     record still names the controller's real port count;
 *   * ITEM-438: when a device *was* reached and then refused, the record
 *     publishes that device's vid/pid and the interface list its configuration
 *     declares instead of the port evidence. ITEM-452 moves that identity to
 *     the device's own `[USB ]` line, so the controller record has room for
 *     what only it can say: its PCI address, how many controllers the machine
 *     has and whether this one was taken;
 *   * ITEM-452: the controller record always names the controller (`pci=`),
 *     the size of the machine (`controllers=`) and whether this visit kept a
 *     session (`taken=`), because "the driver visited only the first
 *     controller" is exactly the failure this item exists to make readable;
 *   * ITEM-452: the port evidence and the `mouse=NONE` finding are exclusive.
 *     The evidence is published when no role is bound on the controller and the
 *     machine-wide [MOUSE] record still carries the pointer's verdict; the two
 *     blocks together do not fit one painted line, and the evidence is the one
 *     that has to survive there;
 *   * ITEM-452: the 16-bit identifiers (`vendor=`, `device=`, and `vid=`/`pid=`
 *     on the device record) are written as "0x" plus four digits, exactly as
 *     the owner's hardware log reads.  The shared 64-bit writer pads them to
 *     eighteen bytes each, which is twelve bytes per identifier more than this
 *     arithmetic reserves.
 *
 * The arithmetic below is the worst case each shape can produce.  The detail
 * of the port-evidence shape cannot reach the 79-byte buffer bound: that block
 * is published only when no role is bound on the controller, and the longest
 * detail a device-stage can compose there is
 * "P255_DEVICE_DESCRIPTOR_PREFIX_MAX_EXIT_LATENCY_TOO_LARGE" (56, the longest
 * completion-code name of xHCI 1.1 table 6-90 next to the longest stage).
 *
 *   no keyboard: 114 (head + 56-byte detail) + 35 (pci/controllers/taken)
 *                + 89 (controller facts) + 76 (evidence)                 = 314
 *   keyboard:     38 (READY + OK + OK) + 35 + 89 + 16 (hid=)
 *                + 39 (mouse=NONE reason=BAD_DESCRIPTOR_LENGTH)           = 217
 *   refused:     133 (head + 79-byte detail) + 35 + 89                   = 257
 *
 * `make test-xhci-host` builds each of those strings and fails if one outgrows
 * the pinned worst case, so a field added here has to make room for itself
 * instead of being cut off the screen.
 */
#define XHCI_RECORD_WORST_CASE_BYTES 314U
#define XHCI_RECORD_BUDGET_BYTES 320U
#define XHCI_RECORD_EVIDENCE_BYTES 76U

_Static_assert(XHCI_RECORD_WORST_CASE_BYTES <= XHCI_RECORD_BUDGET_BYTES,
               "the [XHCI] record must fit the console's one-line budget");
_Static_assert(
    /* " usbcmd=" + digits + " usbsts=" + digits + " portsc=" + list + " pp=" +
       digits + " ppoff=" + digits.  Every PORTSC value is a 32-bit register
       read, so the formatter's uncompacted worst case is "0x" plus eight
       digits, and the power mask covers at most XHCI_PORT_EVIDENCE_MAX ports,
       i.e. one hexadecimal digit. */
    8U + XHCI_EVIDENCE_HEX_DIGITS + 8U + XHCI_EVIDENCE_HEX_DIGITS + 8U +
            (XHCI_PORT_EVIDENCE_MAX * (2U + XHCI_EVIDENCE_HEX_DIGITS) +
             (XHCI_PORT_EVIDENCE_MAX - 1U)) +
            4U + 1U + 7U + 3U <=
        XHCI_RECORD_EVIDENCE_BYTES,
    "the port evidence must fit the budget reserved for it");
_Static_assert(XHCI_EVIDENCE_HEX_MAX >= XHCI_EVIDENCE_HEX_DIGITS,
               "a hexadecimal field must have room for its digits");

/*
 * ITEM-407: one compact hexadecimal field, on both surfaces. The caller has
 * written the "key=" fragment and this writes the value, so the byte sequence
 * the serial stream carries is the byte sequence the console mirror paints.
 */
static void xhci_publish_hex32(uint32_t value) {
    char digits[XHCI_EVIDENCE_HEX_MAX + 1U];

    if (xhci_format_hex32(digits, sizeof(digits), value) < 0) {
        XHCI_PUBLISH("?");
        return;
    }
    XHCI_PUBLISH_TEXT(digits);
}

/*
 * ITEM-452: a 16-bit identifier in the record's established shape - "0x" and
 * four digits, zero padded, exactly as the owner's hardware log reads
 * (`vid=0x046D pid=0xC31C`).  The shared hexadecimal writer pads every value
 * into a 64-bit field, so it costs eighteen bytes per identifier; four of them
 * on the controller record (vendor/device) and two on every device record
 * (vid/pid) are twelve bytes each more than the budget the host proof pins -
 * and on the port-evidence shape that difference is what pushed the record
 * past the console's one painted line.
 */
static void xhci_publish_hex16(uint16_t value) {
    static const char digits[] = "0123456789ABCDEF";
    char text[7];

    text[0] = '0';
    text[1] = 'x';
    text[2] = digits[(value >> 12) & 0xFU];
    text[3] = digits[(value >> 8) & 0xFU];
    text[4] = digits[(value >> 4) & 0xFU];
    text[5] = digits[value & 0xFU];
    text[6] = '\0';
    XHCI_PUBLISH_TEXT(text);
}

/*
 * ITEM-452: the controller names itself before anything else on the line, and
 * the two numbers a reader needs to place it: how many controllers the machine
 * has, and whether this one was taken.  `taken=NO` on a controller the machine
 * then did not use is the answer to "was it even looked at"; the devices it
 * carries are named by their own [USB ] lines.
 */
static void xhci_publish_location(void) {
    static const char digits[] = "0123456789ABCDEF";
    char pair[3];

    pair[0] = digits[(xhci->location.bus >> 4) & 0xFU];
    pair[1] = digits[xhci->location.bus & 0xFU];
    pair[2] = '\0';
    XHCI_PUBLISH_TEXT(pair);
    XHCI_PUBLISH(":");
    pair[0] = digits[(xhci->location.device >> 4) & 0xFU];
    pair[1] = digits[xhci->location.device & 0xFU];
    XHCI_PUBLISH_TEXT(pair);
    XHCI_PUBLISH(".");
    pair[0] = digits[xhci->location.function & 0xFU];
    pair[1] = '\0';
    XHCI_PUBLISH_TEXT(pair);
}

static void xhci_log_record(void) {
    const uint64_t flags = serial_sequence_begin();

    /*
     * ITEM-406: one record per controller the scan attempts, in PCI order, and
     * the record for the controller a keyboard was found on is the READY one.
     * The shape is the ITEM-395 one: state/reason/detail, the controller facts
     * when its register window is mapped, and the device facts when a keyboard
     * is attached - only the detail now names the controller, the port and the
     * stage that decided the outcome. It is assembled from several
     * serial_write() calls, so it stays inside one begin/end pair: an IRQ line
     * between two calls would split the marker the QEMU proofs parse.
     *
     * ITEM-405: the acceptance host has no serial capture and the USB HID path
     * did not come up, so the reason - which lives in this record - was
     * invisible on screen.  Every fragment below therefore goes to the local
     * console through the record mirror as well; the serial stream is
     * unchanged.
     *
     * ITEM-407: a controller that is up and did not find a keyboard also
     * publishes its port evidence - usbcmd/usbsts as the scan left them, the
     * PORTSC values of the first XHCI_PORT_EVIDENCE_MAX ports, which of those
     * was powered, and how many were powered off when the scan claimed them.
     * One more photograph of the screen then answers the question the second
     * hardware run opened: did every port really report "no device connected",
     * or was the driver reading a controller that was not running (HCH=1), a
     * port whose power was never on (pp=0), or a register that is not PORTSC at
     * all (portsc all zero while bar/ports/slots are sane)?
     *
     * ITEM-452: every visit publishes this record now, whether it bound a role
     * or not, and the record always names the controller (`pci=bb:dd.f`), the
     * machine (`controllers=`) and the outcome (`taken=`).  The device identity
     * that used to share this line has its own `[USB ]` line, so the port
     * evidence - "did this controller see anything at all" - is published
     * whenever no role is bound here.
     */
    const struct xhci_hid_device *keyboard =
        xhci_device(XHCI_HID_ROLE_KEYBOARD);
    const struct xhci_hid_device *mouse = xhci_device(XHCI_HID_ROLE_MOUSE);
    const bool taken =
        keyboard->ready || mouse->ready || xhci->storage.bound;

    console_journal_begin();
    XHCI_PUBLISH("[XHCI] state=");
    XHCI_PUBLISH(xhci->state);
    XHCI_PUBLISH(" reason=");
    XHCI_PUBLISH(xhci_status_string(xhci->status));
    XHCI_PUBLISH(" detail=");
    XHCI_PUBLISH(xhci->detail != NULL ? xhci->detail : "UNKNOWN");
    XHCI_PUBLISH(" pci=");
    xhci_publish_location();
    XHCI_PUBLISH(" controllers=");
    XHCI_PUBLISH_U64(xhci_machine.controllers_total);
    XHCI_PUBLISH(" taken=");
    XHCI_PUBLISH(taken ? "YES" : "NO");
    if (xhci->regs != NULL) {
        XHCI_PUBLISH(" bar=");
        XHCI_PUBLISH_HEX(xhci->bar_physical);
        XHCI_PUBLISH(" ports=");
        XHCI_PUBLISH_U64(xhci->max_ports);
        XHCI_PUBLISH(" slots=");
        XHCI_PUBLISH_U64(xhci->max_slots);
        XHCI_PUBLISH(" slot_en=");
        XHCI_PUBLISH_U64(xhci->slots_enabled);
        XHCI_PUBLISH(" ctx=");
        XHCI_PUBLISH_U64(xhci->context_bytes);
        XHCI_PUBLISH(" vendor=");
        xhci_publish_hex16(xhci->pci_vendor);
        XHCI_PUBLISH(" device=");
        xhci_publish_hex16(xhci->pci_device);
    }
    if (xhci->regs != NULL && !taken) {
        char evidence[XHCI_PORT_EVIDENCE_BYTES];

        (void)xhci_format_port_evidence(evidence, sizeof(evidence),
                                        xhci->portsc, xhci->portsc_ports);
        XHCI_PUBLISH(" usbcmd=");
        xhci_publish_hex32(xhci->usbcmd);
        XHCI_PUBLISH(" usbsts=");
        xhci_publish_hex32(xhci->usbsts);
        XHCI_PUBLISH(" portsc=");
        XHCI_PUBLISH(evidence);
        XHCI_PUBLISH(" pp=");
        xhci_publish_hex32(xhci->portsc_powered_mask);
        XHCI_PUBLISH(" ppoff=");
        XHCI_PUBLISH_U64(xhci->portsc_powered_off);
    }
    if (keyboard->ready) {
        /* ITEM-438: a keyboard confirmed by its report descriptor is named as
           such; its port, identity and binding are published by its own
           [USB ] line, and the layout's numbers by the [KBD ] record. */
        XHCI_PUBLISH(keyboard->report_mode ? " hid=REPORT_KEYBOARD"
                                           : " hid=BOOT_KEYBOARD");
    }
    /*
     * ITEM-448: the mouse half of the same controller.  A record that carries
     * both roles names both; a controller that has the keyboard but no pointer
     * says the mouse is missing rather than silent.
     */
    if (mouse->ready) {
        XHCI_PUBLISH(mouse->report_mode ? " hid=REPORT_MOUSE"
                                        : " hid=BOOT_MOUSE");
    } else if (keyboard->ready) {
        /*
         * ITEM-448: the pointer half of the same controller.  The record names
         * the keyboard, so a controller that has it and no pointer says the
         * mouse is missing rather than silent.
         *
         * ITEM-452: this block is exclusive with the port evidence above - the
         * two together do not fit the console's one painted line, and on a
         * controller that bound no role the evidence ("did this controller see
         * anything at all") is the one that has to survive.  The machine-wide
         * [MOUSE] record still carries the pointer's own verdict.
         */
        XHCI_PUBLISH(" mouse=NONE reason=");
        XHCI_PUBLISH(usb_hid_status_string(xhci_machine.mouse_status));
    }
    XHCI_PUBLISH("\n");
    serial_sequence_end(flags);
    console_journal_end();
}

/*
 * ITEM-453: every port of every controller names itself.
 *
 * ITEM-407 published two PORTSC values in the controller's own record because
 * that record has to fit one painted console line; the 2026-09-19 run then
 * could not answer the question it was read for - "was the other four of this
 * six-port controller ever looked at, and what did they say" - and the receiver
 * the owner has plugged into `05:00.3` was reported as no device at all.
 *
 * Each port therefore gets its own `[XHCI-PORT]` line, with the raw PORTSC
 * value it answered before the bring-up (`was=`), the value after it (`now=`),
 * the decoded power/connect/link state, whether the driver asserted power
 * (`pon=`) or moved the link (`kick=`), and how long it waited for a connect
 * (`wait=`).  A port that is empty, a port that was never powered, a port whose
 * link state cannot report a connect at all, and a register that answers
 * nothing now read differently on the next hardware boot.
 *
 * The line is composed by the pure formatter the host proof pins
 * (xhci_format_port_record) and published whole: a formatter failure publishes
 * nothing rather than half a record, because the console mirror and the
 * durable journal would both carry the fragment.
 */
static void xhci_log_port_record(
    uint32_t port,
    const struct xhci_port_bring_up *plan,
    uint32_t now,
    uint32_t waited_ms) {
    const struct xhci_protocol_group *group =
        xhci_protocol_for_port(xhci->protocols, xhci->protocol_count, port);
    const char *label =
        group != NULL ? xhci_protocol_group_label(group->major) : NULL;
    char line[XHCI_PORT_RECORD_BYTES];
    uint64_t flags;

    if (xhci_format_port_record(line, sizeof(line), xhci->location.bus,
                                xhci->location.device, xhci->location.function,
                                port, label != NULL ? label : "NONE", plan, now,
                                waited_ms) == 0U) {
        return;
    }
    flags = serial_sequence_begin();
    console_journal_begin();
    XHCI_PUBLISH_TEXT(line);
    XHCI_PUBLISH("\n");
    serial_sequence_end(flags);
    console_journal_end();
}

/*
 * ITEM-453: the controller's Supported Protocol groups name themselves.
 *
 * One line per group the controller declares, with the parsed fields (major,
 * minor, four-character name, raw port offset and count, the derived raw port
 * range and the PSIVs) and the three raw capability dwords they were parsed
 * from.  The raw dwords are the point: if this driver reads the layout
 * differently than the controller means it, the next hardware run shows the
 * disagreement instead of a wrong label being trusted - and if the controller
 * declares no group at all (QEMU's model does not), the record is absent and
 * its ports are published with `group=NONE`.
 */
static void xhci_log_protocol_records(void) {
    for (uint32_t index = 0U; index < xhci->protocol_count; ++index) {
        const struct xhci_protocol_group *group = &xhci->protocols[index];
        char line[XHCI_PROTOCOL_RECORD_BYTES];
        uint64_t flags;

        if (xhci_format_protocol_record(
                line, sizeof(line), xhci->location.bus, xhci->location.device,
                xhci->location.function, index, xhci->protocol_count, group,
                group->raw_revision, group->raw_name, group->raw_port_info) ==
            0U) {
            continue;
        }
        flags = serial_sequence_begin();
        console_journal_begin();
        XHCI_PUBLISH_TEXT(line);
        XHCI_PUBLISH("\n");
        serial_sequence_end(flags);
        console_journal_end();
    }
}

/*
 * ITEM-452: every device the walk reaches names itself.
 *
 * "Plugged in but seen=0" is not an acceptable record, so this line is
 * published for every root port whose PORTSC reported a connected device - the
 * port, the speed, the role it was bound as or the role that refused it and
 * why.  A device whose descriptors were read carries its vid/pid and the
 * interface list its configuration declared; a device that could not be read
 * that far carries the stage that stopped it; a device the machine did not
 * need is REFUSED with reason=ROLE_TAKEN rather than silently skipped.
 *
 * The identity block is bounded by USB_HID_INTERFACE_SUMMARY_BYTES, and the
 * worst case the host proof pins is " vid=0xFFFF pid=0xFFFF ifaces=" plus the
 * four-entry list: 11 + 11 + 8 + 48.
 */
static void xhci_log_device_record(
    uint32_t port,
    uint8_t speed,
    const char *role,
    const char *state,
    const char *reason,
    const char *detail,
    const struct xhci_hid_device *identity) {
    const uint64_t flags = serial_sequence_begin();

    console_journal_begin();
    XHCI_PUBLISH("[USB ] pci=");
    xhci_publish_location();
    XHCI_PUBLISH(" port=");
    XHCI_PUBLISH_U64(port);
    XHCI_PUBLISH(" speed=");
    XHCI_PUBLISH(xhci_speed_string(speed));
    XHCI_PUBLISH(" role=");
    XHCI_PUBLISH(role != NULL ? role : "NONE");
    if (state != NULL) {
        XHCI_PUBLISH(" state=");
        XHCI_PUBLISH(state);
    }
    if (reason != NULL) {
        XHCI_PUBLISH(" reason=");
        XHCI_PUBLISH(reason);
    }
    if (detail != NULL && detail[0] != '\0') {
        XHCI_PUBLISH(" detail=");
        XHCI_PUBLISH(detail);
    }
    if (identity != NULL && identity->device_seen) {
        /* The shared hexadecimal writer emits the "0x" prefix itself, so the
           fragment names the key only - exactly as the controller facts do. */
        XHCI_PUBLISH(" vid=");
        xhci_publish_hex16(identity->vendor_id);
        XHCI_PUBLISH(" pid=");
        xhci_publish_hex16(identity->product_id);
        XHCI_PUBLISH(" ifaces=");
        XHCI_PUBLISH(identity->interface_summary[0] != '\0'
                         ? identity->interface_summary
                         : "NONE");
    }
    XHCI_PUBLISH("\n");
    serial_sequence_end(flags);
    console_journal_end();
}



/*
 * ITEM-519 increment 4: the block view of the bound stick.  `BLOCK_BOUND`
 * carries the geometry the block layer will enforce (the same capacity the
 * device declared, byte-addressed through the adapter); `BLOCK_REFUSED` names
 * the adapter's own status, so a stick whose transport bound but whose block
 * view did not is never silently described as usable by the journal.
 */
static void xhci_log_storage_block_record(
    const struct xhci_storage_device *storage) {
    const uint64_t flags = serial_sequence_begin();

    console_journal_begin();
    XHCI_PUBLISH("[USB-STORAGE] pci=");
    xhci_publish_location();
    XHCI_PUBLISH(" port=");
    XHCI_PUBLISH_U64(storage->port);
    XHCI_PUBLISH(" vid=");
    xhci_publish_hex16(storage->vendor_id);
    XHCI_PUBLISH(" pid=");
    xhci_publish_hex16(storage->product_id);
    if (storage->block_bound) {
        XHCI_PUBLISH(" state=BLOCK_BOUND reason=OK block=");
        XHCI_PUBLISH_U64(storage->block.device.block_size);
        XHCI_PUBLISH(" blocks=");
        XHCI_PUBLISH_U64(storage->block.device.block_count);
    } else {
        XHCI_PUBLISH(" state=BLOCK_REFUSED reason=");
        XHCI_PUBLISH(block_device_status_string(storage->block_status));
    }
    XHCI_PUBLISH("\n");
    serial_sequence_end(flags);
    console_journal_end();
}

/*
 * ITEM-519 increment 5: what the boot-medium guard saw.  The record carries the
 * loader's raw fields and the device's own disk id in every case, so a machine
 * whose loader reports something unexpected says what it reported instead of
 * leaving the question to a debugger.  `state` is the verdict and `reason` is
 * the first fact that decided it.
 */
static void xhci_log_medium_record(
    const struct xhci_storage_device *storage,
    const struct install_medium_boot_identity *boot,
    const struct install_medium_verdict *verdict,
    const char *state,
    const char *reason) {
    const uint64_t flags = serial_sequence_begin();

    console_journal_begin();
    XHCI_PUBLISH("[INSTALL-MEDIUM] pci=");
    xhci_publish_location();
    XHCI_PUBLISH(" port=");
    XHCI_PUBLISH_U64(storage->port);
    XHCI_PUBLISH(" vid=");
    xhci_publish_hex16(storage->vendor_id);
    XHCI_PUBLISH(" pid=");
    xhci_publish_hex16(storage->product_id);
    XHCI_PUBLISH(" state=");
    XHCI_PUBLISH(state);
    XHCI_PUBLISH(" reason=");
    XHCI_PUBLISH(reason != NULL ? reason : "UNKNOWN");
    XHCI_PUBLISH(" media=");
    XHCI_PUBLISH_U64(boot != NULL ? boot->media_type : 0U);
    XHCI_PUBLISH(" partition=");
    XHCI_PUBLISH_U64(boot != NULL ? boot->partition_index : 0U);
    XHCI_PUBLISH(" loader_id=");
    xhci_publish_hex32(boot != NULL ? boot->mbr_disk_id : 0U);
    XHCI_PUBLISH(" disk_id=");
    xhci_publish_hex32(verdict != NULL ? verdict->disk_id : 0U);
    if (verdict != NULL) {
        XHCI_PUBLISH(" scope=");
        if (verdict->result != INSTALL_MEDIUM_MATCH) {
            XHCI_PUBLISH("NONE");
        } else {
            XHCI_PUBLISH(verdict->partition_index == 0U ? "WHOLE"
                                                        : "PARTITION");
        }
        XHCI_PUBLISH(" type=");
        xhci_publish_hex32(verdict->partition_type);
        XHCI_PUBLISH(" first_lba=");
        XHCI_PUBLISH_U64(verdict->first_lba);
        XHCI_PUBLISH(" length_lba=");
        XHCI_PUBLISH_U64(verdict->length_lba);
    }
    XHCI_PUBLISH("\n");
    serial_sequence_end(flags);
    console_journal_end();
}

/*
 * ITEM-519 increment 5: read LBA 0 of one bound device through its own block
 * view and answer whether it is the medium this machine booted from.  The probe
 * is read-only; a device with no block view, no loader identity, or a read that
 * fails is UNAVAILABLE with the step named, never a silent MATCH.  The device
 * is passed in rather than looked up, because the walk publishes the verdict
 * before the session is recorded in the machine.
 */
/*
 * ITEM-582: the diagnostic-medium attempt on the xHCI path, named whether it
 * bound or not, so an installed machine's export records which stick it saw
 * and why it did not take it.
 */
static void xhci_log_diagnostic_record(const struct xhci_storage_device *storage,
                                       bool bound) {
    const uint64_t flags = serial_sequence_begin();
    bool attempted = false;
    const char *result = NULL;
    const char *reason = NULL;
    const char *placement = NULL;

    install_journal_platform_diagnostic_state(&attempted, &result, &reason,
                                              &placement);
    console_journal_begin();
    XHCI_PUBLISH("[DIAG-MEDIUM] controller=xhci pci=");
    xhci_publish_location();
    XHCI_PUBLISH(" port=");
    XHCI_PUBLISH_U64(storage->port);
    XHCI_PUBLISH(" vid=");
    xhci_publish_hex16(storage->vendor_id);
    XHCI_PUBLISH(" pid=");
    xhci_publish_hex16(storage->product_id);
    XHCI_PUBLISH(" state=");
    XHCI_PUBLISH(bound ? "BOUND" : "REFUSED");
    XHCI_PUBLISH(" result=");
    XHCI_PUBLISH(result != NULL ? result : "UNAVAILABLE");
    XHCI_PUBLISH(" reason=");
    XHCI_PUBLISH(reason != NULL ? reason : "UNKNOWN");
    if (bound && placement != NULL) {
        XHCI_PUBLISH(" placement=");
        XHCI_PUBLISH(placement);
    }
    XHCI_PUBLISH("\n");
    serial_sequence_end(flags);
    console_journal_end();
}

static bool xhci_storage_medium_probe(struct xhci_storage_device *storage) {
    struct install_medium_boot_identity boot;
    struct install_medium_verdict verdict;
    static uint8_t sector[INSTALL_MEDIUM_SECTOR_BYTES];

    if (storage == NULL) {
        return false;
    }
    if (!storage->block_bound) {
        storage->medium_probed = true;
        storage->medium_result = INSTALL_MEDIUM_NO_IDENTITY;
        storage->medium_reason = "NO_BLOCK_DEVICE";
        xhci_log_medium_record(storage, NULL, NULL, "UNAVAILABLE",
                               "NO_BLOCK_DEVICE");
        return false;
    }
    if (!install_medium_boot_identity(&boot)) {
        storage->medium_probed = true;
        storage->medium_result = INSTALL_MEDIUM_NO_IDENTITY;
        storage->medium_reason = "NO_LOADER_IDENTITY";
        xhci_log_medium_record(storage, NULL, NULL, "UNAVAILABLE",
                               "NO_LOADER_IDENTITY");
        return false;
    }
    if (block_device_read(&storage->block.device, 0U, sector,
                          sizeof(sector)) != BLOCK_DEVICE_OK) {
        storage->medium_probed = true;
        storage->medium_result = INSTALL_MEDIUM_NO_IDENTITY;
        storage->medium_reason = "READ";
        xhci_log_medium_record(storage, &boot, NULL, "UNAVAILABLE", "READ");
        return false;
    }
    verdict = install_medium_identify(&boot, sector,
                                      storage->block.device.block_count);
    storage->medium_probed = true;
    storage->medium_result = verdict.result;
    storage->medium_reason = verdict.reason;
    xhci_log_medium_record(storage, &boot, &verdict,
                           install_medium_result_string(verdict.result),
                           verdict.reason);
    if (verdict.result == INSTALL_MEDIUM_MATCH) {
        /*
         * ITEM-521: the medium the machine booted from is the only one the
         * install journal may be placed on, and the identity guard has just
         * said so.  What "placed" means is the answer to the ITEM-520 fork -
         * the FAT12 file the medium's build reserves - and lives behind the
         * platform seam; a refusal there names itself and never fails the
         * install.  The seam is handed
         * the block view this probe proved and the verdict itself, so the
         * answer never repeats the check.
         */
        if (!install_journal_platform_bind_medium(&storage->block.device,
                                                  &verdict)) {
            /*
             * ITEM-521 (increment 8): the proven medium carries no journal.
             * The seam may refuse for its own reason (no partition carries
             * FAT12, the volume's audit refuses it, the table has no
             * signature).  Requirement 3 asks for a record in that case
             * instead of a silent loss, so the loss is named once through the
             * core's own refusal - the same call an optical boot makes, which
             * never fails the run and never overwrites a reason a backend
             * named first.
             */
            (void)install_journal_platform_medium_unbound();
        }
    } else {
        /* ITEM-582: the installed machine boots from its own disk, so the boot
           guard can never match the diagnostic stick.  The stick is bound only
           by its prepared identity and reserved journal, and the attempt is
           recorded either way. */
        const bool bound = install_journal_platform_bind_diagnostic(
            &storage->block.device, &verdict);

        xhci_log_diagnostic_record(storage, bound);
    }
    return verdict.result == INSTALL_MEDIUM_MATCH;
}

static bool xhci_session_holds_role(const struct xhci_session *session);

/*
 * ITEM-582: look for a diagnostic storage device plugged in after boot.
 *
 * The xHCI walk ran once, at boot; a stick inserted later was never
 * enumerated.  This scans the live sessions again, but only ports that read
 * empty at boot and carry nothing now - a port with a bound HID role or a
 * storage device is never reset, so the working keyboard, pointer and storage
 * keep their slots.  The port that answers is brought up and offered to the
 * storage stack exactly as the boot walk offers one, then its medium is
 * probed (which is also where a prepared diagnostic stick is bound).
 */
static bool xhci_rescan_storage(void) {
    struct xhci_session *previous = xhci;
    struct xhci_session *bound_storage = xhci_machine.storage_session;
    const uint32_t bound_port =
        bound_storage != NULL ? bound_storage->storage.port : 0U;
    uint32_t attempts = 0U;
    bool bound = false;
    bool visited = false;

    for (uint32_t slot = 0U; slot < XHCI_SESSION_SLOTS && !bound; ++slot) {
        struct xhci_session *session = &xhci_sessions[slot];

        if (!session->in_use || session->fatal_valid) {
            continue;
        }
        visited = true;
        xhci = session;
        for (uint32_t port = 1U;
             port <= session->max_ports && !bound; ++port) {
            const uint32_t raw = xhci_read32(xhci_portsc(port), 0U);
            uint32_t powered_off = 0U;
            uint32_t waited_ms = 0U;
            struct xhci_port_bring_up bring;
            uint32_t now;
            uint8_t speed;
            bool occupied = false;

            if ((raw & XHCI_PORTSC_CCS) == 0U) {
                continue;
            }
            /* The device the boot walk attached as storage keeps its port:
               resetting it would invalidate the block view it serves. */
            if (session == bound_storage && port == bound_port) {
                continue;
            }
            /* A port with a live HID role is never reset. */
            for (uint8_t role = 0U; role < XHCI_HID_DEVICES; ++role) {
                const struct xhci_hid_device *device = &session->devices[role];

                if (device->ready && device->port == port) {
                    occupied = true;
                    break;
                }
            }
            if (occupied || attempts >= XHCI_STORAGE_MAX_ATTACH_ATTEMPTS) {
                continue;
            }
            ++attempts;
            now = xhci_port_bring_up(port, &powered_off, &bring, &waited_ms);
            if ((now & XHCI_PORTSC_CCS) == 0U) {
                continue;
            }
            speed = (uint8_t)((now & XHCI_PORTSC_SPEED_MASK) >>
                              XHCI_PORTSC_SPEED_SHIFT);
            if (!xhci_storage_attach(&session->storage, port, speed)) {
                continue;
            }
            /*
             * A second storage device (the diagnostic stick behind a foreign
             * one) replaces the machine's storage pointer.  The earlier
             * session stays allocated in its slot: it is no longer written,
             * and the slot pool is bounded.
             */
            xhci_machine.storage_session = session;
            ++xhci_machine.storage_candidates_bound;
            (void)xhci_storage_medium_probe(&session->storage);
            bound = install_journal_platform_ready();
        }
    }
    /*
     * ITEM-582: the boot survey releases the session of a controller that
     * bound nothing, so no live session above sees a stick plugged in later.
     * The last survey session is kept for exactly this case: probe its ports
     * with the production walk, which binds the storage device (and its medium)
     * when one is now connected.  HID roles that were bound at boot live in
     * their own sessions and are not touched.
     */
    if (!visited && !bound && xhci_machine.survey_session != NULL &&
        xhci_machine.survey_session->in_use &&
        !xhci_machine.survey_session->fatal_valid) {
        struct xhci_session *session = xhci_machine.survey_session;
        struct xhci_scan scan;

        xhci = session;
        xhci_scan_init(&scan, xhci_machine.controllers_total,
                       XHCI_MAX_ATTACH_ATTEMPTS);
        xhci_scan_begin_controller(&scan);
        xhci_probe_ports(&scan);
        if (xhci_session_holds_role(session)) {
            xhci_machine.survey_session = NULL;
        }
        bound = install_journal_platform_ready();
    }
    xhci = previous;
    return bound;
}

bool xhci_storage_medium_publish(void) {
    if (install_journal_platform_ready()) {
        return true;
    }
    if (xhci_machine.storage_session != NULL) {
        if (xhci_storage_medium_probe(
                &xhci_machine.storage_session->storage) &&
            install_journal_platform_ready()) {
            return true;
        }
        /* The device is attached but bound no medium; a later stick may still
           arrive on another port, so the rescan below runs too. */
    }
    /* ITEM-582: the operator may have plugged the diagnostic stick in after
       boot; enumerate the ports the boot walk read empty. */
    if (xhci_rescan_storage()) {
        return true;
    }
    if (xhci_machine.storage_session == NULL) {
        const uint64_t flags = serial_sequence_begin();

        console_journal_begin();
        serial_write(
            "[INSTALL-MEDIUM] controller=xhci state=UNAVAILABLE "
            "reason=NO_DEVICE\n");
        console_journal_end();
        serial_sequence_end(flags);
    }
    return false;
}

const char *xhci_storage_medium_result(void) {
    if (xhci_machine.storage_session == NULL ||
        !xhci_machine.storage_session->storage.medium_probed) {
        return "UNCHECKED";
    }
    return install_medium_result_string(
        xhci_machine.storage_session->storage.medium_result);
}

/*
 * ITEM-541: the same verdict with the reason that decided it, for the refusal
 * page.  `UNCHECKED` used to fold "no device at all" and "a device nobody
 * probed" into one word - exactly the F-6 ambiguity - so the two now carry
 * their own reasons (NO_DEVICE, NOT_PROBED), and a verdict carries the name the
 * probe used.  Returns true only for MATCH.
 */
bool xhci_storage_medium_status(const char **state, const char **reason) {
    struct xhci_storage_device *storage;

    if (xhci_machine.storage_session == NULL) {
        if (state != NULL) {
            *state = "UNAVAILABLE";
        }
        if (reason != NULL) {
            *reason = "NO_DEVICE";
        }
        return false;
    }
    storage = &xhci_machine.storage_session->storage;
    if (!storage->medium_probed) {
        if (state != NULL) {
            *state = "UNCHECKED";
        }
        if (reason != NULL) {
            *reason = "NOT_PROBED";
        }
        return false;
    }
    if (state != NULL) {
        *state = install_medium_result_string(storage->medium_result);
    }
    if (reason != NULL) {
        *reason = storage->medium_reason != NULL ? storage->medium_reason
                                                 : "UNKNOWN";
    }
    return storage->medium_result == INSTALL_MEDIUM_MATCH;
}

/*
 * ITEM-519 increment 2: what became of the mass-storage device on this
 * session - its identity, the capacity the device itself declared, and the
 * first sector the bulk path actually read, as one word of evidence that
 * the data stage carried the medium's bytes and not the driver's hope.
 */
static void xhci_log_storage_record(
    const struct xhci_storage_device *storage,
    const char *state,
    const char *reason,
    const char *detail,
    bool bound) {
    const uint64_t flags = serial_sequence_begin();

    console_journal_begin();
    XHCI_PUBLISH("[USB-STORAGE] pci=");
    xhci_publish_location();
    XHCI_PUBLISH(" port=");
    XHCI_PUBLISH_U64(storage->port);
    XHCI_PUBLISH(" vid=");
    xhci_publish_hex16(storage->vendor_id);
    XHCI_PUBLISH(" pid=");
    xhci_publish_hex16(storage->product_id);
    XHCI_PUBLISH(" state=");
    XHCI_PUBLISH(state);
    XHCI_PUBLISH(" reason=");
    XHCI_PUBLISH(reason != NULL ? reason : "UNKNOWN");
    if (detail != NULL && detail[0] != '\0') {
        XHCI_PUBLISH(" detail=");
        XHCI_PUBLISH(detail);
    }
    if (storage->configure_diag_valid) {
        XHCI_PUBLISH(" drp=");
        xhci_publish_hex32(storage->configure_diag[0]);
        XHCI_PUBLISH(" add=");
        xhci_publish_hex32(storage->configure_diag[1]);
        XHCI_PUBLISH(" dci=");
        xhci_publish_hex32(storage->configure_diag[2]);
        XHCI_PUBLISH(" slot=");
        xhci_publish_hex32(storage->configure_diag[3]);
    }
    if (bound) {
        XHCI_PUBLISH(" blocks=");
        XHCI_PUBLISH_U64(storage->usb.block_count);
        XHCI_PUBLISH(" block=");
        XHCI_PUBLISH_U64(storage->usb.block_size);
        XHCI_PUBLISH(" iface=");
        XHCI_PUBLISH_U64(storage->interface_number);
        /* ITEM-545: what the Configure Endpoint context carried as Max Burst
           Size, so a device that declared a SuperSpeed burst is visible in the
           record instead of only in the context it cannot be read back from. */
        XHCI_PUBLISH(" burst_in=");
        XHCI_PUBLISH_U64(storage->bulk_in_max_burst);
        XHCI_PUBLISH(" burst_out=");
        XHCI_PUBLISH_U64(storage->bulk_out_max_burst);
        if (storage->sector0_valid) {
            const uint32_t word =
                (uint32_t)storage->sector0[0] |
                ((uint32_t)storage->sector0[1] << 8U) |
                ((uint32_t)storage->sector0[2] << 16U) |
                ((uint32_t)storage->sector0[3] << 24U);

            XHCI_PUBLISH(" sector0=");
            xhci_publish_hex32(word);
        }
    }
    XHCI_PUBLISH("\n");
    serial_sequence_end(flags);
    console_journal_end();
}

/*
 * ITEM-519 increment 3: the verdict of a write proof on the bound stick.  The
 * fields are the sector the proof used, whether the write was accepted, whether
 * the pattern read back byte for byte, and whether the original sector was
 * restored and read back unchanged.  `reason` names the first step that failed,
 * so a false result is never left without a name.
 */
static void xhci_log_storage_proof_record(
    const struct xhci_storage_device *storage,
    uint64_t lba,
    const char *reason,
    bool write_ok,
    bool verify_ok,
    bool restore_ok) {
    const uint64_t flags = serial_sequence_begin();

    console_journal_begin();
    XHCI_PUBLISH("[USB-STORAGE] pci=");
    xhci_publish_location();
    XHCI_PUBLISH(" port=");
    XHCI_PUBLISH_U64(storage->port);
    XHCI_PUBLISH(" vid=");
    xhci_publish_hex16(storage->vendor_id);
    XHCI_PUBLISH(" pid=");
    xhci_publish_hex16(storage->product_id);
    XHCI_PUBLISH(" state=WRITE_PROOF reason=");
    XHCI_PUBLISH(reason != NULL ? reason : "UNKNOWN");
    XHCI_PUBLISH(" lba=");
    XHCI_PUBLISH_U64(lba);
    XHCI_PUBLISH(" byte=");
    XHCI_PUBLISH_U64(XHCI_STORAGE_PROOF_BYTES);
    XHCI_PUBLISH(" write=");
    XHCI_PUBLISH(write_ok ? "OK" : "FAILED");
    XHCI_PUBLISH(" verify=");
    XHCI_PUBLISH(verify_ok ? "OK" : "FAILED");
    XHCI_PUBLISH(" restore=");
    XHCI_PUBLISH(restore_ok ? "OK" : "FAILED");
    XHCI_PUBLISH("\n");
    serial_sequence_end(flags);
    console_journal_end();
}

#undef XHCI_PUBLISH_HEX
#undef XHCI_PUBLISH_U64
#undef XHCI_PUBLISH
#undef XHCI_PUBLISH_TEXT

/*
 * ITEM-406: stop the controller the driver is leaving, so it cannot write the
 * DMA pool the next controller is about to own. A controller left running keeps
 * its command ring, event ring and DCBAAP pointing at that pool, and a port
 * status change (a device plugged in during the scan, for one) would put an
 * event TRB into the middle of the next controller's event ring. Best effort and
 * bounded: a controller that refuses to halt costs the bound and nothing else.
 */
static void xhci_controller_stop(void) {
    uint32_t command;

    if (xhci->regs == NULL) {
        return;
    }
    command = xhci_read32(xhci_operational(XHCI_OP_USBCMD), 0U);
    xhci_write32(xhci_operational(XHCI_OP_USBCMD), 0U,
                 command & ~(XHCI_USBCMD_RS | XHCI_USBCMD_INTE));
    (void)xhci_wait_status(XHCI_USBSTS_HCH, XHCI_USBSTS_HCH,
                           XHCI_SPIN_LEAVE);
    xhci->regs = NULL;
}

/*
 * ITEM-438: bring one controller up far enough to probe its ports: memory space
 * and bus mastering on, BAR0 mapped, controller started.  The boot scan and the
 * re-bind of a silent candidate both need exactly this, so it lives in one
 * place; the caller owns the record and the `continue`/`stop` decisions.
 */
static void xhci_controller_begin(const struct pci_device_header *header);

static bool xhci_controller_open_rest(const struct pci_device_header *header,
                                      uint64_t hhdm_offset) {
    const struct pci_bar *bar;
    uint32_t command;

    /* Memory space and bus mastering must be on before the register window
       answers and before the controller may read the rings. */
    command = xhci_pci_read32(header->location.bus, header->location.device,
                              header->location.function, XHCI_PCI_COMMAND);
    xhci_pci_write32(header->location.bus, header->location.device,
                     header->location.function, XHCI_PCI_COMMAND,
                     command | XHCI_PCI_COMMAND_MEMORY |
                         XHCI_PCI_COMMAND_BUS_MASTER);

    if (!header->bars[0].present || header->bars[0].io_space ||
        header->bars[0].base == 0U) {
        xhci_set_status(XHCI_STATUS_NO_MEMORY_BAR,
                        xhci_detail_controller("BAR0", "NOT_MEMORY"));
        return false;
    }
    bar = &header->bars[0];
    xhci->bar_physical = bar->base;
    xhci->bar_length = bar->length;
    {
        uint64_t length = bar->length;
        uint64_t pages;

        if (length < XHCI_MMIO_MIN_BYTES) {
            length = XHCI_MMIO_MIN_BYTES;
        }
        if (length > (uint64_t)XHCI_MMIO_MAX_PAGES * 4096U) {
            length = (uint64_t)XHCI_MMIO_MAX_PAGES * 4096U;
        }
        xhci->mmio_bytes = (uint32_t)length;
        pages = (length + 4095U) / 4096U;
        for (uint64_t page = 0U; page < pages; ++page) {
            volatile void *window = NULL;

            if (vmm_map_mmio_page(bar->base + page * 4096U, hhdm_offset,
                                  &window) != VMM_OK ||
                window == NULL) {
                xhci->regs = NULL;
                xhci_set_status(XHCI_STATUS_MMIO_MAP_FAILED,
                                xhci_detail_controller("MMIO", "MAP_FAILED"));
                return false;
            }
            if (page == 0U) {
                xhci->regs = (volatile uint8_t *)window;
            }
        }
    }
    return xhci_controller_start();
}

/*
 * ITEM-574: one visit's controller open, with the firmware's routing state
 * read before the visit writes anything and the visit's final state read
 * afterwards - whatever the outcome, so a controller that failed early still
 * publishes the registers it had.
 */
static bool xhci_controller_open(const struct pci_device_header *header,
                                 uint64_t hhdm_offset) {
    bool opened;

    xhci_controller_begin(header);
    xhci_route_snapshot(xhci, &xhci->route_before);
    opened = xhci_controller_open_rest(header, hhdm_offset);
    xhci_route_snapshot(xhci, &xhci->route_after);
    return opened;
}

/*
 * ITEM-406: make the driver's state describe one controller. Everything the
 * hardware half reads while that controller comes up is reset here, so a
 * controller that fails cannot leave a ring, a slot or a published identity
 * behind for the next one; the fixed DMA pool is re-zeroed for the same reason.
 * The keyboard counters and the decoded-event queue are not part of this: they
 * belong to the console, not to a controller.
 */
static void xhci_controller_begin(const struct pci_device_header *header) {
    /* ITEM-438: the controller's own descriptor is kept so a candidate that has
       to be re-bound after the walk can re-open exactly this function. */
    xhci->header = *header;
    xhci->location = header->location;
    xhci->pci_vendor = header->vendor_id;
    xhci->pci_device = header->device_id;
    /*
     * ITEM-452: the session's published verdict is reset with the rest of the
     * controller state.  A freshly acquired slot was zeroed by the acquire and
     * `XHCI_STATUS_OK` is zero, so a visit whose record is published before an
     * outcome was reached - every device on it refused for a role the machine
     * already has, or a mouse-only controller - would otherwise compose
     * `state=` from a NULL pointer.  An outcome always overwrites these three.
     */
    xhci->status = XHCI_STATUS_UNKNOWN;
    xhci->detail = "NOT_PROBED";
    xhci->state = "UNAVAILABLE";
    xhci->bar_physical = 0U;
    xhci->bar_length = 0U;
    xhci->mmio_bytes = 0U;
    xhci->regs = NULL;
    xhci->caplength = 0U;
    xhci->max_ports = 0U;
    xhci->max_slots = 0U;
    xhci->slots_enabled = 0U;
    /* ITEM-407: the evidence belongs to one controller. A controller that fails
       before its ports are read publishes none of it, and must not publish the
       previous controller's port values. */
    xhci->usbcmd = 0U;
    xhci->usbsts = 0U;
    /* ITEM-574: the routing evidence and the shared window belong to one
       visit; a visit that fails before them must not publish the previous
       controller's registers or verdict. */
    memset(&xhci->route_before, 0, sizeof(xhci->route_before));
    memset(&xhci->route_after, 0, sizeof(xhci->route_after));
    xhci->route_outcome[0] = '\0';
    xhci->route_detail[0] = '\0';
    xhci->route_target_usb2 = 0U;
    xhci->route_target_usb3 = 0U;
    xhci->route_usb2_field = 0U;
    xhci->route_wrote_usb2 = 0U;
    xhci->route_wrote_usb3 = 0U;
    xhci->route_restored = 0U;
    xhci->legacy[0] = '\0';
    xhci->settle_ms = 0U;
    xhci->settle_bound_ms = 0U;
    xhci->settle_pending = 0U;
    xhci->hcs_params2 = 0U;
    xhci->scratchpad_count = 0U;
    xhci->scratchpad_page_bytes = 0U;
    xhci->slot_area_bytes = 0U;
    xhci->device_contexts_offset = 0U;
    xhci->device_contexts_bytes = 0U;
    xhci->scratchpad_array_offset = 0U;
    xhci->scratchpad_array_bytes = 0U;
    xhci->scratchpad_pages_offset = 0U;
    xhci->scratchpad_array_physical = 0U;
    xhci->dcbaa0 = 0U;
    xhci->pool_bytes = 0U;
    xhci->fatal_valid = false;
    xhci->fatal[0] = '\0';
    xhci->fatal_cause[0] = '\0';
    xhci->portsc_ports = 0U;
    xhci->portsc_powered_mask = 0U;
    xhci->portsc_powered_off = 0U;
    for (uint32_t index = 0U; index < XHCI_PORT_EVIDENCE_MAX; ++index) {
        xhci->portsc[index] = 0U;
    }
    /*
     * ITEM-448: every binding belongs to the controller that carries it.  The
     * rings, the report buffers and the device contexts live in the DMA pool
     * this bring-up is about to re-zero, so a binding that survived from the
     * previous controller would point at structures that no longer exist - the
     * two devices are therefore rebuilt from nothing for every controller, and
     * only the machine-wide counters (how many candidates were bound, silent or
     * proven) outlive a controller.
     */
    for (uint32_t role = 0U; role < XHCI_HID_DEVICES; ++role) {
        struct xhci_hid_device *device = xhci_device((uint8_t)role);

        memset(device, 0, sizeof(*device));
        device->role = (uint8_t)role;
        device->report_length =
            role == XHCI_HID_ROLE_MOUSE ? USB_HID_BOOT_MOUSE_TRANSFER_LENGTH
                                        : USB_HID_BOOT_REPORT_LENGTH;
        device->layout.modifier_offset = USB_HID_REPORT_OFFSET_NONE;
        usb_hid_state_reset(&device->hid);
        usb_hid_mouse_state_reset(&device->mouse);
    }
    /* ITEM-519 increment 2: the storage binding dies with the session it
       was built in, like the two HID roles. */
    memset(&xhci->storage, 0, sizeof(xhci->storage));
    xhci->event_index = 0U;
    xhci->event_cycle = 1U;
}

/*
 * ITEM-448: bind one remembered candidate again after the machine-wide walk.
 *
 * The re-bind follows an HCRST, and a controller reset clears every root
 * port's enabled state - so the port is claimed and, when it is not enabled,
 * reset again, exactly as the scan does before its own attach.  Skipping this
 * worked on QEMU's model, which keeps the port state across the reset, and
 * would have failed on the acceptance host's controllers, which do not.
 *
 * The caller opens the controller once and can bind both remembered devices
 * into it; opening it per device would re-zero the DMA pool the first device's
 * rings and report buffers live in, because every binding belongs to the
 * controller session that carries it.
 *
 * Returns true when the device is bound and ready.
 */
static bool xhci_rebind_device(struct xhci_hid_device *device, uint8_t port) {
    struct xhci_device_path *const path = xhci_session_path(xhci, port);
    bool ready = true;
    uint8_t speed = 0U;

    /*
     * ITEM-579: the device may already be enumerated on this port - the pointer
     * of the same composite device, live on the shared path.  Re-binding the
     * keyboard then joins that path: no port reset and no second Address
     * Device.  Resetting here is what took the 10231684 pointer down (its
     * endpoint was left Halted after a USB transaction error, `reports=0`).
     */
    if (path != NULL && xhci_path_live(path)) {
        speed = path->speed;
    } else {
        const uint32_t portsc = xhci_port_claim(port, NULL);

        speed = (uint8_t)((portsc & XHCI_PORTSC_SPEED_MASK) >>
                          XHCI_PORTSC_SPEED_SHIFT);
        ready = (portsc & XHCI_PORTSC_CCS) != 0U;
        /* The previous attempt left no path, so the device still holds the
           address that attempt gave it: a real USB reset is required. */
        if (ready) {
            ready = xhci_port_reset(port, &speed, true);
        }
    }
    if (ready && xhci_speed_ep0_max_packet(speed) != 0U &&
        xhci_attach_device(device, port, speed)) {
        return true;
    }
    if (ready) {
        xhci_attach_refuse(device, XHCI_STATUS_ENUMERATION_FAILED, "REBIND",
                           xhci_detail_port(port, "REBIND",
                                            "ATTACH_FAILED"));
    } else {
        xhci_attach_refuse(device, XHCI_STATUS_NO_DEVICE, "REBIND",
                           xhci_detail_port(port, "REBIND", "PORT_GONE"));
    }
    return false;
}

/* ITEM-448/452: do two remembered candidates share one controller?  The
   pointer's session is already open when a silent keyboard candidate on the
   same controller is re-bound, so the re-bind must not open (and re-zero) that
   session again. */
static bool xhci_same_controller(const struct pci_device_header *left,
                                 const struct pci_device_header *right) {
    return left->location.bus == right->location.bus &&
           left->location.device == right->location.device &&
           left->location.function == right->location.function;
}

/* Does this session hold a role the machine keeps? */
static bool xhci_session_holds_role(const struct xhci_session *session) {
    return session != NULL &&
           (xhci_machine.keyboard_session == session ||
            xhci_machine.mouse_session == session ||
            xhci_machine.storage_session == session);
}

/* ITEM-452: at most one live session per role, so a machine with both roles
   bound still has a free slot for the controller being surveyed. */
_Static_assert(XHCI_SESSION_SLOTS > XHCI_HID_DEVICES + 1U,
               "the walk needs a scratch session beside all live roles");

/*
 * ITEM-579: re-bind the remembered silent keyboard candidate.
 *
 * The walk's last resort: no candidate ever proved itself, but the first one
 * that parsed as a keyboard is still a keyboard - it may simply be a device
 * that ignores SET_IDLE and reports only on change.  ITEM-452: the retry uses
 * the session that is already live on that controller when the pointer was
 * found there too, so the pointer's rings are not disturbed; otherwise it
 * opens the controller in a free session.
 *
 * ITEM-579: the re-bind of one role must not reset the device a live role of
 * the same physical device is using; the path rules inside xhci_rebind_device()
 * decide that, and a physical reset that is really required restores every role
 * of the device instead of dropping them.
 */
static void xhci_silent_candidate_retry(struct xhci_scan *scan,
                                        uint64_t hhdm_offset) {
    const struct pci_device_header *header =
        &xhci_machine.silent_candidate.header;
    struct xhci_session *session = NULL;
    bool opened = false;

    /*
     * ITEM-579: the guard lives here, not only at the call site, so the host
     * proof drives exactly the production decision - a machine that already
     * bound a keyboard (on the silent candidate's port or another one) has
     * nothing to retry.
     */
    if (xhci_machine.keyboard_session != NULL ||
        !xhci_machine.silent_candidate.valid) {
        return;
    }
    if (xhci_machine.mouse_session != NULL &&
        xhci_same_controller(&xhci_machine.mouse_session->header, header)) {
        session = xhci_machine.mouse_session;
    } else {
        session = xhci_session_acquire();
        if (session != NULL && xhci_controller_open(header, hhdm_offset)) {
            opened = true;
        } else if (session != NULL) {
            xhci_log_record();
            xhci_session_release(session);
            session = NULL;
        }
    }
    if (session == NULL) {
        return;
    }
    {
        struct xhci_hid_device *device =
            xhci_session_device(session, XHCI_HID_ROLE_KEYBOARD);

        /* The re-bind works in this session: its rings, its pool and its
           location are the ones the device record and the controller record
           have to name. */
        xhci = session;
        xhci_machine.silent_retry = true;
        if (xhci_rebind_device(device, xhci_machine.silent_candidate.port)) {
            xhci_machine.keyboard_session = session;
            xhci_scan_note_controller(scan, XHCI_STATUS_OK, "OK", true);
            xhci_log_device_record(device->port, device->speed, "KEYBOARD",
                                   "BOUND", "OK", NULL, device);
        } else {
            xhci_log_device_record(
                xhci_machine.silent_candidate.port,
                xhci_machine.silent_candidate.speed, "KEYBOARD", "REFUSED",
                xhci_status_string(xhci->status), xhci->detail, device);
        }
        xhci_log_record();
        /* ITEM-573: the retry's outcome is part of the visit - a keyboard this
           path binds must not stay REFUSED in the kept evidence. */
        xhci_evidence_keep_session(session, false);
        if (!xhci_session_holds_role(session) && opened) {
            xhci_session_release(session);
        }
    }
}

enum xhci_status xhci_keyboard_init(uint64_t hhdm_offset) {
    struct pci_config_io io;
    struct pci_device_header controllers[XHCI_MAX_CONTROLLERS];
    size_t controller_count = 0U;
    enum pci_status pci_status;
    struct xhci_scan scan;
    enum xhci_status verdict;

    memset(&xhci_machine, 0, sizeof(xhci_machine));
    xhci_evidence_reset();
    memset(xhci_sessions, 0, sizeof(xhci_sessions));
    xhci = &xhci_sessions[0];
    xhci->status = XHCI_STATUS_UNKNOWN;
    xhci->detail = "NOT_PROBED";
    xhci->state = "UNAVAILABLE";
    xhci_machine.hhdm_offset = hhdm_offset;
    xhci_machine.mouse_status = USB_HID_STATUS_NO_HID_MOUSE;
    xhci_machine.verdict_status = XHCI_STATUS_UNKNOWN;
    xhci_machine.verdict_detail = "NOT_PROBED";

    memset(&io, 0, sizeof(io));
    io.read32 = xhci_pci_read;
    io.write32 = xhci_pci_write;
    pci_status = pci_scan_class(&io, XHCI_PCI_CLASS_SERIAL_BUS,
                                XHCI_PCI_SUBCLASS_USB, XHCI_PCI_PROGIF_XHCI,
                                controllers, XHCI_MAX_CONTROLLERS,
                                &controller_count);
    if (pci_status == PCI_CAPACITY) {
        /*
         * More xHCI functions than this driver tracks. The scan filled every
         * entry before it stopped, so the driver still visits all of them and
         * says so, instead of reporting a machine without a controller.
         */
        controller_count = XHCI_MAX_CONTROLLERS;
    } else if (pci_status != PCI_OK) {
        controller_count = 0U;
    }
    xhci_machine.initialized = true;
    xhci_machine.controllers_total = (uint32_t)controller_count;
    if (controller_count == 0U) {
        xhci_set_status(XHCI_STATUS_CONTROLLER_ABSENT, "NO_XHCI_FUNCTION");
        xhci_log_record();
        xhci_machine_set_verdict(XHCI_STATUS_CONTROLLER_ABSENT,
                                 "NO_XHCI_FUNCTION");
        xhci_evidence_keep_mouse();
        return xhci->status;
    }

    xhci_scan_init(&scan, (uint32_t)controller_count,
                   XHCI_MAX_ATTACH_ATTEMPTS);

    /*
     * ITEM-406: every xHCI function the machine has, in PCI order, and every
     * root port of each. The acceptance host carries three controllers and the
     * keyboard is on the second one, behind a first controller whose only
     * device is a wireless receiver, so the scan may not stop at the first
     * controller that answers or at the first device it can address. Each
     * visit publishes its own record, and the machine-wide verdict when
     * nothing is found is the most informative one.
     *
     * ITEM-452: the walk visits every controller even after the keyboard is
     * bound, and a visit that binds a needed role keeps its session - its DMA
     * pool, its rings and its event ring - alive with the controller still
     * running.  That is what lets the owner's K120 on one controller and the
     * Logitech receiver on another feed the console at the same time.  A visit
     * that binds nothing releases its session, and its pool is reused by the
     * next visit instead of being handed back to the physical allocator.
     */
    for (size_t index = 0U; index < controller_count; ++index) {
        const struct pci_device_header *header = &controllers[index];
        struct xhci_session *session = xhci_session_acquire();

        if (session == NULL) {
            /*
             * Unreachable while XHCI_SESSION_SLOTS > XHCI_HID_DEVICES + 1 (the
             * static assertion above): the keyboard, the pointer and the
             * mass-storage device can hold three sessions and the fourth is
             * always free.  Kept as a guard so a future change cannot
             * write a visit into a live session's pool; the machine verdict
             * names the pool shortage instead.
             */
            xhci_machine_set_verdict(XHCI_STATUS_DMA_UNAVAILABLE,
                                     "NO_SESSION_SLOT");
            continue;
        }
        if (!xhci_controller_open(header, hhdm_offset)) {
            xhci_log_record();
            xhci_session_release(session);
            continue;
        }
        xhci_scan_begin_controller(&scan);
        xhci_probe_ports(&scan);
        xhci_log_record();
        /* ITEM-573: the visit's evidence, kept before the session may be
           released (the release copies it again with `released=1` and the
           controller's final words). */
        xhci_evidence_keep_session(session, false);
        if (!xhci_session_holds_role(session)) {
            /* ITEM-582: the last survey is kept alive, so a device plugged in
               after boot can be enumerated on the controller it surveyed; an
               earlier one is released as before. */
            if (index + 1U == controller_count) {
                xhci_machine.survey_session = session;
            } else {
                xhci_session_release(session);
            }
        }
    }

    /*
     * ITEM-438: no candidate ever proved itself, but the first one that parsed
     * as a keyboard is still a keyboard - it may simply be a device that
     * ignores SET_IDLE and reports only on change.  Bind it again rather than
     * ending the walk with nothing; `retry=1` in the record says the binding
     * came back after the whole machine was visited.
     *
     * ITEM-579: the retry is its own function, because the host proof has to
     * drive the whole initialization path - the walk, the pointer's binding and
     * this step - rather than stop at xhci_probe_ports(); a proof that stops
     * there cannot see a re-bind that resets the device out from under the
     * pointer, which is what this item fixes.
     */
    if (xhci_machine.keyboard_session == NULL &&
        xhci_machine.silent_candidate.valid) {
        xhci_silent_candidate_retry(&scan, hhdm_offset);
    }

    if (xhci_machine.keyboard_session != NULL) {
        verdict = XHCI_STATUS_OK;
        xhci_machine_set_verdict(verdict, "OK");
    } else {
        /*
         * Nothing on this machine is a boot keyboard. Publish the most
         * informative rejection (the controller, the port and the stage are in
         * the detail) and keep booting: the serial console and the network
         * management plane are unaffected, exactly as on a machine without a
         * controller. The detail is copied out of the scan: that storage ends
         * with this function, while the machine verdict is read by the boot
         * record and by the console long afterwards.
         */
        verdict = scan.status;
        xhci_machine_set_verdict(verdict, scan.detail);
    }
    /*
     * Leave the pointer on a session that is still live, so the accessors that
     * read the current session answer from a real controller rather than from
     * a released scratch slot.
     */
    if (xhci_machine.keyboard_session != NULL) {
        xhci = xhci_machine.keyboard_session;
    } else if (xhci_machine.mouse_session != NULL) {
        xhci = xhci_machine.mouse_session;
    } else {
        xhci = &xhci_sessions[0];
    }
    xhci_evidence_keep_mouse();
    return verdict;
}

/*
 * ITEM-448: one completed transfer, decoded for the device it belongs to.
 *
 * The keyboard's half is the ITEM-436/438 rule unchanged - a boot report is
 * classified against its eight bytes, a report-protocol keyboard against the
 * layout its own descriptor declared - and the mouse's half is the pointer's
 * own rule: the boot report's three fields (plus the wheel byte a device may
 * append) or the report-descriptor layout, decoded into pointer deltas and
 * button edges.
 */
static void xhci_decode_keyboard_report(struct xhci_hid_device *device,
                                        uint8_t slot,
                                        uint32_t transfer_length) {
    const uint8_t *report = xhci_dma_at(
        xhci_report_buffer_offset(device, slot));
    uint8_t boot_report[USB_HID_BOOT_REPORT_LENGTH];
    uint32_t received = 0U;
    bool whole_report = false;

    if (device->report_mode) {
        if (transfer_length > device->report_length) {
            device->last_report_bytes = 0U;
            ++device->refused_report_count;
            return;
        }
        received = device->report_length - transfer_length;
        whole_report = usb_hid_layout_build_boot_report(
            &device->layout, report, received, boot_report);
    } else {
        const enum usb_hid_report_length length_class =
            usb_hid_boot_report_classify(transfer_length, &received);

        if (length_class == USB_HID_REPORT_LENGTH_EXACT) {
            for (uint32_t index = 0U; index < USB_HID_BOOT_REPORT_LENGTH;
                 ++index) {
                boot_report[index] = report[index];
            }
            whole_report = true;
        }
    }

    device->last_report_bytes = received;
    if (whole_report) {
        struct keyboard_event events[USB_HID_BOOT_KEY_COUNT + 6U];
        const size_t count = usb_hid_boot_report_events(
            &device->hid, boot_report, events,
            sizeof(events) / sizeof(events[0]));
        /*
         * ITEM-482: stamp every decoded event with the completion's own clock,
         * so the console can publish the wait until the character is on screen.
         * The stamp is taken once for the whole report, so the events of one
         * report carry the same instant.
         */
        const uint64_t decoded_cycles = timer_cycles();

        ++device->report_count;
        /* ITEM-438: a whole report that carries no key is a normal answer -
           the device reports on change and reports the release too - so the
           count of keys it produced is what says whether this keyboard ever
           typed anything. */
        device->key_count += (uint32_t)count;
        for (size_t index = 0U; index < count; ++index) {
            events[index].reported_cycles = decoded_cycles;
            xhci_push_event(&events[index]);
        }
        /* ITEM-459: a key that produced no character names itself. */
        xhci_note_unmapped_usage(device);
    } else {
        ++device->refused_report_count;
    }
}

static void xhci_decode_mouse_report(struct xhci_hid_device *device,
                                     uint8_t slot,
                                     uint32_t transfer_length) {
    const uint8_t *report = xhci_dma_at(
        xhci_report_buffer_offset(device, slot));
    struct usb_hid_mouse_event event;
    uint32_t received = 0U;
    bool whole_report = false;

    if (transfer_length > device->report_length) {
        device->last_report_bytes = 0U;
        ++device->refused_report_count;
        return;
    }
    received = device->report_length - transfer_length;
    if (device->report_mode) {
        whole_report = usb_hid_mouse_layout_decode(
            &device->mouse, &device->mouse_layout, report, received, &event);
    } else {
        whole_report = usb_hid_boot_mouse_report(&device->mouse, report,
                                                 received, &event);
    }
    device->last_report_bytes = received;
    if (whole_report) {
        ++device->report_count;
        xhci_push_mouse_event(&event);
    } else {
        ++device->refused_report_count;
    }
}

/*
 * ITEM-452: one session's service pass.  Everything below works on the session
 * the global pointer names, so the caller sets it first.
 */
/*
 * ITEM-452: one decoded event, applied to whichever of this session's two
 * devices it belongs to.  Returns true when a bound device owned it.
 *
 * This lives apart from the service pass because the control and command waits
 * share the session's event ring.  An event that was not the one a wait asked
 * for used to be consumed and forgotten, which left the other role's
 * `transfer_pending` set and stopped that device from ever being re-armed -
 * a keyboard that re-sends an idle report about once a second makes the race
 * reachable whenever a second role is enumerated in the session that already
 * holds the first (a mouse on a later port of the keyboard's controller, or
 * the silent-keyboard re-bind in the pointer's live session).
 */
static bool xhci_apply_event(const struct xhci_event *event) {
    struct xhci_hid_device *device = NULL;

    if (event == NULL) {
        return false;
    }
    /*
     * ITEM-473 (СРОЧНО-23): a Host Controller Event belongs to the controller,
     * not to a slot, and Event Ring Full (21) arrives this way.  Counting it on
     * the session is what turns "the pointer went silent" into the reason the
     * next journal can read; the transfer-event handling below never sees it.
     */
    if (event->type == XHCI_TRB_HOST_CONTROLLER_EVENT) {
        ++xhci->controller_event_count;
        if (event->completion_code == XHCI_COMPLETION_EVENT_RING_FULL) {
            ++xhci->event_ring_full_events;
        }
        return true;
    }
    if (event->type == XHCI_TRB_PORT_STATUS_CHANGE) {
        /*
         * ITEM-448: a port-status change belongs to whichever device sits on
         * that port; the other device keeps its binding.
         */
        for (uint32_t role = 0U; role < XHCI_HID_DEVICES; ++role) {
            struct xhci_hid_device *candidate = xhci_device((uint8_t)role);

            if (!candidate->ready || candidate->port != event->port_id) {
                continue;
            }
            {
                const uint32_t portsc =
                    xhci_read32(xhci_portsc(candidate->port), 0U);

                if ((portsc & XHCI_PORTSC_CCS) == 0U) {
                    candidate->ready = false;
                    candidate->transfer_pending = false;
                    /* ITEM-469: the queue dies with the binding. */
                    candidate->armed_count = 0U;
                    candidate->armed_head = 0U;
                    /* ITEM-479: so does the watchdog's silence clock. */
                    candidate->watchdog_progress_ticks = 0U;
                    candidate->watchdog_reported = false;
                    if (role == XHCI_HID_ROLE_KEYBOARD) {
                        xhci_note_keyboard_lost(XHCI_STATUS_KEYBOARD_LOST,
                                                "PORT_DISCONNECTED");
                    }
                }
            }
        }
        return true;
    }
    if (event->type != XHCI_TRB_TRANSFER_EVENT) {
        return false;
    }
    for (uint32_t role = 0U; role < XHCI_HID_DEVICES; ++role) {
        struct xhci_hid_device *candidate = xhci_device((uint8_t)role);

        if (candidate->ready && candidate->slot_id == event->slot_id &&
            candidate->dci == event->endpoint_id) {
            device = candidate;
            break;
        }
    }
    if (device == NULL) {
        return false;
    }
    /* ITEM-579: the controller completed one of this role's submitted TRBs.
       Submitted, completed and decoded are three different questions, and a
       pointer that decoded nothing must still show whether its transfers ran. */
    ++device->completion_count;
    /*
     * ITEM-469: this completion retires the oldest armed transfer.  `armed=`
     * (published by the diagnostics) is what is left behind it, so the queue
     * the fix keeps is a number; every completion code is counted, and the
     * missed-service (23) and event-ring-full (21) errors are named separately
     * because they are exactly the failures the single-TRB window produced.
     */
    {
        uint8_t slot = 0U;

        if (device->armed_count > 0U) {
            slot = device->armed_slots[device->armed_head];
            device->armed_head =
                (uint8_t)((device->armed_head + 1U) % XHCI_REPORT_SLOTS);
            --device->armed_count;
        }
        device->transfer_pending = device->armed_count > 0U;
        /*
         * ITEM-470: a completion that leaves this role with no armed TRB at all
         * is the window the device drops a report into.  The top-up below
         * closes it right away; the counter is what makes a window that still
         * happened visible on the next hardware run.
         */
        if (device->armed_count == 0U) {
            ++device->no_trb_count;
        }
        /* ITEM-461: the tick the re-arm wait is measured from. */
        device->transfer_complete_ticks = timer_ticks();
        /* ITEM-473: the most recent code beside its running counts. */
        device->last_completion_code = event->completion_code;
        if (event->completion_code < XHCI_COMPLETION_CODE_COUNT) {
            ++device->completion_code_counts[event->completion_code];
        }
        if (event->completion_code == XHCI_COMPLETION_MSE) {
            ++device->mse_count;
        }
        if (event->completion_code == XHCI_COMPLETION_EVENT_RING_FULL) {
            ++device->event_ring_full_count;
        }
        if (event->completion_code != XHCI_COMPLETION_SUCCESS &&
            event->completion_code != XHCI_COMPLETION_SHORT_PACKET) {
            ++device->error_count;
            ++device->consecutive_errors;
            /*
             * ITEM-579: keep the code, read the endpoint state the completion
             * left behind, and repair a Halted endpoint with the two commands
             * that can: arming more TRBs and ringing the doorbell cannot.
             */
            device->endpoint_state =
                (uint8_t)xhci_device_endpoint_state(device);
            if (device->endpoint_state == XHCI_EP_STATE_HALTED) {
                ++device->halted_count;
                if (xhci_endpoint_restart(device)) {
                    xhci_publish_endpoint_action(device, "RESET", NULL);
                    xhci_top_up_device(device);
                    return true;
                }
                xhci_publish_endpoint_action(device, "LOST", xhci->detail);
                if (device->role == XHCI_HID_ROLE_KEYBOARD) {
                    xhci_note_keyboard_lost(XHCI_STATUS_KEYBOARD_LOST,
                                            "ENDPOINT_LOST");
                }
                /*
                 * ITEM-579: another role of the same physical device must not be
                 * left on a slot that is about to become unusable.  A physical
                 * reset is the escalation: it stops every role of the device
                 * (this one included) and brings back the ones that were bound.
                 * Only when that fails does this role stay down.
                 */
                if (device->path != NULL) {
                    bool shared = false;

                    for (uint32_t role = 0U; role < XHCI_HID_DEVICES; ++role) {
                        struct xhci_hid_device *other =
                            &xhci->devices[role];

                        if (other != device && other->ready &&
                            other->path == device->path) {
                            shared = true;
                        }
                    }
                    if (shared && xhci_path_restore(xhci, device->path)) {
                        return true;
                    }
                }
                device->ready = false;
                device->transfer_pending = false;
                device->armed_count = 0U;
                device->armed_head = 0U;
                return true;
            }
            if (device->consecutive_errors >= XHCI_MAX_CONSECUTIVE_ERRORS) {
                device->ready = false;
                if (device->role == XHCI_HID_ROLE_KEYBOARD) {
                    xhci_note_keyboard_lost(
                        XHCI_STATUS_KEYBOARD_LOST,
                        xhci_completion_code_string(event->completion_code));
                }
            }
            xhci_top_up_device(device);
            return true;
        }
        device->consecutive_errors = 0U;
        device->endpoint_state = XHCI_EP_STATE_RUNNING;
        /* ITEM-468: a completion the controller raised because the packet was
           shorter than the buffer is the ISP path (the three-byte boot-mouse
           answer); counting it separates "the transfer completed" from "the
           transfer completed with a whole report". */
        if (event->completion_code == XHCI_COMPLETION_SHORT_PACKET) {
            ++device->short_packet_count;
        }
        /* ITEM-467: the uptime of the last completion the device answered with,
           so a pointer that stopped answering has a last-answer time and not
           only a report count. */
        device->last_report_ticks = timer_ticks();
        /* ITEM-479: a completion is progress and ends any silence episode. */
        device->watchdog_progress_ticks = device->last_report_ticks;
        device->watchdog_reported = false;
        /*
         * ITEM-458: the wait between the two most recent reports this device
         * delivered.  The pass drains the ring, so a report that waited in a
         * full ring shows up here as a longer gap, and the record publishes the
         * worst one of the boot.
         */
        {
            const uint64_t serviced_ticks = timer_ticks();

            device->service_gap_ms_max = xhci_service_gap_ms(
                device->service_gap_ms_max, device->service_last_ticks,
                serviced_ticks, HFOC_TIMER_HZ);
            /* ITEM-482: the same gap over the current window, so the
               once-a-second record reports what the last second felt like
               rather than the worst of the whole boot. */
            device->service_gap_window_ms_max = xhci_service_gap_ms(
                device->service_gap_window_ms_max, device->service_last_ticks,
                serviced_ticks, HFOC_TIMER_HZ);
            device->service_last_ticks = serviced_ticks;
        }
        if (device->role == XHCI_HID_ROLE_MOUSE) {
            xhci_decode_mouse_report(device, slot, event->transfer_length);
        } else {
            xhci_decode_keyboard_report(device, slot, event->transfer_length);
        }
        /*
         * ITEM-469: the fix itself - the retired buffer is replaced here, in
         * the completion's own handler, so the endpoint is never left without
         * a TRB while the role is READY and a report in the next 10 ms window
         * is not dropped.
         */
        xhci_top_up_device(device);
    }
    return true;
}

/* ITEM-469: top every bound device back up to the armed depth. */
static void xhci_rearm_devices(void) {
    for (uint32_t role = 0U; role < XHCI_HID_DEVICES; ++role) {
        struct xhci_hid_device *device = xhci_device((uint8_t)role);

        xhci_top_up_device(device);
    }
}

static void xhci_service_session(void) {
    uint32_t events = 0U;
    uint32_t reports = 0U;

    /*
     * ITEM-458: the pass drains the session's event ring.  Until this item it
     * took one whole report and returned, so a keystroke that arrived while the
     * console was printing waited for the next main-loop turn - and the ring,
     * sixteen TRBs deep, silently overflowed when the loop was busy, which is
     * how the 2026-09-19 hardware run lost characters while the screen
     * scrolled.  The two budgets bound the pass instead: the ring is emptied
     * up to XHCI_SERVICE_EVENTS_MAX events and XHCI_SERVICE_REPORTS_MAX whole
     * reports, and the round-robin in xhci_keyboard_service() gives the other
     * session the next turn, so neither controller can starve the machine.
     */
    while (events < XHCI_SERVICE_EVENTS_MAX &&
           reports < XHCI_SERVICE_REPORTS_MAX) {
        struct xhci_event event;

        if (!xhci_next_event(&event)) {
            break;
        }
        ++events;
        if (!xhci_apply_event(&event)) {
            continue;
        }
        if (event.type == XHCI_TRB_TRANSFER_EVENT) {
            ++reports;
        }
    }
    /* ITEM-468: the diagnostic window's share of this session's pass. */
    xhci_diag_window_events += events;
    xhci_diag_window_reports += reports;
    ++xhci_diag_window_passes;
    xhci_rearm_devices();
    /* ITEM-479: after the pass has armed and drained, ask each role whether it
       has held transfers without a completion for longer than the bound. */
    xhci_watchdog_devices();
}

void xhci_keyboard_service(void) {
    /*
     * ITEM-452: the service pass must leave the caller's session where it found
     * it.  It is called from the ordinary-boot loop, but also from
     * xhci_candidate_prove() in the middle of a walk - and there the caller is
     * still working in the session it is visiting, whose rings, detail buffer
     * and header the next attach and the controller record read.
     */
    struct xhci_session *const caller = xhci;

    if (!xhci_machine.initialized) {
        return;
    }
    /*
     * ITEM-452: every live session has its own event ring and its own
     * counters, and the console needs all of them: the keyboard may be on one
     * controller and the pointer on another, each with a controller that stays
     * running.  The pass is round-robin from a rotating start, so neither
     * session can starve the other, and each session drains its own ring up to
     * the XHCI_SERVICE_EVENTS_MAX / XHCI_SERVICE_REPORTS_MAX budgets of
     * ITEM-458 before the next one is visited.
     */
    for (uint32_t step = 0U; step < XHCI_SESSION_SLOTS; ++step) {
        const uint32_t slot =
            (xhci_service_cursor + step) % XHCI_SESSION_SLOTS;
        struct xhci_session *session = &xhci_sessions[slot];

        if (!session->in_use) {
            continue;
        }
        if (!session->devices[XHCI_HID_ROLE_KEYBOARD].ready &&
            !session->devices[XHCI_HID_ROLE_MOUSE].ready) {
            continue;
        }
        xhci = session;
        xhci_service_session();
    }
    xhci_service_cursor = (xhci_service_cursor + 1U) % XHCI_SESSION_SLOTS;
    xhci = caller;
}

bool xhci_keyboard_pop_event(struct keyboard_event *event) {
    struct xhci_hid_device *device =
        xhci_role_device(XHCI_HID_ROLE_KEYBOARD);

    if (event == NULL || device->event_tail == device->event_head) {
        return false;
    }
    *event = device->events[device->event_tail];
    device->event_tail = (device->event_tail + 1U) % XHCI_LOCAL_EVENTS;
    return true;
}

/* ITEM-448: the pointer's queue, drained by the console's pointer path. */
bool xhci_mouse_pop_event(struct usb_hid_mouse_event *event) {
    struct xhci_hid_device *device = xhci_role_device(XHCI_HID_ROLE_MOUSE);

    if (event == NULL || device->mouse_event_tail == device->mouse_event_head) {
        return false;
    }
    *event = device->mouse_events[device->mouse_event_tail];
    device->mouse_event_tail =
        (device->mouse_event_tail + 1U) % XHCI_LOCAL_MOUSE_EVENTS;
    return true;
}

bool xhci_keyboard_attached(void) {
    return xhci_role_ready(XHCI_HID_ROLE_KEYBOARD) &&
           xhci_role_device(XHCI_HID_ROLE_KEYBOARD)->ready;
}

/* ITEM-448: a bound pointer, published beside the keyboard so a machine with
   only one of the two says which one it has. */
bool xhci_mouse_attached(void) {
    return xhci_role_ready(XHCI_HID_ROLE_MOUSE) &&
           xhci_role_device(XHCI_HID_ROLE_MOUSE)->ready;
}

enum xhci_status xhci_keyboard_status(void) {
    return xhci_machine.verdict_status;
}

/* ITEM-452: the machine-wide verdict - OK when a keyboard is bound anywhere,
   the most informative refusal otherwise - not the last visited controller's
   own record. */
const char *xhci_keyboard_detail(void) {
    return xhci_machine.verdict_detail != NULL ? xhci_machine.verdict_detail
                                                : "UNKNOWN";
}

const char *xhci_keyboard_transport_string(void) {
    return "USB_HID";
}

uint8_t xhci_keyboard_speed(void) {
    const struct xhci_hid_device *device =
        xhci_role_device(XHCI_HID_ROLE_KEYBOARD);

    return device->ready ? device->speed : 0U;
}

uint16_t xhci_keyboard_vendor_id(void) {
    const struct xhci_hid_device *device =
        xhci_role_device(XHCI_HID_ROLE_KEYBOARD);

    return device->ready ? device->vendor_id : 0U;
}

uint16_t xhci_keyboard_product_id(void) {
    const struct xhci_hid_device *device =
        xhci_role_device(XHCI_HID_ROLE_KEYBOARD);

    return device->ready ? device->product_id : 0U;
}

uint8_t xhci_keyboard_port(void) {
    const struct xhci_hid_device *device =
        xhci_role_device(XHCI_HID_ROLE_KEYBOARD);

    return device->ready ? device->port : 0U;
}

uint32_t xhci_keyboard_report_count(void) {
    return xhci_role_device(XHCI_HID_ROLE_KEYBOARD)->report_count;
}

uint64_t xhci_keyboard_completion_count(void) {
    return xhci_role_device(XHCI_HID_ROLE_KEYBOARD)->completion_count;
}

uint64_t xhci_keyboard_transfer_count(void) {
    return xhci_role_device(XHCI_HID_ROLE_KEYBOARD)->transfer_count;
}

uint32_t xhci_keyboard_error_count(void) {
    return xhci_role_device(XHCI_HID_ROLE_KEYBOARD)->error_count;
}

/*
 * ITEM-458: the two numbers behind "no character is lost".  `dropped` counts
 * the reports the bounded queue had to refuse, and `gap_ms_max` the longest
 * wait between two reports the service pass delivered; both are published on
 * the console's own `kbd` command and in the boot record.
 */
uint32_t xhci_keyboard_dropped_count(void) {
    return xhci_role_device(XHCI_HID_ROLE_KEYBOARD)->dropped_report_count;
}

uint32_t xhci_keyboard_gap_ms_max(void) {
    return xhci_role_device(XHCI_HID_ROLE_KEYBOARD)->service_gap_ms_max;
}

/* ITEM-482: the gap over the current window, and the window's close.  The
   once-a-second input record reads the first and then calls the second, so the
   published `kbd_gap_ms_max` is the last second's worst, not the boot's. */
uint32_t xhci_keyboard_gap_window_ms_max(void) {
    return xhci_role_device(XHCI_HID_ROLE_KEYBOARD)->service_gap_window_ms_max;
}

void xhci_keyboard_gap_window_reset(void) {
    xhci_role_device(XHCI_HID_ROLE_KEYBOARD)->service_gap_window_ms_max = 0U;
}

uint32_t xhci_keyboard_report_rate(void) {
    return xhci_role_device(XHCI_HID_ROLE_KEYBOARD)->rate_per_second;
}

/* ITEM-461: the longest wait between a completed report and the next interrupt
   transfer being armed, in milliseconds. */
uint32_t xhci_keyboard_repost_ms_max(void) {
    return xhci_role_device(XHCI_HID_ROLE_KEYBOARD)->repost_ms_max;
}

/* ITEM-467: whether the keyboard's one interrupt transfer is still owned by the
   controller, how many completions were followed by a re-arm, and how long ago
   the last completion was.  `pending=1 reports=0` is a transfer the controller
   never completed (the short-packet defect); `pending=0 reposted>0 reports=0`
   is a keyboard that answers without a report. */
bool xhci_keyboard_pending(void) {
    return xhci_role_device(XHCI_HID_ROLE_KEYBOARD)->transfer_pending;
}

uint32_t xhci_keyboard_repost_count(void) {
    return xhci_role_device(XHCI_HID_ROLE_KEYBOARD)->repost_count;
}

/* ITEM-479: the keyboard's watchdog findings. */
uint32_t xhci_keyboard_watchdog_count(void) {
    return xhci_role_device(XHCI_HID_ROLE_KEYBOARD)->watchdog_count;
}

uint32_t xhci_keyboard_watchdog_silence_ms_max(void) {
    return xhci_role_device(XHCI_HID_ROLE_KEYBOARD)->watchdog_silence_ms_max;
}

uint32_t xhci_keyboard_last_report_ms(void) {
    const struct xhci_hid_device *device =
        xhci_role_device(XHCI_HID_ROLE_KEYBOARD);

    return xhci_service_gap_ms(0U, device->last_report_ticks, timer_ticks(),
                               HFOC_TIMER_HZ);
}

/* ITEM-469: the queue depth the fix keeps, and the missed-service completions
   the single-TRB window produced.  Both are in the durable [PRODUCTION-INPUT]
   record so the QEMU proof can require `kbd_armed>=2` and `kbd_mse=0`. */
uint32_t xhci_keyboard_armed_count(void) {
    return xhci_role_device(XHCI_HID_ROLE_KEYBOARD)->armed_count;
}

uint32_t xhci_keyboard_mse_count(void) {
    return xhci_role_device(XHCI_HID_ROLE_KEYBOARD)->mse_count;
}

/* ITEM-470: the unarmed windows a completion left behind. */
uint32_t xhci_keyboard_no_trb_count(void) {
    return xhci_role_device(XHCI_HID_ROLE_KEYBOARD)->no_trb_count;
}

/*
 * ITEM-436: the binding and the traffic the boot record publishes.
 *
 * The binding readers answer with the descriptor values of the interface
 * that was selected and configured, and only while that keyboard is ready: a
 * machine whose keyboard was lost must not keep publishing the numbers of a
 * device that is gone.
 */
uint8_t xhci_keyboard_interface(void) {
    const struct xhci_hid_device *device =
        xhci_role_device(XHCI_HID_ROLE_KEYBOARD);

    return device->ready ? device->interface_number : 0U;
}

uint8_t xhci_keyboard_protocol(void) {
    const struct xhci_hid_device *device =
        xhci_role_device(XHCI_HID_ROLE_KEYBOARD);

    return device->ready ? device->interface_protocol : 0U;
}

uint8_t xhci_keyboard_endpoint_address(void) {
    const struct xhci_hid_device *device =
        xhci_role_device(XHCI_HID_ROLE_KEYBOARD);

    return device->ready ? device->endpoint_address : 0U;
}

/* ITEM-438: the DCI is the doorbell target and the endpoint-context index the
   reports arrive on; the record publishes it so the binding names the slot the
   controller was told about, not only the descriptor's endpoint address. */
uint8_t xhci_keyboard_dci(void) {
    const struct xhci_hid_device *device =
        xhci_role_device(XHCI_HID_ROLE_KEYBOARD);

    return device->ready ? device->dci : 0U;
}

uint8_t xhci_keyboard_endpoint_interval(void) {
    const struct xhci_hid_device *device =
        xhci_role_device(XHCI_HID_ROLE_KEYBOARD);

    return device->ready ? device->endpoint_interval : 0U;
}

/* ITEM-438: the descriptor's own bInterval, the number the interval above is
   the exponent of for a low- or full-speed endpoint. */
uint8_t xhci_keyboard_endpoint_binterval(void) {
    const struct xhci_hid_device *device =
        xhci_role_device(XHCI_HID_ROLE_KEYBOARD);

    return device->ready ? device->endpoint_binterval : 0U;
}

uint16_t xhci_keyboard_endpoint_max_packet(void) {
    const struct xhci_hid_device *device =
        xhci_role_device(XHCI_HID_ROLE_KEYBOARD);

    return device->ready ? device->endpoint_max_packet : 0U;
}

/* ITEM-438: the keys the whole reports carried, which is what separates "the
   device answered with an idle report" from "the device answered and typed". */
uint32_t xhci_keyboard_key_count(void) {
    return xhci_role_device(XHCI_HID_ROLE_KEYBOARD)->key_count;
}

/* ITEM-438: the report-protocol binding and the layout its reports are read
   with.  The readers answer zero while no such keyboard is bound, so a boot
   keyboard's record carries none of them. */
bool xhci_keyboard_report_mode(void) {
    const struct xhci_hid_device *device =
        xhci_role_device(XHCI_HID_ROLE_KEYBOARD);

    return device->ready && device->report_mode;
}

uint8_t xhci_keyboard_report_id(void) {
    const struct xhci_hid_device *device =
        xhci_role_device(XHCI_HID_ROLE_KEYBOARD);

    return device->ready && device->report_mode ? device->layout.report_id
                                                : 0U;
}

uint8_t xhci_keyboard_report_length(void) {
    const struct xhci_hid_device *device =
        xhci_role_device(XHCI_HID_ROLE_KEYBOARD);

    return device->ready && device->report_mode
               ? device->layout.report_length
               : 0U;
}

uint8_t xhci_keyboard_key_offset(void) {
    const struct xhci_hid_device *device =
        xhci_role_device(XHCI_HID_ROLE_KEYBOARD);

    return device->ready && device->report_mode ? device->layout.key_offset
                                                : 0U;
}

uint8_t xhci_keyboard_key_slots(void) {
    const struct xhci_hid_device *device =
        xhci_role_device(XHCI_HID_ROLE_KEYBOARD);

    return device->ready && device->report_mode ? device->layout.key_count
                                                : 0U;
}

bool xhci_keyboard_idle_refused(void) {
    const struct xhci_hid_device *device =
        xhci_role_device(XHCI_HID_ROLE_KEYBOARD);

    return device->ready && device->idle_refused;
}

/* ITEM-438: how the walk ended.  `bound` counts devices that parsed as a
   keyboard and were bound, `silent` those that then never answered inside their
   window, `proven` those that did; `retry` is 1 when the binding that stands is
   the first silent candidate re-bound after the whole machine was visited. */
uint32_t xhci_keyboard_candidates_bound(void) {
    return xhci_machine.candidates_bound;
}

uint32_t xhci_keyboard_candidates_silent(void) {
    return xhci_machine.candidates_silent;
}

uint32_t xhci_keyboard_candidates_proven(void) {
    return xhci_machine.candidates_proven;
}

bool xhci_keyboard_silent_retry(void) {
    return xhci_role_device(XHCI_HID_ROLE_KEYBOARD)->ready &&
           xhci_machine.silent_retry;
}

uint32_t xhci_keyboard_refused_count(void) {
    return xhci_role_device(XHCI_HID_ROLE_KEYBOARD)->refused_report_count;
}

uint32_t xhci_keyboard_last_report_bytes(void) {
    return xhci_role_device(XHCI_HID_ROLE_KEYBOARD)->last_report_bytes;
}

/*
 * ITEM-448: the pointer's own published facts.  They mirror the keyboard's
 * readers one for one - the same binding fields, the same traffic counters -
 * so the [MOUSE] record an operator reads carries the same evidence the
 * [KBD ] record carries, and a mouse that is bound but silent can be told
 * apart from one that was never found.
 */
uint8_t xhci_mouse_speed(void) {
    const struct xhci_hid_device *device = xhci_role_device(XHCI_HID_ROLE_MOUSE);

    return device->ready ? device->speed : 0U;
}

uint8_t xhci_mouse_port(void) {
    const struct xhci_hid_device *device = xhci_role_device(XHCI_HID_ROLE_MOUSE);

    return device->ready ? device->port : 0U;
}

uint16_t xhci_mouse_vendor_id(void) {
    const struct xhci_hid_device *device = xhci_role_device(XHCI_HID_ROLE_MOUSE);

    return device->ready ? device->vendor_id : 0U;
}

uint16_t xhci_mouse_product_id(void) {
    const struct xhci_hid_device *device = xhci_role_device(XHCI_HID_ROLE_MOUSE);

    return device->ready ? device->product_id : 0U;
}

uint8_t xhci_mouse_interface(void) {
    const struct xhci_hid_device *device = xhci_role_device(XHCI_HID_ROLE_MOUSE);

    return device->ready ? device->interface_number : 0U;
}

uint8_t xhci_mouse_protocol(void) {
    const struct xhci_hid_device *device = xhci_role_device(XHCI_HID_ROLE_MOUSE);

    return device->ready ? device->interface_protocol : 0U;
}

uint8_t xhci_mouse_endpoint_address(void) {
    const struct xhci_hid_device *device = xhci_role_device(XHCI_HID_ROLE_MOUSE);

    return device->ready ? device->endpoint_address : 0U;
}

uint8_t xhci_mouse_dci(void) {
    const struct xhci_hid_device *device = xhci_role_device(XHCI_HID_ROLE_MOUSE);

    return device->ready ? device->dci : 0U;
}

uint8_t xhci_mouse_endpoint_interval(void) {
    const struct xhci_hid_device *device = xhci_role_device(XHCI_HID_ROLE_MOUSE);

    return device->ready ? device->endpoint_interval : 0U;
}

uint8_t xhci_mouse_endpoint_binterval(void) {
    const struct xhci_hid_device *device = xhci_role_device(XHCI_HID_ROLE_MOUSE);

    return device->ready ? device->endpoint_binterval : 0U;
}

uint16_t xhci_mouse_endpoint_max_packet(void) {
    const struct xhci_hid_device *device = xhci_role_device(XHCI_HID_ROLE_MOUSE);

    return device->ready ? device->endpoint_max_packet : 0U;
}

bool xhci_mouse_report_mode(void) {
    const struct xhci_hid_device *device = xhci_role_device(XHCI_HID_ROLE_MOUSE);

    return device->ready && device->report_mode;
}

uint32_t xhci_mouse_report_count(void) {
    return xhci_role_device(XHCI_HID_ROLE_MOUSE)->report_count;
}

uint32_t xhci_mouse_transfer_count(void) {
    return xhci_role_device(XHCI_HID_ROLE_MOUSE)->transfer_count;
}

uint32_t xhci_mouse_completion_count(void) {
    return xhci_role_device(XHCI_HID_ROLE_MOUSE)->completion_count;
}

uint32_t xhci_mouse_wheel_count(void) {
    return xhci_role_device(XHCI_HID_ROLE_MOUSE)->mouse.wheels;
}

uint32_t xhci_mouse_endpoint_restarts(void) {
    return xhci_role_device(XHCI_HID_ROLE_MOUSE)->endpoint_restarts;
}

uint32_t xhci_mouse_error_count(void) {
    return xhci_role_device(XHCI_HID_ROLE_MOUSE)->error_count;
}

uint32_t xhci_mouse_refused_count(void) {
    return xhci_role_device(XHCI_HID_ROLE_MOUSE)->refused_report_count;
}

uint32_t xhci_mouse_last_report_bytes(void) {
    return xhci_role_device(XHCI_HID_ROLE_MOUSE)->last_report_bytes;
}

/* ITEM-467: the pointer's share of the same evidence: whether its interrupt
   transfer is still in flight, how many completions were re-armed, how long
   ago the device last answered, and why it has delivered no report. */
bool xhci_mouse_pending(void) {
    return xhci_role_device(XHCI_HID_ROLE_MOUSE)->transfer_pending;
}

uint32_t xhci_mouse_repost_count(void) {
    return xhci_role_device(XHCI_HID_ROLE_MOUSE)->repost_count;
}

uint32_t xhci_mouse_last_report_ms(void) {
    const struct xhci_hid_device *device =
        xhci_role_device(XHCI_HID_ROLE_MOUSE);

    return xhci_service_gap_ms(0U, device->last_report_ticks, timer_ticks(),
                               HFOC_TIMER_HZ);
}

const char *xhci_mouse_no_report_reason(void) {
    const struct xhci_hid_device *device =
        xhci_role_device(XHCI_HID_ROLE_MOUSE);

    return xhci_no_report_reason(device->transfer_pending, device->report_count,
                                 device->transfer_count);
}

/* ITEM-469: the pointer's share of the queue depth and missed-service count. */
uint32_t xhci_mouse_armed_count(void) {
    return xhci_role_device(XHCI_HID_ROLE_MOUSE)->armed_count;
}

uint32_t xhci_mouse_mse_count(void) {
    return xhci_role_device(XHCI_HID_ROLE_MOUSE)->mse_count;
}

/* ITEM-470: the pointer's unarmed windows, the loss `mse` cannot see. */
uint32_t xhci_mouse_no_trb_count(void) {
    return xhci_role_device(XHCI_HID_ROLE_MOUSE)->no_trb_count;
}

/* ITEM-479: the pointer's watchdog findings. */
uint32_t xhci_mouse_watchdog_count(void) {
    return xhci_role_device(XHCI_HID_ROLE_MOUSE)->watchdog_count;
}

/*
 * ITEM-486 (round 24): the stalled-endpoint watchdog gave up for the current
 * silence.  The report flag is set by `xhci_watchdog_device()` and cleared only
 * when the role leaves READY or drops all armed transfers, so once it is set
 * while the endpoint stays armed the watchdog will not publish or recover again
 * - the state the `hw11` run ended in.  Naming it is the `WATCHDOG_GAVE_UP`
 * outcome the mouse event-path classification carries.
 */
bool xhci_mouse_watchdog_gave_up(void) {
    const struct xhci_hid_device *device =
        xhci_role_device(XHCI_HID_ROLE_MOUSE);

    return device->ready && device->armed_count != 0U &&
           device->watchdog_reported;
}

uint32_t xhci_mouse_watchdog_silence_ms_max(void) {
    return xhci_role_device(XHCI_HID_ROLE_MOUSE)->watchdog_silence_ms_max;
}

/* ITEM-482: whether the pointer's silences happened while it was in use, how
   many wanted the stronger reset, the worst re-arm cost, and its report rate. */
uint32_t xhci_mouse_watchdog_active_count(void) {
    return xhci_role_device(XHCI_HID_ROLE_MOUSE)->watchdog_active_count;
}

uint32_t xhci_mouse_watchdog_escalations(void) {
    return xhci_role_device(XHCI_HID_ROLE_MOUSE)->watchdog_escalations;
}

uint32_t xhci_mouse_watchdog_recover_us_max(void) {
    return xhci_role_device(XHCI_HID_ROLE_MOUSE)->watchdog_recover_us_max;
}

uint32_t xhci_mouse_report_rate(void) {
    return xhci_role_device(XHCI_HID_ROLE_MOUSE)->rate_per_second;
}

/* The pointer counters the decoder owns: whole reports, movements, button
   edges and refused reports. */
uint32_t xhci_mouse_move_count(void) {
    return usb_hid_mouse_state_moves(&xhci_role_device(XHCI_HID_ROLE_MOUSE)->mouse);
}

uint32_t xhci_mouse_press_count(void) {
    return usb_hid_mouse_state_presses(
        &xhci_role_device(XHCI_HID_ROLE_MOUSE)->mouse);
}

uint32_t xhci_mouse_release_count(void) {
    return usb_hid_mouse_state_releases(
        &xhci_role_device(XHCI_HID_ROLE_MOUSE)->mouse);
}

/* The mouse role's verdict when no pointer is bound: the interface walk's own
   reason (`NO_HID_MOUSE` when no interface declared one) and the port/stage
   detail behind it, next to the identity of the device that was refused. */
const char *xhci_mouse_status_string(void) {
    return usb_hid_status_string(xhci_machine.mouse_status);
}

const char *xhci_mouse_detail(void) {
    return xhci_machine.mouse_detail[0] != '\0' ? xhci_machine.mouse_detail
                                                  : "NONE";
}

bool xhci_mouse_device_seen(void) {
    return xhci_machine.mouse_device_seen;
}

const char *xhci_mouse_interface_summary(void) {
    return xhci_machine.mouse_interface_summary[0] != '\0'
               ? xhci_machine.mouse_interface_summary
               : "NONE";
}

uint16_t xhci_mouse_refused_vendor_id(void) {
    return xhci_machine.mouse_vendor_id;
}

uint16_t xhci_mouse_refused_product_id(void) {
    return xhci_machine.mouse_product_id;
}

/* ITEM-448: a mouse was bound on a controller the walk had to leave for a
   keyboard somewhere else, and the machine ended without a pointer.  The
   record names the controller, the port and the speed, so the next run reads
   that the pointer exists and why it is not bound - not that it is absent. */
bool xhci_mouse_seen(void) {
    return xhci_machine.mouse_seen;
}

uint8_t xhci_mouse_seen_port(void) {
    return xhci_machine.mouse_seen_port;
}

uint8_t xhci_mouse_seen_speed(void) {
    return xhci_machine.mouse_seen_speed;
}

const char *xhci_mouse_seen_controller(void) {
    return xhci_machine.mouse_seen_controller[0] != '\0'
               ? xhci_machine.mouse_seen_controller
               : "NONE";
}

uint32_t xhci_mouse_candidates_bound(void) {
    return xhci_machine.mouse_candidates_bound;
}

uint32_t xhci_mouse_candidates_released(void) {
    return xhci_machine.mouse_candidates_released;
}

/*
 * ITEM-519 increment 4: the block_device of the bound USB stick, or NULL when
 * there is no stick, its transport never bound, or the adapter refused the
 * view.  This is the handle the journal of ITEM-521 writes through: the caller
 * gets a device the block layer validated and never reaches into the driver's
 * rings.  The pointer stays valid for the boot because the session that owns
 * the backend is held by the walk.
 */
bool xhci_storage_fault_text(char *buffer, size_t size) {
    struct xhci_storage_device *storage;

    if (xhci_machine.storage_session == NULL) {
        return false;
    }
    storage = &xhci_machine.storage_session->storage;
    return usb_storage_fault_text(usb_storage_last_fault(&storage->usb), buffer,
                                  size);
}

struct block_device *xhci_storage_block_device(void) {
    struct xhci_storage_device *storage;

    if (xhci_machine.storage_session == NULL) {
        return NULL;
    }
    storage = &xhci_machine.storage_session->storage;
    if (!storage->bound || !storage->block_bound) {
        return NULL;
    }
    return &storage->block.device;
}

/*
 * ITEM-452: the controller facts name the controller the keyboard is bound to
 * when there is one - the machine's local input comes from there - and the
 * most recently visited controller otherwise.  A machine with both roles live
 * reports the keyboard's controller, which is the one the console records are
 * about.
 */
static const struct xhci_session *xhci_reported_session(void) {
    if (xhci_machine.keyboard_session != NULL) {
        return xhci_machine.keyboard_session;
    }
    if (xhci_machine.mouse_session != NULL) {
        return xhci_machine.mouse_session;
    }
    return xhci;
}

const char *xhci_controller_state_string(void) {
    const char *state = xhci_reported_session()->state;

    return state != NULL ? state : "UNAVAILABLE";
}

uint32_t xhci_controller_max_ports(void) {
    return xhci_reported_session()->max_ports;
}

uint32_t xhci_controller_max_slots(void) {
    return xhci_reported_session()->max_slots;
}

uint64_t xhci_controller_bar(void) {
    return xhci_reported_session()->bar_physical;
}

uint16_t xhci_controller_vendor_id(void) {
    return xhci_reported_session()->pci_vendor;
}

uint16_t xhci_controller_device_id(void) {
    return xhci_reported_session()->pci_device;
}

/* ITEM-473 (СРОЧНО-23): the controller-level Event Ring Full (21) count of the
   reported session, and every Host Controller Event it carried.  The input
   record publishes `ringfull=` from these, so the QEMU proof can require zero
   at the sixteen-deep mouse queue and a hardware run reads the reason the
   events stopped. */
uint32_t xhci_event_ring_full_count(void) {
    return xhci_reported_session()->event_ring_full_events;
}

uint32_t xhci_controller_event_count(void) {
    return xhci_reported_session()->controller_event_count;
}

/*
 * ITEM-468 (СРОЧНО-18): the input path's periodic self-report and the two
 * console commands that read it on demand.
 *
 * This is measurement, not a fix.  The owner's 2026-09-21 hardware run left two
 * questions the log could not answer: the pointer stayed
 * `mouse_reason=INTERRUPT_PENDING` with `mouse_reports=0`, and the keyboard
 * arrived late, and both the input record (written only "on change") and the
 * controller's `usbcmd`/`usbsts` (written only when the controller was bound)
 * were one snapshot taken at tick 10.  Nothing on the machine said whether the
 * controller was still running, whether its event ring had moved, whether the
 * interrupt transfer was armed with ISP, or how long the loop took between two
 * input passes.
 *
 * At most once a second the ordinary-boot loop calls
 * `xhci_service_diagnostics()`, which publishes, through the durable-journal
 * path of ITEM-461 (`console_publish_journal()`, no paint, no serial):
 *
 *   * one `[XHCI-DIAG]` record per live controller: the live USB command and
 *     status registers, the command-ring and event-ring pointers, whether the
 *     interrupter is enabled, and running/halted/error as the part reports
 *     them;
 *   * one `[INPUT-DIAG]` record per HID role: slot, endpoint, DCI, binding
 *     state, the in-flight transfer and its re-arm count, completions, reports,
 *     errors, the ISP flag the armed TRB carries, the short-packet completions,
 *     the age of the last completion and the one-word reason it has delivered
 *     no report;
 *   * one `[LOOP-DIAG]` record for the window: the events and whole reports the
 *     service pass applied, the turns it took, the loop's own cost and the wire
 *     frames of the same window.
 *
 * Every line is bounded by XHCI_DIAG_LINE_MAX, which ITEM-481 raises to the
 * worst case of the longest shape (`[INPUT-DIAG]`: fourteen decimal counters at
 * twenty digits plus its literals and reason) so the local buffer cannot drop a
 * field before the console's bounded journal funnel sees it; the funnel splits
 * a line past 320 into `[<name>-TAIL]` parts.  The `XHCI` and `INPUT` console
 * commands print the same facts on demand; the console transcript of ITEM-460
 * puts their answer in the durable journal too.
 */
#define XHCI_DIAG_LINE_MAX 512U
/* USBSTS.HSE (bit 2) and USBSTS.HCE (bit 12): the two error latches. */

struct xhci_diag_line {
    char text[XHCI_DIAG_LINE_MAX];
    size_t length;
    /* ITEM-481: set when a field did not fit; never a silent drop. */
    bool overflow;
};

static void xhci_diag_begin(struct xhci_diag_line *line) {
    line->length = 0U;
    line->text[0] = '\0';
    line->overflow = false;
}

static void xhci_diag_putc(struct xhci_diag_line *line, char value) {
    if (line->length + 1U < sizeof(line->text)) {
        line->text[line->length++] = value;
        line->text[line->length] = '\0';
        return;
    }
    line->overflow = true;
}

static void xhci_diag_text(struct xhci_diag_line *line, const char *text) {
    if (text == NULL) {
        return;
    }
    while (*text != '\0') {
        xhci_diag_putc(line, *text++);
    }
}

static void xhci_diag_u64(struct xhci_diag_line *line, uint64_t value) {
    char digits[20U];
    size_t count = 0U;

    if (value == 0U) {
        xhci_diag_putc(line, '0');
        return;
    }
    while (value != 0U) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count > 0U) {
        xhci_diag_putc(line, digits[--count]);
    }
}

static void xhci_diag_hex64(struct xhci_diag_line *line, uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    int shift;

    xhci_diag_text(line, "0x");
    for (shift = 60; shift >= 0; shift -= 4) {
        xhci_diag_putc(line, digits[(value >> (unsigned int)shift) & 0x0FU]);
    }
}

static void xhci_diag_hex2(struct xhci_diag_line *line, uint8_t value) {
    static const char digits[] = "0123456789ABCDEF";

    xhci_diag_putc(line, digits[(value >> 4) & 0x0FU]);
    xhci_diag_putc(line, digits[value & 0x0FU]);
}

static void xhci_diag_pci(struct xhci_diag_line *line,
                          const struct pci_location *location) {
    xhci_diag_hex2(line, location->bus);
    xhci_diag_putc(line, ':');
    xhci_diag_hex2(line, location->device);
    xhci_diag_putc(line, '.');
    xhci_diag_hex2(line, location->function);
}

static uint64_t xhci_diag_read64(volatile uint8_t *base) {
    return (uint64_t)xhci_read32(base, 0U) |
           ((uint64_t)xhci_read32(base, 4U) << 32);
}

static void xhci_diag_controller_line(struct xhci_diag_line *line,
                                      const struct xhci_session *session) {
    uint32_t usbcmd = 0U;
    uint32_t usbsts = 0U;
    uint32_t iman = 0U;
    uint64_t crcr = 0U;
    uint64_t erdp = 0U;
    uint64_t erstba = 0U;

    if (session->regs != NULL) {
        usbcmd = xhci_read32(xhci_operational(XHCI_OP_USBCMD), 0U);
        usbsts = xhci_read32(xhci_operational(XHCI_OP_USBSTS), 0U);
        crcr = xhci_diag_read64(xhci_operational(XHCI_OP_CRCR));
        erdp = xhci_diag_read64(
            xhci_runtime(XHCI_RT_IR0 + XHCI_IR_ERDP));
        erstba = xhci_diag_read64(
            xhci_runtime(XHCI_RT_IR0 + XHCI_IR_ERSTBA));
        iman = xhci_read32(xhci_runtime(XHCI_RT_IR0 + XHCI_IR_IMAN), 0U);
    }
    xhci_diag_begin(line);
    xhci_diag_text(line, "[XHCI-DIAG] pci=");
    xhci_diag_pci(line, &session->location);
    xhci_diag_text(line, " caplength=0x");
    xhci_diag_hex2(line, session->caplength);
    xhci_diag_text(line, " mmio_bytes=");
    xhci_diag_u64(line, session->mmio_bytes);
    xhci_diag_text(line, " usbcmd=");
    xhci_diag_hex64(line, usbcmd);
    xhci_diag_text(line, " usbsts=");
    xhci_diag_hex64(line, usbsts);
    xhci_diag_text(line, " crcr=");
    xhci_diag_hex64(line, crcr);
    xhci_diag_text(line, " erdp=");
    xhci_diag_hex64(line, erdp);
    xhci_diag_text(line, " erstba=");
    xhci_diag_hex64(line, erstba);
    xhci_diag_text(line, " interrupter_enabled=");
    xhci_diag_u64(line, (iman & XHCI_DIAG_IMAN_IE) != 0U ? 1U : 0U);
    xhci_diag_text(line, " running=");
    xhci_diag_u64(line, (usbcmd & XHCI_USBCMD_RS) != 0U ? 1U : 0U);
    xhci_diag_text(line, " halted=");
    xhci_diag_u64(line, (usbsts & XHCI_USBSTS_HCH) != 0U ? 1U : 0U);
    xhci_diag_text(line, " error=");
    xhci_diag_hex64(line, usbsts & XHCI_DIAG_USBSTS_ERROR);
    /*
     * ITEM-473 (СРОЧНО-23): the controller-level events.  `ringfull=` is Event
     * Ring Full (21) - the error that stops the controller publishing events
     * until the dequeue pointer moves; before this item the driver could not
     * decode the Host Controller Event TRB that carries it, so a session that
     * died this way published no reason at all.
     */
    xhci_diag_text(line, " host_events=");
    xhci_diag_u64(line, session->controller_event_count);
    xhci_diag_text(line, " ringfull=");
    xhci_diag_u64(line, session->event_ring_full_events);
}

static void xhci_diag_role_line(struct xhci_diag_line *line,
                                const char *name,
                                uint8_t role) {
    const struct xhci_hid_device *device =
        xhci_device(role < XHCI_HID_DEVICES ? role : XHCI_HID_ROLE_KEYBOARD);

    xhci_diag_begin(line);
    xhci_diag_text(line, "[INPUT-DIAG] role=");
    xhci_diag_text(line, name);
    xhci_diag_text(line, " ready=");
    xhci_diag_u64(line, device->ready ? 1U : 0U);
    xhci_diag_text(line, " slot=");
    xhci_diag_u64(line, device->slot_id);
    xhci_diag_text(line, " ep=0x");
    xhci_diag_hex2(line, device->endpoint_address);
    xhci_diag_text(line, " dci=");
    xhci_diag_u64(line, device->dci);
    xhci_diag_text(line, " state=");
    xhci_diag_text(line, device->ready ? "READY" : "DOWN");
    xhci_diag_text(line, " pending=");
    xhci_diag_u64(line, device->transfer_pending ? 1U : 0U);
    xhci_diag_text(line, " reposted=");
    xhci_diag_u64(line, device->repost_count);
    /*
     * ITEM-580: the two counters the old line conflated under `completed=`.
     * `submitted` is every interrupt TRB this role armed; `completions` is
     * every transfer event the controller answered.  A pointer that armed 17
     * and completed 3 is a different machine from one that armed 17 and
     * completed 17 with no report decoded, and the exported records have to
     * say which.
     */
    xhci_diag_text(line, " submitted=");
    xhci_diag_u64(line, device->transfer_count);
    xhci_diag_text(line, " completions=");
    xhci_diag_u64(line, device->completion_count);
    xhci_diag_text(line, " reports=");
    xhci_diag_u64(line, device->report_count);
    xhci_diag_text(line, " errors=");
    xhci_diag_u64(line, device->error_count);
    xhci_diag_text(line, " isp=");
    xhci_diag_u64(line, device->last_transfer_isp ? 1U : 0U);
    xhci_diag_text(line, " short=");
    xhci_diag_u64(line, device->short_packet_count);
    /* ITEM-469: the depth the fix keeps, and the missed-service completions
       the single-TRB window used to cause. */
    xhci_diag_text(line, " armed=");
    xhci_diag_u64(line, device->armed_count);
    xhci_diag_text(line, " mse=");
    xhci_diag_u64(line, device->mse_count);
    /* ITEM-470: how many completions left this role with no armed TRB at all -
       the device's silent NAK loss, which `mse` is the controller's own state
       and cannot report. */
    xhci_diag_text(line, " no_trb=");
    xhci_diag_u64(line, device->no_trb_count);
    xhci_diag_text(line, " last_completion_ms=");
    xhci_diag_u64(line, xhci_service_gap_ms(0U, device->last_report_ticks,
                                             timer_ticks(), HFOC_TIMER_HZ));
    xhci_diag_text(line, " reason=");
    xhci_diag_text(line, xhci_no_report_reason(device->transfer_pending,
                                               device->report_count,
                                               device->transfer_count));
}

/*
 * ITEM-473 (СРОЧНО-23): the per-role completion-code record.
 *
 * The round-11 journal could not say why the pointer went silent: `mse=0`,
 * `no_trb=0`, `errors=0`, `state=READY`, controller `running=1` - every field
 * healthy, and nothing naming the controller event (Event Ring Full, 21) that
 * had actually stopped the events.  This short record publishes the controller
 * `ringfull=` count, the role's Endpoint State read from the device context
 * (so "the endpoint is Running" is a fact, not an inference) and the last
 * completion code beside the running count of every code the role has seen.
 * It is a separate line because a full code list would push the role's
 * `reason=` off the 320-byte record bound.
 */
static uint32_t xhci_device_endpoint_state(
    const struct xhci_hid_device *device) {
    if (device == NULL || !device->ready || device->slot_id == 0U ||
        xhci->device_contexts == NULL || xhci->context_bytes == 0U) {
        return 0U;
    }
    return xhci_endpoint_state(
        xhci->device_contexts +
            (size_t)(device->slot_id - 1U) * xhci->slot_area_bytes,
        xhci->context_bytes, device->dci);
}

static void xhci_diag_code_line(struct xhci_diag_line *line,
                                const char *name,
                                uint8_t role) {
    const struct xhci_hid_device *device =
        xhci_device(role < XHCI_HID_DEVICES ? role : XHCI_HID_ROLE_KEYBOARD);
    bool first = true;

    xhci_diag_begin(line);
    xhci_diag_text(line, "[INPUT-CODE] role=");
    xhci_diag_text(line, name);
    xhci_diag_text(line, " ringfull=");
    xhci_diag_u64(line, xhci->event_ring_full_events);
    xhci_diag_text(line, " ep_state=");
    xhci_diag_text(
        line, xhci_endpoint_state_string(xhci_device_endpoint_state(device)));
    xhci_diag_text(line, " last=");
    if (device->transfer_count == 0U) {
        xhci_diag_text(line, "NONE");
    } else {
        xhci_diag_text(
            line, xhci_completion_code_string(device->last_completion_code));
    }
    xhci_diag_text(line, " codes=");
    for (uint32_t code = 0U; code < XHCI_COMPLETION_CODE_COUNT; ++code) {
        if (device->completion_code_counts[code] == 0U) {
            continue;
        }
        if (!first) {
            xhci_diag_putc(line, ',');
        }
        first = false;
        xhci_diag_u64(line, code);
        xhci_diag_putc(line, ':');
        xhci_diag_u64(line, device->completion_code_counts[code]);
    }
    if (first) {
        xhci_diag_text(line, "none");
    }
}

static void xhci_diag_pass_line(struct xhci_diag_line *line,
                                uint64_t frames) {
    xhci_diag_begin(line);
    xhci_diag_text(line, "[LOOP-DIAG] events=");
    xhci_diag_u64(line, xhci_diag_window_events);
    xhci_diag_text(line, " reports=");
    xhci_diag_u64(line, xhci_diag_window_reports);
    xhci_diag_text(line, " passes=");
    xhci_diag_u64(line, xhci_diag_window_passes);
    xhci_diag_text(line, " iter_us=");
    xhci_diag_u64(line, kernel_loop_iter_us());
    xhci_diag_text(line, " iter_max_us=");
    xhci_diag_u64(line, kernel_loop_iter_max_us());
    /*
     * ITEM-470 (СРОЧНО-20): the explicit loop-turn counter next to `iter_us`,
     * so the next record settles whether `passes` (the service passes, two or
     * more per turn) or `iter_us` (the cycle-counter average) describes the
     * real turnaround instead of leaving an eight-fold gap unexplained.
     */
    xhci_diag_text(line, " loops=");
    xhci_diag_u64(line, kernel_loop_iteration_count());
    /*
     * ITEM-471 (СРОЧНО-21): the TSC rate the microsecond clock was calibrated
     * to.  With `iter_us`, `loops` and this number on one line the next run
     * settles whether `iter_us` is mis-scaled or the PIT tick is the clock
     * that lost real time, instead of comparing two averages across records.
     */
    xhci_diag_text(line, " cal_hz=");
    xhci_diag_u64(line, timer_cycles_per_second());
    /*
     * ITEM-472 (СРОЧНО-22): the two readings the item exists to separate.
     * `tick_hz=` is the IRQ tick counted against the PIT-calibrated clock, so a
     * value well under 100 is the tick counter losing real time; `second=` is
     * the same clock's wall-clock uptime.  With `cal_hz` now measured against
     * the PIT hardware, the three numbers agree only when the tick is honest.
     */
    xhci_diag_text(line, " tick_hz=");
    xhci_diag_u64(line, timer_measured_tick_hz());
    xhci_diag_text(line, " second=");
    xhci_diag_u64(line, timer_real_seconds());
    /* ITEM-469: `iter_us` is the window since this heartbeat (reset below), and
       `iter_max_us` is the worst turn since boot; naming the scopes stops a
       reader from taking the boot maximum for this second. */
    xhci_diag_text(line, " iter_window=1s iter_max_scope=boot");
    xhci_diag_text(line, " frames=");
    xhci_diag_u64(line, frames);
}

static void xhci_diag_publish(const struct xhci_diag_line *line) {
    static const char overflow_marker[] =
        "[XHCI-DIAG] overflow=1 truncated=1";

    console_publish_journal(line->text, line->length);
    /*
     * ITEM-481: XHCI_DIAG_LINE_MAX is the worst case of the longest shape, so
     * this is a defensive path - but if a future field ever fills the buffer,
     * the fact is published instead of being lost with the dropped bytes.
     */
    if (line->overflow) {
        console_publish_journal(overflow_marker,
                                sizeof(overflow_marker) - 1U);
    }
}

/*
 * ITEM-552: while the diagnostic export is being composed, the display path is
 * a sink instead of the screen; the formatting above is unchanged, so the
 * report and the screen cannot disagree about what the machine says.
 */
static xhci_report_sink_fn xhci_diag_sink;
static void *xhci_diag_sink_context;

/* ITEM-573: the evidence module's sink and the report's are the same shape;
   this adapter keeps the two headers independent of each other. */
struct xhci_evidence_bridge {
    xhci_report_sink_fn sink;
    void *context;
};

static void xhci_evidence_bridge_sink(void *context, const char *line) {
    struct xhci_evidence_bridge *bridge = context;

    if (bridge != NULL && bridge->sink != NULL) {
        bridge->sink(bridge->context, line);
    }
}

static void xhci_diag_display(const struct xhci_diag_line *line) {
    if (xhci_diag_sink != NULL) {
        xhci_diag_sink(xhci_diag_sink_context, line->text);
        return;
    }
    console_write(line->text);
    console_putc('\n');
}

void xhci_diagnostics_report(xhci_report_sink_fn sink, void *context) {
    const xhci_report_sink_fn previous = xhci_diag_sink;
    void *const previous_context = xhci_diag_sink_context;

    if (sink == NULL) {
        return;
    }
    /*
     * ITEM-580: one snapshot of the live machine for the whole attempt, taken
     * before the first record is composed.  `[INPUT-DIAG]` reads the live
     * structures and the two evidence families read the refreshed store, so all
     * three describe the same moment; the refresh itself only copies values.
     */
    xhci_evidence_refresh_live();
    xhci_diag_sink = sink;
    xhci_diag_sink_context = context;
    xhci_diagnostics_print_xhci();
    xhci_diagnostics_print_input();
    /* ITEM-573: every surveyed controller, from RAM - the sessions may all be
       released, stopped and reused by the time `diag save` runs, and these
       records must not need one. */
    {
        struct xhci_evidence_bridge bridge;

        bridge.sink = sink;
        bridge.context = context;
        xhci_evidence_report(xhci_evidence_bridge_sink, &bridge);
    }
    xhci_diag_sink = previous;
    xhci_diag_sink_context = previous_context;
}

/*
 * ITEM-497: the boot's input verdict - the record that has to survive the boot
 * after it.
 *
 * The owner's 2026-09-30 run came up once with no keyboard and no pointer and
 * came up working the next time, and the durable log could not say why: the
 * boot that failed had been overwritten.  This publishes, once a second and
 * only when it changes, the three facts a reader needs from the failed boot -
 * was a HID device bound at all (`keyboard=`/`mouse=`), was its endpoint armed
 * (`ARMED`/`NO_REPORTS`/`NOT_ARMED`/`ABSENT`), and how long after boot that
 * happened (`arm_ms`, `first_report_ms`).  The store renders and numbers it,
 * writes it as a flush marker, and re-states it with the two boots before it.
 */
static uint64_t xhci_input_arm_ms;
static uint64_t xhci_input_first_report_ms;

static const char *xhci_input_role_state(uint8_t role) {
    const struct xhci_hid_device *device = xhci_role_device(role);

    if (!device->ready) {
        return device->slot_id != 0U ? "NOT_ARMED" : "ABSENT";
    }
    return device->report_count != 0U ? "ARMED" : "NO_REPORTS";
}

static void xhci_input_verdict_publish(uint64_t now) {
    const struct xhci_hid_device *const keyboard =
        xhci_role_device(XHCI_HID_ROLE_KEYBOARD);
    const struct xhci_hid_device *const mouse =
        xhci_role_device(XHCI_HID_ROLE_MOUSE);
    const uint64_t reports =
        (uint64_t)keyboard->report_count + (uint64_t)mouse->report_count;
    const uint64_t now_ms = now * 1000U / HFOC_TIMER_HZ;
    uint64_t ports = 0U;

    if ((keyboard->ready || mouse->ready) && xhci_input_arm_ms == 0U) {
        xhci_input_arm_ms = now_ms;
    }
    if (reports != 0U && xhci_input_first_report_ms == 0U) {
        xhci_input_first_report_ms = now_ms;
    }
    /* The controllers that actually bound a HID device, not every controller
     * the machine has: a verdict of "two ports" has to mean two devices. */
    for (uint32_t slot = 0U; slot < XHCI_SESSION_SLOTS; ++slot) {
        const struct xhci_session *const session = &xhci_sessions[slot];

        if (!session->in_use) {
            continue;
        }
        if (session->devices[XHCI_HID_ROLE_KEYBOARD].slot_id != 0U ||
            session->devices[XHCI_HID_ROLE_MOUSE].slot_id != 0U) {
            ++ports;
        }
    }
    appliance_boot_log_store_input_state(
        xhci_input_role_state(XHCI_HID_ROLE_KEYBOARD),
        xhci_input_role_state(XHCI_HID_ROLE_MOUSE), xhci_input_arm_ms,
        xhci_input_first_report_ms, ports, reports);
    /* The carried verdicts are restated on their own window, not only when the
     * loop heartbeat happens to be published. */
    appliance_boot_log_store_input_verdict_tick();
}

void xhci_input_verdict_publish_now(void) {
    if (!xhci_machine.initialized) {
        return;
    }
    xhci_input_verdict_publish(timer_ticks());
}

void xhci_service_diagnostics(uint64_t frames) {
    struct xhci_session *const caller = xhci;
    const uint64_t now = timer_ticks();

    if (!xhci_machine.initialized) {
        return;
    }
    if (xhci_diag_last_ticks != 0U &&
        now - xhci_diag_last_ticks < HFOC_TIMER_HZ) {
        return;
    }
    xhci_diag_last_ticks = now;
    ++xhci_ep_state_publications;
    xhci_input_verdict_publish(now);
    for (uint32_t slot = 0U; slot < XHCI_SESSION_SLOTS; ++slot) {
        struct xhci_session *session = &xhci_sessions[slot];
        struct xhci_diag_line line;

        if (!session->in_use) {
            continue;
        }
        xhci = session;
        xhci_diag_controller_line(&line, session);
        xhci_diag_publish(&line);
        xhci_diag_role_line(&line, "keyboard", XHCI_HID_ROLE_KEYBOARD);
        xhci_diag_publish(&line);
        xhci_diag_code_line(&line, "keyboard", XHCI_HID_ROLE_KEYBOARD);
        xhci_diag_publish(&line);
        xhci_diag_role_line(&line, "mouse", XHCI_HID_ROLE_MOUSE);
        xhci_diag_publish(&line);
        xhci_diag_code_line(&line, "mouse", XHCI_HID_ROLE_MOUSE);
        xhci_diag_publish(&line);
        /*
         * MOUSE-DIAG step 3: one endpoint/ring pair every fifth diagnostic
         * publication, so normal operation carries it without flooding the
         * journal the ring already drops records from.
         */
        if (xhci_ep_state_publications % 5U == 0U) {
            xhci_ep_state_publish(XHCI_HID_ROLE_KEYBOARD);
            xhci_ep_state_publish(XHCI_HID_ROLE_MOUSE);
        }
    }
    xhci = caller;
    {
        struct xhci_diag_line line;

        xhci_diag_pass_line(&line, frames);
        xhci_diag_publish(&line);
    }
    xhci_diag_window_events = 0U;
    xhci_diag_window_reports = 0U;
    xhci_diag_window_passes = 0U;
    /*
     * ITEM-469: this heartbeat is the loop's once-a-second publication, so the
     * iteration window ends here.  The window used to be cleared only by the
     * ten-second [PRODUCTION-LOOP] record, which is why the owner's seven
     * second run read 83 ms as "the loop's turn" instead of "the loop's turn
     * since the last record that happened to clear it".
     */
    kernel_loop_iter_window_reset();
}

/*
 * ITEM-550: the kept command failure, printed on demand.
 *
 * The three records are built by the same formatters the capture published, so
 * the screen and the durable journal cannot diverge, and printing needs no
 * controller: every value was read at the failure point and kept as a value.
 */
static void xhci_diag_show_record(const char *record) {
    struct xhci_diag_line line;

    xhci_diag_begin(&line);
    xhci_diag_text(&line, record);
    xhci_diag_display(&line);
}

static void xhci_diag_command_lines(void) {
    char record[XHCI_COMMAND_RECORD_BYTES];
    size_t length;

    if (!xhci_machine.command.valid) {
        return;
    }
    length = xhci_format_command_record(record, sizeof(record),
                                        &xhci_machine.command);
    if (length > 0U) {
        xhci_diag_show_record(record);
    }
    length = xhci_format_command_register_record(record, sizeof(record),
                                                 &xhci_machine.command);
    if (length > 0U) {
        xhci_diag_show_record(record);
    }
    length = xhci_format_command_event_record(record, sizeof(record),
                                              &xhci_machine.command);
    if (length > 0U) {
        xhci_diag_show_record(record);
    }
}

void xhci_diagnostics_print_xhci(void) {
    struct xhci_session *const caller = xhci;
    bool any = false;

    for (uint32_t slot = 0U; slot < XHCI_SESSION_SLOTS; ++slot) {
        struct xhci_session *session = &xhci_sessions[slot];
        struct xhci_diag_line line;

        if (!session->in_use) {
            continue;
        }
        any = true;
        xhci = session;
        xhci_diag_controller_line(&line, session);
        xhci_diag_display(&line);
    }
    xhci = caller;
    if (!any) {
        if (xhci_machine.last_probe.valid) {
            struct xhci_diag_line line;

            xhci_diag_begin(&line);
            xhci_diag_text(&line, "[XHCI-DIAG] UNAVAILABLE last_probe=1 pci=");
            xhci_diag_pci(&line, &xhci_machine.last_probe.location);
            xhci_diag_text(&line, " caplength=0x");
            xhci_diag_hex2(&line, xhci_machine.last_probe.caplength);
            xhci_diag_text(&line, " mmio_bytes=");
            xhci_diag_u64(&line, xhci_machine.last_probe.mmio_bytes);
            xhci_diag_text(&line, " reason=");
            xhci_diag_text(&line,
                           xhci_status_string(xhci_machine.last_probe.status));
            xhci_diag_text(&line, " detail=");
            xhci_diag_text(&line, xhci_machine.last_probe.detail);
            xhci_diag_display(&line);
        } else {
            struct xhci_diag_line line;

            xhci_diag_begin(&line);
            xhci_diag_text(&line, "[XHCI-DIAG] UNAVAILABLE");
            xhci_diag_display(&line);
        }
    }
    /* ITEM-550: the last command that never completed is printed whether or not
       a session survived, because that failure is usually why none did. */
    xhci_diag_command_lines();
}

void xhci_diagnostics_print_input(void) {
    struct xhci_session *const caller = xhci;

    for (uint32_t slot = 0U; slot < XHCI_SESSION_SLOTS; ++slot) {
        struct xhci_session *session = &xhci_sessions[slot];
        struct xhci_diag_line line;

        if (!session->in_use) {
            continue;
        }
        xhci = session;
        xhci_diag_role_line(&line, "keyboard", XHCI_HID_ROLE_KEYBOARD);
        xhci_diag_display(&line);
        xhci_diag_code_line(&line, "keyboard", XHCI_HID_ROLE_KEYBOARD);
        xhci_diag_display(&line);
        xhci_diag_role_line(&line, "mouse", XHCI_HID_ROLE_MOUSE);
        xhci_diag_display(&line);
        xhci_diag_code_line(&line, "mouse", XHCI_HID_ROLE_MOUSE);
        xhci_diag_display(&line);
    }
    xhci = caller;
    {
        struct xhci_diag_line line;

        xhci_diag_pass_line(&line, 0U);
        xhci_diag_display(&line);
    }
}
