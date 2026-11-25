#include "appliance_support_session.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void appliance_support_session_zero_bytes(
    unsigned char *bytes,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void appliance_support_session_copy_bytes(
    unsigned char *destination,
    const unsigned char *source,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        destination[index] = source[index];
    }
}

static bool appliance_support_session_state_valid(
    enum appliance_support_session_state state)
{
    return state >= APPLIANCE_SUPPORT_SESSION_STATE_NONE &&
           state <= APPLIANCE_SUPPORT_SESSION_STATE_STOPPED;
}

static bool appliance_support_session_status_valid(
    enum appliance_support_session_status status)
{
    return status >= APPLIANCE_SUPPORT_SESSION_OK &&
           status <= APPLIANCE_SUPPORT_SESSION_BUFFER_TOO_SMALL;
}

static bool appliance_support_session_origin_valid(
    enum appliance_support_session_origin origin)
{
    return origin >= APPLIANCE_SUPPORT_SESSION_ORIGIN_NONE &&
           origin <= APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK;
}

static bool appliance_support_session_initialized(
    const struct appliance_support_session *session)
{
    return session != NULL &&
           session->initialized &&
           session->version == APPLIANCE_SUPPORT_SESSION_VERSION &&
           appliance_support_session_state_valid(session->state) &&
           appliance_support_session_status_valid(session->last_status);
}

static void appliance_support_session_audit(
    struct appliance_support_session *session,
    enum appliance_support_session_event event,
    enum appliance_support_session_status outcome,
    uint32_t origin,
    uint32_t principal_id,
    uint64_t tick)
{
    struct appliance_support_session_audit_entry entry;
    uint32_t index;

    appliance_support_session_zero_bytes((unsigned char *)&entry,
                                         sizeof(entry));
    entry.tick = tick;
    entry.request_id = session->request.request_id;
    entry.event = (uint32_t)event;
    entry.outcome = (uint32_t)outcome;
    entry.requester_id = session->request.requester_id;
    entry.principal_id = principal_id;
    entry.scopes = session->request.scopes;
    entry.origin = origin;

    if (session->audit_count < APPLIANCE_SUPPORT_SESSION_AUDIT_MAX) {
        session->audit[session->audit_count] = entry;
        ++session->audit_count;
        return;
    }
    for (index = 1U; index < APPLIANCE_SUPPORT_SESSION_AUDIT_MAX;
         ++index) {
        session->audit[index - 1U] = session->audit[index];
    }
    session->audit[APPLIANCE_SUPPORT_SESSION_AUDIT_MAX - 1U] = entry;
    ++session->audit_dropped;
}

static bool appliance_support_session_scopes_declared(uint32_t scopes)
{
    return scopes != APPLIANCE_SUPPORT_SESSION_SCOPE_NONE &&
           (scopes & ~APPLIANCE_SUPPORT_SESSION_SCOPES_DECLARED) == 0U;
}

enum appliance_support_session_status appliance_support_session_init(
    struct appliance_support_session *session)
{
    if (session == NULL) {
        return APPLIANCE_SUPPORT_SESSION_NULL;
    }
    appliance_support_session_zero_bytes((unsigned char *)session,
                                         sizeof(*session));
    session->initialized = true;
    session->version = APPLIANCE_SUPPORT_SESSION_VERSION;
    session->state = APPLIANCE_SUPPORT_SESSION_STATE_IDLE;
    session->last_status = APPLIANCE_SUPPORT_SESSION_OK;
    session->available = true;
    return APPLIANCE_SUPPORT_SESSION_OK;
}

