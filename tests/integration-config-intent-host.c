#include "integration_config_intent.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_INTEGRATION_CONFIG_INTENT_HOST_TEST

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

static const char initial_text[] =
    "schema_version=1\n"
    "recovery_mode=1\n"
    "max_processes=32\n"
    "max_user_memory_pages=512\n"
    "audit_ring_capacity=64\n"
    "boot_timeout_ms=5000\n"
    "restart_limit=3\n"
    "service_health_timeout_ms=10000\n";

static const char replacement_text[] =
    "schema_version=1\n"
    "recovery_mode=1\n"
    "max_processes=48\n"
    "max_user_memory_pages=512\n"
    "audit_ring_capacity=64\n"
    "boot_timeout_ms=5000\n"
    "restart_limit=3\n"
    "service_health_timeout_ms=10000\n";

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

static bool config_equal(const struct appliance_config *left,
                         const struct appliance_config *right)
{
    return left != NULL && right != NULL &&
           memcmp(left, right, sizeof(*left)) == 0;
}

static bool commit_accept(const struct appliance_config *candidate,
                          void *context)
{
    (void)candidate;
    (void)context;
    return true;
}

static bool commit_reject(const struct appliance_config *candidate,
                          void *context)
{
    (void)candidate;
    (void)context;
    return false;
}

static struct integration_config_intent make_intent(
    uint32_t intent_id,
    uint32_t value)
{
    struct integration_config_intent intent;

    memset(&intent, 0, sizeof(intent));
    intent.version = INTEGRATION_CONFIG_INTENT_VERSION;
    intent.intent_id = intent_id;
    intent.peer_id = 7U;
    intent.value = value;
    return intent;
}

static struct audit_correlation make_correlation(void)
{
    struct audit_correlation correlation;

    memset(&correlation, 0, sizeof(correlation));
    correlation.version = AUDIT_CORRELATION_VERSION;
    correlation.session_id = 3U;
    correlation.principal_id = 4U;
    correlation.command_id = CONTROLLED_CONFIGURATION_COMMAND_ID;
    correlation.result = AUDIT_CORRELATION_RESULT_OK;
    return correlation;
}

static struct authorization_matrix make_matrix(void)
{
    struct authorization_entry entry;
    struct authorization_matrix matrix;

    memset(&entry, 0, sizeof(entry));
    memset(&matrix, 0, sizeof(matrix));
    CHECK(controlled_configuration_authorization_entry(&entry) ==
              CONTROLLED_CONFIGURATION_OK,
          "intent fixture builds controlled configuration entry");
    CHECK(authorization_matrix_init(&matrix, &entry, 1U) ==
              AUTHORIZATION_OK,
          "intent fixture builds controlled configuration matrix");
    return matrix;
}

static bool fixture_initialize(struct config_snapshot *snapshot,
                               struct event_ring *ring)
{
    struct appliance_config initial;

    memset(snapshot, 0, sizeof(*snapshot));
    memset(ring, 0, sizeof(*ring));
    return config_schema_parse(initial_text, sizeof(initial_text) - 1U,
                               &initial) == CONFIG_OK &&
           config_snapshot_init(snapshot, &initial) ==
               CONFIG_SNAPSHOT_OK &&
           event_ring_init(ring) == EVENT_RING_OK;
}

static bool management_record_matches(
    const struct audit_record *record,
    const struct audit_correlation *correlation,
    uint32_t result,
    uint64_t detail)
{
    return record != NULL && correlation != NULL &&
           record->type == AUDIT_RECORD_MANAGEMENT &&
           record->operation ==
               INTEGRATION_CONFIG_INTENT_AUDIT_OPERATION &&
           record->result == result &&
           record->session_id == correlation->session_id &&
           record->principal_id == correlation->principal_id &&
           record->command_id == correlation->command_id &&
           record->detail == detail;
}

