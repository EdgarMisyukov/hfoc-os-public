#include "appliance_update_manifest.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_UPDATE_MANIFEST_IDENTITY_FIELD_COUNT 6U
#define APPLIANCE_UPDATE_MANIFEST_COMPATIBILITY_FIELD_COUNT 19U

enum appliance_update_manifest_identity_field {
    APPLIANCE_UPDATE_MANIFEST_IDENTITY_UPDATE_PACKAGE_VERSION = 0,
    APPLIANCE_UPDATE_MANIFEST_IDENTITY_RELEASE_ID = 1,
    APPLIANCE_UPDATE_MANIFEST_IDENTITY_ARCHITECTURE = 2,
    APPLIANCE_UPDATE_MANIFEST_IDENTITY_SOURCE_REVISION = 3,
    APPLIANCE_UPDATE_MANIFEST_IDENTITY_METADATA_DIGEST = 4,
    APPLIANCE_UPDATE_MANIFEST_IDENTITY_PAYLOAD_DIGEST = 5,
};

static size_t appliance_update_manifest_strlen(const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static bool appliance_update_manifest_mem_equal(const char *left,
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

static void appliance_update_manifest_zero_bytes(char *destination,
                                                 size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = '\0';
    }
}

static void appliance_update_manifest_copy_bytes(char *destination,
                                                 const char *source,
                                                 size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = source[index];
    }
}

static bool appliance_update_manifest_key_char_valid(unsigned char value)
{
    return (value >= (unsigned char)'a' &&
            value <= (unsigned char)'z') ||
           (value >= (unsigned char)'0' &&
            value <= (unsigned char)'9') ||
           value == (unsigned char)'_';
}

static bool appliance_update_manifest_digit_valid(unsigned char value)
{
    return value >= (unsigned char)'0' &&
           value <= (unsigned char)'9';
}

static bool appliance_update_manifest_lower_hex_valid(unsigned char value)
{
    return appliance_update_manifest_digit_valid(value) ||
           (value >= (unsigned char)'a' &&
            value <= (unsigned char)'f');
}

