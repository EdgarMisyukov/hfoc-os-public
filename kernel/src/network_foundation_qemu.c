#ifdef HFOC_NETWORK_FOUNDATION_QEMU_TEST

#include "appliance_status.h"
#include "authorization.h"
#include "event_ring.h"
#include "icmp_reachability.h"
#include "ipv4_config.h"
#include "link_state.h"
#include "management_command.h"
#include "management_transport.h"
#include "network_admission.h"
#include "network_config_transaction.h"
#include "network_device.h"
#include "network_event.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define NETWORK_FOUNDATION_DEVICE_ID 1U
#define NETWORK_FOUNDATION_DEVICE_GENERATION 7U
#define NETWORK_FOUNDATION_LINK_GENERATION 1U
#define NETWORK_FOUNDATION_PORT 8443U
#define NETWORK_FOUNDATION_PAYLOAD_MAX 64U
#define NETWORK_FOUNDATION_PEER_SESSION_ID 1U
#define NETWORK_FOUNDATION_PEER_PRINCIPAL_ID 1U
#define NETWORK_FOUNDATION_PEER_GENERATION 1U

static void network_foundation_qemu_fail(const char *message)
    __attribute__((noreturn));

static void network_foundation_qemu_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[NETWORK-FOUNDATION] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void network_foundation_qemu_require(
    bool condition,
    const char *message)
{
    if (!condition) {
        network_foundation_qemu_fail(message);
    }
}

