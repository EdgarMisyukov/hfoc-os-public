#include "usb_hid.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-395: USB HID boot-protocol keyboard decoding.
 *
 * The two character tables below are indexed by HID keyboard usage ID (HID
 * 1.11 Appendix B.1.2) instead of by PS/2 scan code, and they hold the same
 * characters the PS/2 tables in kernel/src/keyboard.c hold for the
 * corresponding keys: the host proof re-types the console's command alphabet
 * through both decoders and requires the same character sequence, so the two
 * input paths cannot drift apart without a test failing.
 *
 * Only the usages a boot keyboard actually declares on a US layout are
 * populated; every other entry is '\0', which means "this usage produces no
 * character", exactly like the PS/2 tables.
 *
 * ITEM-459: the populated range used to stop at 0x38, so every usage above it
 * was dead - including 0x64 `Keyboard Non-US \ and |`, the key the 2026-09-19
 * hardware run pressed and got nothing from, and the whole numeric keypad.
 * The printable usages of HID 1.11 Appendix B.1.2 are all here now (0x04 to
 * 0x67), and a usage that still produces no character is reported by the
 * decoder instead of disappearing - see `state->unmapped_usage`.
 */
static const char normal_map[USB_HID_USAGE_TABLE_SIZE] = {
    [0x04]='a',[0x05]='b',[0x06]='c',[0x07]='d',[0x08]='e',[0x09]='f',[0x0a]='g',[0x0b]='h',[0x0c]='i',[0x0d]='j',
    [0x0e]='k',[0x0f]='l',[0x10]='m',[0x11]='n',[0x12]='o',[0x13]='p',[0x14]='q',[0x15]='r',[0x16]='s',[0x17]='t',
    [0x18]='u',[0x19]='v',[0x1a]='w',[0x1b]='x',[0x1c]='y',[0x1d]='z',
    [0x1e]='1',[0x1f]='2',[0x20]='3',[0x21]='4',[0x22]='5',[0x23]='6',[0x24]='7',[0x25]='8',[0x26]='9',[0x27]='0',
    [0x28]='\n',[0x2a]='\b',[0x2b]='\t',[0x2c]=' ',
    [0x2d]='-',[0x2e]='=',[0x2f]='[',[0x30]=']',[0x31]='\\',[0x32]='#',[0x33]=';',[0x34]='\'',[0x35]='`',
    [0x36]=',',[0x37]='.',[0x38]='/',
    /* ITEM-459: everything the boot report can carry above the letters and the
       digits - the numeric keypad, the non-US keys, and 0x64 in particular. */
    [0x54]='/',[0x55]='*',[0x56]='-',[0x57]='+',[0x58]='\n',
    [0x59]='1',[0x5a]='2',[0x5b]='3',[0x5c]='4',[0x5d]='5',
    [0x5e]='6',[0x5f]='7',[0x60]='8',[0x61]='9',[0x62]='0',
    [0x63]='.',[0x64]='\\',[0x67]='='
};

static const char shifted_map[USB_HID_USAGE_TABLE_SIZE] = {
    [0x04]='A',[0x05]='B',[0x06]='C',[0x07]='D',[0x08]='E',[0x09]='F',[0x0a]='G',[0x0b]='H',[0x0c]='I',[0x0d]='J',
    [0x0e]='K',[0x0f]='L',[0x10]='M',[0x11]='N',[0x12]='O',[0x13]='P',[0x14]='Q',[0x15]='R',[0x16]='S',[0x17]='T',
    [0x18]='U',[0x19]='V',[0x1a]='W',[0x1b]='X',[0x1c]='Y',[0x1d]='Z',
    [0x1e]='!',[0x1f]='@',[0x20]='#',[0x21]='$',[0x22]='%',[0x23]='^',[0x24]='&',[0x25]='*',[0x26]='(',[0x27]=')',
    [0x28]='\n',[0x2a]='\b',[0x2b]='\t',[0x2c]=' ',
    [0x2d]='_',[0x2e]='+',[0x2f]='{',[0x30]='}',[0x31]='|',[0x32]='~',[0x33]=':',[0x34]='"',[0x35]='~',
    [0x36]='<',[0x37]='>',[0x38]='?',
    /* ITEM-459: the shifted half of the same usages.  0x64 is the key the
       owner pressed: `\` with no modifier and `|` with Shift. */
    [0x54]='/',[0x55]='*',[0x56]='-',[0x57]='+',[0x58]='\n',
    [0x59]='1',[0x5a]='2',[0x5b]='3',[0x5c]='4',[0x5d]='5',
    [0x5e]='6',[0x5f]='7',[0x60]='8',[0x61]='9',[0x62]='0',
    [0x63]='.',[0x64]='|',[0x67]='+'
};

static uint16_t read_le16(const uint8_t *bytes) {
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U));
}

static struct usb_hid_config usb_hid_config_failed(
    enum usb_hid_status status) {
    struct usb_hid_config config;

    config.status = status;
    config.config_value = 0U;
    config.interface_number = 0U;
    config.interface_protocol = 0U;
    config.interface_subclass = 0U;
    config.report_descriptor_needed = false;
    config.endpoint_address = 0U;
    config.endpoint_number = 0U;
    config.endpoint_max_packet = 0U;
    config.endpoint_interval = 0U;
    config.total_length = 0U;
    return config;
}

uint16_t usb_hid_config_total_length(
    const uint8_t *descriptor,
    size_t length) {
    if (descriptor == NULL || length < USB_CONFIG_DESCRIPTOR_PREFIX) {
        return 0U;
    }
    if (descriptor[1] != USB_DESCRIPTOR_CONFIGURATION) {
        return 0U;
    }
    return read_le16(&descriptor[2]);
}

/*
 * ITEM-438: the interfaces a refused device actually declares.
 *
 * A refusal that says only "no boot keyboard in this configuration" cannot be
 * acted on: the 2026-09-17 hardware run read exactly that for a wired keyboard
 * whose interface list was never published, so the next run could not tell a
 * report-protocol keyboard from a mouse from a device that answered with a
 * descriptor this walk never reached.  This renders the list the walk saw.
 *
 * Two hexadecimal digits per field are exact: bInterfaceNumber,
 * bInterfaceClass, bInterfaceSubClass and bInterfaceProtocol are all one byte
 * wide.  The walk is bounded by `length` and by the interface count the
 * configuration itself declares, so a device cannot make it read past its own
 * descriptor, and each entry is bounded by USB_HID_INTERFACE_SUMMARY_MAX.
 */
static char *usb_hid_hex2(char *cursor, const char *end, uint8_t value) {
    static const char digits[] = "0123456789ABCDEF";

    if (cursor + 2 > end) {
        return cursor;
    }
    *cursor++ = digits[(value >> 4U) & 0x0FU];
    *cursor++ = digits[value & 0x0FU];
    return cursor;
}