static void test_exact_constants(void)
{
    CHECK(INTEGRATION_CONFIG_INTENT_VERSION == 1U,
          "inbound config intent version is exactly 1");
    CHECK(INTEGRATION_CONFIG_INTENT_PEER_ID_MIN == 1U &&
              INTEGRATION_CONFIG_INTENT_PEER_ID_MAX == 65535U,
          "inbound config intent peer bounds are exact");
    CHECK(INTEGRATION_CONFIG_INTENT_COUNT == 7U,
          "inbound config intent count is exactly 7");
    CHECK(INTEGRATION_CONFIG_INTENT_RECOVERY_MODE == 1U &&
              INTEGRATION_CONFIG_INTENT_MAX_PROCESSES == 2U &&
              INTEGRATION_CONFIG_INTENT_MAX_USER_MEMORY_PAGES == 3U &&
              INTEGRATION_CONFIG_INTENT_AUDIT_RING_CAPACITY == 4U &&
              INTEGRATION_CONFIG_INTENT_BOOT_TIMEOUT_MS == 5U &&
              INTEGRATION_CONFIG_INTENT_RESTART_LIMIT == 6U &&
              INTEGRATION_CONFIG_INTENT_SERVICE_HEALTH_TIMEOUT_MS == 7U,
          "inbound config intent IDs are exact and distinct");
    CHECK(INTEGRATION_CONFIG_INTENT_AUDIT_OPERATION ==
              CONTROLLED_CONFIGURATION_AUDIT_OPERATION_ACTIVATE &&
              INTEGRATION_CONFIG_INTENT_AUDIT_RESULT_DENIED ==
                  AUDIT_CORRELATION_RESULT_DENIED,
          "inbound config intent audit constants reuse controlled boundary");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "inbound config intent constant checking performs no allocation");
}

static void test_validate(void)
{
    struct integration_config_intent valid =
        make_intent(INTEGRATION_CONFIG_INTENT_MAX_PROCESSES, 48U);
    struct integration_config_intent bad_version = valid;
    struct integration_config_intent bad_peer = valid;
    struct integration_config_intent bad_intent = valid;
    struct integration_config_intent bad_value = valid;

    CHECK(integration_config_intent_validate(NULL) ==
              INTEGRATION_CONFIG_INTENT_NULL,
          "inbound config intent validate rejects null intent");
    CHECK(integration_config_intent_validate(&valid) ==
              INTEGRATION_CONFIG_INTENT_OK,
          "inbound config intent validate accepts exact intent");
    bad_version.version = 0U;
    CHECK(integration_config_intent_validate(&bad_version) ==
              INTEGRATION_CONFIG_INTENT_BAD_VERSION,
          "inbound config intent validate rejects unknown version");
    bad_peer = valid;
    bad_peer.peer_id = 0U;
    CHECK(integration_config_intent_validate(&bad_peer) ==
              INTEGRATION_CONFIG_INTENT_INVALID_PEER_ID,
          "inbound config intent validate rejects zero peer");
    bad_peer = valid;
    bad_peer.peer_id = 65536U;
    CHECK(integration_config_intent_validate(&bad_peer) ==
              INTEGRATION_CONFIG_INTENT_INVALID_PEER_ID,
          "inbound config intent validate rejects over-limit peer");
    bad_intent = valid;
    bad_intent.intent_id = 0U;
    CHECK(integration_config_intent_validate(&bad_intent) ==
              INTEGRATION_CONFIG_INTENT_INVALID_INTENT_ID,
          "inbound config intent validate rejects zero intent ID");
    bad_intent = valid;
    bad_intent.intent_id = 8U;
    CHECK(integration_config_intent_validate(&bad_intent) ==
              INTEGRATION_CONFIG_INTENT_INVALID_INTENT_ID,
          "inbound config intent validate rejects unknown intent ID");
    bad_value = valid;
    bad_value.value = 0U;
    CHECK(integration_config_intent_validate(&bad_value) ==
              INTEGRATION_CONFIG_INTENT_INVALID_VALUE,
          "inbound config intent validate rejects below minimum value");
    bad_value = valid;
    bad_value.value = 65U;
    CHECK(integration_config_intent_validate(&bad_value) ==
              INTEGRATION_CONFIG_INTENT_INVALID_VALUE,
          "inbound config intent validate rejects above maximum value");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "inbound config intent validation performs no allocation");
}

