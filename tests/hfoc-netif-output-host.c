#include "hfoc_netif_output.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_NETIF_OUTPUT_HOST_TEST

#define TEST_COUNT 2U
#define TEST_CAPACITY 2048U
#define TEST_HEADROOM 0U
#define TEST_LENGTH 2048U
#define TEST_STORAGE_SIZE (TEST_COUNT * (TEST_CAPACITY + TEST_HEADROOM))
#define TEST_SLICE_LENGTH 64U

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
    enum hfoc_netif_output_result verdict;
    bool called;
    bool invalid_submission;
    bool wrong_slot;
    bool wrong_generation;
    unsigned int calls;
    struct packet_slice slice_copy;
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

static bool init_tx_slice(struct packet_slice *slice)
{
    struct packet_pool_slot_descriptor descriptor;

    if (packet_pool_reserve(
            &pool, PACKET_POOL_DIRECTION_TX,
            &descriptor) != PACKET_POOL_OK) {
        return false;
    }
    if (packet_pool_commit(
            &pool, &descriptor) != PACKET_POOL_OK) {
        return false;
    }
    return packet_slice_init(
               slice, &pool, &descriptor,
               TEST_SLICE_LENGTH) == PACKET_SLICE_OK;
}

static bool init_rx_slice(struct packet_slice *slice)
{
    struct packet_pool_slot_descriptor descriptor;

    if (packet_pool_reserve(
            &pool, PACKET_POOL_DIRECTION_RX,
            &descriptor) != PACKET_POOL_OK) {
        return false;
    }
    if (packet_pool_commit(
            &pool, &descriptor) != PACKET_POOL_OK) {
        return false;
    }
    return packet_slice_init(
               slice, &pool, &descriptor,
               TEST_SLICE_LENGTH) == PACKET_SLICE_OK;
}

static enum hfoc_netif_output_result adapter_submit(
    void *context,
    struct packet_slice *slice,
    struct hfoc_netif_output_submission *submission)
{
    struct adapter_fixture *fixture =
        (struct adapter_fixture *)context;

    fixture->called = true;
    ++fixture->calls;
    fixture->slice_copy = *slice;
    if (fixture->verdict == HFOC_NETIF_OUTPUT_SUBMITTED) {
        if (fixture->invalid_submission) {
            memset(submission, 0, sizeof(*submission));
            return fixture->verdict;
        }
        submission->slot = fixture->wrong_slot
            ? slice->slot.slot + 1U
            : slice->slot.slot;
        submission->generation = fixture->wrong_generation
            ? slice->slot.generation + 1U
            : slice->slot.generation;
    }
    return fixture->verdict;
}

static bool init_output(
    struct hfoc_netif_output *output,
    const struct adapter_fixture *fixture)
{
    struct hfoc_netif_output_adapter adapter;

    memset(output, 0, sizeof(*output));
    adapter.submit = adapter_submit;
    adapter.context = (void *)fixture;
    return hfoc_netif_output_init(
               output, &pool, &adapter) == HFOC_NETIF_OUTPUT_OK;
}

static bool prepare_submit(
    struct hfoc_netif_output *output,
    struct adapter_fixture *fixture,
    enum hfoc_netif_output_result verdict,
    bool invalid_submission,
    bool wrong_slot,
    bool wrong_generation,
    struct packet_slice *slice)
{
    if (!init_pool()) {
        return false;
    }
    memset(fixture, 0, sizeof(*fixture));
    fixture->verdict = verdict;
    fixture->invalid_submission = invalid_submission;
    fixture->wrong_slot = wrong_slot;
    fixture->wrong_generation = wrong_generation;
    if (!init_tx_slice(slice)) {
        return false;
    }
    return init_output(output, fixture);
}

static bool adapter_saw_exact_slice(
    const struct adapter_fixture *fixture,
    const struct packet_slice *slice)
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
           fixture->slice_copy.cursor == slice->cursor;
}

