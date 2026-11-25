#include "verification_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_VERIFICATION_POLICY_HOST_TEST

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
    enum trust_anchor_state state)
{
    struct trust_anchor anchor;

    memset(&anchor, 0, sizeof(anchor));
    anchor.id = id;
    anchor.version = TRUST_ANCHOR_DESCRIPTOR_VERSION;
    anchor.algorithm_id = algorithm_id;
    anchor.state = state;
    anchor.accepted_purposes = KEY_USE_ACCEPTED_PURPOSES_ALL;
    return anchor;
}

static struct verification_policy_snapshot make_snapshot(
    uint32_t version,
    uint32_t generation,
    uint32_t anchor_count,
    const struct trust_anchor *anchors)
{
    struct verification_policy_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = version;
    snapshot.generation = generation;
    snapshot.anchor_count = anchor_count;
    if (anchor_count > 0U &&
        anchor_count <= TRUST_ANCHOR_SET_MAX &&
        anchors != NULL) {
        memcpy(snapshot.anchors, anchors,
               anchor_count * sizeof(snapshot.anchors[0]));
    }
    return snapshot;
}

static bool snapshot_equal(
    const struct verification_policy_snapshot *left,
    const struct verification_policy_snapshot *right)
{
    return left != NULL && right != NULL &&
           memcmp(left, right, sizeof(*left)) == 0;
}

