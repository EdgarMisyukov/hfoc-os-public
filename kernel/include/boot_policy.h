#ifndef HFOC_BOOT_POLICY_H
#define HFOC_BOOT_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define BOOT_POLICY_MODE_COUNT 3U
#define BOOT_POLICY_IMAGE_ID_MIN 1U
#define BOOT_POLICY_IMAGE_ID_MAX 4096U

enum boot_policy_mode {
    BOOT_POLICY_MODE_NORMAL = 0,
    BOOT_POLICY_MODE_RECOVERY = 1,
    BOOT_POLICY_MODE_LAST_KNOWN_GOOD = 2,
};

struct boot_policy_record {
    uint32_t image_ids[BOOT_POLICY_MODE_COUNT];
};

struct boot_policy {
    struct boot_policy_record record;
    bool published;
};

enum boot_policy_status {
    BOOT_POLICY_OK = 0,
    BOOT_POLICY_NULL = 1,
    BOOT_POLICY_BAD_STATE = 2,
    BOOT_POLICY_INVALID_RECORD = 3,
    BOOT_POLICY_UNPUBLISHED = 4,
    BOOT_POLICY_INVALID_MODE = 5,
};

enum boot_policy_status boot_policy_init(
    struct boot_policy *policy,
    const struct boot_policy_record *record);
enum boot_policy_status boot_policy_select(
    const struct boot_policy *policy,
    enum boot_policy_mode mode,
    uint32_t *image_id_out);
enum boot_policy_status boot_policy_destroy(
    struct boot_policy *policy);
const struct boot_policy_record *boot_policy_record(
    const struct boot_policy *policy);
const char *boot_policy_mode_string(enum boot_policy_mode mode);
const char *boot_policy_status_string(enum boot_policy_status status);

#endif
