#include "virtio_net_tx.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_VIRTIO_NET_TX_HOST_TEST

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

struct test_notify_state {
    unsigned int calls;
    bool result;
};

struct tx_fixture {
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

static bool test_notify(
    void *context,
    const struct virtio_net_tx_submission *submission)
{
    struct test_notify_state *state =
        (struct test_notify_state *)context;

    (void)submission;
    ++state->calls;
    return state->result;
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
    struct tx_fixture *fixture,
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
          "host TX composition helper activates receive and transmit queues");
}

static void fill_default_headers(
    struct virtio_net_tx_header_descriptor *headers,
    uint8_t aliases[][64],
    uint32_t count)
{
    uint32_t index;

    for (index = 0U; index < count; ++index) {
        headers[index].physical_base =
            UINT64_C(0x110000) + (uint64_t)index * UINT64_C(0x100);
        headers[index].length = 64U;
        headers[index].alias = aliases[index];
    }
}

static struct virtio_net_tx_config make_tx_config(
    const struct virtio_net_tx_header_descriptor *headers,
    uint32_t header_count,
    uint32_t maximum_frame_length,
    struct test_zeroize_state *zeroize)
{
    struct virtio_net_tx_config config;

    memset(&config, 0, sizeof(config));
    config.headers = headers;
    config.header_count = header_count;
    config.maximum_frame_length = maximum_frame_length;
    config.dma_window.base = UINT64_C(0x100000);
    config.dma_window.length = UINT64_C(0x40000);
    config.dma_window.alignment = 16U;
    config.dma_zeroize.zeroize = test_zeroize;
    config.dma_zeroize.context = zeroize;
    config.dma_capacity = header_count * 2U;
    return config;
}

static bool status_string_matches(
    enum virtio_net_tx_status status,
    const char *string)
{
    const char *expected = NULL;

    switch (status) {
    case VIRTIO_NET_TX_OK:
        expected = "VIRTIO_NET_TX_OK";
        break;
    case VIRTIO_NET_TX_NULL:
        expected = "VIRTIO_NET_TX_NULL";
        break;
    case VIRTIO_NET_TX_INVALID_VERSION:
        expected = "VIRTIO_NET_TX_INVALID_VERSION";
        break;
    case VIRTIO_NET_TX_INVALID_GENERATION:
        expected = "VIRTIO_NET_TX_INVALID_GENERATION";
        break;
    case VIRTIO_NET_TX_ALREADY_PUBLISHED:
        expected = "VIRTIO_NET_TX_ALREADY_PUBLISHED";
        break;
    case VIRTIO_NET_TX_UNPUBLISHED:
        expected = "VIRTIO_NET_TX_UNPUBLISHED";
        break;
    case VIRTIO_NET_TX_INVALID_COMPOSITION:
        expected = "VIRTIO_NET_TX_INVALID_COMPOSITION";
        break;
    case VIRTIO_NET_TX_INVALID_CONFIG:
        expected = "VIRTIO_NET_TX_INVALID_CONFIG";
        break;
    case VIRTIO_NET_TX_INVALID_HEADER_COUNT:
        expected = "VIRTIO_NET_TX_INVALID_HEADER_COUNT";
        break;
    case VIRTIO_NET_TX_QUEUE_FULL:
        expected = "VIRTIO_NET_TX_QUEUE_FULL";
        break;
    case VIRTIO_NET_TX_DMA_SHORTAGE:
        expected = "VIRTIO_NET_TX_DMA_SHORTAGE";
        break;
    case VIRTIO_NET_TX_INVALID_MAX_FRAME_LENGTH:
        expected = "VIRTIO_NET_TX_INVALID_MAX_FRAME_LENGTH";
        break;
    case VIRTIO_NET_TX_INVALID_HEADER:
        expected = "VIRTIO_NET_TX_INVALID_HEADER";
        break;
    case VIRTIO_NET_TX_HEADER_OVERFLOW:
        expected = "VIRTIO_NET_TX_HEADER_OVERFLOW";
        break;
    case VIRTIO_NET_TX_HEADER_UNALIGNED:
        expected = "VIRTIO_NET_TX_HEADER_UNALIGNED";
        break;
    case VIRTIO_NET_TX_HEADER_OUTSIDE_WINDOW:
        expected = "VIRTIO_NET_TX_HEADER_OUTSIDE_WINDOW";
        break;
    case VIRTIO_NET_TX_HEADER_ALIAS:
        expected = "VIRTIO_NET_TX_HEADER_ALIAS";
        break;
    case VIRTIO_NET_TX_INVALID_DMA:
        expected = "VIRTIO_NET_TX_INVALID_DMA";
        break;
    case VIRTIO_NET_TX_DESCRIPTOR_FAILED:
        expected = "VIRTIO_NET_TX_DESCRIPTOR_FAILED";
        break;
    case VIRTIO_NET_TX_AVAILABLE_FAILED:
        expected = "VIRTIO_NET_TX_AVAILABLE_FAILED";
        break;
    case VIRTIO_NET_TX_NOTIFY_FAILED:
        expected = "VIRTIO_NET_TX_NOTIFY_FAILED";
        break;
    case VIRTIO_NET_TX_INVALID_FRAME:
        expected = "VIRTIO_NET_TX_INVALID_FRAME";
        break;
    case VIRTIO_NET_TX_TEARDOWN_FAILED:
        expected = "VIRTIO_NET_TX_TEARDOWN_FAILED";
        break;
    case VIRTIO_NET_TX_INVALID_STATE:
        expected = "VIRTIO_NET_TX_INVALID_STATE";
        break;
    case VIRTIO_NET_TX_USED_EMPTY:
        expected = "VIRTIO_NET_TX_USED_EMPTY";
        break;
    case VIRTIO_NET_TX_INVALID_HEAD:
        expected = "VIRTIO_NET_TX_INVALID_HEAD";
        break;
    case VIRTIO_NET_TX_INVALID_LENGTH:
        expected = "VIRTIO_NET_TX_INVALID_LENGTH";
        break;
    case VIRTIO_NET_TX_STALE_COMPLETION:
        expected = "VIRTIO_NET_TX_STALE_COMPLETION";
        break;
    case VIRTIO_NET_TX_RELEASE_FAILED:
        expected = "VIRTIO_NET_TX_RELEASE_FAILED";
        break;
    case VIRTIO_NET_TX_USED_INVALID:
        expected = "VIRTIO_NET_TX_USED_INVALID";
        break;
    }
    return string != NULL && expected != NULL && strcmp(string, expected) == 0;
}

