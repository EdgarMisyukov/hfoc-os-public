#ifndef HFOC_USB_HID_H
#define HFOC_USB_HID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "keyboard.h"

/*
 * ITEM-395: USB HID boot-protocol keyboard decoding.
 *
 * This module is deliberately transport-free: it parses the standard USB
 * descriptors a HID keyboard answers with and turns 8-byte boot-protocol
 * reports into the same struct keyboard_event values the PS/2 driver
 * (kernel/src/keyboard.c) produces, so both input paths meet the console at
 * exactly one interface. It owns no controller state, performs no I/O and
 * allocates nothing, which is what makes it provable on the host.
 *
 * References used for the tables and the report layout:
 *   * USB Device Class Definition for Human Interface Devices (HID) 1.11,
 *     Appendix B.1 "Boot Protocol" (8-byte keyboard report: 1 modifier byte,
 *     1 reserved byte, 6 usage-code slots) and B.1.2 "Keyboard Usage IDs".
 *   * USB Device Class Definition for HID 1.11, section 7.2 (SET_PROTOCOL /
 *     SET_IDLE class requests used by the caller).
 *   * USB 2.0 specification, section 9.6 (device/configuration/interface/
 *     endpoint descriptor layout).
 */

/* One interface can declare HID with a boot-protocol keyboard protocol. */
#define USB_HID_CLASS 0x03U
#define USB_HID_SUBCLASS_BOOT 0x01U
#define USB_HID_PROTOCOL_KEYBOARD 0x01U
/* ITEM-448: the other boot-protocol interface a HID device may declare. */
#define USB_HID_PROTOCOL_MOUSE 0x02U

/* Descriptor types (USB 2.0 table 9-5) and transfer direction bits. */
#define USB_DESCRIPTOR_DEVICE 0x01U
#define USB_DESCRIPTOR_CONFIGURATION 0x02U
#define USB_DESCRIPTOR_INTERFACE 0x04U
#define USB_DESCRIPTOR_ENDPOINT 0x05U
#define USB_DESCRIPTOR_HID 0x21U
#define USB_DESCRIPTOR_HID_REPORT 0x22U

#define USB_REQUEST_DIRECTION_DEVICE_TO_HOST 0x80U
#define USB_REQUEST_TYPE_CLASS 0x20U
#define USB_REQUEST_TYPE_STANDARD 0x00U

#define USB_REQUEST_GET_DESCRIPTOR 0x06U
#define USB_REQUEST_SET_CONFIGURATION 0x09U
#define USB_HID_REQUEST_SET_IDLE 0x0AU
#define USB_HID_REQUEST_SET_PROTOCOL 0x0BU

#define USB_HID_BOOT_REPORT_LENGTH 8U
#define USB_HID_BOOT_KEY_COUNT 6U

/* The largest configuration descriptor this decode accepts. The production
   QEMU keyboard advertises 34 bytes (9 + 9 + 9 + 7); the bound keeps a
   hostile or broken device from asking for an unbounded read. */
#define USB_HID_CONFIG_DESCRIPTOR_MAX 512U

/* Shortest configuration descriptor prefix that names wTotalLength. */
#define USB_CONFIG_DESCRIPTOR_PREFIX 9U

/*
 * ITEM-438: how much of a rejected device's interface list a record can carry.
 *
 * The 2026-09-17 hardware run refused a keyboard with
 * `detail=P5_CONFIG_PARSE_NO_BOOT_KEYBOARD` and nothing else: which device it
 * was, and which interfaces that device actually declared, could not be read
 * from the record at all.  The next run has to answer both from one line, so
 * the walk renders each interface as "<number>:<class>/<subclass>/<protocol>"
 * (hexadecimal, two digits each, no prefix) and the record publishes the list
 * next to the device's vid/pid.
 *
 * Four entries fit the bytes the [XHCI] record's budget has left once the
 * head, the detail and the controller facts are written (see
 * XHCI_RECORD_WORST_CASE_BYTES in kernel/src/xhci.c, which the host proof
 * pins); a device with more interfaces ends with '+' rather than silently
 * reading as a device that declares four.
 */
#define USB_HID_INTERFACE_SUMMARY_MAX 4U
#define USB_HID_INTERFACE_SUMMARY_BYTES 56U

