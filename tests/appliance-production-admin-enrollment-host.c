/*
 * ITEM-401 host proof for the administrator credential enrollment owner.
 *
 * The proof runs the production module against an in-memory persistent secret
 * store and the real ITEM-365 request gate, so enrolment, fingerprint
 * checking, placeholder refusal, throttling, session issuance and the
 * authenticated request decision are all exercised without a heap.
 */

#include "appliance_admin_auth.h"
#include "appliance_management_authorization.h"
#include "appliance_persistent_secret_config.h"
#include "appliance_production_admin_enrollment.h"
#include "appliance_production_auth.h"
#include "appliance_secret_protection.h"
#include "block_device.h"
#include "config_journal.h"
#include "data_region.h"
#include "http_parser.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_HOST_TEST

#define TEST_BLOCK_SIZE 512U
#define TEST_BLOCK_COUNT 8U
#define TEST_CAPACITY (TEST_BLOCK_SIZE * TEST_BLOCK_COUNT)
#define TEST_REGION_OFFSET 1U
#define TEST_REGION_COUNT 7U

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

struct fixture_device {
    unsigned char storage[TEST_CAPACITY];
    size_t read_calls;
    size_t write_calls;
};

static void zero_bytes(void *destination, size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void copy_bytes(void *destination,
                       const void *source,
                       size_t length)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    size_t index;

    for (index = 0U; index < length; ++index) {
        out[index] = in[index];
    }
}

static void store_le64(unsigned char *bytes, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static void init_fixture(struct fixture_device *fixture)
{
    static const unsigned char magic[DATA_REGION_MAGIC_SIZE] =
        DATA_REGION_MAGIC_BYTES;

    zero_bytes(fixture, sizeof(*fixture));
    copy_bytes(fixture->storage, magic, sizeof(magic));
    fixture->storage[8U] = DATA_REGION_FORMAT_VERSION;
    store_le64(fixture->storage + 16U, TEST_REGION_OFFSET);
    store_le64(fixture->storage + 24U, TEST_REGION_COUNT);
}

static enum block_device_status fixture_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct fixture_device *fixture =
        (struct fixture_device *)device->context;

    ++fixture->read_calls;
    copy_bytes(buffer, fixture->storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct fixture_device *fixture =
        (struct fixture_device *)device->context;

    ++fixture->write_calls;
    copy_bytes(fixture->storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static struct block_device make_device(struct fixture_device *fixture)
{
    struct block_device device;

    zero_bytes(&device, sizeof(device));
    device.block_size = TEST_BLOCK_SIZE;
    device.block_count = TEST_BLOCK_COUNT;
    device.context = fixture;
    device.read_fn = fixture_read;
    device.write_fn = fixture_write;
    return device;
}

static bool contains_bytes(const void *haystack,
                           size_t haystack_length,
                           const char *needle)
{
    const unsigned char *bytes = haystack;
    size_t needle_length = strlen(needle);
    size_t index;

    if (needle_length == 0U || needle_length > haystack_length) {
        return false;
    }
    for (index = 0U; index + needle_length <= haystack_length; ++index) {
        if (memcmp(bytes + index, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static bool derive_fingerprint(const char *secret,
                               uint8_t fingerprint[SHA256_DIGEST_SIZE])
{
    return appliance_production_admin_enrollment_credential_fingerprint(
               secret, strlen(secret), fingerprint) ==
           APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK;
}

static bool factory_fingerprint(uint8_t fingerprint[SHA256_DIGEST_SIZE])
{
    return appliance_secret_protection_fingerprint(
               APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL,
               APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_GENERATION,
               appliance_production_admin_enrollment_factory_credential,
               APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_FACTORY_CREDENTIAL_LENGTH,
               fingerprint) == APPLIANCE_SECRET_PROTECTION_OK;
}

static enum appliance_persistent_secret_config_status load_secret_store(
    struct appliance_persistent_secret_config *config,
    struct block_device *device)
{
    zero_bytes(config, sizeof(*config));
    return appliance_persistent_secret_config_load(config, device);
}

static bool commit_fingerprint(
    struct appliance_persistent_secret_config *config,
    const uint8_t fingerprint[SHA256_DIGEST_SIZE])
{
    return appliance_persistent_secret_config_commit_admin(
               config, fingerprint) ==
           APPLIANCE_PERSISTENT_SECRET_CONFIG_OK;
}

static bool build_bearer_header(
    const uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    uint8_t header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH])
{
    copy_bytes(header, APPLIANCE_ADMIN_AUTH_BEARER_PREFIX,
               APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH);
    copy_bytes(header + APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH,
               token, APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH);
    return true;
}

static enum appliance_production_auth_status authorize_system(
    struct appliance_production_auth *auth,
    const uint8_t *header,
    uint32_t header_length,
    uint64_t now_tick)
{
    static const char target[] = "/api/v1/system";
    struct appliance_admin_auth_decision decision;
    enum appliance_management_authorization_route route =
        APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_NONE;

    zero_bytes(&decision, sizeof(decision));
    return appliance_production_auth_authorize_request(
        auth, header, header_length, HTTP_METHOD_GET,
        (const uint8_t *)target, (uint32_t)(sizeof(target) - 1U),
        now_tick, &decision, &route);
}

static void test_status_strings(void)
{
    static const enum appliance_production_admin_enrollment_status statuses[] = {
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NULL,
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_STATE,
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_VERSION,
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_ALREADY_INITIALIZED,
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNAVAILABLE,
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_SECRET,
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNPROVISIONED,
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED,
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_THROTTLED,
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_PUBLISH_FAILED,
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_SESSION_FAILED,
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BUFFER_TOO_SMALL,
    };
    static const enum appliance_production_admin_enrollment_state states[] = {
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_NONE,
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNINITIALIZED,
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNAVAILABLE,
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNPROVISIONED,
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_AWAITING_SECRET,
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_ACTIVE,
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_FAILED,
    };
    size_t status_index;
    size_t state_index;

    for (status_index = 0U;
         status_index < sizeof(statuses) / sizeof(statuses[0]);
         ++status_index) {
        const char *text =
            appliance_production_admin_enrollment_status_string(
                statuses[status_index]);
        size_t other;

        CHECK(text != NULL && text[0] != '\0',
              "every enrollment status renders a non-empty string");
        for (other = status_index + 1U;
             other < sizeof(statuses) / sizeof(statuses[0]);
             ++other) {
            CHECK(strcmp(
                      text,
                      appliance_production_admin_enrollment_status_string(
                          statuses[other])) != 0,
                  "enrollment status strings are distinct");
        }
    }
    for (state_index = 0U;
         state_index < sizeof(states) / sizeof(states[0]);
         ++state_index) {
        const char *text =
            appliance_production_admin_enrollment_state_string(
                states[state_index]);

        CHECK(text != NULL && text[0] != '\0',
              "every enrollment state renders a non-empty string");
    }
}

static void test_init_and_provisionable(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_secret_config secrets;
    struct appliance_production_auth auth;
    struct appliance_production_admin_enrollment enrollment;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_secret_store(&secrets, &device) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "an empty secret store loads");
    zero_bytes(&auth, sizeof(auth));
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "the request gate initializes");
    zero_bytes(&enrollment, sizeof(enrollment));
    CHECK(appliance_production_admin_enrollment_init(
              &enrollment, &auth, &secrets) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the enrollment owner initializes over an empty store");
    CHECK(appliance_production_admin_enrollment_state(&enrollment) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNPROVISIONED,
          "an appliance with no credential record is provisionable");
    CHECK(!appliance_production_admin_enrollment_published(&enrollment),
          "an unprovisioned owner never publishes the gate");
    CHECK(!appliance_production_auth_available(&auth),
          "the gate stays unavailable without a credential");
    CHECK(appliance_production_admin_enrollment_authenticate(
              &enrollment, "operator-secret", 15U, 100U,
              APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, NULL, NULL) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NULL,
          "a null token pointer is refused");
    {
        uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
        uint32_t session_id = 0U;

        zero_bytes(token, sizeof(token));
        CHECK(appliance_production_admin_enrollment_authenticate(
                  &enrollment, "operator-secret", 15U, 100U,
                  APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX,
                  token, &session_id) ==
                  APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNPROVISIONED,
              "authenticating before provisioning is refused");
    }
    CHECK(appliance_production_admin_enrollment_provision(
              &enrollment, "operator-secret", 15U) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "an absent record can be provisioned");
    CHECK(appliance_production_admin_enrollment_state(&enrollment) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_ACTIVE &&
              appliance_production_admin_enrollment_published(&enrollment) &&
              appliance_production_auth_available(&auth),
          "provisioning an absent record publishes the gate");
    CHECK(appliance_production_admin_enrollment_init(
              &enrollment, &auth, &secrets) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_ALREADY_INITIALIZED,
          "a second init is refused");
}

static void test_unavailable_without_store(void)
{
    struct appliance_production_auth auth;
    struct appliance_production_admin_enrollment enrollment;
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t session_id = 0U;

    zero_bytes(&auth, sizeof(auth));
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "the gate initializes without a secret store");
    zero_bytes(&enrollment, sizeof(enrollment));
    CHECK(appliance_production_admin_enrollment_init(
              &enrollment, &auth, NULL) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the owner initializes without a secret store");
    CHECK(appliance_production_admin_enrollment_state(&enrollment) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNAVAILABLE,
          "a missing secret store stays UNAVAILABLE");
    zero_bytes(token, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate(
              &enrollment, "operator-secret", 15U, 100U,
              APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX,
              token, &session_id) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNAVAILABLE,
          "authenticating without a store fails unavailable");
    CHECK(appliance_production_admin_enrollment_provision(
              &enrollment, "operator-secret", 15U) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNAVAILABLE,
          "provisioning without a store fails unavailable");
    CHECK(!appliance_production_admin_enrollment_published(&enrollment),
          "a storeless owner never publishes the gate");
}

static void test_factory_placeholder_is_never_a_credential(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_secret_config secrets;
    struct appliance_production_auth auth;
    struct appliance_production_admin_enrollment enrollment;
    uint8_t fingerprint[SHA256_DIGEST_SIZE];
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t session_id = 0U;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_secret_store(&secrets, &device) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "the factory secret store loads");
    CHECK(factory_fingerprint(fingerprint),
          "the factory fingerprint derives");
    CHECK(commit_fingerprint(&secrets, fingerprint),
          "the factory fingerprint commits");
    zero_bytes(&auth, sizeof(auth));
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "the gate initializes over the factory record");
    zero_bytes(&enrollment, sizeof(enrollment));
    CHECK(appliance_production_admin_enrollment_init(
              &enrollment, &auth, &secrets) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the enrollment owner initializes over the factory record");
    CHECK(appliance_production_admin_enrollment_state(&enrollment) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNPROVISIONED,
          "the factory record is published as UNPROVISIONED");
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate(
              &enrollment, "HFOC-APPLIANCE-V1-PROVISIONING-1", 32U,
              100U, APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX,
              token, &session_id) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNPROVISIONED,
          "the factory credential is never accepted as an authenticating secret");
    CHECK(!appliance_production_admin_enrollment_published(&enrollment),
          "the factory record never publishes the gate");
    CHECK(!appliance_production_auth_available(&auth),
          "the gate stays unavailable on the factory record");
}

static void test_provision_then_authenticate(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_secret_config secrets;
    struct appliance_production_auth auth;
    struct appliance_production_admin_enrollment enrollment;
    uint8_t factory[SHA256_DIGEST_SIZE];
    uint8_t expected[SHA256_DIGEST_SIZE];
    uint8_t stored[SHA256_DIGEST_SIZE];
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t session_id = 0U;
    size_t index;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_secret_store(&secrets, &device) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "the factory store loads before provisioning");
    CHECK(factory_fingerprint(factory), "the factory fingerprint derives");
    CHECK(commit_fingerprint(&secrets, factory), "the factory record commits");
    zero_bytes(&auth, sizeof(auth));
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "the gate initializes before provisioning");
    zero_bytes(&enrollment, sizeof(enrollment));
    CHECK(appliance_production_admin_enrollment_init(
              &enrollment, &auth, &secrets) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the owner initializes before provisioning");
    CHECK(appliance_production_admin_enrollment_provision(
              &enrollment, "operator-secret", 15U) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "provisioning from the factory record succeeds");
    CHECK(appliance_production_admin_enrollment_state(&enrollment) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_ACTIVE,
          "provisioning publishes the owner ACTIVE");
    CHECK(appliance_production_admin_enrollment_published(&enrollment),
          "provisioning publishes the gate");
    CHECK(appliance_production_auth_available(&auth),
          "the request gate reports an available credential");
    CHECK(derive_fingerprint("operator-secret", expected),
          "the operator fingerprint derives");
    CHECK(appliance_persistent_secret_config_admin(&secrets, stored),
          "the committed record reads back");
    CHECK(memcmp(expected, stored, SHA256_DIGEST_SIZE) == 0,
          "the operator fingerprint replaced the factory record");
    CHECK(appliance_production_admin_enrollment_provision(
              &enrollment, "second-secret", 13U) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED,
          "a second provisioning is refused");
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate(
              &enrollment, "wrong-secret", 12U, 200U,
              APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX,
              token, &session_id) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED,
          "a wrong secret is denied after provisioning");
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate(
              &enrollment, "operator-secret", 15U, 200U,
              APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX,
              token, &session_id) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the provisioned secret issues a session");
    CHECK(session_id != 0U, "the session id is published");
    for (index = 0U; index < sizeof(token); ++index) {
        CHECK(token[index] >= 0x21U && token[index] <= 0x7eU,
              "every bearer token byte is header-safe");
    }
    CHECK(build_bearer_header(token, header), "the bearer header builds");
    CHECK(authorize_system(&auth, header, sizeof(header), 200U) ==
              APPLIANCE_PRODUCTION_AUTH_ALLOWED,
          "the issued session authorizes the read-only system target");
    CHECK(authorize_system(&auth, NULL, 0U, 200U) ==
              APPLIANCE_PRODUCTION_AUTH_UNAVAILABLE ||
              authorize_system(&auth, NULL, 0U, 200U) !=
                  APPLIANCE_PRODUCTION_AUTH_ALLOWED,
          "an anonymous request stays denied");
    CHECK(!contains_bytes(&enrollment, sizeof(enrollment), "operator-secret"),
          "the owner retains no plaintext secret");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "enrollment performs no heap allocation");
}

