#include "installer_first_boot_wizard.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static struct installer_discovery_report fixture_report;
static bool fixture_report_ready;

static void fixture_ipv4(
    struct network_ipv4_config *config,
    uint8_t host)
{
    memset(config, 0, sizeof(*config));
    config->schema_version = NETWORK_IPV4_CONFIG_VERSION;
    config->address.octets[0] = 192U;
    config->address.octets[1] = 168U;
    config->address.octets[2] = 50U;
    config->address.octets[3] = host;
    config->prefix_length = 24U;
    config->gateway.octets[0] = 192U;
    config->gateway.octets[1] = 168U;
    config->gateway.octets[2] = 50U;
    config->gateway.octets[3] = 1U;
    config->dns.octets[0] = 192U;
    config->dns.octets[1] = 168U;
    config->dns.octets[2] = 50U;
    config->dns.octets[3] = 53U;
}

static void fixture_init_report(void)
{
    struct installer_discovery_nic nic;

    if (fixture_report_ready) {
        return;
    }
    CHECK(installer_discovery_init(&fixture_report) ==
              INSTALLER_DISCOVERY_OK,
          "discovery fixture initializes");
    CHECK(installer_discovery_publish_firmware(
              &fixture_report, INSTALLER_FIRMWARE_UEFI) ==
              INSTALLER_DISCOVERY_OK,
          "discovery fixture publishes firmware");
    CHECK(installer_discovery_publish_cpu(
              &fixture_report, 2U) == INSTALLER_DISCOVERY_OK,
          "discovery fixture publishes CPU");
    CHECK(installer_discovery_publish_memory(
              &fixture_report, UINT64_C(0x40000000)) ==
              INSTALLER_DISCOVERY_OK,
          "discovery fixture publishes RAM");
    memset(&nic, 0, sizeof(nic));
    nic.id = 1U;
    nic.present = true;
    nic.vendor_id = 0x1AF4U;
    nic.product_id = 0x1000U;
    nic.bus_address = 0x10U;
    nic.mac_address[0] = 0x52U;
    nic.mac_address[1] = 0x54U;
    nic.mac_address[2] = 0x00U;
    nic.mac_address[3] = 0x12U;
    nic.mac_address[4] = 0x34U;
    nic.mac_address[5] = 0x56U;
    CHECK(installer_discovery_publish_nic(
              &fixture_report, &nic) == INSTALLER_DISCOVERY_OK,
          "discovery fixture publishes NIC");
    CHECK(installer_discovery_finalize(&fixture_report) ==
              INSTALLER_DISCOVERY_OK,
          "discovery fixture finalizes");
    fixture_report_ready = true;
}

static struct installer_first_boot_wizard_evidence
make_evidence(bool first_boot, bool storage, bool recovery)
{
    struct installer_first_boot_wizard_evidence evidence;

    fixture_init_report();
    memset(&evidence, 0, sizeof(evidence));
    evidence.version = INSTALLER_FIRST_BOOT_WIZARD_VERSION;
    evidence.discovery = &fixture_report;
    evidence.first_boot_pending = first_boot;
    evidence.storage_ready = storage;
    evidence.recovery_trust_active = recovery;
    return evidence;
}

static struct appliance_secret_protection_at_rest_record
make_at_rest(void)
{
    struct appliance_secret_protection_at_rest_record record;

    memset(&record, 0, sizeof(record));
    record.version = APPLIANCE_SECRET_PROTECTION_VERSION;
    record.kind = APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL;
    record.generation = 1U;
    memset(record.fingerprint, 0xA5U,
           sizeof(record.fingerprint));
    return record;
}

static struct installer_first_boot_wizard_admin make_admin(void)
{
    struct installer_first_boot_wizard_admin admin;

    memset(&admin, 0, sizeof(admin));
    admin.version = INSTALLER_FIRST_BOOT_WIZARD_VERSION;
    admin.at_rest = NULL;
    return admin;
}

static struct appliance_network_config make_static_network(
    const struct network_ipv4_config *static_config)
{
    struct appliance_network_config network;

    memset(&network, 0, sizeof(network));
    network.version = APPLIANCE_NETWORK_CONFIG_VERSION;
    network.interface_length = 4U;
    network.interface[0] = 'e';
    network.interface[1] = 't';
    network.interface[2] = 'h';
    network.interface[3] = '0';
    network.mode = APPLIANCE_NETWORK_CONFIG_MODE_STATIC;
    network.static_present = true;
    network.static_config = *static_config;
    network.initialized = true;
    return network;
}