enum appliance_support_session_status appliance_support_session_set_available(
    struct appliance_support_session *session,
    bool available)
{
    if (!appliance_support_session_initialized(session)) {
        return APPLIANCE_SUPPORT_SESSION_NULL;
    }
    if (available) {
        session->available = true;
        if (session->state ==
            APPLIANCE_SUPPORT_SESSION_STATE_UNAVAILABLE) {
            session->state = APPLIANCE_SUPPORT_SESSION_STATE_IDLE;
            session->last_status = APPLIANCE_SUPPORT_SESSION_OK;
        }
        return APPLIANCE_SUPPORT_SESSION_OK;
    }

    session->available = false;
    if (session->state == APPLIANCE_SUPPORT_SESSION_STATE_ACTIVE ||
        session->state ==
            APPLIANCE_SUPPORT_SESSION_STATE_REQUESTED) {
        appliance_support_session_audit(
            session, APPLIANCE_SUPPORT_SESSION_EVENT_UNAVAILABLE,
            APPLIANCE_SUPPORT_SESSION_UNAVAILABLE,
            (uint32_t)APPLIANCE_SUPPORT_SESSION_ORIGIN_NONE, 0U, 0U);
        ++session->unavailable_count;
    }
    session->state = APPLIANCE_SUPPORT_SESSION_STATE_UNAVAILABLE;
    session->last_status = APPLIANCE_SUPPORT_SESSION_UNAVAILABLE;
    return APPLIANCE_SUPPORT_SESSION_OK;
}

static bool appliance_support_session_can_request(
    enum appliance_support_session_state state)
{
    return state == APPLIANCE_SUPPORT_SESSION_STATE_IDLE ||
           state == APPLIANCE_SUPPORT_SESSION_STATE_DENIED ||
           state == APPLIANCE_SUPPORT_SESSION_STATE_EXPIRED ||
           state == APPLIANCE_SUPPORT_SESSION_STATE_REVOKED;
}

enum appliance_support_session_status appliance_support_session_request(
    struct appliance_support_session *session,
    enum appliance_support_session_origin origin,
    uint32_t requester_id,
    uint32_t scopes,
    uint64_t now_tick)
{
    if (!appliance_support_session_initialized(session) ||
        !appliance_support_session_origin_valid(origin)) {
        return APPLIANCE_SUPPORT_SESSION_NULL;
    }
    if (!session->available ||
        session->state == APPLIANCE_SUPPORT_SESSION_STATE_UNAVAILABLE) {
        session->last_status = APPLIANCE_SUPPORT_SESSION_UNAVAILABLE;
        return APPLIANCE_SUPPORT_SESSION_UNAVAILABLE;
    }
    if (!appliance_support_session_scopes_declared(scopes)) {
        session->last_status = APPLIANCE_SUPPORT_SESSION_BAD_SCOPE;
        return APPLIANCE_SUPPORT_SESSION_BAD_SCOPE;
    }
    if (!appliance_support_session_can_request(session->state)) {
        session->last_status = APPLIANCE_SUPPORT_SESSION_BAD_STATE;
        return APPLIANCE_SUPPORT_SESSION_BAD_STATE;
    }

    ++session->request_sequence;
    session->request.version = APPLIANCE_SUPPORT_SESSION_VERSION;
    session->request.request_id = session->request_sequence;
    session->request.requester_id = requester_id;
    session->request.scopes = scopes;
    session->request.requested_tick = now_tick;
    session->principal_id = 0U;
    session->granted_scopes = APPLIANCE_SUPPORT_SESSION_SCOPE_NONE;
    session->consent_origin =
        (uint32_t)APPLIANCE_SUPPORT_SESSION_ORIGIN_NONE;
    session->consent_tick = 0U;
    session->expiry_tick = 0U;
    session->ttl_ticks = 0U;
    session->state = APPLIANCE_SUPPORT_SESSION_STATE_REQUESTED;
    session->last_status = APPLIANCE_SUPPORT_SESSION_OK;
    ++session->request_count;
    appliance_support_session_audit(
        session, APPLIANCE_SUPPORT_SESSION_EVENT_REQUEST,
        APPLIANCE_SUPPORT_SESSION_OK, (uint32_t)origin, 0U, now_tick);
    return APPLIANCE_SUPPORT_SESSION_OK;
}

