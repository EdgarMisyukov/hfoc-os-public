#include "appliance_persistent_config.h"
#include "block_device.h"
#include "config_journal.h"
#include "data_region.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_PERSISTENT_CONFIG_HOST_TEST

#define TEST_BLOCK_SIZE 512U
#define TEST_BLOCK_COUNT 8U
#define TEST_CAPACITY (TEST_BLOCK_SIZE * TEST_BLOCK_COUNT)
#define TEST_REGION_OFFSET 1U
#define TEST_REGION_COUNT 7U

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

struct fixture_device {
    unsigned char storage[TEST_CAPACITY];
    bool fail_write;
    size_t read_calls;
    size_t write_calls;
};

static void zero_bytes(void *destination, size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void copy_bytes(void *destination,
                       const void *source,
                       size_t length)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    size_t index;

    for (index = 0U; index < length; ++index) {
        out[index] = in[index];
    }
}

static void store_le64(unsigned char *bytes, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static void init_fixture(struct fixture_device *fixture)
{
    static const unsigned char magic[DATA_REGION_MAGIC_SIZE] =
        DATA_REGION_MAGIC_BYTES;

    zero_bytes(fixture, sizeof(*fixture));
    copy_bytes(fixture->storage, magic, sizeof(magic));
    fixture->storage[8U] = DATA_REGION_FORMAT_VERSION;
    store_le64(fixture->storage + 16U, TEST_REGION_OFFSET);
    store_le64(fixture->storage + 24U, TEST_REGION_COUNT);
}

static enum block_device_status fixture_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct fixture_device *fixture =
        (struct fixture_device *)device->context;

    ++fixture->read_calls;
    copy_bytes(buffer, fixture->storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct fixture_device *fixture =
        (struct fixture_device *)device->context;

    ++fixture->write_calls;
    if (fixture->fail_write) {
        fixture->fail_write = false;
        return BLOCK_DEVICE_IO_ERROR;
    }
    copy_bytes(fixture->storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static struct block_device make_device(struct fixture_device *fixture)
{
    struct block_device device;

    zero_bytes(&device, sizeof(device));
    device.block_size = TEST_BLOCK_SIZE;
    device.block_count = TEST_BLOCK_COUNT;
    device.context = fixture;
    device.read_fn = fixture_read;
    device.write_fn = fixture_write;
    return device;
}

static bool hostname_is(const struct appliance_persistent_config *config,
                        const char *expected)
{
    char output[APPLIANCE_PERSISTENT_CONFIG_HOSTNAME_MAX + 1U];
    size_t length = 0U;
    size_t expected_length = strlen(expected);

    return config != NULL &&
           appliance_persistent_config_hostname(
               config, output, sizeof(output), &length) ==
               APPLIANCE_PERSISTENT_CONFIG_OK &&
           length == expected_length &&
           memcmp(output, expected, expected_length) == 0;
}

static bool language_is(const struct appliance_persistent_config *config,
                        enum appliance_language expected)
{
    enum appliance_language language = APPLIANCE_LANGUAGE_COUNT;

    return config != NULL &&
           appliance_persistent_config_language(config, &language) ==
               APPLIANCE_PERSISTENT_CONFIG_OK &&
           language == expected;
}

static bool head_is(const struct appliance_persistent_config *config,
                    const char *expected)
{
    unsigned char record[CONFIG_JOURNAL_DATA_MAX];
    size_t length = 0U;
    size_t expected_length = strlen(expected);

    return config != NULL &&
           config_journal_head(
               &config->journal, record, sizeof(record), &length) ==
               CONFIG_JOURNAL_OK &&
           length == expected_length &&
           memcmp(record, expected, expected_length) == 0;
}

static enum appliance_persistent_config_status load_fresh(
    struct appliance_persistent_config *config,
    struct block_device *device)
{
    zero_bytes(config, sizeof(*config));
    return appliance_persistent_config_load(config, device);
}

static void test_status_strings(void)
{
    static const enum appliance_persistent_config_status statuses[] = {
        APPLIANCE_PERSISTENT_CONFIG_OK,
        APPLIANCE_PERSISTENT_CONFIG_NULL,
        APPLIANCE_PERSISTENT_CONFIG_BAD_STATE,
        APPLIANCE_PERSISTENT_CONFIG_BAD_DEVICE,
        APPLIANCE_PERSISTENT_CONFIG_IO_ERROR,
        APPLIANCE_PERSISTENT_CONFIG_CORRUPT,
        APPLIANCE_PERSISTENT_CONFIG_FULL,
        APPLIANCE_PERSISTENT_CONFIG_BAD_RECORD,
        APPLIANCE_PERSISTENT_CONFIG_BAD_HOSTNAME,
        APPLIANCE_PERSISTENT_CONFIG_BUFFER_TOO_SMALL,
        APPLIANCE_PERSISTENT_CONFIG_BAD_LANGUAGE,
        APPLIANCE_PERSISTENT_CONFIG_BAD_PANEL_AFTER_BOOT,
    };
    const char *expected[] = {
        "APPLIANCE_PERSISTENT_CONFIG_OK",
        "APPLIANCE_PERSISTENT_CONFIG_NULL",
        "APPLIANCE_PERSISTENT_CONFIG_BAD_STATE",
        "APPLIANCE_PERSISTENT_CONFIG_BAD_DEVICE",
        "APPLIANCE_PERSISTENT_CONFIG_IO_ERROR",
        "APPLIANCE_PERSISTENT_CONFIG_CORRUPT",
        "APPLIANCE_PERSISTENT_CONFIG_FULL",
        "APPLIANCE_PERSISTENT_CONFIG_BAD_RECORD",
        "APPLIANCE_PERSISTENT_CONFIG_BAD_HOSTNAME",
        "APPLIANCE_PERSISTENT_CONFIG_BUFFER_TOO_SMALL",
        "APPLIANCE_PERSISTENT_CONFIG_BAD_LANGUAGE",
        "APPLIANCE_PERSISTENT_CONFIG_BAD_PANEL_AFTER_BOOT",
    };
    size_t index;

    CHECK(sizeof(statuses) / sizeof(statuses[0]) ==
              sizeof(expected) / sizeof(expected[0]),
          "persistent config status table is complete");
    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        CHECK(strcmp(appliance_persistent_config_status_string(
                         statuses[index]),
                     expected[index]) == 0,
              "persistent config status string is exact");
    }
    CHECK(appliance_persistent_config_status_string(
              (enum appliance_persistent_config_status)12U) == NULL,
          "persistent config rejects undefined status string");
}

static void test_load_default(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_config config;

    init_fixture(&fixture);
    device = make_device(&fixture);

    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
              hostname_is(&config,
                          APPLIANCE_PERSISTENT_CONFIG_DEFAULT_HOSTNAME),
          "empty persistent config region publishes safe default");
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
              hostname_is(&config,
                          APPLIANCE_PERSISTENT_CONFIG_DEFAULT_HOSTNAME),
          "repeated load from empty region remains deterministic");
}

