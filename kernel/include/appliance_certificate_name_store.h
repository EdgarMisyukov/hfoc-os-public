#ifndef HFOC_APPLIANCE_CERTIFICATE_NAME_STORE_H
#define HFOC_APPLIANCE_CERTIFICATE_NAME_STORE_H

#include "appliance_production_certificate.h"
#include "appliance_state_block_device.h"
#include "appliance_state_loader.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-507 increment 2: the durable "name -> certificate and key" table.
 *
 * ITEM-507 serves a different certificate per public DNS name.  The profile
 * increment made SNI possible; this increment owns the declaration it selects
 * from: a bounded table of exact names, each with the certificate DER and the
 * private-key DER the operator imported for that name, written to the
 * appliance's own encrypted state volume so it survives the reboot that the
 * panel promises the operator.
 *
 * The record lives next to the ITEM-503 certificate record, in the same
 * CERTIFICATE-namespace store, as a bounded prefix that starts after the
 * twelve blocks that record owns.  It is deliberately a binary value: the
 * panel rendering is a presentation and may grow, while this record is a
 * format with a version and a checksum a future boot has to read byte for
 * byte.  The body is little-endian, length-prefixed per entry and bounded by
 * this module's own limits:
 *
 *   header  magic "CNST", version, entry count, SHA-256(body) truncated to the
 *           first four bytes;
 *   body    the default-certificate declaration (u32), then per entry: name
 *           length, certificate length, key length, reserved, then the name
 *           (normalized to lower case, no terminator), the certificate DER and
 *           the key DER.
 *
 * Fail-closed, in both directions:
 *
 *   * a record that is absent, corrupt, of another version or failing its
 *     checksum never yields a partial table.  `load` reports the reason and
 *     hands out the empty table, which selects nothing;
 *   * `save` writes the record and reads it back before it reports OK.  A
 *     store that cannot prove the record (unavailable, sealed, a short write,
 *     a read-back that disagrees) returns a failure, and the caller must not
 *     apply the change in memory either: the panel never shows a name the
 *     next boot would not carry.
 *
 * The private key never leaves this store as text: it is never rendered,
 * logged or hashed into a marker, and the model's own renderer names only the
 * certificate's byte count and the presence of the key.
 */

#define APPLIANCE_CERTIFICATE_NAME_STORE_VERSION 1U
/* "CNST" in little-endian byte order: 0x43 0x4E 0x53 0x54. */
#define APPLIANCE_CERTIFICATE_NAME_STORE_MAGIC UINT32_C(0x54534E43)
#define APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_VERSION 1U

/* One exact DNS name, its certificate DER and its private-key DER. */
#define APPLIANCE_CERTIFICATE_NAME_MAX 253U
/*
 * The DER bounds mirror the ITEM-503 record bounds: a name certificate is the
 * same kind of object as the appliance identity certificate, and a bound that
 * differed would let one record accept what the other refuses.
 */
#define APPLIANCE_CERTIFICATE_NAME_DER_MAX \
    APPLIANCE_PRODUCTION_CERTIFICATE_DER_MAX
#define APPLIANCE_CERTIFICATE_NAME_KEY_DER_MAX \
    APPLIANCE_PRODUCTION_CERTIFICATE_KEY_DER_MAX
/*
 * Eight names is the route table maximum: point 7 of the item requires that a
 * name routed to a backend has a certificate, so a table smaller than the
 * route table could not satisfy it.
 */
#define APPLIANCE_CERTIFICATE_NAME_MAX_COUNT 8U
/* A bounded rendering of one entry, never truncating. */
#define APPLIANCE_CERTIFICATE_NAME_RECORD_MAX 320U

#define APPLIANCE_CERTIFICATE_NAME_STORE_HEADER_BYTES 16U
#define APPLIANCE_CERTIFICATE_NAME_STORE_BODY_FLAGS_BYTES 4U
#define APPLIANCE_CERTIFICATE_NAME_STORE_ENTRY_FIXED_BYTES 8U
#define APPLIANCE_CERTIFICATE_NAME_STORE_ENTRY_MAX \
    (APPLIANCE_CERTIFICATE_NAME_STORE_ENTRY_FIXED_BYTES + \
     APPLIANCE_CERTIFICATE_NAME_MAX + \
     APPLIANCE_CERTIFICATE_NAME_DER_MAX + \
     APPLIANCE_CERTIFICATE_NAME_KEY_DER_MAX)
