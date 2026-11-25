#include "release_profile.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_RELEASE_PROFILE_HOST_TEST

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

static struct release_profile make_profile(void)
{
    struct release_profile profile;

    memset(&profile, 0xA5, sizeof(profile));
    CHECK(release_profile_init(&profile) == RELEASE_PROFILE_OK,
          "release profile fixture initializes descriptor");
    return profile;
}

static struct release_profile_request make_request(void)
{
    struct release_profile_request request;

    memset(&request, 0, sizeof(request));
    request.architecture = RELEASE_PROFILE_ARCH;
    request.architecture_length = RELEASE_PROFILE_ARCH_LENGTH;
    request.image_generation = RELEASE_PROFILE_IMAGE_GENERATION;
    request.policy_version = RELEASE_PROFILE_POLICY_VERSION;
    request.config_schema_version =
        RELEASE_PROFILE_CONFIG_SCHEMA_VERSION;
    request.state_schema_version = RELEASE_PROFILE_STATE_SCHEMA_VERSION;
    request.image_manifest_version =
        RELEASE_PROFILE_IMAGE_MANIFEST_VERSION;
    request.update_compatibility_version =
        RELEASE_PROFILE_UPDATE_COMPATIBILITY_VERSION;
    request.integration_schema_version =
        RELEASE_PROFILE_INTEGRATION_SCHEMA_VERSION;
    request.integration_transport_version =
        RELEASE_PROFILE_INTEGRATION_TRANSPORT_VERSION;
    return request;
}

static void capture_profile(
    const struct release_profile *profile,
    unsigned char *original)
{
    memcpy(original, profile, sizeof(*profile));
}

static void check_profile_unchanged(
    const struct release_profile *profile,
    const unsigned char *original,
    const char *message)
{
    CHECK(memcmp(profile, original, sizeof(*profile)) == 0, message);
}

static void capture_request(
    const struct release_profile_request *request,
    unsigned char *original)
{
    memcpy(original, request, sizeof(*request));
}

static void check_request_unchanged(
    const struct release_profile_request *request,
    const unsigned char *original,
    const char *message)
{
    CHECK(memcmp(request, original, sizeof(*request)) == 0, message);
}

static void check_no_allocation(void)
{
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "release profile performs no allocation");
}

static void test_exact_constants(void)
{
    CHECK(RELEASE_PROFILE_VERSION == 1U,
          "release profile version is exactly 1");
    CHECK(strcmp(RELEASE_PROFILE_ID, "HFOC-APPLIANCE-V1") == 0 &&
              RELEASE_PROFILE_ID_LENGTH == 17U,
          "release profile ID is exactly HFOC-APPLIANCE-V1");
    CHECK(strcmp(RELEASE_PROFILE_ARCH, SYSTEM_IMAGE_ARCH_VALUE) == 0 &&
              strcmp(RELEASE_PROFILE_ARCH, "x86_64") == 0 &&
              RELEASE_PROFILE_ARCH_LENGTH == 6U,
          "release profile architecture is exactly x86_64");
    CHECK(RELEASE_PROFILE_IMAGE_GENERATION == 1U &&
              RELEASE_PROFILE_POLICY_VERSION == 1U,
          "release profile generation and policy are exact");
    CHECK(RELEASE_PROFILE_CONFIG_SCHEMA_VERSION ==
              CONFIG_SCHEMA_VERSION &&
              RELEASE_PROFILE_CONFIG_SCHEMA_VERSION == 1U,
          "release profile config schema version is exact");
    CHECK(RELEASE_PROFILE_STATE_SCHEMA_VERSION == 1U,
          "release profile state schema version is exact");
    CHECK(RELEASE_PROFILE_IMAGE_MANIFEST_VERSION ==
              SYSTEM_IMAGE_MANIFEST_VERSION &&
              RELEASE_PROFILE_UPDATE_COMPATIBILITY_VERSION ==
                  UPDATE_COMPATIBILITY_VERSION,
          "release profile image and update versions are exact");
    CHECK(RELEASE_PROFILE_INTEGRATION_SCHEMA_VERSION ==
              INTEGRATION_BOUNDARY_SCHEMA_VERSION_MAX &&
              RELEASE_PROFILE_INTEGRATION_SCHEMA_VERSION == 1U &&
              RELEASE_PROFILE_INTEGRATION_TRANSPORT_VERSION ==
                  INTEGRATION_BOUNDARY_TRANSPORT_VERSION &&
              RELEASE_PROFILE_INTEGRATION_TRANSPORT_VERSION == 1U,
          "release profile integration versions are exact");
    check_no_allocation();
}

