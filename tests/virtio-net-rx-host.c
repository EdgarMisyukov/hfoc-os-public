#include "virtio_net_rx.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_VIRTIO_NET_RX_HOST_TEST

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

struct rx_fixture {
    struct virtio_available_ring rx_available;
    struct virtio_available_ring tx_available;
    struct virtio_used_ring rx_used;
    struct virtio_used_ring tx_used;
    struct test_barrier_state rx_barrier;
    struct test_barrier_state tx_barrier;
    struct test_reclaim_state rx_reclaim;
    struct test_reclaim_state tx_reclaim;
    struct test_zeroize_state rx_zeroize;
    struct test_zeroize_state tx_zeroize;
    struct virtio_net_feature_profile profile;
    struct virtio_net_queue_composition composition;
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

static struct virtio_net_queue_config make_queue_config(
    enum virtio_net_queue_direction direction,
    uint32_t queue_index,
    uint32_t queue_size,
    uint64_t region_base,
    struct virtio_available_ring *available,
    struct virtio_used_ring *used,
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
    config.available_ring = available;
    config.used_ring = used;
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
    config.dma_capacity = 8U;
    return config;
}

static void activate_composition(
    struct rx_fixture *fixture,
    uint32_t queue_size)
{
    struct virtio_net_queue_config receive_config;
    struct virtio_net_queue_config transmit_config;

    memset(fixture, 0, sizeof(*fixture));
    fixture->rx_zeroize.allow = true;
    fixture->tx_zeroize.allow = true;
    fixture->profile = make_profile();
    receive_config = make_queue_config(
        VIRTIO_NET_QUEUE_RECEIVE,
        VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_INDEX_RX,
        queue_size,
        UINT64_C(0x100000),
        &fixture->rx_available,
        &fixture->rx_used,
        &fixture->rx_barrier,
        &fixture->rx_reclaim,
        &fixture->rx_zeroize);
    transmit_config = make_queue_config(
        VIRTIO_NET_QUEUE_TRANSMIT,
        VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_INDEX_TX,
        queue_size,
        UINT64_C(0x200000),
        &fixture->tx_available,
        &fixture->tx_used,
        &fixture->tx_barrier,
        &fixture->tx_reclaim,
        &fixture->tx_zeroize);

    CHECK(virtio_net_queue_composition_activate(
              &fixture->composition,
              &fixture->profile,
              &receive_config,
              &transmit_config,
              7U) == VIRTIO_NET_QUEUE_COMPOSITION_OK,
          "host composition helper activates receive and transmit queues");
}

static struct virtio_net_rx_config make_rx_config(
    const struct virtio_net_rx_buffer_descriptor *buffers,
    uint32_t buffer_count,
    struct test_zeroize_state *zeroize)
{
    struct virtio_net_rx_config config;

