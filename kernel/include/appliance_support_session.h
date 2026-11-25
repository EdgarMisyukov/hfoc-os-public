#ifndef HFOC_APPLIANCE_SUPPORT_SESSION_H
#define HFOC_APPLIANCE_SUPPORT_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-378 consent-based remote support session.
 *
 * A remote support session is the one management-plane capability that a
 * customer is right to be afraid of, so the appliance never opens one by
 * itself and never opens one silently:
 *
 *   - a session starts as a REQUEST from the network and is inert until a
 *     locally present administrator consents on the local console.  A
 *     consent that arrives over the network is refused outright, and a
 *     consent without an identified principal is refused as well;
 *   - a granted session is time-bounded: it expires on the monotonic tick it
 *     was granted for, and it can be revoked at any moment;
 *   - the scope vocabulary is closed: a support session can read diagnostics
 *     and export the diagnostic bundle, and nothing else.  There is
 *     deliberately no command, no configuration and no shell scope, so the
 *     "no shell required" property is a declared constant, not a promise;
 *   - every transition produces an audit entry (request, consent, denial,
 *     expiry, revocation, loss of availability) that the composition appends
 *     to the ITEM-327 audit ring;
 *   - losing the composed dependencies terminates an active session: an
 *     unavailable session is never reported as active.
 *
 * Allocation-free; the module owns only the bounded state below.
 */

#define APPLIANCE_SUPPORT_SESSION_VERSION 1U
#define APPLIANCE_SUPPORT_SESSION_SCHEMA_ID "HFOC-SUPPORT-SESSION-1"
#define APPLIANCE_SUPPORT_SESSION_SCHEMA_ID_LENGTH 21U
#define APPLIANCE_SUPPORT_SESSION_DEFAULT_TTL_TICKS 600U
#define APPLIANCE_SUPPORT_SESSION_MAX_TTL_TICKS 3600U
#define APPLIANCE_SUPPORT_SESSION_AUDIT_MAX 8U
#define APPLIANCE_SUPPORT_SESSION_RENDER_MAX 200U

/* The complete scope vocabulary.  A bit outside this mask is refused, and no
 * bit in it can execute a command. */
#define APPLIANCE_SUPPORT_SESSION_SCOPE_NONE 0U
#define APPLIANCE_SUPPORT_SESSION_SCOPE_READ_DIAGNOSTICS (1U << 0)
#define APPLIANCE_SUPPORT_SESSION_SCOPE_EXPORT_BUNDLE (1U << 1)
#define APPLIANCE_SUPPORT_SESSION_SCOPES_DECLARED \
    (APPLIANCE_SUPPORT_SESSION_SCOPE_READ_DIAGNOSTICS | \
     APPLIANCE_SUPPORT_SESSION_SCOPE_EXPORT_BUNDLE)
/* Declared absence of a shell or command scope. */
#define APPLIANCE_SUPPORT_SESSION_SCOPE_SHELL 0U

enum appliance_support_session_origin {
    APPLIANCE_SUPPORT_SESSION_ORIGIN_NONE = 0,
    APPLIANCE_SUPPORT_SESSION_ORIGIN_LOCAL_CONSOLE = 1,
    APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK = 2,
};

enum appliance_support_session_state {
    APPLIANCE_SUPPORT_SESSION_STATE_NONE = 0,
    APPLIANCE_SUPPORT_SESSION_STATE_UNINITIALIZED = 1,
    APPLIANCE_SUPPORT_SESSION_STATE_UNAVAILABLE = 2,
    APPLIANCE_SUPPORT_SESSION_STATE_IDLE = 3,
    APPLIANCE_SUPPORT_SESSION_STATE_REQUESTED = 4,
    APPLIANCE_SUPPORT_SESSION_STATE_ACTIVE = 5,
    APPLIANCE_SUPPORT_SESSION_STATE_DENIED = 6,
    APPLIANCE_SUPPORT_SESSION_STATE_EXPIRED = 7,
    APPLIANCE_SUPPORT_SESSION_STATE_REVOKED = 8,
    APPLIANCE_SUPPORT_SESSION_STATE_STOPPED = 9,
};

enum appliance_support_session_status {
    APPLIANCE_SUPPORT_SESSION_OK = 0,
    APPLIANCE_SUPPORT_SESSION_NULL = 1,
    APPLIANCE_SUPPORT_SESSION_BAD_STATE = 2,
    APPLIANCE_SUPPORT_SESSION_BAD_VERSION = 3,
    APPLIANCE_SUPPORT_SESSION_UNAVAILABLE = 4,
    APPLIANCE_SUPPORT_SESSION_NO_REQUEST = 5,
    APPLIANCE_SUPPORT_SESSION_REMOTE_CONSENT_REFUSED = 6,
    APPLIANCE_SUPPORT_SESSION_DENIED = 7,
    APPLIANCE_SUPPORT_SESSION_ALREADY_ACTIVE = 8,
    APPLIANCE_SUPPORT_SESSION_BAD_SCOPE = 9,
    APPLIANCE_SUPPORT_SESSION_BAD_TTL = 10,
    APPLIANCE_SUPPORT_SESSION_EXPIRED = 11,
    APPLIANCE_SUPPORT_SESSION_BUFFER_TOO_SMALL = 12,
};

