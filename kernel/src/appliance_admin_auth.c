#include "appliance_admin_auth.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void appliance_admin_auth_zero(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool appliance_admin_auth_state_valid(
    enum appliance_admin_auth_state state)
{
    return state == APPLIANCE_ADMIN_AUTH_STATE_ABSENT ||
           state == APPLIANCE_ADMIN_AUTH_STATE_INITIALIZED ||
           state == APPLIANCE_ADMIN_AUTH_STATE_DISABLED ||
           state == APPLIANCE_ADMIN_AUTH_STATE_ACTIVE ||
           state == APPLIANCE_ADMIN_AUTH_STATE_RESET_PENDING;
}

static bool appliance_admin_auth_session_state_valid(
    enum appliance_admin_auth_session_state state)
{
    return state == APPLIANCE_ADMIN_AUTH_SESSION_STATE_CLOSED ||
           state == APPLIANCE_ADMIN_AUTH_SESSION_STATE_OPEN;
}

static bool appliance_admin_auth_bytes_nonzero(
    const uint8_t *bytes,
    size_t length)
{
    size_t index;
    uint8_t nonzero = 0U;

    for (index = 0U; index < length; ++index) {
        nonzero |= bytes[index];
    }
    return nonzero != 0U;
}

static bool appliance_admin_auth_bytes_equal(
    const uint8_t *left,
    const uint8_t *right,
    size_t length)
{
    volatile uint8_t difference = 0U;
    size_t index;

    for (index = 0U; index < length; ++index) {
        difference |= (uint8_t)(left[index] ^ right[index]);
    }
    return difference == 0U;
}

static enum appliance_admin_auth_status
appliance_admin_auth_validate(
    const struct appliance_admin_auth *auth)
{
    uint32_t index;
    uint32_t open_count = 0U;

    if (auth == NULL) {
        return APPLIANCE_ADMIN_AUTH_NULL;
    }
    if (auth->version != APPLIANCE_ADMIN_AUTH_VERSION) {
        return APPLIANCE_ADMIN_AUTH_BAD_VERSION;
    }
    if (!appliance_admin_auth_state_valid(auth->state)) {
        return APPLIANCE_ADMIN_AUTH_BAD_STATE;
    }
    if (auth->session_count > APPLIANCE_ADMIN_AUTH_SESSION_MAX ||
        auth->next_session_id < 1U ||
        auth->next_session_id > APPLIANCE_ADMIN_AUTH_SESSION_ID_MAX + 1U) {
        return APPLIANCE_ADMIN_AUTH_BAD_STATE;
    }
    if (auth->state == APPLIANCE_ADMIN_AUTH_STATE_ABSENT &&
        auth->credential_published) {
        return APPLIANCE_ADMIN_AUTH_BAD_STATE;
    }
    if (!auth->credential_published &&
        (auth->state == APPLIANCE_ADMIN_AUTH_STATE_INITIALIZED ||
         auth->state == APPLIANCE_ADMIN_AUTH_STATE_DISABLED ||
         auth->state == APPLIANCE_ADMIN_AUTH_STATE_ACTIVE)) {
        return APPLIANCE_ADMIN_AUTH_BAD_STATE;
    }
    for (index = 0U; index < APPLIANCE_ADMIN_AUTH_SESSION_MAX; ++index) {
        if (!appliance_admin_auth_session_state_valid(
                auth->sessions[index].state)) {
            return APPLIANCE_ADMIN_AUTH_BAD_STATE;
        }
        if (auth->sessions[index].state ==
            APPLIANCE_ADMIN_AUTH_SESSION_STATE_OPEN) {
            ++open_count;
        }
    }
    if (open_count != auth->session_count) {
        return APPLIANCE_ADMIN_AUTH_BAD_STATE;
    }
    return APPLIANCE_ADMIN_AUTH_OK;
}

static void appliance_admin_auth_close_all_sessions(
    struct appliance_admin_auth *auth)
{
    uint32_t index;

