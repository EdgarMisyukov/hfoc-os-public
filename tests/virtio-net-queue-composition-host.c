#include "virtio_net_queue_composition.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_VIRTIO_NET_QUEUE_COMPOSITION_HOST_TEST

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

static struct virtio_available_ring rx_available_ring;
static struct virtio_available_ring tx_available_ring;
static struct virtio_used_ring rx_used_ring;
static struct virtio_used_ring tx_used_ring;

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
    identity.device_id = VIRTIO_PCI_DEVICE_ID_NET_MODERN;
    identity.bdf.bus = 0U;
    identity.bdf.device = 0U;
    identity.bdf.function = 0U;
    identity.modern = 1U;
    return identity;
}

static struct virtio_net_feature_profile make_profile(void)
{
    struct virtio_net_feature_profile profile;
    struct virtio_pci_identity identity;
    struct virtio_net_feature_bitset offered;
    struct virtio_net_configuration configuration;
    struct virtio_net_topology topology;

    identity = make_identity();
    offered.low = VIRTIO_NET_REQUIRED_FEATURE_LO_MASK;
    offered.high = VIRTIO_NET_VERSION_1_HIGH_BIT;
    memset(&configuration, 0, sizeof(configuration));
    configuration.version = VIRTIO_NET_CONFIGURATION_VERSION;
    configuration.mac_address[0] = 0x52U;
    configuration.mac_address[1] = 0x54U;
    configuration.mac_address[2] = 0x00U;
    configuration.mac_address[3] = 0x12U;
    configuration.mac_address[4] = 0x34U;
    configuration.mac_address[5] = 0x56U;
    configuration.status = VIRTIO_NET_STATUS_LINK_UP;
    topology.max_virtqueue_pairs = 1U;
    topology.receive_queue_count = 1U;
    topology.transmit_queue_count = 1U;
    topology.control_queue_present = false;

    memset(&profile, 0, sizeof(profile));
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology,
              1U) == VIRTIO_NET_FEATURE_PROFILE_OK,
          "host profile helper activates valid modern profile");
    return profile;
}

static struct virtio_net_queue_config make_config(
    enum virtio_net_queue_direction direction,
    uint32_t queue_index,
    uint32_t queue_size,
    struct virtio_available_ring *available_ring,
    struct virtio_used_ring *used_ring,
    uint64_t region_base,
    struct test_barrier_state *barrier,
    struct test_reclaim_state *reclaim,
    struct test_zeroize_state *zeroize)
{
    struct virtio_net_queue_config config;

