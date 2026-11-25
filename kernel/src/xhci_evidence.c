/*
 * ITEM-573: the durable per-controller xHCI evidence and its report records.
 *
 * See kernel/include/xhci_evidence.h for why this exists.  The store is a
 * plain, bounded array of values; the renderer turns it into bounded records.
 * Nothing here reads a register, looks at a session, or changes the driver's
 * state - which is what lets `diag save` print a refused controller after its
 * session was released, stopped, zeroed and reused.
 */

#include "xhci_evidence.h"

#include <string.h>

static struct xhci_evidence_controller
    xhci_evidence_controllers[XHCI_EVIDENCE_CONTROLLERS];
static uint32_t evidence_controller_count;
static struct xhci_evidence_mouse xhci_evidence_mouse_state;

/* One bounded line being composed; a field that does not fit sets `overflow`
   and the whole line is dropped rather than published as a fragment. */
struct evidence_line {
    char text[512];
    size_t length;
    bool overflow;
};

static void line_begin(struct evidence_line *line) {
    line->length = 0U;
    line->text[0] = '\0';
    line->overflow = false;
}

static void line_putc(struct evidence_line *line, char value) {
    if (line->length + 1U < sizeof(line->text)) {
        line->text[line->length++] = value;
        line->text[line->length] = '\0';
        return;
    }
    line->overflow = true;
}

static void line_text(struct evidence_line *line, const char *text) {
    while (text != NULL && *text != '\0') {
        line_putc(line, *text++);
    }
}

static void line_u64(struct evidence_line *line, uint64_t value) {
    char digits[20];
    size_t count = 0U;

    if (value == 0U) {
        line_putc(line, '0');
        return;
    }
    while (value != 0U) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count > 0U) {
        line_putc(line, digits[--count]);
    }
}

static void line_hex32(struct evidence_line *line, uint32_t value) {
    static const char digits[] = "0123456789ABCDEF";
    int shift;

    line_text(line, "0x");
    for (shift = 28; shift >= 0; shift -= 4) {
        line_putc(line, digits[(value >> (unsigned int)shift) & 0x0FU]);
    }
}

static void line_hex64(struct evidence_line *line, uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    int shift;

    line_text(line, "0x");
    for (shift = 60; shift >= 0; shift -= 4) {
        line_putc(line, digits[(value >> (unsigned int)shift) & 0x0FU]);
    }
}

/* ITEM-579: the endpoint state the driver recorded, in the record's own
   vocabulary (the store is linked without the TRB module that owns the other
   spelling). */
static const char *evidence_endpoint_state(uint32_t state) {
    switch (state & 0x07U) {
    case 0U:
        return "DISABLED";
    case 1U:
        return "RUNNING";
    case 2U:
        return "HALTED";
    case 3U:
        return "STOPPED";
    case 4U:
        return "ERROR";
    default:
        return "RESERVED";
    }
}

/* ITEM-578: the bounded report-descriptor copy, two uppercase digits per byte
   and no separators, so the record stays one token and a reader can slice it. */
static void line_hex_bytes(struct evidence_line *line, const uint8_t *bytes,
                           size_t count) {
    static const char digits[] = "0123456789ABCDEF";

    if (bytes == NULL) {
        return;
    }
    if (count > XHCI_EVIDENCE_DESCRIPTOR_BYTES) {
        count = XHCI_EVIDENCE_DESCRIPTOR_BYTES;
    }
    for (size_t index = 0U; index < count; ++index) {
        line_putc(line, digits[(bytes[index] >> 4) & 0x0FU]);
        line_putc(line, digits[bytes[index] & 0x0FU]);
    }
}

static void line_pci(struct evidence_line *line,
                     const struct pci_location *location) {
    static const char digits[] = "0123456789ABCDEF";

    line_putc(line, digits[(location->bus >> 4) & 0x0FU]);
    line_putc(line, digits[location->bus & 0x0FU]);
    line_putc(line, ':');
    line_putc(line, digits[(location->device >> 4) & 0x0FU]);
    line_putc(line, digits[location->device & 0x0FU]);
    line_putc(line, '.');
    line_u64(line, location->function);
}

static void evidence_publish(xhci_evidence_sink_fn sink,
                             void *context,
                             const struct evidence_line *line) {
    if (sink != NULL && !line->overflow) {
        sink(context, line->text);
    }
}