/*
 * Render the interfaces of a configuration descriptor as
 * "0:03/01/01,1:03/01/02" - the number, class, subclass and protocol of every
 * interface descriptor the walk reaches, in descriptor order.  At most
 * USB_HID_INTERFACE_SUMMARY_MAX interfaces and at most `size` bytes are
 * written, always NUL-terminated; a list that had to stop early ends with '+'
 * and an unusable buffer becomes the empty string.  Returns the number of
 * characters written, excluding the terminator.
 */
size_t usb_hid_describe_interfaces(
    const uint8_t *descriptor,
    size_t length,
    char *out,
    size_t size);

/* Endpoint transfer type bits (bmAttributes bits 1:0) and direction bit. */
#define USB_ENDPOINT_TYPE_MASK 0x03U
#define USB_ENDPOINT_TYPE_INTERRUPT 0x03U
#define USB_ENDPOINT_DIRECTION_IN 0x80U
#define USB_ENDPOINT_NUMBER_MASK 0x0FU

/*
 * Why the descriptor decode did or did not select a boot keyboard. Only
 * USB_HID_STATUS_OK is usable; every other value is a reason an operator can
 * read in the boot record.
 */
enum usb_hid_status {
    USB_HID_STATUS_OK = 0,
    USB_HID_STATUS_NULL,
    /* Fewer bytes than the descriptor prefix, or a descriptor that walks off
       the end of the buffer. */
    USB_HID_STATUS_TRUNCATED,
    /* The buffer does not start with a configuration descriptor. */
    USB_HID_STATUS_NOT_CONFIGURATION,
    /* wTotalLength is zero, or larger than the caller supplied/supports. */
    USB_HID_STATUS_BAD_TOTAL_LENGTH,
    /* bNumInterfaces is zero: there is nothing to select. */
    USB_HID_STATUS_NO_INTERFACES,
    /* Descriptors with a zero bLength would never terminate; rejected. */
    USB_HID_STATUS_BAD_DESCRIPTOR_LENGTH,
    /* No interface declares a HID keyboard: not HID class 3, or a boot
       subclass with the boot mouse protocol, or nothing with an interrupt IN
       endpoint. */
    USB_HID_STATUS_NO_BOOT_KEYBOARD,
    /* ITEM-448: the same verdict for the other role - no interface of this
       configuration declares a HID mouse. */
    USB_HID_STATUS_NO_HID_MOUSE,
    /* The boot keyboard interface has no interrupt IN endpoint, so there is
       no report path. */
    USB_HID_STATUS_NO_INTERRUPT_IN,
    /* The selected endpoint is not an interrupt endpoint. */
    USB_HID_STATUS_BAD_ENDPOINT_TYPE,
    /* wMaxPacketSize is zero or above the boot-protocol ceiling. */
    USB_HID_STATUS_BAD_ENDPOINT_SIZE
};

/*
 * ITEM-438: the class request values that are *not* the interface descriptor's
 * bInterfaceProtocol field.  A HID device is asked to speak the boot protocol
 * with SET_PROTOCOL(0) and the report protocol with SET_PROTOCOL(1); the same
 * numbers in a descriptor mean "no protocol declared" and "keyboard", which is
 * why the two sets carry different names here.
 */
#define USB_HID_BOOT_PROTOCOL_VALUE 0x00U
#define USB_HID_REPORT_PROTOCOL_VALUE 0x01U

/*
 * ITEM-438: the idle duration the driver asks a keyboard for, in the HID
 * specification's 4 ms units - 250 is one second.  A keyboard that honours it
 * re-sends its report once per second even when nothing changed, which is what
 * makes "the device is bound and answering" a fact the driver can observe
 * instead of an assumption: with SET_IDLE(0) an untouched keyboard and a dead
 * endpoint are both simply silent.  The value is small enough that the report
 * traffic is one 8-byte transfer per second and large enough that it is not a
 * busy poll.
 */
#define USB_HID_IDLE_PERIOD 250U

