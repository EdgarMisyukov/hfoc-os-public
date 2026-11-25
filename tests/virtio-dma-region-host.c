#include "virtio_dma_region.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_VIRTIO_DMA_REGION_HOST_TEST

#define TEST_CAPACITY 4U
#define TEST_WINDOW_BASE UINT64_C(0x100000)
#define TEST_WINDOW_LENGTH UINT64_C(0x100000)
#define TEST_ALIGNMENT 16U

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

struct zeroize_state {
    unsigned int calls;
    bool allow;
    uint64_t last_base;
    uint64_t last_length;
};

static bool test_zeroize(
    void *context,
    uint64_t physical_base,
    uint64_t length)
{
    struct zeroize_state *state = (struct zeroize_state *)context;

    ++state->calls;
    state->last_base = physical_base;
    state->last_length = length;
    return state->allow;
}

static bool make_window(
    uint64_t base,
    uint64_t length,
    uint32_t alignment,
    struct virtio_dma_window_policy *window)
{
    window->base = base;
    window->length = length;
    window->alignment = alignment;
    return true;
}

static void reset_alloc_counts(void)
{
    alloc_attempts = 0U;
    free_attempts = 0U;
}

static bool init_registry(
    struct virtio_dma_region_registry *registry,
    struct zeroize_state *state,
    struct virtio_dma_zeroize *zeroize,
    uint32_t capacity)
{
    struct virtio_dma_window_policy window;

    memset(registry, 0, sizeof(*registry));
    memset(state, 0, sizeof(*state));
    state->allow = true;
    zeroize->zeroize = test_zeroize;
    zeroize->context = state;
    make_window(TEST_WINDOW_BASE, TEST_WINDOW_LENGTH, TEST_ALIGNMENT,
                &window);
    reset_alloc_counts();
    return virtio_dma_region_init(
               registry, &window, zeroize, capacity) ==
           VIRTIO_DMA_REGION_OK;
}

static bool token_valid(
    const struct virtio_dma_region_registry *registry,
    const struct virtio_dma_region_token *token)
{
    return token != NULL &&
           token->index < registry->capacity &&
           token->generation != 0U;
}

static bool region_matches_request(
    const struct virtio_dma_region *region,
    const struct virtio_dma_region_request *request,
    uint32_t index,
    uint32_t generation)
{
    return region != NULL &&
           region->published &&
           region->version == VIRTIO_DMA_REGION_VERSION &&
           region->index == index &&
           region->generation == generation &&
           region->physical_base == request->physical_base &&
           region->physical_end ==
               request->physical_base + request->length &&
           region->length == request->length &&
           region->direction == request->direction &&
           region->lifetime == request->lifetime &&
           region->zeroization == request->zeroization;
}

