#include "appliance_recovery_factory_reset_integration.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_HOST_TEST

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

static struct appliance_recovery_factory_reset_integration_evidence
make_evidence(uint32_t operation, uint64_t generation, bool complete)
{
    struct appliance_recovery_factory_reset_integration_evidence evidence;

    memset(&evidence, 0, sizeof(evidence));
    evidence.version =
        APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_EVIDENCE_VERSION;
    evidence.source_generation = generation;
    evidence.published = true;
    evidence.operation_kind = operation;
    evidence.storage_loader_ready = complete;
    evidence.boot_foundation_verified = complete;
    evidence.installer_recovery_ready = complete;
    evidence.efi_environment_ready = complete;
    if (operation ==
        APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_RECOVERY) {
        evidence.recovery_decision_present = complete;
        evidence.recovery_action = complete ? 5U : 0U;
        evidence.recovery_local_only = true;
        evidence.recovery_requires_reboot = false;
    } else if (operation ==
               APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_FACTORY_RESET) {
        evidence.factory_reset_requested = complete;
        evidence.factory_reset_plan_published = complete;
        evidence.authorization_verified = complete;
    }
    return evidence;
}

static enum appliance_recovery_factory_reset_integration_status
fresh_init(struct appliance_recovery_factory_reset_integration *integration)
{
    memset(integration, 0, sizeof(*integration));
    return appliance_recovery_factory_reset_integration_init(integration);
}

static void test_init_contract(void)
{
    struct appliance_recovery_factory_reset_integration integration;

    CHECK(appliance_recovery_factory_reset_integration_init(NULL) ==
              APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_NULL,
          "init rejects null");
    CHECK(fresh_init(&integration) ==
              APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
              appliance_recovery_factory_reset_integration_state(
                  &integration) ==
                  APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_PREPARED &&
              !appliance_recovery_factory_reset_integration_published(
                  &integration),
          "init publishes prepared integration");
    CHECK(appliance_recovery_factory_reset_integration_init(&integration) ==
              APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_ALREADY_PUBLISHED,
          "duplicate init rejected");
    CHECK(appliance_recovery_factory_reset_integration_state_string(
              APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_READY) !=
              NULL &&
              strcmp(appliance_recovery_factory_reset_integration_state_string(
                         APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_READY),
                     "READY") == 0,
          "state strings published");
}

static void test_missing_evidence_paths(void)
{
    struct appliance_recovery_factory_reset_integration integration;
    struct appliance_recovery_factory_reset_integration_evidence evidence =
        make_evidence(
            APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_RECOVERY, 1U, true);
    struct appliance_recovery_factory_reset_integration_evidence none =
        make_evidence(
            APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_NONE, 1U, false);
    struct appliance_recovery_factory_reset_integration_evidence missing =
        make_evidence(
            APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_FACTORY_RESET,
            1U, false);

    CHECK(fresh_init(&integration) ==
              APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
              appliance_recovery_factory_reset_integration_publish(
                  &integration, &none) ==
                  APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_MISSING_EVIDENCE &&
              appliance_recovery_factory_reset_integration_state(
                  &integration) ==
                  APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_AWAITING_RECOVERY,
          "no operation enters awaiting recovery");

    CHECK(fresh_init(&integration) ==
              APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
              appliance_recovery_factory_reset_integration_publish(
                  &integration, &missing) ==
                  APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_MISSING_EVIDENCE &&
              appliance_recovery_factory_reset_integration_state(
                  &integration) ==
                  APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_AWAITING_AUTHORIZATION,
          "factory reset without authorization enters awaiting authorization");

    missing.authorization_verified = true;
    CHECK(fresh_init(&integration) ==
              APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
              appliance_recovery_factory_reset_integration_publish(
                  &integration, &missing) ==
                  APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_MISSING_EVIDENCE &&
              appliance_recovery_factory_reset_integration_state(
                  &integration) ==
                  APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_AWAITING_FOUNDATION,
          "incomplete foundation enters awaiting foundation");

    evidence.recovery_trust_active = true;
    CHECK(fresh_init(&integration) ==
              APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
              appliance_recovery_factory_reset_integration_publish(
                  &integration, &evidence) ==
                  APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_INVALID_EVIDENCE,
          "active recovery trust rejects evidence");
}

