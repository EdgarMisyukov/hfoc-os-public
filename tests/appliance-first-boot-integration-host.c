#include "appliance_first_boot_integration.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_FIRST_BOOT_INTEGRATION_HOST_TEST

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

static struct appliance_first_boot_integration_evidence make_evidence(
    uint64_t generation)
{
    struct appliance_first_boot_integration_evidence evidence;

    memset(&evidence, 0, sizeof(evidence));
    evidence.version =
        APPLIANCE_FIRST_BOOT_INTEGRATION_EVIDENCE_VERSION;
    evidence.source_generation = generation;
    evidence.published = true;
    evidence.first_boot_pending = true;
    evidence.storage_ready = true;
    evidence.wizard_complete = true;
    evidence.management_active = true;
    evidence.network_active = true;
    evidence.https_management_ready = true;
    return evidence;
}

static enum appliance_first_boot_integration_status
fresh_init(struct appliance_first_boot_integration *integration)
{
    memset(integration, 0, sizeof(*integration));
    return appliance_first_boot_integration_init(integration);
}

static void test_init_contract(void)
{
    struct appliance_first_boot_integration integration;

    CHECK(appliance_first_boot_integration_init(NULL) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_NULL,
          "init rejects null");
    CHECK(fresh_init(&integration) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
              appliance_first_boot_integration_state(&integration) ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_PREPARED &&
              appliance_first_boot_integration_failure(&integration) ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_NONE &&
              !appliance_first_boot_integration_published(&integration),
          "init publishes prepared integration");
    CHECK(appliance_first_boot_integration_init(&integration) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_ALREADY_PUBLISHED,
          "duplicate init rejected");
    CHECK(appliance_first_boot_integration_state_string(
              APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_READY) != NULL &&
              strcmp(appliance_first_boot_integration_state_string(
                         APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_READY),
                     "READY") == 0,
          "state strings published");
    CHECK(appliance_first_boot_integration_state_string(
              (enum appliance_first_boot_integration_state)99U) == NULL &&
              appliance_first_boot_integration_status_string(
                  (enum appliance_first_boot_integration_status)99U) ==
                  NULL &&
              appliance_first_boot_integration_failure_string(
                  (enum appliance_first_boot_integration_failure)99U) ==
                  NULL,
          "invalid strings rejected");
}

