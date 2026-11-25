#include "state_migration.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_STATE_MIGRATION_HOST_TEST

#define TEST_CAPACITY 4U

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

static struct state_schema_descriptor make_descriptor(
    uint8_t target_kind,
    const char *id,
    uint16_t minimum_length)
{
    struct state_schema_descriptor descriptor;
    size_t id_length = strlen(id);

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.kind = STATE_SCHEMA_KIND_KV;
    descriptor.target_kind = target_kind;
    descriptor.id_length = (uint8_t)id_length;
    memcpy(descriptor.id, id, id_length);
    descriptor.version = 2U;
    descriptor.minimum_version = 1U;
    descriptor.maximum_version = 2U;
    descriptor.minimum_length = minimum_length;
    descriptor.maximum_length = CONFIG_KV_VALUE_MAX;
    return descriptor;
}

static void init_registry(struct state_schema_registry *registry)
{
    struct state_schema_descriptor descriptors[2];

    descriptors[0] = make_descriptor(
        CONFIG_KV_KIND_CONFIG, "settings", 1U);
    descriptors[1] = make_descriptor(
        CONFIG_KV_KIND_AUDIT, "events", 1U);
    CHECK(state_schema_registry_init(
              registry, descriptors, 2U) == STATE_SCHEMA_OK,
          "migration registry fixture initializes");
}

static struct state_migration_step make_step(
    uint8_t target_kind,
    const char *id,
    uint32_t from_version,
    uint32_t to_version,
    const char *key,
    const char *value)
{
    struct state_migration_step step;
    size_t id_length = strlen(id);
    size_t key_length = strlen(key);
    size_t value_length = strlen(value);

    memset(&step, 0, sizeof(step));
    step.kind = STATE_SCHEMA_KIND_KV;
    step.target_kind = target_kind;
    step.id_length = (uint8_t)id_length;
    memcpy(step.id, id, id_length);
    step.from_version = from_version;
    step.to_version = to_version;
    step.key_length = (uint8_t)key_length;
    memcpy(step.key, key, key_length);
    step.value_length = (uint16_t)value_length;
    memcpy(step.value, value, value_length);
    return step;
}

static void init_migration_steps(struct state_migration_step *steps)
{
    steps[0] = make_step(
        CONFIG_KV_KIND_CONFIG, "settings", 1U, 2U, "theme", "v2");
    steps[1] = make_step(
        CONFIG_KV_KIND_AUDIT, "events", 1U, 2U, "epoch", "a2");
}

static bool step_equal(const struct state_migration_step *left,
                       const struct state_migration_step *right)
{
    return left->kind == right->kind &&
           left->target_kind == right->target_kind &&
           left->id_length == right->id_length &&
           memcmp(left->id, right->id, sizeof(left->id)) == 0 &&
           left->from_version == right->from_version &&
           left->to_version == right->to_version &&
           left->key_length == right->key_length &&
           memcmp(left->key, right->key, sizeof(left->key)) == 0 &&
           left->value_length == right->value_length &&
           memcmp(left->value, right->value, sizeof(left->value)) == 0;
}

static void populate_active(struct config_kv *active,
                            struct config_kv_entry *entries,
                            size_t capacity)
{
    bool replaced = false;

    memset(entries, 0, sizeof(entries[0]) * capacity);
    CHECK(config_kv_init(active, entries, capacity) == CONFIG_KV_OK,
          "migration active fixture initializes kv");
    CHECK(config_kv_put(active, CONFIG_KV_KIND_CONFIG,
                        "theme", 5U, "old", 3U, &replaced) == CONFIG_KV_OK &&
              config_kv_put(active, CONFIG_KV_KIND_CONFIG,
                            "other", 5U, "keep", 4U, &replaced) ==
                  CONFIG_KV_OK &&
              config_kv_put(active, CONFIG_KV_KIND_AUDIT,
                            "epoch", 5U, "old-a", 5U, &replaced) ==
                  CONFIG_KV_OK &&
              config_kv_put(active, CONFIG_KV_KIND_AUDIT,
                            "other", 5U, "keep-a", 6U, &replaced) ==
                  CONFIG_KV_OK,
          "migration active fixture populates exact entries");
}

