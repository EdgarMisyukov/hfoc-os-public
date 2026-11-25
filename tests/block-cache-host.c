#include "block_cache.h"
#include "block_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_BLOCK_CACHE_HOST_TEST

#define TEST_BLOCK_SIZE 512U
#define TEST_BLOCK_COUNT 8U
#define TEST_CAPACITY (TEST_BLOCK_SIZE * TEST_BLOCK_COUNT)
#define TEST_CACHE_CAPACITY 3U
#define TEST_CACHE_STORAGE_SIZE \
    (TEST_CACHE_CAPACITY * TEST_BLOCK_SIZE)

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
    bool fail_read;
    bool fail_write;
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
    if (fixture->fail_read) {
        fixture->fail_read = false;
        return BLOCK_DEVICE_IO_ERROR;
    }
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
    if (fixture->fail_write) {
        fixture->fail_write = false;
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(fixture->storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static void init_fixture(struct fixture_device *fixture) {
    uint64_t block;
    size_t offset;

    memset(fixture, 0, sizeof(*fixture));
    for (block = 0U; block < TEST_BLOCK_COUNT; ++block) {
        for (offset = 0U; offset < TEST_BLOCK_SIZE; ++offset) {
            fixture->storage[(size_t)block * TEST_BLOCK_SIZE + offset] =
                (unsigned char)(0x11U + (unsigned int)block);
        }
    }
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

static enum block_cache_status init_cache(
    struct block_cache *cache,
    struct block_device *device,
    struct block_cache_entry *entries,
    unsigned char *storage)
{
    memset(cache, 0, sizeof(*cache));
    return block_cache_init(cache, device, entries, TEST_CACHE_CAPACITY,
                            storage, TEST_CACHE_STORAGE_SIZE);
}

static unsigned int valid_entry_count(const struct block_cache *cache) {
    size_t index;
    unsigned int count = 0U;

    for (index = 0U; index < cache->capacity; ++index) {
        if (cache->entries[index].valid) {
            ++count;
        }
    }
    return count;
}

static unsigned int dirty_entry_count(const struct block_cache *cache) {
    size_t index;
    unsigned int count = 0U;

    for (index = 0U; index < cache->capacity; ++index) {
        if (cache->entries[index].valid &&
            cache->entries[index].dirty) {
            ++count;
        }
    }
    return count;
}

static bool ownership_is_unique(const struct block_cache *cache) {
    bool seen[TEST_BLOCK_COUNT];
    size_t index;

    memset(seen, 0, sizeof(seen));
    for (index = 0U; index < cache->capacity; ++index) {
        const struct block_cache_entry *entry = &cache->entries[index];

        if (!entry->valid) {
            continue;
        }
        if (entry->block_index >= TEST_BLOCK_COUNT ||
            seen[entry->block_index]) {
            return false;
        }
        seen[entry->block_index] = true;
    }
    return true;
}

static bool all_bytes(const unsigned char *buffer,
                      size_t length,
                      unsigned char value)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (buffer[index] != value) {
            return false;
        }
    }
    return true;
}

static void fill_block(unsigned char *buffer,
                       size_t length,
                       unsigned char value)
{
    memset(buffer, value, length);
}

static bool entry_holds(const struct block_cache *cache,
                        unsigned int slot,
                        uint64_t block_index,
                        bool valid,
                        bool dirty)
{
    const struct block_cache_entry *entry = &cache->entries[slot];

    return entry->block_index == block_index &&
           entry->valid == valid && entry->dirty == dirty;
}

static void test_validation_and_status_strings(void) {
    struct fixture_device fixture;
    struct block_device device;
    struct block_cache_entry entries[TEST_CACHE_CAPACITY];
    unsigned char storage[TEST_CACHE_STORAGE_SIZE];
    unsigned char buffer[TEST_BLOCK_SIZE];
    struct block_cache cache;
    struct block_cache uninitialized;

    init_fixture(&fixture);
    device = make_device(&fixture);
    memset(entries, 0xA5, sizeof(entries));
    memset(storage, 0xA5, sizeof(storage));
    memset(buffer, 0, sizeof(buffer));
    memset(&uninitialized, 0, sizeof(uninitialized));

    CHECK(block_cache_init(NULL, &device, entries, TEST_CACHE_CAPACITY,
                           storage, TEST_CACHE_STORAGE_SIZE) ==
              BLOCK_CACHE_NULL,
          "cache init rejects null cache");
    CHECK(block_cache_init(&cache, NULL, entries, TEST_CACHE_CAPACITY,
                           storage, TEST_CACHE_STORAGE_SIZE) ==
              BLOCK_CACHE_NULL,
          "cache init rejects null device");

    device.read_fn = NULL;
    CHECK(block_cache_init(&cache, &device, entries, TEST_CACHE_CAPACITY,
                           storage, TEST_CACHE_STORAGE_SIZE) ==
              BLOCK_CACHE_BAD_DEVICE,
          "cache init rejects invalid device");

    device = make_device(&fixture);
    CHECK(block_cache_init(&cache, &device, NULL, TEST_CACHE_CAPACITY,
                           storage, TEST_CACHE_STORAGE_SIZE) ==
              BLOCK_CACHE_BAD_STORAGE,
          "cache init rejects null entry storage");
    CHECK(block_cache_init(&cache, &device, entries, TEST_CACHE_CAPACITY,
                           NULL, TEST_CACHE_STORAGE_SIZE) ==
              BLOCK_CACHE_BAD_STORAGE,
          "cache init rejects null data storage");
    CHECK(block_cache_init(&cache, &device, entries, 0U,
                           storage, TEST_CACHE_STORAGE_SIZE) ==
              BLOCK_CACHE_BAD_CAPACITY,
          "cache init rejects zero capacity");
    CHECK(block_cache_init(&cache, &device, entries,
                           BLOCK_CACHE_CAPACITY_MAX + 1U,
                           storage, TEST_CACHE_STORAGE_SIZE) ==
              BLOCK_CACHE_BAD_CAPACITY,
          "cache init rejects over-limit capacity");
    CHECK(block_cache_init(&cache, &device, entries, TEST_CACHE_CAPACITY,
                           storage, TEST_CACHE_STORAGE_SIZE - 1U) ==
              BLOCK_CACHE_BAD_STORAGE,
          "cache init rejects undersized storage");

    CHECK(init_cache(&cache, &device, entries, storage) == BLOCK_CACHE_OK &&
              cache.capacity == TEST_CACHE_CAPACITY &&
              cache.device == &device &&
              cache.entries == entries &&
              cache.storage == storage &&
              valid_entry_count(&cache) == 0U &&
              dirty_entry_count(&cache) == 0U,
          "valid cache init publishes empty clean entries");

    CHECK(block_cache_read(&uninitialized, 0U, buffer) ==
              BLOCK_CACHE_BAD_DEVICE,
          "uninitialized cache rejects reads");
    CHECK(block_cache_read(NULL, 0U, buffer) == BLOCK_CACHE_NULL,
          "cache read rejects null cache");
    CHECK(block_cache_read(&cache, 0U, NULL) == BLOCK_CACHE_NULL,
          "cache read rejects null buffer");
    CHECK(block_cache_write(&cache, 0U, NULL) == BLOCK_CACHE_NULL,
          "cache write rejects null buffer");
    CHECK(block_cache_read(&cache, TEST_BLOCK_COUNT, buffer) ==
              BLOCK_CACHE_BAD_BLOCK_INDEX &&
              fixture.read_calls == 0U,
          "out-of-range read is rejected without device I/O");
    CHECK(block_cache_write(&cache, TEST_BLOCK_COUNT, buffer) ==
              BLOCK_CACHE_BAD_BLOCK_INDEX &&
              fixture.write_calls == 0U,
          "out-of-range write is rejected without device I/O");

    CHECK(block_cache_status_string(BLOCK_CACHE_OK) != NULL &&
              strcmp(block_cache_status_string(BLOCK_CACHE_OK),
                     "BLOCK_CACHE_OK") == 0,
          "block-cache OK status string is exact");
    CHECK(block_cache_status_string(BLOCK_CACHE_IO_ERROR) != NULL &&
              strcmp(block_cache_status_string(BLOCK_CACHE_IO_ERROR),
                     "BLOCK_CACHE_IO_ERROR") == 0,
          "block-cache I/O error status string is exact");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "validation paths perform no allocation");
}