static void test_init(void)
{
    struct release_profile profile;

    CHECK(release_profile_init(NULL) == RELEASE_PROFILE_NULL,
          "release profile init rejects null descriptor");
    CHECK(release_profile_init(&profile) == RELEASE_PROFILE_OK &&
              profile.version == RELEASE_PROFILE_VERSION &&
              strcmp(profile.release_id, RELEASE_PROFILE_ID) == 0 &&
              strcmp(profile.architecture, RELEASE_PROFILE_ARCH) == 0 &&
              profile.image_generation ==
                  RELEASE_PROFILE_IMAGE_GENERATION &&
              profile.policy_version == RELEASE_PROFILE_POLICY_VERSION &&
              profile.config_schema_version ==
                  RELEASE_PROFILE_CONFIG_SCHEMA_VERSION &&
              profile.state_schema_version ==
                  RELEASE_PROFILE_STATE_SCHEMA_VERSION &&
              profile.image_manifest_version ==
                  RELEASE_PROFILE_IMAGE_MANIFEST_VERSION &&
              profile.update_compatibility_version ==
                  RELEASE_PROFILE_UPDATE_COMPATIBILITY_VERSION &&
              profile.integration_schema_version ==
                  RELEASE_PROFILE_INTEGRATION_SCHEMA_VERSION &&
              profile.integration_transport_version ==
                  RELEASE_PROFILE_INTEGRATION_TRANSPORT_VERSION,
          "release profile init publishes exact immutable descriptor");
    check_no_allocation();
}

static void test_validate(void)
{
    struct release_profile profile = make_profile();
    struct release_profile bad = profile;
    unsigned char original[sizeof(profile)];

    capture_profile(&profile, original);
    CHECK(release_profile_validate(NULL) == RELEASE_PROFILE_NULL,
          "release profile validate rejects null descriptor");
    check_profile_unchanged(&profile, original,
                            "null descriptor validation leaves profile unchanged");

    bad = profile;
    bad.version = 0U;
    CHECK(release_profile_validate(&bad) == RELEASE_PROFILE_BAD_VERSION,
          "release profile validate rejects bad version");
    bad = profile;
    memset(bad.release_id, 'X', sizeof(bad.release_id));
    bad.release_id[RELEASE_PROFILE_ID_LENGTH] = '\0';
    CHECK(release_profile_validate(&bad) == RELEASE_PROFILE_BAD_RELEASE_ID,
          "release profile validate rejects malformed release ID");
    bad = profile;
    memset(bad.architecture, 'Y', sizeof(bad.architecture));
    bad.architecture[RELEASE_PROFILE_ARCH_LENGTH] = '\0';
    CHECK(release_profile_validate(&bad) ==
              RELEASE_PROFILE_BAD_ARCHITECTURE,
          "release profile validate rejects malformed architecture");
    bad = profile;
    bad.image_generation = 2U;
    CHECK(release_profile_validate(&bad) == RELEASE_PROFILE_BAD_STATE,
          "release profile validate rejects unsupported image generation");
    bad = profile;
    bad.integration_transport_version = 2U;
    CHECK(release_profile_validate(&bad) == RELEASE_PROFILE_BAD_STATE,
          "release profile validate rejects unsupported integration transport");
    CHECK(release_profile_validate(&profile) == RELEASE_PROFILE_OK,
          "release profile validate accepts exact descriptor");
    check_no_allocation();
}