size_t usb_hid_describe_interfaces(
    const uint8_t *descriptor,
    size_t length,
    char *out,
    size_t size) {
    uint16_t total_length;
    size_t offset;
    size_t walk_end;
    uint32_t rendered = 0U;
    bool truncated = false;
    char *cursor = out;
    const char *end;

    if (out == NULL || size == 0U) {
        return 0U;
    }
    out[0] = '\0';
    end = out + size - 1U;
    if (descriptor == NULL || length < USB_CONFIG_DESCRIPTOR_PREFIX ||
        descriptor[1] != USB_DESCRIPTOR_CONFIGURATION ||
        descriptor[0] < USB_CONFIG_DESCRIPTOR_PREFIX) {
        return 0U;
    }
    total_length = read_le16(&descriptor[2]);
    if (total_length == 0U || total_length > length) {
        total_length = (uint16_t)(length > 0xFFFFU ? 0xFFFFU : length);
    }
    walk_end = (size_t)total_length;
    offset = (size_t)descriptor[0];

    while (offset + 2U <= walk_end) {
        const uint8_t descriptor_length = descriptor[offset];
        const uint8_t descriptor_type = descriptor[offset + 1U];
        /* "N:CC/SS/PP" is eleven characters, a separating comma is one more,
           and the terminator is always one: an entry is written only when all
           three fit, so a clipped list never leaves a half-written entry. */
        const size_t entry_bytes = (rendered > 0U) ? 13U : 12U;

        if (descriptor_length < 2U ||
            descriptor_length > walk_end - offset) {
            break;
        }
        if (descriptor_type != USB_DESCRIPTOR_INTERFACE ||
            descriptor_length < 9U) {
            offset += descriptor_length;
            continue;
        }
        if (rendered >= USB_HID_INTERFACE_SUMMARY_MAX ||
            (size_t)(end - cursor) + 1U < entry_bytes) {
            truncated = true;
            break;
        }
        if (rendered > 0U) {
            *cursor++ = ',';
        }
        cursor = usb_hid_hex2(cursor, end, descriptor[offset + 2U]);
        *cursor++ = ':';
        cursor = usb_hid_hex2(cursor, end, descriptor[offset + 5U]);
        *cursor++ = '/';
        cursor = usb_hid_hex2(cursor, end, descriptor[offset + 6U]);
        *cursor++ = '/';
        cursor = usb_hid_hex2(cursor, end, descriptor[offset + 7U]);
        ++rendered;
        offset += descriptor_length;
    }
    if (truncated && cursor < end) {
        *cursor++ = '+';
    }
    *cursor = '\0';
    return (size_t)(cursor - out);
}

/*
 * ITEM-438: how strongly an interface claims to be a keyboard, and what the
 * driver must do to confirm it.
 *
 *   * a declared boot keyboard (class 3, subclass 1, protocol 1) speaks the
 *     8-byte boot report the decoder already knows, so it is the best
 *     interface and needs nothing read;
 *   * a boot-capable interface in report protocol (class 3, subclass 1,
 *     protocol 0) may be a keyboard or the mouse half of a combo receiver -
 *     the descriptor does not say.  Its report descriptor does, so it is a
 *     candidate whose confirmation is that read;
 *   * a HID interface with no boot subclass (class 3, subclass 0) is the shape
 *     the 2026-09-17 run's keyboard was refused for.  Only its report
 *     descriptor can say whether it is a keyboard, so it is the weakest
 *     candidate and needs the same read;
 *   * anything else - a boot mouse (protocol 2), a non-HID class - never is.
 */
#define USB_HID_INTERFACE_SCORE_BOOT_KEYBOARD 3U
#define USB_HID_INTERFACE_SCORE_REPORT_PROTOCOL 2U
#define USB_HID_INTERFACE_SCORE_HID 1U

/*
 * ITEM-448: the same preference order for the other role.  The boot protocol
 * the interface declares is the whole difference between the two walks: a
 * keyboard interface is protocol 1 and a mouse interface is protocol 2, and an
 * interface that declares the *other* role's boot protocol is not a candidate
 * for this role at all - which is what keeps a boot mouse from being selected
 * as a keyboard and reported as `NO_BOOT_KEYBOARD` (the 2026-09-17 hardware
 * run's receiver) and a boot keyboard from being selected as the mouse.
 */
static uint8_t usb_hid_interface_score(
    uint8_t interface_class,
    uint8_t interface_subclass,
    uint8_t interface_protocol,
    enum usb_hid_role role) {
    const uint8_t boot_protocol = role == USB_HID_ROLE_MOUSE
                                      ? USB_HID_PROTOCOL_MOUSE
                                      : USB_HID_PROTOCOL_KEYBOARD;

    if (interface_class != USB_HID_CLASS) {
        return 0U;
    }
    if (interface_subclass == USB_HID_SUBCLASS_BOOT) {
        if (interface_protocol == boot_protocol) {
            return USB_HID_INTERFACE_SCORE_BOOT_KEYBOARD;
        }
        if (interface_protocol == 0U) {
            return USB_HID_INTERFACE_SCORE_REPORT_PROTOCOL;
        }
        return 0U;
    }
    return USB_HID_INTERFACE_SCORE_HID;
}

/*
 * ITEM-448: which interface a walk with no candidate publishes as its verdict.
 * The caller reads the status string, so "no keyboard here" and "no mouse here"
 * have to be different values rather than one generic failure.
 */
static enum usb_hid_status usb_hid_role_none_status(enum usb_hid_role role) {
    return role == USB_HID_ROLE_MOUSE ? USB_HID_STATUS_NO_HID_MOUSE
                                      : USB_HID_STATUS_NO_BOOT_KEYBOARD;
}

struct usb_hid_config usb_hid_parse_config(
    const uint8_t *descriptor,
    size_t length) {
    return usb_hid_parse_config_role(descriptor, length,
                                     USB_HID_ROLE_KEYBOARD);
}

