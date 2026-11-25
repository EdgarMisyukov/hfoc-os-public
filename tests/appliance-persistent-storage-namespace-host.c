#include "appliance_persistent_storage_namespace.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_HOST_TEST

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

struct expected_namespace {
    enum appliance_persistent_storage_namespace_id id;
    const char *name;
    enum appliance_persistent_storage_namespace_sensitivity sensitivity;
    const char *purpose;
    uint32_t prefix_count;
    const char *prefixes[
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_RECORD_PREFIX_MAX];
    const char *id_string;
};

static const struct expected_namespace expected_namespaces[] = {
    {
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
        "system.config",
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_PRIVATE,
        "validated system, network and security configuration",
        3U,
        {"v1:system.", "v1:network.", "v1:security."},
        "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG",
    },
    {
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SERVICE_DATA,
        "service.data",
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_PRIVATE,
        "validated service enablement and service state",
        1U,
        {"v1:service.", NULL},
        "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SERVICE_DATA",
    },
    {
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_CERTIFICATE,
        "certificate",
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_PRIVATE,
        "persistent certificate material",
        1U,
        {"v1:certificate.", NULL},
        "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_CERTIFICATE",
    },
    {
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SECRET,
        "secret",
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_SECRET,
        "persistent secret material and protection metadata",
        1U,
        {"v1:secret.", NULL},
        "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SECRET",
    },
    {
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_DIAGNOSTIC,
        "diagnostic",
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_PRIVATE,
        "bounded runtime diagnostics and health evidence",
        1U,
        {"v1:diagnostic.", NULL},
        "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_DIAGNOSTIC",
    },
    {
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_RECOVERY_METADATA,
        "recovery.metadata",
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_PRIVATE,
        "recovery and rollback metadata",
        1U,
        {"v1:recovery.", NULL},
        "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_RECOVERY_METADATA",
    },
};

static const char *expected_status_string(
    enum appliance_persistent_storage_namespace_status status)
{
    switch (status) {
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_OK:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_OK";
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_NULL:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_NULL";
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_BAD_ID:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_BAD_ID";
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_OUT_OF_RANGE:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_OUT_OF_RANGE";
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_BAD_KEY:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_BAD_KEY";
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_UNKNOWN_NAMESPACE:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_UNKNOWN_NAMESPACE";
    default:
        return NULL;
    }
}

static const char *expected_sensitivity_string(
    enum appliance_persistent_storage_namespace_sensitivity sensitivity)
{
    switch (sensitivity) {
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_NONE:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_NONE";
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_GENERAL:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_GENERAL";
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_PRIVATE:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_PRIVATE";
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_SECRET:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_SECRET";
    default:
        return NULL;
    }
}

static bool descriptor_matches_expected(
    const struct appliance_persistent_storage_namespace *descriptor,
    const struct expected_namespace *expected)
{
    size_t index;

    if (descriptor->id != expected->id ||
        descriptor->name == NULL ||
        strcmp(descriptor->name, expected->name) != 0 ||
        descriptor->record_prefix_count != expected->prefix_count ||
        descriptor->sensitivity != expected->sensitivity ||
        descriptor->purpose == NULL ||
        strcmp(descriptor->purpose, expected->purpose) != 0) {
        return false;
    }
    for (index = 0U;
         index < expected->prefix_count &&
             index < APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_RECORD_PREFIX_MAX;
         ++index) {
        const char *prefix = descriptor->record_prefixes[index];
        const char *expected_prefix = expected->prefixes[index];

        if (prefix == NULL || expected_prefix == NULL ||
            descriptor->record_prefix_lengths[index] != strlen(prefix) ||
            strcmp(prefix, expected_prefix) != 0) {
            return false;
        }
    }
    return true;
}

