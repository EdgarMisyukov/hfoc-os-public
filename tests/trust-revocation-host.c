#include "trust_revocation.h"

#include "audit.h"
#include "signed_metadata.h"
#include "trust_rotation.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_TRUST_REVOCATION_HOST_TEST

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
          "trust revocation fixture initializes active policy");
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
    const unsigned char metadata[] = "revocation behavior";
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
        enum trust_revocation_status status;
        const char *string;
    } statuses[] = {
        {TRUST_REVOCATION_OK, "TRUST_REVOCATION_OK"},
        {TRUST_REVOCATION_NULL, "TRUST_REVOCATION_NULL"},
        {TRUST_REVOCATION_BAD_STATE, "TRUST_REVOCATION_BAD_STATE"},
        {TRUST_REVOCATION_NO_POLICY, "TRUST_REVOCATION_NO_POLICY"},
        {TRUST_REVOCATION_UNKNOWN_ANCHOR,
         "TRUST_REVOCATION_UNKNOWN_ANCHOR"},
        {TRUST_REVOCATION_NON_ACTIVE_ANCHOR,
         "TRUST_REVOCATION_NON_ACTIVE_ANCHOR"},
        {TRUST_REVOCATION_GENERATION_OVERFLOW,
         "TRUST_REVOCATION_GENERATION_OVERFLOW"},
        {TRUST_REVOCATION_POLICY_STAGE_FAILED,
         "TRUST_REVOCATION_POLICY_STAGE_FAILED"},
        {TRUST_REVOCATION_POLICY_COMMIT_FAILED,
         "TRUST_REVOCATION_POLICY_COMMIT_FAILED"},
        {TRUST_REVOCATION_AUDIT_FAILED,
         "TRUST_REVOCATION_AUDIT_FAILED"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = trust_revocation_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "trust revocation status string is exact");
    }
    CHECK(trust_revocation_status_string(
              (enum trust_revocation_status)10U) == NULL,
          "trust revocation undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "trust revocation status handling performs no allocation");
}

