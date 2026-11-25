#ifndef HFOC_XHCI_EVIDENCE_H
#define HFOC_XHCI_EVIDENCE_H

/*
 * ITEM-573: each surveyed xHCI controller's evidence, kept after its session is
 * stopped, zeroed and reused.
 *
 * The driver already collected everything a refused controller can be read
 * from: the start verdict, USBCMD/USBSTS, the Supported Protocol groups, every
 * port's raw PORTSC before and after the bring-up, the actions taken (power,
 * link kick, settle window), the HID roles' outcomes and the transfer counters.
 * All of it lived in the session, and `xhci_session_release()` copies only a
 * five-field `last_probe` before the stop and the memset - so on the owner's
 * laptop the report carried `NO_DEVICE/NO_CONNECTED_DEVICE` for the controller
 * the mouse was plugged into, and the mouse could not be read at all.
 *
 * This module is the durable half: a bounded, value-only copy per controller,
 * taken before the session is stopped (and again for a session that stays
 * live), held in the machine's RAM, and rendered by `diag save` / the `xhci`
 * command with no MMIO access, no session and no re-initialization.  It is
 * pure: the sink only receives text, the store only copies values.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pci.h"
#include "usb_hid.h"

/* The machine's xHCI controllers (the driver tracks up to eight PCI
   functions, ITEM-406). */
#define XHCI_EVIDENCE_CONTROLLERS 8U
/* One entry per port the driver read.  xHCI's MaxPorts is an eight-bit field;
   this bound covers the largest controller the driver accepts while keeping
   the store bounded, and the ports past it are counted, never silently
   dropped. */
#define XHCI_EVIDENCE_PORTS 32U
#define XHCI_EVIDENCE_PROTOCOLS 4U
#define XHCI_EVIDENCE_ROLES 2U
#define XHCI_EVIDENCE_DETAIL 128U
#define XHCI_EVIDENCE_NAME 5U
#define XHCI_EVIDENCE_STATUS 24U
/* ITEM-578: the stage that decided one role attempt, and the bounded copy of
   the report descriptor the device returned (the bytes the parser read). */
#define XHCI_EVIDENCE_STAGE 24U
#define XHCI_EVIDENCE_DESCRIPTOR_BYTES 32U
/* The report descriptor's read and classification states.  They are separate
   so a transfer that failed at the wire can never be published as a
   descriptor verdict. */
#define XHCI_EVIDENCE_RDESC_NONE 0U
#define XHCI_EVIDENCE_RDESC_READ_OK 1U
#define XHCI_EVIDENCE_RDESC_READ_FAILED 2U
#define XHCI_EVIDENCE_RDESC_PARSE_OK 3U
#define XHCI_EVIDENCE_RDESC_PARSE_REFUSED 4U

typedef void (*xhci_evidence_sink_fn)(void *context, const char *line);

/* One root port, before and after the driver's own preparation. */
struct xhci_evidence_port {
    uint32_t was;      /* PORTSC before any write */
    uint32_t now;      /* PORTSC after the bring-up */
    uint16_t waited_ms;
    uint8_t powered;   /* PORTSC.PP was clear and the driver set it */
    uint8_t kicked;    /* the link was moved to RxDetect */
    uint8_t connected; /* CCS in the final word */
};

/* One HID role's outcome on the controller. */
struct xhci_evidence_role {
    uint8_t role; /* 0 keyboard, 1 mouse (XHCI_HID_ROLE_*) */
    uint8_t seen;
    uint8_t bound;
    uint8_t port;
    uint8_t speed;
    uint8_t slot_id;
    uint8_t dci;
    uint8_t interface_number;
    uint8_t interface_protocol;
    uint8_t endpoint_address;
    uint16_t vendor_id;
    uint16_t product_id;
    uint32_t reports;
    uint32_t transfers;
    uint32_t errors;
    /* ITEM-579: the three counters kept apart - TRBs submitted, transfer events
       completed, reports decoded - plus the endpoint repair evidence. */
    uint32_t completions;
    uint32_t restarts;
    uint8_t endpoint_state;
    uint32_t refused;
    uint32_t dropped;
    uint32_t short_packets;
    uint32_t mse;
    uint32_t event_ring_full;
    uint32_t no_trb;
    uint8_t last_completion_code;
    char interface_summary[USB_HID_INTERFACE_SUMMARY_BYTES];
    /* ITEM-578: this role's own attempt, even when it refused before the
       device's identity was read.  `stage` names the enumeration step and
       `reason` names the status; `detail` is the full port-prefixed detail the
       role published, so the pointer record never borrows the keyboard's. */
    uint8_t attempted;
    char stage[XHCI_EVIDENCE_STAGE];
    char reason[XHCI_EVIDENCE_STATUS];
    char detail[XHCI_EVIDENCE_DETAIL];
    /* The report-descriptor read: its result, its length and the first bytes
       the parser classified. */
    uint8_t report_descriptor_read;
    uint8_t report_descriptor_parse;
    uint16_t report_descriptor_length;
    uint8_t report_descriptor_bytes[XHCI_EVIDENCE_DESCRIPTOR_BYTES];
};

