#include "appliance_recovery_mode.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef APPLIANCE_RECOVERY_MODE_HOST_TEST

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

static struct appliance_recovery_mode_evidence make_evidence(
    uint32_t domain,
    enum appliance_config_integrity_recovery config_recovery,
    bool update_verified,
    bool pending_update,
    bool first_boot_health_failed,
    bool restart_available,
    bool recovery_trust_active)
{
    struct appliance_recovery_mode_evidence evidence;

    memset(&evidence, 0, sizeof(evidence));
    evidence.version = APPLIANCE_RECOVERY_MODE_VERSION;
    evidence.domain = domain;
    evidence.config_recovery = config_recovery;
    evidence.update_verified = update_verified;
    evidence.pending_update = pending_update;
    evidence.first_boot_health_failed = first_boot_health_failed;
    evidence.restart_available = restart_available;
    evidence.recovery_trust_active = recovery_trust_active;
    return evidence;
}

static void poison_gate(struct appliance_recovery_mode_gate *gate)
{
    memset(gate, 0xA5, sizeof(*gate));
    gate->evaluated = false;
}

static bool gate_unchanged(
    const struct appliance_recovery_mode_gate *gate,
    const struct appliance_recovery_mode_gate *original)
{
    return memcmp(gate, original, sizeof(*gate)) == 0;
}

static bool decision_unchanged(
    const struct appliance_recovery_mode_decision *decision,
    const struct appliance_recovery_mode_decision *original)
{
    return memcmp(decision, original, sizeof(*decision)) == 0;
}

