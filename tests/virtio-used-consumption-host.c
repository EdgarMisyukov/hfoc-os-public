#include "virtio_used_consumption.h"
#include "virtio_queue_layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_VIRTIO_USED_CONSUMPTION_HOST_TEST

#define TEST_QUEUE_SIZE 4U
#define TEST_MAX_LENGTH 1024U
#define TEST_RING_FILL UINT32_C(0xFFFFFFFF)

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;
static struct virtio_used_ring ring;

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

struct reclaim_state {
    unsigned int calls;
    bool allow;
    bool reject_duplicate;
    bool seen;
    uint32_t released_ids[4];
    unsigned int released_count;
    struct virtio_used_element last_element;
    struct virtio_used_token last_token;
};

static bool test_reclaim(
    void *context,
    const struct virtio_used_element *element,
    const struct virtio_used_token *token)
{
    struct reclaim_state *state = (struct reclaim_state *)context;
    unsigned int index;

    state->last_element = *element;
    state->last_token = *token;
    ++state->calls;
    if (state->reject_duplicate) {
        for (index = 0U; index < state->released_count; ++index) {
            if (state->released_ids[index] == element->id) {
                return false;
            }
        }
    }
    if (!state->allow) {
        return false;
    }
    if (state->released_count <
        sizeof(state->released_ids) / sizeof(state->released_ids[0])) {
        state->released_ids[state->released_count] = element->id;
        ++state->released_count;
    }
    return true;
}

static void reset_used_ring(uint16_t idx)
{
    size_t index;

    ring.flags = 0U;
    ring.idx = idx;
    ring.used_event = 0U;
    for (index = 0U; index < 16U; ++index) {
        ring.ring[index].id = TEST_RING_FILL;
        ring.ring[index].len = TEST_RING_FILL;
    }
}

