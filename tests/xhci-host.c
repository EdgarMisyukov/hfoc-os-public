/*
 * ITEM-395 xHCI TRB, ring and context host proof.
 *
 * The hardware half of the USB HID driver (kernel/src/xhci.c) owns one
 * controller and cannot be unit-tested on a host, so every bit position, ring
 * rule and context index it depends on lives in kernel/src/xhci_trb.c, which
 * this proof drives directly. The QEMU proof shows a real emulated controller
 * accepting the result; this proof shows *why* the encoding is the one the
 * controller accepts, including the three defects that only a controller can
 * otherwise report:
 *
 *   - the Input Control Context is Drop flags then Add flags, and a swap makes
 *     every Add flag absent, which the controller answers with a TRB Error;
 *   - an Add Context flag bit is the device context index, while the context
 *     entry is one index further along the Input Context, and confusing the two
 *     silently configures the wrong endpoint (the doorbell then kicks an
 *     endpoint that does not exist and no transfer ever completes);
 *   - a ring's consumer must compare the cycle bit against the cycle state it
 *     is waiting for, not against a constant, or the first wrap either stops
 *     the consumer (the controller reports an event ring full) or makes it
 *     re-read retired entries.
 *
 * It also pins the Transfer Event, Command Completion Event, Setup/Data/Status
 * Stage and Link TRB layouts and the two address types devices depend on
 * (Setup Stage always carries 8 inline bytes; Link TRB always toggles the cycle
 * state of the segment it closes).
 */
#ifdef HFOC_XHCI_HOST_TEST

#include "xhci_trb.h"
/* ITEM-406: the scan policy lives in xhci.h (it is expressed in
   enum xhci_status) and is implemented in xhci_trb.c; usb_hid.h gives this
   proof the descriptor walk the driver feeds into that policy, so the machine
   the acceptance host has can be driven end to end without hardware. */
#include "usb_hid.h"
#include "xhci.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } \
} while (0)

static void test_trb_encoding(void) {
    const struct xhci_trb noop = xhci_trb_normal(1U, 0x1000U, 8U, true);

    CHECK(xhci_trb_type(&noop) == XHCI_TRB_NORMAL,
          "a Normal TRB carries its type in bits 15:10");
    CHECK(xhci_trb_cycle(&noop), "a Normal TRB carries its producer cycle bit");
    CHECK(noop.dword[0] == 0x1000U, "a Normal TRB buffer address is dword 0");
    CHECK(noop.dword[2] == 8U, "a Normal TRB transfer length is dword 2");
    CHECK((noop.dword[3] & XHCI_TRB_IOC) != 0U,
          "interrupt-on-completion is bit 5 of dword 3");

    {
        const struct xhci_trb setup = xhci_trb_setup(
            1U, 0x80U, 0x06U, 0x0100U, 0x0000U, 18U, XHCI_TRT_IN);

        CHECK(setup.dword[0] == 0x01000680U,
              "a Setup Stage TRB packs request type, request and value");
        CHECK(setup.dword[1] == (18U << 16),
              "a Setup Stage TRB packs index low and length high");
        CHECK(setup.dword[2] == 8U,
              "a Setup Stage TRB always transfers eight setup bytes");
        CHECK((setup.dword[3] & XHCI_TRB_IDT) != 0U,
              "a Setup Stage TRB carries the setup bytes inline");
        CHECK(((setup.dword[3] >> 16) & 0x3U) == XHCI_TRT_IN,
              "the Setup Stage transfer type is in bits 17:16");
    }

    {
        const struct xhci_trb data = xhci_trb_data(
            1U, 0x2000U, 18U, true, true);

        CHECK(xhci_trb_type(&data) == XHCI_TRB_DATA_STAGE,
              "a Data Stage TRB is typed");
        CHECK((data.dword[3] & XHCI_TRB_DIR) != 0U,
              "an IN Data Stage TRB sets its direction bit");
    }

    {
        const struct xhci_trb status_in = xhci_trb_status(1U, true);
        const struct xhci_trb status_out = xhci_trb_status(1U, false);

        CHECK(xhci_trb_type(&status_in) == XHCI_TRB_STATUS_STAGE,
              "a Status Stage TRB is typed");
        CHECK((status_in.dword[3] & XHCI_TRB_DIR) != 0U,
              "an IN Status Stage sets its direction bit");
        CHECK((status_out.dword[3] & XHCI_TRB_DIR) == 0U,
              "an OUT Status Stage clears its direction bit");
        CHECK((status_in.dword[3] & XHCI_TRB_IOC) != 0U,
              "a Status Stage TRB interrupts on completion");
    }

    {
        const struct xhci_trb enable = xhci_trb_enable_slot(1U);
        const struct xhci_trb address =
            xhci_trb_address_device(1U, 0x511000U, 7U);
        const struct xhci_trb configure =
            xhci_trb_configure_endpoint(1U, 0x511000U, 7U);
        const struct xhci_trb disable = xhci_trb_disable_slot(1U, 7U);
        const struct xhci_trb evaluate =
            xhci_trb_evaluate_context(1U, 0x511000U, 7U);

        CHECK(xhci_trb_type(&enable) == XHCI_TRB_ENABLE_SLOT,
              "Enable Slot is command TRB type 9");
        CHECK(xhci_trb_type(&address) == XHCI_TRB_ADDRESS_DEVICE,
              "Address Device is command TRB type 11");
        CHECK((address.dword[3] & XHCI_TRB_BSR) == 0U,
              "Address Device does not set the Block Set Address bit");
        CHECK((address.dword[3] >> XHCI_TRB_SLOT_SHIFT) == 7U,
              "a command TRB carries its slot id in bits 31:24");
        CHECK(address.dword[0] == 0x511000U &&
                  address.dword[1] == 0U,
              "a command TRB carries the input context address");
        CHECK(xhci_trb_type(&configure) == XHCI_TRB_CONFIGURE_ENDPOINT,
              "Configure Endpoint is command TRB type 12");
        CHECK(xhci_trb_type(&disable) == XHCI_TRB_DISABLE_SLOT,
              "Disable Slot is command TRB type 10");
        CHECK((disable.dword[3] >> XHCI_TRB_SLOT_SHIFT) == 7U,
              "Disable Slot names the slot it releases");
        /*
         * ITEM-420: Evaluate Context is its own command (type 13), not an
         * Address Device with the Block Set Address bit set.  The acceptance
         * host's xHCI answered the mislabelled command with completion code 19
         * - Context State Error - and the boot keyboard never came up.
         */
        CHECK(xhci_trb_type(&evaluate) == XHCI_TRB_EVALUATE_CONTEXT,
              "Evaluate Context is command TRB type 13");
        CHECK((evaluate.dword[3] & XHCI_TRB_BSR) == 0U,
              "Evaluate Context is not an Address Device with BSR set");
        CHECK((evaluate.dword[3] >> XHCI_TRB_SLOT_SHIFT) == 7U,
              "Evaluate Context names the slot whose context it evaluates");
        CHECK(evaluate.dword[0] == 0x511000U &&
                  evaluate.dword[1] == 0U,
              "Evaluate Context carries the input context address");
    }
}

static void test_ring_wrap(void) {
    struct xhci_trb storage[XHCI_RING_TRBS];
    struct xhci_ring ring;
    uint32_t index = 0U;

    memset(storage, 0, sizeof(storage));
    xhci_ring_init(&ring, storage, 0x5000U, XHCI_RING_TRBS, 1U);

    {
        const struct xhci_trb link = storage[XHCI_RING_TRBS - 1U];

        CHECK(xhci_trb_type(&link) == XHCI_TRB_LINK,
              "a ring ends in a Link TRB");
        CHECK((link.dword[3] & XHCI_TRB_TC) != 0U,
              "a ring's Link TRB toggles the cycle state");
        CHECK(link.dword[0] == 0x5000U && link.dword[1] == 0U,
              "a ring's Link TRB points back at the ring base");
    }

    /* One lap: every entry carries the initial cycle state. */
    for (uint32_t entry = 0U; entry < XHCI_RING_TRBS - 1U; ++entry) {
        const struct xhci_trb trb = xhci_trb_noop_command(1U);

        CHECK(xhci_ring_push_index(&ring, &trb, &index),
              "a ring accepts entries until it wraps");
        CHECK(index == entry, "a ring fills from its base");
        CHECK(xhci_trb_cycle(&storage[entry]),
              "entries of the first lap carry cycle state 1");
    }

    /* The next entry wraps: the Link TRB is published and the state flips. */
    {
        const struct xhci_trb trb = xhci_trb_noop_command(1U);

        CHECK(xhci_ring_push_index(&ring, &trb, &index),
              "a ring accepts the entry that wraps it");
        CHECK(index == 0U, "a ring wraps to its base");
        CHECK(!xhci_trb_cycle(&storage[0]),
              "entries after the wrap carry the flipped cycle state");
        CHECK(xhci_trb_cycle(&storage[XHCI_RING_TRBS - 1U]),
              "the Link TRB keeps the cycle state the producer was in");
        CHECK(xhci_ring_at(&ring, XHCI_RING_TRBS) == NULL,
              "the ring never hands out an index past its segment");
    }
}

static void test_event_decode(void) {
    struct xhci_event event;
    struct xhci_trb trb;

    memset(&trb, 0, sizeof(trb));
    trb.dword[0] = 0x3000U;
    trb.dword[1] = 0U;
    trb.dword[2] = (uint32_t)XHCI_COMPLETION_SUCCESS << 24;
    trb.dword[3] = (uint32_t)XHCI_TRB_TRANSFER_EVENT << 10U | 1U |
                   ((uint32_t)3U << XHCI_TRB_EP_SHIFT) |
                   ((uint32_t)1U << XHCI_TRB_SLOT_SHIFT);
    CHECK(xhci_event_decode(&trb, &event), "a Transfer Event decodes");
    CHECK(event.type == XHCI_TRB_TRANSFER_EVENT, "the event type is decoded");
    CHECK(event.slot_id == 1U && event.endpoint_id == 3U,
          "a Transfer Event names the slot and endpoint");
    CHECK(event.completion_code == XHCI_COMPLETION_SUCCESS,
          "a Transfer Event carries its completion code");
    CHECK(event.transfer_length == 0U, "a Transfer Event carries its residual");
    CHECK(event.parameter == 0x3000U,
          "a Transfer Event points at the TRB it retired");

    memset(&trb, 0, sizeof(trb));
    trb.dword[2] = ((uint32_t)XHCI_COMPLETION_SUCCESS << 24) | 5U;
    trb.dword[3] = (uint32_t)XHCI_TRB_COMMAND_COMPLETION << 10U | 1U |
                   ((uint32_t)9U << XHCI_TRB_SLOT_SHIFT);
    CHECK(xhci_event_decode(&trb, &event), "a Command Completion decodes");
    CHECK(event.parameter_low == 5U,
          "the command completion parameter carries the assigned slot id");
    CHECK(event.slot_id == 9U, "a Command Completion may name a slot");

    memset(&trb, 0, sizeof(trb));
    trb.dword[0] = 5U;
    trb.dword[2] = (uint32_t)XHCI_COMPLETION_SUCCESS << 24;
    trb.dword[3] = (uint32_t)XHCI_TRB_PORT_STATUS_CHANGE << 10U | 1U;
    CHECK(xhci_event_decode(&trb, &event), "a Port Status Change decodes");
    CHECK(event.port_id == 5U, "a Port Status Change names the port");

    /*
     * ITEM-473 (СРОЧНО-23): Event Ring Full (21) is a Host Controller Event,
     * not a transfer code.  The driver could not decode this TRB, so the one
     * error that explained the round-11 silent pointer was retired uncounted.
     */
    memset(&trb, 0, sizeof(trb));
    trb.dword[2] = (uint32_t)21U << 24;
    trb.dword[3] =
        (uint32_t)XHCI_TRB_HOST_CONTROLLER_EVENT << 10U | 1U;
    CHECK(xhci_event_decode(&trb, &event),
          "a Host Controller Event decodes");
    CHECK(event.type == XHCI_TRB_HOST_CONTROLLER_EVENT,
          "the Host Controller Event type is decoded");
    CHECK(event.completion_code == 21U,
          "Event Ring Full arrives as completion code 21");

    memset(&trb, 0, sizeof(trb));
    trb.dword[3] = (uint32_t)XHCI_TRB_NORMAL << 10U | 1U;
    CHECK(!xhci_event_decode(&trb, &event),
          "a transfer TRB is not an event");
    CHECK(!xhci_event_decode(NULL, &event), "a missing TRB is refused");
    CHECK(!xhci_event_decode(&trb, NULL), "a missing output is refused");
}

static void test_ring_full_of_garbage_is_not_an_event(void) {
    struct xhci_trb trb;
    struct xhci_event event;

    /* A retired entry (cycle clear) must never be mistaken for a published
       one: the consumer compares the bit against its own cycle state. */
    memset(&trb, 0, sizeof(trb));
    trb.dword[3] = (uint32_t)XHCI_TRB_TRANSFER_EVENT << 10U;
    CHECK(!xhci_trb_cycle(&trb), "a cycle-0 entry reads as unpublished");
    CHECK(xhci_event_decode(&trb, &event),
          "the transfer event decoder itself does not filter on cycle");
}

static void test_names(void) {
    CHECK(strcmp(xhci_trb_type_string(XHCI_TRB_ADDRESS_DEVICE),
                 "ADDRESS_DEVICE") == 0,
          "TRB types are named for the boot record");
    CHECK(strcmp(xhci_completion_code_string(XHCI_COMPLETION_SUCCESS),
                 "SUCCESS") == 0,
          "completion codes are named for the boot record");
    CHECK(strcmp(xhci_completion_code_string(5U), "TRB_ERROR") == 0,
          "a TRB Error is named, not printed as a number");
    /*
     * ITEM-420: the codes are xHCI 1.1 table 6-90, and the table this driver
     * carried was shifted by two from 16 upwards.  The acceptance host answered
     * the Evaluate Context command with 19, which the record published as
     * `EVENT_RING_FULL` - an error the event ring never had - while the real
     * error was a Context State Error, i.e. the command itself was wrong.
     */
    CHECK(strcmp(xhci_completion_code_string(17U), "PARAMETER_ERROR") == 0,
          "parameter errors are named");
    CHECK(strcmp(xhci_completion_code_string(19U), "CONTEXT_STATE_ERROR") == 0,
          "a context state error is named, not mistaken for a full event ring");
    CHECK(strcmp(xhci_completion_code_string(21U), "EVENT_RING_FULL_ERROR") == 0,
          "a full event ring is named by its own code");
    CHECK(strcmp(xhci_completion_code_string(24U),
                 "COMMAND_RING_STOPPED") == 0,
          "a stopped command ring is named by its own code");
    CHECK(strcmp(xhci_completion_code_string(200U), "UNKNOWN") == 0,
          "an unclassified completion code is reported as UNKNOWN");
    /* ITEM-452: a controller the machine did not need says so instead of
       inventing a refusal it never made. */
    CHECK(strcmp(xhci_status_string(XHCI_STATUS_ROLE_TAKEN), "ROLE_TAKEN") == 0,
          "a role the machine already holds is named");
    CHECK(strcmp(xhci_speed_string(XHCI_SPEED_HIGH), "HIGH") == 0,
          "port speeds are named");
    CHECK(strcmp(xhci_speed_string(0U), "UNKNOWN") == 0,
          "an unknown port speed is reported as UNKNOWN");
}

/*
 * ITEM-473 (СРОЧНО-23): the event ring and the arm depth.
 *
 * The mouse arms sixteen transfers and the controller raises one completion
 * per transfer; the ring that holds those completions must be deeper than
 * every queue that feeds it, or a burst raises Event Ring Full (21) and the
 * controller stops publishing until the dequeue pointer moves.  The round-11
 * hardware run froze exactly this way, so the depth ceiling the `ARM` handle
 * clamps to is pinned below the ring here and by a static assertion in the
 * driver.
 */
static void test_event_ring_and_arm_depth(void) {
    CHECK(XHCI_ARM_DEPTH_MAX < XHCI_EVENT_TRBS,
          "the deepest armable queue is below the event ring's capacity");
    CHECK(XHCI_EVENT_TRBS * XHCI_TRB_BYTES == 4096U,
          "the event ring is the whole 4 KiB page it lives in");

    CHECK(xhci_arm_depth_clamp(0U) == XHCI_ARM_DEPTH_MIN,
          "ARM 0 answers the minimum depth, never a disabled endpoint");
    CHECK(xhci_arm_depth_clamp(3U) == 3U,
          "ARM 3 is the ITEM-469 depth that moved the pointer");
    CHECK(xhci_arm_depth_clamp(XHCI_ARM_DEPTH_MAX) == XHCI_ARM_DEPTH_MAX,
          "the compiled depth is accepted unchanged");
    CHECK(xhci_arm_depth_clamp(4096U) == XHCI_ARM_DEPTH_MAX,
          "a depth past the interrupt ring is clamped to the maximum");

    CHECK(strcmp(xhci_trb_type_string(XHCI_TRB_HOST_CONTROLLER_EVENT),
                 "HOST_CONTROLLER_EVENT") == 0,
          "the Host Controller Event is named for the record");
}

