/*
 * ITEM-573 host proof: the refused controller survives its session.
 *
 * The owner's laptop answered `NO_DEVICE/NO_CONNECTED_DEVICE` for the xHCI
 * function the mouse was plugged into, and nothing else about it reached the
 * report: the driver had read every port, the Supported Protocol groups, the
 * start verdict and the HID roles' outcomes, but a visit that binds nothing
 * releases its session and the release zeroed it after copying five fields.
 * This proof drives the durable store the way the driver does - keep, then
 * destroy the source as `xhci_session_release()` does, then render - and pins:
 *
 *   * the raw PORTSC of every port before and after the bring-up, the actions
 *     taken, and the start verdict survive the source being zeroed;
 *   * the record does not read the source after the keep (the source is
 *     poisoned);
 *   * two controllers keep two independent records;
 *   * a re-visit of the same PCI function updates its record, not appends;
 *   * the pointer verdict covers bound, seen-but-refused and not-detected.
 */
#include "xhci_evidence.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } \
} while (0)

struct collected {
    char lines[64][512];
    size_t count;
};

static void collect(void *context, const char *line) {
    struct collected *out = context;

    if (out->count < 64U) {
        snprintf(out->lines[out->count], sizeof(out->lines[0]), "%s", line);
        out->count += 1U;
    }
}

static const char *find_line(const struct collected *out, const char *needle) {
    for (size_t index = 0U; index < out->count; ++index) {
        if (strstr(out->lines[index], needle) != NULL) {
            return out->lines[index];
        }
    }
    return NULL;
}

static struct xhci_evidence_controller controller(uint8_t bus, uint8_t device,
                                                  uint16_t vid,
                                                  uint16_t did) {
    struct xhci_evidence_controller controller;

    memset(&controller, 0, sizeof(controller));
    controller.valid = true;
    controller.location.bus = bus;
    controller.location.device = device;
    controller.location.function = 0U;
    controller.pci_vendor = vid;
    controller.pci_device = did;
    controller.bar_physical = 0xFEBD8000ULL;
    controller.mmio_bytes = 65536U;
    controller.caplength = 0x80U;
    controller.max_ports = 2U;
    controller.max_slots = 64U;
    controller.slots_enabled = 64U;
    controller.usbcmd = 0x1U;
    controller.usbsts = 0x0U;
    controller.ports_seen = 2U;
    controller.port_count = 2U;
    controller.ports[0].was = 0x000202A0U;
    controller.ports[0].now = 0x000202A0U;
    controller.ports[0].powered = 1U;
    controller.ports[0].kicked = 0U;
    controller.ports[0].waited_ms = 100U;
    controller.ports[0].connected = 0U;
    controller.ports[1].was = 0x000002A0U;
    controller.ports[1].now = 0x000003A1U;
    controller.ports[1].powered = 1U;
    controller.ports[1].kicked = 1U;
    controller.ports[1].waited_ms = 40U;
    controller.ports[1].connected = 1U;
    controller.protocol_count = 1U;
    controller.protocols[0].major = 3U;
    controller.protocols[0].minor = 0U;
    controller.protocols[0].port_offset = 1U;
    controller.protocols[0].port_count = 1U;
    memcpy(controller.protocols[0].name, "USB ", 4U);
    snprintf(controller.status, sizeof(controller.status), "%s",
             "ENUMERATION_FAILED");
    snprintf(controller.detail, sizeof(controller.detail), "%s",
             "P1_ENABLE_SLOT_TIMEOUT");
    /* ITEM-576: the measured ae67d8ee registers (0/F/0/F) were routed by the
       documented 8086:1E31 rule; the firmware had already handed the
       controller over. */
    controller.route_before.valid = true;
    controller.route_before.read_ok = true;
    controller.route_before.xusb2pr = 0x00000000U;
    controller.route_before.usb2prm = 0x0000000FU;
    controller.route_before.usb3_pssen = 0x00000000U;
    controller.route_before.usb3prm = 0x0000000FU;
    controller.route_after.valid = true;
    controller.route_after.read_ok = true;
    controller.route_after.xusb2pr = 0x0000000FU;
    controller.route_after.usb2prm = 0x0000000FU;
    controller.route_after.usb3_pssen = 0x0000000FU;
    controller.route_after.usb3prm = 0x0000000FU;
    snprintf(controller.route_outcome, sizeof(controller.route_outcome), "%s",
             "OK");
    snprintf(controller.route_detail, sizeof(controller.route_detail), "%s",
             "d26=OK d29=OK u2m=0x0000000F u2=0x0000000F "
             "u3m=0x0000000F u3=0x0000000F");
    controller.route_target_usb2 = 0x0000000FU;
    controller.route_target_usb3 = 0x0000000FU;
    controller.route_usb2_field = 0x0000000FU;
    controller.route_wrote_usb2 = 1U;
    controller.route_wrote_usb3 = 1U;
    controller.route_restored = 0U;
    snprintf(controller.legacy, sizeof(controller.legacy), "%s",
             "cap=0x00000070 bios=0x00000001 result=TAKEN");
    controller.settle_ms = 500U;
    controller.settle_bound_ms = 500U;
    controller.settle_pending = 1U;
    controller.role_count = 2U;
    controller.roles[0].role = 0U; /* keyboard */
    controller.roles[1].role = 1U; /* mouse */
    controller.roles[1].seen = 1U;
    controller.roles[1].port = 2U;
    controller.roles[1].speed = 3U;
    controller.roles[1].vendor_id = 0x046DU;
    controller.roles[1].product_id = 0xC077U;
    controller.roles[1].interface_number = 0U;
    controller.roles[1].endpoint_address = 0x81U;
    controller.roles[1].reports = 3U;
    controller.roles[1].transfers = 5U;
    controller.roles[1].completions = 4U;
    controller.roles[1].endpoint_state = 2U; /* Halted */
    controller.roles[1].restarts = 1U;
    controller.roles[1].errors = 1U;
    return controller;
}

