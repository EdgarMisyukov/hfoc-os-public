#include "appliance_production_service_https.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_PRODUCTION_SERVICE_HTTPS_HOST_TEST

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

static struct appliance_production_service_https_evidence make_evidence(
    bool https_enabled)
{
    struct appliance_production_service_https_evidence evidence;

    memset(&evidence, 0, sizeof(evidence));
    evidence.version =
        APPLIANCE_PRODUCTION_SERVICE_HTTPS_EVIDENCE_VERSION;
    evidence.source_generation = 1U;
    evidence.published = true;
    evidence.loaded_state_committed = true;
    evidence.manager_configured = true;
    evidence.manager_stopped = true;
    evidence.https_enabled = https_enabled;
    evidence.https_stopped = https_enabled;
    return evidence;
}

static struct appliance_production_service_https_readiness make_ready(
    uint64_t generation)
{
    struct appliance_production_service_https_readiness readiness;

    memset(&readiness, 0, sizeof(readiness));
    readiness.version =
        APPLIANCE_PRODUCTION_SERVICE_HTTPS_READINESS_VERSION;
    readiness.source_generation = generation;
    readiness.network_active = true;
    readiness.entropy_ready = true;
    readiness.certificate_ready = true;
    return readiness;
}

static void test_init_contract(void)
{
    struct appliance_production_service_https composition;

    memset(&composition, 0, sizeof(composition));
    CHECK(appliance_production_service_https_init(NULL) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_NULL,
          "init rejects null");
    CHECK(appliance_production_service_https_init(&composition) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_state(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STOPPED &&
              appliance_production_service_https_failure(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_NONE &&
              !appliance_production_service_https_published(&composition),
          "init publishes stopped composition");
    CHECK(appliance_production_service_https_init(&composition) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_ALREADY_PUBLISHED,
          "duplicate init rejected");
    CHECK(appliance_production_service_https_state_string(
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_RUNNING) != NULL &&
              strcmp(appliance_production_service_https_state_string(
                         APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_RUNNING),
                     "RUNNING") == 0,
          "state strings published");
    CHECK(appliance_production_service_https_state_string(
              (enum appliance_production_service_https_state)99U) == NULL &&
              appliance_production_service_https_status_string(
                  (enum appliance_production_service_https_status)99U) ==
                  NULL &&
              appliance_production_service_https_failure_string(
                  (enum appliance_production_service_https_failure)99U) ==
                  NULL,
          "invalid strings rejected");
}

static void test_invalid_evidence(void)
{
    struct appliance_production_service_https composition;
    struct appliance_production_service_https_evidence evidence =
        make_evidence(true);
    struct appliance_production_service_https_evidence invalid =
        evidence;

    memset(&composition, 0, sizeof(composition));
    CHECK(appliance_production_service_https_init(&composition) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK,
          "invalid evidence fixture initialized");
    CHECK(appliance_production_service_https_configure(
              &composition, NULL) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_NULL,
          "configure rejects null evidence");
    invalid.published = false;
    CHECK(appliance_production_service_https_configure(
              &composition, &invalid) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_INVALID_EVIDENCE &&
              appliance_production_service_https_state(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STOPPED,
          "unpublished evidence rejected without mutation");
    invalid = evidence;
    invalid.source_generation = 0U;
    CHECK(appliance_production_service_https_configure(
              &composition, &invalid) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_INVALID_EVIDENCE,
          "zero source generation rejected");
    invalid = evidence;
    invalid.manager_stopped = false;
    CHECK(appliance_production_service_https_configure(
              &composition, &invalid) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_INVALID_EVIDENCE,
          "running manager evidence rejected");
    invalid = evidence;
    invalid.https_stopped = false;
    CHECK(appliance_production_service_https_configure(
              &composition, &invalid) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_INVALID_EVIDENCE,
          "unstopped HTTPS evidence rejected");
}

static void test_state_ordering_and_readiness(void)
{
    struct appliance_production_service_https composition;
    struct appliance_production_service_https_evidence evidence =
        make_evidence(true);
    struct appliance_production_service_https_readiness missing =
        make_ready(1U);
    struct appliance_production_service_https_readiness ready =
        make_ready(1U);
    struct appliance_production_service_https_snapshot snapshot;
    char output[APPLIANCE_PRODUCTION_SERVICE_HTTPS_OUTPUT_MAX + 1U];
    size_t output_length = 0U;

    missing.network_active = false;
    memset(&composition, 0, sizeof(composition));
    CHECK(appliance_production_service_https_init(&composition) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_start(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_MISSING_READINESS,
          "unconfigured start rejected");
    CHECK(appliance_production_service_https_configure(
              &composition, &evidence) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_state(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_CONFIGURED,
          "valid evidence configures composition");
    CHECK(appliance_production_service_https_readiness(
              &composition, &missing) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_MISSING_READINESS &&
              appliance_production_service_https_state(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_AWAITING_READINESS &&
              appliance_production_service_https_start(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_MISSING_READINESS,
          "missing HTTPS readiness blocks start");
    CHECK(appliance_production_service_https_readiness(
              &composition, &ready) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_state(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_AWAITING_READINESS,
          "complete readiness accepted");
    CHECK(appliance_production_service_https_start(&composition) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_state(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STARTING &&
              composition.start_count == 1U,
          "readiness allows start");
    CHECK(appliance_production_service_https_running(&composition) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_state(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_RUNNING &&
              appliance_production_service_https_published(&composition),
          "running confirmation publishes snapshot");
    CHECK(appliance_production_service_https_snapshot(
              &composition, &snapshot) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              snapshot.state ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_RUNNING &&
              snapshot.evidence_published,
          "canonical snapshot published");
    CHECK(appliance_production_service_https_render(
              &snapshot, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              strstr(output,
                     "production_service_https v=1 state=RUNNING "
                     "https=ENABLED network=ACTIVE entropy=READY "
                     "certificate=READY health=PASS") != NULL,
          "canonical render");
    CHECK(appliance_production_service_https_stop(&composition) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_state(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STOPPING &&
              composition.stop_count == 1U,
          "stop enters stopping");
    CHECK(appliance_production_service_https_stopped(&composition) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_state(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STOPPED &&
              !appliance_production_service_https_published(&composition),
          "drained stop returns to stopped");
    CHECK(appliance_production_service_https_reset(&composition) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              composition.reset_count == 1U,
          "stopped reset allowed");
}

static void test_stale_evidence_and_failure_paths(void)
{
    struct appliance_production_service_https composition;
    struct appliance_production_service_https_evidence evidence =
        make_evidence(true);
    struct appliance_production_service_https_readiness ready =
        make_ready(1U);
    struct appliance_production_service_https_readiness stale =
        make_ready(2U);
    struct appliance_production_service_https_readiness bad_version =
        make_ready(1U);

    bad_version.version = 99U;
    memset(&composition, 0, sizeof(composition));
    CHECK(appliance_production_service_https_init(&composition) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_configure(
                  &composition, &evidence) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK,
          "failure path fixture configured");
    CHECK(appliance_production_service_https_readiness(
              &composition, &stale) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_STALE_EVIDENCE &&
              appliance_production_service_https_state(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_CONFIGURED,
          "stale readiness evidence rejected without mutation");
    CHECK(appliance_production_service_https_readiness(
              &composition, &bad_version) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_BAD_VERSION &&
              appliance_production_service_https_state(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_CONFIGURED,
          "bad readiness version rejected without mutation");
    CHECK(appliance_production_service_https_readiness(
              &composition, &ready) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_start(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK,
          "failure path fixture reaches starting");
    CHECK(appliance_production_service_https_start_failed(
              &composition,
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_READY) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_state(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_FAILED &&
              appliance_production_service_https_failure(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_READY &&
              !appliance_production_service_https_published(&composition),
          "start failure fails closed");
    CHECK(appliance_production_service_https_reset(&composition) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_configure(
                  &composition, &evidence) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_readiness(
                  &composition, &ready) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_start(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_running(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK,
          "reset allows a clean retry");
    CHECK(appliance_production_service_https_stop(&composition) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_stop(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_STOP_PENDING &&
              composition.stop_count == 1U,
          "repeated stop is pending without counter increment");
    CHECK(appliance_production_service_https_stop_failed(&composition) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_state(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_FAILED &&
              appliance_production_service_https_failure(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_STOP,
          "stop failure fails closed");
}

static void test_fail_closed_and_disabled_https(void)
{
    struct appliance_production_service_https composition;
    struct appliance_production_service_https_evidence evidence =
        make_evidence(true);
    struct appliance_production_service_https_readiness ready =
        make_ready(1U);
    struct appliance_production_service_https_evidence evidence_disabled =
        make_evidence(false);
    struct appliance_production_service_https_readiness no_https_readiness =
        make_ready(1U);
    struct appliance_production_service_https_snapshot snapshot;
    char sentinel[3] = {'x', 'y', '\0'};
    size_t unchanged_length = 77U;
    size_t output_length = unchanged_length;

    no_https_readiness.network_active = false;
    no_https_readiness.entropy_ready = false;
    no_https_readiness.certificate_ready = false;
    memset(&composition, 0, sizeof(composition));
    CHECK(appliance_production_service_https_init(&composition) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_configure(
                  &composition, &evidence) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_readiness(
                  &composition, &ready) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_start(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_running(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK,
          "enabled HTTPS lifecycle reaches running");
    CHECK(appliance_production_service_https_fail_closed(
              &composition,
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_NETWORK) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_state(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_FAILED &&
              appliance_production_service_https_failure(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_NETWORK &&
              !appliance_production_service_https_published(&composition),
          "link loss fails closed and clears stale evidence");
    CHECK(appliance_production_service_https_reset(&composition) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK,
          "failed composition reset allowed");
    memset(&composition, 0, sizeof(composition));
    CHECK(appliance_production_service_https_init(&composition) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_configure(
                  &composition, &evidence_disabled) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_readiness(
                  &composition, &no_https_readiness) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_start(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_running(&composition) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              composition.https_enabled == false,
          "disabled HTTPS does not require HTTPS readiness");
    CHECK(appliance_production_service_https_snapshot(
              &composition, &snapshot) ==
              APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
              appliance_production_service_https_render(
                  &snapshot, sentinel, sizeof(sentinel),
                  &output_length) ==
                  APPLIANCE_PRODUCTION_SERVICE_HTTPS_BUFFER_TOO_SMALL &&
              sentinel[0] == 'x' && sentinel[1] == 'y' &&
              sentinel[2] == '\0' && output_length == unchanged_length,
          "overflow render does not mutate output");
}

int main(void)
{
    test_init_contract();
    test_invalid_evidence();
    test_state_ordering_and_readiness();
    test_stale_evidence_and_failure_paths();
    test_fail_closed_and_disabled_https();
    if (failures != 0) {
        fprintf(stderr,
                "Appliance production service HTTPS host tests: %d FAIL\n",
                failures);
        return 1;
    }
    if (alloc_attempts != 0U || free_attempts != 0U) {
        fprintf(stderr,
                "Appliance production service HTTPS host test allocation\n");
        return 1;
    }
    printf("Appliance production service HTTPS host tests: ALL OK\n");
    return 0;
}

#endif
