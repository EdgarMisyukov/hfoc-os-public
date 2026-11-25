#include "network_event.h"

#include <stddef.h>
#include <string.h>

static bool network_event_type_valid(uint32_t type)
{
    return type >= NETWORK_EVENT_TYPE_LINK &&
           type < NETWORK_EVENT_TYPE_COUNT;
}

static bool network_event_result_valid(uint32_t result)
{
    return result == NETWORK_EVENT_RESULT_OK ||
           result == NETWORK_EVENT_RESULT_FAILURE;
}

enum network_event_status network_event_validate(
    const struct network_event *event)
{
    if (event == NULL) {
        return NETWORK_EVENT_NULL;
    }
    if (event->version != NETWORK_EVENT_VERSION) {
        return NETWORK_EVENT_BAD_VERSION;
    }
    if (!network_event_type_valid(event->type)) {
        return NETWORK_EVENT_INVALID_TYPE;
    }
    if (!network_event_result_valid(event->result)) {
        return NETWORK_EVENT_INVALID_RESULT;
    }
    if (event->device_id > NETWORK_EVENT_DEVICE_ID_MAX) {
        return NETWORK_EVENT_INVALID_DEVICE_ID;
    }
    if (event->generation > NETWORK_EVENT_GENERATION_MAX) {
        return NETWORK_EVENT_INVALID_GENERATION;
    }
    return NETWORK_EVENT_OK;
}

enum network_event_status network_event_append(
    struct event_ring *ring,
    const struct network_event *event,
    uint64_t *sequence_out)
{
    struct audit_record record;
    enum network_event_status validation_status;
    enum audit_status audit_status;

    if (ring == NULL || event == NULL) {
        return NETWORK_EVENT_NULL;
    }
    validation_status = network_event_validate(event);
    if (validation_status != NETWORK_EVENT_OK) {
        return validation_status;
    }
    if (!event_ring_is_published(ring)) {
        return NETWORK_EVENT_BAD_STATE;
    }
    if (event_ring_count(ring) >= KERNEL_EVENT_RING_CAPACITY) {
        return NETWORK_EVENT_FULL;
    }

    audit_status = audit_record_init(
        &record, AUDIT_RECORD_NETWORK, event->type, event->result,
        event->device_id, 0U, event->generation, event->detail);
    if (audit_status != AUDIT_OK) {
        return NETWORK_EVENT_BAD_STATE;
    }
    audit_status = audit_record_append(ring, &record, sequence_out);
    switch (audit_status) {
    case AUDIT_OK:
        return NETWORK_EVENT_OK;
    case AUDIT_BAD_STATE:
        return NETWORK_EVENT_BAD_STATE;
    case AUDIT_FULL:
        return NETWORK_EVENT_FULL;
    default:
        return NETWORK_EVENT_BAD_STATE;
    }
}

enum network_event_status network_event_from_audit(
    const struct audit_record *record,
    struct network_event *event_out)
{
    struct network_event event;

    if (record == NULL || event_out == NULL) {
        return NETWORK_EVENT_NULL;
    }
    if (record->type != AUDIT_RECORD_NETWORK) {
        return NETWORK_EVENT_INVALID_EVENT;
    }

    memset(&event, 0, sizeof(event));
    event.version = NETWORK_EVENT_VERSION;
    event.type = record->operation;
    event.result = record->result;
    event.device_id = record->pid;
    event.generation = record->generation;
    event.detail = record->detail;
    if (network_event_validate(&event) != NETWORK_EVENT_OK) {
        return NETWORK_EVENT_INVALID_EVENT;
    }
    *event_out = event;
    return NETWORK_EVENT_OK;
}

enum network_event_status network_event_from_event(
    const struct kernel_event *event,
    struct network_event *event_out)
{
    struct audit_record record;
    enum audit_status audit_status;

    if (event == NULL || event_out == NULL) {
        return NETWORK_EVENT_NULL;
    }
    audit_status = audit_record_from_event(event, &record);
    if (audit_status != AUDIT_OK) {
        return NETWORK_EVENT_INVALID_EVENT;
    }
    return network_event_from_audit(&record, event_out);
}

enum network_event_status network_event_snapshot(
    const struct event_ring *ring,
    struct network_event *events_out,
    uint32_t capacity,
    uint32_t *count_out)
{
    struct kernel_event raw[KERNEL_EVENT_RING_CAPACITY];
    struct network_event decoded[KERNEL_EVENT_RING_CAPACITY];
    enum event_ring_status raw_status;
    enum network_event_status decode_status;
    uint32_t raw_count;
    uint32_t index;

    if (ring == NULL || count_out == NULL) {
        return NETWORK_EVENT_NULL;
    }
    if ((events_out == NULL) != (capacity == 0U)) {
        return NETWORK_EVENT_NULL;
    }
    if (!event_ring_is_published(ring)) {
        return NETWORK_EVENT_BAD_STATE;
    }
    if (events_out == NULL) {
        *count_out = event_ring_count(ring);
        return NETWORK_EVENT_OK;
    }

    raw_status = event_ring_snapshot(ring, raw,
                                     KERNEL_EVENT_RING_CAPACITY,
                                     &raw_count);
    if (raw_status != EVENT_RING_OK) {
        switch (raw_status) {
        case EVENT_RING_NULL:
            return NETWORK_EVENT_NULL;
        case EVENT_RING_BAD_STATE:
            return NETWORK_EVENT_BAD_STATE;
        case EVENT_RING_OVERFLOW:
            return NETWORK_EVENT_SNAPSHOT_OVERFLOW;
        default:
            return NETWORK_EVENT_BAD_STATE;
        }
    }
    if (capacity < raw_count) {
        *count_out = raw_count;
        return NETWORK_EVENT_SNAPSHOT_OVERFLOW;
    }

    for (index = 0U; index < raw_count; ++index) {
        decode_status = network_event_from_event(
            &raw[index], &decoded[index]);
        if (decode_status != NETWORK_EVENT_OK) {
            return decode_status;
        }
    }
    for (index = 0U; index < raw_count; ++index) {
        events_out[index] = decoded[index];
    }
    *count_out = raw_count;
    return NETWORK_EVENT_OK;
}

