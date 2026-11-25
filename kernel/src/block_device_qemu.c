#ifdef HFOC_BLOCK_DEVICE_QEMU_TEST

#include "block_device.h"
#include "kstring.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLOCK_DEVICE_QEMU_BLOCK_SIZE 512U
#define BLOCK_DEVICE_QEMU_BLOCK_COUNT 2U
#define BLOCK_DEVICE_QEMU_CAPACITY \
    (BLOCK_DEVICE_QEMU_BLOCK_SIZE * BLOCK_DEVICE_QEMU_BLOCK_COUNT)

static unsigned char block_device_qemu_storage[BLOCK_DEVICE_QEMU_CAPACITY];
static bool block_device_qemu_fail_read;
static bool block_device_qemu_fail_write;
static bool block_device_qemu_fail_flush;
static size_t block_device_qemu_read_calls;
static size_t block_device_qemu_write_calls;
static size_t block_device_qemu_flush_calls;

static void block_device_qemu_failure(const char *message)
    __attribute__((noreturn));

static void block_device_qemu_failure(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[BLOCK-DEVICE] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void block_device_qemu_require(bool condition,
                                      const char *message)
{
    if (!condition) {
        block_device_qemu_failure(message);
    }
}

static enum block_device_status block_device_qemu_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    (void)device;
    ++block_device_qemu_read_calls;
    if (block_device_qemu_fail_read) {
        block_device_qemu_fail_read = false;
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(buffer, block_device_qemu_storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status block_device_qemu_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    (void)device;
    ++block_device_qemu_write_calls;
    if (block_device_qemu_fail_write) {
        block_device_qemu_fail_write = false;
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(block_device_qemu_storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status block_device_qemu_flush(
    struct block_device *device)
{
    (void)device;
    ++block_device_qemu_flush_calls;
    if (block_device_qemu_fail_flush) {
        block_device_qemu_fail_flush = false;
        return BLOCK_DEVICE_IO_ERROR;
    }
    return BLOCK_DEVICE_OK;
}

static void block_device_qemu_init_storage(void)
{
    size_t index;

    for (index = 0U; index < BLOCK_DEVICE_QEMU_BLOCK_SIZE; ++index) {
        block_device_qemu_storage[index] = 0x11U;
    }
    for (index = BLOCK_DEVICE_QEMU_BLOCK_SIZE;
         index < BLOCK_DEVICE_QEMU_CAPACITY; ++index) {
        block_device_qemu_storage[index] = 0x22U;
    }
}

static bool block_device_qemu_all_bytes(const unsigned char *buffer,
                                        size_t length,
                                        unsigned char value)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (buffer[index] != value) {
            return false;
        }
    }
    return true;
}

void block_device_qemu_test_init(void)
{
    struct block_device device;
    unsigned char buffer[BLOCK_DEVICE_QEMU_BLOCK_SIZE];
    unsigned char write_block[BLOCK_DEVICE_QEMU_BLOCK_SIZE];
    enum block_device_status status;
    size_t index;

    block_device_qemu_init_storage();
    memset(&device, 0, sizeof(device));
    device.block_size = BLOCK_DEVICE_QEMU_BLOCK_SIZE;
    device.block_count = BLOCK_DEVICE_QEMU_BLOCK_COUNT;
    device.read_fn = block_device_qemu_read;
    device.write_fn = block_device_qemu_write;
    device.flush_fn = block_device_qemu_flush;

    serial_write("[BLOCK-DEVICE] proof init\n");

    memset(buffer, 0xA5, sizeof(buffer));
    status = block_device_read(&device, 256U, buffer, sizeof(buffer));
    block_device_qemu_require(
        status == BLOCK_DEVICE_UNALIGNED &&
            block_device_qemu_read_calls == 0U &&
            block_device_qemu_all_bytes(buffer, sizeof(buffer), 0xA5U),
        "alignment rejection");
    serial_write("[BLOCK-DEVICE] alignment rejected\n");

    memset(buffer, 0xA5, sizeof(buffer));
    status = block_device_read(
        &device, BLOCK_DEVICE_QEMU_CAPACITY, buffer, sizeof(buffer));
    block_device_qemu_require(
        status == BLOCK_DEVICE_OUT_OF_RANGE &&
            block_device_qemu_read_calls == 0U &&
            block_device_qemu_all_bytes(buffer, sizeof(buffer), 0xA5U),
        "range rejection");
    serial_write("[BLOCK-DEVICE] range rejected\n");

    memset(buffer, 0xA5, sizeof(buffer));
    block_device_qemu_fail_read = true;
    status = block_device_read(&device, 0U, buffer, sizeof(buffer));
    block_device_qemu_require(
        status == BLOCK_DEVICE_IO_ERROR &&
            !block_device_qemu_fail_read &&
            block_device_qemu_read_calls == 1U &&
            block_device_qemu_all_bytes(buffer, sizeof(buffer), 0xA5U),
        "injected read failure");
    status = block_device_read(&device, 0U, buffer, sizeof(buffer));
    block_device_qemu_require(
        status == BLOCK_DEVICE_OK &&
            block_device_qemu_read_calls == 2U &&
            block_device_qemu_all_bytes(buffer, sizeof(buffer), 0x11U),
        "read recovery");
    serial_write("[BLOCK-DEVICE] injected read failure\n");

    for (index = 0U; index < sizeof(write_block); ++index) {
        write_block[index] = 0x44U;
    }
    block_device_qemu_fail_write = true;
    status = block_device_write(&device, 0U, write_block, sizeof(write_block));
    block_device_qemu_require(
        status == BLOCK_DEVICE_IO_ERROR &&
            !block_device_qemu_fail_write &&
            block_device_qemu_write_calls == 1U &&
            block_device_qemu_all_bytes(block_device_qemu_storage,
                                        sizeof(write_block), 0x11U),
        "injected write failure");
    status = block_device_write(&device, 0U, write_block, sizeof(write_block));
    block_device_qemu_require(
        status == BLOCK_DEVICE_OK &&
            block_device_qemu_write_calls == 2U &&
            block_device_qemu_all_bytes(block_device_qemu_storage,
                                        sizeof(write_block), 0x44U),
        "write recovery");
    serial_write("[BLOCK-DEVICE] injected write failure\n");

    block_device_qemu_fail_flush = true;
    status = block_device_flush(&device);
    block_device_qemu_require(
        status == BLOCK_DEVICE_IO_ERROR &&
            !block_device_qemu_fail_flush &&
            block_device_qemu_flush_calls == 1U,
        "injected flush failure");
    status = block_device_flush(&device);
    block_device_qemu_require(
        status == BLOCK_DEVICE_OK &&
            block_device_qemu_flush_calls == 2U,
        "flush recovery");
    serial_write("[BLOCK-DEVICE] flush observed\n");

    serial_write("[BLOCK-DEVICE] clean shutdown\n");
    serial_write("[BLOCK-DEVICE] proof OK\n");
}

#else
/* Keep this test-only translation unit non-empty in normal kernel builds. */
int hfoc_block_device_qemu_translation_unit_marker;
#endif