    memset(&config, 0, sizeof(config));
    config.buffers = buffers;
    config.buffer_count = buffer_count;
    config.minimum_frame_capacity = 64U;
    config.dma_window.base = UINT64_C(0x110000);
    config.dma_window.length = UINT64_C(0x1000);
    config.dma_window.alignment = 16U;
    config.dma_zeroize.zeroize = test_zeroize;
    config.dma_zeroize.context = zeroize;
    config.dma_capacity = buffer_count;
    return config;
}

static void fill_default_buffers(
    struct virtio_net_rx_buffer_descriptor *buffers,
    uint32_t count)
{
    uint32_t index;

    for (index = 0U; index < count; ++index) {
        buffers[index].physical_base =
            UINT64_C(0x110000) + (uint64_t)index * UINT64_C(0x100);
        buffers[index].length = 128U;
    }
}

static bool status_string_matches(
    enum virtio_net_rx_status status,
    const char *string)
{
    switch (status) {
    case VIRTIO_NET_RX_OK:
        return strcmp(string, "VIRTIO_NET_RX_OK") == 0;
    case VIRTIO_NET_RX_NULL:
        return strcmp(string, "VIRTIO_NET_RX_NULL") == 0;
    case VIRTIO_NET_RX_INVALID_VERSION:
        return strcmp(
            string, "VIRTIO_NET_RX_INVALID_VERSION") == 0;
    case VIRTIO_NET_RX_INVALID_GENERATION:
        return strcmp(
            string, "VIRTIO_NET_RX_INVALID_GENERATION") == 0;
    case VIRTIO_NET_RX_ALREADY_PUBLISHED:
        return strcmp(
            string, "VIRTIO_NET_RX_ALREADY_PUBLISHED") == 0;
    case VIRTIO_NET_RX_UNPUBLISHED:
        return strcmp(string, "VIRTIO_NET_RX_UNPUBLISHED") == 0;
    case VIRTIO_NET_RX_INVALID_COMPOSITION:
        return strcmp(
            string, "VIRTIO_NET_RX_INVALID_COMPOSITION") == 0;
    case VIRTIO_NET_RX_INVALID_CONFIG:
        return strcmp(string, "VIRTIO_NET_RX_INVALID_CONFIG") == 0;
    case VIRTIO_NET_RX_INVALID_BUFFER_COUNT:
        return strcmp(
            string, "VIRTIO_NET_RX_INVALID_BUFFER_COUNT") == 0;
    case VIRTIO_NET_RX_QUEUE_FULL:
        return strcmp(string, "VIRTIO_NET_RX_QUEUE_FULL") == 0;
    case VIRTIO_NET_RX_DMA_SHORTAGE:
        return strcmp(string, "VIRTIO_NET_RX_DMA_SHORTAGE") == 0;
    case VIRTIO_NET_RX_INVALID_FRAME_CAPACITY:
        return strcmp(
            string, "VIRTIO_NET_RX_INVALID_FRAME_CAPACITY") == 0;
    case VIRTIO_NET_RX_INVALID_BUFFER:
        return strcmp(string, "VIRTIO_NET_RX_INVALID_BUFFER") == 0;
    case VIRTIO_NET_RX_BUFFER_OVERFLOW:
        return strcmp(
            string, "VIRTIO_NET_RX_BUFFER_OVERFLOW") == 0;
    case VIRTIO_NET_RX_BUFFER_UNALIGNED:
        return strcmp(
            string, "VIRTIO_NET_RX_BUFFER_UNALIGNED") == 0;
    case VIRTIO_NET_RX_BUFFER_OUTSIDE_WINDOW:
        return strcmp(
            string, "VIRTIO_NET_RX_BUFFER_OUTSIDE_WINDOW") == 0;
    case VIRTIO_NET_RX_BUFFER_ALIAS:
        return strcmp(string, "VIRTIO_NET_RX_BUFFER_ALIAS") == 0;
    case VIRTIO_NET_RX_INVALID_DMA:
        return strcmp(string, "VIRTIO_NET_RX_INVALID_DMA") == 0;
    case VIRTIO_NET_RX_DMA_BIND_FAILED:
        return strcmp(
            string, "VIRTIO_NET_RX_DMA_BIND_FAILED") == 0;
    case VIRTIO_NET_RX_DESCRIPTOR_FAILED:
        return strcmp(
            string, "VIRTIO_NET_RX_DESCRIPTOR_FAILED") == 0;
    case VIRTIO_NET_RX_AVAILABLE_FAILED:
        return strcmp(
            string, "VIRTIO_NET_RX_AVAILABLE_FAILED") == 0;
    case VIRTIO_NET_RX_TEARDOWN_FAILED:
        return strcmp(
            string, "VIRTIO_NET_RX_TEARDOWN_FAILED") == 0;
    case VIRTIO_NET_RX_INVALID_STATE:
        return strcmp(string, "VIRTIO_NET_RX_INVALID_STATE") == 0;
    case VIRTIO_NET_RX_INVALID_HEADER:
        return strcmp(string, "VIRTIO_NET_RX_INVALID_HEADER") == 0;
    case VIRTIO_NET_RX_SHORT_FRAME:
        return strcmp(string, "VIRTIO_NET_RX_SHORT_FRAME") == 0;
    case VIRTIO_NET_RX_OVERSIZED_FRAME:
        return strcmp(string, "VIRTIO_NET_RX_OVERSIZED_FRAME") == 0;
    case VIRTIO_NET_RX_STALE_COMPLETION:
        return strcmp(
            string, "VIRTIO_NET_RX_STALE_COMPLETION") == 0;
    case VIRTIO_NET_RX_USED_EMPTY:
        return strcmp(string, "VIRTIO_NET_RX_USED_EMPTY") == 0;
    case VIRTIO_NET_RX_USED_INVALID:
        return strcmp(string, "VIRTIO_NET_RX_USED_INVALID") == 0;
    case VIRTIO_NET_RX_RELEASE_FAILED:
        return strcmp(string, "VIRTIO_NET_RX_RELEASE_FAILED") == 0;
    default:
        return false;
    }
}

static void test_status_strings(void)
{
    static const enum virtio_net_rx_status statuses[] = {
        VIRTIO_NET_RX_OK,
        VIRTIO_NET_RX_NULL,
        VIRTIO_NET_RX_INVALID_VERSION,
        VIRTIO_NET_RX_INVALID_GENERATION,
        VIRTIO_NET_RX_ALREADY_PUBLISHED,
        VIRTIO_NET_RX_UNPUBLISHED,
        VIRTIO_NET_RX_INVALID_COMPOSITION,
        VIRTIO_NET_RX_INVALID_CONFIG,
        VIRTIO_NET_RX_INVALID_BUFFER_COUNT,
        VIRTIO_NET_RX_QUEUE_FULL,
        VIRTIO_NET_RX_DMA_SHORTAGE,
        VIRTIO_NET_RX_INVALID_FRAME_CAPACITY,
        VIRTIO_NET_RX_INVALID_BUFFER,
        VIRTIO_NET_RX_BUFFER_OVERFLOW,
        VIRTIO_NET_RX_BUFFER_UNALIGNED,
        VIRTIO_NET_RX_BUFFER_OUTSIDE_WINDOW,
        VIRTIO_NET_RX_BUFFER_ALIAS,
        VIRTIO_NET_RX_INVALID_DMA,
        VIRTIO_NET_RX_DMA_BIND_FAILED,
        VIRTIO_NET_RX_DESCRIPTOR_FAILED,
        VIRTIO_NET_RX_AVAILABLE_FAILED,
        VIRTIO_NET_RX_TEARDOWN_FAILED,
        VIRTIO_NET_RX_INVALID_STATE,
        VIRTIO_NET_RX_INVALID_HEADER,
        VIRTIO_NET_RX_SHORT_FRAME,
        VIRTIO_NET_RX_OVERSIZED_FRAME,
        VIRTIO_NET_RX_STALE_COMPLETION,
        VIRTIO_NET_RX_USED_EMPTY,
        VIRTIO_NET_RX_USED_INVALID,
        VIRTIO_NET_RX_RELEASE_FAILED,
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        enum virtio_net_rx_status status = statuses[index];
        const char *string = virtio_net_rx_status_string(status);

        CHECK(string != NULL && status_string_matches(status, string),
              "virtio net RX status string is exact");
    }
    CHECK(virtio_net_rx_status_string(
              (enum virtio_net_rx_status)30U) == NULL,
          "virtio net RX undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio net RX status handling allocates nothing");
}

static void test_null_and_composition(void)
{
    struct rx_fixture fixture;
    struct virtio_net_rx rx;
    struct virtio_net_rx_config config;
    struct test_zeroize_state zeroize;
    struct virtio_net_rx_buffer_descriptor buffers[4];

    fill_default_buffers(buffers, 4U);
    memset(&zeroize, 0, sizeof(zeroize));
    zeroize.allow = true;
    config = make_rx_config(buffers, 4U, &zeroize);
    memset(&rx, 0, sizeof(rx));
    activate_composition(&fixture, 4U);

    CHECK(virtio_net_rx_activate(
              NULL, &fixture.composition, &config) ==
              VIRTIO_NET_RX_NULL,
          "RX activate rejects null controller");
    CHECK(virtio_net_rx_activate(
              &rx, NULL, &config) == VIRTIO_NET_RX_NULL,
          "RX activate rejects null composition");
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, NULL) == VIRTIO_NET_RX_NULL,
          "RX activate rejects null config");
    CHECK(!rx.published,
          "RX null rejection leaves controller unpublished");

    memset(&rx, 0, sizeof(rx));
    rx.version = 99U;
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_INVALID_VERSION &&
              !rx.published,
          "RX activate rejects invalid internal version");

    memset(&rx, 0, sizeof(rx));
    fixture.composition.published = false;
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_INVALID_COMPOSITION &&
              !rx.published,
          "RX activate rejects unpublished composition");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "RX null/composition handling allocates nothing");
}

