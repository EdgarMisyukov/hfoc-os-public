#include "network_health.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_NETWORK_HEALTH_HOST_TEST

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

static const char valid_config[] =
    "schema_version=1\n"
    "ipv4_address=192.0.2.10\n"
    "ipv4_prefix=24\n"
    "ipv4_gateway=192.0.2.1\n"
    "ipv4_dns=192.0.2.53\n";

static const char replacement_config[] =
    "schema_version=1\n"
    "ipv4_address=192.0.2.11\n"
    "ipv4_prefix=24\n"
    "ipv4_gateway=192.0.2.1\n"
    "ipv4_dns=192.0.2.53\n";

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

static struct network_device_snapshot make_device_snapshot(
    uint32_t device_id,
    uint32_t capability_mask)
{
    struct network_device_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = NETWORK_DEVICE_SNAPSHOT_VERSION;
    snapshot.device_id = device_id;
    snapshot.vendor_id = 0x1234U;
    snapshot.product_id = 0x5678U;
    snapshot.bus_address = 0U;
    snapshot.capability_mask = capability_mask;
    snapshot.mac_address[0] = 0x02U;
    snapshot.mac_address[1] = 0x00U;
    snapshot.mac_address[2] = 0x00U;
    snapshot.mac_address[3] = 0x00U;
    snapshot.mac_address[4] = 0x00U;
    snapshot.mac_address[5] = 0x01U;
    return snapshot;
}

static struct network_link_state_record make_link_record(
    uint32_t device_id,
    enum network_link_state state,
    uint32_t generation)
{
    struct network_link_state_record link;

    memset(&link, 0, sizeof(link));
    link.version = NETWORK_LINK_STATE_VERSION;
    link.device_id = device_id;
    link.state = state;
    link.generation = generation;
    return link;
}

static struct network_admission_policy make_admission_policy(
    enum network_admission_mode mode)
{
    struct network_admission_policy policy;

    memset(&policy, 0, sizeof(policy));
    policy.version = NETWORK_ADMISSION_VERSION;
    policy.mode = mode;
    policy.port = 3000U;
    policy.payload_max = 255U;
    return policy;
}

static bool init_device(
    struct network_device_selection *selection,
    uint32_t device_id,
    uint32_t capability_mask,
    uint32_t generation)
{
    struct network_device_snapshot snapshot =
        make_device_snapshot(device_id, capability_mask);

    memset(selection, 0, sizeof(*selection));
    return network_device_selection_init(
               selection, &snapshot, generation) == NETWORK_DEVICE_OK;
}

static bool init_link(
    struct network_link_selection *selection,
    uint32_t device_id,
    enum network_link_state state,
    uint32_t selection_generation,
    uint32_t link_generation)
{
    struct network_link_state_record link =
        make_link_record(device_id, state, link_generation);

    memset(selection, 0, sizeof(*selection));
    return network_link_selection_init(
               selection, &link, selection_generation) == NETWORK_LINK_OK;
}

static bool init_config(
    struct network_config_transaction *transaction)
{
    struct network_ipv4_config config;

    memset(transaction, 0, sizeof(*transaction));
    if (network_ipv4_config_parse(
            valid_config, sizeof(valid_config) - 1U,
            &config) != NETWORK_IPV4_OK) {
        return false;
    }
    return network_config_transaction_init(
               transaction, &config) ==
           NETWORK_CONFIG_TRANSACTION_OK;
}

static bool stage_pending(
    struct network_config_transaction *transaction)
{
    return network_config_transaction_stage(
               transaction, replacement_config,
               sizeof(replacement_config) - 1U) ==
           NETWORK_CONFIG_TRANSACTION_OK;
}

static bool init_admission(
    struct network_admission_selection *selection,
    enum network_admission_mode mode)
{
    struct network_admission_policy policy =
        make_admission_policy(mode);

    memset(selection, 0, sizeof(*selection));
    return network_admission_selection_init(
               selection, &policy) == NETWORK_ADMISSION_OK;
}

