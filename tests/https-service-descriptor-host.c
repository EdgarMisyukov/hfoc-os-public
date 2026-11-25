#include "https_service.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_HTTPS_SERVICE_HOST_TEST

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

static struct https_service_resource_envelope default_envelope(void) {
    struct https_service_resource_envelope envelope;

    memset(&envelope, 0, sizeof(envelope));
    envelope.connection_capacity = HTTPS_SERVICE_CONNECTION_CAPACITY;
    envelope.tls_context_capacity = HTTPS_SERVICE_TLS_CONTEXT_CAPACITY;
    envelope.http_parser_capacity = HTTPS_SERVICE_HTTP_PARSER_CAPACITY;
    envelope.packet_pool_capacity =
        HTTPS_SERVICE_PACKET_POOL_CAPACITY_DEFAULT;
    return envelope;
}

static struct https_service_descriptor make_descriptor(
    uint32_t version,
    uint32_t id,
    const char *name,
    uint32_t restart_budget,
    uint32_t readiness_deadline_ms,
    struct https_service_resource_envelope envelope)
{
    struct https_service_descriptor descriptor;
    size_t name_length;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.version = version;
    descriptor.id = id;
    descriptor.restart_budget = restart_budget;
    descriptor.readiness_deadline_ms = readiness_deadline_ms;
    descriptor.envelope = envelope;
    if (name != NULL) {
        name_length = strlen(name);
        if (name_length > HTTPS_SERVICE_NAME_MAX) {
            name_length = HTTPS_SERVICE_NAME_MAX;
        }
        memcpy(descriptor.name, name, name_length);
    }
    return descriptor;
}

static struct https_service_descriptor valid_descriptor(void) {
    return make_descriptor(
        HTTPS_SERVICE_DESCRIPTOR_VERSION,
        HTTPS_SERVICE_ID,
        HTTPS_SERVICE_NAME,
        HTTPS_SERVICE_RESTART_BUDGET_DEFAULT,
        HTTPS_SERVICE_READINESS_DEADLINE_DEFAULT_MS,
        default_envelope());
}

static struct https_service_readiness make_readiness(uint32_t mask) {
    struct https_service_readiness readiness;

    memset(&readiness, 0, sizeof(readiness));
    readiness.version = HTTPS_SERVICE_READINESS_VERSION;
    readiness.available_mask = mask;
    return readiness;
}

static void test_status_strings(void) {
    enum https_service_status status;

    CHECK(https_service_status_string(
              (enum https_service_status)99U) == NULL,
          "undefined HTTPS descriptor status string is rejected");
    for (status = HTTPS_SERVICE_OK;
         status <= HTTPS_SERVICE_DEPENDENCY_UNAVAILABLE;
         ++status) {
        const char *string = https_service_status_string(status);

        CHECK(string != NULL &&
                  strcmp(string, status == HTTPS_SERVICE_OK ?
                             "HTTPS_SERVICE_OK" :
                         status == HTTPS_SERVICE_NULL ?
                             "HTTPS_SERVICE_NULL" :
                         status == HTTPS_SERVICE_BAD_STATE ?
                             "HTTPS_SERVICE_BAD_STATE" :
                         status == HTTPS_SERVICE_BAD_VERSION ?
                             "HTTPS_SERVICE_BAD_VERSION" :
                         status == HTTPS_SERVICE_BAD_ID ?
                             "HTTPS_SERVICE_BAD_ID" :
                         status == HTTPS_SERVICE_BAD_NAME ?
                             "HTTPS_SERVICE_BAD_NAME" :
                         status == HTTPS_SERVICE_BAD_RESTART_BUDGET ?
                             "HTTPS_SERVICE_BAD_RESTART_BUDGET" :
                         status == HTTPS_SERVICE_BAD_READINESS_DEADLINE ?
                             "HTTPS_SERVICE_BAD_READINESS_DEADLINE" :
                         status == HTTPS_SERVICE_BAD_RESOURCE_ENVELOPE ?
                             "HTTPS_SERVICE_BAD_RESOURCE_ENVELOPE" :
                         status == HTTPS_SERVICE_BAD_DEPENDENCY ?
                             "HTTPS_SERVICE_BAD_DEPENDENCY" :
                         status == HTTPS_SERVICE_ALREADY_PUBLISHED ?
                             "HTTPS_SERVICE_ALREADY_PUBLISHED" :
                         status == HTTPS_SERVICE_NOT_PUBLISHED ?
                             "HTTPS_SERVICE_NOT_PUBLISHED" :
                             "HTTPS_SERVICE_DEPENDENCY_UNAVAILABLE") == 0,
              "HTTPS descriptor status string is exact for every status");
    }
}