/*
 * ITEM-438: what a report-protocol keyboard's reports look like, read from its
 * own HID report descriptor.
 *
 * A device that declares HID class 3 but no boot subclass never promises the
 * 8-byte boot report, so its report descriptor is the only place that says
 * whether it is a keyboard at all and where its modifier byte and key array
 * sit.  The 2026-09-17 hardware run refused such devices with
 * `NO_BOOT_KEYBOARD` without ever looking; a keyboard that declares protocol 0
 * (or no boot subclass) cannot type on this machine because of it.
 *
 * The offsets are absolute inside one transfer's bytes, i.e. they include the
 * report ID prefix when the descriptor declares report IDs; `report_length` is
 * the number of bytes a report of this layout must carry.
 */
#define USB_HID_REPORT_KEYS_MAX 6U
#define USB_HID_REPORT_LENGTH_MAX 64U
#define USB_HID_REPORT_OFFSET_NONE 0xFFU
#define USB_HID_REPORT_DESCRIPTOR_MAX 256U

struct usb_hid_report_layout {
    /* Report ID this layout belongs to; 0 when the descriptor declares none
       (then every report carries the layout). */
    uint8_t report_id;
    /* Bytes a report of this layout carries: the ID prefix, when declared,
       through the last key slot. */
    uint8_t report_length;
    /* Absolute offset of the 1-bit modifier field's byte, or
       USB_HID_REPORT_OFFSET_NONE when the descriptor declares none. */
    uint8_t modifier_offset;
    /* Absolute offset of the first key-usage byte. */
    uint8_t key_offset;
    /* Key-usage bytes the layout carries, 1..USB_HID_REPORT_KEYS_MAX. */
    uint8_t key_count;
};

/* The interface and endpoint a HID keyboard was found on. */
struct usb_hid_config {
    enum usb_hid_status status;
    /* bConfigurationValue of the configuration that carries the interface. */
    uint8_t config_value;
    uint8_t interface_number;
    /* bInterfaceProtocol of the selected interface (1 for the boot keyboard,
       0 for a device that declares no boot protocol). */
    uint8_t interface_protocol;
    /* bInterfaceSubClass of the selected interface. */
    uint8_t interface_subclass;
    /*
     * ITEM-438: true when the selected interface declares no boot keyboard
     * protocol, so the driver must read the interface's HID report descriptor
     * and confirm it is a keyboard before binding it.  A boot keyboard
     * interface (subclass 1, protocol 1) needs none.
     */
    bool report_descriptor_needed;
    /* Endpoint address as it appears in the descriptor, direction bit included
       (0x81 is the usual boot-keyboard address). */
    uint8_t endpoint_address;
    /* Data endpoint number (1..15), i.e. the address without its direction. */
    uint8_t endpoint_number;
    uint16_t endpoint_max_packet;
    /* bInterval as declared, for the xHCI endpoint context. */
    uint8_t endpoint_interval;
    /* wTotalLength that was walked. */
    uint16_t total_length;
};

/*
 * Decode a configuration descriptor blob (the bytes a GET_DESCRIPTOR
 * (CONFIGURATION) control transfer returned, exactly `length` of them) and
 * select the best HID keyboard interface with an interrupt IN endpoint.
 *
 * ITEM-438: the walk visits every interface and prefers, in order, a declared
 * boot keyboard (class 3/subclass 1/protocol 1), a boot-capable interface in
 * report protocol (class 3/subclass 1/protocol 0) and a HID interface with no
 * boot subclass (class 3/subclass 0).  The last two set
 * `report_descriptor_needed`, because only the report descriptor can say
 * whether the interface is a keyboard rather than a mouse; the caller reads it
 * and refuses the device when it is not.  The walk is bounded by `length` and
 * by USB_HID_CONFIG_DESCRIPTOR_MAX.
 */
struct usb_hid_config usb_hid_parse_config(
    const uint8_t *descriptor,
    size_t length);

/*
 * ITEM-448: which of the two HID devices this walk is looking for.  A keyboard
 * and a mouse can sit on one controller at the same time, and on a combined
 * receiver they are two interfaces of one device - so the same configuration
 * descriptor is walked once per role, and each walk selects its own interface
 * with its own preference order:
 *
 *   * keyboard: boot keyboard (class 3/subclass 1/protocol 1) first, then a
 *     boot subclass interface in report protocol, then a bare HID interface;
 *   * mouse: boot mouse (class 3/subclass 1/protocol 2) first, then the same
 *     two report-descriptor shapes.
 *
 * The statuses differ for the same reason: a configuration with no mouse is
 * USB_HID_STATUS_NO_HID_MOUSE, so the record for a mouse candidate says what
 * was missing instead of publishing a keyboard verdict.
 */
