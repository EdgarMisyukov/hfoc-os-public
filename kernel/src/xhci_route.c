/*
 * ITEM-574/576: the Intel xHCI USB port-routing stage - the pure decision half.
 *
 * See kernel/include/xhci_route.h for the documented 8086:1E31 participants
 * (xHCI 00:14.0, medium EHCI 00:1A.0, routing EHCI 00:1D.0), the four routing
 * registers and the Linux reference implementation.  This file contains no
 * MMIO and no driver state: every access goes through `struct xhci_route_ops`,
 * which is what lets tests/xhci-route-host.c substitute a configuration space
 * (the three functions, a refusing one, a lying one) and pin the decision.
 */

#include "xhci_route.h"

#include <string.h>

#define XHCI_ROUTE_PCI_VENDOR_INTEL 0x8086U
#define XHCI_ROUTE_PCI_CLASS_SERIAL_BUS 0x0CU
#define XHCI_ROUTE_PCI_SUBCLASS_USB 0x03U
#define XHCI_ROUTE_PCI_PROGIF_EHCI 0x20U

/* ---------------------------------------------------------------------------
 * A bounded detail string.  A field that does not fit is the end of the
 * message, never a half-written number.
 */

struct route_text {
    char *data;
    size_t size;
    size_t length;
};

static void text_reset(struct route_text *text, char *data, size_t size) {
    text->data = data;
    text->size = size;
    text->length = 0U;
    if (size > 0U) {
        data[0] = '\0';
    }
}

static void text_put(struct route_text *text, char value) {
    if (text->size == 0U || text->length + 1U >= text->size) {
        return;
    }
    text->data[text->length++] = value;
    text->data[text->length] = '\0';
}

static void text_str(struct route_text *text, const char *value) {
    while (value != NULL && *value != '\0') {
        text_put(text, *value++);
    }
}

static void text_hex32(struct route_text *text, uint32_t value) {
    static const char digits[] = "0123456789ABCDEF";

    text_str(text, "0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        text_put(text, digits[(value >> (unsigned int)shift) & 0x0FU]);
    }
}

static void text_key_hex(struct route_text *text, const char *key,
                         uint32_t value) {
    text_str(text, key);
    text_hex32(text, value);
}

/* ---------------------------------------------------------------------------
 * Configuration-space access through the caller's seam.  A read that the
 * configuration space refuses, or that answers all-ones (the "no function"
 * answer), is a failure: no decision is derived from it.
 */

static bool route_read(const struct xhci_route_ops *ops, void *context,
                       uint8_t bus, uint8_t device, uint8_t function,
                       uint16_t offset, uint32_t *value) {
    if (ops == NULL || ops->read32 == NULL || value == NULL) {
        return false;
    }
    *value = 0xFFFFFFFFU;
    if (!ops->read32(context, bus, device, function, offset, value)) {
        return false;
    }
    return *value != 0xFFFFFFFFU;
}

static bool route_write(const struct xhci_route_ops *ops, void *context,
                        uint8_t bus, uint8_t device, uint8_t function,
                        uint16_t offset, uint32_t value) {
    if (ops == NULL || ops->write32 == NULL) {
        return false;
    }
    return ops->write32(context, bus, device, function, offset, value);
}

static bool route_read_xhci_regs(const struct xhci_route_ops *ops,
                                 void *context, uint8_t bus, uint8_t device,
                                 uint8_t function,
                                 struct xhci_route_regs *regs) {
    bool ok = true;

    memset(regs, 0, sizeof(*regs));
    if (!route_read(ops, context, bus, device, function,
                    XHCI_ROUTE_XUSB2PR, &regs->xusb2pr)) {
        ok = false;
    }
    if (!route_read(ops, context, bus, device, function,
                    XHCI_ROUTE_USB2PRM, &regs->usb2prm)) {
        ok = false;
    }
    if (!route_read(ops, context, bus, device, function,
                    XHCI_ROUTE_USB3_PSSEN, &regs->usb3_pssen)) {
        ok = false;
    }
    if (!route_read(ops, context, bus, device, function,
                    XHCI_ROUTE_USB3PRM, &regs->usb3prm)) {
        ok = false;
    }
    regs->read_ok = ok;
    return ok;
}