static struct network_health_input make_input(
    const struct network_device_selection *device,
    const struct network_link_selection *link,
    const struct network_config_transaction *config,
    const struct network_admission_selection *admission)
{
    struct network_health_input input;

    memset(&input, 0, sizeof(input));
    input.version = NETWORK_HEALTH_VERSION;
    input.device = device;
    input.link = link;
    input.config = config;
    input.admission = admission;
    return input;
}

static void expect_bad_state(
    const struct network_health_input *input,
    const char *message)
{
    struct network_health_verdict verdict;
    unsigned char before[sizeof(verdict)];

    memset(&verdict, 0xA5, sizeof(verdict));
    memcpy(before, &verdict, sizeof(before));
    CHECK(network_health_evaluate(input, &verdict) ==
              NETWORK_HEALTH_BAD_STATE &&
              memcmp(&verdict, before, sizeof(before)) == 0,
          message);
}

static void test_status_and_fault_strings(void)
{
    static const struct {
        enum network_health_status status;
        const char *string;
    } statuses[] = {
        {NETWORK_HEALTH_OK, "NETWORK_HEALTH_OK"},
        {NETWORK_HEALTH_NULL, "NETWORK_HEALTH_NULL"},
        {NETWORK_HEALTH_BAD_VERSION, "NETWORK_HEALTH_BAD_VERSION"},
        {NETWORK_HEALTH_BAD_STATE, "NETWORK_HEALTH_BAD_STATE"},
    };
    static const struct {
        enum network_health_fault fault;
        const char *string;
    } faults[] = {
        {NETWORK_HEALTH_FAULT_NONE, "NETWORK_HEALTH_FAULT_NONE"},
        {NETWORK_HEALTH_FAULT_LINK, "NETWORK_HEALTH_FAULT_LINK"},
        {NETWORK_HEALTH_FAULT_CONFIG, "NETWORK_HEALTH_FAULT_CONFIG"},
        {NETWORK_HEALTH_FAULT_ADMISSION,
         "NETWORK_HEALTH_FAULT_ADMISSION"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = network_health_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "network health status string is exact");
    }
    CHECK(network_health_status_string(
              (enum network_health_status)4U) == NULL,
          "network health undefined status is rejected");

    for (index = 0U;
         index < sizeof(faults) / sizeof(faults[0]);
         ++index) {
        const char *string = network_health_fault_string(
            faults[index].fault);

        CHECK(string != NULL &&
                  strcmp(string, faults[index].string) == 0,
              "network health fault string is exact");
    }
    CHECK(network_health_fault_string(
              (enum network_health_fault)4U) == NULL,
          "network health undefined fault is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "network health strings perform no allocation");
}

static void test_null_and_version_failures(void)
{
    struct network_device_selection device;
    struct network_link_selection link;
    struct network_config_transaction config;
    struct network_admission_selection admission;
    struct network_health_input input;
    struct network_health_verdict verdict;

    CHECK(init_device(&device, 7U, NETWORK_DEVICE_CAPABILITY_MASK, 3U),
          "network health null fixture publishes device");
    CHECK(init_link(&link, 7U, NETWORK_LINK_UP, 3U, 5U),
          "network health null fixture publishes link");
    CHECK(init_config(&config),
          "network health null fixture publishes config");
    CHECK(init_admission(&admission, NETWORK_ADMISSION_MODE_ROLE_SESSION),
          "network health null fixture publishes admission");
    input = make_input(&device, &link, &config, &admission);
    memset(&verdict, 0xA5, sizeof(verdict));

    CHECK(network_health_evaluate(NULL, &verdict) ==
              NETWORK_HEALTH_NULL,
          "network health evaluate rejects null input");
    CHECK(network_health_evaluate(&input, NULL) ==
              NETWORK_HEALTH_NULL,
          "network health evaluate rejects null verdict");

    input.version = 0U;
    CHECK(network_health_evaluate(&input, &verdict) ==
              NETWORK_HEALTH_BAD_VERSION,
          "network health evaluate rejects zero version");
    input.version = NETWORK_HEALTH_VERSION + 1U;
    CHECK(network_health_evaluate(&input, &verdict) ==
              NETWORK_HEALTH_BAD_VERSION,
          "network health evaluate rejects future version");

    input = make_input(&device, &link, &config, &admission);
    input.device = NULL;
    CHECK(network_health_evaluate(&input, &verdict) ==
              NETWORK_HEALTH_NULL,
          "network health evaluate rejects null device");
    input = make_input(&device, &link, &config, &admission);
    input.link = NULL;
    CHECK(network_health_evaluate(&input, &verdict) ==
              NETWORK_HEALTH_NULL,
          "network health evaluate rejects null link");
    input = make_input(&device, &link, &config, &admission);
    input.config = NULL;
    CHECK(network_health_evaluate(&input, &verdict) ==
              NETWORK_HEALTH_NULL,
          "network health evaluate rejects null config");
    input = make_input(&device, &link, &config, &admission);
    input.admission = NULL;
    CHECK(network_health_evaluate(&input, &verdict) ==
              NETWORK_HEALTH_NULL,
          "network health evaluate rejects null admission");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "network health null and version paths perform no allocation");
}