static struct appliance_network_config make_dhcp_network(void)
{
    struct appliance_network_config network;

    memset(&network, 0, sizeof(network));
    network.version = APPLIANCE_NETWORK_CONFIG_VERSION;
    network.interface_length = 4U;
    network.interface[0] = 'e';
    network.interface[1] = 't';
    network.interface[2] = 'h';
    network.interface[3] = '0';
    network.mode = APPLIANCE_NETWORK_CONFIG_MODE_DHCP;
    network.initialized = true;
    return network;
}

static struct installer_first_boot_wizard_connectivity
make_connectivity(
    const struct network_ipv4_config *runtime,
    bool configured,
    bool gateway,
    bool dns)
{
    struct installer_first_boot_wizard_connectivity connectivity;

    memset(&connectivity, 0, sizeof(connectivity));
    connectivity.version = INSTALLER_FIRST_BOOT_WIZARD_VERSION;
    connectivity.configured = configured;
    connectivity.gateway_reachable = gateway;
    connectivity.dns_reachable = dns;
    connectivity.runtime = runtime;
    return connectivity;
}

static struct installer_first_boot_wizard_persistence
make_persistence(bool admin, bool hostname, bool network)
{
    struct installer_first_boot_wizard_persistence persistence;

    memset(&persistence, 0, sizeof(persistence));
    persistence.version = INSTALLER_FIRST_BOOT_WIZARD_VERSION;
    persistence.admin_secret_record_committed = admin;
    persistence.hostname_record_committed = hostname;
    persistence.network_records_committed = network;
    return persistence;
}

static enum installer_first_boot_wizard_status
fresh_init(struct installer_first_boot_wizard *wizard)
{
    memset(wizard, 0, sizeof(*wizard));
    return installer_first_boot_wizard_init(wizard);
}

static bool complete_static_flow(
    struct installer_first_boot_wizard *wizard,
    const struct installer_first_boot_wizard_evidence *evidence,
    const struct network_ipv4_config *static_config)
{
    struct appliance_secret_protection_at_rest_record at_rest =
        make_at_rest();
    struct installer_first_boot_wizard_admin admin = make_admin();
    struct appliance_network_config network =
        make_static_network(static_config);
    struct installer_first_boot_wizard_connectivity connectivity =
        make_connectivity(static_config, true, true, true);
    struct installer_first_boot_wizard_persistence persistence =
        make_persistence(true, true, true);

    admin.at_rest = &at_rest;
    return fresh_init(wizard) ==
               INSTALLER_FIRST_BOOT_WIZARD_OK &&
           installer_first_boot_wizard_begin(
               wizard, evidence) == INSTALLER_FIRST_BOOT_WIZARD_OK &&
           installer_first_boot_wizard_submit_admin(
               wizard, &admin) == INSTALLER_FIRST_BOOT_WIZARD_OK &&
           installer_first_boot_wizard_submit_hostname(
               wizard, "hfoc-01", 7U) ==
               INSTALLER_FIRST_BOOT_WIZARD_OK &&
           installer_first_boot_wizard_submit_network(
               wizard, &network) == INSTALLER_FIRST_BOOT_WIZARD_OK &&
           installer_first_boot_wizard_submit_connectivity(
               wizard, &connectivity) ==
               INSTALLER_FIRST_BOOT_WIZARD_OK &&
           installer_first_boot_wizard_submit_persistence(
               wizard, &persistence) ==
               INSTALLER_FIRST_BOOT_WIZARD_OK;
}

static void test_status_and_stage_strings(void)
{
    enum installer_first_boot_wizard_status status;
    enum installer_first_boot_wizard_stage stage;

    for (status = INSTALLER_FIRST_BOOT_WIZARD_OK;
         status <= INSTALLER_FIRST_BOOT_WIZARD_BUFFER_TOO_SMALL;
         ++status) {
        CHECK(installer_first_boot_wizard_status_string(status) != NULL,
              "every first boot wizard status has a string");
    }
    CHECK(installer_first_boot_wizard_status_string(
              (enum installer_first_boot_wizard_status)99U) == NULL,
          "undefined first boot wizard status is rejected");
    for (stage = INSTALLER_FIRST_BOOT_WIZARD_STAGE_NONE;
         stage <= INSTALLER_FIRST_BOOT_WIZARD_STAGE_COMPLETE;
         ++stage) {
        CHECK(installer_first_boot_wizard_stage_string(stage) != NULL,
              "every first boot wizard stage has a string");
    }
    CHECK(installer_first_boot_wizard_stage_string(
              (enum installer_first_boot_wizard_stage)99U) == NULL,
          "undefined first boot wizard stage is rejected");
}

