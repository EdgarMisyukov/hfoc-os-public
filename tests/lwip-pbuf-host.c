#include "hfoc_lwip_pbuf.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_LWIP_PBUF_HOST_TEST

#define TEST_COUNT 16U
#define TEST_CAPACITY 2048U
#define TEST_HEADROOM 16U
#define TEST_LENGTH 2048U
#define TEST_STORAGE_SIZE \
    ((size_t)TEST_COUNT * (TEST_CAPACITY + TEST_HEADROOM))

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

static bool init_bridge(struct hfoc_lwip_pbuf_bridge *bridge)
{
    memset(bridge, 0, sizeof(*bridge));
    return hfoc_lwip_pbuf_bridge_init(
               bridge, &pool) == HFOC_LWIP_PBUF_OK;
}

static uint8_t *slot_payload(const struct packet_slice *slice)
{
    return slice->pool->storage +
        (size_t)slice->slot.slot *
            ((size_t)slice->pool->headroom +
             (size_t)slice->pool->capacity) +
        (size_t)slice->start;
}

static bool make_slice(
    struct packet_slice *slice,
    enum packet_pool_direction direction,
    uint32_t length,
    uint8_t seed)
{
    struct packet_pool_slot_descriptor descriptor;
    uint32_t index;
    uint8_t byte;

    if (packet_pool_reserve(
            &pool, direction, &descriptor) != PACKET_POOL_OK) {
        return false;
    }
    if (packet_pool_commit(
            &pool, &descriptor) != PACKET_POOL_OK) {
        return false;
    }
    if (packet_slice_init(
            slice, &pool, &descriptor,
            length) != PACKET_SLICE_OK) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        byte = (uint8_t)((index * 7U + seed + slice->slot.slot) & 0xFFU);
        if (packet_slice_write(slice, &byte, 1U) != PACKET_SLICE_OK) {
            return false;
        }
    }
    return packet_slice_seek(slice, 0U) == PACKET_SLICE_OK;
}

static bool make_rx_slice(
    struct packet_slice *slice,
    uint32_t length,
    uint8_t seed)
{
    return make_slice(
        slice, PACKET_POOL_DIRECTION_RX, length, seed);
}

static bool make_tx_slice(
    struct packet_slice *slice,
    uint32_t length,
    uint8_t seed)
{
    return make_slice(
        slice, PACKET_POOL_DIRECTION_TX, length, seed);
}

static bool import_ok(
    struct hfoc_lwip_pbuf_bridge *bridge,
    struct packet_slice *slice,
    struct hfoc_lwip_pbuf_chain *chain)
{
    enum hfoc_lwip_pbuf_result result =
        HFOC_LWIP_PBUF_REJECTED;

    return hfoc_lwip_pbuf_import_rx(
               bridge, slice, chain, &result) ==
               HFOC_LWIP_PBUF_OK &&
           result == HFOC_LWIP_PBUF_ACCEPTED;
}

static bool export_ok(
    const struct hfoc_lwip_pbuf_bridge *bridge,
    const struct pbuf *chain,
    struct packet_slice *slice)
{
    enum hfoc_lwip_pbuf_result result =
        HFOC_LWIP_PBUF_REJECTED;

    return hfoc_lwip_pbuf_export_tx(
               bridge, chain, slice, &result) ==
               HFOC_LWIP_PBUF_OK &&
           result == HFOC_LWIP_PBUF_ACCEPTED;
}

static bool slice_bytes_equal(
    struct packet_slice *slice,
    const uint8_t *expected,
    uint32_t length)
{
    uint8_t actual[256U];
    uint32_t index;
    uint32_t chunk;

    if (packet_slice_seek(slice, 0U) != PACKET_SLICE_OK) {
        return false;
    }
    for (index = 0U; index < length;) {
        chunk = length - index;
        if (chunk > sizeof(actual)) {
            chunk = (uint32_t)sizeof(actual);
        }
        if (packet_slice_read(
                slice, actual, chunk) != PACKET_SLICE_OK) {
            return false;
        }
        if (memcmp(actual, &expected[index], chunk) != 0) {
            return false;
        }
        index += chunk;
    }
    return true;
}

static void init_pbuf(
    struct pbuf *pbuf,
    void *payload,
    uint16_t len,
    uint16_t tot_len,
    struct pbuf *next)
{
    memset(pbuf, 0, sizeof(*pbuf));
    pbuf->next = next;
    pbuf->payload = payload;
    pbuf->tot_len = tot_len;
    pbuf->len = len;
    pbuf->type_internal = (uint8_t)PBUF_REF;
    pbuf->ref = 1U;
}