static void test_read_hit_and_unique_ownership(void) {
    struct fixture_device fixture;
    struct block_device device;
    struct block_cache_entry entries[TEST_CACHE_CAPACITY];
    unsigned char storage[TEST_CACHE_STORAGE_SIZE];
    unsigned char buffer[TEST_BLOCK_SIZE];
    struct block_cache cache;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(init_cache(&cache, &device, entries, storage) == BLOCK_CACHE_OK,
          "read test cache init succeeds");

    CHECK(block_cache_read(&cache, 0U, buffer) == BLOCK_CACHE_OK &&
              fixture.read_calls == 1U &&
              all_bytes(buffer, sizeof(buffer), 0x11U),
          "first read publishes block 0");
    CHECK(block_cache_read(&cache, 0U, buffer) == BLOCK_CACHE_OK &&
              fixture.read_calls == 1U &&
              all_bytes(buffer, sizeof(buffer), 0x11U),
          "cache hit performs no second device read");
    CHECK(block_cache_read(&cache, 1U, buffer) == BLOCK_CACHE_OK &&
              fixture.read_calls == 2U &&
              all_bytes(buffer, sizeof(buffer), 0x12U),
          "second distinct read publishes block 1");
    CHECK(block_cache_read(&cache, 2U, buffer) == BLOCK_CACHE_OK &&
              fixture.read_calls == 3U,
          "third distinct read publishes block 2");
    CHECK(valid_entry_count(&cache) == 3U &&
              ownership_is_unique(&cache) &&
              dirty_entry_count(&cache) == 0U,
          "three distinct blocks own unique clean entries");

    CHECK(block_cache_read(&cache, 7U, buffer) == BLOCK_CACHE_OK &&
              fixture.read_calls == 4U &&
              all_bytes(buffer, sizeof(buffer), 0x18U),
          "full-cache read evicts oldest clean block");
    CHECK(valid_entry_count(&cache) == 3U &&
              ownership_is_unique(&cache),
          "eviction leaves unique ownership without duplicate block");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "read path performs no allocation");
}