static void test_null_and_publication(void) {
    struct https_service_descriptor descriptor = valid_descriptor();
    struct https_service_descriptor published;
    unsigned char original[sizeof(published)];

    CHECK(https_service_descriptor_validate(NULL) == HTTPS_SERVICE_NULL,
          "HTTPS descriptor validate rejects null descriptor");
    CHECK(https_service_descriptor_init(NULL, &descriptor) ==
              HTTPS_SERVICE_NULL,
          "HTTPS descriptor init rejects null published output");
    CHECK(https_service_descriptor_init(&published, NULL) ==
              HTTPS_SERVICE_NULL,
          "HTTPS descriptor init rejects null descriptor");
    CHECK(!https_service_descriptor_published(NULL),
          "HTTPS descriptor published accessor rejects null descriptor");

    memset(&published, 0, sizeof(published));
    CHECK(https_service_descriptor_validate(&descriptor) ==
              HTTPS_SERVICE_OK &&
              https_service_descriptor_init(
                  &published, &descriptor) == HTTPS_SERVICE_OK &&
              https_service_descriptor_published(&published) &&
              published.version == HTTPS_SERVICE_DESCRIPTOR_VERSION &&
              published.id == HTTPS_SERVICE_ID &&
              strcmp(published.name, HTTPS_SERVICE_NAME) == 0 &&
              published.restart_budget ==
                  HTTPS_SERVICE_RESTART_BUDGET_DEFAULT &&
              published.readiness_deadline_ms ==
                  HTTPS_SERVICE_READINESS_DEADLINE_DEFAULT_MS &&
              published.envelope.connection_capacity ==
                  HTTPS_SERVICE_CONNECTION_CAPACITY &&
              published.envelope.tls_context_capacity ==
                  HTTPS_SERVICE_TLS_CONTEXT_CAPACITY &&
              published.envelope.http_parser_capacity ==
                  HTTPS_SERVICE_HTTP_PARSER_CAPACITY &&
              published.envelope.packet_pool_capacity ==
                  HTTPS_SERVICE_PACKET_POOL_CAPACITY_DEFAULT,
          "valid HTTPS descriptor publishes exact fixed fields");
    CHECK(https_service_descriptor_init(
              &published, &descriptor) ==
              HTTPS_SERVICE_ALREADY_PUBLISHED,
          "HTTPS descriptor init rejects double publication");

    memcpy(original, &published, sizeof(original));
    CHECK(https_service_descriptor_init(
              &published, &descriptor) ==
              HTTPS_SERVICE_ALREADY_PUBLISHED &&
              memcmp(&published, original, sizeof(original)) == 0,
          "HTTPS descriptor double publication leaves bytes unchanged");
}

static void check_init_failure(
    const struct https_service_descriptor *descriptor,
    enum https_service_status expected,
    const char *message)
{
    struct https_service_descriptor published;
    unsigned char original[sizeof(published)];

    memset(&published, 0xA5, sizeof(published));
    published.published = false;
    memcpy(original, &published, sizeof(original));
    CHECK(https_service_descriptor_init(&published, descriptor) == expected &&
              memcmp(&published, original, sizeof(original)) == 0,
          message);
}

