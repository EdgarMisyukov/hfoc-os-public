#include "hfoc_netif_input.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_NETIF_INPUT_HOST_TEST

#define TEST_COUNT 2U
#define TEST_CAPACITY 2048U
#define TEST_HEADROOM 0U
#define TEST_LENGTH 2048U
#define TEST_STORAGE_SIZE (TEST_COUNT * (TEST_CAPACITY + TEST_HEADROOM))
#define TEST_PAYLOAD_LENGTH 42U
#define TEST_PADDING_LENGTH 4U
#define TEST_FRAME_LENGTH \
    (ETHERNET_FRAME_HEADER_LENGTH + \
     TEST_PAYLOAD_LENGTH + TEST_PADDING_LENGTH)

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;
static struct packet_pool_descriptor pool;
static unsigned char storage[TEST_STORAGE_SIZE];

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

struct adapter_fixture {
    enum hfoc_netif_input_result verdict;
    bool called;
    bool take_ownership;
    bool release_before_return;
    unsigned int calls;
    struct packet_slice slice_copy;
    struct ethernet_frame_view view_copy;
    struct packet_pool_slot_descriptor owned;
};

static void reset_pool(void)
{
    memset(&pool, 0, sizeof(pool));
    memset(storage, 0xA5, sizeof(storage));
    alloc_attempts = 0U;
    free_attempts = 0U;
}

static bool init_pool(void)
{
    reset_pool();
    return packet_pool_init(
               &pool, storage, sizeof(storage),
               TEST_COUNT, TEST_CAPACITY,
               TEST_HEADROOM, TEST_LENGTH) == PACKET_POOL_OK;
}

static bool counts_are(
    uint32_t free_count,
    uint32_t reserved_count,
    uint32_t active_count)
{
    return packet_pool_free_count(&pool) == free_count &&
           packet_pool_reserved_count(&pool) == reserved_count &&
           packet_pool_active_count(&pool) == active_count;
}

static bool write_bytes(
    struct packet_slice *slice,
    const void *data,
    size_t size)
{
    return packet_slice_write(slice, data, size) == PACKET_SLICE_OK;
}

static bool init_valid_rx_frame(
    struct packet_slice *slice,
    struct ethernet_frame_view *view)
{
    static const uint8_t destination[6] =
        {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U};
    static const uint8_t source[6] =
        {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U};
    static const uint16_t allowed_types[] = {
        ETHERNET_FRAME_ETHERTYPE_IPV4,
        ETHERNET_FRAME_ETHERTYPE_ARP,
    };
    struct packet_pool_slot_descriptor descriptor;
    struct ethernet_frame_policy policy;
    uint8_t type_hi = (uint8_t)(
        (ETHERNET_FRAME_ETHERTYPE_IPV4 >> 8U) & 0xFFU);
    uint8_t type_lo = (uint8_t)(
        ETHERNET_FRAME_ETHERTYPE_IPV4 & 0xFFU);
    uint32_t index;
    uint8_t byte;

    if (packet_pool_reserve(
            &pool, PACKET_POOL_DIRECTION_RX,
            &descriptor) != PACKET_POOL_OK) {
        return false;
    }
    if (packet_pool_commit(
            &pool, &descriptor) != PACKET_POOL_OK) {
        return false;
    }
    if (packet_slice_init(
            slice, &pool, &descriptor,
            TEST_FRAME_LENGTH) != PACKET_SLICE_OK) {
        return false;
    }
    if (!write_bytes(
            slice, destination,
            ETHERNET_FRAME_DESTINATION_LENGTH) ||
        !write_bytes(
            slice, source,
            ETHERNET_FRAME_SOURCE_LENGTH) ||
        !write_bytes(slice, &type_hi, 1U) ||
        !write_bytes(slice, &type_lo, 1U)) {
        return false;
    }
    for (index = 0U; index < TEST_PAYLOAD_LENGTH; ++index) {
        byte = (uint8_t)(index & 0xFFU);
        if (!write_bytes(slice, &byte, 1U)) {
            return false;
        }
    }
    for (index = 0U; index < TEST_PADDING_LENGTH; ++index) {
        byte = 0U;
        if (!write_bytes(slice, &byte, 1U)) {
            return false;
        }
    }
    if (packet_slice_seek(slice, 0U) != PACKET_SLICE_OK) {
        return false;
    }

