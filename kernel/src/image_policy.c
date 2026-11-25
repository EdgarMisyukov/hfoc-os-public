#include "image_policy.h"

#include "image_integrity.h"
#include "system_image.h"
#include "verification_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static size_t image_policy_bounded_strlen(const char *text,
                                          size_t maximum)
{
    size_t length = 0U;

    if (text == NULL) {
        return maximum + 1U;
    }
    while (length < maximum && text[length] != '\0') {
        ++length;
    }
    if (text[length] != '\0') {
        return maximum + 1U;
    }
    return length;
}

static bool image_policy_mem_equal(const char *left,
                                   const char *right,
                                   size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        if ((unsigned char)left[index] != (unsigned char)right[index]) {
            return false;
        }
    }
    return true;
}

static bool image_policy_name_char_valid(unsigned char value)
{
    return (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
           (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
           (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
           value == (unsigned char)'_' || value == (unsigned char)'.' ||
           value == (unsigned char)'-';
}

static bool image_policy_version_char_valid(unsigned char value)
{
    return (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
           (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
           (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
           value == (unsigned char)'_' || value == (unsigned char)'.' ||
           value == (unsigned char)'+' || value == (unsigned char)'-';
}

static bool image_policy_hex_char_valid(unsigned char value)
{
    return (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
           (value >= (unsigned char)'a' && value <= (unsigned char)'f') ||
           (value >= (unsigned char)'A' && value <= (unsigned char)'F');
}

static bool image_policy_component_valid(
    const struct system_image_component *component)
{
    const size_t name_length =
        image_policy_bounded_strlen(component->name,
                                    SYSTEM_IMAGE_COMPONENT_NAME_MAX);
    const size_t version_length =
        image_policy_bounded_strlen(component->version,
                                    SYSTEM_IMAGE_COMPONENT_VERSION_MAX);
    size_t index;

    if (name_length == 0U ||
        name_length > SYSTEM_IMAGE_COMPONENT_NAME_MAX ||
        version_length == 0U ||
        version_length > SYSTEM_IMAGE_COMPONENT_VERSION_MAX) {
        return false;
    }
    for (index = 0U; index < name_length; ++index) {
        if (!image_policy_name_char_valid(
                (unsigned char)component->name[index])) {
            return false;
        }
    }
    for (index = 0U; index < version_length; ++index) {
        if (!image_policy_version_char_valid(
                (unsigned char)component->version[index])) {
            return false;
        }
    }
    if (component->hash[SYSTEM_IMAGE_HASH_HEX_LEN] != '\0') {
        return false;
    }
    for (index = 0U; index < SYSTEM_IMAGE_HASH_HEX_LEN; ++index) {
        if (!image_policy_hex_char_valid(
                (unsigned char)component->hash[index])) {
            return false;
        }
    }
    return true;
}

static bool image_policy_manifest_valid(
    const struct system_image_manifest *manifest)
{
    uint32_t left;
    uint32_t right;
    size_t arch_length;

    if (manifest == NULL ||
        manifest->manifest_version != SYSTEM_IMAGE_MANIFEST_VERSION ||
        manifest->image_id < SYSTEM_IMAGE_IMAGE_ID_MIN ||
        manifest->image_id > SYSTEM_IMAGE_IMAGE_ID_MAX ||
        manifest->min_kernel_abi < SYSTEM_IMAGE_KERNEL_ABI_MIN ||
        manifest->min_kernel_abi > SYSTEM_IMAGE_KERNEL_ABI_MAX ||
        manifest->max_kernel_abi < SYSTEM_IMAGE_KERNEL_ABI_MIN ||
        manifest->max_kernel_abi > SYSTEM_IMAGE_KERNEL_ABI_MAX ||
        manifest->min_kernel_abi > manifest->max_kernel_abi ||
        manifest->component_count == 0U ||
        manifest->component_count > SYSTEM_IMAGE_COMPONENT_MAX) {
        return false;
    }

    arch_length = image_policy_bounded_strlen(
        manifest->arch, SYSTEM_IMAGE_ARCH_MAX);
    if (arch_length != 6U ||
        !image_policy_mem_equal(manifest->arch, "x86_64", 6U)) {
        return false;
    }

    for (left = 0U; left < manifest->component_count; ++left) {
        if (!image_policy_component_valid(
                &manifest->components[left])) {
            return false;
        }
        for (right = left + 1U;
             right < manifest->component_count; ++right) {
            const size_t left_name_length =
                image_policy_bounded_strlen(
                    manifest->components[left].name,
                    SYSTEM_IMAGE_COMPONENT_NAME_MAX);
            const size_t right_name_length =
                image_policy_bounded_strlen(
                    manifest->components[right].name,
                    SYSTEM_IMAGE_COMPONENT_NAME_MAX);

            if (left_name_length == right_name_length &&
                image_policy_mem_equal(
                    manifest->components[left].name,
                    manifest->components[right].name,
                    left_name_length)) {
                return false;
            }
        }
    }
    return true;
}

static bool image_policy_binding_valid(
    const struct image_policy_binding *binding)
{
    return binding != NULL &&
           binding->version == IMAGE_POLICY_VERSION &&
           binding->published &&
           image_policy_manifest_valid(&binding->manifest);
}

enum image_policy_status image_policy_bind(
    struct image_policy_binding *binding,
    const struct verification_policy *policy,
    const struct image_integrity_gate *gate)
{
    const struct verification_policy_snapshot *active;
    struct image_policy_binding draft;

    if (binding == NULL || policy == NULL || gate == NULL) {
        return IMAGE_POLICY_NULL;
    }
    if (binding->published) {
        return IMAGE_POLICY_BAD_STATE;
    }
    active = verification_policy_active(policy);
    if (active == NULL) {
        return IMAGE_POLICY_NO_POLICY;
    }
    if (!image_integrity_is_published(gate)) {
        return IMAGE_POLICY_NO_IMAGE;
    }
    if (!image_policy_manifest_valid(&gate->manifest)) {
        return IMAGE_POLICY_INVALID_IMAGE;
    }

    memset(&draft, 0, sizeof(draft));
    draft.version = IMAGE_POLICY_VERSION;
    draft.policy_generation = active->generation;
    draft.manifest = gate->manifest;
    draft.published = true;
    *binding = draft;
    return IMAGE_POLICY_OK;
}

enum image_policy_status image_policy_activate(
    const struct image_policy_binding *binding,
    const struct verification_policy *policy,
    struct system_image_manifest *manifest_out)
{
    const struct verification_policy_snapshot *active;

    if (binding == NULL || policy == NULL || manifest_out == NULL) {
        return IMAGE_POLICY_NULL;
    }
    active = verification_policy_active(policy);
    if (active == NULL) {
        return IMAGE_POLICY_NO_POLICY;
    }
    if (!image_policy_binding_valid(binding)) {
        return IMAGE_POLICY_NOT_PUBLISHED;
    }
    if (binding->policy_generation != active->generation) {
        return IMAGE_POLICY_POLICY_MISMATCH;
    }

    *manifest_out = binding->manifest;
    return IMAGE_POLICY_OK;
}

enum image_policy_status image_policy_reset(
    struct image_policy_binding *binding)
{
    if (binding == NULL) {
        return IMAGE_POLICY_NULL;
    }
    if (!binding->published) {
        return IMAGE_POLICY_BAD_STATE;
    }

    memset(binding, 0, sizeof(*binding));
    return IMAGE_POLICY_OK;
}

bool image_policy_is_published(
    const struct image_policy_binding *binding)
{
    return image_policy_binding_valid(binding);
}

const struct system_image_manifest *image_policy_manifest(
    const struct image_policy_binding *binding)
{
    return image_policy_binding_valid(binding)
        ? &binding->manifest
        : NULL;
}

const char *image_policy_status_string(
    enum image_policy_status status)
{
    switch (status) {
    case IMAGE_POLICY_OK:
        return "IMAGE_POLICY_OK";
    case IMAGE_POLICY_NULL:
        return "IMAGE_POLICY_NULL";
    case IMAGE_POLICY_BAD_STATE:
        return "IMAGE_POLICY_BAD_STATE";
    case IMAGE_POLICY_NO_POLICY:
        return "IMAGE_POLICY_NO_POLICY";
    case IMAGE_POLICY_NO_IMAGE:
        return "IMAGE_POLICY_NO_IMAGE";
    case IMAGE_POLICY_INVALID_IMAGE:
        return "IMAGE_POLICY_INVALID_IMAGE";
    case IMAGE_POLICY_NOT_PUBLISHED:
        return "IMAGE_POLICY_NOT_PUBLISHED";
    case IMAGE_POLICY_POLICY_MISMATCH:
        return "IMAGE_POLICY_POLICY_MISMATCH";
    default:
        return NULL;
    }
}