static void test_status_strings(void)
{
    static const struct {
        enum hfoc_lwip_pbuf_result result;
        const char *string;
    } results[] = {
        {HFOC_LWIP_PBUF_ACCEPTED, "HFOC_LWIP_PBUF_ACCEPTED"},
        {HFOC_LWIP_PBUF_BACKPRESSURE,
         "HFOC_LWIP_PBUF_BACKPRESSURE"},
        {HFOC_LWIP_PBUF_REJECTED, "HFOC_LWIP_PBUF_REJECTED"},
    };
    static const struct {
        enum hfoc_lwip_pbuf_status status;
        const char *string;
    } statuses[] = {
        {HFOC_LWIP_PBUF_OK, "HFOC_LWIP_PBUF_OK"},
        {HFOC_LWIP_PBUF_NULL, "HFOC_LWIP_PBUF_NULL"},
        {HFOC_LWIP_PBUF_BAD_STATE, "HFOC_LWIP_PBUF_BAD_STATE"},
        {HFOC_LWIP_PBUF_INVALID_VERSION,
         "HFOC_LWIP_PBUF_INVALID_VERSION"},
        {HFOC_LWIP_PBUF_ALREADY_PUBLISHED,
         "HFOC_LWIP_PBUF_ALREADY_PUBLISHED"},
        {HFOC_LWIP_PBUF_UNPUBLISHED,
         "HFOC_LWIP_PBUF_UNPUBLISHED"},
        {HFOC_LWIP_PBUF_BAD_POOL, "HFOC_LWIP_PBUF_BAD_POOL"},
        {HFOC_LWIP_PBUF_BAD_SLICE, "HFOC_LWIP_PBUF_BAD_SLICE"},
        {HFOC_LWIP_PBUF_WRONG_DIRECTION,
         "HFOC_LWIP_PBUF_WRONG_DIRECTION"},
        {HFOC_LWIP_PBUF_STALE_DESCRIPTOR,
         "HFOC_LWIP_PBUF_STALE_DESCRIPTOR"},
        {HFOC_LWIP_PBUF_INVALID_LENGTH,
         "HFOC_LWIP_PBUF_INVALID_LENGTH"},
        {HFOC_LWIP_PBUF_INVALID_CHAIN,
         "HFOC_LWIP_PBUF_INVALID_CHAIN"},
        {HFOC_LWIP_PBUF_EXHAUSTED, "HFOC_LWIP_PBUF_EXHAUSTED"},
        {HFOC_LWIP_PBUF_LIVE_CHAIN, "HFOC_LWIP_PBUF_LIVE_CHAIN"},
        {HFOC_LWIP_PBUF_STALE_CHAIN, "HFOC_LWIP_PBUF_STALE_CHAIN"},
        {HFOC_LWIP_PBUF_COPY_FAILED, "HFOC_LWIP_PBUF_COPY_FAILED"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(results) / sizeof(results[0]);
         ++index) {
        const char *string = hfoc_lwip_pbuf_result_string(
            results[index].result);

        CHECK(string != NULL &&
                  strcmp(string, results[index].string) == 0,
              "pbuf bridge result string is exact");
    }
    CHECK(hfoc_lwip_pbuf_result_string(
              (enum hfoc_lwip_pbuf_result)3U) == NULL,
          "pbuf bridge undefined result string is rejected");
    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = hfoc_lwip_pbuf_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "pbuf bridge status string is exact");
    }
    CHECK(hfoc_lwip_pbuf_status_string(
              (enum hfoc_lwip_pbuf_status)16U) == NULL,
          "pbuf bridge undefined status string is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "pbuf bridge status handling performs no allocation");
}

