#include "appliance_audit_export.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-377 export cursor.  Every operation is fail-closed: a cursor that does
 * not belong to the chain generation, a cursor that asks for a record the
 * retention window has already retired, and an acknowledgement that is not the
 * exact contiguous successor of the last exported sequence are all refused or
 * declared instead of being silently resolved.
 */

static void appliance_audit_export_zero(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool appliance_audit_export_chain_valid(
    const struct appliance_audit_chain *chain)
{
    return chain != NULL &&
           appliance_audit_chain_generation(chain) != 0U &&
           appliance_audit_chain_next_sequence(chain) != 0U;
}

static bool appliance_audit_export_cursor_valid(
    const struct appliance_audit_export_cursor *cursor)
{
    return cursor != NULL &&
           cursor->version == APPLIANCE_AUDIT_EXPORT_VERSION;
}

static void appliance_audit_export_refresh(
    const struct appliance_audit_chain *chain,
    struct appliance_audit_export_cursor *cursor)
{
    cursor->generation = appliance_audit_chain_generation(chain);
    cursor->first_sequence = appliance_audit_chain_first_sequence(chain);
    cursor->end_sequence = appliance_audit_chain_next_sequence(chain);
}

static enum appliance_audit_export_state appliance_audit_export_state_of(
    const struct appliance_audit_export_cursor *cursor)
{
    if (cursor->gap_declared) {
        return APPLIANCE_AUDIT_EXPORT_STATE_EXPIRED;
    }
    if (!cursor->active) {
        return APPLIANCE_AUDIT_EXPORT_STATE_IDLE;
    }
    if (cursor->next_sequence >= cursor->end_sequence) {
        return APPLIANCE_AUDIT_EXPORT_STATE_COMPLETE;
    }
    return APPLIANCE_AUDIT_EXPORT_STATE_ACTIVE;
}

enum appliance_audit_export_status appliance_audit_export_cursor_init(
    struct appliance_audit_export_cursor *cursor)
{
    if (cursor == NULL) {
        return APPLIANCE_AUDIT_EXPORT_NULL;
    }
    memset(cursor, 0, sizeof(*cursor));
    cursor->version = APPLIANCE_AUDIT_EXPORT_VERSION;
    return APPLIANCE_AUDIT_EXPORT_OK;
}

enum appliance_audit_export_status appliance_audit_export_begin(
    const struct appliance_audit_chain *chain,
    struct appliance_audit_export_cursor *cursor,
    uint64_t requested_sequence)
{
    struct appliance_audit_chain_verdict verdict;
    uint64_t first_sequence;
    uint64_t end_sequence;
    bool expired = false;

    if (chain == NULL || cursor == NULL) {
        return APPLIANCE_AUDIT_EXPORT_NULL;
    }
    if (!appliance_audit_export_chain_valid(chain)) {
        return APPLIANCE_AUDIT_EXPORT_BAD_STATE;
    }
    /*
     * A cursor is never opened on a window that does not verify: the export
     * contract is that only verified records leave the appliance.
     */
    if (appliance_audit_chain_verify(chain, &verdict) !=
            APPLIANCE_AUDIT_CHAIN_OK ||
        !verdict.chain_ok) {
        return APPLIANCE_AUDIT_EXPORT_BAD_STATE;
    }
    if (!appliance_audit_export_cursor_valid(cursor)) {
        return APPLIANCE_AUDIT_EXPORT_BAD_VERSION;
    }
    if (cursor->active) {
        return APPLIANCE_AUDIT_EXPORT_ALREADY_ACTIVE;
    }

    first_sequence = appliance_audit_chain_first_sequence(chain);
    end_sequence = appliance_audit_chain_next_sequence(chain);
    if (requested_sequence == 0U) {
        requested_sequence = first_sequence;
    } else if (requested_sequence < first_sequence) {
        expired = true;
        requested_sequence = first_sequence;
    } else if (requested_sequence > end_sequence) {
        return APPLIANCE_AUDIT_EXPORT_OUT_OF_RANGE;
    }

    cursor->active = true;
    cursor->gap_declared = false;
    cursor->generation = appliance_audit_chain_generation(chain);
    cursor->first_sequence = first_sequence;
    cursor->end_sequence = end_sequence;
    cursor->next_sequence = requested_sequence;
    cursor->last_exported_sequence = 0U;
    cursor->last_ack_sequence = 0U;
    cursor->exported_count = 0U;
    cursor->ack_count = 0U;
    if (expired) {
        cursor->gap_declared = true;
        ++cursor->gap_count;
        return APPLIANCE_AUDIT_EXPORT_EXPIRED;
    }
    return APPLIANCE_AUDIT_EXPORT_OK;
}

enum appliance_audit_export_status appliance_audit_export_next(
    const struct appliance_audit_chain *chain,
    struct appliance_audit_export_cursor *cursor,
    struct audit_record *record_out,
    uint8_t digest_out[SHA256_DIGEST_SIZE],
    uint64_t *sequence_out)
{
    struct audit_record record;
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint64_t sequence;

    if (chain == NULL || cursor == NULL || record_out == NULL ||
        digest_out == NULL) {
        return APPLIANCE_AUDIT_EXPORT_NULL;
    }
    if (!appliance_audit_export_chain_valid(chain)) {
        return APPLIANCE_AUDIT_EXPORT_BAD_STATE;
    }
    if (!appliance_audit_export_cursor_valid(cursor)) {
        return APPLIANCE_AUDIT_EXPORT_BAD_VERSION;
    }
    if (!cursor->active) {
        return APPLIANCE_AUDIT_EXPORT_NOT_ACTIVE;
    }
    if (cursor->generation !=
        appliance_audit_chain_generation(chain)) {
        return APPLIANCE_AUDIT_EXPORT_STALE_GENERATION;
    }

    appliance_audit_export_refresh(chain, cursor);
    if (cursor->next_sequence <
        appliance_audit_chain_first_sequence(chain)) {
        cursor->next_sequence =
            appliance_audit_chain_first_sequence(chain);
        cursor->gap_declared = true;
        ++cursor->gap_count;
        return APPLIANCE_AUDIT_EXPORT_EXPIRED;
    }
    if (cursor->next_sequence >=
        appliance_audit_chain_next_sequence(chain)) {
        return APPLIANCE_AUDIT_EXPORT_EMPTY;
    }

    sequence = cursor->next_sequence;
    if (!appliance_audit_chain_find(
            chain, sequence, &record, digest)) {
        return APPLIANCE_AUDIT_EXPORT_NOT_FOUND;
    }
    *record_out = record;
    memcpy(digest_out, digest, SHA256_DIGEST_SIZE);
    if (sequence_out != NULL) {
        *sequence_out = sequence;
    }
    cursor->last_exported_sequence = sequence;
    ++cursor->next_sequence;
    ++cursor->exported_count;
    return APPLIANCE_AUDIT_EXPORT_OK;
}

enum appliance_audit_export_status appliance_audit_export_batch(
    const struct appliance_audit_chain *chain,
    struct appliance_audit_export_cursor *cursor,
    struct audit_record *records_out,
    uint8_t *digests_out,
    uint32_t capacity,
    uint32_t *count_out)
{
    uint32_t index;
    enum appliance_audit_export_status status =
        APPLIANCE_AUDIT_EXPORT_EMPTY;

    if (chain == NULL || cursor == NULL || records_out == NULL ||
        digests_out == NULL || count_out == NULL) {
        return APPLIANCE_AUDIT_EXPORT_NULL;
    }
    *count_out = 0U;
    if (capacity == 0U) {
        return APPLIANCE_AUDIT_EXPORT_BAD_ARGUMENT;
    }
    for (index = 0U; index < capacity; ++index) {
        status = appliance_audit_export_next(
            chain, cursor,
            &records_out[index],
            &digests_out[index * SHA256_DIGEST_SIZE],
            NULL);
        if (status != APPLIANCE_AUDIT_EXPORT_OK) {
            break;
        }
        ++(*count_out);
    }
    if (*count_out != 0U) {
        return APPLIANCE_AUDIT_EXPORT_OK;
    }
    return status;
}

enum appliance_audit_export_status appliance_audit_export_ack(
    struct appliance_audit_export_cursor *cursor,
    uint64_t sequence)
{
    if (cursor == NULL) {
        return APPLIANCE_AUDIT_EXPORT_NULL;
    }
    if (!appliance_audit_export_cursor_valid(cursor)) {
        return APPLIANCE_AUDIT_EXPORT_BAD_VERSION;
    }
    if (!cursor->active) {
        return APPLIANCE_AUDIT_EXPORT_NOT_ACTIVE;
    }
    if (sequence == 0U) {
        return APPLIANCE_AUDIT_EXPORT_BAD_ARGUMENT;
    }
    /*
     * Only a sequence that was actually exported can be acknowledged, and it
     * must advance the acknowledged prefix, so a batch can be acknowledged in
     * order while an unexported, out-of-order or duplicated acknowledgement is
     * refused.
     */
    if (sequence > cursor->last_exported_sequence) {
        return APPLIANCE_AUDIT_EXPORT_OUT_OF_ORDER;
    }
    if (cursor->ack_count != 0U &&
        sequence <= cursor->last_ack_sequence) {
        return APPLIANCE_AUDIT_EXPORT_OUT_OF_ORDER;
    }
    cursor->last_ack_sequence = sequence;
    ++cursor->ack_count;
    return APPLIANCE_AUDIT_EXPORT_OK;
}

enum appliance_audit_export_status appliance_audit_export_snapshot(
    const struct appliance_audit_export_cursor *cursor,
    struct appliance_audit_export_snapshot *snapshot_out)
{
    if (cursor == NULL || snapshot_out == NULL) {
        return APPLIANCE_AUDIT_EXPORT_NULL;
    }
    if (!appliance_audit_export_cursor_valid(cursor)) {
        return APPLIANCE_AUDIT_EXPORT_BAD_VERSION;
    }

    memset(snapshot_out, 0, sizeof(*snapshot_out));
    snapshot_out->version = APPLIANCE_AUDIT_EXPORT_VERSION;
    snapshot_out->published = true;
    snapshot_out->state = appliance_audit_export_state_of(cursor);
    snapshot_out->generation = cursor->generation;
    snapshot_out->first_sequence = cursor->first_sequence;
    snapshot_out->next_sequence = cursor->next_sequence;
    snapshot_out->end_sequence = cursor->end_sequence;
    snapshot_out->last_exported_sequence =
        cursor->last_exported_sequence;
    snapshot_out->last_ack_sequence = cursor->last_ack_sequence;
    snapshot_out->exported_count = cursor->exported_count;
    snapshot_out->ack_count = cursor->ack_count;
    snapshot_out->gap_count = cursor->gap_count;
    return APPLIANCE_AUDIT_EXPORT_OK;
}

static bool appliance_audit_export_append_text(
    char *output,
    size_t capacity,
    size_t *offset,
    const char *text)
{
    size_t index = 0U;

    if (output == NULL || offset == NULL || text == NULL) {
        return false;
    }
    while (text[index] != '\0') {
        if (*offset + 1U > capacity) {
            return false;
        }
        if (*offset < capacity) {
            output[*offset] = text[index];
        }
        ++(*offset);
        ++index;
    }
    return true;
}

static bool appliance_audit_export_append_u64(
    char *output,
    size_t capacity,
    size_t *offset,
    uint64_t value)
{
    char digits[21];
    uint32_t count = 0U;

    if (value == 0U) {
        return appliance_audit_export_append_text(
            output, capacity, offset, "0");
    }
    while (value != 0U && count < 20U) {
        digits[count] = (char)('0' + (value % 10U));
        value /= 10U;
        ++count;
    }
    while (count != 0U) {
        char single[2];

        --count;
        single[0] = digits[count];
        single[1] = '\0';
        if (!appliance_audit_export_append_text(
                output, capacity, offset, single)) {
            return false;
        }
    }
    return true;
}

enum appliance_audit_export_status appliance_audit_export_render(
    const struct appliance_audit_export_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    const char *state;
    size_t offset = 0U;

    if (snapshot == NULL || output == NULL || output_length == NULL) {
        return APPLIANCE_AUDIT_EXPORT_NULL;
    }
    if (!snapshot->published ||
        snapshot->version != APPLIANCE_AUDIT_EXPORT_VERSION) {
        return APPLIANCE_AUDIT_EXPORT_BAD_STATE;
    }
    state = appliance_audit_export_state_string(snapshot->state);
    if (state == NULL) {
        return APPLIANCE_AUDIT_EXPORT_BAD_STATE;
    }
    appliance_audit_export_zero(output, capacity);
    if (!appliance_audit_export_append_text(
            output, capacity, &offset, "audit_export v=1 state=") ||
        !appliance_audit_export_append_text(
            output, capacity, &offset, state) ||
        !appliance_audit_export_append_text(
            output, capacity, &offset, " generation=") ||
        !appliance_audit_export_append_u64(
            output, capacity, &offset, snapshot->generation) ||
        !appliance_audit_export_append_text(
            output, capacity, &offset, " first=") ||
        !appliance_audit_export_append_u64(
            output, capacity, &offset, snapshot->first_sequence) ||
        !appliance_audit_export_append_text(
            output, capacity, &offset, " next=") ||
        !appliance_audit_export_append_u64(
            output, capacity, &offset, snapshot->next_sequence) ||
        !appliance_audit_export_append_text(
            output, capacity, &offset, " end=") ||
        !appliance_audit_export_append_u64(
            output, capacity, &offset, snapshot->end_sequence) ||
        !appliance_audit_export_append_text(
            output, capacity, &offset, " ack=") ||
        !appliance_audit_export_append_u64(
            output, capacity, &offset, snapshot->last_ack_sequence) ||
        !appliance_audit_export_append_text(
            output, capacity, &offset, " exported=") ||
        !appliance_audit_export_append_u64(
            output, capacity, &offset, snapshot->exported_count) ||
        !appliance_audit_export_append_text(
            output, capacity, &offset, " acked=") ||
        !appliance_audit_export_append_u64(
            output, capacity, &offset, snapshot->ack_count) ||
        !appliance_audit_export_append_text(
            output, capacity, &offset, " gap=") ||
        !appliance_audit_export_append_u64(
            output, capacity, &offset, snapshot->gap_count)) {
        return APPLIANCE_AUDIT_EXPORT_BUFFER_TOO_SMALL;
    }
    if (offset + 1U > capacity) {
        return APPLIANCE_AUDIT_EXPORT_BUFFER_TOO_SMALL;
    }
    output[offset] = '\0';
    *output_length = offset;
    return APPLIANCE_AUDIT_EXPORT_OK;
}

bool appliance_audit_export_cursor_active(
    const struct appliance_audit_export_cursor *cursor)
{
    return appliance_audit_export_cursor_valid(cursor) &&
           cursor->active;
}

uint64_t appliance_audit_export_next_sequence(
    const struct appliance_audit_export_cursor *cursor)
{
    return appliance_audit_export_cursor_valid(cursor)
        ? cursor->next_sequence
        : 0U;
}

uint64_t appliance_audit_export_last_ack_sequence(
    const struct appliance_audit_export_cursor *cursor)
{
    return appliance_audit_export_cursor_valid(cursor)
        ? cursor->last_ack_sequence
        : 0U;
}

uint64_t appliance_audit_export_gap_count(
    const struct appliance_audit_export_cursor *cursor)
{
    return appliance_audit_export_cursor_valid(cursor)
        ? cursor->gap_count
        : 0U;
}

const char *appliance_audit_export_status_string(
    enum appliance_audit_export_status status)
{
    switch (status) {
    case APPLIANCE_AUDIT_EXPORT_OK:
        return "APPLIANCE_AUDIT_EXPORT_OK";
    case APPLIANCE_AUDIT_EXPORT_NULL:
        return "APPLIANCE_AUDIT_EXPORT_NULL";
    case APPLIANCE_AUDIT_EXPORT_BAD_VERSION:
        return "APPLIANCE_AUDIT_EXPORT_BAD_VERSION";
    case APPLIANCE_AUDIT_EXPORT_BAD_STATE:
        return "APPLIANCE_AUDIT_EXPORT_BAD_STATE";
    case APPLIANCE_AUDIT_EXPORT_EMPTY:
        return "APPLIANCE_AUDIT_EXPORT_EMPTY";
    case APPLIANCE_AUDIT_EXPORT_EXPIRED:
        return "APPLIANCE_AUDIT_EXPORT_EXPIRED";
    case APPLIANCE_AUDIT_EXPORT_OUT_OF_RANGE:
        return "APPLIANCE_AUDIT_EXPORT_OUT_OF_RANGE";
    case APPLIANCE_AUDIT_EXPORT_OUT_OF_ORDER:
        return "APPLIANCE_AUDIT_EXPORT_OUT_OF_ORDER";
    case APPLIANCE_AUDIT_EXPORT_NOT_ACTIVE:
        return "APPLIANCE_AUDIT_EXPORT_NOT_ACTIVE";
    case APPLIANCE_AUDIT_EXPORT_ALREADY_ACTIVE:
        return "APPLIANCE_AUDIT_EXPORT_ALREADY_ACTIVE";
    case APPLIANCE_AUDIT_EXPORT_BUFFER_TOO_SMALL:
        return "APPLIANCE_AUDIT_EXPORT_BUFFER_TOO_SMALL";
    case APPLIANCE_AUDIT_EXPORT_STALE_GENERATION:
        return "APPLIANCE_AUDIT_EXPORT_STALE_GENERATION";
    case APPLIANCE_AUDIT_EXPORT_BAD_ARGUMENT:
        return "APPLIANCE_AUDIT_EXPORT_BAD_ARGUMENT";
    case APPLIANCE_AUDIT_EXPORT_NOT_FOUND:
        return "APPLIANCE_AUDIT_EXPORT_NOT_FOUND";
    default:
        return NULL;
    }
}

const char *appliance_audit_export_state_string(
    enum appliance_audit_export_state state)
{
    switch (state) {
    case APPLIANCE_AUDIT_EXPORT_STATE_NONE:
        return "NONE";
    case APPLIANCE_AUDIT_EXPORT_STATE_IDLE:
        return "IDLE";
    case APPLIANCE_AUDIT_EXPORT_STATE_ACTIVE:
        return "ACTIVE";
    case APPLIANCE_AUDIT_EXPORT_STATE_COMPLETE:
        return "COMPLETE";
    case APPLIANCE_AUDIT_EXPORT_STATE_EXPIRED:
        return "EXPIRED";
    default:
        return NULL;
    }
}
