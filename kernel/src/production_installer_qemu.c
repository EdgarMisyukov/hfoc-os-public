#ifdef HFOC_PRODUCTION_INSTALLER_QEMU_TEST

#include "appliance_gpt_discovery.h"
#include "appliance_persistent_network_config.h"
#include "appliance_persistent_secret_config.h"
#include "appliance_state_block_device.h"
#include "appliance_state_layout.h"
#include "appliance_state_loader.h"
#include "block_device.h"
#include "efi_boot_environment.h"
#include "efi_boot_variables.h"
#include "efi_fat32_esp.h"
#include "installer_hardware_discovery.h"
#include "installer_layout.h"
#include "installer_layout_contract.h"
#include "installer_media_verification.h"
#include "ipv4_config.h"
#include "installer_supported_hardware.h"
#include "installer_boot_menu.h"
#include "installer_gpt_preserve.h"
#include "installer_uninstall.h"
#include "kstring.h"
#include "production_installer.h"
#include "production_installer_boot_stage.h"
#include "production_installer_efi.h"
#include "production_installer_esp.h"
#include "production_installer_layout.h"
#include "production_installer_preserve.h"
#include "production_installer_state.h"
#include "serial.h"
#include "sha256.h"
#include "verification_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INSTALLER_QEMU_BLOCK_SIZE 512U
/*
 * ITEM-391: the fixture models the acceptance disk's size class (120 GB), not
 * a small test shape, so the footprint the proof publishes is the number a
 * hardware runbook can quote.  The backing store is sparse - only the sectors
 * a proof actually writes are kept - so the size costs a bounded store rather
 * than a 120 GB array.
 */
#define INSTALLER_QEMU_MEDIA_LBA UINT64_C(234375000)
#define INSTALLER_QEMU_SECTOR_SLOTS 4096U
/*
 * Deliberately small role lengths used only by the no-write refusal proofs,
 * which must not depend on the product sizes they are refusing around.
 */
#define INSTALLER_QEMU_REFUSAL_PARTITION_LENGTH 2048U
#define INSTALLER_QEMU_VARIABLE_CAPACITY 32U
#define INSTALLER_QEMU_DATA_MAX 512U
#define INSTALLER_QEMU_ORDER_CAPACITY 128U
#define INSTALLER_QEMU_BOOT_PAYLOAD_LENGTH 16U
#define INSTALLER_QEMU_LIMINE_CONFIG_LENGTH 11U
#define INSTALLER_QEMU_KERNEL_PAYLOAD_LENGTH 32U
#define INSTALLER_QEMU_MEDIA_LENGTH \
    (INSTALLER_QEMU_BOOT_PAYLOAD_LENGTH + \
     INSTALLER_QEMU_LIMINE_CONFIG_LENGTH + \
     INSTALLER_QEMU_KERNEL_PAYLOAD_LENGTH)
#define INSTALLER_QEMU_SIGNATURE_LENGTH 32U
#define INSTALLER_QEMU_RELEASE_ID 11U
#define INSTALLER_QEMU_SIGNER_ID 1U

struct installer_qemu_variable {
    bool present;
    struct efi_boot_variable_key key;
    size_t length;
    unsigned char data[INSTALLER_QEMU_DATA_MAX];
};

struct installer_qemu_variable_store {
    size_t variable_count;
    struct installer_qemu_variable
        variables[INSTALLER_QEMU_VARIABLE_CAPACITY];
    size_t write_calls;
};

static const unsigned char installer_qemu_global_guid[16U] = {
    0x61U, 0xDFU, 0xE4U, 0x8BU, 0xCAU, 0x93U, 0xD2U, 0x11U,
    0xAAU, 0x0DU, 0x00U, 0xE0U, 0x98U, 0x03U, 0x2BU, 0x8CU
};
static const unsigned char installer_qemu_device_path[8U] = {
    0x01U, 0x01U, 0x04U, 0x00U,
    0x7FU, 0xFFU, 0x04U, 0x00U
};

struct installer_qemu_sector {
    bool used;
    uint64_t lba;
    unsigned char data[INSTALLER_QEMU_BLOCK_SIZE];
};

/*
 * ITEM-396 Case B: the acceptance host's second disk - Debian on nvme0n1 while
 * the appliance goes to /dev/sda.  Its ESP carries Debian's shim, the menu has to
 * offer it, and nothing in this install may write a byte of it.  The fixture
 * therefore gives it its own sector store and its own write counter.
 */
#define INSTALLER_QEMU_NVME_SLOTS 512U
#define INSTALLER_QEMU_NVME_MEDIA_LBA UINT64_C(500118192)
#define INSTALLER_QEMU_NVME_ESP_FIRST UINT64_C(2048)
#define INSTALLER_QEMU_NVME_ESP_LENGTH UINT64_C(4096)
#define INSTALLER_QEMU_NVME_ROOT_FIRST UINT64_C(6144)
#define INSTALLER_QEMU_NVME_ROOT_LENGTH UINT64_C(2000000)
#define INSTALLER_QEMU_NVME_SHIM_LENGTH 640U
#define INSTALLER_QEMU_NVME_SHIM_SECTORS 2U
#define INSTALLER_QEMU_NVME_GRUB_LENGTH 512U

static struct installer_qemu_sector
    installer_qemu_nvme_sectors[INSTALLER_QEMU_NVME_SLOTS];
static size_t installer_qemu_nvme_sector_count;
static size_t installer_qemu_nvme_write_calls;
static struct block_device installer_qemu_nvme_device;
static struct installer_qemu_sector
    installer_qemu_sectors[INSTALLER_QEMU_SECTOR_SLOTS];
static size_t installer_qemu_sector_count;
static unsigned char installer_qemu_media[INSTALLER_QEMU_MEDIA_LENGTH];
static unsigned char installer_qemu_envelope[256U];
static size_t installer_qemu_envelope_length;
static struct verification_policy installer_qemu_policy;
static size_t installer_qemu_write_calls;
static size_t installer_qemu_fail_write_call;
static struct block_device installer_qemu_block_device;
static struct installer_qemu_variable_store installer_qemu_variables;

static const unsigned char installer_qemu_boot_payload[
    INSTALLER_QEMU_BOOT_PAYLOAD_LENGTH] = {
    0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U,
    0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU
};
static const unsigned char installer_qemu_limine_config[
    INSTALLER_QEMU_LIMINE_CONFIG_LENGTH] = "timeout: 3\n";
static const unsigned char installer_qemu_kernel_payload[
    INSTALLER_QEMU_KERNEL_PAYLOAD_LENGTH] = {
    0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U,
    0x18U, 0x19U, 0x1AU, 0x1BU, 0x1CU, 0x1DU, 0x1EU, 0x1FU,
    0x20U, 0x21U, 0x22U, 0x23U, 0x24U, 0x25U, 0x26U, 0x27U,
    0x28U, 0x29U, 0x2AU, 0x2BU, 0x2CU, 0x2DU, 0x2EU, 0x2FU
};
/*
 * ITEM-398: the loader and the sealed chain the boot stage stages into the
 * shared ESP.  They are not part of the sealed media prefix - the digest covers
 * the payload, the configuration and the kernel - so their content is the
 * fixture's, and the proof compares the volume with exactly these bytes.
 */
static const unsigned char installer_qemu_installed_loader[64U] = {
    0x30U, 0x31U, 0x32U, 0x33U, 0x34U, 0x35U, 0x36U, 0x37U,
    0x38U, 0x39U, 0x3AU, 0x3BU, 0x3CU, 0x3DU, 0x3EU, 0x3FU,
    0x40U, 0x41U, 0x42U, 0x43U, 0x44U, 0x45U, 0x46U, 0x47U,
    0x48U, 0x49U, 0x4AU, 0x4BU, 0x4CU, 0x4DU, 0x4EU, 0x4FU,
    0x50U, 0x51U, 0x52U, 0x53U, 0x54U, 0x55U, 0x56U, 0x57U,
    0x58U, 0x59U, 0x5AU, 0x5BU, 0x5CU, 0x5DU, 0x5EU, 0x5FU,
    0x60U, 0x61U, 0x62U, 0x63U, 0x64U, 0x65U, 0x66U, 0x67U,
    0x68U, 0x69U, 0x6AU, 0x6BU, 0x6CU, 0x6DU, 0x6EU, 0x6FU
};
static const unsigned char installer_qemu_installed_manifest[48U] = {
    0x70U, 0x71U, 0x72U, 0x73U, 0x74U, 0x75U, 0x76U, 0x77U,
    0x78U, 0x79U, 0x7AU, 0x7BU, 0x7CU, 0x7DU, 0x7EU, 0x7FU,
    0x80U, 0x81U, 0x82U, 0x83U, 0x84U, 0x85U, 0x86U, 0x87U,
    0x88U, 0x89U, 0x8AU, 0x8BU, 0x8CU, 0x8DU, 0x8EU, 0x8FU,
    0x90U, 0x91U, 0x92U, 0x93U, 0x94U, 0x95U, 0x96U, 0x97U,
    0x98U, 0x99U, 0x9AU, 0x9BU, 0x9CU, 0x9DU, 0x9EU, 0x9FU
};
static const char installer_qemu_disk_identity[] = "qemu-installer-disk";

static void installer_qemu_test_uninstall_restore(
    const struct installer_discovery_report *install_report,
    const struct installer_layout_plan *layout_plan,
    const struct installer_layout_snapshot *layout_snapshot);

static void installer_qemu_fail(const char *message)
    __attribute__((noreturn));

static void installer_qemu_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[PRODUCTION-INSTALLER] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void installer_qemu_require(bool condition, const char *message)
{
    if (!condition) {
        installer_qemu_fail(message);
    }
}

static const char *installer_qemu_find_text(
    const char *haystack,
    const char *needle)
{
    size_t start;

    if (haystack == NULL || needle == NULL) {
        return NULL;
    }
    for (start = 0U; haystack[start] != '\0'; ++start) {
        size_t offset = 0U;

        while (needle[offset] != '\0' &&
               haystack[start + offset] == needle[offset]) {
            ++offset;
        }
        if (needle[offset] == '\0') {
            return haystack + start;
        }
    }
    return needle[0U] == '\0' ? haystack : NULL;
}

static bool installer_qemu_contains(
    const char *haystack,
    const char *needle)
{
    return installer_qemu_find_text(haystack, needle) != NULL;
}

static char installer_qemu_hex_digit(unsigned int value)
{
    static const char digits[] = "0123456789ABCDEF";

    return digits[value & 0x0FU];
}

static void installer_qemu_boot_key(
    uint16_t number,
    struct efi_boot_variable_key *key)
{
    memset(key, 0, sizeof(*key));
    memcpy(key->guid, installer_qemu_global_guid,
           sizeof(installer_qemu_global_guid));
    key->name[0U] = 'B';
    key->name[1U] = 'o';
    key->name[2U] = 'o';
    key->name[3U] = 't';
    key->name[4U] = installer_qemu_hex_digit(
        (unsigned int)(number >> 12U));
    key->name[5U] = installer_qemu_hex_digit(
        (unsigned int)((number >> 8U) & 0x0FU));
    key->name[6U] = installer_qemu_hex_digit(
        (unsigned int)((number >> 4U) & 0x0FU));
    key->name[7U] = installer_qemu_hex_digit(
        (unsigned int)(number & 0x0FU));
}

static void installer_qemu_order_key(
    struct efi_boot_variable_key *key)
{
    memset(key, 0, sizeof(*key));
    memcpy(key->guid, installer_qemu_global_guid,
           sizeof(installer_qemu_global_guid));
    memcpy(key->name, "BootOrder", 10U);
}

static bool installer_qemu_key_equal(
    const struct efi_boot_variable_key *left,
    const struct efi_boot_variable_key *right)
{
    return memcmp(left->guid, right->guid,
                  sizeof(installer_qemu_global_guid)) == 0 &&
           strcmp(left->name, right->name) == 0;
}

static void installer_qemu_variable_reset(void)
{
    memset(&installer_qemu_variables, 0,
           sizeof(installer_qemu_variables));
}

static void installer_qemu_variable_delete(
    const struct efi_boot_variable_key *key)
{
    size_t index;

    for (index = 0U;
         index < installer_qemu_variables.variable_count;
         ++index) {
        if (installer_qemu_variables.variables[index].present &&
            installer_qemu_key_equal(
                &installer_qemu_variables.variables[index].key,
                key)) {
            installer_qemu_variables.variables[index].present = false;
            installer_qemu_variables.variables[index].length = 0U;
            return;
        }
    }
}

static void installer_qemu_variable_add(
    const struct efi_boot_variable_key *key,
    const unsigned char *data,
    size_t length)
{
    size_t index;

    for (index = 0U;
         index < installer_qemu_variables.variable_count;
         ++index) {
        if (installer_qemu_variables.variables[index].present &&
            installer_qemu_key_equal(
                &installer_qemu_variables.variables[index].key,
                key)) {
            installer_qemu_variables.variables[index].length = length;
            memcpy(installer_qemu_variables.variables[index].data,
                   data, length);
            return;
        }
    }
    installer_qemu_require(
        installer_qemu_variables.variable_count <
            INSTALLER_QEMU_VARIABLE_CAPACITY,
        "variable store capacity");
    index = installer_qemu_variables.variable_count++;
    installer_qemu_variables.variables[index].present = true;
    installer_qemu_variables.variables[index].key = *key;
    installer_qemu_variables.variables[index].length = length;
    memcpy(installer_qemu_variables.variables[index].data,
           data, length);
}

static void installer_qemu_boot_add(
    uint16_t number,
    const unsigned char *data,
    size_t length)
{
    struct efi_boot_variable_key key;

    installer_qemu_boot_key(number, &key);
    installer_qemu_variable_add(&key, data, length);
}

static void installer_qemu_order_add(const uint16_t *entries, size_t count)
{
    struct efi_boot_variable_key key;
    unsigned char order[INSTALLER_QEMU_ORDER_CAPACITY];
    size_t index;
    size_t length = 0U;

    for (index = 0U; index < count; ++index) {
        order[length++] =
            (unsigned char)(entries[index] & 0xFFU);
        order[length++] =
            (unsigned char)(entries[index] >> 8U);
    }
    installer_qemu_order_key(&key);
    installer_qemu_variable_add(&key, order, length);
}

static bool installer_qemu_boot_get(
    uint16_t number,
    unsigned char *data,
    size_t capacity,
    size_t *length)
{
    struct efi_boot_variable_key key;
    size_t index;

    installer_qemu_boot_key(number, &key);
    for (index = 0U;
         index < installer_qemu_variables.variable_count;
         ++index) {
        const struct installer_qemu_variable *variable =
            &installer_qemu_variables.variables[index];
        if (variable->present &&
            installer_qemu_key_equal(&variable->key, &key)) {
            if (variable->length > capacity) {
                return false;
            }
            memcpy(data, variable->data, variable->length);
            *length = variable->length;
            return true;
        }
    }
    return false;
}

static enum efi_boot_variable_io_status installer_qemu_variable_read(
    void *context,
    const struct efi_boot_variable_key *key,
    unsigned char *data,
    size_t capacity,
    size_t *length)
{
    struct installer_qemu_variable_store *store =
        (struct installer_qemu_variable_store *)context;
    size_t index;

    if (store == NULL || key == NULL || data == NULL ||
        length == NULL) {
        return EFI_BOOT_VARIABLE_IO_ERROR;
    }
    for (index = 0U; index < store->variable_count; ++index) {
        const struct installer_qemu_variable *variable =
            &store->variables[index];
        if (variable->present &&
            installer_qemu_key_equal(&variable->key, key)) {
            if (variable->length > capacity) {
                return EFI_BOOT_VARIABLE_IO_BUFFER_TOO_SMALL;
            }
            memcpy(data, variable->data, variable->length);
            *length = variable->length;
            return EFI_BOOT_VARIABLE_IO_OK;
        }
    }
    return EFI_BOOT_VARIABLE_IO_NOT_FOUND;
}

static enum efi_boot_variable_io_status installer_qemu_variable_write(
    void *context,
    const struct efi_boot_variable_key *key,
    const unsigned char *data,
    size_t length)
{
    struct installer_qemu_variable_store *store =
        (struct installer_qemu_variable_store *)context;

    if (store == NULL || key == NULL || data == NULL) {
        return EFI_BOOT_VARIABLE_IO_ERROR;
    }
    ++store->write_calls;
    if (length > INSTALLER_QEMU_DATA_MAX) {
        return EFI_BOOT_VARIABLE_IO_BUFFER_TOO_SMALL;
    }
    if (length == 0U) {
        installer_qemu_variable_delete(key);
        return EFI_BOOT_VARIABLE_IO_OK;
    }
    installer_qemu_variable_add(key, data, length);
    return EFI_BOOT_VARIABLE_IO_OK;
}

static struct installer_qemu_sector *installer_qemu_sector_find(
    uint64_t lba)
{
    size_t index;

    for (index = 0U; index < installer_qemu_sector_count; ++index) {
        if (installer_qemu_sectors[index].used &&
            installer_qemu_sectors[index].lba == lba) {
            return &installer_qemu_sectors[index];
        }
    }
    return NULL;
}

static struct installer_qemu_sector *installer_qemu_sector_allocate(
    uint64_t lba)
{
    struct installer_qemu_sector *sector =
        installer_qemu_sector_find(lba);

    if (sector != NULL) {
        return sector;
    }
    if (installer_qemu_sector_count >= INSTALLER_QEMU_SECTOR_SLOTS) {
        return NULL;
    }
    sector = &installer_qemu_sectors[installer_qemu_sector_count++];
    memset(sector, 0, sizeof(*sector));
    sector->used = true;
    sector->lba = lba;
    return sector;
}

static void installer_qemu_sector_read(
    uint64_t lba,
    unsigned char *sector_out)
{
    const struct installer_qemu_sector *sector =
        installer_qemu_sector_find(lba);

    if (sector == NULL) {
        memset(sector_out, 0, INSTALLER_QEMU_BLOCK_SIZE);
        return;
    }
    memcpy(sector_out, sector->data, INSTALLER_QEMU_BLOCK_SIZE);
}

static void installer_qemu_sector_store(
    uint64_t lba,
    const unsigned char *data)
{
    struct installer_qemu_sector *sector =
        installer_qemu_sector_allocate(lba);

    if (sector == NULL) {
        installer_qemu_fail("fixture sector store exhausted");
        return;
    }
    memcpy(sector->data, data, INSTALLER_QEMU_BLOCK_SIZE);
}

static bool installer_qemu_storage_zero(void)
{
    size_t index;

    for (index = 0U; index < installer_qemu_sector_count; ++index) {
        size_t byte_index;

        if (!installer_qemu_sectors[index].used) {
            continue;
        }
        for (byte_index = 0U;
             byte_index < INSTALLER_QEMU_BLOCK_SIZE; ++byte_index) {
            if (installer_qemu_sectors[index].data[byte_index] != 0U) {
                return false;
            }
        }
    }
    return true;
}

static enum block_device_status installer_qemu_disk_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    unsigned char *bytes = (unsigned char *)buffer;
    unsigned char sector[INSTALLER_QEMU_BLOCK_SIZE];
    uint64_t lba;
    uint64_t sector_count;
    uint64_t index;
    size_t in_sector;

    (void)device;
    if (buffer == NULL || length == 0U ||
        offset % INSTALLER_QEMU_BLOCK_SIZE != 0U ||
        length % INSTALLER_QEMU_BLOCK_SIZE != 0U) {
        return BLOCK_DEVICE_UNALIGNED;
    }
    lba = offset / INSTALLER_QEMU_BLOCK_SIZE;
    sector_count = (uint64_t)(length / INSTALLER_QEMU_BLOCK_SIZE);
    if (lba >= INSTALLER_QEMU_MEDIA_LBA ||
        sector_count > INSTALLER_QEMU_MEDIA_LBA - lba) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    for (index = 0U; index < sector_count; ++index) {
        installer_qemu_sector_read(lba + index, sector);
        in_sector = (size_t)index * INSTALLER_QEMU_BLOCK_SIZE;
        memcpy(bytes + in_sector, sector, INSTALLER_QEMU_BLOCK_SIZE);
    }
    return BLOCK_DEVICE_OK;
}

static enum block_device_status installer_qemu_disk_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    const unsigned char *bytes = (const unsigned char *)buffer;
    uint64_t lba;
    uint64_t sector_count;
    uint64_t index;
    size_t in_sector;

    (void)device;
    if (buffer == NULL || length == 0U ||
        offset % INSTALLER_QEMU_BLOCK_SIZE != 0U ||
        length % INSTALLER_QEMU_BLOCK_SIZE != 0U) {
        return BLOCK_DEVICE_UNALIGNED;
    }
    ++installer_qemu_write_calls;
    if (installer_qemu_fail_write_call != 0U &&
        installer_qemu_write_calls == installer_qemu_fail_write_call) {
        installer_qemu_fail_write_call = 0U;
        return BLOCK_DEVICE_IO_ERROR;
    }
    lba = offset / INSTALLER_QEMU_BLOCK_SIZE;
    sector_count = (uint64_t)(length / INSTALLER_QEMU_BLOCK_SIZE);
    if (lba >= INSTALLER_QEMU_MEDIA_LBA ||
        sector_count > INSTALLER_QEMU_MEDIA_LBA - lba) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    for (index = 0U; index < sector_count; ++index) {
        struct installer_qemu_sector *sector =
            installer_qemu_sector_allocate(lba + index);

        if (sector == NULL) {
            return BLOCK_DEVICE_IO_ERROR;
        }
        in_sector = (size_t)index * INSTALLER_QEMU_BLOCK_SIZE;
        memcpy(sector->data, bytes + in_sector,
               INSTALLER_QEMU_BLOCK_SIZE);
    }
    return BLOCK_DEVICE_OK;
}

