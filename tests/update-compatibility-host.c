#include "update_compatibility.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_UPDATE_COMPATIBILITY_HOST_TEST

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

static const char valid_text[] =
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

void *kmalloc(size_t size) {
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr) {
    (void)ptr;
    ++free_attempts;
    return false;
}

static struct update_compatibility_manifest make_manifest(void) {
    struct update_compatibility_manifest manifest;

    memset(&manifest, 0, sizeof(manifest));
    manifest.compatibility_version = UPDATE_COMPATIBILITY_VERSION;
    manifest.source_image_id = 7U;
    manifest.target_image_id = 8U;
    manifest.source_schema_min = 1U;
    manifest.source_schema_max = 1U;
    manifest.target_schema_min = 1U;
    manifest.target_schema_max = 2U;
    manifest.source_kernel_abi_min = 1U;
    manifest.source_kernel_abi_max = 3U;
    manifest.target_kernel_abi_min = 1U;
    manifest.target_kernel_abi_max = 3U;
    manifest.source_service_abi_min = 1U;
    manifest.source_service_abi_max = 1U;
    manifest.target_service_abi_min = 1U;
    manifest.target_service_abi_max = 1U;
    manifest.source_policy_generation_min = 1U;
    manifest.source_policy_generation_max = 1U;
    manifest.target_policy_generation_min = 2U;
    manifest.target_policy_generation_max = 2U;
    return manifest;
}

static struct update_compatibility_source make_source(void) {
    struct update_compatibility_source source;

    memset(&source, 0, sizeof(source));
    source.image_id = 7U;
    source.schema_version = 1U;
    source.kernel_abi = 2U;
    source.service_abi = 1U;
    source.policy_generation = 1U;
    return source;
}

static struct update_compatibility_target make_target(void) {
    struct update_compatibility_target target;

    memset(&target, 0, sizeof(target));
    target.image_id = 8U;
    target.schema_version = 1U;
    target.kernel_abi = 2U;
    target.service_abi = 1U;
    target.policy_generation = 2U;
    return target;
}

static void poison_manifest(struct update_compatibility_manifest *manifest) {
    memset(manifest, 0xA5, sizeof(*manifest));
}

static bool manifest_unchanged(
    const struct update_compatibility_manifest *manifest)
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

static void poison_gate(struct update_compatibility_gate *gate) {
    memset(gate, 0xA5, sizeof(*gate));
}

static bool gate_unchanged(const struct update_compatibility_gate *gate,
                           const struct update_compatibility_gate *original)
{
    return memcmp(gate, original, sizeof(*gate)) == 0;
}

