/*
 * ITEM-378 focused proof: the consent-based remote support session.
 *
 * The fixture proves that a network request is inert until a locally present
 * administrator consents, that a network consent is refused, that the grant
 * is time-bounded and revocable, that the scope vocabulary has no shell bit,
 * and that every transition is audited.
 */
#include "appliance_support_session.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_SUPPORT_SESSION_HOST_TEST

#define TEST_RENDER_CAPACITY 256U
#define TEST_TICK_BASE 1000U

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

void *kmalloc(size_t size) {
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr) {
    (void)ptr;
    ++free_attempts;
    return false;
}

static void test_declared_vocabulary(void)
{
    CHECK(!appliance_support_session_shell_permitted(),
          "a support session never permits a shell");
    CHECK(appliance_support_session_shell_scope() == 0U,
          "the declared shell scope is empty");
    CHECK(appliance_support_session_declared_scopes() ==
              (APPLIANCE_SUPPORT_SESSION_SCOPE_READ_DIAGNOSTICS |
               APPLIANCE_SUPPORT_SESSION_SCOPE_EXPORT_BUNDLE),
          "the declared scope vocabulary is read and bundle export only");
    CHECK(strcmp(appliance_support_session_schema_id(),
                 APPLIANCE_SUPPORT_SESSION_SCHEMA_ID) == 0,
          "the session schema id is the declared id");
    CHECK(strcmp(appliance_support_session_scope_string(
                     APPLIANCE_SUPPORT_SESSION_SCOPE_READ_DIAGNOSTICS),
                 "READ_DIAGNOSTICS") == 0 &&
              strcmp(appliance_support_session_scope_string(
                         APPLIANCE_SUPPORT_SESSION_SCOPE_EXPORT_BUNDLE),
                     "EXPORT_BUNDLE") == 0 &&
              strcmp(appliance_support_session_scope_string(
                         0x80000000U),
                     "UNKNOWN") == 0,
          "scope strings are the declared vocabulary");
}

static void test_request_requires_consent(void)
{
    struct appliance_support_session session;
    struct appliance_support_session_audit_entry entry;

    CHECK(appliance_support_session_init(&session) ==
              APPLIANCE_SUPPORT_SESSION_OK,
          "session initializes");
    CHECK(appliance_support_session_state(&session) ==
              APPLIANCE_SUPPORT_SESSION_STATE_IDLE,
          "a fresh session is idle");
    CHECK(appliance_support_session_audit_count(&session) == 0U,
          "a fresh session has an empty audit trail");

    CHECK(appliance_support_session_request(
              &session,
              APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK, 7U,
              APPLIANCE_SUPPORT_SESSION_SCOPES_DECLARED,
              TEST_TICK_BASE) == APPLIANCE_SUPPORT_SESSION_OK,
          "a declared-scope network request is accepted");
    CHECK(appliance_support_session_state(&session) ==
              APPLIANCE_SUPPORT_SESSION_STATE_REQUESTED,
          "a request is inert until consent");
    CHECK(!appliance_support_session_active(&session,
                                            TEST_TICK_BASE),
          "a request grants no active session");
    CHECK(!appliance_support_session_scope_allowed(
              &session,
              APPLIANCE_SUPPORT_SESSION_SCOPE_READ_DIAGNOSTICS,
              TEST_TICK_BASE),
          "a request grants no scope");
    CHECK(appliance_support_session_audit_count(&session) == 1U,
          "a request is audited");
    memset(&entry, 0, sizeof(entry));
    CHECK(appliance_support_session_audit_entry(&session, 0U, &entry) ==
              APPLIANCE_SUPPORT_SESSION_OK &&
              entry.event == APPLIANCE_SUPPORT_SESSION_EVENT_REQUEST &&
              entry.requester_id == 7U &&
              entry.scopes ==
                  APPLIANCE_SUPPORT_SESSION_SCOPES_DECLARED &&
              entry.origin ==
                  APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK,
          "the request audit entry names requester, scopes and origin");

    CHECK(appliance_support_session_request(
              &session,
              APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK, 7U,
              APPLIANCE_SUPPORT_SESSION_SCOPE_READ_DIAGNOSTICS,
              TEST_TICK_BASE + 1U) ==
              APPLIANCE_SUPPORT_SESSION_BAD_STATE,
          "a second request while one is pending is refused");

    CHECK(appliance_support_session_consent(
              &session,
              APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK, 1U, 0U,
              TEST_TICK_BASE + 2U) ==
              APPLIANCE_SUPPORT_SESSION_REMOTE_CONSENT_REFUSED,
          "consent over the network is refused");
    CHECK(appliance_support_session_state(&session) ==
              APPLIANCE_SUPPORT_SESSION_STATE_DENIED,
          "a refused network consent denies the session");
    CHECK(!appliance_support_session_active(&session,
                                            TEST_TICK_BASE + 2U),
          "a denied session is not active");
    CHECK(appliance_support_session_audit_count(&session) == 2U,
          "the denial is audited");
    memset(&entry, 0, sizeof(entry));
    CHECK(appliance_support_session_audit_entry(&session, 1U, &entry) ==
              APPLIANCE_SUPPORT_SESSION_OK &&
              entry.event == APPLIANCE_SUPPORT_SESSION_EVENT_DENY,
          "the denial audit entry records the denial");

    CHECK(appliance_support_session_consent(
              &session,
              APPLIANCE_SUPPORT_SESSION_ORIGIN_LOCAL_CONSOLE, 1U, 0U,
              TEST_TICK_BASE + 3U) ==
              APPLIANCE_SUPPORT_SESSION_NO_REQUEST,
          "consent without a pending request is refused");
}