static void test_status_strings(void)
{
    static const struct {
        enum hfoc_netif_output_result result;
        const char *string;
    } results[] = {
        {HFOC_NETIF_OUTPUT_SUBMITTED,
         "HFOC_NETIF_OUTPUT_SUBMITTED"},
        {HFOC_NETIF_OUTPUT_BACKPRESSURE,
         "HFOC_NETIF_OUTPUT_BACKPRESSURE"},
        {HFOC_NETIF_OUTPUT_REJECTED,
         "HFOC_NETIF_OUTPUT_REJECTED"},
    };
    static const struct {
        enum hfoc_netif_output_status status;
        const char *string;
    } statuses[] = {
        {HFOC_NETIF_OUTPUT_OK, "HFOC_NETIF_OUTPUT_OK"},
        {HFOC_NETIF_OUTPUT_NULL, "HFOC_NETIF_OUTPUT_NULL"},
        {HFOC_NETIF_OUTPUT_BAD_STATE, "HFOC_NETIF_OUTPUT_BAD_STATE"},
        {HFOC_NETIF_OUTPUT_INVALID_VERSION,
         "HFOC_NETIF_OUTPUT_INVALID_VERSION"},
        {HFOC_NETIF_OUTPUT_ALREADY_PUBLISHED,
         "HFOC_NETIF_OUTPUT_ALREADY_PUBLISHED"},
        {HFOC_NETIF_OUTPUT_UNPUBLISHED,
         "HFOC_NETIF_OUTPUT_UNPUBLISHED"},
        {HFOC_NETIF_OUTPUT_BAD_POOL, "HFOC_NETIF_OUTPUT_BAD_POOL"},
        {HFOC_NETIF_OUTPUT_INVALID_ADAPTER,
         "HFOC_NETIF_OUTPUT_INVALID_ADAPTER"},
        {HFOC_NETIF_OUTPUT_BAD_SLICE,
         "HFOC_NETIF_OUTPUT_BAD_SLICE"},
        {HFOC_NETIF_OUTPUT_WRONG_DIRECTION,
         "HFOC_NETIF_OUTPUT_WRONG_DIRECTION"},
        {HFOC_NETIF_OUTPUT_ALREADY_SUBMITTED,
         "HFOC_NETIF_OUTPUT_ALREADY_SUBMITTED"},
        {HFOC_NETIF_OUTPUT_NOT_SUBMITTED,
         "HFOC_NETIF_OUTPUT_NOT_SUBMITTED"},
        {HFOC_NETIF_OUTPUT_INVALID_SUBMISSION,
         "HFOC_NETIF_OUTPUT_INVALID_SUBMISSION"},
        {HFOC_NETIF_OUTPUT_INVALID_COMPLETION,
         "HFOC_NETIF_OUTPUT_INVALID_COMPLETION"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(results) / sizeof(results[0]);
         ++index) {
        const char *string = hfoc_netif_output_result_string(
            results[index].result);

        CHECK(string != NULL &&
                  strcmp(string, results[index].string) == 0,
              "netif output result string is exact");
    }
    CHECK(hfoc_netif_output_result_string(
              (enum hfoc_netif_output_result)3U) == NULL,
          "netif output undefined result string is rejected");
    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = hfoc_netif_output_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "netif output status string is exact");
    }
    CHECK(hfoc_netif_output_status_string(
              (enum hfoc_netif_output_status)14U) == NULL,
          "netif output undefined status string is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "netif output status handling performs no allocation");
}

static void test_init_contracts(void)
{
    struct hfoc_netif_output output;
    struct hfoc_netif_output_adapter adapter;
    struct adapter_fixture fixture;

    memset(&output, 0, sizeof(output));
    memset(&fixture, 0, sizeof(fixture));
    CHECK(init_pool(),
          "netif output init fixture initializes pool");

    adapter.submit = adapter_submit;
    adapter.context = &fixture;
    CHECK(hfoc_netif_output_init(
              NULL, &pool, &adapter) == HFOC_NETIF_OUTPUT_NULL,
          "netif output init rejects null output");
    CHECK(hfoc_netif_output_init(
              &output, NULL, &adapter) == HFOC_NETIF_OUTPUT_BAD_POOL,
          "netif output init rejects null pool");
    CHECK(hfoc_netif_output_init(
              &output, &pool, NULL) == HFOC_NETIF_OUTPUT_NULL,
          "netif output init rejects null adapter");
    adapter.submit = NULL;
    CHECK(hfoc_netif_output_init(
              &output, &pool, &adapter) ==
              HFOC_NETIF_OUTPUT_INVALID_ADAPTER,
          "netif output init rejects null submit callback");
    adapter.submit = adapter_submit;
    CHECK(hfoc_netif_output_init(
              &output, &pool, &adapter) == HFOC_NETIF_OUTPUT_OK &&
              hfoc_netif_output_published(&output),
          "netif output init publishes valid output");
    CHECK(hfoc_netif_output_init(
              &output, &pool, &adapter) ==
              HFOC_NETIF_OUTPUT_ALREADY_PUBLISHED,
          "netif output init rejects duplicate publication");
    CHECK(hfoc_netif_output_published(NULL) == false,
          "netif output published rejects null output");
    CHECK(hfoc_netif_output_pending_count(NULL) == 0U,
          "netif output pending count rejects null output");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "netif output init paths perform no allocation");
}

