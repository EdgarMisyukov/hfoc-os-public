#ifdef HFOC_CONFIG_JOURNAL_QEMU_TEST

#include "block_device.h"
#include "config_journal.h"
#include "data_region.h"
#include "serial.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CONFIG_JOURNAL_QEMU_BLOCK_SIZE 512U
#define CONFIG_JOURNAL_QEMU_BLOCK_COUNT 8U
#define CONFIG_JOURNAL_QEMU_CAPACITY \
    (CONFIG_JOURNAL_QEMU_BLOCK_SIZE * CONFIG_JOURNAL_QEMU_BLOCK_COUNT)
#define CONFIG_JOURNAL_QEMU_REGION_OFFSET 1U
#define CONFIG_JOURNAL_QEMU_REGION_COUNT 7U

static unsigned char config_journal_qemu_storage[
    CONFIG_JOURNAL_QEMU_CAPACITY];
static bool config_journal_qemu_fail_write_next;
static bool config_journal_qemu_torn_write_next;

static void config_journal_qemu_zero(unsigned char *buffer, size_t length) {
    size_t index;

    for (index = 0U; index < length; ++index) {
        buffer[index] = 0U;
    }
}

static void config_journal_qemu_fill(unsigned char *buffer,
                                     unsigned char value,
                                     size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        buffer[index] = value;
    }
}

static void config_journal_qemu_copy(unsigned char *destination,
                                     const unsigned char *source,
                                     size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        destination[index] = source[index];
    }
}