static void test_catalogue_and_describe(void)
{
    const struct appliance_persistent_storage_namespace *descriptor;
    enum appliance_persistent_storage_namespace_status status;
    enum appliance_persistent_storage_namespace_id id;
    size_t index;

    CHECK(appliance_persistent_storage_namespace_count() ==
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_COUNT,
          "persistent storage namespace catalogue count is exact");
    CHECK(appliance_persistent_storage_namespace_catalogue_at(
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_COUNT,
              &descriptor) ==
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_OUT_OF_RANGE &&
              descriptor == NULL,
          "persistent storage namespace catalogue rejects out-of-range index");
    CHECK(appliance_persistent_storage_namespace_catalogue_at(
              0U, NULL) == APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_NULL,
          "persistent storage namespace catalogue rejects null output");
    CHECK(appliance_persistent_storage_namespace_describe(
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE,
              &descriptor) == APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_BAD_ID &&
              descriptor == NULL,
          "persistent storage namespace describe rejects none id");
    CHECK(appliance_persistent_storage_namespace_describe(
              (enum appliance_persistent_storage_namespace_id)7U,
              &descriptor) == APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_BAD_ID &&
              descriptor == NULL,
          "persistent storage namespace describe rejects unknown id");
    CHECK(appliance_persistent_storage_namespace_describe(
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
              NULL) == APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_NULL,
          "persistent storage namespace describe rejects null output");

    for (index = 0U;
         index < APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_COUNT;
         ++index) {
        status = appliance_persistent_storage_namespace_catalogue_at(
            index, &descriptor);
        CHECK(status == APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_OK &&
                  descriptor != NULL &&
                  descriptor_matches_expected(
                      descriptor, &expected_namespaces[index]),
              "persistent storage namespace catalogue entry is exact");

        id = (enum appliance_persistent_storage_namespace_id)
            (index + 1U);
        status = appliance_persistent_storage_namespace_describe(
            id, &descriptor);
        CHECK(status == APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_OK &&
                  descriptor != NULL &&
                  descriptor_matches_expected(
                      descriptor, &expected_namespaces[index]),
              "persistent storage namespace describe entry is exact");
    }
}

static void test_classification(void)
{
    enum appliance_persistent_storage_namespace_id namespace_id =
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE;
    enum appliance_persistent_storage_namespace_id expected;
    size_t index;

    CHECK(appliance_persistent_storage_namespace_classify_record_key(
              NULL, 0U, &namespace_id) ==
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_NULL &&
              namespace_id == APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE,
          "persistent storage namespace classifier rejects null key");
    CHECK(appliance_persistent_storage_namespace_classify_record_key(
              "v1:system.hostname=hfoc-appliance",
              strlen("v1:system.hostname=hfoc-appliance"),
              NULL) == APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_NULL,
          "persistent storage namespace classifier rejects null output");
    CHECK(appliance_persistent_storage_namespace_classify_record_key(
              "", 0U, &namespace_id) ==
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_BAD_KEY &&
              namespace_id == APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE,
          "persistent storage namespace classifier rejects empty key");
    CHECK(appliance_persistent_storage_namespace_classify_record_key(
              "v1:system.", strlen("v1:system."), &namespace_id) ==
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_BAD_KEY &&
              namespace_id == APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE,
          "persistent storage namespace classifier rejects bare prefix");
    /* ITEM-516: the after-boot policy line is system configuration too. */
    CHECK(appliance_persistent_storage_namespace_classify_record_key(
              "v1:security.panel.after-boot=open",
              strlen("v1:security.panel.after-boot=open"), &namespace_id) ==
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_OK &&
              namespace_id ==
                  APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
          "the management-gate key classifies to system configuration");
    CHECK(appliance_persistent_storage_namespace_classify_record_key(
              "v1:audit.entry=1",
              strlen("v1:audit.entry=1"),
              &namespace_id) ==
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_UNKNOWN_NAMESPACE &&
              namespace_id == APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE,
          "persistent storage namespace classifier rejects unknown prefix");
    CHECK(appliance_persistent_storage_namespace_classify_record_key(
              "system.config", strlen("system.config"), &namespace_id) ==
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_UNKNOWN_NAMESPACE &&
              namespace_id == APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE,
          "persistent storage namespace classifier rejects missing v1 prefix");

    for (index = 0U;
         index < APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_COUNT;
         ++index) {
        expected = (enum appliance_persistent_storage_namespace_id)
            (index + 1U);
        namespace_id = APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE;
        if (expected ==
            APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG) {
            CHECK(appliance_persistent_storage_namespace_classify_record_key(
                      "v1:system.hostname=hfoc-appliance",
                      strlen("v1:system.hostname=hfoc-appliance"),
                      &namespace_id) ==
                      APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_OK &&
                      namespace_id == expected,
                  "persistent storage namespace system record classifies");
            CHECK(appliance_persistent_storage_namespace_classify_record_key(
                      "v1:network.interface=eth0",
                      strlen("v1:network.interface=eth0"),
                      &namespace_id) ==
                      APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_OK &&
                      namespace_id == expected,
                  "persistent storage namespace network record classifies");
        } else {
            const char *key =
                expected_namespaces[index].prefixes[0];
            char record[64];
            size_t record_length;

            record_length = strlen(key);
            memcpy(record, key, record_length);
            record[record_length] = 'x';
            ++record_length;
            CHECK(appliance_persistent_storage_namespace_classify_record_key(
                      record, record_length, &namespace_id) ==
                      APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_OK &&
                      namespace_id == expected,
                  "persistent storage namespace record classifies by prefix");
        }
    }
}

