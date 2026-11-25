#include "appliance_update_manifest.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef APPLIANCE_UPDATE_MANIFEST_HOST_TEST

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

void *kmalloc(size_t size)
{
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr)
{
    (void)ptr;
    ++free_attempts;
    return false;
}

static const char valid_text[] =
    "update_package_version=1\n"
    "release_id=HFOC-APPLIANCE-V1\n"
    "architecture=x86_64\n"
    "source_revision=0123456789abcdef0123456789abcdef01234567\n"
    "metadata_digest=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n"
    "payload_digest=abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789\n"
    "compatibility_version=1\n"
    "source_image_id=7\n"
    "target_image_id=8\n"
    "source_schema_min=1\n"
    "source_schema_max=1\n"
    "target_schema_min=1\n"
    "target_schema_max=2\n"
    "source_kernel_abi_min=1\n"
    "source_kernel_abi_max=3\n"
    "target_kernel_abi_min=1\n"
    "target_kernel_abi_max=3\n"
    "source_service_abi_min=1\n"
    "source_service_abi_max=1\n"
    "target_service_abi_min=1\n"
    "target_service_abi_max=1\n"
    "source_policy_generation_min=1\n"
    "source_policy_generation_max=1\n"
    "target_policy_generation_min=2\n"
    "target_policy_generation_max=2\n";

static struct update_compatibility_source make_source(void)
{
    struct update_compatibility_source source;

    memset(&source, 0, sizeof(source));
    source.image_id = 7U;
    source.schema_version = 1U;
    source.kernel_abi = 2U;
    source.service_abi = 1U;
    source.policy_generation = 1U;
    return source;
}

static struct update_compatibility_target make_target(void)
{
    struct update_compatibility_target target;

    memset(&target, 0, sizeof(target));
    target.image_id = 8U;
    target.schema_version = 1U;
    target.kernel_abi = 2U;
    target.service_abi = 1U;
    target.policy_generation = 2U;
    return target;
}

static void poison_manifest(struct appliance_update_manifest *manifest)
{
    memset(manifest, 0xA5, sizeof(*manifest));
}

static bool manifest_unchanged(
    const struct appliance_update_manifest *manifest)
{
    const unsigned char *bytes = (const unsigned char *)manifest;
    size_t index;

    for (index = 0U; index < sizeof(*manifest); ++index) {
        if (bytes[index] != 0xA5U) {
            return false;
        }
    }
    return true;
}