void xhci_evidence_reset(void) {
    memset(xhci_evidence_controllers, 0,
           sizeof(xhci_evidence_controllers));
    evidence_controller_count = 0U;
    memset(&xhci_evidence_mouse_state, 0, sizeof(xhci_evidence_mouse_state));
}

/* Keyed by PCI location: a controller is visited once, and a later keep for
   the same function (the silent-candidate rebind) updates the same entry
   rather than appending a second one. */
void xhci_evidence_controller_keep(
    const struct xhci_evidence_controller *controller) {
    if (controller == NULL) {
        return;
    }
    for (uint32_t index = 0U; index < evidence_controller_count; ++index) {
        struct xhci_evidence_controller *slot =
            &xhci_evidence_controllers[index];

        if (slot->location.bus == controller->location.bus &&
            slot->location.device == controller->location.device &&
            slot->location.function == controller->location.function) {
            *slot = *controller;
            slot->valid = true;
            return;
        }
    }
    if (evidence_controller_count < XHCI_EVIDENCE_CONTROLLERS) {
        xhci_evidence_controllers[evidence_controller_count] =
            *controller;
        xhci_evidence_controllers[evidence_controller_count].valid = true;
        evidence_controller_count += 1U;
    }
}

/*
 * ITEM-580: merge a live copy into the kept record.  Only what a running
 * machine changes is taken from the live copy; everything the visit
 * established - including the first fatal snapshot and the original refusal
 * verdict - keeps its value, so the export can never erase why a controller
 * was refused with the fact that a role later started working.
 */
void xhci_evidence_controller_refresh(
    const struct xhci_evidence_controller *live) {
    struct xhci_evidence_controller *kept = NULL;

    if (live == NULL) {
        return;
    }
    for (uint32_t index = 0U; index < evidence_controller_count; ++index) {
        struct xhci_evidence_controller *candidate =
            &xhci_evidence_controllers[index];

        if (candidate->location.bus == live->location.bus &&
            candidate->location.device == live->location.device &&
            candidate->location.function == live->location.function) {
            kept = candidate;
            break;
        }
    }
    if (kept == NULL) {
        xhci_evidence_controller_keep(live);
        return;
    }
    kept->usbcmd = live->usbcmd;
    kept->usbsts = live->usbsts;
    kept->role_count = live->role_count;
    for (uint32_t role = 0U; role < XHCI_EVIDENCE_ROLES; ++role) {
        kept->roles[role] = live->roles[role];
    }
}

static void evidence_text_copy(char *target, size_t size,
                               const char *source) {
    size_t index = 0U;

    if (size == 0U) {
        return;
    }
    while (source != NULL && source[index] != '\0' && index + 1U < size) {
        target[index] = source[index];
        ++index;
    }
    target[index] = '\0';
}

/* The `bb:dd.f` text the report prints, without a formatter. */
static void evidence_location_text(char *target, size_t size,
                                   const struct pci_location *location) {
    static const char digits[] = "0123456789ABCDEF";

    if (size < 8U) {
        if (size > 0U) {
            target[0] = '\0';
        }
        return;
    }
    target[0] = digits[(location->bus >> 4) & 0xFU];
    target[1] = digits[location->bus & 0xFU];
    target[2] = ':';
    target[3] = digits[(location->device >> 4) & 0xFU];
    target[4] = digits[location->device & 0xFU];
    target[5] = '.';
    target[6] = digits[location->function & 0xFU];
    target[7] = '\0';
}

/*
 * ITEM-573: a pointer the driver could not identify was still detected when a
 * port of a kept controller reports a connect: the controller read the device
 * before the enumeration stopped, and the port and the stage are exactly what
 * the report must name.  The verdict is derived here, from the durable copies,
 * so it is the same answer whether or not the driver's own session survived -
 * and a machine whose ports read no connect keeps NOT_DETECTED, so the store
 * never invents a device.
 */
