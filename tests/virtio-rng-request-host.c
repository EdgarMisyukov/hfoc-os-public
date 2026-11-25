#include "virtio_rng_request.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_VIRTIO_RNG_REQUEST_HOST_TEST

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

static bool status_string_matches(
    enum virtio_rng_request_api_status status,
    const char *string)
{
    switch (status) {
    case VIRTIO_RNG_REQUEST_API_OK:
        return strcmp(string, "VIRTIO_RNG_REQUEST_API_OK") == 0;
    case VIRTIO_RNG_REQUEST_API_NULL:
        return strcmp(string, "VIRTIO_RNG_REQUEST_API_NULL") == 0;
    case VIRTIO_RNG_REQUEST_API_BAD_STATE:
        return strcmp(string, "VIRTIO_RNG_REQUEST_API_BAD_STATE") == 0;
    case VIRTIO_RNG_REQUEST_API_BAD_VERSION:
        return strcmp(string, "VIRTIO_RNG_REQUEST_API_BAD_VERSION") == 0;
    case VIRTIO_RNG_REQUEST_API_BAD_GENERATION:
        return strcmp(
            string, "VIRTIO_RNG_REQUEST_API_BAD_GENERATION") == 0;
    case VIRTIO_RNG_REQUEST_API_ALREADY_PUBLISHED:
        return strcmp(
            string, "VIRTIO_RNG_REQUEST_API_ALREADY_PUBLISHED") == 0;
    case VIRTIO_RNG_REQUEST_API_UNPUBLISHED:
        return strcmp(
            string, "VIRTIO_RNG_REQUEST_API_UNPUBLISHED") == 0;
    case VIRTIO_RNG_REQUEST_API_BUSY:
        return strcmp(string, "VIRTIO_RNG_REQUEST_API_BUSY") == 0;
    case VIRTIO_RNG_REQUEST_API_ZERO_LENGTH:
        return strcmp(
            string, "VIRTIO_RNG_REQUEST_API_ZERO_LENGTH") == 0;
    case VIRTIO_RNG_REQUEST_API_TOO_LONG:
        return strcmp(
            string, "VIRTIO_RNG_REQUEST_API_TOO_LONG") == 0;
    case VIRTIO_RNG_REQUEST_API_NULL_DESTINATION:
        return strcmp(
            string, "VIRTIO_RNG_REQUEST_API_NULL_DESTINATION") == 0;
    case VIRTIO_RNG_REQUEST_API_OVERLAPPING_DESTINATION:
        return strcmp(
            string, "VIRTIO_RNG_REQUEST_API_OVERLAPPING_DESTINATION") == 0;
    case VIRTIO_RNG_REQUEST_API_INVALID_CONTEXT:
        return strcmp(
            string, "VIRTIO_RNG_REQUEST_API_INVALID_CONTEXT") == 0;
    case VIRTIO_RNG_REQUEST_API_INTERRUPT_CONTEXT:
        return strcmp(
            string, "VIRTIO_RNG_REQUEST_API_INTERRUPT_CONTEXT") == 0;
    case VIRTIO_RNG_REQUEST_API_INVALID_RESULT:
        return strcmp(
            string, "VIRTIO_RNG_REQUEST_API_INVALID_RESULT") == 0;
    case VIRTIO_RNG_REQUEST_API_NOT_PENDING:
        return strcmp(
            string, "VIRTIO_RNG_REQUEST_API_NOT_PENDING") == 0;
    default:
        return false;
    }
}

static struct virtio_rng_request make_request(
    uint8_t *destination,
    uint32_t destination_length,
    uint32_t context_kind,
    uint32_t generation,
    enum virtio_rng_request_result result)
{
    struct virtio_rng_request request;

    memset(&request, 0, sizeof(request));
    request.version = VIRTIO_RNG_REQUEST_VERSION;
    request.destination = destination;
    request.destination_length = destination_length;
    request.context.kind = context_kind;
    request.context.id = 7U;
    request.generation = generation;
    request.result = result;
    return request;
}

