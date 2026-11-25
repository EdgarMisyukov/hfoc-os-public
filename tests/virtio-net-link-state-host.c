#include "virtio_net_link_state.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_VIRTIO_NET_LINK_STATE_HOST_TEST

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

struct test_io_state {
    bool read_result;
    uint32_t status;
    unsigned int calls;
};

static bool test_read_config_status(void *context, uint32_t *status)
{
    struct test_io_state *state = (struct test_io_state *)context;

    ++state->calls;
    if (!state->read_result) {
        return false;
    }
    *status = state->status;
    return true;
}

static struct network_device_snapshot make_device_snapshot(
    uint32_t device_id,
    uint32_t capability_mask)
{
    struct network_device_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = NETWORK_DEVICE_SNAPSHOT_VERSION;
    snapshot.device_id = device_id;
    snapshot.vendor_id = 0x1AF4U;
    snapshot.product_id = 0x1041U;
    snapshot.bus_address = 0x0010U;
    snapshot.capability_mask = capability_mask;
    snapshot.mac_address[0] = 0x52U;
    snapshot.mac_address[1] = 0x54U;
    snapshot.mac_address[2] = 0x00U;
    snapshot.mac_address[3] = 0x12U;
    snapshot.mac_address[4] = 0x34U;
    snapshot.mac_address[5] = 0x56U;
    return snapshot;
}

static struct network_link_state_record make_link_state(
    uint32_t device_id,
    enum network_link_state state,
    uint32_t generation)
{
    struct network_link_state_record record;

    memset(&record, 0, sizeof(record));
    record.version = NETWORK_LINK_STATE_VERSION;
    record.device_id = device_id;
    record.state = state;
    record.generation = generation;
    return record;
}

static struct virtio_net_link_readiness make_readiness(bool ready)
{
    struct virtio_net_link_readiness readiness;

    memset(&readiness, 0, sizeof(readiness));
    readiness.profile_published = ready;
    readiness.queue_composition_published = ready;
    readiness.interrupt_published = ready;
    return readiness;
}

static void activate_ready(
    struct virtio_net_link_state *binding,
    struct test_io_state *io,
    uint32_t status)
{
    struct virtio_net_link_readiness readiness = make_readiness(true);
    struct virtio_net_link_io io_seam;
    struct network_device_snapshot device =
        make_device_snapshot(VIRTIO_NET_LINK_DEVICE_ID,
                             VIRTIO_NET_LINK_CAPABILITY_MASK);
    struct network_link_state_record state =
        make_link_state(VIRTIO_NET_LINK_DEVICE_ID,
                        NETWORK_LINK_UP, 5U);

    memset(binding, 0, sizeof(*binding));
    memset(io, 0, sizeof(*io));
    io->read_result = true;
    io->status = status;
    io_seam.read_config_status = test_read_config_status;
    io_seam.context = io;
    CHECK(virtio_net_link_state_activate(
              binding, &device, &readiness, &io_seam, &state, 3U,
              7U) == VIRTIO_NET_LINK_OK,
          "link-state helper activates ready binding");
}

static void test_status_strings(void)
{
    static const struct {
        enum virtio_net_link_status status;
        const char *string;
    } cases[] = {
        {VIRTIO_NET_LINK_OK, "VIRTIO_NET_LINK_OK"},
        {VIRTIO_NET_LINK_NULL, "VIRTIO_NET_LINK_NULL"},
        {VIRTIO_NET_LINK_INVALID_VERSION,
         "VIRTIO_NET_LINK_INVALID_VERSION"},
        {VIRTIO_NET_LINK_INVALID_GENERATION,
         "VIRTIO_NET_LINK_INVALID_GENERATION"},
        {VIRTIO_NET_LINK_ALREADY_PUBLISHED,
         "VIRTIO_NET_LINK_ALREADY_PUBLISHED"},
        {VIRTIO_NET_LINK_UNPUBLISHED,
         "VIRTIO_NET_LINK_UNPUBLISHED"},
        {VIRTIO_NET_LINK_INVALID_IO,
         "VIRTIO_NET_LINK_INVALID_IO"},
        {VIRTIO_NET_LINK_IO_FAILED,
         "VIRTIO_NET_LINK_IO_FAILED"},
        {VIRTIO_NET_LINK_INVALID_DEVICE_STATUS,
         "VIRTIO_NET_LINK_INVALID_DEVICE_STATUS"},
        {VIRTIO_NET_LINK_QUEUE_READINESS_MISMATCH,
         "VIRTIO_NET_LINK_QUEUE_READINESS_MISMATCH"},
        {VIRTIO_NET_LINK_INVALID_STATE,
         "VIRTIO_NET_LINK_INVALID_STATE"},
        {VIRTIO_NET_LINK_DEVICE_MISMATCH,
         "VIRTIO_NET_LINK_DEVICE_MISMATCH"},
        {VIRTIO_NET_LINK_PUBLICATION_FAILED,
         "VIRTIO_NET_LINK_PUBLICATION_FAILED"},
        {VIRTIO_NET_LINK_INVALID_DEVICE_SNAPSHOT,
         "VIRTIO_NET_LINK_INVALID_DEVICE_SNAPSHOT"},
    };
    size_t index;

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const char *string = virtio_net_link_status_string(
            cases[index].status);

        CHECK(string != NULL && strcmp(string, cases[index].string) == 0,
              "virtio net link status strings match");
    }
    CHECK(virtio_net_link_status_string(
              (enum virtio_net_link_status)999) == NULL,
          "virtio net link undefined status is NULL");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio net link status handling allocates nothing");
}

