#include "appliance_config_integrity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef APPLIANCE_CONFIG_INTEGRITY_HOST_TEST

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

struct expected_record {
    enum appliance_config_integrity_record_kind kind;
    enum appliance_persistent_storage_namespace_id namespace_id;
    const char *name;
    const char *prefix;
    const char *purpose;
};

static const struct expected_record expected_records[] = {
    {
        APPLIANCE_CONFIG_INTEGRITY_KIND_SYSTEM_HOSTNAME,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
        "system.hostname",
        "v1:system.hostname=",
        "validated appliance hostname",
    },
    {
        APPLIANCE_CONFIG_INTEGRITY_KIND_SYSTEM_LANGUAGE,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
        "system.language",
        "v1:system.language=",
        "validated interface language",
    },
    {
        APPLIANCE_CONFIG_INTEGRITY_KIND_SECURITY_PANEL_AFTER_BOOT,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
        "security.panel.after-boot",
        "v1:security.panel.after-boot=",
        "validated management gate after-boot policy",
    },
    {
        APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_INTERFACE,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
        "network.interface",
        "v1:network.interface=",
        "validated network interface identity",
    },
    {
        APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_MODE,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
        "network.mode",
        "v1:network.mode=",
        "validated DHCP or STATIC address mode",
    },
    {
        APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_IPV4_ADDRESS,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
        "network.ipv4_address",
        "v1:network.ipv4_address=",
        "validated static IPv4 address",
    },
    {
        APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_IPV4_PREFIX,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
        "network.ipv4_prefix",
        "v1:network.ipv4_prefix=",
        "validated static IPv4 prefix length",
    },
    {
        APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_IPV4_GATEWAY,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
        "network.ipv4_gateway",
        "v1:network.ipv4_gateway=",
        "validated static IPv4 gateway",
    },
    {
        APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_IPV4_DNS,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
        "network.ipv4_dns",
        "v1:network.ipv4_dns=",
        "validated static IPv4 DNS server",
    },
    {
        APPLIANCE_CONFIG_INTEGRITY_KIND_SERVICE_TABLE,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SERVICE_DATA,
        "service.table",
        "v1:services=",
        "validated persistent service table count",
    },
    {
        APPLIANCE_CONFIG_INTEGRITY_KIND_SERVICE_ENTRY,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SERVICE_DATA,
        "service.entry",
        "v1:service.",
        "validated persistent service enablement entry",
    },
};

static bool record_matches(const struct appliance_config_integrity_record *record,
                           const struct expected_record *expected)
{
    return record != NULL &&
           record->kind == expected->kind &&
           record->namespace_id == expected->namespace_id &&
           strcmp(record->name, expected->name) == 0 &&
           strcmp(record->prefix, expected->prefix) == 0 &&
           record->version == APPLIANCE_CONFIG_INTEGRITY_VERSION &&
           record->minimum_version == APPLIANCE_CONFIG_INTEGRITY_VERSION &&
           record->maximum_version == APPLIANCE_CONFIG_INTEGRITY_VERSION &&
           strcmp(record->purpose, expected->purpose) == 0;
}