enum appliance_support_session_status appliance_support_session_consent(
    struct appliance_support_session *session,
    enum appliance_support_session_origin origin,
    uint32_t principal_id,
    uint64_t ttl_ticks,
    uint64_t now_tick)
{
    if (!appliance_support_session_initialized(session) ||
        !appliance_support_session_origin_valid(origin)) {
        return APPLIANCE_SUPPORT_SESSION_NULL;
    }
    if (!session->available ||
        session->state == APPLIANCE_SUPPORT_SESSION_STATE_UNAVAILABLE) {
        session->last_status = APPLIANCE_SUPPORT_SESSION_UNAVAILABLE;
        return APPLIANCE_SUPPORT_SESSION_UNAVAILABLE;
    }
    if (session->state == APPLIANCE_SUPPORT_SESSION_STATE_ACTIVE) {
        session->last_status = APPLIANCE_SUPPORT_SESSION_ALREADY_ACTIVE;
        return APPLIANCE_SUPPORT_SESSION_ALREADY_ACTIVE;
    }
    if (session->state != APPLIANCE_SUPPORT_SESSION_STATE_REQUESTED) {
        session->last_status = APPLIANCE_SUPPORT_SESSION_NO_REQUEST;
        return APPLIANCE_SUPPORT_SESSION_NO_REQUEST;
    }
    if (origin != APPLIANCE_SUPPORT_SESSION_ORIGIN_LOCAL_CONSOLE) {
        session->state = APPLIANCE_SUPPORT_SESSION_STATE_DENIED;
        session->last_status =
            APPLIANCE_SUPPORT_SESSION_REMOTE_CONSENT_REFUSED;
        ++session->deny_count;
        appliance_support_session_audit(
            session, APPLIANCE_SUPPORT_SESSION_EVENT_DENY,
            APPLIANCE_SUPPORT_SESSION_REMOTE_CONSENT_REFUSED,
            (uint32_t)origin, principal_id, now_tick);
        return APPLIANCE_SUPPORT_SESSION_REMOTE_CONSENT_REFUSED;
    }
    if (principal_id == 0U) {
        session->state = APPLIANCE_SUPPORT_SESSION_STATE_DENIED;
        session->last_status = APPLIANCE_SUPPORT_SESSION_DENIED;
        ++session->deny_count;
        appliance_support_session_audit(
            session, APPLIANCE_SUPPORT_SESSION_EVENT_DENY,
            APPLIANCE_SUPPORT_SESSION_DENIED, (uint32_t)origin,
            principal_id, now_tick);
        return APPLIANCE_SUPPORT_SESSION_DENIED;
    }
    if (ttl_ticks == 0U) {
        ttl_ticks = APPLIANCE_SUPPORT_SESSION_DEFAULT_TTL_TICKS;
    }
    if (ttl_ticks > APPLIANCE_SUPPORT_SESSION_MAX_TTL_TICKS) {
        session->last_status = APPLIANCE_SUPPORT_SESSION_BAD_TTL;
        return APPLIANCE_SUPPORT_SESSION_BAD_TTL;
    }

    session->principal_id = principal_id;
    session->granted_scopes = session->request.scopes;
    session->consent_origin =
        (uint32_t)APPLIANCE_SUPPORT_SESSION_ORIGIN_LOCAL_CONSOLE;
    session->consent_tick = now_tick;
    session->ttl_ticks = ttl_ticks;
    session->expiry_tick = now_tick + ttl_ticks;
    session->state = APPLIANCE_SUPPORT_SESSION_STATE_ACTIVE;
    session->last_status = APPLIANCE_SUPPORT_SESSION_OK;
    ++session->consent_count;
    appliance_support_session_audit(
        session, APPLIANCE_SUPPORT_SESSION_EVENT_CONSENT,
        APPLIANCE_SUPPORT_SESSION_OK, (uint32_t)origin, principal_id,
        now_tick);
    return APPLIANCE_SUPPORT_SESSION_OK;
}

