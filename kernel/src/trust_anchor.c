#include "trust_anchor.h"

#include <stddef.h>
#include <stdint.h>

enum trust_anchor_status trust_anchor_validate(
    const struct trust_anchor *anchor)
{
    if (anchor == NULL) {
        return TRUST_ANCHOR_NULL;
    }
    if (anchor->id < TRUST_ANCHOR_ID_MIN ||
        anchor->id > TRUST_ANCHOR_ID_MAX) {
        return TRUST_ANCHOR_INVALID_ID;
    }
    if (anchor->version != TRUST_ANCHOR_DESCRIPTOR_VERSION) {
        return TRUST_ANCHOR_INVALID_VERSION;
    }
    if (anchor->algorithm_id != TRUST_ANCHOR_ALGORITHM_ED25519 &&
        anchor->algorithm_id != TRUST_ANCHOR_ALGORITHM_ECDSA_P256) {
        return TRUST_ANCHOR_INVALID_ALGORITHM;
    }
    if (anchor->state != TRUST_ANCHOR_STATE_INACTIVE &&
        anchor->state != TRUST_ANCHOR_STATE_ACTIVE &&
        anchor->state != TRUST_ANCHOR_STATE_REVOKED) {
        return TRUST_ANCHOR_INVALID_STATE;
    }
    if (!key_use_accepted_purposes_valid(anchor->accepted_purposes)) {
        return TRUST_ANCHOR_INVALID_PURPOSES;
    }
    return TRUST_ANCHOR_OK;
}

enum trust_anchor_status trust_anchor_init(
    struct trust_anchor *published,
    const struct trust_anchor *anchor)
{
    enum trust_anchor_status status;

    if (published == NULL || anchor == NULL) {
        return TRUST_ANCHOR_NULL;
    }
    status = trust_anchor_validate(anchor);
    if (status != TRUST_ANCHOR_OK) {
        return status;
    }
    if (published != anchor) {
        *published = *anchor;
    }
    return TRUST_ANCHOR_OK;
}

const char *trust_anchor_status_string(
    enum trust_anchor_status status)
{
    switch (status) {
    case TRUST_ANCHOR_OK:
        return "TRUST_ANCHOR_OK";
    case TRUST_ANCHOR_NULL:
        return "TRUST_ANCHOR_NULL";
    case TRUST_ANCHOR_INVALID_ID:
        return "TRUST_ANCHOR_INVALID_ID";
    case TRUST_ANCHOR_INVALID_VERSION:
        return "TRUST_ANCHOR_INVALID_VERSION";
    case TRUST_ANCHOR_INVALID_ALGORITHM:
        return "TRUST_ANCHOR_INVALID_ALGORITHM";
    case TRUST_ANCHOR_INVALID_STATE:
        return "TRUST_ANCHOR_INVALID_STATE";
    case TRUST_ANCHOR_INVALID_PURPOSES:
        return "TRUST_ANCHOR_INVALID_PURPOSES";
    default:
        return NULL;
    }
}

enum trust_anchor_set_status trust_anchor_set_validate(
    const struct trust_anchor *anchors,
    uint32_t anchor_count)
{
    uint32_t index;
    uint32_t previous;

    if (anchor_count > TRUST_ANCHOR_SET_MAX) {
        return TRUST_ANCHOR_SET_BAD_CAPACITY;
    }
    if (anchor_count > 0U && anchors == NULL) {
        return TRUST_ANCHOR_SET_NULL;
    }
    if (anchor_count == 0U) {
        return TRUST_ANCHOR_SET_OK;
    }
    for (index = 0U; index < anchor_count; ++index) {
        if (trust_anchor_validate(&anchors[index]) != TRUST_ANCHOR_OK) {
            return TRUST_ANCHOR_SET_INVALID_DESCRIPTOR;
        }
    }
    for (index = 0U; index < anchor_count; ++index) {
        for (previous = 0U; previous < index; ++previous) {
            if (anchors[index].id == anchors[previous].id) {
                return TRUST_ANCHOR_SET_DUPLICATE_ANCHOR;
            }
        }
    }
    return TRUST_ANCHOR_SET_OK;
}

const char *trust_anchor_set_status_string(
    enum trust_anchor_set_status status)
{
    switch (status) {
    case TRUST_ANCHOR_SET_OK:
        return "TRUST_ANCHOR_SET_OK";
    case TRUST_ANCHOR_SET_NULL:
        return "TRUST_ANCHOR_SET_NULL";
    case TRUST_ANCHOR_SET_BAD_CAPACITY:
        return "TRUST_ANCHOR_SET_BAD_CAPACITY";
    case TRUST_ANCHOR_SET_INVALID_DESCRIPTOR:
        return "TRUST_ANCHOR_SET_INVALID_DESCRIPTOR";
    case TRUST_ANCHOR_SET_DUPLICATE_ANCHOR:
        return "TRUST_ANCHOR_SET_DUPLICATE_ANCHOR";
    default:
        return NULL;
    }
}