static bool appliance_update_manifest_parse_u32(const char *value,
                                                size_t value_length,
                                                uint32_t *value_out)
{
    uint64_t parsed = 0U;
    size_t index;

    for (index = 0U; index < value_length; ++index) {
        const uint32_t digit =
            (uint32_t)((unsigned char)value[index] - (unsigned char)'0');

        if (parsed > ((uint64_t)UINT32_MAX - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    *value_out = (uint32_t)parsed;
    return true;
}

static bool appliance_update_manifest_identity_key(
    const char *key,
    size_t key_length,
    enum appliance_update_manifest_identity_field *field_out)
{
    static const char *const names[
        APPLIANCE_UPDATE_MANIFEST_IDENTITY_FIELD_COUNT] = {
        "update_package_version",
        "release_id",
        "architecture",
        "source_revision",
        "metadata_digest",
        "payload_digest",
    };
    uint32_t index;

    for (index = 0U;
         index < APPLIANCE_UPDATE_MANIFEST_IDENTITY_FIELD_COUNT;
         ++index) {
        const size_t name_length = appliance_update_manifest_strlen(
            names[index]);

        if (name_length == key_length &&
            appliance_update_manifest_mem_equal(
                names[index], key, key_length)) {
            *field_out = (enum appliance_update_manifest_identity_field)index;
            return true;
        }
    }
    return false;
}

static bool appliance_update_manifest_compatibility_key(
    const char *key,
    size_t key_length)
{
    static const char *const names[
        APPLIANCE_UPDATE_MANIFEST_COMPATIBILITY_FIELD_COUNT] = {
        "compatibility_version",
        "source_image_id",
        "target_image_id",
        "source_schema_min",
        "source_schema_max",
        "target_schema_min",
        "target_schema_max",
        "source_kernel_abi_min",
        "source_kernel_abi_max",
        "target_kernel_abi_min",
        "target_kernel_abi_max",
        "source_service_abi_min",
        "source_service_abi_max",
        "target_service_abi_min",
        "target_service_abi_max",
        "source_policy_generation_min",
        "source_policy_generation_max",
        "target_policy_generation_min",
        "target_policy_generation_max",
    };
    uint32_t index;

    for (index = 0U;
         index < APPLIANCE_UPDATE_MANIFEST_COMPATIBILITY_FIELD_COUNT;
         ++index) {
        const size_t name_length = appliance_update_manifest_strlen(
            names[index]);

        if (name_length == key_length &&
            appliance_update_manifest_mem_equal(
                names[index], key, key_length)) {
            return true;
        }
    }
    return false;
}

static bool appliance_update_manifest_lower_hex_text(
    const char *value,
    size_t value_length,
    size_t expected_length)
{
    size_t index;

    if (value_length != expected_length) {
        return false;
    }
    for (index = 0U; index < value_length; ++index) {
        if (!appliance_update_manifest_lower_hex_valid(
                (unsigned char)value[index])) {
            return false;
        }
    }
    return true;
}

static enum appliance_update_manifest_status
appliance_update_manifest_map_compatibility(
    enum update_compatibility_status status)
{
    switch (status) {
    case UPDATE_COMPATIBILITY_OK:
        return APPLIANCE_UPDATE_MANIFEST_OK;
    case UPDATE_COMPATIBILITY_NULL:
        return APPLIANCE_UPDATE_MANIFEST_NULL;
    case UPDATE_COMPATIBILITY_BAD_STATE:
        return APPLIANCE_UPDATE_MANIFEST_BAD_REQUEST;
    case UPDATE_COMPATIBILITY_TOO_LONG:
        return APPLIANCE_UPDATE_MANIFEST_TOO_LONG;
    case UPDATE_COMPATIBILITY_BAD_VERSION:
        return APPLIANCE_UPDATE_MANIFEST_BAD_VERSION;
    case UPDATE_COMPATIBILITY_MISSING_FIELD:
        return APPLIANCE_UPDATE_MANIFEST_MISSING_FIELD;
    case UPDATE_COMPATIBILITY_DUPLICATE_FIELD:
        return APPLIANCE_UPDATE_MANIFEST_DUPLICATE_FIELD;
    case UPDATE_COMPATIBILITY_UNKNOWN_FIELD:
        return APPLIANCE_UPDATE_MANIFEST_UNKNOWN_FIELD;
    case UPDATE_COMPATIBILITY_MALFORMED_FIELD:
        return APPLIANCE_UPDATE_MANIFEST_MALFORMED_FIELD;
    case UPDATE_COMPATIBILITY_OUT_OF_RANGE:
        return APPLIANCE_UPDATE_MANIFEST_OUT_OF_RANGE;
    case UPDATE_COMPATIBILITY_INCOMPATIBLE_RANGE:
        return APPLIANCE_UPDATE_MANIFEST_INCOMPATIBLE_RANGE;
    case UPDATE_COMPATIBILITY_INVALID_SOURCE:
        return APPLIANCE_UPDATE_MANIFEST_INVALID_SOURCE;
    case UPDATE_COMPATIBILITY_INVALID_TARGET:
        return APPLIANCE_UPDATE_MANIFEST_INVALID_TARGET;
    case UPDATE_COMPATIBILITY_SOURCE_IMAGE_MISMATCH:
    case UPDATE_COMPATIBILITY_SOURCE_SCHEMA_MISMATCH:
    case UPDATE_COMPATIBILITY_SOURCE_ABI_MISMATCH:
    case UPDATE_COMPATIBILITY_SOURCE_SERVICE_MISMATCH:
    case UPDATE_COMPATIBILITY_SOURCE_POLICY_MISMATCH:
    case UPDATE_COMPATIBILITY_TARGET_IMAGE_MISMATCH:
    case UPDATE_COMPATIBILITY_TARGET_SCHEMA_MISMATCH:
    case UPDATE_COMPATIBILITY_TARGET_ABI_MISMATCH:
    case UPDATE_COMPATIBILITY_TARGET_SERVICE_MISMATCH:
    case UPDATE_COMPATIBILITY_TARGET_POLICY_MISMATCH:
        return APPLIANCE_UPDATE_MANIFEST_COMPATIBILITY_DENIED;
    case UPDATE_COMPATIBILITY_SAME_IMAGE:
        return APPLIANCE_UPDATE_MANIFEST_SAME_IMAGE;
    default:
        return APPLIANCE_UPDATE_MANIFEST_BAD_REQUEST;
    }
}

static enum appliance_update_manifest_status
appliance_update_manifest_parse_identity(
    enum appliance_update_manifest_identity_field field,
    const char *value,
    size_t value_length,
    struct appliance_update_manifest *draft,
    bool *seen)
{
    uint32_t parsed_value;

    if (value_length == 0U ||
        value_length > APPLIANCE_UPDATE_MANIFEST_LINE_MAX) {
        return APPLIANCE_UPDATE_MANIFEST_MALFORMED_FIELD;
    }
    if (seen[field]) {
        return APPLIANCE_UPDATE_MANIFEST_DUPLICATE_FIELD;
    }

    switch (field) {
    case APPLIANCE_UPDATE_MANIFEST_IDENTITY_UPDATE_PACKAGE_VERSION:
        if (!appliance_update_manifest_parse_u32(
                value, value_length, &parsed_value)) {
            size_t index;

            for (index = 0U; index < value_length; ++index) {
                if (!appliance_update_manifest_digit_valid(
                        (unsigned char)value[index])) {
                    return APPLIANCE_UPDATE_MANIFEST_MALFORMED_FIELD;
                }
            }
            return APPLIANCE_UPDATE_MANIFEST_OUT_OF_RANGE;
        }
        if (parsed_value != APPLIANCE_UPDATE_MANIFEST_VERSION) {
            return APPLIANCE_UPDATE_MANIFEST_BAD_VERSION;
        }
        draft->update_package_version = parsed_value;
        break;
    case APPLIANCE_UPDATE_MANIFEST_IDENTITY_RELEASE_ID:
        if (value_length !=
                appliance_update_manifest_strlen(RELEASE_PROFILE_ID) ||
            !appliance_update_manifest_mem_equal(
                value, RELEASE_PROFILE_ID, value_length)) {
            return APPLIANCE_UPDATE_MANIFEST_BAD_RELEASE_ID;
        }
        appliance_update_manifest_copy_bytes(
            draft->release_id, value, value_length);
        draft->release_id[value_length] = '\0';
        break;
    case APPLIANCE_UPDATE_MANIFEST_IDENTITY_ARCHITECTURE:
        if (value_length !=
                appliance_update_manifest_strlen(RELEASE_PROFILE_ARCH) ||
            !appliance_update_manifest_mem_equal(
                value, RELEASE_PROFILE_ARCH, value_length)) {
            return APPLIANCE_UPDATE_MANIFEST_UNSUPPORTED_ARCH;
        }
        appliance_update_manifest_copy_bytes(
            draft->architecture, value, value_length);
        draft->architecture[value_length] = '\0';
        break;
    case APPLIANCE_UPDATE_MANIFEST_IDENTITY_SOURCE_REVISION:
        if (!appliance_update_manifest_lower_hex_text(
                value, value_length,
                APPLIANCE_UPDATE_MANIFEST_REVISION_LENGTH)) {
            return APPLIANCE_UPDATE_MANIFEST_BAD_REVISION;
        }
        appliance_update_manifest_copy_bytes(
            draft->source_revision, value, value_length);
        draft->source_revision[value_length] = '\0';
        break;
    case APPLIANCE_UPDATE_MANIFEST_IDENTITY_METADATA_DIGEST:
    case APPLIANCE_UPDATE_MANIFEST_IDENTITY_PAYLOAD_DIGEST:
        if (!appliance_update_manifest_lower_hex_text(
                value, value_length,
                APPLIANCE_UPDATE_MANIFEST_DIGEST_LENGTH)) {
            return APPLIANCE_UPDATE_MANIFEST_BAD_DIGEST;
        }
        if (field == APPLIANCE_UPDATE_MANIFEST_IDENTITY_METADATA_DIGEST) {
            appliance_update_manifest_copy_bytes(
                draft->metadata_digest, value, value_length);
            draft->metadata_digest[value_length] = '\0';
        } else {
            appliance_update_manifest_copy_bytes(
                draft->payload_digest, value, value_length);
            draft->payload_digest[value_length] = '\0';
        }
        break;
    default:
        return APPLIANCE_UPDATE_MANIFEST_BAD_REQUEST;
    }

    seen[field] = true;
    return APPLIANCE_UPDATE_MANIFEST_OK;
}

static enum appliance_update_manifest_status
appliance_update_manifest_parse_line(
    const char *line,
    size_t line_length,
    struct appliance_update_manifest *draft,
    bool *identity_seen,
    char *compatibility_input,
    size_t *compatibility_length)
{
    size_t equals_index = 0U;
    bool equals_found = false;
    enum appliance_update_manifest_identity_field identity_field;
    size_t key_length;
    size_t value_length;
    size_t index;

    if (line_length == 0U ||
        line_length > APPLIANCE_UPDATE_MANIFEST_LINE_MAX) {
        return APPLIANCE_UPDATE_MANIFEST_MALFORMED_FIELD;
    }
    for (index = 0U; index < line_length; ++index) {
        if (line[index] == '=') {
            if (equals_found) {
                return APPLIANCE_UPDATE_MANIFEST_MALFORMED_FIELD;
            }
            equals_found = true;
            equals_index = index;
        }
    }
    if (!equals_found) {
        return APPLIANCE_UPDATE_MANIFEST_MALFORMED_FIELD;
    }

    key_length = equals_index;
    value_length = line_length - equals_index - 1U;
    if (key_length == 0U ||
        key_length > APPLIANCE_UPDATE_MANIFEST_KEY_MAX ||
        value_length == 0U) {
        return APPLIANCE_UPDATE_MANIFEST_MALFORMED_FIELD;
    }
    for (index = 0U; index < key_length; ++index) {
        if (!appliance_update_manifest_key_char_valid(
                (unsigned char)line[index])) {
            return APPLIANCE_UPDATE_MANIFEST_MALFORMED_FIELD;
        }
    }

    if (appliance_update_manifest_identity_key(
            line, key_length, &identity_field)) {
        return appliance_update_manifest_parse_identity(
            identity_field,
            line + equals_index + 1U,
            value_length,
            draft,
            identity_seen);
    }
    if (!appliance_update_manifest_compatibility_key(
            line, key_length)) {
        return APPLIANCE_UPDATE_MANIFEST_UNKNOWN_FIELD;
    }
    if (*compatibility_length + line_length + 1U >
        APPLIANCE_UPDATE_MANIFEST_COMPATIBILITY_INPUT_MAX) {
        return APPLIANCE_UPDATE_MANIFEST_TOO_LONG;
    }
    appliance_update_manifest_copy_bytes(
        compatibility_input + *compatibility_length,
        line,
        line_length);
    *compatibility_length += line_length;
    compatibility_input[*compatibility_length] = '\n';
    ++*compatibility_length;
    return APPLIANCE_UPDATE_MANIFEST_OK;
}

static enum appliance_update_manifest_status
appliance_update_manifest_identity_valid(
    const struct appliance_update_manifest *manifest)
{
    size_t release_length;
    size_t arch_length;
    size_t index;

    if (manifest == NULL) {
        return APPLIANCE_UPDATE_MANIFEST_NULL;
    }
    if (manifest->update_package_version !=
        APPLIANCE_UPDATE_MANIFEST_VERSION) {
        return APPLIANCE_UPDATE_MANIFEST_BAD_VERSION;
    }
    release_length = appliance_update_manifest_strlen(
        RELEASE_PROFILE_ID);
    arch_length = appliance_update_manifest_strlen(RELEASE_PROFILE_ARCH);
    if (appliance_update_manifest_strlen(manifest->release_id) !=
            release_length ||
        !appliance_update_manifest_mem_equal(
            manifest->release_id, RELEASE_PROFILE_ID, release_length)) {
        return APPLIANCE_UPDATE_MANIFEST_BAD_RELEASE_ID;
    }
    if (appliance_update_manifest_strlen(manifest->architecture) !=
            arch_length ||
        !appliance_update_manifest_mem_equal(
            manifest->architecture, RELEASE_PROFILE_ARCH, arch_length)) {
        return APPLIANCE_UPDATE_MANIFEST_UNSUPPORTED_ARCH;
    }
    if (!appliance_update_manifest_lower_hex_text(
            manifest->source_revision,
            appliance_update_manifest_strlen(manifest->source_revision),
            APPLIANCE_UPDATE_MANIFEST_REVISION_LENGTH)) {
        return APPLIANCE_UPDATE_MANIFEST_BAD_REVISION;
    }
    for (index = 0U; index < 2U; ++index) {
        const char *digest = index == 0U
            ? manifest->metadata_digest
            : manifest->payload_digest;

        if (!appliance_update_manifest_lower_hex_text(
                digest,
                appliance_update_manifest_strlen(digest),
                APPLIANCE_UPDATE_MANIFEST_DIGEST_LENGTH)) {
            return APPLIANCE_UPDATE_MANIFEST_BAD_DIGEST;
        }
    }
    return APPLIANCE_UPDATE_MANIFEST_OK;
}

enum appliance_update_manifest_status appliance_update_manifest_parse(
    const char *input,
    size_t input_length,
    struct appliance_update_manifest *manifest_out)
{
    struct appliance_update_manifest draft;
    bool identity_seen[APPLIANCE_UPDATE_MANIFEST_IDENTITY_FIELD_COUNT] =
        {false};
    char compatibility_input[
        APPLIANCE_UPDATE_MANIFEST_COMPATIBILITY_INPUT_MAX];
    size_t compatibility_length = 0U;
    size_t position = 0U;
    uint32_t index;
    enum update_compatibility_status compatibility_status;
    enum appliance_update_manifest_status status;

    if (input == NULL || manifest_out == NULL) {
        return APPLIANCE_UPDATE_MANIFEST_NULL;
    }
    if (input_length > APPLIANCE_UPDATE_MANIFEST_INPUT_MAX) {
        return APPLIANCE_UPDATE_MANIFEST_TOO_LONG;
    }
    appliance_update_manifest_zero_bytes(
        (char *)&draft, sizeof(draft));
    appliance_update_manifest_zero_bytes(
        compatibility_input, sizeof(compatibility_input));

    while (position < input_length) {
        const size_t line_start = position;
        size_t line_length;

        while (position < input_length &&
               input[position] != '\n') {
            ++position;
        }
        line_length = position - line_start;
        status = appliance_update_manifest_parse_line(
            input + line_start,
            line_length,
            &draft,
            identity_seen,
            compatibility_input,
            &compatibility_length);
        if (status != APPLIANCE_UPDATE_MANIFEST_OK) {
            return status;
        }
        if (position < input_length) {
            ++position;
        }
    }

    for (index = 0U;
         index < APPLIANCE_UPDATE_MANIFEST_IDENTITY_FIELD_COUNT;
         ++index) {
        if (!identity_seen[index]) {
            return APPLIANCE_UPDATE_MANIFEST_MISSING_FIELD;
        }
    }
    compatibility_status = update_compatibility_manifest_parse(
        compatibility_input,
        compatibility_length,
        &draft.compatibility);
    if (compatibility_status != UPDATE_COMPATIBILITY_OK) {
        return appliance_update_manifest_map_compatibility(
            compatibility_status);
    }
    status = appliance_update_manifest_identity_valid(&draft);
    if (status != APPLIANCE_UPDATE_MANIFEST_OK) {
        return status;
    }
    appliance_update_manifest_copy_bytes(
        (char *)manifest_out, (const char *)&draft, sizeof(draft));
    return APPLIANCE_UPDATE_MANIFEST_OK;
}

enum appliance_update_manifest_status appliance_update_manifest_check(
    const struct appliance_update_manifest *manifest,
    const struct update_compatibility_source *source,
    const struct update_compatibility_target *target,
    enum update_compatibility_status *compatibility_status_out)
{
    enum appliance_update_manifest_status status;
    enum update_compatibility_status compatibility_status;

    if (manifest == NULL || source == NULL || target == NULL) {
        return APPLIANCE_UPDATE_MANIFEST_NULL;
    }
    status = appliance_update_manifest_identity_valid(manifest);
    if (status != APPLIANCE_UPDATE_MANIFEST_OK) {
        if (compatibility_status_out != NULL) {
            *compatibility_status_out = UPDATE_COMPATIBILITY_BAD_STATE;
        }
        return status;
    }
    compatibility_status = update_compatibility_check(
        &manifest->compatibility, source, target);
    if (compatibility_status_out != NULL) {
        *compatibility_status_out = compatibility_status;
    }
    if (compatibility_status == UPDATE_COMPATIBILITY_OK) {
        return APPLIANCE_UPDATE_MANIFEST_OK;
    }
    if (compatibility_status == UPDATE_COMPATIBILITY_INVALID_SOURCE) {
        return APPLIANCE_UPDATE_MANIFEST_INVALID_SOURCE;
    }
    if (compatibility_status == UPDATE_COMPATIBILITY_INVALID_TARGET) {
        return APPLIANCE_UPDATE_MANIFEST_INVALID_TARGET;
    }
    return APPLIANCE_UPDATE_MANIFEST_COMPATIBILITY_DENIED;
}

const char *appliance_update_manifest_status_string(
    enum appliance_update_manifest_status status)
{
    switch (status) {
    case APPLIANCE_UPDATE_MANIFEST_OK:
        return "APPLIANCE_UPDATE_MANIFEST_OK";
    case APPLIANCE_UPDATE_MANIFEST_NULL:
        return "APPLIANCE_UPDATE_MANIFEST_NULL";
    case APPLIANCE_UPDATE_MANIFEST_TOO_LONG:
        return "APPLIANCE_UPDATE_MANIFEST_TOO_LONG";
    case APPLIANCE_UPDATE_MANIFEST_BAD_VERSION:
        return "APPLIANCE_UPDATE_MANIFEST_BAD_VERSION";
    case APPLIANCE_UPDATE_MANIFEST_MISSING_FIELD:
        return "APPLIANCE_UPDATE_MANIFEST_MISSING_FIELD";
    case APPLIANCE_UPDATE_MANIFEST_DUPLICATE_FIELD:
        return "APPLIANCE_UPDATE_MANIFEST_DUPLICATE_FIELD";
    case APPLIANCE_UPDATE_MANIFEST_UNKNOWN_FIELD:
        return "APPLIANCE_UPDATE_MANIFEST_UNKNOWN_FIELD";
    case APPLIANCE_UPDATE_MANIFEST_MALFORMED_FIELD:
        return "APPLIANCE_UPDATE_MANIFEST_MALFORMED_FIELD";
    case APPLIANCE_UPDATE_MANIFEST_OUT_OF_RANGE:
        return "APPLIANCE_UPDATE_MANIFEST_OUT_OF_RANGE";
    case APPLIANCE_UPDATE_MANIFEST_BAD_RELEASE_ID:
        return "APPLIANCE_UPDATE_MANIFEST_BAD_RELEASE_ID";
    case APPLIANCE_UPDATE_MANIFEST_UNSUPPORTED_ARCH:
        return "APPLIANCE_UPDATE_MANIFEST_UNSUPPORTED_ARCH";
    case APPLIANCE_UPDATE_MANIFEST_BAD_REVISION:
        return "APPLIANCE_UPDATE_MANIFEST_BAD_REVISION";
    case APPLIANCE_UPDATE_MANIFEST_BAD_DIGEST:
        return "APPLIANCE_UPDATE_MANIFEST_BAD_DIGEST";
    case APPLIANCE_UPDATE_MANIFEST_INCOMPATIBLE_RANGE:
        return "APPLIANCE_UPDATE_MANIFEST_INCOMPATIBLE_RANGE";
    case APPLIANCE_UPDATE_MANIFEST_SAME_IMAGE:
        return "APPLIANCE_UPDATE_MANIFEST_SAME_IMAGE";
    case APPLIANCE_UPDATE_MANIFEST_INVALID_SOURCE:
        return "APPLIANCE_UPDATE_MANIFEST_INVALID_SOURCE";
    case APPLIANCE_UPDATE_MANIFEST_INVALID_TARGET:
        return "APPLIANCE_UPDATE_MANIFEST_INVALID_TARGET";
    case APPLIANCE_UPDATE_MANIFEST_COMPATIBILITY_DENIED:
        return "APPLIANCE_UPDATE_MANIFEST_COMPATIBILITY_DENIED";
    case APPLIANCE_UPDATE_MANIFEST_BAD_REQUEST:
        return "APPLIANCE_UPDATE_MANIFEST_BAD_REQUEST";
    default:
        return NULL;
    }
}