enum appliance_support_session_status appliance_support_session_refresh(
    struct appliance_support_session *session,
    uint64_t now_tick)
{
    if (!appliance_support_session_initialized(session)) {
        return APPLIANCE_SUPPORT_SESSION_NULL;
    }
    if (session->state == APPLIANCE_SUPPORT_SESSION_STATE_REQUESTED) {
        session->last_status = APPLIANCE_SUPPORT_SESSION_OK;
        return APPLIANCE_SUPPORT_SESSION_OK;
    }
    if (session->state != APPLIANCE_SUPPORT_SESSION_STATE_ACTIVE) {
        session->last_status = APPLIANCE_SUPPORT_SESSION_BAD_STATE;
        return APPLIANCE_SUPPORT_SESSION_BAD_STATE;
    }
    if (now_tick >= session->expiry_tick) {
        session->state = APPLIANCE_SUPPORT_SESSION_STATE_EXPIRED;
        session->granted_scopes = APPLIANCE_SUPPORT_SESSION_SCOPE_NONE;
        session->last_status = APPLIANCE_SUPPORT_SESSION_EXPIRED;
        ++session->expire_count;
        appliance_support_session_audit(
            session, APPLIANCE_SUPPORT_SESSION_EVENT_EXPIRE,
            APPLIANCE_SUPPORT_SESSION_EXPIRED,
            session->consent_origin, session->principal_id, now_tick);
        return APPLIANCE_SUPPORT_SESSION_EXPIRED;
    }
    session->last_status = APPLIANCE_SUPPORT_SESSION_OK;
    return APPLIANCE_SUPPORT_SESSION_OK;
}

enum appliance_support_session_status appliance_support_session_revoke(
    struct appliance_support_session *session,
    enum appliance_support_session_origin origin,
    uint32_t principal_id,
    uint64_t now_tick)
{
    if (!appliance_support_session_initialized(session) ||
        !appliance_support_session_origin_valid(origin)) {
        return APPLIANCE_SUPPORT_SESSION_NULL;
    }
    if (session->state != APPLIANCE_SUPPORT_SESSION_STATE_ACTIVE &&
        session->state != APPLIANCE_SUPPORT_SESSION_STATE_REQUESTED) {
        session->last_status = APPLIANCE_SUPPORT_SESSION_BAD_STATE;
        return APPLIANCE_SUPPORT_SESSION_BAD_STATE;
    }
    if (origin == APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK &&
        principal_id == 0U) {
        ++session->deny_count;
        session->last_status = APPLIANCE_SUPPORT_SESSION_DENIED;
        appliance_support_session_audit(
            session, APPLIANCE_SUPPORT_SESSION_EVENT_DENY,
            APPLIANCE_SUPPORT_SESSION_DENIED, (uint32_t)origin,
            principal_id, now_tick);
        return APPLIANCE_SUPPORT_SESSION_DENIED;
    }

    session->state = APPLIANCE_SUPPORT_SESSION_STATE_REVOKED;
    session->granted_scopes = APPLIANCE_SUPPORT_SESSION_SCOPE_NONE;
    session->last_status = APPLIANCE_SUPPORT_SESSION_OK;
    ++session->revoke_count;
    appliance_support_session_audit(
        session, APPLIANCE_SUPPORT_SESSION_EVENT_REVOKE,
        APPLIANCE_SUPPORT_SESSION_OK, (uint32_t)origin, principal_id,
        now_tick);
    return APPLIANCE_SUPPORT_SESSION_OK;
}

enum appliance_support_session_status appliance_support_session_stop(
    struct appliance_support_session *session,
    uint64_t now_tick)
{
    if (!appliance_support_session_initialized(session)) {
        return APPLIANCE_SUPPORT_SESSION_NULL;
    }
    if (session->state != APPLIANCE_SUPPORT_SESSION_STATE_STOPPED) {
        appliance_support_session_audit(
            session, APPLIANCE_SUPPORT_SESSION_EVENT_STOP,
            APPLIANCE_SUPPORT_SESSION_OK,
            (uint32_t)APPLIANCE_SUPPORT_SESSION_ORIGIN_NONE, 0U,
            now_tick);
    }
    session->state = APPLIANCE_SUPPORT_SESSION_STATE_STOPPED;
    session->granted_scopes = APPLIANCE_SUPPORT_SESSION_SCOPE_NONE;
    session->last_status = APPLIANCE_SUPPORT_SESSION_OK;
    return APPLIANCE_SUPPORT_SESSION_OK;
}

