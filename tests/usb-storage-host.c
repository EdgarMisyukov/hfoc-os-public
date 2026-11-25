/*
 * ITEM-519 increment 1: USB mass storage BOT/SCSI, proven on the host.
 *
 * The owner's installation medium is a USB stick and the kernel cannot write
 * to it: there is no USB mass storage transport at all, so the install journal
 * the owner asked for has nowhere to go (item-519-blocker.md).  The full item
 * is a subsystem - an xHCI bulk path, the Bulk-Only Transport with its SCSI
 * command set, and a block_device adapter - and this is its transport-free
 * half, so it can be driven here without a controller, a QEMU machine or the
 * owner's hardware.
 *
 * The fake below is a scripted BOT device, not a mock of the module: it parses
 * the CBW the module sends, answers the data stage and the CSW, and can be told
 * to misbehave in each way the wire allows (short data stage, wrong residue,
 * wrong CSW signature, wrong tag, phase error, failed command with sense data,
 * a transport that stops answering).  The proof then pins:
 *
 *   * the configuration walk selects exactly the BOT interface - after other
 *     interfaces, never a UAS or HID one - and refuses the malformed shapes;
 *   * a binding issues INQUIRY, TEST UNIT READY and READ CAPACITY, and a stick
 *     that is not ready, or whose capacity is unusable, is named as such
 *     instead of being reported as working until the first read;
 *   * every CBW carries the signature, a fresh tag, the direction flag, the
 *     data length and the CDB the command needs;
 *   * a short or corrupted answer is a named refusal and, where BOT requires
 *     it, the reset recovery runs (the class reset request plus
 *     CLEAR_FEATURE(HALT) on both bulk endpoints);
 *   * the block_device view checks alignment and range and splits a request
 *     larger than one BOT command into bounded commands.
 */
#include "usb_storage.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define FAKE_MEDIA_MAX_BLOCKS 512U
#define FAKE_BLOCK_BYTES 512U
#define FAKE_SENSE_BYTES 18U

#define CBW_SIGNATURE 0x43425355U
#define CSW_SIGNATURE 0x53425355U

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static void put_le16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value & 0xFFU);
    bytes[1] = (uint8_t)((value >> 8U) & 0xFFU);
}

static uint32_t get_le32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

static void put_le32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value & 0xFFU);
    bytes[1] = (uint8_t)((value >> 8U) & 0xFFU);
    bytes[2] = (uint8_t)((value >> 16U) & 0xFFU);
    bytes[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

static uint32_t get_be32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) | (uint32_t)bytes[3];
}

static void put_be32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)((value >> 24U) & 0xFFU);
    bytes[1] = (uint8_t)((value >> 16U) & 0xFFU);
    bytes[2] = (uint8_t)((value >> 8U) & 0xFFU);
    bytes[3] = (uint8_t)(value & 0xFFU);
}

/*
 * The scripted device.  `pending` is the command whose CBW arrived and whose
 * CSW has not been sent yet; the one-shot flags fire once and clear, so a test
 * can corrupt exactly the answer it means to.
 */
struct fake_device {
    uint8_t media[FAKE_MEDIA_MAX_BLOCKS * FAKE_BLOCK_BYTES];
    uint32_t media_blocks;
    uint32_t media_block_size;
    uint32_t capacity_last_lba;
    uint32_t capacity_block_size;
    uint8_t sense_key;
    uint8_t sense_asc;
    uint8_t sense_ascq;
    /* ITEM-571: the REQUEST SENSE response code (SPC-4 4.5.1); 0x70 is a
       current-error response, and any other value must not be trusted. */
    uint8_t sense_response;
    /* ITEM-572: the caching page the fake answers, and how. */
    bool mode_sense_wce;
    bool mode_sense_fail;
    bool mode_sense_invalid;
    bool sense_fail;
    uint32_t mode_sense_actual;
    bool inquiry_fail;
    bool capacity_fail;
    bool not_ready;
    bool fail_cbw;
    bool fail_data;
    bool short_data;
    bool bad_residue;
    bool corrupt_csw_signature;
    bool corrupt_csw_tag;
    bool force_csw_status;
    uint8_t csw_status;
    bool pending;
    bool data_in;
    bool data_served;
    uint8_t opcode;
    uint32_t tag;
    uint32_t data_length;
    uint32_t lba;
    uint16_t blocks;
    uint8_t csw[USB_STORAGE_CSW_BYTES];
    /* Observations. */
    uint32_t control_count;
    uint32_t reset_requests;
    uint32_t clear_feature_requests;
    uint8_t last_reset_interface;
    uint8_t clear_feature_index[2];
    uint32_t bulk_in_count;
    uint32_t bulk_out_count;
    uint32_t transport_failures;
    uint8_t last_cbw[USB_STORAGE_CBW_BYTES];
    uint32_t last_tag;
    uint32_t cbw_count;
};

static void fake_init(struct fake_device *fake, uint32_t blocks) {
    uint32_t block;
    uint32_t offset;

    memset(fake, 0, sizeof(*fake));
    fake->media_blocks = blocks;
    fake->media_block_size = FAKE_BLOCK_BYTES;
    fake->capacity_last_lba = blocks - 1U;
    fake->capacity_block_size = FAKE_BLOCK_BYTES;
    fake->sense_key = 0x03U;
    fake->sense_asc = 0x11U;
    fake->sense_ascq = 0x00U;
    fake->sense_response = 0x70U;
    /* Default: the device has a write cache, so the flush command is issued
       and the tests that exercise its answers are meaningful.  The
       write-through case asks for WCE=0 explicitly. */
    fake->mode_sense_wce = true;
    fake->mode_sense_actual = 24U;
    for (block = 0U; block < blocks; ++block) {
        for (offset = 0U; offset < FAKE_BLOCK_BYTES; ++offset) {
            fake->media[block * FAKE_BLOCK_BYTES + offset] =
                (uint8_t)((block * 7U + offset) & 0xFFU);
        }
    }
}

static uint8_t *fake_block(struct fake_device *fake, uint32_t lba) {
    return fake->media + (size_t)lba * FAKE_BLOCK_BYTES;
}

static void fake_parse_cbw(struct fake_device *fake, const uint8_t *cbw) {
    ++fake->cbw_count;
    memcpy(fake->last_cbw, cbw, USB_STORAGE_CBW_BYTES);
    fake->tag = get_le32(cbw + 4U);
    fake->last_tag = fake->tag;
    fake->data_length = get_le32(cbw + 8U);
    fake->data_in = (cbw[12] & 0x80U) != 0U;
    fake->opcode = cbw[15];
    fake->lba = 0U;
    fake->blocks = 0U;
    if (fake->opcode == USB_SCSI_READ_10 ||
        fake->opcode == USB_SCSI_WRITE_10) {
        fake->lba = get_be32(cbw + 17U);
        fake->blocks = (uint16_t)(((uint16_t)cbw[22] << 8U) | cbw[23]);
    }
    fake->pending = true;
    fake->data_served = fake->data_length == 0U;
}