/*
 * ITEM-465: the reset restores the deferred first-boot state, closes every
 * issued session and lets a new secret be enrolled.
 *
 * The owner forgot the administrator secret and had no way back: provision
 * refuses once a real record exists.  The reset must rewrite the persisted
 * record to the factory placeholder (so a later boot reads UNPROVISIONED),
 * close the sessions the operator already opened, and leave the gate closed
 * until a new secret is presented.
 */
static void test_reset_restores_provisionable(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_secret_config secrets;
    struct appliance_production_auth auth;
    struct appliance_production_admin_enrollment enrollment;
    uint8_t factory[SHA256_DIGEST_SIZE];
    uint8_t stored[SHA256_DIGEST_SIZE];
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                   APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t new_token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t session_id = 0U;
    size_t index;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_secret_store(&secrets, &device) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "the factory store loads before the reset proof");
    CHECK(factory_fingerprint(factory), "the factory fingerprint derives");
    CHECK(commit_fingerprint(&secrets, factory), "the factory record commits");
    zero_bytes(&auth, sizeof(auth));
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "the gate initializes before the reset proof");
    zero_bytes(&enrollment, sizeof(enrollment));
    CHECK(appliance_production_admin_enrollment_init(
              &enrollment, &auth, &secrets) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the owner initializes before the reset proof");
    CHECK(appliance_production_admin_enrollment_provision(
              &enrollment, "forgotten-secret", 16U) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "a secret is enrolled before it is forgotten");
    CHECK(appliance_production_admin_enrollment_authenticate(
              &enrollment, "forgotten-secret", 16U, 200U,
              APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX,
              token, &session_id) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the forgotten secret opens a session first");
    CHECK(build_bearer_header(token, header), "the session header builds");
    CHECK(authorize_system(&auth, header, sizeof(header), 200U) ==
              APPLIANCE_PRODUCTION_AUTH_ALLOWED,
          "the open session authorizes before the reset");

    CHECK(appliance_production_admin_enrollment_reset(&enrollment) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the local reset succeeds");
    CHECK(appliance_production_admin_enrollment_state(&enrollment) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNPROVISIONED,
          "the reset publishes the owner UNPROVISIONED");
    CHECK(!appliance_production_admin_enrollment_published(&enrollment),
          "the reset closes the gate");
    CHECK(!appliance_production_auth_available(&auth),
          "the gate holds no credential after the reset");
    CHECK(appliance_persistent_secret_config_admin(&secrets, stored),
          "the rewritten record reads back");
    CHECK(memcmp(factory, stored, SHA256_DIGEST_SIZE) == 0,
          "the reset restored the factory placeholder");
    CHECK(authorize_system(&auth, header, sizeof(header), 200U) !=
              APPLIANCE_PRODUCTION_AUTH_ALLOWED,
          "the session issued before the reset no longer authorizes");
    {
        bool token_nonzero = false;

        for (index = 0U; index < sizeof(token); ++index) {
            if (token[index] != 0U) {
                token_nonzero = true;
                break;
            }
        }
        CHECK(token_nonzero, "the issued session token was real before the reset");
    }

    CHECK(appliance_production_admin_enrollment_provision(
              &enrollment, "replacement-secret", 18U) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "a new secret enrolls after the reset");
    CHECK(appliance_production_admin_enrollment_state(&enrollment) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_ACTIVE,
          "the replacement secret publishes the owner ACTIVE");
    memset(new_token, 0, sizeof(new_token));
    session_id = 0U;
    CHECK(appliance_production_admin_enrollment_authenticate(
              &enrollment, "replacement-secret", 18U, 400U,
              APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX,
              new_token, &session_id) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the replacement secret opens a session");
    CHECK(appliance_production_admin_enrollment_reset(&enrollment) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "a second reset is still accepted");
    CHECK(appliance_production_admin_enrollment_state(&enrollment) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNPROVISIONED,
          "the second reset returns to UNPROVISIONED");
    CHECK(!contains_bytes(&enrollment, sizeof(enrollment), "forgotten-secret"),
          "the reset retains no plaintext secret");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "the reset performs no heap allocation");
}

static void test_enrollment_from_committed_record(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_secret_config secrets;
    struct appliance_production_auth auth;
    struct appliance_production_admin_enrollment enrollment;
    uint8_t expected[SHA256_DIGEST_SIZE];
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t session_id = 0U;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_secret_store(&secrets, &device) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "a provisioned store loads");
    CHECK(derive_fingerprint("provisioned-secret", expected),
          "the provisioned fingerprint derives");
    CHECK(commit_fingerprint(&secrets, expected),
          "the provisioned fingerprint commits");
    zero_bytes(&auth, sizeof(auth));
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "the gate initializes over the provisioned record");
    zero_bytes(&enrollment, sizeof(enrollment));
    CHECK(appliance_production_admin_enrollment_init(
              &enrollment, &auth, &secrets) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the owner initializes over the provisioned record");
    CHECK(appliance_production_admin_enrollment_state(&enrollment) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_AWAITING_SECRET,
          "a provisioned appliance awaits the local secret");
    CHECK(!appliance_production_admin_enrollment_published(&enrollment),
          "the gate is not published before the secret is presented");
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate(
              &enrollment, "provisioned-secret", 18U, 300U,
              APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX,
              token, &session_id) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the persisted record enrolls the verifier");
    CHECK(appliance_production_admin_enrollment_published(&enrollment),
          "enrollment publishes the gate");
    CHECK(appliance_production_admin_enrollment_state(&enrollment) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_ACTIVE,
          "enrollment reports ACTIVE");
    CHECK(appliance_production_auth_available(&auth),
          "the gate becomes available after enrollment");
}

static void test_throttling(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_secret_config secrets;
    struct appliance_production_auth auth;
    struct appliance_production_admin_enrollment enrollment;
    uint8_t expected[SHA256_DIGEST_SIZE];
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint32_t session_id = 0U;
    uint32_t attempt;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_secret_store(&secrets, &device) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "the throttling store loads");
    CHECK(derive_fingerprint("throttle-secret", expected),
          "the throttling fingerprint derives");
    CHECK(commit_fingerprint(&secrets, expected),
          "the throttling fingerprint commits");
    zero_bytes(&auth, sizeof(auth));
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "the gate initializes for throttling");
    zero_bytes(&enrollment, sizeof(enrollment));
    CHECK(appliance_production_admin_enrollment_init(
              &enrollment, &auth, &secrets) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the owner initializes for throttling");
    for (attempt = 0U;
         attempt + 1U <
             APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_FAILURE_LIMIT;
         ++attempt) {
        memset(token, 0, sizeof(token));
        CHECK(appliance_production_admin_enrollment_authenticate(
                  &enrollment, "wrong", 5U, 400U,
                  APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX,
                  token, &session_id) ==
                  APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED,
              "a wrong secret is denied");
    }
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate(
              &enrollment, "wrong", 5U, 400U,
              APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX,
              token, &session_id) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_THROTTLED,
          "the failure limit throttles the attempt that reaches it");
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate(
              &enrollment, "throttle-secret", 15U, 400U,
              APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX,
              token, &session_id) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_THROTTLED,
          "the correct secret is throttled during cooldown");
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate(
              &enrollment, "throttle-secret", 15U,
              400U + APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_COOLDOWN_TICKS,
              APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX,
              token, &session_id) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the correct secret works after the cooldown");
}