static void test_activation_rejections(void)
{
    struct virtio_net_link_state binding;
    struct virtio_net_link_state original;
    struct virtio_net_link_readiness readiness = make_readiness(true);
    struct virtio_net_link_readiness partial = make_readiness(false);
    struct virtio_net_link_io io;
    struct test_io_state io_state;
    struct network_device_snapshot device =
        make_device_snapshot(VIRTIO_NET_LINK_DEVICE_ID,
                             VIRTIO_NET_LINK_CAPABILITY_MASK);
    struct network_link_state_record state =
        make_link_state(VIRTIO_NET_LINK_DEVICE_ID,
                        NETWORK_LINK_UP, 5U);

    memset(&binding, 0, sizeof(binding));
    memset(&io_state, 0, sizeof(io_state));
    io_state.read_result = true;
    io_state.status = VIRTIO_NET_STATUS_LINK_UP;
    io.read_config_status = test_read_config_status;
    io.context = &io_state;

    CHECK(virtio_net_link_state_activate(
              NULL, &device, &readiness, &io, &state, 3U, 7U) ==
              VIRTIO_NET_LINK_NULL,
          "link-state activation rejects NULL binding");
    CHECK(virtio_net_link_state_activate(
              &binding, NULL, &readiness, &io, &state, 3U, 7U) ==
              VIRTIO_NET_LINK_NULL,
          "link-state activation rejects NULL device snapshot");
    CHECK(virtio_net_link_state_activate(
              &binding, &device, NULL, &io, &state, 3U, 7U) ==
              VIRTIO_NET_LINK_NULL,
          "link-state activation rejects NULL readiness");
    CHECK(virtio_net_link_state_activate(
              &binding, &device, &readiness, NULL, &state, 3U, 7U) ==
              VIRTIO_NET_LINK_NULL,
          "link-state activation rejects NULL IO");
    CHECK(virtio_net_link_state_activate(
              &binding, &device, &readiness, &io, NULL, 3U, 7U) ==
              VIRTIO_NET_LINK_NULL,
          "link-state activation rejects NULL state");

    memset(&binding, 0, sizeof(binding));
    binding.version = VIRTIO_NET_LINK_STATE_VERSION + 1U;
    CHECK(virtio_net_link_state_activate(
              &binding, &device, &readiness, &io, &state, 3U, 7U) ==
              VIRTIO_NET_LINK_INVALID_VERSION,
          "link-state activation rejects unknown version");

    memset(&binding, 0, sizeof(binding));
    CHECK(virtio_net_link_state_activate(
              &binding, &device, &readiness, &io, &state, 3U, 0U) ==
              VIRTIO_NET_LINK_INVALID_GENERATION,
          "link-state activation rejects zero generation");
    CHECK(virtio_net_link_state_activate(
              &binding, &device, &readiness, &io, &state, 3U,
              VIRTIO_NET_LINK_STATE_GENERATION_MAX + 1U) ==
              VIRTIO_NET_LINK_INVALID_GENERATION,
          "link-state activation rejects overflow generation");

    memset(&binding, 0, sizeof(binding));
    partial.queue_composition_published = true;
    partial.interrupt_published = true;
    CHECK(virtio_net_link_state_activate(
              &binding, &device, &partial, &io, &state, 3U, 7U) ==
              VIRTIO_NET_LINK_QUEUE_READINESS_MISMATCH,
          "link-state activation rejects partial readiness");

    memset(&binding, 0, sizeof(binding));
    io.read_config_status = NULL;
    CHECK(virtio_net_link_state_activate(
              &binding, &device, &readiness, &io, &state, 3U, 7U) ==
              VIRTIO_NET_LINK_INVALID_IO,
          "link-state activation rejects missing IO seam");
    io.read_config_status = test_read_config_status;
    io.context = NULL;
    CHECK(virtio_net_link_state_activate(
              &binding, &device, &readiness, &io, &state, 3U, 7U) ==
              VIRTIO_NET_LINK_INVALID_IO,
          "link-state activation rejects NULL IO context");
    io.context = &io_state;

    memset(&binding, 0, sizeof(binding));
    device.capability_mask = 0U;
    CHECK(virtio_net_link_state_activate(
              &binding, &device, &readiness, &io, &state, 3U, 7U) ==
              VIRTIO_NET_LINK_INVALID_DEVICE_SNAPSHOT,
          "link-state activation rejects invalid device snapshot");
    device = make_device_snapshot(VIRTIO_NET_LINK_DEVICE_ID,
                                  VIRTIO_NET_LINK_CAPABILITY_MASK);

    memset(&binding, 0, sizeof(binding));
    state.version = NETWORK_LINK_STATE_VERSION + 1U;
    CHECK(virtio_net_link_state_activate(
              &binding, &device, &readiness, &io, &state, 3U, 7U) ==
              VIRTIO_NET_LINK_INVALID_STATE,
          "link-state activation rejects invalid state record");
    state = make_link_state(VIRTIO_NET_LINK_DEVICE_ID,
                            NETWORK_LINK_UP, 5U);

    memset(&binding, 0, sizeof(binding));
    state.device_id = 2U;
    CHECK(virtio_net_link_state_activate(
              &binding, &device, &readiness, &io, &state, 3U, 7U) ==
              VIRTIO_NET_LINK_DEVICE_MISMATCH,
          "link-state activation rejects device mismatch");
    state = make_link_state(VIRTIO_NET_LINK_DEVICE_ID,
                            NETWORK_LINK_UP, 5U);

    memset(&binding, 0, sizeof(binding));
    io_state.read_result = false;
    CHECK(virtio_net_link_state_activate(
              &binding, &device, &readiness, &io, &state, 3U, 7U) ==
              VIRTIO_NET_LINK_IO_FAILED,
          "link-state activation rejects failed status read");
    io_state.read_result = true;
    io_state.status = 4U;
    CHECK(virtio_net_link_state_activate(
              &binding, &device, &readiness, &io, &state, 3U, 7U) ==
              VIRTIO_NET_LINK_INVALID_DEVICE_STATUS,
          "link-state activation rejects unknown status for UP");
    io_state.status = VIRTIO_NET_STATUS_LINK_UP;

    memset(&binding, 0, sizeof(binding));
    original = binding;
    CHECK(virtio_net_link_state_activate(
              &binding, &device, &readiness, &io, &state, 3U, 7U) ==
              VIRTIO_NET_LINK_OK &&
              memcmp(&binding, &original, sizeof(binding)) != 0 &&
              virtio_net_link_state_published(&binding) &&
              virtio_net_link_state_path_enabled(&binding),
          "link-state activation publishes valid UP binding");
    original = binding;
    CHECK(virtio_net_link_state_activate(
              &binding, &device, &readiness, &io, &state, 3U, 7U) ==
              VIRTIO_NET_LINK_ALREADY_PUBLISHED &&
              memcmp(&binding, &original, sizeof(binding)) == 0,
          "link-state activation rejects duplicate publication");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "link-state activation handling allocates nothing");
}