static void fake_build_csw(struct fake_device *fake) {
    uint8_t status = 0U;
    uint32_t residue = 0U;

    if (fake->force_csw_status) {
        status = fake->csw_status;
        fake->force_csw_status = false;
    } else if (fake->opcode == USB_SCSI_INQUIRY && fake->inquiry_fail) {
        status = 1U;
    } else if (fake->opcode == USB_SCSI_READ_CAPACITY_10 &&
               fake->capacity_fail) {
        status = 1U;
    } else if (fake->opcode == USB_SCSI_TEST_UNIT_READY &&
               fake->not_ready) {
        status = 1U;
    } else if (fake->opcode == USB_STORAGE_MODE_SENSE_6 &&
               fake->mode_sense_fail) {
        status = 1U;
    } else if (fake->opcode == USB_SCSI_REQUEST_SENSE && fake->sense_fail) {
        status = 1U;
    }
    if (fake->opcode == USB_STORAGE_MODE_SENSE_6 &&
        fake->mode_sense_actual < fake->data_length) {
        residue = fake->data_length - fake->mode_sense_actual;
    }
    if (fake->short_data && fake->data_in) {
        residue = 1U;
    }
    if (fake->bad_residue) {
        residue = 7U;
    }
    memset(fake->csw, 0, sizeof(fake->csw));
    put_le32(fake->csw, CSW_SIGNATURE);
    put_le32(fake->csw + 4U, fake->tag);
    put_le32(fake->csw + 8U, residue);
    fake->csw[12] = status;
    if (fake->corrupt_csw_signature) {
        put_le32(fake->csw, 0U);
        fake->corrupt_csw_signature = false;
    }
    if (fake->corrupt_csw_tag) {
        put_le32(fake->csw + 4U, fake->tag + 1U);
        fake->corrupt_csw_tag = false;
    }
}

static void fake_fill_inquiry(struct fake_device *fake, uint8_t *buffer,
                              uint32_t length) {
    static const char vendor[8] = {'H', 'F', 'O', 'C', ' ', ' ', ' ', ' '};
    static const char product[16] = {'V', 'I', 'R', 'T', 'U', 'A', 'L', ' ',
                                     'S', 'T', 'I', 'C', 'K', ' ', ' ', ' '};
    static const char revision[4] = {'1', '.', '0', ' '};

    memset(buffer, 0, length);
    buffer[0] = 0x00U;
    buffer[1] = 0x00U;
    buffer[2] = 0x05U;
    buffer[3] = 0x02U;
    buffer[4] = 31U;
    memcpy(buffer + 8U, vendor, sizeof(vendor));
    memcpy(buffer + 16U, product, sizeof(product));
    memcpy(buffer + 32U, revision, sizeof(revision));
    (void)fake;
}

static bool fake_bulk_in(void *context, void *buffer, uint32_t length,
                         uint32_t *transferred) {
    struct fake_device *fake = (struct fake_device *)context;

    ++fake->bulk_in_count;
    if (!fake->pending) {
        ++fake->transport_failures;
        return false;
    }
    if (!fake->data_served && fake->data_in) {
        if (fake->fail_data) {
            fake->fail_data = false;
            fake->pending = false;
            ++fake->transport_failures;
            return false;
        }
        if (length != fake->data_length) {
            ++fake->transport_failures;
            return false;
        }
        if (fake->opcode == USB_SCSI_INQUIRY) {
            fake_fill_inquiry(fake, (uint8_t *)buffer, length);
        } else if (fake->opcode == USB_SCSI_READ_CAPACITY_10) {
            put_be32((uint8_t *)buffer, fake->capacity_last_lba);
            put_be32((uint8_t *)buffer + 4U, fake->capacity_block_size);
        } else if (fake->opcode == USB_SCSI_REQUEST_SENSE) {
            memset(buffer, 0, length);
            ((uint8_t *)buffer)[0] = fake->sense_response;
            ((uint8_t *)buffer)[2] = fake->sense_key;
            ((uint8_t *)buffer)[12] = fake->sense_asc;
            ((uint8_t *)buffer)[13] = fake->sense_ascq;
        } else if (fake->opcode == USB_STORAGE_MODE_SENSE_6) {
            uint8_t page[24];
            const uint32_t actual =
                fake->mode_sense_actual < length ? fake->mode_sense_actual
                                                 : length;

            memset(page, 0, sizeof(page));
            if (fake->mode_sense_invalid) {
                /* The legacy "no mode sense data" shape: all ones. */
                memset(page, 0xFF, sizeof(page));
            } else {
                page[0] = 0x17U; /* mode data length, 24 bytes total */
                page[4] = USB_STORAGE_CACHING_PAGE;
                page[5] = 0x12U; /* caching page length */
                page[6] = fake->mode_sense_wce ? 0x04U : 0x00U;
            }
            memcpy(buffer, page, actual);
            fake->data_served = true;
            *transferred = actual;
            return true;
        } else if (fake->opcode == USB_SCSI_READ_10) {
            uint32_t bytes = fake->blocks * fake->media_block_size;

            if (fake->lba + fake->blocks <= fake->media_blocks) {
                memcpy(buffer, fake_block(fake, fake->lba), bytes);
            } else {
                memset(buffer, 0, bytes);
            }
        } else {
            memset(buffer, 0, length);
        }
        fake->data_served = true;
        *transferred = fake->short_data ? length - 1U : length;
        return true;
    }
    if (length != USB_STORAGE_CSW_BYTES) {
        ++fake->transport_failures;
        return false;
    }
    fake_build_csw(fake);
    memcpy(buffer, fake->csw, USB_STORAGE_CSW_BYTES);
    *transferred = USB_STORAGE_CSW_BYTES;
    fake->pending = false;
    return true;
}

static bool fake_bulk_out(void *context, const void *buffer, uint32_t length,
                          uint32_t *transferred) {
    struct fake_device *fake = (struct fake_device *)context;

    ++fake->bulk_out_count;
    if (length == USB_STORAGE_CBW_BYTES && !fake->pending) {
        if (fake->fail_cbw) {
            fake->fail_cbw = false;
            ++fake->transport_failures;
            return false;
        }
        if (get_le32((const uint8_t *)buffer) != CBW_SIGNATURE) {
            ++fake->transport_failures;
            return false;
        }
        fake_parse_cbw(fake, (const uint8_t *)buffer);
        *transferred = USB_STORAGE_CBW_BYTES;
        return true;
    }
    if (fake->pending && !fake->data_in && !fake->data_served) {
        if (fake->fail_data) {
            fake->fail_data = false;
            fake->pending = false;
            ++fake->transport_failures;
            return false;
        }
        if (length != fake->data_length) {
            ++fake->transport_failures;
            return false;
        }
        if (fake->opcode == USB_SCSI_WRITE_10 &&
            fake->lba + fake->blocks <= fake->media_blocks) {
            memcpy(fake_block(fake, fake->lba), buffer,
                   fake->blocks * fake->media_block_size);
        }
        fake->data_served = true;
        *transferred = fake->short_data ? length - 1U : length;
        return true;
    }
    ++fake->transport_failures;
    return false;
}