static void test_defined_ranges(void)
{
    struct integration_config_intent intent =
        make_intent(INTEGRATION_CONFIG_INTENT_RECOVERY_MODE, 0U);

    CHECK(integration_config_intent_validate(&intent) ==
              INTEGRATION_CONFIG_INTENT_OK &&
              intent.value == 0U,
          "recovery mode intent accepts minimum 0");
    intent.value = 2U;
    CHECK(integration_config_intent_validate(&intent) ==
              INTEGRATION_CONFIG_INTENT_OK,
          "recovery mode intent accepts maximum 2");
    intent.value = 3U;
    CHECK(integration_config_intent_validate(&intent) ==
              INTEGRATION_CONFIG_INTENT_INVALID_VALUE,
          "recovery mode intent rejects over-limit 3");

    intent = make_intent(INTEGRATION_CONFIG_INTENT_MAX_PROCESSES, 1U);
    CHECK(integration_config_intent_validate(&intent) ==
              INTEGRATION_CONFIG_INTENT_OK,
          "max processes intent accepts minimum 1");
    intent.value = 64U;
    CHECK(integration_config_intent_validate(&intent) ==
              INTEGRATION_CONFIG_INTENT_OK,
          "max processes intent accepts maximum 64");

    intent = make_intent(
        INTEGRATION_CONFIG_INTENT_MAX_USER_MEMORY_PAGES, 16U);
    CHECK(integration_config_intent_validate(&intent) ==
              INTEGRATION_CONFIG_INTENT_OK,
          "max user memory intent accepts minimum 16");
    intent.value = 4096U;
    CHECK(integration_config_intent_validate(&intent) ==
              INTEGRATION_CONFIG_INTENT_OK,
          "max user memory intent accepts maximum 4096");

    intent = make_intent(
        INTEGRATION_CONFIG_INTENT_AUDIT_RING_CAPACITY, 16U);
    CHECK(integration_config_intent_validate(&intent) ==
              INTEGRATION_CONFIG_INTENT_OK,
          "audit ring capacity intent accepts minimum 16");
    intent.value = 64U;
    CHECK(integration_config_intent_validate(&intent) ==
              INTEGRATION_CONFIG_INTENT_OK,
          "audit ring capacity intent accepts maximum 64");

    intent = make_intent(INTEGRATION_CONFIG_INTENT_BOOT_TIMEOUT_MS, 1000U);
    CHECK(integration_config_intent_validate(&intent) ==
              INTEGRATION_CONFIG_INTENT_OK,
          "boot timeout intent accepts minimum 1000");
    intent.value = 60000U;
    CHECK(integration_config_intent_validate(&intent) ==
              INTEGRATION_CONFIG_INTENT_OK,
          "boot timeout intent accepts maximum 60000");

    intent = make_intent(INTEGRATION_CONFIG_INTENT_RESTART_LIMIT, 0U);
    CHECK(integration_config_intent_validate(&intent) ==
              INTEGRATION_CONFIG_INTENT_OK,
          "restart limit intent accepts minimum 0");
    intent.value = 16U;
    CHECK(integration_config_intent_validate(&intent) ==
              INTEGRATION_CONFIG_INTENT_OK,
          "restart limit intent accepts maximum 16");

    intent = make_intent(
        INTEGRATION_CONFIG_INTENT_SERVICE_HEALTH_TIMEOUT_MS, 1000U);
    CHECK(integration_config_intent_validate(&intent) ==
              INTEGRATION_CONFIG_INTENT_OK,
          "service health timeout intent accepts minimum 1000");
    intent.value = 30000U;
    CHECK(integration_config_intent_validate(&intent) ==
              INTEGRATION_CONFIG_INTENT_OK,
          "service health timeout intent accepts maximum 30000");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "inbound config intent range checking performs no allocation");
}

static void test_integration_scope_denied(void)
{
    struct config_snapshot snapshot;
    struct event_ring ring;
    struct authorization_matrix matrix = make_matrix();
    struct integration_config_intent intent =
        make_intent(INTEGRATION_CONFIG_INTENT_MAX_PROCESSES, 48U);
    struct audit_correlation correlation = make_correlation();
    struct appliance_config active_before;
    struct audit_record record;
    uint8_t integration_role[] = { AUTHORIZATION_ROLE_INTEGRATION };
    uint8_t integration_capability[] = {
        AUTHORIZATION_CAPABILITY_INTEGRATION_SESSION
    };
    uint64_t sequence = UINT64_MAX;

    CHECK(fixture_initialize(&snapshot, &ring),
          "integration denial fixture initializes snapshot and ring");
    active_before = *config_snapshot_active(&snapshot);
    CHECK(integration_config_intent_apply(
              &snapshot, &intent, &matrix,
              integration_role, 1U,
              integration_capability, 1U,
              &correlation, commit_accept, NULL,
              &ring, &sequence) ==
              INTEGRATION_CONFIG_INTENT_DENIED &&
              sequence == 0U,
          "integration scope is denied for inbound config intent");
    CHECK(config_equal(config_snapshot_active(&snapshot), &active_before) &&
              !config_snapshot_has_pending(&snapshot),
          "integration denial leaves active config unchanged");
    CHECK(event_ring_count(&ring) == 1U &&
              audit_record_pop(&ring, &record) == AUDIT_OK &&
              management_record_matches(
                  &record, &correlation,
                  INTEGRATION_CONFIG_INTENT_AUDIT_RESULT_DENIED,
                  CONTROLLED_CONFIGURATION_COMMAND_ID),
          "integration denial publishes one denied audit record");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "integration denial performs no allocation");
}