static void test_invalid_descriptors_never_publish(void) {
    struct https_service_descriptor descriptor;
    struct https_service_resource_envelope envelope;

    descriptor = valid_descriptor();
    descriptor.version = HTTPS_SERVICE_DESCRIPTOR_VERSION + 1U;
    check_init_failure(&descriptor, HTTPS_SERVICE_BAD_VERSION,
                       "HTTPS descriptor rejects unknown version");

    descriptor = valid_descriptor();
    descriptor.id = 0U;
    check_init_failure(&descriptor, HTTPS_SERVICE_BAD_ID,
                       "HTTPS descriptor rejects zero id");

    descriptor = valid_descriptor();
    descriptor.id = HTTPS_SERVICE_ID + 1U;
    check_init_failure(&descriptor, HTTPS_SERVICE_BAD_ID,
                       "HTTPS descriptor rejects mutable identity id");

    descriptor = valid_descriptor();
    memset(descriptor.name, 0, sizeof(descriptor.name));
    check_init_failure(&descriptor, HTTPS_SERVICE_BAD_NAME,
                       "HTTPS descriptor rejects empty name");

    descriptor = valid_descriptor();
    memcpy(descriptor.name, "http", 5U);
    check_init_failure(&descriptor, HTTPS_SERVICE_BAD_NAME,
                       "HTTPS descriptor rejects non-https name");

    descriptor = valid_descriptor();
    descriptor.name[HTTPS_SERVICE_NAME_LENGTH] = 'x';
    check_init_failure(&descriptor, HTTPS_SERVICE_BAD_NAME,
                       "HTTPS descriptor rejects nonzero name tail");

    descriptor = valid_descriptor();
    descriptor.restart_budget = HTTPS_SERVICE_RESTART_BUDGET_MIN - 1U;
    check_init_failure(&descriptor, HTTPS_SERVICE_BAD_RESTART_BUDGET,
                       "HTTPS descriptor rejects zero restart budget");

    descriptor = valid_descriptor();
    descriptor.restart_budget = HTTPS_SERVICE_RESTART_BUDGET_MAX + 1U;
    check_init_failure(&descriptor, HTTPS_SERVICE_BAD_RESTART_BUDGET,
                       "HTTPS descriptor rejects over-limit restart budget");

    descriptor = valid_descriptor();
    descriptor.readiness_deadline_ms =
        HTTPS_SERVICE_READINESS_DEADLINE_MIN_MS - 1U;
    check_init_failure(&descriptor, HTTPS_SERVICE_BAD_READINESS_DEADLINE,
                       "HTTPS descriptor rejects zero readiness deadline");

    descriptor = valid_descriptor();
    descriptor.readiness_deadline_ms =
        HTTPS_SERVICE_READINESS_DEADLINE_MAX_MS + 1U;
    check_init_failure(&descriptor, HTTPS_SERVICE_BAD_READINESS_DEADLINE,
                       "HTTPS descriptor rejects over-limit readiness deadline");

    descriptor = valid_descriptor();
    descriptor.envelope.connection_capacity =
        HTTPS_SERVICE_CONNECTION_CAPACITY - 1U;
    check_init_failure(&descriptor, HTTPS_SERVICE_BAD_RESOURCE_ENVELOPE,
                       "HTTPS descriptor rejects low connection capacity");

    descriptor = valid_descriptor();
    descriptor.envelope.connection_capacity =
        HTTPS_SERVICE_CONNECTION_CAPACITY + 1U;
    check_init_failure(&descriptor, HTTPS_SERVICE_BAD_RESOURCE_ENVELOPE,
                       "HTTPS descriptor rejects high connection capacity");

    descriptor = valid_descriptor();
    descriptor.envelope.tls_context_capacity =
        HTTPS_SERVICE_TLS_CONTEXT_CAPACITY + 1U;
    check_init_failure(&descriptor, HTTPS_SERVICE_BAD_RESOURCE_ENVELOPE,
                       "HTTPS descriptor rejects over-limit TLS capacity");

    descriptor = valid_descriptor();
    descriptor.envelope.http_parser_capacity =
        HTTPS_SERVICE_HTTP_PARSER_CAPACITY + 1U;
    check_init_failure(&descriptor, HTTPS_SERVICE_BAD_RESOURCE_ENVELOPE,
                       "HTTPS descriptor rejects over-limit parser capacity");

    descriptor = valid_descriptor();
    descriptor.envelope.packet_pool_capacity =
        HTTPS_SERVICE_PACKET_POOL_CAPACITY_MIN - 1U;
    check_init_failure(&descriptor, HTTPS_SERVICE_BAD_RESOURCE_ENVELOPE,
                       "HTTPS descriptor rejects zero packet-pool capacity");

    descriptor = valid_descriptor();
    descriptor.envelope.packet_pool_capacity =
        HTTPS_SERVICE_PACKET_POOL_CAPACITY_MAX + 1U;
    check_init_failure(&descriptor, HTTPS_SERVICE_BAD_RESOURCE_ENVELOPE,
                       "HTTPS descriptor rejects over-limit packet-pool capacity");

    envelope = default_envelope();
    envelope.packet_pool_capacity = HTTPS_SERVICE_PACKET_POOL_CAPACITY_MIN;
    descriptor = make_descriptor(
        HTTPS_SERVICE_DESCRIPTOR_VERSION, HTTPS_SERVICE_ID,
        HTTPS_SERVICE_NAME, HTTPS_SERVICE_RESTART_BUDGET_DEFAULT,
        HTTPS_SERVICE_READINESS_DEADLINE_DEFAULT_MS, envelope);
    CHECK(https_service_descriptor_validate(&descriptor) ==
              HTTPS_SERVICE_OK,
          "HTTPS descriptor accepts bounded minimum packet-pool capacity");
}

static void check_start_failure(
    const struct https_service_descriptor *descriptor,
    const struct https_service_readiness *readiness,
    enum https_service_status expected,
    bool expected_ready,
    const char *message)
{
    bool ready = true;

    CHECK(https_service_start_check(
              descriptor, readiness, &ready) == expected &&
              ready == expected_ready,
          message);
}