static bool fake_control(void *context, uint8_t request_type, uint8_t request,
                         uint16_t value, uint16_t index, uint16_t length,
                         bool data_in, void *data, uint32_t *transferred) {
    struct fake_device *fake = (struct fake_device *)context;

    ++fake->control_count;
    (void)value;
    (void)length;
    (void)data_in;
    (void)data;
    *transferred = 0U;
    if (request_type == 0x21U && request == 0xFFU) {
        ++fake->reset_requests;
        fake->last_reset_interface = (uint8_t)index;
        return true;
    }
    if (request_type == 0x02U && request == 0x01U && value == 0U) {
        if (fake->clear_feature_requests < 2U) {
            fake->clear_feature_index[fake->clear_feature_requests] =
                (uint8_t)index;
        }
        ++fake->clear_feature_requests;
        return true;
    }
    return false;
}

static struct usb_storage_transport fake_transport(
    struct fake_device *fake) {
    struct usb_storage_transport transport;

    transport.context = fake;
    transport.control = fake_control;
    transport.bulk_in = fake_bulk_in;
    transport.bulk_out = fake_bulk_out;
    return transport;
}

static enum usb_storage_status fake_bind(
    struct usb_storage *storage,
    struct fake_device *fake,
    const struct usb_storage_config *config) {
    struct usb_storage_transport transport = fake_transport(fake);

    return usb_storage_bind(storage, &transport, config);
}

/* A configuration descriptor blob built one descriptor at a time. */
struct blob {
    uint8_t bytes[512];
    size_t length;
};

static void blob_append(struct blob *blob, const uint8_t *descriptor,
                        size_t length) {
    memcpy(blob->bytes + blob->length, descriptor, length);
    blob->length += length;
}

static void blob_begin(struct blob *blob, uint16_t total, uint8_t interfaces) {
    uint8_t header[9];

    memset(blob, 0, sizeof(*blob));
    memset(header, 0, sizeof(header));
    header[0] = 9U;
    header[1] = 0x02U;
    put_le16(header + 2U, total);
    header[4] = interfaces;
    header[5] = 1U;
    blob_append(blob, header, sizeof(header));
}

static void blob_interface(struct blob *blob, uint8_t number,
                           uint8_t endpoints, uint8_t class_code,
                           uint8_t subclass, uint8_t protocol) {
    uint8_t descriptor[9];

    memset(descriptor, 0, sizeof(descriptor));
    descriptor[0] = 9U;
    descriptor[1] = 0x04U;
    descriptor[2] = number;
    descriptor[4] = endpoints;
    descriptor[5] = class_code;
    descriptor[6] = subclass;
    descriptor[7] = protocol;
    blob_append(blob, descriptor, sizeof(descriptor));
}

static void blob_endpoint(struct blob *blob, uint8_t address,
                          uint8_t attributes, uint16_t packet) {
    uint8_t descriptor[7];

    memset(descriptor, 0, sizeof(descriptor));
    descriptor[0] = 7U;
    descriptor[1] = 0x05U;
    descriptor[2] = address;
    descriptor[3] = attributes;
    put_le16(descriptor + 4U, packet);
    blob_append(blob, descriptor, sizeof(descriptor));
}

static void blob_bot_interface(struct blob *blob, uint8_t number) {
    blob_interface(blob, number, 2U, 0x08U, 0x06U, 0x50U);
    blob_endpoint(blob, 0x81U, 0x02U, 512U);
    blob_endpoint(blob, 0x02U, 0x02U, 512U);
}

/* USB 3.0 section 9.6.7: the SuperSpeed endpoint companion that follows a
   bulk endpoint and names the burst the endpoint can carry. */
static void blob_companion(struct blob *blob, uint8_t max_burst) {
    uint8_t descriptor[6];

    memset(descriptor, 0, sizeof(descriptor));
    descriptor[0] = 6U;
    descriptor[1] = 0x30U;
    descriptor[2] = max_burst;
    blob_append(blob, descriptor, sizeof(descriptor));
}

/*
 * One shared configuration for the tests that do not exercise the parser: the
 * same BOT shape the walk above selects.
 */
static void blob_check_config(struct usb_storage_config *config) {
    struct blob blob;

    blob_begin(&blob, 9U + 9U + 7U + 7U, 1U);
    blob_bot_interface(&blob, 0U);
    if (usb_storage_parse_config(blob.bytes, blob.length, config) !=
        USB_STORAGE_OK) {
        fprintf(stderr, "FAIL: the shared BOT configuration did not parse\n");
        ++failures;
    }
}

/* ------------------------------------------------------------------ parse --- */

static void test_parser_selects_the_bot_interface(void) {
    struct blob blob;
    struct usb_storage_config config;

    blob_begin(&blob, 9U + 9U + 7U + 7U, 1U);
    blob.bytes[5] = 3U;
    blob_bot_interface(&blob, 0U);
    CHECK(usb_storage_parse_config(blob.bytes, blob.length, &config) ==
              USB_STORAGE_OK,
          "the BOT interface is selected");
    CHECK(config.interface_number == 0U, "the interface number is kept");
    CHECK(config.configuration_value == 3U,
          "the configuration value SET_CONFIGURATION needs is kept");
    CHECK(config.bulk_in_endpoint == 0x81U, "the bulk IN endpoint is kept");
    CHECK(config.bulk_out_endpoint == 0x02U, "the bulk OUT endpoint is kept");
    CHECK(config.bulk_in_max_packet == 512U &&
              config.bulk_out_max_packet == 512U,
          "both endpoint packet sizes are kept");
}

static void test_parser_reads_past_other_interfaces(void) {
    struct blob blob;
    struct usb_storage_config config;

    /* A HID interface first, the storage interface second: the walk must not
       stop at the first interface and call the device "not storage". */
    blob_begin(&blob, 9U + 9U + 7U + 9U + 7U + 7U, 2U);
    blob_interface(&blob, 0U, 1U, 0x03U, 0x01U, 0x01U);
    blob_endpoint(&blob, 0x81U, 0x03U, 8U);
    blob_bot_interface(&blob, 1U);
    CHECK(usb_storage_parse_config(blob.bytes, blob.length, &config) ==
              USB_STORAGE_OK,
          "the storage interface behind a HID interface is found");
    CHECK(config.interface_number == 1U,
          "the walk kept the second interface's number");
}