    memset(&config, 0, sizeof(config));
    config.direction = direction;
    config.queue_index = queue_index;
    config.queue_size = queue_size;
    config.event_index = false;
    config.region_base = region_base;
    config.region_length = UINT64_C(0x1000);
    config.available_ring = available_ring;
    config.used_ring = used_ring;
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
    enum virtio_net_queue_composition_status status,
    const char *string)
{
    switch (status) {
    case VIRTIO_NET_QUEUE_COMPOSITION_OK:
        return strcmp(
            string, "VIRTIO_NET_QUEUE_COMPOSITION_OK") == 0;
    case VIRTIO_NET_QUEUE_COMPOSITION_NULL:
        return strcmp(
            string, "VIRTIO_NET_QUEUE_COMPOSITION_NULL") == 0;
    case VIRTIO_NET_QUEUE_COMPOSITION_INVALID_VERSION:
        return strcmp(
            string,
            "VIRTIO_NET_QUEUE_COMPOSITION_INVALID_VERSION") == 0;
    case VIRTIO_NET_QUEUE_COMPOSITION_INVALID_GENERATION:
        return strcmp(
            string,
            "VIRTIO_NET_QUEUE_COMPOSITION_INVALID_GENERATION") == 0;
    case VIRTIO_NET_QUEUE_COMPOSITION_ALREADY_PUBLISHED:
        return strcmp(
            string,
            "VIRTIO_NET_QUEUE_COMPOSITION_ALREADY_PUBLISHED") == 0;
    case VIRTIO_NET_QUEUE_COMPOSITION_UNPUBLISHED:
        return strcmp(
            string,
            "VIRTIO_NET_QUEUE_COMPOSITION_UNPUBLISHED") == 0;
    case VIRTIO_NET_QUEUE_COMPOSITION_INVALID_PROFILE:
        return strcmp(
            string,
            "VIRTIO_NET_QUEUE_COMPOSITION_INVALID_PROFILE") == 0;
    case VIRTIO_NET_QUEUE_COMPOSITION_INVALID_CONFIG:
        return strcmp(
            string,
            "VIRTIO_NET_QUEUE_COMPOSITION_INVALID_CONFIG") == 0;
    case VIRTIO_NET_QUEUE_COMPOSITION_INVALID_DIRECTION:
        return strcmp(
            string,
            "VIRTIO_NET_QUEUE_COMPOSITION_INVALID_DIRECTION") == 0;
    case VIRTIO_NET_QUEUE_COMPOSITION_INVALID_QUEUE_INDEX:
        return strcmp(
            string,
            "VIRTIO_NET_QUEUE_COMPOSITION_INVALID_QUEUE_INDEX") == 0;
    case VIRTIO_NET_QUEUE_COMPOSITION_INVALID_QUEUE_SIZE:
        return strcmp(
            string,
            "VIRTIO_NET_QUEUE_COMPOSITION_INVALID_QUEUE_SIZE") == 0;
    case VIRTIO_NET_QUEUE_COMPOSITION_INVALID_LAYOUT:
        return strcmp(
            string,
            "VIRTIO_NET_QUEUE_COMPOSITION_INVALID_LAYOUT") == 0;
    case VIRTIO_NET_QUEUE_COMPOSITION_INVALID_RING:
        return strcmp(
            string,
            "VIRTIO_NET_QUEUE_COMPOSITION_INVALID_RING") == 0;
    case VIRTIO_NET_QUEUE_COMPOSITION_INVALID_BARRIER:
        return strcmp(
            string,
            "VIRTIO_NET_QUEUE_COMPOSITION_INVALID_BARRIER") == 0;
    case VIRTIO_NET_QUEUE_COMPOSITION_INVALID_RELEASE:
        return strcmp(
            string,
            "VIRTIO_NET_QUEUE_COMPOSITION_INVALID_RELEASE") == 0;
    case VIRTIO_NET_QUEUE_COMPOSITION_INVALID_MAX_LENGTH:
        return strcmp(
            string,
            "VIRTIO_NET_QUEUE_COMPOSITION_INVALID_MAX_LENGTH") == 0;
    case VIRTIO_NET_QUEUE_COMPOSITION_INVALID_DMA:
        return strcmp(
            string,
            "VIRTIO_NET_QUEUE_COMPOSITION_INVALID_DMA") == 0;
    case VIRTIO_NET_QUEUE_COMPOSITION_DMA_BIND_FAILED:
        return strcmp(
            string,
            "VIRTIO_NET_QUEUE_COMPOSITION_DMA_BIND_FAILED") == 0;
    case VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_INIT_FAILED:
        return strcmp(
            string,
            "VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_INIT_FAILED") == 0;
    case VIRTIO_NET_QUEUE_COMPOSITION_TEARDOWN_FAILED:
        return strcmp(
            string,
            "VIRTIO_NET_QUEUE_COMPOSITION_TEARDOWN_FAILED") == 0;
    case VIRTIO_NET_QUEUE_COMPOSITION_INVALID_STATE:
        return strcmp(
            string,
            "VIRTIO_NET_QUEUE_COMPOSITION_INVALID_STATE") == 0;
    default:
        return false;
    }
}

