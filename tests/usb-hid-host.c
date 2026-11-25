/*
 * ITEM-395 USB HID boot-protocol host proof.
 *
 * The console accepts input from two keyboards now: the PS/2 controller
 * (IRQ driven) and a USB HID boot-protocol keyboard behind an xHCI controller
 * (polled). The QEMU proof shows a typed command arriving through a real
 * emulated xHCI controller; it cannot say *why* a descriptor was rejected, and
 * it cannot enumerate the negative cases at all. This proof drives the
 * transport-free half of the USB HID driver - the configuration descriptor
 * walk and the 8-byte boot report decode - and asserts that
 *
 *   - a HID boot keyboard interface with an interrupt IN endpoint is selected
 *     from a real QEMU-shaped configuration descriptor, and the interface
 *     number, endpoint address, packet size and interval it reports are the
 *     ones the xHCI endpoint context is built from;
 *   - every way a descriptor can fail to describe a boot keyboard is
 *     classified and named (truncation, wrong type or total length, no
 *     interface, no boot keyboard, no interrupt IN endpoint, an endpoint of
 *     the wrong type, an unusable packet size, a zero-length descriptor);
 *   - the walk is bounded by wTotalLength and by the buffer the caller
 *     supplied, so trailing bytes and overlong declarations cannot make it
 *     read past either;
 *   - the console's whole command alphabet is producible: typing each
 *     documented console command through boot reports yields exactly that
 *     command, so the USB path can drive the console the PS/2 path drives;
 *   - Shift and Caps Lock decode the way the PS/2 tables decode them, and the
 *     Ctrl combination decodes to the US-ASCII control character;
 *   - a held key produces one event, not one per report, and a malformed
 *     report cannot produce two presses for one keystroke;
 *   - the decode never writes past the caller's buffer.
 */
#ifdef HFOC_USB_HID_HOST_TEST

#include "usb_hid.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } \
} while (0)

/* A configuration descriptor in the shape QEMU's usb-kbd answers with: one
   interface, HID class, boot subclass, keyboard protocol, one interrupt IN
   endpoint at address 0x81 with 8-byte packets. */
static const uint8_t keyboard_config[] = {
    0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x27, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0a
};

/* A high-speed interrupt IN endpoint of a boot keyboard declares the same
   8-byte report; this variant only differs in bInterval. */
static const uint8_t keyboard_config_high_speed[] = {
    0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x27, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x04
};

/* Two interfaces: a boot mouse first, then the boot keyboard. */
static const uint8_t mouse_then_keyboard[] = {
    0x09, 0x02, 0x3b, 0x00, 0x02, 0x01, 0x00, 0x80, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x02, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x27, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x04, 0x00, 0x0a,
    0x09, 0x04, 0x01, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x27, 0x00,
    0x07, 0x05, 0x82, 0x03, 0x08, 0x00, 0x04
};

/*
 * ITEM-406: the device the acceptance host actually has, byte for byte as its
 * own descriptor dump describes it - a Logitech K120 (046d:c31c, bcdUSB 1.10,
 * low speed, bDeviceClass 0) with one configuration and two interfaces:
 *
 *   IF0  class 3 / subclass 1 / protocol 1  (HID boot keyboard)
 *        EP 0x81 interrupt IN, wMaxPacketSize 8, bInterval 10
 *   IF1  class 3 / subclass 0 / protocol 0  (HID, no boot protocol)
 *        EP 0x82 interrupt IN, wMaxPacketSize 4, bInterval 255
 *
 * The keyboard interface is the one to select, and the 8-byte endpoint with its
 * 10 ms interval is what the xHCI endpoint context must carry. bmAttributes and
 * bMaxPower are the device's own; nothing here is guessed for the fields the
 * walk does not read.
 */
static const uint8_t k120_logitech_config[] = {
    0x09, 0x02, 0x3b, 0x00, 0x02, 0x01, 0x00, 0xa0, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3f, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0a,
    0x09, 0x04, 0x01, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x17, 0x00,
    0x07, 0x05, 0x82, 0x03, 0x04, 0x00, 0xff
};

/* The same device with its interfaces swapped: a HID interface without the boot
   subclass first, the boot keyboard second. A device must not be rejected
   because its first interface is not the keyboard. */
static const uint8_t k120_keyboard_second[] = {
    0x09, 0x02, 0x3b, 0x00, 0x02, 0x01, 0x00, 0xa0, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x17, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x04, 0x00, 0xff,
    0x09, 0x04, 0x01, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3f, 0x00,
    0x07, 0x05, 0x82, 0x03, 0x08, 0x00, 0x0a
};

/* A wireless receiver in the shape the acceptance machine's other controller
   carries: two HID interfaces, neither of them a boot keyboard (a HID interface
   without the boot subclass, then a boot mouse). The keyboard that pairs with
   it is not a USB device of this machine at all. */
static const uint8_t receiver_without_boot_keyboard[] = {
    0x09, 0x02, 0x3b, 0x00, 0x02, 0x01, 0x00, 0xa0, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x1f, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0a,
    0x09, 0x04, 0x01, 0x00, 0x01, 0x03, 0x01, 0x02, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x32, 0x00,
    0x07, 0x05, 0x82, 0x03, 0x04, 0x00, 0x0a
};

/* A device whose only interface is a boot mouse. */
static const uint8_t mouse_only[] = {
    0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0xa0, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x02, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x32, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x04, 0x00, 0x0a
};

/*
 * ITEM-438: the interface descriptors the 2026-09-17 hardware run refused, and
 * the report descriptors that decide them.
 *
 * The Logitech receiver (046d:c53f) is a composite HID device whose keyboard
 * half need not declare the boot keyboard protocol, and the wired K120
 * (046d:c31c) is the device the run's `P5_CONFIG_PARSE_NO_BOOT_KEYBOARD`
 * named.  Neither interface field alone says "keyboard": the interface may
 * declare protocol 0 (report protocol) or no boot subclass at all.  These
 * fixtures are the shapes the driver must now accept, with the report
 * descriptor as the discriminator, plus the mouse shapes it must still refuse.
 */

/* A keyboard interface in report protocol: class 3, subclass 1, protocol 0,
   one 8-byte interrupt IN endpoint. Its report descriptor decides it. */
static const uint8_t report_protocol_keyboard_config[] = {
    0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0xa0, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x00, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3f, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0a
};

/* The same device with no boot subclass at all (class 3, subclass 0,
   protocol 0) - the shape the run's wired keyboard was refused for. */
static const uint8_t no_boot_subclass_keyboard_config[] = {
    0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0xa0, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3f, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x0a
};

/* A boot mouse in a configuration of its own, class 3/subclass 1/protocol 2:
   never a candidate, whatever its report descriptor says. */
static const uint8_t boot_mouse_config[] = {
    0x09, 0x02, 0x22, 0x00, 0x01, 0x01, 0x00, 0xa0, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x01, 0x02, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x32, 0x00,
    0x07, 0x05, 0x81, 0x03, 0x04, 0x00, 0x0a
};

/*
 * The boot keyboard report descriptor, byte for byte the classic one (HID
 * 1.11 Appendix B.1): a Generic Desktop keyboard application collection,
 * eight 1-bit modifier usages 0xE0..0xE7, one reserved byte, then six 8-bit
 * key usages 0x00..0x65.  Its input layout is modifier byte 0, keys at byte 2,
 * eight bytes in total.
 */
static const uint8_t keyboard_report_descriptor[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xc0
};

/* A mouse report descriptor: buttons, X and Y. No keyboard collection. */
static const uint8_t mouse_report_descriptor[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01,
    0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
    0xc0, 0xc0
};

/*
 * ITEM-448: the pointer half of a combined receiver: the mouse collection sits
 * under report ID 2, next to a keyboard collection under its own ID, and the
 * report carries a wheel byte the pointer decoder must not read as a delta.
 */
static const uint8_t report_id_mouse_report_descriptor[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,
    0x85, 0x01,
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xc0,
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01,
    0x85, 0x02,
    0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0xc0, 0xc0
};

/* A pointer with two 12-bit axes: a field this decoder cannot express, so the
   walk must refuse the descriptor rather than read the wrong bytes. */
static const uint8_t wide_axis_mouse_report_descriptor[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01,
    0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x15, 0x81, 0x25, 0x7f, 0x75, 0x0c, 0x95, 0x02, 0x81, 0x06,
    0xc0, 0xc0
};

/*
 * ITEM-640: the exact 71-byte report descriptor of the physical 04D9:A09F
 * pointer on the Toshiba acceptance machine, read read-only from the same
 * device on Debian.  Buttons occupy byte 0, X and Y are signed little-endian
 * 16-bit relative fields at bytes 1 and 3, the wheel is signed 8-bit at byte 5,
 * and byte 6 is Consumer AC Pan, which this pointer decoder must ignore.
 * SHA-256 of the bytes: aafe4959849fa3b3102be488a80dbfd228d47d733e7050297dfc977a80e56daa
 */
static const uint8_t toshiba_16bit_mouse_report_descriptor[] = {
    0x05U, 0x01U, 0x09U, 0x02U, 0xA1U, 0x01U, 0x09U, 0x01U,
    0xA1U, 0x00U, 0x05U, 0x09U, 0x15U, 0x00U, 0x25U, 0x01U,
    0x19U, 0x01U, 0x29U, 0x05U, 0x75U, 0x01U, 0x95U, 0x05U,
    0x81U, 0x02U, 0x95U, 0x03U, 0x81U, 0x01U, 0x05U, 0x01U,
    0x16U, 0x01U, 0x80U, 0x26U, 0xFFU, 0x7FU, 0x09U, 0x30U,
    0x09U, 0x31U, 0x75U, 0x10U, 0x95U, 0x02U, 0x81U, 0x06U,
    0x15U, 0x81U, 0x25U, 0x7FU, 0x09U, 0x38U, 0x75U, 0x08U,
    0x95U, 0x01U, 0x81U, 0x06U, 0x05U, 0x0CU, 0x0AU, 0x38U,
    0x02U, 0x95U, 0x01U, 0x81U, 0x06U, 0xC0U, 0xC0U,
};