static void test_write_dirty_flush_and_flush_all(void) {
    struct fixture_device fixture;
    struct block_device device;
    struct block_cache_entry entries[TEST_CACHE_CAPACITY];
    unsigned char storage[TEST_CACHE_STORAGE_SIZE];
    unsigned char buffer[TEST_BLOCK_SIZE];
    unsigned char data[TEST_BLOCK_SIZE];
    struct block_cache cache;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(init_cache(&cache, &device, entries, storage) == BLOCK_CACHE_OK,
          "write test cache init succeeds");

    fill_block(data, sizeof(data), 0x33U);
    CHECK(block_cache_write(&cache, 0U, data) == BLOCK_CACHE_OK &&
              fixture.write_calls == 0U &&
              entry_holds(&cache, 0U, 0U, true, true),
          "write miss owns dirty block without immediate device write");
    CHECK(block_cache_read(&cache, 0U, buffer) == BLOCK_CACHE_OK &&
              fixture.read_calls == 0U &&
              all_bytes(buffer, sizeof(buffer), 0x33U),
          "dirty cache write is visible without backing read");
    CHECK(block_cache_flush(&cache, 0U) == BLOCK_CACHE_OK &&
              fixture.write_calls == 1U &&
              !cache.entries[0].dirty &&
              all_bytes(fixture.storage, sizeof(data), 0x33U),
          "flush publishes exact dirty block and clears dirty state");
    CHECK(block_cache_flush(&cache, 0U) == BLOCK_CACHE_OK &&
              fixture.write_calls == 1U,
          "clean flush is a no-op");

    fill_block(data, sizeof(data), 0x44U);
    CHECK(block_cache_write(&cache, 1U, data) == BLOCK_CACHE_OK &&
              dirty_entry_count(&cache) == 1U,
          "second dirty block is owned");
    fill_block(data, sizeof(data), 0x55U);
    CHECK(block_cache_write(&cache, 2U, data) == BLOCK_CACHE_OK &&
              dirty_entry_count(&cache) == 2U,
          "third dirty block is owned");
    CHECK(block_cache_flush_all(&cache) == BLOCK_CACHE_OK &&
              fixture.write_calls == 3U &&
              dirty_entry_count(&cache) == 0U &&
              all_bytes(fixture.storage + TEST_BLOCK_SIZE, TEST_BLOCK_SIZE,
                        0x44U) &&
              all_bytes(fixture.storage + 2U * TEST_BLOCK_SIZE,
                        TEST_BLOCK_SIZE, 0x55U),
          "flush-all publishes all dirty blocks exactly once");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "write/flush path performs no allocation");
}