static void test_status_strings(void)
{
    static const enum virtio_rng_request_api_status statuses[] = {
        VIRTIO_RNG_REQUEST_API_OK,
        VIRTIO_RNG_REQUEST_API_NULL,
        VIRTIO_RNG_REQUEST_API_BAD_STATE,
        VIRTIO_RNG_REQUEST_API_BAD_VERSION,
        VIRTIO_RNG_REQUEST_API_BAD_GENERATION,
        VIRTIO_RNG_REQUEST_API_ALREADY_PUBLISHED,
        VIRTIO_RNG_REQUEST_API_UNPUBLISHED,
        VIRTIO_RNG_REQUEST_API_BUSY,
        VIRTIO_RNG_REQUEST_API_ZERO_LENGTH,
        VIRTIO_RNG_REQUEST_API_TOO_LONG,
        VIRTIO_RNG_REQUEST_API_NULL_DESTINATION,
        VIRTIO_RNG_REQUEST_API_OVERLAPPING_DESTINATION,
        VIRTIO_RNG_REQUEST_API_INVALID_CONTEXT,
        VIRTIO_RNG_REQUEST_API_INTERRUPT_CONTEXT,
        VIRTIO_RNG_REQUEST_API_INVALID_RESULT,
        VIRTIO_RNG_REQUEST_API_NOT_PENDING,
    };
    size_t index;

    for (index = 0U; index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        CHECK(status_string_matches(
                  statuses[index],
                  virtio_rng_request_api_status_string(statuses[index])),
              "request API status string is exact");
    }
    CHECK(virtio_rng_request_api_status_string(
              (enum virtio_rng_request_api_status)999) == NULL,
          "request API rejects undefined status string");
}

static void test_init(void)
{
    struct virtio_rng_request_api api;

    CHECK(virtio_rng_request_api_init(NULL, 7U) ==
              VIRTIO_RNG_REQUEST_API_NULL,
          "request API init rejects null");

    memset(&api, 0, sizeof(api));
    CHECK(virtio_rng_request_api_init(&api, 0U) ==
              VIRTIO_RNG_REQUEST_API_BAD_GENERATION &&
          !api.initialized,
          "request API init rejects zero generation");

    memset(&api, 0, sizeof(api));
    CHECK(virtio_rng_request_api_init(
              &api, VIRTIO_RNG_REQUEST_GENERATION_MAX + 1U) ==
              VIRTIO_RNG_REQUEST_API_BAD_GENERATION &&
          !api.initialized,
          "request API init rejects over-limit generation");

    memset(&api, 0, sizeof(api));
    CHECK(virtio_rng_request_api_init(&api, 7U) ==
              VIRTIO_RNG_REQUEST_API_OK &&
          virtio_rng_request_api_initialized(&api) &&
          api.version == VIRTIO_RNG_REQUEST_VERSION &&
          api.generation == 7U &&
          !virtio_rng_request_api_pending(&api) &&
          virtio_rng_request_api_generation(&api) == 7U,
          "valid request API init publishes exact generation");

    CHECK(virtio_rng_request_api_init(&api, 8U) ==
              VIRTIO_RNG_REQUEST_API_ALREADY_PUBLISHED &&
          api.generation == 7U,
          "duplicate request API init is rejected without mutation");
}

static void test_bad_state(void)
{
    struct virtio_rng_request_api api;
    struct virtio_rng_request request;
    uint8_t destination[8];

    memset(destination, 0x11, sizeof(destination));
    request = make_request(
        destination, (uint32_t)sizeof(destination),
        VIRTIO_RNG_CALLER_CONTEXT_PROCESS, 7U,
        VIRTIO_RNG_REQUEST_RESULT_PENDING);

    memset(&api, 0, sizeof(api));
    api.initialized = true;
    api.version = 0U;
    api.generation = 7U;
    CHECK(!virtio_rng_request_api_initialized(&api) &&
          virtio_rng_request_api_submit(&api, &request) ==
              VIRTIO_RNG_REQUEST_API_BAD_STATE,
          "request API detects corrupted version state");

    memset(&api, 0, sizeof(api));
    api.initialized = true;
    api.version = VIRTIO_RNG_REQUEST_VERSION;
    api.generation = 0U;
    CHECK(!virtio_rng_request_api_initialized(&api) &&
          virtio_rng_request_api_submit(&api, &request) ==
              VIRTIO_RNG_REQUEST_API_BAD_STATE,
          "request API detects invalid API generation");
}

