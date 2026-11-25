#ifndef HFOC_APPLIANCE_HEALTH_H
#define HFOC_APPLIANCE_HEALTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_HEALTH_VERSION 1U
#define APPLIANCE_HEALTH_DOMAIN_COUNT 6U

enum appliance_health_status {
    APPLIANCE_HEALTH_STATUS_OK = 0,
    APPLIANCE_HEALTH_STATUS_NULL = 1,
    APPLIANCE_HEALTH_STATUS_BAD_VERSION = 2,
    APPLIANCE_HEALTH_STATUS_BAD_STATE = 3,
    APPLIANCE_HEALTH_STATUS_BAD_DOMAIN = 4,
    APPLIANCE_HEALTH_STATUS_BAD_FAULT = 5,
};

enum appliance_health_state {
    APPLIANCE_HEALTH_STATE_UNKNOWN = 0,
    APPLIANCE_HEALTH_STATE_PASS = 1,
    APPLIANCE_HEALTH_STATE_WARN = 2,
    APPLIANCE_HEALTH_STATE_FAIL = 3,
    APPLIANCE_HEALTH_STATE_OK = APPLIANCE_HEALTH_STATE_PASS,
    APPLIANCE_HEALTH_STATE_DEGRADED = APPLIANCE_HEALTH_STATE_WARN,
    APPLIANCE_HEALTH_STATE_FAILED = APPLIANCE_HEALTH_STATE_FAIL,
};

enum appliance_health_domain {
    APPLIANCE_HEALTH_DOMAIN_NONE = 0,
    APPLIANCE_HEALTH_DOMAIN_BOOT = 1,
    APPLIANCE_HEALTH_DOMAIN_STORAGE = 2,
    APPLIANCE_HEALTH_DOMAIN_CONFIGURATION = 3,
    APPLIANCE_HEALTH_DOMAIN_NETWORK = 4,
    APPLIANCE_HEALTH_DOMAIN_SERVICE = 5,
    APPLIANCE_HEALTH_DOMAIN_HTTPS_MANAGEMENT = 6,
};

enum appliance_health_fault {
    APPLIANCE_HEALTH_FAULT_NONE = 0,
    APPLIANCE_HEALTH_DOMAIN_FAULT_UNKNOWN_BASE = 100,
    APPLIANCE_HEALTH_BOOT_FAULT_INCOMPLETE = 101,
    APPLIANCE_HEALTH_BOOT_FAULT_PANIC = 102,
    APPLIANCE_HEALTH_STORAGE_FAULT_JOURNAL = 110,
    APPLIANCE_HEALTH_STORAGE_FAULT_CHECKPOINT = 111,
    APPLIANCE_HEALTH_STORAGE_FAULT_BACKUP = 112,
    APPLIANCE_HEALTH_CONFIG_FAULT_MISSING = 120,
    APPLIANCE_HEALTH_CONFIG_FAULT_INVALID = 121,
    APPLIANCE_HEALTH_CONFIG_FAULT_COMMIT = 122,
    APPLIANCE_HEALTH_NETWORK_FAULT_LINK = 130,
    APPLIANCE_HEALTH_NETWORK_FAULT_ADDRESS = 131,
    APPLIANCE_HEALTH_NETWORK_FAULT_REACHABILITY = 132,
    APPLIANCE_HEALTH_SERVICE_FAULT_NOT_READY = 140,
    APPLIANCE_HEALTH_SERVICE_FAULT_STOPPED = 141,
    APPLIANCE_HEALTH_SERVICE_FAULT_FAILED = 142,
    APPLIANCE_HEALTH_HTTPS_FAULT_LISTENER = 150,
    APPLIANCE_HEALTH_HTTPS_FAULT_AUTH = 151,
    APPLIANCE_HEALTH_HTTPS_FAULT_UNAVAILABLE = 152,
};

struct appliance_health_verdict {
    uint32_t version;
    enum appliance_health_domain domain;
    enum appliance_health_state state;
    enum appliance_health_fault fault;
    uint64_t last_checked_ticks;
};

struct appliance_health_input {
    uint32_t version;
    uint64_t generation;
    bool critical;
    struct appliance_health_verdict domains[
        APPLIANCE_HEALTH_DOMAIN_COUNT];
};

struct appliance_health_output {
    uint32_t version;
    bool published;
    enum appliance_health_state state;
    bool healthy;
    bool critical;
    enum appliance_health_domain first_fault_domain;
    enum appliance_health_fault first_fault;
};

enum appliance_health_status appliance_health_evaluate(
    const struct appliance_health_input *input,
    struct appliance_health_output *output_out);
const char *appliance_health_status_string(
    enum appliance_health_status status);
const char *appliance_health_state_string(
    enum appliance_health_state state);
const char *appliance_health_domain_string(
    enum appliance_health_domain domain);
const char *appliance_health_fault_string(
    enum appliance_health_fault fault);

#endif