/*
 * ITEM-473: the endpoint-state reader.
 *
 * The diagnostics read Endpoint State (bits 2:0 of the Endpoint Context's
 * dword 0, xHCI 1.1 section 6.2.3) out of the device context the controller
 * was given, so "the endpoint went silent" reads as Running/Halted/Stopped
 * instead of being inferred from a missing report.  The reader uses the
 * driver's own context offset arithmetic, so the device-context index (the
 * slot context at 0, Endpoint 0 at DCI 1) is pinned against the builder's rule.
 */
static void test_endpoint_state(void) {
    uint8_t context[64U * 4U];
    uint32_t dword0;

    memset(context, 0, sizeof(context));
    /* DCI 3: the mouse's interrupt IN endpoint context. */
    dword0 = 1U; /* Running */
    memcpy(&context[64U * 3U], &dword0, sizeof(dword0));
    CHECK(xhci_endpoint_state(context, 64U, 3U) == 1U,
          "the endpoint state is read from the context at its DCI");
    CHECK(strcmp(xhci_endpoint_state_string(1U), "Running") == 0,
          "a running endpoint is named");
    CHECK(strcmp(xhci_endpoint_state_string(2U), "Halted") == 0,
          "a halted endpoint is named");
    CHECK(strcmp(xhci_endpoint_state_string(3U), "Stopped") == 0,
          "a stopped endpoint is named");
    CHECK(xhci_endpoint_state(context, 0U, 3U) == 0U,
          "a missing context answers Disabled, not a read through");
    CHECK(xhci_endpoint_state(NULL, 64U, 3U) == 0U,
          "a null context answers Disabled");
}

static void test_speed_tables(void) {
    CHECK(xhci_speed_ep0_max_packet(XHCI_SPEED_LOW) == 8U,
          "a low-speed device has an 8-byte control pipe");
    CHECK(xhci_speed_ep0_max_packet(XHCI_SPEED_FULL) == 8U,
          "a full-speed device defaults to an 8-byte control pipe");
    CHECK(xhci_speed_ep0_max_packet(XHCI_SPEED_HIGH) == 64U,
          "a high-speed device has a 64-byte control pipe");
    CHECK(xhci_speed_ep0_max_packet(XHCI_SPEED_SUPER) == 512U,
          "a SuperSpeed device has the 512-byte control pipe USB 3.0 defines");
    CHECK(xhci_speed_ep0_max_packet(XHCI_SPEED_SUPER_PLUS) == 512U,
          "a SuperSpeedPlus device has the same 512-byte control pipe");
    CHECK(xhci_speed_ep0_max_packet(9U) == 0U,
          "an unknown speed is not addressable");
}


static void read32(const void *base, size_t offset, uint32_t *value) {
    const uint8_t *bytes = (const uint8_t *)base;

    *value = (uint32_t)bytes[offset] |
             ((uint32_t)bytes[offset + 1U] << 8) |
             ((uint32_t)bytes[offset + 2U] << 16) |
             ((uint32_t)bytes[offset + 3U] << 24);
}

/*
 * ITEM-406: the acceptance host's keyboard is a low-speed USB 1.1 device
 * (Logitech K120, 046d:c31c) whose interrupt IN endpoint declares
 * wMaxPacketSize 8 and bInterval 10. Three numbers have to survive that:
 *
 *   * the port speed field a low-speed device reports (2) must be addressable,
 *     which the speed table above pins;
 *   * the endpoint's own packet size must reach the context (8, never a
 *     hardcoded 64);
 *   * the Interval field is *not* the descriptor's bInterval and not a
 *     microframe count either: xHCI 1.2 section 6.2.3.6 gives a low- or
 *     full-speed interrupt endpoint the period 2^Interval x 125 us with the
 *     field in 3..10, so a 10 ms descriptor is the exponent 6 (64 microframes,
 *     8 ms, rounded down) and a 1 ms descriptor is the exponent 3.  A
 *     high-speed endpoint takes bInterval as the exponent directly.  ITEM-438:
 *     the driver programmed bInterval x 8 - the microframe count - so the
 *     2026-09-17 hardware run's receiver, whose endpoint declares bInterval 1,
 *     was polled every 2^8 x 125 us = 32 ms instead of every millisecond.
 */
static void test_low_speed_endpoint_context(void) {
    uint8_t entry[XHCI_CONTEXT_64];
    uint32_t value = 0U;

    CHECK(xhci_endpoint_interval(XHCI_SPEED_LOW, 10U) == 6U,
          "a low-speed 10 ms interval is the exponent 6, 8 ms rounded down");
    CHECK(xhci_endpoint_interval(XHCI_SPEED_FULL, 10U) == 6U,
          "a full-speed 10 ms interval is the exponent 6 too");
    CHECK(xhci_endpoint_interval(XHCI_SPEED_FULL, 1U) == 3U,
          "the receiver's 1 ms endpoint is the exponent 3, not 8");
    CHECK(xhci_endpoint_interval(XHCI_SPEED_LOW, 1U) == 3U,
          "a low-speed 1 ms interval is the exponent 3");
    CHECK(xhci_endpoint_interval(XHCI_SPEED_LOW, 0U) == 3U,
          "a zero bInterval cannot encode a zero interval");
    CHECK(xhci_endpoint_interval(XHCI_SPEED_LOW, 255U) == 10U,
          "the descriptor's 255 caps at the 128 ms ceiling, the exponent 10");
    CHECK(xhci_endpoint_interval(XHCI_SPEED_FULL, 16U) == 7U,
          "16 ms is the exponent 7 for a full-speed endpoint too");
    CHECK(xhci_endpoint_interval(XHCI_SPEED_FULL, 8U) == 6U,
          "8 ms is the exponent 6");
    CHECK(xhci_endpoint_interval(XHCI_SPEED_FULL, 9U) == 6U,
          "a period that is no power of two rounds down to 8 ms");
    CHECK(xhci_endpoint_interval(XHCI_SPEED_HIGH, 10U) == 10U,
          "a high-speed endpoint's bInterval is already the exponent");
    CHECK(xhci_endpoint_interval(XHCI_SPEED_HIGH, 4U) == 4U,
          "the QEMU keyboard's high-speed interval is programmed as declared");
    CHECK(xhci_endpoint_interval(XHCI_SPEED_HIGH, 255U) == 16U,
          "a high-speed interval above 16 caps at 16");
    /* ITEM-438: a SuperSpeed endpoint's bInterval is that field's own value
       (1..16), and the port speed the 2026-09-17 run refused is addressable. */
    CHECK(xhci_endpoint_interval(XHCI_SPEED_SUPER, 10U) == 10U,
          "a SuperSpeed endpoint's bInterval is programmed as declared");
    CHECK(xhci_endpoint_interval(XHCI_SPEED_SUPER, 0U) == 1U,
          "a SuperSpeed bInterval of zero is the smallest legal one");
    CHECK(xhci_endpoint_interval(XHCI_SPEED_SUPER, 255U) == 16U,
          "a SuperSpeed interval above 16 caps at 16");
    CHECK(xhci_speed_ep0_max_packet(XHCI_SPEED_SUPER) == 512U,
          "a SuperSpeed device has a 512-byte default control pipe");
    CHECK(xhci_speed_ep0_max_packet(XHCI_SPEED_SUPER_PLUS) == 512U,
          "and so does a SuperSpeedPlus device");
    CHECK(xhci_endpoint_interval(9U, 10U) == 0U,
          "an unknown speed has no interval");

    /* The K120's interrupt endpoint, exactly as kernel/src/xhci.c programs it
       after reading the descriptor: 8 bytes, interval from the speed table. */
    memset(entry, 0xFF, sizeof(entry));
    xhci_endpoint_context(entry, XHCI_EP_TYPE_INTERRUPT_IN, 8U,
                          xhci_endpoint_interval(XHCI_SPEED_LOW, 10U),
                          0x513000U, 8U);
    read32(entry, 0U, &value);
    CHECK(((value >> 16) & 0xFFU) == 6U,
          "the low-speed context carries the exponent, not a microframe count");
    read32(entry, 4U, &value);
    CHECK(((value >> 3) & 0x7U) == XHCI_EP_TYPE_INTERRUPT_IN,
          "the low-speed endpoint is an interrupt IN endpoint");
    CHECK((value >> 16) == 8U,
          "the context carries the descriptor's 8-byte packet, not 64");

    /* And the slot context carries the low-speed port speed field. */
    memset(entry, 0xFF, sizeof(entry));
    xhci_slot_context(entry, XHCI_SPEED_LOW, 2U, 2U);
    read32(entry, 0U, &value);
    CHECK(((value >> 20) & 0xFU) == XHCI_SPEED_LOW,
          "the slot context carries the low-speed port speed field (2)");
    CHECK(((value >> 27) & 0x1FU) == 2U,
          "the slot context names the endpoint count it configured");
    read32(entry, 4U, &value);
    CHECK(((value >> 16) & 0xFFU) == 2U,
          "the slot context carries the root port the device is on");
    CHECK(xhci_speed_ep0_max_packet(XHCI_SPEED_LOW) == 8U,
          "a low-speed device is addressed with an 8-byte control pipe");
}

/*
 * ITEM-406: a PORTSC read-modify-write must not write PED back. PED is
 * write-one-to-clear (Linux: XHCI_PORT_RW1CS, xhci_port_state_to_neutral), so
 * the driver's old "read, then write the value back" sequence disabled the very
 * port it had just reset, and every device behind it failed to enumerate. QEMU
 * ignores writes to PED, so only this proof and a real controller can see it.
 */
static void test_portsc_neutral_write(void) {
    const uint32_t portsc =
        XHCI_PORTSC_CCS | XHCI_PORTSC_PED | XHCI_PORTSC_PR |
        (5U << XHCI_PORTSC_PLS_SHIFT) | XHCI_PORTSC_PP |
        (XHCI_SPEED_LOW << XHCI_PORTSC_SPEED_SHIFT) |
        XHCI_PORTSC_PIC_MASK | XHCI_PORTSC_LWS | XHCI_PORTSC_CSC |
        XHCI_PORTSC_PRC | XHCI_PORTSC_WAKE_MASK;
    const uint32_t neutral = xhci_portsc_neutral(portsc);

    CHECK((neutral & XHCI_PORTSC_PED) == 0U,
          "PED is never written back: writing 1 disables the port");
    CHECK((neutral & XHCI_PORTSC_CCS) == 0U,
          "connect status is read-only and stays out of the write");
    CHECK((neutral & XHCI_PORTSC_SPEED_MASK) == 0U,
          "the port speed field is read-only and stays out of the write");
    CHECK((neutral & XHCI_PORTSC_LWS) == 0U,
          "the link state strobe is not raised by a plain read-modify-write");
    CHECK((neutral & XHCI_PORTSC_PR) == 0U,
          "a port reset is not started by a write that did not ask for one");
    CHECK((neutral & XHCI_PORTSC_RW1C) == 0U,
          "the change bits are write-one-to-clear and stay out of the write");
    CHECK((neutral & XHCI_PORTSC_PP) != 0U,
          "the port stays powered");
    CHECK((neutral & XHCI_PORTSC_PIC_MASK) == XHCI_PORTSC_PIC_MASK,
          "the port indicator state is preserved");
    CHECK((neutral & XHCI_PORTSC_PLS_MASK) != 0U,
          "the link state is preserved");
    CHECK((neutral & XHCI_PORTSC_WAKE_MASK) == XHCI_PORTSC_WAKE_MASK,
          "the wake enables are preserved");
    CHECK(xhci_portsc_neutral(0U) == 0U,
          "a cleared PORTSC stays cleared");
}

/*
 * ITEM-406: the machine-wide rule. The acceptance host is three controllers
 * with the keyboard on one of them; the scan must reach it, and a controller
 * whose only device is not a boot keyboard must not end the scan.  ITEM-452:
 * nothing ends it - a bound keyboard makes the verdict OK and the controllers
 * behind it are still visited, because the pointer may be there and a
 * controller that is never opened cannot name itself.
 */
static void test_scan_policy(void) {
    struct xhci_scan scan;
    /* The driver's working buffer: reused and rewritten by every probe, so a
       policy that kept the pointer would publish whatever landed there last. */
    char working[XHCI_SCAN_DETAIL_BYTES];

    strcpy(working, "P7_CONFIG_PARSE_NO_BOOT_KEYBOARD");
    xhci_scan_init(&scan, 4U, 8U);
    CHECK(scan.status == XHCI_STATUS_CONTROLLER_ABSENT,
          "a scan that found nothing starts from the absent verdict");

    /* A controller whose device is not a keyboard: the scan continues. */
    xhci_scan_begin_controller(&scan);
    CHECK(!xhci_scan_note_controller(&scan, XHCI_STATUS_NOT_A_KEYBOARD,
                                     working, false),
          "a controller of non-keyboard devices does not end the scan");
    CHECK(!scan.selected, "nothing is selected yet");
    CHECK(scan.controllers_seen == 1U, "one controller was visited");

    /* Rejections rank by how much they say about the machine. */
    CHECK(xhci_scan_progress(XHCI_STATUS_NOT_A_KEYBOARD) >
              xhci_scan_progress(XHCI_STATUS_ENUMERATION_FAILED),
          "a described device says more than a failed transfer");
    CHECK(xhci_scan_progress(XHCI_STATUS_ENUMERATION_FAILED) >
              xhci_scan_progress(XHCI_STATUS_UNSUPPORTED_SPEED),
          "a failed transfer says more than an unusable speed");
    CHECK(xhci_scan_progress(XHCI_STATUS_UNSUPPORTED_SPEED) >
              xhci_scan_progress(XHCI_STATUS_NO_DEVICE),
          "a connected device says more than an empty controller");
    CHECK(xhci_scan_progress(XHCI_STATUS_NO_DEVICE) >
              xhci_scan_progress(XHCI_STATUS_ROLE_TAKEN),
          "an empty controller says more than one whose devices the machine "
          "already had roles for");
    CHECK(xhci_scan_progress(XHCI_STATUS_NO_DEVICE) >
              xhci_scan_progress(XHCI_STATUS_RESET_TIMEOUT),
          "a controller that came up says more than one that did not");
    CHECK(xhci_scan_progress(XHCI_STATUS_HALT_TIMEOUT) >
              xhci_scan_progress(XHCI_STATUS_NO_MEMORY_BAR),
          "a controller that answered says more than one with no BAR");
    CHECK(xhci_scan_progress(XHCI_STATUS_CONTROLLER_ABSENT) >
              xhci_scan_progress(XHCI_STATUS_UNKNOWN),
          "an absent controller says more than an unclassified one");
    CHECK(!xhci_scan_prefers(XHCI_STATUS_NO_DEVICE, XHCI_STATUS_NO_DEVICE),
          "a tie keeps the evidence of the earlier controller");

    /* Later controllers may not overwrite the kept detail, even though the
       driver's working buffer is rewritten by every probe. */
    xhci_scan_begin_controller(&scan);
    strcpy(working, "OVERWRITTEN");
    CHECK(!xhci_scan_note_controller(&scan, XHCI_STATUS_NO_DEVICE,
                                     working, false),
          "a less informative controller does not end the scan");
    CHECK(scan.status == XHCI_STATUS_NOT_A_KEYBOARD,
          "the published reason stays the most informative one");
    CHECK(strcmp(scan.detail, "P7_CONFIG_PARSE_NO_BOOT_KEYBOARD") == 0,
          "the kept detail is the scan's own copy, not the caller's buffer");

    /* ITEM-452: a usable keyboard on the third controller is bound and makes
       the machine verdict OK - and the fourth controller is still visited. */
    xhci_scan_begin_controller(&scan);
    CHECK(xhci_scan_note_controller(&scan, XHCI_STATUS_OK, "OK", true),
          "a boot keyboard is bound on the controller it was found on");
    CHECK(scan.selected && scan.status == XHCI_STATUS_OK,
          "the scan publishes the keyboard it found");
    CHECK(scan.controllers_seen == 3U,
          "the keyboard was found on the third controller the scan visited");
    CHECK(strcmp(scan.detail, "OK") == 0, "the selected detail is OK");

    xhci_scan_begin_controller(&scan);
    CHECK(!xhci_scan_note_controller(&scan, XHCI_STATUS_NOT_A_KEYBOARD,
                                     "P9_CONFIG_PARSE_NO_HID_MOUSE", false),
          "a refusal behind the keyboard's controller is not a keyboard");
    CHECK(scan.selected && scan.status == XHCI_STATUS_OK,
          "the machine verdict stays the keyboard the machine has");
    CHECK(strcmp(scan.detail, "OK") == 0,
          "and a later refusal does not overwrite its detail");
    CHECK(scan.controllers_seen == 4U,
          "the controller behind the keyboard was visited too");
    CHECK(scan.controllers_seen == scan.controllers_total,
          "the walk reached the machine's last controller");
}