static void test_static_ready_flow(void)
{
    struct installer_first_boot_wizard wizard;
    struct network_ipv4_config runtime;
    struct installer_first_boot_wizard_evidence evidence =
        make_evidence(true, true, false);
    char output[INSTALLER_FIRST_BOOT_WIZARD_OUTPUT_MAX];
    size_t output_length = 0U;
    static const char expected[] =
        "HFOC Appliance is ready\n\n"
        "Management:\n"
        "https://192.168.50.10/\n";

    fixture_ipv4(&runtime, 10U);
    CHECK(complete_static_flow(
              &wizard, &evidence, &runtime),
          "complete static first boot flow succeeds");
    CHECK(installer_first_boot_wizard_stage(&wizard) ==
              INSTALLER_FIRST_BOOT_WIZARD_STAGE_READY,
          "static flow reaches ready stage");
    CHECK(installer_first_boot_wizard_publish_ready(
              &wizard, output, sizeof(output), &output_length) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK,
          "static flow publishes ready output");
    CHECK(output_length == sizeof(expected) - 1U &&
              memcmp(output, expected, output_length) == 0,
          "static ready output is exact");
    CHECK(installer_first_boot_wizard_stage(&wizard) ==
              INSTALLER_FIRST_BOOT_WIZARD_STAGE_COMPLETE,
          "ready publication completes wizard");
    CHECK(installer_first_boot_wizard_publish_ready(
              &wizard, output, sizeof(output), &output_length) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK,
          "ready publication after completion is idempotent");
}

static void test_dhcp_ready_flow(void)
{
    struct installer_first_boot_wizard wizard;
    struct network_ipv4_config lease;
    struct installer_first_boot_wizard_evidence evidence =
        make_evidence(true, true, false);
    struct appliance_secret_protection_at_rest_record at_rest =
        make_at_rest();
    struct installer_first_boot_wizard_admin admin = make_admin();
    struct appliance_network_config network = make_dhcp_network();
    struct installer_first_boot_wizard_connectivity connectivity;
    struct installer_first_boot_wizard_persistence persistence =
        make_persistence(true, true, true);
    char output[INSTALLER_FIRST_BOOT_WIZARD_OUTPUT_MAX];
    size_t output_length = 0U;

    fixture_ipv4(&lease, 20U);
    admin.at_rest = &at_rest;
    connectivity = make_connectivity(&lease, true, true, true);
    CHECK(fresh_init(&wizard) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK,
          "DHCP wizard initializes");
    CHECK(installer_first_boot_wizard_begin(
              &wizard, &evidence) == INSTALLER_FIRST_BOOT_WIZARD_OK,
          "DHCP wizard begins");
    CHECK(installer_first_boot_wizard_submit_admin(
              &wizard, &admin) == INSTALLER_FIRST_BOOT_WIZARD_OK,
          "DHCP wizard accepts admin record");
    CHECK(installer_first_boot_wizard_submit_hostname(
              &wizard, "hfoc-dhcp", 9U) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK,
          "DHCP wizard accepts hostname");
    CHECK(installer_first_boot_wizard_submit_network(
              &wizard, &network) == INSTALLER_FIRST_BOOT_WIZARD_OK,
          "DHCP wizard accepts network");
    CHECK(installer_first_boot_wizard_submit_connectivity(
              &wizard, &connectivity) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK,
          "DHCP wizard accepts connectivity");
    CHECK(installer_first_boot_wizard_submit_persistence(
              &wizard, &persistence) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK,
          "DHCP wizard accepts persistence");
    CHECK(installer_first_boot_wizard_publish_ready(
              &wizard, output, sizeof(output), &output_length) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK &&
              strstr(output, "https://192.168.50.20/") != NULL,
          "DHCP ready output uses bound lease address");
}

