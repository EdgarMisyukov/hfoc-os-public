#ifndef HFOC_APPLIANCE_PRODUCTION_SERVICE_HTTPS_H
#define HFOC_APPLIANCE_PRODUCTION_SERVICE_HTTPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_PRODUCTION_SERVICE_HTTPS_VERSION 1U
#define APPLIANCE_PRODUCTION_SERVICE_HTTPS_EVIDENCE_VERSION 1U
#define APPLIANCE_PRODUCTION_SERVICE_HTTPS_READINESS_VERSION 1U
#define APPLIANCE_PRODUCTION_SERVICE_HTTPS_OUTPUT_MAX 512U

enum appliance_production_service_https_state {
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_NONE = 0,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STOPPED = 1,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_CONFIGURED = 2,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_AWAITING_READINESS = 3,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STARTING = 4,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_RUNNING = 5,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STOPPING = 6,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_FAILED = 7,
};

enum appliance_production_service_https_status {
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK = 0,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_NULL = 1,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_BAD_STATE = 2,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_BAD_VERSION = 3,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_ALREADY_PUBLISHED = 4,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_UNPUBLISHED = 5,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_INVALID_EVIDENCE = 6,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_STALE_EVIDENCE = 7,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_MISSING_READINESS = 8,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_START_FAILED = 9,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_STOP_PENDING = 10,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_STOP_FAILED = 11,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_BUFFER_TOO_SMALL = 12,
};

enum appliance_production_service_https_failure {
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_NONE = 0,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_START = 1,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_READY = 2,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_STOP = 3,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_SERVICE = 4,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_NETWORK = 5,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_RETRY_EXHAUSTED = 6,
    APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_INVALID_EVIDENCE = 7,
};

struct appliance_production_service_https_evidence {
    uint32_t version;
    uint64_t source_generation;
    bool published;
    bool loaded_state_committed;
    bool manager_configured;
    bool manager_stopped;
    bool https_enabled;
    bool https_stopped;
};

struct appliance_production_service_https_readiness {
    uint32_t version;
    uint64_t source_generation;
    bool network_active;
    bool entropy_ready;
    bool certificate_ready;
};

struct appliance_production_service_https {
    bool initialized;
    bool configured;
    bool readiness_present;
    bool evidence_published;
    uint32_t version;
    enum appliance_production_service_https_state state;
    enum appliance_production_service_https_status last_status;
    enum appliance_production_service_https_failure failure;
    bool https_enabled;
    bool network_active;
    bool entropy_ready;
    bool certificate_ready;
    uint64_t source_generation;
    uint32_t start_count;
    uint32_t stop_count;
    uint32_t reset_count;
};

struct appliance_production_service_https_snapshot {
    bool published;
    uint32_t version;
    enum appliance_production_service_https_state state;
    enum appliance_production_service_https_status last_status;
    enum appliance_production_service_https_failure failure;
    bool https_enabled;
    bool evidence_published;
    uint64_t source_generation;
    uint32_t start_count;
    uint32_t stop_count;
    uint32_t reset_count;
};

enum appliance_production_service_https_status
appliance_production_service_https_init(
    struct appliance_production_service_https *composition);
enum appliance_production_service_https_status
appliance_production_service_https_configure(
    struct appliance_production_service_https *composition,
    const struct appliance_production_service_https_evidence *evidence);
enum appliance_production_service_https_status
appliance_production_service_https_readiness(
    struct appliance_production_service_https *composition,
    const struct appliance_production_service_https_readiness *readiness);
enum appliance_production_service_https_status
appliance_production_service_https_start(
    struct appliance_production_service_https *composition);
enum appliance_production_service_https_status
appliance_production_service_https_running(
    struct appliance_production_service_https *composition);
enum appliance_production_service_https_status
appliance_production_service_https_start_failed(
    struct appliance_production_service_https *composition,
    enum appliance_production_service_https_failure failure);
enum appliance_production_service_https_status
appliance_production_service_https_stop(
    struct appliance_production_service_https *composition);
enum appliance_production_service_https_status
appliance_production_service_https_stopped(
    struct appliance_production_service_https *composition);
enum appliance_production_service_https_status
appliance_production_service_https_stop_failed(
    struct appliance_production_service_https *composition);
enum appliance_production_service_https_status
appliance_production_service_https_fail_closed(
    struct appliance_production_service_https *composition,
    enum appliance_production_service_https_failure failure);
enum appliance_production_service_https_status
appliance_production_service_https_reset(
    struct appliance_production_service_https *composition);
enum appliance_production_service_https_status
appliance_production_service_https_snapshot(
    const struct appliance_production_service_https *composition,
    struct appliance_production_service_https_snapshot *snapshot_out);
enum appliance_production_service_https_status
appliance_production_service_https_render(
    const struct appliance_production_service_https_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length);
enum appliance_production_service_https_state
appliance_production_service_https_state(
    const struct appliance_production_service_https *composition);
enum appliance_production_service_https_failure
appliance_production_service_https_failure(
    const struct appliance_production_service_https *composition);
bool appliance_production_service_https_published(
    const struct appliance_production_service_https *composition);
const char *appliance_production_service_https_state_string(
    enum appliance_production_service_https_state state);
const char *appliance_production_service_https_status_string(
    enum appliance_production_service_https_status status);
const char *appliance_production_service_https_failure_string(
    enum appliance_production_service_https_failure failure);

#endif
