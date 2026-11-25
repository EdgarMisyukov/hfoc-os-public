#include "config_kv.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

static bool entry_equal(const unsigned char *left,
                        const unsigned char *right,
                        size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

static void test_validation_and_status_strings(void) {
    static const enum config_kv_status statuses[] = {
        CONFIG_KV_OK,
        CONFIG_KV_NULL,
        CONFIG_KV_BAD_CAPACITY,
        CONFIG_KV_BAD_KIND,
        CONFIG_KV_BAD_KEY,
        CONFIG_KV_BAD_VALUE,
        CONFIG_KV_BAD_STATE,
        CONFIG_KV_NOT_FOUND,
        CONFIG_KV_FULL,
        CONFIG_KV_TRUNCATED,
    };
    struct config_kv_entry entries[TEST_CAPACITY];
    struct config_kv kv;
    unsigned char value[CONFIG_KV_VALUE_MAX];
    size_t value_length = 0U;
    uint64_t revision = 0U;
    bool replaced = false;
    bool removed = false;
    size_t count = 0U;
    size_t index;

    memset(entries, 0, sizeof(entries));
    memset(value, 0, sizeof(value));

    CHECK(config_kv_init(NULL, entries, TEST_CAPACITY) ==
              CONFIG_KV_NULL,
          "kv init rejects null kv");
    CHECK(config_kv_init(&kv, NULL, TEST_CAPACITY) ==
              CONFIG_KV_NULL,
          "kv init rejects null entries");
    CHECK(config_kv_init(&kv, entries, 0U) ==
              CONFIG_KV_BAD_CAPACITY,
          "kv init rejects zero capacity");
    CHECK(config_kv_init(&kv, entries, CONFIG_KV_CAPACITY_MAX + 1U) ==
              CONFIG_KV_BAD_CAPACITY,
          "kv init rejects over-limit capacity");
    CHECK(config_kv_init(&kv, entries, TEST_CAPACITY) == CONFIG_KV_OK &&
              kv.initialized &&
              kv.count == 0U,
          "kv init publishes empty bounded table");

    CHECK(config_kv_put(NULL, CONFIG_KV_KIND_CONFIG,
                        "alpha", 5U, "one", 3U, &replaced) ==
              CONFIG_KV_NULL,
          "kv put rejects null kv");
    CHECK(config_kv_put(&kv, CONFIG_KV_KIND_CONFIG,
                        NULL, 5U, "one", 3U, &replaced) ==
              CONFIG_KV_NULL,
          "kv put rejects null key");
    CHECK(config_kv_put(&kv, CONFIG_KV_KIND_CONFIG,
                        "alpha", 0U, "one", 3U, &replaced) ==
              CONFIG_KV_BAD_KEY,
          "kv put rejects empty key");
    CHECK(config_kv_put(&kv, CONFIG_KV_KIND_CONFIG,
                        value, CONFIG_KV_KEY_MAX + 1U,
                        "one", 3U, &replaced) == CONFIG_KV_BAD_KEY,
          "kv put rejects over-limit key");
    CHECK(config_kv_put(&kv, CONFIG_KV_KIND_CONFIG,
                        "alpha", 5U, NULL, 3U, &replaced) ==
              CONFIG_KV_NULL,
          "kv put rejects null value");
    CHECK(config_kv_put(&kv, CONFIG_KV_KIND_CONFIG,
                        "alpha", 5U, value,
                        CONFIG_KV_VALUE_MAX + 1U, &replaced) ==
              CONFIG_KV_BAD_VALUE,
          "kv put rejects over-limit value");
    CHECK(config_kv_put(&kv, CONFIG_KV_KIND_COUNT,
                        "alpha", 5U, "one", 3U, &replaced) ==
              CONFIG_KV_BAD_KIND,
          "kv put rejects bad kind");
    CHECK(config_kv_put(&kv, CONFIG_KV_KIND_CONFIG,
                        "alpha", 5U, "one", 3U, NULL) ==
              CONFIG_KV_NULL,
          "kv put rejects null replaced output");
    memset(&kv, 0, sizeof(kv));
    CHECK(config_kv_put(&kv, CONFIG_KV_KIND_CONFIG,
                        "alpha", 5U, "one", 3U, &replaced) ==
              CONFIG_KV_BAD_STATE,
          "uninitialized kv rejects put");
    config_kv_init(&kv, entries, TEST_CAPACITY);

    CHECK(config_kv_get(NULL, CONFIG_KV_KIND_CONFIG,
                        "alpha", 5U, value, sizeof(value),
                        &value_length, &revision) == CONFIG_KV_NULL,
          "kv get rejects null kv");
    CHECK(config_kv_get(&kv, CONFIG_KV_KIND_CONFIG,
                        NULL, 5U, value, sizeof(value),
                        &value_length, &revision) == CONFIG_KV_NULL,
          "kv get rejects null key");
    CHECK(config_kv_get(&kv, CONFIG_KV_KIND_CONFIG,
                        "alpha", 5U, NULL, sizeof(value),
                        &value_length, &revision) == CONFIG_KV_NULL,
          "kv get rejects null output");
    CHECK(config_kv_get(&kv, CONFIG_KV_KIND_CONFIG,
                        "alpha", 5U, value, sizeof(value),
                        NULL, &revision) == CONFIG_KV_NULL,
          "kv get rejects null length output");
    CHECK(config_kv_remove(NULL, CONFIG_KV_KIND_CONFIG,
                           "alpha", 5U, &removed) == CONFIG_KV_NULL,
          "kv remove rejects null kv");
    CHECK(config_kv_remove(&kv, CONFIG_KV_KIND_CONFIG,
                           NULL, 5U, &removed) == CONFIG_KV_NULL,
          "kv remove rejects null key");
    CHECK(config_kv_remove(&kv, CONFIG_KV_KIND_CONFIG,
                           "alpha", 5U, NULL) == CONFIG_KV_NULL,
          "kv remove rejects null removed output");
    CHECK(config_kv_count(NULL, &count) == CONFIG_KV_NULL,
          "kv count rejects null kv");
    CHECK(config_kv_count(&kv, NULL) == CONFIG_KV_NULL,
          "kv count rejects null count");

    for (index = 0U; index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        CHECK(config_kv_status_string(statuses[index]) != NULL &&
                  strcmp(config_kv_status_string(statuses[index]),
                         statuses[index] == CONFIG_KV_OK ?
                             "CONFIG_KV_OK" :
                         statuses[index] == CONFIG_KV_NULL ?
                             "CONFIG_KV_NULL" :
                         statuses[index] == CONFIG_KV_BAD_CAPACITY ?
                             "CONFIG_KV_BAD_CAPACITY" :
                         statuses[index] == CONFIG_KV_BAD_KIND ?
                             "CONFIG_KV_BAD_KIND" :
                         statuses[index] == CONFIG_KV_BAD_KEY ?
                             "CONFIG_KV_BAD_KEY" :
                         statuses[index] == CONFIG_KV_BAD_VALUE ?
                             "CONFIG_KV_BAD_VALUE" :
                         statuses[index] == CONFIG_KV_BAD_STATE ?
                             "CONFIG_KV_BAD_STATE" :
                         statuses[index] == CONFIG_KV_NOT_FOUND ?
                             "CONFIG_KV_NOT_FOUND" :
                         statuses[index] == CONFIG_KV_FULL ?
                             "CONFIG_KV_FULL" :
                             "CONFIG_KV_TRUNCATED") == 0,
              "config-kv status string is exact");
    }
    CHECK(strcmp(config_kv_status_string(
                     (enum config_kv_status)99U),
                 "CONFIG_KV_UNKNOWN") == 0,
          "config-kv undefined status string is exact");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "kv validation performs no allocation");
}

