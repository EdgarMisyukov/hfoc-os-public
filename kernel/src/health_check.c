#include "health_check.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void health_check_zero_bytes(char *destination, size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = '\0';
    }
}

static bool health_check_severity_valid(
    enum health_check_severity severity)
{
    return severity >= HEALTH_CHECK_SEVERITY_INFO &&
           severity <= HEALTH_CHECK_SEVERITY_CRITICAL;
}

static bool health_check_result_valid(
    enum health_check_result_state result)
{
    return result >= HEALTH_CHECK_RESULT_UNKNOWN &&
           result <= HEALTH_CHECK_RESULT_FAIL;
}

static size_t health_check_find(
    const struct health_check_store *store,
    uint32_t check_id)
{
    size_t index;

    for (index = 0U;
         index < store->descriptor_count;
         ++index) {
        if (store->entries[index].descriptor.check_id == check_id) {
            return index;
        }
    }
    return (size_t)-1;
}

enum health_check_status health_check_init(
    struct health_check_store *store)
{
    if (store == NULL) {
        return HEALTH_CHECK_NULL;
    }
    if (store->initialized) {
        return HEALTH_CHECK_BAD_STATE;
    }

    health_check_zero_bytes((char *)store, sizeof(*store));
    store->version = HEALTH_CHECK_VERSION;
    store->initialized = true;
    return HEALTH_CHECK_OK;
}

enum health_check_status health_check_publish(
    struct health_check_store *store,
    const struct health_check_descriptor *descriptors,
    size_t descriptor_count)
{
    size_t index;
    size_t previous;

    if (store == NULL || descriptors == NULL) {
        return HEALTH_CHECK_NULL;
    }
    if (!store->initialized) {
        return HEALTH_CHECK_BAD_STATE;
    }
    if (store->published) {
        return HEALTH_CHECK_ALREADY_PUBLISHED;
    }
    if (descriptor_count == 0U ||
        descriptor_count > HEALTH_CHECK_MAX_DESCRIPTORS) {
        return HEALTH_CHECK_BAD_COUNT;
    }

    for (index = 0U; index < descriptor_count; ++index) {
        const struct health_check_descriptor *descriptor =
            &descriptors[index];

        if (descriptor->version != HEALTH_CHECK_VERSION) {
            return HEALTH_CHECK_BAD_VERSION;
        }
        if (descriptor->check_id < HEALTH_CHECK_ID_MIN ||
            descriptor->check_id > HEALTH_CHECK_ID_MAX) {
            return HEALTH_CHECK_BAD_ID;
        }
        if (!health_check_severity_valid(descriptor->severity)) {
            return HEALTH_CHECK_BAD_SEVERITY;
        }
        if (descriptor->cadence_ms < HEALTH_CHECK_CADENCE_MIN_MS ||
            descriptor->cadence_ms > HEALTH_CHECK_CADENCE_MAX_MS) {
            return HEALTH_CHECK_BAD_CADENCE;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (descriptors[previous].check_id ==
                descriptor->check_id) {
                return HEALTH_CHECK_DUPLICATE;
            }
        }
    }

    for (index = 0U;
         index < HEALTH_CHECK_MAX_DESCRIPTORS;
         ++index) {
        health_check_zero_bytes(
            (char *)&store->entries[index],
            sizeof(store->entries[index]));
    }
    for (index = 0U; index < descriptor_count; ++index) {
        store->entries[index].descriptor = descriptors[index];
        store->entries[index].result =
            HEALTH_CHECK_RESULT_UNKNOWN;
    }
    store->descriptor_count = descriptor_count;
    store->generation = 1U;
    store->published = true;
    return HEALTH_CHECK_OK;
}

enum health_check_status health_check_result(
    struct health_check_store *store,
    uint32_t check_id,
    enum health_check_result_state result)
{
    size_t index;

    if (store == NULL) {
        return HEALTH_CHECK_NULL;
    }
    if (!store->initialized) {
        return HEALTH_CHECK_BAD_STATE;
    }
    if (!store->published) {
        return HEALTH_CHECK_NOT_PUBLISHED;
    }
    if (!health_check_result_valid(result)) {
        return HEALTH_CHECK_BAD_RESULT;
    }

    index = health_check_find(store, check_id);
    if (index == (size_t)-1) {
        return HEALTH_CHECK_UNKNOWN;
    }
    store->entries[index].result = result;
    return HEALTH_CHECK_OK;
}

