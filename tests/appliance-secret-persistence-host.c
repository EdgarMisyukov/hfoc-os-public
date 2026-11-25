#include "appliance_secret_persistence.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef APPLIANCE_SECRET_PERSISTENCE_HOST_TEST

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

static const char hex_digest[] =
    "0123456789abcdef0123456789abcdef"
    "0123456789abcdef0123456789abcdef";

struct expected_record {
    enum appliance_secret_persistence_record_kind kind;
    enum appliance_persistent_storage_namespace_id namespace_id;
    enum appliance_secret_persistence_handling handling;
    const char *name;
    const char *prefix;
    const char *purpose;
};

static const struct expected_record expected_records[] = {
    {
        APPLIANCE_SECRET_PERSISTENCE_KIND_CERTIFICATE_HTTPS_IDENTITY,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_CERTIFICATE,
        APPLIANCE_SECRET_PERSISTENCE_HANDLING_CERTIFICATE_RAW,
        "certificate.https",
        "v1:certificate.https=",
        "TLS HTTPS identity certificate chain",
    },
    {
        APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_ADMIN_CREDENTIAL,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SECRET,
        APPLIANCE_SECRET_PERSISTENCE_HANDLING_PBKDF2_VERIFIER,
        "secret.admin.credential",
        "v1:secret.admin.credential=",
        "administrative credential PBKDF2-HMAC-SHA256 verifier with salt and iteration count",
    },
    {
        APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_HTTPS_PRIVATE_KEY_DIGEST,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SECRET,
        APPLIANCE_SECRET_PERSISTENCE_HANDLING_DIGEST_ONLY,
        "secret.https.private_key.digest",
        "v1:secret.https.private_key.digest=",
        "HTTPS private-key digest and rotation metadata",
    },
    {
        APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_HTTPS_PRIVATE_KEY_WRAPPED,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SECRET,
        APPLIANCE_SECRET_PERSISTENCE_HANDLING_WRAPPED_ONLY,
        "secret.https.private_key.wrapped",
        "v1:secret.https.private_key.wrapped=",
        "opaque wrapped HTTPS private-key blob",
    },
};

static bool record_matches(
    const struct appliance_secret_persistence_record *record,
    const struct expected_record *expected)
{
    return record != NULL &&
           record->kind == expected->kind &&
           record->namespace_id == expected->namespace_id &&
           record->handling == expected->handling &&
           strcmp(record->name, expected->name) == 0 &&
           strcmp(record->prefix, expected->prefix) == 0 &&
           record->version == APPLIANCE_SECRET_PERSISTENCE_VERSION &&
           record->minimum_version ==
               APPLIANCE_SECRET_PERSISTENCE_VERSION &&
           record->maximum_version ==
               APPLIANCE_SECRET_PERSISTENCE_VERSION &&
           strcmp(record->purpose, expected->purpose) == 0;
}

static void test_catalogue_and_describe(void)
{
    size_t index;

    CHECK(appliance_secret_persistence_count() ==
              APPLIANCE_SECRET_PERSISTENCE_RECORD_COUNT,
          "secret persistence catalogue has exact record count");
    CHECK(appliance_secret_persistence_catalogue_at(
              0U, NULL) == APPLIANCE_SECRET_PERSISTENCE_NULL,
          "catalogue-at rejects null record output");
    for (index = 0U;
         index < APPLIANCE_SECRET_PERSISTENCE_RECORD_COUNT;
         ++index) {
        const struct appliance_secret_persistence_record *record = NULL;

        CHECK(appliance_secret_persistence_catalogue_at(
                  index, &record) == APPLIANCE_SECRET_PERSISTENCE_OK &&
                  record_matches(record, &expected_records[index]),
              "catalogue-at returns exact immutable record");
        CHECK(appliance_secret_persistence_describe(
                  expected_records[index].kind,
                  &record) == APPLIANCE_SECRET_PERSISTENCE_OK &&
                  record_matches(record, &expected_records[index]),
              "describe returns exact immutable record by kind");
    }
    CHECK(appliance_secret_persistence_catalogue_at(
              APPLIANCE_SECRET_PERSISTENCE_RECORD_COUNT,
              NULL) == APPLIANCE_SECRET_PERSISTENCE_NULL,
          "catalogue-at rejects out-of-range before output check");
    {
        const struct appliance_secret_persistence_record *record = NULL;

        CHECK(appliance_secret_persistence_catalogue_at(
                  APPLIANCE_SECRET_PERSISTENCE_RECORD_COUNT,
                  &record) == APPLIANCE_SECRET_PERSISTENCE_OUT_OF_RANGE &&
                  record == NULL,
              "catalogue-at out-of-range publishes no record");
        CHECK(appliance_secret_persistence_describe(
                  APPLIANCE_SECRET_PERSISTENCE_KIND_NONE,
                  &record) == APPLIANCE_SECRET_PERSISTENCE_BAD_ID &&
                  record == NULL,
              "describe unknown kind publishes no record");
    }
}