static void test_invalid_states_fail_closed(void)
{
    struct network_device_selection device;
    struct network_link_selection link;
    struct network_config_transaction config;
    struct network_admission_selection admission;
    struct network_health_input input;

    CHECK(init_device(&device, 7U, NETWORK_DEVICE_CAPABILITY_MASK, 3U),
          "invalid-state fixture publishes device");
    CHECK(init_link(&link, 7U, NETWORK_LINK_UP, 3U, 5U),
          "invalid-state fixture publishes link");
    CHECK(init_config(&config),
          "invalid-state fixture publishes config");
    CHECK(init_admission(&admission, NETWORK_ADMISSION_MODE_ROLE_SESSION),
          "invalid-state fixture publishes admission");
    input = make_input(&device, &link, &config, &admission);

    device.published = false;
    expect_bad_state(&input,
                     "network health rejects unpublished device");
    init_device(&device, 7U, NETWORK_DEVICE_CAPABILITY_MASK, 3U);
    device.version = NETWORK_DEVICE_SELECTION_VERSION + 1U;
    expect_bad_state(&input,
                     "network health rejects future device version");
    init_device(&device, 7U, NETWORK_DEVICE_CAPABILITY_MASK, 3U);
    device.device.device_id = 0U;
    expect_bad_state(&input,
                     "network health rejects invalid device snapshot");
    init_device(&device, 7U, NETWORK_DEVICE_CAPABILITY_MASK, 3U);
    device.generation = 0U;
    expect_bad_state(&input,
                     "network health rejects invalid device generation");

    init_device(&device, 7U, NETWORK_DEVICE_CAPABILITY_MASK, 3U);
    link.published = false;
    expect_bad_state(&input,
                     "network health rejects unpublished link");
    init_link(&link, 7U, NETWORK_LINK_UP, 3U, 5U);
    link.version = NETWORK_LINK_SELECTION_VERSION + 1U;
    expect_bad_state(&input,
                     "network health rejects future link version");
    init_link(&link, 7U, NETWORK_LINK_UP, 3U, 5U);
    link.link.state = (enum network_link_state)NETWORK_LINK_STATE_COUNT;
    expect_bad_state(&input,
                     "network health rejects invalid link state");
    init_link(&link, 7U, NETWORK_LINK_UP, 3U, 5U);
    link.generation = 0U;
    expect_bad_state(&input,
                     "network health rejects invalid link generation");
    init_link(&link, 7U, NETWORK_LINK_UP, 3U, 5U);
    link.link.device_id = 8U;
    expect_bad_state(&input,
                     "network health rejects device/link mismatch");
    init_link(&link, 7U, NETWORK_LINK_UP, 3U, 5U);
    device.device.capability_mask = NETWORK_DEVICE_CAPABILITY_RX;
    expect_bad_state(&input,
                     "network health rejects unsupported link capability");
    init_device(&device, 7U, NETWORK_DEVICE_CAPABILITY_MASK, 3U);

    config.version = NETWORK_CONFIG_TRANSACTION_VERSION + 1U;
    expect_bad_state(&input,
                     "network health rejects future config version");
    init_config(&config);
    config.has_active = false;
    expect_bad_state(&input,
                     "network health rejects missing active config");
    init_config(&config);
    config.active.schema_version = NETWORK_IPV4_CONFIG_VERSION + 1U;
    expect_bad_state(&input,
                     "network health rejects invalid active config");
    init_config(&config);
    CHECK(stage_pending(&config),
          "invalid-state fixture stages pending config");
    config.pending.schema_version = NETWORK_IPV4_CONFIG_VERSION + 1U;
    expect_bad_state(&input,
                     "network health rejects invalid pending config");

    init_config(&config);
    admission.published = false;
    expect_bad_state(&input,
                     "network health rejects unpublished admission");
    init_admission(&admission, NETWORK_ADMISSION_MODE_ROLE_SESSION);
    admission.version = NETWORK_ADMISSION_VERSION + 1U;
    expect_bad_state(&input,
                     "network health rejects future admission version");
    init_admission(&admission, NETWORK_ADMISSION_MODE_ROLE_SESSION);
    admission.policy.mode = (enum network_admission_mode)2U;
    expect_bad_state(&input,
                     "network health rejects invalid admission policy");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "invalid network state paths perform no allocation");
}

