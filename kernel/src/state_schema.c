#include "state_schema.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void state_schema_zero_bytes(unsigned char *bytes, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void state_schema_copy_bytes(unsigned char *destination,
                                    const unsigned char *source,
                                    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        destination[index] = source[index];
    }
}

static bool state_schema_bytes_equal(const unsigned char *left,
                                     const unsigned char *right,
                                     size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

static bool state_schema_id_char_valid(unsigned char value)
{
    return (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
           (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
           (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
           value == (unsigned char)'_';
}

static bool state_schema_id_valid(const unsigned char *id,
                                  size_t id_length)
{
    size_t index;

    if (id == NULL ||
        id_length == 0U ||
        id_length > STATE_SCHEMA_ID_MAX) {
        return false;
    }
    for (index = 0U; index < id_length; ++index) {
        if (!state_schema_id_char_valid(id[index])) {
            return false;
        }
    }
    return true;
}

static enum state_schema_status state_schema_descriptor_status(
    const struct state_schema_descriptor *descriptor)
{
    if (descriptor->kind >= STATE_SCHEMA_KIND_COUNT) {
        return STATE_SCHEMA_BAD_KIND;
    }
    if (!state_schema_id_valid(descriptor->id,
                               descriptor->id_length)) {
        return STATE_SCHEMA_BAD_ID;
    }
    if (descriptor->kind == STATE_SCHEMA_KIND_JOURNAL) {
        if (descriptor->target_kind != STATE_SCHEMA_TARGET_JOURNAL) {
            return STATE_SCHEMA_BAD_KIND;
        }
    } else if (descriptor->target_kind >= CONFIG_KV_KIND_COUNT) {
        return STATE_SCHEMA_BAD_KIND;
    }
    if (descriptor->version == 0U ||
        descriptor->minimum_version == 0U ||
        descriptor->minimum_version > descriptor->maximum_version ||
        descriptor->version < descriptor->minimum_version ||
        descriptor->version > descriptor->maximum_version) {
        return STATE_SCHEMA_BAD_VERSION;
    }
    if (descriptor->minimum_length > descriptor->maximum_length ||
        descriptor->maximum_length > STATE_SCHEMA_RECORD_LENGTH_MAX) {
        return STATE_SCHEMA_BAD_LENGTH;
    }
    return STATE_SCHEMA_OK;
}

static const struct state_schema_descriptor *state_schema_find(
    const struct state_schema_registry *registry,
    uint8_t kind,
    const unsigned char *id,
    size_t id_length)
{
    size_t index;

    for (index = 0U; index < registry->count; ++index) {
        const struct state_schema_descriptor *descriptor =
            &registry->descriptors[index];

        if (descriptor->kind == kind &&
            descriptor->id_length == id_length &&
            state_schema_bytes_equal(descriptor->id, id, id_length)) {
            return descriptor;
        }
    }
    return NULL;
}

enum state_schema_status state_schema_registry_init(
    struct state_schema_registry *registry,
    const struct state_schema_descriptor *descriptors,
    size_t count)
{
    enum state_schema_status status;
    size_t index;
    size_t previous;

    if (registry == NULL ||
        (count > 0U && descriptors == NULL)) {
        return STATE_SCHEMA_NULL;
    }
    if (count == 0U ||
        count > STATE_SCHEMA_REGISTRY_CAPACITY_MAX) {
        return STATE_SCHEMA_BAD_CAPACITY;
    }

    for (index = 0U; index < count; ++index) {
        status = state_schema_descriptor_status(&descriptors[index]);
        if (status != STATE_SCHEMA_OK) {
            return status;
        }
    }
    for (index = 0U; index < count; ++index) {
        for (previous = 0U; previous < index; ++previous) {
            if (descriptors[index].kind == descriptors[previous].kind &&
                descriptors[index].id_length ==
                    descriptors[previous].id_length &&
                state_schema_bytes_equal(
                    descriptors[index].id,
                    descriptors[previous].id,
                    descriptors[index].id_length)) {
                return STATE_SCHEMA_DUPLICATE;
            }
        }
    }

    state_schema_zero_bytes((unsigned char *)registry,
                            sizeof(*registry));
    for (index = 0U; index < count; ++index) {
        state_schema_copy_bytes(
            (unsigned char *)&registry->descriptors[index],
            (const unsigned char *)&descriptors[index],
            sizeof(descriptors[index]));
    }
    registry->count = count;
    registry->initialized = true;
    return STATE_SCHEMA_OK;
}

enum state_schema_status state_schema_validate_record(
    const struct state_schema_registry *registry,
    uint8_t kind,
    uint8_t target_kind,
    const void *id,
    size_t id_length,
    uint32_t version,
    size_t record_length)
{
    const struct state_schema_descriptor *descriptor;

    if (registry == NULL || id == NULL) {
        return STATE_SCHEMA_NULL;
    }
    if (!registry->initialized ||
        registry->count == 0U ||
        registry->count > STATE_SCHEMA_REGISTRY_CAPACITY_MAX) {
        return STATE_SCHEMA_BAD_STATE;
    }
    if (kind >= STATE_SCHEMA_KIND_COUNT) {
        return STATE_SCHEMA_BAD_KIND;
    }
    if (kind == STATE_SCHEMA_KIND_JOURNAL) {
        if (target_kind != STATE_SCHEMA_TARGET_JOURNAL) {
            return STATE_SCHEMA_BAD_KIND;
        }
    } else if (target_kind >= CONFIG_KV_KIND_COUNT) {
        return STATE_SCHEMA_BAD_KIND;
    }
    if (!state_schema_id_valid((const unsigned char *)id, id_length)) {
        return STATE_SCHEMA_BAD_ID;
    }
    if (version == 0U) {
        return STATE_SCHEMA_BAD_VERSION;
    }

    descriptor = state_schema_find(
        registry, kind, (const unsigned char *)id, id_length);
    if (descriptor == NULL) {
        return STATE_SCHEMA_UNKNOWN;
    }
    if (descriptor->target_kind != target_kind) {
        return STATE_SCHEMA_BAD_KIND;
    }
    if (version < descriptor->minimum_version ||
        version > descriptor->maximum_version) {
        return STATE_SCHEMA_UNSUPPORTED_VERSION;
    }
    if (record_length < descriptor->minimum_length) {
        return STATE_SCHEMA_RECORD_TOO_SHORT;
    }
    if (record_length > descriptor->maximum_length) {
        return STATE_SCHEMA_RECORD_TOO_LONG;
    }
    return STATE_SCHEMA_OK;
}

enum state_schema_status state_schema_append_journal(
    struct config_journal *journal,
    const struct state_schema_registry *registry,
    const void *id,
    size_t id_length,
    uint32_t version,
    const void *data,
    size_t data_length)
{
    enum state_schema_status status;

    status = state_schema_validate_record(
        registry,
        STATE_SCHEMA_KIND_JOURNAL,
        STATE_SCHEMA_TARGET_JOURNAL,
        id, id_length, version, data_length);
    if (status != STATE_SCHEMA_OK) {
        return status;
    }
    if (config_journal_append(journal, data, data_length) !=
        CONFIG_JOURNAL_OK) {
        return STATE_SCHEMA_PUBLISH_FAILED;
    }
    return STATE_SCHEMA_OK;
}

enum state_schema_status state_schema_kv_put(
    struct config_kv *kv,
    const struct state_schema_registry *registry,
    const void *id,
    size_t id_length,
    uint32_t version,
    uint8_t kind,
    const void *key,
    size_t key_length,
    const void *value,
    size_t value_length,
    bool *replaced)
{
    enum state_schema_status status;

    status = state_schema_validate_record(
        registry,
        STATE_SCHEMA_KIND_KV,
        kind,
        id, id_length, version, value_length);
    if (status != STATE_SCHEMA_OK) {
        return status;
    }
    if (config_kv_put(kv, kind, key, key_length, value, value_length,
                      replaced) != CONFIG_KV_OK) {
        return STATE_SCHEMA_PUBLISH_FAILED;
    }
    return STATE_SCHEMA_OK;
}

const char *state_schema_status_string(enum state_schema_status status)
{
    switch (status) {
    case STATE_SCHEMA_OK:
        return "STATE_SCHEMA_OK";
    case STATE_SCHEMA_NULL:
        return "STATE_SCHEMA_NULL";
    case STATE_SCHEMA_BAD_KIND:
        return "STATE_SCHEMA_BAD_KIND";
    case STATE_SCHEMA_BAD_ID:
        return "STATE_SCHEMA_BAD_ID";
    case STATE_SCHEMA_BAD_VERSION:
        return "STATE_SCHEMA_BAD_VERSION";
    case STATE_SCHEMA_BAD_LENGTH:
        return "STATE_SCHEMA_BAD_LENGTH";
    case STATE_SCHEMA_BAD_CAPACITY:
        return "STATE_SCHEMA_BAD_CAPACITY";
    case STATE_SCHEMA_DUPLICATE:
        return "STATE_SCHEMA_DUPLICATE";
    case STATE_SCHEMA_BAD_STATE:
        return "STATE_SCHEMA_BAD_STATE";
    case STATE_SCHEMA_UNKNOWN:
        return "STATE_SCHEMA_UNKNOWN";
    case STATE_SCHEMA_UNSUPPORTED_VERSION:
        return "STATE_SCHEMA_UNSUPPORTED_VERSION";
    case STATE_SCHEMA_RECORD_TOO_SHORT:
        return "STATE_SCHEMA_RECORD_TOO_SHORT";
    case STATE_SCHEMA_RECORD_TOO_LONG:
        return "STATE_SCHEMA_RECORD_TOO_LONG";
    case STATE_SCHEMA_PUBLISH_FAILED:
        return "STATE_SCHEMA_PUBLISH_FAILED";
    default:
        return "STATE_SCHEMA_UNKNOWN";
    }
}