static void test_parser_reads_the_superspeed_companion(void) {
    struct blob blob;
    struct usb_storage_config config;

    /*
     * The shape the owner's stick (24a9:205a) and QEMU's usb-bot both
     * declare: two 1024-byte bulk endpoints, each followed by its SuperSpeed
     * companion.  The companion's bMaxBurst is what the xHCI endpoint
     * context has to carry (xHCI 1.1 section 6.2.3.5), so the parser has to
     * read it instead of stepping over the descriptor.
     */
    blob_begin(&blob, 9U + 9U + (7U + 6U) + (7U + 6U), 1U);
    blob_interface(&blob, 0U, 2U, 0x08U, 0x06U, 0x50U);
    blob_endpoint(&blob, 0x81U, 0x02U, 1024U);
    blob_companion(&blob, 3U);
    blob_endpoint(&blob, 0x02U, 0x02U, 1024U);
    blob_companion(&blob, 3U);
    CHECK(usb_storage_parse_config(blob.bytes, blob.length, &config) ==
              USB_STORAGE_OK,
          "the SuperSpeed BOT interface parses");
    CHECK(config.bulk_in_max_packet == 1024U &&
              config.bulk_out_max_packet == 1024U,
          "the 1024-byte SuperSpeed packets are kept");
    CHECK(config.bulk_in_max_burst == 3U &&
              config.bulk_out_max_burst == 3U,
          "both companion bursts are kept");
}

static void test_parser_defaults_the_burst_without_a_companion(void) {
    struct usb_storage_config config;

    /* A high-speed BOT interface declares no companion: the burst is 0, the
       value the xHCI endpoint context must carry for such an endpoint. */
    blob_check_config(&config);
    CHECK(config.bulk_in_max_burst == 0U &&
              config.bulk_out_max_burst == 0U,
          "an interface without companions keeps the zero burst");
}

static void test_parser_keeps_the_companion_with_its_own_endpoint(void) {
    struct blob blob;
    struct usb_storage_config config;

    /*
     * A non-bulk endpoint's companion must not be mistaken for the bulk
     * pair's: the burst belongs to the endpoint the companion follows, and
     * the walk keeps it that way (USB 3.0 section 9.6.7: the companion
     * immediately follows its endpoint descriptor).
     */
    blob_begin(&blob, 9U + 9U + (7U + 6U) + (7U + 6U) + 7U, 1U);
    blob_interface(&blob, 0U, 3U, 0x08U, 0x06U, 0x50U);
    blob_endpoint(&blob, 0x81U, 0x03U, 8U); /* interrupt IN, not bulk */
    blob_companion(&blob, 7U);
    blob_endpoint(&blob, 0x82U, 0x02U, 1024U);
    blob_companion(&blob, 3U);
    blob_endpoint(&blob, 0x02U, 0x02U, 1024U);
    blob_companion(&blob, 4U);
    CHECK(usb_storage_parse_config(blob.bytes, blob.length, &config) ==
              USB_STORAGE_OK,
          "the BOT pair behind a non-bulk companion parses");
    CHECK(config.bulk_in_max_burst == 3U,
          "the bulk IN burst is its own companion's, not the neighbour's");
    CHECK(config.bulk_out_max_burst == 4U,
          "the bulk OUT burst is its own companion's as well");
}

static void test_parser_names_non_storage(void) {
    struct blob blob;
    struct usb_storage_config config;

    blob_begin(&blob, 9U + 9U + 7U, 1U);
    blob_interface(&blob, 0U, 1U, 0x03U, 0x01U, 0x01U);
    blob_endpoint(&blob, 0x81U, 0x03U, 8U);
    CHECK(usb_storage_parse_config(blob.bytes, blob.length, &config) ==
              USB_STORAGE_UNSUPPORTED_INTERFACE,
          "a HID configuration is not mass storage");

    /* UAS is mass storage, but not the protocol this module drives. */
    blob_begin(&blob, 9U + 9U + 7U + 7U, 1U);
    blob_interface(&blob, 0U, 2U, 0x08U, 0x06U, 0x62U);
    blob_endpoint(&blob, 0x81U, 0x02U, 1024U);
    blob_endpoint(&blob, 0x02U, 0x02U, 1024U);
    CHECK(usb_storage_parse_config(blob.bytes, blob.length, &config) ==
              USB_STORAGE_UNSUPPORTED_INTERFACE,
          "a UAS interface is refused by name");
}

static void test_parser_refuses_malformed_shapes(void) {
    struct blob blob;
    struct usb_storage_config config;

    /* BOT interface with only the IN endpoint. */
    blob_begin(&blob, 9U + 9U + 7U, 1U);
    blob_interface(&blob, 0U, 1U, 0x08U, 0x06U, 0x50U);
    blob_endpoint(&blob, 0x81U, 0x02U, 512U);
    CHECK(usb_storage_parse_config(blob.bytes, blob.length, &config) ==
              USB_STORAGE_MALFORMED_INTERFACE,
          "a BOT interface without a bulk OUT endpoint is refused");

    /* A descriptor that runs past the blob. */
    blob_begin(&blob, 9U + 9U + 7U + 7U, 1U);
    blob_bot_interface(&blob, 0U);
    CHECK(usb_storage_parse_config(blob.bytes, blob.length - 3U, &config) ==
              USB_STORAGE_MALFORMED_DESCRIPTOR,
          "a descriptor that runs past the received length is refused");

    /* A zero bLength would make the walk stand still. */
    blob.bytes[9] = 0U;
    CHECK(usb_storage_parse_config(blob.bytes, blob.length, &config) ==
              USB_STORAGE_MALFORMED_DESCRIPTOR,
          "a zero descriptor length is refused");

    CHECK(usb_storage_parse_config(NULL, blob.length, &config) ==
              USB_STORAGE_NULL,
          "a missing descriptor is refused");
    CHECK(usb_storage_parse_config(blob.bytes, 4U, &config) ==
              USB_STORAGE_MALFORMED_DESCRIPTOR,
          "a blob shorter than a configuration descriptor is refused");
}

/* ------------------------------------------------------------------- bind --- */

static void test_bind_reads_identity_and_capacity(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;

    fake_init(&fake, 64U);
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "a well-behaved stick binds");
    CHECK(storage.ready, "a bound stick is ready");
    CHECK(storage.block_count == 64U, "the capacity block count is kept");
    CHECK(storage.block_size == 512U, "the capacity block size is kept");
    CHECK(strcmp(storage.identity.vendor, "HFOC") == 0 &&
              strcmp(storage.identity.product, "VIRTUAL STICK") == 0 &&
              strcmp(storage.identity.revision, "1.0") == 0,
          "INQUIRY names the device");
    CHECK(storage.command_count == 4U,
          "binding issued INQUIRY, TEST UNIT READY, READ CAPACITY and the "
          "caching page's MODE SENSE");
    CHECK(storage.cache_known && storage.wce,
          "the caching page answered WCE=1 and the bind kept it");
    CHECK(fake.reset_requests == 0U,
          "a well-behaved stick needs no reset recovery");
}

static void test_bind_names_a_not_ready_device(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;

    fake_init(&fake, 64U);
    blob_check_config(&config);
    fake.not_ready = true;
    fake.sense_key = 0x02U;
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_NOT_READY,
          "a device with no medium is NOT_READY, not working");
    CHECK(storage.last_sense_key == 0x02U,
          "the device's own sense key is kept");
    CHECK(!storage.ready, "a not-ready device is not published as ready");
}

