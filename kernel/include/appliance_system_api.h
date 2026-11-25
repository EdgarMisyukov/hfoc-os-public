#ifndef HFOC_APPLIANCE_SYSTEM_API_H
#define HFOC_APPLIANCE_SYSTEM_API_H

#include "appliance_config_validation.h"
#include "appliance_identity.h"
#include "http_parser.h"
#include "http_response.h"

#include <stdbool.h>
#include <stdint.h>

#define APPLIANCE_SYSTEM_API_VERSION 1U
#define APPLIANCE_SYSTEM_API_PATH "/api/v1/system"
#define APPLIANCE_SYSTEM_API_PATH_LENGTH 14U
#define APPLIANCE_SYSTEM_API_BODY_MAX HTTP_RESPONSE_API_BODY_MAX
#define APPLIANCE_SYSTEM_API_FIELD_MAX APPLIANCE_IDENTITY_FIELD_MAX
#define APPLIANCE_SYSTEM_API_CONFIG_HOSTNAME_MAX \
    APPLIANCE_CONFIG_VALIDATION_HOSTNAME_MAX
#define APPLIANCE_SYSTEM_API_COUNT_MAX 255U
#define APPLIANCE_SYSTEM_API_RECOVERY_MODE_MAX 2U

enum appliance_system_api_status {
    APPLIANCE_SYSTEM_API_OK = 0,
    APPLIANCE_SYSTEM_API_NULL = 1,
    APPLIANCE_SYSTEM_API_NOT_SYSTEM = 2,
    APPLIANCE_SYSTEM_API_METHOD_NOT_ALLOWED = 3,
    APPLIANCE_SYSTEM_API_BAD_STATE = 4,
    APPLIANCE_SYSTEM_API_BAD_VERSION = 5,
    APPLIANCE_SYSTEM_API_BAD_IDENTITY = 6,
    APPLIANCE_SYSTEM_API_BAD_CONFIG = 7,
    APPLIANCE_SYSTEM_API_BAD_HEALTH = 8,
    APPLIANCE_SYSTEM_API_OVERFLOW = 9,
};

enum appliance_system_api_state {
    APPLIANCE_SYSTEM_STATE_UNAVAILABLE = 0,
    APPLIANCE_SYSTEM_STATE_OK = 1,
    APPLIANCE_SYSTEM_STATE_DEGRADED = 2,
    APPLIANCE_SYSTEM_STATE_FAILED = 3,
};

struct appliance_system_api_snapshot {
    uint32_t version;
    bool published;
    char machine_class[APPLIANCE_SYSTEM_API_FIELD_MAX];
    char product[APPLIANCE_SYSTEM_API_FIELD_MAX];
    char architecture[APPLIANCE_SYSTEM_API_FIELD_MAX];
    char platform[APPLIANCE_SYSTEM_API_FIELD_MAX];
    char hostname[APPLIANCE_SYSTEM_API_FIELD_MAX];
    char instance[APPLIANCE_SYSTEM_API_FIELD_MAX];
    enum appliance_system_api_state health_state;
    uint64_t uptime_ticks;
    enum appliance_system_api_state config_state;
    enum appliance_config_validation_source config_source;
    char config_hostname[APPLIANCE_SYSTEM_API_CONFIG_HOSTNAME_MAX + 1U];
    uint8_t config_hostname_length;
    uint32_t recovery_mode;
    uint32_t active_process_count;
};

struct appliance_system_api_result {
    uint32_t version;
    bool published;
    struct http_response response;
    uint8_t body[APPLIANCE_SYSTEM_API_BODY_MAX];
};

enum appliance_system_api_status appliance_system_api_snapshot_init(
    struct appliance_system_api_snapshot *snapshot,
    const struct appliance_identity *identity,
    enum appliance_system_api_state health_state,
    uint64_t uptime_ticks,
    enum appliance_system_api_state config_state,
    enum appliance_config_validation_source config_source,
    const char *config_hostname,
    size_t config_hostname_length,
    uint32_t recovery_mode,
    uint32_t active_process_count);
enum appliance_system_api_status appliance_system_api_dispatch(
    enum http_method_kind method,
    const uint8_t *target,
    uint32_t target_length,
    const struct appliance_system_api_snapshot *snapshot,
    struct appliance_system_api_result *result_out);
const char *appliance_system_api_status_string(
    enum appliance_system_api_status status);
const char *appliance_system_api_state_string(
    enum appliance_system_api_state state);

#endif