enum usb_hid_role {
    USB_HID_ROLE_KEYBOARD = 0,
    USB_HID_ROLE_MOUSE
};

struct usb_hid_config usb_hid_parse_config_role(
    const uint8_t *descriptor,
    size_t length,
    enum usb_hid_role role);

/*
 * ITEM-438: read a HID report descriptor (GET_DESCRIPTOR(REPORT), the bytes
 * the interface answered with) and fill the keyboard layout when it declares a
 * Generic Desktop keyboard application collection with a usable input report.
 *
 * The walk understands the short-item encoding of HID 1.11 section 6.2.2.7,
 * the global items that matter here (Usage Page, Report Size, Report Count,
 * Report ID) and the local ones (Usage, Usage Minimum, Usage Maximum).  It
 * returns true exactly when a keyboard collection declared both a 1-bit
 * modifier field and an 8-bit key array; `layout` is then complete and bounded
 * by USB_HID_REPORT_LENGTH_MAX.  Returns false, and leaves nothing usable
 * behind, for anything else - a mouse, a consumer-control collection, a
 * descriptor that walks off its own end, or a layout this decoder cannot
 * express.
 */
bool usb_hid_parse_report_descriptor(
    const uint8_t *descriptor,
    size_t length,
    struct usb_hid_report_layout *layout);

/*
 * ITEM-438: turn one report-protocol transfer into the 8-byte boot report the
 * tested decoder already understands: modifier byte first, a reserved byte,
 * then up to six key usages, zero-filled past the layout's own key count.
 *
 * Returns false for a transfer this layout does not describe: shorter than
 * USB_HID_REPORT_LENGTH_MAX bytes, or a report whose ID byte is not the
 * layout's.  The caller counts that as a refused report, exactly like a short
 * boot report, so a device that answers with a different report is a number in
 * the record rather than a character on the console.
 */
bool usb_hid_layout_build_boot_report(
    const struct usb_hid_report_layout *layout,
    const uint8_t *report,
    uint32_t length,
    uint8_t boot_report[USB_HID_BOOT_REPORT_LENGTH]);

/* Total length the device declares, or 0 when the prefix is unusable. Used to
   size the second GET_DESCRIPTOR read. */
uint16_t usb_hid_config_total_length(
    const uint8_t *descriptor,
    size_t length);

const char *usb_hid_status_string(enum usb_hid_status status);

/*
 * ITEM-436: how one completed interrupt IN transfer reads against the 8-byte
 * boot report the driver asked for.
 *
 * The controller reports a residual: the bytes of the transfer buffer it did
 * not fill. A boot keyboard answers with exactly eight bytes, so a residual of
 * zero is a whole report; every other residual is a transfer that carried
 * fewer bytes and that the decoder must therefore never see. Until this item
 * the driver dropped those transfers without counting them, so a device that
 * answered with a four-byte report read exactly like a device that answered
 * nothing at all - the shape the 2026-09-17 hardware run could not tell apart.
 */
enum usb_hid_report_length {
    /* The transfer filled the whole 8-byte report buffer. */
    USB_HID_REPORT_LENGTH_EXACT = 0,
    /* The transfer carried fewer than USB_HID_BOOT_REPORT_LENGTH bytes. */
    USB_HID_REPORT_LENGTH_SHORT,
    /* The residual is not a length this 8-byte buffer can produce. */
    USB_HID_REPORT_LENGTH_IMPOSSIBLE
};

/*
 * Classify one completed interrupt IN transfer against the 8-byte report
 * buffer, writing the number of report bytes the transfer carried to
 * `received_bytes` when that pointer is not NULL. A residual in
 * [0, USB_HID_BOOT_REPORT_LENGTH] is readable - residual 0 is the whole
 * report, anything larger is a short transfer - and anything outside that
 * range is a controller answer this driver cannot interpret, reported as
 * IMPOSSIBLE rather than underflowed into a huge length.
 */
enum usb_hid_report_length usb_hid_boot_report_classify(
    uint32_t transfer_length,
    uint32_t *received_bytes);