static bool classify_ok(
    const char *key,
    enum appliance_secret_persistence_record_kind kind,
    enum appliance_persistent_storage_namespace_id ns)
{
    enum appliance_secret_persistence_record_kind actual_kind =
        APPLIANCE_SECRET_PERSISTENCE_KIND_NONE;
    enum appliance_persistent_storage_namespace_id actual_ns =
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE;
    uint32_t actual_version = 0U;

    return appliance_secret_persistence_classify_record_key(
               key, strlen(key), &actual_kind, &actual_ns,
               &actual_version) == APPLIANCE_SECRET_PERSISTENCE_OK &&
           actual_kind == kind && actual_ns == ns &&
           actual_version == APPLIANCE_SECRET_PERSISTENCE_VERSION;
}

static void test_classification(void)
{
    CHECK(classify_ok(
              "v1:certificate.https=DER",
              APPLIANCE_SECRET_PERSISTENCE_KIND_CERTIFICATE_HTTPS_IDENTITY,
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_CERTIFICATE),
          "certificate identity key classifies to certificate namespace");
    {
        char key[128];

        snprintf(key, sizeof(key), "v1:secret.admin.credential=%s",
                 hex_digest);
        CHECK(classify_ok(
                  key,
                  APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_ADMIN_CREDENTIAL,
                  APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SECRET),
              "admin credential key classifies to secret namespace");
        snprintf(
            key, sizeof(key),
            "v1:secret.https.private_key.digest=%s\n",
            hex_digest);
        CHECK(classify_ok(
                  key,
                  APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_HTTPS_PRIVATE_KEY_DIGEST,
                  APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SECRET),
              "private-key digest key accepts trailing newline");
        snprintf(
            key, sizeof(key),
            "v1:secret.https.private_key.wrapped=%s",
            "opaque-blob");
        CHECK(classify_ok(
                  key,
                  APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_HTTPS_PRIVATE_KEY_WRAPPED,
                  APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SECRET),
              "wrapped private-key key classifies to secret namespace");
    }
}

