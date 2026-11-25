#include "appliance_update_lifecycle.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-376 boot-side update lifecycle.  See the header for the contract.
 */

static void appliance_update_lifecycle_zero(void *destination,
                                            size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool appliance_update_lifecycle_state_valid(
    enum appliance_update_lifecycle_state state)
{
    return state >= APPLIANCE_UPDATE_LIFECYCLE_STATE_NONE &&
           state <= APPLIANCE_UPDATE_LIFECYCLE_STATE_FAILED;
}

static bool appliance_update_lifecycle_io_valid(
    const struct appliance_update_lifecycle_io *io)
{
    return io != NULL && io->record.read != NULL &&
           io->record.write != NULL && io->record.flush != NULL &&
           io->slot_intact != NULL;
}

static enum appliance_update_lifecycle_status
appliance_update_lifecycle_reject(
    struct appliance_update_lifecycle *lifecycle,
    enum appliance_update_lifecycle_status status)
{
    if (lifecycle != NULL) {
        lifecycle->reject_count += 1U;
        lifecycle->last_status = status;
    }
    return status;
}

static enum appliance_update_lifecycle_status
appliance_update_lifecycle_map_record(
    enum appliance_update_record_status status)
{
    switch (status) {
    case APPLIANCE_UPDATE_RECORD_OK:
        return APPLIANCE_UPDATE_LIFECYCLE_OK;
    case APPLIANCE_UPDATE_RECORD_NULL:
        return APPLIANCE_UPDATE_LIFECYCLE_NULL;
    case APPLIANCE_UPDATE_RECORD_BAD_VERSION:
        return APPLIANCE_UPDATE_LIFECYCLE_BAD_VERSION;
    case APPLIANCE_UPDATE_RECORD_NO_PENDING:
        return APPLIANCE_UPDATE_LIFECYCLE_NO_PENDING;
    case APPLIANCE_UPDATE_RECORD_DOWNGRADE_REFUSED:
        return APPLIANCE_UPDATE_LIFECYCLE_DOWNGRADE_REFUSED;
    case APPLIANCE_UPDATE_RECORD_SOURCE_MISMATCH:
        return APPLIANCE_UPDATE_LIFECYCLE_SOURCE_MISMATCH;
    default:
        return APPLIANCE_UPDATE_LIFECYCLE_RECORD_FAILED;
    }
}

/*
 * Roll back to the known-good slot.  A settle that cannot be made durable is
 * reported as a record failure and the in-memory state is not advanced.
 */
static enum appliance_update_lifecycle_status
appliance_update_lifecycle_roll_back(
    struct appliance_update_lifecycle *lifecycle,
    const struct appliance_update_lifecycle_io *io,
    bool slot_verified)
{
    enum appliance_update_record_status record_status =
        appliance_update_record_roll_back(&lifecycle->record,
                                          &io->record);

    if (record_status != APPLIANCE_UPDATE_RECORD_OK) {
        lifecycle->state = APPLIANCE_UPDATE_LIFECYCLE_STATE_FAILED;
        return appliance_update_lifecycle_reject(
            lifecycle,
            appliance_update_lifecycle_map_record(record_status));
    }
    lifecycle->active = lifecycle->record.known_good;
    lifecycle->rollback_selected = true;
    lifecycle->slot_verified = slot_verified;
    lifecycle->state = APPLIANCE_UPDATE_LIFECYCLE_STATE_ROLLED_BACK;
    lifecycle->rollback_count += 1U;
    lifecycle->last_status = APPLIANCE_UPDATE_LIFECYCLE_OK;
    return APPLIANCE_UPDATE_LIFECYCLE_OK;
}

enum appliance_update_lifecycle_status appliance_update_lifecycle_init(
    struct appliance_update_lifecycle *lifecycle,
    uint32_t installed_image_id,
    const struct appliance_update_record_entry *known_good,
    const struct appliance_update_lifecycle_io *io)
{
    enum appliance_update_record_status record_status;

    if (lifecycle == NULL || known_good == NULL) {
        return APPLIANCE_UPDATE_LIFECYCLE_NULL;
    }
    if (!appliance_update_lifecycle_io_valid(io)) {
        return APPLIANCE_UPDATE_LIFECYCLE_NULL;
    }

    appliance_update_lifecycle_zero(lifecycle, sizeof(*lifecycle));
    lifecycle->version = APPLIANCE_UPDATE_LIFECYCLE_VERSION;

    record_status = appliance_update_record_load(
        &lifecycle->record, &io->record);
    if (record_status == APPLIANCE_UPDATE_RECORD_NO_RECORD) {
        record_status = appliance_update_record_init(
            &lifecycle->record, installed_image_id, known_good,
            &io->record);
    }
    if (record_status != APPLIANCE_UPDATE_RECORD_OK) {
        lifecycle->initialized = true;
        lifecycle->state = APPLIANCE_UPDATE_LIFECYCLE_STATE_FAILED;
        return appliance_update_lifecycle_reject(
            lifecycle,
            appliance_update_lifecycle_map_record(record_status));
    }

    lifecycle->initialized = true;
    lifecycle->active = lifecycle->record.known_good;
    lifecycle->state = APPLIANCE_UPDATE_LIFECYCLE_STATE_IDLE;
    lifecycle->last_status = APPLIANCE_UPDATE_LIFECYCLE_OK;
    return APPLIANCE_UPDATE_LIFECYCLE_OK;
}

enum appliance_update_lifecycle_status appliance_update_lifecycle_resolve(
    struct appliance_update_lifecycle *lifecycle,
    const struct appliance_update_lifecycle_io *io)
{
    enum appliance_update_record_status record_status;
    const struct appliance_update_record_entry *rollback_source;

    if (lifecycle == NULL) {
        return APPLIANCE_UPDATE_LIFECYCLE_NULL;
    }
    if (!appliance_update_lifecycle_io_valid(io)) {
        return APPLIANCE_UPDATE_LIFECYCLE_NULL;
    }
    if (!lifecycle->initialized ||
        lifecycle->version != APPLIANCE_UPDATE_LIFECYCLE_VERSION ||
        !appliance_update_lifecycle_state_valid(lifecycle->state) ||
        lifecycle->state == APPLIANCE_UPDATE_LIFECYCLE_STATE_NONE) {
        return APPLIANCE_UPDATE_LIFECYCLE_BAD_STATE;
    }
    lifecycle->resolve_count += 1U;

    if (!appliance_update_record_pending(&lifecycle->record)) {
        lifecycle->active = lifecycle->record.known_good;
        lifecycle->rollback_selected = false;
        lifecycle->slot_verified = false;
        lifecycle->last_status = APPLIANCE_UPDATE_LIFECYCLE_OK;
        return APPLIANCE_UPDATE_LIFECYCLE_OK;
    }

    rollback_source =
        appliance_update_record_rollback_source(&lifecycle->record);
    if (rollback_source == NULL) {
        lifecycle->state = APPLIANCE_UPDATE_LIFECYCLE_STATE_FAILED;
        return appliance_update_lifecycle_reject(
            lifecycle, APPLIANCE_UPDATE_LIFECYCLE_NO_ROLLBACK_SOURCE);
    }

    /*
     * A pending target is handed over only when its bytes are still intact
     * and its attempt budget is not exhausted.  Both refusals fall back to
     * the known-good slot rather than booting a candidate that cannot
     * confirm itself.
     */
    if (!io->slot_intact(io->context, &lifecycle->record.pending)) {
        return appliance_update_lifecycle_roll_back(lifecycle, io, false);
    }
    if (appliance_update_record_attempts_exhausted(&lifecycle->record)) {
        return appliance_update_lifecycle_roll_back(lifecycle, io, true);
    }

    record_status = appliance_update_record_note_attempt(
        &lifecycle->record, &io->record);
    if (record_status != APPLIANCE_UPDATE_RECORD_OK) {
        lifecycle->state = APPLIANCE_UPDATE_LIFECYCLE_STATE_FAILED;
        return appliance_update_lifecycle_reject(
            lifecycle,
            appliance_update_lifecycle_map_record(record_status));
    }
    lifecycle->active = lifecycle->record.pending;
    lifecycle->rollback_selected = false;
    lifecycle->slot_verified = true;
    lifecycle->state = APPLIANCE_UPDATE_LIFECYCLE_STATE_PENDING;
    lifecycle->last_status = APPLIANCE_UPDATE_LIFECYCLE_OK;
    return APPLIANCE_UPDATE_LIFECYCLE_OK;
}

enum appliance_update_lifecycle_status appliance_update_lifecycle_accept(
    struct appliance_update_lifecycle *lifecycle,
    uint32_t accepted_image_id,
    const struct appliance_update_lifecycle_io *io)
{
    enum appliance_update_record_status record_status;

    if (lifecycle == NULL) {
        return APPLIANCE_UPDATE_LIFECYCLE_NULL;
    }
    if (!appliance_update_lifecycle_io_valid(io)) {
        return APPLIANCE_UPDATE_LIFECYCLE_NULL;
    }
    if (!lifecycle->initialized ||
        lifecycle->version != APPLIANCE_UPDATE_LIFECYCLE_VERSION) {
        return APPLIANCE_UPDATE_LIFECYCLE_BAD_STATE;
    }
    if (!appliance_update_record_pending(&lifecycle->record)) {
        return appliance_update_lifecycle_reject(
            lifecycle, APPLIANCE_UPDATE_LIFECYCLE_NO_PENDING);
    }

    record_status = appliance_update_record_accept(
        &lifecycle->record, accepted_image_id, &io->record);
    if (record_status != APPLIANCE_UPDATE_RECORD_OK) {
        lifecycle->state = APPLIANCE_UPDATE_LIFECYCLE_STATE_FAILED;
        return appliance_update_lifecycle_reject(
            lifecycle,
            appliance_update_lifecycle_map_record(record_status));
    }
    lifecycle->active = lifecycle->record.known_good;
    lifecycle->rollback_selected = false;
    lifecycle->slot_verified = true;
    lifecycle->state = APPLIANCE_UPDATE_LIFECYCLE_STATE_ACCEPTED;
    lifecycle->accept_count += 1U;
    lifecycle->last_status = APPLIANCE_UPDATE_LIFECYCLE_OK;
    return APPLIANCE_UPDATE_LIFECYCLE_OK;
}

enum appliance_update_lifecycle_status appliance_update_lifecycle_fail(
    struct appliance_update_lifecycle *lifecycle,
    const struct appliance_update_lifecycle_io *io)
{
    if (lifecycle == NULL) {
        return APPLIANCE_UPDATE_LIFECYCLE_NULL;
    }
    if (!appliance_update_lifecycle_io_valid(io)) {
        return APPLIANCE_UPDATE_LIFECYCLE_NULL;
    }
    if (!lifecycle->initialized ||
        lifecycle->version != APPLIANCE_UPDATE_LIFECYCLE_VERSION) {
        return APPLIANCE_UPDATE_LIFECYCLE_BAD_STATE;
    }
    if (!appliance_update_record_pending(&lifecycle->record)) {
        return appliance_update_lifecycle_reject(
            lifecycle, APPLIANCE_UPDATE_LIFECYCLE_NO_PENDING);
    }
    return appliance_update_lifecycle_roll_back(lifecycle, io, false);
}

enum appliance_update_lifecycle_status appliance_update_lifecycle_admit(
    const struct appliance_update_lifecycle *lifecycle,
    uint32_t target_image_id,
    uint32_t source_image_id)
{
    if (lifecycle == NULL) {
        return APPLIANCE_UPDATE_LIFECYCLE_NULL;
    }
    if (!lifecycle->initialized ||
        lifecycle->version != APPLIANCE_UPDATE_LIFECYCLE_VERSION) {
        return APPLIANCE_UPDATE_LIFECYCLE_BAD_STATE;
    }
    switch (appliance_update_record_check_upgrade(
        &lifecycle->record, target_image_id, source_image_id)) {
    case APPLIANCE_UPDATE_RECORD_OK:
        return APPLIANCE_UPDATE_LIFECYCLE_OK;
    case APPLIANCE_UPDATE_RECORD_DOWNGRADE_REFUSED:
        return APPLIANCE_UPDATE_LIFECYCLE_DOWNGRADE_REFUSED;
    case APPLIANCE_UPDATE_RECORD_SOURCE_MISMATCH:
        return APPLIANCE_UPDATE_LIFECYCLE_SOURCE_MISMATCH;
    case APPLIANCE_UPDATE_RECORD_NULL:
        return APPLIANCE_UPDATE_LIFECYCLE_NULL;
    default:
        return APPLIANCE_UPDATE_LIFECYCLE_BAD_STATE;
    }
}

const struct appliance_update_record_entry *
appliance_update_lifecycle_active(
    const struct appliance_update_lifecycle *lifecycle)
{
    if (lifecycle == NULL || !lifecycle->initialized) {
        return NULL;
    }
    return &lifecycle->active;
}

enum appliance_update_lifecycle_status appliance_update_lifecycle_snapshot(
    const struct appliance_update_lifecycle *lifecycle,
    struct appliance_update_lifecycle_snapshot *snapshot_out)
{
    if (lifecycle == NULL || snapshot_out == NULL) {
        return APPLIANCE_UPDATE_LIFECYCLE_NULL;
    }
    if (!lifecycle->initialized ||
        lifecycle->version != APPLIANCE_UPDATE_LIFECYCLE_VERSION ||
        !appliance_update_lifecycle_state_valid(lifecycle->state) ||
        lifecycle->state == APPLIANCE_UPDATE_LIFECYCLE_STATE_NONE) {
        return APPLIANCE_UPDATE_LIFECYCLE_BAD_STATE;
    }

    memset(snapshot_out, 0, sizeof(*snapshot_out));
    snapshot_out->published = true;
    snapshot_out->version = APPLIANCE_UPDATE_LIFECYCLE_VERSION;
    snapshot_out->state = lifecycle->state;
    snapshot_out->last_status = lifecycle->last_status;
    snapshot_out->outcome = lifecycle->record.outcome;
    snapshot_out->installed_image_id =
        lifecycle->record.installed_image_id;
    snapshot_out->attempts = lifecycle->record.attempts;
    snapshot_out->active_slot_id = lifecycle->active.slot_id;
    snapshot_out->pending =
        appliance_update_record_pending(&lifecycle->record);
    snapshot_out->rollback_selected = lifecycle->rollback_selected;
    snapshot_out->resolve_count = lifecycle->resolve_count;
    snapshot_out->accept_count = lifecycle->accept_count;
    snapshot_out->rollback_count = lifecycle->rollback_count;
    snapshot_out->reject_count = lifecycle->reject_count;
    return APPLIANCE_UPDATE_LIFECYCLE_OK;
}

static bool appliance_update_lifecycle_append_text(
    char *rendered,
    size_t *offset,
    const char *text)
{
    size_t index;

    for (index = 0U; text[index] != '\0'; ++index) {
        if (*offset >= APPLIANCE_UPDATE_LIFECYCLE_RENDER_MAX) {
            return false;
        }
        rendered[*offset] = text[index];
        *offset += 1U;
    }
    return true;
}

static bool appliance_update_lifecycle_append_u32(
    char *rendered,
    size_t *offset,
    uint32_t value)
{
    char digits[10];
    size_t count = 0U;

    do {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U);
    while (count != 0U) {
        if (*offset >= APPLIANCE_UPDATE_LIFECYCLE_RENDER_MAX) {
            return false;
        }
        rendered[*offset] = digits[--count];
        *offset += 1U;
    }
    return true;
}

enum appliance_update_lifecycle_status appliance_update_lifecycle_render(
    const struct appliance_update_lifecycle_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    char rendered[APPLIANCE_UPDATE_LIFECYCLE_RENDER_MAX];
    const char *outcome;
    size_t offset = 0U;
    size_t index;

    if (snapshot == NULL || output == NULL || output_length == NULL) {
        return APPLIANCE_UPDATE_LIFECYCLE_NULL;
    }
    if (!snapshot->published ||
        snapshot->version != APPLIANCE_UPDATE_LIFECYCLE_VERSION ||
        !appliance_update_lifecycle_state_valid(snapshot->state) ||
        snapshot->state == APPLIANCE_UPDATE_LIFECYCLE_STATE_NONE) {
        return APPLIANCE_UPDATE_LIFECYCLE_BAD_STATE;
    }
    outcome = appliance_update_record_outcome_string(snapshot->outcome);
    if (outcome == NULL) {
        return APPLIANCE_UPDATE_LIFECYCLE_BAD_STATE;
    }

    if (!appliance_update_lifecycle_append_text(
            rendered, &offset, "update_lifecycle v=") ||
        !appliance_update_lifecycle_append_u32(
            rendered, &offset, snapshot->version) ||
        !appliance_update_lifecycle_append_text(
            rendered, &offset, " state=") ||
        !appliance_update_lifecycle_append_text(
            rendered, &offset,
            appliance_update_lifecycle_state_string(snapshot->state)) ||
        !appliance_update_lifecycle_append_text(
            rendered, &offset, " outcome=") ||
        !appliance_update_lifecycle_append_text(
            rendered, &offset, outcome) ||
        !appliance_update_lifecycle_append_text(
            rendered, &offset, " image=") ||
        !appliance_update_lifecycle_append_u32(
            rendered, &offset, snapshot->installed_image_id) ||
        !appliance_update_lifecycle_append_text(
            rendered, &offset, " slot=") ||
        !appliance_update_lifecycle_append_u32(
            rendered, &offset, snapshot->active_slot_id) ||
        !appliance_update_lifecycle_append_text(
            rendered, &offset, " attempts=") ||
        !appliance_update_lifecycle_append_u32(
            rendered, &offset, snapshot->attempts) ||
        !appliance_update_lifecycle_append_text(
            rendered, &offset, " pending=") ||
        !appliance_update_lifecycle_append_text(
            rendered, &offset, snapshot->pending ? "YES" : "NO") ||
        !appliance_update_lifecycle_append_text(
            rendered, &offset, " rollback=") ||
        !appliance_update_lifecycle_append_text(
            rendered,
            &offset,
            snapshot->rollback_selected ? "SELECTED" : "NONE") ||
        !appliance_update_lifecycle_append_text(
            rendered, &offset, " resolve=") ||
        !appliance_update_lifecycle_append_u32(
            rendered, &offset, snapshot->resolve_count) ||
        !appliance_update_lifecycle_append_text(
            rendered, &offset, " accept=") ||
        !appliance_update_lifecycle_append_u32(
            rendered, &offset, snapshot->accept_count) ||
        !appliance_update_lifecycle_append_text(
            rendered, &offset, " rolled=") ||
        !appliance_update_lifecycle_append_u32(
            rendered, &offset, snapshot->rollback_count) ||
        !appliance_update_lifecycle_append_text(
            rendered, &offset, " reject=") ||
        !appliance_update_lifecycle_append_u32(
            rendered, &offset, snapshot->reject_count)) {
        return APPLIANCE_UPDATE_LIFECYCLE_BUFFER_TOO_SMALL;
    }
    if (capacity <= offset) {
        return APPLIANCE_UPDATE_LIFECYCLE_BUFFER_TOO_SMALL;
    }
    for (index = 0U; index < offset; ++index) {
        output[index] = rendered[index];
    }
    output[offset] = '\0';
    *output_length = offset;
    return APPLIANCE_UPDATE_LIFECYCLE_OK;
}

const char *appliance_update_lifecycle_state_string(
    enum appliance_update_lifecycle_state state)
{
    switch (state) {
    case APPLIANCE_UPDATE_LIFECYCLE_STATE_NONE:
        return "NONE";
    case APPLIANCE_UPDATE_LIFECYCLE_STATE_IDLE:
        return "IDLE";
    case APPLIANCE_UPDATE_LIFECYCLE_STATE_PENDING:
        return "PENDING";
    case APPLIANCE_UPDATE_LIFECYCLE_STATE_ACCEPTED:
        return "ACCEPTED";
    case APPLIANCE_UPDATE_LIFECYCLE_STATE_ROLLED_BACK:
        return "ROLLED_BACK";
    case APPLIANCE_UPDATE_LIFECYCLE_STATE_FAILED:
        return "FAILED";
    default:
        return NULL;
    }
}

const char *appliance_update_lifecycle_status_string(
    enum appliance_update_lifecycle_status status)
{
    switch (status) {
    case APPLIANCE_UPDATE_LIFECYCLE_OK:
        return "OK";
    case APPLIANCE_UPDATE_LIFECYCLE_NULL:
        return "NULL";
    case APPLIANCE_UPDATE_LIFECYCLE_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_UPDATE_LIFECYCLE_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_UPDATE_LIFECYCLE_RECORD_FAILED:
        return "RECORD_FAILED";
    case APPLIANCE_UPDATE_LIFECYCLE_NO_PENDING:
        return "NO_PENDING";
    case APPLIANCE_UPDATE_LIFECYCLE_NO_ROLLBACK_SOURCE:
        return "NO_ROLLBACK_SOURCE";
    case APPLIANCE_UPDATE_LIFECYCLE_DOWNGRADE_REFUSED:
        return "DOWNGRADE_REFUSED";
    case APPLIANCE_UPDATE_LIFECYCLE_SOURCE_MISMATCH:
        return "SOURCE_MISMATCH";
    case APPLIANCE_UPDATE_LIFECYCLE_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    default:
        return NULL;
    }
}
