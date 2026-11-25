#ifndef HFOC_APPLIANCE_ROUTE_CONFIG_H
#define HFOC_APPLIANCE_ROUTE_CONFIG_H

#include "appliance_route_table.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-504 part B: the route table as the two-phase mutation plane stages and
 * commits it.
 *
 * The route table (ITEM-504 part A) owns the grammar and the fail-closed
 * lookup.  This module owns the candidate pair: the table the machine applies
 * now, and the table a staged mutation would replace it with.  A stage parses
 * one `application/x-www-form-urlencoded` body, applies exactly one add or
 * remove to a copy of the applied table through the model's own validated
 * functions, and holds the copy.  Nothing durable is written before the commit
 * and the applied table is only ever replaced whole, so a failed or cancelled
 * mutation leaves the previous table exactly as it was.
 *
 * The module owns no store and no socket: the mutation plane owns the
 * authorization, the single-use token and the audit, and the serving path owns
 * the HTTP targets.  This module never authenticates anything.
 *
 * The body grammar is deliberately small and fail-closed:
 *
 *   action=add|remove      (optional; `add` when absent)
 *   name=<public name>     (required)
 *   backend=<literal IPv4> (required for add)
 *   port=<1..65535>        (required for add)
 *   tls=on|off             (optional for add; `off` when absent)
 *
 * Values are not percent-decoded in this increment: the panel and the API
 * submit tokens the grammar accepts literally, and a value carrying `%`, `+`
 * or any other encoding byte is refused as malformed rather than guessed at.
 *
 * ITEM-504 increment 5: every stage that reaches a terminal state leaves one
 * bounded record body, and the machine publishes it as `[PRODUCTION-ROUTE]`.
 * A commit names the route it applied (`result=APPLIED`), a cancellation of a
 * held candidate names the route it dropped (`result=CANCELLED`), and a
 * refusal that applied nothing names the requested name and the model's own
 * reason (`result=REFUSED reason=DUPLICATE_NAME`).  The composition holds one
 * record slot, so a drain that never happens loses the older record rather
 * than growing without bound; `take` hands the record over exactly once and
 * refuses, without dropping it, a buffer that cannot hold the whole body.
 */

#define APPLIANCE_ROUTE_CONFIG_VERSION 1U
#define APPLIANCE_ROUTE_CONFIG_CONTENT_TYPE \
    "application/x-www-form-urlencoded"
#define APPLIANCE_ROUTE_CONFIG_CONTENT_TYPE_LENGTH 33U
/* The frozen request framing bound, exactly like the certificate import. */
#define APPLIANCE_ROUTE_CONFIG_BODY_MAX 8192U
#define APPLIANCE_ROUTE_CONFIG_NAME_MAX APPLIANCE_ROUTE_NAME_MAX
#define APPLIANCE_ROUTE_CONFIG_BACKEND_MAX 15U
#define APPLIANCE_ROUTE_CONFIG_PORT_MAX 5U
#define APPLIANCE_ROUTE_CONFIG_FLAG_MAX 3U
#define APPLIANCE_ROUTE_CONFIG_ACTION_MAX 6U
/*
 * One whole record: the route body `appliance_route_render` writes (320 bytes:
 * the 253-byte name maximum, the 15-byte address, a five-digit port, the
 * longest source token and the fixed words) plus ` result=`, the longest
 * result word and the longest refusal suffix (` reason=DUPLICATE_NAME`).
 */
#define APPLIANCE_ROUTE_CONFIG_RECORD_MAX (APPLIANCE_ROUTE_RECORD_MAX + 64U)

enum appliance_route_config_status {
    APPLIANCE_ROUTE_CONFIG_OK = 0,
    APPLIANCE_ROUTE_CONFIG_NULL = 1,
    APPLIANCE_ROUTE_CONFIG_BAD_VERSION = 2,
    APPLIANCE_ROUTE_CONFIG_BAD_CONTENT_TYPE = 3,
    APPLIANCE_ROUTE_CONFIG_MALFORMED = 4,
    APPLIANCE_ROUTE_CONFIG_MISSING_FIELD = 5,
    APPLIANCE_ROUTE_CONFIG_BAD_ACTION = 6,
    APPLIANCE_ROUTE_CONFIG_BAD_FLAG = 7,
    APPLIANCE_ROUTE_CONFIG_NO_CANDIDATE = 8,
    /*
     * The model refused the candidate.  The model's own status is kept in
     * `last_route_status`, so a refusal names `DUPLICATE_NAME`, `TABLE_FULL`,
     * `BAD_BACKEND`, `NOT_FOUND` and so on instead of a generic failure.
     */
    APPLIANCE_ROUTE_CONFIG_REFUSED = 9,
};

