#ifndef HFOC_XHCI_TRB_H
#define HFOC_XHCI_TRB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-395: the pure half of the xHCI driver.
 *
 * Everything in this module is arithmetic over caller-owned buffers: TRB
 * encoding, ring bookkeeping, event decoding, device-context setup, the
 * port-speed and port-interval tables, the writable subset of PORTSC and the
 * controller/port scan policy (ITEM-406). It performs no MMIO, no allocation
 * and no I/O, so the host proof drives the same code the kernel runs, and the
 * hardware-facing half (kernel/src/xhci.c) cannot encode a TRB, a context or a
 * scan decision differently from what the proof checks.
 *
 * References used for every layout below:
 *   * eXtensible Host Controller Interface for Universal Serial Bus (xHCI)
 *     specification, revision 1.1: section 4.11 (device context), 6.2.2
 *     (transfer request blocks), 6.4.3 (command TRBs), 6.4.2 (event TRBs) and
 *     tables 5-27 (port speed / PSI), 6-9 (completion codes).
 *   * The Linux xhci driver's register and TRB macros (drivers/usb/host/
 *     xhci.h, xhci-ring.c) were used as a cross-check for the bit positions
 *     named here, and the QEMU xhci model (hw/usb/hcd-xhci.c) as the
 *     behavioural reference for the polled bring-up sequence.
 */

#define XHCI_TRB_BYTES 16U
/* TRB count of every ring this driver builds, the trailing Link TRB included.
   Sixteen entries keep the two rings and their segment tables inside the fixed
   DMA pool, and one TRB in flight at a time never fills one. */
#define XHCI_RING_TRBS 16U
/*
 * ITEM-473 (СРОЧНО-23): the session event ring's capacity in TRBs.
 *
 * The driver used sixteen - exactly the mouse's armed-transfer depth - so a
 * burst of sixteen completions filled the ring, the controller raised Event
 * Ring Full (21) and, by the specification, stopped publishing events until
 * the software moved the dequeue pointer.  The round-11 hardware run showed
 * the result: `completed=40` and `mouse_reports=24` froze after the first
 * burst while `armed=16` stayed set and the controller was still running.
 *
 * The page between XHCI_OFF_EVENT_RING and XHCI_OFF_ERST is 4 KiB = 256 TRBs,
 * so the ring is its whole page and a burst of armed transfers can no longer
 * fill it.  The static assertions in kernel/src/xhci.c pin both the page fit
 * and that the armed depths stay below this capacity.
 */
#define XHCI_EVENT_TRBS 256U
/*
 * ITEM-438: the Interval field's legal range for a low- or full-speed
 * interrupt endpoint.  xHCI 1.2 section 6.2.3.6 gives such an endpoint the
 * period 2^Interval x 125 us, so the field is 3 (1 ms) to 10 (128 ms), and the
 * descriptor's bInterval in milliseconds has to be turned into that exponent
 * rather than into a microframe count.  The host proof pins the table.
 */
#define XHCI_INTERVAL_FS_MIN 3U
#define XHCI_INTERVAL_FS_MAX 10U
#define XHCI_MAX_PORTS 255U
/*
 * ITEM-407: the number of device slots a controller may advertise and the
 * number this driver is willing to enable are two different bounds, and they
 * used to be one constant.
 *
 * HCSPARAMS1.MaxSlots is an eight-bit field (xHCI 1.1 section 5.3.3, table
 * 5-5), so a conforming controller may advertise up to 255 slots - and the
 * acceptance host's AMD A520 xHCI (`1022:43ec`) advertises 127. The previous
 * single constant was both "the biggest controller this driver accepts" and
 * "the room in the device context table", so that controller was refused with
 * `BAD_HCSPARAMS` for being *larger* than our table: a refusal this driver
 * invented, not a hardware fault. The bounds are now separate:
 *
 *   * XHCI_MAX_SLOTS is the specification's field maximum. A controller that
 *     advertises at most this many slots is accepted whatever its size; only
 *     the controller's own MaxSlots == 0 makes its HCSPARAMS1 invalid.
 *   * XHCI_DRIVER_SLOTS is how many slots this driver enables at once. It
 *     bounds the structures whose size follows the number of slots *used* -
 *     the device context table, and the Device Context Base Address Array
 *     entries that must point into it - while the array itself is still sized
 *     for the controller's own MaxSlots+1 entries (xHCI 1.1 section 6.1).
 *
 * The driver enables one slot per device it addresses and addresses one device
 * at a time, so XHCI_DRIVER_SLOTS is headroom, not a working set: the
 * acceptance host needs 1 of the 64.
 */
#define XHCI_MAX_SLOTS 255U
#define XHCI_DRIVER_SLOTS 64U
#define XHCI_MAX_INTERRUPTERS 1024U

/* Operational, runtime and doorbell register offsets used by the driver and
   by the register-window proof. */
#define XHCI_CAP_CAPLENGTH 0x00U
#define XHCI_CAP_HCIVERSION 0x02U
#define XHCI_CAP_HCSPARAMS1 0x04U
#define XHCI_CAP_HCSPARAMS2 0x08U
#define XHCI_CAP_HCCPARAMS1 0x10U
#define XHCI_CAP_DBOFF 0x14U
#define XHCI_CAP_RTSOFF 0x18U

/*
 * ITEM-453: the extended capability list (xHCI 1.1 section 7.2).  HCCPARAMS1
 * bits 31:16 hold the list's offset in dwords from the capability registers;
 * each entry's first dword carries its ID (bits 7:0) and the offset of the
 * next entry in dwords (bits 15:8); a next pointer of zero ends the list.
 *
 * A Supported Protocol capability (ID 2) declares one protocol group: the
 * major/minor revision, a four-character name, the raw port range the group
 * owns, and the Protocol Speed IDs (PSIV) its ports report.  The owner's
 * built-in controllers declare two groups - a USB2 half and a USB3 half - and
 * "which raw port number is Linux's USB2 port 1" is exactly what the missing
 * port evidence could not answer.
 */
#define XHCI_EXT_CAP_ID_MASK 0x000000FFU
#define XHCI_EXT_CAP_NEXT_SHIFT 8U
#define XHCI_EXT_CAP_NEXT_MASK 0x0000FF00U
#define XHCI_EXT_CAP_PROTOCOL 2U
#define XHCI_EXT_CAP_MAJOR_SHIFT 24U
#define XHCI_EXT_CAP_MINOR_SHIFT 16U
/* Dword 2 of a protocol capability: the port range and the PSI count. */
#define XHCI_EXT_PORT_OFFSET_MASK 0x000000FFU
#define XHCI_EXT_PORT_COUNT_SHIFT 8U
#define XHCI_EXT_PORT_COUNT_MASK 0x0000FF00U
#define XHCI_EXT_PORT_PSIC_SHIFT 28U
#define XHCI_EXT_PORT_PSIC_MASK 0xF0000000U
/* A Protocol Speed ID dword: PSIV is the value PORTSC's speed field reports. */
#define XHCI_EXT_PSI_PSIV_MASK 0x0000000FU
/* The walk of the extended capability list is bounded: the list is a linked
   list a broken controller could make cyclic, and 48 entries is the worst case
   Linux allows itself (XHCI_MAX_EXT_CAPS). */
#define XHCI_EXT_CAP_ENTRIES_MAX 48U

#define XHCI_OP_USBCMD 0x00U
#define XHCI_OP_USBSTS 0x04U
#define XHCI_OP_PAGESIZE 0x08U
#define XHCI_OP_CRCR 0x18U
#define XHCI_OP_DCBAAP 0x30U
#define XHCI_OP_CONFIG 0x38U
#define XHCI_OP_PORTSC_BASE 0x400U
#define XHCI_OP_PORTSC_STRIDE 0x10U