/*
 * A consumer-control report descriptor: a Generic Desktop consumer collection
 * with one 16-bit usage. It declares no keyboard application collection, so it
 * must not be bound as one - the receiver's third collection has this shape.
 */
static const uint8_t consumer_report_descriptor[] = {
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01,
    0x15, 0x00, 0x26, 0xff, 0x03, 0x19, 0x00, 0x2a, 0xff, 0x03,
    0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xc0
};

/* A keyboard report descriptor that declares a report ID: the modifier byte
   and key array shift one byte along, and a report carrying another ID is not
   this layout's. */
static const uint8_t report_id_keyboard_report_descriptor[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,
    0x85, 0x01,
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xc0
};

static void test_parse_selects_boot_keyboard(void) {
    const struct usb_hid_config config =
        usb_hid_parse_config(keyboard_config, sizeof(keyboard_config));

    CHECK(config.status == USB_HID_STATUS_OK,
          "boot keyboard configuration is accepted");
    CHECK(config.config_value == 1U, "configuration value is 1");
    CHECK(config.interface_number == 0U, "interface number is 0");
    CHECK(config.interface_protocol == USB_HID_PROTOCOL_KEYBOARD,
          "interface protocol is the boot keyboard protocol");
    CHECK(config.endpoint_address == 0x81U, "endpoint address is 0x81");
    CHECK(config.endpoint_number == 1U, "endpoint number is 1");
    CHECK(config.endpoint_max_packet == 8U, "endpoint packet size is 8");
    CHECK(config.endpoint_interval == 10U, "endpoint interval is 10");
    CHECK(config.total_length == 34U, "total length is 34");
    CHECK(usb_hid_config_total_length(keyboard_config,
                                      sizeof(keyboard_config)) == 34U,
          "total length is readable from the prefix alone");

    {
        const struct usb_hid_config high = usb_hid_parse_config(
            keyboard_config_high_speed, sizeof(keyboard_config_high_speed));

        CHECK(high.status == USB_HID_STATUS_OK,
              "high-speed boot keyboard configuration is accepted");
        CHECK(high.endpoint_interval == 4U,
              "high-speed interval is read from the descriptor");
    }
}

static void test_parse_walks_past_other_interfaces(void) {
    const struct usb_hid_config config =
        usb_hid_parse_config(mouse_then_keyboard,
                             sizeof(mouse_then_keyboard));

    CHECK(config.status == USB_HID_STATUS_OK,
          "the boot keyboard is found behind a boot mouse interface");
    CHECK(config.interface_number == 1U,
          "the keyboard interface number is selected");
    CHECK(config.endpoint_address == 0x82U,
          "the keyboard endpoint address is selected");
}

/*
 * ITEM-406: the acceptance host's own keyboard. Its descriptors decide which
 * interface is used, which endpoint the interrupt transfer goes to, how many
 * bytes a report is and how often the endpoint is polled; every one of those
 * numbers is asserted here because the hardware run that found no keyboard
 * could not show them.
 */
static void test_parse_acceptance_host_keyboard(void) {
    const struct usb_hid_config config =
        usb_hid_parse_config(k120_logitech_config,
                             sizeof(k120_logitech_config));

    CHECK(config.status == USB_HID_STATUS_OK,
          "the K120 configuration selects its boot keyboard interface");
    CHECK(config.total_length == 59U,
          "the K120 configuration is 59 bytes (two interfaces)");
    CHECK(config.config_value == 1U, "the K120 configuration value is 1");
    CHECK(config.interface_number == 0U,
          "the K120 keyboard is interface 0");
    CHECK(config.interface_protocol == USB_HID_PROTOCOL_KEYBOARD,
          "the selected interface is the boot keyboard protocol");
    CHECK(config.endpoint_address == 0x81U,
          "the K120 keyboard report endpoint is 0x81");
    CHECK(config.endpoint_number == 1U,
          "the K120 keyboard endpoint number is 1");
    CHECK(config.endpoint_max_packet == 8U,
          "the K120 keyboard endpoint carries 8 bytes, not 64");
    CHECK(config.endpoint_interval == 10U,
          "the K120 keyboard declares bInterval 10");
    CHECK(usb_hid_config_total_length(k120_logitech_config,
                                      sizeof(k120_logitech_config)) == 59U,
          "the K120 total length is readable from the prefix alone");

    /* The second interface is a HID interface without the boot subclass, with
       a 4-byte endpoint at bInterval 255; it must not be selected, and it must
       not make the device unusable either. */
    {
        const struct usb_hid_config second =
            usb_hid_parse_config(k120_keyboard_second,
                                 sizeof(k120_keyboard_second));

        CHECK(second.status == USB_HID_STATUS_OK,
              "a keyboard on the second interface is still selected");
        CHECK(second.interface_number == 1U,
              "the selected interface is the second one");
        CHECK(second.endpoint_address == 0x82U,
              "the endpoint of the second interface is the one selected");
        CHECK(second.endpoint_max_packet == 8U,
              "the selected endpoint keeps its own packet size");
    }
}

/*
 * ITEM-438: a refusal has to name the device and the interfaces it declares.
 *
 * The 2026-09-17 hardware run read `detail=P5_CONFIG_PARSE_NO_BOOT_KEYBOARD`
 * and nothing else - not the vid/pid, not whether the device declared a
 * report-protocol keyboard, a mouse or something the walk never reached - so
 * the next hardware run could not act on it.  This pins the rendering the
 * [XHCI] refusal record publishes: interface number, class, subclass and
 * protocol in descriptor order, hexadecimal and prefix-free, bounded by the
 * record's own budget and marked when it had to stop early.
 */
static void test_describe_interfaces(void) {
    char summary[USB_HID_INTERFACE_SUMMARY_BYTES];

    CHECK(usb_hid_describe_interfaces(k120_logitech_config,
                                      sizeof(k120_logitech_config), summary,
                                      sizeof(summary)) == 23U,
          "the K120 renders both interfaces");
    CHECK(strcmp(summary, "00:03/01/01,01:03/00/00") == 0,
          "the K120's boot keyboard and its second HID interface are readable");

    CHECK(usb_hid_describe_interfaces(receiver_without_boot_keyboard,
                                      sizeof(receiver_without_boot_keyboard),
                                      summary, sizeof(summary)) == 23U,
          "the receiver renders both of its interfaces");
    CHECK(strcmp(summary, "00:03/00/00,01:03/01/02") == 0,
          "a report-protocol HID interface and a boot mouse are distinguishable");

    CHECK(usb_hid_describe_interfaces(mouse_only, sizeof(mouse_only),
                                      summary, sizeof(summary)) == 11U,
          "a one-interface device renders one entry");
    CHECK(strcmp(summary, "00:03/01/02") == 0,
          "the boot mouse names its own protocol, not the keyboard's");

    CHECK(usb_hid_describe_interfaces(keyboard_config,
                                      sizeof(keyboard_config), summary,
                                      sizeof(summary)) == 11U,
          "the QEMU keyboard renders its one interface");
    CHECK(strcmp(summary, "00:03/01/01") == 0,
          "the QEMU keyboard is the boot keyboard protocol");

    /* The buffer is the record's budget, not the device's: too small is the
       empty string and a partial list ends with '+' instead of reading as a
       device that declares fewer interfaces. Two entries need 11 + 1 + 11
       characters and a terminator, so 23 bytes hold one and the marker. */
    CHECK(usb_hid_describe_interfaces(k120_logitech_config,
                                      sizeof(k120_logitech_config), summary,
                                      1U) == 0U,
          "a one-byte buffer carries nothing");
    CHECK(summary[0] == '\0', "and is still terminated");
    CHECK(usb_hid_describe_interfaces(k120_logitech_config,
                                      sizeof(k120_logitech_config), summary,
                                      23U) == 12U,
          "a buffer that fits one entry stops after it");
    CHECK(strcmp(summary, "00:03/01/01+") == 0,
          "and says that it stopped early");
    CHECK(usb_hid_describe_interfaces(k120_logitech_config,
                                      sizeof(k120_logitech_config), summary,
                                      24U) == 23U,
          "one more byte fits the second entry");
    CHECK(strcmp(summary, "00:03/01/01,01:03/00/00") == 0,
          "without a truncation marker");
    CHECK(usb_hid_describe_interfaces(mouse_then_keyboard,
                                      sizeof(mouse_then_keyboard), summary,
                                      sizeof(summary)) == 23U,
          "a mouse-then-keyboard configuration renders both");
    CHECK(strcmp(summary, "00:03/01/02,01:03/01/01") == 0,
          "in descriptor order");

    CHECK(usb_hid_describe_interfaces(NULL, 0U, summary, sizeof(summary)) == 0U,
          "a missing descriptor renders nothing");
    CHECK(summary[0] == '\0', "and leaves an empty string");
}

/*
 * ITEM-438: the two interface shapes the 2026-09-17 hardware run refused, and
 * the report descriptor that decides them.
 *
 * A keyboard that declares report protocol (bInterfaceProtocol 0) or no boot
 * subclass at all is not distinguishable from a mouse by its interface
 * descriptor; the configuration walk therefore selects it as a candidate that
 * needs its report descriptor read, and the report walk answers whether it is
 * a keyboard and where its modifier byte and key array are.
 */