static void test_request_rejections(void)
{
    struct virtio_rng_request_api api;
    struct virtio_rng_request request;
    uint8_t destination[16];

    memset(destination, 0x22, sizeof(destination));
    memset(&api, 0, sizeof(api));
    CHECK(virtio_rng_request_api_init(&api, 7U) ==
              VIRTIO_RNG_REQUEST_API_OK,
          "request rejection fixture initializes API");

    CHECK(virtio_rng_request_api_submit(NULL, NULL) ==
              VIRTIO_RNG_REQUEST_API_NULL,
          "request submit rejects null arguments");
    request = make_request(
        destination, (uint32_t)sizeof(destination),
        VIRTIO_RNG_CALLER_CONTEXT_PROCESS, 7U,
        VIRTIO_RNG_REQUEST_RESULT_PENDING);
    CHECK(virtio_rng_request_api_submit(NULL, &request) ==
              VIRTIO_RNG_REQUEST_API_NULL,
          "request submit rejects null API");

    {
        struct virtio_rng_request_api unpublished;

        memset(&unpublished, 0, sizeof(unpublished));
        CHECK(virtio_rng_request_api_submit(&unpublished, &request) ==
                  VIRTIO_RNG_REQUEST_API_UNPUBLISHED,
              "request submit rejects unpublished API");
    }

    request.version = 0U;
    CHECK(virtio_rng_request_api_submit(&api, &request) ==
              VIRTIO_RNG_REQUEST_API_BAD_VERSION &&
          !api.pending,
          "request submit rejects bad request version");

    request.version = VIRTIO_RNG_REQUEST_VERSION;
    request.generation = 6U;
    CHECK(virtio_rng_request_api_submit(&api, &request) ==
              VIRTIO_RNG_REQUEST_API_BAD_GENERATION &&
          !api.pending,
          "request submit rejects generation mismatch");

    request.generation = 7U;
    request.destination = NULL;
    CHECK(virtio_rng_request_api_submit(&api, &request) ==
              VIRTIO_RNG_REQUEST_API_NULL_DESTINATION &&
          !api.pending,
          "request submit rejects null destination");

    request.destination = destination;
    request.destination_length = 0U;
    CHECK(virtio_rng_request_api_submit(&api, &request) ==
              VIRTIO_RNG_REQUEST_API_ZERO_LENGTH &&
          !api.pending,
          "request submit rejects zero destination length");

    request.destination_length = VIRTIO_RNG_REQUEST_LENGTH_MAX + 1U;
    CHECK(virtio_rng_request_api_submit(&api, &request) ==
              VIRTIO_RNG_REQUEST_API_TOO_LONG &&
          !api.pending,
          "request submit rejects oversized destination");

    request.destination_length = (uint32_t)sizeof(destination);
    request.context.kind = VIRTIO_RNG_CALLER_CONTEXT_UNKNOWN;
    CHECK(virtio_rng_request_api_submit(&api, &request) ==
              VIRTIO_RNG_REQUEST_API_INVALID_CONTEXT &&
          !api.pending,
          "request submit rejects unknown caller context");

    request.context.kind = VIRTIO_RNG_CALLER_CONTEXT_INTERRUPT;
    CHECK(virtio_rng_request_api_submit(&api, &request) ==
              VIRTIO_RNG_REQUEST_API_INTERRUPT_CONTEXT &&
          !api.pending,
          "request submit rejects interrupt caller context");

    request.context.kind = VIRTIO_RNG_CALLER_CONTEXT_PROCESS;
    request.result = VIRTIO_RNG_REQUEST_RESULT_READY;
    CHECK(virtio_rng_request_api_submit(&api, &request) ==
              VIRTIO_RNG_REQUEST_API_INVALID_RESULT &&
          !api.pending,
          "request submit rejects non-pending completion result");
}

