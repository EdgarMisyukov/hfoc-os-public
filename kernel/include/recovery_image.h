#ifndef HFOC_RECOVERY_IMAGE_H
#define HFOC_RECOVERY_IMAGE_H

#include "boot_policy.h"
#include "image_integrity.h"
#include "system_image.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum recovery_image_status {
    RECOVERY_IMAGE_OK = 0,
    RECOVERY_IMAGE_NULL = 1,
    RECOVERY_IMAGE_BAD_STATE = 2,
    RECOVERY_IMAGE_POLICY_UNAVAILABLE = 3,
    RECOVERY_IMAGE_NO_IMAGE = 4,
};

enum recovery_image_selected_mode {
    RECOVERY_IMAGE_SELECTED_NONE = 0,
    RECOVERY_IMAGE_SELECTED_NORMAL = 1,
    RECOVERY_IMAGE_SELECTED_RECOVERY = 2,
};

struct recovery_image_candidate {
    const struct system_image_manifest *manifest;
    const struct image_integrity_payload *payloads;
    size_t payload_count;
};

struct recovery_image_selection {
    struct image_integrity_gate gate;
    uint32_t selected_image_id;
    enum recovery_image_selected_mode selected_mode;
    bool published;
};

enum recovery_image_status recovery_image_select(
    struct recovery_image_selection *selection,
    const struct boot_policy *policy,
    const struct recovery_image_candidate *normal,
    const struct recovery_image_candidate *recovery);
bool recovery_image_is_published(
    const struct recovery_image_selection *selection);
const struct system_image_manifest *recovery_image_manifest(
    const struct recovery_image_selection *selection);
const char *recovery_image_status_string(
    enum recovery_image_status status);

#endif
