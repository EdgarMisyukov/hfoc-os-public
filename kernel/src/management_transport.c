#include "management_transport.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool management_transport_policy_valid(
    const struct management_transport_policy *policy)
{
    return policy != NULL &&
           policy->version == MANAGEMENT_TRANSPORT_VERSION &&
           policy->port >= MANAGEMENT_TRANSPORT_PORT_MIN &&
           policy->payload_max <= MANAGEMENT_TRANSPORT_PAYLOAD_MAX;
}

static uint32_t management_transport_read_u32(
    const uint8_t *data)
{
    return ((uint32_t)data[0] << 0U) |
           ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) |
           ((uint32_t)data[3] << 24U);
}

static uint16_t management_transport_read_u16(
    const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] |
                      ((uint16_t)data[1] << 8U));
}

enum management_transport_status management_transport_validate_frame(
    const struct management_transport_policy *policy,
    const void *frame,
    size_t frame_length,
    const void **payload,
    size_t *payload_length)
{
    const uint8_t *bytes;
    uint32_t version;
    uint16_t port;
    uint16_t declared_payload_length;
    uint32_t flags;
    uint32_t direction;
    size_t required_frame_length;

    if (policy == NULL || frame == NULL ||
        payload == NULL || payload_length == NULL) {
        return MANAGEMENT_TRANSPORT_NULL;
    }
    if (!management_transport_policy_valid(policy)) {
        return MANAGEMENT_TRANSPORT_BAD_POLICY;
    }
    if (frame_length < MANAGEMENT_TRANSPORT_HEADER_SIZE) {
        return MANAGEMENT_TRANSPORT_PARTIAL;
    }

    bytes = (const uint8_t *)frame;
    version = management_transport_read_u32(bytes);
    port = management_transport_read_u16(bytes + 4U);
    declared_payload_length =
        management_transport_read_u16(bytes + 6U);
    flags = management_transport_read_u32(bytes + 8U);
    direction = flags & MANAGEMENT_TRANSPORT_FLAG_DIRECTION_MASK;

    if (version != MANAGEMENT_TRANSPORT_VERSION) {
        return MANAGEMENT_TRANSPORT_BAD_VERSION;
    }
    if (port != policy->port) {
        return MANAGEMENT_TRANSPORT_BAD_PORT;
    }
    if (direction >= MANAGEMENT_TRANSPORT_DIRECTION_COUNT) {
        return MANAGEMENT_TRANSPORT_BAD_DIRECTION;
    }
    if ((flags & MANAGEMENT_TRANSPORT_FLAG_RESERVED_MASK) != 0U) {
        return MANAGEMENT_TRANSPORT_BAD_FLAGS;
    }
    if ((uint32_t)declared_payload_length >
            (uint32_t)policy->payload_max ||
        frame_length > MANAGEMENT_TRANSPORT_FRAME_MAX) {
        return MANAGEMENT_TRANSPORT_OVERSIZED;
    }

    required_frame_length =
        (size_t)MANAGEMENT_TRANSPORT_HEADER_SIZE +
        (size_t)declared_payload_length;
    if (frame_length < required_frame_length) {
        return MANAGEMENT_TRANSPORT_PARTIAL;
    }
    if (frame_length > required_frame_length) {
        return MANAGEMENT_TRANSPORT_TRAILING_FIELDS;
    }

    *payload = bytes + MANAGEMENT_TRANSPORT_HEADER_SIZE;
    *payload_length = (size_t)declared_payload_length;
    return MANAGEMENT_TRANSPORT_OK;
}

const char *management_transport_status_string(
    enum management_transport_status status)
{
    switch (status) {
    case MANAGEMENT_TRANSPORT_OK:
        return "MANAGEMENT_TRANSPORT_OK";
    case MANAGEMENT_TRANSPORT_NULL:
        return "MANAGEMENT_TRANSPORT_NULL";
    case MANAGEMENT_TRANSPORT_BAD_POLICY:
        return "MANAGEMENT_TRANSPORT_BAD_POLICY";
    case MANAGEMENT_TRANSPORT_BAD_VERSION:
        return "MANAGEMENT_TRANSPORT_BAD_VERSION";
    case MANAGEMENT_TRANSPORT_BAD_PORT:
        return "MANAGEMENT_TRANSPORT_BAD_PORT";
    case MANAGEMENT_TRANSPORT_BAD_DIRECTION:
        return "MANAGEMENT_TRANSPORT_BAD_DIRECTION";
    case MANAGEMENT_TRANSPORT_BAD_FLAGS:
        return "MANAGEMENT_TRANSPORT_BAD_FLAGS";
    case MANAGEMENT_TRANSPORT_OVERSIZED:
        return "MANAGEMENT_TRANSPORT_OVERSIZED";
    case MANAGEMENT_TRANSPORT_PARTIAL:
        return "MANAGEMENT_TRANSPORT_PARTIAL";
    case MANAGEMENT_TRANSPORT_TRAILING_FIELDS:
        return "MANAGEMENT_TRANSPORT_TRAILING_FIELDS";
    default:
        return NULL;
    }
}
