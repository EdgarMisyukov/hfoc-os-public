#include "virtio_available_publication.h"
#include "virtio_queue_layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_VIRTIO_AVAILABLE_PUBLICATION_HOST_TEST

#define TEST_QUEUE_SIZE 4U
#define TEST_RING_FILL UINT16_C(0xFFFF)

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;
static struct virtio_available_ring ring;

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

struct barrier_state {
    struct virtio_available_ring *ring;
    unsigned int count;
    uint16_t idx_before_first;
    uint16_t idx_before_second;
    uint16_t first_entry_before_first;
    uint32_t slot;
};

static void test_fence(void *context)
{
    struct barrier_state *state = (struct barrier_state *)context;

    if (state->count == 0U) {
        state->idx_before_first = state->ring->idx;
        state->first_entry_before_first =
            state->ring->ring[state->slot % TEST_QUEUE_SIZE];
    } else if (state->count == 1U) {
        state->idx_before_second = state->ring->idx;
    }
    ++state->count;
}

static void reset_ring(uint16_t idx)
{
    size_t index;

    ring.flags = 0U;
    ring.idx = idx;
    ring.used_event = 0U;
    for (index = 0U; index < 16U; ++index) {
        ring.ring[index] = TEST_RING_FILL;
    }
}

static bool make_layout(
    uint32_t queue_size,
    bool event_index,
    struct virtio_queue_layout_result *layout)
{
    struct virtio_queue_layout_policy policy;

    policy.queue_size = queue_size;
    policy.event_index = event_index;
    return virtio_queue_layout_calculate(
               &policy, 0U, UINT64_C(0x100000), layout) ==
           VIRTIO_QUEUE_LAYOUT_OK;
}

static void init_barrier(
    struct virtio_available_barrier *barrier,
    struct barrier_state *state)
{
    memset(state, 0, sizeof(*state));
    state->ring = &ring;
    barrier->fence = test_fence;
    barrier->context = state;
}

static bool init_controller(
    struct virtio_available_publication *controller,
    const struct virtio_queue_layout_result *layout,
    struct virtio_available_barrier *barrier,
    struct barrier_state *state,
    uint32_t generation)
{
    memset(controller, 0, sizeof(*controller));
    init_barrier(barrier, state);
    return virtio_available_publication_init(
               controller, layout, &ring, barrier, generation) ==
           VIRTIO_AVAILABLE_PUBLICATION_OK;
}

static bool token_valid(
    const struct virtio_available_publication *controller,
    const struct virtio_available_token *token)
{
    return token != NULL &&
           token->sequence != 0U &&
           token->generation == controller->generation;
}

