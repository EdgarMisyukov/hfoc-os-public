#ifndef HFOC_ACTIVATION_INTENT_H
#define HFOC_ACTIVATION_INTENT_H

#include "candidate_staged_write.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ACTIVATION_INTENT_VERSION 1U
#define ACTIVATION_INTENT_DIGEST_SIZE CANDIDATE_SLOT_INVENTORY_DIGEST_SIZE
#define ACTIVATION_INTENT_BUNDLE_MIN_SIZE CANDIDATE_PREFLIGHT_BUNDLE_MIN_SIZE
#define ACTIVATION_INTENT_BUNDLE_MAX_SIZE CANDIDATE_PREFLIGHT_BUNDLE_MAX_SIZE

enum activation_intent_status {
    ACTIVATION_INTENT_OK = 0,
    ACTIVATION_INTENT_NULL = 1,
    ACTIVATION_INTENT_BAD_STATE = 2,
    ACTIVATION_INTENT_BAD_REQUEST = 3,
    ACTIVATION_INTENT_BAD_CANDIDATE = 4,
    ACTIVATION_INTENT_BAD_ROLLBACK = 5,
    ACTIVATION_INTENT_SAME_SLOT = 6,
    ACTIVATION_INTENT_WRITE_FAULT = 7,
    ACTIVATION_INTENT_NO_PENDING = 8,
};

struct activation_intent_entry {
    uint32_t slot_id;
    uint32_t generation;
    uint8_t digest[ACTIVATION_INTENT_DIGEST_SIZE];
};

struct activation_intent_record {
    uint32_t version;
    struct activation_intent_entry target;
    struct activation_intent_entry rollback;
    size_t bundle_size;
    bool pending;
};

typedef bool (*activation_intent_write_fn)(
    void *context,
    uint8_t phase,
    const struct activation_intent_record *record);

struct activation_intent_io {
    void *context;
    activation_intent_write_fn write;
};

struct activation_intent_store {
    uint32_t version;
    struct activation_intent_entry known_good;
    struct activation_intent_record pending;
    bool initialized;
};

enum activation_intent_status activation_intent_init(
    struct activation_intent_store *store,
    const struct activation_intent_entry *known_good);
enum activation_intent_status activation_intent_record(
    struct activation_intent_store *store,
    const struct candidate_staged_write_gate *candidate,
    const struct activation_intent_io *io);
enum activation_intent_status activation_intent_pending(
    const struct activation_intent_store *store,
    struct activation_intent_record *record_out);
enum activation_intent_status activation_intent_known_good(
    const struct activation_intent_store *store,
    struct activation_intent_entry *entry_out);
enum activation_intent_status activation_intent_commit_known_good(
    struct activation_intent_store *store);
enum activation_intent_status activation_intent_resolve(
    struct activation_intent_store *store,
    struct activation_intent_entry *entry_out);
const char *activation_intent_status_string(
    enum activation_intent_status status);

#endif