static void test_status_strings(void) {
    static const struct {
        enum update_compatibility_status status;
        const char *string;
    } statuses[] = {
        {UPDATE_COMPATIBILITY_OK, "UPDATE_COMPATIBILITY_OK"},
        {UPDATE_COMPATIBILITY_NULL, "UPDATE_COMPATIBILITY_NULL"},
        {UPDATE_COMPATIBILITY_BAD_STATE, "UPDATE_COMPATIBILITY_BAD_STATE"},
        {UPDATE_COMPATIBILITY_TOO_LONG, "UPDATE_COMPATIBILITY_TOO_LONG"},
        {UPDATE_COMPATIBILITY_BAD_VERSION,
         "UPDATE_COMPATIBILITY_BAD_VERSION"},
        {UPDATE_COMPATIBILITY_MISSING_FIELD,
         "UPDATE_COMPATIBILITY_MISSING_FIELD"},
        {UPDATE_COMPATIBILITY_DUPLICATE_FIELD,
         "UPDATE_COMPATIBILITY_DUPLICATE_FIELD"},
        {UPDATE_COMPATIBILITY_UNKNOWN_FIELD,
         "UPDATE_COMPATIBILITY_UNKNOWN_FIELD"},
        {UPDATE_COMPATIBILITY_MALFORMED_FIELD,
         "UPDATE_COMPATIBILITY_MALFORMED_FIELD"},
        {UPDATE_COMPATIBILITY_OUT_OF_RANGE,
         "UPDATE_COMPATIBILITY_OUT_OF_RANGE"},
        {UPDATE_COMPATIBILITY_INCOMPATIBLE_RANGE,
         "UPDATE_COMPATIBILITY_INCOMPATIBLE_RANGE"},
        {UPDATE_COMPATIBILITY_INVALID_SOURCE,
         "UPDATE_COMPATIBILITY_INVALID_SOURCE"},
        {UPDATE_COMPATIBILITY_INVALID_TARGET,
         "UPDATE_COMPATIBILITY_INVALID_TARGET"},
        {UPDATE_COMPATIBILITY_SOURCE_IMAGE_MISMATCH,
         "UPDATE_COMPATIBILITY_SOURCE_IMAGE_MISMATCH"},
        {UPDATE_COMPATIBILITY_SOURCE_SCHEMA_MISMATCH,
         "UPDATE_COMPATIBILITY_SOURCE_SCHEMA_MISMATCH"},
        {UPDATE_COMPATIBILITY_SOURCE_ABI_MISMATCH,
         "UPDATE_COMPATIBILITY_SOURCE_ABI_MISMATCH"},
        {UPDATE_COMPATIBILITY_SOURCE_SERVICE_MISMATCH,
         "UPDATE_COMPATIBILITY_SOURCE_SERVICE_MISMATCH"},
        {UPDATE_COMPATIBILITY_SOURCE_POLICY_MISMATCH,
         "UPDATE_COMPATIBILITY_SOURCE_POLICY_MISMATCH"},
        {UPDATE_COMPATIBILITY_TARGET_IMAGE_MISMATCH,
         "UPDATE_COMPATIBILITY_TARGET_IMAGE_MISMATCH"},
        {UPDATE_COMPATIBILITY_TARGET_SCHEMA_MISMATCH,
         "UPDATE_COMPATIBILITY_TARGET_SCHEMA_MISMATCH"},
        {UPDATE_COMPATIBILITY_TARGET_ABI_MISMATCH,
         "UPDATE_COMPATIBILITY_TARGET_ABI_MISMATCH"},
        {UPDATE_COMPATIBILITY_TARGET_SERVICE_MISMATCH,
         "UPDATE_COMPATIBILITY_TARGET_SERVICE_MISMATCH"},
        {UPDATE_COMPATIBILITY_TARGET_POLICY_MISMATCH,
         "UPDATE_COMPATIBILITY_TARGET_POLICY_MISMATCH"},
        {UPDATE_COMPATIBILITY_SAME_IMAGE,
         "UPDATE_COMPATIBILITY_SAME_IMAGE"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = update_compatibility_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "update compatibility status string is exact");
    }
    CHECK(update_compatibility_status_string(
              (enum update_compatibility_status)24U) == NULL,
          "update compatibility undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "update compatibility status handling performs no allocation");
}

static void test_null_and_too_long(void) {
    struct update_compatibility_manifest manifest;
    struct update_compatibility_gate gate;
    char too_long[UPDATE_COMPATIBILITY_INPUT_MAX + 1U];

    memset(&manifest, 0, sizeof(manifest));
    memset(&gate, 0, sizeof(gate));
    CHECK(update_compatibility_manifest_parse(NULL, 0U, &manifest) ==
              UPDATE_COMPATIBILITY_NULL,
          "update compatibility parse rejects null input");
    CHECK(update_compatibility_manifest_parse("", 0U, NULL) ==
              UPDATE_COMPATIBILITY_NULL,
          "update compatibility parse rejects null output");
    CHECK(update_compatibility_manifest_parse(NULL, 0U, NULL) ==
              UPDATE_COMPATIBILITY_NULL,
          "update compatibility parse rejects null input and output");
    CHECK(update_compatibility_manifest_validate(NULL) ==
              UPDATE_COMPATIBILITY_NULL,
          "update compatibility validate rejects null manifest");
    CHECK(update_compatibility_check(NULL, NULL, NULL) ==
              UPDATE_COMPATIBILITY_NULL,
          "update compatibility check rejects null inputs");
    CHECK(update_compatibility_gate_init(NULL) ==
              UPDATE_COMPATIBILITY_NULL,
          "update compatibility gate init rejects null gate");
    CHECK(update_compatibility_stage(NULL, NULL, NULL, NULL) ==
              UPDATE_COMPATIBILITY_NULL,
          "update compatibility stage rejects null inputs");

    memset(too_long, 'x', sizeof(too_long));
    CHECK(update_compatibility_manifest_parse(
              too_long, sizeof(too_long), &manifest) ==
              UPDATE_COMPATIBILITY_TOO_LONG,
          "update compatibility parse rejects input over maximum length");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "update compatibility validation performs no allocation");
}

