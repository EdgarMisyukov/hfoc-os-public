#include "appliance_production_first_boot.h"

#include "appliance_config_validation.h"
#include "appliance_persistent_config.h"
#include "appliance_persistent_network_config.h"
#include "appliance_persistent_secret_config.h"
#include "appliance_network_config.h"
#include "appliance_secret_protection.h"
#include "appliance_state_loader.h"
#include "installer_first_boot_wizard.h"
#include "installer_hardware_discovery.h"
#include "ipv4_config.h"
#include "network_device.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void appliance_production_first_boot_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void appliance_production_first_boot_copy_bytes(
    void *destination,
    const void *source,
    size_t length)
{
    unsigned char *output = destination;
    const unsigned char *input = source;
    size_t index;

    for (index = 0U; index < length; ++index) {
        output[index] = input[index];
    }
}

static bool appliance_production_first_boot_device_valid(
    const struct network_device_snapshot *device)
{
    return network_device_snapshot_validate(device) == NETWORK_DEVICE_OK;
}

static bool appliance_production_first_boot_build_discovery(
    const struct network_device_snapshot *device,
    struct installer_discovery_report *report)
{
    struct installer_discovery_nic nic;

    if (installer_discovery_init(report) != INSTALLER_DISCOVERY_OK ||
        installer_discovery_publish_firmware(
            report, INSTALLER_FIRMWARE_BIOS_LEGACY) !=
            INSTALLER_DISCOVERY_OK ||
        installer_discovery_publish_cpu(report, 1U) !=
            INSTALLER_DISCOVERY_OK ||
        installer_discovery_publish_memory(
            report, UINT64_C(536870912)) != INSTALLER_DISCOVERY_OK) {
        return false;
    }
    appliance_production_first_boot_zero_bytes(&nic, sizeof(nic));
    nic.present = true;
    nic.id = 1U;
    nic.vendor_id = device->vendor_id;
    nic.product_id = device->product_id;
    nic.bus_address = device->bus_address;
    appliance_production_first_boot_copy_bytes(
        nic.mac_address, device->mac_address, NETWORK_DEVICE_MAC_LENGTH);
    if (installer_discovery_publish_nic(report, &nic) !=
            INSTALLER_DISCOVERY_OK ||
        installer_discovery_finalize(report) != INSTALLER_DISCOVERY_OK) {
        return false;
    }
    return true;
}

static enum appliance_production_first_boot_status
appliance_production_first_boot_commit_network(
    struct appliance_state_loader *loader)
{
    const struct appliance_network_config *desired =
        appliance_persistent_network_config_current(
            &loader->network_config);
    enum appliance_persistent_network_config_status status;

    if (desired == NULL) {
        return APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_PERSISTENCE;
    }
    if (desired->mode == APPLIANCE_NETWORK_CONFIG_MODE_DHCP) {
        status = appliance_persistent_network_config_set_dhcp(
            &loader->network_config, desired->interface,
            desired->interface_length);
    } else if (desired->mode == APPLIANCE_NETWORK_CONFIG_MODE_STATIC &&
               desired->static_present) {
        status = appliance_persistent_network_config_set_static(
            &loader->network_config, desired->interface,
            desired->interface_length, &desired->static_config);
    } else {
        return APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_PERSISTENCE;
    }
    return status == APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK
        ? APPLIANCE_PRODUCTION_FIRST_BOOT_OK
        : APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_PERSISTENCE;
}

