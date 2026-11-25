#include "trust_rotation.h"

#include "signed_metadata.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_TRUST_ROTATION_HOST_TEST

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

static struct trust_anchor make_anchor(
    uint32_t id,
    enum trust_anchor_algorithm algorithm_id,
    enum trust_anchor_state state,
    uint32_t accepted_purposes)
{
    struct trust_anchor anchor;

    memset(&anchor, 0, sizeof(anchor));
    anchor.id = id;
    anchor.version = TRUST_ANCHOR_DESCRIPTOR_VERSION;
    anchor.algorithm_id = algorithm_id;
    anchor.state = state;
    anchor.accepted_purposes = accepted_purposes;
    return anchor;
}

static struct verification_policy_snapshot make_snapshot(
    uint32_t generation,
    const struct trust_anchor *anchors,
    uint32_t anchor_count)
{
    struct verification_policy_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = VERIFICATION_POLICY_VERSION;
    snapshot.generation = generation;
    snapshot.anchor_count = anchor_count;
    if (anchor_count > 0U && anchors != NULL) {
        memcpy(snapshot.anchors, anchors,
               anchor_count * sizeof(snapshot.anchors[0]));
    }
    return snapshot;
}

static void init_policy(
    struct verification_policy *policy,
    uint32_t generation,
    const struct trust_anchor *anchors,
    uint32_t anchor_count)
{
    struct verification_policy_snapshot snapshot;

    snapshot = make_snapshot(generation, anchors, anchor_count);
    memset(policy, 0, sizeof(*policy));
    CHECK(verification_policy_init(policy, &snapshot) ==
              VERIFICATION_POLICY_OK,
          "trust rotation fixture initializes active policy");
}

static bool snapshot_equal(
    const struct verification_policy_snapshot *left,
    const struct verification_policy_snapshot *right)
{
    return left != NULL && right != NULL &&
           memcmp(left, right, sizeof(*left)) == 0;
}

static bool active_equal(
    const struct verification_policy *policy,
    const struct verification_policy_snapshot *expected)
{
    return snapshot_equal(verification_policy_active(policy), expected);
}

static void check_seal_behavior(
    const struct verification_policy *policy,
    uint32_t signer_id,
    enum signed_metadata_status expected,
    const char *message)
{
    unsigned char buffer[256];
    const unsigned char metadata[] = "rotation behavior";
    size_t length = 0U;

    memset(buffer, 0, sizeof(buffer));
    CHECK(signed_metadata_seal(
              policy, KEY_USE_PURPOSE_IMAGE, signer_id,
              metadata, sizeof(metadata),
              buffer, sizeof(buffer), &length) == expected,
          message);
}