enum appliance_support_session_event {
    APPLIANCE_SUPPORT_SESSION_EVENT_NONE = 0,
    APPLIANCE_SUPPORT_SESSION_EVENT_REQUEST = 1,
    APPLIANCE_SUPPORT_SESSION_EVENT_CONSENT = 2,
    APPLIANCE_SUPPORT_SESSION_EVENT_DENY = 3,
    APPLIANCE_SUPPORT_SESSION_EVENT_EXPIRE = 4,
    APPLIANCE_SUPPORT_SESSION_EVENT_REVOKE = 5,
    APPLIANCE_SUPPORT_SESSION_EVENT_UNAVAILABLE = 6,
    APPLIANCE_SUPPORT_SESSION_EVENT_STOP = 7,
};

struct appliance_support_session_request {
    uint32_t version;
    uint64_t request_id;
    uint32_t requester_id;
    uint32_t scopes;
    uint64_t requested_tick;
};

struct appliance_support_session_audit_entry {
    uint64_t tick;
    uint64_t request_id;
    uint32_t event;
    uint32_t outcome;
    uint32_t requester_id;
    uint32_t principal_id;
    uint32_t scopes;
    uint32_t origin;
};

struct appliance_support_session {
    bool initialized;
    uint32_t version;
    enum appliance_support_session_state state;
    enum appliance_support_session_status last_status;
    bool available;
    struct appliance_support_session_request request;
    uint64_t request_sequence;
    uint64_t consent_tick;
    uint64_t expiry_tick;
    uint64_t ttl_ticks;
    uint32_t principal_id;
    uint32_t granted_scopes;
    uint32_t consent_origin;
    uint32_t request_count;
    uint32_t consent_count;
    uint32_t deny_count;
    uint32_t expire_count;
    uint32_t revoke_count;
    uint32_t unavailable_count;
    struct appliance_support_session_audit_entry
        audit[APPLIANCE_SUPPORT_SESSION_AUDIT_MAX];
    uint32_t audit_count;
    uint32_t audit_dropped;
};

enum appliance_support_session_status appliance_support_session_init(
    struct appliance_support_session *session);
enum appliance_support_session_status appliance_support_session_set_available(
    struct appliance_support_session *session,
    bool available);
/* A network-originated, inert request for a declared scope subset. */
enum appliance_support_session_status appliance_support_session_request(
    struct appliance_support_session *session,
    enum appliance_support_session_origin origin,
    uint32_t requester_id,
    uint32_t scopes,
    uint64_t now_tick);
/* Consent exists only on the local console, for an identified principal. */
enum appliance_support_session_status appliance_support_session_consent(
    struct appliance_support_session *session,
    enum appliance_support_session_origin origin,
    uint32_t principal_id,
    uint64_t ttl_ticks,
    uint64_t now_tick);
enum appliance_support_session_status appliance_support_session_refresh(
    struct appliance_support_session *session,
    uint64_t now_tick);
enum appliance_support_session_status appliance_support_session_revoke(
    struct appliance_support_session *session,
    enum appliance_support_session_origin origin,
    uint32_t principal_id,
    uint64_t now_tick);
enum appliance_support_session_status appliance_support_session_stop(
    struct appliance_support_session *session,
    uint64_t now_tick);
bool appliance_support_session_active(
    const struct appliance_support_session *session,
    uint64_t now_tick);
bool appliance_support_session_scope_allowed(
    const struct appliance_support_session *session,
    uint32_t scope,
    uint64_t now_tick);
bool appliance_support_session_shell_permitted(void);
uint32_t appliance_support_session_shell_scope(void);
uint32_t appliance_support_session_declared_scopes(void);
enum appliance_support_session_state appliance_support_session_state(
    const struct appliance_support_session *session);
enum appliance_support_session_status
appliance_support_session_last_status(
    const struct appliance_support_session *session);
uint32_t appliance_support_session_audit_count(
    const struct appliance_support_session *session);
enum appliance_support_session_status appliance_support_session_audit_entry(
    const struct appliance_support_session *session,
    uint32_t index,
    struct appliance_support_session_audit_entry *entry_out);
enum appliance_support_session_status appliance_support_session_render(
    const struct appliance_support_session *session,
    uint64_t now_tick,
    char *output,
    size_t capacity,
    size_t *output_length);
const char *appliance_support_session_state_string(
    enum appliance_support_session_state state);
const char *appliance_support_session_status_string(
    enum appliance_support_session_status status);
const char *appliance_support_session_event_string(
    enum appliance_support_session_event event);
const char *appliance_support_session_origin_string(
    enum appliance_support_session_origin origin);
const char *appliance_support_session_scope_string(uint32_t scope);
const char *appliance_support_session_schema_id(void);

#endif