static void test_status_and_name_strings(void)
{
    static const struct {
        enum appliance_recovery_mode_status status;
        const char *string;
    } statuses[] = {
        {APPLIANCE_RECOVERY_MODE_OK,
         "APPLIANCE_RECOVERY_MODE_OK"},
        {APPLIANCE_RECOVERY_MODE_NULL,
         "APPLIANCE_RECOVERY_MODE_NULL"},
        {APPLIANCE_RECOVERY_MODE_BAD_STATE,
         "APPLIANCE_RECOVERY_MODE_BAD_STATE"},
        {APPLIANCE_RECOVERY_MODE_BAD_REQUEST,
         "APPLIANCE_RECOVERY_MODE_BAD_REQUEST"},
    };
    static const struct {
        uint32_t domain;
        const char *string;
    } domains[] = {
        {APPLIANCE_RECOVERY_MODE_DOMAIN_NONE,
         "APPLIANCE_RECOVERY_MODE_DOMAIN_NONE"},
        {APPLIANCE_RECOVERY_MODE_DOMAIN_CONFIGURATION,
         "APPLIANCE_RECOVERY_MODE_DOMAIN_CONFIGURATION"},
        {APPLIANCE_RECOVERY_MODE_DOMAIN_UPDATE,
         "APPLIANCE_RECOVERY_MODE_DOMAIN_UPDATE"},
        {APPLIANCE_RECOVERY_MODE_DOMAIN_RUNTIME,
         "APPLIANCE_RECOVERY_MODE_DOMAIN_RUNTIME"},
    };
    static const struct {
        uint32_t action;
        const char *string;
    } actions[] = {
        {APPLIANCE_RECOVERY_MODE_ACTION_NONE,
         "APPLIANCE_RECOVERY_MODE_ACTION_NONE"},
        {APPLIANCE_RECOVERY_MODE_ACTION_KEEP_LAST_KNOWN_GOOD,
         "APPLIANCE_RECOVERY_MODE_ACTION_KEEP_LAST_KNOWN_GOOD"},
        {APPLIANCE_RECOVERY_MODE_ACTION_SAFE_DEFAULT,
         "APPLIANCE_RECOVERY_MODE_ACTION_SAFE_DEFAULT"},
        {APPLIANCE_RECOVERY_MODE_ACTION_AUTOMATIC_ROLLBACK,
         "APPLIANCE_RECOVERY_MODE_ACTION_AUTOMATIC_ROLLBACK"},
        {APPLIANCE_RECOVERY_MODE_ACTION_SUPERVISED_RESTART,
         "APPLIANCE_RECOVERY_MODE_ACTION_SUPERVISED_RESTART"},
        {APPLIANCE_RECOVERY_MODE_ACTION_READ_ONLY,
         "APPLIANCE_RECOVERY_MODE_ACTION_READ_ONLY"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = appliance_recovery_mode_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "appliance recovery mode status string is exact");
    }
    CHECK(appliance_recovery_mode_status_string(
              (enum appliance_recovery_mode_status)4U) == NULL,
          "appliance recovery mode undefined status is rejected");

    for (index = 0U;
         index < sizeof(domains) / sizeof(domains[0]);
         ++index) {
        const char *string = appliance_recovery_mode_domain_string(
            domains[index].domain);

        CHECK(string != NULL &&
                  strcmp(string, domains[index].string) == 0,
              "appliance recovery mode domain string is exact");
    }
    CHECK(appliance_recovery_mode_domain_string(4U) == NULL,
          "appliance recovery mode undefined domain is rejected");

    for (index = 0U;
         index < sizeof(actions) / sizeof(actions[0]);
         ++index) {
        const char *string = appliance_recovery_mode_action_string(
            actions[index].action);

        CHECK(string != NULL &&
                  strcmp(string, actions[index].string) == 0,
              "appliance recovery mode action string is exact");
    }
    CHECK(appliance_recovery_mode_action_string(6U) == NULL,
          "appliance recovery mode undefined action is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "appliance recovery mode strings perform no allocation");
}

static void test_null_and_bad_state(void)
{
    struct appliance_recovery_mode_gate gate;
    struct appliance_recovery_mode_gate zeroed;
    struct appliance_recovery_mode_decision decision;
    struct appliance_recovery_mode_evidence evidence =
        make_evidence(
            APPLIANCE_RECOVERY_MODE_DOMAIN_RUNTIME,
            APPLIANCE_CONFIG_INTEGRITY_RECOVERY_NONE,
            false, false, false, true, false);

    memset(&zeroed, 0, sizeof(zeroed));
    memset(&decision, 0x5A, sizeof(decision));
    poison_gate(&gate);

    CHECK(appliance_recovery_mode_init(NULL) ==
              APPLIANCE_RECOVERY_MODE_NULL,
          "appliance recovery mode init rejects null gate");
    CHECK(appliance_recovery_mode_evaluate(
              NULL, &evidence, &decision) ==
              APPLIANCE_RECOVERY_MODE_NULL,
          "appliance recovery mode evaluate rejects null gate");
    CHECK(appliance_recovery_mode_evaluate(
              &gate, NULL, &decision) ==
              APPLIANCE_RECOVERY_MODE_NULL,
          "appliance recovery mode evaluate rejects null evidence");
    CHECK(appliance_recovery_mode_evaluate(
              &gate, &evidence, NULL) ==
              APPLIANCE_RECOVERY_MODE_NULL,
          "appliance recovery mode evaluate rejects null decision");
    CHECK(appliance_recovery_mode_evaluate(
              &zeroed, &evidence, &decision) ==
              APPLIANCE_RECOVERY_MODE_BAD_STATE,
          "appliance recovery mode evaluate rejects uninitialized gate");
    CHECK(appliance_recovery_mode_init(&gate) ==
              APPLIANCE_RECOVERY_MODE_OK &&
              gate.version == APPLIANCE_RECOVERY_MODE_VERSION &&
              !gate.evaluated,
          "appliance recovery mode init publishes clean one-shot gate");
    CHECK(appliance_recovery_mode_init(&gate) ==
              APPLIANCE_RECOVERY_MODE_BAD_STATE,
          "appliance recovery mode init rejects reinitialization");
    CHECK(appliance_recovery_mode_evaluate(
              &gate, &evidence, &decision) ==
              APPLIANCE_RECOVERY_MODE_OK &&
              appliance_recovery_mode_is_evaluated(&gate),
          "appliance recovery mode evaluate fixture succeeds");
    CHECK(appliance_recovery_mode_evaluate(
              &gate, &evidence, &decision) ==
              APPLIANCE_RECOVERY_MODE_BAD_STATE,
          "appliance recovery mode evaluate is one-shot");
    CHECK(!appliance_recovery_mode_is_evaluated(NULL) &&
              !appliance_recovery_mode_is_evaluated(&zeroed),
          "appliance recovery mode evaluated query rejects null and empty");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "appliance recovery mode null handling performs no allocation");
}

