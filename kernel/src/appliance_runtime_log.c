#include "appliance_runtime_log.h"

#include <stddef.h>
#include <string.h>

static bool appliance_runtime_log_severity_valid(
    enum appliance_runtime_log_severity severity)
{
    return severity >= APPLIANCE_RUNTIME_LOG_SEVERITY_DEBUG &&
           severity <= APPLIANCE_RUNTIME_LOG_SEVERITY_CRITICAL;
}

static bool appliance_runtime_log_category_valid(
    enum appliance_runtime_log_category category)
{
    return category >= APPLIANCE_RUNTIME_LOG_CATEGORY_BOOT &&
           category <= APPLIANCE_RUNTIME_LOG_CATEGORY_PROCESS;
}

static void appliance_runtime_log_copy_record(
    struct appliance_runtime_log_entry *entry_out,
    const struct appliance_runtime_log_record *record,
    uint64_t sequence)
{
    memset(entry_out, 0, sizeof(*entry_out));
    entry_out->sequence = sequence;
    entry_out->ticks = record->ticks;
    entry_out->severity = record->severity;
    entry_out->category = record->category;
    entry_out->operation = record->operation;
    entry_out->result = record->result;
    entry_out->fault = record->fault;
    entry_out->context_generation = record->context_generation;
    entry_out->detail = record->detail;
}

static enum appliance_runtime_log_status
appliance_runtime_log_status_from_append_space(uint32_t count)
{
    if (count == APPLIANCE_RUNTIME_LOG_CAPACITY) {
        return APPLIANCE_RUNTIME_LOG_FULL;
    }
    return APPLIANCE_RUNTIME_LOG_OK;
}

enum appliance_runtime_log_status appliance_runtime_log_validate_record(
    const struct appliance_runtime_log_record *record)
{
    if (record == NULL) {
        return APPLIANCE_RUNTIME_LOG_NULL;
    }
    if (record->version != APPLIANCE_RUNTIME_LOG_VERSION) {
        return APPLIANCE_RUNTIME_LOG_BAD_VERSION;
    }
    if (!appliance_runtime_log_severity_valid(record->severity)) {
        return APPLIANCE_RUNTIME_LOG_BAD_SEVERITY;
    }
    if (!appliance_runtime_log_category_valid(record->category)) {
        return APPLIANCE_RUNTIME_LOG_BAD_CATEGORY;
    }
    if (record->operation < 1U ||
        record->operation > APPLIANCE_RUNTIME_LOG_OPERATION_MAX) {
        return APPLIANCE_RUNTIME_LOG_BAD_OPERATION;
    }
    if (record->result > APPLIANCE_RUNTIME_LOG_RESULT_MAX) {
        return APPLIANCE_RUNTIME_LOG_BAD_RESULT;
    }
    if (record->fault > APPLIANCE_RUNTIME_LOG_FAULT_MAX) {
        return APPLIANCE_RUNTIME_LOG_BAD_FAULT;
    }
    if (record->context_generation >
        APPLIANCE_RUNTIME_LOG_GENERATION_MAX) {
        return APPLIANCE_RUNTIME_LOG_BAD_GENERATION;
    }
    if (record->detail > APPLIANCE_RUNTIME_LOG_DETAIL_MAX) {
        return APPLIANCE_RUNTIME_LOG_BAD_DETAIL;
    }
    return APPLIANCE_RUNTIME_LOG_OK;
}

enum appliance_runtime_log_status appliance_runtime_log_init(
    struct appliance_runtime_log *log)
{
    if (log == NULL) {
        return APPLIANCE_RUNTIME_LOG_NULL;
    }
    if (log->published) {
        return APPLIANCE_RUNTIME_LOG_BAD_STATE;
    }
    memset(log, 0, sizeof(*log));
    log->published = true;
    return APPLIANCE_RUNTIME_LOG_OK;
}

enum appliance_runtime_log_status appliance_runtime_log_destroy(
    struct appliance_runtime_log *log)
{
    if (log == NULL) {
        return APPLIANCE_RUNTIME_LOG_NULL;
    }
    if (!log->published) {
        return APPLIANCE_RUNTIME_LOG_BAD_STATE;
    }
    memset(log, 0, sizeof(*log));
    return APPLIANCE_RUNTIME_LOG_OK;
}

