#include "boot_policy.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool boot_policy_record_valid(
    const struct boot_policy_record *record)
{
    uint32_t index;

    if (record == NULL) {
        return false;
    }
    for (index = 0U; index < BOOT_POLICY_MODE_COUNT; ++index) {
        if (record->image_ids[index] < BOOT_POLICY_IMAGE_ID_MIN ||
            record->image_ids[index] > BOOT_POLICY_IMAGE_ID_MAX) {
            return false;
        }
    }
    return true;
}

enum boot_policy_status boot_policy_init(
    struct boot_policy *policy,
    const struct boot_policy_record *record)
{
    if (policy == NULL || record == NULL) {
        return BOOT_POLICY_NULL;
    }
    if (policy->published) {
        return BOOT_POLICY_BAD_STATE;
    }
    if (!boot_policy_record_valid(record)) {
        return BOOT_POLICY_INVALID_RECORD;
    }
    policy->record = *record;
    policy->published = true;
    return BOOT_POLICY_OK;
}

enum boot_policy_status boot_policy_select(
    const struct boot_policy *policy,
    enum boot_policy_mode mode,
    uint32_t *image_id_out)
{
    if (policy == NULL || image_id_out == NULL) {
        return BOOT_POLICY_NULL;
    }
    if (!policy->published) {
        return BOOT_POLICY_UNPUBLISHED;
    }
    if (!boot_policy_record_valid(&policy->record)) {
        return BOOT_POLICY_INVALID_RECORD;
    }
    if ((unsigned int)mode >= BOOT_POLICY_MODE_COUNT) {
        return BOOT_POLICY_INVALID_MODE;
    }
    *image_id_out = policy->record.image_ids[mode];
    return BOOT_POLICY_OK;
}

enum boot_policy_status boot_policy_destroy(
    struct boot_policy *policy)
{
    if (policy == NULL) {
        return BOOT_POLICY_NULL;
    }
    if (!policy->published) {
        return BOOT_POLICY_BAD_STATE;
    }
    memset(policy, 0, sizeof(*policy));
    return BOOT_POLICY_OK;
}

const struct boot_policy_record *boot_policy_record(
    const struct boot_policy *policy)
{
    return policy != NULL && policy->published &&
           boot_policy_record_valid(&policy->record)
        ? &policy->record
        : NULL;
}

const char *boot_policy_mode_string(enum boot_policy_mode mode) {
    switch (mode) {
    case BOOT_POLICY_MODE_NORMAL:
        return "BOOT_POLICY_MODE_NORMAL";
    case BOOT_POLICY_MODE_RECOVERY:
        return "BOOT_POLICY_MODE_RECOVERY";
    case BOOT_POLICY_MODE_LAST_KNOWN_GOOD:
        return "BOOT_POLICY_MODE_LAST_KNOWN_GOOD";
    default:
        return NULL;
    }
}

const char *boot_policy_status_string(enum boot_policy_status status) {
    switch (status) {
    case BOOT_POLICY_OK:
        return "BOOT_POLICY_OK";
    case BOOT_POLICY_NULL:
        return "BOOT_POLICY_NULL";
    case BOOT_POLICY_BAD_STATE:
        return "BOOT_POLICY_BAD_STATE";
    case BOOT_POLICY_INVALID_RECORD:
        return "BOOT_POLICY_INVALID_RECORD";
    case BOOT_POLICY_UNPUBLISHED:
        return "BOOT_POLICY_UNPUBLISHED";
    case BOOT_POLICY_INVALID_MODE:
        return "BOOT_POLICY_INVALID_MODE";
    default:
        return "BOOT_POLICY_UNKNOWN";
    }
}