static void test_parse_report_protocol_candidates(void) {
    const struct usb_hid_config report_protocol = usb_hid_parse_config(
        report_protocol_keyboard_config,
        sizeof(report_protocol_keyboard_config));
    const struct usb_hid_config no_subclass = usb_hid_parse_config(
        no_boot_subclass_keyboard_config,
        sizeof(no_boot_subclass_keyboard_config));

    CHECK(report_protocol.status == USB_HID_STATUS_OK,
          "a boot-subclass interface in report protocol is a candidate");
    CHECK(report_protocol.interface_subclass == USB_HID_SUBCLASS_BOOT,
          "its boot subclass is published");
    CHECK(report_protocol.interface_protocol == 0U,
          "its declared protocol is the report protocol");
    CHECK(report_protocol.report_descriptor_needed,
          "and its report descriptor has to confirm it is a keyboard");
    CHECK(report_protocol.endpoint_address == 0x81U,
          "its report endpoint is selected");
    CHECK(report_protocol.endpoint_max_packet == 8U,
          "and carries the descriptor's own packet size");

    CHECK(no_subclass.status == USB_HID_STATUS_OK,
          "a HID interface with no boot subclass is a candidate too");
    CHECK(no_subclass.interface_subclass == 0U,
          "its missing boot subclass is published");
    CHECK(no_subclass.interface_protocol == 0U,
          "its declared protocol is published");
    CHECK(no_subclass.report_descriptor_needed,
          "and its report descriptor has to confirm it is a keyboard");
    CHECK(no_subclass.endpoint_address == 0x81U,
          "the wire the 2026-09-17 run refused now has a candidate");

    /* The declared boot keyboard is still preferred over both, whatever the
       descriptor order: a device that promises boot reports needs no read. */
    {
        static const uint8_t hid_then_boot_keyboard[] = {
            0x09, 0x02, 0x3b, 0x00, 0x02, 0x01, 0x00, 0xa0, 0x32,
            0x09, 0x04, 0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
            0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x17, 0x00,
            0x07, 0x05, 0x81, 0x03, 0x04, 0x00, 0xff,
            0x09, 0x04, 0x01, 0x00, 0x01, 0x03, 0x01, 0x01, 0x00,
            0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x3f, 0x00,
            0x07, 0x05, 0x82, 0x03, 0x08, 0x00, 0x0a
        };
        const struct usb_hid_config best = usb_hid_parse_config(
            hid_then_boot_keyboard, sizeof(hid_then_boot_keyboard));

        CHECK(best.status == USB_HID_STATUS_OK,
              "a device with both shapes selects one");
        CHECK(best.interface_number == 1U,
              "the declared boot keyboard wins over the report-protocol HID");
        CHECK(best.report_descriptor_needed == false,
              "so no report descriptor read is needed");
    }
}

static void test_report_descriptor_walk(void) {
    struct usb_hid_report_layout layout;

    memset(&layout, 0, sizeof(layout));
    CHECK(usb_hid_parse_report_descriptor(
              keyboard_report_descriptor,
              sizeof(keyboard_report_descriptor), &layout),
          "the boot keyboard report descriptor is a keyboard");
    CHECK(layout.report_id == 0U, "it declares no report ID");
    CHECK(layout.report_length == 8U, "its reports are eight bytes");
    CHECK(layout.modifier_offset == 0U, "its modifier byte is byte 0");
    CHECK(layout.key_offset == 2U, "its key array starts at byte 2");
    CHECK(layout.key_count == 6U, "and carries six key usages");

    memset(&layout, 0, sizeof(layout));
    CHECK(usb_hid_parse_report_descriptor(
              report_id_keyboard_report_descriptor,
              sizeof(report_id_keyboard_report_descriptor), &layout),
          "a report-ID keyboard descriptor is a keyboard");
    CHECK(layout.report_id == 1U, "its report ID is 1");
    CHECK(layout.report_length == 9U,
          "its reports carry the ID byte and eight more");
    CHECK(layout.modifier_offset == 1U,
          "its modifier byte follows the ID byte");
    CHECK(layout.key_offset == 3U, "its key array starts at byte 3");
    CHECK(layout.key_count == 6U, "and still carries six keys");

    memset(&layout, 0, sizeof(layout));
    CHECK(usb_hid_parse_report_descriptor(mouse_report_descriptor,
                                          sizeof(mouse_report_descriptor),
                                          &layout) == false,
          "a mouse report descriptor is not a keyboard");
    CHECK(usb_hid_parse_report_descriptor(consumer_report_descriptor,
                                          sizeof(consumer_report_descriptor),
                                          &layout) == false,
          "a consumer-control collection is not a keyboard");
    CHECK(usb_hid_parse_report_descriptor(NULL, 0U, &layout) == false,
          "a missing report descriptor is not a keyboard");
    CHECK(usb_hid_parse_report_descriptor(keyboard_report_descriptor, 0U,
                                          &layout) == false,
          "an empty report descriptor is not a keyboard");
    CHECK(usb_hid_parse_report_descriptor(keyboard_report_descriptor,
                                          sizeof(keyboard_report_descriptor),
                                          NULL) == false,
          "a missing layout is refused, not written through");

    /* A descriptor whose key array is cut off mid-collection is refused: the
       walk never sees the End Collection, and a layout without keys is not a
       layout. */
    CHECK(usb_hid_parse_report_descriptor(
              keyboard_report_descriptor,
              sizeof(keyboard_report_descriptor) - 8U, &layout) == false,
          "a descriptor truncated before its key array is refused");
}

static void test_report_protocol_decode(void) {
    struct usb_hid_report_layout layout;
    uint8_t boot_report[USB_HID_BOOT_REPORT_LENGTH];
    /* A report-ID keyboard's report: ID 1, modifier 0x02 (left Shift), one
       reserved byte, then 'a' (usage 0x04) and 'b' (usage 0x05). */
    static const uint8_t id_report[] = {
        0x01, 0x02, 0x00, 0x04, 0x05, 0x00, 0x00, 0x00, 0x00
    };

    CHECK(usb_hid_parse_report_descriptor(
              report_id_keyboard_report_descriptor,
              sizeof(report_id_keyboard_report_descriptor), &layout),
          "the report-ID layout parses");
    CHECK(usb_hid_layout_build_boot_report(&layout, id_report,
                                           sizeof(id_report),
                                           boot_report),
          "a whole report of that layout is converted");
    CHECK(boot_report[0] == 0x02U, "the modifier byte is carried over");
    CHECK(boot_report[1] == 0x00U, "the reserved byte is zeroed");
    CHECK(boot_report[2] == 0x04U, "the first key usage is carried over");
    CHECK(boot_report[3] == 0x05U, "the second key usage is carried over");
    CHECK(boot_report[4] == 0x00U, "the unused key slots are zeroed");
    CHECK(usb_hid_layout_build_boot_report(
              &layout, id_report, sizeof(id_report) - 1U, boot_report) == false,
          "a report shorter than the layout is refused");
    {
        static const uint8_t other_id[] = {
            0x02, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00
        };

        CHECK(usb_hid_layout_build_boot_report(&layout, other_id,
                                               sizeof(other_id),
                                               boot_report) == false,
              "a report of another report ID is not this layout's");
    }

    /* The decoded report reaches the tested boot decoder unchanged. */
    CHECK(usb_hid_parse_report_descriptor(
              report_id_keyboard_report_descriptor,
              sizeof(report_id_keyboard_report_descriptor), &layout),
          "the layout is intact for the decode");
    CHECK(usb_hid_layout_build_boot_report(&layout, id_report,
                                           sizeof(id_report), boot_report),
          "the conversion succeeds");
    {
        struct usb_hid_state state;
        struct keyboard_event events[4];
        size_t count;

        usb_hid_state_reset(&state);
        count = usb_hid_boot_report_events(&state, boot_report, events, 4U);
        CHECK(count == 2U, "Shift+a and b produce two events");
        CHECK(events[0].character == 'A',
              "the report-protocol keyboard types the shifted letter");
        CHECK(events[1].character == 'B',
              "Shift applies to the second key as well");
    }
}

static void test_parse_rejects_devices_that_are_not_keyboards(void) {
    const struct usb_hid_config mouse =
        usb_hid_parse_config(boot_mouse_config, sizeof(boot_mouse_config));

    CHECK(mouse.status == USB_HID_STATUS_NO_BOOT_KEYBOARD,
          "a boot mouse is not a keyboard candidate");
    CHECK(mouse.endpoint_address == 0U,
          "a mouse gives no report endpoint to poll");
    CHECK(mouse.report_descriptor_needed == false,
          "and asks for no report descriptor read");
}

/*
 * ITEM-438: the receiver fixture is no longer a flat refusal, and that is the
 * point.  Its first interface is HID class 3 with no boot subclass - the same
 * shape the run's wired keyboard was refused for - so it is now a candidate
 * whose report descriptor decides; the fixture is the interface list, not the
 * report descriptor, so the walk selects it and the driver would read the
 * report descriptor and refuse it there if it is not a keyboard.  The boot
 * mouse (protocol 2) is still refused by the interface walk itself.
 */
