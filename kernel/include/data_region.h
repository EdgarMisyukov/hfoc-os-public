#ifndef HFOC_DATA_REGION_H
#define HFOC_DATA_REGION_H

#include <stdint.h>

#include "block_device.h"

#define DATA_REGION_HEADER_BLOCK_SIZE 512U
#define DATA_REGION_HEADER_SIZE 64U
#define DATA_REGION_MAGIC_SIZE 8U
#define DATA_REGION_FORMAT_VERSION 1U
#define DATA_REGION_MAGIC_BYTES \
    { 'H', 'F', 'O', 'C', 'D', 'A', 'T', 'A' }

enum data_region_status {
    DATA_REGION_OK = 0,
    DATA_REGION_NULL = 1,
    DATA_REGION_BAD_DEVICE = 2,
    DATA_REGION_UNSUPPORTED_BLOCK_SIZE = 3,
    DATA_REGION_IO_ERROR = 4,
    DATA_REGION_BAD_MAGIC = 5,
    DATA_REGION_UNSUPPORTED_VERSION = 6,
    DATA_REGION_RESERVED_NONZERO = 7,
    DATA_REGION_INVALID_LAYOUT = 8,
};

struct data_region {
    uint64_t block_offset;
    uint64_t block_count;
};

enum data_region_status data_region_discover(
    struct block_device *device,
    struct data_region *region);
const char *data_region_status_string(enum data_region_status status);

#endif
