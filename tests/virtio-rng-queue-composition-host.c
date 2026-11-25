#include "virtio_rng_queue_composition.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_VIRTIO_RNG_QUEUE_COMPOSITION_HOST_TEST

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

static struct virtio_available_ring available_ring;
static struct virtio_used_ring used_ring;

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

static struct virtio_rng_queue_config make_config(
    uint32_t queue_size,
    uint64_t region_base,
    struct test_barrier_state *barrier,
    struct test_reclaim_state *reclaim,
    struct test_zeroize_state *zeroize)
{
    struct virtio_rng_queue_config config;

    memset(&config, 0, sizeof(config));
    config.queue_index = VIRTIO_RNG_QUEUE_COMPOSITION_QUEUE_INDEX;
    config.queue_size = queue_size;
    config.event_index = false;
    config.region_base = region_base;
    config.region_length = UINT64_C(0x1000);
    config.available_ring = &available_ring;
    config.used_ring = &used_ring;
    config.barrier.fence = test_fence;
    config.barrier.context = barrier;
    config.reclaim.reclaim = test_reclaim;
    config.reclaim.context = reclaim;
    config.max_used_length = 1024U;
    config.dma_window.base = region_base;
    config.dma_window.length = UINT64_C(0x1000);
    config.dma_window.alignment = 16U;
    config.dma_zeroize.zeroize = test_zeroize;
    config.dma_zeroize.context = zeroize;
    config.dma_capacity = 4U;
    return config;
}

static bool status_string_matches(
    enum virtio_rng_queue_composition_status status,
    const char *string)
{
    switch (status) {
    case VIRTIO_RNG_QUEUE_COMPOSITION_OK:
        return strcmp(string, "VIRTIO_RNG_QUEUE_COMPOSITION_OK") == 0;
    case VIRTIO_RNG_QUEUE_COMPOSITION_NULL:
        return strcmp(string, "VIRTIO_RNG_QUEUE_COMPOSITION_NULL") == 0;
    case VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_VERSION:
        return strcmp(
            string,
            "VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_VERSION") == 0;
    case VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_GENERATION:
        return strcmp(
            string,
            "VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_GENERATION") == 0;
    case VIRTIO_RNG_QUEUE_COMPOSITION_ALREADY_PUBLISHED:
        return strcmp(
            string,
            "VIRTIO_RNG_QUEUE_COMPOSITION_ALREADY_PUBLISHED") == 0;
    case VIRTIO_RNG_QUEUE_COMPOSITION_UNPUBLISHED:
        return strcmp(
            string,
            "VIRTIO_RNG_QUEUE_COMPOSITION_UNPUBLISHED") == 0;
    case VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_PROFILE:
        return strcmp(
            string,
            "VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_PROFILE") == 0;
    case VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_QUEUE_INDEX:
        return strcmp(
            string,
            "VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_QUEUE_INDEX") == 0;
    case VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_QUEUE_SIZE:
        return strcmp(
            string,
            "VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_QUEUE_SIZE") == 0;
    case VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_LAYOUT:
        return strcmp(
            string,
            "VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_LAYOUT") == 0;
    case VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_RING:
        return strcmp(
            string,
            "VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_RING") == 0;
    case VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_BARRIER:
        return strcmp(
            string,
            "VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_BARRIER") == 0;
    case VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_RELEASE:
        return strcmp(
            string,
            "VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_RELEASE") == 0;
    case VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_MAX_LENGTH:
        return strcmp(
            string,
            "VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_MAX_LENGTH") == 0;
    case VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_DMA:
        return strcmp(
            string,
            "VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_DMA") == 0;
    case VIRTIO_RNG_QUEUE_COMPOSITION_DMA_BIND_FAILED:
        return strcmp(
            string,
            "VIRTIO_RNG_QUEUE_COMPOSITION_DMA_BIND_FAILED") == 0;
    case VIRTIO_RNG_QUEUE_COMPOSITION_QUEUE_INIT_FAILED:
        return strcmp(
            string,
            "VIRTIO_RNG_QUEUE_COMPOSITION_QUEUE_INIT_FAILED") == 0;
    case VIRTIO_RNG_QUEUE_COMPOSITION_TEARDOWN_FAILED:
        return strcmp(
            string,
            "VIRTIO_RNG_QUEUE_COMPOSITION_TEARDOWN_FAILED") == 0;
    case VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_STATE:
        return strcmp(
            string,
            "VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_STATE") == 0;
    default:
        return false;
    }
}