static void test_parse_receiver_is_a_report_descriptor_candidate(void) {
    const struct usb_hid_config receiver = usb_hid_parse_config(
        receiver_without_boot_keyboard,
        sizeof(receiver_without_boot_keyboard));
    const struct usb_hid_config mouse =
        usb_hid_parse_config(mouse_only, sizeof(mouse_only));

    CHECK(receiver.status == USB_HID_STATUS_OK,
          "a receiver's HID interface is a candidate, not a flat refusal");
    CHECK(receiver.interface_number == 0U,
          "its first interface is the one selected");
    CHECK(receiver.interface_subclass == 0U,
          "which declares no boot subclass");
    CHECK(receiver.report_descriptor_needed,
          "so only its report descriptor can confirm a keyboard");
    CHECK(receiver.endpoint_address == 0x81U,
          "and its endpoint is the one the driver would poll");

    CHECK(mouse.status == USB_HID_STATUS_NO_BOOT_KEYBOARD,
          "a boot mouse is not a keyboard");
    CHECK(mouse.endpoint_address == 0U,
          "a mouse gives no report endpoint to poll");
}

static void test_parse_bounds(void) {
    uint8_t buffer[64];

    CHECK(usb_hid_parse_config(NULL, 0U).status == USB_HID_STATUS_NULL,
          "a missing descriptor is NULL, not a decode");

    memcpy(buffer, keyboard_config, sizeof(keyboard_config));
    CHECK(usb_hid_parse_config(buffer, 4U).status ==
              USB_HID_STATUS_TRUNCATED,
          "a buffer shorter than the configuration prefix is truncated");

    CHECK(usb_hid_parse_config(buffer, sizeof(keyboard_config)).status ==
              USB_HID_STATUS_OK,
          "the exact buffer length decodes");
    /* Trailing bytes past wTotalLength must be ignored, not walked. */
    memset(buffer + sizeof(keyboard_config), 0xFF, 8U);
    CHECK(usb_hid_parse_config(buffer, sizeof(keyboard_config) + 8U).status ==
              USB_HID_STATUS_OK,
          "bytes past wTotalLength are ignored");

    memcpy(buffer, keyboard_config, sizeof(keyboard_config));
    buffer[1] = 0x03U;
    CHECK(usb_hid_parse_config(buffer, sizeof(keyboard_config)).status ==
              USB_HID_STATUS_NOT_CONFIGURATION,
          "a non-configuration descriptor is rejected");

    memcpy(buffer, keyboard_config, sizeof(keyboard_config));
    buffer[2] = 0x00U;
    buffer[3] = 0x00U;
    CHECK(usb_hid_parse_config(buffer, sizeof(keyboard_config)).status ==
              USB_HID_STATUS_BAD_TOTAL_LENGTH,
          "a zero total length is rejected");

    memcpy(buffer, keyboard_config, sizeof(keyboard_config));
    buffer[2] = 0x00U;
    buffer[3] = 0x08U;
    CHECK(usb_hid_parse_config(buffer, sizeof(keyboard_config)).status ==
              USB_HID_STATUS_BAD_TOTAL_LENGTH,
          "a total length above the decoder bound is rejected");

    memcpy(buffer, keyboard_config, sizeof(keyboard_config));
    buffer[4] = 0x00U;
    CHECK(usb_hid_parse_config(buffer, sizeof(keyboard_config)).status ==
              USB_HID_STATUS_NO_INTERFACES,
          "a configuration without interfaces is rejected");

    /* wTotalLength claims more than the caller read. */
    memcpy(buffer, keyboard_config, sizeof(keyboard_config));
    buffer[2] = 0x40U;
    CHECK(usb_hid_parse_config(buffer, sizeof(keyboard_config)).status ==
              USB_HID_STATUS_TRUNCATED,
          "a total length beyond the supplied bytes is truncated");
}

static void test_parse_rejects_non_keyboards(void) {
    uint8_t buffer[64];

    memcpy(buffer, keyboard_config, sizeof(keyboard_config));
    buffer[14] = 0x08U; /* interface class: mass storage */
    CHECK(usb_hid_parse_config(buffer, sizeof(keyboard_config)).status ==
              USB_HID_STATUS_NO_BOOT_KEYBOARD,
          "a non-HID interface is not a keyboard");

    /* ITEM-438: a HID interface without the boot subclass is no longer a flat
       refusal - it is a candidate whose own report descriptor decides whether
       it is a keyboard.  The configuration walk says so; the report walk (and
       the driver) answers. */
    memcpy(buffer, keyboard_config, sizeof(keyboard_config));
    buffer[15] = 0x00U; /* HID subclass: no boot support */
    {
        const struct usb_hid_config hid_only =
            usb_hid_parse_config(buffer, sizeof(keyboard_config));

        CHECK(hid_only.status == USB_HID_STATUS_OK,
              "a HID interface without boot subclass is a candidate");
        CHECK(hid_only.report_descriptor_needed,
              "whose report descriptor has to confirm it is a keyboard");
    }

    memcpy(buffer, keyboard_config, sizeof(keyboard_config));
    buffer[16] = 0x02U; /* protocol: mouse */
    CHECK(usb_hid_parse_config(buffer, sizeof(keyboard_config)).status ==
              USB_HID_STATUS_NO_BOOT_KEYBOARD,
          "a boot mouse is not a boot keyboard");

    /* Remove the endpoint descriptor from the interface. */
    memcpy(buffer, keyboard_config, 27U);
    buffer[2] = 0x1bU;
    CHECK(usb_hid_parse_config(buffer, 27U).status ==
              USB_HID_STATUS_NO_INTERRUPT_IN,
          "a boot keyboard without an endpoint has no report path");

    /* Keep the endpoint but make it an interrupt OUT endpoint. */
    memcpy(buffer, keyboard_config, sizeof(keyboard_config));
    buffer[29] = 0x01U;
    CHECK(usb_hid_parse_config(buffer, sizeof(keyboard_config)).status ==
              USB_HID_STATUS_BAD_ENDPOINT_TYPE,
          "an interrupt OUT endpoint is not a report path");

    memcpy(buffer, keyboard_config, sizeof(keyboard_config));
    buffer[30] = 0x02U; /* bulk endpoint */
    CHECK(usb_hid_parse_config(buffer, sizeof(keyboard_config)).status ==
              USB_HID_STATUS_BAD_ENDPOINT_TYPE,
          "a bulk endpoint is not a report path");

    memcpy(buffer, keyboard_config, sizeof(keyboard_config));
    buffer[31] = 0x00U;
    buffer[32] = 0x00U;
    CHECK(usb_hid_parse_config(buffer, sizeof(keyboard_config)).status ==
              USB_HID_STATUS_BAD_ENDPOINT_SIZE,
          "a zero-length report endpoint is rejected");

    memcpy(buffer, keyboard_config, sizeof(keyboard_config));
    buffer[31] = 0x80U;
    buffer[32] = 0x00U;
    CHECK(usb_hid_parse_config(buffer, sizeof(keyboard_config)).status ==
              USB_HID_STATUS_BAD_ENDPOINT_SIZE,
          "an oversized report endpoint is rejected");

    memcpy(buffer, keyboard_config, sizeof(keyboard_config));
    buffer[9] = 0x00U; /* zero-length interface descriptor */
    CHECK(usb_hid_parse_config(buffer, sizeof(keyboard_config)).status ==
              USB_HID_STATUS_BAD_DESCRIPTOR_LENGTH,
          "a zero-length descriptor is rejected instead of looping");

    memcpy(buffer, keyboard_config, sizeof(keyboard_config));
    buffer[9] = 0x20U; /* interface descriptor longer than the remaining */
    CHECK(usb_hid_parse_config(buffer, sizeof(keyboard_config)).status ==
              USB_HID_STATUS_TRUNCATED,
          "a descriptor running past the buffer is truncated");
}

static void test_status_strings(void) {
    CHECK(strcmp(usb_hid_status_string(USB_HID_STATUS_OK), "OK") == 0,
          "OK is named");
    CHECK(strcmp(usb_hid_status_string(USB_HID_STATUS_NO_BOOT_KEYBOARD),
                 "NO_BOOT_KEYBOARD") == 0,
          "the no-boot-keyboard reason is named for the record");
    CHECK(strcmp(usb_hid_status_string(USB_HID_STATUS_NO_INTERRUPT_IN),
                 "NO_INTERRUPT_IN") == 0,
          "the no-report-path reason is named for the record");
    CHECK(strcmp(usb_hid_status_string((enum usb_hid_status)99), "UNKNOWN") ==
              0,
          "an unclassified value is reported as UNKNOWN");
}

/* One key the test can press: usage id, whether Shift is held, and the
   character or event it must produce. */
struct key_fixture {
    char character;
    uint8_t usage;
    bool shift;
};

static const struct key_fixture console_alphabet[] = {
    { 'a', 0x04U, false }, { 'b', 0x05U, false }, { 'c', 0x06U, false },
    { 'd', 0x07U, false }, { 'e', 0x08U, false }, { 'f', 0x09U, false },
    { 'g', 0x0aU, false }, { 'h', 0x0bU, false }, { 'i', 0x0cU, false },
    { 'j', 0x0dU, false }, { 'k', 0x0eU, false }, { 'l', 0x0fU, false },
    { 'm', 0x10U, false }, { 'n', 0x11U, false }, { 'o', 0x12U, false },
    { 'p', 0x13U, false }, { 'q', 0x14U, false }, { 'r', 0x15U, false },
    { 's', 0x16U, false }, { 't', 0x17U, false }, { 'u', 0x18U, false },
    { 'v', 0x19U, false }, { 'w', 0x1aU, false }, { 'x', 0x1bU, false },
    { 'y', 0x1cU, false }, { 'z', 0x1dU, false },
    { '1', 0x1eU, false }, { '2', 0x1fU, false }, { '3', 0x20U, false },
    { '4', 0x21U, false }, { '5', 0x22U, false }, { '6', 0x23U, false },
    { '7', 0x24U, false }, { '8', 0x25U, false }, { '9', 0x26U, false },
    { '0', 0x27U, false },
    { '-', 0x2dU, false }, { '=', 0x2eU, false }, { '[', 0x2fU, false },
    { ']', 0x30U, false }, { '\\', 0x31U, false }, { ';', 0x33U, false },
    { '\'', 0x34U, false }, { '`', 0x35U, false }, { ',', 0x36U, false },
    { '.', 0x37U, false }, { '/', 0x38U, false }, { ' ', 0x2cU, false },
    { '!', 0x1eU, true }, { '@', 0x1fU, true }, { '#', 0x20U, true },
    { '$', 0x21U, true }, { '%', 0x22U, true }, { '^', 0x23U, true },
    { '&', 0x24U, true }, { '*', 0x25U, true }, { '(', 0x26U, true },
    { ')', 0x27U, true }, { '_', 0x2dU, true }, { '+', 0x2eU, true },
    { '{', 0x2fU, true }, { '}', 0x30U, true }, { '|', 0x31U, true },
    { ':', 0x33U, true }, { '"', 0x34U, true }, { '~', 0x35U, true },
    { '<', 0x36U, true }, { '>', 0x37U, true }, { '?', 0x38U, true },
    { 'A', 0x04U, true }, { 'Z', 0x1dU, true }
};

