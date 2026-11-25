#include "network_admission.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_NETWORK_ADMISSION_HOST_TEST

#define NETWORK_ADMISSION_TEST_PORT 3000U
#define NETWORK_ADMISSION_TEST_PAYLOAD_MAX 255U
#define NETWORK_ADMISSION_TEST_SESSION 1U
#define NETWORK_ADMISSION_TEST_PRINCIPAL 7U
#define NETWORK_ADMISSION_TEST_GENERATION 1U

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;
static struct network_admission_policy policy;
static struct network_admission_peer peer;
static struct network_admission_selection selection;
static struct management_command_registry registry;
static struct authorization_matrix matrix;
static unsigned char frame[MANAGEMENT_TRANSPORT_FRAME_MAX];

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

static void reset_policy(void)
{
    memset(&policy, 0, sizeof(policy));
    policy.version = NETWORK_ADMISSION_VERSION;
    policy.mode = NETWORK_ADMISSION_MODE_ROLE_SESSION;
    policy.port = NETWORK_ADMISSION_TEST_PORT;
    policy.payload_max = NETWORK_ADMISSION_TEST_PAYLOAD_MAX;
}

static void reset_peer(void)
{
    memset(&peer, 0, sizeof(peer));
    peer.version = NETWORK_ADMISSION_VERSION;
    peer.session_id = NETWORK_ADMISSION_TEST_SESSION;
    peer.principal_id = NETWORK_ADMISSION_TEST_PRINCIPAL;
    peer.generation = NETWORK_ADMISSION_TEST_GENERATION;
    peer.authenticated = true;
    peer.role_count = 1U;
    peer.roles[0] = AUTHORIZATION_ROLE_ADMIN;
    peer.capability_count = 2U;
    peer.capabilities[0] = AUTHORIZATION_CAPABILITY_STATUS_READ;
    peer.capabilities[1] = AUTHORIZATION_CAPABILITY_CONFIG_WRITE;
}

static struct management_command_descriptor make_descriptor(
    uint32_t command_id,
    uint16_t request_min_length,
    uint16_t request_max_length)
{
    struct management_command_descriptor descriptor;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.version = MANAGEMENT_COMMAND_VERSION;
    descriptor.command_id = command_id;
    descriptor.request_min_length = request_min_length;
    descriptor.request_max_length = request_max_length;
    descriptor.response_min_length = 0U;
    descriptor.response_max_length = MANAGEMENT_COMMAND_PAYLOAD_MAX;
    return descriptor;
}

static void init_fixtures(void)
{
    struct management_command_descriptor descriptors[2];
    struct authorization_entry entries[2];
    static const uint8_t admin_roles[1] = { AUTHORIZATION_ROLE_ADMIN };
    static const uint8_t status_capabilities[1] = {
        AUTHORIZATION_CAPABILITY_STATUS_READ
    };
    static const uint8_t config_capabilities[1] = {
        AUTHORIZATION_CAPABILITY_CONFIG_WRITE
    };

    reset_policy();
    reset_peer();
    memset(&selection, 0, sizeof(selection));
    memset(&frame, 0, sizeof(frame));

    descriptors[0] = make_descriptor(1U, 0U, 0U);
    descriptors[1] = make_descriptor(2U, 1U, 4U);
    CHECK(management_command_registry_init(
              &registry, descriptors, 2U) == MANAGEMENT_COMMAND_OK,
          "network admission fixture registry initializes");

    memset(&entries, 0, sizeof(entries));
    entries[0].version = AUTHORIZATION_VERSION;
    entries[0].command_id = 1U;
    entries[0].role_count = 1U;
    memcpy(entries[0].roles, admin_roles, sizeof(admin_roles));
    entries[0].capability_count = 1U;
    memcpy(entries[0].capabilities, status_capabilities,
           sizeof(status_capabilities));
    entries[1].version = AUTHORIZATION_VERSION;
    entries[1].command_id = 2U;
    entries[1].role_count = 1U;
    memcpy(entries[1].roles, admin_roles, sizeof(admin_roles));
    entries[1].capability_count = 1U;
    memcpy(entries[1].capabilities, config_capabilities,
           sizeof(config_capabilities));
    CHECK(authorization_matrix_init(
              &matrix, entries, 2U) == AUTHORIZATION_OK,
          "network admission fixture authorization matrix initializes");
}

