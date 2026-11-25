#ifndef HFOC_UPDATE_COMPATIBILITY_H
#define HFOC_UPDATE_COMPATIBILITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UPDATE_COMPATIBILITY_VERSION 1U
#define UPDATE_COMPATIBILITY_INPUT_MAX 8192U
#define UPDATE_COMPATIBILITY_LINE_MAX 256U
#define UPDATE_COMPATIBILITY_KEY_MAX 32U
#define UPDATE_COMPATIBILITY_IMAGE_ID_MIN 1U
#define UPDATE_COMPATIBILITY_IMAGE_ID_MAX 4096U
#define UPDATE_COMPATIBILITY_SCHEMA_MIN 1U
#define UPDATE_COMPATIBILITY_SCHEMA_MAX 0xFFFFFFFFU
#define UPDATE_COMPATIBILITY_ABI_MIN 1U
#define UPDATE_COMPATIBILITY_ABI_MAX 255U
#define UPDATE_COMPATIBILITY_SERVICE_ABI_MIN 1U
#define UPDATE_COMPATIBILITY_SERVICE_ABI_MAX 255U
#define UPDATE_COMPATIBILITY_POLICY_MIN 1U
#define UPDATE_COMPATIBILITY_POLICY_MAX 0xFFFFFFFFU

enum update_compatibility_status {
    UPDATE_COMPATIBILITY_OK = 0,
    UPDATE_COMPATIBILITY_NULL = 1,
    UPDATE_COMPATIBILITY_BAD_STATE = 2,
    UPDATE_COMPATIBILITY_TOO_LONG = 3,
    UPDATE_COMPATIBILITY_BAD_VERSION = 4,
    UPDATE_COMPATIBILITY_MISSING_FIELD = 5,
    UPDATE_COMPATIBILITY_DUPLICATE_FIELD = 6,
    UPDATE_COMPATIBILITY_UNKNOWN_FIELD = 7,
    UPDATE_COMPATIBILITY_MALFORMED_FIELD = 8,
    UPDATE_COMPATIBILITY_OUT_OF_RANGE = 9,
    UPDATE_COMPATIBILITY_INCOMPATIBLE_RANGE = 10,
    UPDATE_COMPATIBILITY_INVALID_SOURCE = 11,
    UPDATE_COMPATIBILITY_INVALID_TARGET = 12,
    UPDATE_COMPATIBILITY_SOURCE_IMAGE_MISMATCH = 13,
    UPDATE_COMPATIBILITY_SOURCE_SCHEMA_MISMATCH = 14,
    UPDATE_COMPATIBILITY_SOURCE_ABI_MISMATCH = 15,
    UPDATE_COMPATIBILITY_SOURCE_SERVICE_MISMATCH = 16,
    UPDATE_COMPATIBILITY_SOURCE_POLICY_MISMATCH = 17,
    UPDATE_COMPATIBILITY_TARGET_IMAGE_MISMATCH = 18,
    UPDATE_COMPATIBILITY_TARGET_SCHEMA_MISMATCH = 19,
    UPDATE_COMPATIBILITY_TARGET_ABI_MISMATCH = 20,
    UPDATE_COMPATIBILITY_TARGET_SERVICE_MISMATCH = 21,
    UPDATE_COMPATIBILITY_TARGET_POLICY_MISMATCH = 22,
    UPDATE_COMPATIBILITY_SAME_IMAGE = 23,
};

struct update_compatibility_manifest {
    uint32_t compatibility_version;
    uint32_t source_image_id;
    uint32_t target_image_id;
    uint32_t source_schema_min;
    uint32_t source_schema_max;
    uint32_t target_schema_min;
    uint32_t target_schema_max;
    uint32_t source_kernel_abi_min;
    uint32_t source_kernel_abi_max;
    uint32_t target_kernel_abi_min;
    uint32_t target_kernel_abi_max;
    uint32_t source_service_abi_min;
    uint32_t source_service_abi_max;
    uint32_t target_service_abi_min;
    uint32_t target_service_abi_max;
    uint32_t source_policy_generation_min;
    uint32_t source_policy_generation_max;
    uint32_t target_policy_generation_min;
    uint32_t target_policy_generation_max;
};

struct update_compatibility_source {
    uint32_t image_id;
    uint32_t schema_version;
    uint32_t kernel_abi;
    uint32_t service_abi;
    uint32_t policy_generation;
};

struct update_compatibility_target {
    uint32_t image_id;
    uint32_t schema_version;
    uint32_t kernel_abi;
    uint32_t service_abi;
    uint32_t policy_generation;
};

struct update_compatibility_gate {
    uint32_t version;
    struct update_compatibility_manifest manifest;
    bool staged;
};

enum update_compatibility_status update_compatibility_manifest_parse(
    const char *input,
    size_t input_length,
    struct update_compatibility_manifest *manifest_out);
enum update_compatibility_status update_compatibility_manifest_validate(
    const struct update_compatibility_manifest *manifest);
enum update_compatibility_status update_compatibility_check(
    const struct update_compatibility_manifest *candidate,
    const struct update_compatibility_source *source,
    const struct update_compatibility_target *target);
enum update_compatibility_status update_compatibility_gate_init(
    struct update_compatibility_gate *gate);
enum update_compatibility_status update_compatibility_stage(
    struct update_compatibility_gate *gate,
    const struct update_compatibility_manifest *candidate,
    const struct update_compatibility_source *source,
    const struct update_compatibility_target *target);
const char *update_compatibility_status_string(
    enum update_compatibility_status status);

#endif