#define ALPHABET_COUNT \
    (sizeof(console_alphabet) / sizeof(console_alphabet[0]))

static void report_press(uint8_t report[USB_HID_BOOT_REPORT_LENGTH],
                         uint8_t usage,
                         bool shift) {
    memset(report, 0, USB_HID_BOOT_REPORT_LENGTH);
    if (shift) {
        report[0] = USB_HID_MODIFIER_LEFT_SHIFT;
    }
    report[2] = usage;
}

static void report_release(uint8_t report[USB_HID_BOOT_REPORT_LENGTH]) {
    memset(report, 0, USB_HID_BOOT_REPORT_LENGTH);
}

static void test_mapping_table(void) {
    size_t index;

    for (index = 0U; index < ALPHABET_COUNT; ++index) {
        const char expected = console_alphabet[index].character;
        const char value = usb_hid_usage_character(
            console_alphabet[index].usage, console_alphabet[index].shift,
            false);

        if (value != expected) {
            printf("FAIL: usage 0x%02x shift=%d decoded 0x%02x, expected "
                   "0x%02x\n",
                   console_alphabet[index].usage,
                   console_alphabet[index].shift ? 1 : 0,
                   (unsigned char)value, (unsigned char)expected);
            ++failures;
        }
    }
    CHECK(usb_hid_usage_character(0x00U, false, false) == '\0',
          "usage 0 produces no character");
    CHECK(usb_hid_usage_character(0x39U, false, false) == '\0',
          "Caps Lock produces no character of its own");
    /*
     * ITEM-459: 0x64 was the "out-of-table usage" this check used to name, and
     * it is exactly the key the 2026-09-19 hardware run found dead - so the
     * expectation flips consciously: the non-US \ and | key produces its two
     * characters now, and an out-of-table usage (0x68, one past the table) is
     * what still produces nothing.
     */
    CHECK(usb_hid_usage_character(0x64U, false, false) == '\\',
          "the non-US backslash key produces a backslash");
    CHECK(usb_hid_usage_character(0x64U, true, false) == '|',
          "and a pipe with Shift, which is the key the owner pressed");
    CHECK(usb_hid_usage_character(0x68U, false, false) == '\0',
          "an out-of-table usage still produces no character");
}

/* Type one console command through 8-byte boot reports and return how the
   console would see it. */
static size_t type_command(const char *command, char *out, size_t capacity) {
    struct usb_hid_state state;
    uint8_t report[USB_HID_BOOT_REPORT_LENGTH];
    size_t length = 0U;

    usb_hid_state_reset(&state);
    while (*command != '\0' && length + 1U < capacity) {
        struct keyboard_event events[USB_HID_BOOT_KEY_COUNT + 6U];
        size_t index;
        size_t count = 0U;
        size_t found = 0U;

        for (index = 0U; index < ALPHABET_COUNT; ++index) {
            if (console_alphabet[index].character == *command &&
                !console_alphabet[index].shift) {
                report_press(report, console_alphabet[index].usage, false);
                found = 1U;
                break;
            }
        }
        if (found == 0U) {
            return length;
        }
        count = usb_hid_boot_report_events(&state, report, events,
                                           sizeof(events) / sizeof(events[0]));
        CHECK(count == 1U, "one pressed key produces one event");
        if (count != 1U) {
            return length;
        }
        CHECK(events[0].type == KEYBOARD_EVENT_CHARACTER,
              "a printable key produces a character event");
        out[length++] = events[0].character;
        report_release(report);
        (void)usb_hid_boot_report_events(&state, report, events,
                                         sizeof(events) / sizeof(events[0]));
        ++command;
    }
    out[length] = '\0';
    return length;
}

static void test_console_commands_are_typable(void) {
    static const char *const commands[] = {
        "help", "clear", "version", "uptime", "ticks", "irq", "kbd", "mem",
        "pmm", "heap", "about", "appliance", "appliance-boot", "panic",
        "divzero", "pagefault", "doublefault", "vmmmap"
    };
    char typed[64];

    for (size_t index = 0U;
         index < sizeof(commands) / sizeof(commands[0]); ++index) {
        const size_t length = type_command(commands[index], typed,
                                           sizeof(typed));

        if (length != strlen(commands[index]) ||
            strcmp(typed, commands[index]) != 0) {
            printf("FAIL: command %s typed through boot reports as \"%s\"\n",
                   commands[index], typed);
            ++failures;
        }
    }
}

static void test_report_events(void) {
    struct usb_hid_state state;
    uint8_t report[USB_HID_BOOT_REPORT_LENGTH];
    struct keyboard_event events[USB_HID_BOOT_KEY_COUNT + 6U];
    size_t count;

    usb_hid_state_reset(&state);

    /* Enter, Backspace and the line-editing keys keep the console contract
       the PS/2 path already provides. */
    report_press(report, USB_HID_USAGE_ENTER, false);
    count = usb_hid_boot_report_events(&state, report, events, 8U);
    CHECK(count == 1U && events[0].type == KEYBOARD_EVENT_ENTER,
          "Enter maps to the console's enter event");
    report_release(report);
    (void)usb_hid_boot_report_events(&state, report, events, 8U);

    report_press(report, USB_HID_USAGE_BACKSPACE, false);
    count = usb_hid_boot_report_events(&state, report, events, 8U);
    CHECK(count == 1U && events[0].type == KEYBOARD_EVENT_BACKSPACE,
          "Backspace maps to the console's backspace event");
    report_release(report);
    (void)usb_hid_boot_report_events(&state, report, events, 8U);

    {
        const struct {
            uint8_t usage;
            enum keyboard_event_type type;
        } named[] = {
            { USB_HID_USAGE_LEFT_ARROW, KEYBOARD_EVENT_LEFT },
            { USB_HID_USAGE_RIGHT_ARROW, KEYBOARD_EVENT_RIGHT },
            { USB_HID_USAGE_HOME, KEYBOARD_EVENT_HOME },
            { USB_HID_USAGE_END, KEYBOARD_EVENT_END },
            { USB_HID_USAGE_UP_ARROW, KEYBOARD_EVENT_UP },
            { USB_HID_USAGE_DOWN_ARROW, KEYBOARD_EVENT_DOWN },
            { USB_HID_USAGE_DELETE, KEYBOARD_EVENT_DELETE }
        };

        for (size_t index = 0U;
             index < sizeof(named) / sizeof(named[0]); ++index) {
            report_press(report, named[index].usage, false);
            count = usb_hid_boot_report_events(&state, report, events, 8U);
            CHECK(count == 1U && events[0].type == named[index].type,
                  "a line-editing usage maps to its console event");
            report_release(report);
            (void)usb_hid_boot_report_events(&state, report, events, 8U);
        }
    }

    /* Escape has no console meaning, exactly as in the PS/2 decode. */
    report_press(report, USB_HID_USAGE_ESCAPE, false);
    count = usb_hid_boot_report_events(&state, report, events, 8U);
    CHECK(count == 0U, "Escape produces no console event");
    report_release(report);
    (void)usb_hid_boot_report_events(&state, report, events, 8U);

    /* Tab is decoded as a character, as the PS/2 table decodes it. */
    report_press(report, USB_HID_USAGE_TAB, false);
    count = usb_hid_boot_report_events(&state, report, events, 8U);
    CHECK(count == 1U && events[0].type == KEYBOARD_EVENT_CHARACTER &&
              events[0].character == '\t',
          "Tab decodes to a tab character");
    report_release(report);
    (void)usb_hid_boot_report_events(&state, report, events, 8U);
}

