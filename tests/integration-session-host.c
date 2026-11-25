#include "integration_session.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_INTEGRATION_SESSION_HOST_TEST

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

static void fill_nonce(uint8_t *nonce, uint8_t first_byte)
{
    memset(nonce, 0, INTEGRATION_SESSION_NONCE_SIZE);
    nonce[0] = first_byte;
}

static struct integration_session_request make_request(
    uint32_t schema_version,
    uint32_t transport_version,
    uint32_t peer_id,
    uint8_t first_nonce_byte)
{
    struct integration_session_request request;

    memset(&request, 0, sizeof(request));
    request.schema_version = schema_version;
    request.transport_version = transport_version;
    request.peer_id = peer_id;
    fill_nonce(request.nonce, first_nonce_byte);
    return request;
}

static struct integration_session_nonce_registry make_registry(
    uint32_t capacity)
{
    struct integration_session_nonce_registry registry;

    memset(&registry, 0xA5, sizeof(registry));
    CHECK(integration_session_nonce_registry_init(&registry, capacity) ==
              INTEGRATION_SESSION_OK,
          "integration session registry fixture initializes");
    return registry;
}

static void capture_registry(
    const struct integration_session_nonce_registry *registry,
    unsigned char *original)
{
    memcpy(original, registry, sizeof(*registry));
}

static void check_registry_unchanged(
    const struct integration_session_nonce_registry *registry,
    const unsigned char *original,
    const char *message)
{
    CHECK(memcmp(registry, original, sizeof(*registry)) == 0, message);
}

static void check_request_failure(
    const struct integration_session_request *request,
    struct integration_session_nonce_registry *registry,
    enum integration_session_status expected,
    const char *message)
{
    unsigned char original[sizeof(*registry)];

    capture_registry(registry, original);
    CHECK(integration_session_handshake_validate(request, registry) ==
              expected,
          message);
    check_registry_unchanged(registry, original,
                             "integration session rejection leaves registry unchanged");
}

static void test_exact_constants(void)
{
    CHECK(INTEGRATION_SESSION_VERSION == 1U,
          "integration session version is exactly 1");
    CHECK(INTEGRATION_SESSION_SCHEMA_VERSION_MIN ==
              INTEGRATION_BOUNDARY_SCHEMA_VERSION_MIN &&
              INTEGRATION_SESSION_SCHEMA_VERSION_MIN == 1U,
          "integration session schema minimum is the boundary minimum");
    CHECK(INTEGRATION_SESSION_SCHEMA_VERSION_MAX ==
              INTEGRATION_BOUNDARY_SCHEMA_VERSION_MAX &&
              INTEGRATION_SESSION_SCHEMA_VERSION_MAX == 1U,
          "integration session schema maximum is the boundary maximum");
    CHECK(INTEGRATION_SESSION_TRANSPORT_VERSION ==
              INTEGRATION_BOUNDARY_TRANSPORT_VERSION &&
              INTEGRATION_SESSION_TRANSPORT_VERSION == 1U,
          "integration session transport version is exactly 1");
    CHECK(INTEGRATION_SESSION_PEER_ID_MIN == 1U &&
              INTEGRATION_SESSION_PEER_ID_MAX == 65535U,
          "integration session peer identity bounds are exact");
    CHECK(INTEGRATION_SESSION_NONCE_SIZE == 32U,
          "integration session nonce size is exactly 32 bytes");
    CHECK(INTEGRATION_SESSION_NONCE_REGISTRY_MAX == 64U,
          "integration session nonce registry maximum is exactly 64");
}

static void test_registry_init(void)
{
    struct integration_session_nonce_registry registry;
    uint32_t index;

    CHECK(integration_session_nonce_registry_init(NULL, 1U) ==
              INTEGRATION_SESSION_NULL,
          "integration session registry init rejects null registry");
    CHECK(integration_session_nonce_registry_init(&registry, 0U) ==
              INTEGRATION_SESSION_BAD_CAPACITY,
          "integration session registry init rejects zero capacity");
    CHECK(integration_session_nonce_registry_init(
              &registry,
              INTEGRATION_SESSION_NONCE_REGISTRY_MAX + 1U) ==
              INTEGRATION_SESSION_BAD_CAPACITY,
          "integration session registry init rejects over-limit capacity");
    CHECK(integration_session_nonce_registry_init(&registry, 3U) ==
              INTEGRATION_SESSION_OK &&
              registry.version == INTEGRATION_SESSION_VERSION &&
              registry.capacity == 3U &&
              registry.count == 0U,
          "integration session registry init publishes exact empty state");
    for (index = 0U;
         index < INTEGRATION_SESSION_NONCE_REGISTRY_MAX; ++index) {
        CHECK(registry.entries[index].peer_id == 0U,
              "integration session registry init zeroes every peer id");
        {
            uint8_t nonce_byte;

            for (nonce_byte = 0U;
                 nonce_byte < INTEGRATION_SESSION_NONCE_SIZE;
                 ++nonce_byte) {
                if (registry.entries[index].nonce[nonce_byte] != 0U) {
                    break;
                }
            }
            CHECK(nonce_byte == INTEGRATION_SESSION_NONCE_SIZE,
                  "integration session registry init zeroes every nonce byte");
        }
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "integration session registry init performs no allocation");
}

