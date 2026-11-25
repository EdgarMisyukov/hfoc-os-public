#include "network_backend.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_NETWORK_BACKEND_HOST_TEST

#define TEST_GENERATION 1U
#define TEST_LINK_GENERATION 1U
#define TEST_VENDOR_ID 0x10ECU
#define TEST_PRODUCT_ID 0x8168U
#define TEST_BUS_ADDRESS UINT32_C(0x0310)
#define TEST_DEVICE_ID NETWORK_BACKEND_DEVICE_ID

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

struct link_fixture {
    bool link_up;
    bool fail_read;
    size_t reads;
};

static bool fixture_read_link_up(void *context, bool *link_up)
{
    struct link_fixture *fixture = (struct link_fixture *)context;

    if (fixture == NULL || link_up == NULL) {
        return false;
    }
    ++fixture->reads;
    if (fixture->fail_read) {
        return false;
    }
    *link_up = fixture->link_up;
    return true;
}

static void make_snapshot(struct network_device_snapshot *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->version = NETWORK_DEVICE_SNAPSHOT_VERSION;
    snapshot->device_id = TEST_DEVICE_ID;
    snapshot->vendor_id = TEST_VENDOR_ID;
    snapshot->product_id = TEST_PRODUCT_ID;
    snapshot->bus_address = TEST_BUS_ADDRESS;
    snapshot->mac_address[0] = 0x52U;
    snapshot->mac_address[1] = 0x54U;
    snapshot->mac_address[2] = 0x00U;
    snapshot->mac_address[3] = 0x12U;
    snapshot->mac_address[4] = 0x34U;
    snapshot->mac_address[5] = 0x56U;
    snapshot->capability_mask = NETWORK_BACKEND_CAPABILITY_MASK;
}

static void make_link_record(
    struct network_link_state_record *record,
    enum network_link_state state)
{
    memset(record, 0, sizeof(*record));
    record->version = NETWORK_LINK_STATE_VERSION;
    record->device_id = TEST_DEVICE_ID;
    record->state = state;
    record->generation = TEST_LINK_GENERATION;
}

static void make_readiness(
    struct network_backend_readiness *readiness,
    bool ready)
{
    readiness->receive_path_ready = ready;
    readiness->transmit_path_ready = ready;
}

static enum network_backend_status bind_fixture(
    struct network_backend_binding *binding,
    struct link_fixture *fixture,
    enum network_link_state state,
    bool ready,
    struct network_backend_link_io *io_out)
{
    struct network_device_snapshot snapshot;
    struct network_link_state_record record;
    struct network_backend_readiness readiness;
    struct network_backend_link_io io;

    make_snapshot(&snapshot);
    make_link_record(&record, state);
    make_readiness(&readiness, ready);
    memset(&io, 0, sizeof(io));
    io.read_link_up = fixture_read_link_up;
    io.context = fixture;
    if (io_out != NULL) {
        *io_out = io;
    }
    return network_backend_bind(
        binding, &snapshot, &record, &readiness, &io, TEST_GENERATION);
}

static void test_bind_success(void)
{
    struct network_backend_binding binding;
    struct link_fixture fixture = { true, false, 0U };
    const struct network_device_selection *device;
    const struct network_link_selection *link;
    bool link_up = false;

    memset(&binding, 0, sizeof(binding));
    CHECK(bind_fixture(&binding, &fixture, NETWORK_LINK_UP, true, NULL) ==
              NETWORK_BACKEND_OK,
          "backend bind accepts a present device with an up link");
    CHECK(network_backend_published(&binding),
          "bound backend is published");
    CHECK(binding.version == NETWORK_BACKEND_VERSION,
          "bound backend carries the binding version");
    CHECK(binding.generation == TEST_GENERATION,
          "bound backend carries the generation");
    device = network_backend_device_selection(&binding);
    CHECK(device != NULL && device->published,
          "bound backend publishes the device selection");
    CHECK(device != NULL &&
              device->device.vendor_id == TEST_VENDOR_ID &&
              device->device.product_id == TEST_PRODUCT_ID,
          "device selection carries the Realtek identity");
    link = network_backend_link_selection(&binding);
    CHECK(link != NULL && link->published &&
              link->link.state == NETWORK_LINK_UP,
          "bound backend publishes the up link record");
    CHECK(network_backend_path_enabled(&binding),
          "an up link enables the path");
    CHECK(network_backend_link_up(&binding, &link_up) && link_up,
          "backend link read reports the device link up");
    CHECK(network_backend_status_string(NETWORK_BACKEND_OK) != NULL,
          "backend status string is published");
}

