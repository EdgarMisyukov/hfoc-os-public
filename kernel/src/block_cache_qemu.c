#ifdef HFOC_BLOCK_CACHE_QEMU_TEST

#include "block_cache.h"
#include "block_device.h"
#include "kstring.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLOCK_CACHE_QEMU_BLOCK_SIZE 512U
#define BLOCK_CACHE_QEMU_BLOCK_COUNT 8U
#define BLOCK_CACHE_QEMU_CAPACITY \
    (BLOCK_CACHE_QEMU_BLOCK_SIZE * BLOCK_CACHE_QEMU_BLOCK_COUNT)
#define BLOCK_CACHE_QEMU_CACHE_CAPACITY 3U
#define BLOCK_CACHE_QEMU_CACHE_STORAGE_SIZE \
    (BLOCK_CACHE_QEMU_CACHE_CAPACITY * BLOCK_CACHE_QEMU_BLOCK_SIZE)

static unsigned char block_cache_qemu_storage[BLOCK_CACHE_QEMU_CAPACITY];
static bool block_cache_qemu_fail_read;
static bool block_cache_qemu_fail_write;
static size_t block_cache_qemu_read_calls;
static size_t block_cache_qemu_write_calls;

static void block_cache_qemu_failure(const char *message)
    __attribute__((noreturn));

