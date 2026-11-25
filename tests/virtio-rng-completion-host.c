#include "virtio_rng_completion.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_VIRTIO_RNG_COMPLETION_HOST_TEST

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

struct test_barrier_state {
    unsigned int calls;
};

struct test_reclaim_state {
    unsigned int calls;
};

struct test_zeroize_state {
    unsigned int calls;
    bool allow;
};

struct completion_fixture {
    struct virtio_available_ring available;
    struct virtio_used_ring used;
    struct test_barrier_state barrier;
    struct test_reclaim_state reclaim;
    struct test_zeroize_state zeroize;
    struct virtio_rng_feature_profile profile;
    struct virtio_rng_queue_composition composition;
    struct virtio_rng_request_api api;
    struct virtio_descriptor_token descriptor;
    uint8_t destination[16];
};

static void test_fence(void *context)
{
    struct test_barrier_state *state =
        (struct test_barrier_state *)context;

    ++state->calls;
}

static bool test_reclaim(
    void *context,
    const struct virtio_used_element *element,
    const struct virtio_used_token *token)
{
    struct test_reclaim_state *state =
        (struct test_reclaim_state *)context;

    (void)element;
    (void)token;
    ++state->calls;
    return true;
}

static bool test_zeroize(
    void *context,
    uint64_t physical_base,
    uint64_t length)
{
    struct test_zeroize_state *state =
        (struct test_zeroize_state *)context;

    (void)physical_base;
    (void)length;
    if (state->allow) {
        ++state->calls;
    }
    return state->allow;
}

static struct virtio_pci_identity make_identity(void)
{
    struct virtio_pci_identity identity;

    memset(&identity, 0, sizeof(identity));
    identity.version = VIRTIO_PCI_IDENTITY_VERSION;
    identity.vendor_id = VIRTIO_PCI_VENDOR_ID;
    identity.device_id = VIRTIO_PCI_DEVICE_ID_RNG_MODERN;
    identity.bdf.bus = 0U;
    identity.bdf.device = 0U;
    identity.bdf.function = 0U;
    identity.modern = 1U;
    return identity;
}

static struct virtio_rng_feature_profile make_profile(void)
{
    struct virtio_rng_feature_profile profile;
    struct virtio_pci_identity identity;
    struct virtio_rng_feature_bitset offered;
    struct virtio_rng_topology topology;

    identity = make_identity();
    offered.low = VIRTIO_RNG_REQUIRED_FEATURE_LO_MASK;
    offered.high = VIRTIO_RNG_VERSION_1_HIGH_BIT;
    memset(&topology, 0, sizeof(topology));
    topology.version = VIRTIO_RNG_TOPOLOGY_VERSION;
    topology.request_queue_count = VIRTIO_RNG_REQUEST_QUEUE_COUNT_MAX;
    topology.buffer_writer = VIRTIO_RNG_BUFFER_WRITER_DEVICE;

    memset(&profile, 0, sizeof(profile));
    CHECK(virtio_rng_feature_profile_activate(
              &profile, &identity, &offered, &topology,
              1U) == VIRTIO_RNG_FEATURE_PROFILE_OK,
          "host profile helper activates valid modern RNG profile");
    return profile;
}

static struct virtio_rng_queue_config make_queue_config(
    struct completion_fixture *fixture)
{
    struct virtio_rng_queue_config config;

    memset(&config, 0, sizeof(config));
    config.queue_index = VIRTIO_RNG_QUEUE_COMPOSITION_QUEUE_INDEX;
    config.queue_size = 4U;
    config.event_index = false;
    config.region_base = UINT64_C(0x100000);
    config.region_length = UINT64_C(0x1000);
    config.available_ring = &fixture->available;
    config.used_ring = &fixture->used;
    config.barrier.fence = test_fence;
    config.barrier.context = &fixture->barrier;
    config.reclaim.reclaim = test_reclaim;
    config.reclaim.context = &fixture->reclaim;
    config.max_used_length = 1024U;
    config.dma_window.base = UINT64_C(0x100000);
    config.dma_window.length = UINT64_C(0x1000);
    config.dma_window.alignment = 16U;
    config.dma_zeroize.zeroize = test_zeroize;
    config.dma_zeroize.context = &fixture->zeroize;
    config.dma_capacity = 4U;
    return config;
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
    request.context.id = 11U;
    request.generation = generation;
    request.result = result;
    return request;
}