struct usb_hid_config usb_hid_parse_config_role(
    const uint8_t *descriptor,
    size_t length,
    enum usb_hid_role role) {
    uint16_t total_length;
    size_t offset;
    size_t walk_end;
    uint8_t config_value;
    uint8_t interface_number = 0U;
    uint8_t interface_subclass = 0U;
    uint8_t interface_protocol = 0U;
    bool in_candidate = false;
    bool candidate_needs_report_descriptor = false;
    bool saw_endpoint = false;
    uint8_t candidate_score = 0U;
    uint8_t best_score = 0U;
    enum usb_hid_status best_status = usb_hid_role_none_status(role);
    struct usb_hid_config best = usb_hid_config_failed(
        usb_hid_role_none_status(role));

    if (descriptor == NULL) {
        return usb_hid_config_failed(USB_HID_STATUS_NULL);
    }
    if (length < USB_CONFIG_DESCRIPTOR_PREFIX) {
        return usb_hid_config_failed(USB_HID_STATUS_TRUNCATED);
    }
    if (descriptor[1] != USB_DESCRIPTOR_CONFIGURATION) {
        return usb_hid_config_failed(USB_HID_STATUS_NOT_CONFIGURATION);
    }
    if (descriptor[0] < USB_CONFIG_DESCRIPTOR_PREFIX) {
        return usb_hid_config_failed(USB_HID_STATUS_BAD_DESCRIPTOR_LENGTH);
    }

    total_length = read_le16(&descriptor[2]);
    if (total_length == 0U) {
        return usb_hid_config_failed(USB_HID_STATUS_BAD_TOTAL_LENGTH);
    }
    if (total_length > USB_HID_CONFIG_DESCRIPTOR_MAX ||
        total_length > length) {
        /*
         * The caller reads exactly wTotalLength bytes, so a value above the
         * size this decoder accepts - or above what was actually read - is a
         * device this kernel cannot describe, not a descriptor to guess at.
         */
        return usb_hid_config_failed(
            total_length > USB_HID_CONFIG_DESCRIPTOR_MAX
                ? USB_HID_STATUS_BAD_TOTAL_LENGTH
                : USB_HID_STATUS_TRUNCATED);
    }
    if (descriptor[4] == 0U) {
        return usb_hid_config_failed(USB_HID_STATUS_NO_INTERFACES);
    }

    config_value = descriptor[5];
    saw_endpoint = false;
    walk_end = (size_t)total_length;
    offset = (size_t)descriptor[0];

    while (offset < walk_end) {
        const uint8_t descriptor_length = descriptor[offset];
        const uint8_t descriptor_type = descriptor[offset + 1U];

        if (descriptor_length < 2U ||
            descriptor_length > walk_end - offset) {
            return usb_hid_config_failed(
                descriptor_length < 2U
                    ? USB_HID_STATUS_BAD_DESCRIPTOR_LENGTH
                    : USB_HID_STATUS_TRUNCATED);
        }

        if (descriptor_type == USB_DESCRIPTOR_INTERFACE) {
            if (descriptor_length < 9U) {
                return usb_hid_config_failed(
                    USB_HID_STATUS_BAD_DESCRIPTOR_LENGTH);
            }
            if (in_candidate && !saw_endpoint) {
                /* The interface ended without a report path; that is only the
                   verdict when nothing better was found, so remember it and
                   keep walking: the next interface may be the keyboard. */
                if (candidate_score > best_score) {
                    best_status = USB_HID_STATUS_NO_INTERRUPT_IN;
                }
            }
            interface_number = descriptor[offset + 2U];
            interface_subclass = descriptor[offset + 6U];
            interface_protocol = descriptor[offset + 7U];
            candidate_score =
                usb_hid_interface_score(descriptor[offset + 5U],
                                        interface_subclass,
                                        interface_protocol,
                                        role);
            in_candidate = candidate_score != 0U;
            candidate_needs_report_descriptor =
                candidate_score < USB_HID_INTERFACE_SCORE_BOOT_KEYBOARD;
            saw_endpoint = false;
        } else if (descriptor_type == USB_DESCRIPTOR_ENDPOINT) {
            if (descriptor_length < 7U) {
                return usb_hid_config_failed(
                    USB_HID_STATUS_BAD_DESCRIPTOR_LENGTH);
            }
            if (in_candidate) {
                const uint8_t address = descriptor[offset + 2U];
                const uint8_t attributes = descriptor[offset + 3U];
                const uint16_t max_packet = read_le16(&descriptor[offset + 4U]);
                const bool interrupt_in =
                    (attributes & USB_ENDPOINT_TYPE_MASK) ==
                        USB_ENDPOINT_TYPE_INTERRUPT &&
                    (address & USB_ENDPOINT_DIRECTION_IN) != 0U &&
                    (address & USB_ENDPOINT_NUMBER_MASK) != 0U;

                saw_endpoint = true;
                if (!interrupt_in) {
                    /* Not the report endpoint; keep looking inside this
                       interface, and report a wrong-type endpoint only if the
                       interface has nothing better. */
                    if (candidate_score > best_score) {
                        best_status = USB_HID_STATUS_BAD_ENDPOINT_TYPE;
                    }
                    offset += descriptor_length;
                    continue;
                }
                if (max_packet == 0U || max_packet > 64U) {
                    if (candidate_score > best_score) {
                        best_status = USB_HID_STATUS_BAD_ENDPOINT_SIZE;
                    }
                    offset += descriptor_length;
                    continue;
                }
                if (candidate_score > best_score) {
                    struct usb_hid_config config;

                    best_score = candidate_score;
                    best_status = USB_HID_STATUS_OK;
                    config.status = USB_HID_STATUS_OK;
                    config.config_value = config_value;
                    config.interface_number = interface_number;
                    config.interface_protocol = interface_protocol;
                    config.interface_subclass = interface_subclass;
                    config.report_descriptor_needed =
                        candidate_needs_report_descriptor;
                    config.endpoint_address = address;
                    config.endpoint_number =
                        (uint8_t)(address & USB_ENDPOINT_NUMBER_MASK);
                    config.endpoint_max_packet = max_packet;
                    config.endpoint_interval = descriptor[offset + 6U];
                    config.total_length = total_length;
                    best = config;
                }
            } else {
                saw_endpoint = true;
            }
        }
        offset += descriptor_length;
    }

    if (best_score != 0U) {
        return best;
    }
    if (in_candidate && !saw_endpoint) {
        /* The last interface was a candidate whose report path never came. */
        best_status = USB_HID_STATUS_NO_INTERRUPT_IN;
    }
    return usb_hid_config_failed(best_status);
}

/*
 * ITEM-438: the HID report-descriptor walk.
 *
 * The device classes this kernel binds are told apart by their report
 * descriptors, not by their interface descriptors: a combo receiver's keyboard
 * half and its mouse half can both declare "HID class 3" and nothing else, and
 * the 2026-09-17 hardware run's wired keyboard was refused exactly because the
 * interface descriptor alone did not spell "keyboard".  This walk answers the
 * only question the driver has - "is this a keyboard, and where in its reports
 * are the modifier byte and the key array" - and answers it with the
 * descriptor's own numbers.
 *
 * HID 1.11 section 6.2.2.7 item encoding: one prefix byte per short item (bits
 * 1:0 size, 3:2 type, 7:4 tag), a 0xFE prefix for a long item, and a payload
 * of 0, 1, 2 or 4 bytes.  Only the items this question needs are interpreted:
 * the global Usage Page, Report Size, Report Count and Report ID, the local
 * Usage, Usage Minimum and Usage Maximum, and the main Collection, End
 * Collection and Input.  Everything else is skipped by its own length, so a
 * descriptor with items this walk does not know still parses.
 */
#define USB_HID_ITEM_TYPE_MAIN 0U
#define USB_HID_ITEM_TYPE_GLOBAL 1U
#define USB_HID_ITEM_TYPE_LOCAL 2U

#define USB_HID_GLOBAL_USAGE_PAGE 0x0U
#define USB_HID_GLOBAL_REPORT_SIZE 0x7U
#define USB_HID_GLOBAL_REPORT_ID 0x8U
#define USB_HID_GLOBAL_REPORT_COUNT 0x9U

#define USB_HID_LOCAL_USAGE 0x0U
#define USB_HID_LOCAL_USAGE_MINIMUM 0x1U
#define USB_HID_LOCAL_USAGE_MAXIMUM 0x2U

#define USB_HID_MAIN_INPUT 0x8U
#define USB_HID_MAIN_COLLECTION 0xAU
#define USB_HID_MAIN_END_COLLECTION 0xCU

#define USB_HID_USAGE_PAGE_GENERIC_DESKTOP 0x01U
#define USB_HID_USAGE_GENERIC_KEYBOARD 0x06U
#define USB_HID_USAGE_PAGE_KEYBOARD 0x07U
#define USB_HID_USAGE_KEYBOARD_MODIFIERS_MIN 0xE0U
#define USB_HID_USAGE_KEYBOARD_MODIFIERS_MAX 0xE7U

static uint32_t usb_hid_item_value(const uint8_t *bytes, uint8_t size) {
    uint32_t value = 0U;

    for (uint8_t index = 0U; index < size; ++index) {
        value |= (uint32_t)bytes[index] << (8U * index);
    }
    return value;
}