static void test_scan_attempt_budget(void) {
    struct xhci_scan scan;

    xhci_scan_init(&scan, 2U, 3U);
    xhci_scan_begin_controller(&scan);
    CHECK(xhci_scan_take_attempt(&scan), "the first attempt is within budget");
    CHECK(xhci_scan_take_attempt(&scan), "the second attempt is within budget");
    CHECK(xhci_scan_take_attempt(&scan), "the third attempt is within budget");
    CHECK(!xhci_scan_take_attempt(&scan),
          "a controller cannot spend more attempts than its budget");
    xhci_scan_begin_controller(&scan);
    CHECK(xhci_scan_take_attempt(&scan),
          "the next controller gets its own attempt budget");
    CHECK(scan.controllers_seen == 2U, "both controllers were visited");

    /* A scan over no controller at all publishes the absent verdict, which is
       what a machine without an xHCI function must keep reporting. */
    xhci_scan_init(&scan, 0U, 3U);
    CHECK(scan.status == XHCI_STATUS_CONTROLLER_ABSENT,
          "a machine with no controller publishes CONTROLLER_ABSENT");
    CHECK(strcmp(scan.detail, "NO_XHCI_FUNCTION") == 0,
          "and names the absent function");
}

/*
 * ITEM-406: the machine the acceptance host has, driven end to end through the
 * same two pieces the kernel uses - the descriptor walk and the scan policy.
 * Controller 1 carries a receiver whose interfaces are not a boot keyboard,
 * controller 2 carries the K120 on a root port, controller 3 carries the
 * pointer (ITEM-452: the owner has the keyboard and the receiver on two
 * different controllers). The first version of the driver would have stopped at
 * controller 1 (or at the first device it could address).
 */
static void test_machine_scan_reaches_a_later_controller(void) {
    /* The acceptance host's K120: two interfaces, boot keyboard first. */
    static const uint8_t k120_config[] = {
        0x09, 0x02, 0x3b, 0x00, 0x02, 0x01, 0x00, 0xa0, 0x32,
        0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,
        0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3f, 0x00,
        0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0a,
        0x09, 0x04, 0x01, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
        0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x17, 0x00,
        0x07, 0x05, 0x82, 0x03, 0x04, 0x00, 0xff
    };
    /* The wireless receiver on controller 1: HID, but no boot keyboard. */
    static const uint8_t receiver_config[] = {
        0x09, 0x02, 0x3b, 0x00, 0x02, 0x01, 0x00, 0xa0, 0x32,
        0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
        0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x1f, 0x00,
        0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0a,
        0x09, 0x04, 0x01, 0x00, 0x01, 0x03, 0x01, 0x02, 0x00,
        0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x32, 0x00,
        0x07, 0x05, 0x82, 0x03, 0x04, 0x00, 0x0a
    };
    struct xhci_scan scan;
    struct usb_hid_config config;

    /* Controller 1, port 7: the receiver. */
    xhci_scan_init(&scan, 3U, 8U);
    xhci_scan_begin_controller(&scan);
    CHECK(xhci_scan_take_attempt(&scan), "the receiver costs one attempt");
    config = usb_hid_parse_config(receiver_config, sizeof(receiver_config));
    /* ITEM-438: the receiver's HID interface is a *candidate* - only its report
       descriptor can say whether it is a keyboard - and the walk's verdict on
       it is still not a keyboard: this proof's config carries no report
       descriptor, so the driver's next step would be that read.  What matters
       to the scan is unchanged: whatever the verdict, the controller with the
       receiver does not end the machine scan. */
    CHECK(config.status == USB_HID_STATUS_OK,
          "the receiver's HID interface is a report-descriptor candidate");
    CHECK(config.report_descriptor_needed,
          "so the driver would read its report descriptor before binding it");
    CHECK(!xhci_scan_note_controller(&scan, XHCI_STATUS_NOT_A_KEYBOARD,
                                     "P7_REPORT_DESCRIPTOR_NOT_KEYBOARD",
                                     false),
          "the controller with the receiver does not end the scan");

    /* Controller 2, port 2: the keyboard, as a low-speed device. */
    xhci_scan_begin_controller(&scan);
    config = usb_hid_parse_config(k120_config, sizeof(k120_config));
    CHECK(config.status == USB_HID_STATUS_OK,
          "the K120 configuration is a boot keyboard");
    CHECK(config.endpoint_max_packet == 8U,
          "its report endpoint carries 8 bytes");
    CHECK(xhci_endpoint_interval(XHCI_SPEED_LOW,
                                 config.endpoint_interval) == 6U,
          "its 10 ms low-speed interval is programmed as the exponent 6");
    CHECK(xhci_scan_note_controller(&scan, XHCI_STATUS_OK, "OK", true),
          "the keyboard on the second controller is bound");

    /*
     * ITEM-452: controller 3 is still visited.  On the owner's machine it is
     * the controller the Logitech receiver sits on, and the walk has to reach
     * it with the keyboard already bound - the whole point of this item.
     */
    xhci_scan_begin_controller(&scan);
    CHECK(xhci_scan_take_attempt(&scan),
          "the pointed-at device on the later controller costs one attempt");
    CHECK(!xhci_scan_note_controller(&scan, XHCI_STATUS_NO_DEVICE,
                                     "NO_CONNECTED_DEVICE", false),
          "an empty controller behind the keyboard is not a keyboard");
    CHECK(scan.selected, "the machine ends up with a keyboard");
    CHECK(scan.controllers_seen == 3U,
          "every controller was visited, including the one behind the keyboard");
    CHECK(scan.status == XHCI_STATUS_OK, "the published verdict is OK");
    CHECK(strcmp(scan.detail, "OK") == 0,
          "and the keyboard's detail is the one that stands");

    /* The same machine without the keyboard controller: the receiver alone must
       publish why, and it must name the port and the stage. */
    xhci_scan_init(&scan, 3U, 8U);
    xhci_scan_begin_controller(&scan);
    (void)xhci_scan_note_controller(&scan, XHCI_STATUS_NOT_A_KEYBOARD,
                                    "P7_CONFIG_PARSE_NO_BOOT_KEYBOARD", false);
    xhci_scan_begin_controller(&scan);
    (void)xhci_scan_note_controller(&scan, XHCI_STATUS_NO_DEVICE,
                                    "NO_CONNECTED_DEVICE", false);
    xhci_scan_begin_controller(&scan);
    (void)xhci_scan_note_controller(&scan, XHCI_STATUS_NO_DEVICE,
                                    "NO_CONNECTED_DEVICE", false);
    CHECK(!scan.selected, "a machine of receivers and empty ports has no input");
    CHECK(scan.controllers_seen == scan.controllers_total,
          "and every one of its controllers was still visited");
    CHECK(scan.status == XHCI_STATUS_NOT_A_KEYBOARD,
          "the published reason is the most informative rejection");
    CHECK(strcmp(scan.detail, "P7_CONFIG_PARSE_NO_BOOT_KEYBOARD") == 0,
          "the published detail names the port and the stage");
}

/*
 * ITEM-407: the slot bound. The controller that opened this ITEM advertises 127
 * device slots, and the driver used to refuse it with BAD_HCSPARAMS because its
 * single constant was both "the biggest controller I accept" and "the room in
 * my device context table" - a refusal this driver invented, on a controller
 * that had already answered with a perfectly valid HCSPARAMS1.
 *
 * The two bounds are separate now: the controller is accepted for any value its
 * eight-bit field can carry, the array that must be sized for the controller is
 * the (MaxSlots+1)-entry DCBAA, and what is capped is the number of slots this
 * driver enables - which is what the per-slot structures are sized for.
 */
static void test_capability_window(void) {
    CHECK(xhci_caplength_valid(0x80U, 0x10000U),
          "Intel 7 Series CAPLENGTH 0x80 is a valid operational offset");
    CHECK(xhci_caplength_valid(0x20U, 0x10000U) &&
              xhci_caplength_valid(0x40U, 0x10000U),
          "smaller conforming capability blocks remain valid");
    CHECK(xhci_caplength_valid(0xFCU, 0x10000U),
          "the largest aligned eight-bit offset is valid inside the window");
    CHECK(!xhci_caplength_valid(0U, 0x10000U) &&
              !xhci_caplength_valid(0x1CU, 0x10000U) &&
              !xhci_caplength_valid(0x81U, 0x10000U) &&
              !xhci_caplength_valid(0xFFU, 0x10000U) &&
              !xhci_caplength_valid(0x100U, 0x10000U),
          "missing, undersized, unaligned and non-field offsets are refused");
    CHECK(!xhci_caplength_valid(0x80U, 0xBBU) &&
              xhci_caplength_valid(0x80U, 0xBCU),
          "the fixed operational registers through CONFIG fit exactly");
    CHECK(!xhci_caplength_valid(0x20U, 1U),
          "a short mapped window cannot underflow the bounds calculation");
    CHECK(xhci_caplength_valid(0x80U, 0U),
          "an unspecified host-model window keeps the existing convention");
}

static void test_slot_bound(void) {
    /* The acceptance host's A520 xHCI: HCSPARAMS1 MaxSlots = 127. */
    CHECK(xhci_max_slots_valid(127U),
          "a controller advertising 127 slots is a valid controller");
    CHECK(xhci_slots_enabled(127U) == XHCI_DRIVER_SLOTS,
          "the driver enables only its own slot bound on it");
    CHECK(xhci_slots_enabled(127U) <= XHCI_DRIVER_SLOTS,
          "the enabled slot count never exceeds the device context table");
    CHECK(xhci_max_slots_valid(XHCI_MAX_SLOTS),
          "the specification's field maximum is a valid value");
    CHECK(xhci_slots_enabled(XHCI_MAX_SLOTS) == XHCI_DRIVER_SLOTS,
          "the widest controller is accepted and still bounded");
    CHECK(xhci_slots_enabled(1U) == 1U,
          "a small controller enables exactly its own slots");
    CHECK(xhci_slots_enabled(XHCI_DRIVER_SLOTS) == XHCI_DRIVER_SLOTS,
          "the bound itself is enabled in full");
    CHECK(xhci_slots_enabled(XHCI_DRIVER_SLOTS + 1U) == XHCI_DRIVER_SLOTS,
          "one slot above the bound is capped, not refused");
    CHECK(xhci_slots_enabled(0U) == 0U,
          "a controller with no slots has no valid HCSPARAMS1");
    CHECK(!xhci_max_slots_valid(0U),
          "MaxSlots 0 is the only slot value this driver refuses");
    CHECK(!xhci_max_slots_valid(XHCI_MAX_SLOTS + 1U),
          "a slot count the eight-bit field cannot carry is a bad read");

    /* The DCBAA is the controller's, not the driver's: MaxSlots+1 entries of
       eight bytes each (xHCI 1.1 section 6.1, entry 0 is the scratchpad
       array). The pool the driver claims must hold that for the widest
       controller, and the device context table must stay inside one page. */
    CHECK(xhci_dcbaa_bytes(127U) == 128U * 8U,
          "a 127-slot controller gets 128 DCBAA entries");
    CHECK(xhci_dcbaa_bytes(XHCI_MAX_SLOTS) ==
              ((size_t)XHCI_MAX_SLOTS + 1U) * sizeof(uint64_t),
          "the widest controller's array is sized for its own MaxSlots");
    CHECK(xhci_dcbaa_bytes(1U) == 2U * 8U,
          "a one-slot controller's array is two entries");
    CHECK(xhci_dcbaa_bytes(0U) == 0U,
          "an invalid slot count has no array size");
    CHECK((size_t)XHCI_DRIVER_SLOTS * 64U == 4096U,
          "the device context table is one page at its fixed offset");
    CHECK(xhci_slot_within_driver_bound(1U),
          "slot 1 is inside the table");
    CHECK(xhci_slot_within_driver_bound(XHCI_DRIVER_SLOTS),
          "the last enabled slot is inside the table");
    CHECK(!xhci_slot_within_driver_bound(0U),
          "there is no slot 0 to write a device context for");
    CHECK(!xhci_slot_within_driver_bound(XHCI_DRIVER_SLOTS + 1U),
          "a slot above the bound has no device context to point at");
    /* The A520's 14 ports, and the Renoir controller's 6, are both valid. */
    CHECK(xhci_max_ports_valid(14U) && xhci_max_ports_valid(6U),
          "the acceptance host's port counts are valid");
    CHECK(!xhci_max_ports_valid(0U) &&
              !xhci_max_ports_valid(XHCI_MAX_PORTS + 1U),
          "only a port count the field cannot carry is refused");
}

/*
 * ITEM-407: the port evidence. One more photograph of the acceptance host's
 * screen has to say whether every port really reported "no device connected" or
 * whether the driver was reading the wrong thing, so what the record publishes
 * is formatted here and pinned byte for byte.
 */
static void test_port_evidence(void) {
    const uint32_t portsc[XHCI_PORT_EVIDENCE_MAX] = {
        0x00000600U, 0x00000603U
    };
    char buffer[XHCI_PORT_EVIDENCE_BYTES];
    char exact[XHCI_PORT_EVIDENCE_BYTES];
    char small[16];

    CHECK(XHCI_PORT_EVIDENCE_BYTES == XHCI_PORT_EVIDENCE_MAX * 11U + 2U,
          "the evidence buffer holds every value and its separator");
    CHECK(xhci_port_evidence_count(6U) == XHCI_PORT_EVIDENCE_MAX,
          "a six-port controller is cut at the evidence budget");
    CHECK(xhci_port_evidence_count(1U) == 1U,
          "a one-port controller publishes its port");
    CHECK(xhci_port_evidence_count(0U) == 0U,
          "an invalid port count publishes nothing");

    memset(buffer, 0x7F, sizeof(buffer));
    CHECK(xhci_format_port_evidence(buffer, sizeof(buffer), portsc,
                                    XHCI_PORT_EVIDENCE_MAX) ==
              XHCI_PORT_EVIDENCE_MAX,
          "every value fits the worst-case buffer");
    CHECK(strcmp(buffer, "0x00000600,0x00000603") == 0,
          "the evidence is uppercase hex, comma separated, no leading or "
          "trailing separator");

    /* The controller's real port count is what is published, so a list that
       stops early is never mistaken for a controller with two ports. */
    CHECK(xhci_format_port_evidence(buffer, sizeof(buffer), portsc, 1U) == 1U,
          "only the ports that were read are formatted");
    CHECK(strcmp(buffer, "0x00000600") == 0,
          "a complete list has no trailing comma");

    /* A buffer too small for a value writes what fits and says how much. */
    memset(small, 0x7F, sizeof(small));
    CHECK(xhci_format_port_evidence(small, sizeof(small), portsc,
                                    XHCI_PORT_EVIDENCE_MAX) == 1U,
          "a short buffer takes one value and stops");
    CHECK(strcmp(small, "0x00000600,") == 0,
          "the trailing comma marks evidence that was cut short");

    /* Nothing is written at all when even one value does not fit. */
    memset(small, 0x7F, sizeof(small));
    CHECK(xhci_format_port_evidence(small, 9U, portsc,
                                    XHCI_PORT_EVIDENCE_MAX) == 0U,
          "a buffer of nine bytes holds no ten-character value");
    CHECK(small[0] == '\0', "and the buffer is still a valid empty string");
    CHECK(xhci_format_port_evidence(exact, sizeof(exact), NULL, 2U) == 0U,
          "no port values means no evidence");
    CHECK(exact[0] == '\0', "and an empty string");
    CHECK(xhci_format_port_evidence(NULL, sizeof(exact), portsc, 1U) == 0U,
          "a missing buffer is refused rather than written through");

    /* Which ports were powered: one value to read instead of a scan of the
       list, because "the port was never powered" is the failure this ITEM
       exists for. */
    CHECK(xhci_ports_powered_mask(portsc, 2U) == 0x3U,
          "the PP bit of each published PORTSC is compressed into a mask");
    CHECK(xhci_ports_powered_mask(portsc, 1U) == 0x1U,
          "only the published ports are in the mask");
    CHECK(xhci_ports_powered_mask(portsc, 0U) == 0U,
          "no ports means no mask");
    CHECK(xhci_ports_powered_mask(NULL, 2U) == 0U,
          "a missing list has no power mask");
}

/*
 * ITEM-453: the port bring-up decision.  The 2026-09-19 hardware run published
 * `portsc=0x000002A0,0x000002A0 pp=3 ppoff=0` for the six-port controller the
 * owner's receiver is attached to, and `reason=NO_DEVICE
 * detail=NO_CONNECTED_DEVICE` for it.  0x000002A0 is PORTSC.PP set with the link
 * in RxDetect (PLS 5): a powered port that samples the connect status and
 * answers "nothing attached" - the one shape where a second read, not a write,
 * is the honest next step.  Every other shape has to be brought up first, and
 * the plan is what says how; it is pure so this proof drives it with the exact
 * values the owner's machine published.
 */