static void test_a_released_controller_survives_its_session(void) {
    struct xhci_evidence_controller source = controller(0U, 0x14U, 0x8086U,
                                                        0x1E31U);
    struct collected out;
    const char *line;

    xhci_evidence_reset();
    source.released = true;
    xhci_evidence_controller_keep(&source);
    /* What `xhci_session_release()` does to the session afterwards. */
    memset(&source, 0, sizeof(source));

    CHECK(xhci_evidence_controller_count() == 1U,
          "one controller is kept after its session was zeroed");
    out.count = 0U;
    xhci_evidence_report(collect, &out);
    line = find_line(&out, "[XHCI-CTRL] pci=00:14.0");
    CHECK(line != NULL, "the released controller has a record");
    CHECK(line != NULL && strstr(line, "state=RELEASED") != NULL,
          "the record says the session was released");
    CHECK(line != NULL && strstr(line, "status=ENUMERATION_FAILED") != NULL &&
              strstr(line, "detail=P1_ENABLE_SLOT_TIMEOUT") != NULL,
          "the start verdict survives the release");
    CHECK(line != NULL && strstr(line, "usbcmd=0x00000001") != NULL &&
              strstr(line, "usbsts=0x00000000") != NULL,
          "USBCMD and USBSTS survive the release");
    line = find_line(&out, "[XHCI-PORT] pci=00:14.0 port=1");
    CHECK(line != NULL && strstr(line, "was=0x000202A0") != NULL &&
              strstr(line, "now=0x000202A0") != NULL &&
              strstr(line, "pon=1") != NULL && strstr(line, "ccs=0") != NULL,
          "port 1 keeps both raw words and its actions");
    line = find_line(&out, "[XHCI-PORT] pci=00:14.0 port=2");
    CHECK(line != NULL && strstr(line, "was=0x000002A0") != NULL &&
              strstr(line, "now=0x000003A1") != NULL &&
              strstr(line, "kick=1") != NULL && strstr(line, "ccs=1") != NULL,
          "port 2 keeps the word before and after the link kick");
    line = find_line(&out, "[XHCI-ROUTE] pci=00:14.0 phase=BEFORE");
    CHECK(line != NULL && strstr(line, "read=OK") != NULL &&
              strstr(line, "xusb2pr=0x00000000") != NULL &&
              strstr(line, "usb2prm=0x0000000F") != NULL,
          "the firmware's routing registers are kept before the visit");
    line = find_line(&out, "[XHCI-ROUTE] pci=00:14.0 phase=AFTER");
    CHECK(line != NULL && strstr(line, "action=OK") != NULL &&
              strstr(line, "target_usb2=0x0000000F") != NULL &&
              strstr(line, "field=0x0000000F") != NULL &&
              strstr(line, "wrote=11") != NULL &&
              strstr(line, "detail=d26=OK d29=OK") != NULL,
          "the routing stage's verdict, participants and values are kept");
    line = find_line(&out, "[XHCI-LEGACY] pci=00:14.0");
    CHECK(line != NULL && strstr(line, "result=TAKEN") != NULL &&
              strstr(line, "cap=0x00000070") != NULL,
          "the BIOS/OS handoff result is kept");
    line = find_line(&out, "[XHCI-CTRL] pci=00:14.0");
    CHECK(line != NULL && strstr(line, "settle_ms=500") != NULL &&
              strstr(line, "settle_bound_ms=500") != NULL &&
              strstr(line, "settle_pending=1") != NULL,
          "the shared connect window is kept even when per-port waits were 0");
    line = find_line(&out, "[XHCI-PROTO] pci=00:14.0");
    CHECK(line != NULL && strstr(line, "major=3") != NULL &&
              strstr(line, "name=USB ") != NULL &&
              strstr(line, "off=1") != NULL && strstr(line, "count=1") != NULL,
          "the Supported Protocol group is kept");
    line = find_line(&out, "[XHCI-INPUT] pci=00:14.0 role=MOUSE");
    CHECK(line != NULL && strstr(line, "state=REFUSED") != NULL &&
              strstr(line, "vid=0x0000046D") != NULL &&
              strstr(line, "pid=0x0000C077") != NULL &&
              strstr(line, "ep=0x00000081") != NULL,
          "the mouse role's identity and endpoint are kept");
    CHECK(line != NULL && strstr(line, "reports=3") != NULL &&
              strstr(line, "submitted=5") != NULL &&
              strstr(line, "completions=4") != NULL &&
              strstr(line, "errors=1") != NULL,
          "the mouse role's transfer counters are kept");
    CHECK(find_line(&out, "role=KEYBOARD") == NULL,
          "a role that was never seen publishes no record");
}