static void test_healthy_state(void)
{
    struct network_device_selection device;
    struct network_link_selection link;
    struct network_config_transaction config;
    struct network_admission_selection admission;
    struct network_health_input input;
    struct network_health_verdict verdict;

    CHECK(init_device(&device, 7U, NETWORK_DEVICE_CAPABILITY_MASK, 3U),
          "healthy fixture publishes device");
    CHECK(init_link(&link, 7U, NETWORK_LINK_UP, 3U, 5U),
          "healthy fixture publishes link");
    CHECK(init_config(&config),
          "healthy fixture publishes config");
    CHECK(init_admission(&admission, NETWORK_ADMISSION_MODE_ROLE_SESSION),
          "healthy fixture publishes admission");
    input = make_input(&device, &link, &config, &admission);
    memset(&verdict, 0xA5, sizeof(verdict));

    CHECK(network_health_evaluate(&input, &verdict) ==
              NETWORK_HEALTH_OK &&
              verdict.version == NETWORK_HEALTH_VERSION &&
              verdict.healthy &&
              verdict.result == HEALTH_CHECK_RESULT_PASS &&
              verdict.severity == HEALTH_CHECK_SEVERITY_INFO &&
              verdict.fault == NETWORK_HEALTH_FAULT_NONE &&
              verdict.device_id == 7U &&
              verdict.link_state == NETWORK_LINK_UP &&
              verdict.link_generation == 5U &&
              verdict.has_active_config &&
              !verdict.has_pending_config &&
              verdict.admission_published &&
              verdict.admission_mode ==
                  NETWORK_ADMISSION_MODE_ROLE_SESSION &&
              verdict.local_control_available,
          "healthy network state publishes PASS with local control");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "healthy network evaluation performs no allocation");
}