static void test_port_bring_up_plan(void) {
    struct xhci_port_bring_up plan;
    uint32_t state;

    /* The owner's evidence, value for value. */
    xhci_port_bring_up_plan(0x000002A0U, &plan);
    CHECK(plan.was == 0x000002A0U,
          "the plan keeps the raw PORTSC it decided on");
    CHECK(plan.link == XHCI_PORT_LINK_RXDETECT,
          "0x2A0 is a link in RxDetect");
    CHECK(plan.powered, "0x2A0 has PORTSC.PP set");
    CHECK(!plan.connected, "0x2A0 reports no connect");
    CHECK(plan.samples, "RxDetect samples the connect status");
    CHECK(!plan.power_on, "a powered port is not powered again");
    CHECK(!plan.link_kick, "a link already in RxDetect is not kicked");
    CHECK(plan.settle_ms == 0U,
          "a port the driver did not touch is not waited on twice: the "
          "controller's own window after the reset covers it, and this port "
          "answered the same value before and after it");

    /* A port that came up unpowered and Disabled: this is the closed loop
       ITEM-407 opened, and the plan does both halves of the bring-up. */
    state = XHCI_PORT_LINK_DISABLED << XHCI_PORTSC_PLS_SHIFT;
    xhci_port_bring_up_plan(state, &plan);
    CHECK(plan.power_on, "an unpowered port is powered before it is read");
    CHECK(plan.link_kick, "a Disabled link is moved to RxDetect");
    CHECK(!plan.samples, "a Disabled port does not sample connect");
    CHECK(plan.settle_ms == XHCI_PORT_SETTLE_MS,
          "and the change is given its whole debounce window");
    CHECK(!plan.connected, "a Disabled port has no connect to report");

    /* Inactive (6) and Compliance (10) do not sample connect either, and a
       connect read on them is exactly the "no device" answer that is wrong. */
    state = XHCI_PORTSC_PP | (XHCI_PORT_LINK_INACTIVE << XHCI_PORTSC_PLS_SHIFT);
    xhci_port_bring_up_plan(state, &plan);
    CHECK(plan.link_kick && !plan.power_on,
          "an Inactive powered link is kicked, not re-powered");
    state = XHCI_PORTSC_PP | (XHCI_PORT_LINK_COMPLIANCE << XHCI_PORTSC_PLS_SHIFT);
    xhci_port_bring_up_plan(state, &plan);
    CHECK(plan.link_kick && !plan.samples,
          "a port in Compliance is moved back to RxDetect");

    /* A connected port needs nothing: no write, no wait, and the scan can
       believe it at once. */
    state = XHCI_PORTSC_CCS | XHCI_PORTSC_PED | XHCI_PORTSC_PP |
            (XHCI_SPEED_LOW << XHCI_PORTSC_SPEED_SHIFT);
    xhci_port_bring_up_plan(state, &plan);
    CHECK(plan.connected && plan.samples, "a connected port reports itself");
    CHECK(!plan.power_on && !plan.link_kick,
          "a connected port is not written to at all");
    CHECK(plan.settle_ms == 0U, "and is not waited on");

    /* A port already in U0 (a running link) is not kicked either. */
    state = XHCI_PORTSC_CCS | XHCI_PORTSC_PP |
            (XHCI_PORT_LINK_U0 << XHCI_PORTSC_PLS_SHIFT);
    xhci_port_bring_up_plan(state, &plan);
    CHECK(!plan.link_kick, "U0 is a link state the port may keep");

    /* A missing plan is refused rather than written through. */
    xhci_port_bring_up_plan(0x000002A0U, NULL);
}

static void test_port_link_state(void) {
    const uint32_t disabled = XHCI_PORTSC_PED | XHCI_PORTSC_PP |
                              XHCI_PORTSC_CSC | XHCI_PORTSC_PRC |
                              (XHCI_PORT_LINK_DISABLED << XHCI_PORTSC_PLS_SHIFT);
    const uint32_t kicked = xhci_portsc_link_kick(disabled);

    CHECK(xhci_port_link_state(0x000002A0U) == XHCI_PORT_LINK_RXDETECT,
          "PORTSC.PLS is read from bits 8:5");
    CHECK(xhci_port_link_state(XHCI_PORTSC_PP) == XHCI_PORT_LINK_U0,
          "a port with a zero link field is in U0");
    CHECK(strcmp(xhci_port_link_state_string(XHCI_PORT_LINK_RXDETECT),
                 "RxDetect") == 0,
          "RxDetect names itself");
    CHECK(strcmp(xhci_port_link_state_string(XHCI_PORT_LINK_DISABLED),
                 "Disabled") == 0,
          "Disabled names itself");
    CHECK(strcmp(xhci_port_link_state_string(XHCI_PORT_LINK_U3), "U3") == 0,
          "U3 names itself");
    CHECK(strcmp(xhci_port_link_state_string(12U), "UNKNOWN") == 0,
          "a reserved link state is published as unknown");

    CHECK(xhci_port_link_samples_connect(XHCI_PORT_LINK_RXDETECT),
          "RxDetect samples connect");
    CHECK(xhci_port_link_samples_connect(XHCI_PORT_LINK_U3),
          "a suspended port keeps its connect");
    CHECK(!xhci_port_link_samples_connect(XHCI_PORT_LINK_DISABLED),
          "a Disabled port does not sample connect");
    CHECK(!xhci_port_link_samples_connect(XHCI_PORT_LINK_INACTIVE),
          "an Inactive port does not sample connect");
    CHECK(!xhci_port_link_samples_connect(XHCI_PORT_LINK_COMPLIANCE),
          "a Compliance port does not sample connect");
    CHECK(!xhci_port_link_samples_connect(XHCI_PORT_LINK_TEST_MODE),
          "a Test Mode port does not sample connect");
    CHECK(!xhci_port_link_samples_connect(12U),
          "a reserved link state is not trusted to sample connect");

    /* The kick is the only write that moves a link, and it still never carries
       PED or a write-one-to-clear change bit (the ITEM-406 rule). */
    CHECK((kicked & XHCI_PORTSC_PLS_MASK) ==
              (XHCI_PORT_LINK_RXDETECT << XHCI_PORTSC_PLS_SHIFT),
          "the kick asks for RxDetect");
    CHECK((kicked & XHCI_PORTSC_LWS) != 0U,
          "and strobes the link state so the write takes effect");
    CHECK((kicked & XHCI_PORTSC_PP) != 0U, "power is preserved");
    CHECK((kicked & XHCI_PORTSC_PED) == 0U, "PED is never written back");
    CHECK((kicked & XHCI_PORTSC_RW1C) == 0U,
          "no write-one-to-clear change bit is carried");
}

/*
 * ITEM-453: the per-port record.  Every port of every controller publishes its
 * own line with the raw PORTSC value before and after the bring-up, so "which
 * port, and what did it answer" is answered for all of them - the 2026-09-19
 * record named two of six and could not tell "empty" from "never read".
 */
/*
 * ITEM-545: the controller's post-reset window.  Linux's xhci_start() sets a
 * 500 ms grace period because a SuperSpeed device is only visible after its
 * link finishes training (drivers/usb/host/xhci.c: "run_graceperiod"), and a
 * connect on a USB 2 port arrives while the USB 3 port beside it is still
 * training.  The driver's window therefore may not end because *one* port
 * answered: it ends when no port is left that can still report a connect.
 */
static void test_port_connect_pending(void) {
    const uint32_t connected = XHCI_PORTSC_CCS | XHCI_PORTSC_PED |
                               XHCI_PORTSC_PP |
                               (XHCI_PORT_LINK_U0 << XHCI_PORTSC_PLS_SHIFT);
    const uint32_t training = XHCI_PORTSC_PP |
                              (XHCI_PORT_LINK_RXDETECT << XHCI_PORTSC_PLS_SHIFT);
    const uint32_t polling = XHCI_PORTSC_CCS | XHCI_PORTSC_PP |
                             (XHCI_PORT_LINK_POLLING << XHCI_PORTSC_PLS_SHIFT);
    const uint32_t disabled = XHCI_PORTSC_PP |
                              (XHCI_PORT_LINK_DISABLED << XHCI_PORTSC_PLS_SHIFT);

    CHECK(!xhci_port_connect_pending(connected),
          "a connected port has nothing left to wait for");
    CHECK(xhci_port_connect_pending(training),
          "a powered port still sampling connect keeps the window open");
    CHECK(!xhci_port_connect_pending(polling),
          "a port that already reported a connect is not pending");
    CHECK(!xhci_port_connect_pending(disabled),
          "a port that cannot sample connect is not pending");
}

static void test_endpoint_context_burst(void) {
    uint8_t entry[XHCI_CONTEXT_64];
    uint32_t value = 0U;

    /* xHCI 1.1 section 6.2.3.5: Max Burst Size is dword 4 bits 23:16, and it
       is the endpoint companion's bMaxBurst.  The owner's stick (24a9:205a)
       declares 3 and QEMU's usb-bot 15; both are SuperSpeed bulk endpoints,
       and a context that does not carry the burst tells the controller the
       endpoint can send one packet per burst. */
    memset(entry, 0, sizeof(entry));
    xhci_endpoint_context_burst(entry, XHCI_EP_TYPE_BULK_IN, 1024U, 0U,
                                0x12345000U, 1024U, 15U);
    read32(entry, 4U, &value);
    CHECK(((value >> 16) & 0xFFFFU) == 1024U,
          "the SuperSpeed bulk context carries its 1024-byte packet");
    CHECK(((value >> 3) & 0x7U) == XHCI_EP_TYPE_BULK_IN,
          "and is a bulk IN endpoint");
    read32(entry, 16U, &value);
    CHECK(((value >> 16) & 0xFFU) == 15U,
          "the companion's bMaxBurst is programmed into Max Burst Size");
    CHECK((value & 0xFFFFU) == 1024U,
          "the average TRB length is still the endpoint's own packet");

    /* The burst is 8 bits wide and the other fields of dword 4 stay zero. */
    memset(entry, 0xFF, sizeof(entry));
    xhci_endpoint_context_burst(entry, XHCI_EP_TYPE_BULK_OUT, 512U, 0U,
                                0x4000U, 512U, 255U);
    read32(entry, 16U, &value);
    CHECK(((value >> 16) & 0xFFU) == 255U,
          "the whole companion byte is kept");
    CHECK((value & 0xFFFFU) == 512U, "and the low half is unchanged");

    /* The plain builder every other endpoint uses declares burst 0. */
    memset(entry, 0, sizeof(entry));
    xhci_endpoint_context(entry, XHCI_EP_TYPE_CONTROL, 512U, 0U,
                          0x12000U, 8U);
    read32(entry, 16U, &value);
    CHECK(((value >> 16) & 0xFFU) == 0U,
          "an endpoint without a companion bursts one packet");
}

static void test_port_record(void) {
    struct xhci_port_bring_up plan;
    char buffer[XHCI_PORT_RECORD_BYTES];
    char small[24];
    size_t length;

    xhci_port_bring_up_plan(0x000002A0U, &plan);
    memset(buffer, 0x7F, sizeof(buffer));
    length = xhci_format_port_record(buffer, sizeof(buffer), 0x05U, 0x00U, 0x3U,
                                     3U, "USB2", &plan, 0x000002A0U, 100U);
    CHECK(length == strlen(buffer), "the record reports its own length");
    CHECK(strcmp(buffer,
                 "[XHCI-PORT] pci=05:00.3 port=3 group=USB2"
                 " was=0x000002A0 now=0x000002A0"
                 " pp=1 ccs=0 pls=5 pon=0 kick=0 wait=100") == 0,
          "the port record names the controller, the port, the group, both raw "
          "values and the decoded state");

    /* The worst case the budget reserves: every field at full width. */
    xhci_port_bring_up_plan(XHCI_PORTSC_PP | XHCI_PORTSC_CCS |
                                (XHCI_PORT_LINK_RESUME << XHCI_PORTSC_PLS_SHIFT),
                            &plan);
    plan.power_on = true;
    plan.link_kick = true;
    length = xhci_format_port_record(buffer, sizeof(buffer), 0xFFU, 0xFFU, 0xFU,
                                     255U, "USB3", &plan, 0xFFFFFFFFU, 1000U);
    CHECK(length > 0U && length < XHCI_PORT_RECORD_BYTES,
          "the worst-case record fits the budget it reserves");
    CHECK(length <= 320U,
          "and the console's one-line budget, which is what matters");
    CHECK(strlen(buffer) == length, "the worst case is a whole string");

    /* A buffer too small writes nothing at all rather than half a line. */
    memset(small, 0x7F, sizeof(small));
    CHECK(xhci_format_port_record(small, sizeof(small), 0x05U, 0x00U, 0x3U, 3U,
                                  "USB2", &plan, 0x000002A0U, 100U) == 0U,
          "a short buffer is refused");
    CHECK(small[0] == '\0', "and left a valid empty string");
    CHECK(xhci_format_port_record(NULL, sizeof(buffer), 0x05U, 0x00U, 0x3U, 3U,
                                  "USB2", &plan, 0x000002A0U, 100U) == 0U,
          "a missing buffer is refused rather than written through");
    CHECK(xhci_format_port_record(buffer, 0U, 0x05U, 0x00U, 0x3U, 3U, "USB2",
                                  &plan, 0x000002A0U, 100U) == 0U,
          "a zero-sized buffer is refused too");
    CHECK(xhci_format_port_record(buffer, sizeof(buffer), 0x05U, 0x00U, 0x3U, 3U,
                                  NULL, &plan, 0x000002A0U, 100U) == 0U,
          "a missing group name is refused");
    CHECK(xhci_format_port_record(buffer, sizeof(buffer), 0x05U, 0x00U, 0x3U, 3U,
                                  "USB2", NULL, 0x000002A0U, 100U) == 0U,
          "a missing plan is refused");
}

/*
 * ITEM-453: the Supported Protocol capability.  The owner's built-in
 * controllers declare two groups - a USB2 half and a USB3 half - and "which raw
 * port number is Linux's USB2 port 1" is the question the missing port evidence
 * could not answer.  The parser is pure, so this proof drives it with the exact
 * dwords a controller would answer, including the ones that must be refused.
 */
