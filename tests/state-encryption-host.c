/*
 * ITEM-373 focused host proof: state encryption and key custody.
 *
 * The proof walks the whole boundary on RAM block devices: crypto known
 * answers, custody creation and failure modes, the sealed store adapter, an
 * encrypted install, and the production state loader asking for and rejecting
 * keys.  A wrong key never yields plaintext and never publishes a ready state.
 */

#ifdef HFOC_STATE_ENCRYPTION_HOST_TEST

#include "appliance_config_validation.h"
#include "appliance_key_custody.h"
#include "appliance_state_block_device.h"
#include "appliance_state_crypto.h"
#include "sha256.h"
#include "appliance_state_encryption.h"
#include "appliance_state_layout.h"
#include "appliance_state_loader.h"
#include "data_region.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The state-crypto Mbed TLS profile declares MBEDTLS_PLATFORM_ZEROIZE_ALT and
 * expects exactly one definition in the embedding image; the production kernel
 * takes the ITEM-372 one and this proof supplies its own. */
void mbedtls_platform_zeroize(void *buffer, size_t length)
{
    volatile unsigned char *bytes = (volatile unsigned char *)buffer;

    if (bytes == NULL) {
        return;
    }
    while (length > 0U) {
        *bytes = 0U;
        ++bytes;
        --length;
    }
}

#define HOST_BLOCK_SIZE 512U
#define HOST_DISK_BLOCK_COUNT 8192U
#define HOST_DISK_CAPACITY (HOST_BLOCK_SIZE * HOST_DISK_BLOCK_COUNT)
/*
 * ITEM-504 increment 6: the default layout grew to 512 blocks, and the seal
 * refuses a partition it does not fit in with a custody block to spare, so the
 * fixture models the real 32 MiB partition rather than the layout exactly.
 */
#define HOST_PARTITION_BLOCK_COUNT 640U
#define HOST_PARTITION_CAPACITY \
    (HOST_BLOCK_SIZE * HOST_PARTITION_BLOCK_COUNT)
#define HFOC_FIRST_LBA UINT64_C(2048)
#define HOST_MAX_STORES 8U

static int failures;
static int checks;

#define CHECK(condition, message)                                        \
    do {                                                                 \
        ++checks;                                                        \
        if (!(condition)) {                                              \
            ++failures;                                                  \
            fprintf(stderr, "FAIL: %s\n", (message));                    \
        }                                                                \
    } while (0)

static unsigned char disk_storage[HOST_DISK_CAPACITY];
static unsigned char partition_storage[HOST_PARTITION_CAPACITY];
static size_t disk_flush_calls;
static size_t partition_flush_calls;

static const unsigned char hfoc_guid[16U] = {
    0xE1U, 0x44U, 0x01U, 0x4CU, 0x0DU, 0x6EU, 0x7AU, 0x4BU,
    0xB4U, 0x9DU, 0x22U, 0x10U, 0x33U, 0x3DU, 0x41U, 0x31U
};

static const unsigned char test_passphrase[] =
    "correct horse battery staple";
static const unsigned char wrong_passphrase[] =
    "correct horse battery stapl3";

static bool host_find_bytes(
    const unsigned char *haystack,
    size_t haystack_length,
    const unsigned char *needle,
    size_t needle_length)
{
    size_t offset;

    if (needle_length == 0U || needle_length > haystack_length) {
        return false;
    }
    for (offset = 0U;
         offset <= haystack_length - needle_length; ++offset) {
        if (memcmp(haystack + offset, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static void host_le32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value & 0xFFU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xFFU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xFFU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xFFU);
}

static void host_le64(unsigned char *bytes, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static uint32_t host_crc32_update(
    uint32_t crc,
    const unsigned char *bytes,
    size_t length)
{
    size_t byte_index;

    for (byte_index = 0U; byte_index < length; ++byte_index) {
        unsigned int bit;

        crc ^= bytes[byte_index];
        for (bit = 0U; bit < 8U; ++bit) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1U) ^ 0xEDB88320U;
            } else {
                crc >>= 1U;
            }
        }
    }
    return crc;
}

static uint32_t host_crc32(const unsigned char *bytes, size_t length)
{
    return host_crc32_update(0xFFFFFFFFU, bytes, length) ^ 0xFFFFFFFFU;
}

/* --- RAM block devices -------------------------------------------------- */

static void disk_init(void)
{
    memset(disk_storage, 0, sizeof(disk_storage));
    disk_flush_calls = 0U;
}

