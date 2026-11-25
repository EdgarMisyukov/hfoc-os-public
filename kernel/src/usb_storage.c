#include "usb_storage.h"

/*
 * ITEM-519 increment 1: USB mass storage, the transport-free half.
 *
 * The shape of the module follows the wire protocol exactly, because that is
 * what makes it provable without a controller:
 *
 *   * `usb_storage_parse_config` is a pure walk over a configuration
 *     descriptor blob - no state, no transport, bounded by the blob it is
 *     handed;
 *   * `usb_storage_execute` is the BOT state machine (CBW, optional data
 *     stage, CSW) over the caller's two callbacks;
 *   * the SCSI commands (INQUIRY, TEST UNIT READY, REQUEST SENSE, READ
 *     CAPACITY (10), READ (10), WRITE (10), SYNCHRONIZE CACHE (10)) are thin
 *     CDB builders on top of it;
 *   * `usb_storage_block_device_bind` is the adapter to the block layer.
 *
 * Every failure the wire can produce has its own token and its own repair:
 * a phase error or a broken CSW runs the BOT reset recovery, a failed command
 * captures the device's own sense data, and a short data stage is a named
 * refusal rather than bytes the caller might mistake for a complete read.
 * Nothing here allocates, spins or touches a register; the xHCI bulk path
 * (increment 2) supplies the callbacks.
 */

/* The CBW and CSW field values the transport and the device agree on. */
#define USB_STORAGE_CBW_FLAGS_IN 0x80U
#define USB_STORAGE_CSW_STATUS_PASS 0U
#define USB_STORAGE_CSW_STATUS_FAILED 1U
#define USB_STORAGE_CSW_STATUS_PHASE 2U

/* bmRequestType and request for CLEAR_FEATURE(ENDPOINT_HALT). */
#define USB_STORAGE_REQUEST_CLEAR_FEATURE 0x01U
#define USB_STORAGE_FEATURE_ENDPOINT_HALT 0x00U

/* One SCSI CDB is at most sixteen bytes (a 10-byte command is ten). */
#define USB_STORAGE_CDB_MAX 16U

/* The sense key that means "the medium is not ready yet" (SPC-4). */
#define USB_SCSI_SENSE_NOT_READY 0x02U

static void usb_storage_zero(void *buffer, size_t length) {
    uint8_t *bytes = (uint8_t *)buffer;

    while (length > 0U) {
        *bytes = 0U;
        ++bytes;
        --length;
    }
}

static void usb_storage_copy(void *target, const void *source, size_t length) {
    uint8_t *to = (uint8_t *)target;
    const uint8_t *from = (const uint8_t *)source;

    while (length > 0U) {
        *to = *from;
        ++to;
        ++from;
        --length;
    }
}

/*
 * The descriptor fields are little-endian (USB 2.0 section 9.6) and the SCSI
 * parameter data is big-endian (SPC-4); keeping the two families apart here is
 * what keeps a caller from reading a capacity in the wrong order.
 */
static uint16_t usb_storage_le16(const uint8_t *bytes) {
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U));
}

static uint32_t usb_storage_le32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

static void usb_storage_write_le32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value & 0xFFU);
    bytes[1] = (uint8_t)((value >> 8U) & 0xFFU);
    bytes[2] = (uint8_t)((value >> 16U) & 0xFFU);
    bytes[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

static uint32_t usb_storage_be32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) | (uint32_t)bytes[3];
}

static void usb_storage_write_be32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)((value >> 24U) & 0xFFU);
    bytes[1] = (uint8_t)((value >> 16U) & 0xFFU);
    bytes[2] = (uint8_t)((value >> 8U) & 0xFFU);
    bytes[3] = (uint8_t)(value & 0xFFU);
}

static void usb_storage_write_be16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)((value >> 8U) & 0xFFU);
    bytes[1] = (uint8_t)(value & 0xFFU);
}

const struct usb_storage_fault *usb_storage_last_fault(
    const struct usb_storage *storage) {
    if (storage == NULL || !storage->fault.valid) {
        return NULL;
    }
    return &storage->fault;
}

/* Append one bounded field; a field that does not fit is not started. */
static size_t fault_append(char *buffer, size_t size, size_t length,
                           const char *text) {
    size_t needed = 0U;

    if (buffer == NULL || text == NULL) {
        return length;
    }
    while (text[needed] != '\0') {
        ++needed;
    }
    if (length + needed + 1U > size) {
        return length;
    }
    for (size_t index = 0U; index < needed; ++index) {
        buffer[length + index] = text[index];
    }
    length += needed;
    buffer[length] = '\0';
    return length;
}

static size_t fault_append_hex(char *buffer, size_t size, size_t length,
                               uint32_t value, uint32_t digits) {
    static const char hex[] = "0123456789ABCDEF";
    char text[10];
    size_t needed = 0U;

    if (digits < 1U || digits > 8U) {
        digits = 2U;
    }
    needed = digits;
    for (uint32_t index = 0U; index < digits; ++index) {
        text[index] = hex[(value >> (4U * (digits - 1U - index))) & 0x0FU];
    }
    text[needed] = '\0';
    return fault_append(buffer, size, length, text);
}

static const char *fault_operation_string(
    enum usb_storage_fault_operation operation) {
    switch (operation) {
    case USB_STORAGE_FAULT_READ:
        return "READ";
    case USB_STORAGE_FAULT_WRITE:
        return "WRITE";
    case USB_STORAGE_FAULT_FLUSH:
        return "FLUSH";
    case USB_STORAGE_FAULT_OTHER:
        return "OTHER";
    case USB_STORAGE_FAULT_NONE:
        break;
    }
    return "NONE";
}