enum network_event_status network_event_pop(
    struct event_ring *ring,
    struct network_event *event_out)
{
    struct kernel_event raw[KERNEL_EVENT_RING_CAPACITY];
    struct network_event candidate;
    enum event_ring_status ring_status;
    enum network_event_status decode_status;
    uint32_t count;

    if (ring == NULL || event_out == NULL) {
        return NETWORK_EVENT_NULL;
    }
    if (!event_ring_is_published(ring)) {
        return NETWORK_EVENT_BAD_STATE;
    }

    ring_status = event_ring_snapshot(
        ring, raw, KERNEL_EVENT_RING_CAPACITY, &count);
    if (ring_status != EVENT_RING_OK) {
        switch (ring_status) {
        case EVENT_RING_NULL:
            return NETWORK_EVENT_NULL;
        case EVENT_RING_BAD_STATE:
            return NETWORK_EVENT_BAD_STATE;
        case EVENT_RING_OVERFLOW:
            return NETWORK_EVENT_SNAPSHOT_OVERFLOW;
        default:
            return NETWORK_EVENT_BAD_STATE;
        }
    }
    if (count == 0U) {
        return NETWORK_EVENT_EMPTY;
    }

    decode_status = network_event_from_event(&raw[0], &candidate);
    if (decode_status != NETWORK_EVENT_OK) {
        return decode_status;
    }
    ring_status = event_ring_pop(ring, &raw[0]);
    if (ring_status != EVENT_RING_OK) {
        return NETWORK_EVENT_BAD_STATE;
    }
    *event_out = candidate;
    return NETWORK_EVENT_OK;
}

const char *network_event_type_string(uint32_t type)
{
    switch (type) {
    case NETWORK_EVENT_TYPE_LINK:
        return "NETWORK_EVENT_TYPE_LINK";
    case NETWORK_EVENT_TYPE_ADDRESS:
        return "NETWORK_EVENT_TYPE_ADDRESS";
    case NETWORK_EVENT_TYPE_ADMISSION:
        return "NETWORK_EVENT_TYPE_ADMISSION";
    case NETWORK_EVENT_TYPE_TRANSPORT:
        return "NETWORK_EVENT_TYPE_TRANSPORT";
    default:
        return NULL;
    }
}

const char *network_event_result_string(uint32_t result)
{
    switch (result) {
    case NETWORK_EVENT_RESULT_OK:
        return "NETWORK_EVENT_RESULT_OK";
    case NETWORK_EVENT_RESULT_FAILURE:
        return "NETWORK_EVENT_RESULT_FAILURE";
    default:
        return NULL;
    }
}

const char *network_event_status_string(
    enum network_event_status status)
{
    switch (status) {
    case NETWORK_EVENT_OK:
        return "NETWORK_EVENT_OK";
    case NETWORK_EVENT_NULL:
        return "NETWORK_EVENT_NULL";
    case NETWORK_EVENT_BAD_VERSION:
        return "NETWORK_EVENT_BAD_VERSION";
    case NETWORK_EVENT_INVALID_TYPE:
        return "NETWORK_EVENT_INVALID_TYPE";
    case NETWORK_EVENT_INVALID_RESULT:
        return "NETWORK_EVENT_INVALID_RESULT";
    case NETWORK_EVENT_INVALID_DEVICE_ID:
        return "NETWORK_EVENT_INVALID_DEVICE_ID";
    case NETWORK_EVENT_INVALID_GENERATION:
        return "NETWORK_EVENT_INVALID_GENERATION";
    case NETWORK_EVENT_BAD_STATE:
        return "NETWORK_EVENT_BAD_STATE";
    case NETWORK_EVENT_FULL:
        return "NETWORK_EVENT_FULL";
    case NETWORK_EVENT_EMPTY:
        return "NETWORK_EVENT_EMPTY";
    case NETWORK_EVENT_INVALID_EVENT:
        return "NETWORK_EVENT_INVALID_EVENT";
    case NETWORK_EVENT_SNAPSHOT_OVERFLOW:
        return "NETWORK_EVENT_SNAPSHOT_OVERFLOW";
    default:
        return NULL;
    }
}