static enum appliance_production_first_boot_status
appliance_production_first_boot_provision(
    struct appliance_production_first_boot *first_boot,
    struct appliance_state_loader *loader,
    const struct network_device_snapshot *device,
    const char *hostname,
    size_t hostname_length,
    const struct network_ipv4_config *runtime,
    bool gateway_reachable,
    bool dns_reachable,
    const uint8_t credential[
        APPLIANCE_PRODUCTION_FIRST_BOOT_CREDENTIAL_LENGTH])
{
    struct installer_first_boot_wizard_evidence evidence;
    struct installer_first_boot_wizard_admin admin;
    struct installer_first_boot_wizard_connectivity connectivity;
    struct installer_first_boot_wizard_persistence persistence;
    struct installer_discovery_report report;
    struct appliance_secret_protection protection;
    struct appliance_secret_protection_at_rest_record at_rest;
    struct appliance_secret_material material;
    const struct appliance_network_config *desired;
    enum appliance_persistent_config_status hostname_status;
    enum appliance_persistent_secret_config_status secret_status;
    enum appliance_production_first_boot_status network_status;

    if (!appliance_production_first_boot_build_discovery(device, &report)) {
        first_boot->last_status =
            APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_DEVICE;
        return first_boot->last_status;
    }

    appliance_production_first_boot_zero_bytes(
        &first_boot->wizard, sizeof(first_boot->wizard));
    if (installer_first_boot_wizard_init(&first_boot->wizard) !=
        INSTALLER_FIRST_BOOT_WIZARD_OK) {
        first_boot->last_status =
            APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_WIZARD;
        return first_boot->last_status;
    }

    appliance_production_first_boot_zero_bytes(&evidence, sizeof(evidence));
    evidence.version = INSTALLER_FIRST_BOOT_WIZARD_VERSION;
    evidence.discovery = &report;
    evidence.first_boot_pending = true;
    evidence.storage_ready = true;
    evidence.recovery_trust_active = false;
    if (installer_first_boot_wizard_begin(
            &first_boot->wizard, &evidence) !=
        INSTALLER_FIRST_BOOT_WIZARD_OK) {
        first_boot->last_status =
            APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_WIZARD;
        return first_boot->last_status;
    }

    if (credential != NULL) {
        appliance_production_first_boot_zero_bytes(
            &protection, sizeof(protection));
        appliance_production_first_boot_zero_bytes(
            &at_rest, sizeof(at_rest));
        appliance_production_first_boot_zero_bytes(
            &material, sizeof(material));
        material.version = APPLIANCE_SECRET_PROTECTION_VERSION;
        material.kind = APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL;
        material.bytes = credential;
        material.length = APPLIANCE_PRODUCTION_FIRST_BOOT_CREDENTIAL_LENGTH;
        if (appliance_secret_protection_init(&protection) !=
                APPLIANCE_SECRET_PROTECTION_OK ||
            appliance_secret_protection_protect(&protection, &material) !=
                APPLIANCE_SECRET_PROTECTION_OK ||
            appliance_secret_protection_export_at_rest(
                &protection, &at_rest) != APPLIANCE_SECRET_PROTECTION_OK) {
            first_boot->last_status =
                APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_CREDENTIAL;
            return first_boot->last_status;
        }
        appliance_production_first_boot_zero_bytes(&admin, sizeof(admin));
        admin.version = INSTALLER_FIRST_BOOT_WIZARD_VERSION;
        admin.at_rest = &at_rest;
        if (installer_first_boot_wizard_submit_admin(
                &first_boot->wizard, &admin) !=
            INSTALLER_FIRST_BOOT_WIZARD_OK) {
            first_boot->last_status =
                APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_WIZARD;
            return first_boot->last_status;
        }
    } else if (installer_first_boot_wizard_defer_admin(
                   &first_boot->wizard) !=
               INSTALLER_FIRST_BOOT_WIZARD_OK) {
        /*
         * ITEM-403: ordinary first boot has no operator secret yet, so the
         * administrator stage is deferred and no credential record is
         * committed.  The console enrollment owner provisions it later.
         */
        first_boot->last_status =
            APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_WIZARD;
        return first_boot->last_status;
    }

    if (installer_first_boot_wizard_submit_hostname(
            &first_boot->wizard, hostname, hostname_length) !=
        INSTALLER_FIRST_BOOT_WIZARD_OK) {
        first_boot->last_status =
            APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_WIZARD;
        return first_boot->last_status;
    }

    desired = appliance_persistent_network_config_current(
        &loader->network_config);
    if (desired == NULL ||
        installer_first_boot_wizard_submit_network(
            &first_boot->wizard, desired) !=
            INSTALLER_FIRST_BOOT_WIZARD_OK) {
        first_boot->last_status =
            APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_WIZARD;
        return first_boot->last_status;
    }

    appliance_production_first_boot_zero_bytes(
        &connectivity, sizeof(connectivity));
    connectivity.version = INSTALLER_FIRST_BOOT_WIZARD_VERSION;
    connectivity.configured = true;
    connectivity.gateway_reachable = gateway_reachable;
    connectivity.dns_reachable = dns_reachable;
    connectivity.runtime = runtime;
    if (installer_first_boot_wizard_submit_connectivity(
            &first_boot->wizard, &connectivity) !=
        INSTALLER_FIRST_BOOT_WIZARD_OK) {
        first_boot->last_status =
            APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_WIZARD;
        return first_boot->last_status;
    }

    if (credential != NULL) {
        secret_status = appliance_persistent_secret_config_commit_admin(
            &loader->secret_config, at_rest.fingerprint);
        if (secret_status != APPLIANCE_PERSISTENT_SECRET_CONFIG_OK) {
            first_boot->last_status =
                APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_PERSISTENCE;
            first_boot->state = APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_FAILED;
            return first_boot->last_status;
        }
        appliance_production_first_boot_copy_bytes(
            first_boot->admin_fingerprint, at_rest.fingerprint,
            SHA256_DIGEST_SIZE);
        first_boot->admin_provisioned = true;
    } else {
        secret_status = APPLIANCE_PERSISTENT_SECRET_CONFIG_OK;
        first_boot->admin_provisioned = false;
    }
    hostname_status = appliance_persistent_config_set_hostname(
        &loader->system_config, hostname, hostname_length);
    network_status =
        appliance_production_first_boot_commit_network(loader);
    if (hostname_status != APPLIANCE_PERSISTENT_CONFIG_OK ||
        network_status != APPLIANCE_PRODUCTION_FIRST_BOOT_OK) {
        first_boot->last_status =
            APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_PERSISTENCE;
        first_boot->state = APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_FAILED;
        return first_boot->last_status;
    }

    appliance_production_first_boot_zero_bytes(
        &persistence, sizeof(persistence));
    persistence.version = INSTALLER_FIRST_BOOT_WIZARD_VERSION;
    persistence.admin_secret_record_committed = credential != NULL;
    persistence.hostname_record_committed = true;
    persistence.network_records_committed = true;
    if (installer_first_boot_wizard_submit_persistence(
            &first_boot->wizard, &persistence) !=
        INSTALLER_FIRST_BOOT_WIZARD_OK) {
        first_boot->last_status =
            APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_WIZARD;
        first_boot->state = APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_FAILED;
        return first_boot->last_status;
    }
    if (installer_first_boot_wizard_publish_ready(
            &first_boot->wizard, first_boot->ready_output,
            sizeof(first_boot->ready_output),
            &first_boot->ready_output_length) !=
        INSTALLER_FIRST_BOOT_WIZARD_OK) {
        first_boot->last_status =
            APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_WIZARD;
        first_boot->state = APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_FAILED;
        return first_boot->last_status;
    }

    first_boot->wizard_complete = true;
    first_boot->ready_published = true;
    first_boot->network = *desired;
    first_boot->runtime = *runtime;
    first_boot->first_boot_pending = false;
    first_boot->state = APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_PROVISIONED;
    first_boot->last_status = APPLIANCE_PRODUCTION_FIRST_BOOT_OK;
    return APPLIANCE_PRODUCTION_FIRST_BOOT_OK;
}

