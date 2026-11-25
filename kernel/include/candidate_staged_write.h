#ifndef HFOC_CANDIDATE_STAGED_WRITE_H
#define HFOC_CANDIDATE_STAGED_WRITE_H

#include "candidate_preflight.h"
#include "candidate_slot_inventory.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CANDIDATE_STAGED_WRITE_VERSION 1U
#define CANDIDATE_STAGED_WRITE_READBACK_CHUNK_SIZE 4096U

enum candidate_staged_write_status {
    CANDIDATE_STAGED_WRITE_OK = 0,
    CANDIDATE_STAGED_WRITE_NULL = 1,
    CANDIDATE_STAGED_WRITE_BAD_STATE = 2,
    CANDIDATE_STAGED_WRITE_BAD_REQUEST = 3,
    CANDIDATE_STAGED_WRITE_BAD_BUNDLE = 4,
    CANDIDATE_STAGED_WRITE_PREFLIGHT_DENIED = 5,
    CANDIDATE_STAGED_WRITE_BAD_SLOT = 6,
    CANDIDATE_STAGED_WRITE_SLOT_UNAVAILABLE = 7,
    CANDIDATE_STAGED_WRITE_GENERATION_MISMATCH = 8,
    CANDIDATE_STAGED_WRITE_DIGEST_MISMATCH = 9,
    CANDIDATE_STAGED_WRITE_WRITE_FAULT = 10,
    CANDIDATE_STAGED_WRITE_READBACK_FAULT = 11,
};

typedef bool (*candidate_staged_write_fn)(
    void *context,
    uint32_t slot_id,
    const void *data,
    size_t size);
typedef bool (*candidate_staged_readback_fn)(
    void *context,
    uint32_t slot_id,
    size_t offset,
    void *data,
    size_t capacity,
    size_t *bytes_read);

struct candidate_staged_write_io {
    void *context;
    candidate_staged_write_fn write;
    candidate_staged_readback_fn readback;
};

struct candidate_staged_write_gate {
    uint32_t version;
    uint32_t slot_id;
    uint32_t generation;
    uint8_t digest[SHA256_DIGEST_SIZE];
    size_t bundle_size;
    bool ready;
};

enum candidate_staged_write_status candidate_staged_write_init(
    struct candidate_staged_write_gate *gate);
enum candidate_staged_write_status candidate_staged_write_run(
    struct candidate_staged_write_gate *gate,
    const struct candidate_preflight_gate *preflight,
    const struct candidate_slot_inventory *inventory,
    const void *data,
    const struct candidate_staged_write_io *io);
const char *candidate_staged_write_status_string(
    enum candidate_staged_write_status status);

#endif