static void setup_fixture(
    struct completion_fixture *fixture,
    uint32_t destination_length)
{
    struct virtio_rng_queue_config config;
    struct virtio_rng_queue *queue;
    struct virtio_rng_request request;
    uint16_t chain[1];
    struct virtio_available_token available_token;

    memset(fixture, 0, sizeof(*fixture));
    fixture->zeroize.allow = true;
    fixture->profile = make_profile();
    config = make_queue_config(fixture);
    CHECK(virtio_rng_queue_composition_activate(
              &fixture->composition,
              &fixture->profile,
              &config,
              7U) == VIRTIO_RNG_QUEUE_COMPOSITION_OK,
          "completion fixture activates request queue");
    CHECK(virtio_rng_request_api_init(&fixture->api, 7U) ==
              VIRTIO_RNG_REQUEST_API_OK,
          "completion fixture initializes request API");

    memset(fixture->destination, 0x5A, sizeof(fixture->destination));
    queue = &fixture->composition.queue;
    CHECK(virtio_descriptor_ownership_reserve(
              &queue->descriptors, &fixture->descriptor) ==
              VIRTIO_DESCRIPTOR_OWNERSHIP_OK,
          "completion fixture reserves request descriptor");
    CHECK(virtio_descriptor_ownership_publish(
              &queue->descriptors, &fixture->descriptor) ==
              VIRTIO_DESCRIPTOR_OWNERSHIP_OK,
          "completion fixture publishes request descriptor");

    chain[0] = (uint16_t)fixture->descriptor.index;
    CHECK(virtio_available_publication_prepare(
              &queue->available,
              chain,
              1U,
              &available_token) == VIRTIO_AVAILABLE_PUBLICATION_OK,
          "completion fixture prepares available entry");
    CHECK(virtio_available_publication_publish(
              &queue->available,
              &available_token) == VIRTIO_AVAILABLE_PUBLICATION_OK,
          "completion fixture publishes available entry");

    request = make_request(
        fixture->destination,
        destination_length,
        VIRTIO_RNG_CALLER_CONTEXT_PROCESS,
        7U,
        VIRTIO_RNG_REQUEST_RESULT_PENDING);
    CHECK(virtio_rng_request_api_submit(&fixture->api, &request) ==
              VIRTIO_RNG_REQUEST_API_OK,
          "completion fixture submits pending request");
}

static void set_used_entry(
    struct completion_fixture *fixture,
    uint32_t id,
    uint32_t length)
{
    fixture->used.idx = 1U;
    fixture->used.ring[0].id = id;
    fixture->used.ring[0].len = length;
}

static bool status_string_matches(
    enum virtio_rng_completion_status status,
    const char *string)
{
    switch (status) {
    case VIRTIO_RNG_COMPLETION_OK:
        return strcmp(string, "VIRTIO_RNG_COMPLETION_OK") == 0;
    case VIRTIO_RNG_COMPLETION_NULL:
        return strcmp(string, "VIRTIO_RNG_COMPLETION_NULL") == 0;
    case VIRTIO_RNG_COMPLETION_INVALID_REQUEST_API:
        return strcmp(
            string,
            "VIRTIO_RNG_COMPLETION_INVALID_REQUEST_API") == 0;
    case VIRTIO_RNG_COMPLETION_INVALID_COMPOSITION:
        return strcmp(
            string,
            "VIRTIO_RNG_COMPLETION_INVALID_COMPOSITION") == 0;
    case VIRTIO_RNG_COMPLETION_INVALID_DESCRIPTOR:
        return strcmp(
            string,
            "VIRTIO_RNG_COMPLETION_INVALID_DESCRIPTOR") == 0;
    case VIRTIO_RNG_COMPLETION_NOT_PENDING:
        return strcmp(
            string, "VIRTIO_RNG_COMPLETION_NOT_PENDING") == 0;
    case VIRTIO_RNG_COMPLETION_INVALID_STATE:
        return strcmp(
            string, "VIRTIO_RNG_COMPLETION_INVALID_STATE") == 0;
    case VIRTIO_RNG_COMPLETION_DUPLICATE_COMPLETION:
        return strcmp(
            string,
            "VIRTIO_RNG_COMPLETION_DUPLICATE_COMPLETION") == 0;
    case VIRTIO_RNG_COMPLETION_USED_INVALID:
        return strcmp(
            string, "VIRTIO_RNG_COMPLETION_USED_INVALID") == 0;
    case VIRTIO_RNG_COMPLETION_SHORT_COMPLETION:
        return strcmp(
            string, "VIRTIO_RNG_COMPLETION_SHORT_COMPLETION") == 0;
    case VIRTIO_RNG_COMPLETION_OVERSIZED_COMPLETION:
        return strcmp(
            string,
            "VIRTIO_RNG_COMPLETION_OVERSIZED_COMPLETION") == 0;
    case VIRTIO_RNG_COMPLETION_STALE_COMPLETION:
        return strcmp(
            string, "VIRTIO_RNG_COMPLETION_STALE_COMPLETION") == 0;
    case VIRTIO_RNG_COMPLETION_RELEASE_FAILED:
        return strcmp(
            string, "VIRTIO_RNG_COMPLETION_RELEASE_FAILED") == 0;
    default:
        return false;
    }
}