static void test_validation_failures(void)
{
    struct integration_session_nonce_registry registry = make_registry(3U);
    struct integration_session_request valid = make_request(
        INTEGRATION_SESSION_SCHEMA_VERSION_MIN,
        INTEGRATION_SESSION_TRANSPORT_VERSION,
        7U, 0x11U);
    struct integration_session_request bad_schema_low = make_request(
        INTEGRATION_SESSION_SCHEMA_VERSION_MIN - 1U,
        INTEGRATION_SESSION_TRANSPORT_VERSION,
        7U, 0x11U);
    struct integration_session_request bad_schema_high = make_request(
        INTEGRATION_SESSION_SCHEMA_VERSION_MAX + 1U,
        INTEGRATION_SESSION_TRANSPORT_VERSION,
        7U, 0x11U);
    struct integration_session_request bad_transport_low = make_request(
        INTEGRATION_SESSION_SCHEMA_VERSION_MIN,
        INTEGRATION_SESSION_TRANSPORT_VERSION - 1U,
        7U, 0x11U);
    struct integration_session_request bad_transport_high = make_request(
        INTEGRATION_SESSION_SCHEMA_VERSION_MIN,
        INTEGRATION_SESSION_TRANSPORT_VERSION + 1U,
        7U, 0x11U);
    struct integration_session_request bad_peer_low = make_request(
        INTEGRATION_SESSION_SCHEMA_VERSION_MIN,
        INTEGRATION_SESSION_TRANSPORT_VERSION,
        0U, 0x11U);
    struct integration_session_request bad_peer_high = make_request(
        INTEGRATION_SESSION_SCHEMA_VERSION_MIN,
        INTEGRATION_SESSION_TRANSPORT_VERSION,
        INTEGRATION_SESSION_PEER_ID_MAX + 1U,
        0x11U);
    struct integration_session_request zero_nonce = make_request(
        INTEGRATION_SESSION_SCHEMA_VERSION_MIN,
        INTEGRATION_SESSION_TRANSPORT_VERSION,
        7U, 0U);
    struct integration_session_nonce_registry bad_state = registry;
    unsigned char original[sizeof(registry)];

    memset(zero_nonce.nonce, 0, sizeof(zero_nonce.nonce));
    capture_registry(&registry, original);
    CHECK(integration_session_handshake_validate(NULL, &registry) ==
              INTEGRATION_SESSION_NULL,
          "integration session handshake rejects null request");
    CHECK(integration_session_handshake_validate(&valid, NULL) ==
              INTEGRATION_SESSION_NULL,
          "integration session handshake rejects null registry");
    check_registry_unchanged(&registry, original,
                             "null handshake rejection leaves registry unchanged");

    bad_state.version = 0U;
    check_request_failure(&valid, &bad_state,
                          INTEGRATION_SESSION_BAD_STATE,
                          "integration session handshake rejects bad registry version");
    bad_state = registry;
    bad_state.capacity = 0U;
    check_request_failure(&valid, &bad_state,
                          INTEGRATION_SESSION_BAD_CAPACITY,
                          "integration session handshake rejects zero capacity");
    bad_state = registry;
    bad_state.capacity = INTEGRATION_SESSION_NONCE_REGISTRY_MAX + 1U;
    check_request_failure(&valid, &bad_state,
                          INTEGRATION_SESSION_BAD_CAPACITY,
                          "integration session handshake rejects over-limit capacity");
    bad_state = registry;
    bad_state.count = bad_state.capacity + 1U;
    check_request_failure(&valid, &bad_state,
                          INTEGRATION_SESSION_BAD_STATE,
                          "integration session handshake rejects corrupted count");

    check_request_failure(&bad_schema_low, &registry,
                          INTEGRATION_SESSION_UNSUPPORTED_SCHEMA_VERSION,
                          "integration session handshake rejects low schema version");
    check_request_failure(&bad_schema_high, &registry,
                          INTEGRATION_SESSION_UNSUPPORTED_SCHEMA_VERSION,
                          "integration session handshake rejects high schema version");
    check_request_failure(&bad_transport_low, &registry,
                          INTEGRATION_SESSION_UNSUPPORTED_TRANSPORT_VERSION,
                          "integration session handshake rejects low transport version");
    check_request_failure(&bad_transport_high, &registry,
                          INTEGRATION_SESSION_UNSUPPORTED_TRANSPORT_VERSION,
                          "integration session handshake rejects high transport version");
    check_request_failure(&bad_peer_low, &registry,
                          INTEGRATION_SESSION_INVALID_PEER_ID,
                          "integration session handshake rejects zero peer id");
    check_request_failure(&bad_peer_high, &registry,
                          INTEGRATION_SESSION_INVALID_PEER_ID,
                          "integration session handshake rejects over-limit peer id");
    check_request_failure(&zero_nonce, &registry,
                          INTEGRATION_SESSION_INVALID_NONCE,
                          "integration session handshake rejects zero nonce");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "integration session validation failures perform no allocation");
}