struct write_fixture {
    size_t calls;
    size_t fail_index;
    bool never_fail;
};

static bool fixture_write(void *context, size_t step_index)
{
    struct write_fixture *fixture = (struct write_fixture *)context;

    ++fixture->calls;
    if (!fixture->never_fail && step_index == fixture->fail_index) {
        return false;
    }
    return true;
}

static bool active_has_value(const struct config_kv *active,
                             uint8_t kind,
                             const char *key,
                             const char *expected)
{
    unsigned char value[CONFIG_KV_VALUE_MAX];
    size_t value_length = 0U;
    uint64_t revision = 0U;
    size_t expected_length = strlen(expected);

    if (config_kv_get(active, kind, key, strlen(key),
                      value, sizeof(value), &value_length,
                      &revision) != CONFIG_KV_OK) {
        return false;
    }
    return value_length == expected_length &&
           memcmp(value, expected, expected_length) == 0;
}

static void test_validation_and_status_strings(void) {
    static const enum state_migration_status statuses[] = {
        STATE_MIGRATION_OK,
        STATE_MIGRATION_NULL,
        STATE_MIGRATION_BAD_STATE,
        STATE_MIGRATION_BAD_PLAN,
        STATE_MIGRATION_BAD_CAPACITY,
        STATE_MIGRATION_BAD_KIND,
        STATE_MIGRATION_BAD_TARGET,
        STATE_MIGRATION_BAD_ID,
        STATE_MIGRATION_BAD_VERSION,
        STATE_MIGRATION_BAD_LENGTH,
        STATE_MIGRATION_UNKNOWN,
        STATE_MIGRATION_UNSUPPORTED_VERSION,
        STATE_MIGRATION_DUPLICATE,
        STATE_MIGRATION_OVERFLOW,
        STATE_MIGRATION_PUBLISH_FAILED,
        STATE_MIGRATION_WRITE_FAULT,
    };
    struct state_schema_registry registry;
    struct state_migration_step steps[2];
    struct state_migration_step bad_step;
    struct state_migration_plan plan;
    unsigned char before[sizeof(plan)];
    size_t index;

    init_registry(&registry);
    init_migration_steps(steps);
    memset(&plan, 0, sizeof(plan));

    CHECK(state_migration_plan_init(NULL, &registry, steps, 2U) ==
              STATE_MIGRATION_NULL,
          "migration plan init rejects null plan");
    CHECK(state_migration_plan_init(&plan, NULL, steps, 2U) ==
              STATE_MIGRATION_NULL,
          "migration plan init rejects null registry");
    CHECK(state_migration_plan_init(&plan, &registry, NULL, 2U) ==
              STATE_MIGRATION_NULL,
          "migration plan init rejects null steps with count");
    CHECK(state_migration_plan_init(&plan, &registry, steps, 0U) ==
              STATE_MIGRATION_BAD_CAPACITY,
          "migration plan init rejects zero step count");
    CHECK(state_migration_plan_init(
              &plan, &registry, steps,
              STATE_MIGRATION_MAX_STEPS + 1U) ==
              STATE_MIGRATION_BAD_CAPACITY,
          "migration plan init rejects over-limit step count");

    bad_step = make_step(
        STATE_SCHEMA_KIND_JOURNAL, "settings", 1U, 2U, "theme", "v2");
    bad_step.kind = STATE_SCHEMA_KIND_JOURNAL;
    CHECK(state_migration_plan_init(&plan, &registry, &bad_step, 1U) ==
              STATE_MIGRATION_BAD_KIND,
          "migration plan init rejects journal kind");
    bad_step = make_step(
        CONFIG_KV_KIND_CONFIG, "settings", 1U, 2U, "theme", "v2");
    bad_step.target_kind = CONFIG_KV_KIND_COUNT;
    CHECK(state_migration_plan_init(&plan, &registry, &bad_step, 1U) ==
              STATE_MIGRATION_BAD_TARGET,
          "migration plan init rejects bad target kind");
    bad_step = make_step(
        CONFIG_KV_KIND_CONFIG, "", 1U, 2U, "theme", "v2");
    CHECK(state_migration_plan_init(&plan, &registry, &bad_step, 1U) ==
              STATE_MIGRATION_BAD_ID,
          "migration plan init rejects empty schema id");
    bad_step = make_step(
        CONFIG_KV_KIND_CONFIG, "bad-id", 1U, 2U, "theme", "v2");
    CHECK(state_migration_plan_init(&plan, &registry, &bad_step, 1U) ==
              STATE_MIGRATION_BAD_ID,
          "migration plan init rejects non-alphanumeric schema id");
    bad_step = make_step(
        CONFIG_KV_KIND_CONFIG, "missing", 1U, 2U, "theme", "v2");
    CHECK(state_migration_plan_init(&plan, &registry, &bad_step, 1U) ==
              STATE_MIGRATION_UNKNOWN,
          "migration plan init rejects unknown schema id");
    bad_step = make_step(
        CONFIG_KV_KIND_CONFIG, "settings", 0U, 2U, "theme", "v2");
    CHECK(state_migration_plan_init(&plan, &registry, &bad_step, 1U) ==
              STATE_MIGRATION_BAD_VERSION,
          "migration plan init rejects zero from version");
    bad_step = make_step(
        CONFIG_KV_KIND_CONFIG, "settings", 2U, 1U, "theme", "v2");
    CHECK(state_migration_plan_init(&plan, &registry, &bad_step, 1U) ==
              STATE_MIGRATION_BAD_VERSION,
          "migration plan init rejects non-forward versions");
    bad_step = make_step(
        CONFIG_KV_KIND_CONFIG, "settings", 2U, 2U, "theme", "v2");
    CHECK(state_migration_plan_init(&plan, &registry, &bad_step, 1U) ==
              STATE_MIGRATION_BAD_VERSION,
          "migration plan init rejects equal versions");
    bad_step = make_step(
        CONFIG_KV_KIND_CONFIG, "settings", 3U, 4U, "theme", "v2");
    CHECK(state_migration_plan_init(&plan, &registry, &bad_step, 1U) ==
              STATE_MIGRATION_UNSUPPORTED_VERSION,
          "migration plan init rejects version outside compatibility bounds");
    bad_step = make_step(
        CONFIG_KV_KIND_CONFIG, "settings", 1U, 2U, "theme", "");
    CHECK(state_migration_plan_init(&plan, &registry, &bad_step, 1U) ==
              STATE_MIGRATION_BAD_LENGTH,
          "migration plan init rejects empty migrated value");

    memset(&plan, 0x5A, sizeof(plan));
    memcpy(before, &plan, sizeof(before));
    steps[0].from_version = 0U;
    CHECK(state_migration_plan_init(&plan, &registry, steps, 2U) ==
              STATE_MIGRATION_BAD_VERSION &&
              memcmp(&plan, before, sizeof(before)) == 0,
          "failed migration plan init leaves destination unchanged");
    init_migration_steps(steps);
    steps[1] = steps[0];
    CHECK(state_migration_plan_init(&plan, &registry, steps, 2U) ==
              STATE_MIGRATION_DUPLICATE,
          "migration plan init rejects duplicate schema migration");

    init_migration_steps(steps);
    CHECK(state_migration_plan_init(&plan, &registry, steps, 2U) ==
              STATE_MIGRATION_OK &&
              plan.valid &&
              plan.count == 2U &&
              step_equal(&plan.steps[0], &steps[0]) &&
              step_equal(&plan.steps[1], &steps[1]),
          "migration plan init publishes exact immutable snapshot");
    steps[0].to_version = 9U;
    CHECK(plan.steps[0].to_version == 2U,
          "migration plan snapshot does not change after source mutation");

    for (index = 0U; index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        if (state_migration_status_string(statuses[index]) == NULL) {
            CHECK(false, "migration status string is present");
        }
    }
    CHECK(strcmp(state_migration_status_string(
                     (enum state_migration_status)99U),
                 "STATE_MIGRATION_UNKNOWN") == 0,
          "migration undefined status string is exact");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "migration plan validation performs no allocation");
}

