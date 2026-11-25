#include "management_command.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static enum management_command_status
management_command_descriptor_valid(
    const struct management_command_descriptor *descriptor)
{
    if (descriptor->version != MANAGEMENT_COMMAND_VERSION) {
        return MANAGEMENT_COMMAND_BAD_VERSION;
    }
    if (descriptor->command_id < MANAGEMENT_COMMAND_ID_MIN ||
        descriptor->command_id > MANAGEMENT_COMMAND_ID_MAX) {
        return MANAGEMENT_COMMAND_INVALID_DESCRIPTOR;
    }
    if (descriptor->request_min_length >
            descriptor->request_max_length ||
        descriptor->request_max_length >
            MANAGEMENT_COMMAND_PAYLOAD_MAX ||
        descriptor->response_min_length >
            descriptor->response_max_length ||
        descriptor->response_max_length >
            MANAGEMENT_COMMAND_PAYLOAD_MAX) {
        return MANAGEMENT_COMMAND_INVALID_DESCRIPTOR;
    }
    return MANAGEMENT_COMMAND_OK;
}

static enum management_command_status
management_command_registry_valid(
    const struct management_command_registry *registry)
{
    enum management_command_status status;
    size_t index;

    if (registry == NULL) {
        return MANAGEMENT_COMMAND_NULL;
    }
    if (registry->version != MANAGEMENT_COMMAND_VERSION) {
        return MANAGEMENT_COMMAND_BAD_VERSION;
    }
    if (registry->count == 0U ||
        registry->count > MANAGEMENT_COMMAND_REGISTRY_CAPACITY_MAX ||
        !registry->initialized) {
        return MANAGEMENT_COMMAND_BAD_STATE;
    }
    for (index = 0U; index < registry->count; ++index) {
        status = management_command_descriptor_valid(
            &registry->descriptors[index]);
        if (status != MANAGEMENT_COMMAND_OK) {
            return MANAGEMENT_COMMAND_BAD_STATE;
        }
    }
    return MANAGEMENT_COMMAND_OK;
}

enum management_command_status management_command_registry_init(
    struct management_command_registry *registry,
    const struct management_command_descriptor *descriptors,
    size_t count)
{
    enum management_command_status status;
    size_t index;
    size_t previous;

    if (registry == NULL ||
        (count > 0U && descriptors == NULL)) {
        return MANAGEMENT_COMMAND_NULL;
    }
    if (count == 0U ||
        count > MANAGEMENT_COMMAND_REGISTRY_CAPACITY_MAX) {
        return MANAGEMENT_COMMAND_BAD_CAPACITY;
    }

    for (index = 0U; index < count; ++index) {
        status = management_command_descriptor_valid(&descriptors[index]);
        if (status != MANAGEMENT_COMMAND_OK) {
            return status;
        }
    }
    for (index = 0U; index < count; ++index) {
        for (previous = 0U; previous < index; ++previous) {
            if (descriptors[index].command_id ==
                descriptors[previous].command_id) {
                return MANAGEMENT_COMMAND_DUPLICATE;
            }
        }
    }

    memset(registry, 0, sizeof(*registry));
    for (index = 0U; index < count; ++index) {
        registry->descriptors[index] = descriptors[index];
    }
    registry->version = MANAGEMENT_COMMAND_VERSION;
    registry->count = count;
    registry->initialized = true;
    return MANAGEMENT_COMMAND_OK;
}

enum management_command_status management_command_validate_frame(
    const struct management_command_registry *registry,
    uint8_t direction,
    const struct management_command_header *header,
    const void *payload,
    size_t payload_length,
    size_t frame_length)
{
    const struct management_command_descriptor *descriptor = NULL;
    enum management_command_status status;
    uint16_t minimum_length;
    uint16_t maximum_length;
    size_t index;

    if (registry == NULL || header == NULL ||
        (payload_length > 0U && payload == NULL)) {
        return MANAGEMENT_COMMAND_NULL;
    }
    status = management_command_registry_valid(registry);
    if (status != MANAGEMENT_COMMAND_OK) {
        return status;
    }
    if (direction >= MANAGEMENT_COMMAND_DIRECTION_COUNT) {
        return MANAGEMENT_COMMAND_BAD_DIRECTION;
    }
    if (header->version != MANAGEMENT_COMMAND_VERSION) {
        return MANAGEMENT_COMMAND_BAD_VERSION;
    }
    if (header->command_id < MANAGEMENT_COMMAND_ID_MIN ||
        header->command_id > MANAGEMENT_COMMAND_ID_MAX) {
        return MANAGEMENT_COMMAND_UNKNOWN_ID;
    }

    for (index = 0U; index < registry->count; ++index) {
        if (registry->descriptors[index].command_id ==
            header->command_id) {
            descriptor = &registry->descriptors[index];
            break;
        }
    }
    if (descriptor == NULL) {
        return MANAGEMENT_COMMAND_UNKNOWN_ID;
    }

    if (direction == MANAGEMENT_COMMAND_DIRECTION_REQUEST) {
        minimum_length = descriptor->request_min_length;
        maximum_length = descriptor->request_max_length;
    } else {
        minimum_length = descriptor->response_min_length;
        maximum_length = descriptor->response_max_length;
    }
    if (payload_length < minimum_length ||
        payload_length > maximum_length) {
        return MANAGEMENT_COMMAND_BAD_LENGTH;
    }
    if (frame_length <
        MANAGEMENT_COMMAND_HEADER_LENGTH + payload_length) {
        return MANAGEMENT_COMMAND_BAD_LENGTH;
    }
    if (frame_length >
        MANAGEMENT_COMMAND_HEADER_LENGTH + payload_length) {
        return MANAGEMENT_COMMAND_TRAILING_FIELDS;
    }
    return MANAGEMENT_COMMAND_OK;
}

const char *management_command_status_string(
    enum management_command_status status)
{
    switch (status) {
    case MANAGEMENT_COMMAND_OK:
        return "MANAGEMENT_COMMAND_OK";
    case MANAGEMENT_COMMAND_NULL:
        return "MANAGEMENT_COMMAND_NULL";
    case MANAGEMENT_COMMAND_BAD_CAPACITY:
        return "MANAGEMENT_COMMAND_BAD_CAPACITY";
    case MANAGEMENT_COMMAND_BAD_VERSION:
        return "MANAGEMENT_COMMAND_BAD_VERSION";
    case MANAGEMENT_COMMAND_UNKNOWN_ID:
        return "MANAGEMENT_COMMAND_UNKNOWN_ID";
    case MANAGEMENT_COMMAND_BAD_DIRECTION:
        return "MANAGEMENT_COMMAND_BAD_DIRECTION";
    case MANAGEMENT_COMMAND_BAD_LENGTH:
        return "MANAGEMENT_COMMAND_BAD_LENGTH";
    case MANAGEMENT_COMMAND_TRAILING_FIELDS:
        return "MANAGEMENT_COMMAND_TRAILING_FIELDS";
    case MANAGEMENT_COMMAND_BAD_STATE:
        return "MANAGEMENT_COMMAND_BAD_STATE";
    case MANAGEMENT_COMMAND_DUPLICATE:
        return "MANAGEMENT_COMMAND_DUPLICATE";
    case MANAGEMENT_COMMAND_INVALID_DESCRIPTOR:
        return "MANAGEMENT_COMMAND_INVALID_DESCRIPTOR";
    default:
        return NULL;
    }
}