static void write_transport_frame(
    unsigned char *data,
    uint32_t version,
    uint16_t port,
    uint16_t payload_length,
    uint32_t direction)
{
    memset(data, 0, MANAGEMENT_TRANSPORT_FRAME_MAX);
    data[0] = (unsigned char)(version & 0xFFU);
    data[1] = (unsigned char)((version >> 8U) & 0xFFU);
    data[2] = (unsigned char)((version >> 16U) & 0xFFU);
    data[3] = (unsigned char)((version >> 24U) & 0xFFU);
    data[4] = (unsigned char)(port & 0xFFU);
    data[5] = (unsigned char)((port >> 8U) & 0xFFU);
    data[6] = (unsigned char)(payload_length & 0xFFU);
    data[7] = (unsigned char)((payload_length >> 8U) & 0xFFU);
    data[8] = (unsigned char)(direction & 0xFFU);
    data[9] = (unsigned char)((direction >> 8U) & 0xFFU);
    data[10] = (unsigned char)((direction >> 16U) & 0xFFU);
    data[11] = (unsigned char)((direction >> 24U) & 0xFFU);
}

static void write_command_frame(
    unsigned char *payload,
    uint32_t version,
    uint32_t command_id,
    const unsigned char *command_payload,
    size_t command_payload_length)
{
    payload[0] = (unsigned char)(version & 0xFFU);
    payload[1] = (unsigned char)((version >> 8U) & 0xFFU);
    payload[2] = (unsigned char)((version >> 16U) & 0xFFU);
    payload[3] = (unsigned char)((version >> 24U) & 0xFFU);
    payload[4] = (unsigned char)(command_id & 0xFFU);
    payload[5] = (unsigned char)((command_id >> 8U) & 0xFFU);
    payload[6] = (unsigned char)((command_id >> 16U) & 0xFFU);
    payload[7] = (unsigned char)((command_id >> 24U) & 0xFFU);
    if (command_payload_length > 0U && command_payload != NULL) {
        memcpy(payload + MANAGEMENT_COMMAND_HEADER_LENGTH,
               command_payload, command_payload_length);
    }
}

static size_t write_valid_config_request(void)
{
    static const unsigned char config_payload[1] = { 0x11U };
    size_t payload_length =
        MANAGEMENT_COMMAND_HEADER_LENGTH + sizeof(config_payload);

    write_transport_frame(frame, MANAGEMENT_TRANSPORT_VERSION,
                          NETWORK_ADMISSION_TEST_PORT,
                          (uint16_t)payload_length,
                          MANAGEMENT_TRANSPORT_DIRECTION_REQUEST);
    write_command_frame(frame + MANAGEMENT_TRANSPORT_HEADER_SIZE,
                        MANAGEMENT_COMMAND_VERSION, 2U,
                        config_payload, sizeof(config_payload));
    return MANAGEMENT_TRANSPORT_HEADER_SIZE + payload_length;
}

