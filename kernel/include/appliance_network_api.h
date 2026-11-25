#ifndef HFOC_APPLIANCE_NETWORK_API_H
#define HFOC_APPLIANCE_NETWORK_API_H

#include "appliance_network_config.h"
#include "appliance_network_diagnostics.h"
#include "http_parser.h"
#include "http_response.h"

#include <stdbool.h>
#include <stdint.h>

#define APPLIANCE_NETWORK_API_VERSION 1U
#define APPLIANCE_NETWORK_API_CONFIG_PATH "/api/v1/network"
#define APPLIANCE_NETWORK_API_CONFIG_PATH_LENGTH 15U
#define APPLIANCE_NETWORK_API_STATUS_PATH "/api/v1/network/status"
#define APPLIANCE_NETWORK_API_STATUS_PATH_LENGTH 22U
#define APPLIANCE_NETWORK_API_BODY_MAX HTTP_RESPONSE_API_BODY_MAX

enum appliance_network_api_status {
    APPLIANCE_NETWORK_API_OK = 0,
    APPLIANCE_NETWORK_API_NULL = 1,
    APPLIANCE_NETWORK_API_NOT_NETWORK = 2,
    APPLIANCE_NETWORK_API_METHOD_NOT_ALLOWED = 3,
    APPLIANCE_NETWORK_API_BAD_STATE = 4,
    APPLIANCE_NETWORK_API_BAD_VERSION = 5,
    APPLIANCE_NETWORK_API_BAD_CONFIG = 6,
    APPLIANCE_NETWORK_API_BAD_DIAGNOSTICS = 7,
    APPLIANCE_NETWORK_API_OVERFLOW = 8,
};

enum appliance_network_api_route {
    APPLIANCE_NETWORK_API_ROUTE_NONE = 0,
    APPLIANCE_NETWORK_API_ROUTE_CONFIG = 1,
    APPLIANCE_NETWORK_API_ROUTE_STATUS = 2,
};

enum appliance_network_api_config_state {
    APPLIANCE_NETWORK_API_CONFIG_UNAVAILABLE = 0,
    APPLIANCE_NETWORK_API_CONFIG_OK = 1,
};

enum appliance_network_api_config_source {
    APPLIANCE_NETWORK_API_CONFIG_SOURCE_NONE = 0,
    APPLIANCE_NETWORK_API_CONFIG_SOURCE_COMMITTED = 1,
    APPLIANCE_NETWORK_API_CONFIG_SOURCE_DEFAULT_MISSING = 2,
    APPLIANCE_NETWORK_API_CONFIG_SOURCE_DEFAULT_INVALID = 3,
};

struct appliance_network_api_snapshot {
    uint32_t version;
    bool published;
    enum appliance_network_api_config_state config_state;
    enum appliance_network_api_config_source config_source;
    struct appliance_network_config desired;
    struct appliance_network_diagnostics diagnostics;
};

struct appliance_network_api_result {
    uint32_t version;
    bool published;
    enum appliance_network_api_route route;
    struct http_response response;
    uint8_t body[APPLIANCE_NETWORK_API_BODY_MAX];
};

enum appliance_network_api_status appliance_network_api_snapshot_init(
    struct appliance_network_api_snapshot *snapshot,
    const struct appliance_network_config *desired,
    enum appliance_network_api_config_source config_source,
    const struct appliance_network_diagnostics *diagnostics);
enum appliance_network_api_status appliance_network_api_dispatch(
    enum http_method_kind method,
    const uint8_t *target,
    uint32_t target_length,
    const struct appliance_network_api_snapshot *snapshot,
    struct appliance_network_api_result *result_out);
const char *appliance_network_api_status_string(
    enum appliance_network_api_status status);
const char *appliance_network_api_route_string(
    enum appliance_network_api_route route);
const char *appliance_network_api_config_state_string(
    enum appliance_network_api_config_state state);
const char *appliance_network_api_config_source_string(
    enum appliance_network_api_config_source source);

#endif
