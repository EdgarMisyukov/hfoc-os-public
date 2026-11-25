#ifndef HFOC_IMAGE_INTEGRITY_H
#define HFOC_IMAGE_INTEGRITY_H

#include "system_image.h"

#include <stdbool.h>
#include <stddef.h>

#define IMAGE_INTEGRITY_PAYLOAD_MAX 16U
#define IMAGE_INTEGRITY_NAME_MAX 32U
#define IMAGE_INTEGRITY_DATA_MAX 65536U

enum image_integrity_status {
    IMAGE_INTEGRITY_OK = 0,
    IMAGE_INTEGRITY_NULL = 1,
    IMAGE_INTEGRITY_BAD_STATE = 2,
    IMAGE_INTEGRITY_TOO_MANY_PAYLOADS = 3,
    IMAGE_INTEGRITY_BAD_MANIFEST = 4,
    IMAGE_INTEGRITY_DUPLICATE_PAYLOAD = 5,
    IMAGE_INTEGRITY_MISSING_COMPONENT = 6,
    IMAGE_INTEGRITY_UNKNOWN_COMPONENT = 7,
    IMAGE_INTEGRITY_PAYLOAD_TOO_LONG = 8,
    IMAGE_INTEGRITY_HASH_MISMATCH = 9,
    IMAGE_INTEGRITY_BAD_PAYLOAD = 10,
};

struct image_integrity_payload {
    const char *name;
    const void *data;
    size_t data_length;
};

struct image_integrity_gate {
    struct system_image_manifest manifest;
    bool published;
};

enum image_integrity_status image_integrity_verify(
    struct image_integrity_gate *gate,
    const struct system_image_manifest *manifest,
    const struct image_integrity_payload *payloads,
    size_t payload_count);
bool image_integrity_is_published(
    const struct image_integrity_gate *gate);
const char *image_integrity_status_string(
    enum image_integrity_status status);

#endif