static void test_strings(void)
{
    enum appliance_persistent_storage_namespace_status status;
    enum appliance_persistent_storage_namespace_id id;
    enum appliance_persistent_storage_namespace_sensitivity sensitivity;
    size_t index;

    for (status = APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_OK;
         status <= APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_UNKNOWN_NAMESPACE;
         ++status) {
        const char *string =
            appliance_persistent_storage_namespace_status_string(status);
        const char *expected = expected_status_string(status);

        CHECK(string != NULL && expected != NULL &&
                  strcmp(string, expected) == 0,
              "persistent storage namespace status string is exact");
    }
    CHECK(appliance_persistent_storage_namespace_status_string(
              (enum appliance_persistent_storage_namespace_status)6U) == NULL,
          "persistent storage namespace undefined status string is rejected");

    for (id = APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE;
         id <= APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_RECOVERY_METADATA;
         ++id) {
        const char *string =
            appliance_persistent_storage_namespace_id_string(id);
        const char *expected;

        if (id == APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE) {
            expected =
                "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE";
        } else {
            expected = expected_namespaces[(size_t)id - 1U].id_string;
        }
        CHECK(string != NULL && expected != NULL &&
                  strcmp(string, expected) == 0,
              "persistent storage namespace id string is exact");
    }
    CHECK(appliance_persistent_storage_namespace_id_string(
              (enum appliance_persistent_storage_namespace_id)7U) == NULL,
          "persistent storage namespace undefined id string is rejected");

    for (sensitivity =
             APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_NONE;
         sensitivity <=
             APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_SECRET;
         ++sensitivity) {
        const char *string =
            appliance_persistent_storage_namespace_sensitivity_string(
                sensitivity);
        const char *expected = expected_sensitivity_string(sensitivity);

        CHECK(string != NULL && expected != NULL &&
                  strcmp(string, expected) == 0,
              "persistent storage namespace sensitivity string is exact");
    }
    CHECK(appliance_persistent_storage_namespace_sensitivity_string(
              (enum appliance_persistent_storage_namespace_sensitivity)4U) ==
              NULL,
          "persistent storage namespace undefined sensitivity string is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "persistent storage namespace handling performs no allocation");

    index = 0U;
    (void)index;
}

int main(void)
{
    test_catalogue_and_describe();
    test_classification();
    test_strings();
    if (failures != 0) {
        fprintf(stderr,
                "Persistent storage namespace host test: FAIL (%d)\n",
                failures);
        return 1;
    }
    printf("Persistent storage namespace host test: ALL OK\n");
    return 0;
}

#endif