static void test_classification_rejections(void)
{
    enum appliance_secret_persistence_record_kind kind =
        APPLIANCE_SECRET_PERSISTENCE_KIND_NONE;
    enum appliance_persistent_storage_namespace_id ns =
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE;
    uint32_t version = 0U;
    unsigned char buffer[
        APPLIANCE_SECRET_PERSISTENCE_RECORD_MAX + 1U];

    CHECK(appliance_secret_persistence_classify_record_key(
              NULL, 1U, &kind, &ns, &version) ==
              APPLIANCE_SECRET_PERSISTENCE_NULL,
          "classify rejects null key");
    CHECK(appliance_secret_persistence_classify_record_key(
              "v1:certificate.https=a", 0U, &kind, &ns, &version) ==
              APPLIANCE_SECRET_PERSISTENCE_BAD_RECORD,
          "classify rejects zero length record");
    CHECK(appliance_secret_persistence_classify_record_key(
              "", 0U, &kind, &ns, &version) ==
              APPLIANCE_SECRET_PERSISTENCE_BAD_RECORD,
          "classify rejects empty key");
    CHECK(appliance_secret_persistence_classify_record_key(
              "v2:certificate.https=a",
              strlen("v2:certificate.https=a"),
              &kind, &ns, &version) ==
              APPLIANCE_SECRET_PERSISTENCE_BAD_VERSION,
          "classify rejects unsupported version");
    CHECK(appliance_secret_persistence_classify_record_key(
              "v1:system.hostname=hfoc-appliance",
              strlen("v1:system.hostname=hfoc-appliance"),
              &kind, &ns, &version) ==
              APPLIANCE_SECRET_PERSISTENCE_UNKNOWN_KEY,
          "classify rejects system configuration key as unknown secret");
    CHECK(appliance_secret_persistence_classify_record_key(
              "v1:certificate.https=",
              strlen("v1:certificate.https="),
              &kind, &ns, &version) ==
              APPLIANCE_SECRET_PERSISTENCE_BAD_RECORD,
          "classify rejects bare certificate prefix");
    CHECK(appliance_secret_persistence_classify_record_key(
              "v1:certificate.https",
              strlen("v1:certificate.https"),
              &kind, &ns, &version) ==
              APPLIANCE_SECRET_PERSISTENCE_BAD_RECORD,
          "classify rejects missing equals");
    CHECK(appliance_secret_persistence_classify_record_key(
              "v1:secret.https.private_key=",
              strlen("v1:secret.https.private_key="),
              &kind, &ns, &version) ==
              APPLIANCE_SECRET_PERSISTENCE_UNKNOWN_KEY,
          "classify rejects raw private-key record key");
    CHECK(appliance_secret_persistence_classify_record_key(
              "v1:certificate.https=DER\nBAD",
              strlen("v1:certificate.https=DER\nBAD"),
              &kind, &ns, &version) ==
              APPLIANCE_SECRET_PERSISTENCE_BAD_RECORD,
          "classify rejects interior newline");
    memset(buffer, (int)'x', sizeof(buffer));
    CHECK(appliance_secret_persistence_classify_record_key(
              (const char *)buffer, sizeof(buffer), &kind, &ns,
              &version) == APPLIANCE_SECRET_PERSISTENCE_BAD_RECORD,
          "classify rejects over-limit record");
    CHECK(appliance_secret_persistence_classify_record_key(
              "v1:certificate.https=DER", 1U, &kind, &ns, &version) ==
              APPLIANCE_SECRET_PERSISTENCE_BAD_RECORD,
          "classify length mismatch is rejected");
}

