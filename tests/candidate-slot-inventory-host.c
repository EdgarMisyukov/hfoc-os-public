#include "candidate_slot_inventory.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_CANDIDATE_SLOT_INVENTORY_HOST_TEST

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

static struct candidate_slot_inventory_entry make_entry(
    uint32_t slot_id,
    uint32_t generation,
    uint8_t seed)
{
    struct candidate_slot_inventory_entry entry;
    size_t index;

    memset(&entry, 0, sizeof(entry));
    entry.slot_id = slot_id;
    entry.generation = generation;
    for (index = 0U; index < CANDIDATE_SLOT_INVENTORY_DIGEST_SIZE; ++index) {
        entry.digest[index] = (uint8_t)(seed + index);
    }
    return entry;
}

static bool entry_matches(
    const struct candidate_slot_inventory_entry *actual,
    const struct candidate_slot_inventory_entry *expected)
{
    return actual->slot_id == expected->slot_id &&
           actual->generation == expected->generation &&
           memcmp(actual->digest, expected->digest,
                  CANDIDATE_SLOT_INVENTORY_DIGEST_SIZE) == 0;
}

static void test_status_strings(void) {
    static const struct {
        enum candidate_slot_inventory_status status;
        const char *string;
    } statuses[] = {
        {CANDIDATE_SLOT_INVENTORY_OK, "CANDIDATE_SLOT_INVENTORY_OK"},
        {CANDIDATE_SLOT_INVENTORY_NULL, "CANDIDATE_SLOT_INVENTORY_NULL"},
        {CANDIDATE_SLOT_INVENTORY_BAD_STATE,
         "CANDIDATE_SLOT_INVENTORY_BAD_STATE"},
        {CANDIDATE_SLOT_INVENTORY_EMPTY, "CANDIDATE_SLOT_INVENTORY_EMPTY"},
        {CANDIDATE_SLOT_INVENTORY_TOO_MANY,
         "CANDIDATE_SLOT_INVENTORY_TOO_MANY"},
        {CANDIDATE_SLOT_INVENTORY_BAD_SLOT,
         "CANDIDATE_SLOT_INVENTORY_BAD_SLOT"},
        {CANDIDATE_SLOT_INVENTORY_BAD_GENERATION,
         "CANDIDATE_SLOT_INVENTORY_BAD_GENERATION"},
        {CANDIDATE_SLOT_INVENTORY_BAD_DIGEST,
         "CANDIDATE_SLOT_INVENTORY_BAD_DIGEST"},
        {CANDIDATE_SLOT_INVENTORY_DUPLICATE_SLOT,
         "CANDIDATE_SLOT_INVENTORY_DUPLICATE_SLOT"},
        {CANDIDATE_SLOT_INVENTORY_STALE_GENERATION,
         "CANDIDATE_SLOT_INVENTORY_STALE_GENERATION"},
        {CANDIDATE_SLOT_INVENTORY_NOT_FOUND,
         "CANDIDATE_SLOT_INVENTORY_NOT_FOUND"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = candidate_slot_inventory_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "candidate slot inventory status string is exact");
    }
    CHECK(candidate_slot_inventory_status_string(
              (enum candidate_slot_inventory_status)11U) == NULL,
          "candidate slot inventory undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "candidate slot inventory status handling performs no allocation");
}

static void test_null_and_bounds(void) {
    struct candidate_slot_inventory inventory;
    struct candidate_slot_inventory_entry entry = make_entry(1U, 1U, 0x10U);
    struct candidate_slot_inventory_entry output;
    struct candidate_slot_inventory_entry too_many[
        CANDIDATE_SLOT_INVENTORY_MAX_SLOTS + 1U];
    size_t index;

    memset(&inventory, 0xCD, sizeof(inventory));
    memset(&output, 0xCD, sizeof(output));
    for (index = 0U; index < CANDIDATE_SLOT_INVENTORY_MAX_SLOTS + 1U;
         ++index) {
        too_many[index] = make_entry(
            (uint32_t)index + 1U, 1U, (uint8_t)index);
    }

    CHECK(candidate_slot_inventory_init(NULL) ==
              CANDIDATE_SLOT_INVENTORY_NULL,
          "candidate slot inventory init rejects null inventory");
    CHECK(candidate_slot_inventory_publish(
              NULL, &entry, 1U, 1U) == CANDIDATE_SLOT_INVENTORY_NULL,
          "candidate slot inventory publish rejects null inventory");
    CHECK(candidate_slot_inventory_publish(
              &inventory, NULL, 1U, 1U) == CANDIDATE_SLOT_INVENTORY_NULL,
          "candidate slot inventory publish rejects null entries");
    CHECK(candidate_slot_inventory_publish(
              &inventory, &entry, 0U, 1U) == CANDIDATE_SLOT_INVENTORY_EMPTY,
          "candidate slot inventory publish rejects empty inventory");
    CHECK(candidate_slot_inventory_publish(
              &inventory, too_many,
              CANDIDATE_SLOT_INVENTORY_MAX_SLOTS + 1U, 1U) ==
              CANDIDATE_SLOT_INVENTORY_TOO_MANY,
          "candidate slot inventory publish rejects over-limit inventory");
    CHECK(candidate_slot_inventory_lookup(
              NULL, 1U, &output) == CANDIDATE_SLOT_INVENTORY_NULL,
          "candidate slot inventory lookup rejects null inventory");
    CHECK(candidate_slot_inventory_lookup(
              &inventory, 1U, NULL) == CANDIDATE_SLOT_INVENTORY_NULL,
          "candidate slot inventory lookup rejects null output");
    CHECK(candidate_slot_inventory_generation(NULL) == 0U,
          "candidate slot inventory generation rejects null input");
    CHECK(candidate_slot_inventory_slot_count(NULL) == 0U,
          "candidate slot inventory slot count rejects null input");
    CHECK(!candidate_slot_inventory_active(NULL),
          "candidate slot inventory active rejects null input");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "candidate slot inventory bounds checks perform no allocation");
}