static void test_deferred_admin_flow(void)
{
    struct installer_first_boot_wizard wizard;
    struct network_ipv4_config lease;
    struct installer_first_boot_wizard_evidence evidence =
        make_evidence(true, true, false);
    struct appliance_secret_protection_at_rest_record at_rest =
        make_at_rest();
    struct installer_first_boot_wizard_admin admin = make_admin();
    struct appliance_network_config network = make_dhcp_network();
    struct installer_first_boot_wizard_connectivity connectivity;
    struct installer_first_boot_wizard_persistence deferred =
        make_persistence(false, true, true);
    struct installer_first_boot_wizard_persistence claimed =
        make_persistence(true, true, true);
    char output[INSTALLER_FIRST_BOOT_WIZARD_OUTPUT_MAX];
    size_t output_length = 0U;

    fixture_ipv4(&lease, 30U);
    admin.at_rest = &at_rest;
    connectivity = make_connectivity(&lease, true, true, true);
    CHECK(fresh_init(&wizard) == INSTALLER_FIRST_BOOT_WIZARD_OK,
          "deferred wizard initializes");
    CHECK(installer_first_boot_wizard_begin(
              &wizard, &evidence) == INSTALLER_FIRST_BOOT_WIZARD_OK,
          "deferred wizard begins");
    CHECK(installer_first_boot_wizard_defer_admin(&wizard) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK,
          "deferred wizard accepts a deferred administrator");
    CHECK(installer_first_boot_wizard_submit_admin(&wizard, &admin) ==
              INSTALLER_FIRST_BOOT_WIZARD_BAD_STATE,
          "an administrator cannot be submitted after deferral");
    CHECK(installer_first_boot_wizard_defer_admin(&wizard) ==
              INSTALLER_FIRST_BOOT_WIZARD_BAD_STATE,
          "deferral is not repeatable");
    CHECK(installer_first_boot_wizard_submit_hostname(
              &wizard, "hfoc-deferred", 13U) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK,
          "deferred wizard accepts hostname");
    CHECK(installer_first_boot_wizard_submit_network(
              &wizard, &network) == INSTALLER_FIRST_BOOT_WIZARD_OK,
          "deferred wizard accepts network");
    CHECK(installer_first_boot_wizard_submit_connectivity(
              &wizard, &connectivity) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK,
          "deferred wizard accepts connectivity");
    CHECK(installer_first_boot_wizard_submit_persistence(
              &wizard, &claimed) ==
              INSTALLER_FIRST_BOOT_WIZARD_BAD_PERSISTENCE,
          "a deferred flow cannot claim a committed administrator record");
    CHECK(installer_first_boot_wizard_submit_persistence(
              &wizard, &deferred) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK,
          "a deferred flow accepts the remaining records");
    CHECK(installer_first_boot_wizard_publish_ready(
              &wizard, output, sizeof(output), &output_length) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK &&
              strstr(output, "https://192.168.50.30/") != NULL,
          "deferred flow publishes ready output");
}

static void test_submitted_admin_rejects_deferral(void)
{
    struct installer_first_boot_wizard wizard;
    struct installer_first_boot_wizard_evidence evidence =
        make_evidence(true, true, false);
    struct appliance_secret_protection_at_rest_record at_rest =
        make_at_rest();
    struct installer_first_boot_wizard_admin admin = make_admin();

    admin.at_rest = &at_rest;
    CHECK(fresh_init(&wizard) == INSTALLER_FIRST_BOOT_WIZARD_OK,
          "submitted-admin wizard initializes");
    CHECK(installer_first_boot_wizard_begin(
              &wizard, &evidence) == INSTALLER_FIRST_BOOT_WIZARD_OK,
          "submitted-admin wizard begins");
    CHECK(installer_first_boot_wizard_submit_admin(&wizard, &admin) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK,
          "submitted-admin wizard accepts the record");
    CHECK(installer_first_boot_wizard_defer_admin(&wizard) ==
              INSTALLER_FIRST_BOOT_WIZARD_BAD_STATE,
          "a submitted administrator cannot be deferred");
}

static void test_begin_fail_closed_paths(void)
{
    struct installer_first_boot_wizard wizard;
    struct installer_first_boot_wizard_evidence evidence =
        make_evidence(true, true, false);
    struct installer_first_boot_wizard_evidence not_first =
        make_evidence(false, true, false);
    struct installer_first_boot_wizard_evidence no_storage =
        make_evidence(true, false, false);
    struct installer_first_boot_wizard_evidence recovery =
        make_evidence(true, true, true);

    CHECK(fresh_init(&wizard) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK,
          "wizard init succeeds");
    CHECK(installer_first_boot_wizard_begin(
              &wizard, &evidence) == INSTALLER_FIRST_BOOT_WIZARD_OK,
          "wizard begin succeeds with complete evidence");
    CHECK(installer_first_boot_wizard_begin(
              &wizard, &evidence) ==
              INSTALLER_FIRST_BOOT_WIZARD_BAD_STATE,
          "wizard begin rejects repeated boot stage");

    CHECK(fresh_init(&wizard) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_begin(
              &wizard, &not_first) ==
              INSTALLER_FIRST_BOOT_WIZARD_NOT_FIRST_BOOT,
          "wizard begin rejects no pending first boot");
    CHECK(fresh_init(&wizard) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_begin(
              &wizard, &no_storage) ==
              INSTALLER_FIRST_BOOT_WIZARD_STORAGE_UNAVAILABLE,
          "wizard begin rejects unavailable storage");
    CHECK(fresh_init(&wizard) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_begin(
              &wizard, &recovery) ==
              INSTALLER_FIRST_BOOT_WIZARD_RECOVERY_TRUST_ACTIVE,
          "wizard begin rejects active recovery trust");
}

