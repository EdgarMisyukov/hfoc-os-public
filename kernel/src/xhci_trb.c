#include "xhci_trb.h"

/* The scan policy declared in xhci.h is implemented here, next to the rest of
   the pure half, so the host proof drives the same decision code the driver
   runs. */
#include "xhci.h"

#include "kstring.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* All TRB and context encoders are plain little-endian field packing. They are
   intentionally free functions: the host proof calls them directly, and the
   hardware half of the driver (kernel/src/xhci.c) has no second copy of any
   bit position. */

static void write32(void *base, size_t offset, uint32_t value) {
    uint8_t *bytes = (uint8_t *)base;

    bytes[offset] = (uint8_t)(value & 0xFFU);
    bytes[offset + 1U] = (uint8_t)((value >> 8U) & 0xFFU);
    bytes[offset + 2U] = (uint8_t)((value >> 16U) & 0xFFU);
    bytes[offset + 3U] = (uint8_t)((value >> 24U) & 0xFFU);
}

/* ITEM-473: the counterpart the endpoint-state reader needs; it reads through
   the byte pointer so a context at any alignment is safe. */
static uint32_t read32(const void *base, size_t offset) {
    const uint8_t *bytes = (const uint8_t *)base;

    return (uint32_t)bytes[offset] |
           ((uint32_t)bytes[offset + 1U] << 8U) |
           ((uint32_t)bytes[offset + 2U] << 16U) |
           ((uint32_t)bytes[offset + 3U] << 24U);
}

static struct xhci_trb xhci_trb_empty(void) {
    struct xhci_trb trb;

    trb.dword[0] = 0U;
    trb.dword[1] = 0U;
    trb.dword[2] = 0U;
    trb.dword[3] = 0U;
    return trb;
}

static struct xhci_trb xhci_trb_with_type(
    uint8_t cycle,
    enum xhci_trb_type type) {
    struct xhci_trb trb = xhci_trb_empty();

    trb.dword[3] = (uint32_t)((cycle != 0U ? XHCI_TRB_CYCLE : 0U) |
                              ((uint32_t)type << XHCI_TRB_TYPE_SHIFT));
    return trb;
}

void xhci_ring_init(
    struct xhci_ring *ring,
    struct xhci_trb *trbs,
    uint64_t phys,
    uint32_t trb_count,
    uint8_t cycle) {
    uint32_t index;

    if (ring == NULL || trbs == NULL || trb_count < 2U) {
        return;
    }
    for (index = 0U; index < trb_count; ++index) {
        trbs[index] = xhci_trb_empty();
    }
    trbs[trb_count - 1U] = xhci_trb_link(phys, cycle);
    ring->trbs = trbs;
    ring->phys = phys;
    ring->trb_count = trb_count;
    ring->enqueue = 0U;
    ring->cycle = cycle;
}

bool xhci_ring_push(struct xhci_ring *ring, const struct xhci_trb *trb) {
    return xhci_ring_push_index(ring, trb, NULL);
}

bool xhci_ring_push_index(
    struct xhci_ring *ring,
    const struct xhci_trb *trb,
    uint32_t *index) {
    struct xhci_trb entry;

    if (ring == NULL || ring->trbs == NULL || trb == NULL ||
        ring->trb_count < 2U) {
        return false;
    }
    if (ring->enqueue >= ring->trb_count - 1U) {
        /*
         * The last entry is the Link TRB back to the start of the segment. It
         * must carry the producer's current cycle state before the state
         * flips, or the controller would stop following the ring.
         */
        ring->trbs[ring->trb_count - 1U] =
            xhci_trb_link(ring->phys, ring->cycle);
        ring->cycle = (uint8_t)(ring->cycle ^ 1U);
        ring->enqueue = 0U;
    }
    entry = *trb;
    entry.dword[3] = (entry.dword[3] & ~XHCI_TRB_CYCLE) |
                     (ring->cycle != 0U ? XHCI_TRB_CYCLE : 0U);
    ring->trbs[ring->enqueue] = entry;
    if (index != NULL) {
        *index = ring->enqueue;
    }
    ++ring->enqueue;
    return true;
}

const struct xhci_trb *xhci_ring_at(const struct xhci_ring *ring, uint32_t index) {
    if (ring == NULL || ring->trbs == NULL || index >= ring->trb_count) {
        return NULL;
    }
    return &ring->trbs[index];
}

struct xhci_trb xhci_trb_noop(uint8_t cycle) {
    return xhci_trb_with_type(cycle, XHCI_TRB_NORMAL);
}

struct xhci_trb xhci_trb_link(uint64_t ring_physical, uint8_t cycle) {
    struct xhci_trb trb = xhci_trb_with_type(cycle, XHCI_TRB_LINK);

    trb.dword[0] = (uint32_t)(ring_physical & 0xFFFFFFFFULL);
    trb.dword[1] = (uint32_t)(ring_physical >> 32);
    trb.dword[3] |= XHCI_TRB_TC;
    return trb;
}

struct xhci_trb xhci_trb_normal(
    uint8_t cycle,
    uint64_t buffer_physical,
    uint32_t length,
    bool interrupt_on_completion) {
    struct xhci_trb trb = xhci_trb_with_type(cycle, XHCI_TRB_NORMAL);

    trb.dword[0] = (uint32_t)(buffer_physical & 0xFFFFFFFFULL);
    trb.dword[1] = (uint32_t)(buffer_physical >> 32);
    trb.dword[2] = length & 0x0001FFFFU;
    if (interrupt_on_completion) {
        trb.dword[3] |= XHCI_TRB_IOC;
    }
    return trb;
}

struct xhci_trb xhci_trb_interrupt_in(
    uint8_t cycle,
    uint64_t buffer_physical,
    uint32_t length) {
    /*
     * IOC and ISP together: complete on a full buffer *and* on a short packet.
     * The boot mouse's three-byte answer is the case ISP exists for; a device
     * that fills the buffer takes the same path.
     */
    struct xhci_trb trb =
        xhci_trb_normal(cycle, buffer_physical, length, true);

    trb.dword[3] |= XHCI_TRB_ISP;
    return trb;
}

const char *xhci_no_report_reason(
    bool pending,
    uint32_t reports,
    uint32_t transfers) {
    if (reports != 0U) {
        return "OK";
    }
    if (pending) {
        return "INTERRUPT_PENDING";
    }
    if (transfers != 0U) {
        return "COMPLETION_WITHOUT_REPORT";
    }
    return "NOT_ARMED";
}

struct xhci_trb xhci_trb_setup(
    uint8_t cycle,
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    uint16_t length,
    uint8_t transfer_type) {
    struct xhci_trb trb = xhci_trb_with_type(cycle, XHCI_TRB_SETUP_STAGE);

    trb.dword[0] = (uint32_t)request_type |
                   ((uint32_t)request << 8) |
                   ((uint32_t)value << 16);
    trb.dword[1] = (uint32_t)index | ((uint32_t)length << 16);
    /* A Setup Stage TRB always carries the 8 setup bytes inline (IDT). */
    trb.dword[2] = 8U;
    trb.dword[3] |= XHCI_TRB_IDT |
                    ((uint32_t)(transfer_type & 0x03U) << 16);
    return trb;
}

struct xhci_trb xhci_trb_data(
    uint8_t cycle,
    uint64_t buffer_physical,
    uint32_t length,
    bool direction_in,
    bool interrupt_on_completion) {
    struct xhci_trb trb = xhci_trb_with_type(cycle, XHCI_TRB_DATA_STAGE);

    trb.dword[0] = (uint32_t)(buffer_physical & 0xFFFFFFFFULL);
    trb.dword[1] = (uint32_t)(buffer_physical >> 32);
    trb.dword[2] = length & 0x0001FFFFU;
    if (direction_in) {
        trb.dword[3] |= XHCI_TRB_DIR;
    }
    if (interrupt_on_completion) {
        trb.dword[3] |= XHCI_TRB_IOC;
    }
    return trb;
}

struct xhci_trb xhci_trb_status(uint8_t cycle, bool direction_in) {
    struct xhci_trb trb = xhci_trb_with_type(cycle, XHCI_TRB_STATUS_STAGE);

    /*
     * The Status Stage of a control transfer runs opposite to its Data Stage:
     * an IN data stage is acknowledged by an OUT status stage and an OUT data
     * stage by an IN one. The caller passes the status direction directly.
     */
    if (direction_in) {
        trb.dword[3] |= XHCI_TRB_DIR;
    }
    trb.dword[3] |= XHCI_TRB_IOC;
    return trb;
}

struct xhci_trb xhci_trb_enable_slot(uint8_t cycle) {
    return xhci_trb_with_type(cycle, XHCI_TRB_ENABLE_SLOT);
}

struct xhci_trb xhci_trb_disable_slot(uint8_t cycle, uint8_t slot_id) {
    struct xhci_trb trb = xhci_trb_with_type(cycle, XHCI_TRB_DISABLE_SLOT);

    trb.dword[3] |= (uint32_t)slot_id << XHCI_TRB_SLOT_SHIFT;
    return trb;
}

struct xhci_trb xhci_trb_address_device(
    uint8_t cycle,
    uint64_t input_context_physical,
    uint8_t slot_id) {
    struct xhci_trb trb = xhci_trb_with_type(cycle, XHCI_TRB_ADDRESS_DEVICE);

    trb.dword[0] = (uint32_t)(input_context_physical & 0xFFFFFFFFULL);
    trb.dword[1] = (uint32_t)(input_context_physical >> 32);
    /* BSR stays clear: this is an Address Device, not a Block Set Address. */
    trb.dword[3] |= (uint32_t)slot_id << XHCI_TRB_SLOT_SHIFT;
    return trb;
}

struct xhci_trb xhci_trb_configure_endpoint(
    uint8_t cycle,
    uint64_t input_context_physical,
    uint8_t slot_id) {
    struct xhci_trb trb =
        xhci_trb_with_type(cycle, XHCI_TRB_CONFIGURE_ENDPOINT);

    trb.dword[0] = (uint32_t)(input_context_physical & 0xFFFFFFFFULL);
    trb.dword[1] = (uint32_t)(input_context_physical >> 32);
    trb.dword[3] |= (uint32_t)slot_id << XHCI_TRB_SLOT_SHIFT;
    return trb;
}

struct xhci_trb xhci_trb_evaluate_context(
    uint8_t cycle,
    uint64_t input_context_physical,
    uint8_t slot_id) {
    struct xhci_trb trb =
        xhci_trb_with_type(cycle, XHCI_TRB_EVALUATE_CONTEXT);

    trb.dword[0] = (uint32_t)(input_context_physical & 0xFFFFFFFFULL);
    trb.dword[1] = (uint32_t)(input_context_physical >> 32);
    trb.dword[3] |= (uint32_t)slot_id << XHCI_TRB_SLOT_SHIFT;
    return trb;
}

struct xhci_trb xhci_trb_noop_command(uint8_t cycle) {
    return xhci_trb_with_type(cycle, XHCI_TRB_NOOP_COMMAND);
}

uint8_t xhci_trb_type(const struct xhci_trb *trb) {
    if (trb == NULL) {
        return 0U;
    }
    return (uint8_t)((trb->dword[3] & XHCI_TRB_TYPE_MASK) >>
                     XHCI_TRB_TYPE_SHIFT);
}

bool xhci_trb_cycle(const struct xhci_trb *trb) {
    if (trb == NULL) {
        return false;
    }
    return (trb->dword[3] & XHCI_TRB_CYCLE) != 0U;
}