#define XHCI_RT_MFINDEX 0x00U
#define XHCI_RT_IR0 0x20U
#define XHCI_RT_IR_STRIDE 0x20U
#define XHCI_IR_IMAN 0x00U
#define XHCI_IR_IMOD 0x04U
#define XHCI_IR_ERSTSZ 0x08U
#define XHCI_IR_ERSTBA 0x10U
#define XHCI_IR_ERDP 0x18U

#define XHCI_USBCMD_RS 0x00000001U
#define XHCI_USBCMD_HCRST 0x00000002U
#define XHCI_USBCMD_INTE 0x00000004U
#define XHCI_USBCMD_EWE 0x00000400U

#define XHCI_USBSTS_HCH 0x00000001U
#define XHCI_USBSTS_CNR 0x00000800U
#define XHCI_USBSTS_HSE 0x00000004U

#define XHCI_PORTSC_CCS 0x00000001U
#define XHCI_PORTSC_PED 0x00000002U
#define XHCI_PORTSC_OCA 0x00000008U
#define XHCI_PORTSC_PR 0x00000010U
#define XHCI_PORTSC_PLS_SHIFT 5U
#define XHCI_PORTSC_PLS_MASK 0x000001E0U
#define XHCI_PORTSC_PP 0x00000200U

/*
 * ITEM-453: PORTSC.PLS (bits 8:5), the port link state of xHCI 1.1 table 5-27.
 *
 * A port that is not in a state which samples the connect status reports
 * CCS = 0 even with a device on it: the Disabled (4), Inactive (6), Compliance
 * (10) and Test Mode (11) states do not detect an attach at all.  A scan that
 * reads CCS once and moves on therefore answers "no device" for a port it
 * never brought up - and the 2026-09-19 hardware run is exactly that shape:
 * the owner's receiver is on `05:00.3`, where Linux enumerates it, and the
 * driver's port evidence reads `0x000002A0` (powered, RxDetect, no connect)
 * for the only two ports it published, with no record of the other four.
 */
#define XHCI_PORT_LINK_U0 0U
#define XHCI_PORT_LINK_U1 1U
#define XHCI_PORT_LINK_U2 2U
#define XHCI_PORT_LINK_U3 3U
#define XHCI_PORT_LINK_DISABLED 4U
#define XHCI_PORT_LINK_RXDETECT 5U
#define XHCI_PORT_LINK_INACTIVE 6U
#define XHCI_PORT_LINK_POLLING 7U
#define XHCI_PORT_LINK_RECOVERY 8U
#define XHCI_PORT_LINK_HOT_RESET 9U
#define XHCI_PORT_LINK_COMPLIANCE 10U
#define XHCI_PORT_LINK_TEST_MODE 11U
#define XHCI_PORT_LINK_RESUME 15U
#define XHCI_PORTSC_SPEED_SHIFT 10U
#define XHCI_PORTSC_SPEED_MASK 0x00003C00U
#define XHCI_PORTSC_PIC_MASK 0x0000C000U
#define XHCI_PORTSC_LWS 0x00010000U
/* Wake on connect / disconnect / over-current enable (bits 25:27). */
#define XHCI_PORTSC_WAKE_MASK 0x0E000000U
#define XHCI_PORTSC_CSC 0x00020000U
#define XHCI_PORTSC_PEC 0x00040000U
#define XHCI_PORTSC_WRC 0x00080000U
#define XHCI_PORTSC_PRC 0x00200000U
#define XHCI_PORTSC_PLC 0x00400000U
#define XHCI_PORTSC_CEC 0x00800000U
#define XHCI_PORTSC_WPR 0x80000000U
/* Every write-1-to-clear status bit of PORTSC. */
#define XHCI_PORTSC_RW1C (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | \
                          XHCI_PORTSC_WRC | XHCI_PORTSC_PRC | \
                          XHCI_PORTSC_PLC | XHCI_PORTSC_CEC)

#define XHCI_CRCR_RCS 0x00000001U
#define XHCI_ERDP_EHB 0x00000008U

/* Port speed (PSI) values, xHCI 1.1 table 5-27.  ITEM-438: all five are
   addressable - a device in a SuperSpeed (blue) port enumerates with the
   512-byte control pipe USB 3.0 section 9.6.1 defines. */
#define XHCI_SPEED_FULL 1U
#define XHCI_SPEED_LOW 2U
#define XHCI_SPEED_HIGH 3U
#define XHCI_SPEED_SUPER 4U
#define XHCI_SPEED_SUPER_PLUS 5U

/*
 * ITEM-406: the bits of PORTSC a read-modify-write may write back.
 *
 * PED (bit 1) is deliberately absent. xHCI 1.1 table 5-27 defines bits 1 and
 * 17:23 as "RW; writing a 1 clears the bit, writing a 0 has no effect": bit 1
 * is the port enable, so writing back a read value of 1 *disables* an enabled
 * port. The speed field (10:13), CCS (0) and the other read-only bits are
 * sticky state the controller owns, and the change bits (17:23) are
 * write-one-to-clear, so a plain read-modify-write must carry neither. What is
 * left - link state, port power, the port indicator and the wake enables - is
 * what this driver legitimately rewrites.
 */
#define XHCI_PORTSC_WRITE_PRESERVE \
    (XHCI_PORTSC_PLS_MASK | XHCI_PORTSC_PP | XHCI_PORTSC_PIC_MASK | \
     XHCI_PORTSC_WAKE_MASK)

/* The writable subset of a PORTSC value (see XHCI_PORTSC_WRITE_PRESERVE). */
uint32_t xhci_portsc_neutral(uint32_t portsc);

/*
 * ITEM-407: the value the scan writes to a root port before it reads that
 * port's connect status: the writable subset of what the port reported, plus
 * PORTSC.PP. Every other write this driver makes is driven by a status it has
 * already read; this one is not, because the status it needs (CCS) is exactly
 * what a port whose power is off may refuse to report - and the scan used to
 * reach the port-powering code only after it had seen CCS. PED and the
 * write-one-to-clear change bits stay out for the same reason as everywhere
 * else (xHCI 1.1 table 5-27).
 */
uint32_t xhci_portsc_claim(uint32_t portsc);

/* TRB types this driver produces and consumes, xHCI 1.1 table 6-9. */
enum xhci_trb_type {
    XHCI_TRB_NORMAL = 1,
    XHCI_TRB_SETUP_STAGE = 2,
    XHCI_TRB_DATA_STAGE = 3,
    XHCI_TRB_STATUS_STAGE = 4,
    XHCI_TRB_LINK = 6,
    XHCI_TRB_ENABLE_SLOT = 9,
    XHCI_TRB_DISABLE_SLOT = 10,
    XHCI_TRB_ADDRESS_DEVICE = 11,
    XHCI_TRB_CONFIGURE_ENDPOINT = 12,
    XHCI_TRB_EVALUATE_CONTEXT = 13,
    /*
     * ITEM-579: the two commands that repair a halted endpoint.  Reset Endpoint
     * moves a Halted endpoint to Stopped and Set TR Dequeue Pointer re-establishes
     * where its ring resumes; arming new TRBs and ringing the doorbell against a
     * Halted endpoint changes nothing, which is what the 10231684 run showed.
     */
    XHCI_TRB_RESET_ENDPOINT = 14,
    XHCI_TRB_SET_TR_DEQUEUE = 16,
    XHCI_TRB_NOOP_COMMAND = 23,
    XHCI_TRB_TRANSFER_EVENT = 32,
    XHCI_TRB_COMMAND_COMPLETION = 33,
    XHCI_TRB_PORT_STATUS_CHANGE = 34,
    /*
     * ITEM-473: the Host Controller Event TRB (xHCI 1.1 table 6-9).  It is how
     * the controller reports Event Ring Full (completion code 21); until this
     * item the driver could not decode it, so the one error that explained the
     * silent pointer was retired uncounted.
     */
    XHCI_TRB_HOST_CONTROLLER_EVENT = 37
};