static void test_init_contracts(void)
{
    struct hfoc_lwip_pbuf_bridge bridge;
    struct packet_pool_descriptor invalid_pool;

    memset(&bridge, 0, sizeof(bridge));
    CHECK(init_pool(), "pbuf bridge init fixture initializes pool");
    CHECK(hfoc_lwip_pbuf_bridge_init(
              NULL, &pool) == HFOC_LWIP_PBUF_NULL,
          "pbuf bridge init rejects null bridge");
    CHECK(hfoc_lwip_pbuf_bridge_init(
              &bridge, NULL) == HFOC_LWIP_PBUF_BAD_POOL,
          "pbuf bridge init rejects null pool");
    CHECK(hfoc_lwip_pbuf_bridge_init(
              &bridge, &invalid_pool) == HFOC_LWIP_PBUF_BAD_POOL,
          "pbuf bridge init rejects unpublished pool");
    CHECK(init_bridge(&bridge),
          "pbuf bridge init publishes fixed state");
    CHECK(hfoc_lwip_pbuf_bridge_published(&bridge),
          "pbuf bridge is published after init");
    CHECK(hfoc_lwip_pbuf_bridge_used_count(&bridge) == 0U &&
              hfoc_lwip_pbuf_bridge_free_count(&bridge) ==
                  HFOC_LWIP_PBUF_SLOT_COUNT,
          "pbuf bridge init reports exact free/used counts");
    CHECK(hfoc_lwip_pbuf_bridge_init(
              &bridge, &pool) ==
              HFOC_LWIP_PBUF_ALREADY_PUBLISHED,
          "pbuf bridge duplicate init is rejected");
    CHECK(hfoc_lwip_pbuf_bridge_shutdown(&bridge) ==
              HFOC_LWIP_PBUF_OK,
          "empty pbuf bridge shuts down");
    CHECK(!hfoc_lwip_pbuf_bridge_published(&bridge),
          "pbuf bridge is unpublished after shutdown");
}

static void test_import_acceptance_and_duplicate_rejection(void)
{
    struct hfoc_lwip_pbuf_bridge bridge;
    struct hfoc_lwip_pbuf_chain chain;
    struct hfoc_lwip_pbuf_chain duplicate;
    struct packet_slice slice;
    struct packet_pool_slot_descriptor current;
    struct pbuf *pbuf;
    enum hfoc_lwip_pbuf_result result;
    uint32_t free_before;

    CHECK(init_pool(), "import fixture initializes pool");
    CHECK(init_bridge(&bridge), "import fixture initializes bridge");
    CHECK(make_rx_slice(&slice, 64U, 0x11U),
          "import fixture creates RX packet");
    free_before = packet_pool_free_count(&pool);
    CHECK(import_ok(&bridge, &slice, &chain),
          "RX packet imports into custom pbuf");
    CHECK(hfoc_lwip_pbuf_bridge_used_count(&bridge) == 1U &&
              hfoc_lwip_pbuf_bridge_free_count(&bridge) ==
                  HFOC_LWIP_PBUF_SLOT_COUNT - 1U,
          "import publishes one used pbuf slot");
    CHECK(chain.published && chain.head != NULL,
          "imported chain is published with head pbuf");
    pbuf = chain.head;
    CHECK(pbuf->payload == slot_payload(&slice),
          "imported pbuf points into packet storage without copy");
    CHECK(pbuf->len == 64U && pbuf->tot_len == 64U &&
              pbuf->ref == 1U && pbuf->next == NULL,
          "imported pbuf has exact length, total, ref, and no tail");
    CHECK(pbuf_match_allocsrc(pbuf, PBUF_REF),
          "imported pbuf uses permitted PBUF_REF allocation source");
    CHECK((pbuf->flags & PBUF_FLAG_IS_CUSTOM) != 0U,
          "imported pbuf carries custom-free flag");
    CHECK(packet_pool_free_count(&pool) == free_before &&
              packet_pool_active_count(&pool) == 1U,
          "import leaves packet pool ownership active");

    memset(&duplicate, 0, sizeof(duplicate));
    result = HFOC_LWIP_PBUF_REJECTED;
    CHECK(hfoc_lwip_pbuf_import_rx(
              &bridge, &slice, &duplicate, &result) ==
              HFOC_LWIP_PBUF_OK &&
              result == HFOC_LWIP_PBUF_REJECTED,
          "duplicate import of owned packet is rejected");
    CHECK(!duplicate.published &&
              hfoc_lwip_pbuf_bridge_used_count(&bridge) == 1U,
          "duplicate import leaves bridge unchanged");

    CHECK(hfoc_lwip_pbuf_release_chain(
              &bridge, &chain) == HFOC_LWIP_PBUF_OK,
          "imported chain releases packet");
    CHECK(packet_pool_slot(
              &pool, slice.slot.slot, &current) == PACKET_POOL_OK &&
              current.owner == PACKET_POOL_OWNER_FREE &&
              current.generation == slice.slot.generation + 1U,
          "release returns packet to pool and advances generation");
    CHECK(!chain.published,
          "released chain is invalidated");
    CHECK(hfoc_lwip_pbuf_release_chain(
              &bridge, &chain) == HFOC_LWIP_PBUF_STALE_CHAIN,
          "stale chain release is rejected");
}