static void test_submit_preconditions(void)
{
    struct hfoc_netif_output output;
    struct adapter_fixture fixture;
    struct packet_slice slice;
    enum hfoc_netif_output_result result;

    CHECK(prepare_submit(
              &output, &fixture,
              HFOC_NETIF_OUTPUT_SUBMITTED,
              false, false, false, &slice),
          "netif output precondition fixture initializes");

    CHECK(hfoc_netif_output_submit(
              NULL, &slice, &result) ==
              HFOC_NETIF_OUTPUT_NULL &&
              !fixture.called,
          "netif output submit rejects null output");
    CHECK(hfoc_netif_output_submit(
              &output, NULL, &result) ==
              HFOC_NETIF_OUTPUT_NULL &&
              !fixture.called,
          "netif output submit rejects null slice");
    CHECK(hfoc_netif_output_submit(
              &output, &slice, NULL) ==
              HFOC_NETIF_OUTPUT_NULL &&
              !fixture.called,
          "netif output submit rejects null result");

    memset(&slice, 0, sizeof(slice));
    slice.version = PACKET_SLICE_VERSION;
    slice.pool = &pool;
    CHECK(hfoc_netif_output_submit(
              &output, &slice, &result) ==
              HFOC_NETIF_OUTPUT_BAD_SLICE &&
              !fixture.called,
          "netif output submit rejects invalid slice");

    CHECK(prepare_submit(
              &output, &fixture,
              HFOC_NETIF_OUTPUT_SUBMITTED,
              false, false, false, &slice),
          "netif output invalid-version fixture initializes");
    output.version = 0U;
    CHECK(hfoc_netif_output_submit(
              &output, &slice, &result) ==
              HFOC_NETIF_OUTPUT_INVALID_VERSION &&
              !fixture.called,
          "netif output submit rejects invalid version");

    CHECK(prepare_submit(
              &output, &fixture,
              HFOC_NETIF_OUTPUT_SUBMITTED,
              false, false, false, &slice),
          "netif output unpublished fixture initializes");
    memset(&output, 0, sizeof(output));
    CHECK(hfoc_netif_output_submit(
              &output, &slice, &result) ==
              HFOC_NETIF_OUTPUT_UNPUBLISHED &&
              !fixture.called,
          "netif output submit rejects unpublished output");

    CHECK(init_pool(),
          "netif output wrong-direction fixture initializes pool");
    CHECK(init_rx_slice(&slice),
          "netif output wrong-direction RX slice initializes");
    {
        struct hfoc_netif_output_adapter adapter;

        memset(&fixture, 0, sizeof(fixture));
        adapter.submit = adapter_submit;
        adapter.context = &fixture;
        CHECK(hfoc_netif_output_init(
                  &output, &pool, &adapter) ==
                  HFOC_NETIF_OUTPUT_OK &&
                  hfoc_netif_output_submit(
                      &output, &slice, &result) ==
                      HFOC_NETIF_OUTPUT_WRONG_DIRECTION &&
                  !fixture.called &&
                  counts_are(TEST_COUNT - 1U, 0U, 1U),
              "netif output submit rejects RX ownership");
    }

    CHECK(prepare_submit(
              &output, &fixture,
              HFOC_NETIF_OUTPUT_SUBMITTED,
              true, false, false, &slice),
          "netif output invalid-submission fixture initializes");
    CHECK(hfoc_netif_output_submit(
              &output, &slice, &result) ==
              HFOC_NETIF_OUTPUT_INVALID_SUBMISSION &&
              fixture.called &&
              fixture.calls == 1U &&
              hfoc_netif_output_pending_count(&output) == 0U &&
              counts_are(TEST_COUNT - 1U, 0U, 1U),
          "invalid adapter submission is rejected without pending ownership");

    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "netif output precondition paths perform no allocation");
}