static void test_null_and_bad_state(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_config config;

    init_fixture(&fixture);
    device = make_device(&fixture);

    CHECK(appliance_persistent_config_load(
              NULL, &device) == APPLIANCE_PERSISTENT_CONFIG_NULL,
          "persistent config load rejects null config");
    CHECK(appliance_persistent_config_load(
              &config, NULL) == APPLIANCE_PERSISTENT_CONFIG_NULL,
          "persistent config load rejects null device");
    CHECK(appliance_persistent_config_set_hostname(
              NULL, "edge-a", 6U) == APPLIANCE_PERSISTENT_CONFIG_NULL,
          "persistent config set rejects null config");
    CHECK(appliance_persistent_config_set_hostname(
              &config, NULL, 6U) == APPLIANCE_PERSISTENT_CONFIG_NULL,
          "persistent config set rejects null hostname");
    CHECK(appliance_persistent_config_hostname(
              NULL, NULL, 0U, NULL) == APPLIANCE_PERSISTENT_CONFIG_NULL,
          "persistent config hostname rejects null arguments");

    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK,
          "persistent config load publishes safe default");
    CHECK(appliance_persistent_config_load(
              &config, &device) == APPLIANCE_PERSISTENT_CONFIG_BAD_STATE,
          "persistent config rejects second load without clearing");
    CHECK(appliance_persistent_config_hostname(
              &config, NULL, 1U, NULL) ==
              APPLIANCE_PERSISTENT_CONFIG_NULL,
          "persistent config hostname rejects null output");
}