static void test_status_strings(void)
{
    static const enum virtio_net_tx_status statuses[] = {
        VIRTIO_NET_TX_OK,
        VIRTIO_NET_TX_NULL,
        VIRTIO_NET_TX_INVALID_VERSION,
        VIRTIO_NET_TX_INVALID_GENERATION,
        VIRTIO_NET_TX_ALREADY_PUBLISHED,
        VIRTIO_NET_TX_UNPUBLISHED,
        VIRTIO_NET_TX_INVALID_COMPOSITION,
        VIRTIO_NET_TX_INVALID_CONFIG,
        VIRTIO_NET_TX_INVALID_HEADER_COUNT,
        VIRTIO_NET_TX_QUEUE_FULL,
        VIRTIO_NET_TX_DMA_SHORTAGE,
        VIRTIO_NET_TX_INVALID_MAX_FRAME_LENGTH,
        VIRTIO_NET_TX_INVALID_HEADER,
        VIRTIO_NET_TX_HEADER_OVERFLOW,
        VIRTIO_NET_TX_HEADER_UNALIGNED,
        VIRTIO_NET_TX_HEADER_OUTSIDE_WINDOW,
        VIRTIO_NET_TX_HEADER_ALIAS,
        VIRTIO_NET_TX_INVALID_DMA,
        VIRTIO_NET_TX_DESCRIPTOR_FAILED,
        VIRTIO_NET_TX_AVAILABLE_FAILED,
        VIRTIO_NET_TX_NOTIFY_FAILED,
        VIRTIO_NET_TX_INVALID_FRAME,
        VIRTIO_NET_TX_TEARDOWN_FAILED,
        VIRTIO_NET_TX_INVALID_STATE,
        VIRTIO_NET_TX_USED_EMPTY,
        VIRTIO_NET_TX_INVALID_HEAD,
        VIRTIO_NET_TX_INVALID_LENGTH,
        VIRTIO_NET_TX_STALE_COMPLETION,
        VIRTIO_NET_TX_RELEASE_FAILED,
        VIRTIO_NET_TX_USED_INVALID,
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        enum virtio_net_tx_status status = statuses[index];
        const char *string = virtio_net_tx_status_string(status);

        CHECK(string != NULL && status_string_matches(status, string),
              "virtio net TX status string is exact");
    }
    CHECK(virtio_net_tx_status_string(
              (enum virtio_net_tx_status)30U) == NULL,
          "virtio net TX undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio net TX status handling allocates nothing");
}

