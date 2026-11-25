#ifndef HFOC_APPLIANCE_RUNTIME_LOG_H
#define HFOC_APPLIANCE_RUNTIME_LOG_H

#include <stdbool.h>
#include <stdint.h>

#define APPLIANCE_RUNTIME_LOG_VERSION 1U
#define APPLIANCE_RUNTIME_LOG_CAPACITY 64U
#define APPLIANCE_RUNTIME_LOG_OPERATION_MAX UINT32_C(0xFF)
#define APPLIANCE_RUNTIME_LOG_RESULT_MAX UINT32_C(0xFF)
#define APPLIANCE_RUNTIME_LOG_FAULT_MAX UINT32_C(0xFFFF)
#define APPLIANCE_RUNTIME_LOG_GENERATION_MAX UINT32_C(0xFFFFFF)
#define APPLIANCE_RUNTIME_LOG_DETAIL_MAX UINT32_C(0xFFFFFFFF)

enum appliance_runtime_log_status {
    APPLIANCE_RUNTIME_LOG_OK = 0,
    APPLIANCE_RUNTIME_LOG_NULL = 1,
    APPLIANCE_RUNTIME_LOG_BAD_STATE = 2,
    APPLIANCE_RUNTIME_LOG_BAD_VERSION = 3,
    APPLIANCE_RUNTIME_LOG_BAD_SEVERITY = 4,
    APPLIANCE_RUNTIME_LOG_BAD_CATEGORY = 5,
    APPLIANCE_RUNTIME_LOG_BAD_OPERATION = 6,
    APPLIANCE_RUNTIME_LOG_BAD_RESULT = 7,
    APPLIANCE_RUNTIME_LOG_BAD_FAULT = 8,
    APPLIANCE_RUNTIME_LOG_BAD_GENERATION = 9,
    APPLIANCE_RUNTIME_LOG_BAD_DETAIL = 10,
    APPLIANCE_RUNTIME_LOG_FULL = 11,
    APPLIANCE_RUNTIME_LOG_EMPTY = 12,
    APPLIANCE_RUNTIME_LOG_SNAPSHOT_OVERFLOW = 13,
};

enum appliance_runtime_log_severity {
    APPLIANCE_RUNTIME_LOG_SEVERITY_NONE = 0,
    APPLIANCE_RUNTIME_LOG_SEVERITY_DEBUG = 1,
    APPLIANCE_RUNTIME_LOG_SEVERITY_INFO = 2,
    APPLIANCE_RUNTIME_LOG_SEVERITY_WARN = 3,
    APPLIANCE_RUNTIME_LOG_SEVERITY_ERROR = 4,
    APPLIANCE_RUNTIME_LOG_SEVERITY_CRITICAL = 5,
    APPLIANCE_RUNTIME_LOG_SEVERITY_COUNT = 6,
};

enum appliance_runtime_log_category {
    APPLIANCE_RUNTIME_LOG_CATEGORY_NONE = 0,
    APPLIANCE_RUNTIME_LOG_CATEGORY_BOOT = 1,
    APPLIANCE_RUNTIME_LOG_CATEGORY_STORAGE = 2,
    APPLIANCE_RUNTIME_LOG_CATEGORY_CONFIGURATION = 3,
    APPLIANCE_RUNTIME_LOG_CATEGORY_NETWORK = 4,
    APPLIANCE_RUNTIME_LOG_CATEGORY_SERVICE = 5,
    APPLIANCE_RUNTIME_LOG_CATEGORY_HTTPS_MANAGEMENT = 6,
    APPLIANCE_RUNTIME_LOG_CATEGORY_MANAGEMENT = 7,
    APPLIANCE_RUNTIME_LOG_CATEGORY_SECURITY = 8,
    APPLIANCE_RUNTIME_LOG_CATEGORY_PROCESS = 9,
    APPLIANCE_RUNTIME_LOG_CATEGORY_COUNT = 10,
};

struct appliance_runtime_log_record {
    uint32_t version;
    enum appliance_runtime_log_severity severity;
    enum appliance_runtime_log_category category;
    uint32_t operation;
    uint32_t result;
    uint32_t fault;
    uint32_t context_generation;
    uint64_t ticks;
    uint64_t detail;
};

struct appliance_runtime_log_entry {
    uint64_t sequence;
    uint64_t ticks;
    enum appliance_runtime_log_severity severity;
    enum appliance_runtime_log_category category;
    uint32_t operation;
    uint32_t result;
    uint32_t fault;
    uint32_t context_generation;
    uint64_t detail;
};

struct appliance_runtime_log {
    bool published;
    uint64_t next_sequence;
    uint64_t write_cursor;
    uint64_t read_cursor;
    uint32_t count;
    struct appliance_runtime_log_entry entries[
        APPLIANCE_RUNTIME_LOG_CAPACITY];
};

struct appliance_runtime_log_state {
    uint32_t version;
    bool published;
    uint32_t count;
    uint32_t capacity;
    uint32_t available;
    uint32_t utilization_percent;
    uint64_t next_sequence;
    uint64_t write_cursor;
    uint64_t read_cursor;
};

enum appliance_runtime_log_status appliance_runtime_log_validate_record(
    const struct appliance_runtime_log_record *record);
enum appliance_runtime_log_status appliance_runtime_log_init(
    struct appliance_runtime_log *log);
enum appliance_runtime_log_status appliance_runtime_log_destroy(
    struct appliance_runtime_log *log);
enum appliance_runtime_log_status appliance_runtime_log_append(
    struct appliance_runtime_log *log,
    const struct appliance_runtime_log_record *record,
    uint64_t *sequence_out);
enum appliance_runtime_log_status appliance_runtime_log_snapshot(
    const struct appliance_runtime_log *log,
    struct appliance_runtime_log_entry *entries_out,
    uint32_t capacity,
    uint32_t *count_out);
enum appliance_runtime_log_status appliance_runtime_log_pop(
    struct appliance_runtime_log *log,
    struct appliance_runtime_log_entry *entry_out);
enum appliance_runtime_log_status appliance_runtime_log_state(
    const struct appliance_runtime_log *log,
    struct appliance_runtime_log_state *state_out);
const char *appliance_runtime_log_status_string(
    enum appliance_runtime_log_status status);
const char *appliance_runtime_log_severity_string(
    enum appliance_runtime_log_severity severity);
const char *appliance_runtime_log_category_string(
    enum appliance_runtime_log_category category);

#endif
