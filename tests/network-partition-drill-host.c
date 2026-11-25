#include "audit.h"
#include "event_ring.h"
#include "fault_injection.h"
#include "ipv4_config.h"
#include "link_state.h"
#include "management_command.h"
#include "network_admission.h"
#include "network_config_transaction.h"
#include "network_device.h"
#include "network_event.h"
#include "network_health.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_NETWORK_PARTITION_DRILL_HOST_TEST

#define NETWORK_PARTITION_DEVICE_ID 7U
#define NETWORK_PARTITION_DEVICE_GENERATION 3U
#define NETWORK_PARTITION_LINK_GENERATION 5U
#define NETWORK_PARTITION_LOCAL_COMMAND_ID 1U
#define NETWORK_PARTITION_LINK_LOSS_DETAIL UINT64_C(0x000000000000C001)
#define NETWORK_PARTITION_PEER_SILENCE_DETAIL UINT64_C(0x000000000000C002)

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static bool init_device(struct network_device_selection *selection)
{
    struct network_device_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = NETWORK_DEVICE_SNAPSHOT_VERSION;
    snapshot.device_id = NETWORK_PARTITION_DEVICE_ID;
    snapshot.vendor_id = 0x1234U;
    snapshot.product_id = 0x5678U;
    snapshot.capability_mask = NETWORK_DEVICE_CAPABILITY_MASK;
    snapshot.mac_address[0] = 0x02U;
    snapshot.mac_address[5] = 0x01U;

    memset(selection, 0, sizeof(*selection));
    return network_device_selection_init(
               selection, &snapshot,
               NETWORK_PARTITION_DEVICE_GENERATION) == NETWORK_DEVICE_OK;
}

static bool init_link(struct network_link_selection *selection,
                      enum network_link_state state)
{
    struct network_link_state_record record;

    memset(&record, 0, sizeof(record));
    record.version = NETWORK_LINK_STATE_VERSION;
    record.device_id = NETWORK_PARTITION_DEVICE_ID;
    record.state = state;
    record.generation = NETWORK_PARTITION_LINK_GENERATION;

    memset(selection, 0, sizeof(*selection));
    return network_link_selection_init(
               selection, &record,
               NETWORK_PARTITION_DEVICE_GENERATION) == NETWORK_LINK_OK;
}

static bool init_config(struct network_config_transaction *transaction)
{
    static const char config[] =
        "schema_version=1\n"
        "ipv4_address=192.0.2.10\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=192.0.2.1\n"
        "ipv4_dns=192.0.2.53\n";
    struct network_ipv4_config parsed;

    memset(&parsed, 0, sizeof(parsed));
    if (network_ipv4_config_parse(
            config, sizeof(config) - 1U, &parsed) != NETWORK_IPV4_OK) {
        return false;
    }
    memset(transaction, 0, sizeof(*transaction));
    return network_config_transaction_init(
               transaction, &parsed) ==
               NETWORK_CONFIG_TRANSACTION_OK;
}

static bool init_admission(
    struct network_admission_selection *selection,
    enum network_admission_mode mode)
{
    struct network_admission_policy policy;