static void test_activate_success_and_invalid(void)
{
    struct tx_fixture fixture;
    struct virtio_net_tx tx;
    struct virtio_net_tx_config config;
    struct virtio_net_tx_header_descriptor headers[2];
    uint8_t aliases[2][64];
    struct test_zeroize_state zeroize;
    struct test_notify_state notify_state;
    struct virtio_net_tx_notify notify;

    fill_default_headers(headers, aliases, 2U);
    memset(&zeroize, 0, sizeof(zeroize));
    zeroize.allow = true;
    notify_state.calls = 0U;
    notify_state.result = true;
    notify.notify = test_notify;
    notify.context = &notify_state;
    config = make_tx_config(headers, 2U, 100U, &zeroize);
    memset(&tx, 0, sizeof(tx));
    activate_composition(&fixture, 4U);

    CHECK(virtio_net_tx_activate(
              NULL, &fixture.composition, &config, &notify) ==
              VIRTIO_NET_TX_NULL,
          "TX activate rejects null controller");
    CHECK(virtio_net_tx_activate(
              &tx, NULL, &config, &notify) == VIRTIO_NET_TX_NULL,
          "TX activate rejects null composition");
    CHECK(virtio_net_tx_activate(
              &tx, &fixture.composition, NULL, &notify) ==
              VIRTIO_NET_TX_NULL,
          "TX activate rejects null config");
    CHECK(virtio_net_tx_activate(
              &tx, &fixture.composition, &config, NULL) ==
              VIRTIO_NET_TX_NULL,
          "TX activate rejects null notify");
    CHECK(!tx.published,
          "TX null rejection leaves controller unpublished");

    config.headers = NULL;
    CHECK(virtio_net_tx_activate(
              &tx, &fixture.composition, &config, &notify) ==
              VIRTIO_NET_TX_INVALID_CONFIG && !tx.published,
          "TX rejects null header table");
    config = make_tx_config(headers, 2U, 100U, &zeroize);

    config.header_count = 0U;
    CHECK(virtio_net_tx_activate(
              &tx, &fixture.composition, &config, &notify) ==
              VIRTIO_NET_TX_INVALID_HEADER_COUNT && !tx.published,
          "TX rejects zero header count");
    config = make_tx_config(headers, 2U, 100U, &zeroize);

    config.header_count = VIRTIO_NET_TX_CAPACITY_MAX + 1U;
    CHECK(virtio_net_tx_activate(
              &tx, &fixture.composition, &config, &notify) ==
              VIRTIO_NET_TX_INVALID_HEADER_COUNT && !tx.published,
          "TX rejects over-limit header count");
    config = make_tx_config(headers, 2U, 100U, &zeroize);

    config.maximum_frame_length = VIRTIO_NET_TX_FRAME_LENGTH_MIN - 1U;
    CHECK(virtio_net_tx_activate(
              &tx, &fixture.composition, &config, &notify) ==
              VIRTIO_NET_TX_INVALID_MAX_FRAME_LENGTH && !tx.published,
          "TX rejects undersized maximum frame length");
    config = make_tx_config(headers, 2U, 100U, &zeroize);

    config.maximum_frame_length = VIRTIO_NET_TX_FRAME_LENGTH_MAX + 1U;
    CHECK(virtio_net_tx_activate(
              &tx, &fixture.composition, &config, &notify) ==
              VIRTIO_NET_TX_INVALID_MAX_FRAME_LENGTH && !tx.published,
          "TX rejects oversized maximum frame length");
    config = make_tx_config(headers, 2U, 100U, &zeroize);

    headers[0].length = 1U;
    CHECK(virtio_net_tx_activate(
              &tx, &fixture.composition, &config, &notify) ==
              VIRTIO_NET_TX_INVALID_HEADER && !tx.published,
          "TX rejects header shorter than virtio header");
    fill_default_headers(headers, aliases, 2U);
    config = make_tx_config(headers, 2U, 100U, &zeroize);

    headers[0].alias = NULL;
    CHECK(virtio_net_tx_activate(
              &tx, &fixture.composition, &config, &notify) ==
              VIRTIO_NET_TX_INVALID_HEADER && !tx.published,
          "TX rejects header with null alias");
    fill_default_headers(headers, aliases, 2U);
    config = make_tx_config(headers, 2U, 100U, &zeroize);

    config.dma_capacity = 3U;
    CHECK(virtio_net_tx_activate(
              &tx, &fixture.composition, &config, &notify) ==
              VIRTIO_NET_TX_DMA_SHORTAGE && !tx.published,
          "TX rejects DMA shortage");
    config = make_tx_config(headers, 2U, 100U, &zeroize);

    config.header_count = 3U;
    CHECK(virtio_net_tx_activate(
              &tx, &fixture.composition, &config, &notify) ==
              VIRTIO_NET_TX_QUEUE_FULL && !tx.published,
          "TX rejects header count exceeding half queue");

    memset(&tx, 0, sizeof(tx));
    config = make_tx_config(headers, 2U, 100U, &zeroize);
    CHECK(virtio_net_tx_activate(
              &tx, &fixture.composition, &config, &notify) ==
              VIRTIO_NET_TX_OK,
          "valid TX activation publishes fixed header capacity");
    CHECK(virtio_net_tx_published(&tx) &&
              tx.version == VIRTIO_NET_TX_VERSION &&
              tx.generation == 7U &&
              tx.capacity == 2U &&
              tx.submitted_count == 0U &&
              virtio_descriptor_ownership_free_count(
                  &fixture.composition.transmit.descriptors) == 4U &&
              virtio_available_publication_visible_index(
                  &fixture.composition.transmit.available) == 0U &&
              virtio_dma_region_bound_count(&tx.dma) == 0U,
          "TX activation leaves transmit queue and DMA registry clean");
    CHECK(virtio_net_tx_activate(
              &tx, &fixture.composition, &config, &notify) ==
              VIRTIO_NET_TX_ALREADY_PUBLISHED,
          "TX duplicate activation is rejected");
    CHECK(virtio_net_tx_teardown(&tx) == VIRTIO_NET_TX_OK &&
              !virtio_net_tx_published(&tx) &&
              virtio_descriptor_ownership_free_count(
                  &fixture.composition.transmit.descriptors) == 4U,
          "TX clean activation tears down without queue mutation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "TX activation handling allocates nothing");
}

static void test_submit_success_exhaustion_and_teardown(void)
{
    struct tx_fixture fixture;
    struct virtio_net_tx tx;
    struct virtio_net_tx_config config;
    struct virtio_net_tx_header_descriptor headers[2];
    uint8_t aliases[2][64];
    uint8_t payload[128];
    uint8_t second_payload[128];
    struct test_zeroize_state zeroize;
    struct test_notify_state notify_state;
    struct virtio_net_tx_notify notify;
    struct virtio_net_tx_frame frame;
    struct virtio_net_tx_submission first;
    struct virtio_net_tx_submission second;
    uint32_t first_header;
    uint32_t first_frame;
    uint32_t second_header;
    uint32_t second_frame;
    uint32_t index;

    fill_default_headers(headers, aliases, 2U);
    memset(&zeroize, 0, sizeof(zeroize));
    zeroize.allow = true;
    notify_state.calls = 0U;
    notify_state.result = true;
    notify.notify = test_notify;
    notify.context = &notify_state;
    config = make_tx_config(headers, 2U, 100U, &zeroize);
    memset(&tx, 0, sizeof(tx));
    activate_composition(&fixture, 4U);
    CHECK(virtio_net_tx_activate(
              &tx, &fixture.composition, &config, &notify) ==
              VIRTIO_NET_TX_OK,
          "TX submit fixture activates");

    memset(aliases, 0xAA, sizeof(aliases));
    memset(payload, 0x5A, sizeof(payload));
    frame.data = payload;
    frame.physical_base = UINT64_C(0x120000);
    frame.length = 64U;
    CHECK(virtio_net_tx_submit(&tx, &frame, &first) ==
              VIRTIO_NET_TX_OK &&
              first.slot_index == 0U &&
              first.generation == 7U &&
              notify_state.calls == 1U,
          "TX submit publishes exact first frame");
    first_header = first.header_descriptor_index;
    first_frame = first.frame_descriptor_index;
    CHECK(first_header < 4U &&
              first_frame < 4U &&
              first_header != first_frame &&
              aliases[0][0] == 0U &&
              aliases[0][11] == 0U &&
              virtio_descriptor_ownership_free_count(
                  &fixture.composition.transmit.descriptors) == 2U &&
              virtio_available_publication_visible_index(
                  &fixture.composition.transmit.available) == 1U &&
              fixture.composition.transmit.available.ring->ring[0] ==
                  (uint16_t)first_header &&
              fixture.composition.transmit.available.ring->ring[1] ==
                  0U &&
              virtio_dma_region_bound_count(&tx.dma) == 2U &&
              tx.submitted_count == 1U &&
              tx.slots[0].active,
          "TX success publishes chain head and device-read DMA owners");
    for (index = 0U; index < 12U; ++index) {
        CHECK(aliases[0][index] == 0U,
              "TX header alias is fully zeroed to header size");
    }

    memset(second_payload, 0x6B, sizeof(second_payload));
    frame.data = second_payload;
    frame.physical_base = UINT64_C(0x120100);
    frame.length = 80U;
    CHECK(virtio_net_tx_submit(&tx, &frame, &second) ==
              VIRTIO_NET_TX_OK &&
              second.slot_index == 1U &&
              second.generation == 7U &&
              notify_state.calls == 2U,
          "TX submit publishes exact second frame");
    second_header = second.header_descriptor_index;
    second_frame = second.frame_descriptor_index;
    CHECK(second_header < 4U &&
              second_frame < 4U &&
              virtio_descriptor_ownership_free_count(
                  &fixture.composition.transmit.descriptors) == 0U &&
              virtio_available_publication_visible_index(
                  &fixture.composition.transmit.available) == 2U &&
              fixture.composition.transmit.available.ring->ring[1] ==
                  (uint16_t)second_header &&
              fixture.composition.transmit.available.ring->ring[2] ==
                  0U &&
              virtio_dma_region_bound_count(&tx.dma) == 4U &&
              tx.submitted_count == 2U,
          "TX second submission exhausts two-descriptor capacity");

    memset(&frame, 0, sizeof(frame));
    frame.data = payload;
    frame.physical_base = UINT64_C(0x120200);
    frame.length = 64U;
    CHECK(virtio_net_tx_submit(&tx, &frame, &first) ==
              VIRTIO_NET_TX_QUEUE_FULL &&
              tx.submitted_count == 2U &&
              virtio_descriptor_ownership_free_count(
                  &fixture.composition.transmit.descriptors) == 0U &&
              virtio_available_publication_visible_index(
                  &fixture.composition.transmit.available) == 2U &&
              virtio_dma_region_bound_count(&tx.dma) == 4U &&
              notify_state.calls == 2U,
          "TX queue exhaustion is caller-visible and mutation-free");

    CHECK(virtio_net_tx_teardown(&tx) == VIRTIO_NET_TX_OK &&
              !virtio_net_tx_published(&tx) &&
              virtio_descriptor_ownership_free_count(
                  &fixture.composition.transmit.descriptors) == 4U &&
              virtio_descriptor_ownership_published_count(
                  &fixture.composition.transmit.descriptors) == 0U &&
              virtio_available_publication_visible_index(
                  &fixture.composition.transmit.available) == 0U &&
              virtio_dma_region_bound_count(&tx.dma) == 0U,
          "TX partial teardown returns descriptors and unbinds all DMA owners");
    CHECK(zeroize.calls == 4U,
          "TX teardown zeroizes every header and frame DMA owner");
    CHECK(virtio_net_tx_teardown(&tx) == VIRTIO_NET_TX_UNPUBLISHED,
          "TX teardown is idempotent for unpublished controller");
    CHECK(virtio_net_tx_activate(
              &tx, &fixture.composition, &config, &notify) ==
              VIRTIO_NET_TX_OK &&
              virtio_net_tx_submitted_count(&tx) == 0U,
          "TX controller permits reactivation after teardown");
    CHECK(virtio_net_tx_teardown(&tx) == VIRTIO_NET_TX_OK,
          "TX reactivated controller tears down cleanly");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "TX success/exhaustion/teardown allocates nothing");
}