static void test_state_ordering_render_and_fail_closed(void)
{
    struct appliance_recovery_factory_reset_integration integration;
    struct appliance_recovery_factory_reset_integration_evidence recovery =
        make_evidence(
            APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_RECOVERY, 1U, true);
    struct appliance_recovery_factory_reset_integration_evidence reset =
        make_evidence(
            APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_FACTORY_RESET,
            1U, true);
    struct appliance_recovery_factory_reset_integration_evidence stale =
        make_evidence(
            APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_RECOVERY, 1U, true);
    struct appliance_recovery_factory_reset_integration_evidence advanced =
        make_evidence(
            APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_FACTORY_RESET,
            2U, true);
    struct appliance_recovery_factory_reset_integration_evidence broken =
        make_evidence(
            APPLIANCE_RECOVERY_FACTORY_RESET_OPERATION_FACTORY_RESET,
            3U, false);
    struct appliance_recovery_factory_reset_integration_snapshot snapshot;
    char output[
        APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OUTPUT_MAX + 1U];
    char sentinel[5] = {'x', 'y', 'z', 'w', '\0'};
    size_t output_length = 0U;
    size_t unchanged_length = 91U;

    broken.authorization_verified = true;
    CHECK(fresh_init(&integration) ==
              APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
              appliance_recovery_factory_reset_integration_publish(
                  &integration, &recovery) ==
                  APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
              appliance_recovery_factory_reset_integration_state(
                  &integration) ==
                  APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_READY &&
              appliance_recovery_factory_reset_integration_snapshot(
                  &integration, &snapshot) ==
                  APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
              appliance_recovery_factory_reset_integration_render(
                  &snapshot, output, sizeof(output), &output_length) ==
                  APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
              strstr(
                  output,
                  "recovery_integration v=1 state=READY "
                  "operation=RECOVERY recovery=READ_ONLY reset=NONE "
                  "auth=UNKNOWN boot=VERIFIED storage=READY "
                  "installer=READY efi=READY management=UNKNOWN "
                  "health=PASS") != NULL,
          "recovery readiness canonical render");

    CHECK(appliance_recovery_factory_reset_integration_publish(
              &integration, &reset) ==
              APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
              appliance_recovery_factory_reset_integration_snapshot(
                  &integration, &snapshot) ==
                  APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
              appliance_recovery_factory_reset_integration_render(
                  &snapshot, output, sizeof(output), &output_length) ==
                  APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
              strstr(
                  output,
                  "operation=FACTORY_RESET recovery=NONE "
                  "reset=APPROVED") != NULL &&
              strstr(output, "auth=VERIFIED") != NULL &&
              strstr(output, "boot=VERIFIED") != NULL,
          "factory reset readiness canonical render");

    CHECK(appliance_recovery_factory_reset_integration_publish(
              &integration, &advanced) ==
              APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
              appliance_recovery_factory_reset_integration_publish(
                  &integration, &stale) ==
                  APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STALE_EVIDENCE,
          "stale evidence rejected after generation advance");
    CHECK(appliance_recovery_factory_reset_integration_publish(
              &integration, &broken) ==
              APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
              appliance_recovery_factory_reset_integration_state(
                  &integration) ==
                  APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_FAILED &&
              !appliance_recovery_factory_reset_integration_published(
                  &integration),
          "incomplete evidence after ready fails closed");
    CHECK(appliance_recovery_factory_reset_integration_snapshot(
              &integration, &snapshot) ==
              APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
              (output_length = unchanged_length) == unchanged_length &&
              appliance_recovery_factory_reset_integration_render(
                  &snapshot, sentinel, sizeof(sentinel),
                  &output_length) ==
                  APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_BUFFER_TOO_SMALL &&
              sentinel[0] == 'x' && sentinel[1] == 'y' &&
              sentinel[2] == 'z' && sentinel[3] == 'w' &&
              sentinel[4] == '\0' && output_length == unchanged_length,
          "overflow render does not mutate output");
    CHECK(appliance_recovery_factory_reset_integration_reset(
              &integration) ==
              APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
              appliance_recovery_factory_reset_integration_publish(
                  &integration, &reset) ==
                  APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
              appliance_recovery_factory_reset_integration_complete(
                  &integration) ==
                  APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_OK &&
              appliance_recovery_factory_reset_integration_state(
                  &integration) ==
                  APPLIANCE_RECOVERY_FACTORY_RESET_INTEGRATION_STATE_COMPLETE,
          "reset allows clean retry and completion");
}

int main(void)
{
    test_init_contract();
    test_missing_evidence_paths();
    test_state_ordering_render_and_fail_closed();
    if (failures != 0) {
        fprintf(stderr,
                "Recovery factory reset integration host tests: %d FAIL\n",
                failures);
        return 1;
    }
    if (alloc_attempts != 0U || free_attempts != 0U) {
        fprintf(stderr,
                "Recovery factory reset integration host allocation\n");
        return 1;
    }
    printf("Recovery factory reset integration host tests: ALL OK\n");
    return 0;
}

#endif
