#include "service.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool service_name_valid(const struct service_descriptor *descriptor) {
    size_t index = 0U;

    while (index <= SERVICE_NAME_MAX) {
        unsigned char byte = (unsigned char)descriptor->name[index];

        if (byte == '\0') {
            break;
        }
        if (byte < 0x20U || byte > 0x7EU) {
            return false;
        }
        ++index;
    }
    if (index == 0U || index > SERVICE_NAME_MAX ||
        descriptor->name[0] == ' ' ||
        descriptor->name[index - 1U] == ' ') {
        return false;
    }
    while (index <= SERVICE_NAME_MAX) {
        if (descriptor->name[index] != '\0') {
            return false;
        }
        ++index;
    }
    return true;
}

static bool service_dependency_list_valid(
    const struct service_descriptor *descriptor)
{
    uint32_t index;
    uint32_t previous;

    if (descriptor->dependency_count > SERVICE_DEPENDENCY_MAX) {
        return false;
    }
    for (index = 0U; index < descriptor->dependency_count; ++index) {
        const uint8_t dependency = descriptor->dependencies[index];

        if (dependency < SERVICE_ID_MIN ||
            dependency == descriptor->id) {
            return false;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (dependency == descriptor->dependencies[previous]) {
                return false;
            }
        }
    }
    return true;
}

enum service_descriptor_status service_descriptor_validate(
    const struct service_descriptor *descriptor)
{
    if (descriptor == NULL) {
        return SERVICE_DESCRIPTOR_NULL;
    }
    if (descriptor->id < SERVICE_ID_MIN ||
        descriptor->id > SERVICE_ID_MAX) {
        return SERVICE_DESCRIPTOR_INVALID_ID;
    }
    if (descriptor->version != SERVICE_DESCRIPTOR_VERSION) {
        return SERVICE_DESCRIPTOR_INVALID_VERSION;
    }
    if (!service_name_valid(descriptor)) {
        return SERVICE_DESCRIPTOR_INVALID_NAME;
    }
    if (descriptor->restart_limit > SERVICE_RESTART_LIMIT_MAX) {
        return SERVICE_DESCRIPTOR_INVALID_RESTART_LIMIT;
    }
    if (descriptor->readiness_timeout_ms <
            SERVICE_READINESS_TIMEOUT_MIN_MS ||
        descriptor->readiness_timeout_ms >
            SERVICE_READINESS_TIMEOUT_MAX_MS) {
        return SERVICE_DESCRIPTOR_INVALID_TIMEOUT;
    }
    if (descriptor->cpu_ticks_limit > SERVICE_CPU_TICKS_LIMIT_MAX) {
        return SERVICE_DESCRIPTOR_INVALID_CPU_LIMIT;
    }
    if (descriptor->user_memory_limit_pages >
            SERVICE_USER_MEMORY_LIMIT_MAX_PAGES) {
        return SERVICE_DESCRIPTOR_INVALID_USER_MEMORY_LIMIT;
    }
    if (!service_dependency_list_valid(descriptor)) {
        return SERVICE_DESCRIPTOR_INVALID_DEPENDENCY;
    }
    return SERVICE_DESCRIPTOR_OK;
}

enum service_descriptor_status service_descriptor_init(
    struct service_descriptor *published,
    const struct service_descriptor *descriptor)
{
    enum service_descriptor_status status;

    if (published == NULL || descriptor == NULL) {
        return SERVICE_DESCRIPTOR_NULL;
    }
    status = service_descriptor_validate(descriptor);
    if (status != SERVICE_DESCRIPTOR_OK) {
        return status;
    }
    if (published != descriptor) {
        *published = *descriptor;
    }
    return SERVICE_DESCRIPTOR_OK;
}

