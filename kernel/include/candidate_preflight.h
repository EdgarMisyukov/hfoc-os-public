#ifndef HFOC_CANDIDATE_PREFLIGHT_H
#define HFOC_CANDIDATE_PREFLIGHT_H

#include "candidate_slot_inventory.h"
#include "signed_metadata.h"
#include "update_compatibility.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CANDIDATE_PREFLIGHT_VERSION 1U
#define CANDIDATE_PREFLIGHT_BUNDLE_MIN_SIZE 1U
#define CANDIDATE_PREFLIGHT_BUNDLE_MAX_SIZE 0x20000000U

enum candidate_preflight_status {
    CANDIDATE_PREFLIGHT_OK = 0,
    CANDIDATE_PREFLIGHT_NULL = 1,
    CANDIDATE_PREFLIGHT_BAD_STATE = 2,
    CANDIDATE_PREFLIGHT_BAD_REQUEST = 3,
    CANDIDATE_PREFLIGHT_BAD_BUNDLE = 4,
    CANDIDATE_PREFLIGHT_BAD_SIZE = 5,
    CANDIDATE_PREFLIGHT_TRUST_DENIED = 6,
    CANDIDATE_PREFLIGHT_BAD_SLOT = 7,
    CANDIDATE_PREFLIGHT_SLOT_UNAVAILABLE = 8,
    CANDIDATE_PREFLIGHT_GENERATION_MISMATCH = 9,
    CANDIDATE_PREFLIGHT_DIGEST_MISMATCH = 10,
    CANDIDATE_PREFLIGHT_COMPATIBILITY_DENIED = 11,
};

struct candidate_preflight_bundle {
    size_t size;
    uint8_t digest[CANDIDATE_SLOT_INVENTORY_DIGEST_SIZE];
};

struct candidate_preflight_request {
    uint32_t version;
    uint32_t slot_id;
    uint32_t generation;
    enum signed_metadata_status trust_status;
    struct candidate_preflight_bundle bundle;
    struct update_compatibility_manifest compatibility;
    struct update_compatibility_source source;
    struct update_compatibility_target target;
};

struct candidate_preflight_gate {
    uint32_t version;
    struct candidate_preflight_request candidate;
    bool preflighted;
};

enum candidate_preflight_status candidate_preflight_init(
    struct candidate_preflight_gate *gate);
enum candidate_preflight_status candidate_preflight_run(
    struct candidate_preflight_gate *gate,
    const struct candidate_preflight_request *request,
    const struct candidate_slot_inventory *inventory);
const char *candidate_preflight_status_string(
    enum candidate_preflight_status status);

#endif
