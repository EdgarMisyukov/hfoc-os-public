#include "key_use.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_KEY_USE_HOST_TEST

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

void *kmalloc(size_t size)
{
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr)
{
    (void)ptr;
    ++free_attempts;
    return false;
}

static void test_purpose_validity(void)
{
    uint32_t purpose;

    CHECK(!key_use_purpose_valid(0U),
          "key use rejects zero purpose");
    CHECK(!key_use_purpose_valid(KEY_USE_PURPOSE_MAX + 1U),
          "key use rejects purpose above recovery");
    CHECK(!key_use_purpose_valid(UINT32_MAX),
          "key use rejects arbitrary large purpose");
    for (purpose = KEY_USE_PURPOSE_MIN;
         purpose <= KEY_USE_PURPOSE_MAX;
         ++purpose) {
        CHECK(key_use_purpose_valid(purpose),
              "key use accepts every defined purpose");
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "key use purpose validation performs no allocation");
}

static void test_accepted_purposes_validity(void)
{
    CHECK(!key_use_accepted_purposes_valid(0U),
          "key use rejects empty accepted-purpose mask");
    CHECK(!key_use_accepted_purposes_valid(0x20U),
          "key use rejects mask with unknown bit");
    CHECK(!key_use_accepted_purposes_valid(UINT32_MAX),
          "key use rejects arbitrary large accepted-purpose mask");
    CHECK(key_use_accepted_purposes_valid(
              KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE)),
          "key use accepts image-only mask");
    CHECK(key_use_accepted_purposes_valid(
              KEY_USE_ACCEPTED_PURPOSES_MASK),
          "key use accepts all-purpose mask");
    CHECK(key_use_accepted_purposes_valid(
              KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_RECOVERY)),
          "key use accepts recovery-only mask");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "key use mask validation performs no allocation");
}

static void test_membership(void)
{
    uint32_t purpose;

    CHECK(!key_use_purpose_is_accepted(
              KEY_USE_ACCEPTED_PURPOSES_ALL, 0U),
          "key use membership rejects zero purpose");
    CHECK(!key_use_purpose_is_accepted(
              KEY_USE_ACCEPTED_PURPOSES_ALL, KEY_USE_PURPOSE_MAX + 1U),
          "key use membership rejects unknown purpose");
    for (purpose = KEY_USE_PURPOSE_MIN;
         purpose <= KEY_USE_PURPOSE_MAX;
         ++purpose) {
        CHECK(key_use_purpose_is_accepted(
                  KEY_USE_ACCEPTED_PURPOSES_ALL, purpose),
              "key use all-purpose mask accepts every defined purpose");
    }

    CHECK(key_use_purpose_is_accepted(
              KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE),
              KEY_USE_PURPOSE_IMAGE),
          "key use image-only mask accepts image purpose");
    CHECK(!key_use_purpose_is_accepted(
              KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE),
              KEY_USE_PURPOSE_UPDATE),
          "key use image-only mask denies update purpose");
    CHECK(!key_use_purpose_is_accepted(
              KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE),
              KEY_USE_PURPOSE_BACKUP),
          "key use image-only mask denies backup purpose");
    CHECK(!key_use_purpose_is_accepted(
              KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE),
              KEY_USE_PURPOSE_INTEGRATION),
          "key use image-only mask denies integration purpose");
    CHECK(!key_use_purpose_is_accepted(
              KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE),
              KEY_USE_PURPOSE_RECOVERY),
          "key use image-only mask denies recovery purpose");

    CHECK(key_use_purpose_is_accepted(
              KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_UPDATE) |
              KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_BACKUP),
              KEY_USE_PURPOSE_UPDATE),
          "key use update/backup mask accepts update purpose");
    CHECK(key_use_purpose_is_accepted(
              KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_UPDATE) |
              KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_BACKUP),
              KEY_USE_PURPOSE_BACKUP),
          "key use update/backup mask accepts backup purpose");
    CHECK(!key_use_purpose_is_accepted(
              KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_UPDATE) |
              KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_BACKUP),
              KEY_USE_PURPOSE_INTEGRATION),
          "key use update/backup mask denies integration purpose");
    CHECK(key_use_purpose_is_accepted(
              KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_RECOVERY) |
              KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE),
              KEY_USE_PURPOSE_RECOVERY),
          "key use recovery/image mask accepts recovery purpose");
    CHECK(!key_use_purpose_is_accepted(
              KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE),
              KEY_USE_PURPOSE_RECOVERY),
          "key use recovery membership rejects unsupported mask");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "key use membership checks perform no allocation");
}

int main(void)
{
    test_purpose_validity();
    test_accepted_purposes_validity();
    test_membership();

    if (failures != 0) {
        fprintf(stderr, "Key use host tests: %d FAILURES\n", failures);
        return 1;
    }
    printf("Key use host tests: ALL OK\n");
    return 0;
}

#else
#error "This host test must be built with HFOC_KEY_USE_HOST_TEST"
#endif