static bool status_string_matches(
    enum virtio_dma_region_status status,
    const char *string)
{
    switch (status) {
    case VIRTIO_DMA_REGION_OK:
        return strcmp(string, "VIRTIO_DMA_REGION_OK") == 0;
    case VIRTIO_DMA_REGION_NULL:
        return strcmp(string, "VIRTIO_DMA_REGION_NULL") == 0;
    case VIRTIO_DMA_REGION_BAD_STATE:
        return strcmp(string, "VIRTIO_DMA_REGION_BAD_STATE") == 0;
    case VIRTIO_DMA_REGION_BAD_CAPACITY:
        return strcmp(string, "VIRTIO_DMA_REGION_BAD_CAPACITY") == 0;
    case VIRTIO_DMA_REGION_ALREADY_PUBLISHED:
        return strcmp(
            string, "VIRTIO_DMA_REGION_ALREADY_PUBLISHED") == 0;
    case VIRTIO_DMA_REGION_UNPUBLISHED:
        return strcmp(string, "VIRTIO_DMA_REGION_UNPUBLISHED") == 0;
    case VIRTIO_DMA_REGION_INVALID_VERSION:
        return strcmp(
            string, "VIRTIO_DMA_REGION_INVALID_VERSION") == 0;
    case VIRTIO_DMA_REGION_INVALID_WINDOW:
        return strcmp(
            string, "VIRTIO_DMA_REGION_INVALID_WINDOW") == 0;
    case VIRTIO_DMA_REGION_INVALID_ALIGNMENT:
        return strcmp(
            string, "VIRTIO_DMA_REGION_INVALID_ALIGNMENT") == 0;
    case VIRTIO_DMA_REGION_INVALID_LIFETIME:
        return strcmp(
            string, "VIRTIO_DMA_REGION_INVALID_LIFETIME") == 0;
    case VIRTIO_DMA_REGION_INVALID_DIRECTION:
        return strcmp(
            string, "VIRTIO_DMA_REGION_INVALID_DIRECTION") == 0;
    case VIRTIO_DMA_REGION_INVALID_ZEROIZATION:
        return strcmp(
            string, "VIRTIO_DMA_REGION_INVALID_ZEROIZATION") == 0;
    case VIRTIO_DMA_REGION_INVALID_RANGE:
        return strcmp(
            string, "VIRTIO_DMA_REGION_INVALID_RANGE") == 0;
    case VIRTIO_DMA_REGION_OVERFLOW:
        return strcmp(string, "VIRTIO_DMA_REGION_OVERFLOW") == 0;
    case VIRTIO_DMA_REGION_UNALIGNED:
        return strcmp(string, "VIRTIO_DMA_REGION_UNALIGNED") == 0;
    case VIRTIO_DMA_REGION_FORBIDDEN_RANGE:
        return strcmp(
            string, "VIRTIO_DMA_REGION_FORBIDDEN_RANGE") == 0;
    case VIRTIO_DMA_REGION_ALIAS:
        return strcmp(string, "VIRTIO_DMA_REGION_ALIAS") == 0;
    case VIRTIO_DMA_REGION_EXHAUSTED:
        return strcmp(string, "VIRTIO_DMA_REGION_EXHAUSTED") == 0;
    case VIRTIO_DMA_REGION_BAD_TOKEN:
        return strcmp(string, "VIRTIO_DMA_REGION_BAD_TOKEN") == 0;
    case VIRTIO_DMA_REGION_STALE_TOKEN:
        return strcmp(string, "VIRTIO_DMA_REGION_STALE_TOKEN") == 0;
    case VIRTIO_DMA_REGION_ZEROIZE_REJECTED:
        return strcmp(
            string, "VIRTIO_DMA_REGION_ZEROIZE_REJECTED") == 0;
    case VIRTIO_DMA_REGION_LIVE_OWNERS:
        return strcmp(string, "VIRTIO_DMA_REGION_LIVE_OWNERS") == 0;
    default:
        return false;
    }
}

static void test_status_strings(void)
{
    static const enum virtio_dma_region_status statuses[] = {
        VIRTIO_DMA_REGION_OK,
        VIRTIO_DMA_REGION_NULL,
        VIRTIO_DMA_REGION_BAD_STATE,
        VIRTIO_DMA_REGION_BAD_CAPACITY,
        VIRTIO_DMA_REGION_ALREADY_PUBLISHED,
        VIRTIO_DMA_REGION_UNPUBLISHED,
        VIRTIO_DMA_REGION_INVALID_VERSION,
        VIRTIO_DMA_REGION_INVALID_WINDOW,
        VIRTIO_DMA_REGION_INVALID_ALIGNMENT,
        VIRTIO_DMA_REGION_INVALID_LIFETIME,
        VIRTIO_DMA_REGION_INVALID_DIRECTION,
        VIRTIO_DMA_REGION_INVALID_ZEROIZATION,
        VIRTIO_DMA_REGION_INVALID_RANGE,
        VIRTIO_DMA_REGION_OVERFLOW,
        VIRTIO_DMA_REGION_UNALIGNED,
        VIRTIO_DMA_REGION_FORBIDDEN_RANGE,
        VIRTIO_DMA_REGION_ALIAS,
        VIRTIO_DMA_REGION_EXHAUSTED,
        VIRTIO_DMA_REGION_BAD_TOKEN,
        VIRTIO_DMA_REGION_STALE_TOKEN,
        VIRTIO_DMA_REGION_ZEROIZE_REJECTED,
        VIRTIO_DMA_REGION_LIVE_OWNERS,
    };
    size_t index;

    reset_alloc_counts();
    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        enum virtio_dma_region_status status = statuses[index];
        const char *string = virtio_dma_region_status_string(status);

        CHECK(string != NULL &&
                  status_string_matches(status, string),
              "DMA region status string is exact");
    }
    CHECK(virtio_dma_region_status_string(
              (enum virtio_dma_region_status)99U) == NULL,
          "DMA region undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "DMA region status handling performs no allocation");
}