enum appliance_runtime_log_status appliance_runtime_log_append(
    struct appliance_runtime_log *log,
    const struct appliance_runtime_log_record *record,
    uint64_t *sequence_out)
{
    enum appliance_runtime_log_status status;
    struct appliance_runtime_log_entry *slot;
    uint64_t sequence;
    uint32_t index;

    if (log == NULL || record == NULL) {
        return APPLIANCE_RUNTIME_LOG_NULL;
    }
    if (!log->published) {
        return APPLIANCE_RUNTIME_LOG_BAD_STATE;
    }
    status = appliance_runtime_log_validate_record(record);
    if (status != APPLIANCE_RUNTIME_LOG_OK) {
        return status;
    }
    status = appliance_runtime_log_status_from_append_space(log->count);
    if (status != APPLIANCE_RUNTIME_LOG_OK) {
        return status;
    }

    sequence = log->next_sequence;
    index = (uint32_t)(log->write_cursor % APPLIANCE_RUNTIME_LOG_CAPACITY);
    slot = &log->entries[index];
    appliance_runtime_log_copy_record(slot, record, sequence);
    log->next_sequence = sequence + 1U;
    ++log->write_cursor;
    ++log->count;
    if (sequence_out != NULL) {
        *sequence_out = sequence;
    }
    return APPLIANCE_RUNTIME_LOG_OK;
}

enum appliance_runtime_log_status appliance_runtime_log_snapshot(
    const struct appliance_runtime_log *log,
    struct appliance_runtime_log_entry *entries_out,
    uint32_t capacity,
    uint32_t *count_out)
{
    uint32_t index;
    uint64_t cursor;

    if (log == NULL || count_out == NULL) {
        return APPLIANCE_RUNTIME_LOG_NULL;
    }
    if ((entries_out == NULL) != (capacity == 0U)) {
        return APPLIANCE_RUNTIME_LOG_NULL;
    }
    if (!log->published) {
        return APPLIANCE_RUNTIME_LOG_BAD_STATE;
    }
    *count_out = log->count;
    if (entries_out == NULL) {
        return APPLIANCE_RUNTIME_LOG_OK;
    }
    if (capacity < log->count) {
        return APPLIANCE_RUNTIME_LOG_SNAPSHOT_OVERFLOW;
    }
    cursor = log->read_cursor;
    for (index = 0U; index < log->count; ++index) {
        uint32_t slot_index =
            (uint32_t)(cursor % APPLIANCE_RUNTIME_LOG_CAPACITY);

        entries_out[index] = log->entries[slot_index];
        ++cursor;
    }
    return APPLIANCE_RUNTIME_LOG_OK;
}

enum appliance_runtime_log_status appliance_runtime_log_pop(
    struct appliance_runtime_log *log,
    struct appliance_runtime_log_entry *entry_out)
{
    uint32_t index;

    if (log == NULL || entry_out == NULL) {
        return APPLIANCE_RUNTIME_LOG_NULL;
    }
    if (!log->published) {
        return APPLIANCE_RUNTIME_LOG_BAD_STATE;
    }
    if (log->count == 0U) {
        return APPLIANCE_RUNTIME_LOG_EMPTY;
    }
    index = (uint32_t)(log->read_cursor % APPLIANCE_RUNTIME_LOG_CAPACITY);
    *entry_out = log->entries[index];
    ++log->read_cursor;
    --log->count;
    return APPLIANCE_RUNTIME_LOG_OK;
}

enum appliance_runtime_log_status appliance_runtime_log_state(
    const struct appliance_runtime_log *log,
    struct appliance_runtime_log_state *state_out)
{
    if (log == NULL || state_out == NULL) {
        return APPLIANCE_RUNTIME_LOG_NULL;
    }
    if (!log->published) {
        return APPLIANCE_RUNTIME_LOG_BAD_STATE;
    }
    state_out->version = APPLIANCE_RUNTIME_LOG_VERSION;
    state_out->published = true;
    state_out->count = log->count;
    state_out->capacity = APPLIANCE_RUNTIME_LOG_CAPACITY;
    state_out->available =
        APPLIANCE_RUNTIME_LOG_CAPACITY - log->count;
    state_out->utilization_percent =
        (log->count * 100U) / APPLIANCE_RUNTIME_LOG_CAPACITY;
    state_out->next_sequence = log->next_sequence;
    state_out->write_cursor = log->write_cursor;
    state_out->read_cursor = log->read_cursor;
    return APPLIANCE_RUNTIME_LOG_OK;
}