static void test_status_strings(void)
{
    static const struct {
        enum network_admission_status status;
        const char *string;
    } statuses[] = {
        {NETWORK_ADMISSION_OK, "NETWORK_ADMISSION_OK"},
        {NETWORK_ADMISSION_NULL, "NETWORK_ADMISSION_NULL"},
        {NETWORK_ADMISSION_BAD_STATE, "NETWORK_ADMISSION_BAD_STATE"},
        {NETWORK_ADMISSION_BAD_VERSION, "NETWORK_ADMISSION_BAD_VERSION"},
        {NETWORK_ADMISSION_BAD_MODE, "NETWORK_ADMISSION_BAD_MODE"},
        {NETWORK_ADMISSION_BAD_PORT, "NETWORK_ADMISSION_BAD_PORT"},
        {NETWORK_ADMISSION_BAD_PAYLOAD_MAX,
         "NETWORK_ADMISSION_BAD_PAYLOAD_MAX"},
        {NETWORK_ADMISSION_ALREADY_PUBLISHED,
         "NETWORK_ADMISSION_ALREADY_PUBLISHED"},
        {NETWORK_ADMISSION_UNPUBLISHED,
         "NETWORK_ADMISSION_UNPUBLISHED"},
        {NETWORK_ADMISSION_INVALID_PEER,
         "NETWORK_ADMISSION_INVALID_PEER"},
        {NETWORK_ADMISSION_UNAUTHENTICATED,
         "NETWORK_ADMISSION_UNAUTHENTICATED"},
        {NETWORK_ADMISSION_DENIED_BY_DEFAULT,
         "NETWORK_ADMISSION_DENIED_BY_DEFAULT"},
        {NETWORK_ADMISSION_BAD_FRAME, "NETWORK_ADMISSION_BAD_FRAME"},
        {NETWORK_ADMISSION_BAD_COMMAND, "NETWORK_ADMISSION_BAD_COMMAND"},
        {NETWORK_ADMISSION_DENIED_ROLE,
         "NETWORK_ADMISSION_DENIED_ROLE"},
        {NETWORK_ADMISSION_DENIED_CAPABILITY,
         "NETWORK_ADMISSION_DENIED_CAPABILITY"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = network_admission_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "network admission status string is exact");
    }
    CHECK(network_admission_status_string(
              (enum network_admission_status)16U) == NULL,
          "network admission undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "network admission status handling performs no allocation");
}

static void test_policy_validation(void)
{
    struct network_admission_policy bad;

    init_fixtures();
    CHECK(network_admission_policy_validate(NULL) ==
              NETWORK_ADMISSION_NULL,
          "network admission policy rejects null policy");
    bad = policy;
    bad.version = NETWORK_ADMISSION_VERSION + 1U;
    CHECK(network_admission_policy_validate(&bad) ==
              NETWORK_ADMISSION_BAD_VERSION,
          "network admission policy rejects unknown version");
    bad = policy;
    bad.mode = (enum network_admission_mode)2U;
    CHECK(network_admission_policy_validate(&bad) ==
              NETWORK_ADMISSION_BAD_MODE,
          "network admission policy rejects unknown mode");
    bad = policy;
    bad.port = NETWORK_ADMISSION_PORT_MIN - 1U;
    CHECK(network_admission_policy_validate(&bad) ==
              NETWORK_ADMISSION_BAD_PORT,
          "network admission policy rejects zero port");
    bad = policy;
    bad.payload_max = NETWORK_ADMISSION_PAYLOAD_MAX_MAX + 1U;
    CHECK(network_admission_policy_validate(&bad) ==
              NETWORK_ADMISSION_BAD_PAYLOAD_MAX,
          "network admission policy rejects over-limit payload maximum");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "network admission policy validation performs no allocation");
}

static void test_peer_validation(void)
{
    struct network_admission_peer bad;

    init_fixtures();
    CHECK(network_admission_peer_validate(NULL) ==
              NETWORK_ADMISSION_NULL,
          "network admission peer rejects null peer");
    bad = peer;
    bad.version = NETWORK_ADMISSION_VERSION + 1U;
    CHECK(network_admission_peer_validate(&bad) ==
              NETWORK_ADMISSION_INVALID_PEER,
          "network admission peer rejects unknown version");
    bad = peer;
    bad.session_id = NETWORK_ADMISSION_PEER_SESSION_ID_MIN - 1U;
    CHECK(network_admission_peer_validate(&bad) ==
              NETWORK_ADMISSION_INVALID_PEER,
          "network admission peer rejects zero session");
    bad = peer;
    bad.principal_id = NETWORK_ADMISSION_PEER_PRINCIPAL_ID_MIN - 1U;
    CHECK(network_admission_peer_validate(&bad) ==
              NETWORK_ADMISSION_INVALID_PEER,
          "network admission peer rejects zero principal");
    bad = peer;
    bad.generation = NETWORK_ADMISSION_PEER_GENERATION_MIN - 1U;
    CHECK(network_admission_peer_validate(&bad) ==
              NETWORK_ADMISSION_INVALID_PEER,
          "network admission peer rejects zero generation");
    bad = peer;
    bad.role_count = NETWORK_ADMISSION_ROLE_SET_MAX + 1U;
    CHECK(network_admission_peer_validate(&bad) ==
              NETWORK_ADMISSION_INVALID_PEER,
          "network admission peer rejects over-limit role set");
    bad = peer;
    bad.roles[0] = 0U;
    CHECK(network_admission_peer_validate(&bad) ==
              NETWORK_ADMISSION_INVALID_PEER,
          "network admission peer rejects zero role");
    bad = peer;
    bad.role_count = 2U;
    bad.roles[0] = 1U;
    bad.roles[1] = 1U;
    CHECK(network_admission_peer_validate(&bad) ==
              NETWORK_ADMISSION_INVALID_PEER,
          "network admission peer rejects duplicate roles");
    bad = peer;
    bad.capability_count = NETWORK_ADMISSION_CAPABILITY_SET_MAX + 1U;
    CHECK(network_admission_peer_validate(&bad) ==
              NETWORK_ADMISSION_INVALID_PEER,
          "network admission peer rejects over-limit capability set");
    bad = peer;
    bad.capabilities[0] = 0U;
    CHECK(network_admission_peer_validate(&bad) ==
              NETWORK_ADMISSION_INVALID_PEER,
          "network admission peer rejects zero capability");
    bad = peer;
    bad.capability_count = 2U;
    bad.capabilities[0] = 1U;
    bad.capabilities[1] = 1U;
    CHECK(network_admission_peer_validate(&bad) ==
              NETWORK_ADMISSION_INVALID_PEER,
          "network admission peer rejects duplicate capabilities");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "network admission peer validation performs no allocation");
}

