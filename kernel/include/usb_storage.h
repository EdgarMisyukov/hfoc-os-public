#ifndef HFOC_USB_STORAGE_H
#define HFOC_USB_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block_device.h"

/*
 * ITEM-519 increment 1: the transport-free half of USB mass storage.
 *
 * The owner's installation medium is a USB stick, and the kernel cannot see it:
 * the xHCI driver knows only HID roles and `appliance_production_storage`
 * scans AHCI and NVMe, so the install journal the owner asked for has nowhere
 * to be written (see /srv/hfoc-example/queue/item-519-blocker.md).  USB mass
 * storage is a subsystem, not a patch: a bulk-transfer path in xHCI, the
 * Bulk-Only Transport (BOT) with its SCSI command set, and a `block_device`
 * adapter on top.
 *
 * This module is the middle layer, and it is deliberately free of every
 * kernel dependency: the wire protocol - CBW, data stage, CSW, the SCSI
 * commands a stick must answer, and the reset recovery BOT defines for a
 * broken sequence - is pure state over two callbacks, so the host proof drives
 * every error shape (short data, residue, wrong tag, phase error, stalled
 * endpoint, rejected command) without a controller, a QEMU machine or the
 * owner's hardware.  The xHCI bulk path that will feed it is increment 2, and
 * it changes no code here.
 *
 * What this module is *not*: it does not enumerate, address or configure a
 * device and it owns no DMA memory.  The caller passes one control-transfer
 * callback and two bulk callbacks bound to an enumerated interface, exactly
 * the shape xhci.c already produces for HID, and all buffers stay the
 * caller's.
 *
 * References: USB Mass Storage Class Bulk-Only Transport revision 1.0
 * (CBW/CSW layout, the reset recovery, the class-specific reset request), SCSI
 * Primary Commands-4 (INQUIRY, TEST UNIT READY, REQUEST SENSE, READ CAPACITY
 * (10), READ (10), WRITE (10), SYNCHRONIZE CACHE (10)) and the USB 2.0
 * specification section 9.6.6 for the interface and endpoint descriptors the
 * configuration parser walks.
 */

/* The fixed BOT structures (Bulk-Only Transport 1.0 sections 3.1 and 3.2). */
#define USB_STORAGE_CBW_BYTES 31U
#define USB_STORAGE_CSW_BYTES 13U
#define USB_STORAGE_CSW_SIGNATURE 0x53425355U
#define USB_STORAGE_CBW_SIGNATURE 0x43425355U
/* One command's data stage is bounded by the caller's DMA buffer; the block
   adapter splits a longer request into commands of at most this many blocks
   (240 x 512 = 120 KiB, and never more than the CBW's 32-bit length field). */
#define USB_STORAGE_MAX_BLOCKS_PER_COMMAND 240U
#define USB_STORAGE_INQUIRY_BYTES 36U
#define USB_STORAGE_SENSE_BYTES 18U
#define USB_STORAGE_CAPACITY_BYTES 8U

/* The SCSI opcodes this module issues. */
#define USB_SCSI_TEST_UNIT_READY 0x00U
#define USB_SCSI_REQUEST_SENSE 0x03U
#define USB_SCSI_INQUIRY 0x12U
#define USB_SCSI_READ_CAPACITY_10 0x25U
#define USB_SCSI_READ_10 0x28U
#define USB_SCSI_WRITE_10 0x2AU
#define USB_SCSI_SYNCHRONIZE_CACHE_10 0x35U

/* The interface shape USB mass storage BOT declares (USB Mass Storage Class
   specification, table 2.1): class 8, subclass 6, protocol 0x50.  Every other
   protocol - UAS (0x62), the transparent SCSI command set (0x00) - is named
   UNSUPPORTED_INTERFACE rather than guessed at. */
#define USB_STORAGE_INTERFACE_CLASS 0x08U
#define USB_STORAGE_INTERFACE_SUBCLASS 0x06U
#define USB_STORAGE_INTERFACE_PROTOCOL_BOT 0x50U

/* The class-specific reset request (BOT 1.0 section 3.1). */
#define USB_STORAGE_REQUEST_RESET 0xFFU
#define USB_STORAGE_REQUEST_TYPE_CLASS_INTERFACE 0x21U
#define USB_STORAGE_REQUEST_TYPE_STANDARD_OUT 0x02U

/*
 * Why a USB storage command or binding did not succeed.  Every value is a
 * token an operator can read in a record; only USB_STORAGE_OK means the
 * device answered a command the way BOT and SCSI say it must.
 */