static void test_submit_rejections_and_notify_rollback(void)
{
    struct tx_fixture fixture;
    struct virtio_net_tx tx;
    struct virtio_net_tx_config config;
    struct virtio_net_tx_header_descriptor headers[2];
    uint8_t aliases[2][64];
    uint8_t payload[128];
    struct test_zeroize_state zeroize;
    struct test_notify_state notify_state;
    struct virtio_net_tx_notify notify;
    struct virtio_net_tx_frame frame;
    struct virtio_net_tx_submission submission;

    fill_default_headers(headers, aliases, 2U);
    memset(&zeroize, 0, sizeof(zeroize));
    zeroize.allow = true;
    notify_state.calls = 0U;
    notify_state.result = true;
    notify.notify = test_notify;
    notify.context = &notify_state;
    config = make_tx_config(headers, 2U, 100U, &zeroize);
    memset(&tx, 0, sizeof(tx));
    activate_composition(&fixture, 4U);
    CHECK(virtio_net_tx_activate(
              &tx, &fixture.composition, &config, &notify) ==
              VIRTIO_NET_TX_OK,
          "TX rejection fixture activates");

    CHECK(virtio_net_tx_submit(NULL, &frame, &submission) ==
              VIRTIO_NET_TX_NULL,
          "TX submit rejects null controller");
    CHECK(virtio_net_tx_submit(&tx, NULL, &submission) ==
              VIRTIO_NET_TX_NULL,
          "TX submit rejects null frame");
    CHECK(virtio_net_tx_submit(&tx, &frame, NULL) ==
              VIRTIO_NET_TX_NULL,
          "TX submit rejects null submission output");

    memset(payload, 0x5A, sizeof(payload));
    memset(&frame, 0, sizeof(frame));
    frame.data = payload;
    frame.physical_base = UINT64_C(0x120000);
    frame.length = 0U;
    CHECK(virtio_net_tx_submit(&tx, &frame, &submission) ==
              VIRTIO_NET_TX_INVALID_FRAME &&
              tx.submitted_count == 0U,
          "TX submit rejects zero-length frame");
    frame.length = VIRTIO_NET_TX_FRAME_LENGTH_MIN - 1U;
    CHECK(virtio_net_tx_submit(&tx, &frame, &submission) ==
              VIRTIO_NET_TX_INVALID_FRAME &&
              tx.submitted_count == 0U,
          "TX submit rejects undersized Ethernet frame");
    frame.length = 101U;
    CHECK(virtio_net_tx_submit(&tx, &frame, &submission) ==
              VIRTIO_NET_TX_INVALID_FRAME &&
              tx.submitted_count == 0U,
          "TX submit rejects frame above configured maximum");
    frame.length = VIRTIO_NET_TX_FRAME_LENGTH_MAX + 1U;
    CHECK(virtio_net_tx_submit(&tx, &frame, &submission) ==
              VIRTIO_NET_TX_INVALID_FRAME &&
              tx.submitted_count == 0U,
          "TX submit rejects frame above protocol maximum");
    frame.length = 64U;
    frame.data = NULL;
    CHECK(virtio_net_tx_submit(&tx, &frame, &submission) ==
              VIRTIO_NET_TX_INVALID_FRAME &&
              tx.submitted_count == 0U,
          "TX submit rejects null frame data");
    frame.data = payload;
    frame.physical_base = UINT64_C(0x120000) + 8U;
    CHECK(virtio_net_tx_submit(&tx, &frame, &submission) ==
              VIRTIO_NET_TX_INVALID_FRAME &&
              tx.submitted_count == 0U,
          "TX submit rejects unaligned frame physical base");

    frame.physical_base = UINT64_C(0x120000);
    notify_state.result = false;
    CHECK(virtio_net_tx_submit(&tx, &frame, &submission) ==
              VIRTIO_NET_TX_NOTIFY_FAILED &&
              notify_state.calls == 1U &&
              tx.submitted_count == 0U &&
              virtio_descriptor_ownership_free_count(
                  &fixture.composition.transmit.descriptors) == 4U &&
              virtio_available_publication_visible_index(
                  &fixture.composition.transmit.available) == 0U &&
              virtio_available_publication_pending_length(
                  &fixture.composition.transmit.available) == 0U &&
              virtio_dma_region_bound_count(&tx.dma) == 0U &&
              !tx.slots[0].active,
          "TX notify failure rolls back chain and DMA without double ownership");
    CHECK(virtio_net_tx_submit(&tx, &frame, &submission) ==
              VIRTIO_NET_TX_NOTIFY_FAILED &&
              virtio_descriptor_ownership_free_count(
                  &fixture.composition.transmit.descriptors) == 4U &&
              virtio_dma_region_bound_count(&tx.dma) == 0U,
          "TX repeated notify failure remains mutation-free");
    notify_state.result = true;
    CHECK(virtio_net_tx_submit(&tx, &frame, &submission) ==
              VIRTIO_NET_TX_OK &&
              tx.submitted_count == 1U,
          "TX recovers after notify failure");
    CHECK(virtio_net_tx_teardown(&tx) == VIRTIO_NET_TX_OK,
          "TX rejection fixture tears down cleanly");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "TX rejection/notify handling allocates nothing");
}