bool usb_storage_fault_text(const struct usb_storage_fault *fault,
                            char *buffer,
                            size_t size) {
    size_t length = 0U;

    if (buffer == NULL || size == 0U) {
        return false;
    }
    buffer[0] = '\0';
    if (fault == NULL || !fault->valid) {
        return false;
    }
    length = fault_append(buffer, size, length, "op=");
    length = fault_append_hex(buffer, size, length, fault->opcode, 2U);
    length = fault_append(buffer, size, length, "/");
    length = fault_append(buffer, size, length,
                          usb_storage_status_string(fault->status));
    length = fault_append(buffer, size, length, " ");
    length = fault_append(buffer, size, length,
                          fault_operation_string(fault->operation));
    length = fault_append(buffer, size, length, " csw=");
    if (fault->csw_valid) {
        length = fault_append_hex(buffer, size, length, fault->csw_status, 1U);
    } else {
        length = fault_append(buffer, size, length, "NONE");
    }
    length = fault_append(buffer, size, length, " res=");
    length = fault_append_hex(buffer, size, length, fault->residue, 8U);
    length = fault_append(buffer, size, length, " sense=");
    if (!fault->sense_attempted) {
        length = fault_append(buffer, size, length, "NONE");
    } else {
        length = fault_append(buffer, size, length,
                              usb_storage_status_string(fault->sense_status));
        length = fault_append(buffer, size, length, "/");
        if (fault->sense_valid) {
            length = fault_append_hex(buffer, size, length,
                                      fault->sense_response, 2U);
            length = fault_append(buffer, size, length, "-");
            length = fault_append_hex(buffer, size, length, fault->sense_key,
                                      2U);
            length = fault_append(buffer, size, length, "-");
            length = fault_append_hex(buffer, size, length, fault->sense_asc,
                                      2U);
            length = fault_append(buffer, size, length, "-");
            length = fault_append_hex(buffer, size, length, fault->sense_ascq,
                                      2U);
        } else {
            length = fault_append(buffer, size, length, "INVALID");
        }
    }
    return length != 0U;
}

const char *usb_storage_status_string(enum usb_storage_status status) {
    switch (status) {
    case USB_STORAGE_OK:
        return "OK";
    case USB_STORAGE_NULL:
        return "NULL";
    case USB_STORAGE_BAD_TRANSPORT:
        return "BAD_TRANSPORT";
    case USB_STORAGE_MALFORMED_DESCRIPTOR:
        return "MALFORMED_DESCRIPTOR";
    case USB_STORAGE_UNSUPPORTED_INTERFACE:
        return "UNSUPPORTED_INTERFACE";
    case USB_STORAGE_MALFORMED_INTERFACE:
        return "MALFORMED_INTERFACE";
    case USB_STORAGE_NOT_READY:
        return "NOT_READY";
    case USB_STORAGE_TRANSPORT_ERROR:
        return "TRANSPORT_ERROR";
    case USB_STORAGE_SHORT_TRANSFER:
        return "SHORT_TRANSFER";
    case USB_STORAGE_BAD_CSW_SIGNATURE:
        return "BAD_CSW_SIGNATURE";
    case USB_STORAGE_CSW_TAG_MISMATCH:
        return "CSW_TAG_MISMATCH";
    case USB_STORAGE_PHASE_ERROR:
        return "PHASE_ERROR";
    case USB_STORAGE_COMMAND_FAILED:
        return "COMMAND_FAILED";
    case USB_STORAGE_FLUSH_UNSUPPORTED:
        return "FLUSH_UNSUPPORTED";
    case USB_STORAGE_SENSE_FAILED:
        return "SENSE_FAILED";
    case USB_STORAGE_BAD_BLOCK_SIZE:
        return "BAD_BLOCK_SIZE";
    case USB_STORAGE_BAD_CAPACITY:
        return "BAD_CAPACITY";
    case USB_STORAGE_OUT_OF_RANGE:
        return "OUT_OF_RANGE";
    case USB_STORAGE_BAD_ALIGNMENT:
        return "BAD_ALIGNMENT";
    case USB_STORAGE_INVALID_LENGTH:
        return "INVALID_LENGTH";
    case USB_STORAGE_UNKNOWN:
        break;
    }
    return "UNKNOWN";
}

/*
 * The configuration walk.  The blob is what a GET_DESCRIPTOR(CONFIGURATION)
 * control transfer returned, so `length` is the authoritative end and the
 * wTotalLength field is only a hint a malformed device can lie about: the walk
 * ends at `length` and at each descriptor's own bLength, and any descriptor
 * that runs past either is MALFORMED_DESCRIPTOR, never a read past the buffer.
 *
 * Only the BOT shape is selected (class 8, subclass 6, protocol 0x50, setting
 * 0).  A device that declares mass storage through UAS answers
 * UNSUPPORTED_INTERFACE by name; this module does not guess at a protocol the
 * kernel has no transport for.
 */