#define APPLIANCE_CERTIFICATE_NAME_STORE_BODY_MAX \
    (APPLIANCE_CERTIFICATE_NAME_STORE_BODY_FLAGS_BYTES + \
     (APPLIANCE_CERTIFICATE_NAME_MAX_COUNT * \
      APPLIANCE_CERTIFICATE_NAME_STORE_ENTRY_MAX))
/*
 * The record starts after the twelve blocks the ITEM-503 certificate record
 * owns in the shared sixty-four-block CERTIFICATE store, and thirty-two blocks
 * (16384 bytes) hold the header plus the largest legal table (14392 bytes).
 */
#define APPLIANCE_CERTIFICATE_NAME_STORE_BLOCK_OFFSET \
    APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_BLOCKS
#define APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_BLOCKS 32U
#define APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_BYTES \
    (APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_BLOCKS * \
     APPLIANCE_STATE_BLOCK_SIZE)

enum appliance_certificate_name_status {
    APPLIANCE_CERTIFICATE_NAME_OK = 0,
    APPLIANCE_CERTIFICATE_NAME_NULL = 1,
    APPLIANCE_CERTIFICATE_NAME_BAD_VERSION = 2,
    APPLIANCE_CERTIFICATE_NAME_BAD_NAME = 3,
    APPLIANCE_CERTIFICATE_NAME_NAME_TOO_LONG = 4,
    APPLIANCE_CERTIFICATE_NAME_BAD_CERTIFICATE = 5,
    APPLIANCE_CERTIFICATE_NAME_BAD_KEY = 6,
    APPLIANCE_CERTIFICATE_NAME_DUPLICATE_NAME = 7,
    APPLIANCE_CERTIFICATE_NAME_TABLE_FULL = 8,
    APPLIANCE_CERTIFICATE_NAME_NOT_FOUND = 9,
    APPLIANCE_CERTIFICATE_NAME_BUFFER_TOO_SMALL = 10,
    /*
     * ITEM-507 increment 4: the name is routed to a backend, but no
     * certificate is declared for it.  The handshake is refused even when a
     * default certificate is declared: a routed name never silently borrows
     * another identity.
     */
    APPLIANCE_CERTIFICATE_NAME_ROUTE_WITHOUT_CERTIFICATE = 11,
};

enum appliance_certificate_name_store_status {
    APPLIANCE_CERTIFICATE_NAME_STORE_OK = 0,
    APPLIANCE_CERTIFICATE_NAME_STORE_NULL = 1,
    APPLIANCE_CERTIFICATE_NAME_STORE_BAD_STATE = 2,
    /* No state volume, or a layout an older volume does not carry. */
    APPLIANCE_CERTIFICATE_NAME_STORE_STORE_UNAVAILABLE = 3,
    /* A sealed volume is never written in the clear. */
    APPLIANCE_CERTIFICATE_NAME_STORE_STORE_SEALED = 4,
    APPLIANCE_CERTIFICATE_NAME_STORE_READ_FAILED = 5,
    APPLIANCE_CERTIFICATE_NAME_STORE_WRITE_FAILED = 6,
    APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_CORRUPT = 7,
    APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_CHECKSUM = 8,
    /* The decoded table did not pass the model's own validate(). */
    APPLIANCE_CERTIFICATE_NAME_STORE_TABLE_REFUSED = 9,
    APPLIANCE_CERTIFICATE_NAME_STORE_TABLE_TOO_LARGE = 10,
};

/*
 * Where the table in hand came from.  `DEFAULT_MISSING` is a store that was
 * read successfully and simply holds no name record yet; `DEFAULT_INVALID` is
 * a record that exists and could not be trusted; `DEFAULT_UNAVAILABLE` is no
 * store at all.  All three mean the same thing to the facade - an empty table
 * that selects nothing - and differ only in what the boot publishes.
 */
