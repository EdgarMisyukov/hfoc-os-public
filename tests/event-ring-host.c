#include "event_ring.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_EVENT_RING_HOST_TEST

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static struct event_ring ring;
static unsigned int alloc_attempts;
static unsigned int free_attempts;

void *kmalloc(size_t size) {
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr) {
    (void)ptr;
    ++free_attempts;
    return false;
}

static void ring_reset(void) {
    memset(&ring, 0, sizeof(ring));
    alloc_attempts = 0U;
    free_attempts = 0U;
}

static struct kernel_event make_event(uint32_t type,
                                      uint64_t data0,
                                      uint64_t data1)
{
    struct kernel_event event;

    memset(&event, 0, sizeof(event));
    event.type = type;
    event.flags = 1U;
    event.data[0] = data0;
    event.data[1] = data1;
    return event;
}

static bool event_matches(const struct kernel_event *event,
                          uint64_t sequence,
                          uint32_t type,
                          uint64_t data0,
                          uint64_t data1)
{
    return event != NULL &&
           event->sequence == sequence &&
           event->type == type &&
           event->flags == 1U &&
           event->data[0] == data0 &&
           event->data[1] == data1;
}

static void test_validation_and_lifecycle(void) {
    struct kernel_event event = make_event(1U, 2U, 3U);
    struct kernel_event output;

    ring_reset();
    CHECK(event_ring_init(NULL) == EVENT_RING_NULL,
          "ring init rejects null ring");
    CHECK(event_ring_destroy(NULL) == EVENT_RING_NULL,
          "ring destroy rejects null ring");
    CHECK(event_ring_push(NULL, &event) == EVENT_RING_NULL,
          "ring push rejects null ring");
    CHECK(event_ring_pop(NULL, &output) == EVENT_RING_NULL,
          "ring pop rejects null ring");
    CHECK(event_ring_push(&ring, NULL) == EVENT_RING_NULL,
          "ring push rejects null event");
    CHECK(event_ring_pop(&ring, NULL) == EVENT_RING_NULL,
          "ring pop rejects null output");
    CHECK(event_ring_count(NULL) == 0U &&
              event_ring_capacity(NULL) == 0U &&
              event_ring_write_cursor(NULL) == 0U &&
              event_ring_read_cursor(NULL) == 0U &&
              !event_ring_is_published(NULL),
          "null ring accessors return zero/inactive state");

    CHECK(event_ring_push(&ring, &event) == EVENT_RING_BAD_STATE,
          "unpublished ring rejects push");
    CHECK(event_ring_pop(&ring, &output) == EVENT_RING_BAD_STATE,
          "unpublished ring rejects pop");
    CHECK(event_ring_destroy(&ring) == EVENT_RING_BAD_STATE,
          "unpublished ring rejects destroy");

    CHECK(event_ring_init(&ring) == EVENT_RING_OK &&
              event_ring_is_published(&ring) &&
              event_ring_count(&ring) == 0U &&
              event_ring_capacity(&ring) == KERNEL_EVENT_RING_CAPACITY &&
              event_ring_write_cursor(&ring) == 0U &&
              event_ring_read_cursor(&ring) == 0U,
          "ring init publishes empty bounded ring");
    CHECK(event_ring_init(&ring) == EVENT_RING_BAD_STATE,
          "double ring init is rejected");
    CHECK(event_ring_pop(&ring, &output) == EVENT_RING_EMPTY &&
              event_ring_count(&ring) == 0U &&
              event_ring_write_cursor(&ring) == 0U &&
              event_ring_read_cursor(&ring) == 0U,
          "empty ring pop leaves cursors unchanged");
    CHECK(event_ring_destroy(&ring) == EVENT_RING_OK &&
              !event_ring_is_published(&ring),
          "ring destroy clears published state");
    CHECK(event_ring_destroy(&ring) == EVENT_RING_BAD_STATE,
          "double ring destroy is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "ring validation and lifecycle perform no allocation");
}

static void test_exact_push_pop_and_sequence(void) {
    struct kernel_event event_a = make_event(10U, 100U, 200U);
    struct kernel_event event_b = make_event(20U, 300U, 400U);
    struct kernel_event event_c = make_event(30U, 500U, 600U);
    struct kernel_event output;

    ring_reset();
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "exact push/pop fixture initializes ring");
    CHECK(event_ring_push(&ring, &event_a) == EVENT_RING_OK &&
              event_ring_count(&ring) == 1U &&
              event_ring_write_cursor(&ring) == 1U &&
              event_ring_read_cursor(&ring) == 0U,
          "first push publishes one event and advances producer cursor");
    CHECK(event_ring_pop(&ring, &output) == EVENT_RING_OK &&
              event_matches(&output, 0U, 10U, 100U, 200U) &&
              event_ring_count(&ring) == 0U &&
              event_ring_write_cursor(&ring) == 1U &&
              event_ring_read_cursor(&ring) == 1U,
          "first pop returns exact ring-owned sequence and advances consumer");

    CHECK(event_ring_push(&ring, &event_b) == EVENT_RING_OK &&
              event_ring_push(&ring, &event_c) == EVENT_RING_OK &&
              event_ring_count(&ring) == 2U,
          "subsequent pushes keep FIFO order");
    CHECK(event_ring_pop(&ring, &output) == EVENT_RING_OK &&
              event_matches(&output, 1U, 20U, 300U, 400U),
          "second event gets sequence one");
    CHECK(event_ring_pop(&ring, &output) == EVENT_RING_OK &&
              event_matches(&output, 2U, 30U, 500U, 600U),
          "third event gets sequence two");
    CHECK(event_ring_pop(&ring, &output) == EVENT_RING_EMPTY,
          "drained ring pop returns empty");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "exact push/pop performs no allocation");
}