static void test_submitted_completion_retry(void)
{
    struct hfoc_netif_output output;
    struct adapter_fixture fixture;
    struct packet_slice slice;
    enum hfoc_netif_output_result result;
    struct hfoc_netif_output_submission submission;

    CHECK(prepare_submit(
              &output, &fixture,
              HFOC_NETIF_OUTPUT_SUBMITTED,
              false, false, false, &slice),
          "netif output submitted fixture initializes");
    CHECK(hfoc_netif_output_submit(
              &output, &slice, &result) ==
              HFOC_NETIF_OUTPUT_OK &&
              result == HFOC_NETIF_OUTPUT_SUBMITTED &&
              fixture.called &&
              fixture.calls == 1U &&
              hfoc_netif_output_pending_count(&output) == 1U &&
              counts_are(TEST_COUNT - 1U, 0U, 1U) &&
              packet_slice_is_valid(&slice),
          "submitted transfer records one pending device owner");
    CHECK(adapter_saw_exact_slice(&fixture, &slice),
          "submitted adapter receives exact slice");

    CHECK(hfoc_netif_output_submit(
              &output, &slice, &result) ==
              HFOC_NETIF_OUTPUT_ALREADY_SUBMITTED &&
              fixture.calls == 1U &&
              hfoc_netif_output_pending_count(&output) == 1U,
          "retry before completion cannot duplicate submitted frame");

    submission.slot = slice.slot.slot;
    submission.generation = slice.slot.generation;
    CHECK(hfoc_netif_output_complete(
              &output, &submission) == HFOC_NETIF_OUTPUT_OK &&
              hfoc_netif_output_pending_count(&output) == 0U &&
              packet_slice_is_valid(&slice),
          "completion returns caller ownership and clears pending state");
    CHECK(hfoc_netif_output_complete(
              &output, &submission) ==
              HFOC_NETIF_OUTPUT_NOT_SUBMITTED,
          "completion of unknown pending state is rejected");

    CHECK(hfoc_netif_output_submit(
              &output, &slice, &result) ==
              HFOC_NETIF_OUTPUT_OK &&
              result == HFOC_NETIF_OUTPUT_SUBMITTED &&
              fixture.calls == 2U &&
              hfoc_netif_output_pending_count(&output) == 1U,
          "caller can intentionally submit again after completion");
    submission.slot = slice.slot.slot;
    submission.generation = slice.slot.generation;
    CHECK(hfoc_netif_output_complete(
              &output, &submission) == HFOC_NETIF_OUTPUT_OK &&
              hfoc_netif_output_pending_count(&output) == 0U,
          "second completion clears second submission");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "submitted/completion path performs no allocation");
}

static void test_completion_failures(void)
{
    struct hfoc_netif_output output;
    struct adapter_fixture fixture;
    struct packet_slice slice;
    enum hfoc_netif_output_result result;
    struct hfoc_netif_output_submission submission;

    CHECK(prepare_submit(
              &output, &fixture,
              HFOC_NETIF_OUTPUT_SUBMITTED,
              false, false, false, &slice),
          "netif output completion-failure fixture initializes");
    CHECK(hfoc_netif_output_submit(
              &output, &slice, &result) ==
              HFOC_NETIF_OUTPUT_OK &&
              result == HFOC_NETIF_OUTPUT_SUBMITTED,
          "netif output completion fixture submits frame");
    CHECK(hfoc_netif_output_complete(
              &output, NULL) == HFOC_NETIF_OUTPUT_NULL &&
              hfoc_netif_output_pending_count(&output) == 1U,
          "completion rejects null submission without mutation");
    CHECK(hfoc_netif_output_complete(
              NULL, &submission) == HFOC_NETIF_OUTPUT_NULL,
          "completion rejects null output");

    submission.slot = slice.slot.slot;
    submission.generation = slice.slot.generation + 1U;
    CHECK(hfoc_netif_output_complete(
              &output, &submission) ==
              HFOC_NETIF_OUTPUT_INVALID_COMPLETION &&
              hfoc_netif_output_pending_count(&output) == 1U,
          "completion rejects generation mismatch");

    submission.slot = slice.slot.slot + 1U;
    submission.generation = slice.slot.generation;
    CHECK(hfoc_netif_output_complete(
              &output, &submission) ==
              HFOC_NETIF_OUTPUT_NOT_SUBMITTED &&
              hfoc_netif_output_pending_count(&output) == 1U,
          "completion rejects unknown slot as not submitted");

    submission.slot = slice.slot.slot;
    submission.generation = slice.slot.generation;
    CHECK(hfoc_netif_output_complete(
              &output, &submission) == HFOC_NETIF_OUTPUT_OK &&
              hfoc_netif_output_pending_count(&output) == 0U,
          "exact completion clears pending after failure cases");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "completion-failure path performs no allocation");
}