static void test_down_link(void)
{
    struct network_backend_binding binding;
    struct link_fixture fixture = { false, false, 0U };
    const struct network_link_selection *link;
    bool link_up = true;

    memset(&binding, 0, sizeof(binding));
    CHECK(bind_fixture(&binding, &fixture, NETWORK_LINK_DOWN, false, NULL) ==
              NETWORK_BACKEND_OK,
          "backend bind accepts a present device with a down link");
    link = network_backend_link_selection(&binding);
    CHECK(link != NULL && link->link.state == NETWORK_LINK_DOWN,
          "the down link record is published honestly");
    CHECK(!network_backend_path_enabled(&binding),
          "a down link does not enable the path");
    CHECK(network_backend_link_up(&binding, &link_up) && !link_up,
          "backend link read reports the device link down");
}

static void test_device_rejection(void)
{
    struct network_device_snapshot snapshot;
    struct network_link_state_record record;
    struct network_backend_readiness readiness;
    struct network_backend_link_io io;
    struct network_backend_binding binding;
    struct link_fixture fixture = { true, false, 0U };

    memset(&io, 0, sizeof(io));
    io.read_link_up = fixture_read_link_up;
    io.context = &fixture;
    make_link_record(&record, NETWORK_LINK_UP);
    make_readiness(&readiness, true);

    make_snapshot(&snapshot);
    snapshot.version = NETWORK_DEVICE_SNAPSHOT_VERSION + 1U;
    memset(&binding, 0, sizeof(binding));
    CHECK(network_backend_bind(&binding, &snapshot, &record, &readiness,
                               &io, TEST_GENERATION) ==
              NETWORK_BACKEND_INVALID_DEVICE_SNAPSHOT,
          "an invalid device snapshot is rejected");

    make_snapshot(&snapshot);
    memset(snapshot.mac_address, 0, NETWORK_DEVICE_MAC_LENGTH);
    memset(&binding, 0, sizeof(binding));
    CHECK(network_backend_bind(&binding, &snapshot, &record, &readiness,
                               &io, TEST_GENERATION) ==
              NETWORK_BACKEND_INVALID_MAC_ADDRESS,
          "a zero MAC is rejected");

    make_snapshot(&snapshot);
    snapshot.mac_address[0] = 0x01U;
    memset(&binding, 0, sizeof(binding));
    CHECK(network_backend_bind(&binding, &snapshot, &record, &readiness,
                               &io, TEST_GENERATION) ==
              NETWORK_BACKEND_MULTICAST_MAC_ADDRESS,
          "a multicast MAC is rejected");

    make_snapshot(&snapshot);
    snapshot.capability_mask = NETWORK_DEVICE_CAPABILITY_RX;
    memset(&binding, 0, sizeof(binding));
    CHECK(network_backend_bind(&binding, &snapshot, &record, &readiness,
                               &io, TEST_GENERATION) ==
              NETWORK_BACKEND_UNSUPPORTED_CAPABILITY,
          "a device without the transmit and link capabilities is refused");
}

static void test_link_rejection(void)
{
    struct network_device_snapshot snapshot;
    struct network_link_state_record record;
    struct network_backend_readiness readiness;
    struct network_backend_link_io io;
    struct network_backend_binding binding;
    struct link_fixture fixture = { true, false, 0U };

    memset(&io, 0, sizeof(io));
    io.read_link_up = fixture_read_link_up;
    io.context = &fixture;
    make_snapshot(&snapshot);

    make_link_record(&record, NETWORK_LINK_UP);
    record.device_id = TEST_DEVICE_ID + 1U;
    make_readiness(&readiness, true);
    memset(&binding, 0, sizeof(binding));
    CHECK(network_backend_bind(&binding, &snapshot, &record, &readiness,
                               &io, TEST_GENERATION) ==
              NETWORK_BACKEND_DEVICE_MISMATCH,
          "a link record for another device is rejected");

    make_link_record(&record, NETWORK_LINK_UP);
    make_readiness(&readiness, false);
    memset(&binding, 0, sizeof(binding));
    CHECK(network_backend_bind(&binding, &snapshot, &record, &readiness,
                               &io, TEST_GENERATION) ==
              NETWORK_BACKEND_READINESS_MISMATCH,
          "an up link without a ready data path is rejected");

    make_link_record(&record, NETWORK_LINK_FAILED);
    make_readiness(&readiness, true);
    memset(&binding, 0, sizeof(binding));
    CHECK(network_backend_bind(&binding, &snapshot, &record, &readiness,
                               &io, TEST_GENERATION) ==
              NETWORK_BACKEND_READINESS_MISMATCH,
          "a failed link with a ready data path is rejected");

    make_link_record(&record, NETWORK_LINK_INITIALIZING);
    make_readiness(&readiness, true);
    memset(&binding, 0, sizeof(binding));
    CHECK(network_backend_bind(&binding, &snapshot, &record, &readiness,
                               &io, TEST_GENERATION) ==
              NETWORK_BACKEND_LINK_DISAGREEMENT,
          "an initializing link with a live device link is rejected");

    make_link_record(&record, NETWORK_LINK_UP);
    make_readiness(&readiness, true);
    record.version = NETWORK_LINK_STATE_VERSION + 1U;
    memset(&binding, 0, sizeof(binding));
    CHECK(network_backend_bind(&binding, &snapshot, &record, &readiness,
                               &io, TEST_GENERATION) ==
              NETWORK_BACKEND_INVALID_LINK_STATE,
          "an invalid link record is rejected");
}