static void test_hostname_validation(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_config config;
    size_t write_calls;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK,
          "persistent config load initializes validation fixture");
    write_calls = fixture.write_calls;

    CHECK(appliance_persistent_config_set_hostname(
              &config, "", 0U) == APPLIANCE_PERSISTENT_CONFIG_BAD_HOSTNAME,
          "persistent config rejects empty hostname");
    CHECK(appliance_persistent_config_set_hostname(
              &config, "-edge", 5U) ==
              APPLIANCE_PERSISTENT_CONFIG_BAD_HOSTNAME,
          "persistent config rejects leading hyphen");
    CHECK(appliance_persistent_config_set_hostname(
              &config, "edge-", 5U) ==
              APPLIANCE_PERSISTENT_CONFIG_BAD_HOSTNAME,
          "persistent config rejects trailing hyphen");
    CHECK(appliance_persistent_config_set_hostname(
              &config, "edge_a", 6U) ==
              APPLIANCE_PERSISTENT_CONFIG_BAD_HOSTNAME,
          "persistent config rejects underscore hostname");

    {
        char too_long[APPLIANCE_PERSISTENT_CONFIG_HOSTNAME_MAX + 2U];

        memset(too_long, 'a', sizeof(too_long));
        CHECK(appliance_persistent_config_set_hostname(
                  &config, too_long, sizeof(too_long)) ==
                  APPLIANCE_PERSISTENT_CONFIG_BAD_HOSTNAME,
              "persistent config rejects over-limit hostname");
    }

    CHECK(fixture.write_calls == write_calls &&
              hostname_is(&config,
                          APPLIANCE_PERSISTENT_CONFIG_DEFAULT_HOSTNAME),
          "invalid persistent config hostname causes no write or mutation");
}

static void test_set_and_reload(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_config config;
    size_t write_calls;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK,
          "persistent config set fixture load initializes");

    CHECK(appliance_persistent_config_set_hostname(
              &config, "edge-a", 6U) == APPLIANCE_PERSISTENT_CONFIG_OK &&
              hostname_is(&config, "edge-a"),
          "persistent config set publishes new hostname");
    write_calls = fixture.write_calls;
    CHECK(appliance_persistent_config_set_hostname(
              &config, "edge-a", 6U) == APPLIANCE_PERSISTENT_CONFIG_OK &&
              fixture.write_calls == write_calls &&
              hostname_is(&config, "edge-a"),
          "persistent config same-hostname set is a no-op");

    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
              hostname_is(&config, "edge-a"),
          "fresh persistent config load reads committed hostname");
}

static void test_failed_append_preserves_value(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_config config;
    size_t write_calls;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK,
          "persistent config failed-append load initializes");
    CHECK(appliance_persistent_config_set_hostname(
              &config, "edge-a", 6U) == APPLIANCE_PERSISTENT_CONFIG_OK,
          "persistent config failed-append fixture set succeeds");

    fixture.fail_write = true;
    write_calls = fixture.write_calls;
    CHECK(appliance_persistent_config_set_hostname(
              &config, "broken-b", 8U) ==
              APPLIANCE_PERSISTENT_CONFIG_IO_ERROR &&
              fixture.write_calls == write_calls + 1U &&
              hostname_is(&config, "edge-a"),
          "persistent config failed append preserves previous hostname");

    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
              hostname_is(&config, "edge-a"),
          "persistent config reload after failed append reads previous value");
}