    memset(&policy, 0, sizeof(policy));
    policy.version = ETHERNET_FRAME_POLICY_VERSION;
    policy.accept_unicast = true;
    policy.accept_broadcast = true;
    policy.accept_multicast = false;
    policy.allowed_ether_types = allowed_types;
    policy.allowed_ether_type_count = 2U;
    return ethernet_frame_validate(
               slice, &policy, TEST_PAYLOAD_LENGTH,
               view) == ETHERNET_FRAME_OK;
}

static enum hfoc_netif_input_result adapter_deliver(
    void *context,
    struct packet_slice *slice,
    const struct ethernet_frame_view *view)
{
    struct adapter_fixture *fixture =
        (struct adapter_fixture *)context;

    fixture->called = true;
    ++fixture->calls;
    fixture->slice_copy = *slice;
    fixture->view_copy = *view;
    if (fixture->take_ownership) {
        fixture->owned = slice->slot;
    }
    if (fixture->release_before_return) {
        (void)packet_pool_release(&pool, &slice->slot);
    }
    return fixture->verdict;
}

static bool init_input(
    struct hfoc_netif_input *input,
    const struct adapter_fixture *fixture)
{
    struct hfoc_netif_input_adapter adapter;

    memset(input, 0, sizeof(*input));
    adapter.deliver = adapter_deliver;
    adapter.context = (void *)fixture;
    return hfoc_netif_input_init(
               input, &pool, &adapter) == HFOC_NETIF_INPUT_OK;
}

static bool prepare_transfer(
    struct hfoc_netif_input *input,
    struct adapter_fixture *fixture,
    enum hfoc_netif_input_result verdict,
    bool take_ownership,
    bool release_before_return,
    struct packet_slice *slice,
    struct ethernet_frame_view *view)
{
    if (!init_pool()) {
        return false;
    }
    memset(fixture, 0, sizeof(*fixture));
    fixture->verdict = verdict;
    fixture->take_ownership = take_ownership;
    fixture->release_before_return = release_before_return;
    if (!init_valid_rx_frame(slice, view)) {
        return false;
    }
    return init_input(input, fixture);
}

static bool adapter_saw_exact_input(
    const struct adapter_fixture *fixture,
    const struct packet_slice *slice,
    const struct ethernet_frame_view *view)
{
    return fixture->called &&
           fixture->calls == 1U &&
           fixture->slice_copy.version == slice->version &&
           fixture->slice_copy.pool == slice->pool &&
           fixture->slice_copy.slot.slot == slice->slot.slot &&
           fixture->slice_copy.slot.generation ==
               slice->slot.generation &&
           fixture->slice_copy.slot.owner == slice->slot.owner &&
           fixture->slice_copy.slot.direction ==
               slice->slot.direction &&
           fixture->slice_copy.start == slice->start &&
           fixture->slice_copy.length == slice->length &&
           fixture->slice_copy.cursor == slice->cursor &&
           fixture->view_copy.version == view->version &&
           fixture->view_copy.destination == view->destination &&
           fixture->view_copy.ether_type == view->ether_type &&
           fixture->view_copy.payload_offset ==
               view->payload_offset &&
           fixture->view_copy.payload_length ==
               view->payload_length &&
           fixture->view_copy.padding_length ==
               view->padding_length;
}