static void test_bind_refuses_an_unusable_capacity(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;

    fake_init(&fake, 64U);
    blob_check_config(&config);
    fake.capacity_block_size = 256U;
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_BAD_BLOCK_SIZE,
          "a block size below the block layer's minimum is refused");

    fake_init(&fake, 64U);
    fake.capacity_last_lba = 0xFFFFFFFFU;
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_BAD_CAPACITY,
          "the all-ones capacity answer is refused, not truncated");
}

static void test_bind_needs_a_transport(void) {
    struct usb_storage storage;
    struct usb_storage_config config;
    struct usb_storage_transport transport;

    blob_check_config(&config);
    memset(&transport, 0, sizeof(transport));
    CHECK(usb_storage_bind(&storage, &transport, &config) ==
              USB_STORAGE_BAD_TRANSPORT,
          "a transport without callbacks is refused");
    CHECK(usb_storage_bind(NULL, &transport, &config) == USB_STORAGE_NULL,
          "a missing storage state is refused");
}

/* ---------------------------------------------------------------- commands --- */

static void test_read_carries_the_command_the_wire_expects(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;
    uint8_t buffer[2U * FAKE_BLOCK_BYTES];

    fake_init(&fake, 64U);
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "the stick binds before the read");
    memset(buffer, 0, sizeof(buffer));
    CHECK(usb_storage_read(&storage, 5U, 2U, buffer) == USB_STORAGE_OK,
          "a two-block read succeeds");
    CHECK(memcmp(buffer, fake_block(&fake, 5U), sizeof(buffer)) == 0,
          "the read returns the medium's bytes");
    CHECK(storage.read_count == 1U, "the read is counted");
    CHECK(fake.last_cbw[15] == USB_SCSI_READ_10,
          "the CBW carried READ (10)");
    CHECK((fake.last_cbw[12] & 0x80U) != 0U,
          "the CBW asked for a device-to-host data stage");
    CHECK(get_le32(fake.last_cbw + 8U) == sizeof(buffer),
          "the CBW carried the data length");
    CHECK(get_be32(fake.last_cbw + 17U) == 5U &&
              fake.last_cbw[22] == 0U && fake.last_cbw[23] == 2U,
          "the CDB carried the LBA and the block count");
    CHECK(get_le32(fake.last_cbw + 4U) != 0U,
          "the CBW carried a tag");
}

static void test_write_reaches_the_medium(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;
    uint8_t written[FAKE_BLOCK_BYTES];
    uint8_t readback[FAKE_BLOCK_BYTES];

    fake_init(&fake, 64U);
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "the stick binds before the write");
    memset(written, 0xA5, sizeof(written));
    CHECK(usb_storage_write(&storage, 7U, 1U, written) == USB_STORAGE_OK,
          "a one-block write succeeds");
    CHECK(storage.write_count == 1U, "the write is counted");
    CHECK(fake.last_cbw[15] == USB_SCSI_WRITE_10,
          "the CBW carried WRITE (10)");
    CHECK((fake.last_cbw[12] & 0x80U) == 0U,
          "the CBW asked for a host-to-device data stage");
    memset(readback, 0, sizeof(readback));
    CHECK(usb_storage_read(&storage, 7U, 1U, readback) == USB_STORAGE_OK,
          "the written block reads back");
    CHECK(memcmp(readback, written, sizeof(written)) == 0,
          "the medium carries the written bytes");
}

static void test_requests_outside_the_capacity_are_refused(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;
    uint8_t buffer[FAKE_BLOCK_BYTES];

    fake_init(&fake, 64U);
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "the stick binds before the boundary checks");
    CHECK(usb_storage_read(&storage, 64U, 1U, buffer) ==
              USB_STORAGE_OUT_OF_RANGE,
          "a read past the last block is refused");
    CHECK(usb_storage_read(&storage, 63U, 2U, buffer) ==
              USB_STORAGE_OUT_OF_RANGE,
          "a read that runs past the last block is refused");
    CHECK(usb_storage_read(&storage, 0U, 0U, buffer) ==
              USB_STORAGE_INVALID_LENGTH,
          "a zero-block read is refused");
    CHECK(usb_storage_read(&storage, 0U, 1U, NULL) ==
              USB_STORAGE_INVALID_LENGTH,
          "a read without a buffer is refused");
    CHECK(usb_storage_read(&storage, 0U,
                           USB_STORAGE_MAX_BLOCKS_PER_COMMAND + 1U,
                           buffer) == USB_STORAGE_INVALID_LENGTH,
          "a read beyond one command's bound is refused");
    {
        struct usb_storage unbound;

        memset(&unbound, 0, sizeof(unbound));
        CHECK(usb_storage_read(&unbound, 0U, 1U, buffer) ==
                  USB_STORAGE_NOT_READY,
              "an unbound stick has no blocks to read");
    }
}

/* --------------------------------------------------------- wire failures --- */

static void test_short_data_stage_is_named(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;
    uint8_t buffer[FAKE_BLOCK_BYTES];

    fake_init(&fake, 64U);
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "the stick binds before the short read");
    fake.short_data = true;
    CHECK(usb_storage_read(&storage, 1U, 1U, buffer) ==
              USB_STORAGE_SHORT_TRANSFER,
          "a short data stage is a named refusal");
    CHECK(storage.last_residue == 1U,
          "the CSW residue is kept as evidence");
}

static void test_a_wrong_residue_is_a_short_transfer(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;
    uint8_t buffer[FAKE_BLOCK_BYTES];

    fake_init(&fake, 64U);
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "the stick binds before the residue case");
    fake.bad_residue = true;
    CHECK(usb_storage_read(&storage, 1U, 1U, buffer) ==
              USB_STORAGE_SHORT_TRANSFER,
          "a pass CSW with a residue is not a complete read");
    CHECK(storage.last_residue == 7U, "the residue is kept");
}

static void test_a_failed_command_keeps_the_sense_data(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;
    uint8_t buffer[FAKE_BLOCK_BYTES];

    fake_init(&fake, 64U);
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "the stick binds before the failed command");
    fake.force_csw_status = true;
    fake.csw_status = 1U;
    fake.sense_key = 0x03U;
    fake.sense_asc = 0x11U;
    CHECK(usb_storage_read(&storage, 1U, 1U, buffer) ==
              USB_STORAGE_COMMAND_FAILED,
          "a failed command is named as failed");
    CHECK(storage.last_csw_status == 1U,
          "the failed CSW status is kept after REQUEST SENSE");
    CHECK(storage.last_sense_key == 0x03U &&
              storage.last_sense_asc == 0x11U,
          "the device's own sense data is kept");
    CHECK(fake.last_cbw[15] == USB_SCSI_REQUEST_SENSE,
          "REQUEST SENSE followed the failed command");
}