/* ---------------------------------------------------------------------------
 * The documented participants.
 *
 * A participant is a PCI function that must exist and identify itself as an
 * Intel EHCI controller.  `device_id` and the class fields are recorded for
 * the exported detail; the function address is the rule's input.
 */

struct route_participant {
    const char *name;
    uint8_t device;
    uint16_t device_id;
    uint32_t class_word;
    const char *status; /* OK, READ, VID or CLASS */
    bool present;
};

static bool route_read_participant(const struct xhci_route_ops *ops,
                                   void *context, struct route_participant *part) {
    uint32_t identity;
    uint32_t class_word;

    part->present = false;
    part->status = "OK";
    if (!route_read(ops, context, XHCI_ROUTE_1E31_BUS, part->device,
                    XHCI_ROUTE_1E31_FUNCTION, 0x00U, &identity)) {
        part->status = "READ";
        return false;
    }
    if ((identity & 0xFFFFU) != XHCI_ROUTE_PCI_VENDOR_INTEL) {
        part->status = "VID";
        return false;
    }
    if (!route_read(ops, context, XHCI_ROUTE_1E31_BUS, part->device,
                    XHCI_ROUTE_1E31_FUNCTION, 0x08U, &class_word)) {
        part->status = "READ";
        return false;
    }
    if (((class_word >> 24U) & 0xFFU) != XHCI_ROUTE_PCI_CLASS_SERIAL_BUS ||
        ((class_word >> 16U) & 0xFFU) != XHCI_ROUTE_PCI_SUBCLASS_USB ||
        ((class_word >> 8U) & 0xFFU) != XHCI_ROUTE_PCI_PROGIF_EHCI) {
        part->status = "CLASS";
        return false;
    }
    part->device_id = (uint16_t)(identity >> 16U);
    part->class_word = class_word;
    part->present = true;
    return true;
}

/* ---------------------------------------------------------------------------
 * The decision detail text.
 */

static void detail_refusal(struct xhci_route_result *result, uint16_t vendor,
                           uint16_t device, const char *reason) {
    struct route_text text;

    text_reset(&text, result->detail, sizeof(result->detail));
    text_key_hex(&text, "vendor=", vendor);
    text_key_hex(&text, " device=", device);
    if (reason != NULL && *reason != '\0') {
        text_str(&text, " ");
        text_str(&text, reason);
    }
}

static void detail_participant_failure(struct xhci_route_result *result,
                                       const struct route_participant *medium,
                                       const struct route_participant *route) {
    struct route_text text;

    text_reset(&text, result->detail, sizeof(result->detail));
    if (!medium->present) {
        text_str(&text, "d26=");
        text_str(&text, medium->status);
    } else {
        text_str(&text, "d29=");
        text_str(&text, route->status);
    }
}

static void detail_topology(struct xhci_route_result *result,
                            const struct route_participant *medium,
                            const struct route_participant *route) {
    struct route_text text;

    text_reset(&text, result->detail, sizeof(result->detail));
    text_str(&text, medium->present ? "d26=OK" : "d26=BAD");
    text_str(&text, route->present ? " d29=OK" : " d29=BAD");
    if (result->before.read_ok) {
        text_key_hex(&text, " u2m=", result->before.usb2prm);
        text_key_hex(&text, " u2=", result->target_usb2);
        text_key_hex(&text, " u3m=", result->before.usb3prm);
        text_key_hex(&text, " u3=", result->target_usb3);
    }
}

/* ---------------------------------------------------------------------------
 * Public API.
 */

bool xhci_route_device_known(uint16_t vendor, uint16_t device) {
    if (vendor != XHCI_ROUTE_PCI_VENDOR_INTEL) {
        return false;
    }
    switch (device) {
    case 0x1E31U: /* 7 Series/C210 - Panther Point (the L875's controller) */
    case 0x8C31U: /* 8 Series/C220 - Lynx Point */
    case 0x8CB1U: /* 8 Series/C220 - Lynx Point */
    case 0x9C31U: /* 8 Series - Lynx Point-LP */
    case 0x9CB1U: /* 8 Series - Wildcat Point-LP */
        return true;
    default:
        return false;
    }
}

