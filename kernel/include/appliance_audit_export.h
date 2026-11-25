#ifndef HFOC_APPLIANCE_AUDIT_EXPORT_H
#define HFOC_APPLIANCE_AUDIT_EXPORT_H

#include "appliance_audit_chain.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-377 cursor-based external audit export.
 *
 * An external consumer (a log collector, a support bundle, a customer SIEM)
 * cannot read the kernel's chain structure directly, and it must never be
 * silently handed a gap: if the chain rotated while the consumer was away, the
 * consumer has to learn that its cursor expired instead of receiving a stream
 * that looks contiguous.  This module owns that cursor.
 *
 * A cursor is caller-owned and binds to one chain generation.  `begin` accepts
 * an absolute start sequence, or 0 for "oldest retained record"; a start that
 * is older than the retained window is reported as EXPIRED with a declared gap,
 * and the cursor resynchronizes to the oldest retained sequence so the consumer
 * can continue instead of stalling.  `next` exports exactly one record with its
 * chain digest in order, `batch` exports a bounded run of records, and `ack`
 * accepts only a contiguous acknowledgement of the last exported sequence, so
 * an out-of-order or duplicated acknowledgement is refused.
 *
 * A record is exported only after the chain verified it: `begin` verifies the
 * whole retained window and refuses to open a cursor on a chain that does not
 * verify, so a tampered window is detected before any of its bytes leave the
 * appliance.
 *
 * The module is allocation-free and fully host-testable.
 */

#define APPLIANCE_AUDIT_EXPORT_VERSION 1U
#define APPLIANCE_AUDIT_EXPORT_RENDER_MAX 288U

enum appliance_audit_export_status {
    APPLIANCE_AUDIT_EXPORT_OK = 0,
    APPLIANCE_AUDIT_EXPORT_NULL = 1,
    APPLIANCE_AUDIT_EXPORT_BAD_VERSION = 2,
    APPLIANCE_AUDIT_EXPORT_BAD_STATE = 3,
    APPLIANCE_AUDIT_EXPORT_EMPTY = 4,
    APPLIANCE_AUDIT_EXPORT_EXPIRED = 5,
    APPLIANCE_AUDIT_EXPORT_OUT_OF_RANGE = 6,
    APPLIANCE_AUDIT_EXPORT_OUT_OF_ORDER = 7,
    APPLIANCE_AUDIT_EXPORT_NOT_ACTIVE = 8,
    APPLIANCE_AUDIT_EXPORT_ALREADY_ACTIVE = 9,
    APPLIANCE_AUDIT_EXPORT_BUFFER_TOO_SMALL = 10,
    APPLIANCE_AUDIT_EXPORT_STALE_GENERATION = 11,
    APPLIANCE_AUDIT_EXPORT_BAD_ARGUMENT = 12,
    APPLIANCE_AUDIT_EXPORT_NOT_FOUND = 13,
};

enum appliance_audit_export_state {
    APPLIANCE_AUDIT_EXPORT_STATE_NONE = 0,
    APPLIANCE_AUDIT_EXPORT_STATE_IDLE = 1,
    APPLIANCE_AUDIT_EXPORT_STATE_ACTIVE = 2,
    APPLIANCE_AUDIT_EXPORT_STATE_COMPLETE = 3,
    APPLIANCE_AUDIT_EXPORT_STATE_EXPIRED = 4,
};

struct appliance_audit_export_cursor {
    uint32_t version;
    bool active;
    bool gap_declared;
    uint64_t generation;
    uint64_t first_sequence;
    uint64_t next_sequence;
    uint64_t end_sequence;
    uint64_t last_exported_sequence;
    uint64_t last_ack_sequence;
    uint64_t exported_count;
    uint64_t ack_count;
    uint64_t gap_count;
};

struct appliance_audit_export_snapshot {
    uint32_t version;
    bool published;
    enum appliance_audit_export_state state;
    uint64_t generation;
    uint64_t first_sequence;
    uint64_t next_sequence;
    uint64_t end_sequence;
    uint64_t last_exported_sequence;
    uint64_t last_ack_sequence;
    uint64_t exported_count;
    uint64_t ack_count;
    uint64_t gap_count;
};

enum appliance_audit_export_status appliance_audit_export_cursor_init(
    struct appliance_audit_export_cursor *cursor);
enum appliance_audit_export_status appliance_audit_export_begin(
    const struct appliance_audit_chain *chain,
    struct appliance_audit_export_cursor *cursor,
    uint64_t requested_sequence);
enum appliance_audit_export_status appliance_audit_export_next(
    const struct appliance_audit_chain *chain,
    struct appliance_audit_export_cursor *cursor,
    struct audit_record *record_out,
    uint8_t digest_out[SHA256_DIGEST_SIZE],
    uint64_t *sequence_out);
enum appliance_audit_export_status appliance_audit_export_batch(
    const struct appliance_audit_chain *chain,
    struct appliance_audit_export_cursor *cursor,
    struct audit_record *records_out,
    uint8_t *digests_out,
    uint32_t capacity,
    uint32_t *count_out);
enum appliance_audit_export_status appliance_audit_export_ack(
    struct appliance_audit_export_cursor *cursor,
    uint64_t sequence);
enum appliance_audit_export_status appliance_audit_export_snapshot(
    const struct appliance_audit_export_cursor *cursor,
    struct appliance_audit_export_snapshot *snapshot_out);
enum appliance_audit_export_status appliance_audit_export_render(
    const struct appliance_audit_export_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length);
bool appliance_audit_export_cursor_active(
    const struct appliance_audit_export_cursor *cursor);
uint64_t appliance_audit_export_next_sequence(
    const struct appliance_audit_export_cursor *cursor);
uint64_t appliance_audit_export_last_ack_sequence(
    const struct appliance_audit_export_cursor *cursor);
uint64_t appliance_audit_export_gap_count(
    const struct appliance_audit_export_cursor *cursor);
const char *appliance_audit_export_status_string(
    enum appliance_audit_export_status status);
const char *appliance_audit_export_state_string(
    enum appliance_audit_export_state state);

#endif