/*
 * What the last record says happened.  `NONE` means no record was produced
 * yet; it is not a fourth outcome.  The result word is rendered into the
 * record body itself, so a reader of the serial line needs no lookup.
 */
enum appliance_route_config_record_result {
    APPLIANCE_ROUTE_CONFIG_RECORD_NONE = 0,
    APPLIANCE_ROUTE_CONFIG_RECORD_APPLIED = 1,
    APPLIANCE_ROUTE_CONFIG_RECORD_REFUSED = 2,
    APPLIANCE_ROUTE_CONFIG_RECORD_CANCELLED = 3,
};

struct appliance_route_config {
    bool initialized;
    uint32_t version;
    bool staged;
    struct appliance_route_table applied;
    struct appliance_route_table candidate;
    enum appliance_route_config_status last_status;
    enum appliance_route_status last_route_status;
    uint32_t stage_count;
    uint32_t commit_count;
    uint32_t rollback_count;
    uint32_t refused_count;
    /*
     * The operation a held candidate would apply, remembered so the record a
     * terminal state produces does not have to re-parse the body.  For a
     * remove it is the route the applied table carried before the change, for
     * an add the route the candidate accepted after the model normalized it.
     */
    bool operation_set;
    struct appliance_route operation;
    /* One bounded record slot, drained by `take_record` exactly once. */
    char record[APPLIANCE_ROUTE_CONFIG_RECORD_MAX + 1U];
    size_t record_length;
    bool record_pending;
    enum appliance_route_config_record_result record_result;
    uint32_t record_count;
};

void appliance_route_config_init(struct appliance_route_config *config);
/*
 * Decode one form body and hold the candidate.  `content_type` must be the
 * form content type; a body that does not parse, names a field twice, carries
 * an unknown field, or asks the model for a route it refuses applies nothing.
 */
enum appliance_route_config_status appliance_route_config_stage(
    struct appliance_route_config *config,
    const uint8_t *content_type,
    uint32_t content_type_length,
    const uint8_t *body,
    uint32_t body_length);
/* Replace the applied table with the candidate, whole. */
enum appliance_route_config_status appliance_route_config_commit(
    struct appliance_route_config *config);
/* Drop the candidate; the applied table was never touched. */
enum appliance_route_config_status appliance_route_config_rollback(
    struct appliance_route_config *config);
const struct appliance_route_table *appliance_route_config_applied(
    const struct appliance_route_config *config);
/*
 * ITEM-504 increment 6: the candidate a commit would apply, or NULL when
 * nothing is staged.  The serving path persists exactly this table before it
 * lets the commit replace the applied one, so the durable table and the table
 * in force are decided from the same value.
 */
const struct appliance_route_table *appliance_route_config_candidate(
    const struct appliance_route_config *config);
/*
 * ITEM-504 increment 6: seed the applied table from durable state.  The table
 * is validated by the model before it replaces anything; a table that does not
 * pass is refused whole and the applied table is left exactly as it was.  This
 * is the only way a table the panel did not build enters the composition, so
 * it is also the only place a stored record has to pass the model's rules.
 */
enum appliance_route_config_status appliance_route_config_restore(
    struct appliance_route_config *config,
    const struct appliance_route_table *table);
bool appliance_route_config_staged(const struct appliance_route_config *config);
enum appliance_route_config_status appliance_route_config_last_status(
    const struct appliance_route_config *config);
enum appliance_route_status appliance_route_config_last_route_status(
    const struct appliance_route_config *config);
uint32_t appliance_route_config_stage_count(
    const struct appliance_route_config *config);
uint32_t appliance_route_config_commit_count(
    const struct appliance_route_config *config);
uint32_t appliance_route_config_rollback_count(
    const struct appliance_route_config *config);
uint32_t appliance_route_config_refused_count(
    const struct appliance_route_config *config);
/*
 * ITEM-504 increment 5: hand over the record the last terminal state left,
 * exactly once.  The body is NUL-terminated in `buffer` and its length (without
 * the terminator) is stored in `length_out`.  A buffer that cannot hold the
 * whole body returns false and keeps the record pending, so a short buffer
 * loses nothing.  False also means "no record": a stage that only holds a
 * candidate produces none until it is committed or cancelled.
 */
bool appliance_route_config_take_record(
    struct appliance_route_config *config,
    char *buffer,
    size_t capacity,
    size_t *length_out);
/* What the record slot says happened; NONE when nothing was recorded. */
enum appliance_route_config_record_result appliance_route_config_record_result(
    const struct appliance_route_config *config);
/* How many records were produced, drained or not. */
uint32_t appliance_route_config_record_count(
    const struct appliance_route_config *config);
const char *appliance_route_config_record_result_string(
    enum appliance_route_config_record_result result);
const char *appliance_route_config_status_string(
    enum appliance_route_config_status status);

#endif