static void block_cache_qemu_failure(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[BLOCK-CACHE] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void block_cache_qemu_require(bool condition,
                                     const char *message)
{
    if (!condition) {
        block_cache_qemu_failure(message);
    }
}

static enum block_device_status block_cache_qemu_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    (void)device;
    ++block_cache_qemu_read_calls;
    if (block_cache_qemu_fail_read) {
        block_cache_qemu_fail_read = false;
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(buffer, block_cache_qemu_storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status block_cache_qemu_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    (void)device;
    ++block_cache_qemu_write_calls;
    if (block_cache_qemu_fail_write) {
        block_cache_qemu_fail_write = false;
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(block_cache_qemu_storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static void block_cache_qemu_init_storage(void)
{
    uint64_t block;
    size_t offset;

    for (block = 0U; block < BLOCK_CACHE_QEMU_BLOCK_COUNT; ++block) {
        for (offset = 0U; offset < BLOCK_CACHE_QEMU_BLOCK_SIZE; ++offset) {
            block_cache_qemu_storage[
                (size_t)block * BLOCK_CACHE_QEMU_BLOCK_SIZE + offset] =
                (unsigned char)(0x11U + (unsigned int)block);
        }
    }
}

static bool block_cache_qemu_all_bytes(const unsigned char *buffer,
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

static void block_cache_qemu_fill(unsigned char *buffer,
                                  size_t length,
                                  unsigned char value)
{
    memset(buffer, value, length);
}

static unsigned int block_cache_qemu_valid_entries(
    const struct block_cache *cache)
{
    size_t index;
    unsigned int count = 0U;

    for (index = 0U; index < cache->capacity; ++index) {
        if (cache->entries[index].valid) {
            ++count;
        }
    }
    return count;
}

static bool block_cache_qemu_unique_ownership(
    const struct block_cache *cache)
{
    bool seen[BLOCK_CACHE_QEMU_BLOCK_COUNT];
    size_t index;

    memset(seen, 0, sizeof(seen));
    for (index = 0U; index < cache->capacity; ++index) {
        const struct block_cache_entry *entry = &cache->entries[index];

        if (!entry->valid) {
            continue;
        }
        if (entry->block_index >= BLOCK_CACHE_QEMU_BLOCK_COUNT ||
            seen[entry->block_index]) {
            return false;
        }
        seen[entry->block_index] = true;
    }
    return true;
}

void block_cache_qemu_test_init(void)
{
    struct block_device device;
    struct block_cache_entry entries[BLOCK_CACHE_QEMU_CACHE_CAPACITY];
    unsigned char cache_storage[BLOCK_CACHE_QEMU_CACHE_STORAGE_SIZE];
    unsigned char buffer[BLOCK_CACHE_QEMU_BLOCK_SIZE];
    unsigned char data[BLOCK_CACHE_QEMU_BLOCK_SIZE];
    struct block_cache cache;
    enum block_cache_status status;

    block_cache_qemu_init_storage();
    memset(&device, 0, sizeof(device));
    device.block_size = BLOCK_CACHE_QEMU_BLOCK_SIZE;
    device.block_count = BLOCK_CACHE_QEMU_BLOCK_COUNT;
    device.read_fn = block_cache_qemu_read;
    device.write_fn = block_cache_qemu_write;

    serial_write("[BLOCK-CACHE] proof init\n");

    status = block_cache_init(&cache, &device, entries,
                              BLOCK_CACHE_QEMU_CACHE_CAPACITY,
                              cache_storage,
                              BLOCK_CACHE_QEMU_CACHE_STORAGE_SIZE);
    block_cache_qemu_require(
        status == BLOCK_CACHE_OK &&
            block_cache_qemu_valid_entries(&cache) == 0U,
        "cache initialization");
    serial_write("[BLOCK-CACHE] cache initialized\n");

    status = block_cache_read(&cache, 0U, buffer);
    block_cache_qemu_require(
        status == BLOCK_CACHE_OK &&
            block_cache_qemu_read_calls == 1U &&
            block_cache_qemu_all_bytes(buffer, sizeof(buffer), 0x11U),
        "first cache read");
    status = block_cache_read(&cache, 0U, buffer);
    block_cache_qemu_require(
        status == BLOCK_CACHE_OK &&
            block_cache_qemu_read_calls == 1U &&
            block_cache_qemu_all_bytes(buffer, sizeof(buffer), 0x11U),
        "cache hit");
    serial_write("[BLOCK-CACHE] cache hit preserved\n");

    block_cache_qemu_fill(data, sizeof(data), 0x33U);
    status = block_cache_write(&cache, 0U, data);
    block_cache_qemu_require(
        status == BLOCK_CACHE_OK &&
            block_cache_qemu_write_calls == 0U &&
            cache.entries[0].valid && cache.entries[0].dirty,
        "dirty write ownership");
    status = block_cache_read(&cache, 0U, buffer);
    block_cache_qemu_require(
        status == BLOCK_CACHE_OK &&
            block_cache_qemu_read_calls == 1U &&
            block_cache_qemu_all_bytes(buffer, sizeof(buffer), 0x33U),
        "dirty write visibility");
    serial_write("[BLOCK-CACHE] dirty block cached\n");

    status = block_cache_flush(&cache, 0U);
    block_cache_qemu_require(
        status == BLOCK_CACHE_OK &&
            block_cache_qemu_write_calls == 1U &&
            !cache.entries[0].dirty &&
            block_cache_qemu_all_bytes(block_cache_qemu_storage,
                                       sizeof(data), 0x33U),
        "dirty flush persistence");
    serial_write("[BLOCK-CACHE] dirty flush persisted\n");

    block_cache_qemu_fill(data, sizeof(data), 0x44U);
    status = block_cache_write(&cache, 1U, data);
    block_cache_qemu_require(
        status == BLOCK_CACHE_OK && cache.entries[1].dirty,
        "second dirty block");
    block_cache_qemu_fail_write = true;
    status = block_cache_flush(&cache, 1U);
    block_cache_qemu_require(
        status == BLOCK_CACHE_IO_ERROR &&
            block_cache_qemu_write_calls == 2U &&
            !block_cache_qemu_fail_write &&
            cache.entries[1].valid && cache.entries[1].dirty,
        "flush failure keeps dirty");
    status = block_cache_flush(&cache, 1U);
    block_cache_qemu_require(
        status == BLOCK_CACHE_OK &&
            block_cache_qemu_write_calls == 3U &&
            !cache.entries[1].dirty,
        "flush retry");
    serial_write("[BLOCK-CACHE] flush failure kept dirty\n");

    status = block_cache_read(&cache, 2U, buffer);
    block_cache_qemu_require(
        status == BLOCK_CACHE_OK &&
            block_cache_qemu_read_calls == 2U,
        "third cache block");
    block_cache_qemu_fill(data, sizeof(data), 0x77U);
    status = block_cache_write(&cache, 0U, data);
    block_cache_qemu_require(
        status == BLOCK_CACHE_OK && cache.entries[0].dirty,
        "oldest dirty eviction candidate");
    status = block_cache_read(&cache, 3U, buffer);
    block_cache_qemu_require(
        status == BLOCK_CACHE_OK &&
            block_cache_qemu_write_calls == 4U &&
            block_cache_qemu_read_calls == 3U &&
            block_cache_qemu_all_bytes(block_cache_qemu_storage,
                                       sizeof(data), 0x77U) &&
            cache.entries[0].valid &&
            cache.entries[0].block_index == 3U &&
            !cache.entries[0].dirty,
        "dirty eviction flush");
    serial_write("[BLOCK-CACHE] dirty eviction flushed\n");

    status = block_cache_read(&cache, 4U, buffer);
    block_cache_qemu_require(
        status == BLOCK_CACHE_OK &&
            block_cache_qemu_read_calls == 4U &&
            block_cache_qemu_write_calls == 4U &&
            cache.entries[1].valid &&
            cache.entries[1].block_index == 4U &&
            !cache.entries[1].dirty,
        "clean eviction no write");
    serial_write("[BLOCK-CACHE] clean eviction skipped write\n");

    block_cache_qemu_fill(data, sizeof(data), 0x88U);
    status = block_cache_write(&cache, 2U, data);
    block_cache_qemu_require(
        status == BLOCK_CACHE_OK && cache.entries[2].dirty,
        "oldest block for failed-read eviction");
    block_cache_qemu_fail_read = true;
    status = block_cache_read(&cache, 5U, buffer);
    block_cache_qemu_require(
        status == BLOCK_CACHE_IO_ERROR &&
            block_cache_qemu_read_calls == 5U &&
            block_cache_qemu_write_calls == 5U &&
            !cache.entries[2].valid && !cache.entries[2].dirty &&
            block_cache_qemu_valid_entries(&cache) == 2U &&
            block_cache_qemu_unique_ownership(&cache),
        "failed read leaves no duplicate");
    status = block_cache_read(&cache, 5U, buffer);
    block_cache_qemu_require(
        status == BLOCK_CACHE_OK &&
            block_cache_qemu_read_calls == 6U &&
            block_cache_qemu_valid_entries(&cache) == 3U &&
            block_cache_qemu_unique_ownership(&cache),
        "read retry after failed eviction");
    serial_write("[BLOCK-CACHE] read failure left no duplicate\n");

    serial_write("[BLOCK-CACHE] clean shutdown\n");
    serial_write("[BLOCK-CACHE] proof OK\n");
}

#else
/* Keep this test-only translation unit non-empty in normal kernel builds. */
int hfoc_block_cache_qemu_translation_unit_marker;
#endif
