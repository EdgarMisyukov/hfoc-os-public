#ifndef HFOC_MANAGEMENT_COMMAND_H
#define HFOC_MANAGEMENT_COMMAND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MANAGEMENT_COMMAND_VERSION 1U
#define MANAGEMENT_COMMAND_ID_MIN 1U
#define MANAGEMENT_COMMAND_ID_MAX 255U
#define MANAGEMENT_COMMAND_PAYLOAD_MAX 255U
#define MANAGEMENT_COMMAND_REGISTRY_CAPACITY_MAX 16U
#define MANAGEMENT_COMMAND_HEADER_LENGTH 8U

#define MANAGEMENT_COMMAND_DIRECTION_REQUEST 0U
#define MANAGEMENT_COMMAND_DIRECTION_RESPONSE 1U
#define MANAGEMENT_COMMAND_DIRECTION_COUNT 2U

enum management_command_status {
    MANAGEMENT_COMMAND_OK = 0,
    MANAGEMENT_COMMAND_NULL = 1,
    MANAGEMENT_COMMAND_BAD_CAPACITY = 2,
    MANAGEMENT_COMMAND_BAD_VERSION = 3,
    MANAGEMENT_COMMAND_UNKNOWN_ID = 4,
    MANAGEMENT_COMMAND_BAD_DIRECTION = 5,
    MANAGEMENT_COMMAND_BAD_LENGTH = 6,
    MANAGEMENT_COMMAND_TRAILING_FIELDS = 7,
    MANAGEMENT_COMMAND_BAD_STATE = 8,
    MANAGEMENT_COMMAND_DUPLICATE = 9,
    MANAGEMENT_COMMAND_INVALID_DESCRIPTOR = 10,
};

struct management_command_header {
    uint32_t version;
    uint32_t command_id;
};

struct management_command_descriptor {
    uint32_t version;
    uint32_t command_id;
    uint16_t request_min_length;
    uint16_t request_max_length;
    uint16_t response_min_length;
    uint16_t response_max_length;
};

struct management_command_registry {
    uint32_t version;
    size_t count;
    bool initialized;
    struct management_command_descriptor descriptors[
        MANAGEMENT_COMMAND_REGISTRY_CAPACITY_MAX];
};

enum management_command_status management_command_registry_init(
    struct management_command_registry *registry,
    const struct management_command_descriptor *descriptors,
    size_t count);
enum management_command_status management_command_validate_frame(
    const struct management_command_registry *registry,
    uint8_t direction,
    const struct management_command_header *header,
    const void *payload,
    size_t payload_length,
    size_t frame_length);
const char *management_command_status_string(
    enum management_command_status status);

#endif