static void test_full_rejection_and_no_allocation(void) {
    struct kernel_event event;
    struct kernel_event output;
    uint64_t data;
    uint32_t index;

    ring_reset();
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "full-ring fixture initializes ring");
    for (index = 0U; index < KERNEL_EVENT_RING_CAPACITY; ++index) {
        event = make_event(index + 1U, index, KERNEL_EVENT_RING_CAPACITY);
        CHECK(event_ring_push(&ring, &event) == EVENT_RING_OK,
              "full-ring fill push succeeds");
    }
    CHECK(event_ring_count(&ring) == KERNEL_EVENT_RING_CAPACITY &&
              event_ring_write_cursor(&ring) ==
                  KERNEL_EVENT_RING_CAPACITY &&
              event_ring_read_cursor(&ring) == 0U,
          "ring reaches exact capacity with producer cursor at capacity");

    event = make_event(0xFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU);
    CHECK(event_ring_push(&ring, &event) == EVENT_RING_FULL &&
              event_ring_count(&ring) == KERNEL_EVENT_RING_CAPACITY &&
              event_ring_write_cursor(&ring) ==
                  KERNEL_EVENT_RING_CAPACITY,
          "full ring rejects push without changing producer state");

    for (index = 0U; index < KERNEL_EVENT_RING_CAPACITY; ++index) {
        data = KERNEL_EVENT_RING_CAPACITY - 1U - index;
        CHECK(event_ring_pop(&ring, &output) == EVENT_RING_OK &&
                  event_matches(&output, index, index + 1U,
                                index, KERNEL_EVENT_RING_CAPACITY),
              "full ring drains in FIFO order");
        (void)data;
    }
    CHECK(event_ring_count(&ring) == 0U &&
              event_ring_write_cursor(&ring) ==
                  KERNEL_EVENT_RING_CAPACITY &&
              event_ring_read_cursor(&ring) ==
                  KERNEL_EVENT_RING_CAPACITY,
          "full ring drains to empty with equal cursors");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "full and drained ring performs no allocation");
}

static void test_cursor_matrix_and_slot_wraparound(void) {
    struct kernel_event event;
    struct kernel_event output;
    uint64_t expected_sequence = 1U;
    uint32_t index;

    ring_reset();
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "cursor matrix fixture initializes ring");
    for (index = 0U; index < 3U; ++index) {
        event = make_event(index + 1U, index, 0U);
        CHECK(event_ring_push(&ring, &event) == EVENT_RING_OK,
              "cursor matrix pushes initial events");
    }
    CHECK(event_ring_pop(&ring, &output) == EVENT_RING_OK &&
              event_matches(&output, 0U, 1U, 0U, 0U) &&
              event_ring_write_cursor(&ring) == 3U &&
              event_ring_read_cursor(&ring) == 1U &&
              event_ring_count(&ring) == 2U,
          "cursor matrix consumes one of three events");

    for (index = 0U; index < KERNEL_EVENT_RING_CAPACITY - 2U; ++index) {
        event = make_event(index + 4U, index + 3U, 0U);
        CHECK(event_ring_push(&ring, &event) == EVENT_RING_OK,
              "cursor matrix fills through slot zero wraparound");
    }
    CHECK(event_ring_count(&ring) == KERNEL_EVENT_RING_CAPACITY &&
              event_ring_write_cursor(&ring) ==
                  KERNEL_EVENT_RING_CAPACITY + 1U &&
              event_ring_read_cursor(&ring) == 1U,
          "cursor matrix publishes exact wrap-around cursors");
    event = make_event(0xFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU);
    CHECK(event_ring_push(&ring, &event) == EVENT_RING_FULL,
          "wrapped ring rejects push at exact capacity");

    for (index = 0U; index < KERNEL_EVENT_RING_CAPACITY; ++index) {
        CHECK(event_ring_pop(&ring, &output) == EVENT_RING_OK &&
                  output.sequence == expected_sequence,
              "cursor matrix drains wrapped sequence order");
        ++expected_sequence;
    }
    CHECK(event_ring_count(&ring) == 0U &&
              event_ring_write_cursor(&ring) ==
                  KERNEL_EVENT_RING_CAPACITY + 1U &&
              event_ring_read_cursor(&ring) ==
                  KERNEL_EVENT_RING_CAPACITY + 1U,
          "cursor matrix returns to empty after wrapped drain");

    event = make_event(100U, 1000U, 2000U);
    CHECK(event_ring_push(&ring, &event) == EVENT_RING_OK &&
              event_ring_pop(&ring, &output) == EVENT_RING_OK &&
              event_matches(&output, KERNEL_EVENT_RING_CAPACITY + 1U,
                            100U, 1000U, 2000U),
          "reused slot after wrap keeps monotonically increasing sequence");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "wrapped cursor matrix performs no allocation");
}

int main(void) {
    test_validation_and_lifecycle();
    test_exact_push_pop_and_sequence();
    test_full_rejection_and_no_allocation();
    test_cursor_matrix_and_slot_wraparound();

    if (failures != 0) {
        fprintf(stderr, "Event ring host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Event ring host tests: ALL OK");
    return 0;
}

#endif
