#include "virtio_descriptor_ownership.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_VIRTIO_DESCRIPTOR_OWNERSHIP_HOST_TEST

#define TEST_CAPACITY 4U

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;
static struct virtio_descriptor_ownership pool;

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
    alloc_attempts = 0U;
    free_attempts = 0U;
}

static bool init_pool(void)
{
    reset_pool();
    return virtio_descriptor_ownership_init(
               &pool, TEST_CAPACITY) ==
           VIRTIO_DESCRIPTOR_OWNERSHIP_OK;
}

static uint32_t state_count(enum virtio_descriptor_state state)
{
    uint32_t count = 0U;
    uint32_t index;

    for (index = 0U; index < pool.capacity; ++index) {
        if ((enum virtio_descriptor_state)pool.states[index] == state) {
            ++count;
        }
    }
    return count;
}

static bool token_valid(const struct virtio_descriptor_token *token)
{
    return token != NULL &&
           token->index < pool.capacity &&
           token->generation != 0U;
}

static void test_status_strings(void)
{
    static const struct {
        enum virtio_descriptor_ownership_status status;
        const char *string;
    } statuses[] = {
        {VIRTIO_DESCRIPTOR_OWNERSHIP_OK,
         "VIRTIO_DESCRIPTOR_OWNERSHIP_OK"},
        {VIRTIO_DESCRIPTOR_OWNERSHIP_NULL,
         "VIRTIO_DESCRIPTOR_OWNERSHIP_NULL"},
        {VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_STATE,
         "VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_STATE"},
        {VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_CAPACITY,
         "VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_CAPACITY"},
        {VIRTIO_DESCRIPTOR_OWNERSHIP_EXHAUSTED,
         "VIRTIO_DESCRIPTOR_OWNERSHIP_EXHAUSTED"},
        {VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_DESCRIPTOR,
         "VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_DESCRIPTOR"},
        {VIRTIO_DESCRIPTOR_OWNERSHIP_STALE_DESCRIPTOR,
         "VIRTIO_DESCRIPTOR_OWNERSHIP_STALE_DESCRIPTOR"},
        {VIRTIO_DESCRIPTOR_OWNERSHIP_WRONG_STATE,
         "VIRTIO_DESCRIPTOR_OWNERSHIP_WRONG_STATE"},
        {VIRTIO_DESCRIPTOR_OWNERSHIP_DOUBLE_FREE,
         "VIRTIO_DESCRIPTOR_OWNERSHIP_DOUBLE_FREE"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = virtio_descriptor_ownership_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "descriptor ownership status string is exact");
    }
    CHECK(virtio_descriptor_ownership_status_string(
              (enum virtio_descriptor_ownership_status)9U) == NULL,
          "descriptor ownership undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "descriptor ownership status handling performs no allocation");
}

static void test_init_and_null(void)
{
    struct virtio_descriptor_token token;

    reset_pool();
    CHECK(virtio_descriptor_ownership_init(
              NULL, TEST_CAPACITY) == VIRTIO_DESCRIPTOR_OWNERSHIP_NULL,
          "descriptor ownership init rejects null pool");
    CHECK(virtio_descriptor_ownership_init(
              &pool, VIRTIO_DESCRIPTOR_OWNERSHIP_CAPACITY_MIN - 1U) ==
              VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_CAPACITY,
          "descriptor ownership init rejects zero capacity");
    CHECK(virtio_descriptor_ownership_init(
              &pool, VIRTIO_DESCRIPTOR_OWNERSHIP_CAPACITY_MAX + 1U) ==
              VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_CAPACITY,
          "descriptor ownership init rejects over-limit capacity");
    CHECK(virtio_descriptor_ownership_init(
              &pool, TEST_CAPACITY) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK,
          "descriptor ownership init publishes bounded pool");
    CHECK(virtio_descriptor_ownership_published(&pool),
          "descriptor ownership pool is published after init");
    CHECK(virtio_descriptor_ownership_free_count(&pool) == TEST_CAPACITY &&
              virtio_descriptor_ownership_reserved_count(&pool) == 0U &&
              virtio_descriptor_ownership_published_count(&pool) == 0U &&
              pool.freelist_top == TEST_CAPACITY &&
              state_count(VIRTIO_DESCRIPTOR_STATE_FREE) == TEST_CAPACITY,
          "descriptor ownership init builds exact freelist");
    CHECK(virtio_descriptor_ownership_init(
              &pool, TEST_CAPACITY) ==
              VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_STATE,
          "descriptor ownership init rejects duplicate publication");

    reset_pool();
    CHECK(!virtio_descriptor_ownership_published(&pool) &&
              virtio_descriptor_ownership_free_count(&pool) == 0U &&
              virtio_descriptor_ownership_reserved_count(&pool) == 0U &&
              virtio_descriptor_ownership_published_count(&pool) == 0U,
          "unpublished pool accessors return inactive state");
    CHECK(virtio_descriptor_ownership_reserve(
              &pool, &token) == VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_STATE,
          "unpublished pool rejects reserve");
    CHECK(virtio_descriptor_ownership_publish(
              &pool, &token) == VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_STATE,
          "unpublished pool rejects publish");
    CHECK(virtio_descriptor_ownership_rollback(
              &pool, &token) == VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_STATE,
          "unpublished pool rejects rollback");
    CHECK(virtio_descriptor_ownership_consume(
              &pool, &token) == VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_STATE,
          "unpublished pool rejects consume");

    CHECK(init_pool(), "valid descriptor pool init succeeds");
    CHECK(virtio_descriptor_ownership_reserve(
              &pool, NULL) == VIRTIO_DESCRIPTOR_OWNERSHIP_NULL,
          "descriptor reserve rejects null token");
    CHECK(virtio_descriptor_ownership_publish(
              &pool, NULL) == VIRTIO_DESCRIPTOR_OWNERSHIP_NULL,
          "descriptor publish rejects null token");
    CHECK(virtio_descriptor_ownership_rollback(
              &pool, NULL) == VIRTIO_DESCRIPTOR_OWNERSHIP_NULL,
          "descriptor rollback rejects null token");
    CHECK(virtio_descriptor_ownership_consume(
              &pool, NULL) == VIRTIO_DESCRIPTOR_OWNERSHIP_NULL,
          "descriptor consume rejects null token");
    CHECK(virtio_descriptor_ownership_reserve(
              NULL, &token) == VIRTIO_DESCRIPTOR_OWNERSHIP_NULL,
          "descriptor reserve rejects null pool");
    CHECK(virtio_descriptor_ownership_publish(
              NULL, &token) == VIRTIO_DESCRIPTOR_OWNERSHIP_NULL,
          "descriptor publish rejects null pool");
    CHECK(virtio_descriptor_ownership_rollback(
              NULL, &token) == VIRTIO_DESCRIPTOR_OWNERSHIP_NULL,
          "descriptor rollback rejects null pool");
    CHECK(virtio_descriptor_ownership_consume(
              NULL, &token) == VIRTIO_DESCRIPTOR_OWNERSHIP_NULL,
          "descriptor consume rejects null pool");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "descriptor ownership init/null checks perform no allocation");
}

