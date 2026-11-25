#include "appliance_network_mode.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

enum appliance_network_mode_status appliance_network_mode_decide(
    const struct appliance_network_config *desired,
    bool device_present,
    struct appliance_network_mode_decision *decision)
{
    if (desired == NULL || decision == NULL) {
        return APPLIANCE_NETWORK_MODE_NULL;
    }
    if (desired->version != APPLIANCE_NETWORK_CONFIG_VERSION) {
        return APPLIANCE_NETWORK_MODE_BAD_VERSION;
    }
    if (appliance_network_config_validate(desired) !=
        APPLIANCE_NETWORK_CONFIG_OK) {
        return APPLIANCE_NETWORK_MODE_BAD_CONFIG;
    }

    memset(decision, 0, sizeof(*decision));
    decision->version = APPLIANCE_NETWORK_MODE_VERSION;
    decision->mode = desired->mode;

    if (!device_present) {
        decision->action =
            APPLIANCE_NETWORK_MODE_ACTION_UNAVAILABLE;
        decision->addressing = false;
        decision->degraded = true;
        return APPLIANCE_NETWORK_MODE_OK;
    }

    switch (desired->mode) {
    case APPLIANCE_NETWORK_CONFIG_MODE_DHCP:
        decision->action = APPLIANCE_NETWORK_MODE_ACTION_DHCP;
        decision->addressing = true;
        decision->degraded = false;
        return APPLIANCE_NETWORK_MODE_OK;
    case APPLIANCE_NETWORK_CONFIG_MODE_STATIC:
        decision->action = APPLIANCE_NETWORK_MODE_ACTION_STATIC;
        decision->addressing = true;
        decision->degraded = false;
        return APPLIANCE_NETWORK_MODE_OK;
    case APPLIANCE_NETWORK_CONFIG_MODE_OFFLINE:
        /* No address and no DHCP/DNS attempt: the appliance boots offline
         * and publishes DEGRADED instead of pretending to be connected. */
        decision->action = APPLIANCE_NETWORK_MODE_ACTION_OFFLINE;
        decision->addressing = false;
        decision->degraded = true;
        return APPLIANCE_NETWORK_MODE_OK;
    default:
        return APPLIANCE_NETWORK_MODE_BAD_CONFIG;
    }
}

const char *appliance_network_mode_action_string(
    enum appliance_network_mode_action action)
{
    switch (action) {
    case APPLIANCE_NETWORK_MODE_ACTION_NONE:
        return "NONE";
    case APPLIANCE_NETWORK_MODE_ACTION_DHCP:
        return "DHCP";
    case APPLIANCE_NETWORK_MODE_ACTION_STATIC:
        return "STATIC";
    case APPLIANCE_NETWORK_MODE_ACTION_OFFLINE:
        return "OFFLINE";
    case APPLIANCE_NETWORK_MODE_ACTION_UNAVAILABLE:
        return "UNAVAILABLE";
    default:
        return NULL;
    }
}

const char *appliance_network_mode_status_string(
    enum appliance_network_mode_status status)
{
    switch (status) {
    case APPLIANCE_NETWORK_MODE_OK:
        return "OK";
    case APPLIANCE_NETWORK_MODE_NULL:
        return "NULL";
    case APPLIANCE_NETWORK_MODE_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_NETWORK_MODE_BAD_CONFIG:
        return "BAD_CONFIG";
    default:
        return NULL;
    }
}