static void test_invalid_record_recovery(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_config config;
    static const char invalid_value[] =
        "v1:system.hostname=bad_name\n";
    const size_t default_length =
        sizeof(APPLIANCE_PERSISTENT_CONFIG_DEFAULT_HOSTNAME) - 1U;
    size_t write_calls;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK,
          "persistent config invalid-record fixture load initializes");
    write_calls = fixture.write_calls;
    CHECK(config_journal_append(
              &config.journal, invalid_value,
              sizeof(invalid_value) - 1U) == CONFIG_JOURNAL_OK &&
              fixture.write_calls == write_calls + 1U,
          "persistent config invalid-record fixture appends malformed value");

    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
              appliance_persistent_config_source(&config) ==
                  APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_INVALID &&
              hostname_is(
                  &config,
                  APPLIANCE_PERSISTENT_CONFIG_DEFAULT_HOSTNAME),
          "persistent config invalid record recovers to safe default");
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
              appliance_persistent_config_source(&config) ==
                  APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_INVALID &&
              hostname_is(
                  &config,
                  APPLIANCE_PERSISTENT_CONFIG_DEFAULT_HOSTNAME),
          "persistent config invalid-record recovery is deterministic");

    write_calls = fixture.write_calls;
    CHECK(appliance_persistent_config_set_hostname(
              &config,
              APPLIANCE_PERSISTENT_CONFIG_DEFAULT_HOSTNAME,
              default_length) == APPLIANCE_PERSISTENT_CONFIG_OK &&
              fixture.write_calls == write_calls + 1U &&
              appliance_persistent_config_source(&config) ==
                  APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED &&
              hostname_is(
                  &config,
                  APPLIANCE_PERSISTENT_CONFIG_DEFAULT_HOSTNAME),
          "persistent config recovery write persists safe default");
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
              appliance_persistent_config_source(&config) ==
                  APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED &&
              hostname_is(
                  &config,
                  APPLIANCE_PERSISTENT_CONFIG_DEFAULT_HOSTNAME),
          "fresh load after recovery reads committed safe default");
}

static void test_hostname_copy_bounds(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_config config;
    char small[1] = {'K'};
    size_t length = 99U;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK,
          "persistent config hostname bounds fixture load initializes");
    CHECK(appliance_persistent_config_hostname(
              &config, small, sizeof(small), &length) ==
              APPLIANCE_PERSISTENT_CONFIG_BUFFER_TOO_SMALL &&
              small[0] == 'K' && length == 99U,
          "persistent config hostname copy rejects small buffer unchanged");
}

/*
 * ITEM-509 increment 2: the durable language shares the system record.  A
 * record that never chose a language keeps the exact bytes of the previous
 * format, and an old-format record still reads with the default language.
 */
static void test_language_round_trip(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_config config;
    size_t write_calls;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
              language_is(&config, APPLIANCE_LANGUAGE_EN),
          "empty persistent config publishes the default language");

    CHECK(appliance_persistent_config_set_language(
              &config, APPLIANCE_LANGUAGE_RU) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
              language_is(&config, APPLIANCE_LANGUAGE_RU) &&
              head_is(&config,
                      "v1:system.hostname=hfoc-appliance\n"
                      "v1:system.language=ru\n"),
          "choosing Russian writes the language line");

    write_calls = fixture.write_calls;
    CHECK(appliance_persistent_config_set_language(
              &config, APPLIANCE_LANGUAGE_RU) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
              fixture.write_calls == write_calls,
          "same-language set is a no-op");

    CHECK(appliance_persistent_config_set_hostname(
              &config, "edge-a", 6U) == APPLIANCE_PERSISTENT_CONFIG_OK &&
              hostname_is(&config, "edge-a") &&
              language_is(&config, APPLIANCE_LANGUAGE_RU) &&
              head_is(&config,
                      "v1:system.hostname=edge-a\n"
                      "v1:system.language=ru\n"),
          "a hostname change keeps the chosen language in the record");

    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
              hostname_is(&config, "edge-a") &&
              language_is(&config, APPLIANCE_LANGUAGE_RU),
          "fresh load reads hostname and language together");

    CHECK(appliance_persistent_config_set_language(
              &config, APPLIANCE_LANGUAGE_EN) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
              language_is(&config, APPLIANCE_LANGUAGE_EN) &&
              head_is(&config, "v1:system.hostname=edge-a\n"),
          "returning to English writes the record of the previous format");

    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
              hostname_is(&config, "edge-a") &&
              language_is(&config, APPLIANCE_LANGUAGE_EN),
          "fresh load of the previous format reads the default language");
}

static void test_old_record_reads_as_default_language(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_config config;
    static const char old_record[] = "v1:system.hostname=legacy-1\n";

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK,
          "persistent config legacy fixture load initializes");
    CHECK(config_journal_append(
              &config.journal, old_record, sizeof(old_record) - 1U) ==
              CONFIG_JOURNAL_OK,
          "persistent config legacy fixture appends the old format");

    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
              appliance_persistent_config_source(&config) ==
                  APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED &&
              hostname_is(&config, "legacy-1") &&
              language_is(&config, APPLIANCE_LANGUAGE_EN),
          "an old-format record keeps its hostname and reads as English");
}

