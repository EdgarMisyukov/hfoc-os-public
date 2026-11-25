#include "state_schema.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_STATE_SCHEMA_HOST_TEST

#define TEST_BLOCK_SIZE 512U
#define TEST_BLOCK_COUNT 8U
#define TEST_CAPACITY (TEST_BLOCK_SIZE * TEST_BLOCK_COUNT)
#define TEST_REGION_OFFSET 1U
#define TEST_REGION_COUNT 7U

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

struct fixture_device {
    unsigned char storage[TEST_CAPACITY];
    size_t read_calls;
    size_t write_calls;
};

static enum block_device_status fixture_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct fixture_device *fixture =
        (struct fixture_device *)device->context;

    ++fixture->read_calls;
    memcpy(buffer, fixture->storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct fixture_device *fixture =
        (struct fixture_device *)device->context;

    ++fixture->write_calls;
    memcpy(fixture->storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static struct block_device make_device(struct fixture_device *fixture) {
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = TEST_BLOCK_SIZE;
    device.block_count = TEST_BLOCK_COUNT;
    device.context = fixture;
    device.read_fn = fixture_read;
    device.write_fn = fixture_write;
    return device;
}

static struct state_schema_descriptor make_descriptor(
    uint8_t kind,
    uint8_t target_kind,
    const char *id,
    uint32_t version,
    uint32_t minimum_version,
    uint32_t maximum_version,
    uint16_t minimum_length,
    uint16_t maximum_length)
{
    struct state_schema_descriptor descriptor;
    size_t id_length = strlen(id);

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.kind = kind;
    descriptor.target_kind = target_kind;
    descriptor.id_length = (uint8_t)id_length;
    memcpy(descriptor.id, id, id_length);
    descriptor.version = version;
    descriptor.minimum_version = minimum_version;
    descriptor.maximum_version = maximum_version;
    descriptor.minimum_length = minimum_length;
    descriptor.maximum_length = maximum_length;
    return descriptor;
}

static void init_test_descriptors(
    struct state_schema_descriptor *descriptors)
{
    descriptors[0] = make_descriptor(
        STATE_SCHEMA_KIND_JOURNAL,
        STATE_SCHEMA_TARGET_JOURNAL,
        "config", 1U, 1U, 1U, 1U, CONFIG_JOURNAL_DATA_MAX);
    descriptors[1] = make_descriptor(
        STATE_SCHEMA_KIND_KV,
        CONFIG_KV_KIND_CONFIG,
        "config", 1U, 1U, 1U, 1U, CONFIG_KV_VALUE_MAX);
    descriptors[2] = make_descriptor(
        STATE_SCHEMA_KIND_KV,
        CONFIG_KV_KIND_AUDIT,
        "audit", 1U, 1U, 1U, 1U, CONFIG_KV_VALUE_MAX);
}

static bool descriptor_equal(const struct state_schema_descriptor *left,
                             const struct state_schema_descriptor *right)
{
    return left->kind == right->kind &&
           left->target_kind == right->target_kind &&
           left->id_length == right->id_length &&
           memcmp(left->id, right->id, sizeof(left->id)) == 0 &&
           left->version == right->version &&
           left->minimum_version == right->minimum_version &&
           left->maximum_version == right->maximum_version &&
           left->minimum_length == right->minimum_length &&
           left->maximum_length == right->maximum_length;
}

static void test_registry_validation_and_status_strings(void) {
    static const enum state_schema_status statuses[] = {
        STATE_SCHEMA_OK,
        STATE_SCHEMA_NULL,
        STATE_SCHEMA_BAD_KIND,
        STATE_SCHEMA_BAD_ID,
        STATE_SCHEMA_BAD_VERSION,
        STATE_SCHEMA_BAD_LENGTH,
        STATE_SCHEMA_BAD_CAPACITY,
        STATE_SCHEMA_DUPLICATE,
        STATE_SCHEMA_BAD_STATE,
        STATE_SCHEMA_UNKNOWN,
        STATE_SCHEMA_UNSUPPORTED_VERSION,
        STATE_SCHEMA_RECORD_TOO_SHORT,
        STATE_SCHEMA_RECORD_TOO_LONG,
        STATE_SCHEMA_PUBLISH_FAILED,
    };
    struct state_schema_descriptor descriptors[3];
    struct state_schema_descriptor bad_descriptor;
    struct state_schema_registry registry;
    unsigned char before[sizeof(registry)];
    size_t index;

    init_test_descriptors(descriptors);
    memset(&registry, 0, sizeof(registry));

    CHECK(state_schema_registry_init(NULL, descriptors, 3U) ==
              STATE_SCHEMA_NULL,
          "registry init rejects null registry");
    CHECK(state_schema_registry_init(&registry, NULL, 0U) ==
              STATE_SCHEMA_BAD_CAPACITY,
          "registry init rejects zero capacity before descriptor check");
    CHECK(state_schema_registry_init(&registry, NULL, 1U) ==
              STATE_SCHEMA_NULL,
          "registry init rejects null descriptors with count");
    CHECK(state_schema_registry_init(&registry, descriptors,
                                     STATE_SCHEMA_REGISTRY_CAPACITY_MAX + 1U)
              == STATE_SCHEMA_BAD_CAPACITY,
          "registry init rejects over-limit capacity");

    bad_descriptor = make_descriptor(
        STATE_SCHEMA_KIND_COUNT,
        STATE_SCHEMA_TARGET_JOURNAL,
        "config", 1U, 1U, 1U, 1U, CONFIG_JOURNAL_DATA_MAX);
    CHECK(state_schema_registry_init(&registry, &bad_descriptor, 1U) ==
              STATE_SCHEMA_BAD_KIND,
          "registry init rejects bad kind");

    bad_descriptor = make_descriptor(
        STATE_SCHEMA_KIND_KV,
        CONFIG_KV_KIND_COUNT,
        "config", 1U, 1U, 1U, 1U, CONFIG_KV_VALUE_MAX);
    CHECK(state_schema_registry_init(&registry, &bad_descriptor, 1U) ==
              STATE_SCHEMA_BAD_KIND,
          "registry init rejects bad target kind");

    bad_descriptor = make_descriptor(
        STATE_SCHEMA_KIND_JOURNAL,
        STATE_SCHEMA_TARGET_JOURNAL,
        "", 1U, 1U, 1U, 1U, CONFIG_JOURNAL_DATA_MAX);
    CHECK(state_schema_registry_init(&registry, &bad_descriptor, 1U) ==
              STATE_SCHEMA_BAD_ID,
          "registry init rejects empty id");
    bad_descriptor = make_descriptor(
        STATE_SCHEMA_KIND_JOURNAL,
        STATE_SCHEMA_TARGET_JOURNAL,
        "abcdefghijklmnopq", 1U, 1U, 1U, 1U, CONFIG_JOURNAL_DATA_MAX);
    CHECK(state_schema_registry_init(&registry, &bad_descriptor, 1U) ==
              STATE_SCHEMA_BAD_ID,
          "registry init rejects over-limit id");
    bad_descriptor = make_descriptor(
        STATE_SCHEMA_KIND_JOURNAL,
        STATE_SCHEMA_TARGET_JOURNAL,
        "bad-id", 1U, 1U, 1U, 1U, CONFIG_JOURNAL_DATA_MAX);
    CHECK(state_schema_registry_init(&registry, &bad_descriptor, 1U) ==
              STATE_SCHEMA_BAD_ID,
          "registry init rejects non-alphanumeric id");

    bad_descriptor = make_descriptor(
        STATE_SCHEMA_KIND_JOURNAL,
        STATE_SCHEMA_TARGET_JOURNAL,
        "config", 0U, 1U, 1U, 1U, CONFIG_JOURNAL_DATA_MAX);
    CHECK(state_schema_registry_init(&registry, &bad_descriptor, 1U) ==
              STATE_SCHEMA_BAD_VERSION,
          "registry init rejects zero version");
    bad_descriptor = make_descriptor(
        STATE_SCHEMA_KIND_JOURNAL,
        STATE_SCHEMA_TARGET_JOURNAL,
        "config", 2U, 1U, 1U, 1U, CONFIG_JOURNAL_DATA_MAX);
    CHECK(state_schema_registry_init(&registry, &bad_descriptor, 1U) ==
              STATE_SCHEMA_BAD_VERSION,
          "registry init rejects version outside compatibility bounds");

    bad_descriptor = make_descriptor(
        STATE_SCHEMA_KIND_JOURNAL,
        STATE_SCHEMA_TARGET_JOURNAL,
        "config", 1U, 1U, 1U, 2U, 1U);
    CHECK(state_schema_registry_init(&registry, &bad_descriptor, 1U) ==
              STATE_SCHEMA_BAD_LENGTH,
          "registry init rejects inverted length bounds");
    bad_descriptor = make_descriptor(
        STATE_SCHEMA_KIND_JOURNAL,
        STATE_SCHEMA_TARGET_JOURNAL,
        "config", 1U, 1U, 1U, 1U,
        STATE_SCHEMA_RECORD_LENGTH_MAX + 1U);
    CHECK(state_schema_registry_init(&registry, &bad_descriptor, 1U) ==
              STATE_SCHEMA_BAD_LENGTH,
          "registry init rejects over-limit length bound");

    memset(&registry, 0x5A, sizeof(registry));
    memcpy(before, &registry, sizeof(before));
    descriptors[0].id_length = 0U;
    CHECK(state_schema_registry_init(&registry, descriptors, 3U) ==
              STATE_SCHEMA_BAD_ID &&
              memcmp(&registry, before, sizeof(before)) == 0,
          "failed registry init leaves destination unchanged");
    init_test_descriptors(descriptors);
    descriptors[2] = descriptors[1];
    CHECK(state_schema_registry_init(&registry, descriptors, 3U) ==
              STATE_SCHEMA_DUPLICATE,
          "registry init rejects duplicate ids in one kind");

    init_test_descriptors(descriptors);
    CHECK(state_schema_registry_init(&registry, descriptors, 3U) ==
              STATE_SCHEMA_OK &&
              registry.initialized &&
              registry.count == 3U &&
              descriptor_equal(&registry.descriptors[0],
                               &descriptors[0]) &&
              descriptor_equal(&registry.descriptors[1],
                               &descriptors[1]) &&
              descriptor_equal(&registry.descriptors[2],
                               &descriptors[2]),
          "registry init publishes exact immutable snapshot");

    descriptors[0].version = 99U;
    CHECK(state_schema_validate_record(
              &registry,
              STATE_SCHEMA_KIND_JOURNAL,
              STATE_SCHEMA_TARGET_JOURNAL,
              "config", 6U, 1U, 3U) == STATE_SCHEMA_OK &&
              registry.descriptors[0].version == 1U,
          "registry snapshot does not change after source mutation");

    memset(&registry, 0, sizeof(registry));
    CHECK(state_schema_validate_record(
              &registry,
              STATE_SCHEMA_KIND_JOURNAL,
              STATE_SCHEMA_TARGET_JOURNAL,
              "config", 6U, 1U, 3U) == STATE_SCHEMA_BAD_STATE,
          "record validation rejects uninitialized registry");
    init_test_descriptors(descriptors);
    state_schema_registry_init(&registry, descriptors, 3U);

    CHECK(state_schema_validate_record(
              NULL, STATE_SCHEMA_KIND_JOURNAL,
              STATE_SCHEMA_TARGET_JOURNAL,
              "config", 6U, 1U, 3U) == STATE_SCHEMA_NULL,
          "record validation rejects null registry");
    CHECK(state_schema_validate_record(
              &registry, STATE_SCHEMA_KIND_COUNT,
              STATE_SCHEMA_TARGET_JOURNAL,
              "config", 6U, 1U, 3U) == STATE_SCHEMA_BAD_KIND,
          "record validation rejects bad kind");
    CHECK(state_schema_validate_record(
              &registry, STATE_SCHEMA_KIND_KV,
              CONFIG_KV_KIND_COUNT,
              "config", 6U, 1U, 3U) == STATE_SCHEMA_BAD_KIND,
          "record validation rejects bad target kind");
    CHECK(state_schema_validate_record(
              &registry, STATE_SCHEMA_KIND_JOURNAL,
              STATE_SCHEMA_TARGET_JOURNAL,
              NULL, 6U, 1U, 3U) == STATE_SCHEMA_NULL,
          "record validation rejects null id");
    CHECK(state_schema_validate_record(
              &registry, STATE_SCHEMA_KIND_JOURNAL,
              STATE_SCHEMA_TARGET_JOURNAL,
              "", 0U, 1U, 3U) == STATE_SCHEMA_BAD_ID,
          "record validation rejects empty id");
    CHECK(state_schema_validate_record(
              &registry, STATE_SCHEMA_KIND_JOURNAL,
              STATE_SCHEMA_TARGET_JOURNAL,
              "abcdefghijklmnopq", 17U, 1U, 3U) ==
              STATE_SCHEMA_BAD_ID,
          "record validation rejects over-limit id");
    CHECK(state_schema_validate_record(
              &registry, STATE_SCHEMA_KIND_JOURNAL,
              STATE_SCHEMA_TARGET_JOURNAL,
              "bad-id", 6U, 1U, 3U) == STATE_SCHEMA_BAD_ID,
          "record validation rejects non-alphanumeric id");
    CHECK(state_schema_validate_record(
              &registry, STATE_SCHEMA_KIND_JOURNAL,
              STATE_SCHEMA_TARGET_JOURNAL,
              "config", 6U, 0U, 3U) == STATE_SCHEMA_BAD_VERSION,
          "record validation rejects zero version");
    CHECK(state_schema_validate_record(
              &registry, STATE_SCHEMA_KIND_JOURNAL,
              STATE_SCHEMA_TARGET_JOURNAL,
              "missing", 7U, 1U, 3U) == STATE_SCHEMA_UNKNOWN,
          "record validation rejects unknown schema id");
    CHECK(state_schema_validate_record(
              &registry, STATE_SCHEMA_KIND_KV,
              CONFIG_KV_KIND_AUDIT,
              "config", 6U, 1U, 3U) == STATE_SCHEMA_BAD_KIND,
          "record validation rejects schema id for wrong target");
    CHECK(state_schema_validate_record(
              &registry, STATE_SCHEMA_KIND_JOURNAL,
              STATE_SCHEMA_TARGET_JOURNAL,
              "config", 6U, 2U, 3U) ==
              STATE_SCHEMA_UNSUPPORTED_VERSION,
          "record validation rejects unsupported version");
    CHECK(state_schema_validate_record(
              &registry, STATE_SCHEMA_KIND_JOURNAL,
              STATE_SCHEMA_TARGET_JOURNAL,
              "config", 6U, 1U, 0U) ==
              STATE_SCHEMA_RECORD_TOO_SHORT,
          "record validation rejects too-short record");
    CHECK(state_schema_validate_record(
              &registry, STATE_SCHEMA_KIND_JOURNAL,
              STATE_SCHEMA_TARGET_JOURNAL,
              "config", 6U, 1U, CONFIG_JOURNAL_DATA_MAX + 1U) ==
              STATE_SCHEMA_RECORD_TOO_LONG,
          "record validation rejects too-long record");
    CHECK(state_schema_validate_record(
              &registry, STATE_SCHEMA_KIND_KV,
              CONFIG_KV_KIND_CONFIG,
              "config", 6U, 1U, 3U) == STATE_SCHEMA_OK,
          "record validation accepts known compatible kv record");

    for (index = 0U; index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        CHECK(state_schema_status_string(statuses[index]) != NULL &&
                  strcmp(state_schema_status_string(statuses[index]),
                         statuses[index] == STATE_SCHEMA_OK ?
                             "STATE_SCHEMA_OK" :
                         statuses[index] == STATE_SCHEMA_NULL ?
                             "STATE_SCHEMA_NULL" :
                         statuses[index] == STATE_SCHEMA_BAD_KIND ?
                             "STATE_SCHEMA_BAD_KIND" :
                         statuses[index] == STATE_SCHEMA_BAD_ID ?
                             "STATE_SCHEMA_BAD_ID" :
                         statuses[index] == STATE_SCHEMA_BAD_VERSION ?
                             "STATE_SCHEMA_BAD_VERSION" :
                         statuses[index] == STATE_SCHEMA_BAD_LENGTH ?
                             "STATE_SCHEMA_BAD_LENGTH" :
                         statuses[index] == STATE_SCHEMA_BAD_CAPACITY ?
                             "STATE_SCHEMA_BAD_CAPACITY" :
                         statuses[index] == STATE_SCHEMA_DUPLICATE ?
                             "STATE_SCHEMA_DUPLICATE" :
                         statuses[index] == STATE_SCHEMA_BAD_STATE ?
                             "STATE_SCHEMA_BAD_STATE" :
                         statuses[index] == STATE_SCHEMA_UNKNOWN ?
                             "STATE_SCHEMA_UNKNOWN" :
                         statuses[index] ==
                             STATE_SCHEMA_UNSUPPORTED_VERSION ?
                             "STATE_SCHEMA_UNSUPPORTED_VERSION" :
                         statuses[index] ==
                             STATE_SCHEMA_RECORD_TOO_SHORT ?
                             "STATE_SCHEMA_RECORD_TOO_SHORT" :
                         statuses[index] ==
                             STATE_SCHEMA_RECORD_TOO_LONG ?
                             "STATE_SCHEMA_RECORD_TOO_LONG" :
                             "STATE_SCHEMA_PUBLISH_FAILED") == 0,
              "state-schema status string is exact");
    }
    CHECK(strcmp(state_schema_status_string(
                     (enum state_schema_status)99U),
                 "STATE_SCHEMA_UNKNOWN") == 0,
          "state-schema undefined status string is exact");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "registry validation performs no allocation");
}

static void test_journal_publication_guard(void) {
    struct state_schema_descriptor descriptors[1];
    struct state_schema_registry registry;
    struct fixture_device fixture;
    struct block_device device;
    struct config_journal journal;
    bool replaced = false;
    size_t count = 0U;

    descriptors[0] = make_descriptor(
        STATE_SCHEMA_KIND_JOURNAL,
        STATE_SCHEMA_TARGET_JOURNAL,
        "config", 1U, 1U, 1U, 1U, CONFIG_JOURNAL_DATA_MAX);
    CHECK(state_schema_registry_init(&registry, descriptors, 1U) ==
              STATE_SCHEMA_OK,
          "journal guard registry initializes");

    memset(&fixture, 0, sizeof(fixture));
    device = make_device(&fixture);
    memset(&journal, 0, sizeof(journal));
    CHECK(config_journal_init(&journal, &device, TEST_REGION_OFFSET,
                              TEST_REGION_COUNT) == CONFIG_JOURNAL_OK &&
              journal.next_block == 0U,
          "journal guard fixture initializes empty journal");

    CHECK(state_schema_append_journal(
              &journal, &registry,
              "missing", 7U, 1U, "x", 1U) == STATE_SCHEMA_UNKNOWN &&
              fixture.write_calls == 0U &&
              journal.next_block == 0U,
          "journal guard rejects unknown schema before write");
    CHECK(state_schema_append_journal(
              &journal, &registry,
              "config", 6U, 2U, "x", 1U) ==
              STATE_SCHEMA_UNSUPPORTED_VERSION &&
              fixture.write_calls == 0U &&
              journal.next_block == 0U,
          "journal guard rejects unsupported version before write");
    CHECK(state_schema_append_journal(
              &journal, &registry,
              "config", 6U, 1U, "x", 0U) ==
              STATE_SCHEMA_RECORD_TOO_SHORT &&
              fixture.write_calls == 0U &&
              journal.next_block == 0U,
          "journal guard rejects short record before write");
    CHECK(state_schema_append_journal(
              &journal, &registry,
              "config", 6U, 1U, "x",
              CONFIG_JOURNAL_DATA_MAX + 1U) ==
              STATE_SCHEMA_RECORD_TOO_LONG &&
              fixture.write_calls == 0U &&
              journal.next_block == 0U,
          "journal guard rejects long record before write");
    CHECK(state_schema_append_journal(
              &journal, &registry,
              "config", 6U, 1U, "one", 3U) == STATE_SCHEMA_OK &&
              fixture.write_calls == 1U &&
              journal.next_block == 1U,
          "journal guard accepts known compatible record");

    (void)replaced;
    (void)count;
}

static void test_kv_publication_guard(void) {
    struct state_schema_descriptor descriptors[2];
    struct state_schema_registry registry;
    struct config_kv_entry entries[4];
    struct config_kv kv;
    bool replaced = false;
    size_t count = 0U;

    descriptors[0] = make_descriptor(
        STATE_SCHEMA_KIND_KV,
        CONFIG_KV_KIND_CONFIG,
        "config", 1U, 1U, 1U, 1U, CONFIG_KV_VALUE_MAX);
    descriptors[1] = make_descriptor(
        STATE_SCHEMA_KIND_KV,
        CONFIG_KV_KIND_AUDIT,
        "audit", 1U, 1U, 1U, 1U, CONFIG_KV_VALUE_MAX);
    CHECK(state_schema_registry_init(&registry, descriptors, 2U) ==
              STATE_SCHEMA_OK,
          "kv guard registry initializes");

    memset(entries, 0, sizeof(entries));
    memset(&kv, 0, sizeof(kv));
    CHECK(config_kv_init(&kv, entries, 4U) == CONFIG_KV_OK,
          "kv guard fixture initializes empty table");

    CHECK(state_schema_kv_put(
              &kv, &registry,
              "missing", 7U, 1U, CONFIG_KV_KIND_CONFIG,
              "alpha", 5U, "one", 3U, &replaced) ==
              STATE_SCHEMA_UNKNOWN &&
              config_kv_count(&kv, &count) == CONFIG_KV_OK &&
              count == 0U,
          "kv guard rejects unknown schema before mutation");
    CHECK(state_schema_kv_put(
              &kv, &registry,
              "config", 6U, 2U, CONFIG_KV_KIND_CONFIG,
              "alpha", 5U, "one", 3U, &replaced) ==
              STATE_SCHEMA_UNSUPPORTED_VERSION &&
              config_kv_count(&kv, &count) == CONFIG_KV_OK &&
              count == 0U,
          "kv guard rejects unsupported version before mutation");
    CHECK(state_schema_kv_put(
              &kv, &registry,
              "config", 6U, 1U, CONFIG_KV_KIND_AUDIT,
              "alpha", 5U, "one", 3U, &replaced) ==
              STATE_SCHEMA_BAD_KIND &&
              config_kv_count(&kv, &count) == CONFIG_KV_OK &&
              count == 0U,
          "kv guard rejects schema id for wrong target before mutation");
    CHECK(state_schema_kv_put(
              &kv, &registry,
              "config", 6U, 1U, CONFIG_KV_KIND_CONFIG,
              "alpha", 5U, "", 0U, &replaced) ==
              STATE_SCHEMA_RECORD_TOO_SHORT &&
              config_kv_count(&kv, &count) == CONFIG_KV_OK &&
              count == 0U,
          "kv guard rejects short value before mutation");
    CHECK(state_schema_kv_put(
              &kv, &registry,
              "config", 6U, 1U, CONFIG_KV_KIND_CONFIG,
              "alpha", 5U, "too-long",
              CONFIG_KV_VALUE_MAX + 1U, &replaced) ==
              STATE_SCHEMA_RECORD_TOO_LONG &&
              config_kv_count(&kv, &count) == CONFIG_KV_OK &&
              count == 0U,
          "kv guard rejects long value before mutation");

    CHECK(state_schema_kv_put(
              &kv, &registry,
              "config", 6U, 1U, CONFIG_KV_KIND_CONFIG,
              "alpha", 5U, "one", 3U, &replaced) ==
              STATE_SCHEMA_OK &&
              !replaced &&
              config_kv_count(&kv, &count) == CONFIG_KV_OK &&
              count == 1U,
          "kv guard accepts known config record");
    CHECK(state_schema_kv_put(
              &kv, &registry,
              "audit", 5U, 1U, CONFIG_KV_KIND_AUDIT,
              "alpha", 5U, "audit-one", 9U, &replaced) ==
              STATE_SCHEMA_OK &&
              config_kv_count(&kv, &count) == CONFIG_KV_OK &&
              count == 2U,
          "kv guard accepts known audit record");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "publication guards perform no allocation");
}

int main(void) {
    test_registry_validation_and_status_strings();
    test_journal_publication_guard();
    test_kv_publication_guard();

    if (failures != 0) {
        fprintf(stderr, "State schema host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("State schema host tests: ALL OK");
    return 0;
}

#endif
