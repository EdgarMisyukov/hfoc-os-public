#ifndef HFOC_APPLIANCE_HEALTH_API_H
#define HFOC_APPLIANCE_HEALTH_API_H

#include "appliance_health.h"
#include "http_parser.h"
#include "http_response.h"

#include <stdbool.h>
#include <stdint.h>

#define APPLIANCE_HEALTH_API_VERSION 1U
#define APPLIANCE_HEALTH_API_PATH "/api/v1/health"
#define APPLIANCE_HEALTH_API_PATH_LENGTH 14U
#define APPLIANCE_HEALTH_API_BODY_MAX HTTP_RESPONSE_API_BODY_MAX

enum appliance_health_api_status {
    APPLIANCE_HEALTH_API_OK = 0,
    APPLIANCE_HEALTH_API_NULL = 1,
    APPLIANCE_HEALTH_API_NOT_HEALTH = 2,
    APPLIANCE_HEALTH_API_METHOD_NOT_ALLOWED = 3,
    APPLIANCE_HEALTH_API_BAD_STATE = 4,
    APPLIANCE_HEALTH_API_BAD_VERSION = 5,
    APPLIANCE_HEALTH_API_BAD_HEALTH = 6,
    APPLIANCE_HEALTH_API_OVERFLOW = 7,
};

struct appliance_health_api_snapshot {
    uint32_t version;
    bool published;
    struct appliance_health_output health;
};

struct appliance_health_api_result {
    uint32_t version;
    bool published;
    struct http_response response;
    uint8_t body[APPLIANCE_HEALTH_API_BODY_MAX];
};

enum appliance_health_api_status appliance_health_api_snapshot_init(
    struct appliance_health_api_snapshot *snapshot,
    const struct appliance_health_input *input);
enum appliance_health_api_status appliance_health_api_dispatch(
    enum http_method_kind method,
    const uint8_t *target,
    uint32_t target_length,
    const struct appliance_health_api_snapshot *snapshot,
    struct appliance_health_api_result *result_out);
const char *appliance_health_api_status_string(
    enum appliance_health_api_status status);

#endif