/* A re-visit of the same function updates its record instead of appending. */
static void test_a_revisit_updates_the_same_controller(void) {
    struct xhci_evidence_controller source = controller(0U, 0x14U, 0x8086U,
                                                        0x1E31U);

    xhci_evidence_reset();
    source.released = true;
    xhci_evidence_controller_keep(&source);
    source.released = false;
    source.status[0] = '\0';
    snprintf(source.status, sizeof(source.status), "%s", "OK");
    source.roles[1].bound = 1U;
    xhci_evidence_controller_keep(&source);
    CHECK(xhci_evidence_controller_count() == 1U,
          "the same PCI function is one record, not two");
    CHECK(xhci_evidence_controller(0U) != NULL &&
              !xhci_evidence_controller(0U)->released &&
              xhci_evidence_controller(0U)->roles[1].bound == 1U,
          "the later keep is what the record holds");
}

static void test_two_controllers_keep_two_records(void) {
    struct xhci_evidence_controller first = controller(0U, 0x14U, 0x8086U,
                                                       0x1E31U);
    struct xhci_evidence_controller second = controller(0U, 0x1AU, 0x8086U,
                                                        0x8C31U);
    struct collected out;

    xhci_evidence_reset();
    first.released = true;
    second.released = false;
    second.max_ports = 4U;
    second.ports_seen = 4U;
    second.port_count = 4U;
    second.ports[2].was = 0x000002A0U;
    second.ports[2].now = 0x000002A0U;
    second.ports[3].was = 0x000002A0U;
    second.ports[3].now = 0x000003A1U;
    second.ports[3].connected = 1U;
    xhci_evidence_controller_keep(&first);
    xhci_evidence_controller_keep(&second);
    CHECK(xhci_evidence_controller_count() == 2U,
          "two controllers keep two records");
    out.count = 0U;
    xhci_evidence_report(collect, &out);
    CHECK(find_line(&out, "[XHCI-CTRL] pci=00:14.0") != NULL &&
              find_line(&out, "[XHCI-CTRL] pci=00:1A.0") != NULL,
          "both controllers have their own record");
    CHECK(find_line(&out, "[XHCI-PORT] pci=00:1A.0 port=4") != NULL,
          "the second controller's fourth port is kept");
}