    for (index = 0U; index < APPLIANCE_ADMIN_AUTH_SESSION_MAX; ++index) {
        if (auth->sessions[index].state ==
            APPLIANCE_ADMIN_AUTH_SESSION_STATE_OPEN) {
            auth->sessions[index].state =
                APPLIANCE_ADMIN_AUTH_SESSION_STATE_CLOSED;
            appliance_admin_auth_zero(
                auth->sessions[index].token,
                sizeof(auth->sessions[index].token));
        }
    }
    auth->session_count = 0U;
}

enum appliance_admin_auth_status appliance_admin_auth_init(
    struct appliance_admin_auth *auth)
{
    uint32_t index;

    if (auth == NULL) {
        return APPLIANCE_ADMIN_AUTH_NULL;
    }
    appliance_admin_auth_zero(auth, sizeof(*auth));
    auth->version = APPLIANCE_ADMIN_AUTH_VERSION;
    auth->state = APPLIANCE_ADMIN_AUTH_STATE_ABSENT;
    auth->next_session_id = 1U;
    for (index = 0U; index < APPLIANCE_ADMIN_AUTH_SESSION_MAX; ++index) {
        auth->sessions[index].version =
            APPLIANCE_ADMIN_AUTH_VERSION;
        auth->sessions[index].state =
            APPLIANCE_ADMIN_AUTH_SESSION_STATE_CLOSED;
    }
    return APPLIANCE_ADMIN_AUTH_OK;
}

enum appliance_admin_auth_status appliance_admin_auth_credential_publish(
    struct appliance_admin_auth *auth,
    const struct appliance_admin_auth_credential *credential)
{
    enum appliance_admin_auth_status status;

    if (auth == NULL || credential == NULL) {
        return APPLIANCE_ADMIN_AUTH_NULL;
    }
    status = appliance_admin_auth_validate(auth);
    if (status != APPLIANCE_ADMIN_AUTH_OK) {
        return status;
    }
    if (credential->version != APPLIANCE_ADMIN_CREDENTIAL_VERSION) {
        return APPLIANCE_ADMIN_AUTH_BAD_CREDENTIAL;
    }
    if (!appliance_admin_auth_bytes_nonzero(
            credential->verifier,
            sizeof(credential->verifier))) {
        return APPLIANCE_ADMIN_AUTH_BAD_CREDENTIAL;
    }
    if (auth->state != APPLIANCE_ADMIN_AUTH_STATE_ABSENT &&
        auth->state != APPLIANCE_ADMIN_AUTH_STATE_RESET_PENDING) {
        return APPLIANCE_ADMIN_AUTH_BAD_STATE;
    }
    if (auth->credential_generation == UINT32_MAX) {
        return APPLIANCE_ADMIN_AUTH_GENERATION_EXHAUSTED;
    }

    ++auth->credential_generation;
    memcpy(auth->credential.verifier,
           credential->verifier,
           sizeof(auth->credential.verifier));
    auth->credential.version = APPLIANCE_ADMIN_CREDENTIAL_VERSION;
    auth->credential_published = true;
    auth->state = APPLIANCE_ADMIN_AUTH_STATE_INITIALIZED;
    auth->auth_failures = 0U;
    auth->locked_until_tick = 0U;
    return APPLIANCE_ADMIN_AUTH_OK;
}

enum appliance_admin_auth_status appliance_admin_auth_enable(
    struct appliance_admin_auth *auth)
{
    enum appliance_admin_auth_status status;

    if (auth == NULL) {
        return APPLIANCE_ADMIN_AUTH_NULL;
    }
    status = appliance_admin_auth_validate(auth);
    if (status != APPLIANCE_ADMIN_AUTH_OK) {
        return status;
    }
    if (auth->state == APPLIANCE_ADMIN_AUTH_STATE_ACTIVE) {
        return APPLIANCE_ADMIN_AUTH_OK;
    }
    if (auth->state != APPLIANCE_ADMIN_AUTH_STATE_INITIALIZED &&
        auth->state != APPLIANCE_ADMIN_AUTH_STATE_DISABLED) {
        return APPLIANCE_ADMIN_AUTH_BAD_STATE;
    }
    auth->state = APPLIANCE_ADMIN_AUTH_STATE_ACTIVE;
    return APPLIANCE_ADMIN_AUTH_OK;
}