static void test_status_strings(void)
{
    static const struct {
        enum verification_policy_status status;
        const char *string;
    } statuses[] = {
        {VERIFICATION_POLICY_OK, "VERIFICATION_POLICY_OK"},
        {VERIFICATION_POLICY_NULL, "VERIFICATION_POLICY_NULL"},
        {VERIFICATION_POLICY_BAD_STATE,
         "VERIFICATION_POLICY_BAD_STATE"},
        {VERIFICATION_POLICY_NO_PENDING,
         "VERIFICATION_POLICY_NO_PENDING"},
        {VERIFICATION_POLICY_INVALID_VERSION,
         "VERIFICATION_POLICY_INVALID_VERSION"},
        {VERIFICATION_POLICY_INVALID_GENERATION,
         "VERIFICATION_POLICY_INVALID_GENERATION"},
        {VERIFICATION_POLICY_INVALID_ANCHOR_COUNT,
         "VERIFICATION_POLICY_INVALID_ANCHOR_COUNT"},
        {VERIFICATION_POLICY_INVALID_ANCHOR,
         "VERIFICATION_POLICY_INVALID_ANCHOR"},
        {VERIFICATION_POLICY_DUPLICATE_ANCHOR,
         "VERIFICATION_POLICY_DUPLICATE_ANCHOR"},
        {VERIFICATION_POLICY_NON_ACTIVE_ANCHOR,
         "VERIFICATION_POLICY_NON_ACTIVE_ANCHOR"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = verification_policy_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "verification policy status string is exact");
    }
    CHECK(verification_policy_status_string(
              (enum verification_policy_status)10U) == NULL,
          "verification policy undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "verification policy status handling performs no allocation");
}

static void test_snapshot_validation(void)
{
    struct trust_anchor anchors[2];
    struct trust_anchor invalid_anchor;
    struct trust_anchor non_active;
    struct verification_policy_snapshot snapshot;

    anchors[0] = make_anchor(
        1U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_ACTIVE);
    anchors[1] = make_anchor(
        2U, TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
        TRUST_ANCHOR_STATE_ACTIVE);

    CHECK(verification_policy_snapshot_validate(NULL) ==
              VERIFICATION_POLICY_NULL,
          "verification policy validation rejects null snapshot");

    snapshot = make_snapshot(
        0U, 1U, 1U, &anchors[0]);
    CHECK(verification_policy_snapshot_validate(&snapshot) ==
              VERIFICATION_POLICY_INVALID_VERSION,
          "verification policy validation rejects zero version");
    snapshot = make_snapshot(
        VERIFICATION_POLICY_VERSION + 1U, 1U, 1U, &anchors[0]);
    CHECK(verification_policy_snapshot_validate(&snapshot) ==
              VERIFICATION_POLICY_INVALID_VERSION,
          "verification policy validation rejects unknown version");
    snapshot = make_snapshot(
        VERIFICATION_POLICY_VERSION, 0U, 1U, &anchors[0]);
    CHECK(verification_policy_snapshot_validate(&snapshot) ==
              VERIFICATION_POLICY_INVALID_GENERATION,
          "verification policy validation rejects zero generation");
    snapshot = make_snapshot(
        VERIFICATION_POLICY_VERSION, 1U, 0U, NULL);
    CHECK(verification_policy_snapshot_validate(&snapshot) ==
              VERIFICATION_POLICY_INVALID_ANCHOR_COUNT,
          "verification policy validation rejects empty anchor set");
    snapshot = make_snapshot(
        VERIFICATION_POLICY_VERSION, 1U,
        TRUST_ANCHOR_SET_MAX + 1U, anchors);
    CHECK(verification_policy_snapshot_validate(&snapshot) ==
              VERIFICATION_POLICY_INVALID_ANCHOR_COUNT,
          "verification policy validation rejects over-limit anchor set");

    invalid_anchor = anchors[0];
    invalid_anchor.id = 0U;
    snapshot = make_snapshot(
        VERIFICATION_POLICY_VERSION, 1U, 1U, &invalid_anchor);
    CHECK(verification_policy_snapshot_validate(&snapshot) ==
              VERIFICATION_POLICY_INVALID_ANCHOR,
          "verification policy validation rejects malformed anchor");

    invalid_anchor = anchors[0];
    invalid_anchor.accepted_purposes = 0U;
    snapshot = make_snapshot(
        VERIFICATION_POLICY_VERSION, 1U, 1U, &invalid_anchor);
    CHECK(verification_policy_snapshot_validate(&snapshot) ==
              VERIFICATION_POLICY_INVALID_ANCHOR,
          "verification policy validation rejects empty accepted-purpose mask");

    non_active = make_anchor(
        3U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_INACTIVE);
    snapshot = make_snapshot(
        VERIFICATION_POLICY_VERSION, 1U, 1U, &non_active);
    CHECK(verification_policy_snapshot_validate(&snapshot) ==
              VERIFICATION_POLICY_NON_ACTIVE_ANCHOR,
          "verification policy validation rejects non-active anchor");

    non_active = make_anchor(
        3U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_REVOKED);
    snapshot = make_snapshot(
        VERIFICATION_POLICY_VERSION, 1U, 1U, &non_active);
    CHECK(verification_policy_snapshot_validate(&snapshot) ==
              VERIFICATION_POLICY_OK,
          "verification policy validation accepts explicit revoked anchor");

    snapshot = make_snapshot(
        VERIFICATION_POLICY_VERSION, 1U, 2U, anchors);
    CHECK(verification_policy_snapshot_validate(&snapshot) ==
              VERIFICATION_POLICY_OK,
          "verification policy validation accepts exact active policy");
    snapshot = make_snapshot(
        VERIFICATION_POLICY_VERSION, 1U, 2U,
        (const struct trust_anchor[]){anchors[0], anchors[0]});
    CHECK(verification_policy_snapshot_validate(&snapshot) ==
              VERIFICATION_POLICY_DUPLICATE_ANCHOR,
          "verification policy validation rejects duplicate anchors");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "verification policy validation performs no allocation");
}