static void test_initializing_link(void)
{
    struct network_device_selection device;
    struct network_link_selection link;
    struct network_config_transaction config;
    struct network_admission_selection admission;
    struct network_health_input input;
    struct network_health_verdict verdict;

    CHECK(init_device(&device, 7U, NETWORK_DEVICE_CAPABILITY_MASK, 3U),
          "initializing fixture publishes device");
    CHECK(init_link(&link, 7U, NETWORK_LINK_INITIALIZING, 3U, 5U),
          "initializing fixture publishes link");
    CHECK(init_config(&config),
          "initializing fixture publishes config");
    CHECK(init_admission(&admission, NETWORK_ADMISSION_MODE_ROLE_SESSION),
          "initializing fixture publishes admission");
    input = make_input(&device, &link, &config, &admission);
    memset(&verdict, 0xA5, sizeof(verdict));

    CHECK(network_health_evaluate(&input, &verdict) ==
              NETWORK_HEALTH_OK &&
              !verdict.healthy &&
              verdict.result == HEALTH_CHECK_RESULT_UNKNOWN &&
              verdict.severity == HEALTH_CHECK_SEVERITY_INFO &&
              verdict.fault == NETWORK_HEALTH_FAULT_NONE &&
              verdict.link_state == NETWORK_LINK_INITIALIZING &&
              verdict.local_control_available,
          "initializing link publishes UNKNOWN without losing local control");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "initializing link evaluation performs no allocation");
}

static void test_link_failure_preserves_local_control(void)
{
    static const enum network_link_state states[] = {
        NETWORK_LINK_DOWN,
        NETWORK_LINK_FAILED,
    };
    size_t index;

    for (index = 0U;
         index < sizeof(states) / sizeof(states[0]);
         ++index) {
        struct network_device_selection device;
        struct network_link_selection link;
        struct network_config_transaction config;
        struct network_admission_selection admission;
        struct network_health_input input;
        struct network_health_verdict verdict;

        CHECK(init_device(
                  &device, 7U, NETWORK_DEVICE_CAPABILITY_MASK, 3U),
              "link-failure fixture publishes device");
        CHECK(init_link(
                  &link, 7U, states[index], 3U, 5U),
              "link-failure fixture publishes link");
        CHECK(init_config(&config),
              "link-failure fixture publishes config");
        CHECK(init_admission(
                  &admission, NETWORK_ADMISSION_MODE_ROLE_SESSION),
              "link-failure fixture publishes admission");
        input = make_input(&device, &link, &config, &admission);
        memset(&verdict, 0xA5, sizeof(verdict));

        CHECK(network_health_evaluate(&input, &verdict) ==
                  NETWORK_HEALTH_OK &&
                  !verdict.healthy &&
                  verdict.result == HEALTH_CHECK_RESULT_FAIL &&
                  verdict.severity == HEALTH_CHECK_SEVERITY_CRITICAL &&
                  verdict.fault == NETWORK_HEALTH_FAULT_LINK &&
                  verdict.link_state == states[index] &&
                  verdict.local_control_available,
              "failed link publishes FAIL while local control stays available");
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "failed link evaluation performs no allocation");
}

static void test_pending_config(void)
{
    struct network_device_selection device;
    struct network_link_selection link;
    struct network_config_transaction config;
    struct network_admission_selection admission;
    struct network_health_input input;
    struct network_health_verdict verdict;

    CHECK(init_device(&device, 7U, NETWORK_DEVICE_CAPABILITY_MASK, 3U),
          "pending-config fixture publishes device");
    CHECK(init_link(&link, 7U, NETWORK_LINK_UP, 3U, 5U),
          "pending-config fixture publishes link");
    CHECK(init_config(&config),
          "pending-config fixture publishes config");
    CHECK(stage_pending(&config),
          "pending-config fixture stages candidate");
    CHECK(init_admission(&admission, NETWORK_ADMISSION_MODE_ROLE_SESSION),
          "pending-config fixture publishes admission");
    input = make_input(&device, &link, &config, &admission);
    memset(&verdict, 0xA5, sizeof(verdict));

    CHECK(network_health_evaluate(&input, &verdict) ==
              NETWORK_HEALTH_OK &&
              !verdict.healthy &&
              verdict.result == HEALTH_CHECK_RESULT_WARN &&
              verdict.severity == HEALTH_CHECK_SEVERITY_WARNING &&
              verdict.fault == NETWORK_HEALTH_FAULT_CONFIG &&
              verdict.has_active_config &&
              verdict.has_pending_config &&
              verdict.local_control_available,
          "valid pending config publishes WARN with active config preserved");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "pending config evaluation performs no allocation");
}