static void test_lifecycle(void)
{
    struct virtio_descriptor_token first;
    struct virtio_descriptor_token second;
    struct virtio_descriptor_token reused;

    CHECK(init_pool(), "lifecycle fixture initializes descriptor pool");
    CHECK(virtio_descriptor_ownership_reserve(
              &pool, &first) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK &&
              token_valid(&first),
          "descriptor reserve returns valid token");
    CHECK(virtio_descriptor_ownership_reserve(
              &pool, &second) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK &&
              token_valid(&second) &&
              second.index != first.index,
          "descriptor reserve returns distinct token");
    CHECK(virtio_descriptor_ownership_free_count(&pool) == 2U &&
              virtio_descriptor_ownership_reserved_count(&pool) == 2U &&
              virtio_descriptor_ownership_published_count(&pool) == 0U,
          "descriptor reserve updates exact counts");

    CHECK(virtio_descriptor_ownership_consume(
              &pool, &first) == VIRTIO_DESCRIPTOR_OWNERSHIP_WRONG_STATE,
          "descriptor consume rejects reserved token");
    CHECK(virtio_descriptor_ownership_publish(
              &pool, &first) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK &&
              virtio_descriptor_ownership_reserved_count(&pool) == 1U &&
              virtio_descriptor_ownership_published_count(&pool) == 1U,
          "descriptor publish moves one descriptor to published");
    CHECK(virtio_descriptor_ownership_publish(
              &pool, &second) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK &&
              virtio_descriptor_ownership_free_count(&pool) == 2U &&
              virtio_descriptor_ownership_reserved_count(&pool) == 0U &&
              virtio_descriptor_ownership_published_count(&pool) == 2U,
          "descriptor publish drains reserved state");
    CHECK(virtio_descriptor_ownership_consume(
              &pool, &second) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK &&
              virtio_descriptor_ownership_free_count(&pool) == 3U &&
              virtio_descriptor_ownership_published_count(&pool) == 1U,
          "descriptor consume reclaims first published descriptor");
    CHECK(virtio_descriptor_ownership_consume(
              &pool, &first) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK &&
              virtio_descriptor_ownership_free_count(&pool) == 4U &&
              virtio_descriptor_ownership_published_count(&pool) == 0U,
          "descriptor consume reclaims remaining descriptor");

    CHECK(virtio_descriptor_ownership_reserve(
              &pool, &reused) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK &&
              reused.index == first.index &&
              reused.generation == first.generation + 1U,
          "descriptor consume returns slot with advanced generation");
    CHECK(virtio_descriptor_ownership_publish(
              &pool, &first) == VIRTIO_DESCRIPTOR_OWNERSHIP_STALE_DESCRIPTOR,
          "descriptor stale token cannot publish after reuse");
    CHECK(virtio_descriptor_ownership_rollback(
              &pool, &reused) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK &&
              virtio_descriptor_ownership_free_count(&pool) == TEST_CAPACITY,
          "descriptor rollback returns reserved slot to freelist");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "descriptor lifecycle performs no allocation");
}