static void test_admin_record_fail_closed(void)
{
    struct installer_first_boot_wizard wizard;
    struct installer_first_boot_wizard_evidence evidence =
        make_evidence(true, true, false);
    struct appliance_secret_protection_at_rest_record at_rest =
        make_at_rest();
    struct installer_first_boot_wizard_admin admin = make_admin();

    admin.at_rest = &at_rest;
    at_rest.kind = APPLIANCE_SECRET_PROTECTION_KIND_BEARER_TOKEN;
    CHECK(fresh_init(&wizard) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_begin(
              &wizard, &evidence) == INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_submit_admin(
              &wizard, &admin) ==
              INSTALLER_FIRST_BOOT_WIZARD_BAD_ADMIN_RECORD,
          "wizard rejects non-admin at-rest record");

    at_rest = make_at_rest();
    at_rest.version = APPLIANCE_SECRET_PROTECTION_VERSION + 1U;
    CHECK(fresh_init(&wizard) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_begin(
              &wizard, &evidence) == INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_submit_admin(
              &wizard, &admin) ==
              INSTALLER_FIRST_BOOT_WIZARD_BAD_ADMIN_RECORD,
          "wizard rejects wrong admin record version");

    at_rest = make_at_rest();
    memset(at_rest.fingerprint, 0U,
           sizeof(at_rest.fingerprint));
    CHECK(fresh_init(&wizard) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_begin(
              &wizard, &evidence) == INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_submit_admin(
              &wizard, &admin) ==
              INSTALLER_FIRST_BOOT_WIZARD_BAD_ADMIN_RECORD,
          "wizard rejects all-zero admin fingerprint");

    at_rest = make_at_rest();
    at_rest.reserved[0] = 1U;
    CHECK(fresh_init(&wizard) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_begin(
              &wizard, &evidence) == INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_submit_admin(
              &wizard, &admin) ==
              INSTALLER_FIRST_BOOT_WIZARD_BAD_ADMIN_RECORD,
          "wizard rejects nonzero reserved admin record bytes");
}

static void test_network_and_connectivity_fail_closed(void)
{
    struct installer_first_boot_wizard wizard;
    struct network_ipv4_config runtime;
    struct network_ipv4_config wrong;
    struct installer_first_boot_wizard_evidence evidence =
        make_evidence(true, true, false);
    struct appliance_secret_protection_at_rest_record at_rest =
        make_at_rest();
    struct installer_first_boot_wizard_admin admin = make_admin();
    struct appliance_network_config network;
    struct installer_first_boot_wizard_connectivity connectivity;

    fixture_ipv4(&runtime, 10U);
    network = make_static_network(&runtime);
    admin.at_rest = &at_rest;

    network.mode = APPLIANCE_NETWORK_CONFIG_MODE_NONE;
    CHECK(fresh_init(&wizard) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_begin(
              &wizard, &evidence) == INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_submit_admin(
              &wizard, &admin) == INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_submit_hostname(
              &wizard, "hfoc-01", 7U) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_submit_network(
              &wizard, &network) ==
              INSTALLER_FIRST_BOOT_WIZARD_BAD_NETWORK,
          "wizard rejects invalid network mode");

    network = make_static_network(&runtime);
    connectivity = make_connectivity(
        &runtime, true, false, true);
    CHECK(fresh_init(&wizard) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_begin(
              &wizard, &evidence) == INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_submit_admin(
              &wizard, &admin) == INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_submit_hostname(
              &wizard, "hfoc-01", 7U) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_submit_network(
              &wizard, &network) == INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_submit_connectivity(
              &wizard, &connectivity) ==
              INSTALLER_FIRST_BOOT_WIZARD_BAD_CONNECTIVITY,
          "wizard rejects unreachable gateway");

    wrong = runtime;
    wrong.address.octets[3] = 99U;
    connectivity = make_connectivity(
        &wrong, true, true, true);
    CHECK(fresh_init(&wizard) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_begin(
              &wizard, &evidence) == INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_submit_admin(
              &wizard, &admin) == INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_submit_hostname(
              &wizard, "hfoc-01", 7U) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_submit_network(
              &wizard, &network) == INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_submit_connectivity(
              &wizard, &connectivity) ==
              INSTALLER_FIRST_BOOT_WIZARD_BAD_RUNTIME_CONFIG,
          "wizard rejects static runtime mismatch");
}

