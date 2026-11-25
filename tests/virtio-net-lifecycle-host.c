#include "virtio_net_lifecycle.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_VIRTIO_NET_LIFECYCLE_HOST_TEST

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

struct test_notification_state {
    unsigned int calls;
    bool result;
};

struct test_link_io_state {
    unsigned int calls;
    bool read_result;
    uint32_t status;
};

struct test_interrupt_io_state {
    unsigned int calls;
    bool read_result;
    uint8_t status;
};

struct test_work_state {
    unsigned int calls;
    bool has_work;
};

struct test_publish_state {
    unsigned int calls;
};

struct test_device_status_state {
    uint8_t register_value;
    bool write_fail;
    bool read_fail;
    bool read_override;
    uint8_t read_override_value;
    unsigned int write_count;
    unsigned int read_count;
    uint8_t last_write;
};

struct lifecycle_fixture {
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
    struct virtio_net_rx rx;
    struct virtio_net_tx tx;
    struct virtio_net_interrupt interrupt;
    struct virtio_net_link_state link;
    struct virtio_pci_device_status_controller device_status;
    struct test_notification_state notifications;
    struct test_link_io_state link_io;
    struct test_interrupt_io_state interrupt_io;
    struct test_work_state receive_work;
    struct test_work_state transmit_work;
    struct test_publish_state publication;
    struct test_device_status_state device;
    struct virtio_net_notification_io notification_seam;
    struct virtio_net_link_io link_io_seam;
    struct virtio_net_interrupt_io interrupt_io_seam;
    struct virtio_net_interrupt_direction receive_seam;
    struct virtio_net_interrupt_direction transmit_seam;
    struct virtio_net_interrupt_publication publication_seam;
    struct virtio_pci_device_status_io device_status_io;
    struct virtio_net_rx_buffer_descriptor rx_buffers[2];
    struct virtio_net_tx_header_descriptor tx_headers[2];
    uint8_t tx_aliases[2][64];
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
    struct test_notification_state *state =
        (struct test_notification_state *)context;

    (void)submission;
    ++state->calls;
    return state->result;
}

static bool test_quiesce(void *context)
{
    struct test_notification_state *state =
        (struct test_notification_state *)context;

    ++state->calls;
    return state->result;
}

static bool test_read_config_status(void *context, uint32_t *status)
{
    struct test_link_io_state *state =
        (struct test_link_io_state *)context;

    ++state->calls;
    if (!state->read_result) {
        return false;
    }
    *status = state->status;
    return true;
}

static bool test_read_interrupt_status(void *context, uint8_t *status)
{
    struct test_interrupt_io_state *state =
        (struct test_interrupt_io_state *)context;

    ++state->calls;
    if (!state->read_result) {
        return false;
    }
    *status = state->status;
    return true;
}

static bool test_has_work(void *context)
{
    struct test_work_state *state = (struct test_work_state *)context;

    ++state->calls;
    return state->has_work;
}

static void test_publish(
    void *context,
    const struct virtio_net_interrupt_work *work)
{
    struct test_publish_state *state =
        (struct test_publish_state *)context;

    (void)work;
    ++state->calls;
}

static bool test_status_write(void *context, uint8_t value)
{
    struct test_device_status_state *state =
        (struct test_device_status_state *)context;

    if (state->write_fail) {
        return false;
    }
    state->register_value = value;
    state->last_write = value;
    ++state->write_count;
    return true;
}

static bool test_status_read(void *context, uint8_t *value)
{
    struct test_device_status_state *state =
        (struct test_device_status_state *)context;

    if (state->read_fail) {
        return false;
    }
    *value = state->read_override ?
                 state->read_override_value : state->register_value;
    ++state->read_count;
    return true;
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
          "lifecycle helper activates valid modern profile");
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

static void activate_composition(struct lifecycle_fixture *fixture)
{
    struct virtio_net_queue_config receive_config;
    struct virtio_net_queue_config transmit_config;

    memset(fixture, 0, sizeof(*fixture));
    fixture->rx_zeroize.allow = true;
    fixture->tx_zeroize.allow = true;
    fixture->notifications.result = true;
    fixture->link_io.read_result = true;
    fixture->link_io.status = VIRTIO_NET_STATUS_LINK_UP;
    fixture->interrupt_io.read_result = true;
    fixture->interrupt_io.status = 0U;
    fixture->receive_work.has_work = true;
    fixture->transmit_work.has_work = false;
    fixture->profile = make_profile();

    receive_config = make_queue_config(
        VIRTIO_NET_QUEUE_RECEIVE,
        VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_INDEX_RX,
        4U,
        UINT64_C(0x100000),
        &fixture->rx_available,
        &fixture->rx_used,
        &fixture->rx_barrier,
        &fixture->rx_reclaim,
        &fixture->rx_zeroize);
    transmit_config = make_queue_config(
        VIRTIO_NET_QUEUE_TRANSMIT,
        VIRTIO_NET_QUEUE_COMPOSITION_QUEUE_INDEX_TX,
        4U,
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
          "lifecycle helper activates receive and transmit queues");
}