static void test_start_readiness(void) {
    struct https_service_descriptor source = valid_descriptor();
    struct https_service_descriptor descriptor;
    struct https_service_descriptor unpublished = valid_descriptor();
    struct https_service_readiness readiness;
    bool ready;

    memset(&descriptor, 0, sizeof(descriptor));
    CHECK(https_service_descriptor_init(
              &descriptor, &source) == HTTPS_SERVICE_OK &&
              https_service_descriptor_published(&descriptor),
          "HTTPS start check fixture publishes a valid descriptor");
    readiness = make_readiness(
        HTTPS_SERVICE_REQUIRED_DEPENDENCY_MASK);
    CHECK(https_service_start_check(
              &descriptor, &readiness, &ready) == HTTPS_SERVICE_OK &&
              ready,
          "HTTPS descriptor allows start when every dependency is ready");

    readiness = make_readiness(
        HTTPS_SERVICE_REQUIRED_DEPENDENCY_MASK &
        ~HTTPS_SERVICE_DEPENDENCY_NETWORK_READY);
    check_start_failure(
        &descriptor, &readiness,
        HTTPS_SERVICE_DEPENDENCY_UNAVAILABLE, false,
        "missing network readiness prevents HTTPS start");

    readiness = make_readiness(
        HTTPS_SERVICE_REQUIRED_DEPENDENCY_MASK &
        ~HTTPS_SERVICE_DEPENDENCY_ENTROPY_READY);
    check_start_failure(
        &descriptor, &readiness,
        HTTPS_SERVICE_DEPENDENCY_UNAVAILABLE, false,
        "missing entropy readiness prevents HTTPS start");

    readiness = make_readiness(
        HTTPS_SERVICE_REQUIRED_DEPENDENCY_MASK &
        ~HTTPS_SERVICE_DEPENDENCY_CERTIFICATE_READY);
    check_start_failure(
        &descriptor, &readiness,
        HTTPS_SERVICE_DEPENDENCY_UNAVAILABLE, false,
        "missing certificate readiness prevents HTTPS start");

    readiness = make_readiness(
        HTTPS_SERVICE_DEPENDENCY_ENTROPY_READY |
        HTTPS_SERVICE_DEPENDENCY_CERTIFICATE_READY);
    check_start_failure(
        &descriptor, &readiness,
        HTTPS_SERVICE_DEPENDENCY_UNAVAILABLE, false,
        "one missing dependency prevents HTTPS start despite other ready deps");

    readiness = make_readiness(
        HTTPS_SERVICE_REQUIRED_DEPENDENCY_MASK | 0x8U);
    check_start_failure(
        &descriptor, &readiness,
        HTTPS_SERVICE_BAD_DEPENDENCY, true,
        "unknown readiness dependency bits are rejected");

    readiness = make_readiness(
        HTTPS_SERVICE_REQUIRED_DEPENDENCY_MASK);
    readiness.version = HTTPS_SERVICE_READINESS_VERSION + 1U;
    check_start_failure(
        &descriptor, &readiness,
        HTTPS_SERVICE_BAD_VERSION, true,
        "HTTPS start check rejects unknown readiness version");

    readiness = make_readiness(
        HTTPS_SERVICE_REQUIRED_DEPENDENCY_MASK);
    CHECK(https_service_start_check(
              &unpublished, &readiness, &ready) ==
              HTTPS_SERVICE_NOT_PUBLISHED &&
              ready,
          "HTTPS start check rejects unpublished descriptor");
    CHECK(https_service_start_check(
              NULL, &readiness, &ready) == HTTPS_SERVICE_NULL &&
              ready,
          "HTTPS start check rejects null descriptor");
    CHECK(https_service_start_check(
              &descriptor, NULL, &ready) == HTTPS_SERVICE_NULL &&
              ready,
          "HTTPS start check rejects null readiness");
    CHECK(https_service_start_check(
              &descriptor, &readiness, NULL) == HTTPS_SERVICE_NULL,
          "HTTPS start check rejects null ready output");
}

static void test_no_allocation(void) {
    struct https_service_descriptor descriptor = valid_descriptor();
    struct https_service_descriptor published;
    struct https_service_readiness readiness;
    bool ready;

    memset(&published, 0, sizeof(published));
    readiness = make_readiness(
        HTTPS_SERVICE_REQUIRED_DEPENDENCY_MASK);
    (void)https_service_descriptor_validate(&descriptor);
    (void)https_service_descriptor_init(&published, &descriptor);
    (void)https_service_start_check(
        &published, &readiness, &ready);
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "HTTPS descriptor boundary performs no allocation");
}

int main(void) {
    test_status_strings();
    test_null_and_publication();
    test_invalid_descriptors_never_publish();
    test_start_readiness();
    test_no_allocation();

    if (failures != 0) {
        fprintf(stderr, "HTTPS service descriptor host test: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("HTTPS service descriptor host test: ALL OK\n");
    return 0;
}

#else

int main(void) {
    return 0;
}

#endif
