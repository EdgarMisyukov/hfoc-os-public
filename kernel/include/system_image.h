#ifndef HFOC_SYSTEM_IMAGE_H
#define HFOC_SYSTEM_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#define SYSTEM_IMAGE_MANIFEST_VERSION 1U
#define SYSTEM_IMAGE_INPUT_MAX 8192U
#define SYSTEM_IMAGE_LINE_MAX 256U
#define SYSTEM_IMAGE_KEY_MAX 32U
#define SYSTEM_IMAGE_ARCH_MAX 16U
#define SYSTEM_IMAGE_ARCH_VALUE "x86_64"
#define SYSTEM_IMAGE_COMPONENT_NAME_MAX 32U
#define SYSTEM_IMAGE_COMPONENT_VERSION_MAX 32U
#define SYSTEM_IMAGE_HASH_HEX_LEN 64U
#define SYSTEM_IMAGE_COMPONENT_MAX 16U
#define SYSTEM_IMAGE_IMAGE_ID_MIN 1U
#define SYSTEM_IMAGE_IMAGE_ID_MAX 4096U
#define SYSTEM_IMAGE_KERNEL_ABI_MIN 1U
#define SYSTEM_IMAGE_KERNEL_ABI_MAX 255U

enum system_image_status {
    SYSTEM_IMAGE_OK = 0,
    SYSTEM_IMAGE_NULL = 1,
    SYSTEM_IMAGE_TOO_LONG = 2,
    SYSTEM_IMAGE_BAD_VERSION = 3,
    SYSTEM_IMAGE_MISSING_FIELD = 4,
    SYSTEM_IMAGE_DUPLICATE_FIELD = 5,
    SYSTEM_IMAGE_UNKNOWN_FIELD = 6,
    SYSTEM_IMAGE_MALFORMED_FIELD = 7,
    SYSTEM_IMAGE_OUT_OF_RANGE = 8,
    SYSTEM_IMAGE_EMPTY_COMPONENT_LIST = 9,
    SYSTEM_IMAGE_TOO_MANY_COMPONENTS = 10,
    SYSTEM_IMAGE_DUPLICATE_COMPONENT = 11,
    SYSTEM_IMAGE_UNSUPPORTED_ARCH = 12,
    SYSTEM_IMAGE_INCOMPATIBLE_ABI = 13,
};

struct system_image_component {
    char name[SYSTEM_IMAGE_COMPONENT_NAME_MAX + 1U];
    char version[SYSTEM_IMAGE_COMPONENT_VERSION_MAX + 1U];
    char hash[SYSTEM_IMAGE_HASH_HEX_LEN + 1U];
};

struct system_image_manifest {
    uint32_t manifest_version;
    uint32_t image_id;
    uint32_t min_kernel_abi;
    uint32_t max_kernel_abi;
    char arch[SYSTEM_IMAGE_ARCH_MAX + 1U];
    struct system_image_component components[SYSTEM_IMAGE_COMPONENT_MAX];
    uint32_t component_count;
};

enum system_image_status system_image_manifest_parse(
    const char *input,
    size_t input_length,
    struct system_image_manifest *manifest_out);
const char *system_image_status_string(enum system_image_status status);

#endif