    memset(&policy, 0, sizeof(policy));
    policy.version = NETWORK_ADMISSION_VERSION;
    policy.mode = mode;
    policy.port = 3000U;
    policy.payload_max = 255U;

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

static bool local_command_valid(void)
{
    struct management_command_descriptor descriptor;
    struct management_command_registry registry;
    struct management_command_header header;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.version = MANAGEMENT_COMMAND_VERSION;
    descriptor.command_id = NETWORK_PARTITION_LOCAL_COMMAND_ID;
    descriptor.request_min_length = 0U;
    descriptor.request_max_length = 0U;
    descriptor.response_min_length = 1U;
    descriptor.response_max_length = 64U;
    if (management_command_registry_init(
            &registry, &descriptor, 1U) != MANAGEMENT_COMMAND_OK) {
        return false;
    }

    memset(&header, 0, sizeof(header));
    header.version = MANAGEMENT_COMMAND_VERSION;
    header.command_id = NETWORK_PARTITION_LOCAL_COMMAND_ID;
    return management_command_validate_frame(
               &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
               &header, NULL, 0U,
               MANAGEMENT_COMMAND_HEADER_LENGTH) ==
               MANAGEMENT_COMMAND_OK;
}

static void test_network_fault_catalogue(void)
{
    const struct fault_injection_site *site = NULL;

    CHECK(fault_injection_site_by_id(
              FAULT_INJECTION_NETWORK_LINK_LOSS, &site) ==
              FAULT_INJECTION_OK &&
              site->subsystem == FAULT_INJECTION_SUBSYSTEM_NETWORK &&
              site->bounded && site->test_only &&
              site->expected_outcome != NULL &&
              site->expected_outcome[0] != '\0',
          "network link-loss fault point is exact");
    CHECK(fault_injection_site_by_id(
              FAULT_INJECTION_NETWORK_PEER_SILENCE, &site) ==
              FAULT_INJECTION_OK &&
              site->subsystem == FAULT_INJECTION_SUBSYSTEM_NETWORK &&
              site->bounded && site->test_only &&
              site->expected_outcome != NULL &&
              site->expected_outcome[0] != '\0',
          "network peer-silence fault point is exact");
}

static void test_link_loss_preserves_local_operation(void)
{
    static const enum network_link_state failed_states[] = {
        NETWORK_LINK_DOWN,
        NETWORK_LINK_FAILED,
    };
    struct network_device_selection device;
    struct network_link_selection link;
    struct network_config_transaction config;
    struct network_admission_selection admission;
    struct network_health_input input;
    struct network_health_verdict verdict;
    size_t index;

    CHECK(init_device(&device) &&
              init_link(&link, NETWORK_LINK_UP) &&
              init_config(&config) &&
              init_admission(
                  &admission, NETWORK_ADMISSION_MODE_ROLE_SESSION),
          "network partition drill initializes published selections");
    input = make_input(&device, &link, &config, &admission);
    memset(&verdict, 0xA5, sizeof(verdict));

    CHECK(network_link_path_status(
              &link, &device.device,
              NETWORK_PARTITION_DEVICE_GENERATION,
              NETWORK_PARTITION_LINK_GENERATION) == NETWORK_LINK_OK &&
              network_link_path_enabled(
                  &link, &device.device,
                  NETWORK_PARTITION_DEVICE_GENERATION,
                  NETWORK_PARTITION_LINK_GENERATION) &&
              network_health_evaluate(&input, &verdict) ==
                  NETWORK_HEALTH_OK &&
              verdict.healthy &&
              verdict.result == HEALTH_CHECK_RESULT_PASS &&
              verdict.fault == NETWORK_HEALTH_FAULT_NONE &&
              verdict.local_control_available,
          "up link publishes healthy packet path with local control");

    for (index = 0U;
         index < sizeof(failed_states) / sizeof(failed_states[0]);
         ++index) {
        link.link.state = failed_states[index];
        memset(&verdict, 0xA5, sizeof(verdict));
        CHECK(network_link_path_status(
                  &link, &device.device,
                  NETWORK_PARTITION_DEVICE_GENERATION,
                  NETWORK_PARTITION_LINK_GENERATION) ==
                  NETWORK_LINK_NOT_UP &&
                  !network_link_path_enabled(
                      &link, &device.device,
                      NETWORK_PARTITION_DEVICE_GENERATION,
                      NETWORK_PARTITION_LINK_GENERATION) &&
                  network_health_evaluate(&input, &verdict) ==
                      NETWORK_HEALTH_OK &&
                  !verdict.healthy &&
                  verdict.result == HEALTH_CHECK_RESULT_FAIL &&
                  verdict.severity == HEALTH_CHECK_SEVERITY_CRITICAL &&
                  verdict.fault == NETWORK_HEALTH_FAULT_LINK &&
                  verdict.link_state == failed_states[index] &&
                  verdict.local_control_available,
              "link loss disables packet path without losing local control");
    }
    CHECK(local_command_valid(),
          "local management command still validates after link loss");
}

static struct network_event make_partition_event(
    uint32_t type,
    uint64_t detail)
{
    struct network_event event;

    memset(&event, 0, sizeof(event));
    event.version = NETWORK_EVENT_VERSION;
    event.type = type;
    event.result = NETWORK_EVENT_RESULT_FAILURE;
    event.device_id = NETWORK_PARTITION_DEVICE_ID;
    event.generation = NETWORK_PARTITION_LINK_GENERATION;
    event.detail = detail;
    return event;
}

static void test_peer_silence_audit_and_local_operation(void)
{
    struct event_ring ring;
    struct network_event link_event = make_partition_event(
        NETWORK_EVENT_TYPE_LINK, NETWORK_PARTITION_LINK_LOSS_DETAIL);
    struct network_event peer_event = make_partition_event(
        NETWORK_EVENT_TYPE_TRANSPORT,
        NETWORK_PARTITION_PEER_SILENCE_DETAIL);
    struct network_event events[2];
    struct network_event popped;
    uint64_t sequence = UINT64_MAX;
    uint32_t count = UINT32_MAX;

    memset(&ring, 0, sizeof(ring));
    CHECK(event_ring_init(&ring) == EVENT_RING_OK &&
              network_event_append(&ring, &link_event, &sequence) ==
                  NETWORK_EVENT_OK &&
              sequence == 0U &&
              network_event_append(&ring, &peer_event, NULL) ==
                  NETWORK_EVENT_OK &&
              event_ring_count(&ring) == 2U,
          "peer silence drill appends bounded network failure events");

    count = UINT32_MAX;
    CHECK(network_event_snapshot(
              &ring, events, 2U, &count) == NETWORK_EVENT_OK &&
              count == 2U &&
              events[0].type == NETWORK_EVENT_TYPE_LINK &&
              events[0].result == NETWORK_EVENT_RESULT_FAILURE &&
              events[0].device_id == NETWORK_PARTITION_DEVICE_ID &&
              events[0].generation == NETWORK_PARTITION_LINK_GENERATION &&
              events[0].detail == NETWORK_PARTITION_LINK_LOSS_DETAIL &&
              events[1].type == NETWORK_EVENT_TYPE_TRANSPORT &&
              events[1].result == NETWORK_EVENT_RESULT_FAILURE &&
              events[1].device_id == NETWORK_PARTITION_DEVICE_ID &&
              events[1].generation == NETWORK_PARTITION_LINK_GENERATION &&
              events[1].detail ==
                  NETWORK_PARTITION_PEER_SILENCE_DETAIL,
          "peer silence snapshot retains exact failure evidence");

    CHECK(network_event_pop(&ring, &popped) == NETWORK_EVENT_OK &&
              popped.type == NETWORK_EVENT_TYPE_LINK &&
              popped.detail == NETWORK_PARTITION_LINK_LOSS_DETAIL &&
              network_event_pop(&ring, &popped) == NETWORK_EVENT_OK &&
              popped.type == NETWORK_EVENT_TYPE_TRANSPORT &&
              popped.detail == NETWORK_PARTITION_PEER_SILENCE_DETAIL &&
              event_ring_count(&ring) == 0U &&
              network_event_pop(&ring, &popped) == NETWORK_EVENT_EMPTY,
          "peer silence evidence is consumed exactly once without residue");
    CHECK(local_command_valid(),
          "local management command still validates after peer silence");
}

int main(void)
{
    test_network_fault_catalogue();
    test_link_loss_preserves_local_operation();
    test_peer_silence_audit_and_local_operation();

    if (failures != 0) {
        fprintf(stderr,
                "Network partition drill host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Network partition drill host tests: ALL OK\n");
    return 0;
}

#else
#error "network-partition-drill-host.c must be compiled with HFOC_NETWORK_PARTITION_DRILL_HOST_TEST"
#endif