static void test_exhaustion_replacement_and_removal(void) {
    struct config_kv_entry entries[TEST_CAPACITY];
    struct config_kv kv;
    unsigned char value[CONFIG_KV_VALUE_MAX];
    size_t value_length = 0U;
    uint64_t revision = 0U;
    bool replaced = false;
    bool removed = false;
    size_t count = 0U;

    memset(entries, 0, sizeof(entries));
    memset(value, 0xA5, sizeof(value));
    CHECK(config_kv_init(&kv, entries, TEST_CAPACITY) == CONFIG_KV_OK &&
              config_kv_count(&kv, &count) == CONFIG_KV_OK &&
              count == 0U,
          "kv fixture initializes empty table");

    CHECK(config_kv_put(&kv, CONFIG_KV_KIND_CONFIG,
                        "alpha", 5U, "one", 3U, &replaced) ==
              CONFIG_KV_OK &&
              !replaced &&
              config_kv_count(&kv, &count) == CONFIG_KV_OK &&
              count == 1U,
          "first config checkpoint put occupies one slot");
    CHECK(config_kv_put(&kv, CONFIG_KV_KIND_AUDIT,
                        "alpha", 5U, "audit-one", 9U, &replaced) ==
              CONFIG_KV_OK &&
              !replaced &&
              config_kv_count(&kv, &count) == CONFIG_KV_OK &&
              count == 2U,
          "same key in audit kind occupies a separate slot");
    CHECK(config_kv_put(&kv, CONFIG_KV_KIND_CONFIG,
                        "beta", 4U, "two", 3U, &replaced) ==
              CONFIG_KV_OK &&
              config_kv_put(&kv, CONFIG_KV_KIND_AUDIT,
                            "beta", 4U, "audit-two", 9U, &replaced) ==
                  CONFIG_KV_OK &&
              config_kv_count(&kv, &count) == CONFIG_KV_OK &&
              count == TEST_CAPACITY,
          "config and audit checkpoints fill bounded capacity");

    CHECK(config_kv_put(&kv, CONFIG_KV_KIND_CONFIG,
                        "gamma", 5U, "full", 4U, &replaced) ==
              CONFIG_KV_FULL &&
              config_kv_count(&kv, &count) == CONFIG_KV_OK &&
              count == TEST_CAPACITY,
          "full kv put rejects without adding a slot");
    CHECK(config_kv_get(&kv, CONFIG_KV_KIND_CONFIG,
                        "alpha", 5U, value, sizeof(value),
                        &value_length, &revision) == CONFIG_KV_OK &&
              value_length == 3U &&
              entry_equal(value, (const unsigned char *)"one", 3U),
          "full kv put leaves existing config value readable");
    CHECK(config_kv_get(&kv, CONFIG_KV_KIND_AUDIT,
                        "alpha", 5U, value, sizeof(value),
                        &value_length, &revision) == CONFIG_KV_OK &&
              value_length == 9U &&
              entry_equal(value, (const unsigned char *)"audit-one", 9U),
          "full kv put leaves existing audit value readable");

    CHECK(config_kv_put(&kv, CONFIG_KV_KIND_CONFIG,
                        "alpha", 5U, "new-one", 7U, &replaced) ==
              CONFIG_KV_OK &&
              replaced &&
              config_kv_count(&kv, &count) == CONFIG_KV_OK &&
              count == TEST_CAPACITY,
          "replacement releases old entry without creating duplicate");
    CHECK(config_kv_get(&kv, CONFIG_KV_KIND_CONFIG,
                        "alpha", 5U, value, sizeof(value),
                        &value_length, &revision) == CONFIG_KV_OK &&
              value_length == 7U &&
              entry_equal(value, (const unsigned char *)"new-one", 7U) &&
              revision == 2U,
          "replacement publishes only the new value and next revision");
    CHECK(config_kv_get(&kv, CONFIG_KV_KIND_AUDIT,
                        "alpha", 5U, value, sizeof(value),
                        &value_length, &revision) == CONFIG_KV_OK &&
              value_length == 9U &&
              entry_equal(value, (const unsigned char *)"audit-one", 9U),
          "replacement does not release the audit-kind checkpoint");

    memset(value, 0x5A, sizeof(value));
    CHECK(config_kv_get(&kv, CONFIG_KV_KIND_CONFIG,
                        "alpha", 5U, value, 4U,
                        &value_length, &revision) ==
              CONFIG_KV_TRUNCATED &&
              entry_equal(value, (const unsigned char *)"\x5A\x5A\x5A", 3U),
          "too-small kv get rejects without partial output");

    CHECK(config_kv_remove(&kv, CONFIG_KV_KIND_CONFIG,
                           "beta", 4U, &removed) == CONFIG_KV_OK &&
              removed &&
              config_kv_count(&kv, &count) == CONFIG_KV_OK &&
              count == TEST_CAPACITY - 1U,
          "remove releases one config checkpoint slot");
    CHECK(config_kv_get(&kv, CONFIG_KV_KIND_CONFIG,
                        "beta", 4U, value, sizeof(value),
                        &value_length, &revision) ==
              CONFIG_KV_NOT_FOUND,
          "removed config key is no longer readable");
    CHECK(config_kv_get(&kv, CONFIG_KV_KIND_AUDIT,
                        "beta", 4U, value, sizeof(value),
                        &value_length, &revision) == CONFIG_KV_OK &&
              value_length == 9U,
          "remove does not release the audit-kind checkpoint");
    CHECK(config_kv_remove(&kv, CONFIG_KV_KIND_CONFIG,
                           "beta", 4U, &removed) ==
              CONFIG_KV_NOT_FOUND &&
              !removed,
          "removing a missing key is not-found and not a release");

    CHECK(config_kv_put(&kv, CONFIG_KV_KIND_CONFIG,
                        "gamma", 5U, "reused", 6U, &replaced) ==
              CONFIG_KV_OK &&
              !replaced &&
              config_kv_count(&kv, &count) == CONFIG_KV_OK &&
              count == TEST_CAPACITY,
          "removed slot is reusable exactly once");
    CHECK(config_kv_get(&kv, CONFIG_KV_KIND_CONFIG,
                        "gamma", 5U, value, sizeof(value),
                        &value_length, &revision) == CONFIG_KV_OK &&
              value_length == 6U &&
              entry_equal(value, (const unsigned char *)"reused", 6U) &&
              revision == 1U,
          "reused slot publishes exactly one new checkpoint");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "kv lifecycle performs no allocation");
}

int main(void) {
    test_validation_and_status_strings();
    test_exhaustion_replacement_and_removal();
    if (failures != 0) {
        fprintf(stderr, "Config-KV host tests: FAILED (%d)\n", failures);
        return 1;
    }
    printf("Config-KV host tests: ALL OK\n");
    return 0;
}
