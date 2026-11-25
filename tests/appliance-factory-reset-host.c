#include "appliance_factory_reset.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef APPLIANCE_FACTORY_RESET_HOST_TEST

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

static struct appliance_factory_reset_evidence make_evidence(
    bool authorization_verified,
    bool boot_foundation_verified,
    bool storage_loader_ready,
    bool recovery_trust_active)
{
    struct appliance_factory_reset_evidence evidence;

    memset(&evidence, 0, sizeof(evidence));
    evidence.version = APPLIANCE_FACTORY_RESET_VERSION;
    evidence.authorization_verified = authorization_verified;
    evidence.boot_foundation_verified = boot_foundation_verified;
    evidence.storage_loader_ready = storage_loader_ready;
    evidence.recovery_trust_active = recovery_trust_active;
    return evidence;
}

static void poison_gate(struct appliance_factory_reset_gate *gate)
{
    memset(gate, 0xA5, sizeof(*gate));
    gate->version = 0U;
    gate->published = false;
}

static void poison_plan(struct appliance_factory_reset_plan *plan)
{
    memset(plan, 0x5A, sizeof(*plan));
}

static bool gate_unchanged(
    const struct appliance_factory_reset_gate *gate,
    const struct appliance_factory_reset_gate *original)
{
    return memcmp(gate, original, sizeof(*gate)) == 0;
}

static bool plan_unchanged(
    const struct appliance_factory_reset_plan *plan,
    const struct appliance_factory_reset_plan *original)
{
    return memcmp(plan, original, sizeof(*plan)) == 0;
}

static void test_status_and_action_strings(void)
{
    static const struct {
        enum appliance_factory_reset_status status;
        const char *string;
    } statuses[] = {
        {APPLIANCE_FACTORY_RESET_OK,
         "APPLIANCE_FACTORY_RESET_OK"},
        {APPLIANCE_FACTORY_RESET_NULL,
         "APPLIANCE_FACTORY_RESET_NULL"},
        {APPLIANCE_FACTORY_RESET_BAD_STATE,
         "APPLIANCE_FACTORY_RESET_BAD_STATE"},
        {APPLIANCE_FACTORY_RESET_BAD_REQUEST,
         "APPLIANCE_FACTORY_RESET_BAD_REQUEST"},
        {APPLIANCE_FACTORY_RESET_UNAUTHORIZED,
         "APPLIANCE_FACTORY_RESET_UNAUTHORIZED"},
        {APPLIANCE_FACTORY_RESET_BOOT_FOUNDATION_UNAVAILABLE,
         "APPLIANCE_FACTORY_RESET_BOOT_FOUNDATION_UNAVAILABLE"},
        {APPLIANCE_FACTORY_RESET_STORAGE_NOT_READY,
         "APPLIANCE_FACTORY_RESET_STORAGE_NOT_READY"},
        {APPLIANCE_FACTORY_RESET_RECOVERY_TRUST_ACTIVE,
         "APPLIANCE_FACTORY_RESET_RECOVERY_TRUST_ACTIVE"},
    };
    static const struct {
        uint32_t action;
        const char *string;
    } actions[] = {
        {APPLIANCE_FACTORY_RESET_ACTION_NONE,
         "APPLIANCE_FACTORY_RESET_ACTION_NONE"},
        {APPLIANCE_FACTORY_RESET_ACTION_REWRITE_FACTORY_DEFAULT,
         "APPLIANCE_FACTORY_RESET_ACTION_REWRITE_FACTORY_DEFAULT"},
        {APPLIANCE_FACTORY_RESET_ACTION_CLEAR,
         "APPLIANCE_FACTORY_RESET_ACTION_CLEAR"},
        {APPLIANCE_FACTORY_RESET_ACTION_PRESERVE_FOUNDATION,
         "APPLIANCE_FACTORY_RESET_ACTION_PRESERVE_FOUNDATION"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = appliance_factory_reset_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "appliance factory reset status string is exact");
    }
    CHECK(appliance_factory_reset_status_string(
              (enum appliance_factory_reset_status)8U) == NULL,
          "appliance factory reset undefined status is rejected");

    for (index = 0U;
         index < sizeof(actions) / sizeof(actions[0]);
         ++index) {
        const char *string = appliance_factory_reset_action_string(
            actions[index].action);

        CHECK(string != NULL &&
                  strcmp(string, actions[index].string) == 0,
              "appliance factory reset action string is exact");
    }
    CHECK(appliance_factory_reset_action_string(4U) == NULL,
          "appliance factory reset undefined action is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "appliance factory reset strings perform no allocation");
}

static void test_null_and_bad_state(void)
{
    struct appliance_factory_reset_gate gate;
    struct appliance_factory_reset_gate zeroed;
    struct appliance_factory_reset_plan plan;
    struct appliance_factory_reset_evidence evidence =
        make_evidence(true, true, true, false);

    memset(&zeroed, 0, sizeof(zeroed));
    poison_plan(&plan);
    poison_gate(&gate);

    CHECK(appliance_factory_reset_init(NULL) ==
              APPLIANCE_FACTORY_RESET_NULL,
          "appliance factory reset init rejects null gate");
    CHECK(appliance_factory_reset_evaluate(
              NULL, &evidence, &plan) ==
              APPLIANCE_FACTORY_RESET_NULL,
          "appliance factory reset evaluate rejects null gate");
    CHECK(appliance_factory_reset_evaluate(
              &gate, NULL, &plan) ==
              APPLIANCE_FACTORY_RESET_NULL,
          "appliance factory reset evaluate rejects null evidence");
    CHECK(appliance_factory_reset_evaluate(
              &gate, &evidence, NULL) ==
              APPLIANCE_FACTORY_RESET_NULL,
          "appliance factory reset evaluate rejects null plan");
    CHECK(appliance_factory_reset_evaluate(
              &zeroed, &evidence, &plan) ==
              APPLIANCE_FACTORY_RESET_BAD_STATE,
          "appliance factory reset evaluate rejects uninitialized gate");
    CHECK(appliance_factory_reset_init(&gate) ==
              APPLIANCE_FACTORY_RESET_OK &&
              gate.version == APPLIANCE_FACTORY_RESET_VERSION &&
              !gate.published,
          "appliance factory reset init publishes clean gate");
    CHECK(appliance_factory_reset_init(&gate) ==
              APPLIANCE_FACTORY_RESET_BAD_STATE,
          "appliance factory reset init rejects reinitialization");
    CHECK(appliance_factory_reset_evaluate(
              &gate, &evidence, &plan) ==
              APPLIANCE_FACTORY_RESET_OK &&
              appliance_factory_reset_is_published(&gate),
          "appliance factory reset evaluate fixture succeeds");
    CHECK(appliance_factory_reset_evaluate(
              &gate, &evidence, &plan) ==
              APPLIANCE_FACTORY_RESET_BAD_STATE,
          "appliance factory reset evaluate is one-shot");
    CHECK(!appliance_factory_reset_is_published(NULL) &&
              !appliance_factory_reset_is_published(&zeroed),
          "appliance factory reset published query rejects null and empty");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "appliance factory reset null handling performs no allocation");
}

