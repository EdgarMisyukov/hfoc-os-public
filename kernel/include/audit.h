#ifndef HFOC_AUDIT_H
#define HFOC_AUDIT_H

#include "event_ring.h"
#include <stdbool.h>
#include <stdint.h>

#define AUDIT_EVENT_FLAG_STRUCTURED 1U

enum audit_record_type {
    AUDIT_RECORD_PROCESS = 1,
    AUDIT_RECORD_SYSCALL = 2,
    AUDIT_RECORD_CAPABILITY = 3,
    AUDIT_RECORD_FAULT = 4,
    AUDIT_RECORD_REBOOT_REASON = 5,
    AUDIT_RECORD_MANAGEMENT = 6,
    AUDIT_RECORD_NETWORK = 7,
    AUDIT_RECORD_TRUST = 8,
};

enum audit_status {
    AUDIT_OK = 0,
    AUDIT_NULL = 1,
    AUDIT_BAD_STATE = 2,
    AUDIT_FULL = 3,
    AUDIT_INVALID_RECORD = 4,
    AUDIT_INVALID_EVENT = 5,
    AUDIT_SNAPSHOT_OVERFLOW = 6,
    AUDIT_EMPTY = 7,
};

struct audit_record {
    uint64_t sequence;
    uint32_t type;
    uint32_t operation;
    uint32_t result;
    uint32_t pid;
    uint32_t domain;
    uint32_t generation;
    uint32_t session_id;
    uint32_t principal_id;
    uint32_t command_id;
    uint64_t detail;
};

enum audit_status audit_record_init(
    struct audit_record *record,
    uint32_t type,
    uint32_t operation,
    uint32_t result,
    uint32_t pid,
    uint32_t domain,
    uint32_t generation,
    uint64_t detail);
/* Exact validity of a decoded record, shared with the ITEM-377 audit chain so
 * a record can never be chained under different rules than it was accepted. */
bool audit_record_is_valid(const struct audit_record *record);
enum audit_status audit_record_init_management(
    struct audit_record *record,
    uint32_t operation,
    uint32_t result,
    uint32_t session_id,
    uint32_t principal_id,
    uint32_t command_id,
    uint64_t detail);
enum audit_status audit_record_append(
    struct event_ring *ring,
    const struct audit_record *record,
    uint64_t *sequence_out);
enum audit_status audit_record_to_event(
    const struct audit_record *record,
    struct kernel_event *event_out);
enum audit_status audit_record_from_event(
    const struct kernel_event *event,
    struct audit_record *record_out);
enum audit_status audit_snapshot(
    const struct event_ring *ring,
    struct audit_record *records_out,
    uint32_t capacity,
    uint32_t *count_out);
enum audit_status audit_record_pop(
    struct event_ring *ring,
    struct audit_record *record_out);
const char *audit_status_string(enum audit_status status);
const char *audit_record_type_string(enum audit_record_type type);

#endif