/*
 * ITEM-516: the durable management gate is the optional third key of the same
 * record.  The default `closed` is never written - the record of a machine
 * that never enabled the policy is byte-identical to the record of the
 * previous format - and `open` is one durable line that the other two setters
 * carry forward.
 */
static void test_panel_after_boot_round_trip(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_config config;
    bool open = true;
    size_t write_calls;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
              appliance_persistent_config_panel_after_boot(&config, &open) ==
                  APPLIANCE_PERSISTENT_CONFIG_OK &&
              open == false &&
              appliance_persistent_config_source(&config) ==
                  APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING,
          "an empty store reads closed and writes no key");

    /*
     * The store has no record yet, so the first write materializes the
     * default hostname line exactly as `set_language(en)` does; the policy
     * itself adds no key.
     */
    write_calls = fixture.write_calls;
    CHECK(appliance_persistent_config_set_panel_after_boot(&config, false) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
              fixture.write_calls == write_calls + 1U &&
              appliance_persistent_config_panel_after_boot(&config, &open) ==
                  APPLIANCE_PERSISTENT_CONFIG_OK &&
              open == false &&
              head_is(&config, "v1:system.hostname=hfoc-appliance\n"),
          "writing the default policy materializes the record without a key");

    write_calls = fixture.write_calls;
    CHECK(appliance_persistent_config_set_panel_after_boot(&config, false) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
              fixture.write_calls == write_calls,
          "repeating the default closed policy is a no-op");

    CHECK(appliance_persistent_config_set_panel_after_boot(&config, true) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
              appliance_persistent_config_panel_after_boot(&config, &open) ==
                  APPLIANCE_PERSISTENT_CONFIG_OK &&
              open == true &&
              head_is(&config,
                      "v1:system.hostname=hfoc-appliance\n"
                      "v1:security.panel.after-boot=open\n"),
          "enabling the policy writes the one open line");

    write_calls = fixture.write_calls;
    CHECK(appliance_persistent_config_set_panel_after_boot(&config, true) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
              fixture.write_calls == write_calls,
          "enabling an already open policy is a no-op");

    CHECK(appliance_persistent_config_set_hostname(
              &config, "edge-a", 6U) == APPLIANCE_PERSISTENT_CONFIG_OK &&
              appliance_persistent_config_panel_after_boot(&config, &open) ==
                  APPLIANCE_PERSISTENT_CONFIG_OK &&
              open == true &&
              head_is(&config,
                      "v1:system.hostname=edge-a\n"
                      "v1:security.panel.after-boot=open\n"),
          "a hostname change keeps the enabled policy in the record");

    CHECK(appliance_persistent_config_set_language(
              &config, APPLIANCE_LANGUAGE_RU) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
              language_is(&config, APPLIANCE_LANGUAGE_RU) &&
              appliance_persistent_config_panel_after_boot(&config, &open) ==
                  APPLIANCE_PERSISTENT_CONFIG_OK &&
              open == true &&
              head_is(&config,
                      "v1:system.hostname=edge-a\n"
                      "v1:system.language=ru\n"
                      "v1:security.panel.after-boot=open\n"),
          "a language change keeps the enabled policy in the record");

    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
              hostname_is(&config, "edge-a") &&
              language_is(&config, APPLIANCE_LANGUAGE_RU) &&
              appliance_persistent_config_panel_after_boot(&config, &open) ==
                  APPLIANCE_PERSISTENT_CONFIG_OK &&
              open == true,
          "fresh load reads hostname, language and policy together");

    CHECK(appliance_persistent_config_set_panel_after_boot(&config, false) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
              appliance_persistent_config_panel_after_boot(&config, &open) ==
                  APPLIANCE_PERSISTENT_CONFIG_OK &&
              open == false &&
              head_is(&config,
                      "v1:system.hostname=edge-a\n"
                      "v1:system.language=ru\n"),
          "disabling the policy writes the record without the key");

    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
              hostname_is(&config, "edge-a") &&
              language_is(&config, APPLIANCE_LANGUAGE_RU) &&
              appliance_persistent_config_panel_after_boot(&config, &open) ==
                  APPLIANCE_PERSISTENT_CONFIG_OK &&
              open == false,
          "fresh load of the record without the key reads closed");
}