const char *service_descriptor_status_string(
    enum service_descriptor_status status)
{
    switch (status) {
    case SERVICE_DESCRIPTOR_OK:
        return "SERVICE_DESCRIPTOR_OK";
    case SERVICE_DESCRIPTOR_NULL:
        return "SERVICE_DESCRIPTOR_NULL";
    case SERVICE_DESCRIPTOR_INVALID_ID:
        return "SERVICE_DESCRIPTOR_INVALID_ID";
    case SERVICE_DESCRIPTOR_INVALID_VERSION:
        return "SERVICE_DESCRIPTOR_INVALID_VERSION";
    case SERVICE_DESCRIPTOR_INVALID_NAME:
        return "SERVICE_DESCRIPTOR_INVALID_NAME";
    case SERVICE_DESCRIPTOR_INVALID_RESTART_LIMIT:
        return "SERVICE_DESCRIPTOR_INVALID_RESTART_LIMIT";
    case SERVICE_DESCRIPTOR_INVALID_TIMEOUT:
        return "SERVICE_DESCRIPTOR_INVALID_TIMEOUT";
    case SERVICE_DESCRIPTOR_INVALID_DEPENDENCY:
        return "SERVICE_DESCRIPTOR_INVALID_DEPENDENCY";
    case SERVICE_DESCRIPTOR_INVALID_CPU_LIMIT:
        return "SERVICE_DESCRIPTOR_INVALID_CPU_LIMIT";
    case SERVICE_DESCRIPTOR_INVALID_USER_MEMORY_LIMIT:
        return "SERVICE_DESCRIPTOR_INVALID_USER_MEMORY_LIMIT";
    default:
        return NULL;
    }
}

static uint32_t service_dependency_index(
    const struct service_descriptor *descriptors,
    uint32_t descriptor_count,
    uint32_t service_id)
{
    uint32_t index;

    for (index = 0U; index < descriptor_count; ++index) {
        if (descriptors[index].id == service_id) {
            return index;
        }
    }
    return descriptor_count;
}

static enum service_dependency_status service_dependency_visit(
    const struct service_descriptor *descriptors,
    uint32_t descriptor_count,
    uint32_t descriptor_index,
    uint8_t *state)
{
    uint32_t dependency_position;

    state[descriptor_index] = 1U;
    for (dependency_position = 0U;
         dependency_position <
             descriptors[descriptor_index].dependency_count;
         ++dependency_position) {
        const uint32_t dependency_index = service_dependency_index(
            descriptors,
            descriptor_count,
            descriptors[descriptor_index].dependencies[dependency_position]);
        enum service_dependency_status status;

        if (dependency_index >= descriptor_count) {
            return SERVICE_DEPENDENCY_UNKNOWN;
        }
        if (state[dependency_index] == 1U) {
            return SERVICE_DEPENDENCY_CYCLE;
        }
        if (state[dependency_index] == 0U) {
            status = service_dependency_visit(
                descriptors,
                descriptor_count,
                dependency_index,
                state);
            if (status != SERVICE_DEPENDENCY_OK) {
                return status;
            }
        }
    }
    state[descriptor_index] = 2U;
    return SERVICE_DEPENDENCY_OK;
}