/* Transfer-type field of a Setup Stage TRB. */
#define XHCI_TRT_NO_DATA 0U
#define XHCI_TRT_OUT 2U
#define XHCI_TRT_IN 3U

#define XHCI_TRB_CYCLE 0x00000001U
#define XHCI_TRB_ENT 0x00000002U
#define XHCI_TRB_ISP 0x00000004U
#define XHCI_TRB_NS 0x00000008U
#define XHCI_TRB_CHAIN 0x00000010U
#define XHCI_TRB_IOC 0x00000020U
#define XHCI_TRB_IDT 0x00000040U
#define XHCI_TRB_BSR 0x00000200U
#define XHCI_TRB_DIR 0x00010000U
#define XHCI_TRB_TYPE_SHIFT 10U
#define XHCI_TRB_TYPE_MASK 0x0000FC00U
#define XHCI_TRB_SLOT_SHIFT 24U
#define XHCI_TRB_EP_SHIFT 16U
#define XHCI_TRB_EP_MASK 0x001F0000U
#define XHCI_TRB_TC 0x00000002U

/* Endpoint context types, xHCI 1.1 table 6-7. */
#define XHCI_EP_TYPE_CONTROL 4U
#define XHCI_EP_TYPE_INTERRUPT_IN 7U

/* Completion codes this driver names, xHCI 1.1 table 6-9. */
#define XHCI_COMPLETION_SUCCESS 1U
#define XHCI_COMPLETION_SHORT_PACKET 13U

struct xhci_trb {
    uint32_t dword[4];
};

/* One decoded event TRB. Only the fields the driver reads are kept. */
struct xhci_event {
    enum xhci_trb_type type;
    uint8_t completion_code;
    uint8_t slot_id;
    uint8_t endpoint_id;
    uint16_t port_id;
    /* Residual transfer length of a Transfer Event TRB. */
    uint32_t transfer_length;
    /* Command Completion parameter (the assigned Slot ID, for example) or the
       physical address a completion refers to. */
    uint32_t parameter_low;
    uint64_t parameter;
    /*
     * ITEM-550: the four dwords exactly as the controller wrote them.  A
     * failure snapshot has to carry the entry the wait dequeued; re-reading the
     * slot afterwards would read whatever the producer has put there since.
     */
    uint32_t raw[4];
};

/*
 * A command or transfer ring: an array of TRBs whose last entry is a Link TRB
 * back to the first, with the producer cycle state that tells the controller
 * which entries are valid.
 */
struct xhci_ring {
    struct xhci_trb *trbs;
    uint64_t phys;
    uint32_t trb_count;
    uint32_t enqueue;
    uint8_t cycle;
};

void xhci_ring_init(
    struct xhci_ring *ring,
    struct xhci_trb *trbs,
    uint64_t phys,
    uint32_t trb_count,
    uint8_t cycle);
/* Append one TRB, writing the Link TRB first when the ring wraps. False when
   the ring is full (which cannot happen: the driver keeps one TRB in flight). */
bool xhci_ring_push(struct xhci_ring *ring, const struct xhci_trb *trb);
/* The same, reporting the slot the TRB landed in so the caller can compute the
   physical address a completion event refers to. */
bool xhci_ring_push_index(
    struct xhci_ring *ring,
    const struct xhci_trb *trb,
    uint32_t *index);
/* The TRB the controller will read next, or NULL past the end. */
const struct xhci_trb *xhci_ring_at(const struct xhci_ring *ring, uint32_t index);

struct xhci_trb xhci_trb_noop(uint8_t cycle);
struct xhci_trb xhci_trb_link(uint64_t ring_physical, uint8_t cycle);
struct xhci_trb xhci_trb_normal(
    uint8_t cycle,
    uint64_t buffer_physical,
    uint32_t length,
    bool interrupt_on_completion);
/*
 * ITEM-467: the Normal TRB an interrupt IN transfer is armed with.
 *
 * It carries both IOC and ISP.  IOC asks for a Transfer Event when the
 * transfer completes; ISP extends that to a *short* packet as well.  The
 * distinction is what a boot mouse needs: the driver asks for four bytes (the
 * three boot fields and the wheel byte a report is commonly extended with),
 * while the Logitech receiver's boot mouse answers with three, and without ISP
 * the controller keeps waiting for a fourth byte the device never sends - the
 * transfer is never completed, never re-armed, and the pointer never moves.
 * A keyboard is unaffected: its eight-byte boot report fills the buffer, so
 * the short-packet path is simply never taken.
 */
struct xhci_trb xhci_trb_interrupt_in(
    uint8_t cycle,
    uint64_t buffer_physical,
    uint32_t length);
/*
 * ITEM-467: why a bound device has delivered no report yet, as one word the
 * record can publish.  A transfer that never completed means the controller
 * still owns the buffer (`INTERRUPT_PENDING`); a transfer that completed and
 * was re-armed but carried nothing means the device answered without a report
 * (`COMPLETION_WITHOUT_REPORT`); a device that was never armed is
 * `NOT_ARMED`.  Pure, so the host proof drives every shape.
 */
const char *xhci_no_report_reason(
    bool pending,
    uint32_t reports,
    uint32_t transfers);
struct xhci_trb xhci_trb_setup(
    uint8_t cycle,
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    uint16_t length,
    uint8_t transfer_type);
struct xhci_trb xhci_trb_data(
    uint8_t cycle,
    uint64_t buffer_physical,
    uint32_t length,
    bool direction_in,
    bool interrupt_on_completion);
struct xhci_trb xhci_trb_status(uint8_t cycle, bool direction_in);
struct xhci_trb xhci_trb_enable_slot(uint8_t cycle);
struct xhci_trb xhci_trb_disable_slot(uint8_t cycle, uint8_t slot_id);
struct xhci_trb xhci_trb_address_device(
    uint8_t cycle,
    uint64_t input_context_physical,
    uint8_t slot_id);
struct xhci_trb xhci_trb_configure_endpoint(
    uint8_t cycle,
    uint64_t input_context_physical,
    uint8_t slot_id);
/*
 * ITEM-420: Evaluate Context (type 13) is its own command.  Sending an Address
 * Device TRB with the Block Set Address bit set - which is what this driver did
 * while bringing up the boot keyboard - is a different command to the
 * controller: the acceptance host answered it with a Context State Error, and
 * the keyboard never reached its configuration.
 */
struct xhci_trb xhci_trb_evaluate_context(
    uint8_t cycle,
    uint64_t input_context_physical,
    uint8_t slot_id);
/*
 * ITEM-579: repair a halted endpoint (xHCI 1.1 sections 6.4.3.7 and 6.4.3.9).
 * Reset Endpoint names the slot and the endpoint; Set TR Dequeue Pointer then
 * republishes the ring position - with the Dequeue Cycle State bit set, because
 * the recovered ring starts at cycle 1 again.
 */
struct xhci_trb xhci_trb_reset_endpoint(
    uint8_t cycle,
    uint8_t slot_id,
    uint8_t dci);
struct xhci_trb xhci_trb_set_tr_dequeue(
    uint8_t cycle,
    uint8_t slot_id,
    uint8_t dci,
    uint64_t dequeue_physical);
struct xhci_trb xhci_trb_noop_command(uint8_t cycle);

