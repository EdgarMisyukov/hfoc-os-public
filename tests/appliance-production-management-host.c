#include "appliance_production_management.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_PRODUCTION_MANAGEMENT_HOST_TEST

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

static struct appliance_production_management_evidence make_evidence(
    uint64_t generation)
{
    struct appliance_production_management_evidence evidence;

    memset(&evidence, 0, sizeof(evidence));
    evidence.version =
        APPLIANCE_PRODUCTION_MANAGEMENT_EVIDENCE_VERSION;
    evidence.source_generation = generation;
    evidence.published = true;
    evidence.system_identity_present = true;
    evidence.configuration_ready = true;
    evidence.network_required = true;
    evidence.network_active = true;
    evidence.network_health_ok = true;
    evidence.production_services_ready = true;
    evidence.https_management_ready = true;
    evidence.audit_chain_present = true;
    evidence.audit_chain = APPLIANCE_PRODUCTION_MANAGEMENT_AUDIT_OK;
    evidence.audit_records = 3U;
    evidence.audit_head = UINT64_C(0x0123456789abcdef);
    return evidence;
}

static void test_init_contract(void)
{
    struct appliance_production_management management;

    memset(&management, 0, sizeof(management));
    CHECK(appliance_production_management_init(NULL) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_NULL,
          "init rejects null");
    CHECK(appliance_production_management_init(&management) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
              appliance_production_management_state(&management) ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_STATE_PREPARED &&
              appliance_production_management_failure(&management) ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_NONE &&
              !appliance_production_management_published(&management),
          "init publishes prepared management");
    CHECK(appliance_production_management_init(&management) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_ALREADY_PUBLISHED,
          "duplicate init rejected");
    CHECK(appliance_production_management_state_string(
              APPLIANCE_PRODUCTION_MANAGEMENT_STATE_ACTIVE) != NULL &&
              strcmp(appliance_production_management_state_string(
                         APPLIANCE_PRODUCTION_MANAGEMENT_STATE_ACTIVE),
                     "ACTIVE") == 0,
          "state strings published");
    CHECK(appliance_production_management_state_string(
              (enum appliance_production_management_state)99U) == NULL &&
              appliance_production_management_status_string(
                  (enum appliance_production_management_status)99U) ==
                  NULL &&
              appliance_production_management_failure_string(
                  (enum appliance_production_management_failure)99U) ==
                  NULL,
          "invalid strings rejected");
}

static void test_invalid_evidence(void)
{
    struct appliance_production_management management;
    struct appliance_production_management_evidence evidence =
        make_evidence(1U);
    struct appliance_production_management_evidence invalid =
        evidence;

    memset(&management, 0, sizeof(management));
    CHECK(appliance_production_management_init(&management) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK,
          "invalid evidence fixture initialized");
    CHECK(appliance_production_management_publish(
              &management, NULL) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_NULL,
          "publish rejects null evidence");
    invalid.published = false;
    CHECK(appliance_production_management_publish(
              &management, &invalid) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_INVALID_EVIDENCE &&
              appliance_production_management_state(&management) ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_STATE_PREPARED,
          "unpublished evidence rejected without mutation");
    invalid = evidence;
    invalid.source_generation = 0U;
    CHECK(appliance_production_management_publish(
              &management, &invalid) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_INVALID_EVIDENCE,
          "zero source generation rejected");
    invalid = evidence;
    invalid.version = 99U;
    CHECK(appliance_production_management_publish(
              &management, &invalid) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_BAD_VERSION,
          "bad evidence version rejected");
    invalid = evidence;
    invalid.system_identity_present = false;
    CHECK(appliance_production_management_publish(
              &management, &invalid) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_MISSING_EVIDENCE &&
              appliance_production_management_state(&management) ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_STATE_AWAITING_EVIDENCE,
          "missing identity enters awaiting evidence");
    invalid = evidence;
    invalid.configuration_ready = false;
    CHECK(appliance_production_management_publish(
              &management, &invalid) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_MISSING_EVIDENCE,
          "missing configuration blocks publication");
    invalid = evidence;
    invalid.network_active = false;
    CHECK(appliance_production_management_publish(
              &management, &invalid) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_MISSING_EVIDENCE,
          "missing network blocks publication");
    invalid = evidence;
    invalid.production_services_ready = false;
    CHECK(appliance_production_management_publish(
              &management, &invalid) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_MISSING_EVIDENCE,
          "missing production services blocks publication");
    invalid = evidence;
    invalid.https_management_ready = false;
    CHECK(appliance_production_management_publish(
              &management, &invalid) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_MISSING_EVIDENCE,
          "missing HTTPS readiness blocks publication");
}

