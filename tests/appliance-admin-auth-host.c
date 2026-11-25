#include "appliance_admin_auth.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_ADMIN_AUTH_HOST_TEST

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

void *kmalloc(size_t size)
{
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr)
{
    (void)ptr;
    ++free_attempts;
    return false;
}

static void fill_bytes(uint8_t *bytes, uint8_t value, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = value;
    }
}

static struct appliance_admin_auth_credential make_credential(
    uint32_t version,
    uint8_t value)
{
    struct appliance_admin_auth_credential credential;

    memset(&credential, 0, sizeof(credential));
    credential.version = version;
    fill_bytes(credential.verifier, value,
               sizeof(credential.verifier));
    return credential;
}

static struct appliance_admin_auth_proof make_proof(
    uint32_t version,
    uint8_t value)
{
    struct appliance_admin_auth_proof proof;

    memset(&proof, 0, sizeof(proof));
    proof.version = version;
    fill_bytes(proof.proof, value, sizeof(proof.proof));
    return proof;
}

static struct appliance_admin_auth make_auth(void)
{
    struct appliance_admin_auth auth;

    memset(&auth, 0xA5, sizeof(auth));
    if (appliance_admin_auth_init(&auth) != APPLIANCE_ADMIN_AUTH_OK) {
        fprintf(stderr, "fixture failed admin auth init\n");
        __builtin_unreachable();
    }
    return auth;
}

static void make_token(uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
                       uint8_t value)
{
    fill_bytes(token, value, APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);
}

static void make_header(uint8_t *header,
                        const uint8_t token[
                            APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH])
{
    memcpy(header, APPLIANCE_ADMIN_AUTH_BEARER_PREFIX,
           APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH);
    memcpy(header + APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH,
           token,
           APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);
}

