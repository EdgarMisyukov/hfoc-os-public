#include "appliance_secret_protection.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef APPLIANCE_SECRET_PROTECTION_HOST_TEST

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

static bool all_zero(const uint8_t *bytes, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static void fill_bytes(uint8_t *bytes, uint8_t value, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = value;
    }
}

struct fixture_material {
    uint8_t bytes[
        APPLIANCE_SECRET_PROTECTION_MAX_SECRET_LENGTH + 1U];
    struct appliance_secret_material material;
};

static struct fixture_material make_material(
    enum appliance_secret_protection_kind kind,
    uint8_t value,
    uint32_t length)
{
    struct fixture_material fixture;

    memset(&fixture, 0, sizeof(fixture));
    fill_bytes(fixture.bytes, value, length);
    fixture.material.version = APPLIANCE_SECRET_PROTECTION_VERSION;
    fixture.material.kind = kind;
    fixture.material.bytes = NULL;
    fixture.material.length = length;
    return fixture;
}

#define MAKE_MATERIAL(fixture, kind_value, byte_value, length_value) \
    struct fixture_material fixture = make_material( \
        kind_value, byte_value, length_value); \
    fixture.material.bytes = fixture.bytes

static struct appliance_secret_protection make_protection(void)
{
    struct appliance_secret_protection protection;

    memset(&protection, 0xA5, sizeof(protection));
    if (appliance_secret_protection_init(&protection) !=
        APPLIANCE_SECRET_PROTECTION_OK) {
        fprintf(stderr, "fixture failed secret protection init\n");
        __builtin_unreachable();
    }
    return protection;
}

static const char *expected_status_string(
    enum appliance_secret_protection_status status)
{
    switch (status) {
    case APPLIANCE_SECRET_PROTECTION_OK:
        return "APPLIANCE_SECRET_PROTECTION_OK";
    case APPLIANCE_SECRET_PROTECTION_NULL:
        return "APPLIANCE_SECRET_PROTECTION_NULL";
    case APPLIANCE_SECRET_PROTECTION_BAD_VERSION:
        return "APPLIANCE_SECRET_PROTECTION_BAD_VERSION";
    case APPLIANCE_SECRET_PROTECTION_BAD_STATE:
        return "APPLIANCE_SECRET_PROTECTION_BAD_STATE";
    case APPLIANCE_SECRET_PROTECTION_BAD_KIND:
        return "APPLIANCE_SECRET_PROTECTION_BAD_KIND";
    case APPLIANCE_SECRET_PROTECTION_BAD_LENGTH:
        return "APPLIANCE_SECRET_PROTECTION_BAD_LENGTH";
    case APPLIANCE_SECRET_PROTECTION_EMPTY_MATERIAL:
        return "APPLIANCE_SECRET_PROTECTION_EMPTY_MATERIAL";
    case APPLIANCE_SECRET_PROTECTION_ALL_ZERO_MATERIAL:
        return "APPLIANCE_SECRET_PROTECTION_ALL_ZERO_MATERIAL";
    case APPLIANCE_SECRET_PROTECTION_EMPTY:
        return "APPLIANCE_SECRET_PROTECTION_EMPTY";
    case APPLIANCE_SECRET_PROTECTION_ALREADY_PROTECTED:
        return "APPLIANCE_SECRET_PROTECTION_ALREADY_PROTECTED";
    case APPLIANCE_SECRET_PROTECTION_KIND_MISMATCH:
        return "APPLIANCE_SECRET_PROTECTION_KIND_MISMATCH";
    case APPLIANCE_SECRET_PROTECTION_GENERATION_EXHAUSTED:
        return "APPLIANCE_SECRET_PROTECTION_GENERATION_EXHAUSTED";
    case APPLIANCE_SECRET_PROTECTION_AT_REST_UNSUPPORTED:
        return "APPLIANCE_SECRET_PROTECTION_AT_REST_UNSUPPORTED";
    case APPLIANCE_SECRET_PROTECTION_VERIFY_FAILED:
        return "APPLIANCE_SECRET_PROTECTION_VERIFY_FAILED";
    default:
        return NULL;
    }
}