enum usb_storage_status usb_storage_parse_config(
    const void *descriptor,
    size_t length,
    struct usb_storage_config *config) {
    const uint8_t *bytes = (const uint8_t *)descriptor;
    size_t offset = 0U;

    if (descriptor == NULL || config == NULL) {
        return USB_STORAGE_NULL;
    }
    if (length < 9U) {
        return USB_STORAGE_MALFORMED_DESCRIPTOR;
    }
    /* The walk starts at the configuration descriptor itself (type 2). */
    if (bytes[1] != 0x02U) {
        return USB_STORAGE_MALFORMED_DESCRIPTOR;
    }
    while (offset + 2U <= length) {
        const uint8_t descriptor_length = bytes[offset];
        const uint8_t descriptor_type = bytes[offset + 1U];

        if (descriptor_length < 2U ||
            descriptor_length > length - offset) {
            return USB_STORAGE_MALFORMED_DESCRIPTOR;
        }
        if (descriptor_type == 0x04U) {
            struct usb_storage_config found;
            size_t endpoint_offset;
            bool bulk_in = false;
            bool bulk_out = false;
            /* ITEM-545: which bulk endpoint the next SuperSpeed companion
               belongs to (USB 3.0 section 9.6.7: the companion immediately
               follows its endpoint descriptor).  0 = none, 1 = IN, 2 = OUT. */
            uint8_t companion_target = 0U;

            /* An interface descriptor is nine bytes (USB 2.0 table 9-12). */
            if (descriptor_length < 9U) {
                return USB_STORAGE_MALFORMED_DESCRIPTOR;
            }
            if (bytes[offset + 3U] != 0U ||
                bytes[offset + 5U] != USB_STORAGE_INTERFACE_CLASS ||
                bytes[offset + 6U] != USB_STORAGE_INTERFACE_SUBCLASS ||
                bytes[offset + 7U] != USB_STORAGE_INTERFACE_PROTOCOL_BOT) {
                offset += descriptor_length;
                continue;
            }
            usb_storage_zero(&found, sizeof(found));
            found.configuration_value = bytes[5];
            found.interface_number = bytes[offset + 2U];
            /*
             * Endpoints follow their interface until the next interface
             * descriptor, wherever the next one is in the blob (another
             * alternate setting, another interface, another class).
             */
            endpoint_offset = offset + descriptor_length;
            while (endpoint_offset + 2U <= length) {
                const uint8_t endpoint_length = bytes[endpoint_offset];
                const uint8_t endpoint_type = bytes[endpoint_offset + 1U];

                if (endpoint_length < 2U ||
                    endpoint_length > length - endpoint_offset) {
                    return USB_STORAGE_MALFORMED_DESCRIPTOR;
                }
                if (endpoint_type == 0x04U) {
                    break;
                }
                if (endpoint_type == 0x05U) {
                    uint8_t address;
                    uint16_t packet;

                    /* An endpoint descriptor is seven bytes (table 9-13). */
                    if (endpoint_length < 7U) {
                        return USB_STORAGE_MALFORMED_DESCRIPTOR;
                    }
                    address = bytes[endpoint_offset + 2U];
                    companion_target = 0U;
                    /* Only bulk endpoints (transfer type 2) carry BOT. */
                    if ((bytes[endpoint_offset + 3U] & 0x03U) == 0x02U) {
                        packet = (uint16_t)(usb_storage_le16(
                                                bytes + endpoint_offset + 4U) &
                                            0x7FFU);
                        if (packet != 0U) {
                            if ((address & 0x80U) != 0U) {
                                found.bulk_in_endpoint = address;
                                found.bulk_in_max_packet = packet;
                                bulk_in = true;
                                companion_target = 1U;
                            } else {
                                found.bulk_out_endpoint = address;
                                found.bulk_out_max_packet = packet;
                                bulk_out = true;
                                companion_target = 2U;
                            }
                        }
                    }
                } else if (endpoint_type == 0x30U) {
                    /*
                     * ITEM-545: the SuperSpeed endpoint companion that
                     * immediately follows its bulk endpoint, carrying the
                     * burst in bMaxBurst (USB 3.0 section 9.6.7).  A
                     * companion of any other endpoint, or one too short to
                     * carry the byte, leaves the burst at its zero default.
                     */
                    if (companion_target == 1U && endpoint_length >= 6U) {
                        found.bulk_in_max_burst = bytes[endpoint_offset + 2U];
                    } else if (companion_target == 2U &&
                               endpoint_length >= 6U) {
                        found.bulk_out_max_burst = bytes[endpoint_offset + 2U];
                    }
                    companion_target = 0U;
                } else {
                    companion_target = 0U;
                }
                endpoint_offset += endpoint_length;
            }
            if (!bulk_in || !bulk_out) {
                return USB_STORAGE_MALFORMED_INTERFACE;
            }
            *config = found;
            return USB_STORAGE_OK;
        }
        offset += descriptor_length;
    }
    return USB_STORAGE_UNSUPPORTED_INTERFACE;
}

/*
 * REQUEST SENSE after a failed command (SCSI SPC-4 6.28): the device's own
 * reason, kept in the state so a record can name it.  This runs its own CBW and
 * is therefore called with the recovery disabled - a sense request that fails
 * must not issue another sense request.
 */
static enum usb_storage_status usb_storage_execute(
    struct usb_storage *storage,
    const uint8_t *cb,
    uint8_t cb_length,
    void *data,
    uint32_t length,
    bool data_in,
    uint32_t *transferred,
    bool allow_recovery);

/*
 * ITEM-571: keep the primary command's own answer before the recovery runs.
 * The fault is the caller's (the block adapter, the record and the diag line
 * read it); REQUEST SENSE only fills the sense fields of the same snapshot and
 * never replaces the command that failed.
 */
static void usb_storage_record_fault(struct usb_storage *storage,
                                     enum usb_storage_status status,
                                     const uint8_t *cb,
                                     uint8_t cb_length,
                                     bool csw_valid,
                                     uint8_t csw_status,
                                     uint32_t residue) {
    struct usb_storage_fault *fault = &storage->fault;

    *fault = (struct usb_storage_fault){0};
    fault->valid = true;
    fault->operation = storage->fault_operation;
    fault->status = status;
    if (cb != NULL && cb_length != 0U) {
        const uint8_t copy = cb_length <= (uint8_t)sizeof(fault->cdb)
                                 ? cb_length
                                 : (uint8_t)sizeof(fault->cdb);

        fault->opcode = cb[0];
        fault->cdb_length = copy;
        for (uint8_t index = 0U; index < copy; ++index) {
            fault->cdb[index] = cb[index];
        }
    }
    fault->csw_valid = csw_valid;
    fault->csw_status = csw_status;
    fault->residue = residue;
}