static void check_decision(
    const struct appliance_recovery_mode_evidence *evidence,
    uint32_t expected_action,
    bool expected_requires_reboot,
    const char *message)
{
    struct appliance_recovery_mode_gate gate;
    struct appliance_recovery_mode_decision decision;

    poison_gate(&gate);
    memset(&decision, 0x5A, sizeof(decision));
    CHECK(appliance_recovery_mode_init(&gate) ==
              APPLIANCE_RECOVERY_MODE_OK &&
              appliance_recovery_mode_evaluate(
                  &gate, evidence, &decision) ==
                  APPLIANCE_RECOVERY_MODE_OK &&
              decision.version == APPLIANCE_RECOVERY_MODE_VERSION &&
              decision.domain == evidence->domain &&
              decision.action == expected_action &&
              decision.local_only &&
              decision.requires_reboot == expected_requires_reboot &&
              gate.evaluated &&
              gate.decision.action == expected_action,
          message);
}

static void test_configuration_decisions(void)
{
    struct appliance_recovery_mode_evidence evidence;

    evidence = make_evidence(
        APPLIANCE_RECOVERY_MODE_DOMAIN_CONFIGURATION,
        APPLIANCE_CONFIG_INTEGRITY_RECOVERY_KEEP_LAST_KNOWN_GOOD,
        false, false, false, false, false);
    check_decision(
        &evidence,
        APPLIANCE_RECOVERY_MODE_ACTION_KEEP_LAST_KNOWN_GOOD,
        false,
        "configuration known-good maps to last-known-good decision");

    evidence = make_evidence(
        APPLIANCE_RECOVERY_MODE_DOMAIN_CONFIGURATION,
        APPLIANCE_CONFIG_INTEGRITY_RECOVERY_DEFAULT_INVALID,
        false, false, false, false, false);
    check_decision(
        &evidence,
        APPLIANCE_RECOVERY_MODE_ACTION_SAFE_DEFAULT,
        false,
        "configuration invalid-default maps to safe-default decision");

    evidence = make_evidence(
        APPLIANCE_RECOVERY_MODE_DOMAIN_CONFIGURATION,
        APPLIANCE_CONFIG_INTEGRITY_RECOVERY_EXPLICIT_ERROR,
        false, false, false, false, false);
    check_decision(
        &evidence,
        APPLIANCE_RECOVERY_MODE_ACTION_READ_ONLY,
        false,
        "configuration explicit error maps to read-only decision");

    evidence = make_evidence(
        APPLIANCE_RECOVERY_MODE_DOMAIN_CONFIGURATION,
        APPLIANCE_CONFIG_INTEGRITY_RECOVERY_COMMIT,
        false, false, false, false, false);
    check_decision(
        &evidence,
        APPLIANCE_RECOVERY_MODE_ACTION_NONE,
        false,
        "configuration commit maps to no recovery decision");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "configuration recovery decisions perform no allocation");
}

static void test_update_and_runtime_decisions(void)
{
    struct appliance_recovery_mode_evidence evidence;

    evidence = make_evidence(
        APPLIANCE_RECOVERY_MODE_DOMAIN_UPDATE,
        APPLIANCE_CONFIG_INTEGRITY_RECOVERY_NONE,
        true, true, true, false, false);
    check_decision(
        &evidence,
        APPLIANCE_RECOVERY_MODE_ACTION_AUTOMATIC_ROLLBACK,
        true,
        "failed verified pending update maps to automatic rollback");

    evidence = make_evidence(
        APPLIANCE_RECOVERY_MODE_DOMAIN_UPDATE,
        APPLIANCE_CONFIG_INTEGRITY_RECOVERY_NONE,
        false, true, true, false, false);
    check_decision(
        &evidence,
        APPLIANCE_RECOVERY_MODE_ACTION_READ_ONLY,
        false,
        "unverified update cannot map to automatic rollback");

    evidence = make_evidence(
        APPLIANCE_RECOVERY_MODE_DOMAIN_UPDATE,
        APPLIANCE_CONFIG_INTEGRITY_RECOVERY_NONE,
        true, true, false, false, false);
    check_decision(
        &evidence,
        APPLIANCE_RECOVERY_MODE_ACTION_READ_ONLY,
        false,
        "healthy pending update maps to read-only decision");

    evidence = make_evidence(
        APPLIANCE_RECOVERY_MODE_DOMAIN_RUNTIME,
        APPLIANCE_CONFIG_INTEGRITY_RECOVERY_NONE,
        false, false, false, true, false);
    check_decision(
        &evidence,
        APPLIANCE_RECOVERY_MODE_ACTION_SUPERVISED_RESTART,
        false,
        "available runtime restart maps to supervised restart");

    evidence = make_evidence(
        APPLIANCE_RECOVERY_MODE_DOMAIN_RUNTIME,
        APPLIANCE_CONFIG_INTEGRITY_RECOVERY_NONE,
        false, false, false, false, false);
    check_decision(
        &evidence,
        APPLIANCE_RECOVERY_MODE_ACTION_READ_ONLY,
        false,
        "exhausted runtime restart maps to read-only decision");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "update and runtime recovery decisions perform no allocation");
}

