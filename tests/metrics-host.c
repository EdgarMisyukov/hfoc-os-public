#include "metrics.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_METRICS_HOST_TEST

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

static struct metrics_health_values make_health(void)
{
    struct metrics_health_values values;

    memset(&values, 0, sizeof(values));
    values.version = METRICS_VERSION;
    values.check_count = 16U;
    values.pass_count = 4U;
    values.warn_count = 1U;
    values.fail_count = 0U;
    values.unknown_count = 2U;
    values.ring_published = true;
    values.ring_count = 16U;
    values.ring_available = 48U;
    values.ring_utilization_percent = 25U;
    return values;
}

static struct metrics_state_values make_state(void)
{
    struct metrics_state_values values;

    memset(&values, 0, sizeof(values));
    values.version = METRICS_VERSION;
    values.generation = 3U;
    values.journal_generation = 7U;
    values.checkpoint_sequence = 9U;
    values.schema_version = 1U;
    values.active_generation = 1U;
    values.migrations = 2U;
    values.compactions = 3U;
    values.restores = 0U;
    values.backup_exports = 1U;
    return values;
}

static struct metrics_service_values make_service(void)
{
    struct metrics_service_values values;

    memset(&values, 0, sizeof(values));
    values.version = METRICS_VERSION;
    values.generation = 5U;
    values.service_count = 2U;
    values.ready_count = 2U;
    values.pending_count = 0U;
    values.failed_count = 0U;
    values.exhausted_count = 0U;
    values.dependency_blocked_count = 0U;
    values.restarts = 1U;
    values.exits = 0U;
    return values;
}

static struct metrics_update_values make_update(void)
{
    struct metrics_update_values values;

    memset(&values, 0, sizeof(values));
    values.version = METRICS_VERSION;
    values.generation = 2U;
    values.inventory_generation = 4U;
    values.slot_count = 2U;
    values.known_good_slot = 1U;
    values.pending_slot = 2U;
    values.rollback_slot = 1U;
    values.candidate_pending = true;
    values.candidate_active = false;
    values.accepted = false;
    values.rollback_published = false;
    values.acceptances = 1U;
    values.rollbacks = 0U;
    values.rejections = 1U;
    return values;
}

static enum metrics_status stage_all(struct metrics_store *store)
{
    const struct metrics_health_values health = make_health();
    const struct metrics_state_values state = make_state();
    const struct metrics_service_values service = make_service();
    const struct metrics_update_values update = make_update();
    enum metrics_status status;

    status = metrics_begin_update(store);
    if (status != METRICS_OK) {
        return status;
    }
    status = metrics_set_health(store, &health);
    if (status != METRICS_OK) {
        return status;
    }
    status = metrics_set_state(store, &state);
    if (status != METRICS_OK) {
        return status;
    }
    status = metrics_set_service(store, &service);
    if (status != METRICS_OK) {
        return status;
    }
    status = metrics_set_update(store, &update);
    if (status != METRICS_OK) {
        return status;
    }
    return metrics_commit(store);
}

static void test_status_and_schema(void)
{
    static const struct {
        enum metrics_status status;
        const char *string;
    } statuses[] = {
        {METRICS_OK, "METRICS_OK"},
        {METRICS_NULL, "METRICS_NULL"},
        {METRICS_BAD_STATE, "METRICS_BAD_STATE"},
        {METRICS_BAD_VERSION, "METRICS_BAD_VERSION"},
        {METRICS_NOT_PUBLISHED, "METRICS_NOT_PUBLISHED"},
        {METRICS_NO_TRANSACTION, "METRICS_NO_TRANSACTION"},
        {METRICS_TRANSACTION_OPEN, "METRICS_TRANSACTION_OPEN"},
        {METRICS_BAD_VALUE, "METRICS_BAD_VALUE"},
        {METRICS_INCOMPLETE, "METRICS_INCOMPLETE"},
        {METRICS_GENERATION_OVERFLOW,
         "METRICS_GENERATION_OVERFLOW"},
    };
    size_t index;

    alloc_attempts = 0U;
    free_attempts = 0U;
    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = metrics_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "metrics status string is exact");
    }
    CHECK(metrics_status_string((enum metrics_status)10U) == NULL,
          "metrics undefined status is rejected");
    CHECK(strcmp(metrics_schema_id(), METRICS_SCHEMA_ID) == 0,
          "metrics schema ID is exact");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "metrics strings perform no allocation");
}