static void test_phase_error_runs_the_reset_recovery(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;
    uint8_t buffer[FAKE_BLOCK_BYTES];

    fake_init(&fake, 64U);
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "the stick binds before the phase error");
    fake.force_csw_status = true;
    fake.csw_status = 2U;
    CHECK(usb_storage_read(&storage, 1U, 1U, buffer) ==
              USB_STORAGE_PHASE_ERROR,
          "a phase error is named");
    CHECK(fake.reset_requests == 1U,
          "the BOT class reset request ran");
    CHECK(fake.clear_feature_requests == 2U,
          "CLEAR_FEATURE(HALT) ran on both bulk endpoints");
    CHECK(fake.last_reset_interface == config.interface_number,
          "the reset named the bound interface");
    CHECK(storage.reset_count == 1U, "the reset is counted");
}

static void test_broken_csw_runs_the_reset_recovery(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;
    uint8_t buffer[FAKE_BLOCK_BYTES];

    fake_init(&fake, 64U);
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "the stick binds before the broken CSW");
    fake.corrupt_csw_signature = true;
    CHECK(usb_storage_read(&storage, 1U, 1U, buffer) ==
              USB_STORAGE_BAD_CSW_SIGNATURE,
          "a wrong CSW signature is named");
    CHECK(fake.reset_requests == 1U,
          "a wrong CSW signature runs the reset recovery");

    fake.corrupt_csw_tag = true;
    CHECK(usb_storage_read(&storage, 1U, 1U, buffer) ==
              USB_STORAGE_CSW_TAG_MISMATCH,
          "a CSW for another command is named");
    CHECK(fake.reset_requests == 2U,
          "a wrong tag runs the reset recovery");
}

static void test_a_transport_that_stops_answering_is_named(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;
    uint8_t buffer[FAKE_BLOCK_BYTES];

    fake_init(&fake, 64U);
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "the stick binds before the transport failure");
    fake.fail_cbw = true;
    CHECK(usb_storage_read(&storage, 1U, 1U, buffer) ==
              USB_STORAGE_TRANSPORT_ERROR,
          "a transport that does not carry the CBW is named");
    CHECK(storage.transport_error_count >= 1U,
          "the transport error is counted");
    CHECK(fake.reset_requests == 1U,
          "a broken transport runs the reset recovery");
}

/* ------------------------------------------------------------ block layer --- */

static void test_block_device_reads_and_writes(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;
    struct usb_storage_block_device backend;
    uint8_t buffer[2U * FAKE_BLOCK_BYTES];

    fake_init(&fake, 64U);
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "the stick binds before the block view");
    CHECK(usb_storage_block_device_bind(&storage, &backend) == BLOCK_DEVICE_OK,
          "the block_device view binds");
    CHECK(backend.device.block_size == 512U &&
              backend.device.block_count == 64U,
          "the block view carries the capacity");
    memset(buffer, 0, sizeof(buffer));
    CHECK(block_device_read(&backend.device, 1024U, buffer, sizeof(buffer)) ==
              BLOCK_DEVICE_OK,
          "a byte-addressed read succeeds");
    CHECK(memcmp(buffer, fake_block(&fake, 2U), sizeof(buffer)) == 0,
          "the block view returns the medium's bytes");
    memset(buffer, 0x5A, sizeof(buffer));
    CHECK(block_device_write(&backend.device, 2048U, buffer, sizeof(buffer)) ==
              BLOCK_DEVICE_OK,
          "a byte-addressed write succeeds");
    CHECK(memcmp(fake_block(&fake, 4U), buffer, sizeof(buffer)) == 0,
          "the medium carries the block view's bytes");
}

/*
 * ITEM-519 increment 4: the write proof flushes the medium through the block
 * view, so the view's flush must reach the device as the SCSI command that
 * makes the stick's own cache durable - not as a silent success on the host.
 */
static void test_block_device_flush_reaches_the_device(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;
    struct usb_storage_block_device backend;

    fake_init(&fake, 64U);
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "the stick binds before the flush");
    CHECK(usb_storage_block_device_bind(&storage, &backend) == BLOCK_DEVICE_OK,
          "the block view binds before the flush");
    CHECK(block_device_flush(&backend.device) == BLOCK_DEVICE_OK,
          "the block view flushes");
    CHECK(fake.opcode == USB_SCSI_SYNCHRONIZE_CACHE_10,
          "the flush reached the device as SYNCHRONIZE CACHE (10)");
}

static void test_block_device_checks_alignment_and_range(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;
    struct usb_storage_block_device backend;
    uint8_t buffer[2U * FAKE_BLOCK_BYTES];

    fake_init(&fake, 64U);
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "the stick binds before the alignment checks");
    CHECK(usb_storage_block_device_bind(&storage, &backend) == BLOCK_DEVICE_OK,
          "the block view binds");
    CHECK(block_device_read(&backend.device, 100U, buffer, 512U) ==
              BLOCK_DEVICE_UNALIGNED,
          "an unaligned offset is refused before the wire");
    CHECK(block_device_read(&backend.device, 0U, buffer, 100U) ==
              BLOCK_DEVICE_UNALIGNED,
          "an unaligned length is refused before the wire");
    CHECK(block_device_read(&backend.device, 64U * 512U, buffer, 512U) ==
              BLOCK_DEVICE_OUT_OF_RANGE,
          "a read past the capacity is refused");
    CHECK(block_device_read(&backend.device, 0U, NULL, 512U) ==
              BLOCK_DEVICE_NULL,
          "a read without a buffer is refused");
}

static void test_block_device_splits_a_long_request(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;
    struct usb_storage_block_device backend;
    static uint8_t buffer[(USB_STORAGE_MAX_BLOCKS_PER_COMMAND + 60U) *
                          FAKE_BLOCK_BYTES];
    uint32_t index;

    fake_init(&fake, USB_STORAGE_MAX_BLOCKS_PER_COMMAND + 60U);
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "the large stick binds");
    CHECK(usb_storage_block_device_bind(&storage, &backend) == BLOCK_DEVICE_OK,
          "the large stick's block view binds");
    memset(buffer, 0, sizeof(buffer));
    CHECK(block_device_read(&backend.device, 0U, buffer, sizeof(buffer)) ==
              BLOCK_DEVICE_OK,
          "a request longer than one BOT command succeeds");
    CHECK(storage.read_count == 2U,
          "the request was split into two bounded BOT commands");
    for (index = 0U; index < sizeof(buffer); ++index) {
        if (buffer[index] != fake.media[index]) {
            CHECK(false, "the split read returned the medium's bytes");
            return;
        }
    }
    CHECK(true, "the split read returned the medium's bytes");
}

static void test_block_device_refuses_an_unbound_stick(void) {
    struct usb_storage storage;
    struct usb_storage_block_device backend;

    memset(&storage, 0, sizeof(storage));
    CHECK(usb_storage_block_device_bind(&storage, &backend) ==
              BLOCK_DEVICE_BAD_DEVICE,
          "an unbound stick has no block view");
    CHECK(usb_storage_block_device_bind(NULL, &backend) == BLOCK_DEVICE_NULL,
          "a missing stick is refused");
}

/*
 * One shared configuration for the tests that do not exercise the parser: the
 * same BOT shape the walk above selects.
 */

/* ITEM-571: the flush refusal is the flush's, and it carries the device's own
   answer for SYNCHRONIZE CACHE - not the recovery's REQUEST SENSE. */