static enum block_device_status installer_qemu_disk_flush(
    struct block_device *device)
{
    (void)device;
    return BLOCK_DEVICE_OK;
}

static struct block_device installer_qemu_make_disk(void)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = INSTALLER_QEMU_BLOCK_SIZE;
    device.block_count = INSTALLER_QEMU_MEDIA_LBA;
    device.context = NULL;
    device.read_fn = installer_qemu_disk_read;
    device.write_fn = installer_qemu_disk_write;
    device.flush_fn = installer_qemu_disk_flush;
    return device;
}

static struct installer_qemu_sector *installer_qemu_nvme_find(uint64_t lba)
{
    size_t index;

    for (index = 0U; index < installer_qemu_nvme_sector_count; ++index) {
        if (installer_qemu_nvme_sectors[index].used &&
            installer_qemu_nvme_sectors[index].lba == lba) {
            return &installer_qemu_nvme_sectors[index];
        }
    }
    return NULL;
}

static void installer_qemu_nvme_store(
    uint64_t lba,
    const unsigned char *data)
{
    struct installer_qemu_sector *sector =
        installer_qemu_nvme_find(lba);

    if (sector == NULL) {
        if (installer_qemu_nvme_sector_count >=
            INSTALLER_QEMU_NVME_SLOTS) {
            installer_qemu_fail("nvme fixture store exhausted");
            return;
        }
        sector = &installer_qemu_nvme_sectors[
            installer_qemu_nvme_sector_count++];
        memset(sector, 0, sizeof(*sector));
        sector->used = true;
        sector->lba = lba;
    }
    memcpy(sector->data, data, INSTALLER_QEMU_BLOCK_SIZE);
}

static enum block_device_status installer_qemu_nvme_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    unsigned char *bytes = (unsigned char *)buffer;
    uint64_t lba = offset / INSTALLER_QEMU_BLOCK_SIZE;
    size_t count = length / INSTALLER_QEMU_BLOCK_SIZE;
    size_t index;

    (void)device;
    for (index = 0U; index < count; ++index) {
        const struct installer_qemu_sector *sector =
            installer_qemu_nvme_find(lba + index);

        if (sector == NULL) {
            memset(bytes + index * INSTALLER_QEMU_BLOCK_SIZE, 0,
                   INSTALLER_QEMU_BLOCK_SIZE);
        } else {
            memcpy(bytes + index * INSTALLER_QEMU_BLOCK_SIZE,
                   sector->data, INSTALLER_QEMU_BLOCK_SIZE);
        }
    }
    return BLOCK_DEVICE_OK;
}

/*
 * The only writer for this disk, and the install must never call it: it counts
 * what a bug would have written, so the proof can assert the count is zero.
 */
static enum block_device_status installer_qemu_nvme_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    const unsigned char *bytes = (const unsigned char *)buffer;
    uint64_t lba = offset / INSTALLER_QEMU_BLOCK_SIZE;
    size_t count = length / INSTALLER_QEMU_BLOCK_SIZE;
    size_t index;

    (void)device;
    ++installer_qemu_nvme_write_calls;
    for (index = 0U; index < count; ++index) {
        installer_qemu_nvme_store(lba + index,
                                  bytes + index * INSTALLER_QEMU_BLOCK_SIZE);
    }
    return BLOCK_DEVICE_OK;
}

static enum block_device_status installer_qemu_nvme_flush(
    struct block_device *device)
{
    (void)device;
    return BLOCK_DEVICE_OK;
}

static struct block_device installer_qemu_make_nvme_disk(void)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = INSTALLER_QEMU_BLOCK_SIZE;
    device.block_count = INSTALLER_QEMU_NVME_MEDIA_LBA;
    device.read_fn = installer_qemu_nvme_read;
    device.write_fn = installer_qemu_nvme_write;
    device.flush_fn = installer_qemu_nvme_flush;
    return device;
}

static void installer_qemu_reset_disk(void)
{
    memset(installer_qemu_sectors, 0, sizeof(installer_qemu_sectors));
    installer_qemu_sector_count = 0U;
    installer_qemu_write_calls = 0U;
    installer_qemu_fail_write_call = 0U;
    installer_qemu_block_device = installer_qemu_make_disk();
    memset(installer_qemu_nvme_sectors, 0,
           sizeof(installer_qemu_nvme_sectors));
    installer_qemu_nvme_sector_count = 0U;
    installer_qemu_nvme_write_calls = 0U;
    installer_qemu_nvme_device = installer_qemu_make_nvme_disk();
    installer_qemu_variable_reset();
}

static void installer_qemu_report(
    struct installer_discovery_report *report)
{
    memset(report, 0, sizeof(*report));
    report->version = INSTALLER_HARDWARE_DISCOVERY_VERSION;
    report->firmware_mode = INSTALLER_FIRMWARE_UEFI;
    report->read_only = true;
    report->scan_complete = true;
    report->cpu_present = true;
    report->cpu_cores = 4U;
    report->ram_present = true;
    report->ram_bytes = 4294967296ULL;
    report->disk_count = 1U;
    report->disks[0].present = true;
    report->disks[0].id = 1U;
    report->disks[0].controller = INSTALLER_DISK_CONTROLLER_VIRTIO_BLK;
    report->disks[0].partition_table = INSTALLER_PARTITION_TABLE_GPT;
    report->disks[0].block_size = INSTALLER_QEMU_BLOCK_SIZE;
    report->disks[0].block_count = INSTALLER_QEMU_MEDIA_LBA;
    memcpy(report->disks[0].identity, installer_qemu_disk_identity,
           sizeof(installer_qemu_disk_identity));
}

static enum installer_media_signature_status
installer_qemu_signature_verify(
    void *context,
    enum trust_anchor_algorithm algorithm,
    uint32_t signer_id,
    const unsigned char digest[SHA256_DIGEST_SIZE],
    const unsigned char *signature,
    size_t signature_length)
{
    size_t index;

    (void)context;
    if (algorithm != TRUST_ANCHOR_ALGORITHM_ECDSA_P256 ||
        signer_id != INSTALLER_QEMU_SIGNER_ID ||
        signature_length != INSTALLER_QEMU_SIGNATURE_LENGTH) {
        return INSTALLER_MEDIA_SIGNATURE_MISMATCH;
    }
    for (index = 0U; index < INSTALLER_QEMU_SIGNATURE_LENGTH; ++index) {
        if (signature[index] != (unsigned char)(
                digest[index % SHA256_DIGEST_SIZE] ^ 0x3CU)) {
            return INSTALLER_MEDIA_SIGNATURE_MISMATCH;
        }
    }
    return INSTALLER_MEDIA_SIGNATURE_OK;
}

/*
 * The QEMU composition binds a deterministic test signature verifier.  The
 * production verifier and its key material arrive with the signed boot chain
 * (ITEM-372); without it the installer refuses to plan or write, which is
 * exactly what installer_qemu_test_unverified_media_no_writes proves.
 */
static void installer_qemu_media_init(void)
{
    struct verification_policy_snapshot snapshot;
    unsigned char digest[SHA256_DIGEST_SIZE];
    unsigned char signature[INSTALLER_QEMU_SIGNATURE_LENGTH];
    size_t index;

    memcpy(installer_qemu_media, installer_qemu_boot_payload,
           sizeof(installer_qemu_boot_payload));
    memcpy(installer_qemu_media + sizeof(installer_qemu_boot_payload),
           installer_qemu_limine_config,
           sizeof(installer_qemu_limine_config));
    memcpy(installer_qemu_media + sizeof(installer_qemu_boot_payload) +
               sizeof(installer_qemu_limine_config),
           installer_qemu_kernel_payload,
           sizeof(installer_qemu_kernel_payload));
    installer_qemu_require(
        sha256_compute(installer_qemu_media,
                       sizeof(installer_qemu_media),
                       digest) == SHA256_OK,
        "media digest fixture");
    for (index = 0U; index < sizeof(signature); ++index) {
        signature[index] = (unsigned char)(
            digest[index % SHA256_DIGEST_SIZE] ^ 0x3CU);
    }
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = VERIFICATION_POLICY_VERSION;
    snapshot.generation = 1U;
    snapshot.anchor_count = 1U;
    snapshot.anchors[0U].id = INSTALLER_QEMU_SIGNER_ID;
    snapshot.anchors[0U].version = TRUST_ANCHOR_DESCRIPTOR_VERSION;
    snapshot.anchors[0U].algorithm_id =
        TRUST_ANCHOR_ALGORITHM_ECDSA_P256;
    snapshot.anchors[0U].state = TRUST_ANCHOR_STATE_ACTIVE;
    snapshot.anchors[0U].accepted_purposes =
        KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE);
    memset(&installer_qemu_policy, 0, sizeof(installer_qemu_policy));
    installer_qemu_require(
        verification_policy_init(&installer_qemu_policy, &snapshot) ==
            VERIFICATION_POLICY_OK,
        "media policy fixture");
    installer_qemu_envelope_length = 0U;
    installer_qemu_require(
        installer_media_manifest_seal(
            &installer_qemu_policy, INSTALLER_QEMU_RELEASE_ID,
            INSTALLER_QEMU_SIGNER_ID,
            TRUST_ANCHOR_ALGORITHM_ECDSA_P256, digest,
            sizeof(installer_qemu_media), digest,
            sizeof(installer_qemu_media), signature,
            sizeof(signature), installer_qemu_envelope,
            sizeof(installer_qemu_envelope),
            &installer_qemu_envelope_length) == INSTALLER_MEDIA_OK,
        "media envelope fixture");
}

static void installer_qemu_hardening_bind(
    struct production_installer *installer,
    const struct installer_discovery_report *report)
{
    struct installer_media_verifier verifier;
    struct installer_supported_hardware_policy hardware_policy;

    memset(&verifier, 0, sizeof(verifier));
    verifier.verify = installer_qemu_signature_verify;
    installer_qemu_require(
        production_installer_verify_media(
            installer, &installer_qemu_policy, &verifier,
            INSTALLER_QEMU_RELEASE_ID, installer_qemu_envelope,
            installer_qemu_envelope_length, installer_qemu_media,
            sizeof(installer_qemu_media)) == PRODUCTION_INSTALLER_OK,
        "media verification");
    installer_qemu_require(
        installer_supported_hardware_policy_default(&hardware_policy) ==
                INSTALLER_SUPPORTED_HARDWARE_OK &&
            production_installer_verify_hardware(
                installer, report, &hardware_policy) ==
                PRODUCTION_INSTALLER_OK,
        "hardware verification");
}

/*
 * Deliberately small range set for the refusal proofs (unconfirmed execution,
 * unverified media, unsupported hardware, injected write failure).  Those
 * proofs must not depend on the product sizes they are refusing around; the
 * install proof plans the declared layout instead.
 */
static void installer_qemu_requests(
    struct installer_layout_partition requests[4U])
{
    memset(requests, 0, 4U * sizeof(requests[0U]));
    requests[0U].present = true;
    requests[0U].role = INSTALLER_LAYOUT_ROLE_EFI_SYSTEM;
    requests[0U].length_lba = INSTALLER_QEMU_REFUSAL_PARTITION_LENGTH;
    requests[1U].present = true;
    requests[1U].role = INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM;
    requests[1U].length_lba = INSTALLER_QEMU_REFUSAL_PARTITION_LENGTH;
    requests[2U].present = true;
    requests[2U].role = INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE;
    requests[2U].length_lba = INSTALLER_QEMU_REFUSAL_PARTITION_LENGTH;
    requests[3U].present = true;
    requests[3U].role = INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY;
    requests[3U].length_lba = INSTALLER_QEMU_REFUSAL_PARTITION_LENGTH;
}

static struct production_installer_source installer_qemu_source(void)
{
    struct production_installer_source source;

    memset(&source, 0, sizeof(source));
    source.version = PRODUCTION_INSTALLER_SOURCE_VERSION;
    source.boot_payload = installer_qemu_media;
    source.boot_payload_size = sizeof(installer_qemu_boot_payload);
    source.limine_config =
        installer_qemu_media + sizeof(installer_qemu_boot_payload);
    source.limine_config_size =
        sizeof(installer_qemu_limine_config);
    source.kernel = installer_qemu_media +
                    sizeof(installer_qemu_boot_payload) +
                    sizeof(installer_qemu_limine_config);
    source.kernel_size = sizeof(installer_qemu_kernel_payload);
    return source;
}

static void installer_qemu_prepare_evidence(
    struct production_installer *installer,
    struct installer_discovery_report *report)
{
    struct production_installer_source source = installer_qemu_source();

    installer_qemu_report(report);
    installer_qemu_require(
        production_installer_init(installer) ==
            PRODUCTION_INSTALLER_OK &&
        production_installer_prepare(
            installer, report,
            PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
            installer_qemu_disk_identity,
            sizeof(installer_qemu_disk_identity) - 1U,
            &source, true) == PRODUCTION_INSTALLER_OK,
        "controller evidence preparation");
}

static void installer_qemu_prepare_controller(
    struct production_installer *installer,
    struct installer_discovery_report *report)
{
    installer_qemu_prepare_evidence(installer, report);
    installer_qemu_media_init();
    installer_qemu_hardening_bind(installer, report);
    installer_qemu_require(
        production_installer_build_plan(installer) ==
            PRODUCTION_INSTALLER_OK &&
        production_installer_publish_preview(installer) ==
            PRODUCTION_INSTALLER_OK,
        "controller preparation");
}

/*
 * ITEM-391: the install proof runs the production entry point in the product
 * order - evidence, plan, contracted layout, published preview - so the layout
 * it executes and publishes is the declared one, not a fixture range set.
 */
static void installer_qemu_prepare_contracted_controller(
    struct production_installer *installer,
    struct installer_discovery_report *report,
    struct installer_layout_plan *plan)
{
    installer_qemu_prepare_evidence(installer, report);
    installer_qemu_media_init();
    installer_qemu_hardening_bind(installer, report);
    installer_qemu_require(
        production_installer_build_plan(installer) ==
                PRODUCTION_INSTALLER_OK &&
            production_installer_plan_layout(
                installer, report, plan, NULL) ==
                PRODUCTION_INSTALLER_OK &&
            installer->layout_ready &&
            production_installer_publish_preview(installer) ==
                PRODUCTION_INSTALLER_OK,
        "contracted controller preparation");
    installer_qemu_require(
        installer->layout.mode ==
                INSTALLER_LAYOUT_CONTRACT_MODE_CLEAN_WHOLE_DISK &&
            installer->layout.footprint_lba ==
                INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA &&
            installer->layout.media_lba == INSTALLER_QEMU_MEDIA_LBA &&
            plan->partition_count ==
                INSTALLER_LAYOUT_CONTRACT_CLEAN_ROLE_COUNT,
        "contracted layout carries the declared sizes");
    installer_qemu_require(
        installer_qemu_contains(installer->preview, "footprint_lba=") &&
            installer_qemu_contains(
                installer->preview, "footprint_mib=161") &&
            installer_qemu_contains(
                installer->preview, "minimum_media_lba=331810") &&
            installer_qemu_contains(installer->preview, "free_lba=") &&
            installer_qemu_contains(
                installer->preview,
                "hfoc-system:69632+131072=64mib"),
        "contracted preview publishes the installed footprint");
    installer_qemu_require(
        installer_layout_contract_validate_summary(
            report, 1U, &installer->layout) ==
            INSTALLER_LAYOUT_CONTRACT_OK,
        "contracted layout summary validates");
}

static uint64_t installer_qemu_gpt_entry_lba(
    const unsigned char *entry,
    size_t offset)
{
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)entry[offset + index] << (8U * index);
    }
    return value;
}

/*
 * The published record, the plan and the bytes on the disk must agree: this
 * proof reads the GPT array the execution wrote and compares every role with
 * the summary the record was rendered from.
 */
static void installer_qemu_verify_published_layout(
    const struct production_installer *installer,
    const struct installer_layout_plan *plan)
{
    unsigned char array[INSTALLER_QEMU_BLOCK_SIZE];
    char record[INSTALLER_LAYOUT_CONTRACT_RECORD_LENGTH];
    size_t record_length = 0U;
    uint32_t index;

    installer_qemu_require(
        installer_layout_contract_render_record(
            &installer->layout, record, sizeof(record),
            &record_length) == INSTALLER_LAYOUT_CONTRACT_OK &&
            record_length != 0U,
        "published layout record renders");
    installer_qemu_require(
        installer_qemu_find_text(
            record, "[INSTALLER-LAYOUT] plan ") == record &&
            installer_qemu_contains(
                record,
                "[INSTALLER-LAYOUT] role=hfoc-system first_lba=69632 "
                "length_lba=131072 length_mib=64 reused=0\n") &&
            installer_qemu_contains(record, " footprint_mib=161 "),
        "published layout record carries the declared numbers");
    installer_qemu_require(
        block_device_read(
            &installer_qemu_block_device,
            2U * INSTALLER_QEMU_BLOCK_SIZE, array, sizeof(array)) ==
            BLOCK_DEVICE_OK,
        "published layout GPT array readback");
    for (index = 0U; index < plan->partition_count; ++index) {
        const unsigned char *entry =
            array + (size_t)index * 128U;
        const uint64_t first =
            installer_qemu_gpt_entry_lba(entry, 32U);
        const uint64_t last =
            installer_qemu_gpt_entry_lba(entry, 40U);

        installer_qemu_require(
            index < installer->layout.role_count &&
                first == installer->layout.roles[index].first_lba &&
                first == plan->partitions[index].first_lba &&
                last + 1U ==
                    installer->layout.roles[index].first_lba +
                        installer->layout.roles[index].length_lba &&
                last + 1U ==
                    plan->partitions[index].first_lba +
                        plan->partitions[index].length_lba,
            "published layout matches the executed GPT");
    }
}

static void installer_qemu_layout_fixture(
    struct installer_discovery_report *report,
    struct installer_layout_plan *plan)
{
    struct installer_layout_partition requests[4U];

    installer_qemu_report(report);
    installer_qemu_requests(requests);
    installer_qemu_require(
        installer_layout_plan_whole_disk(
            report, 1U, requests, 4U, plan) ==
            INSTALLER_LAYOUT_OK &&
        installer_layout_confirm(plan) == INSTALLER_LAYOUT_OK,
        "whole disk layout fixture");
}

static void installer_qemu_test_unconfirmed_no_writes(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_plan plan;
    struct installer_layout_snapshot snapshot;

    installer_qemu_reset_disk();
    installer_qemu_prepare_controller(&installer, &report);
    installer_qemu_layout_fixture(&report, &plan);
    memset(&snapshot, 0, sizeof(snapshot));
    installer_qemu_require(
        production_installer_execute_layout(
            &installer, &installer_qemu_block_device,
            &plan, &snapshot) == PRODUCTION_INSTALLER_BAD_STATE &&
        installer_qemu_write_calls == 0U,
        "unconfirmed controller performs no writes");
    serial_write(
        "[PRODUCTION-INSTALLER] unconfirmed no-write proof OK\n");
}

static void installer_qemu_test_unverified_media_no_writes(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_plan plan;
    struct installer_layout_snapshot snapshot;
    struct installer_media_verifier verifier;
    struct installer_supported_hardware_policy hardware_policy;

    installer_qemu_reset_disk();
    installer_qemu_prepare_evidence(&installer, &report);
    installer_qemu_media_init();
    installer_qemu_media[sizeof(installer_qemu_boot_payload) +
                        sizeof(installer_qemu_limine_config)] ^= 0x01U;
    memset(&verifier, 0, sizeof(verifier));
    verifier.verify = installer_qemu_signature_verify;
    installer_qemu_require(
        production_installer_verify_media(
            &installer, &installer_qemu_policy, &verifier,
            INSTALLER_QEMU_RELEASE_ID, installer_qemu_envelope,
            installer_qemu_envelope_length, installer_qemu_media,
            sizeof(installer_qemu_media)) ==
            PRODUCTION_INSTALLER_MEDIA_UNVERIFIED &&
        !installer.media_verified,
        "tampered media is refused before any write");
    installer_qemu_require(
        installer_supported_hardware_policy_default(&hardware_policy) ==
                INSTALLER_SUPPORTED_HARDWARE_OK &&
            production_installer_verify_hardware(
                &installer, &report, &hardware_policy) ==
                PRODUCTION_INSTALLER_OK,
        "hardware verification for the media proof");
    installer_qemu_layout_fixture(&report, &plan);
    memset(&snapshot, 0, sizeof(snapshot));
    installer_qemu_require(
        production_installer_build_plan(&installer) ==
                PRODUCTION_INSTALLER_MEDIA_UNVERIFIED &&
            production_installer_publish_preview(&installer) ==
                PRODUCTION_INSTALLER_BAD_STATE &&
            production_installer_confirm(&installer) ==
                PRODUCTION_INSTALLER_BAD_STATE &&
            production_installer_execute_layout(
                &installer, &installer_qemu_block_device,
                &plan, &snapshot) == PRODUCTION_INSTALLER_BAD_STATE &&
            installer_qemu_write_calls == 0U,
        "unverified media performs no writes");
    installer_qemu_media[sizeof(installer_qemu_boot_payload) +
                        sizeof(installer_qemu_limine_config)] ^= 0x01U;
    serial_write(
        "[PRODUCTION-INSTALLER] unverified media no-write proof OK\n");
}