static enum usb_storage_status usb_storage_request_sense(
    struct usb_storage *storage) {
    uint8_t cb[10];
    uint32_t moved = 0U;
    enum usb_storage_status status;

    usb_storage_zero(cb, sizeof(cb));
    cb[0] = USB_SCSI_REQUEST_SENSE;
    cb[4] = USB_STORAGE_SENSE_BYTES;
    status = usb_storage_execute(storage, cb, 6U, storage->sense,
                                 USB_STORAGE_SENSE_BYTES, true, &moved,
                                 false);
    if (status != USB_STORAGE_OK) {
        return USB_STORAGE_SENSE_FAILED;
    }
    storage->last_sense_response = (uint8_t)(storage->sense[0] & 0x7FU);
    /* SPC-4: the response code is 0x70 (current) or 0x71 (deferred); any other
       value means the bytes are not a sense response and the key/ASC/ASCQ in
       them must not be presented as one. */
    storage->last_sense_valid = storage->last_sense_response == 0x70U ||
                                storage->last_sense_response == 0x71U;
    storage->last_sense_key = (uint8_t)(storage->sense[2] & 0x0FU);
    storage->last_sense_asc = storage->sense[12];
    storage->last_sense_ascq = storage->sense[13];
    return USB_STORAGE_OK;
}

/*
 * The BOT reset recovery (Bulk-Only Transport 1.0 section 3.1): the
 * class-specific reset request, then CLEAR_FEATURE(HALT) on both bulk
 * endpoints.  It is what a phase error, a broken CSW and a failed bulk
 * transfer all end with, because after any of those the host and the device no
 * longer agree on where in the sequence they are.
 */
enum usb_storage_status usb_storage_reset(struct usb_storage *storage) {
    uint32_t moved = 0U;
    uint8_t endpoints[2];
    size_t index;

    if (storage == NULL) {
        return USB_STORAGE_NULL;
    }
    if (storage->transport.control == NULL ||
        storage->transport.bulk_in == NULL ||
        storage->transport.bulk_out == NULL) {
        return USB_STORAGE_BAD_TRANSPORT;
    }
    ++storage->reset_count;
    if (!storage->transport.control(
            storage->transport.context,
            USB_STORAGE_REQUEST_TYPE_CLASS_INTERFACE,
            USB_STORAGE_REQUEST_RESET, 0U,
            storage->config.interface_number, 0U, false, NULL, &moved)) {
        ++storage->transport_error_count;
        return USB_STORAGE_TRANSPORT_ERROR;
    }
    endpoints[0] = storage->config.bulk_in_endpoint;
    endpoints[1] = storage->config.bulk_out_endpoint;
    for (index = 0U; index < 2U; ++index) {
        if (!storage->transport.control(
                storage->transport.context,
                USB_STORAGE_REQUEST_TYPE_STANDARD_OUT,
                USB_STORAGE_REQUEST_CLEAR_FEATURE,
                USB_STORAGE_FEATURE_ENDPOINT_HALT, endpoints[index], 0U,
                false, NULL, &moved)) {
            ++storage->transport_error_count;
            return USB_STORAGE_TRANSPORT_ERROR;
        }
    }
    return USB_STORAGE_OK;
}

/*
 * One BOT command: CBW out, optional data stage, CSW in.
 *
 * The CSW is read even when the data stage failed, because that is the only
 * way the transport returns to a defined state; a device that never answers
 * the CSW leaves the sequence broken and the recovery runs.  `allow_recovery`
 * is false for the recovery's own REQUEST SENSE, so a failing device cannot
 * make the driver recurse.
 */