enum appliance_admin_auth_status appliance_admin_auth_disable(
    struct appliance_admin_auth *auth)
{
    enum appliance_admin_auth_status status;

    if (auth == NULL) {
        return APPLIANCE_ADMIN_AUTH_NULL;
    }
    status = appliance_admin_auth_validate(auth);
    if (status != APPLIANCE_ADMIN_AUTH_OK) {
        return status;
    }
    if (auth->state == APPLIANCE_ADMIN_AUTH_STATE_DISABLED) {
        return APPLIANCE_ADMIN_AUTH_OK;
    }
    if (auth->state != APPLIANCE_ADMIN_AUTH_STATE_INITIALIZED &&
        auth->state != APPLIANCE_ADMIN_AUTH_STATE_ACTIVE) {
        return APPLIANCE_ADMIN_AUTH_BAD_STATE;
    }
    if (auth->credential_generation == UINT32_MAX) {
        return APPLIANCE_ADMIN_AUTH_GENERATION_EXHAUSTED;
    }
    ++auth->credential_generation;
    appliance_admin_auth_close_all_sessions(auth);
    auth->state = APPLIANCE_ADMIN_AUTH_STATE_DISABLED;
    return APPLIANCE_ADMIN_AUTH_OK;
}

enum appliance_admin_auth_status appliance_admin_auth_reset(
    struct appliance_admin_auth *auth)
{
    enum appliance_admin_auth_status status;

    if (auth == NULL) {
        return APPLIANCE_ADMIN_AUTH_NULL;
    }
    status = appliance_admin_auth_validate(auth);
    if (status != APPLIANCE_ADMIN_AUTH_OK) {
        return status;
    }
    if (auth->state == APPLIANCE_ADMIN_AUTH_STATE_ABSENT) {
        return APPLIANCE_ADMIN_AUTH_CREDENTIAL_ABSENT;
    }
    if (auth->state == APPLIANCE_ADMIN_AUTH_STATE_RESET_PENDING) {
        return APPLIANCE_ADMIN_AUTH_OK;
    }
    appliance_admin_auth_close_all_sessions(auth);
    appliance_admin_auth_zero(
        &auth->credential,
        sizeof(auth->credential));
    auth->credential_published = false;
    auth->state = APPLIANCE_ADMIN_AUTH_STATE_RESET_PENDING;
    auth->auth_failures = 0U;
    auth->locked_until_tick = 0U;
    return APPLIANCE_ADMIN_AUTH_OK;
}

static enum appliance_admin_auth_status
appliance_admin_auth_issue_session(
    struct appliance_admin_auth *auth,
    const uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    uint64_t now_tick,
    uint32_t lifetime_ticks,
    uint32_t *out_session_id)
{
    struct appliance_admin_auth_session *session = NULL;
    uint32_t index;

    if (auth == NULL || token == NULL || out_session_id == NULL) {
        return APPLIANCE_ADMIN_AUTH_NULL;
    }
    if (auth->state != APPLIANCE_ADMIN_AUTH_STATE_ACTIVE) {
        return APPLIANCE_ADMIN_AUTH_BAD_STATE;
    }
    if (!appliance_admin_auth_bytes_nonzero(
            token,
            APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH)) {
        return APPLIANCE_ADMIN_AUTH_TOKEN_INVALID;
    }
    if (lifetime_ticks == 0U ||
        lifetime_ticks > APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX) {
        return APPLIANCE_ADMIN_AUTH_BAD_LIFETIME;
    }
    if (now_tick > UINT64_MAX - (uint64_t)lifetime_ticks) {
        return APPLIANCE_ADMIN_AUTH_BAD_TICK;
    }
    if (auth->next_session_id < 1U ||
        auth->next_session_id > APPLIANCE_ADMIN_AUTH_SESSION_ID_MAX) {
        return APPLIANCE_ADMIN_AUTH_SESSION_ID_EXHAUSTED;
    }

    for (index = 0U; index < APPLIANCE_ADMIN_AUTH_SESSION_MAX; ++index) {
        if (auth->sessions[index].state ==
            APPLIANCE_ADMIN_AUTH_SESSION_STATE_CLOSED) {
            session = &auth->sessions[index];
            break;
        }
    }
    if (session == NULL) {
        return APPLIANCE_ADMIN_AUTH_SESSION_FULL;
    }

    session->version = APPLIANCE_ADMIN_AUTH_VERSION;
    session->id = auth->next_session_id;
    session->credential_generation = auth->credential_generation;
    session->opened_tick = now_tick;
    session->expires_tick = now_tick + (uint64_t)lifetime_ticks;
    session->state = APPLIANCE_ADMIN_AUTH_SESSION_STATE_OPEN;
    memcpy(session->token,
           token,
           APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);
    ++auth->next_session_id;
    ++auth->session_count;
    *out_session_id = session->id;
    return APPLIANCE_ADMIN_AUTH_OK;
}