static void test_recovery_trust_override(void)
{
    struct appliance_recovery_mode_evidence evidence;

    evidence = make_evidence(
        APPLIANCE_RECOVERY_MODE_DOMAIN_CONFIGURATION,
        APPLIANCE_CONFIG_INTEGRITY_RECOVERY_KEEP_LAST_KNOWN_GOOD,
        false, false, false, false, true);
    check_decision(
        &evidence,
        APPLIANCE_RECOVERY_MODE_ACTION_READ_ONLY,
        false,
        "active recovery trust overrides configuration mutation");

    evidence = make_evidence(
        APPLIANCE_RECOVERY_MODE_DOMAIN_UPDATE,
        APPLIANCE_CONFIG_INTEGRITY_RECOVERY_NONE,
        true, true, true, false, true);
    check_decision(
        &evidence,
        APPLIANCE_RECOVERY_MODE_ACTION_READ_ONLY,
        false,
        "active recovery trust overrides update rollback");

    evidence = make_evidence(
        APPLIANCE_RECOVERY_MODE_DOMAIN_RUNTIME,
        APPLIANCE_CONFIG_INTEGRITY_RECOVERY_NONE,
        false, false, false, true, true);
    check_decision(
        &evidence,
        APPLIANCE_RECOVERY_MODE_ACTION_READ_ONLY,
        false,
        "active recovery trust overrides runtime restart");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "recovery trust override performs no allocation");
}

static void test_invalid_evidence_no_write(void)
{
    struct appliance_recovery_mode_gate gate;
    struct appliance_recovery_mode_gate original;
    struct appliance_recovery_mode_decision decision;
    struct appliance_recovery_mode_decision decision_before;
    struct appliance_recovery_mode_evidence evidence;

    poison_gate(&gate);
    CHECK(appliance_recovery_mode_init(&gate) ==
              APPLIANCE_RECOVERY_MODE_OK,
          "invalid evidence fixture initializes gate");
    memcpy(&original, &gate, sizeof(gate));
    memset(&decision, 0x5A, sizeof(decision));
    memcpy(&decision_before, &decision, sizeof(decision));

    evidence = make_evidence(
        APPLIANCE_RECOVERY_MODE_DOMAIN_RUNTIME,
        APPLIANCE_CONFIG_INTEGRITY_RECOVERY_NONE,
        false, false, false, true, false);
    evidence.version = 2U;
    CHECK(appliance_recovery_mode_evaluate(
              &gate, &evidence, &decision) ==
              APPLIANCE_RECOVERY_MODE_BAD_REQUEST &&
              gate_unchanged(&gate, &original) &&
              decision_unchanged(&decision, &decision_before),
          "bad recovery evidence version leaves gate and decision unchanged");

    evidence.version = APPLIANCE_RECOVERY_MODE_VERSION;
    evidence.domain = APPLIANCE_RECOVERY_MODE_DOMAIN_NONE;
    CHECK(appliance_recovery_mode_evaluate(
              &gate, &evidence, &decision) ==
              APPLIANCE_RECOVERY_MODE_BAD_REQUEST &&
              gate_unchanged(&gate, &original),
          "none recovery domain cannot publish a decision");

    evidence.domain = APPLIANCE_RECOVERY_MODE_DOMAIN_RUNTIME;
    evidence.config_recovery =
        (enum appliance_config_integrity_recovery)6U;
    CHECK(appliance_recovery_mode_evaluate(
              &gate, &evidence, &decision) ==
              APPLIANCE_RECOVERY_MODE_BAD_REQUEST &&
              gate_unchanged(&gate, &original),
          "invalid config recovery evidence cannot publish a decision");
    CHECK(!gate.evaluated,
          "invalid evidence never marks recovery gate evaluated");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "invalid recovery evidence performs no allocation");
}

int main(void)
{
    test_status_and_name_strings();
    test_null_and_bad_state();
    test_configuration_decisions();
    test_update_and_runtime_decisions();
    test_recovery_trust_override();
    test_invalid_evidence_no_write();
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "appliance recovery mode host proof performs no allocation");

    if (failures != 0) {
        fprintf(stderr, "appliance recovery mode host: %d failures\n",
                failures);
        return 1;
    }
    printf("appliance recovery mode host: OK\n");
    return 0;
}

#endif