static void test_null_and_state_failures(void)
{
    struct metrics_store store;
    struct metrics_snapshot snapshot;
    struct metrics_health_values health;
    struct metrics_state_values state;
    struct metrics_service_values service;
    struct metrics_update_values update;

    memset(&store, 0, sizeof(store));
    memset(&snapshot, 0xA5, sizeof(snapshot));
    health = make_health();
    state = make_state();
    service = make_service();
    update = make_update();

    CHECK(metrics_init(NULL) == METRICS_NULL,
          "metrics init rejects null store");
    CHECK(metrics_begin_update(NULL) == METRICS_NULL,
          "metrics begin rejects null store");
    CHECK(metrics_set_health(NULL, &health) == METRICS_NULL,
          "metrics set health rejects null store");
    CHECK(metrics_set_health(&store, NULL) == METRICS_NULL,
          "metrics set health rejects null values");
    CHECK(metrics_commit(NULL) == METRICS_NULL,
          "metrics commit rejects null store");
    CHECK(metrics_rollback(NULL) == METRICS_NULL,
          "metrics rollback rejects null store");
    CHECK(metrics_snapshot(NULL, &snapshot) == METRICS_NULL,
          "metrics snapshot rejects null store");
    CHECK(metrics_snapshot(&store, NULL) == METRICS_NULL,
          "metrics snapshot rejects null output");
    CHECK(metrics_begin_update(&store) == METRICS_BAD_STATE,
          "metrics begin rejects uninitialized store");
    CHECK(metrics_snapshot(&store, &snapshot) == METRICS_BAD_STATE,
          "metrics snapshot rejects uninitialized store");

    CHECK(metrics_init(&store) == METRICS_OK,
          "metrics init initializes store");
    CHECK(metrics_init(&store) == METRICS_BAD_STATE,
          "metrics init rejects double init");
    CHECK(metrics_snapshot(&store, &snapshot) ==
              METRICS_NOT_PUBLISHED,
          "metrics snapshot rejects unpublished store");
    CHECK(metrics_set_health(&store, &health) ==
              METRICS_NO_TRANSACTION,
          "metrics set path rejects missing transaction");

    CHECK(metrics_begin_update(&store) == METRICS_OK,
          "metrics begin opens transaction");
    CHECK(metrics_begin_update(&store) == METRICS_TRANSACTION_OPEN,
          "metrics begin rejects open transaction");
    health.version = METRICS_VERSION + 1U;
    CHECK(metrics_set_health(&store, &health) == METRICS_BAD_VERSION,
          "metrics set health rejects unsupported version");
    health = make_health();
    state.version = 0U;
    CHECK(metrics_set_state(&store, &state) == METRICS_BAD_VERSION,
          "metrics set state rejects zero version");
    state = make_state();
    service.version = METRICS_VERSION + 1U;
    CHECK(metrics_set_service(&store, &service) == METRICS_BAD_VERSION,
          "metrics set service rejects unsupported version");
    service = make_service();
    update.version = 0U;
    CHECK(metrics_set_update(&store, &update) == METRICS_BAD_VERSION,
          "metrics set update rejects zero version");
    CHECK(metrics_rollback(&store) == METRICS_OK,
          "metrics rollback closes failed transaction");
    CHECK(metrics_rollback(&store) == METRICS_NO_TRANSACTION,
          "metrics rollback rejects missing transaction");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "metrics null and state paths perform no allocation");
}