static void test_init_and_null(void)
{
    struct virtio_dma_region_registry registry;
    struct virtio_dma_window_policy window;
    struct virtio_dma_zeroize zeroize;
    struct zeroize_state state;
    struct virtio_dma_region_request request;
    struct virtio_dma_region_token token;
    struct virtio_dma_region region_out;

    memset(&registry, 0, sizeof(registry));
    memset(&state, 0, sizeof(state));
    zeroize.zeroize = test_zeroize;
    zeroize.context = &state;
    make_window(TEST_WINDOW_BASE, TEST_WINDOW_LENGTH, TEST_ALIGNMENT,
                &window);
    reset_alloc_counts();

    CHECK(virtio_dma_region_init(
              NULL, &window, &zeroize, TEST_CAPACITY) ==
              VIRTIO_DMA_REGION_NULL,
          "DMA init rejects null registry");
    CHECK(virtio_dma_region_init(
              &registry, NULL, &zeroize, TEST_CAPACITY) ==
              VIRTIO_DMA_REGION_NULL,
          "DMA init rejects null window");
    CHECK(virtio_dma_region_init(
              &registry, &window, NULL, TEST_CAPACITY) ==
              VIRTIO_DMA_REGION_NULL,
          "DMA init rejects null zeroize seam");
    zeroize.zeroize = NULL;
    CHECK(virtio_dma_region_init(
              &registry, &window, &zeroize, TEST_CAPACITY) ==
              VIRTIO_DMA_REGION_NULL,
          "DMA init rejects null zeroize callback");
    zeroize.zeroize = test_zeroize;
    zeroize.context = NULL;
    CHECK(virtio_dma_region_init(
              &registry, &window, &zeroize, TEST_CAPACITY) ==
              VIRTIO_DMA_REGION_NULL,
          "DMA init rejects null zeroize context");
    zeroize.context = &state;
    CHECK(virtio_dma_region_init(
              &registry, &window, &zeroize,
              VIRTIO_DMA_REGION_CAPACITY_MIN - 1U) ==
              VIRTIO_DMA_REGION_BAD_CAPACITY,
          "DMA init rejects zero capacity");
    CHECK(virtio_dma_region_init(
              &registry, &window, &zeroize,
              VIRTIO_DMA_REGION_CAPACITY_MAX + 1U) ==
              VIRTIO_DMA_REGION_BAD_CAPACITY,
          "DMA init rejects over-limit capacity");

    make_window(TEST_WINDOW_BASE, 0U, TEST_ALIGNMENT, &window);
    CHECK(virtio_dma_region_init(
              &registry, &window, &zeroize, TEST_CAPACITY) ==
              VIRTIO_DMA_REGION_INVALID_WINDOW,
          "DMA init rejects zero-length window");
    make_window(UINT64_MAX - 3U, 16U, TEST_ALIGNMENT, &window);
    CHECK(virtio_dma_region_init(
              &registry, &window, &zeroize, TEST_CAPACITY) ==
              VIRTIO_DMA_REGION_OVERFLOW,
          "DMA init rejects overflowing window");
    make_window(TEST_WINDOW_BASE, TEST_WINDOW_LENGTH, 0U, &window);
    CHECK(virtio_dma_region_init(
              &registry, &window, &zeroize, TEST_CAPACITY) ==
              VIRTIO_DMA_REGION_INVALID_ALIGNMENT,
          "DMA init rejects zero alignment");
    make_window(TEST_WINDOW_BASE, TEST_WINDOW_LENGTH,
                VIRTIO_DMA_REGION_WINDOW_ALIGNMENT_MAX + 1U, &window);
    CHECK(virtio_dma_region_init(
              &registry, &window, &zeroize, TEST_CAPACITY) ==
              VIRTIO_DMA_REGION_INVALID_ALIGNMENT,
          "DMA init rejects over-limit alignment");
    make_window(TEST_WINDOW_BASE, TEST_WINDOW_LENGTH, 3U, &window);
    CHECK(virtio_dma_region_init(
              &registry, &window, &zeroize, TEST_CAPACITY) ==
              VIRTIO_DMA_REGION_INVALID_ALIGNMENT,
          "DMA init rejects non-power-of-two alignment");
    make_window(TEST_WINDOW_BASE + 16U, TEST_WINDOW_LENGTH, 64U, &window);
    CHECK(virtio_dma_region_init(
              &registry, &window, &zeroize, TEST_CAPACITY) ==
              VIRTIO_DMA_REGION_UNALIGNED,
          "DMA init rejects unaligned window base");

    make_window(TEST_WINDOW_BASE, TEST_WINDOW_LENGTH, TEST_ALIGNMENT,
                &window);
    CHECK(virtio_dma_region_init(
              &registry, &window, &zeroize, TEST_CAPACITY) ==
              VIRTIO_DMA_REGION_OK,
          "DMA init publishes bounded registry");
    CHECK(virtio_dma_region_published(&registry) &&
              virtio_dma_region_free_count(&registry) == TEST_CAPACITY &&
              virtio_dma_region_bound_count(&registry) == 0U,
          "DMA init reports exact initial counts");
    CHECK(virtio_dma_region_init(
              &registry, &window, &zeroize, TEST_CAPACITY) ==
              VIRTIO_DMA_REGION_ALREADY_PUBLISHED,
          "DMA init rejects duplicate publication");

    memset(&request, 0, sizeof(request));
    memset(&token, 0, sizeof(token));
    request.physical_base = TEST_WINDOW_BASE;
    request.length = 64U;
    request.direction = VIRTIO_DMA_DIRECTION_DEVICE_READ;
    request.lifetime = VIRTIO_DMA_LIFETIME_QUEUE;
    request.zeroization = VIRTIO_DMA_ZEROIZATION_NONE;
    CHECK(virtio_dma_region_bind(
              NULL, &request, &token, &region_out) ==
              VIRTIO_DMA_REGION_NULL,
          "DMA bind rejects null registry");
    CHECK(virtio_dma_region_bind(
              &registry, NULL, &token, &region_out) ==
              VIRTIO_DMA_REGION_NULL,
          "DMA bind rejects null request");
    CHECK(virtio_dma_region_bind(
              &registry, &request, NULL, &region_out) ==
              VIRTIO_DMA_REGION_NULL,
          "DMA bind rejects null token");
    CHECK(virtio_dma_region_bind(
              &registry, &request, &token, NULL) ==
              VIRTIO_DMA_REGION_NULL,
          "DMA bind rejects null region output");
    CHECK(virtio_dma_region_unbind(
              &registry, NULL) == VIRTIO_DMA_REGION_NULL,
          "DMA unbind rejects null token");
    CHECK(virtio_dma_region_unbind(
              NULL, &token) == VIRTIO_DMA_REGION_NULL,
          "DMA unbind rejects null registry");
    CHECK(virtio_dma_region_teardown(NULL) == VIRTIO_DMA_REGION_NULL,
          "DMA teardown rejects null registry");

    memset(&registry, 0, sizeof(registry));
    CHECK(!virtio_dma_region_published(&registry) &&
              virtio_dma_region_free_count(&registry) == 0U &&
              virtio_dma_region_bound_count(&registry) == 0U,
          "unpublished DMA registry accessors return inactive state");
    CHECK(virtio_dma_region_bind(
              &registry, &request, &token, &region_out) ==
              VIRTIO_DMA_REGION_UNPUBLISHED,
          "unpublished DMA registry rejects bind");
    CHECK(virtio_dma_region_unbind(
              &registry, &token) == VIRTIO_DMA_REGION_UNPUBLISHED,
          "unpublished DMA registry rejects unbind");
    CHECK(virtio_dma_region_teardown(
              &registry) == VIRTIO_DMA_REGION_UNPUBLISHED,
          "unpublished DMA registry rejects teardown");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "DMA init and null checks perform no allocation");
}

