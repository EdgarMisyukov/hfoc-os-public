#include "network_admission.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static uint32_t network_admission_read_u32(
    const uint8_t *data)
{
    return ((uint32_t)data[0] << 0U) |
           ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) |
           ((uint32_t)data[3] << 24U);
}

static bool network_admission_set_valid(
    const uint8_t *items,
    uint32_t count,
    uint32_t maximum)
{
    uint32_t index;
    uint32_t previous;

    if (count > maximum) {
        return false;
    }
    if (count > 0U && items == NULL) {
        return false;
    }
    for (index = 0U; index < count; ++index) {
        if (items[index] == 0U) {
            return false;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (items[index] == items[previous]) {
                return false;
            }
        }
    }
    return true;
}

enum network_admission_status network_admission_policy_validate(
    const struct network_admission_policy *policy)
{
    if (policy == NULL) {
        return NETWORK_ADMISSION_NULL;
    }
    if (policy->version != NETWORK_ADMISSION_VERSION) {
        return NETWORK_ADMISSION_BAD_VERSION;
    }
    if (policy->mode != NETWORK_ADMISSION_MODE_DENY_ALL &&
        policy->mode != NETWORK_ADMISSION_MODE_ROLE_SESSION) {
        return NETWORK_ADMISSION_BAD_MODE;
    }
    if (policy->port < NETWORK_ADMISSION_PORT_MIN) {
        return NETWORK_ADMISSION_BAD_PORT;
    }
    if (policy->payload_max > NETWORK_ADMISSION_PAYLOAD_MAX_MAX) {
        return NETWORK_ADMISSION_BAD_PAYLOAD_MAX;
    }
    return NETWORK_ADMISSION_OK;
}

enum network_admission_status network_admission_peer_validate(
    const struct network_admission_peer *peer)
{
    if (peer == NULL) {
        return NETWORK_ADMISSION_NULL;
    }
    if (peer->version != NETWORK_ADMISSION_VERSION ||
        peer->session_id < NETWORK_ADMISSION_PEER_SESSION_ID_MIN ||
        peer->session_id > NETWORK_ADMISSION_PEER_SESSION_ID_MAX ||
        peer->principal_id < NETWORK_ADMISSION_PEER_PRINCIPAL_ID_MIN ||
        peer->principal_id > NETWORK_ADMISSION_PEER_PRINCIPAL_ID_MAX ||
        peer->generation < NETWORK_ADMISSION_PEER_GENERATION_MIN ||
        peer->generation > NETWORK_ADMISSION_PEER_GENERATION_MAX ||
        !network_admission_set_valid(
            peer->roles, peer->role_count,
            NETWORK_ADMISSION_ROLE_SET_MAX) ||
        !network_admission_set_valid(
            peer->capabilities, peer->capability_count,
            NETWORK_ADMISSION_CAPABILITY_SET_MAX)) {
        return NETWORK_ADMISSION_INVALID_PEER;
    }
    return NETWORK_ADMISSION_OK;
}

enum network_admission_status network_admission_selection_init(
    struct network_admission_selection *selection,
    const struct network_admission_policy *policy)
{
    enum network_admission_status status;

    if (selection == NULL || policy == NULL) {
        return NETWORK_ADMISSION_NULL;
    }
    if (selection->published) {
        return NETWORK_ADMISSION_ALREADY_PUBLISHED;
    }
    status = network_admission_policy_validate(policy);
    if (status != NETWORK_ADMISSION_OK) {
        return status;
    }

    selection->published = true;
    selection->version = NETWORK_ADMISSION_VERSION;
    selection->policy = *policy;
    return NETWORK_ADMISSION_OK;
}

bool network_admission_selection_published(
    const struct network_admission_selection *selection)
{
    return selection != NULL &&
           selection->published &&
           selection->version == NETWORK_ADMISSION_VERSION &&
           network_admission_policy_validate(&selection->policy) ==
               NETWORK_ADMISSION_OK;
}