static void test_status_strings(void)
{
    static const enum virtio_rng_completion_status statuses[] = {
        VIRTIO_RNG_COMPLETION_OK,
        VIRTIO_RNG_COMPLETION_NULL,
        VIRTIO_RNG_COMPLETION_INVALID_REQUEST_API,
        VIRTIO_RNG_COMPLETION_INVALID_COMPOSITION,
        VIRTIO_RNG_COMPLETION_INVALID_DESCRIPTOR,
        VIRTIO_RNG_COMPLETION_NOT_PENDING,
        VIRTIO_RNG_COMPLETION_INVALID_STATE,
        VIRTIO_RNG_COMPLETION_DUPLICATE_COMPLETION,
        VIRTIO_RNG_COMPLETION_USED_INVALID,
        VIRTIO_RNG_COMPLETION_SHORT_COMPLETION,
        VIRTIO_RNG_COMPLETION_OVERSIZED_COMPLETION,
        VIRTIO_RNG_COMPLETION_STALE_COMPLETION,
        VIRTIO_RNG_COMPLETION_RELEASE_FAILED,
    };
    size_t index;

    for (index = 0U; index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        enum virtio_rng_completion_status status = statuses[index];
        const char *string =
            virtio_rng_completion_status_string(status);

        CHECK(string != NULL && status_string_matches(status, string),
              "virtio rng completion status string is exact");
    }
    CHECK(virtio_rng_completion_status_string(
              (enum virtio_rng_completion_status)99U) == NULL,
          "virtio rng completion rejects undefined status");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio rng completion status handling allocates nothing");
}

static void test_null_and_state(void)
{
    struct completion_fixture fixture;
    struct virtio_rng_request_api unpublished;
    struct virtio_rng_completion_result result;
    uint8_t payload[16];

    memset(payload, 0x36, sizeof(payload));
    setup_fixture(&fixture, 8U);

    CHECK(virtio_rng_completion_complete(
              NULL, &fixture.composition, &fixture.descriptor,
              payload, &result) == VIRTIO_RNG_COMPLETION_NULL,
          "completion rejects null API");
    CHECK(virtio_rng_completion_complete(
              &fixture.api, NULL, &fixture.descriptor,
              payload, &result) == VIRTIO_RNG_COMPLETION_NULL,
          "completion rejects null composition");
    CHECK(virtio_rng_completion_complete(
              &fixture.api, &fixture.composition, NULL,
              payload, &result) == VIRTIO_RNG_COMPLETION_NULL,
          "completion rejects null descriptor");
    CHECK(virtio_rng_completion_complete(
              &fixture.api, &fixture.composition, &fixture.descriptor,
              NULL, &result) == VIRTIO_RNG_COMPLETION_NULL,
          "completion rejects null payload");
    CHECK(virtio_rng_completion_complete(
              &fixture.api, &fixture.composition, &fixture.descriptor,
              payload, NULL) == VIRTIO_RNG_COMPLETION_NULL,
          "completion rejects null result");

    memset(&unpublished, 0, sizeof(unpublished));
    CHECK(virtio_rng_completion_complete(
              &unpublished, &fixture.composition, &fixture.descriptor,
              payload, &result) ==
              VIRTIO_RNG_COMPLETION_INVALID_REQUEST_API,
          "completion rejects uninitialized request API");

    CHECK(virtio_rng_request_api_complete(
              &fixture.api, VIRTIO_RNG_REQUEST_RESULT_FAILED) ==
              VIRTIO_RNG_REQUEST_API_OK,
          "not-pending fixture clears request");
    CHECK(virtio_rng_completion_complete(
              &fixture.api, &fixture.composition, &fixture.descriptor,
              payload, &result) == VIRTIO_RNG_COMPLETION_NOT_PENDING,
          "completion rejects non-pending request");
}

