#ifndef HFOC_PACKET_SLICE_H
#define HFOC_PACKET_SLICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "packet_pool.h"

#define PACKET_SLICE_VERSION 1U

enum packet_slice_status {
    PACKET_SLICE_OK = 0,
    PACKET_SLICE_NULL = 1,
    PACKET_SLICE_BAD_STATE = 2,
    PACKET_SLICE_BAD_POOL = 3,
    PACKET_SLICE_STALE_DESCRIPTOR = 4,
    PACKET_SLICE_WRONG_OWNER = 5,
    PACKET_SLICE_BAD_LENGTH = 6,
    PACKET_SLICE_BAD_RANGE = 7,
    PACKET_SLICE_BAD_CURSOR = 8,
    PACKET_SLICE_UNDERFLOW = 9,
    PACKET_SLICE_OVERFLOW = 10,
    PACKET_SLICE_OVERLAP = 11,
};

struct packet_slice {
    uint32_t version;
    const struct packet_pool_descriptor *pool;
    struct packet_pool_slot_descriptor slot;
    uint32_t start;
    uint32_t length;
    uint32_t cursor;
};

enum packet_slice_status packet_slice_init(
    struct packet_slice *slice,
    const struct packet_pool_descriptor *pool,
    const struct packet_pool_slot_descriptor *slot,
    uint32_t length);
bool packet_slice_is_valid(const struct packet_slice *slice);
enum packet_slice_status packet_slice_prepend(
    struct packet_slice *slice,
    const void *data,
    size_t size);
enum packet_slice_status packet_slice_append(
    struct packet_slice *slice,
    const void *data,
    size_t size);
enum packet_slice_status packet_slice_trim(
    struct packet_slice *slice,
    uint32_t amount);
enum packet_slice_status packet_slice_read(
    struct packet_slice *slice,
    void *destination,
    size_t size);
enum packet_slice_status packet_slice_write(
    struct packet_slice *slice,
    const void *data,
    size_t size);
enum packet_slice_status packet_slice_seek(
    struct packet_slice *slice,
    uint32_t offset);
uint32_t packet_slice_start(const struct packet_slice *slice);
uint32_t packet_slice_length(const struct packet_slice *slice);
uint32_t packet_slice_cursor(const struct packet_slice *slice);
const char *packet_slice_status_string(
    enum packet_slice_status status);

#endif
