#include "virtio_queue_layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_VIRTIO_QUEUE_LAYOUT_HOST_TEST

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

static bool status_string_matches(
    enum virtio_queue_layout_status status,
    const char *string)
{
    switch (status) {
    case VIRTIO_QUEUE_LAYOUT_OK:
        return strcmp(string, "VIRTIO_QUEUE_LAYOUT_OK") == 0;
    case VIRTIO_QUEUE_LAYOUT_NULL:
        return strcmp(string, "VIRTIO_QUEUE_LAYOUT_NULL") == 0;
    case VIRTIO_QUEUE_LAYOUT_UNPUBLISHED:
        return strcmp(string, "VIRTIO_QUEUE_LAYOUT_UNPUBLISHED") == 0;
    case VIRTIO_QUEUE_LAYOUT_INVALID_RESULT_VERSION:
        return strcmp(
            string,
            "VIRTIO_QUEUE_LAYOUT_INVALID_RESULT_VERSION") == 0;
    case VIRTIO_QUEUE_LAYOUT_INVALID_QUEUE_SIZE:
        return strcmp(
            string,
            "VIRTIO_QUEUE_LAYOUT_INVALID_QUEUE_SIZE") == 0;
    case VIRTIO_QUEUE_LAYOUT_INVALID_RANGE:
        return strcmp(
            string,
            "VIRTIO_QUEUE_LAYOUT_INVALID_RANGE") == 0;
    case VIRTIO_QUEUE_LAYOUT_OVERFLOW:
        return strcmp(string, "VIRTIO_QUEUE_LAYOUT_OVERFLOW") == 0;
    case VIRTIO_QUEUE_LAYOUT_UNALIGNED:
        return strcmp(string, "VIRTIO_QUEUE_LAYOUT_UNALIGNED") == 0;
    case VIRTIO_QUEUE_LAYOUT_RANGE_TOO_SMALL:
        return strcmp(
            string,
            "VIRTIO_QUEUE_LAYOUT_RANGE_TOO_SMALL") == 0;
    case VIRTIO_QUEUE_LAYOUT_INVALID_LAYOUT:
        return strcmp(
            string,
            "VIRTIO_QUEUE_LAYOUT_INVALID_LAYOUT") == 0;
    default:
        return false;
    }
}

static void test_status_strings(void) {
    static const enum virtio_queue_layout_status statuses[] = {
        VIRTIO_QUEUE_LAYOUT_OK,
        VIRTIO_QUEUE_LAYOUT_NULL,
        VIRTIO_QUEUE_LAYOUT_UNPUBLISHED,
        VIRTIO_QUEUE_LAYOUT_INVALID_RESULT_VERSION,
        VIRTIO_QUEUE_LAYOUT_INVALID_QUEUE_SIZE,
        VIRTIO_QUEUE_LAYOUT_INVALID_RANGE,
        VIRTIO_QUEUE_LAYOUT_OVERFLOW,
        VIRTIO_QUEUE_LAYOUT_UNALIGNED,
        VIRTIO_QUEUE_LAYOUT_RANGE_TOO_SMALL,
        VIRTIO_QUEUE_LAYOUT_INVALID_LAYOUT,
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        enum virtio_queue_layout_status status = statuses[index];
        const char *string = virtio_queue_layout_status_string(status);

        CHECK(string != NULL &&
                  status_string_matches(status, string),
              "virtio queue layout status string is exact");
    }
    CHECK(virtio_queue_layout_status_string(
              (enum virtio_queue_layout_status)10U) == NULL,
          "virtio queue layout undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio queue layout status handling performs no allocation");
}