static void test_panel_after_boot_old_record_and_refusals(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_config config;
    static const char open_record[] =
        "v1:system.hostname=legacy-1\n"
        "v1:security.panel.after-boot=open\n";
    static const char bad_record[] =
        "v1:system.hostname=legacy-1\n"
        "v1:security.panel.after-boot=maybe\n";
    bool open = true;
    size_t write_calls;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK,
          "persistent config policy fixture load initializes");
    CHECK(config_journal_append(
              &config.journal, open_record, sizeof(open_record) - 1U) ==
              CONFIG_JOURNAL_OK,
          "persistent config policy fixture appends the open record");

    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
              hostname_is(&config, "legacy-1") &&
              language_is(&config, APPLIANCE_LANGUAGE_EN) &&
              appliance_persistent_config_panel_after_boot(&config, &open) ==
                  APPLIANCE_PERSISTENT_CONFIG_OK &&
              open == true,
          "an open record without a language line reads the default language");

    write_calls = fixture.write_calls;
    CHECK(config_journal_append(
              &config.journal, bad_record, sizeof(bad_record) - 1U) ==
              CONFIG_JOURNAL_OK,
          "persistent config policy fixture appends the bad record");
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK &&
              appliance_persistent_config_source(&config) ==
                  APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_INVALID &&
              appliance_persistent_config_panel_after_boot(&config, &open) ==
                  APPLIANCE_PERSISTENT_CONFIG_OK &&
              open == false,
          "an unknown policy value recovers to the closed default");

    write_calls = fixture.write_calls;
    CHECK(appliance_persistent_config_set_panel_after_boot(
              NULL, true) == APPLIANCE_PERSISTENT_CONFIG_NULL &&
              appliance_persistent_config_panel_after_boot(NULL, &open) ==
                  APPLIANCE_PERSISTENT_CONFIG_NULL &&
              appliance_persistent_config_panel_after_boot(&config, NULL) ==
                  APPLIANCE_PERSISTENT_CONFIG_NULL &&
              appliance_persistent_config_set_panel_after_boot(
                  &config, true) == APPLIANCE_PERSISTENT_CONFIG_OK &&
              fixture.write_calls == write_calls + 1U,
          "policy setters refuse null arguments without a write");
}

static void test_language_refusals(void)
{
    struct fixture_device fixture;
    struct block_device device;
    struct appliance_persistent_config config;
    size_t write_calls;

    init_fixture(&fixture);
    device = make_device(&fixture);
    CHECK(load_fresh(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK,
          "persistent config language fixture load initializes");
    write_calls = fixture.write_calls;

    CHECK(appliance_persistent_config_set_language(
              NULL, APPLIANCE_LANGUAGE_RU) ==
              APPLIANCE_PERSISTENT_CONFIG_NULL,
          "persistent config language set rejects null config");
    CHECK(appliance_persistent_config_language(NULL, NULL) ==
              APPLIANCE_PERSISTENT_CONFIG_NULL,
          "persistent config language getter rejects null arguments");
    CHECK(appliance_persistent_config_set_language(
              &config, (enum appliance_language)7) ==
              APPLIANCE_PERSISTENT_CONFIG_BAD_LANGUAGE &&
              fixture.write_calls == write_calls &&
              language_is(&config, APPLIANCE_LANGUAGE_EN),
          "persistent config refuses an unknown language without a write");
}

int main(void)
{
    test_status_strings();
    test_load_default();
    test_null_and_bad_state();
    test_hostname_validation();
    test_set_and_reload();
    test_failed_append_preserves_value();
    test_invalid_record_recovery();
    test_hostname_copy_bounds();
    test_language_round_trip();
    test_old_record_reads_as_default_language();
    test_panel_after_boot_round_trip();
    test_panel_after_boot_old_record_and_refusals();
    test_language_refusals();
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "persistent config performs no allocation");
    if (failures != 0) {
        fprintf(stderr,
                "Appliance persistent config host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Appliance persistent config host tests: ALL OK");
    return 0;
}

#endif
