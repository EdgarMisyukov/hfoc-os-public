#include "boot_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_BOOT_POLICY_HOST_TEST

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

static struct boot_policy_record make_record(uint32_t normal,
                                             uint32_t recovery,
                                             uint32_t last_known_good)
{
    struct boot_policy_record record;

    record.image_ids[BOOT_POLICY_MODE_NORMAL] = normal;
    record.image_ids[BOOT_POLICY_MODE_RECOVERY] = recovery;
    record.image_ids[BOOT_POLICY_MODE_LAST_KNOWN_GOOD] = last_known_good;
    return record;
}

static bool record_equal(const struct boot_policy_record *left,
                         const struct boot_policy_record *right)
{
    return left != NULL && right != NULL &&
           memcmp(left, right, sizeof(*left)) == 0;
}

static void test_validation_and_status_strings(void) {
    struct boot_policy policy;
    struct boot_policy_record record;
    uint32_t image_id = 0U;
    enum boot_policy_mode mode;
    enum boot_policy_status status;

    memset(&policy, 0, sizeof(policy));
    record = make_record(1U, 2U, 3U);

    CHECK(boot_policy_init(NULL, &record) == BOOT_POLICY_NULL,
          "policy init rejects null policy");
    CHECK(boot_policy_init(&policy, NULL) == BOOT_POLICY_NULL,
          "policy init rejects null record");
    CHECK(boot_policy_select(NULL, BOOT_POLICY_MODE_NORMAL, &image_id) ==
              BOOT_POLICY_NULL,
          "policy select rejects null policy");
    CHECK(boot_policy_select(&policy, BOOT_POLICY_MODE_NORMAL, NULL) ==
              BOOT_POLICY_NULL,
          "policy select rejects null output");
    CHECK(boot_policy_destroy(NULL) == BOOT_POLICY_NULL,
          "policy destroy rejects null policy");
    CHECK(boot_policy_record(NULL) == NULL,
          "policy record query rejects null policy");

    for (mode = BOOT_POLICY_MODE_NORMAL;
         mode <= BOOT_POLICY_MODE_LAST_KNOWN_GOOD; ++mode) {
        CHECK(boot_policy_mode_string(mode) != NULL &&
                  (mode == BOOT_POLICY_MODE_NORMAL ?
                       strcmp(boot_policy_mode_string(mode),
                              "BOOT_POLICY_MODE_NORMAL") == 0 :
                       mode == BOOT_POLICY_MODE_RECOVERY ?
                           strcmp(boot_policy_mode_string(mode),
                                  "BOOT_POLICY_MODE_RECOVERY") == 0 :
                           strcmp(boot_policy_mode_string(mode),
                                  "BOOT_POLICY_MODE_LAST_KNOWN_GOOD") == 0),
              "policy mode string is present for every defined mode");
    }
    CHECK(boot_policy_mode_string((enum boot_policy_mode)3U) == NULL,
          "policy mode string rejects undefined mode");
    for (status = BOOT_POLICY_OK; status <= BOOT_POLICY_INVALID_MODE; ++status) {
        CHECK(boot_policy_status_string(status) != NULL &&
                  (status == BOOT_POLICY_OK ?
                       strcmp(boot_policy_status_string(status),
                              "BOOT_POLICY_OK") == 0 :
                       status == BOOT_POLICY_NULL ?
                           strcmp(boot_policy_status_string(status),
                                  "BOOT_POLICY_NULL") == 0 :
                           status == BOOT_POLICY_BAD_STATE ?
                               strcmp(boot_policy_status_string(status),
                                      "BOOT_POLICY_BAD_STATE") == 0 :
                               status == BOOT_POLICY_INVALID_RECORD ?
                                   strcmp(boot_policy_status_string(status),
                                          "BOOT_POLICY_INVALID_RECORD") == 0 :
                                   status == BOOT_POLICY_UNPUBLISHED ?
                                       strcmp(boot_policy_status_string(status),
                                              "BOOT_POLICY_UNPUBLISHED") == 0 :
                                       strcmp(boot_policy_status_string(status),
                                              "BOOT_POLICY_INVALID_MODE") == 0),
              "policy status string is present for every defined status");
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "boot policy validation performs no allocation");
}

static void test_invalid_records_never_publish_or_select(void) {
    struct boot_policy policy;
    struct boot_policy_record record;
    uint32_t image_id = 0xA5A5A5A5U;

    memset(&policy, 0, sizeof(policy));
    record = make_record(0U, 2U, 3U);
    CHECK(boot_policy_init(&policy, &record) == BOOT_POLICY_INVALID_RECORD &&
              boot_policy_record(&policy) == NULL &&
              !policy.published,
          "zero normal image id is rejected without publication");
    CHECK(boot_policy_select(&policy, BOOT_POLICY_MODE_NORMAL, &image_id) ==
              BOOT_POLICY_UNPUBLISHED &&
              image_id == 0xA5A5A5A5U,
          "unpublished policy never writes selected image");
    CHECK(boot_policy_destroy(&policy) == BOOT_POLICY_BAD_STATE,
          "destroy rejects unpublished policy");

    memset(&policy, 0, sizeof(policy));
    policy.published = true;
    policy.record = make_record(0U, 2U, 3U);
    image_id = 0xA5A5A5A5U;
    CHECK(boot_policy_select(&policy, BOOT_POLICY_MODE_NORMAL, &image_id) ==
              BOOT_POLICY_INVALID_RECORD &&
              image_id == 0xA5A5A5A5U &&
              boot_policy_record(&policy) == NULL,
          "corrupted published record is rejected without image selection");

    memset(&policy, 0, sizeof(policy));
    record = make_record(1U, BOOT_POLICY_IMAGE_ID_MAX + 1U, 3U);
    CHECK(boot_policy_init(&policy, &record) == BOOT_POLICY_INVALID_RECORD &&
              boot_policy_record(&policy) == NULL &&
              !policy.published,
          "over-limit recovery image id is rejected without publication");

    memset(&policy, 0, sizeof(policy));
    record = make_record(1U, 2U, UINT32_MAX);
    CHECK(boot_policy_init(&policy, &record) == BOOT_POLICY_INVALID_RECORD &&
              boot_policy_record(&policy) == NULL &&
              !policy.published,
          "overflow image id is rejected without publication");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "invalid-record handling performs no allocation");
}