bool usb_hid_parse_report_descriptor(
    const uint8_t *descriptor,
    size_t length,
    struct usb_hid_report_layout *layout) {
    uint32_t usage_page = 0U;
    uint32_t usage = 0U;
    uint32_t usage_min = 0U;
    uint32_t usage_max = 0U;
    uint32_t report_size = 0U;
    uint32_t report_count = 0U;
    uint32_t report_id = 0U;
    uint32_t bit_offset = 0U;
    uint32_t depth = 0U;
    uint32_t keyboard_depth = 0U;
    bool have_report_id = false;
    bool have_modifiers = false;
    bool have_keys = false;
    struct usb_hid_report_layout found;
    size_t offset = 0U;

    if (layout == NULL) {
        return false;
    }
    found.report_id = 0U;
    found.report_length = 0U;
    found.modifier_offset = USB_HID_REPORT_OFFSET_NONE;
    found.key_offset = 0U;
    found.key_count = 0U;
    if (descriptor == NULL || length == 0U) {
        return false;
    }

    while (offset < length) {
        const uint8_t prefix = descriptor[offset];
        uint8_t item_size;
        uint8_t item_type;
        uint8_t item_tag;
        uint32_t value;

        if (prefix == 0xFEU) {
            /* Long item: bDataSize then bLongItemTag, then the data. */
            const uint8_t data_size =
                offset + 1U < length ? descriptor[offset + 1U] : 0U;

            if (offset + 3U + (size_t)data_size > length) {
                break;
            }
            offset += 3U + (size_t)data_size;
            continue;
        }
        item_size = (uint8_t)(prefix & 0x03U);
        if (item_size == 3U) {
            item_size = 4U;
        }
        item_type = (uint8_t)((prefix >> 2U) & 0x03U);
        item_tag = (uint8_t)((prefix >> 4U) & 0x0FU);
        if (offset + 1U + (size_t)item_size > length) {
            break;
        }
        value = usb_hid_item_value(&descriptor[offset + 1U], item_size);
        offset += 1U + (size_t)item_size;

        if (item_type == USB_HID_ITEM_TYPE_GLOBAL) {
            switch (item_tag) {
            case USB_HID_GLOBAL_USAGE_PAGE:
                usage_page = value;
                break;
            case USB_HID_GLOBAL_REPORT_SIZE:
                /* Bounded so a hostile descriptor's arithmetic stays inside
                   this function; 32 bits is already beyond any HID field. */
                report_size = value > 32U ? 32U : value;
                break;
            case USB_HID_GLOBAL_REPORT_ID:
                report_id = value & 0xFFU;
                have_report_id = report_id != 0U;
                /* A report ID starts a new report, so its bit layout starts
                   at zero.  Once the keyboard's key array is found, later
                   report IDs describe other reports and do not move it. */
                if (!have_keys) {
                    bit_offset = 0U;
                }
                break;
            case USB_HID_GLOBAL_REPORT_COUNT:
                report_count = value > 256U ? 256U : value;
                break;
            default:
                break;
            }
            continue;
        }
        if (item_type == USB_HID_ITEM_TYPE_LOCAL) {
            switch (item_tag) {
            case USB_HID_LOCAL_USAGE:
                usage = value;
                break;
            case USB_HID_LOCAL_USAGE_MINIMUM:
                usage_min = value;
                break;
            case USB_HID_LOCAL_USAGE_MAXIMUM:
                usage_max = value;
                break;
            default:
                break;
            }
            continue;
        }
        if (item_type != USB_HID_ITEM_TYPE_MAIN) {
            continue;
        }
        if (item_tag == USB_HID_MAIN_COLLECTION) {
            ++depth;
            if (usage_page == USB_HID_USAGE_PAGE_GENERIC_DESKTOP &&
                usage == USB_HID_USAGE_GENERIC_KEYBOARD) {
                keyboard_depth = depth;
            }
        } else if (item_tag == USB_HID_MAIN_END_COLLECTION) {
            if (keyboard_depth == depth) {
                keyboard_depth = 0U;
            }
            if (depth > 0U) {
                --depth;
            }
        } else if (item_tag == USB_HID_MAIN_INPUT && keyboard_depth != 0U &&
                   !have_keys) {
            const uint32_t prefix_bytes = have_report_id ? 1U : 0U;
            const uint32_t byte_offset = bit_offset / 8U;
            const bool byte_aligned = (bit_offset % 8U) == 0U;

            if (!have_modifiers && report_size == 1U && report_count >= 3U &&
                byte_aligned && usage_page == USB_HID_USAGE_PAGE_KEYBOARD &&
                usage_min == USB_HID_USAGE_KEYBOARD_MODIFIERS_MIN &&
                usage_max >= USB_HID_USAGE_KEYBOARD_MODIFIERS_MAX &&
                prefix_bytes + byte_offset < USB_HID_REPORT_LENGTH_MAX) {
                found.modifier_offset =
                    (uint8_t)(prefix_bytes + byte_offset);
                have_modifiers = true;
            } else if (report_size == 8U &&
                       report_count >= USB_HID_BOOT_KEY_COUNT &&
                       byte_aligned &&
                       usage_page == USB_HID_USAGE_PAGE_KEYBOARD) {
                uint32_t keys = report_count > USB_HID_REPORT_KEYS_MAX
                                    ? USB_HID_REPORT_KEYS_MAX
                                    : report_count;
                const uint32_t start = prefix_bytes + byte_offset;

                if (start + keys <= USB_HID_REPORT_LENGTH_MAX) {
                    found.report_id = have_report_id ? (uint8_t)report_id : 0U;
                    found.key_offset = (uint8_t)start;
                    found.key_count = (uint8_t)keys;
                    found.report_length = (uint8_t)(start + keys);
                    have_keys = true;
                }
            }
        }
        if (item_type == USB_HID_ITEM_TYPE_MAIN) {
            /* Local items apply to one main item only. */
            usage = 0U;
            usage_min = 0U;
            usage_max = 0U;
            /*
             * Only a data item consumes report bits: Input, Output and
             * Feature carry Report Size x Report Count bits each, while a
             * Collection or End Collection carries none.  Counting the
             * collections too would put every offset after the first
             * collection in the wrong byte.
             */
            if (item_tag == USB_HID_MAIN_INPUT || item_tag == 0x9U ||
                item_tag == 0xBU) {
                bit_offset += report_size * report_count;
                if (bit_offset > 4096U) {
                    bit_offset = 4096U;
                }
            }
        }
    }

    if (!have_keys || found.key_count == 0U) {
        return false;
    }
    *layout = found;
    return true;
}

bool usb_hid_layout_build_boot_report(
    const struct usb_hid_report_layout *layout,
    const uint8_t *report,
    uint32_t length,
    uint8_t boot_report[USB_HID_BOOT_REPORT_LENGTH]) {
    if (layout == NULL || report == NULL || boot_report == NULL) {
        return false;
    }
    if (layout->key_count == 0U ||
        layout->key_count > USB_HID_REPORT_KEYS_MAX ||
        layout->key_offset < (layout->report_id != 0U ? 1U : 0U) ||
        (uint32_t)layout->key_offset + layout->key_count >
            USB_HID_REPORT_LENGTH_MAX ||
        (uint32_t)layout->key_offset + layout->key_count >
            (uint32_t)layout->report_length) {
        return false;
    }
    if (length < layout->report_length) {
        return false;
    }
    if (layout->report_id != 0U && report[0] != layout->report_id) {
        return false;
    }
    /* Freestanding: the kernel build has no memset here, so the boot report is
       zeroed by hand before its three fields are filled. */
    for (uint32_t index = 0U; index < USB_HID_BOOT_REPORT_LENGTH; ++index) {
        boot_report[index] = 0U;
    }
    if (layout->modifier_offset != USB_HID_REPORT_OFFSET_NONE) {
        if ((uint32_t)layout->modifier_offset >= length) {
            return false;
        }
        boot_report[0] = report[layout->modifier_offset];
    }
    for (uint32_t index = 0U; index < USB_HID_REPORT_KEYS_MAX; ++index) {
        if (index < layout->key_count) {
            boot_report[2U + index] = report[layout->key_offset + index];
        }
    }
    return true;
}

