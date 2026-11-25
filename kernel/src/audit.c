#include "audit.h"

#include <stddef.h>
#include <string.h>

#define AUDIT_TYPE_MASK UINT32_C(0xFF)
#define AUDIT_OPERATION_MASK UINT32_C(0xFF)
#define AUDIT_RESULT_MASK UINT32_C(0xFF)
#define AUDIT_PID_MASK UINT32_C(0xFF)
#define AUDIT_DOMAIN_MASK UINT32_C(0xFF)
#define AUDIT_GENERATION_MASK UINT32_C(0xFFFFFF)
#define AUDIT_SESSION_MASK UINT32_C(0xFF)
#define AUDIT_PRINCIPAL_MASK UINT32_C(0xFF)
#define AUDIT_COMMAND_MASK UINT32_C(0xFF)
#define AUDIT_CORRELATION_MASK UINT64_C(0xFFFFFF)

static bool audit_type_valid(uint32_t type) {
    switch (type) {
    case AUDIT_RECORD_PROCESS:
    case AUDIT_RECORD_SYSCALL:
    case AUDIT_RECORD_CAPABILITY:
    case AUDIT_RECORD_FAULT:
    case AUDIT_RECORD_REBOOT_REASON:
    case AUDIT_RECORD_MANAGEMENT:
    case AUDIT_RECORD_NETWORK:
    case AUDIT_RECORD_TRUST:
        return true;
    default:
        return false;
    }
}

static bool audit_record_valid(const struct audit_record *record) {
    return record != NULL &&
           audit_type_valid(record->type) &&
           (record->operation & ~AUDIT_OPERATION_MASK) == 0U &&
           (record->result & ~AUDIT_RESULT_MASK) == 0U &&
           (record->pid & ~AUDIT_PID_MASK) == 0U &&
           (record->domain & ~AUDIT_DOMAIN_MASK) == 0U &&
           (record->generation & ~AUDIT_GENERATION_MASK) == 0U &&
           (record->session_id & ~AUDIT_SESSION_MASK) == 0U &&
           (record->principal_id & ~AUDIT_PRINCIPAL_MASK) == 0U &&
           (record->command_id & ~AUDIT_COMMAND_MASK) == 0U &&
           (record->type == AUDIT_RECORD_MANAGEMENT ||
            (record->session_id == 0U &&
             record->principal_id == 0U &&
             record->command_id == 0U)) &&
           (record->type != AUDIT_RECORD_MANAGEMENT ||
            (record->pid == 0U &&
             record->domain == 0U &&
             record->generation == 0U));
}

bool audit_record_is_valid(const struct audit_record *record) {
    return audit_record_valid(record);
}