enum appliance_admin_auth_status appliance_admin_auth_authenticate_local(
    struct appliance_admin_auth *auth,
    const struct appliance_admin_auth_proof *proof,
    const uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    uint64_t now_tick,
    uint32_t lifetime_ticks,
    uint32_t *out_session_id)
{
    enum appliance_admin_auth_status status;
    bool proof_matches;

    if (auth == NULL || proof == NULL || token == NULL ||
        out_session_id == NULL) {
        return APPLIANCE_ADMIN_AUTH_NULL;
    }
    status = appliance_admin_auth_validate(auth);
    if (status != APPLIANCE_ADMIN_AUTH_OK) {
        return status;
    }
    if (proof->version != APPLIANCE_ADMIN_CREDENTIAL_VERSION) {
        return APPLIANCE_ADMIN_AUTH_BAD_CREDENTIAL;
    }
    if (auth->state != APPLIANCE_ADMIN_AUTH_STATE_ACTIVE) {
        return APPLIANCE_ADMIN_AUTH_BAD_STATE;
    }
    if (auth->locked_until_tick > now_tick) {
        return APPLIANCE_ADMIN_AUTH_AUTH_THROTTLED;
    }

    proof_matches = appliance_admin_auth_bytes_equal(
        auth->credential.verifier,
        proof->proof,
        APPLIANCE_ADMIN_AUTH_CREDENTIAL_LENGTH);
    if (!proof_matches) {
        ++auth->auth_failures;
        if (auth->auth_failures >= APPLIANCE_ADMIN_AUTH_FAILURE_LIMIT) {
            if (now_tick >
                UINT64_MAX - APPLIANCE_ADMIN_AUTH_COOLDOWN_TICKS) {
                return APPLIANCE_ADMIN_AUTH_BAD_TICK;
            }
            auth->locked_until_tick =
                now_tick + APPLIANCE_ADMIN_AUTH_COOLDOWN_TICKS;
            return APPLIANCE_ADMIN_AUTH_AUTH_THROTTLED;
        }
        return APPLIANCE_ADMIN_AUTH_AUTH_FAILED;
    }

    auth->auth_failures = 0U;
    auth->locked_until_tick = 0U;
    return appliance_admin_auth_issue_session(
        auth, token, now_tick, lifetime_ticks, out_session_id);
}

enum appliance_admin_auth_status appliance_admin_auth_authenticate_request(
    const struct appliance_admin_auth *auth,
    const uint8_t *authorization_header,
    uint32_t authorization_header_length,
    uint64_t now_tick,
    struct appliance_admin_auth_decision *decision)
{
    enum appliance_admin_auth_status status;
    uint32_t index;
    int match_index = -1;