const char *usb_hid_status_string(enum usb_hid_status status) {    switch (status) {
    case USB_HID_STATUS_OK:
        return "OK";
    case USB_HID_STATUS_NULL:
        return "NULL";
    case USB_HID_STATUS_TRUNCATED:
        return "TRUNCATED";
    case USB_HID_STATUS_NOT_CONFIGURATION:
        return "NOT_CONFIGURATION";
    case USB_HID_STATUS_BAD_TOTAL_LENGTH:
        return "BAD_TOTAL_LENGTH";
    case USB_HID_STATUS_NO_INTERFACES:
        return "NO_INTERFACES";
    case USB_HID_STATUS_BAD_DESCRIPTOR_LENGTH:
        return "BAD_DESCRIPTOR_LENGTH";
    case USB_HID_STATUS_NO_BOOT_KEYBOARD:
        return "NO_BOOT_KEYBOARD";
    case USB_HID_STATUS_NO_HID_MOUSE:
        return "NO_HID_MOUSE";
    case USB_HID_STATUS_NO_INTERRUPT_IN:
        return "NO_INTERRUPT_IN";
    case USB_HID_STATUS_BAD_ENDPOINT_TYPE:
        return "BAD_ENDPOINT_TYPE";
    case USB_HID_STATUS_BAD_ENDPOINT_SIZE:
        return "BAD_ENDPOINT_SIZE";
    default:
        return "UNKNOWN";
    }
}

enum usb_hid_report_length usb_hid_boot_report_classify(
    uint32_t transfer_length,
    uint32_t *received_bytes) {
    /*
     * ITEM-436: the buffer is USB_HID_BOOT_REPORT_LENGTH bytes, so a residual
     * of zero is the whole report and any residual up to that length is a
     * short transfer. Anything else is not a completion this 8-byte buffer can
     * produce: the subtraction below is done only once the residual is known
     * to fit, so a controller answer that cannot happen is named instead of
     * wrapping into USB_HID_BOOT_REPORT_LENGTH - transfer_length.
     */
    if (transfer_length > USB_HID_BOOT_REPORT_LENGTH) {
        if (received_bytes != NULL) {
            *received_bytes = 0U;
        }
        return USB_HID_REPORT_LENGTH_IMPOSSIBLE;
    }
    if (received_bytes != NULL) {
        *received_bytes = USB_HID_BOOT_REPORT_LENGTH - transfer_length;
    }
    return transfer_length == 0U ? USB_HID_REPORT_LENGTH_EXACT
                                 : USB_HID_REPORT_LENGTH_SHORT;
}

bool usb_hid_no_report_due(
    bool bound,
    uint32_t decoded_reports,
    uint64_t elapsed_seconds,
    uint64_t window_seconds) {
    /* A silence that has not lasted the window yet is still just silence. */
    return bound && decoded_reports == 0U && elapsed_seconds >= window_seconds;
}

char usb_hid_usage_character(uint8_t usage, bool shifted, bool caps_lock) {
    char value;

    if (usage >= USB_HID_USAGE_TABLE_SIZE) {
        return '\0';
    }
    value = shifted ? shifted_map[usage] : normal_map[usage];
    if (value >= 'a' && value <= 'z' && caps_lock) {
        value = (char)(value - 'a' + 'A');
    } else if (value >= 'A' && value <= 'Z' && caps_lock) {
        value = (char)(value - 'A' + 'a');
    }
    return value;
}

void usb_hid_state_reset(struct usb_hid_state *state) {
    size_t index;

    if (state == NULL) {
        return;
    }
    state->initialized = true;
    state->caps_lock = false;
    state->modifiers = 0U;
    state->unmapped_usage = 0U;
    state->unmapped_modifiers = 0U;
    for (index = 0U; index < USB_HID_BOOT_KEY_COUNT; ++index) {
        state->keys[index] = 0U;
    }
}

/*
 * ITEM-459: the usages this decoder handles by name, with no character of
 * their own.  They are answers, not findings: a key that moves the cursor or
 * toggles Caps Lock produces no character on purpose, so reporting it as
 * "unmapped" would drown the one usage that really has no meaning.
 */
static bool usb_hid_usage_is_named(uint8_t usage) {
    switch (usage) {
        case USB_HID_USAGE_ENTER:
        case USB_HID_USAGE_ESCAPE:
        case USB_HID_USAGE_BACKSPACE:
        case USB_HID_USAGE_TAB:
        case USB_HID_USAGE_SPACE:
        case USB_HID_USAGE_CAPS_LOCK:
        case USB_HID_USAGE_HOME:
        case USB_HID_USAGE_DELETE:
        case USB_HID_USAGE_END:
        case USB_HID_USAGE_RIGHT_ARROW:
        case USB_HID_USAGE_LEFT_ARROW:
        case USB_HID_USAGE_DOWN_ARROW:
        case USB_HID_USAGE_UP_ARROW:
            return true;
        default:
            return false;
    }
}

/*
 * The usages the boot protocol reserves: 0x00 is "no key", 0x01 the rollover
 * error, 0x02 POST fail and 0x03 "error undefined".  0xE0..0xE7 are the
 * modifiers, which the report carries in its own byte, and 0x3A..0x45 are the
 * function keys and 0x46..0x53 the keys with no character of their own that
 * the named list above does not cover - a finding is a key the machine cannot
 * name, not a key that never had a name to begin with.
 */
static bool usb_hid_usage_is_reserved(uint8_t usage) {
    return usage <= 0x03U || usage >= 0xe0U ||
           (usage >= 0x3aU && usage <= 0x53U);
}

static bool usb_hid_usage_is_letter(uint8_t usage) {
    return usage >= 0x04U && usage <= 0x1dU;
}

static bool usb_hid_key_held(
    const struct usb_hid_state *state,
    uint8_t usage) {
    size_t index;

    for (index = 0U; index < USB_HID_BOOT_KEY_COUNT; ++index) {
        if (state->keys[index] == usage) {
            return true;
        }
    }
    return false;
}

