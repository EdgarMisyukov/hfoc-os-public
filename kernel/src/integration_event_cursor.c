#include "integration_event_cursor.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef HFOC_INTEGRATION_EVENT_CURSOR_HOST_TEST
static bool integration_event_cursor_host_snapshot_mutate;

void integration_event_cursor_host_test_set_snapshot_mutate(bool enabled)
{
    integration_event_cursor_host_snapshot_mutate = enabled;
}
#endif

static enum integration_event_cursor_status
integration_event_cursor_validate(
    const struct integration_event_cursor *cursor)
{
    if (cursor->capacity == 0U ||
        cursor->capacity > INTEGRATION_EVENT_CURSOR_MAX_RECORDS) {
        return INTEGRATION_EVENT_CURSOR_BAD_CAPACITY;
    }
    if (cursor->version != INTEGRATION_EVENT_CURSOR_VERSION ||
        cursor->pending_count > cursor->capacity ||
        cursor->next_export_sequence < cursor->pending_count ||
        cursor->last_ack_sequence > cursor->next_export_sequence) {
        return INTEGRATION_EVENT_CURSOR_BAD_STATE;
    }
    return INTEGRATION_EVENT_CURSOR_OK;
}

static bool integration_event_cursor_event_valid(
    const struct kernel_event *event)
{
    return event != NULL && event->type != 0U;
}

enum integration_event_cursor_status integration_event_cursor_init(
    struct integration_event_cursor *cursor,
    uint32_t capacity)
{
    if (cursor == NULL) {
        return INTEGRATION_EVENT_CURSOR_NULL;
    }
    if (capacity == 0U ||
        capacity > INTEGRATION_EVENT_CURSOR_MAX_RECORDS) {
        return INTEGRATION_EVENT_CURSOR_BAD_CAPACITY;
    }

    memset(cursor, 0, sizeof(*cursor));
    cursor->version = INTEGRATION_EVENT_CURSOR_VERSION;
    cursor->capacity = capacity;
    return INTEGRATION_EVENT_CURSOR_OK;
}

enum integration_event_cursor_status integration_event_cursor_export(
    struct integration_event_cursor *cursor,
    const struct kernel_event *event)
{
    struct integration_event_cursor_record *record;
    enum integration_event_cursor_status status;

    if (cursor == NULL || event == NULL) {
        return INTEGRATION_EVENT_CURSOR_NULL;
    }
    status = integration_event_cursor_validate(cursor);
    if (status != INTEGRATION_EVENT_CURSOR_OK) {
        return status;
    }
    if (!integration_event_cursor_event_valid(event)) {
        return INTEGRATION_EVENT_CURSOR_INVALID_EVENT;
    }
    if (cursor->pending_count >= cursor->capacity) {
        return INTEGRATION_EVENT_CURSOR_FULL;
    }
    if (cursor->next_export_sequence == UINT64_MAX ||
        cursor->mutation_generation == UINT64_MAX) {
        return INTEGRATION_EVENT_CURSOR_BAD_STATE;
    }

    record = &cursor->records[cursor->pending_count];
    memset(record, 0, sizeof(*record));
    record->export_sequence = cursor->next_export_sequence;
    record->source_sequence = event->sequence;
    record->generation = cursor->mutation_generation;
    record->event = *event;
    record->acknowledged = false;
    ++cursor->pending_count;
    ++cursor->next_export_sequence;
    ++cursor->mutation_generation;
    return INTEGRATION_EVENT_CURSOR_OK;
}

enum integration_event_cursor_status integration_event_cursor_acknowledge(
    struct integration_event_cursor *cursor,
    uint64_t export_sequence)
{
    enum integration_event_cursor_status status;
    uint32_t index;

    if (cursor == NULL) {
        return INTEGRATION_EVENT_CURSOR_NULL;
    }
    status = integration_event_cursor_validate(cursor);
    if (status != INTEGRATION_EVENT_CURSOR_OK) {
        return status;
    }
    if (cursor->pending_count == 0U) {
        return INTEGRATION_EVENT_CURSOR_EMPTY;
    }
    if (cursor->mutation_generation == UINT64_MAX) {
        return INTEGRATION_EVENT_CURSOR_BAD_STATE;
    }

    if (cursor->records[0].export_sequence != export_sequence) {
        for (index = 1U; index < cursor->pending_count; ++index) {
            if (cursor->records[index].export_sequence ==
                export_sequence) {
                return INTEGRATION_EVENT_CURSOR_OUT_OF_ORDER;
            }
        }
        return INTEGRATION_EVENT_CURSOR_NOT_FOUND;
    }
    if (cursor->records[0].acknowledged) {
        return INTEGRATION_EVENT_CURSOR_BAD_STATE;
    }

    cursor->records[0].acknowledged = true;
    cursor->last_ack_sequence = export_sequence;
    if (cursor->pending_count > 1U) {
        memmove(&cursor->records[0],
                &cursor->records[1],
                sizeof(cursor->records[0]) *
                    (cursor->pending_count - 1U));
    }
    memset(&cursor->records[cursor->pending_count - 1U],
           0,
           sizeof(cursor->records[0]));
    --cursor->pending_count;
    ++cursor->mutation_generation;
    return INTEGRATION_EVENT_CURSOR_OK;
}