static void test_import_rejection_fail_closed(void)
{
    struct hfoc_lwip_pbuf_bridge bridge;
    struct hfoc_lwip_pbuf_chain chain;
    struct packet_slice rx_slice;
    struct packet_slice tx_slice;
    struct packet_slice zero_slice;
    enum hfoc_lwip_pbuf_result result;
    uint32_t free_before;

    CHECK(init_pool(), "rejection fixture initializes pool");
    CHECK(init_bridge(&bridge), "rejection fixture initializes bridge");
    CHECK(make_rx_slice(&rx_slice, 32U, 0x22U),
          "rejection fixture creates RX packet");
    CHECK(make_tx_slice(&tx_slice, 32U, 0x33U),
          "rejection fixture creates TX packet");
    CHECK(make_rx_slice(&zero_slice, 0U, 0x44U),
          "rejection fixture creates zero-length RX packet");

    free_before = packet_pool_free_count(&pool);
    memset(&chain, 0, sizeof(chain));
    result = HFOC_LWIP_PBUF_ACCEPTED;
    CHECK(hfoc_lwip_pbuf_import_rx(
              &bridge, &tx_slice, &chain, &result) ==
              HFOC_LWIP_PBUF_OK &&
              result == HFOC_LWIP_PBUF_REJECTED,
          "TX packet import is rejected");
    CHECK(!chain.published, "rejected TX import publishes no chain");
    memset(&chain, 0, sizeof(chain));
    result = HFOC_LWIP_PBUF_ACCEPTED;
    CHECK(hfoc_lwip_pbuf_import_rx(
              &bridge, &zero_slice, &chain, &result) ==
              HFOC_LWIP_PBUF_OK &&
              result == HFOC_LWIP_PBUF_REJECTED,
          "zero-length RX packet import is rejected");
    CHECK(!chain.published, "rejected zero import publishes no chain");
    CHECK(packet_pool_free_count(&pool) == free_before &&
              packet_pool_active_count(&pool) == 3U,
          "rejected imports do not mutate packet ownership");
}

static void test_backpressure_and_slot_reuse(void)
{
    struct hfoc_lwip_pbuf_bridge bridge;
    struct packet_slice slices[HFOC_LWIP_PBUF_SLOT_COUNT];
    struct hfoc_lwip_pbuf_chain chains[HFOC_LWIP_PBUF_SLOT_COUNT];
    struct hfoc_lwip_pbuf_chain spare_chain;
    struct packet_slice spare_slice;
    enum hfoc_lwip_pbuf_result result;
    uint32_t first_generation;
    size_t index;

    CHECK(init_pool(), "backpressure fixture initializes pool");
    CHECK(init_bridge(&bridge), "backpressure fixture initializes bridge");
    for (index = 0U; index < HFOC_LWIP_PBUF_SLOT_COUNT; ++index) {
        CHECK(make_rx_slice(
                  &slices[index], 16U,
                  (uint8_t)index) &&
                  import_ok(
                      &bridge, &slices[index],
                      &chains[index]),
              "bridge imports packets until slot exhaustion");
    }
    CHECK(make_rx_slice(&spare_slice, 16U, 0x55U),
          "backpressure fixture creates spare packet");
    memset(&spare_chain, 0, sizeof(spare_chain));
    result = HFOC_LWIP_PBUF_ACCEPTED;
    CHECK(hfoc_lwip_pbuf_import_rx(
              &bridge, &spare_slice, &spare_chain, &result) ==
              HFOC_LWIP_PBUF_OK &&
              result == HFOC_LWIP_PBUF_BACKPRESSURE,
          "ninth import returns backpressure");
    CHECK(!spare_chain.published &&
              packet_pool_active_count(&pool) == 9U,
          "backpressure keeps spare packet caller-owned");
    first_generation = chains[0].slot_generation;
    CHECK(hfoc_lwip_pbuf_release_chain(
              &bridge, &chains[0]) == HFOC_LWIP_PBUF_OK,
          "one exhausted bridge slot releases");
    CHECK(hfoc_lwip_pbuf_bridge_free_count(&bridge) == 1U,
          "release restores one bridge slot");

    CHECK(import_ok(&bridge, &spare_slice, &spare_chain),
          "freed bridge slot accepts spare packet");
    CHECK(spare_chain.slot == chains[0].slot &&
              spare_chain.slot_generation ==
                  first_generation + 1U,
          "freed bridge slot reuses with a new generation");
    for (index = 1U; index < HFOC_LWIP_PBUF_SLOT_COUNT; ++index) {
        CHECK(hfoc_lwip_pbuf_release_chain(
                  &bridge, &chains[index]) == HFOC_LWIP_PBUF_OK,
              "remaining imported chains release");
    }
    CHECK(hfoc_lwip_pbuf_release_chain(
              &bridge, &spare_chain) == HFOC_LWIP_PBUF_OK,
          "spare chain releases");
}