static void test_incomplete_and_invalid_commit(void)
{
    struct metrics_store store;
    struct metrics_snapshot snapshot;
    struct metrics_health_values health = make_health();
    struct metrics_state_values state = make_state();
    struct metrics_service_values service = make_service();
    struct metrics_update_values update = make_update();
    unsigned char before[sizeof(snapshot)];

    memset(&store, 0, sizeof(store));
    memset(&snapshot, 0xA5, sizeof(snapshot));
    memcpy(before, &snapshot, sizeof(before));

    CHECK(metrics_init(&store) == METRICS_OK,
          "incomplete fixture initializes store");
    CHECK(metrics_begin_update(&store) == METRICS_OK,
          "incomplete fixture opens transaction");
    CHECK(metrics_set_health(&store, &health) == METRICS_OK,
          "incomplete fixture sets health");
    CHECK(metrics_commit(&store) == METRICS_INCOMPLETE,
          "metrics commit rejects incomplete paths");
    CHECK(!store.published &&
              store.generation == 0U &&
              store.mutation_open,
          "metrics incomplete commit leaves store unpublished");
    CHECK(metrics_rollback(&store) == METRICS_OK,
          "metrics rollback closes incomplete transaction");

    CHECK(metrics_begin_update(&store) == METRICS_OK,
          "invalid fixture opens transaction");
    health.check_count = METRICS_HEALTH_CHECK_MAX + 1U;
    CHECK(metrics_set_health(&store, &health) == METRICS_BAD_VALUE,
          "metrics health rejects over-limit check count");
    health = make_health();
    health.ring_published = true;
    health.ring_count = 20U;
    health.ring_available = 40U;
    CHECK(metrics_set_health(&store, &health) == METRICS_BAD_VALUE,
          "metrics health rejects inconsistent ring capacity");
    health = make_health();
    health.ring_published = false;
    health.ring_count = 1U;
    CHECK(metrics_set_health(&store, &health) == METRICS_BAD_VALUE,
          "metrics health rejects unpublished ring count");
    health = make_health();
    CHECK(metrics_set_health(&store, &health) == METRICS_OK,
          "metrics invalid fixture sets valid health");

    state.schema_version = METRICS_STATE_SCHEMA_VERSION_MIN - 1U;
    CHECK(metrics_set_state(&store, &state) == METRICS_BAD_VALUE,
          "metrics state rejects unsupported schema version");
    state = make_state();
    CHECK(metrics_set_state(&store, &state) == METRICS_OK,
          "metrics invalid fixture sets valid state");

    service.service_count = METRICS_SERVICE_CAPACITY + 1U;
    CHECK(metrics_set_service(&store, &service) == METRICS_BAD_VALUE,
          "metrics service rejects over-limit service count");
    service = make_service();
    service.ready_count = 1U;
    service.failed_count = 0U;
    CHECK(metrics_set_service(&store, &service) == METRICS_BAD_VALUE,
          "metrics service rejects inconsistent readiness sum");
    service = make_service();
    service.exhausted_count = 1U;
    CHECK(metrics_set_service(&store, &service) == METRICS_BAD_VALUE,
          "metrics service rejects exhausted count above failed count");
    service = make_service();
    service.dependency_blocked_count =
        service.service_count + 1U;
    CHECK(metrics_set_service(&store, &service) == METRICS_BAD_VALUE,
          "metrics service rejects blocked count above service count");
    service = make_service();
    CHECK(metrics_set_service(&store, &service) == METRICS_OK,
          "metrics invalid fixture sets valid service");

    update.slot_count = METRICS_SLOT_CAPACITY + 1U;
    CHECK(metrics_set_update(&store, &update) == METRICS_BAD_VALUE,
          "metrics update rejects over-limit slot count");
    update = make_update();
    update.known_good_slot = METRICS_SLOT_CAPACITY + 1U;
    CHECK(metrics_set_update(&store, &update) == METRICS_BAD_VALUE,
          "metrics update rejects slot above capacity");
    update = make_update();
    CHECK(metrics_set_update(&store, &update) == METRICS_OK,
          "metrics invalid fixture sets valid update");
    CHECK(metrics_commit(&store) == METRICS_OK,
          "metrics valid staged paths commit");

    CHECK(metrics_snapshot(&store, &snapshot) == METRICS_OK &&
              memcmp(&snapshot, before, sizeof(before)) != 0,
          "metrics snapshot publishes valid commit");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "metrics invalid paths perform no allocation");
}

