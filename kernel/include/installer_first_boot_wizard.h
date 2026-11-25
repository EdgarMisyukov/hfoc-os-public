#ifndef HFOC_INSTALLER_FIRST_BOOT_WIZARD_H
#define HFOC_INSTALLER_FIRST_BOOT_WIZARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "appliance_config_validation.h"
#include "appliance_network_config.h"
#include "appliance_secret_protection.h"
#include "installer_hardware_discovery.h"

#define INSTALLER_FIRST_BOOT_WIZARD_VERSION 1U
#define INSTALLER_FIRST_BOOT_WIZARD_HOSTNAME_MAX \
    APPLIANCE_CONFIG_VALIDATION_HOSTNAME_MAX
#define INSTALLER_FIRST_BOOT_WIZARD_OUTPUT_MAX 256U

enum installer_first_boot_wizard_stage {
    INSTALLER_FIRST_BOOT_WIZARD_STAGE_NONE = 0,
    INSTALLER_FIRST_BOOT_WIZARD_STAGE_BOOT = 1,
    INSTALLER_FIRST_BOOT_WIZARD_STAGE_ADMIN = 2,
    INSTALLER_FIRST_BOOT_WIZARD_STAGE_HOSTNAME = 3,
    INSTALLER_FIRST_BOOT_WIZARD_STAGE_NETWORK = 4,
    INSTALLER_FIRST_BOOT_WIZARD_STAGE_CONNECTIVITY = 5,
    INSTALLER_FIRST_BOOT_WIZARD_STAGE_PERSISTENCE = 6,
    INSTALLER_FIRST_BOOT_WIZARD_STAGE_READY = 7,
    INSTALLER_FIRST_BOOT_WIZARD_STAGE_COMPLETE = 8,
};

enum installer_first_boot_wizard_status {
    INSTALLER_FIRST_BOOT_WIZARD_OK = 0,
    INSTALLER_FIRST_BOOT_WIZARD_NULL = 1,
    INSTALLER_FIRST_BOOT_WIZARD_BAD_STATE = 2,
    INSTALLER_FIRST_BOOT_WIZARD_BAD_VERSION = 3,
    INSTALLER_FIRST_BOOT_WIZARD_BAD_EVIDENCE = 4,
    INSTALLER_FIRST_BOOT_WIZARD_NOT_FIRST_BOOT = 5,
    INSTALLER_FIRST_BOOT_WIZARD_STORAGE_UNAVAILABLE = 6,
    INSTALLER_FIRST_BOOT_WIZARD_RECOVERY_TRUST_ACTIVE = 7,
    INSTALLER_FIRST_BOOT_WIZARD_NO_NIC = 8,
    INSTALLER_FIRST_BOOT_WIZARD_BAD_ADMIN_RECORD = 9,
    INSTALLER_FIRST_BOOT_WIZARD_BAD_HOSTNAME = 10,
    INSTALLER_FIRST_BOOT_WIZARD_BAD_NETWORK = 11,
    INSTALLER_FIRST_BOOT_WIZARD_BAD_CONNECTIVITY = 12,
    INSTALLER_FIRST_BOOT_WIZARD_BAD_RUNTIME_CONFIG = 13,
    INSTALLER_FIRST_BOOT_WIZARD_BAD_PERSISTENCE = 14,
    INSTALLER_FIRST_BOOT_WIZARD_BUFFER_TOO_SMALL = 15,
};

struct installer_first_boot_wizard_evidence {
    uint32_t version;
    const struct installer_discovery_report *discovery;
    bool first_boot_pending;
    bool storage_ready;
    bool recovery_trust_active;
};

struct installer_first_boot_wizard_admin {
    uint32_t version;
    const struct appliance_secret_protection_at_rest_record *at_rest;
};

struct installer_first_boot_wizard_connectivity {
    uint32_t version;
    bool configured;
    bool gateway_reachable;
    bool dns_reachable;
    const struct network_ipv4_config *runtime;
};

struct installer_first_boot_wizard_persistence {
    uint32_t version;
    bool admin_secret_record_committed;
    bool hostname_record_committed;
    bool network_records_committed;
};

struct installer_first_boot_wizard {
    uint32_t version;
    enum installer_first_boot_wizard_stage stage;
    bool initialized;
    bool admin_secret_ready;
    /*
     * ITEM-403: an ordinary first boot with no operator secret yet advances
     * through the administrator stage without a credential record.  The
     * persistence stage only accepts a deferred flow when it does not claim a
     * committed administrator record.
     */
    bool admin_deferred;
    bool nic_available;
    bool runtime_ready;
    bool persistence_complete;
    bool ready_published;
    uint8_t hostname_length;
    char hostname[
        INSTALLER_FIRST_BOOT_WIZARD_HOSTNAME_MAX + 1U];
    struct appliance_network_config network;
    struct network_ipv4_config runtime;
};

enum installer_first_boot_wizard_status
installer_first_boot_wizard_init(
    struct installer_first_boot_wizard *wizard);
enum installer_first_boot_wizard_status
installer_first_boot_wizard_begin(
    struct installer_first_boot_wizard *wizard,
    const struct installer_first_boot_wizard_evidence *evidence);
enum installer_first_boot_wizard_status
installer_first_boot_wizard_submit_admin(
    struct installer_first_boot_wizard *wizard,
    const struct installer_first_boot_wizard_admin *admin);
/*
 * ITEM-403: advance through the administrator stage without a credential
 * record.  The caller must persist no administrator fingerprint, and the
 * persistence stage rejects a deferred flow that claims one.
 */
enum installer_first_boot_wizard_status
installer_first_boot_wizard_defer_admin(
    struct installer_first_boot_wizard *wizard);
enum installer_first_boot_wizard_status
installer_first_boot_wizard_submit_hostname(
    struct installer_first_boot_wizard *wizard,
    const char *hostname,
    size_t hostname_length);
enum installer_first_boot_wizard_status
installer_first_boot_wizard_submit_network(
    struct installer_first_boot_wizard *wizard,
    const struct appliance_network_config *network);
enum installer_first_boot_wizard_status
installer_first_boot_wizard_submit_connectivity(
    struct installer_first_boot_wizard *wizard,
    const struct installer_first_boot_wizard_connectivity *connectivity);
enum installer_first_boot_wizard_status
installer_first_boot_wizard_submit_persistence(
    struct installer_first_boot_wizard *wizard,
    const struct installer_first_boot_wizard_persistence *persistence);
enum installer_first_boot_wizard_status
installer_first_boot_wizard_publish_ready(
    struct installer_first_boot_wizard *wizard,
    char *output,
    size_t capacity,
    size_t *output_length);
enum installer_first_boot_wizard_stage
installer_first_boot_wizard_stage(
    const struct installer_first_boot_wizard *wizard);
const char *installer_first_boot_wizard_status_string(
    enum installer_first_boot_wizard_status status);
const char *installer_first_boot_wizard_stage_string(
    enum installer_first_boot_wizard_stage stage);

#endif