static void setup_reclaim_fixture(
    struct tx_fixture *fixture,
    struct virtio_net_tx *tx,
    struct virtio_net_tx_config *config,
    struct virtio_net_tx_header_descriptor *headers,
    uint8_t aliases[][64],
    struct test_zeroize_state *zeroize,
    struct test_notify_state *notify_state,
    struct virtio_net_tx_notify *notify)
{
    fill_default_headers(headers, aliases, 2U);
    memset(zeroize, 0, sizeof(*zeroize));
    zeroize->allow = true;
    notify_state->calls = 0U;
    notify_state->result = true;
    notify->notify = test_notify;
    notify->context = notify_state;
    *config = make_tx_config(headers, 2U, 100U, zeroize);
    memset(tx, 0, sizeof(*tx));
    activate_composition(fixture, 4U);
    CHECK(virtio_net_tx_activate(
              tx, &fixture->composition, config, notify) ==
              VIRTIO_NET_TX_OK,
          "TX reclaim fixture activates");
}

static void test_reclaim_success_reuse_and_wraparound(void)
{
    struct tx_fixture fixture;
    struct virtio_net_tx tx;
    struct virtio_net_tx_config config;
    struct virtio_net_tx_header_descriptor headers[2];
    uint8_t aliases[2][64];
    uint8_t payload[128];
    uint8_t second_payload[128];
    struct test_zeroize_state zeroize;
    struct test_notify_state notify_state;
    struct virtio_net_tx_notify notify;
    struct virtio_net_tx_frame frame;
    struct virtio_net_tx_submission first;
    struct virtio_net_tx_submission second;
    struct virtio_net_tx_completion completion;

    setup_reclaim_fixture(
        &fixture,
        &tx,
        &config,
        headers,
        aliases,
        &zeroize,
        &notify_state,
        &notify);

    memset(payload, 0x5A, sizeof(payload));
    memset(&frame, 0, sizeof(frame));
    frame.data = payload;
    frame.physical_base = UINT64_C(0x120000);
    frame.length = 64U;
    CHECK(virtio_net_tx_submit(&tx, &frame, &first) ==
              VIRTIO_NET_TX_OK,
          "TX reclaim fixture submits first frame");

    fixture.tx_used.idx = 1U;
    fixture.tx_used.ring[0].id = first.header_descriptor_index;
    fixture.tx_used.ring[0].len = 0U;
    memset(&completion, 0xAA, sizeof(completion));
    CHECK(virtio_net_tx_reclaim(&tx, &completion) ==
              VIRTIO_NET_TX_OK &&
              completion.data == payload &&
              completion.physical_base == UINT64_C(0x120000) &&
              completion.length == 64U &&
              completion.slot_index == 0U &&
              completion.generation == 7U,
          "TX reclaim returns exact first frame ownership");
    CHECK(tx.submitted_count == 0U &&
              tx.reclaimed_count == 1U &&
              virtio_descriptor_ownership_free_count(
                  &fixture.composition.transmit.descriptors) == 4U &&
              virtio_available_publication_visible_index(
                  &fixture.composition.transmit.available) == 1U &&
              virtio_dma_region_bound_count(&tx.dma) == 0U &&
              virtio_used_consumption_consumed_count(
                  &fixture.composition.transmit.used) == 1U &&
              !tx.slots[0].active,
          "TX reclaim consumes descriptors and DMA owners exactly once");
    CHECK(zeroize.calls == 2U,
          "TX reclaim zeroizes header and frame DMA owners");

    memset(second_payload, 0x6B, sizeof(second_payload));
    memset(&frame, 0, sizeof(frame));
    frame.data = second_payload;
    frame.physical_base = UINT64_C(0x120100);
    frame.length = 64U;
    CHECK(virtio_net_tx_submit(&tx, &frame, &second) ==
              VIRTIO_NET_TX_OK &&
              second.slot_index == 0U &&
              tx.slots[0].frame_data == second_payload &&
              tx.slots[0].frame_physical_base == UINT64_C(0x120100) &&
              tx.slots[0].frame_length == 64U,
          "TX slot reuse publishes a new frame after reclaim");

    fixture.composition.transmit.used.expected_idx = UINT16_MAX;
    fixture.tx_used.idx = 0U;
    fixture.tx_used.ring[3].id = second.header_descriptor_index;
    fixture.tx_used.ring[3].len = 0U;
    memset(&completion, 0xAA, sizeof(completion));
    CHECK(virtio_net_tx_reclaim(&tx, &completion) ==
              VIRTIO_NET_TX_OK &&
              completion.data == second_payload &&
              completion.physical_base == UINT64_C(0x120100) &&
              completion.length == 64U &&
              completion.slot_index == 0U &&
              virtio_used_consumption_consumed_count(
                  &fixture.composition.transmit.used) == 2U &&
              virtio_dma_region_bound_count(&tx.dma) == 0U,
          "TX reclaim consumes a used ring entry across wraparound");
    CHECK(virtio_net_tx_teardown(&tx) == VIRTIO_NET_TX_OK &&
              zeroize.calls == 4U,
          "TX reclaim wraparound fixture tears down cleanly");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "TX reclaim success/wraparound allocates nothing");
}