static void installer_qemu_test_unsupported_hardware_no_writes(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_media_verifier verifier;
    struct installer_supported_hardware_policy hardware_policy;
    struct installer_layout_plan plan;
    struct installer_layout_snapshot snapshot;

    installer_qemu_reset_disk();
    installer_qemu_prepare_evidence(&installer, &report);
    installer_qemu_media_init();
    report.ram_bytes = 268435456ULL;
    memset(&verifier, 0, sizeof(verifier));
    verifier.verify = installer_qemu_signature_verify;
    installer_qemu_require(
        production_installer_verify_media(
            &installer, &installer_qemu_policy, &verifier,
            INSTALLER_QEMU_RELEASE_ID, installer_qemu_envelope,
            installer_qemu_envelope_length, installer_qemu_media,
            sizeof(installer_qemu_media)) == PRODUCTION_INSTALLER_OK,
        "media verification for the hardware proof");
    installer_qemu_require(
        installer_supported_hardware_policy_default(&hardware_policy) ==
                INSTALLER_SUPPORTED_HARDWARE_OK &&
            production_installer_verify_hardware(
                &installer, &report, &hardware_policy) ==
                PRODUCTION_INSTALLER_UNSUPPORTED_HARDWARE &&
            installer.hardware_reason ==
                INSTALLER_HARDWARE_REASON_MEMORY_INSUFFICIENT,
        "undeclared hardware is refused");
    installer_qemu_layout_fixture(&report, &plan);
    memset(&snapshot, 0, sizeof(snapshot));
    installer_qemu_require(
        production_installer_build_plan(&installer) ==
                PRODUCTION_INSTALLER_UNSUPPORTED_HARDWARE &&
            production_installer_execute_layout(
                &installer, &installer_qemu_block_device,
                &plan, &snapshot) == PRODUCTION_INSTALLER_BAD_STATE &&
            installer_qemu_write_calls == 0U,
        "unsupported hardware performs no writes");
    serial_write(
        "[PRODUCTION-INSTALLER] unsupported hardware no-write proof OK\n");
}

/*
 * ITEM-502: commit the state a previous run would leave behind - a static
 * address and an administrator credential - into the persistence partition of
 * the in-memory disk, through the real owners.
 */
static bool installer_qemu_commit_old_state(
    struct block_device *device,
    const struct installer_layout_plan *plan)
{
    struct appliance_state_layout layout;
    struct appliance_state_block_device partition_device;
    struct appliance_state_block_device store;
    struct appliance_persistent_network_config network;
    struct appliance_persistent_secret_config secret;
    struct network_ipv4_config static_config;
    unsigned char fingerprint[SHA256_DIGEST_SIZE];
    uint32_t index;

    for (index = 0U; index < plan->partition_count; ++index) {
        if (plan->partitions[index].present &&
            plan->partitions[index].role ==
                INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE) {
            break;
        }
    }
    if (index == plan->partition_count ||
        appliance_state_layout_default(&layout) !=
            APPLIANCE_STATE_LAYOUT_OK ||
        appliance_state_block_device_bind_range(
            device, plan->partitions[index].first_lba,
            plan->partitions[index].length_lba,
            &partition_device) != APPLIANCE_STATE_BLOCK_DEVICE_OK) {
        return false;
    }
    if (appliance_state_block_device_bind(
            &partition_device.device, &layout.entries[1U], &store) !=
        APPLIANCE_STATE_BLOCK_DEVICE_OK) {
        return false;
    }
    memset(&network, 0, sizeof(network));
    if (appliance_persistent_network_config_load(
            &network, &store.device) !=
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK ||
        network_ipv4_config_from_parts(
            "192.0.2.33/24", sizeof("192.0.2.33/24") - 1U,
            "192.0.2.1", sizeof("192.0.2.1") - 1U,
            "192.0.2.1", sizeof("192.0.2.1") - 1U,
            &static_config) != NETWORK_IPV4_OK ||
        appliance_persistent_network_config_set_static(
            &network, "eth0", 4U, &static_config) !=
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK) {
        return false;
    }
    if (appliance_state_block_device_bind(
            &partition_device.device, &layout.entries[4U], &store) !=
        APPLIANCE_STATE_BLOCK_DEVICE_OK) {
        return false;
    }
    memset(&secret, 0, sizeof(secret));
    memset(fingerprint, 0x5AU, sizeof(fingerprint));
    return appliance_persistent_secret_config_load(
               &secret, &store.device) ==
               APPLIANCE_PERSISTENT_SECRET_CONFIG_OK &&
           appliance_persistent_secret_config_commit_admin(
               &secret, fingerprint) ==
               APPLIANCE_PERSISTENT_SECRET_CONFIG_OK;
}

/* Does the machine still read a committed address or credential? */
static bool installer_qemu_old_state_present(struct block_device *device)
{
    struct appliance_state_layout layout;
    struct appliance_state_loader loader;
    const struct appliance_persistent_network_config *network;
    const struct appliance_persistent_secret_config *secret;
    unsigned char fingerprint[SHA256_DIGEST_SIZE];

    if (appliance_state_layout_default(&layout) !=
            APPLIANCE_STATE_LAYOUT_OK) {
        return true;
    }
    memset(&loader, 0, sizeof(loader));
    if (appliance_state_loader_init(&loader) !=
            APPLIANCE_STATE_LOADER_OK ||
        appliance_state_loader_load(&loader, device, &layout) !=
            APPLIANCE_STATE_LOADER_OK) {
        return true;
    }
    network = appliance_state_loader_network_config(&loader);
    secret = appliance_state_loader_secret_config(&loader);
    return network == NULL || secret == NULL ||
           network->source ==
               APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED ||
           appliance_persistent_secret_config_admin(secret, fingerprint);
}

static void installer_qemu_test_clean_install(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_plan plan;
    struct installer_layout_snapshot snapshot;
    struct appliance_state_layout state_layout;
    struct appliance_state_loader loader;
    struct efi_boot_environment environment;
    struct efi_fat32_esp esp;
    struct production_installer_source source = installer_qemu_source();
    unsigned char linux_before[64U];
    unsigned char linux_after[64U];
    unsigned char windows_before[80U];
    unsigned char windows_after[80U];
    unsigned char hfoc_entry[INSTALLER_QEMU_DATA_MAX];
    unsigned char readback[sizeof(installer_qemu_boot_payload)];
    size_t read_length = 0U;
    uint16_t unrelated_order[2U] = { 1U, 2U };
    uint16_t expected_order[3U] = { 0U, 1U, 2U };
    size_t index;

    installer_qemu_reset_disk();
    installer_qemu_prepare_contracted_controller(
        &installer, &report, &plan);
    /*
     * Both gates stay explicit: the controller confirms the install and the
     * plan confirms the layout it will write, exactly as the ITEM-340
     * executor requires.
     */
    installer_qemu_require(
        production_installer_confirm(&installer) ==
                PRODUCTION_INSTALLER_OK &&
            installer_layout_confirm(&plan) == INSTALLER_LAYOUT_OK,
        "controller confirmation");
    memset(&snapshot, 0, sizeof(snapshot));
    installer_qemu_require(
        production_installer_execute_layout(
            &installer, &installer_qemu_block_device,
            &plan, &snapshot) == PRODUCTION_INSTALLER_OK &&
        plan.executed,
        "clean GPT layout execution");
    serial_write(
        "[PRODUCTION-INSTALLER] clean GPT layout proof OK\n");

    memset(&loader, 0, sizeof(loader));
    installer_qemu_require(
        appliance_state_layout_default(&state_layout) ==
            APPLIANCE_STATE_LAYOUT_OK,
        "persistent state layout default");
    /*
     * ITEM-502: the state the previous run left behind is erased, read back
     * empty, and only then initialized - the same order the product flow uses.
     */
    {
        struct production_installer_state_erase_evidence erase_evidence;

        memset(&erase_evidence, 0, sizeof(erase_evidence));
        installer_qemu_require(
            production_installer_state_erase(
                &installer, &installer_qemu_block_device, &state_layout,
                &plan, &loader, &erase_evidence) ==
                    PRODUCTION_INSTALLER_OK &&
                erase_evidence.verify_clean,
            "persistent state erase");
        serial_write(
            "[PRODUCTION-INSTALLER] state erase proof OK\n");
    }
    installer_qemu_require(
        production_installer_state_initialize(
            &installer, &installer_qemu_block_device,
            &state_layout, &loader) == PRODUCTION_INSTALLER_OK &&
        appliance_state_loader_ready(&loader),
        "persistent state initialization");
    serial_write(
        "[PRODUCTION-INSTALLER] persistent state proof OK\n");

    /*
     * ITEM-502: the reinstall effect, in the guest.  A static address and an
     * administrator credential are committed the way a real run commits them;
     * a fresh controller then writes the layout again (which touches only the
     * GPT) and runs the erase step; the machine must read neither of them back.
     */
    {
        struct production_installer reinstall;
        struct installer_discovery_report reinstall_report;
        struct installer_layout_plan reinstall_plan;
        struct production_installer_state_erase_evidence erase_evidence;

        installer_qemu_require(
            installer_qemu_commit_old_state(
                &installer_qemu_block_device, &plan) &&
                installer_qemu_old_state_present(
                    &installer_qemu_block_device),
            "previous state is committed and readable");
        serial_write(
            "[PRODUCTION-INSTALLER] previous state committed\n");
        installer_qemu_prepare_contracted_controller(
            &reinstall, &reinstall_report, &reinstall_plan);
        installer_qemu_require(
            production_installer_confirm(&reinstall) ==
                    PRODUCTION_INSTALLER_OK &&
                installer_layout_confirm(&reinstall_plan) ==
                    INSTALLER_LAYOUT_OK,
            "reinstall confirmation");
        memset(&snapshot, 0, sizeof(snapshot));
        installer_qemu_require(
            production_installer_execute_layout(
                &reinstall, &installer_qemu_block_device,
                &reinstall_plan, &snapshot) ==
                    PRODUCTION_INSTALLER_OK &&
                installer_qemu_old_state_present(
                    &installer_qemu_block_device),
            "the layout write alone leaves the previous state readable");
        memset(&erase_evidence, 0, sizeof(erase_evidence));
        installer_qemu_require(
            production_installer_state_erase(
                &reinstall, &installer_qemu_block_device,
                &state_layout, &reinstall_plan, &loader,
                &erase_evidence) == PRODUCTION_INSTALLER_OK &&
                erase_evidence.verify_clean &&
                !installer_qemu_old_state_present(
                    &installer_qemu_block_device),
            "the reinstall erase leaves no address and no credential");
        serial_write(
            "[PRODUCTION-INSTALLER] state erase reinstall proof OK\n");
    }

    memset(&environment, 0, sizeof(environment));
    installer_qemu_require(
        efi_boot_environment_init(
            &environment, EFI_BOOT_FIRMWARE_UEFI,
            &installer_qemu_block_device,
            &installer_qemu_variables,
            installer_qemu_variable_read,
            installer_qemu_variable_write) ==
            EFI_BOOT_ENVIRONMENT_OK &&
        efi_boot_environment_set_device_path(
            &environment, installer_qemu_device_path,
            sizeof(installer_qemu_device_path)) ==
            EFI_BOOT_ENVIRONMENT_OK,
        "EFI environment initialization");
    installer_qemu_require(
        production_installer_esp_install(
            &installer, &environment,
            plan.partitions[0U].first_lba,
            plan.partitions[0U].length_lba,
            &source, true) == PRODUCTION_INSTALLER_OK,
        "ESP payload installation");
    memset(&esp, 0, sizeof(esp));
    installer_qemu_require(
        efi_fat32_esp_bind(
            &environment,
            plan.partitions[0U].first_lba,
            plan.partitions[0U].length_lba,
            &esp) == EFI_FAT32_ESP_OK &&
        efi_fat32_esp_read_payload(
            &esp, readback, sizeof(readback),
            &read_length) == EFI_FAT32_ESP_OK &&
        read_length == sizeof(installer_qemu_boot_payload) &&
        memcmp(readback, installer_qemu_boot_payload,
               sizeof(installer_qemu_boot_payload)) == 0,
        "ESP payload readback");
    serial_write(
        "[PRODUCTION-INSTALLER] ESP payload proof OK\n");

    memset(linux_before, 0x11U, sizeof(linux_before));
    memset(windows_before, 0x22U, sizeof(windows_before));
    installer_qemu_boot_add(
        1U, linux_before, sizeof(linux_before));
    installer_qemu_boot_add(
        2U, windows_before, sizeof(windows_before));
    installer_qemu_order_add(unrelated_order, 2U);
    installer_qemu_require(
        production_installer_efi_apply_boot(
            &installer, &environment) == PRODUCTION_INSTALLER_OK,
        "UEFI boot entry application");
    serial_write(
        "[PRODUCTION-INSTALLER] UEFI boot entry proof OK\n");

    /* Keep the 19 KiB snapshot out of the 16 KiB kernel thread stack. */
    static struct efi_boot_variables_snapshot after;

    read_length = 0U;
    installer_qemu_require(
        installer_qemu_boot_get(
            0U, hfoc_entry, sizeof(hfoc_entry),
            &read_length) && read_length > 0U,
        "HFOC Boot0000 created");
    read_length = 0U;
    installer_qemu_require(
        installer_qemu_boot_get(
            1U, linux_after, sizeof(linux_after),
            &read_length) &&
        read_length == sizeof(linux_before) &&
        memcmp(linux_after, linux_before,
               sizeof(linux_before)) == 0,
        "unrelated Boot0001 preserved");
    read_length = 0U;
    installer_qemu_require(
        installer_qemu_boot_get(
            2U, windows_after, sizeof(windows_after),
            &read_length) &&
        read_length == sizeof(windows_before) &&
        memcmp(windows_after, windows_before,
               sizeof(windows_before)) == 0,
        "unrelated Boot0002 preserved");
    serial_write(
        "[PRODUCTION-INSTALLER] unrelated boot entries preserved\n");

    memset(&after, 0, sizeof(after));
    installer_qemu_require(
        efi_boot_variables_snapshot(
            &environment, &after) ==
            EFI_BOOT_VARIABLES_OK &&
        after.boot_order_present &&
        after.boot_order_length == 6U &&
        after.entry_count == 3U,
        "boot variable snapshot");
    for (index = 0U; index < 3U; ++index) {
        installer_qemu_require(
            (uint16_t)(
                after.boot_order[index * 2U] |
                ((uint16_t)after.boot_order[index * 2U + 1U] <<
                 8U)) == expected_order[index],
            "BootOrder relative order");
    }
    serial_write(
        "[PRODUCTION-INSTALLER] BootOrder preservation proof OK\n");

    installer_qemu_require(
        production_installer_verify_layout(
            &installer, &installer_qemu_block_device,
            &plan) == PRODUCTION_INSTALLER_OK &&
        installer.stage == PRODUCTION_INSTALLER_STAGE_VERIFIED,
        "complete install verification");
    serial_write(
        "[PRODUCTION-INSTALLER] complete install verification proof OK\n");

    /*
     * ITEM-391: the installed footprint is published as the serial record the
     * acceptance INSTALLER stage and the hardware runbook quote, and the
     * record, the plan and the bytes on the disk are compared here.
     */
    installer_qemu_verify_published_layout(&installer, &plan);
    installer_qemu_require(
        production_installer_publish_layout_record(&installer) ==
            PRODUCTION_INSTALLER_OK,
        "layout record publication");
    serial_write(
        "[PRODUCTION-INSTALLER] layout contract proof OK\n");

    installer_qemu_test_uninstall_restore(&report, &plan, &snapshot);
}

/*
 * ITEM-396: the dual-boot preservation proof.
 *
 * The disk this phase runs on is prepared the way a machine that already boots
 * Debian is: a protective MBR with boot code, a primary and a backup GPT whose
 * 128 entries carry the shared EFI System Partition, a Debian root, a Debian
 * data partition and an entry above 4 GiB, and an ESP that holds
 * `\EFI\DEBIAN\GRUBX64.EFI` and `\EFI\DEBIAN\GRUB.CFG` and nothing of ours.
 *
 * The install then runs the production entry points in the product order -
 * discovery scan, evidence, plan, contracted dual-boot layout, menu, preview,
 * publication, confirm, preserving write, ESP reuse - and the proof asserts
 * what an operator would check afterwards: the foreign entries are still on the
 * disk byte for byte, the foreign loader and its configuration are still in
 * their cluster, the appliance's roles were added, the menu lists both systems,
 * and the published record matches the disk.
 */
#define INSTALLER_QEMU_ESP_SECTOR 512U
#define INSTALLER_QEMU_FOREIGN_ESP_FIRST UINT64_C(2048)
#define INSTALLER_QEMU_FOREIGN_ESP_LENGTH UINT64_C(4096)
#define INSTALLER_QEMU_FOREIGN_ROOT_FIRST UINT64_C(6144)
#define INSTALLER_QEMU_FOREIGN_ROOT_LENGTH UINT64_C(1000000)
#define INSTALLER_QEMU_FOREIGN_DATA_FIRST UINT64_C(2000000)
#define INSTALLER_QEMU_FOREIGN_DATA_LENGTH UINT64_C(400000)
#define INSTALLER_QEMU_FOREIGN_HIGH_FIRST UINT64_C(8388608)
#define INSTALLER_QEMU_FOREIGN_HIGH_LENGTH UINT64_C(1000000)
#define INSTALLER_QEMU_FOREIGN_ARRAY_SECTORS 32U
#define INSTALLER_QEMU_FOREIGN_ENTRY_COUNT 128U
#define INSTALLER_QEMU_FOREIGN_ENTRY_SIZE 128U
#define INSTALLER_QEMU_FOREIGN_ENTRIES_PER_SECTOR 4U
#define INSTALLER_QEMU_FOREIGN_ARRAY_LENGTH \
    (INSTALLER_QEMU_FOREIGN_ARRAY_SECTORS * INSTALLER_QEMU_ESP_SECTOR)
/* The fixture ESP: the FAT32 geometry the product formatter writes. */
#define INSTALLER_QEMU_ESP_RESERVED_SECTORS 32U
#define INSTALLER_QEMU_ESP_FAT_COUNT 2U
#define INSTALLER_QEMU_ESP_SECTORS_PER_FAT 32U
#define INSTALLER_QEMU_ESP_DATA_START \
    (INSTALLER_QEMU_ESP_RESERVED_SECTORS + \
     INSTALLER_QEMU_ESP_FAT_COUNT * INSTALLER_QEMU_ESP_SECTORS_PER_FAT)
#define INSTALLER_QEMU_ESP_ROOT_CLUSTER 2U
#define INSTALLER_QEMU_ESP_EFI_CLUSTER 3U
#define INSTALLER_QEMU_ESP_DEBIAN_CLUSTER 4U
#define INSTALLER_QEMU_ESP_LOADER_CLUSTER 5U
/*
 * ITEM-409: the GRUB Debian's shim loads, one cluster after the shim's two.
 * A real `\EFI\debian` carries both files; the shim is the signed entry point
 * the firmware boots, so it is the one the menu has to chainload.
 */
#define INSTALLER_QEMU_ESP_GRUB_CLUSTER 8U
#define INSTALLER_QEMU_ESP_CONFIG_CLUSTER 7U
/*
 * ITEM-398: the two paths this install takes over on a shared ESP, as a real
 * machine has them - the previous system's removable-media fallback loader at
 * `\EFI\BOOT\BOOTX64.EFI` and the Limine configuration that would shadow the
 * generated menu at `\EFI\BOOT\limine.conf`.
 */
#define INSTALLER_QEMU_ESP_BOOT_CLUSTER 8U
#define INSTALLER_QEMU_ESP_FALLBACK_CLUSTER 9U
#define INSTALLER_QEMU_ESP_FOREIGN_MENU_LENGTH 15U
#define INSTALLER_QEMU_ESP_LOADER_LENGTH 700U
#define INSTALLER_QEMU_ESP_FOREIGN_CONFIG_LENGTH 11U
#define INSTALLER_QEMU_ESP_LOADER_SECTORS 2U
#define INSTALLER_QEMU_ESP_DIRECTORY_ENTRY_SIZE 32U
#define INSTALLER_QEMU_ESP_EOC UINT32_C(0x0FFFFFFF)
#define INSTALLER_QEMU_KERNEL_BOOT_PATH "boot():/boot/hfoc-kernel.elf"
#define INSTALLER_QEMU_RECOVERY_BOOT_PATH "boot():/boot/hfoc-recovery.elf"

/* Snapshots of what the machine looked like before the install. */
static unsigned char installer_qemu_foreign_array_before[
    INSTALLER_QEMU_FOREIGN_ARRAY_LENGTH];
static unsigned char installer_qemu_nvme_array[
    INSTALLER_QEMU_FOREIGN_ARRAY_LENGTH];
static unsigned char installer_qemu_foreign_mbr_before[INSTALLER_QEMU_ESP_SECTOR];
static unsigned char installer_qemu_foreign_debian_dir_before[
    INSTALLER_QEMU_ESP_SECTOR];
static unsigned char installer_qemu_foreign_loader_before[
    INSTALLER_QEMU_ESP_LOADER_SECTORS][INSTALLER_QEMU_ESP_SECTOR];
static unsigned char installer_qemu_foreign_config_before[
    INSTALLER_QEMU_ESP_SECTOR];
static unsigned char installer_qemu_foreign_boot_dir_before[
    INSTALLER_QEMU_ESP_SECTOR];
static unsigned char installer_qemu_foreign_fallback_before[
    INSTALLER_QEMU_ESP_LOADER_SECTORS][INSTALLER_QEMU_ESP_SECTOR];