static void test_status_strings(void)
{
    static const enum virtio_net_queue_composition_status statuses[] = {
        VIRTIO_NET_QUEUE_COMPOSITION_OK,
        VIRTIO_NET_QUEUE_COMPOSITION_NULL,
        VIRTIO_NET_QUEUE_COMPOSITION_INVALID_VERSION,
        VIRTIO_NET_QUEUE_COMPOSITION_INVALID_GENERATION,
        VIRTIO_NET_QUEUE_COMPOSITION_ALREADY_PUBLISHED,
        VIRTIO_NET_QUEUE_COMPOSITION_UNPUBLISHED,
        VIRTIO_NET_QUEUE_COMPOSITION_INVALID_PROFILE,
        VIRTIO_NET_QUEUE_COMPOSITION_INVALID_CONFIG,
        VIRTIO_NET_QUEUE_COMPOSITION_INVALID_DIRECTION,
        VIRTIO_NET_QUEUE_COMPOSITION_INVALID_QUEUE_INDEX,
        VIRTIO_NET_QUEUE_COMPOSITION_INVALID_QUEUE_SIZE,
        VIRTIO_NET_QUEUE_COMPOSITION_INVALID_LAYOUT,
        VIRTIO_NET_QUEUE_COMPOSITION_INVALID_RING,
        VIRTIO_NET_QUEUE_COMPOSITION_INVALID_BARRIER,
        VIRTIO_NET_QUEUE_COMPOSITION_INVALID_RELEASE,
        VIRTIO_NET_QUEUE_COMPOSITION_INVALID_MAX_LENGTH,
        VIRTIO_NET_QUEUE_COMPOSITION_INVALID_DMA,
        VIRTIO_NET_QUEUE_COMPOSITION_DMA_BIND_FAILED,
        VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_INIT_FAILED,
        VIRTIO_NET_QUEUE_COMPOSITION_TEARDOWN_FAILED,
        VIRTIO_NET_QUEUE_COMPOSITION_INVALID_STATE,
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        enum virtio_net_queue_composition_status status = statuses[index];
        const char *string =
            virtio_net_queue_composition_status_string(status);

        CHECK(string != NULL && status_string_matches(status, string),
              "virtio net queue composition status string is exact");
    }
    CHECK(virtio_net_queue_composition_status_string(
              (enum virtio_net_queue_composition_status)21U) == NULL,
          "virtio net queue composition undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio net queue composition status handling allocates nothing");
}