static void test_the_pointer_verdict_covers_every_outcome(void) {
    struct xhci_evidence_mouse mouse;
    struct collected out;

    xhci_evidence_reset();
    memset(&mouse, 0, sizeof(mouse));
    mouse.valid = true;
    snprintf(mouse.verdict, sizeof(mouse.verdict), "%s", "NOT_DETECTED");
    snprintf(mouse.detail, sizeof(mouse.detail), "%s", "NO_DEVICE");
    xhci_evidence_mouse_keep(&mouse);
    out.count = 0U;
    xhci_evidence_report(collect, &out);
    CHECK(find_line(&out, "[XHCI-MOUSE] verdict=NOT_DETECTED") != NULL &&
              find_line(&out, "seen=0") != NULL,
          "a machine with no pointer publishes NOT_DETECTED");

    memset(&mouse, 0, sizeof(mouse));
    mouse.valid = true;
    mouse.seen = true;
    mouse.vendor_id = 0x046DU;
    mouse.product_id = 0xC077U;
    mouse.port = 2U;
    mouse.speed = 3U;
    snprintf(mouse.verdict, sizeof(mouse.verdict), "%s", "REFUSED");
    snprintf(mouse.detail, sizeof(mouse.detail), "%s", "P1_ENABLE_SLOT_TIMEOUT");
    snprintf(mouse.controller, sizeof(mouse.controller), "%s", "00:14.0");
    xhci_evidence_mouse_keep(&mouse);
    out.count = 0U;
    xhci_evidence_report(collect, &out);
    CHECK(find_line(&out, "[XHCI-MOUSE] verdict=REFUSED") != NULL &&
              find_line(&out, "vid=0x0000046D") != NULL &&
              find_line(&out, "pid=0x0000C077") != NULL &&
              find_line(&out, "port=2") != NULL,
          "a seen pointer keeps its identity and port");

    memset(&mouse, 0, sizeof(mouse));
    mouse.valid = true;
    mouse.seen = true;
    mouse.bound = true;
    mouse.reports = 12U;
    mouse.transfers = 15U;
    mouse.completions = 14U;
    mouse.wheels = 2U;
    mouse.endpoint_address = 0x81U;
    mouse.interface_number = 0U;
    snprintf(mouse.verdict, sizeof(mouse.verdict), "%s", "BOUND");
    snprintf(mouse.detail, sizeof(mouse.detail), "%s", "OK");
    snprintf(mouse.controller, sizeof(mouse.controller), "%s", "00:14.0");
    xhci_evidence_mouse_keep(&mouse);
    out.count = 0U;
    xhci_evidence_report(collect, &out);
    CHECK(find_line(&out, "[XHCI-MOUSE] verdict=BOUND") != NULL &&
              find_line(&out, "reports=12") != NULL &&
              find_line(&out, "submitted=15") != NULL &&
              find_line(&out, "completions=14") != NULL &&
              find_line(&out, "wheels=2") != NULL,
          "a bound pointer publishes the submitted/completed/decoded counts");
}

/*
 * ITEM-639: a physical diag must tell a three-byte boot report from a
 * report-protocol wheel without reading the source.  The pointer record names
 * the selected wire format, the descriptor's wheel byte and the last transfer's
 * actual length; a boot pointer publishes no layout instead of a zero offset.
 */
