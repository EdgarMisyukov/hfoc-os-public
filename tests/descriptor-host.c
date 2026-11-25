#include "descriptor.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_DESCRIPTOR_HOST_TEST

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static void test_init_and_validation(void) {
    struct descriptor_table table;
    uint32_t descriptor = 0U;

    memset(&table, 0, sizeof(table));
    CHECK(descriptor_table_init(NULL) == DESCRIPTOR_NULL,
          "descriptor init with null table returns NULL");
    CHECK(descriptor_table_init(&table) == DESCRIPTOR_OK,
          "unpublished descriptor table initializes");
    CHECK(descriptor_table_is_published(&table) &&
              descriptor_table_open_count(&table) == 0U,
          "initialized table is published and empty");
    CHECK(descriptor_table_init(&table) == DESCRIPTOR_BAD_STATE,
          "double descriptor init returns BAD_STATE");
    CHECK(descriptor_table_release(&table, 0U) ==
              DESCRIPTOR_BAD_DESCRIPTOR,
          "release of unused descriptor returns BAD_DESCRIPTOR");
    CHECK(descriptor_table_release(&table, PROCESS_DESCRIPTOR_CAPACITY) ==
              DESCRIPTOR_BAD_DESCRIPTOR,
          "release beyond capacity returns BAD_DESCRIPTOR");
    CHECK(descriptor_table_reserve(NULL, &descriptor) == DESCRIPTOR_NULL,
          "descriptor reserve with null table returns NULL");
    CHECK(descriptor_table_reserve(&table, NULL) == DESCRIPTOR_NULL,
          "descriptor reserve with null output returns NULL");
    CHECK(descriptor_table_destroy(&table) == DESCRIPTOR_OK &&
              !descriptor_table_is_published(&table) &&
              descriptor_table_open_count(&table) == 0U,
          "descriptor table destroys and clears");
    CHECK(descriptor_table_destroy(&table) == DESCRIPTOR_BAD_STATE,
          "double descriptor destroy returns BAD_STATE");
}

static void test_reserve_release_and_reuse(void) {
    struct descriptor_table table;
    uint32_t descriptor = 0U;
    uint32_t index;

    memset(&table, 0, sizeof(table));
    CHECK(descriptor_table_init(&table) == DESCRIPTOR_OK,
          "reuse fixture initializes descriptor table");
    for (index = 0U; index < 16U; ++index) {
        CHECK(descriptor_table_reserve(&table, &descriptor) ==
                  DESCRIPTOR_OK && descriptor == index,
              "descriptor reserve returns lowest free descriptor");
    }
    CHECK(descriptor_table_open_count(&table) == 16U,
          "sixteen reservations update open count");

    CHECK(descriptor_table_release(&table, 5U) == DESCRIPTOR_OK &&
              descriptor_table_open_count(&table) == 15U,
          "descriptor release clears the exact slot");
    CHECK(descriptor_table_reserve(&table, &descriptor) ==
              DESCRIPTOR_OK && descriptor == 5U,
          "released descriptor is immediately reusable");
    CHECK(descriptor_table_open_count(&table) == 16U,
          "reused descriptor restores open count");
}

static void test_exhaustion_and_teardown(void) {
    struct descriptor_table table;
    uint32_t descriptor = 0U;
    uint32_t index;

    memset(&table, 0, sizeof(table));
    CHECK(descriptor_table_init(&table) == DESCRIPTOR_OK,
          "exhaustion fixture initializes descriptor table");
    for (index = 0U; index < PROCESS_DESCRIPTOR_CAPACITY; ++index) {
        CHECK(descriptor_table_reserve(&table, &descriptor) ==
                  DESCRIPTOR_OK && descriptor == index,
              "descriptor exhaustion reserves exact slot");
    }
    CHECK(descriptor_table_reserve(&table, &descriptor) == DESCRIPTOR_FULL,
          "descriptor exhaustion returns FULL");
    CHECK(descriptor_table_open_count(&table) ==
              PROCESS_DESCRIPTOR_CAPACITY,
          "full table open count is exact");

    CHECK(descriptor_table_release(&table, 0U) == DESCRIPTOR_OK &&
              descriptor_table_reserve(&table, &descriptor) ==
                  DESCRIPTOR_OK && descriptor == 0U,
          "released descriptor is recovered after exhaustion");
    CHECK(descriptor_table_destroy(&table) == DESCRIPTOR_OK &&
              descriptor_table_open_count(&table) == 0U,
          "descriptor destroy tears down all reservations");
    CHECK(descriptor_status_string(DESCRIPTOR_FULL) != NULL &&
              descriptor_status_string(DESCRIPTOR_BAD_DESCRIPTOR) != NULL,
          "descriptor status strings are present");
}

int main(void) {
    test_init_and_validation();
    test_reserve_release_and_reuse();
    test_exhaustion_and_teardown();

    if (failures != 0) {
        fprintf(stderr, "Descriptor table host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Descriptor table host tests: ALL OK");
    return 0;
}

#endif