uint8_t xhci_trb_type(const struct xhci_trb *trb);
bool xhci_trb_cycle(const struct xhci_trb *trb);
bool xhci_event_decode(const struct xhci_trb *trb, struct xhci_event *event);

const char *xhci_trb_type_string(uint8_t type);
const char *xhci_completion_code_string(uint8_t code);
const char *xhci_speed_string(uint8_t speed);
/* Default Endpoint 0 max packet size for a port speed, xHCI 1.1 section 4.8.1:
   8 bytes for low/full speed, 64 for high speed, 512 for SuperSpeed. 0 means
   the speed is not one this driver can address. */
uint16_t xhci_speed_ep0_max_packet(uint8_t speed);

/*
 * ITEM-406: the Interval field of an interrupt endpoint context, from the
 * endpoint descriptor's bInterval and the speed the port reported.
 *
 * The controller's field is a period, not the descriptor's number: for a
 * high-speed endpoint the period is 2^(Interval-1) x 125 us, so the descriptor
 * value is the exponent and is written as it stands; for a low- or full-speed
 * endpoint the field counts 125 us units, so bInterval (milliseconds) becomes
 * bInterval * 8, and any value above 16 (the descriptor allows up to 255, which
 * a boot keyboard's second interface uses) means "16 ms" and encodes as 128
 * (xHCI 1.1 section 6.2.3.6 and table 6-8). A speed this driver cannot address
 * has no interval and returns 0.
 */
uint8_t xhci_endpoint_interval(uint8_t speed, uint8_t b_interval);

/*
 * ITEM-407: how many device slots this driver enables on a controller that
 * advertises `max_slots` of them. A controller is never refused for being
 * bigger than this driver's table: what is capped is the number of slots the
 * driver turns on (`CONFIG.MaxSlotsEn`), and therefore the size of the
 * structures that must exist per slot. Zero means the controller's own
 * HCSPARAMS1 is unusable (`MaxSlots == 0`); every controller at or above
 * XHCI_DRIVER_SLOTS gets exactly XHCI_DRIVER_SLOTS.
 */
uint32_t xhci_slots_enabled(uint32_t max_slots);

/* True when `slot_id` is one this driver enabled, i.e. a slot whose device
   context exists in the table the DCBAAP entries point into. */
bool xhci_slot_within_driver_bound(uint32_t slot_id);

/* XHCI_MAX_PORTS and XHCI_MAX_SLOTS are the widest values the specification's
   fields can carry; a caller that would store more has read a bogus register. */
bool xhci_max_ports_valid(uint32_t max_ports);
bool xhci_max_slots_valid(uint32_t max_slots);

/* CAPLENGTH is a byte-sized, DWORD-aligned operational-register offset.
   The fixed operational block through CONFIG must fit the mapped window.
   A zero window keeps the existing host-model "unspecified" convention. */
bool xhci_caplength_valid(uint32_t caplength, uint32_t mmio_bytes);

/* Bytes of the Device Context Base Address Array for a controller (xHCI 1.1
   section 6.1: MaxSlots+1 entries, entry 0 being the scratchpad array). Zero
   for a slot count that is not a valid HCSPARAMS1 field, so a caller can use
   it as its own validity check. */
size_t xhci_dcbaa_bytes(uint32_t max_slots);

/*
 * ITEM-407: the bounded port evidence a controller publishes.
 *
 * The first hardware run with the ITEM-406 driver printed NO_DEVICE for the
 * controller the keyboard is on, and nothing in the record said whether every
 * port really reported "no device connected" or whether the driver was reading
 * the wrong register - the acceptance host has no serial capture, so the only
 * evidence is the one photograph of the screen. These helpers bound and format
 * that evidence so the text is composed and proven on the host:
 *
 *   * xhci_port_evidence_count() is how many ports are published: the
 *     controller's own count, capped at XHCI_PORT_EVIDENCE_MAX so a 255-port
 *     controller cannot outgrow the record. The cap is two because the whole
 *     `[XHCI]` line has to stay inside the console mirror's one-painted-line
 *     budget (CONSOLE_RECORD_MAX, 320 bytes) with a detail at the full width of
 *     its own buffer: the record's worst case is 314 of those bytes
 *     (`XHCI_RECORD_WORST_CASE_BYTES` in kernel/src/xhci.c, which
 *     `make test-xhci-host` pins). Two ports already separate "no connect on
 *     every port" from "the port was never powered" and from "the register does
 *     not answer", and `ports=` at the end of the record still names the
 *     controller's real port count, so evidence that stops early is never
 *     mistaken for a controller with two ports;
 *   * xhci_format_port_evidence() writes those PORTSC values as
 *     "0x1A2B3C4D,0x00000600,..." - most significant byte first, values
 *     separated by a comma and a trailing comma when more ports follow, no
 *     leading separator, always NUL-terminated - and returns how many values
 *     fit; a buffer too small for even one value is filled with the empty
 *     string. XHCI_PORT_EVIDENCE_BYTES is that buffer's worst case;
 *   * xhci_ports_powered_mask() compresses which of those PORTSC values had
 *     PORTSC.PP set, so "the port was never powered" is one value to read
 *     rather than a scan of the list.
 */
#define XHCI_PORT_EVIDENCE_MAX 2U
#define XHCI_PORT_EVIDENCE_BYTES \
    (XHCI_PORT_EVIDENCE_MAX * 11U + 2U)

uint32_t xhci_port_evidence_count(uint32_t max_ports);
size_t xhci_format_port_evidence(
    char *buffer,
    size_t size,
    const uint32_t *portsc,
    uint32_t ports);
uint32_t xhci_ports_powered_mask(const uint32_t *portsc, uint32_t ports);

/*
 * ITEM-453: the port link state, and what a port needs before its "no device"
 * answer may be believed.
 *
 * The 2026-09-19 hardware run showed the driver answering NO_CONNECTED_DEVICE
 * for a controller where Linux enumerates the owner's receiver: both built-in
 * controllers published `pp=3 ppoff=0` and two raw PORTSC values of
 * `0x000002A0` (powered, link in RxDetect, no connect) - and the other four
 * ports of those six-port controllers were not published at all, so "the
 * register answered no device" and "the driver never looked" cannot be told
 * apart from the record.  The claim that made ITEM-407 possible (write PP,
 * then read CCS) is still the right first step, but it is not a bring-up:
 *
 *   * a port whose power was never asserted is powered and then given a
 *     bounded window for its connect status to rise - USB 2.0 section 7.1.7.3
 *     allows a device up to 100 ms (TATTDB) to signal an attach after power;
 *   * a port whose link state is one that does not sample the connect status
 *     (Disabled, Inactive, Compliance, Test Mode) is written back to RxDetect
 *     with the Link State Write Strobe, because a port in those states reports
 *     CCS = 0 whatever is plugged into it;
 *   * a controller that has just been reset gets one shared connect-debounce
 *     window before any of its ports is believed empty;
 *   * every port of every controller publishes its own `[XHCI-PORT]` line with
 *     the raw PORTSC value it answered before the bring-up, the value after
 *     it, and the decoded power/connect/link state - so the next hardware run
 *     answers "which port, and what did it say" for all of them, not for two.
 *
 * xhci_port_bring_up_plan() is the pure decision: it never touches a register
 * and is the half the host proof drives with the exact PORTSC values the
 * owner's machine published.
 */