static void test_export_chained_output(void)
{
    struct hfoc_lwip_pbuf_bridge bridge;
    struct packet_slice slice;
    struct pbuf pbufs[4];
    uint8_t part1[20U];
    uint8_t part2[30U];
    uint8_t part3[10U];
    uint8_t part4[4U];
    uint8_t expected[64U];
    uint16_t total = 64U;

    CHECK(init_pool(), "export fixture initializes pool");
    CHECK(init_bridge(&bridge), "export fixture initializes bridge");
    memset(part1, 0x41, sizeof(part1));
    memset(part2, 0x42, sizeof(part2));
    memset(part3, 0x43, sizeof(part3));
    memset(part4, 0x44, sizeof(part4));
    memcpy(expected, part1, sizeof(part1));
    memcpy(&expected[sizeof(part1)], part2, sizeof(part2));
    memcpy(&expected[sizeof(part1) + sizeof(part2)],
           part3, sizeof(part3));
    memcpy(&expected[sizeof(part1) + sizeof(part2) + sizeof(part3)],
           part4, sizeof(part4));

    init_pbuf(&pbufs[0], part1, 20U, total, &pbufs[1]);
    init_pbuf(&pbufs[1], part2, 30U, 44U, &pbufs[2]);
    init_pbuf(&pbufs[2], part3, 10U, 14U, &pbufs[3]);
    init_pbuf(&pbufs[3], part4, 4U, 4U, NULL);
    CHECK(make_tx_slice(&slice, total, 0x77U),
          "export fixture creates TX packet");
    CHECK(export_ok(&bridge, &pbufs[0], &slice),
          "four-node pbuf chain exports into one TX packet");
    CHECK(slice_bytes_equal(&slice, expected, total),
          "exported TX packet contains exact chained bytes");
}