/*
 * ITEM-506: the login+password path.  The enrolled account is one structured
 * record; the old secret-only path refuses to guess once that record exists,
 * a legacy record is refused as a credential and migrated only by an explicit
 * provisioning call, and the five-failure lockout covers the new path too.
 */
static bool credential_random_fill(
    void *context,
    uint8_t *output,
    size_t length)
{
    uint8_t *state = context;
    size_t index;

    if (state == NULL || output == NULL || length == 0U) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        *state = (uint8_t)((uint8_t)(*state * 29U) + 17U);
        output[index] = *state;
    }
    return true;
}

static void test_credential_path(void)
{
    static const char login[] = "operator";
    static const char password[] = "correct horse battery staple";
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_secret_config secrets;
    struct appliance_production_auth auth;
    struct appliance_production_admin_enrollment enrollment;
    struct appliance_admin_credential stored;
    uint8_t legacy[32];
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t random_state = 0x5aU;
    uint32_t session_id = 0U;
    uint32_t attempt;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_secret_store(&secrets, &device) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "the credential store loads");
    zero_bytes(&auth, sizeof(auth));
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "the gate initializes for the credential path");
    zero_bytes(&enrollment, sizeof(enrollment));
    CHECK(appliance_production_admin_enrollment_init(
              &enrollment, &auth, &secrets) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the owner initializes for the credential path");
    CHECK(appliance_production_admin_enrollment_state(&enrollment) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNPROVISIONED,
          "an empty store is provisionable");

    /* No salt source: nothing is written and the refusal is named. */
    CHECK(appliance_production_admin_enrollment_provision_credential(
              &enrollment, login, strlen(login), password, strlen(password),
              100U) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NO_RANDOM,
          "provisioning without a salt source is refused");
    CHECK(appliance_production_admin_enrollment_credential_set(&enrollment) ==
              false,
          "a refused provisioning writes no credential");
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate(
              &enrollment, password, strlen(password), 100U,
              APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, token,
              &session_id) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNPROVISIONED,
          "a refused provisioning leaves the appliance unprovisioned");

    CHECK(appliance_production_admin_enrollment_set_random(
              &enrollment, credential_random_fill, &random_state) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the salt source binds");
    CHECK(appliance_production_admin_enrollment_provision_credential(
              &enrollment, "bad login", 9U, password, strlen(password),
              100U) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_LOGIN,
          "a login outside the policy is refused");
    CHECK(appliance_production_admin_enrollment_provision_credential(
              &enrollment, login, strlen(login), "password", 8U, 100U) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_WEAK_PASSWORD,
          "a weak password is refused by name");
    CHECK(appliance_production_admin_enrollment_provision_credential(
              &enrollment, login, strlen(login), "short", 5U, 100U) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_SECRET,
          "a short password is refused");
    CHECK(appliance_production_admin_enrollment_credential_set(&enrollment) ==
              false,
          "the policy refusals wrote nothing");

    CHECK(appliance_production_admin_enrollment_provision_credential(
              &enrollment, login, strlen(login), password, strlen(password),
              100U) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the credential is set");
    CHECK(appliance_production_admin_enrollment_credential_set(&enrollment),
          "the structured credential is the head");
    CHECK(appliance_production_admin_enrollment_state(&enrollment) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_ACTIVE,
          "setting the credential publishes the gate");
    zero_bytes(&stored, sizeof(stored));
    CHECK(appliance_persistent_secret_config_admin_credential(
              &secrets, &stored) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "the committed credential reads back from the store");
    CHECK(strcmp(stored.login, login) == 0 &&
          stored.iterations ==
              APPLIANCE_ADMIN_CREDENTIAL_ITERATIONS_DEFAULT,
          "the record carries the login and the iteration count");
    CHECK(stored.salt[0] == 0x43U,
          "the salt comes from the bound source");

    memset(token, 0, sizeof(token));
    session_id = 0U;
    CHECK(appliance_production_admin_enrollment_authenticate_credential(
              &enrollment, login, strlen(login), password, strlen(password),
              200U, APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, token,
              &session_id) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the right login and password open a session");
    CHECK(session_id != 0U, "the session id is published");
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate_credential(
              &enrollment, login, strlen(login), "wrong password!", 15U,
              200U, APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, token,
              &session_id) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED,
          "a wrong password is denied");
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate_credential(
              &enrollment, "someoneelse", 11U, password, strlen(password),
              200U, APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, token,
              &session_id) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED,
          "a wrong login is denied");

    /* The same five-failure limit as the legacy path. */
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate_credential(
              &enrollment, login, strlen(login), password, strlen(password),
              300U, APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, token,
              &session_id) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "a success resets the failure count before the lockout drill");
    for (attempt = 0U;
         attempt + 1U <
             APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_FAILURE_LIMIT;
         ++attempt) {
        memset(token, 0, sizeof(token));
        CHECK(appliance_production_admin_enrollment_authenticate_credential(
                  &enrollment, login, strlen(login), "wrong password!", 15U,
                  300U, APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, token,
                  &session_id) ==
                  APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED,
              "a repeated wrong password is denied until the limit");
    }
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate_credential(
              &enrollment, login, strlen(login), "wrong password!", 15U,
              300U, APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, token,
              &session_id) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_THROTTLED,
          "the fifth failure locks the account");
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate_credential(
              &enrollment, login, strlen(login), password, strlen(password),
              300U, APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, token,
              &session_id) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_THROTTLED,
          "a locked account refuses even the right password");
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate_credential(
              &enrollment, login, strlen(login), password, strlen(password),
              300U + APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_COOLDOWN_TICKS,
              APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, token,
              &session_id) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the right password works after the cooldown");

    /* The legacy path must not silently downgrade the account. */
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate(
              &enrollment, password, strlen(password), 400U,
              APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, token,
              &session_id) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_CREDENTIAL_REQUIRED,
          "the legacy authenticate refuses a structured credential");
    CHECK(appliance_production_admin_enrollment_provision(
              &enrollment, password, strlen(password)) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_CREDENTIAL_REQUIRED,
          "the legacy provision refuses a structured credential");

    /* A legacy record is refused as a credential and migrated explicitly. */
    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_secret_store(&secrets, &device) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "the migration store loads");
    CHECK(derive_fingerprint("legacy-secret", legacy),
          "the legacy fingerprint derives");
    CHECK(commit_fingerprint(&secrets, legacy),
          "the legacy fingerprint commits");
    zero_bytes(&auth, sizeof(auth));
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "the gate initializes for migration");
    zero_bytes(&enrollment, sizeof(enrollment));
    CHECK(appliance_production_admin_enrollment_init(
              &enrollment, &auth, &secrets) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the owner initializes for migration");
    CHECK(appliance_production_admin_enrollment_set_random(
              &enrollment, credential_random_fill, &random_state) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the salt source binds for migration");
    CHECK(appliance_production_admin_enrollment_credential_set(&enrollment) ==
              false,
          "a legacy record is not a structured credential");
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate_credential(
              &enrollment, login, strlen(login), password, strlen(password),
              500U, APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, token,
              &session_id) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_LEGACY_RECORD,
          "a legacy record refuses the credential path by name");
    /*
     * ITEM-506: the verify-only entry answers LEGACY_RECORD too, so the change
     * conversation cannot mistake a real legacy secret for an unset account
     * and overwrite it without proof.
     */
    CHECK(appliance_production_admin_enrollment_verify_credential(
              &enrollment, password, strlen(password), 500U) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_LEGACY_RECORD,
          "the verify step refuses a legacy record by name");
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate(
              &enrollment, "legacy-secret", 13U, 500U,
              APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, token,
              &session_id) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the legacy path still serves the legacy record");
    CHECK(appliance_production_admin_enrollment_provision_credential(
              &enrollment, login, strlen(login), password, strlen(password),
              500U) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "an explicit provisioning migrates the legacy record");
    CHECK(appliance_production_admin_enrollment_credential_set(&enrollment),
          "the migrated head is the structured credential");
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate(
              &enrollment, "legacy-secret", 13U, 500U,
              APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, token,
              &session_id) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_CREDENTIAL_REQUIRED,
          "the legacy secret is refused after the migration");

    /* The break-glass reset returns the appliance to the unprovisioned state. */
    CHECK(appliance_production_admin_enrollment_reset(&enrollment) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the reset rewrites the factory placeholder");
    CHECK(appliance_production_admin_enrollment_credential_set(&enrollment) ==
              false,
          "the reset removes the structured credential");
    CHECK(appliance_production_admin_enrollment_state(&enrollment) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_STATE_UNPROVISIONED,
          "the reset returns the unprovisioned state");
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate_credential(
              &enrollment, login, strlen(login), password, strlen(password),
              600U, APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, token,
              &session_id) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNPROVISIONED,
          "an unprovisioned store refuses the credential path");
    CHECK(appliance_production_admin_enrollment_provision_credential(
              &enrollment, login, strlen(login), password, strlen(password),
              600U) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the account can be set again after the reset");
}