const char *xhci_route_outcome_string(enum xhci_route_outcome outcome) {
    switch (outcome) {
    case XHCI_ROUTE_OK:
        return "OK";
    case XHCI_ROUTE_SKIP_NOT_INTEL:
        return "SKIP_NOT_INTEL";
    case XHCI_ROUTE_SKIP_UNKNOWN_DEVICE:
        return "SKIP_UNKNOWN_DEVICE";
    case XHCI_ROUTE_SKIP_UNVERIFIED_TOPOLOGY:
        return "SKIP_UNVERIFIED_TOPOLOGY";
    case XHCI_ROUTE_REFUSE_TOPOLOGY:
        return "REFUSE_TOPOLOGY";
    case XHCI_ROUTE_REFUSE_READ:
        return "REFUSE_READ";
    case XHCI_ROUTE_REFUSE_MASK:
        return "REFUSE_MASK";
    case XHCI_ROUTE_SKIP_NOTHING:
        return "SKIP_NOTHING";
    case XHCI_ROUTE_FAIL_WRITE:
        return "FAIL_WRITE";
    case XHCI_ROUTE_FAIL_READBACK:
        return "FAIL_READBACK";
    default:
        return "UNKNOWN";
    }
}

/* Write one register and verify it: an access the configuration space refuses
   is a write failure, an accepted write whose read-back differs is a
   read-back failure. */
static enum xhci_route_outcome route_apply(const struct xhci_route_ops *ops,
                                           void *context, uint8_t bus,
                                           uint8_t device, uint8_t function,
                                           uint16_t offset, uint32_t value) {
    uint32_t read_back = 0U;

    if (!route_write(ops, context, bus, device, function, offset, value)) {
        return XHCI_ROUTE_FAIL_WRITE;
    }
    if (!route_read(ops, context, bus, device, function, offset, &read_back) ||
        read_back != value) {
        return XHCI_ROUTE_FAIL_READBACK;
    }
    return XHCI_ROUTE_OK;
}

/* Put the previous routing back after a failed write or read-back.  Only the
   registers this call actually changed are written. */
static bool route_restore(const struct xhci_route_ops *ops, void *context,
                          uint8_t bus, uint8_t device, uint8_t function,
                          struct xhci_route_result *result) {
    bool wrote = true;

    if (result->target_usb3 != result->before.usb3_pssen) {
        wrote = route_write(ops, context, bus, device, function,
                            XHCI_ROUTE_USB3_PSSEN,
                            result->before.usb3_pssen) &&
                wrote;
    }
    if (result->target_usb2 != result->before.xusb2pr) {
        wrote = route_write(ops, context, bus, device, function,
                            XHCI_ROUTE_XUSB2PR,
                            result->before.xusb2pr) &&
                wrote;
    }
    if (!wrote) {
        result->restored = false;
        return false;
    }
    if (!route_read_xhci_regs(ops, context, bus, device, function,
                              &result->after)) {
        result->restored = false;
        return false;
    }
    result->restored =
        (result->target_usb3 == result->before.usb3_pssen ||
         result->after.usb3_pssen == result->before.usb3_pssen) &&
        (result->target_usb2 == result->before.xusb2pr ||
         result->after.xusb2pr == result->before.xusb2pr);
    return result->restored;
}