static const char *expected_kind_string(
    enum appliance_secret_protection_kind kind)
{
    switch (kind) {
    case APPLIANCE_SECRET_PROTECTION_KIND_NONE:
        return "APPLIANCE_SECRET_PROTECTION_KIND_NONE";
    case APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL:
        return "APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL";
    case APPLIANCE_SECRET_PROTECTION_KIND_BEARER_TOKEN:
        return "APPLIANCE_SECRET_PROTECTION_KIND_BEARER_TOKEN";
    case APPLIANCE_SECRET_PROTECTION_KIND_PRIVATE_KEY:
        return "APPLIANCE_SECRET_PROTECTION_KIND_PRIVATE_KEY";
    case APPLIANCE_SECRET_PROTECTION_KIND_OTHER_SECRET:
        return "APPLIANCE_SECRET_PROTECTION_KIND_OTHER_SECRET";
    default:
        return NULL;
    }
}

static const char *expected_state_string(
    enum appliance_secret_protection_state state)
{
    switch (state) {
    case APPLIANCE_SECRET_PROTECTION_STATE_EMPTY:
        return "APPLIANCE_SECRET_PROTECTION_STATE_EMPTY";
    case APPLIANCE_SECRET_PROTECTION_STATE_PROTECTED:
        return "APPLIANCE_SECRET_PROTECTION_STATE_PROTECTED";
    default:
        return NULL;
    }
}

