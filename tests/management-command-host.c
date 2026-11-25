#include "management_command.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_MANAGEMENT_COMMAND_HOST_TEST

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

static struct management_command_descriptor make_descriptor(
    uint32_t command_id,
    uint16_t request_min_length,
    uint16_t request_max_length,
    uint16_t response_min_length,
    uint16_t response_max_length)
{
    struct management_command_descriptor descriptor;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.version = MANAGEMENT_COMMAND_VERSION;
    descriptor.command_id = command_id;
    descriptor.request_min_length = request_min_length;
    descriptor.request_max_length = request_max_length;
    descriptor.response_min_length = response_min_length;
    descriptor.response_max_length = response_max_length;
    return descriptor;
}

static void init_test_descriptors(
    struct management_command_descriptor *descriptors)
{
    descriptors[0] = make_descriptor(1U, 0U, 0U, 0U,
                                     MANAGEMENT_COMMAND_PAYLOAD_MAX);
    descriptors[1] = make_descriptor(2U, 1U, 4U, 2U, 8U);
}

static void capture_registry(const struct management_command_registry *registry,
                             unsigned char *original)
{
    memcpy(original, registry, sizeof(*registry));
}

static struct management_command_registry make_registry(void) {
    struct management_command_descriptor descriptors[2];
    struct management_command_registry registry;

    init_test_descriptors(descriptors);
    CHECK(management_command_registry_init(&registry, descriptors, 2U) ==
              MANAGEMENT_COMMAND_OK,
          "management command registry fixture initializes");
    return registry;
}