enum service_dependency_status service_dependency_set_validate(
    const struct service_descriptor *descriptors,
    uint32_t descriptor_count)
{
    uint8_t state[SERVICE_DEPENDENCY_SET_MAX];
    uint32_t index;
    uint32_t previous;
    uint32_t dependency_position;
    enum service_dependency_status status;

    if (descriptor_count > SERVICE_DEPENDENCY_SET_MAX) {
        return SERVICE_DEPENDENCY_BAD_CAPACITY;
    }
    if (descriptor_count > 0U && descriptors == NULL) {
        return SERVICE_DEPENDENCY_NULL;
    }
    if (descriptor_count == 0U) {
        return SERVICE_DEPENDENCY_OK;
    }

    for (index = 0U; index < descriptor_count; ++index) {
        if (service_descriptor_validate(&descriptors[index]) !=
            SERVICE_DESCRIPTOR_OK) {
            return SERVICE_DEPENDENCY_INVALID_DESCRIPTOR;
        }
    }
    for (index = 0U; index < descriptor_count; ++index) {
        for (previous = 0U; previous < index; ++previous) {
            if (descriptors[index].id == descriptors[previous].id) {
                return SERVICE_DEPENDENCY_DUPLICATE_SERVICE;
            }
        }
    }
    for (index = 0U; index < descriptor_count; ++index) {
        for (dependency_position = 0U;
             dependency_position < descriptors[index].dependency_count;
             ++dependency_position) {
            if (service_dependency_index(
                    descriptors,
                    descriptor_count,
                    descriptors[index].dependencies[dependency_position]) >=
                descriptor_count) {
                return SERVICE_DEPENDENCY_UNKNOWN;
            }
        }
    }

    for (index = 0U; index < SERVICE_DEPENDENCY_SET_MAX; ++index) {
        state[index] = 0U;
    }
    for (index = 0U; index < descriptor_count; ++index) {
        if (state[index] == 0U) {
            status = service_dependency_visit(
                descriptors, descriptor_count, index, state);
            if (status != SERVICE_DEPENDENCY_OK) {
                return status;
            }
        }
    }
    return SERVICE_DEPENDENCY_OK;
}

const char *service_dependency_status_string(
    enum service_dependency_status status)
{
    switch (status) {
    case SERVICE_DEPENDENCY_OK:
        return "SERVICE_DEPENDENCY_OK";
    case SERVICE_DEPENDENCY_NULL:
        return "SERVICE_DEPENDENCY_NULL";
    case SERVICE_DEPENDENCY_BAD_CAPACITY:
        return "SERVICE_DEPENDENCY_BAD_CAPACITY";
    case SERVICE_DEPENDENCY_INVALID_DESCRIPTOR:
        return "SERVICE_DEPENDENCY_INVALID_DESCRIPTOR";
    case SERVICE_DEPENDENCY_DUPLICATE_SERVICE:
        return "SERVICE_DEPENDENCY_DUPLICATE_SERVICE";
    case SERVICE_DEPENDENCY_UNKNOWN:
        return "SERVICE_DEPENDENCY_UNKNOWN";
    case SERVICE_DEPENDENCY_CYCLE:
        return "SERVICE_DEPENDENCY_CYCLE";
    default:
        return NULL;
    }
}

static enum service_graph_status service_graph_compute_order(
    const struct service_descriptor *descriptors,
    uint32_t descriptor_count,
    uint32_t *start_ids)
{
    uint8_t indegree[SERVICE_GRAPH_SNAPSHOT_MAX];
    uint8_t emitted[SERVICE_GRAPH_SNAPSHOT_MAX];
    uint32_t emitted_count;
    uint32_t index;

    for (index = 0U; index < SERVICE_GRAPH_SNAPSHOT_MAX; ++index) {
        indegree[index] = 0U;
        emitted[index] = 0U;
    }
    for (index = 0U; index < descriptor_count; ++index) {
        indegree[index] = (uint8_t)descriptors[index].dependency_count;
    }

    for (emitted_count = 0U;
         emitted_count < descriptor_count;
         ++emitted_count) {
        uint32_t chosen = descriptor_count;
        uint32_t candidate;

        for (candidate = 0U; candidate < descriptor_count; ++candidate) {
            if (emitted[candidate] != 0U || indegree[candidate] != 0U) {
                continue;
            }
            if (chosen == descriptor_count ||
                descriptors[candidate].id < descriptors[chosen].id) {
                chosen = candidate;
            }
        }
        if (chosen == descriptor_count) {
            return SERVICE_GRAPH_INVALID_SET;
        }

        start_ids[emitted_count] = descriptors[chosen].id;
        emitted[chosen] = 1U;
        for (candidate = 0U; candidate < descriptor_count; ++candidate) {
            uint32_t dependency_position;

            for (dependency_position = 0U;
                 dependency_position <
                     descriptors[candidate].dependency_count;
                 ++dependency_position) {
                if (descriptors[candidate].dependencies[dependency_position] ==
                    descriptors[chosen].id) {
                    --indegree[candidate];
                }
            }
        }
    }
    return SERVICE_GRAPH_OK;
}

