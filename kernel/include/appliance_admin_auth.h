#ifndef HFOC_APPLIANCE_ADMIN_AUTH_H
#define HFOC_APPLIANCE_ADMIN_AUTH_H

#include <stdbool.h>
#include <stdint.h>

#define APPLIANCE_ADMIN_AUTH_VERSION 1U
#define APPLIANCE_ADMIN_PRINCIPAL_ID 1U
#define APPLIANCE_ADMIN_CREDENTIAL_VERSION 1U
#define APPLIANCE_ADMIN_AUTH_CREDENTIAL_LENGTH 32U
#define APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH 32U
#define APPLIANCE_ADMIN_AUTH_SESSION_MAX 16U
#define APPLIANCE_ADMIN_AUTH_SESSION_ID_MAX 255U
/*
 * ITEM-475 (round 14): the session lifetime is milliseconds, the unit of the
 * `now_tick` every caller passes (`hfoc_lwip_time_now()`), and it is one hour.
 * The value read as 3600 while the tick source was the ~2.4 Hz Local APIC
 * timer, so a 3.6-hour session only looked correct because the clock was 40x
 * slow; with the production PIT at 100 Hz a bare 3600 is 3.6 seconds and the
 * authenticated management proofs expire mid-client.
 */
#define APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX (3600U * 1000U)
#define APPLIANCE_ADMIN_AUTH_BEARER_PREFIX "Authorization: Bearer "
#define APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH 22U
#define APPLIANCE_ADMIN_AUTH_FAILURE_LIMIT 5U
#define APPLIANCE_ADMIN_AUTH_COOLDOWN_TICKS 100U

enum appliance_admin_auth_state {
    APPLIANCE_ADMIN_AUTH_STATE_ABSENT = 0,
    APPLIANCE_ADMIN_AUTH_STATE_INITIALIZED = 1,
    APPLIANCE_ADMIN_AUTH_STATE_DISABLED = 2,
    APPLIANCE_ADMIN_AUTH_STATE_ACTIVE = 3,
    APPLIANCE_ADMIN_AUTH_STATE_RESET_PENDING = 4,
};

enum appliance_admin_auth_session_state {
    APPLIANCE_ADMIN_AUTH_SESSION_STATE_CLOSED = 0,
    APPLIANCE_ADMIN_AUTH_SESSION_STATE_OPEN = 1,
};

enum appliance_admin_auth_status {
    APPLIANCE_ADMIN_AUTH_OK = 0,
    APPLIANCE_ADMIN_AUTH_NULL = 1,
    APPLIANCE_ADMIN_AUTH_BAD_STATE = 2,
    APPLIANCE_ADMIN_AUTH_BAD_VERSION = 3,
    APPLIANCE_ADMIN_AUTH_BAD_CREDENTIAL = 4,
    APPLIANCE_ADMIN_AUTH_CREDENTIAL_ABSENT = 5,
    APPLIANCE_ADMIN_AUTH_AUTH_FAILED = 6,
    APPLIANCE_ADMIN_AUTH_AUTH_THROTTLED = 7,
    APPLIANCE_ADMIN_AUTH_SESSION_FULL = 8,
    APPLIANCE_ADMIN_AUTH_SESSION_ID_EXHAUSTED = 9,
    APPLIANCE_ADMIN_AUTH_SESSION_INVALID = 10,
    APPLIANCE_ADMIN_AUTH_SESSION_CLOSED = 11,
    APPLIANCE_ADMIN_AUTH_SESSION_EXPIRED = 12,
    APPLIANCE_ADMIN_AUTH_SESSION_REVOKED = 13,
    APPLIANCE_ADMIN_AUTH_TOKEN_INVALID = 14,
    APPLIANCE_ADMIN_AUTH_HEADER_MISSING = 15,
    APPLIANCE_ADMIN_AUTH_HEADER_MALFORMED = 16,
    APPLIANCE_ADMIN_AUTH_BAD_LIFETIME = 17,
    APPLIANCE_ADMIN_AUTH_BAD_TICK = 18,
    APPLIANCE_ADMIN_AUTH_GENERATION_EXHAUSTED = 19,
};

struct appliance_admin_auth_credential {
    uint32_t version;
    uint8_t verifier[APPLIANCE_ADMIN_AUTH_CREDENTIAL_LENGTH];
};

struct appliance_admin_auth_proof {
    uint32_t version;
    uint8_t proof[APPLIANCE_ADMIN_AUTH_CREDENTIAL_LENGTH];
};

struct appliance_admin_auth_session {
    uint32_t version;
    uint32_t id;
    uint32_t credential_generation;
    uint64_t opened_tick;
    uint64_t expires_tick;
    enum appliance_admin_auth_session_state state;
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
};

struct appliance_admin_auth {
    uint32_t version;
    enum appliance_admin_auth_state state;
    bool credential_published;
    uint32_t credential_generation;
    uint32_t next_session_id;
    uint32_t session_count;
    uint32_t auth_failures;
    uint64_t locked_until_tick;
    struct appliance_admin_auth_credential credential;
    struct appliance_admin_auth_session
        sessions[APPLIANCE_ADMIN_AUTH_SESSION_MAX];
};

struct appliance_admin_auth_decision {
    bool authenticated;
    uint32_t principal_id;
    uint32_t session_id;
    uint32_t credential_generation;
};

enum appliance_admin_auth_status appliance_admin_auth_init(
    struct appliance_admin_auth *auth);
enum appliance_admin_auth_status appliance_admin_auth_credential_publish(
    struct appliance_admin_auth *auth,
    const struct appliance_admin_auth_credential *credential);
enum appliance_admin_auth_status appliance_admin_auth_enable(
    struct appliance_admin_auth *auth);
enum appliance_admin_auth_status appliance_admin_auth_disable(
    struct appliance_admin_auth *auth);
enum appliance_admin_auth_status appliance_admin_auth_reset(
    struct appliance_admin_auth *auth);
enum appliance_admin_auth_status appliance_admin_auth_authenticate_local(
    struct appliance_admin_auth *auth,
    const struct appliance_admin_auth_proof *proof,
    const uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    uint64_t now_tick,
    uint32_t lifetime_ticks,
    uint32_t *out_session_id);
enum appliance_admin_auth_status appliance_admin_auth_authenticate_request(
    const struct appliance_admin_auth *auth,
    const uint8_t *authorization_header,
    uint32_t authorization_header_length,
    uint64_t now_tick,
    struct appliance_admin_auth_decision *decision);
enum appliance_admin_auth_status appliance_admin_auth_close_session(
    struct appliance_admin_auth *auth,
    uint32_t session_id);
const char *appliance_admin_auth_status_string(
    enum appliance_admin_auth_status status);
const char *appliance_admin_auth_state_string(
    enum appliance_admin_auth_state state);

#endif
