#ifndef HFOC_SERVICE_H
#define HFOC_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#define SERVICE_DESCRIPTOR_VERSION 3U
#define SERVICE_ID_MIN 1U
#define SERVICE_ID_MAX 255U
#define SERVICE_NAME_MAX 31U
#define SERVICE_RESTART_LIMIT_MAX 255U
#define SERVICE_READINESS_TIMEOUT_MIN_MS 1U
#define SERVICE_READINESS_TIMEOUT_MAX_MS 600000U
#define SERVICE_CPU_TICKS_LIMIT_MAX UINT64_C(0xFFFFFFFFFFFFFFFE)
#define SERVICE_USER_MEMORY_LIMIT_MAX_PAGES UINT64_C(0xFFFFFFFFFFFFFFFE)
#define SERVICE_DEPENDENCY_MAX 8U
#define SERVICE_DEPENDENCY_SET_MAX 8U
#define SERVICE_GRAPH_SNAPSHOT_VERSION 1U
#define SERVICE_GRAPH_SNAPSHOT_MAX SERVICE_DEPENDENCY_SET_MAX

enum service_descriptor_status {
    SERVICE_DESCRIPTOR_OK = 0,
    SERVICE_DESCRIPTOR_NULL = 1,
    SERVICE_DESCRIPTOR_INVALID_ID = 2,
    SERVICE_DESCRIPTOR_INVALID_VERSION = 3,
    SERVICE_DESCRIPTOR_INVALID_NAME = 4,
    SERVICE_DESCRIPTOR_INVALID_RESTART_LIMIT = 5,
    SERVICE_DESCRIPTOR_INVALID_TIMEOUT = 6,
    SERVICE_DESCRIPTOR_INVALID_DEPENDENCY = 7,
    SERVICE_DESCRIPTOR_INVALID_CPU_LIMIT = 8,
    SERVICE_DESCRIPTOR_INVALID_USER_MEMORY_LIMIT = 9,
};

struct service_descriptor {
    uint32_t id;
    uint32_t version;
    bool enabled;
    uint32_t restart_limit;
    uint32_t readiness_timeout_ms;
    uint64_t cpu_ticks_limit;
    uint64_t user_memory_limit_pages;
    char name[SERVICE_NAME_MAX + 1U];
    uint32_t dependency_count;
    uint8_t dependencies[SERVICE_DEPENDENCY_MAX];
};

enum service_dependency_status {
    SERVICE_DEPENDENCY_OK = 0,
    SERVICE_DEPENDENCY_NULL = 1,
    SERVICE_DEPENDENCY_BAD_CAPACITY = 2,
    SERVICE_DEPENDENCY_INVALID_DESCRIPTOR = 3,
    SERVICE_DEPENDENCY_DUPLICATE_SERVICE = 4,
    SERVICE_DEPENDENCY_UNKNOWN = 5,
    SERVICE_DEPENDENCY_CYCLE = 6,
};

enum service_graph_status {
    SERVICE_GRAPH_OK = 0,
    SERVICE_GRAPH_NULL = 1,
    SERVICE_GRAPH_BAD_CAPACITY = 2,
    SERVICE_GRAPH_BAD_STATE = 3,
    SERVICE_GRAPH_INVALID_SET = 4,
};

struct service_graph_snapshot {
    bool published;
    uint32_t version;
    uint32_t count;
    struct service_descriptor descriptors[SERVICE_GRAPH_SNAPSHOT_MAX];
    uint32_t start_ids[SERVICE_GRAPH_SNAPSHOT_MAX];
};

enum service_descriptor_status service_descriptor_validate(
    const struct service_descriptor *descriptor);
enum service_descriptor_status service_descriptor_init(
    struct service_descriptor *published,
    const struct service_descriptor *descriptor);
const char *service_descriptor_status_string(
    enum service_descriptor_status status);
enum service_dependency_status service_dependency_set_validate(
    const struct service_descriptor *descriptors,
    uint32_t descriptor_count);
const char *service_dependency_status_string(
    enum service_dependency_status status);
enum service_graph_status service_graph_snapshot_build(
    struct service_graph_snapshot *snapshot,
    const struct service_descriptor *descriptors,
    uint32_t descriptor_count);
uint32_t service_graph_snapshot_count(
    const struct service_graph_snapshot *snapshot);
const struct service_descriptor *service_graph_snapshot_descriptor(
    const struct service_graph_snapshot *snapshot,
    uint32_t position);
uint32_t service_graph_snapshot_start_id(
    const struct service_graph_snapshot *snapshot,
    uint32_t position);
bool service_graph_snapshot_published(
    const struct service_graph_snapshot *snapshot);
const char *service_graph_status_string(enum service_graph_status status);

#endif