static void test_invalid_intent_audit_no_mutation(void)
{
    struct config_snapshot snapshot;
    struct event_ring ring;
    struct authorization_matrix matrix = make_matrix();
    struct integration_config_intent intent =
        make_intent(INTEGRATION_CONFIG_INTENT_MAX_PROCESSES, 48U);
    struct audit_correlation correlation = make_correlation();
    struct appliance_config active_before;
    struct audit_record record;
    uint8_t admin_role[] = { AUTHORIZATION_ROLE_ADMIN };
    uint8_t config_capability[] = {
        AUTHORIZATION_CAPABILITY_CONFIG_WRITE
    };
    uint64_t sequence = UINT64_MAX;

    CHECK(fixture_initialize(&snapshot, &ring),
          "invalid-intent fixture initializes snapshot and ring");
    active_before = *config_snapshot_active(&snapshot);
    intent.intent_id = 99U;
    CHECK(integration_config_intent_apply(
              &snapshot, &intent, &matrix,
              admin_role, 1U, config_capability, 1U,
              &correlation, commit_accept, NULL,
              &ring, &sequence) ==
              INTEGRATION_CONFIG_INTENT_INVALID_INTENT_ID &&
              sequence == 0U,
          "unknown inbound config intent is rejected");
    CHECK(config_equal(config_snapshot_active(&snapshot), &active_before) &&
              !config_snapshot_has_pending(&snapshot),
          "unknown inbound config intent leaves config unchanged");
    CHECK(event_ring_count(&ring) == 1U &&
              audit_record_pop(&ring, &record) == AUDIT_OK &&
              management_record_matches(
                  &record, &correlation,
                  INTEGRATION_CONFIG_INTENT_AUDIT_RESULT_DENIED,
                  CONTROLLED_CONFIGURATION_COMMAND_ID),
          "unknown inbound config intent publishes denied audit");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "invalid-intent rejection performs no allocation");
}

static void test_successful_apply(void)
{
    struct config_snapshot snapshot;
    struct event_ring ring;
    struct authorization_matrix matrix = make_matrix();
    struct integration_config_intent intent =
        make_intent(INTEGRATION_CONFIG_INTENT_MAX_PROCESSES, 48U);
    struct audit_correlation correlation = make_correlation();
    struct appliance_config expected;
    struct audit_record record;
    uint8_t admin_role[] = { AUTHORIZATION_ROLE_ADMIN };
    uint8_t config_capability[] = {
        AUTHORIZATION_CAPABILITY_CONFIG_WRITE
    };
    uint64_t sequence = UINT64_MAX;

    CHECK(fixture_initialize(&snapshot, &ring),
          "success fixture initializes snapshot and ring");
    CHECK(config_schema_parse(replacement_text,
                              sizeof(replacement_text) - 1U,
                              &expected) == CONFIG_OK,
          "success fixture parses expected candidate");
    CHECK(integration_config_intent_apply(
              &snapshot, &intent, &matrix,
              admin_role, 1U, config_capability, 1U,
              &correlation, commit_accept, NULL,
              &ring, &sequence) ==
              INTEGRATION_CONFIG_INTENT_OK &&
              sequence == 0U,
          "locally authorized inbound config intent applies");
    CHECK(config_equal(config_snapshot_active(&snapshot), &expected) &&
              !config_snapshot_has_pending(&snapshot),
          "successful inbound config intent publishes exact active config");
    CHECK(event_ring_count(&ring) == 1U &&
              audit_record_pop(&ring, &record) == AUDIT_OK &&
              management_record_matches(
                  &record, &correlation,
                  AUDIT_CORRELATION_RESULT_OK,
                  CONTROLLED_CONFIGURATION_COMMAND_ID),
          "successful inbound config intent publishes success audit");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "successful inbound config intent performs no allocation");
}