static void test_status_strings(void)
{
    static const struct {
        enum hfoc_netif_input_result result;
        const char *string;
    } results[] = {
        {HFOC_NETIF_INPUT_ACCEPTED, "HFOC_NETIF_INPUT_ACCEPTED"},
        {HFOC_NETIF_INPUT_BACKPRESSURE,
         "HFOC_NETIF_INPUT_BACKPRESSURE"},
        {HFOC_NETIF_INPUT_REJECTED, "HFOC_NETIF_INPUT_REJECTED"},
    };
    static const struct {
        enum hfoc_netif_input_status status;
        const char *string;
    } statuses[] = {
        {HFOC_NETIF_INPUT_OK, "HFOC_NETIF_INPUT_OK"},
        {HFOC_NETIF_INPUT_NULL, "HFOC_NETIF_INPUT_NULL"},
        {HFOC_NETIF_INPUT_BAD_STATE, "HFOC_NETIF_INPUT_BAD_STATE"},
        {HFOC_NETIF_INPUT_INVALID_VERSION,
         "HFOC_NETIF_INPUT_INVALID_VERSION"},
        {HFOC_NETIF_INPUT_ALREADY_PUBLISHED,
         "HFOC_NETIF_INPUT_ALREADY_PUBLISHED"},
        {HFOC_NETIF_INPUT_UNPUBLISHED,
         "HFOC_NETIF_INPUT_UNPUBLISHED"},
        {HFOC_NETIF_INPUT_BAD_POOL, "HFOC_NETIF_INPUT_BAD_POOL"},
        {HFOC_NETIF_INPUT_INVALID_ADAPTER,
         "HFOC_NETIF_INPUT_INVALID_ADAPTER"},
        {HFOC_NETIF_INPUT_BAD_SLICE, "HFOC_NETIF_INPUT_BAD_SLICE"},
        {HFOC_NETIF_INPUT_WRONG_DIRECTION,
         "HFOC_NETIF_INPUT_WRONG_DIRECTION"},
        {HFOC_NETIF_INPUT_INVALID_VIEW,
         "HFOC_NETIF_INPUT_INVALID_VIEW"},
        {HFOC_NETIF_INPUT_RELEASE_FAILED,
         "HFOC_NETIF_INPUT_RELEASE_FAILED"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(results) / sizeof(results[0]);
         ++index) {
        const char *string = hfoc_netif_input_result_string(
            results[index].result);

        CHECK(string != NULL &&
                  strcmp(string, results[index].string) == 0,
              "netif input result string is exact");
    }
    CHECK(hfoc_netif_input_result_string(
              (enum hfoc_netif_input_result)3U) == NULL,
          "netif input undefined result string is rejected");
    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = hfoc_netif_input_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "netif input status string is exact");
    }
    CHECK(hfoc_netif_input_status_string(
              (enum hfoc_netif_input_status)12U) == NULL,
          "netif input undefined status string is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "netif input status handling performs no allocation");
}

static void test_init_contracts(void)
{
    struct hfoc_netif_input input;
    struct hfoc_netif_input_adapter adapter;
    struct adapter_fixture fixture;

    memset(&input, 0, sizeof(input));
    memset(&fixture, 0, sizeof(fixture));
    CHECK(init_pool(),
          "netif input init fixture initializes pool");

    adapter.deliver = adapter_deliver;
    adapter.context = &fixture;
    CHECK(hfoc_netif_input_init(
              NULL, &pool, &adapter) == HFOC_NETIF_INPUT_NULL,
          "netif input init rejects null input");
    CHECK(hfoc_netif_input_init(
              &input, NULL, &adapter) == HFOC_NETIF_INPUT_BAD_POOL,
          "netif input init rejects null pool");
    CHECK(hfoc_netif_input_init(
              &input, &pool, NULL) == HFOC_NETIF_INPUT_NULL,
          "netif input init rejects null adapter");
    adapter.deliver = NULL;
    CHECK(hfoc_netif_input_init(
              &input, &pool, &adapter) ==
              HFOC_NETIF_INPUT_INVALID_ADAPTER,
          "netif input init rejects null deliver callback");
    adapter.deliver = adapter_deliver;
    CHECK(hfoc_netif_input_init(
              &input, &pool, &adapter) == HFOC_NETIF_INPUT_OK &&
              hfoc_netif_input_published(&input),
          "netif input init publishes valid input");
    CHECK(hfoc_netif_input_init(
              &input, &pool, &adapter) ==
              HFOC_NETIF_INPUT_ALREADY_PUBLISHED,
          "netif input init rejects duplicate publication");
    CHECK(hfoc_netif_input_published(NULL) == false,
          "netif input published rejects null input");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "netif input init paths perform no allocation");
}