static void test_state_ordering_and_render(void)
{
    struct appliance_production_management management;
    struct appliance_production_management_evidence evidence =
        make_evidence(1U);
    struct appliance_production_management_evidence without_audit;
    struct appliance_production_management_snapshot snapshot;
    char output[APPLIANCE_PRODUCTION_MANAGEMENT_OUTPUT_MAX + 1U];
    size_t output_length = 0U;

    memset(&management, 0, sizeof(management));
    CHECK(appliance_production_management_init(&management) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
              appliance_production_management_publish(
                  &management, &evidence) ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
              appliance_production_management_state(&management) ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_STATE_ACTIVE &&
              appliance_production_management_published(&management),
          "complete evidence publishes active management");
    CHECK(appliance_production_management_snapshot(
              &management, &snapshot) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
              snapshot.health ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_HEALTH_PASS &&
              snapshot.network_active &&
              snapshot.production_services_ready &&
              snapshot.https_management_ready,
          "active snapshot published");
    CHECK(appliance_production_management_render(
              &snapshot, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
              strstr(output,
                     "production_management v=1 state=ACTIVE "
                     "identity=PRESENT config=COMMITTED network=ACTIVE "
                     "services=ACTIVE https=READY "
                     "authorization=UNAVAILABLE attestation=UNAVAILABLE "
                     "health=PASS audit_chain=OK audit_records=3 "
                     "audit_head=0123456789abcdef") != NULL &&
              snapshot.audit_chain ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_AUDIT_OK &&
              snapshot.audit_records == 3U &&
              snapshot.audit_head == UINT64_C(0x0123456789abcdef),
          "canonical render carries the audit integrity view");
    without_audit = evidence;
    without_audit.audit_chain_present = false;
    without_audit.audit_chain =
        APPLIANCE_PRODUCTION_MANAGEMENT_AUDIT_TAMPERED;
    without_audit.audit_records = 9U;
    without_audit.audit_head = UINT64_MAX;
    without_audit.source_generation = 2U;
    CHECK(appliance_production_management_publish(
              &management, &without_audit) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
              appliance_production_management_snapshot(
                  &management, &snapshot) ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
              snapshot.audit_chain ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_AUDIT_UNAVAILABLE &&
              snapshot.audit_records == 0U &&
              snapshot.audit_head == 0U,
          "absent audit evidence publishes UNAVAILABLE, not a stale pass");
    CHECK(appliance_production_management_render(
              &snapshot, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
              strstr(output,
                     "audit_chain=UNAVAILABLE audit_records=0 "
                     "audit_head=0000000000000000") != NULL,
          "the audit view renders UNAVAILABLE without evidence");
    {
        struct appliance_production_management_evidence with_support =
            evidence;
        struct appliance_production_management_evidence with_unknown =
            evidence;

        with_support.support_present = true;
        with_support.support_bundle =
            APPLIANCE_PRODUCTION_MANAGEMENT_SUPPORT_SEALED;
        with_support.support_withheld = 4U;
        with_support.support_session_active = true;
        with_support.support_break_glass_armed = true;
        with_support.source_generation = 3U;
        CHECK(appliance_production_management_publish(
                  &management, &with_support) ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
                  appliance_production_management_snapshot(
                      &management, &snapshot) ==
                      APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
                  snapshot.support_bundle ==
                      APPLIANCE_PRODUCTION_MANAGEMENT_SUPPORT_SEALED &&
                  snapshot.support_withheld == 4U &&
                  snapshot.support_session_active &&
                  snapshot.support_break_glass_armed,
              "ITEM-378 support evidence publishes the sealed view");
        CHECK(appliance_production_management_render(
                  &snapshot, output, sizeof(output), &output_length) ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
                  strstr(output,
                         " support=SEALED support_withheld=4 "
                         "session=ACTIVE break_glass=ARMED") != NULL,
              "canonical render carries the support plane view");

        with_unknown.support_present = true;
        with_unknown.support_bundle =
            APPLIANCE_PRODUCTION_MANAGEMENT_SUPPORT_UNKNOWN;
        with_unknown.source_generation = 4U;
        CHECK(appliance_production_management_publish(
                  &management, &with_unknown) ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_INVALID_EVIDENCE,
              "support evidence without a verdict is refused");
        CHECK(strcmp(appliance_production_management_support_string(
                         APPLIANCE_PRODUCTION_MANAGEMENT_SUPPORT_SEALED),
                     "SEALED") == 0 &&
                  strcmp(appliance_production_management_support_string(
                             APPLIANCE_PRODUCTION_MANAGEMENT_SUPPORT_REDACTION_FAILED),
                         "REDACTION_FAILED") == 0 &&
                  strcmp(appliance_production_management_support_string(
                             (enum appliance_production_management_support)99U),
                         "UNKNOWN") == 0,
              "support view strings published");
    }
    CHECK(appliance_production_management_audit_string(
              APPLIANCE_PRODUCTION_MANAGEMENT_AUDIT_TAMPERED) != NULL &&
              strcmp(appliance_production_management_audit_string(
                         APPLIANCE_PRODUCTION_MANAGEMENT_AUDIT_TAMPERED),
                     "TAMPERED") == 0 &&
              strcmp(appliance_production_management_audit_string(
                         (enum appliance_production_management_audit)99U),
                     "UNKNOWN") == 0,
          "audit view strings published");
    {
        struct appliance_production_management_evidence invalid_audit =
            evidence;

        invalid_audit.audit_chain_present = true;
        invalid_audit.audit_chain =
            APPLIANCE_PRODUCTION_MANAGEMENT_AUDIT_UNKNOWN;
        invalid_audit.source_generation = 3U;
        CHECK(appliance_production_management_publish(
                  &management, &invalid_audit) ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_INVALID_EVIDENCE,
              "present audit evidence without a real state is refused");
        invalid_audit.audit_chain =
            (enum appliance_production_management_audit)99U;
        CHECK(appliance_production_management_publish(
                  &management, &invalid_audit) ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_INVALID_EVIDENCE,
              "out-of-range audit evidence is refused");
    }
    CHECK(appliance_production_management_stop(&management) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
              appliance_production_management_state(&management) ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_STATE_STOPPED &&
              !appliance_production_management_published(&management) &&
              management.stop_count == 1U,
          "stop clears management snapshot");
    CHECK(appliance_production_management_publish(
              &management, &evidence) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_BAD_STATE,
          "stopped management rejects evidence");
    CHECK(appliance_production_management_reset(&management) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
              appliance_production_management_state(&management) ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_STATE_PREPARED &&
              management.reset_count == 1U,
          "stopped management reset allowed");
}

static void test_stale_failure_and_degraded(void)
{
    struct appliance_production_management management;
    struct appliance_production_management_evidence evidence =
        make_evidence(1U);
    struct appliance_production_management_evidence stale =
        make_evidence(2U);
    struct appliance_production_management_evidence degraded =
        make_evidence(2U);
    struct appliance_production_management_snapshot snapshot;
    char sentinel[3] = {'x', 'y', '\0'};
    size_t unchanged_length = 77U;
    size_t output_length = unchanged_length;

    memset(&management, 0, sizeof(management));
    CHECK(appliance_production_management_init(&management) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK,
          "stale evidence fixture initialized");
    CHECK(appliance_production_management_publish(
              &management, &stale) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
              appliance_production_management_state(&management) ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_STATE_ACTIVE,
          "generation advance accepted");
    CHECK(appliance_production_management_publish(
              &management, &evidence) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_STALE_EVIDENCE &&
              appliance_production_management_state(&management) ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_STATE_ACTIVE,
          "stale evidence rejected without active mutation");
    degraded.network_health_ok = false;
    CHECK(appliance_production_management_publish(
              &management, &degraded) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
              appliance_production_management_state(&management) ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_STATE_DEGRADED &&
              appliance_production_management_snapshot(
                  &management, &snapshot) ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
              snapshot.health ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_HEALTH_WARN,
          "network warning publishes degraded health");
    CHECK(appliance_production_management_fail_closed(
              &management,
              APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_NETWORK) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
              appliance_production_management_state(&management) ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_STATE_FAILED &&
              appliance_production_management_failure(&management) ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_FAILURE_NETWORK &&
              !appliance_production_management_published(&management),
          "network failure fails closed and clears stale evidence");
    CHECK(appliance_production_management_snapshot(
              &management, &snapshot) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
              appliance_production_management_render(
                  &snapshot, sentinel, sizeof(sentinel),
                  &output_length) ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_BUFFER_TOO_SMALL &&
              sentinel[0] == 'x' && sentinel[1] == 'y' &&
              sentinel[2] == '\0' && output_length == unchanged_length,
          "overflow render does not mutate output");
    CHECK(appliance_production_management_reset(&management) ==
              APPLIANCE_PRODUCTION_MANAGEMENT_OK &&
              appliance_production_management_publish(
                  &management, &evidence) ==
                  APPLIANCE_PRODUCTION_MANAGEMENT_OK,
          "reset allows clean retry");
}

int main(void)
{
    test_init_contract();
    test_invalid_evidence();
    test_state_ordering_and_render();
    test_stale_failure_and_degraded();
    if (failures != 0) {
        fprintf(stderr,
                "Appliance production management host tests: %d FAIL\n",
                failures);
        return 1;
    }
    if (alloc_attempts != 0U || free_attempts != 0U) {
        fprintf(stderr,
                "Appliance production management host test allocation\n");
        return 1;
    }
    printf("Appliance production management host tests: ALL OK\n");
    return 0;
}

#endif
