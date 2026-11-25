#ifndef HFOC_BLOCK_DEVICE_H
#define HFOC_BLOCK_DEVICE_H

#include <stddef.h>
#include <stdint.h>

#define BLOCK_DEVICE_BLOCK_SIZE_MIN 512U
#define BLOCK_DEVICE_BLOCK_SIZE_MAX 65536U
#define BLOCK_DEVICE_BLOCK_COUNT_MAX UINT64_C(0xFFFFFFFF)

enum block_device_status {
    BLOCK_DEVICE_OK = 0,
    BLOCK_DEVICE_NULL = 1,
    BLOCK_DEVICE_BAD_DEVICE = 2,
    BLOCK_DEVICE_BAD_BLOCK_SIZE = 3,
    BLOCK_DEVICE_BAD_BLOCK_COUNT = 4,
    BLOCK_DEVICE_UNALIGNED = 5,
    BLOCK_DEVICE_INVALID_LENGTH = 6,
    BLOCK_DEVICE_OUT_OF_RANGE = 7,
    BLOCK_DEVICE_IO_ERROR = 8,
    BLOCK_DEVICE_UNSUPPORTED = 9,
};

struct block_device;

typedef enum block_device_status (*block_device_read_fn)(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length);
typedef enum block_device_status (*block_device_write_fn)(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length);
typedef enum block_device_status (*block_device_flush_fn)(
    struct block_device *device);

struct block_device {
    uint32_t block_size;
    uint64_t block_count;
    void *context;
    block_device_read_fn read_fn;
    block_device_write_fn write_fn;
    block_device_flush_fn flush_fn;
};

enum block_device_status block_device_validate(
    const struct block_device *device);
enum block_device_status block_device_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length);
enum block_device_status block_device_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length);
enum block_device_status block_device_flush(
    struct block_device *device);
const char *block_device_status_string(enum block_device_status status);

#endif