static void test_null_and_profile(void)
{
    struct virtio_net_queue_composition composition;
    struct virtio_net_feature_profile profile;
    struct virtio_net_queue_config receive_config;
    struct virtio_net_queue_config transmit_config;
    struct test_barrier_state rx_barrier;
    struct test_barrier_state tx_barrier;
    struct test_reclaim_state rx_reclaim;
    struct test_reclaim_state tx_reclaim;
    struct test_zeroize_state rx_zeroize;
    struct test_zeroize_state tx_zeroize;

    memset(&rx_available_ring, 0, sizeof(rx_available_ring));
    memset(&tx_available_ring, 0, sizeof(tx_available_ring));
    memset(&rx_used_ring, 0, sizeof(rx_used_ring));
    memset(&tx_used_ring, 0, sizeof(tx_used_ring));
    memset(&rx_barrier, 0, sizeof(rx_barrier));
    memset(&tx_barrier, 0, sizeof(tx_barrier));
    memset(&rx_reclaim, 0, sizeof(rx_reclaim));
    memset(&tx_reclaim, 0, sizeof(tx_reclaim));
    memset(&rx_zeroize, 0, sizeof(rx_zeroize));
    memset(&tx_zeroize, 0, sizeof(tx_zeroize));
    rx_zeroize.allow = true;
    tx_zeroize.allow = true;
    profile = make_profile();
    receive_config = make_config(
        VIRTIO_NET_QUEUE_RECEIVE, VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_INDEX_RX,
        4U, &rx_available_ring, &rx_used_ring, UINT64_C(0x100000),
        &rx_barrier, &rx_reclaim, &rx_zeroize);
    transmit_config = make_config(
        VIRTIO_NET_QUEUE_TRANSMIT, VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_INDEX_TX,
        4U, &tx_available_ring, &tx_used_ring, UINT64_C(0x200000),
        &tx_barrier, &tx_reclaim, &tx_zeroize);
    memset(&composition, 0, sizeof(composition));

    CHECK(virtio_net_queue_composition_activate(
              NULL, &profile, &receive_config, &transmit_config, 7U) ==
              VIRTIO_NET_QUEUE_COMPOSITION_NULL,
          "queue composition activate rejects null composition");
    CHECK(virtio_net_queue_composition_activate(
              &composition, NULL, &receive_config, &transmit_config, 7U) ==
              VIRTIO_NET_QUEUE_COMPOSITION_NULL,
          "queue composition activate rejects null profile");
    CHECK(virtio_net_queue_composition_activate(
              &composition, &profile, NULL, &transmit_config, 7U) ==
              VIRTIO_NET_QUEUE_COMPOSITION_NULL,
          "queue composition activate rejects null receive config");
    CHECK(virtio_net_queue_composition_activate(
              &composition, &profile, &receive_config, NULL, 7U) ==
              VIRTIO_NET_QUEUE_COMPOSITION_NULL,
          "queue composition activate rejects null transmit config");

    profile.published = false;
    CHECK(virtio_net_queue_composition_activate(
              &composition, &profile, &receive_config, &transmit_config, 7U) ==
              VIRTIO_NET_QUEUE_COMPOSITION_INVALID_PROFILE &&
              !composition.published,
          "queue composition rejects unpublished profile without mutation");
    profile = make_profile();
    profile.receive_queue_count = 2U;
    CHECK(virtio_net_queue_composition_activate(
              &composition, &profile, &receive_config, &transmit_config, 7U) ==
              VIRTIO_NET_QUEUE_COMPOSITION_INVALID_PROFILE &&
              !composition.published,
          "queue composition rejects non-one-RX profile");

    profile = make_profile();
    CHECK(virtio_net_queue_composition_activate(
              &composition, &profile, &receive_config, &transmit_config, 0U) ==
              VIRTIO_NET_QUEUE_COMPOSITION_INVALID_GENERATION &&
              !composition.published,
          "queue composition rejects invalid generation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "queue composition null/profile handling allocates nothing");
}

static void test_success_and_teardown(void)
{
    struct virtio_net_queue_composition composition;
    struct virtio_net_feature_profile profile;
    struct virtio_net_queue_config receive_config;
    struct virtio_net_queue_config transmit_config;
    struct test_barrier_state rx_barrier;
    struct test_barrier_state tx_barrier;
    struct test_reclaim_state rx_reclaim;
    struct test_reclaim_state tx_reclaim;
    struct test_zeroize_state rx_zeroize;
    struct test_zeroize_state tx_zeroize;
    const struct virtio_net_queue *receive;
    const struct virtio_net_queue *transmit;

    memset(&rx_available_ring, 0, sizeof(rx_available_ring));
    memset(&tx_available_ring, 0, sizeof(tx_available_ring));
    memset(&rx_used_ring, 0, sizeof(rx_used_ring));
    memset(&tx_used_ring, 0, sizeof(tx_used_ring));
    memset(&rx_barrier, 0, sizeof(rx_barrier));
    memset(&tx_barrier, 0, sizeof(tx_barrier));
    memset(&rx_reclaim, 0, sizeof(rx_reclaim));
    memset(&tx_reclaim, 0, sizeof(tx_reclaim));
    memset(&rx_zeroize, 0, sizeof(rx_zeroize));
    memset(&tx_zeroize, 0, sizeof(tx_zeroize));
    rx_zeroize.allow = true;
    tx_zeroize.allow = true;
    profile = make_profile();
    receive_config = make_config(
        VIRTIO_NET_QUEUE_RECEIVE, VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_INDEX_RX,
        4U, &rx_available_ring, &rx_used_ring, UINT64_C(0x100000),
        &rx_barrier, &rx_reclaim, &rx_zeroize);
    transmit_config = make_config(
        VIRTIO_NET_QUEUE_TRANSMIT, VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_INDEX_TX,
        4U, &tx_available_ring, &tx_used_ring, UINT64_C(0x200000),
        &tx_barrier, &tx_reclaim, &tx_zeroize);
    memset(&composition, 0, sizeof(composition));

    CHECK(virtio_net_queue_composition_activate(
              &composition, &profile, &receive_config, &transmit_config, 7U) ==
              VIRTIO_NET_QUEUE_COMPOSITION_OK,
          "valid queue composition activates both queues");
    CHECK(virtio_net_queue_composition_published(&composition) &&
              composition.version == VIRTIO_NET_QUEUE_COMPOSITION_VERSION &&
              composition.generation == 7U &&
              composition.receive.published &&
              composition.transmit.published &&
              composition.receive.direction == VIRTIO_NET_QUEUE_RECEIVE &&
              composition.transmit.direction == VIRTIO_NET_QUEUE_TRANSMIT &&
              composition.receive.queue_index ==
                  VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_INDEX_RX &&
              composition.transmit.queue_index ==
                  VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_INDEX_TX &&
              composition.receive.queue_size == 4U &&
              composition.transmit.queue_size == 4U,
          "published queue composition has exact queue pair");
    receive = virtio_net_queue_composition_receive(&composition);
    transmit = virtio_net_queue_composition_transmit(&composition);
    CHECK(receive != NULL && receive == &composition.receive &&
              virtio_queue_layout_result_published(&receive->layout) &&
              virtio_descriptor_ownership_published(&receive->descriptors) &&
              virtio_descriptor_ownership_free_count(&receive->descriptors) ==
                  4U &&
              virtio_available_publication_published(&receive->available) &&
              virtio_used_consumption_published(&receive->used) &&
              virtio_dma_region_published(&receive->dma) &&
              virtio_dma_region_bound_count(&receive->dma) == 1U &&
              receive->dma_region.physical_base ==
                  UINT64_C(0x100000) &&
              receive->dma_region.length == receive->layout.total_size &&
              receive->dma_region.direction ==
                  VIRTIO_DMA_DIRECTION_BIDIRECTIONAL &&
              receive->dma_region.lifetime == VIRTIO_DMA_LIFETIME_QUEUE &&
              receive->dma_region.zeroization ==
                  VIRTIO_DMA_ZEROIZATION_ON_RELEASE,
          "receive queue publishes generic controllers and one DMA owner");
    CHECK(transmit != NULL && transmit == &composition.transmit &&
              virtio_queue_layout_result_published(&transmit->layout) &&
              virtio_descriptor_ownership_published(&transmit->descriptors) &&
              virtio_descriptor_ownership_free_count(&transmit->descriptors) ==
                  4U &&
              virtio_available_publication_published(&transmit->available) &&
              virtio_used_consumption_published(&transmit->used) &&
              virtio_dma_region_published(&transmit->dma) &&
              virtio_dma_region_bound_count(&transmit->dma) == 1U &&
              transmit->dma_region.physical_base ==
                  UINT64_C(0x200000) &&
              transmit->dma_region.length == transmit->layout.total_size &&
              transmit->dma_region.direction ==
                  VIRTIO_DMA_DIRECTION_BIDIRECTIONAL &&
              transmit->dma_region.lifetime == VIRTIO_DMA_LIFETIME_QUEUE &&
              transmit->dma_region.zeroization ==
                  VIRTIO_DMA_ZEROIZATION_ON_RELEASE,
          "transmit queue publishes generic controllers and one DMA owner");
    CHECK(virtio_net_queue_composition_generation(&composition) == 7U,
          "queue composition generation accessor is exact");

    CHECK(virtio_net_queue_composition_teardown(&composition) ==
              VIRTIO_NET_QUEUE_COMPOSITION_OK &&
              !virtio_net_queue_composition_published(&composition) &&
              !composition.receive.published &&
              !composition.transmit.published &&
              virtio_dma_region_bound_count(&composition.receive.dma) == 0U &&
              virtio_dma_region_bound_count(&composition.transmit.dma) == 0U,
          "queue composition teardown releases both queues");
    CHECK(rx_zeroize.calls == 1U && tx_zeroize.calls == 1U,
          "queue composition teardown zeroizes both queue DMA owners");
    CHECK(virtio_net_queue_composition_teardown(&composition) ==
              VIRTIO_NET_QUEUE_COMPOSITION_UNPUBLISHED,
          "queue composition teardown is idempotent for unpublished state");

    CHECK(virtio_net_queue_composition_activate(
              &composition, &profile, &receive_config, &transmit_config, 8U) ==
              VIRTIO_NET_QUEUE_COMPOSITION_OK &&
              virtio_net_queue_composition_generation(&composition) == 8U,
          "queue composition permits reinitialization after teardown");
    CHECK(virtio_net_queue_composition_teardown(&composition) ==
              VIRTIO_NET_QUEUE_COMPOSITION_OK,
          "queue composition second activation tears down cleanly");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "queue composition success/teardown allocates nothing");
}