static void test_rollback_and_double_free(void)
{
    struct virtio_descriptor_token first;
    struct virtio_descriptor_token second;

    CHECK(init_pool(), "rollback fixture initializes descriptor pool");
    CHECK(virtio_descriptor_ownership_reserve(
              &pool, &first) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK &&
              virtio_descriptor_ownership_reserve(
                  &pool, &second) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK,
          "rollback fixture reserves two descriptors");
    CHECK(virtio_descriptor_ownership_rollback(
              &pool, &first) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK &&
              virtio_descriptor_ownership_reserved_count(&pool) == 1U &&
              virtio_descriptor_ownership_free_count(&pool) == 3U,
          "descriptor rollback returns one reserved descriptor");
    CHECK(virtio_descriptor_ownership_rollback(
              &pool, &first) == VIRTIO_DESCRIPTOR_OWNERSHIP_DOUBLE_FREE,
          "descriptor repeated rollback is rejected as double free");
    CHECK(virtio_descriptor_ownership_consume(
              &pool, &first) == VIRTIO_DESCRIPTOR_OWNERSHIP_DOUBLE_FREE,
          "descriptor consume of free slot is rejected as double free");

    CHECK(virtio_descriptor_ownership_reserve(
              &pool, &second) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK &&
              second.index == first.index &&
              second.generation == first.generation + 1U,
          "descriptor rollback makes slot reusable");
    CHECK(virtio_descriptor_ownership_publish(
              &pool, &second) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK &&
              virtio_descriptor_ownership_consume(
                  &pool, &second) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK &&
              virtio_descriptor_ownership_consume(
                  &pool, &second) == VIRTIO_DESCRIPTOR_OWNERSHIP_DOUBLE_FREE,
          "descriptor duplicate completion is rejected as double free");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "descriptor rollback/double-free checks perform no allocation");
}