static void test_invalid_config(void)
{
    struct rx_fixture fixture;
    struct virtio_net_rx rx;
    struct virtio_net_rx_config config;
    struct test_zeroize_state zeroize;
    struct virtio_net_rx_buffer_descriptor buffers[4];
    struct virtio_net_rx_buffer_descriptor alias[2];
    uint32_t queue_size = 4U;

    fill_default_buffers(buffers, 4U);
    memset(&zeroize, 0, sizeof(zeroize));
    zeroize.allow = true;
    config = make_rx_config(buffers, 4U, &zeroize);
    activate_composition(&fixture, queue_size);

    memset(&rx, 0, sizeof(rx));
    config.buffers = NULL;
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_INVALID_CONFIG && !rx.published,
          "RX rejects null buffer table without mutation");
    config = make_rx_config(buffers, 4U, &zeroize);

    memset(&rx, 0, sizeof(rx));
    config.buffer_count = 0U;
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_INVALID_BUFFER_COUNT && !rx.published,
          "RX rejects zero buffer count");
    config = make_rx_config(buffers, 4U, &zeroize);

    memset(&rx, 0, sizeof(rx));
    config.buffer_count = VIRTIO_NET_RX_CAPACITY_MAX + 1U;
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_INVALID_BUFFER_COUNT && !rx.published,
          "RX rejects over-limit buffer count");
    config = make_rx_config(buffers, 4U, &zeroize);

    memset(&rx, 0, sizeof(rx));
    config.buffer_count = queue_size + 1U;
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_QUEUE_FULL && !rx.published &&
              virtio_descriptor_ownership_free_count(
                  &fixture.composition.receive.descriptors) == queue_size &&
              virtio_available_publication_published_count(
                  &fixture.composition.receive.available) == 0U &&
              fixture.composition.receive.available.ring->idx == 0U,
          "RX queue-full rejection leaves queue unchanged");
    config = make_rx_config(buffers, 4U, &zeroize);

    memset(&rx, 0, sizeof(rx));
    config.dma_capacity = 3U;
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_DMA_SHORTAGE && !rx.published &&
              virtio_descriptor_ownership_free_count(
                  &fixture.composition.receive.descriptors) == queue_size &&
              virtio_available_publication_published_count(
                  &fixture.composition.receive.available) == 0U &&
              fixture.composition.receive.available.ring->idx == 0U,
          "RX DMA-shortage rejection leaves queue unchanged");
    config = make_rx_config(buffers, 4U, &zeroize);

    memset(&rx, 0, sizeof(rx));
    config.minimum_frame_capacity =
        VIRTIO_NET_RX_MINIMUM_FRAME_CAPACITY_MIN - 1U;
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_INVALID_FRAME_CAPACITY && !rx.published,
          "RX rejects undersized minimum frame capacity");
    config = make_rx_config(buffers, 4U, &zeroize);

    memset(&rx, 0, sizeof(rx));
    config.minimum_frame_capacity =
        VIRTIO_NET_RX_MINIMUM_FRAME_CAPACITY_MAX + 1U;
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_INVALID_FRAME_CAPACITY && !rx.published,
          "RX rejects oversized minimum frame capacity");
    config = make_rx_config(buffers, 4U, &zeroize);

    memset(&rx, 0, sizeof(rx));
    buffers[0].length = config.minimum_frame_capacity - 1U;
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_INVALID_BUFFER && !rx.published,
          "RX rejects buffer below minimum frame capacity");
    fill_default_buffers(buffers, 4U);
    config = make_rx_config(buffers, 4U, &zeroize);

    memset(&rx, 0, sizeof(rx));
    buffers[0].length = VIRTIO_NET_RX_BUFFER_LENGTH_MAX + 1U;
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_INVALID_BUFFER && !rx.published,
          "RX rejects over-limit buffer length");
    fill_default_buffers(buffers, 4U);
    config = make_rx_config(buffers, 4U, &zeroize);

    memset(&rx, 0, sizeof(rx));
    buffers[0].physical_base = UINT64_MAX - 31U;
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_BUFFER_OVERFLOW && !rx.published,
          "RX rejects overflowing buffer range");
    fill_default_buffers(buffers, 4U);
    config = make_rx_config(buffers, 4U, &zeroize);

    memset(&rx, 0, sizeof(rx));
    buffers[0].physical_base += 8U;
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_BUFFER_UNALIGNED && !rx.published,
          "RX rejects unaligned buffer base");
    fill_default_buffers(buffers, 4U);
    config = make_rx_config(buffers, 4U, &zeroize);

    memset(&rx, 0, sizeof(rx));
    buffers[0].physical_base = UINT64_C(0x120000);
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_BUFFER_OUTSIDE_WINDOW && !rx.published,
          "RX rejects buffer outside DMA window");
    fill_default_buffers(buffers, 4U);
    config = make_rx_config(buffers, 4U, &zeroize);

    memset(&rx, 0, sizeof(rx));
    buffers[1] = buffers[0];
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_BUFFER_ALIAS && !rx.published,
          "RX rejects overlapping buffer table");
    fill_default_buffers(buffers, 4U);
    config = make_rx_config(buffers, 4U, &zeroize);

    memset(&rx, 0, sizeof(rx));
    config.dma_window.base = UINT64_C(0x100000);
    config.dma_window.length = UINT64_C(0x2000);
    buffers[0].physical_base = UINT64_C(0x100000);
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_BUFFER_ALIAS && !rx.published,
          "RX rejects buffer overlapping receive queue region");
    fill_default_buffers(buffers, 4U);
    config = make_rx_config(buffers, 4U, &zeroize);

    memset(&rx, 0, sizeof(rx));
    config.dma_window.length = 0U;
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_INVALID_DMA && !rx.published,
          "RX rejects invalid DMA window");
    config = make_rx_config(buffers, 4U, &zeroize);

    memset(&rx, 0, sizeof(rx));
    config.dma_zeroize.zeroize = NULL;
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_INVALID_DMA && !rx.published,
          "RX rejects null zeroize seam");
    config = make_rx_config(buffers, 4U, &zeroize);

    memset(&rx, 0, sizeof(rx));
    config.dma_capacity = 0U;
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_INVALID_DMA && !rx.published,
          "RX rejects invalid DMA capacity");
    config = make_rx_config(buffers, 4U, &zeroize);

    (void)alias;
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "RX invalid-config handling allocates nothing");
}

