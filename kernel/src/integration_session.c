#include "integration_session.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static int integration_session_nonce_all_zero(
    const uint8_t *nonce)
{
    uint32_t index;

    for (index = 0U; index < INTEGRATION_SESSION_NONCE_SIZE; ++index) {
        if (nonce[index] != 0U) {
            return 0;
        }
    }
    return 1;
}

enum integration_session_status integration_session_nonce_registry_init(
    struct integration_session_nonce_registry *registry,
    uint32_t capacity)
{
    if (registry == NULL) {
        return INTEGRATION_SESSION_NULL;
    }
    if (capacity == 0U ||
        capacity > INTEGRATION_SESSION_NONCE_REGISTRY_MAX) {
        return INTEGRATION_SESSION_BAD_CAPACITY;
    }

    memset(registry, 0, sizeof(*registry));
    registry->version = INTEGRATION_SESSION_VERSION;
    registry->capacity = capacity;
    return INTEGRATION_SESSION_OK;
}

enum integration_session_status integration_session_handshake_validate(
    const struct integration_session_request *request,
    struct integration_session_nonce_registry *registry)
{
    uint32_t index;

    if (request == NULL || registry == NULL) {
        return INTEGRATION_SESSION_NULL;
    }
    if (registry->capacity == 0U ||
        registry->capacity > INTEGRATION_SESSION_NONCE_REGISTRY_MAX) {
        return INTEGRATION_SESSION_BAD_CAPACITY;
    }
    if (registry->version != INTEGRATION_SESSION_VERSION ||
        registry->count > registry->capacity) {
        return INTEGRATION_SESSION_BAD_STATE;
    }
    if (request->schema_version <
            INTEGRATION_SESSION_SCHEMA_VERSION_MIN ||
        request->schema_version >
            INTEGRATION_SESSION_SCHEMA_VERSION_MAX) {
        return INTEGRATION_SESSION_UNSUPPORTED_SCHEMA_VERSION;
    }
    if (request->transport_version !=
        INTEGRATION_SESSION_TRANSPORT_VERSION) {
        return INTEGRATION_SESSION_UNSUPPORTED_TRANSPORT_VERSION;
    }
    if (request->peer_id < INTEGRATION_SESSION_PEER_ID_MIN ||
        request->peer_id > INTEGRATION_SESSION_PEER_ID_MAX) {
        return INTEGRATION_SESSION_INVALID_PEER_ID;
    }
    if (integration_session_nonce_all_zero(request->nonce)) {
        return INTEGRATION_SESSION_INVALID_NONCE;
    }

    for (index = 0U; index < registry->count; ++index) {
        if (registry->entries[index].peer_id == request->peer_id &&
            memcmp(registry->entries[index].nonce,
                   request->nonce,
                   INTEGRATION_SESSION_NONCE_SIZE) == 0) {
            return INTEGRATION_SESSION_REPLAY;
        }
    }
    if (registry->count >= registry->capacity) {
        return INTEGRATION_SESSION_REGISTRY_FULL;
    }

    registry->entries[registry->count].peer_id = request->peer_id;
    memcpy(registry->entries[registry->count].nonce,
           request->nonce,
           INTEGRATION_SESSION_NONCE_SIZE);
    ++registry->count;
    return INTEGRATION_SESSION_OK;
}

const char *integration_session_status_string(
    enum integration_session_status status)
{
    switch (status) {
    case INTEGRATION_SESSION_OK:
        return "INTEGRATION_SESSION_OK";
    case INTEGRATION_SESSION_NULL:
        return "INTEGRATION_SESSION_NULL";
    case INTEGRATION_SESSION_BAD_CAPACITY:
        return "INTEGRATION_SESSION_BAD_CAPACITY";
    case INTEGRATION_SESSION_BAD_STATE:
        return "INTEGRATION_SESSION_BAD_STATE";
    case INTEGRATION_SESSION_UNSUPPORTED_SCHEMA_VERSION:
        return "INTEGRATION_SESSION_UNSUPPORTED_SCHEMA_VERSION";
    case INTEGRATION_SESSION_UNSUPPORTED_TRANSPORT_VERSION:
        return "INTEGRATION_SESSION_UNSUPPORTED_TRANSPORT_VERSION";
    case INTEGRATION_SESSION_INVALID_PEER_ID:
        return "INTEGRATION_SESSION_INVALID_PEER_ID";
    case INTEGRATION_SESSION_INVALID_NONCE:
        return "INTEGRATION_SESSION_INVALID_NONCE";
    case INTEGRATION_SESSION_REPLAY:
        return "INTEGRATION_SESSION_REPLAY";
    case INTEGRATION_SESSION_REGISTRY_FULL:
        return "INTEGRATION_SESSION_REGISTRY_FULL";
    default:
        return NULL;
    }
}