enum service_graph_status service_graph_snapshot_build(
    struct service_graph_snapshot *snapshot,
    const struct service_descriptor *descriptors,
    uint32_t descriptor_count)
{
    struct service_descriptor published_descriptors[
        SERVICE_GRAPH_SNAPSHOT_MAX];
    uint32_t start_ids[SERVICE_GRAPH_SNAPSHOT_MAX];
    uint32_t index;
    enum service_graph_status status;

    if (snapshot == NULL) {
        return SERVICE_GRAPH_NULL;
    }
    if (snapshot->published) {
        return SERVICE_GRAPH_BAD_STATE;
    }
    if (descriptor_count > SERVICE_GRAPH_SNAPSHOT_MAX) {
        return SERVICE_GRAPH_BAD_CAPACITY;
    }
    if (descriptor_count > 0U && descriptors == NULL) {
        return SERVICE_GRAPH_NULL;
    }
    if (service_dependency_set_validate(descriptors, descriptor_count) !=
        SERVICE_DEPENDENCY_OK) {
        return SERVICE_GRAPH_INVALID_SET;
    }

    status = service_graph_compute_order(
        descriptors, descriptor_count, start_ids);
    if (status != SERVICE_GRAPH_OK) {
        return status;
    }
    for (index = 0U; index < descriptor_count; ++index) {
        published_descriptors[index] = descriptors[index];
    }

    memset(snapshot, 0, sizeof(*snapshot));
    for (index = 0U; index < descriptor_count; ++index) {
        snapshot->descriptors[index] = published_descriptors[index];
        snapshot->start_ids[index] = start_ids[index];
    }
    snapshot->version = SERVICE_GRAPH_SNAPSHOT_VERSION;
    snapshot->count = descriptor_count;
    snapshot->published = true;
    return SERVICE_GRAPH_OK;
}

uint32_t service_graph_snapshot_count(
    const struct service_graph_snapshot *snapshot)
{
    if (snapshot == NULL || !snapshot->published) {
        return 0U;
    }
    return snapshot->count;
}

const struct service_descriptor *service_graph_snapshot_descriptor(
    const struct service_graph_snapshot *snapshot,
    uint32_t position)
{
    if (snapshot == NULL || !snapshot->published ||
        position >= snapshot->count) {
        return NULL;
    }
    return &snapshot->descriptors[position];
}

uint32_t service_graph_snapshot_start_id(
    const struct service_graph_snapshot *snapshot,
    uint32_t position)
{
    if (snapshot == NULL || !snapshot->published ||
        position >= snapshot->count) {
        return 0U;
    }
    return snapshot->start_ids[position];
}

bool service_graph_snapshot_published(
    const struct service_graph_snapshot *snapshot)
{
    return snapshot != NULL && snapshot->published;
}

const char *service_graph_status_string(enum service_graph_status status) {
    switch (status) {
    case SERVICE_GRAPH_OK:
        return "SERVICE_GRAPH_OK";
    case SERVICE_GRAPH_NULL:
        return "SERVICE_GRAPH_NULL";
    case SERVICE_GRAPH_BAD_CAPACITY:
        return "SERVICE_GRAPH_BAD_CAPACITY";
    case SERVICE_GRAPH_BAD_STATE:
        return "SERVICE_GRAPH_BAD_STATE";
    case SERVICE_GRAPH_INVALID_SET:
        return "SERVICE_GRAPH_INVALID_SET";
    default:
        return NULL;
    }
}