void xhci_route_ports(const struct xhci_route_ops *ops, void *context,
                      uint8_t bus, uint8_t device, uint8_t function,
                      uint16_t vendor, uint16_t device_id,
                      struct xhci_route_result *result) {
    struct route_participant medium = {
        "d26", XHCI_ROUTE_1E31_MEDIUM_EHCI_DEVICE, 0U, 0U, "OK", false};
    struct route_participant route = {
        "d29", XHCI_ROUTE_1E31_ROUTE_EHCI_DEVICE, 0U, 0U, "OK", false};
    uint32_t writable_usb2;
    uint32_t writable_usb3;
    enum xhci_route_outcome outcome;

    if (result == NULL) {
        return;
    }
    memset(result, 0, sizeof(*result));
    detail_refusal(result, vendor, device_id, "");

    if (vendor != XHCI_ROUTE_PCI_VENDOR_INTEL) {
        result->outcome = XHCI_ROUTE_SKIP_NOT_INTEL;
        return;
    }
    if (device_id != 0x1E31U) {
        /* The other known 7/8-series IDs have their own participant layouts
           and field widths; applying Panther Point's addresses to them would
           be a guess, so they are evidence-only until checked the same way. */
        result->outcome = xhci_route_device_known(vendor, device_id)
                              ? XHCI_ROUTE_SKIP_UNVERIFIED_TOPOLOGY
                              : XHCI_ROUTE_SKIP_UNKNOWN_DEVICE;
        return;
    }
    if (ops == NULL || ops->read32 == NULL || ops->write32 == NULL) {
        result->outcome = XHCI_ROUTE_REFUSE_READ;
        return;
    }
    if (bus != XHCI_ROUTE_1E31_BUS || device != XHCI_ROUTE_1E31_XHCI_DEVICE ||
        function != XHCI_ROUTE_1E31_FUNCTION) {
        /* The register semantics below belong to D20:F0.  A 1E31 xHCI at
           another address is not the documented topology. */
        result->outcome = XHCI_ROUTE_REFUSE_TOPOLOGY;
        detail_refusal(result, vendor, device_id, "xhci=NOT_00:14.0");
        return;
    }
    if (!route_read_participant(ops, context, &medium) ||
        !route_read_participant(ops, context, &route)) {
        /* 00:1A.0 is the controller whose root port carries the boot medium
           (behind the RMH); 00:1D.0 is the routing domain.  Both are part of
           the documented topology; if either is not that function, the rule
           does not apply. */
        result->outcome = XHCI_ROUTE_REFUSE_TOPOLOGY;
        detail_participant_failure(result, &medium, &route);
        return;
    }
    if (!route_read_xhci_regs(ops, context, bus, device, function,
                              &result->before)) {
        result->outcome = XHCI_ROUTE_REFUSE_READ;
        detail_topology(result, &medium, &route);
        return;
    }
    result->usb2_field = XHCI_ROUTE_USB2_FIELD;
    result->usb3_field = XHCI_ROUTE_USB3_FIELD;
    if ((result->before.usb2prm & ~XHCI_ROUTE_USB2_FIELD) != 0U ||
        (result->before.usb3prm & ~XHCI_ROUTE_USB3_FIELD) != 0U) {
        result->outcome = XHCI_ROUTE_REFUSE_MASK;
        detail_topology(result, &medium, &route);
        return;
    }
    writable_usb2 = result->before.usb2prm & XHCI_ROUTE_USB2_FIELD;
    writable_usb3 = result->before.usb3prm & XHCI_ROUTE_USB3_FIELD;
    result->writable_usb2 = writable_usb2;
    result->writable_usb3 = writable_usb3;
    /* The switchable bits become the routing value; bits outside the field
       (reserved) are preserved. */
    result->target_usb2 =
        (result->before.xusb2pr & ~XHCI_ROUTE_USB2_FIELD) | writable_usb2;
    result->target_usb3 = (result->before.usb3_pssen &
                           ~XHCI_ROUTE_USB3_FIELD) |
                          writable_usb3;
    if (writable_usb2 == 0U && writable_usb3 == 0U) {
        result->outcome = XHCI_ROUTE_SKIP_NOTHING;
        detail_topology(result, &medium, &route);
        return;
    }

    /* SuperSpeed terminations first: Linux's order, so a USB 3.0 device sees
       the terminations before the USB 2.0 wires move. */
    if (result->target_usb3 != result->before.usb3_pssen) {
        outcome = route_apply(ops, context, bus, device, function,
                              XHCI_ROUTE_USB3_PSSEN, result->target_usb3);
        if (outcome != XHCI_ROUTE_OK) {
            result->outcome = outcome;
            (void)route_restore(ops, context, bus, device, function, result);
            detail_topology(result, &medium, &route);
            return;
        }
        result->wrote_usb3 = true;
    }
    if (result->target_usb2 != result->before.xusb2pr) {
        outcome = route_apply(ops, context, bus, device, function,
                              XHCI_ROUTE_XUSB2PR, result->target_usb2);
        if (outcome != XHCI_ROUTE_OK) {
            result->outcome = outcome;
            (void)route_restore(ops, context, bus, device, function, result);
            detail_topology(result, &medium, &route);
            return;
        }
        result->wrote_usb2 = true;
    }

    if (!route_read_xhci_regs(ops, context, bus, device, function,
                              &result->after)) {
        result->outcome = XHCI_ROUTE_FAIL_READBACK;
        (void)route_restore(ops, context, bus, device, function, result);
        detail_topology(result, &medium, &route);
        return;
    }
    result->outcome = XHCI_ROUTE_OK;
    detail_topology(result, &medium, &route);
}
