#include "image_integrity.h"

#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static size_t image_integrity_bounded_strlen(const char *text,
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

static bool image_integrity_mem_equal(const char *left,
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

static bool image_integrity_name_char_valid(unsigned char value) {
    return (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
           (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
           (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
           value == (unsigned char)'_' || value == (unsigned char)'.' ||
           value == (unsigned char)'-';
}

static bool image_integrity_version_char_valid(unsigned char value) {
    return (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
           (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
           (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
           value == (unsigned char)'_' || value == (unsigned char)'.' ||
           value == (unsigned char)'+' || value == (unsigned char)'-';
}

static bool image_integrity_hex_char_valid(unsigned char value) {
    return (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
           (value >= (unsigned char)'a' && value <= (unsigned char)'f') ||
           (value >= (unsigned char)'A' && value <= (unsigned char)'F');
}

static bool image_integrity_component_valid(
    const struct system_image_component *component)
{
    const size_t name_length =
        image_integrity_bounded_strlen(component->name,
                                       SYSTEM_IMAGE_COMPONENT_NAME_MAX);
    const size_t version_length =
        image_integrity_bounded_strlen(component->version,
                                       SYSTEM_IMAGE_COMPONENT_VERSION_MAX);
    size_t index;

    if (name_length == 0U || name_length > SYSTEM_IMAGE_COMPONENT_NAME_MAX ||
        version_length == 0U ||
        version_length > SYSTEM_IMAGE_COMPONENT_VERSION_MAX) {
        return false;
    }
    for (index = 0U; index < name_length; ++index) {
        if (!image_integrity_name_char_valid(
                (unsigned char)component->name[index])) {
            return false;
        }
    }
    for (index = 0U; index < version_length; ++index) {
        if (!image_integrity_version_char_valid(
                (unsigned char)component->version[index])) {
            return false;
        }
    }
    if (component->hash[SYSTEM_IMAGE_HASH_HEX_LEN] != '\0') {
        return false;
    }
    for (index = 0U; index < SYSTEM_IMAGE_HASH_HEX_LEN; ++index) {
        if (!image_integrity_hex_char_valid(
                (unsigned char)component->hash[index])) {
            return false;
        }
    }
    return true;
}

static bool image_integrity_manifest_valid(
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
    arch_length = image_integrity_bounded_strlen(manifest->arch,
                                                 SYSTEM_IMAGE_ARCH_MAX);
    if (arch_length != 6U ||
        !image_integrity_mem_equal(manifest->arch, "x86_64", 6U)) {
        return false;
    }
    for (left = 0U; left < manifest->component_count; ++left) {
        if (!image_integrity_component_valid(
                &manifest->components[left])) {
            return false;
        }
        for (right = left + 1U; right < manifest->component_count; ++right) {
            const size_t left_name_length =
                image_integrity_bounded_strlen(
                    manifest->components[left].name,
                    SYSTEM_IMAGE_COMPONENT_NAME_MAX);
            const size_t right_name_length =
                image_integrity_bounded_strlen(
                    manifest->components[right].name,
                    SYSTEM_IMAGE_COMPONENT_NAME_MAX);

            if (left_name_length == right_name_length &&
                image_integrity_mem_equal(
                    manifest->components[left].name,
                    manifest->components[right].name,
                    left_name_length)) {
                return false;
            }
        }
    }
    return true;
}

static bool image_integrity_payload_name_valid(const char *name) {
    const size_t name_length =
        image_integrity_bounded_strlen(name, IMAGE_INTEGRITY_NAME_MAX);
    size_t index;

    if (name_length == 0U || name_length > IMAGE_INTEGRITY_NAME_MAX) {
        return false;
    }
    for (index = 0U; index < name_length; ++index) {
        if (!image_integrity_name_char_valid((unsigned char)name[index])) {
            return false;
        }
    }
    return true;
}

static int image_integrity_find_component(
    const struct system_image_manifest *manifest,
    const char *name)
{
    const size_t name_length =
        image_integrity_bounded_strlen(name, IMAGE_INTEGRITY_NAME_MAX);
    uint32_t index;

    for (index = 0U; index < manifest->component_count; ++index) {
        const size_t component_length =
            image_integrity_bounded_strlen(
                manifest->components[index].name,
                SYSTEM_IMAGE_COMPONENT_NAME_MAX);

        if (name_length == component_length &&
            image_integrity_mem_equal(name,
                                      manifest->components[index].name,
                                      name_length)) {
            return (int)index;
        }
    }
    return -1;
}

static void image_integrity_store_hex_byte(uint8_t *output,
                                           uint8_t value)
{
    static const char digits[] = "0123456789abcdef";

    output[0] = (uint8_t)digits[value >> 4U];
    output[1] = (uint8_t)digits[value & 0x0FU];
}

static void image_integrity_zero_bytes(uint8_t *destination,
                                       size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = 0U;
    }
}

static bool image_integrity_hash_equal(const char *expected,
                                       const uint8_t digest[SHA256_DIGEST_SIZE])
{
    char measured[2U * SHA256_DIGEST_SIZE + 1U];
    uint32_t index;

    for (index = 0U; index < SHA256_DIGEST_SIZE; ++index) {
        image_integrity_store_hex_byte(
            (uint8_t *)&measured[2U * index], digest[index]);
    }
    measured[2U * SHA256_DIGEST_SIZE] = '\0';
    for (index = 0U; index < 2U * SHA256_DIGEST_SIZE; ++index) {
        const unsigned char expected_value =
            (unsigned char)expected[index];
        const unsigned char measured_value =
            (unsigned char)measured[index];

        if (expected_value >= (unsigned char)'A' &&
            expected_value <= (unsigned char)'F') {
            if (expected_value + 32U != measured_value) {
                return false;
            }
        } else if (expected_value != measured_value) {
            return false;
        }
    }
    return true;
}

enum image_integrity_status image_integrity_verify(
    struct image_integrity_gate *gate,
    const struct system_image_manifest *manifest,
    const struct image_integrity_payload *payloads,
    size_t payload_count)
{
    uint8_t digests[IMAGE_INTEGRITY_PAYLOAD_MAX][SHA256_DIGEST_SIZE];
    bool seen[SYSTEM_IMAGE_COMPONENT_MAX] = {false};
    struct image_integrity_gate draft;
    size_t payload_index;
    uint32_t component_index;

    if (gate == NULL || manifest == NULL ||
        (payloads == NULL && payload_count != 0U)) {
        return IMAGE_INTEGRITY_NULL;
    }
    if (gate->published) {
        return IMAGE_INTEGRITY_BAD_STATE;
    }
    if (payload_count > IMAGE_INTEGRITY_PAYLOAD_MAX) {
        return IMAGE_INTEGRITY_TOO_MANY_PAYLOADS;
    }
    if (!image_integrity_manifest_valid(manifest)) {
        return IMAGE_INTEGRITY_BAD_MANIFEST;
    }

    for (payload_index = 0U; payload_index < payload_count; ++payload_index) {
        const struct image_integrity_payload *const payload =
            &payloads[payload_index];
        size_t previous_index;
        int component;

        for (previous_index = 0U; previous_index < payload_index;
             ++previous_index) {
            const size_t previous_length =
                image_integrity_bounded_strlen(
                    payloads[previous_index].name,
                    IMAGE_INTEGRITY_NAME_MAX);
            const size_t current_length =
                image_integrity_bounded_strlen(
                    payload->name, IMAGE_INTEGRITY_NAME_MAX);

            if (previous_length == current_length &&
                image_integrity_mem_equal(
                    payloads[previous_index].name,
                    payload->name, current_length)) {
                return IMAGE_INTEGRITY_DUPLICATE_PAYLOAD;
            }
        }
        if (payload->data_length > IMAGE_INTEGRITY_DATA_MAX) {
            return IMAGE_INTEGRITY_PAYLOAD_TOO_LONG;
        }
        if (!image_integrity_payload_name_valid(payload->name) ||
            payload->data == NULL) {
            return IMAGE_INTEGRITY_BAD_PAYLOAD;
        }
        component = image_integrity_find_component(manifest, payload->name);
        if (component < 0) {
            return IMAGE_INTEGRITY_UNKNOWN_COMPONENT;
        }
        if (sha256_compute(payload->data, payload->data_length,
                           digests[payload_index]) != SHA256_OK) {
            return IMAGE_INTEGRITY_BAD_PAYLOAD;
        }
        if (!image_integrity_hash_equal(
                manifest->components[(uint32_t)component].hash,
                digests[payload_index])) {
            return IMAGE_INTEGRITY_HASH_MISMATCH;
        }
        seen[(uint32_t)component] = true;
    }

    for (component_index = 0U;
         component_index < manifest->component_count; ++component_index) {
        if (!seen[component_index]) {
            return IMAGE_INTEGRITY_MISSING_COMPONENT;
        }
    }

    image_integrity_zero_bytes((uint8_t *)&draft, sizeof(draft));
    draft.manifest = *manifest;
    draft.published = true;
    *gate = draft;
    return IMAGE_INTEGRITY_OK;
}

bool image_integrity_is_published(
    const struct image_integrity_gate *gate)
{
    return gate != NULL && gate->published;
}

const char *image_integrity_status_string(
    enum image_integrity_status status)
{
    switch (status) {
    case IMAGE_INTEGRITY_OK:
        return "IMAGE_INTEGRITY_OK";
    case IMAGE_INTEGRITY_NULL:
        return "IMAGE_INTEGRITY_NULL";
    case IMAGE_INTEGRITY_BAD_STATE:
        return "IMAGE_INTEGRITY_BAD_STATE";
    case IMAGE_INTEGRITY_TOO_MANY_PAYLOADS:
        return "IMAGE_INTEGRITY_TOO_MANY_PAYLOADS";
    case IMAGE_INTEGRITY_BAD_MANIFEST:
        return "IMAGE_INTEGRITY_BAD_MANIFEST";
    case IMAGE_INTEGRITY_DUPLICATE_PAYLOAD:
        return "IMAGE_INTEGRITY_DUPLICATE_PAYLOAD";
    case IMAGE_INTEGRITY_MISSING_COMPONENT:
        return "IMAGE_INTEGRITY_MISSING_COMPONENT";
    case IMAGE_INTEGRITY_UNKNOWN_COMPONENT:
        return "IMAGE_INTEGRITY_UNKNOWN_COMPONENT";
    case IMAGE_INTEGRITY_PAYLOAD_TOO_LONG:
        return "IMAGE_INTEGRITY_PAYLOAD_TOO_LONG";
    case IMAGE_INTEGRITY_HASH_MISMATCH:
        return "IMAGE_INTEGRITY_HASH_MISMATCH";
    case IMAGE_INTEGRITY_BAD_PAYLOAD:
        return "IMAGE_INTEGRITY_BAD_PAYLOAD";
    default:
        return "IMAGE_INTEGRITY_UNKNOWN";
    }
}