/*
 * ITEM-398: the foreign configuration's bytes.  The file itself is written
 * through the ESP writer, so this is the whole of what the proof compares the
 * preserved `.PRE` copy against.
 */
static const unsigned char installer_qemu_foreign_menu[
    INSTALLER_QEMU_ESP_FOREIGN_MENU_LENGTH] = "foreign limine!";

/* Caller-owned storage: none of this belongs on the boot stack. */
static struct installer_discovery_report installer_qemu_preserve_report;
static struct production_installer installer_qemu_preserve;
static struct installer_layout_plan installer_qemu_preserve_plan;
static struct installer_dual_boot_plan installer_qemu_preserve_dual;
static struct installer_gpt_preserve_reuse installer_qemu_preserve_reuse;
static struct installer_gpt_preserve_table installer_qemu_preserve_gpt;
static struct installer_boot_menu installer_qemu_preserve_menu;
static struct installer_boot_menu_inventory installer_qemu_preserve_inventory;
static struct installer_boot_menu_entry_source installer_qemu_preserve_appliance;
static struct installer_boot_menu_entry_source installer_qemu_preserve_recovery;
static struct efi_fat32_esp_loader
    installer_qemu_preserve_loaders[EFI_FAT32_ESP_LOADER_CAPACITY];
static struct efi_fat32_esp installer_qemu_preserve_esp;
static struct efi_boot_environment installer_qemu_preserve_environment;
static struct production_installer_source installer_qemu_preserve_source;
static unsigned char installer_qemu_foreign_array[
    INSTALLER_QEMU_FOREIGN_ARRAY_LENGTH];
static unsigned char installer_qemu_menu_readback[
    EFI_FAT32_ESP_CONFIG_MAX];
static char installer_qemu_menu_record[INSTALLER_BOOT_MENU_RECORD_LENGTH];

static void installer_qemu_store_le32(unsigned char *bytes, uint32_t value)
{
    bytes[0U] = (unsigned char)(value & 0xFFU);
    bytes[1U] = (unsigned char)((value >> 8U) & 0xFFU);
    bytes[2U] = (unsigned char)((value >> 16U) & 0xFFU);
    bytes[3U] = (unsigned char)((value >> 24U) & 0xFFU);
}