static void test_flush_failure_keeps_dirty_ownership(void) {
    struct fixture_device fixture;
    struct block_device device;
    struct block_cache_entry entries[TEST_CACHE_CAPACITY];
    unsigned char storage[TEST_CACHE_STORAGE_SIZE];
    unsigned char data[TEST_BLOCK_SIZE];
    unsigned char original[TEST_BLOCK_SIZE];
    struct block_cache cache;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(init_cache(&cache, &device, entries, storage) == BLOCK_CACHE_OK,
          "flush-failure test cache init succeeds");
    fill_block(data, sizeof(data), 0x66U);
    memcpy(original, fixture.storage, sizeof(original));

    CHECK(block_cache_write(&cache, 0U, data) == BLOCK_CACHE_OK,
          "flush-failure test writes dirty block");
    fixture.fail_write = true;
    CHECK(block_cache_flush(&cache, 0U) == BLOCK_CACHE_IO_ERROR &&
              fixture.write_calls == 1U &&
              !fixture.fail_write &&
              entry_holds(&cache, 0U, 0U, true, true) &&
              memcmp(fixture.storage, original, sizeof(original)) == 0,
          "injected flush failure keeps dirty ownership and storage");
    CHECK(block_cache_flush(&cache, 0U) == BLOCK_CACHE_OK &&
              fixture.write_calls == 2U &&
              !cache.entries[0].dirty &&
              all_bytes(fixture.storage, sizeof(data), 0x66U),
          "retry flush persists dirty block and clears dirty state");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "flush failure path performs no allocation");
}

static void test_dirty_eviction_flushes_before_reuse(void) {
    struct fixture_device fixture;
    struct block_device device;
    struct block_cache_entry entries[TEST_CACHE_CAPACITY];
    unsigned char storage[TEST_CACHE_STORAGE_SIZE];
    unsigned char buffer[TEST_BLOCK_SIZE];
    unsigned char data[TEST_BLOCK_SIZE];
    struct block_cache cache;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(init_cache(&cache, &device, entries, storage) == BLOCK_CACHE_OK,
          "dirty-eviction test cache init succeeds");
    CHECK(block_cache_read(&cache, 0U, buffer) == BLOCK_CACHE_OK &&
              block_cache_read(&cache, 1U, buffer) == BLOCK_CACHE_OK &&
              block_cache_read(&cache, 2U, buffer) == BLOCK_CACHE_OK &&
              fixture.read_calls == 3U,
          "dirty-eviction test fills cache");
    fill_block(data, sizeof(data), 0x77U);
    CHECK(block_cache_write(&cache, 0U, data) == BLOCK_CACHE_OK &&
              fixture.write_calls == 0U,
          "oldest block becomes dirty before eviction");

    CHECK(block_cache_read(&cache, 3U, buffer) == BLOCK_CACHE_OK &&
              fixture.write_calls == 1U &&
              fixture.read_calls == 4U &&
              all_bytes(fixture.storage, sizeof(data), 0x77U) &&
              entry_holds(&cache, 0U, 3U, true, false),
          "dirty eviction flushes old block before reuse");
    CHECK(block_cache_read(&cache, 0U, buffer) == BLOCK_CACHE_OK &&
              fixture.read_calls == 5U &&
              all_bytes(buffer, sizeof(buffer), 0x77U) &&
              !cache.entries[0].dirty,
          "reloaded block has no stale dirty state");
    CHECK(valid_entry_count(&cache) == 3U &&
              ownership_is_unique(&cache) &&
              dirty_entry_count(&cache) == 0U,
          "post-eviction cache has unique clean ownership");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "dirty eviction performs no allocation");
}

static void test_clean_eviction_skips_write(void) {
    struct fixture_device fixture;
    struct block_device device;
    struct block_cache_entry entries[TEST_CACHE_CAPACITY];
    unsigned char storage[TEST_CACHE_STORAGE_SIZE];
    unsigned char buffer[TEST_BLOCK_SIZE];
    struct block_cache cache;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(init_cache(&cache, &device, entries, storage) == BLOCK_CACHE_OK,
          "clean-eviction test cache init succeeds");
    CHECK(block_cache_read(&cache, 0U, buffer) == BLOCK_CACHE_OK &&
              block_cache_read(&cache, 1U, buffer) == BLOCK_CACHE_OK &&
              block_cache_read(&cache, 2U, buffer) == BLOCK_CACHE_OK,
          "clean-eviction test fills cache");

    CHECK(block_cache_read(&cache, 3U, buffer) == BLOCK_CACHE_OK &&
              fixture.read_calls == 4U &&
              fixture.write_calls == 0U &&
              entry_holds(&cache, 0U, 3U, true, false),
          "clean eviction reuses oldest slot without device write");
    CHECK(valid_entry_count(&cache) == 3U &&
              ownership_is_unique(&cache) &&
              dirty_entry_count(&cache) == 0U,
          "clean eviction leaves unique clean ownership");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "clean eviction performs no allocation");
}