static void evidence_mouse_seen(struct xhci_evidence_mouse *mouse) {
    size_t index;

    if (mouse->bound || mouse->seen) {
        return;
    }
    for (index = 0U; index < evidence_controller_count; ++index) {
        const struct xhci_evidence_controller *controller =
            &xhci_evidence_controllers[index];

        for (uint32_t port = 0U; port < controller->port_count; ++port) {
            if (controller->ports[port].connected == 0U) {
                continue;
            }
            mouse->seen = true;
            mouse->port = (uint8_t)(port + 1U);
            evidence_location_text(mouse->controller,
                                   sizeof(mouse->controller),
                                   &controller->location);
            /*
             * ITEM-578: the pointer names its own stage.  The controller's own
             * detail belongs to whichever role produced it - on the owner's
             * laptop it is the keyboard's "not a keyboard" verdict - so it is
             * never copied here; without a pointer attempt the record says the
             * device is connected and that no pointer outcome exists yet.
             */
            if (mouse->detail[0] == '\0') {
                evidence_text_copy(mouse->detail, sizeof(mouse->detail),
                                   "PORT_CONNECTED");
            }
            evidence_text_copy(mouse->verdict, sizeof(mouse->verdict),
                               "SEEN");
            return;
        }
    }
}

void xhci_evidence_mouse_keep(const struct xhci_evidence_mouse *mouse) {
    if (mouse == NULL) {
        return;
    }
    xhci_evidence_mouse_state = *mouse;
    xhci_evidence_mouse_state.valid = true;
    evidence_mouse_seen(&xhci_evidence_mouse_state);
}

size_t xhci_evidence_controller_count(void) {
    return evidence_controller_count;
}

const struct xhci_evidence_controller *xhci_evidence_controller(
    uint32_t index) {
    if (index >= evidence_controller_count) {
        return NULL;
    }
    return &xhci_evidence_controllers[index];
}

const struct xhci_evidence_mouse *xhci_evidence_mouse(void) {
    return xhci_evidence_mouse_state.valid ? &xhci_evidence_mouse_state : NULL;
}