static void test_selection_init(void)
{
    struct network_admission_policy bad;
    unsigned char before[sizeof(selection)];

    init_fixtures();
    CHECK(network_admission_selection_init(
              NULL, &policy) == NETWORK_ADMISSION_NULL,
          "network admission selection rejects null selection");
    CHECK(network_admission_selection_init(
              &selection, NULL) == NETWORK_ADMISSION_NULL,
          "network admission selection rejects null policy");
    bad = policy;
    bad.mode = (enum network_admission_mode)2U;
    CHECK(network_admission_selection_init(
              &selection, &bad) == NETWORK_ADMISSION_BAD_MODE,
          "network admission selection rejects invalid policy");
    memset(&selection, 0, sizeof(selection));
    memcpy(before, &selection, sizeof(before));
    CHECK(network_admission_selection_init(
              &selection, &bad) == NETWORK_ADMISSION_BAD_MODE &&
              memcmp(&selection, before, sizeof(before)) == 0,
          "network admission selection failure does not mutate destination");
    memset(&selection, 0, sizeof(selection));
    CHECK(network_admission_selection_init(
              &selection, &policy) == NETWORK_ADMISSION_OK &&
              network_admission_selection_published(&selection),
          "network admission selection publishes valid policy");
    CHECK(network_admission_selection_init(
              &selection, &policy) ==
              NETWORK_ADMISSION_ALREADY_PUBLISHED,
          "network admission duplicate selection publication is rejected");
    CHECK(network_admission_selection_published(NULL) == false,
          "null network admission selection is unpublished");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "network admission selection init performs no allocation");
}

static void test_deny_by_default_and_unauthenticated(void)
{
    size_t frame_length;

    init_fixtures();
    frame_length = write_valid_config_request();
    CHECK(network_admission_check_request(
              &selection, &peer, &registry, &matrix,
              frame, frame_length) == NETWORK_ADMISSION_UNPUBLISHED,
          "unpublished network admission selection denies request");

    policy.mode = NETWORK_ADMISSION_MODE_DENY_ALL;
    CHECK((network_admission_selection_init(
              &selection, &policy) == NETWORK_ADMISSION_OK),
          "deny-all policy selection publishes");
    CHECK(network_admission_check_request(
              &selection, &peer, &registry, &matrix,
              frame, frame_length) == NETWORK_ADMISSION_DENIED_BY_DEFAULT,
          "published deny-all network admission policy denies request");

    policy.mode = NETWORK_ADMISSION_MODE_ROLE_SESSION;
    memset(&selection, 0, sizeof(selection));
    CHECK((network_admission_selection_init(
              &selection, &policy) == NETWORK_ADMISSION_OK),
          "role-session policy selection publishes");
    peer.authenticated = false;
    CHECK(network_admission_check_request(
              &selection, &peer, &registry, &matrix,
              frame, frame_length) == NETWORK_ADMISSION_UNAUTHENTICATED,
          "unauthenticated peer cannot invoke control command");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "deny-by-default and unauthenticated checks perform no allocation");
}