static void test_overlap(void)
{
    struct virtio_rng_request_api api;
    struct virtio_rng_request request;

    memset(&api, 0, sizeof(api));
    CHECK(virtio_rng_request_api_init(&api, 7U) ==
              VIRTIO_RNG_REQUEST_API_OK,
          "overlap fixture initializes API");

    request = make_request(
        (uint8_t *)&request, (uint32_t)sizeof(request),
        VIRTIO_RNG_CALLER_CONTEXT_PROCESS, 7U,
        VIRTIO_RNG_REQUEST_RESULT_PENDING);
    CHECK(virtio_rng_request_api_submit(&api, &request) ==
              VIRTIO_RNG_REQUEST_API_OVERLAPPING_DESTINATION &&
          !api.pending,
          "request submit rejects destination overlapping descriptor");

    request = make_request(
        (uint8_t *)&api, 1U,
        VIRTIO_RNG_CALLER_CONTEXT_PROCESS, 7U,
        VIRTIO_RNG_REQUEST_RESULT_PENDING);
    CHECK(virtio_rng_request_api_submit(&api, &request) ==
              VIRTIO_RNG_REQUEST_API_OVERLAPPING_DESTINATION &&
          !api.pending,
          "request submit rejects destination overlapping API object");
}

static void test_success_single_pending(void)
{
    struct virtio_rng_request_api api;
    struct virtio_rng_request request;
    const struct virtio_rng_request *current;
    uint8_t first[16];
    uint8_t second[16];

    memset(first, 0x33, sizeof(first));
    memset(second, 0x44, sizeof(second));
    memset(&api, 0, sizeof(api));
    CHECK(virtio_rng_request_api_init(&api, 3U) ==
              VIRTIO_RNG_REQUEST_API_OK,
          "success fixture initializes API");

    request = make_request(
        first, (uint32_t)sizeof(first),
        VIRTIO_RNG_CALLER_CONTEXT_PROCESS, 3U,
        VIRTIO_RNG_REQUEST_RESULT_PENDING);
    CHECK(virtio_rng_request_api_submit(&api, &request) ==
              VIRTIO_RNG_REQUEST_API_OK &&
          virtio_rng_request_api_pending(&api),
          "valid request submits as pending");

    current = virtio_rng_request_api_current(&api);
    CHECK(current != NULL &&
          current->destination == first &&
          current->destination_length == sizeof(first) &&
          current->context.kind == VIRTIO_RNG_CALLER_CONTEXT_PROCESS &&
          current->context.id == 7U &&
          current->generation == 3U &&
          current->result == VIRTIO_RNG_REQUEST_RESULT_PENDING,
          "pending request publishes exact descriptor");
    CHECK(first[0] == 0x33U && first[15] == 0x33U,
          "request submission does not mutate destination");

    CHECK(virtio_rng_request_api_complete(
              &api, VIRTIO_RNG_REQUEST_RESULT_PENDING) ==
              VIRTIO_RNG_REQUEST_API_INVALID_RESULT &&
          virtio_rng_request_api_pending(&api),
          "request completion rejects pending result");
    CHECK(virtio_rng_request_api_complete(
              &api, VIRTIO_RNG_REQUEST_RESULT_UNKNOWN) ==
              VIRTIO_RNG_REQUEST_API_INVALID_RESULT &&
          virtio_rng_request_api_pending(&api),
          "request completion rejects unknown result");

    request = make_request(
        first, (uint32_t)sizeof(first),
        VIRTIO_RNG_CALLER_CONTEXT_KERNEL, 3U,
        VIRTIO_RNG_REQUEST_RESULT_PENDING);
    CHECK(virtio_rng_request_api_submit(&api, &request) ==
              VIRTIO_RNG_REQUEST_API_OVERLAPPING_DESTINATION &&
          virtio_rng_request_api_pending(&api),
          "pending API rejects overlapping destination");

    request = make_request(
        second, (uint32_t)sizeof(second),
        VIRTIO_RNG_CALLER_CONTEXT_KERNEL, 3U,
        VIRTIO_RNG_REQUEST_RESULT_PENDING);
    CHECK(virtio_rng_request_api_submit(&api, &request) ==
              VIRTIO_RNG_REQUEST_API_BUSY &&
          virtio_rng_request_api_pending(&api) &&
          virtio_rng_request_api_current(&api)->destination == first,
          "pending API rejects non-overlapping second request");

    CHECK(virtio_rng_request_api_complete(
              &api, VIRTIO_RNG_REQUEST_RESULT_READY) ==
              VIRTIO_RNG_REQUEST_API_OK &&
          !virtio_rng_request_api_pending(&api) &&
          virtio_rng_request_api_current(&api)->result ==
              VIRTIO_RNG_REQUEST_RESULT_READY,
          "ready completion clears pending and records result");

    request = make_request(
        second, (uint32_t)sizeof(second),
        VIRTIO_RNG_CALLER_CONTEXT_SERVICE, 3U,
        VIRTIO_RNG_REQUEST_RESULT_PENDING);
    CHECK(virtio_rng_request_api_submit(&api, &request) ==
              VIRTIO_RNG_REQUEST_API_OK &&
          virtio_rng_request_api_pending(&api) &&
          virtio_rng_request_api_current(&api)->destination == second,
          "completed API accepts a fresh request");

    CHECK(virtio_rng_request_api_complete(
              &api, VIRTIO_RNG_REQUEST_RESULT_FAILED) ==
              VIRTIO_RNG_REQUEST_API_OK &&
          !virtio_rng_request_api_pending(&api) &&
          virtio_rng_request_api_current(&api)->result ==
              VIRTIO_RNG_REQUEST_RESULT_FAILED,
          "failed completion clears pending and records result");
}