static void test_reclaim_empty_and_invalid_evidence(void)
{
    struct tx_fixture fixture;
    struct virtio_net_tx tx;
    struct virtio_net_tx_config config;
    struct virtio_net_tx_header_descriptor headers[2];
    uint8_t aliases[2][64];
    uint8_t payload[128];
    struct test_zeroize_state zeroize;
    struct test_notify_state notify_state;
    struct virtio_net_tx_notify notify;
    struct virtio_net_tx_frame frame;
    struct virtio_net_tx_submission submission;
    struct virtio_net_tx_completion completion;

    setup_reclaim_fixture(
        &fixture,
        &tx,
        &config,
        headers,
        aliases,
        &zeroize,
        &notify_state,
        &notify);

    memset(payload, 0x5A, sizeof(payload));
    memset(&frame, 0, sizeof(frame));
    frame.data = payload;
    frame.physical_base = UINT64_C(0x120000);
    frame.length = 64U;
    CHECK(virtio_net_tx_submit(&tx, &frame, &submission) ==
              VIRTIO_NET_TX_OK,
          "TX reclaim evidence fixture submits one frame");

    CHECK(virtio_net_tx_reclaim(NULL, &completion) ==
              VIRTIO_NET_TX_NULL,
          "TX reclaim rejects null controller");
    CHECK(virtio_net_tx_reclaim(&tx, NULL) ==
              VIRTIO_NET_TX_NULL,
          "TX reclaim rejects null completion output");
    CHECK(virtio_net_tx_reclaim(&tx, &completion) ==
              VIRTIO_NET_TX_USED_EMPTY &&
              tx.submitted_count == 1U &&
              virtio_used_consumption_consumed_count(
                  &fixture.composition.transmit.used) == 0U,
          "TX reclaim empty used ring is immutable");

    fixture.tx_used.idx = 1U;
    fixture.tx_used.ring[0].id = 4U;
    fixture.tx_used.ring[0].len = 0U;
    CHECK(virtio_net_tx_reclaim(&tx, &completion) ==
              VIRTIO_NET_TX_INVALID_HEAD &&
              virtio_net_tx_invalid_head_count(&tx) == 1U &&
              tx.submitted_count == 1U &&
              virtio_used_consumption_consumed_count(
                  &fixture.composition.transmit.used) == 0U &&
              virtio_dma_region_bound_count(&tx.dma) == 2U,
          "TX reclaim records invalid used head without mutation");

    fixture.tx_used.ring[0].id = submission.header_descriptor_index;
    fixture.tx_used.ring[0].len = 1U;
    CHECK(virtio_net_tx_reclaim(&tx, &completion) ==
              VIRTIO_NET_TX_INVALID_LENGTH &&
              virtio_net_tx_invalid_length_count(&tx) == 1U &&
              tx.submitted_count == 1U &&
              virtio_used_consumption_consumed_count(
                  &fixture.composition.transmit.used) == 1U &&
              virtio_dma_region_bound_count(&tx.dma) == 2U &&
              tx.slots[0].active,
          "TX reclaim consumes invalid length evidence without reclaim");

    fixture.tx_used.idx = 2U;
    fixture.tx_used.ring[1].id =
        (uint16_t)((submission.header_descriptor_index + 2U) % 4U);
    fixture.tx_used.ring[1].len = 0U;
    CHECK(virtio_net_tx_reclaim(&tx, &completion) ==
              VIRTIO_NET_TX_STALE_COMPLETION &&
              virtio_net_tx_stale_completion_count(&tx) == 1U &&
              tx.submitted_count == 1U &&
              virtio_used_consumption_consumed_count(
                  &fixture.composition.transmit.used) == 2U &&
              virtio_dma_region_bound_count(&tx.dma) == 2U &&
              tx.slots[0].active,
          "TX stale completion cannot reclaim a live slot");

    fixture.tx_used.idx = 3U;
    fixture.tx_used.ring[2].id = submission.header_descriptor_index;
    fixture.tx_used.ring[2].len = 0U;
    CHECK(virtio_net_tx_reclaim(&tx, &completion) ==
              VIRTIO_NET_TX_OK &&
              completion.data == payload &&
              virtio_net_tx_reclaimed_count(&tx) == 1U &&
              virtio_used_consumption_consumed_count(
                  &fixture.composition.transmit.used) == 3U &&
              virtio_dma_region_bound_count(&tx.dma) == 0U &&
              !tx.slots[0].active,
          "TX reclaim recovers after invalid evidence");
    CHECK(virtio_net_tx_teardown(&tx) == VIRTIO_NET_TX_OK,
          "TX reclaim evidence fixture tears down cleanly");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "TX reclaim invalid-evidence handling allocates nothing");
}