static void activate_rx(struct lifecycle_fixture *fixture)
{
    struct virtio_net_rx_config config;
    uint32_t index;

    for (index = 0U; index < 2U; ++index) {
        fixture->rx_buffers[index].physical_base =
            UINT64_C(0x110000) + (uint64_t)index * UINT64_C(0x100);
        fixture->rx_buffers[index].length = 128U;
    }
    memset(&config, 0, sizeof(config));
    config.buffers = fixture->rx_buffers;
    config.buffer_count = 2U;
    config.minimum_frame_capacity = 64U;
    config.dma_window.base = UINT64_C(0x110000);
    config.dma_window.length = UINT64_C(0x1000);
    config.dma_window.alignment = 16U;
    config.dma_zeroize.zeroize = test_zeroize;
    config.dma_zeroize.context = &fixture->rx_zeroize;
    config.dma_capacity = 2U;
    CHECK(virtio_net_rx_activate(
              &fixture->rx, &fixture->composition, &config) ==
              VIRTIO_NET_RX_OK,
          "lifecycle helper activates RX buffers");
}

static void activate_tx(struct lifecycle_fixture *fixture)
{
    struct virtio_net_tx_config config;
    struct virtio_net_tx_notify notify;
    uint32_t index;

    for (index = 0U; index < 2U; ++index) {
        fixture->tx_headers[index].physical_base =
            UINT64_C(0x120000) + (uint64_t)index * UINT64_C(0x100);
        fixture->tx_headers[index].length = 64U;
        fixture->tx_headers[index].alias = fixture->tx_aliases[index];
    }
    memset(&config, 0, sizeof(config));
    config.headers = fixture->tx_headers;
    config.header_count = 2U;
    config.maximum_frame_length = 100U;
    config.dma_window.base = UINT64_C(0x100000);
    config.dma_window.length = UINT64_C(0x40000);
    config.dma_window.alignment = 16U;
    config.dma_zeroize.zeroize = test_zeroize;
    config.dma_zeroize.context = &fixture->tx_zeroize;
    config.dma_capacity = 4U;
    notify.notify = test_notify;
    notify.context = &fixture->notifications;
    CHECK(virtio_net_tx_activate(
              &fixture->tx, &fixture->composition, &config, &notify) ==
              VIRTIO_NET_TX_OK,
          "lifecycle helper activates TX headers");
}

static void activate_interrupt(struct lifecycle_fixture *fixture)
{
    fixture->interrupt_io_seam.read_status = test_read_interrupt_status;
    fixture->interrupt_io_seam.context = &fixture->interrupt_io;
    fixture->receive_seam.has_work = test_has_work;
    fixture->receive_seam.context = &fixture->receive_work;
    fixture->transmit_seam.has_work = test_has_work;
    fixture->transmit_seam.context = &fixture->transmit_work;
    fixture->publication_seam.publish = test_publish;
    fixture->publication_seam.context = &fixture->publication;
    CHECK(virtio_net_interrupt_activate(
              &fixture->interrupt,
              &fixture->interrupt_io_seam,
              &fixture->receive_seam,
              &fixture->transmit_seam,
              &fixture->publication_seam,
              7U) == VIRTIO_NET_INTERRUPT_OK,
          "lifecycle helper activates interrupt controller");
}

static struct network_device_snapshot make_device_snapshot(void)
{
    struct network_device_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = NETWORK_DEVICE_SNAPSHOT_VERSION;
    snapshot.device_id = VIRTIO_NET_LINK_DEVICE_ID;
    snapshot.vendor_id = 0x1AF4U;
    snapshot.product_id = 0x1041U;
    snapshot.bus_address = 0x0010U;
    snapshot.capability_mask = VIRTIO_NET_LINK_CAPABILITY_MASK;
    snapshot.mac_address[0] = 0x52U;
    snapshot.mac_address[1] = 0x54U;
    snapshot.mac_address[2] = 0x00U;
    snapshot.mac_address[3] = 0x12U;
    snapshot.mac_address[4] = 0x34U;
    snapshot.mac_address[5] = 0x56U;
    return snapshot;
}