static enum block_device_status disk_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    (void)device;
    if (offset > sizeof(disk_storage) ||
        length > sizeof(disk_storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(buffer, disk_storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status disk_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    (void)device;
    if (offset > sizeof(disk_storage) ||
        length > sizeof(disk_storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(disk_storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status disk_flush(struct block_device *device)
{
    (void)device;
    ++disk_flush_calls;
    return BLOCK_DEVICE_OK;
}

static struct block_device disk_device(void)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = HOST_BLOCK_SIZE;
    device.block_count = HOST_DISK_BLOCK_COUNT;
    device.read_fn = disk_read;
    device.write_fn = disk_write;
    device.flush_fn = disk_flush;
    return device;
}

static enum block_device_status partition_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    (void)device;
    if (offset > sizeof(partition_storage) ||
        length > sizeof(partition_storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(buffer, partition_storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status partition_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    (void)device;
    if (offset > sizeof(partition_storage) ||
        length > sizeof(partition_storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(partition_storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status partition_flush(
    struct block_device *device)
{
    (void)device;
    ++partition_flush_calls;
    return BLOCK_DEVICE_OK;
}

static struct block_device partition_device(void)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = HOST_BLOCK_SIZE;
    device.block_count = HOST_PARTITION_BLOCK_COUNT;
    device.read_fn = partition_read;
    device.write_fn = partition_write;
    device.flush_fn = partition_flush;
    return device;
}

/* --- GPT fixture (same shape the state loader consumes) ----------------- */

static void set_entry(
    uint32_t index,
    const unsigned char type_guid[16U],
    uint64_t first_lba,
    uint64_t last_lba)
{
    unsigned char *entry =
        disk_storage + 2U * HOST_BLOCK_SIZE +
        (uint64_t)index * 128U;

    if (type_guid != NULL) {
        memcpy(entry, type_guid, 16U);
    }
    host_le64(entry + 16U, index + 1U);
    host_le64(entry + 32U, first_lba);
    host_le64(entry + 40U, last_lba);
}

static void finalize_entries(void)
{
    unsigned char *header = disk_storage + HOST_BLOCK_SIZE;
    unsigned char copy[HOST_BLOCK_SIZE];
    uint32_t crc = 0xFFFFFFFFU;
    size_t index;

    for (index = 0U; index < 4U * 128U; ++index) {
        crc = host_crc32_update(
            crc,
            disk_storage + 2U * HOST_BLOCK_SIZE + index,
            1U);
    }
    host_le32(header + 88U, crc ^ 0xFFFFFFFFU);
    host_le32(header + 16U, 0U);
    memset(copy, 0, sizeof(copy));
    memcpy(copy, header, 92U);
    memset(copy + 16U, 0, 4U);
    host_le32(header + 16U, host_crc32(copy, 92U));
}

static void install_gpt(void)
{
    static const unsigned char signature[8U] = {
        'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'
    };
    unsigned char *header;
    unsigned char copy[HOST_BLOCK_SIZE];

    disk_init();
    header = disk_storage + HOST_BLOCK_SIZE;
    memcpy(header, signature, sizeof(signature));
    host_le32(header + 8U, UINT32_C(0x00010000));
    host_le32(header + 12U, 92U);
    host_le64(header + 24U, 1U);
    host_le64(header + 32U, HOST_DISK_BLOCK_COUNT - 1U);
    host_le64(header + 40U, 34U);
    host_le64(header + 48U, HOST_DISK_BLOCK_COUNT - 1U);
    host_le64(header + 72U, 2U);
    host_le32(header + 80U, 4U);
    host_le32(header + 84U, 128U);
    host_le32(header + 88U, 0U);
    memset(copy, 0, sizeof(copy));
    memcpy(copy, header, 92U);
    host_le32(header + 88U, host_crc32(copy, 92U));
    host_le32(header + 16U, 0U);
    memcpy(copy, header, 92U);
    memset(copy + 16U, 0, 4U);
    host_le32(header + 16U, host_crc32(copy, 92U));

    set_entry(0U, NULL, 34U, HFOC_FIRST_LBA - 1U);
    set_entry(
        1U, hfoc_guid, HFOC_FIRST_LBA,
        HFOC_FIRST_LBA + HOST_PARTITION_BLOCK_COUNT - 1U);
    finalize_entries();
}

/* Writes the plaintext data-region headers the legacy path expects. */
static void write_plaintext_state(
    struct block_device *partition,
    const struct appliance_state_layout *layout)
{
    unsigned char header[DATA_REGION_HEADER_BLOCK_SIZE];
    uint32_t index;

    for (index = 0U; index < layout->entry_count; ++index) {
        struct appliance_state_block_device store;

        if (appliance_state_block_device_bind(
                partition, &layout->entries[index], &store) !=
            APPLIANCE_STATE_BLOCK_DEVICE_OK) {
            return;
        }
        memset(header, 0, sizeof(header));
        memcpy(header, "HFOCDATA", 8U);
        header[8U] = DATA_REGION_FORMAT_VERSION;
        host_le64(header + 16U, 1U);
        host_le64(header + 24U, store.device.block_count - 1U);
        (void)block_device_write(
            &store.device, 0U, header, sizeof(header));
        (void)block_device_flush(&store.device);
    }
}

/* --- deterministic randomness ------------------------------------------ */

struct host_rng {
    uint32_t state;
};

static enum appliance_state_crypto_status host_random(
    void *context,
    uint8_t *output,
    size_t length)
{
    struct host_rng *rng = (struct host_rng *)context;
    size_t index;

    if (rng == NULL || (output == NULL && length != 0U)) {
        return APPLIANCE_STATE_CRYPTO_NULL;
    }
    for (index = 0U; index < length; ++index) {
        rng->state = rng->state * 1664525U + 1013904223U;
        output[index] = (uint8_t)(rng->state >> 24);
    }
    return APPLIANCE_STATE_CRYPTO_OK;
}

static enum appliance_state_crypto_status host_random_failure(
    void *context,
    uint8_t *output,
    size_t length)
{
    (void)context;
    (void)output;
    (void)length;
    return APPLIANCE_STATE_CRYPTO_RANDOM_FAILED;
}

static struct appliance_key_custody_material passphrase_material(void)
{
    struct appliance_key_custody_material material;

    memset(&material, 0, sizeof(material));
    material.binding = APPLIANCE_KEY_CUSTODY_BINDING_PASSPHRASE;
    material.secret = test_passphrase;
    material.secret_length = sizeof(test_passphrase) - 1U;
    return material;
}

static struct appliance_key_custody_material wrong_material(void)
{
    struct appliance_key_custody_material material;

    memset(&material, 0, sizeof(material));
    material.binding = APPLIANCE_KEY_CUSTODY_BINDING_PASSPHRASE;
    material.secret = wrong_passphrase;
    material.secret_length = sizeof(wrong_passphrase) - 1U;
    return material;
}

static struct appliance_key_custody_material recovery_material(
    const uint8_t key[APPLIANCE_KEY_CUSTODY_RECOVERY_KEY_LENGTH])
{
    struct appliance_key_custody_material material;

    memset(&material, 0, sizeof(material));
    material.binding = APPLIANCE_KEY_CUSTODY_BINDING_RECOVERY_KEY;
    material.secret = key;
    material.secret_length = APPLIANCE_KEY_CUSTODY_RECOVERY_KEY_LENGTH;
    return material;
}

static struct appliance_key_custody_material tpm_material(void)
{
    struct appliance_key_custody_material material;

    memset(&material, 0, sizeof(material));
    material.binding = APPLIANCE_KEY_CUSTODY_BINDING_TPM;
    return material;
}

static bool bytes_all_zero(const uint8_t *bytes, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

/* --- crypto known answers ----------------------------------------------- */

static void host_crypto_known_answers(void)
{
    static const uint8_t hmac_key_1[20U] = {
        0x0BU, 0x0BU, 0x0BU, 0x0BU, 0x0BU, 0x0BU, 0x0BU, 0x0BU,
        0x0BU, 0x0BU, 0x0BU, 0x0BU, 0x0BU, 0x0BU, 0x0BU, 0x0BU,
        0x0BU, 0x0BU, 0x0BU, 0x0BU
    };
    static const uint8_t hmac_expected_1[32U] = {
        0xB0U, 0x34U, 0x4CU, 0x61U, 0xD8U, 0xDBU, 0x38U, 0x53U,
        0x5CU, 0xA8U, 0xAFU, 0xCEU, 0xAFU, 0x0BU, 0xF1U, 0x2BU,
        0x88U, 0x1DU, 0xC2U, 0x00U, 0xC9U, 0x83U, 0x3DU, 0xA7U,
        0x26U, 0xE9U, 0x37U, 0x6CU, 0x2EU, 0x32U, 0xCFU, 0xF7U
    };
    static const uint8_t hmac_expected_2[32U] = {
        0x5BU, 0xDCU, 0xC1U, 0x46U, 0xBFU, 0x60U, 0x75U, 0x4EU,
        0x6AU, 0x04U, 0x24U, 0x26U, 0x08U, 0x95U, 0x75U, 0xC7U,
        0x5AU, 0x00U, 0x3FU, 0x08U, 0x9DU, 0x27U, 0x39U, 0x83U,
        0x9DU, 0xECU, 0x58U, 0xB9U, 0x64U, 0xECU, 0x38U, 0x43U
    };
    static const uint8_t hmac_expected_3[32U] = {
        0x60U, 0xE4U, 0x31U, 0x59U, 0x1EU, 0xE0U, 0xB6U, 0x7FU,
        0x0DU, 0x8AU, 0x26U, 0xAAU, 0xCBU, 0xF5U, 0xB7U, 0x7FU,
        0x8EU, 0x0BU, 0xC6U, 0x21U, 0x37U, 0x28U, 0xC5U, 0x14U,
        0x05U, 0x46U, 0x04U, 0x0FU, 0x0EU, 0xE3U, 0x7FU, 0x54U
    };
    static const uint8_t large_key[131U] = {
        0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU,
        0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU,
        0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU,
        0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU,
        0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU,
        0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU,
        0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU,
        0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU,
        0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU,
        0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU,
        0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU,
        0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU,
        0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU,
        0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU,
        0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU,
        0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU, 0xAAU,
        0xAAU, 0xAAU, 0xAAU
    };
    static const uint8_t large_message[] =
        "Test Using Larger Than Block-Size Key - Hash Key First";
    static const uint8_t salt[APPLIANCE_STATE_CRYPTO_SALT_LENGTH] = {
        0x73U, 0x61U, 0x6CU, 0x74U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U
    };
    static const uint8_t pbkdf2_expected_1[32U] = {
        0x32U, 0x67U, 0xA0U, 0x66U, 0x14U, 0xB9U, 0xD0U, 0x90U,
        0xBCU, 0x3EU, 0x68U, 0x4EU, 0xEBU, 0xDBU, 0x6AU, 0xF8U,
        0xEEU, 0x75U, 0x3CU, 0xF8U, 0x0BU, 0x7FU, 0x7FU, 0x7CU,
        0xF5U, 0xE6U, 0x76U, 0xC6U, 0x54U, 0x22U, 0xD0U, 0x54U
    };
    static const uint8_t pbkdf2_expected_2[32U] = {
        0x8DU, 0xB3U, 0x27U, 0x7EU, 0xD0U, 0x85U, 0xE8U, 0xABU,
        0x63U, 0xE4U, 0xFDU, 0xD5U, 0xC1U, 0x1EU, 0x6CU, 0x60U,
        0xBDU, 0x37U, 0x88U, 0xABU, 0x41U, 0x64U, 0xF2U, 0x5DU,
        0x63U, 0x49U, 0x78U, 0x6FU, 0xA6U, 0x26U, 0x20U, 0x23U
    };
    static const uint8_t pbkdf2_expected_4096[32U] = {
        0xD9U, 0x3DU, 0xE1U, 0xBFU, 0xE5U, 0x08U, 0x11U, 0xBBU,
        0xBBU, 0xBDU, 0x4FU, 0x1AU, 0x13U, 0x34U, 0xB2U, 0x43U,
        0x40U, 0xE3U, 0x6FU, 0x24U, 0x6DU, 0x38U, 0x17U, 0xB2U,
        0x32U, 0x4DU, 0x42U, 0xA1U, 0x2FU, 0x60U, 0x7CU, 0xF9U
    };
    static const uint8_t ctr_key[APPLIANCE_STATE_CRYPTO_KEY_LENGTH] = {
        0x60U, 0x3DU, 0xEBU, 0x10U, 0x15U, 0xCAU, 0x71U, 0xBEU,
        0x2BU, 0x73U, 0xAEU, 0xF0U, 0x85U, 0x7DU, 0x77U, 0x81U,
        0x1FU, 0x35U, 0x2CU, 0x07U, 0x3BU, 0x61U, 0x08U, 0xD7U,
        0x2DU, 0x98U, 0x10U, 0xA3U, 0x09U, 0x14U, 0xDFU, 0xF4U
    };
    static const uint8_t ctr_nonce[APPLIANCE_STATE_CRYPTO_NONCE_LENGTH] = {
        0xF0U, 0xF1U, 0xF2U, 0xF3U, 0xF4U, 0xF5U, 0xF6U, 0xF7U,
        0xF8U, 0xF9U, 0xFAU, 0xFBU, 0xFCU, 0xFDU, 0xFEU, 0xFFU
    };
    static const uint8_t ctr_plaintext[64U] = {
        0x6BU, 0xC1U, 0xBEU, 0xE2U, 0x2EU, 0x40U, 0x9FU, 0x96U,
        0xE9U, 0x3DU, 0x7EU, 0x11U, 0x73U, 0x93U, 0x17U, 0x2AU,
        0xAEU, 0x2DU, 0x8AU, 0x57U, 0x1EU, 0x03U, 0xACU, 0x9CU,
        0x9EU, 0xB7U, 0x6FU, 0xACU, 0x45U, 0xAFU, 0x8EU, 0x51U,
        0x30U, 0xC8U, 0x1CU, 0x46U, 0xA3U, 0x5CU, 0xE4U, 0x11U,
        0xE5U, 0xFBU, 0xC1U, 0x19U, 0x1AU, 0x0AU, 0x52U, 0xEFU,
        0xF6U, 0x9FU, 0x24U, 0x45U, 0xDFU, 0x4FU, 0x9BU, 0x17U,
        0xADU, 0x2BU, 0x41U, 0x7BU, 0xE6U, 0x6CU, 0x37U, 0x10U
    };
    static const uint8_t ctr_expected[64U] = {
        0x60U, 0x1EU, 0xC3U, 0x13U, 0x77U, 0x57U, 0x89U, 0xA5U,
        0xB7U, 0xA7U, 0xF5U, 0x04U, 0xBBU, 0xF3U, 0xD2U, 0x28U,
        0xF4U, 0x43U, 0xE3U, 0xCAU, 0x4DU, 0x62U, 0xB5U, 0x9AU,
        0xCAU, 0x84U, 0xE9U, 0x90U, 0xCAU, 0xCAU, 0xF5U, 0xC5U,
        0x2BU, 0x09U, 0x30U, 0xDAU, 0xA2U, 0x3DU, 0xE9U, 0x4CU,
        0xE8U, 0x70U, 0x17U, 0xBAU, 0x2DU, 0x84U, 0x98U, 0x8DU,
        0xDFU, 0xC9U, 0xC5U, 0x8DU, 0xB6U, 0x7AU, 0xADU, 0xA6U,
        0x13U, 0xC2U, 0xDDU, 0x08U, 0x45U, 0x79U, 0x41U, 0xA6U
    };
    uint8_t mac[APPLIANCE_STATE_CRYPTO_MAC_LENGTH];
    uint8_t output[APPLIANCE_STATE_CRYPTO_OUTPUT_MAX];
    uint8_t ctr_output[64U];
    uint8_t left[32U];
    uint8_t right[32U];

    memset(ctr_output, 0, sizeof(ctr_output));
    memset(left, 0, sizeof(left));
    memset(right, 0, sizeof(right));

    CHECK(appliance_state_crypto_hmac_sha256(
              hmac_key_1, sizeof(hmac_key_1), (const uint8_t *)"Hi There",
              8U, mac) == APPLIANCE_STATE_CRYPTO_OK,
          "HMAC-SHA-256 accepts a variable-length key");
    CHECK(memcmp(mac, hmac_expected_1, sizeof(mac)) == 0,
          "HMAC-SHA-256 matches RFC 4231 case 1");
    CHECK(appliance_state_crypto_hmac_sha256(
              (const uint8_t *)"Jefe", 4U,
              (const uint8_t *)"what do ya want for nothing?", 28U,
              mac) == APPLIANCE_STATE_CRYPTO_OK &&
              memcmp(mac, hmac_expected_2, sizeof(mac)) == 0,
          "HMAC-SHA-256 matches RFC 4231 case 2");
    CHECK(appliance_state_crypto_hmac_sha256(
              large_key, sizeof(large_key), large_message,
              sizeof(large_message) - 1U, mac) ==
              APPLIANCE_STATE_CRYPTO_OK &&
              memcmp(mac, hmac_expected_3, sizeof(mac)) == 0,
          "HMAC-SHA-256 hashes a key larger than the block size");
    CHECK(appliance_state_crypto_hmac_sha256(
              NULL, 0U, (const uint8_t *)"x", 1U, mac) ==
              APPLIANCE_STATE_CRYPTO_NULL,
          "HMAC-SHA-256 refuses a null key");

    CHECK(appliance_state_crypto_pbkdf2_sha256(
              (const uint8_t *)"password", 8U, salt, 1U, output,
              32U) == APPLIANCE_STATE_CRYPTO_OK &&
              memcmp(output, pbkdf2_expected_1, 32U) == 0,
          "PBKDF2-HMAC-SHA-256 matches the c=1 vector");
    CHECK(appliance_state_crypto_pbkdf2_sha256(
              (const uint8_t *)"password", 8U, salt, 2U, output,
              32U) == APPLIANCE_STATE_CRYPTO_OK &&
              memcmp(output, pbkdf2_expected_2, 32U) == 0,
          "PBKDF2-HMAC-SHA-256 matches the c=2 vector");
    CHECK(appliance_state_crypto_pbkdf2_sha256(
              (const uint8_t *)"password", 8U, salt, 4096U, output,
              32U) == APPLIANCE_STATE_CRYPTO_OK &&
              memcmp(output, pbkdf2_expected_4096, 32U) == 0,
          "PBKDF2-HMAC-SHA-256 matches the c=4096 vector");
    {
        static const uint8_t pbkdf2_expected_40[40U] = {
            0xD9U, 0x3DU, 0xE1U, 0xBFU, 0xE5U, 0x08U, 0x11U, 0xBBU,
            0xBBU, 0xBDU, 0x4FU, 0x1AU, 0x13U, 0x34U, 0xB2U, 0x43U,
            0x40U, 0xE3U, 0x6FU, 0x24U, 0x6DU, 0x38U, 0x17U, 0xB2U,
            0x32U, 0x4DU, 0x42U, 0xA1U, 0x2FU, 0x60U, 0x7CU, 0xF9U,
            0xF8U, 0x8AU, 0x9EU, 0x94U, 0x31U, 0x20U, 0x4EU, 0x73U
        };
        uint8_t output_40[40U];

        CHECK(appliance_state_crypto_pbkdf2_sha256(
                  (const uint8_t *)"password", 8U, salt, 4096U,
                  output_40, sizeof(output_40)) ==
                  APPLIANCE_STATE_CRYPTO_OK &&
                  memcmp(output_40, pbkdf2_expected_40,
                         sizeof(pbkdf2_expected_40)) == 0,
              "PBKDF2-HMAC-SHA-256 matches the multi-block vector");
    }
    {
        /*
         * ITEM-611: one prepared HMAC key per derivation.  A 20,000-iteration,
         * 64-byte derivation is two blocks, so the budget is the two pad
         * compressions plus two compressions per HMAC round.  The old path
         * hashed both pads on every round and roughly doubles this count.
         */
        uint8_t output_20k[64U];

        sha256_host_test_reset_compressions();
        CHECK(appliance_state_crypto_pbkdf2_sha256(
                  (const uint8_t *)"password", 8U, salt, 20000U,
                  output_20k, sizeof(output_20k)) ==
                  APPLIANCE_STATE_CRYPTO_OK,
              "PBKDF2 completes the 20,000-iteration seed derivation");
        CHECK(sha256_host_test_compressions() <= 81000ULL,
              "PBKDF2 prepares one HMAC key per derivation");
    }
    CHECK(appliance_state_crypto_pbkdf2_sha256(
              (const uint8_t *)"password", 8U, salt, 0U, output,
              32U) == APPLIANCE_STATE_CRYPTO_BAD_ITERATIONS,
          "PBKDF2 refuses an iteration count of zero");
    CHECK(appliance_state_crypto_pbkdf2_sha256(
              (const uint8_t *)"password", 8U, salt,
              APPLIANCE_STATE_CRYPTO_PBKDF2_MAX_ITERATIONS + 1U,
              output, 32U) == APPLIANCE_STATE_CRYPTO_BAD_ITERATIONS,
          "PBKDF2 refuses an unbounded iteration count");
    CHECK(appliance_state_crypto_pbkdf2_sha256(
              (const uint8_t *)"password", 8U, salt, 1U, output,
              APPLIANCE_STATE_CRYPTO_OUTPUT_MAX + 1U) ==
              APPLIANCE_STATE_CRYPTO_BAD_LENGTH,
          "PBKDF2 refuses an output beyond the bounded maximum");

    CHECK(appliance_state_crypto_ctr(
              ctr_key, ctr_nonce, ctr_plaintext, ctr_output,
              sizeof(ctr_plaintext)) == APPLIANCE_STATE_CRYPTO_OK &&
              memcmp(ctr_output, ctr_expected, sizeof(ctr_expected)) == 0,
          "AES-256-CTR matches the NIST SP 800-38A F.5.5 vector");
    memcpy(ctr_output, ctr_expected, 20U);
    CHECK(appliance_state_crypto_ctr(
              ctr_key, ctr_nonce, ctr_output, ctr_output, 20U) ==
              APPLIANCE_STATE_CRYPTO_OK &&
              memcmp(ctr_output, ctr_plaintext, 20U) == 0,
          "AES-256-CTR round-trips a length that is not a block multiple");

    left[0] = 0x5AU;
    right[0] = 0x5AU;
    CHECK(appliance_state_crypto_equal(left, right, 32U),
          "constant-time comparison accepts equal buffers");
    right[31] = 0x01U;
    CHECK(!appliance_state_crypto_equal(left, right, 32U),
          "constant-time comparison rejects a differing tail byte");
    CHECK(!appliance_state_crypto_equal(left, NULL, 32U),
          "constant-time comparison rejects a null operand");

    CHECK(appliance_state_crypto_random_fill(
              host_random_failure, NULL, output, 32U) ==
              APPLIANCE_STATE_CRYPTO_RANDOM_FAILED,
          "a failing randomness source fails closed");
    CHECK(appliance_state_crypto_random_fill(
              host_random, NULL, output, 32U) ==
              APPLIANCE_STATE_CRYPTO_NULL,
          "a null randomness context fails closed");
}

/* --- key custody -------------------------------------------------------- */

static void host_custody_round_trip(void)
{
    struct appliance_key_custody custody;
    struct appliance_key_custody_record record;
    struct appliance_key_custody_record decoded;
    struct appliance_key_custody_material material = passphrase_material();
    struct appliance_key_custody opened;
    struct host_rng rng = { 0x12345678U };
    uint8_t encoded[APPLIANCE_KEY_CUSTODY_RECORD_LENGTH];
    uint8_t recovery_key[APPLIANCE_KEY_CUSTODY_RECOVERY_KEY_LENGTH];
    struct appliance_key_custody_material recovery;
    char recovery_text[APPLIANCE_KEY_CUSTODY_RECOVERY_TEXT_LENGTH + 1U];
    char escrow[APPLIANCE_KEY_CUSTODY_ESCROW_MAX];
    size_t escrow_length = 0U;
    size_t index;
    bool hex = true;

    memset(&custody, 0, sizeof(custody));
    memset(&record, 0, sizeof(record));
    memset(&opened, 0, sizeof(opened));
    memset(encoded, 0, sizeof(encoded));
    memset(recovery_text, 0, sizeof(recovery_text));
    memset(escrow, 0, sizeof(escrow));

    CHECK(appliance_key_custody_create(
              &custody, host_random, &rng, &material, &record,
              recovery_text) == APPLIANCE_KEY_CUSTODY_OK,
          "custody creation seals a fresh data key");
    CHECK(custody.state == APPLIANCE_KEY_CUSTODY_STATE_UNSEALED &&
              custody.binding ==
                  APPLIANCE_KEY_CUSTODY_BINDING_PASSPHRASE,
          "a created custody is unsealed under the passphrase binding");
    CHECK(!bytes_all_zero(custody.dek,
                          APPLIANCE_KEY_CUSTODY_DEK_LENGTH) &&
              !bytes_all_zero(custody.dek_fingerprint,
                              APPLIANCE_KEY_CUSTODY_FINGERPRINT_LENGTH),
          "custody creation publishes a non-zero data key and fingerprint");
    CHECK(record.version == APPLIANCE_KEY_CUSTODY_VERSION &&
              record.binding ==
                  APPLIANCE_KEY_CUSTODY_BINDING_PASSPHRASE &&
              record.kdf_id ==
                  APPLIANCE_KEY_CUSTODY_KDF_PBKDF2_SHA256 &&
              record.kdf_iterations >=
                  APPLIANCE_STATE_CRYPTO_PBKDF2_MIN_ITERATIONS,
          "the record declares the passphrase binding and the KDF");
    CHECK(record.kdf_iterations ==
              APPLIANCE_STATE_CRYPTO_PBKDF2_DEFAULT_ITERATIONS,
          "the record carries the default bounded iteration count");
    CHECK(!bytes_all_zero(record.salt,
                          APPLIANCE_STATE_CRYPTO_SALT_LENGTH) &&
              !bytes_all_zero(record.recovery_salt,
                              APPLIANCE_STATE_CRYPTO_SALT_LENGTH) &&
              !bytes_all_zero(record.volume_id,
                              APPLIANCE_KEY_CUSTODY_VOLUME_ID_LENGTH),
          "the record carries fresh salts and a volume identity");
    CHECK(memcmp(record.dek_fingerprint, custody.dek_fingerprint,
                 APPLIANCE_KEY_CUSTODY_FINGERPRINT_LENGTH) == 0,
          "the record binds the data-key fingerprint");

    for (index = 0U;
         index < APPLIANCE_KEY_CUSTODY_RECOVERY_TEXT_LENGTH; ++index) {
        const char character = recovery_text[index];

        if (!((character >= '0' && character <= '9') ||
              (character >= 'A' && character <= 'F'))) {
            hex = false;
        }
    }
    CHECK(hex && recovery_text[
              APPLIANCE_KEY_CUSTODY_RECOVERY_TEXT_LENGTH] == '\0',
          "the recovery key is published as fixed-length uppercase hex");
    CHECK(appliance_key_custody_recovery_decode(
              recovery_text,
              APPLIANCE_KEY_CUSTODY_RECOVERY_TEXT_LENGTH,
              recovery_key) == APPLIANCE_KEY_CUSTODY_OK,
          "the published recovery key decodes");
    recovery = recovery_material(recovery_key);

    CHECK(appliance_key_custody_record_encode(&record, encoded) ==
              APPLIANCE_KEY_CUSTODY_OK,
          "the custody record encodes into its fixed 512-byte block");
    CHECK(memcmp(encoded, "HFOCKEY1", 8U) == 0,
          "the encoded custody record carries its magic");
    CHECK(appliance_key_custody_record_decode(encoded, &decoded) ==
              APPLIANCE_KEY_CUSTODY_OK,
          "the encoded custody record decodes");
    CHECK(decoded.version == record.version &&
              decoded.binding == record.binding &&
              decoded.kdf_id == record.kdf_id &&
              decoded.kdf_iterations == record.kdf_iterations &&
              memcmp(decoded.salt, record.salt,
                     sizeof(record.salt)) == 0 &&
              memcmp(decoded.volume_id, record.volume_id,
                     sizeof(record.volume_id)) == 0 &&
              memcmp(decoded.wrapped_dek, record.wrapped_dek,
                     sizeof(record.wrapped_dek)) == 0 &&
              memcmp(decoded.record_mac, record.record_mac,
                     sizeof(record.record_mac)) == 0,
          "the decoded record preserves every canonical field");

    CHECK(appliance_key_custody_open(
              &opened, &record, &material) ==
              APPLIANCE_KEY_CUSTODY_OK &&
              appliance_key_custody_unsealed(&opened) &&
              memcmp(opened.dek, custody.dek,
                     APPLIANCE_KEY_CUSTODY_DEK_LENGTH) == 0,
          "the passphrase unwraps the same data key");
    appliance_key_custody_clear(&opened);
    CHECK(appliance_key_custody_open(
              &opened, &record, &recovery) ==
              APPLIANCE_KEY_CUSTODY_OK &&
              memcmp(opened.dek, custody.dek,
                     APPLIANCE_KEY_CUSTODY_DEK_LENGTH) == 0,
          "the recovery key unwraps the same data key");

    CHECK(appliance_key_custody_escrow_line(
              &record, recovery_text, escrow, sizeof(escrow),
              &escrow_length) == APPLIANCE_KEY_CUSTODY_OK &&
              escrow_length > 0U &&
              strstr(escrow, "HFOC-RECOVERY-KEY") != NULL &&
              strstr(escrow, recovery_text) != NULL &&
              strstr(escrow, "fp=") != NULL,
          "the escrow line documents the recovery key and fingerprint");
    CHECK(appliance_key_custody_escrow_line(
              &record, recovery_text, escrow, 8U, &escrow_length) ==
              APPLIANCE_KEY_CUSTODY_ESCROW_TOO_SMALL,
          "a short escrow buffer is refused");

    CHECK(strcmp(appliance_key_custody_state_string(
                     APPLIANCE_KEY_CUSTODY_STATE_UNSEALED),
                 "UNSEALED") == 0 &&
              strcmp(appliance_key_custody_binding_string(
                         APPLIANCE_KEY_CUSTODY_BINDING_RECOVERY_KEY),
                     "RECOVERY_KEY") == 0,
          "custody status strings are exact");
}

static void host_custody_failure_modes(void)
{
    struct appliance_key_custody custody;
    struct appliance_key_custody_record record;
    struct appliance_key_custody_material material = passphrase_material();
    struct appliance_key_custody_material wrong = wrong_material();
    struct appliance_key_custody_material tpm = tpm_material();
    struct appliance_key_custody opened;
    struct host_rng rng = { 0xA5A5A5A5U };
    uint8_t encoded[APPLIANCE_KEY_CUSTODY_RECORD_LENGTH];
    uint8_t recovery_key[APPLIANCE_KEY_CUSTODY_RECOVERY_KEY_LENGTH];
    char recovery_text[APPLIANCE_KEY_CUSTODY_RECOVERY_TEXT_LENGTH + 1U];

    memset(&custody, 0, sizeof(custody));
    memset(&record, 0, sizeof(record));
    memset(&opened, 0, sizeof(opened));
    memset(recovery_text, 0, sizeof(recovery_text));

    CHECK(appliance_key_custody_create(
              &custody, host_random_failure, &rng, &material, &record,
              recovery_text) == APPLIANCE_KEY_CUSTODY_RANDOM_FAILED,
          "custody creation fails closed without randomness");
    CHECK(appliance_key_custody_create(
              &custody, host_random, &rng, &material, &record,
              recovery_text) == APPLIANCE_KEY_CUSTODY_OK &&
              appliance_key_custody_record_encode(&record, encoded) ==
                  APPLIANCE_KEY_CUSTODY_OK,
          "the failure fixture seals a fresh record");

    CHECK(appliance_key_custody_open(&opened, &record, NULL) ==
              APPLIANCE_KEY_CUSTODY_KEY_REQUIRED,
          "an unsealed volume without key material reports KEY_REQUIRED");
    CHECK(appliance_key_custody_open(&opened, &record, &tpm) ==
              APPLIANCE_KEY_CUSTODY_TPM_UNAVAILABLE,
          "the TPM binding reports UNAVAILABLE without a backend");
    CHECK(!appliance_key_custody_tpm_available(),
          "the frozen baseline publishes no TPM backend");
    CHECK(appliance_key_custody_open(&opened, &record, &wrong) ==
              APPLIANCE_KEY_CUSTODY_WRONG_KEY &&
              !appliance_key_custody_unsealed(&opened) &&
              bytes_all_zero(opened.dek,
                             APPLIANCE_KEY_CUSTODY_DEK_LENGTH),
          "a wrong passphrase fails closed and exposes no key");

    {
        struct appliance_key_custody_record tampered = record;

        tampered.wrapped_dek_tag[0] ^= 0x01U;
        CHECK(appliance_key_custody_open(&opened, &tampered, &material) ==
                  APPLIANCE_KEY_CUSTODY_WRONG_KEY,
              "a tampered wrap tag fails closed");
    }
    {
        struct appliance_key_custody_record tampered = record;

        tampered.record_mac[0] ^= 0x01U;
        CHECK(appliance_key_custody_open(&opened, &tampered, &material) ==
                  APPLIANCE_KEY_CUSTODY_AUTH_FAILED,
              "a tampered record MAC fails closed after unwrapping");
    }
    {
        uint8_t broken[APPLIANCE_KEY_CUSTODY_RECORD_LENGTH];
        struct appliance_key_custody_record decoded;

        memcpy(broken, encoded, sizeof(broken));
        broken[0] ^= 0xFFU;
        CHECK(appliance_key_custody_record_decode(broken, &decoded) ==
                  APPLIANCE_KEY_CUSTODY_BAD_MAGIC,
              "a corrupted custody magic is refused");
        memcpy(broken, encoded, sizeof(broken));
        broken[280U] = 0x01U;
        CHECK(appliance_key_custody_record_decode(broken, &decoded) ==
                  APPLIANCE_KEY_CUSTODY_RESERVED_NONZERO,
              "a non-zero reserved byte is refused");
        memcpy(broken, encoded, sizeof(broken));
        broken[8U] = 0x7FU;
        CHECK(appliance_key_custody_record_decode(broken, &decoded) ==
                  APPLIANCE_KEY_CUSTODY_BAD_VERSION,
              "an unsupported custody version is refused");
        memcpy(broken, encoded, sizeof(broken));
        broken[12U] = 0x09U;
        CHECK(appliance_key_custody_record_decode(broken, &decoded) ==
                  APPLIANCE_KEY_CUSTODY_BAD_BINDING,
              "an unknown custody binding is refused");
        memcpy(broken, encoded, sizeof(broken));
        memset(broken + 20U, 0, 4U);
        CHECK(appliance_key_custody_record_decode(broken, &decoded) ==
                  APPLIANCE_KEY_CUSTODY_BAD_ITERATIONS,
              "an unbounded KDF iteration count is refused");
    }
    CHECK(appliance_key_custody_recovery_decode(
              "not-a-recovery-key",
              sizeof("not-a-recovery-key") - 1U, recovery_key) ==
              APPLIANCE_KEY_CUSTODY_BAD_RECOVERY_KEY,
          "a malformed recovery key is refused");
    CHECK(appliance_key_custody_record_decode(NULL, NULL) ==
              APPLIANCE_KEY_CUSTODY_NULL &&
              appliance_key_custody_open(NULL, NULL, NULL) ==
                  APPLIANCE_KEY_CUSTODY_NULL,
          "null custody arguments are refused");
}

/* --- sealed store adapter ----------------------------------------------- */

static void host_sealed_store_round_trip(void)
{
    struct appliance_state_encryption_volume volume;
    struct appliance_state_block_device range;
    struct appliance_state_encryption_store store;
    struct appliance_state_encryption_store reopened;
    struct appliance_state_encryption_install_result result;
    struct appliance_state_layout layout;
    struct appliance_key_custody_material material = passphrase_material();
    struct appliance_key_custody_material wrong = wrong_material();
    struct host_rng rng = { 0xDEADBEEFU };
    struct block_device partition = partition_device();
    struct block_device *view;
    uint8_t plaintext[HOST_BLOCK_SIZE];
    uint8_t read_back[HOST_BLOCK_SIZE];
    uint8_t raw_before[HOST_BLOCK_SIZE];
    uint8_t raw_after[HOST_BLOCK_SIZE];
    size_t index;

    CHECK(appliance_state_layout_default(&layout) ==
              APPLIANCE_STATE_LAYOUT_OK,
          "the default layout is available to the proof");
    memset(partition_storage, 0, sizeof(partition_storage));
    partition_flush_calls = 0U;
    memset(&volume, 0, sizeof(volume));
    memset(&store, 0, sizeof(store));
    memset(&reopened, 0, sizeof(reopened));
    memset(&result, 0, sizeof(result));
    memset(plaintext, 0, sizeof(plaintext));

    CHECK(appliance_state_encryption_install(
              NULL, &layout, &material, host_random, &rng, &result) ==
              APPLIANCE_STATE_ENCRYPTION_NULL &&
              appliance_state_encryption_install(
                  &partition, NULL, &material, host_random, &rng,
                  &result) == APPLIANCE_STATE_ENCRYPTION_NULL &&
              appliance_state_encryption_install(
                  &partition, &layout, NULL, host_random, &rng,
                  &result) == APPLIANCE_STATE_ENCRYPTION_NULL,
          "the encrypted install refuses null arguments");

    memset(&volume, 0, sizeof(volume));
    CHECK(appliance_state_encryption_volume_probe(&volume, &partition) ==
              APPLIANCE_STATE_ENCRYPTION_NOT_ENCRYPTED &&
              !appliance_state_encryption_volume_sealed(&volume),
          "an untouched partition reports not encrypted");

    memset(&result, 0, sizeof(result));
    CHECK(appliance_state_encryption_install(
              &partition, &layout, &material, host_random, &rng,
              &result) == APPLIANCE_STATE_ENCRYPTION_OK,
          "the encrypted install seals the whole state partition");
    CHECK(result.sealed_store_count == layout.entry_count &&
              result.recovery_text[0] != '\0' &&
              !bytes_all_zero(result.dek_fingerprint,
                              sizeof(result.dek_fingerprint)),
          "the install result publishes the recovery key and digest");

    for (index = 0U; index < layout.entry_count; ++index) {
        const uint64_t offset =
            layout.entries[index].block_offset * HOST_BLOCK_SIZE;

        CHECK(memcmp(partition_storage + offset, "HFOCDATA", 8U) != 0,
              "no store header stays in plaintext on the raw volume");
        CHECK(memcmp(partition_storage + offset, plaintext,
                     sizeof(plaintext)) != 0,
              "a zero plaintext block is stored as ciphertext");
    }
    CHECK(memcmp(partition_storage +
                     (HOST_PARTITION_BLOCK_COUNT - 1U) * HOST_BLOCK_SIZE,
                 "HFOCKEY1", 8U) == 0,
          "the custody record lives unencrypted in the last block");
    CHECK(!host_find_bytes(
              partition_storage, sizeof(partition_storage),
              test_passphrase, sizeof(test_passphrase) - 1U),
          "the passphrase never reaches the raw volume");

    memset(&volume, 0, sizeof(volume));
    CHECK(appliance_state_encryption_volume_probe(&volume, &partition) ==
              APPLIANCE_STATE_ENCRYPTION_OK &&
              appliance_state_encryption_volume_sealed(&volume),
          "the sealed volume is detected before any key is supplied");
    CHECK(appliance_state_encryption_volume_unlock(&volume, NULL) ==
              APPLIANCE_STATE_ENCRYPTION_KEY_REQUIRED &&
              !appliance_state_encryption_volume_unlocked(&volume),
          "a sealed volume without key material reports KEY_REQUIRED");
    CHECK(appliance_state_encryption_volume_unlock(&volume, &wrong) ==
              APPLIANCE_STATE_ENCRYPTION_WRONG_KEY &&
              !appliance_state_encryption_volume_unlocked(&volume),
          "a wrong passphrase fails closed at the volume");
    {
        struct appliance_key_custody_material tpm = tpm_material();

        CHECK(appliance_state_encryption_volume_unlock(&volume, &tpm) ==
                  APPLIANCE_STATE_ENCRYPTION_TPM_UNAVAILABLE,
              "the volume refuses the TPM binding without a backend");
    }
    CHECK(appliance_state_encryption_volume_unlock(
              &volume, &material) == APPLIANCE_STATE_ENCRYPTION_OK &&
              appliance_state_encryption_volume_unlocked(&volume),
          "the correct passphrase unlocks the volume");

    CHECK(appliance_state_block_device_bind(
              &partition, &layout.entries[0], &range) ==
              APPLIANCE_STATE_BLOCK_DEVICE_OK,
          "the first store range binds");
    CHECK(appliance_state_encryption_store_bind(
              &volume, layout.entries[0].namespace_id,
              layout.entries[0].store_id, &range.device, &store) ==
              APPLIANCE_STATE_ENCRYPTION_OK,
          "the sealed store view binds on an unlocked volume");
    view = &store.device;
    CHECK(appliance_state_encryption_store_device(&store) ==
              &store.device &&
              view->block_size == HOST_BLOCK_SIZE &&
              view->block_count ==
                  layout.entries[0].block_count - 2U,
          "a 64-block store exposes 62 authenticated data blocks");
    CHECK(block_device_read(view, 0U, read_back, sizeof(read_back)) ==
              BLOCK_DEVICE_OK &&
              memcmp(read_back, "HFOCDATA", 8U) == 0,
          "the sealed store decrypts its data-region header");

    for (index = 0U; index < sizeof(plaintext); ++index) {
        plaintext[index] = (uint8_t)(index ^ 0x5AU);
    }
    CHECK(block_device_write(view, 5U * HOST_BLOCK_SIZE, plaintext,
                             sizeof(plaintext)) == BLOCK_DEVICE_OK &&
              block_device_flush(view) == BLOCK_DEVICE_OK,
          "a sealed block writes and flushes");
    memcpy(raw_before, partition_storage + 5U * HOST_BLOCK_SIZE,
           sizeof(raw_before));
    CHECK(memcmp(raw_before, plaintext, sizeof(plaintext)) != 0,
          "the written block is ciphertext on the raw volume");

    memset(read_back, 0, sizeof(read_back));
    CHECK(block_device_read(view, 5U * HOST_BLOCK_SIZE, read_back,
                            sizeof(read_back)) == BLOCK_DEVICE_OK &&
              memcmp(read_back, plaintext, sizeof(plaintext)) == 0,
          "the sealed store reads the block back");

    {
        struct appliance_state_encryption_volume reopened_volume;

        memset(&reopened_volume, 0, sizeof(reopened_volume));
        CHECK(appliance_state_encryption_volume_probe(
                  &reopened_volume, &partition) ==
                  APPLIANCE_STATE_ENCRYPTION_OK &&
              appliance_state_encryption_volume_unlock(
                  &reopened_volume, &material) ==
                  APPLIANCE_STATE_ENCRYPTION_OK &&
              appliance_state_encryption_store_bind(
                  &reopened_volume, layout.entries[0].namespace_id,
                  layout.entries[0].store_id, &range.device,
                  &reopened) == APPLIANCE_STATE_ENCRYPTION_OK,
              "a fresh volume handle reopens the store with the same key");
        view = &reopened.device;
        memset(read_back, 0, sizeof(read_back));
        CHECK(block_device_read(view, 5U * HOST_BLOCK_SIZE, read_back,
                                sizeof(read_back)) == BLOCK_DEVICE_OK &&
                  memcmp(read_back, plaintext, sizeof(plaintext)) == 0,
              "the reopened store decrypts the committed block");
    }

    /* A rewritten block advances the write counter, so the ciphertext of the
     * same plaintext changes and an old ciphertext snapshot stays useless. */
    memcpy(raw_before, partition_storage + 5U * HOST_BLOCK_SIZE,
           sizeof(raw_before));
    CHECK(block_device_write(view, 5U * HOST_BLOCK_SIZE, plaintext,
                             sizeof(plaintext)) == BLOCK_DEVICE_OK &&
              block_device_flush(view) == BLOCK_DEVICE_OK,
          "the same plaintext rewrites through the sealed store");
    memcpy(raw_after, partition_storage + 5U * HOST_BLOCK_SIZE,
           sizeof(raw_after));
    CHECK(memcmp(raw_before, raw_after, sizeof(raw_before)) != 0,
          "the rewrite advances the counter and changes the ciphertext");
    memset(read_back, 0, sizeof(read_back));
    CHECK(block_device_read(view, 5U * HOST_BLOCK_SIZE, read_back,
                            sizeof(read_back)) == BLOCK_DEVICE_OK &&
              memcmp(read_back, plaintext, sizeof(plaintext)) == 0,
          "the rewritten block still decrypts to the same plaintext");
}

static void host_sealed_store_fail_closed(void)
{
    struct appliance_state_encryption_volume volume;
    struct appliance_state_encryption_volume foreign_volume;
    struct appliance_state_block_device range;
    struct appliance_state_encryption_store store;
    struct appliance_state_encryption_store foreign;
    struct appliance_state_encryption_install_result result;
    struct appliance_state_layout layout;
    struct appliance_key_custody_material material = passphrase_material();
    struct host_rng rng = { 0x0BADF00DU };
    struct block_device partition = partition_device();
    uint8_t buffer[HOST_BLOCK_SIZE];
    uint8_t snapshot[HOST_BLOCK_SIZE];
    size_t data_offset;
    size_t tag_offset;

    CHECK(appliance_state_layout_default(&layout) ==
              APPLIANCE_STATE_LAYOUT_OK,
          "the layout is available for the fail-closed proof");
    memset(partition_storage, 0, sizeof(partition_storage));
    memset(&volume, 0, sizeof(volume));
    memset(&foreign_volume, 0, sizeof(foreign_volume));
    memset(&store, 0, sizeof(store));
    memset(&foreign, 0, sizeof(foreign));
    memset(&result, 0, sizeof(result));
    memset(snapshot, 0, sizeof(snapshot));

    CHECK(appliance_state_encryption_install(
              &partition, &layout, &material, host_random, &rng,
              &result) == APPLIANCE_STATE_ENCRYPTION_OK &&
              appliance_state_encryption_volume_probe(
                  &volume, &partition) == APPLIANCE_STATE_ENCRYPTION_OK &&
              appliance_state_encryption_volume_unlock(
                  &volume, &material) == APPLIANCE_STATE_ENCRYPTION_OK,
          "the tamper fixture installs and unlocks");
    CHECK(appliance_state_block_device_bind(
              &partition, &layout.entries[3], &range) ==
              APPLIANCE_STATE_BLOCK_DEVICE_OK &&
              appliance_state_encryption_store_bind(
                  &volume, layout.entries[3].namespace_id,
                  layout.entries[3].store_id, &range.device, &store) ==
                  APPLIANCE_STATE_ENCRYPTION_OK,
          "the tamper fixture binds a secret store");

    data_offset =
        (size_t)(layout.entries[3].block_offset * HOST_BLOCK_SIZE);
    tag_offset = data_offset +
        (size_t)((layout.entries[3].block_count - 2U) * HOST_BLOCK_SIZE);

    memset(buffer, 0xAAU, sizeof(buffer));
    partition_storage[data_offset + 3U] ^= 0x01U;
    CHECK(block_device_read(
              &store.device,
              0U, buffer, sizeof(buffer)) == BLOCK_DEVICE_IO_ERROR &&
              buffer[0] == 0xAAU,
          "a tampered ciphertext block fails closed with no output");
    partition_storage[data_offset + 3U] ^= 0x01U;

    memset(buffer, 0xAAU, sizeof(buffer));
    partition_storage[tag_offset + 1U] ^= 0x01U;
    CHECK(appliance_state_encryption_store_bind(
              &volume, layout.entries[3].namespace_id,
              layout.entries[3].store_id, &range.device, &store) ==
              APPLIANCE_STATE_ENCRYPTION_OK &&
              block_device_read(
                  &store.device,
                  0U, buffer, sizeof(buffer)) == BLOCK_DEVICE_IO_ERROR &&
              buffer[0] == 0xAAU,
          "a tampered tag entry fails closed with no output");
    partition_storage[tag_offset + 1U] ^= 0x01U;

    /* A tampered tag must not let the next write advance the counter and
     * reuse a nonce: the write path authenticates the current block first. */
    memcpy(snapshot, partition_storage + data_offset, sizeof(snapshot));
    memset(buffer, 0x5AU, sizeof(buffer));
    partition_storage[tag_offset + 5U] ^= 0x01U;
    CHECK(appliance_state_encryption_store_bind(
              &volume, layout.entries[3].namespace_id,
              layout.entries[3].store_id, &range.device, &store) ==
              APPLIANCE_STATE_ENCRYPTION_OK &&
              block_device_write(&store.device, 0U, buffer,
                                 sizeof(buffer)) == BLOCK_DEVICE_IO_ERROR &&
              memcmp(partition_storage + data_offset, snapshot,
                     sizeof(snapshot)) == 0,
          "a tampered tag entry refuses the next write and leaves the block");
    partition_storage[tag_offset + 5U] ^= 0x01U;

    memset(buffer, 0x5AU, sizeof(buffer));
    memset(partition_storage + tag_offset, 0, 16U);
    CHECK(appliance_state_encryption_store_bind(
              &volume, layout.entries[3].namespace_id,
              layout.entries[3].store_id, &range.device, &store) ==
              APPLIANCE_STATE_ENCRYPTION_OK &&
              block_device_write(&store.device, 0U, buffer,
                                 sizeof(buffer)) == BLOCK_DEVICE_IO_ERROR &&
              memcmp(partition_storage + data_offset, snapshot,
                     sizeof(snapshot)) == 0,
          "an erased tag entry refuses the next write and leaves the block");

    /* A second volume under the same passphrase must not read the first
     * volume's ciphertext: the per-volume identity is part of every tag. */
    memcpy(snapshot, partition_storage + data_offset, sizeof(snapshot));
    {
        struct host_rng other_rng = { 0x11223344U };

        memset(&result, 0, sizeof(result));
        CHECK(appliance_state_encryption_install(
                  &partition, &layout, &material, host_random, &other_rng,
                  &result) == APPLIANCE_STATE_ENCRYPTION_OK &&
                  appliance_state_encryption_volume_probe(
                      &foreign_volume, &partition) ==
                      APPLIANCE_STATE_ENCRYPTION_OK &&
                  appliance_state_encryption_volume_unlock(
                      &foreign_volume, &material) ==
                      APPLIANCE_STATE_ENCRYPTION_OK &&
                  appliance_state_encryption_store_bind(
                      &foreign_volume, layout.entries[3].namespace_id,
                      layout.entries[3].store_id, &range.device,
                      &foreign) == APPLIANCE_STATE_ENCRYPTION_OK,
              "a second volume installs under the same passphrase");
    }
    memcpy(partition_storage + data_offset, snapshot, sizeof(snapshot));
    memset(buffer, 0xAAU, sizeof(buffer));
    CHECK(block_device_read(
              &foreign.device,
              0U, buffer, sizeof(buffer)) == BLOCK_DEVICE_IO_ERROR &&
              buffer[0] == 0xAAU,
          "a foreign volume cannot read another volume's ciphertext");
}

/* --- encrypted install and loader --------------------------------------- */

static void host_encrypted_install_and_loader(void)
{
    struct appliance_state_layout layout;
    struct appliance_state_encryption_install_result result;
    struct appliance_state_block_device partition;
    struct appliance_state_loader loader;
    struct appliance_key_custody_material material = passphrase_material();
    struct appliance_key_custody_material wrong = wrong_material();
    struct host_rng rng = { 0x5EED1234U };
    struct block_device disk;
    uint8_t decoded[APPLIANCE_KEY_CUSTODY_RECOVERY_KEY_LENGTH];

    memset(&result, 0, sizeof(result));
    memset(&loader, 0, sizeof(loader));
    memset(decoded, 0, sizeof(decoded));

    CHECK(appliance_state_layout_default(&layout) ==
              APPLIANCE_STATE_LAYOUT_OK,
          "the layout is available to the loader proof");
    install_gpt();
    disk = disk_device();

    CHECK(appliance_state_block_device_bind_range(
              &disk, HFOC_FIRST_LBA, HOST_PARTITION_BLOCK_COUNT,
              &partition) == APPLIANCE_STATE_BLOCK_DEVICE_OK,
          "the HFOC state partition binds");
    CHECK(appliance_state_encryption_install(
              &partition.device, &layout, &material, host_random, &rng,
              &result) == APPLIANCE_STATE_ENCRYPTION_OK,
          "the encrypted install seals the GPT state partition");
    CHECK(result.recovery_text[0] != '\0' &&
              appliance_key_custody_recovery_decode(
                  result.recovery_text,
                  APPLIANCE_KEY_CUSTODY_RECOVERY_TEXT_LENGTH,
                  decoded) == APPLIANCE_KEY_CUSTODY_OK,
          "the install publishes a decodable recovery key");
    CHECK(memcmp(disk_storage +
                     (HFOC_FIRST_LBA + HOST_PARTITION_BLOCK_COUNT - 1U) *
                         HOST_BLOCK_SIZE,
                 "HFOCKEY1", 8U) == 0,
          "the custody record is the last block of the GPT partition");
    CHECK(memcmp(disk_storage + HFOC_FIRST_LBA * HOST_BLOCK_SIZE,
                 "HFOCDATA", 8U) != 0,
          "the first store is ciphertext before the loader runs");

    CHECK(appliance_state_loader_init(&loader) ==
              APPLIANCE_STATE_LOADER_OK,
          "the loader initializes");
    CHECK(appliance_state_loader_load_with_key(
              &loader, &disk, &layout, NULL) ==
              APPLIANCE_STATE_LOADER_KEY_REQUIRED &&
              !appliance_state_loader_ready(&loader) &&
              loader.status == APPLIANCE_STATE_LOADER_KEY_REQUIRED,
          "ordinary boot without a key reports KEY_REQUIRED");
    CHECK(appliance_state_loader_load_with_key(
              &loader, &disk, &layout, &wrong) ==
              APPLIANCE_STATE_LOADER_KEY_REJECTED &&
              !appliance_state_loader_ready(&loader) &&
              loader.status == APPLIANCE_STATE_LOADER_KEY_REJECTED,
          "ordinary boot with a wrong key reports KEY_REJECTED");
    CHECK(appliance_state_loader_load_with_key(
              &loader, &disk, &layout, &(struct appliance_key_custody_material)
              { APPLIANCE_KEY_CUSTODY_BINDING_TPM, NULL, 0U }) ==
              APPLIANCE_STATE_LOADER_KEY_UNAVAILABLE &&
              !appliance_state_loader_ready(&loader),
          "ordinary boot with the TPM binding reports UNAVAILABLE");
    CHECK(appliance_state_loader_load_with_key(
              &loader, &disk, &layout, &material) ==
              APPLIANCE_STATE_LOADER_OK &&
              appliance_state_loader_ready(&loader),
          "the passphrase unlocks the state and the loader publishes ready");
    CHECK(appliance_state_loader_system_config(&loader) != NULL &&
              appliance_state_loader_system_config(&loader)->values.source ==
                  APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING,
          "the sealed stores load their safe defaults");
    memset(&loader, 0, sizeof(loader));
    {
        struct appliance_key_custody_material recovery =
            recovery_material(decoded);

        CHECK(appliance_state_loader_init(&loader) ==
                  APPLIANCE_STATE_LOADER_OK &&
                  appliance_state_loader_load_with_key(
                      &loader, &disk, &layout, &recovery) ==
                  APPLIANCE_STATE_LOADER_OK &&
                  appliance_state_loader_ready(&loader),
              "the escrowed recovery key unlocks the same state");
    }

    /* A plaintext state volume keeps the legacy path: no custody block means
     * no key is required, and the loader still publishes ready. */
    disk_init();
    install_gpt();
    disk = disk_device();
    memset(&loader, 0, sizeof(loader));
    CHECK(appliance_state_block_device_bind_range(
              &disk, HFOC_FIRST_LBA, HOST_PARTITION_BLOCK_COUNT,
              &partition) == APPLIANCE_STATE_BLOCK_DEVICE_OK,
          "the plaintext partition binds");
    write_plaintext_state(&partition.device, &layout);
    CHECK(appliance_state_loader_init(&loader) ==
              APPLIANCE_STATE_LOADER_OK &&
              appliance_state_loader_load_with_key(
                  &loader, &disk, &layout, &material) ==
                  APPLIANCE_STATE_LOADER_OK &&
              appliance_state_loader_ready(&loader),
          "a plaintext state volume keeps the legacy ready path");
    CHECK(strcmp(appliance_state_loader_status_string(
                     APPLIANCE_STATE_LOADER_KEY_REQUIRED),
                 "KEY_REQUIRED") == 0 &&
              strcmp(appliance_state_loader_status_string(
                         APPLIANCE_STATE_LOADER_KEY_REJECTED),
                     "KEY_REJECTED") == 0 &&
              strcmp(appliance_state_loader_status_string(
                         APPLIANCE_STATE_LOADER_KEY_UNAVAILABLE),
                     "KEY_UNAVAILABLE") == 0,
          "the new loader key statuses have exact strings");
}

int main(void)
{
    host_crypto_known_answers();
    host_custody_round_trip();
    host_custody_failure_modes();
    host_sealed_store_round_trip();
    host_sealed_store_fail_closed();
    host_encrypted_install_and_loader();

    if (failures != 0) {
        fprintf(stderr, "Appliance state encryption host tests: %d FAILED\n",
                failures);
        return 1;
    }
    printf("Appliance state encryption host tests: ALL OK (%d checks)\n",
           checks);
    return 0;
}

#else
int appliance_state_encryption_host_translation_unit_marker;
#endif