enum appliance_production_first_boot_status
appliance_production_first_boot_init(
    struct appliance_production_first_boot *first_boot,
    struct appliance_state_loader *loader,
    const struct network_device_snapshot *device)
{
    const struct appliance_persistent_config *system_config;

    if (first_boot == NULL || loader == NULL || device == NULL) {
        return APPLIANCE_PRODUCTION_FIRST_BOOT_NULL;
    }
    if (first_boot->initialized) {
        return APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_STATE;
    }
    if (!appliance_production_first_boot_device_valid(device)) {
        return APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_DEVICE;
    }
    system_config = appliance_state_loader_system_config(loader);
    if (system_config == NULL ||
        !appliance_state_loader_ready(loader)) {
        return APPLIANCE_PRODUCTION_FIRST_BOOT_STORAGE_UNAVAILABLE;
    }

    appliance_production_first_boot_zero_bytes(first_boot, sizeof(*first_boot));
    first_boot->version = APPLIANCE_PRODUCTION_FIRST_BOOT_VERSION;
    first_boot->initialized = true;
    first_boot->storage_ready = true;
    first_boot->loader = loader;
    first_boot->device = *device;
    first_boot->first_boot_pending =
        appliance_persistent_config_source(system_config) !=
        APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED;
    first_boot->state = first_boot->first_boot_pending
        ? APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_AWAITING_NETWORK
        : APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_RELOADED;
    first_boot->last_status = APPLIANCE_PRODUCTION_FIRST_BOOT_OK;
    return APPLIANCE_PRODUCTION_FIRST_BOOT_OK;
}