static void test_coherent_generation(void)
{
    struct metrics_store store;
    struct metrics_snapshot first;
    struct metrics_snapshot second;
    struct metrics_health_values health = make_health();
    struct metrics_state_values state = make_state();
    struct metrics_service_values service = make_service();
    struct metrics_update_values update = make_update();

    memset(&store, 0, sizeof(store));
    CHECK(metrics_init(&store) == METRICS_OK,
          "coherence fixture initializes store");
    CHECK(stage_all(&store) == METRICS_OK,
          "coherence fixture publishes generation one");
    CHECK(metrics_snapshot(&store, &first) == METRICS_OK &&
              first.version == METRICS_VERSION &&
              first.generation == 1U &&
              first.paths.health.check_count == 16U &&
              first.paths.state.schema_version == 1U &&
              first.paths.service.service_count == 2U &&
              first.paths.update.slot_count == 2U,
          "metrics generation one snapshot is exact");
    CHECK(metrics_snapshot(&store, &second) == METRICS_OK &&
              memcmp(&first, &second, sizeof(first)) == 0,
          "metrics same-generation snapshots are byte-identical");

    CHECK(metrics_begin_update(&store) == METRICS_OK,
          "coherence fixture opens second transaction");
    health.pass_count = 99U;
    health.ring_count = 32U;
    health.ring_available = 32U;
    health.ring_utilization_percent = 50U;
    state.checkpoint_sequence = 11U;
    service.ready_count = 1U;
    service.pending_count = 1U;
    update.slot_count = 1U;
    update.pending_slot = 0U;
    update.rollback_slot = 0U;
    update.candidate_pending = false;

    CHECK(metrics_snapshot(&store, &second) == METRICS_OK &&
              memcmp(&first, &second, sizeof(first)) == 0,
          "metrics pending mutation is invisible to readers");
    CHECK(metrics_set_health(&store, &health) == METRICS_OK &&
              metrics_set_state(&store, &state) == METRICS_OK &&
              metrics_set_service(&store, &service) == METRICS_OK &&
              metrics_set_update(&store, &update) == METRICS_OK &&
              metrics_commit(&store) == METRICS_OK,
          "metrics second coherent generation commits");

    CHECK(metrics_snapshot(&store, &second) == METRICS_OK &&
              second.generation == 2U &&
              second.paths.health.pass_count == 99U &&
              second.paths.health.ring_count == 32U &&
              second.paths.state.checkpoint_sequence == 11U &&
              second.paths.service.ready_count == 1U &&
              second.paths.service.pending_count == 1U &&
              second.paths.update.slot_count == 1U &&
              !second.paths.update.candidate_pending,
          "metrics committed generation is fully visible");

    CHECK(metrics_begin_update(&store) == METRICS_OK,
          "rollback fixture opens transaction");
    health.pass_count = 1000U;
    CHECK(metrics_set_health(&store, &health) == METRICS_OK,
          "rollback fixture stages health");
    CHECK(metrics_rollback(&store) == METRICS_OK,
          "metrics rollback discards pending paths");
    CHECK(metrics_snapshot(&store, &second) == METRICS_OK &&
              memcmp(&first, &second, sizeof(first)) != 0 &&
              second.generation == 2U &&
              second.paths.health.pass_count == 99U,
          "metrics rollback preserves published generation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "metrics coherent generation performs no allocation");
}

static void test_generation_overflow(void)
{
    struct metrics_store store;
    struct metrics_health_values health = make_health();
    struct metrics_state_values state = make_state();
    struct metrics_service_values service = make_service();
    struct metrics_update_values update = make_update();

    memset(&store, 0, sizeof(store));
    CHECK(metrics_init(&store) == METRICS_OK,
          "overflow fixture initializes store");
    CHECK(metrics_begin_update(&store) == METRICS_OK,
          "overflow fixture opens transaction");
    CHECK(metrics_set_health(&store, &health) == METRICS_OK &&
              metrics_set_state(&store, &state) == METRICS_OK &&
              metrics_set_service(&store, &service) == METRICS_OK &&
              metrics_set_update(&store, &update) == METRICS_OK,
          "overflow fixture stages all paths");
    store.generation = UINT64_MAX;
    CHECK(metrics_commit(&store) == METRICS_GENERATION_OVERFLOW,
          "metrics commit rejects generation overflow");
    CHECK(!store.published &&
              store.generation == UINT64_MAX &&
              store.mutation_open,
          "metrics overflow commit leaves store unchanged");
    CHECK(metrics_rollback(&store) == METRICS_OK,
          "overflow fixture rolls back");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "metrics overflow path performs no allocation");
}

int main(void)
{
    test_status_and_schema();
    test_null_and_state_failures();
    test_incomplete_and_invalid_commit();
    test_coherent_generation();
    test_generation_overflow();

    if (failures != 0) {
        fprintf(stderr, "Metrics host tests: %d FAILURE(S)\n",
                failures);
        return 1;
    }
    printf("Metrics host tests: ALL OK\n");
    return 0;
}

#endif