static void test_the_pointer_record_publishes_its_wire_format(void) {
    struct xhci_evidence_mouse mouse;
    struct collected out;
    const char *line;

    xhci_evidence_reset();
    memset(&mouse, 0, sizeof(mouse));
    mouse.valid = true;
    mouse.seen = true;
    mouse.bound = true;
    mouse.wheel_offset = USB_HID_REPORT_OFFSET_NONE;
    mouse.last_report_bytes = 3U;
    snprintf(mouse.verdict, sizeof(mouse.verdict), "%s", "BOUND");
    snprintf(mouse.detail, sizeof(mouse.detail), "%s", "OK");
    snprintf(mouse.controller, sizeof(mouse.controller), "%s", "00:14.0");
    xhci_evidence_mouse_keep(&mouse);
    out.count = 0U;
    xhci_evidence_report(collect, &out);
    line = find_line(&out, "[XHCI-MOUSE] ");
    CHECK(line != NULL && strstr(line, "mode=BOOT") != NULL &&
              strstr(line, "wheel_offset=none") != NULL &&
              strstr(line, "report_length=0") != NULL &&
              strstr(line, "last_bytes=3") != NULL,
          "a boot pointer publishes its three-byte format and no wheel field");
    CHECK(line != NULL && strstr(line, "last_bytes=3") != NULL &&
              strstr(line, "last_bytes=0") == NULL,
          "the boot record keeps the actual transfer length");

    memset(&mouse, 0, sizeof(mouse));
    mouse.valid = true;
    mouse.seen = true;
    mouse.bound = true;
    mouse.report_mode = true;
    mouse.wheel_offset = 3U;
    mouse.report_length = 4U;
    mouse.last_report_bytes = 4U;
    snprintf(mouse.verdict, sizeof(mouse.verdict), "%s", "BOUND");
    snprintf(mouse.detail, sizeof(mouse.detail), "%s", "OK");
    snprintf(mouse.controller, sizeof(mouse.controller), "%s", "00:14.0");
    xhci_evidence_mouse_keep(&mouse);
    out.count = 0U;
    xhci_evidence_report(collect, &out);
    line = find_line(&out, "[XHCI-MOUSE] ");
    CHECK(line != NULL && strstr(line, "mode=REPORT") != NULL &&
              strstr(line, "wheel_offset=3") != NULL &&
              strstr(line, "report_length=4") != NULL &&
              strstr(line, "last_bytes=4") != NULL,
          "a report-protocol pointer publishes its descriptor wheel and length");
}

/*
 * A connected port that failed before the HID descriptors is a detected
 * device: the store must say SEEN, name the controller and the port, and not
 * claim NOT_DETECTED.  This is the shape the QEMU proof meets (a mouse on a
 * port whose ENABLE_SLOT never completed).
 *
 * ITEM-578: the verdict keeps the pointer's own detail.  It used to copy the
 * controller's - which on the owner's laptop was the keyboard's "not a
 * keyboard" verdict, so the pointer record published a reason the pointer
 * never produced.
 */
