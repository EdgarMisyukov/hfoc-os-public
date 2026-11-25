#include "trust_anchor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_TRUST_ANCHOR_HOST_TEST

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

static struct trust_anchor make_anchor(
    uint32_t id,
    uint32_t version,
    enum trust_anchor_algorithm algorithm_id,
    enum trust_anchor_state state)
{
    struct trust_anchor anchor;

    memset(&anchor, 0, sizeof(anchor));
    anchor.id = id;
    anchor.version = version;
    anchor.algorithm_id = algorithm_id;
    anchor.state = state;
    anchor.accepted_purposes = KEY_USE_ACCEPTED_PURPOSES_ALL;
    return anchor;
}

static void check_init_failure(const struct trust_anchor *anchor,
                               enum trust_anchor_status expected,
                               const char *message)
{
    struct trust_anchor published;
    unsigned char original[sizeof(published)];

    memset(&published, 0xA5, sizeof(published));
    memcpy(original, &published, sizeof(original));
    CHECK(trust_anchor_init(&published, anchor) == expected &&
              memcmp(&published, original, sizeof(original)) == 0,
          message);
}

static void test_status_strings(void) {
    enum trust_anchor_status status;
    enum trust_anchor_set_status set_status;

    for (status = TRUST_ANCHOR_OK;
         status <= TRUST_ANCHOR_INVALID_PURPOSES; ++status) {
        const char *string = trust_anchor_status_string(status);

        CHECK(string != NULL &&
                  (status == TRUST_ANCHOR_OK ?
                       strcmp(string, "TRUST_ANCHOR_OK") == 0 :
                   status == TRUST_ANCHOR_NULL ?
                       strcmp(string, "TRUST_ANCHOR_NULL") == 0 :
                   status == TRUST_ANCHOR_INVALID_ID ?
                       strcmp(string, "TRUST_ANCHOR_INVALID_ID") == 0 :
                   status == TRUST_ANCHOR_INVALID_VERSION ?
                       strcmp(string,
                              "TRUST_ANCHOR_INVALID_VERSION") == 0 :
                   status == TRUST_ANCHOR_INVALID_ALGORITHM ?
                       strcmp(string,
                              "TRUST_ANCHOR_INVALID_ALGORITHM") == 0 :
                   status == TRUST_ANCHOR_INVALID_STATE ?
                       strcmp(string,
                              "TRUST_ANCHOR_INVALID_STATE") == 0 :
                       strcmp(string,
                              "TRUST_ANCHOR_INVALID_PURPOSES") == 0),
              "trust anchor status string is exact for every status");
    }
    CHECK(trust_anchor_status_string(
              (enum trust_anchor_status)7U) == NULL,
          "trust anchor undefined status is rejected");

    for (set_status = TRUST_ANCHOR_SET_OK;
         set_status <= TRUST_ANCHOR_SET_DUPLICATE_ANCHOR; ++set_status) {
        const char *string = trust_anchor_set_status_string(set_status);

        CHECK(string != NULL &&
                  (set_status == TRUST_ANCHOR_SET_OK ?
                       strcmp(string, "TRUST_ANCHOR_SET_OK") == 0 :
                   set_status == TRUST_ANCHOR_SET_NULL ?
                       strcmp(string, "TRUST_ANCHOR_SET_NULL") == 0 :
                   set_status == TRUST_ANCHOR_SET_BAD_CAPACITY ?
                       strcmp(string,
                              "TRUST_ANCHOR_SET_BAD_CAPACITY") == 0 :
                   set_status == TRUST_ANCHOR_SET_INVALID_DESCRIPTOR ?
                       strcmp(string,
                              "TRUST_ANCHOR_SET_INVALID_DESCRIPTOR") == 0 :
                       strcmp(string,
                              "TRUST_ANCHOR_SET_DUPLICATE_ANCHOR") == 0),
              "trust anchor set status string is exact for every status");
    }
    CHECK(trust_anchor_set_status_string(
              (enum trust_anchor_set_status)5U) == NULL,
          "trust anchor undefined set status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "trust anchor status handling performs no allocation");
}

static void test_input_and_descriptor_failures(void) {
    struct trust_anchor anchor;
    struct trust_anchor set_anchor;

    CHECK(trust_anchor_validate(NULL) == TRUST_ANCHOR_NULL,
          "trust anchor validate rejects null anchor");
    CHECK(trust_anchor_init(NULL, NULL) == TRUST_ANCHOR_NULL,
          "trust anchor init rejects null published output");
    CHECK(trust_anchor_init(NULL, &anchor) == TRUST_ANCHOR_NULL,
          "trust anchor init rejects null anchor");
    CHECK(trust_anchor_set_validate(NULL, 0U) ==
              TRUST_ANCHOR_SET_OK,
          "trust anchor empty set validates");
    CHECK(trust_anchor_set_validate(NULL, 1U) ==
              TRUST_ANCHOR_SET_NULL,
          "trust anchor non-empty null set is rejected");
    CHECK(trust_anchor_set_validate(NULL,
              TRUST_ANCHOR_SET_MAX + 1U) ==
              TRUST_ANCHOR_SET_BAD_CAPACITY,
          "trust anchor over-limit set is rejected");

    anchor = make_anchor(0U, TRUST_ANCHOR_DESCRIPTOR_VERSION,
                         TRUST_ANCHOR_ALGORITHM_ED25519,
                         TRUST_ANCHOR_STATE_ACTIVE);
    check_init_failure(&anchor, TRUST_ANCHOR_INVALID_ID,
                       "zero trust anchor id is rejected without publication");

    anchor = make_anchor(TRUST_ANCHOR_ID_MAX + 1U,
                         TRUST_ANCHOR_DESCRIPTOR_VERSION,
                         TRUST_ANCHOR_ALGORITHM_ED25519,
                         TRUST_ANCHOR_STATE_ACTIVE);
    check_init_failure(&anchor, TRUST_ANCHOR_INVALID_ID,
                       "over-limit trust anchor id is rejected without publication");

    anchor = make_anchor(1U, 0U,
                         TRUST_ANCHOR_ALGORITHM_ED25519,
                         TRUST_ANCHOR_STATE_ACTIVE);
    check_init_failure(&anchor, TRUST_ANCHOR_INVALID_VERSION,
                       "zero trust anchor version is rejected");

    anchor = make_anchor(1U, TRUST_ANCHOR_DESCRIPTOR_VERSION + 1U,
                         TRUST_ANCHOR_ALGORITHM_ED25519,
                         TRUST_ANCHOR_STATE_ACTIVE);
    check_init_failure(&anchor, TRUST_ANCHOR_INVALID_VERSION,
                       "unknown trust anchor version is rejected");

    anchor = make_anchor(1U, TRUST_ANCHOR_DESCRIPTOR_VERSION,
                         (enum trust_anchor_algorithm)0U,
                         TRUST_ANCHOR_STATE_ACTIVE);
    check_init_failure(&anchor, TRUST_ANCHOR_INVALID_ALGORITHM,
                       "zero trust anchor algorithm id is rejected");

    anchor = make_anchor(1U, TRUST_ANCHOR_DESCRIPTOR_VERSION,
                         (enum trust_anchor_algorithm)
                             (TRUST_ANCHOR_ALGORITHM_ID_MAX + 1U),
                         TRUST_ANCHOR_STATE_ACTIVE);
    check_init_failure(&anchor, TRUST_ANCHOR_INVALID_ALGORITHM,
                       "over-limit trust anchor algorithm id is rejected");

    anchor = make_anchor(1U, TRUST_ANCHOR_DESCRIPTOR_VERSION,
                         TRUST_ANCHOR_ALGORITHM_ED25519,
                         (enum trust_anchor_state)3U);
    check_init_failure(&anchor, TRUST_ANCHOR_INVALID_STATE,
                       "invalid trust anchor lifecycle state is rejected");

    anchor = make_anchor(1U, TRUST_ANCHOR_DESCRIPTOR_VERSION,
                         TRUST_ANCHOR_ALGORITHM_ED25519,
                         TRUST_ANCHOR_STATE_ACTIVE);
    anchor.accepted_purposes = 0U;
    check_init_failure(&anchor, TRUST_ANCHOR_INVALID_PURPOSES,
                       "empty trust anchor accepted-purpose mask is rejected");

    anchor = make_anchor(1U, TRUST_ANCHOR_DESCRIPTOR_VERSION,
                         TRUST_ANCHOR_ALGORITHM_ED25519,
                         TRUST_ANCHOR_STATE_ACTIVE);
    anchor.accepted_purposes = 0x20U;
    check_init_failure(&anchor, TRUST_ANCHOR_INVALID_PURPOSES,
                       "unknown trust anchor accepted-purpose bit is rejected");

    anchor = make_anchor(1U, TRUST_ANCHOR_DESCRIPTOR_VERSION,
                         TRUST_ANCHOR_ALGORITHM_ED25519,
                         TRUST_ANCHOR_STATE_ACTIVE);
    anchor.accepted_purposes =
        KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_RECOVERY);
    CHECK(trust_anchor_validate(&anchor) == TRUST_ANCHOR_OK,
          "trust anchor validation accepts recovery-only mask");

    set_anchor = make_anchor(1U, TRUST_ANCHOR_DESCRIPTOR_VERSION,
                             (enum trust_anchor_algorithm)0U,
                             TRUST_ANCHOR_STATE_ACTIVE);
    CHECK(trust_anchor_set_validate(&set_anchor, 1U) ==
              TRUST_ANCHOR_SET_INVALID_DESCRIPTOR,
          "trust anchor set validation rejects malformed anchor");
    set_anchor = make_anchor(1U, TRUST_ANCHOR_DESCRIPTOR_VERSION,
                             TRUST_ANCHOR_ALGORITHM_ED25519,
                             TRUST_ANCHOR_STATE_ACTIVE);
    set_anchor.accepted_purposes = 0U;
    CHECK(trust_anchor_set_validate(&set_anchor, 1U) ==
              TRUST_ANCHOR_SET_INVALID_DESCRIPTOR,
          "trust anchor set validation rejects empty accepted-purpose mask");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "trust anchor input handling performs no allocation");
}