static void test_status_strings(void)
{
    static const struct {
        enum appliance_update_manifest_status status;
        const char *string;
    } statuses[] = {
        {APPLIANCE_UPDATE_MANIFEST_OK,
         "APPLIANCE_UPDATE_MANIFEST_OK"},
        {APPLIANCE_UPDATE_MANIFEST_NULL,
         "APPLIANCE_UPDATE_MANIFEST_NULL"},
        {APPLIANCE_UPDATE_MANIFEST_TOO_LONG,
         "APPLIANCE_UPDATE_MANIFEST_TOO_LONG"},
        {APPLIANCE_UPDATE_MANIFEST_BAD_VERSION,
         "APPLIANCE_UPDATE_MANIFEST_BAD_VERSION"},
        {APPLIANCE_UPDATE_MANIFEST_MISSING_FIELD,
         "APPLIANCE_UPDATE_MANIFEST_MISSING_FIELD"},
        {APPLIANCE_UPDATE_MANIFEST_DUPLICATE_FIELD,
         "APPLIANCE_UPDATE_MANIFEST_DUPLICATE_FIELD"},
        {APPLIANCE_UPDATE_MANIFEST_UNKNOWN_FIELD,
         "APPLIANCE_UPDATE_MANIFEST_UNKNOWN_FIELD"},
        {APPLIANCE_UPDATE_MANIFEST_MALFORMED_FIELD,
         "APPLIANCE_UPDATE_MANIFEST_MALFORMED_FIELD"},
        {APPLIANCE_UPDATE_MANIFEST_OUT_OF_RANGE,
         "APPLIANCE_UPDATE_MANIFEST_OUT_OF_RANGE"},
        {APPLIANCE_UPDATE_MANIFEST_BAD_RELEASE_ID,
         "APPLIANCE_UPDATE_MANIFEST_BAD_RELEASE_ID"},
        {APPLIANCE_UPDATE_MANIFEST_UNSUPPORTED_ARCH,
         "APPLIANCE_UPDATE_MANIFEST_UNSUPPORTED_ARCH"},
        {APPLIANCE_UPDATE_MANIFEST_BAD_REVISION,
         "APPLIANCE_UPDATE_MANIFEST_BAD_REVISION"},
        {APPLIANCE_UPDATE_MANIFEST_BAD_DIGEST,
         "APPLIANCE_UPDATE_MANIFEST_BAD_DIGEST"},
        {APPLIANCE_UPDATE_MANIFEST_INCOMPATIBLE_RANGE,
         "APPLIANCE_UPDATE_MANIFEST_INCOMPATIBLE_RANGE"},
        {APPLIANCE_UPDATE_MANIFEST_SAME_IMAGE,
         "APPLIANCE_UPDATE_MANIFEST_SAME_IMAGE"},
        {APPLIANCE_UPDATE_MANIFEST_INVALID_SOURCE,
         "APPLIANCE_UPDATE_MANIFEST_INVALID_SOURCE"},
        {APPLIANCE_UPDATE_MANIFEST_INVALID_TARGET,
         "APPLIANCE_UPDATE_MANIFEST_INVALID_TARGET"},
        {APPLIANCE_UPDATE_MANIFEST_COMPATIBILITY_DENIED,
         "APPLIANCE_UPDATE_MANIFEST_COMPATIBILITY_DENIED"},
        {APPLIANCE_UPDATE_MANIFEST_BAD_REQUEST,
         "APPLIANCE_UPDATE_MANIFEST_BAD_REQUEST"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = appliance_update_manifest_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "appliance update manifest status string is exact");
    }
    CHECK(appliance_update_manifest_status_string(
              (enum appliance_update_manifest_status)19U) == NULL,
          "appliance update manifest undefined status is rejected");
}

static void test_null_and_too_long(void)
{
    struct appliance_update_manifest manifest;
    char too_long[APPLIANCE_UPDATE_MANIFEST_INPUT_MAX + 1U];
    enum update_compatibility_status compatibility_status =
        UPDATE_COMPATIBILITY_OK;

    memset(&manifest, 0, sizeof(manifest));
    memset(too_long, 'x', sizeof(too_long));
    CHECK(appliance_update_manifest_parse(
              NULL, 0U, &manifest) == APPLIANCE_UPDATE_MANIFEST_NULL,
          "appliance update manifest parse rejects null input");
    CHECK(appliance_update_manifest_parse(
              "", 0U, NULL) == APPLIANCE_UPDATE_MANIFEST_NULL,
          "appliance update manifest parse rejects null output");
    CHECK(appliance_update_manifest_parse(
              NULL, 0U, NULL) == APPLIANCE_UPDATE_MANIFEST_NULL,
          "appliance update manifest parse rejects null input and output");
    CHECK(appliance_update_manifest_parse(
              too_long, sizeof(too_long), &manifest) ==
              APPLIANCE_UPDATE_MANIFEST_TOO_LONG,
          "appliance update manifest parse rejects over-limit input");
    CHECK(appliance_update_manifest_check(
              NULL, NULL, NULL, &compatibility_status) ==
              APPLIANCE_UPDATE_MANIFEST_NULL,
          "appliance update manifest check rejects null manifest");
    CHECK(appliance_update_manifest_check(
              NULL, NULL, NULL, NULL) == APPLIANCE_UPDATE_MANIFEST_NULL,
          "appliance update manifest check rejects null status output");
}