static void write_used_element(uint32_t slot, uint32_t id, uint32_t len)
{
    ring.ring[slot % TEST_QUEUE_SIZE].id = id;
    ring.ring[slot % TEST_QUEUE_SIZE].len = len;
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

static bool init_controller(
    struct virtio_used_consumption *controller,
    const struct virtio_queue_layout_result *layout,
    struct reclaim_state *state,
    struct virtio_used_reclaim *reclaim,
    uint32_t generation,
    uint32_t max_length)
{
    memset(controller, 0, sizeof(*controller));
    memset(state, 0, sizeof(*state));
    state->allow = true;
    reclaim->reclaim = test_reclaim;
    reclaim->context = state;
    return virtio_used_consumption_init(
               controller, layout, &ring, reclaim,
               generation, max_length) ==
           VIRTIO_USED_CONSUMPTION_OK;
}

static bool token_valid(
    const struct virtio_used_consumption *controller,
    const struct virtio_used_token *token)
{
    return token != NULL &&
           token->sequence != 0U &&
           token->generation == controller->generation;
}

static bool status_string_matches(
    enum virtio_used_consumption_status status,
    const char *string)
{
    switch (status) {
    case VIRTIO_USED_CONSUMPTION_OK:
        return strcmp(string, "VIRTIO_USED_CONSUMPTION_OK") == 0;
    case VIRTIO_USED_CONSUMPTION_NULL:
        return strcmp(string, "VIRTIO_USED_CONSUMPTION_NULL") == 0;
    case VIRTIO_USED_CONSUMPTION_INVALID_VERSION:
        return strcmp(
            string, "VIRTIO_USED_CONSUMPTION_INVALID_VERSION") == 0;
    case VIRTIO_USED_CONSUMPTION_INVALID_GENERATION:
        return strcmp(
            string, "VIRTIO_USED_CONSUMPTION_INVALID_GENERATION") == 0;
    case VIRTIO_USED_CONSUMPTION_ALREADY_PUBLISHED:
        return strcmp(
            string, "VIRTIO_USED_CONSUMPTION_ALREADY_PUBLISHED") == 0;
    case VIRTIO_USED_CONSUMPTION_UNPUBLISHED:
        return strcmp(
            string, "VIRTIO_USED_CONSUMPTION_UNPUBLISHED") == 0;
    case VIRTIO_USED_CONSUMPTION_INVALID_LAYOUT:
        return strcmp(
            string, "VIRTIO_USED_CONSUMPTION_INVALID_LAYOUT") == 0;
    case VIRTIO_USED_CONSUMPTION_INVALID_QUEUE_SIZE:
        return strcmp(
            string, "VIRTIO_USED_CONSUMPTION_INVALID_QUEUE_SIZE") == 0;
    case VIRTIO_USED_CONSUMPTION_INVALID_MAX_LENGTH:
        return strcmp(
            string, "VIRTIO_USED_CONSUMPTION_INVALID_MAX_LENGTH") == 0;
    case VIRTIO_USED_CONSUMPTION_INVALID_RING:
        return strcmp(
            string, "VIRTIO_USED_CONSUMPTION_INVALID_RING") == 0;
    case VIRTIO_USED_CONSUMPTION_INVALID_RELEASE:
        return strcmp(
            string, "VIRTIO_USED_CONSUMPTION_INVALID_RELEASE") == 0;
    case VIRTIO_USED_CONSUMPTION_EMPTY:
        return strcmp(string, "VIRTIO_USED_CONSUMPTION_EMPTY") == 0;
    case VIRTIO_USED_CONSUMPTION_RING_OVERFLOW:
        return strcmp(
            string, "VIRTIO_USED_CONSUMPTION_RING_OVERFLOW") == 0;
    case VIRTIO_USED_CONSUMPTION_INVALID_HEAD:
        return strcmp(
            string, "VIRTIO_USED_CONSUMPTION_INVALID_HEAD") == 0;
    case VIRTIO_USED_CONSUMPTION_INVALID_LENGTH:
        return strcmp(
            string, "VIRTIO_USED_CONSUMPTION_INVALID_LENGTH") == 0;
    case VIRTIO_USED_CONSUMPTION_PENDING_FULL:
        return strcmp(
            string, "VIRTIO_USED_CONSUMPTION_PENDING_FULL") == 0;
    case VIRTIO_USED_CONSUMPTION_NO_PENDING:
        return strcmp(
            string, "VIRTIO_USED_CONSUMPTION_NO_PENDING") == 0;
    case VIRTIO_USED_CONSUMPTION_STALE_TOKEN:
        return strcmp(
            string, "VIRTIO_USED_CONSUMPTION_STALE_TOKEN") == 0;
    case VIRTIO_USED_CONSUMPTION_INDEX_CHANGED:
        return strcmp(
            string, "VIRTIO_USED_CONSUMPTION_INDEX_CHANGED") == 0;
    case VIRTIO_USED_CONSUMPTION_ENTRY_CHANGED:
        return strcmp(
            string, "VIRTIO_USED_CONSUMPTION_ENTRY_CHANGED") == 0;
    case VIRTIO_USED_CONSUMPTION_RELEASE_REJECTED:
        return strcmp(
            string, "VIRTIO_USED_CONSUMPTION_RELEASE_REJECTED") == 0;
    case VIRTIO_USED_CONSUMPTION_INVALID_STATE:
        return strcmp(
            string, "VIRTIO_USED_CONSUMPTION_INVALID_STATE") == 0;
    default:
        return false;
    }
}

static void test_status_strings(void)
{
    static const enum virtio_used_consumption_status statuses[] = {
        VIRTIO_USED_CONSUMPTION_OK,
        VIRTIO_USED_CONSUMPTION_NULL,
        VIRTIO_USED_CONSUMPTION_INVALID_VERSION,
        VIRTIO_USED_CONSUMPTION_INVALID_GENERATION,
        VIRTIO_USED_CONSUMPTION_ALREADY_PUBLISHED,
        VIRTIO_USED_CONSUMPTION_UNPUBLISHED,
        VIRTIO_USED_CONSUMPTION_INVALID_LAYOUT,
        VIRTIO_USED_CONSUMPTION_INVALID_QUEUE_SIZE,
        VIRTIO_USED_CONSUMPTION_INVALID_MAX_LENGTH,
        VIRTIO_USED_CONSUMPTION_INVALID_RING,
        VIRTIO_USED_CONSUMPTION_INVALID_RELEASE,
        VIRTIO_USED_CONSUMPTION_EMPTY,
        VIRTIO_USED_CONSUMPTION_RING_OVERFLOW,
        VIRTIO_USED_CONSUMPTION_INVALID_HEAD,
        VIRTIO_USED_CONSUMPTION_INVALID_LENGTH,
        VIRTIO_USED_CONSUMPTION_PENDING_FULL,
        VIRTIO_USED_CONSUMPTION_NO_PENDING,
        VIRTIO_USED_CONSUMPTION_STALE_TOKEN,
        VIRTIO_USED_CONSUMPTION_INDEX_CHANGED,
        VIRTIO_USED_CONSUMPTION_ENTRY_CHANGED,
        VIRTIO_USED_CONSUMPTION_RELEASE_REJECTED,
        VIRTIO_USED_CONSUMPTION_INVALID_STATE,
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        enum virtio_used_consumption_status status = statuses[index];
        const char *string =
            virtio_used_consumption_status_string(status);

        CHECK(string != NULL &&
                  status_string_matches(status, string),
              "status string matches exact status name");
    }
    CHECK(virtio_used_consumption_status_string(
              (enum virtio_used_consumption_status)99) == NULL,
          "undefined status string is rejected");
}

static void test_init_and_null(void)
{
    struct virtio_used_consumption controller;
    struct virtio_queue_layout_result layout;
    struct virtio_used_reclaim reclaim;
    struct reclaim_state state;
    struct virtio_queue_layout_policy policy;
    struct virtio_used_consumption duplicate;

    memset(&controller, 0, sizeof(controller));
    memset(&state, 0, sizeof(state));
    state.allow = true;
    reclaim.reclaim = test_reclaim;
    reclaim.context = &state;
    reset_used_ring(0U);
    CHECK(make_layout(TEST_QUEUE_SIZE, false, &layout),
          "init fixture publishes layout");
    CHECK(virtio_used_consumption_init(
              NULL, &layout, &ring, &reclaim, 1U, TEST_MAX_LENGTH) ==
              VIRTIO_USED_CONSUMPTION_NULL,
          "init rejects null controller");
    CHECK(virtio_used_consumption_init(
              &controller, NULL, &ring, &reclaim, 1U, TEST_MAX_LENGTH) ==
              VIRTIO_USED_CONSUMPTION_NULL,
          "init rejects null layout");
    CHECK(virtio_used_consumption_init(
              &controller, &layout, NULL, &reclaim, 1U, TEST_MAX_LENGTH) ==
              VIRTIO_USED_CONSUMPTION_NULL,
          "init rejects null ring");
    CHECK(virtio_used_consumption_init(
              &controller, &layout, &ring, NULL, 1U, TEST_MAX_LENGTH) ==
              VIRTIO_USED_CONSUMPTION_NULL,
          "init rejects null reclaim");

    policy.queue_size = TEST_QUEUE_SIZE;
    policy.event_index = false;
    CHECK(virtio_queue_layout_calculate(
              &policy, 0U, UINT64_C(0x100000), &layout) ==
              VIRTIO_QUEUE_LAYOUT_OK,
          "init fixture calculates layout");
    layout.published = false;
    CHECK(virtio_used_consumption_init(
              &controller, &layout, &ring, &reclaim, 1U, TEST_MAX_LENGTH) ==
              VIRTIO_USED_CONSUMPTION_INVALID_LAYOUT,
          "init rejects unpublished layout");
    layout.published = true;
    layout.queue_size = 3U;
    CHECK(virtio_used_consumption_init(
              &controller, &layout, &ring, &reclaim, 1U, TEST_MAX_LENGTH) ==
              VIRTIO_USED_CONSUMPTION_INVALID_LAYOUT,
          "init rejects invalid queue size layout");
    CHECK(make_layout(TEST_QUEUE_SIZE, false, &layout),
          "init fixture republishes layout");
    CHECK(virtio_used_consumption_init(
              &controller, &layout, &ring, &reclaim, 0U, TEST_MAX_LENGTH) ==
              VIRTIO_USED_CONSUMPTION_INVALID_GENERATION,
          "init rejects invalid generation");
    CHECK(virtio_used_consumption_init(
              &controller, &layout, &ring, &reclaim, 1U,
              VIRTIO_USED_CONSUMPTION_LENGTH_MAX + 1U) ==
              VIRTIO_USED_CONSUMPTION_INVALID_MAX_LENGTH,
          "init rejects invalid max length");

    memset(&controller, 0, sizeof(controller));
    memset(&state, 0, sizeof(state));
    state.allow = true;
    reclaim.reclaim = test_reclaim;
    reclaim.context = &state;
    CHECK(virtio_used_consumption_init(
              &controller, &layout, &ring, &reclaim, 1U, TEST_MAX_LENGTH) ==
              VIRTIO_USED_CONSUMPTION_OK,
          "init publishes controller");
    duplicate = controller;
    CHECK(virtio_used_consumption_init(
              &duplicate, &layout, &ring, &reclaim, 1U, TEST_MAX_LENGTH) ==
              VIRTIO_USED_CONSUMPTION_ALREADY_PUBLISHED,
          "init rejects duplicate publication");
    CHECK(virtio_used_consumption_published(&controller),
          "published accessor accepts valid controller");
    CHECK(virtio_used_consumption_visible_index(&controller) == 0U &&
              virtio_used_consumption_consumed_count(&controller) == 0U &&
              virtio_used_consumption_pending_head(&controller) == 0U,
          "accessors report exact initial state");
}

static void test_successful_consume(void)
{
    struct virtio_used_consumption controller;
    struct virtio_queue_layout_result layout;
    struct virtio_used_reclaim reclaim;
    struct reclaim_state state;
    struct virtio_used_element entry;
    struct virtio_used_token token;

    reset_used_ring(1U);
    write_used_element(0U, 1U, 64U);
    CHECK(make_layout(TEST_QUEUE_SIZE, false, &layout),
          "success fixture publishes layout");
    CHECK(init_controller(
              &controller, &layout, &state, &reclaim, 7U, TEST_MAX_LENGTH),
          "success fixture initializes controller");

    CHECK(virtio_used_consumption_prepare(
              &controller, &entry, &token) ==
              VIRTIO_USED_CONSUMPTION_OK &&
              token_valid(&controller, &token) &&
              entry.id == 1U &&
              entry.len == 64U &&
              controller.expected_idx == 0U &&
              controller.consumed_count == 0U &&
              controller.pending &&
              controller.pending_sequence == token.sequence &&
              state.calls == 0U,
          "prepare copies exact used element without advancing index");
    CHECK(virtio_used_consumption_consume(
              &controller, &token) == VIRTIO_USED_CONSUMPTION_OK &&
              controller.expected_idx == 1U &&
              controller.consumed_count == 1U &&
              !controller.pending &&
              state.calls == 1U &&
              state.last_element.id == 1U &&
              state.last_element.len == 64U &&
              state.last_token.sequence == token.sequence &&
              state.last_token.generation == controller.generation,
          "consume reclaims once and advances exact index");
    CHECK(ring.idx == 1U &&
              virtio_used_consumption_visible_index(&controller) == 1U &&
              alloc_attempts == 0U && free_attempts == 0U,
          "consume leaves device idx and performs no allocation");
}

static void test_prepare_failures(void)
{
    struct virtio_used_consumption controller;
    struct virtio_queue_layout_result layout;
    struct virtio_used_reclaim reclaim;
    struct reclaim_state state;
    struct virtio_used_element entry;
    struct virtio_used_token token;
    struct virtio_used_token stale;

    reset_used_ring(0U);
    CHECK(make_layout(TEST_QUEUE_SIZE, false, &layout),
          "empty fixture publishes layout");
    CHECK(init_controller(
              &controller, &layout, &state, &reclaim, 1U, TEST_MAX_LENGTH),
          "empty fixture initializes controller");
    CHECK(virtio_used_consumption_prepare(
              &controller, &entry, &token) ==
              VIRTIO_USED_CONSUMPTION_EMPTY &&
              !controller.pending &&
              controller.expected_idx == 0U &&
              controller.consumed_count == 0U,
          "empty ring prepare fails without mutation");

    reset_used_ring(1U);
    write_used_element(0U, TEST_QUEUE_SIZE, 64U);
    CHECK(virtio_used_consumption_prepare(
              &controller, &entry, &token) ==
              VIRTIO_USED_CONSUMPTION_INVALID_HEAD &&
              !controller.pending,
          "out-of-range head is rejected without mutation");

    write_used_element(0U, 1U, TEST_MAX_LENGTH + 1U);
    CHECK(virtio_used_consumption_prepare(
              &controller, &entry, &token) ==
              VIRTIO_USED_CONSUMPTION_INVALID_LENGTH &&
              !controller.pending,
          "out-of-range length is rejected without mutation");

    write_used_element(0U, 1U, 64U);
    CHECK(virtio_used_consumption_prepare(
              &controller, &entry, &token) ==
              VIRTIO_USED_CONSUMPTION_OK,
          "valid entry prepares after failure");
    CHECK(virtio_used_consumption_prepare(
              &controller, &entry, &token) ==
              VIRTIO_USED_CONSUMPTION_PENDING_FULL,
          "pending-full prepare is rejected");
    stale = token;
    CHECK(virtio_used_consumption_cancel(
              &controller, &stale) == VIRTIO_USED_CONSUMPTION_OK,
          "cancel clears pending for stale fixture");

    reset_used_ring(5U);
    write_used_element(0U, 1U, 64U);
    CHECK(virtio_used_consumption_prepare(
              &controller, &entry, &token) ==
              VIRTIO_USED_CONSUMPTION_RING_OVERFLOW,
          "ring overflow is rejected without mutation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "prepare failures perform no allocation");
}

static void test_consume_failures_and_cancel(void)
{
    struct virtio_used_consumption controller;
    struct virtio_queue_layout_result layout;
    struct virtio_used_reclaim reclaim;
    struct reclaim_state state;
    struct virtio_used_element entry;
    struct virtio_used_token token;
    struct virtio_used_token first;
    struct virtio_used_token stale;

    reset_used_ring(1U);
    write_used_element(0U, 1U, 64U);
    CHECK(make_layout(TEST_QUEUE_SIZE, false, &layout),
          "failure fixture publishes layout");
    CHECK(init_controller(
              &controller, &layout, &state, &reclaim, 2U, TEST_MAX_LENGTH),
          "failure fixture initializes controller");
    CHECK(virtio_used_consumption_consume(
              &controller, &token) == VIRTIO_USED_CONSUMPTION_NO_PENDING,
          "consume rejects missing pending token");

    CHECK(virtio_used_consumption_prepare(
              &controller, &entry, &token) ==
              VIRTIO_USED_CONSUMPTION_OK,
          "failure fixture prepares first entry");
    first = token;
    stale = first;
    stale.sequence = first.sequence + 1U;
    CHECK(virtio_used_consumption_consume(
              &controller, &stale) == VIRTIO_USED_CONSUMPTION_STALE_TOKEN &&
              controller.expected_idx == 0U &&
              controller.consumed_count == 0U &&
              controller.pending,
          "stale token consume is rejected without mutation");
    CHECK(virtio_used_consumption_consume(
              &controller, &first) == VIRTIO_USED_CONSUMPTION_OK &&
              controller.expected_idx == 1U,
          "current token consumes first entry");
    CHECK(virtio_used_consumption_consume(
              &controller, &first) == VIRTIO_USED_CONSUMPTION_NO_PENDING &&
              controller.expected_idx == 1U,
          "double consumption is rejected without index change");

    reset_used_ring(2U);
    write_used_element(1U, 2U, 32U);
    CHECK(virtio_used_consumption_prepare(
              &controller, &entry, &token) ==
              VIRTIO_USED_CONSUMPTION_OK,
          "cancel fixture prepares second entry");
    write_used_element(1U, 9U, 32U);
    CHECK(virtio_used_consumption_consume(
              &controller, &token) == VIRTIO_USED_CONSUMPTION_ENTRY_CHANGED &&
              controller.expected_idx == 1U &&
              controller.consumed_count == 1U &&
              controller.pending,
          "changed pending entry is rejected without consumption");
    write_used_element(1U, 2U, 32U);
    state.allow = false;
    CHECK(virtio_used_consumption_consume(
              &controller, &token) ==
              VIRTIO_USED_CONSUMPTION_RELEASE_REJECTED &&
              controller.expected_idx == 1U &&
              controller.consumed_count == 1U &&
              controller.pending &&
              state.calls == 2U &&
              state.released_count == 1U,
          "rejected reclaim does not advance or count completion");
    CHECK(virtio_used_consumption_cancel(
              &controller, &token) == VIRTIO_USED_CONSUMPTION_OK &&
              !controller.pending,
          "cancel clears rejected pending entry");
    state.allow = true;
    CHECK(virtio_used_consumption_prepare(
              &controller, &entry, &token) ==
              VIRTIO_USED_CONSUMPTION_OK &&
              token.sequence == first.sequence + 2U &&
              virtio_used_consumption_consume(
                  &controller, &token) ==
                  VIRTIO_USED_CONSUMPTION_OK &&
              controller.expected_idx == 2U &&
              controller.consumed_count == 2U &&
              state.released_count == 2U,
          "cancel permits later consumption with exact sequence");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "consume failures and cancel perform no allocation");
}

static void test_repeated_completion(void)
{
    struct virtio_used_consumption controller;
    struct virtio_queue_layout_result layout;
    struct virtio_used_reclaim reclaim;
    struct reclaim_state state;
    struct virtio_used_element entry;
    struct virtio_used_token token;

    reset_used_ring(1U);
    write_used_element(0U, 3U, 128U);
    CHECK(make_layout(TEST_QUEUE_SIZE, false, &layout),
          "repeated fixture publishes layout");
    CHECK(init_controller(
              &controller, &layout, &state, &reclaim, 9U, TEST_MAX_LENGTH),
          "repeated fixture initializes controller");
    state.reject_duplicate = true;
    CHECK(virtio_used_consumption_prepare(
              &controller, &entry, &token) ==
              VIRTIO_USED_CONSUMPTION_OK &&
              virtio_used_consumption_consume(
                  &controller, &token) ==
                  VIRTIO_USED_CONSUMPTION_OK &&
              controller.expected_idx == 1U &&
              controller.consumed_count == 1U &&
              state.released_count == 1U,
          "first valid completion is consumed once");

    reset_used_ring(2U);
    write_used_element(1U, 3U, 128U);
    CHECK(virtio_used_consumption_prepare(
              &controller, &entry, &token) ==
              VIRTIO_USED_CONSUMPTION_OK &&
              virtio_used_consumption_consume(
                  &controller, &token) ==
                  VIRTIO_USED_CONSUMPTION_RELEASE_REJECTED &&
              controller.expected_idx == 1U &&
              controller.consumed_count == 1U &&
              state.released_count == 1U &&
              state.released_ids[0] == 3U,
          "repeated completion cannot release or advance ownership");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "repeated completion performs no allocation");
}

