#include "appliance_identity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_IDENTITY_HOST_TEST

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

static void test_status_strings(void)
{
    CHECK(strcmp(appliance_identity_status_string(
                     APPLIANCE_IDENTITY_OK),
                 "APPLIANCE_IDENTITY_OK") == 0 &&
              strcmp(appliance_identity_status_string(
                         APPLIANCE_IDENTITY_NULL),
                     "APPLIANCE_IDENTITY_NULL") == 0 &&
              strcmp(appliance_identity_status_string(
                         APPLIANCE_IDENTITY_BAD_STATE),
                     "APPLIANCE_IDENTITY_BAD_STATE") == 0 &&
              strcmp(appliance_identity_status_string(
                         APPLIANCE_IDENTITY_BAD_VERSION),
                     "APPLIANCE_IDENTITY_BAD_VERSION") == 0 &&
              strcmp(appliance_identity_status_string(
                         APPLIANCE_IDENTITY_BAD_FIELD),
                     "APPLIANCE_IDENTITY_BAD_FIELD") == 0 &&
              strcmp(appliance_identity_status_string(
                         APPLIANCE_IDENTITY_OVERFLOW),
                     "APPLIANCE_IDENTITY_OVERFLOW") == 0,
          "appliance identity status strings are exact");
    CHECK(appliance_identity_status_string(
              (enum appliance_identity_status)6U) == NULL,
          "appliance identity rejects undefined status string");
}

static void test_boot_init_and_get(void)
{
    const struct appliance_identity *identity;

    CHECK(appliance_identity_get() == NULL,
          "appliance identity is not available before boot init");
    CHECK(appliance_identity_boot_init() == APPLIANCE_IDENTITY_OK,
          "appliance identity boot init publishes exact default");
    identity = appliance_identity_get();
    CHECK(identity != NULL &&
              identity->version == APPLIANCE_IDENTITY_VERSION &&
              strcmp(identity->machine_class,
                     APPLIANCE_IDENTITY_MACHINE_CLASS) == 0 &&
              strcmp(identity->product,
                     APPLIANCE_IDENTITY_PRODUCT) == 0 &&
              strcmp(identity->architecture,
                     APPLIANCE_IDENTITY_ARCHITECTURE) == 0 &&
              strcmp(identity->platform,
                     APPLIANCE_IDENTITY_PLATFORM) == 0 &&
              strcmp(identity->hostname,
                     APPLIANCE_IDENTITY_DEFAULT_HOSTNAME) == 0 &&
              strcmp(identity->instance,
                     APPLIANCE_IDENTITY_DEFAULT_INSTANCE) == 0 &&
              identity->published,
          "appliance identity get returns published immutable descriptor");
    CHECK(appliance_identity_boot_init() == APPLIANCE_IDENTITY_BAD_STATE,
          "appliance identity boot init rejects second publication");
}

static struct appliance_identity make_bad_identity(void)
{
    struct appliance_identity identity;

    memset(&identity, 0, sizeof(identity));
    identity.version = APPLIANCE_IDENTITY_VERSION;
    memcpy(identity.machine_class, APPLIANCE_IDENTITY_MACHINE_CLASS,
           sizeof(APPLIANCE_IDENTITY_MACHINE_CLASS));
    memcpy(identity.product, APPLIANCE_IDENTITY_PRODUCT,
           sizeof(APPLIANCE_IDENTITY_PRODUCT));
    memcpy(identity.platform, APPLIANCE_IDENTITY_PLATFORM,
           sizeof(APPLIANCE_IDENTITY_PLATFORM));
    memcpy(identity.architecture, APPLIANCE_IDENTITY_ARCHITECTURE,
           sizeof(APPLIANCE_IDENTITY_ARCHITECTURE));
    memcpy(identity.hostname, APPLIANCE_IDENTITY_DEFAULT_HOSTNAME,
           sizeof(APPLIANCE_IDENTITY_DEFAULT_HOSTNAME));
    memcpy(identity.instance, APPLIANCE_IDENTITY_DEFAULT_INSTANCE,
           sizeof(APPLIANCE_IDENTITY_DEFAULT_INSTANCE));
    identity.published = true;
    return identity;
}

static void test_validate(void)
{
    const struct appliance_identity *identity = appliance_identity_get();
    struct appliance_identity bad;

    CHECK(appliance_identity_validate(NULL) == APPLIANCE_IDENTITY_NULL,
          "appliance identity validate rejects null descriptor");
    CHECK(identity != NULL &&
              appliance_identity_validate(identity) ==
                  APPLIANCE_IDENTITY_OK,
          "appliance identity validate accepts exact boot descriptor");

    bad = make_bad_identity();
    bad.version = 0U;
    CHECK(appliance_identity_validate(&bad) ==
              APPLIANCE_IDENTITY_BAD_VERSION,
          "appliance identity validate rejects bad version");
    bad = make_bad_identity();
    bad.published = false;
    CHECK(appliance_identity_validate(&bad) ==
              APPLIANCE_IDENTITY_BAD_STATE,
          "appliance identity validate rejects unpublished descriptor");
    bad = make_bad_identity();
    bad.machine_class[0] = 'X';
    CHECK(appliance_identity_validate(&bad) ==
              APPLIANCE_IDENTITY_BAD_FIELD,
          "appliance identity validate rejects changed machine class");
    bad = make_bad_identity();
    bad.hostname[0] = 'X';
    CHECK(appliance_identity_validate(&bad) ==
              APPLIANCE_IDENTITY_BAD_FIELD,
          "appliance identity validate rejects changed default hostname");
}

static void test_render(void)
{
    const struct appliance_identity *identity = appliance_identity_get();
    const char *expected =
        "identity v=1 class=hfoc-appliance product=HFOC-APPLIANCE-V1 "
        "arch=x86_64 platform=q35 hostname=hfoc-appliance "
        "instance=default\n";
    char output[APPLIANCE_IDENTITY_RENDER_MAX + 1U];
    char small[4] = {'K', 'E', 'E', 'P'};
    size_t length = 0U;
    size_t length_a = 0U;
    size_t length_b = 0U;

    CHECK(identity != NULL &&
              appliance_identity_render(identity, output,
                                        sizeof(output), &length) ==
                  APPLIANCE_IDENTITY_OK &&
              length == strlen(expected) &&
              strcmp(output, expected) == 0 &&
              output[length] == '\0',
          "appliance identity render emits exact canonical record");
    CHECK(appliance_identity_render(identity, small,
                                    sizeof(small), &length) ==
              APPLIANCE_IDENTITY_OVERFLOW &&
              memcmp(small, "KEEP", sizeof(small)) == 0,
          "appliance identity render rejects small buffer without mutation");
    CHECK(appliance_identity_render(identity, output,
                                    sizeof(output), &length_a) ==
              APPLIANCE_IDENTITY_OK &&
              appliance_identity_render(identity, output,
                                        sizeof(output), &length_b) ==
                  APPLIANCE_IDENTITY_OK &&
              length_a == length_b &&
              strcmp(output, expected) == 0,
          "appliance identity repeated render is byte-identical");
    CHECK(appliance_identity_render(NULL, output,
                                    sizeof(output), &length) ==
              APPLIANCE_IDENTITY_NULL,
          "appliance identity render rejects null identity");
}

int main(void)
{
    test_status_strings();
    test_boot_init_and_get();
    test_validate();
    test_render();
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "appliance identity performs no allocation");
    if (failures != 0) {
        fprintf(stderr, "Appliance identity host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Appliance identity host tests: ALL OK");
    return 0;
}

#endif