static void test_scope_vocabulary_is_closed(void)
{
    struct appliance_support_session session;

    (void)appliance_support_session_init(&session);
    CHECK(appliance_support_session_request(
              &session,
              APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK, 7U, 0U,
              TEST_TICK_BASE) == APPLIANCE_SUPPORT_SESSION_BAD_SCOPE,
          "a request without a scope is refused");
    CHECK(appliance_support_session_request(
              &session,
              APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK, 7U,
              0x80000000U, TEST_TICK_BASE) ==
              APPLIANCE_SUPPORT_SESSION_BAD_SCOPE,
          "a request with an undeclared scope bit is refused");
    CHECK(appliance_support_session_request(
              &session,
              APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK, 7U,
              APPLIANCE_SUPPORT_SESSION_SCOPES_DECLARED | 0x40U,
              TEST_TICK_BASE) == APPLIANCE_SUPPORT_SESSION_BAD_SCOPE,
          "a request with a command-shaped scope bit is refused");
    CHECK(appliance_support_session_state(&session) ==
              APPLIANCE_SUPPORT_SESSION_STATE_IDLE,
          "a refused scope leaves the session idle");
    CHECK(appliance_support_session_audit_count(&session) == 0U,
          "a refused scope does not fabricate an audit entry");
}