static void check_unsupported_request(
    struct release_profile_request request,
    enum release_profile_status expected,
    const char *message)
{
    struct release_profile profile = make_profile();
    unsigned char profile_original[sizeof(profile)];
    unsigned char request_original[sizeof(request)];

    capture_profile(&profile, profile_original);
    capture_request(&request, request_original);
    CHECK(release_profile_check(&profile, &request) == expected, message);
    check_profile_unchanged(&profile, profile_original,
                            "unsupported check leaves profile unchanged");
    check_request_unchanged(&request, request_original,
                            "unsupported check leaves request unchanged");
}

static void test_check(void)
{
    struct release_profile profile = make_profile();
    struct release_profile_request request = make_request();
    unsigned char original[sizeof(profile)];

    capture_profile(&profile, original);
    CHECK(release_profile_check(NULL, &request) == RELEASE_PROFILE_NULL,
          "release profile check rejects null profile");
    CHECK(release_profile_check(&profile, NULL) == RELEASE_PROFILE_NULL,
          "release profile check rejects null request");
    check_profile_unchanged(&profile, original,
                            "null profile check leaves profile unchanged");

    request.architecture = NULL;
    check_unsupported_request(
        request, RELEASE_PROFILE_NULL,
        "release profile check rejects null architecture");
    request = make_request();
    request.architecture_length = 5U;
    check_unsupported_request(
        request, RELEASE_PROFILE_UNSUPPORTED_ARCHITECTURE,
        "release profile check rejects wrong architecture length");
    request = make_request();
    request.architecture = "i386";
    request.architecture_length = 4U;
    check_unsupported_request(
        request, RELEASE_PROFILE_UNSUPPORTED_ARCHITECTURE,
        "release profile check rejects unsupported architecture");
    request = make_request();
    request.image_generation = 2U;
    check_unsupported_request(
        request, RELEASE_PROFILE_UNSUPPORTED_IMAGE_GENERATION,
        "release profile check rejects unsupported image generation");
    request = make_request();
    request.policy_version = 2U;
    check_unsupported_request(
        request, RELEASE_PROFILE_UNSUPPORTED_POLICY_VERSION,
        "release profile check rejects unsupported policy version");
    request = make_request();
    request.config_schema_version = 2U;
    check_unsupported_request(
        request, RELEASE_PROFILE_UNSUPPORTED_CONFIG_SCHEMA_VERSION,
        "release profile check rejects unsupported config schema version");
    request = make_request();
    request.state_schema_version = 2U;
    check_unsupported_request(
        request, RELEASE_PROFILE_UNSUPPORTED_STATE_SCHEMA_VERSION,
        "release profile check rejects unsupported state schema version");
    request = make_request();
    request.image_manifest_version = 2U;
    check_unsupported_request(
        request, RELEASE_PROFILE_UNSUPPORTED_IMAGE_MANIFEST_VERSION,
        "release profile check rejects unsupported image manifest version");
    request = make_request();
    request.update_compatibility_version = 2U;
    check_unsupported_request(
        request,
        RELEASE_PROFILE_UNSUPPORTED_UPDATE_COMPATIBILITY_VERSION,
        "release profile check rejects unsupported update compatibility version");
    request = make_request();
    request.integration_schema_version = 2U;
    check_unsupported_request(
        request, RELEASE_PROFILE_UNSUPPORTED_INTEGRATION_SCHEMA_VERSION,
        "release profile check rejects unsupported integration schema version");
    request = make_request();
    request.integration_transport_version = 2U;
    check_unsupported_request(
        request,
        RELEASE_PROFILE_UNSUPPORTED_INTEGRATION_TRANSPORT_VERSION,
        "release profile check rejects unsupported integration transport version");
    request = make_request();
    CHECK(release_profile_check(&profile, &request) == RELEASE_PROFILE_OK,
          "release profile check accepts exact release request");
    check_no_allocation();
}