static void test_catalogue_and_describe(void)
{
    size_t index;

    CHECK(appliance_config_integrity_count() ==
              APPLIANCE_CONFIG_INTEGRITY_RECORD_COUNT,
          "configuration integrity catalogue has exact record count");
    CHECK(appliance_config_integrity_catalogue_at(
              0U, NULL) == APPLIANCE_CONFIG_INTEGRITY_NULL,
          "catalogue-at rejects null record output");
    for (index = 0U;
         index < APPLIANCE_CONFIG_INTEGRITY_RECORD_COUNT;
         ++index) {
        const struct appliance_config_integrity_record *record = NULL;

        CHECK(appliance_config_integrity_catalogue_at(
                  index, &record) == APPLIANCE_CONFIG_INTEGRITY_OK &&
                  record_matches(record, &expected_records[index]),
              "catalogue-at returns exact immutable record");
        CHECK(appliance_config_integrity_describe(
                  expected_records[index].kind,
                  &record) == APPLIANCE_CONFIG_INTEGRITY_OK &&
                  record_matches(record, &expected_records[index]),
              "describe returns exact immutable record by kind");
    }
    CHECK(appliance_config_integrity_catalogue_at(
              APPLIANCE_CONFIG_INTEGRITY_RECORD_COUNT,
              NULL) == APPLIANCE_CONFIG_INTEGRITY_NULL,
          "catalogue-at rejects out-of-range before output check");
    {
        const struct appliance_config_integrity_record *record = NULL;

        CHECK(appliance_config_integrity_catalogue_at(
                  APPLIANCE_CONFIG_INTEGRITY_RECORD_COUNT,
                  &record) == APPLIANCE_CONFIG_INTEGRITY_OUT_OF_RANGE &&
                  record == NULL,
              "catalogue-at out-of-range publishes no record");
        CHECK(appliance_config_integrity_describe(
                  APPLIANCE_CONFIG_INTEGRITY_KIND_NONE,
                  &record) == APPLIANCE_CONFIG_INTEGRITY_BAD_ID &&
                  record == NULL,
              "describe unknown kind publishes no record");
    }
}

static bool classify_ok(const char *key,
                        enum appliance_config_integrity_record_kind kind,
                        enum appliance_persistent_storage_namespace_id ns,
                        uint32_t version)
{
    enum appliance_config_integrity_record_kind actual_kind =
        APPLIANCE_CONFIG_INTEGRITY_KIND_NONE;
    enum appliance_persistent_storage_namespace_id actual_ns =
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE;
    uint32_t actual_version = 0U;

    return appliance_config_integrity_classify_record_key(
               key, strlen(key), &actual_kind, &actual_ns,
               &actual_version) == APPLIANCE_CONFIG_INTEGRITY_OK &&
           actual_kind == kind && actual_ns == ns &&
           actual_version == version;
}

static void test_classification(void)
{
    CHECK(classify_ok(
              "v1:system.hostname=hfoc-appliance",
              APPLIANCE_CONFIG_INTEGRITY_KIND_SYSTEM_HOSTNAME,
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
              1U),
          "hostname record key classifies to system config");
    CHECK(classify_ok(
              "v1:system.language=ru",
              APPLIANCE_CONFIG_INTEGRITY_KIND_SYSTEM_LANGUAGE,
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
              1U),
          "language record key classifies to system config");
    CHECK(classify_ok(
              "v1:security.panel.after-boot=open",
              APPLIANCE_CONFIG_INTEGRITY_KIND_SECURITY_PANEL_AFTER_BOOT,
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
              1U),
          "after-boot policy key classifies to system config");
    CHECK(classify_ok(
              "v1:network.interface=eth0",
              APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_INTERFACE,
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
              1U),
          "network interface key classifies to system config");
    CHECK(classify_ok(
              "v1:network.mode=DHCP",
              APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_MODE,
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
              1U),
          "network mode key classifies to system config");
    CHECK(classify_ok(
              "v1:network.ipv4_address=10.0.2.15",
              APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_IPV4_ADDRESS,
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
              1U),
          "network IPv4 address key classifies to system config");
    CHECK(classify_ok(
              "v1:network.ipv4_prefix=24",
              APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_IPV4_PREFIX,
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
              1U),
          "network IPv4 prefix key classifies to system config");
    CHECK(classify_ok(
              "v1:network.ipv4_gateway=10.0.2.2",
              APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_IPV4_GATEWAY,
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
              1U),
          "network IPv4 gateway key classifies to system config");
    CHECK(classify_ok(
              "v1:network.ipv4_dns=10.0.2.3",
              APPLIANCE_CONFIG_INTEGRITY_KIND_NETWORK_IPV4_DNS,
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
              1U),
          "network IPv4 DNS key classifies to system config");
    CHECK(classify_ok(
              "v1:services=3",
              APPLIANCE_CONFIG_INTEGRITY_KIND_SERVICE_TABLE,
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SERVICE_DATA,
              1U),
          "service table key classifies to service data");
    CHECK(classify_ok(
              "v1:service.1=1,2,1000\n",
              APPLIANCE_CONFIG_INTEGRITY_KIND_SERVICE_ENTRY,
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SERVICE_DATA,
              1U),
          "service entry key classifies to service data");
    CHECK(classify_ok(
              "v1:service.255=0,3,2000",
              APPLIANCE_CONFIG_INTEGRITY_KIND_SERVICE_ENTRY,
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SERVICE_DATA,
              1U),
          "service entry accepts maximum service ID");
}