static void test_persistence_and_order_fail_closed(void)
{
    struct installer_first_boot_wizard wizard;
    struct network_ipv4_config runtime;
    struct installer_first_boot_wizard_evidence evidence =
        make_evidence(true, true, false);
    struct installer_first_boot_wizard_persistence persistence =
        make_persistence(true, false, true);
    struct appliance_secret_protection_at_rest_record at_rest =
        make_at_rest();
    struct installer_first_boot_wizard_admin admin = make_admin();

    fixture_ipv4(&runtime, 10U);
    admin.at_rest = &at_rest;
    CHECK(complete_static_flow(
              &wizard, &evidence, &runtime),
          "complete static flow fixture is reusable");
    CHECK(installer_first_boot_wizard_submit_persistence(
              &wizard, &persistence) ==
              INSTALLER_FIRST_BOOT_WIZARD_BAD_STATE,
          "wizard rejects persistence before connectivity stage");

    persistence = make_persistence(true, false, true);
    CHECK(fresh_init(&wizard) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_begin(
              &wizard, &evidence) == INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_submit_admin(
              &wizard, &admin) == INSTALLER_FIRST_BOOT_WIZARD_OK &&
          installer_first_boot_wizard_submit_hostname(
              &wizard, "hfoc-01", 7U) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK,
          "wizard reaches hostname stage");
    CHECK(installer_first_boot_wizard_submit_hostname(
              &wizard, "hfoc-01", 7U) ==
              INSTALLER_FIRST_BOOT_WIZARD_BAD_STATE,
          "wizard rejects repeated hostname submission");
}

static void test_ready_buffer_and_secret_absence(void)
{
    struct installer_first_boot_wizard wizard;
    struct network_ipv4_config runtime;
    struct installer_first_boot_wizard_evidence evidence =
        make_evidence(true, true, false);
    char small[16U];
    char output[INSTALLER_FIRST_BOOT_WIZARD_OUTPUT_MAX];
    size_t output_length = 0U;
    size_t index;
    bool no_secret_byte = true;

    fixture_ipv4(&runtime, 10U);
    CHECK(complete_static_flow(
              &wizard, &evidence, &runtime),
          "buffer fixture reaches ready");
    CHECK(installer_first_boot_wizard_publish_ready(
              &wizard, small, sizeof(small), &output_length) ==
              INSTALLER_FIRST_BOOT_WIZARD_BUFFER_TOO_SMALL,
          "ready publication rejects small buffer");
    CHECK(installer_first_boot_wizard_stage(&wizard) ==
              INSTALLER_FIRST_BOOT_WIZARD_STAGE_READY,
          "failed ready publication does not complete wizard");
    CHECK(installer_first_boot_wizard_publish_ready(
              &wizard, output, sizeof(output), &output_length) ==
              INSTALLER_FIRST_BOOT_WIZARD_OK,
          "ready publication succeeds with sufficient buffer");
    for (index = 0U; index < output_length; ++index) {
        if ((unsigned char)output[index] == 0xA5U) {
            no_secret_byte = false;
        }
    }
    CHECK(no_secret_byte,
          "ready output does not contain credential fingerprint byte");
}

int main(void)
{
    test_status_and_stage_strings();
    test_static_ready_flow();
    test_dhcp_ready_flow();
    test_deferred_admin_flow();
    test_submitted_admin_rejects_deferral();
    test_begin_fail_closed_paths();
    test_admin_record_fail_closed();
    test_network_and_connectivity_fail_closed();
    test_persistence_and_order_fail_closed();
    test_ready_buffer_and_secret_absence();
    if (failures != 0) {
        fprintf(stderr,
                "Installer first boot wizard host test: %d failures\n",
                failures);
        return 1;
    }
    printf("Installer first boot wizard host test: OK\n");
    return 0;
}