static void evidence_controller_line(xhci_evidence_sink_fn sink,
                                     void *context,
                                     const struct xhci_evidence_controller *c) {
    struct evidence_line line;

    line_begin(&line);
    line_text(&line, "[XHCI-CTRL] pci=");
    line_pci(&line, &c->location);
    line_text(&line, " state=");
    line_text(&line, c->released ? "RELEASED" : "LIVE");
    line_text(&line, " status=");
    line_text(&line, c->status);
    line_text(&line, " detail=");
    line_text(&line, c->detail);
    line_text(&line, " vid=");
    line_hex32(&line, c->pci_vendor);
    line_text(&line, " did=");
    line_hex32(&line, c->pci_device);
    line_text(&line, " bar=");
    line_hex64(&line, c->bar_physical);
    line_text(&line, " mmio=");
    line_u64(&line, c->mmio_bytes);
    line_text(&line, " cap=");
    line_hex32(&line, c->caplength);
    line_text(&line, " ports=");
    line_u64(&line, c->ports_seen);
    line_text(&line, " slots=");
    line_u64(&line, c->max_slots);
    line_text(&line, " enabled=");
    line_u64(&line, c->slots_enabled);
    line_text(&line, " usbcmd=");
    line_hex32(&line, c->usbcmd);
    line_text(&line, " usbsts=");
    line_hex32(&line, c->usbsts);
    line_text(&line, " protocols=");
    line_u64(&line, c->protocol_count);
    line_text(&line, " settle_ms=");
    line_u64(&line, c->settle_ms);
    line_text(&line, " settle_bound_ms=");
    line_u64(&line, c->settle_bound_ms);
    line_text(&line, " settle_pending=");
    line_u64(&line, c->settle_pending ? 1U : 0U);
    evidence_publish(sink, context, &line);

    if (c->hcs_params2 != 0U || c->scratchpad_count != 0U ||
        c->slot_area_bytes != 0U) {
        line_begin(&line);
        line_text(&line, "[XHCI-DMA] pci=");
        line_pci(&line, &c->location);
        line_text(&line, " hcsparams2=");
        line_hex32(&line, c->hcs_params2);
        line_text(&line, " spb=");
        line_u64(&line, c->scratchpad_count);
        line_text(&line, " page=");
        line_u64(&line, c->scratchpad_page_bytes);
        line_text(&line, " ctx=");
        line_u64(&line, c->context_bytes);
        line_text(&line, " slot=");
        line_u64(&line, c->slot_area_bytes);
        line_text(&line, " pool=");
        line_u64(&line, c->pool_bytes);
        line_text(&line, " dcbaa=");
        line_hex64(&line, c->dcbaa_physical);
        line_text(&line, " dcbaa0=");
        line_hex64(&line, c->dcbaa0);
        evidence_publish(sink, context, &line);
    }
    if (c->fatal[0] != '\0') {
        line_begin(&line);
        line_text(&line, "[XHCI-FATAL] pci=");
        line_pci(&line, &c->location);
        line_text(&line, " ");
        line_text(&line, c->fatal);
        evidence_publish(sink, context, &line);
    }

    if (c->route_before.valid) {
        line_begin(&line);
        line_text(&line, "[XHCI-ROUTE] pci=");
        line_pci(&line, &c->location);
        line_text(&line, " phase=BEFORE read=");
        line_text(&line, c->route_before.read_ok ? "OK" : "FAIL");
        line_text(&line, " xusb2pr=");
        line_hex32(&line, c->route_before.xusb2pr);
        line_text(&line, " usb2prm=");
        line_hex32(&line, c->route_before.usb2prm);
        line_text(&line, " usb3_pssen=");
        line_hex32(&line, c->route_before.usb3_pssen);
        line_text(&line, " usb3prm=");
        line_hex32(&line, c->route_before.usb3prm);
        evidence_publish(sink, context, &line);
    }
    if (c->route_after.valid) {
        line_begin(&line);
        line_text(&line, "[XHCI-ROUTE] pci=");
        line_pci(&line, &c->location);
        line_text(&line, " phase=AFTER read=");
        line_text(&line, c->route_after.read_ok ? "OK" : "FAIL");
        line_text(&line, " xusb2pr=");
        line_hex32(&line, c->route_after.xusb2pr);
        line_text(&line, " usb2prm=");
        line_hex32(&line, c->route_after.usb2prm);
        line_text(&line, " usb3_pssen=");
        line_hex32(&line, c->route_after.usb3_pssen);
        line_text(&line, " usb3prm=");
        line_hex32(&line, c->route_after.usb3prm);
        line_text(&line, " action=");
        line_text(&line, c->route_outcome);
        line_text(&line, " target_usb2=");
        line_hex32(&line, c->route_target_usb2);
        line_text(&line, " target_usb3=");
        line_hex32(&line, c->route_target_usb3);
        line_text(&line, " field=");
        line_hex32(&line, c->route_usb2_field);
        line_text(&line, " wrote=");
        line_u64(&line, c->route_wrote_usb3 ? 1U : 0U);
        line_u64(&line, c->route_wrote_usb2 ? 1U : 0U);
        line_text(&line, " restored=");
        line_u64(&line, c->route_restored ? 1U : 0U);
        if (c->route_detail[0] != '\0') {
            line_text(&line, " detail=");
            line_text(&line, c->route_detail);
        }
        evidence_publish(sink, context, &line);
    }
    if (c->legacy[0] != '\0') {
        line_begin(&line);
        line_text(&line, "[XHCI-LEGACY] pci=");
        line_pci(&line, &c->location);
        line_text(&line, " ");
        line_text(&line, c->legacy);
        evidence_publish(sink, context, &line);
    }

    for (uint32_t index = 0U;
         index < c->protocol_count && index < XHCI_EVIDENCE_PROTOCOLS;
         ++index) {
        const struct xhci_evidence_protocol *protocol = &c->protocols[index];

        line_begin(&line);
        line_text(&line, "[XHCI-PROTO] pci=");
        line_pci(&line, &c->location);
        line_text(&line, " index=");
        line_u64(&line, index);
        line_text(&line, " major=");
        line_u64(&line, protocol->major);
        line_text(&line, " minor=");
        line_u64(&line, protocol->minor);
        line_text(&line, " name=");
        line_text(&line, protocol->name);
        line_text(&line, " off=");
        line_u64(&line, protocol->port_offset);
        line_text(&line, " count=");
        line_u64(&line, protocol->port_count);
        evidence_publish(sink, context, &line);
    }

    for (uint32_t index = 0U;
         index < c->port_count && index < XHCI_EVIDENCE_PORTS; ++index) {
        const struct xhci_evidence_port *port = &c->ports[index];

        line_begin(&line);
        line_text(&line, "[XHCI-PORT] pci=");
        line_pci(&line, &c->location);
        line_text(&line, " port=");
        line_u64(&line, index + 1U);
        line_text(&line, " was=");
        line_hex32(&line, port->was);
        line_text(&line, " now=");
        line_hex32(&line, port->now);
        line_text(&line, " pon=");
        line_u64(&line, port->powered);
        line_text(&line, " kick=");
        line_u64(&line, port->kicked);
        line_text(&line, " wait_ms=");
        line_u64(&line, port->waited_ms);
        line_text(&line, " ccs=");
        line_u64(&line, port->connected);
        evidence_publish(sink, context, &line);
    }

    for (uint32_t index = 0U;
         index < c->role_count && index < XHCI_EVIDENCE_ROLES; ++index) {
        const struct xhci_evidence_role *role = &c->roles[index];

        /*
         * ITEM-578: a role that was attempted is published even when it stopped
         * before the device's identity was read - that attempt is what says the
         * pointer's stage is its own and not the keyboard's.
         */
        if (!role->attempted && !role->seen && !role->bound) {
            continue;
        }
        line_begin(&line);
        line_text(&line, "[XHCI-INPUT] pci=");
        line_pci(&line, &c->location);
        line_text(&line, " role=");
        line_text(&line, role->role == 1U ? "MOUSE" : "KEYBOARD");
        line_text(&line, " state=");
        line_text(&line, role->bound
                             ? "BOUND"
                             : (role->attempted || role->seen ? "REFUSED"
                                                              : "NONE"));
        line_text(&line, " port=");
        line_u64(&line, role->port);
        line_text(&line, " speed=");
        line_u64(&line, role->speed);
        line_text(&line, " vid=");
        line_hex32(&line, role->vendor_id);
        line_text(&line, " pid=");
        line_hex32(&line, role->product_id);
        line_text(&line, " slot=");
        line_u64(&line, role->slot_id);
        line_text(&line, " dci=");
        line_u64(&line, role->dci);
        line_text(&line, " iface=");
        line_u64(&line, role->interface_number);
        line_text(&line, " proto=");
        line_u64(&line, role->interface_protocol);
        line_text(&line, " ep=");
        line_hex32(&line, role->endpoint_address);
        /*
         * ITEM-579: submitted, completed and decoded are three different
         * numbers.  A pointer whose transfers complete but which decodes no
         * report is a decoder problem; one whose transfers never complete is a
         * transport problem; the endpoint state and the repair count say which.
         */
        line_text(&line, " submitted=");
        line_u64(&line, role->transfers);
        line_text(&line, " completions=");
        line_u64(&line, role->completions);
        line_text(&line, " reports=");
        line_u64(&line, role->reports);
        line_text(&line, " epstate=");
        line_text(&line, evidence_endpoint_state(role->endpoint_state));
        line_text(&line, " restarts=");
        line_u64(&line, role->restarts);
        line_text(&line, " errors=");
        line_u64(&line, role->errors);
        line_text(&line, " refused=");
        line_u64(&line, role->refused);
        line_text(&line, " dropped=");
        line_u64(&line, role->dropped);
        line_text(&line, " short=");
        line_u64(&line, role->short_packets);
        line_text(&line, " mse=");
        line_u64(&line, role->mse);
        line_text(&line, " erfull=");
        line_u64(&line, role->event_ring_full);
        line_text(&line, " notrb=");
        line_u64(&line, role->no_trb);
        line_text(&line, " last=");
        line_u64(&line, role->last_completion_code);
        if (role->attempted) {
            line_text(&line, " stage=");
            line_text(&line, role->stage[0] != '\0' ? role->stage : "NONE");
            line_text(&line, " reason=");
            line_text(&line, role->reason[0] != '\0' ? role->reason : "UNKNOWN");
            if (role->detail[0] != '\0') {
                line_text(&line, " detail=");
                line_text(&line, role->detail);
            }
        }
        evidence_publish(sink, context, &line);

        /*
         * ITEM-578: what the report-descriptor read actually returned.  The
         * result, the length and a bounded copy of the bytes are the three
         * facts that tell a refused device from a failed transfer.
         */
        if (role->report_descriptor_read != XHCI_EVIDENCE_RDESC_NONE) {
            const char *read_state =
                role->report_descriptor_read == XHCI_EVIDENCE_RDESC_READ_OK
                    ? "OK"
                    : "FAILED";
            const char *parse_state =
                role->report_descriptor_parse == XHCI_EVIDENCE_RDESC_PARSE_OK
                    ? "OK"
                    : (role->report_descriptor_parse ==
                               XHCI_EVIDENCE_RDESC_PARSE_REFUSED
                           ? "REFUSED"
                           : "SKIPPED");
            uint32_t copies = role->report_descriptor_length;

            if (copies > XHCI_EVIDENCE_DESCRIPTOR_BYTES) {
                copies = XHCI_EVIDENCE_DESCRIPTOR_BYTES;
            }
            line_begin(&line);
            line_text(&line, "[XHCI-RDESC] pci=");
            line_pci(&line, &c->location);
            line_text(&line, " role=");
            line_text(&line, role->role == 1U ? "MOUSE" : "KEYBOARD");
            line_text(&line, " port=");
            line_u64(&line, role->port);
            line_text(&line, " iface=");
            line_u64(&line, role->interface_number);
            line_text(&line, " read=");
            line_text(&line, read_state);
            line_text(&line, " parse=");
            line_text(&line, parse_state);
            line_text(&line, " length=");
            line_u64(&line, role->report_descriptor_length);
            line_text(&line, " bytes=");
            if (copies == 0U) {
                line_text(&line, "NONE");
            } else {
                line_hex_bytes(&line, role->report_descriptor_bytes, copies);
            }
            evidence_publish(sink, context, &line);
        }
    }
}