enum integration_event_cursor_status integration_event_cursor_snapshot(
    const struct integration_event_cursor *cursor,
    struct integration_event_cursor_record *records_out,
    uint32_t capacity,
    uint32_t *count_out,
    uint64_t *generation_out)
{
    struct integration_event_cursor_record
        draft[INTEGRATION_EVENT_CURSOR_MAX_RECORDS];
    enum integration_event_cursor_status status;
    uint64_t generation;
    uint32_t index;

    if (cursor == NULL || count_out == NULL || generation_out == NULL) {
        return INTEGRATION_EVENT_CURSOR_NULL;
    }
    if ((records_out == NULL) != (capacity == 0U)) {
        return INTEGRATION_EVENT_CURSOR_NULL;
    }
    status = integration_event_cursor_validate(cursor);
    if (status != INTEGRATION_EVENT_CURSOR_OK) {
        return status;
    }
    if (records_out == NULL) {
        *count_out = cursor->pending_count;
        *generation_out = cursor->mutation_generation;
        return INTEGRATION_EVENT_CURSOR_OK;
    }
    if (capacity < cursor->pending_count) {
        return INTEGRATION_EVENT_CURSOR_OVERFLOW;
    }

    generation = cursor->mutation_generation;
    for (index = 0U; index < cursor->pending_count; ++index) {
        draft[index] = cursor->records[index];
    }

#ifdef HFOC_INTEGRATION_EVENT_CURSOR_HOST_TEST
    if (integration_event_cursor_host_snapshot_mutate) {
        struct integration_event_cursor *const mutable_cursor =
            (struct integration_event_cursor *)cursor;
        struct kernel_event mutation_event;

        memset(&mutation_event, 0, sizeof(mutation_event));
        mutation_event.type = 1U;
        mutation_event.flags = 1U;
        (void)integration_event_cursor_export(
            mutable_cursor, &mutation_event);
    }
#endif

    if (cursor->mutation_generation != generation) {
        return INTEGRATION_EVENT_CURSOR_STALE_GENERATION;
    }

    for (index = 0U; index < cursor->pending_count; ++index) {
        records_out[index] = draft[index];
    }
    *count_out = cursor->pending_count;
    *generation_out = generation;
    return INTEGRATION_EVENT_CURSOR_OK;
}

uint32_t integration_event_cursor_pending_count(
    const struct integration_event_cursor *cursor)
{
    return cursor != NULL ? cursor->pending_count : 0U;
}

uint32_t integration_event_cursor_capacity(
    const struct integration_event_cursor *cursor)
{
    return cursor != NULL ? cursor->capacity : 0U;
}

uint64_t integration_event_cursor_next_export_sequence(
    const struct integration_event_cursor *cursor)
{
    return cursor != NULL ? cursor->next_export_sequence : 0U;
}

uint64_t integration_event_cursor_last_ack_sequence(
    const struct integration_event_cursor *cursor)
{
    return cursor != NULL ? cursor->last_ack_sequence : 0U;
}

const char *integration_event_cursor_status_string(
    enum integration_event_cursor_status status)
{
    switch (status) {
    case INTEGRATION_EVENT_CURSOR_OK:
        return "INTEGRATION_EVENT_CURSOR_OK";
    case INTEGRATION_EVENT_CURSOR_NULL:
        return "INTEGRATION_EVENT_CURSOR_NULL";
    case INTEGRATION_EVENT_CURSOR_BAD_CAPACITY:
        return "INTEGRATION_EVENT_CURSOR_BAD_CAPACITY";
    case INTEGRATION_EVENT_CURSOR_BAD_STATE:
        return "INTEGRATION_EVENT_CURSOR_BAD_STATE";
    case INTEGRATION_EVENT_CURSOR_INVALID_EVENT:
        return "INTEGRATION_EVENT_CURSOR_INVALID_EVENT";
    case INTEGRATION_EVENT_CURSOR_FULL:
        return "INTEGRATION_EVENT_CURSOR_FULL";
    case INTEGRATION_EVENT_CURSOR_EMPTY:
        return "INTEGRATION_EVENT_CURSOR_EMPTY";
    case INTEGRATION_EVENT_CURSOR_NOT_FOUND:
        return "INTEGRATION_EVENT_CURSOR_NOT_FOUND";
    case INTEGRATION_EVENT_CURSOR_OUT_OF_ORDER:
        return "INTEGRATION_EVENT_CURSOR_OUT_OF_ORDER";
    case INTEGRATION_EVENT_CURSOR_OVERFLOW:
        return "INTEGRATION_EVENT_CURSOR_OVERFLOW";
    case INTEGRATION_EVENT_CURSOR_STALE_GENERATION:
        return "INTEGRATION_EVENT_CURSOR_STALE_GENERATION";
    default:
        return NULL;
    }
}