static void test_valid_parse_and_check(void)
{
    struct appliance_update_manifest manifest;
    struct update_compatibility_source source = make_source();
    struct update_compatibility_target target = make_target();
    enum update_compatibility_status compatibility_status =
        UPDATE_COMPATIBILITY_BAD_STATE;

    poison_manifest(&manifest);
    CHECK(appliance_update_manifest_parse(
              valid_text, sizeof(valid_text) - 1U, &manifest) ==
              APPLIANCE_UPDATE_MANIFEST_OK,
          "valid appliance update manifest parses successfully");
    CHECK(manifest.update_package_version ==
              APPLIANCE_UPDATE_MANIFEST_VERSION &&
              strcmp(manifest.release_id, "HFOC-APPLIANCE-V1") == 0 &&
              strcmp(manifest.architecture, "x86_64") == 0 &&
              strcmp(manifest.source_revision,
                     "0123456789abcdef0123456789abcdef01234567") == 0 &&
              strcmp(manifest.metadata_digest,
                     "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef") == 0,
          "valid manifest publishes exact identity and digest fields");
    CHECK(manifest.compatibility.source_image_id == 7U &&
              manifest.compatibility.target_image_id == 8U &&
              manifest.compatibility.source_schema_min == 1U,
          "valid manifest publishes exact compatibility fields");
    CHECK(appliance_update_manifest_check(
              &manifest, &source, &target, &compatibility_status) ==
              APPLIANCE_UPDATE_MANIFEST_OK &&
              compatibility_status == UPDATE_COMPATIBILITY_OK,
          "compatible source and target pass update manifest check");
}