static void test_protocol_capability(void) {
    /* A USB3 group first, four-character name "USB ", two SuperSpeed PSIDs. */
    const uint32_t usb3_revision =
        (3U << XHCI_EXT_CAP_MAJOR_SHIFT) |
        (0U << XHCI_EXT_CAP_MINOR_SHIFT) |
        (4U << XHCI_EXT_CAP_NEXT_SHIFT) | XHCI_EXT_CAP_PROTOCOL;
    const uint32_t usb3_name = 0x20425355U;
    const uint32_t usb3_info = (2U << XHCI_EXT_PORT_PSIC_SHIFT) |
                               (2U << XHCI_EXT_PORT_COUNT_SHIFT) | 1U;
    const uint32_t usb3_psi[2] = {4U, 5U};
    /* The USB2 half that follows: ports 3..6, PSIV 1 (full) and 3 (high). */
    const uint32_t usb2_revision = (2U << XHCI_EXT_CAP_MAJOR_SHIFT) |
                                   XHCI_EXT_CAP_PROTOCOL;
    const uint32_t usb2_info = (2U << XHCI_EXT_PORT_PSIC_SHIFT) |
                               (4U << XHCI_EXT_PORT_COUNT_SHIFT) | 3U;
    const uint32_t usb2_psi[2] = {1U, 3U};
    struct xhci_protocol_group usb3;
    struct xhci_protocol_group usb2;
    struct xhci_protocol_group groups[2];
    const struct xhci_protocol_group *owner;
    uint32_t first = 0U;
    uint32_t last = 0U;
    char buffer[XHCI_PROTOCOL_RECORD_BYTES];
    size_t length;

    CHECK(xhci_protocol_capability(usb3_revision, usb3_name, usb3_info, usb3_psi,
                                   2U, &usb3),
          "a USB3 Supported Protocol capability parses");
    CHECK(usb3.major == 3U && usb3.minor == 0U, "its revision is published");
    CHECK(usb3.port_offset == 1U && usb3.port_count == 2U,
          "its raw port range is published");
    CHECK(usb3.psic == 2U && usb3.psiv_count == 2U,
          "the PSI count and the PSIDs it declares are published");
    CHECK(usb3.psiv[0] == 4U && usb3.psiv[1] == 5U,
          "PSIVs are the speed ids the ports report");
    CHECK(strcmp(usb3.name, "USB") == 0,
          "the four-character name is trimmed of its padding");
    CHECK(usb3.raw_revision == usb3_revision && usb3.raw_name == usb3_name &&
              usb3.raw_port_info == usb3_info,
          "the three raw dwords are kept beside the parsed fields");
    CHECK(xhci_protocol_port_range(&usb3, &first, &last) && first == 1U &&
              last == 2U,
          "the group owns ports offset through offset+count-1: the offset is "
          "the group's first raw port number, as QEMU's model declares it");
    CHECK(strcmp(xhci_protocol_group_label(usb3.major), "USB3") == 0,
          "a major revision of 3 is the USB3 group");
    CHECK(strcmp(xhci_protocol_group_label(2U), "USB2") == 0,
          "a major revision of 2 is the USB2 group");
    CHECK(xhci_protocol_group_label(9U) == NULL,
          "an unknown revision has no label to invent");

    CHECK(xhci_protocol_capability(usb2_revision, usb3_name, usb2_info, usb2_psi,
                                   2U, &usb2),
          "the USB2 half of the same controller parses");
    CHECK(xhci_protocol_port_range(&usb2, &first, &last) && first == 3U &&
              last == 6U,
          "and owns the raw ports after the USB3 half");

    groups[0] = usb3;
    groups[1] = usb2;
    owner = xhci_protocol_for_port(groups, 2U, 1U);
    CHECK(owner != NULL && owner->major == 3U,
          "raw port 1 belongs to the USB3 group");
    owner = xhci_protocol_for_port(groups, 2U, 5U);
    CHECK(owner != NULL && owner->major == 2U,
          "raw port 5 belongs to the USB2 group");
    CHECK(xhci_protocol_for_port(groups, 2U, 7U) == NULL,
          "a port no group declares belongs to none");
    CHECK(xhci_protocol_for_port(NULL, 2U, 1U) == NULL,
          "a missing list owns no port");
    CHECK(xhci_protocol_for_port(groups, 0U, 1U) == NULL,
          "an empty list owns no port");

    /* What must be refused: not a protocol capability at all, a group with no
       ports, a group whose offset is not a port number (the specification's
       valid values are 1 to MaxPorts), and PSI dwords the caller does not
       have. */
    CHECK(!xhci_protocol_capability(usb3_revision & ~XHCI_EXT_CAP_ID_MASK,
                                    usb3_name, usb3_info, usb3_psi, 2U, &usb3),
          "a capability that is not a Supported Protocol is refused");
    CHECK(!xhci_protocol_capability(usb3_revision, usb3_name,
                                    usb3_info & ~XHCI_EXT_PORT_COUNT_MASK,
                                    usb3_psi, 2U, &usb3),
          "a protocol group with zero ports is refused");
    CHECK(!xhci_protocol_capability(
              usb3_revision, usb3_name,
              usb3_info & ~XHCI_EXT_PORT_OFFSET_MASK, usb3_psi, 2U, &usb3),
          "a group whose offset is zero declares no first port and is refused");
    CHECK(xhci_protocol_capability(
              usb3_revision, usb3_name,
              (usb3_info & ~XHCI_EXT_PORT_OFFSET_MASK) | 1U, usb3_psi, 2U,
              &usb3) &&
              xhci_protocol_port_range(&usb3, &first, &last) && first == 1U &&
              last == 2U,
          "and the same capability with a real first port parses");
    CHECK(!xhci_protocol_capability(usb3_revision, usb3_name, usb3_info, NULL,
                                    2U, &usb3),
          "PSIDs the caller cannot supply are refused, not invented");
    CHECK(xhci_protocol_capability(usb3_revision, usb3_name,
                                   usb3_info & ~XHCI_EXT_PORT_PSIC_MASK, NULL,
                                   0U, &usb3) &&
              usb3.psiv_count == 0U && usb3.psic == 0U,
          "a group that declares no PSID parses with no speeds");
    CHECK(!xhci_protocol_capability(usb3_revision, usb3_name, usb3_info, usb3_psi,
                                    2U, NULL),
          "a missing output is refused");
    CHECK(!xhci_protocol_port_range(NULL, &first, &last),
          "a missing group has no range");
    CHECK(!xhci_protocol_port_range(&usb3, NULL, &last),
          "a missing output has no range");
    CHECK(!xhci_protocol_port_range(&usb2, &first, &last) ||
              (first == 3U && last == 6U),
          "a valid group still answers after a refused call");

    /* The record publishes the parsed fields *and* the raw dwords, so a wrong
       interpretation is visible on the next run instead of being trusted.  The
       group is parsed again first: the refusal checks above overwrote it on
       purpose, and a record pinned against a reused struct would pin the wrong
       fields. */
    CHECK(xhci_protocol_capability(usb3_revision, usb3_name, usb3_info, usb3_psi,
                                   2U, &usb3),
          "the USB3 capability parses again for the record");
    length = xhci_format_protocol_record(buffer, sizeof(buffer), 0x05U, 0x00U,
                                         0x3U, 0U, 2U, &usb3, usb3_revision,
                                         usb3_name, usb3_info);
    CHECK(length == strlen(buffer), "the protocol record reports its length");
    CHECK(strcmp(buffer,
                 "[XHCI-PROTO] pci=05:00.3 index=0 groups=2"
                 " major=3 minor=0 name=USB off=1 count=2 ports=1-2 psiv=4,5"
                 " d0=0x03000402 d1=0x20425355 d2=0x20000201") == 0,
          "the protocol record names the group, its range, its PSIDs and the "
          "raw capability dwords");
    length = xhci_format_protocol_record(buffer, sizeof(buffer), 0xFFU, 0xFFU,
                                         0xFU, 3U, 4U, &usb3, 0xFFFFFFFFU,
                                         0xFFFFFFFFU, 0xFFFFFFFFU);
    CHECK(length > 0U && length <= XHCI_PROTOCOL_RECORD_BYTES &&
              length <= 320U,
          "the worst-case protocol record fits its budget");
    CHECK(xhci_format_protocol_record(buffer, sizeof(buffer), 0xFFU, 0xFFU, 0xFU,
                                      3U, 4U, NULL, 0U, 0U, 0U) == 0U,
          "a missing group is refused");
    CHECK(xhci_format_protocol_record(NULL, sizeof(buffer), 0xFFU, 0xFFU, 0xFU,
                                      3U, 4U, &usb3, 0U, 0U, 0U) == 0U,
          "a missing buffer is refused");
}


/*
 * ITEM-407: the record budget. The `[XHCI]` line is mirrored to the console and
 * the console paints exactly one bounded line of it (CONSOLE_RECORD_MAX, 320
 * bytes); a longer record is cut and closed with "...", which would hide the
 * port evidence this item exists to publish.
 *
 * ITEM-452: the shapes changed.  The controller record now always carries
 * `pci=`, `controllers=` and `taken=` (35 bytes), and the device identity that
 * used to share the line has its own `[USB ]` record.  The worst `[XHCI]` shape
 * is therefore the port-evidence one: no role bound on that controller, a
 * device stage that failed, and the longest detail a device stage *can*
 * compose there - "P255_DEVICE_DESCRIPTOR_PREFIX_MAX_EXIT_LATENCY_TOO_LARGE"
 * (56 bytes: the longest stage name next to the longest completion-code name of
 * xHCI 1.1 table 6-90).  The port-evidence shape cannot carry a 79-byte detail,
 * because the composers that produce one only run after a device answered with
 * descriptors - and that device is published on its own `[USB ]` line instead.
 * Nor can it carry `mouse=NONE`: the pointer finding and the evidence are
 * exclusive in the record, because the two together do not fit.
 *
 * The 16-bit identifiers are pinned at their real written width ("0x" and four
 * digits - `vendor=0xFFFF`, `device=0xFFFF`, `vid=0xFFFF`, `pid=0xFFFF`): the
 * shared 64-bit writer would pad each to eighteen bytes, and an earlier
 * revision of this proof pinned the padded numbers against a record that wrote
 * the short ones - a pin that cannot fail is not a budget.
 *
 * The `[USB ]` shapes are pinned with it: a bound device and a refused device
 * carrying its identity, reason and detail.
 */
static void test_record_budget(void) {
    /* kernel/src/xhci.c: XHCI_RECORD_WORST_CASE_BYTES and the arithmetic in
       its comment. Every shape must fit it. */
    const size_t worst_case = 314U;
    /* The longest detail the port-evidence shape can produce: the device
       descriptor prefix read failed with the longest completion-code name. */
    static const char evidence_detail[] =
        "P255_DEVICE_DESCRIPTOR_PREFIX_MAX_EXIT_LATENCY_TOO_LARGE";
    /* A detail at the full width of its own buffer (XHCI_DETAIL_BYTES, 80
       including the terminator): "CTRL_<bus>:<dev>.<fn>_" + "P255_" + the
       longest stage + "_" + the longest outcome, which the composer truncates
       to the 79 bytes the buffer holds - so any detail, however long its
       pieces, is bounded by this string. */
    static const char longest_detail[] =
        "CTRL_FF:FF.F_P255_DEVICE_DESCRIPTOR_PREFIX_"
        "XHCI_COMPLETION_STOPPED_LENGTTTTTTTT";
    static const char controller_facts[] =
        " bar=0xFFFFFFFFFFFFFFFF ports=255 slots=255 slot_en=64 ctx=64"
        " vendor=0xFFFF device=0xFFFF";
    static const char controller_new[] =
        " pci=FF:FF.F controllers=8 taken=NO";
    static const char evidence[] =
        " usbcmd=FFFFFFFF usbsts=FFFFFFFF"
        " portsc=0xFFFFFFFF,0xFFFFFFFF pp=3 ppoff=255";
    static const char no_keyboard[] =
        "[XHCI] state=UNAVAILABLE reason=ENUMERATION_FAILED detail="
        "P255_DEVICE_DESCRIPTOR_PREFIX_MAX_EXIT_LATENCY_TOO_LARGE"
        " pci=FF:FF.F controllers=8 taken=NO"
        " bar=0xFFFFFFFFFFFFFFFF ports=255 slots=255 slot_en=64 ctx=64"
        " vendor=0xFFFF device=0xFFFF"
        " usbcmd=FFFFFFFF usbsts=FFFFFFFF"
        " portsc=0xFFFFFFFF,0xFFFFFFFF pp=3 ppoff=255";
    static const char keyboard[] =
        "[XHCI] state=READY reason=OK detail=OK"
        " pci=05:00.3 controllers=8 taken=YES"
        " bar=0xFFFFFFFFFFFFFFFF ports=255 slots=255 slot_en=64 ctx=64"
        " vendor=0xFFFF device=0xFFFF"
        " hid=REPORT_KEYBOARD hid=REPORT_MOUSE"
        " mouse=NONE reason=BAD_DESCRIPTOR_LENGTH";
    /* ITEM-438/452: a device that answered with descriptors and was refused
       still names itself on its own line, with its identity, the role that
       refused it, the reason and the stage. */
    static const char refused_device[] =
        "[USB ] pci=FF:FF.F port=255 speed=SUPER_PLUS role=KEYBOARD"
        " state=REFUSED reason=ENUMERATION_FAILED detail="
        "P255_DEVICE_DESCRIPTOR_PREFIX_MAX_EXIT_LATENCY_TOO_LARGE"
        " vid=0xFFFF pid=0xFFFF ifaces="
        "FF:FF/FF/FF,FF:FF/FF/FF,FF:FF/FF/FF,FF:FF/FF/FF+";
    static const char bound_device[] =
        "[USB ] pci=05:00.3 port=255 speed=SUPER_PLUS role=MOUSE state=BOUND"
        " reason=OK vid=0xFFFF pid=0xFFFF ifaces="
        "FF:FF/FF/FF,FF:FF/FF/FF,FF:FF/FF/FF,FF:FF/FF/FF+";
    static const char skipped_device[] =
        "[USB ] pci=FF:FF.F port=255 speed=SUPER_PLUS role=NONE"
        " state=REFUSED reason=ROLE_TAKEN";

    CHECK(strlen(evidence_detail) == 56U,
          "the evidence shape's longest detail is the stage plus the longest "
          "completion code");
    CHECK(strlen(longest_detail) == 79U,
          "a detail fills its whole 79-byte slot");
    CHECK(strlen(controller_facts) == 89U,
          "the controller facts are the width the budget assumes");
    CHECK(strlen(controller_new) == 35U,
          "pci/controllers/taken cost the 35 bytes the arithmetic reserves");
    CHECK(strlen(evidence) == 76U,
          "the port evidence costs the 76 bytes the budget reserves");
    CHECK(strlen(no_keyboard) == worst_case,
          "the no-keyboard record is exactly the pinned worst case");
    CHECK(strlen(no_keyboard) <= 320U,
          "the port evidence fits the console's one-line budget");
    CHECK(strlen(keyboard) <= 320U,
          "the keyboard record fits the console's one-line budget");
    CHECK(strlen(keyboard) <= worst_case,
          "the keyboard shape is inside the same worst case");
    CHECK(strlen(refused_device) <= 320U,
          "the refused device's own line fits the console's one-line budget");
    CHECK(strlen(bound_device) <= 320U,
          "the bound device's own line fits the console's one-line budget");
    CHECK(strlen(skipped_device) <= 320U,
          "the role-taken line fits the console's one-line budget");
    CHECK(strlen(no_keyboard) >= strlen(refused_device),
          "the controller's port evidence is the widest line of the two");
    /* The pointer finding and the port evidence are exclusive: the two
       together do not fit one painted line, so a controller that bound no role
       publishes the evidence and leaves the pointer's verdict to the
       machine-wide [MOUSE] record. */
    CHECK(strlen(no_keyboard) +
                  strlen(" mouse=NONE reason=BAD_DESCRIPTOR_LENGTH") >
              320U,
          "the pointer finding and the port evidence cannot share one line");
    CHECK(USB_HID_INTERFACE_SUMMARY_MAX * 11U +
                  (USB_HID_INTERFACE_SUMMARY_MAX - 1U) + 1U <=
              USB_HID_INTERFACE_SUMMARY_BYTES - 1U,
          "the interface list fits the buffer the record reserves for it");
    /* The evidence block's own budget: " usbcmd=" and eight digits, the same
       for usbsts, " portsc=" and two 32-bit values, " pp=" and the one-digit
       mask of two published ports, " ppoff=" and a three-digit count.
       kernel/src/xhci.c asserts the same arithmetic over compile-time
       constants; this is it over the pure formatter's worst-case buffer. */
    CHECK(8U + 8U + 8U + 8U + 8U + (2U * 10U + 1U) + 4U + 1U + 7U + 3U <=
              76U,
          "the evidence block fits the budget reserved for it");
    /* A port-evidence record whose detail were the 79-byte buffer bound would
       not fit; the composer that produces such a detail only runs after a
       device answered, and that record carries the device's own [USB ] line
       instead of the port evidence. */
    CHECK(strlen(no_keyboard) - strlen(evidence_detail) + strlen(longest_detail) >
              320U,
          "the 79-byte detail and the port evidence cannot share one line");
}

/*
 * ITEM-407: the port-claiming write. The scan reads CCS to decide whether a
 * port has a device, and it used to write PORTSC.PP only for a port it had
 * already decided to reset - which it decides from CCS. On a controller whose
 * ports come up unpowered that loop can never be left, so every port reports no
 * device on a machine that has a keyboard plugged in. The write that breaks the
 * loop is pinned here: it always asks for power, and it still never carries PED
 * or a write-one-to-clear change bit.
 */
static void test_port_power_claim(void) {
    /* The state the acceptance host's second controller would show if its
       ports came up unpowered: no connect, no power, valid link state. */
    const uint32_t unpowered = (5U << XHCI_PORTSC_PLS_SHIFT);
    const uint32_t claim = xhci_portsc_claim(unpowered);

    CHECK((claim & XHCI_PORTSC_PP) != 0U,
          "the claim always asks for port power, even with CCS clear");
    CHECK((claim & XHCI_PORTSC_PLS_MASK) == (unpowered & XHCI_PORTSC_PLS_MASK),
          "the link state the port reported is preserved");
    CHECK((claim & XHCI_PORTSC_PED) == 0U,
          "the claim never writes PED back");
    CHECK((claim & XHCI_PORTSC_RW1C) == 0U,
          "the claim never clears a change bit it did not read");
    CHECK(claim == (unpowered | XHCI_PORTSC_PP),
          "the claim adds power and nothing else to a port with no writable "
          "status of its own");

    /* A port that is already powered and enabled keeps every writable bit it
       had, and is not disabled by the claim. */
    {
        const uint32_t powered = XHCI_PORTSC_CCS | XHCI_PORTSC_PED |
                                 XHCI_PORTSC_PP |
                                 (XHCI_SPEED_LOW << XHCI_PORTSC_SPEED_SHIFT) |
                                 XHCI_PORTSC_PRC;
        const uint32_t again = xhci_portsc_claim(powered);

        CHECK((again & XHCI_PORTSC_PP) != 0U, "it stays powered");
        CHECK((again & XHCI_PORTSC_PED) == 0U, "and is not disabled");
        CHECK((again & XHCI_PORTSC_PRC) == 0U,
              "the reset change bit is not retired by the scan");
        CHECK((again & XHCI_PORTSC_PLS_MASK) == 0U,
              "a port with no link state asked for one is not given one");
    }
    CHECK(xhci_portsc_claim(0U) == XHCI_PORTSC_PP,
          "a blank PORTSC becomes exactly a power request");
}

