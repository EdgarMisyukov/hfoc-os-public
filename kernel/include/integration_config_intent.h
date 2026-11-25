#ifndef HFOC_INTEGRATION_CONFIG_INTENT_H
#define HFOC_INTEGRATION_CONFIG_INTENT_H

#include "audit_correlation.h"
#include "authorization.h"
#include "config_schema.h"
#include "controlled_configuration.h"
#include "event_ring.h"

#include <stddef.h>
#include <stdint.h>

#define INTEGRATION_CONFIG_INTENT_VERSION 1U
#define INTEGRATION_CONFIG_INTENT_PEER_ID_MIN 1U
#define INTEGRATION_CONFIG_INTENT_PEER_ID_MAX 65535U
#define INTEGRATION_CONFIG_INTENT_COUNT 7U

#define INTEGRATION_CONFIG_INTENT_RECOVERY_MODE 1U
#define INTEGRATION_CONFIG_INTENT_MAX_PROCESSES 2U
#define INTEGRATION_CONFIG_INTENT_MAX_USER_MEMORY_PAGES 3U
#define INTEGRATION_CONFIG_INTENT_AUDIT_RING_CAPACITY 4U
#define INTEGRATION_CONFIG_INTENT_BOOT_TIMEOUT_MS 5U
#define INTEGRATION_CONFIG_INTENT_RESTART_LIMIT 6U
#define INTEGRATION_CONFIG_INTENT_SERVICE_HEALTH_TIMEOUT_MS 7U

#define INTEGRATION_CONFIG_INTENT_AUDIT_OPERATION \
    CONTROLLED_CONFIGURATION_AUDIT_OPERATION_ACTIVATE
#define INTEGRATION_CONFIG_INTENT_AUDIT_RESULT_DENIED \
    AUDIT_CORRELATION_RESULT_DENIED

enum integration_config_intent_status {
    INTEGRATION_CONFIG_INTENT_OK = 0,
    INTEGRATION_CONFIG_INTENT_NULL = 1,
    INTEGRATION_CONFIG_INTENT_BAD_STATE = 2,
    INTEGRATION_CONFIG_INTENT_BAD_VERSION = 3,
    INTEGRATION_CONFIG_INTENT_INVALID_PEER_ID = 4,
    INTEGRATION_CONFIG_INTENT_INVALID_INTENT_ID = 5,
    INTEGRATION_CONFIG_INTENT_INVALID_VALUE = 6,
    INTEGRATION_CONFIG_INTENT_DENIED = 7,
    INTEGRATION_CONFIG_INTENT_PARSE_FAILED = 8,
    INTEGRATION_CONFIG_INTENT_COMMIT_FAILED = 9,
    INTEGRATION_CONFIG_INTENT_AUDIT_FAILED = 10,
};

struct integration_config_intent {
    uint32_t version;
    uint32_t intent_id;
    uint32_t peer_id;
    uint32_t value;
};

enum integration_config_intent_status integration_config_intent_validate(
    const struct integration_config_intent *intent);
enum integration_config_intent_status integration_config_intent_apply(
    struct config_snapshot *snapshot,
    const struct integration_config_intent *intent,
    const struct authorization_matrix *matrix,
    const uint8_t *roles,
    size_t role_count,
    const uint8_t *capabilities,
    size_t capability_count,
    const struct audit_correlation *correlation,
    config_commit_validator validator,
    void *context,
    struct event_ring *audit_ring,
    uint64_t *audit_sequence_out);
const char *integration_config_intent_status_string(
    enum integration_config_intent_status status);

#endif