enum usb_storage_status {
    USB_STORAGE_OK = 0,
    /* A required pointer was NULL. */
    USB_STORAGE_NULL,
    /* A transport callback is missing, so no command can be issued. */
    USB_STORAGE_BAD_TRANSPORT,
    /* The descriptor blob is not a configuration descriptor this parser can
       walk (too short, a zero or overlong bLength, a length past the blob). */
    USB_STORAGE_MALFORMED_DESCRIPTOR,
    /* The configuration has no BOT mass-storage interface: the device is
       another class, another subclass or another protocol (UAS, for one). */
    USB_STORAGE_UNSUPPORTED_INTERFACE,
    /* The BOT interface declares no usable bulk IN or bulk OUT endpoint. */
    USB_STORAGE_MALFORMED_INTERFACE,
    /* A binding has not succeeded yet, so there is no capacity to address. */
    USB_STORAGE_NOT_READY,
    /* A transport callback reported failure. */
    USB_STORAGE_TRANSPORT_ERROR,
    /* A stage carried fewer bytes than the command asked for. */
    USB_STORAGE_SHORT_TRANSFER,
    /* The CSW signature is not 0x53425355: the sequence is broken. */
    USB_STORAGE_BAD_CSW_SIGNATURE,
    /* The CSW answers another command's tag. */
    USB_STORAGE_CSW_TAG_MISMATCH,
    /* The CSW reports phase error (status 2); BOT requires a reset. */
    USB_STORAGE_PHASE_ERROR,
    /* The CSW reports the command failed (status 1); the sense data is the
       device's own reason and is kept in the state. */
    USB_STORAGE_COMMAND_FAILED,
    /* The device answered a failed command but its sense reply was unusable. */
    USB_STORAGE_SENSE_FAILED,
    /* ITEM-572: the device rejected SYNCHRONIZE CACHE as an invalid command
       (sense 05/20/00) and its cache state says a flush was required (WCE=1).
       The policy in usb_storage.h decides when this is reported at all: a
       write-through device is never asked, and an unknown cache state reports
       the documented weaker guarantee instead. */
    USB_STORAGE_FLUSH_UNSUPPORTED,
    /* READ CAPACITY answered a block size a block_device cannot carry. */
    USB_STORAGE_BAD_BLOCK_SIZE,
    /* READ CAPACITY answered an unreadable (all-ones) capacity. */
    USB_STORAGE_BAD_CAPACITY,
    /* The request addresses blocks outside the declared capacity. */
    USB_STORAGE_OUT_OF_RANGE,
    /* A byte offset or length is not a whole number of blocks. */
    USB_STORAGE_BAD_ALIGNMENT,
    /* A transfer length is zero. */
    USB_STORAGE_INVALID_LENGTH,
    USB_STORAGE_UNKNOWN
};

/* The interface binding the parser returns and the transport feeds. */
struct usb_storage_config {
    /* bConfigurationValue of the configuration descriptor the interface was
       found in; SET_CONFIGURATION needs it (ITEM-519 increment 2). */
    uint8_t configuration_value;
    uint8_t interface_number;
    /* The endpoint addresses as the descriptors declare them (bit 7 is the
       direction), and their own wMaxPacketSize. */
    uint8_t bulk_in_endpoint;
    uint8_t bulk_out_endpoint;
    uint16_t bulk_in_max_packet;
    uint16_t bulk_out_max_packet;
    /*
     * ITEM-545: the burst each endpoint's SuperSpeed companion descriptor
     * declares (USB 3.0 section 9.6.7, bMaxBurst), or 0 when the interface
     * declares no companion.  The xHCI endpoint context has to carry this
     * value in its Max Burst Size field (xHCI 1.1 section 6.2.3.5); a device
     * that declares a burst and a host that programs zero are not saying the
     * same thing about the wire.
     */
    uint8_t bulk_in_max_burst;
    uint8_t bulk_out_max_burst;
};

/*
 * The device's own answer to INQUIRY, trimmed and NUL-terminated, so a record
 * can name the stick the owner plugged in instead of only its vid/pid.
 */
struct usb_storage_identity {
    bool valid;
    char vendor[9];
    char product[17];
    char revision[5];
};

/*
 * The pipes one enumerated BOT interface offers.  The caller owns the
 * context and decides what a transfer means; this module only requires that a
 * call returns true exactly when the whole transfer completed.
 */
struct usb_storage_transport {
    void *context;
    bool (*control)(void *context, uint8_t request_type, uint8_t request,
                    uint16_t value, uint16_t index, uint16_t length,
                    bool data_in, void *data, uint32_t *transferred);
    bool (*bulk_in)(void *context, void *buffer, uint32_t length,
                    uint32_t *transferred);
    bool (*bulk_out)(void *context, const void *buffer, uint32_t length,
                     uint32_t *transferred);
};