static void test_local_consent_bounds_and_expiry(void)
{
    struct appliance_support_session session;
    struct appliance_support_session_audit_entry entry;

    (void)appliance_support_session_init(&session);
    CHECK(appliance_support_session_request(
              &session,
              APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK, 7U,
              APPLIANCE_SUPPORT_SESSION_SCOPES_DECLARED,
              TEST_TICK_BASE) == APPLIANCE_SUPPORT_SESSION_OK,
          "session request is pending");
    CHECK(appliance_support_session_consent(
              &session,
              APPLIANCE_SUPPORT_SESSION_ORIGIN_LOCAL_CONSOLE, 0U, 0U,
              TEST_TICK_BASE + 1U) ==
              APPLIANCE_SUPPORT_SESSION_DENIED,
          "consent without an identified principal is refused");
    CHECK(appliance_support_session_state(&session) ==
              APPLIANCE_SUPPORT_SESSION_STATE_DENIED,
          "an anonymous consent denies the session");

    (void)appliance_support_session_init(&session);
    (void)appliance_support_session_request(
        &session, APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK, 7U,
        APPLIANCE_SUPPORT_SESSION_SCOPES_DECLARED, TEST_TICK_BASE);
    CHECK(appliance_support_session_consent(
              &session,
              APPLIANCE_SUPPORT_SESSION_ORIGIN_LOCAL_CONSOLE, 1U,
              APPLIANCE_SUPPORT_SESSION_MAX_TTL_TICKS + 1U,
              TEST_TICK_BASE + 1U) ==
              APPLIANCE_SUPPORT_SESSION_BAD_TTL,
          "a consent beyond the maximum lifetime is refused");

    CHECK(appliance_support_session_consent(
              &session,
              APPLIANCE_SUPPORT_SESSION_ORIGIN_LOCAL_CONSOLE, 1U, 0U,
              TEST_TICK_BASE + 1U) == APPLIANCE_SUPPORT_SESSION_OK,
          "a local consent with the default lifetime is granted");
    CHECK(appliance_support_session_state(&session) ==
              APPLIANCE_SUPPORT_SESSION_STATE_ACTIVE,
          "a consented session is active");
    CHECK(session.ttl_ticks ==
              APPLIANCE_SUPPORT_SESSION_DEFAULT_TTL_TICKS &&
              session.expiry_tick ==
                  TEST_TICK_BASE + 1U +
                      APPLIANCE_SUPPORT_SESSION_DEFAULT_TTL_TICKS,
          "the grant is bounded by the declared lifetime");
    CHECK(appliance_support_session_active(
              &session,
              TEST_TICK_BASE + APPLIANCE_SUPPORT_SESSION_DEFAULT_TTL_TICKS),
          "the session is active until its expiry tick");
    CHECK(!appliance_support_session_active(
              &session,
              TEST_TICK_BASE + 1U +
                  APPLIANCE_SUPPORT_SESSION_DEFAULT_TTL_TICKS),
          "the session is inactive at its expiry tick");
    CHECK(appliance_support_session_scope_allowed(
              &session,
              APPLIANCE_SUPPORT_SESSION_SCOPE_READ_DIAGNOSTICS,
              TEST_TICK_BASE + 2U) &&
              appliance_support_session_scope_allowed(
                  &session,
                  APPLIANCE_SUPPORT_SESSION_SCOPE_EXPORT_BUNDLE,
                  TEST_TICK_BASE + 2U),
          "a granted session allows exactly the declared scopes");
    CHECK(!appliance_support_session_scope_allowed(
              &session, APPLIANCE_SUPPORT_SESSION_SCOPE_SHELL,
              TEST_TICK_BASE + 2U) &&
              !appliance_support_session_scope_allowed(
                  &session, 0x80000000U, TEST_TICK_BASE + 2U),
          "a granted session allows no shell or undeclared scope");
    CHECK(appliance_support_session_consent(
              &session,
              APPLIANCE_SUPPORT_SESSION_ORIGIN_LOCAL_CONSOLE, 1U, 0U,
              TEST_TICK_BASE + 3U) ==
              APPLIANCE_SUPPORT_SESSION_ALREADY_ACTIVE,
          "a second consent while active is refused");
    CHECK(appliance_support_session_request(
              &session,
              APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK, 8U,
              APPLIANCE_SUPPORT_SESSION_SCOPE_READ_DIAGNOSTICS,
              TEST_TICK_BASE + 3U) ==
              APPLIANCE_SUPPORT_SESSION_BAD_STATE,
          "a new request while active is refused");

    CHECK(appliance_support_session_refresh(
              &session, TEST_TICK_BASE + 2U) ==
              APPLIANCE_SUPPORT_SESSION_OK,
          "a refresh before expiry keeps the session");
    CHECK(appliance_support_session_refresh(
              &session,
              TEST_TICK_BASE + 1U +
                  APPLIANCE_SUPPORT_SESSION_DEFAULT_TTL_TICKS) ==
              APPLIANCE_SUPPORT_SESSION_EXPIRED,
          "a refresh at the expiry tick expires the session");
    CHECK(appliance_support_session_state(&session) ==
              APPLIANCE_SUPPORT_SESSION_STATE_EXPIRED &&
              session.expire_count == 1U,
          "an expired session publishes EXPIRED once");
    CHECK(appliance_support_session_audit_count(&session) == 3U,
          "request, consent and expiry are audited");
    memset(&entry, 0, sizeof(entry));
    CHECK(appliance_support_session_audit_entry(&session, 2U, &entry) ==
              APPLIANCE_SUPPORT_SESSION_OK &&
              entry.event == APPLIANCE_SUPPORT_SESSION_EVENT_EXPIRE,
          "the expiry audit entry is recorded");
}

