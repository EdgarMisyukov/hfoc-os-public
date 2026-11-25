#include "credential_verifier.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_CREDENTIAL_VERIFIER_HOST_TEST

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

static struct credential_verifier make_verifier(
    uint32_t version,
    uint32_t principal_id,
    enum credential_verifier_state state,
    const uint8_t secret[CREDENTIAL_VERIFIER_SECRET_LENGTH])
{
    struct credential_verifier verifier;

    memset(&verifier, 0, sizeof(verifier));
    verifier.version = version;
    verifier.principal_id = principal_id;
    verifier.state = state;
    if (secret != NULL) {
        memcpy(verifier.expected_secret, secret,
               CREDENTIAL_VERIFIER_SECRET_LENGTH);
    }
    return verifier;
}

static struct credential_input make_input(
    uint32_t version,
    uint32_t principal_id,
    const uint8_t secret[CREDENTIAL_VERIFIER_SECRET_LENGTH])
{
    struct credential_input input;

    memset(&input, 0, sizeof(input));
    input.version = version;
    input.principal_id = principal_id;
    if (secret != NULL) {
        memcpy(input.secret, secret, CREDENTIAL_VERIFIER_SECRET_LENGTH);
    }
    return input;
}

static void test_status_strings(void) {
    enum credential_verifier_status status;
    static const uint8_t secret[CREDENTIAL_VERIFIER_SECRET_LENGTH] = {
        's', 'u', 'p', '3', 'r', 's', '3', 'c', 'r', '3', 't'
    };

    for (status = CREDENTIAL_VERIFIER_OK;
         status <= CREDENTIAL_VERIFIER_AUTHENTICATION_FAILED; ++status) {
        const char *string = credential_verifier_status_string(status);

        CHECK(string != NULL &&
                  (status == CREDENTIAL_VERIFIER_OK ?
                       strcmp(string, "CREDENTIAL_VERIFIER_OK") == 0 :
                   status == CREDENTIAL_VERIFIER_NULL ?
                       strcmp(string, "CREDENTIAL_VERIFIER_NULL") == 0 :
                   status == CREDENTIAL_VERIFIER_INVALID_VERSION ?
                       strcmp(string,
                              "CREDENTIAL_VERIFIER_INVALID_VERSION") == 0 :
                   status == CREDENTIAL_VERIFIER_INVALID_PRINCIPAL_ID ?
                       strcmp(string,
                              "CREDENTIAL_VERIFIER_INVALID_PRINCIPAL_ID") == 0 :
                   status == CREDENTIAL_VERIFIER_INVALID_STATE ?
                       strcmp(string,
                              "CREDENTIAL_VERIFIER_INVALID_STATE") == 0 :
                   status == CREDENTIAL_VERIFIER_DISABLED ?
                       strcmp(string, "CREDENTIAL_VERIFIER_DISABLED") == 0 :
                   status == CREDENTIAL_VERIFIER_PRINCIPAL_MISMATCH ?
                       strcmp(string,
                              "CREDENTIAL_VERIFIER_PRINCIPAL_MISMATCH") == 0 :
                       strcmp(string,
                              "CREDENTIAL_VERIFIER_AUTHENTICATION_FAILED") == 0),
              "credential verifier status string is exact for every status");
        CHECK(string == NULL || strstr(string, (const char *)secret) == NULL,
              "credential verifier status string never contains the secret");
    }
    CHECK(credential_verifier_status_string(
              (enum credential_verifier_status)8U) == NULL,
          "credential verifier undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "credential verifier status handling performs no allocation");
}

static void test_input_and_verifier_failures(void) {
    struct credential_verifier verifier;
    struct credential_input input;

    CHECK(credential_verifier_validate_verifier(NULL) ==
              CREDENTIAL_VERIFIER_NULL,
          "credential verifier validates null verifier as failure");
    CHECK(credential_verifier_validate_input(NULL) ==
              CREDENTIAL_VERIFIER_NULL,
          "credential verifier validates null input as failure");
    CHECK(credential_verifier_verify(NULL, NULL) ==
              CREDENTIAL_VERIFIER_NULL,
          "credential verifier verify rejects null inputs");

    verifier = make_verifier(CREDENTIAL_VERIFIER_VERSION + 1U, 1U,
                             CREDENTIAL_VERIFIER_STATE_ENABLED, NULL);
    CHECK(credential_verifier_validate_verifier(&verifier) ==
              CREDENTIAL_VERIFIER_INVALID_VERSION,
          "unknown verifier version is rejected");

    verifier = make_verifier(CREDENTIAL_VERIFIER_VERSION, 0U,
                             CREDENTIAL_VERIFIER_STATE_ENABLED, NULL);
    CHECK(credential_verifier_validate_verifier(&verifier) ==
              CREDENTIAL_VERIFIER_INVALID_PRINCIPAL_ID,
          "zero verifier principal id is rejected");

    verifier = make_verifier(CREDENTIAL_VERIFIER_VERSION,
                             CREDENTIAL_VERIFIER_ID_MAX + 1U,
                             CREDENTIAL_VERIFIER_STATE_ENABLED, NULL);
    CHECK(credential_verifier_validate_verifier(&verifier) ==
              CREDENTIAL_VERIFIER_INVALID_PRINCIPAL_ID,
          "over-limit verifier principal id is rejected");

    verifier = make_verifier(CREDENTIAL_VERIFIER_VERSION, 1U,
                             (enum credential_verifier_state)2U, NULL);
    CHECK(credential_verifier_validate_verifier(&verifier) ==
              CREDENTIAL_VERIFIER_INVALID_STATE,
          "invalid verifier state is rejected");

    input = make_input(CREDENTIAL_VERIFIER_VERSION + 1U, 1U, NULL);
    CHECK(credential_verifier_validate_input(&input) ==
              CREDENTIAL_VERIFIER_INVALID_VERSION,
          "unknown credential input version is rejected");

    input = make_input(CREDENTIAL_VERIFIER_VERSION, 0U, NULL);
    CHECK(credential_verifier_validate_input(&input) ==
              CREDENTIAL_VERIFIER_INVALID_PRINCIPAL_ID,
          "zero credential input principal id is rejected");

    input = make_input(CREDENTIAL_VERIFIER_VERSION,
                       CREDENTIAL_VERIFIER_ID_MAX + 1U, NULL);
    CHECK(credential_verifier_validate_input(&input) ==
              CREDENTIAL_VERIFIER_INVALID_PRINCIPAL_ID,
          "over-limit credential input principal id is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "credential verifier failure handling performs no allocation");
}

static void test_verification_states(void) {
    static const uint8_t secret_a[CREDENTIAL_VERIFIER_SECRET_LENGTH] = {
        'a', 'l', 'p', 'h', 'a'
    };
    static const uint8_t secret_b[CREDENTIAL_VERIFIER_SECRET_LENGTH] = {
        'b', 'r', 'a', 'v', 'o'
    };
    struct credential_verifier verifier;
    struct credential_input input;

    verifier = make_verifier(CREDENTIAL_VERIFIER_VERSION, 7U,
                             CREDENTIAL_VERIFIER_STATE_DISABLED, secret_a);
    input = make_input(CREDENTIAL_VERIFIER_VERSION, 7U, secret_a);
    CHECK(credential_verifier_validate_verifier(&verifier) ==
              CREDENTIAL_VERIFIER_OK &&
              credential_verifier_verify(&verifier, &input) ==
                  CREDENTIAL_VERIFIER_DISABLED,
          "disabled verifier is an explicit failure state");

    verifier = make_verifier(CREDENTIAL_VERIFIER_VERSION, 7U,
                             CREDENTIAL_VERIFIER_STATE_ENABLED, secret_a);
    input = make_input(CREDENTIAL_VERIFIER_VERSION, 8U, secret_a);
    CHECK(credential_verifier_verify(&verifier, &input) ==
              CREDENTIAL_VERIFIER_PRINCIPAL_MISMATCH,
          "principal mismatch is an explicit failure state");

    input = make_input(CREDENTIAL_VERIFIER_VERSION, 7U, secret_b);
    CHECK(credential_verifier_verify(&verifier, &input) ==
              CREDENTIAL_VERIFIER_AUTHENTICATION_FAILED,
          "secret mismatch is an explicit failure state");

    input = make_input(CREDENTIAL_VERIFIER_VERSION, 7U, secret_a);
    CHECK(credential_verifier_verify(&verifier, &input) ==
              CREDENTIAL_VERIFIER_OK,
          "enabled verifier accepts the exact fixed-length secret");

    verifier = make_verifier(CREDENTIAL_VERIFIER_VERSION, 9U,
                             CREDENTIAL_VERIFIER_STATE_ENABLED, NULL);
    input = make_input(CREDENTIAL_VERIFIER_VERSION, 9U, NULL);
    CHECK(credential_verifier_verify(&verifier, &input) ==
              CREDENTIAL_VERIFIER_OK,
          "all-zero fixed-length secret is a valid verifier and input");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "credential verifier verification performs no allocation");
}

int main(void) {
    test_status_strings();
    test_input_and_verifier_failures();
    test_verification_states();

    if (failures != 0) {
        fprintf(stderr, "Credential verifier host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Credential verifier host tests: ALL OK\n");
    return 0;
}

#else
#error "This host test must be built with HFOC_CREDENTIAL_VERIFIER_HOST_TEST"
#endif