static void test_all_or_nothing(void)
{
    struct virtio_net_queue_composition composition;
    struct virtio_net_feature_profile profile;
    struct virtio_net_queue_config receive_config;
    struct virtio_net_queue_config transmit_config;
    struct test_barrier_state rx_barrier;
    struct test_barrier_state tx_barrier;
    struct test_reclaim_state rx_reclaim;
    struct test_reclaim_state tx_reclaim;
    struct test_zeroize_state rx_zeroize;
    struct test_zeroize_state tx_zeroize;

    memset(&rx_available_ring, 0, sizeof(rx_available_ring));
    memset(&tx_available_ring, 0, sizeof(tx_available_ring));
    memset(&rx_used_ring, 0, sizeof(rx_used_ring));
    memset(&tx_used_ring, 0, sizeof(tx_used_ring));
    memset(&rx_barrier, 0, sizeof(rx_barrier));
    memset(&tx_barrier, 0, sizeof(tx_barrier));
    memset(&rx_reclaim, 0, sizeof(rx_reclaim));
    memset(&tx_reclaim, 0, sizeof(tx_reclaim));
    memset(&rx_zeroize, 0, sizeof(rx_zeroize));
    memset(&tx_zeroize, 0, sizeof(tx_zeroize));
    rx_zeroize.allow = true;
    tx_zeroize.allow = true;
    profile = make_profile();
    receive_config = make_config(
        VIRTIO_NET_QUEUE_RECEIVE, VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_INDEX_RX,
        4U, &rx_available_ring, &rx_used_ring, UINT64_C(0x100000),
        &rx_barrier, &rx_reclaim, &rx_zeroize);
    transmit_config = make_config(
        VIRTIO_NET_QUEUE_TRANSMIT, VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_INDEX_TX,
        4U, &tx_available_ring, &tx_used_ring, UINT64_C(0x200000),
        &tx_barrier, &tx_reclaim, &tx_zeroize);

    memset(&composition, 0, sizeof(composition));
    transmit_config.queue_index = VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_INDEX_RX;
    CHECK(virtio_net_queue_composition_activate(
              &composition, &profile, &receive_config, &transmit_config, 7U) ==
              VIRTIO_NET_QUEUE_COMPOSITION_INVALID_QUEUE_INDEX &&
              !composition.published &&
              !composition.receive.published &&
              !composition.transmit.published,
          "TX index failure leaves queue composition unpublished");

    memset(&composition, 0, sizeof(composition));
    memset(&rx_zeroize, 0, sizeof(rx_zeroize));
    memset(&tx_zeroize, 0, sizeof(tx_zeroize));
    rx_zeroize.allow = true;
    tx_zeroize.allow = true;
    transmit_config = make_config(
        VIRTIO_NET_QUEUE_TRANSMIT, VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_INDEX_TX,
        4U, &tx_available_ring, &tx_used_ring, UINT64_C(0x200000),
        &tx_barrier, &tx_reclaim, &tx_zeroize);
    transmit_config.used_ring = NULL;
    CHECK(virtio_net_queue_composition_activate(
              &composition, &profile, &receive_config, &transmit_config, 7U) ==
              VIRTIO_NET_QUEUE_COMPOSITION_INVALID_RING &&
              !composition.published &&
              !composition.receive.published &&
              !composition.transmit.published &&
              rx_zeroize.calls == 1U,
          "TX ring failure tears down the already initialized RX queue");

    memset(&composition, 0, sizeof(composition));
    memset(&rx_zeroize, 0, sizeof(rx_zeroize));
    memset(&tx_zeroize, 0, sizeof(tx_zeroize));
    rx_zeroize.allow = true;
    tx_zeroize.allow = true;
    receive_config.available_ring = NULL;
    transmit_config = make_config(
        VIRTIO_NET_QUEUE_TRANSMIT, VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_INDEX_TX,
        4U, &tx_available_ring, &tx_used_ring, UINT64_C(0x200000),
        &tx_barrier, &tx_reclaim, &tx_zeroize);
    CHECK(virtio_net_queue_composition_activate(
              &composition, &profile, &receive_config, &transmit_config, 7U) ==
              VIRTIO_NET_QUEUE_COMPOSITION_INVALID_RING &&
              !composition.published &&
              rx_zeroize.calls == 0U,
          "RX failure before publication leaves composition unpublished");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "queue composition rollback allocates nothing");
}

