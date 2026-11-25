#include "service.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_SERVICE_GRAPH_HOST_TEST

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

void *kmalloc(size_t size) {
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr) {
    (void)ptr;
    ++free_attempts;
    return false;
}

static struct service_descriptor make_descriptor(
    uint32_t id,
    const uint8_t *dependencies,
    uint32_t dependency_count)
{
    struct service_descriptor descriptor;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.id = id;
    descriptor.version = SERVICE_DESCRIPTOR_VERSION;
    descriptor.enabled = true;
    descriptor.restart_limit = 0U;
    descriptor.readiness_timeout_ms = 1000U;
    memcpy(descriptor.name, "svc", 4U);
    descriptor.dependency_count = dependency_count;
    if (dependencies != NULL && dependency_count > 0U) {
        memcpy(descriptor.dependencies, dependencies, dependency_count);
    }
    return descriptor;
}

static void test_status_strings(void) {
    static const enum service_graph_status statuses[] = {
        SERVICE_GRAPH_OK,
        SERVICE_GRAPH_NULL,
        SERVICE_GRAPH_BAD_CAPACITY,
        SERVICE_GRAPH_BAD_STATE,
        SERVICE_GRAPH_INVALID_SET,
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        enum service_graph_status status = statuses[index];
        const char *string = service_graph_status_string(status);

        CHECK(string != NULL &&
                  (status == SERVICE_GRAPH_OK ?
                       strcmp(string, "SERVICE_GRAPH_OK") == 0 :
                   status == SERVICE_GRAPH_NULL ?
                       strcmp(string, "SERVICE_GRAPH_NULL") == 0 :
                   status == SERVICE_GRAPH_BAD_CAPACITY ?
                       strcmp(string, "SERVICE_GRAPH_BAD_CAPACITY") == 0 :
                   status == SERVICE_GRAPH_BAD_STATE ?
                       strcmp(string, "SERVICE_GRAPH_BAD_STATE") == 0 :
                       strcmp(string, "SERVICE_GRAPH_INVALID_SET") == 0),
              "service graph status string is exact");
    }
    CHECK(service_graph_status_string(
              (enum service_graph_status)5U) == NULL,
          "service graph undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "service graph status handling performs no allocation");
}

static void test_input_and_query_validation(void) {
    struct service_graph_snapshot snapshot;
    unsigned char before[sizeof(snapshot)];
    struct service_descriptor descriptors[1];

    memset(&snapshot, 0, sizeof(snapshot));
    descriptors[0] = make_descriptor(1U, NULL, 0U);

    CHECK(service_graph_snapshot_build(NULL, descriptors, 1U) ==
              SERVICE_GRAPH_NULL,
          "graph build rejects null snapshot");
    CHECK(service_graph_snapshot_build(&snapshot, NULL, 1U) ==
              SERVICE_GRAPH_NULL,
          "graph build rejects null descriptor array with count");
    CHECK(service_graph_snapshot_build(
              &snapshot, descriptors, SERVICE_GRAPH_SNAPSHOT_MAX + 1U) ==
              SERVICE_GRAPH_BAD_CAPACITY,
          "graph build rejects over-limit capacity");
    CHECK(!service_graph_snapshot_published(&snapshot),
          "failed graph build leaves snapshot unpublished");
    CHECK(service_graph_snapshot_count(&snapshot) == 0U,
          "failed graph build leaves count zero");
    CHECK(service_graph_snapshot_count(NULL) == 0U,
          "graph count rejects null snapshot");
    CHECK(service_graph_snapshot_descriptor(NULL, 0U) == NULL,
          "graph descriptor rejects null snapshot");
    CHECK(service_graph_snapshot_start_id(NULL, 0U) == 0U,
          "graph start id rejects null snapshot");
    CHECK(!service_graph_snapshot_published(NULL),
          "graph published rejects null snapshot");

    memset(&snapshot, 0xA5, sizeof(snapshot));
    snapshot.published = false;
    memcpy(before, &snapshot, sizeof(before));
    CHECK(service_graph_snapshot_build(
              &snapshot, descriptors, SERVICE_GRAPH_SNAPSHOT_MAX + 1U) ==
              SERVICE_GRAPH_BAD_CAPACITY,
          "graph over-capacity build returns bad capacity");
    CHECK(memcmp(&snapshot, before, sizeof(before)) == 0,
          "over-capacity failure publishes no partial snapshot");

    memset(&snapshot, 0xA5, sizeof(snapshot));
    snapshot.published = false;
    memcpy(before, &snapshot, sizeof(before));
    CHECK(service_graph_snapshot_build(&snapshot, NULL, 1U) ==
              SERVICE_GRAPH_NULL,
          "graph null descriptor build returns null");
    CHECK(memcmp(&snapshot, before, sizeof(before)) == 0,
          "null descriptor failure publishes no partial snapshot");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "graph input and query handling performs no allocation");
}