static void test_invalid_composition_and_descriptor(void)
{
    struct completion_fixture fixture;
    struct virtio_descriptor_token bad_descriptor;
    struct virtio_rng_completion_result result;
    uint8_t payload[16];

    memset(payload, 0x36, sizeof(payload));
    setup_fixture(&fixture, 8U);

    fixture.composition.published = false;
    CHECK(virtio_rng_completion_complete(
              &fixture.api, &fixture.composition, &fixture.descriptor,
              payload, &result) ==
              VIRTIO_RNG_COMPLETION_INVALID_COMPOSITION,
          "completion rejects unpublished composition");

    setup_fixture(&fixture, 8U);
    fixture.composition.generation = 6U;
    CHECK(virtio_rng_completion_complete(
              &fixture.api, &fixture.composition, &fixture.descriptor,
              payload, &result) == VIRTIO_RNG_COMPLETION_INVALID_STATE,
          "completion rejects composition generation mismatch");

    setup_fixture(&fixture, 8U);
    bad_descriptor = fixture.descriptor;
    bad_descriptor.index = fixture.composition.queue.queue_size;
    CHECK(virtio_rng_completion_complete(
              &fixture.api, &fixture.composition, &bad_descriptor,
              payload, &result) ==
              VIRTIO_RNG_COMPLETION_INVALID_DESCRIPTOR,
          "completion rejects descriptor index outside queue");

    setup_fixture(&fixture, 8U);
    bad_descriptor = fixture.descriptor;
    bad_descriptor.generation += 1U;
    CHECK(virtio_rng_completion_complete(
              &fixture.api, &fixture.composition, &bad_descriptor,
              payload, &result) ==
              VIRTIO_RNG_COMPLETION_INVALID_DESCRIPTOR,
          "completion rejects stale descriptor generation");

    setup_fixture(&fixture, 8U);
    fixture.composition.queue.descriptors.states[
        fixture.descriptor.index] = VIRTIO_DESCRIPTOR_STATE_FREE;
    ++fixture.composition.queue.descriptors.free_count;
    --fixture.composition.queue.descriptors.published_count;
    fixture.composition.queue.descriptors.freelist[
        fixture.composition.queue.descriptors.freelist_top] =
        fixture.descriptor.index;
    ++fixture.composition.queue.descriptors.freelist_top;
    CHECK(virtio_rng_completion_complete(
              &fixture.api, &fixture.composition, &fixture.descriptor,
              payload, &result) ==
              VIRTIO_RNG_COMPLETION_INVALID_DESCRIPTOR,
          "completion rejects descriptor that is no longer published");
}

static void test_success(void)
{
    struct completion_fixture fixture;
    struct virtio_rng_completion_result result;
    uint8_t payload[16];
    uint32_t index;

    setup_fixture(&fixture, 8U);
    memset(payload, 0x37, sizeof(payload));
    set_used_entry(&fixture, fixture.descriptor.index, 8U);
    memset(&result, 0, sizeof(result));

    CHECK(virtio_rng_completion_complete(
              &fixture.api,
              &fixture.composition,
              &fixture.descriptor,
              payload,
              &result) == VIRTIO_RNG_COMPLETION_OK &&
          result.bytes_written == 8U &&
          result.request_result == VIRTIO_RNG_REQUEST_RESULT_READY &&
          virtio_rng_request_api_current(&fixture.api)->result ==
              VIRTIO_RNG_REQUEST_RESULT_READY,
          "valid completion publishes READY and exact byte count");

    for (index = 0U; index < 8U; ++index) {
        if (fixture.destination[index] != 0x37U) {
            ++failures;
            fprintf(stderr, "FAIL: valid completion copied all bytes\n");
            return;
        }
    }
    for (index = 8U; index < sizeof(fixture.destination); ++index) {
        if (fixture.destination[index] != 0x5AU) {
            ++failures;
            fprintf(stderr, "FAIL: valid completion preserved trailing bytes\n");
            return;
        }
    }
    printf("PASS: valid completion copied exact device bytes\n");

    CHECK(virtio_used_consumption_consumed_count(
              &fixture.composition.queue.used) == 1U &&
          virtio_descriptor_ownership_free_count(
              &fixture.composition.queue.descriptors) == 4U &&
          virtio_descriptor_ownership_published_count(
              &fixture.composition.queue.descriptors) == 0U,
          "valid completion consumes used entry and reclaims descriptor once");
    CHECK(virtio_rng_completion_complete(
              &fixture.api,
              &fixture.composition,
              &fixture.descriptor,
              payload,
              &result) == VIRTIO_RNG_COMPLETION_NOT_PENDING,
          "post-success duplicate is rejected without further mutation");
}