static enum appliance_production_first_boot_status
appliance_production_first_boot_reload(
    struct appliance_production_first_boot *first_boot,
    struct appliance_state_loader *loader)
{
    char hostname[APPLIANCE_PRODUCTION_FIRST_BOOT_HOSTNAME_MAX + 1U];
    size_t hostname_length = 0U;
    const struct appliance_network_config *network;

    if (appliance_persistent_config_hostname(
            &loader->system_config, hostname, sizeof(hostname),
            &hostname_length) != APPLIANCE_PERSISTENT_CONFIG_OK ||
        hostname_length > APPLIANCE_PRODUCTION_FIRST_BOOT_HOSTNAME_MAX) {
        first_boot->state = APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_FAILED;
        first_boot->last_status =
            APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_PERSISTENCE;
        return first_boot->last_status;
    }
    network = appliance_persistent_network_config_current(
        &loader->network_config);
    if (network == NULL) {
        first_boot->state = APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_FAILED;
        first_boot->last_status =
            APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_PERSISTENCE;
        return first_boot->last_status;
    }
    appliance_production_first_boot_copy_bytes(
        first_boot->hostname, hostname, hostname_length);
    first_boot->hostname[hostname_length] = '\0';
    first_boot->hostname_length = (uint8_t)hostname_length;
    first_boot->network = *network;
    first_boot->wizard_complete = true;
    first_boot->network_ready = true;
    first_boot->state = APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_RELOADED;
    first_boot->last_status = APPLIANCE_PRODUCTION_FIRST_BOOT_OK;
    return APPLIANCE_PRODUCTION_FIRST_BOOT_OK;
}