static void activate_link(struct lifecycle_fixture *fixture)
{
    struct virtio_net_link_readiness readiness;
    struct network_device_snapshot device = make_device_snapshot();
    struct network_link_state_record state;

    memset(&readiness, 0, sizeof(readiness));
    readiness.profile_published = true;
    readiness.queue_composition_published = true;
    readiness.interrupt_published = true;
    memset(&state, 0, sizeof(state));
    state.version = NETWORK_LINK_STATE_VERSION;
    state.device_id = VIRTIO_NET_LINK_DEVICE_ID;
    state.state = NETWORK_LINK_UP;
    state.generation = 5U;
    fixture->link_io_seam.read_config_status = test_read_config_status;
    fixture->link_io_seam.context = &fixture->link_io;
    CHECK(virtio_net_link_state_activate(
              &fixture->link,
              &device,
              &readiness,
              &fixture->link_io_seam,
              &state,
              3U,
              7U) == VIRTIO_NET_LINK_OK,
          "lifecycle helper activates link-state binding");
}

static void activate_device_status(struct lifecycle_fixture *fixture)
{
    fixture->device_status_io.write = test_status_write;
    fixture->device_status_io.read = test_status_read;
    fixture->device_status_io.context = &fixture->device;
    CHECK(virtio_pci_device_status_init(
              &fixture->device_status,
              &fixture->device_status_io,
              7U) == VIRTIO_PCI_DEVICE_STATUS_OK,
          "lifecycle helper initializes device status");
    CHECK(virtio_pci_device_status_acknowledge(
              &fixture->device_status) == VIRTIO_PCI_DEVICE_STATUS_OK &&
          virtio_pci_device_status_driver(
              &fixture->device_status) == VIRTIO_PCI_DEVICE_STATUS_OK &&
          virtio_pci_device_status_features_ok(
              &fixture->device_status) == VIRTIO_PCI_DEVICE_STATUS_OK &&
          virtio_pci_device_status_driver_ok(
              &fixture->device_status) == VIRTIO_PCI_DEVICE_STATUS_OK,
          "lifecycle helper drives device status to DRIVER_OK");
}

static void activate_fixture(struct lifecycle_fixture *fixture)
{
    activate_composition(fixture);
    activate_rx(fixture);
    activate_tx(fixture);
    activate_interrupt(fixture);
    activate_link(fixture);
    activate_device_status(fixture);
}

static void activate_lifecycle(
    struct lifecycle_fixture *fixture,
    struct virtio_net_lifecycle *lifecycle)
{
    fixture->notification_seam.quiesce = test_quiesce;
    fixture->notification_seam.context = &fixture->notifications;
    memset(lifecycle, 0, sizeof(*lifecycle));
    CHECK(virtio_net_lifecycle_activate(
              lifecycle,
              &fixture->link,
              &fixture->interrupt,
              &fixture->rx,
              &fixture->tx,
              &fixture->composition,
              &fixture->device_status,
              &fixture->notification_seam,
              7U) == VIRTIO_NET_LIFECYCLE_OK,
          "lifecycle fixture activates");
}