/*
 * ITEM-438: how long a bound keyboard may stay silent before its silence is a
 * finding instead of a state nobody can read.  The 2026-09-17 hardware run's
 * receiver was bound, its interrupt transfer was in flight, and not one report
 * arrived - which the record and the heartbeat printed exactly like a keyboard
 * that had simply not been touched yet.  Thirty seconds is the console's own
 * idle window (ITEM-430) and is measured on the loop's own clock.
 */
#define USB_HID_NO_REPORT_WINDOW_SECONDS 30U

/*
 * Is a bound keyboard's silence due to be published?  True exactly when a
 * keyboard is bound, no whole report has been decoded, and the loop has been
 * running for at least the window.  A machine with no keyboard is a different
 * record ([KBD ] state=UNAVAILABLE), so `bound` false is never due, and a
 * keyboard that has answered once is never due either however long it is then
 * left alone: the question is "did the device ever answer", not "is it quiet
 * now".
 */
bool usb_hid_no_report_due(
    bool bound,
    uint32_t decoded_reports,
    uint64_t elapsed_seconds,
    uint64_t window_seconds);

/*
 * Decoder state carried across reports: which keys are already down (so a
 * held key does not repeat) and the Caps Lock latch. Boot-protocol reports
 * carry no lock state, so the host owns it exactly as it does for the PS/2
 * path.
 */
struct usb_hid_state {
    bool initialized;
    bool caps_lock;
    uint8_t modifiers;
    uint8_t keys[USB_HID_BOOT_KEY_COUNT];
    /*
     * ITEM-459: the most recent newly pressed usage that produced no character,
     * and the modifiers that were down with it.  A key the machine cannot name
     * is a finding, not a silence: the caller takes this pair and publishes
     * `[KBD ] unmapped usage=0x.. modifier=0x..`, so the operator's dead key
     * names its own code in the log.  Zero means "nothing to report".
     */
    uint8_t unmapped_usage;
    uint8_t unmapped_modifiers;
};

/* Modifier bits of boot report byte 0. */
#define USB_HID_MODIFIER_LEFT_CTRL 0x01U
#define USB_HID_MODIFIER_LEFT_SHIFT 0x02U
#define USB_HID_MODIFIER_LEFT_ALT 0x04U
#define USB_HID_MODIFIER_LEFT_GUI 0x08U
#define USB_HID_MODIFIER_RIGHT_CTRL 0x10U
#define USB_HID_MODIFIER_RIGHT_SHIFT 0x20U
#define USB_HID_MODIFIER_RIGHT_ALT 0x40U
#define USB_HID_MODIFIER_RIGHT_GUI 0x80U

/* Usage IDs this decoder names, from HID 1.11 Appendix B.1.2. */
#define USB_HID_USAGE_ENTER 0x28U
#define USB_HID_USAGE_ESCAPE 0x29U
#define USB_HID_USAGE_BACKSPACE 0x2AU
#define USB_HID_USAGE_TAB 0x2BU
#define USB_HID_USAGE_SPACE 0x2CU
#define USB_HID_USAGE_CAPS_LOCK 0x39U
#define USB_HID_USAGE_HOME 0x4AU
#define USB_HID_USAGE_DELETE 0x4CU
#define USB_HID_USAGE_END 0x4DU
#define USB_HID_USAGE_RIGHT_ARROW 0x4FU
#define USB_HID_USAGE_LEFT_ARROW 0x50U
#define USB_HID_USAGE_DOWN_ARROW 0x51U
#define USB_HID_USAGE_UP_ARROW 0x52U

/*
 * ITEM-459: the range the character tables cover.  It ended at 0x38 while the
 * tables themselves were 0x65 entries, so every usage above the US letter rows
 * - the whole numeric keypad and 0x64 `Keyboard Non-US \ and |` - produced
 * nothing.  The table now reaches 0x67 (Keypad =), and a usage inside the range
 * that still has no character is reported by the decoder rather than dropped.
 */
#define USB_HID_USAGE_MIN 0x04U
#define USB_HID_USAGE_MAX 0x67U
#define USB_HID_USAGE_TABLE_SIZE 0x68U

/* The character a usage produces with no modifier, and with Shift. '\0'
   means "this usage produces no character". Caps Lock applies to the
   alphabetic entries exactly as it does in the PS/2 decode. */