bool appliance_support_session_active(
    const struct appliance_support_session *session,
    uint64_t now_tick)
{
    return appliance_support_session_initialized(session) &&
           session->available &&
           session->state == APPLIANCE_SUPPORT_SESSION_STATE_ACTIVE &&
           now_tick < session->expiry_tick;
}

bool appliance_support_session_scope_allowed(
    const struct appliance_support_session *session,
    uint32_t scope,
    uint64_t now_tick)
{
    if (!appliance_support_session_active(session, now_tick) ||
        scope == APPLIANCE_SUPPORT_SESSION_SCOPE_NONE ||
        (scope & ~APPLIANCE_SUPPORT_SESSION_SCOPES_DECLARED) != 0U) {
        return false;
    }
    return (session->granted_scopes & scope) == scope;
}

bool appliance_support_session_shell_permitted(void)
{
    return false;
}

uint32_t appliance_support_session_shell_scope(void)
{
    return APPLIANCE_SUPPORT_SESSION_SCOPE_SHELL;
}

uint32_t appliance_support_session_declared_scopes(void)
{
    return APPLIANCE_SUPPORT_SESSION_SCOPES_DECLARED;
}

enum appliance_support_session_state appliance_support_session_state(
    const struct appliance_support_session *session)
{
    if (!appliance_support_session_initialized(session)) {
        return APPLIANCE_SUPPORT_SESSION_STATE_NONE;
    }
    return session->state;
}

enum appliance_support_session_status
appliance_support_session_last_status(
    const struct appliance_support_session *session)
{
    if (!appliance_support_session_initialized(session)) {
        return APPLIANCE_SUPPORT_SESSION_NULL;
    }
    return session->last_status;
}

uint32_t appliance_support_session_audit_count(
    const struct appliance_support_session *session)
{
    if (!appliance_support_session_initialized(session)) {
        return 0U;
    }
    return session->audit_count;
}

enum appliance_support_session_status appliance_support_session_audit_entry(
    const struct appliance_support_session *session,
    uint32_t index,
    struct appliance_support_session_audit_entry *entry_out)
{
    if (!appliance_support_session_initialized(session) ||
        entry_out == NULL) {
        return APPLIANCE_SUPPORT_SESSION_NULL;
    }
    if (index >= session->audit_count) {
        return APPLIANCE_SUPPORT_SESSION_BAD_STATE;
    }
    *entry_out = session->audit[index];
    return APPLIANCE_SUPPORT_SESSION_OK;
}

struct appliance_support_session_render_buffer {
    char *data;
    size_t capacity;
    size_t length;
    bool overflow;
};

static void appliance_support_session_render_text(
    struct appliance_support_session_render_buffer *buffer,
    const char *text)
{
    size_t index = 0U;

    while (text[index] != '\0') {
        if (buffer->length + 1U < buffer->capacity) {
            buffer->data[buffer->length] = text[index];
            ++buffer->length;
        } else {
            buffer->overflow = true;
        }
        ++index;
    }
}

static void appliance_support_session_render_u64(
    struct appliance_support_session_render_buffer *buffer,
    uint64_t value)
{
    char digits[20];
    size_t count = 0U;

    if (value == 0U) {
        appliance_support_session_render_text(buffer, "0");
        return;
    }
    while (value != 0U && count < sizeof(digits)) {
        digits[count] = (char)('0' + (value % 10U));
        value /= 10U;
        ++count;
    }
    while (count > 0U) {
        char single[2];

        --count;
        single[0] = digits[count];
        single[1] = '\0';
        appliance_support_session_render_text(buffer, single);
    }
}

