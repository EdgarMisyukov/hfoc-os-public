#include "recovery_image.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum recovery_image_status recovery_image_select(
    struct recovery_image_selection *selection,
    const struct boot_policy *policy,
    const struct recovery_image_candidate *normal,
    const struct recovery_image_candidate *recovery)
{
    struct image_integrity_gate normal_gate;
    struct image_integrity_gate recovery_gate;
    uint32_t normal_image_id = 0U;
    uint32_t recovery_image_id = 0U;
    enum boot_policy_status policy_status;

    if (selection == NULL) {
        return RECOVERY_IMAGE_NULL;
    }
    if (selection->published) {
        return RECOVERY_IMAGE_BAD_STATE;
    }
    if (policy == NULL || normal == NULL || recovery == NULL ||
        normal->manifest == NULL ||
        recovery->manifest == NULL ||
        (normal->payload_count != 0U && normal->payloads == NULL) ||
        (recovery->payload_count != 0U && recovery->payloads == NULL)) {
        return RECOVERY_IMAGE_NULL;
    }

    policy_status = boot_policy_select(
        policy, BOOT_POLICY_MODE_NORMAL, &normal_image_id);
    if (policy_status != BOOT_POLICY_OK) {
        return RECOVERY_IMAGE_POLICY_UNAVAILABLE;
    }
    policy_status = boot_policy_select(
        policy, BOOT_POLICY_MODE_RECOVERY, &recovery_image_id);
    if (policy_status != BOOT_POLICY_OK) {
        return RECOVERY_IMAGE_POLICY_UNAVAILABLE;
    }

    if (normal->manifest->image_id == normal_image_id) {
        memset(&normal_gate, 0, sizeof(normal_gate));
        if (image_integrity_verify(
                &normal_gate, normal->manifest,
                normal->payloads, normal->payload_count) ==
            IMAGE_INTEGRITY_OK) {
            memset(selection, 0, sizeof(*selection));
            selection->gate = normal_gate;
            selection->selected_image_id = normal_image_id;
            selection->selected_mode = RECOVERY_IMAGE_SELECTED_NORMAL;
            selection->published = true;
            return RECOVERY_IMAGE_OK;
        }
    }

    if (recovery->manifest->image_id == recovery_image_id) {
        memset(&recovery_gate, 0, sizeof(recovery_gate));
        if (image_integrity_verify(
                &recovery_gate, recovery->manifest,
                recovery->payloads, recovery->payload_count) ==
            IMAGE_INTEGRITY_OK) {
            memset(selection, 0, sizeof(*selection));
            selection->gate = recovery_gate;
            selection->selected_image_id = recovery_image_id;
            selection->selected_mode = RECOVERY_IMAGE_SELECTED_RECOVERY;
            selection->published = true;
            return RECOVERY_IMAGE_OK;
        }
    }

    return RECOVERY_IMAGE_NO_IMAGE;
}

bool recovery_image_is_published(
    const struct recovery_image_selection *selection)
{
    return selection != NULL && selection->published;
}

const struct system_image_manifest *recovery_image_manifest(
    const struct recovery_image_selection *selection)
{
    if (selection == NULL || !selection->published) {
        return NULL;
    }
    return &selection->gate.manifest;
}

const char *recovery_image_status_string(
    enum recovery_image_status status)
{
    switch (status) {
    case RECOVERY_IMAGE_OK:
        return "RECOVERY_IMAGE_OK";
    case RECOVERY_IMAGE_NULL:
        return "RECOVERY_IMAGE_NULL";
    case RECOVERY_IMAGE_BAD_STATE:
        return "RECOVERY_IMAGE_BAD_STATE";
    case RECOVERY_IMAGE_POLICY_UNAVAILABLE:
        return "RECOVERY_IMAGE_POLICY_UNAVAILABLE";
    case RECOVERY_IMAGE_NO_IMAGE:
        return "RECOVERY_IMAGE_NO_IMAGE";
    default:
        return "RECOVERY_IMAGE_UNKNOWN";
    }
}