static void test_agreement_matrix(void)
{
    static const struct {
        enum network_link_state state;
        uint32_t status;
        bool ready;
        enum virtio_net_link_status expected;
    } cases[] = {
        {NETWORK_LINK_DOWN, 0U, true, VIRTIO_NET_LINK_OK},
        {NETWORK_LINK_DOWN, VIRTIO_NET_STATUS_LINK_UP, true,
         VIRTIO_NET_LINK_INVALID_DEVICE_STATUS},
        {NETWORK_LINK_INITIALIZING, 0U, true, VIRTIO_NET_LINK_OK},
        {NETWORK_LINK_INITIALIZING, VIRTIO_NET_STATUS_LINK_UP, true,
         VIRTIO_NET_LINK_INVALID_DEVICE_STATUS},
        {NETWORK_LINK_INITIALIZING, 0U, false,
         VIRTIO_NET_LINK_QUEUE_READINESS_MISMATCH},
        {NETWORK_LINK_UP, VIRTIO_NET_STATUS_LINK_UP, true,
         VIRTIO_NET_LINK_OK},
        {NETWORK_LINK_UP, 0U, true,
         VIRTIO_NET_LINK_INVALID_DEVICE_STATUS},
        {NETWORK_LINK_UP, VIRTIO_NET_STATUS_LINK_UP, false,
         VIRTIO_NET_LINK_QUEUE_READINESS_MISMATCH},
        {NETWORK_LINK_FAILED, 4U, true, VIRTIO_NET_LINK_OK},
        {NETWORK_LINK_FAILED, VIRTIO_NET_STATUS_LINK_UP, true,
         VIRTIO_NET_LINK_QUEUE_READINESS_MISMATCH},
        {NETWORK_LINK_FAILED, 0U, true,
         VIRTIO_NET_LINK_QUEUE_READINESS_MISMATCH},
    };
    struct virtio_net_link_state binding;
    struct test_io_state io;
    struct virtio_net_link_readiness readiness;
    struct virtio_net_link_io io_seam;
    struct network_device_snapshot device =
        make_device_snapshot(VIRTIO_NET_LINK_DEVICE_ID,
                             VIRTIO_NET_LINK_CAPABILITY_MASK);
    struct network_link_state_record state;
    size_t index;

    for (index = 0U;
         index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        memset(&binding, 0, sizeof(binding));
        memset(&io, 0, sizeof(io));
        io.read_result = true;
        io.status = cases[index].status;
        readiness = make_readiness(cases[index].ready);
        io_seam.read_config_status = test_read_config_status;
        io_seam.context = &io;
        state = make_link_state(VIRTIO_NET_LINK_DEVICE_ID,
                                cases[index].state, 5U);

        CHECK(virtio_net_link_state_activate(
                  &binding, &device, &readiness, &io_seam, &state,
                  3U, 7U) == cases[index].expected,
              "link-state agreement matrix matches");
        if (cases[index].expected == VIRTIO_NET_LINK_OK) {
            CHECK(virtio_net_link_state_published(&binding) &&
                      virtio_net_link_state_selection(&binding) != NULL &&
                      virtio_net_link_state_selection(&binding)->link.state ==
                          cases[index].state,
                  "agreement matrix publishes exact state");
        } else {
            CHECK(!virtio_net_link_state_published(&binding),
                  "agreement matrix leaves binding unpublished");
        }
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "link-state agreement matrix allocates nothing");
}

