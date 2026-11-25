#ifndef HFOC_IMAGE_POLICY_H
#define HFOC_IMAGE_POLICY_H

#include "image_integrity.h"
#include "system_image.h"
#include "verification_policy.h"

#include <stdbool.h>
#include <stdint.h>

#define IMAGE_POLICY_VERSION 1U

enum image_policy_status {
    IMAGE_POLICY_OK = 0,
    IMAGE_POLICY_NULL = 1,
    IMAGE_POLICY_BAD_STATE = 2,
    IMAGE_POLICY_NO_POLICY = 3,
    IMAGE_POLICY_NO_IMAGE = 4,
    IMAGE_POLICY_INVALID_IMAGE = 5,
    IMAGE_POLICY_NOT_PUBLISHED = 6,
    IMAGE_POLICY_POLICY_MISMATCH = 7,
};

struct image_policy_binding {
    uint32_t version;
    uint32_t policy_generation;
    struct system_image_manifest manifest;
    bool published;
};

enum image_policy_status image_policy_bind(
    struct image_policy_binding *binding,
    const struct verification_policy *policy,
    const struct image_integrity_gate *gate);
enum image_policy_status image_policy_activate(
    const struct image_policy_binding *binding,
    const struct verification_policy *policy,
    struct system_image_manifest *manifest_out);
enum image_policy_status image_policy_reset(
    struct image_policy_binding *binding);
bool image_policy_is_published(
    const struct image_policy_binding *binding);
const struct system_image_manifest *image_policy_manifest(
    const struct image_policy_binding *binding);
const char *image_policy_status_string(
    enum image_policy_status status);

#endif
