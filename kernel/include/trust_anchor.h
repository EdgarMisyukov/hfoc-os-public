#ifndef HFOC_TRUST_ANCHOR_H
#define HFOC_TRUST_ANCHOR_H

#include "key_use.h"

#include <stdint.h>

#define TRUST_ANCHOR_DESCRIPTOR_VERSION 2U
#define TRUST_ANCHOR_ID_MIN 1U
#define TRUST_ANCHOR_ID_MAX 255U
#define TRUST_ANCHOR_ALGORITHM_ID_MIN 1U
#define TRUST_ANCHOR_ALGORITHM_ID_MAX 2U
#define TRUST_ANCHOR_SET_MAX 16U

enum trust_anchor_algorithm {
    TRUST_ANCHOR_ALGORITHM_ED25519 = 1,
    TRUST_ANCHOR_ALGORITHM_ECDSA_P256 = 2,
};

enum trust_anchor_state {
    TRUST_ANCHOR_STATE_INACTIVE = 0,
    TRUST_ANCHOR_STATE_ACTIVE = 1,
    TRUST_ANCHOR_STATE_REVOKED = 2,
};

enum trust_anchor_status {
    TRUST_ANCHOR_OK = 0,
    TRUST_ANCHOR_NULL = 1,
    TRUST_ANCHOR_INVALID_ID = 2,
    TRUST_ANCHOR_INVALID_VERSION = 3,
    TRUST_ANCHOR_INVALID_ALGORITHM = 4,
    TRUST_ANCHOR_INVALID_STATE = 5,
    TRUST_ANCHOR_INVALID_PURPOSES = 6,
};

enum trust_anchor_set_status {
    TRUST_ANCHOR_SET_OK = 0,
    TRUST_ANCHOR_SET_NULL = 1,
    TRUST_ANCHOR_SET_BAD_CAPACITY = 2,
    TRUST_ANCHOR_SET_INVALID_DESCRIPTOR = 3,
    TRUST_ANCHOR_SET_DUPLICATE_ANCHOR = 4,
};

struct trust_anchor {
    uint32_t id;
    uint32_t version;
    enum trust_anchor_algorithm algorithm_id;
    enum trust_anchor_state state;
    uint32_t accepted_purposes;
};

enum trust_anchor_status trust_anchor_validate(
    const struct trust_anchor *anchor);
enum trust_anchor_status trust_anchor_init(
    struct trust_anchor *published,
    const struct trust_anchor *anchor);
const char *trust_anchor_status_string(
    enum trust_anchor_status status);
enum trust_anchor_set_status trust_anchor_set_validate(
    const struct trust_anchor *anchors,
    uint32_t anchor_count);
const char *trust_anchor_set_status_string(
    enum trust_anchor_set_status status);

#endif