static void test_duplicate_pending(void)
{
    struct completion_fixture fixture;
    struct virtio_rng_completion_result result;
    uint8_t payload[16];
    uint32_t index;

    setup_fixture(&fixture, 8U);
    memset(payload, 0x38, sizeof(payload));
    memset(&result, 0, sizeof(result));

    CHECK(virtio_rng_completion_complete(
              &fixture.api,
              &fixture.composition,
              &fixture.descriptor,
              payload,
              &result) == VIRTIO_RNG_COMPLETION_DUPLICATE_COMPLETION &&
          result.bytes_written == 0U &&
          result.request_result == VIRTIO_RNG_REQUEST_RESULT_FAILED &&
          virtio_rng_request_api_current(&fixture.api)->result ==
              VIRTIO_RNG_REQUEST_RESULT_FAILED,
          "duplicate pending completion fails request");
    for (index = 0U; index < 8U; ++index) {
        if (fixture.destination[index] != 0U) {
            ++failures;
            fprintf(stderr, "FAIL: duplicate completion zeroized destination\n");
            return;
        }
    }
    printf("PASS: duplicate completion zeroized destination\n");
    CHECK(virtio_used_consumption_consumed_count(
              &fixture.composition.queue.used) == 0U &&
          virtio_descriptor_ownership_published_count(
              &fixture.composition.queue.descriptors) == 1U,
          "duplicate completion does not advance used or reclaim descriptor");
}

static void test_stale(void)
{
    struct completion_fixture fixture;
    struct virtio_rng_completion_result result;
    uint8_t payload[16];
    uint32_t index;

    setup_fixture(&fixture, 8U);
    memset(payload, 0x39, sizeof(payload));
    set_used_entry(
        &fixture,
        (fixture.descriptor.index + 1U) %
            fixture.composition.queue.queue_size,
        8U);
    memset(&result, 0, sizeof(result));

    CHECK(virtio_rng_completion_complete(
              &fixture.api,
              &fixture.composition,
              &fixture.descriptor,
              payload,
              &result) == VIRTIO_RNG_COMPLETION_STALE_COMPLETION &&
          result.request_result == VIRTIO_RNG_REQUEST_RESULT_FAILED &&
          virtio_rng_request_api_current(&fixture.api)->result ==
              VIRTIO_RNG_REQUEST_RESULT_FAILED,
          "stale completion fails request");
    for (index = 0U; index < 8U; ++index) {
        if (fixture.destination[index] != 0U) {
            ++failures;
            fprintf(stderr, "FAIL: stale completion zeroized destination\n");
            return;
        }
    }
    printf("PASS: stale completion zeroized destination\n");
    CHECK(virtio_used_consumption_consumed_count(
              &fixture.composition.queue.used) == 0U &&
          virtio_used_consumption_pending_head(
              &fixture.composition.queue.used) == 0U &&
          virtio_descriptor_ownership_published_count(
              &fixture.composition.queue.descriptors) == 1U,
          "stale completion cancels used without reclaiming descriptor");
}

static void test_used_invalid(void)
{
    struct completion_fixture fixture;
    struct virtio_rng_completion_result result;
    uint8_t payload[16];
    uint32_t index;

    setup_fixture(&fixture, 8U);
    memset(payload, 0x40, sizeof(payload));
    set_used_entry(&fixture, fixture.composition.queue.queue_size, 8U);
    memset(&result, 0, sizeof(result));

    CHECK(virtio_rng_completion_complete(
              &fixture.api,
              &fixture.composition,
              &fixture.descriptor,
              payload,
              &result) == VIRTIO_RNG_COMPLETION_USED_INVALID &&
          result.request_result == VIRTIO_RNG_REQUEST_RESULT_FAILED &&
          virtio_rng_request_api_current(&fixture.api)->result ==
              VIRTIO_RNG_REQUEST_RESULT_FAILED,
          "invalid used entry fails request");
    for (index = 0U; index < 8U; ++index) {
        if (fixture.destination[index] != 0U) {
            ++failures;
            fprintf(stderr, "FAIL: invalid used entry zeroized destination\n");
            return;
        }
    }
    printf("PASS: invalid used entry zeroized destination\n");
    CHECK(virtio_used_consumption_consumed_count(
              &fixture.composition.queue.used) == 0U &&
          virtio_descriptor_ownership_published_count(
              &fixture.composition.queue.descriptors) == 1U,
          "invalid used entry does not advance used or reclaim descriptor");
}