static const char *expected_status_string(
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

static void test_strings(void)
{
    enum appliance_admin_auth_status status;
    enum appliance_admin_auth_state state;

    CHECK(sizeof(APPLIANCE_ADMIN_AUTH_BEARER_PREFIX) - 1U ==
              APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH,
          "admin auth bearer prefix length matches the exact header literal");
    for (status = APPLIANCE_ADMIN_AUTH_OK;
         status <= APPLIANCE_ADMIN_AUTH_GENERATION_EXHAUSTED;
         ++status) {
        const char *string =
            appliance_admin_auth_status_string(status);
        const char *expected = expected_status_string(status);

        CHECK(string != NULL && expected != NULL &&
                  strcmp(string, expected) == 0,
              "admin auth status string is exact for every status");
    }
    CHECK(appliance_admin_auth_status_string(
              (enum appliance_admin_auth_status)20U) == NULL,
          "admin auth undefined status string is rejected");

    for (state = APPLIANCE_ADMIN_AUTH_STATE_ABSENT;
         state <= APPLIANCE_ADMIN_AUTH_STATE_RESET_PENDING;
         ++state) {
        CHECK(appliance_admin_auth_state_string(state) != NULL,
              "admin auth state string is present for every state");
    }
    CHECK(appliance_admin_auth_state_string(
              (enum appliance_admin_auth_state)5U) == NULL,
          "admin auth undefined state string is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "admin auth string handling performs no allocation");
}

static void test_lifecycle(void)
{
    struct appliance_admin_auth auth = make_auth();
    struct appliance_admin_auth_credential first =
        make_credential(APPLIANCE_ADMIN_CREDENTIAL_VERSION, 0x11U);
    struct appliance_admin_auth_credential bad_version =
        make_credential(APPLIANCE_ADMIN_CREDENTIAL_VERSION + 1U, 0x11U);
    struct appliance_admin_auth_credential all_zero =
        make_credential(APPLIANCE_ADMIN_CREDENTIAL_VERSION, 0x00U);

    CHECK(auth.version == APPLIANCE_ADMIN_AUTH_VERSION &&
              auth.state == APPLIANCE_ADMIN_AUTH_STATE_ABSENT &&
              !auth.credential_published &&
              auth.credential_generation == 0U &&
              auth.session_count == 0U &&
              auth.next_session_id == 1U,
          "admin auth init publishes exact absent state");
    CHECK(appliance_admin_auth_init(NULL) == APPLIANCE_ADMIN_AUTH_NULL,
          "admin auth init rejects null auth");

    CHECK(appliance_admin_auth_enable(&auth) ==
              APPLIANCE_ADMIN_AUTH_BAD_STATE,
          "absent auth cannot enable");
    CHECK(appliance_admin_auth_disable(&auth) ==
              APPLIANCE_ADMIN_AUTH_BAD_STATE,
          "absent auth cannot disable");
    CHECK(appliance_admin_auth_reset(&auth) ==
              APPLIANCE_ADMIN_AUTH_CREDENTIAL_ABSENT,
          "absent auth reset is rejected");

    CHECK(appliance_admin_auth_credential_publish(
              &auth, &bad_version) == APPLIANCE_ADMIN_AUTH_BAD_CREDENTIAL,
          "admin auth rejects bad credential version");
    CHECK(appliance_admin_auth_credential_publish(
              &auth, &all_zero) == APPLIANCE_ADMIN_AUTH_BAD_CREDENTIAL,
          "admin auth rejects all-zero credential verifier");
    CHECK(appliance_admin_auth_credential_publish(
              &auth, &first) == APPLIANCE_ADMIN_AUTH_OK &&
              auth.state == APPLIANCE_ADMIN_AUTH_STATE_INITIALIZED &&
              auth.credential_published &&
              auth.credential_generation == 1U &&
              auth.credential.version ==
                  APPLIANCE_ADMIN_CREDENTIAL_VERSION &&
              memcmp(auth.credential.verifier,
                     first.verifier,
                     sizeof(first.verifier)) == 0,
          "admin auth credential publish enters initialized state");
    CHECK(appliance_admin_auth_credential_publish(
              &auth, &first) == APPLIANCE_ADMIN_AUTH_BAD_STATE,
          "admin auth rejects repeated credential publish");

    CHECK(appliance_admin_auth_enable(&auth) ==
              APPLIANCE_ADMIN_AUTH_OK &&
              auth.state == APPLIANCE_ADMIN_AUTH_STATE_ACTIVE,
          "admin auth enable enters active state");
    CHECK(appliance_admin_auth_disable(&auth) ==
              APPLIANCE_ADMIN_AUTH_OK &&
              auth.state == APPLIANCE_ADMIN_AUTH_STATE_DISABLED &&
              auth.credential_generation == 2U,
          "admin auth disable enters disabled state and advances generation");
    CHECK(appliance_admin_auth_enable(&auth) ==
              APPLIANCE_ADMIN_AUTH_OK &&
              auth.state == APPLIANCE_ADMIN_AUTH_STATE_ACTIVE,
          "admin auth disabled credential can be re-enabled");
    CHECK(appliance_admin_auth_reset(&auth) ==
              APPLIANCE_ADMIN_AUTH_OK &&
              auth.state == APPLIANCE_ADMIN_AUTH_STATE_RESET_PENDING &&
              !auth.credential_published &&
              auth.credential_generation == 2U,
          "admin auth reset enters reset-pending and clears credential");

    CHECK(appliance_admin_auth_credential_publish(
              &auth, &first) == APPLIANCE_ADMIN_AUTH_OK &&
              auth.credential_generation == 3U,
          "admin auth reinitializes after reset");
}

static void test_local_auth_and_request(void)
{
    struct appliance_admin_auth auth = make_auth();
    struct appliance_admin_auth_credential credential =
        make_credential(APPLIANCE_ADMIN_CREDENTIAL_VERSION, 0x22U);
    struct appliance_admin_auth_proof proof =
        make_proof(APPLIANCE_ADMIN_CREDENTIAL_VERSION, 0x22U);
    struct appliance_admin_auth_proof wrong_proof =
        make_proof(APPLIANCE_ADMIN_CREDENTIAL_VERSION, 0x23U);
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t zero_token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t malformed[sizeof(header)];
    struct appliance_admin_auth_decision decision;
    uint32_t session_id = 0U;

    make_token(token, 0x5AU);
    make_header(header, token);
    memset(zero_token, 0, sizeof(zero_token));

    CHECK(appliance_admin_auth_authenticate_local(
              &auth, &proof, token, 100U, 100U, &session_id) ==
              APPLIANCE_ADMIN_AUTH_BAD_STATE,
          "local auth rejects absent auth");
    CHECK(appliance_admin_auth_authenticate_local(
              NULL, &proof, token, 100U, 100U, &session_id) ==
              APPLIANCE_ADMIN_AUTH_NULL,
          "local auth rejects null auth");
    CHECK(appliance_admin_auth_authenticate_local(
              &auth, NULL, token, 100U, 100U, &session_id) ==
              APPLIANCE_ADMIN_AUTH_NULL,
          "local auth rejects null proof");
    CHECK(appliance_admin_auth_authenticate_local(
              &auth, &proof, NULL, 100U, 100U, &session_id) ==
              APPLIANCE_ADMIN_AUTH_NULL,
          "local auth rejects null token");

    CHECK(appliance_admin_auth_credential_publish(
              &auth, &credential) == APPLIANCE_ADMIN_AUTH_OK &&
              appliance_admin_auth_enable(&auth) ==
                  APPLIANCE_ADMIN_AUTH_OK,
          "local auth fixture activates auth");

    CHECK(appliance_admin_auth_authenticate_local(
              &auth, &wrong_proof, token, 100U, 100U, &session_id) ==
              APPLIANCE_ADMIN_AUTH_AUTH_FAILED &&
              auth.auth_failures == 1U,
          "local auth rejects wrong proof and records one failure");
    CHECK(appliance_admin_auth_authenticate_local(
              &auth, &proof, zero_token, 100U, 100U, &session_id) ==
              APPLIANCE_ADMIN_AUTH_TOKEN_INVALID,
          "local auth rejects all-zero token input");
    memset(&session_id, 0, sizeof(session_id));
    CHECK(appliance_admin_auth_authenticate_local(
              &auth, &proof, token, 100U, 100U, &session_id) ==
              APPLIANCE_ADMIN_AUTH_OK &&
              session_id == 1U &&
              auth.session_count == 1U &&
              auth.sessions[0].id == 1U &&
              auth.sessions[0].credential_generation == 1U &&
              auth.sessions[0].opened_tick == 100U &&
              auth.sessions[0].expires_tick == 200U &&
              auth.sessions[0].state ==
                  APPLIANCE_ADMIN_AUTH_SESSION_STATE_OPEN,
          "local auth opens an exact bounded session");

    memset(&decision, 0xA5, sizeof(decision));
    CHECK(appliance_admin_auth_authenticate_request(
              &auth, header, sizeof(header), 150U, &decision) ==
              APPLIANCE_ADMIN_AUTH_OK &&
              decision.authenticated &&
              decision.principal_id == APPLIANCE_ADMIN_PRINCIPAL_ID &&
              decision.session_id == 1U &&
              decision.credential_generation == 1U,
          "exact bearer request authenticates");

    CHECK(appliance_admin_auth_authenticate_request(
              &auth, header, sizeof(header), 200U, &decision) ==
              APPLIANCE_ADMIN_AUTH_SESSION_EXPIRED &&
              !decision.authenticated,
          "bearer request fails closed at exact expiry");
    CHECK(appliance_admin_auth_authenticate_request(
              &auth, NULL, 0U, 150U, &decision) ==
              APPLIANCE_ADMIN_AUTH_HEADER_MISSING &&
              !decision.authenticated,
          "missing bearer header is rejected");

    malformed[0] = 'x';
    CHECK(appliance_admin_auth_authenticate_request(
              &auth, malformed, sizeof(malformed), 150U, &decision) ==
              APPLIANCE_ADMIN_AUTH_HEADER_MALFORMED,
          "malformed bearer prefix is rejected");
    CHECK(appliance_admin_auth_authenticate_request(
              &auth, header, sizeof(header) + 1U, 150U, &decision) ==
              APPLIANCE_ADMIN_AUTH_HEADER_MALFORMED,
          "overlong bearer header is rejected");
    uint8_t wrong_token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    make_token(wrong_token, 0x5BU);
    make_header(header, wrong_token);
    CHECK(appliance_admin_auth_authenticate_request(
              &auth, header, sizeof(header), 150U, &decision) ==
              APPLIANCE_ADMIN_AUTH_TOKEN_INVALID,
          "wrong opaque bearer token is rejected");

    CHECK(appliance_admin_auth_close_session(&auth, 1U) ==
              APPLIANCE_ADMIN_AUTH_OK &&
              auth.session_count == 0U,
          "admin auth closes session explicitly");
    CHECK(appliance_admin_auth_close_session(&auth, 1U) ==
              APPLIANCE_ADMIN_AUTH_OK,
          "admin auth close is idempotent");
    CHECK(appliance_admin_auth_close_session(&auth, 2U) ==
              APPLIANCE_ADMIN_AUTH_SESSION_INVALID,
          "admin auth rejects unknown close session id");
}

static void test_revocation_and_throttle(void)
{
    struct appliance_admin_auth auth = make_auth();
    struct appliance_admin_auth_credential credential =
        make_credential(APPLIANCE_ADMIN_CREDENTIAL_VERSION, 0x33U);
    struct appliance_admin_auth_credential replacement =
        make_credential(APPLIANCE_ADMIN_CREDENTIAL_VERSION, 0x44U);
    struct appliance_admin_auth_proof proof =
        make_proof(APPLIANCE_ADMIN_CREDENTIAL_VERSION, 0x33U);
    struct appliance_admin_auth_proof bad_proof =
        make_proof(APPLIANCE_ADMIN_CREDENTIAL_VERSION, 0x55U);
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    struct appliance_admin_auth_decision decision;
    uint32_t session_id = 0U;

    make_token(token, 0x6AU);
    make_header(header, token);
    CHECK(appliance_admin_auth_credential_publish(
              &auth, &credential) == APPLIANCE_ADMIN_AUTH_OK &&
              appliance_admin_auth_enable(&auth) ==
                  APPLIANCE_ADMIN_AUTH_OK &&
              appliance_admin_auth_authenticate_local(
                  &auth, &proof, token, 0U, 100U, &session_id) ==
                  APPLIANCE_ADMIN_AUTH_OK,
          "revocation fixture opens one session");

    CHECK(appliance_admin_auth_disable(&auth) ==
              APPLIANCE_ADMIN_AUTH_OK &&
              auth.session_count == 0U,
          "disable revokes every active session");
    CHECK(appliance_admin_auth_authenticate_request(
              &auth, header, sizeof(header), 10U, &decision) ==
              APPLIANCE_ADMIN_AUTH_BAD_STATE,
          "disabled auth denies request enforcement");

    CHECK(appliance_admin_auth_reset(&auth) ==
              APPLIANCE_ADMIN_AUTH_OK &&
              appliance_admin_auth_credential_publish(
                  &auth, &replacement) == APPLIANCE_ADMIN_AUTH_OK &&
              appliance_admin_auth_enable(&auth) ==
                  APPLIANCE_ADMIN_AUTH_OK,
          "admin auth recovery publishes replacement credential");
    CHECK(appliance_admin_auth_authenticate_request(
              &auth, header, sizeof(header), 10U, &decision) ==
              APPLIANCE_ADMIN_AUTH_TOKEN_INVALID,
          "old bearer token cannot authenticate after recovery");

    memset(&auth, 0, sizeof(auth));
    CHECK(appliance_admin_auth_init(&auth) == APPLIANCE_ADMIN_AUTH_OK &&
              appliance_admin_auth_credential_publish(
                  &auth, &credential) == APPLIANCE_ADMIN_AUTH_OK &&
              appliance_admin_auth_enable(&auth) ==
                  APPLIANCE_ADMIN_AUTH_OK,
          "throttle fixture activates auth");
    for (uint32_t attempt = 0U; attempt < 4U; ++attempt) {
        CHECK(appliance_admin_auth_authenticate_local(
                  &auth, &bad_proof, token, 0U, 100U, &session_id) ==
                  APPLIANCE_ADMIN_AUTH_AUTH_FAILED,
              "throttle counts failed local proof attempts");
    }
    CHECK(appliance_admin_auth_authenticate_local(
              &auth, &bad_proof, token, 0U, 100U, &session_id) ==
              APPLIANCE_ADMIN_AUTH_AUTH_THROTTLED &&
              auth.locked_until_tick == 100U,
          "local auth locks after the bounded failure limit");
    CHECK(appliance_admin_auth_authenticate_local(
              &auth, &proof, token, 50U, 100U, &session_id) ==
              APPLIANCE_ADMIN_AUTH_AUTH_THROTTLED,
          "locked local auth remains throttled");
}

int main(void)
{
    test_strings();
    test_lifecycle();
    test_local_auth_and_request();
    test_revocation_and_throttle();

    if (failures != 0) {
        fprintf(stderr, "Appliance admin auth host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Appliance admin auth host tests: ALL OK\n");
    return 0;
}

#else
#error "This host test must be built with HFOC_APPLIANCE_ADMIN_AUTH_HOST_TEST"
#endif