static void test_success_and_teardown(void)
{
    struct rx_fixture fixture;
    struct virtio_net_rx rx;
    struct virtio_net_rx_config config;
    struct test_zeroize_state zeroize;
    struct virtio_net_rx_buffer_descriptor buffers[4];
    const struct virtio_net_rx_buffer *slot;
    bool seen[4];
    uint32_t index;

    memset(seen, 0, sizeof(seen));
    fill_default_buffers(buffers, 4U);
    memset(&zeroize, 0, sizeof(zeroize));
    zeroize.allow = true;
    config = make_rx_config(buffers, 4U, &zeroize);
    memset(&rx, 0, sizeof(rx));
    activate_composition(&fixture, 4U);

    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_OK,
          "valid RX activation posts all fixed buffers");
    CHECK(virtio_net_rx_published(&rx) &&
              rx.version == VIRTIO_NET_RX_VERSION &&
              rx.generation == 7U &&
              rx.capacity == 4U &&
              rx.posted_count == 4U &&
              rx.queue_size == 4U,
          "published RX controller has exact state");
    CHECK(virtio_net_queue_composition_published(&fixture.composition),
          "RX activation keeps queue composition published");
    CHECK(virtio_descriptor_ownership_free_count(
              &fixture.composition.receive.descriptors) == 0U &&
              virtio_descriptor_ownership_published_count(
                  &fixture.composition.receive.descriptors) == 4U &&
              virtio_available_publication_visible_index(
                  &fixture.composition.receive.available) == 4U &&
              virtio_available_publication_published_count(
                  &fixture.composition.receive.available) == 4U &&
              virtio_dma_region_bound_count(&rx.dma) == 4U,
          "RX activation publishes descriptors, ring entries, and DMA owners");
    for (index = 0U; index < 4U; ++index) {
        slot = virtio_net_rx_buffer_at(&rx, index);
        CHECK(slot != NULL &&
                  slot->posted &&
                  slot->generation == 1U &&
                  slot->physical_base == buffers[index].physical_base &&
                  slot->length == buffers[index].length &&
                  slot->descriptor.index < 4U &&
                  slot->dma.index == index &&
                  fixture.composition.receive.available.ring->ring[index] ==
                      (uint16_t)slot->descriptor.index,
              "RX buffer slot publishes exact descriptor and DMA owner");
        seen[slot->descriptor.index] = true;
    }
    for (index = 0U; index < 4U; ++index) {
        CHECK(seen[index],
              "RX posted descriptor set covers every receive descriptor");
    }
    CHECK(virtio_net_rx_buffer_at(&rx, 4U) == NULL,
          "RX buffer accessor rejects out-of-range index");
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_ALREADY_PUBLISHED,
          "RX duplicate activation is rejected");

    CHECK(virtio_net_rx_teardown(&rx) == VIRTIO_NET_RX_OK &&
              !virtio_net_rx_published(&rx) &&
              virtio_descriptor_ownership_free_count(
                  &fixture.composition.receive.descriptors) == 4U &&
              virtio_descriptor_ownership_published_count(
                  &fixture.composition.receive.descriptors) == 0U &&
              virtio_available_publication_visible_index(
                  &fixture.composition.receive.available) == 0U &&
              virtio_available_publication_published_count(
                  &fixture.composition.receive.available) == 0U &&
              fixture.composition.receive.available.ring->ring[0] == 0U &&
              fixture.composition.receive.available.ring->ring[3] == 0U,
          "RX teardown returns descriptors and resets available ring");
    CHECK(zeroize.calls == 4U,
          "RX teardown zeroizes every payload DMA owner");
    CHECK(virtio_net_rx_teardown(&rx) == VIRTIO_NET_RX_UNPUBLISHED,
          "RX teardown is idempotent for unpublished controller");

    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_OK &&
              virtio_net_rx_posted_count(&rx) == 4U,
          "RX controller permits reactivation after teardown");
    CHECK(virtio_net_rx_teardown(&rx) == VIRTIO_NET_RX_OK,
          "RX reactivated controller tears down cleanly");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "RX success/teardown allocates nothing");
}