static void test_valid_descriptors_and_immutability(void) {
    struct trust_anchor anchor;
    struct trust_anchor published;
    struct trust_anchor source;
    struct trust_anchor expected;
    struct trust_anchor anchors[2];

    anchor = make_anchor(1U, TRUST_ANCHOR_DESCRIPTOR_VERSION,
                         TRUST_ANCHOR_ALGORITHM_ED25519,
                         TRUST_ANCHOR_STATE_INACTIVE);
    CHECK(trust_anchor_validate(&anchor) == TRUST_ANCHOR_OK &&
              trust_anchor_init(&published, &anchor) == TRUST_ANCHOR_OK &&
              published.id == 1U &&
              published.version == TRUST_ANCHOR_DESCRIPTOR_VERSION &&
              published.algorithm_id == TRUST_ANCHOR_ALGORITHM_ED25519 &&
              published.state == TRUST_ANCHOR_STATE_INACTIVE,
          "valid inactive trust anchor publishes exactly");

    anchor = make_anchor(2U, TRUST_ANCHOR_DESCRIPTOR_VERSION,
                         TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
                         TRUST_ANCHOR_STATE_ACTIVE);
    CHECK(trust_anchor_validate(&anchor) == TRUST_ANCHOR_OK &&
              trust_anchor_init(&published, &anchor) == TRUST_ANCHOR_OK &&
              published.algorithm_id == TRUST_ANCHOR_ALGORITHM_ECDSA_P256 &&
              published.state == TRUST_ANCHOR_STATE_ACTIVE,
          "valid active trust anchor publishes exact algorithm and state");

    anchor = make_anchor(3U, TRUST_ANCHOR_DESCRIPTOR_VERSION,
                         TRUST_ANCHOR_ALGORITHM_ED25519,
                         TRUST_ANCHOR_STATE_REVOKED);
    CHECK(trust_anchor_validate(&anchor) == TRUST_ANCHOR_OK &&
              trust_anchor_init(&published, &anchor) == TRUST_ANCHOR_OK &&
              published.state == TRUST_ANCHOR_STATE_REVOKED,
          "valid revoked trust anchor lifecycle state is accepted");

    source = make_anchor(4U, TRUST_ANCHOR_DESCRIPTOR_VERSION,
                         TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
                         TRUST_ANCHOR_STATE_ACTIVE);
    expected = source;
    CHECK(trust_anchor_init(&published, &source) == TRUST_ANCHOR_OK,
          "valid trust anchor initializes for immutable copy");
    source.id = 99U;
    source.version = 0U;
    source.algorithm_id = TRUST_ANCHOR_ALGORITHM_ED25519;
    source.state = TRUST_ANCHOR_STATE_INACTIVE;
    source.accepted_purposes = 0U;
    CHECK(memcmp(&published, &expected, sizeof(published)) == 0,
          "published trust anchor is an immutable exact copy");

    anchors[0] = make_anchor(1U, TRUST_ANCHOR_DESCRIPTOR_VERSION,
                             TRUST_ANCHOR_ALGORITHM_ED25519,
                             TRUST_ANCHOR_STATE_ACTIVE);
    anchors[1] = make_anchor(2U, TRUST_ANCHOR_DESCRIPTOR_VERSION,
                             TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
                             TRUST_ANCHOR_STATE_REVOKED);
    CHECK(trust_anchor_set_validate(anchors, 2U) ==
              TRUST_ANCHOR_SET_OK,
          "valid bounded trust anchor set validates");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "valid trust anchor publication performs no allocation");
}

static void test_duplicate_anchor_rejection(void) {
    struct trust_anchor anchors[2];

    anchors[0] = make_anchor(1U, TRUST_ANCHOR_DESCRIPTOR_VERSION,
                             TRUST_ANCHOR_ALGORITHM_ED25519,
                             TRUST_ANCHOR_STATE_ACTIVE);
    anchors[1] = make_anchor(1U, TRUST_ANCHOR_DESCRIPTOR_VERSION,
                             TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
                             TRUST_ANCHOR_STATE_INACTIVE);
    CHECK(trust_anchor_set_validate(anchors, 2U) ==
              TRUST_ANCHOR_SET_DUPLICATE_ANCHOR,
          "duplicate trust anchor id is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "duplicate trust anchor rejection performs no allocation");
}

int main(void) {
    test_status_strings();
    test_input_and_descriptor_failures();
    test_valid_descriptors_and_immutability();
    test_duplicate_anchor_rejection();

    if (failures != 0) {
        fprintf(stderr, "Trust anchor host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Trust anchor host tests: ALL OK\n");
    return 0;
}

#else
#error "This host test must be built with HFOC_TRUST_ANCHOR_HOST_TEST"
#endif