static void test_value_policy(void)
{
    uint8_t zero[APPLIANCE_SECRET_PERSISTENCE_HEX_DIGEST_LENGTH];
    uint8_t oversized[
        APPLIANCE_SECRET_PERSISTENCE_RECORD_MAX + 1U];
    char uppercase[APPLIANCE_SECRET_PERSISTENCE_HEX_DIGEST_LENGTH + 1U];
    size_t index;

    memset(zero, 0, sizeof(zero));
    memset(oversized, (int)'A', sizeof(oversized));

    CHECK(appliance_secret_persistence_validate_value(
              APPLIANCE_SECRET_PERSISTENCE_KIND_CERTIFICATE_HTTPS_IDENTITY,
              (const uint8_t *)"DER", 3U) ==
              APPLIANCE_SECRET_PERSISTENCE_OK,
          "certificate identity accepts nonempty bounded chain");
    CHECK(appliance_secret_persistence_validate_value(
              APPLIANCE_SECRET_PERSISTENCE_KIND_CERTIFICATE_HTTPS_IDENTITY,
              (const uint8_t *)"", 0U) ==
              APPLIANCE_SECRET_PERSISTENCE_BAD_VALUE,
          "certificate identity rejects empty chain");
    CHECK(appliance_secret_persistence_validate_value(
              APPLIANCE_SECRET_PERSISTENCE_KIND_CERTIFICATE_HTTPS_IDENTITY,
              zero, 1U) == APPLIANCE_SECRET_PERSISTENCE_BAD_VALUE,
          "certificate identity rejects all-zero chain");
    CHECK(appliance_secret_persistence_validate_value(
              APPLIANCE_SECRET_PERSISTENCE_KIND_CERTIFICATE_HTTPS_IDENTITY,
              oversized, sizeof(oversized)) ==
              APPLIANCE_SECRET_PERSISTENCE_BAD_VALUE,
          "certificate identity rejects over-limit chain");

    CHECK(appliance_secret_persistence_validate_value(
              APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_ADMIN_CREDENTIAL,
              (const uint8_t *)hex_digest,
              APPLIANCE_SECRET_PERSISTENCE_HEX_DIGEST_LENGTH) ==
              APPLIANCE_SECRET_PERSISTENCE_OK,
          "admin credential fingerprint accepts lowercase hex digest");
    CHECK(appliance_secret_persistence_validate_value(
              APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_ADMIN_CREDENTIAL,
              (const uint8_t *)hex_digest,
              APPLIANCE_SECRET_PERSISTENCE_HEX_DIGEST_LENGTH - 1U) ==
              APPLIANCE_SECRET_PERSISTENCE_BAD_VALUE,
          "admin credential fingerprint rejects short value");
    for (index = 0U;
         index < APPLIANCE_SECRET_PERSISTENCE_HEX_DIGEST_LENGTH;
         ++index) {
        char byte = hex_digest[index];

        uppercase[index] =
            byte >= 'a' && byte <= 'f'
            ? (char)(byte - ('a' - 'A'))
            : byte;
    }
    uppercase[APPLIANCE_SECRET_PERSISTENCE_HEX_DIGEST_LENGTH] = '\0';
    CHECK(appliance_secret_persistence_validate_value(
              APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_ADMIN_CREDENTIAL,
              (const uint8_t *)uppercase,
              APPLIANCE_SECRET_PERSISTENCE_HEX_DIGEST_LENGTH) ==
              APPLIANCE_SECRET_PERSISTENCE_BAD_VALUE,
          "admin credential fingerprint rejects uppercase hex");
    /*
     * ITEM-506: the same record kind also accepts the structured PBKDF2
     * value, and only that shape - not every printable blob.
     */
    {
        static const char structured[] =
            "pbkdf2-sha256$10000$00112233445566778899aabbccddeeff$"
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef$"
            "operator";
        static const char truncated[] =
            "pbkdf2-sha256$10000$00112233445566778899aabbccddeeff$"
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde$"
            "operator";
        static const char foreign[] = "some other printable value";

        CHECK(appliance_secret_persistence_validate_value(
                  APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_ADMIN_CREDENTIAL,
                  (const uint8_t *)structured, strlen(structured)) ==
                  APPLIANCE_SECRET_PERSISTENCE_OK,
              "admin credential accepts the structured PBKDF2 value");
        /*
         * The catalogue checks the shared shape (label, printable bytes,
         * bound); the exact field grammar belongs to
         * `appliance_admin_credential`, which refuses this truncated verifier.
         * The layering is asserted here so a later reader does not mistake
         * the catalogue for the policy owner.
         */
        CHECK(appliance_secret_persistence_validate_value(
                  APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_ADMIN_CREDENTIAL,
                  (const uint8_t *)truncated, strlen(truncated)) ==
                  APPLIANCE_SECRET_PERSISTENCE_OK,
              "the catalogue accepts the shared shape and leaves the exact grammar to the credential module");
        CHECK(appliance_secret_persistence_validate_value(
                  APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_ADMIN_CREDENTIAL,
                  (const uint8_t *)foreign, strlen(foreign)) ==
                  APPLIANCE_SECRET_PERSISTENCE_BAD_VALUE,
              "admin credential rejects a printable value of another shape");
    }

    CHECK(appliance_secret_persistence_validate_value(
              APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_HTTPS_PRIVATE_KEY_DIGEST,
              (const uint8_t *)hex_digest,
              APPLIANCE_SECRET_PERSISTENCE_HEX_DIGEST_LENGTH) ==
              APPLIANCE_SECRET_PERSISTENCE_OK,
          "private-key digest accepts lowercase hex digest");
    CHECK(appliance_secret_persistence_validate_value(
              APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_HTTPS_PRIVATE_KEY_DIGEST,
              (const uint8_t *)"raw-private-key",
              strlen("raw-private-key")) ==
              APPLIANCE_SECRET_PERSISTENCE_BAD_VALUE,
          "private-key digest rejects raw key material");

    CHECK(appliance_secret_persistence_validate_value(
              APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_HTTPS_PRIVATE_KEY_WRAPPED,
              (const uint8_t *)"opaque", 6U) ==
              APPLIANCE_SECRET_PERSISTENCE_OK,
          "wrapped private key accepts nonempty opaque blob");
    CHECK(appliance_secret_persistence_validate_value(
              APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_HTTPS_PRIVATE_KEY_WRAPPED,
              zero, 1U) == APPLIANCE_SECRET_PERSISTENCE_BAD_VALUE,
          "wrapped private key rejects all-zero blob");
    CHECK(appliance_secret_persistence_validate_value(
              APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_HTTPS_PRIVATE_KEY_WRAPPED,
              oversized, sizeof(oversized)) ==
              APPLIANCE_SECRET_PERSISTENCE_BAD_VALUE,
          "wrapped private key rejects over-limit blob");

    CHECK(appliance_secret_persistence_validate_value(
              APPLIANCE_SECRET_PERSISTENCE_KIND_NONE,
              (const uint8_t *)hex_digest,
              APPLIANCE_SECRET_PERSISTENCE_HEX_DIGEST_LENGTH) ==
              APPLIANCE_SECRET_PERSISTENCE_BAD_ID,
          "value policy rejects unknown kind");
    CHECK(appliance_secret_persistence_validate_value(
              APPLIANCE_SECRET_PERSISTENCE_KIND_CERTIFICATE_HTTPS_IDENTITY,
              NULL, 1U) == APPLIANCE_SECRET_PERSISTENCE_NULL,
          "value policy rejects null value");
}