uint32_t xhci_port_link_state(uint32_t portsc);
const char *xhci_port_link_state_string(uint32_t state);
/* True when a port in this link state samples PORTSC.CCS at all. */
bool xhci_port_link_samples_connect(uint32_t state);
/*
 * ITEM-545: true while a port that samples a connect has not reported one
 * yet.  A link that is still training answers false to "connected" and true
 * here, and the controller's post-reset window keeps waiting while any port
 * answers true: Linux's xhci_start() polls the roothubs for 500 ms after the
 * controller starts for exactly this reason ("SS devices are only visible to
 * roothub after link training completes"), and a connect on one port must not
 * cancel the wait for a SuperSpeed link beside it.
 */
bool xhci_port_connect_pending(uint32_t portsc);
/* Linux xhci_start(): the grace window a just-started controller gets before
   its still-training ports are believed empty. */
#define XHCI_PORT_TRAINING_MS 500U

struct xhci_port_bring_up {
    /* The PORTSC the port answered before any write. */
    uint32_t was;
    /* was & PORTSC.PLS, and whether that state samples a connect. */
    uint32_t link;
    bool powered;
    bool connected;
    bool samples;
    /* PORTSC.PP was clear: assert power before reading the port again. */
    bool power_on;
    /* The link state does not sample connect: write PLS=RxDetect with LWS. */
    bool link_kick;
    /* Bounded window, in milliseconds, the port is given to show a connect
       after the writes above; zero when the port is already connected or was
       not touched (a controller that was just reset gets one shared window
       before any of its ports is believed empty, and the driver runs it). */
    uint32_t settle_ms;
};
/* USB 2.0 section 7.1.7.3: a device may take up to 100 ms (TATTDB) to signal
   an attach after the port it is on was powered or brought back to RxDetect. */
#define XHCI_PORT_SETTLE_MS 100U
void xhci_port_bring_up_plan(uint32_t portsc, struct xhci_port_bring_up *plan);
/* The PORTSC write that moves a port to RxDetect: the writable bits the port
   reported, its power preserved, LWS set, PLS = RxDetect, and never PED or a
   write-one-to-clear change bit. */
uint32_t xhci_portsc_link_kick(uint32_t portsc);

/* MOUSE-DIAG (owner task 2026-09-27): the bounded stall snapshot.
 *
 * Two hardware runs in a row ended with a working pointer that stopped:
 * `mouse_armed=16`, the endpoint context Running, every completion code
 * SUCCESS, `mse=0`, `no_trb=0`, `ringfull=0`, and no report for the rest of
 * the run.  The journals could not separate the two candidate causes:
 *
 *   A) the USB mouse stopped sending reports;
 *   B) the xHCI controller stopped delivering events for that device.
 *
 * The difference is readable from three places the records did not carry at
 * the moment of the stall: the controller's event-ring dequeue pointer and
 * interrupter registers, the endpoint's own counters, and the PORTSC of the
 * port the device sits on.  These formatters are the pure half - they take the
 * values and write one bounded line - so `make test-xhci-host` pins the field
 * set without a controller, and the kernel reads the registers once per
 * silence episode (never per loop turn) and publishes them.
 *
 * `XHCI_SILENCE_*_RECORD_BYTES` is each buffer's worst case; a buffer too
 * small is filled with the empty string and the call returns 0.
 */
#define XHCI_SILENCE_RECORD_BYTES 320U
#define XHCI_SILENCE_PORT_RECORD_BYTES 320U
#define XHCI_ITER_MAX_RECORD_BYTES 192U

struct xhci_silence_time {
    uint64_t ticks;
    uint32_t since_completion_ms;
    uint32_t since_report_ms;
};

struct xhci_silence_endpoint {
    uint32_t slot;
    uint32_t dci;
    uint32_t endpoint_state;
    uint32_t armed;
    uint32_t pending;
    uint32_t reposted;
    uint32_t completed;
    uint32_t reports;
    uint32_t last_completion_code;
    uint64_t last_completion_ticks;
};

struct xhci_silence_controller {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint32_t usbcmd;
    uint32_t usbsts;
    uint64_t crcr;
    uint64_t erdp;
    uint64_t erstba;
    uint32_t iman;
    uint32_t imod;
    uint64_t error;
    uint32_t host_events;
    uint32_t ringfull;
};

size_t xhci_format_silence_record(
    char *buffer,
    size_t size,
    const struct xhci_silence_time *time,
    const struct xhci_silence_endpoint *endpoint);
size_t xhci_format_silence_controller_record(
    char *buffer,
    size_t size,
    const struct xhci_silence_controller *controller);
/* `portsc` holds the ports read from the controller's operational registers,
   `portsc[index]` being port `index + 1`.  `mouse_port` is the port the bound
   device was found on, or 0 when the driver cannot name it - the record then
   carries `identified=0` and the raw value of every port it read, which is the
   fallback the diagnostic contract asks for. */
size_t xhci_format_silence_port_record(
    char *buffer,
    size_t size,
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint32_t mouse_port,
    const uint32_t *portsc,
    size_t portsc_count);
/* The same controller and port read again after the watchdog's single
   re-arm/kick, so "the dequeue pointer did not move after the recovery" is a
   number in the journal and not an inference from a later heartbeat. */
size_t xhci_format_silence_after_record(
    char *buffer,
    size_t size,
    uint64_t ticks,
    uint32_t waited_ms,
    uint64_t erdp_before,
    uint64_t erdp_after,
    uint32_t usbcmd,
    uint32_t usbsts,
    uint32_t portsc);
/* One `[ITER-MAX]` line: a new worst loop turn past the long-turn budget, with
   the phase that spent it and the clock it happened on.  The boot maximum was
   published as a number without a phase or a time, so `iter_max_us=3259516`
   could not be told from a measurement artifact; this record is written once
   per new maximum, not per turn. */
size_t xhci_format_iter_max_record(
    char *buffer,
    size_t size,
    uint64_t ticks,
    uint64_t loops,
    uint64_t second,
    uint64_t iter_max_us,
    const char *phase);

/*
 * MOUSE-DIAG step 3 (owner task 2026-09-27): the three records that close the
 * remaining unknowns.  Telemetry only - nothing here reads or writes a
 * register, and no caller changes what the kernel does with the values.
 *
 *   * `[TIMING-CHECK]` - an independent raw anchor beside the existing phase
 *     accounting.  `hw14` published `journal_us=2 829 709` and
 *     `xhci_us=3 458 581` in windows whose ISR tick counter advanced exactly
 *     100 ticks and whose cumulative cycle clock advanced one second, so the
 *     multi-second spans are an accounting artifact and not elapsed time; the
 *     mechanism is unknown.  This record carries the raw cycle and tick values
 *     at both ends of the window plus `raw_elapsed_us` and a
 *     `timing_accounting_mismatch` flag, so the next run shows the
 *     inconsistency in place instead of needing the arithmetic reconstructed
 *     from six records.
 *   * `[EP-STATE]` - the endpoint context and ring values neither endpoint
 *     ever published: the TR Dequeue Pointer and its dequeue cycle state, the
 *     transfer ring base, the producer enqueue position and its cycle.  The
 *     stall evidence (ERDP frozen, EP0 `code=254`, PORTSC `0x00000603`) says
 *     the controller stopped delivering, but not whether it stopped *fetching*
 *     the armed TRBs; these fields are what would say.
 *   * `[BOOTLOG-FLUSH]` - the existing flush path, measured.  The regular
 *     flush carries 59 records / 8111 bytes of text, and a single-record
 *     marker flush costs 0.13 s of tick time; the publish/write/commit split
 *     the owner asked for is published where the architecture makes the point
 *     measurable and marked `unavailable=` where it does not (the plan walk
 *     and the slot write are interleaved, so "publish" is derived).
 *
 * Each formatter is pure, so `make test-xhci-host` pins every field, the
 * `unavailable` naming and every record's worst case inside the console's
 * 320-byte line budget.
 */