static void evidence_mouse_line(xhci_evidence_sink_fn sink, void *context) {
    const struct xhci_evidence_mouse *mouse = &xhci_evidence_mouse_state;
    struct evidence_line line;

    if (!mouse->valid) {
        return;
    }
    line_begin(&line);
    line_text(&line, "[XHCI-MOUSE] verdict=");
    line_text(&line, mouse->verdict);
    line_text(&line, " detail=");
    line_text(&line, mouse->detail);
    line_text(&line, " seen=");
    line_u64(&line, mouse->seen ? 1U : 0U);
    line_text(&line, " bound=");
    line_u64(&line, mouse->bound ? 1U : 0U);
    line_text(&line, " controller=");
    line_text(&line, mouse->controller);
    line_text(&line, " port=");
    line_u64(&line, mouse->port);
    line_text(&line, " speed=");
    line_u64(&line, mouse->speed);
    line_text(&line, " vid=");
    line_hex32(&line, mouse->vendor_id);
    line_text(&line, " pid=");
    line_hex32(&line, mouse->product_id);
    line_text(&line, " iface=");
    line_u64(&line, mouse->interface_number);
    line_text(&line, " ep=");
    line_hex32(&line, mouse->endpoint_address);
    line_text(&line, " mode=");
    line_text(&line, mouse->report_mode ? "REPORT" : "BOOT");
    line_text(&line, " wheel_offset=");
    if (mouse->wheel_offset == USB_HID_REPORT_OFFSET_NONE) {
        line_text(&line, "none");
    } else {
        line_u64(&line, mouse->wheel_offset);
    }
    line_text(&line, " report_length=");
    line_u64(&line, mouse->report_length);
    line_text(&line, " last_bytes=");
    line_u64(&line, mouse->last_report_bytes);
    line_text(&line, " submitted=");
    line_u64(&line, mouse->transfers);
    line_text(&line, " completions=");
    line_u64(&line, mouse->completions);
    line_text(&line, " reports=");
    line_u64(&line, mouse->reports);
    line_text(&line, " wheels=");
    line_u64(&line, mouse->wheels);
    line_text(&line, " errors=");
    line_u64(&line, mouse->errors);
    evidence_publish(sink, context, &line);
}

void xhci_evidence_report(xhci_evidence_sink_fn sink, void *context) {
    if (sink == NULL) {
        return;
    }
    for (uint32_t index = 0U; index < evidence_controller_count;
         ++index) {
        evidence_controller_line(sink, context,
                                 &xhci_evidence_controllers[index]);
    }
    evidence_mouse_line(sink, context);
}
