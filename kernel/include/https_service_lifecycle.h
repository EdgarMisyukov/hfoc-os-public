#ifndef HFOC_HTTPS_SERVICE_LIFECYCLE_H
#define HFOC_HTTPS_SERVICE_LIFECYCLE_H

#include <stdbool.h>
#include <stdint.h>

#define HTTPS_SERVICE_LIFECYCLE_VERSION 1U
#define HTTPS_SERVICE_LIFECYCLE_PHASE_COUNT 6U
#define HTTPS_SERVICE_LIFECYCLE_DRAIN_TIMEOUT_MIN 1U
#define HTTPS_SERVICE_LIFECYCLE_DRAIN_TIMEOUT_MAX (UINT32_MAX / 4U)
#define HTTPS_SERVICE_LIFECYCLE_GENERATION_MIN 1U
#define HTTPS_SERVICE_LIFECYCLE_GENERATION_MAX UINT32_C(0xFFFFFFFE)
#define HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY 4U
#define HTTPS_SERVICE_LIFECYCLE_TLS_CONTEXT_CAPACITY 4U
#define HTTPS_SERVICE_LIFECYCLE_HTTP_PARSER_CAPACITY 4U
#define HTTPS_SERVICE_LIFECYCLE_PACKET_POOL_CAPACITY_MIN 1U
#define HTTPS_SERVICE_LIFECYCLE_PACKET_POOL_CAPACITY_MAX 64U

enum https_service_lifecycle_phase {
    HTTPS_SERVICE_LIFECYCLE_PHASE_IDLE = 0,
    HTTPS_SERVICE_LIFECYCLE_PHASE_LISTENING = 1,
    HTTPS_SERVICE_LIFECYCLE_PHASE_DRAINING = 2,
    HTTPS_SERVICE_LIFECYCLE_PHASE_DRAINED = 3,
    HTTPS_SERVICE_LIFECYCLE_PHASE_STOPPED = 4,
    HTTPS_SERVICE_LIFECYCLE_PHASE_RESTARTED = 5,
};

enum https_service_lifecycle_status {
    HTTPS_SERVICE_LIFECYCLE_OK = 0,
    HTTPS_SERVICE_LIFECYCLE_NULL = 1,
    HTTPS_SERVICE_LIFECYCLE_BAD_STATE = 2,
    HTTPS_SERVICE_LIFECYCLE_INVALID_VERSION = 3,
    HTTPS_SERVICE_LIFECYCLE_INVALID_CONFIG = 4,
    HTTPS_SERVICE_LIFECYCLE_ALREADY_PUBLISHED = 5,
    HTTPS_SERVICE_LIFECYCLE_UNPUBLISHED = 6,
    HTTPS_SERVICE_LIFECYCLE_INVALID_PHASE = 7,
    HTTPS_SERVICE_LIFECYCLE_INVALID_GENERATION = 8,
    HTTPS_SERVICE_LIFECYCLE_INVALID_OPS = 9,
    HTTPS_SERVICE_LIFECYCLE_ADMISSION_STOP_FAILED = 10,
    HTTPS_SERVICE_LIFECYCLE_DRAIN_FAILED = 11,
    HTTPS_SERVICE_LIFECYCLE_NOT_DRAINED = 12,
    HTTPS_SERVICE_LIFECYCLE_ZEROIZE_FAILED = 13,
    HTTPS_SERVICE_LIFECYCLE_HTTP_RELEASE_FAILED = 14,
    HTTPS_SERVICE_LIFECYCLE_PACKET_RELEASE_FAILED = 15,
    HTTPS_SERVICE_LIFECYCLE_RESTART_FAILED = 16,
    HTTPS_SERVICE_LIFECYCLE_GENERATION_EXHAUSTED = 17,
};

struct https_service_lifecycle_config {
    uint32_t version;
    uint32_t drain_timeout_ms;
    uint32_t connection_capacity;
    uint32_t tls_context_capacity;
    uint32_t http_parser_capacity;
    uint32_t packet_pool_capacity;
};

struct https_service_lifecycle_ops {
    bool (*stop_admission)(void *context, uint32_t generation);
    bool (*drain_connection)(void *context, uint32_t generation,
                             uint32_t slot);
    bool (*abort_connection)(void *context, uint32_t generation,
                             uint32_t slot);
    bool (*zeroize_tls)(void *context, uint32_t generation,
                        uint32_t count);
    bool (*release_http)(void *context, uint32_t generation,
                         uint32_t count);
    bool (*release_packet)(void *context, uint32_t generation,
                           uint32_t count);
    bool (*restart_admission)(void *context, uint32_t generation);
    void *context;
};

struct https_service_lifecycle {
    bool published;
    uint32_t version;
    enum https_service_lifecycle_phase phase;
    uint32_t generation;
    uint32_t drain_timeout_ms;
    uint32_t connection_capacity;
    uint32_t tls_context_capacity;
    uint32_t http_parser_capacity;
    uint32_t packet_pool_capacity;
    uint32_t drain_deadline_ms;
    uint32_t active_slots;
    uint32_t stopped_count;
    uint32_t drained_count;
    uint32_t aborted_count;
    uint32_t zeroized_count;
    uint32_t http_released_count;
    uint32_t packet_released_count;
    uint32_t restarted_count;
    struct https_service_lifecycle_ops ops;
};

struct https_service_lifecycle_snapshot {
    uint32_t version;
    enum https_service_lifecycle_phase phase;
    uint32_t generation;
    uint32_t drain_deadline_ms;
    uint32_t active_connection_count;
    uint32_t stopped_count;
    uint32_t drained_count;
    uint32_t aborted_count;
    uint32_t zeroized_count;
    uint32_t http_released_count;
    uint32_t packet_released_count;
    uint32_t restarted_count;
};

enum https_service_lifecycle_status https_service_lifecycle_init(
    struct https_service_lifecycle *lifecycle,
    const struct https_service_lifecycle_config *config,
    const struct https_service_lifecycle_ops *ops);
enum https_service_lifecycle_status https_service_lifecycle_start(
    struct https_service_lifecycle *lifecycle,
    uint32_t generation);
enum https_service_lifecycle_status https_service_lifecycle_stop_admission(
    struct https_service_lifecycle *lifecycle,
    uint32_t now_ms);
enum https_service_lifecycle_status https_service_lifecycle_step(
    struct https_service_lifecycle *lifecycle,
    uint32_t now_ms);
enum https_service_lifecycle_status https_service_lifecycle_finish_drain(
    struct https_service_lifecycle *lifecycle);
enum https_service_lifecycle_status https_service_lifecycle_restart(
    struct https_service_lifecycle *lifecycle);
enum https_service_lifecycle_status https_service_lifecycle_snapshot(
    const struct https_service_lifecycle *lifecycle,
    struct https_service_lifecycle_snapshot *snapshot_out);
bool https_service_lifecycle_published(
    const struct https_service_lifecycle *lifecycle);
enum https_service_lifecycle_phase https_service_lifecycle_phase(
    const struct https_service_lifecycle *lifecycle);
uint32_t https_service_lifecycle_generation(
    const struct https_service_lifecycle *lifecycle);
uint32_t https_service_lifecycle_active_connection_count(
    const struct https_service_lifecycle *lifecycle);
const char *https_service_lifecycle_phase_string(
    enum https_service_lifecycle_phase phase);
const char *https_service_lifecycle_status_string(
    enum https_service_lifecycle_status status);

#endif