static void test_rejections(void)
{
    enum appliance_config_integrity_record_kind kind =
        APPLIANCE_CONFIG_INTEGRITY_KIND_NONE;
    enum appliance_persistent_storage_namespace_id ns =
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE;
    uint32_t version = 0U;
    unsigned char buffer[
        APPLIANCE_CONFIG_INTEGRITY_RECORD_MAX + 1U];

    CHECK(appliance_config_integrity_classify_record_key(
              NULL, 1U, &kind, &ns, &version) ==
              APPLIANCE_CONFIG_INTEGRITY_NULL,
          "classify rejects null key");
    CHECK(appliance_config_integrity_classify_record_key(
              "v1:system.hostname=a", 0U, &kind, &ns, &version) ==
              APPLIANCE_CONFIG_INTEGRITY_BAD_RECORD,
          "classify rejects zero length record");
    CHECK(appliance_config_integrity_classify_record_key(
              "", 0U, &kind, &ns, &version) ==
              APPLIANCE_CONFIG_INTEGRITY_BAD_RECORD,
          "classify rejects empty key");
    CHECK(appliance_config_integrity_classify_record_key(
              "v2:system.hostname=a", strlen("v2:system.hostname=a"),
              &kind, &ns, &version) == APPLIANCE_CONFIG_INTEGRITY_BAD_VERSION,
          "classify rejects unsupported version");
    CHECK(appliance_config_integrity_classify_record_key(
              "v1:certificate.https=abc",
              strlen("v1:certificate.https=abc"),
              &kind, &ns, &version) == APPLIANCE_CONFIG_INTEGRITY_UNKNOWN_KEY,
          "classify rejects future certificate key as unknown configuration");
    CHECK(appliance_config_integrity_classify_record_key(
              "v1:system.hostname=",
              strlen("v1:system.hostname="),
              &kind, &ns, &version) == APPLIANCE_CONFIG_INTEGRITY_BAD_RECORD,
          "classify rejects bare prefix without value");
    CHECK(appliance_config_integrity_classify_record_key(
              "v1:system.hostname",
              strlen("v1:system.hostname"),
              &kind, &ns, &version) == APPLIANCE_CONFIG_INTEGRITY_BAD_RECORD,
          "classify rejects missing equals");
    CHECK(appliance_config_integrity_classify_record_key(
              "v1:service.0=1,2,1000",
              strlen("v1:service.0=1,2,1000"),
              &kind, &ns, &version) == APPLIANCE_CONFIG_INTEGRITY_BAD_ID,
          "classify rejects zero service ID");
    CHECK(appliance_config_integrity_classify_record_key(
              "v1:service.256=1,2,1000",
              strlen("v1:service.256=1,2,1000"),
              &kind, &ns, &version) == APPLIANCE_CONFIG_INTEGRITY_BAD_ID,
          "classify rejects over-limit service ID");
    CHECK(appliance_config_integrity_classify_record_key(
              "v1:service.1=",
              strlen("v1:service.1="),
              &kind, &ns, &version) == APPLIANCE_CONFIG_INTEGRITY_BAD_RECORD,
          "classify rejects service entry without value");
    memset(buffer, (int)'x', sizeof(buffer));
    CHECK(appliance_config_integrity_classify_record_key(
              (const char *)buffer, sizeof(buffer), &kind, &ns,
              &version) == APPLIANCE_CONFIG_INTEGRITY_BAD_RECORD,
          "classify rejects over-limit record");
}