static void test_successful_lifecycle(void)
{
    struct virtio_dma_region_registry registry;
    struct virtio_dma_zeroize zeroize;
    struct zeroize_state state;
    struct virtio_dma_region_request request;
    struct virtio_dma_region_token token[TEST_CAPACITY];
    struct virtio_dma_region output[TEST_CAPACITY];
    struct virtio_dma_region_token old_token;
    uint32_t index;

    CHECK(init_registry(&registry, &state, &zeroize, TEST_CAPACITY),
          "lifecycle fixture initializes DMA registry");

    request.physical_base = TEST_WINDOW_BASE;
    request.length = 64U;
    request.direction = VIRTIO_DMA_DIRECTION_DEVICE_READ;
    request.lifetime = VIRTIO_DMA_LIFETIME_QUEUE;
    request.zeroization = VIRTIO_DMA_ZEROIZATION_NONE;
    CHECK(virtio_dma_region_bind(
              &registry, &request, &token[0], &output[0]) ==
              VIRTIO_DMA_REGION_OK &&
              token_valid(&registry, &token[0]) &&
              region_matches_request(
                  &output[0], &request, token[0].index,
                  token[0].generation),
          "DMA bind publishes queue device-read region");

    request.physical_base = TEST_WINDOW_BASE + 4096U;
    request.length = 256U;
    request.direction = VIRTIO_DMA_DIRECTION_DEVICE_WRITE;
    request.lifetime = VIRTIO_DMA_LIFETIME_PAYLOAD;
    request.zeroization = VIRTIO_DMA_ZEROIZATION_ON_RELEASE;
    CHECK(virtio_dma_region_bind(
              &registry, &request, &token[1], &output[1]) ==
              VIRTIO_DMA_REGION_OK &&
              token_valid(&registry, &token[1]) &&
              token[1].index != token[0].index &&
              region_matches_request(
                  &output[1], &request, token[1].index,
                  token[1].generation),
          "DMA bind publishes payload device-write region");

    request.physical_base = TEST_WINDOW_BASE + 8192U;
    request.length = 128U;
    request.direction = VIRTIO_DMA_DIRECTION_BIDIRECTIONAL;
    request.lifetime = VIRTIO_DMA_LIFETIME_QUEUE;
    request.zeroization = VIRTIO_DMA_ZEROIZATION_NONE;
    CHECK(virtio_dma_region_bind(
              &registry, &request, &token[2], &output[2]) ==
              VIRTIO_DMA_REGION_OK &&
              region_matches_request(
                  &output[2], &request, token[2].index,
                  token[2].generation),
          "DMA bind publishes bidirectional queue region");

    request.physical_base = TEST_WINDOW_BASE + 16384U;
    request.length = 32U;
    request.direction = VIRTIO_DMA_DIRECTION_DEVICE_READ;
    request.lifetime = VIRTIO_DMA_LIFETIME_PAYLOAD;
    request.zeroization = VIRTIO_DMA_ZEROIZATION_ON_RELEASE;
    CHECK(virtio_dma_region_bind(
              &registry, &request, &token[3], &output[3]) ==
              VIRTIO_DMA_REGION_OK &&
              virtio_dma_region_free_count(&registry) == 0U &&
              virtio_dma_region_bound_count(&registry) == TEST_CAPACITY,
          "DMA bind fills exact capacity");
    CHECK(virtio_dma_region_bind(
              &registry, &request, &token[3], &output[3]) ==
              VIRTIO_DMA_REGION_EXHAUSTED &&
              virtio_dma_region_free_count(&registry) == 0U &&
              virtio_dma_region_bound_count(&registry) == TEST_CAPACITY,
          "DMA bind rejects exhaustion without mutation");

    old_token = token[1];
    CHECK(virtio_dma_region_unbind(
              &registry, &token[1]) == VIRTIO_DMA_REGION_OK &&
              state.calls == 1U &&
              state.last_base == TEST_WINDOW_BASE + 4096U &&
              state.last_length == 256U &&
              virtio_dma_region_free_count(&registry) == 1U &&
              virtio_dma_region_bound_count(&registry) == 3U,
          "DMA unbind zeroizes and releases on-release region once");
    CHECK(virtio_dma_region_unbind(
              &registry, &token[1]) == VIRTIO_DMA_REGION_BAD_TOKEN &&
              state.calls == 1U,
          "DMA unbind rejects released token");

    request.physical_base = TEST_WINDOW_BASE + 4096U;
    request.length = 256U;
    request.direction = VIRTIO_DMA_DIRECTION_DEVICE_WRITE;
    request.lifetime = VIRTIO_DMA_LIFETIME_PAYLOAD;
    request.zeroization = VIRTIO_DMA_ZEROIZATION_ON_RELEASE;
    CHECK(virtio_dma_region_bind(
              &registry, &request, &token[1], &output[1]) ==
              VIRTIO_DMA_REGION_OK &&
              token[1].index == old_token.index &&
              token[1].generation == old_token.generation + 1U,
          "DMA rebind reuses slot with advanced generation");
    CHECK(virtio_dma_region_unbind(
              &registry, &old_token) ==
              VIRTIO_DMA_REGION_STALE_TOKEN,
          "DMA stale generation token is rejected");
    CHECK(virtio_dma_region_unbind(
              &registry, &token[0]) == VIRTIO_DMA_REGION_OK &&
              state.calls == 1U &&
              virtio_dma_region_free_count(&registry) == 1U &&
              virtio_dma_region_bound_count(&registry) == 3U,
          "DMA none-policy unbind performs no zeroization");

    for (index = 0U; index < TEST_CAPACITY; ++index) {
        if (registry.slots[token[index].index].bound) {
            CHECK(virtio_dma_region_unbind(
                      &registry, &token[index]) ==
                      VIRTIO_DMA_REGION_OK,
                  "DMA lifecycle unbinds remaining region");
        }
    }
    CHECK(virtio_dma_region_free_count(&registry) == TEST_CAPACITY &&
              virtio_dma_region_bound_count(&registry) == 0U,
          "DMA lifecycle returns all slots to free");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "DMA successful lifecycle performs no allocation");
}

