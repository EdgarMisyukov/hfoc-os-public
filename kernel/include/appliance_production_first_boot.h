#ifndef HFOC_APPLIANCE_PRODUCTION_FIRST_BOOT_H
#define HFOC_APPLIANCE_PRODUCTION_FIRST_BOOT_H

#include "appliance_config_validation.h"
#include "appliance_network_config.h"
#include "appliance_persistent_secret_config.h"
#include "appliance_state_loader.h"
#include "installer_first_boot_wizard.h"
#include "ipv4_config.h"
#include "network_device.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_PRODUCTION_FIRST_BOOT_VERSION 1U
#define APPLIANCE_PRODUCTION_FIRST_BOOT_HOSTNAME_MAX \
    APPLIANCE_CONFIG_VALIDATION_HOSTNAME_MAX
#define APPLIANCE_PRODUCTION_FIRST_BOOT_CREDENTIAL_LENGTH \
    APPLIANCE_SECRET_PROTECTION_CREDENTIAL_LENGTH
#define APPLIANCE_PRODUCTION_FIRST_BOOT_OUTPUT_MAX 256U

enum appliance_production_first_boot_state {
    APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_NONE = 0,
    APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_AWAITING_STORAGE = 1,
    APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_AWAITING_NETWORK = 2,
    APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_PROVISIONED = 3,
    APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_RELOADED = 4,
    APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_FAILED = 5,
};

enum appliance_production_first_boot_status {
    APPLIANCE_PRODUCTION_FIRST_BOOT_OK = 0,
    APPLIANCE_PRODUCTION_FIRST_BOOT_NULL = 1,
    APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_STATE = 2,
    APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_VERSION = 3,
    APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_DEVICE = 4,
    APPLIANCE_PRODUCTION_FIRST_BOOT_STORAGE_UNAVAILABLE = 5,
    APPLIANCE_PRODUCTION_FIRST_BOOT_NETWORK_UNAVAILABLE = 6,
    APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_CREDENTIAL = 7,
    APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_WIZARD = 8,
    APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_PERSISTENCE = 9,
    APPLIANCE_PRODUCTION_FIRST_BOOT_BUFFER_TOO_SMALL = 10,
};

struct appliance_production_first_boot {
    bool initialized;
    uint32_t version;
    enum appliance_production_first_boot_state state;
    enum appliance_production_first_boot_status last_status;
    bool first_boot_pending;
    bool storage_ready;
    bool network_ready;
    bool wizard_complete;
    bool ready_published;
    struct appliance_state_loader *loader;
    struct network_device_snapshot device;
    char hostname[
        APPLIANCE_PRODUCTION_FIRST_BOOT_HOSTNAME_MAX + 1U];
    uint8_t hostname_length;
    struct appliance_network_config network;
    struct network_ipv4_config runtime;
    uint8_t admin_fingerprint[SHA256_DIGEST_SIZE];
    /*
     * ITEM-403: false when ordinary first boot deferred the administrator
     * credential; the local console enrollment owner provisions it later.
     */
    bool admin_provisioned;
    char ready_output[APPLIANCE_PRODUCTION_FIRST_BOOT_OUTPUT_MAX];
    size_t ready_output_length;
    struct installer_first_boot_wizard wizard;
};

enum appliance_production_first_boot_status
appliance_production_first_boot_init(
    struct appliance_production_first_boot *first_boot,
    struct appliance_state_loader *loader,
    const struct network_device_snapshot *device);
enum appliance_production_first_boot_status
appliance_production_first_boot_run(
    struct appliance_production_first_boot *first_boot,
    const char *hostname,
    size_t hostname_length,
    const struct network_ipv4_config *runtime,
    bool gateway_reachable,
    bool dns_reachable,
    const uint8_t credential[APPLIANCE_PRODUCTION_FIRST_BOOT_CREDENTIAL_LENGTH]);
/*
 * ITEM-403: ordinary first boot with no operator secret yet.  The wizard
 * advances through the administrator stage without committing a credential
 * record; the local console enrollment owner provisions it later.
 */
enum appliance_production_first_boot_status
appliance_production_first_boot_run_deferred(
    struct appliance_production_first_boot *first_boot,
    const char *hostname,
    size_t hostname_length,
    const struct network_ipv4_config *runtime,
    bool gateway_reachable,
    bool dns_reachable);
enum appliance_production_first_boot_status
appliance_production_first_boot_render(
    const struct appliance_production_first_boot *first_boot,
    char *output,
    size_t capacity,
    size_t *output_length);
bool appliance_production_first_boot_pending(
    const struct appliance_production_first_boot *first_boot);
bool appliance_production_first_boot_provisioned(
    const struct appliance_production_first_boot *first_boot);
/*
 * ITEM-403: true only when this boot committed (or loaded) an administrator
 * credential record.
 */
bool appliance_production_first_boot_admin_provisioned(
    const struct appliance_production_first_boot *first_boot);
const struct network_ipv4_config *
appliance_production_first_boot_runtime(
    const struct appliance_production_first_boot *first_boot);
const char *appliance_production_first_boot_state_string(
    enum appliance_production_first_boot_state state);
const char *appliance_production_first_boot_status_string(
    enum appliance_production_first_boot_status status);

#endif