static void test_status_strings(void) {
    enum management_command_status status;

    for (status = MANAGEMENT_COMMAND_OK;
         status <= MANAGEMENT_COMMAND_INVALID_DESCRIPTOR; ++status) {
        const char *string = management_command_status_string(status);

        CHECK(string != NULL &&
                  (status == MANAGEMENT_COMMAND_OK ?
                       strcmp(string, "MANAGEMENT_COMMAND_OK") == 0 :
                   status == MANAGEMENT_COMMAND_NULL ?
                       strcmp(string, "MANAGEMENT_COMMAND_NULL") == 0 :
                   status == MANAGEMENT_COMMAND_BAD_CAPACITY ?
                       strcmp(string,
                              "MANAGEMENT_COMMAND_BAD_CAPACITY") == 0 :
                   status == MANAGEMENT_COMMAND_BAD_VERSION ?
                       strcmp(string,
                              "MANAGEMENT_COMMAND_BAD_VERSION") == 0 :
                   status == MANAGEMENT_COMMAND_UNKNOWN_ID ?
                       strcmp(string,
                              "MANAGEMENT_COMMAND_UNKNOWN_ID") == 0 :
                   status == MANAGEMENT_COMMAND_BAD_DIRECTION ?
                       strcmp(string,
                              "MANAGEMENT_COMMAND_BAD_DIRECTION") == 0 :
                   status == MANAGEMENT_COMMAND_BAD_LENGTH ?
                       strcmp(string,
                              "MANAGEMENT_COMMAND_BAD_LENGTH") == 0 :
                   status == MANAGEMENT_COMMAND_TRAILING_FIELDS ?
                       strcmp(string,
                              "MANAGEMENT_COMMAND_TRAILING_FIELDS") == 0 :
                   status == MANAGEMENT_COMMAND_BAD_STATE ?
                       strcmp(string,
                              "MANAGEMENT_COMMAND_BAD_STATE") == 0 :
                   status == MANAGEMENT_COMMAND_DUPLICATE ?
                       strcmp(string,
                              "MANAGEMENT_COMMAND_DUPLICATE") == 0 :
                       strcmp(string,
                              "MANAGEMENT_COMMAND_INVALID_DESCRIPTOR") == 0),
              "management command status string is exact for every status");
    }
    CHECK(management_command_status_string(
              (enum management_command_status)11U) == NULL,
          "management command undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "management command status handling performs no allocation");
}

static void test_registry_init(void) {
    struct management_command_descriptor descriptors[2];
    struct management_command_descriptor bad_descriptor;
    struct management_command_registry registry;
    unsigned char before[sizeof(registry)];
    uint32_t index;

    init_test_descriptors(descriptors);
    memset(&registry, 0, sizeof(registry));

    CHECK(management_command_registry_init(NULL, descriptors, 2U) ==
              MANAGEMENT_COMMAND_NULL,
          "management command registry init rejects null registry");
    CHECK(management_command_registry_init(&registry, NULL, 0U) ==
              MANAGEMENT_COMMAND_BAD_CAPACITY,
          "management command registry init rejects zero capacity");
    CHECK(management_command_registry_init(&registry, NULL, 1U) ==
              MANAGEMENT_COMMAND_NULL,
          "management command registry init rejects null descriptors");
    CHECK(management_command_registry_init(
              &registry, descriptors,
              MANAGEMENT_COMMAND_REGISTRY_CAPACITY_MAX + 1U) ==
              MANAGEMENT_COMMAND_BAD_CAPACITY,
          "management command registry init rejects over-limit capacity");

    bad_descriptor = make_descriptor(1U, 0U, 0U, 0U,
                                     MANAGEMENT_COMMAND_PAYLOAD_MAX);
    bad_descriptor.version = MANAGEMENT_COMMAND_VERSION + 1U;
    CHECK(management_command_registry_init(&registry,
                                           &bad_descriptor, 1U) ==
              MANAGEMENT_COMMAND_BAD_VERSION,
          "management command registry init rejects unknown descriptor version");

    bad_descriptor = make_descriptor(0U, 0U, 0U, 0U,
                                     MANAGEMENT_COMMAND_PAYLOAD_MAX);
    CHECK(management_command_registry_init(&registry,
                                           &bad_descriptor, 1U) ==
              MANAGEMENT_COMMAND_INVALID_DESCRIPTOR,
          "management command registry init rejects zero command id");

    bad_descriptor = make_descriptor(MANAGEMENT_COMMAND_ID_MAX + 1U,
                                     0U, 0U, 0U,
                                     MANAGEMENT_COMMAND_PAYLOAD_MAX);
    CHECK(management_command_registry_init(&registry,
                                           &bad_descriptor, 1U) ==
              MANAGEMENT_COMMAND_INVALID_DESCRIPTOR,
          "management command registry init rejects over-limit command id");

    bad_descriptor = make_descriptor(1U, 2U, 1U, 0U,
                                     MANAGEMENT_COMMAND_PAYLOAD_MAX);
    CHECK(management_command_registry_init(&registry,
                                           &bad_descriptor, 1U) ==
              MANAGEMENT_COMMAND_INVALID_DESCRIPTOR,
          "management command registry init rejects inverted request bounds");

    bad_descriptor = make_descriptor(
        1U, 0U, MANAGEMENT_COMMAND_PAYLOAD_MAX + 1U, 0U,
        MANAGEMENT_COMMAND_PAYLOAD_MAX);
    CHECK(management_command_registry_init(&registry,
                                           &bad_descriptor, 1U) ==
              MANAGEMENT_COMMAND_INVALID_DESCRIPTOR,
          "management command registry init rejects over-limit request bound");

    bad_descriptor = make_descriptor(1U, 0U, 0U, 2U, 1U);
    CHECK(management_command_registry_init(&registry,
                                           &bad_descriptor, 1U) ==
              MANAGEMENT_COMMAND_INVALID_DESCRIPTOR,
          "management command registry init rejects inverted response bounds");

    bad_descriptor = make_descriptor(
        1U, 0U, 0U, 0U, MANAGEMENT_COMMAND_PAYLOAD_MAX + 1U);
    CHECK(management_command_registry_init(&registry,
                                           &bad_descriptor, 1U) ==
              MANAGEMENT_COMMAND_INVALID_DESCRIPTOR,
          "management command registry init rejects over-limit response bound");

    init_test_descriptors(descriptors);
    descriptors[1].command_id = descriptors[0].command_id;
    memset(&registry, 0x5A, sizeof(registry));
    capture_registry(&registry, before);
    CHECK(management_command_registry_init(&registry, descriptors, 2U) ==
              MANAGEMENT_COMMAND_DUPLICATE &&
              memcmp(&registry, before, sizeof(before)) == 0,
          "management command registry init rejects duplicates without mutation");

    init_test_descriptors(descriptors);
    CHECK(management_command_registry_init(&registry, descriptors, 2U) ==
              MANAGEMENT_COMMAND_OK &&
              registry.version == MANAGEMENT_COMMAND_VERSION &&
              registry.count == 2U &&
              registry.initialized,
          "management command registry init publishes exact configuration");
    for (index = 0U; index < MANAGEMENT_COMMAND_REGISTRY_CAPACITY_MAX;
         ++index) {
        if (index < 2U) {
            CHECK(registry.descriptors[index].version ==
                      MANAGEMENT_COMMAND_VERSION &&
                      registry.descriptors[index].command_id ==
                          descriptors[index].command_id &&
                      registry.descriptors[index].request_min_length ==
                          descriptors[index].request_min_length &&
                      registry.descriptors[index].request_max_length ==
                          descriptors[index].request_max_length &&
                      registry.descriptors[index].response_min_length ==
                          descriptors[index].response_min_length &&
                      registry.descriptors[index].response_max_length ==
                          descriptors[index].response_max_length,
                  "management command registry init copies exact descriptors");
        } else {
            CHECK(registry.descriptors[index].version == 0U &&
                      registry.descriptors[index].command_id == 0U &&
                      registry.descriptors[index].request_min_length == 0U &&
                      registry.descriptors[index].request_max_length == 0U &&
                      registry.descriptors[index].response_min_length == 0U &&
                      registry.descriptors[index].response_max_length == 0U,
                  "management command registry init zeroes unused descriptors");
        }
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "management command registry init performs no allocation");
}

static void test_frame_validation_failures(void) {
    struct management_command_registry registry = make_registry();
    struct management_command_registry bad_registry = registry;
    struct management_command_header header;
    unsigned char payload[8];

    memset(payload, 0xAB, sizeof(payload));
    header.version = MANAGEMENT_COMMAND_VERSION;
    header.command_id = 1U;

    CHECK(management_command_validate_frame(
              NULL, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
              &header, NULL, 0U, MANAGEMENT_COMMAND_HEADER_LENGTH) ==
              MANAGEMENT_COMMAND_NULL,
          "management command frame validation rejects null registry");
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
              NULL, NULL, 0U, MANAGEMENT_COMMAND_HEADER_LENGTH) ==
              MANAGEMENT_COMMAND_NULL,
          "management command frame validation rejects null header");
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
              &header, NULL, 1U, MANAGEMENT_COMMAND_HEADER_LENGTH + 1U) ==
              MANAGEMENT_COMMAND_NULL,
          "management command frame validation rejects null payload with length");

    bad_registry.initialized = false;
    CHECK(management_command_validate_frame(
              &bad_registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
              &header, NULL, 0U, MANAGEMENT_COMMAND_HEADER_LENGTH) ==
              MANAGEMENT_COMMAND_BAD_STATE,
          "management command frame validation rejects uninitialized registry");

    bad_registry = registry;
    bad_registry.count = 0U;
    CHECK(management_command_validate_frame(
              &bad_registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
              &header, NULL, 0U, MANAGEMENT_COMMAND_HEADER_LENGTH) ==
              MANAGEMENT_COMMAND_BAD_STATE,
          "management command frame validation rejects zero registry count");

    bad_registry = registry;
    bad_registry.version = MANAGEMENT_COMMAND_VERSION + 1U;
    CHECK(management_command_validate_frame(
              &bad_registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
              &header, NULL, 0U, MANAGEMENT_COMMAND_HEADER_LENGTH) ==
              MANAGEMENT_COMMAND_BAD_VERSION,
          "management command frame validation rejects unknown registry version");

    header.version = MANAGEMENT_COMMAND_VERSION + 1U;
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
              &header, NULL, 0U, MANAGEMENT_COMMAND_HEADER_LENGTH) ==
              MANAGEMENT_COMMAND_BAD_VERSION,
          "management command frame validation rejects unknown header version");
    header.version = MANAGEMENT_COMMAND_VERSION;

    header.command_id = 0U;
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
              &header, NULL, 0U, MANAGEMENT_COMMAND_HEADER_LENGTH) ==
              MANAGEMENT_COMMAND_UNKNOWN_ID,
          "management command frame validation rejects zero command id");
    header.command_id = MANAGEMENT_COMMAND_ID_MAX + 1U;
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
              &header, NULL, 0U, MANAGEMENT_COMMAND_HEADER_LENGTH) ==
              MANAGEMENT_COMMAND_UNKNOWN_ID,
          "management command frame validation rejects over-limit command id");
    header.command_id = 3U;
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
              &header, NULL, 0U, MANAGEMENT_COMMAND_HEADER_LENGTH) ==
              MANAGEMENT_COMMAND_UNKNOWN_ID,
          "management command frame validation rejects unregistered command id");
    header.command_id = 1U;

    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_COUNT,
              &header, NULL, 0U, MANAGEMENT_COMMAND_HEADER_LENGTH) ==
              MANAGEMENT_COMMAND_BAD_DIRECTION,
          "management command frame validation rejects invalid direction");

    header.command_id = 2U;
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
              &header, payload, 0U, MANAGEMENT_COMMAND_HEADER_LENGTH) ==
              MANAGEMENT_COMMAND_BAD_LENGTH,
          "management command request rejects too-short payload");
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
              &header, payload, 5U, MANAGEMENT_COMMAND_HEADER_LENGTH + 5U) ==
              MANAGEMENT_COMMAND_BAD_LENGTH,
          "management command request rejects too-long payload");
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_RESPONSE,
              &header, payload, 1U, MANAGEMENT_COMMAND_HEADER_LENGTH + 1U) ==
              MANAGEMENT_COMMAND_BAD_LENGTH,
          "management command response rejects too-short payload");
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_RESPONSE,
              &header, payload, 9U, MANAGEMENT_COMMAND_HEADER_LENGTH + 9U) ==
              MANAGEMENT_COMMAND_BAD_LENGTH,
          "management command response rejects too-long payload");

    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
              &header, payload, 1U, MANAGEMENT_COMMAND_HEADER_LENGTH - 1U) ==
              MANAGEMENT_COMMAND_BAD_LENGTH,
          "management command frame validation rejects missing header bytes");
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
              &header, payload, 1U, MANAGEMENT_COMMAND_HEADER_LENGTH) ==
              MANAGEMENT_COMMAND_BAD_LENGTH,
          "management command frame validation rejects missing payload bytes");
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
              &header, payload, 1U, MANAGEMENT_COMMAND_HEADER_LENGTH + 2U) ==
              MANAGEMENT_COMMAND_TRAILING_FIELDS,
          "management command frame validation rejects trailing fields");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "management command frame failures perform no allocation");
}