static void test_teardown_rejected(void)
{
    struct rx_fixture fixture;
    struct virtio_net_rx rx;
    struct virtio_net_rx_config config;
    struct test_zeroize_state zeroize;
    struct virtio_net_rx_buffer_descriptor buffers[4];

    fill_default_buffers(buffers, 4U);
    memset(&zeroize, 0, sizeof(zeroize));
    zeroize.allow = true;
    config = make_rx_config(buffers, 4U, &zeroize);
    memset(&rx, 0, sizeof(rx));
    activate_composition(&fixture, 4U);
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_OK,
          "RX rejected-zeroize fixture activates");

    zeroize.allow = false;
    CHECK(virtio_net_rx_teardown(&rx) == VIRTIO_NET_RX_TEARDOWN_FAILED &&
              virtio_net_rx_published(&rx) &&
              virtio_descriptor_ownership_published_count(
                  &fixture.composition.receive.descriptors) == 4U &&
              virtio_available_publication_visible_index(
                  &fixture.composition.receive.available) == 4U &&
              virtio_dma_region_bound_count(&rx.dma) == 4U,
          "RX teardown failure leaves controller and queue unchanged");
    zeroize.allow = true;
    CHECK(virtio_net_rx_teardown(&rx) == VIRTIO_NET_RX_OK,
          "RX teardown succeeds after zeroize policy permits release");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "RX rejected teardown allocates nothing");
}

