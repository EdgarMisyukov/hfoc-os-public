#ifndef HFOC_APPLIANCE_PRODUCTION_MANAGEMENT_API_H
#define HFOC_APPLIANCE_PRODUCTION_MANAGEMENT_API_H

#include "appliance_config_validation.h"
#include "appliance_health_api.h"
#include "appliance_identity.h"
#include "appliance_network_api.h"
#include "appliance_network_config.h"
#include "appliance_network_diagnostics.h"
#include "appliance_production_boot.h"
#include "appliance_production_service_activation.h"
#include "appliance_service_api.h"
#include "appliance_system_api.h"
#include "http_parser.h"
#include "http_response.h"
#include "network_device.h"
#include "network_ipv4_profile.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-400 ordinary-boot read-only management snapshot owner.
 *
 * Before this owner an ordinary production boot resolved only the public
 * targets (`/`, `/health`, `/api/v1` and the frozen 404/405 answers).  The
 * four read-only management targets were served by macro-gated QEMU fixtures
 * only, so a real appliance answered every one of them with the unknown-route
 * body.
 *
 * This caller-owned, allocation-free module publishes the four read-only
 * snapshots from live production evidence and owns their route resolution:
 *
 *   /api/v1/system          appliance_system_api
 *   /api/v1/network         appliance_network_api (config)
 *   /api/v1/network/status  appliance_network_api (diagnostics)
 *   /api/v1/service         appliance_service_api
 *   /api/v1/health          appliance_health_api
 *
 * It never fabricates a PASS: an owner that is missing, awaiting evidence or
 * degraded is rendered through the same honest vocabularies the API modules
 * already publish (UNAVAILABLE / DEGRADED / FAILED).  It performs no request
 * authorization: the ITEM-365 production request gate stays in front of every
 * one of these targets, and this module only renders the body once the gate
 * has admitted the request.
 */

#define APPLIANCE_PRODUCTION_MANAGEMENT_API_VERSION 1U
#define APPLIANCE_PRODUCTION_MANAGEMENT_API_EVIDENCE_VERSION 1U
#define APPLIANCE_PRODUCTION_MANAGEMENT_API_BODY_MAX HTTP_RESPONSE_API_BODY_MAX

enum appliance_production_management_api_status {
    APPLIANCE_PRODUCTION_MANAGEMENT_API_OK = 0,
    APPLIANCE_PRODUCTION_MANAGEMENT_API_NULL = 1,
    APPLIANCE_PRODUCTION_MANAGEMENT_API_BAD_STATE = 2,
    APPLIANCE_PRODUCTION_MANAGEMENT_API_BAD_EVIDENCE = 3,
    APPLIANCE_PRODUCTION_MANAGEMENT_API_NOT_OWNED = 4,
    APPLIANCE_PRODUCTION_MANAGEMENT_API_METHOD_NOT_ALLOWED = 5,
    APPLIANCE_PRODUCTION_MANAGEMENT_API_UNAVAILABLE = 6,
};

struct appliance_production_management_api_evidence {
    uint32_t version;
    const struct appliance_identity *identity;
    enum appliance_production_boot_readiness boot_readiness;
    enum appliance_production_boot_mode boot_mode;
    bool storage_ready;
    enum appliance_config_validation_source config_source;
    const char *config_hostname;
    size_t config_hostname_length;
    const struct appliance_network_config *network_desired;
    enum appliance_network_api_config_source network_config_source;
    const struct network_ipv4_config *network_active;
    const struct network_device_snapshot *network_device;
    bool network_link_up;
    bool gateway_reachable;
    bool dns_reachable;
    bool https_listening;
    bool service_present;
    enum appliance_production_service_activation_state service_state;
    uint64_t uptime_ticks;
};

struct appliance_production_management_api {
    bool published;
    uint32_t version;
    struct appliance_production_management_api_evidence evidence;
};

struct appliance_production_management_api_result {
    uint32_t version;
    struct http_response response;
    uint8_t body[APPLIANCE_PRODUCTION_MANAGEMENT_API_BODY_MAX];
};

enum appliance_production_management_api_status
appliance_production_management_api_publish(
    struct appliance_production_management_api *owner,
    const struct appliance_production_management_api_evidence *evidence);
bool appliance_production_management_api_published(
    const struct appliance_production_management_api *owner);
bool appliance_production_management_api_owns(
    const struct appliance_production_management_api *owner,
    const uint8_t *target,
    uint32_t target_length);
enum appliance_production_management_api_status
appliance_production_management_api_dispatch(
    const struct appliance_production_management_api *owner,
    enum http_method_kind method,
    const uint8_t *target,
    uint32_t target_length,
    struct appliance_production_management_api_result *result_out);
const char *appliance_production_management_api_status_string(
    enum appliance_production_management_api_status status);

#endif