static void test_init_and_null_arguments(void)
{
    struct trust_anchor anchors[2];
    struct verification_policy_snapshot initial;
    struct verification_policy_snapshot invalid;
    struct verification_policy policy;
    unsigned char original[sizeof(policy)];

    anchors[0] = make_anchor(
        1U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_ACTIVE);
    anchors[1] = make_anchor(
        2U, TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
        TRUST_ANCHOR_STATE_ACTIVE);
    initial = make_snapshot(
        VERIFICATION_POLICY_VERSION, 1U, 2U, anchors);

    CHECK(verification_policy_init(NULL, &initial) ==
              VERIFICATION_POLICY_NULL,
          "verification policy init rejects null policy");
    CHECK(verification_policy_init(&policy, NULL) ==
              VERIFICATION_POLICY_NULL,
          "verification policy init rejects null initial snapshot");
    CHECK(verification_policy_stage(NULL, &initial) ==
              VERIFICATION_POLICY_NULL,
          "verification policy stage rejects null policy");
    CHECK(verification_policy_stage(&policy, NULL) ==
              VERIFICATION_POLICY_NULL,
          "verification policy stage rejects null candidate");
    CHECK(verification_policy_commit(NULL) ==
              VERIFICATION_POLICY_NULL,
          "verification policy commit rejects null policy");
    CHECK(verification_policy_rollback(NULL) ==
              VERIFICATION_POLICY_NULL,
          "verification policy rollback rejects null policy");
    CHECK(verification_policy_active(NULL) == NULL,
          "verification policy active rejects null policy");
    CHECK(!verification_policy_has_pending(NULL),
          "verification policy pending query rejects null policy");

    memset(&policy, 0xA5, sizeof(policy));
    policy.has_active = false;
    memcpy(original, &policy, sizeof(policy));
    invalid = initial;
    invalid.generation = 0U;
    CHECK(verification_policy_init(&policy, &invalid) ==
              VERIFICATION_POLICY_INVALID_GENERATION &&
              memcmp(&policy, original, sizeof(policy)) == 0 &&
              verification_policy_active(&policy) == NULL,
          "invalid verification policy init leaves policy unchanged");

    memset(&policy, 0, sizeof(policy));
    CHECK(verification_policy_stage(
              &policy, &initial) == VERIFICATION_POLICY_BAD_STATE,
          "verification policy stage before init fails closed");
    CHECK(verification_policy_commit(&policy) ==
              VERIFICATION_POLICY_BAD_STATE,
          "verification policy commit before init fails closed");
    CHECK(verification_policy_rollback(&policy) ==
              VERIFICATION_POLICY_BAD_STATE,
          "verification policy rollback before init fails closed");
    CHECK(verification_policy_init(&policy, &initial) ==
              VERIFICATION_POLICY_OK &&
              verification_policy_has_pending(&policy) == false &&
              snapshot_equal(
                  verification_policy_active(&policy), &initial),
          "valid verification policy init publishes exact active snapshot");
    CHECK(verification_policy_init(&policy, &initial) ==
              VERIFICATION_POLICY_BAD_STATE,
          "verification policy init rejects reinitialization");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "verification policy init performs no allocation");
}

static void test_stage_failure_preserves_active(void)
{
    struct trust_anchor anchors[2];
    struct verification_policy_snapshot initial;
    struct verification_policy_snapshot replacement;
    struct verification_policy_snapshot invalid;
    struct verification_policy policy;

    anchors[0] = make_anchor(
        1U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_ACTIVE);
    anchors[1] = make_anchor(
        2U, TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
        TRUST_ANCHOR_STATE_ACTIVE);
    initial = make_snapshot(
        VERIFICATION_POLICY_VERSION, 1U, 2U, anchors);
    replacement = make_snapshot(
        VERIFICATION_POLICY_VERSION, 2U, 1U, &anchors[1]);
    invalid = replacement;
    invalid.anchor_count = 0U;

    memset(&policy, 0, sizeof(policy));
    CHECK(verification_policy_init(&policy, &initial) ==
              VERIFICATION_POLICY_OK,
          "stage fixture initializes active verification policy");
    CHECK(verification_policy_stage(
              &policy, &invalid) ==
              VERIFICATION_POLICY_INVALID_ANCHOR_COUNT &&
              !verification_policy_has_pending(&policy) &&
              snapshot_equal(
                  verification_policy_active(&policy), &initial),
          "invalid verification policy stage preserves active snapshot");
    CHECK(verification_policy_stage(
              &policy, &replacement) == VERIFICATION_POLICY_OK &&
              verification_policy_has_pending(&policy) &&
              snapshot_equal(&policy.pending, &replacement) &&
              snapshot_equal(
                  verification_policy_active(&policy), &initial),
          "valid verification policy stage creates pending without active mutation");
    CHECK(verification_policy_stage(
              &policy, &invalid) ==
              VERIFICATION_POLICY_INVALID_ANCHOR_COUNT &&
              !verification_policy_has_pending(&policy) &&
              snapshot_equal(
                  verification_policy_active(&policy), &initial),
          "later invalid stage clears stale pending and preserves active");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "verification policy stage handling performs no allocation");
}