static void test_parse_failures_never_publish(void)
{
    static const struct {
        const char *text;
        enum appliance_update_manifest_status expected;
    } cases[] = {
        {"update_package_version=2\nrelease_id=HFOC-APPLIANCE-V1\n"
         "architecture=x86_64\nsource_revision=0000000000000000000000000000000000000000\n"
         "metadata_digest=0000000000000000000000000000000000000000000000000000000000000000\n"
         "payload_digest=0000000000000000000000000000000000000000000000000000000000000000\n"
         "compatibility_version=1\nsource_image_id=7\ntarget_image_id=8\n"
         "source_schema_min=1\nsource_schema_max=1\ntarget_schema_min=1\n"
         "target_schema_max=2\nsource_kernel_abi_min=1\nsource_kernel_abi_max=3\n"
         "target_kernel_abi_min=1\ntarget_kernel_abi_max=3\n"
         "source_service_abi_min=1\nsource_service_abi_max=1\n"
         "target_service_abi_min=1\ntarget_service_abi_max=1\n"
         "source_policy_generation_min=1\nsource_policy_generation_max=1\n"
         "target_policy_generation_min=2\ntarget_policy_generation_max=2\n",
         APPLIANCE_UPDATE_MANIFEST_BAD_VERSION},
        {"update_package_version=1\nrelease_id=OTHER\n"
         "architecture=x86_64\nsource_revision=0000000000000000000000000000000000000000\n"
         "metadata_digest=0000000000000000000000000000000000000000000000000000000000000000\n"
         "payload_digest=0000000000000000000000000000000000000000000000000000000000000000\n"
         "compatibility_version=1\nsource_image_id=7\ntarget_image_id=8\n"
         "source_schema_min=1\nsource_schema_max=1\ntarget_schema_min=1\n"
         "target_schema_max=2\nsource_kernel_abi_min=1\nsource_kernel_abi_max=3\n"
         "target_kernel_abi_min=1\ntarget_kernel_abi_max=3\n"
         "source_service_abi_min=1\nsource_service_abi_max=1\n"
         "target_service_abi_min=1\ntarget_service_abi_max=1\n"
         "source_policy_generation_min=1\nsource_policy_generation_max=1\n"
         "target_policy_generation_min=2\ntarget_policy_generation_max=2\n",
         APPLIANCE_UPDATE_MANIFEST_BAD_RELEASE_ID},
        {"update_package_version=1\nrelease_id=HFOC-APPLIANCE-V1\n"
         "architecture=arm64\nsource_revision=0000000000000000000000000000000000000000\n"
         "metadata_digest=0000000000000000000000000000000000000000000000000000000000000000\n"
         "payload_digest=0000000000000000000000000000000000000000000000000000000000000000\n"
         "compatibility_version=1\nsource_image_id=7\ntarget_image_id=8\n"
         "source_schema_min=1\nsource_schema_max=1\ntarget_schema_min=1\n"
         "target_schema_max=2\nsource_kernel_abi_min=1\nsource_kernel_abi_max=3\n"
         "target_kernel_abi_min=1\ntarget_kernel_abi_max=3\n"
         "source_service_abi_min=1\nsource_service_abi_max=1\n"
         "target_service_abi_min=1\ntarget_service_abi_max=1\n"
         "source_policy_generation_min=1\nsource_policy_generation_max=1\n"
         "target_policy_generation_min=2\ntarget_policy_generation_max=2\n",
         APPLIANCE_UPDATE_MANIFEST_UNSUPPORTED_ARCH},
        {"update_package_version=1\nrelease_id=HFOC-APPLIANCE-V1\n"
         "architecture=x86_64\nsource_revision=SHORT\n"
         "metadata_digest=0000000000000000000000000000000000000000000000000000000000000000\n"
         "payload_digest=0000000000000000000000000000000000000000000000000000000000000000\n"
         "compatibility_version=1\nsource_image_id=7\ntarget_image_id=8\n"
         "source_schema_min=1\nsource_schema_max=1\ntarget_schema_min=1\n"
         "target_schema_max=2\nsource_kernel_abi_min=1\nsource_kernel_abi_max=3\n"
         "target_kernel_abi_min=1\ntarget_kernel_abi_max=3\n"
         "source_service_abi_min=1\nsource_service_abi_max=1\n"
         "target_service_abi_min=1\ntarget_service_abi_max=1\n"
         "source_policy_generation_min=1\nsource_policy_generation_max=1\n"
         "target_policy_generation_min=2\ntarget_policy_generation_max=2\n",
         APPLIANCE_UPDATE_MANIFEST_BAD_REVISION},
        {"update_package_version=1\nrelease_id=HFOC-APPLIANCE-V1\n"
         "architecture=x86_64\nsource_revision=0000000000000000000000000000000000000000\n"
         "metadata_digest=BAD\n"
         "payload_digest=0000000000000000000000000000000000000000000000000000000000000000\n"
         "compatibility_version=1\nsource_image_id=7\ntarget_image_id=8\n"
         "source_schema_min=1\nsource_schema_max=1\ntarget_schema_min=1\n"
         "target_schema_max=2\nsource_kernel_abi_min=1\nsource_kernel_abi_max=3\n"
         "target_kernel_abi_min=1\ntarget_kernel_abi_max=3\n"
         "source_service_abi_min=1\nsource_service_abi_max=1\n"
         "target_service_abi_min=1\ntarget_service_abi_max=1\n"
         "source_policy_generation_min=1\nsource_policy_generation_max=1\n"
         "target_policy_generation_min=2\ntarget_policy_generation_max=2\n",
         APPLIANCE_UPDATE_MANIFEST_BAD_DIGEST},
        {"update_package_version=1\nrelease_id=HFOC-APPLIANCE-V1\n"
         "architecture=x86_64\nsource_revision=0000000000000000000000000000000000000000\n"
         "metadata_digest=0000000000000000000000000000000000000000000000000000000000000000\n"
         "payload_digest=0000000000000000000000000000000000000000000000000000000000000000\n"
         "compatibility_version=1\nsource_image_id=7\ntarget_image_id=8\n"
         "source_schema_min=1\nsource_schema_max=1\ntarget_schema_min=1\n"
         "target_schema_max=2\nsource_kernel_abi_min=1\nsource_kernel_abi_max=3\n"
         "target_kernel_abi_min=1\ntarget_kernel_abi_max=3\n"
         "source_service_abi_min=1\nsource_service_abi_max=1\n"
         "target_service_abi_min=1\ntarget_service_abi_max=1\n"
         "source_policy_generation_min=1\nsource_policy_generation_max=1\n"
         "target_policy_generation_min=2\ntarget_policy_generation_max=2\n"
         "unknown=1\n",
         APPLIANCE_UPDATE_MANIFEST_UNKNOWN_FIELD},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        struct appliance_update_manifest output;

        poison_manifest(&output);
        CHECK(appliance_update_manifest_parse(
                  cases[index].text,
                  strlen(cases[index].text),
                  &output) == cases[index].expected &&
                  manifest_unchanged(&output),
              "malformed appliance update manifest never publishes output");
    }
}