static void test_https_readiness(void)
{
    struct appliance_secret_persistence_https_state state;
    uint32_t mask;
    bool ready;
    uint32_t untouched_mask;
    bool untouched_ready;

    memset(&state, 0, sizeof(state));
    state.version = APPLIANCE_SECRET_PERSISTENCE_VERSION;
    state.certificate_published = false;
    state.private_key_wrapped_published = false;
    mask = UINT32_MAX;
    ready = false;
    CHECK(appliance_secret_persistence_https_readiness(
              &state, &mask, &ready) == APPLIANCE_SECRET_PERSISTENCE_OK &&
              mask == 0U && !ready,
          "https readiness reports unavailable when both records missing");

    state.certificate_published = true;
    mask = UINT32_MAX;
    CHECK(appliance_secret_persistence_https_readiness(
              &state, &mask, &ready) == APPLIANCE_SECRET_PERSISTENCE_OK &&
              mask == 0U && !ready,
          "https readiness stays unavailable without wrapped key");

    state.certificate_published = false;
    state.private_key_wrapped_published = true;
    mask = UINT32_MAX;
    CHECK(appliance_secret_persistence_https_readiness(
              &state, &mask, &ready) == APPLIANCE_SECRET_PERSISTENCE_OK &&
              mask == 0U && !ready,
          "https readiness stays unavailable without certificate");

    state.certificate_published = true;
    mask = 0U;
    ready = false;
    CHECK(appliance_secret_persistence_https_readiness(
              &state, &mask, &ready) == APPLIANCE_SECRET_PERSISTENCE_OK &&
              mask ==
                  APPLIANCE_SECRET_PERSISTENCE_HTTPS_DEPENDENCY_READY &&
              ready,
          "https readiness publishes certificate dependency when pair exists");

    untouched_mask = 0x1234U;
    untouched_ready = true;
    state.version = 0U;
    CHECK(appliance_secret_persistence_https_readiness(
              &state, &untouched_mask, &untouched_ready) ==
              APPLIANCE_SECRET_PERSISTENCE_BAD_VERSION &&
              untouched_mask == 0x1234U && untouched_ready,
          "https readiness rejects bad version without mutation");
    CHECK(appliance_secret_persistence_https_readiness(
              NULL, &untouched_mask, &untouched_ready) ==
              APPLIANCE_SECRET_PERSISTENCE_NULL &&
              untouched_mask == 0x1234U && untouched_ready,
          "https readiness rejects null state without mutation");
    state.version = APPLIANCE_SECRET_PERSISTENCE_VERSION;
    CHECK(appliance_secret_persistence_https_readiness(
              &state, NULL, &untouched_ready) ==
              APPLIANCE_SECRET_PERSISTENCE_NULL,
          "https readiness rejects null mask output");
    CHECK(appliance_secret_persistence_https_readiness(
              &state, &untouched_mask, NULL) ==
              APPLIANCE_SECRET_PERSISTENCE_NULL,
          "https readiness rejects null ready output");
}