static void test_bad_transport_frames(void)
{
    size_t frame_length;

    init_fixtures();
    CHECK((network_admission_selection_init(
              &selection, &policy) == NETWORK_ADMISSION_OK),
          "transport failure fixture policy publishes");

    frame_length = write_valid_config_request();
    CHECK(network_admission_check_request(
              &selection, &peer, &registry, &matrix,
              frame, frame_length) == NETWORK_ADMISSION_OK,
          "network admission baseline request is accepted");

    frame_length = write_valid_config_request();
    write_transport_frame(frame, MANAGEMENT_TRANSPORT_VERSION + 1U,
                          NETWORK_ADMISSION_TEST_PORT,
                          (uint16_t)(
                              MANAGEMENT_COMMAND_HEADER_LENGTH + 1U),
                          MANAGEMENT_TRANSPORT_DIRECTION_REQUEST);
    CHECK(network_admission_check_request(
              &selection, &peer, &registry, &matrix,
              frame, frame_length) == NETWORK_ADMISSION_BAD_FRAME,
          "unknown transport version is denied before command parsing");

    frame_length = write_valid_config_request();
    write_transport_frame(frame, MANAGEMENT_TRANSPORT_VERSION,
                          (uint16_t)(NETWORK_ADMISSION_TEST_PORT + 1U),
                          (uint16_t)(
                              MANAGEMENT_COMMAND_HEADER_LENGTH + 1U),
                          MANAGEMENT_TRANSPORT_DIRECTION_REQUEST);
    CHECK(network_admission_check_request(
              &selection, &peer, &registry, &matrix,
              frame, frame_length) == NETWORK_ADMISSION_BAD_FRAME,
          "wrong network admission port is denied");

    frame_length = write_valid_config_request();
    write_transport_frame(frame, MANAGEMENT_TRANSPORT_VERSION,
                          NETWORK_ADMISSION_TEST_PORT,
                          (uint16_t)(
                              MANAGEMENT_COMMAND_HEADER_LENGTH + 1U),
                          MANAGEMENT_TRANSPORT_DIRECTION_RESPONSE);
    CHECK(network_admission_check_request(
              &selection, &peer, &registry, &matrix,
              frame, frame_length) == NETWORK_ADMISSION_BAD_FRAME,
          "response transport frame is denied by request admission");

    frame_length = write_valid_config_request();
    write_transport_frame(frame, MANAGEMENT_TRANSPORT_VERSION,
                          NETWORK_ADMISSION_TEST_PORT,
                          (uint16_t)(
                              MANAGEMENT_COMMAND_HEADER_LENGTH + 1U),
                          MANAGEMENT_TRANSPORT_DIRECTION_REQUEST);
    CHECK(network_admission_check_request(
              &selection, &peer, &registry, &matrix,
              frame, MANAGEMENT_TRANSPORT_HEADER_SIZE) ==
              NETWORK_ADMISSION_BAD_FRAME,
          "partial transport frame is denied before command parsing");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "network admission transport failures perform no allocation");
}