static void test_null_and_queue_size(void) {
    struct virtio_queue_layout_policy policy;
    struct virtio_queue_layout_result result;

    policy.queue_size = VIRTIO_QUEUE_SIZE_MIN;
    policy.event_index = false;
    memset(&result, 0, sizeof(result));

    CHECK(virtio_queue_layout_calculate(
              NULL, 0U, 0U, &result) == VIRTIO_QUEUE_LAYOUT_NULL,
          "queue layout calculate rejects null policy");
    CHECK(virtio_queue_layout_calculate(
              &policy, 0U, 0U, NULL) == VIRTIO_QUEUE_LAYOUT_NULL,
          "queue layout calculate rejects null result");
    CHECK(virtio_queue_layout_result_validate(NULL) ==
              VIRTIO_QUEUE_LAYOUT_NULL,
          "queue layout result validation rejects null result");

    policy.queue_size = 0U;
    CHECK(virtio_queue_layout_calculate(
              &policy, 0U, 0x10000U, &result) ==
              VIRTIO_QUEUE_LAYOUT_INVALID_QUEUE_SIZE,
          "queue layout rejects zero queue size");
    policy.queue_size = VIRTIO_QUEUE_SIZE_MAX + 1U;
    CHECK(virtio_queue_layout_calculate(
              &policy, 0U, 0x100000U, &result) ==
              VIRTIO_QUEUE_LAYOUT_INVALID_QUEUE_SIZE,
          "queue layout rejects over-limit queue size");
    policy.queue_size = 3U;
    CHECK(virtio_queue_layout_calculate(
              &policy, 0U, 0x100000U, &result) ==
              VIRTIO_QUEUE_LAYOUT_INVALID_QUEUE_SIZE,
          "queue layout rejects non-power-of-two queue size");
    policy.queue_size = 12U;
    CHECK(virtio_queue_layout_calculate(
              &policy, 0U, 0x100000U, &result) ==
              VIRTIO_QUEUE_LAYOUT_INVALID_QUEUE_SIZE,
          "queue layout rejects composite non-power-of-two queue size");
    policy.queue_size = UINT32_MAX;
    CHECK(virtio_queue_layout_calculate(
              &policy, 0U, 0x100000U, &result) ==
              VIRTIO_QUEUE_LAYOUT_INVALID_QUEUE_SIZE,
          "queue layout rejects maximum 32-bit queue size");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "queue layout null/size checks perform no allocation");
}

static void check_ring(
    const struct virtio_queue_ring_layout *ring,
    uint64_t expected_offset,
    uint64_t expected_size,
    uint32_t expected_alignment,
    const char *name)
{
    char message[96];

    snprintf(message, sizeof(message), "%s offset is exact", name);
    CHECK(ring->offset == expected_offset, message);
    snprintf(message, sizeof(message), "%s size is exact", name);
    CHECK(ring->size == expected_size, message);
    snprintf(message, sizeof(message), "%s alignment is exact", name);
    CHECK(ring->alignment == expected_alignment, message);
}

static void test_exact_layouts(void) {
    struct virtio_queue_layout_policy policy;
    struct virtio_queue_layout_result result;

    policy.queue_size = 16U;
    policy.event_index = false;
    memset(&result, 0, sizeof(result));
    CHECK(virtio_queue_layout_calculate(
              &policy, 0U, UINT64_C(0x100000), &result) ==
              VIRTIO_QUEUE_LAYOUT_OK,
          "queue layout publishes size 16 without event index");
    CHECK(result.published &&
              result.version == VIRTIO_QUEUE_LAYOUT_RESULT_VERSION &&
              result.queue_size == 16U &&
              !result.event_index &&
              result.region_base == 0U &&
              result.region_length == UINT64_C(0x100000),
          "queue layout publishes size-16 result metadata");
    check_ring(&result.descriptor, 0U, 256U, 16U, "size-16 descriptor");
    check_ring(&result.available, 256U, 36U, 2U, "size-16 available");
    check_ring(&result.used, 292U, 132U, 4U, "size-16 used");
    CHECK(result.total_size == 432U,
          "size-16 total size is aligned to 16");

    policy.event_index = true;
    memset(&result, 0, sizeof(result));
    CHECK(virtio_queue_layout_calculate(
              &policy, 0U, UINT64_C(0x100000), &result) ==
              VIRTIO_QUEUE_LAYOUT_OK,
          "queue layout publishes size 16 with event index");
    CHECK(result.event_index,
          "queue layout preserves event-index policy");
    check_ring(&result.available, 256U, 38U, 2U,
               "size-16 event available");
    check_ring(&result.used, 296U, 134U, 4U,
               "size-16 event used");
    CHECK(result.total_size == 432U,
          "size-16 event total size is aligned to 16");

    policy.queue_size = 64U;
    policy.event_index = true;
    memset(&result, 0, sizeof(result));
    CHECK(virtio_queue_layout_calculate(
              &policy, 0U, UINT64_C(0x100000), &result) ==
              VIRTIO_QUEUE_LAYOUT_OK,
          "queue layout publishes size 64 with event index");
    check_ring(&result.descriptor, 0U, 1024U, 16U, "size-64 descriptor");
    check_ring(&result.available, 1024U, 134U, 2U,
               "size-64 available");
    check_ring(&result.used, 1160U, 518U, 4U, "size-64 used");
    CHECK(result.total_size == 1680U,
          "size-64 event total size is exact");

    policy.queue_size = 256U;
    memset(&result, 0, sizeof(result));
    CHECK(virtio_queue_layout_calculate(
              &policy, 0U, UINT64_C(0x100000), &result) ==
              VIRTIO_QUEUE_LAYOUT_OK,
          "queue layout publishes size 256 with event index");
    check_ring(&result.descriptor, 0U, 4096U, 16U,
               "size-256 descriptor");
    check_ring(&result.available, 4096U, 518U, 2U,
               "size-256 available");
    check_ring(&result.used, 4616U, 2054U, 4U, "size-256 used");
    CHECK(result.total_size == 6672U,
          "size-256 event total size is exact");

    policy.queue_size = VIRTIO_QUEUE_SIZE_MAX;
    memset(&result, 0, sizeof(result));
    CHECK(virtio_queue_layout_calculate(
              &policy, 0U, UINT64_C(0x200000), &result) ==
              VIRTIO_QUEUE_LAYOUT_OK,
          "queue layout publishes maximum queue size");
    check_ring(&result.descriptor, 0U, 524288U, 16U,
               "maximum descriptor");
    check_ring(&result.available, 524288U, 65542U, 2U,
               "maximum available");
    check_ring(&result.used, 589832U, 262150U, 4U, "maximum used");
    CHECK(result.total_size == 851984U,
          "maximum queue total size does not overflow");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "queue layout exact calculations perform no allocation");
}