static void test_revocation_channel(void)
{
    struct appliance_support_session session;
    struct appliance_support_session_audit_entry entry;

    (void)appliance_support_session_init(&session);
    (void)appliance_support_session_request(
        &session, APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK, 9U,
        APPLIANCE_SUPPORT_SESSION_SCOPE_EXPORT_BUNDLE, TEST_TICK_BASE);
    CHECK(appliance_support_session_consent(
              &session,
              APPLIANCE_SUPPORT_SESSION_ORIGIN_LOCAL_CONSOLE, 1U, 120U,
              TEST_TICK_BASE + 1U) == APPLIANCE_SUPPORT_SESSION_OK,
          "a short-lived local consent is granted");
    CHECK(appliance_support_session_revoke(
              &session,
              APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK, 0U,
              TEST_TICK_BASE + 2U) ==
              APPLIANCE_SUPPORT_SESSION_DENIED,
          "a remote revocation without a principal is refused");
    CHECK(appliance_support_session_state(&session) ==
              APPLIANCE_SUPPORT_SESSION_STATE_ACTIVE,
          "a refused revocation leaves the session untouched");
    CHECK(appliance_support_session_revoke(
              &session,
              APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK, 1U,
              TEST_TICK_BASE + 3U) == APPLIANCE_SUPPORT_SESSION_OK,
          "an identified principal may revoke the session");
    CHECK(appliance_support_session_state(&session) ==
              APPLIANCE_SUPPORT_SESSION_STATE_REVOKED &&
              session.revoke_count == 1U,
          "a revoked session publishes REVOKED once");
    CHECK(!appliance_support_session_active(&session,
                                            TEST_TICK_BASE + 4U) &&
              !appliance_support_session_scope_allowed(
                  &session,
                  APPLIANCE_SUPPORT_SESSION_SCOPE_EXPORT_BUNDLE,
                  TEST_TICK_BASE + 4U),
          "a revoked session grants nothing");
    CHECK(appliance_support_session_refresh(
              &session, TEST_TICK_BASE + 500U) ==
              APPLIANCE_SUPPORT_SESSION_BAD_STATE,
          "a revoked session is not refreshed");

    memset(&entry, 0, sizeof(entry));
    CHECK(appliance_support_session_audit_entry(&session, 3U, &entry) ==
              APPLIANCE_SUPPORT_SESSION_OK &&
              entry.event == APPLIANCE_SUPPORT_SESSION_EVENT_REVOKE,
          "the revocation audit entry is recorded");

    /* A revoked session can be requested again, and it stays inert. */
    CHECK(appliance_support_session_request(
              &session,
              APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK, 10U,
              APPLIANCE_SUPPORT_SESSION_SCOPE_READ_DIAGNOSTICS,
              TEST_TICK_BASE + 6U) == APPLIANCE_SUPPORT_SESSION_OK,
          "a new request after revocation is accepted");
    CHECK(appliance_support_session_state(&session) ==
              APPLIANCE_SUPPORT_SESSION_STATE_REQUESTED,
          "a new request is inert until a new consent");
    CHECK(session.request_count == 2U && session.consent_count == 1U,
          "the session counts requests and consents exactly");
}