static void appliance_support_session_render_scopes(
    struct appliance_support_session_render_buffer *buffer,
    uint32_t scopes)
{
    if ((scopes & APPLIANCE_SUPPORT_SESSION_SCOPE_READ_DIAGNOSTICS) !=
        0U) {
        appliance_support_session_render_text(buffer,
                                              "READ_DIAGNOSTICS");
    }
    if ((scopes & APPLIANCE_SUPPORT_SESSION_SCOPE_EXPORT_BUNDLE) != 0U) {
        if (scopes != APPLIANCE_SUPPORT_SESSION_SCOPE_EXPORT_BUNDLE) {
            appliance_support_session_render_text(buffer, "|");
        }
        appliance_support_session_render_text(buffer, "EXPORT_BUNDLE");
    }
    if (scopes == APPLIANCE_SUPPORT_SESSION_SCOPE_NONE) {
        appliance_support_session_render_text(buffer, "NONE");
    }
}

static const char *appliance_support_session_consent_string(
    const struct appliance_support_session *session)
{
    if (session->consent_origin ==
        (uint32_t)APPLIANCE_SUPPORT_SESSION_ORIGIN_LOCAL_CONSOLE) {
        return "LOCAL";
    }
    if (session->consent_origin ==
        (uint32_t)APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK) {
        return "NETWORK";
    }
    return "NONE";
}

enum appliance_support_session_status appliance_support_session_render(
    const struct appliance_support_session *session,
    uint64_t now_tick,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    struct appliance_support_session_render_buffer buffer;
    char staging[APPLIANCE_SUPPORT_SESSION_RENDER_MAX];
    bool active;
    uint64_t remaining = 0U;

    if (!appliance_support_session_initialized(session) ||
        output == NULL || output_length == NULL) {
        return APPLIANCE_SUPPORT_SESSION_NULL;
    }
    active = appliance_support_session_active(session, now_tick);
    if (active && session->expiry_tick > now_tick) {
        remaining = session->expiry_tick - now_tick;
    }

    buffer.data = staging;
    buffer.capacity = sizeof(staging);
    buffer.length = 0U;
    buffer.overflow = false;

    appliance_support_session_render_text(&buffer,
                                          "support_session v=1 state=");
    appliance_support_session_render_text(
        &buffer, appliance_support_session_state_string(session->state));
    appliance_support_session_render_text(&buffer, " active=");
    appliance_support_session_render_text(&buffer,
                                          active ? "YES" : "NO");
    appliance_support_session_render_text(&buffer, " requester=");
    appliance_support_session_render_u64(
        &buffer, session->request.requester_id);
    appliance_support_session_render_text(&buffer, " scopes=");
    appliance_support_session_render_scopes(
        &buffer, active ? session->granted_scopes
                        : APPLIANCE_SUPPORT_SESSION_SCOPE_NONE);
    appliance_support_session_render_text(&buffer, " consent=");
    appliance_support_session_render_text(
        &buffer, appliance_support_session_consent_string(session));
    appliance_support_session_render_text(&buffer, " ttl=");
    appliance_support_session_render_u64(&buffer, session->ttl_ticks);
    appliance_support_session_render_text(&buffer, " remaining=");
    appliance_support_session_render_u64(&buffer, remaining);
    appliance_support_session_render_text(&buffer, " shell=FORBIDDEN");
    appliance_support_session_render_text(&buffer, " audit=");
    appliance_support_session_render_u64(&buffer, session->audit_count);

    if (buffer.overflow || buffer.length + 1U > capacity) {
        *output_length = 0U;
        return APPLIANCE_SUPPORT_SESSION_BUFFER_TOO_SMALL;
    }
    appliance_support_session_copy_bytes(
        (unsigned char *)output, (const unsigned char *)staging,
        buffer.length);
    output[buffer.length] = '\0';
    *output_length = buffer.length;
    return APPLIANCE_SUPPORT_SESSION_OK;
}