static void apply_expected_field(struct appliance_config *expected,
                                 uint32_t intent_id,
                                 uint32_t value)
{
    switch (intent_id) {
    case INTEGRATION_CONFIG_INTENT_RECOVERY_MODE:
        expected->recovery_mode = value;
        break;
    case INTEGRATION_CONFIG_INTENT_MAX_PROCESSES:
        expected->max_processes = value;
        break;
    case INTEGRATION_CONFIG_INTENT_MAX_USER_MEMORY_PAGES:
        expected->max_user_memory_pages = value;
        break;
    case INTEGRATION_CONFIG_INTENT_AUDIT_RING_CAPACITY:
        expected->audit_ring_capacity = value;
        break;
    case INTEGRATION_CONFIG_INTENT_BOOT_TIMEOUT_MS:
        expected->boot_timeout_ms = value;
        break;
    case INTEGRATION_CONFIG_INTENT_RESTART_LIMIT:
        expected->restart_limit = value;
        break;
    case INTEGRATION_CONFIG_INTENT_SERVICE_HEALTH_TIMEOUT_MS:
        expected->service_health_timeout_ms = value;
        break;
    default:
        break;
    }
}

static void test_every_intent_applies(void)
{
    static const struct {
        uint32_t intent_id;
        uint32_t value;
    } cases[INTEGRATION_CONFIG_INTENT_COUNT] = {
        { INTEGRATION_CONFIG_INTENT_RECOVERY_MODE, 2U },
        { INTEGRATION_CONFIG_INTENT_MAX_PROCESSES, 48U },
        { INTEGRATION_CONFIG_INTENT_MAX_USER_MEMORY_PAGES, 1024U },
        { INTEGRATION_CONFIG_INTENT_AUDIT_RING_CAPACITY, 32U },
        { INTEGRATION_CONFIG_INTENT_BOOT_TIMEOUT_MS, 7000U },
        { INTEGRATION_CONFIG_INTENT_RESTART_LIMIT, 5U },
        { INTEGRATION_CONFIG_INTENT_SERVICE_HEALTH_TIMEOUT_MS, 12000U },
    };
    struct authorization_matrix matrix = make_matrix();
    struct audit_correlation correlation = make_correlation();
    uint8_t admin_role[] = { AUTHORIZATION_ROLE_ADMIN };
    uint8_t config_capability[] = {
        AUTHORIZATION_CAPABILITY_CONFIG_WRITE
    };
    uint32_t index;

    for (index = 0U; index < INTEGRATION_CONFIG_INTENT_COUNT; ++index) {
        struct config_snapshot snapshot;
        struct event_ring ring;
        struct integration_config_intent intent = make_intent(
            cases[index].intent_id, cases[index].value);
        struct appliance_config expected;
        struct audit_record record;
        uint64_t sequence = UINT64_MAX;

        CHECK(fixture_initialize(&snapshot, &ring),
              "every-intent fixture initializes snapshot and ring");
        expected = *config_snapshot_active(&snapshot);
        apply_expected_field(&expected,
                             cases[index].intent_id,
                             cases[index].value);
        CHECK(integration_config_intent_apply(
                  &snapshot, &intent, &matrix,
                  admin_role, 1U, config_capability, 1U,
                  &correlation, commit_accept, NULL,
                  &ring, &sequence) ==
                  INTEGRATION_CONFIG_INTENT_OK &&
                  sequence == 0U,
              "every defined inbound config intent applies");
        CHECK(config_equal(config_snapshot_active(&snapshot), &expected) &&
                  !config_snapshot_has_pending(&snapshot),
              "every applied inbound config intent publishes exact active state");
        CHECK(event_ring_count(&ring) == 1U &&
                  audit_record_pop(&ring, &record) == AUDIT_OK &&
                  management_record_matches(
                      &record, &correlation,
                      AUDIT_CORRELATION_RESULT_OK,
                      CONTROLLED_CONFIGURATION_COMMAND_ID),
              "every applied inbound config intent publishes one success audit");
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "every-intent apply proof performs no allocation");
}