static void test_valid_parse_check_and_stage(void) {
    struct update_compatibility_manifest manifest;
    struct update_compatibility_source source = make_source();
    struct update_compatibility_target target = make_target();
    struct update_compatibility_gate gate;
    struct update_compatibility_manifest expected = make_manifest();

    poison_manifest(&manifest);
    CHECK(update_compatibility_manifest_parse(
              valid_text, sizeof(valid_text) - 1U, &manifest) ==
              UPDATE_COMPATIBILITY_OK,
          "valid update compatibility manifest parses successfully");
    CHECK(memcmp(&manifest, &expected, sizeof(manifest)) == 0,
          "valid manifest publishes every exact field");
    CHECK(update_compatibility_manifest_validate(&manifest) ==
              UPDATE_COMPATIBILITY_OK,
          "parsed manifest validates successfully");
    CHECK(update_compatibility_check(&manifest, &source, &target) ==
              UPDATE_COMPATIBILITY_OK,
          "compatible source and target pass compatibility check");

    CHECK(update_compatibility_gate_init(&gate) ==
              UPDATE_COMPATIBILITY_OK &&
              gate.version == UPDATE_COMPATIBILITY_VERSION &&
              !gate.staged,
          "gate init publishes an empty unstaged gate");
    CHECK(update_compatibility_stage(
              &gate, &manifest, &source, &target) ==
              UPDATE_COMPATIBILITY_OK &&
              gate.staged &&
              gate.version == UPDATE_COMPATIBILITY_VERSION &&
              memcmp(&gate.manifest, &manifest, sizeof(manifest)) == 0,
          "compatible candidate enters compatibility staging with exact manifest");
    CHECK(update_compatibility_stage(
              &gate, &manifest, &source, &target) ==
              UPDATE_COMPATIBILITY_BAD_STATE,
          "second stage on published gate is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "valid update compatibility flow performs no allocation");
}

static void test_parse_failures_never_publish(void) {
    static const char *const texts[] = {
        "compatibility_version=1\ncompatibility_version=1\n"
        "source_image_id=7\ntarget_image_id=8\n"
        "source_schema_min=1\nsource_schema_max=1\n"
        "target_schema_min=1\ntarget_schema_max=2\n"
        "source_kernel_abi_min=1\nsource_kernel_abi_max=3\n"
        "target_kernel_abi_min=1\ntarget_kernel_abi_max=3\n"
        "source_service_abi_min=1\nsource_service_abi_max=1\n"
        "target_service_abi_min=1\ntarget_service_abi_max=1\n"
        "source_policy_generation_min=1\nsource_policy_generation_max=1\n"
        "target_policy_generation_min=2\ntarget_policy_generation_max=2\n",
        "compatibility_version=1\n"
        "source_image_id=7\ntarget_image_id=8\n"
        "source_schema_min=1\nsource_schema_max=1\n"
        "target_schema_min=1\ntarget_schema_max=2\n"
        "source_kernel_abi_min=1\nsource_kernel_abi_max=3\n"
        "target_kernel_abi_min=1\ntarget_kernel_abi_max=3\n"
        "source_service_abi_min=1\nsource_service_abi_max=1\n"
        "target_service_abi_min=1\ntarget_service_abi_max=1\n"
        "source_policy_generation_min=1\nsource_policy_generation_max=1\n",
        "compatibility_version=1\n"
        "source_image_id=7\ntarget_image_id=8\nunknown=1\n"
        "source_schema_min=1\nsource_schema_max=1\n"
        "target_schema_min=1\ntarget_schema_max=2\n"
        "source_kernel_abi_min=1\nsource_kernel_abi_max=3\n"
        "target_kernel_abi_min=1\ntarget_kernel_abi_max=3\n"
        "source_service_abi_min=1\nsource_service_abi_max=1\n"
        "target_service_abi_min=1\ntarget_service_abi_max=1\n"
        "source_policy_generation_min=1\nsource_policy_generation_max=1\n"
        "target_policy_generation_min=2\ntarget_policy_generation_max=2\n",
        "compatibility_version=1\n"
        "source_image_id=7\ntarget_image_id=8\n"
        "source_schema_min=1\nsource_schema_max=1\n"
        "target_schema_min=1\ntarget_schema_max=2\n"
        "source_kernel_abi_min=1\nsource_kernel_abi_max=3\n"
        "target_kernel_abi_min=1\ntarget_kernel_abi_max=3\n"
        "source_service_abi_min=1\nsource_service_abi_max=1\n"
        "target_service_abi_min=1\ntarget_service_abi_max=1\n"
        "source_policy_generation_min=1\nsource_policy_generation_max=1\n"
        "target_policy_generation_min=2\ntarget_policy_generation_max=2\n"
        "extra=1\n",
    };
    struct update_compatibility_manifest output;
    size_t index;

    for (index = 0U;
         index < sizeof(texts) / sizeof(texts[0]);
         ++index) {
        enum update_compatibility_status status;

        poison_manifest(&output);
        status = update_compatibility_manifest_parse(
            texts[index], strlen(texts[index]), &output);
        CHECK(status != UPDATE_COMPATIBILITY_OK &&
                  manifest_unchanged(&output),
              "malformed update compatibility input never publishes output");
    }
}

static void test_explicit_parse_failures(void) {
    static const char *const texts[] = {
        "compatibility_version=2\n"
        "source_image_id=7\ntarget_image_id=8\n"
        "source_schema_min=1\nsource_schema_max=1\n"
        "target_schema_min=1\ntarget_schema_max=2\n"
        "source_kernel_abi_min=1\nsource_kernel_abi_max=3\n"
        "target_kernel_abi_min=1\ntarget_kernel_abi_max=3\n"
        "source_service_abi_min=1\nsource_service_abi_max=1\n"
        "target_service_abi_min=1\ntarget_service_abi_max=1\n"
        "source_policy_generation_min=1\nsource_policy_generation_max=1\n"
        "target_policy_generation_min=2\ntarget_policy_generation_max=2\n",
        "compatibility_version=1\n"
        "source_image_id=0\ntarget_image_id=8\n"
        "source_schema_min=1\nsource_schema_max=1\n"
        "target_schema_min=1\ntarget_schema_max=2\n"
        "source_kernel_abi_min=1\nsource_kernel_abi_max=3\n"
        "target_kernel_abi_min=1\ntarget_kernel_abi_max=3\n"
        "source_service_abi_min=1\nsource_service_abi_max=1\n"
        "target_service_abi_min=1\ntarget_service_abi_max=1\n"
        "source_policy_generation_min=1\nsource_policy_generation_max=1\n"
        "target_policy_generation_min=2\ntarget_policy_generation_max=2\n",
        "compatibility_version=1\n"
        "source_image_id=7\ntarget_image_id=8\n"
        "source_schema_min=2\nsource_schema_max=1\n"
        "target_schema_min=1\ntarget_schema_max=2\n"
        "source_kernel_abi_min=1\nsource_kernel_abi_max=3\n"
        "target_kernel_abi_min=1\ntarget_kernel_abi_max=3\n"
        "source_service_abi_min=1\nsource_service_abi_max=1\n"
        "target_service_abi_min=1\ntarget_service_abi_max=1\n"
        "source_policy_generation_min=1\nsource_policy_generation_max=1\n"
        "target_policy_generation_min=2\ntarget_policy_generation_max=2\n",
        "compatibility_version=1\n"
        "source_image_id=7\ntarget_image_id=7\n"
        "source_schema_min=1\nsource_schema_max=1\n"
        "target_schema_min=1\ntarget_schema_max=2\n"
        "source_kernel_abi_min=1\nsource_kernel_abi_max=3\n"
        "target_kernel_abi_min=1\ntarget_kernel_abi_max=3\n"
        "source_service_abi_min=1\nsource_service_abi_max=1\n"
        "target_service_abi_min=1\ntarget_service_abi_max=1\n"
        "source_policy_generation_min=1\nsource_policy_generation_max=1\n"
        "target_policy_generation_min=2\ntarget_policy_generation_max=2\n",
    };
    static const enum update_compatibility_status expected[] = {
        UPDATE_COMPATIBILITY_BAD_VERSION,
        UPDATE_COMPATIBILITY_OUT_OF_RANGE,
        UPDATE_COMPATIBILITY_INCOMPATIBLE_RANGE,
        UPDATE_COMPATIBILITY_SAME_IMAGE,
    };
    struct update_compatibility_manifest output;
    size_t index;

    for (index = 0U;
         index < sizeof(texts) / sizeof(texts[0]);
         ++index) {
        poison_manifest(&output);
        CHECK(update_compatibility_manifest_parse(
                  texts[index], strlen(texts[index]), &output) ==
                  expected[index] &&
                  manifest_unchanged(&output),
              "explicit update compatibility parse failure is exact and no-write");
    }
}

static void test_manifest_validate_mutations(void) {
    struct update_compatibility_manifest manifest = make_manifest();
    struct update_compatibility_manifest original = manifest;

    manifest.compatibility_version = 2U;
    CHECK(update_compatibility_manifest_validate(&manifest) ==
              UPDATE_COMPATIBILITY_BAD_VERSION,
          "validate rejects mutated compatibility version");
    manifest = original;
    manifest.source_image_id = 0U;
    CHECK(update_compatibility_manifest_validate(&manifest) ==
              UPDATE_COMPATIBILITY_OUT_OF_RANGE,
          "validate rejects out-of-range image ID");
    manifest = original;
    manifest.target_schema_min = 3U;
    manifest.target_schema_max = 2U;
    CHECK(update_compatibility_manifest_validate(&manifest) ==
              UPDATE_COMPATIBILITY_INCOMPATIBLE_RANGE,
          "validate rejects inverted target schema range");
    manifest = original;
    manifest.target_image_id = manifest.source_image_id;
    CHECK(update_compatibility_manifest_validate(&manifest) ==
              UPDATE_COMPATIBILITY_SAME_IMAGE,
          "validate rejects source and target image collision");
    CHECK(update_compatibility_manifest_validate(&original) ==
              UPDATE_COMPATIBILITY_OK,
          "validate accepts original manifest");
}

static void test_check_mismatches(void) {
    struct update_compatibility_manifest manifest = make_manifest();
    struct update_compatibility_source source = make_source();
    struct update_compatibility_target target = make_target();

    source.image_id = 99U;
    CHECK(update_compatibility_check(&manifest, &source, &target) ==
              UPDATE_COMPATIBILITY_SOURCE_IMAGE_MISMATCH,
          "source image mismatch is rejected");
    source = make_source();
    source.schema_version = 2U;
    CHECK(update_compatibility_check(&manifest, &source, &target) ==
              UPDATE_COMPATIBILITY_SOURCE_SCHEMA_MISMATCH,
          "source schema mismatch is rejected");
    source = make_source();
    source.kernel_abi = 4U;
    CHECK(update_compatibility_check(&manifest, &source, &target) ==
              UPDATE_COMPATIBILITY_SOURCE_ABI_MISMATCH,
          "source ABI mismatch is rejected");
    source = make_source();
    source.service_abi = 2U;
    CHECK(update_compatibility_check(&manifest, &source, &target) ==
              UPDATE_COMPATIBILITY_SOURCE_SERVICE_MISMATCH,
          "source service ABI mismatch is rejected");
    source = make_source();
    source.policy_generation = 2U;
    CHECK(update_compatibility_check(&manifest, &source, &target) ==
              UPDATE_COMPATIBILITY_SOURCE_POLICY_MISMATCH,
          "source policy mismatch is rejected");

    source = make_source();
    target.image_id = 99U;
    CHECK(update_compatibility_check(&manifest, &source, &target) ==
              UPDATE_COMPATIBILITY_TARGET_IMAGE_MISMATCH,
          "target image mismatch is rejected");
    target = make_target();
    target.schema_version = 3U;
    CHECK(update_compatibility_check(&manifest, &source, &target) ==
              UPDATE_COMPATIBILITY_TARGET_SCHEMA_MISMATCH,
          "target schema mismatch is rejected");
    target = make_target();
    target.kernel_abi = 4U;
    CHECK(update_compatibility_check(&manifest, &source, &target) ==
              UPDATE_COMPATIBILITY_TARGET_ABI_MISMATCH,
          "target ABI mismatch is rejected");
    target = make_target();
    target.service_abi = 2U;
    CHECK(update_compatibility_check(&manifest, &source, &target) ==
              UPDATE_COMPATIBILITY_TARGET_SERVICE_MISMATCH,
          "target service ABI mismatch is rejected");
    target = make_target();
    target.policy_generation = 3U;
    CHECK(update_compatibility_check(&manifest, &source, &target) ==
              UPDATE_COMPATIBILITY_TARGET_POLICY_MISMATCH,
          "target policy mismatch is rejected");

    source = make_source();
    source.image_id = 0U;
    CHECK(update_compatibility_check(&manifest, &source, &target) ==
              UPDATE_COMPATIBILITY_INVALID_SOURCE,
          "invalid source state is rejected");
    source = make_source();
    target.image_id = 0U;
    CHECK(update_compatibility_check(&manifest, &source, &target) ==
              UPDATE_COMPATIBILITY_INVALID_TARGET,
          "invalid target state is rejected");
}

static void test_stage_failures_leave_gate_unchanged(void) {
    struct update_compatibility_manifest manifest = make_manifest();
    struct update_compatibility_source source = make_source();
    struct update_compatibility_target target = make_target();
    struct update_compatibility_gate gate;
    struct update_compatibility_gate original;

    poison_gate(&gate);
    gate.version = UPDATE_COMPATIBILITY_VERSION;
    gate.staged = false;
    memcpy(&original, &gate, sizeof(gate));
    source.image_id = 99U;
    CHECK(update_compatibility_stage(
              &gate, &manifest, &source, &target) ==
              UPDATE_COMPATIBILITY_SOURCE_IMAGE_MISMATCH &&
              gate_unchanged(&gate, &original),
          "source-mismatched candidate cannot enter staging");

    source = make_source();
    target.image_id = 99U;
    CHECK(update_compatibility_stage(
              &gate, &manifest, &source, &target) ==
              UPDATE_COMPATIBILITY_TARGET_IMAGE_MISMATCH &&
              gate_unchanged(&gate, &original),
          "target-mismatched candidate cannot enter staging");

    manifest.source_schema_min = 2U;
    manifest.source_schema_max = 1U;
    CHECK(update_compatibility_stage(
              &gate, &manifest, &source, &target) ==
              UPDATE_COMPATIBILITY_INCOMPATIBLE_RANGE &&
              gate_unchanged(&gate, &original),
          "internally incompatible candidate cannot enter staging");
}

int main(void) {
    test_status_strings();
    test_null_and_too_long();
    test_valid_parse_check_and_stage();
    test_parse_failures_never_publish();
    test_explicit_parse_failures();
    test_manifest_validate_mutations();
    test_check_mismatches();
    test_stage_failures_leave_gate_unchanged();

    if (failures != 0) {
        fprintf(stderr, "Update compatibility host tests: FAILED\n");
        return 1;
    }
    printf("Update compatibility host tests: ALL OK\n");
    return 0;
}

#else
#error "This host test must be built with HFOC_UPDATE_COMPATIBILITY_HOST_TEST"
#endif