static void test_status_strings(void)
{
    static const enum virtio_rng_queue_composition_status statuses[] = {
        VIRTIO_RNG_QUEUE_COMPOSITION_OK,
        VIRTIO_RNG_QUEUE_COMPOSITION_NULL,
        VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_VERSION,
        VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_GENERATION,
        VIRTIO_RNG_QUEUE_COMPOSITION_ALREADY_PUBLISHED,
        VIRTIO_RNG_QUEUE_COMPOSITION_UNPUBLISHED,
        VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_PROFILE,
        VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_QUEUE_INDEX,
        VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_QUEUE_SIZE,
        VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_LAYOUT,
        VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_RING,
        VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_BARRIER,
        VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_RELEASE,
        VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_MAX_LENGTH,
        VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_DMA,
        VIRTIO_RNG_QUEUE_COMPOSITION_DMA_BIND_FAILED,
        VIRTIO_RNG_QUEUE_COMPOSITION_QUEUE_INIT_FAILED,
        VIRTIO_RNG_QUEUE_COMPOSITION_TEARDOWN_FAILED,
        VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_STATE,
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        enum virtio_rng_queue_composition_status status = statuses[index];
        const char *string =
            virtio_rng_queue_composition_status_string(status);

        CHECK(string != NULL && status_string_matches(status, string),
              "virtio rng queue composition status string is exact");
    }
    CHECK(virtio_rng_queue_composition_status_string(
              (enum virtio_rng_queue_composition_status)19U) == NULL,
          "virtio rng queue composition undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio rng queue composition status handling allocates nothing");
}

