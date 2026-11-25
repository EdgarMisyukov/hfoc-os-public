#include "update_compatibility.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UPDATE_COMPATIBILITY_FIELD_COUNT 19U

enum update_compatibility_field {
    UPDATE_COMPATIBILITY_FIELD_VERSION = 0,
    UPDATE_COMPATIBILITY_FIELD_SOURCE_IMAGE_ID = 1,
    UPDATE_COMPATIBILITY_FIELD_TARGET_IMAGE_ID = 2,
    UPDATE_COMPATIBILITY_FIELD_SOURCE_SCHEMA_MIN = 3,
    UPDATE_COMPATIBILITY_FIELD_SOURCE_SCHEMA_MAX = 4,
    UPDATE_COMPATIBILITY_FIELD_TARGET_SCHEMA_MIN = 5,
    UPDATE_COMPATIBILITY_FIELD_TARGET_SCHEMA_MAX = 6,
    UPDATE_COMPATIBILITY_FIELD_SOURCE_ABI_MIN = 7,
    UPDATE_COMPATIBILITY_FIELD_SOURCE_ABI_MAX = 8,
    UPDATE_COMPATIBILITY_FIELD_TARGET_ABI_MIN = 9,
    UPDATE_COMPATIBILITY_FIELD_TARGET_ABI_MAX = 10,
    UPDATE_COMPATIBILITY_FIELD_SOURCE_SERVICE_MIN = 11,
    UPDATE_COMPATIBILITY_FIELD_SOURCE_SERVICE_MAX = 12,
    UPDATE_COMPATIBILITY_FIELD_TARGET_SERVICE_MIN = 13,
    UPDATE_COMPATIBILITY_FIELD_TARGET_SERVICE_MAX = 14,
    UPDATE_COMPATIBILITY_FIELD_SOURCE_POLICY_MIN = 15,
    UPDATE_COMPATIBILITY_FIELD_SOURCE_POLICY_MAX = 16,
    UPDATE_COMPATIBILITY_FIELD_TARGET_POLICY_MIN = 17,
    UPDATE_COMPATIBILITY_FIELD_TARGET_POLICY_MAX = 18,
};

static size_t update_compatibility_strlen(const char *text) {
    size_t length = 0U;

    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static bool update_compatibility_mem_equal(const char *left,
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

static void update_compatibility_zero_bytes(char *destination,
                                            size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = '\0';
    }
}

static void update_compatibility_copy_bytes(char *destination,
                                            const char *source,
                                            size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = source[index];
    }
}