static void test_invalid_and_missing_evidence(void)
{
    struct appliance_first_boot_integration integration;
    struct appliance_first_boot_integration_evidence evidence =
        make_evidence(1U);
    struct appliance_first_boot_integration_evidence invalid =
        evidence;

    memset(&integration, 0, sizeof(integration));
    CHECK(appliance_first_boot_integration_publish(
              &integration, &evidence) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_UNPUBLISHED,
          "publish before init rejected");
    CHECK(fresh_init(&integration) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_OK,
          "invalid evidence fixture initialized");
    CHECK(appliance_first_boot_integration_publish(
              &integration, NULL) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_NULL,
          "publish rejects null evidence");
    invalid.published = false;
    CHECK(appliance_first_boot_integration_publish(
              &integration, &invalid) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_INVALID_EVIDENCE &&
              appliance_first_boot_integration_state(&integration) ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_PREPARED,
          "unpublished evidence rejected without mutation");
    invalid = evidence;
    invalid.source_generation = 0U;
    CHECK(appliance_first_boot_integration_publish(
              &integration, &invalid) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_INVALID_EVIDENCE,
          "zero source generation rejected");
    invalid = evidence;
    invalid.version = 99U;
    CHECK(appliance_first_boot_integration_publish(
              &integration, &invalid) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_BAD_VERSION,
          "bad evidence version rejected");
    invalid = evidence;
    invalid.first_boot_pending = false;
    CHECK(appliance_first_boot_integration_publish(
              &integration, &invalid) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_INVALID_EVIDENCE &&
              appliance_first_boot_integration_state(&integration) ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_PREPARED,
          "no pending first boot rejected without mutation");

    invalid = evidence;
    invalid.storage_ready = false;
    CHECK(appliance_first_boot_integration_publish(
              &integration, &invalid) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_MISSING_EVIDENCE &&
              appliance_first_boot_integration_state(&integration) ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_AWAITING_WIZARD,
          "missing storage enters awaiting wizard");
    invalid = evidence;
    invalid.wizard_complete = false;
    CHECK(appliance_first_boot_integration_publish(
              &integration, &invalid) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_MISSING_EVIDENCE &&
              appliance_first_boot_integration_state(&integration) ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_AWAITING_WIZARD,
          "missing wizard blocks ready");
    invalid = evidence;
    invalid.management_active = false;
    CHECK(appliance_first_boot_integration_publish(
              &integration, &invalid) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_MISSING_EVIDENCE &&
              appliance_first_boot_integration_state(&integration) ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_AWAITING_MANAGEMENT,
          "missing management enters awaiting management");
    invalid = evidence;
    invalid.network_active = false;
    CHECK(appliance_first_boot_integration_publish(
              &integration, &invalid) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_MISSING_EVIDENCE &&
              appliance_first_boot_integration_state(&integration) ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_AWAITING_MANAGEMENT,
          "missing network blocks ready");
    invalid = evidence;
    invalid.https_management_ready = false;
    CHECK(appliance_first_boot_integration_publish(
              &integration, &invalid) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_MISSING_EVIDENCE,
          "missing HTTPS readiness blocks ready");
    invalid = evidence;
    invalid.wizard_complete = false;
    CHECK(appliance_first_boot_integration_publish(
              &integration, &invalid) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_MISSING_EVIDENCE &&
              appliance_first_boot_integration_state(&integration) ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_AWAITING_WIZARD,
          "awaiting management returns to awaiting wizard");
}

static void test_state_ordering_and_render(void)
{
    struct appliance_first_boot_integration integration;
    struct appliance_first_boot_integration_evidence evidence =
        make_evidence(1U);
    struct appliance_first_boot_integration_snapshot snapshot;
    char output[
        APPLIANCE_FIRST_BOOT_INTEGRATION_OUTPUT_MAX + 1U];
    size_t output_length = 0U;

    CHECK(fresh_init(&integration) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
              appliance_first_boot_integration_publish(
                  &integration, &evidence) ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
              appliance_first_boot_integration_state(&integration) ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_READY &&
              appliance_first_boot_integration_published(&integration),
          "complete evidence publishes ready integration");
    CHECK(appliance_first_boot_integration_snapshot(
              &integration, &snapshot) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
              snapshot.health ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_HEALTH_PASS &&
              snapshot.storage_ready &&
              snapshot.wizard_complete &&
              snapshot.management_active &&
              snapshot.network_active &&
              snapshot.https_management_ready,
          "ready snapshot published");
    CHECK(appliance_first_boot_integration_render(
              &snapshot, output, sizeof(output), &output_length) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
              strstr(
                  output,
                  "first_boot_integration v=1 state=READY "
                  "first_boot=PENDING storage=READY wizard=COMPLETE "
                  "management=ACTIVE network=ACTIVE https=READY "
                  "health=PASS") != NULL,
          "canonical render");
    CHECK(appliance_first_boot_integration_complete(&integration) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
              appliance_first_boot_integration_state(&integration) ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_COMPLETE &&
              appliance_first_boot_integration_complete(&integration) ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_BAD_STATE,
          "complete is confirmed exactly once");
    CHECK(appliance_first_boot_integration_snapshot(
              &integration, &snapshot) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
              appliance_first_boot_integration_render(
                  &snapshot, output, sizeof(output), &output_length) ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
              strstr(output, "state=COMPLETE") != NULL,
          "complete snapshot renders");
    CHECK(appliance_first_boot_integration_publish(
              &integration, &evidence) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_BAD_STATE,
          "completed integration rejects new evidence");
    CHECK(appliance_first_boot_integration_reset(&integration) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
              appliance_first_boot_integration_state(&integration) ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_PREPARED &&
              !appliance_first_boot_integration_published(&integration) &&
              integration.reset_count == 1U,
          "completed integration reset allowed");
    CHECK(appliance_first_boot_integration_publish(
              &integration, &evidence) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
              appliance_first_boot_integration_state(&integration) ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_READY,
          "reset allows clean retry");
}