static bool status_string_matches(
    enum virtio_net_lifecycle_status status,
    const char *string)
{
    switch (status) {
    case VIRTIO_NET_LIFECYCLE_OK:
        return strcmp(string, "VIRTIO_NET_LIFECYCLE_OK") == 0;
    case VIRTIO_NET_LIFECYCLE_NULL:
        return strcmp(string, "VIRTIO_NET_LIFECYCLE_NULL") == 0;
    case VIRTIO_NET_LIFECYCLE_INVALID_VERSION:
        return strcmp(
            string, "VIRTIO_NET_LIFECYCLE_INVALID_VERSION") == 0;
    case VIRTIO_NET_LIFECYCLE_INVALID_GENERATION:
        return strcmp(
            string, "VIRTIO_NET_LIFECYCLE_INVALID_GENERATION") == 0;
    case VIRTIO_NET_LIFECYCLE_ALREADY_PUBLISHED:
        return strcmp(
            string, "VIRTIO_NET_LIFECYCLE_ALREADY_PUBLISHED") == 0;
    case VIRTIO_NET_LIFECYCLE_UNPUBLISHED:
        return strcmp(
            string, "VIRTIO_NET_LIFECYCLE_UNPUBLISHED") == 0;
    case VIRTIO_NET_LIFECYCLE_INVALID_NOTIFICATIONS:
        return strcmp(
            string, "VIRTIO_NET_LIFECYCLE_INVALID_NOTIFICATIONS") == 0;
    case VIRTIO_NET_LIFECYCLE_INVALID_LINK_STATE:
        return strcmp(
            string, "VIRTIO_NET_LIFECYCLE_INVALID_LINK_STATE") == 0;
    case VIRTIO_NET_LIFECYCLE_INVALID_INTERRUPT:
        return strcmp(
            string, "VIRTIO_NET_LIFECYCLE_INVALID_INTERRUPT") == 0;
    case VIRTIO_NET_LIFECYCLE_INVALID_RX:
        return strcmp(
            string, "VIRTIO_NET_LIFECYCLE_INVALID_RX") == 0;
    case VIRTIO_NET_LIFECYCLE_INVALID_TX:
        return strcmp(
            string, "VIRTIO_NET_LIFECYCLE_INVALID_TX") == 0;
    case VIRTIO_NET_LIFECYCLE_INVALID_COMPOSITION:
        return strcmp(
            string, "VIRTIO_NET_LIFECYCLE_INVALID_COMPOSITION") == 0;
    case VIRTIO_NET_LIFECYCLE_INVALID_DEVICE_STATUS:
        return strcmp(
            string, "VIRTIO_NET_LIFECYCLE_INVALID_DEVICE_STATUS") == 0;
    case VIRTIO_NET_LIFECYCLE_DEVICE_NOT_LIVE:
        return strcmp(
            string, "VIRTIO_NET_LIFECYCLE_DEVICE_NOT_LIVE") == 0;
    case VIRTIO_NET_LIFECYCLE_GENERATION_MISMATCH:
        return strcmp(
            string, "VIRTIO_NET_LIFECYCLE_GENERATION_MISMATCH") == 0;
    case VIRTIO_NET_LIFECYCLE_NOTIFICATIONS_FAILED:
        return strcmp(
            string, "VIRTIO_NET_LIFECYCLE_NOTIFICATIONS_FAILED") == 0;
    case VIRTIO_NET_LIFECYCLE_ADMISSION_STOP_FAILED:
        return strcmp(
            string, "VIRTIO_NET_LIFECYCLE_ADMISSION_STOP_FAILED") == 0;
    case VIRTIO_NET_LIFECYCLE_RX_TEARDOWN_FAILED:
        return strcmp(
            string, "VIRTIO_NET_LIFECYCLE_RX_TEARDOWN_FAILED") == 0;
    case VIRTIO_NET_LIFECYCLE_TX_TEARDOWN_FAILED:
        return strcmp(
            string, "VIRTIO_NET_LIFECYCLE_TX_TEARDOWN_FAILED") == 0;
    case VIRTIO_NET_LIFECYCLE_DEVICE_RESET_FAILED:
        return strcmp(
            string, "VIRTIO_NET_LIFECYCLE_DEVICE_RESET_FAILED") == 0;
    case VIRTIO_NET_LIFECYCLE_QUEUE_TEARDOWN_FAILED:
        return strcmp(
            string, "VIRTIO_NET_LIFECYCLE_QUEUE_TEARDOWN_FAILED") == 0;
    case VIRTIO_NET_LIFECYCLE_INVALID_STATE:
        return strcmp(
            string, "VIRTIO_NET_LIFECYCLE_INVALID_STATE") == 0;
    default:
        return false;
    }
}

static void test_status_strings(void)
{
    static const enum virtio_net_lifecycle_status statuses[] = {
        VIRTIO_NET_LIFECYCLE_OK,
        VIRTIO_NET_LIFECYCLE_NULL,
        VIRTIO_NET_LIFECYCLE_INVALID_VERSION,
        VIRTIO_NET_LIFECYCLE_INVALID_GENERATION,
        VIRTIO_NET_LIFECYCLE_ALREADY_PUBLISHED,
        VIRTIO_NET_LIFECYCLE_UNPUBLISHED,
        VIRTIO_NET_LIFECYCLE_INVALID_NOTIFICATIONS,
        VIRTIO_NET_LIFECYCLE_INVALID_LINK_STATE,
        VIRTIO_NET_LIFECYCLE_INVALID_INTERRUPT,
        VIRTIO_NET_LIFECYCLE_INVALID_RX,
        VIRTIO_NET_LIFECYCLE_INVALID_TX,
        VIRTIO_NET_LIFECYCLE_INVALID_COMPOSITION,
        VIRTIO_NET_LIFECYCLE_INVALID_DEVICE_STATUS,
        VIRTIO_NET_LIFECYCLE_DEVICE_NOT_LIVE,
        VIRTIO_NET_LIFECYCLE_GENERATION_MISMATCH,
        VIRTIO_NET_LIFECYCLE_NOTIFICATIONS_FAILED,
        VIRTIO_NET_LIFECYCLE_ADMISSION_STOP_FAILED,
        VIRTIO_NET_LIFECYCLE_RX_TEARDOWN_FAILED,
        VIRTIO_NET_LIFECYCLE_TX_TEARDOWN_FAILED,
        VIRTIO_NET_LIFECYCLE_DEVICE_RESET_FAILED,
        VIRTIO_NET_LIFECYCLE_QUEUE_TEARDOWN_FAILED,
        VIRTIO_NET_LIFECYCLE_INVALID_STATE,
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        enum virtio_net_lifecycle_status status = statuses[index];
        const char *string = virtio_net_lifecycle_status_string(status);

        CHECK(string != NULL && status_string_matches(status, string),
              "virtio net lifecycle status string is exact");
    }
    CHECK(virtio_net_lifecycle_status_string(
              (enum virtio_net_lifecycle_status)999) == NULL,
          "virtio net lifecycle undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio net lifecycle status handling allocates nothing");
}