#define XHCI_TIMING_CHECK_RECORD_BYTES 320U
#define XHCI_EP_STATE_RECORD_BYTES 320U
#define XHCI_BOOTLOG_FLUSH_RECORD_BYTES 320U

struct xhci_timing_check {
    uint64_t start_tsc;
    uint64_t end_tsc;
    uint64_t elapsed_cycles;
    uint64_t ticks_start;
    uint64_t ticks_end;
    uint64_t real_seconds_start;
    uint64_t real_seconds_end;
    uint64_t tick_hz;
    uint64_t raw_elapsed_us;
    uint64_t sum_phase_us;
    uint64_t work_us;
    uint64_t turn_us;
    uint64_t worst_us;
    uint64_t iter_max_us;
    uint32_t mismatch;
};

struct xhci_ep_state {
    /* 0 keyboard, 1 mouse: the record names the role, not only the endpoint. */
    uint32_t role;
    uint32_t endpoint_address;
    uint32_t dci;
    uint32_t endpoint_state;
    uint64_t ring_base;
    uint64_t dequeue_pointer;
    uint32_t dequeue_cycle;
    uint32_t enqueue;
    uint32_t ring_trbs;
    uint32_t producer_cycle;
    uint32_t armed;
    uint32_t pending;
    uint32_t reposted;
    uint32_t last_completion_code;
    uint32_t last_completion_ms;
};

/* What the store measured about the last flush.  `publish_us` is derived
   (total minus the two measured spans) because the plan walk and the slot
   write are interleaved in one loop; `unavailable` names that in the record
   instead of pretending the point was measured. */
struct xhci_bootlog_flush {
    uint32_t records;
    uint64_t text_bytes;
    uint64_t slot_bytes;
    uint64_t publish_us;
    uint64_t esp_write_us;
    uint64_t commit_us;
    uint64_t total_us;
};

size_t xhci_format_timing_check_record(
    char *buffer,
    size_t size,
    const struct xhci_timing_check *check);
/* The accounting half (phase sum, work, turn, worst, boot maximum and the
   mismatch flag), beside the raw anchors the main record carries. */
size_t xhci_format_timing_check_tail_record(
    char *buffer,
    size_t size,
    const struct xhci_timing_check *check);
size_t xhci_format_ep_state_record(
    char *buffer,
    size_t size,
    const struct xhci_ep_state *state);
/* The ring half of the same snapshot (ring base, TR Dequeue Pointer and its
   cycle, producer enqueue and cycle), on its own bounded line. */
size_t xhci_format_ep_state_tail_record(
    char *buffer,
    size_t size,
    const struct xhci_ep_state *state);
size_t xhci_format_bootlog_flush_record(
    char *buffer,
    size_t size,
    const struct xhci_bootlog_flush *flush);

/* The Endpoint Context's TR Dequeue Pointer (xHCI 1.1 section 6.2.3, dwords
   2 and 3 of the endpoint's own context entry): bits 63:4 are the pointer and
   bit 0 is the Dequeue Cycle State.  Read-only, and the same context entry
   `xhci_endpoint_state()` reads.  False when the context is missing or the DCI
   is out of range, with both outputs left at zero. */
bool xhci_endpoint_dequeue(
    const void *device_context,
    size_t context_size,
    uint8_t dci,
    uint64_t *pointer_out,
    uint32_t *cycle_out);

/* One `[XHCI-PORT]` line: the raw PORTSC before and after the bring-up, the
   decoded power/connect/link state, and whether the bring-up wrote anything.
   XHCI_PORT_RECORD_BYTES is the buffer's worst case; a buffer too small is
   filled with the empty string and the call returns 0. */
#define XHCI_PORT_RECORD_BYTES 128U
size_t xhci_format_port_record(
    char *buffer,
    size_t size,
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint32_t port,
    const char *group,
    const struct xhci_port_bring_up *plan,
    uint32_t now,
    uint32_t waited_ms);

/*
 * ITEM-453: one Supported Protocol capability (xHCI 1.1 section 7.2.1), the
 * controller saying which raw ports belong to which protocol.  The four
 * `dwords`-free arguments are the capability's own first three dwords and its
 * Protocol Speed ID dwords; the parser only extracts fields, so the host proof
 * drives it with the exact values a controller would answer.
 *
 * `port_offset` and `port_count` are published raw, and the group's ports are
 * numbered `port_offset + 1` through `port_offset + port_count` (the offset is
 * the number of ports before the group, xHCI 1.1 section 7.2.1) - the raw
 * fields are in the record beside the derived range so a controller that
 * numbers them differently is readable rather than silently mislabelled.
 */
#define XHCI_PROTOCOL_GROUPS_MAX 4U
#define XHCI_PROTOCOL_PSI_MAX 4U
struct xhci_protocol_group {
    uint8_t major;
    uint8_t minor;
    uint8_t port_offset;
    uint8_t port_count;
    uint8_t psic;
    uint8_t psiv_count;
    uint8_t psiv[XHCI_PROTOCOL_PSI_MAX];
    /* The capability's four-character name with trailing spaces removed. */
    char name[5];
    /* The three dwords the fields above were parsed from, kept so the record
       can publish them beside the interpretation. */
    uint32_t raw_revision;
    uint32_t raw_name;
    uint32_t raw_port_info;
};

bool xhci_protocol_capability(
    uint32_t revision,
    uint32_t name_string,
    uint32_t port_info,
    const uint32_t *psi,
    uint32_t psi_count,
    struct xhci_protocol_group *group);
/* The raw port range the capability declares: first = offset + 1,
   last = offset + count. False when the capability is unusable. */
bool xhci_protocol_port_range(
    const struct xhci_protocol_group *group,
    uint32_t *first,
    uint32_t *last);
/* The group that owns a raw port number, or NULL when no declared group does.
   A null `groups` or a zero count answers NULL rather than reading through. */
const struct xhci_protocol_group *xhci_protocol_for_port(
    const struct xhci_protocol_group *groups,
    uint32_t count,
    uint32_t port);
/* The stable label of a protocol group from its major revision ("USB3" for 3,
   "USB2" for 2, ...), or NULL for a revision this driver cannot name. */
const char *xhci_protocol_group_label(uint8_t major);

/* One `[XHCI-PROTO]` line: the group's parsed fields and the raw dwords it was
   parsed from, so a wrong interpretation of the capability is visible on the
   next hardware run instead of being trusted. */
#define XHCI_PROTOCOL_RECORD_BYTES 192U
size_t xhci_format_protocol_record(
    char *buffer,
    size_t size,
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint32_t index,
    uint32_t groups,
    const struct xhci_protocol_group *group,
    uint32_t revision,
    uint32_t name_string,
    uint32_t port_info);

/* Device context layout, xHCI 1.1 section 6.2.2. */
#define XHCI_CONTEXT_32 32U
/* One Device Context: the slot context plus 31 endpoint contexts. */
#define XHCI_DEVICE_CONTEXT_ENTRIES 32U
#define XHCI_CONTEXT_64 64U
#define XHCI_INPUT_CONTEXT_ENTRIES 33U
#define XHCI_INPUT_CONTEXT_MAX_BYTES \
    (XHCI_INPUT_CONTEXT_ENTRIES * XHCI_CONTEXT_64)

void xhci_context_clear(void *base, size_t bytes);
/* Input Control Context: Add and Drop flags for context index 0 (slot) and
   index N (the endpoint with device context index N). */
void xhci_input_control_context(
    void *base,
    size_t context_size,
    uint32_t add_flags,
    uint32_t drop_flags);
void xhci_slot_context(
    void *entry,
    uint8_t speed,
    uint8_t context_entries,
    uint8_t root_hub_port);