    if (auth == NULL || decision == NULL) {
        return APPLIANCE_ADMIN_AUTH_NULL;
    }
    memset(decision, 0, sizeof(*decision));
    status = appliance_admin_auth_validate(auth);
    if (status != APPLIANCE_ADMIN_AUTH_OK) {
        return status;
    }
    if (auth->state != APPLIANCE_ADMIN_AUTH_STATE_ACTIVE) {
        return APPLIANCE_ADMIN_AUTH_BAD_STATE;
    }
    if (authorization_header == NULL) {
        return APPLIANCE_ADMIN_AUTH_HEADER_MISSING;
    }
    if (authorization_header_length !=
        APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
            APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH) {
        return APPLIANCE_ADMIN_AUTH_HEADER_MALFORMED;
    }
    for (index = 0U; index < APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH;
         ++index) {
        if (authorization_header[index] !=
            (uint8_t)APPLIANCE_ADMIN_AUTH_BEARER_PREFIX[index]) {
            return APPLIANCE_ADMIN_AUTH_HEADER_MALFORMED;
        }
    }
    if (!appliance_admin_auth_bytes_nonzero(
            authorization_header +
                APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH,
            APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH)) {
        return APPLIANCE_ADMIN_AUTH_TOKEN_INVALID;
    }

    for (index = 0U; index < APPLIANCE_ADMIN_AUTH_SESSION_MAX; ++index) {
        if (appliance_admin_auth_bytes_equal(
                auth->sessions[index].token,
                authorization_header +
                    APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH,
                APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH)) {
            match_index = (int)index;
        }
    }
    if (match_index < 0) {
        return APPLIANCE_ADMIN_AUTH_TOKEN_INVALID;
    }
    if (auth->sessions[match_index].state !=
        APPLIANCE_ADMIN_AUTH_SESSION_STATE_OPEN) {
        return APPLIANCE_ADMIN_AUTH_SESSION_CLOSED;
    }
    if (auth->sessions[match_index].credential_generation !=
        auth->credential_generation) {
        return APPLIANCE_ADMIN_AUTH_SESSION_REVOKED;
    }
    if (now_tick >= auth->sessions[match_index].expires_tick) {
        return APPLIANCE_ADMIN_AUTH_SESSION_EXPIRED;
    }

    decision->authenticated = true;
    decision->principal_id = APPLIANCE_ADMIN_PRINCIPAL_ID;
    decision->session_id = auth->sessions[match_index].id;
    decision->credential_generation =
        auth->sessions[match_index].credential_generation;
    return APPLIANCE_ADMIN_AUTH_OK;
}

enum appliance_admin_auth_status appliance_admin_auth_close_session(
    struct appliance_admin_auth *auth,
    uint32_t session_id)
{
    enum appliance_admin_auth_status status;
    uint32_t index;

    if (auth == NULL) {
        return APPLIANCE_ADMIN_AUTH_NULL;
    }
    status = appliance_admin_auth_validate(auth);
    if (status != APPLIANCE_ADMIN_AUTH_OK) {
        return status;
    }
    if (session_id < 1U ||
        session_id > APPLIANCE_ADMIN_AUTH_SESSION_ID_MAX) {
        return APPLIANCE_ADMIN_AUTH_SESSION_INVALID;
    }
    for (index = 0U; index < APPLIANCE_ADMIN_AUTH_SESSION_MAX; ++index) {
        if (auth->sessions[index].id == session_id) {
            if (auth->sessions[index].state ==
                APPLIANCE_ADMIN_AUTH_SESSION_STATE_CLOSED) {
                return APPLIANCE_ADMIN_AUTH_OK;
            }
            if (auth->sessions[index].state !=
                APPLIANCE_ADMIN_AUTH_SESSION_STATE_OPEN) {
                return APPLIANCE_ADMIN_AUTH_BAD_STATE;
            }
            auth->sessions[index].state =
                APPLIANCE_ADMIN_AUTH_SESSION_STATE_CLOSED;
            appliance_admin_auth_zero(
                auth->sessions[index].token,
                sizeof(auth->sessions[index].token));
            if (auth->session_count != 0U) {
                --auth->session_count;
            }
            return APPLIANCE_ADMIN_AUTH_OK;
        }
    }
    return APPLIANCE_ADMIN_AUTH_SESSION_INVALID;
}

