#ifndef HFOC_CONTROLLED_CONFIGURATION_H
#define HFOC_CONTROLLED_CONFIGURATION_H

#include "audit.h"
#include "audit_correlation.h"
#include "authorization.h"
#include "config_schema.h"
#include "event_ring.h"
#include "management_command.h"

#include <stddef.h>
#include <stdint.h>

#define CONTROLLED_CONFIGURATION_VERSION 1U
#define CONTROLLED_CONFIGURATION_COMMAND_ID 2U
#define CONTROLLED_CONFIGURATION_REQUEST_MIN 1U
#define CONTROLLED_CONFIGURATION_REQUEST_MAX 255U
#define CONTROLLED_CONFIGURATION_RESPONSE_MIN 0U
#define CONTROLLED_CONFIGURATION_RESPONSE_MAX 0U
#define CONTROLLED_CONFIGURATION_AUDIT_OPERATION_ACTIVATE 1U
#define CONTROLLED_CONFIGURATION_AUDIT_RESULT_OK 0U
#define CONTROLLED_CONFIGURATION_AUDIT_RESULT_FAILED 1U

enum controlled_configuration_status {
    CONTROLLED_CONFIGURATION_OK = 0,
    CONTROLLED_CONFIGURATION_NULL = 1,
    CONTROLLED_CONFIGURATION_BAD_STATE = 2,
    CONTROLLED_CONFIGURATION_TOO_LONG = 3,
    CONTROLLED_CONFIGURATION_PARSE_FAILED = 4,
    CONTROLLED_CONFIGURATION_COMMIT_FAILED = 5,
    CONTROLLED_CONFIGURATION_AUDIT_FAILED = 6,
};

enum controlled_configuration_status controlled_configuration_apply(
    struct config_snapshot *snapshot,
    const char *input,
    size_t input_length,
    config_commit_validator validator,
    void *context,
    struct event_ring *audit_ring,
    uint64_t *audit_sequence_out);
enum controlled_configuration_status
controlled_configuration_apply_correlated(
    struct config_snapshot *snapshot,
    const char *input,
    size_t input_length,
    config_commit_validator validator,
    void *context,
    const struct audit_correlation *correlation,
    struct event_ring *audit_ring,
    uint64_t *audit_sequence_out);
enum controlled_configuration_status
controlled_configuration_command_descriptor(
    struct management_command_descriptor *descriptor);
enum controlled_configuration_status
controlled_configuration_authorization_entry(
    struct authorization_entry *entry);
const char *controlled_configuration_status_string(
    enum controlled_configuration_status status);

#endif
