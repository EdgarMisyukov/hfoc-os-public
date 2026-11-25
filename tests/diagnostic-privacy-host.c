#include "diagnostic_privacy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_DIAGNOSTIC_PRIVACY_HOST_TEST

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

static bool all_zero(const unsigned char *bytes, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static void test_strings_and_policy_id(void)
{
    static const struct {
        enum diagnostic_privacy_status status;
        const char *string;
    } statuses[] = {
        {DIAGNOSTIC_PRIVACY_OK, "DIAGNOSTIC_PRIVACY_OK"},
        {DIAGNOSTIC_PRIVACY_NULL, "DIAGNOSTIC_PRIVACY_NULL"},
        {DIAGNOSTIC_PRIVACY_BAD_CATEGORY,
         "DIAGNOSTIC_PRIVACY_BAD_CATEGORY"},
        {DIAGNOSTIC_PRIVACY_BAD_ADDRESS_KIND,
         "DIAGNOSTIC_PRIVACY_BAD_ADDRESS_KIND"},
        {DIAGNOSTIC_PRIVACY_BAD_LENGTH,
         "DIAGNOSTIC_PRIVACY_BAD_LENGTH"},
        {DIAGNOSTIC_PRIVACY_TOO_SMALL,
         "DIAGNOSTIC_PRIVACY_TOO_SMALL"},
    };
    static const struct {
        enum diagnostic_privacy_category category;
        const char *string;
    } categories[] = {
        {DIAGNOSTIC_PRIVACY_CATEGORY_NONE,
         "DIAGNOSTIC_PRIVACY_CATEGORY_NONE"},
        {DIAGNOSTIC_PRIVACY_CATEGORY_CREDENTIAL,
         "DIAGNOSTIC_PRIVACY_CATEGORY_CREDENTIAL"},
        {DIAGNOSTIC_PRIVACY_CATEGORY_VERIFIER_MATERIAL,
         "DIAGNOSTIC_PRIVACY_CATEGORY_VERIFIER_MATERIAL"},
        {DIAGNOSTIC_PRIVACY_CATEGORY_CAPABILITY_VALUE,
         "DIAGNOSTIC_PRIVACY_CATEGORY_CAPABILITY_VALUE"},
        {DIAGNOSTIC_PRIVACY_CATEGORY_PRIVATE_ADDRESS,
         "DIAGNOSTIC_PRIVACY_CATEGORY_PRIVATE_ADDRESS"},
    };
    size_t index;

    alloc_attempts = 0U;
    free_attempts = 0U;
    CHECK(diagnostic_privacy_version() == DIAGNOSTIC_PRIVACY_VERSION,
          "diagnostic privacy version is exact");
    CHECK(diagnostic_privacy_policy_id() != NULL &&
              strlen(diagnostic_privacy_policy_id()) ==
                  DIAGNOSTIC_PRIVACY_POLICY_ID_LENGTH &&
              memcmp(diagnostic_privacy_policy_id(),
                     DIAGNOSTIC_PRIVACY_POLICY_ID,
                     DIAGNOSTIC_PRIVACY_POLICY_ID_LENGTH) == 0,
          "diagnostic privacy policy ID is exact");
    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        CHECK(diagnostic_privacy_status_string(
                  statuses[index].status) != NULL &&
                  strcmp(diagnostic_privacy_status_string(
                             statuses[index].status),
                         statuses[index].string) == 0,
              "diagnostic privacy status string is exact");
    }
    CHECK(strcmp(diagnostic_privacy_status_string(
                     (enum diagnostic_privacy_status)99U),
                 "DIAGNOSTIC_PRIVACY_UNKNOWN") == 0,
          "undefined diagnostic privacy status string is exact");
    for (index = 0U;
         index < sizeof(categories) / sizeof(categories[0]);
         ++index) {
        CHECK(diagnostic_privacy_category_string(
                  categories[index].category) != NULL &&
                  strcmp(diagnostic_privacy_category_string(
                             categories[index].category),
                         categories[index].string) == 0,
              "diagnostic privacy category string is exact");
    }
    CHECK(strcmp(diagnostic_privacy_category_string(
                     (enum diagnostic_privacy_category)99U),
                 "DIAGNOSTIC_PRIVACY_CATEGORY_UNKNOWN") == 0,
          "undefined diagnostic privacy category string is exact");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "diagnostic privacy strings perform no allocation");
}