static void test_status_strings(void)
{
    static const struct {
        enum trust_rotation_status status;
        const char *string;
    } statuses[] = {
        {TRUST_ROTATION_OK, "TRUST_ROTATION_OK"},
        {TRUST_ROTATION_NULL, "TRUST_ROTATION_NULL"},
        {TRUST_ROTATION_BAD_STATE, "TRUST_ROTATION_BAD_STATE"},
        {TRUST_ROTATION_NO_STAGED, "TRUST_ROTATION_NO_STAGED"},
        {TRUST_ROTATION_NO_PREPARED, "TRUST_ROTATION_NO_PREPARED"},
        {TRUST_ROTATION_NO_POLICY, "TRUST_ROTATION_NO_POLICY"},
        {TRUST_ROTATION_INVALID_ANCHOR,
         "TRUST_ROTATION_INVALID_ANCHOR"},
        {TRUST_ROTATION_NON_INACTIVE_ANCHOR,
         "TRUST_ROTATION_NON_INACTIVE_ANCHOR"},
        {TRUST_ROTATION_DUPLICATE_ANCHOR,
         "TRUST_ROTATION_DUPLICATE_ANCHOR"},
        {TRUST_ROTATION_POLICY_FULL, "TRUST_ROTATION_POLICY_FULL"},
        {TRUST_ROTATION_GENERATION_OVERFLOW,
         "TRUST_ROTATION_GENERATION_OVERFLOW"},
        {TRUST_ROTATION_STAGE_FAILED, "TRUST_ROTATION_STAGE_FAILED"},
        {TRUST_ROTATION_COMMIT_FAILED,
         "TRUST_ROTATION_COMMIT_FAILED"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = trust_rotation_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "trust rotation status string is exact");
    }
    CHECK(trust_rotation_status_string(
              (enum trust_rotation_status)13U) == NULL,
          "trust rotation undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "trust rotation status handling performs no allocation");
}

static void test_null_and_uninitialized(void)
{
    struct trust_anchor anchor;
    struct verification_policy policy;
    struct trust_rotation rotation;
    struct trust_rotation zeroed;

    anchor = make_anchor(
        1U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_INACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    memset(&policy, 0, sizeof(policy));
    memset(&zeroed, 0, sizeof(zeroed));

    CHECK(trust_rotation_init(NULL) == TRUST_ROTATION_NULL,
          "trust rotation init rejects null rotation");
    CHECK(trust_rotation_stage(NULL, &policy, &anchor) ==
              TRUST_ROTATION_NULL,
          "trust rotation stage rejects null rotation");
    CHECK(trust_rotation_stage(&zeroed, &policy, &anchor) ==
              TRUST_ROTATION_BAD_STATE,
          "trust rotation stage rejects uninitialized rotation");
    CHECK(trust_rotation_prepare(&zeroed, &policy) ==
              TRUST_ROTATION_BAD_STATE,
          "trust rotation prepare rejects uninitialized rotation");
    CHECK(trust_rotation_activate(&zeroed, &policy) ==
              TRUST_ROTATION_BAD_STATE,
          "trust rotation activate rejects uninitialized rotation");
    CHECK(trust_rotation_rollback(&zeroed, &policy) ==
              TRUST_ROTATION_BAD_STATE,
          "trust rotation rollback rejects uninitialized rotation");
    CHECK(trust_rotation_has_staged(NULL) == false &&
              trust_rotation_has_prepared(NULL) == false &&
              trust_rotation_staged(NULL) == NULL &&
              trust_rotation_prepared(NULL) == NULL,
          "trust rotation accessors reject null rotation");

    memset(&rotation, 0xA5, sizeof(rotation));
    CHECK(trust_rotation_init(&rotation) == TRUST_ROTATION_OK &&
              rotation.version == TRUST_ROTATION_VERSION &&
              !trust_rotation_has_staged(&rotation) &&
              !trust_rotation_has_prepared(&rotation),
          "trust rotation init clears transaction state");
    CHECK(trust_rotation_prepare(&rotation, &policy) ==
              TRUST_ROTATION_NO_POLICY,
          "trust rotation prepare rejects unpublished policy");
    CHECK(trust_rotation_activate(&rotation, &policy) ==
              TRUST_ROTATION_NO_POLICY,
          "trust rotation activate rejects unpublished policy");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "trust rotation null handling performs no allocation");
}

static void test_stage_failures(void)
{
    struct trust_anchor anchors[2];
    struct verification_policy_snapshot initial;
    struct verification_policy policy;
    struct trust_rotation rotation;
    struct trust_anchor invalid;
    struct trust_anchor duplicate;

    anchors[0] = make_anchor(
        1U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    anchors[1] = make_anchor(
        2U, TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
        TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    initial = make_snapshot(1U, anchors, 2U);
    init_policy(&policy, 1U, anchors, 2U);
    CHECK(trust_rotation_init(&rotation) == TRUST_ROTATION_OK,
          "stage failure fixture initializes rotation");

    invalid = make_anchor(
        3U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_INACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    invalid.id = 0U;
    CHECK(trust_rotation_stage(&rotation, &policy, &invalid) ==
              TRUST_ROTATION_INVALID_ANCHOR &&
              active_equal(&policy, &initial) &&
              !trust_rotation_has_staged(&rotation),
          "trust rotation rejects zero anchor id");

    invalid = make_anchor(
        3U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_INACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    invalid.version = 0U;
    CHECK(trust_rotation_stage(&rotation, &policy, &invalid) ==
              TRUST_ROTATION_INVALID_ANCHOR &&
              active_equal(&policy, &initial),
          "trust rotation rejects invalid anchor version");

    invalid = make_anchor(
        3U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_INACTIVE, 0U);
    CHECK(trust_rotation_stage(&rotation, &policy, &invalid) ==
              TRUST_ROTATION_INVALID_ANCHOR &&
              active_equal(&policy, &initial),
          "trust rotation rejects empty accepted-purpose mask");

    invalid = make_anchor(
        3U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    CHECK(trust_rotation_stage(&rotation, &policy, &invalid) ==
              TRUST_ROTATION_NON_INACTIVE_ANCHOR &&
              active_equal(&policy, &initial),
          "trust rotation rejects active anchor for staging");

    invalid = make_anchor(
        3U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_REVOKED, KEY_USE_ACCEPTED_PURPOSES_ALL);
    CHECK(trust_rotation_stage(&rotation, &policy, &invalid) ==
              TRUST_ROTATION_NON_INACTIVE_ANCHOR &&
              active_equal(&policy, &initial),
          "trust rotation rejects revoked anchor for staging");

    duplicate = make_anchor(
        1U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_INACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    CHECK(trust_rotation_stage(
              &rotation, &policy, &duplicate) ==
              TRUST_ROTATION_DUPLICATE_ANCHOR &&
              active_equal(&policy, &initial),
          "trust rotation rejects duplicate active anchor id");

    CHECK(trust_rotation_stage(
              &rotation, &policy, &invalid) ==
              TRUST_ROTATION_NON_INACTIVE_ANCHOR,
          "repeated failed stage leaves rotation clean");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "trust rotation stage failures perform no allocation");
}

static void test_valid_stage_and_interrupted_prepare(void)
{
    struct trust_anchor anchors[2];
    struct verification_policy_snapshot initial;
    struct verification_policy policy;
    struct trust_rotation rotation;
    struct trust_anchor staged;
    struct trust_anchor expected;
    unsigned char buffer[256];
    struct signed_metadata_layout layout;
    const unsigned char metadata[] = "rotation verification";
    size_t length = 0U;

    anchors[0] = make_anchor(
        1U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    anchors[1] = make_anchor(
        2U, TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
        TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    initial = make_snapshot(1U, anchors, 2U);
    init_policy(&policy, 1U, anchors, 2U);
    CHECK(trust_rotation_init(&rotation) == TRUST_ROTATION_OK,
          "interrupted-rotation fixture initializes rotation");

    staged = make_anchor(
        3U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_INACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    expected = staged;
    CHECK(trust_rotation_stage(&rotation, &policy, &staged) ==
              TRUST_ROTATION_OK &&
              trust_rotation_has_staged(&rotation) &&
              memcmp(trust_rotation_staged(&rotation), &expected,
                     sizeof(expected)) == 0 &&
              active_equal(&policy, &initial),
          "valid rotation stage stores exact inactive anchor");
    CHECK(trust_rotation_stage(
              &rotation, &policy, &staged) ==
              TRUST_ROTATION_BAD_STATE,
          "rotation stage rejects second staged anchor");

    memset(buffer, 0, sizeof(buffer));
    CHECK(signed_metadata_seal(
              &policy, KEY_USE_PURPOSE_IMAGE, 1U,
              metadata, sizeof(metadata),
              buffer, sizeof(buffer), &length) == SIGNED_METADATA_OK &&
              signed_metadata_verify(
                  &policy, KEY_USE_PURPOSE_IMAGE,
                  buffer, length, &layout) == SIGNED_METADATA_OK &&
              layout.signer_id == 1U,
          "existing active signer verifies before rotation prepare");
    check_seal_behavior(
        &policy, 3U, SIGNED_METADATA_UNKNOWN_SIGNER,
        "staged inactive anchor cannot sign before activation");

    CHECK(trust_rotation_prepare(&rotation, &policy) ==
              TRUST_ROTATION_OK &&
              trust_rotation_has_prepared(&rotation) &&
              verification_policy_has_pending(&policy) &&
              active_equal(&policy, &initial),
          "interrupted rotation prepare preserves active policy");
    CHECK(trust_rotation_prepare(&rotation, &policy) ==
              TRUST_ROTATION_BAD_STATE,
          "rotation prepare rejects second prepare");
    CHECK(trust_rotation_prepared(&rotation)->generation == 2U &&
              trust_rotation_prepared(&rotation)->anchor_count == 3U &&
              trust_rotation_prepared(&rotation)->anchors[2].id == 3U &&
              trust_rotation_prepared(&rotation)->anchors[2].state ==
                  TRUST_ANCHOR_STATE_ACTIVE,
          "rotation prepare builds exact next-generation snapshot");

    memset(buffer, 0, sizeof(buffer));
    CHECK(signed_metadata_seal(
              &policy, KEY_USE_PURPOSE_IMAGE, 1U,
              metadata, sizeof(metadata),
              buffer, sizeof(buffer), &length) == SIGNED_METADATA_OK &&
              signed_metadata_verify(
                  &policy, KEY_USE_PURPOSE_IMAGE,
                  buffer, length, &layout) == SIGNED_METADATA_OK,
          "existing active verification works during interrupted prepare");
    check_seal_behavior(
        &policy, 3U, SIGNED_METADATA_UNKNOWN_SIGNER,
        "inactive staged anchor still cannot sign during interruption");

    CHECK(trust_rotation_rollback(&rotation, &policy) ==
              TRUST_ROTATION_OK &&
              !trust_rotation_has_staged(&rotation) &&
              !trust_rotation_has_prepared(&rotation) &&
              !verification_policy_has_pending(&policy) &&
              active_equal(&policy, &initial),
          "rotation rollback clears transaction and preserves active policy");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "trust rotation stage and prepare perform no allocation");
}

static void test_prepare_failures(void)
{
    struct trust_anchor anchors[TRUST_ANCHOR_SET_MAX];
    struct verification_policy_snapshot initial;
    struct verification_policy policy;
    struct trust_rotation rotation;
    struct trust_anchor staged;
    uint32_t index;

    for (index = 0U; index < TRUST_ANCHOR_SET_MAX; ++index) {
        anchors[index] = make_anchor(
            index + 1U, TRUST_ANCHOR_ALGORITHM_ED25519,
            TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    }
    initial = make_snapshot(1U, anchors, TRUST_ANCHOR_SET_MAX);
    init_policy(&policy, 1U, anchors, TRUST_ANCHOR_SET_MAX);
    staged = make_anchor(
        TRUST_ANCHOR_ID_MAX, TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
        TRUST_ANCHOR_STATE_INACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    CHECK(trust_rotation_init(&rotation) == TRUST_ROTATION_OK &&
              trust_rotation_stage(&rotation, &policy, &staged) ==
                  TRUST_ROTATION_OK &&
              trust_rotation_prepare(&rotation, &policy) ==
                  TRUST_ROTATION_POLICY_FULL &&
              !trust_rotation_has_prepared(&rotation) &&
              !verification_policy_has_pending(&policy) &&
              active_equal(&policy, &initial),
          "rotation prepare rejects full active policy");
    CHECK(trust_rotation_rollback(&rotation, &policy) ==
              TRUST_ROTATION_OK,
          "full policy prepare failure rollback clears staged anchor");

    init_policy(&policy, VERIFICATION_POLICY_GENERATION_MAX,
                &anchors[0], 1U);
    CHECK(trust_rotation_init(&rotation) == TRUST_ROTATION_OK,
          "generation overflow fixture initializes rotation");
    staged = make_anchor(
        2U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_INACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    initial = make_snapshot(VERIFICATION_POLICY_GENERATION_MAX,
                            &anchors[0], 1U);
    CHECK(trust_rotation_stage(&rotation, &policy, &staged) ==
              TRUST_ROTATION_OK &&
              trust_rotation_prepare(&rotation, &policy) ==
                  TRUST_ROTATION_GENERATION_OVERFLOW &&
              !trust_rotation_has_prepared(&rotation) &&
              !verification_policy_has_pending(&policy) &&
              active_equal(&policy, &initial),
          "rotation prepare rejects generation overflow");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "trust rotation prepare failures perform no allocation");
}

static void test_successful_activation(void)
{
    struct trust_anchor anchors[2];
    struct verification_policy policy;
    struct trust_rotation rotation;
    struct trust_anchor staged;
    const struct verification_policy_snapshot *active;
    unsigned char buffer[256];
    struct signed_metadata_layout layout;
    const unsigned char metadata[] = "rotated anchor";
    size_t length = 0U;

    anchors[0] = make_anchor(
        1U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    anchors[1] = make_anchor(
        2U, TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
        TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    init_policy(&policy, 1U, anchors, 2U);
    CHECK(trust_rotation_init(&rotation) == TRUST_ROTATION_OK,
          "activation fixture initializes rotation");
    staged = make_anchor(
        3U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_INACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    CHECK(trust_rotation_stage(&rotation, &policy, &staged) ==
              TRUST_ROTATION_OK &&
              trust_rotation_prepare(&rotation, &policy) ==
                  TRUST_ROTATION_OK &&
              trust_rotation_activate(&rotation, &policy) ==
                  TRUST_ROTATION_OK &&
              !trust_rotation_has_staged(&rotation) &&
              !trust_rotation_has_prepared(&rotation) &&
              !verification_policy_has_pending(&policy),
          "trust rotation activation publishes new active policy");

    active = verification_policy_active(&policy);
    CHECK(active != NULL &&
              active->generation == 2U &&
              active->anchor_count == 3U &&
              memcmp(&active->anchors[0], &anchors[0],
                     sizeof(anchors[0])) == 0 &&
              memcmp(&active->anchors[1], &anchors[1],
                     sizeof(anchors[1])) == 0 &&
              active->anchors[2].id == 3U &&
              active->anchors[2].state == TRUST_ANCHOR_STATE_ACTIVE,
          "activated policy keeps old anchors and activates rotated anchor");

    memset(buffer, 0, sizeof(buffer));
    CHECK(signed_metadata_seal(
              &policy, KEY_USE_PURPOSE_IMAGE, 3U,
              metadata, sizeof(metadata),
              buffer, sizeof(buffer), &length) == SIGNED_METADATA_OK &&
              signed_metadata_verify(
                  &policy, KEY_USE_PURPOSE_IMAGE,
                  buffer, length, &layout) == SIGNED_METADATA_OK &&
              layout.signer_id == 3U,
          "activated rotated anchor signs and verifies metadata");
    check_seal_behavior(
        &policy, 1U, SIGNED_METADATA_OK,
        "existing active signer still signs after rotation activation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "trust rotation activation performs no allocation");
}

static void test_failed_activation(void)
{
    struct trust_anchor anchors[2];
    struct verification_policy_snapshot initial;
    struct verification_policy policy;
    struct trust_rotation rotation;
    struct trust_anchor staged;

    anchors[0] = make_anchor(
        1U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    anchors[1] = make_anchor(
        2U, TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
        TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    initial = make_snapshot(1U, anchors, 2U);
    init_policy(&policy, 1U, anchors, 2U);
    CHECK(trust_rotation_init(&rotation) == TRUST_ROTATION_OK,
          "failed-activation fixture initializes rotation");
    staged = make_anchor(
        3U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_INACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    CHECK(trust_rotation_stage(&rotation, &policy, &staged) ==
              TRUST_ROTATION_OK,
          "failed-activation fixture stages and prepares rotation");
    CHECK(trust_rotation_activate(&rotation, &policy) ==
              TRUST_ROTATION_NO_PREPARED &&
              active_equal(&policy, &initial) &&
              trust_rotation_has_staged(&rotation) &&
              !trust_rotation_has_prepared(&rotation),
          "activation before prepare is rejected");
    CHECK(trust_rotation_prepare(&rotation, &policy) ==
              TRUST_ROTATION_OK,
          "failed-activation fixture prepares rotation");
    CHECK(trust_rotation_rollback(&rotation, &policy) ==
              TRUST_ROTATION_OK,
          "failed activation fixture rollback restores clean state");

    CHECK(trust_rotation_stage(&rotation, &policy, &staged) ==
              TRUST_ROTATION_OK &&
              trust_rotation_prepare(&rotation, &policy) ==
                  TRUST_ROTATION_OK,
          "commit-failure fixture stages and prepares rotation");
    policy.pending.generation = 0U;
    rotation.prepared.generation = 0U;
    CHECK(trust_rotation_activate(&rotation, &policy) ==
              TRUST_ROTATION_COMMIT_FAILED &&
              !trust_rotation_has_staged(&rotation) &&
              !trust_rotation_has_prepared(&rotation) &&
              !verification_policy_has_pending(&policy) &&
              active_equal(&policy, &initial),
          "failed activation clears rotation and preserves active policy");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "trust rotation failed activation performs no allocation");
}

int main(void)
{
    test_status_strings();
    test_null_and_uninitialized();
    test_stage_failures();
    test_valid_stage_and_interrupted_prepare();
    test_prepare_failures();
    test_successful_activation();
    test_failed_activation();

    if (failures != 0) {
        fprintf(stderr, "Trust rotation host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Trust rotation host tests: ALL OK\n");
    return 0;
}

#else
#error "This host test must be built with HFOC_TRUST_ROTATION_HOST_TEST"
#endif
