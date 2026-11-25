#ifndef HFOC_BLOCK_CACHE_H
#define HFOC_BLOCK_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block_device.h"

#define BLOCK_CACHE_CAPACITY_MIN 1U
#define BLOCK_CACHE_CAPACITY_MAX 256U

enum block_cache_status {
    BLOCK_CACHE_OK = 0,
    BLOCK_CACHE_NULL = 1,
    BLOCK_CACHE_BAD_DEVICE = 2,
    BLOCK_CACHE_BAD_CAPACITY = 3,
    BLOCK_CACHE_BAD_STORAGE = 4,
    BLOCK_CACHE_BAD_BLOCK_INDEX = 5,
    BLOCK_CACHE_IO_ERROR = 6,
};

struct block_cache_entry {
    uint64_t block_index;
    uint64_t sequence;
    bool valid;
    bool dirty;
};

struct block_cache {
    struct block_device *device;
    struct block_cache_entry *entries;
    unsigned char *storage;
    size_t capacity;
    uint64_t next_sequence;
    bool initialized;
};

enum block_cache_status block_cache_init(
    struct block_cache *cache,
    struct block_device *device,
    struct block_cache_entry *entries,
    size_t capacity,
    void *storage,
    size_t storage_size);
enum block_cache_status block_cache_read(
    struct block_cache *cache,
    uint64_t block_index,
    void *buffer);
enum block_cache_status block_cache_write(
    struct block_cache *cache,
    uint64_t block_index,
    const void *buffer);
enum block_cache_status block_cache_flush(
    struct block_cache *cache,
    uint64_t block_index);
enum block_cache_status block_cache_flush_all(
    struct block_cache *cache);
const char *block_cache_status_string(enum block_cache_status status);

#endif
