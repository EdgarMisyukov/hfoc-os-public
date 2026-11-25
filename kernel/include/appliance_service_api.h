#ifndef HFOC_APPLIANCE_SERVICE_API_H
#define HFOC_APPLIANCE_SERVICE_API_H

#include "appliance_service_manager.h"
#include "health_check.h"
#include "http_parser.h"
#include "http_response.h"
#include "service_health.h"

#include <stdbool.h>
#include <stdint.h>

#define APPLIANCE_SERVICE_API_VERSION 1U
#define APPLIANCE_SERVICE_API_PATH "/api/v1/service"
#define APPLIANCE_SERVICE_API_PATH_LENGTH 15U
#define APPLIANCE_SERVICE_API_BODY_MAX HTTP_RESPONSE_API_BODY_MAX

enum appliance_service_api_status {
    APPLIANCE_SERVICE_API_OK = 0,
    APPLIANCE_SERVICE_API_NULL = 1,
    APPLIANCE_SERVICE_API_NOT_SERVICE = 2,
    APPLIANCE_SERVICE_API_METHOD_NOT_ALLOWED = 3,
    APPLIANCE_SERVICE_API_BAD_STATE = 4,
    APPLIANCE_SERVICE_API_BAD_VERSION = 5,
    APPLIANCE_SERVICE_API_BAD_MANAGER = 6,
    APPLIANCE_SERVICE_API_OVERFLOW = 7,
};

enum appliance_service_api_route {
    APPLIANCE_SERVICE_API_ROUTE_NONE = 0,
    APPLIANCE_SERVICE_API_ROUTE_STATUS = 1,
};

struct appliance_service_api_snapshot {
    uint32_t version;
    bool published;
    struct appliance_service_manager_snapshot manager;
};

struct appliance_service_api_result {
    uint32_t version;
    bool published;
    enum appliance_service_api_route route;
    struct http_response response;
    uint8_t body[APPLIANCE_SERVICE_API_BODY_MAX];
};

enum appliance_service_api_status appliance_service_api_snapshot_init(
    struct appliance_service_api_snapshot *snapshot,
    const struct appliance_service_manager_snapshot *manager);
enum appliance_service_api_status appliance_service_api_dispatch(
    enum http_method_kind method,
    const uint8_t *target,
    uint32_t target_length,
    const struct appliance_service_api_snapshot *snapshot,
    struct appliance_service_api_result *result_out);
const char *appliance_service_api_status_string(
    enum appliance_service_api_status status);
const char *appliance_service_api_route_string(
    enum appliance_service_api_route route);

#endif