/* `interval` is the endpoint context's Interval field value, i.e.
   xhci_endpoint_interval(speed, bInterval) and not the descriptor's raw
   bInterval. */
void xhci_endpoint_context(
    void *entry,
    uint8_t endpoint_type,
    uint16_t max_packet_size,
    uint8_t interval,
    uint64_t dequeue_physical,
    uint16_t average_trb_length);
/*
 * ITEM-545: the same context with the endpoint's Max Burst Size (xHCI 1.1
 * section 6.2.3.5, dword 4 bits 23:16).  A SuperSpeed bulk endpoint declares
 * its burst in the companion descriptor (bMaxBurst); every other endpoint
 * declares none, which is the zero xhci_endpoint_context() programs.
 */
void xhci_endpoint_context_burst(
    void *entry,
    uint8_t endpoint_type,
    uint16_t max_packet_size,
    uint8_t interval,
    uint64_t dequeue_physical,
    uint16_t average_trb_length,
    uint8_t max_burst);

/* Endpoint context types, xHCI 1.1 table 6-7 (2 = Bulk OUT, 6 = Bulk IN). */
#define XHCI_EP_TYPE_BULK_OUT 2U
#define XHCI_EP_TYPE_BULK_IN 6U

/* Device context index of an endpoint: 2 * number + direction. Endpoint 0 is
   always index 1. */
uint8_t xhci_endpoint_dci(uint8_t endpoint_number, bool direction_in);

/*
 * The two halves of the context addressing rule, which are easy to swap and
 * impossible to debug from a completion code alone:
 *   * an Add/Drop Context flag bit is the device context index itself, so the
 *     slot context is bit 0, Endpoint 0 is bit 1 and an endpoint with DCI d is
 *     bit d;
 *   * the context entry lives one slot further along the Input Context,
 *     because entry 0 is the Input Control Context and Endpoint 0 sits between
 *     the slot context and the first data endpoint, so the entry index is the
 *     DCI plus one.
 * The host proof pins both halves against the specification's tables.
 */
uint32_t xhci_context_flag(uint8_t dci);
size_t xhci_context_entry_index(uint8_t dci);

/* The offset of context index `index` inside an input/device context. */
size_t xhci_context_offset(size_t context_size, uint32_t index);

/*
 * ITEM-473 (СРОЧНО-23): the run-time arm depth the console's `ARM <n>` command
 * sets, and the endpoint state the diagnostics publish.
 *
 * The clamp is pure so the host proof can pin the bound: the request is held
 * between XHCI_ARM_DEPTH_MIN and XHCI_ARM_DEPTH_MAX (the compiled mouse depth
 * the interrupt ring backs, which is the driver's default).  Because the
 * maximum is below XHCI_EVENT_TRBS, no legal depth can oversubscribe the
 * session event ring - the property the static assertions in xhci.c also pin.
 *
 * `xhci_endpoint_state()` reads Endpoint State (bits 2:0 of the Endpoint
 * Context's dword 0, xHCI 1.1 section 6.2.3) out of a device context at
 * `device_context`, at the endpoint's own device-context index (the slot
 * context is entry 0 and Endpoint 0 is DCI 1).  `xhci_endpoint_state_string()`
 * names the five values (Disabled / Running / Halted / Stopped / Error) so "the
 * endpoint went silent" is read from the context instead of inferred from a
 * missing report.
 */
#define XHCI_ARM_DEPTH_MIN 1U
#define XHCI_ARM_DEPTH_MAX 16U
uint8_t xhci_arm_depth_clamp(uint32_t requested);

uint32_t xhci_endpoint_state(const void *device_context, size_t context_size,
                             uint8_t dci);
const char *xhci_endpoint_state_string(uint32_t state);

/*
 * MOUSE-DIAG (owner task 2026-09-27, second step): is the *control* plane of a
 * silent pointer still answering?
 *
 * `hw13` narrowed the pointer's stall to the transfer leg: the port stayed
 * connected and enabled (PORTSC `ccs=1 ped=1 pls=U0`, every change bit clear),
 * the controller stayed running with no error and no event-ring pressure, the
 * endpoint stayed `Running` with sixteen transfers armed - and no completion
 * arrived.  What that evidence cannot say is which end stopped: a device whose
 * interrupt pipe went quiet, or a slot/device that stopped answering at all.
 *
 * The one read-only observation that separates them is a single standard
 * `GET_STATUS` control transfer on the same slot's EP0.  A device that answers
 * it has a live control plane and a quiet interrupt pipe
 * (`INTERRUPT_ENDPOINT_OR_DEVICE_REPORT_PATH`); a device that does not answer
 * it is unresponsive at the slot level (`DEVICE_OR_SLOT_UNRESPONSIVE`).  The
 * probe writes no register, resets nothing, changes no address and re-arms
 * nothing, and it is issued at most once per silence episode.
 *
 * `XHCI_EP0_NOT_SUBMITTED` and `XHCI_EP0_NO_COMPLETION` are the two outcomes
 * that are not a device completion code: the first says the driver could not
 * even place the transfer (so nothing was observed), the second that the
 * status stage never completed inside the driver's own transfer budget.
 */
#define XHCI_EP0_NOT_SUBMITTED 0xFFU
#define XHCI_EP0_NO_COMPLETION 0xFEU
#define XHCI_EP0_PROBE_RECORD_BYTES 320U

struct xhci_ep0_probe {
    uint64_t ticks;
    uint32_t slot;
    uint32_t dci;
    uint32_t request;
    uint32_t submitted;
    uint32_t completion_code;
    uint64_t elapsed_us;
    uint32_t endpoint_state_before;
    uint32_t endpoint_state_after;
    uint32_t slot_state_before;
    uint32_t slot_state_after;
    uint32_t portsc_before;
    uint32_t portsc_after;
    uint32_t usbsts_before;
    uint32_t usbsts_after;
    uint64_t erdp_before;
    uint64_t erdp_after;
};

/* The control result: what was asked, whether it was submitted, its completion
   code and how long the driver's bounded transfer budget took. */
size_t xhci_format_ep0_probe_record(
    char *buffer,
    size_t size,
    const struct xhci_ep0_probe *probe);
/* The state around the probe: the port, the controller status, the event-ring
   dequeue pointer and the two context states, before and after. */
size_t xhci_format_ep0_state_record(
    char *buffer,
    size_t size,
    const struct xhci_ep0_probe *probe);

/*
 * ITEM-550: why one command never completed, and what its wait consumed.
 *
 * The owner's L875 run ended with `reason=ENUMERATION_FAILED
 * detail=P1_ENABLE_SLOT_TIMEOUT` and nothing that could be read after the boot:
 * which command was submitted, where in the command ring it landed, what the
 * controller's own registers said at that moment and what the wait actually
 * dequeued were all local to a function whose session had been released by the
 * time the `xhci` command ran.  Two very different defects produce that one
 * detail - a command the ring never accepted and a command the controller
 * accepted but never completed - and this vocabulary is what separates them.
 *
 * The snapshot is a plain value filled at the failure point inside the wait and
 * kept in the machine's own RAM, never in a session (which is zeroed on
 * release), so the command can print it afterwards without touching a
 * controller that may be gone.  Nothing here prints inside the wait loop and
 * nothing here writes a register at all: the diagnostic must not change the
 * state it describes.
 */
enum xhci_command_result {
    XHCI_COMMAND_RESULT_SUCCESS = 0,
    XHCI_COMMAND_RESULT_ENQUEUE_FAILED = 1,
    XHCI_COMMAND_RESULT_TIMEOUT = 2
};