static void test_null_and_malformed_inputs(void)
{
    struct credential_verifier verifier;
    struct capability capability;
    enum diagnostic_privacy_category category =
        DIAGNOSTIC_PRIVACY_CATEGORY_PRIVATE_ADDRESS;
    unsigned char output[4];
    size_t output_length = 0U;

    memset(&verifier, 0, sizeof(verifier));
    memset(&capability, 0, sizeof(capability));
    memset(output, 0xA5U, sizeof(output));

    CHECK(diagnostic_privacy_classify_key(NULL, 1U, &category) ==
              DIAGNOSTIC_PRIVACY_NULL &&
              category == DIAGNOSTIC_PRIVACY_CATEGORY_PRIVATE_ADDRESS,
          "key classifier rejects null key without mutation");
    CHECK(diagnostic_privacy_classify_key("secret", 6U, NULL) ==
              DIAGNOSTIC_PRIVACY_NULL,
          "key classifier rejects null category output");
    CHECK(diagnostic_privacy_classify_key("", 0U, &category) ==
              DIAGNOSTIC_PRIVACY_OK &&
              category == DIAGNOSTIC_PRIVACY_CATEGORY_NONE,
          "empty key classifies as safe");
    CHECK(diagnostic_privacy_classify_verifier(NULL, &category) ==
              DIAGNOSTIC_PRIVACY_NULL,
          "verifier classifier rejects null verifier");
    CHECK(diagnostic_privacy_classify_verifier(&verifier, NULL) ==
              DIAGNOSTIC_PRIVACY_NULL,
          "verifier classifier rejects null category output");
    CHECK(diagnostic_privacy_classify_capability(NULL, &category) ==
              DIAGNOSTIC_PRIVACY_NULL,
          "capability classifier rejects null capability");
    CHECK(diagnostic_privacy_classify_capability(&capability, NULL) ==
              DIAGNOSTIC_PRIVACY_NULL,
          "capability classifier rejects null category output");
    CHECK(diagnostic_privacy_classify_address(
              DIAGNOSTIC_PRIVACY_ADDRESS_IPV4, NULL, 4U,
              &category) == DIAGNOSTIC_PRIVACY_NULL,
          "address classifier rejects null data");
    CHECK(diagnostic_privacy_classify_address(
              DIAGNOSTIC_PRIVACY_ADDRESS_IPV4, "192.168.1.1", 11U,
              NULL) == DIAGNOSTIC_PRIVACY_NULL,
          "address classifier rejects null category output");
    CHECK(diagnostic_privacy_classify_address(
              DIAGNOSTIC_PRIVACY_ADDRESS_NONE, "abcd", 4U,
              &category) == DIAGNOSTIC_PRIVACY_BAD_ADDRESS_KIND,
          "address classifier rejects address kind none");
    CHECK(diagnostic_privacy_classify_address(
              DIAGNOSTIC_PRIVACY_ADDRESS_COUNT, "abcd", 4U,
              &category) == DIAGNOSTIC_PRIVACY_BAD_ADDRESS_KIND,
          "address classifier rejects unknown address kind");
    CHECK(diagnostic_privacy_classify_address(
              DIAGNOSTIC_PRIVACY_ADDRESS_MAC, "abcdef", 5U,
              &category) == DIAGNOSTIC_PRIVACY_BAD_LENGTH,
          "address classifier rejects wrong MAC length");
    CHECK(diagnostic_privacy_redact(
              DIAGNOSTIC_PRIVACY_CATEGORY_CREDENTIAL, NULL, 1U,
              output, sizeof(output), &output_length) ==
              DIAGNOSTIC_PRIVACY_NULL,
          "redaction rejects null input with nonzero length");
    CHECK(diagnostic_privacy_redact(
              DIAGNOSTIC_PRIVACY_CATEGORY_CREDENTIAL, "x", 1U,
              NULL, sizeof(output), &output_length) ==
              DIAGNOSTIC_PRIVACY_NULL,
          "redaction rejects null output");
    CHECK(diagnostic_privacy_redact(
              DIAGNOSTIC_PRIVACY_CATEGORY_CREDENTIAL, "x", 1U,
              output, sizeof(output), NULL) ==
              DIAGNOSTIC_PRIVACY_NULL,
          "redaction rejects null length output");
    CHECK(diagnostic_privacy_redact(
              DIAGNOSTIC_PRIVACY_CATEGORY_COUNT, "x", 1U,
              output, sizeof(output), &output_length) ==
              DIAGNOSTIC_PRIVACY_BAD_CATEGORY,
          "redaction rejects category count");
}