static enum usb_storage_status usb_storage_execute(
    struct usb_storage *storage,
    const uint8_t *cb,
    uint8_t cb_length,
    void *data,
    uint32_t length,
    bool data_in,
    uint32_t *transferred,
    bool allow_recovery) {
    uint8_t *cbw;
    uint32_t moved = 0U;
    uint32_t tag;
    uint32_t residue;
    uint8_t csw_status;
    uint32_t data_moved = 0U;
    bool short_stage = false;

    if (transferred != NULL) {
        *transferred = 0U;
    }
    if (storage == NULL || cb == NULL) {
        return USB_STORAGE_NULL;
    }
    if (storage->transport.control == NULL ||
        storage->transport.bulk_in == NULL ||
        storage->transport.bulk_out == NULL) {
        return USB_STORAGE_BAD_TRANSPORT;
    }
    if (cb_length == 0U || cb_length > USB_STORAGE_CDB_MAX) {
        return USB_STORAGE_UNKNOWN;
    }
    ++storage->command_count;
    storage->tag += 1U;
    if (storage->tag == 0U) {
        storage->tag = 1U;
    }
    tag = storage->tag;

    /* Command Block Wrapper (BOT 1.0 section 3.1, table 1). */
    cbw = storage->cbw;
    usb_storage_zero(cbw, USB_STORAGE_CBW_BYTES);
    usb_storage_write_le32(cbw, USB_STORAGE_CBW_SIGNATURE);
    usb_storage_write_le32(cbw + 4U, tag);
    usb_storage_write_le32(cbw + 8U, length);
    cbw[12] = data_in ? USB_STORAGE_CBW_FLAGS_IN : 0U;
    cbw[13] = 0U;
    cbw[14] = cb_length;
    usb_storage_copy(cbw + 15U, cb, cb_length);
    if (!storage->transport.bulk_out(storage->transport.context, cbw,
                                     USB_STORAGE_CBW_BYTES, &moved) ||
        moved != USB_STORAGE_CBW_BYTES) {
        ++storage->transport_error_count;
        if (allow_recovery) {
            usb_storage_record_fault(storage, USB_STORAGE_TRANSPORT_ERROR, cb,
                                     cb_length, false, 0U, 0U);
        }
        (void)usb_storage_reset(storage);
        return USB_STORAGE_TRANSPORT_ERROR;
    }
    if (length != 0U) {
        const bool completed = data_in
                                   ? storage->transport.bulk_in(
                                         storage->transport.context, data,
                                         length, &moved)
                                   : storage->transport.bulk_out(
                                         storage->transport.context, data,
                                         length, &moved);

        if (!completed) {
            ++storage->transport_error_count;
            if (allow_recovery) {
                usb_storage_record_fault(storage, USB_STORAGE_TRANSPORT_ERROR,
                                         cb, cb_length, false, 0U, 0U);
            }
            (void)usb_storage_reset(storage);
            return USB_STORAGE_TRANSPORT_ERROR;
        }
        short_stage = moved != length;
        data_moved = moved;
    }
    /* Command Status Wrapper (BOT 1.0 section 3.2, table 2). */
    if (!storage->transport.bulk_in(storage->transport.context, storage->csw,
                                    USB_STORAGE_CSW_BYTES, &moved) ||
        moved != USB_STORAGE_CSW_BYTES) {
        ++storage->transport_error_count;
        if (allow_recovery) {
            usb_storage_record_fault(storage, USB_STORAGE_TRANSPORT_ERROR, cb,
                                     cb_length, false, 0U, 0U);
        }
        (void)usb_storage_reset(storage);
        return USB_STORAGE_TRANSPORT_ERROR;
    }
    if (usb_storage_le32(storage->csw) != USB_STORAGE_CSW_SIGNATURE) {
        if (allow_recovery) {
            usb_storage_record_fault(storage, USB_STORAGE_BAD_CSW_SIGNATURE, cb,
                                     cb_length, false, 0U, 0U);
        }
        (void)usb_storage_reset(storage);
        return USB_STORAGE_BAD_CSW_SIGNATURE;
    }
    if (usb_storage_le32(storage->csw + 4U) != tag) {
        if (allow_recovery) {
            usb_storage_record_fault(storage, USB_STORAGE_CSW_TAG_MISMATCH, cb,
                                     cb_length, false, 0U, 0U);
        }
        (void)usb_storage_reset(storage);
        return USB_STORAGE_CSW_TAG_MISMATCH;
    }
    residue = usb_storage_le32(storage->csw + 8U);
    csw_status = storage->csw[12];
    storage->last_residue = residue;
    storage->last_csw_status = csw_status;
    if (csw_status == USB_STORAGE_CSW_STATUS_PHASE) {
        if (allow_recovery) {
            usb_storage_record_fault(storage, USB_STORAGE_PHASE_ERROR, cb,
                                     cb_length, true, csw_status, residue);
        }
        (void)usb_storage_reset(storage);
        return USB_STORAGE_PHASE_ERROR;
    }
    if (csw_status == USB_STORAGE_CSW_STATUS_FAILED) {
        uint8_t saved_status = storage->last_csw_status;
        uint32_t saved_residue = storage->last_residue;
        const bool record = allow_recovery;

        if (record) {
            usb_storage_record_fault(storage, USB_STORAGE_COMMAND_FAILED, cb,
                                     cb_length, true, csw_status, residue);
        }
        if (allow_recovery) {
            const enum usb_storage_status sense =
                usb_storage_request_sense(storage);

            storage->fault.sense_attempted = true;
            storage->fault.sense_status = sense;
            storage->fault.sense_valid = storage->last_sense_valid;
            storage->fault.sense_response = storage->last_sense_response;
            storage->fault.sense_key = storage->last_sense_key;
            storage->fault.sense_asc = storage->last_sense_asc;
            storage->fault.sense_ascq = storage->last_sense_ascq;
        }
        storage->last_csw_status = saved_status;
        storage->last_residue = saved_residue;
        return USB_STORAGE_COMMAND_FAILED;
    }
    if (csw_status != USB_STORAGE_CSW_STATUS_PASS) {
        if (allow_recovery) {
            usb_storage_record_fault(storage, USB_STORAGE_UNKNOWN, cb,
                                     cb_length, true, csw_status, residue);
        }
        (void)usb_storage_reset(storage);
        return USB_STORAGE_UNKNOWN;
    }
    /* ITEM-572: a short data-in still says how much arrived, which is what a
       MODE SENSE page is read from (the CSW's own transfer has reused `moved`
       by now). */
    if (transferred != NULL) {
        *transferred = data_moved;
    }
    if (short_stage || residue != 0U) {
        return USB_STORAGE_SHORT_TRANSFER;
    }
    return USB_STORAGE_OK;
}

static enum usb_storage_status usb_storage_trim_string(
    char *target,
    size_t capacity,
    const uint8_t *source,
    size_t length) {
    size_t end = length;
    size_t index;

    if (target == NULL || capacity == 0U) {
        return USB_STORAGE_NULL;
    }
    /* INQUIRY pads its strings with spaces (SPC-4 6.6.2). */
    while (end > 0U && source[end - 1U] == ' ') {
        --end;
    }
    if (end >= capacity) {
        end = capacity - 1U;
    }
    for (index = 0U; index < end; ++index) {
        target[index] = (char)source[index];
    }
    target[end] = '\0';
    return USB_STORAGE_OK;
}

enum usb_storage_status usb_storage_inquiry(struct usb_storage *storage) {
    uint8_t cb[10];
    uint8_t response[USB_STORAGE_INQUIRY_BYTES];
    uint32_t moved = 0U;
    enum usb_storage_status status;

    if (storage == NULL) {
        return USB_STORAGE_NULL;
    }
    usb_storage_zero(cb, sizeof(cb));
    cb[0] = USB_SCSI_INQUIRY;
    cb[4] = USB_STORAGE_INQUIRY_BYTES;
    status = usb_storage_execute(storage, cb, 6U, response,
                                 USB_STORAGE_INQUIRY_BYTES, true, &moved,
                                 true);
    if (status != USB_STORAGE_OK) {
        return status;
    }
    storage->identity.valid = true;
    (void)usb_storage_trim_string(storage->identity.vendor,
                                  sizeof(storage->identity.vendor),
                                  response + 8U, 8U);
    (void)usb_storage_trim_string(storage->identity.product,
                                  sizeof(storage->identity.product),
                                  response + 16U, 16U);
    (void)usb_storage_trim_string(storage->identity.revision,
                                  sizeof(storage->identity.revision),
                                  response + 32U, 4U);
    return USB_STORAGE_OK;
}