static void test_bad_command_frames(void)
{
    size_t payload_length;
    size_t frame_length;

    init_fixtures();
    CHECK((network_admission_selection_init(
              &selection, &policy) == NETWORK_ADMISSION_OK),
          "command failure fixture policy publishes");

    payload_length = MANAGEMENT_COMMAND_HEADER_LENGTH;
    frame_length = MANAGEMENT_TRANSPORT_HEADER_SIZE + payload_length;
    write_transport_frame(frame, MANAGEMENT_TRANSPORT_VERSION,
                          NETWORK_ADMISSION_TEST_PORT,
                          (uint16_t)payload_length,
                          MANAGEMENT_TRANSPORT_DIRECTION_REQUEST);
    write_command_frame(frame + MANAGEMENT_TRANSPORT_HEADER_SIZE,
                        MANAGEMENT_COMMAND_VERSION, 99U, NULL, 0U);
    CHECK(network_admission_check_request(
              &selection, &peer, &registry, &matrix,
              frame, frame_length) == NETWORK_ADMISSION_BAD_COMMAND,
          "unknown command is denied before authorization");

    payload_length = MANAGEMENT_COMMAND_HEADER_LENGTH + 1U;
    frame_length = MANAGEMENT_TRANSPORT_HEADER_SIZE + payload_length;
    write_transport_frame(frame, MANAGEMENT_TRANSPORT_VERSION,
                          NETWORK_ADMISSION_TEST_PORT,
                          (uint16_t)payload_length,
                          MANAGEMENT_TRANSPORT_DIRECTION_REQUEST);
    write_command_frame(frame + MANAGEMENT_TRANSPORT_HEADER_SIZE,
                        MANAGEMENT_COMMAND_VERSION + 1U, 2U, NULL, 0U);
    CHECK(network_admission_check_request(
              &selection, &peer, &registry, &matrix,
              frame, frame_length) == NETWORK_ADMISSION_BAD_COMMAND,
          "unknown command version is denied before authorization");

    payload_length = MANAGEMENT_COMMAND_HEADER_LENGTH;
    frame_length = MANAGEMENT_TRANSPORT_HEADER_SIZE + payload_length;
    write_transport_frame(frame, MANAGEMENT_TRANSPORT_VERSION,
                          NETWORK_ADMISSION_TEST_PORT,
                          (uint16_t)payload_length,
                          MANAGEMENT_TRANSPORT_DIRECTION_REQUEST);
    write_command_frame(frame + MANAGEMENT_TRANSPORT_HEADER_SIZE,
                        MANAGEMENT_COMMAND_VERSION, 2U, NULL, 0U);
    CHECK(network_admission_check_request(
              &selection, &peer, &registry, &matrix,
              frame, frame_length) == NETWORK_ADMISSION_BAD_COMMAND,
          "too-short registered command is denied before authorization");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "network admission command failures perform no allocation");
}

static void test_authorization_and_success(void)
{
    size_t frame_length;

    init_fixtures();
    CHECK((network_admission_selection_init(
              &selection, &policy) == NETWORK_ADMISSION_OK),
          "authorization fixture policy publishes");

    frame_length = write_valid_config_request();
    peer.role_count = 1U;
    peer.roles[0] = AUTHORIZATION_ROLE_OPERATOR;
    peer.capability_count = 1U;
    peer.capabilities[0] = AUTHORIZATION_CAPABILITY_STATUS_READ;
    CHECK(network_admission_check_request(
              &selection, &peer, &registry, &matrix,
              frame, frame_length) == NETWORK_ADMISSION_DENIED_ROLE,
          "missing authorization role denies network control command");

    reset_peer();
    frame_length = write_valid_config_request();
    peer.role_count = 1U;
    peer.roles[0] = AUTHORIZATION_ROLE_ADMIN;
    peer.capability_count = 1U;
    peer.capabilities[0] = AUTHORIZATION_CAPABILITY_STATUS_READ;
    CHECK(network_admission_check_request(
              &selection, &peer, &registry, &matrix,
              frame, frame_length) == NETWORK_ADMISSION_DENIED_CAPABILITY,
          "missing authorization capability denies network control command");

    reset_peer();
    frame_length = write_valid_config_request();
    CHECK(network_admission_check_request(
              &selection, &peer, &registry, &matrix,
              frame, frame_length) == NETWORK_ADMISSION_OK,
          "authenticated authorized peer may invoke framed control command");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "network admission authorization cycle performs no allocation");
}

int main(void)
{
    init_fixtures();
    alloc_attempts = 0U;
    free_attempts = 0U;
    test_status_strings();
    test_policy_validation();
    test_peer_validation();
    test_selection_init();
    test_deny_by_default_and_unauthenticated();
    test_bad_transport_frames();
    test_bad_command_frames();
    test_authorization_and_success();

    if (failures == 0) {
        printf("Network admission host tests: ALL OK\n");
    }
    return failures != 0;
}

#else
#error "This host test must be built with HFOC_NETWORK_ADMISSION_HOST_TEST"
#endif