static const char *expected_kind_string(
    enum appliance_secret_persistence_record_kind kind)
{
    switch (kind) {
    case APPLIANCE_SECRET_PERSISTENCE_KIND_NONE:
        return "APPLIANCE_SECRET_PERSISTENCE_KIND_NONE";
    case APPLIANCE_SECRET_PERSISTENCE_KIND_CERTIFICATE_HTTPS_IDENTITY:
        return "APPLIANCE_SECRET_PERSISTENCE_KIND_CERTIFICATE_HTTPS_IDENTITY";
    case APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_ADMIN_CREDENTIAL:
        return "APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_ADMIN_CREDENTIAL";
    case APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_HTTPS_PRIVATE_KEY_DIGEST:
        return "APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_HTTPS_PRIVATE_KEY_DIGEST";
    case APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_HTTPS_PRIVATE_KEY_WRAPPED:
        return "APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_HTTPS_PRIVATE_KEY_WRAPPED";
    default:
        return NULL;
    }
}

static const char *expected_handling_string(
    enum appliance_secret_persistence_handling handling)
{
    switch (handling) {
    case APPLIANCE_SECRET_PERSISTENCE_HANDLING_NONE:
        return "APPLIANCE_SECRET_PERSISTENCE_HANDLING_NONE";
    case APPLIANCE_SECRET_PERSISTENCE_HANDLING_CERTIFICATE_RAW:
        return "APPLIANCE_SECRET_PERSISTENCE_HANDLING_CERTIFICATE_RAW";
    case APPLIANCE_SECRET_PERSISTENCE_HANDLING_FINGERPRINT_ONLY:
        return "APPLIANCE_SECRET_PERSISTENCE_HANDLING_FINGERPRINT_ONLY";
    case APPLIANCE_SECRET_PERSISTENCE_HANDLING_DIGEST_ONLY:
        return "APPLIANCE_SECRET_PERSISTENCE_HANDLING_DIGEST_ONLY";
    case APPLIANCE_SECRET_PERSISTENCE_HANDLING_WRAPPED_ONLY:
        return "APPLIANCE_SECRET_PERSISTENCE_HANDLING_WRAPPED_ONLY";
    case APPLIANCE_SECRET_PERSISTENCE_HANDLING_PBKDF2_VERIFIER:
        return "APPLIANCE_SECRET_PERSISTENCE_HANDLING_PBKDF2_VERIFIER";
    default:
        return NULL;
    }
}