enum usb_storage_status usb_storage_test_unit_ready(
    struct usb_storage *storage) {
    uint8_t cb[10];
    enum usb_storage_status status;

    if (storage == NULL) {
        return USB_STORAGE_NULL;
    }
    usb_storage_zero(cb, sizeof(cb));
    cb[0] = USB_SCSI_TEST_UNIT_READY;
    status = usb_storage_execute(storage, cb, 6U, NULL, 0U, false, NULL,
                                 true);
    if (status == USB_STORAGE_COMMAND_FAILED &&
        storage->last_sense_key == USB_SCSI_SENSE_NOT_READY) {
        /*
         * The device answered - it just has no medium ready.  Naming that as
         * its own status keeps a stick that needs a moment from reading as a
         * device that failed a command, which is what the caller sees.
         */
        return USB_STORAGE_NOT_READY;
    }
    return status;
}

enum usb_storage_status usb_storage_read_capacity(
    struct usb_storage *storage) {
    uint8_t cb[10];
    uint8_t response[USB_STORAGE_CAPACITY_BYTES];
    uint32_t moved = 0U;
    uint32_t last_lba;
    uint32_t block_size;
    enum usb_storage_status status;

    if (storage == NULL) {
        return USB_STORAGE_NULL;
    }
    usb_storage_zero(cb, sizeof(cb));
    cb[0] = USB_SCSI_READ_CAPACITY_10;
    status = usb_storage_execute(storage, cb, 10U, response,
                                 USB_STORAGE_CAPACITY_BYTES, true, &moved,
                                 true);
    if (status != USB_STORAGE_OK) {
        return status;
    }
    last_lba = usb_storage_be32(response);
    block_size = usb_storage_be32(response + 4U);
    if (block_size < BLOCK_DEVICE_BLOCK_SIZE_MIN ||
        block_size > BLOCK_DEVICE_BLOCK_SIZE_MAX) {
        return USB_STORAGE_BAD_BLOCK_SIZE;
    }
    /*
     * All ones is the "capacity exceeds what this command can express" answer
     * (SPC-4 5.15); READ CAPACITY (16) is the command that would answer it,
     * and this module does not issue it, so the device is named as having no
     * usable capacity instead of being silently truncated.
     */
    if (last_lba == UINT32_C(0xFFFFFFFF)) {
        return USB_STORAGE_BAD_CAPACITY;
    }
    storage->block_size = block_size;
    storage->block_count = last_lba + 1U;
    return USB_STORAGE_OK;
}

enum usb_storage_status usb_storage_bind(
    struct usb_storage *storage,
    const struct usb_storage_transport *transport,
    const struct usb_storage_config *config) {
    enum usb_storage_status status;

    if (storage == NULL || transport == NULL || config == NULL) {
        return USB_STORAGE_NULL;
    }
    if (transport->control == NULL || transport->bulk_in == NULL ||
        transport->bulk_out == NULL) {
        return USB_STORAGE_BAD_TRANSPORT;
    }
    usb_storage_zero(storage, sizeof(*storage));
    storage->transport = *transport;
    storage->config = *config;

    status = usb_storage_inquiry(storage);
    if (status != USB_STORAGE_OK) {
        return status;
    }
    status = usb_storage_test_unit_ready(storage);
    if (status != USB_STORAGE_OK) {
        return status;
    }
    status = usb_storage_read_capacity(storage);
    if (status != USB_STORAGE_OK) {
        return status;
    }
    storage->ready = true;
    /* ITEM-572: ask what the device's cache looks like.  The answer decides the
       flush policy, and a device that does not answer leaves it unknown - which
       is not the same fact as WCE=0.  A capability read never fails the bind. */
    (void)usb_storage_query_cache(storage);
    return USB_STORAGE_OK;
}

static enum usb_storage_status usb_storage_read_write(
    struct usb_storage *storage,
    uint64_t lba,
    uint32_t blocks,
    void *buffer,
    bool write) {
    uint8_t cb[10];
    uint32_t length;
    uint32_t moved = 0U;
    enum usb_storage_status status;

    if (storage == NULL) {
        return USB_STORAGE_NULL;
    }
    if (!storage->ready) {
        return USB_STORAGE_NOT_READY;
    }
    if (buffer == NULL || blocks == 0U) {
        return USB_STORAGE_INVALID_LENGTH;
    }
    if (blocks > USB_STORAGE_MAX_BLOCKS_PER_COMMAND) {
        return USB_STORAGE_INVALID_LENGTH;
    }
    if (lba >= storage->block_count ||
        blocks > storage->block_count - (uint32_t)lba) {
        return USB_STORAGE_OUT_OF_RANGE;
    }
    length = blocks * storage->block_size;
    usb_storage_zero(cb, sizeof(cb));
    cb[0] = write ? USB_SCSI_WRITE_10 : USB_SCSI_READ_10;
    usb_storage_write_be32(cb + 2U, (uint32_t)lba);
    usb_storage_write_be16(cb + 7U, (uint16_t)blocks);
    storage->fault_operation = write ? USB_STORAGE_FAULT_WRITE
                                     : USB_STORAGE_FAULT_READ;
    status = usb_storage_execute(storage, cb, 10U, buffer, length, !write,
                                 &moved, true);
    if (status != USB_STORAGE_OK) {
        return status;
    }
    if (write) {
        ++storage->write_count;
    } else {
        ++storage->read_count;
    }
    return USB_STORAGE_OK;
}

enum usb_storage_status usb_storage_read(
    struct usb_storage *storage,
    uint64_t lba,
    uint32_t blocks,
    void *buffer) {
    return usb_storage_read_write(storage, lba, blocks, buffer, false);
}

enum usb_storage_status usb_storage_write(
    struct usb_storage *storage,
    uint64_t lba,
    uint32_t blocks,
    const void *buffer) {
    return usb_storage_read_write(storage, lba, blocks, (void *)buffer, true);
}

