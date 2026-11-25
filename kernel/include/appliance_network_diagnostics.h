#ifndef HFOC_APPLIANCE_NETWORK_DIAGNOSTICS_H
#define HFOC_APPLIANCE_NETWORK_DIAGNOSTICS_H

#include "appliance_network_config.h"
#include "health_check.h"
#include "link_state.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_NETWORK_DIAGNOSTICS_VERSION 1U
#define APPLIANCE_NETWORK_DIAGNOSTICS_OUTPUT_MAX 512U

enum appliance_network_diagnostics_status {
    APPLIANCE_NETWORK_DIAGNOSTICS_OK = 0,
    APPLIANCE_NETWORK_DIAGNOSTICS_NULL = 1,
    APPLIANCE_NETWORK_DIAGNOSTICS_BAD_VERSION = 2,
    APPLIANCE_NETWORK_DIAGNOSTICS_BAD_STATE = 3,
    APPLIANCE_NETWORK_DIAGNOSTICS_BAD_CONFIG = 4,
    APPLIANCE_NETWORK_DIAGNOSTICS_BAD_LINK = 5,
    APPLIANCE_NETWORK_DIAGNOSTICS_BAD_ACTIVE = 6,
    APPLIANCE_NETWORK_DIAGNOSTICS_BAD_REACHABILITY = 7,
    APPLIANCE_NETWORK_DIAGNOSTICS_OVERFLOW = 8,
};

enum appliance_network_reachability {
    APPLIANCE_NETWORK_REACHABILITY_UNKNOWN = 0,
    APPLIANCE_NETWORK_REACHABILITY_REACHABLE = 1,
    APPLIANCE_NETWORK_REACHABILITY_UNREACHABLE = 2,
};

enum appliance_network_diagnostics_fault {
    APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_NONE = 0,
    APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_LINK = 1,
    APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_ADDRESS = 2,
    APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_CONFIG = 3,
    APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_REACHABILITY = 4,
};

struct appliance_network_diagnostics {
    uint32_t version;
    bool published;
    char interface[APPLIANCE_NETWORK_CONFIG_INTERFACE_MAX + 1U];
    uint8_t interface_length;
    enum appliance_network_config_mode mode;
    uint32_t device_id;
    enum network_link_state link_state;
    uint32_t link_generation;
    bool has_active;
    bool static_config_consistent;
    struct network_ipv4_config active;
    enum appliance_network_reachability gateway_reachability;
    enum appliance_network_reachability dns_reachability;
    enum health_check_result_state result;
    enum appliance_network_diagnostics_fault fault;
};

enum appliance_network_diagnostics_status
appliance_network_diagnostics_publish(
    struct appliance_network_diagnostics *diagnostics,
    const struct appliance_network_config *desired,
    const struct network_link_state_record *link,
    const struct network_ipv4_config *active,
    enum appliance_network_reachability gateway_reachability,
    enum appliance_network_reachability dns_reachability);
enum appliance_network_diagnostics_status
appliance_network_diagnostics_render(
    const struct appliance_network_diagnostics *diagnostics,
    char *output,
    size_t capacity,
    size_t *output_length);
const char *appliance_network_diagnostics_status_string(
    enum appliance_network_diagnostics_status status);
const char *appliance_network_reachability_string(
    enum appliance_network_reachability reachability);
const char *appliance_network_diagnostics_fault_string(
    enum appliance_network_diagnostics_fault fault);

#endif