static void test_valid_record_selects_exact_images(void) {
    struct boot_policy policy;
    struct boot_policy_record record;
    struct boot_policy_record published;
    uint32_t image_id = 0U;

    memset(&policy, 0, sizeof(policy));
    record = make_record(7U, 9U, 7U);
    CHECK(boot_policy_init(&policy, &record) == BOOT_POLICY_OK &&
              boot_policy_record(&policy) != NULL &&
              record_equal(boot_policy_record(&policy), &record),
          "valid policy record publishes exact image ids");

    published = *boot_policy_record(&policy);
    CHECK(boot_policy_select(&policy, BOOT_POLICY_MODE_NORMAL, &image_id) ==
              BOOT_POLICY_OK &&
              image_id == published.image_ids[BOOT_POLICY_MODE_NORMAL],
          "normal mode selects exact image id");
    CHECK(boot_policy_select(&policy, BOOT_POLICY_MODE_RECOVERY, &image_id) ==
              BOOT_POLICY_OK &&
              image_id == published.image_ids[BOOT_POLICY_MODE_RECOVERY],
          "recovery mode selects exact image id");
    CHECK(boot_policy_select(&policy, BOOT_POLICY_MODE_LAST_KNOWN_GOOD,
                             &image_id) == BOOT_POLICY_OK &&
              image_id ==
                  published.image_ids[BOOT_POLICY_MODE_LAST_KNOWN_GOOD],
          "last-known-good mode selects exact image id");

    CHECK(boot_policy_init(&policy, &record) == BOOT_POLICY_BAD_STATE &&
              record_equal(boot_policy_record(&policy), &record),
          "reinitializing a published policy is rejected");
    CHECK(boot_policy_select(&policy, (enum boot_policy_mode)3U, &image_id) ==
              BOOT_POLICY_INVALID_MODE,
          "undefined mode is rejected");

    CHECK(boot_policy_destroy(&policy) == BOOT_POLICY_OK &&
              boot_policy_record(&policy) == NULL &&
              !policy.published,
          "destroy clears published policy");
    CHECK(boot_policy_select(&policy, BOOT_POLICY_MODE_NORMAL, &image_id) ==
              BOOT_POLICY_UNPUBLISHED,
          "destroyed policy rejects selection");
    CHECK(boot_policy_destroy(&policy) == BOOT_POLICY_BAD_STATE,
          "destroyed policy rejects second destroy");
    CHECK(boot_policy_init(&policy, &record) == BOOT_POLICY_OK &&
              boot_policy_record(&policy) != NULL,
          "destroyed policy can be initialized again");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "valid-record lifecycle performs no allocation");
}

static void test_failed_select_never_writes_output(void) {
    struct boot_policy policy;
    struct boot_policy_record record;
    uint32_t image_id;

    memset(&policy, 0, sizeof(policy));
    record = make_record(11U, 13U, 17U);
    CHECK(boot_policy_init(&policy, &record) == BOOT_POLICY_OK,
          "no-write fixture initializes");

    image_id = 0xA5A5A5A5U;
    CHECK(boot_policy_select(&policy, (enum boot_policy_mode)3U, &image_id) ==
              BOOT_POLICY_INVALID_MODE &&
              image_id == 0xA5A5A5A5U,
          "invalid mode selection never writes output");

    image_id = 0xA5A5A5A5U;
    CHECK(boot_policy_select(&policy, BOOT_POLICY_MODE_NORMAL, NULL) ==
              BOOT_POLICY_NULL,
          "null output selection is rejected");

    CHECK(boot_policy_destroy(&policy) == BOOT_POLICY_OK,
          "no-write fixture destroys policy");
    image_id = 0xA5A5A5A5U;
    CHECK(boot_policy_select(&policy, BOOT_POLICY_MODE_NORMAL, &image_id) ==
              BOOT_POLICY_UNPUBLISHED &&
              image_id == 0xA5A5A5A5U,
          "unpublished selection never writes output");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "failed selection performs no allocation");
}

int main(void) {
    test_validation_and_status_strings();
    test_invalid_records_never_publish_or_select();
    test_valid_record_selects_exact_images();
    test_failed_select_never_writes_output();

    if (failures != 0) {
        fprintf(stderr, "Boot policy host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Boot policy host tests: ALL OK");
    return 0;
}

#endif