char usb_hid_usage_character(uint8_t usage, bool shifted, bool caps_lock);

/*
 * ITEM-459: take the usage that produced no character, if one was seen since
 * the last call.  The pair is cleared by the call, so a held key is reported
 * once per press rather than once per report.
 */
bool usb_hid_take_unmapped(struct usb_hid_state *state, uint8_t *usage_out,
                           uint8_t *modifiers_out);

/*
 * Reset a decoder state to "nothing pressed". Called when a device is
 * attached and after a report that lost the device.
 */
void usb_hid_state_reset(struct usb_hid_state *state);

/*
 * Turn one 8-byte boot report into console events.
 *
 * Only newly pressed keys produce events: a key held down appears in every
 * report and must not repeat, exactly like the PS/2 path where only make
 * codes outside the typematic repeat reach the queue. Modifier changes alone
 * produce no event; Caps Lock toggles the latch and produces no character.
 *
 * At most USB_HID_BOOT_KEY_COUNT + 6 events are appended, and the function
 * never writes past `capacity`; the number of events appended is returned.
 */
size_t usb_hid_boot_report_events(
    struct usb_hid_state *state,
    const uint8_t report[USB_HID_BOOT_REPORT_LENGTH],
    struct keyboard_event *events,
    size_t capacity);

/*
 * ITEM-448: the boot mouse report, and the pointer half of the USB path.
 *
 * The appliance accepts a keyboard and a mouse at the same time now, and the
 * two devices have different reports.  This decoder owns the bytes a boot
 * mouse sends - one button byte and two signed relative deltas - and the
 * counters the input record publishes for it; where the pointer is and what a
 * click hits stays with the console, exactly as the character a key produces
 * stays with the key decoder above.
 *
 * The wire format is the HID boot mouse report and nothing else: one button
 * byte and two signed relative deltas.  A device may append the wheel byte the
 * boot mouse report is commonly extended with, so the driver asks for
 * USB_HID_BOOT_MOUSE_TRANSFER_LENGTH bytes and the decoder reads the report's
 * own three fields; a transfer shorter than them is counted as refused instead
 * of being padded, because the one thing a pointer must never do is move
 * somewhere the operator did not.
 */
#define USB_HID_BOOT_MOUSE_REPORT_LENGTH 3U
#define USB_HID_BOOT_MOUSE_TRANSFER_LENGTH 4U
#define USB_HID_MOUSE_BUTTON_LEFT 0x01U
#define USB_HID_MOUSE_BUTTON_RIGHT 0x02U
#define USB_HID_MOUSE_BUTTON_MIDDLE 0x04U

struct usb_hid_mouse_state {
    bool initialized;
    /* The button byte of the last accepted report, for the edge detection. */
    uint8_t buttons;
    uint32_t reports;
    uint32_t moves;
    uint32_t presses;
    uint32_t releases;
    /* ITEM-579: reports that carried a wheel movement. */
    uint32_t wheels;
    uint32_t refused;
};

struct usb_hid_mouse_event {
    int16_t dx;
    int16_t dy;
    /*
     * ITEM-579: the wheel delta of the report, positive away from the user.
     * The boot report carries it as the fourth byte common devices append
     * (the boot protocol itself defines three), and a report-protocol pointer
     * carries it in the field its own descriptor declared - so the pointer
     * event names the wheel instead of the driver decoding it and dropping it.
     */
    int16_t wheel;
    uint8_t buttons;
    /* The bits that went down and came up in this report. */
    uint8_t pressed;
    uint8_t released;
};

/*
 * ITEM-480 (round 18): one draw-worthy unit of the pointer's queue.
 *
 * A run of consecutive movements is summed into one batch, so the console
 * paints the cursor once for the whole run instead of once per report.  A
 * button edge breaks the run and is carried by a batch of its own (which may
 * also carry the movement decoded in the same report), so no press and no
 * release is merged away or reordered.  `events` is how many decoded reports
 * the batch consumed - one for a button edge, the run's length for a movement -
 * which is what lets the proof compare reports with drawings.
 */