bool xhci_event_decode(const struct xhci_trb *trb, struct xhci_event *event) {
    uint32_t dword2;
    uint32_t dword3;
    uint8_t type;

    if (trb == NULL || event == NULL) {
        return false;
    }
    dword2 = trb->dword[2];
    dword3 = trb->dword[3];
    type = (uint8_t)((dword3 & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT);

    event->type = (enum xhci_trb_type)type;
    event->completion_code = 0U;
    event->slot_id = 0U;
    event->endpoint_id = 0U;
    event->port_id = 0U;
    event->transfer_length = 0U;
    event->parameter_low = trb->dword[0];
    event->parameter = ((uint64_t)trb->dword[1] << 32) | (uint64_t)trb->dword[0];
    for (uint32_t dword = 0U; dword < 4U; ++dword) {
        event->raw[dword] = trb->dword[dword];
    }

    switch (type) {
    case XHCI_TRB_TRANSFER_EVENT:
        event->completion_code = (uint8_t)(dword2 >> 24);
        event->transfer_length = dword2 & 0x00FFFFFFU;
        event->slot_id = (uint8_t)(dword3 >> XHCI_TRB_SLOT_SHIFT);
        event->endpoint_id = (uint8_t)((dword3 & XHCI_TRB_EP_MASK) >>
                                       XHCI_TRB_EP_SHIFT);
        return true;
    case XHCI_TRB_COMMAND_COMPLETION:
        event->completion_code = (uint8_t)(dword2 >> 24);
        /* The completion parameter of a slot command is the Slot ID. */
        event->parameter_low = dword2 & 0x00FFFFFFU;
        event->slot_id = (uint8_t)(dword3 >> XHCI_TRB_SLOT_SHIFT);
        return true;
    case XHCI_TRB_PORT_STATUS_CHANGE:
        event->completion_code = (uint8_t)(dword2 >> 24);
        event->port_id = (uint16_t)(trb->dword[0] & 0x0000FFFFU);
        return true;
    case XHCI_TRB_HOST_CONTROLLER_EVENT:
        /*
         * ITEM-473: a controller event carries only a completion code - no
         * slot and no endpoint - and Event Ring Full (21) is reported this way
         * (xHCI 1.1 table 6-9 and table 6-90).  Decoding it lets the service
         * pass count the one error that explained the silent pointer.
         */
        event->completion_code = (uint8_t)(dword2 >> 24);
        return true;
    default:
        return false;
    }
}

/* ITEM-579: the endpoint-repair commands.  The endpoint id lives in dword 2
   bits 20:16 for both, and the slot in dword 3 (xHCI 1.1 table 6-11). */
struct xhci_trb xhci_trb_reset_endpoint(uint8_t cycle, uint8_t slot_id,
                                       uint8_t dci) {
    struct xhci_trb trb = xhci_trb_with_type(cycle, XHCI_TRB_RESET_ENDPOINT);

    trb.dword[2] = ((uint32_t)dci & 0x1FU) << 16;
    trb.dword[3] |= ((uint32_t)slot_id & 0xFFU) << XHCI_TRB_SLOT_SHIFT;
    return trb;
}

struct xhci_trb xhci_trb_set_tr_dequeue(uint8_t cycle, uint8_t slot_id,
                                       uint8_t dci,
                                       uint64_t dequeue_physical) {
    struct xhci_trb trb = xhci_trb_with_type(cycle, XHCI_TRB_SET_TR_DEQUEUE);

    /* The Dequeue Cycle State bit: the caller republishes a ring that starts at
       cycle 1, and the controller must read the first TRB with that cycle. */
    trb.dword[0] = (uint32_t)(dequeue_physical & 0xFFFFFFF0ULL) | 1U;
    trb.dword[1] = (uint32_t)(dequeue_physical >> 32);
    trb.dword[2] = ((uint32_t)dci & 0x1FU) << 16;
    trb.dword[3] |= ((uint32_t)slot_id & 0xFFU) << XHCI_TRB_SLOT_SHIFT;
    return trb;
}

const char *xhci_trb_type_string(uint8_t type) {
    switch (type) {
    case XHCI_TRB_NORMAL:
        return "NORMAL";
    case XHCI_TRB_SETUP_STAGE:
        return "SETUP";
    case XHCI_TRB_DATA_STAGE:
        return "DATA";
    case XHCI_TRB_STATUS_STAGE:
        return "STATUS";
    case XHCI_TRB_LINK:
        return "LINK";
    case XHCI_TRB_ENABLE_SLOT:
        return "ENABLE_SLOT";
    case XHCI_TRB_DISABLE_SLOT:
        return "DISABLE_SLOT";
    case XHCI_TRB_ADDRESS_DEVICE:
        return "ADDRESS_DEVICE";
    case XHCI_TRB_CONFIGURE_ENDPOINT:
        return "CONFIGURE_ENDPOINT";
    case XHCI_TRB_EVALUATE_CONTEXT:
        return "EVALUATE_CONTEXT";
    case XHCI_TRB_RESET_ENDPOINT:
        return "RESET_ENDPOINT";
    case XHCI_TRB_SET_TR_DEQUEUE:
        return "SET_TR_DEQUEUE";
    case XHCI_TRB_NOOP_COMMAND:
        return "NOOP_COMMAND";
    case XHCI_TRB_TRANSFER_EVENT:
        return "TRANSFER_EVENT";
    case XHCI_TRB_COMMAND_COMPLETION:
        return "COMMAND_COMPLETION";
    case XHCI_TRB_PORT_STATUS_CHANGE:
        return "PORT_STATUS_CHANGE";
    case XHCI_TRB_HOST_CONTROLLER_EVENT:
        return "HOST_CONTROLLER_EVENT";
    default:
        return "UNKNOWN";
    }
}

const char *xhci_completion_code_string(uint8_t code) {
    switch (code) {
    case 0U:
        return "INVALID";
    case XHCI_COMPLETION_SUCCESS:
        return "SUCCESS";
    case 2U:
        return "DATA_BUFFER_ERROR";
    case 3U:
        return "BABBLE_DETECTED";
    case 4U:
        return "USB_TRANSACTION_ERROR";
    case 5U:
        return "TRB_ERROR";
    case 6U:
        return "STALL";
    case 9U:
        return "BANDWIDTH_ERROR";
    case 11U:
        return "SLOT_NOT_ENABLED";
    case 12U:
        return "ENDPOINT_NOT_ENABLED";
    case XHCI_COMPLETION_SHORT_PACKET:
        return "SHORT_PACKET";
    /*
     * ITEM-420: xHCI 1.1 table 6-90.  The names from 16 upwards were carried
     * two codes too high, so the completion the acceptance host returned for
     * the boot keyboard's Evaluate Context command - 19, a Context State Error,
     * the controller saying the command itself was illegal for the slot - was
     * published as `EVENT_RING_FULL`, an error the event ring never had.  The
     * record sent the investigation after a ring that was fine.
     */
    case 16U:
        return "VF_EVENT_RING_FULL_ERROR";
    case 17U:
        return "PARAMETER_ERROR";
    case 18U:
        return "BANDWIDTH_OVERRUN_ERROR";
    case 19U:
        return "CONTEXT_STATE_ERROR";
    case 20U:
        return "NO_PING_RESPONSE_ERROR";
    case 21U:
        return "EVENT_RING_FULL_ERROR";
    case 22U:
        return "INCOMPATIBLE_DEVICE_ERROR";
    case 23U:
        return "MISSED_SERVICE_ERROR";
    case 24U:
        return "COMMAND_RING_STOPPED";
    case 25U:
        return "COMMAND_ABORTED";
    case 26U:
        return "STOPPED";
    case 27U:
        return "STOPPED_LENGTH_INVALID";
    case 28U:
        return "STOPPED_SHORT_PACKET";
    case 29U:
        return "MAX_EXIT_LATENCY_TOO_LARGE";
    case 30U:
        return "ISOCH_BUFFER_OVERRUN";
    case 31U:
        return "EVENT_LOST_ERROR";
    case 32U:
        return "UNDEFINED_ERROR";
    case 33U:
        return "INVALID_STREAM_ID_ERROR";
    case 34U:
        return "SECONDARY_BANDWIDTH_ERROR";
    case 35U:
        return "SPLIT_TRANSACTION_ERROR";
    default:
        return "UNKNOWN";
    }
}

/*
 * ITEM-395/406/452: the status names the boot records and the console publish.
 * The table lives in the pure half so the host proof can pin every reason a
 * record can carry - including ITEM-452's ROLE_TAKEN, which is not a refusal
 * the controller made but the statement that the machine already holds the
 * role the devices on it would fill.
 */
const char *xhci_status_string(enum xhci_status status) {
    switch (status) {
    case XHCI_STATUS_OK:
        return "OK";
    case XHCI_STATUS_CONTROLLER_ABSENT:
        return "CONTROLLER_ABSENT";
    case XHCI_STATUS_NO_MEMORY_BAR:
        return "NO_MEMORY_BAR";
    case XHCI_STATUS_MMIO_MAP_FAILED:
        return "MMIO_MAP_FAILED";
    case XHCI_STATUS_DMA_UNAVAILABLE:
        return "DMA_UNAVAILABLE";
    case XHCI_STATUS_HALT_TIMEOUT:
        return "HALT_TIMEOUT";
    case XHCI_STATUS_RESET_TIMEOUT:
        return "RESET_TIMEOUT";
    case XHCI_STATUS_COMMAND_FAILED:
        return "COMMAND_FAILED";
    case XHCI_STATUS_NO_DEVICE:
        return "NO_DEVICE";
    case XHCI_STATUS_UNSUPPORTED_SPEED:
        return "UNSUPPORTED_SPEED";
    case XHCI_STATUS_ENUMERATION_FAILED:
        return "ENUMERATION_FAILED";
    case XHCI_STATUS_NOT_A_KEYBOARD:
        return "NOT_A_KEYBOARD";
    case XHCI_STATUS_NOT_A_MOUSE:
        return "NOT_A_MOUSE";
    case XHCI_STATUS_ROLE_TAKEN:
        return "ROLE_TAKEN";
    case XHCI_STATUS_KEYBOARD_LOST:
        return "KEYBOARD_LOST";
    default:
        return "UNKNOWN";
    }
}

const char *xhci_speed_string(uint8_t speed) {
    switch (speed) {
    case XHCI_SPEED_FULL:
        return "FULL";
    case XHCI_SPEED_LOW:
        return "LOW";
    case XHCI_SPEED_HIGH:
        return "HIGH";
    case XHCI_SPEED_SUPER:
        return "SUPER";
    case XHCI_SPEED_SUPER_PLUS:
        return "SUPER_PLUS";
    default:
        return "UNKNOWN";
    }
}

uint16_t xhci_speed_ep0_max_packet(uint8_t speed) {
    switch (speed) {
    case XHCI_SPEED_LOW:
    case XHCI_SPEED_FULL:
        /* USB 2.0 section 5.5.3: the default control pipe of a low- or
           full-speed device is 8 bytes unless the device says otherwise. */
        return 8U;
    case XHCI_SPEED_HIGH:
        /* USB 2.0 section 5.5.3: high-speed default control pipe is 64. */
        return 64U;
    case XHCI_SPEED_SUPER:
    case XHCI_SPEED_SUPER_PLUS:
        /*
         * ITEM-438: USB 3.0 section 9.6.1 gives a SuperSpeed device a
         * 512-byte default control pipe (bMaxPacketSize0 = 9, the exponent),
         * and the xHCI slot context carries the same speed IDs 4 and 5 the
         * port register reported.  The 2026-09-17 hardware run read
         * `reason=UNSUPPORTED_SPEED detail=P2_SPEED_SUPER` for a keyboard
         * plugged into a blue port: the device was addressable all along, and
         * this driver refused it.  The endpoint-context encoding below is the
         * same one every other speed uses - only the packet ceiling differs -
         * so a SuperSpeed keyboard enumerates with this table.
         */
        return 512U;
    default:
        return 0U;
    }
}

uint8_t xhci_endpoint_interval(uint8_t speed, uint8_t b_interval) {
    uint32_t frames;

    switch (speed) {
    case XHCI_SPEED_HIGH:
        /* High speed: this field is an exponent there too, and bInterval
           already carries one (1..16).  The value is programmed unchanged;
           ITEM-438 changed only the low- and full-speed branch, because both
           keyboards on the acceptance host are those speeds and a high-speed
           device has no proof here that would say more. */
        frames = b_interval;
        if (frames == 0U) {
            frames = 1U;
        }
        if (frames > 16U) {
            frames = 16U;
        }
        return (uint8_t)frames;
    case XHCI_SPEED_SUPER:
    case XHCI_SPEED_SUPER_PLUS:
        /*
         * ITEM-438: a SuperSpeed interrupt endpoint's bInterval is already the
         * field's own value - xHCI 1.2 section 6.2.3.6 gives it the period
         * 2^(Interval-1) x 125 us with Interval in 1..16 - so it is clamped
         * and programmed unchanged, exactly like the high-speed branch.  The
         * 2026-09-17 run's SuperSpeed port was refused before this table was
         * ever consulted.
         */
        frames = b_interval;
        if (frames == 0U) {
            frames = 1U;
        }
        if (frames > 16U) {
            frames = 16U;
        }
        return (uint8_t)frames;
    case XHCI_SPEED_LOW:
    case XHCI_SPEED_FULL: {
        /*
         * ITEM-438: low and full speed do *not* put a microframe count here.
         * xHCI 1.2 section 6.2.3.6 gives a low- or full-speed interrupt
         * endpoint the period 2^Interval x 125 us with the field in 3..10, and
         * the descriptor's bInterval is in 1 ms frames; the conversion is
         * therefore "the exponent of the microframe count", rounded down, so
         * that 1 ms is 3 (8 microframes), 10 ms is 6 (64 microframes, 8 ms)
         * and 255 ms clamps to 10 (1024 microframes, 128 ms).  The Linux
         * xhci driver (`xhci_parse_frame_interval()`, `fls(bInterval * 8) - 1`
         * clamped to 3..10), Haiku's (`xhci 1.2 6.2.3.6`: highest set bit plus
         * three) and QEMU's model (`1 << Interval`) agree on both the formula
         * and the range.
         *
         * The 2026-09-17 hardware run's receiver declares bInterval 1, and the
         * driver used to program bInterval * 8 = 8 here: a microframe count
         * where the controller reads an exponent, so the endpoint was polled
         * every 2^8 x 125 us = 32 ms instead of every millisecond.  Nothing in
         * QEMU could show it - its model only exponentiates whatever it is
         * given, and every proof's budget is far longer than either period -
         * which is why the table is pinned on the host.
         */
        uint32_t microframes = b_interval;
        uint8_t exponent = XHCI_INTERVAL_FS_MIN;

        if (microframes == 0U) {
            microframes = 1U;
        }
        if (microframes > 255U) {
            microframes = 255U;
        }
        while (microframes > 1U && exponent < XHCI_INTERVAL_FS_MAX) {
            microframes >>= 1U;
            ++exponent;
        }
        return exponent;
    }
    default:
        /* SuperSpeed and above use the exponent encoding with a different
           ceiling and a speed this driver does not address at all. */
        return 0U;
    }
}

uint32_t xhci_portsc_neutral(uint32_t portsc) {
    return portsc & XHCI_PORTSC_WRITE_PRESERVE;
}

uint32_t xhci_portsc_claim(uint32_t portsc) {
    return xhci_portsc_neutral(portsc) | XHCI_PORTSC_PP;
}

/* Validate the capability offset independently of the MMIO startup sequence. */
bool xhci_caplength_valid(uint32_t caplength, uint32_t mmio_bytes) {
    const uint32_t operational_fixed_bytes = 0x3CU; /* CONFIG at 0x38 + 4. */

    if (caplength < 0x20U || caplength > 0xFCU ||
        (caplength & 3U) != 0U) {
        return false;
    }
    return mmio_bytes == 0U ||
           (mmio_bytes >= operational_fixed_bytes &&
            caplength <= mmio_bytes - operational_fixed_bytes);
}

/*
 * ITEM-407: the slot bound, the array that must be sized for the controller and
 * the table that is sized for what this driver turns on.
 */
bool xhci_max_slots_valid(uint32_t max_slots) {
    return max_slots != 0U && max_slots <= XHCI_MAX_SLOTS;
}

bool xhci_max_ports_valid(uint32_t max_ports) {
    return max_ports != 0U && max_ports <= XHCI_MAX_PORTS;
}

uint32_t xhci_slots_enabled(uint32_t max_slots) {
    if (!xhci_max_slots_valid(max_slots)) {
        return 0U;
    }
    return max_slots < XHCI_DRIVER_SLOTS ? max_slots : XHCI_DRIVER_SLOTS;
}

bool xhci_slot_within_driver_bound(uint32_t slot_id) {
    /* Slot 0 does not exist: slots are numbered from 1. */
    return slot_id >= 1U && slot_id <= XHCI_DRIVER_SLOTS;
}

size_t xhci_dcbaa_bytes(uint32_t max_slots) {
    if (!xhci_max_slots_valid(max_slots)) {
        return 0U;
    }
    /* MaxSlots+1 entries: entry 0 is the scratchpad buffer array (xHCI 1.1
       section 6.1). The multiplication is done in the wider type so the
       addition cannot wrap for any value the field can carry. */
    return ((size_t)max_slots + 1U) * sizeof(uint64_t);
}

uint32_t xhci_port_evidence_count(uint32_t max_ports) {
    if (!xhci_max_ports_valid(max_ports)) {
        return 0U;
    }
    return max_ports < XHCI_PORT_EVIDENCE_MAX ? max_ports
                                              : XHCI_PORT_EVIDENCE_MAX;
}

static char *xhci_format_hex32(char *cursor, const char *end, uint32_t value) {
    static const char digits[] = "0123456789ABCDEF";

    if (cursor + 10 > end) {
        return cursor;
    }
    *cursor++ = '0';
    *cursor++ = 'x';
    for (uint32_t shift = 28U;; shift -= 4U) {
        *cursor++ = digits[(value >> shift) & 0xFU];
        if (shift == 0U) {
            break;
        }
    }
    return cursor;
}

size_t xhci_format_port_evidence(
    char *buffer,
    size_t size,
    const uint32_t *portsc,
    uint32_t ports) {
    char *cursor = buffer;
    const char *end;
    size_t written = 0U;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (portsc == NULL) {
        return 0U;
    }
    if (ports > XHCI_PORT_EVIDENCE_MAX) {
        ports = XHCI_PORT_EVIDENCE_MAX;
    }
    /* One value is at most ten characters plus a separator, and the string is
       NUL-terminated, so the last position stays reserved. */
    end = buffer + size - 1U;
    for (uint32_t index = 0U; index < ports; ++index) {
        char value[12];
        char *value_end = xhci_format_hex32(value, value + sizeof(value),
                                            portsc[index]);

        if (value_end == value || cursor + (size_t)(value_end - value) > end) {
            break;
        }
        for (char *source = value; source < value_end; ++source) {
            *cursor++ = *source;
        }
        ++written;
        if (index + 1U < ports && cursor < end) {
            /* A trailing comma means the list itself was cut short. */
            *cursor++ = ',';
        }
    }
    *cursor = '\0';
    return written;
}

uint32_t xhci_ports_powered_mask(const uint32_t *portsc, uint32_t ports) {
    uint32_t mask = 0U;

    if (portsc == NULL) {
        return 0U;
    }
    if (ports > XHCI_PORT_EVIDENCE_MAX) {
        ports = XHCI_PORT_EVIDENCE_MAX;
    }
    for (uint32_t index = 0U; index < ports; ++index) {
        if ((portsc[index] & XHCI_PORTSC_PP) != 0U) {
            mask |= 1U << index;
        }
    }
    return mask;
}

/*
 * ITEM-453: the port link state, the states that cannot report a connect, and
 * the write that brings one back to RxDetect.
 *
 * xHCI 1.1 table 5-27 gives PORTSC.PLS (bits 8:5).  For the scan the field is
 * not decoration: a port in the Disabled (4), Inactive (6), Compliance (10) or
 * Test Mode (11) state does not detect an attach, so it answers CCS = 0 with a
 * device plugged into it, and a scan that reads CCS once and moves on publishes
 * "no device" for a port it never brought up.  The four states above therefore
 * answer false from xhci_port_link_samples_connect(); every other defined state
 * either samples the connect itself (RxDetect, Polling, Recovery, U0..U3,
 * Resume, Hot Reset) or is a running link, and a reserved value is not trusted
 * to sample anything.
 */
uint32_t xhci_port_link_state(uint32_t portsc) {
    return (portsc & XHCI_PORTSC_PLS_MASK) >> XHCI_PORTSC_PLS_SHIFT;
}

const char *xhci_port_link_state_string(uint32_t state) {
    switch (state) {
    case XHCI_PORT_LINK_U0:
        return "U0";
    case XHCI_PORT_LINK_U1:
        return "U1";
    case XHCI_PORT_LINK_U2:
        return "U2";
    case XHCI_PORT_LINK_U3:
        return "U3";
    case XHCI_PORT_LINK_DISABLED:
        return "Disabled";
    case XHCI_PORT_LINK_RXDETECT:
        return "RxDetect";
    case XHCI_PORT_LINK_INACTIVE:
        return "Inactive";
    case XHCI_PORT_LINK_POLLING:
        return "Polling";
    case XHCI_PORT_LINK_RECOVERY:
        return "Recovery";
    case XHCI_PORT_LINK_HOT_RESET:
        return "HotReset";
    case XHCI_PORT_LINK_COMPLIANCE:
        return "Compliance";
    case XHCI_PORT_LINK_TEST_MODE:
        return "TestMode";
    case XHCI_PORT_LINK_RESUME:
        return "Resume";
    default:
        return "UNKNOWN";
    }
}

bool xhci_port_link_samples_connect(uint32_t state) {
    switch (state) {
    case XHCI_PORT_LINK_U0:
    case XHCI_PORT_LINK_U1:
    case XHCI_PORT_LINK_U2:
    case XHCI_PORT_LINK_U3:
    case XHCI_PORT_LINK_RXDETECT:
    case XHCI_PORT_LINK_POLLING:
    case XHCI_PORT_LINK_RECOVERY:
    case XHCI_PORT_LINK_HOT_RESET:
    case XHCI_PORT_LINK_RESUME:
        return true;
    case XHCI_PORT_LINK_DISABLED:
    case XHCI_PORT_LINK_INACTIVE:
    case XHCI_PORT_LINK_COMPLIANCE:
    case XHCI_PORT_LINK_TEST_MODE:
        return false;
    default:
        /* A reserved value is not evidence that the port is watching. */
        return false;
    }
}

/* ITEM-545: a port the controller just reset may still be training its
   SuperSpeed link; until it reports a connect, or stops sampling one, the
   controller's window has a reason to keep waiting. */
bool xhci_port_connect_pending(uint32_t portsc) {
    if ((portsc & XHCI_PORTSC_CCS) != 0U) {
        return false;
    }
    return xhci_port_link_samples_connect(xhci_port_link_state(portsc));
}

uint32_t xhci_portsc_link_kick(uint32_t portsc) {
    /*
     * The link state field is write-preserve, so the value the port reported
     * has to be cleared before RxDetect is written: a port in U3 reports
     * PLS = 3 (0x60), and OR-ing RxDetect (0xA0) into that would ask for
     * Polling (7) instead.  LWS is the strobe that makes the write take
     * effect, and PED and the write-one-to-clear change bits stay out exactly
     * as in every other PORTSC write of this driver.
     */
    return (xhci_portsc_neutral(portsc) & ~XHCI_PORTSC_PLS_MASK) |
           XHCI_PORTSC_PP | XHCI_PORTSC_LWS |
           (XHCI_PORT_LINK_RXDETECT << XHCI_PORTSC_PLS_SHIFT);
}

void xhci_port_bring_up_plan(uint32_t portsc, struct xhci_port_bring_up *plan) {
    if (plan == NULL) {
        return;
    }
    plan->was = portsc;
    plan->link = xhci_port_link_state(portsc);
    plan->powered = (portsc & XHCI_PORTSC_PP) != 0U;
    plan->connected = (portsc & XHCI_PORTSC_CCS) != 0U;
    plan->samples = xhci_port_link_samples_connect(plan->link);
    plan->power_on = !plan->powered;
    plan->link_kick = !plan->samples;
    /*
     * A port that was written to is given its connect-debounce window before
     * the scan believes the read that follows; a port the driver did not touch
     * is covered by the controller's own window after the reset (the driver
     * runs one before it reads any port), and a connected port needs no window
     * at all - the whole point of the wait is a connect that has not risen yet.
     */
    plan->settle_ms = (!plan->connected && (plan->power_on || plan->link_kick))
                          ? XHCI_PORT_SETTLE_MS
                          : 0U;
}

/* One decimal field, right-aligned by the caller's separator.  NULL means the
   field did not fit; every composer below stops on the first NULL, so a record
   is either whole or not written at all. */
static char *xhci_record_u32(char *cursor, const char *end, uint32_t value) {
    char digits[10];
    size_t length = 0U;

    if (cursor == NULL) {
        return NULL;
    }
    do {
        digits[length++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U && length < sizeof(digits));
    if (cursor + length > end) {
        return NULL;
    }
    while (length > 0U) {
        *cursor++ = digits[--length];
    }
    return cursor;
}

static char *xhci_record_text(char *cursor, const char *end, const char *text) {
    if (cursor == NULL || text == NULL) {
        return NULL;
    }
    while (*text != '\0') {
        if (cursor + 1 > end) {
            return NULL;
        }
        *cursor++ = *text++;
    }
    return cursor;
}

/* The shared hexadecimal writer fails by writing nothing when its ten bytes do
   not fit, so the overflow case is the cursor coming back unchanged. */
/* Two hexadecimal digits: the endpoint address as `[INPUT-DIAG]` prints it. */
static char *xhci_record_hex2(char *cursor, const char *end, uint8_t value) {
    static const char digits[] = "0123456789ABCDEF";

    if (cursor == NULL || cursor + 2 > end) {
        return NULL;
    }
    *cursor++ = digits[(value >> 4) & 0x0FU];
    *cursor++ = digits[value & 0x0FU];
    return cursor;
}

static char *xhci_record_hex32(char *cursor, const char *end, uint32_t value) {
    char *next;

    if (cursor == NULL || cursor + 10 > end) {
        return NULL;
    }
    next = xhci_format_hex32(cursor, end, value);
    return next == cursor ? NULL : next;
}

/* MOUSE-DIAG (2026-09-27): the 64-bit counters the stall snapshot carries -
   CRCR, ERDP and ERSTBA are 64 bits wide and the 32-bit writer would cut them.
   Written as `0x` plus sixteen digits, so two journals compare byte for byte. */
static char *xhci_record_hex64(char *cursor, const char *end, uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    int shift;

    if (cursor == NULL || cursor + 18 > end) {
        return NULL;
    }
    *cursor++ = '0';
    *cursor++ = 'x';
    for (shift = 60; shift >= 0; shift -= 4) {
        *cursor++ = digits[(value >> (unsigned int)shift) & 0x0FU];
    }
    return cursor;
}

static char *xhci_record_u64(char *cursor, const char *end, uint64_t value) {
    char digits[20];
    size_t length = 0U;

    if (cursor == NULL) {
        return NULL;
    }
    do {
        digits[length++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U && length < sizeof(digits));
    if (cursor + length > end) {
        return NULL;
    }
    while (length > 0U) {
        *cursor++ = digits[--length];
    }
    return cursor;
}

/* One PORTSC bit as the record's own 0/1 field. */
static char *xhci_record_bit(
    char *cursor,
    const char *end,
    const char *name,
    uint32_t value,
    uint32_t mask) {
    cursor = xhci_record_text(cursor, end, name);
    return xhci_record_u32(cursor, end, (value & mask) != 0U ? 1U : 0U);
}

static char *xhci_record_location(
    char *cursor,
    const char *end,
    uint8_t bus,
    uint8_t device,
    uint8_t function) {
    static const char digits[] = "0123456789ABCDEF";
    char location[9];

    location[0] = digits[(bus >> 4) & 0xFU];
    location[1] = digits[bus & 0xFU];
    location[2] = ':';
    location[3] = digits[(device >> 4) & 0xFU];
    location[4] = digits[device & 0xFU];
    location[5] = '.';
    location[6] = digits[function & 0xFU];
    location[7] = '\0';
    return xhci_record_text(cursor, end, location);
}

/* Copy a whole composed record into the caller's buffer, or leave it empty: a
   truncated line is worse than no line, because the console mirror and the
   durable journal would both carry half a record. */
static size_t xhci_record_finish(
    char *buffer,
    size_t size,
    const char *line,
    const char *cursor) {
    size_t length;

    if (cursor == NULL) {
        buffer[0] = '\0';
        return 0U;
    }
    length = (size_t)(cursor - line);
    if (length + 1U > size) {
        buffer[0] = '\0';
        return 0U;
    }
    for (size_t index = 0U; index < length; ++index) {
        buffer[index] = line[index];
    }
    buffer[length] = '\0';
    return length;
}

/* MOUSE-DIAG step 3: the TR Dequeue Pointer and its cycle state.  The index
   is the DCI itself, exactly as `xhci_endpoint_state()` reads it: a device
   context's entry 0 is the slot context and entry n is DCI n. */
bool xhci_endpoint_dequeue(
    const void *device_context,
    size_t context_size,
    uint8_t dci,
    uint64_t *pointer_out,
    uint32_t *cycle_out) {
    uint64_t raw;

    if (pointer_out != NULL) {
        *pointer_out = 0U;
    }
    if (cycle_out != NULL) {
        *cycle_out = 0U;
    }
    if (device_context == NULL || context_size == 0U) {
        return false;
    }
    raw = (uint64_t)read32(device_context,
                           xhci_context_offset(context_size, dci) + 8U) |
          ((uint64_t)read32(device_context,
                            xhci_context_offset(context_size, dci) + 12U) << 32U);
    if (pointer_out != NULL) {
        *pointer_out = raw & ~UINT64_C(0x0F);
    }
    if (cycle_out != NULL) {
        *cycle_out = (uint32_t)(raw & 0x01U);
    }
    return true;
}

size_t xhci_format_ep_state_record(
    char *buffer,
    size_t size,
    const struct xhci_ep_state *state) {
    char line[XHCI_EP_STATE_RECORD_BYTES];
    const char *end = line + sizeof(line) - 1U;
    char *cursor = line;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (state == NULL) {
        return 0U;
    }
    cursor = xhci_record_text(cursor, end, "[EP-STATE] role=");
    cursor = xhci_record_text(cursor, end,
                              state->role == 1U ? "mouse" : "keyboard");
    cursor = xhci_record_text(cursor, end, " ep=0x");
    cursor = xhci_record_hex2(cursor, end, (uint8_t)state->endpoint_address);
    cursor = xhci_record_text(cursor, end, " dci=");
    cursor = xhci_record_u32(cursor, end, state->dci);
    cursor = xhci_record_text(cursor, end, " state=");
    cursor = xhci_record_u32(cursor, end, state->endpoint_state);
    cursor = xhci_record_text(cursor, end, " armed=");
    cursor = xhci_record_u32(cursor, end, state->armed);
    cursor = xhci_record_text(cursor, end, " pending=");
    cursor = xhci_record_u32(cursor, end, state->pending);
    cursor = xhci_record_text(cursor, end, " reposted=");
    cursor = xhci_record_u32(cursor, end, state->reposted);
    cursor = xhci_record_text(cursor, end, " code=");
    cursor = xhci_record_u32(cursor, end, state->last_completion_code);
    cursor = xhci_record_text(cursor, end, " last_ms=");
    cursor = xhci_record_u32(cursor, end, state->last_completion_ms);
    return xhci_record_finish(buffer, size, line, cursor);
}

/* The ring half of the snapshot, on its own line: two 64-bit pointers and the
   ring geometry do not fit beside the context fields inside the console's
   320-byte record bound. */
size_t xhci_format_ep_state_tail_record(
    char *buffer,
    size_t size,
    const struct xhci_ep_state *state) {
    char line[XHCI_EP_STATE_RECORD_BYTES];
    const char *end = line + sizeof(line) - 1U;
    char *cursor = line;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (state == NULL) {
        return 0U;
    }
    cursor = xhci_record_text(cursor, end, "[EP-STATE-TAIL] role=");
    cursor = xhci_record_text(cursor, end,
                              state->role == 1U ? "mouse" : "keyboard");
    cursor = xhci_record_text(cursor, end, " ring=");
    cursor = xhci_record_hex64(cursor, end, state->ring_base);
    cursor = xhci_record_text(cursor, end, " dq=");
    cursor = xhci_record_hex64(cursor, end, state->dequeue_pointer);
    cursor = xhci_record_text(cursor, end, " dc=");
    cursor = xhci_record_u32(cursor, end, state->dequeue_cycle);
    cursor = xhci_record_text(cursor, end, " enq=");
    cursor = xhci_record_u32(cursor, end, state->enqueue);
    cursor = xhci_record_text(cursor, end, "/");
    cursor = xhci_record_u32(cursor, end, state->ring_trbs);
    cursor = xhci_record_text(cursor, end, " pc=");
    cursor = xhci_record_u32(cursor, end, state->producer_cycle);
    return xhci_record_finish(buffer, size, line, cursor);
}

size_t xhci_format_timing_check_record(
    char *buffer,
    size_t size,
    const struct xhci_timing_check *check) {
    char line[XHCI_TIMING_CHECK_RECORD_BYTES];
    const char *end = line + sizeof(line) - 1U;
    char *cursor = line;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (check == NULL) {
        return 0U;
    }
    cursor = xhci_record_text(cursor, end, "[TIMING-CHECK] start_tsc=");
    cursor = xhci_record_hex64(cursor, end, check->start_tsc);
    cursor = xhci_record_text(cursor, end, " end_tsc=");
    cursor = xhci_record_hex64(cursor, end, check->end_tsc);
    cursor = xhci_record_text(cursor, end, " elapsed_cycles=");
    cursor = xhci_record_u64(cursor, end, check->elapsed_cycles);
    cursor = xhci_record_text(cursor, end, " ticks_start=");
    cursor = xhci_record_u64(cursor, end, check->ticks_start);
    cursor = xhci_record_text(cursor, end, " ticks_end=");
    cursor = xhci_record_u64(cursor, end, check->ticks_end);
    cursor = xhci_record_text(cursor, end, " real_start=");
    cursor = xhci_record_u64(cursor, end, check->real_seconds_start);
    cursor = xhci_record_text(cursor, end, " real_end=");
    cursor = xhci_record_u64(cursor, end, check->real_seconds_end);
    cursor = xhci_record_text(cursor, end, " tick_hz=");
    cursor = xhci_record_u64(cursor, end, check->tick_hz);
    cursor = xhci_record_text(cursor, end, " raw_elapsed_us=");
    cursor = xhci_record_u64(cursor, end, check->raw_elapsed_us);
    return xhci_record_finish(buffer, size, line, cursor);
}

/* The accounting half, so the raw anchors and the metrics each stay inside the
   320-byte bound with every field at full width. */
size_t xhci_format_timing_check_tail_record(
    char *buffer,
    size_t size,
    const struct xhci_timing_check *check) {
    char line[XHCI_TIMING_CHECK_RECORD_BYTES];
    const char *end = line + sizeof(line) - 1U;
    char *cursor = line;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (check == NULL) {
        return 0U;
    }
    cursor = xhci_record_text(cursor, end, "[TIMING-CHECK-TAIL] sum_phase_us=");
    cursor = xhci_record_u64(cursor, end, check->sum_phase_us);
    cursor = xhci_record_text(cursor, end, " work_us=");
    cursor = xhci_record_u64(cursor, end, check->work_us);
    cursor = xhci_record_text(cursor, end, " turn_us=");
    cursor = xhci_record_u64(cursor, end, check->turn_us);
    cursor = xhci_record_text(cursor, end, " worst_us=");
    cursor = xhci_record_u64(cursor, end, check->worst_us);
    cursor = xhci_record_text(cursor, end, " iter_max_us=");
    cursor = xhci_record_u64(cursor, end, check->iter_max_us);
    cursor = xhci_record_text(cursor, end, " timing_accounting_mismatch=");
    cursor = xhci_record_u32(cursor, end, check->mismatch);
    return xhci_record_finish(buffer, size, line, cursor);
}

size_t xhci_format_bootlog_flush_record(
    char *buffer,
    size_t size,
    const struct xhci_bootlog_flush *flush) {
    char line[XHCI_BOOTLOG_FLUSH_RECORD_BYTES];
    const char *end = line + sizeof(line) - 1U;
    char *cursor = line;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (flush == NULL) {
        return 0U;
    }
    cursor = xhci_record_text(cursor, end, "[BOOTLOG-FLUSH] records=");
    cursor = xhci_record_u32(cursor, end, flush->records);
    cursor = xhci_record_text(cursor, end, " text_bytes=");
    cursor = xhci_record_u64(cursor, end, flush->text_bytes);
    cursor = xhci_record_text(cursor, end, " slot_bytes=");
    cursor = xhci_record_u64(cursor, end, flush->slot_bytes);
    cursor = xhci_record_text(cursor, end, " publish_us=");
    cursor = xhci_record_u64(cursor, end, flush->publish_us);
    cursor = xhci_record_text(cursor, end, " publish=derived");
    cursor = xhci_record_text(cursor, end, " esp_write_us=");
    cursor = xhci_record_u64(cursor, end, flush->esp_write_us);
    cursor = xhci_record_text(cursor, end, " commit_us=");
    cursor = xhci_record_u64(cursor, end, flush->commit_us);
    cursor = xhci_record_text(cursor, end, " total_us=");
    cursor = xhci_record_u64(cursor, end, flush->total_us);
    cursor = xhci_record_text(cursor, end, " unavailable=esp_blocks,clusters");
    return xhci_record_finish(buffer, size, line, cursor);
}

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
    uint32_t waited_ms) {
    char line[XHCI_PORT_RECORD_BYTES];
    const char *end = line + sizeof(line) - 1U;
    char *cursor = line;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (group == NULL || plan == NULL) {
        return 0U;
    }

    cursor = xhci_record_text(cursor, end, "[XHCI-PORT] pci=");
    cursor = xhci_record_location(cursor, end, bus, device, function);
    cursor = xhci_record_text(cursor, end, " port=");
    cursor = xhci_record_u32(cursor, end, port);
    cursor = xhci_record_text(cursor, end, " group=");
    cursor = xhci_record_text(cursor, end, group);
    cursor = xhci_record_text(cursor, end, " was=");
    cursor = xhci_record_hex32(cursor, end, plan->was);
    cursor = xhci_record_text(cursor, end, " now=");
    cursor = xhci_record_hex32(cursor, end, now);
    cursor = xhci_record_text(cursor, end, " pp=");
    cursor = xhci_record_u32(cursor, end, plan->powered ? 1U : 0U);
    cursor = xhci_record_text(cursor, end, " ccs=");
    cursor = xhci_record_u32(cursor, end,
                             ((now & XHCI_PORTSC_CCS) != 0U) ? 1U : 0U);
    cursor = xhci_record_text(cursor, end, " pls=");
    cursor = xhci_record_u32(cursor, end, xhci_port_link_state(now));
    cursor = xhci_record_text(cursor, end, " pon=");
    cursor = xhci_record_u32(cursor, end, plan->power_on ? 1U : 0U);
    cursor = xhci_record_text(cursor, end, " kick=");
    cursor = xhci_record_u32(cursor, end, plan->link_kick ? 1U : 0U);
    cursor = xhci_record_text(cursor, end, " wait=");
    cursor = xhci_record_u32(cursor, end, waited_ms);
    return xhci_record_finish(buffer, size, line, cursor);
}

/*
 * MOUSE-DIAG (owner task 2026-09-27): the stall snapshot's four records.
 *
 * The kernel reads the endpoint's counters, the controller's registers and the
 * port's PORTSC exactly once per silence episode and hands them here; the
 * formatters decide nothing, so the host proof can pin every field and the
 * kernel is not trusted to have printed the right ones.  `identified=` is the
 * contract's own honesty bit: a record that could not name the port the device
 * sits on says so and carries every port value it read instead of presenting
 * one of them as the device's.
 */
size_t xhci_format_silence_record(
    char *buffer,
    size_t size,
    const struct xhci_silence_time *time,
    const struct xhci_silence_endpoint *endpoint) {
    char line[XHCI_SILENCE_RECORD_BYTES];
    const char *end = line + sizeof(line) - 1U;
    char *cursor = line;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (time == NULL || endpoint == NULL) {
        return 0U;
    }
    cursor = xhci_record_text(cursor, end, "[MOUSE-SILENCE] ticks=");
    cursor = xhci_record_u64(cursor, end, time->ticks);
    cursor = xhci_record_text(cursor, end, " since_completion_ms=");
    cursor = xhci_record_u32(cursor, end, time->since_completion_ms);
    cursor = xhci_record_text(cursor, end, " since_report_ms=");
    cursor = xhci_record_u32(cursor, end, time->since_report_ms);
    cursor = xhci_record_text(cursor, end, " slot=");
    cursor = xhci_record_u32(cursor, end, endpoint->slot);
    cursor = xhci_record_text(cursor, end, " dci=");
    cursor = xhci_record_u32(cursor, end, endpoint->dci);
    cursor = xhci_record_text(cursor, end, " ep_state=");
    cursor = xhci_record_u32(cursor, end, endpoint->endpoint_state);
    cursor = xhci_record_text(cursor, end, " armed=");
    cursor = xhci_record_u32(cursor, end, endpoint->armed);
    cursor = xhci_record_text(cursor, end, " pending=");
    cursor = xhci_record_u32(cursor, end, endpoint->pending);
    cursor = xhci_record_text(cursor, end, " reposted=");
    cursor = xhci_record_u32(cursor, end, endpoint->reposted);
    cursor = xhci_record_text(cursor, end, " completed=");
    cursor = xhci_record_u32(cursor, end, endpoint->completed);
    cursor = xhci_record_text(cursor, end, " reports=");
    cursor = xhci_record_u32(cursor, end, endpoint->reports);
    cursor = xhci_record_text(cursor, end, " last_code=");
    cursor = xhci_record_u32(cursor, end, endpoint->last_completion_code);
    cursor = xhci_record_text(cursor, end, " last_completion_ticks=");
    cursor = xhci_record_u64(cursor, end, endpoint->last_completion_ticks);
    return xhci_record_finish(buffer, size, line, cursor);
}

size_t xhci_format_silence_controller_record(
    char *buffer,
    size_t size,
    const struct xhci_silence_controller *controller) {
    char line[XHCI_SILENCE_RECORD_BYTES];
    const char *end = line + sizeof(line) - 1U;
    char *cursor = line;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (controller == NULL) {
        return 0U;
    }
    cursor = xhci_record_text(cursor, end, "[MOUSE-SILENCE-XHCI] pci=");
    cursor = xhci_record_location(cursor, end, controller->bus,
                                  controller->device, controller->function);
    cursor = xhci_record_text(cursor, end, " usbcmd=");
    cursor = xhci_record_hex64(cursor, end, controller->usbcmd);
    cursor = xhci_record_text(cursor, end, " usbsts=");
    cursor = xhci_record_hex64(cursor, end, controller->usbsts);
    cursor = xhci_record_text(cursor, end, " crcr=");
    cursor = xhci_record_hex64(cursor, end, controller->crcr);
    cursor = xhci_record_text(cursor, end, " erdp=");
    cursor = xhci_record_hex64(cursor, end, controller->erdp);
    cursor = xhci_record_text(cursor, end, " erstba=");
    cursor = xhci_record_hex64(cursor, end, controller->erstba);
    cursor = xhci_record_text(cursor, end, " iman=");
    cursor = xhci_record_hex64(cursor, end, controller->iman);
    cursor = xhci_record_text(cursor, end, " imod=");
    cursor = xhci_record_hex64(cursor, end, controller->imod);
    cursor = xhci_record_text(cursor, end, " running=");
    cursor = xhci_record_u32(cursor, end,
                             (controller->usbcmd & XHCI_USBCMD_RS) != 0U ? 1U
                                                                        : 0U);
    cursor = xhci_record_text(cursor, end, " halted=");
    cursor = xhci_record_u32(cursor, end,
                             (controller->usbsts & XHCI_USBSTS_HCH) != 0U ? 1U
                                                                         : 0U);
    cursor = xhci_record_text(cursor, end, " error=");
    cursor = xhci_record_hex64(cursor, end, controller->error);
    cursor = xhci_record_text(cursor, end, " host_events=");
    cursor = xhci_record_u32(cursor, end, controller->host_events);
    cursor = xhci_record_text(cursor, end, " ringfull=");
    cursor = xhci_record_u32(cursor, end, controller->ringfull);
    return xhci_record_finish(buffer, size, line, cursor);
}

size_t xhci_format_silence_port_record(
    char *buffer,
    size_t size,
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint32_t mouse_port,
    const uint32_t *portsc,
    size_t portsc_count) {
    char line[XHCI_SILENCE_PORT_RECORD_BYTES];
    const char *end = line + sizeof(line) - 1U;
    char *cursor = line;
    const bool identified =
        mouse_port != 0U && portsc != NULL && mouse_port <= portsc_count;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (portsc == NULL && portsc_count != 0U) {
        return 0U;
    }
    cursor = xhci_record_text(cursor, end, "[MOUSE-SILENCE-PORT] pci=");
    cursor = xhci_record_location(cursor, end, bus, device, function);
    cursor = xhci_record_text(cursor, end, " identified=");
    cursor = xhci_record_u32(cursor, end, identified ? 1U : 0U);
    cursor = xhci_record_text(cursor, end, " port=");
    cursor = xhci_record_u32(cursor, end, identified ? mouse_port : 0U);
    cursor = xhci_record_text(cursor, end, " ports=");
    cursor = xhci_record_u32(cursor, end, (uint32_t)portsc_count);
    if (identified) {
        const uint32_t value = portsc[mouse_port - 1U];

        cursor = xhci_record_text(cursor, end, " portsc=");
        cursor = xhci_record_hex32(cursor, end, value);
        cursor = xhci_record_bit(cursor, end, " ccs=", value, XHCI_PORTSC_CCS);
        cursor = xhci_record_bit(cursor, end, " ped=", value, XHCI_PORTSC_PED);
        cursor = xhci_record_bit(cursor, end, " pr=", value, XHCI_PORTSC_PR);
        cursor = xhci_record_bit(cursor, end, " pp=", value, XHCI_PORTSC_PP);
        /* xHCI's PORTSC has no over-current *change* bit: bit 3 is Over-current
           Active (OCA), which is what this record publishes and what the
           contract's `OCC` names. */
        cursor = xhci_record_bit(cursor, end, " oca=", value, XHCI_PORTSC_OCA);
        cursor = xhci_record_bit(cursor, end, " csc=", value, XHCI_PORTSC_CSC);
        cursor = xhci_record_bit(cursor, end, " pec=", value, XHCI_PORTSC_PEC);
        cursor = xhci_record_bit(cursor, end, " wrc=", value, XHCI_PORTSC_WRC);
        cursor = xhci_record_bit(cursor, end, " prc=", value, XHCI_PORTSC_PRC);
        cursor = xhci_record_bit(cursor, end, " plc=", value, XHCI_PORTSC_PLC);
        cursor = xhci_record_bit(cursor, end, " cec=", value, XHCI_PORTSC_CEC);
        cursor = xhci_record_text(cursor, end, " pls=");
        cursor = xhci_record_u32(cursor, end, xhci_port_link_state(value));
        cursor = xhci_record_text(cursor, end, " wake=");
        cursor = xhci_record_u32(
            cursor, end, (value & XHCI_PORTSC_WAKE_MASK) >> 25U);
    } else {
        /* The driver cannot name the port the device sits on, so every port
           value it read is published instead of one presented as the device's. */
        for (size_t index = 0U; index < portsc_count; ++index) {
            cursor = xhci_record_text(cursor, end, " p");
            cursor = xhci_record_u32(cursor, end, (uint32_t)(index + 1U));
            cursor = xhci_record_text(cursor, end, "=");
            cursor = xhci_record_hex32(cursor, end, portsc[index]);
        }
    }
    return xhci_record_finish(buffer, size, line, cursor);
}

size_t xhci_format_silence_after_record(
    char *buffer,
    size_t size,
    uint64_t ticks,
    uint32_t waited_ms,
    uint64_t erdp_before,
    uint64_t erdp_after,
    uint32_t usbcmd,
    uint32_t usbsts,
    uint32_t portsc) {
    char line[XHCI_SILENCE_RECORD_BYTES];
    const char *end = line + sizeof(line) - 1U;
    char *cursor = line;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    cursor = xhci_record_text(cursor, end, "[MOUSE-SILENCE-AFTER] ticks=");
    cursor = xhci_record_u64(cursor, end, ticks);
    cursor = xhci_record_text(cursor, end, " waited_ms=");
    cursor = xhci_record_u32(cursor, end, waited_ms);
    cursor = xhci_record_text(cursor, end, " erdp_before=");
    cursor = xhci_record_hex64(cursor, end, erdp_before);
    cursor = xhci_record_text(cursor, end, " erdp_after=");
    cursor = xhci_record_hex64(cursor, end, erdp_after);
    cursor = xhci_record_text(cursor, end, " erdp_moved=");
    cursor = xhci_record_u32(cursor, end,
                             erdp_before == erdp_after ? 0U : 1U);
    cursor = xhci_record_text(cursor, end, " usbcmd=");
    cursor = xhci_record_hex64(cursor, end, usbcmd);
    cursor = xhci_record_text(cursor, end, " usbsts=");
    cursor = xhci_record_hex64(cursor, end, usbsts);
    cursor = xhci_record_text(cursor, end, " portsc=");
    cursor = xhci_record_hex32(cursor, end, portsc);
    return xhci_record_finish(buffer, size, line, cursor);
}

/*
 * MOUSE-DIAG (owner task 2026-09-27, second step): the EP0 probe's two records.
 *
 * The control result and the state around it are separate lines because the
 * state half is wide (three 64-bit registers and two 32-bit ones on each side)
 * and the console's one-line budget is 320 bytes; both are checked at their
 * widest by the host proof.
 */
size_t xhci_format_ep0_probe_record(
    char *buffer,
    size_t size,
    const struct xhci_ep0_probe *probe) {
    char line[XHCI_EP0_PROBE_RECORD_BYTES];
    const char *end = line + sizeof(line) - 1U;
    char *cursor = line;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (probe == NULL) {
        return 0U;
    }
    cursor = xhci_record_text(cursor, end, "[MOUSE-EP0] ticks=");
    cursor = xhci_record_u64(cursor, end, probe->ticks);
    cursor = xhci_record_text(cursor, end, " slot=");
    cursor = xhci_record_u32(cursor, end, probe->slot);
    cursor = xhci_record_text(cursor, end, " dci=");
    cursor = xhci_record_u32(cursor, end, probe->dci);
    cursor = xhci_record_text(cursor, end, " request=");
    cursor = xhci_record_u32(cursor, end, probe->request);
    cursor = xhci_record_text(cursor, end, " submitted=");
    cursor = xhci_record_u32(cursor, end, probe->submitted);
    cursor = xhci_record_text(cursor, end, " code=");
    cursor = xhci_record_u32(cursor, end, probe->completion_code);
    cursor = xhci_record_text(cursor, end, " elapsed_us=");
    cursor = xhci_record_u64(cursor, end, probe->elapsed_us);
    cursor = xhci_record_text(cursor, end, " ep_before=");
    cursor = xhci_record_u32(cursor, end, probe->endpoint_state_before);
    cursor = xhci_record_text(cursor, end, " ep_after=");
    cursor = xhci_record_u32(cursor, end, probe->endpoint_state_after);
    cursor = xhci_record_text(cursor, end, " slot_before=");
    cursor = xhci_record_u32(cursor, end, probe->slot_state_before);
    cursor = xhci_record_text(cursor, end, " slot_after=");
    cursor = xhci_record_u32(cursor, end, probe->slot_state_after);
    return xhci_record_finish(buffer, size, line, cursor);
}

size_t xhci_format_ep0_state_record(
    char *buffer,
    size_t size,
    const struct xhci_ep0_probe *probe) {
    char line[XHCI_EP0_PROBE_RECORD_BYTES];
    const char *end = line + sizeof(line) - 1U;
    char *cursor = line;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (probe == NULL) {
        return 0U;
    }
    cursor = xhci_record_text(cursor, end, "[MOUSE-EP0-STATE] portsc_before=");
    cursor = xhci_record_hex32(cursor, end, probe->portsc_before);
    cursor = xhci_record_text(cursor, end, " portsc_after=");
    cursor = xhci_record_hex32(cursor, end, probe->portsc_after);
    cursor = xhci_record_text(cursor, end, " usbsts_before=");
    cursor = xhci_record_hex64(cursor, end, probe->usbsts_before);
    cursor = xhci_record_text(cursor, end, " usbsts_after=");
    cursor = xhci_record_hex64(cursor, end, probe->usbsts_after);
    cursor = xhci_record_text(cursor, end, " erdp_before=");
    cursor = xhci_record_hex64(cursor, end, probe->erdp_before);
    cursor = xhci_record_text(cursor, end, " erdp_after=");
    cursor = xhci_record_hex64(cursor, end, probe->erdp_after);
    cursor = xhci_record_text(cursor, end, " erdp_moved=");
    cursor = xhci_record_u32(cursor, end,
                             probe->erdp_before == probe->erdp_after ? 0U : 1U);
    return xhci_record_finish(buffer, size, line, cursor);
}

size_t xhci_format_iter_max_record(
    char *buffer,
    size_t size,
    uint64_t ticks,
    uint64_t loops,
    uint64_t second,
    uint64_t iter_max_us,
    const char *phase) {    char line[XHCI_ITER_MAX_RECORD_BYTES];
    const char *end = line + sizeof(line) - 1U;
    char *cursor = line;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (phase == NULL) {
        return 0U;
    }
    cursor = xhci_record_text(cursor, end, "[ITER-MAX] ticks=");
    cursor = xhci_record_u64(cursor, end, ticks);
    cursor = xhci_record_text(cursor, end, " second=");
    cursor = xhci_record_u64(cursor, end, second);
    cursor = xhci_record_text(cursor, end, " loops=");
    cursor = xhci_record_u64(cursor, end, loops);
    cursor = xhci_record_text(cursor, end, " iter_max_us=");
    cursor = xhci_record_u64(cursor, end, iter_max_us);
    cursor = xhci_record_text(cursor, end, " phase=");
    cursor = xhci_record_text(cursor, end, phase);
    return xhci_record_finish(buffer, size, line, cursor);
}

/*
 * ITEM-453: the Supported Protocol capability (xHCI 1.1 section 7.2.1).
 *
 * Dword 0 carries the capability ID, the next pointer, and the major and minor
 * revisions; dword 1 is the four-character name; dword 2 carries the raw port
 * range (offset in bits 7:0, count in bits 15:8) and the number of Protocol
 * Speed ID dwords that follow it in bits 31:28.  Each PSI dword's low nibble is
 * the PSIV - the value the port's speed field reports for a device in that
 * group.
 *
 * The parser extracts fields and refuses what it cannot use; the caller decides
 * what to do with a controller that declares no group at all, and the record
 * publishes the three raw dwords beside the parsed fields so a wrong reading of
 * this layout is visible on the next hardware run rather than trusted.
 */
bool xhci_protocol_capability(
    uint32_t revision,
    uint32_t name_string,
    uint32_t port_info,
    const uint32_t *psi,
    uint32_t psi_count,
    struct xhci_protocol_group *group) {
    uint32_t count;
    uint32_t declared;

    if (group == NULL) {
        return false;
    }
    if ((revision & XHCI_EXT_CAP_ID_MASK) != XHCI_EXT_CAP_PROTOCOL) {
        return false;
    }
    count = (port_info & XHCI_EXT_PORT_COUNT_MASK) >> XHCI_EXT_PORT_COUNT_SHIFT;
    if (count == 0U || (port_info & XHCI_EXT_PORT_OFFSET_MASK) == 0U) {
        /* A group with no ports, or one whose first port would be port 0, is
           not a port range the specification allows (xHCI 1.1 section 7.2.1:
           the offset's valid values are 1 to MaxPorts). */
        return false;
    }
    declared = (port_info & XHCI_EXT_PORT_PSIC_MASK) >> XHCI_EXT_PORT_PSIC_SHIFT;
    if (declared != 0U && psi == NULL) {
        /* The caller does not have the PSI dwords: refuse rather than publish
           a group whose declared speeds were never read. */
        return false;
    }
    group->major = (uint8_t)((revision >> XHCI_EXT_CAP_MAJOR_SHIFT) & 0xFFU);
    group->minor = (uint8_t)((revision >> XHCI_EXT_CAP_MINOR_SHIFT) & 0xFFU);
    group->port_offset = (uint8_t)(port_info & XHCI_EXT_PORT_OFFSET_MASK);
    group->port_count = (uint8_t)count;
    group->psic = (uint8_t)declared;
    group->raw_revision = revision;
    group->raw_name = name_string;
    group->raw_port_info = port_info;
    group->psiv_count = 0U;
    for (uint32_t index = 0U; index < XHCI_PROTOCOL_PSI_MAX; ++index) {
        group->psiv[index] = 0U;
    }
    for (uint32_t index = 0U;
         index < declared && index < psi_count && index < XHCI_PROTOCOL_PSI_MAX;
         ++index) {
        group->psiv[index] = (uint8_t)(psi[index] & XHCI_EXT_PSI_PSIV_MASK);
        ++group->psiv_count;
    }
    for (uint32_t index = 0U; index < 4U; ++index) {
        const char letter = (char)((name_string >> (index * 8U)) & 0xFFU);

        group->name[index] = (letter >= 0x20 && letter < 0x7F) ? letter : '.';
    }
    group->name[4] = '\0';
    for (uint32_t length = 4U; length > 0U; --length) {
        if (group->name[length - 1U] == ' ') {
            group->name[length - 1U] = '\0';
        } else {
            break;
        }
    }
    return true;
}

bool xhci_protocol_port_range(
    const struct xhci_protocol_group *group,
    uint32_t *first,
    uint32_t *last) {
    if (group == NULL || first == NULL || last == NULL ||
        group->port_count == 0U || group->port_offset == 0U) {
        return false;
    }
    /*
     * xHCI 1.1 section 7.2.1: the Protocol Port Offset field is the first raw
     * port number of the group ("valid values are 1 to MaxPorts", Linux's
     * xhci-mem.c states the same rule), and the count includes that port - so
     * the group owns offset .. offset + count - 1.  QEMU's model pins the
     * reading: its USB2 group declares (offset 5, count 4) for the four ports
     * it numbers 5..8 after its four USB3 ports, which is also where the
     * keyboard attached to its first USB2 port answers.
     */
    *first = (uint32_t)group->port_offset;
    *last = (uint32_t)group->port_offset + (uint32_t)group->port_count - 1U;
    return true;
}

const struct xhci_protocol_group *xhci_protocol_for_port(
    const struct xhci_protocol_group *groups,
    uint32_t count,
    uint32_t port) {
    if (groups == NULL) {
        return NULL;
    }
    for (uint32_t index = 0U; index < count; ++index) {
        uint32_t first = 0U;
        uint32_t last = 0U;

        if (!xhci_protocol_port_range(&groups[index], &first, &last)) {
            continue;
        }
        if (port >= first && port <= last) {
            return &groups[index];
        }
    }
    return NULL;
}

const char *xhci_protocol_group_label(uint8_t major) {
    switch (major) {
    case 1U:
        return "USB1";
    case 2U:
        return "USB2";
    case 3U:
        return "USB3";
    case 4U:
        return "USB4";
    default:
        return NULL;
    }
}

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
    uint32_t port_info) {
    char line[XHCI_PROTOCOL_RECORD_BYTES];
    const char *end = line + sizeof(line) - 1U;
    char *cursor = line;
    uint32_t first = 0U;
    uint32_t last = 0U;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (group == NULL) {
        return 0U;
    }

    cursor = xhci_record_text(cursor, end, "[XHCI-PROTO] pci=");
    cursor = xhci_record_location(cursor, end, bus, device, function);
    cursor = xhci_record_text(cursor, end, " index=");
    cursor = xhci_record_u32(cursor, end, index);
    cursor = xhci_record_text(cursor, end, " groups=");
    cursor = xhci_record_u32(cursor, end, groups);
    cursor = xhci_record_text(cursor, end, " major=");
    cursor = xhci_record_u32(cursor, end, group->major);
    cursor = xhci_record_text(cursor, end, " minor=");
    cursor = xhci_record_u32(cursor, end, group->minor);
    cursor = xhci_record_text(cursor, end, " name=");
    cursor = xhci_record_text(cursor, end, group->name);
    cursor = xhci_record_text(cursor, end, " off=");
    cursor = xhci_record_u32(cursor, end, group->port_offset);
    cursor = xhci_record_text(cursor, end, " count=");
    cursor = xhci_record_u32(cursor, end, group->port_count);
    if (xhci_protocol_port_range(group, &first, &last)) {
        cursor = xhci_record_text(cursor, end, " ports=");
        cursor = xhci_record_u32(cursor, end, first);
        cursor = xhci_record_text(cursor, end, "-");
        cursor = xhci_record_u32(cursor, end, last);
    }
    cursor = xhci_record_text(cursor, end, " psiv=");
    if (group->psiv_count == 0U) {
        cursor = xhci_record_text(cursor, end, "NONE");
    }
    for (uint32_t psi_index = 0U; psi_index < group->psiv_count; ++psi_index) {
        if (psi_index != 0U) {
            cursor = xhci_record_text(cursor, end, ",");
        }
        cursor = xhci_record_u32(cursor, end, group->psiv[psi_index]);
    }
    cursor = xhci_record_text(cursor, end, " d0=");
    cursor = xhci_record_hex32(cursor, end, revision);
    cursor = xhci_record_text(cursor, end, " d1=");
    cursor = xhci_record_hex32(cursor, end, name_string);
    cursor = xhci_record_text(cursor, end, " d2=");
    cursor = xhci_record_hex32(cursor, end, port_info);
    return xhci_record_finish(buffer, size, line, cursor);
}

uint32_t xhci_scan_progress(enum xhci_status status) {
    /*
     * Deeper progress ranks higher. The order is what an operator needs: a
     * device that answered and was fully described is more informative than a
     * controller with nothing attached, and a controller that never came up is
     * the least.
     */
    switch (status) {
    case XHCI_STATUS_OK:
        return 100U;
    case XHCI_STATUS_KEYBOARD_LOST:
        return 90U;
    case XHCI_STATUS_NOT_A_KEYBOARD:
    case XHCI_STATUS_NOT_A_MOUSE:
        return 80U;
    case XHCI_STATUS_ROLE_TAKEN:
        /* ITEM-452: a controller that was surveyed but not needed - the device
           is named, and the machine already has the role it would fill. */
        return 35U;
    case XHCI_STATUS_ENUMERATION_FAILED:
        return 70U;
    case XHCI_STATUS_UNSUPPORTED_SPEED:
        return 60U;
    case XHCI_STATUS_COMMAND_FAILED:
        return 50U;
    case XHCI_STATUS_DMA_UNAVAILABLE:
        return 45U;
    case XHCI_STATUS_NO_DEVICE:
        return 40U;
    case XHCI_STATUS_RESET_TIMEOUT:
    case XHCI_STATUS_HALT_TIMEOUT:
        return 30U;
    case XHCI_STATUS_MMIO_MAP_FAILED:
        return 20U;
    case XHCI_STATUS_NO_MEMORY_BAR:
        return 15U;
    case XHCI_STATUS_CONTROLLER_ABSENT:
        return 10U;
    case XHCI_STATUS_UNKNOWN:
    default:
        return 0U;
    }
}

bool xhci_scan_prefers(
    enum xhci_status candidate,
    enum xhci_status current) {
    return xhci_scan_progress(candidate) > xhci_scan_progress(current);
}

/* Copy the published detail into the scan's own storage. The caller's string is
   typically the driver's working buffer and is rewritten by the next probe. */
static void xhci_scan_keep_detail(
    struct xhci_scan *scan,
    const char *detail) {
    size_t index = 0U;

    if (detail != NULL) {
        while (detail[index] != '\0' &&
               index + 1U < XHCI_SCAN_DETAIL_BYTES) {
            scan->detail_storage[index] = detail[index];
            ++index;
        }
    }
    scan->detail_storage[index] = '\0';
    scan->detail = scan->detail_storage;
}

void xhci_scan_init(
    struct xhci_scan *scan,
    uint32_t controllers_total,
    uint32_t attempt_limit) {
    if (scan == NULL) {
        return;
    }
    scan->controllers_total = controllers_total;
    scan->controllers_seen = 0U;
    scan->attempts = 0U;
    scan->attempt_limit = attempt_limit;
    scan->selected = false;
    scan->status = XHCI_STATUS_CONTROLLER_ABSENT;
    xhci_scan_keep_detail(scan, "NO_XHCI_FUNCTION");
}

void xhci_scan_begin_controller(struct xhci_scan *scan) {
    if (scan == NULL) {
        return;
    }
    ++scan->controllers_seen;
    scan->attempts = 0U;
}

bool xhci_scan_take_attempt(struct xhci_scan *scan) {
    if (scan == NULL || scan->attempts >= scan->attempt_limit) {
        return false;
    }
    ++scan->attempts;
    return true;
}

bool xhci_scan_note_controller(
    struct xhci_scan *scan,
    enum xhci_status status,
    const char *detail,
    bool selected) {
    if (scan == NULL) {
        return false;
    }
    if (selected) {
        /*
         * ITEM-406: a usable boot keyboard was addressed on this controller,
         * so the machine has local input and the machine-wide verdict is OK.
         *
         * ITEM-452: this is no longer the end of the machine-wide walk.  Every
         * controller the machine has is still visited and still publishes its
         * own record - the owner's pointer sits on a controller after the
         * keyboard's, and a controller the driver never opened cannot name
         * itself.  The return value still reports that a keyboard was bound
         * here; the caller keeps walking regardless.
         */
        scan->selected = true;
        scan->status = XHCI_STATUS_OK;
        xhci_scan_keep_detail(scan, detail != NULL ? detail : "OK");
        return true;
    }
    if (xhci_scan_prefers(status, scan->status)) {
        scan->status = status;
        xhci_scan_keep_detail(scan, detail != NULL ? detail : "UNKNOWN");
    }
    return false;
}

void xhci_context_clear(void *base, size_t bytes) {
    if (base == NULL) {
        return;
    }
    memset(base, 0, bytes);
}

void xhci_input_control_context(
    void *base,
    size_t context_size,
    uint32_t add_flags,
    uint32_t drop_flags) {
    if (base == NULL || context_size < 8U) {
        return;
    }
    /* Input Control Context dword 0 is the Drop flags and dword 1 the Add
       flags (xHCI 1.1 table 6-6); a swap here makes every Add flag absent and
       the controller answers the command with a TRB Error. */
    write32(base, 0U, drop_flags);
    write32(base, 4U, add_flags);
}

void xhci_slot_context(
    void *entry,
    uint8_t speed,
    uint8_t context_entries,
    uint8_t root_hub_port) {
    if (entry == NULL) {
        return;
    }
    /* Dword 0: Route String (0 for a root-port device), Speed, Context
       Entries. Dword 1: Root Hub Port Number. */
    write32(entry, 0U,
            ((uint32_t)(speed & 0x0FU) << 20) |
                ((uint32_t)(context_entries & 0x1FU) << 27));
    write32(entry, 4U, (uint32_t)root_hub_port << 16);
    write32(entry, 8U, 0U);
    write32(entry, 12U, 0U);
}

void xhci_endpoint_context(
    void *entry,
    uint8_t endpoint_type,
    uint16_t max_packet_size,
    uint8_t interval,
    uint64_t dequeue_physical,
    uint16_t average_trb_length) {
    xhci_endpoint_context_burst(entry, endpoint_type, max_packet_size,
                                interval, dequeue_physical,
                                average_trb_length, 0U);
}

void xhci_endpoint_context_burst(
    void *entry,
    uint8_t endpoint_type,
    uint16_t max_packet_size,
    uint8_t interval,
    uint64_t dequeue_physical,
    uint16_t average_trb_length,
    uint8_t max_burst) {
    if (entry == NULL) {
        return;
    }
    /* Dword 0: Interval. Dword 1: CErr = 3, Endpoint Type, Max Packet Size.
       Dwords 2..3: TR Dequeue Pointer with its Dequeue Cycle State bit.
       Dword 4: Average TRB Length, Max Burst Size. */
    write32(entry, 0U, (uint32_t)interval << 16);
    write32(entry, 4U,
            (3U << 1) |
                ((uint32_t)(endpoint_type & 0x07U) << 3) |
                ((uint32_t)max_packet_size << 16));
    write32(entry, 8U,
            (uint32_t)(dequeue_physical & 0xFFFFFFF0ULL) | 1U);
    write32(entry, 12U, (uint32_t)(dequeue_physical >> 32));
    write32(entry, 16U, (uint32_t)average_trb_length |
                            ((uint32_t)max_burst << 16));
}

uint8_t xhci_endpoint_dci(uint8_t endpoint_number, bool direction_in) {
    return (uint8_t)(((uint32_t)endpoint_number * 2U) +
                     (direction_in ? 1U : 0U));
}

uint32_t xhci_context_flag(uint8_t dci) {
    return (uint32_t)1U << dci;
}

size_t xhci_context_entry_index(uint8_t dci) {
    return (size_t)dci + 1U;
}

size_t xhci_context_offset(size_t context_size, uint32_t index) {
    return context_size * (size_t)index;
}

/*
 * ITEM-473 (СРОЧНО-23): the arm-depth clamp and the endpoint-state reader.
 *
 * The `ARM <n>` command lets the owner try a depth in a hardware run instead of
 * another code round.  The clamp keeps every legal value inside the interrupt
 * ring (XHCI_MOUSE_ARM_DEPTH) and - because that maximum is below
 * XHCI_EVENT_TRBS - inside the session event ring as well; a request of zero
 * answers the minimum rather than disabling the endpoint.
 */
uint8_t xhci_arm_depth_clamp(uint32_t requested) {
    if (requested < XHCI_ARM_DEPTH_MIN) {
        return (uint8_t)XHCI_ARM_DEPTH_MIN;
    }
    if (requested > XHCI_ARM_DEPTH_MAX) {
        return (uint8_t)XHCI_ARM_DEPTH_MAX;
    }
    return (uint8_t)requested;
}

/* Endpoint State is bits 2:0 of the Endpoint Context's dword 0 (xHCI 1.1
   section 6.2.3).  A missing context answers 0, the Disabled state, rather
   than reading through. */
uint32_t xhci_endpoint_state(const void *device_context, size_t context_size,
                             uint8_t dci) {
    if (device_context == NULL || context_size == 0U) {
        return 0U;
    }
    return read32(device_context, xhci_context_offset(context_size, dci)) &
           0x07U;
}

const char *xhci_endpoint_state_string(uint32_t state) {
    switch (state & 0x07U) {
    case 0U:
        return "Disabled";
    case 1U:
        return "Running";
    case 2U:
        return "Halted";
    case 3U:
        return "Stopped";
    case 4U:
        return "Error";
    default:
        return "Reserved";
    }
}

/* MOUSE-DIAG (2026-09-27): Slot State is bits 31:27 of the Slot Context's
   dword 0 (xHCI 1.1 section 6.2.2), entry 0 of the device context.  A missing
   context answers 0, the Disabled state, rather than reading through. */
uint32_t xhci_slot_state(const void *device_context, size_t context_size) {
    if (device_context == NULL || context_size == 0U) {
        return 0U;
    }
    return (read32(device_context, 0U) >> 27U) & 0x1FU;
}

const char *xhci_slot_state_string(uint32_t state) {
    switch (state & 0x1FU) {
    case 0U:
        return "Disabled";
    case 1U:
        return "Default";
    case 2U:
        return "Addressed";
    case 3U:
        return "Configured";
    default:
        return "Reserved";
    }
}

/*
 * ITEM-458: the local queue's push decision and the service gap.
 *
 * `head` is the next slot to write and `tail` the oldest unread one, so the
 * queue is full exactly when advancing the head would reach the tail - the
 * same one-free-slot rule the driver's rings use, and the reason a full queue
 * drops the newest report instead of overwriting an unread one.
 */
bool xhci_local_queue_push(uint32_t head, uint32_t tail, uint32_t capacity,
                           uint32_t *next_out) {
    uint32_t next;

    if (capacity == 0U) {
        return false;
    }
    next = (head + 1U) % capacity;
    if (next == tail) {
        return false;
    }
    if (next_out != NULL) {
        *next_out = next;
    }
    return true;
}

uint32_t xhci_service_gap_ms(uint32_t maximum_ms, uint64_t last_ticks,
                             uint64_t now_ticks, uint32_t tick_hz) {
    uint64_t elapsed_ticks;

    /* No earlier report, a clock that did not move, or an unknown rate: the
       gap is not measurable and the maximum is left exactly as it was. */
    if (last_ticks == 0U || now_ticks < last_ticks || tick_hz == 0U) {
        return maximum_ms;
    }
    elapsed_ticks = now_ticks - last_ticks;
    elapsed_ticks = elapsed_ticks * 1000U / tick_hz;
    if (elapsed_ticks > maximum_ms) {
        return elapsed_ticks > UINT32_MAX ? UINT32_MAX
                                          : (uint32_t)elapsed_ticks;
    }
    return maximum_ms;
}

uint32_t xhci_watchdog_silence_ms(uint64_t last_ticks, uint64_t now_ticks,
                                  uint32_t tick_hz) {
    uint64_t elapsed_ticks;
    uint64_t elapsed_ms;

    /* A clock that did not move, no progress yet, or an unknown rate: the wait
       is not measurable and the watchdog must not invent one. */
    if (last_ticks == 0U || now_ticks < last_ticks || tick_hz == 0U) {
        return 0U;
    }
    elapsed_ticks = now_ticks - last_ticks;
    if (elapsed_ticks > UINT64_MAX / 1000U) {
        return UINT32_MAX;
    }
    elapsed_ms = elapsed_ticks * 1000U / tick_hz;
    return elapsed_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed_ms;
}

bool xhci_watchdog_due(bool ready, uint32_t armed, uint32_t silence_ms,
                       uint32_t threshold_ms) {
    return ready && armed != 0U && silence_ms > threshold_ms;
}

bool xhci_watchdog_active_before(uint64_t last_report_ticks,
                                 uint64_t progress_ticks,
                                 uint64_t active_window_ticks) {
    /* No report ever, no window to judge by, or a clock that moved backwards:
       the device cannot be called active. */
    if (last_report_ticks == 0U || active_window_ticks == 0U ||
        progress_ticks < last_report_ticks) {
        return false;
    }
    return progress_ticks - last_report_ticks <= active_window_ticks;
}

uint32_t xhci_report_rate_per_second(uint32_t reports, uint64_t elapsed_ms) {
    uint64_t rate;

    if (elapsed_ms == 0U) {
        return 0U;
    }
    rate = (uint64_t)reports * 1000U / elapsed_ms;
    return rate > UINT32_MAX ? UINT32_MAX : (uint32_t)rate;
}

bool xhci_watchdog_escalate(bool active_before, uint32_t active_silences) {
    /* A re-arm is enough for an idle device; one active silence may be a
       one-off, the second says the plain recovery did not hold. */
    return active_before && active_silences >= 2U;
}

/*
 * ITEM-550: the command wait's own vocabulary, and the three records a failed
 * command is published as.
 *
 * The wait is a bounded loop that dequeues events and looks for the Command
 * Completion whose Command TRB Pointer is the address the submitted command
 * landed on.  Three things can happen and the old code collapsed all three
 * into one `false` return: the ring can refuse the TRB (nothing was ever
 * submitted), the controller can complete somebody else's command and never
 * this one (the wait times out and the other completion is silently dropped),
 * or the completion can arrive.  The record keeps them apart, and it keeps the
 * event-ring consumer state and the last dequeued event with them, because
 * "the consumer never moved" and "the consumer moved through foreign events"
 * point at different defects.
 */
const char *xhci_command_result_string(enum xhci_command_result result) {
    switch (result) {
    case XHCI_COMMAND_RESULT_SUCCESS:
        return "SUCCESS";
    case XHCI_COMMAND_RESULT_ENQUEUE_FAILED:
        return "ENQUEUE_FAILED";
    case XHCI_COMMAND_RESULT_TIMEOUT:
        return "TIMEOUT";
    }
    return "UNKNOWN";
}

enum xhci_command_event_kind xhci_command_event_kind_of(
    const struct xhci_event *event,
    uint64_t expected_address) {
    if (event == NULL || event->type != XHCI_TRB_COMMAND_COMPLETION) {
        return XHCI_COMMAND_EVENT_OTHER;
    }
    return event->parameter == expected_address ? XHCI_COMMAND_EVENT_OWN
                                                : XHCI_COMMAND_EVENT_FOREIGN;
}

void xhci_command_wait_init(struct xhci_command_wait *wait) {
    if (wait == NULL) {
        return;
    }
    for (size_t byte = 0U; byte < sizeof(*wait); ++byte) {
        ((uint8_t *)wait)[byte] = 0U;
    }
}

void xhci_command_wait_note(struct xhci_command_wait *wait,
                            const struct xhci_event *event,
                            enum xhci_command_event_kind kind) {
    if (wait == NULL || event == NULL) {
        return;
    }
    ++wait->events;
    if (kind != XHCI_COMMAND_EVENT_FOREIGN) {
        return;
    }
    ++wait->unmatched;
    wait->unmatched_valid = true;
    wait->unmatched_address = event->parameter;
    wait->unmatched_type = (uint8_t)event->type;
    wait->unmatched_code = event->completion_code;
    wait->unmatched_slot = event->slot_id;
}

enum xhci_command_result xhci_command_result_of(bool enqueued,
                                                bool completed) {
    if (!enqueued) {
        return XHCI_COMMAND_RESULT_ENQUEUE_FAILED;
    }
    return completed ? XHCI_COMMAND_RESULT_SUCCESS
                     : XHCI_COMMAND_RESULT_TIMEOUT;
}

void xhci_command_snapshot_keep(struct xhci_command_snapshot *kept,
                                const struct xhci_command_snapshot *captured) {
    if (kept == NULL || captured == NULL) {
        return;
    }
    *kept = *captured;
}

size_t xhci_format_command_record(
    char *buffer,
    size_t size,
    const struct xhci_command_snapshot *snapshot) {
    char line[XHCI_COMMAND_RECORD_BYTES];
    const char *end = line + sizeof(line) - 1U;
    char *cursor = line;
    size_t index;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (snapshot == NULL) {
        return 0U;
    }
    cursor = xhci_record_text(cursor, end, "[XHCI-CMD] result=");
    cursor = xhci_record_text(
        cursor, end, xhci_command_result_string(snapshot->result));
    cursor = xhci_record_text(cursor, end, " pci=");
    cursor = xhci_record_location(cursor, end, snapshot->bus, snapshot->device,
                                  snapshot->function);
    cursor = xhci_record_text(cursor, end, " type=");
    cursor = xhci_record_u32(cursor, end, snapshot->type);
    cursor = xhci_record_text(cursor, end, " index=");
    cursor = xhci_record_u32(cursor, end, snapshot->ring_index);
    cursor = xhci_record_text(cursor, end, " cycle=");
    cursor = xhci_record_u32(cursor, end, snapshot->ring_cycle);
    cursor = xhci_record_text(cursor, end, " phys=");
    cursor = xhci_record_hex64(cursor, end, snapshot->physical);
    cursor = xhci_record_text(cursor, end, " trb=");
    for (index = 0U; index < 4U; ++index) {
        if (index != 0U) {
            cursor = xhci_record_text(cursor, end, ",");
        }
        cursor = xhci_record_hex32(cursor, end, snapshot->trb[index]);
        if (cursor == NULL) {
            break;
        }
    }
    return xhci_record_finish(buffer, size, line, cursor);
}

size_t xhci_format_command_register_record(
    char *buffer,
    size_t size,
    const struct xhci_command_snapshot *snapshot) {
    char line[XHCI_COMMAND_RECORD_BYTES];
    const char *end = line + sizeof(line) - 1U;
    char *cursor = line;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (snapshot == NULL) {
        return 0U;
    }
    cursor = xhci_record_text(cursor, end, "[XHCI-CMD-REG] usbcmd=");
    cursor = xhci_record_hex32(cursor, end, snapshot->usbcmd);
    cursor = xhci_record_text(cursor, end, " usbsts=");
    cursor = xhci_record_hex32(cursor, end, snapshot->usbsts);
    cursor = xhci_record_text(cursor, end, " crcr=");
    cursor = xhci_record_hex64(cursor, end, snapshot->crcr);
    cursor = xhci_record_text(cursor, end, " config=");
    cursor = xhci_record_hex32(cursor, end, snapshot->config);
    cursor = xhci_record_text(cursor, end, " dcbaap=");
    cursor = xhci_record_hex64(cursor, end, snapshot->dcbaap);
    cursor = xhci_record_text(cursor, end, " erstsz=");
    cursor = xhci_record_hex32(cursor, end, snapshot->erstsz);
    cursor = xhci_record_text(cursor, end, " erstba=");
    cursor = xhci_record_hex64(cursor, end, snapshot->erstba);
    cursor = xhci_record_text(cursor, end, " erdp=");
    cursor = xhci_record_hex64(cursor, end, snapshot->erdp);
    cursor = xhci_record_text(cursor, end, " iman=");
    cursor = xhci_record_hex32(cursor, end, snapshot->iman);
    return xhci_record_finish(buffer, size, line, cursor);
}

size_t xhci_format_command_event_record(
    char *buffer,
    size_t size,
    const struct xhci_command_snapshot *snapshot) {
    char line[XHCI_COMMAND_RECORD_BYTES];
    const char *end = line + sizeof(line) - 1U;
    char *cursor = line;
    size_t index;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (snapshot == NULL) {
        return 0U;
    }
    cursor = xhci_record_text(cursor, end, "[XHCI-CMD-EVT] index=");
    cursor = xhci_record_u32(cursor, end, snapshot->event_index);
    cursor = xhci_record_text(cursor, end, " cycle=");
    cursor = xhci_record_u32(cursor, end, snapshot->event_cycle);
    cursor = xhci_record_text(cursor, end, " event=");
    if (snapshot->event_valid) {
        for (index = 0U; index < 4U; ++index) {
            if (index != 0U) {
                cursor = xhci_record_text(cursor, end, ",");
            }
            cursor = xhci_record_hex32(cursor, end, snapshot->event[index]);
            if (cursor == NULL) {
                break;
            }
        }
    } else {
        cursor = xhci_record_text(cursor, end, "none");
    }
    cursor = xhci_record_text(cursor, end, " events=");
    cursor = xhci_record_u32(cursor, end, snapshot->wait.events);
    cursor = xhci_record_text(cursor, end, " unmatched=");
    cursor = xhci_record_u32(cursor, end, snapshot->wait.unmatched);
    cursor = xhci_record_text(cursor, end, " last=");
    if (snapshot->wait.unmatched_valid) {
        cursor = xhci_record_text(cursor, end, "addr=");
        cursor = xhci_record_hex64(cursor, end,
                                   snapshot->wait.unmatched_address);
        cursor = xhci_record_text(cursor, end, ",type=");
        cursor = xhci_record_u32(cursor, end, snapshot->wait.unmatched_type);
        cursor = xhci_record_text(cursor, end, ",code=");
        cursor = xhci_record_u32(cursor, end, snapshot->wait.unmatched_code);
        cursor = xhci_record_text(cursor, end, ",slot=");
        cursor = xhci_record_u32(cursor, end, snapshot->wait.unmatched_slot);
    } else {
        cursor = xhci_record_text(cursor, end, "none");
    }
    return xhci_record_finish(buffer, size, line, cursor);
}