static void test_bind_failures(void)
{
    struct virtio_dma_region_registry registry;
    struct virtio_dma_zeroize zeroize;
    struct zeroize_state state;
    struct virtio_dma_region_request request;
    struct virtio_dma_region_token token;
    struct virtio_dma_region output;
    uint32_t free_before;
    uint32_t bound_before;

    CHECK(init_registry(&registry, &state, &zeroize, TEST_CAPACITY),
          "failure fixture initializes DMA registry");

    request.physical_base = TEST_WINDOW_BASE;
    request.length = 0U;
    request.direction = VIRTIO_DMA_DIRECTION_DEVICE_READ;
    request.lifetime = VIRTIO_DMA_LIFETIME_QUEUE;
    request.zeroization = VIRTIO_DMA_ZEROIZATION_NONE;
    CHECK(virtio_dma_region_bind(
              &registry, &request, &token, &output) ==
              VIRTIO_DMA_REGION_INVALID_RANGE,
          "DMA bind rejects zero-length region");

    request.length = VIRTIO_DMA_REGION_LENGTH_MAX + 1U;
    CHECK(virtio_dma_region_bind(
              &registry, &request, &token, &output) ==
              VIRTIO_DMA_REGION_INVALID_RANGE,
          "DMA bind rejects over-limit region");

    request.length = 64U;
    request.physical_base = TEST_WINDOW_BASE + 8U;
    CHECK(virtio_dma_region_bind(
              &registry, &request, &token, &output) ==
              VIRTIO_DMA_REGION_UNALIGNED,
          "DMA bind rejects unaligned base");

    request.physical_base = TEST_WINDOW_BASE - 16U;
    CHECK(virtio_dma_region_bind(
              &registry, &request, &token, &output) ==
              VIRTIO_DMA_REGION_FORBIDDEN_RANGE,
          "DMA bind rejects range below window");

    request.physical_base = TEST_WINDOW_BASE + TEST_WINDOW_LENGTH;
    CHECK(virtio_dma_region_bind(
              &registry, &request, &token, &output) ==
              VIRTIO_DMA_REGION_FORBIDDEN_RANGE,
          "DMA bind rejects range above window");

    request.physical_base = TEST_WINDOW_BASE + TEST_WINDOW_LENGTH - 32U;
    request.length = 64U;
    CHECK(virtio_dma_region_bind(
              &registry, &request, &token, &output) ==
              VIRTIO_DMA_REGION_FORBIDDEN_RANGE,
          "DMA bind rejects range crossing window end");

    request.physical_base = UINT64_MAX - 63U;
    request.length = 64U;
    CHECK(virtio_dma_region_bind(
              &registry, &request, &token, &output) ==
              VIRTIO_DMA_REGION_OVERFLOW,
          "DMA bind rejects overflowing end");

    request.physical_base = TEST_WINDOW_BASE;
    request.length = 64U;
    request.direction = (enum virtio_dma_direction)99U;
    request.lifetime = VIRTIO_DMA_LIFETIME_QUEUE;
    request.zeroization = VIRTIO_DMA_ZEROIZATION_NONE;
    CHECK(virtio_dma_region_bind(
              &registry, &request, &token, &output) ==
              VIRTIO_DMA_REGION_INVALID_DIRECTION,
          "DMA bind rejects invalid direction");

    request.direction = VIRTIO_DMA_DIRECTION_DEVICE_READ;
    request.lifetime = (enum virtio_dma_lifetime)99U;
    CHECK(virtio_dma_region_bind(
              &registry, &request, &token, &output) ==
              VIRTIO_DMA_REGION_INVALID_LIFETIME,
          "DMA bind rejects invalid lifetime");

    request.lifetime = VIRTIO_DMA_LIFETIME_QUEUE;
    request.zeroization = (enum virtio_dma_zeroization)99U;
    CHECK(virtio_dma_region_bind(
              &registry, &request, &token, &output) ==
              VIRTIO_DMA_REGION_INVALID_ZEROIZATION,
          "DMA bind rejects invalid zeroization policy");

    request.zeroization = VIRTIO_DMA_ZEROIZATION_NONE;
    free_before = virtio_dma_region_free_count(&registry);
    bound_before = virtio_dma_region_bound_count(&registry);
    request.physical_base = TEST_WINDOW_BASE + 4096U;
    request.length = 256U;
    CHECK(virtio_dma_region_bind(
              &registry, &request, &token, &output) ==
              VIRTIO_DMA_REGION_OK,
          "DMA bind succeeds after invalid request");

    request.physical_base = TEST_WINDOW_BASE + 4096U;
    request.length = 256U;
    CHECK(virtio_dma_region_bind(
              &registry, &request, &token, &output) ==
              VIRTIO_DMA_REGION_ALIAS,
          "DMA bind rejects exact alias");

    request.physical_base = TEST_WINDOW_BASE + 4160U;
    request.length = 256U;
    CHECK(virtio_dma_region_bind(
              &registry, &request, &token, &output) ==
              VIRTIO_DMA_REGION_ALIAS,
          "DMA bind rejects partial alias");

    request.physical_base = TEST_WINDOW_BASE + 4128U;
    request.length = 128U;
    CHECK(virtio_dma_region_bind(
              &registry, &request, &token, &output) ==
              VIRTIO_DMA_REGION_ALIAS,
          "DMA bind rejects inner alias");

    request.physical_base = TEST_WINDOW_BASE + 4032U;
    request.length = 256U;
    CHECK(virtio_dma_region_bind(
              &registry, &request, &token, &output) ==
              VIRTIO_DMA_REGION_ALIAS,
          "DMA bind rejects outer alias");

    CHECK(virtio_dma_region_free_count(&registry) ==
              free_before - 1U &&
              virtio_dma_region_bound_count(&registry) ==
                  bound_before + 1U,
          "DMA bind failure matrix preserves exact counts");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "DMA bind failures perform no allocation");
}