static void test_exact_publication(void) {
    struct state_schema_registry registry;
    struct state_migration_step steps[2];
    struct state_migration_plan plan;
    struct config_kv active;
    struct config_kv_entry active_entries[TEST_CAPACITY];
    struct config_kv_entry rollback_entries[TEST_CAPACITY];
    struct write_fixture fixture;
    struct state_migration_io io;

    init_registry(&registry);
    init_migration_steps(steps);
    populate_active(&active, active_entries, TEST_CAPACITY);
    CHECK(state_migration_plan_init(&plan, &registry, steps, 2U) ==
              STATE_MIGRATION_OK,
          "exact publication fixture plans migrations");

    memset(&fixture, 0, sizeof(fixture));
    fixture.never_fail = true;
    io.context = &fixture;
    io.write = fixture_write;
    io.rollback = rollback_entries;
    io.rollback_capacity = TEST_CAPACITY;
    CHECK(state_migration_commit(&active, &registry, &plan, &io) ==
              STATE_MIGRATION_OK &&
              fixture.calls == 2U,
          "valid migration commits every planned step");
    CHECK(active_has_value(&active, CONFIG_KV_KIND_CONFIG,
                           "theme", "v2"),
          "migration publishes exact migrated config value");
    CHECK(active_has_value(&active, CONFIG_KV_KIND_CONFIG,
                           "other", "keep"),
          "migration leaves unrelated config value unchanged");
    CHECK(active_has_value(&active, CONFIG_KV_KIND_AUDIT,
                           "epoch", "a2"),
          "migration publishes exact migrated audit value");
    CHECK(active_has_value(&active, CONFIG_KV_KIND_AUDIT,
                           "other", "keep-a"),
          "migration leaves unrelated audit value unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "migration exact publication performs no allocation");
}

static void test_write_fault_rollback(void) {
    struct state_schema_registry registry;
    struct state_migration_step steps[2];
    struct state_migration_plan plan;
    struct config_kv active;
    struct config_kv_entry active_entries[TEST_CAPACITY];
    struct config_kv_entry rollback_entries[TEST_CAPACITY];
    unsigned char active_before[sizeof(active_entries)];
    size_t previous_count;
    struct write_fixture fixture;
    struct state_migration_io io;

    init_registry(&registry);
    init_migration_steps(steps);
    populate_active(&active, active_entries, TEST_CAPACITY);
    memcpy(active_before, active_entries, sizeof(active_entries));
    previous_count = active.count;
    CHECK(state_migration_plan_init(&plan, &registry, steps, 2U) ==
              STATE_MIGRATION_OK,
          "write-fault fixture plans migrations");

    memset(&fixture, 0, sizeof(fixture));
    fixture.fail_index = 1U;
    io.context = &fixture;
    io.write = fixture_write;
    io.rollback = rollback_entries;
    io.rollback_capacity = TEST_CAPACITY;
    CHECK(state_migration_commit(&active, &registry, &plan, &io) ==
              STATE_MIGRATION_WRITE_FAULT &&
              fixture.calls >= 2U,
          "write-callback fault returns migration write fault");
    CHECK(memcmp(active_entries, active_before,
                 sizeof(active_entries)) == 0 &&
              active.count == previous_count,
          "write-fault migration restores exact previous active bytes");
    CHECK(active_has_value(&active, CONFIG_KV_KIND_CONFIG,
                           "theme", "old"),
          "write-fault migration keeps previous config value readable");
    CHECK(active_has_value(&active, CONFIG_KV_KIND_AUDIT,
                           "epoch", "old-a"),
          "write-fault migration keeps previous audit value readable");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "write-fault rollback performs no allocation");
}

