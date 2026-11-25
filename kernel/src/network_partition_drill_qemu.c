#ifdef HFOC_NETWORK_PARTITION_DRILL_QEMU_TEST

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
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETWORK_PARTITION_DEVICE_ID 1U
#define NETWORK_PARTITION_DEVICE_GENERATION 3U
#define NETWORK_PARTITION_LINK_GENERATION 5U
#define NETWORK_PARTITION_LOCAL_COMMAND_ID 1U
#define NETWORK_PARTITION_LINK_LOSS_DETAIL UINT64_C(0x000000000000C001)
#define NETWORK_PARTITION_PEER_SILENCE_DETAIL UINT64_C(0x000000000000C002)

static void network_partition_drill_fail(const char *message)
    __attribute__((noreturn));

static void network_partition_drill_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[NETWORK-PARTITION-DRILL] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void network_partition_drill_require(
    bool condition,
    const char *message)
{
    if (!condition) {
        network_partition_drill_fail(message);
    }
}

static void network_partition_drill_zero(
    void *destination,
    size_t length)
{
    unsigned char *const bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool network_partition_drill_init_device(
    struct network_device_selection *selection)
{
    struct network_device_snapshot snapshot;

    network_partition_drill_zero(&snapshot, sizeof(snapshot));
    snapshot.version = NETWORK_DEVICE_SNAPSHOT_VERSION;
    snapshot.device_id = NETWORK_PARTITION_DEVICE_ID;
    snapshot.vendor_id = 0x1234U;
    snapshot.product_id = 0x5678U;
    snapshot.capability_mask = NETWORK_DEVICE_CAPABILITY_MASK;
    snapshot.mac_address[0] = 0x02U;
    snapshot.mac_address[5] = 0x01U;

    network_partition_drill_zero(selection, sizeof(*selection));
    return network_device_selection_init(
               selection, &snapshot,
               NETWORK_PARTITION_DEVICE_GENERATION) == NETWORK_DEVICE_OK;
}

static bool network_partition_drill_init_link(
    struct network_link_selection *selection,
    enum network_link_state state)
{
    struct network_link_state_record record;

    network_partition_drill_zero(&record, sizeof(record));
    record.version = NETWORK_LINK_STATE_VERSION;
    record.device_id = NETWORK_PARTITION_DEVICE_ID;
    record.state = state;
    record.generation = NETWORK_PARTITION_LINK_GENERATION;

    network_partition_drill_zero(selection, sizeof(*selection));
    return network_link_selection_init(
               selection, &record,
               NETWORK_PARTITION_DEVICE_GENERATION) == NETWORK_LINK_OK;
}

static bool network_partition_drill_init_config(
    struct network_config_transaction *transaction)
{
    static const char config[] =
        "schema_version=1\n"
        "ipv4_address=192.0.2.10\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=192.0.2.1\n"
        "ipv4_dns=192.0.2.53\n";
    struct network_ipv4_config parsed;

    network_partition_drill_zero(&parsed, sizeof(parsed));
    if (network_ipv4_config_parse(
            config, sizeof(config) - 1U, &parsed) != NETWORK_IPV4_OK) {
        return false;
    }
    network_partition_drill_zero(transaction, sizeof(*transaction));
    return network_config_transaction_init(
               transaction, &parsed) ==
               NETWORK_CONFIG_TRANSACTION_OK;
}

static bool network_partition_drill_init_admission(
    struct network_admission_selection *selection,
    enum network_admission_mode mode)
{
    struct network_admission_policy policy;

    network_partition_drill_zero(&policy, sizeof(policy));
    policy.version = NETWORK_ADMISSION_VERSION;
    policy.mode = mode;
    policy.port = 3000U;
    policy.payload_max = 255U;

    network_partition_drill_zero(selection, sizeof(*selection));
    return network_admission_selection_init(
               selection, &policy) == NETWORK_ADMISSION_OK;
}

static void network_partition_drill_make_input(
    struct network_health_input *input,
    const struct network_device_selection *device,
    const struct network_link_selection *link,
    const struct network_config_transaction *config,
    const struct network_admission_selection *admission)
{
    network_partition_drill_zero(input, sizeof(*input));
    input->version = NETWORK_HEALTH_VERSION;
    input->device = device;
    input->link = link;
    input->config = config;
    input->admission = admission;
}

static bool network_partition_drill_local_command_valid(void)
{
    struct management_command_descriptor descriptor;
    struct management_command_registry registry;
    struct management_command_header header;

    network_partition_drill_zero(&descriptor, sizeof(descriptor));
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

    network_partition_drill_zero(&header, sizeof(header));
    header.version = MANAGEMENT_COMMAND_VERSION;
    header.command_id = NETWORK_PARTITION_LOCAL_COMMAND_ID;
    return management_command_validate_frame(
               &registry, MANAGEMENT_COMMAND_DIRECTION_REQUEST,
               &header, NULL, 0U,
               MANAGEMENT_COMMAND_HEADER_LENGTH) ==
               MANAGEMENT_COMMAND_OK;
}

static void network_partition_drill_check_faults(void)
{
    const struct fault_injection_site *site = NULL;

    network_partition_drill_require(
        fault_injection_site_by_id(
            FAULT_INJECTION_NETWORK_LINK_LOSS, &site) ==
            FAULT_INJECTION_OK &&
            site->subsystem == FAULT_INJECTION_SUBSYSTEM_NETWORK &&
            site->bounded && site->test_only &&
        fault_injection_site_by_id(
            FAULT_INJECTION_NETWORK_PEER_SILENCE, &site) ==
            FAULT_INJECTION_OK &&
            site->subsystem == FAULT_INJECTION_SUBSYSTEM_NETWORK &&
            site->bounded && site->test_only &&
            site->expected_outcome != NULL &&
            site->expected_outcome[0] != '\0',
        "catalogue network fault points");
}

static void network_partition_drill_make_event(
    struct network_event *event,
    uint32_t type,
    uint64_t detail)
{
    network_partition_drill_zero(event, sizeof(*event));
    event->version = NETWORK_EVENT_VERSION;
    event->type = type;
    event->result = NETWORK_EVENT_RESULT_FAILURE;
    event->device_id = NETWORK_PARTITION_DEVICE_ID;
    event->generation = NETWORK_PARTITION_LINK_GENERATION;
    event->detail = detail;
}

void network_partition_drill_qemu_test_init(void)
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
    struct event_ring ring;
    struct network_event link_event;
    struct network_event peer_event;
    struct network_event events[2];
    struct network_event popped;
    uint64_t sequence = 0U;
    uint32_t count = 0U;
    size_t index;

    serial_write("[NETWORK-PARTITION-DRILL] proof init\n");
    network_partition_drill_check_faults();
    serial_write(
        "[NETWORK-PARTITION-DRILL] catalogue network fault points exact\n");

    network_partition_drill_require(
        network_partition_drill_init_device(&device) &&
        network_partition_drill_init_link(&link, NETWORK_LINK_UP) &&
        network_partition_drill_init_config(&config) &&
        network_partition_drill_init_admission(
            &admission, NETWORK_ADMISSION_MODE_ROLE_SESSION),
        "published network selections");
    network_partition_drill_make_input(
        &input, &device, &link, &config, &admission);
    network_partition_drill_zero(&verdict, sizeof(verdict));
    network_partition_drill_require(
        network_link_path_status(
            &link, &device.device,
            NETWORK_PARTITION_DEVICE_GENERATION,
            NETWORK_PARTITION_LINK_GENERATION) == NETWORK_LINK_OK &&
        network_link_path_enabled(
            &link, &device.device,
            NETWORK_PARTITION_DEVICE_GENERATION,
            NETWORK_PARTITION_LINK_GENERATION) &&
        network_health_evaluate(&input, &verdict) == NETWORK_HEALTH_OK &&
        verdict.healthy &&
        verdict.result == HEALTH_CHECK_RESULT_PASS &&
        verdict.fault == NETWORK_HEALTH_FAULT_NONE &&
        verdict.local_control_available,
        "healthy packet path");
    serial_write("[NETWORK-PARTITION-DRILL] healthy path verified\n");

    for (index = 0U;
         index < sizeof(failed_states) / sizeof(failed_states[0]);
         ++index) {
        link.link.state = failed_states[index];
        network_partition_drill_zero(&verdict, sizeof(verdict));
        network_partition_drill_require(
            network_link_path_status(
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
            "link loss health");
    }
    network_partition_drill_require(
        network_partition_drill_local_command_valid(),
        "local command after link loss");
    serial_write(
        "[NETWORK-PARTITION-DRILL] link loss health preserved\n");

    network_partition_drill_zero(&ring, sizeof(ring));
    network_partition_drill_require(
        event_ring_init(&ring) == EVENT_RING_OK,
        "event ring fixture");
    network_partition_drill_make_event(
        &link_event, NETWORK_EVENT_TYPE_LINK,
        NETWORK_PARTITION_LINK_LOSS_DETAIL);
    network_partition_drill_make_event(
        &peer_event, NETWORK_EVENT_TYPE_TRANSPORT,
        NETWORK_PARTITION_PEER_SILENCE_DETAIL);
    network_partition_drill_require(
        network_event_append(&ring, &link_event, &sequence) ==
            NETWORK_EVENT_OK &&
        sequence == 0U &&
        network_event_append(&ring, &peer_event, NULL) ==
            NETWORK_EVENT_OK &&
        event_ring_count(&ring) == 2U,
        "bounded network failure events");
    network_partition_drill_require(
        network_event_snapshot(
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
        events[1].detail == NETWORK_PARTITION_PEER_SILENCE_DETAIL,
        "peer silence audit snapshot");
    network_partition_drill_require(
        network_event_pop(&ring, &popped) == NETWORK_EVENT_OK &&
        popped.type == NETWORK_EVENT_TYPE_LINK &&
        popped.detail == NETWORK_PARTITION_LINK_LOSS_DETAIL &&
        network_event_pop(&ring, &popped) == NETWORK_EVENT_OK &&
        popped.type == NETWORK_EVENT_TYPE_TRANSPORT &&
        popped.detail == NETWORK_PARTITION_PEER_SILENCE_DETAIL &&
        event_ring_count(&ring) == 0U &&
        network_event_pop(&ring, &popped) == NETWORK_EVENT_EMPTY,
        "peer silence exact-once evidence");
    network_partition_drill_require(
        network_partition_drill_local_command_valid(),
        "local command after peer silence");
    serial_write(
        "[NETWORK-PARTITION-DRILL] peer silence evidence bounded\n");
    serial_write(
        "[NETWORK-PARTITION-DRILL] local operation preserved\n");
    serial_write("[NETWORK-PARTITION-DRILL] proof OK\n");
}

#else
typedef int network_partition_drill_qemu_translation_unit_guard;
#endif