struct usb_hid_mouse_batch {
    int32_t dx;
    int32_t dy;
    /* ITEM-579: the wheel movement of the run, carried by the same batch. */
    int32_t wheel;
    uint8_t pressed;
    uint8_t released;
    uint16_t events;
};

void usb_hid_mouse_state_reset(struct usb_hid_mouse_state *state);

/*
 * Decode one accepted boot mouse report.  `received_bytes` is the byte count
 * the interrupt transfer carried: the three boot fields, or a fourth wheel byte
 * (ITEM-579 decodes it as the wheel).  `report` is readable for
 * `received_bytes`; the function returns false - and counts the report as
 * refused - for anything shorter, and leaves `event_out` untouched then.
 */
bool usb_hid_boot_mouse_report(
    struct usb_hid_mouse_state *state,
    const uint8_t *report,
    uint32_t received_bytes,
    struct usb_hid_mouse_event *event_out);

uint32_t usb_hid_mouse_state_reports(
    const struct usb_hid_mouse_state *state);
uint32_t usb_hid_mouse_state_moves(
    const struct usb_hid_mouse_state *state);
uint32_t usb_hid_mouse_state_presses(
    const struct usb_hid_mouse_state *state);
uint32_t usb_hid_mouse_state_releases(
    const struct usb_hid_mouse_state *state);
uint32_t usb_hid_mouse_state_wheels(
    const struct usb_hid_mouse_state *state);
uint32_t usb_hid_mouse_state_refused(
    const struct usb_hid_mouse_state *state);

/*
 * ITEM-448: a mouse that does not speak the boot protocol, decoded from its own
 * report descriptor.
 *
 * A combined receiver declares "HID class 3" for both halves and spells out
 * which is which only in the report descriptors; a mouse that declares no boot
 * subclass has no 3-byte report at all, and reading its bytes as the boot
 * report would move the pointer by whatever happened to sit in the first three
 * bytes.  This walk answers the pointer's own question - where are the buttons,
 * the X delta and the Y delta in one report - and refuses a descriptor whose
 * fields this decoder cannot express (a 12-bit axis, a button field split
 * across bytes, a report with no X or no Y).
 *
 * The layout's offsets are absolute bytes inside one transfer, report ID
 * prefix included, exactly like the keyboard layout above.  ITEM-640: a
 * physical 04D9:A09F pointer declares signed relative 16-bit X and Y; the
 * layout carries each axis's field width so the decoder can read the field the
 * device actually sent instead of truncating it to its low byte.
 */
#define USB_HID_MOUSE_BUTTONS_MAX 8U

struct usb_hid_mouse_layout {
    /* Report ID this layout belongs to; 0 when the descriptor declares none. */
    uint8_t report_id;
    /* Bytes one report of this layout carries, ID prefix included. */
    uint8_t report_length;
    /* Byte holding the button bits, USB_HID_REPORT_OFFSET_NONE when the
       descriptor declares no buttons (a pointer may still move). */
    uint8_t buttons_offset;
    /* Which bits of that byte are buttons: 0x07 for three buttons. */
    uint8_t buttons_mask;
    uint8_t dx_offset;
    uint8_t dy_offset;
    /* Field width in bits: 8 or 16 for each axis. */
    uint8_t dx_size;
    uint8_t dy_size;
    /* ITEM-579: the byte holding the wheel, USB_HID_REPORT_OFFSET_NONE when the
       descriptor declares no wheel; ITEM-640 keeps it an 8-bit field. */
    uint8_t wheel_offset;
    uint8_t wheel_size;
};

bool usb_hid_parse_mouse_report_descriptor(
    const uint8_t *descriptor,
    size_t length,
    struct usb_hid_mouse_layout *layout);

/*
 * Decode one report-protocol transfer through `layout`.  The state carries the
 * counters and the previous button byte exactly as it does for the boot report,
 * so both paths publish the same numbers.  Returns false - with nothing written
 * to `event_out` - for a transfer shorter than the layout, a report whose ID
 * byte is not the layout's, or an unusable layout; the caller counts that as a
 * refused report, exactly as it does for the boot report.
 */
bool usb_hid_mouse_layout_decode(
    struct usb_hid_mouse_state *state,
    const struct usb_hid_mouse_layout *layout,
    const uint8_t *report,
    uint32_t length,
    struct usb_hid_mouse_event *event_out);

#endif