enum audit_status audit_record_init(
    struct audit_record *record,
    uint32_t type,
    uint32_t operation,
    uint32_t result,
    uint32_t pid,
    uint32_t domain,
    uint32_t generation,
    uint64_t detail)
{
    struct audit_record candidate;

    if (record == NULL) {
        return AUDIT_NULL;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.type = type;
    candidate.operation = operation;
    candidate.result = result;
    candidate.pid = pid;
    candidate.domain = domain;
    candidate.generation = generation;
    candidate.detail = detail;
    if (!audit_record_valid(&candidate)) {
        return AUDIT_INVALID_RECORD;
    }
    *record = candidate;
    return AUDIT_OK;
}

enum audit_status audit_record_init_management(
    struct audit_record *record,
    uint32_t operation,
    uint32_t result,
    uint32_t session_id,
    uint32_t principal_id,
    uint32_t command_id,
    uint64_t detail)
{
    struct audit_record candidate;

    if (record == NULL) {
        return AUDIT_NULL;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.type = AUDIT_RECORD_MANAGEMENT;
    candidate.operation = operation;
    candidate.result = result;
    candidate.session_id = session_id;
    candidate.principal_id = principal_id;
    candidate.command_id = command_id;
    candidate.detail = detail;
    if (!audit_record_valid(&candidate)) {
        return AUDIT_INVALID_RECORD;
    }
    *record = candidate;
    return AUDIT_OK;
}

enum audit_status audit_record_to_event(
    const struct audit_record *record,
    struct kernel_event *event_out)
{
    struct kernel_event event;

    if (record == NULL || event_out == NULL) {
        return AUDIT_NULL;
    }
    if (!audit_record_valid(record)) {
        return AUDIT_INVALID_RECORD;
    }
    memset(&event, 0, sizeof(event));
    event.type = record->type;
    event.flags = AUDIT_EVENT_FLAG_STRUCTURED;
    event.data[0] = (uint64_t)record->type |
                    ((uint64_t)record->operation << 8U) |
                    ((uint64_t)record->result << 16U) |
                    ((uint64_t)record->pid << 24U) |
                    ((uint64_t)record->domain << 32U) |
                    ((uint64_t)record->generation << 40U);
    event.data[1] = record->detail;
    event.data[2] = 0U;
    if (record->type == AUDIT_RECORD_MANAGEMENT) {
        event.data[2] = (uint64_t)record->session_id |
                        ((uint64_t)record->principal_id << 8U) |
                        ((uint64_t)record->command_id << 16U);
    }
    *event_out = event;
    return AUDIT_OK;
}

enum audit_status audit_record_from_event(
    const struct kernel_event *event,
    struct audit_record *record_out)
{
    struct audit_record record;

    if (event == NULL || record_out == NULL) {
        return AUDIT_NULL;
    }
    if (event->flags != AUDIT_EVENT_FLAG_STRUCTURED ||
        !audit_type_valid(event->type)) {
        return AUDIT_INVALID_EVENT;
    }
    if ((event->data[2] & ~AUDIT_CORRELATION_MASK) != 0U) {
        return AUDIT_INVALID_EVENT;
    }
    if (event->type != AUDIT_RECORD_MANAGEMENT &&
        event->data[2] != 0U) {
        return AUDIT_INVALID_EVENT;
    }
    memset(&record, 0, sizeof(record));
    record.sequence = event->sequence;
    record.type = (uint32_t)(event->data[0] & AUDIT_TYPE_MASK);
    record.operation = (uint32_t)((event->data[0] >> 8U) &
                                  AUDIT_OPERATION_MASK);
    record.result = (uint32_t)((event->data[0] >> 16U) &
                               AUDIT_RESULT_MASK);
    record.pid = (uint32_t)((event->data[0] >> 24U) &
                            AUDIT_PID_MASK);
    record.domain = (uint32_t)((event->data[0] >> 32U) &
                               AUDIT_DOMAIN_MASK);
    record.generation = (uint32_t)((event->data[0] >> 40U) &
                                   AUDIT_GENERATION_MASK);
    if (event->type == AUDIT_RECORD_MANAGEMENT) {
        record.session_id = (uint32_t)(event->data[2] &
                                       AUDIT_SESSION_MASK);
        record.principal_id = (uint32_t)((event->data[2] >> 8U) &
                                         AUDIT_PRINCIPAL_MASK);
        record.command_id = (uint32_t)((event->data[2] >> 16U) &
                                       AUDIT_COMMAND_MASK);
    }
    record.detail = event->data[1];
    if (event->type != record.type) {
        return AUDIT_INVALID_EVENT;
    }
    if (!audit_record_valid(&record)) {
        return AUDIT_INVALID_EVENT;
    }
    *record_out = record;
    return AUDIT_OK;
}

enum audit_status audit_record_append(
    struct event_ring *ring,
    const struct audit_record *record,
    uint64_t *sequence_out)
{
    struct kernel_event event;
    enum audit_status encode_status;
    enum event_ring_status ring_status;

    if (ring == NULL || record == NULL) {
        return AUDIT_NULL;
    }
    if (!audit_record_valid(record)) {
        return AUDIT_INVALID_RECORD;
    }
    encode_status = audit_record_to_event(record, &event);
    if (encode_status != AUDIT_OK) {
        return encode_status;
    }
    ring_status = event_ring_push(ring, &event);
    if (ring_status != EVENT_RING_OK) {
        switch (ring_status) {
        case EVENT_RING_NULL:
            return AUDIT_NULL;
        case EVENT_RING_BAD_STATE:
            return AUDIT_BAD_STATE;
        case EVENT_RING_FULL:
            return AUDIT_FULL;
        default:
            return AUDIT_BAD_STATE;
        }
    }
    if (sequence_out != NULL) {
        *sequence_out = event_ring_write_cursor(ring) - 1U;
    }
    return AUDIT_OK;
}

enum audit_status audit_snapshot(
    const struct event_ring *ring,
    struct audit_record *records_out,
    uint32_t capacity,
    uint32_t *count_out)
{
    struct kernel_event raw[KERNEL_EVENT_RING_CAPACITY];
    struct audit_record decoded[KERNEL_EVENT_RING_CAPACITY];
    enum event_ring_status raw_status;
    enum audit_status decode_status;
    uint32_t raw_count;
    uint32_t index;

    if (ring == NULL || count_out == NULL) {
        return AUDIT_NULL;
    }
    if ((records_out == NULL) != (capacity == 0U)) {
        return AUDIT_NULL;
    }
    if (!event_ring_is_published(ring)) {
        return AUDIT_BAD_STATE;
    }
    if (records_out == NULL) {
        *count_out = event_ring_count(ring);
        return AUDIT_OK;
    }

    raw_status = event_ring_snapshot(ring, raw,
                                     KERNEL_EVENT_RING_CAPACITY,
                                     &raw_count);
    if (raw_status != EVENT_RING_OK) {
        switch (raw_status) {
        case EVENT_RING_NULL:
            return AUDIT_NULL;
        case EVENT_RING_BAD_STATE:
            return AUDIT_BAD_STATE;
        case EVENT_RING_OVERFLOW:
            return AUDIT_SNAPSHOT_OVERFLOW;
        default:
            return AUDIT_BAD_STATE;
        }
    }
    if (capacity < raw_count) {
        *count_out = raw_count;
        return AUDIT_SNAPSHOT_OVERFLOW;
    }

    for (index = 0U; index < raw_count; ++index) {
        decode_status = audit_record_from_event(&raw[index],
                                                &decoded[index]);
        if (decode_status != AUDIT_OK) {
            return decode_status;
        }
    }
    for (index = 0U; index < raw_count; ++index) {
        records_out[index] = decoded[index];
    }
    *count_out = raw_count;
    return AUDIT_OK;
}

enum audit_status audit_record_pop(
    struct event_ring *ring,
    struct audit_record *record_out)
{
    struct kernel_event raw[KERNEL_EVENT_RING_CAPACITY];
    struct audit_record candidate;
    enum event_ring_status ring_status;
    enum audit_status decode_status;
    uint32_t count;

    if (ring == NULL || record_out == NULL) {
        return AUDIT_NULL;
    }
    if (!event_ring_is_published(ring)) {
        return AUDIT_BAD_STATE;
    }

    ring_status = event_ring_snapshot(ring, raw,
                                      KERNEL_EVENT_RING_CAPACITY,
                                      &count);
    if (ring_status != EVENT_RING_OK) {
        switch (ring_status) {
        case EVENT_RING_NULL:
            return AUDIT_NULL;
        case EVENT_RING_BAD_STATE:
            return AUDIT_BAD_STATE;
        case EVENT_RING_OVERFLOW:
            return AUDIT_SNAPSHOT_OVERFLOW;
        default:
            return AUDIT_BAD_STATE;
        }
    }
    if (count == 0U) {
        return AUDIT_EMPTY;
    }

    decode_status = audit_record_from_event(&raw[0], &candidate);
    if (decode_status != AUDIT_OK) {
        return decode_status;
    }
    ring_status = event_ring_pop(ring, &raw[0]);
    if (ring_status != EVENT_RING_OK) {
        return AUDIT_BAD_STATE;
    }
    *record_out = candidate;
    return AUDIT_OK;
}

const char *audit_status_string(enum audit_status status) {
    switch (status) {
    case AUDIT_OK:
        return "AUDIT_OK";
    case AUDIT_NULL:
        return "AUDIT_NULL";
    case AUDIT_BAD_STATE:
        return "AUDIT_BAD_STATE";
    case AUDIT_FULL:
        return "AUDIT_FULL";
    case AUDIT_INVALID_RECORD:
        return "AUDIT_INVALID_RECORD";
    case AUDIT_INVALID_EVENT:
        return "AUDIT_INVALID_EVENT";
    case AUDIT_SNAPSHOT_OVERFLOW:
        return "AUDIT_SNAPSHOT_OVERFLOW";
    case AUDIT_EMPTY:
        return "AUDIT_EMPTY";
    default:
        return "AUDIT_UNKNOWN";
    }
}

const char *audit_record_type_string(enum audit_record_type type) {
    switch (type) {
    case AUDIT_RECORD_PROCESS:
        return "AUDIT_RECORD_PROCESS";
    case AUDIT_RECORD_SYSCALL:
        return "AUDIT_RECORD_SYSCALL";
    case AUDIT_RECORD_CAPABILITY:
        return "AUDIT_RECORD_CAPABILITY";
    case AUDIT_RECORD_FAULT:
        return "AUDIT_RECORD_FAULT";
    case AUDIT_RECORD_REBOOT_REASON:
        return "AUDIT_RECORD_REBOOT_REASON";
    case AUDIT_RECORD_MANAGEMENT:
        return "AUDIT_RECORD_MANAGEMENT";
    case AUDIT_RECORD_NETWORK:
        return "AUDIT_RECORD_NETWORK";
    case AUDIT_RECORD_TRUST:
        return "AUDIT_RECORD_TRUST";
    default:
        return "AUDIT_RECORD_UNKNOWN";
    }
}