static void test_hold_repeat_and_rollover(void) {
    struct usb_hid_state state;
    uint8_t report[USB_HID_BOOT_REPORT_LENGTH];
    struct keyboard_event events[USB_HID_BOOT_KEY_COUNT + 6U];
    size_t count;

    usb_hid_state_reset(&state);
    report_press(report, 0x04U, false);
    count = usb_hid_boot_report_events(&state, report, events, 8U);
    CHECK(count == 1U, "a fresh key press produces one event");
    /* The boot protocol repeats a held key in every report. */
    count = usb_hid_boot_report_events(&state, report, events, 8U);
    CHECK(count == 0U, "a held key does not repeat");
    report_release(report);
    (void)usb_hid_boot_report_events(&state, report, events, 8U);
    report_press(report, 0x04U, false);
    count = usb_hid_boot_report_events(&state, report, events, 8U);
    CHECK(count == 1U, "the same key pressed again after release repeats");

    /* Six-key rollover keeps report order. */
    report_release(report);
    (void)usb_hid_boot_report_events(&state, report, events, 8U);
    memset(report, 0, sizeof(report));
    report[2] = 0x04U;
    report[3] = 0x05U;
    report[4] = 0x06U;
    report[5] = 0x07U;
    report[6] = 0x08U;
    report[7] = 0x09U;
    count = usb_hid_boot_report_events(&state, report, events, 8U);
    CHECK(count == 6U, "six new keys produce six events");
    CHECK(events[0].character == 'a' && events[5].character == 'f',
          "rollover events keep report order");

    /* A malformed report that repeats a usage is one keystroke. */
    report_release(report);
    (void)usb_hid_boot_report_events(&state, report, events, 8U);
    memset(report, 0, sizeof(report));
    report[2] = 0x04U;
    report[3] = 0x04U;
    count = usb_hid_boot_report_events(&state, report, events, 8U);
    CHECK(count == 1U, "a duplicated usage in one report is one press");

    /* The rollover-error usage is not a key. */
    report_release(report);
    (void)usb_hid_boot_report_events(&state, report, events, 8U);
    memset(report, 0, sizeof(report));
    report[2] = 0x01U;
    count = usb_hid_boot_report_events(&state, report, events, 8U);
    CHECK(count == 0U, "the rollover-error usage produces no event");
}

static void test_modifiers(void) {
    struct usb_hid_state state;
    uint8_t report[USB_HID_BOOT_REPORT_LENGTH];
    struct keyboard_event events[USB_HID_BOOT_KEY_COUNT + 6U];
    size_t count;

    /* Shift produces the same uppercase letter the PS/2 table produces. */
    usb_hid_state_reset(&state);
    report_press(report, 0x04U, true);
    count = usb_hid_boot_report_events(&state, report, events, 8U);
    CHECK(count == 1U && events[0].character == 'A',
          "Shift+letter is uppercase");
    report_release(report);
    (void)usb_hid_boot_report_events(&state, report, events, 8U);

    /* Right Shift counts as Shift. */
    memset(report, 0, sizeof(report));
    report[0] = USB_HID_MODIFIER_RIGHT_SHIFT;
    report[2] = 0x05U;
    count = usb_hid_boot_report_events(&state, report, events, 8U);
    CHECK(count == 1U && events[0].character == 'B',
          "right Shift is a Shift");
    report_release(report);
    (void)usb_hid_boot_report_events(&state, report, events, 8U);

    /* Caps Lock latches case and produces no character, like the PS/2 path. */
    usb_hid_state_reset(&state);
    report_press(report, USB_HID_USAGE_CAPS_LOCK, false);
    count = usb_hid_boot_report_events(&state, report, events, 8U);
    CHECK(count == 0U, "Caps Lock produces no character");
    report_release(report);
    (void)usb_hid_boot_report_events(&state, report, events, 8U);
    report_press(report, 0x04U, false);
    count = usb_hid_boot_report_events(&state, report, events, 8U);
    CHECK(count == 1U && events[0].character == 'A',
          "Caps Lock uppercases a letter");
    report_release(report);
    (void)usb_hid_boot_report_events(&state, report, events, 8U);
    report_press(report, 0x04U, true);
    count = usb_hid_boot_report_events(&state, report, events, 8U);
    CHECK(count == 1U && events[0].character == 'a',
          "Caps Lock inverts Shift, as the PS/2 decode does");

    /* Ctrl decodes to the US-ASCII control character. */
    usb_hid_state_reset(&state);
    memset(report, 0, sizeof(report));
    report[0] = USB_HID_MODIFIER_LEFT_CTRL;
    report[2] = 0x06U; /* c */
    count = usb_hid_boot_report_events(&state, report, events, 8U);
    CHECK(count == 1U && events[0].character == 0x03,
          "Ctrl+C decodes to the US-ASCII control character");
}

/*
 * ITEM-436: one completed interrupt IN transfer is classified against the
 * eight-byte boot report before the decoder sees it.
 *
 * The 2026-09-17 hardware run published a bound keyboard and no evidence that
 * any report ever arrived: the driver decoded a transfer only when it carried
 * exactly eight bytes and dropped every other length without counting it. The
 * classification is the seam that makes the difference publishable, and it is
 * transport-free, so the host owns it: residual 0 is the whole report, a
 * residual inside the buffer is a short transfer whose own length is reported,
 * and a residual outside the buffer is IMPOSSIBLE instead of underflowing into
 * a huge "received" count.
 */
static void test_report_length_classification(void) {
    uint32_t received;

    received = 0xFFFFFFFFU;
    CHECK(usb_hid_boot_report_classify(0U, &received) ==
              USB_HID_REPORT_LENGTH_EXACT,
          "a transfer that left no byte unfilled is a whole boot report");
    CHECK(received == USB_HID_BOOT_REPORT_LENGTH,
          "a whole report carries all eight bytes");

    received = 0xFFFFFFFFU;
    CHECK(usb_hid_boot_report_classify(4U, &received) ==
              USB_HID_REPORT_LENGTH_SHORT,
          "a transfer that left four bytes unfilled is short");
    CHECK(received == 4U, "a short transfer reports its own length");

    received = 0xFFFFFFFFU;
    CHECK(usb_hid_boot_report_classify(USB_HID_BOOT_REPORT_LENGTH, &received) ==
              USB_HID_REPORT_LENGTH_SHORT,
          "a transfer that carried nothing is short, not a report");
    CHECK(received == 0U, "an empty transfer carried no report byte");

    received = 0xFFFFFFFFU;
    CHECK(usb_hid_boot_report_classify(USB_HID_BOOT_REPORT_LENGTH + 1U,
                                       &received) ==
              USB_HID_REPORT_LENGTH_IMPOSSIBLE,
          "a residual past the buffer is not a length this driver can read");
    CHECK(received == 0U,
          "an impossible residual reports no bytes rather than underflowing");

    CHECK(usb_hid_boot_report_classify(0xFFFFFFFFU, NULL) ==
              USB_HID_REPORT_LENGTH_IMPOSSIBLE,
          "the classifier reads a residual the controller cannot have sent "
          "without a destination pointer");
    CHECK(usb_hid_boot_report_classify(0U, NULL) ==
              USB_HID_REPORT_LENGTH_EXACT,
          "the classifier accepts a null destination pointer");
}

static void test_event_capacity_is_respected(void) {
    struct usb_hid_state state;
    uint8_t report[USB_HID_BOOT_REPORT_LENGTH];
    struct {
        struct keyboard_event events[2];
        uint32_t canary;
    } bounded;
    size_t count;

    usb_hid_state_reset(&state);
    memset(report, 0, sizeof(report));
    report[2] = 0x04U;
    report[3] = 0x05U;
    report[4] = 0x06U;
    bounded.canary = 0xA5A5A5A5U;
    count = usb_hid_boot_report_events(&state, report, bounded.events, 2U);
    CHECK(count == 3U, "the total event count is reported");
    CHECK(bounded.canary == 0xA5A5A5A5U,
          "the decode never writes past the caller's capacity");

    CHECK(usb_hid_boot_report_events(NULL, report, bounded.events, 2U) == 0U,
          "a missing state is refused");
    CHECK(usb_hid_boot_report_events(&state, NULL, bounded.events, 2U) == 0U,
          "a missing report is refused");
    usb_hid_state_reset(NULL);
}

/*
 * ITEM-448: the boot mouse report decoder.  The pointer and the hit test live
 * in the console; this half owns the bytes a mouse sends and nothing else.
 */
