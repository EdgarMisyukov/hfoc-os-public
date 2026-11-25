#ifndef HFOC_APPLIANCE_ACME_CONTROL_H
#define HFOC_APPLIANCE_ACME_CONTROL_H

#include "appliance_certificate_name_store.h"
#include "appliance_production_certificate.h"
#include "appliance_state_block_device.h"
#include "appliance_state_loader.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-508 increment 34: the operator's durable control over ACME.
 *
 * The panel can already read the ACME state (increment 32).  Point 10 of the
 * item makes enabling, disabling and starting an issuance two-phase operator
 * mutations with an audit trail, and a mutation that is forgotten by the next
 * reboot is not a setting the operator chose - it is a state the machine
 * happened to be in.  This module owns that setting: one small record next to
 * the ITEM-503 certificate record and the ITEM-507 name table, in the same
 * CERTIFICATE-namespace store, at the first block after the name table's
 * thirty-two.
 *
 *   header  magic "ACMC", version, flags (bit 0 enabled, bit 1 order
 *           requested), the instant the order was requested at, and the first
 *           four bytes of SHA-256 over the first sixteen bytes;
 *   absent  a blank record is not a fault: ACME is off and no order is
 *           requested, exactly as a machine that never saw the operator;
 *   corrupt a record of another version, another magic or a bad checksum never
 *           yields half a setting.  `load` reports the reason and hands out the
 *           off state, and the caller publishes that honestly instead of
 *           acting on a value it could not trust.
 *
 * `save` writes the record and reads it back before it reports OK.  A store
 * that cannot prove the record (unavailable, sealed, a short write, a
 * read-back that disagrees) returns a failure, and the caller must not apply
 * the change in memory either: the panel never shows a setting the next boot
 * would not carry.
 *
 * Disabling clears a pending order request: an operator who turned ACME off
 * did not ask for an issuance to happen the next time the machine boots.
 * Requesting an order while ACME is off is refused by name rather than
 * queued - a request that cannot run is not a request.
 */

#define APPLIANCE_ACME_CONTROL_VERSION 1U
/* "ACMC" in little-endian byte order: 0x41 0x43 0x4D 0x43. */
#define APPLIANCE_ACME_CONTROL_MAGIC UINT32_C(0x434D4341)
#define APPLIANCE_ACME_CONTROL_RECORD_VERSION 1U

/*
 * The record starts after the ITEM-503 certificate record and the ITEM-507
 * name table in the shared sixty-four-block CERTIFICATE store.  One block
 * holds it with room to spare, and the offset is compiled from the two owners'
 * own block counts so a later change to either record moves this one with it
 * instead of overlapping it.
 */
#define APPLIANCE_ACME_CONTROL_BLOCK_OFFSET \
    (APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_BLOCKS + \
     APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_BLOCKS)
#define APPLIANCE_ACME_CONTROL_RECORD_BLOCKS 1U
#define APPLIANCE_ACME_CONTROL_RECORD_BYTES \
    (APPLIANCE_ACME_CONTROL_RECORD_BLOCKS * APPLIANCE_STATE_BLOCK_SIZE)

/* The header is magic, version, flags, requested-at and the checksum. */
#define APPLIANCE_ACME_CONTROL_HEADER_BYTES 20U
#define APPLIANCE_ACME_CONTROL_CHECKSUM_BYTES 4U

#define APPLIANCE_ACME_CONTROL_FLAG_ENABLED UINT32_C(0x00000001)
#define APPLIANCE_ACME_CONTROL_FLAG_ORDER_REQUESTED UINT32_C(0x00000002)