static void test_a_connected_port_without_a_role_is_seen(void) {
    struct xhci_evidence_controller source = controller(0U, 0x14U, 0x8086U,
                                                        0x1E31U);
    struct xhci_evidence_mouse mouse;
    struct collected out;
    const char *line;

    xhci_evidence_reset();
    source.released = true;
    xhci_evidence_controller_keep(&source);
    memset(&mouse, 0, sizeof(mouse));
    mouse.valid = true;
    /* The driver's own verdict for a pointer it could not follow. */
    snprintf(mouse.verdict, sizeof(mouse.verdict), "%s", "NOT_DETECTED");
    snprintf(mouse.detail, sizeof(mouse.detail), "%s", "NO_DEVICE");
    xhci_evidence_mouse_keep(&mouse);
    out.count = 0U;
    xhci_evidence_report(collect, &out);
    line = find_line(&out, "[XHCI-MOUSE] ");
    CHECK(line != NULL && strstr(line, "verdict=SEEN") != NULL,
          "a connected port with no HID role is a detected device");
    CHECK(line != NULL && strstr(line, "seen=1") != NULL &&
              strstr(line, "bound=0") != NULL,
          "the detected device is not a bound one");
    CHECK(line != NULL && strstr(line, "port=2") != NULL &&
              strstr(line, "controller=00:14.0") != NULL,
          "the detected device's controller and port are named");
    CHECK(line != NULL && strstr(line, "detail=NO_DEVICE") != NULL,
          "the detected device keeps the pointer's own detail");
    CHECK(line != NULL && strstr(line, "P1_ENABLE_SLOT_TIMEOUT") == NULL,
          "the pointer verdict never borrows the controller's role reason");
    CHECK(line != NULL && strstr(line, "vid=0x00000000") != NULL,
          "an unidentified device has no invented identity");
}

/* The store never invents a device: ports that read no connect leave the
   driver's NOT_DETECTED answer standing. */
static void test_a_port_without_a_connect_stays_not_detected(void) {
    struct xhci_evidence_controller source = controller(0U, 0x14U, 0x8086U,
                                                        0x1E31U);
    struct xhci_evidence_mouse mouse;
    struct collected out;

    xhci_evidence_reset();
    source.released = true;
    source.ports[1].connected = 0U;
    xhci_evidence_controller_keep(&source);
    memset(&mouse, 0, sizeof(mouse));
    mouse.valid = true;
    snprintf(mouse.verdict, sizeof(mouse.verdict), "%s", "NOT_DETECTED");
    snprintf(mouse.detail, sizeof(mouse.detail), "%s", "NO_CONNECTED_DEVICE");
    xhci_evidence_mouse_keep(&mouse);
    out.count = 0U;
    xhci_evidence_report(collect, &out);
    CHECK(find_line(&out, "[XHCI-MOUSE] verdict=NOT_DETECTED") != NULL &&
              find_line(&out, "seen=0") != NULL,
          "no port with a connect leaves the pointer not detected");
}

/*
 * ITEM-580: the export's live refresh takes the working fields (registers,
 * role binding and counters) from the live copy and leaves the visit's
 * evidence untouched - including the first fatal snapshot and the original
 * refusal verdict, which a later successful binding must not erase.
 */