static void test_activation_rejections(void)
{
    struct lifecycle_fixture fixture;
    struct virtio_net_lifecycle lifecycle;
    struct virtio_net_notification_io notifications;

    activate_fixture(&fixture);
    notifications.quiesce = test_quiesce;
    notifications.context = &fixture.notifications;
    memset(&lifecycle, 0, sizeof(lifecycle));

    CHECK(virtio_net_lifecycle_activate(
              NULL,
              &fixture.link,
              &fixture.interrupt,
              &fixture.rx,
              &fixture.tx,
              &fixture.composition,
              &fixture.device_status,
              &notifications,
              7U) == VIRTIO_NET_LIFECYCLE_NULL,
          "lifecycle activation rejects NULL controller");
    CHECK(virtio_net_lifecycle_activate(
              &lifecycle,
              NULL,
              &fixture.interrupt,
              &fixture.rx,
              &fixture.tx,
              &fixture.composition,
              &fixture.device_status,
              &notifications,
              7U) == VIRTIO_NET_LIFECYCLE_NULL,
          "lifecycle activation rejects NULL link state");
    CHECK(virtio_net_lifecycle_activate(
              &lifecycle,
              &fixture.link,
              NULL,
              &fixture.rx,
              &fixture.tx,
              &fixture.composition,
              &fixture.device_status,
              &notifications,
              7U) == VIRTIO_NET_LIFECYCLE_NULL,
          "lifecycle activation rejects NULL interrupt");
    CHECK(virtio_net_lifecycle_activate(
              &lifecycle,
              &fixture.link,
              &fixture.interrupt,
              NULL,
              &fixture.tx,
              &fixture.composition,
              &fixture.device_status,
              &notifications,
              7U) == VIRTIO_NET_LIFECYCLE_NULL,
          "lifecycle activation rejects NULL RX");
    CHECK(virtio_net_lifecycle_activate(
              &lifecycle,
              &fixture.link,
              &fixture.interrupt,
              &fixture.rx,
              NULL,
              &fixture.composition,
              &fixture.device_status,
              &notifications,
              7U) == VIRTIO_NET_LIFECYCLE_NULL,
          "lifecycle activation rejects NULL TX");
    CHECK(virtio_net_lifecycle_activate(
              &lifecycle,
              &fixture.link,
              &fixture.interrupt,
              &fixture.rx,
              &fixture.tx,
              NULL,
              &fixture.device_status,
              &notifications,
              7U) == VIRTIO_NET_LIFECYCLE_NULL,
          "lifecycle activation rejects NULL composition");
    CHECK(virtio_net_lifecycle_activate(
              &lifecycle,
              &fixture.link,
              &fixture.interrupt,
              &fixture.rx,
              &fixture.tx,
              &fixture.composition,
              NULL,
              &notifications,
              7U) == VIRTIO_NET_LIFECYCLE_NULL,
          "lifecycle activation rejects NULL device status");
    CHECK(virtio_net_lifecycle_activate(
              &lifecycle,
              &fixture.link,
              &fixture.interrupt,
              &fixture.rx,
              &fixture.tx,
              &fixture.composition,
              &fixture.device_status,
              NULL,
              7U) == VIRTIO_NET_LIFECYCLE_NULL,
          "lifecycle activation rejects NULL notifications");

    memset(&lifecycle, 0, sizeof(lifecycle));
    lifecycle.version = VIRTIO_NET_LIFECYCLE_VERSION + 1U;
    CHECK(virtio_net_lifecycle_activate(
              &lifecycle,
              &fixture.link,
              &fixture.interrupt,
              &fixture.rx,
              &fixture.tx,
              &fixture.composition,
              &fixture.device_status,
              &notifications,
              7U) == VIRTIO_NET_LIFECYCLE_INVALID_VERSION &&
              !lifecycle.published,
          "lifecycle activation rejects unknown internal version");

    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(virtio_net_lifecycle_activate(
              &lifecycle,
              &fixture.link,
              &fixture.interrupt,
              &fixture.rx,
              &fixture.tx,
              &fixture.composition,
              &fixture.device_status,
              &notifications,
              0U) == VIRTIO_NET_LIFECYCLE_INVALID_GENERATION &&
              !lifecycle.published,
          "lifecycle activation rejects zero generation");

    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(virtio_net_lifecycle_activate(
              &lifecycle,
              &fixture.link,
              &fixture.interrupt,
              &fixture.rx,
              &fixture.tx,
              &fixture.composition,
              &fixture.device_status,
              &notifications,
              VIRTIO_NET_LIFECYCLE_GENERATION_MAX + 1U) ==
              VIRTIO_NET_LIFECYCLE_INVALID_GENERATION &&
              !lifecycle.published,
          "lifecycle activation rejects overflow generation");

    memset(&lifecycle, 0, sizeof(lifecycle));
    notifications.quiesce = NULL;
    CHECK(virtio_net_lifecycle_activate(
              &lifecycle,
              &fixture.link,
              &fixture.interrupt,
              &fixture.rx,
              &fixture.tx,
              &fixture.composition,
              &fixture.device_status,
              &notifications,
              7U) == VIRTIO_NET_LIFECYCLE_INVALID_NOTIFICATIONS &&
              !lifecycle.published,
          "lifecycle activation rejects missing quiesce seam");
    notifications.quiesce = test_quiesce;
    notifications.context = NULL;
    CHECK(virtio_net_lifecycle_activate(
              &lifecycle,
              &fixture.link,
              &fixture.interrupt,
              &fixture.rx,
              &fixture.tx,
              &fixture.composition,
              &fixture.device_status,
              &notifications,
              7U) == VIRTIO_NET_LIFECYCLE_INVALID_NOTIFICATIONS &&
              !lifecycle.published,
          "lifecycle activation rejects NULL notification context");
    notifications.context = &fixture.notifications;

    fixture.link.published = false;
    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(virtio_net_lifecycle_activate(
              &lifecycle,
              &fixture.link,
              &fixture.interrupt,
              &fixture.rx,
              &fixture.tx,
              &fixture.composition,
              &fixture.device_status,
              &notifications,
              7U) == VIRTIO_NET_LIFECYCLE_INVALID_LINK_STATE &&
              !lifecycle.published,
          "lifecycle activation rejects unpublished link state");
    fixture.link.published = true;

    fixture.interrupt.published = false;
    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(virtio_net_lifecycle_activate(
              &lifecycle,
              &fixture.link,
              &fixture.interrupt,
              &fixture.rx,
              &fixture.tx,
              &fixture.composition,
              &fixture.device_status,
              &notifications,
              7U) == VIRTIO_NET_LIFECYCLE_INVALID_INTERRUPT &&
              !lifecycle.published,
          "lifecycle activation rejects unpublished interrupt");
    fixture.interrupt.published = true;

    fixture.rx.published = false;
    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(virtio_net_lifecycle_activate(
              &lifecycle,
              &fixture.link,
              &fixture.interrupt,
              &fixture.rx,
              &fixture.tx,
              &fixture.composition,
              &fixture.device_status,
              &notifications,
              7U) == VIRTIO_NET_LIFECYCLE_INVALID_RX &&
              !lifecycle.published,
          "lifecycle activation rejects unpublished RX");
    fixture.rx.published = true;

    fixture.tx.published = false;
    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(virtio_net_lifecycle_activate(
              &lifecycle,
              &fixture.link,
              &fixture.interrupt,
              &fixture.rx,
              &fixture.tx,
              &fixture.composition,
              &fixture.device_status,
              &notifications,
              7U) == VIRTIO_NET_LIFECYCLE_INVALID_TX &&
              !lifecycle.published,
          "lifecycle activation rejects unpublished TX");
    fixture.tx.published = true;

    fixture.composition.published = false;
    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(virtio_net_lifecycle_activate(
              &lifecycle,
              &fixture.link,
              &fixture.interrupt,
              &fixture.rx,
              &fixture.tx,
              &fixture.composition,
              &fixture.device_status,
              &notifications,
              7U) == VIRTIO_NET_LIFECYCLE_INVALID_COMPOSITION &&
              !lifecycle.published,
          "lifecycle activation rejects unpublished composition");
    fixture.composition.published = true;

    fixture.device_status.published = false;
    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(virtio_net_lifecycle_activate(
              &lifecycle,
              &fixture.link,
              &fixture.interrupt,
              &fixture.rx,
              &fixture.tx,
              &fixture.composition,
              &fixture.device_status,
              &notifications,
              7U) == VIRTIO_NET_LIFECYCLE_INVALID_DEVICE_STATUS &&
              !lifecycle.published,
          "lifecycle activation rejects unpublished device status");
    fixture.device_status.published = true;

    CHECK(virtio_pci_device_status_reset(
              &fixture.device_status) == VIRTIO_PCI_DEVICE_STATUS_OK,
          "lifecycle non-live fixture resets device status");
    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(virtio_net_lifecycle_activate(
              &lifecycle,
              &fixture.link,
              &fixture.interrupt,
              &fixture.rx,
              &fixture.tx,
              &fixture.composition,
              &fixture.device_status,
              &notifications,
              7U) == VIRTIO_NET_LIFECYCLE_DEVICE_NOT_LIVE &&
              !lifecycle.published,
          "lifecycle activation rejects non-live device status");

    activate_fixture(&fixture);
    fixture.rx.generation = 8U;
    memset(&lifecycle, 0, sizeof(lifecycle));
    CHECK(virtio_net_lifecycle_activate(
              &lifecycle,
              &fixture.link,
              &fixture.interrupt,
              &fixture.rx,
              &fixture.tx,
              &fixture.composition,
              &fixture.device_status,
              &notifications,
              7U) == VIRTIO_NET_LIFECYCLE_GENERATION_MISMATCH &&
              !lifecycle.published,
          "lifecycle activation rejects generation mismatch");

    activate_fixture(&fixture);
    activate_lifecycle(&fixture, &lifecycle);
    CHECK(virtio_net_lifecycle_activate(
              &lifecycle,
              &fixture.link,
              &fixture.interrupt,
              &fixture.rx,
              &fixture.tx,
              &fixture.composition,
              &fixture.device_status,
              &notifications,
              7U) == VIRTIO_NET_LIFECYCLE_ALREADY_PUBLISHED,
          "lifecycle activation rejects duplicate publication");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "lifecycle activation rejections allocate nothing");
}