enum appliance_acme_control_status {
    APPLIANCE_ACME_CONTROL_OK = 0,
    APPLIANCE_ACME_CONTROL_NULL = 1,
    APPLIANCE_ACME_CONTROL_BAD_STATE = 2,
    APPLIANCE_ACME_CONTROL_BAD_VERSION = 3,
    /* No state volume, or a layout that does not carry the certificate store. */
    APPLIANCE_ACME_CONTROL_STORE_UNAVAILABLE = 4,
    /* A sealed volume is never written in the clear. */
    APPLIANCE_ACME_CONTROL_STORE_SEALED = 5,
    APPLIANCE_ACME_CONTROL_READ_FAILED = 6,
    APPLIANCE_ACME_CONTROL_WRITE_FAILED = 7,
    APPLIANCE_ACME_CONTROL_RECORD_CORRUPT = 8,
    APPLIANCE_ACME_CONTROL_RECORD_CHECKSUM = 9,
    APPLIANCE_ACME_CONTROL_RECORD_VERSION_UNKNOWN = 10,
    /* An order can be requested only while ACME is enabled. */
    APPLIANCE_ACME_CONTROL_NOT_ENABLED = 11,
    /* One order request is pending; a second is refused, not queued twice. */
    APPLIANCE_ACME_CONTROL_ALREADY_REQUESTED = 12,
};

enum appliance_acme_control_source {
    APPLIANCE_ACME_CONTROL_SOURCE_NONE = 0,
    APPLIANCE_ACME_CONTROL_SOURCE_COMMITTED = 1,
    APPLIANCE_ACME_CONTROL_SOURCE_DEFAULT_MISSING = 2,
    APPLIANCE_ACME_CONTROL_SOURCE_DEFAULT_INVALID = 3,
    APPLIANCE_ACME_CONTROL_SOURCE_DEFAULT_UNAVAILABLE = 4,
};

struct appliance_acme_control {
    uint32_t version;
    bool initialized;
    /* The operator's setting: off until an operator turned it on. */
    bool enabled;
    /* One pending issuance request, consumed by the order path. */
    bool order_requested;
    /* The instant the request was made; zero when the clock was unset. */
    uint32_t order_requested_at;
};

struct appliance_acme_control_store {
    bool initialized;
    bool bound;
    uint32_t version;
    struct appliance_state_block_device store;
    enum appliance_acme_control_source source;
    enum appliance_acme_control_status last_status;
    uint32_t load_count;
    uint32_t save_count;
};

/* The off state: ACME disabled and no order requested. */
void appliance_acme_control_init(
    struct appliance_acme_control *control);
void appliance_acme_control_store_init(
    struct appliance_acme_control_store *store);

/*
 * Bind the store from the loader and read the record.  The call always
 * publishes a control value: a missing, unreadable or corrupt record yields
 * the off state and a named source, never half a setting.
 */
enum appliance_acme_control_status appliance_acme_control_store_load(
    struct appliance_acme_control_store *store,
    struct appliance_state_loader *loader,
    struct appliance_acme_control *control_out);

/*
 * Write the setting and read it back.  Returns OK only when the record on the
 * device byte-for-byte equals the record that was written.
 */
enum appliance_acme_control_status appliance_acme_control_store_save(
    struct appliance_acme_control_store *store,
    const struct appliance_acme_control *control);

/*
 * Enable or disable ACME.  Disabling clears a pending order request; enabling
 * leaves the request as it was.
 */
enum appliance_acme_control_status appliance_acme_control_set_enabled(
    struct appliance_acme_control *control,
    bool enabled);

/*
 * Request one order.  Refused while ACME is disabled or while a request is
 * already pending; `now_epoch_seconds` is recorded as the request's instant
 * and may be zero when the caller has no validated clock yet.
 */
enum appliance_acme_control_status appliance_acme_control_request_order(
    struct appliance_acme_control *control,
    uint32_t now_epoch_seconds);

/* Consume the pending request; the order path calls this once it took it. */
enum appliance_acme_control_status appliance_acme_control_take_request(
    struct appliance_acme_control *control);

bool appliance_acme_control_enabled(
    const struct appliance_acme_control *control);
bool appliance_acme_control_order_requested(
    const struct appliance_acme_control *control);
uint32_t appliance_acme_control_order_requested_at(
    const struct appliance_acme_control *control);
enum appliance_acme_control_source appliance_acme_control_store_source(
    const struct appliance_acme_control_store *store);

const char *appliance_acme_control_status_string(
    enum appliance_acme_control_status status);
const char *appliance_acme_control_source_string(
    enum appliance_acme_control_source source);

#endif