static void test_publish_failure_rollback(void) {
    struct state_schema_registry registry;
    struct state_migration_step steps[2];
    struct state_migration_plan plan;
    struct config_kv active;
    struct config_kv_entry active_entries[2];
    struct config_kv_entry rollback_entries[2];
    unsigned char active_before[sizeof(active_entries)];
    bool replaced = false;
    size_t previous_count;
    struct write_fixture fixture;
    struct state_migration_io io;

    init_registry(&registry);
    memset(active_entries, 0, sizeof(active_entries));
    CHECK(config_kv_init(&active, active_entries, 2U) == CONFIG_KV_OK &&
              config_kv_put(&active, CONFIG_KV_KIND_CONFIG,
                            "theme", 5U, "old", 3U, &replaced) ==
                  CONFIG_KV_OK &&
              config_kv_put(&active, CONFIG_KV_KIND_CONFIG,
                            "other", 5U, "keep", 4U, &replaced) ==
                  CONFIG_KV_OK,
          "publish-failure fixture fills active kv");
    memcpy(active_before, active_entries, sizeof(active_entries));
    previous_count = active.count;
    steps[0] = make_step(
        CONFIG_KV_KIND_CONFIG, "settings", 1U, 2U, "theme", "v2");
    steps[1] = make_step(
        CONFIG_KV_KIND_AUDIT, "events", 1U, 2U, "gamma", "g2");
    CHECK(state_migration_plan_init(&plan, &registry, steps, 2U) ==
              STATE_MIGRATION_OK,
          "publish-failure fixture plans migrations");

    memset(&fixture, 0, sizeof(fixture));
    fixture.never_fail = true;
    io.context = &fixture;
    io.write = fixture_write;
    io.rollback = rollback_entries;
    io.rollback_capacity = 2U;
    CHECK(state_migration_commit(&active, &registry, &plan, &io) ==
              STATE_MIGRATION_PUBLISH_FAILED,
          "config_kv full publication returns migration publish failed");
    CHECK(memcmp(active_entries, active_before,
                 sizeof(active_entries)) == 0 &&
              active.count == previous_count,
          "config_kv publication failure restores exact previous active state");
    CHECK(active_has_value(&active, CONFIG_KV_KIND_CONFIG,
                           "theme", "old"),
          "config_kv publication failure keeps previous value readable");
    CHECK(active_has_value(&active, CONFIG_KV_KIND_CONFIG,
                           "other", "keep"),
          "config_kv publication failure keeps unrelated value readable");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "publish-failure rollback performs no allocation");
}