static void test_a_live_refresh_keeps_the_visit_evidence(void) {
    struct xhci_evidence_controller kept = controller(0U, 0x14U, 0x8086U,
                                                      0x1E31U);
    struct xhci_evidence_controller live;
    struct collected out;
    const char *line;

    xhci_evidence_reset();
    kept.released = true;
    kept.hcs_params2 = 0x84000054U;
    kept.slot_area_bytes = 1024U;
    kept.pool_bytes = 192512U;
    kept.dcbaa_physical = 0x831000ULL;
    kept.dcbaa0 = 0x83F000ULL;
    snprintf(kept.fatal, sizeof(kept.fatal), "%s",
             "cause=HSE usbcmd=0x00000000 usbsts=0x0000001D crcr=0x8 dcbaap=0x0");
    xhci_evidence_controller_keep(&kept);

    live = kept;
    live.released = false;
    live.usbcmd = 0x00000001U;
    live.usbsts = 0x00000018U;
    snprintf(live.status, sizeof(live.status), "%s", "OK");
    snprintf(live.detail, sizeof(live.detail), "%s", "OK");
    snprintf(live.fatal, sizeof(live.fatal), "%s", "cause=HCH usbcmd=0x0");
    live.route_after.xusb2pr = 0xDEADBEEFU;
    live.settle_ms = 7U;
    live.ports[1].now = 0xDEADBEEFU;
    live.roles[1].reports = 500U;
    live.roles[1].transfers = 800U;
    live.roles[1].completions = 700U;
    live.roles[1].endpoint_state = 1U;
    live.roles[1].restarts = 3U;
    xhci_evidence_controller_refresh(&live);

    out.count = 0U;
    xhci_evidence_report(collect, &out);
    line = find_line(&out, "[XHCI-CTRL] pci=00:14.0");
    CHECK(line != NULL && strstr(line, "status=ENUMERATION_FAILED") != NULL &&
              strstr(line, "detail=P1_ENABLE_SLOT_TIMEOUT") != NULL,
          "the live refresh keeps the visit's refusal verdict");
    CHECK(line != NULL && strstr(line, "usbcmd=0x00000001") != NULL &&
              strstr(line, "usbsts=0x00000018") != NULL,
          "the live refresh takes the running registers");
    CHECK(line != NULL && strstr(line, "state=RELEASED") != NULL,
          "the live refresh does not claim a released session is live");
    line = find_line(&out, "[XHCI-FATAL] pci=00:14.0");
    CHECK(line != NULL && strstr(line, "cause=HSE") != NULL &&
              strstr(line, "cause=HCH") == NULL,
          "the first fatal snapshot survives the live refresh");
    line = find_line(&out, "[XHCI-ROUTE] pci=00:14.0 phase=AFTER");
    CHECK(line != NULL && strstr(line, "target_usb2=0x0000000F") != NULL,
          "the routing evidence survives the live refresh");
    line = find_line(&out, "[XHCI-PORT] pci=00:14.0 port=2");
    CHECK(line != NULL && strstr(line, "now=0x000003A1") != NULL,
          "the port evidence survives the live refresh");
    line = find_line(&out, "[XHCI-INPUT] pci=00:14.0 role=MOUSE");
    CHECK(line != NULL && strstr(line, "reports=500") != NULL &&
              strstr(line, "submitted=800") != NULL &&
              strstr(line, "completions=700") != NULL &&
              strstr(line, "epstate=RUNNING") != NULL &&
              strstr(line, "restarts=3") != NULL,
          "the live role counters and endpoint state are the ones exported");
}

/*
 * ITEM-580: refreshing one controller must not rewrite another controller's
 * record - a released session's evidence is its own, and slot numbers are
 * per controller.
 */
static void test_a_live_refresh_leaves_other_controllers_alone(void) {
    struct xhci_evidence_controller released = controller(0U, 0x14U, 0x8086U,
                                                          0x1E31U);
    struct xhci_evidence_controller live = controller(0U, 0x10U, 0x1B36U,
                                                      0x000DU);
    struct collected out;
    const char *line;

    xhci_evidence_reset();
    released.released = true;
    xhci_evidence_controller_keep(&released);
    xhci_evidence_controller_keep(&live);

    {
        struct xhci_evidence_controller newer = live;

        newer.roles[1].reports = 900U;
        newer.roles[1].transfers = 1000U;
        newer.roles[1].completions = 950U;
        newer.usbcmd = 0x00000001U;
        xhci_evidence_controller_refresh(&newer);
    }

    out.count = 0U;
    xhci_evidence_report(collect, &out);
    line = find_line(&out, "[XHCI-INPUT] pci=00:14.0 role=MOUSE");
    CHECK(line != NULL && strstr(line, "reports=3") != NULL,
          "the released controller keeps its own role counters");
    line = find_line(&out, "[XHCI-INPUT] pci=00:10.0 role=MOUSE");
    CHECK(line != NULL && strstr(line, "reports=900") != NULL &&
              strstr(line, "completions=950") != NULL,
          "the live controller takes the refreshed counters");
}

int main(void) {
    test_a_released_controller_survives_its_session();
    test_a_revisit_updates_the_same_controller();
    test_two_controllers_keep_two_records();
    test_the_pointer_verdict_covers_every_outcome();
    test_the_pointer_record_publishes_its_wire_format();
    test_a_connected_port_without_a_role_is_seen();
    test_a_port_without_a_connect_stays_not_detected();
    test_a_live_refresh_keeps_the_visit_evidence();
    test_a_live_refresh_leaves_other_controllers_alone();
    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    printf("xHCI evidence host test: ALL OK\n");
    return 0;
}
