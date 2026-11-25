#ifndef HFOC_HEALTH_CHECK_H
#define HFOC_HEALTH_CHECK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HEALTH_CHECK_VERSION 1U
#define HEALTH_CHECK_MAX_DESCRIPTORS 16U
#define HEALTH_CHECK_ID_MIN 1U
#define HEALTH_CHECK_ID_MAX HEALTH_CHECK_MAX_DESCRIPTORS
#define HEALTH_CHECK_CADENCE_MIN_MS 1000U
#define HEALTH_CHECK_CADENCE_MAX_MS UINT32_C(86400000)

enum health_check_status {
    HEALTH_CHECK_OK = 0,
    HEALTH_CHECK_NULL = 1,
    HEALTH_CHECK_BAD_STATE = 2,
    HEALTH_CHECK_ALREADY_PUBLISHED = 3,
    HEALTH_CHECK_BAD_VERSION = 4,
    HEALTH_CHECK_BAD_COUNT = 5,
    HEALTH_CHECK_BAD_ID = 6,
    HEALTH_CHECK_BAD_SEVERITY = 7,
    HEALTH_CHECK_BAD_CADENCE = 8,
    HEALTH_CHECK_DUPLICATE = 9,
    HEALTH_CHECK_NOT_PUBLISHED = 10,
    HEALTH_CHECK_UNKNOWN = 11,
    HEALTH_CHECK_BAD_RESULT = 12,
};

enum health_check_severity {
    HEALTH_CHECK_SEVERITY_INFO = 1,
    HEALTH_CHECK_SEVERITY_WARNING = 2,
    HEALTH_CHECK_SEVERITY_CRITICAL = 3,
};

enum health_check_result_state {
    HEALTH_CHECK_RESULT_UNKNOWN = 0,
    HEALTH_CHECK_RESULT_PASS = 1,
    HEALTH_CHECK_RESULT_WARN = 2,
    HEALTH_CHECK_RESULT_FAIL = 3,
};

struct health_check_descriptor {
    uint32_t version;
    uint32_t check_id;
    enum health_check_severity severity;
    uint32_t cadence_ms;
};

struct health_check_entry {
    struct health_check_descriptor descriptor;
    enum health_check_result_state result;
};

struct health_check_snapshot {
    uint32_t version;
    uint32_t generation;
    size_t descriptor_count;
    struct health_check_entry entries[
        HEALTH_CHECK_MAX_DESCRIPTORS];
};

struct health_check_store {
    uint32_t version;
    bool initialized;
    bool published;
    uint32_t generation;
    size_t descriptor_count;
    struct health_check_entry entries[
        HEALTH_CHECK_MAX_DESCRIPTORS];
};

enum health_check_status health_check_init(
    struct health_check_store *store);
enum health_check_status health_check_publish(
    struct health_check_store *store,
    const struct health_check_descriptor *descriptors,
    size_t descriptor_count);
enum health_check_status health_check_result(
    struct health_check_store *store,
    uint32_t check_id,
    enum health_check_result_state result);
enum health_check_status health_check_lookup(
    const struct health_check_store *store,
    uint32_t check_id,
    struct health_check_descriptor *descriptor_out);
enum health_check_status health_check_snapshot(
    const struct health_check_store *store,
    struct health_check_snapshot *snapshot_out);
const char *health_check_status_string(
    enum health_check_status status);
const char *health_check_severity_string(
    enum health_check_severity severity);
const char *health_check_result_string(
    enum health_check_result_state result);

#endif