static void test_unbind_and_teardown(void)
{
    struct virtio_dma_region_registry registry;
    struct virtio_dma_zeroize zeroize;
    struct zeroize_state state;
    struct virtio_dma_region_request request;
    struct virtio_dma_region_token token;
    struct virtio_dma_region output;
    struct virtio_dma_region_token stale;

    CHECK(init_registry(&registry, &state, &zeroize, TEST_CAPACITY),
          "teardown fixture initializes DMA registry");
    request.physical_base = TEST_WINDOW_BASE;
    request.length = 64U;
    request.direction = VIRTIO_DMA_DIRECTION_DEVICE_WRITE;
    request.lifetime = VIRTIO_DMA_LIFETIME_PAYLOAD;
    request.zeroization = VIRTIO_DMA_ZEROIZATION_ON_RELEASE;
    CHECK(virtio_dma_region_bind(
              &registry, &request, &token, &output) ==
              VIRTIO_DMA_REGION_OK,
          "teardown fixture binds live region");
    CHECK(virtio_dma_region_teardown(
              &registry) == VIRTIO_DMA_REGION_LIVE_OWNERS &&
              virtio_dma_region_published(&registry) &&
              virtio_dma_region_bound_count(&registry) == 1U,
          "DMA teardown rejects live owner");

    stale = token;
    stale.generation = token.generation + 1U;
    CHECK(virtio_dma_region_unbind(
              &registry, &stale) == VIRTIO_DMA_REGION_STALE_TOKEN &&
              virtio_dma_region_bound_count(&registry) == 1U,
          "DMA unbind rejects stale generation");
    stale.index = registry.capacity;
    stale.generation = token.generation;
    CHECK(virtio_dma_region_unbind(
              &registry, &stale) == VIRTIO_DMA_REGION_BAD_TOKEN &&
              virtio_dma_region_bound_count(&registry) == 1U,
          "DMA unbind rejects out-of-range token");

    state.allow = false;
    CHECK(virtio_dma_region_unbind(
              &registry, &token) ==
              VIRTIO_DMA_REGION_ZEROIZE_REJECTED &&
              state.calls == 1U &&
              virtio_dma_region_bound_count(&registry) == 1U &&
              virtio_dma_region_free_count(&registry) ==
                  TEST_CAPACITY - 1U,
          "DMA unbind keeps region bound after rejected zeroization");
    state.allow = true;
    CHECK(virtio_dma_region_unbind(
              &registry, &token) == VIRTIO_DMA_REGION_OK &&
              state.calls == 2U &&
              virtio_dma_region_bound_count(&registry) == 0U,
          "DMA unbind retry succeeds after zeroization accepted");

    CHECK(virtio_dma_region_teardown(
              &registry) == VIRTIO_DMA_REGION_OK &&
              !virtio_dma_region_published(&registry) &&
              virtio_dma_region_free_count(&registry) == 0U &&
              virtio_dma_region_bound_count(&registry) == 0U,
          "DMA teardown unpublishes empty registry");

    CHECK(init_registry(&registry, &state, &zeroize, TEST_CAPACITY),
          "DMA registry reinitializes after teardown");
    CHECK(virtio_dma_region_bind(
              &registry, &request, &token, &output) ==
              VIRTIO_DMA_REGION_OK &&
              token.generation == 1U,
          "DMA reinitialized registry starts at fresh generation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "DMA unbind and teardown perform no allocation");
}

int main(void)
{
    test_status_strings();
    test_init_and_null();
    test_successful_lifecycle();
    test_bind_failures();
    test_unbind_and_teardown();

    if (failures != 0) {
        fprintf(stderr,
                "Virtio DMA region host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Virtio DMA region host tests: ALL OK\n");
    return 0;
}

#endif