/*
 * ITEM-571: the device's own answer for the command that failed.
 *
 * The sequence-commit refusal (`VERIFY/SEQUENCE_COMMIT`) was a single bool on
 * the way out, so a failed header write and a failed flush were the same
 * event, and the command that failed - its opcode, its CSW status and residue,
 * the sense data and whether that sense data is trustworthy - was lost.  This
 * snapshot is kept in RAM, per device, and survives a journal that cannot be
 * written: REQUEST SENSE is the recovery, never the substituted command, so the
 * primary fields are filled before it runs and are not touched by it.
 */
/* The block_device operation a fault belongs to (the adapter sets it before the
   command, the USB layer never overwrites it). */
enum usb_storage_fault_operation {
    USB_STORAGE_FAULT_NONE = 0,
    USB_STORAGE_FAULT_READ,
    USB_STORAGE_FAULT_WRITE,
    USB_STORAGE_FAULT_FLUSH,
    USB_STORAGE_FAULT_OTHER
};

struct usb_storage_fault {
    bool valid;
    enum usb_storage_fault_operation operation;
    /* The command that failed, as it was issued. */
    uint8_t opcode;
    uint8_t cdb[16];
    uint8_t cdb_length;
    /* Its own outcome. */
    enum usb_storage_status status;
    bool csw_valid;
    uint8_t csw_status;
    uint32_t residue;
    /* The recovery's own outcome, kept apart from the primary command. */
    bool sense_attempted;
    enum usb_storage_status sense_status;
    bool sense_valid;
    uint8_t sense_response;
    uint8_t sense_key;
    uint8_t sense_asc;
    uint8_t sense_ascq;
};

/*
 * ITEM-572: what the device said about its write cache, and the flush policy
 * that follows from it.
 *
 * The machine's stick rejects SYNCHRONIZE CACHE (10) as an invalid command
 * (failed CSW, sense 05/20/00), and its MODE SENSE caching page answers WCE=0
 * with a valid page.  A device with no write cache has nothing to flush, so the
 * command is not sent at all; a device whose cache page is unreadable keeps
 * `cache_known = false` (an absent answer is not WCE=0) and follows the
 * documented weaker guarantee; a device with WCE=1 that rejects the flush keeps
 * the failure, because there is a cache and no way to make it durable.
 */
#define USB_STORAGE_MODE_SENSE_BYTES 64U
#define USB_STORAGE_CACHING_PAGE 0x08U
#define USB_STORAGE_MODE_SENSE_6 0x1AU
#define USB_STORAGE_MODE_SENSE_CDB_BYTES 6U

enum usb_storage_flush_policy {
    USB_STORAGE_FLUSH_NONE = 0,
    /* The command was sent and its CSW passed. */
    USB_STORAGE_FLUSH_SENT,
    /* WCE=0 was confirmed by the caching page: no command is sent, because a
       write-through device has nothing to flush. */
    USB_STORAGE_FLUSH_WRITE_THROUGH,
    /* The command was rejected as invalid, and the caching page had already
       said WCE=0: the rejection confirms the same fact. */
    USB_STORAGE_FLUSH_UNSUPPORTED_WRITE_THROUGH,
    /* The command was rejected as invalid and the cache state is unknown.  The
       flush reports success under the documented guarantee: every WRITE's CSW
       passed and the session reads back, which is not a power-loss claim. */
    USB_STORAGE_FLUSH_UNSUPPORTED_CACHE_UNKNOWN,
    /* Anything else: a real error, reported as one. */
    USB_STORAGE_FLUSH_FAILED
};

/*
 * One bound mass-storage device.  The command buffers live here, so the
 * caller only supplies the data buffer of a read or a write; the identity and
 * the sense data stay for the record, and the counters make "how many
 * commands, how many failures, how many resets" numbers rather than a claim.
 */