static const char *expected_status_string(
    enum appliance_secret_persistence_status status)
{
    switch (status) {
    case APPLIANCE_SECRET_PERSISTENCE_OK:
        return "APPLIANCE_SECRET_PERSISTENCE_OK";
    case APPLIANCE_SECRET_PERSISTENCE_NULL:
        return "APPLIANCE_SECRET_PERSISTENCE_NULL";
    case APPLIANCE_SECRET_PERSISTENCE_OUT_OF_RANGE:
        return "APPLIANCE_SECRET_PERSISTENCE_OUT_OF_RANGE";
    case APPLIANCE_SECRET_PERSISTENCE_BAD_RECORD:
        return "APPLIANCE_SECRET_PERSISTENCE_BAD_RECORD";
    case APPLIANCE_SECRET_PERSISTENCE_BAD_VERSION:
        return "APPLIANCE_SECRET_PERSISTENCE_BAD_VERSION";
    case APPLIANCE_SECRET_PERSISTENCE_UNKNOWN_KEY:
        return "APPLIANCE_SECRET_PERSISTENCE_UNKNOWN_KEY";
    case APPLIANCE_SECRET_PERSISTENCE_BAD_ID:
        return "APPLIANCE_SECRET_PERSISTENCE_BAD_ID";
    case APPLIANCE_SECRET_PERSISTENCE_BAD_VALUE:
        return "APPLIANCE_SECRET_PERSISTENCE_BAD_VALUE";
    case APPLIANCE_SECRET_PERSISTENCE_BAD_STATE:
        return "APPLIANCE_SECRET_PERSISTENCE_BAD_STATE";
    default:
        return NULL;
    }
}

static void test_strings(void)
{
    enum appliance_secret_persistence_record_kind kind;
    enum appliance_secret_persistence_handling handling;
    enum appliance_secret_persistence_status status;

    for (kind = APPLIANCE_SECRET_PERSISTENCE_KIND_NONE;
         kind <=
             APPLIANCE_SECRET_PERSISTENCE_KIND_SECRET_HTTPS_PRIVATE_KEY_WRAPPED;
         ++kind) {
        const char *string =
            appliance_secret_persistence_record_kind_string(kind);
        const char *expected = expected_kind_string(kind);

        CHECK(string != NULL && expected != NULL &&
                  strcmp(string, expected) == 0,
              "record kind string is exact");
    }
    CHECK(appliance_secret_persistence_record_kind_string(
              (enum appliance_secret_persistence_record_kind)99U) == NULL,
          "undefined record kind string is rejected");

    for (handling = APPLIANCE_SECRET_PERSISTENCE_HANDLING_NONE;
         handling <= APPLIANCE_SECRET_PERSISTENCE_HANDLING_WRAPPED_ONLY;
         ++handling) {
        const char *string =
            appliance_secret_persistence_handling_string(handling);
        const char *expected = expected_handling_string(handling);

        CHECK(string != NULL && expected != NULL &&
                  strcmp(string, expected) == 0,
              "handling string is exact");
    }
    CHECK(appliance_secret_persistence_handling_string(
              (enum appliance_secret_persistence_handling)99U) == NULL,
          "undefined handling string is rejected");

    for (status = APPLIANCE_SECRET_PERSISTENCE_OK;
         status <= APPLIANCE_SECRET_PERSISTENCE_BAD_STATE;
         ++status) {
        const char *string =
            appliance_secret_persistence_status_string(status);
        const char *expected = expected_status_string(status);

        CHECK(string != NULL && expected != NULL &&
                  strcmp(string, expected) == 0,
              "status string is exact");
    }
    CHECK(appliance_secret_persistence_status_string(
              (enum appliance_secret_persistence_status)99U) == NULL,
          "undefined status string is rejected");
}

int main(void)
{
    test_catalogue_and_describe();
    test_classification();
    test_classification_rejections();
    test_value_policy();
    test_https_readiness();
    test_strings();
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "secret persistence performs no allocation");
    if (failures != 0) {
        fprintf(stderr,
                "Appliance secret persistence host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Appliance secret persistence host tests: ALL OK");
    return 0;
}

#endif