static bool status_string_matches(
    enum virtio_available_publication_status status,
    const char *string)
{
    switch (status) {
    case VIRTIO_AVAILABLE_PUBLICATION_OK:
        return strcmp(
            string, "VIRTIO_AVAILABLE_PUBLICATION_OK") == 0;
    case VIRTIO_AVAILABLE_PUBLICATION_NULL:
        return strcmp(
            string, "VIRTIO_AVAILABLE_PUBLICATION_NULL") == 0;
    case VIRTIO_AVAILABLE_PUBLICATION_INVALID_VERSION:
        return strcmp(
            string, "VIRTIO_AVAILABLE_PUBLICATION_INVALID_VERSION") == 0;
    case VIRTIO_AVAILABLE_PUBLICATION_INVALID_GENERATION:
        return strcmp(
            string,
            "VIRTIO_AVAILABLE_PUBLICATION_INVALID_GENERATION") == 0;
    case VIRTIO_AVAILABLE_PUBLICATION_ALREADY_PUBLISHED:
        return strcmp(
            string,
            "VIRTIO_AVAILABLE_PUBLICATION_ALREADY_PUBLISHED") == 0;
    case VIRTIO_AVAILABLE_PUBLICATION_UNPUBLISHED:
        return strcmp(
            string, "VIRTIO_AVAILABLE_PUBLICATION_UNPUBLISHED") == 0;
    case VIRTIO_AVAILABLE_PUBLICATION_INVALID_LAYOUT:
        return strcmp(
            string, "VIRTIO_AVAILABLE_PUBLICATION_INVALID_LAYOUT") == 0;
    case VIRTIO_AVAILABLE_PUBLICATION_INVALID_BARRIER:
        return strcmp(
            string, "VIRTIO_AVAILABLE_PUBLICATION_INVALID_BARRIER") == 0;
    case VIRTIO_AVAILABLE_PUBLICATION_INVALID_QUEUE_SIZE:
        return strcmp(
            string,
            "VIRTIO_AVAILABLE_PUBLICATION_INVALID_QUEUE_SIZE") == 0;
    case VIRTIO_AVAILABLE_PUBLICATION_INVALID_CHAIN:
        return strcmp(
            string, "VIRTIO_AVAILABLE_PUBLICATION_INVALID_CHAIN") == 0;
    case VIRTIO_AVAILABLE_PUBLICATION_DUPLICATE_DESCRIPTOR:
        return strcmp(
            string,
            "VIRTIO_AVAILABLE_PUBLICATION_DUPLICATE_DESCRIPTOR") == 0;
    case VIRTIO_AVAILABLE_PUBLICATION_RING_FULL:
        return strcmp(
            string, "VIRTIO_AVAILABLE_PUBLICATION_RING_FULL") == 0;
    case VIRTIO_AVAILABLE_PUBLICATION_PENDING_FULL:
        return strcmp(
            string, "VIRTIO_AVAILABLE_PUBLICATION_PENDING_FULL") == 0;
    case VIRTIO_AVAILABLE_PUBLICATION_NO_PENDING:
        return strcmp(
            string, "VIRTIO_AVAILABLE_PUBLICATION_NO_PENDING") == 0;
    case VIRTIO_AVAILABLE_PUBLICATION_STALE_TOKEN:
        return strcmp(
            string, "VIRTIO_AVAILABLE_PUBLICATION_STALE_TOKEN") == 0;
    case VIRTIO_AVAILABLE_PUBLICATION_INDEX_CHANGED:
        return strcmp(
            string, "VIRTIO_AVAILABLE_PUBLICATION_INDEX_CHANGED") == 0;
    case VIRTIO_AVAILABLE_PUBLICATION_INVALID_STATE:
        return strcmp(
            string, "VIRTIO_AVAILABLE_PUBLICATION_INVALID_STATE") == 0;
    default:
        return false;
    }
}

static void test_status_strings(void)
{
    static const enum virtio_available_publication_status statuses[] = {
        VIRTIO_AVAILABLE_PUBLICATION_OK,
        VIRTIO_AVAILABLE_PUBLICATION_NULL,
        VIRTIO_AVAILABLE_PUBLICATION_INVALID_VERSION,
        VIRTIO_AVAILABLE_PUBLICATION_INVALID_GENERATION,
        VIRTIO_AVAILABLE_PUBLICATION_ALREADY_PUBLISHED,
        VIRTIO_AVAILABLE_PUBLICATION_UNPUBLISHED,
        VIRTIO_AVAILABLE_PUBLICATION_INVALID_LAYOUT,
        VIRTIO_AVAILABLE_PUBLICATION_INVALID_BARRIER,
        VIRTIO_AVAILABLE_PUBLICATION_INVALID_QUEUE_SIZE,
        VIRTIO_AVAILABLE_PUBLICATION_INVALID_CHAIN,
        VIRTIO_AVAILABLE_PUBLICATION_DUPLICATE_DESCRIPTOR,
        VIRTIO_AVAILABLE_PUBLICATION_RING_FULL,
        VIRTIO_AVAILABLE_PUBLICATION_PENDING_FULL,
        VIRTIO_AVAILABLE_PUBLICATION_NO_PENDING,
        VIRTIO_AVAILABLE_PUBLICATION_STALE_TOKEN,
        VIRTIO_AVAILABLE_PUBLICATION_INDEX_CHANGED,
        VIRTIO_AVAILABLE_PUBLICATION_INVALID_STATE,
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        enum virtio_available_publication_status status = statuses[index];
        const char *string =
            virtio_available_publication_status_string(status);

        CHECK(string != NULL &&
                  status_string_matches(status, string),
              "virtio available publication status string is exact");
    }
    CHECK(virtio_available_publication_status_string(
              (enum virtio_available_publication_status)17U) == NULL,
          "virtio available publication undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio available publication status handling performs no allocation");
}