static void test_a_flush_failure_keeps_the_primary_command(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;
    struct usb_storage_block_device backend;
    const struct usb_storage_fault *fault;
    uint8_t buffer[FAKE_BLOCK_BYTES];
    char text[160];

    fake_init(&fake, 64U);
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "the stick binds before the flush refusal");
    CHECK(usb_storage_block_device_bind(&storage, &backend) == BLOCK_DEVICE_OK,
          "the block view binds before the flush refusal");
    memset(buffer, 0x5A, sizeof(buffer));
    CHECK(usb_storage_write(&storage, 2U, 1U, buffer) == USB_STORAGE_OK,
          "the WRITE before the flush succeeds");
    CHECK(usb_storage_last_fault(&storage) == NULL,
          "a successful write leaves no fault behind");

    /* SYNCHRONIZE CACHE answers a failed CSW; REQUEST SENSE gives the device's
       reason.  The order matters: the flush's command must be the one kept. */
    fake.force_csw_status = true;
    fake.csw_status = 1U;
    fake.sense_key = 0x03U;
    fake.sense_asc = 0x11U;
    fake.sense_ascq = 0x04U;
    CHECK(usb_storage_synchronize_cache(&storage) ==
              USB_STORAGE_COMMAND_FAILED,
          "a failed flush is named as the device's command failure");
    fault = usb_storage_last_fault(&storage);
    CHECK(fault != NULL && fault->valid,
          "the failed flush leaves a snapshot");
    CHECK(fault != NULL && fault->opcode == USB_SCSI_SYNCHRONIZE_CACHE_10,
          "the snapshot keeps the flush's opcode, not REQUEST SENSE");
    CHECK(fault != NULL && fault->operation == USB_STORAGE_FAULT_FLUSH,
          "the snapshot names the flush operation");
    CHECK(fault != NULL && fault->csw_valid && fault->csw_status == 1U,
          "the snapshot keeps the failed CSW status");
    CHECK(fault != NULL && fault->sense_attempted &&
              fault->sense_status == USB_STORAGE_OK,
          "REQUEST SENSE ran, and its own result is kept apart");
    CHECK(fault != NULL && fault->sense_valid &&
              fault->sense_response == 0x70U && fault->sense_key == 0x03U &&
              fault->sense_asc == 0x11U && fault->sense_ascq == 0x04U,
          "the device's sense key/ASC/ASCQ are kept and marked valid");
    CHECK(usb_storage_fault_text(fault, text, sizeof(text)) &&
              strstr(text, "op=35") != NULL &&
              strstr(text, "FLUSH") != NULL && strstr(text, "csw=1") != NULL &&
              strstr(text, "70-03-11-04") != NULL,
          "the rendered snapshot names the primary command and its sense");
    memset(&storage.fault, 0, sizeof(storage.fault));
    CHECK(usb_storage_test_unit_ready(&storage) == USB_STORAGE_OK,
          "a later successful command still answers");
}

/* The same snapshot for a failed WRITE: the write's opcode, not the flush's. */
static void test_a_write_failure_keeps_the_write_command(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;
    const struct usb_storage_fault *fault;
    uint8_t buffer[FAKE_BLOCK_BYTES];

    fake_init(&fake, 64U);
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "the stick binds before the write refusal");
    memset(buffer, 0x5A, sizeof(buffer));
    fake.force_csw_status = true;
    fake.csw_status = 1U;
    fake.sense_key = 0x09U;
    fake.sense_asc = 0x80U;
    fake.sense_ascq = 0x00U;
    CHECK(usb_storage_write(&storage, 3U, 1U, buffer) ==
              USB_STORAGE_COMMAND_FAILED,
          "a failed WRITE is named as the device's command failure");
    fault = usb_storage_last_fault(&storage);
    CHECK(fault != NULL && fault->opcode == USB_SCSI_WRITE_10 &&
              fault->operation == USB_STORAGE_FAULT_WRITE,
          "the snapshot keeps the WRITE's opcode and operation");
    CHECK(fault != NULL && fault->sense_valid && fault->sense_key == 0x09U,
          "the write's own sense data is kept");
}

/* A sense response that is not one SPC-4 defines is not trusted: the key,
   ASC and ASCQ stay out of the record rather than being dressed as data. */
static void test_an_untrustworthy_sense_response_is_not_trusted(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;
    const struct usb_storage_fault *fault;
    uint8_t buffer[FAKE_BLOCK_BYTES];

    fake_init(&fake, 64U);
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "the stick binds before the unusable sense");
    fake.force_csw_status = true;
    fake.csw_status = 1U;
    fake.sense_response = 0x00U;
    memset(buffer, 0, sizeof(buffer));
    CHECK(usb_storage_read(&storage, 0U, 1U, buffer) ==
              USB_STORAGE_COMMAND_FAILED,
          "the device's failed command is still a command failure");
    fault = usb_storage_last_fault(&storage);
    CHECK(fault != NULL && fault->sense_attempted && !fault->sense_valid,
          "a response code that is neither 0x70 nor 0x71 is not trusted");
}

/* The block adapter reports the flush failure as an I/O error - the device
   answered - never as "unsupported". */
static void test_the_block_view_reports_the_flush_failure(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;
    struct usb_storage_block_device backend;

    fake_init(&fake, 64U);
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "the stick binds before the block flush refusal");
    CHECK(usb_storage_block_device_bind(&storage, &backend) == BLOCK_DEVICE_OK,
          "the block view binds before the block flush refusal");
    fake.force_csw_status = true;
    fake.csw_status = 1U;
    CHECK(block_device_flush(&backend.device) == BLOCK_DEVICE_IO_ERROR,
          "a failed flush is an I/O error, not an unsupported command");
}

static void test_a_clean_flush_leaves_no_fault(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;
    uint8_t buffer[FAKE_BLOCK_BYTES];

    fake_init(&fake, 64U);
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "the stick binds before the clean flush");
    memset(buffer, 0x11, sizeof(buffer));
    CHECK(usb_storage_write(&storage, 1U, 1U, buffer) == USB_STORAGE_OK,
          "a clean WRITE succeeds");
    CHECK(usb_storage_synchronize_cache(&storage) == USB_STORAGE_OK,
          "a clean flush succeeds");
    CHECK(usb_storage_last_fault(&storage) == NULL,
          "the success path leaves no fault snapshot");
}


/* ITEM-572: the flush policy.
 *
 * The machine's stick answers MODE SENSE with WCE=0 and rejects SYNCHRONIZE
 * CACHE as an invalid command.  A write-through device has nothing to flush, so
 * the command is not sent; a device whose cache page is unreadable keeps an
 * unknown cache state (not WCE=0) and its rejected flush is accepted under the
 * documented weaker guarantee; a device with WCE=1 that rejects the flush is a
 * real error; and every other failure stays the error it is. */