static void test_unavailability_fails_closed(void)
{
    struct appliance_support_session session;

    (void)appliance_support_session_init(&session);
    CHECK(appliance_support_session_set_available(&session, false) ==
              APPLIANCE_SUPPORT_SESSION_OK,
          "the session composition can withdraw availability");
    CHECK(appliance_support_session_state(&session) ==
              APPLIANCE_SUPPORT_SESSION_STATE_UNAVAILABLE,
          "an unavailable session publishes UNAVAILABLE");
    CHECK(appliance_support_session_request(
              &session,
              APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK, 7U,
              APPLIANCE_SUPPORT_SESSION_SCOPE_READ_DIAGNOSTICS,
              TEST_TICK_BASE) == APPLIANCE_SUPPORT_SESSION_UNAVAILABLE,
          "an unavailable session refuses a request");
    CHECK(appliance_support_session_state(&session) ==
              APPLIANCE_SUPPORT_SESSION_STATE_UNAVAILABLE,
          "a refused request does not revive the session");

    (void)appliance_support_session_init(&session);
    (void)appliance_support_session_request(
        &session, APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK, 7U,
        APPLIANCE_SUPPORT_SESSION_SCOPE_READ_DIAGNOSTICS,
        TEST_TICK_BASE);
    (void)appliance_support_session_consent(
        &session, APPLIANCE_SUPPORT_SESSION_ORIGIN_LOCAL_CONSOLE, 1U, 0U,
        TEST_TICK_BASE + 1U);
    CHECK(appliance_support_session_set_available(&session, false) ==
              APPLIANCE_SUPPORT_SESSION_OK,
          "availability can be withdrawn while a session is active");
    CHECK(appliance_support_session_state(&session) ==
              APPLIANCE_SUPPORT_SESSION_STATE_UNAVAILABLE,
          "an active session is terminated when dependencies vanish");
    CHECK(!appliance_support_session_active(&session,
                                            TEST_TICK_BASE + 2U),
          "an unavailable session never reports active");
    CHECK(appliance_support_session_audit_count(&session) == 3U &&
              session.unavailable_count == 1U,
          "the loss of availability is audited once");
}

static void test_render_and_metadata(void)
{
    struct appliance_support_session session;
    char output[TEST_RENDER_CAPACITY];
    size_t output_length = 0U;

    (void)appliance_support_session_init(&session);
    (void)appliance_support_session_request(
        &session, APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK, 7U,
        APPLIANCE_SUPPORT_SESSION_SCOPES_DECLARED, TEST_TICK_BASE);
    (void)appliance_support_session_consent(
        &session, APPLIANCE_SUPPORT_SESSION_ORIGIN_LOCAL_CONSOLE, 1U, 0U,
        TEST_TICK_BASE + 1U);

    memset(output, 0, sizeof(output));
    CHECK(appliance_support_session_render(
              &session, TEST_TICK_BASE + 1U, output, sizeof(output),
              &output_length) == APPLIANCE_SUPPORT_SESSION_OK,
          "render accepts an active session");
    CHECK(strstr(output, "support_session v=1 state=ACTIVE") != NULL,
          "render names the active state");
    CHECK(strstr(output, "active=YES") != NULL,
          "render publishes the active verdict");
    CHECK(strstr(output,
                 "scopes=READ_DIAGNOSTICS|EXPORT_BUNDLE") != NULL,
          "render publishes the granted scopes");
    CHECK(strstr(output, "consent=LOCAL") != NULL,
          "render publishes the local consent origin");
    CHECK(strstr(output, "shell=FORBIDDEN") != NULL,
          "render publishes the declared absence of a shell");
    CHECK(strstr(output, "remaining=600") != NULL,
          "render publishes the remaining bounded lifetime");
    CHECK(output_length == strlen(output),
          "render reports the exact length");

    (void)appliance_support_session_refresh(
        &session,
        TEST_TICK_BASE + 1U +
            APPLIANCE_SUPPORT_SESSION_DEFAULT_TTL_TICKS);
    CHECK(appliance_support_session_render(
              &session,
              TEST_TICK_BASE + 1U +
                  APPLIANCE_SUPPORT_SESSION_DEFAULT_TTL_TICKS,
              output, sizeof(output), &output_length) ==
              APPLIANCE_SUPPORT_SESSION_OK &&
              strstr(output, "state=EXPIRED") != NULL &&
              strstr(output, "active=NO") != NULL &&
              strstr(output, "remaining=0") != NULL,
          "render publishes an expired session honestly");
    CHECK(appliance_support_session_render(
              &session, TEST_TICK_BASE, output, 8U, &output_length) ==
              APPLIANCE_SUPPORT_SESSION_BUFFER_TOO_SMALL,
          "render refuses a short buffer");
    CHECK(appliance_support_session_render(
              NULL, TEST_TICK_BASE, output, sizeof(output),
              &output_length) == APPLIANCE_SUPPORT_SESSION_NULL,
          "render refuses a null session");

    CHECK(strcmp(appliance_support_session_state_string(
                     APPLIANCE_SUPPORT_SESSION_STATE_REQUESTED),
                 "REQUESTED") == 0 &&
              strcmp(appliance_support_session_status_string(
                         APPLIANCE_SUPPORT_SESSION_REMOTE_CONSENT_REFUSED),
                     "REMOTE_CONSENT_REFUSED") == 0 &&
              strcmp(appliance_support_session_event_string(
                         APPLIANCE_SUPPORT_SESSION_EVENT_CONSENT),
                     "CONSENT") == 0 &&
              strcmp(appliance_support_session_origin_string(
                         APPLIANCE_SUPPORT_SESSION_ORIGIN_LOCAL_CONSOLE),
                     "LOCAL") == 0,
          "state, status, event and origin strings are declared");
    CHECK(appliance_support_session_state(NULL) ==
              APPLIANCE_SUPPORT_SESSION_STATE_NONE &&
              appliance_support_session_last_status(NULL) ==
                  APPLIANCE_SUPPORT_SESSION_NULL &&
              appliance_support_session_audit_count(NULL) == 0U &&
              !appliance_support_session_active(NULL, 0U),
          "accessors fail closed on a null session");
}