static void test_transitions(void)
{
    struct virtio_net_link_state binding;
    struct virtio_net_link_state original;
    struct test_io_state io;
    struct virtio_net_link_readiness readiness = make_readiness(true);
    struct virtio_net_link_readiness lost = make_readiness(false);
    struct network_link_state_record state;

    activate_ready(&binding, &io, VIRTIO_NET_STATUS_LINK_UP);

    CHECK(virtio_net_link_state_transition(
              NULL, &readiness, &state, 3U) == VIRTIO_NET_LINK_NULL,
          "link-state transition rejects NULL binding");
    state = make_link_state(VIRTIO_NET_LINK_DEVICE_ID,
                            NETWORK_LINK_DOWN, 6U);
    io.status = 0U;
    CHECK(virtio_net_link_state_transition(
              &binding, NULL, &state, 3U) == VIRTIO_NET_LINK_NULL,
          "link-state transition rejects NULL readiness");
    CHECK(virtio_net_link_state_transition(
              &binding, &readiness, NULL, 3U) == VIRTIO_NET_LINK_NULL,
          "link-state transition rejects NULL state");

    memset(&binding, 0, sizeof(binding));
    CHECK(virtio_net_link_state_transition(
              &binding, &readiness, &state, 3U) ==
              VIRTIO_NET_LINK_UNPUBLISHED,
          "link-state transition rejects unpublished binding");

    activate_ready(&binding, &io, VIRTIO_NET_STATUS_LINK_UP);
    original = binding;
    state = make_link_state(2U, NETWORK_LINK_DOWN, 6U);
    io.status = 0U;
    CHECK(virtio_net_link_state_transition(
              &binding, &readiness, &state, 3U) ==
              VIRTIO_NET_LINK_DEVICE_MISMATCH &&
              memcmp(&binding, &original, sizeof(binding)) == 0,
          "link-state transition rejects changed device without mutation");
    state = make_link_state(VIRTIO_NET_LINK_DEVICE_ID,
                            NETWORK_LINK_DOWN, 6U);

    original = binding;
    io.status = VIRTIO_NET_STATUS_LINK_UP;
    CHECK(virtio_net_link_state_transition(
              &binding, &readiness,
              &(struct network_link_state_record){
                  .version = NETWORK_LINK_STATE_VERSION,
                  .device_id = VIRTIO_NET_LINK_DEVICE_ID,
                  .state = NETWORK_LINK_UP,
                  .generation = 6U,
              },
              3U) == VIRTIO_NET_LINK_OK &&
              virtio_net_link_state_path_enabled(&binding),
          "link-state transition republishes UP");
    original = binding;
    io.status = VIRTIO_NET_STATUS_LINK_UP;
    CHECK(virtio_net_link_state_transition(
              &binding, &readiness, &state, 3U) ==
              VIRTIO_NET_LINK_INVALID_DEVICE_STATUS &&
              memcmp(&binding, &original, sizeof(binding)) == 0,
          "link-state transition rejects UP status with DOWN state");

    activate_ready(&binding, &io, VIRTIO_NET_STATUS_LINK_UP);
    state = make_link_state(VIRTIO_NET_LINK_DEVICE_ID,
                            NETWORK_LINK_DOWN, 6U);
    io.status = 0U;
    CHECK(virtio_net_link_state_transition(
              &binding, &readiness, &state, 3U) ==
              VIRTIO_NET_LINK_OK &&
              virtio_net_link_state_published(&binding) &&
              virtio_net_link_state_selection(&binding) != NULL &&
              virtio_net_link_state_selection(&binding)->link.state ==
                  NETWORK_LINK_DOWN &&
              !virtio_net_link_state_path_enabled(&binding),
          "link loss closes admission while queues remain ready");

    state = make_link_state(VIRTIO_NET_LINK_DEVICE_ID,
                            NETWORK_LINK_FAILED, 7U);
    io.status = VIRTIO_NET_STATUS_LINK_UP;
    CHECK(virtio_net_link_state_transition(
              &binding, &lost, &state, 3U) ==
              VIRTIO_NET_LINK_OK &&
              virtio_net_link_state_published(&binding) &&
              virtio_net_link_state_selection(&binding)->link.state ==
                  NETWORK_LINK_FAILED &&
              !virtio_net_link_state_path_enabled(&binding),
          "lost readiness publishes failed state and keeps admission closed");

    activate_ready(&binding, &io, VIRTIO_NET_STATUS_LINK_UP);
    state = make_link_state(VIRTIO_NET_LINK_DEVICE_ID,
                            NETWORK_LINK_FAILED, 7U);
    io.status = 4U;
    CHECK(virtio_net_link_state_transition(
              &binding, &readiness, &state, 3U) ==
              VIRTIO_NET_LINK_OK &&
              !virtio_net_link_state_path_enabled(&binding),
          "invalid device status publishes failed state");

    activate_ready(&binding, &io, VIRTIO_NET_STATUS_LINK_UP);
    original = binding;
    state = make_link_state(VIRTIO_NET_LINK_DEVICE_ID,
                            NETWORK_LINK_DOWN, 8U);
    io.read_result = false;
    CHECK(virtio_net_link_state_transition(
              &binding, &readiness, &state, 3U) ==
              VIRTIO_NET_LINK_IO_FAILED &&
              memcmp(&binding, &original, sizeof(binding)) == 0,
          "link-state transition failed status read leaves binding unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "link-state transition handling allocates nothing");
}

int main(void)
{
    test_status_strings();
    test_activation_rejections();
    test_agreement_matrix();
    test_transitions();

    if (failures != 0) {
        fprintf(stderr,
                "Virtio net link-state binding host tests: FAIL\n");
        return 1;
    }
    printf("Virtio net link-state binding host tests: ALL OK\n");
    return 0;
}

#endif