static void test_transfer_preconditions(void)
{
    struct hfoc_netif_input input;
    struct adapter_fixture fixture;
    struct packet_slice slice;
    struct ethernet_frame_view view;
    enum hfoc_netif_input_result result;

    CHECK(prepare_transfer(
              &input, &fixture,
              HFOC_NETIF_INPUT_ACCEPTED, false, false,
              &slice, &view),
          "netif input precondition fixture initializes");

    CHECK(hfoc_netif_input_transfer(
              &input, NULL, &view, &result) ==
              HFOC_NETIF_INPUT_NULL &&
              !fixture.called,
          "netif input transfer rejects null slice");
    CHECK(hfoc_netif_input_transfer(
              &input, &slice, NULL, &result) ==
              HFOC_NETIF_INPUT_NULL &&
              !fixture.called,
          "netif input transfer rejects null view");
    CHECK(hfoc_netif_input_transfer(
              &input, &slice, &view, NULL) ==
              HFOC_NETIF_INPUT_NULL &&
              !fixture.called,
          "netif input transfer rejects null result");

    view.version = UINT32_C(0xDEADBEEF);
    CHECK(hfoc_netif_input_transfer(
              &input, &slice, &view, &result) ==
              HFOC_NETIF_INPUT_INVALID_VIEW &&
              !fixture.called &&
              counts_are(TEST_COUNT - 1U, 0U, 1U),
          "netif input transfer rejects invalid view without mutation");
    view.version = ETHERNET_FRAME_VERSION;

    CHECK(hfoc_netif_input_transfer(
              &input, NULL, &view, &result) ==
              HFOC_NETIF_INPUT_NULL &&
              !fixture.called &&
              counts_are(TEST_COUNT - 1U, 0U, 1U),
          "netif input null transfer leaves ownership unchanged");

    memset(&slice, 0, sizeof(slice));
    slice.version = PACKET_SLICE_VERSION;
    slice.pool = &pool;
    CHECK(hfoc_netif_input_transfer(
              &input, &slice, &view, &result) ==
              HFOC_NETIF_INPUT_BAD_SLICE &&
              !fixture.called,
          "netif input transfer rejects invalid slice");

    CHECK(prepare_transfer(
              &input, &fixture,
              HFOC_NETIF_INPUT_ACCEPTED, false, false,
              &slice, &view),
          "netif input unpublished fixture initializes");
    memset(&input, 0, sizeof(input));
    CHECK(hfoc_netif_input_transfer(
              &input, &slice, &view, &result) ==
              HFOC_NETIF_INPUT_UNPUBLISHED &&
              !fixture.called,
          "netif input transfer rejects unpublished input");

    CHECK(init_pool(),
          "netif input wrong-direction fixture initializes pool");
    {
        struct packet_pool_slot_descriptor tx;
        struct hfoc_netif_input_adapter adapter;

        CHECK(packet_pool_reserve(
                  &pool, PACKET_POOL_DIRECTION_TX, &tx) ==
                  PACKET_POOL_OK &&
                  packet_pool_commit(&pool, &tx) == PACKET_POOL_OK &&
                  packet_slice_init(
                      &slice, &pool, &tx, 1U) == PACKET_SLICE_OK,
              "netif input wrong-direction TX slice initializes");
        memset(&fixture, 0, sizeof(fixture));
        adapter.deliver = adapter_deliver;
        adapter.context = &fixture;
        CHECK(hfoc_netif_input_init(
                  &input, &pool, &adapter) == HFOC_NETIF_INPUT_OK &&
                  hfoc_netif_input_transfer(
                      &input, &slice, &view, &result) ==
                      HFOC_NETIF_INPUT_WRONG_DIRECTION &&
                  !fixture.called &&
                  counts_are(TEST_COUNT - 1U, 0U, 1U),
              "netif input transfer rejects TX ownership");
    }

    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "netif input precondition paths perform no allocation");
}