/*
 * ITEM-572: the one answer that means "this device does not implement the
 * flush", and nothing else.  It needs all of: the flush's own opcode, a valid
 * failed CSW, a REQUEST SENSE that itself completed, a sense response whose
 * code says it is sense data, and exactly ILLEGAL REQUEST / INVALID COMMAND
 * OPERATION CODE / 00h (SPC-4, sense 05/20/00).  Any other failure - a
 * transport error, an unusable sense, another key - stays an error.
 */
static bool usb_storage_flush_is_unsupported(
    const struct usb_storage *storage) {
    const struct usb_storage_fault *fault = &storage->fault;

    return fault->valid && fault->opcode == USB_SCSI_SYNCHRONIZE_CACHE_10 &&
           fault->csw_valid &&
           fault->csw_status == USB_STORAGE_CSW_STATUS_FAILED &&
           fault->sense_attempted &&
           fault->sense_status == USB_STORAGE_OK && fault->sense_valid &&
           fault->sense_key == 0x05U && fault->sense_asc == 0x20U &&
           fault->sense_ascq == 0x00U;
}

enum usb_storage_status usb_storage_query_cache(struct usb_storage *storage) {
    uint8_t cb[USB_STORAGE_MODE_SENSE_CDB_BYTES];
    uint32_t moved = 0U;
    enum usb_storage_status status;

    if (storage == NULL) {
        return USB_STORAGE_NULL;
    }
    if (!storage->ready) {
        return USB_STORAGE_NOT_READY;
    }
    storage->cache_known = false;
    storage->wce = false;
    storage->cache_page_length = 0U;
    usb_storage_zero(storage->cache_page, sizeof(storage->cache_page));
    usb_storage_zero(cb, sizeof(cb));
    cb[0] = USB_STORAGE_MODE_SENSE_6;
    cb[2] = USB_STORAGE_CACHING_PAGE;
    cb[4] = (uint8_t)USB_STORAGE_MODE_SENSE_BYTES;
    status = usb_storage_execute(storage, cb, sizeof(cb),
                                 storage->cache_page,
                                 USB_STORAGE_MODE_SENSE_BYTES, true, &moved,
                                 true);
    /* A MODE SENSE response is normally shorter than the allocation length, so
       the short data stage and its residue are expected here - the bytes that
       did arrive are what the page is read from. */
    if (status == USB_STORAGE_SHORT_TRANSFER) {
        status = USB_STORAGE_OK;
    }
    if (status != USB_STORAGE_OK || moved < 8U) {
        return USB_STORAGE_OK;
    }
    storage->cache_page_length =
        moved > sizeof(storage->cache_page) ? (uint8_t)sizeof(storage->cache_page)
                                            : (uint8_t)moved;
    {
        const uint8_t mode_length = storage->cache_page[0];
        const uint8_t block_descriptor = storage->cache_page[3];
        const uint32_t page_offset = 4U + block_descriptor;

        if (mode_length < 4U || (uint32_t)mode_length + 1U > moved) {
            return USB_STORAGE_OK;
        }
        if (mode_length == 0xFFU) {
            /* The legacy "no mode sense" shape: not a page. */
            return USB_STORAGE_OK;
        }
        if (page_offset + 4U > moved ||
            (storage->cache_page[page_offset] & 0x3FU) !=
                USB_STORAGE_CACHING_PAGE ||
            storage->cache_page[page_offset + 1U] < 4U) {
            return USB_STORAGE_OK;
        }
        /* SPC-4 caching page, byte 2 bit 2: WCE. */
        storage->wce = (storage->cache_page[page_offset + 2U] & 0x04U) != 0U;
        storage->cache_known = true;
    }
    return USB_STORAGE_OK;
}

const char *usb_storage_flush_policy_string(
    enum usb_storage_flush_policy policy) {
    switch (policy) {
    case USB_STORAGE_FLUSH_SENT:
        return "SENT";
    case USB_STORAGE_FLUSH_WRITE_THROUGH:
        return "WRITE_THROUGH";
    case USB_STORAGE_FLUSH_UNSUPPORTED_WRITE_THROUGH:
        return "UNSUPPORTED_WRITE_THROUGH";
    case USB_STORAGE_FLUSH_UNSUPPORTED_CACHE_UNKNOWN:
        return "UNSUPPORTED_CACHE_UNKNOWN";
    case USB_STORAGE_FLUSH_FAILED:
        return "FAILED";
    case USB_STORAGE_FLUSH_NONE:
        break;
    }
    return "NONE";
}

enum usb_storage_status usb_storage_synchronize_cache(
    struct usb_storage *storage) {
    uint8_t cb[10];
    enum usb_storage_status status;

    if (storage == NULL) {
        return USB_STORAGE_NULL;
    }
    if (!storage->ready) {
        return USB_STORAGE_NOT_READY;
    }
    storage->flush_policy = USB_STORAGE_FLUSH_NONE;
    /* ITEM-572: a device whose caching page says WCE=0 is write-through; there
       is nothing to flush, and the command this device rejects is not sent at
       all. */
    if (storage->cache_known && !storage->wce) {
        storage->flush_policy = USB_STORAGE_FLUSH_WRITE_THROUGH;
        return USB_STORAGE_OK;
    }
    usb_storage_zero(cb, sizeof(cb));
    cb[0] = USB_SCSI_SYNCHRONIZE_CACHE_10;
    storage->fault_operation = USB_STORAGE_FAULT_FLUSH;
    status = usb_storage_execute(storage, cb, 10U, NULL, 0U, false, NULL, true);
    if (status == USB_STORAGE_OK) {
        storage->flush_policy = USB_STORAGE_FLUSH_SENT;
        return USB_STORAGE_OK;
    }
    if (status == USB_STORAGE_COMMAND_FAILED &&
        usb_storage_flush_is_unsupported(storage)) {
        if (storage->cache_known && !storage->wce) {
            storage->flush_policy =
                USB_STORAGE_FLUSH_UNSUPPORTED_WRITE_THROUGH;
            return USB_STORAGE_OK;
        }
        if (!storage->cache_known) {
            /* Documented weaker guarantee: the device answered every WRITE,
               and the session is read back; the cache state is unknown, so the
               flush cannot be claimed to make the writes power-loss safe. */
            storage->flush_policy = USB_STORAGE_FLUSH_UNSUPPORTED_CACHE_UNKNOWN;
            return USB_STORAGE_OK;
        }
        /* WCE=1 and the device cannot flush: there is a cache and no way to
           make it durable - a real failure, not a success. */
        storage->flush_policy = USB_STORAGE_FLUSH_FAILED;
        return USB_STORAGE_FLUSH_UNSUPPORTED;
    }
    storage->flush_policy = USB_STORAGE_FLUSH_FAILED;
    return status;
}