static void test_successful_stop_and_idempotence(void)
{
    struct lifecycle_fixture fixture;
    struct virtio_net_lifecycle lifecycle;

    activate_fixture(&fixture);
    activate_lifecycle(&fixture, &lifecycle);
    fixture.notifications.calls = 0U;

    CHECK(virtio_net_lifecycle_stop(NULL) ==
              VIRTIO_NET_LIFECYCLE_NULL,
          "lifecycle stop rejects NULL controller");
    CHECK(virtio_net_lifecycle_stop(&lifecycle) ==
              VIRTIO_NET_LIFECYCLE_OK &&
              fixture.notifications.calls == 1U &&
              virtio_net_lifecycle_stopped(&lifecycle) &&
              virtio_net_lifecycle_published(&lifecycle) &&
              virtio_net_lifecycle_generation(&lifecycle) == 7U,
          "lifecycle stop quiesces notifications and publishes STOPPED");
    CHECK(virtio_net_link_state_selection(&fixture.link) != NULL &&
              virtio_net_link_state_selection(
                  &fixture.link)->link.state == NETWORK_LINK_FAILED &&
              !virtio_net_link_state_path_enabled(&fixture.link),
          "lifecycle stop closes link admission before teardown");
    CHECK(!virtio_net_rx_published(&fixture.rx) &&
              !virtio_net_tx_published(&fixture.tx) &&
              !virtio_net_queue_composition_published(&fixture.composition),
          "lifecycle stop reclaims RX, TX, and queue composition");
    CHECK(fixture.device_status.state == VIRTIO_PCI_DEVICE_STATE_RESET &&
              fixture.device_status.status_bits == 0U &&
              fixture.device_status.observed_bits == 0U,
          "lifecycle stop resets device status with readback");
    CHECK(lifecycle.link_state == NULL &&
              lifecycle.interrupt == NULL &&
              lifecycle.rx == NULL &&
              lifecycle.tx == NULL &&
              lifecycle.composition == NULL &&
              lifecycle.device_status == NULL,
          "lifecycle stop clears stale controller references");

    fixture.notifications.calls = 0U;
    CHECK(virtio_net_lifecycle_stop(&lifecycle) ==
              VIRTIO_NET_LIFECYCLE_OK &&
              fixture.notifications.calls == 0U &&
              virtio_net_lifecycle_stopped(&lifecycle),
          "repeated lifecycle stop is idempotent");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "lifecycle successful stop allocates nothing");
}