static void test_strings(void)
{
    enum appliance_secret_protection_status status;
    enum appliance_secret_protection_kind kind;
    enum appliance_secret_protection_state state;

    for (status = APPLIANCE_SECRET_PROTECTION_OK;
         status <= APPLIANCE_SECRET_PROTECTION_VERIFY_FAILED;
         ++status) {
        const char *string =
            appliance_secret_protection_status_string(status);
        const char *expected = expected_status_string(status);

        CHECK(string != NULL && expected != NULL &&
                  strcmp(string, expected) == 0,
              "secret protection status string is exact");
    }
    CHECK(appliance_secret_protection_status_string(
              (enum appliance_secret_protection_status)14U) == NULL,
          "secret protection undefined status string is rejected");

    for (kind = APPLIANCE_SECRET_PROTECTION_KIND_NONE;
         kind <= APPLIANCE_SECRET_PROTECTION_KIND_OTHER_SECRET;
         ++kind) {
        const char *string =
            appliance_secret_protection_kind_string(kind);
        const char *expected = expected_kind_string(kind);

        CHECK(string != NULL && expected != NULL &&
                  strcmp(string, expected) == 0,
              "secret protection kind string is exact");
    }
    CHECK(appliance_secret_protection_kind_string(
              (enum appliance_secret_protection_kind)5U) == NULL,
          "secret protection undefined kind string is rejected");

    for (state = APPLIANCE_SECRET_PROTECTION_STATE_EMPTY;
         state <= APPLIANCE_SECRET_PROTECTION_STATE_PROTECTED;
         ++state) {
        const char *string =
            appliance_secret_protection_state_string(state);
        const char *expected = expected_state_string(state);

        CHECK(string != NULL && expected != NULL &&
                  strcmp(string, expected) == 0,
              "secret protection state string is exact");
    }
    CHECK(appliance_secret_protection_state_string(
              (enum appliance_secret_protection_state)2U) == NULL,
          "secret protection undefined state string is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "secret protection string handling performs no allocation");
}

static void test_init_and_lifecycle(void)
{
    struct appliance_secret_protection protection = make_protection();
    MAKE_MATERIAL(
        credential,
        APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL,
        0x42U,
        APPLIANCE_SECRET_PROTECTION_CREDENTIAL_LENGTH);
    MAKE_MATERIAL(
        rotated,
        APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL,
        0x43U,
        APPLIANCE_SECRET_PROTECTION_CREDENTIAL_LENGTH);
    MAKE_MATERIAL(
        token,
        APPLIANCE_SECRET_PROTECTION_KIND_BEARER_TOKEN,
        0x44U,
        APPLIANCE_SECRET_PROTECTION_TOKEN_LENGTH);
    MAKE_MATERIAL(
        empty,
        APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL,
        0x45U,
        0U);
    MAKE_MATERIAL(
        zero_credential,
        APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL,
        0x00U,
        APPLIANCE_SECRET_PROTECTION_CREDENTIAL_LENGTH);
    MAKE_MATERIAL(
        short_material,
        APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL,
        0x46U,
        APPLIANCE_SECRET_PROTECTION_CREDENTIAL_LENGTH - 1U);
    MAKE_MATERIAL(
        long_material,
        APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL,
        0x47U,
        APPLIANCE_SECRET_PROTECTION_CREDENTIAL_LENGTH + 1U);
    struct appliance_secret_protection before;

    credential.material.version = APPLIANCE_SECRET_PROTECTION_VERSION + 1U;
    CHECK(appliance_secret_protection_protect(
              &protection, &credential.material) ==
              APPLIANCE_SECRET_PROTECTION_BAD_VERSION,
          "protect rejects material version mismatch without mutation");
    credential.material.version = APPLIANCE_SECRET_PROTECTION_VERSION;

    CHECK(appliance_secret_protection_init(NULL) ==
              APPLIANCE_SECRET_PROTECTION_NULL,
          "secret protection init rejects null protection");
    CHECK(appliance_secret_protection_protect(NULL,
                                               &credential.material) ==
              APPLIANCE_SECRET_PROTECTION_NULL,
          "secret protection rejects null protection");
    CHECK(appliance_secret_protection_protect(
              &protection, NULL) == APPLIANCE_SECRET_PROTECTION_NULL,
          "secret protection rejects null material");
    CHECK(protection.version == APPLIANCE_SECRET_PROTECTION_VERSION &&
              protection.state == APPLIANCE_SECRET_PROTECTION_STATE_EMPTY &&
              protection.kind == APPLIANCE_SECRET_PROTECTION_KIND_NONE &&
              protection.generation == 0U &&
              !protection.at_rest_supported &&
              !protection.material_retained &&
              protection.retained_length == 0U &&
              all_zero(protection.fingerprint,
                       sizeof(protection.fingerprint)) &&
              all_zero(protection.retained_material,
                       sizeof(protection.retained_material)),
          "secret protection init publishes exact empty state");

    credential.material.kind =
        (enum appliance_secret_protection_kind)
            APPLIANCE_SECRET_PROTECTION_KIND_OTHER_SECRET + 1U;
    CHECK(appliance_secret_protection_protect(
              &protection, &credential.material) ==
              APPLIANCE_SECRET_PROTECTION_BAD_KIND,
          "protect rejects unknown secret kind");
    credential.material.kind =
        APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL;
    credential.material.bytes = NULL;
    CHECK(appliance_secret_protection_protect(
              &protection, &credential.material) ==
              APPLIANCE_SECRET_PROTECTION_NULL,
          "protect rejects null material bytes");
    credential.material.bytes = credential.bytes;

    CHECK(appliance_secret_protection_protect(
              &protection, &empty.material) ==
              APPLIANCE_SECRET_PROTECTION_EMPTY_MATERIAL,
          "protect rejects empty material");
    CHECK(appliance_secret_protection_protect(
              &protection, &zero_credential.material) ==
              APPLIANCE_SECRET_PROTECTION_ALL_ZERO_MATERIAL,
          "protect rejects all-zero material");
    CHECK(appliance_secret_protection_protect(
              &protection, &short_material.material) ==
              APPLIANCE_SECRET_PROTECTION_BAD_LENGTH,
          "protect rejects short administrative credential");
    CHECK(appliance_secret_protection_protect(
              &protection, &long_material.material) ==
              APPLIANCE_SECRET_PROTECTION_BAD_LENGTH,
          "protect rejects long administrative credential");
    token.material.length =
        APPLIANCE_SECRET_PROTECTION_TOKEN_LENGTH - 1U;
    CHECK(appliance_secret_protection_protect(
              &protection, &token.material) ==
              APPLIANCE_SECRET_PROTECTION_BAD_LENGTH,
          "protect rejects short bearer token");
    token.material.length = APPLIANCE_SECRET_PROTECTION_TOKEN_LENGTH;

    CHECK(appliance_secret_protection_protect(
              &protection, &credential.material) ==
              APPLIANCE_SECRET_PROTECTION_OK &&
              protection.state == APPLIANCE_SECRET_PROTECTION_STATE_PROTECTED &&
              protection.kind ==
                  APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL &&
              protection.generation == 1U &&
              protection.at_rest_supported &&
              !protection.material_retained &&
              protection.retained_length == 0U &&
              !all_zero(protection.fingerprint,
                        sizeof(protection.fingerprint)),
          "credential protect publishes digest-only protected state");

    before = protection;
    CHECK(appliance_secret_protection_protect(
              &protection, &rotated.material) ==
              APPLIANCE_SECRET_PROTECTION_ALREADY_PROTECTED &&
              memcmp(&protection, &before, sizeof(protection)) == 0,
          "protect rejects duplicate publication without mutation");

    CHECK(appliance_secret_protection_rotate(
              &protection, &token.material) ==
              APPLIANCE_SECRET_PROTECTION_KIND_MISMATCH &&
              memcmp(&protection, &before, sizeof(protection)) == 0,
          "rotate rejects kind mismatch without mutation");
    CHECK(appliance_secret_protection_rotate(
              &protection, &rotated.material) ==
              APPLIANCE_SECRET_PROTECTION_OK &&
              protection.state == APPLIANCE_SECRET_PROTECTION_STATE_PROTECTED &&
              protection.generation == 2U &&
              memcmp(protection.fingerprint,
                     before.fingerprint,
                     sizeof(protection.fingerprint)) != 0,
          "credential rotation advances generation and changes fingerprint");

    CHECK(appliance_secret_protection_reset(&protection) ==
              APPLIANCE_SECRET_PROTECTION_OK &&
              protection.state == APPLIANCE_SECRET_PROTECTION_STATE_EMPTY &&
              protection.kind == APPLIANCE_SECRET_PROTECTION_KIND_NONE &&
              protection.generation == 2U &&
              !protection.at_rest_supported &&
              !protection.material_retained &&
              protection.retained_length == 0U &&
              all_zero(protection.fingerprint,
                       sizeof(protection.fingerprint)),
          "reset clears credential without changing generation");
    CHECK(appliance_secret_protection_reset(&protection) ==
              APPLIANCE_SECRET_PROTECTION_EMPTY,
          "reset rejects empty protection slot");

    credential.material.version = APPLIANCE_SECRET_PROTECTION_VERSION;
    CHECK(appliance_secret_protection_protect(
              &protection, &credential.material) ==
              APPLIANCE_SECRET_PROTECTION_OK &&
              protection.generation == 3U,
          "protect after reset starts a fresh generation");
}

static void test_at_rest_and_verify(void)
{
    struct appliance_secret_protection protection = make_protection();
    MAKE_MATERIAL(
        credential,
        APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL,
        0x52U,
        APPLIANCE_SECRET_PROTECTION_CREDENTIAL_LENGTH);
    MAKE_MATERIAL(
        wrong,
        APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL,
        0x53U,
        APPLIANCE_SECRET_PROTECTION_CREDENTIAL_LENGTH);
    MAKE_MATERIAL(
        token,
        APPLIANCE_SECRET_PROTECTION_KIND_BEARER_TOKEN,
        0x54U,
        APPLIANCE_SECRET_PROTECTION_TOKEN_LENGTH);
    MAKE_MATERIAL(
        private_key,
        APPLIANCE_SECRET_PROTECTION_KIND_PRIVATE_KEY,
        0x55U,
        121U);
    struct appliance_secret_protection_at_rest_record record;
    struct appliance_secret_protection_at_rest_record untouched;

    memset(&record, 0xA5, sizeof(record));
    untouched = record;
    CHECK(appliance_secret_protection_export_at_rest(
              &protection, &record) == APPLIANCE_SECRET_PROTECTION_EMPTY &&
              memcmp(&record, &untouched, sizeof(record)) == 0,
          "at-rest export rejects empty slot without mutation");
    CHECK(appliance_secret_protection_export_at_rest(
              &protection, NULL) == APPLIANCE_SECRET_PROTECTION_NULL,
          "at-rest export rejects null record");
    CHECK(appliance_secret_protection_verify(
              &protection, &credential.material) ==
              APPLIANCE_SECRET_PROTECTION_EMPTY,
          "verify rejects empty slot");
    CHECK(appliance_secret_protection_verify(
              NULL, &credential.material) ==
              APPLIANCE_SECRET_PROTECTION_NULL,
          "verify rejects null protection");
    CHECK(appliance_secret_protection_verify(
              &protection, NULL) == APPLIANCE_SECRET_PROTECTION_NULL,
          "verify rejects null material");

    CHECK(appliance_secret_protection_protect(
              &protection, &credential.material) ==
              APPLIANCE_SECRET_PROTECTION_OK &&
              appliance_secret_protection_verify(
                  &protection, &credential.material) ==
                  APPLIANCE_SECRET_PROTECTION_OK,
          "correct credential material verifies against protected slot");
    CHECK(appliance_secret_protection_verify(
              &protection, &wrong.material) ==
              APPLIANCE_SECRET_PROTECTION_VERIFY_FAILED,
          "wrong credential material fails closed");
    CHECK(appliance_secret_protection_verify(
              &protection, &private_key.material) ==
              APPLIANCE_SECRET_PROTECTION_KIND_MISMATCH,
          "credential slot rejects private-key material kind");

    CHECK(appliance_secret_protection_export_at_rest(
              &protection, &record) == APPLIANCE_SECRET_PROTECTION_OK &&
              record.version == APPLIANCE_SECRET_PROTECTION_VERSION &&
              record.kind ==
                  APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL &&
              record.generation == protection.generation &&
              memcmp(record.fingerprint,
                     protection.fingerprint,
                     sizeof(record.fingerprint)) == 0 &&
              all_zero(record.reserved, sizeof(record.reserved)),
          "credential at-rest record contains exact fingerprint metadata");
    CHECK(memcmp(record.fingerprint,
                 credential.bytes,
                 sizeof(record.fingerprint)) != 0,
          "credential at-rest record does not contain raw credential bytes");

    protection = make_protection();
    CHECK(appliance_secret_protection_protect(
              &protection, &token.material) ==
              APPLIANCE_SECRET_PROTECTION_OK &&
              !protection.at_rest_supported &&
              protection.material_retained &&
              memcmp(protection.retained_material,
                     token.bytes,
                     token.material.length) == 0,
          "bearer token is retained only in the volatile slot");
    memset(&record, 0xA5, sizeof(record));
    untouched = record;
    CHECK(appliance_secret_protection_export_at_rest(
              &protection, &record) ==
              APPLIANCE_SECRET_PROTECTION_AT_REST_UNSUPPORTED &&
              memcmp(&record, &untouched, sizeof(record)) == 0,
          "bearer token at-rest export is denied without mutation");
    CHECK(appliance_secret_protection_verify(
              &protection, &token.material) ==
              APPLIANCE_SECRET_PROTECTION_OK,
          "bearer token verifies while retained volatile");

    protection = make_protection();
    CHECK(appliance_secret_protection_protect(
              &protection, &private_key.material) ==
              APPLIANCE_SECRET_PROTECTION_OK &&
              !protection.at_rest_supported &&
              protection.material_retained &&
              memcmp(protection.retained_material,
                     private_key.bytes,
                     private_key.material.length) == 0,
          "private key is retained only in the volatile slot");
    memset(&record, 0xA5, sizeof(record));
    untouched = record;
    CHECK(appliance_secret_protection_export_at_rest(
              &protection, &record) ==
              APPLIANCE_SECRET_PROTECTION_AT_REST_UNSUPPORTED &&
              memcmp(&record, &untouched, sizeof(record)) == 0,
          "private-key at-rest export is denied until ITEM-325");
}

static void test_volatile_rotation_zeroization_and_exhaustion(void)
{
    struct appliance_secret_protection protection = make_protection();
    MAKE_MATERIAL(
        token,
        APPLIANCE_SECRET_PROTECTION_KIND_BEARER_TOKEN,
        0x61U,
        APPLIANCE_SECRET_PROTECTION_TOKEN_LENGTH);
    MAKE_MATERIAL(
        replacement,
        APPLIANCE_SECRET_PROTECTION_KIND_BEARER_TOKEN,
        0x62U,
        APPLIANCE_SECRET_PROTECTION_TOKEN_LENGTH);
    MAKE_MATERIAL(
        private_key,
        APPLIANCE_SECRET_PROTECTION_KIND_PRIVATE_KEY,
        0x63U,
        121U);
    MAKE_MATERIAL(
        zero_key,
        APPLIANCE_SECRET_PROTECTION_KIND_PRIVATE_KEY,
        0x00U,
        121U);
    struct appliance_secret_protection before;

    CHECK(appliance_secret_protection_protect(
              &protection, &zero_key.material) ==
              APPLIANCE_SECRET_PROTECTION_ALL_ZERO_MATERIAL,
          "private-key all-zero material is rejected");
    CHECK(appliance_secret_protection_protect(
              &protection, &private_key.material) ==
              APPLIANCE_SECRET_PROTECTION_OK &&
              protection.retained_length == private_key.material.length,
          "private key protects into bounded volatile slot");

    CHECK(appliance_secret_protection_rotate(
              &protection, &token.material) ==
              APPLIANCE_SECRET_PROTECTION_KIND_MISMATCH,
          "private-key slot rejects bearer-token rotation");
    protection = make_protection();
    CHECK(appliance_secret_protection_protect(
              &protection, &token.material) ==
              APPLIANCE_SECRET_PROTECTION_OK,
          "bearer token protects into bounded volatile slot");

    CHECK(appliance_secret_protection_rotate(
              &protection, &replacement.material) ==
              APPLIANCE_SECRET_PROTECTION_OK &&
              protection.retained_length == replacement.material.length &&
              memcmp(protection.retained_material,
                     replacement.bytes,
                     replacement.material.length) == 0,
          "token rotation publishes replacement material");
    CHECK(all_zero(protection.retained_material +
                       replacement.material.length,
                   sizeof(protection.retained_material) -
                       replacement.material.length),
          "token rotation leaves no old bytes after retained replacement");

    before = protection;
    CHECK(appliance_secret_protection_reset(&protection) ==
              APPLIANCE_SECRET_PROTECTION_OK &&
              all_zero(protection.retained_material,
                       sizeof(protection.retained_material)) &&
              protection.retained_length == 0U &&
              all_zero(protection.fingerprint,
                       sizeof(protection.fingerprint)),
          "token reset zeroizes all retained bytes");

    protection = make_protection();
    protection.generation = UINT32_MAX;
    CHECK(appliance_secret_protection_protect(
              &protection, &token.material) ==
              APPLIANCE_SECRET_PROTECTION_GENERATION_EXHAUSTED &&
              protection.state == APPLIANCE_SECRET_PROTECTION_STATE_EMPTY,
          "generation exhaustion rejects initial protect");

    protection = make_protection();
    CHECK(appliance_secret_protection_protect(
              &protection, &token.material) ==
              APPLIANCE_SECRET_PROTECTION_OK,
          "token protects before generation exhaustion fixture");
    protection.generation = UINT32_MAX;
    before = protection;
    CHECK(appliance_secret_protection_rotate(
              &protection, &replacement.material) ==
              APPLIANCE_SECRET_PROTECTION_GENERATION_EXHAUSTED &&
              memcmp(&protection, &before, sizeof(protection)) == 0,
          "generation exhaustion preserves old protected secret");
}

int main(void)
{
    alloc_attempts = 0U;
    free_attempts = 0U;

    test_strings();
    test_init_and_lifecycle();
    test_at_rest_and_verify();
    test_volatile_rotation_zeroization_and_exhaustion();

    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "secret protection host fixture performs no allocation");

    if (failures != 0) {
        fprintf(stderr,
                "Secret protection host tests: %d failures\n",
                failures);
        return 1;
    }
    printf("Secret protection host tests: ALL OK\n");
    return 0;
}

#else

typedef int appliance_secret_protection_host_test_guard;

#endif