static void test_corrupt_state(void)
{
    struct virtio_net_queue_composition composition;
    struct virtio_net_feature_profile profile;
    struct virtio_net_queue_config receive_config;
    struct virtio_net_queue_config transmit_config;
    struct test_barrier_state rx_barrier;
    struct test_barrier_state tx_barrier;
    struct test_reclaim_state rx_reclaim;
    struct test_reclaim_state tx_reclaim;
    struct test_zeroize_state rx_zeroize;
    struct test_zeroize_state tx_zeroize;

    memset(&rx_available_ring, 0, sizeof(rx_available_ring));
    memset(&tx_available_ring, 0, sizeof(tx_available_ring));
    memset(&rx_used_ring, 0, sizeof(rx_used_ring));
    memset(&tx_used_ring, 0, sizeof(tx_used_ring));
    memset(&rx_barrier, 0, sizeof(rx_barrier));
    memset(&tx_barrier, 0, sizeof(tx_barrier));
    memset(&rx_reclaim, 0, sizeof(rx_reclaim));
    memset(&tx_reclaim, 0, sizeof(tx_reclaim));
    memset(&rx_zeroize, 0, sizeof(rx_zeroize));
    memset(&tx_zeroize, 0, sizeof(tx_zeroize));
    rx_zeroize.allow = true;
    tx_zeroize.allow = true;
    profile = make_profile();
    receive_config = make_config(
        VIRTIO_NET_QUEUE_RECEIVE, VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_INDEX_RX,
        4U, &rx_available_ring, &rx_used_ring, UINT64_C(0x100000),
        &rx_barrier, &rx_reclaim, &rx_zeroize);
    transmit_config = make_config(
        VIRTIO_NET_QUEUE_TRANSMIT, VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_INDEX_TX,
        4U, &tx_available_ring, &tx_used_ring, UINT64_C(0x200000),
        &tx_barrier, &tx_reclaim, &tx_zeroize);
    memset(&composition, 0, sizeof(composition));

    CHECK(virtio_net_queue_composition_activate(
              &composition, &profile, &receive_config, &transmit_config, 7U) ==
              VIRTIO_NET_QUEUE_COMPOSITION_OK,
          "corrupt-state fixture activates valid queue composition");
    composition.receive.queue_size = 3U;
    CHECK(!virtio_net_queue_composition_published(&composition),
          "queue composition published rejects corrupt receive queue");
    CHECK(virtio_net_queue_composition_receive(&composition) == NULL &&
              virtio_net_queue_composition_transmit(&composition) == NULL &&
              virtio_net_queue_composition_generation(&composition) == 0U,
          "queue composition accessors reject corrupt state");
    CHECK(virtio_net_queue_composition_teardown(&composition) ==
              VIRTIO_NET_QUEUE_COMPOSITION_INVALID_STATE,
          "queue composition teardown rejects corrupt state");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "queue composition corrupt-state handling allocates nothing");
}