static void test_accepted_ownership(void)
{
    struct hfoc_netif_input input;
    struct adapter_fixture fixture;
    struct packet_slice slice;
    struct ethernet_frame_view view;
    enum hfoc_netif_input_result result;

    CHECK(prepare_transfer(
              &input, &fixture,
              HFOC_NETIF_INPUT_ACCEPTED, true, false,
              &slice, &view),
          "netif input accepted fixture initializes");
    CHECK(hfoc_netif_input_transfer(
              &input, &slice, &view, &result) ==
              HFOC_NETIF_INPUT_OK &&
              result == HFOC_NETIF_INPUT_ACCEPTED &&
              fixture.called &&
              fixture.calls == 1U &&
              counts_are(TEST_COUNT - 1U, 0U, 1U) &&
              packet_slice_is_valid(&slice),
          "accepted transfer leaves one active packet owner");
    CHECK(adapter_saw_exact_input(&fixture, &slice, &view),
          "accepted adapter receives exact slice and view");
    CHECK(fixture.owned.slot == slice.slot.slot &&
              fixture.owned.generation == slice.slot.generation,
          "accepted adapter stores exact packet descriptor");
    CHECK(packet_pool_release(
              &pool, &fixture.owned) == PACKET_POOL_OK &&
              counts_are(TEST_COUNT, 0U, 0U),
          "accepted adapter release returns packet to pool once");
    CHECK(packet_pool_release(
              &pool, &fixture.owned) ==
              PACKET_POOL_DOUBLE_FREE &&
              counts_are(TEST_COUNT, 0U, 0U),
          "accepted adapter double release is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "accepted path performs no allocation");
}

static void test_reclaim_results(void)
{
    static const enum hfoc_netif_input_result invalid_verdict =
        (enum hfoc_netif_input_result)99U;
    struct hfoc_netif_input input;
    struct adapter_fixture fixture;
    struct packet_slice slice;
    struct ethernet_frame_view view;
    enum hfoc_netif_input_result result;
    struct packet_pool_slot_descriptor current;

    CHECK(prepare_transfer(
              &input, &fixture, invalid_verdict,
              false, false, &slice, &view),
          "netif input reclaim fixture initializes");
    CHECK(hfoc_netif_input_transfer(
              &input, &slice, &view, &result) ==
              HFOC_NETIF_INPUT_OK &&
              result == HFOC_NETIF_INPUT_REJECTED &&
              fixture.called &&
              fixture.calls == 1U &&
              counts_are(TEST_COUNT, 0U, 0U) &&
              !packet_slice_is_valid(&slice),
          "undefined verdict is reclaimed and reported rejected");
    CHECK(packet_pool_slot(
              &pool, slice.slot.slot, &current) ==
              PACKET_POOL_OK &&
              current.owner == PACKET_POOL_OWNER_FREE,
          "reclaimed packet slot is free in pool");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "reclaim path performs no allocation");

    {
        struct hfoc_netif_input input;
        struct adapter_fixture fixture;
        struct packet_slice slice;
        struct ethernet_frame_view view;
        enum hfoc_netif_input_result result;

        CHECK(prepare_transfer(
                  &input, &fixture,
                  HFOC_NETIF_INPUT_BACKPRESSURE, false, false,
                  &slice, &view),
              "netif input backpressure fixture initializes");
        CHECK(hfoc_netif_input_transfer(
                  &input, &slice, &view, &result) ==
                  HFOC_NETIF_INPUT_OK &&
                  result == HFOC_NETIF_INPUT_BACKPRESSURE &&
                  counts_are(TEST_COUNT, 0U, 0U),
              "backpressure result is explicit and reclaimed");
    }

    {
        struct hfoc_netif_input input;
        struct adapter_fixture fixture;
        struct packet_slice slice;
        struct ethernet_frame_view view;
        enum hfoc_netif_input_result result;

        CHECK(prepare_transfer(
                  &input, &fixture,
                  HFOC_NETIF_INPUT_REJECTED, false, false,
                  &slice, &view),
              "netif input rejected fixture initializes");
        CHECK(hfoc_netif_input_transfer(
                  &input, &slice, &view, &result) ==
                  HFOC_NETIF_INPUT_OK &&
                  result == HFOC_NETIF_INPUT_REJECTED &&
                  counts_are(TEST_COUNT, 0U, 0U),
              "rejected result is explicit and reclaimed");
    }
}

static void test_release_failure(void)
{
    struct hfoc_netif_input input;
    struct adapter_fixture fixture;
    struct packet_slice slice;
    struct ethernet_frame_view view;
    enum hfoc_netif_input_result result;

    CHECK(prepare_transfer(
              &input, &fixture,
              HFOC_NETIF_INPUT_REJECTED, false, true,
              &slice, &view),
          "netif input release-failure fixture initializes");
    CHECK(hfoc_netif_input_transfer(
              &input, &slice, &view, &result) ==
              HFOC_NETIF_INPUT_RELEASE_FAILED &&
              result == HFOC_NETIF_INPUT_REJECTED &&
              fixture.called &&
              counts_are(TEST_COUNT, 0U, 0U) &&
              !packet_slice_is_valid(&slice),
          "release failure reports rejected and leaves no active owner");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "release-failure path performs no allocation");
}

int main(void)
{
    test_status_strings();
    test_init_contracts();
    test_transfer_preconditions();
    test_accepted_ownership();
    test_reclaim_results();
    test_release_failure();

    if (failures != 0) {
        fprintf(stderr,
                "HFOC netif input host tests: FAILED\n");
        return 1;
    }
    printf("HFOC netif input host tests: ALL OK\n");
    return 0;
}

#else
#error "HFOC_NETIF_INPUT_HOST_TEST must be defined"
#endif