static void test_key_classification(void)
{
    static const char *const secret_keys[] = {
        "credential",
        "secret",
        "password",
        "token",
        "private_key",
        "capability",
    };
    static const char *const safe_keys[] = {
        "alpha",
        "username",
        "host",
        "dns",
    };
    enum diagnostic_privacy_category category =
        DIAGNOSTIC_PRIVACY_CATEGORY_NONE;
    size_t index;

    for (index = 0U;
         index < sizeof(secret_keys) / sizeof(secret_keys[0]);
         ++index) {
        CHECK(diagnostic_privacy_classify_key(
                  secret_keys[index], strlen(secret_keys[index]),
                  &category) == DIAGNOSTIC_PRIVACY_OK &&
                  category ==
                      DIAGNOSTIC_PRIVACY_CATEGORY_CREDENTIAL,
              "fixed secret config key is classified as credential");
    }
    for (index = 0U;
         index < sizeof(safe_keys) / sizeof(safe_keys[0]);
         ++index) {
        CHECK(diagnostic_privacy_classify_key(
                  safe_keys[index], strlen(safe_keys[index]),
                  &category) == DIAGNOSTIC_PRIVACY_OK &&
                  category == DIAGNOSTIC_PRIVACY_CATEGORY_NONE,
              "safe config key is not classified as credential");
    }
}

static void test_verifier_and_capability_classification(void)
{
    struct credential_verifier verifier;
    struct capability capability;
    enum diagnostic_privacy_category category =
        DIAGNOSTIC_PRIVACY_CATEGORY_NONE;

    memset(&verifier, 0, sizeof(verifier));
    verifier.version = CREDENTIAL_VERIFIER_VERSION;
    verifier.principal_id = 1U;
    verifier.state = CREDENTIAL_VERIFIER_STATE_ENABLED;
    memset(verifier.expected_secret, 0x5AU,
           sizeof(verifier.expected_secret));

    memset(&capability, 0, sizeof(capability));
    capability.type = CAPABILITY_TYPE_SYSCALL_WRITE;
    capability.flags = 0x1000U;
    capability.object = 0x123456789U;

    CHECK(diagnostic_privacy_classify_verifier(
              &verifier, &category) == DIAGNOSTIC_PRIVACY_OK &&
              category ==
                  DIAGNOSTIC_PRIVACY_CATEGORY_VERIFIER_MATERIAL,
          "verifier object classifies as verifier material");
    CHECK(diagnostic_privacy_classify_capability(
              &capability, &category) == DIAGNOSTIC_PRIVACY_OK &&
              category ==
                  DIAGNOSTIC_PRIVACY_CATEGORY_CAPABILITY_VALUE,
          "capability object classifies as raw capability value");
}

static void test_address_classification(void)
{
    static const unsigned char ipv4[
        DIAGNOSTIC_PRIVACY_IPV4_LENGTH] = {
        192U, 168U, 1U, 10U
    };
    static const unsigned char mac[
        DIAGNOSTIC_PRIVACY_MAC_LENGTH] = {
        0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U
    };
    static const unsigned char bus[
        DIAGNOSTIC_PRIVACY_BUS_ADDRESS_LENGTH] = {
        0x00U, 0x10U, 0x00U, 0x00U
    };
    enum diagnostic_privacy_category category =
        DIAGNOSTIC_PRIVACY_CATEGORY_NONE;

    CHECK(diagnostic_privacy_classify_address(
              DIAGNOSTIC_PRIVACY_ADDRESS_IPV4, ipv4, sizeof(ipv4),
              &category) == DIAGNOSTIC_PRIVACY_OK &&
              category ==
                  DIAGNOSTIC_PRIVACY_CATEGORY_PRIVATE_ADDRESS,
          "IPv4 address classifies as private");
    CHECK(diagnostic_privacy_classify_address(
              DIAGNOSTIC_PRIVACY_ADDRESS_MAC, mac, sizeof(mac),
              &category) == DIAGNOSTIC_PRIVACY_OK &&
              category ==
                  DIAGNOSTIC_PRIVACY_CATEGORY_PRIVATE_ADDRESS,
          "MAC address classifies as private");
    CHECK(diagnostic_privacy_classify_address(
              DIAGNOSTIC_PRIVACY_ADDRESS_BUS, bus, sizeof(bus),
              &category) == DIAGNOSTIC_PRIVACY_OK &&
              category ==
                  DIAGNOSTIC_PRIVACY_CATEGORY_PRIVATE_ADDRESS,
          "bus address classifies as private");
}