static bool config_journal_qemu_equal(const unsigned char *left,
                                      const unsigned char *right,
                                      size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

static void config_journal_qemu_failure(const char *message)
    __attribute__((noreturn));

static void config_journal_qemu_failure(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[CONFIG-JOURNAL] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void config_journal_qemu_require(bool condition,
                                        const char *message)
{
    if (!condition) {
        config_journal_qemu_failure(message);
    }
}

static enum block_device_status config_journal_qemu_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    (void)device;
    config_journal_qemu_copy((unsigned char *)buffer,
                             config_journal_qemu_storage + (size_t)offset,
                             length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status config_journal_qemu_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    (void)device;
    if (config_journal_qemu_fail_write_next) {
        config_journal_qemu_fail_write_next = false;
        return BLOCK_DEVICE_IO_ERROR;
    }
    if (config_journal_qemu_torn_write_next) {
        config_journal_qemu_torn_write_next = false;
        config_journal_qemu_copy(config_journal_qemu_storage + (size_t)offset,
                                 (const unsigned char *)buffer,
                                 CONFIG_JOURNAL_COMMIT_OFFSET);
        return BLOCK_DEVICE_IO_ERROR;
    }
    config_journal_qemu_copy(config_journal_qemu_storage + (size_t)offset,
                             (const unsigned char *)buffer, length);
    return BLOCK_DEVICE_OK;
}

static void config_journal_qemu_store_le64(unsigned char *bytes,
                                           uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static void config_journal_qemu_write_interrupted_tail(void)
{
    static const unsigned char magic[CONFIG_JOURNAL_MAGIC_SIZE] =
        CONFIG_JOURNAL_MAGIC_BYTES;
    unsigned char block[CONFIG_JOURNAL_QEMU_BLOCK_SIZE];
    unsigned char digest[SHA256_DIGEST_SIZE];
    struct sha256 context;

    config_journal_qemu_zero(block, sizeof(block));
    config_journal_qemu_copy(block, magic, sizeof(magic));
    block[8U] = CONFIG_JOURNAL_FORMAT_VERSION;
    config_journal_qemu_store_le64(block + 16U, 4U);
    block[24U] = 4U;
    config_journal_qemu_copy(block + CONFIG_JOURNAL_DATA_OFFSET,
                             (const unsigned char *)"four", 4U);

    config_journal_qemu_require(
        sha256_init(&context) == SHA256_OK &&
            sha256_update(&context, block, 32U) == SHA256_OK &&
            sha256_update(&context,
                          block + CONFIG_JOURNAL_DATA_OFFSET, 4U) ==
                SHA256_OK &&
            sha256_final(&context, digest) == SHA256_OK,
        "markerless tail helper checksum");
    config_journal_qemu_copy(block + CONFIG_JOURNAL_CHECKSUM_OFFSET,
                             digest, SHA256_DIGEST_SIZE);
    config_journal_qemu_copy(
        config_journal_qemu_storage +
            (size_t)((CONFIG_JOURNAL_QEMU_REGION_OFFSET + 3U) *
                     CONFIG_JOURNAL_QEMU_BLOCK_SIZE),
        block, sizeof(block));
}

static void config_journal_qemu_init_data_region(void)
{
    static const unsigned char magic[8U] = { 'H', 'F', 'O', 'C',
                                              'D', 'A', 'T', 'A' };
    unsigned char *header = config_journal_qemu_storage;

    config_journal_qemu_zero(header, CONFIG_JOURNAL_QEMU_BLOCK_SIZE);
    config_journal_qemu_copy(header, magic, sizeof(magic));
    header[8U] = 1U;
    config_journal_qemu_store_le64(header + 16U,
                                   CONFIG_JOURNAL_QEMU_REGION_OFFSET);
    config_journal_qemu_store_le64(header + 24U,
                                   CONFIG_JOURNAL_QEMU_REGION_COUNT);
}

void config_journal_qemu_test_init(void)
{
    struct block_device device;
    struct data_region region;
    struct config_journal journal;
    unsigned char buffer[CONFIG_JOURNAL_DATA_MAX];
    size_t length = 0U;
    enum config_journal_status status;

    config_journal_qemu_zero(config_journal_qemu_storage,
                             sizeof(config_journal_qemu_storage));
    config_journal_qemu_init_data_region();

    config_journal_qemu_zero((unsigned char *)&device, sizeof(device));
    device.block_size = CONFIG_JOURNAL_QEMU_BLOCK_SIZE;
    device.block_count = CONFIG_JOURNAL_QEMU_BLOCK_COUNT;
    device.read_fn = config_journal_qemu_read;
    device.write_fn = config_journal_qemu_write;

    serial_write("[CONFIG-JOURNAL] proof init\n");

    config_journal_qemu_require(
        data_region_discover(&device, &region) == DATA_REGION_OK &&
            region.block_offset == CONFIG_JOURNAL_QEMU_REGION_OFFSET &&
            region.block_count == CONFIG_JOURNAL_QEMU_REGION_COUNT,
        "data-region discovery");
    config_journal_qemu_zero((unsigned char *)&journal, sizeof(journal));
    status = config_journal_init(&journal, &device, region.block_offset,
                                 region.block_count);
    config_journal_qemu_require(
        status == CONFIG_JOURNAL_OK &&
            journal.initialized &&
            journal.next_block == 0U &&
            journal.next_sequence == 1U,
        "empty journal initialization");
    serial_write("[CONFIG-JOURNAL] empty journal initialized\n");

    status = config_journal_append(&journal, "one", 3U);
    config_journal_qemu_require(
        status == CONFIG_JOURNAL_OK &&
            journal.next_block == 1U &&
            journal.next_sequence == 2U,
        "first append");
    serial_write("[CONFIG-JOURNAL] record 1 appended\n");

    status = config_journal_append(&journal, "two", 3U);
    config_journal_qemu_require(
        status == CONFIG_JOURNAL_OK &&
            journal.next_block == 2U &&
            journal.next_sequence == 3U,
        "second append");
    serial_write("[CONFIG-JOURNAL] record 2 appended\n");

    config_journal_qemu_fill(
        config_journal_qemu_storage +
            (size_t)((CONFIG_JOURNAL_QEMU_REGION_OFFSET + 2U) *
                     CONFIG_JOURNAL_QEMU_BLOCK_SIZE),
        0xA5, CONFIG_JOURNAL_QEMU_BLOCK_SIZE);
    config_journal_qemu_zero((unsigned char *)&journal, sizeof(journal));
    status = config_journal_init(&journal, &device, region.block_offset,
                                 region.block_count);
    config_journal_qemu_require(
        status == CONFIG_JOURNAL_OK &&
            journal.next_block == 2U &&
            journal.next_sequence == 3U,
        "interrupted tail recovery");
    serial_write("[CONFIG-JOURNAL] interrupted tail rejected\n");

    status = config_journal_head(&journal, buffer, sizeof(buffer), &length);
    config_journal_qemu_require(
        status == CONFIG_JOURNAL_OK &&
            length == 3U &&
            config_journal_qemu_equal(buffer,
                                      (const unsigned char *)"two", 3U),
        "recovered head");
    serial_write("[CONFIG-JOURNAL] head recovered\n");

    status = config_journal_append(&journal, "three", 5U);
    config_journal_qemu_require(
        status == CONFIG_JOURNAL_OK &&
            journal.next_block == 3U &&
            journal.next_sequence == 4U,
        "interrupted tail overwrite");
    serial_write("[CONFIG-JOURNAL] interrupted tail overwritten\n");

    config_journal_qemu_torn_write_next = true;
    status = config_journal_append(&journal, "four", 4U);
    config_journal_qemu_require(
        status == CONFIG_JOURNAL_IO_ERROR &&
            journal.next_block == 3U &&
            journal.next_sequence == 4U,
        "torn append failure");
    serial_write("[CONFIG-JOURNAL] atomic torn write rejected\n");

    status = config_journal_head(&journal, buffer, sizeof(buffer), &length);
    config_journal_qemu_require(
        status == CONFIG_JOURNAL_OK &&
            length == 5U &&
            config_journal_qemu_equal(buffer,
                                      (const unsigned char *)"three", 5U),
        "head after torn write");
    serial_write("[CONFIG-JOURNAL] previous head readable after torn write\n");

    config_journal_qemu_fail_write_next = true;
    status = config_journal_append(&journal, "four", 4U);
    config_journal_qemu_require(
        status == CONFIG_JOURNAL_IO_ERROR &&
            journal.next_block == 3U &&
            journal.next_sequence == 4U,
        "failed flush");
    serial_write("[CONFIG-JOURNAL] atomic failed flush rejected\n");

    status = config_journal_head(&journal, buffer, sizeof(buffer), &length);
    config_journal_qemu_require(
        status == CONFIG_JOURNAL_OK &&
            length == 5U &&
            config_journal_qemu_equal(buffer,
                                      (const unsigned char *)"three", 5U),
        "head after failed flush");
    serial_write(
        "[CONFIG-JOURNAL] previous head readable after failed flush\n");

    config_journal_qemu_write_interrupted_tail();
    config_journal_qemu_zero((unsigned char *)&journal, sizeof(journal));
    status = config_journal_init(&journal, &device, region.block_offset,
                                 region.block_count);
    config_journal_qemu_require(
        status == CONFIG_JOURNAL_OK &&
            journal.next_block == 3U &&
            journal.next_sequence == 4U,
        "markerless interrupted commit recovery");
    serial_write("[CONFIG-JOURNAL] interrupted commit marker ignored\n");

    status = config_journal_append(&journal, "four", 4U);
    config_journal_qemu_require(
        status == CONFIG_JOURNAL_OK &&
            journal.next_block == 4U &&
            journal.next_sequence == 5U,
        "atomic retry commit");
    serial_write("[CONFIG-JOURNAL] atomic retry committed\n");

    serial_write("[CONFIG-JOURNAL] clean shutdown\n");
    serial_write("[CONFIG-JOURNAL] proof OK\n");
}

#else
/* Keep this test-only translation unit non-empty in normal kernel builds. */
int hfoc_config_journal_qemu_translation_unit_marker;
#endif
