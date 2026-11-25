#ifndef HFOC_INTEGRATION_EVENT_CURSOR_H
#define HFOC_INTEGRATION_EVENT_CURSOR_H

#include "event_ring.h"

#include <stdbool.h>
#include <stdint.h>

#define INTEGRATION_EVENT_CURSOR_VERSION 1U
#define INTEGRATION_EVENT_CURSOR_MAX_RECORDS 64U

enum integration_event_cursor_status {
    INTEGRATION_EVENT_CURSOR_OK = 0,
    INTEGRATION_EVENT_CURSOR_NULL = 1,
    INTEGRATION_EVENT_CURSOR_BAD_CAPACITY = 2,
    INTEGRATION_EVENT_CURSOR_BAD_STATE = 3,
    INTEGRATION_EVENT_CURSOR_INVALID_EVENT = 4,
    INTEGRATION_EVENT_CURSOR_FULL = 5,
    INTEGRATION_EVENT_CURSOR_EMPTY = 6,
    INTEGRATION_EVENT_CURSOR_NOT_FOUND = 7,
    INTEGRATION_EVENT_CURSOR_OUT_OF_ORDER = 8,
    INTEGRATION_EVENT_CURSOR_OVERFLOW = 9,
    INTEGRATION_EVENT_CURSOR_STALE_GENERATION = 10,
};

struct integration_event_cursor_record {
    uint64_t export_sequence;
    uint64_t source_sequence;
    uint64_t generation;
    struct kernel_event event;
    bool acknowledged;
};

struct integration_event_cursor {
    uint32_t version;
    uint32_t capacity;
    uint64_t next_export_sequence;
    uint64_t last_ack_sequence;
    uint64_t mutation_generation;
    uint32_t pending_count;
    struct integration_event_cursor_record
        records[INTEGRATION_EVENT_CURSOR_MAX_RECORDS];
};

enum integration_event_cursor_status integration_event_cursor_init(
    struct integration_event_cursor *cursor,
    uint32_t capacity);
enum integration_event_cursor_status integration_event_cursor_export(
    struct integration_event_cursor *cursor,
    const struct kernel_event *event);
enum integration_event_cursor_status integration_event_cursor_acknowledge(
    struct integration_event_cursor *cursor,
    uint64_t export_sequence);
enum integration_event_cursor_status integration_event_cursor_snapshot(
    const struct integration_event_cursor *cursor,
    struct integration_event_cursor_record *records_out,
    uint32_t capacity,
    uint32_t *count_out,
    uint64_t *generation_out);
uint32_t integration_event_cursor_pending_count(
    const struct integration_event_cursor *cursor);
uint32_t integration_event_cursor_capacity(
    const struct integration_event_cursor *cursor);
uint64_t integration_event_cursor_next_export_sequence(
    const struct integration_event_cursor *cursor);
uint64_t integration_event_cursor_last_ack_sequence(
    const struct integration_event_cursor *cursor);
const char *integration_event_cursor_status_string(
    enum integration_event_cursor_status status);

#ifdef HFOC_INTEGRATION_EVENT_CURSOR_HOST_TEST
void integration_event_cursor_host_test_set_snapshot_mutate(bool enabled);
#endif

#endif