enum health_check_status health_check_lookup(
    const struct health_check_store *store,
    uint32_t check_id,
    struct health_check_descriptor *descriptor_out)
{
    size_t index;

    if (store == NULL || descriptor_out == NULL) {
        return HEALTH_CHECK_NULL;
    }
    if (!store->initialized) {
        return HEALTH_CHECK_BAD_STATE;
    }
    if (!store->published) {
        return HEALTH_CHECK_NOT_PUBLISHED;
    }

    index = health_check_find(store, check_id);
    if (index == (size_t)-1) {
        return HEALTH_CHECK_UNKNOWN;
    }
    *descriptor_out = store->entries[index].descriptor;
    return HEALTH_CHECK_OK;
}

enum health_check_status health_check_snapshot(
    const struct health_check_store *store,
    struct health_check_snapshot *snapshot_out)
{
    size_t index;

    if (store == NULL || snapshot_out == NULL) {
        return HEALTH_CHECK_NULL;
    }
    if (!store->initialized) {
        return HEALTH_CHECK_BAD_STATE;
    }
    if (!store->published) {
        return HEALTH_CHECK_NOT_PUBLISHED;
    }

    health_check_zero_bytes(
        (char *)snapshot_out, sizeof(*snapshot_out));
    snapshot_out->version = HEALTH_CHECK_VERSION;
    snapshot_out->generation = store->generation;
    snapshot_out->descriptor_count = store->descriptor_count;
    for (index = 0U;
         index < store->descriptor_count;
         ++index) {
        snapshot_out->entries[index] = store->entries[index];
    }
    return HEALTH_CHECK_OK;
}

const char *health_check_status_string(
    enum health_check_status status)
{
    switch (status) {
    case HEALTH_CHECK_OK:
        return "HEALTH_CHECK_OK";
    case HEALTH_CHECK_NULL:
        return "HEALTH_CHECK_NULL";
    case HEALTH_CHECK_BAD_STATE:
        return "HEALTH_CHECK_BAD_STATE";
    case HEALTH_CHECK_ALREADY_PUBLISHED:
        return "HEALTH_CHECK_ALREADY_PUBLISHED";
    case HEALTH_CHECK_BAD_VERSION:
        return "HEALTH_CHECK_BAD_VERSION";
    case HEALTH_CHECK_BAD_COUNT:
        return "HEALTH_CHECK_BAD_COUNT";
    case HEALTH_CHECK_BAD_ID:
        return "HEALTH_CHECK_BAD_ID";
    case HEALTH_CHECK_BAD_SEVERITY:
        return "HEALTH_CHECK_BAD_SEVERITY";
    case HEALTH_CHECK_BAD_CADENCE:
        return "HEALTH_CHECK_BAD_CADENCE";
    case HEALTH_CHECK_DUPLICATE:
        return "HEALTH_CHECK_DUPLICATE";
    case HEALTH_CHECK_NOT_PUBLISHED:
        return "HEALTH_CHECK_NOT_PUBLISHED";
    case HEALTH_CHECK_UNKNOWN:
        return "HEALTH_CHECK_UNKNOWN";
    case HEALTH_CHECK_BAD_RESULT:
        return "HEALTH_CHECK_BAD_RESULT";
    default:
        return NULL;
    }
}

const char *health_check_severity_string(
    enum health_check_severity severity)
{
    switch (severity) {
    case HEALTH_CHECK_SEVERITY_INFO:
        return "HEALTH_CHECK_SEVERITY_INFO";
    case HEALTH_CHECK_SEVERITY_WARNING:
        return "HEALTH_CHECK_SEVERITY_WARNING";
    case HEALTH_CHECK_SEVERITY_CRITICAL:
        return "HEALTH_CHECK_SEVERITY_CRITICAL";
    default:
        return NULL;
    }
}

const char *health_check_result_string(
    enum health_check_result_state result)
{
    switch (result) {
    case HEALTH_CHECK_RESULT_UNKNOWN:
        return "HEALTH_CHECK_RESULT_UNKNOWN";
    case HEALTH_CHECK_RESULT_PASS:
        return "HEALTH_CHECK_RESULT_PASS";
    case HEALTH_CHECK_RESULT_WARN:
        return "HEALTH_CHECK_RESULT_WARN";
    case HEALTH_CHECK_RESULT_FAIL:
        return "HEALTH_CHECK_RESULT_FAIL";
    default:
        return NULL;
    }
}