static void test_ring_wraparound(void)
{
    struct virtio_used_consumption controller;
    struct virtio_queue_layout_result layout;
    struct virtio_used_reclaim reclaim;
    struct reclaim_state state;
    struct virtio_used_element entry;
    struct virtio_used_token token;

    reset_used_ring(0U);
    write_used_element(3U, 3U, 8U);
    CHECK(make_layout(TEST_QUEUE_SIZE, false, &layout),
          "wrap fixture publishes layout");
    CHECK(init_controller(
              &controller, &layout, &state, &reclaim, 4U, TEST_MAX_LENGTH),
          "wrap fixture initializes controller");
    controller.expected_idx = UINT16_MAX;

    CHECK(virtio_used_consumption_prepare(
              &controller, &entry, &token) ==
              VIRTIO_USED_CONSUMPTION_OK &&
              entry.id == 3U &&
              virtio_used_consumption_consume(
                  &controller, &token) ==
                  VIRTIO_USED_CONSUMPTION_OK &&
              controller.expected_idx == 0U &&
              controller.consumed_count == 1U,
          "used consumption wraps local index exactly");

    reset_used_ring(1U);
    write_used_element(0U, 0U, 16U);
    CHECK(virtio_used_consumption_prepare(
              &controller, &entry, &token) ==
              VIRTIO_USED_CONSUMPTION_OK &&
              entry.id == 0U &&
              virtio_used_consumption_consume(
                  &controller, &token) ==
                  VIRTIO_USED_CONSUMPTION_OK &&
              controller.expected_idx == 1U &&
              controller.consumed_count == 2U,
          "used consumption continues after 16-bit wrap");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "wrap-around consumption performs no allocation");
}

int main(void)
{
    test_status_strings();
    test_init_and_null();
    test_successful_consume();
    test_prepare_failures();
    test_consume_failures_and_cancel();
    test_repeated_completion();
    test_ring_wraparound();

    if (failures != 0) {
        fprintf(stderr,
                "Virtio used consumption host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Virtio used consumption host tests: ALL OK\n");
    return 0;
}

#endif