static void test_eviction_failures_never_publish(void) {
    struct fixture_device fixture;
    struct block_device device;
    struct block_cache_entry entries[TEST_CACHE_CAPACITY];
    unsigned char storage[TEST_CACHE_STORAGE_SIZE];
    unsigned char buffer[TEST_BLOCK_SIZE];
    unsigned char data[TEST_BLOCK_SIZE];
    unsigned char original[TEST_BLOCK_SIZE];
    struct block_cache cache;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(init_cache(&cache, &device, entries, storage) == BLOCK_CACHE_OK,
          "eviction-failure test cache init succeeds");
    CHECK(block_cache_read(&cache, 0U, buffer) == BLOCK_CACHE_OK &&
              block_cache_read(&cache, 1U, buffer) == BLOCK_CACHE_OK &&
              block_cache_read(&cache, 2U, buffer) == BLOCK_CACHE_OK,
          "eviction-failure test fills cache");
    fill_block(data, sizeof(data), 0x88U);
    CHECK(block_cache_write(&cache, 0U, data) == BLOCK_CACHE_OK,
          "eviction-failure test dirties oldest block");
    memcpy(original, fixture.storage, sizeof(original));

    fixture.fail_read = true;
    CHECK(block_cache_read(&cache, 3U, buffer) == BLOCK_CACHE_IO_ERROR &&
              fixture.read_calls == 4U &&
              fixture.write_calls == 1U &&
              !cache.entries[0].valid && !cache.entries[0].dirty &&
              valid_entry_count(&cache) == 2U &&
              ownership_is_unique(&cache),
          "failed read after dirty flush never publishes requested block");
    CHECK(block_cache_read(&cache, 3U, buffer) == BLOCK_CACHE_OK &&
              fixture.read_calls == 5U &&
              valid_entry_count(&cache) == 3U &&
              ownership_is_unique(&cache),
          "read succeeds after injected failure");

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(init_cache(&cache, &device, entries, storage) == BLOCK_CACHE_OK &&
              block_cache_read(&cache, 0U, buffer) == BLOCK_CACHE_OK &&
              block_cache_read(&cache, 1U, buffer) == BLOCK_CACHE_OK &&
              block_cache_read(&cache, 2U, buffer) == BLOCK_CACHE_OK,
          "write-eviction test refills cache");
    fill_block(data, sizeof(data), 0x99U);
    CHECK(block_cache_write(&cache, 0U, data) == BLOCK_CACHE_OK,
          "write-eviction test dirties oldest block");
    memcpy(original, fixture.storage, sizeof(original));
    fixture.fail_write = true;
    fill_block(data, sizeof(data), 0xAAU);
    CHECK(block_cache_write(&cache, 3U, data) == BLOCK_CACHE_IO_ERROR &&
              fixture.write_calls == 1U &&
              !fixture.fail_write &&
              entry_holds(&cache, 0U, 0U, true, true) &&
              valid_entry_count(&cache) == 3U &&
              ownership_is_unique(&cache) &&
              memcmp(fixture.storage, original, sizeof(original)) == 0,
          "failed dirty write eviction publishes nothing");
    CHECK(block_cache_flush(&cache, 0U) == BLOCK_CACHE_OK &&
              fixture.write_calls == 2U &&
              !cache.entries[0].dirty,
          "failed write eviction retry flushes old dirty block");
    fill_block(data, sizeof(data), 0xBBU);
    CHECK(block_cache_write(&cache, 3U, data) == BLOCK_CACHE_OK &&
              fixture.write_calls == 2U,
          "write eviction succeeds after old block is clean");
    CHECK(entry_holds(&cache, 0U, 3U, true, true) &&
              all_bytes(cache.storage, sizeof(data), 0xBBU),
          "write eviction publishes new dirty block in reused slot");
    CHECK(valid_entry_count(&cache) == 3U &&
              ownership_is_unique(&cache),
          "post-failure eviction keeps unique ownership");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "eviction failure paths perform no allocation");
}

int main(void) {
    test_validation_and_status_strings();
    test_read_hit_and_unique_ownership();
    test_write_dirty_flush_and_flush_all();
    test_flush_failure_keeps_dirty_ownership();
    test_dirty_eviction_flushes_before_reuse();
    test_clean_eviction_skips_write();
    test_eviction_failures_never_publish();

    if (failures != 0) {
        fprintf(stderr, "Block-cache host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Block-cache host tests: ALL OK");
    return 0;
}

#endif