/* What one dequeued event meant to the wait for one submitted command. */
enum xhci_command_event_kind {
    XHCI_COMMAND_EVENT_OTHER = 0,   /* not a Command Completion at all */
    XHCI_COMMAND_EVENT_FOREIGN = 1, /* a completion for a different TRB */
    XHCI_COMMAND_EVENT_OWN = 2      /* the completion of this command */
};

/* The wait's own bookkeeping, so "no event at all" and "only completions for
   somebody else's command" are different records instead of the same silence. */
struct xhci_command_wait {
    uint32_t events;
    uint32_t unmatched;
    bool unmatched_valid;
    uint64_t unmatched_address;
    uint8_t unmatched_type;
    uint8_t unmatched_code;
    uint8_t unmatched_slot;
};

struct xhci_command_snapshot {
    bool valid;
    enum xhci_command_result result;
    /* The submitted command: its type, all four dwords and where they landed. */
    uint8_t type;
    uint32_t trb[4];
    uint64_t physical;
    uint32_t ring_index;
    uint8_t ring_cycle;
    /* The controller that was addressed. */
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    /* The controller registers at the failure point (read-only). */
    uint32_t usbcmd;
    uint32_t usbsts;
    uint32_t config;
    uint32_t erstsz;
    uint32_t iman;
    uint64_t crcr;
    uint64_t dcbaap;
    uint64_t erstba;
    uint64_t erdp;
    /* The event ring's consumer position and the last entry it read. */
    uint32_t event_index;
    uint8_t event_cycle;
    bool event_valid;
    uint32_t event[4];
    struct xhci_command_wait wait;
};

const char *xhci_command_result_string(enum xhci_command_result result);
/* OTHER for anything that is not a Command Completion, FOREIGN for a completion
   whose Command TRB Pointer is not the submitted command's address, OWN for the
   completion this wait is waiting for. */
enum xhci_command_event_kind xhci_command_event_kind_of(
    const struct xhci_event *event,
    uint64_t expected_address);
void xhci_command_wait_init(struct xhci_command_wait *wait);
/* Counts one dequeued event and remembers the last foreign completion. */
void xhci_command_wait_note(struct xhci_command_wait *wait,
                            const struct xhci_event *event,
                            enum xhci_command_event_kind kind);
/* The outcome of one submitted command: SUCCESS when the ring accepted it and
   its own completion arrived, ENQUEUE_FAILED when the ring refused it, TIMEOUT
   when the ring accepted it and the completion never came. */
enum xhci_command_result xhci_command_result_of(bool enqueued, bool completed);
/* Hands a captured snapshot to the machine-lifetime record: the value is
   copied, so wiping the capture buffer - which is what releasing the session
   does to everything it held - leaves the record intact. */
void xhci_command_snapshot_keep(struct xhci_command_snapshot *kept,
                                const struct xhci_command_snapshot *captured);

#define XHCI_COMMAND_RECORD_BYTES 320U
/* The command itself: outcome, controller, TRB type and address, ring position
   and the four dwords the ring holds. */
size_t xhci_format_command_record(
    char *buffer,
    size_t size,
    const struct xhci_command_snapshot *snapshot);
/* The controller's registers at the failure point. */
size_t xhci_format_command_register_record(
    char *buffer,
    size_t size,
    const struct xhci_command_snapshot *snapshot);
/* What the wait consumed: the event-ring consumer position, the last entry it
   read, how many events it saw and the last command completion that named
   another TRB. */
size_t xhci_format_command_event_record(
    char *buffer,
    size_t size,
    const struct xhci_command_snapshot *snapshot);

/* Slot State (bits 31:27 of the Slot Context's dword 0, xHCI 1.1 section
   6.2.2) out of the same device context `xhci_endpoint_state()` reads.  The
   pointer's snapshot published the endpoint's state but not the slot's, and
   "the slot left the Enabled state" is a different failure from "the endpoint
   stopped". */
uint32_t xhci_slot_state(const void *device_context, size_t context_size);
const char *xhci_slot_state_string(uint32_t state);

/*
 * ITEM-458: the two decisions behind "the keyboard loses no character".
 *
 * The driver keeps a bounded queue of decoded reports between the service pass
 * and the console.  A full queue must not overwrite an unread report, so the
 * push is refused and the caller counts the refusal: the count is published as
 * `kbd_dropped`, which is what turns "characters vanished" into a number a
 * reader can see.  `xhci_local_queue_push` answers the decision for a queue
 * with `head`/`tail` indices and `capacity` slots and writes the next head to
 * `next_out` only when the push is allowed.
 *
 * The service gap is the wait between the two most recent reports the pass
 * delivered, in milliseconds, kept as a running maximum: with the ring drained
 * to empty on every pass this is the main loop's own period, and a pass that
 * leaves reports waiting shows up in it.  `last_ticks == 0` means "no report
 * has been serviced yet" and leaves the maximum unchanged.
 */
bool xhci_local_queue_push(uint32_t head, uint32_t tail, uint32_t capacity,
                           uint32_t *next_out);
uint32_t xhci_service_gap_ms(uint32_t maximum_ms, uint64_t last_ticks,
                             uint64_t now_ticks, uint32_t tick_hz);

/*
 * ITEM-479 (СРОЧНО-28, round 17): the stalled-endpoint watchdog's decision.
 *
 * The owner's 2026-09-25 run ended with the pointer armed sixteen deep
 * (`armed=16`), its endpoint context still Running and every completion a
 * SUCCESS - and no report for the last 95 s.  `xhci_watchdog_silence_ms()` is
 * the wait since the role's last progress (a completion or a fresh arm), in
 * milliseconds; an unmeasurable clock answers zero rather than wrapping.
 * `xhci_watchdog_due()` is true only when the role is ready, holds at least one
 * armed transfer and has waited strictly longer than `threshold_ms`, which is
 * the exact state the run named.  Pure, so the host proof drives both sides of
 * the bound and the QEMU proof reads the record the driver publishes.
 */
uint32_t xhci_watchdog_silence_ms(uint64_t last_ticks, uint64_t now_ticks,
                                  uint32_t tick_hz);
bool xhci_watchdog_due(bool ready, uint32_t armed, uint32_t silence_ms,
                       uint32_t threshold_ms);

/*
 * ITEM-482 (round 20): what the silence means, not just that it happened.
 *
 * The round-19 hardware run published seven one-second mouse silences while
 * `armed=16`, every completion code clean - and no way to tell a pointer
 * nobody moved from one that stopped answering while it was being moved.  The
 * owner's complaint ("the mouse works, then freezes, then works again") is
 * the second case, so the record has to name it:
 *
 *   * `xhci_watchdog_active_before()` compares the report clock with the tick
 *     the silence clock restarted at (the last completion or fresh arm): a
 *     report inside `active_window_ticks` of it means the device was in use,
 *     no report or no window means it was merely idle.
 *   * `xhci_report_rate_per_second()` turns a report count and an elapsed
 *     window into reports per second, answering zero for an unmeasurable one.
 *   * `xhci_watchdog_escalate()` is the repair rule: a silence that happened
 *     while the device was active is a fault, and a second active silence is
 *     the point at which a plain re-arm has demonstrably not fixed it, so the
 *     caller escalates the recovery and says so with its own code.
 *
 * All three are pure, so the host proof drives both sides of every bound.
 */
bool xhci_watchdog_active_before(uint64_t last_report_ticks,
                                 uint64_t progress_ticks,
                                 uint64_t active_window_ticks);
uint32_t xhci_report_rate_per_second(uint32_t reports, uint64_t elapsed_ms);
bool xhci_watchdog_escalate(bool active_before, uint32_t active_silences);

#endif
