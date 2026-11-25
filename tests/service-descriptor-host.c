#include "service.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_SERVICE_DESCRIPTOR_HOST_TEST

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

static struct service_descriptor make_descriptor(uint32_t id,
                                                 uint32_t version,
                                                 bool enabled,
                                                 uint32_t restart_limit,
                                                 uint32_t timeout_ms,
                                                 const char *name)
{
    struct service_descriptor descriptor;
    size_t name_length;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.id = id;
    descriptor.version = version;
    descriptor.enabled = enabled;
    descriptor.restart_limit = restart_limit;
    descriptor.readiness_timeout_ms = timeout_ms;
    if (name != NULL) {
        name_length = strlen(name);
        if (name_length > SERVICE_NAME_MAX) {
            name_length = SERVICE_NAME_MAX;
        }
        memcpy(descriptor.name, name, name_length);
    }
    return descriptor;
}

static void set_dependencies(struct service_descriptor *descriptor,
                             uint32_t dependency_count,
                             const uint8_t *dependencies)
{
    descriptor->dependency_count = dependency_count;
    if (dependencies != NULL && dependency_count > 0U) {
        memcpy(descriptor->dependencies, dependencies, dependency_count);
    }
}

static void test_validation_and_status_strings(void) {
    enum service_descriptor_status status;

    CHECK(service_descriptor_validate(NULL) == SERVICE_DESCRIPTOR_NULL,
          "descriptor validate rejects null descriptor");
    CHECK(service_descriptor_init(NULL, NULL) == SERVICE_DESCRIPTOR_NULL,
          "descriptor init rejects null published output");
    CHECK(service_descriptor_init(NULL,
              &(struct service_descriptor){0}) == SERVICE_DESCRIPTOR_NULL,
          "descriptor init rejects null descriptor");

    for (status = SERVICE_DESCRIPTOR_OK;
         status <= SERVICE_DESCRIPTOR_INVALID_USER_MEMORY_LIMIT; ++status) {
        CHECK(service_descriptor_status_string(status) != NULL &&
                  strcmp(service_descriptor_status_string(status),
                         status == SERVICE_DESCRIPTOR_OK ?
                             "SERVICE_DESCRIPTOR_OK" :
                         status == SERVICE_DESCRIPTOR_NULL ?
                             "SERVICE_DESCRIPTOR_NULL" :
                         status == SERVICE_DESCRIPTOR_INVALID_ID ?
                             "SERVICE_DESCRIPTOR_INVALID_ID" :
                         status == SERVICE_DESCRIPTOR_INVALID_VERSION ?
                             "SERVICE_DESCRIPTOR_INVALID_VERSION" :
                         status == SERVICE_DESCRIPTOR_INVALID_NAME ?
                             "SERVICE_DESCRIPTOR_INVALID_NAME" :
                         status == SERVICE_DESCRIPTOR_INVALID_RESTART_LIMIT ?
                             "SERVICE_DESCRIPTOR_INVALID_RESTART_LIMIT" :
                         status == SERVICE_DESCRIPTOR_INVALID_CPU_LIMIT ?
                             "SERVICE_DESCRIPTOR_INVALID_CPU_LIMIT" :
                         status ==
                             SERVICE_DESCRIPTOR_INVALID_USER_MEMORY_LIMIT ?
                             "SERVICE_DESCRIPTOR_INVALID_USER_MEMORY_LIMIT" :
                         status == SERVICE_DESCRIPTOR_INVALID_DEPENDENCY ?
                             "SERVICE_DESCRIPTOR_INVALID_DEPENDENCY" :
                             "SERVICE_DESCRIPTOR_INVALID_TIMEOUT") == 0,
              "descriptor status string is present for every defined status");
    }
    CHECK(service_descriptor_status_string(
              (enum service_descriptor_status)
                  (SERVICE_DESCRIPTOR_INVALID_USER_MEMORY_LIMIT + 1U)) == NULL,
          "descriptor status string rejects undefined status");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "service descriptor validation performs no allocation");
}

static void check_init_failure(const struct service_descriptor *descriptor,
                               enum service_descriptor_status expected,
                               const char *message)
{
    struct service_descriptor published;
    unsigned char original[SERVICE_NAME_MAX + 1U];
    unsigned char original_dependencies[SERVICE_DEPENDENCY_MAX];

    memset(&published, 0xA5, sizeof(published));
    memcpy(original, published.name, sizeof(original));
    memcpy(original_dependencies, published.dependencies,
           sizeof(original_dependencies));
    CHECK(service_descriptor_init(&published, descriptor) == expected &&
              memcmp(published.name, original, sizeof(original)) == 0 &&
              published.id == 0xA5A5A5A5U &&
              published.version == 0xA5A5A5A5U &&
              published.restart_limit == 0xA5A5A5A5U &&
              published.readiness_timeout_ms == 0xA5A5A5A5U &&
              published.cpu_ticks_limit == UINT64_C(0xA5A5A5A5A5A5A5A5) &&
              published.user_memory_limit_pages ==
                  UINT64_C(0xA5A5A5A5A5A5A5A5) &&
              published.dependency_count == 0xA5A5A5A5U &&
              memcmp(published.dependencies, original_dependencies,
                     sizeof(original_dependencies)) == 0,
          message);
}