static void test_mouse_report_decode(void) {
    struct usb_hid_mouse_state state;
    struct usb_hid_mouse_event event;

    usb_hid_mouse_state_reset(&state);
    memset(&event, 0, sizeof(event));

    /* The QEMU usb-mouse sends three-byte boot reports. */
    CHECK(usb_hid_boot_mouse_report(
              &state, (const uint8_t[]){0x00U, 0x05U, 0xFBU},
              USB_HID_BOOT_MOUSE_REPORT_LENGTH, &event) &&
              event.dx == 5 && event.dy == -5 &&
              event.buttons == 0U &&
              event.pressed == 0U && event.released == 0U,
          "a boot mouse report decodes its signed deltas");
    CHECK(state.reports == 1U && state.moves == 1U &&
              state.presses == 0U && state.refused == 0U,
          "a movement is counted as one report and one move");

    CHECK(usb_hid_boot_mouse_report(
              &state, (const uint8_t[]){0x01U, 0x00U, 0x00U},
              USB_HID_BOOT_MOUSE_REPORT_LENGTH, &event) &&
              event.buttons == USB_HID_MOUSE_BUTTON_LEFT &&
              event.pressed == USB_HID_MOUSE_BUTTON_LEFT &&
              event.released == 0U,
          "a left button going down is a press edge");
    CHECK(state.presses == 1U && state.releases == 0U,
          "the press edge is counted");

    CHECK(usb_hid_boot_mouse_report(
              &state, (const uint8_t[]){0x01U, 0x00U, 0x00U},
              USB_HID_BOOT_MOUSE_REPORT_LENGTH, &event) &&
              event.pressed == 0U && event.released == 0U,
          "a held button is not a second press");

    CHECK(usb_hid_boot_mouse_report(
              &state, (const uint8_t[]){0x00U, 0x00U, 0x00U},
              USB_HID_BOOT_MOUSE_REPORT_LENGTH, &event) &&
              event.released == USB_HID_MOUSE_BUTTON_LEFT,
          "a button coming up is a release edge");
    CHECK(state.releases == 1U, "the release edge is counted");

    /* The middle and right buttons are edges of their own. */
    CHECK(usb_hid_boot_mouse_report(
              &state, (const uint8_t[]){0x06U, 0x01U, 0x01U},
              USB_HID_BOOT_MOUSE_REPORT_LENGTH, &event) &&
              event.pressed == (USB_HID_MOUSE_BUTTON_MIDDLE |
                                USB_HID_MOUSE_BUTTON_RIGHT),
          "the middle and right buttons are edges of their own");

    /* A report the boot protocol does not describe is refused, not guessed. */
    {
        const uint32_t reports_before = state.reports;
        const uint32_t refused_before = state.refused;

        /*
         * ITEM-448: the fourth byte is the wheel the boot mouse report is
         * commonly extended with; the three boot fields are still the report.
         */
        CHECK(usb_hid_boot_mouse_report(
                  &state, (const uint8_t[]){0x00U, 0x07U, 0xFBU, 0x01U},
                  USB_HID_BOOT_MOUSE_TRANSFER_LENGTH, &event) &&
                  event.dx == 7 && event.dy == -5 && event.wheel == 1 &&
                  state.reports == reports_before + 1U,
              "a boot mouse report with a wheel byte decodes the wheel");
        CHECK(!usb_hid_boot_mouse_report(
                  &state, (const uint8_t[]){0x00U, 0x01U, 0x00U},
                  USB_HID_BOOT_MOUSE_REPORT_LENGTH - 1U, &event) &&
                  state.refused == refused_before + 1U,
              "a short mouse report is refused, never padded");
        CHECK(!usb_hid_boot_mouse_report(&state, NULL,
                                         USB_HID_BOOT_MOUSE_REPORT_LENGTH,
                                         &event) &&
                  state.refused == refused_before + 2U,
              "a null mouse report is refused");
        CHECK(!usb_hid_boot_mouse_report(
                  &state, (const uint8_t[]){0x00U, 0x00U, 0x00U},
                  USB_HID_BOOT_MOUSE_REPORT_LENGTH, NULL),
              "a null event destination is refused");
    }

    /* The counters are the pointer's own view: transfers and errors too. */
    CHECK(usb_hid_mouse_state_reports(&state) == state.reports &&
              usb_hid_mouse_state_moves(&state) == state.moves &&
              usb_hid_mouse_state_presses(&state) == state.presses &&
              usb_hid_mouse_state_releases(&state) == state.releases &&
              usb_hid_mouse_state_refused(&state) == state.refused,
          "the mouse counters are published individually");

    usb_hid_mouse_state_reset(&state);
    CHECK(state.reports == 0U && state.moves == 0U &&
              state.buttons == 0U && state.initialized,
          "a reset forgets the old edges and keeps the state usable");
}

/*
 * ITEM-448: one configuration descriptor, two role walks.  A combined receiver
 * declares a keyboard interface and a mouse interface next to each other, so
 * the same walk has to be asked for each role - and each verdict has to name
 * its own role, not publish "no keyboard" for a machine that has a mouse.
 */
static void test_mouse_role_selection(void) {
    const struct usb_hid_config mouse_only_mouse = usb_hid_parse_config_role(
        boot_mouse_config, sizeof(boot_mouse_config), USB_HID_ROLE_MOUSE);
    const struct usb_hid_config mouse_only_keyboard = usb_hid_parse_config_role(
        boot_mouse_config, sizeof(boot_mouse_config), USB_HID_ROLE_KEYBOARD);
    const struct usb_hid_config both_mouse = usb_hid_parse_config_role(
        mouse_then_keyboard, sizeof(mouse_then_keyboard), USB_HID_ROLE_MOUSE);
    const struct usb_hid_config both_keyboard = usb_hid_parse_config_role(
        mouse_then_keyboard, sizeof(mouse_then_keyboard),
        USB_HID_ROLE_KEYBOARD);

    CHECK(mouse_only_mouse.status == USB_HID_STATUS_OK &&
              mouse_only_mouse.interface_number == 0U &&
              mouse_only_mouse.endpoint_address == 0x81U &&
              mouse_only_mouse.endpoint_max_packet == 4U &&
              !mouse_only_mouse.report_descriptor_needed,
          "the boot mouse is selected, and only, for the mouse role");
    CHECK(mouse_only_keyboard.status == USB_HID_STATUS_NO_BOOT_KEYBOARD &&
              mouse_only_keyboard.endpoint_address == 0U,
          "the same boot mouse is still no keyboard candidate");
    CHECK(both_mouse.status == USB_HID_STATUS_OK &&
              both_mouse.interface_number == 0U &&
              both_mouse.endpoint_address == 0x81U &&
              !both_mouse.report_descriptor_needed &&
              both_mouse.endpoint_max_packet == 4U,
          "the mouse role selects the boot mouse interface of a combined device");
    CHECK(both_keyboard.status == USB_HID_STATUS_OK &&
              both_keyboard.interface_number == 1U &&
              both_keyboard.endpoint_address == 0x82U &&
              both_keyboard.endpoint_max_packet == 8U,
          "the keyboard role selects the keyboard interface of the same device");
    CHECK(strcmp(usb_hid_status_string(USB_HID_STATUS_NO_HID_MOUSE),
                 "NO_HID_MOUSE") == 0,
          "a configuration with no mouse names the mouse verdict");
}

/*
 * ITEM-448: the report-descriptor half of the pointer.  A mouse that declares
 * no boot subclass is bound only when its own descriptor names a pointer
 * layout; the walk has to find buttons, X and Y - including the report-ID
 * layout a combined receiver uses - and refuse the shapes it cannot read.
 */
static void test_mouse_report_descriptor_walk(void) {
    struct usb_hid_mouse_layout layout;
    struct usb_hid_mouse_state state;
    struct usb_hid_mouse_event event;

    usb_hid_mouse_state_reset(&state);
    memset(&event, 0, sizeof(event));

    CHECK(usb_hid_parse_mouse_report_descriptor(
              mouse_report_descriptor, sizeof(mouse_report_descriptor),
              &layout) &&
              layout.report_id == 0U && layout.report_length == 3U &&
              layout.buttons_offset == 0U && layout.buttons_mask == 0x07U &&
              layout.dx_offset == 1U && layout.dy_offset == 2U &&
              layout.dx_size == 8U && layout.dy_size == 8U &&
              layout.wheel_offset == USB_HID_REPORT_OFFSET_NONE,
          "a mouse report descriptor names its pointer layout");
    CHECK(usb_hid_mouse_layout_decode(
              &state, &layout, (const uint8_t[]){0x02U, 0x03U, 0xFDU}, 3U,
              &event) &&
              event.buttons == USB_HID_MOUSE_BUTTON_RIGHT &&
              event.pressed == USB_HID_MOUSE_BUTTON_RIGHT &&
              event.dx == 3 && event.dy == -3 && state.reports == 1U,
          "a report-protocol mouse report decodes through its layout");

    {
        const uint32_t refused_before = state.refused;

        CHECK(!usb_hid_mouse_layout_decode(
                  &state, &layout, (const uint8_t[]){0x00U, 0x01U}, 2U,
                  &event) &&
                  state.refused == refused_before + 1U,
              "a report shorter than the layout is refused, not padded");
        CHECK(!usb_hid_mouse_layout_decode(
                  &state, &layout, (const uint8_t[]){0x00U, 0x00U, 0x00U},
                  0U, &event) &&
                  state.refused == refused_before + 2U,
              "an empty report is refused");
    }

    CHECK(usb_hid_parse_mouse_report_descriptor(
              report_id_mouse_report_descriptor,
              sizeof(report_id_mouse_report_descriptor), &layout) &&
              layout.report_id == 0x02U && layout.report_length == 5U &&
              layout.buttons_offset == 1U && layout.buttons_mask == 0x1FU &&
              layout.dx_offset == 2U && layout.dy_offset == 3U &&
              layout.dx_size == 8U && layout.dy_size == 8U &&
              layout.wheel_offset == 4U && layout.wheel_size == 8U,
          "a combined receiver's mouse collection is found under its report ID");
    CHECK(usb_hid_mouse_layout_decode(
              &state, &layout,
              (const uint8_t[]){0x02U, 0x02U, 0x10U, 0xF0U, 0x01U}, 5U,
              &event) &&
              event.dx == 16 && event.dy == -16 && event.wheel == 1 &&
              event.buttons == USB_HID_MOUSE_BUTTON_RIGHT,
          "the wheel byte after the axes is the wheel, not a delta");
    CHECK(!usb_hid_mouse_layout_decode(
              &state, &layout,
              (const uint8_t[]){0x03U, 0x00U, 0x00U, 0x00U, 0x00U}, 5U,
              &event),
          "a report of another report ID is refused");

    /*
     * ITEM-640: the physical 04D9:A09F layout.  The axes are signed
     * little-endian 16-bit fields and the wheel is the byte after them; the
     * trailing AC Pan byte must not become a delta or a wheel.
     */
    usb_hid_mouse_state_reset(&state);
    CHECK(usb_hid_parse_mouse_report_descriptor(
              toshiba_16bit_mouse_report_descriptor,
              sizeof(toshiba_16bit_mouse_report_descriptor), &layout) &&
              layout.report_id == 0U && layout.report_length == 7U &&
              layout.buttons_offset == 0U && layout.buttons_mask == 0x1FU &&
              layout.dx_offset == 1U && layout.dy_offset == 3U &&
              layout.dx_size == 16U && layout.dy_size == 16U &&
              layout.wheel_offset == 5U && layout.wheel_size == 8U,
          "a 16-bit relative pointer descriptor parses through its real layout");
    CHECK(usb_hid_mouse_layout_decode(
              &state, &layout,
              (const uint8_t[]){0x05U, 0x34U, 0x12U, 0xFEU, 0xFFU, 0x01U,
                                0x7FU},
              7U, &event) &&
              event.buttons == 0x05U && event.pressed == 0x05U &&
              event.dx == 0x1234 && event.dy == -2 && event.wheel == 1,
          "16-bit little-endian deltas keep their sign and wheel byte");
    CHECK(usb_hid_mouse_layout_decode(
              &state, &layout,
              (const uint8_t[]){0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0xFFU,
                                0x00U},
              7U, &event) &&
              event.dx == 0 && event.dy == 0 && event.wheel == -1 &&
              event.buttons == 0U,
          "a wheel-only report carries the negative wheel and no movement");
    CHECK(usb_hid_mouse_layout_decode(
              &state, &layout,
              (const uint8_t[]){0x00U, 0x00U, 0x80U, 0xFFU, 0x7FU, 0x00U,
                                0x00U},
              7U, &event) &&
              event.dx == -32768 && event.dy == 32767,
          "the signed 16-bit axis boundaries are not truncated");
    CHECK(!usb_hid_mouse_layout_decode(
              &state, &layout,
              (const uint8_t[]){0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U},
              6U, &event),
          "a report shorter than the pointer's own layout is refused");

    CHECK(!usb_hid_parse_mouse_report_descriptor(
              keyboard_report_descriptor, sizeof(keyboard_report_descriptor),
              &layout),
          "a keyboard report descriptor is not a pointer");
    CHECK(!usb_hid_parse_mouse_report_descriptor(
              consumer_report_descriptor, sizeof(consumer_report_descriptor),
              &layout),
          "a consumer-control descriptor is not a pointer");
    CHECK(!usb_hid_parse_mouse_report_descriptor(
              wide_axis_mouse_report_descriptor,
              sizeof(wide_axis_mouse_report_descriptor), &layout),
          "a pointer with 12-bit axes is refused, not read as bytes");
    CHECK(!usb_hid_parse_mouse_report_descriptor(NULL, 0U, &layout) &&
              !usb_hid_parse_mouse_report_descriptor(
                  mouse_report_descriptor, sizeof(mouse_report_descriptor),
                  NULL),
          "a null pointer descriptor or destination is refused");
}