static void test_audit_ring_is_bounded(void)
{
    struct appliance_support_session session;
    struct appliance_support_session_audit_entry entry;
    uint32_t index;

    (void)appliance_support_session_init(&session);
    for (index = 0U; index < 6U; ++index) {
        (void)appliance_support_session_request(
            &session, APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK,
            100U + index,
            APPLIANCE_SUPPORT_SESSION_SCOPE_READ_DIAGNOSTICS,
            TEST_TICK_BASE + index);
        (void)appliance_support_session_consent(
            &session,
            APPLIANCE_SUPPORT_SESSION_ORIGIN_NETWORK, 1U, 0U,
            TEST_TICK_BASE + index);
    }
    CHECK(appliance_support_session_audit_count(&session) ==
              APPLIANCE_SUPPORT_SESSION_AUDIT_MAX,
          "the audit trail is bounded by the declared capacity");
    CHECK(session.audit_dropped == 4U,
          "dropped audit entries are counted instead of hidden");
    memset(&entry, 0, sizeof(entry));
    CHECK(appliance_support_session_audit_entry(
              &session,
              APPLIANCE_SUPPORT_SESSION_AUDIT_MAX - 1U, &entry) ==
              APPLIANCE_SUPPORT_SESSION_OK &&
              entry.event == APPLIANCE_SUPPORT_SESSION_EVENT_DENY,
          "the retained window ends at the newest entry");
    CHECK(appliance_support_session_audit_entry(
              &session, APPLIANCE_SUPPORT_SESSION_AUDIT_MAX, &entry) ==
              APPLIANCE_SUPPORT_SESSION_BAD_STATE,
          "an out-of-range audit index is refused");
    CHECK(appliance_support_session_audit_entry(&session, 0U, NULL) ==
              APPLIANCE_SUPPORT_SESSION_NULL,
          "a null audit output is refused");
}

int main(void)
{
    test_declared_vocabulary();
    test_request_requires_consent();
    test_scope_vocabulary_is_closed();
    test_local_consent_bounds_and_expiry();
    test_revocation_channel();
    test_unavailability_fails_closed();
    test_render_and_metadata();
    test_audit_ring_is_bounded();

    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "the support session path performs no allocation");

    if (failures != 0) {
        fprintf(stderr,
                "Appliance support session host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Appliance support session host tests: ALL OK\n");
    return 0;
}

#endif