static void test_input_context_layout(void) {
    uint8_t context[XHCI_INPUT_CONTEXT_MAX_BYTES];
    uint32_t value = 0U;

    memset(context, 0xFF, sizeof(context));
    xhci_context_clear(context, sizeof(context));
    {
        bool all_zero = true;

        for (size_t index = 0U; index < sizeof(context); ++index) {
            if (context[index] != 0U) {
                all_zero = false;
            }
        }
        CHECK(all_zero, "a cleared context is all zero");
    }

    xhci_input_control_context(context, XHCI_CONTEXT_32, 0x3U, 0U);
    read32(context, 0U, &value);
    CHECK(value == 0U,
          "Input Control Context dword 0 is the Drop flags");
    read32(context, 4U, &value);
    CHECK(value == 0x3U,
          "Input Control Context dword 1 is the Add flags");

    /* The Add flag bit is the device context index; the entry index is one
       further along, because entry 0 is the Input Control Context. */
    CHECK(xhci_context_flag(0U) == 0x1U, "the slot context is Add flag bit 0");
    CHECK(xhci_context_flag(1U) == 0x2U, "Endpoint 0 is Add flag bit 1");
    CHECK(xhci_context_flag(3U) == 0x8U,
          "the endpoint with DCI 3 is Add flag bit 3");
    CHECK(xhci_context_entry_index(0U) == 1U,
          "the slot context is entry 1, after the Input Control Context");
    CHECK(xhci_context_entry_index(1U) == 2U,
          "Endpoint 0 is entry 2, between the slot and the data endpoints");
    CHECK(xhci_context_entry_index(3U) == 4U,
          "the endpoint with DCI 3 is entry 4");
    CHECK(xhci_context_offset(XHCI_CONTEXT_32, 1U) == 32U,
          "a 32-byte context at entry 1 starts at offset 32");
    CHECK(xhci_context_offset(XHCI_CONTEXT_64, 4U) == 256U,
          "a 64-byte context at entry 4 starts at offset 256");

    CHECK(xhci_endpoint_dci(0U, true) == 1U, "Endpoint 0 has DCI 1");
    CHECK(xhci_endpoint_dci(1U, false) == 2U, "Endpoint 1 OUT has DCI 2");
    CHECK(xhci_endpoint_dci(1U, true) == 3U, "Endpoint 1 IN has DCI 3");
}

static void test_device_context_layout(void) {
    uint8_t entry[XHCI_CONTEXT_64];
    uint32_t value = 0U;

    xhci_context_clear(entry, sizeof(entry));
    xhci_slot_context(entry, XHCI_SPEED_HIGH, 3U, 5U);
    read32(entry, 0U, &value);
    CHECK(((value >> 20) & 0xFU) == XHCI_SPEED_HIGH,
          "the slot context carries the port speed in bits 23:20");
    CHECK(((value >> 27) & 0x1FU) == 3U,
          "the slot context carries its context entries in bits 31:27");
    read32(entry, 4U, &value);
    CHECK(((value >> 16) & 0xFFU) == 5U,
          "the slot context carries the root hub port in bits 23:16");

    xhci_context_clear(entry, sizeof(entry));
    xhci_endpoint_context(entry, XHCI_EP_TYPE_INTERRUPT_IN, 8U, 4U,
                          0x513000U, 8U);
    read32(entry, 0U, &value);
    CHECK(((value >> 16) & 0xFFU) == 4U,
          "the endpoint context carries its interval in bits 23:16");
    read32(entry, 4U, &value);
    CHECK(((value >> 1) & 0x3U) == 3U,
          "the endpoint context retries three times");
    CHECK(((value >> 3) & 0x7U) == XHCI_EP_TYPE_INTERRUPT_IN,
          "the endpoint context carries its type in bits 5:3");
    CHECK((value >> 16) == 8U,
          "the endpoint context carries its max packet size");
    read32(entry, 8U, &value);
    CHECK((value & 0xFFFFFFF0U) == 0x513000U,
          "the endpoint context carries the ring dequeue pointer");
    CHECK((value & 0x1U) == 1U,
          "the endpoint context marks the dequeue cycle state valid");
    read32(entry, 16U, &value);
    CHECK(value == 8U, "the endpoint context carries the average TRB length");
}

/*
 * ITEM-438: the silence decision behind the `finding=NO_REPORT` record.
 *
 * The 2026-09-17 hardware run's keyboard was bound, its transfer was in
 * flight, and not one report arrived - which the record printed exactly like a
 * keyboard nobody had touched. The decision is now a pure one, and this pins
 * every side of it: a bound keyboard that has not answered past the window is
 * due; one that has answered once is never due again, however quiet it is
 * afterwards; one inside the window is not due yet; and a machine with no
 * keyboard is a different record entirely, never a silence.
 */
static void test_no_report_window(void) {
    const uint64_t window = USB_HID_NO_REPORT_WINDOW_SECONDS;

    CHECK(window == 30U, "the silence window is the console's own 30 s idle one");
    CHECK(usb_hid_no_report_due(true, 0U, 0U, window) == false,
          "a keyboard bound for no time at all is not silent yet");
    CHECK(usb_hid_no_report_due(true, 0U, window - 1U, window) == false,
          "one second short of the window is still just quiet");
    CHECK(usb_hid_no_report_due(true, 0U, window, window) == true,
          "the window reached with no report is a finding");
    CHECK(usb_hid_no_report_due(true, 0U, window * 10U, window) == true,
          "and it stays one however long the machine runs");
    CHECK(usb_hid_no_report_due(true, 1U, window, window) == false,
          "a keyboard that answered once is never reported as never answering");
    CHECK(usb_hid_no_report_due(true, 1U, window * 10U, window) == false,
          "not even long afterwards");
    CHECK(usb_hid_no_report_due(false, 0U, window * 10U, window) == false,
          "a machine with no keyboard publishes UNAVAILABLE, not NO_REPORT");
    CHECK(usb_hid_no_report_due(false, 0U, 0U, window) == false,
          "and not before the window either");
    CHECK(usb_hid_no_report_due(true, 0U, 5U, 5U) == true,
          "the window is the caller's number, not a constant inside");
}

/*
 * ITEM-458: the two decisions behind "the keyboard loses no character".
 *
 * The driver keeps a bounded queue of decoded reports between the service pass
 * and the console, and the 2026-09-19 hardware run lost characters while the
 * screen scrolled.  A full queue must refuse the newest report rather than
 * overwrite an unread one - and the refusal is what the caller counts and
 * publishes - and the wait between two delivered reports is what the pass
 * measures as its own gap.  Both decisions live in the pure half of the driver
 * so this proof drives the same arithmetic the kernel runs.
 */
static void test_report_queue_and_service_gap(void) {
    uint32_t next = 0U;

    /* A queue with a free slot accepts and names the next head. */
    CHECK(xhci_local_queue_push(0U, 0U, 32U, &next) == true,
          "an empty queue accepts a report");
    CHECK(next == 1U, "and advances the head by one");
    CHECK(xhci_local_queue_push(30U, 0U, 32U, &next) == true,
          "the slot before the tail is still free");
    CHECK(next == 31U, "so the head moves onto it");
    /* head 31 -> next 0 == tail 0: the whole ring is now outstanding.  The
       sentinel proves a refused push leaves the caller's head untouched. */
    next = 12345U;
    CHECK(xhci_local_queue_push(31U, 0U, 32U, &next) == false,
          "a queue whose next head would reach the tail is full");
    CHECK(xhci_local_queue_push(5U, 6U, 32U, &next) == false,
          "a full queue refuses wherever the head is");
    CHECK(xhci_local_queue_push(0U, 1U, 32U, &next) == false,
          "and refuses the report that would fill its last free slot");
    CHECK(xhci_local_queue_push(0U, 0U, 0U, &next) == false,
          "a queue of no slots refuses everything");
    CHECK(next == 12345U, "a refused push writes no next head");

    /* The gap: no earlier report leaves the maximum alone. */
    CHECK(xhci_service_gap_ms(0U, 0U, 100U, 100U) == 0U,
          "the first report of a boot has no gap to measure");
    CHECK(xhci_service_gap_ms(40U, 0U, 100U, 100U) == 40U,
          "an unmeasurable gap does not clear a maximum either");
    /* One tick of a 100 Hz clock is ten milliseconds. */
    CHECK(xhci_service_gap_ms(0U, 100U, 101U, 100U) == 10U,
          "one tick is ten milliseconds");
    CHECK(xhci_service_gap_ms(0U, 100U, 130U, 100U) == 300U,
          "thirty ticks are three hundred milliseconds");
    CHECK(xhci_service_gap_ms(300U, 100U, 120U, 100U) == 300U,
          "a shorter gap does not lower the maximum");
    CHECK(xhci_service_gap_ms(300U, 100U, 160U, 100U) == 600U,
          "a longer gap raises it");
    CHECK(xhci_service_gap_ms(300U, 160U, 100U, 100U) == 300U,
          "a clock that went backwards leaves the maximum alone");
    CHECK(xhci_service_gap_ms(300U, 100U, 160U, 0U) == 300U,
          "an unknown tick rate leaves the maximum alone");
}

/*
 * ITEM-467: a device that never reported.
 *
 * The hardware run bound the Logitech receiver's mouse and then showed
 * `reports=0 transfers=1`: the driver had armed the interrupt transfer once and
 * the controller never completed it, so it was never re-armed.  The two halves
 * proven here are the ones a host can prove: the Normal TRB an interrupt IN
 * transfer is armed with asks the controller to complete on a short packet
 * (ISP), which is exactly the three-byte boot mouse answer the receiver sends
 * against the driver's four-byte request; and the one-word reason a record
 * publishes for a silent device distinguishes "the transfer is still pending"
 * from "the transfer completed and carried nothing".
 */
static void test_interrupt_short_packet_and_reason(void) {
    const struct xhci_trb transfer = xhci_trb_interrupt_in(1U, 0x3000U, 4U);

    CHECK(xhci_trb_type(&transfer) == XHCI_TRB_NORMAL,
          "an interrupt IN transfer is a Normal TRB");
    CHECK(transfer.dword[0] == 0x3000U && transfer.dword[1] == 0U,
          "an interrupt IN transfer carries the report buffer address");
    CHECK(transfer.dword[2] == 4U,
          "an interrupt IN transfer asks for the report length");
    CHECK(xhci_trb_cycle(&transfer),
          "an interrupt IN transfer carries its producer cycle bit");
    CHECK((transfer.dword[3] & XHCI_TRB_IOC) != 0U,
          "an interrupt IN transfer completes with an event");
    CHECK((transfer.dword[3] & XHCI_TRB_ISP) != 0U,
          "an interrupt IN transfer completes on a short packet too");
    CHECK((xhci_trb_normal(1U, 0x3000U, 4U, true).dword[3] & XHCI_TRB_ISP) == 0U,
          "a plain Normal TRB does not ask for the short-packet event");

    CHECK(strcmp(xhci_no_report_reason(false, 3U, 9U), "OK") == 0,
          "a device that reported is OK");
    CHECK(strcmp(xhci_no_report_reason(true, 0U, 1U), "INTERRUPT_PENDING") == 0,
          "an unfinished transfer names itself as pending");
    CHECK(strcmp(xhci_no_report_reason(false, 0U, 2U),
                 "COMPLETION_WITHOUT_REPORT") == 0,
          "a completion that carried no report names that");
    CHECK(strcmp(xhci_no_report_reason(false, 0U, 0U), "NOT_ARMED") == 0,
          "a device that was never armed names that");
    CHECK(strcmp(xhci_no_report_reason(true, 5U, 5U), "OK") == 0,
          "a report outranks a pending flag");
}

static void test_watchdog_decision(void) {
    const uint32_t threshold = 1000U;

    /* The wait since progress: one tick of a 100 Hz clock is ten ms. */
    CHECK(xhci_watchdog_silence_ms(0U, 500U, 100U) == 0U,
          "a role with no progress yet has no measurable silence");
    CHECK(xhci_watchdog_silence_ms(500U, 500U, 100U) == 0U,
          "no elapsed ticks is no silence");
    CHECK(xhci_watchdog_silence_ms(100U, 101U, 100U) == 10U,
          "one tick of the hundred-hertz clock is ten milliseconds");
    CHECK(xhci_watchdog_silence_ms(100U, 200U, 100U) == 1000U,
          "a hundred ticks are one thousand milliseconds");
    CHECK(xhci_watchdog_silence_ms(100U, 99U, 100U) == 0U,
          "a clock that went backwards is not a silence");
    CHECK(xhci_watchdog_silence_ms(100U, 200U, 0U) == 0U,
          "an unknown tick rate is not a silence");

    /* The decision: ready, armed and past the bound - nothing else. */
    CHECK(xhci_watchdog_due(true, 16U, threshold + 1U, threshold) == true,
          "a ready role with armed transfers past the bound is due");
    CHECK(xhci_watchdog_due(true, 16U, threshold, threshold) == false,
          "a role exactly at the bound is not due yet");
    CHECK(xhci_watchdog_due(true, 16U, threshold - 1U, threshold) == false,
          "a role inside the bound is not due yet");
    CHECK(xhci_watchdog_due(true, 0U, threshold * 10U, threshold) == false,
          "a role with nothing armed is not due");
    CHECK(xhci_watchdog_due(false, 16U, threshold * 10U, threshold) == false,
          "a role that is not ready is not due");
}

/*
 * ITEM-482 (round 20): what the silence means and what the repair rule does.
 * The round-19 hardware run published seven one-second mouse silences with
 * clean completion codes and no way to tell an idle pointer from a frozen one;
 * these are the pure halves of the classification and the escalation.
 */
static void test_watchdog_meaning_and_escalation(void) {
    const uint64_t two_seconds = 200U;

    /* Active: a report inside the window before progress. */
    CHECK(xhci_watchdog_active_before(100U, 150U, two_seconds) == true,
          "a report inside the window means the device was in use");
    CHECK(xhci_watchdog_active_before(100U, 100U + two_seconds,
                                      two_seconds) == true,
          "a report exactly at the window edge still counts as active");
    CHECK(xhci_watchdog_active_before(100U, 100U + two_seconds + 1U,
                                      two_seconds) == false,
          "a report older than the window means the device was idle");
    CHECK(xhci_watchdog_active_before(0U, 500U, two_seconds) == false,
          "a device that never reported cannot be called active");
    CHECK(xhci_watchdog_active_before(100U, 500U, 0U) == false,
          "a zero window cannot call a device active");
    CHECK(xhci_watchdog_active_before(500U, 100U, two_seconds) == false,
          "a report after progress is not a preceding one");

    /* Rate: reports scaled to one second, zero when unmeasurable. */
    CHECK(xhci_report_rate_per_second(0U, 1000U) == 0U,
          "no reports is a rate of zero");
    CHECK(xhci_report_rate_per_second(10U, 1000U) == 10U,
          "ten reports in a second are ten a second");
    CHECK(xhci_report_rate_per_second(5U, 500U) == 10U,
          "five reports in half a second scale to ten a second");
    CHECK(xhci_report_rate_per_second(100U, 0U) == 0U,
          "an unmeasurable window is not a rate");

    /* Escalate only on a repeat while the device was in use. */
    CHECK(xhci_watchdog_escalate(true, 1U) == false,
          "the first active silence still gets a plain re-arm");
    CHECK(xhci_watchdog_escalate(true, 2U) == true,
          "the second active silence escalates the recovery");
    CHECK(xhci_watchdog_escalate(false, 9U) == false,
          "an idle device never escalates - nobody was moving it");
    CHECK(xhci_watchdog_escalate(true, 0U) == false,
          "a silence with no active count yet is not a repeat");
}

/*
 * MOUSE-DIAG (owner task 2026-09-27, second step): the slot state beside the
 * endpoint state, and the EP0 probe's two records.
 *
 * `hw13` narrowed the pointer's stall to the transfer leg; the probe exists to
 * say whether the device still answers at all.  The proof pins the record set,
 * the two sentinels that tell "not even submitted" from "never answered", the
 * before/after state half, and every record's worst case inside the console's
 * 320-byte line budget.
 */
static void test_slot_state(void) {
    uint8_t context[64U * 4U];
    uint32_t dword0;

    memset(context, 0, sizeof(context));
    dword0 = 3U << 27U; /* Configured */
    memcpy(&context[0], &dword0, sizeof(dword0));
    CHECK(xhci_slot_state(context, 64U) == 3U,
          "the slot state is read from the slot context's own dword");
    CHECK(strcmp(xhci_slot_state_string(3U), "Configured") == 0,
          "a configured slot is named");
    CHECK(strcmp(xhci_slot_state_string(0U), "Disabled") == 0,
          "a disabled slot is named");
    CHECK(xhci_slot_state(context, 0U) == 0U,
          "a missing context answers Disabled, not a read through");
    CHECK(xhci_slot_state(NULL, 64U) == 0U, "a null context answers Disabled");
}

