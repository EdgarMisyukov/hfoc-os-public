#ifndef HFOC_NETWORK_EVENT_H
#define HFOC_NETWORK_EVENT_H

#include "audit.h"
#include "event_ring.h"

#include <stdint.h>

#define NETWORK_EVENT_VERSION 1U
#define NETWORK_EVENT_TYPE_LINK 1U
#define NETWORK_EVENT_TYPE_ADDRESS 2U
#define NETWORK_EVENT_TYPE_ADMISSION 3U
#define NETWORK_EVENT_TYPE_TRANSPORT 4U
#define NETWORK_EVENT_TYPE_COUNT 5U
#define NETWORK_EVENT_RESULT_OK 0U
#define NETWORK_EVENT_RESULT_FAILURE 1U
#define NETWORK_EVENT_DEVICE_ID_MAX 255U
#define NETWORK_EVENT_GENERATION_MAX UINT32_C(0xFFFFFF)

enum network_event_status {
    NETWORK_EVENT_OK = 0,
    NETWORK_EVENT_NULL = 1,
    NETWORK_EVENT_BAD_VERSION = 2,
    NETWORK_EVENT_INVALID_TYPE = 3,
    NETWORK_EVENT_INVALID_RESULT = 4,
    NETWORK_EVENT_INVALID_DEVICE_ID = 5,
    NETWORK_EVENT_INVALID_GENERATION = 6,
    NETWORK_EVENT_BAD_STATE = 7,
    NETWORK_EVENT_FULL = 8,
    NETWORK_EVENT_EMPTY = 9,
    NETWORK_EVENT_INVALID_EVENT = 10,
    NETWORK_EVENT_SNAPSHOT_OVERFLOW = 11,
};

struct network_event {
    uint32_t version;
    uint32_t type;
    uint32_t result;
    uint32_t device_id;
    uint32_t generation;
    uint64_t detail;
};

enum network_event_status network_event_validate(
    const struct network_event *event);
enum network_event_status network_event_append(
    struct event_ring *ring,
    const struct network_event *event,
    uint64_t *sequence_out);
enum network_event_status network_event_from_event(
    const struct kernel_event *event,
    struct network_event *event_out);
enum network_event_status network_event_from_audit(
    const struct audit_record *record,
    struct network_event *event_out);
enum network_event_status network_event_snapshot(
    const struct event_ring *ring,
    struct network_event *events_out,
    uint32_t capacity,
    uint32_t *count_out);
enum network_event_status network_event_pop(
    struct event_ring *ring,
    struct network_event *event_out);
const char *network_event_type_string(uint32_t type);
const char *network_event_result_string(uint32_t result);
const char *network_event_status_string(enum network_event_status status);

#endif