static void test_valid_publication_and_lookup(void) {
    struct candidate_slot_inventory inventory;
    struct candidate_slot_inventory_entry entries[2];
    struct candidate_slot_inventory_entry expected[2];
    struct candidate_slot_inventory_entry output;
    struct candidate_slot_inventory_entry missing;

    entries[0] = make_entry(1U, 1U, 0x10U);
    entries[1] = make_entry(2U, 1U, 0x20U);
    expected[0] = entries[0];
    expected[1] = entries[1];
    memset(&output, 0xCD, sizeof(output));
    memset(&missing, 0xCD, sizeof(missing));

    CHECK(candidate_slot_inventory_init(&inventory) ==
              CANDIDATE_SLOT_INVENTORY_OK &&
              inventory.version == CANDIDATE_SLOT_INVENTORY_VERSION &&
              !inventory.active && inventory.slot_count == 0U,
          "candidate slot inventory init publishes empty inactive inventory");
    CHECK(candidate_slot_inventory_publish(
              &inventory, entries, 2U, 1U) == CANDIDATE_SLOT_INVENTORY_OK &&
              candidate_slot_inventory_active(&inventory) &&
              candidate_slot_inventory_generation(&inventory) == 1U &&
              candidate_slot_inventory_slot_count(&inventory) == 2U &&
              entry_matches(&inventory.slots[0], &expected[0]) &&
              entry_matches(&inventory.slots[1], &expected[1]),
          "valid candidate inventory publishes exact slot entries");
    CHECK(candidate_slot_inventory_lookup(
              &inventory, 1U, &output) == CANDIDATE_SLOT_INVENTORY_OK &&
              entry_matches(&output, &expected[0]),
          "candidate slot lookup returns exact first entry");
    CHECK(candidate_slot_inventory_lookup(
              &inventory, 2U, &output) == CANDIDATE_SLOT_INVENTORY_OK &&
              entry_matches(&output, &expected[1]),
          "candidate slot lookup returns exact second entry");
    CHECK(candidate_slot_inventory_lookup(
              &inventory, 9U, &missing) ==
              CANDIDATE_SLOT_INVENTORY_NOT_FOUND &&
              (unsigned char)missing.digest[0] == 0xCDU,
          "candidate slot lookup rejects unknown slot without writing output");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "valid candidate slot inventory flow performs no allocation");
}