static void test_range(void) {
    struct virtio_queue_layout_policy policy;
    struct virtio_queue_layout_result result;

    policy.queue_size = 16U;
    policy.event_index = false;
    memset(&result, 0, sizeof(result));

    CHECK(virtio_queue_layout_calculate(
              &policy, 0U, 0U, &result) ==
              VIRTIO_QUEUE_LAYOUT_INVALID_RANGE,
          "queue layout rejects zero-length range");

    CHECK(virtio_queue_layout_calculate(
              &policy, UINT64_MAX - 15U, 16U, &result) ==
              VIRTIO_QUEUE_LAYOUT_OVERFLOW,
          "queue layout rejects wrapping address range");

    CHECK(virtio_queue_layout_calculate(
              &policy, 1U, UINT64_C(0x100000), &result) ==
              VIRTIO_QUEUE_LAYOUT_UNALIGNED,
          "queue layout rejects unaligned region base");

    CHECK(virtio_queue_layout_calculate(
              &policy, 0U, 431U, &result) ==
              VIRTIO_QUEUE_LAYOUT_RANGE_TOO_SMALL,
          "queue layout rejects range below total size");

    CHECK(virtio_queue_layout_calculate(
              &policy, 0U, 432U, &result) ==
              VIRTIO_QUEUE_LAYOUT_OK,
          "queue layout accepts exact total-size range");
    CHECK(result.region_length == 432U &&
              result.total_size == 432U,
          "queue layout preserves exact range");

    memset(&result, 0, sizeof(result));
    CHECK(virtio_queue_layout_calculate(
              &policy, UINT64_C(0x1000), UINT64_C(0x200000), &result) ==
              VIRTIO_QUEUE_LAYOUT_OK,
          "queue layout accepts larger aligned range");
    CHECK(result.region_base == UINT64_C(0x1000) &&
              result.region_length == UINT64_C(0x200000) &&
              result.descriptor.offset == 0U &&
              result.available.offset == 256U &&
              result.used.offset == 292U &&
              result.total_size == 432U,
          "queue layout offsets are relative to aligned range");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "queue layout range checks perform no allocation");
}