static void test_commit_rejection(void)
{
    struct config_snapshot snapshot;
    struct event_ring ring;
    struct authorization_matrix matrix = make_matrix();
    struct integration_config_intent intent =
        make_intent(INTEGRATION_CONFIG_INTENT_MAX_PROCESSES, 48U);
    struct audit_correlation correlation = make_correlation();
    struct appliance_config active_before;
    struct audit_record record;
    uint8_t admin_role[] = { AUTHORIZATION_ROLE_ADMIN };
    uint8_t config_capability[] = {
        AUTHORIZATION_CAPABILITY_CONFIG_WRITE
    };
    uint64_t sequence = UINT64_MAX;

    CHECK(fixture_initialize(&snapshot, &ring),
          "commit-rejection fixture initializes snapshot and ring");
    active_before = *config_snapshot_active(&snapshot);
    CHECK(integration_config_intent_apply(
              &snapshot, &intent, &matrix,
              admin_role, 1U, config_capability, 1U,
              &correlation, commit_reject, NULL,
              &ring, &sequence) ==
              INTEGRATION_CONFIG_INTENT_COMMIT_FAILED &&
              sequence == 0U,
          "commit rejection is surfaced as explicit intent failure");
    CHECK(config_equal(config_snapshot_active(&snapshot), &active_before) &&
              !config_snapshot_has_pending(&snapshot),
          "commit rejection preserves active config and clears pending");
    CHECK(event_ring_count(&ring) == 1U &&
              audit_record_pop(&ring, &record) == AUDIT_OK &&
              management_record_matches(
                  &record, &correlation,
                  INTEGRATION_CONFIG_INTENT_AUDIT_RESULT_DENIED,
                  CONTROLLED_CONFIGURATION_COMMAND_ID),
          "commit rejection publishes denied audit record");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "commit rejection performs no allocation");
}

static void test_status_strings(void)
{
    enum integration_config_intent_status status;

    for (status = INTEGRATION_CONFIG_INTENT_OK;
         status <= INTEGRATION_CONFIG_INTENT_AUDIT_FAILED; ++status) {
        const char *string =
            integration_config_intent_status_string(status);

        CHECK(string != NULL &&
                  (status == INTEGRATION_CONFIG_INTENT_OK ?
                       strcmp(string,
                              "INTEGRATION_CONFIG_INTENT_OK") == 0 :
                   status == INTEGRATION_CONFIG_INTENT_NULL ?
                       strcmp(string,
                              "INTEGRATION_CONFIG_INTENT_NULL") == 0 :
                   status == INTEGRATION_CONFIG_INTENT_BAD_STATE ?
                       strcmp(string,
                              "INTEGRATION_CONFIG_INTENT_BAD_STATE") == 0 :
                   status == INTEGRATION_CONFIG_INTENT_BAD_VERSION ?
                       strcmp(string,
                              "INTEGRATION_CONFIG_INTENT_BAD_VERSION") == 0 :
                   status == INTEGRATION_CONFIG_INTENT_INVALID_PEER_ID ?
                       strcmp(string,
                              "INTEGRATION_CONFIG_INTENT_INVALID_PEER_ID") == 0 :
                   status == INTEGRATION_CONFIG_INTENT_INVALID_INTENT_ID ?
                       strcmp(string,
                              "INTEGRATION_CONFIG_INTENT_INVALID_INTENT_ID") == 0 :
                   status == INTEGRATION_CONFIG_INTENT_INVALID_VALUE ?
                       strcmp(string,
                              "INTEGRATION_CONFIG_INTENT_INVALID_VALUE") == 0 :
                   status == INTEGRATION_CONFIG_INTENT_DENIED ?
                       strcmp(string,
                              "INTEGRATION_CONFIG_INTENT_DENIED") == 0 :
                   status == INTEGRATION_CONFIG_INTENT_PARSE_FAILED ?
                       strcmp(string,
                              "INTEGRATION_CONFIG_INTENT_PARSE_FAILED") == 0 :
                   status == INTEGRATION_CONFIG_INTENT_COMMIT_FAILED ?
                       strcmp(string,
                              "INTEGRATION_CONFIG_INTENT_COMMIT_FAILED") == 0 :
                       strcmp(string,
                              "INTEGRATION_CONFIG_INTENT_AUDIT_FAILED") == 0),
              "inbound config intent status string is exact");
    }
    CHECK(integration_config_intent_status_string(
              (enum integration_config_intent_status)11U) == NULL,
          "inbound config intent undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "inbound config intent status handling performs no allocation");
}

int main(void)
{
    test_exact_constants();
    test_validate();
    test_defined_ranges();
    test_integration_scope_denied();
    test_invalid_intent_audit_no_mutation();
    test_successful_apply();
    test_every_intent_applies();
    test_commit_rejection();
    test_status_strings();
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "inbound config intent host suite performs no allocation");
    if (failures != 0) {
        fprintf(stderr,
                "Integration config intent host tests: FAILED (%d)\n",
                failures);
        return 1;
    }
    printf("Integration config intent host tests: ALL OK\n");
    return 0;
}

#endif