/*
 * The block-device view.  Offsets and lengths are byte quantities like every
 * other block_device in the tree; they are checked for alignment and range
 * before anything reaches the wire, and one long request is split into BOT
 * commands of at most USB_STORAGE_MAX_BLOCKS_PER_COMMAND blocks so the CBW's
 * length field and the caller's buffer stay in a size a controller transfer
 * can carry.
 */
static enum block_device_status usb_storage_block_status(
    enum usb_storage_status status) {
    switch (status) {
    case USB_STORAGE_OK:
        return BLOCK_DEVICE_OK;
    case USB_STORAGE_NULL:
        return BLOCK_DEVICE_NULL;
    case USB_STORAGE_OUT_OF_RANGE:
        return BLOCK_DEVICE_OUT_OF_RANGE;
    case USB_STORAGE_BAD_ALIGNMENT:
        return BLOCK_DEVICE_UNALIGNED;
    case USB_STORAGE_INVALID_LENGTH:
        return BLOCK_DEVICE_INVALID_LENGTH;
    case USB_STORAGE_NOT_READY:
    case USB_STORAGE_UNSUPPORTED_INTERFACE:
        return BLOCK_DEVICE_UNSUPPORTED;
    case USB_STORAGE_BAD_TRANSPORT:
    case USB_STORAGE_BAD_BLOCK_SIZE:
    case USB_STORAGE_BAD_CAPACITY:
        return BLOCK_DEVICE_BAD_DEVICE;
    default:
        return BLOCK_DEVICE_IO_ERROR;
    }
}

static enum block_device_status usb_storage_block_transfer(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length,
    bool write) {
    struct usb_storage_block_device *backend =
        (struct usb_storage_block_device *)device;
    struct usb_storage *storage;
    uint64_t capacity;
    uint64_t first_block;
    uint32_t blocks;
    uint32_t done = 0U;
    uint32_t block_size;

    if (device == NULL || backend->storage == NULL) {
        return BLOCK_DEVICE_NULL;
    }
    if (buffer == NULL || length == 0U) {
        return BLOCK_DEVICE_INVALID_LENGTH;
    }
    storage = backend->storage;
    block_size = device->block_size;
    if (block_size == 0U || offset % block_size != 0U ||
        length % block_size != 0U) {
        return BLOCK_DEVICE_UNALIGNED;
    }
    capacity = device->block_count * (uint64_t)block_size;
    if (offset > capacity || (uint64_t)length > capacity - offset) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    first_block = offset / block_size;
    blocks = (uint32_t)(length / block_size);
    while (done < blocks) {
        uint32_t chunk = blocks - done;

        if (chunk > USB_STORAGE_MAX_BLOCKS_PER_COMMAND) {
            chunk = USB_STORAGE_MAX_BLOCKS_PER_COMMAND;
        }
        if (write) {
            const uint8_t *source = (const uint8_t *)buffer +
                                    (size_t)done * block_size;
            enum usb_storage_status status = usb_storage_write(
                storage, first_block + done, chunk, source);

            if (status != USB_STORAGE_OK) {
                return usb_storage_block_status(status);
            }
        } else {
            uint8_t *target = (uint8_t *)buffer + (size_t)done * block_size;
            enum usb_storage_status status = usb_storage_read(
                storage, first_block + done, chunk, target);

            if (status != USB_STORAGE_OK) {
                return usb_storage_block_status(status);
            }
        }
        done += chunk;
    }
    return BLOCK_DEVICE_OK;
}

static enum block_device_status usb_storage_block_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length) {
    return usb_storage_block_transfer(device, offset, buffer, length, false);
}

static enum block_device_status usb_storage_block_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length) {
    return usb_storage_block_transfer(device, offset, (void *)buffer, length,
                                      true);
}

static enum block_device_status usb_storage_block_flush(
    struct block_device *device) {
    struct usb_storage_block_device *backend =
        (struct usb_storage_block_device *)device;

    if (device == NULL || backend->storage == NULL) {
        return BLOCK_DEVICE_NULL;
    }
    return usb_storage_block_status(
        usb_storage_synchronize_cache(backend->storage));
}

enum block_device_status usb_storage_block_device_bind(
    struct usb_storage *storage,
    struct usb_storage_block_device *backend) {
    if (storage == NULL || backend == NULL) {
        return BLOCK_DEVICE_NULL;
    }
    if (!storage->ready || storage->block_size == 0U ||
        storage->block_count == 0U) {
        return BLOCK_DEVICE_BAD_DEVICE;
    }
    usb_storage_zero(backend, sizeof(*backend));
    backend->storage = storage;
    backend->device.block_size = storage->block_size;
    backend->device.block_count = storage->block_count;
    backend->device.context = backend;
    backend->device.read_fn = usb_storage_block_read;
    backend->device.write_fn = usb_storage_block_write;
    backend->device.flush_fn = usb_storage_block_flush;
    return BLOCK_DEVICE_OK;
}