static void network_foundation_qemu_zero(
    void *destination,
    size_t length)
{
    unsigned char *const bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void network_foundation_qemu_write_u32(
    uint8_t *bytes,
    uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xFFU);
    bytes[1] = (uint8_t)((value >> 8U) & 0xFFU);
    bytes[2] = (uint8_t)((value >> 16U) & 0xFFU);
    bytes[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

static void network_foundation_qemu_write_u16(
    uint8_t *bytes,
    uint16_t value)
{
    bytes[0] = (uint8_t)(value & 0xFFU);
    bytes[1] = (uint8_t)((value >> 8U) & 0xFFU);
}

static uint16_t network_foundation_qemu_checksum(
    const uint8_t *data,
    size_t length)
{
    uint32_t sum = 0U;
    size_t offset;

    for (offset = 0U; offset + 1U < length; offset += 2U) {
        sum += ((uint32_t)data[offset] << 8U) |
               (uint32_t)data[offset + 1U];
    }
    if ((length & 1U) != 0U) {
        sum += (uint32_t)data[length - 1U] << 8U;
    }
    while ((sum >> 16U) != 0U) {
        sum = (sum & 0xFFFFU) + (sum >> 16U);
    }
    return (uint16_t)~sum;
}

static void network_foundation_qemu_fill_icmp_request(
    uint8_t *request,
    size_t request_length)
{
    uint16_t checksum;

    network_foundation_qemu_zero(request, request_length);
    request[0] = (uint8_t)ICMP_REACHABILITY_TYPE_ECHO_REQUEST;
    request[4] = 0x34U;
    request[5] = 0x12U;
    request[6] = 1U;
    request[7] = 0U;
    request[8] = 'H';
    request[9] = 'F';
    request[10] = 'O';
    request[11] = 'C';
    checksum = network_foundation_qemu_checksum(
        request, request_length);
    request[2] = (uint8_t)(checksum >> 8U);
    request[3] = (uint8_t)(checksum & 0xFFU);
}

static bool network_foundation_qemu_config_matches_active(
    const struct network_config_transaction *transaction,
    uint8_t third_octet,
    uint8_t prefix)
{
    const struct network_ipv4_config *active =
        network_config_transaction_active(transaction);

    return active != NULL &&
           active->address.octets[0] == 10U &&
           active->address.octets[1] == 0U &&
           active->address.octets[2] == third_octet &&
           active->address.octets[3] == 2U &&
           active->prefix_length == prefix;
}

static void network_foundation_qemu_append_event(
    struct event_ring *ring,
    uint32_t type,
    uint32_t result,
    uint64_t detail)
{
    struct network_event event;

    network_foundation_qemu_zero(&event, sizeof(event));
    event.version = NETWORK_EVENT_VERSION;
    event.type = type;
    event.result = result;
    event.device_id = NETWORK_FOUNDATION_DEVICE_ID;
    event.generation = NETWORK_FOUNDATION_LINK_GENERATION;
    event.detail = detail;
    network_foundation_qemu_require(
        network_event_append(ring, &event, NULL) == NETWORK_EVENT_OK,
        "network event append");
}

void network_foundation_qemu_test_init(void)
{
    static const char initial_config_text[] =
        "schema_version=1\n"
        "ipv4_address=10.0.0.2\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=10.0.0.1\n"
        "ipv4_dns=10.0.0.1\n";
    static const char replacement_config_text[] =
        "schema_version=1\n"
        "ipv4_address=10.0.1.2\n"
        "ipv4_prefix=24\n"
        "ipv4_gateway=10.0.1.1\n"
        "ipv4_dns=10.0.1.1\n";
    struct network_device_snapshot device_snapshot;
    struct network_device_selection device_selection;
    struct network_link_state_record link_record;
    struct network_link_selection link_selection;
    struct network_ipv4_config initial_config;
    struct network_ipv4_config replacement_config;
    struct network_config_transaction transaction;
    struct icmp_reachability_policy icmp_policy;
    struct icmp_reachability_engine icmp_engine;
    uint8_t icmp_request[ICMP_REACHABILITY_HEADER_SIZE + 4U];
    uint8_t icmp_reply[sizeof(icmp_request)];
    size_t icmp_reply_length = 0U;
    struct management_command_descriptor command_descriptor;
    struct management_command_registry command_registry;
    struct authorization_entry authorization_entry;
    struct authorization_matrix authorization_matrix;
    struct network_admission_policy role_policy;
    struct network_admission_policy deny_policy;
    struct network_admission_selection role_selection;
    struct network_admission_selection deny_selection;
    struct network_admission_peer admin_peer;
    struct network_admission_peer unauthenticated_peer;
    uint8_t transport_frame[
        MANAGEMENT_TRANSPORT_HEADER_SIZE +
        MANAGEMENT_COMMAND_HEADER_LENGTH];
    struct network_event events[NETWORK_EVENT_TYPE_COUNT];
    struct network_event event;
    struct event_ring *const ring = kernel_event_ring_get();
    uint32_t event_count = 0U;
    uint32_t index;

    network_foundation_qemu_require(
        ring != NULL && event_ring_is_published(ring) &&
        event_ring_count(ring) == 0U,
        "event ring fixture");
    serial_write("[NETWORK-FOUNDATION] proof init\n");

    network_foundation_qemu_zero(
        &device_snapshot, sizeof(device_snapshot));
    device_snapshot.version = NETWORK_DEVICE_SNAPSHOT_VERSION;
    device_snapshot.device_id = NETWORK_FOUNDATION_DEVICE_ID;
    device_snapshot.vendor_id = 0x1234U;
    device_snapshot.product_id = 0x5678U;
    device_snapshot.bus_address = 0U;
    device_snapshot.mac_address[0] = 0x02U;
    device_snapshot.mac_address[1] = 0x00U;
    device_snapshot.mac_address[2] = 0x00U;
    device_snapshot.mac_address[3] = 0x00U;
    device_snapshot.mac_address[4] = 0x00U;
    device_snapshot.mac_address[5] = 0x01U;
    device_snapshot.capability_mask =
        NETWORK_DEVICE_CAPABILITY_RX |
        NETWORK_DEVICE_CAPABILITY_TX |
        NETWORK_DEVICE_CAPABILITY_LINK_STATE;
    network_foundation_qemu_zero(
        &device_selection, sizeof(device_selection));
    network_foundation_qemu_require(
        network_device_selection_init(
            &device_selection, &device_snapshot,
            NETWORK_FOUNDATION_DEVICE_GENERATION) ==
            NETWORK_DEVICE_OK &&
        network_device_selection_published(&device_selection),
        "network device publication");

    network_foundation_qemu_zero(
        &link_record, sizeof(link_record));
    link_record.version = NETWORK_LINK_STATE_VERSION;
    link_record.device_id = NETWORK_FOUNDATION_DEVICE_ID;
    link_record.state = NETWORK_LINK_DOWN;
    link_record.generation = NETWORK_FOUNDATION_LINK_GENERATION;
    network_foundation_qemu_zero(
        &link_selection, sizeof(link_selection));
    network_foundation_qemu_require(
        network_link_selection_init(
            &link_selection, &link_record,
            NETWORK_FOUNDATION_DEVICE_GENERATION) ==
            NETWORK_LINK_OK &&
        network_link_path_status(
            &link_selection, &device_selection.device,
            NETWORK_FOUNDATION_DEVICE_GENERATION,
            NETWORK_FOUNDATION_LINK_GENERATION) ==
            NETWORK_LINK_NOT_UP &&
        !network_link_path_enabled(
            &link_selection, &device_selection.device,
            NETWORK_FOUNDATION_DEVICE_GENERATION,
            NETWORK_FOUNDATION_LINK_GENERATION),
        "down link blocks packet path");

    link_selection.link.state = NETWORK_LINK_UP;
    network_foundation_qemu_require(
        network_link_path_status(
            &link_selection, &device_selection.device,
            NETWORK_FOUNDATION_DEVICE_GENERATION,
            NETWORK_FOUNDATION_LINK_GENERATION) ==
            NETWORK_LINK_OK &&
        network_link_path_enabled(
            &link_selection, &device_selection.device,
            NETWORK_FOUNDATION_DEVICE_GENERATION,
            NETWORK_FOUNDATION_LINK_GENERATION),
        "up link enables packet path");
    network_foundation_qemu_append_event(
        ring, NETWORK_EVENT_TYPE_LINK, NETWORK_EVENT_RESULT_OK,
        NETWORK_LINK_UP);
    serial_write(
        "[NETWORK-FOUNDATION] device/link transition ok\n");

    network_foundation_qemu_require(
        network_ipv4_config_parse(
            initial_config_text,
            sizeof(initial_config_text) - 1U,
            &initial_config) == NETWORK_IPV4_OK &&
        network_ipv4_config_parse(
            replacement_config_text,
            sizeof(replacement_config_text) - 1U,
            &replacement_config) == NETWORK_IPV4_OK,
        "static IPv4 parsing");
    network_foundation_qemu_zero(
        &transaction, sizeof(transaction));
    network_foundation_qemu_require(
        network_config_transaction_init(
            &transaction, &initial_config) ==
            NETWORK_CONFIG_TRANSACTION_OK &&
        network_config_transaction_stage(
            &transaction,
            replacement_config_text,
            sizeof(replacement_config_text) - 1U) ==
            NETWORK_CONFIG_TRANSACTION_OK &&
        network_foundation_qemu_config_matches_active(
            &transaction, 0U, 24U) &&
        network_config_transaction_commit(&transaction) ==
            NETWORK_CONFIG_TRANSACTION_OK &&
        network_foundation_qemu_config_matches_active(
            &transaction, 1U, 24U) &&
        !network_config_transaction_has_pending(&transaction),
        "network configuration transaction");
    network_foundation_qemu_append_event(
        ring, NETWORK_EVENT_TYPE_ADDRESS,
        NETWORK_EVENT_RESULT_OK, 1U);
    serial_write(
        "[NETWORK-FOUNDATION] static config activated\n");

    network_foundation_qemu_zero(
        &icmp_policy, sizeof(icmp_policy));
    icmp_policy.version = ICMP_REACHABILITY_VERSION;
    icmp_policy.rate_budget = 2U;
    icmp_policy.rate_window_ticks = 100U;
    network_foundation_qemu_zero(
        &icmp_engine, sizeof(icmp_engine));
    network_foundation_qemu_require(
        icmp_reachability_engine_init(
            &icmp_engine, &icmp_policy) ==
            ICMP_REACHABILITY_OK &&
        icmp_reachability_published(&icmp_engine),
        "ICMP engine publication");
    network_foundation_qemu_fill_icmp_request(
        icmp_request, sizeof(icmp_request));
    network_foundation_qemu_require(
        icmp_reachability_handle_request(
            &icmp_engine, icmp_request, sizeof(icmp_request),
            true, 1000U, icmp_reply,
            sizeof(icmp_reply), &icmp_reply_length) ==
            ICMP_REACHABILITY_OK &&
        icmp_reply_length == sizeof(icmp_request) &&
        icmp_reply[0] ==
            (uint8_t)ICMP_REACHABILITY_TYPE_ECHO_REPLY &&
        icmp_reachability_window_count(&icmp_engine) == 1U &&
        icmp_reachability_handle_request(
            &icmp_engine, icmp_request, sizeof(icmp_request),
            true, 1001U, icmp_reply,
            sizeof(icmp_reply), &icmp_reply_length) ==
            ICMP_REACHABILITY_OK &&
        icmp_reachability_window_count(&icmp_engine) == 2U &&
        icmp_reachability_handle_request(
            &icmp_engine, icmp_request, sizeof(icmp_request),
            true, 1002U, icmp_reply,
            sizeof(icmp_reply), &icmp_reply_length) ==
            ICMP_REACHABILITY_RATE_LIMITED &&
        icmp_reachability_handle_request(
            &icmp_engine, icmp_request, sizeof(icmp_request),
            false, 1003U, icmp_reply,
            sizeof(icmp_reply), &icmp_reply_length) ==
            ICMP_REACHABILITY_DENIED &&
        icmp_reachability_window_count(&icmp_engine) == 2U &&
        icmp_reachability_handle_request(
            &icmp_engine, icmp_request, sizeof(icmp_request),
            true, 1100U, icmp_reply,
            sizeof(icmp_reply), &icmp_reply_length) ==
            ICMP_REACHABILITY_OK,
        "bounded ICMP reachability");
    serial_write(
        "[NETWORK-FOUNDATION] bounded reachability verified\n");

    network_foundation_qemu_zero(
        &command_descriptor, sizeof(command_descriptor));
    network_foundation_qemu_zero(
        &command_registry, sizeof(command_registry));
    network_foundation_qemu_require(
        appliance_status_command_descriptor(
            &command_descriptor) == APPLIANCE_STATUS_OK &&
        management_command_registry_init(
            &command_registry, &command_descriptor, 1U) ==
            MANAGEMENT_COMMAND_OK,
        "management command registry");
    network_foundation_qemu_zero(
        &authorization_entry, sizeof(authorization_entry));
    network_foundation_qemu_zero(
        &authorization_matrix, sizeof(authorization_matrix));
    network_foundation_qemu_require(
        appliance_status_authorization_entry(
            &authorization_entry) == APPLIANCE_STATUS_OK &&
        authorization_matrix_init(
            &authorization_matrix, &authorization_entry, 1U) ==
            AUTHORIZATION_OK,
        "authorization matrix");

    network_foundation_qemu_zero(
        &role_policy, sizeof(role_policy));
    role_policy.version = NETWORK_ADMISSION_VERSION;
    role_policy.mode = NETWORK_ADMISSION_MODE_ROLE_SESSION;
    role_policy.port = NETWORK_FOUNDATION_PORT;
    role_policy.payload_max = NETWORK_FOUNDATION_PAYLOAD_MAX;
    network_foundation_qemu_zero(
        &deny_policy, sizeof(deny_policy));
    deny_policy.version = NETWORK_ADMISSION_VERSION;
    deny_policy.mode = NETWORK_ADMISSION_MODE_DENY_ALL;
    deny_policy.port = NETWORK_FOUNDATION_PORT;
    deny_policy.payload_max = NETWORK_FOUNDATION_PAYLOAD_MAX;
    network_foundation_qemu_zero(
        &role_selection, sizeof(role_selection));
    network_foundation_qemu_zero(
        &deny_selection, sizeof(deny_selection));
    network_foundation_qemu_require(
        network_admission_selection_init(
            &role_selection, &role_policy) ==
            NETWORK_ADMISSION_OK &&
        network_admission_selection_init(
            &deny_selection, &deny_policy) ==
            NETWORK_ADMISSION_OK,
        "admission policy publication");

    network_foundation_qemu_zero(
        &admin_peer, sizeof(admin_peer));
    admin_peer.version = NETWORK_ADMISSION_VERSION;
    admin_peer.session_id = NETWORK_FOUNDATION_PEER_SESSION_ID;
    admin_peer.principal_id = NETWORK_FOUNDATION_PEER_PRINCIPAL_ID;
    admin_peer.generation = NETWORK_FOUNDATION_PEER_GENERATION;
    admin_peer.authenticated = true;
    admin_peer.role_count = 1U;
    admin_peer.roles[0] = AUTHORIZATION_ROLE_ADMIN;
    admin_peer.capability_count = 1U;
    admin_peer.capabilities[0] =
        AUTHORIZATION_CAPABILITY_STATUS_READ;
    network_foundation_qemu_zero(
        &unauthenticated_peer, sizeof(unauthenticated_peer));
    unauthenticated_peer.version = NETWORK_ADMISSION_VERSION;
    unauthenticated_peer.session_id =
        NETWORK_FOUNDATION_PEER_SESSION_ID;
    unauthenticated_peer.principal_id =
        NETWORK_FOUNDATION_PEER_PRINCIPAL_ID;
    unauthenticated_peer.generation =
        NETWORK_FOUNDATION_PEER_GENERATION;
    unauthenticated_peer.authenticated = false;
    unauthenticated_peer.role_count = 1U;
    unauthenticated_peer.roles[0] = AUTHORIZATION_ROLE_ADMIN;
    unauthenticated_peer.capability_count = 1U;
    unauthenticated_peer.capabilities[0] =
        AUTHORIZATION_CAPABILITY_STATUS_READ;

    network_foundation_qemu_zero(
        transport_frame, sizeof(transport_frame));
    network_foundation_qemu_write_u32(
        transport_frame, MANAGEMENT_TRANSPORT_VERSION);
    network_foundation_qemu_write_u16(
        transport_frame + 4U, NETWORK_FOUNDATION_PORT);
    network_foundation_qemu_write_u16(
        transport_frame + 6U,
        MANAGEMENT_COMMAND_HEADER_LENGTH);
    network_foundation_qemu_write_u32(
        transport_frame + 8U,
        MANAGEMENT_TRANSPORT_DIRECTION_REQUEST);
    network_foundation_qemu_write_u32(
        transport_frame + MANAGEMENT_TRANSPORT_HEADER_SIZE,
        MANAGEMENT_COMMAND_VERSION);
    network_foundation_qemu_write_u32(
        transport_frame + MANAGEMENT_TRANSPORT_HEADER_SIZE + 4U,
        APPLIANCE_STATUS_COMMAND_ID);
    network_foundation_qemu_require(
        network_admission_check_request(
            &deny_selection, &admin_peer,
            &command_registry, &authorization_matrix,
            transport_frame, sizeof(transport_frame)) ==
            NETWORK_ADMISSION_DENIED_BY_DEFAULT &&
        network_admission_check_request(
            &role_selection, &unauthenticated_peer,
            &command_registry, &authorization_matrix,
            transport_frame, sizeof(transport_frame)) ==
            NETWORK_ADMISSION_UNAUTHENTICATED &&
        network_admission_check_request(
            &role_selection, &admin_peer,
            &command_registry, &authorization_matrix,
            transport_frame, sizeof(transport_frame)) ==
            NETWORK_ADMISSION_OK,
        "network admission and framed transport");
    network_foundation_qemu_append_event(
        ring, NETWORK_EVENT_TYPE_ADMISSION,
        NETWORK_EVENT_RESULT_OK, APPLIANCE_STATUS_COMMAND_ID);
    network_foundation_qemu_append_event(
        ring, NETWORK_EVENT_TYPE_TRANSPORT,
        NETWORK_EVENT_RESULT_FAILURE,
        NETWORK_ADMISSION_DENIED_BY_DEFAULT);
    serial_write(
        "[NETWORK-FOUNDATION] remote control denied\n");
    serial_write(
        "[NETWORK-FOUNDATION] authorized framed request\n");

    network_foundation_qemu_require(
        network_event_snapshot(
            ring, events, NETWORK_EVENT_TYPE_COUNT,
            &event_count) == NETWORK_EVENT_OK &&
        event_count == 4U &&
        events[0].type == NETWORK_EVENT_TYPE_LINK &&
        events[1].type == NETWORK_EVENT_TYPE_ADDRESS &&
        events[2].type == NETWORK_EVENT_TYPE_ADMISSION &&
        events[3].type == NETWORK_EVENT_TYPE_TRANSPORT &&
        events[3].result == NETWORK_EVENT_RESULT_FAILURE,
        "network audit snapshot");
    for (index = 0U; index < event_count; ++index) {
        network_foundation_qemu_require(
            network_event_pop(ring, &event) == NETWORK_EVENT_OK &&
            event.version == NETWORK_EVENT_VERSION &&
            event.device_id == NETWORK_FOUNDATION_DEVICE_ID &&
            event.generation == NETWORK_FOUNDATION_LINK_GENERATION,
            "network audit exact-once pop");
    }
    network_foundation_qemu_require(
        event_ring_count(ring) == 0U &&
        network_event_pop(ring, &event) == NETWORK_EVENT_EMPTY,
        "network audit ring drained");
    serial_write(
        "[NETWORK-FOUNDATION] audit evidence verified\n");
    serial_write("[NETWORK-FOUNDATION] proof OK\n");
}

#else
typedef int network_foundation_qemu_translation_unit_guard;
#endif
