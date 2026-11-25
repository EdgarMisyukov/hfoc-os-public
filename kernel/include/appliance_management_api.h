#ifndef HFOC_APPLIANCE_MANAGEMENT_API_H
#define HFOC_APPLIANCE_MANAGEMENT_API_H

#include "http_parser.h"
#include "http_response.h"

#include <stdbool.h>
#include <stdint.h>

#define APPLIANCE_MANAGEMENT_API_VERSION 1U
#define APPLIANCE_MANAGEMENT_API_PREFIX "/api"
#define APPLIANCE_MANAGEMENT_API_PREFIX_LENGTH 4U
#define APPLIANCE_MANAGEMENT_API_PATH "/api/v1"
#define APPLIANCE_MANAGEMENT_API_PATH_LENGTH 7U
#define APPLIANCE_MANAGEMENT_API_BODY_MAX 160U

enum appliance_management_api_route {
    APPLIANCE_MANAGEMENT_API_ROUTE_NONE = 0,
    APPLIANCE_MANAGEMENT_API_ROUTE_VERSION = 1,
};

enum appliance_management_api_status {
    APPLIANCE_MANAGEMENT_API_OK = 0,
    APPLIANCE_MANAGEMENT_API_NULL = 1,
    APPLIANCE_MANAGEMENT_API_NOT_MANAGEMENT = 2,
    APPLIANCE_MANAGEMENT_API_NOT_FOUND = 3,
    APPLIANCE_MANAGEMENT_API_METHOD_NOT_ALLOWED = 4,
};

struct appliance_management_api_result {
    uint32_t version;
    bool published;
    enum appliance_management_api_route route;
    struct http_response response;
};

enum appliance_management_api_status appliance_management_api_dispatch(
    enum http_method_kind method,
    const uint8_t *target,
    uint32_t target_length,
    struct appliance_management_api_result *result_out);
const char *appliance_management_api_status_string(
    enum appliance_management_api_status status);
const char *appliance_management_api_route_string(
    enum appliance_management_api_route route);

#endif
