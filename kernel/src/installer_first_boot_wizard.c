#include "installer_first_boot_wizard.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool installer_first_boot_wizard_bytes_nonzero(
    const uint8_t *bytes,
    size_t length)
{
    size_t index;
    uint8_t nonzero = 0U;

    if (bytes == NULL) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        nonzero |= bytes[index];
    }
    return nonzero != 0U;
}

static bool installer_first_boot_wizard_reserved_zero(
    const uint8_t *bytes,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool installer_first_boot_wizard_at_rest_valid(
    const struct appliance_secret_protection_at_rest_record *record)
{
    return record != NULL &&
           record->version == APPLIANCE_SECRET_PROTECTION_VERSION &&
           record->kind ==
               APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL &&
           record->generation != 0U &&
           installer_first_boot_wizard_bytes_nonzero(
               record->fingerprint,
               sizeof(record->fingerprint)) &&
           installer_first_boot_wizard_reserved_zero(
               record->reserved,
               sizeof(record->reserved));
}

static bool installer_first_boot_wizard_stage_valid(
    enum installer_first_boot_wizard_stage stage)
{
    return stage >= INSTALLER_FIRST_BOOT_WIZARD_STAGE_NONE &&
           stage <= INSTALLER_FIRST_BOOT_WIZARD_STAGE_COMPLETE;
}

static bool installer_first_boot_wizard_state_valid(
    const struct installer_first_boot_wizard *wizard)
{
    return wizard != NULL &&
           wizard->initialized &&
           wizard->version == INSTALLER_FIRST_BOOT_WIZARD_VERSION &&
           installer_first_boot_wizard_stage_valid(wizard->stage);
}

static enum installer_first_boot_wizard_status
installer_first_boot_wizard_check_stage(
    struct installer_first_boot_wizard *wizard,
    enum installer_first_boot_wizard_stage required)
{
    if (wizard == NULL) {
        return INSTALLER_FIRST_BOOT_WIZARD_NULL;
    }
    if (!installer_first_boot_wizard_state_valid(wizard)) {
        return INSTALLER_FIRST_BOOT_WIZARD_BAD_STATE;
    }
    if (wizard->stage != required) {
        return INSTALLER_FIRST_BOOT_WIZARD_BAD_STATE;
    }
    return INSTALLER_FIRST_BOOT_WIZARD_OK;
}

static bool installer_first_boot_wizard_ipv4_equal(
    const struct network_ipv4_config *left,
    const struct network_ipv4_config *right)
{
    return left->schema_version == right->schema_version &&
           left->prefix_length == right->prefix_length &&
           memcmp(left->address.octets,
                  right->address.octets,
                  sizeof(left->address.octets)) == 0 &&
           memcmp(left->gateway.octets,
                  right->gateway.octets,
                  sizeof(left->gateway.octets)) == 0 &&
           memcmp(left->dns.octets,
                  right->dns.octets,
                  sizeof(left->dns.octets)) == 0;
}

static void installer_first_boot_wizard_append_bytes(
    char *output,
    size_t capacity,
    size_t *position,
    const char *bytes,
    size_t length)
{
    size_t index;

    if (*position + length >= capacity) {
        return;
    }
    for (index = 0U; index < length; ++index) {
        output[*position + index] = bytes[index];
    }
    *position += length;
    output[*position] = '\0';
}

static void installer_first_boot_wizard_append_string(
    char *output,
    size_t capacity,
    size_t *position,
    const char *text)
{
    size_t index;

    for (index = 0U; text[index] != '\0'; ++index) {
        installer_first_boot_wizard_append_bytes(
            output, capacity, position, &text[index], 1U);
    }
}

static void installer_first_boot_wizard_append_u8(
    char *output,
    size_t capacity,
    size_t *position,
    uint8_t value)
{
    char digits[3U];
    size_t count = 0U;
    uint16_t remaining = value;

    do {
        digits[count] = (char)('0' + (remaining % 10U));
        ++count;
        remaining /= 10U;
    } while (remaining != 0U);

    while (count > 0U) {
        --count;
        installer_first_boot_wizard_append_bytes(
            output, capacity, position, &digits[count], 1U);
    }
}

static bool installer_first_boot_wizard_render_ready(
    const struct network_ipv4_config *runtime,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    char buffer[INSTALLER_FIRST_BOOT_WIZARD_OUTPUT_MAX];
    size_t position = 0U;
    size_t octet;

    installer_first_boot_wizard_append_string(
        buffer, sizeof(buffer), &position,
        "HFOC Appliance is ready\n\n"
        "Management:\n"
        "https://");
    for (octet = 0U;
         octet < NETWORK_IPV4_ADDRESS_OCTETS;
         ++octet) {
        if (octet != 0U) {
            installer_first_boot_wizard_append_bytes(
                buffer, sizeof(buffer), &position, ".", 1U);
        }
        installer_first_boot_wizard_append_u8(
            buffer, sizeof(buffer), &position,
            runtime->address.octets[octet]);
    }
    installer_first_boot_wizard_append_string(
        buffer, sizeof(buffer), &position, "/\n");
    if (position == 0U ||
        position > capacity ||
        output == NULL ||
        output_length == NULL) {
        return false;
    }
    memcpy(output, buffer, position);
    output[position] = '\0';
    *output_length = position;
    return true;
}

enum installer_first_boot_wizard_status
installer_first_boot_wizard_init(
    struct installer_first_boot_wizard *wizard)
{
    if (wizard == NULL) {
        return INSTALLER_FIRST_BOOT_WIZARD_NULL;
    }
    if (wizard->version == INSTALLER_FIRST_BOOT_WIZARD_VERSION &&
        wizard->initialized) {
        return INSTALLER_FIRST_BOOT_WIZARD_BAD_STATE;
    }

    memset(wizard, 0, sizeof(*wizard));
    wizard->version = INSTALLER_FIRST_BOOT_WIZARD_VERSION;
    wizard->initialized = true;
    wizard->stage = INSTALLER_FIRST_BOOT_WIZARD_STAGE_NONE;
    return INSTALLER_FIRST_BOOT_WIZARD_OK;
}

enum installer_first_boot_wizard_status
installer_first_boot_wizard_begin(
    struct installer_first_boot_wizard *wizard,
    const struct installer_first_boot_wizard_evidence *evidence)
{
    enum installer_discovery_status discovery_status;
    enum installer_first_boot_wizard_status status;

    if (wizard == NULL || evidence == NULL) {
        return INSTALLER_FIRST_BOOT_WIZARD_NULL;
    }
    status = installer_first_boot_wizard_check_stage(
        wizard, INSTALLER_FIRST_BOOT_WIZARD_STAGE_NONE);
    if (status != INSTALLER_FIRST_BOOT_WIZARD_OK) {
        return status;
    }
    if (evidence->version != INSTALLER_FIRST_BOOT_WIZARD_VERSION) {
        return INSTALLER_FIRST_BOOT_WIZARD_BAD_VERSION;
    }
    if (evidence->discovery == NULL) {
        return INSTALLER_FIRST_BOOT_WIZARD_BAD_EVIDENCE;
    }
    discovery_status =
        installer_discovery_validate(evidence->discovery);
    if (discovery_status != INSTALLER_DISCOVERY_OK) {
        return INSTALLER_FIRST_BOOT_WIZARD_BAD_EVIDENCE;
    }
    if (!evidence->first_boot_pending) {
        return INSTALLER_FIRST_BOOT_WIZARD_NOT_FIRST_BOOT;
    }
    if (!evidence->storage_ready) {
        return INSTALLER_FIRST_BOOT_WIZARD_STORAGE_UNAVAILABLE;
    }
    if (evidence->recovery_trust_active) {
        return INSTALLER_FIRST_BOOT_WIZARD_RECOVERY_TRUST_ACTIVE;
    }
    if (evidence->discovery->nic_count == 0U) {
        return INSTALLER_FIRST_BOOT_WIZARD_NO_NIC;
    }

    wizard->nic_available = true;
    wizard->stage = INSTALLER_FIRST_BOOT_WIZARD_STAGE_BOOT;
    return INSTALLER_FIRST_BOOT_WIZARD_OK;
}

enum installer_first_boot_wizard_status
installer_first_boot_wizard_submit_admin(
    struct installer_first_boot_wizard *wizard,
    const struct installer_first_boot_wizard_admin *admin)
{
    enum installer_first_boot_wizard_status status;

    if (wizard == NULL || admin == NULL) {
        return INSTALLER_FIRST_BOOT_WIZARD_NULL;
    }
    status = installer_first_boot_wizard_check_stage(
        wizard, INSTALLER_FIRST_BOOT_WIZARD_STAGE_BOOT);
    if (status != INSTALLER_FIRST_BOOT_WIZARD_OK) {
        return status;
    }
    if (admin->version != INSTALLER_FIRST_BOOT_WIZARD_VERSION) {
        return INSTALLER_FIRST_BOOT_WIZARD_BAD_VERSION;
    }
    if (!installer_first_boot_wizard_at_rest_valid(
            admin->at_rest)) {
        return INSTALLER_FIRST_BOOT_WIZARD_BAD_ADMIN_RECORD;
    }

    wizard->admin_secret_ready = true;
    wizard->stage = INSTALLER_FIRST_BOOT_WIZARD_STAGE_ADMIN;
    return INSTALLER_FIRST_BOOT_WIZARD_OK;
}

enum installer_first_boot_wizard_status
installer_first_boot_wizard_defer_admin(
    struct installer_first_boot_wizard *wizard)
{
    enum installer_first_boot_wizard_status status;

    if (wizard == NULL) {
        return INSTALLER_FIRST_BOOT_WIZARD_NULL;
    }
    status = installer_first_boot_wizard_check_stage(
        wizard, INSTALLER_FIRST_BOOT_WIZARD_STAGE_BOOT);
    if (status != INSTALLER_FIRST_BOOT_WIZARD_OK) {
        return status;
    }
    if (wizard->admin_secret_ready) {
        return INSTALLER_FIRST_BOOT_WIZARD_BAD_STATE;
    }
    wizard->admin_deferred = true;
    wizard->stage = INSTALLER_FIRST_BOOT_WIZARD_STAGE_ADMIN;
    return INSTALLER_FIRST_BOOT_WIZARD_OK;
}

enum installer_first_boot_wizard_status
installer_first_boot_wizard_submit_hostname(
    struct installer_first_boot_wizard *wizard,
    const char *hostname,
    size_t hostname_length)
{
    enum appliance_config_validation_status validation_status;
    enum installer_first_boot_wizard_status status;

    if (wizard == NULL || hostname == NULL) {
        return INSTALLER_FIRST_BOOT_WIZARD_NULL;
    }
    status = installer_first_boot_wizard_check_stage(
        wizard, INSTALLER_FIRST_BOOT_WIZARD_STAGE_ADMIN);
    if (status != INSTALLER_FIRST_BOOT_WIZARD_OK) {
        return status;
    }
    validation_status =
        appliance_config_validation_validate_hostname(
            hostname, hostname_length);
    if (validation_status != APPLIANCE_CONFIG_VALIDATION_OK) {
        return INSTALLER_FIRST_BOOT_WIZARD_BAD_HOSTNAME;
    }
    if (hostname_length >
        INSTALLER_FIRST_BOOT_WIZARD_HOSTNAME_MAX) {
        return INSTALLER_FIRST_BOOT_WIZARD_BAD_HOSTNAME;
    }
    memcpy(wizard->hostname, hostname, hostname_length);
    wizard->hostname[hostname_length] = '\0';
    wizard->hostname_length = (uint8_t)hostname_length;
    wizard->stage = INSTALLER_FIRST_BOOT_WIZARD_STAGE_HOSTNAME;
    return INSTALLER_FIRST_BOOT_WIZARD_OK;
}

enum installer_first_boot_wizard_status
installer_first_boot_wizard_submit_network(
    struct installer_first_boot_wizard *wizard,
    const struct appliance_network_config *network)
{
    enum appliance_network_config_status validation_status;
    enum installer_first_boot_wizard_status status;

    if (wizard == NULL || network == NULL) {
        return INSTALLER_FIRST_BOOT_WIZARD_NULL;
    }
    status = installer_first_boot_wizard_check_stage(
        wizard, INSTALLER_FIRST_BOOT_WIZARD_STAGE_HOSTNAME);
    if (status != INSTALLER_FIRST_BOOT_WIZARD_OK) {
        return status;
    }
    validation_status =
        appliance_network_config_validate(network);
    if (validation_status != APPLIANCE_NETWORK_CONFIG_OK) {
        return INSTALLER_FIRST_BOOT_WIZARD_BAD_NETWORK;
    }
    if (!wizard->nic_available) {
        return INSTALLER_FIRST_BOOT_WIZARD_NO_NIC;
    }

    wizard->network = *network;
    wizard->stage = INSTALLER_FIRST_BOOT_WIZARD_STAGE_NETWORK;
    return INSTALLER_FIRST_BOOT_WIZARD_OK;
}

enum installer_first_boot_wizard_status
installer_first_boot_wizard_submit_connectivity(
    struct installer_first_boot_wizard *wizard,
    const struct installer_first_boot_wizard_connectivity *connectivity)
{
    enum network_ipv4_status ipv4_status;
    enum installer_first_boot_wizard_status status;

    if (wizard == NULL || connectivity == NULL) {
        return INSTALLER_FIRST_BOOT_WIZARD_NULL;
    }
    status = installer_first_boot_wizard_check_stage(
        wizard, INSTALLER_FIRST_BOOT_WIZARD_STAGE_NETWORK);
    if (status != INSTALLER_FIRST_BOOT_WIZARD_OK) {
        return status;
    }
    if (connectivity->version !=
        INSTALLER_FIRST_BOOT_WIZARD_VERSION) {
        return INSTALLER_FIRST_BOOT_WIZARD_BAD_VERSION;
    }
    if (connectivity->runtime == NULL) {
        return INSTALLER_FIRST_BOOT_WIZARD_NULL;
    }
    if (!connectivity->configured ||
        !connectivity->gateway_reachable ||
        !connectivity->dns_reachable) {
        return INSTALLER_FIRST_BOOT_WIZARD_BAD_CONNECTIVITY;
    }
    ipv4_status =
        network_ipv4_config_validate(connectivity->runtime);
    if (ipv4_status != NETWORK_IPV4_OK) {
        return INSTALLER_FIRST_BOOT_WIZARD_BAD_RUNTIME_CONFIG;
    }
    if (wizard->network.mode == APPLIANCE_NETWORK_CONFIG_MODE_STATIC) {
        if (!wizard->network.static_present ||
            !installer_first_boot_wizard_ipv4_equal(
                connectivity->runtime,
                &wizard->network.static_config)) {
            return INSTALLER_FIRST_BOOT_WIZARD_BAD_RUNTIME_CONFIG;
        }
    } else if (wizard->network.mode !=
               APPLIANCE_NETWORK_CONFIG_MODE_DHCP) {
        return INSTALLER_FIRST_BOOT_WIZARD_BAD_NETWORK;
    }

    wizard->runtime = *connectivity->runtime;
    wizard->runtime_ready = true;
    wizard->stage =
        INSTALLER_FIRST_BOOT_WIZARD_STAGE_CONNECTIVITY;
    return INSTALLER_FIRST_BOOT_WIZARD_OK;
}

enum installer_first_boot_wizard_status
installer_first_boot_wizard_submit_persistence(
    struct installer_first_boot_wizard *wizard,
    const struct installer_first_boot_wizard_persistence *persistence)
{
    enum installer_first_boot_wizard_status status;

    if (wizard == NULL || persistence == NULL) {
        return INSTALLER_FIRST_BOOT_WIZARD_NULL;
    }
    status = installer_first_boot_wizard_check_stage(
        wizard, INSTALLER_FIRST_BOOT_WIZARD_STAGE_CONNECTIVITY);
    if (status != INSTALLER_FIRST_BOOT_WIZARD_OK) {
        return status;
    }
    if (persistence->version !=
        INSTALLER_FIRST_BOOT_WIZARD_VERSION) {
        return INSTALLER_FIRST_BOOT_WIZARD_BAD_VERSION;
    }
    if (persistence->admin_secret_record_committed !=
            wizard->admin_secret_ready ||
        !persistence->hostname_record_committed ||
        !persistence->network_records_committed) {
        return INSTALLER_FIRST_BOOT_WIZARD_BAD_PERSISTENCE;
    }

    wizard->persistence_complete = true;
    wizard->stage = INSTALLER_FIRST_BOOT_WIZARD_STAGE_READY;
    return INSTALLER_FIRST_BOOT_WIZARD_OK;
}

enum installer_first_boot_wizard_status
installer_first_boot_wizard_publish_ready(
    struct installer_first_boot_wizard *wizard,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    enum installer_first_boot_wizard_status status;
    size_t length = 0U;

    if (wizard == NULL || output == NULL ||
        output_length == NULL) {
        return INSTALLER_FIRST_BOOT_WIZARD_NULL;
    }
    if (!installer_first_boot_wizard_state_valid(wizard)) {
        return INSTALLER_FIRST_BOOT_WIZARD_BAD_STATE;
    }
    if (wizard->ready_published &&
        wizard->stage == INSTALLER_FIRST_BOOT_WIZARD_STAGE_COMPLETE) {
        return INSTALLER_FIRST_BOOT_WIZARD_OK;
    }
    status = installer_first_boot_wizard_check_stage(
        wizard, INSTALLER_FIRST_BOOT_WIZARD_STAGE_READY);
    if (status != INSTALLER_FIRST_BOOT_WIZARD_OK) {
        return status;
    }
    if (!installer_first_boot_wizard_render_ready(
            &wizard->runtime, output, capacity, &length)) {
        return INSTALLER_FIRST_BOOT_WIZARD_BUFFER_TOO_SMALL;
    }

    *output_length = length;
    wizard->ready_published = true;
    wizard->stage = INSTALLER_FIRST_BOOT_WIZARD_STAGE_COMPLETE;
    return INSTALLER_FIRST_BOOT_WIZARD_OK;
}

enum installer_first_boot_wizard_stage
installer_first_boot_wizard_stage(
    const struct installer_first_boot_wizard *wizard)
{
    if (!installer_first_boot_wizard_state_valid(wizard)) {
        return INSTALLER_FIRST_BOOT_WIZARD_STAGE_NONE;
    }
    return wizard->stage;
}

const char *installer_first_boot_wizard_status_string(
    enum installer_first_boot_wizard_status status)
{
    switch (status) {
    case INSTALLER_FIRST_BOOT_WIZARD_OK:
        return "INSTALLER_FIRST_BOOT_WIZARD_OK";
    case INSTALLER_FIRST_BOOT_WIZARD_NULL:
        return "INSTALLER_FIRST_BOOT_WIZARD_NULL";
    case INSTALLER_FIRST_BOOT_WIZARD_BAD_STATE:
        return "INSTALLER_FIRST_BOOT_WIZARD_BAD_STATE";
    case INSTALLER_FIRST_BOOT_WIZARD_BAD_VERSION:
        return "INSTALLER_FIRST_BOOT_WIZARD_BAD_VERSION";
    case INSTALLER_FIRST_BOOT_WIZARD_BAD_EVIDENCE:
        return "INSTALLER_FIRST_BOOT_WIZARD_BAD_EVIDENCE";
    case INSTALLER_FIRST_BOOT_WIZARD_NOT_FIRST_BOOT:
        return "INSTALLER_FIRST_BOOT_WIZARD_NOT_FIRST_BOOT";
    case INSTALLER_FIRST_BOOT_WIZARD_STORAGE_UNAVAILABLE:
        return "INSTALLER_FIRST_BOOT_WIZARD_STORAGE_UNAVAILABLE";
    case INSTALLER_FIRST_BOOT_WIZARD_RECOVERY_TRUST_ACTIVE:
        return "INSTALLER_FIRST_BOOT_WIZARD_RECOVERY_TRUST_ACTIVE";
    case INSTALLER_FIRST_BOOT_WIZARD_NO_NIC:
        return "INSTALLER_FIRST_BOOT_WIZARD_NO_NIC";
    case INSTALLER_FIRST_BOOT_WIZARD_BAD_ADMIN_RECORD:
        return "INSTALLER_FIRST_BOOT_WIZARD_BAD_ADMIN_RECORD";
    case INSTALLER_FIRST_BOOT_WIZARD_BAD_HOSTNAME:
        return "INSTALLER_FIRST_BOOT_WIZARD_BAD_HOSTNAME";
    case INSTALLER_FIRST_BOOT_WIZARD_BAD_NETWORK:
        return "INSTALLER_FIRST_BOOT_WIZARD_BAD_NETWORK";
    case INSTALLER_FIRST_BOOT_WIZARD_BAD_CONNECTIVITY:
        return "INSTALLER_FIRST_BOOT_WIZARD_BAD_CONNECTIVITY";
    case INSTALLER_FIRST_BOOT_WIZARD_BAD_RUNTIME_CONFIG:
        return "INSTALLER_FIRST_BOOT_WIZARD_BAD_RUNTIME_CONFIG";
    case INSTALLER_FIRST_BOOT_WIZARD_BAD_PERSISTENCE:
        return "INSTALLER_FIRST_BOOT_WIZARD_BAD_PERSISTENCE";
    case INSTALLER_FIRST_BOOT_WIZARD_BUFFER_TOO_SMALL:
        return "INSTALLER_FIRST_BOOT_WIZARD_BUFFER_TOO_SMALL";
    default:
        return NULL;
    }
}

const char *installer_first_boot_wizard_stage_string(
    enum installer_first_boot_wizard_stage stage)
{
    switch (stage) {
    case INSTALLER_FIRST_BOOT_WIZARD_STAGE_NONE:
        return "INSTALLER_FIRST_BOOT_WIZARD_STAGE_NONE";
    case INSTALLER_FIRST_BOOT_WIZARD_STAGE_BOOT:
        return "INSTALLER_FIRST_BOOT_WIZARD_STAGE_BOOT";
    case INSTALLER_FIRST_BOOT_WIZARD_STAGE_ADMIN:
        return "INSTALLER_FIRST_BOOT_WIZARD_STAGE_ADMIN";
    case INSTALLER_FIRST_BOOT_WIZARD_STAGE_HOSTNAME:
        return "INSTALLER_FIRST_BOOT_WIZARD_STAGE_HOSTNAME";
    case INSTALLER_FIRST_BOOT_WIZARD_STAGE_NETWORK:
        return "INSTALLER_FIRST_BOOT_WIZARD_STAGE_NETWORK";
    case INSTALLER_FIRST_BOOT_WIZARD_STAGE_CONNECTIVITY:
        return "INSTALLER_FIRST_BOOT_WIZARD_STAGE_CONNECTIVITY";
    case INSTALLER_FIRST_BOOT_WIZARD_STAGE_PERSISTENCE:
        return "INSTALLER_FIRST_BOOT_WIZARD_STAGE_PERSISTENCE";
    case INSTALLER_FIRST_BOOT_WIZARD_STAGE_READY:
        return "INSTALLER_FIRST_BOOT_WIZARD_STAGE_READY";
    case INSTALLER_FIRST_BOOT_WIZARD_STAGE_COMPLETE:
        return "INSTALLER_FIRST_BOOT_WIZARD_STAGE_COMPLETE";
    default:
        return NULL;
    }
}
