#ifndef HFOC_INTEGRATION_SESSION_H
#define HFOC_INTEGRATION_SESSION_H

#include "integration_boundary.h"

#include <stdint.h>

#define INTEGRATION_SESSION_VERSION 1U
#define INTEGRATION_SESSION_SCHEMA_VERSION_MIN \
    INTEGRATION_BOUNDARY_SCHEMA_VERSION_MIN
#define INTEGRATION_SESSION_SCHEMA_VERSION_MAX \
    INTEGRATION_BOUNDARY_SCHEMA_VERSION_MAX
#define INTEGRATION_SESSION_TRANSPORT_VERSION \
    INTEGRATION_BOUNDARY_TRANSPORT_VERSION
#define INTEGRATION_SESSION_PEER_ID_MIN 1U
#define INTEGRATION_SESSION_PEER_ID_MAX 65535U
#define INTEGRATION_SESSION_NONCE_SIZE 32U
#define INTEGRATION_SESSION_NONCE_REGISTRY_MAX 64U

enum integration_session_status {
    INTEGRATION_SESSION_OK = 0,
    INTEGRATION_SESSION_NULL = 1,
    INTEGRATION_SESSION_BAD_CAPACITY = 2,
    INTEGRATION_SESSION_BAD_STATE = 3,
    INTEGRATION_SESSION_UNSUPPORTED_SCHEMA_VERSION = 4,
    INTEGRATION_SESSION_UNSUPPORTED_TRANSPORT_VERSION = 5,
    INTEGRATION_SESSION_INVALID_PEER_ID = 6,
    INTEGRATION_SESSION_INVALID_NONCE = 7,
    INTEGRATION_SESSION_REPLAY = 8,
    INTEGRATION_SESSION_REGISTRY_FULL = 9,
};

struct integration_session_nonce {
    uint32_t peer_id;
    uint8_t nonce[INTEGRATION_SESSION_NONCE_SIZE];
};

struct integration_session_nonce_registry {
    uint32_t version;
    uint32_t capacity;
    uint32_t count;
    struct integration_session_nonce
        entries[INTEGRATION_SESSION_NONCE_REGISTRY_MAX];
};

struct integration_session_request {
    uint32_t schema_version;
    uint32_t transport_version;
    uint32_t peer_id;
    uint8_t nonce[INTEGRATION_SESSION_NONCE_SIZE];
};

enum integration_session_status integration_session_nonce_registry_init(
    struct integration_session_nonce_registry *registry,
    uint32_t capacity);
enum integration_session_status integration_session_handshake_validate(
    const struct integration_session_request *request,
    struct integration_session_nonce_registry *registry);
const char *integration_session_status_string(
    enum integration_session_status status);

#endif
