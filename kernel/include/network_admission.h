#ifndef HFOC_NETWORK_ADMISSION_H
#define HFOC_NETWORK_ADMISSION_H

#include "authorization.h"
#include "management_command.h"
#include "management_transport.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETWORK_ADMISSION_VERSION 1U
#define NETWORK_ADMISSION_PORT_MIN 1U
#define NETWORK_ADMISSION_PORT_MAX 65535U
#define NETWORK_ADMISSION_PAYLOAD_MAX_MAX 2048U
#define NETWORK_ADMISSION_PEER_SESSION_ID_MIN 1U
#define NETWORK_ADMISSION_PEER_SESSION_ID_MAX 255U
#define NETWORK_ADMISSION_PEER_PRINCIPAL_ID_MIN 1U
#define NETWORK_ADMISSION_PEER_PRINCIPAL_ID_MAX 255U
#define NETWORK_ADMISSION_PEER_GENERATION_MIN 1U
#define NETWORK_ADMISSION_PEER_GENERATION_MAX UINT32_C(0xFFFFFFFE)
#define NETWORK_ADMISSION_ROLE_SET_MAX 8U
#define NETWORK_ADMISSION_CAPABILITY_SET_MAX 8U

enum network_admission_mode {
    NETWORK_ADMISSION_MODE_DENY_ALL = 0,
    NETWORK_ADMISSION_MODE_ROLE_SESSION = 1,
};

enum network_admission_status {
    NETWORK_ADMISSION_OK = 0,
    NETWORK_ADMISSION_NULL = 1,
    NETWORK_ADMISSION_BAD_STATE = 2,
    NETWORK_ADMISSION_BAD_VERSION = 3,
    NETWORK_ADMISSION_BAD_MODE = 4,
    NETWORK_ADMISSION_BAD_PORT = 5,
    NETWORK_ADMISSION_BAD_PAYLOAD_MAX = 6,
    NETWORK_ADMISSION_ALREADY_PUBLISHED = 7,
    NETWORK_ADMISSION_UNPUBLISHED = 8,
    NETWORK_ADMISSION_INVALID_PEER = 9,
    NETWORK_ADMISSION_UNAUTHENTICATED = 10,
    NETWORK_ADMISSION_DENIED_BY_DEFAULT = 11,
    NETWORK_ADMISSION_BAD_FRAME = 12,
    NETWORK_ADMISSION_BAD_COMMAND = 13,
    NETWORK_ADMISSION_DENIED_ROLE = 14,
    NETWORK_ADMISSION_DENIED_CAPABILITY = 15,
};

struct network_admission_policy {
    uint32_t version;
    enum network_admission_mode mode;
    uint16_t port;
    uint16_t payload_max;
};

struct network_admission_peer {
    uint32_t version;
    uint32_t session_id;
    uint32_t principal_id;
    uint32_t generation;
    bool authenticated;
    uint32_t role_count;
    uint8_t roles[NETWORK_ADMISSION_ROLE_SET_MAX];
    uint32_t capability_count;
    uint8_t capabilities[NETWORK_ADMISSION_CAPABILITY_SET_MAX];
};

struct network_admission_selection {
    bool published;
    uint32_t version;
    struct network_admission_policy policy;
};

enum network_admission_status network_admission_policy_validate(
    const struct network_admission_policy *policy);
enum network_admission_status network_admission_peer_validate(
    const struct network_admission_peer *peer);
enum network_admission_status network_admission_selection_init(
    struct network_admission_selection *selection,
    const struct network_admission_policy *policy);
bool network_admission_selection_published(
    const struct network_admission_selection *selection);
enum network_admission_status network_admission_check_request(
    const struct network_admission_selection *selection,
    const struct network_admission_peer *peer,
    const struct management_command_registry *registry,
    const struct authorization_matrix *matrix,
    const void *frame,
    size_t frame_length);
const char *network_admission_status_string(
    enum network_admission_status status);

#endif