/*
 * ITEM-459: every printable US-ASCII character has a usage.
 *
 * The tables stopped at 0x38, so a key above the US letter rows produced
 * nothing at all - the 2026-09-19 hardware run pressed the key left of Enter
 * on an ISO keyboard (usage 0x64, `Keyboard Non-US \ and |`) and watched a
 * dead key.  This proof walks the whole printable range and requires each
 * character to be reachable from some usage, states the usages the item is
 * about explicitly, and requires an unmapped usage to name itself instead of
 * disappearing.
 */
static void test_printable_ascii_is_reachable(void) {
    static const struct {
        uint8_t usage;
        bool shift;
        char character;
    } pinned[] = {
        {0x64U, false, '\\'},   /* Keyboard Non-US \ and | */
        {0x64U, true, '|'},
        {0x31U, false, '\\'},   /* Keyboard \ and |, the US key */
        {0x31U, true, '|'},
        {0x36U, false, ','},
        {0x36U, true, '<'},
        {0x37U, true, '>'},
        {0x54U, false, '/'},     /* Keypad / */
        {0x55U, false, '*'},     /* Keypad * */
        {0x56U, false, '-'},     /* Keypad - */
        {0x57U, false, '+'},     /* Keypad + */
        {0x59U, false, '1'},     /* Keypad 1 */
        {0x62U, false, '0'},     /* Keypad 0 */
        {0x63U, false, '.'},     /* Keypad . */
        {0x67U, false, '='},     /* Keypad = */
    };
    size_t index;

    for (index = 0U; index < sizeof(pinned) / sizeof(pinned[0]); ++index) {
        const char value = usb_hid_usage_character(
            pinned[index].usage, pinned[index].shift, false);

        if (value != pinned[index].character) {
            printf("FAIL: usage 0x%02x shift=%d decoded 0x%02x, expected "
                   "0x%02x\n",
                   pinned[index].usage, pinned[index].shift ? 1 : 0,
                   (unsigned char)value,
                   (unsigned char)pinned[index].character);
            ++failures;
        }
    }

    for (unsigned int character = 0x20U; character <= 0x7eU; ++character) {
        bool found = false;

        for (uint16_t usage = USB_HID_USAGE_MIN;
             usage <= USB_HID_USAGE_MAX && !found; ++usage) {
            for (unsigned int shifted = 0U; shifted < 2U && !found;
                 ++shifted) {
                if (usb_hid_usage_character((uint8_t)usage,
                                            shifted != 0U, false) ==
                    (char)character) {
                    found = true;
                }
            }
        }
        if (!found) {
            printf("FAIL: printable 0x%02x ('%c') has no usage\n", character,
                   (char)character);
            ++failures;
        }
    }
    CHECK(true, "every printable US-ASCII character is reachable");
}

/*
 * ITEM-459: a usage the tables cannot name is a finding the caller can
 * publish, and a key the decoder handles by name is not one.
 */
static void test_unmapped_usage_is_reported(void) {
    struct usb_hid_state state;
    struct keyboard_event events[USB_HID_BOOT_KEY_COUNT + 6U];
    uint8_t report[USB_HID_BOOT_REPORT_LENGTH];
    uint8_t usage = 0U;
    uint8_t modifiers = 0U;

    usb_hid_state_reset(&state);
    CHECK(!usb_hid_take_unmapped(&state, &usage, &modifiers),
          "a fresh decoder has no finding to report");

    /* 0x65 Keyboard Application has no character: the press names itself. */
    report_press(report, 0x65U, false);
    (void)usb_hid_boot_report_events(&state, report, events,
                                     sizeof(events) / sizeof(events[0]));
    CHECK(usb_hid_take_unmapped(&state, &usage, &modifiers),
          "an unmapped usage is a finding");
    CHECK(usage == 0x65U, "and names the usage");
    CHECK(modifiers == 0U, "with the modifiers that were down");
    CHECK(!usb_hid_take_unmapped(&state, &usage, &modifiers),
          "the finding is taken once, not once per report");

    /* A held key reports again only when it is pressed again. */
    (void)usb_hid_boot_report_events(&state, report, events,
                                     sizeof(events) / sizeof(events[0]));
    CHECK(!usb_hid_take_unmapped(&state, &usage, &modifiers),
          "a held unmapped key is one finding, not one per report");
    report_release(report);
    (void)usb_hid_boot_report_events(&state, report, events,
                                     sizeof(events) / sizeof(events[0]));
    report_press(report, 0x65U, false);
    (void)usb_hid_boot_report_events(&state, report, events,
                                     sizeof(events) / sizeof(events[0]));
    CHECK(usb_hid_take_unmapped(&state, &usage, &modifiers) && usage == 0x65U,
          "and a new press reports again");

    /* Shift with the finding is carried, so a reader sees the combination. */
    report_release(report);
    (void)usb_hid_boot_report_events(&state, report, events,
                                     sizeof(events) / sizeof(events[0]));
    report_press(report, 0x65U, true);
    (void)usb_hid_boot_report_events(&state, report, events,
                                     sizeof(events) / sizeof(events[0]));
    CHECK(usb_hid_take_unmapped(&state, &usage, &modifiers) &&
              modifiers == USB_HID_MODIFIER_LEFT_SHIFT,
          "the finding carries the modifiers that were down");

    /* A key the decoder handles by name is an answer, not a finding. */
    report_release(report);
    (void)usb_hid_boot_report_events(&state, report, events,
                                     sizeof(events) / sizeof(events[0]));
    report_press(report, USB_HID_USAGE_CAPS_LOCK, false);
    (void)usb_hid_boot_report_events(&state, report, events,
                                     sizeof(events) / sizeof(events[0]));
    CHECK(!usb_hid_take_unmapped(&state, &usage, &modifiers),
          "Caps Lock produces no character on purpose and is no finding");
    report_release(report);
    (void)usb_hid_boot_report_events(&state, report, events,
                                     sizeof(events) / sizeof(events[0]));
    report_press(report, USB_HID_USAGE_LEFT_ARROW, false);
    (void)usb_hid_boot_report_events(&state, report, events,
                                     sizeof(events) / sizeof(events[0]));
    CHECK(!usb_hid_take_unmapped(&state, &usage, &modifiers),
          "and neither is an arrow key the console names");
}

int main(void) {
    test_parse_selects_boot_keyboard();
    test_parse_walks_past_other_interfaces();
    test_parse_acceptance_host_keyboard();
    test_parse_rejects_devices_that_are_not_keyboards();
    test_parse_receiver_is_a_report_descriptor_candidate();
    test_parse_report_protocol_candidates();
    test_report_descriptor_walk();
    test_report_protocol_decode();
    test_describe_interfaces();
    test_parse_bounds();
    test_parse_rejects_non_keyboards();
    test_status_strings();
    test_mapping_table();
    test_printable_ascii_is_reachable();
    test_unmapped_usage_is_reported();
    test_console_commands_are_typable();
    test_report_events();
    test_hold_repeat_and_rollover();
    test_modifiers();
    test_report_length_classification();
    test_event_capacity_is_respected();
    test_mouse_report_decode();
    test_mouse_role_selection();
    test_mouse_report_descriptor_walk();

    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    puts("USB HID boot keyboard host test: ALL OK");
    return 0;
}

#else
#error "tests/usb-hid-host.c requires -DHFOC_USB_HID_HOST_TEST"
#endif