static void test_backpressure_rejected_retry(void)
{
    struct hfoc_netif_output output;
    struct adapter_fixture fixture;
    struct packet_slice slice;
    enum hfoc_netif_output_result result;
    struct hfoc_netif_output_submission submission;

    CHECK(prepare_submit(
              &output, &fixture,
              HFOC_NETIF_OUTPUT_BACKPRESSURE,
              false, false, false, &slice),
          "netif output backpressure fixture initializes");
    CHECK(hfoc_netif_output_submit(
              &output, &slice, &result) ==
              HFOC_NETIF_OUTPUT_OK &&
              result == HFOC_NETIF_OUTPUT_BACKPRESSURE &&
              fixture.calls == 1U &&
              hfoc_netif_output_pending_count(&output) == 0U &&
              counts_are(TEST_COUNT - 1U, 0U, 1U) &&
              packet_slice_is_valid(&slice),
          "backpressure leaves caller ownership unchanged");
    CHECK(hfoc_netif_output_submit(
              &output, &slice, &result) ==
              HFOC_NETIF_OUTPUT_OK &&
              result == HFOC_NETIF_OUTPUT_BACKPRESSURE &&
              fixture.calls == 2U &&
              hfoc_netif_output_pending_count(&output) == 0U,
          "backpressure retry does not duplicate or submit frame");
    fixture.verdict = HFOC_NETIF_OUTPUT_SUBMITTED;
    CHECK(hfoc_netif_output_submit(
              &output, &slice, &result) ==
              HFOC_NETIF_OUTPUT_OK &&
              result == HFOC_NETIF_OUTPUT_SUBMITTED &&
              fixture.calls == 3U &&
              hfoc_netif_output_pending_count(&output) == 1U,
          "backpressure retry can later submit exactly once");
    submission.slot = slice.slot.slot;
    submission.generation = slice.slot.generation;
    CHECK(hfoc_netif_output_complete(
              &output, &submission) == HFOC_NETIF_OUTPUT_OK,
          "backpressure retry completion returns ownership");

    CHECK(prepare_submit(
              &output, &fixture,
              HFOC_NETIF_OUTPUT_REJECTED,
              false, false, false, &slice),
          "netif output rejected fixture initializes");
    CHECK(hfoc_netif_output_submit(
              &output, &slice, &result) ==
              HFOC_NETIF_OUTPUT_OK &&
              result == HFOC_NETIF_OUTPUT_REJECTED &&
              fixture.calls == 1U &&
              hfoc_netif_output_pending_count(&output) == 0U &&
              packet_slice_is_valid(&slice),
          "rejected transfer leaves caller ownership unchanged");
    CHECK(hfoc_netif_output_submit(
              &output, &slice, &result) ==
              HFOC_NETIF_OUTPUT_OK &&
              result == HFOC_NETIF_OUTPUT_REJECTED &&
              fixture.calls == 2U &&
              hfoc_netif_output_pending_count(&output) == 0U,
          "rejected retry does not duplicate or submit frame");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "backpressure/rejected retry performs no allocation");
}

static void test_undefined_verdict(void)
{
    static const enum hfoc_netif_output_result invalid_verdict =
        (enum hfoc_netif_output_result)99U;
    struct hfoc_netif_output output;
    struct adapter_fixture fixture;
    struct packet_slice slice;
    enum hfoc_netif_output_result result;

    CHECK(prepare_submit(
              &output, &fixture, invalid_verdict,
              false, false, false, &slice),
          "netif output undefined-verdict fixture initializes");
    CHECK(hfoc_netif_output_submit(
              &output, &slice, &result) ==
              HFOC_NETIF_OUTPUT_OK &&
              result == HFOC_NETIF_OUTPUT_REJECTED &&
              fixture.called &&
              fixture.calls == 1U &&
              hfoc_netif_output_pending_count(&output) == 0U &&
              packet_slice_is_valid(&slice),
          "undefined adapter verdict is reported rejected without mutation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "undefined-verdict path performs no allocation");
}

int main(void)
{
    test_status_strings();
    test_init_contracts();
    test_submit_preconditions();
    test_submitted_completion_retry();
    test_completion_failures();
    test_backpressure_rejected_retry();
    test_undefined_verdict();

    if (failures != 0) {
        fprintf(stderr,
                "HFOC netif output host tests: FAILED\n");
        return 1;
    }
    printf("HFOC netif output host tests: ALL OK\n");
    return 0;
}

#else
#error "HFOC_NETIF_OUTPUT_HOST_TEST must be defined"
#endif