enum appliance_certificate_name_store_source {
    APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_NONE = 0,
    APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_COMMITTED = 1,
    APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_DEFAULT_MISSING = 2,
    APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_DEFAULT_INVALID = 3,
    APPLIANCE_CERTIFICATE_NAME_STORE_SOURCE_DEFAULT_UNAVAILABLE = 4,
};

enum appliance_certificate_name_default {
    /* An unknown name is refused; the handshake never sees a borrowed identity. */
    APPLIANCE_CERTIFICATE_NAME_DEFAULT_REFUSE = 0,
    /* An unknown name is served the explicitly declared default certificate. */
    APPLIANCE_CERTIFICATE_NAME_DEFAULT_CERTIFICATE = 1,
};

struct appliance_certificate_name {
    /* Normalized (lower case, no trailing dot) and NUL-terminated. */
    char name[APPLIANCE_CERTIFICATE_NAME_MAX + 1U];
    uint32_t name_length;
    uint8_t certificate_der[APPLIANCE_CERTIFICATE_NAME_DER_MAX];
    uint32_t certificate_length;
    uint8_t key_der[APPLIANCE_CERTIFICATE_NAME_KEY_DER_MAX];
    uint32_t key_length;
};

struct appliance_certificate_name_table {
    uint32_t version;
    uint32_t count;
    /*
     * The operator's explicit declaration of what an unknown SNI name gets.
     * REFUSE is the default: a name the table does not carry is refused
     * rather than quietly served another identity.
     */
    uint32_t default_mode;
    struct appliance_certificate_name entries[
        APPLIANCE_CERTIFICATE_NAME_MAX_COUNT];
};

struct appliance_certificate_name_store {
    bool initialized;
    bool bound;
    uint32_t version;
    struct appliance_state_block_device store;
    enum appliance_certificate_name_store_source source;
    enum appliance_certificate_name_store_status last_status;
    uint32_t load_count;
    uint32_t save_count;
};

/* An empty table is a valid table: it declares no name and selects nothing. */
void appliance_certificate_name_table_init(
    struct appliance_certificate_name_table *table);

/*
 * Add one name.  The name is normalized to lower case and validated as an
 * exact DNS name (no wildcard, no trailing dot), the certificate and key must
 * be non-empty and within the DER bounds, and a duplicate name in any case is
 * refused.  Nothing is stored on any refusal.
 */
enum appliance_certificate_name_status appliance_certificate_name_table_add(
    struct appliance_certificate_name_table *table,
    const char *name,
    size_t name_length,
    const uint8_t *certificate_der,
    size_t certificate_length,
    const uint8_t *key_der,
    size_t key_length);

/*
 * Declare what an unknown SNI name is served: REFUSE (the default) or the
 * default certificate.  `default_mode` is stored in the record, so the choice
 * is configuration the next boot reads back, not a code guess.  An unknown
 * mode is refused and the table is left unchanged.
 */
enum appliance_certificate_name_status
appliance_certificate_name_table_set_default(
    struct appliance_certificate_name_table *table,
    enum appliance_certificate_name_default default_mode);

/* Remove one name; a name the table does not carry is NOT_FOUND. */
enum appliance_certificate_name_status appliance_certificate_name_table_remove(
    struct appliance_certificate_name_table *table,
    const char *name,
    size_t name_length);

/*
 * The selection.  `name` is the SNI name, not a Host header: a malformed name
 * is BAD_NAME and a well-formed name the table does not carry is NOT_FOUND, so
 * the caller can publish why the handshake was refused.  The matched entry is
 * returned as a borrowed pointer into the caller-owned table, never copied.
 */
enum appliance_certificate_name_status appliance_certificate_name_table_lookup(
    const struct appliance_certificate_name_table *table,
    const char *name,
    size_t name_length,
    const struct appliance_certificate_name **entry_out);

/*
 * The model's own validation, applied to a table built from the disk by the
 * store's decoder.  BAD_VERSION, BAD_NAME, too-long names, duplicate names and
 * entries over the count are refused; an empty table is valid.
 */
