#ifndef HFOC_HTTPS_SERVICE_H
#define HFOC_HTTPS_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#define HTTPS_SERVICE_DESCRIPTOR_VERSION 1U
#define HTTPS_SERVICE_READINESS_VERSION 1U
#define HTTPS_SERVICE_ID 1U
#define HTTPS_SERVICE_NAME "https"
#define HTTPS_SERVICE_NAME_LENGTH 5U
#define HTTPS_SERVICE_NAME_MAX 15U

#define HTTPS_SERVICE_RESTART_BUDGET_MIN 1U
#define HTTPS_SERVICE_RESTART_BUDGET_MAX 255U
#define HTTPS_SERVICE_RESTART_BUDGET_DEFAULT 3U
#define HTTPS_SERVICE_READINESS_DEADLINE_MIN_MS 1U
#define HTTPS_SERVICE_READINESS_DEADLINE_MAX_MS 600000U
#define HTTPS_SERVICE_READINESS_DEADLINE_DEFAULT_MS 5000U

#define HTTPS_SERVICE_CONNECTION_CAPACITY 4U
#define HTTPS_SERVICE_TLS_CONTEXT_CAPACITY 4U
#define HTTPS_SERVICE_HTTP_PARSER_CAPACITY 4U
#define HTTPS_SERVICE_PACKET_POOL_CAPACITY_MIN 1U
#define HTTPS_SERVICE_PACKET_POOL_CAPACITY_MAX 64U
#define HTTPS_SERVICE_PACKET_POOL_CAPACITY_DEFAULT 16U

#define HTTPS_SERVICE_DEPENDENCY_NETWORK_READY 0x1U
#define HTTPS_SERVICE_DEPENDENCY_ENTROPY_READY 0x2U
#define HTTPS_SERVICE_DEPENDENCY_CERTIFICATE_READY 0x4U
#define HTTPS_SERVICE_REQUIRED_DEPENDENCY_MASK 0x7U

enum https_service_status {
    HTTPS_SERVICE_OK = 0,
    HTTPS_SERVICE_NULL = 1,
    HTTPS_SERVICE_BAD_STATE = 2,
    HTTPS_SERVICE_BAD_VERSION = 3,
    HTTPS_SERVICE_BAD_ID = 4,
    HTTPS_SERVICE_BAD_NAME = 5,
    HTTPS_SERVICE_BAD_RESTART_BUDGET = 6,
    HTTPS_SERVICE_BAD_READINESS_DEADLINE = 7,
    HTTPS_SERVICE_BAD_RESOURCE_ENVELOPE = 8,
    HTTPS_SERVICE_BAD_DEPENDENCY = 9,
    HTTPS_SERVICE_ALREADY_PUBLISHED = 10,
    HTTPS_SERVICE_NOT_PUBLISHED = 11,
    HTTPS_SERVICE_DEPENDENCY_UNAVAILABLE = 12,
};

struct https_service_resource_envelope {
    uint32_t connection_capacity;
    uint32_t tls_context_capacity;
    uint32_t http_parser_capacity;
    uint32_t packet_pool_capacity;
};

struct https_service_descriptor {
    uint32_t version;
    uint32_t id;
    char name[HTTPS_SERVICE_NAME_MAX + 1U];
    uint32_t restart_budget;
    uint32_t readiness_deadline_ms;
    struct https_service_resource_envelope envelope;
    bool published;
};

struct https_service_readiness {
    uint32_t version;
    uint32_t available_mask;
};

enum https_service_status https_service_descriptor_validate(
    const struct https_service_descriptor *descriptor);
enum https_service_status https_service_descriptor_init(
    struct https_service_descriptor *published,
    const struct https_service_descriptor *descriptor);
bool https_service_descriptor_published(
    const struct https_service_descriptor *descriptor);
enum https_service_status https_service_start_check(
    const struct https_service_descriptor *descriptor,
    const struct https_service_readiness *readiness,
    bool *ready);
const char *https_service_status_string(
    enum https_service_status status);

#endif
