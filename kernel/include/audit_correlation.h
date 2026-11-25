#ifndef HFOC_AUDIT_CORRELATION_H
#define HFOC_AUDIT_CORRELATION_H

#include "audit.h"
#include "event_ring.h"

#include <stdint.h>

#define AUDIT_CORRELATION_VERSION 1U
#define AUDIT_CORRELATION_ID_MIN 1U
#define AUDIT_CORRELATION_ID_MAX 255U
#define AUDIT_CORRELATION_OPERATION_MIN 1U
#define AUDIT_CORRELATION_OPERATION_MAX 255U
#define AUDIT_CORRELATION_RESULT_OK 0U
#define AUDIT_CORRELATION_RESULT_DENIED 1U

enum audit_correlation_status {
    AUDIT_CORRELATION_OK = 0,
    AUDIT_CORRELATION_NULL = 1,
    AUDIT_CORRELATION_BAD_VERSION = 2,
    AUDIT_CORRELATION_INVALID_ID = 3,
    AUDIT_CORRELATION_INVALID_OPERATION = 4,
    AUDIT_CORRELATION_INVALID_RESULT = 5,
    AUDIT_CORRELATION_BAD_STATE = 6,
    AUDIT_CORRELATION_AUDIT_FAILED = 7,
};

struct audit_correlation {
    uint32_t version;
    uint32_t session_id;
    uint32_t principal_id;
    uint32_t command_id;
    uint32_t result;
};

enum audit_correlation_status audit_correlation_validate(
    const struct audit_correlation *correlation);
enum audit_correlation_status audit_correlation_append(
    struct event_ring *ring,
    uint32_t operation,
    const struct audit_correlation *correlation,
    uint64_t detail,
    uint64_t *sequence_out);
const char *audit_correlation_status_string(
    enum audit_correlation_status status);

#endif
