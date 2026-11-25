#ifndef HFOC_APPLIANCE_ROUTE_STORE_H
#define HFOC_APPLIANCE_ROUTE_STORE_H

#include "appliance_route_table.h"
#include "appliance_state_block_device.h"
#include "appliance_state_loader.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-504 increment 6: the route table as durable state.
 *
 * The table the operator configured in the panel is the facade's access
 * decision, so it has to survive the reboot the operator will eventually do.
 * This module owns exactly that: one bounded record in the SYSTEM_CONFIG
 * namespace's own store, encoded from `appliance_route_table` and decoded back
 * through the model's own `appliance_route_table_validate`, so a table read
 * from the disk is checked by the same rules a table built from the panel is.
 *
 * The record is deliberately a binary value and not the textual rendering the
 * panel shows: the rendering is a presentation and it is allowed to grow, the
 * record is a format with a version and a checksum that a future boot has to
 * read byte for byte.  The body is little-endian, length-prefixed per route
 * and bounded by the model's own limits:
 *
 *   header  magic "RTRT", version, route count, SHA-256(body) truncated to
 *           the first four bytes;
 *   body    per route: name length, four backend address bytes, backend port,
 *           TLS flag, source, then the normalized name (no terminator).
 *
 * Fail-closed, in both directions:
 *
 *   * a record that is absent, corrupt, of another version or failing its
 *     checksum never yields a partial table.  `load` reports the reason and
 *     hands out the empty table, which refuses every name;
 *   * `save` writes the record and reads it back before it reports OK.  A
 *     store that cannot prove the record (unavailable, sealed, a short write,
 *     a read-back that disagrees) returns a failure, and the caller must not
 *     apply the change in memory either: the panel never shows a route the
 *     next boot would not carry.
 *
 * The store is bound from the same `appliance_state_layout` the rest of the
 * state volume uses; a layout without the route store (an older volume) is
 * honestly reported as "no stored routes" instead of reading a neighbour.
 */

#define APPLIANCE_ROUTE_STORE_VERSION 1U
/* "RTRT" in little-endian byte order: 0x52 0x54 0x52 0x54. */
#define APPLIANCE_ROUTE_STORE_MAGIC UINT32_C(0x54525452)
#define APPLIANCE_ROUTE_STORE_RECORD_VERSION 1U
/*
 * Eight blocks (4 KiB) hold the header plus the largest legal table: eight
 * routes of a 253-byte name and ten fixed bytes each is 2104 bytes.  The
 * store itself is 64 blocks, so the record is a bounded prefix of it.
 */
#define APPLIANCE_ROUTE_STORE_RECORD_BLOCKS 8U
#define APPLIANCE_ROUTE_STORE_RECORD_BYTES \
    (APPLIANCE_ROUTE_STORE_RECORD_BLOCKS * APPLIANCE_STATE_BLOCK_SIZE)
#define APPLIANCE_ROUTE_STORE_HEADER_BYTES 16U
#define APPLIANCE_ROUTE_STORE_ENTRY_FIXED_BYTES 10U
#define APPLIANCE_ROUTE_STORE_BODY_MAX \
    (APPLIANCE_ROUTE_TABLE_MAX * \
     (APPLIANCE_ROUTE_STORE_ENTRY_FIXED_BYTES + APPLIANCE_ROUTE_NAME_MAX))

enum appliance_route_store_status {
    APPLIANCE_ROUTE_STORE_OK = 0,
    APPLIANCE_ROUTE_STORE_NULL = 1,
    APPLIANCE_ROUTE_STORE_BAD_STATE = 2,
    /* No state volume, or a layout an older volume does not carry. */
    APPLIANCE_ROUTE_STORE_STORE_UNAVAILABLE = 3,
    /* A sealed volume is never written in the clear. */
    APPLIANCE_ROUTE_STORE_STORE_SEALED = 4,
    APPLIANCE_ROUTE_STORE_READ_FAILED = 5,
    APPLIANCE_ROUTE_STORE_WRITE_FAILED = 6,
    APPLIANCE_ROUTE_STORE_RECORD_CORRUPT = 7,
    APPLIANCE_ROUTE_STORE_RECORD_CHECKSUM = 8,
    /* The decoded table did not pass the model's own validate(). */
    APPLIANCE_ROUTE_STORE_TABLE_REFUSED = 9,
    APPLIANCE_ROUTE_STORE_TABLE_TOO_LARGE = 10,
};

/*
 * Where the table in hand came from.  `DEFAULT_MISSING` is a store that was
 * read successfully and simply holds no route record yet; `DEFAULT_INVALID`
 * is a record that exists and could not be trusted; `DEFAULT_UNAVAILABLE` is
 * no store at all.  All three mean the same thing to the facade - an empty
 * table that refuses every name - and differ only in what the boot publishes.
 */
enum appliance_route_store_source {
    APPLIANCE_ROUTE_STORE_SOURCE_NONE = 0,
    APPLIANCE_ROUTE_STORE_SOURCE_COMMITTED = 1,
    APPLIANCE_ROUTE_STORE_SOURCE_DEFAULT_MISSING = 2,
    APPLIANCE_ROUTE_STORE_SOURCE_DEFAULT_INVALID = 3,
    APPLIANCE_ROUTE_STORE_SOURCE_DEFAULT_UNAVAILABLE = 4,
};

struct appliance_route_store {
    bool initialized;
    bool bound;
    uint32_t version;
    struct appliance_state_block_device store;
    enum appliance_route_store_source source;
    enum appliance_route_store_status last_status;
    uint32_t load_count;
    uint32_t save_count;
};

void appliance_route_store_init(struct appliance_route_store *store);

/*
 * Bind the store from the loader and read the record.  The call always
 * succeeds in publishing a table: a missing, unreadable or corrupt record
 * yields the empty table and a source that says which of the three it was.  A
 * NULL store or a store that was not initialized is the caller's error and
 * returns NULL; the table is still initialized empty.
 */
enum appliance_route_store_status appliance_route_store_load(
    struct appliance_route_store *store,
    struct appliance_state_loader *loader,
    struct appliance_route_table *table_out);

/*
 * Write the table and read it back.  Returns OK only when the record on the
 * device byte-for-byte equals the record that was written, so the caller can
 * refuse the in-memory commit when the durable state did not take.
 */
enum appliance_route_store_status appliance_route_store_save(
    struct appliance_route_store *store,
    const struct appliance_route_table *table);

bool appliance_route_store_available(const struct appliance_route_store *store);
enum appliance_route_store_source appliance_route_store_source(
    const struct appliance_route_store *store);
enum appliance_route_store_status appliance_route_store_last_status(
    const struct appliance_route_store *store);
uint32_t appliance_route_store_save_count(
    const struct appliance_route_store *store);
const char *appliance_route_store_source_string(
    enum appliance_route_store_source source);
const char *appliance_route_store_status_string(
    enum appliance_route_store_status status);

#endif