static void check_valid_plan(
    const struct appliance_factory_reset_evidence *evidence,
    const char *message)
{
    struct appliance_factory_reset_gate gate;
    struct appliance_factory_reset_plan plan;

    poison_gate(&gate);
    poison_plan(&plan);
    CHECK(appliance_factory_reset_init(&gate) ==
              APPLIANCE_FACTORY_RESET_OK &&
              appliance_factory_reset_evaluate(
                  &gate, evidence, &plan) ==
                  APPLIANCE_FACTORY_RESET_OK &&
              plan.version == APPLIANCE_FACTORY_RESET_VERSION &&
              plan.namespace_count ==
                  APPLIANCE_FACTORY_RESET_NAMESPACE_COUNT &&
              plan.local_only &&
              plan.requires_reboot &&
              plan.preserves_boot_foundation &&
              plan.preserves_recovery_foundation &&
              gate.published &&
              gate.plan.namespace_count ==
                  APPLIANCE_FACTORY_RESET_NAMESPACE_COUNT,
          message);
}

static void test_valid_state_map(void)
{
    static const struct {
        enum appliance_persistent_storage_namespace_id id;
        uint32_t action;
    } expected[] = {
        {APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
         APPLIANCE_FACTORY_RESET_ACTION_REWRITE_FACTORY_DEFAULT},
        {APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SERVICE_DATA,
         APPLIANCE_FACTORY_RESET_ACTION_CLEAR},
        {APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_CERTIFICATE,
         APPLIANCE_FACTORY_RESET_ACTION_CLEAR},
        {APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SECRET,
         APPLIANCE_FACTORY_RESET_ACTION_CLEAR},
        {APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_DIAGNOSTIC,
         APPLIANCE_FACTORY_RESET_ACTION_CLEAR},
        {APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_RECOVERY_METADATA,
         APPLIANCE_FACTORY_RESET_ACTION_PRESERVE_FOUNDATION},
    };
    struct appliance_factory_reset_evidence evidence =
        make_evidence(true, true, true, false);
    struct appliance_factory_reset_gate gate;
    struct appliance_factory_reset_plan plan;
    size_t index;

    check_valid_plan(&evidence,
                     "valid factory reset request publishes exact plan");
    poison_gate(&gate);
    poison_plan(&plan);
    CHECK(appliance_factory_reset_init(&gate) ==
              APPLIANCE_FACTORY_RESET_OK &&
              appliance_factory_reset_evaluate(
                  &gate, &evidence, &plan) ==
                  APPLIANCE_FACTORY_RESET_OK,
          "valid factory reset fixture publishes state map");
    for (index = 0U;
         index < sizeof(expected) / sizeof(expected[0]);
         ++index) {
        CHECK(plan.namespaces[index].id == expected[index].id &&
                  plan.namespaces[index].action == expected[index].action,
              "factory reset namespace action is exact");
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "factory reset state-map publication performs no allocation");
}