static void test_ep0_probe_records(void) {
    struct xhci_ep0_probe probe = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U,
                                   0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
    char buffer[XHCI_EP0_PROBE_RECORD_BYTES];
    char small[24];
    size_t length;

    probe.ticks = 6776U;
    probe.slot = 1U;
    probe.dci = 5U;
    probe.request = 0x00U;
    probe.submitted = 1U;
    probe.completion_code = 1U;
    probe.elapsed_us = 812U;
    probe.endpoint_state_before = 1U;
    probe.endpoint_state_after = 1U;
    probe.slot_state_before = 3U;
    probe.slot_state_after = 3U;
    probe.portsc_before = 0x00000603U;
    probe.portsc_after = 0x00000603U;
    probe.usbsts_before = 0x10U;
    probe.usbsts_after = 0x10U;
    probe.erdp_before = 0x5754C0U;
    probe.erdp_after = 0x5754C0U;
    memset(buffer, 0x7F, sizeof(buffer));
    length = xhci_format_ep0_probe_record(buffer, sizeof(buffer), &probe);
    CHECK(length == strlen(buffer), "the probe record reports its own length");
    CHECK(strcmp(buffer,
                 "[MOUSE-EP0] ticks=6776 slot=1 dci=5 request=0 submitted=1"
                 " code=1 elapsed_us=812 ep_before=1 ep_after=1"
                 " slot_before=3 slot_after=3") == 0,
          "the probe record carries the request, its outcome and the two "
          "context states");
    length = xhci_format_ep0_state_record(buffer, sizeof(buffer), &probe);
    CHECK(strcmp(buffer,
                 "[MOUSE-EP0-STATE] portsc_before=0x00000603"
                 " portsc_after=0x00000603"
                 " usbsts_before=0x0000000000000010"
                 " usbsts_after=0x0000000000000010"
                 " erdp_before=0x00000000005754C0"
                 " erdp_after=0x00000000005754C0 erdp_moved=0") == 0,
          "the state record carries the port, the controller status and the "
          "dequeue pointer on both sides of the probe");

    /* The two sentinels: nothing was submitted, or the device never answered. */
    probe.submitted = 0U;
    probe.completion_code = XHCI_EP0_NOT_SUBMITTED;
    CHECK(xhci_format_ep0_probe_record(buffer, sizeof(buffer), &probe) > 0U &&
              strstr(buffer, " submitted=0 code=255") != NULL,
          "a transfer the driver could not place says so");
    probe.submitted = 1U;
    probe.completion_code = XHCI_EP0_NO_COMPLETION;
    length = xhci_format_ep0_probe_record(buffer, sizeof(buffer), &probe);
    CHECK(length > 0U && strstr(buffer, " submitted=1 code=254") != NULL,
          "a transfer that never completed inside the budget says so");
    probe.erdp_after = 0x575500U;
    length = xhci_format_ep0_state_record(buffer, sizeof(buffer), &probe);
    CHECK(length > 0U && strstr(buffer, " erdp_moved=1") != NULL,
          "and the state record says when the dequeue pointer moved");

    CHECK(xhci_format_ep0_probe_record(buffer, sizeof(buffer), NULL) == 0U,
          "a missing probe is refused");
    CHECK(xhci_format_ep0_state_record(buffer, sizeof(buffer), NULL) == 0U,
          "a missing probe is refused by the state record too");
    memset(small, 0x7F, sizeof(small));
    CHECK(xhci_format_ep0_probe_record(small, sizeof(small), &probe) == 0U,
          "a short buffer is refused rather than half-written");
    CHECK(small[0] == '\0', "and left a valid empty string");

    /* The journal's bound again: the widest values must still fit one line. */
    {
        struct xhci_ep0_probe wide = {~0ULL, ~0U, ~0U, ~0U, 1U, ~0U, ~0ULL,
                                      ~0U,  ~0U, ~0U, ~0U, ~0U, ~0U, ~0U,
                                      ~0U,  ~0ULL, ~0ULL};

        CHECK(xhci_format_ep0_probe_record(buffer, sizeof(buffer), &wide) > 0U &&
                  strlen(buffer) <= 320U,
              "the widest probe record fits the journal's line budget");
        CHECK(xhci_format_ep0_state_record(buffer, sizeof(buffer), &wide) > 0U &&
                  strlen(buffer) <= 320U,
              "and so does the widest state record");
    }
}

/*
 * MOUSE-DIAG (owner task 2026-09-27): the stall snapshot's field set.
 *
 * The two hardware runs that motivated it ended with a silent pointer and a
 * journal that could not say whether the device or the controller stopped.  The
 * records below are the ones that separate the cases, so the proof pins every
 * field they must carry, the unidentified-port fallback the contract asks for,
 * and the "a short buffer is refused rather than half-written" rule the durable
 * journal depends on.
 */
static void test_silence_records(void) {
    struct xhci_silence_time time = {0U, 0U, 0U};
    struct xhci_silence_endpoint endpoint = {0U, 0U, 0U, 0U, 0U, 0U,
                                             0U, 0U, 0U, 0U};
    struct xhci_silence_controller controller = {0U, 0U, 0U, 0U, 0U, 0U, 0U,
                                                 0U, 0U, 0U, 0U, 0U, 0U};
    char buffer[XHCI_SILENCE_PORT_RECORD_BYTES];
    char small[24];
    size_t length;

    time.ticks = 16442U;
    time.since_completion_ms = 1010U;
    time.since_report_ms = 1010U;
    endpoint.slot = 1U;
    endpoint.dci = 5U;
    endpoint.endpoint_state = 1U;
    endpoint.armed = 16U;
    endpoint.pending = 1U;
    endpoint.reposted = 16189U;
    endpoint.completed = 16205U;
    endpoint.reports = 16189U;
    endpoint.last_completion_code = 1U;
    endpoint.last_completion_ticks = 16442U;
    memset(buffer, 0x7F, sizeof(buffer));
    length = xhci_format_silence_record(buffer, sizeof(buffer), &time, &endpoint);
    CHECK(length == strlen(buffer), "the silence record reports its own length");
    CHECK(strcmp(buffer,
                 "[MOUSE-SILENCE] ticks=16442 since_completion_ms=1010"
                 " since_report_ms=1010 slot=1 dci=5 ep_state=1 armed=16"
                 " pending=1 reposted=16189 completed=16205 reports=16189"
                 " last_code=1 last_completion_ticks=16442") == 0,
          "the silence record carries the time, the endpoint's counters and the "
          "last completion code");
    CHECK(xhci_format_silence_record(buffer, sizeof(buffer), NULL, &endpoint) == 0U,
          "a missing time is refused");
    CHECK(xhci_format_silence_record(buffer, sizeof(buffer), &time, NULL) == 0U,
          "a missing endpoint is refused");
    memset(small, 0x7F, sizeof(small));
    CHECK(xhci_format_silence_record(small, sizeof(small), &time, &endpoint) == 0U,
          "a short buffer is refused");
    CHECK(small[0] == '\0', "and left a valid empty string");

    controller.bus = 0x05U;
    controller.device = 0x00U;
    controller.function = 0x3U;
    controller.usbcmd = 0x1U;
    controller.usbsts = 0x10U;
    controller.crcr = 0x8U;
    controller.erdp = 0x5754F0U;
    controller.erstba = 0x576000U;
    controller.iman = 0x2U;
    controller.imod = 0x4CU;
    controller.error = 0U;
    controller.host_events = 0U;
    controller.ringfull = 0U;
    length = xhci_format_silence_controller_record(buffer, sizeof(buffer),
                                                    &controller);
    CHECK(length == strlen(buffer), "the controller record reports its own length");
    CHECK(strcmp(buffer,
                 "[MOUSE-SILENCE-XHCI] pci=05:00.3 usbcmd=0x0000000000000001"
                 " usbsts=0x0000000000000010 crcr=0x0000000000000008"
                 " erdp=0x00000000005754F0 erstba=0x0000000000576000"
                 " iman=0x0000000000000002 imod=0x000000000000004C running=1"
                 " halted=0 error=0x0000000000000000 host_events=0 ringfull=0") == 0,
          "the controller record carries the command, status, ring and "
          "interrupter registers with the decoded verdicts");
    CHECK(xhci_format_silence_controller_record(buffer, sizeof(buffer), NULL) == 0U,
          "a missing controller is refused");

    {
        const uint32_t ports[4] = {0x000002A0U, 0x00000000U, 0x000002A3U,
                                   0x00000000U};

        length = xhci_format_silence_port_record(buffer, sizeof(buffer), 0x05U,
                                                 0x00U, 0x3U, 3U, ports, 4U);
        CHECK(length == strlen(buffer), "the port record reports its own length");
        CHECK(strcmp(buffer,
                     "[MOUSE-SILENCE-PORT] pci=05:00.3 identified=1 port=3"
                     " ports=4 portsc=0x000002A3 ccs=1 ped=1 pr=0 pp=1 oca=0"
                     " csc=0 pec=0 wrc=0 prc=0 plc=0 cec=0 pls=5 wake=0") == 0,
              "an identified port publishes its decoded PORTSC bits");
        /* The contract's fallback: when the driver cannot name the port, every
           port value it read is published and `identified=0` says why. */
        length = xhci_format_silence_port_record(buffer, sizeof(buffer), 0x05U,
                                                 0x00U, 0x3U, 0U, ports, 4U);
        CHECK(strcmp(buffer,
                     "[MOUSE-SILENCE-PORT] pci=05:00.3 identified=0 port=0"
                     " ports=4 p1=0x000002A0 p2=0x00000000 p3=0x000002A3"
                     " p4=0x00000000") == 0,
              "an unnamed port publishes every port value it read instead");
        length = xhci_format_silence_port_record(buffer, sizeof(buffer), 0x05U,
                                                 0x00U, 0x3U, 9U, ports, 4U);
        CHECK(strcmp(buffer,
                     "[MOUSE-SILENCE-PORT] pci=05:00.3 identified=0 port=0"
                     " ports=4 p1=0x000002A0 p2=0x00000000 p3=0x000002A3"
                     " p4=0x00000000") == 0,
              "a port number past the ports actually read is not claimed");
        CHECK(xhci_format_silence_port_record(buffer, sizeof(buffer), 0x05U,
                                              0x00U, 0x3U, 3U, NULL, 4U) == 0U,
              "a missing port array is refused");
    }

    /* The controller read again after the recovery: the dequeue pointer either
       moved or it did not, and the record says which. */
    length = xhci_format_silence_after_record(buffer, sizeof(buffer), 16942U,
                                              1010U, 0x5754F0U, 0x5754F0U,
                                              0x1U, 0x10U, 0x000002A3U);
    CHECK(strcmp(buffer,
                 "[MOUSE-SILENCE-AFTER] ticks=16942 waited_ms=1010"
                 " erdp_before=0x00000000005754F0 erdp_after=0x00000000005754F0"
                 " erdp_moved=0 usbcmd=0x0000000000000001"
                 " usbsts=0x0000000000000010 portsc=0x000002A3") == 0,
          "the after record says whether the recovery moved the dequeue pointer");
    length = xhci_format_silence_after_record(buffer, sizeof(buffer), 16942U,
                                              1010U, 0x5754F0U, 0x575500U,
                                              0x1U, 0x10U, 0x000002A3U);
    CHECK(strstr(buffer, " erdp_moved=1") != NULL,
          "and reports movement when there was some");

    /* The iteration maximum is published with the phase that spent it and the
       clock it happened on, so `iter_max_us` is no longer a bare number. */
    length = xhci_format_iter_max_record(buffer, sizeof(buffer), 17500U, 14614U,
                                         175U, 3259516U, "net");
    CHECK(strcmp(buffer,
                 "[ITER-MAX] ticks=17500 second=175 loops=14614"
                 " iter_max_us=3259516 phase=net") == 0,
          "the iteration maximum names its phase and its time");
    CHECK(length <= 320U, "the iteration record fits the console's line budget");
    CHECK(xhci_format_iter_max_record(buffer, sizeof(buffer), 17500U, 14614U,
                                      175U, 3259516U, NULL) == 0U,
          "a missing phase name is refused");

    /*
     * The journal's own bound: a record longer than the console's line budget is
     * cut, and a cut diagnostic record is worse than none.  Every snapshot
     * record is checked at its widest (all-numeric fields at full width) so a
     * future field cannot silently push one past it.
     */
    {
        struct xhci_silence_time wide_time = {~0ULL, ~0U, ~0U};
        struct xhci_silence_endpoint wide_endpoint = {~0U, ~0U, ~0U, ~0U, ~0U,
                                                      ~0U, ~0U, ~0U, ~0U, ~0ULL};
        struct xhci_silence_controller wide_controller = {0U};
        const uint32_t wide_ports[8] = {~0U, ~0U, ~0U, ~0U,
                                        ~0U, ~0U, ~0U, ~0U};

        wide_controller.bus = 0xFFU;
        wide_controller.device = 0xFFU;
        wide_controller.function = 0xFU;
        wide_controller.usbcmd = ~0U;
        wide_controller.usbsts = ~0U;
        wide_controller.crcr = ~0ULL;
        wide_controller.erdp = ~0ULL;
        wide_controller.erstba = ~0ULL;
        wide_controller.iman = ~0U;
        wide_controller.imod = ~0U;
        wide_controller.error = ~0ULL;
        wide_controller.host_events = ~0U;
        wide_controller.ringfull = ~0U;
        CHECK(xhci_format_silence_record(buffer, sizeof(buffer), &wide_time,
                                         &wide_endpoint) > 0U &&
                  strlen(buffer) <= 320U,
              "the widest silence record fits the journal's line budget");
        CHECK(xhci_format_silence_controller_record(buffer, sizeof(buffer),
                                                    &wide_controller) > 0U &&
                  strlen(buffer) <= 320U,
              "the widest controller record fits it too");
        CHECK(xhci_format_silence_port_record(buffer, sizeof(buffer), 0xFFU,
                                              0xFFU, 0xFU, 0U, wide_ports,
                                              8U) > 0U &&
                  strlen(buffer) <= 320U,
              "and so does the all-ports fallback");
        CHECK(xhci_format_silence_after_record(buffer, sizeof(buffer), ~0ULL, ~0U,
                                               ~0ULL, ~0ULL, ~0U, ~0U, ~0U) > 0U &&
                  strlen(buffer) <= 320U,
              "the post-recovery record has room for its widest values");
        CHECK(xhci_format_iter_max_record(buffer, sizeof(buffer), ~0ULL, ~0ULL,
                                          ~0ULL, ~0ULL, "in_ptr") > 0U &&
                  strlen(buffer) <= 320U,
              "the iteration record has room for its widest values");
    }
}

/*
 * MOUSE-DIAG step 3: the three records that close the remaining unknowns, and
 * the endpoint-context reader they depend on.  Every field is pinned, the
 * `publish=derived` and `unavailable=` naming is pinned (so a later reader
 * cannot mistake a derived span for a measured one), and every record's worst
 * case is checked inside the console's 320-byte line budget.
 */