static void test_a_write_through_device_is_never_asked_to_flush(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;
    uint32_t commands;

    fake_init(&fake, 64U);
    fake.mode_sense_wce = false;
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "the stick binds with its caching page answered");
    CHECK(storage.cache_known && !storage.wce,
          "MODE SENSE says WCE=0 and the bind keeps it");
    commands = fake.cbw_count;
    CHECK(usb_storage_synchronize_cache(&storage) == USB_STORAGE_OK,
          "a write-through device's flush reports success");
    CHECK(storage.flush_policy == USB_STORAGE_FLUSH_WRITE_THROUGH,
          "the policy is WRITE_THROUGH");
    CHECK(fake.cbw_count == commands,
          "no SYNCHRONIZE CACHE command is sent to a write-through device");
}

static void test_an_unsupported_flush_with_unknown_cache_is_documented(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;
    const struct usb_storage_fault *fault;

    fake_init(&fake, 64U);
    fake.mode_sense_fail = true;
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "a device whose caching page does not answer still binds");
    CHECK(!storage.cache_known && !storage.wce,
          "an absent caching page is UNKNOWN, never WCE=0");
    fake.force_csw_status = true;
    fake.csw_status = 1U;
    fake.sense_key = 0x05U;
    fake.sense_asc = 0x20U;
    fake.sense_ascq = 0x00U;
    CHECK(usb_storage_synchronize_cache(&storage) == USB_STORAGE_OK,
          "the exact invalid-command answer is accepted for an unknown cache");
    CHECK(storage.flush_policy ==
              USB_STORAGE_FLUSH_UNSUPPORTED_CACHE_UNKNOWN,
          "the policy is the documented unknown-cache case");
    fault = usb_storage_last_fault(&storage);
    CHECK(fault != NULL && fault->opcode == USB_SCSI_SYNCHRONIZE_CACHE_10 &&
              fault->sense_valid && fault->sense_key == 0x05U &&
              fault->sense_asc == 0x20U && fault->sense_ascq == 0x00U,
          "the device's own answer is kept for the record");
}

static void test_an_unsupported_flush_with_enabled_cache_is_an_error(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;

    fake_init(&fake, 64U);
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "the cache-enabled stick binds");
    CHECK(storage.cache_known && storage.wce,
          "MODE SENSE says WCE=1");
    fake.force_csw_status = true;
    fake.csw_status = 1U;
    fake.sense_key = 0x05U;
    fake.sense_asc = 0x20U;
    fake.sense_ascq = 0x00U;
    CHECK(usb_storage_synchronize_cache(&storage) ==
              USB_STORAGE_FLUSH_UNSUPPORTED,
          "a cache-enabled device that cannot flush is a real error");
    CHECK(storage.flush_policy == USB_STORAGE_FLUSH_FAILED,
          "the policy is FAILED, not an acceptance");
}

static void test_another_sense_leaves_the_flush_an_error(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;

    fake_init(&fake, 64U);
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "the stick binds before a different sense answer");
    fake.force_csw_status = true;
    fake.csw_status = 1U;
    fake.sense_key = 0x04U;
    fake.sense_asc = 0x08U;
    fake.sense_ascq = 0x03U;
    CHECK(usb_storage_synchronize_cache(&storage) ==
              USB_STORAGE_COMMAND_FAILED,
          "a different sense stays a command failure, not an unsupported "
          "flush");
    CHECK(storage.flush_policy == USB_STORAGE_FLUSH_FAILED,
          "a different sense is not accepted by the policy");
}

static void test_a_failed_sense_leaves_the_flush_an_error(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;

    fake_init(&fake, 64U);
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "the stick binds before the unusable sense");
    fake.force_csw_status = true;
    fake.csw_status = 1U;
    fake.sense_fail = true;
    CHECK(usb_storage_synchronize_cache(&storage) ==
              USB_STORAGE_COMMAND_FAILED,
          "a flush whose REQUEST SENSE failed is still an error");
    CHECK(storage.flush_policy == USB_STORAGE_FLUSH_FAILED,
          "no success is invented from a failed sense");
}

static void test_a_supported_flush_still_returns_ok(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;

    fake_init(&fake, 64U);
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "the stick binds before the supported flush");
    CHECK(usb_storage_synchronize_cache(&storage) == USB_STORAGE_OK,
          "a device that supports the flush answers it");
    CHECK(storage.flush_policy == USB_STORAGE_FLUSH_SENT,
          "the policy is SENT, not a policy bypass");
    CHECK(fake.opcode == USB_SCSI_SYNCHRONIZE_CACHE_10,
          "the flush reached the device as SYNCHRONIZE CACHE (10)");
}

static void test_an_unreadable_caching_page_is_unknown(void) {
    struct fake_device fake;
    struct usb_storage storage;
    struct usb_storage_config config;

    fake_init(&fake, 64U);
    fake.mode_sense_invalid = true;
    blob_check_config(&config);
    CHECK(fake_bind(&storage, &fake, &config) == USB_STORAGE_OK,
          "the stick binds with an all-ones caching page");
    CHECK(!storage.cache_known,
          "an unusable caching page is UNKNOWN, not WCE=0");
}

int main(void) {
    test_parser_selects_the_bot_interface();
    test_parser_reads_past_other_interfaces();
    test_parser_reads_the_superspeed_companion();
    test_parser_defaults_the_burst_without_a_companion();
    test_parser_keeps_the_companion_with_its_own_endpoint();
    test_parser_names_non_storage();
    test_parser_refuses_malformed_shapes();
    test_bind_reads_identity_and_capacity();
    test_bind_names_a_not_ready_device();
    test_bind_refuses_an_unusable_capacity();
    test_bind_needs_a_transport();
    test_read_carries_the_command_the_wire_expects();
    test_write_reaches_the_medium();
    test_requests_outside_the_capacity_are_refused();
    test_short_data_stage_is_named();
    test_a_wrong_residue_is_a_short_transfer();
    test_a_failed_command_keeps_the_sense_data();
    test_phase_error_runs_the_reset_recovery();
    test_broken_csw_runs_the_reset_recovery();
    test_a_transport_that_stops_answering_is_named();
    test_block_device_reads_and_writes();
    test_block_device_flush_reaches_the_device();
    test_a_flush_failure_keeps_the_primary_command();
    test_a_write_failure_keeps_the_write_command();
    test_an_untrustworthy_sense_response_is_not_trusted();
    test_the_block_view_reports_the_flush_failure();
    test_a_clean_flush_leaves_no_fault();
    test_a_write_through_device_is_never_asked_to_flush();
    test_an_unsupported_flush_with_unknown_cache_is_documented();
    test_an_unsupported_flush_with_enabled_cache_is_an_error();
    test_another_sense_leaves_the_flush_an_error();
    test_a_failed_sense_leaves_the_flush_an_error();
    test_a_supported_flush_still_returns_ok();
    test_an_unreadable_caching_page_is_unknown();
    test_block_device_checks_alignment_and_range();
    test_block_device_splits_a_long_request();
    test_block_device_refuses_an_unbound_stick();

    if (failures != 0) {
        fprintf(stderr, "USB storage host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("USB storage host tests: ALL OK");
    return 0;
}
