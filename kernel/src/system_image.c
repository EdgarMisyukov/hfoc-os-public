#include "system_image.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SYSTEM_IMAGE_HEADER_FIELD_COUNT 5U

enum system_image_header_field {
    SYSTEM_IMAGE_FIELD_MANIFEST_VERSION = 0,
    SYSTEM_IMAGE_FIELD_IMAGE_ID = 1,
    SYSTEM_IMAGE_FIELD_ARCH = 2,
    SYSTEM_IMAGE_FIELD_MIN_KERNEL_ABI = 3,
    SYSTEM_IMAGE_FIELD_MAX_KERNEL_ABI = 4,
};

static size_t system_image_strlen(const char *text) {
    size_t length = 0U;

    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static bool system_image_mem_equal(const char *left,
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

static void system_image_copy_bytes(char *destination,
                                    const char *source,
                                    size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = source[index];
    }
}

static void system_image_zero_bytes(char *destination, size_t count) {
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = '\0';
    }
}

static bool system_image_key_char_valid(unsigned char value) {
    return (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
           (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
           (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
           value == (unsigned char)'_';
}

static bool system_image_arch_char_valid(unsigned char value) {
    return (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
           (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
           value == (unsigned char)'_';
}

static bool system_image_name_char_valid(unsigned char value) {
    return (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
           (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
           value == (unsigned char)'_' || value == (unsigned char)'.' ||
           value == (unsigned char)'-';
}

static bool system_image_version_char_valid(unsigned char value) {
    return (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
           (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
           (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
           value == (unsigned char)'_' || value == (unsigned char)'.' ||
           value == (unsigned char)'+' || value == (unsigned char)'-';
}

static bool system_image_hex_char_valid(unsigned char value) {
    return (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
           (value >= (unsigned char)'a' && value <= (unsigned char)'f') ||
           (value >= (unsigned char)'A' && value <= (unsigned char)'F');
}

static bool system_image_parse_u32(const char *value,
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

static int system_image_header_index(const char *key,
                                     size_t key_length)
{
    static const char *const names[SYSTEM_IMAGE_HEADER_FIELD_COUNT] = {
        "manifest_version",
        "image_id",
        "arch",
        "min_kernel_abi",
        "max_kernel_abi",
    };
    uint32_t index;

    for (index = 0U; index < SYSTEM_IMAGE_HEADER_FIELD_COUNT; ++index) {
        const size_t name_length = system_image_strlen(names[index]);

        if (name_length == key_length &&
            system_image_mem_equal(names[index], key, key_length)) {
            return (int)index;
        }
    }
    return -1;
}

static bool system_image_component_exists(
    const struct system_image_manifest *draft,
    const char *name,
    size_t name_length)
{
    uint32_t index;

    for (index = 0U; index < draft->component_count; ++index) {
        const size_t existing_length =
            system_image_strlen(draft->components[index].name);

        if (existing_length == name_length &&
            system_image_mem_equal(draft->components[index].name,
                                   name, name_length)) {
            return true;
        }
    }
    return false;
}

static enum system_image_status system_image_parse_component(
    const char *value,
    size_t value_length,
    struct system_image_manifest *draft)
{
    size_t first_colon = 0U;
    size_t second_colon = 0U;
    size_t colon_count = 0U;
    size_t name_length;
    size_t version_length;
    size_t hash_length;
    size_t index;

    for (index = 0U; index < value_length; ++index) {
        if (value[index] != ':') {
            continue;
        }
        if (colon_count == 0U) {
            first_colon = index;
        } else if (colon_count == 1U) {
            second_colon = index;
        }
        ++colon_count;
    }
    if (colon_count != 2U) {
        return SYSTEM_IMAGE_MALFORMED_FIELD;
    }

    name_length = first_colon;
    version_length = second_colon - first_colon - 1U;
    hash_length = value_length - second_colon - 1U;
    if (name_length == 0U ||
        name_length > SYSTEM_IMAGE_COMPONENT_NAME_MAX ||
        version_length == 0U ||
        version_length > SYSTEM_IMAGE_COMPONENT_VERSION_MAX ||
        hash_length != SYSTEM_IMAGE_HASH_HEX_LEN) {
        return SYSTEM_IMAGE_MALFORMED_FIELD;
    }
    for (index = 0U; index < name_length; ++index) {
        if (!system_image_name_char_valid((unsigned char)value[index])) {
            return SYSTEM_IMAGE_MALFORMED_FIELD;
        }
    }
    for (index = 0U; index < version_length; ++index) {
        if (!system_image_version_char_valid(
                (unsigned char)value[first_colon + 1U + index])) {
            return SYSTEM_IMAGE_MALFORMED_FIELD;
        }
    }
    for (index = 0U; index < hash_length; ++index) {
        if (!system_image_hex_char_valid(
                (unsigned char)value[second_colon + 1U + index])) {
            return SYSTEM_IMAGE_MALFORMED_FIELD;
        }
    }
    if (system_image_component_exists(draft, value, name_length)) {
        return SYSTEM_IMAGE_DUPLICATE_COMPONENT;
    }
    if (draft->component_count >= SYSTEM_IMAGE_COMPONENT_MAX) {
        return SYSTEM_IMAGE_TOO_MANY_COMPONENTS;
    }

    system_image_copy_bytes(draft->components[draft->component_count].name,
                            value, name_length);
    draft->components[draft->component_count].name[name_length] = '\0';
    system_image_copy_bytes(
        draft->components[draft->component_count].version,
        value + first_colon + 1U, version_length);
    draft->components[draft->component_count]
        .version[version_length] = '\0';
    system_image_copy_bytes(
        draft->components[draft->component_count].hash,
        value + second_colon + 1U, hash_length);
    draft->components[draft->component_count].hash[hash_length] = '\0';
    ++draft->component_count;
    return SYSTEM_IMAGE_OK;
}

static enum system_image_status system_image_parse_header(
    const char *key,
    size_t key_length,
    const char *value,
    size_t value_length,
    struct system_image_manifest *draft,
    bool *seen)
{
    int field;
    uint32_t parsed_value;
    size_t index;

    if (key_length == 0U || key_length > SYSTEM_IMAGE_KEY_MAX ||
        value_length == 0U || value_length > SYSTEM_IMAGE_LINE_MAX) {
        return SYSTEM_IMAGE_MALFORMED_FIELD;
    }
    for (index = 0U; index < key_length; ++index) {
        if (!system_image_key_char_valid((unsigned char)key[index])) {
            return SYSTEM_IMAGE_MALFORMED_FIELD;
        }
    }
    if (key_length == system_image_strlen("component") &&
        system_image_mem_equal(key, "component", key_length)) {
        return system_image_parse_component(value, value_length, draft);
    }

    field = system_image_header_index(key, key_length);
    if (field < 0) {
        return SYSTEM_IMAGE_UNKNOWN_FIELD;
    }
    if (seen[field]) {
        return SYSTEM_IMAGE_DUPLICATE_FIELD;
    }

    if (field == SYSTEM_IMAGE_FIELD_ARCH) {
        if (value_length > SYSTEM_IMAGE_ARCH_MAX) {
            return SYSTEM_IMAGE_MALFORMED_FIELD;
        }
        for (index = 0U; index < value_length; ++index) {
            if (!system_image_arch_char_valid((unsigned char)value[index])) {
                return SYSTEM_IMAGE_MALFORMED_FIELD;
            }
        }
        if (value_length != system_image_strlen(SYSTEM_IMAGE_ARCH_VALUE) ||
            !system_image_mem_equal(value, SYSTEM_IMAGE_ARCH_VALUE,
                                    value_length)) {
            return SYSTEM_IMAGE_UNSUPPORTED_ARCH;
        }
        system_image_copy_bytes(draft->arch, value, value_length);
        draft->arch[value_length] = '\0';
    } else {
        for (index = 0U; index < value_length; ++index) {
            if (value[index] < '0' || value[index] > '9') {
                return SYSTEM_IMAGE_MALFORMED_FIELD;
            }
        }
        if (!system_image_parse_u32(value, value_length, &parsed_value)) {
            return SYSTEM_IMAGE_OUT_OF_RANGE;
        }
        switch (field) {
        case SYSTEM_IMAGE_FIELD_MANIFEST_VERSION:
            if (parsed_value != SYSTEM_IMAGE_MANIFEST_VERSION) {
                return SYSTEM_IMAGE_BAD_VERSION;
            }
            draft->manifest_version = parsed_value;
            break;
        case SYSTEM_IMAGE_FIELD_IMAGE_ID:
            if (parsed_value < SYSTEM_IMAGE_IMAGE_ID_MIN ||
                parsed_value > SYSTEM_IMAGE_IMAGE_ID_MAX) {
                return SYSTEM_IMAGE_OUT_OF_RANGE;
            }
            draft->image_id = parsed_value;
            break;
        case SYSTEM_IMAGE_FIELD_MIN_KERNEL_ABI:
            if (parsed_value < SYSTEM_IMAGE_KERNEL_ABI_MIN ||
                parsed_value > SYSTEM_IMAGE_KERNEL_ABI_MAX) {
                return SYSTEM_IMAGE_OUT_OF_RANGE;
            }
            draft->min_kernel_abi = parsed_value;
            break;
        default:
            if (parsed_value < SYSTEM_IMAGE_KERNEL_ABI_MIN ||
                parsed_value > SYSTEM_IMAGE_KERNEL_ABI_MAX) {
                return SYSTEM_IMAGE_OUT_OF_RANGE;
            }
            draft->max_kernel_abi = parsed_value;
            break;
        }
    }
    seen[field] = true;
    return SYSTEM_IMAGE_OK;
}

static enum system_image_status system_image_parse_line(
    const char *line,
    size_t line_length,
    struct system_image_manifest *draft,
    bool *seen)
{
    size_t equals_index = 0U;
    bool equals_found = false;
    size_t index;

    if (line_length == 0U || line_length > SYSTEM_IMAGE_LINE_MAX) {
        return SYSTEM_IMAGE_MALFORMED_FIELD;
    }
    for (index = 0U; index < line_length; ++index) {
        if (line[index] == '=') {
            if (equals_found) {
                return SYSTEM_IMAGE_MALFORMED_FIELD;
            }
            equals_found = true;
            equals_index = index;
        }
    }
    if (!equals_found) {
        return SYSTEM_IMAGE_MALFORMED_FIELD;
    }
    return system_image_parse_header(line, equals_index,
                                     line + equals_index + 1U,
                                     line_length - equals_index - 1U,
                                     draft, seen);
}

enum system_image_status system_image_manifest_parse(
    const char *input,
    size_t input_length,
    struct system_image_manifest *manifest_out)
{
    struct system_image_manifest draft;
    bool seen[SYSTEM_IMAGE_HEADER_FIELD_COUNT] = {false};
    size_t position = 0U;
    uint32_t index;

    if (input == NULL || manifest_out == NULL) {
        return SYSTEM_IMAGE_NULL;
    }
    if (input_length > SYSTEM_IMAGE_INPUT_MAX) {
        return SYSTEM_IMAGE_TOO_LONG;
    }
    system_image_zero_bytes((char *)&draft, sizeof(draft));

    while (position < input_length) {
        const size_t line_start = position;
        size_t line_length;
        enum system_image_status status;

        while (position < input_length && input[position] != '\n') {
            ++position;
        }
        line_length = position - line_start;
        status = system_image_parse_line(input + line_start, line_length,
                                         &draft, seen);
        if (status != SYSTEM_IMAGE_OK) {
            return status;
        }
        if (position < input_length) {
            ++position;
        }
    }

    for (index = 0U; index < SYSTEM_IMAGE_HEADER_FIELD_COUNT; ++index) {
        if (!seen[index]) {
            return SYSTEM_IMAGE_MISSING_FIELD;
        }
    }
    if (draft.component_count == 0U) {
        return SYSTEM_IMAGE_EMPTY_COMPONENT_LIST;
    }
    if (draft.min_kernel_abi > draft.max_kernel_abi) {
        return SYSTEM_IMAGE_INCOMPATIBLE_ABI;
    }
    *manifest_out = draft;
    return SYSTEM_IMAGE_OK;
}

const char *system_image_status_string(enum system_image_status status) {
    switch (status) {
    case SYSTEM_IMAGE_OK:
        return "SYSTEM_IMAGE_OK";
    case SYSTEM_IMAGE_NULL:
        return "SYSTEM_IMAGE_NULL";
    case SYSTEM_IMAGE_TOO_LONG:
        return "SYSTEM_IMAGE_TOO_LONG";
    case SYSTEM_IMAGE_BAD_VERSION:
        return "SYSTEM_IMAGE_BAD_VERSION";
    case SYSTEM_IMAGE_MISSING_FIELD:
        return "SYSTEM_IMAGE_MISSING_FIELD";
    case SYSTEM_IMAGE_DUPLICATE_FIELD:
        return "SYSTEM_IMAGE_DUPLICATE_FIELD";
    case SYSTEM_IMAGE_UNKNOWN_FIELD:
        return "SYSTEM_IMAGE_UNKNOWN_FIELD";
    case SYSTEM_IMAGE_MALFORMED_FIELD:
        return "SYSTEM_IMAGE_MALFORMED_FIELD";
    case SYSTEM_IMAGE_OUT_OF_RANGE:
        return "SYSTEM_IMAGE_OUT_OF_RANGE";
    case SYSTEM_IMAGE_EMPTY_COMPONENT_LIST:
        return "SYSTEM_IMAGE_EMPTY_COMPONENT_LIST";
    case SYSTEM_IMAGE_TOO_MANY_COMPONENTS:
        return "SYSTEM_IMAGE_TOO_MANY_COMPONENTS";
    case SYSTEM_IMAGE_DUPLICATE_COMPONENT:
        return "SYSTEM_IMAGE_DUPLICATE_COMPONENT";
    case SYSTEM_IMAGE_UNSUPPORTED_ARCH:
        return "SYSTEM_IMAGE_UNSUPPORTED_ARCH";
    case SYSTEM_IMAGE_INCOMPATIBLE_ABI:
        return "SYSTEM_IMAGE_INCOMPATIBLE_ABI";
    default:
        return "SYSTEM_IMAGE_UNKNOWN";
    }
}