const char *appliance_admin_auth_status_string(
    enum appliance_admin_auth_status status)
{
    switch (status) {
    case APPLIANCE_ADMIN_AUTH_OK:
        return "APPLIANCE_ADMIN_AUTH_OK";
    case APPLIANCE_ADMIN_AUTH_NULL:
        return "APPLIANCE_ADMIN_AUTH_NULL";
    case APPLIANCE_ADMIN_AUTH_BAD_STATE:
        return "APPLIANCE_ADMIN_AUTH_BAD_STATE";
    case APPLIANCE_ADMIN_AUTH_BAD_VERSION:
        return "APPLIANCE_ADMIN_AUTH_BAD_VERSION";
    case APPLIANCE_ADMIN_AUTH_BAD_CREDENTIAL:
        return "APPLIANCE_ADMIN_AUTH_BAD_CREDENTIAL";
    case APPLIANCE_ADMIN_AUTH_CREDENTIAL_ABSENT:
        return "APPLIANCE_ADMIN_AUTH_CREDENTIAL_ABSENT";
    case APPLIANCE_ADMIN_AUTH_AUTH_FAILED:
        return "APPLIANCE_ADMIN_AUTH_AUTH_FAILED";
    case APPLIANCE_ADMIN_AUTH_AUTH_THROTTLED:
        return "APPLIANCE_ADMIN_AUTH_AUTH_THROTTLED";
    case APPLIANCE_ADMIN_AUTH_SESSION_FULL:
        return "APPLIANCE_ADMIN_AUTH_SESSION_FULL";
    case APPLIANCE_ADMIN_AUTH_SESSION_ID_EXHAUSTED:
        return "APPLIANCE_ADMIN_AUTH_SESSION_ID_EXHAUSTED";
    case APPLIANCE_ADMIN_AUTH_SESSION_INVALID:
        return "APPLIANCE_ADMIN_AUTH_SESSION_INVALID";
    case APPLIANCE_ADMIN_AUTH_SESSION_CLOSED:
        return "APPLIANCE_ADMIN_AUTH_SESSION_CLOSED";
    case APPLIANCE_ADMIN_AUTH_SESSION_EXPIRED:
        return "APPLIANCE_ADMIN_AUTH_SESSION_EXPIRED";
    case APPLIANCE_ADMIN_AUTH_SESSION_REVOKED:
        return "APPLIANCE_ADMIN_AUTH_SESSION_REVOKED";
    case APPLIANCE_ADMIN_AUTH_TOKEN_INVALID:
        return "APPLIANCE_ADMIN_AUTH_TOKEN_INVALID";
    case APPLIANCE_ADMIN_AUTH_HEADER_MISSING:
        return "APPLIANCE_ADMIN_AUTH_HEADER_MISSING";
    case APPLIANCE_ADMIN_AUTH_HEADER_MALFORMED:
        return "APPLIANCE_ADMIN_AUTH_HEADER_MALFORMED";
    case APPLIANCE_ADMIN_AUTH_BAD_LIFETIME:
        return "APPLIANCE_ADMIN_AUTH_BAD_LIFETIME";
    case APPLIANCE_ADMIN_AUTH_BAD_TICK:
        return "APPLIANCE_ADMIN_AUTH_BAD_TICK";
    case APPLIANCE_ADMIN_AUTH_GENERATION_EXHAUSTED:
        return "APPLIANCE_ADMIN_AUTH_GENERATION_EXHAUSTED";
    default:
        return NULL;
    }
}

const char *appliance_admin_auth_state_string(
    enum appliance_admin_auth_state state)
{
    switch (state) {
    case APPLIANCE_ADMIN_AUTH_STATE_ABSENT:
        return "APPLIANCE_ADMIN_AUTH_STATE_ABSENT";
    case APPLIANCE_ADMIN_AUTH_STATE_INITIALIZED:
        return "APPLIANCE_ADMIN_AUTH_STATE_INITIALIZED";
    case APPLIANCE_ADMIN_AUTH_STATE_DISABLED:
        return "APPLIANCE_ADMIN_AUTH_STATE_DISABLED";
    case APPLIANCE_ADMIN_AUTH_STATE_ACTIVE:
        return "APPLIANCE_ADMIN_AUTH_STATE_ACTIVE";
    case APPLIANCE_ADMIN_AUTH_STATE_RESET_PENDING:
        return "APPLIANCE_ADMIN_AUTH_STATE_RESET_PENDING";
    default:
        return NULL;
    }
}