enum appliance_certificate_name_status
appliance_certificate_name_table_validate(
    const struct appliance_certificate_name_table *table);

/*
 * A bounded presentation of one entry: `name=... certificate_bytes=...
 * key=present`.  It never renders the certificate body or any key byte, and
 * refuses a buffer that cannot hold the whole record instead of truncating it.
 */
enum appliance_certificate_name_status appliance_certificate_name_render(
    const struct appliance_certificate_name *entry,
    char *buffer,
    size_t buffer_size,
    size_t *length_out);

/*
 * ITEM-507 increment 3: what the handshake does with the name the client
 * presented.  Increment 4 adds the routing consistency of point 7.
 *
 *   EXACT    the table carries this exact name (in any case); `entry_out` is
 *            that entry and the handshake serves its certificate;
 *   DEFAULT  no name was presented, or the name is unknown and the record
 *            explicitly declares the default certificate; the handshake keeps
 *            the appliance's own configured certificate;
 *   REFUSED  the name is unknown (or malformed) and the record does not
 *            declare a default: the handshake is refused rather than served an
 *            identity the operator did not declare for it.
 *
 * A client without SNI is not an unknown *name*, so it always gets the
 * declared default.  The returned status is the lookup's own reason
 * (`NOT_FOUND`/`BAD_NAME`) so the caller can publish why it refused; a NULL
 * table or a table of another version is the caller's error and refuses.
 *
 * `name_is_routed` is the route table's answer for the same name: a name the
 * facade routes to a backend but for which no certificate is declared is
 * refused even when a default certificate is declared, with
 * `ROUTE_WITHOUT_CERTIFICATE` as the reason - a routed name never silently
 * borrows another identity.
 */
enum appliance_certificate_name_selection {
    APPLIANCE_CERTIFICATE_NAME_SELECTION_EXACT = 0,
    APPLIANCE_CERTIFICATE_NAME_SELECTION_DEFAULT = 1,
    APPLIANCE_CERTIFICATE_NAME_SELECTION_REFUSED = 2,
};

enum appliance_certificate_name_status appliance_certificate_name_select(
    const struct appliance_certificate_name_table *table,
    const char *name,
    size_t name_length,
    bool name_is_routed,
    enum appliance_certificate_name_selection *selection_out,
    const struct appliance_certificate_name **entry_out);

void appliance_certificate_name_store_init(
    struct appliance_certificate_name_store *store);

/*
 * Bind the store from the loader and read the record.  The call always
 * succeeds in publishing a table: a missing, unreadable or corrupt record
 * yields the empty table and a source that says which of the three it was.  A
 * NULL store or a store that was not initialized is the caller's error and
 * returns NULL; the table is still initialized empty.
 */
enum appliance_certificate_name_store_status
appliance_certificate_name_store_load(
    struct appliance_certificate_name_store *store,
    struct appliance_state_loader *loader,
    struct appliance_certificate_name_table *table_out);

/*
 * Write the table and read it back.  Returns OK only when the record on the
 * device byte-for-byte equals the record that was written, so the caller can
 * refuse the in-memory commit when the durable state did not take.
 */
enum appliance_certificate_name_store_status
appliance_certificate_name_store_save(
    struct appliance_certificate_name_store *store,
    const struct appliance_certificate_name_table *table);

bool appliance_certificate_name_store_available(
    const struct appliance_certificate_name_store *store);
enum appliance_certificate_name_store_source
appliance_certificate_name_store_source(
    const struct appliance_certificate_name_store *store);
enum appliance_certificate_name_store_status
appliance_certificate_name_store_last_status(
    const struct appliance_certificate_name_store *store);
uint32_t appliance_certificate_name_store_save_count(
    const struct appliance_certificate_name_store *store);
const char *appliance_certificate_name_status_string(
    enum appliance_certificate_name_status status);
const char *appliance_certificate_name_store_source_string(
    enum appliance_certificate_name_store_source source);
const char *appliance_certificate_name_store_status_string(
    enum appliance_certificate_name_store_status status);

#endif