static void test_failed_quiesce_unchanged(void)
{
    struct lifecycle_fixture fixture;
    struct virtio_net_lifecycle lifecycle;

    activate_fixture(&fixture);
    activate_lifecycle(&fixture, &lifecycle);
    fixture.notifications.calls = 0U;
    fixture.notifications.result = false;

    CHECK(virtio_net_lifecycle_stop(&lifecycle) ==
              VIRTIO_NET_LIFECYCLE_NOTIFICATIONS_FAILED &&
              fixture.notifications.calls == 1U &&
              !virtio_net_lifecycle_stopped(&lifecycle) &&
              virtio_net_link_state_path_enabled(&fixture.link) &&
              virtio_net_rx_published(&fixture.rx) &&
              virtio_net_tx_published(&fixture.tx) &&
              virtio_net_queue_composition_published(&fixture.composition) &&
              virtio_pci_device_status_live(&fixture.device_status),
          "failed notification quiesce leaves all controllers unchanged");

    fixture.notifications.result = true;
    CHECK(virtio_net_lifecycle_stop(&lifecycle) ==
              VIRTIO_NET_LIFECYCLE_OK &&
              virtio_net_lifecycle_stopped(&lifecycle),
          "lifecycle retries stop after quiesce recovery");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "lifecycle quiesce failure allocates nothing");
}