static void test_corrupt_state(void)
{
    struct rx_fixture fixture;
    struct virtio_net_rx rx;
    struct virtio_net_rx_config config;
    struct test_zeroize_state zeroize;
    struct virtio_net_rx_buffer_descriptor buffers[4];

    fill_default_buffers(buffers, 4U);
    memset(&zeroize, 0, sizeof(zeroize));
    zeroize.allow = true;
    config = make_rx_config(buffers, 4U, &zeroize);
    memset(&rx, 0, sizeof(rx));
    activate_composition(&fixture, 4U);
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_OK,
          "RX corruption fixture activates");

    rx.posted_count = 0U;
    CHECK(!virtio_net_rx_published(&rx) &&
              virtio_net_rx_generation(&rx) == 0U &&
              virtio_net_rx_capacity(&rx) == 0U &&
              virtio_net_rx_posted_count(&rx) == 0U &&
              virtio_net_rx_buffer_at(&rx, 0U) == NULL &&
              virtio_net_rx_teardown(&rx) == VIRTIO_NET_RX_INVALID_STATE,
          "RX corruption is detected by published, accessors, and teardown");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "RX corruption handling allocates nothing");
}

static void test_completion_null_and_invalid_head(void)
{
    struct rx_fixture fixture;
    struct virtio_net_rx rx;
    struct virtio_net_rx_config config;
    struct test_zeroize_state zeroize;
    struct virtio_net_rx_buffer_descriptor buffers[4];
    struct virtio_net_rx_frame frame;
    uint8_t payload[128];

    memset(payload, 0, sizeof(payload));
    fill_default_buffers(buffers, 4U);
    memset(&zeroize, 0, sizeof(zeroize));
    zeroize.allow = true;
    config = make_rx_config(buffers, 4U, &zeroize);
    memset(&rx, 0, sizeof(rx));
    activate_composition(&fixture, 4U);
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_OK,
          "RX completion null fixture activates");

    CHECK(virtio_net_rx_complete(NULL, payload, &frame) ==
              VIRTIO_NET_RX_NULL &&
              virtio_net_rx_posted_count(&rx) == 4U,
          "RX completion rejects null controller");
    CHECK(virtio_net_rx_complete(&rx, NULL, &frame) ==
              VIRTIO_NET_RX_NULL &&
              virtio_net_rx_posted_count(&rx) == 4U,
          "RX completion rejects null payload");
    CHECK(virtio_net_rx_complete(&rx, payload, NULL) ==
              VIRTIO_NET_RX_NULL &&
              virtio_net_rx_posted_count(&rx) == 4U,
          "RX completion rejects null frame output");

    fixture.rx_used.idx = 1U;
    fixture.rx_used.ring[0].id = 4U;
    fixture.rx_used.ring[0].len = 76U;
    CHECK(virtio_net_rx_complete(&rx, payload, &frame) ==
              VIRTIO_NET_RX_USED_INVALID &&
              virtio_net_rx_posted_count(&rx) == 4U &&
              virtio_used_consumption_consumed_count(
                  &fixture.composition.receive.used) == 0U &&
              virtio_used_consumption_pending_head(
                  &fixture.composition.receive.used) == 0U,
          "RX completion rejects invalid used head without mutation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "RX completion null/invalid-head handling allocates nothing");
}