static void test_link_disagreement(void)
{
    struct network_backend_binding binding;
    struct link_fixture fixture = { false, false, 0U };

    memset(&binding, 0, sizeof(binding));
    CHECK(bind_fixture(&binding, &fixture, NETWORK_LINK_UP, true, NULL) ==
              NETWORK_BACKEND_LINK_DISAGREEMENT,
          "a claimed up link the device denies is refused");

    memset(&binding, 0, sizeof(binding));
    fixture.link_up = true;
    CHECK(bind_fixture(&binding, &fixture, NETWORK_LINK_DOWN, false, NULL) ==
              NETWORK_BACKEND_LINK_DISAGREEMENT,
          "a claimed down link the device reports up is refused");
}

static void test_io_and_state_rejection(void)
{
    struct network_device_snapshot snapshot;
    struct network_link_state_record record;
    struct network_backend_readiness readiness;
    struct network_backend_link_io io;
    struct network_backend_binding binding;
    struct link_fixture fixture = { true, true, 0U };

    make_snapshot(&snapshot);
    make_link_record(&record, NETWORK_LINK_UP);
    make_readiness(&readiness, true);

    memset(&binding, 0, sizeof(binding));
    CHECK(network_backend_bind(&binding, &snapshot, &record, &readiness,
                               NULL, TEST_GENERATION) ==
              NETWORK_BACKEND_NULL,
          "a null io contract is rejected");

    memset(&io, 0, sizeof(io));
    memset(&binding, 0, sizeof(binding));
    CHECK(network_backend_bind(&binding, &snapshot, &record, &readiness,
                               &io, TEST_GENERATION) ==
              NETWORK_BACKEND_INVALID_IO,
          "an io contract without a link read is rejected");

    io.read_link_up = fixture_read_link_up;
    io.context = &fixture;
    memset(&binding, 0, sizeof(binding));
    CHECK(network_backend_bind(&binding, &snapshot, &record, &readiness,
                               &io, TEST_GENERATION) ==
              NETWORK_BACKEND_IO_FAILED,
          "a failed device link read is not published as a link");

    fixture.fail_read = false;
    memset(&binding, 0, sizeof(binding));
    CHECK(network_backend_bind(&binding, &snapshot, &record, &readiness,
                               &io, TEST_GENERATION + 0U) ==
              NETWORK_BACKEND_OK,
          "a valid bind still succeeds after a failed read attempt");

    memset(&binding, 0, sizeof(binding));
    CHECK(network_backend_bind(&binding, &snapshot, &record, &readiness,
                               &io, 0U) ==
              NETWORK_BACKEND_INVALID_GENERATION,
          "an out-of-range generation is rejected");

    memset(&binding, 0, sizeof(binding));
    CHECK(network_backend_bind(&binding, &snapshot, &record, &readiness,
                               &io, TEST_GENERATION) ==
              NETWORK_BACKEND_OK &&
              network_backend_bind(&binding, &snapshot, &record,
                                   &readiness, &io,
                                   TEST_GENERATION) ==
                  NETWORK_BACKEND_ALREADY_PUBLISHED,
          "a second bind on a published binding is refused");

    CHECK(!network_backend_published(NULL),
          "a null binding is not published");
}

static void test_published_tamper(void)
{
    struct network_backend_binding binding;
    struct link_fixture fixture = { true, false, 0U };
    bool link_up = true;

    memset(&binding, 0, sizeof(binding));
    CHECK(bind_fixture(&binding, &fixture, NETWORK_LINK_UP, true, NULL) ==
              NETWORK_BACKEND_OK,
          "tamper fixture binds");

    binding.device_snapshot.mac_address[0] = 0x01U;
    CHECK(!network_backend_published(&binding),
          "a binding whose device MAC turned multicast is not published");
    CHECK(network_backend_device_selection(&binding) == NULL,
          "an unpublished binding exposes no device selection");
    CHECK(network_backend_link_selection(&binding) == NULL,
          "an unpublished binding exposes no link selection");
    CHECK(!network_backend_path_enabled(&binding),
          "an unpublished binding has no enabled path");
    CHECK(!network_backend_link_up(&binding, &link_up) && !link_up,
          "an unpublished binding reports no link");
    CHECK(!network_backend_link_up(&binding, NULL),
          "a null link output is refused");
}

int main(void)
{
    test_bind_success();
    test_down_link();
    test_device_rejection();
    test_link_rejection();
    test_link_disagreement();
    test_io_and_state_rejection();
    test_published_tamper();

    if (failures != 0) {
        fprintf(stderr, "Network backend host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Network backend host tests: ALL OK");
    return 0;
}

#endif
