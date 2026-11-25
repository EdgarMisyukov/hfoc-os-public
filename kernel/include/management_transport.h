#ifndef HFOC_MANAGEMENT_TRANSPORT_H
#define HFOC_MANAGEMENT_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#define MANAGEMENT_TRANSPORT_VERSION 1U
#define MANAGEMENT_TRANSPORT_PORT_MIN 1U
#define MANAGEMENT_TRANSPORT_PORT_MAX 65535U
#define MANAGEMENT_TRANSPORT_HEADER_SIZE 12U
#define MANAGEMENT_TRANSPORT_PAYLOAD_MIN 0U
#define MANAGEMENT_TRANSPORT_PAYLOAD_MAX 2048U
#define MANAGEMENT_TRANSPORT_FRAME_MAX \
    (MANAGEMENT_TRANSPORT_HEADER_SIZE + MANAGEMENT_TRANSPORT_PAYLOAD_MAX)

#define MANAGEMENT_TRANSPORT_DIRECTION_REQUEST 0U
#define MANAGEMENT_TRANSPORT_DIRECTION_RESPONSE 1U
#define MANAGEMENT_TRANSPORT_DIRECTION_COUNT 2U

#define MANAGEMENT_TRANSPORT_FLAG_DIRECTION_MASK 0x00000003U
#define MANAGEMENT_TRANSPORT_FLAG_RESERVED_MASK 0xFFFFFFFCU

enum management_transport_status {
    MANAGEMENT_TRANSPORT_OK = 0,
    MANAGEMENT_TRANSPORT_NULL = 1,
    MANAGEMENT_TRANSPORT_BAD_POLICY = 2,
    MANAGEMENT_TRANSPORT_BAD_VERSION = 3,
    MANAGEMENT_TRANSPORT_BAD_PORT = 4,
    MANAGEMENT_TRANSPORT_BAD_DIRECTION = 5,
    MANAGEMENT_TRANSPORT_BAD_FLAGS = 6,
    MANAGEMENT_TRANSPORT_OVERSIZED = 7,
    MANAGEMENT_TRANSPORT_PARTIAL = 8,
    MANAGEMENT_TRANSPORT_TRAILING_FIELDS = 9,
};

struct management_transport_header {
    uint32_t version;
    uint16_t port;
    uint16_t payload_length;
    uint32_t flags;
};

struct management_transport_policy {
    uint32_t version;
    uint16_t port;
    uint16_t payload_max;
};

enum management_transport_status management_transport_validate_frame(
    const struct management_transport_policy *policy,
    const void *frame,
    size_t frame_length,
    const void **payload,
    size_t *payload_length);
const char *management_transport_status_string(
    enum management_transport_status status);

#endif