static void test_short_and_oversized(void)
{
    struct completion_fixture short_fixture;
    struct completion_fixture oversized_fixture;
    struct virtio_rng_completion_result result;
    uint8_t payload[16];
    uint32_t index;

    setup_fixture(&short_fixture, 8U);
    memset(payload, 0x41, sizeof(payload));
    set_used_entry(&short_fixture, short_fixture.descriptor.index, 7U);
    memset(&result, 0, sizeof(result));
    CHECK(virtio_rng_completion_complete(
              &short_fixture.api,
              &short_fixture.composition,
              &short_fixture.descriptor,
              payload,
              &result) == VIRTIO_RNG_COMPLETION_SHORT_COMPLETION &&
          result.request_result == VIRTIO_RNG_REQUEST_RESULT_FAILED &&
          virtio_rng_request_api_current(&short_fixture.api)->result ==
              VIRTIO_RNG_REQUEST_RESULT_FAILED,
          "short completion fails request");
    for (index = 0U; index < 8U; ++index) {
        if (short_fixture.destination[index] != 0U) {
            ++failures;
            fprintf(stderr, "FAIL: short completion zeroized destination\n");
            return;
        }
    }
    printf("PASS: short completion zeroized destination\n");
    CHECK(virtio_used_consumption_consumed_count(
              &short_fixture.composition.queue.used) == 1U &&
          virtio_descriptor_ownership_free_count(
              &short_fixture.composition.queue.descriptors) == 4U,
          "short completion consumes used and reclaims descriptor once");

    setup_fixture(&oversized_fixture, 8U);
    memset(payload, 0x42, sizeof(payload));
    set_used_entry(&oversized_fixture, oversized_fixture.descriptor.index, 9U);
    memset(&result, 0, sizeof(result));
    CHECK(virtio_rng_completion_complete(
              &oversized_fixture.api,
              &oversized_fixture.composition,
              &oversized_fixture.descriptor,
              payload,
              &result) == VIRTIO_RNG_COMPLETION_OVERSIZED_COMPLETION &&
          result.request_result == VIRTIO_RNG_REQUEST_RESULT_FAILED &&
          virtio_rng_request_api_current(&oversized_fixture.api)->result ==
              VIRTIO_RNG_REQUEST_RESULT_FAILED,
          "oversized completion fails request");
    for (index = 0U; index < 8U; ++index) {
        if (oversized_fixture.destination[index] != 0U) {
            ++failures;
            fprintf(stderr, "FAIL: oversized completion zeroized destination\n");
            return;
        }
    }
    printf("PASS: oversized completion zeroized destination\n");
    CHECK(virtio_used_consumption_consumed_count(
              &oversized_fixture.composition.queue.used) == 1U &&
          virtio_descriptor_ownership_free_count(
              &oversized_fixture.composition.queue.descriptors) == 4U,
          "oversized completion consumes used and reclaims descriptor once");
}

static void test_no_allocation(void)
{
    struct completion_fixture fixture;
    struct virtio_rng_completion_result result;
    uint8_t payload[16];

    setup_fixture(&fixture, 8U);
    memset(payload, 0x43, sizeof(payload));
    set_used_entry(&fixture, fixture.descriptor.index, 8U);
    memset(&result, 0, sizeof(result));
    alloc_attempts = 0U;
    free_attempts = 0U;
    CHECK(virtio_rng_completion_complete(
              &fixture.api,
              &fixture.composition,
              &fixture.descriptor,
              payload,
              &result) == VIRTIO_RNG_COMPLETION_OK,
          "no-allocation fixture completes valid request");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "completion path performs no allocation or free");
}

int main(void)
{
    test_status_strings();
    test_null_and_state();
    test_invalid_composition_and_descriptor();
    test_success();
    test_duplicate_pending();
    test_stale();
    test_used_invalid();
    test_short_and_oversized();
    test_no_allocation();

    if (failures == 0) {
        printf("Virtio rng completion host tests: ALL OK\n");
        return 0;
    }
    fprintf(stderr, "Virtio rng completion host tests: %d FAILURES\n",
            failures);
    return 1;
}

#endif