static void test_deny_all_admission(void)
{
    struct network_device_selection device;
    struct network_link_selection link;
    struct network_config_transaction config;
    struct network_admission_selection admission;
    struct network_health_input input;
    struct network_health_verdict verdict;

    CHECK(init_device(&device, 7U, NETWORK_DEVICE_CAPABILITY_MASK, 3U),
          "deny-all fixture publishes device");
    CHECK(init_link(&link, 7U, NETWORK_LINK_UP, 3U, 5U),
          "deny-all fixture publishes link");
    CHECK(init_config(&config),
          "deny-all fixture publishes config");
    CHECK(init_admission(&admission, NETWORK_ADMISSION_MODE_DENY_ALL),
          "deny-all fixture publishes admission");
    input = make_input(&device, &link, &config, &admission);
    memset(&verdict, 0xA5, sizeof(verdict));

    CHECK(network_health_evaluate(&input, &verdict) ==
              NETWORK_HEALTH_OK &&
              !verdict.healthy &&
              verdict.result == HEALTH_CHECK_RESULT_UNKNOWN &&
              verdict.severity == HEALTH_CHECK_SEVERITY_INFO &&
              verdict.fault == NETWORK_HEALTH_FAULT_NONE &&
              verdict.admission_published &&
              verdict.admission_mode ==
                  NETWORK_ADMISSION_MODE_DENY_ALL &&
              verdict.local_control_available,
          "valid deny-all admission is UNKNOWN and local control remains");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "deny-all evaluation performs no allocation");
}

static void test_severity_ordering(void)
{
    struct network_device_selection device;
    struct network_link_selection link;
    struct network_config_transaction config;
    struct network_admission_selection admission;
    struct network_health_input input;
    struct network_health_verdict verdict;

    CHECK(init_device(&device, 7U, NETWORK_DEVICE_CAPABILITY_MASK, 3U),
          "ordering fixture publishes device");
    CHECK(init_link(&link, 7U, NETWORK_LINK_INITIALIZING, 3U, 5U),
          "ordering fixture publishes initializing link");
    CHECK(init_config(&config),
          "ordering fixture publishes config");
    CHECK(stage_pending(&config),
          "ordering fixture stages pending config");
    CHECK(init_admission(&admission, NETWORK_ADMISSION_MODE_ROLE_SESSION),
          "ordering fixture publishes admission");
    input = make_input(&device, &link, &config, &admission);
    memset(&verdict, 0xA5, sizeof(verdict));
    CHECK(network_health_evaluate(&input, &verdict) ==
              NETWORK_HEALTH_OK &&
              verdict.result == HEALTH_CHECK_RESULT_WARN &&
              verdict.severity == HEALTH_CHECK_SEVERITY_WARNING &&
              verdict.fault == NETWORK_HEALTH_FAULT_CONFIG,
          "pending config dominates initializing link");

    init_link(&link, 7U, NETWORK_LINK_DOWN, 3U, 5U);
    input = make_input(&device, &link, &config, &admission);
    memset(&verdict, 0xA5, sizeof(verdict));
    CHECK(network_health_evaluate(&input, &verdict) ==
              NETWORK_HEALTH_OK &&
              verdict.result == HEALTH_CHECK_RESULT_FAIL &&
              verdict.severity == HEALTH_CHECK_SEVERITY_CRITICAL &&
              verdict.fault == NETWORK_HEALTH_FAULT_LINK,
          "failed link dominates pending config");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "severity ordering evaluation performs no allocation");
}

int main(void)
{
    test_status_and_fault_strings();
    test_null_and_version_failures();
    test_invalid_states_fail_closed();
    test_healthy_state();
    test_initializing_link();
    test_link_failure_preserves_local_control();
    test_pending_config();
    test_deny_all_admission();
    test_severity_ordering();

    if (failures != 0) {
        fprintf(stderr, "Network health host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Network health host tests: ALL OK\n");
    return 0;
}

#endif