static void test_failed_construction_keeps_bytes_unchanged(void) {
    struct service_graph_snapshot snapshot;
    unsigned char before[sizeof(snapshot)];
    struct service_descriptor descriptors[2];
    uint8_t dependency[1] = {2U};

    descriptors[0] = make_descriptor(1U, dependency, 1U);
    descriptors[1] = make_descriptor(1U, NULL, 0U);
    memset(&snapshot, 0xA5, sizeof(snapshot));
    snapshot.published = false;
    memcpy(before, &snapshot, sizeof(before));
    CHECK(service_graph_snapshot_build(&snapshot, descriptors, 2U) ==
              SERVICE_GRAPH_INVALID_SET,
          "graph build rejects duplicate service ids");
    CHECK(memcmp(&snapshot, before, sizeof(before)) == 0,
          "duplicate service id failure publishes no partial snapshot");

    descriptors[0] = make_descriptor(1U, dependency, 1U);
    memset(&snapshot, 0xA5, sizeof(snapshot));
    snapshot.published = false;
    memcpy(before, &snapshot, sizeof(before));
    CHECK(service_graph_snapshot_build(&snapshot, descriptors, 1U) ==
              SERVICE_GRAPH_INVALID_SET,
          "graph build rejects unknown dependency id");
    CHECK(memcmp(&snapshot, before, sizeof(before)) == 0,
          "unknown dependency failure publishes no partial snapshot");

    descriptors[0] = make_descriptor(1U, dependency, 1U);
    descriptors[1] = make_descriptor(2U, (uint8_t[]){1U}, 1U);
    memset(&snapshot, 0xA5, sizeof(snapshot));
    snapshot.published = false;
    memcpy(before, &snapshot, sizeof(before));
    CHECK(service_graph_snapshot_build(&snapshot, descriptors, 2U) ==
              SERVICE_GRAPH_INVALID_SET,
          "graph build rejects dependency cycle");
    CHECK(memcmp(&snapshot, before, sizeof(before)) == 0,
          "cycle failure publishes no partial snapshot");

    descriptors[0] = make_descriptor(1U, NULL, 0U);
    descriptors[0].version = SERVICE_DESCRIPTOR_VERSION + 1U;
    memset(&snapshot, 0xA5, sizeof(snapshot));
    snapshot.published = false;
    memcpy(before, &snapshot, sizeof(before));
    CHECK(service_graph_snapshot_build(&snapshot, descriptors, 1U) ==
              SERVICE_GRAPH_INVALID_SET,
          "graph build rejects invalid descriptor");
    CHECK(memcmp(&snapshot, before, sizeof(before)) == 0,
          "invalid descriptor failure publishes no partial snapshot");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "failed graph construction performs no allocation");
}

static void test_empty_snapshot(void) {
    struct service_graph_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(service_graph_snapshot_build(&snapshot, NULL, 0U) ==
              SERVICE_GRAPH_OK,
          "graph build accepts empty descriptor set");
    CHECK(service_graph_snapshot_published(&snapshot),
          "empty graph build publishes snapshot");
    CHECK(snapshot.version == SERVICE_GRAPH_SNAPSHOT_VERSION,
          "empty graph snapshot has exact version");
    CHECK(service_graph_snapshot_count(&snapshot) == 0U,
          "empty graph snapshot has zero count");
    CHECK(service_graph_snapshot_build(&snapshot, NULL, 0U) ==
              SERVICE_GRAPH_BAD_STATE,
          "published graph snapshot rejects rebuild");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "empty graph snapshot performs no allocation");
}

static void test_independent_and_diamond_order(void) {
    struct service_graph_snapshot snapshot;
    struct service_descriptor descriptors[4];

    descriptors[0] = make_descriptor(5U, NULL, 0U);
    descriptors[1] = make_descriptor(3U, NULL, 0U);
    descriptors[2] = make_descriptor(1U, NULL, 0U);
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(service_graph_snapshot_build(&snapshot, descriptors, 3U) ==
              SERVICE_GRAPH_OK,
          "independent service graph builds");
    CHECK(service_graph_snapshot_start_id(&snapshot, 0U) == 1U &&
              service_graph_snapshot_start_id(&snapshot, 1U) == 3U &&
              service_graph_snapshot_start_id(&snapshot, 2U) == 5U,
          "independent services order by ascending id");

    descriptors[0] = make_descriptor(1U, NULL, 0U);
    descriptors[1] = make_descriptor(2U, (uint8_t[]){1U}, 1U);
    descriptors[2] = make_descriptor(3U, (uint8_t[]){1U}, 1U);
    descriptors[3] = make_descriptor(4U, (uint8_t[]){2U, 3U}, 2U);
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(service_graph_snapshot_build(&snapshot, descriptors, 4U) ==
              SERVICE_GRAPH_OK,
          "diamond dependency graph builds");
    CHECK(service_graph_snapshot_start_id(&snapshot, 0U) == 1U &&
              service_graph_snapshot_start_id(&snapshot, 1U) == 2U &&
              service_graph_snapshot_start_id(&snapshot, 2U) == 3U &&
              service_graph_snapshot_start_id(&snapshot, 3U) == 4U,
          "diamond graph publishes dependency-first ascending order");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "ordered graph construction performs no allocation");
}