enum network_admission_status network_admission_check_request(
    const struct network_admission_selection *selection,
    const struct network_admission_peer *peer,
    const struct management_command_registry *registry,
    const struct authorization_matrix *matrix,
    const void *frame,
    size_t frame_length)
{
    const uint8_t *bytes;
    const void *transport_payload;
    size_t transport_payload_length = 0U;
    struct management_transport_policy transport_policy;
    struct management_command_header header;
    enum network_admission_status status;
    enum management_transport_status transport_status;
    enum management_command_status command_status;
    enum authorization_status authorization_status;
    uint32_t direction;
    uint32_t flags;

    if (selection == NULL || peer == NULL ||
        registry == NULL || matrix == NULL || frame == NULL) {
        return NETWORK_ADMISSION_NULL;
    }
    if (!selection->published) {
        return NETWORK_ADMISSION_UNPUBLISHED;
    }
    if (selection->version != NETWORK_ADMISSION_VERSION) {
        return NETWORK_ADMISSION_BAD_VERSION;
    }
    status = network_admission_policy_validate(&selection->policy);
    if (status != NETWORK_ADMISSION_OK) {
        return status;
    }
    if (selection->policy.mode == NETWORK_ADMISSION_MODE_DENY_ALL) {
        return NETWORK_ADMISSION_DENIED_BY_DEFAULT;
    }

    status = network_admission_peer_validate(peer);
    if (status != NETWORK_ADMISSION_OK) {
        return status;
    }
    if (!peer->authenticated) {
        return NETWORK_ADMISSION_UNAUTHENTICATED;
    }

    transport_policy.version = MANAGEMENT_TRANSPORT_VERSION;
    transport_policy.port = selection->policy.port;
    transport_policy.payload_max = selection->policy.payload_max;
    transport_status = management_transport_validate_frame(
        &transport_policy, frame, frame_length,
        &transport_payload, &transport_payload_length);
    if (transport_status != MANAGEMENT_TRANSPORT_OK) {
        return NETWORK_ADMISSION_BAD_FRAME;
    }

    bytes = (const uint8_t *)frame;
    flags = network_admission_read_u32(bytes + 8U);
    direction = flags & MANAGEMENT_TRANSPORT_FLAG_DIRECTION_MASK;
    if (direction != MANAGEMENT_TRANSPORT_DIRECTION_REQUEST) {
        return NETWORK_ADMISSION_BAD_FRAME;
    }
    if (transport_payload_length < MANAGEMENT_COMMAND_HEADER_LENGTH) {
        return NETWORK_ADMISSION_BAD_COMMAND;
    }

    header.version = network_admission_read_u32(
        (const uint8_t *)transport_payload);
    header.command_id = network_admission_read_u32(
        (const uint8_t *)transport_payload + 4U);
    command_status = management_command_validate_frame(
        registry,
        MANAGEMENT_COMMAND_DIRECTION_REQUEST,
        &header,
        (const uint8_t *)transport_payload +
            MANAGEMENT_COMMAND_HEADER_LENGTH,
        transport_payload_length - MANAGEMENT_COMMAND_HEADER_LENGTH,
        transport_payload_length);
    if (command_status != MANAGEMENT_COMMAND_OK) {
        return NETWORK_ADMISSION_BAD_COMMAND;
    }

    authorization_status = authorization_check(
        matrix, header.command_id,
        peer->roles, peer->role_count,
        peer->capabilities, peer->capability_count);
    if (authorization_status == AUTHORIZATION_OK) {
        return NETWORK_ADMISSION_OK;
    }
    if (authorization_status == AUTHORIZATION_MISSING_ROLE) {
        return NETWORK_ADMISSION_DENIED_ROLE;
    }
    if (authorization_status == AUTHORIZATION_MISSING_CAPABILITY) {
        return NETWORK_ADMISSION_DENIED_CAPABILITY;
    }
    return NETWORK_ADMISSION_BAD_STATE;
}

const char *network_admission_status_string(
    enum network_admission_status status)
{
    switch (status) {
    case NETWORK_ADMISSION_OK:
        return "NETWORK_ADMISSION_OK";
    case NETWORK_ADMISSION_NULL:
        return "NETWORK_ADMISSION_NULL";
    case NETWORK_ADMISSION_BAD_STATE:
        return "NETWORK_ADMISSION_BAD_STATE";
    case NETWORK_ADMISSION_BAD_VERSION:
        return "NETWORK_ADMISSION_BAD_VERSION";
    case NETWORK_ADMISSION_BAD_MODE:
        return "NETWORK_ADMISSION_BAD_MODE";
    case NETWORK_ADMISSION_BAD_PORT:
        return "NETWORK_ADMISSION_BAD_PORT";
    case NETWORK_ADMISSION_BAD_PAYLOAD_MAX:
        return "NETWORK_ADMISSION_BAD_PAYLOAD_MAX";
    case NETWORK_ADMISSION_ALREADY_PUBLISHED:
        return "NETWORK_ADMISSION_ALREADY_PUBLISHED";
    case NETWORK_ADMISSION_UNPUBLISHED:
        return "NETWORK_ADMISSION_UNPUBLISHED";
    case NETWORK_ADMISSION_INVALID_PEER:
        return "NETWORK_ADMISSION_INVALID_PEER";
    case NETWORK_ADMISSION_UNAUTHENTICATED:
        return "NETWORK_ADMISSION_UNAUTHENTICATED";
    case NETWORK_ADMISSION_DENIED_BY_DEFAULT:
        return "NETWORK_ADMISSION_DENIED_BY_DEFAULT";
    case NETWORK_ADMISSION_BAD_FRAME:
        return "NETWORK_ADMISSION_BAD_FRAME";
    case NETWORK_ADMISSION_BAD_COMMAND:
        return "NETWORK_ADMISSION_BAD_COMMAND";
    case NETWORK_ADMISSION_DENIED_ROLE:
        return "NETWORK_ADMISSION_DENIED_ROLE";
    case NETWORK_ADMISSION_DENIED_CAPABILITY:
        return "NETWORK_ADMISSION_DENIED_CAPABILITY";
    default:
        return NULL;
    }
}