static void test_invalid_descriptors_never_publish(void) {
    struct service_descriptor descriptor;

    descriptor = make_descriptor(0U, SERVICE_DESCRIPTOR_VERSION, true,
                                 1U, 1000U, "audit-log");
    check_init_failure(&descriptor, SERVICE_DESCRIPTOR_INVALID_ID,
                       "zero service id is rejected without publication");

    descriptor = make_descriptor(SERVICE_ID_MAX + 1U,
                                 SERVICE_DESCRIPTOR_VERSION, true,
                                 1U, 1000U, "audit-log");
    check_init_failure(&descriptor, SERVICE_DESCRIPTOR_INVALID_ID,
                       "over-limit service id is rejected without publication");

    descriptor = make_descriptor(1U, SERVICE_DESCRIPTOR_VERSION + 1U,
                                 true, 1U, 1000U, "audit-log");
    check_init_failure(&descriptor, SERVICE_DESCRIPTOR_INVALID_VERSION,
                       "unknown descriptor version is rejected");

    descriptor = make_descriptor(1U, SERVICE_DESCRIPTOR_VERSION, true,
                                 1U, 1000U, "");
    check_init_failure(&descriptor, SERVICE_DESCRIPTOR_INVALID_NAME,
                       "empty service name is rejected");

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.id = 1U;
    descriptor.version = SERVICE_DESCRIPTOR_VERSION;
    descriptor.enabled = true;
    descriptor.restart_limit = 1U;
    descriptor.readiness_timeout_ms = 1000U;
    memset(descriptor.name, 'a', SERVICE_NAME_MAX + 1U);
    check_init_failure(&descriptor, SERVICE_DESCRIPTOR_INVALID_NAME,
                       "overlong service name is rejected");

    descriptor = make_descriptor(1U, SERVICE_DESCRIPTOR_VERSION, true,
                                 1U, 1000U, "bad\nname");
    check_init_failure(&descriptor, SERVICE_DESCRIPTOR_INVALID_NAME,
                       "control character in service name is rejected");

    descriptor = make_descriptor(1U, SERVICE_DESCRIPTOR_VERSION, true,
                                 1U, 1000U, " audit");
    check_init_failure(&descriptor, SERVICE_DESCRIPTOR_INVALID_NAME,
                       "leading space in service name is rejected");

    descriptor = make_descriptor(1U, SERVICE_DESCRIPTOR_VERSION, true,
                                 1U, 1000U, "audit ");
    check_init_failure(&descriptor, SERVICE_DESCRIPTOR_INVALID_NAME,
                       "trailing space in service name is rejected");

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.id = 1U;
    descriptor.version = SERVICE_DESCRIPTOR_VERSION;
    descriptor.enabled = true;
    descriptor.restart_limit = 1U;
    descriptor.readiness_timeout_ms = 1000U;
    memcpy(descriptor.name, "audit", 6U);
    descriptor.name[6U] = (char)0xA5;
    check_init_failure(&descriptor, SERVICE_DESCRIPTOR_INVALID_NAME,
                       "non-zero tail after service name is rejected");

    descriptor = make_descriptor(1U, SERVICE_DESCRIPTOR_VERSION, true,
                                 SERVICE_RESTART_LIMIT_MAX + 1U,
                                 1000U, "audit-log");
    check_init_failure(&descriptor, SERVICE_DESCRIPTOR_INVALID_RESTART_LIMIT,
                       "over-limit restart limit is rejected");

    descriptor = make_descriptor(1U, SERVICE_DESCRIPTOR_VERSION, true,
                                 1U, 0U, "audit-log");
    check_init_failure(&descriptor, SERVICE_DESCRIPTOR_INVALID_TIMEOUT,
                       "zero readiness timeout is rejected");

    descriptor = make_descriptor(1U, SERVICE_DESCRIPTOR_VERSION, true,
                                 1U,
                                 SERVICE_READINESS_TIMEOUT_MAX_MS + 1U,
                                 "audit-log");
    check_init_failure(&descriptor, SERVICE_DESCRIPTOR_INVALID_TIMEOUT,
                       "over-limit readiness timeout is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "invalid descriptor handling performs no allocation");
}

static void test_invalid_dependencies_never_publish(void) {
    struct service_descriptor descriptor;
    uint8_t dependencies[SERVICE_DEPENDENCY_MAX];

    descriptor = make_descriptor(1U, SERVICE_DESCRIPTOR_VERSION, true,
                                 1U, 1000U, "audit-log");
    descriptor.dependency_count = SERVICE_DEPENDENCY_MAX + 1U;
    check_init_failure(&descriptor, SERVICE_DESCRIPTOR_INVALID_DEPENDENCY,
                       "over-limit dependency count is rejected");

    memset(dependencies, 0, sizeof(dependencies));
    dependencies[0] = 0U;
    descriptor = make_descriptor(1U, SERVICE_DESCRIPTOR_VERSION, true,
                                 1U, 1000U, "audit-log");
    set_dependencies(&descriptor, 1U, dependencies);
    check_init_failure(&descriptor, SERVICE_DESCRIPTOR_INVALID_DEPENDENCY,
                       "zero dependency id is rejected");

    dependencies[0] = (uint8_t)1U;
    descriptor = make_descriptor(1U, SERVICE_DESCRIPTOR_VERSION, true,
                                 1U, 1000U, "audit-log");
    set_dependencies(&descriptor, 1U, dependencies);
    check_init_failure(&descriptor, SERVICE_DESCRIPTOR_INVALID_DEPENDENCY,
                       "self dependency is rejected");

    dependencies[0] = 2U;
    dependencies[1] = 2U;
    descriptor = make_descriptor(1U, SERVICE_DESCRIPTOR_VERSION, true,
                                 1U, 1000U, "audit-log");
    set_dependencies(&descriptor, 2U, dependencies);
    check_init_failure(&descriptor, SERVICE_DESCRIPTOR_INVALID_DEPENDENCY,
                       "duplicate dependency entries are rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "invalid dependency handling performs no allocation");
}

static void test_invalid_resource_limits_never_publish(void) {
    struct service_descriptor descriptor;

    descriptor = make_descriptor(1U, SERVICE_DESCRIPTOR_VERSION, true,
                                 1U, 1000U, "limited");
    descriptor.cpu_ticks_limit = SERVICE_CPU_TICKS_LIMIT_MAX + 1U;
    check_init_failure(&descriptor, SERVICE_DESCRIPTOR_INVALID_CPU_LIMIT,
                       "over-limit CPU tick limit is rejected");

    descriptor = make_descriptor(1U, SERVICE_DESCRIPTOR_VERSION, true,
                                 1U, 1000U, "limited");
    descriptor.user_memory_limit_pages =
        SERVICE_USER_MEMORY_LIMIT_MAX_PAGES + 1U;
    check_init_failure(
        &descriptor, SERVICE_DESCRIPTOR_INVALID_USER_MEMORY_LIMIT,
        "over-limit user-memory limit is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "invalid resource limit handling performs no allocation");
}

static void test_valid_descriptor_publishes_exact(void) {
    struct service_descriptor descriptor;
    struct service_descriptor published;

    memset(&published, 0, sizeof(published));
    descriptor = make_descriptor(7U, SERVICE_DESCRIPTOR_VERSION, true,
                                 3U, 5000U, "audit-log");
    descriptor.cpu_ticks_limit = UINT64_C(123);
    descriptor.user_memory_limit_pages = UINT64_C(456);
    CHECK(service_descriptor_validate(&descriptor) == SERVICE_DESCRIPTOR_OK,
          "valid service descriptor validates");
    CHECK(service_descriptor_init(&published, &descriptor) ==
              SERVICE_DESCRIPTOR_OK &&
              memcmp(&published, &descriptor, sizeof(descriptor)) == 0,
          "valid service descriptor publishes exact bytes");
    CHECK(published.id == 7U &&
              published.version == SERVICE_DESCRIPTOR_VERSION &&
              published.enabled &&
              published.restart_limit == 3U &&
              published.readiness_timeout_ms == 5000U &&
              published.cpu_ticks_limit == UINT64_C(123) &&
              published.user_memory_limit_pages == UINT64_C(456) &&
              strcmp(published.name, "audit-log") == 0,
          "published service descriptor preserves exact fields");

    descriptor = make_descriptor(SERVICE_ID_MIN, SERVICE_DESCRIPTOR_VERSION,
                                 false, SERVICE_RESTART_LIMIT_MAX,
                                 SERVICE_READINESS_TIMEOUT_MAX_MS, "b");
    descriptor.cpu_ticks_limit = SERVICE_CPU_TICKS_LIMIT_MAX;
    descriptor.user_memory_limit_pages =
        SERVICE_USER_MEMORY_LIMIT_MAX_PAGES;
    CHECK(service_descriptor_init(&published, &descriptor) ==
              SERVICE_DESCRIPTOR_OK &&
              published.id == SERVICE_ID_MIN &&
              !published.enabled &&
              published.restart_limit == SERVICE_RESTART_LIMIT_MAX &&
              published.readiness_timeout_ms ==
                  SERVICE_READINESS_TIMEOUT_MAX_MS &&
              published.cpu_ticks_limit == SERVICE_CPU_TICKS_LIMIT_MAX &&
              published.user_memory_limit_pages ==
                  SERVICE_USER_MEMORY_LIMIT_MAX_PAGES &&
              strcmp(published.name, "b") == 0,
          "boundary values publish exactly");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "valid descriptor publication performs no allocation");
}

int main(void) {
    test_validation_and_status_strings();
    test_invalid_descriptors_never_publish();
    test_invalid_dependencies_never_publish();
    test_invalid_resource_limits_never_publish();
    test_valid_descriptor_publishes_exact();

    if (failures != 0) {
        fprintf(stderr, "Service descriptor host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Service descriptor host tests: ALL OK");
    return 0;
}

#endif