static void test_completion_without_pending(void)
{
    struct virtio_rng_request_api api;

    memset(&api, 0, sizeof(api));
    CHECK(virtio_rng_request_api_init(&api, 7U) ==
              VIRTIO_RNG_REQUEST_API_OK,
          "completion fixture initializes API");
    CHECK(virtio_rng_request_api_complete(
              &api, VIRTIO_RNG_REQUEST_RESULT_READY) ==
              VIRTIO_RNG_REQUEST_API_NOT_PENDING,
          "completion without pending request is rejected");
}

static void test_no_allocation(void)
{
    struct virtio_rng_request_api api;
    struct virtio_rng_request request;
    uint8_t destination[8];

    memset(destination, 0x55, sizeof(destination));
    memset(&api, 0, sizeof(api));
    alloc_attempts = 0U;
    free_attempts = 0U;
    CHECK(virtio_rng_request_api_init(&api, 9U) ==
              VIRTIO_RNG_REQUEST_API_OK,
          "no-allocation fixture initializes API");
    request = make_request(
        destination, (uint32_t)sizeof(destination),
        VIRTIO_RNG_CALLER_CONTEXT_KERNEL, 9U,
        VIRTIO_RNG_REQUEST_RESULT_PENDING);
    CHECK(virtio_rng_request_api_submit(&api, &request) ==
              VIRTIO_RNG_REQUEST_API_OK,
          "no-allocation fixture submits request");
    CHECK(virtio_rng_request_api_complete(
              &api, VIRTIO_RNG_REQUEST_RESULT_READY) ==
              VIRTIO_RNG_REQUEST_API_OK,
          "no-allocation fixture completes request");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "request API performs no allocation or free");
}

int main(void)
{
    test_status_strings();
    test_init();
    test_bad_state();
    test_request_rejections();
    test_overlap();
    test_success_single_pending();
    test_completion_without_pending();
    test_no_allocation();

    if (failures == 0) {
        printf("Virtio rng request host tests: ALL OK\n");
        return 0;
    }
    return 1;
}

#endif