static bool update_compatibility_key_char_valid(unsigned char value) {
    return (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
           (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
           value == (unsigned char)'_';
}

static bool update_compatibility_parse_u32(const char *value,
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

static int update_compatibility_field_index(const char *key,
                                             size_t key_length)
{
    static const char *const names[UPDATE_COMPATIBILITY_FIELD_COUNT] = {
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

    for (index = 0U; index < UPDATE_COMPATIBILITY_FIELD_COUNT; ++index) {
        const size_t name_length = update_compatibility_strlen(names[index]);

        if (name_length == key_length &&
            update_compatibility_mem_equal(names[index], key, key_length)) {
            return (int)index;
        }
    }
    return -1;
}

static bool update_compatibility_value_in_range(uint32_t value,
                                                uint32_t minimum,
                                                uint32_t maximum)
{
    return value >= minimum && value <= maximum;
}

static bool update_compatibility_field_bounds(
    enum update_compatibility_field field,
    uint32_t *minimum,
    uint32_t *maximum)
{
    switch (field) {
    case UPDATE_COMPATIBILITY_FIELD_SOURCE_SCHEMA_MIN:
    case UPDATE_COMPATIBILITY_FIELD_SOURCE_SCHEMA_MAX:
    case UPDATE_COMPATIBILITY_FIELD_TARGET_SCHEMA_MIN:
    case UPDATE_COMPATIBILITY_FIELD_TARGET_SCHEMA_MAX:
    case UPDATE_COMPATIBILITY_FIELD_SOURCE_POLICY_MIN:
    case UPDATE_COMPATIBILITY_FIELD_SOURCE_POLICY_MAX:
    case UPDATE_COMPATIBILITY_FIELD_TARGET_POLICY_MIN:
    case UPDATE_COMPATIBILITY_FIELD_TARGET_POLICY_MAX:
        *minimum = UPDATE_COMPATIBILITY_SCHEMA_MIN;
        *maximum = UPDATE_COMPATIBILITY_SCHEMA_MAX;
        return true;
    case UPDATE_COMPATIBILITY_FIELD_SOURCE_ABI_MIN:
    case UPDATE_COMPATIBILITY_FIELD_SOURCE_ABI_MAX:
    case UPDATE_COMPATIBILITY_FIELD_TARGET_ABI_MIN:
    case UPDATE_COMPATIBILITY_FIELD_TARGET_ABI_MAX:
    case UPDATE_COMPATIBILITY_FIELD_SOURCE_SERVICE_MIN:
    case UPDATE_COMPATIBILITY_FIELD_SOURCE_SERVICE_MAX:
    case UPDATE_COMPATIBILITY_FIELD_TARGET_SERVICE_MIN:
    case UPDATE_COMPATIBILITY_FIELD_TARGET_SERVICE_MAX:
        *minimum = UPDATE_COMPATIBILITY_ABI_MIN;
        *maximum = UPDATE_COMPATIBILITY_ABI_MAX;
        return true;
    default:
        return false;
    }
}

static bool update_compatibility_manifest_ranges_valid(
    const struct update_compatibility_manifest *manifest)
{
    return manifest->source_schema_min <= manifest->source_schema_max &&
           manifest->target_schema_min <= manifest->target_schema_max &&
           manifest->source_kernel_abi_min <=
               manifest->source_kernel_abi_max &&
           manifest->target_kernel_abi_min <=
               manifest->target_kernel_abi_max &&
           manifest->source_service_abi_min <=
               manifest->source_service_abi_max &&
           manifest->target_service_abi_min <=
               manifest->target_service_abi_max &&
           manifest->source_policy_generation_min <=
               manifest->source_policy_generation_max &&
           manifest->target_policy_generation_min <=
               manifest->target_policy_generation_max;
}

static bool update_compatibility_source_valid(
    const struct update_compatibility_source *source)
{
    return source != NULL &&
           update_compatibility_value_in_range(
               source->image_id,
               UPDATE_COMPATIBILITY_IMAGE_ID_MIN,
               UPDATE_COMPATIBILITY_IMAGE_ID_MAX) &&
           update_compatibility_value_in_range(
               source->schema_version,
               UPDATE_COMPATIBILITY_SCHEMA_MIN,
               UPDATE_COMPATIBILITY_SCHEMA_MAX) &&
           update_compatibility_value_in_range(
               source->kernel_abi,
               UPDATE_COMPATIBILITY_ABI_MIN,
               UPDATE_COMPATIBILITY_ABI_MAX) &&
           update_compatibility_value_in_range(
               source->service_abi,
               UPDATE_COMPATIBILITY_SERVICE_ABI_MIN,
               UPDATE_COMPATIBILITY_SERVICE_ABI_MAX) &&
           update_compatibility_value_in_range(
               source->policy_generation,
               UPDATE_COMPATIBILITY_POLICY_MIN,
               UPDATE_COMPATIBILITY_POLICY_MAX);
}

static bool update_compatibility_target_valid(
    const struct update_compatibility_target *target)
{
    return target != NULL &&
           update_compatibility_value_in_range(
               target->image_id,
               UPDATE_COMPATIBILITY_IMAGE_ID_MIN,
               UPDATE_COMPATIBILITY_IMAGE_ID_MAX) &&
           update_compatibility_value_in_range(
               target->schema_version,
               UPDATE_COMPATIBILITY_SCHEMA_MIN,
               UPDATE_COMPATIBILITY_SCHEMA_MAX) &&
           update_compatibility_value_in_range(
               target->kernel_abi,
               UPDATE_COMPATIBILITY_ABI_MIN,
               UPDATE_COMPATIBILITY_ABI_MAX) &&
           update_compatibility_value_in_range(
               target->service_abi,
               UPDATE_COMPATIBILITY_SERVICE_ABI_MIN,
               UPDATE_COMPATIBILITY_SERVICE_ABI_MAX) &&
           update_compatibility_value_in_range(
               target->policy_generation,
               UPDATE_COMPATIBILITY_POLICY_MIN,
               UPDATE_COMPATIBILITY_POLICY_MAX);
}

enum update_compatibility_status update_compatibility_manifest_validate(
    const struct update_compatibility_manifest *manifest)
{
    uint32_t field_min;
    uint32_t field_max;

    if (manifest == NULL) {
        return UPDATE_COMPATIBILITY_NULL;
    }
    if (manifest->compatibility_version != UPDATE_COMPATIBILITY_VERSION) {
        return UPDATE_COMPATIBILITY_BAD_VERSION;
    }
    if (!update_compatibility_value_in_range(
            manifest->source_image_id,
            UPDATE_COMPATIBILITY_IMAGE_ID_MIN,
            UPDATE_COMPATIBILITY_IMAGE_ID_MAX) ||
        !update_compatibility_value_in_range(
            manifest->target_image_id,
            UPDATE_COMPATIBILITY_IMAGE_ID_MIN,
            UPDATE_COMPATIBILITY_IMAGE_ID_MAX)) {
        return UPDATE_COMPATIBILITY_OUT_OF_RANGE;
    }
    if (manifest->source_image_id == manifest->target_image_id) {
        return UPDATE_COMPATIBILITY_SAME_IMAGE;
    }

    if (!update_compatibility_field_bounds(
            UPDATE_COMPATIBILITY_FIELD_SOURCE_SCHEMA_MIN,
            &field_min, &field_max) ||
        !update_compatibility_value_in_range(
            manifest->source_schema_min, field_min, field_max) ||
        !update_compatibility_value_in_range(
            manifest->source_schema_max, field_min, field_max) ||
        !update_compatibility_value_in_range(
            manifest->target_schema_min, field_min, field_max) ||
        !update_compatibility_value_in_range(
            manifest->target_schema_max, field_min, field_max) ||
        !update_compatibility_value_in_range(
            manifest->source_policy_generation_min, field_min, field_max) ||
        !update_compatibility_value_in_range(
            manifest->source_policy_generation_max, field_min, field_max) ||
        !update_compatibility_value_in_range(
            manifest->target_policy_generation_min, field_min, field_max) ||
        !update_compatibility_value_in_range(
            manifest->target_policy_generation_max, field_min, field_max)) {
        return UPDATE_COMPATIBILITY_OUT_OF_RANGE;
    }

    if (!update_compatibility_field_bounds(
            UPDATE_COMPATIBILITY_FIELD_SOURCE_ABI_MIN,
            &field_min, &field_max) ||
        !update_compatibility_value_in_range(
            manifest->source_kernel_abi_min, field_min, field_max) ||
        !update_compatibility_value_in_range(
            manifest->source_kernel_abi_max, field_min, field_max) ||
        !update_compatibility_value_in_range(
            manifest->target_kernel_abi_min, field_min, field_max) ||
        !update_compatibility_value_in_range(
            manifest->target_kernel_abi_max, field_min, field_max) ||
        !update_compatibility_value_in_range(
            manifest->source_service_abi_min, field_min, field_max) ||
        !update_compatibility_value_in_range(
            manifest->source_service_abi_max, field_min, field_max) ||
        !update_compatibility_value_in_range(
            manifest->target_service_abi_min, field_min, field_max) ||
        !update_compatibility_value_in_range(
            manifest->target_service_abi_max, field_min, field_max)) {
        return UPDATE_COMPATIBILITY_OUT_OF_RANGE;
    }

    if (!update_compatibility_manifest_ranges_valid(manifest)) {
        return UPDATE_COMPATIBILITY_INCOMPATIBLE_RANGE;
    }
    return UPDATE_COMPATIBILITY_OK;
}

static enum update_compatibility_status update_compatibility_parse_header(
    const char *key,
    size_t key_length,
    const char *value,
    size_t value_length,
    struct update_compatibility_manifest *draft,
    bool *seen)
{
    int field;
    uint32_t parsed_value;
    uint32_t field_min;
    uint32_t field_max;
    size_t index;

    if (key_length == 0U || key_length > UPDATE_COMPATIBILITY_KEY_MAX ||
        value_length == 0U || value_length > UPDATE_COMPATIBILITY_LINE_MAX) {
        return UPDATE_COMPATIBILITY_MALFORMED_FIELD;
    }
    for (index = 0U; index < key_length; ++index) {
        if (!update_compatibility_key_char_valid(
                (unsigned char)key[index])) {
            return UPDATE_COMPATIBILITY_MALFORMED_FIELD;
        }
    }
    for (index = 0U; index < value_length; ++index) {
        if (value[index] < '0' || value[index] > '9') {
            return UPDATE_COMPATIBILITY_MALFORMED_FIELD;
        }
    }
    if (!update_compatibility_parse_u32(
            value, value_length, &parsed_value)) {
        return UPDATE_COMPATIBILITY_OUT_OF_RANGE;
    }

    field = update_compatibility_field_index(key, key_length);
    if (field < 0) {
        return UPDATE_COMPATIBILITY_UNKNOWN_FIELD;
    }
    if (seen[field]) {
        return UPDATE_COMPATIBILITY_DUPLICATE_FIELD;
    }

    switch ((enum update_compatibility_field)field) {
    case UPDATE_COMPATIBILITY_FIELD_VERSION:
        if (parsed_value != UPDATE_COMPATIBILITY_VERSION) {
            return UPDATE_COMPATIBILITY_BAD_VERSION;
        }
        draft->compatibility_version = parsed_value;
        break;
    case UPDATE_COMPATIBILITY_FIELD_SOURCE_IMAGE_ID:
        if (!update_compatibility_value_in_range(
                parsed_value,
                UPDATE_COMPATIBILITY_IMAGE_ID_MIN,
                UPDATE_COMPATIBILITY_IMAGE_ID_MAX)) {
            return UPDATE_COMPATIBILITY_OUT_OF_RANGE;
        }
        draft->source_image_id = parsed_value;
        break;
    case UPDATE_COMPATIBILITY_FIELD_TARGET_IMAGE_ID:
        if (!update_compatibility_value_in_range(
                parsed_value,
                UPDATE_COMPATIBILITY_IMAGE_ID_MIN,
                UPDATE_COMPATIBILITY_IMAGE_ID_MAX)) {
            return UPDATE_COMPATIBILITY_OUT_OF_RANGE;
        }
        draft->target_image_id = parsed_value;
        break;
    default:
        if (!update_compatibility_field_bounds(
                (enum update_compatibility_field)field,
                &field_min, &field_max)) {
            return UPDATE_COMPATIBILITY_UNKNOWN_FIELD;
        }
        if (!update_compatibility_value_in_range(
                parsed_value, field_min, field_max)) {
            return UPDATE_COMPATIBILITY_OUT_OF_RANGE;
        }
        switch ((enum update_compatibility_field)field) {
        case UPDATE_COMPATIBILITY_FIELD_SOURCE_SCHEMA_MIN:
            draft->source_schema_min = parsed_value;
            break;
        case UPDATE_COMPATIBILITY_FIELD_SOURCE_SCHEMA_MAX:
            draft->source_schema_max = parsed_value;
            break;
        case UPDATE_COMPATIBILITY_FIELD_TARGET_SCHEMA_MIN:
            draft->target_schema_min = parsed_value;
            break;
        case UPDATE_COMPATIBILITY_FIELD_TARGET_SCHEMA_MAX:
            draft->target_schema_max = parsed_value;
            break;
        case UPDATE_COMPATIBILITY_FIELD_SOURCE_ABI_MIN:
            draft->source_kernel_abi_min = parsed_value;
            break;
        case UPDATE_COMPATIBILITY_FIELD_SOURCE_ABI_MAX:
            draft->source_kernel_abi_max = parsed_value;
            break;
        case UPDATE_COMPATIBILITY_FIELD_TARGET_ABI_MIN:
            draft->target_kernel_abi_min = parsed_value;
            break;
        case UPDATE_COMPATIBILITY_FIELD_TARGET_ABI_MAX:
            draft->target_kernel_abi_max = parsed_value;
            break;
        case UPDATE_COMPATIBILITY_FIELD_SOURCE_SERVICE_MIN:
            draft->source_service_abi_min = parsed_value;
            break;
        case UPDATE_COMPATIBILITY_FIELD_SOURCE_SERVICE_MAX:
            draft->source_service_abi_max = parsed_value;
            break;
        case UPDATE_COMPATIBILITY_FIELD_TARGET_SERVICE_MIN:
            draft->target_service_abi_min = parsed_value;
            break;
        case UPDATE_COMPATIBILITY_FIELD_TARGET_SERVICE_MAX:
            draft->target_service_abi_max = parsed_value;
            break;
        case UPDATE_COMPATIBILITY_FIELD_SOURCE_POLICY_MIN:
            draft->source_policy_generation_min = parsed_value;
            break;
        case UPDATE_COMPATIBILITY_FIELD_SOURCE_POLICY_MAX:
            draft->source_policy_generation_max = parsed_value;
            break;
        case UPDATE_COMPATIBILITY_FIELD_TARGET_POLICY_MIN:
            draft->target_policy_generation_min = parsed_value;
            break;
        case UPDATE_COMPATIBILITY_FIELD_TARGET_POLICY_MAX:
            draft->target_policy_generation_max = parsed_value;
            break;
        default:
            return UPDATE_COMPATIBILITY_UNKNOWN_FIELD;
        }
        break;
    }

    seen[field] = true;
    return UPDATE_COMPATIBILITY_OK;
}

static enum update_compatibility_status update_compatibility_parse_line(
    const char *line,
    size_t line_length,
    struct update_compatibility_manifest *draft,
    bool *seen)
{
    size_t equals_index = 0U;
    bool equals_found = false;
    size_t index;

    if (line_length == 0U || line_length > UPDATE_COMPATIBILITY_LINE_MAX) {
        return UPDATE_COMPATIBILITY_MALFORMED_FIELD;
    }
    for (index = 0U; index < line_length; ++index) {
        if (line[index] == '=') {
            if (equals_found) {
                return UPDATE_COMPATIBILITY_MALFORMED_FIELD;
            }
            equals_found = true;
            equals_index = index;
        }
    }
    if (!equals_found) {
        return UPDATE_COMPATIBILITY_MALFORMED_FIELD;
    }
    return update_compatibility_parse_header(
        line, equals_index, line + equals_index + 1U,
        line_length - equals_index - 1U, draft, seen);
}

enum update_compatibility_status update_compatibility_manifest_parse(
    const char *input,
    size_t input_length,
    struct update_compatibility_manifest *manifest_out)
{
    struct update_compatibility_manifest draft;
    bool seen[UPDATE_COMPATIBILITY_FIELD_COUNT] = {false};
    size_t position = 0U;
    uint32_t index;
    enum update_compatibility_status status;

    if (input == NULL || manifest_out == NULL) {
        return UPDATE_COMPATIBILITY_NULL;
    }
    if (input_length > UPDATE_COMPATIBILITY_INPUT_MAX) {
        return UPDATE_COMPATIBILITY_TOO_LONG;
    }
    update_compatibility_zero_bytes((char *)&draft, sizeof(draft));

    while (position < input_length) {
        const size_t line_start = position;
        size_t line_length;
        enum update_compatibility_status status;

        while (position < input_length && input[position] != '\n') {
            ++position;
        }
        line_length = position - line_start;
        status = update_compatibility_parse_line(
            input + line_start, line_length, &draft, seen);
        if (status != UPDATE_COMPATIBILITY_OK) {
            return status;
        }
        if (position < input_length) {
            ++position;
        }
    }

    for (index = 0U; index < UPDATE_COMPATIBILITY_FIELD_COUNT; ++index) {
        if (!seen[index]) {
            return UPDATE_COMPATIBILITY_MISSING_FIELD;
        }
    }
    status = update_compatibility_manifest_validate(&draft);
    if (status != UPDATE_COMPATIBILITY_OK) {
        return status;
    }

    update_compatibility_copy_bytes(
        (char *)manifest_out, (const char *)&draft, sizeof(draft));
    return UPDATE_COMPATIBILITY_OK;
}

enum update_compatibility_status update_compatibility_check(
    const struct update_compatibility_manifest *candidate,
    const struct update_compatibility_source *source,
    const struct update_compatibility_target *target)
{
    enum update_compatibility_status status;

    if (candidate == NULL || source == NULL || target == NULL) {
        return UPDATE_COMPATIBILITY_NULL;
    }
    status = update_compatibility_manifest_validate(candidate);
    if (status != UPDATE_COMPATIBILITY_OK) {
        return status;
    }
    if (!update_compatibility_source_valid(source)) {
        return UPDATE_COMPATIBILITY_INVALID_SOURCE;
    }
    if (!update_compatibility_target_valid(target)) {
        return UPDATE_COMPATIBILITY_INVALID_TARGET;
    }

    if (source->image_id != candidate->source_image_id) {
        return UPDATE_COMPATIBILITY_SOURCE_IMAGE_MISMATCH;
    }
    if (!update_compatibility_value_in_range(
            source->schema_version,
            candidate->source_schema_min,
            candidate->source_schema_max)) {
        return UPDATE_COMPATIBILITY_SOURCE_SCHEMA_MISMATCH;
    }
    if (!update_compatibility_value_in_range(
            source->kernel_abi,
            candidate->source_kernel_abi_min,
            candidate->source_kernel_abi_max)) {
        return UPDATE_COMPATIBILITY_SOURCE_ABI_MISMATCH;
    }
    if (!update_compatibility_value_in_range(
            source->service_abi,
            candidate->source_service_abi_min,
            candidate->source_service_abi_max)) {
        return UPDATE_COMPATIBILITY_SOURCE_SERVICE_MISMATCH;
    }
    if (!update_compatibility_value_in_range(
            source->policy_generation,
            candidate->source_policy_generation_min,
            candidate->source_policy_generation_max)) {
        return UPDATE_COMPATIBILITY_SOURCE_POLICY_MISMATCH;
    }

    if (target->image_id != candidate->target_image_id) {
        return UPDATE_COMPATIBILITY_TARGET_IMAGE_MISMATCH;
    }
    if (!update_compatibility_value_in_range(
            target->schema_version,
            candidate->target_schema_min,
            candidate->target_schema_max)) {
        return UPDATE_COMPATIBILITY_TARGET_SCHEMA_MISMATCH;
    }
    if (!update_compatibility_value_in_range(
            target->kernel_abi,
            candidate->target_kernel_abi_min,
            candidate->target_kernel_abi_max)) {
        return UPDATE_COMPATIBILITY_TARGET_ABI_MISMATCH;
    }
    if (!update_compatibility_value_in_range(
            target->service_abi,
            candidate->target_service_abi_min,
            candidate->target_service_abi_max)) {
        return UPDATE_COMPATIBILITY_TARGET_SERVICE_MISMATCH;
    }
    if (!update_compatibility_value_in_range(
            target->policy_generation,
            candidate->target_policy_generation_min,
            candidate->target_policy_generation_max)) {
        return UPDATE_COMPATIBILITY_TARGET_POLICY_MISMATCH;
    }
    return UPDATE_COMPATIBILITY_OK;
}

enum update_compatibility_status update_compatibility_gate_init(
    struct update_compatibility_gate *gate)
{
    if (gate == NULL) {
        return UPDATE_COMPATIBILITY_NULL;
    }
    update_compatibility_zero_bytes((char *)gate, sizeof(*gate));
    gate->version = UPDATE_COMPATIBILITY_VERSION;
    return UPDATE_COMPATIBILITY_OK;
}

enum update_compatibility_status update_compatibility_stage(
    struct update_compatibility_gate *gate,
    const struct update_compatibility_manifest *candidate,
    const struct update_compatibility_source *source,
    const struct update_compatibility_target *target)
{
    struct update_compatibility_gate draft;
    enum update_compatibility_status status;

    if (gate == NULL || candidate == NULL || source == NULL ||
        target == NULL) {
        return UPDATE_COMPATIBILITY_NULL;
    }
    if (gate->staged) {
        return UPDATE_COMPATIBILITY_BAD_STATE;
    }
    status = update_compatibility_check(candidate, source, target);
    if (status != UPDATE_COMPATIBILITY_OK) {
        return status;
    }

    update_compatibility_zero_bytes((char *)&draft, sizeof(draft));
    draft.version = UPDATE_COMPATIBILITY_VERSION;
    draft.manifest = *candidate;
    draft.staged = true;
    update_compatibility_copy_bytes(
        (char *)gate, (const char *)&draft, sizeof(draft));
    return UPDATE_COMPATIBILITY_OK;
}

const char *update_compatibility_status_string(
    enum update_compatibility_status status)
{
    switch (status) {
    case UPDATE_COMPATIBILITY_OK:
        return "UPDATE_COMPATIBILITY_OK";
    case UPDATE_COMPATIBILITY_NULL:
        return "UPDATE_COMPATIBILITY_NULL";
    case UPDATE_COMPATIBILITY_BAD_STATE:
        return "UPDATE_COMPATIBILITY_BAD_STATE";
    case UPDATE_COMPATIBILITY_TOO_LONG:
        return "UPDATE_COMPATIBILITY_TOO_LONG";
    case UPDATE_COMPATIBILITY_BAD_VERSION:
        return "UPDATE_COMPATIBILITY_BAD_VERSION";
    case UPDATE_COMPATIBILITY_MISSING_FIELD:
        return "UPDATE_COMPATIBILITY_MISSING_FIELD";
    case UPDATE_COMPATIBILITY_DUPLICATE_FIELD:
        return "UPDATE_COMPATIBILITY_DUPLICATE_FIELD";
    case UPDATE_COMPATIBILITY_UNKNOWN_FIELD:
        return "UPDATE_COMPATIBILITY_UNKNOWN_FIELD";
    case UPDATE_COMPATIBILITY_MALFORMED_FIELD:
        return "UPDATE_COMPATIBILITY_MALFORMED_FIELD";
    case UPDATE_COMPATIBILITY_OUT_OF_RANGE:
        return "UPDATE_COMPATIBILITY_OUT_OF_RANGE";
    case UPDATE_COMPATIBILITY_INCOMPATIBLE_RANGE:
        return "UPDATE_COMPATIBILITY_INCOMPATIBLE_RANGE";
    case UPDATE_COMPATIBILITY_INVALID_SOURCE:
        return "UPDATE_COMPATIBILITY_INVALID_SOURCE";
    case UPDATE_COMPATIBILITY_INVALID_TARGET:
        return "UPDATE_COMPATIBILITY_INVALID_TARGET";
    case UPDATE_COMPATIBILITY_SOURCE_IMAGE_MISMATCH:
        return "UPDATE_COMPATIBILITY_SOURCE_IMAGE_MISMATCH";
    case UPDATE_COMPATIBILITY_SOURCE_SCHEMA_MISMATCH:
        return "UPDATE_COMPATIBILITY_SOURCE_SCHEMA_MISMATCH";
    case UPDATE_COMPATIBILITY_SOURCE_ABI_MISMATCH:
        return "UPDATE_COMPATIBILITY_SOURCE_ABI_MISMATCH";
    case UPDATE_COMPATIBILITY_SOURCE_SERVICE_MISMATCH:
        return "UPDATE_COMPATIBILITY_SOURCE_SERVICE_MISMATCH";
    case UPDATE_COMPATIBILITY_SOURCE_POLICY_MISMATCH:
        return "UPDATE_COMPATIBILITY_SOURCE_POLICY_MISMATCH";
    case UPDATE_COMPATIBILITY_TARGET_IMAGE_MISMATCH:
        return "UPDATE_COMPATIBILITY_TARGET_IMAGE_MISMATCH";
    case UPDATE_COMPATIBILITY_TARGET_SCHEMA_MISMATCH:
        return "UPDATE_COMPATIBILITY_TARGET_SCHEMA_MISMATCH";
    case UPDATE_COMPATIBILITY_TARGET_ABI_MISMATCH:
        return "UPDATE_COMPATIBILITY_TARGET_ABI_MISMATCH";
    case UPDATE_COMPATIBILITY_TARGET_SERVICE_MISMATCH:
        return "UPDATE_COMPATIBILITY_TARGET_SERVICE_MISMATCH";
    case UPDATE_COMPATIBILITY_TARGET_POLICY_MISMATCH:
        return "UPDATE_COMPATIBILITY_TARGET_POLICY_MISMATCH";
    case UPDATE_COMPATIBILITY_SAME_IMAGE:
        return "UPDATE_COMPATIBILITY_SAME_IMAGE";
    default:
        return NULL;
    }
}