static void test_apply_convenience(void) {
    struct state_schema_registry registry;
    struct state_migration_step steps[1];
    struct config_kv active;
    struct config_kv_entry active_entries[TEST_CAPACITY];
    struct config_kv_entry rollback_entries[TEST_CAPACITY];
    struct write_fixture fixture;
    struct state_migration_io io;

    init_registry(&registry);
    steps[0] = make_step(
        CONFIG_KV_KIND_CONFIG, "settings", 1U, 2U, "theme", "v2");
    populate_active(&active, active_entries, TEST_CAPACITY);
    memset(&fixture, 0, sizeof(fixture));
    fixture.never_fail = true;
    io.context = &fixture;
    io.write = fixture_write;
    io.rollback = rollback_entries;
    io.rollback_capacity = TEST_CAPACITY;
    CHECK(state_migration_apply(&active, &registry, steps, 1U, &io) ==
              STATE_MIGRATION_OK &&
              active_has_value(&active, CONFIG_KV_KIND_CONFIG,
                               "theme", "v2"),
          "migration apply validates and publishes in one call");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "migration apply performs no allocation");
}

int main(void) {
    test_validation_and_status_strings();
    test_exact_publication();
    test_write_fault_rollback();
    test_publish_failure_rollback();
    test_apply_convenience();

    if (failures != 0) {
        fprintf(stderr, "State migration host tests: FAILED (%d)\n",
                failures);
        return 1;
    }
    printf("State migration host tests: ALL OK\n");
    return 0;
}

#endif