size_t usb_hid_boot_report_events(
    struct usb_hid_state *state,
    const uint8_t report[USB_HID_BOOT_REPORT_LENGTH],
    struct keyboard_event *events,
    size_t capacity) {
    uint8_t pressed[USB_HID_BOOT_KEY_COUNT] = { 0U };
    uint8_t modifiers;
    size_t pressed_count = 0U;
    size_t event_count = 0U;
    size_t index;

    if (state == NULL || report == NULL) {
        return 0U;
    }
    if (!state->initialized) {
        usb_hid_state_reset(state);
    }
    if (events == NULL && capacity != 0U) {
        return 0U;
    }

    modifiers = report[0];
    /*
     * Collect the keys this report declares, in report order and without
     * duplicates, so a malformed report that repeats a usage cannot produce
     * two presses for one keystroke.
     */
    for (index = 0U; index < USB_HID_BOOT_KEY_COUNT; ++index) {
        const uint8_t usage = report[2U + index];
        size_t seen;

        /* 0 is "no key"; 1 is the rollover-error usage and is not a key. */
        if (usage <= 0x01U) {
            continue;
        }
        for (seen = 0U; seen < pressed_count; ++seen) {
            if (pressed[seen] == usage) {
                break;
            }
        }
        if (seen != pressed_count) {
            continue;
        }
        if (pressed_count < USB_HID_BOOT_KEY_COUNT) {
            pressed[pressed_count++] = usage;
        }
    }

    for (index = 0U; index < pressed_count; ++index) {
        const uint8_t usage = pressed[index];
        const bool shifted =
            (modifiers &
             (USB_HID_MODIFIER_LEFT_SHIFT |
              USB_HID_MODIFIER_RIGHT_SHIFT)) != 0U;
        const bool control =
            (modifiers &
             (USB_HID_MODIFIER_LEFT_CTRL |
              USB_HID_MODIFIER_RIGHT_CTRL)) != 0U;

        if (usb_hid_key_held(state, usage)) {
            /* Still down: the boot protocol repeats held keys in every
               report, and the console must see one press. */
            continue;
        }
        if (usage == USB_HID_USAGE_CAPS_LOCK) {
            /*
             * Boot reports carry no lock state, so the host owns the latch -
             * exactly as the PS/2 decode does - and the device LED is left
             * alone (documented limitation).
             */
            state->caps_lock = !state->caps_lock;
            continue;
        }
        if (usage == USB_HID_USAGE_ENTER) {
            if (event_count < capacity) {
                events[event_count].type = KEYBOARD_EVENT_ENTER;
                events[event_count].character = '\0';
            }
            ++event_count;
            continue;
        }
        if (usage == USB_HID_USAGE_BACKSPACE) {
            if (event_count < capacity) {
                events[event_count].type = KEYBOARD_EVENT_BACKSPACE;
                events[event_count].character = '\0';
            }
            ++event_count;
            continue;
        }

        {
            enum keyboard_event_type type = KEYBOARD_EVENT_CHARACTER;
            char character = '\0';
            bool named = true;

            switch (usage) {
            case USB_HID_USAGE_LEFT_ARROW:
                type = KEYBOARD_EVENT_LEFT;
                break;
            case USB_HID_USAGE_RIGHT_ARROW:
                type = KEYBOARD_EVENT_RIGHT;
                break;
            case USB_HID_USAGE_HOME:
                type = KEYBOARD_EVENT_HOME;
                break;
            case USB_HID_USAGE_END:
                type = KEYBOARD_EVENT_END;
                break;
            case USB_HID_USAGE_UP_ARROW:
                type = KEYBOARD_EVENT_UP;
                break;
            case USB_HID_USAGE_DOWN_ARROW:
                type = KEYBOARD_EVENT_DOWN;
                break;
            case USB_HID_USAGE_DELETE:
                type = KEYBOARD_EVENT_DELETE;
                break;
            case USB_HID_USAGE_ESCAPE:
                /*
                 * The PS/2 decoder has no escape path either: scancode 0x01
                 * produces no character and no event, and the console line
                 * editor accepts printable characters only.
                 */
                named = false;
                break;
            default:
                named = false;
                character =
                    usb_hid_usage_character(usage, shifted, state->caps_lock);
                if (character == '\0') {
                    /*
                     * ITEM-459: a usage the tables cannot name is a finding.
                     * The state carries the most recent one to the caller,
                     * which publishes it once per press, so the operator's dead
                     * key names its own code instead of vanishing.
                     */
                    if (!usb_hid_usage_is_named(usage) &&
                        !usb_hid_usage_is_reserved(usage)) {
                        state->unmapped_usage = usage;
                        state->unmapped_modifiers = modifiers;
                    }
                    continue;
                }
                if (control && usb_hid_usage_is_letter(usage)) {
                    /*
                     * Ctrl+letter decodes to the US-ASCII control character,
                     * the same convention the PS/2 path would follow if it
                     * decoded Ctrl. The console line editor accepts printable
                     * characters only, so a control character is currently
                     * dropped there rather than acted on: this is a decode,
                     * not a new console binding.
                     */
                    char lower = character;
                    if (lower >= 'A' && lower <= 'Z') {
                        lower = (char)(lower - 'A' + 'a');
                    }
                    character = (char)(lower - 'a' + 1);
                }
                break;
            }
            if (named || character != '\0') {
                if (event_count < capacity) {
                    events[event_count].type = type;
                    events[event_count].character = character;
                }
                ++event_count;
            }
        }
    }

    for (index = 0U; index < USB_HID_BOOT_KEY_COUNT; ++index) {
        state->keys[index] = pressed[index];
    }
    state->modifiers = modifiers;
    return event_count;
}

/*
 * ITEM-448: the boot mouse report decoder.  Three bytes, one button byte and
 * two signed relative deltas; every other length is refused, so a pointer can
 * only move by a report the device actually sent.
 */
void usb_hid_mouse_state_reset(struct usb_hid_mouse_state *state)
{
    if (state == NULL) {
        return;
    }
    *state = (struct usb_hid_mouse_state){0};
    state->initialized = true;
}

/*
 * ITEM-448: the one place a mouse report becomes an event and a counter.  Both
 * wire formats - the boot report and a report-descriptor layout - end here, so
 * the button edges and the counters cannot drift apart between them.
 */
static void usb_hid_mouse_decode(
    struct usb_hid_mouse_state *state,
    uint8_t buttons,
    int16_t dx_raw,
    int16_t dy_raw,
    int16_t wheel_raw,
    struct usb_hid_mouse_event *event_out) {
    uint8_t pressed;
    uint8_t released;

    if (!state->initialized) {
        usb_hid_mouse_state_reset(state);
    }
    pressed = (uint8_t)(buttons & (uint8_t)~state->buttons);
    released = (uint8_t)(state->buttons & (uint8_t)~buttons);
    event_out->dx = (int16_t)dx_raw;
    event_out->dy = (int16_t)dy_raw;
    event_out->wheel = (int16_t)wheel_raw;
    event_out->buttons = buttons;
    event_out->pressed = pressed;
    event_out->released = released;
    state->buttons = buttons;
    ++state->reports;
    if (event_out->dx != 0 || event_out->dy != 0) {
        ++state->moves;
    }
    if (wheel_raw != 0) {
        ++state->wheels;
    }
    if (pressed != 0U) {
        ++state->presses;
    }
    if (released != 0U) {
        ++state->releases;
    }
}

bool usb_hid_boot_mouse_report(
    struct usb_hid_mouse_state *state,
    const uint8_t *report,
    uint32_t received_bytes,
    struct usb_hid_mouse_event *event_out)
{
    /*
     * ITEM-448: the boot report is three bytes; a device commonly appends the
     * wheel byte the same report is defined with in the report protocol, so a
     * four-byte transfer still carries the boot report and the extra byte is
     * not read.  Anything shorter is not a report at all: padding it would
     * move the pointer by a delta the device never sent.
     */
    if (state == NULL || report == NULL || event_out == NULL) {
        if (state != NULL) {
            ++state->refused;
        }
        return false;
    }
    if (received_bytes < USB_HID_BOOT_MOUSE_REPORT_LENGTH ||
        received_bytes > USB_HID_BOOT_MOUSE_TRANSFER_LENGTH) {
        ++state->refused;
        return false;
    }
    /*
     * ITEM-579: the boot protocol defines three bytes, and the fourth byte a
     * device appends to them is the wheel (the same field the report protocol
     * defines).  A three-byte report leaves the wheel at zero.
     */
    usb_hid_mouse_decode(state, report[0U], (int8_t)report[1U],
                         (int8_t)report[2U],
                         received_bytes >= USB_HID_BOOT_MOUSE_TRANSFER_LENGTH
                             ? (int8_t)report[3U]
                             : 0,
                         event_out);
    return true;
}