static void test_export_rejection_rollback(void)
{
    struct hfoc_lwip_pbuf_bridge bridge;
    struct packet_slice slice;
    struct pbuf pbufs[5];
    uint8_t data[16U];
    uint8_t original[32U];
    uint8_t expected[32U];
    enum hfoc_lwip_pbuf_result result;
    size_t index;

    CHECK(init_pool(), "export rejection fixture initializes pool");
    CHECK(init_bridge(&bridge), "export rejection fixture initializes bridge");
    memset(data, 0x66, sizeof(data));
    CHECK(make_tx_slice(&slice, 32U, 0x88U),
          "export rejection fixture creates TX packet");
    CHECK(packet_slice_seek(&slice, 0U) == PACKET_SLICE_OK &&
              packet_slice_read(
                  &slice, original, sizeof(original)) ==
                  PACKET_SLICE_OK,
          "export rejection fixture captures original bytes");
    memcpy(expected, original, sizeof(expected));

    init_pbuf(&pbufs[0], data, 16U, 64U, NULL);
    result = HFOC_LWIP_PBUF_ACCEPTED;
    CHECK(hfoc_lwip_pbuf_export_tx(
              &bridge, &pbufs[0], &slice, &result) ==
              HFOC_LWIP_PBUF_OK &&
              result == HFOC_LWIP_PBUF_REJECTED,
          "overlong chain export is rejected");

    init_pbuf(&pbufs[0], data, 16U, 16U, NULL);
    init_pbuf(&pbufs[1], data, 16U, 32U, NULL);
    pbufs[0].tot_len = 16U;
    pbufs[0].next = &pbufs[1];
    result = HFOC_LWIP_PBUF_ACCEPTED;
    CHECK(hfoc_lwip_pbuf_export_tx(
              &bridge, &pbufs[0], &slice, &result) ==
              HFOC_LWIP_PBUF_OK &&
              result == HFOC_LWIP_PBUF_REJECTED,
          "invalid chain total export is rejected");

    init_pbuf(&pbufs[0], NULL, 16U, 16U, NULL);
    result = HFOC_LWIP_PBUF_ACCEPTED;
    CHECK(hfoc_lwip_pbuf_export_tx(
              &bridge, &pbufs[0], &slice, &result) ==
              HFOC_LWIP_PBUF_OK &&
              result == HFOC_LWIP_PBUF_REJECTED,
          "null payload chain export is rejected");

    init_pbuf(&pbufs[0], NULL, 0U, 0U, NULL);
    result = HFOC_LWIP_PBUF_ACCEPTED;
    CHECK(hfoc_lwip_pbuf_export_tx(
              &bridge, &pbufs[0], &slice, &result) ==
              HFOC_LWIP_PBUF_OK &&
              result == HFOC_LWIP_PBUF_REJECTED,
          "empty chain export is rejected");

    for (index = 0U; index < 5U; ++index) {
        init_pbuf(
            &pbufs[index], data, 4U,
            (uint16_t)(4U * (5U - index)),
            index + 1U < 5U ? &pbufs[index + 1U] : NULL);
    }
    result = HFOC_LWIP_PBUF_ACCEPTED;
    CHECK(hfoc_lwip_pbuf_export_tx(
              &bridge, &pbufs[0], &slice, &result) ==
              HFOC_LWIP_PBUF_OK &&
              result == HFOC_LWIP_PBUF_REJECTED,
          "too-many-node chain export is rejected");

    CHECK(packet_slice_seek(&slice, 0U) == PACKET_SLICE_OK &&
              packet_slice_read(
                  &slice, original, sizeof(original)) ==
                  PACKET_SLICE_OK &&
              memcmp(original, expected, sizeof(original)) == 0,
          "rejected exports leave TX packet bytes unchanged");
}

static void test_shutdown_gating(void)
{
    struct hfoc_lwip_pbuf_bridge bridge;
    struct hfoc_lwip_pbuf_chain chain;
    struct packet_slice slice;

    CHECK(init_pool(), "shutdown fixture initializes pool");
    CHECK(init_bridge(&bridge), "shutdown fixture initializes bridge");
    CHECK(make_rx_slice(&slice, 16U, 0x99U) &&
              import_ok(&bridge, &slice, &chain),
          "shutdown fixture imports one packet");
    CHECK(hfoc_lwip_pbuf_bridge_shutdown(&bridge) ==
              HFOC_LWIP_PBUF_LIVE_CHAIN,
          "shutdown with live chain is rejected");
    CHECK(hfoc_lwip_pbuf_bridge_published(&bridge) &&
              hfoc_lwip_pbuf_bridge_used_count(&bridge) == 1U,
          "rejected shutdown leaves bridge published");
    CHECK(hfoc_lwip_pbuf_release_chain(
              &bridge, &chain) == HFOC_LWIP_PBUF_OK,
          "live chain releases before shutdown");
    CHECK(hfoc_lwip_pbuf_bridge_shutdown(&bridge) ==
              HFOC_LWIP_PBUF_OK,
          "empty bridge shuts down");
    CHECK(!hfoc_lwip_pbuf_bridge_published(&bridge),
          "shutdown unpublishes bridge");
    CHECK(hfoc_lwip_pbuf_bridge_shutdown(&bridge) ==
              HFOC_LWIP_PBUF_UNPUBLISHED,
          "second shutdown is rejected");
}

int main(void)
{
    test_status_strings();
    test_init_contracts();
    test_import_acceptance_and_duplicate_rejection();
    test_import_rejection_fail_closed();
    test_backpressure_and_slot_reuse();
    test_export_chained_output();
    test_export_rejection_rollback();
    test_shutdown_gating();

    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "pbuf bridge host tests perform no allocation");
    if (failures != 0) {
        fprintf(stderr,
                "lwIP pbuf/packet ownership bridge host tests: FAIL (%d)\n",
                failures);
        return 1;
    }
    printf("lwIP pbuf/packet ownership bridge host tests: ALL OK\n");
    return 0;
}

#else

int main(void)
{
    return 0;
}

#endif