const char *appliance_runtime_log_status_string(
    enum appliance_runtime_log_status status)
{
    switch (status) {
    case APPLIANCE_RUNTIME_LOG_OK:
        return "APPLIANCE_RUNTIME_LOG_OK";
    case APPLIANCE_RUNTIME_LOG_NULL:
        return "APPLIANCE_RUNTIME_LOG_NULL";
    case APPLIANCE_RUNTIME_LOG_BAD_STATE:
        return "APPLIANCE_RUNTIME_LOG_BAD_STATE";
    case APPLIANCE_RUNTIME_LOG_BAD_VERSION:
        return "APPLIANCE_RUNTIME_LOG_BAD_VERSION";
    case APPLIANCE_RUNTIME_LOG_BAD_SEVERITY:
        return "APPLIANCE_RUNTIME_LOG_BAD_SEVERITY";
    case APPLIANCE_RUNTIME_LOG_BAD_CATEGORY:
        return "APPLIANCE_RUNTIME_LOG_BAD_CATEGORY";
    case APPLIANCE_RUNTIME_LOG_BAD_OPERATION:
        return "APPLIANCE_RUNTIME_LOG_BAD_OPERATION";
    case APPLIANCE_RUNTIME_LOG_BAD_RESULT:
        return "APPLIANCE_RUNTIME_LOG_BAD_RESULT";
    case APPLIANCE_RUNTIME_LOG_BAD_FAULT:
        return "APPLIANCE_RUNTIME_LOG_BAD_FAULT";
    case APPLIANCE_RUNTIME_LOG_BAD_GENERATION:
        return "APPLIANCE_RUNTIME_LOG_BAD_GENERATION";
    case APPLIANCE_RUNTIME_LOG_BAD_DETAIL:
        return "APPLIANCE_RUNTIME_LOG_BAD_DETAIL";
    case APPLIANCE_RUNTIME_LOG_FULL:
        return "APPLIANCE_RUNTIME_LOG_FULL";
    case APPLIANCE_RUNTIME_LOG_EMPTY:
        return "APPLIANCE_RUNTIME_LOG_EMPTY";
    case APPLIANCE_RUNTIME_LOG_SNAPSHOT_OVERFLOW:
        return "APPLIANCE_RUNTIME_LOG_SNAPSHOT_OVERFLOW";
    default:
        return NULL;
    }
}

const char *appliance_runtime_log_severity_string(
    enum appliance_runtime_log_severity severity)
{
    switch (severity) {
    case APPLIANCE_RUNTIME_LOG_SEVERITY_NONE:
        return "NONE";
    case APPLIANCE_RUNTIME_LOG_SEVERITY_DEBUG:
        return "DEBUG";
    case APPLIANCE_RUNTIME_LOG_SEVERITY_INFO:
        return "INFO";
    case APPLIANCE_RUNTIME_LOG_SEVERITY_WARN:
        return "WARN";
    case APPLIANCE_RUNTIME_LOG_SEVERITY_ERROR:
        return "ERROR";
    case APPLIANCE_RUNTIME_LOG_SEVERITY_CRITICAL:
        return "CRITICAL";
    default:
        return NULL;
    }
}

const char *appliance_runtime_log_category_string(
    enum appliance_runtime_log_category category)
{
    switch (category) {
    case APPLIANCE_RUNTIME_LOG_CATEGORY_NONE:
        return "NONE";
    case APPLIANCE_RUNTIME_LOG_CATEGORY_BOOT:
        return "BOOT";
    case APPLIANCE_RUNTIME_LOG_CATEGORY_STORAGE:
        return "STORAGE";
    case APPLIANCE_RUNTIME_LOG_CATEGORY_CONFIGURATION:
        return "CONFIGURATION";
    case APPLIANCE_RUNTIME_LOG_CATEGORY_NETWORK:
        return "NETWORK";
    case APPLIANCE_RUNTIME_LOG_CATEGORY_SERVICE:
        return "SERVICE";
    case APPLIANCE_RUNTIME_LOG_CATEGORY_HTTPS_MANAGEMENT:
        return "HTTPS_MANAGEMENT";
    case APPLIANCE_RUNTIME_LOG_CATEGORY_MANAGEMENT:
        return "MANAGEMENT";
    case APPLIANCE_RUNTIME_LOG_CATEGORY_SECURITY:
        return "SECURITY";
    case APPLIANCE_RUNTIME_LOG_CATEGORY_PROCESS:
        return "PROCESS";
    default:
        return NULL;
    }
}