static void test_completion_success(void)
{
    struct rx_fixture fixture;
    struct virtio_net_rx rx;
    struct virtio_net_rx_config config;
    struct test_zeroize_state zeroize;
    struct virtio_net_rx_buffer_descriptor buffers[4];
    const struct virtio_net_rx_buffer *first;
    const struct virtio_net_rx_buffer *second;
    struct virtio_net_rx_frame frame;
    uint8_t payload[128];
    uint32_t first_id;
    uint32_t second_id;

    fill_default_buffers(buffers, 4U);
    memset(&zeroize, 0, sizeof(zeroize));
    zeroize.allow = true;
    config = make_rx_config(buffers, 4U, &zeroize);
    memset(&rx, 0, sizeof(rx));
    activate_composition(&fixture, 4U);
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_OK,
          "RX completion success fixture activates");

    first = virtio_net_rx_buffer_at(&rx, 0U);
    second = virtio_net_rx_buffer_at(&rx, 1U);
    first_id = first->descriptor.index;
    second_id = second->descriptor.index;
    memset(payload, 0, sizeof(payload));
    fixture.rx_used.idx = 1U;
    fixture.rx_used.ring[0].id = first_id;
    fixture.rx_used.ring[0].len = 12U + 64U;

    memset(&frame, 0, sizeof(frame));
    CHECK(virtio_net_rx_complete(&rx, payload, &frame) ==
              VIRTIO_NET_RX_OK &&
              frame.data == payload + 12U &&
              frame.physical_base == buffers[0].physical_base &&
              frame.length == 64U &&
              frame.buffer_index == 0U &&
              frame.generation == 7U,
          "RX completion publishes exact valid frame");
    CHECK(virtio_net_rx_posted_count(&rx) == 3U &&
              virtio_descriptor_ownership_free_count(
                  &fixture.composition.receive.descriptors) == 1U &&
              virtio_descriptor_ownership_published_count(
                  &fixture.composition.receive.descriptors) == 3U &&
              virtio_used_consumption_consumed_count(
                  &fixture.composition.receive.used) == 1U &&
              virtio_dma_region_bound_count(&rx.dma) == 4U,
          "RX completion reclaims descriptor once and keeps DMA ownership");
    CHECK(virtio_net_rx_buffer_at(&rx, 0U) == NULL &&
              virtio_net_rx_published(&rx),
          "RX completion removes completed buffer from posted accessors");
    CHECK(virtio_net_rx_complete(&rx, payload, &frame) ==
              VIRTIO_NET_RX_USED_EMPTY &&
              virtio_net_rx_posted_count(&rx) == 3U &&
              virtio_used_consumption_consumed_count(
                  &fixture.composition.receive.used) == 1U,
          "RX repeated used ring without new index is empty and immutable");

    fixture.rx_used.idx = 2U;
    fixture.rx_used.ring[1].id = second_id;
    fixture.rx_used.ring[1].len = 12U + 64U;
    CHECK(virtio_net_rx_complete(&rx, payload, &frame) ==
              VIRTIO_NET_RX_OK &&
              frame.buffer_index == 1U &&
              virtio_net_rx_posted_count(&rx) == 2U &&
              virtio_used_consumption_consumed_count(
                  &fixture.composition.receive.used) == 2U,
          "RX completion accepts second exact frame after first reclaim");
    CHECK(virtio_net_rx_teardown(&rx) == VIRTIO_NET_RX_OK &&
              !virtio_net_rx_published(&rx) &&
              virtio_descriptor_ownership_free_count(
                  &fixture.composition.receive.descriptors) == 4U &&
              virtio_dma_region_bound_count(&rx.dma) == 0U,
          "RX partial completion teardown reclaims remaining descriptors");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "RX completion success/teardown allocates nothing");
}

static void test_completion_dropped(void)
{
    struct rx_fixture fixture;
    struct virtio_net_rx rx;
    struct virtio_net_rx_config config;
    struct test_zeroize_state zeroize;
    struct virtio_net_rx_buffer_descriptor buffers[4];
    const struct virtio_net_rx_buffer *slot;
    struct virtio_net_rx_frame frame;
    uint8_t payload[128];
    uint32_t ids[4];
    uint32_t index;

    fill_default_buffers(buffers, 4U);
    memset(&zeroize, 0, sizeof(zeroize));
    zeroize.allow = true;
    config = make_rx_config(buffers, 4U, &zeroize);
    memset(&rx, 0, sizeof(rx));
    activate_composition(&fixture, 4U);
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_OK,
          "RX dropped-completion fixture activates");
    for (index = 0U; index < 4U; ++index) {
        slot = virtio_net_rx_buffer_at(&rx, index);
        ids[index] = slot->descriptor.index;
    }

    memset(payload, 0, sizeof(payload));
    memset(&frame, 0xAA, sizeof(frame));
    payload[0] = 1U;
    fixture.rx_used.idx = 1U;
    fixture.rx_used.ring[0].id = ids[0];
    fixture.rx_used.ring[0].len = 12U + 64U;
    CHECK(virtio_net_rx_complete(&rx, payload, &frame) ==
              VIRTIO_NET_RX_INVALID_HEADER &&
              frame.data == (void *)UINT64_C(0xAAAAAAAAAAAAAAAA) &&
              virtio_net_rx_posted_count(&rx) == 3U &&
              virtio_used_consumption_consumed_count(
                  &fixture.composition.receive.used) == 1U,
          "RX non-zero virtio header drops without publishing frame");

    memset(payload, 0, sizeof(payload));
    memset(&frame, 0xAA, sizeof(frame));
    fixture.rx_used.idx = 2U;
    fixture.rx_used.ring[1].id = ids[1];
    fixture.rx_used.ring[1].len = 12U + 59U;
    CHECK(virtio_net_rx_complete(&rx, payload, &frame) ==
              VIRTIO_NET_RX_SHORT_FRAME &&
              frame.data == (void *)UINT64_C(0xAAAAAAAAAAAAAAAA) &&
              virtio_net_rx_posted_count(&rx) == 2U,
          "RX short frame drops without publishing frame");

    memset(payload, 0, sizeof(payload));
    memset(&frame, 0xAA, sizeof(frame));
    fixture.rx_used.idx = 3U;
    fixture.rx_used.ring[2].id = ids[2];
    fixture.rx_used.ring[2].len = 129U;
    CHECK(virtio_net_rx_complete(&rx, payload, &frame) ==
              VIRTIO_NET_RX_OVERSIZED_FRAME &&
              frame.data == (void *)UINT64_C(0xAAAAAAAAAAAAAAAA) &&
              virtio_net_rx_posted_count(&rx) == 1U &&
              virtio_used_consumption_consumed_count(
                  &fixture.composition.receive.used) == 3U,
          "RX oversized frame drops without publishing frame");
    CHECK(virtio_descriptor_ownership_free_count(
              &fixture.composition.receive.descriptors) == 3U,
          "RX dropped completions reclaim each descriptor once");
    CHECK(virtio_net_rx_teardown(&rx) == VIRTIO_NET_RX_OK &&
              virtio_dma_region_bound_count(&rx.dma) == 0U,
          "RX dropped-completion teardown unbinds all DMA owners");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "RX dropped-completion handling allocates nothing");
}