struct usb_storage {
    struct usb_storage_transport transport;
    struct usb_storage_config config;
    struct usb_storage_identity identity;
    uint32_t tag;
    uint32_t block_count;
    uint32_t block_size;
    bool ready;
    uint32_t command_count;
    uint32_t read_count;
    uint32_t write_count;
    uint32_t transport_error_count;
    uint32_t reset_count;
    uint8_t last_csw_status;
    uint8_t last_sense_key;
    uint8_t last_sense_asc;
    uint8_t last_sense_ascq;
    /* ITEM-571: the sense response code, and whether it is one SPC-4 defines
       (0x70 current / 0x71 deferred) - the key/ASC/ASCQ are only trustworthy
       when it is. */
    uint8_t last_sense_response;
    bool last_sense_valid;
    uint32_t last_residue;
    /* ITEM-571: the last failed command's own snapshot (see the struct above). */
    struct usb_storage_fault fault;
    enum usb_storage_fault_operation fault_operation;
    /* ITEM-572: the caching page the device answered (or not), and the flush
       policy the last flush used.  `cache_known` is false when MODE SENSE did
       not answer or answered something that is not a caching page - that is
       UNKNOWN, never WCE=0. */
    bool cache_known;
    bool wce;
    uint8_t cache_page[USB_STORAGE_MODE_SENSE_BYTES];
    uint8_t cache_page_length;
    enum usb_storage_flush_policy flush_policy;
    uint8_t cbw[USB_STORAGE_CBW_BYTES];
    uint8_t csw[USB_STORAGE_CSW_BYTES];
    uint8_t sense[USB_STORAGE_SENSE_BYTES];
};

/* The `block_device` view of a bound stick, so the rest of the storage stack
   addresses it like any AHCI or NVMe disk. */
struct usb_storage_block_device {
    struct block_device device;
    struct usb_storage *storage;
};

const char *usb_storage_status_string(enum usb_storage_status status);
/*
 * The snapshot of the last command that failed on this device, or NULL when
 * nothing has failed.  It is the caller's to render: the block adapter, the
 * record and the diag command all need the same fields, and none of them may
 * depend on a journal that is not writable.
 */
const struct usb_storage_fault *usb_storage_last_fault(
    const struct usb_storage *storage);
/*
 * Read the device's MODE SENSE caching page (page 08h) and record what it says:
 * `cache_known` and `wce`, plus the raw response.  A device that does not
 * answer, answers a short or malformed page, or answers something that is not
 * the caching page leaves `cache_known` false - UNKNOWN, not WCE=0.  The call
 * never fails the device: it is a capability read.
 */
enum usb_storage_status usb_storage_query_cache(struct usb_storage *storage);
const char *usb_storage_flush_policy_string(
    enum usb_storage_flush_policy policy);
/* Render the snapshot as one bounded token line, e.g.
   `op=0x35 csw=1 res=0 sense=1/70-03-11-00 rs=OK`. */
bool usb_storage_fault_text(const struct usb_storage_fault *fault,
                            char *buffer,
                            size_t size);

/*
 * Walk one configuration descriptor blob (`length` is what the control
 * transfer received) and name the BOT mass-storage interface and its two bulk
 * endpoints.  The walk is bounded by the blob and by each descriptor's own
 * bLength; it never reads past `length` and never trusts the blob's total
 * length field.
 */
enum usb_storage_status usb_storage_parse_config(
    const void *descriptor,
    size_t length,
    struct usb_storage_config *config);

/*
 * Bind one enumerated BOT interface: store the transport and config, issue
 * INQUIRY, TEST UNIT READY and READ CAPACITY, and publish `ready` only when
 * all three answered.  A device that is inserted but not yet spun up comes
 * back as the named status of the command that failed, with its sense data
 * kept, never as a device that "works until the first read".
 */
enum usb_storage_status usb_storage_bind(
    struct usb_storage *storage,
    const struct usb_storage_transport *transport,
    const struct usb_storage_config *config);

/* BOT reset recovery: the class-specific reset request followed by
   CLEAR_FEATURE(HALT) on both bulk endpoints, as BOT 1.0 section 3.1 defines
   it.  The caller keeps its own counters; this is the repair, and it is also
   what a phase error or a broken CSW triggers before the next command. */
enum usb_storage_status usb_storage_reset(struct usb_storage *storage);

enum usb_storage_status usb_storage_test_unit_ready(
    struct usb_storage *storage);
enum usb_storage_status usb_storage_inquiry(struct usb_storage *storage);
enum usb_storage_status usb_storage_read_capacity(
    struct usb_storage *storage);
enum usb_storage_status usb_storage_read(
    struct usb_storage *storage,
    uint64_t lba,
    uint32_t blocks,
    void *buffer);
enum usb_storage_status usb_storage_write(
    struct usb_storage *storage,
    uint64_t lba,
    uint32_t blocks,
    const void *buffer);
enum usb_storage_status usb_storage_synchronize_cache(
    struct usb_storage *storage);

/*
 * Bind the block_device view of a ready stick.  Reads and writes are byte
 * addressed like every other block_device, must be block aligned and inside
 * the capacity, and are split into bounded BOT commands internally.
 */
enum block_device_status usb_storage_block_device_bind(
    struct usb_storage *storage,
    struct usb_storage_block_device *backend);

#endif