static void test_redaction_negative_fixtures(void)
{
    static const unsigned char safe[] = "node-state:ok\n";
    static const unsigned char credential[] = "hunter2";
    static const unsigned char ipv4[
        DIAGNOSTIC_PRIVACY_IPV4_LENGTH] = {
        192U, 168U, 1U, 10U
    };
    struct credential_verifier verifier;
    struct capability capability;
    unsigned char output[sizeof(struct credential_verifier) +
                         sizeof(struct capability) +
                         sizeof(safe) + sizeof(credential) + 4U];
    size_t output_length = 0U;
    size_t index;

    memset(&verifier, 0, sizeof(verifier));
    memset(verifier.expected_secret, 0xA5U,
           sizeof(verifier.expected_secret));
    memset(&capability, 0, sizeof(capability));
    capability.type = 1U;
    capability.flags = 0x80000000U;
    capability.object = UINT64_C(0x0123456789ABCDEF);

    memset(output, 0xCCU, sizeof(output));
    CHECK(diagnostic_privacy_redact(
              DIAGNOSTIC_PRIVACY_CATEGORY_NONE, safe, sizeof(safe),
              output, sizeof(output), &output_length) ==
              DIAGNOSTIC_PRIVACY_OK &&
              output_length == sizeof(safe) &&
              memcmp(output, safe, sizeof(safe)) == 0,
          "safe diagnostic bytes are copied exactly");

    CHECK(diagnostic_privacy_redact(
              DIAGNOSTIC_PRIVACY_CATEGORY_CREDENTIAL, credential,
              sizeof(credential), output, sizeof(output),
              &output_length) == DIAGNOSTIC_PRIVACY_OK &&
              output_length == sizeof(credential) &&
              all_zero(output, sizeof(credential)),
          "credential bytes are zero redacted");
    for (index = 0U; index < sizeof(credential); ++index) {
        if (credential[index] != 0U) {
            CHECK(output[index] != credential[index],
                  "credential negative fixture contains no original byte");
        }
    }

    CHECK(diagnostic_privacy_redact(
              DIAGNOSTIC_PRIVACY_CATEGORY_VERIFIER_MATERIAL,
              verifier.expected_secret,
              sizeof(verifier.expected_secret), output,
              sizeof(output), &output_length) ==
              DIAGNOSTIC_PRIVACY_OK &&
              output_length == sizeof(verifier.expected_secret) &&
              all_zero(output, sizeof(verifier.expected_secret)),
          "verifier material is zero redacted");
    CHECK(diagnostic_privacy_redact(
              DIAGNOSTIC_PRIVACY_CATEGORY_CAPABILITY_VALUE,
              &capability, sizeof(capability), output,
              sizeof(output), &output_length) ==
              DIAGNOSTIC_PRIVACY_OK &&
              output_length == sizeof(capability) &&
              all_zero(output, sizeof(capability)),
          "raw capability values are zero redacted");
    CHECK(diagnostic_privacy_redact(
              DIAGNOSTIC_PRIVACY_CATEGORY_PRIVATE_ADDRESS, ipv4,
              sizeof(ipv4), output, sizeof(output),
              &output_length) == DIAGNOSTIC_PRIVACY_OK &&
              output_length == sizeof(ipv4) &&
              all_zero(output, sizeof(ipv4)),
          "private address bytes are zero redacted");

    CHECK(memcmp(credential, "hunter2", sizeof(credential)) == 0 &&
              verifier.expected_secret[0] == 0xA5U &&
              capability.object == UINT64_C(0x0123456789ABCDEF),
          "redaction leaves all source bytes unchanged");
}

static void test_redaction_back_pressure(void)
{
    static const unsigned char input[] = {
        0x10U, 0x20U, 0x30U, 0x40U
    };
    unsigned char output[4];
    unsigned char before[4];
    size_t output_length = 99U;

    memset(output, 0xA5U, sizeof(output));
    memcpy(before, output, sizeof(before));
    CHECK(diagnostic_privacy_redact(
              DIAGNOSTIC_PRIVACY_CATEGORY_PRIVATE_ADDRESS, input,
              sizeof(input), output, sizeof(output) - 1U,
              &output_length) == DIAGNOSTIC_PRIVACY_TOO_SMALL &&
              memcmp(output, before, sizeof(before)) == 0 &&
              output_length == 99U,
          "short redaction output returns too-small without mutation");
    CHECK(diagnostic_privacy_redact(
              DIAGNOSTIC_PRIVACY_CATEGORY_PRIVATE_ADDRESS, input,
              sizeof(input), output, sizeof(output),
              &output_length) == DIAGNOSTIC_PRIVACY_OK &&
              output_length == sizeof(input) &&
              all_zero(output, sizeof(input)),
          "redaction retry succeeds with enough capacity");
}

int main(void)
{
    test_strings_and_policy_id();
    test_null_and_malformed_inputs();
    test_key_classification();
    test_verifier_and_capability_classification();
    test_address_classification();
    test_redaction_negative_fixtures();
    test_redaction_back_pressure();

    if (failures != 0) {
        fprintf(stderr, "Diagnostic privacy host tests: %d failures\n",
                failures);
        return 1;
    }
    printf("Diagnostic privacy host tests: ALL OK\n");
    return 0;
}

#endif
