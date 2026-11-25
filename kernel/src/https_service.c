#include "https_service.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool https_service_name_valid(
    const struct https_service_descriptor *descriptor)
{
    uint32_t index;

    for (index = 0U; index <= HTTPS_SERVICE_NAME_MAX; ++index) {
        const unsigned char byte =
            (unsigned char)descriptor->name[index];

        if (index < HTTPS_SERVICE_NAME_LENGTH) {
            if (byte != (unsigned char)HTTPS_SERVICE_NAME[index]) {
                return false;
            }
        } else if (byte != '\0') {
            return false;
        }
    }
    return true;
}

static bool https_service_resource_envelope_valid(
    const struct https_service_resource_envelope *envelope)
{
    if (envelope == NULL ||
        envelope->connection_capacity !=
            HTTPS_SERVICE_CONNECTION_CAPACITY ||
        envelope->tls_context_capacity !=
            HTTPS_SERVICE_TLS_CONTEXT_CAPACITY ||
        envelope->http_parser_capacity !=
            HTTPS_SERVICE_HTTP_PARSER_CAPACITY ||
        envelope->packet_pool_capacity <
            HTTPS_SERVICE_PACKET_POOL_CAPACITY_MIN ||
        envelope->packet_pool_capacity >
            HTTPS_SERVICE_PACKET_POOL_CAPACITY_MAX) {
        return false;
    }
    return true;
}

enum https_service_status https_service_descriptor_validate(
    const struct https_service_descriptor *descriptor)
{
    if (descriptor == NULL) {
        return HTTPS_SERVICE_NULL;
    }
    if (descriptor->version != HTTPS_SERVICE_DESCRIPTOR_VERSION) {
        return HTTPS_SERVICE_BAD_VERSION;
    }
    if (descriptor->id != HTTPS_SERVICE_ID) {
        return HTTPS_SERVICE_BAD_ID;
    }
    if (!https_service_name_valid(descriptor)) {
        return HTTPS_SERVICE_BAD_NAME;
    }
    if (descriptor->restart_budget <
            HTTPS_SERVICE_RESTART_BUDGET_MIN ||
        descriptor->restart_budget >
            HTTPS_SERVICE_RESTART_BUDGET_MAX) {
        return HTTPS_SERVICE_BAD_RESTART_BUDGET;
    }
    if (descriptor->readiness_deadline_ms <
            HTTPS_SERVICE_READINESS_DEADLINE_MIN_MS ||
        descriptor->readiness_deadline_ms >
            HTTPS_SERVICE_READINESS_DEADLINE_MAX_MS) {
        return HTTPS_SERVICE_BAD_READINESS_DEADLINE;
    }
    if (!https_service_resource_envelope_valid(
            &descriptor->envelope)) {
        return HTTPS_SERVICE_BAD_RESOURCE_ENVELOPE;
    }
    return HTTPS_SERVICE_OK;
}

enum https_service_status https_service_descriptor_init(
    struct https_service_descriptor *published,
    const struct https_service_descriptor *descriptor)
{
    enum https_service_status status;

    if (published == NULL || descriptor == NULL) {
        return HTTPS_SERVICE_NULL;
    }
    if (published->published) {
        return HTTPS_SERVICE_ALREADY_PUBLISHED;
    }
    status = https_service_descriptor_validate(descriptor);
    if (status != HTTPS_SERVICE_OK) {
        return status;
    }
    *published = *descriptor;
    published->published = true;
    return HTTPS_SERVICE_OK;
}

bool https_service_descriptor_published(
    const struct https_service_descriptor *descriptor)
{
    return descriptor != NULL &&
           descriptor->published &&
           https_service_descriptor_validate(descriptor) ==
               HTTPS_SERVICE_OK;
}

enum https_service_status https_service_start_check(
    const struct https_service_descriptor *descriptor,
    const struct https_service_readiness *readiness,
    bool *ready)
{
    enum https_service_status status;

    if (descriptor == NULL || readiness == NULL || ready == NULL) {
        return HTTPS_SERVICE_NULL;
    }
    status = https_service_descriptor_validate(descriptor);
    if (status != HTTPS_SERVICE_OK) {
        return status;
    }
    if (!descriptor->published) {
        return HTTPS_SERVICE_NOT_PUBLISHED;
    }
    if (readiness->version != HTTPS_SERVICE_READINESS_VERSION) {
        return HTTPS_SERVICE_BAD_VERSION;
    }
    if ((readiness->available_mask &
         ~HTTPS_SERVICE_REQUIRED_DEPENDENCY_MASK) != 0U) {
        return HTTPS_SERVICE_BAD_DEPENDENCY;
    }
    if ((readiness->available_mask &
         HTTPS_SERVICE_REQUIRED_DEPENDENCY_MASK) !=
        HTTPS_SERVICE_REQUIRED_DEPENDENCY_MASK) {
        *ready = false;
        return HTTPS_SERVICE_DEPENDENCY_UNAVAILABLE;
    }
    *ready = true;
    return HTTPS_SERVICE_OK;
}

const char *https_service_status_string(
    enum https_service_status status)
{
    switch (status) {
    case HTTPS_SERVICE_OK:
        return "HTTPS_SERVICE_OK";
    case HTTPS_SERVICE_NULL:
        return "HTTPS_SERVICE_NULL";
    case HTTPS_SERVICE_BAD_STATE:
        return "HTTPS_SERVICE_BAD_STATE";
    case HTTPS_SERVICE_BAD_VERSION:
        return "HTTPS_SERVICE_BAD_VERSION";
    case HTTPS_SERVICE_BAD_ID:
        return "HTTPS_SERVICE_BAD_ID";
    case HTTPS_SERVICE_BAD_NAME:
        return "HTTPS_SERVICE_BAD_NAME";
    case HTTPS_SERVICE_BAD_RESTART_BUDGET:
        return "HTTPS_SERVICE_BAD_RESTART_BUDGET";
    case HTTPS_SERVICE_BAD_READINESS_DEADLINE:
        return "HTTPS_SERVICE_BAD_READINESS_DEADLINE";
    case HTTPS_SERVICE_BAD_RESOURCE_ENVELOPE:
        return "HTTPS_SERVICE_BAD_RESOURCE_ENVELOPE";
    case HTTPS_SERVICE_BAD_DEPENDENCY:
        return "HTTPS_SERVICE_BAD_DEPENDENCY";
    case HTTPS_SERVICE_ALREADY_PUBLISHED:
        return "HTTPS_SERVICE_ALREADY_PUBLISHED";
    case HTTPS_SERVICE_NOT_PUBLISHED:
        return "HTTPS_SERVICE_NOT_PUBLISHED";
    case HTTPS_SERVICE_DEPENDENCY_UNAVAILABLE:
        return "HTTPS_SERVICE_DEPENDENCY_UNAVAILABLE";
    default:
        return NULL;
    }
}
