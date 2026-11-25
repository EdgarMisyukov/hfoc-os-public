#ifndef HFOC_APPLIANCE_CONSOLE_H
#define HFOC_APPLIANCE_CONSOLE_H

#include "appliance_config_validation.h"
#include "appliance_identity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_CONSOLE_VERSION 1U
#define APPLIANCE_CONSOLE_FIELD_MAX 32U
#define APPLIANCE_CONSOLE_HOSTNAME_MAX \
    APPLIANCE_CONFIG_VALIDATION_HOSTNAME_MAX
#define APPLIANCE_CONSOLE_DOMAIN_MAX 8U
#define APPLIANCE_CONSOLE_OUTPUT_MAX 512U

enum appliance_console_state {
    APPLIANCE_CONSOLE_STATE_UNAVAILABLE = 0,
    APPLIANCE_CONSOLE_STATE_OK = 1,
    APPLIANCE_CONSOLE_STATE_DEGRADED = 2,
    APPLIANCE_CONSOLE_STATE_FAILED = 3,
};

enum appliance_console_status {
    APPLIANCE_CONSOLE_OK = 0,
    APPLIANCE_CONSOLE_NULL = 1,
    APPLIANCE_CONSOLE_BAD_VERSION = 2,
    APPLIANCE_CONSOLE_BAD_STATE = 3,
    APPLIANCE_CONSOLE_BAD_IDENTITY = 4,
    APPLIANCE_CONSOLE_BAD_CONFIG = 5,
    APPLIANCE_CONSOLE_BAD_HEALTH = 6,
    APPLIANCE_CONSOLE_BAD_NETWORK = 7,
    APPLIANCE_CONSOLE_BAD_SERVICE = 8,
    APPLIANCE_CONSOLE_OVERFLOW = 9,
};

struct appliance_console_snapshot {
    uint32_t version;
    bool published;
    bool identity_published;
    char product[APPLIANCE_CONSOLE_FIELD_MAX];
    char architecture[APPLIANCE_CONSOLE_FIELD_MAX];
    char platform[APPLIANCE_CONSOLE_FIELD_MAX];
    char hostname[APPLIANCE_CONSOLE_HOSTNAME_MAX + 1U];
    enum appliance_console_state config_state;
    enum appliance_config_validation_source config_source;
    char config_hostname[APPLIANCE_CONSOLE_HOSTNAME_MAX + 1U];
    enum appliance_console_state health_state;
    uint64_t uptime_ticks;
    enum appliance_console_state network_state;
    uint64_t network_generation;
    uint32_t network_devices;
    enum appliance_console_state services_state;
    uint32_t service_count;
    uint32_t service_ready;
    uint32_t service_failed;
};

enum appliance_console_status appliance_console_snapshot_init(
    struct appliance_console_snapshot *snapshot,
    const struct appliance_identity *identity,
    enum appliance_console_state health_state,
    uint64_t uptime_ticks);
enum appliance_console_status appliance_console_publish_config(
    struct appliance_console_snapshot *snapshot,
    enum appliance_config_validation_source source,
    const char *hostname,
    size_t hostname_length);
enum appliance_console_status appliance_console_publish_network(
    struct appliance_console_snapshot *snapshot,
    enum appliance_console_state state,
    uint64_t generation,
    uint32_t device_count);
enum appliance_console_status appliance_console_publish_services(
    struct appliance_console_snapshot *snapshot,
    enum appliance_console_state state,
    uint32_t service_count,
    uint32_t ready_count,
    uint32_t failed_count);
enum appliance_console_status appliance_console_render(
    const struct appliance_console_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length);
const char *appliance_console_state_string(
    enum appliance_console_state state);
const char *appliance_console_status_string(
    enum appliance_console_status status);

#endif