/*
 * ITEM-448: the report-descriptor walk for the pointer.
 *
 * The same item encoding as the keyboard walk above, and the same rule about
 * what counts as proof: a Generic Desktop mouse (or pointer) collection with an
 * input report this decoder can read as a pointer - byte-aligned signed
 * relative X and Y of 8 or 16 bits and a button field of at most eight
 * buttons.  Everything else is
 * refused rather than guessed at: a 12-bit axis, a button field that shares a
 * byte with another field, or a report with no X or no Y would put the pointer
 * somewhere the operator did not ask for.
 */
#define USB_HID_USAGE_PAGE_BUTTON 0x09U
#define USB_HID_USAGE_GENERIC_POINTER 0x01U
#define USB_HID_USAGE_GENERIC_MOUSE 0x02U
#define USB_HID_USAGE_GENERIC_X 0x30U
#define USB_HID_USAGE_GENERIC_Y 0x31U
#define USB_HID_USAGE_GENERIC_WHEEL 0x38U

/*
 * A main item may carry several local usages - the classic mouse descriptor
 * declares `Usage (X), Usage (Y), Report Size (8), Report Count (2)` - and the
 * report's fields follow the usages in order, so the walk keeps the list rather
 * than only the last one.  Beyond this many usages the item is not a pointer
 * this decoder claims to read.
 */
#define USB_HID_MOUSE_USAGE_MAX 8U

bool usb_hid_parse_mouse_report_descriptor(
    const uint8_t *descriptor,
    size_t length,
    struct usb_hid_mouse_layout *layout) {
    uint32_t usage_page = 0U;
    uint32_t usages[USB_HID_MOUSE_USAGE_MAX];
    uint32_t usage_count = 0U;
    uint32_t report_size = 0U;
    uint32_t report_count = 0U;
    uint32_t report_id = 0U;
    uint32_t bit_offset = 0U;
    uint32_t mouse_bits = 0U;
    uint32_t depth = 0U;
    uint32_t mouse_depth = 0U;
    bool have_report_id = false;
    bool have_buttons = false;
    bool have_dx = false;
    bool have_dy = false;
    bool have_wheel = false;
    struct usb_hid_mouse_layout found;
    size_t offset = 0U;

    for (uint32_t index = 0U; index < USB_HID_MOUSE_USAGE_MAX; ++index) {
        usages[index] = 0U;
    }

    if (layout == NULL) {
        return false;
    }
    found.report_id = 0U;
    found.report_length = 0U;
    found.buttons_offset = USB_HID_REPORT_OFFSET_NONE;
    found.buttons_mask = 0U;
    found.dx_offset = 0U;
    found.dy_offset = 0U;
    found.dx_size = 0U;
    found.dy_size = 0U;
    found.wheel_offset = USB_HID_REPORT_OFFSET_NONE;
    found.wheel_size = 0U;
    if (descriptor == NULL || length == 0U) {
        return false;
    }

    while (offset < length) {
        const uint8_t prefix = descriptor[offset];
        uint8_t item_size;
        uint8_t item_type;
        uint8_t item_tag;
        uint32_t value;

        if (prefix == 0xFEU) {
            const uint8_t data_size =
                offset + 1U < length ? descriptor[offset + 1U] : 0U;

            if (offset + 3U + (size_t)data_size > length) {
                break;
            }
            offset += 3U + (size_t)data_size;
            continue;
        }
        item_size = (uint8_t)(prefix & 0x03U);
        if (item_size == 3U) {
            item_size = 4U;
        }
        item_type = (uint8_t)((prefix >> 2U) & 0x03U);
        item_tag = (uint8_t)((prefix >> 4U) & 0x0FU);
        if (offset + 1U + (size_t)item_size > length) {
            break;
        }
        value = usb_hid_item_value(&descriptor[offset + 1U], item_size);
        offset += 1U + (size_t)item_size;

        if (item_type == USB_HID_ITEM_TYPE_GLOBAL) {
            switch (item_tag) {
            case USB_HID_GLOBAL_USAGE_PAGE:
                usage_page = value;
                break;
            case USB_HID_GLOBAL_REPORT_SIZE:
                report_size = value > 32U ? 32U : value;
                break;
            case USB_HID_GLOBAL_REPORT_ID:
                report_id = value & 0xFFU;
                have_report_id = report_id != 0U;
                /* A report ID starts a new report; once the pointer's fields
                   are complete, later report IDs describe other reports and
                   must not move them. */
                if (!have_dx || !have_dy) {
                    bit_offset = 0U;
                }
                break;
            case USB_HID_GLOBAL_REPORT_COUNT:
                report_count = value > 256U ? 256U : value;
                break;
            default:
                break;
            }
            continue;
        }
        if (item_type == USB_HID_ITEM_TYPE_LOCAL) {
            if (item_tag == USB_HID_LOCAL_USAGE &&
                usage_count < USB_HID_MOUSE_USAGE_MAX) {
                usages[usage_count] = value;
                ++usage_count;
            }
            continue;
        }
        if (item_type != USB_HID_ITEM_TYPE_MAIN) {
            continue;
        }
        if (item_tag == USB_HID_MAIN_COLLECTION) {
            ++depth;
            /*
             * The collection's own usage is the last local usage before it: a
             * reference to the application collection's type, not to a field.
             */
            if (usage_page == USB_HID_USAGE_PAGE_GENERIC_DESKTOP &&
                usage_count != 0U &&
                (usages[usage_count - 1U] == USB_HID_USAGE_GENERIC_MOUSE ||
                 usages[usage_count - 1U] == USB_HID_USAGE_GENERIC_POINTER)) {
                mouse_depth = depth;
            }
        } else if (item_tag == USB_HID_MAIN_END_COLLECTION) {
            if (mouse_depth == depth) {
                mouse_depth = 0U;
            }
            if (depth > 0U) {
                --depth;
            }
        } else if (item_tag == USB_HID_MAIN_INPUT && mouse_depth != 0U) {
            const uint32_t prefix_bytes = have_report_id ? 1U : 0U;
            const uint32_t byte_offset = bit_offset / 8U;
            const bool byte_aligned = (bit_offset % 8U) == 0U;
            const uint32_t start = prefix_bytes + byte_offset;

            if (!have_buttons && report_size == 1U &&
                report_count >= 1U &&
                report_count <= USB_HID_MOUSE_BUTTONS_MAX &&
                byte_aligned && usage_page == USB_HID_USAGE_PAGE_BUTTON &&
                start < USB_HID_REPORT_LENGTH_MAX) {
                found.buttons_offset = (uint8_t)start;
                found.buttons_mask =
                    (uint8_t)((1U << report_count) - 1U);
                have_buttons = true;
            } else if ((report_size == 8U || report_size == 16U) &&
                       byte_aligned &&
                       report_count >= 1U &&
                       usage_page == USB_HID_USAGE_PAGE_GENERIC_DESKTOP) {
                const uint32_t field_bytes = report_size / 8U;
                /*
                 * The report's fields follow the item's usages in order, so
                 * field `index` starts `index * field_bytes` bytes after the
                 * item; X and Y may be declared as two items or as one item
                 * with two usages.  ITEM-640: a signed relative 16-bit axis is
                 * the same shape with two little-endian bytes per field, and
                 * the wheel stays the signed 8-bit field the boot report and
                 * the descriptor both define.
                 */
                for (uint32_t index = 0U;
                     index < report_count &&
                     index < usage_count;
                     ++index) {
                    const uint32_t field_start =
                        start + index * field_bytes;

                    if (field_start + field_bytes >
                        USB_HID_REPORT_LENGTH_MAX) {
                        break;
                    }
                    if (!have_dx && usages[index] == USB_HID_USAGE_GENERIC_X) {
                        found.dx_offset = (uint8_t)field_start;
                        found.dx_size = (uint8_t)report_size;
                        found.report_id =
                            have_report_id ? (uint8_t)report_id : 0U;
                        have_dx = true;
                    } else if (!have_dy &&
                               usages[index] == USB_HID_USAGE_GENERIC_Y) {
                        found.dy_offset = (uint8_t)field_start;
                        found.dy_size = (uint8_t)report_size;
                        found.report_id =
                            have_report_id ? (uint8_t)report_id : 0U;
                        have_dy = true;
                    } else if (report_size == 8U && !have_wheel &&
                               usages[index] == USB_HID_USAGE_GENERIC_WHEEL) {
                        found.wheel_offset = (uint8_t)field_start;
                        found.wheel_size = 8U;
                        found.report_id =
                            have_report_id ? (uint8_t)report_id : 0U;
                        have_wheel = true;
                    }
                }
            }
        }
        if (item_type == USB_HID_ITEM_TYPE_MAIN) {
            /* Local items apply to one main item only. */
            usage_count = 0U;
            if (item_tag == USB_HID_MAIN_INPUT || item_tag == 0x9U ||
                item_tag == 0xBU) {
                bit_offset += report_size * report_count;
                if (bit_offset > 4096U) {
                    bit_offset = 4096U;
                }
                /* ITEM-640: the layout's length is the whole input report the
                   mouse collection declares, including a trailing field this
                   decoder deliberately ignores (the AC Pan byte), so the
                   transfer is never accepted as a shorter report than the
                   device sends. */
                if (item_tag == USB_HID_MAIN_INPUT && mouse_depth != 0U &&
                    (!(have_dx || have_dy) ||
                     report_id == found.report_id)) {
                    mouse_bits = bit_offset;
                }
            }
        }
    }

    if (!have_dx || !have_dy) {
        return false;
    }
    {
        const uint32_t prefix_bytes = have_report_id ? 1U : 0U;
        const uint32_t report_bytes = (mouse_bits + 7U) / 8U;

        if (mouse_bits == 0U ||
            prefix_bytes + report_bytes > USB_HID_REPORT_LENGTH_MAX) {
            return false;
        }
        found.report_length = (uint8_t)(prefix_bytes + report_bytes);
    }
    if (found.report_length == 0U ||
        found.report_length > USB_HID_REPORT_LENGTH_MAX) {
        return false;
    }
    *layout = found;
    return true;
}