static void test_null_and_profile(void)
{
    struct virtio_rng_queue_composition composition;
    struct virtio_rng_feature_profile profile;
    struct virtio_rng_queue_config config;
    struct test_barrier_state barrier;
    struct test_reclaim_state reclaim;
    struct test_zeroize_state zeroize;

    memset(&available_ring, 0, sizeof(available_ring));
    memset(&used_ring, 0, sizeof(used_ring));
    memset(&barrier, 0, sizeof(barrier));
    memset(&reclaim, 0, sizeof(reclaim));
    memset(&zeroize, 0, sizeof(zeroize));
    zeroize.allow = true;
    profile = make_profile();
    config = make_config(4U, UINT64_C(0x100000),
                         &barrier, &reclaim, &zeroize);
    memset(&composition, 0, sizeof(composition));

    CHECK(virtio_rng_queue_composition_activate(
              NULL, &profile, &config, 7U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_NULL,
          "rng queue composition activate rejects null composition");
    CHECK(virtio_rng_queue_composition_activate(
              &composition, NULL, &config, 7U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_NULL,
          "rng queue composition activate rejects null profile");
    CHECK(virtio_rng_queue_composition_activate(
              &composition, &profile, NULL, 7U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_NULL,
          "rng queue composition activate rejects null config");

    profile.published = false;
    CHECK(virtio_rng_queue_composition_activate(
              &composition, &profile, &config, 7U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_PROFILE &&
              !composition.published,
          "rng queue composition rejects unpublished profile without mutation");
    profile = make_profile();
    profile.request_queue_count = 2U;
    CHECK(virtio_rng_queue_composition_activate(
              &composition, &profile, &config, 7U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_PROFILE &&
              !composition.published,
          "rng queue composition rejects non-one-queue profile");

    profile = make_profile();
    CHECK(virtio_rng_queue_composition_activate(
              &composition, &profile, &config, 0U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_GENERATION &&
              !composition.published,
          "rng queue composition rejects invalid generation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "rng queue composition null/profile handling allocates nothing");
}

static void test_config_validation(void)
{
    struct virtio_rng_queue_composition composition;
    struct virtio_rng_feature_profile profile;
    struct virtio_rng_queue_config config;
    struct test_barrier_state barrier;
    struct test_reclaim_state reclaim;
    struct test_zeroize_state zeroize;

    memset(&barrier, 0, sizeof(barrier));
    memset(&reclaim, 0, sizeof(reclaim));
    memset(&zeroize, 0, sizeof(zeroize));
    zeroize.allow = true;
    profile = make_profile();
    config = make_config(4U, UINT64_C(0x100000),
                         &barrier, &reclaim, &zeroize);
    memset(&composition, 0, sizeof(composition));

    config.queue_index = 1U;
    CHECK(virtio_rng_queue_composition_activate(
              &composition, &profile, &config, 7U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_QUEUE_INDEX &&
              !composition.published,
          "rng queue composition rejects non-zero queue index");

    config = make_config(4U, UINT64_C(0x100000),
                         &barrier, &reclaim, &zeroize);
    config.queue_size = 3U;
    CHECK(virtio_rng_queue_composition_activate(
              &composition, &profile, &config, 7U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_QUEUE_SIZE &&
              !composition.published,
          "rng queue composition rejects non-power-of-two queue size");

    config = make_config(4U, UINT64_C(0x100000),
                         &barrier, &reclaim, &zeroize);
    config.available_ring = NULL;
    CHECK(virtio_rng_queue_composition_activate(
              &composition, &profile, &config, 7U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_RING &&
              !composition.published,
          "rng queue composition rejects null available ring");

    config = make_config(4U, UINT64_C(0x100000),
                         &barrier, &reclaim, &zeroize);
    config.used_ring = NULL;
    CHECK(virtio_rng_queue_composition_activate(
              &composition, &profile, &config, 7U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_RING &&
              !composition.published,
          "rng queue composition rejects null used ring");

    config = make_config(4U, UINT64_C(0x100000),
                         &barrier, &reclaim, &zeroize);
    config.barrier.fence = NULL;
    CHECK(virtio_rng_queue_composition_activate(
              &composition, &profile, &config, 7U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_BARRIER &&
              !composition.published,
          "rng queue composition rejects null barrier fence");

    config = make_config(4U, UINT64_C(0x100000),
                         &barrier, &reclaim, &zeroize);
    config.barrier.context = NULL;
    CHECK(virtio_rng_queue_composition_activate(
              &composition, &profile, &config, 7U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_BARRIER &&
              !composition.published,
          "rng queue composition rejects null barrier context");

    config = make_config(4U, UINT64_C(0x100000),
                         &barrier, &reclaim, &zeroize);
    config.reclaim.reclaim = NULL;
    CHECK(virtio_rng_queue_composition_activate(
              &composition, &profile, &config, 7U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_RELEASE &&
              !composition.published,
          "rng queue composition rejects null reclaim callback");

    config = make_config(4U, UINT64_C(0x100000),
                         &barrier, &reclaim, &zeroize);
    config.reclaim.context = NULL;
    CHECK(virtio_rng_queue_composition_activate(
              &composition, &profile, &config, 7U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_RELEASE &&
              !composition.published,
          "rng queue composition rejects null reclaim context");

    config = make_config(4U, UINT64_C(0x100000),
                         &barrier, &reclaim, &zeroize);
    config.max_used_length =
        VIRTIO_RNG_QUEUE_COMPOSITION_MAX_USED_LENGTH + 1U;
    CHECK(virtio_rng_queue_composition_activate(
              &composition, &profile, &config, 7U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_MAX_LENGTH &&
              !composition.published,
          "rng queue composition rejects over-limit used length");

    config = make_config(4U, UINT64_C(0x100000),
                         &barrier, &reclaim, &zeroize);
    config.dma_capacity = 1U;
    CHECK(virtio_rng_queue_composition_activate(
              &composition, &profile, &config, 7U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_DMA &&
              !composition.published,
          "rng queue composition rejects undersized DMA capacity");

    config = make_config(4U, UINT64_C(0x100000),
                         &barrier, &reclaim, &zeroize);
    config.dma_window.alignment = 3U;
    CHECK(virtio_rng_queue_composition_activate(
              &composition, &profile, &config, 7U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_DMA &&
              !composition.published,
          "rng queue composition rejects invalid DMA alignment");

    config = make_config(4U, UINT64_C(0x100000),
                         &barrier, &reclaim, &zeroize);
    config.dma_window.length = UINT64_C(0x800);
    CHECK(virtio_rng_queue_composition_activate(
              &composition, &profile, &config, 7U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_DMA &&
              !composition.published,
          "rng queue composition rejects DMA window outside layout");

    config = make_config(64U, UINT64_C(0x100000),
                         &barrier, &reclaim, &zeroize);
    config.region_length = 0x100U;
    CHECK(virtio_rng_queue_composition_activate(
              &composition, &profile, &config, 7U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_LAYOUT &&
              !composition.published,
          "rng queue composition rejects too-small layout range");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "rng queue composition config handling allocates nothing");
}

static void test_success_and_teardown(void)
{
    struct virtio_rng_queue_composition composition;
    struct virtio_rng_feature_profile profile;
    struct virtio_rng_queue_config config;
    struct test_barrier_state barrier;
    struct test_reclaim_state reclaim;
    struct test_zeroize_state zeroize;
    const struct virtio_rng_queue *queue;

    memset(&available_ring, 0, sizeof(available_ring));
    memset(&used_ring, 0, sizeof(used_ring));
    memset(&barrier, 0, sizeof(barrier));
    memset(&reclaim, 0, sizeof(reclaim));
    memset(&zeroize, 0, sizeof(zeroize));
    zeroize.allow = true;
    profile = make_profile();
    config = make_config(4U, UINT64_C(0x100000),
                         &barrier, &reclaim, &zeroize);
    memset(&composition, 0, sizeof(composition));

    CHECK(virtio_rng_queue_composition_activate(
              &composition, &profile, &config, 7U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_OK,
          "valid rng queue composition activates request queue");
    CHECK(virtio_rng_queue_composition_published(&composition) &&
              composition.version == VIRTIO_RNG_QUEUE_COMPOSITION_VERSION &&
              composition.generation == 7U &&
              composition.queue.published &&
              composition.queue.queue_index ==
                  VIRTIO_RNG_QUEUE_COMPOSITION_QUEUE_INDEX &&
              composition.queue.queue_size == 4U,
          "published rng queue composition has exact request queue");
    queue = virtio_rng_queue_composition_queue(&composition);
    CHECK(queue != NULL && queue == &composition.queue &&
              virtio_queue_layout_result_published(&queue->layout) &&
              virtio_descriptor_ownership_published(&queue->descriptors) &&
              virtio_descriptor_ownership_free_count(&queue->descriptors) ==
                  4U &&
              virtio_available_publication_published(&queue->available) &&
              virtio_used_consumption_published(&queue->used) &&
              virtio_dma_region_published(&queue->dma) &&
              virtio_dma_region_bound_count(&queue->dma) == 1U &&
              queue->dma_region.physical_base == UINT64_C(0x100000) &&
              queue->dma_region.length == queue->layout.total_size &&
              queue->dma_region.direction ==
                  VIRTIO_DMA_DIRECTION_BIDIRECTIONAL &&
              queue->dma_region.lifetime == VIRTIO_DMA_LIFETIME_QUEUE &&
              queue->dma_region.zeroization ==
                  VIRTIO_DMA_ZEROIZATION_ON_RELEASE,
          "rng request queue publishes generic controllers and one DMA owner");
    CHECK(virtio_rng_queue_composition_generation(&composition) == 7U,
          "rng queue composition generation accessor is exact");

    CHECK(virtio_rng_queue_composition_teardown(&composition) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_OK &&
              !virtio_rng_queue_composition_published(&composition) &&
              !composition.queue.published &&
              virtio_dma_region_bound_count(&composition.queue.dma) == 0U,
          "rng queue composition teardown releases request queue");
    CHECK(zeroize.calls == 1U,
          "rng queue composition teardown zeroizes queue DMA owner");
    CHECK(virtio_rng_queue_composition_teardown(&composition) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_UNPUBLISHED,
          "rng queue composition teardown is idempotent for unpublished state");

    CHECK(virtio_rng_queue_composition_activate(
              &composition, &profile, &config, 8U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_OK &&
              virtio_rng_queue_composition_generation(&composition) == 8U,
          "rng queue composition permits reinitialization after teardown");
    CHECK(virtio_rng_queue_composition_teardown(&composition) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_OK,
          "rng queue composition second activation tears down cleanly");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "rng queue composition success/teardown allocates nothing");
}

static void test_all_or_nothing(void)
{
    struct virtio_rng_queue_composition composition;
    struct virtio_rng_feature_profile profile;
    struct virtio_rng_queue_config config;
    struct test_barrier_state barrier;
    struct test_reclaim_state reclaim;
    struct test_zeroize_state zeroize;

    memset(&barrier, 0, sizeof(barrier));
    memset(&reclaim, 0, sizeof(reclaim));
    memset(&zeroize, 0, sizeof(zeroize));
    zeroize.allow = true;
    profile = make_profile();
    config = make_config(4U, UINT64_C(0x100000),
                         &barrier, &reclaim, &zeroize);
    config.used_ring = NULL;
    memset(&composition, 0, sizeof(composition));

    CHECK(virtio_rng_queue_composition_activate(
              &composition, &profile, &config, 7U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_RING &&
              !composition.published &&
              !composition.queue.published,
          "rng queue failed setup leaves composition unpublished");

    memset(&composition, 0, sizeof(composition));
    config = make_config(64U, UINT64_C(0x100000),
                         &barrier, &reclaim, &zeroize);
    config.region_length = 0x100U;
    CHECK(virtio_rng_queue_composition_activate(
              &composition, &profile, &config, 7U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_LAYOUT &&
              !composition.published &&
              !composition.queue.published &&
              zeroize.calls == 0U,
          "rng queue layout failure leaves composition unpublished");

    memset(&composition, 0, sizeof(composition));
    config = make_config(4U, UINT64_C(0x100000),
                         &barrier, &reclaim, &zeroize);
    CHECK(virtio_rng_queue_composition_activate(
              &composition, &profile, &config, 9U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_OK,
          "rng queue composition recovers after failed setup");
    CHECK(virtio_rng_queue_composition_teardown(&composition) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_OK,
          "rng queue composition recovered fixture tears down cleanly");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "rng queue composition rollback allocates nothing");
}

static void test_corrupt_state(void)
{
    struct virtio_rng_queue_composition composition;
    struct virtio_rng_feature_profile profile;
    struct virtio_rng_queue_config config;
    struct test_barrier_state barrier;
    struct test_reclaim_state reclaim;
    struct test_zeroize_state zeroize;

    memset(&available_ring, 0, sizeof(available_ring));
    memset(&used_ring, 0, sizeof(used_ring));
    memset(&barrier, 0, sizeof(barrier));
    memset(&reclaim, 0, sizeof(reclaim));
    memset(&zeroize, 0, sizeof(zeroize));
    zeroize.allow = true;
    profile = make_profile();
    config = make_config(4U, UINT64_C(0x100000),
                         &barrier, &reclaim, &zeroize);
    memset(&composition, 0, sizeof(composition));

    CHECK(virtio_rng_queue_composition_activate(
              &composition, &profile, &config, 7U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_OK,
          "corrupt-state fixture activates valid rng queue composition");
    composition.queue.queue_size = 3U;
    CHECK(!virtio_rng_queue_composition_published(&composition),
          "rng queue composition published rejects corrupt queue");
    CHECK(virtio_rng_queue_composition_queue(&composition) == NULL &&
              virtio_rng_queue_composition_generation(&composition) == 0U,
          "rng queue composition accessors reject corrupt state");
    CHECK(virtio_rng_queue_composition_teardown(&composition) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_STATE,
          "rng queue composition teardown rejects corrupt state");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "rng queue composition corrupt-state handling allocates nothing");
}

static void test_zeroize_reject(void)
{
    struct virtio_rng_queue_composition composition;
    struct virtio_rng_feature_profile profile;
    struct virtio_rng_queue_config config;
    struct test_barrier_state barrier;
    struct test_reclaim_state reclaim;
    struct test_zeroize_state zeroize;

    memset(&available_ring, 0, sizeof(available_ring));
    memset(&used_ring, 0, sizeof(used_ring));
    memset(&barrier, 0, sizeof(barrier));
    memset(&reclaim, 0, sizeof(reclaim));
    memset(&zeroize, 0, sizeof(zeroize));
    zeroize.allow = true;
    profile = make_profile();
    config = make_config(4U, UINT64_C(0x100000),
                         &barrier, &reclaim, &zeroize);
    memset(&composition, 0, sizeof(composition));

    CHECK(virtio_rng_queue_composition_activate(
              &composition, &profile, &config, 7U) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_OK,
          "zeroize-reject fixture activates rng queue composition");
    zeroize.allow = false;
    CHECK(virtio_rng_queue_composition_teardown(&composition) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_TEARDOWN_FAILED &&
              virtio_rng_queue_composition_published(&composition) &&
              composition.queue.published &&
              zeroize.calls == 0U,
          "rng queue composition teardown fails closed on rejected zeroization");
    zeroize.allow = true;
    CHECK(virtio_rng_queue_composition_teardown(&composition) ==
              VIRTIO_RNG_QUEUE_COMPOSITION_OK &&
              !virtio_rng_queue_composition_published(&composition) &&
              zeroize.calls == 1U,
          "rng queue composition teardown retries after zeroization succeeds");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "rng queue composition zeroize-reject handling allocates nothing");
}

int main(void)
{
    test_status_strings();
    test_null_and_profile();
    test_config_validation();
    test_success_and_teardown();
    test_all_or_nothing();
    test_corrupt_state();
    test_zeroize_reject();

    if (failures != 0) {
        fprintf(stderr, "Virtio rng queue composition host tests: FAIL\n");
        return 1;
    }
    printf("Virtio rng queue composition host tests: ALL OK\n");
    return 0;
}

#endif