static void test_valid_publication_and_replay(void)
{
    struct integration_session_nonce_registry registry = make_registry(4U);
    struct integration_session_request first = make_request(
        INTEGRATION_SESSION_SCHEMA_VERSION_MIN,
        INTEGRATION_SESSION_TRANSPORT_VERSION,
        7U, 0x11U);
    struct integration_session_request same_peer_same_nonce = first;
    struct integration_session_request other_peer_same_nonce = make_request(
        INTEGRATION_SESSION_SCHEMA_VERSION_MIN,
        INTEGRATION_SESSION_TRANSPORT_VERSION,
        8U, 0x11U);
    struct integration_session_request same_peer_other_nonce = make_request(
        INTEGRATION_SESSION_SCHEMA_VERSION_MIN,
        INTEGRATION_SESSION_TRANSPORT_VERSION,
        7U, 0x22U);
    unsigned char before_replay[sizeof(registry)];

    CHECK(integration_session_handshake_validate(
              &first, &registry) == INTEGRATION_SESSION_OK &&
              registry.count == 1U &&
              registry.entries[0].peer_id == 7U &&
              memcmp(registry.entries[0].nonce,
                     first.nonce,
                     INTEGRATION_SESSION_NONCE_SIZE) == 0,
          "integration session handshake publishes exact peer nonce");

    capture_registry(&registry, before_replay);
    CHECK(integration_session_handshake_validate(
              &same_peer_same_nonce, &registry) ==
              INTEGRATION_SESSION_REPLAY,
          "integration session handshake rejects replayed peer nonce");
    check_registry_unchanged(&registry, before_replay,
                             "replay rejection leaves registry unchanged");

    CHECK(integration_session_handshake_validate(
              &other_peer_same_nonce, &registry) == INTEGRATION_SESSION_OK &&
              registry.count == 2U &&
              registry.entries[1].peer_id == 8U,
          "integration session handshake scopes nonce replay by peer identity");

    CHECK(integration_session_handshake_validate(
              &same_peer_other_nonce, &registry) == INTEGRATION_SESSION_OK &&
              registry.count == 3U &&
              registry.entries[2].nonce[0] == 0x22U,
          "integration session handshake accepts a new nonce from same peer");

    capture_registry(&registry, before_replay);
    CHECK(integration_session_handshake_validate(
              &same_peer_other_nonce, &registry) ==
              INTEGRATION_SESSION_REPLAY,
          "integration session handshake rejects second replay of peer nonce");
    check_registry_unchanged(&registry, before_replay,
                             "second replay rejection leaves registry unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "integration session handshake performs no allocation");
}

static void test_registry_full(void)
{
    struct integration_session_nonce_registry registry = make_registry(2U);
    struct integration_session_request first = make_request(
        INTEGRATION_SESSION_SCHEMA_VERSION_MIN,
        INTEGRATION_SESSION_TRANSPORT_VERSION,
        1U, 0x01U);
    struct integration_session_request second = make_request(
        INTEGRATION_SESSION_SCHEMA_VERSION_MIN,
        INTEGRATION_SESSION_TRANSPORT_VERSION,
        2U, 0x02U);
    struct integration_session_request third = make_request(
        INTEGRATION_SESSION_SCHEMA_VERSION_MIN,
        INTEGRATION_SESSION_TRANSPORT_VERSION,
        3U, 0x03U);
    unsigned char original[sizeof(registry)];

    CHECK(integration_session_handshake_validate(
              &first, &registry) == INTEGRATION_SESSION_OK,
          "integration session registry full fixture accepts first nonce");
    CHECK(integration_session_handshake_validate(
              &second, &registry) == INTEGRATION_SESSION_OK,
          "integration session registry full fixture accepts second nonce");
    capture_registry(&registry, original);
    CHECK(integration_session_handshake_validate(
              &third, &registry) == INTEGRATION_SESSION_REGISTRY_FULL,
          "integration session handshake rejects valid request when registry is full");
    check_registry_unchanged(&registry, original,
                             "registry-full rejection leaves registry unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "integration session registry-full path performs no allocation");
}

static const char *expected_status_string(
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

static void test_status_strings(void)
{
    enum integration_session_status status;

    for (status = INTEGRATION_SESSION_OK;
         status <= INTEGRATION_SESSION_REGISTRY_FULL; ++status) {
        const char *string = integration_session_status_string(status);
        const char *expected = expected_status_string(status);

        CHECK(string != NULL && expected != NULL &&
                  strcmp(string, expected) == 0,
              "integration session status string is exact");
    }
    CHECK(integration_session_status_string(
              (enum integration_session_status)10U) == NULL,
          "integration session undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "integration session status handling performs no allocation");
}

int main(void)
{
    test_exact_constants();
    test_registry_init();
    test_validation_failures();
    test_valid_publication_and_replay();
    test_registry_full();
    test_status_strings();

    if (failures != 0) {
        fprintf(stderr, "Integration session host tests: FAILED (%d)\n",
                failures);
        return 1;
    }
    printf("Integration session host tests: ALL OK\n");
    return 0;
}

#endif