static void test_init_and_null(void)
{
    struct virtio_available_publication controller;
    struct virtio_queue_layout_result layout;
    struct virtio_available_barrier barrier;
    struct barrier_state state;
    struct virtio_available_token token;
    struct virtio_available_barrier bad_barrier;
    const uint16_t invalid_state_chain[1] = {0U};

    reset_ring(0U);
    CHECK(make_layout(TEST_QUEUE_SIZE, false, &layout),
          "init fixture publishes valid queue layout");

    CHECK(virtio_available_publication_init(
              NULL, &layout, &ring, &barrier, 1U) ==
              VIRTIO_AVAILABLE_PUBLICATION_NULL,
          "available publication init rejects null controller");
    CHECK(virtio_available_publication_init(
              &controller, NULL, &ring, &barrier, 1U) ==
              VIRTIO_AVAILABLE_PUBLICATION_NULL,
          "available publication init rejects null layout");
    CHECK(virtio_available_publication_init(
              &controller, &layout, NULL, &barrier, 1U) ==
              VIRTIO_AVAILABLE_PUBLICATION_NULL,
          "available publication init rejects null ring");
    CHECK(virtio_available_publication_init(
              &controller, &layout, &ring, NULL, 1U) ==
              VIRTIO_AVAILABLE_PUBLICATION_NULL,
          "available publication init rejects null barrier");

    init_barrier(&barrier, &state);
    bad_barrier = barrier;
    bad_barrier.fence = NULL;
    CHECK(virtio_available_publication_init(
              &controller, &layout, &ring, &bad_barrier, 1U) ==
              VIRTIO_AVAILABLE_PUBLICATION_INVALID_BARRIER,
          "available publication init rejects null fence callback");
    bad_barrier = barrier;
    bad_barrier.context = NULL;
    CHECK(virtio_available_publication_init(
              &controller, &layout, &ring, &bad_barrier, 1U) ==
              VIRTIO_AVAILABLE_PUBLICATION_INVALID_BARRIER,
          "available publication init rejects null barrier context");

    layout.published = false;
    CHECK(virtio_available_publication_init(
              &controller, &layout, &ring, &barrier, 1U) ==
              VIRTIO_AVAILABLE_PUBLICATION_INVALID_LAYOUT,
          "available publication init rejects unpublished layout");
    layout.published = true;
    CHECK(virtio_available_publication_init(
              &controller, &layout, &ring, &barrier, 0U) ==
              VIRTIO_AVAILABLE_PUBLICATION_INVALID_GENERATION,
          "available publication init rejects zero generation");
    CHECK(virtio_available_publication_init(
              &controller, &layout, &ring, &barrier,
              VIRTIO_AVAILABLE_PUBLICATION_GENERATION_MAX + 1U) ==
              VIRTIO_AVAILABLE_PUBLICATION_INVALID_GENERATION,
          "available publication init rejects over-limit generation");

    CHECK(init_controller(
              &controller, &layout, &barrier, &state, 5U),
          "available publication init publishes bounded controller");
    CHECK(virtio_available_publication_published(&controller) &&
              controller.version == VIRTIO_AVAILABLE_PUBLICATION_VERSION &&
              controller.generation == 5U &&
              controller.queue_size == TEST_QUEUE_SIZE &&
              controller.expected_idx == ring.idx &&
              controller.published_count == 0U &&
              !controller.pending &&
              virtio_available_publication_visible_index(&controller) ==
                  ring.idx,
          "available publication init publishes exact controller state");
    CHECK(state.count == 0U,
          "available publication init performs no fence");
    CHECK(ring.ring[0] == TEST_RING_FILL &&
              ring.ring[1] == TEST_RING_FILL,
          "available publication init does not write ring entries");
    CHECK(virtio_available_publication_init(
              &controller, &layout, &ring, &barrier, 5U) ==
              VIRTIO_AVAILABLE_PUBLICATION_ALREADY_PUBLISHED,
          "available publication init rejects duplicate publication");

    memset(&controller, 0, sizeof(controller));
    controller.version = VIRTIO_AVAILABLE_PUBLICATION_VERSION;
    controller.generation = 5U;
    controller.queue_size = TEST_QUEUE_SIZE;
    controller.published_count = 0U;
    controller.ring = &ring;
    controller.barrier = barrier;
    CHECK(virtio_available_publication_prepare(
              &controller, invalid_state_chain, 1U, &token) ==
              VIRTIO_AVAILABLE_PUBLICATION_UNPUBLISHED,
          "unpublished controller rejects prepare");

    controller.published = true;
    controller.version = VIRTIO_AVAILABLE_PUBLICATION_VERSION + 1U;
    CHECK(virtio_available_publication_prepare(
              &controller, invalid_state_chain, 1U, &token) ==
              VIRTIO_AVAILABLE_PUBLICATION_INVALID_VERSION,
          "invalid controller version is rejected");

    controller.version = VIRTIO_AVAILABLE_PUBLICATION_VERSION;
    controller.queue_size = 0U;
    CHECK(virtio_available_publication_prepare(
              &controller, invalid_state_chain, 1U, &token) ==
              VIRTIO_AVAILABLE_PUBLICATION_INVALID_QUEUE_SIZE,
          "invalid queue size is rejected");

    controller.queue_size = TEST_QUEUE_SIZE;
    controller.barrier.fence = NULL;
    CHECK(virtio_available_publication_prepare(
              &controller, invalid_state_chain, 1U, &token) ==
              VIRTIO_AVAILABLE_PUBLICATION_INVALID_BARRIER,
          "invalid barrier is rejected");

    controller.barrier.fence = test_fence;
    controller.published_count = controller.queue_size + 1U;
    CHECK(virtio_available_publication_prepare(
              &controller, invalid_state_chain, 1U, &token) ==
              VIRTIO_AVAILABLE_PUBLICATION_INVALID_STATE,
          "invalid internal count is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "available publication init and null checks perform no allocation");
}

static void test_successful_publication(void)
{
    struct virtio_available_publication controller;
    struct virtio_queue_layout_result layout;
    struct virtio_available_barrier barrier;
    struct barrier_state state;
    struct virtio_available_token first;
    struct virtio_available_token second;
    struct virtio_available_token pending;
    const uint16_t chain_three[3] = {0U, 1U, 2U};
    const uint16_t chain_one[1] = {3U};

    reset_ring(0U);
    CHECK(make_layout(TEST_QUEUE_SIZE, false, &layout),
          "successful publication fixture publishes layout");
    CHECK(init_controller(
              &controller, &layout, &barrier, &state, 7U),
          "successful publication fixture initializes controller");

    state.slot = 0U;
    CHECK(virtio_available_publication_prepare(
              &controller, chain_three, 3U, &first) ==
              VIRTIO_AVAILABLE_PUBLICATION_OK &&
              token_valid(&controller, &first),
          "available publication prepare returns valid token");
    CHECK(virtio_available_publication_prepare(
              &controller, chain_one, 1U, &pending) ==
              VIRTIO_AVAILABLE_PUBLICATION_PENDING_FULL,
          "available publication rejects second pending chain");
    CHECK(ring.idx == 0U &&
              ring.ring[0] == TEST_RING_FILL &&
              ring.ring[1] == TEST_RING_FILL &&
              ring.ring[2] == TEST_RING_FILL &&
              ring.ring[3] == TEST_RING_FILL &&
              virtio_available_publication_published_count(&controller) == 0U &&
              virtio_available_publication_pending_length(&controller) == 3U &&
              state.count == 0U,
          "prepare leaves ring and visible index unchanged");

    CHECK(virtio_available_publication_publish(
              &controller, &first) ==
              VIRTIO_AVAILABLE_PUBLICATION_OK,
          "available publication publishes prepared chain");
    CHECK(state.count == 2U &&
              state.idx_before_first == 0U &&
              state.first_entry_before_first == 0U &&
              state.idx_before_second == 1U,
          "available publication fences before and after idx advance");
    CHECK(ring.idx == 1U &&
              ring.ring[0] == 0U &&
              ring.ring[1] == TEST_RING_FILL &&
              ring.ring[2] == TEST_RING_FILL &&
              ring.ring[3] == TEST_RING_FILL &&
              controller.expected_idx == 1U &&
              virtio_available_publication_published_count(&controller) == 1U &&
              virtio_available_publication_pending_length(&controller) == 0U,
          "publish writes chain head and advances idx once");
    CHECK(virtio_available_publication_publish(
              &controller, &first) ==
              VIRTIO_AVAILABLE_PUBLICATION_NO_PENDING &&
              ring.idx == 1U,
          "duplicate publish is rejected without index advance");

    state.slot = 3U;
    CHECK(virtio_available_publication_prepare(
              &controller, chain_one, 1U, &second) ==
              VIRTIO_AVAILABLE_PUBLICATION_OK &&
              virtio_available_publication_publish(
                  &controller, &second) ==
                  VIRTIO_AVAILABLE_PUBLICATION_OK &&
              ring.idx == 2U &&
              ring.ring[1] == 3U &&
              virtio_available_publication_published_count(&controller) == 2U,
          "available publication fills remaining queue capacity");
    CHECK(virtio_available_publication_prepare(
              &controller, chain_one, 1U, &second) ==
              VIRTIO_AVAILABLE_PUBLICATION_OK &&
              virtio_available_publication_publish(
                  &controller, &second) ==
                  VIRTIO_AVAILABLE_PUBLICATION_OK &&
              ring.idx == 3U &&
              ring.ring[2] == 3U &&
              virtio_available_publication_published_count(&controller) == 3U,
          "available publication fills remaining queue capacity");
    CHECK(virtio_available_publication_prepare(
              &controller, chain_one, 1U, &second) ==
              VIRTIO_AVAILABLE_PUBLICATION_OK &&
              virtio_available_publication_publish(
                  &controller, &second) ==
                  VIRTIO_AVAILABLE_PUBLICATION_OK &&
              ring.idx == 4U &&
              ring.ring[3] == 3U &&
              virtio_available_publication_published_count(&controller) == 4U,
          "available publication fills final queue slot");
    CHECK(virtio_available_publication_prepare(
              &controller, chain_one, 1U, &pending) ==
              VIRTIO_AVAILABLE_PUBLICATION_RING_FULL &&
              ring.idx == 4U &&
              !controller.pending,
          "full available ring rejects new chain without mutation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "available publication success performs no allocation");
}

static void test_prepare_failures(void)
{
    struct virtio_available_publication controller;
    struct virtio_queue_layout_result layout;
    struct virtio_available_barrier barrier;
    struct barrier_state state;
    struct virtio_available_token token;
    const uint16_t valid_chain[2] = {0U, 1U};
    const uint16_t duplicate_chain[2] = {1U, 1U};
    const uint16_t out_of_range_chain[1] = {TEST_QUEUE_SIZE};
    const uint16_t long_chain[VIRTIO_AVAILABLE_PUBLICATION_CHAIN_MAX + 1U];

    reset_ring(0U);
    CHECK(make_layout(TEST_QUEUE_SIZE, false, &layout),
          "prepare failure fixture publishes layout");
    CHECK(init_controller(
              &controller, &layout, &barrier, &state, 7U),
          "prepare failure fixture initializes controller");

    CHECK(virtio_available_publication_prepare(
              &controller, NULL, 1U, &token) ==
              VIRTIO_AVAILABLE_PUBLICATION_NULL,
          "prepare rejects null chain");
    CHECK(virtio_available_publication_prepare(
              &controller, valid_chain, 0U, &token) ==
              VIRTIO_AVAILABLE_PUBLICATION_INVALID_CHAIN,
          "prepare rejects empty chain");
    CHECK(virtio_available_publication_prepare(
              &controller, long_chain,
              VIRTIO_AVAILABLE_PUBLICATION_CHAIN_MAX + 1U, &token) ==
              VIRTIO_AVAILABLE_PUBLICATION_INVALID_CHAIN,
          "prepare rejects over-limit chain");
    CHECK(virtio_available_publication_prepare(
              &controller, out_of_range_chain, 1U, &token) ==
              VIRTIO_AVAILABLE_PUBLICATION_INVALID_CHAIN,
          "prepare rejects out-of-range descriptor");
    CHECK(virtio_available_publication_prepare(
              &controller, duplicate_chain, 2U, &token) ==
              VIRTIO_AVAILABLE_PUBLICATION_DUPLICATE_DESCRIPTOR,
          "prepare rejects duplicate descriptor");
    CHECK(virtio_available_publication_prepare(
              &controller, NULL, 1U, NULL) ==
              VIRTIO_AVAILABLE_PUBLICATION_NULL,
          "prepare rejects null token");
    CHECK(ring.idx == 0U &&
              ring.ring[0] == TEST_RING_FILL &&
              ring.ring[1] == TEST_RING_FILL &&
              !controller.pending &&
              virtio_available_publication_published_count(&controller) == 0U,
          "invalid preparations leave ring and controller unchanged");

    reset_ring(0U);
    CHECK(init_controller(
              &controller, &layout, &barrier, &state, 7U),
          "changed-index failure fixture initializes controller");
    ring.idx = 1U;
    CHECK(virtio_available_publication_prepare(
              &controller, valid_chain, 2U, &token) ==
              VIRTIO_AVAILABLE_PUBLICATION_INDEX_CHANGED &&
              !controller.pending &&
              virtio_available_publication_published_count(&controller) == 0U,
          "changed visible index fails preparation without mutation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "prepare failures perform no allocation");
}

static void test_publish_failures_and_cancel(void)
{
    struct virtio_available_publication controller;
    struct virtio_queue_layout_result layout;
    struct virtio_available_barrier barrier;
    struct barrier_state state;
    struct virtio_available_token first;
    struct virtio_available_token second;
    struct virtio_available_token stale;
    const uint16_t chain[2] = {0U, 1U};

    reset_ring(0U);
    CHECK(make_layout(TEST_QUEUE_SIZE, false, &layout),
          "publish failure fixture publishes layout");
    CHECK(init_controller(
              &controller, &layout, &barrier, &state, 9U),
          "publish failure fixture initializes controller");

    first.sequence = 1U;
    first.generation = controller.generation;
    CHECK(virtio_available_publication_publish(
              &controller, &first) ==
              VIRTIO_AVAILABLE_PUBLICATION_NO_PENDING,
          "publish rejects missing pending token");
    CHECK(virtio_available_publication_cancel(
              &controller, &first) ==
              VIRTIO_AVAILABLE_PUBLICATION_NO_PENDING,
          "cancel rejects missing pending token");

    CHECK(virtio_available_publication_prepare(
              &controller, chain, 2U, &first) ==
              VIRTIO_AVAILABLE_PUBLICATION_OK,
          "changed-index publish fixture prepares chain");
    ring.idx = 2U;
    CHECK(virtio_available_publication_publish(
              &controller, &first) ==
              VIRTIO_AVAILABLE_PUBLICATION_INDEX_CHANGED &&
              ring.ring[0] == TEST_RING_FILL &&
              ring.ring[1] == TEST_RING_FILL &&
              controller.pending &&
              virtio_available_publication_pending_length(&controller) == 2U,
          "changed visible index fails publish without ring mutation");
    ring.idx = 0U;
    CHECK(virtio_available_publication_cancel(
              &controller, &first) ==
              VIRTIO_AVAILABLE_PUBLICATION_OK &&
              !controller.pending &&
              ring.idx == 0U &&
              ring.ring[0] == TEST_RING_FILL &&
              ring.ring[1] == TEST_RING_FILL,
          "cancel clears pending without ring mutation");

    CHECK(virtio_available_publication_prepare(
              &controller, chain, 2U, &first) ==
              VIRTIO_AVAILABLE_PUBLICATION_OK &&
              virtio_available_publication_cancel(
                  &controller, &first) ==
                  VIRTIO_AVAILABLE_PUBLICATION_OK,
          "cancel allows repeatable preparation");
    CHECK(virtio_available_publication_prepare(
              &controller, chain, 2U, &second) ==
              VIRTIO_AVAILABLE_PUBLICATION_OK &&
              second.sequence == first.sequence + 1U,
          "stale-token fixture advances sequence after cancel");
    stale.sequence = first.sequence;
    stale.generation = controller.generation;
    CHECK(virtio_available_publication_publish(
              &controller, &stale) ==
              VIRTIO_AVAILABLE_PUBLICATION_STALE_TOKEN &&
              ring.idx == 0U &&
              ring.ring[0] == TEST_RING_FILL &&
              ring.ring[1] == TEST_RING_FILL &&
              controller.pending,
          "stale token is rejected without mutation");
    CHECK(virtio_available_publication_publish(
              &controller, &second) ==
              VIRTIO_AVAILABLE_PUBLICATION_OK &&
              ring.idx == 1U &&
              ring.ring[0] == 0U &&
              ring.ring[1] == TEST_RING_FILL,
          "current token publishes chain head only");
    CHECK(virtio_available_publication_cancel(
              &controller, &second) ==
              VIRTIO_AVAILABLE_PUBLICATION_NO_PENDING &&
              ring.idx == 1U,
          "cancel after publish is rejected without index change");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "publish failures and cancel perform no allocation");
}

static void test_ring_wraparound(void)
{
    struct virtio_available_publication controller;
    struct virtio_queue_layout_result layout;
    struct virtio_available_barrier barrier;
    struct barrier_state state;
    struct virtio_available_token token;
    const uint16_t chain_three[3] = {0U, 1U, 2U};
    const uint16_t chain_one[1] = {3U};

    reset_ring(UINT16_C(0xFFFE));
    CHECK(make_layout(TEST_QUEUE_SIZE, false, &layout),
          "wrap-around fixture publishes layout");
    CHECK(init_controller(
              &controller, &layout, &barrier, &state, 7U),
          "wrap-around fixture initializes controller");
    state.slot = 2U;

    CHECK(virtio_available_publication_prepare(
              &controller, chain_three, 3U, &token) ==
              VIRTIO_AVAILABLE_PUBLICATION_OK &&
              virtio_available_publication_publish(
                  &controller, &token) ==
                  VIRTIO_AVAILABLE_PUBLICATION_OK &&
              ring.idx == UINT16_C(0xFFFF) &&
              ring.ring[2] == 0U &&
              ring.ring[3] == TEST_RING_FILL &&
              ring.ring[0] == TEST_RING_FILL &&
              ring.ring[1] == TEST_RING_FILL,
          "publication wraps available ring head and idx exactly");
    CHECK(virtio_available_publication_prepare(
              &controller, chain_one, 1U, &token) ==
              VIRTIO_AVAILABLE_PUBLICATION_OK &&
              virtio_available_publication_publish(
                  &controller, &token) ==
                  VIRTIO_AVAILABLE_PUBLICATION_OK &&
              ring.idx == UINT16_C(0x0000) &&
              ring.ring[3] == 3U,
          "publication advances beyond 16-bit wrap boundary");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "wrap-around publication performs no allocation");
}

int main(void)
{
    test_status_strings();
    test_init_and_null();
    test_successful_publication();
    test_prepare_failures();
    test_publish_failures_and_cancel();
    test_ring_wraparound();

    if (failures != 0) {
        fprintf(stderr,
                "Virtio available publication host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Virtio available publication host tests: ALL OK\n");
    return 0;
}

#endif