static void test_no_mutation(void) {
    struct virtio_queue_layout_policy policy;
    struct virtio_queue_layout_result result;
    unsigned char before[sizeof(result)];

    policy.queue_size = 3U;
    policy.event_index = false;
    memset(&result, 0x5A, sizeof(result));
    memcpy(before, &result, sizeof(before));
    CHECK(virtio_queue_layout_calculate(
              &policy, 0U, UINT64_C(0x100000), &result) ==
              VIRTIO_QUEUE_LAYOUT_INVALID_QUEUE_SIZE,
          "invalid queue size is rejected");
    CHECK(memcmp(before, &result, sizeof(before)) == 0,
          "invalid queue size leaves result unchanged");

    policy.queue_size = 16U;
    CHECK(virtio_queue_layout_calculate(
              &policy, 1U, UINT64_C(0x100000), &result) ==
              VIRTIO_QUEUE_LAYOUT_UNALIGNED,
          "invalid range is rejected");
    CHECK(memcmp(before, &result, sizeof(before)) == 0,
          "invalid range leaves result unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "queue layout no-mutation checks perform no allocation");
}

static void test_result_validation(void) {
    struct virtio_queue_layout_policy policy;
    struct virtio_queue_layout_result result;

    policy.queue_size = 16U;
    policy.event_index = false;
    memset(&result, 0, sizeof(result));
    CHECK(virtio_queue_layout_calculate(
              &policy, 0U, 432U, &result) ==
              VIRTIO_QUEUE_LAYOUT_OK,
          "result validation fixture publishes layout");
    CHECK(virtio_queue_layout_result_validate(&result) ==
              VIRTIO_QUEUE_LAYOUT_OK,
          "published layout validates");
    CHECK(virtio_queue_layout_result_published(&result),
          "published layout is published");
    CHECK(!virtio_queue_layout_result_published(NULL),
          "null layout is not published");

    result.published = false;
    CHECK(virtio_queue_layout_result_validate(&result) ==
              VIRTIO_QUEUE_LAYOUT_UNPUBLISHED,
          "unpublished result validation is rejected");
    result.published = true;
    result.version = VIRTIO_QUEUE_LAYOUT_RESULT_VERSION + 1U;
    CHECK(virtio_queue_layout_result_validate(&result) ==
              VIRTIO_QUEUE_LAYOUT_INVALID_RESULT_VERSION,
          "wrong result version is rejected");
    result.version = VIRTIO_QUEUE_LAYOUT_RESULT_VERSION;
    result.queue_size = 0U;
    CHECK(virtio_queue_layout_result_validate(&result) ==
              VIRTIO_QUEUE_LAYOUT_INVALID_QUEUE_SIZE,
          "corrupt queue size is rejected");
    result.queue_size = 16U;
    result.region_length = 0U;
    CHECK(virtio_queue_layout_result_validate(&result) ==
              VIRTIO_QUEUE_LAYOUT_INVALID_RANGE,
          "corrupt zero range is rejected");
    result.region_length = 432U;
    result.region_base = UINT64_MAX - 15U;
    result.region_length = 16U;
    CHECK(virtio_queue_layout_result_validate(&result) ==
              VIRTIO_QUEUE_LAYOUT_OVERFLOW,
          "corrupt overflow range is rejected");
    result.region_base = 1U;
    result.region_length = 432U;
    CHECK(virtio_queue_layout_result_validate(&result) ==
              VIRTIO_QUEUE_LAYOUT_UNALIGNED,
          "corrupt unaligned range is rejected");
    result.region_base = 0U;
    result.region_length = 431U;
    CHECK(virtio_queue_layout_result_validate(&result) ==
              VIRTIO_QUEUE_LAYOUT_RANGE_TOO_SMALL,
          "corrupt small range is rejected");
    result.region_length = 432U;

    result.descriptor.size = 257U;
    CHECK(virtio_queue_layout_result_validate(&result) ==
              VIRTIO_QUEUE_LAYOUT_INVALID_LAYOUT,
          "corrupt descriptor size is rejected");
    result.descriptor.size = 256U;
    result.available.offset = 257U;
    CHECK(virtio_queue_layout_result_validate(&result) ==
              VIRTIO_QUEUE_LAYOUT_INVALID_LAYOUT,
          "corrupt available offset is rejected");
    result.available.offset = 256U;
    result.used.alignment = 8U;
    CHECK(virtio_queue_layout_result_validate(&result) ==
              VIRTIO_QUEUE_LAYOUT_INVALID_LAYOUT,
          "corrupt used alignment is rejected");
    result.used.alignment = VIRTIO_QUEUE_USED_ALIGNMENT;
    result.total_size = 433U;
    CHECK(virtio_queue_layout_result_validate(&result) ==
              VIRTIO_QUEUE_LAYOUT_INVALID_LAYOUT,
          "corrupt total size is rejected");
    result.total_size = 432U;
    result.event_index = true;
    CHECK(virtio_queue_layout_result_validate(&result) ==
              VIRTIO_QUEUE_LAYOUT_INVALID_LAYOUT,
          "corrupt event-index policy is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "queue layout validation performs no allocation");
}

int main(void) {
    test_status_strings();
    test_null_and_queue_size();
    test_exact_layouts();
    test_range();
    test_no_mutation();
    test_result_validation();

    if (failures != 0) {
        fprintf(stderr, "Virtio queue layout host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Virtio queue layout host tests: ALL OK\n");
    return 0;
}

#endif