static void test_duplicate_and_missing(void)
{
    char duplicate[sizeof(valid_text) + 64U];
    char missing[sizeof(valid_text)];
    struct appliance_update_manifest output;

    memcpy(missing, valid_text, sizeof(valid_text));
    {
        char *position = strstr(missing, "source_image_id=7\n");

        if (position != NULL) {
            memmove(position, position + strlen("source_image_id=7\n"),
                    strlen(position + strlen("source_image_id=7\n")) + 1U);
        }
    }
    snprintf(duplicate, sizeof(duplicate), "%s%s", valid_text,
             "source_image_id=7\n");
    poison_manifest(&output);
    CHECK(appliance_update_manifest_parse(
              missing, strlen(missing), &output) ==
              APPLIANCE_UPDATE_MANIFEST_MISSING_FIELD &&
              manifest_unchanged(&output),
          "missing compatibility field is rejected without publication");
    poison_manifest(&output);
    CHECK(appliance_update_manifest_parse(
              duplicate, strlen(duplicate), &output) ==
              APPLIANCE_UPDATE_MANIFEST_DUPLICATE_FIELD &&
              manifest_unchanged(&output),
          "duplicate compatibility field is rejected without publication");
}

static void replace_text(char *text,
                         size_t capacity,
                         const char *old_value,
                         const char *new_value)
{
    char *position;

    (void)capacity;
    position = strstr(text, old_value);
    CHECK(position != NULL, "test replacement source is present");
    if (position != NULL) {
        CHECK(strlen(old_value) == strlen(new_value),
              "test replacement lengths match");
        memcpy(position, new_value, strlen(new_value));
    }
}

