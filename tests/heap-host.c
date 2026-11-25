#include "heap.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

extern void heap_test_reset(void);

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        printf("FAIL: %s\n", message); \
        ++failures; \
    } \
} while (0)

static bool ranges_overlap(const void *left_begin, size_t left_size,
                           const void *right_begin, size_t right_size) {
    const uintptr_t left_start = (uintptr_t)left_begin;
    const uintptr_t right_start = (uintptr_t)right_begin;
    return left_start < right_start + right_size &&
           right_start < left_start + left_size;
}

static void test_zero_alignment_and_reporting(void) {
    printf("zero, alignment, reporting\n");
    heap_test_reset();
    CHECK(!heap_ready(), "heap is not ready before init");
    CHECK(heap_capacity_bytes() == 0U, "capacity is zero before init");
    CHECK(heap_used_bytes() == 0U, "used bytes are zero before init");
    CHECK(heap_init(0U), "host heap initialises");
    CHECK(heap_ready(), "heap is ready after init");
    CHECK(heap_capacity_bytes() == HFOC_HEAP_SIZE,
          "capacity matches fixed arena size");
    CHECK(heap_used_bytes() == 0U, "used bytes start at zero");
    CHECK(kmalloc(0U) == NULL, "zero-size allocation returns NULL");

    void *small = kmalloc(1U);
    void *large = kmalloc(64U);
    CHECK(small != NULL, "small allocation succeeds");
    CHECK(large != NULL, "64-byte allocation succeeds");
    CHECK(((uintptr_t)small & (HFOC_HEAP_ALIGNMENT - 1U)) == 0U,
          "small pointer is 16-byte aligned");
    CHECK(((uintptr_t)large & (HFOC_HEAP_ALIGNMENT - 1U)) == 0U,
          "large pointer is 16-byte aligned");
    CHECK(heap_used_bytes() >= 32U + 64U,
          "used bytes grow with payload allocations");
    CHECK(kfree(small), "small block frees");
    CHECK(kfree(large), "large block frees");
    CHECK(heap_used_bytes() == 0U, "used bytes return to zero after frees");
}

static void test_split_reuse_and_no_overlap(void) {
    printf("split, reuse, no overlap\n");
    heap_test_reset();
    CHECK(heap_init(0U), "host heap initialises");

    void *first = kmalloc(64U);
    void *second = kmalloc(HFOC_HEAP_SIZE / 2U);
    CHECK(first != NULL, "first allocation splits the arena");
    CHECK(second != NULL, "second allocation fits after the split");
    CHECK(!ranges_overlap(first, 64U, second, HFOC_HEAP_SIZE / 2U),
          "split blocks do not overlap");

    CHECK(kfree(first), "first block frees");
    void *reused = kmalloc(64U);
    CHECK(reused == first, "freed first-fit block is reused");
    CHECK(kfree(reused), "reused block frees");
    CHECK(kfree(second), "second block frees");
}

static void test_coalescing_adjacent_free_blocks(void) {
    printf("coalescing adjacent free blocks\n");
    heap_test_reset();
    CHECK(heap_init(0U), "host heap initialises");

    void *left = kmalloc(64U);
    void *middle = kmalloc(64U);
    void *right = kmalloc(64U);
    CHECK(left != NULL && middle != NULL && right != NULL,
          "three adjacent allocations succeed");
    CHECK(kfree(middle), "middle block frees");
    CHECK(kfree(right), "right block frees");

    void *merged = kmalloc(128U);
    CHECK(merged != NULL,
          "adjacent free blocks coalesce into a large-enough block");
    CHECK(kfree(merged), "merged block frees");
    CHECK(kfree(left), "left block frees");
}

static void test_exhaustion_returns_null(void) {
    printf("allocation exhaustion\n");
    heap_test_reset();
    CHECK(heap_init(0U), "host heap initialises");

    const size_t chunk = HFOC_HEAP_SIZE / 4U;
    unsigned int count = 0U;
    void *blocks[8];
    while (count < 8U) {
        void *block = kmalloc(chunk);
        if (block == NULL) {
            break;
        }
        blocks[count] = block;
        ++count;
    }
    CHECK(count == 3U, "three quarter-heap allocations fit");
    unsigned int small_count = 0U;
    while (small_count < 100000U) {
        if (kmalloc(16U) == NULL) {
            break;
        }
        ++small_count;
    }
    CHECK(kmalloc(1U) == NULL, "exhaustion returns NULL");
    for (unsigned int index = 0U; index < count; ++index) {
        CHECK(kfree(blocks[index]), "exhausted block frees");
    }
}

static void test_invalid_pointers_and_double_free(void) {
    printf("invalid pointers and double free\n");
    heap_test_reset();
    CHECK(heap_init(0U), "host heap initialises");

    void *block = kmalloc(32U);
    CHECK(block != NULL, "allocation succeeds");
    CHECK(kfree(block), "valid free succeeds");
    CHECK(!kfree(block), "double free is rejected");
    CHECK(!kfree(NULL), "NULL free is rejected");
    CHECK(!kfree((void *)(uintptr_t)1U), "outside-arena free is rejected");
    CHECK(!kfree((unsigned char *)block + 1U),
          "misaligned interior free is rejected");
}

int main(void) {
    printf("HEAP host test\n");
    test_zero_alignment_and_reporting();
    test_split_reuse_and_no_overlap();
    test_coalescing_adjacent_free_blocks();
    test_exhaustion_returns_null();
    test_invalid_pointers_and_double_free();

    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    puts("HEAP host test: OK");
    return 0;
}