static void test_failures_leave_active_unchanged(void) {
    struct candidate_slot_inventory inventory;
    struct candidate_slot_inventory original;
    struct candidate_slot_inventory_entry entries[2];
    struct candidate_slot_inventory_entry duplicate[2];
    struct candidate_slot_inventory_entry bad_slot;
    struct candidate_slot_inventory_entry bad_digest;
    struct candidate_slot_inventory_entry replacement[2];

    entries[0] = make_entry(1U, 1U, 0x10U);
    entries[1] = make_entry(2U, 1U, 0x20U);
    CHECK(candidate_slot_inventory_init(&inventory) ==
              CANDIDATE_SLOT_INVENTORY_OK,
          "candidate slot inventory failure fixture initializes");
    CHECK(candidate_slot_inventory_publish(
              &inventory, entries, 2U, 1U) == CANDIDATE_SLOT_INVENTORY_OK,
          "candidate slot inventory failure fixture publishes baseline");
    memcpy(&original, &inventory, sizeof(inventory));

    duplicate[0] = make_entry(1U, 2U, 0x30U);
    duplicate[1] = make_entry(1U, 2U, 0x31U);
    CHECK(candidate_slot_inventory_publish(
              &inventory, duplicate, 2U, 2U) ==
              CANDIDATE_SLOT_INVENTORY_DUPLICATE_SLOT &&
              memcmp(&inventory, &original, sizeof(inventory)) == 0,
          "duplicate candidate slot cannot replace active inventory");

    CHECK(candidate_slot_inventory_publish(
              &inventory, entries, 2U, 1U) ==
              CANDIDATE_SLOT_INVENTORY_STALE_GENERATION &&
              memcmp(&inventory, &original, sizeof(inventory)) == 0,
          "stale candidate generation cannot replace active inventory");

    CHECK(candidate_slot_inventory_publish(
              &inventory, entries, 2U, 0U) ==
              CANDIDATE_SLOT_INVENTORY_BAD_GENERATION &&
              memcmp(&inventory, &original, sizeof(inventory)) == 0,
          "zero candidate generation cannot replace active inventory");

    CHECK(candidate_slot_inventory_publish(
              &inventory, entries, 0U, 2U) ==
              CANDIDATE_SLOT_INVENTORY_EMPTY &&
              memcmp(&inventory, &original, sizeof(inventory)) == 0,
          "empty candidate cannot replace active inventory");

    bad_slot = make_entry(0U, 2U, 0x40U);
    CHECK(candidate_slot_inventory_publish(
              &inventory, &bad_slot, 1U, 2U) ==
              CANDIDATE_SLOT_INVENTORY_BAD_SLOT &&
              memcmp(&inventory, &original, sizeof(inventory)) == 0,
          "zero candidate slot ID cannot replace active inventory");

    bad_slot = make_entry(9U, 2U, 0x41U);
    CHECK(candidate_slot_inventory_publish(
              &inventory, &bad_slot, 1U, 2U) ==
              CANDIDATE_SLOT_INVENTORY_BAD_SLOT &&
              memcmp(&inventory, &original, sizeof(inventory)) == 0,
          "over-limit candidate slot ID cannot replace active inventory");

    bad_digest = make_entry(1U, 2U, 0x50U);
    memset(bad_digest.digest, 0, sizeof(bad_digest.digest));
    CHECK(candidate_slot_inventory_publish(
              &inventory, &bad_digest, 1U, 2U) ==
              CANDIDATE_SLOT_INVENTORY_BAD_DIGEST &&
              memcmp(&inventory, &original, sizeof(inventory)) == 0,
          "zero candidate digest cannot replace active inventory");

    CHECK(candidate_slot_inventory_publish(
              &inventory, NULL, 1U, 2U) == CANDIDATE_SLOT_INVENTORY_NULL &&
              memcmp(&inventory, &original, sizeof(inventory)) == 0,
          "null candidate entries cannot replace active inventory");

    replacement[0] = make_entry(1U, 2U, 0x60U);
    replacement[1] = make_entry(2U, 2U, 0x61U);
    CHECK(candidate_slot_inventory_publish(
              &inventory, replacement, 2U, 2U) ==
              CANDIDATE_SLOT_INVENTORY_OK &&
              candidate_slot_inventory_generation(&inventory) == 2U &&
              entry_matches(&inventory.slots[0], &replacement[0]) &&
              entry_matches(&inventory.slots[1], &replacement[1]),
          "strictly newer generation publishes complete replacement inventory");
    memcpy(&original, &inventory, sizeof(inventory));
    CHECK(candidate_slot_inventory_publish(
              &inventory, replacement, 2U, 2U) ==
              CANDIDATE_SLOT_INVENTORY_STALE_GENERATION &&
              memcmp(&inventory, &original, sizeof(inventory)) == 0,
          "duplicate newer attempt cannot replace published replacement");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "candidate slot inventory failure paths perform no allocation");
}

int main(void) {
    test_status_strings();
    test_null_and_bounds();
    test_valid_publication_and_lookup();
    test_failures_leave_active_unchanged();

    if (failures != 0) {
        fprintf(stderr, "Candidate slot inventory host tests: FAILED\n");
        return 1;
    }
    printf("Candidate slot inventory host tests: ALL OK\n");
    return 0;
}

#else
#error "This host test must be built with HFOC_CANDIDATE_SLOT_INVENTORY_HOST_TEST"
#endif