static void test_input_order_independence_and_immutability(void) {
    struct service_graph_snapshot first;
    struct service_graph_snapshot second;
    unsigned char first_before[sizeof(first)];
    struct service_descriptor forward[4];
    struct service_descriptor reverse[4];
    const struct service_descriptor *published;

    forward[0] = make_descriptor(1U, NULL, 0U);
    forward[1] = make_descriptor(2U, (uint8_t[]){1U}, 1U);
    forward[2] = make_descriptor(3U, (uint8_t[]){2U}, 1U);
    forward[3] = make_descriptor(4U, (uint8_t[]){1U}, 1U);
    reverse[0] = forward[3];
    reverse[1] = forward[2];
    reverse[2] = forward[1];
    reverse[3] = forward[0];

    memset(&first, 0, sizeof(first));
    CHECK(service_graph_snapshot_build(&first, forward, 4U) ==
              SERVICE_GRAPH_OK,
          "forward descriptor order builds");
    memset(&second, 0, sizeof(second));
    CHECK(service_graph_snapshot_build(&second, reverse, 4U) ==
              SERVICE_GRAPH_OK,
          "reverse descriptor order builds");
    CHECK(service_graph_snapshot_start_id(&first, 0U) == 1U &&
              service_graph_snapshot_start_id(&first, 1U) == 2U &&
              service_graph_snapshot_start_id(&first, 2U) == 3U &&
              service_graph_snapshot_start_id(&first, 3U) == 4U &&
              service_graph_snapshot_start_id(&second, 0U) == 1U &&
              service_graph_snapshot_start_id(&second, 1U) == 2U &&
              service_graph_snapshot_start_id(&second, 2U) == 3U &&
              service_graph_snapshot_start_id(&second, 3U) == 4U,
          "start order is independent of descriptor array order");

    published = service_graph_snapshot_descriptor(&first, 0U);
    CHECK(published != NULL && published->id == 1U &&
              service_graph_snapshot_descriptor(&first, 4U) == NULL &&
              service_graph_snapshot_start_id(&first, 4U) == 0U,
          "graph queries validate published range");
    forward[0].id = 9U;
    forward[0].restart_limit = 99U;
    forward[0].name[0] = 'X';
    published = service_graph_snapshot_descriptor(&first, 0U);
    CHECK(published != NULL && published->id == 1U &&
              published->restart_limit == 0U && published->name[0] == 's',
          "published snapshot is immutable after source mutation");

    memcpy(first_before, &first, sizeof(first_before));
    CHECK(service_graph_snapshot_build(&first, forward, 4U) ==
              SERVICE_GRAPH_BAD_STATE,
          "published graph snapshot rejects rebuild");
    CHECK(memcmp(&first, first_before, sizeof(first_before)) == 0,
          "rejected rebuild preserves published snapshot bytes");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "immutable graph handling performs no allocation");
}

static void test_maximum_acyclic_chain(void) {
    struct service_graph_snapshot snapshot;
    struct service_descriptor descriptors[SERVICE_GRAPH_SNAPSHOT_MAX];
    uint32_t index;

    for (index = 0U; index < SERVICE_GRAPH_SNAPSHOT_MAX; ++index) {
        uint8_t dependency = (uint8_t)(index + 2U);

        descriptors[index] = make_descriptor(
            index + 1U,
            index + 1U < SERVICE_GRAPH_SNAPSHOT_MAX ? &dependency : NULL,
            index + 1U < SERVICE_GRAPH_SNAPSHOT_MAX ? 1U : 0U);
    }
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(service_graph_snapshot_build(
              &snapshot, descriptors, SERVICE_GRAPH_SNAPSHOT_MAX) ==
              SERVICE_GRAPH_OK,
          "maximum-size acyclic graph builds");
    CHECK(service_graph_snapshot_count(&snapshot) ==
              SERVICE_GRAPH_SNAPSHOT_MAX,
          "maximum-size graph publishes exact count");
    for (index = 0U; index < SERVICE_GRAPH_SNAPSHOT_MAX; ++index) {
        uint32_t expected = SERVICE_GRAPH_SNAPSHOT_MAX - index;

        CHECK(service_graph_snapshot_start_id(&snapshot, index) == expected,
              "maximum-size chain publishes dependency-first order");
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "maximum graph construction performs no allocation");
}

int main(void) {
    test_status_strings();
    test_input_and_query_validation();
    test_failed_construction_keeps_bytes_unchanged();
    test_empty_snapshot();
    test_independent_and_diamond_order();
    test_input_order_independence_and_immutability();
    test_maximum_acyclic_chain();

    if (failures != 0) {
        fprintf(stderr, "Service graph host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Service graph host tests: ALL OK");
    return 0;
}

#endif