static void test_refusal_no_write(void)
{
    struct appliance_factory_reset_gate gate;
    struct appliance_factory_reset_gate gate_before;
    struct appliance_factory_reset_plan plan;
    struct appliance_factory_reset_plan plan_before;
    struct appliance_factory_reset_evidence evidence;

    poison_gate(&gate);
    CHECK(appliance_factory_reset_init(&gate) ==
              APPLIANCE_FACTORY_RESET_OK,
          "factory reset refusal fixture initializes gate");
    memcpy(&gate_before, &gate, sizeof(gate));
    poison_plan(&plan);
    memcpy(&plan_before, &plan, sizeof(plan));

    evidence = make_evidence(false, true, true, false);
    CHECK(appliance_factory_reset_evaluate(
              &gate, &evidence, &plan) ==
              APPLIANCE_FACTORY_RESET_UNAUTHORIZED &&
              gate_unchanged(&gate, &gate_before) &&
              plan_unchanged(&plan, &plan_before),
          "unauthorized reset leaves gate and plan unchanged");

    evidence = make_evidence(true, false, true, false);
    CHECK(appliance_factory_reset_evaluate(
              &gate, &evidence, &plan) ==
              APPLIANCE_FACTORY_RESET_BOOT_FOUNDATION_UNAVAILABLE &&
              gate_unchanged(&gate, &gate_before) &&
              plan_unchanged(&plan, &plan_before),
          "missing boot foundation leaves gate and plan unchanged");

    evidence = make_evidence(true, true, false, false);
    CHECK(appliance_factory_reset_evaluate(
              &gate, &evidence, &plan) ==
              APPLIANCE_FACTORY_RESET_STORAGE_NOT_READY &&
              gate_unchanged(&gate, &gate_before) &&
              plan_unchanged(&plan, &plan_before),
          "unready storage loader leaves gate and plan unchanged");

    evidence = make_evidence(true, true, true, true);
    CHECK(appliance_factory_reset_evaluate(
              &gate, &evidence, &plan) ==
              APPLIANCE_FACTORY_RESET_RECOVERY_TRUST_ACTIVE &&
              gate_unchanged(&gate, &gate_before) &&
              plan_unchanged(&plan, &plan_before),
          "active recovery trust rejects factory reset");

    evidence = make_evidence(true, true, true, false);
    evidence.version = 2U;
    CHECK(appliance_factory_reset_evaluate(
              &gate, &evidence, &plan) ==
              APPLIANCE_FACTORY_RESET_BAD_REQUEST &&
              gate_unchanged(&gate, &gate_before) &&
              plan_unchanged(&plan, &plan_before),
          "bad evidence version leaves gate and plan unchanged");
    CHECK(!appliance_factory_reset_is_published(&gate),
          "refused factory reset never publishes the gate");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "factory reset refusals perform no allocation");
}

int main(void)
{
    test_status_and_action_strings();
    test_null_and_bad_state();
    test_valid_state_map();
    test_refusal_no_write();
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "appliance factory reset host proof performs no allocation");

    if (failures != 0) {
        fprintf(stderr, "appliance factory reset host: %d failures\n",
                failures);
        return 1;
    }
    printf("appliance factory reset host: OK\n");
    return 0;
}

#endif