static void test_semantic_parse_failures(void)
{
    char text[sizeof(valid_text)];
    struct appliance_update_manifest output;

    memcpy(text, valid_text, sizeof(valid_text));
    replace_text(text, sizeof(text), "compatibility_version=1",
                 "compatibility_version=2");
    poison_manifest(&output);
    CHECK(appliance_update_manifest_parse(
              text, strlen(text), &output) ==
              APPLIANCE_UPDATE_MANIFEST_BAD_VERSION &&
              manifest_unchanged(&output),
          "unsupported compatibility version is rejected without publication");

    memcpy(text, valid_text, sizeof(valid_text));
    replace_text(text, sizeof(text), "source_image_id=7",
                 "source_image_id=0");
    poison_manifest(&output);
    CHECK(appliance_update_manifest_parse(
              text, strlen(text), &output) ==
              APPLIANCE_UPDATE_MANIFEST_OUT_OF_RANGE &&
              manifest_unchanged(&output),
          "out-of-range compatibility image is rejected without publication");

    memcpy(text, valid_text, sizeof(valid_text));
    replace_text(text, sizeof(text), "target_image_id=8",
                 "target_image_id=7");
    poison_manifest(&output);
    CHECK(appliance_update_manifest_parse(
              text, strlen(text), &output) ==
              APPLIANCE_UPDATE_MANIFEST_SAME_IMAGE &&
              manifest_unchanged(&output),
          "same-image update is rejected without publication");

    memcpy(text, valid_text, sizeof(valid_text));
    replace_text(text, sizeof(text), "source_schema_min=1",
                 "source_schema_min=2");
    poison_manifest(&output);
    CHECK(appliance_update_manifest_parse(
              text, strlen(text), &output) ==
              APPLIANCE_UPDATE_MANIFEST_INCOMPATIBLE_RANGE &&
              manifest_unchanged(&output),
          "inverted compatibility range is rejected without publication");

    {
        char malformed[sizeof(valid_text) + 32U];

        memcpy(malformed, valid_text, sizeof(valid_text));
        snprintf(malformed + strlen(malformed),
                 sizeof(malformed) - strlen(malformed),
                 "malformed_no_equals\n");
        poison_manifest(&output);
        CHECK(appliance_update_manifest_parse(
                  malformed, strlen(malformed), &output) ==
                  APPLIANCE_UPDATE_MANIFEST_MALFORMED_FIELD &&
                  manifest_unchanged(&output),
              "malformed manifest line is rejected without publication");
    }
}

static void test_compatibility_denials(void)
{
    struct appliance_update_manifest manifest;
    struct update_compatibility_source source = make_source();
    struct update_compatibility_target target = make_target();
    enum update_compatibility_status compatibility_status =
        UPDATE_COMPATIBILITY_OK;

    CHECK(appliance_update_manifest_parse(
              valid_text, sizeof(valid_text) - 1U, &manifest) ==
              APPLIANCE_UPDATE_MANIFEST_OK,
          "denial fixture parses valid manifest");
    source.image_id = 6U;
    CHECK(appliance_update_manifest_check(
              &manifest, &source, &target, &compatibility_status) ==
              APPLIANCE_UPDATE_MANIFEST_COMPATIBILITY_DENIED &&
              compatibility_status ==
                  UPDATE_COMPATIBILITY_SOURCE_IMAGE_MISMATCH,
          "source image mismatch is denied with exact compatibility status");
    source = make_source();
    target.image_id = 9U;
    CHECK(appliance_update_manifest_check(
              &manifest, &source, &target, &compatibility_status) ==
              APPLIANCE_UPDATE_MANIFEST_COMPATIBILITY_DENIED &&
              compatibility_status ==
                  UPDATE_COMPATIBILITY_TARGET_IMAGE_MISMATCH,
          "target image mismatch is denied with exact compatibility status");
    source = make_source();
    source.kernel_abi = 255U;
    CHECK(appliance_update_manifest_check(
              &manifest, &source, &target, &compatibility_status) ==
              APPLIANCE_UPDATE_MANIFEST_COMPATIBILITY_DENIED &&
              compatibility_status ==
                  UPDATE_COMPATIBILITY_SOURCE_ABI_MISMATCH,
          "source ABI mismatch is denied with exact compatibility status");
    source.schema_version = 0U;
    CHECK(appliance_update_manifest_check(
              &manifest, &source, &target, &compatibility_status) ==
              APPLIANCE_UPDATE_MANIFEST_INVALID_SOURCE &&
              compatibility_status == UPDATE_COMPATIBILITY_INVALID_SOURCE,
          "invalid source is rejected with exact compatibility status");
}

int main(void)
{
    test_status_strings();
    test_null_and_too_long();
    test_valid_parse_and_check();
    test_parse_failures_never_publish();
    test_duplicate_and_missing();
    test_semantic_parse_failures();
    test_compatibility_denials();
    if (failures != 0) {
        fprintf(stderr, "appliance update manifest host tests: %d failures\n",
                failures);
        return 1;
    }
    printf("appliance update manifest host tests: OK\n");
    return 0;
}

#endif