static void test_valid_frames(void) {
    struct management_command_registry registry = make_registry();
    struct management_command_header header;
    unsigned char payload[8];

    memset(payload, 0xCD, sizeof(payload));
    header.version = MANAGEMENT_COMMAND_VERSION;
    header.command_id = 1U;

    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
              &header, NULL, 0U, MANAGEMENT_COMMAND_HEADER_LENGTH) ==
              MANAGEMENT_COMMAND_OK,
          "management command zero-length request frame is accepted");
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_RESPONSE,
              &header, payload, MANAGEMENT_COMMAND_PAYLOAD_MAX,
              MANAGEMENT_COMMAND_HEADER_LENGTH +
                  MANAGEMENT_COMMAND_PAYLOAD_MAX) ==
              MANAGEMENT_COMMAND_OK,
          "management command maximum response frame is accepted");

    header.command_id = 2U;
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
              &header, payload, 1U, MANAGEMENT_COMMAND_HEADER_LENGTH + 1U) ==
              MANAGEMENT_COMMAND_OK,
          "management command bounded request frame is accepted");
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
              &header, payload, 4U, MANAGEMENT_COMMAND_HEADER_LENGTH + 4U) ==
              MANAGEMENT_COMMAND_OK,
          "management command maximum request frame is accepted");
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_RESPONSE,
              &header, payload, 2U, MANAGEMENT_COMMAND_HEADER_LENGTH + 2U) ==
              MANAGEMENT_COMMAND_OK,
          "management command bounded response frame is accepted");
    CHECK(management_command_validate_frame(
              &registry, MANAGEMENT_COMMAND_DIRECTION_RESPONSE,
              &header, payload, 8U, MANAGEMENT_COMMAND_HEADER_LENGTH + 8U) ==
              MANAGEMENT_COMMAND_OK,
          "management command maximum response frame is accepted");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "management command valid frames perform no allocation");
}

int main(void) {
    test_status_strings();
    test_registry_init();
    test_frame_validation_failures();
    test_valid_frames();

    if (failures == 0) {
        printf("Management command host tests: ALL OK\n");
        return 0;
    }
    fprintf(stderr, "Management command host tests: FAILED (%d)\n",
            failures);
    return 1;
}

#endif