static void test_ep_state_and_timing_records(void) {
    uint8_t context[64U * 6U];
    uint64_t dequeue = 0U;
    uint32_t cycle = 0U;
    struct xhci_ep_state state = {0U, 0U, 0U, 0U, 0U, 0U, 0U,
                                  0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
    struct xhci_timing_check check = {0U, 0U, 0U, 0U, 0U, 0U, 0U,
                                      0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
    struct xhci_bootlog_flush flush = {0U, 0U, 0U, 0U, 0U, 0U, 0U};
    char buffer[320];
    char small[24];
    size_t length;

    memset(context, 0, sizeof(context));
    /* Keyboard DCI 3 and mouse DCI 5: entry n is DCI n in a device context. */
    {
        const uint64_t keyboard_dq = UINT64_C(0x0000000000571000) | 1U;
        const uint64_t mouse_dq = UINT64_C(0x0000000000572000) | 0U;
        uint32_t low = (uint32_t)(keyboard_dq & 0xFFFFFFFFU);
        uint32_t high = (uint32_t)(keyboard_dq >> 32U);

        memcpy(&context[64U * 3U + 8U], &low, sizeof(low));
        memcpy(&context[64U * 3U + 12U], &high, sizeof(high));
        low = (uint32_t)(mouse_dq & 0xFFFFFFFFU);
        high = (uint32_t)(mouse_dq >> 32U);
        memcpy(&context[64U * 5U + 8U], &low, sizeof(low));
        memcpy(&context[64U * 5U + 12U], &high, sizeof(high));
    }
    CHECK(xhci_endpoint_dequeue(context, 64U, 3U, &dequeue, &cycle),
          "the dequeue pointer is read from the endpoint's own context entry");
    CHECK(dequeue == UINT64_C(0x0000000000571000) && cycle == 1U,
          "the pointer keeps its address bits and the DCS bit is separated");
    CHECK(xhci_endpoint_dequeue(context, 64U, 5U, &dequeue, &cycle) &&
              dequeue == UINT64_C(0x0000000000572000) && cycle == 0U,
          "the mouse entry reads independently of the keyboard entry");
    CHECK(!xhci_endpoint_dequeue(NULL, 64U, 3U, &dequeue, &cycle) &&
              dequeue == 0U && cycle == 0U,
          "a missing context is refused with both outputs cleared");
    CHECK(!xhci_endpoint_dequeue(context, 0U, 3U, &dequeue, &cycle),
          "a zero context size is refused");

    state.role = 1U;
    state.endpoint_address = 0x82U;
    state.dci = 5U;
    state.endpoint_state = 1U;
    state.ring_base = UINT64_C(0x575000);
    state.dequeue_pointer = UINT64_C(0x575000);
    state.dequeue_cycle = 1U;
    state.enqueue = 16U;
    state.ring_trbs = 32U;
    state.producer_cycle = 1U;
    state.armed = 16U;
    state.pending = 1U;
    state.reposted = 13879U;
    state.last_completion_code = 1U;
    state.last_completion_ms = 25000U;
    memset(buffer, 0x7F, sizeof(buffer));
    length = xhci_format_ep_state_record(buffer, sizeof(buffer), &state);
    CHECK(length == strlen(buffer), "the endpoint record reports its own length");
    CHECK(strcmp(buffer,
                 "[EP-STATE] role=mouse ep=0x82 dci=5 state=1 armed=16 pending=1"
                 " reposted=13879 code=1 last_ms=25000") == 0,
          "the endpoint record carries the context state the stall question "
          "needs");
    length = xhci_format_ep_state_tail_record(buffer, sizeof(buffer), &state);
    CHECK(strcmp(buffer,
                 "[EP-STATE-TAIL] role=mouse ring=0x0000000000575000"
                 " dq=0x0000000000575000 dc=1 enq=16/32 pc=1") == 0,
          "the tail carries the ring and the TR Dequeue Pointer");
    state.role = 0U;
    state.endpoint_address = 0x81U;
    state.dci = 3U;
    length = xhci_format_ep_state_record(buffer, sizeof(buffer), &state);
    CHECK(strstr(buffer, "[EP-STATE] role=keyboard ep=0x81 dci=3") != NULL,
          "the keyboard role names itself");

    check.start_tsc = UINT64_C(0x1000);
    check.end_tsc = UINT64_C(0x2000);
    check.elapsed_cycles = UINT64_C(0x1000);
    check.ticks_start = 9400U;
    check.ticks_end = 9500U;
    check.real_seconds_start = 94U;
    check.real_seconds_end = 95U;
    check.tick_hz = 100U;
    check.raw_elapsed_us = 1000000U;
    check.sum_phase_us = 2829709U;
    check.work_us = 2831316U;
    check.turn_us = 38579U;
    check.worst_us = 2839573U;
    check.iter_max_us = 3469478U;
    check.mismatch = 1U;
    length = xhci_format_timing_check_record(buffer, sizeof(buffer), &check);
    CHECK(strcmp(buffer,
                 "[TIMING-CHECK] start_tsc=0x0000000000001000"
                 " end_tsc=0x0000000000002000 elapsed_cycles=4096"
                 " ticks_start=9400 ticks_end=9500 real_start=94 real_end=95"
                 " tick_hz=100 raw_elapsed_us=1000000") == 0,
          "the timing record carries the raw anchors");
    length = xhci_format_timing_check_tail_record(buffer, sizeof(buffer),
                                                  &check);
    CHECK(strcmp(buffer,
                 "[TIMING-CHECK-TAIL] sum_phase_us=2829709 work_us=2831316"
                 " turn_us=38579 worst_us=2839573 iter_max_us=3469478"
                 " timing_accounting_mismatch=1") == 0,
          "the tail carries the existing metrics and the mismatch flag");
    check.mismatch = 0U;
    length = xhci_format_timing_check_tail_record(buffer, sizeof(buffer),
                                                  &check);
    CHECK(strstr(buffer, " timing_accounting_mismatch=0") != NULL,
          "a consistent window says so");

    flush.records = 59U;
    flush.text_bytes = 8111U;
    flush.slot_bytes = 30208U;
    flush.publish_us = 1200U;
    flush.esp_write_us = 2800000U;
    flush.commit_us = 12000U;
    flush.total_us = 2813200U;
    length = xhci_format_bootlog_flush_record(buffer, sizeof(buffer), &flush);
    CHECK(strcmp(buffer,
                 "[BOOTLOG-FLUSH] records=59 text_bytes=8111 slot_bytes=30208"
                 " publish_us=1200 publish=derived esp_write_us=2800000"
                 " commit_us=12000 total_us=2813200"
                 " unavailable=esp_blocks,clusters") == 0,
          "the flush record names the derived point and the unavailable ones");

    {
        struct xhci_ep_state wide = {~0U, ~0U, ~0U, ~0U, ~0ULL, ~0ULL, ~0U,
                                     ~0U, ~0U, ~0U, ~0U, ~0U, ~0U, ~0U, ~0U};

        CHECK(xhci_format_ep_state_record(buffer, sizeof(buffer), &wide) > 0U &&
                  strlen(buffer) <= 320U,
              "the widest endpoint record fits the journal's line budget");
        CHECK(xhci_format_ep_state_tail_record(buffer, sizeof(buffer), &wide) > 0U &&
                  strlen(buffer) <= 320U,
              "and so does its ring tail");
    }
    {
        struct xhci_timing_check wide = {~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL,
                                         ~0ULL, ~0ULL, ~0ULL, ~0ULL, ~0ULL,
                                         ~0ULL, ~0ULL, ~0ULL, ~0ULL, 1U};

        CHECK(xhci_format_timing_check_record(buffer, sizeof(buffer), &wide) > 0U &&
                  strlen(buffer) <= 320U,
              "the widest timing record fits it too");
        CHECK(xhci_format_timing_check_tail_record(buffer, sizeof(buffer), &wide) > 0U &&
                  strlen(buffer) <= 320U,
              "and so does its accounting tail");
    }
    {
        struct xhci_bootlog_flush wide = {~0U, ~0ULL, ~0ULL, ~0ULL,
                                          ~0ULL, ~0ULL, ~0ULL};

        CHECK(xhci_format_bootlog_flush_record(buffer, sizeof(buffer), &wide) > 0U &&
                  strlen(buffer) <= 320U,
              "and the widest flush record as well");
    }
    CHECK(xhci_format_ep_state_record(buffer, sizeof(buffer), NULL) == 0U &&
              xhci_format_ep_state_tail_record(buffer, sizeof(buffer), NULL) == 0U &&
              xhci_format_timing_check_record(buffer, sizeof(buffer), NULL) == 0U &&
              xhci_format_timing_check_tail_record(buffer, sizeof(buffer), NULL) == 0U &&
              xhci_format_bootlog_flush_record(buffer, sizeof(buffer), NULL) == 0U,
          "a missing snapshot is refused");
    memset(small, 0x7F, sizeof(small));
    CHECK(xhci_format_ep_state_record(small, sizeof(small), &state) == 0U &&
              small[0] == '\0',
          "a short buffer is refused rather than half-written");
}


/*
 * ITEM-550: the command wait's record.
 *
 * The L875 run could only say `P1_ENABLE_SLOT_TIMEOUT`.  The wait now separates
 * the three outcomes a command can have and keeps what it saw, so these four
 * proofs drive exactly those outcomes with no controller present: a completion
 * that arrives, a wait that ends with no event at all, a wait that only ever
 * sees the completion of somebody else's command, and the record surviving the
 * release that zeroes the session it was captured from.
 */
static struct xhci_event command_completion(uint64_t address, uint8_t code,
                                            uint8_t slot) {
    struct xhci_event event;

    memset(&event, 0, sizeof(event));
    event.type = XHCI_TRB_COMMAND_COMPLETION;
    event.completion_code = code;
    event.slot_id = slot;
    event.parameter = address;
    event.parameter_low = (uint32_t)address;
    return event;
}

static void test_command_wait_success(void) {
    const uint64_t address = 0x1000U;
    const struct xhci_event event = command_completion(address, 1U, 2U);
    struct xhci_command_wait wait;
    struct xhci_command_snapshot snapshot;
    char record[XHCI_COMMAND_RECORD_BYTES];

    CHECK(xhci_command_event_kind_of(&event, address) ==
              XHCI_COMMAND_EVENT_OWN,
          "a completion naming the submitted address is the wait's own event");
    xhci_command_wait_init(&wait);
    xhci_command_wait_note(&wait, &event, XHCI_COMMAND_EVENT_OWN);
    CHECK(wait.events == 1U && wait.unmatched == 0U,
          "the wait counts one event and no unmatched completion");
    CHECK(xhci_command_result_of(true, true) == XHCI_COMMAND_RESULT_SUCCESS,
          "enqueued and completed is SUCCESS");
    CHECK(strcmp(xhci_command_result_string(XHCI_COMMAND_RESULT_SUCCESS),
                 "SUCCESS") == 0,
          "SUCCESS has its own name in the record");

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.valid = true;
    snapshot.result = xhci_command_result_of(true, true);
    snapshot.type = XHCI_TRB_ENABLE_SLOT;
    snapshot.ring_index = 3U;
    snapshot.ring_cycle = 1U;
    snapshot.physical = 0xBF000000ULL;
    snapshot.wait = wait;
    CHECK(xhci_format_command_record(record, sizeof(record), &snapshot) != 0U &&
              strstr(record, "result=SUCCESS") != NULL,
          "the command record names the outcome it captured");
    CHECK(strstr(record, "type=9") != NULL && strstr(record, "index=3") != NULL,
          "the command record carries the command type and the ring index");
    CHECK(strstr(record, "cycle=1") != NULL &&
              strstr(record, "phys=0x00000000BF000000") != NULL,
          "the command record carries the producer cycle and the TRB address");
}

static void test_command_wait_timeout_without_event(void) {
    struct xhci_command_wait wait;
    struct xhci_command_snapshot snapshot;
    char record[XHCI_COMMAND_RECORD_BYTES];

    CHECK(xhci_command_result_of(true, false) == XHCI_COMMAND_RESULT_TIMEOUT,
          "accepted but never completed is TIMEOUT");
    CHECK(xhci_command_result_of(false, false) ==
              XHCI_COMMAND_RESULT_ENQUEUE_FAILED,
          "a ring that refused the TRB is an enqueue failure, not a timeout");
    CHECK(strcmp(xhci_command_result_string(XHCI_COMMAND_RESULT_TIMEOUT),
                 "TIMEOUT") == 0 &&
              strcmp(xhci_command_result_string(
                         XHCI_COMMAND_RESULT_ENQUEUE_FAILED),
                     "ENQUEUE_FAILED") == 0,
          "the two failures have different names in the record");

    xhci_command_wait_init(&wait);
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.valid = true;
    snapshot.result = xhci_command_result_of(true, false);
    snapshot.wait = wait;
    CHECK(xhci_format_command_event_record(record, sizeof(record), &snapshot) !=
              0U &&
              strstr(record, "event=none") != NULL &&
              strstr(record, "events=0") != NULL &&
              strstr(record, "unmatched=0") != NULL &&
              strstr(record, "last=none") != NULL,
          "a wait that saw no event says so instead of showing a zero event");
}

static void test_command_wait_foreign_completion(void) {
    const uint64_t address = 0x2000U;
    const uint64_t foreign = 0x3000U;
    const struct xhci_event event = command_completion(foreign, 4U, 5U);
    struct xhci_command_wait wait;
    struct xhci_command_snapshot snapshot;
    char record[XHCI_COMMAND_RECORD_BYTES];

    CHECK(xhci_command_event_kind_of(&event, address) ==
              XHCI_COMMAND_EVENT_FOREIGN,
          "a completion naming another TRB is foreign to this wait");
    CHECK(xhci_command_event_kind_of(&event, foreign) ==
              XHCI_COMMAND_EVENT_OWN,
          "the same event is the wait's own once the address matches");
    xhci_command_wait_init(&wait);
    xhci_command_wait_note(&wait, &event, XHCI_COMMAND_EVENT_FOREIGN);
    CHECK(wait.events == 1U && wait.unmatched == 1U && wait.unmatched_valid,
          "a foreign command completion is counted as unmatched");
    CHECK(wait.unmatched_address == foreign && wait.unmatched_type == 33U &&
              wait.unmatched_code == 4U && wait.unmatched_slot == 5U,
          "the last unmatched completion keeps its address, type, code and slot");
    CHECK(xhci_command_result_of(true, false) == XHCI_COMMAND_RESULT_TIMEOUT,
          "a wait that only saw foreign completions still ends in a timeout");

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.valid = true;
    snapshot.result = xhci_command_result_of(true, false);
    snapshot.wait = wait;
    CHECK(xhci_format_command_event_record(record, sizeof(record), &snapshot) !=
              0U &&
              strstr(record, "events=1") != NULL &&
              strstr(record, "unmatched=1") != NULL &&
              strstr(record, "last=addr=0x0000000000003000,type=33,code=4,"
                             "slot=5") != NULL,
          "the event record carries the unmatched completion's own fields");
}

static void test_command_snapshot_outlives_the_session(void) {
    struct xhci_command_snapshot captured;
    struct xhci_command_snapshot kept;
    char command[XHCI_COMMAND_RECORD_BYTES];
    char registers[XHCI_COMMAND_RECORD_BYTES];
    char events[XHCI_COMMAND_RECORD_BYTES];

    memset(&captured, 0, sizeof(captured));
    memset(&kept, 0, sizeof(kept));
    captured.valid = true;
    captured.result = XHCI_COMMAND_RESULT_TIMEOUT;
    captured.type = XHCI_TRB_ENABLE_SLOT;
    captured.trb[0] = 0U;
    captured.trb[3] = 0x00000009U | XHCI_TRB_CYCLE;
    captured.physical = 0x00000000BF000040ULL;
    captured.ring_index = 4U;
    captured.ring_cycle = 1U;
    captured.bus = 0U;
    captured.device = 0x14U;
    captured.function = 0U;
    captured.usbcmd = 1U;
    captured.usbsts = 0U;
    captured.config = 0x8U;
    captured.erstsz = 1U;
    captured.iman = 0U;
    captured.crcr = 0x00000000BF001001ULL;
    captured.dcbaap = 0x00000000BF002000ULL;
    captured.erstba = 0x00000000BF003000ULL;
    captured.erdp = 0x00000000BF004008ULL;
    captured.event_index = 2U;
    captured.event_cycle = 1U;
    captured.event_valid = true;
    captured.event[0] = 0x00000001U;
    captured.event[1] = 0U;
    captured.event[2] = 0x01000000U;
    captured.event[3] = 0x01000221U;
    captured.wait.events = 2U;
    captured.wait.unmatched = 1U;
    captured.wait.unmatched_valid = true;
    captured.wait.unmatched_address = 0x00000000BF000050ULL;
    captured.wait.unmatched_type = 33U;
    captured.wait.unmatched_code = 1U;
    captured.wait.unmatched_slot = 2U;

    xhci_command_snapshot_keep(&kept, &captured);
    /* Releasing the session that captured it zeroes everything it held. */
    memset(&captured, 0, sizeof(captured));

    CHECK(kept.valid && kept.result == XHCI_COMMAND_RESULT_TIMEOUT &&
              kept.ring_index == 4U && kept.wait.unmatched == 1U,
          "the machine's record survives the capture buffer being wiped");
    CHECK(xhci_format_command_record(command, sizeof(command), &kept) != 0U &&
              xhci_format_command_register_record(registers, sizeof(registers),
                                                  &kept) != 0U &&
              xhci_format_command_event_record(events, sizeof(events), &kept) !=
                  0U,
          "all three records still format from the kept copy alone");
    CHECK(strstr(command, "result=TIMEOUT") != NULL &&
              strstr(command, "pci=00:14.0") != NULL &&
              strstr(command, "phys=0x00000000BF000040") != NULL,
          "the kept record still names the controller, the address and outcome");
    CHECK(strstr(registers, "usbcmd=0x00000001") != NULL &&
              strstr(registers, "crcr=0x00000000BF001001") != NULL &&
              strstr(registers, "config=0x00000008") != NULL &&
              strstr(registers, "dcbaap=0x00000000BF002000") != NULL &&
              strstr(registers, "erstsz=0x00000001") != NULL &&
              strstr(registers, "erstba=0x00000000BF003000") != NULL &&
              strstr(registers, "erdp=0x00000000BF004008") != NULL &&
              strstr(registers, "iman=0x00000000") != NULL,
          "the register record carries every register the failure snapshot reads");
    CHECK(strstr(events, "index=2") != NULL && strstr(events, "cycle=1") != NULL &&
              strstr(events, "event=0x00000001,0x00000000,0x01000000,0x01000221") !=
                  NULL &&
              strstr(events, "events=2") != NULL,
          "the event record carries the consumer position and the event dwords");
    xhci_command_snapshot_keep(NULL, &kept);
    CHECK(kept.valid, "keeping into nothing is refused and changes nothing");
    xhci_command_snapshot_keep(&kept, NULL);
    CHECK(kept.valid, "keeping nothing is refused and changes nothing");
}

int main(void) {
    test_capability_window();
    test_trb_encoding();
    test_ring_wrap();
    test_event_decode();
    test_ring_full_of_garbage_is_not_an_event();
    test_names();
    test_event_ring_and_arm_depth();
    test_endpoint_state();
    test_speed_tables();
    test_low_speed_endpoint_context();
    test_no_report_window();
    test_portsc_neutral_write();
    test_port_power_claim();
    test_scan_policy();
    test_scan_attempt_budget();
    test_machine_scan_reaches_a_later_controller();
    test_slot_bound();
    test_port_evidence();
    test_port_bring_up_plan();
    test_port_link_state();
    test_port_connect_pending();
    test_endpoint_context_burst();
    test_port_record();
    test_protocol_capability();
    test_record_budget();
    test_input_context_layout();
    test_device_context_layout();
    test_report_queue_and_service_gap();
    test_interrupt_short_packet_and_reason();
    test_watchdog_decision();
    test_watchdog_meaning_and_escalation();
    test_silence_records();
    test_slot_state();
    test_ep0_probe_records();
    test_ep_state_and_timing_records();
    test_command_wait_success();
    test_command_wait_timeout_without_event();
    test_command_wait_foreign_completion();
    test_command_snapshot_outlives_the_session();

    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    puts("xHCI TRB and context host test: ALL OK");
    return 0;
}

#else
#error "tests/xhci-host.c requires -DHFOC_XHCI_HOST_TEST"
#endif