static void test_completion_stale(void)
{
    struct rx_fixture fixture;
    struct virtio_net_rx rx;
    struct virtio_net_rx_config config;
    struct test_zeroize_state zeroize;
    struct virtio_net_rx_buffer_descriptor buffers[4];
    const struct virtio_net_rx_buffer *slot;
    struct virtio_net_rx_frame frame;
    uint8_t payload[128];
    uint32_t first_id;
    uint32_t second_id;

    fill_default_buffers(buffers, 4U);
    memset(&zeroize, 0, sizeof(zeroize));
    zeroize.allow = true;
    config = make_rx_config(buffers, 4U, &zeroize);
    memset(&rx, 0, sizeof(rx));
    activate_composition(&fixture, 4U);
    CHECK(virtio_net_rx_activate(
              &rx, &fixture.composition, &config) ==
              VIRTIO_NET_RX_OK,
          "RX stale-completion fixture activates");
    slot = virtio_net_rx_buffer_at(&rx, 0U);
    first_id = slot->descriptor.index;
    slot = virtio_net_rx_buffer_at(&rx, 1U);
    second_id = slot->descriptor.index;

    memset(payload, 0, sizeof(payload));
    fixture.rx_used.idx = 1U;
    fixture.rx_used.ring[0].id = first_id;
    fixture.rx_used.ring[0].len = 12U + 64U;
    CHECK(virtio_net_rx_complete(&rx, payload, &frame) ==
              VIRTIO_NET_RX_OK &&
              virtio_net_rx_posted_count(&rx) == 3U,
          "RX stale fixture consumes first valid completion");

    fixture.rx_used.idx = 2U;
    fixture.rx_used.ring[1].id = first_id;
    fixture.rx_used.ring[1].len = 12U + 64U;
    CHECK(virtio_net_rx_complete(&rx, payload, &frame) ==
              VIRTIO_NET_RX_STALE_COMPLETION &&
              virtio_net_rx_posted_count(&rx) == 3U &&
              virtio_descriptor_ownership_free_count(
                  &fixture.composition.receive.descriptors) == 1U &&
              virtio_used_consumption_consumed_count(
                  &fixture.composition.receive.used) == 1U &&
              virtio_used_consumption_pending_head(
                  &fixture.composition.receive.used) == 0U,
          "RX stale completion is cancelled without reclaim");

    fixture.rx_used.ring[1].id = second_id;
    CHECK(virtio_net_rx_complete(&rx, payload, &frame) ==
              VIRTIO_NET_RX_OK &&
              virtio_net_rx_posted_count(&rx) == 2U &&
              virtio_used_consumption_consumed_count(
                  &fixture.composition.receive.used) == 2U,
          "RX stale cancellation permits next posted completion");
    CHECK(virtio_net_rx_teardown(&rx) == VIRTIO_NET_RX_OK,
          "RX stale fixture tears down cleanly");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "RX stale-completion handling allocates nothing");
}

int main(void)
{
    test_status_strings();
    test_null_and_composition();
    test_invalid_config();
    test_success_and_teardown();
    test_teardown_rejected();
    test_corrupt_state();
    test_completion_null_and_invalid_head();
    test_completion_success();
    test_completion_dropped();
    test_completion_stale();

    if (failures != 0) {
        fprintf(stderr, "Virtio net RX host tests: %d FAILURES\n", failures);
        return 1;
    }
    printf("Virtio net RX host tests: ALL OK\n");
    return 0;
}

#else
#error "HFOC_VIRTIO_NET_RX_HOST_TEST must be defined for host fixture"
#endif