static enum appliance_production_first_boot_status
appliance_production_first_boot_dispatch(
    struct appliance_production_first_boot *first_boot,
    const char *hostname,
    size_t hostname_length,
    const struct network_ipv4_config *runtime,
    bool gateway_reachable,
    bool dns_reachable,
    const uint8_t credential[
        APPLIANCE_PRODUCTION_FIRST_BOOT_CREDENTIAL_LENGTH])
{
    if (first_boot == NULL || hostname == NULL) {
        return APPLIANCE_PRODUCTION_FIRST_BOOT_NULL;
    }
    if (!first_boot->initialized || first_boot->loader == NULL) {
        return APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_STATE;
    }
    if (first_boot->state == APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_PROVISIONED ||
        first_boot->state == APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_RELOADED) {
        return APPLIANCE_PRODUCTION_FIRST_BOOT_OK;
    }
    if (hostname_length == 0U ||
        hostname_length > APPLIANCE_PRODUCTION_FIRST_BOOT_HOSTNAME_MAX ||
        appliance_config_validation_validate_hostname(
            hostname, hostname_length) !=
            APPLIANCE_CONFIG_VALIDATION_OK) {
        first_boot->state = APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_FAILED;
        first_boot->last_status =
            APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_WIZARD;
        return first_boot->last_status;
    }
    if (!first_boot->first_boot_pending) {
        return appliance_production_first_boot_reload(
            first_boot, first_boot->loader);
    }
    if (!first_boot->storage_ready) {
        first_boot->state =
            APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_AWAITING_STORAGE;
        first_boot->last_status =
            APPLIANCE_PRODUCTION_FIRST_BOOT_STORAGE_UNAVAILABLE;
        return first_boot->last_status;
    }
    if (runtime == NULL) {
        first_boot->state =
            APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_AWAITING_NETWORK;
        first_boot->last_status =
            APPLIANCE_PRODUCTION_FIRST_BOOT_NETWORK_UNAVAILABLE;
        return first_boot->last_status;
    }
    if (network_ipv4_config_validate(runtime) != NETWORK_IPV4_OK) {
        first_boot->state =
            APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_AWAITING_NETWORK;
        first_boot->last_status =
            APPLIANCE_PRODUCTION_FIRST_BOOT_NETWORK_UNAVAILABLE;
        return first_boot->last_status;
    }
    appliance_production_first_boot_copy_bytes(
        first_boot->hostname, hostname, hostname_length);
    first_boot->hostname[hostname_length] = '\0';
    first_boot->hostname_length = (uint8_t)hostname_length;
    {
        const enum appliance_production_first_boot_status provision_status =
            appliance_production_first_boot_provision(
                first_boot, first_boot->loader, &first_boot->device, hostname,
                hostname_length, runtime, gateway_reachable, dns_reachable,
                credential);

        if (provision_status != APPLIANCE_PRODUCTION_FIRST_BOOT_OK) {
            first_boot->state =
                APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_FAILED;
        }
        return provision_status;
    }
}

enum appliance_production_first_boot_status
appliance_production_first_boot_run(
    struct appliance_production_first_boot *first_boot,
    const char *hostname,
    size_t hostname_length,
    const struct network_ipv4_config *runtime,
    bool gateway_reachable,
    bool dns_reachable,
    const uint8_t credential[
        APPLIANCE_PRODUCTION_FIRST_BOOT_CREDENTIAL_LENGTH])
{
    if (credential == NULL) {
        return APPLIANCE_PRODUCTION_FIRST_BOOT_NULL;
    }
    return appliance_production_first_boot_dispatch(
        first_boot, hostname, hostname_length, runtime, gateway_reachable,
        dns_reachable, credential);
}

enum appliance_production_first_boot_status
appliance_production_first_boot_run_deferred(
    struct appliance_production_first_boot *first_boot,
    const char *hostname,
    size_t hostname_length,
    const struct network_ipv4_config *runtime,
    bool gateway_reachable,
    bool dns_reachable)
{
    return appliance_production_first_boot_dispatch(
        first_boot, hostname, hostname_length, runtime, gateway_reachable,
        dns_reachable, NULL);
}

bool appliance_production_first_boot_pending(
    const struct appliance_production_first_boot *first_boot)
{
    return first_boot != NULL && first_boot->initialized &&
           first_boot->first_boot_pending;
}

bool appliance_production_first_boot_provisioned(
    const struct appliance_production_first_boot *first_boot)
{
    return first_boot != NULL && first_boot->initialized &&
           first_boot->state ==
               APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_PROVISIONED;
}

bool appliance_production_first_boot_admin_provisioned(
    const struct appliance_production_first_boot *first_boot)
{
    return first_boot != NULL && first_boot->initialized &&
           first_boot->admin_provisioned;
}