static const char *expected_status_string(
    enum release_profile_status status)
{
    switch (status) {
    case RELEASE_PROFILE_OK:
        return "RELEASE_PROFILE_OK";
    case RELEASE_PROFILE_NULL:
        return "RELEASE_PROFILE_NULL";
    case RELEASE_PROFILE_BAD_VERSION:
        return "RELEASE_PROFILE_BAD_VERSION";
    case RELEASE_PROFILE_BAD_STATE:
        return "RELEASE_PROFILE_BAD_STATE";
    case RELEASE_PROFILE_BAD_RELEASE_ID:
        return "RELEASE_PROFILE_BAD_RELEASE_ID";
    case RELEASE_PROFILE_BAD_ARCHITECTURE:
        return "RELEASE_PROFILE_BAD_ARCHITECTURE";
    case RELEASE_PROFILE_UNSUPPORTED_ARCHITECTURE:
        return "RELEASE_PROFILE_UNSUPPORTED_ARCHITECTURE";
    case RELEASE_PROFILE_UNSUPPORTED_IMAGE_GENERATION:
        return "RELEASE_PROFILE_UNSUPPORTED_IMAGE_GENERATION";
    case RELEASE_PROFILE_UNSUPPORTED_POLICY_VERSION:
        return "RELEASE_PROFILE_UNSUPPORTED_POLICY_VERSION";
    case RELEASE_PROFILE_UNSUPPORTED_CONFIG_SCHEMA_VERSION:
        return "RELEASE_PROFILE_UNSUPPORTED_CONFIG_SCHEMA_VERSION";
    case RELEASE_PROFILE_UNSUPPORTED_STATE_SCHEMA_VERSION:
        return "RELEASE_PROFILE_UNSUPPORTED_STATE_SCHEMA_VERSION";
    case RELEASE_PROFILE_UNSUPPORTED_IMAGE_MANIFEST_VERSION:
        return "RELEASE_PROFILE_UNSUPPORTED_IMAGE_MANIFEST_VERSION";
    case RELEASE_PROFILE_UNSUPPORTED_UPDATE_COMPATIBILITY_VERSION:
        return "RELEASE_PROFILE_UNSUPPORTED_UPDATE_COMPATIBILITY_VERSION";
    case RELEASE_PROFILE_UNSUPPORTED_INTEGRATION_SCHEMA_VERSION:
        return "RELEASE_PROFILE_UNSUPPORTED_INTEGRATION_SCHEMA_VERSION";
    case RELEASE_PROFILE_UNSUPPORTED_INTEGRATION_TRANSPORT_VERSION:
        return "RELEASE_PROFILE_UNSUPPORTED_INTEGRATION_TRANSPORT_VERSION";
    default:
        return NULL;
    }
}

static void test_status_strings(void)
{
    enum release_profile_status status;

    for (status = RELEASE_PROFILE_OK;
         status <= RELEASE_PROFILE_UNSUPPORTED_INTEGRATION_TRANSPORT_VERSION;
         ++status) {
        const char *string = release_profile_status_string(status);
        const char *expected = expected_status_string(status);

        CHECK(string != NULL && expected != NULL &&
                  strcmp(string, expected) == 0,
              "release profile status string is exact");
    }
    CHECK(release_profile_status_string(
              (enum release_profile_status)15U) == NULL,
          "release profile undefined status is rejected");
    check_no_allocation();
}

int main(void)
{
    test_exact_constants();
    test_init();
    test_validate();
    test_check();
    test_status_strings();

    if (failures != 0) {
        fprintf(stderr, "Release profile host tests: FAILED (%d)\n",
                failures);
        return 1;
    }
    printf("Release profile host tests: ALL OK\n");
    return 0;
}

#endif