static void test_reclaim_release_failure_transactional(void)
{
    struct tx_fixture fixture;
    struct virtio_net_tx tx;
    struct virtio_net_tx_config config;
    struct virtio_net_tx_header_descriptor headers[2];
    uint8_t aliases[2][64];
    uint8_t payload[128];
    struct test_zeroize_state zeroize;
    struct test_notify_state notify_state;
    struct virtio_net_tx_notify notify;
    struct virtio_net_tx_frame frame;
    struct virtio_net_tx_submission submission;
    struct virtio_net_tx_completion completion;

    setup_reclaim_fixture(
        &fixture,
        &tx,
        &config,
        headers,
        aliases,
        &zeroize,
        &notify_state,
        &notify);

    memset(payload, 0x5A, sizeof(payload));
    memset(&frame, 0, sizeof(frame));
    frame.data = payload;
    frame.physical_base = UINT64_C(0x120000);
    frame.length = 64U;
    CHECK(virtio_net_tx_submit(&tx, &frame, &submission) ==
              VIRTIO_NET_TX_OK,
          "TX reclaim failure fixture submits one frame");

    fixture.tx_used.idx = 1U;
    fixture.tx_used.ring[0].id = submission.header_descriptor_index;
    fixture.tx_used.ring[0].len = 0U;
    zeroize.allow = false;
    CHECK(virtio_net_tx_reclaim(&tx, &completion) ==
              VIRTIO_NET_TX_RELEASE_FAILED &&
              tx.submitted_count == 1U &&
              virtio_used_consumption_consumed_count(
                  &fixture.composition.transmit.used) == 0U &&
              virtio_descriptor_ownership_free_count(
                  &fixture.composition.transmit.descriptors) == 2U &&
              virtio_dma_region_bound_count(&tx.dma) == 2U &&
              tx.slots[0].active,
          "TX reclaim release failure leaves all ownership unchanged");

    zeroize.allow = true;
    CHECK(virtio_net_tx_reclaim(&tx, &completion) ==
              VIRTIO_NET_TX_OK &&
              completion.data == payload &&
              tx.submitted_count == 0U &&
              virtio_used_consumption_consumed_count(
                  &fixture.composition.transmit.used) == 1U &&
              virtio_dma_region_bound_count(&tx.dma) == 0U,
          "TX reclaim retries after release policy permits zeroization");
    CHECK(virtio_net_tx_teardown(&tx) == VIRTIO_NET_TX_OK,
          "TX reclaim failure fixture tears down cleanly");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "TX reclaim release failure allocates nothing");
}

int main(void)
{
    test_status_strings();
    test_activate_success_and_invalid();
    test_submit_success_exhaustion_and_teardown();
    test_submit_rejections_and_notify_rollback();
    test_reclaim_success_reuse_and_wraparound();
    test_reclaim_empty_and_invalid_evidence();
    test_reclaim_release_failure_transactional();

    if (failures != 0) {
        fprintf(stderr, "Virtio net TX host tests: %d FAILURES\n", failures);
        return 1;
    }
    printf("Virtio net TX host tests: ALL OK\n");
    return 0;
}

#else
#error "HFOC_VIRTIO_NET_TX_HOST_TEST must be defined for host fixture"
#endif