/* ITEM-574: the Intel USB port-routing registers of the xHCI function, as two
   independent snapshots (before the driver's initialization and after it). */
struct xhci_evidence_route {
    bool valid;
    bool read_ok;
    uint32_t xusb2pr;
    uint32_t usb2prm;
    uint32_t usb3_pssen;
    uint32_t usb3prm;
};

/* One Supported Protocol group as the controller declared it. */
struct xhci_evidence_protocol {
    uint8_t major;
    uint8_t minor;
    uint8_t port_offset;
    uint8_t port_count;
    char name[XHCI_EVIDENCE_NAME];
};

struct xhci_evidence_controller {
    bool valid;
    /* True when the copy was taken while the session was being released, which
       means the controller was stopped after it. */
    bool released;
    struct pci_location location;
    uint16_t pci_vendor;
    uint16_t pci_device;
    uint64_t bar_physical;
    uint32_t mmio_bytes;
    uint8_t caplength;
    uint32_t max_ports;
    uint32_t max_slots;
    uint32_t slots_enabled;
    uint32_t usbcmd;
    uint32_t usbsts;
    uint32_t protocol_count;
    struct xhci_evidence_protocol protocols[XHCI_EVIDENCE_PROTOCOLS];
    /* `ports_seen` is what the controller declares, `port_count` what was kept
       (the two differ only past the store's bound). */
    uint32_t ports_seen;
    uint32_t port_count;
    struct xhci_evidence_port ports[XHCI_EVIDENCE_PORTS];
    char status[XHCI_EVIDENCE_STATUS];
    char detail[XHCI_EVIDENCE_DETAIL];
    uint32_t role_count;
    struct xhci_evidence_role roles[XHCI_EVIDENCE_ROLES];
    /* ITEM-574: the routing stage, the BIOS/OS handoff result and the shared
       connect window.  `route_detail` carries the chosen values and the
       EHCI-exposed bits the driver refused to move; `route_outcome` is one of
       xhci_route_outcome_string()'s names. */
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
    /* ITEM-577: the controller's DMA geometry and the first fatal snapshot. */
    uint32_t hcs_params2;
    uint32_t scratchpad_count;
    uint32_t scratchpad_page_bytes;
    uint32_t context_bytes;
    uint32_t slot_area_bytes;
    uint32_t pool_bytes;
    uint64_t dcbaa_physical;
    uint64_t dcbaa0;
    char fatal[XHCI_EVIDENCE_DETAIL];
};

/* The machine-wide pointer verdict, kept apart from any controller. */
struct xhci_evidence_mouse {
    bool valid;
    bool seen;
    bool bound;
    char verdict[XHCI_EVIDENCE_STATUS];
    char detail[XHCI_EVIDENCE_DETAIL];
    char controller[XHCI_EVIDENCE_STATUS];
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t port;
    uint8_t speed;
    uint8_t interface_number;
    uint8_t endpoint_address;
    char interface_summary[USB_HID_INTERFACE_SUMMARY_BYTES];
    uint32_t reports;
    uint32_t transfers;
    uint32_t errors;
    /* ITEM-579: completions and the wheel movements the pointer decoded. */
    uint32_t completions;
    uint32_t wheels;
    /* ITEM-639: the selected wire format, the descriptor's wheel field and the
       last report's actual length, so a physical diag can tell a three-byte
       boot report from a report-protocol wheel without reading the source. */
    bool report_mode;
    uint8_t wheel_offset;
    uint8_t report_length;
    uint32_t last_report_bytes;
};

/* Forget every kept controller: the boot's walk owns this, once. */
void xhci_evidence_reset(void);
/* Keep (or update, keyed by PCI location) one controller's evidence. */
void xhci_evidence_controller_keep(
    const struct xhci_evidence_controller *controller);
/*
 * ITEM-580: update the working fields of a kept controller from a live copy -
 * the operational registers and the roles' current binding, counters and
 * endpoint state - while the visit's evidence (its verdict and refusal reason,
 * routing, handoff, ports, protocol groups, DMA geometry and the first fatal
 * snapshot) stays as the visit recorded it.  A location with no kept record is
 * kept whole, so a machine whose first keep never ran is not lost.
 */
void xhci_evidence_controller_refresh(
    const struct xhci_evidence_controller *live);
/* Keep the machine-wide mouse verdict. */
void xhci_evidence_mouse_keep(const struct xhci_evidence_mouse *mouse);

/* Read back what was kept (the `xhci` command and the host proof). */
size_t xhci_evidence_controller_count(void);
const struct xhci_evidence_controller *xhci_evidence_controller(
    uint32_t index);
const struct xhci_evidence_mouse *xhci_evidence_mouse(void);

/*
 * Render every kept controller and the mouse verdict into the report's sink.
 * One record each: `[XHCI-CTRL]`, `[XHCI-PROTO]`, `[XHCI-PORT]`,
 * `[XHCI-INPUT]` and one `[XHCI-MOUSE]`.  No MMIO, no session, no side effect.
 */
void xhci_evidence_report(xhci_evidence_sink_fn sink, void *context);

#endif