static void test_render(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_secret_config secrets;
    struct appliance_production_auth auth;
    struct appliance_production_admin_enrollment enrollment;
    char output[APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_RENDER_MAX];
    size_t length = 0U;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_secret_store(&secrets, &device) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "the render store loads");
    zero_bytes(&auth, sizeof(auth));
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "the gate initializes for rendering");
    zero_bytes(&enrollment, sizeof(enrollment));
    CHECK(appliance_production_admin_enrollment_init(
              &enrollment, &auth, &secrets) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the owner initializes for rendering");
    CHECK(appliance_production_admin_enrollment_render(
              &enrollment, output, sizeof(output), &length) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the owner renders a record");
    CHECK(length != 0U && length < sizeof(output),
          "the rendered record is bounded");
    CHECK(strstr(output, "[ADMIN-ENROLLMENT]") != NULL,
          "the record names the owner");
    CHECK(strstr(output, "state=UNPROVISIONED") != NULL,
          "the record publishes the honest state");
    /*
     * ITEM-460: the record names the last refusal, so the durable journal
     * explains a lost console answer without a photograph.  A fresh owner has
     * refused nothing yet.
     */
    CHECK(strstr(output, "last_refusal=NONE") != NULL,
          "the record says when nothing has been refused yet");
    CHECK(strstr(output, "denied=") != NULL,
          "the record still carries the denial counter");
    CHECK(appliance_production_admin_enrollment_render(
              &enrollment, output, 4U, &length) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BUFFER_TOO_SMALL,
          "a too-small render buffer is refused");
}

/*
 * ITEM-506 self-review hardening: the claims the first round of proofs only
 * touched at the edge.  The whole salt comes from the bound source, a failing
 * source is a refusal and not an invented salt, the lockout cannot be shed by
 * switching entry points, an old legacy secret stays refused after the
 * migration, and an uninitialized owner answers instead of dereferencing.
 */
static bool credential_random_fail(
    void *context,
    uint8_t *output,
    size_t length)
{
    (void)context;
    (void)output;
    (void)length;
    return false;
}

static void test_credential_hardening(void)
{
    static const char login[] = "operator";
    static const char password[] = "correct horse battery staple";
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_secret_config secrets;
    struct appliance_production_auth auth;
    struct appliance_production_admin_enrollment enrollment;
    struct appliance_admin_credential stored;
    uint8_t expected_salt[APPLIANCE_ADMIN_CREDENTIAL_SALT_LENGTH];
    uint8_t legacy[32];
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t random_state = 0x5aU;
    uint8_t generator_state;
    uint32_t session_id = 0U;
    size_t index;

    /* An owner that was never initialized is answered, not dereferenced. */
    CHECK(appliance_production_admin_enrollment_credential_set(NULL) == false,
          "a NULL owner has no credential");
    zero_bytes(&enrollment, sizeof(enrollment));
    CHECK(appliance_production_admin_enrollment_credential_set(&enrollment) ==
              false,
          "an uninitialized owner has no credential");

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_secret_store(&secrets, &device) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "the hardening store loads");
    zero_bytes(&auth, sizeof(auth));
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "the gate initializes for hardening");
    zero_bytes(&enrollment, sizeof(enrollment));
    CHECK(appliance_production_admin_enrollment_init(
              &enrollment, &auth, &secrets) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the owner initializes for hardening");

    /* A source that cannot fill the buffer is a refusal, not a salt. */
    CHECK(appliance_production_admin_enrollment_set_random(
              &enrollment, credential_random_fail, NULL) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "a failing salt source still binds");
    CHECK(appliance_production_admin_enrollment_provision_credential(
              &enrollment, login, strlen(login), password, strlen(password),
              100U) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NO_RANDOM,
          "a source that fills nothing is refused as NO_RANDOM");
    CHECK(appliance_production_admin_enrollment_credential_set(&enrollment) ==
              false,
          "a failed salt read wrote no credential");

    /*
     * The whole salt is the source's bytes: the proof generator is replayed
     * with the same seed and every byte has to match.
     */
    CHECK(appliance_production_admin_enrollment_set_random(
              &enrollment, credential_random_fill, &random_state) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the good salt source binds");
    CHECK(appliance_production_admin_enrollment_provision_credential(
              &enrollment, login, strlen(login), password, strlen(password),
              100U) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the hardening credential is set");
    zero_bytes(&stored, sizeof(stored));
    CHECK(appliance_persistent_secret_config_admin_credential(
              &secrets, &stored) == APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "the hardening credential reads back");
    generator_state = 0x5aU;
    for (index = 0U; index < sizeof(expected_salt); ++index) {
        generator_state =
            (uint8_t)((uint8_t)(generator_state * 29U) + 17U);
        expected_salt[index] = generator_state;
    }
    CHECK(memcmp(stored.salt, expected_salt, sizeof(expected_salt)) == 0,
          "every salt byte comes from the bound source");

    /* Unbinding the source refuses the next change without inventing a salt. */
    CHECK(appliance_production_admin_enrollment_set_random(
              &enrollment, NULL, NULL) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the salt source unbinds");
    CHECK(appliance_production_admin_enrollment_provision_credential(
              &enrollment, "second", 6U, "another good password", 21U,
              100U) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NO_RANDOM,
          "an unbound source refuses the change");
    zero_bytes(&stored, sizeof(stored));
    CHECK(appliance_persistent_secret_config_admin_credential(
              &secrets, &stored) == APPLIANCE_PERSISTENT_SECRET_CONFIG_OK &&
              strcmp(stored.login, login) == 0,
          "the refused change left the account alone");

    /* Lock the account and prove the lock survives a trip through the legacy
       entry points: switching paths must not clear one shared counter. */
    for (index = 0U;
         index < APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_FAILURE_LIMIT;
         ++index) {
        memset(token, 0, sizeof(token));
        (void)appliance_production_admin_enrollment_authenticate_credential(
            &enrollment, login, strlen(login), "wrong password!", 15U,
            300U, APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, token,
            &session_id);
    }
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate_credential(
              &enrollment, login, strlen(login), password, strlen(password),
              300U, APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, token,
              &session_id) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_THROTTLED,
          "the fifth failure locked the account before the legacy trip");
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate(
              &enrollment, password, strlen(password), 300U,
              APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, token,
              &session_id) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_CREDENTIAL_REQUIRED,
          "the legacy path refuses while the account is locked");
    CHECK(appliance_production_admin_enrollment_provision(
              &enrollment, password, strlen(password)) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_CREDENTIAL_REQUIRED,
          "the legacy provision refuses while the account is locked");
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate_credential(
              &enrollment, login, strlen(login), password, strlen(password),
              300U, APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, token,
              &session_id) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_THROTTLED,
          "the lockout survives the legacy trip");

    /* After an explicit migration the old legacy secret is not a password. */
    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_secret_store(&secrets, &device) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "the migration store loads again");
    CHECK(derive_fingerprint("legacy-secret", legacy),
          "the migration fingerprint derives");
    CHECK(commit_fingerprint(&secrets, legacy),
          "the migration fingerprint commits");
    zero_bytes(&auth, sizeof(auth));
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "the gate initializes for the migration proof");
    zero_bytes(&enrollment, sizeof(enrollment));
    CHECK(appliance_production_admin_enrollment_init(
              &enrollment, &auth, &secrets) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the owner initializes for the migration proof");
    random_state = 0x11U;
    CHECK(appliance_production_admin_enrollment_set_random(
              &enrollment, credential_random_fill, &random_state) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the migration salt source binds");
    CHECK(appliance_production_admin_enrollment_provision_credential(
              &enrollment, login, strlen(login), password, strlen(password),
              500U) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the migration sets the account");
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate_credential(
              &enrollment, login, strlen(login), "legacy-secret", 13U, 500U,
              APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, token,
              &session_id) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED,
          "the old legacy secret is denied after the migration");
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate_credential(
              &enrollment, login, strlen(login), password, strlen(password),
              500U, APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, token,
              &session_id) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the migrated password opens the session");
}

/*
 * ITEM-506 increment 3: the change conversation's verify-only step.  It proves
 * the current password with the account's own login, opens no session, counts
 * a failure and honours the same lockout, and names the unprovisioned and
 * legacy states exactly like the session-issuing entry.
 */
/*
 * ITEM-619: the same pair check through the cooperative begin/step/finish
 * path.  The job must need more than one slice, finish exactly on the stored
 * 200000 rounds, issue one session on OK and count one failure on DENIED.
 */
static void test_cooperative_credential_check(void)
{
    static const char login[] = "operator";
    static const char password[] = "correct horse battery staple";
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_secret_config secrets;
    struct appliance_production_auth auth;
    struct appliance_production_admin_enrollment enrollment;
    struct appliance_admin_credential_check check;
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t random_state = 0x33U;
    uint32_t session_id = 0U;
    uint32_t slices = 0U;
    enum appliance_admin_credential_status credential_status;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_secret_store(&secrets, &device) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "the cooperative check store loads");
    zero_bytes(&auth, sizeof(auth));
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "the cooperative gate initializes");
    zero_bytes(&enrollment, sizeof(enrollment));
    CHECK(appliance_production_admin_enrollment_init(
              &enrollment, &auth, &secrets) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the cooperative owner initializes");
    CHECK(appliance_production_admin_enrollment_set_random(
              &enrollment, credential_random_fill, &random_state) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the cooperative salt source binds");
    CHECK(appliance_production_admin_enrollment_provision_credential(
              &enrollment, login, strlen(login), password, strlen(password),
              100U) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the cooperative account is provisioned");

    zero_bytes(&check, sizeof(check));
    CHECK(appliance_production_admin_enrollment_begin_credential_check(
              &enrollment, login, strlen(login), password, strlen(password),
              200U, &check) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the cooperative check starts");
    CHECK(appliance_admin_credential_check_pending(&check),
          "the started check is pending");
    do {
        credential_status = appliance_admin_credential_check_step(
            &check, 4096U);
        ++slices;
    } while (credential_status == APPLIANCE_ADMIN_CREDENTIAL_PENDING);
    CHECK(slices > 1U, "the cooperative check needs more than one slice");
    CHECK(appliance_admin_credential_check_iterations_done(&check) == 200000U,
          "the cooperative check still runs the stored 200000 rounds");
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_finish_credential_check(
              &enrollment, &check, 200U,
              APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, token,
              &session_id) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the finished cooperative check issues its session");
    CHECK(session_id != 0U, "the cooperative session id is published");

    zero_bytes(&check, sizeof(check));
    CHECK(appliance_production_admin_enrollment_begin_credential_check(
              &enrollment, login, strlen(login), "wrong password!", 15U,
              300U, &check) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "a wrong cooperative pair still runs the same job");
    do {
        credential_status = appliance_admin_credential_check_step(
            &check, 4096U);
    } while (credential_status == APPLIANCE_ADMIN_CREDENTIAL_PENDING);
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_finish_credential_check(
              &enrollment, &check, 300U,
              APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, token,
              &session_id) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED,
          "the finished wrong cooperative check is denied");
}

static void test_verify_credential(void)
{
    static const char login[] = "operator";
    static const char password[] = "correct horse battery staple";
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_secret_config secrets;
    struct appliance_production_auth auth;
    struct appliance_production_admin_enrollment enrollment;
    uint8_t token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t random_state = 0x33U;
    char rendered[APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_RENDER_MAX];
    uint32_t session_id = 0U;
    size_t rendered_length = 0U;
    uint32_t attempt;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_secret_store(&secrets, &device) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "the verify store loads");
    zero_bytes(&auth, sizeof(auth));
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "the gate initializes for the verify step");
    zero_bytes(&enrollment, sizeof(enrollment));
    CHECK(appliance_production_admin_enrollment_init(
              &enrollment, &auth, &secrets) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the owner initializes for the verify step");

    CHECK(appliance_production_admin_enrollment_verify_credential(
              &enrollment, password, strlen(password), 100U) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNPROVISIONED,
          "the verify step refuses an unprovisioned account by name");

    CHECK(appliance_production_admin_enrollment_set_random(
              &enrollment, credential_random_fill, &random_state) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the verify salt source binds");
    CHECK(appliance_production_admin_enrollment_provision_credential(
              &enrollment, login, strlen(login), password, strlen(password),
              100U) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the verify account is provisioned");

    CHECK(appliance_production_admin_enrollment_verify_credential(
              &enrollment, password, strlen(password), 200U) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the right current password verifies");
    CHECK(appliance_production_admin_enrollment_render(
              &enrollment, rendered, sizeof(rendered), &rendered_length) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK &&
              strstr(rendered, " sessions=0") != NULL,
          "the verify step opens no session");

    CHECK(appliance_production_admin_enrollment_verify_credential(
              &enrollment, "wrong password!", 15U, 200U) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED,
          "a wrong current password is denied");
    CHECK(appliance_production_admin_enrollment_verify_credential(
              &enrollment, "", 0U, 200U) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_SECRET,
          "an empty current password is refused");

    for (attempt = 0U;
         attempt + 2U < APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_FAILURE_LIMIT;
         ++attempt) {
        (void)appliance_production_admin_enrollment_verify_credential(
            &enrollment, "wrong password!", 15U, 300U);
    }
    CHECK(appliance_production_admin_enrollment_verify_credential(
              &enrollment, "wrong password!", 15U, 300U) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_THROTTLED,
          "the verify step shares the five-failure lockout");
    CHECK(appliance_production_admin_enrollment_verify_credential(
              &enrollment, password, strlen(password), 300U) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_THROTTLED,
          "the locked account refuses the verify step too");
    CHECK(appliance_production_admin_enrollment_verify_credential(
              &enrollment, password, strlen(password),
              300U + APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_COOLDOWN_TICKS) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the verify step works after the cooldown");

    /* A successful verify must not have issued anything. */
    memset(token, 0, sizeof(token));
    CHECK(appliance_production_admin_enrollment_authenticate_credential(
              &enrollment, login, strlen(login), password, strlen(password),
              400U, APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, token,
              &session_id) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK &&
              session_id != 0U,
          "the session-issuing entry still works after the verify step");
}

/*
 * ITEM-598: the local login that unlocks the host seed proves the account
 * without opening a bearer session.  Publication is a separate step performed
 * only after the sealed seed has been accepted, so a seed failure cannot leave
 * a live session or an open request gate behind.
 */
static void test_entropy_credential_verification(void)
{
    static const char login[] = "operator";
    static const char password[] = "correct horse battery staple";
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_secret_config secrets;
    struct appliance_production_auth auth;
    struct appliance_production_admin_enrollment enrollment;
    struct appliance_production_admin_enrollment_verification verification;
    struct appliance_production_admin_enrollment_verification discarded;
    uint8_t random_state = 0x71U;
    char rendered[APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_RENDER_MAX];
    size_t rendered_length = 0U;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_secret_store(&secrets, &device) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "the entropy verification store loads");
    zero_bytes(&auth, sizeof(auth));
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "the entropy verification gate initializes");
    zero_bytes(&enrollment, sizeof(enrollment));
    CHECK(appliance_production_admin_enrollment_init(
              &enrollment, &auth, &secrets) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the entropy verification owner initializes");
    CHECK(appliance_production_admin_enrollment_set_random(
              &enrollment, credential_random_fill, &random_state) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the entropy verification salt source binds");
    CHECK(appliance_production_admin_enrollment_provision_credential(
              &enrollment, login, strlen(login), password, strlen(password),
              100U) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the entropy verification account is provisioned");

    /*
     * Provisioning publishes the account for this isolated owner test.  Reset
     * the request gate to model the ordinary boot, which starts closed, then
     * prove that account verification alone does not reopen it.
     */
    CHECK(appliance_production_auth_reset(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "the entropy verification gate resets to the boot state");
    zero_bytes(&verification, sizeof(verification));
    CHECK(appliance_production_admin_enrollment_verify_credential_pair(
              &enrollment, "someoneelse", strlen("someoneelse"), password,
              strlen(password), 200U, &verification) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED &&
              !verification.valid &&
              !appliance_production_admin_enrollment_published(&enrollment) &&
              !appliance_production_auth_available(&auth),
          "a wrong login issues no verification, session or gate publication");
    zero_bytes(&verification, sizeof(verification));
    CHECK(appliance_production_admin_enrollment_verify_credential_pair(
              &enrollment, login, strlen(login), "wrong password!", 15U,
              200U, &verification) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED &&
              !verification.valid &&
              !appliance_production_admin_enrollment_published(&enrollment) &&
              !appliance_production_auth_available(&auth),
          "a wrong password issues no verification, session or gate publication");

    zero_bytes(&verification, sizeof(verification));
    CHECK(appliance_production_admin_enrollment_verify_credential_pair(
              &enrollment, login, strlen(login), password, strlen(password),
              300U, &verification) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK &&
              verification.valid && enrollment.failure_count == 0U &&
              enrollment.locked_until_tick == 0U,
          "the right pair clears prior failures and produces a bounded "
          "verification without a session");
    CHECK(!appliance_production_admin_enrollment_published(&enrollment) &&
              !appliance_production_auth_available(&auth),
          "account verification alone still leaves the request gate closed");
    CHECK(appliance_production_admin_enrollment_render(
              &enrollment, rendered, sizeof(rendered), &rendered_length) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK &&
              strstr(rendered, " sessions=0") != NULL,
          "account verification alone opens no bearer session");

    discarded = verification;
    CHECK(appliance_production_admin_enrollment_publish_verification(
              NULL, &discarded) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NULL &&
              !discarded.valid,
          "a failed publication consumes the one-use verification");
    CHECK(appliance_production_admin_enrollment_publish_verification(
              &enrollment, &verification) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK &&
              !verification.valid &&
              appliance_production_admin_enrollment_published(&enrollment) &&
              appliance_production_auth_available(&auth),
          "the later publication step opens the gate but issues no session");
    CHECK(appliance_production_admin_enrollment_render(
              &enrollment, rendered, sizeof(rendered), &rendered_length) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK &&
              strstr(rendered, " sessions=0") != NULL,
          "gate publication after seed acceptance adds no session");

    zero_bytes(&verification, sizeof(verification));
    CHECK(appliance_production_admin_enrollment_publish_verification(
              &enrollment, &verification) !=
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "an empty verification cannot publish the account");
    appliance_production_admin_enrollment_verification_clear(&verification);
}

/*
 * ITEM-506 increment 6: the one setter is also the one invalidation.  The
 * panel change commits through `provision_credential()`, which republishes the
 * verifier through the request gate; a live gate closes every issued session
 * in that call, so the bearer (and the cookie that carries it) stops
 * authenticating the moment the credential changes.  A future writer that
 * bypassed the owner setter would leave the old sessions alive, so this proof
 * pins the coupling: the old session is refused, the old password stops
 * authenticating, the new pair opens a session and the store carries no
 * plaintext.
 */
static void test_change_revokes_sessions(void)
{
    static const char old_login[] = "operator";
    static const char old_password[] = "correct horse battery staple";
    static const char new_login[] = "operator2";
    static const char new_password[] = "a different long password";
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_secret_config secrets;
    struct appliance_production_auth auth;
    struct appliance_production_admin_enrollment enrollment;
    struct appliance_admin_credential before;
    struct appliance_admin_credential after;
    uint8_t old_token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t new_token[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t old_header[APPLIANCE_ADMIN_AUTH_BEARER_PREFIX_LENGTH +
                       APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH];
    uint8_t random_state = 0x6BU;
    uint32_t session_id = 0U;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_secret_store(&secrets, &device) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "the change store loads");
    zero_bytes(&auth, sizeof(auth));
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "the gate initializes for the change");
    zero_bytes(&enrollment, sizeof(enrollment));
    CHECK(appliance_production_admin_enrollment_init(
              &enrollment, &auth, &secrets) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK &&
              appliance_production_admin_enrollment_set_random(
                  &enrollment, credential_random_fill, &random_state) ==
                  APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the change owner initializes with a salt source");

    CHECK(appliance_production_admin_enrollment_provision_credential(
              &enrollment, old_login, strlen(old_login), old_password,
              strlen(old_password), 100U) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the first account is provisioned");
    zero_bytes(&before, sizeof(before));
    CHECK(appliance_persistent_secret_config_admin_credential(
              &secrets, &before) == APPLIANCE_PERSISTENT_SECRET_CONFIG_OK &&
              strcmp(before.login, old_login) == 0,
          "the first record is readable before the change");
    zero_bytes(old_token, sizeof(old_token));
    CHECK(appliance_production_admin_enrollment_authenticate_credential(
              &enrollment, old_login, strlen(old_login), old_password,
              strlen(old_password), 200U,
              APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, old_token,
              &session_id) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK &&
              session_id != 0U && build_bearer_header(old_token, old_header),
          "the previous session is issued");
    CHECK(authorize_system(&auth, old_header, sizeof(old_header), 200U) ==
              APPLIANCE_PRODUCTION_AUTH_ALLOWED,
          "the previous session authorizes a request before the change");

    CHECK(appliance_production_admin_enrollment_verify_credential(
              &enrollment, old_password, strlen(old_password), 300U) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the current password proves the change");
    CHECK(appliance_production_admin_enrollment_provision_credential(
              &enrollment, new_login, strlen(new_login), new_password,
              strlen(new_password), 301U) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the account changes through the single setter");

    /*
     * The owner setter republished the verifier; the gate reset closed the
     * session slot and moved the credential generation, so the same bearer is
     * refused from here on.
     */
    CHECK(authorize_system(&auth, old_header, sizeof(old_header), 302U) ==
              APPLIANCE_PRODUCTION_AUTH_DENIED,
          "the previous session stops authorizing after the change");
    CHECK(appliance_production_admin_enrollment_verify_credential(
              &enrollment, old_password, strlen(old_password), 303U) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED,
          "the old password no longer proves the account");

    zero_bytes(new_token, sizeof(new_token));
    session_id = 0U;
    CHECK(appliance_production_admin_enrollment_authenticate_credential(
              &enrollment, new_login, strlen(new_login), new_password,
              strlen(new_password), 304U,
              APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, new_token,
              &session_id) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK &&
              session_id != 0U,
          "the new pair opens a session");

    /* The durable record names the new login and never the plaintext. */
    zero_bytes(&after, sizeof(after));
    CHECK(appliance_persistent_secret_config_admin_credential(
              &secrets, &after) == APPLIANCE_PERSISTENT_SECRET_CONFIG_OK &&
              strcmp(after.login, new_login) == 0 &&
              after.iterations != 0U,
          "the stored record carries the new login");
    CHECK(memcmp(after.salt, before.salt, sizeof(after.salt)) != 0,
          "the change drew a fresh salt instead of reusing the old one");
    {
        const uint8_t *raw = (const uint8_t *)&secrets;
        size_t index;
        bool plaintext = false;

        for (index = 0U;
             index + sizeof(new_password) - 1U <= sizeof(secrets);
             ++index) {
            if (memcmp(raw + index, new_password,
                       sizeof(new_password) - 1U) == 0) {
                plaintext = true;
            }
        }
        CHECK(!plaintext,
              "the raw store carries no copy of the new password");
    }
    CHECK(appliance_production_admin_enrollment_authenticate_credential(
              &enrollment, old_login, strlen(old_login), new_password,
              strlen(new_password), 305U,
              APPLIANCE_ADMIN_AUTH_SESSION_LIFETIME_MAX, new_token,
              &session_id) != APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the old login with the new password is refused");
    zero_bytes(&after, sizeof(after));
    zero_bytes(&before, sizeof(before));
}

/*
 * ITEM-506 increment 5: the serving gate's predicate.  Only a structured
 * account (login + PBKDF2 verifier) allows the management plane to serve; an
 * uninitialized owner, a missing store, an empty store, the factory
 * placeholder and a pre-ITEM-506 legacy fingerprint all withhold it.  The
 * caller's state-volume term decides whether the question is asked at all:
 * that half (a volume that is not ready keeps serving) is reached by the
 * orchestration test and by the untouched network proof.
 */
static void test_first_run_serving_gate(void)
{
    static const char login[] = "operator";
    static const char password[] = "correct horse battery staple";
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_secret_config secrets;
    struct appliance_production_auth auth;
    struct appliance_production_admin_enrollment enrollment;
    struct appliance_production_admin_enrollment uninitialized;
    uint8_t fingerprint[SHA256_DIGEST_SIZE];
    uint8_t legacy[SHA256_DIGEST_SIZE];
    uint8_t random_state = 0x5AU;

    CHECK(!appliance_production_admin_enrollment_serving_allowed(NULL),
          "no enrollment withholds serving");
    zero_bytes(&uninitialized, sizeof(uninitialized));
    CHECK(!appliance_production_admin_enrollment_serving_allowed(
              &uninitialized),
          "an uninitialized enrollment withholds serving");

    zero_bytes(&auth, sizeof(auth));
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "the gate initializes without a store");
    zero_bytes(&enrollment, sizeof(enrollment));
    CHECK(appliance_production_admin_enrollment_init(
              &enrollment, &auth, NULL) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the owner initializes without a store");
    CHECK(!appliance_production_admin_enrollment_serving_allowed(&enrollment),
          "an owner without a store withholds serving");

    /* The factory placeholder is not an account. */
    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_secret_store(&secrets, &device) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "the factory store loads");
    CHECK(factory_fingerprint(fingerprint), "the factory fingerprint derives");
    CHECK(commit_fingerprint(&secrets, fingerprint),
          "the factory fingerprint commits");
    zero_bytes(&enrollment, sizeof(enrollment));
    CHECK(appliance_production_admin_enrollment_init(
              &enrollment, &auth, &secrets) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the owner initializes over the factory record");
    CHECK(!appliance_production_admin_enrollment_serving_allowed(&enrollment),
          "the factory placeholder withholds serving");

    /* A pre-ITEM-506 legacy fingerprint is not an account either. */
    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_secret_store(&secrets, &device) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "the legacy store loads");
    CHECK(derive_fingerprint("legacy-secret", legacy),
          "the legacy fingerprint derives");
    CHECK(commit_fingerprint(&secrets, legacy),
          "the legacy fingerprint commits");
    zero_bytes(&enrollment, sizeof(enrollment));
    CHECK(appliance_production_admin_enrollment_init(
              &enrollment, &auth, &secrets) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the owner initializes over the legacy record");
    CHECK(!appliance_production_admin_enrollment_serving_allowed(&enrollment),
          "a legacy fingerprint withholds serving");

    /* The structured account is the one that opens the plane. */
    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_secret_store(&secrets, &device) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "the account store loads");
    zero_bytes(&enrollment, sizeof(enrollment));
    CHECK(appliance_production_admin_enrollment_init(
              &enrollment, &auth, &secrets) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the owner initializes over the empty store");
    CHECK(!appliance_production_admin_enrollment_serving_allowed(&enrollment),
          "an unprovisioned store withholds serving");
    CHECK(appliance_production_admin_enrollment_set_random(
              &enrollment, credential_random_fill, &random_state) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the salt source binds for the gate");
    CHECK(appliance_production_admin_enrollment_provision_credential(
              &enrollment, login, strlen(login), password, strlen(password),
              100U) == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the first account is set");
    CHECK(appliance_production_admin_enrollment_serving_allowed(&enrollment),
          "the structured account allows serving");
    CHECK(appliance_production_admin_enrollment_reset(&enrollment) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "the explicit reset runs");
    CHECK(!appliance_production_admin_enrollment_serving_allowed(&enrollment),
          "a reset account withholds serving again");
}

/*
 * ITEM-516: the boot-time policy publication.
 *
 * The record the operator wrote says "publish without a local login"; the
 * structured record already holds the verifier, so a fresh owner over the same
 * store publishes it and the serving gate opens with no authentication at all.
 * A store with no account, a factory placeholder and a legacy fingerprint
 * refuse by name and publish nothing, so the boot reports the closed gate
 * rather than pretending the policy ran.
 */
static void test_publish_policy(void)
{
    static const char login[] = "operator";
    static const char password[] = "correct horse battery staple";
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_secret_config secrets;
    struct appliance_production_auth auth;
    struct appliance_production_admin_enrollment enrollment;
    struct appliance_production_admin_enrollment next_boot;
    uint8_t fingerprint[SHA256_DIGEST_SIZE];
    uint8_t random_state = 0x33U;

    /* No store at all: the policy refuses by name. */
    zero_bytes(&auth, sizeof(auth));
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "policy publication: the gate initializes without a store");
    zero_bytes(&enrollment, sizeof(enrollment));
    CHECK(appliance_production_admin_enrollment_init(
              &enrollment, &auth, NULL) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK &&
              appliance_production_admin_enrollment_publish_policy(
                  &enrollment) ==
                  APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNAVAILABLE &&
              !appliance_production_admin_enrollment_published(&enrollment),
          "policy publication refuses a machine without a store");

    /* A fresh store with no account: nothing to publish. */
    init_fixture(&fixture);
    device = make_device(&fixture);
    zero_bytes(&auth, sizeof(auth));
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK &&
              load_secret_store(&secrets, &device) ==
                  APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "policy publication: the empty store loads");
    zero_bytes(&enrollment, sizeof(enrollment));
    CHECK(appliance_production_admin_enrollment_init(
              &enrollment, &auth, &secrets) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK &&
              appliance_production_admin_enrollment_publish_policy(
                  &enrollment) ==
                  APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNPROVISIONED &&
              !appliance_production_admin_enrollment_published(&enrollment),
          "policy publication refuses a store without an account");

    /* A legacy fingerprint is not a structured credential. */
    init_fixture(&fixture);
    device = make_device(&fixture);
    zero_bytes(&auth, sizeof(auth));
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK &&
              load_secret_store(&secrets, &device) ==
                  APPLIANCE_PERSISTENT_SECRET_CONFIG_OK &&
              derive_fingerprint("legacy-secret", fingerprint) &&
              commit_fingerprint(&secrets, fingerprint),
          "policy publication: the legacy fixture commits");
    zero_bytes(&enrollment, sizeof(enrollment));
    CHECK(appliance_production_admin_enrollment_init(
              &enrollment, &auth, &secrets) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK &&
              appliance_production_admin_enrollment_publish_policy(
                  &enrollment) ==
                  APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_LEGACY_RECORD &&
              !appliance_production_admin_enrollment_published(&enrollment),
          "policy publication refuses a legacy fingerprint");

    /* The account a previous boot set is published without any login. */
    init_fixture(&fixture);
    device = make_device(&fixture);
    zero_bytes(&auth, sizeof(auth));
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK &&
              load_secret_store(&secrets, &device) ==
                  APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "policy publication: the account store loads");
    zero_bytes(&enrollment, sizeof(enrollment));
    CHECK(appliance_production_admin_enrollment_init(
              &enrollment, &auth, &secrets) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK &&
              appliance_production_admin_enrollment_set_random(
                  &enrollment, credential_random_fill, &random_state) ==
                  APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK &&
              appliance_production_admin_enrollment_provision_credential(
                  &enrollment, login, strlen(login), password,
                  strlen(password), 100U) ==
                  APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK,
          "policy publication: the account is set by the first boot");

    zero_bytes(&auth, sizeof(auth));
    CHECK(appliance_production_auth_init(&auth) ==
              APPLIANCE_PRODUCTION_AUTH_OK,
          "policy publication: the next boot's gate initializes empty");
    zero_bytes(&next_boot, sizeof(next_boot));
    CHECK(appliance_production_admin_enrollment_init(
              &next_boot, &auth, &secrets) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK &&
              !appliance_production_admin_enrollment_published(&next_boot) &&
              appliance_production_admin_enrollment_publish_policy(
                  &next_boot) ==
                  APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK &&
              appliance_production_admin_enrollment_published(&next_boot) &&
              appliance_production_admin_enrollment_serving_allowed(
                  &next_boot),
          "policy publication opens the gate without a local login");
    CHECK(appliance_production_admin_enrollment_publish_policy(NULL) ==
              APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_NULL,
          "policy publication rejects a null owner");
}

int main(void)
{
    test_status_strings();
    test_init_and_provisionable();
    test_unavailable_without_store();
    test_factory_placeholder_is_never_a_credential();
    test_provision_then_authenticate();
    test_reset_restores_provisionable();
    test_enrollment_from_committed_record();
    test_throttling();
    test_credential_path();
    test_credential_hardening();
    test_verify_credential();
    test_cooperative_credential_check();
    test_entropy_credential_verification();
    test_change_revokes_sessions();
    test_first_run_serving_gate();
    test_publish_policy();
    test_render();
    if (failures != 0) {
        fprintf(stderr, "\n%d FAILURES\n", failures);
        return 1;
    }
    printf("Appliance production admin enrollment host tests: ALL OK\n");
    return 0;
}

#endif