static void test_zeroize_reject(void)
{
    struct virtio_net_queue_composition composition;
    struct virtio_net_feature_profile profile;
    struct virtio_net_queue_config receive_config;
    struct virtio_net_queue_config transmit_config;
    struct test_barrier_state rx_barrier;
    struct test_barrier_state tx_barrier;
    struct test_reclaim_state rx_reclaim;
    struct test_reclaim_state tx_reclaim;
    struct test_zeroize_state rx_zeroize;
    struct test_zeroize_state tx_zeroize;

    memset(&rx_available_ring, 0, sizeof(rx_available_ring));
    memset(&tx_available_ring, 0, sizeof(tx_available_ring));
    memset(&rx_used_ring, 0, sizeof(rx_used_ring));
    memset(&tx_used_ring, 0, sizeof(tx_used_ring));
    memset(&rx_barrier, 0, sizeof(rx_barrier));
    memset(&tx_barrier, 0, sizeof(tx_barrier));
    memset(&rx_reclaim, 0, sizeof(rx_reclaim));
    memset(&tx_reclaim, 0, sizeof(tx_reclaim));
    memset(&rx_zeroize, 0, sizeof(rx_zeroize));
    memset(&tx_zeroize, 0, sizeof(tx_zeroize));
    rx_zeroize.allow = true;
    tx_zeroize.allow = true;
    profile = make_profile();
    receive_config = make_config(
        VIRTIO_NET_QUEUE_RECEIVE, VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_INDEX_RX,
        4U, &rx_available_ring, &rx_used_ring, UINT64_C(0x100000),
        &rx_barrier, &rx_reclaim, &rx_zeroize);
    transmit_config = make_config(
        VIRTIO_NET_QUEUE_TRANSMIT, VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_INDEX_TX,
        4U, &tx_available_ring, &tx_used_ring, UINT64_C(0x200000),
        &tx_barrier, &tx_reclaim, &tx_zeroize);
    memset(&composition, 0, sizeof(composition));

    CHECK(virtio_net_queue_composition_activate(
              &composition, &profile, &receive_config, &transmit_config, 7U) ==
              VIRTIO_NET_QUEUE_COMPOSITION_OK,
          "zeroize-reject fixture activates queue composition");
    tx_zeroize.allow = false;
    CHECK(virtio_net_queue_composition_teardown(&composition) ==
              VIRTIO_NET_QUEUE_COMPOSITION_TEARDOWN_FAILED &&
              virtio_net_queue_composition_published(&composition) &&
              composition.receive.published &&
              composition.transmit.published &&
              rx_zeroize.calls == 0U &&
              tx_zeroize.calls == 0U,
          "queue composition teardown fails closed on rejected zeroization");
    tx_zeroize.allow = true;
    CHECK(virtio_net_queue_composition_teardown(&composition) ==
              VIRTIO_NET_QUEUE_COMPOSITION_OK &&
              !virtio_net_queue_composition_published(&composition) &&
              rx_zeroize.calls == 1U &&
              tx_zeroize.calls == 1U,
          "queue composition teardown retries after zeroization succeeds");

    CHECK(virtio_net_queue_composition_activate(
              &composition, &profile, &receive_config, &transmit_config, 7U) ==
              VIRTIO_NET_QUEUE_COMPOSITION_OK,
          "zeroize-reject fixture reactivates after TX rejection");
    rx_zeroize.allow = false;
    CHECK(virtio_net_queue_composition_teardown(&composition) ==
              VIRTIO_NET_QUEUE_COMPOSITION_TEARDOWN_FAILED &&
              virtio_net_queue_composition_published(&composition) &&
              composition.receive.published &&
              composition.transmit.published &&
              virtio_net_queue_composition_generation(&composition) == 7U,
          "queue composition teardown fails closed on RX rejection after TX");
    rx_zeroize.allow = true;
    CHECK(virtio_net_queue_composition_teardown(&composition) ==
              VIRTIO_NET_QUEUE_COMPOSITION_OK &&
              !virtio_net_queue_composition_published(&composition),
          "queue composition teardown retries after RX rejection");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "queue composition zeroize-reject handling allocates nothing");
}

int main(void)
{
    test_status_strings();
    test_null_and_profile();
    test_success_and_teardown();
    test_all_or_nothing();
    test_corrupt_state();
    test_zeroize_reject();

    if (failures != 0) {
        fprintf(stderr, "Virtio net queue composition host tests: FAIL\n");
        return 1;
    }
    printf("Virtio net queue composition host tests: ALL OK\n");
    return 0;
}

#endif