static void test_exhaustion(void)
{
    struct virtio_descriptor_token first;
    struct virtio_descriptor_token second;
    struct virtio_descriptor_token third;
    struct virtio_descriptor_token released;

    reset_pool();
    CHECK(virtio_descriptor_ownership_init(
              &pool, 2U) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK,
          "exhaustion fixture initializes capacity-2 pool");
    CHECK(virtio_descriptor_ownership_reserve(
              &pool, &first) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK &&
              virtio_descriptor_ownership_reserve(
                  &pool, &second) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK,
          "exhaustion fixture reserves all descriptors");
    CHECK(virtio_descriptor_ownership_reserve(
              &pool, &third) == VIRTIO_DESCRIPTOR_OWNERSHIP_EXHAUSTED &&
              virtio_descriptor_ownership_free_count(&pool) == 0U &&
              virtio_descriptor_ownership_reserved_count(&pool) == 2U,
          "descriptor exhaustion rejects without mutation");
    CHECK(virtio_descriptor_ownership_rollback(
              &pool, &first) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK &&
              virtio_descriptor_ownership_reserve(
                  &pool, &released) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK &&
              released.index == first.index,
          "descriptor rollback makes capacity available again");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "descriptor exhaustion performs no allocation");
}

static void test_wrong_state_and_stale_token(void)
{
    struct virtio_descriptor_token first;
    struct virtio_descriptor_token second;
    struct virtio_descriptor_token bad_descriptor;

    CHECK(init_pool(), "wrong-state fixture initializes descriptor pool");
    bad_descriptor.index = TEST_CAPACITY;
    bad_descriptor.generation = 1U;
    CHECK(virtio_descriptor_ownership_publish(
              &pool, &bad_descriptor) ==
              VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_DESCRIPTOR,
          "descriptor publish rejects out-of-range index");
    CHECK(virtio_descriptor_ownership_rollback(
              &pool, &bad_descriptor) ==
              VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_DESCRIPTOR,
          "descriptor rollback rejects out-of-range index");
    CHECK(virtio_descriptor_ownership_consume(
              &pool, &bad_descriptor) ==
              VIRTIO_DESCRIPTOR_OWNERSHIP_BAD_DESCRIPTOR,
          "descriptor consume rejects out-of-range index");

    CHECK(virtio_descriptor_ownership_reserve(
              &pool, &first) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK,
          "wrong-state fixture reserves first descriptor");
    CHECK(virtio_descriptor_ownership_consume(
              &pool, &first) == VIRTIO_DESCRIPTOR_OWNERSHIP_WRONG_STATE,
          "descriptor consume rejects reserved descriptor");
    CHECK(virtio_descriptor_ownership_rollback(
              &pool, &first) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK,
          "wrong-state fixture rolls back first descriptor");
    CHECK(virtio_descriptor_ownership_publish(
              &pool, &first) == VIRTIO_DESCRIPTOR_OWNERSHIP_WRONG_STATE,
          "descriptor publish rejects free descriptor");
    CHECK(virtio_descriptor_ownership_reserve(
              &pool, &first) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK &&
              virtio_descriptor_ownership_publish(
                  &pool, &first) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK,
          "wrong-state fixture publishes descriptor");
    CHECK(virtio_descriptor_ownership_rollback(
              &pool, &first) == VIRTIO_DESCRIPTOR_OWNERSHIP_WRONG_STATE,
          "descriptor rollback rejects published descriptor");

    CHECK(virtio_descriptor_ownership_consume(
              &pool, &first) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK &&
              virtio_descriptor_ownership_reserve(
                  &pool, &second) == VIRTIO_DESCRIPTOR_OWNERSHIP_OK &&
              second.index == first.index &&
              second.generation == first.generation + 1U,
          "stale-token fixture reuses advanced generation");
    CHECK(virtio_descriptor_ownership_publish(
              &pool, &first) == VIRTIO_DESCRIPTOR_OWNERSHIP_STALE_DESCRIPTOR,
          "descriptor stale token is rejected on publish");
    CHECK(virtio_descriptor_ownership_consume(
              &pool, &first) == VIRTIO_DESCRIPTOR_OWNERSHIP_STALE_DESCRIPTOR,
          "descriptor stale token is rejected on consume");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "descriptor wrong-state/stale checks perform no allocation");
}

int main(void)
{
    test_status_strings();
    test_init_and_null();
    test_lifecycle();
    test_rollback_and_double_free();
    test_exhaustion();
    test_wrong_state_and_stale_token();

    if (failures != 0) {
        fprintf(stderr,
                "Virtio descriptor ownership host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Virtio descriptor ownership host tests: ALL OK\n");
    return 0;
}

#endif