static void test_commit_and_rollback(void)
{
    struct trust_anchor anchors[2];
    struct verification_policy_snapshot initial;
    struct verification_policy_snapshot replacement;
    struct verification_policy policy;

    anchors[0] = make_anchor(
        1U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_ACTIVE);
    anchors[1] = make_anchor(
        2U, TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
        TRUST_ANCHOR_STATE_ACTIVE);
    initial = make_snapshot(
        VERIFICATION_POLICY_VERSION, 1U, 1U, &anchors[0]);
    replacement = make_snapshot(
        VERIFICATION_POLICY_VERSION, 2U, 1U, &anchors[1]);

    memset(&policy, 0, sizeof(policy));
    CHECK(verification_policy_init(&policy, &initial) ==
              VERIFICATION_POLICY_OK,
          "commit fixture initializes active verification policy");
    CHECK(verification_policy_commit(&policy) ==
              VERIFICATION_POLICY_NO_PENDING &&
              snapshot_equal(
                  verification_policy_active(&policy), &initial),
          "verification policy commit without pending preserves active");
    CHECK(verification_policy_stage(
              &policy, &replacement) == VERIFICATION_POLICY_OK &&
              verification_policy_rollback(&policy) ==
                  VERIFICATION_POLICY_OK &&
              !verification_policy_has_pending(&policy) &&
              snapshot_equal(
                  verification_policy_active(&policy), &initial),
          "verification policy rollback clears pending and preserves active");
    CHECK(verification_policy_rollback(&policy) ==
              VERIFICATION_POLICY_NO_PENDING,
          "verification policy rollback without pending fails closed");

    CHECK(verification_policy_stage(
              &policy, &replacement) == VERIFICATION_POLICY_OK &&
              verification_policy_commit(&policy) ==
                  VERIFICATION_POLICY_OK &&
              !verification_policy_has_pending(&policy) &&
              snapshot_equal(
                  verification_policy_active(&policy), &replacement),
          "verification policy commit publishes exact pending snapshot");

    CHECK(verification_policy_stage(
              &policy, &replacement) == VERIFICATION_POLICY_OK,
          "invalid-commit fixture stages replacement");
    policy.pending.generation = 0U;
    CHECK(verification_policy_commit(&policy) ==
              VERIFICATION_POLICY_INVALID_GENERATION &&
              !verification_policy_has_pending(&policy) &&
              snapshot_equal(
                  verification_policy_active(&policy), &replacement),
          "invalid verification policy commit preserves active snapshot");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "verification policy commit and rollback perform no allocation");
}

int main(void)
{
    test_status_strings();
    test_snapshot_validation();
    test_init_and_null_arguments();
    test_stage_failure_preserves_active();
    test_commit_and_rollback();

    if (failures != 0) {
        fprintf(stderr, "Verification policy host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Verification policy host tests: ALL OK\n");
    return 0;
}

#else
#error "This host test must be built with HFOC_VERIFICATION_POLICY_HOST_TEST"
#endif