static void test_stale_failure_and_overflow(void)
{
    struct appliance_first_boot_integration integration;
    struct appliance_first_boot_integration_evidence evidence =
        make_evidence(1U);
    struct appliance_first_boot_integration_evidence advanced =
        make_evidence(2U);
    struct appliance_first_boot_integration_evidence missing =
        make_evidence(3U);
    struct appliance_first_boot_integration_snapshot snapshot;
    char sentinel[5] = {'x', 'y', 'z', 'w', '\0'};
    size_t unchanged_length = 77U;
    size_t output_length = unchanged_length;

    missing.management_active = false;
    CHECK(fresh_init(&integration) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
              appliance_first_boot_integration_publish(
                  &integration, &evidence) ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
              appliance_first_boot_integration_publish(
                  &integration, &advanced) ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
              integration.update_count == 2U,
          "generation advance accepted");
    CHECK(appliance_first_boot_integration_publish(
              &integration, &evidence) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_STALE_EVIDENCE &&
              appliance_first_boot_integration_state(&integration) ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_READY,
          "stale evidence rejected without ready mutation");
    CHECK(appliance_first_boot_integration_publish(
              &integration, &missing) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
              appliance_first_boot_integration_state(&integration) ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_FAILED &&
              appliance_first_boot_integration_failure(&integration) ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_EVIDENCE &&
              !appliance_first_boot_integration_published(&integration),
          "missing evidence after ready fails closed");
    CHECK(appliance_first_boot_integration_snapshot(
              &integration, &snapshot) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
              appliance_first_boot_integration_render(
                  &snapshot, sentinel, sizeof(sentinel),
                  &output_length) ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_BUFFER_TOO_SMALL &&
              sentinel[0] == 'x' && sentinel[1] == 'y' &&
              sentinel[2] == 'z' && sentinel[3] == 'w' &&
              sentinel[4] == '\0' && output_length == unchanged_length,
          "overflow render does not mutate output");
    CHECK(appliance_first_boot_integration_fail_closed(
              &integration,
              APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_WIZARD) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_BAD_STATE,
          "failed integration rejects repeated fail closed");
    CHECK(appliance_first_boot_integration_reset(&integration) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
              appliance_first_boot_integration_publish(
                  &integration, &evidence) ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_OK,
          "failed integration reset allows retry");
    CHECK(appliance_first_boot_integration_fail_closed(
              &integration,
              APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_HTTPS) ==
              APPLIANCE_FIRST_BOOT_INTEGRATION_OK &&
              appliance_first_boot_integration_state(&integration) ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_STATE_FAILED &&
              appliance_first_boot_integration_failure(&integration) ==
                  APPLIANCE_FIRST_BOOT_INTEGRATION_FAILURE_HTTPS,
          "explicit fail closed clears ready evidence");
}

int main(void)
{
    test_init_contract();
    test_invalid_and_missing_evidence();
    test_state_ordering_and_render();
    test_stale_failure_and_overflow();
    if (failures != 0) {
        fprintf(stderr,
                "Appliance first boot integration host tests: %d FAIL\n",
                failures);
        return 1;
    }
    if (alloc_attempts != 0U || free_attempts != 0U) {
        fprintf(stderr,
                "Appliance first boot integration host test allocation\n");
        return 1;
    }
    printf("Appliance first boot integration host tests: ALL OK\n");
    return 0;
}

#endif