static void test_null_and_uninitialized(void)
{
    struct trust_anchor anchors[2];
    struct verification_policy policy;
    struct event_ring ring;
    struct trust_revocation revocation;
    struct trust_revocation zeroed;
    uint64_t sequence = UINT64_MAX;

    anchors[0] = make_anchor(
        1U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    anchors[1] = make_anchor(
        2U, TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
        TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    init_policy(&policy, 1U, anchors, 2U);
    memset(&ring, 0, sizeof(ring));
    memset(&zeroed, 0, sizeof(zeroed));

    CHECK(trust_revocation_init(NULL) == TRUST_REVOCATION_NULL,
          "trust revocation init rejects null revocation");
    CHECK(trust_revocation_revoke(
              NULL, &policy, 1U, &ring, &sequence) ==
              TRUST_REVOCATION_NULL,
          "trust revocation revoke rejects null revocation");
    CHECK(trust_revocation_revoke(
              &zeroed, &policy, 1U, &ring, &sequence) ==
              TRUST_REVOCATION_BAD_STATE,
          "trust revocation revoke rejects uninitialized revocation");
    CHECK(trust_revocation_revoke(
              &revocation, NULL, 1U, &ring, &sequence) ==
              TRUST_REVOCATION_NULL,
          "trust revocation revoke rejects null policy");
    CHECK(trust_revocation_revoke(
              &revocation, &policy, 1U, NULL, &sequence) ==
              TRUST_REVOCATION_NULL,
          "trust revocation revoke rejects null audit ring");
    CHECK(trust_revocation_anchor_is_revoked(
              NULL, &policy, 1U) == false &&
              trust_revocation_anchor_is_revoked(
                  &revocation, NULL, 1U) == false,
          "trust revocation revoked query rejects null inputs");

    CHECK(trust_revocation_init(&revocation) ==
              TRUST_REVOCATION_OK &&
              revocation.version == TRUST_REVOCATION_VERSION &&
              !revocation.has_revoked &&
              revocation.last_anchor_id == 0U &&
              revocation.last_generation == 0U,
          "trust revocation init clears revocation state");
    CHECK(trust_revocation_revoke(
              &revocation, &policy, 1U, &ring, &sequence) ==
              TRUST_REVOCATION_BAD_STATE,
          "trust revocation revoke rejects unpublished audit ring");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "trust revocation null handling performs no allocation");
}

static void test_failure_preserves_active(void)
{
    struct trust_anchor anchors[2];
    struct verification_policy_snapshot initial;
    struct verification_policy policy;
    struct event_ring ring;
    struct trust_revocation revocation;
    struct verification_policy_snapshot staged;
    struct audit_record filler;
    uint64_t sequence = UINT64_MAX;
    uint32_t index;

    anchors[0] = make_anchor(
        1U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    anchors[1] = make_anchor(
        2U, TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
        TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    initial = make_snapshot(1U, anchors, 2U);
    init_policy(&policy, 1U, anchors, 2U);
    memset(&ring, 0, sizeof(ring));
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "failure fixture initializes audit ring");
    CHECK(trust_revocation_init(&revocation) == TRUST_REVOCATION_OK,
          "failure fixture initializes revocation");

    CHECK(trust_revocation_revoke(
              &revocation, &policy, 0U, &ring, &sequence) ==
              TRUST_REVOCATION_UNKNOWN_ANCHOR &&
              active_equal(&policy, &initial) &&
              !revocation.has_revoked &&
              event_ring_count(&ring) == 0U &&
              sequence == UINT64_MAX,
          "trust revocation rejects zero anchor id without mutation");
    CHECK(trust_revocation_revoke(
              &revocation, &policy, 3U, &ring, &sequence) ==
              TRUST_REVOCATION_UNKNOWN_ANCHOR &&
              active_equal(&policy, &initial),
          "trust revocation rejects unknown anchor id without mutation");

    memset(&policy, 0, sizeof(policy));
    CHECK(trust_revocation_revoke(
              &revocation, &policy, 1U, &ring, &sequence) ==
              TRUST_REVOCATION_NO_POLICY,
          "trust revocation rejects unpublished policy");

    init_policy(&policy, 1U, anchors, 2U);
    staged = make_snapshot(
        2U, (const struct trust_anchor[]){
            make_anchor(
                1U, TRUST_ANCHOR_ALGORITHM_ED25519,
                TRUST_ANCHOR_STATE_REVOKED,
                KEY_USE_ACCEPTED_PURPOSES_ALL),
            anchors[1]}, 2U);
    CHECK(verification_policy_stage(&policy, &staged) ==
              VERIFICATION_POLICY_OK &&
              verification_policy_commit(&policy) ==
                  VERIFICATION_POLICY_OK,
          "revoked-anchor fixture publishes explicit revoked state");
    CHECK(trust_revocation_revoke(
              &revocation, &policy, 1U, &ring, &sequence) ==
              TRUST_REVOCATION_NON_ACTIVE_ANCHOR &&
              active_equal(&policy, &staged),
          "trust revocation rejects already revoked anchor");

    init_policy(&policy, VERIFICATION_POLICY_GENERATION_MAX,
                &anchors[0], 1U);
    initial = make_snapshot(VERIFICATION_POLICY_GENERATION_MAX,
                            &anchors[0], 1U);
    CHECK(trust_revocation_revoke(
              &revocation, &policy, 1U, &ring, &sequence) ==
              TRUST_REVOCATION_GENERATION_OVERFLOW &&
              active_equal(&policy, &initial) &&
              !revocation.has_revoked,
          "trust revocation rejects generation overflow");

    init_policy(&policy, 1U, anchors, 2U);
    initial = make_snapshot(1U, anchors, 2U);
    for (index = 0U; index < KERNEL_EVENT_RING_CAPACITY; ++index) {
        CHECK(audit_record_init(
                  &filler, AUDIT_RECORD_PROCESS, 1U, 0U,
                  index % 256U, 0U, 0U, index) == AUDIT_OK &&
                  audit_record_append(&ring, &filler, NULL) == AUDIT_OK,
              "full-ring fixture fills audit ring");
    }
    CHECK(trust_revocation_revoke(
              &revocation, &policy, 1U, &ring, &sequence) ==
              TRUST_REVOCATION_AUDIT_FAILED &&
              active_equal(&policy, &initial) &&
              !revocation.has_revoked &&
              event_ring_count(&ring) == KERNEL_EVENT_RING_CAPACITY &&
              sequence == UINT64_MAX,
          "full audit ring rejects revocation without policy mutation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "trust revocation failures perform no allocation");
}

static void test_successful_revocation_and_audit(void)
{
    struct trust_anchor anchors[2];
    struct verification_policy policy;
    struct event_ring ring;
    struct trust_revocation revocation;
    struct audit_record record;
    const struct verification_policy_snapshot *active;
    unsigned char buffer[256];
    struct signed_metadata_layout layout;
    const unsigned char metadata[] = "revoked signer";
    size_t length = 0U;
    uint64_t sequence = UINT64_MAX;

    anchors[0] = make_anchor(
        1U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    anchors[1] = make_anchor(
        2U, TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
        TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    init_policy(&policy, 1U, anchors, 2U);
    memset(&ring, 0, sizeof(ring));
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "success fixture initializes audit ring");
    CHECK(trust_revocation_init(&revocation) == TRUST_REVOCATION_OK,
          "success fixture initializes revocation");

    memset(buffer, 0, sizeof(buffer));
    CHECK(signed_metadata_seal(
              &policy, KEY_USE_PURPOSE_IMAGE, 1U,
              metadata, sizeof(metadata),
              buffer, sizeof(buffer), &length) == SIGNED_METADATA_OK &&
              signed_metadata_verify(
                  &policy, KEY_USE_PURPOSE_IMAGE,
                  buffer, length, &layout) == SIGNED_METADATA_OK &&
              layout.signer_id == 1U,
          "active signer validates before revocation");

    CHECK(trust_revocation_revoke(
              &revocation, &policy, 1U, &ring, &sequence) ==
              TRUST_REVOCATION_OK &&
              sequence == 0U &&
              revocation.last_anchor_id == 1U &&
              revocation.last_generation == 2U &&
              revocation.has_revoked,
          "trust revocation publishes and records last revocation");
    active = verification_policy_active(&policy);
    CHECK(active != NULL &&
              active->generation == 2U &&
              active->anchor_count == 2U &&
              active->anchors[0].id == 1U &&
              active->anchors[0].state == TRUST_ANCHOR_STATE_REVOKED &&
              memcmp(&active->anchors[1], &anchors[1],
                     sizeof(anchors[1])) == 0 &&
              !verification_policy_has_pending(&policy),
          "revocation publishes explicit revoked state without pending");
    CHECK(trust_revocation_anchor_is_revoked(
              &revocation, &policy, 1U) == true &&
              trust_revocation_anchor_is_revoked(
                  &revocation, &policy, 2U) == false,
          "trust revocation query reports only revoked anchor");
    CHECK(event_ring_count(&ring) == 1U &&
              audit_record_pop(&ring, &record) == AUDIT_OK &&
              record.type == AUDIT_RECORD_TRUST &&
              record.operation ==
                  TRUST_REVOCATION_AUDIT_OPERATION_REVOKE &&
              record.result == TRUST_REVOCATION_AUDIT_RESULT_OK &&
              record.pid == 1U &&
              record.domain == 0U &&
              record.generation == 0U &&
              record.detail == (UINT64_C(1) |
                  (UINT64_C(2) <<
                   TRUST_REVOCATION_DETAIL_GENERATION_SHIFT)),
          "revocation appends exact structured trust audit record");

    check_seal_behavior(
        &policy, 1U, SIGNED_METADATA_UNKNOWN_SIGNER,
        "revoked signer cannot seal new metadata");
    CHECK(signed_metadata_verify(
              &policy, KEY_USE_PURPOSE_IMAGE,
              buffer, length, &layout) == SIGNED_METADATA_UNKNOWN_SIGNER,
          "revoked signer cannot validate previously valid envelope");
    check_seal_behavior(
        &policy, 2U, SIGNED_METADATA_OK,
        "unrevoked active signer still seals metadata");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "trust revocation success performs no allocation");
}

static void test_no_rollback_to_revoked_anchor(void)
{
    struct trust_anchor anchors[2];
    struct verification_policy_snapshot initial;
    struct verification_policy policy;
    struct event_ring ring;
    struct trust_revocation revocation;
    struct trust_rotation rotation;
    struct trust_anchor restore;

    anchors[0] = make_anchor(
        1U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    anchors[1] = make_anchor(
        2U, TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
        TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    initial = make_snapshot(1U, anchors, 2U);
    init_policy(&policy, 1U, anchors, 2U);
    memset(&ring, 0, sizeof(ring));
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "no-rollback fixture initializes audit ring");
    CHECK(trust_revocation_init(&revocation) == TRUST_REVOCATION_OK &&
              trust_revocation_revoke(
                  &revocation, &policy, 1U, &ring, NULL) ==
                  TRUST_REVOCATION_OK,
          "no-rollback fixture revokes anchor");
    initial.generation = 2U;
    initial.anchors[0].state = TRUST_ANCHOR_STATE_REVOKED;

    restore = make_anchor(
        1U, TRUST_ANCHOR_ALGORITHM_ED25519,
        TRUST_ANCHOR_STATE_INACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    CHECK(trust_rotation_init(&rotation) == TRUST_ROTATION_OK &&
              trust_rotation_stage(
                  &rotation, &policy, &restore) ==
                  TRUST_ROTATION_DUPLICATE_ANCHOR &&
              !trust_rotation_has_staged(&rotation) &&
              active_equal(&policy, &initial),
          "trust rotation cannot roll back a revoked anchor id");
    CHECK(verification_policy_rollback(&policy) ==
              VERIFICATION_POLICY_NO_PENDING &&
              active_equal(&policy, &initial),
          "verification policy rollback cannot undo committed revocation");
    CHECK(trust_revocation_anchor_is_revoked(
              &revocation, &policy, 1U) == true,
          "revoked anchor remains explicitly non-restorable");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "no-rollback proof performs no allocation");
}

int main(void)
{
    test_status_strings();
    test_null_and_uninitialized();
    test_failure_preserves_active();
    test_successful_revocation_and_audit();
    test_no_rollback_to_revoked_anchor();

    if (failures != 0) {
        fprintf(stderr, "Trust revocation host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Trust revocation host tests: ALL OK\n");
    return 0;
}

#else
#error "This host test must be built with HFOC_TRUST_REVOCATION_HOST_TEST"
#endif