static void test_admission_failure_unchanged(void)
{
    struct lifecycle_fixture fixture;
    struct virtio_net_lifecycle lifecycle;

    activate_fixture(&fixture);
    activate_lifecycle(&fixture, &lifecycle);
    fixture.notifications.calls = 0U;
    fixture.link_io.read_result = false;

    CHECK(virtio_net_lifecycle_stop(&lifecycle) ==
              VIRTIO_NET_LIFECYCLE_ADMISSION_STOP_FAILED &&
              fixture.notifications.calls == 1U &&
              !virtio_net_lifecycle_stopped(&lifecycle) &&
              virtio_net_link_state_path_enabled(&fixture.link) &&
              virtio_net_rx_published(&fixture.rx) &&
              virtio_net_tx_published(&fixture.tx) &&
              virtio_net_queue_composition_published(&fixture.composition) &&
              virtio_pci_device_status_live(&fixture.device_status),
          "failed admission stop leaves RX/TX/queues and device unchanged");

    fixture.link_io.read_result = true;
    CHECK(virtio_net_lifecycle_stop(&lifecycle) ==
              VIRTIO_NET_LIFECYCLE_OK &&
              virtio_net_lifecycle_stopped(&lifecycle),
          "lifecycle retries stop after admission recovery");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "lifecycle admission failure allocates nothing");
}

static void test_rx_teardown_failure_explicit(void)
{
    struct lifecycle_fixture fixture;
    struct virtio_net_lifecycle lifecycle;

    activate_fixture(&fixture);
    activate_lifecycle(&fixture, &lifecycle);
    fixture.notifications.calls = 0U;
    fixture.rx_zeroize.allow = false;

    CHECK(virtio_net_lifecycle_stop(&lifecycle) ==
              VIRTIO_NET_LIFECYCLE_RX_TEARDOWN_FAILED &&
              fixture.notifications.calls == 1U &&
              !virtio_net_lifecycle_stopped(&lifecycle) &&
              virtio_net_link_state_selection(&fixture.link)->link.state ==
                  NETWORK_LINK_FAILED &&
              virtio_net_rx_published(&fixture.rx) &&
              virtio_net_tx_published(&fixture.tx) &&
              virtio_net_queue_composition_published(&fixture.composition),
          "RX teardown failure is explicit and does not mark stopped");

    fixture.rx_zeroize.allow = true;
    CHECK(virtio_net_lifecycle_stop(&lifecycle) ==
              VIRTIO_NET_LIFECYCLE_OK &&
              virtio_net_lifecycle_stopped(&lifecycle) &&
              !virtio_net_rx_published(&fixture.rx) &&
              !virtio_net_tx_published(&fixture.tx) &&
              !virtio_net_queue_composition_published(&fixture.composition),
          "lifecycle retries RX teardown after release policy recovery");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "lifecycle RX teardown failure allocates nothing");
}

int main(void)
{
    test_status_strings();
    test_activation_rejections();
    test_successful_stop_and_idempotence();
    test_failed_quiesce_unchanged();
    test_admission_failure_unchanged();
    test_rx_teardown_failure_explicit();

    if (failures != 0) {
        fprintf(stderr, "Virtio net reset and shutdown host tests: FAIL\n");
        return 1;
    }
    printf("Virtio net reset and shutdown host tests: ALL OK\n");
    return 0;
}

#else
#error "HFOC_VIRTIO_NET_LIFECYCLE_HOST_TEST must be defined for host fixture"
#endif
