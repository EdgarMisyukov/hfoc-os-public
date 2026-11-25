#include "service.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_SERVICE_DEPENDENCY_HOST_TEST

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
    static const enum service_dependency_status statuses[] = {
        SERVICE_DEPENDENCY_OK,
        SERVICE_DEPENDENCY_NULL,
        SERVICE_DEPENDENCY_BAD_CAPACITY,
        SERVICE_DEPENDENCY_INVALID_DESCRIPTOR,
        SERVICE_DEPENDENCY_DUPLICATE_SERVICE,
        SERVICE_DEPENDENCY_UNKNOWN,
        SERVICE_DEPENDENCY_CYCLE,
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        enum service_dependency_status status = statuses[index];
        const char *string = service_dependency_status_string(status);

        CHECK(string != NULL &&
                  (status == SERVICE_DEPENDENCY_OK ?
                       strcmp(string, "SERVICE_DEPENDENCY_OK") == 0 :
                   status == SERVICE_DEPENDENCY_NULL ?
                       strcmp(string, "SERVICE_DEPENDENCY_NULL") == 0 :
                   status == SERVICE_DEPENDENCY_BAD_CAPACITY ?
                       strcmp(string, "SERVICE_DEPENDENCY_BAD_CAPACITY") == 0 :
                   status == SERVICE_DEPENDENCY_INVALID_DESCRIPTOR ?
                       strcmp(string,
                              "SERVICE_DEPENDENCY_INVALID_DESCRIPTOR") == 0 :
                   status == SERVICE_DEPENDENCY_DUPLICATE_SERVICE ?
                       strcmp(string,
                              "SERVICE_DEPENDENCY_DUPLICATE_SERVICE") == 0 :
                   status == SERVICE_DEPENDENCY_UNKNOWN ?
                       strcmp(string, "SERVICE_DEPENDENCY_UNKNOWN") == 0 :
                       strcmp(string, "SERVICE_DEPENDENCY_CYCLE") == 0),
              "service dependency status string is exact");
    }
    CHECK(service_dependency_status_string(
              (enum service_dependency_status)7U) == NULL,
          "service dependency undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "service dependency status handling performs no allocation");
}

static void test_input_and_capacity(void) {
    struct service_descriptor descriptor;

    descriptor = make_descriptor(1U, NULL, 0U);
    CHECK(service_dependency_set_validate(NULL, 0U) ==
              SERVICE_DEPENDENCY_OK,
          "empty dependency set is valid");
    CHECK(service_dependency_set_validate(NULL, 1U) ==
              SERVICE_DEPENDENCY_NULL,
          "non-empty dependency set rejects null descriptor array");
    CHECK(service_dependency_set_validate(
              NULL, SERVICE_DEPENDENCY_SET_MAX + 1U) ==
              SERVICE_DEPENDENCY_BAD_CAPACITY,
          "dependency set rejects over-limit capacity");
    CHECK(service_dependency_set_validate(
              &descriptor, SERVICE_DEPENDENCY_SET_MAX + 1U) ==
              SERVICE_DEPENDENCY_BAD_CAPACITY,
          "dependency set rejects over-limit count before descriptor scan");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "dependency set input handling performs no allocation");
}

static void test_malformed_descriptors_are_rejected(void) {
    struct service_descriptor descriptors[1];
    uint8_t dependencies[SERVICE_DEPENDENCY_MAX];

    dependencies[0] = 1U;
    descriptors[0] = make_descriptor(1U, dependencies, 1U);
    CHECK(service_dependency_set_validate(descriptors, 1U) ==
              SERVICE_DEPENDENCY_INVALID_DESCRIPTOR,
          "dependency set rejects self dependency as invalid descriptor");

    dependencies[0] = 2U;
    dependencies[1] = 2U;
    descriptors[0] = make_descriptor(1U, dependencies, 2U);
    CHECK(service_dependency_set_validate(descriptors, 1U) ==
              SERVICE_DEPENDENCY_INVALID_DESCRIPTOR,
          "dependency set rejects duplicate dependency entries");

    descriptors[0] = make_descriptor(1U, NULL, 0U);
    descriptors[0].version = SERVICE_DESCRIPTOR_VERSION + 1U;
    CHECK(service_dependency_set_validate(descriptors, 1U) ==
              SERVICE_DEPENDENCY_INVALID_DESCRIPTOR,
          "dependency set rejects invalid descriptor version");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "malformed descriptor handling performs no allocation");
}