static void test_recovery_plan(void)
{
    CHECK(appliance_config_integrity_recovery_plan(
              CONFIG_JOURNAL_OK, true, false) ==
              APPLIANCE_CONFIG_INTEGRITY_RECOVERY_COMMIT,
          "valid journal record commits");
    CHECK(appliance_config_integrity_recovery_plan(
              CONFIG_JOURNAL_OK, false, true) ==
              APPLIANCE_CONFIG_INTEGRITY_RECOVERY_KEEP_LAST_KNOWN_GOOD,
          "invalid newest record keeps last-known-good when available");
    CHECK(appliance_config_integrity_recovery_plan(
              CONFIG_JOURNAL_OK, false, false) ==
              APPLIANCE_CONFIG_INTEGRITY_RECOVERY_DEFAULT_INVALID,
          "invalid newest record without known-good uses safe default");
    CHECK(appliance_config_integrity_recovery_plan(
              CONFIG_JOURNAL_EMPTY, false, false) ==
              APPLIANCE_CONFIG_INTEGRITY_RECOVERY_DEFAULT_MISSING,
          "empty journal publishes default-missing recovery");
    CHECK(appliance_config_integrity_recovery_plan(
              CONFIG_JOURNAL_IO_ERROR, false, true) ==
              APPLIANCE_CONFIG_INTEGRITY_RECOVERY_EXPLICIT_ERROR,
          "journal I/O failure is explicit");
    CHECK(appliance_config_integrity_recovery_plan(
              CONFIG_JOURNAL_CORRUPT, false, true) ==
              APPLIANCE_CONFIG_INTEGRITY_RECOVERY_EXPLICIT_ERROR,
          "journal corruption is explicit");
    CHECK(appliance_config_integrity_recovery_plan(
              CONFIG_JOURNAL_FULL, false, true) ==
              APPLIANCE_CONFIG_INTEGRITY_RECOVERY_EXPLICIT_ERROR,
          "full journal is explicit");
    CHECK(appliance_config_integrity_recovery_plan(
              (enum config_journal_status)99U, false, true) ==
              APPLIANCE_CONFIG_INTEGRITY_RECOVERY_EXPLICIT_ERROR,
          "unknown journal status is explicit");
}

static void test_strings(void)
{
    CHECK(strcmp(appliance_config_integrity_record_kind_string(
                     APPLIANCE_CONFIG_INTEGRITY_KIND_SYSTEM_HOSTNAME),
                 "APPLIANCE_CONFIG_INTEGRITY_KIND_SYSTEM_HOSTNAME") == 0,
          "record kind string is exact");
    CHECK(appliance_config_integrity_record_kind_string(
              (enum appliance_config_integrity_record_kind)99U) == NULL,
          "undefined record kind string is rejected");
    CHECK(strcmp(appliance_config_integrity_recovery_string(
                     APPLIANCE_CONFIG_INTEGRITY_RECOVERY_KEEP_LAST_KNOWN_GOOD),
                 "APPLIANCE_CONFIG_INTEGRITY_RECOVERY_KEEP_LAST_KNOWN_GOOD") ==
              0,
          "recovery string is exact");
    CHECK(appliance_config_integrity_recovery_string(
              (enum appliance_config_integrity_recovery)99U) == NULL,
          "undefined recovery string is rejected");
    CHECK(strcmp(appliance_config_integrity_status_string(
                     APPLIANCE_CONFIG_INTEGRITY_UNKNOWN_KEY),
                 "APPLIANCE_CONFIG_INTEGRITY_UNKNOWN_KEY") == 0,
          "status string is exact");
    CHECK(appliance_config_integrity_status_string(
              (enum appliance_config_integrity_status)99U) == NULL,
          "undefined status string is rejected");
}

int main(void)
{
    test_catalogue_and_describe();
    test_classification();
    test_rejections();
    test_recovery_plan();
    test_strings();
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "configuration integrity performs no allocation");
    if (failures != 0) {
        fprintf(stderr,
                "Appliance configuration integrity host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Appliance configuration integrity host tests: ALL OK");
    return 0;
}

#endif