const struct network_ipv4_config *
appliance_production_first_boot_runtime(
    const struct appliance_production_first_boot *first_boot)
{
    if (first_boot == NULL || !first_boot->initialized ||
        first_boot->state !=
            APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_PROVISIONED) {
        return NULL;
    }
    return &first_boot->runtime;
}

static bool appliance_production_first_boot_append(
    char *output,
    size_t capacity,
    size_t *position,
    const char *text)
{
    size_t index = 0U;

    while (text[index] != '\0') {
        if (*position + 1U >= capacity) {
            return false;
        }
        output[*position] = text[index];
        ++(*position);
        ++index;
    }
    output[*position] = '\0';
    return true;
}

enum appliance_production_first_boot_status
appliance_production_first_boot_render(
    const struct appliance_production_first_boot *first_boot,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    size_t position = 0U;
    const char *state;

    if (first_boot == NULL || output == NULL || output_length == NULL) {
        return APPLIANCE_PRODUCTION_FIRST_BOOT_NULL;
    }
    if (!first_boot->initialized ||
        first_boot->version != APPLIANCE_PRODUCTION_FIRST_BOOT_VERSION) {
        return APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_STATE;
    }
    state = appliance_production_first_boot_state_string(first_boot->state);
    if (state == NULL) {
        return APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_STATE;
    }
    if (!appliance_production_first_boot_append(
            output, capacity, &position, "first_boot v=1 pending=") ||
        !appliance_production_first_boot_append(
            output, capacity, &position,
            first_boot->first_boot_pending ? "yes" : "no") ||
        !appliance_production_first_boot_append(
            output, capacity, &position, " state=") ||
        !appliance_production_first_boot_append(
            output, capacity, &position, state) ||
        !appliance_production_first_boot_append(
            output, capacity, &position, " hostname=") ||
        !appliance_production_first_boot_append(
            output, capacity, &position, first_boot->hostname) ||
        !appliance_production_first_boot_append(
            output, capacity, &position, " wizard=") ||
        !appliance_production_first_boot_append(
            output, capacity, &position,
            first_boot->wizard_complete ? "complete" : "pending")) {
        return APPLIANCE_PRODUCTION_FIRST_BOOT_BUFFER_TOO_SMALL;
    }
    *output_length = position;
    return APPLIANCE_PRODUCTION_FIRST_BOOT_OK;
}

const char *appliance_production_first_boot_state_string(
    enum appliance_production_first_boot_state state)
{
    switch (state) {
    case APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_NONE:
        return "NONE";
    case APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_AWAITING_STORAGE:
        return "AWAITING_STORAGE";
    case APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_AWAITING_NETWORK:
        return "AWAITING_NETWORK";
    case APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_PROVISIONED:
        return "PROVISIONED";
    case APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_RELOADED:
        return "RELOADED";
    case APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_FAILED:
        return "FAILED";
    default:
        return NULL;
    }
}

const char *appliance_production_first_boot_status_string(
    enum appliance_production_first_boot_status status)
{
    switch (status) {
    case APPLIANCE_PRODUCTION_FIRST_BOOT_OK:
        return "OK";
    case APPLIANCE_PRODUCTION_FIRST_BOOT_NULL:
        return "NULL";
    case APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_DEVICE:
        return "BAD_DEVICE";
    case APPLIANCE_PRODUCTION_FIRST_BOOT_STORAGE_UNAVAILABLE:
        return "STORAGE_UNAVAILABLE";
    case APPLIANCE_PRODUCTION_FIRST_BOOT_NETWORK_UNAVAILABLE:
        return "NETWORK_UNAVAILABLE";
    case APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_CREDENTIAL:
        return "BAD_CREDENTIAL";
    case APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_WIZARD:
        return "BAD_WIZARD";
    case APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_PERSISTENCE:
        return "BAD_PERSISTENCE";
    case APPLIANCE_PRODUCTION_FIRST_BOOT_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    default:
        return NULL;
    }
}