static void installer_qemu_store_le64(unsigned char *bytes, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static uint32_t installer_qemu_crc32(
    const unsigned char *bytes,
    size_t length)
{
    uint32_t crc = 0xFFFFFFFFU;
    size_t index;

    for (index = 0U; index < length; ++index) {
        unsigned int bit;

        crc ^= bytes[index];
        for (bit = 0U; bit < 8U; ++bit) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1U) ^ 0xEDB88320U;
            } else {
                crc >>= 1U;
            }
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

static const unsigned char installer_qemu_foreign_esp_guid[16U] = {
    0x28U, 0x73U, 0x2AU, 0xC1U, 0x1FU, 0xF8U, 0xD2U, 0x11U,
    0xBAU, 0x4BU, 0x00U, 0xA0U, 0xC9U, 0x3EU, 0xC9U, 0x3BU
};
static const unsigned char installer_qemu_foreign_linux_guid[16U] = {
    0xAFU, 0x3DU, 0xC6U, 0x0FU, 0x83U, 0x84U, 0x72U, 0x47U,
    0x8EU, 0x79U, 0x3DU, 0x69U, 0xD8U, 0x47U, 0x7DU, 0xE4U
};

static void installer_qemu_foreign_entry(
    unsigned char *array,
    uint32_t slot,
    const unsigned char type_guid[16U],
    uint64_t first_lba,
    uint64_t last_lba,
    const char *name)
{
    unsigned char *entry = array + (size_t)slot * INSTALLER_QEMU_FOREIGN_ENTRY_SIZE;
    size_t index;

    memcpy(entry, type_guid, 16U);
    for (index = 0U; index < 16U; ++index) {
        entry[16U + index] = (unsigned char)(0x70U + slot);
    }
    installer_qemu_store_le64(entry + 32U, first_lba);
    installer_qemu_store_le64(entry + 40U, last_lba);
    for (index = 0U; name[index] != '\0' && index < 35U; ++index) {
        entry[56U + index * 2U] = (unsigned char)name[index];
    }
}

static void installer_qemu_foreign_header(
    unsigned char *header,
    uint64_t my_lba,
    uint64_t alternate_lba,
    uint64_t entry_lba,
    const unsigned char *array){
    uint32_t crc;

    memset(header, 0, INSTALLER_QEMU_ESP_SECTOR);
    memcpy(header, "EFI PART", 8U);
    header[10U] = 0x01U;
    installer_qemu_store_le32(header + 12U, 92U);
    installer_qemu_store_le64(header + 24U, my_lba);
    installer_qemu_store_le64(header + 32U, alternate_lba);
    installer_qemu_store_le64(header + 40U, 34U);
    installer_qemu_store_le64(
        header + 48U, INSTALLER_QEMU_MEDIA_LBA - 34U);
    for (crc = 0U; crc < 16U; ++crc) {
        header[56U + crc] = (unsigned char)(0x50U + crc);
    }
    installer_qemu_store_le64(header + 72U, entry_lba);
    installer_qemu_store_le32(header + 80U, INSTALLER_QEMU_FOREIGN_ENTRY_COUNT);
    installer_qemu_store_le32(header + 84U, INSTALLER_QEMU_FOREIGN_ENTRY_SIZE);
    installer_qemu_store_le32(
        header + 88U, installer_qemu_crc32(
            array,
            INSTALLER_QEMU_FOREIGN_ENTRY_COUNT *
                INSTALLER_QEMU_FOREIGN_ENTRY_SIZE));
    crc = installer_qemu_crc32(header, 92U);
    installer_qemu_store_le32(header + 16U, crc);
}

static void installer_qemu_foreign_mbr_bytes(
    unsigned char mbr[INSTALLER_QEMU_ESP_SECTOR])
{
    size_t index;

    memset(mbr, 0, INSTALLER_QEMU_ESP_SECTOR);
    /* Boot code and a disk signature: the boot record the owner asked about. */
    for (index = 0U; index < 446U; ++index) {
        mbr[index] = (unsigned char)(0x40U + (index % 5U));
    }
    mbr[440U] = 0x5AU;
    mbr[441U] = 0x5AU;
    mbr[446U] = 0x00U;
    mbr[447U] = 0x00U;
    mbr[448U] = 0x02U;
    mbr[449U] = 0x00U;
    mbr[450U] = 0xEEU;
    mbr[451U] = 0xFFU;
    mbr[452U] = 0xFFU;
    mbr[453U] = 0xFFU;
    installer_qemu_store_le32(mbr + 454U, 1U);
    installer_qemu_store_le32(mbr + 458U, UINT32_C(0xFFFFFFFF));
    mbr[510U] = 0x55U;
    mbr[511U] = 0xAAU;
}

static void installer_qemu_foreign_mbr(void)
{
    unsigned char mbr[INSTALLER_QEMU_ESP_SECTOR];

    installer_qemu_foreign_mbr_bytes(mbr);
    installer_qemu_sector_store(0U, mbr);
    memcpy(installer_qemu_foreign_mbr_before, mbr, sizeof(mbr));
}

static void installer_qemu_foreign_fat_names(
    unsigned char *entry,
    const char *name,
    const char *extension,
    unsigned char attributes,
    uint32_t first_cluster,
    uint32_t size)
{
    size_t index;

    memset(entry, 0, INSTALLER_QEMU_ESP_DIRECTORY_ENTRY_SIZE);
    /*
     * An 8.3 name is space-padded, not NUL-padded: a reader that compares all
     * eleven bytes (which is what `efi_fat32_dir_find()` does) sees the
     * difference.
     */
    for (index = 0U; index < 11U; ++index) {
        entry[index] = (unsigned char)' ';
    }
    for (index = 0U; name[index] != '\0' && index < 8U; ++index) {
        entry[index] = (unsigned char)name[index];
    }
    for (index = 0U; extension[index] != '\0' && index < 3U; ++index) {
        entry[8U + index] = (unsigned char)extension[index];
    }
    entry[11U] = attributes;
    /*
     * A FAT32 directory entry keeps the high word of the first cluster at
     * offset 20 and the low word at offset 26 - the order `efi_fat32_dir_find()`
     * reads and the order a real formatter writes.
     */
    entry[20U] = (unsigned char)((first_cluster >> 16U) & 0xFFU);
    entry[21U] = (unsigned char)((first_cluster >> 24U) & 0xFFU);
    entry[26U] = (unsigned char)(first_cluster & 0xFFU);
    entry[27U] = (unsigned char)((first_cluster >> 8U) & 0xFFU);
    installer_qemu_store_le32(entry + 28U, size);
}

static void installer_qemu_foreign_esp(void)
{
    unsigned char sector[INSTALLER_QEMU_ESP_SECTOR];
    unsigned char directory[INSTALLER_QEMU_ESP_SECTOR];
    uint64_t base = INSTALLER_QEMU_FOREIGN_ESP_FIRST;
    uint32_t index;

    /* Boot sector. */
    memset(sector, 0, sizeof(sector));
    sector[11U] = 0x00U;
    sector[12U] = 0x02U;
    sector[13U] = 1U;
    sector[14U] = (unsigned char)INSTALLER_QEMU_ESP_RESERVED_SECTORS;
    sector[16U] = (unsigned char)INSTALLER_QEMU_ESP_FAT_COUNT;
    /*
     * ITEM-405: the volume declares its own extent.  A formatted FAT32 always
     * carries it, and the bind now measures the data region against it instead
     * of against the partition alone - a synthetic volume without it is not
     * something a formatter produces.
     */
    installer_qemu_store_le32(
        sector + 19U, 0U);
    installer_qemu_store_le32(
        sector + 32U, (uint32_t)INSTALLER_QEMU_FOREIGN_ESP_LENGTH);
    installer_qemu_store_le32(
        sector + 36U, INSTALLER_QEMU_ESP_SECTORS_PER_FAT);
    installer_qemu_store_le32(
        sector + 44U, INSTALLER_QEMU_ESP_ROOT_CLUSTER);
    sector[510U] = 0x55U;
    sector[511U] = 0xAAU;
    installer_qemu_sector_store(base, sector);

    /* Both FAT copies: the root, EFI and DEBIAN directories plus the chains. */
    memset(sector, 0, sizeof(sector));
    installer_qemu_store_le32(sector, 0x0FFFFFF8U);
    installer_qemu_store_le32(sector + 4U, 0x0FFFFFFFU);
    installer_qemu_store_le32(
        sector + INSTALLER_QEMU_ESP_ROOT_CLUSTER * 4U,
        INSTALLER_QEMU_ESP_EOC);
    installer_qemu_store_le32(
        sector + INSTALLER_QEMU_ESP_EFI_CLUSTER * 4U,
        INSTALLER_QEMU_ESP_EOC);
    installer_qemu_store_le32(
        sector + INSTALLER_QEMU_ESP_DEBIAN_CLUSTER * 4U,
        INSTALLER_QEMU_ESP_EOC);
    installer_qemu_store_le32(
        sector + INSTALLER_QEMU_ESP_LOADER_CLUSTER * 4U,
        INSTALLER_QEMU_ESP_LOADER_CLUSTER + 1U);
    installer_qemu_store_le32(
        sector + (INSTALLER_QEMU_ESP_LOADER_CLUSTER + 1U) * 4U,
        INSTALLER_QEMU_ESP_EOC);
    installer_qemu_store_le32(
        sector + INSTALLER_QEMU_ESP_CONFIG_CLUSTER * 4U,
        INSTALLER_QEMU_ESP_EOC);
    installer_qemu_store_le32(
        sector + INSTALLER_QEMU_ESP_BOOT_CLUSTER * 4U,
        INSTALLER_QEMU_ESP_EOC);
    installer_qemu_store_le32(
        sector + INSTALLER_QEMU_ESP_FALLBACK_CLUSTER * 4U,
        INSTALLER_QEMU_ESP_FALLBACK_CLUSTER + 1U);
    installer_qemu_store_le32(
        sector + (INSTALLER_QEMU_ESP_FALLBACK_CLUSTER + 1U) * 4U,
        INSTALLER_QEMU_ESP_EOC);
    for (index = 0U; index < INSTALLER_QEMU_ESP_SECTORS_PER_FAT; ++index) {
        if (index != 0U) {
            memset(sector, 0, sizeof(sector));
        }
        installer_qemu_sector_store(
            base + INSTALLER_QEMU_ESP_RESERVED_SECTORS + index, sector);
        installer_qemu_sector_store(
            base + INSTALLER_QEMU_ESP_RESERVED_SECTORS +
                INSTALLER_QEMU_ESP_SECTORS_PER_FAT + index,
            sector);
    }

    /* Root directory: the EFI directory entry. */
    memset(directory, 0, sizeof(directory));
    installer_qemu_foreign_fat_names(
        directory, "EFI", "", 0x10U, INSTALLER_QEMU_ESP_EFI_CLUSTER, 0U);
    installer_qemu_sector_store(
        base + INSTALLER_QEMU_ESP_DATA_START +
            (INSTALLER_QEMU_ESP_ROOT_CLUSTER - 2U), directory);

    /* \EFI: the DEBIAN directory entry and the BOOT fallback directory. */
    memset(directory, 0, sizeof(directory));
    installer_qemu_foreign_fat_names(
        directory, "DEBIAN", "", 0x10U, INSTALLER_QEMU_ESP_DEBIAN_CLUSTER, 0U);
    installer_qemu_foreign_fat_names(
        directory + INSTALLER_QEMU_ESP_DIRECTORY_ENTRY_SIZE,
        "BOOT", "", 0x10U, INSTALLER_QEMU_ESP_BOOT_CLUSTER, 0U);
    installer_qemu_sector_store(
        base + INSTALLER_QEMU_ESP_DATA_START +
            (INSTALLER_QEMU_ESP_EFI_CLUSTER - 2U), directory);

    /* \EFI\DEBIAN: the loader and its configuration. */
    memset(directory, 0, sizeof(directory));
    installer_qemu_foreign_fat_names(
        directory, "GRUBX64", "EFI", 0x20U,
        INSTALLER_QEMU_ESP_LOADER_CLUSTER,
        INSTALLER_QEMU_ESP_LOADER_LENGTH);
    installer_qemu_foreign_fat_names(
        directory + INSTALLER_QEMU_ESP_DIRECTORY_ENTRY_SIZE,
        "GRUB", "CFG", 0x20U, INSTALLER_QEMU_ESP_CONFIG_CLUSTER,
        INSTALLER_QEMU_ESP_FOREIGN_CONFIG_LENGTH);
    installer_qemu_sector_store(
        base + INSTALLER_QEMU_ESP_DATA_START +
            (INSTALLER_QEMU_ESP_DEBIAN_CLUSTER - 2U), directory);
    memcpy(installer_qemu_foreign_debian_dir_before, directory,
           sizeof(directory));

    /* The loader's two clusters and the configuration's one. */
    for (index = 0U; index < INSTALLER_QEMU_ESP_LOADER_SECTORS; ++index) {
        size_t byte;

        memset(sector, 0, sizeof(sector));
        for (byte = 0U; byte < INSTALLER_QEMU_ESP_SECTOR; ++byte) {
            const size_t offset =
                index * INSTALLER_QEMU_ESP_SECTOR + byte;

            sector[byte] = (unsigned char)(0x90U + (offset % 13U));
        }
        memcpy(installer_qemu_foreign_loader_before[index], sector,
               sizeof(sector));
        installer_qemu_sector_store(
            base + INSTALLER_QEMU_ESP_DATA_START +
                (INSTALLER_QEMU_ESP_LOADER_CLUSTER - 2U) + index,
            sector);
    }

    memset(sector, 0, sizeof(sector));
    memcpy(sector, "grub config", INSTALLER_QEMU_ESP_FOREIGN_CONFIG_LENGTH);
    installer_qemu_sector_store(
        base + INSTALLER_QEMU_ESP_DATA_START +
            (INSTALLER_QEMU_ESP_CONFIG_CLUSTER - 2U), sector);
    memcpy(installer_qemu_foreign_config_before, sector, sizeof(sector));

    /*
     * ITEM-398: \EFI\BOOT carries the previous system's removable-media fallback
     * loader, which is the path the installed boot set uses, so it is what the
     * install has to take over without losing it.  The loader's own
     * configuration (`\EFI\BOOT\limine.conf`, the first file a Limine reads)
     * is written through the ESP writer below, because `conf` is not an 8.3
     * extension and a real volume stores that name as a VFAT long name plus its
     * derived short entry - which is exactly what the reader expects back.
     */
    memset(directory, 0, sizeof(directory));
    installer_qemu_foreign_fat_names(
        directory, "BOOTX64", "EFI", 0x20U,
        INSTALLER_QEMU_ESP_FALLBACK_CLUSTER,
        INSTALLER_QEMU_ESP_LOADER_LENGTH);
    installer_qemu_sector_store(
        base + INSTALLER_QEMU_ESP_DATA_START +
            (INSTALLER_QEMU_ESP_BOOT_CLUSTER - 2U), directory);
    memcpy(installer_qemu_foreign_boot_dir_before, directory,
           sizeof(directory));

    for (index = 0U; index < INSTALLER_QEMU_ESP_LOADER_SECTORS; ++index) {
        size_t byte;

        memset(sector, 0, sizeof(sector));
        for (byte = 0U; byte < INSTALLER_QEMU_ESP_SECTOR; ++byte) {
            const size_t offset =
                index * INSTALLER_QEMU_ESP_SECTOR + byte;

            sector[byte] = (unsigned char)(0xB0U + (offset % 7U));
        }
        memcpy(installer_qemu_foreign_fallback_before[index], sector,
               sizeof(sector));
        installer_qemu_sector_store(
            base + INSTALLER_QEMU_ESP_DATA_START +
                (INSTALLER_QEMU_ESP_FALLBACK_CLUSTER - 2U) + index,
            sector);
    }
}

/*
 * ITEM-398: stage the foreign `\EFI\BOOT\limine.conf` the way the machine that
 * owned the ESP would have it.
 *
 * `conf` is a four-character extension, so the name is a VFAT long name with a
 * derived short entry (`LIMINE~1CON`) rather than a plain 8.3 entry - and it is
 * the reader under test that decides what "correctly stored" means.  Writing it
 * through the ESP writer therefore keeps one encoding in the proof instead of a
 * second, hand-rolled one that could quietly disagree with it.
 */
static void installer_qemu_foreign_menu_write(void)
{
    struct efi_boot_environment environment;
    struct efi_fat32_esp esp;
    struct efi_fat32_esp_payload payload;

    memset(&environment, 0, sizeof(environment));
    memset(&esp, 0, sizeof(esp));
    installer_qemu_require(
        efi_boot_environment_init(
            &environment, EFI_BOOT_FIRMWARE_UEFI,
            &installer_qemu_block_device, NULL,
            installer_qemu_variable_read,
            installer_qemu_variable_write) == EFI_BOOT_ENVIRONMENT_OK &&
            efi_fat32_esp_bind(
                &environment, INSTALLER_QEMU_FOREIGN_ESP_FIRST,
                INSTALLER_QEMU_FOREIGN_ESP_LENGTH, &esp) ==
                EFI_FAT32_ESP_OK,
        "the foreign ESP binds for its own configuration");
    memset(&payload, 0, sizeof(payload));
    payload.path = PRODUCTION_INSTALLER_BOOT_MENU_PATH;
    payload.data = installer_qemu_foreign_menu;
    payload.size = sizeof(installer_qemu_foreign_menu);
    installer_qemu_require(
        efi_fat32_esp_install_payload_set(&esp, &payload, 1U) ==
            EFI_FAT32_ESP_OK,
        "the foreign configuration is on the ESP before the install");
}

static void installer_qemu_prepare_foreign_disk(void)
{
    unsigned char header[INSTALLER_QEMU_ESP_SECTOR];
    uint64_t backup_lba = INSTALLER_QEMU_MEDIA_LBA - 1U;
    uint64_t backup_array_lba =
        INSTALLER_QEMU_MEDIA_LBA - INSTALLER_QEMU_FOREIGN_ARRAY_SECTORS - 1U;
    size_t index;

    memset(installer_qemu_foreign_array, 0, sizeof(installer_qemu_foreign_array));
    installer_qemu_foreign_entry(
        installer_qemu_foreign_array, 0U,
        installer_qemu_foreign_esp_guid, INSTALLER_QEMU_FOREIGN_ESP_FIRST,
        INSTALLER_QEMU_FOREIGN_ESP_FIRST + INSTALLER_QEMU_FOREIGN_ESP_LENGTH - 1U,
        "EFI System Partition");
    installer_qemu_foreign_entry(
        installer_qemu_foreign_array, 1U,
        installer_qemu_foreign_linux_guid, INSTALLER_QEMU_FOREIGN_ROOT_FIRST,
        INSTALLER_QEMU_FOREIGN_ROOT_FIRST + INSTALLER_QEMU_FOREIGN_ROOT_LENGTH - 1U,
        "Debian GNU/Linux");
    installer_qemu_foreign_entry(
        installer_qemu_foreign_array, 2U,
        installer_qemu_foreign_linux_guid, INSTALLER_QEMU_FOREIGN_DATA_FIRST,
        INSTALLER_QEMU_FOREIGN_DATA_FIRST + INSTALLER_QEMU_FOREIGN_DATA_LENGTH - 1U,
        "Debian home");
    installer_qemu_foreign_entry(
        installer_qemu_foreign_array, 3U,
        installer_qemu_foreign_linux_guid, INSTALLER_QEMU_FOREIGN_HIGH_FIRST,
        INSTALLER_QEMU_FOREIGN_HIGH_FIRST + INSTALLER_QEMU_FOREIGN_HIGH_LENGTH - 1U,
        "Debian above 4 GiB");
    for (index = 0U; index < INSTALLER_QEMU_FOREIGN_ARRAY_SECTORS; ++index) {
        installer_qemu_sector_store(
            2U + index,
            installer_qemu_foreign_array +
                index * INSTALLER_QEMU_ESP_SECTOR);
        installer_qemu_sector_store(
            backup_array_lba + index,
            installer_qemu_foreign_array +
                index * INSTALLER_QEMU_ESP_SECTOR);
    }
    installer_qemu_foreign_header(
        header, 1U, backup_lba, 2U, installer_qemu_foreign_array);
    installer_qemu_sector_store(1U, header);
    installer_qemu_foreign_header(
        header, backup_lba, 1U, backup_array_lba,
        installer_qemu_foreign_array);
    installer_qemu_sector_store(backup_lba, header);
    installer_qemu_foreign_mbr();
    installer_qemu_foreign_esp();
    installer_qemu_foreign_menu_write();
    memcpy(installer_qemu_foreign_array_before,
           installer_qemu_foreign_array,
           sizeof(installer_qemu_foreign_array));
}

static bool installer_qemu_foreign_slot_bytes(
    uint32_t slot,
    unsigned char out[INSTALLER_QEMU_FOREIGN_ENTRY_SIZE])
{
    const struct installer_qemu_sector *sector = installer_qemu_sector_find(
        2U + slot / INSTALLER_QEMU_FOREIGN_ENTRIES_PER_SECTOR);

    if (sector == NULL) {
        return false;
    }
    memcpy(out,
           sector->data +
               (slot % INSTALLER_QEMU_FOREIGN_ENTRIES_PER_SECTOR) *
                   INSTALLER_QEMU_FOREIGN_ENTRY_SIZE,
           INSTALLER_QEMU_FOREIGN_ENTRY_SIZE);
    return true;
}

static void installer_qemu_check_foreign_preserved(void)
{
    unsigned char bytes[INSTALLER_QEMU_FOREIGN_ENTRY_SIZE];
    uint32_t slot;

    for (slot = 0U; slot < 4U; ++slot) {
        installer_qemu_require(
            installer_qemu_foreign_slot_bytes(slot, bytes) &&
                memcmp(bytes,
                       installer_qemu_foreign_array_before +
                           (size_t)slot *
                               INSTALLER_QEMU_FOREIGN_ENTRY_SIZE,
                       INSTALLER_QEMU_FOREIGN_ENTRY_SIZE) == 0,
            "foreign partition entry preserved byte for byte");
    }
    installer_qemu_require(
        memcmp(installer_qemu_sector_find(0U)->data,
               installer_qemu_foreign_mbr_before,
               INSTALLER_QEMU_ESP_SECTOR) == 0,
        "MBR boot code and protective entry preserved");
}

static void installer_qemu_check_foreign_esp_preserved(void)
{
    const struct installer_qemu_sector *sector;
    uint64_t base = INSTALLER_QEMU_FOREIGN_ESP_FIRST;
    size_t index;

    installer_qemu_require(
        memcmp(installer_qemu_sector_find(
                   base + INSTALLER_QEMU_ESP_DATA_START +
                       (INSTALLER_QEMU_ESP_DEBIAN_CLUSTER - 2U))->data,
               installer_qemu_foreign_debian_dir_before,
               INSTALLER_QEMU_ESP_SECTOR) == 0,
        "the foreign EFI\\DEBIAN directory is byte-identical");
    for (index = 0U; index < INSTALLER_QEMU_ESP_LOADER_SECTORS; ++index) {
        installer_qemu_require(
            memcmp(installer_qemu_sector_find(
                       base + INSTALLER_QEMU_ESP_DATA_START +
                           (INSTALLER_QEMU_ESP_LOADER_CLUSTER - 2U) +
                           index)->data,
                   installer_qemu_foreign_loader_before[index],
                   INSTALLER_QEMU_ESP_SECTOR) == 0,
            "the foreign GRUBX64.EFI data is byte-identical");
    }
    sector = installer_qemu_sector_find(
        base + INSTALLER_QEMU_ESP_DATA_START +
            (INSTALLER_QEMU_ESP_CONFIG_CLUSTER - 2U));
    installer_qemu_require(
        sector != NULL &&
            memcmp(sector->data, installer_qemu_foreign_config_before,
                   INSTALLER_QEMU_ESP_SECTOR) == 0,
        "the foreign GRUB.CFG data is byte-identical");
}

/*
 * ITEM-396 Case B: build the second disk - a protective MBR, a GPT whose ESP
 * carries Debian's shim, and a Debian root - in its own store, so the proof can
 * assert that the install read it and never wrote it.
 */
static void installer_qemu_prepare_nvme_disk(void)
{
    unsigned char header[INSTALLER_QEMU_ESP_SECTOR];
    unsigned char directory[INSTALLER_QEMU_ESP_SECTOR];
    unsigned char sector[INSTALLER_QEMU_ESP_SECTOR];
    uint64_t base = INSTALLER_QEMU_NVME_ESP_FIRST;
    uint64_t backup_lba = INSTALLER_QEMU_NVME_MEDIA_LBA - 1U;
    uint64_t backup_array_lba = INSTALLER_QEMU_NVME_MEDIA_LBA -
        INSTALLER_QEMU_FOREIGN_ARRAY_SECTORS - 1U;
    size_t index;

    memset(installer_qemu_nvme_array, 0,
           sizeof(installer_qemu_nvme_array));
    installer_qemu_foreign_entry(
        installer_qemu_nvme_array, 0U, installer_qemu_foreign_esp_guid,
        INSTALLER_QEMU_NVME_ESP_FIRST,
        INSTALLER_QEMU_NVME_ESP_FIRST +
            INSTALLER_QEMU_NVME_ESP_LENGTH - 1U,
        "EFI System Partition");
    installer_qemu_foreign_entry(
        installer_qemu_nvme_array, 1U, installer_qemu_foreign_linux_guid,
        INSTALLER_QEMU_NVME_ROOT_FIRST,
        INSTALLER_QEMU_NVME_ROOT_FIRST +
            INSTALLER_QEMU_NVME_ROOT_LENGTH - 1U,
        "Debian GNU/Linux");
    for (index = 0U; index < INSTALLER_QEMU_FOREIGN_ARRAY_SECTORS;
         ++index) {
        installer_qemu_nvme_store(
            2U + index,
            installer_qemu_nvme_array +
                index * INSTALLER_QEMU_ESP_SECTOR);
        installer_qemu_nvme_store(
            backup_array_lba + index,
            installer_qemu_nvme_array +
                index * INSTALLER_QEMU_ESP_SECTOR);
    }
    /* The GPT headers of this disk are built for its own size. */
    {
        unsigned char header_copy[INSTALLER_QEMU_ESP_SECTOR];
        uint32_t crc;

        memset(header, 0, sizeof(header));
        memcpy(header, "EFI PART", 8U);
        header[10U] = 0x01U;
        installer_qemu_store_le32(header + 12U, 92U);
        installer_qemu_store_le64(header + 24U, 1U);
        installer_qemu_store_le64(header + 32U, backup_lba);
        installer_qemu_store_le64(header + 40U, 34U);
        installer_qemu_store_le64(
            header + 48U, INSTALLER_QEMU_NVME_MEDIA_LBA - 34U);
        for (crc = 0U; crc < 16U; ++crc) {
            header[56U + crc] = (unsigned char)(0x60U + crc);
        }
        installer_qemu_store_le64(header + 72U, 2U);
        installer_qemu_store_le32(
            header + 80U, INSTALLER_QEMU_FOREIGN_ENTRY_COUNT);
        installer_qemu_store_le32(
            header + 84U, INSTALLER_QEMU_FOREIGN_ENTRY_SIZE);
        installer_qemu_store_le32(
            header + 88U,
            installer_qemu_crc32(
                installer_qemu_nvme_array,
                INSTALLER_QEMU_FOREIGN_ENTRY_COUNT *
                    INSTALLER_QEMU_FOREIGN_ENTRY_SIZE));
        installer_qemu_store_le32(
            header + 16U, installer_qemu_crc32(header, 92U));
        installer_qemu_nvme_store(1U, header);
        memcpy(header_copy, header, sizeof(header_copy));
        installer_qemu_store_le64(header_copy + 24U, backup_lba);
        installer_qemu_store_le64(header_copy + 32U, 1U);
        installer_qemu_store_le64(header_copy + 72U, backup_array_lba);
        /* The CRC covers the header with its own CRC field zeroed. */
        installer_qemu_store_le32(header_copy + 16U, 0U);
        installer_qemu_store_le32(
            header_copy + 16U, installer_qemu_crc32(header_copy, 92U));
        installer_qemu_nvme_store(backup_lba, header_copy);
    }
    {
        unsigned char mbr[INSTALLER_QEMU_ESP_SECTOR];

        installer_qemu_foreign_mbr_bytes(mbr);
        installer_qemu_nvme_store(0U, mbr);
    }

    /* The ESP: the same FAT32 geometry, with Debian's shim on it. */
    memset(sector, 0, sizeof(sector));
    sector[12U] = 0x02U;
    sector[13U] = 1U;
    sector[14U] = (unsigned char)INSTALLER_QEMU_ESP_RESERVED_SECTORS;
    sector[16U] = (unsigned char)INSTALLER_QEMU_ESP_FAT_COUNT;
    /* ITEM-405: the volume declares its own extent. */
    installer_qemu_store_le32(
        sector + 32U, (uint32_t)INSTALLER_QEMU_NVME_ESP_LENGTH);
    installer_qemu_store_le32(
        sector + 36U, INSTALLER_QEMU_ESP_SECTORS_PER_FAT);
    installer_qemu_store_le32(
        sector + 44U, INSTALLER_QEMU_ESP_ROOT_CLUSTER);
    sector[510U] = 0x55U;
    sector[511U] = 0xAAU;
    installer_qemu_nvme_store(base, sector);
    memset(sector, 0, sizeof(sector));
    installer_qemu_store_le32(sector, 0x0FFFFFF8U);
    installer_qemu_store_le32(sector + 4U, 0x0FFFFFFFU);
    installer_qemu_store_le32(
        sector + INSTALLER_QEMU_ESP_ROOT_CLUSTER * 4U,
        INSTALLER_QEMU_ESP_EOC);
    installer_qemu_store_le32(
        sector + INSTALLER_QEMU_ESP_EFI_CLUSTER * 4U,
        INSTALLER_QEMU_ESP_EOC);
    installer_qemu_store_le32(
        sector + INSTALLER_QEMU_ESP_DEBIAN_CLUSTER * 4U,
        INSTALLER_QEMU_ESP_EOC);
    installer_qemu_store_le32(
        sector + INSTALLER_QEMU_ESP_LOADER_CLUSTER * 4U,
        INSTALLER_QEMU_ESP_LOADER_CLUSTER + 1U);
    installer_qemu_store_le32(
        sector + (INSTALLER_QEMU_ESP_LOADER_CLUSTER + 1U) * 4U,
        INSTALLER_QEMU_ESP_EOC);
    installer_qemu_store_le32(
        sector + INSTALLER_QEMU_ESP_GRUB_CLUSTER * 4U,
        INSTALLER_QEMU_ESP_EOC);
    for (index = 0U; index < INSTALLER_QEMU_ESP_SECTORS_PER_FAT; ++index) {
        if (index != 0U) {
            memset(sector, 0, sizeof(sector));
        }
        installer_qemu_nvme_store(
            base + INSTALLER_QEMU_ESP_RESERVED_SECTORS + index, sector);
        installer_qemu_nvme_store(
            base + INSTALLER_QEMU_ESP_RESERVED_SECTORS +
                INSTALLER_QEMU_ESP_SECTORS_PER_FAT + index,
            sector);
    }
    memset(directory, 0, sizeof(directory));
    installer_qemu_foreign_fat_names(
        directory, "EFI", "", 0x10U, INSTALLER_QEMU_ESP_EFI_CLUSTER, 0U);
    installer_qemu_nvme_store(
        base + INSTALLER_QEMU_ESP_DATA_START +
            (INSTALLER_QEMU_ESP_ROOT_CLUSTER - 2U), directory);
    memset(directory, 0, sizeof(directory));
    installer_qemu_foreign_fat_names(
        directory, "DEBIAN", "", 0x10U,
        INSTALLER_QEMU_ESP_DEBIAN_CLUSTER, 0U);
    installer_qemu_nvme_store(
        base + INSTALLER_QEMU_ESP_DATA_START +
            (INSTALLER_QEMU_ESP_EFI_CLUSTER - 2U), directory);
    /*
     * ITEM-409: Debian boots through its shim, not through the GRUB behind it,
     * and this `\EFI\DEBIAN` says so the hard way - GRUB comes first in the
     * directory, and the chainload entry still has to name the shim.  A machine
     * with Secure Boot enabled refuses the unsigned GRUB outright, so an entry
     * for GRUB is an entry for a system that does not boot.
     */
    memset(directory, 0, sizeof(directory));
    installer_qemu_foreign_fat_names(
        directory, "GRUBX64", "EFI", 0x20U,
        INSTALLER_QEMU_ESP_GRUB_CLUSTER,
        INSTALLER_QEMU_NVME_GRUB_LENGTH);
    installer_qemu_foreign_fat_names(
        directory + INSTALLER_QEMU_ESP_DIRECTORY_ENTRY_SIZE,
        "SHIMX64", "EFI", 0x20U,
        INSTALLER_QEMU_ESP_LOADER_CLUSTER,
        INSTALLER_QEMU_NVME_SHIM_LENGTH);
    installer_qemu_nvme_store(
        base + INSTALLER_QEMU_ESP_DATA_START +
            (INSTALLER_QEMU_ESP_DEBIAN_CLUSTER - 2U), directory);
    for (index = 0U; index < INSTALLER_QEMU_NVME_SHIM_SECTORS; ++index) {
        size_t byte;

        memset(sector, 0, sizeof(sector));
        for (byte = 0U; byte < INSTALLER_QEMU_ESP_SECTOR; ++byte) {
            sector[byte] = (unsigned char)(0xA0U + (byte % 11U));
        }
        installer_qemu_nvme_store(
            base + INSTALLER_QEMU_ESP_DATA_START +
                (INSTALLER_QEMU_ESP_LOADER_CLUSTER - 2U) + index,
            sector);
    }
    memset(sector, 0, sizeof(sector));
    {
        size_t byte;

        for (byte = 0U; byte < INSTALLER_QEMU_ESP_SECTOR; ++byte) {
            sector[byte] = (unsigned char)(0xC0U + (byte % 7U));
        }
    }
    installer_qemu_nvme_store(
        base + INSTALLER_QEMU_ESP_DATA_START +
            (INSTALLER_QEMU_ESP_GRUB_CLUSTER - 2U), sector);
}

/* The device lookup the menu scan is given: nvme0n1 is disk 2. */
static struct block_device *installer_qemu_preserve_device(
    void *context,
    uint32_t disk_id)
{
    (void)context;
    if (disk_id == 1U) {
        return &installer_qemu_block_device;
    }
    if (disk_id == 2U) {
        return &installer_qemu_nvme_device;
    }
    return NULL;
}

static void installer_qemu_test_dual_boot_preserve(void)
{
    size_t loader_count = 0U;
    size_t readback_length = 0U;
    size_t record_length = 0U;
    size_t config_length;
    uint64_t esp_first = 0U;
    uint64_t esp_length = 0U;
    uint64_t first_lba = 0U;

    installer_qemu_reset_disk();
    /* The ESP is formatted by the fixture, before any product code runs. */
    installer_qemu_prepare_foreign_disk();
    memset(&installer_qemu_preserve_appliance, 0,
           sizeof(installer_qemu_preserve_appliance));
    memset(&installer_qemu_preserve_recovery, 0,
           sizeof(installer_qemu_preserve_recovery));
    installer_qemu_preserve_appliance.label = "HFOC OS";
    installer_qemu_preserve_appliance.path =
        INSTALLER_QEMU_KERNEL_BOOT_PATH;
    installer_qemu_preserve_appliance.module_count = 0U;
    /*
     * ITEM-399: the appliance entry states the management scope the medium
     * declared, and the menu the install writes carries it.
     */
    installer_qemu_preserve_appliance.cmdline =
        "hfoc.management=local-only";
    installer_qemu_preserve_recovery.label = "HFOC OS (recovery)";
    installer_qemu_preserve_recovery.path =
        INSTALLER_QEMU_RECOVERY_BOOT_PATH;
    installer_qemu_preserve_recovery.module_count = 0U;
    serial_write("[PRODUCTION-INSTALLER] foreign disk prepared\n");

    /*
     * The discovery scans the disk that is really there - the same call the
     * install flow makes on real hardware - so the plan below is built from the
     * foreign table and not from a hand-written fixture report.
     */
    memset(&installer_qemu_preserve_report, 0, sizeof(installer_qemu_preserve_report));
    installer_qemu_require(
        installer_discovery_init(&installer_qemu_preserve_report) ==
                INSTALLER_DISCOVERY_OK &&
            installer_discovery_publish_firmware(
                &installer_qemu_preserve_report,
                INSTALLER_FIRMWARE_UEFI) == INSTALLER_DISCOVERY_OK &&
            installer_discovery_publish_cpu(
                &installer_qemu_preserve_report, 4U) ==
                INSTALLER_DISCOVERY_OK &&
            installer_discovery_publish_memory(
                &installer_qemu_preserve_report, 4294967296ULL) ==
                INSTALLER_DISCOVERY_OK &&
            installer_discovery_scan_block_device(
                &installer_qemu_preserve_report,
                &installer_qemu_block_device, 1U,
                INSTALLER_DISK_CONTROLLER_VIRTIO_BLK,
                installer_qemu_disk_identity,
                sizeof(installer_qemu_disk_identity) - 1U) ==
                INSTALLER_DISCOVERY_OK &&
            installer_discovery_finalize(
                &installer_qemu_preserve_report) ==
                INSTALLER_DISCOVERY_OK,
        "the foreign disk is discovered");
    installer_qemu_require(
        installer_qemu_preserve_report.partition_count == 4U &&
            installer_qemu_preserve_report.os_count == 1U &&
            installer_qemu_preserve_report.disks[0U].has_efi_system_partition,
        "discovery finds the four foreign partitions and the Debian system");

    memset(&installer_qemu_preserve, 0, sizeof(installer_qemu_preserve));
    installer_qemu_media_init();
    installer_qemu_preserve_source = installer_qemu_source();
    installer_qemu_require(
        production_installer_init(&installer_qemu_preserve) ==
                PRODUCTION_INSTALLER_OK &&
            production_installer_prepare(
                &installer_qemu_preserve,
                &installer_qemu_preserve_report,
                PRODUCTION_INSTALLER_MODE_DUAL_BOOT, 1U,
                installer_qemu_disk_identity,
                sizeof(installer_qemu_disk_identity) - 1U,
                &installer_qemu_preserve_source, true) ==
                PRODUCTION_INSTALLER_OK,
        "dual-boot controller prepared");
    installer_qemu_hardening_bind(
        &installer_qemu_preserve, &installer_qemu_preserve_report);
    installer_qemu_require(
        production_installer_build_plan(&installer_qemu_preserve) ==
                PRODUCTION_INSTALLER_OK &&
            production_installer_plan_layout(
                &installer_qemu_preserve,
                &installer_qemu_preserve_report,
                &installer_qemu_preserve_plan,
                &installer_qemu_preserve_dual) ==
                PRODUCTION_INSTALLER_OK &&
            production_installer_preserve_reuse(
                &installer_qemu_preserve,
                &installer_qemu_preserve_reuse) ==
                PRODUCTION_INSTALLER_OK,
        "dual-boot layout planned from the contracted sizes");
    installer_qemu_require(
        installer_qemu_preserve.layout.mode ==
                INSTALLER_LAYOUT_CONTRACT_MODE_DUAL_BOOT &&
            installer_qemu_preserve.layout.esp_reused &&
            installer_qemu_preserve.layout.roles[0U].first_lba ==
                INSTALLER_QEMU_FOREIGN_ESP_FIRST &&
            installer_qemu_preserve.layout.roles[0U].length_lba ==
                INSTALLER_QEMU_FOREIGN_ESP_LENGTH &&
            installer_qemu_preserve_plan.partition_count == 3U &&
            !installer_qemu_preserve_plan.destructive &&
            installer_qemu_preserve_reuse.present &&
            installer_qemu_preserve_reuse.first_lba ==
                INSTALLER_QEMU_FOREIGN_ESP_FIRST,
        "the plan reuses the foreign ESP and adds three roles");

    /* The ESP inventory is read-only, and it is foreign loaders only. */
    memset(&installer_qemu_preserve_environment, 0,
           sizeof(installer_qemu_preserve_environment));
    installer_qemu_require(
        efi_boot_environment_init(
            &installer_qemu_preserve_environment, EFI_BOOT_FIRMWARE_UEFI,
            &installer_qemu_block_device, &installer_qemu_variables,
            installer_qemu_variable_read,
            installer_qemu_variable_write) == EFI_BOOT_ENVIRONMENT_OK &&
            efi_boot_environment_set_device_path(
                &installer_qemu_preserve_environment,
                installer_qemu_device_path,
                sizeof(installer_qemu_device_path)) ==
                EFI_BOOT_ENVIRONMENT_OK,
        "EFI environment for the shared ESP");
    memset(&installer_qemu_preserve_esp, 0, sizeof(installer_qemu_preserve_esp));
    installer_qemu_require(
        efi_fat32_esp_bind(
            &installer_qemu_preserve_environment,
            INSTALLER_QEMU_FOREIGN_ESP_FIRST,
            INSTALLER_QEMU_FOREIGN_ESP_LENGTH,
            &installer_qemu_preserve_esp) == EFI_FAT32_ESP_OK,
        "the foreign ESP binds as a FAT32 volume");
    installer_qemu_require(
        efi_fat32_esp_list_loaders(
            &installer_qemu_preserve_esp,
            installer_qemu_preserve_loaders,
            EFI_FAT32_ESP_LOADER_CAPACITY,
            &loader_count) == EFI_FAT32_ESP_OK,
        "the ESP loader inventory is read");
    installer_qemu_require(
        loader_count == 2U &&
            strcmp(installer_qemu_preserve_loaders[0U].vendor,
                   "DEBIAN") == 0 &&
            strcmp(installer_qemu_preserve_loaders[1U].vendor,
                   "BOOT") == 0,
        "the shared ESP carries the two foreign loaders it holds");
    installer_qemu_require(
        strcmp(installer_qemu_preserve_loaders[0U].path,
               "\\EFI\\DEBIAN\\GRUBX64.EFI") == 0 &&
            installer_qemu_preserve_loaders[0U].size ==
                INSTALLER_QEMU_ESP_LOADER_LENGTH &&
            strcmp(installer_qemu_preserve_loaders[1U].path,
                   "\\EFI\\BOOT\\BOOTX64.EFI") == 0 &&
            installer_qemu_preserve_loaders[1U].size ==
                INSTALLER_QEMU_ESP_LOADER_LENGTH,
        "the shared ESP carries Debian's loader and the fallback loader");
    serial_write(
        "[PRODUCTION-INSTALLER] shared ESP inventory proof OK\n");

    installer_qemu_require(
        production_installer_plan_preserve(
            &installer_qemu_preserve, &installer_qemu_preserve_report,
            installer_qemu_preserve_device, NULL,
            &installer_qemu_preserve_appliance,
            &installer_qemu_preserve_recovery,
            &installer_qemu_preserve_menu,
            &installer_qemu_preserve_inventory) ==
                PRODUCTION_INSTALLER_OK &&
            installer_qemu_preserve.preserve_ready,
        "the boot menu is planned from the discovery and the ESP");
    installer_qemu_require(
        installer_qemu_contains(installer_qemu_preserve_menu.text, "/HFOC OS") &&
            installer_qemu_contains(
                installer_qemu_preserve_menu.text, "/Debian GNU/Linux") &&
            installer_qemu_contains(
                installer_qemu_preserve_menu.text, "protocol: efi") &&
            installer_qemu_contains(
                installer_qemu_preserve_menu.text,
                "path: boot():/EFI/DEBIAN/GRUBX64.EFI") &&
            installer_qemu_preserve_menu.entry_count == 3U &&
            installer_qemu_preserve_menu.missing_count == 0U &&
            installer_qemu_preserve_inventory.disk_count == 1U,
        "the generated Limine configuration lists both systems");

    installer_qemu_require(
        production_installer_publish_preview(&installer_qemu_preserve) ==
                PRODUCTION_INSTALLER_OK &&
            installer_qemu_contains(
                installer_qemu_preserve.preview, "preserve preserved=4") &&
            installer_qemu_contains(
                installer_qemu_preserve.preview, "menu_entries=3") &&
            installer_qemu_contains(
                installer_qemu_preserve.preview, "esp_reused=1"),
        "the plan preview publishes the preservation");
    installer_qemu_require(
        installer_gpt_preserve_read(
            &installer_qemu_block_device, 1U,
            &installer_qemu_preserve_gpt) == INSTALLER_GPT_PRESERVE_OK &&
            installer_qemu_preserve_gpt.read_complete &&
            installer_qemu_preserve_gpt.preserved_count == 4U &&
            production_installer_publish_preserve_record(
                &installer_qemu_preserve, &installer_qemu_preserve_plan,
                &installer_qemu_preserve_reuse,
                &installer_qemu_preserve_gpt,
                &installer_qemu_preserve_menu,
                &installer_qemu_preserve_inventory) ==
                PRODUCTION_INSTALLER_OK,
        "the preservation record and the menu are published before the write");
    serial_write(
        "[PRODUCTION-INSTALLER] preserve preview proof OK\n");

    installer_qemu_require(
        production_installer_confirm(&installer_qemu_preserve) ==
                PRODUCTION_INSTALLER_OK &&
            installer_layout_confirm(&installer_qemu_preserve_plan) ==
                INSTALLER_LAYOUT_OK,
        "controller and plan confirmation");
    installer_qemu_require(
        production_installer_execute_layout_preserving(
            &installer_qemu_preserve, &installer_qemu_block_device,
            &installer_qemu_preserve_plan,
            &installer_qemu_preserve_reuse,
            &installer_qemu_preserve_gpt) == PRODUCTION_INSTALLER_OK &&
            installer_qemu_preserve_plan.executed,
        "the preserving GPT write succeeds");
    installer_qemu_require(
        installer_gpt_preserve_verify(
            &installer_qemu_block_device,
            &installer_qemu_preserve_gpt) == INSTALLER_GPT_PRESERVE_OK &&
            installer_qemu_preserve_gpt.added_count == 3U &&
            installer_qemu_preserve_gpt.esp_reused &&
            installer_qemu_preserve_gpt.esp_index == 0U,
        "the written table verifies");
    installer_qemu_check_foreign_preserved();
    {
        unsigned char slot_bytes[INSTALLER_QEMU_FOREIGN_ENTRY_SIZE];

        installer_qemu_require(
            installer_qemu_foreign_slot_bytes(4U, slot_bytes) &&
                memcmp(slot_bytes,
                       installer_qemu_foreign_array_before +
                           4U * INSTALLER_QEMU_FOREIGN_ENTRY_SIZE,
                       INSTALLER_QEMU_FOREIGN_ENTRY_SIZE) != 0,
            "the appliance's first added role takes a free slot");
    }
    (void)first_lba;
    serial_write(
        "[PRODUCTION-INSTALLER] preserved table proof OK\n");

    /*
     * The refusals, on the same disk: a corrupt table is refused, an
     * overlapping request is refused before the first write, and an ESP that is
     * not a volume this module can use is refused without formatting it.
     */
    {
        struct installer_layout_plan conflict;
        size_t write_calls_before;

        installer_qemu_sector_find(2U)->data[0U] ^= 0xFFU;
        memset(&installer_qemu_preserve_gpt, 0,
               sizeof(installer_qemu_preserve_gpt));
        installer_qemu_require(
            installer_gpt_preserve_read(
                &installer_qemu_block_device, 1U,
                &installer_qemu_preserve_gpt) ==
                INSTALLER_GPT_PRESERVE_CORRUPT_TABLE,
            "a corrupt foreign table is refused");
        installer_qemu_sector_find(2U)->data[0U] ^= 0xFFU;
        installer_qemu_require(
            installer_gpt_preserve_read(
                &installer_qemu_block_device, 1U,
                &installer_qemu_preserve_gpt) ==
                INSTALLER_GPT_PRESERVE_OK,
            "the table reads again once the damage is undone");

        memset(&conflict, 0, sizeof(conflict));
        conflict.version = INSTALLER_LAYOUT_VERSION;
        conflict.mode = INSTALLER_LAYOUT_MODE_WHOLE_DISK;
        conflict.disk_id = 1U;
        conflict.block_size = INSTALLER_QEMU_BLOCK_SIZE;
        conflict.block_count = INSTALLER_QEMU_MEDIA_LBA;
        conflict.confirmed = true;
        conflict.partition_count = 1U;
        conflict.partitions[0U].present = true;
        conflict.partitions[0U].role = INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM;
        conflict.partitions[0U].partition_index = 1U;
        conflict.partitions[0U].first_lba = INSTALLER_QEMU_FOREIGN_ROOT_FIRST;
        conflict.partitions[0U].length_lba = 32768U;
        write_calls_before = installer_qemu_write_calls;
        installer_qemu_require(
            installer_gpt_preserve_write(
                &installer_qemu_block_device, &conflict,
                &installer_qemu_preserve_reuse,
                &installer_qemu_preserve_gpt) ==
                INSTALLER_GPT_PRESERVE_EXISTING_SYSTEM_OVERLAP &&
            installer_qemu_write_calls == write_calls_before,
            "an overlapping request is refused before the first write");
        write_calls_before = installer_qemu_write_calls;
        {
            struct efi_fat32_esp probe;

            memset(&probe, 0, sizeof(probe));
            installer_qemu_require(
                production_installer_esp_bind(
                    &installer_qemu_preserve_environment,
                    INSTALLER_QEMU_FOREIGN_ROOT_FIRST, 1000000U, false,
                    &probe) != PRODUCTION_INSTALLER_OK &&
                    installer_qemu_write_calls == write_calls_before,
                "an ESP that cannot host our loader is refused "
                "unformatted");
        }
    }
    serial_write(
        "[PRODUCTION-INSTALLER] dual-boot refusal proof OK\n");

    installer_qemu_require(
        production_installer_preserve_esp(
            &installer_qemu_preserve, &installer_qemu_preserve_report,
            &esp_first, &esp_length) == PRODUCTION_INSTALLER_OK &&
            esp_first == INSTALLER_QEMU_FOREIGN_ESP_FIRST &&
            esp_length == INSTALLER_QEMU_FOREIGN_ESP_LENGTH,
        "the shared ESP is found from the plan");
    config_length = strlen(installer_qemu_preserve_menu.text);
    /*
     * The product flow stages its installed boot set through the boot stage
     * (ITEM-397/398), which is the only writer of these files; this phase proves
     * the shared-ESP behaviour of that path's ESP handling - bind as it is,
     * never format - and the preservation of what was already there.
     */
    installer_qemu_require(
        production_installer_esp_bind(
            &installer_qemu_preserve_environment, esp_first, esp_length,
            false, &installer_qemu_preserve_esp) ==
                PRODUCTION_INSTALLER_OK &&
            efi_fat32_esp_install_payload(
                &installer_qemu_preserve_esp,
                installer_qemu_preserve_source.boot_payload,
                installer_qemu_preserve_source.boot_payload_size) ==
                EFI_FAT32_ESP_OK &&
            efi_fat32_esp_install_config(
                &installer_qemu_preserve_esp,
                (const unsigned char *)
                    installer_qemu_preserve_menu.text,
                config_length) == EFI_FAT32_ESP_OK,
        "the appliance's files are added to the shared ESP");
    installer_qemu_check_foreign_esp_preserved();
    loader_count = 0U;
    installer_qemu_require(
        efi_fat32_esp_bind(
            &installer_qemu_preserve_environment, esp_first, esp_length,
            &installer_qemu_preserve_esp) == EFI_FAT32_ESP_OK &&
            efi_fat32_esp_list_loaders(
                &installer_qemu_preserve_esp,
                installer_qemu_preserve_loaders,
                EFI_FAT32_ESP_LOADER_CAPACITY,
                &loader_count) == EFI_FAT32_ESP_OK,
        "the ESP loader inventory is read after the install");
    installer_qemu_require(
        loader_count == 2U &&
            strcmp(installer_qemu_preserve_loaders[0U].path,
                   "\\EFI\\DEBIAN\\GRUBX64.EFI") == 0 &&
            strcmp(installer_qemu_preserve_loaders[1U].path,
                   "\\EFI\\BOOT\\BOOTX64.EFI") == 0,
        "both foreign loaders are still on the ESP after the install");
    installer_qemu_require(
        efi_fat32_esp_read_config(
            &installer_qemu_preserve_esp,
            installer_qemu_menu_readback,
            sizeof(installer_qemu_menu_readback),
            &readback_length) == EFI_FAT32_ESP_OK &&
            readback_length == config_length &&
            memcmp(installer_qemu_menu_readback,
                   installer_qemu_preserve_menu.text,
                   config_length) == 0,
        "the ESP carries exactly the published Limine configuration");
    /*
     * ITEM-502: the preserving install performs the same explicit erase step
     * before verification; only the plan's own three roles are written, so the
     * foreign partitions and the shared ESP stay byte-for-byte.
     */
    {
        struct appliance_state_layout preserve_state_layout;
        struct appliance_state_loader preserve_loader;
        struct production_installer_state_erase_evidence preserve_erase;

        memset(&preserve_state_layout, 0, sizeof(preserve_state_layout));
        memset(&preserve_loader, 0, sizeof(preserve_loader));
        memset(&preserve_erase, 0, sizeof(preserve_erase));
        installer_qemu_require(
            appliance_state_layout_default(&preserve_state_layout) ==
                    APPLIANCE_STATE_LAYOUT_OK &&
                production_installer_state_erase(
                    &installer_qemu_preserve,
                    &installer_qemu_block_device, &preserve_state_layout,
                    &installer_qemu_preserve_plan, &preserve_loader,
                    &preserve_erase) == PRODUCTION_INSTALLER_OK &&
                preserve_erase.verify_clean,
            "the dual-boot install erases the state before it verifies");
    }
    installer_qemu_require(
        production_installer_verify_layout(
            &installer_qemu_preserve, &installer_qemu_block_device,
            &installer_qemu_preserve_plan) == PRODUCTION_INSTALLER_OK &&
            installer_qemu_preserve.stage ==
                PRODUCTION_INSTALLER_STAGE_VERIFIED,
        "the dual-boot install verifies");
    serial_write(
        "[PRODUCTION-INSTALLER] shared ESP reuse proof OK\n");
    serial_write(
        "[PRODUCTION-INSTALLER] boot menu proof OK\n");

    /*
     * ITEM-396 Case B (owner refinement, 2026-09-13): Debian lives on the
     * machine's other disk and the appliance goes to this one.  The menu must
     * offer Debian by reading that disk's ESP - through Limine's guid()
     * resource, because a firmware drive number is not stable - and it must
     * never write a byte of it.  The second disk has its own store and its own
     * write counter, so "never" is asserted rather than assumed.
     */
    {
        struct installer_discovery_report *report =
            &installer_qemu_preserve_report;
        size_t nvme_writes_before = installer_qemu_nvme_write_calls;
        uint32_t disk;

        installer_qemu_reset_disk();
        installer_qemu_media_init();
        installer_qemu_prepare_foreign_disk();
        installer_qemu_prepare_nvme_disk();
        installer_qemu_preserve_source = installer_qemu_source();

        memset(report, 0, sizeof(*report));
        installer_qemu_require(
            installer_discovery_init(report) ==
                    INSTALLER_DISCOVERY_OK &&
                installer_discovery_publish_firmware(
                    report, INSTALLER_FIRMWARE_UEFI) ==
                    INSTALLER_DISCOVERY_OK &&
                installer_discovery_publish_cpu(report, 4U) ==
                    INSTALLER_DISCOVERY_OK &&
                installer_discovery_publish_memory(
                    report, 4294967296ULL) == INSTALLER_DISCOVERY_OK &&
                installer_discovery_scan_block_device(
                    report, &installer_qemu_block_device, 1U,
                    INSTALLER_DISK_CONTROLLER_VIRTIO_BLK,
                    installer_qemu_disk_identity,
                    sizeof(installer_qemu_disk_identity) - 1U) ==
                    INSTALLER_DISCOVERY_OK &&
                installer_discovery_scan_block_device(
                    report, &installer_qemu_nvme_device, 2U,
                    INSTALLER_DISK_CONTROLLER_NVME,
                    "qemu-debian-disk",
                    sizeof("qemu-debian-disk") - 1U) ==
                    INSTALLER_DISCOVERY_OK &&
                installer_discovery_finalize(report) ==
                    INSTALLER_DISCOVERY_OK,
            "both disks are discovered");
        installer_qemu_require(
            report->disk_count == 2U && report->os_count == 1U,
            "the report carries the appliance disk and Debian disk");

        memset(&installer_qemu_preserve, 0,
               sizeof(installer_qemu_preserve));
        installer_qemu_require(
            production_installer_init(&installer_qemu_preserve) ==
                    PRODUCTION_INSTALLER_OK &&
                production_installer_prepare(
                    &installer_qemu_preserve, report,
                    PRODUCTION_INSTALLER_MODE_DUAL_BOOT, 1U,
                    installer_qemu_disk_identity,
                    sizeof(installer_qemu_disk_identity) - 1U,
                    &installer_qemu_preserve_source, true) ==
                    PRODUCTION_INSTALLER_OK,
            "the dual-boot controller targets the appliance disk");
        installer_qemu_hardening_bind(&installer_qemu_preserve, report);
        installer_qemu_require(
            production_installer_build_plan(&installer_qemu_preserve) ==
                    PRODUCTION_INSTALLER_OK &&
                production_installer_plan_layout(
                    &installer_qemu_preserve, report,
                    &installer_qemu_preserve_plan,
                    &installer_qemu_preserve_dual) ==
                    PRODUCTION_INSTALLER_OK &&
                production_installer_preserve_reuse(
                    &installer_qemu_preserve,
                    &installer_qemu_preserve_reuse) ==
                    PRODUCTION_INSTALLER_OK,
            "the appliance layout is planned on its own disk");
        installer_qemu_require(
            production_installer_plan_preserve(
                &installer_qemu_preserve, report,
                installer_qemu_preserve_device, NULL,
                &installer_qemu_preserve_appliance,
                &installer_qemu_preserve_recovery,
                &installer_qemu_preserve_menu,
                &installer_qemu_preserve_inventory) ==
                    PRODUCTION_INSTALLER_OK,
            "the menu is planned across both disks");
        installer_qemu_require(
            installer_qemu_preserve_inventory.disk_count == 2U,
            "the inventory covers both disks");
        for (disk = 0U;
             disk < installer_qemu_preserve_inventory.disk_count;
             ++disk) {
            const struct installer_boot_menu_disk *record =
                &installer_qemu_preserve_inventory.disks[disk];

            if (record->disk_id == 2U) {
                installer_qemu_require(
                    record->scan == INSTALLER_BOOT_MENU_SCAN_OK &&
                        record->loader_count == 1U &&
                        strcmp(record->loaders[0U].vendor, "DEBIAN") ==
                            0,
                    "Debian loader is found on the other disk");
            }
        }
        installer_qemu_require(
            installer_qemu_contains(
                installer_qemu_preserve_menu.text, "path: guid(") &&
                installer_qemu_contains(
                    installer_qemu_preserve_menu.text,
                    ":/EFI/DEBIAN/SHIMX64.EFI") &&
                installer_qemu_contains(
                    installer_qemu_preserve_menu.text,
                    "/Debian GNU/Linux"),
            "the menu chainloads Debian through guid() on the other disk");
        /*
         * ITEM-399: the appliance entry carries the declared management scope,
         * so the disk this install writes boots with the scope the medium
         * declared instead of the kernel's default.
         */
        installer_qemu_require(
            installer_qemu_contains(
                installer_qemu_preserve_menu.text,
                "cmdline: hfoc.management=local-only"),
            "the appliance entry declares the medium's management scope");
        installer_qemu_require(
            installer_boot_menu_render_record(
                &installer_qemu_preserve_menu,
                &installer_qemu_preserve_inventory,
                installer_qemu_menu_record,
                sizeof(installer_qemu_menu_record),
                &record_length) == INSTALLER_BOOT_MENU_OK &&
                record_length > 0U &&
                installer_qemu_contains(
                    installer_qemu_menu_record,
                    "cmdline=hfoc.management=local-only"),
            "the menu record publishes the declared management scope");
        installer_qemu_require(
            production_installer_publish_preview(
                &installer_qemu_preserve) ==
                    PRODUCTION_INSTALLER_OK &&
                installer_gpt_preserve_read(
                    &installer_qemu_block_device, 1U,
                    &installer_qemu_preserve_gpt) ==
                    INSTALLER_GPT_PRESERVE_OK &&
                production_installer_publish_preserve_record(
                    &installer_qemu_preserve,
                    &installer_qemu_preserve_plan,
                    &installer_qemu_preserve_reuse,
                    &installer_qemu_preserve_gpt,
                    &installer_qemu_preserve_menu,
                    &installer_qemu_preserve_inventory) ==
                    PRODUCTION_INSTALLER_OK,
            "the cross-disk plan is published before the write");
        installer_qemu_require(
            production_installer_confirm(&installer_qemu_preserve) ==
                    PRODUCTION_INSTALLER_OK &&
                installer_layout_confirm(
                    &installer_qemu_preserve_plan) ==
                    INSTALLER_LAYOUT_OK &&
                production_installer_execute_layout_preserving(
                    &installer_qemu_preserve,
                    &installer_qemu_block_device,
                    &installer_qemu_preserve_plan,
                    &installer_qemu_preserve_reuse,
                    &installer_qemu_preserve_gpt) ==
                    PRODUCTION_INSTALLER_OK,
            "the appliance is installed on its own disk");
        installer_qemu_require(
            installer_qemu_nvme_write_calls == nvme_writes_before,
            "the Debian disk was not written by the install");
        serial_write(
            "[PRODUCTION-INSTALLER] cross-disk menu proof OK\n");

        /*
         * And when that disk cannot be read at all, the menu still comes up and
         * the run says, per disk, what it could not offer.
         */
        installer_qemu_nvme_find(1U)->data[16U] ^= 0x01U;
        installer_qemu_require(
            installer_boot_menu_scan(
                report, 1U, installer_qemu_preserve_device, NULL,
                &installer_qemu_preserve_inventory) ==
                    INSTALLER_BOOT_MENU_OK,
            "the scan completes with an unreadable second disk");
        for (disk = 0U;
             disk < installer_qemu_preserve_inventory.disk_count;
             ++disk) {
            const struct installer_boot_menu_disk *record =
                &installer_qemu_preserve_inventory.disks[disk];

            if (record->disk_id == 2U) {
                installer_qemu_require(
                    record->scan != INSTALLER_BOOT_MENU_SCAN_OK &&
                        record->loader_count == 0U,
                    "the unreadable disk is reported, not ignored");
            }
        }
        installer_qemu_require(
            installer_boot_menu_build(
                report, &installer_qemu_preserve_inventory,
                &installer_qemu_preserve_appliance,
                &installer_qemu_preserve_recovery,
                &installer_qemu_preserve_menu) ==
                    INSTALLER_BOOT_MENU_OK &&
                installer_qemu_contains(
                    installer_qemu_preserve_menu.text, "/HFOC OS") &&
                installer_qemu_preserve_menu.unreadable_disk_count >= 1U &&
                installer_qemu_contains(
                    installer_qemu_preserve_menu.text, "reason=CORRUPT"),
            "the menu still comes up and names the disk it could not read");
        /*
         * The run publishes what it found and what it could not, per disk, so
         * the acceptance record states which system was not offered and why.
         */
        installer_qemu_require(
            installer_boot_menu_render_record(
                &installer_qemu_preserve_menu,
                &installer_qemu_preserve_inventory,
                installer_qemu_menu_record,
                sizeof(installer_qemu_menu_record),
                &record_length) == INSTALLER_BOOT_MENU_OK &&
                record_length > 0U &&
                installer_qemu_contains(
                    installer_qemu_menu_record, "disk id=2") &&
                installer_qemu_contains(
                    installer_qemu_menu_record, "scan=CORRUPT"),
            "the unreadable disk is published with its reason");
        serial_write(installer_qemu_menu_record);
        /*
         * And with no loadable ESP anywhere, the menu still comes up, counts the
         * system it could not offer, and says why in the configuration itself.
         */
        memset(&installer_qemu_preserve_inventory, 0,
               sizeof(installer_qemu_preserve_inventory));
        installer_qemu_preserve_inventory.version =
            INSTALLER_BOOT_MENU_VERSION;
        installer_qemu_preserve_inventory.target_disk_id = 1U;
        installer_qemu_preserve_inventory.disk_count = 2U;
        installer_qemu_preserve_inventory.disks[0U].disk_id = 1U;
        installer_qemu_preserve_inventory.disks[0U].scan =
            INSTALLER_BOOT_MENU_SCAN_NO_DEVICE;
        installer_qemu_preserve_inventory.disks[1U].disk_id = 2U;
        installer_qemu_preserve_inventory.disks[1U].scan =
            INSTALLER_BOOT_MENU_SCAN_NO_DEVICE;
        installer_qemu_require(
            installer_boot_menu_build(
                report, &installer_qemu_preserve_inventory,
                &installer_qemu_preserve_appliance,
                &installer_qemu_preserve_recovery,
                &installer_qemu_preserve_menu) ==
                    INSTALLER_BOOT_MENU_OK &&
                installer_qemu_preserve_menu.missing_count == 1U &&
                installer_qemu_preserve_menu.entry_count == 2U &&
                installer_qemu_contains(
                    installer_qemu_preserve_menu.text, "reason=NO_LOADER"),
            "with no loader anywhere the menu still comes up and says so");        serial_write(
            "[PRODUCTION-INSTALLER] unreadable second disk proof OK\n");
    }


}

/*
 * ITEM-398: the boot stage on a shared ESP that already carries the two paths
 * the installed set uses.
 *
 * `\EFI\BOOT\BOOTX64.EFI` is the removable-media fallback a firmware tries when
 * no boot entry matches, so a machine with another system on it frequently has
 * that system's own fallback loader there; `\EFI\BOOT\limine.conf` is the first
 * configuration a Limine reads, so a foreign file there would silently shadow
 * the menu this install writes.  The proof:
 *
 *   * runs the real product order on the foreign disk - plan, contracted
 *     dual-boot layout, menu, publication, confirmation, preserving write - and
 *     then the boot stage, the only writer of these files;
 *   * asserts both occupants are still on the volume byte for byte under the
 *     documented `.PRE` names, with the sizes the record published;
 *   * asserts the removable-media path and the loader's first configuration
 *     path now carry this install's own loader and generated menu - which is
 *     what "the shadowing file was taken over, not lost" means;
 *   * and drains the volume afterwards to prove the second line of defence: a
 *     shared ESP that passes the declared-size gate but has no room for the set
 *     is refused with zero writes, from the read-only free-cluster pre-flight.
 */
static void installer_qemu_test_boot_stage_occupied_paths(void)
{
    static unsigned char readback[INSTALLER_QEMU_ESP_LOADER_SECTORS *
                                   INSTALLER_QEMU_ESP_SECTOR];
    static unsigned char fat_fill[INSTALLER_QEMU_ESP_SECTOR];
    struct production_installer_boot_media boot_media;
    struct production_installer_boot_outcome outcome;
    struct production_installer_boot_esp boot_esp;
    struct efi_boot_environment environment;
    struct efi_fat32_esp esp;
    size_t readback_length = 0U;
    size_t menu_length;
    size_t write_calls_before;
    uint64_t esp_first = 0U;
    uint64_t esp_length = 0U;
    uint32_t index;

    installer_qemu_reset_disk();
    installer_qemu_media_init();
    installer_qemu_prepare_foreign_disk();

    /*
     * The discovery scans the disk that is really there, exactly as the install
     * flow does, so the plan below is built from the foreign table.
     */
    memset(&installer_qemu_preserve_report, 0,
           sizeof(installer_qemu_preserve_report));
    installer_qemu_require(
        installer_discovery_init(&installer_qemu_preserve_report) ==
                INSTALLER_DISCOVERY_OK &&
            installer_discovery_publish_firmware(
                &installer_qemu_preserve_report,
                INSTALLER_FIRMWARE_UEFI) == INSTALLER_DISCOVERY_OK &&
            installer_discovery_publish_cpu(
                &installer_qemu_preserve_report, 4U) ==
                INSTALLER_DISCOVERY_OK &&
            installer_discovery_publish_memory(
                &installer_qemu_preserve_report, 4294967296ULL) ==
                INSTALLER_DISCOVERY_OK &&
            installer_discovery_scan_block_device(
                &installer_qemu_preserve_report,
                &installer_qemu_block_device, 1U,
                INSTALLER_DISK_CONTROLLER_VIRTIO_BLK,
                installer_qemu_disk_identity,
                sizeof(installer_qemu_disk_identity) - 1U) ==
                INSTALLER_DISCOVERY_OK &&
            installer_discovery_finalize(
                &installer_qemu_preserve_report) ==
                INSTALLER_DISCOVERY_OK,
        "the foreign disk is discovered for the occupied-path proof");

    memset(&installer_qemu_preserve, 0, sizeof(installer_qemu_preserve));
    installer_qemu_preserve_source = installer_qemu_source();
    installer_qemu_require(
        production_installer_init(&installer_qemu_preserve) ==
                PRODUCTION_INSTALLER_OK &&
            production_installer_prepare(
                &installer_qemu_preserve,
                &installer_qemu_preserve_report,
                PRODUCTION_INSTALLER_MODE_DUAL_BOOT, 1U,
                installer_qemu_disk_identity,
                sizeof(installer_qemu_disk_identity) - 1U,
                &installer_qemu_preserve_source, true) ==
                PRODUCTION_INSTALLER_OK,
        "the occupied-path controller is prepared");
    installer_qemu_hardening_bind(
        &installer_qemu_preserve, &installer_qemu_preserve_report);
    installer_qemu_require(
        production_installer_build_plan(&installer_qemu_preserve) ==
                PRODUCTION_INSTALLER_OK &&
            production_installer_plan_layout(
                &installer_qemu_preserve,
                &installer_qemu_preserve_report,
                &installer_qemu_preserve_plan,
                &installer_qemu_preserve_dual) ==
                PRODUCTION_INSTALLER_OK &&
            production_installer_preserve_reuse(
                &installer_qemu_preserve,
                &installer_qemu_preserve_reuse) ==
                PRODUCTION_INSTALLER_OK,
        "the preserving layout is planned on the shared ESP");

    /* The appliance entry carries the sealed installed module paths. */
    memset(&installer_qemu_preserve_appliance, 0,
           sizeof(installer_qemu_preserve_appliance));
    memset(&installer_qemu_preserve_recovery, 0,
           sizeof(installer_qemu_preserve_recovery));
    installer_qemu_preserve_appliance.label = "HFOC OS";
    installer_qemu_preserve_appliance.path =
        INSTALLER_QEMU_KERNEL_BOOT_PATH;
    installer_qemu_preserve_appliance.module_paths[0U] =
        "boot():/boot/hfoc-boot.man";
    installer_qemu_preserve_appliance.module_paths[1U] =
        "boot():/boot/limine/limine.conf";
    installer_qemu_preserve_appliance.module_count = 2U;
    installer_qemu_preserve_recovery.label = "HFOC OS (recovery)";
    installer_qemu_preserve_recovery.path = NULL;
    installer_qemu_preserve_recovery.module_count = 0U;
    installer_qemu_require(
        production_installer_plan_preserve(
            &installer_qemu_preserve, &installer_qemu_preserve_report,
            installer_qemu_preserve_device, NULL,
            &installer_qemu_preserve_appliance,
            &installer_qemu_preserve_recovery,
            &installer_qemu_preserve_menu,
            &installer_qemu_preserve_inventory) ==
                PRODUCTION_INSTALLER_OK,
        "the menu is planned from the shared ESP");
    /*
     * The occupied fallback loader is not offered as a system: the install
     * takes that path over, so an entry for it would chainload the menu into
     * itself.  Debian's own loader is still offered.
     */
    installer_qemu_require(
        installer_qemu_contains(
            installer_qemu_preserve_menu.text, "/Debian GNU/Linux") &&
            !installer_qemu_contains(
                installer_qemu_preserve_menu.text,
                "boot():/EFI/BOOT/BOOTX64.EFI") &&
            installer_qemu_preserve_menu.entry_count == 2U,
        "the menu offers Debian and not the path this install takes over");
    installer_qemu_require(
        production_installer_publish_preview(&installer_qemu_preserve) ==
                PRODUCTION_INSTALLER_OK &&
            installer_gpt_preserve_read(
                &installer_qemu_block_device, 1U,
                &installer_qemu_preserve_gpt) ==
                INSTALLER_GPT_PRESERVE_OK &&
            production_installer_publish_preserve_record(
                &installer_qemu_preserve,
                &installer_qemu_preserve_plan,
                &installer_qemu_preserve_reuse,
                &installer_qemu_preserve_gpt,
                &installer_qemu_preserve_menu,
                &installer_qemu_preserve_inventory) ==
                PRODUCTION_INSTALLER_OK,
        "the preservation record is published before the first write");
    installer_qemu_require(
        production_installer_confirm(&installer_qemu_preserve) ==
                PRODUCTION_INSTALLER_OK &&
            installer_layout_confirm(&installer_qemu_preserve_plan) ==
                INSTALLER_LAYOUT_OK &&
            production_installer_execute_layout_preserving(
                &installer_qemu_preserve,
                &installer_qemu_block_device,
                &installer_qemu_preserve_plan,
                &installer_qemu_preserve_reuse,
                &installer_qemu_preserve_gpt) ==
                PRODUCTION_INSTALLER_OK,
        "the preserving GPT write succeeds on the shared ESP");

    /* The ESP the boot stage is handed is the reused one, in full. */
    installer_qemu_require(
        production_installer_preserve_esp(
            &installer_qemu_preserve, &installer_qemu_preserve_report,
            &esp_first, &esp_length) == PRODUCTION_INSTALLER_OK &&
            esp_first == INSTALLER_QEMU_FOREIGN_ESP_FIRST &&
            esp_length == INSTALLER_QEMU_FOREIGN_ESP_LENGTH,
        "the shared ESP is found from the plan");
    memset(&boot_esp, 0, sizeof(boot_esp));
    boot_esp.present = true;
    boot_esp.reused = true;
    boot_esp.first_lba = esp_first;
    boot_esp.length_lba = esp_length;
    memset(&boot_media, 0, sizeof(boot_media));
    boot_media.version = PRODUCTION_INSTALLER_BOOT_STAGE_VERSION;
    boot_media.payload = installer_qemu_preserve_source.boot_payload;
    boot_media.payload_size =
        installer_qemu_preserve_source.boot_payload_size;
    boot_media.config = installer_qemu_preserve_source.limine_config;
    boot_media.config_size =
        installer_qemu_preserve_source.limine_config_size;
    boot_media.kernel = installer_qemu_preserve_source.kernel;
    boot_media.kernel_size = installer_qemu_preserve_source.kernel_size;
    boot_media.loader = installer_qemu_installed_loader;
    boot_media.loader_size = sizeof(installer_qemu_installed_loader);
    boot_media.manifest = installer_qemu_installed_manifest;
    boot_media.manifest_size = sizeof(installer_qemu_installed_manifest);
    menu_length = strlen(installer_qemu_preserve_menu.text);
    boot_media.menu =
        (const unsigned char *)installer_qemu_preserve_menu.text;
    boot_media.menu_size = menu_length;
    boot_media.menu_entries = installer_qemu_preserve_menu.entry_count;
    boot_media.menu_timeout_seconds = INSTALLER_BOOT_MENU_TIMEOUT_SECONDS;
    memset(&environment, 0, sizeof(environment));
    installer_qemu_require(
        efi_boot_environment_init(
            &environment, EFI_BOOT_FIRMWARE_UEFI,
            &installer_qemu_block_device, NULL,
            installer_qemu_variable_read,
            installer_qemu_variable_write) == EFI_BOOT_ENVIRONMENT_OK,
        "the occupied-path proof binds the ESP environment");
    memset(&outcome, 0, sizeof(outcome));
    installer_qemu_require(
        production_installer_boot_stage_requirements(
            &installer_qemu_preserve, &boot_esp, &boot_media,
            &outcome) == PRODUCTION_INSTALLER_OK,
        "the shared ESP passes the declared-size gate");
    write_calls_before = installer_qemu_write_calls;
    installer_qemu_require(
        production_installer_boot_stage_run(
            &installer_qemu_preserve, &environment, &boot_esp,
            &boot_media, &outcome) == PRODUCTION_INSTALLER_OK &&
            installer_qemu_write_calls > write_calls_before,
        "the boot stage stages the installed set into the shared ESP");
    installer_qemu_require(
        outcome.loader_path_occupied && outcome.loader_preserved &&
            outcome.loader_preserved_size ==
                INSTALLER_QEMU_ESP_LOADER_LENGTH &&
            outcome.menu_path_occupied && outcome.menu_preserved &&
            outcome.menu_preserved_size ==
                INSTALLER_QEMU_ESP_FOREIGN_MENU_LENGTH &&
            outcome.menu_installed && outcome.payload_staged &&
            outcome.payload_verified &&
            outcome.config_source ==
                PRODUCTION_INSTALLER_BOOT_CONFIG_SOURCE_GENERATED &&
            outcome.menu_entries ==
                installer_qemu_preserve_menu.entry_count &&
            outcome.sealed_length ==
                installer_qemu_preserve_source.kernel_size +
                    installer_qemu_preserve_source.limine_config_size +
                    installer_qemu_preserve_source.boot_payload_size,
        "both occupied paths are preserved and the menu is installed");

    memset(&esp, 0, sizeof(esp));
    installer_qemu_require(
        efi_fat32_esp_bind(
            &environment, esp_first, esp_length, &esp) ==
                EFI_FAT32_ESP_OK,
        "the shared ESP binds without being formatted");

    /* The foreign fallback loader: same size, same bytes, documented name. */
    readback_length = 0U;
    installer_qemu_require(
        efi_fat32_esp_read_file(
            &esp, PRODUCTION_INSTALLER_BOOT_PRESERVED_LOADER_PATH,
            readback, sizeof(readback), &readback_length) ==
                EFI_FAT32_ESP_OK &&
            readback_length == INSTALLER_QEMU_ESP_LOADER_LENGTH &&
            memcmp(readback, installer_qemu_foreign_fallback_before,
                   INSTALLER_QEMU_ESP_LOADER_LENGTH) == 0,
        "the foreign fallback loader is preserved byte for byte");
    /* The foreign configuration: same size, same bytes, documented name. */
    readback_length = 0U;
    installer_qemu_require(
        efi_fat32_esp_read_file(
            &esp, PRODUCTION_INSTALLER_BOOT_PRESERVED_MENU_PATH,
            readback, sizeof(readback), &readback_length) ==
                EFI_FAT32_ESP_OK &&
            readback_length == INSTALLER_QEMU_ESP_FOREIGN_MENU_LENGTH &&
            memcmp(readback, installer_qemu_foreign_menu,
                   INSTALLER_QEMU_ESP_FOREIGN_MENU_LENGTH) == 0,
        "the foreign limine.conf is preserved byte for byte");
    /* And the two paths now carry this install's own files. */
    readback_length = 0U;
    installer_qemu_require(
        efi_fat32_esp_read_file(
            &esp, PRODUCTION_INSTALLER_BOOT_LOADER_PATH, readback,
            sizeof(readback), &readback_length) == EFI_FAT32_ESP_OK &&
            readback_length == sizeof(installer_qemu_installed_loader) &&
            memcmp(readback, installer_qemu_installed_loader,
                   sizeof(installer_qemu_installed_loader)) == 0,
        "the removable-media path carries the installed loader");
    readback_length = 0U;
    installer_qemu_require(
        efi_fat32_esp_read_file(
            &esp, PRODUCTION_INSTALLER_BOOT_MENU_PATH, readback,
            sizeof(readback), &readback_length) == EFI_FAT32_ESP_OK &&
            readback_length == menu_length &&
            memcmp(readback, installer_qemu_preserve_menu.text,
                   menu_length) == 0,
        "the loader's first configuration is the generated menu");
    /* The sealed configuration is staged verbatim beside it. */
    readback_length = 0U;
    installer_qemu_require(
        efi_fat32_esp_read_file(
            &esp, PRODUCTION_INSTALLER_BOOT_CONFIG_PATH, readback,
            sizeof(readback), &readback_length) == EFI_FAT32_ESP_OK &&
            readback_length ==
                installer_qemu_preserve_source.limine_config_size &&
            memcmp(readback,
                   installer_qemu_preserve_source.limine_config,
                   readback_length) == 0,
        "the sealed configuration is staged verbatim");
    installer_qemu_check_foreign_esp_preserved();
    installer_qemu_require(
        memcmp(installer_qemu_sector_find(
                   INSTALLER_QEMU_FOREIGN_ESP_FIRST +
                       INSTALLER_QEMU_ESP_DATA_START +
                       (INSTALLER_QEMU_ESP_BOOT_CLUSTER - 2U))->data,
               installer_qemu_foreign_boot_dir_before,
               INSTALLER_QEMU_ESP_SECTOR) != 0,
        "the EFI BOOT directory now names this install's files");
    serial_write(
        "[PRODUCTION-INSTALLER] occupied path proof OK\n");

    /*
     * The second line of defence for a volume somebody else owns: the declared
     * size gate cannot see how full the volume is, and the free-cluster
     * pre-flight is read-only.  Every data cluster is marked allocated in both
     * FAT copies, and the stage must refuse without a single write.
     */
    memset(fat_fill, 0xFF, sizeof(fat_fill));
    for (index = 0U; index < INSTALLER_QEMU_ESP_SECTORS_PER_FAT; ++index) {
        installer_qemu_sector_store(
            INSTALLER_QEMU_FOREIGN_ESP_FIRST +
                INSTALLER_QEMU_ESP_RESERVED_SECTORS + index, fat_fill);
        installer_qemu_sector_store(
            INSTALLER_QEMU_FOREIGN_ESP_FIRST +
                INSTALLER_QEMU_ESP_RESERVED_SECTORS +
                INSTALLER_QEMU_ESP_SECTORS_PER_FAT + index, fat_fill);
    }
    memset(&outcome, 0, sizeof(outcome));
    write_calls_before = installer_qemu_write_calls;
    installer_qemu_require(
        production_installer_boot_stage_run(
            &installer_qemu_preserve, &environment, &boot_esp,
            &boot_media, &outcome) ==
                PRODUCTION_INSTALLER_ESP_TOO_SMALL &&
            installer_qemu_write_calls == write_calls_before,
        "a full shared ESP is refused with zero writes");
    serial_write(
        "[PRODUCTION-INSTALLER] shared ESP capacity refusal proof OK\n");
}

/*
 * ITEM-398, owner Case B in whole-disk mode: the target disk is entirely ours -
 * the appliance is the only thing on it - and the system that has to stay
 * selectable lives on the machine's other disk.
 *
 * The menu is built from the other disk's ESP only: a whole-disk install erases
 * the target disk, so offering what is on it would offer exactly what this run
 * destroys.  The entry for the other system is named with Limine's
 * `guid(<GPT partition GUID>)` resource, because a firmware drive number is not
 * stable, and the configuration this install writes carries it.  The other disk
 * has its own store and its own write counter, so "it was only read" is
 * asserted rather than assumed.
 */
static void installer_qemu_test_cross_disk_whole_disk_menu(void)
{
    struct installer_discovery_report *report =
        &installer_qemu_preserve_report;
    struct production_installer_boot_media boot_media;
    struct production_installer_boot_outcome outcome;
    struct production_installer_boot_esp boot_esp;
    struct installer_layout_snapshot snapshot;
    struct efi_boot_environment environment;
    struct efi_fat32_esp esp;
    size_t nvme_writes_before = installer_qemu_nvme_write_calls;
    size_t readback_length = 0U;
    uint32_t role_index;

    installer_qemu_reset_disk();
    installer_qemu_media_init();
    installer_qemu_prepare_nvme_disk();
    installer_qemu_preserve_source = installer_qemu_source();

    memset(report, 0, sizeof(*report));
    installer_qemu_require(
        installer_discovery_init(report) == INSTALLER_DISCOVERY_OK &&
            installer_discovery_publish_firmware(
                report, INSTALLER_FIRMWARE_UEFI) ==
                INSTALLER_DISCOVERY_OK &&
            installer_discovery_publish_cpu(report, 4U) ==
                INSTALLER_DISCOVERY_OK &&
            installer_discovery_publish_memory(
                report, 4294967296ULL) == INSTALLER_DISCOVERY_OK &&
            installer_discovery_scan_block_device(
                report, &installer_qemu_block_device, 1U,
                INSTALLER_DISK_CONTROLLER_VIRTIO_BLK,
                installer_qemu_disk_identity,
                sizeof(installer_qemu_disk_identity) - 1U) ==
                INSTALLER_DISCOVERY_OK &&
            installer_discovery_scan_block_device(
                report, &installer_qemu_nvme_device, 2U,
                INSTALLER_DISK_CONTROLLER_NVME, "qemu-debian-disk",
                sizeof("qemu-debian-disk") - 1U) ==
                INSTALLER_DISCOVERY_OK &&
            installer_discovery_finalize(report) ==
                INSTALLER_DISCOVERY_OK,
        "the empty appliance disk and the Debian disk are discovered");
    installer_qemu_require(
        report->disk_count == 2U && report->partition_count == 2U &&
            report->disks[0U].partition_table ==
                INSTALLER_PARTITION_TABLE_NONE &&
            !report->disks[0U].has_efi_system_partition,
        "the target disk carries nothing of anybody else's");

    memset(&installer_qemu_preserve, 0, sizeof(installer_qemu_preserve));
    installer_qemu_require(
        production_installer_init(&installer_qemu_preserve) ==
                PRODUCTION_INSTALLER_OK &&
            production_installer_prepare(
                &installer_qemu_preserve, report,
                PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
                installer_qemu_disk_identity,
                sizeof(installer_qemu_disk_identity) - 1U,
                &installer_qemu_preserve_source, true) ==
                PRODUCTION_INSTALLER_OK,
        "a whole-disk controller targets the appliance's own disk");
    installer_qemu_hardening_bind(&installer_qemu_preserve, report);
    installer_qemu_require(
        production_installer_build_plan(&installer_qemu_preserve) ==
                PRODUCTION_INSTALLER_OK &&
            production_installer_plan_layout(
                &installer_qemu_preserve, report,
                &installer_qemu_preserve_plan, NULL) ==
                PRODUCTION_INSTALLER_OK,
        "the whole-disk layout is planned on the appliance's disk");

    memset(&installer_qemu_preserve_appliance, 0,
           sizeof(installer_qemu_preserve_appliance));
    memset(&installer_qemu_preserve_recovery, 0,
           sizeof(installer_qemu_preserve_recovery));
    installer_qemu_preserve_appliance.label = "HFOC OS";
    installer_qemu_preserve_appliance.path =
        INSTALLER_QEMU_KERNEL_BOOT_PATH;
    installer_qemu_preserve_appliance.module_paths[0U] =
        "boot():/boot/hfoc-boot.man";
    installer_qemu_preserve_appliance.module_paths[1U] =
        "boot():/boot/limine/limine.conf";
    installer_qemu_preserve_appliance.module_count = 2U;
    installer_qemu_preserve_recovery.label = "HFOC OS (recovery)";
    installer_qemu_preserve_recovery.path = NULL;
    installer_qemu_preserve_recovery.module_count = 0U;
    installer_qemu_require(
        production_installer_plan_preserve(
            &installer_qemu_preserve, report,
            installer_qemu_preserve_device, NULL,
            &installer_qemu_preserve_appliance,
            &installer_qemu_preserve_recovery,
            &installer_qemu_preserve_menu,
            &installer_qemu_preserve_inventory) ==
                PRODUCTION_INSTALLER_OK &&
            installer_qemu_preserve_inventory.disk_count == 2U,
        "the menu is planned across both disks in whole-disk mode");
    installer_qemu_require(
        installer_qemu_preserve_menu.loader_count == 1U &&
            installer_qemu_preserve_menu.entry_count == 2U &&
            installer_qemu_contains(
                installer_qemu_preserve_menu.text, "path: guid(") &&
            installer_qemu_contains(
                installer_qemu_preserve_menu.text,
                ":/EFI/DEBIAN/SHIMX64.EFI") &&
            !installer_qemu_contains(
                installer_qemu_preserve_menu.text,
                ":/EFI/DEBIAN/GRUBX64.EFI") &&
            installer_qemu_contains(
                installer_qemu_preserve_menu.text, "/Debian GNU/Linux") &&
            installer_qemu_contains(
                installer_qemu_preserve_menu.text, "timeout: "),
        "the installed configuration offers the other disk's system through "
        "its shim, which is what the firmware boots - not the GRUB behind it");
    installer_qemu_require(
        production_installer_publish_preview(&installer_qemu_preserve) ==
                PRODUCTION_INSTALLER_OK &&
            production_installer_confirm(&installer_qemu_preserve) ==
                PRODUCTION_INSTALLER_OK &&
            installer_layout_confirm(&installer_qemu_preserve_plan) ==
                INSTALLER_LAYOUT_OK,
        "the whole-disk controller confirms the cross-disk menu");
    memset(&snapshot, 0, sizeof(snapshot));
    installer_qemu_require(
        production_installer_execute_layout(
            &installer_qemu_preserve, &installer_qemu_block_device,
            &installer_qemu_preserve_plan, &snapshot) ==
            PRODUCTION_INSTALLER_OK,
        "the whole-disk layout is written to the appliance's disk");

    memset(&boot_esp, 0, sizeof(boot_esp));
    for (role_index = 0U;
         role_index < INSTALLER_LAYOUT_PARTITION_CAPACITY; ++role_index) {
        if (installer_qemu_preserve_plan.partitions[role_index].present &&
            installer_qemu_preserve_plan.partitions[role_index].role ==
                INSTALLER_LAYOUT_ROLE_EFI_SYSTEM) {
            boot_esp.present = true;
            boot_esp.reused = false;
            boot_esp.first_lba =
                installer_qemu_preserve_plan.partitions[role_index].first_lba;
            boot_esp.length_lba =
                installer_qemu_preserve_plan.partitions[role_index].length_lba;
            break;
        }
    }
    memset(&boot_media, 0, sizeof(boot_media));
    boot_media.version = PRODUCTION_INSTALLER_BOOT_STAGE_VERSION;
    boot_media.payload = installer_qemu_preserve_source.boot_payload;
    boot_media.payload_size =
        installer_qemu_preserve_source.boot_payload_size;
    boot_media.config = installer_qemu_preserve_source.limine_config;
    boot_media.config_size =
        installer_qemu_preserve_source.limine_config_size;
    boot_media.kernel = installer_qemu_preserve_source.kernel;
    boot_media.kernel_size = installer_qemu_preserve_source.kernel_size;
    boot_media.loader = installer_qemu_installed_loader;
    boot_media.loader_size = sizeof(installer_qemu_installed_loader);
    boot_media.manifest = installer_qemu_installed_manifest;
    boot_media.manifest_size = sizeof(installer_qemu_installed_manifest);
    boot_media.menu =
        (const unsigned char *)installer_qemu_preserve_menu.text;
    boot_media.menu_size = strlen(installer_qemu_preserve_menu.text);
    boot_media.menu_entries = installer_qemu_preserve_menu.entry_count;
    boot_media.menu_timeout_seconds = INSTALLER_BOOT_MENU_TIMEOUT_SECONDS;
    memset(&environment, 0, sizeof(environment));
    memset(&outcome, 0, sizeof(outcome));
    installer_qemu_require(
        boot_esp.present &&
            efi_boot_environment_init(
                &environment, EFI_BOOT_FIRMWARE_UEFI,
                &installer_qemu_block_device, NULL,
                installer_qemu_variable_read,
                installer_qemu_variable_write) == EFI_BOOT_ENVIRONMENT_OK &&
            production_installer_boot_stage_requirements(
                &installer_qemu_preserve, &boot_esp, &boot_media,
                &outcome) == PRODUCTION_INSTALLER_OK &&
            production_installer_boot_stage_run(
                &installer_qemu_preserve, &environment, &boot_esp,
                &boot_media, &outcome) == PRODUCTION_INSTALLER_OK &&
            outcome.menu_installed && outcome.payload_verified &&
            outcome.config_source ==
                PRODUCTION_INSTALLER_BOOT_CONFIG_SOURCE_GENERATED &&
            outcome.menu_entries ==
                installer_qemu_preserve_menu.entry_count &&
            outcome.menu_timeout_seconds ==
                INSTALLER_BOOT_MENU_TIMEOUT_SECONDS,
        "the installed set and the cross-disk menu are staged, and the "
        "outcome states the entries and the timeout it was staged with");

    memset(&esp, 0, sizeof(esp));
    readback_length = 0U;
    installer_qemu_require(
        efi_fat32_esp_bind(
            &environment, boot_esp.first_lba, boot_esp.length_lba,
            &esp) == EFI_FAT32_ESP_OK &&
            efi_fat32_esp_read_file(
                &esp, PRODUCTION_INSTALLER_BOOT_MENU_PATH,
                installer_qemu_menu_readback,
                sizeof(installer_qemu_menu_readback),
                &readback_length) == EFI_FAT32_ESP_OK &&
            readback_length == boot_media.menu_size &&
            memcmp(installer_qemu_menu_readback,
                   installer_qemu_preserve_menu.text,
                   readback_length) == 0,
        "the installed configuration carries the cross-disk entry");
    installer_qemu_require(
        installer_qemu_nvme_write_calls == nvme_writes_before,
        "the Debian disk was only read, never written");
    serial_write(
        "[PRODUCTION-INSTALLER] cross-disk whole-disk menu proof OK\n");
}

struct installer_qemu_uninstall_context {
    const struct installer_layout_snapshot *snapshot;
};

static enum installer_uninstall_boot_status
installer_qemu_uninstall_remove_entry(void *context, uint32_t entry_index)
{
    static const unsigned char empty[1U] = { 0U };
    struct efi_boot_variable_key key;

    (void)context;
    installer_qemu_boot_key((uint16_t)entry_index, &key);
    return installer_qemu_variable_write(
               &installer_qemu_variables, &key, empty, 0U) ==
               EFI_BOOT_VARIABLE_IO_OK
               ? INSTALLER_UNINSTALL_BOOT_OK
               : INSTALLER_UNINSTALL_BOOT_FAILED;
}

static enum installer_uninstall_boot_status
installer_qemu_uninstall_restore_order(
    void *context,
    const unsigned char *order,
    size_t order_length,
    uint32_t entry_count)
{
    struct efi_boot_variable_key key;

    (void)context;
    (void)entry_count;
    installer_qemu_order_key(&key);
    return installer_qemu_variable_write(
               &installer_qemu_variables, &key, order,
               order_length) == EFI_BOOT_VARIABLE_IO_OK
               ? INSTALLER_UNINSTALL_BOOT_OK
               : INSTALLER_UNINSTALL_BOOT_FAILED;
}

static bool installer_qemu_boot_data_matches(
    uint16_t number,
    const unsigned char *expected,
    size_t length)
{
    unsigned char data[INSTALLER_QEMU_DATA_MAX];
    size_t read_length = 0U;

    return installer_qemu_boot_get(
               number, data, sizeof(data), &read_length) &&
           read_length == length &&
           memcmp(data, expected, length) == 0;
}

static enum installer_uninstall_boot_status
installer_qemu_uninstall_observe(
    void *context,
    struct installer_uninstall_outcome *outcome)
{
    const struct installer_qemu_uninstall_context *uninstall_context =
        (const struct installer_qemu_uninstall_context *)context;
    unsigned char order[INSTALLER_QEMU_ORDER_CAPACITY];
    unsigned char data[INSTALLER_QEMU_DATA_MAX];
    unsigned char expected_linux[64U];
    unsigned char expected_windows[80U];
    struct efi_boot_variable_key key;
    enum efi_boot_variable_io_status io_status;
    size_t length = 0U;
    size_t index;
    bool table_restored = true;

    memset(outcome, 0, sizeof(*outcome));
    outcome->version = INSTALLER_UNINSTALL_VERSION;
    memset(expected_linux, 0x11U, sizeof(expected_linux));
    memset(expected_windows, 0x22U, sizeof(expected_windows));

    installer_qemu_boot_key(0U, &key);
    length = 0U;
    io_status = installer_qemu_variable_read(
        &installer_qemu_variables, &key, data, sizeof(data), &length);
    outcome->hfoc_boot_entry_present =
        io_status == EFI_BOOT_VARIABLE_IO_OK && length != 0U;

    installer_qemu_order_key(&key);
    length = 0U;
    io_status = installer_qemu_variable_read(
        &installer_qemu_variables, &key, order, sizeof(order), &length);
    outcome->previous_boot_order_restored =
        io_status == EFI_BOOT_VARIABLE_IO_OK && length == 4U &&
        order[0U] == 0x01U && order[1U] == 0x00U &&
        order[2U] == 0x02U && order[3U] == 0x00U;
    outcome->previous_os_boot_paths_preserved =
        installer_qemu_boot_data_matches(
            1U, expected_linux, sizeof(expected_linux)) &&
        installer_qemu_boot_data_matches(
            2U, expected_windows, sizeof(expected_windows));

    for (index = 0U;
         index < uninstall_context->snapshot->sector_count;
         ++index) {
        const uint64_t lba =
            uninstall_context->snapshot->sector_lbas[index];
        unsigned char sector[INSTALLER_QEMU_BLOCK_SIZE];

        if (lba >= INSTALLER_QEMU_MEDIA_LBA) {
            table_restored = false;
            continue;
        }
        installer_qemu_sector_read(lba, sector);
        if (memcmp(sector,
                   uninstall_context->snapshot->sectors[index],
                   INSTALLER_QEMU_BLOCK_SIZE) != 0) {
            table_restored = false;
        }
    }
    outcome->partition_table_restored = table_restored;
    outcome->hfoc_partitions_remaining = !table_restored;
    return INSTALLER_UNINSTALL_BOOT_OK;
}

static void installer_qemu_test_uninstall_restore(
    const struct installer_discovery_report *install_report,
    const struct installer_layout_plan *layout_plan,
    const struct installer_layout_snapshot *layout_snapshot)
{
    static struct installer_discovery_report uninstall_report;
    static struct installer_uninstall_evidence uninstall_evidence;
    static struct installer_uninstall_plan uninstall_plan;
    static struct installer_uninstall_preview uninstall_preview;
    static struct installer_uninstall_boot_ops uninstall_ops;
    static struct installer_qemu_uninstall_context uninstall_context;
    unsigned char hfoc_entry[INSTALLER_QEMU_DATA_MAX];
    size_t read_length = 0U;

    uninstall_report = *install_report;
    uninstall_report.os_count = 2U;
    memset(uninstall_report.oss, 0, sizeof(uninstall_report.oss));
    uninstall_report.oss[0U].present = true;
    uninstall_report.oss[0U].id = 1U;
    uninstall_report.oss[0U].kind = INSTALLER_OS_LINUX;
    uninstall_report.oss[0U].has_efi_entry = true;
    uninstall_report.oss[1U].present = true;
    uninstall_report.oss[1U].id = 2U;
    uninstall_report.oss[1U].kind = INSTALLER_OS_WINDOWS;
    uninstall_report.oss[1U].has_efi_entry = true;

    memset(&uninstall_evidence, 0, sizeof(uninstall_evidence));
    uninstall_evidence.version = INSTALLER_UNINSTALL_VERSION;
    uninstall_evidence.firmware_mode = INSTALLER_FIRMWARE_UEFI;
    uninstall_evidence.disk_id = 1U;
    memcpy(uninstall_evidence.identity, installer_qemu_disk_identity,
           sizeof(installer_qemu_disk_identity));
    uninstall_evidence.hfoc_partition_count = layout_plan->partition_count;
    uninstall_evidence.hfoc_boot_entry_count = 1U;
    uninstall_evidence.hfoc_boot_entry_indices[0U] = 0U;
    uninstall_evidence.previous_os_count = 2U;
    uninstall_evidence.previous_boot_entry_count = 2U;
    uninstall_evidence.previous_boot_order_available = true;
    uninstall_evidence.previous_boot_order_length = 4U;
    uninstall_evidence.previous_boot_order[0U] = 0x01U;
    uninstall_evidence.previous_boot_order[1U] = 0x00U;
    uninstall_evidence.previous_boot_order[2U] = 0x02U;
    uninstall_evidence.previous_boot_order[3U] = 0x00U;
    uninstall_evidence.partition_table_snapshot_available = true;
    installer_qemu_require(
        installer_uninstall_snapshot_digest(
            layout_snapshot,
            uninstall_evidence.snapshot_digest) ==
            INSTALLER_UNINSTALL_OK,
        "uninstall snapshot digest fixture");

    memset(&uninstall_plan, 0, sizeof(uninstall_plan));
    memset(&uninstall_preview, 0, sizeof(uninstall_preview));
    installer_qemu_require(
        installer_uninstall_plan_build(
            &uninstall_report, &uninstall_evidence,
            INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS,
            &uninstall_plan) == INSTALLER_UNINSTALL_OK &&
        installer_uninstall_preview(
            &uninstall_plan, &uninstall_preview) ==
            INSTALLER_UNINSTALL_OK &&
        uninstall_preview.destructive &&
        uninstall_preview.previous_os_preserved,
        "uninstall dry-run plan and preview");
    serial_write(
        "[PRODUCTION-INSTALLER] uninstall dry-run preview proof OK\n");

    uninstall_context.snapshot = layout_snapshot;
    memset(&uninstall_ops, 0, sizeof(uninstall_ops));
    uninstall_ops.context = &uninstall_context;
    uninstall_ops.remove_boot_entry =
        installer_qemu_uninstall_remove_entry;
    uninstall_ops.restore_boot_order =
        installer_qemu_uninstall_restore_order;
    uninstall_ops.observe_outcome = installer_qemu_uninstall_observe;

    installer_qemu_require(
        installer_uninstall_confirm(&uninstall_plan) ==
                INSTALLER_UNINSTALL_OK &&
            installer_uninstall_execute(
                &uninstall_plan, &installer_qemu_block_device,
                layout_snapshot, &uninstall_ops) ==
                INSTALLER_UNINSTALL_OK &&
            uninstall_plan.stage == INSTALLER_UNINSTALL_STAGE_COMPLETE,
        "uninstall execution restores the previous OS");
    installer_qemu_require(
        !installer_qemu_boot_get(
            0U, hfoc_entry, sizeof(hfoc_entry), &read_length),
        "HFOC boot entry removed");
    read_length = 0U;
    serial_write(
        "[PRODUCTION-INSTALLER] uninstall previous-OS restoration proof OK\n");
}

static void installer_qemu_test_rollback_failure(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_plan plan;
    struct installer_layout_snapshot snapshot;

    installer_qemu_reset_disk();
    installer_qemu_require(
        installer_qemu_storage_zero(),
        "rollback fixture starts from an empty disk");
    installer_qemu_prepare_controller(&installer, &report);
    installer_qemu_layout_fixture(&report, &plan);
    installer_qemu_require(
        production_installer_confirm(&installer) ==
            PRODUCTION_INSTALLER_OK,
        "rollback fixture confirmation");
    memset(&snapshot, 0, sizeof(snapshot));
    installer_qemu_fail_write_call = 4U;
    installer_qemu_require(
        production_installer_execute_layout(
            &installer, &installer_qemu_block_device,
            &plan, &snapshot) != PRODUCTION_INSTALLER_OK &&
        installer.stage == PRODUCTION_INSTALLER_STAGE_FAILED,
        "injected layout write failure");
    serial_write(
        "[PRODUCTION-INSTALLER] injected failure proof OK\n");
    installer_qemu_require(
        installer_layout_rollback(
            &installer_qemu_block_device, &snapshot) ==
            INSTALLER_LAYOUT_OK &&
        installer_qemu_storage_zero(),
        "rollback restores original disk");
    serial_write(
        "[PRODUCTION-INSTALLER] rollback restoration proof OK\n");
}

void production_installer_qemu_test_init(void)
{
    serial_write("[PRODUCTION-INSTALLER] proof init\n");
    installer_qemu_test_unconfirmed_no_writes();
    installer_qemu_test_unverified_media_no_writes();
    installer_qemu_test_unsupported_hardware_no_writes();
    installer_qemu_test_clean_install();
    installer_qemu_test_dual_boot_preserve();
    installer_qemu_test_boot_stage_occupied_paths();
    installer_qemu_test_cross_disk_whole_disk_menu();
    installer_qemu_test_rollback_failure();
    serial_write("[PRODUCTION-INSTALLER] clean shutdown\n");
    serial_write("[PRODUCTION-INSTALLER] proof OK\n");
}

#else
/* Keep this test-only translation unit non-empty in normal builds. */
int hfoc_production_installer_qemu_translation_unit_marker;
#endif