static void test_duplicate_and_unknown(void) {
    struct service_descriptor descriptors[2];
    uint8_t dependency[1] = {2U};

    descriptors[0] = make_descriptor(1U, dependency, 1U);
    descriptors[1] = make_descriptor(1U, NULL, 0U);
    CHECK(service_dependency_set_validate(descriptors, 2U) ==
              SERVICE_DEPENDENCY_DUPLICATE_SERVICE,
          "dependency set rejects duplicate service ids");

    descriptors[0] = make_descriptor(1U, dependency, 1U);
    CHECK(service_dependency_set_validate(descriptors, 1U) ==
              SERVICE_DEPENDENCY_UNKNOWN,
          "dependency set rejects unknown dependency id");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "duplicate and unknown handling performs no allocation");
}

static void test_cycles(void) {
    struct service_descriptor descriptors[SERVICE_DEPENDENCY_SET_MAX];
    uint8_t dependency_a[1] = {2U};
    uint8_t dependency_b[1] = {1U};
    uint8_t dependency_c[1] = {3U};
    uint8_t dependency_d[1] = {4U};
    uint8_t dependency_e[1] = {4U};

    descriptors[0] = make_descriptor(1U, dependency_a, 1U);
    descriptors[1] = make_descriptor(2U, dependency_b, 1U);
    CHECK(service_dependency_set_validate(descriptors, 2U) ==
              SERVICE_DEPENDENCY_CYCLE,
          "dependency set rejects two-service cycle");

    descriptors[0] = make_descriptor(1U, dependency_a, 1U);
    descriptors[1] = make_descriptor(2U, dependency_c, 1U);
    descriptors[2] = make_descriptor(3U, dependency_b, 1U);
    CHECK(service_dependency_set_validate(descriptors, 3U) ==
              SERVICE_DEPENDENCY_CYCLE,
          "dependency set rejects three-service cycle");

    descriptors[0] = make_descriptor(1U, dependency_a, 1U);
    descriptors[1] = make_descriptor(2U, dependency_d, 1U);
    descriptors[2] = make_descriptor(3U, dependency_e, 1U);
    descriptors[3] = make_descriptor(4U, dependency_b, 1U);
    CHECK(service_dependency_set_validate(descriptors, 4U) ==
              SERVICE_DEPENDENCY_CYCLE,
          "dependency set rejects indirect four-service cycle");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "cycle handling performs no allocation");
}

static void test_valid_sets(void) {
    struct service_descriptor descriptors[SERVICE_DEPENDENCY_SET_MAX];
    uint8_t dependency[1] = {2U};
    uint32_t index;

    descriptors[0] = make_descriptor(1U, NULL, 0U);
    CHECK(service_dependency_set_validate(descriptors, 1U) ==
              SERVICE_DEPENDENCY_OK,
          "single service with no dependencies is valid");

    descriptors[0] = make_descriptor(1U, dependency, 1U);
    descriptors[1] = make_descriptor(2U, NULL, 0U);
    CHECK(service_dependency_set_validate(descriptors, 2U) ==
              SERVICE_DEPENDENCY_OK,
          "acyclic dependency chain is valid");

    for (index = 0U; index < SERVICE_DEPENDENCY_SET_MAX; ++index) {
        uint8_t next = (uint8_t)(index + 2U);

        descriptors[index] = make_descriptor(
            (uint32_t)(index + 1U),
            index + 1U < SERVICE_DEPENDENCY_SET_MAX ? &next : NULL,
            index + 1U < SERVICE_DEPENDENCY_SET_MAX ? 1U : 0U);
    }
    CHECK(service_dependency_set_validate(
              descriptors, SERVICE_DEPENDENCY_SET_MAX) ==
              SERVICE_DEPENDENCY_OK,
          "maximum-size acyclic dependency set is valid");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "valid dependency set handling performs no allocation");
}

int main(void) {
    test_status_strings();
    test_input_and_capacity();
    test_malformed_descriptors_are_rejected();
    test_duplicate_and_unknown();
    test_cycles();
    test_valid_sets();

    if (failures != 0) {
        fprintf(stderr, "Service dependency host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Service dependency host tests: ALL OK");
    return 0;
}

#endif