/*
 * ITEM-640: one signed relative axis from the layout's own field width.  A
 * 16-bit field is little-endian on the wire, as the physical 04D9:A09F
 * descriptor declares; a value above INT16_MAX is subtracted into range rather
 * than left implementation-defined.
 */
static int16_t usb_hid_mouse_axis_value(const uint8_t *report,
                                        uint8_t offset,
                                        uint8_t size) {
    if (size == 16U) {
        int32_t value = (int32_t)report[offset] |
                        ((int32_t)report[offset + 1U] << 8);

        if ((value & 0x8000) != 0) {
            value -= 0x10000;
        }
        return (int16_t)value;
    }
    return (int16_t)(int8_t)report[offset];
}

bool usb_hid_mouse_layout_decode(
    struct usb_hid_mouse_state *state,
    const struct usb_hid_mouse_layout *layout,
    const uint8_t *report,
    uint32_t length,
    struct usb_hid_mouse_event *event_out) {
    uint8_t buttons = 0U;
    int16_t dx;
    int16_t dy;
    int16_t wheel = 0;
    const uint8_t dx_bytes =
        layout != NULL ? (uint8_t)(layout->dx_size / 8U) : 0U;
    const uint8_t dy_bytes =
        layout != NULL ? (uint8_t)(layout->dy_size / 8U) : 0U;

    if (state == NULL || layout == NULL || report == NULL ||
        event_out == NULL) {
        if (state != NULL) {
            ++state->refused;
        }
        return false;
    }
    if (layout->report_length == 0U ||
        layout->report_length > USB_HID_REPORT_LENGTH_MAX ||
        (layout->dx_size != 8U && layout->dx_size != 16U) ||
        (layout->dy_size != 8U && layout->dy_size != 16U) ||
        (layout->wheel_offset != USB_HID_REPORT_OFFSET_NONE &&
         layout->wheel_size != 8U) ||
        layout->dx_offset + dx_bytes > layout->report_length ||
        layout->dy_offset + dy_bytes > layout->report_length ||
        (layout->buttons_offset != USB_HID_REPORT_OFFSET_NONE &&
         layout->buttons_offset >= layout->report_length) ||
        (layout->wheel_offset != USB_HID_REPORT_OFFSET_NONE &&
         layout->wheel_offset >= layout->report_length)) {
        ++state->refused;
        return false;
    }
    if (length < layout->report_length) {
        ++state->refused;
        return false;
    }
    if (layout->report_id != 0U && report[0] != layout->report_id) {
        ++state->refused;
        return false;
    }
    if (layout->buttons_offset != USB_HID_REPORT_OFFSET_NONE) {
        buttons = (uint8_t)(report[layout->buttons_offset] &
                            layout->buttons_mask);
    }
    dx = usb_hid_mouse_axis_value(report, layout->dx_offset,
                                  layout->dx_size);
    dy = usb_hid_mouse_axis_value(report, layout->dy_offset,
                                  layout->dy_size);
    if (layout->wheel_offset != USB_HID_REPORT_OFFSET_NONE) {
        wheel = (int16_t)(int8_t)report[layout->wheel_offset];
    }
    usb_hid_mouse_decode(state, buttons, dx, dy, wheel, event_out);
    return true;
}

uint32_t usb_hid_mouse_state_reports(const struct usb_hid_mouse_state *state)
{
    return state != NULL ? state->reports : 0U;
}

uint32_t usb_hid_mouse_state_moves(const struct usb_hid_mouse_state *state)
{
    return state != NULL ? state->moves : 0U;
}

uint32_t usb_hid_mouse_state_presses(const struct usb_hid_mouse_state *state)
{
    return state != NULL ? state->presses : 0U;
}

uint32_t usb_hid_mouse_state_releases(const struct usb_hid_mouse_state *state)
{
    return state != NULL ? state->releases : 0U;
}

uint32_t usb_hid_mouse_state_wheels(const struct usb_hid_mouse_state *state)
{
    return state != NULL ? state->wheels : 0U;
}

uint32_t usb_hid_mouse_state_refused(const struct usb_hid_mouse_state *state)
{
    return state != NULL ? state->refused : 0U;
}

/*
 * ITEM-459: hand the most recent unmapped usage to the caller and clear it, so
 * a held key is one finding rather than one per report.
 */
bool usb_hid_take_unmapped(struct usb_hid_state *state, uint8_t *usage_out,
                           uint8_t *modifiers_out) {
    if (state == NULL || state->unmapped_usage == 0U) {
        return false;
    }
    if (usage_out != NULL) {
        *usage_out = state->unmapped_usage;
    }
    if (modifiers_out != NULL) {
        *modifiers_out = state->unmapped_modifiers;
    }
    state->unmapped_usage = 0U;
    state->unmapped_modifiers = 0U;
    return true;
}