const char *appliance_support_session_state_string(
    enum appliance_support_session_state state)
{
    switch (state) {
    case APPLIANCE_SUPPORT_SESSION_STATE_NONE:
        return "NONE";
    case APPLIANCE_SUPPORT_SESSION_STATE_UNINITIALIZED:
        return "UNINITIALIZED";
    case APPLIANCE_SUPPORT_SESSION_STATE_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_SUPPORT_SESSION_STATE_IDLE:
        return "IDLE";
    case APPLIANCE_SUPPORT_SESSION_STATE_REQUESTED:
        return "REQUESTED";
    case APPLIANCE_SUPPORT_SESSION_STATE_ACTIVE:
        return "ACTIVE";
    case APPLIANCE_SUPPORT_SESSION_STATE_DENIED:
        return "DENIED";
    case APPLIANCE_SUPPORT_SESSION_STATE_EXPIRED:
        return "EXPIRED";
    case APPLIANCE_SUPPORT_SESSION_STATE_REVOKED:
        return "REVOKED";
    case APPLIANCE_SUPPORT_SESSION_STATE_STOPPED:
        return "STOPPED";
    default:
        return "UNKNOWN";
    }
}

const char *appliance_support_session_status_string(
    enum appliance_support_session_status status)
{
    switch (status) {
    case APPLIANCE_SUPPORT_SESSION_OK:
        return "OK";
    case APPLIANCE_SUPPORT_SESSION_NULL:
        return "NULL";
    case APPLIANCE_SUPPORT_SESSION_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_SUPPORT_SESSION_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_SUPPORT_SESSION_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_SUPPORT_SESSION_NO_REQUEST:
        return "NO_REQUEST";
    case APPLIANCE_SUPPORT_SESSION_REMOTE_CONSENT_REFUSED:
        return "REMOTE_CONSENT_REFUSED";
    case APPLIANCE_SUPPORT_SESSION_DENIED:
        return "DENIED";
    case APPLIANCE_SUPPORT_SESSION_ALREADY_ACTIVE:
        return "ALREADY_ACTIVE";
    case APPLIANCE_SUPPORT_SESSION_BAD_SCOPE:
        return "BAD_SCOPE";
    case APPLIANCE_SUPPORT_SESSION_BAD_TTL:
        return "BAD_TTL";
    case APPLIANCE_SUPPORT_SESSION_EXPIRED:
        return "EXPIRED";
    case APPLIANCE_SUPPORT_SESSION_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    default:
        return "UNKNOWN";
    }
}

const char *appliance_support_session_event_string(
    enum appliance_support_session_event event)
{
    switch (event) {
    case APPLIANCE_SUPPORT_SESSION_EVENT_NONE:
        return "NONE";
    case APPLIANCE_SUPPORT_SESSION_EVENT_REQUEST:
        return "REQUEST";
    case APPLIANCE_SUPPORT_SESSION_EVENT_CONSENT:
        return "CONSENT";
    case APPLIANCE_SUPPORT_SESSION_EVENT_DENY:
        return "DENY";
    case APPLIANCE_SUPPORT_SESSION_EVENT_EXPIRE:
        return "EXPIRE";
    case APPLIANCE_SUPPORT_SESSION_EVENT_REVOKE:
        return "REVOKE";
    case APPLIANCE_SUPPORT_SESSION_EVENT_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_SUPPORT_SESSION_EVENT_STOP:
        return "STOP";
    default:
        return "UNKNOWN";
    }
}

const char *appliance_support_session_origin_string(
    enum appliance_support_session_origin origin)
{
    switch (origin) {
    case APPLIANCE_SUPPORT_SESSION_ORIGIN_NONE:
        return "NONE";
    case APPLIANCE_SUPPORT_SESSION_ORIGIN_LOCAL_CONSOLE:
        return "LOCAL";
    case APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK:
        return "NETWORK";
    default:
        return "UNKNOWN";
    }
}

const char *appliance_support_session_scope_string(uint32_t scope)
{
    switch (scope) {
    case APPLIANCE_SUPPORT_SESSION_SCOPE_READ_DIAGNOSTICS:
        return "READ_DIAGNOSTICS";
    case APPLIANCE_SUPPORT_SESSION_SCOPE_EXPORT_BUNDLE:
        return "EXPORT_BUNDLE";
    default:
        return "UNKNOWN";
    }
}

const char *appliance_support_session_schema_id(void)
{
    return APPLIANCE_SUPPORT_SESSION_SCHEMA_ID;
}
