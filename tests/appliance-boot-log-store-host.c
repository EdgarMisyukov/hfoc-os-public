/*
 * ITEM-410: the appliance boot log's durable half, proven on the host.
 *
 * This is both a host proof and the fixture builder the reader proof uses.  It
 * builds a *real* disk image - a GPT with an EFI System Partition, a FAT32
 * volume the kernel's own ESP writer binds, and the installed boot manifest
 * that says the ESP is ours - and then runs the store against a file-backed
 * block device exactly as the appliance runs it:
 *
 *   * the log file is created once, preallocated to its declared extent;
 *   * published records are staged in the bounded ring and written at the
 *     moments that matter;
 *   * the second boot reuses the extent, continues the sequence and appends
 *     instead of starting over;
 *   * a flush that overwrites a slot changes no byte of the file allocation
 *     table, no byte of a directory and no byte of another file - the rule that
 *     makes writing into a shared ESP acceptable at all;
 *   * a disk whose ESP is not ours, and a disk with no ESP at all, are probed
 *     and left completely alone;
 *   * an ESP that cannot be written publishes `state=UNAVAILABLE` and never
 *     fails or delays anything, and a later flush still writes what was staged.
 *
 * The image it leaves behind is `build/test-appliance-boot-log-host.img`, and
 * `tests/appliance-boot-log-reader-host.py --require-image` reads the log out of
 * it through `scripts/hfoc-esp-read.py --bootlog`: the writer is proven by the
 * reader, not by itself.
 */
#include "appliance_boot_log_store.h"

#include "appliance_boot_log.h"
#include "appliance_gpt_discovery.h"
#include "arch/x86_64/timer.h"
#include "block_device.h"
#include "efi_boot_environment.h"
#include "efi_fat32_esp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef HFOC_APPLIANCE_BOOT_LOG_STORE_HOST_TEST

#define STORE_BLOCK_SIZE 512U
#define STORE_DISK_BLOCKS 32768U
#define STORE_ESP_FIRST_LBA 2048U
#define STORE_ESP_BLOCKS 16384U
#define STORE_ESP_RESERVED 32U
#define STORE_ESP_FAT_COUNT 2U
#define STORE_ESP_SECTORS_PER_FAT 128U
#define STORE_ESP_ROOT_CLUSTER 2U
#define STORE_ESP_DATA_START \
    (STORE_ESP_RESERVED + STORE_ESP_FAT_COUNT * STORE_ESP_SECTORS_PER_FAT)
#define STORE_ESP_CLUSTERS \
    ((STORE_ESP_BLOCKS - STORE_ESP_DATA_START))
#define STORE_LOG_BYTES ((uint32_t)APPLIANCE_BOOT_LOG_FILE_BYTES)
#define STORE_ESP_GUID_BYTES \
    { 0x28U, 0x73U, 0x2AU, 0xC1U, 0x1FU, 0xF8U, 0xD2U, 0x11U, \
      0xBAU, 0x4BU, 0x00U, 0xA0U, 0xC9U, 0x3EU, 0xC9U, 0x3BU }
#define STORE_MANIFEST_PATH "\\boot\\hfoc-boot.man"
#define STORE_GPT_ENTRIES 4U
#define STORE_IMAGE "build/test-appliance-boot-log-host.img"

struct store_disk {
    int fd;
    uint64_t block_count;
    uint64_t read_calls;
    uint64_t write_calls;
    bool fail_writes;
    char path[512U];
};

static struct store_disk disk_state;
static int failures;
/*
 * ITEM-518: a dump of the staging window is header + 64 records + progress
 * lines + footer, so the capture has to hold more than 64 lines for a proof
 * that reads the end of it.
 */
static char published[192U][APPLIANCE_BOOT_LOG_TEXT_MAX + 64U];
static size_t published_count;
/* ITEM-581: the dump paints the screen; the proof reads that capture. */
static char printed[192U][APPLIANCE_BOOT_LOG_TEXT_MAX + 64U];
static size_t printed_count;
/*
 * ITEM-518: the sink can re-observe what it publishes, which is exactly what
 * the kernel's funnel does - the publish sink reaches the console journal, the
 * journal hands the record back to the log.  `reobserve_cap` bounds the
 * feedback so a store that fails this test fails it instead of hanging, and
 * `reobserve_overflow` is the failure.
 */
static bool sink_reobserves;
static size_t reobserve_cap;
static size_t publish_calls;
static bool reobserve_overflow;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

/* ------------------------------------------------------- the sink the store
 * publishes through: the console's record publication, a text writer and a
 * deterministic clock. */
static void store_sink_publish(const char *record, size_t length)
{
    ++publish_calls;
    if (published_count < sizeof(published) / sizeof(published[0])) {
        size_t copy = length;

        if (copy >= sizeof(published[0])) {
            copy = sizeof(published[0]) - 1U;
        }
        memcpy(published[published_count], record, copy);
        published[published_count][copy] = '\0';
        ++published_count;
    }
    if (sink_reobserves) {
        /*
         * The kernel's funnel hands the log the bounded printable line the
         * console journal accepted, which stops at the newline the dump line
         * ends with; the sink mimics that exactly, or the feedback this proof
         * is about would be refused as BAD_TEXT before it could happen.
         */
        size_t effective = length;
        size_t index;

        for (index = 0U; index < length; ++index) {
            if (record[index] == '\n' || record[index] == '\r') {
                effective = index;
                break;
            }
        }
        if (effective != 0U) {
            if (publish_calls <= reobserve_cap) {
                appliance_boot_log_store_observe(record, effective);
            } else {
                reobserve_overflow = true;
            }
        }
    }
    printf("[record] %.*s\n", (int)length, record);
}

static void store_sink_print(const char *text)
{
    if (printed_count < sizeof(printed) / sizeof(printed[0])) {
        snprintf(printed[printed_count], sizeof(printed[0]), "%s",
                 text != NULL ? text : "");
        ++printed_count;
    }
    printf("[console] %s", text != NULL ? text : "");
}

static uint64_t store_sink_clock;

static uint64_t store_sink_ticks(void)
{
    store_sink_clock += 7U;
    return store_sink_clock;
}

/* ITEM-497: the digest window is ten seconds, so the three-boot proof moves
 * the deterministic clock itself instead of publishing for an hour. */
static void store_sink_advance(uint64_t ticks)
{
    store_sink_clock += ticks;
}

/*
 * ITEM-487: the store's flush reading and the ESP writer's probe both measure
 * with the cycle counter the console measures with, so this proof supplies the
 * same stub `console-host.c` does instead of linking the PIT driver into a host
 * binary.  One cycle is one microsecond and every read advances the counter, so
 * a span the code measured is a span this proof can see: zero means the probe
 * did not run, and nonzero means it did.
 */
static uint64_t store_cycle_value = 1000000U;
static uint64_t store_cycle_step = 1000U;

uint64_t timer_cycles(void)
{
    store_cycle_value += store_cycle_step;
    return store_cycle_value;
}

uint64_t timer_cycles_to_us(uint64_t cycles)
{
    return cycles;
}

static bool printed_contains(const char *needle)
{
    size_t index;

    for (index = 0U; index < printed_count; ++index) {
        if (strstr(printed[index], needle) != NULL) {
            return true;
        }
    }
    return false;
}

static bool published_contains(const char *needle)
{
    size_t index;

    for (index = 0U; index < published_count; ++index) {
        if (strstr(published[index], needle) != NULL) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------ the file device --- */

static enum block_device_status store_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct store_disk *disk = (struct store_disk *)device->context;
    ssize_t result;

    if (disk == NULL || disk->fd < 0 || buffer == NULL ||
        length == 0U || offset % STORE_BLOCK_SIZE != 0U ||
        length % STORE_BLOCK_SIZE != 0U) {
        return BLOCK_DEVICE_UNALIGNED;
    }
    if (offset / STORE_BLOCK_SIZE >= disk->block_count ||
        length / STORE_BLOCK_SIZE >
            disk->block_count - offset / STORE_BLOCK_SIZE) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    ++disk->read_calls;
    result = pread(disk->fd, buffer, length, (off_t)offset);
    if (result != (ssize_t)length) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    return BLOCK_DEVICE_OK;
}

static enum block_device_status store_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct store_disk *disk = (struct store_disk *)device->context;
    ssize_t result;

    if (disk == NULL || disk->fd < 0 || buffer == NULL ||
        length == 0U || offset % STORE_BLOCK_SIZE != 0U ||
        length % STORE_BLOCK_SIZE != 0U) {
        return BLOCK_DEVICE_UNALIGNED;
    }
    if (offset / STORE_BLOCK_SIZE >= disk->block_count ||
        length / STORE_BLOCK_SIZE >
            disk->block_count - offset / STORE_BLOCK_SIZE) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    ++disk->write_calls;
    if (disk->fail_writes) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    result = pwrite(disk->fd, buffer, length, (off_t)offset);
    if (result != (ssize_t)length) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    return BLOCK_DEVICE_OK;
}

static enum block_device_status store_flush(struct block_device *device)
{
    struct store_disk *disk = (struct store_disk *)device->context;

    if (disk == NULL || disk->fd < 0) {
        return BLOCK_DEVICE_BAD_DEVICE;
    }
    if (disk->fail_writes) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    return BLOCK_DEVICE_OK;
}

static struct block_device store_device(void)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = STORE_BLOCK_SIZE;
    device.block_count = disk_state.block_count;
    device.context = &disk_state;
    device.read_fn = store_read;
    device.write_fn = store_write;
    device.flush_fn = store_flush;
    return device;
}

static bool store_disk_open(
    const char *path,
    uint64_t block_count,
    bool create)
{
    struct stat status;

    memset(&disk_state, 0, sizeof(disk_state));
    disk_state.fd = -1;
    disk_state.block_count = block_count;
    if (path == NULL || strlen(path) >= sizeof(disk_state.path)) {
        return false;
    }
    memcpy(disk_state.path, path, strlen(path) + 1U);
    disk_state.fd = create
                        ? open(path, O_RDWR | O_CREAT | O_TRUNC, 0644)
                        : open(path, O_RDWR);
    if (disk_state.fd < 0) {
        fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
        return false;
    }
    if (create) {
        if (ftruncate(disk_state.fd,
                      (off_t)(block_count * STORE_BLOCK_SIZE)) != 0) {
            fprintf(stderr, "cannot size %s: %s\n", path,
                    strerror(errno));
            return false;
        }
    } else if (fstat(disk_state.fd, &status) != 0 ||
               (uint64_t)status.st_size <
                   block_count * STORE_BLOCK_SIZE) {
        return false;
    }
    return true;
}

static void store_disk_close(void)
{
    if (disk_state.fd >= 0) {
        (void)close(disk_state.fd);
        disk_state.fd = -1;
    }
}

static bool store_pread(uint64_t offset, void *buffer, size_t length)
{
    struct block_device device = store_device();

    return store_read(&device, offset, buffer, length) == BLOCK_DEVICE_OK;
}

static bool store_pwrite(uint64_t offset, const void *buffer, size_t length)
{
    struct block_device device = store_device();

    return store_write(&device, offset, buffer, length) ==
           BLOCK_DEVICE_OK;
}

/* ------------------------------------------------------- the GPT and the
 * FAT32 volume the ESP writer binds.  Both are written by this proof, so the
 * image is a disk the kernel's own parser accepts and not an imitation. */

static void store_store_le16(unsigned char *bytes, uint16_t value)
{
    bytes[0] = (unsigned char)(value & 0xFFU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xFFU);
}

static void store_store_le32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value & 0xFFU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xFFU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xFFU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xFFU);
}

static void store_store_le64(unsigned char *bytes, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)((value >> (8U * index)) & 0xFFU);
    }
}

static uint32_t store_crc32(const unsigned char *bytes, size_t length)
{
    uint32_t crc = 0xFFFFFFFFU;
    size_t index;

    for (index = 0U; index < length; ++index) {
        unsigned int bit;

        crc ^= (uint32_t)bytes[index];
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xEDB88320U
                                   : crc >> 1U;
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

static bool store_write_gpt(bool with_esp)
{
    static const unsigned char esp_guid[16U] = STORE_ESP_GUID_BYTES;
    static const unsigned char signature[8U] = {
        'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'
    };
    unsigned char header[STORE_BLOCK_SIZE];
    unsigned char entries[STORE_BLOCK_SIZE];

    memset(header, 0, sizeof(header));
    memcpy(header, signature, sizeof(signature));
    store_store_le32(header + 8U, UINT32_C(0x00010000));
    store_store_le32(header + 12U, 92U);
    store_store_le64(header + 24U, 1U);
    store_store_le64(header + 32U, STORE_DISK_BLOCKS - 1U);
    store_store_le64(header + 40U, 34U);
    store_store_le64(header + 48U, STORE_DISK_BLOCKS - 34U);
    store_store_le64(header + 72U, 2U);
    /*
     * Four entry slots, one used: the entry-array CRC covers exactly the slots
     * the header declares, so the fixture writes the same number it declares.
     */
    store_store_le32(header + 80U, STORE_GPT_ENTRIES);
    store_store_le32(header + 84U, 128U);

    memset(entries, 0, sizeof(entries));
    if (with_esp) {
        unsigned char *entry = entries;

        memcpy(entry, esp_guid, sizeof(esp_guid));
        store_store_le64(entry + 32U, STORE_ESP_FIRST_LBA);
        store_store_le64(
            entry + 40U,
            STORE_ESP_FIRST_LBA + STORE_ESP_BLOCKS - 1U);
    }
    store_store_le32(
        header + 88U,
        store_crc32(entries, (size_t)STORE_GPT_ENTRIES * 128U));
    {
        unsigned char copy[92U];

        memcpy(copy, header, sizeof(copy));
        memset(copy + 16U, 0, 4U);
        store_store_le32(header + 16U, store_crc32(copy, sizeof(copy)));
    }
    return store_pwrite(STORE_BLOCK_SIZE, header, sizeof(header)) &&
           store_pwrite(2U * STORE_BLOCK_SIZE, entries,
                        (size_t)STORE_GPT_ENTRIES * 128U);
}

static bool store_write_volume(void)
{
    unsigned char sector[STORE_BLOCK_SIZE];
    unsigned char fat[STORE_BLOCK_SIZE];
    uint64_t base = (uint64_t)STORE_ESP_FIRST_LBA * STORE_BLOCK_SIZE;
    uint32_t fat_index;

    memset(sector, 0, sizeof(sector));
    store_store_le16(sector + 11U, STORE_BLOCK_SIZE);
    sector[13U] = 1U;
    store_store_le16(sector + 14U, STORE_ESP_RESERVED);
    sector[16U] = STORE_ESP_FAT_COUNT;
    store_store_le32(sector + 32U, STORE_ESP_BLOCKS);
    store_store_le32(sector + 36U, STORE_ESP_SECTORS_PER_FAT);
    store_store_le32(sector + 44U, STORE_ESP_ROOT_CLUSTER);
    sector[510U] = 0x55U;
    sector[511U] = 0xAAU;
    if (!store_pwrite(base, sector, sizeof(sector))) {
        return false;
    }
    memset(fat, 0, sizeof(fat));
    store_store_le32(fat + 0U, 0x0FFFFFF8U);
    store_store_le32(fat + 4U, 0x0FFFFFFFU);
    store_store_le32(fat + 2U * 4U, 0x0FFFFFFFU);
    for (fat_index = 0U; fat_index < STORE_ESP_FAT_COUNT; ++fat_index) {
        const uint64_t lba = base +
            ((uint64_t)STORE_ESP_RESERVED +
             (uint64_t)fat_index * STORE_ESP_SECTORS_PER_FAT) *
                STORE_BLOCK_SIZE;

        if (!store_pwrite(lba, fat, sizeof(fat))) {
            return false;
        }
    }
    return true;
}

static enum efi_boot_variable_io_status store_variable_read(
    void *context,
    const struct efi_boot_variable_key *key,
    unsigned char *data,
    size_t capacity,
    size_t *length)
{
    (void)context;
    (void)key;
    (void)data;
    (void)capacity;
    (void)length;
    return EFI_BOOT_VARIABLE_IO_NOT_FOUND;
}

static enum efi_boot_variable_io_status store_variable_write(
    void *context,
    const struct efi_boot_variable_key *key,
    const unsigned char *data,
    size_t length)
{
    (void)context;
    (void)key;
    (void)data;
    (void)length;
    return EFI_BOOT_VARIABLE_IO_ACCESS_DENIED;
}

/*
 * ITEM-517: a legacy-BIOS boot has no EFI environment to construct, so the
 * store has to bind the FAT boot volume by device and extent.  The Makefile
 * target links this proof with `--wrap=efi_boot_environment_init`, which is
 * what lets one case say "there is no environment here" while every other case
 * in this file keeps the real one: the wrapper delegates unless the case has
 * refused it, and the real function is reached through `__real_`.
 */
static bool store_environment_refused;

extern enum efi_boot_environment_status __real_efi_boot_environment_init(
    struct efi_boot_environment *environment,
    enum efi_boot_firmware_platform platform,
    struct block_device *block_device,
    void *variable_context,
    efi_boot_variable_read_fn variable_read,
    efi_boot_variable_write_fn variable_write);

enum efi_boot_environment_status __wrap_efi_boot_environment_init(
    struct efi_boot_environment *environment,
    enum efi_boot_firmware_platform platform,
    struct block_device *block_device,
    void *variable_context,
    efi_boot_variable_read_fn variable_read,
    efi_boot_variable_write_fn variable_write)
{
    if (store_environment_refused) {
        return EFI_BOOT_ENVIRONMENT_UNAVAILABLE;
    }
    return __real_efi_boot_environment_init(
        environment, platform, block_device, variable_context, variable_read,
        variable_write);
}

/* Stage the installed boot manifest: the marker the store probes for. */
static bool store_stage_manifest(void)
{
    struct block_device device = store_device();
    struct efi_boot_environment environment;
    struct efi_fat32_esp esp;
    static const unsigned char manifest[] = "HFOC-BOOT-MANIFEST-ITEM-410";
    struct efi_fat32_esp_payload payload;

    memset(&environment, 0, sizeof(environment));
    if (efi_boot_environment_init(
            &environment, EFI_BOOT_FIRMWARE_UEFI, &device, NULL,
            store_variable_read, store_variable_write) !=
        EFI_BOOT_ENVIRONMENT_OK) {
        return false;
    }
    memset(&esp, 0, sizeof(esp));
    if (efi_fat32_esp_bind(
            &environment, STORE_ESP_FIRST_LBA, STORE_ESP_BLOCKS, &esp) !=
        EFI_FAT32_ESP_OK) {
        return false;
    }
    payload.path = STORE_MANIFEST_PATH;
    payload.data = manifest;
    payload.size = sizeof(manifest) - 1U;
    return efi_fat32_esp_install_payload_set(&esp, &payload, 1U) ==
           EFI_FAT32_ESP_OK;
}

/* Read the log file back through the ESP module, as any reader would. */
static bool store_read_log(
    unsigned char *buffer,
    size_t capacity,
    size_t *length,
    uint32_t *file_size)
{
    struct block_device device = store_device();
    struct efi_boot_environment environment;
    struct efi_fat32_esp esp;

    memset(&environment, 0, sizeof(environment));
    if (efi_boot_environment_init(
            &environment, EFI_BOOT_FIRMWARE_UEFI, &device, NULL,
            store_variable_read, store_variable_write) !=
        EFI_BOOT_ENVIRONMENT_OK) {
        return false;
    }
    memset(&esp, 0, sizeof(esp));
    if (efi_fat32_esp_bind(
            &environment, STORE_ESP_FIRST_LBA, STORE_ESP_BLOCKS, &esp) !=
        EFI_FAT32_ESP_OK) {
        return false;
    }
    return efi_fat32_esp_read_file_range(
               &esp, APPLIANCE_BOOT_LOG_PATH, 0U, buffer,
               (uint32_t)capacity, length, file_size) == EFI_FAT32_ESP_OK;
}

/*
 * ITEM-517: read the log back the way a legacy-BIOS boot would - bound by
 * device and extent, with no EFI environment anywhere in the path.  This is
 * deliberately not `store_read_log()` above: the fallback has to be proven
 * against the volume the fallback itself bound.
 */
static bool store_read_log_device(
    unsigned char *buffer,
    size_t capacity,
    size_t *length,
    uint32_t *file_size)
{
    struct block_device device = store_device();
    struct efi_fat32_esp esp;

    memset(&esp, 0, sizeof(esp));
    if (efi_fat32_esp_bind_device(
            &device, STORE_ESP_FIRST_LBA, STORE_ESP_BLOCKS, &esp) !=
        EFI_FAT32_ESP_OK) {
        return false;
    }
    return efi_fat32_esp_read_file_range(
               &esp, APPLIANCE_BOOT_LOG_PATH, 0U, buffer,
               (uint32_t)capacity, length, file_size) == EFI_FAT32_ESP_OK;
}

static bool store_file_present(const char *path)
{
    struct block_device device = store_device();
    struct efi_boot_environment environment;
    struct efi_fat32_esp esp;
    bool present = false;
    uint32_t size = 0U;

    memset(&environment, 0, sizeof(environment));
    if (efi_boot_environment_init(
            &environment, EFI_BOOT_FIRMWARE_UEFI, &device, NULL,
            store_variable_read, store_variable_write) !=
        EFI_BOOT_ENVIRONMENT_OK) {
        return false;
    }
    memset(&esp, 0, sizeof(esp));
    if (efi_fat32_esp_bind(
            &environment, STORE_ESP_FIRST_LBA, STORE_ESP_BLOCKS, &esp) !=
        EFI_FAT32_ESP_OK) {
        return false;
    }
    if (efi_fat32_esp_probe_file(&esp, path, &present, &size) !=
        EFI_FAT32_ESP_OK) {
        return false;
    }
    return present && size == STORE_LOG_BYTES;
}

/* The contiguous run of records a reader reconstructs, exactly as
 * `scripts/hfoc-esp-read.py --bootlog` does: newest valid record, then
 * backwards while the slot before it is valid and its sequence is one less. */
struct store_scan {
    size_t valid;
    size_t torn;
    size_t run_slots;
    uint64_t run_oldest;
    uint64_t run_newest;
    uint64_t run_generation;
};

static struct store_scan store_scan_log(const unsigned char *file)
{
    struct store_scan scan;
    struct appliance_boot_log_entry entries[APPLIANCE_BOOT_LOG_SLOT_COUNT];
    enum appliance_boot_log_slot_status status[APPLIANCE_BOOT_LOG_SLOT_COUNT];
    size_t index;
    size_t newest = 0U;
    bool found = false;

    memset(&scan, 0, sizeof(scan));
    memset(entries, 0, sizeof(entries));
    memset(status, 0, sizeof(status));
    for (index = 1U; index < APPLIANCE_BOOT_LOG_SLOT_COUNT; ++index) {
        status[index] = appliance_boot_log_parse_slot(
            file + index * APPLIANCE_BOOT_LOG_SLOT_SIZE, &entries[index]);
        if (status[index] == APPLIANCE_BOOT_LOG_SLOT_OK) {
            ++scan.valid;
        } else if (status[index] == APPLIANCE_BOOT_LOG_SLOT_TORN) {
            ++scan.torn;
        }
    }
    for (index = 1U; index < APPLIANCE_BOOT_LOG_SLOT_COUNT; ++index) {
        if (status[index] != APPLIANCE_BOOT_LOG_SLOT_OK) {
            continue;
        }
        if (!found || entries[index].sequence >= entries[newest].sequence) {
            newest = index;
            found = true;
        }
    }
    if (!found) {
        return scan;
    }
    scan.run_newest = entries[newest].sequence;
    scan.run_oldest = entries[newest].sequence;
    scan.run_generation = entries[newest].generation;
    scan.run_slots = 1U;
    for (;;) {
        const size_t previous = newest == 1U
                                    ? APPLIANCE_BOOT_LOG_SLOT_COUNT - 1U
                                    : newest - 1U;

        if (status[previous] != APPLIANCE_BOOT_LOG_SLOT_OK ||
            entries[previous].sequence + 1U != scan.run_oldest) {
            break;
        }
        newest = previous;
        scan.run_oldest = entries[previous].sequence;
        ++scan.run_slots;
    }
    return scan;
}

static void store_reset_sink(void)
{
    published_count = 0U;
    printed_count = 0U;
    memset(published, 0, sizeof(published));
    memset(printed, 0, sizeof(printed));
}

static struct appliance_boot_log_store_sink store_sink(void)
{
    struct appliance_boot_log_store_sink sink;

    sink.publish = store_sink_publish;
    sink.print = store_sink_print;
    sink.ticks = store_sink_ticks;
    /* ITEM-521: the second consumer is optional; a host sink without one must
       say so instead of leaving the field uninitialised. */
    sink.admitted = NULL;
    return sink;
}

/* ------------------------------------------------------------- the proofs - */

static void test_no_target_is_honest(void)
{
    struct appliance_boot_log_store_sink sink = store_sink();

    store_reset_sink();
    CHECK(appliance_boot_log_store_init(&sink) ==
              APPLIANCE_BOOT_LOG_STORE_OK,
          "the boot log store initialises");
    appliance_boot_log_store_observe("[BOOT] early record", 19U);
    appliance_boot_log_store_flush("readiness");
    CHECK(published_contains("[BOOTLOG] state=UNAVAILABLE reason=NO_TARGET"),
          "a log with no attached target publishes state=UNAVAILABLE "
          "reason=NO_TARGET");
    CHECK(published_count == 1U,
          "the unavailable state is published once, not on every flush");
    appliance_boot_log_store_flush("readiness");
    CHECK(published_count == 1U,
          "a second flush with no target does not repeat the record");
    store_reset_sink();
    appliance_boot_log_store_dump();
    CHECK(printed_contains("BOOTLOG: state=UNAVAILABLE") &&
              printed_contains("#0 [BOOT] early record"),
          "the console dump paints the staged records from RAM and depends on "
          "no device");
    CHECK(published_count == 0U,
          "ITEM-581: the reading is not re-published into the console journal");
}

/*
 * ITEM-518: one `bootlog` is a finite reading, even when the publish sink is
 * the console journal that hands the record straight back to the log.
 *
 * The owner's laptop ran `bootlog` and the command never returned.  The cause
 * is here: the dump published each staged record through the same funnel the
 * boot uses, the funnel observed it, `observe` appended it to the ring, and the
 * loop compared its counter with a `next_sequence` its own output was moving.
 * The proof drives exactly that feedback: the sink re-observes what it
 * publishes, capped so a regression fails instead of hanging.
 */
static void test_the_console_dump_is_bounded_and_does_not_feed_itself(void)
{
    struct appliance_boot_log_store_sink sink = store_sink();
    char record[64U];
    size_t index;

    store_reset_sink();
    publish_calls = 0U;
    reobserve_overflow = false;
    sink_reobserves = true;
    reobserve_cap = 256U;
    CHECK(appliance_boot_log_store_init(&sink) ==
              APPLIANCE_BOOT_LOG_STORE_OK,
          "the store initialises for the bounded-dump proof");
    for (index = 0U; index < 80U; ++index) {
        const int length = snprintf(record, sizeof(record),
                                    "[BOOT] staged %zu", index);

        if (length > 0) {
            appliance_boot_log_store_observe(record, (size_t)length);
        }
    }
    store_reset_sink();
    publish_calls = 0U;
    appliance_boot_log_store_dump();
    /*
     * ITEM-581: the interactive reading paints the screen.  The old dump
     * published every line through the journal funnel, which is serial-only,
     * so `bootlog` answered with nothing on the screen the operator was
     * looking at.
     */
    CHECK(printed_contains("BOOTLOG: state="),
          "ITEM-581: the dump paints its own header on the screen");
    CHECK(printed_contains("records=80 "),
          "ITEM-581: the header names the staged record count");
    CHECK(printed_contains("#79 [BOOT] staged 79"),
          "ITEM-581: the newest staged record is on the screen");
    CHECK(printed_contains("BOOTLOG: shown=64 of=64 done"),
          "ITEM-581: the dump ends on the screen by saying how much it showed");
    CHECK(!reobserve_overflow,
          "ITEM-518: the dump's own output is not fed back into the ring");
    CHECK(published_count == 0U,
          "ITEM-581: the reading is never published back into the journal");
    /*
     * The reading is a reading: the ring still holds the 80 records it held
     * before the dump, not the dump's own echo of them.
     */
    sink_reobserves = false;
    store_reset_sink();
    publish_calls = 0U;
    appliance_boot_log_store_dump();
    CHECK(printed_contains("records=80 "),
          "ITEM-518: the dump leaves the ring exactly as it found it");
    CHECK(printed_contains("BOOTLOG: shown=64 of=64 done"),
          "ITEM-518: the second dump ends the same way");
    CHECK(publish_calls == 0U,
          "ITEM-518: the second dump publishes nothing either");
    CHECK(printed_count >= 65U && printed_count <= 80U,
          "ITEM-518: one dump paints a bounded number of lines");
}

static void test_attach_creates_the_extent(void)
{
    struct block_device device;
    struct appliance_boot_log_store_sink sink = store_sink();
    unsigned char header_slot[APPLIANCE_BOOT_LOG_SLOT_SIZE];
    struct appliance_boot_log_header header;
    size_t length = 0U;
    uint32_t file_size = 0U;
    static unsigned char log[APPLIANCE_BOOT_LOG_FILE_BYTES];

    store_reset_sink();
    CHECK(appliance_boot_log_store_init(&sink) ==
              APPLIANCE_BOOT_LOG_STORE_OK,
          "the store initialises for the attach proof");
    device = store_device();
    CHECK(appliance_boot_log_store_attach_disk(&device, "boot") ==
              APPLIANCE_BOOT_LOG_STORE_OK,
          "the store attaches to the ESP that carries our boot manifest");
    CHECK(appliance_boot_log_store_ready(),
          "the store reports the log is durable after the attach");
    CHECK(published_contains(
              "[BOOTLOG] state=READY path=\\EFI\\HFOC\\BOOTLOG.BIN"),
          "the attach publishes where the log lives");
    CHECK(published_contains("reused=0"),
          "the first attach creates the extent instead of reusing one");
    CHECK(store_file_present(APPLIANCE_BOOT_LOG_PATH),
          "the log file exists at its declared size");
    CHECK(store_read_log(log, sizeof(log), &length, &file_size) &&
              length == STORE_LOG_BYTES && file_size == STORE_LOG_BYTES,
          "the whole extent reads back at its declared length");
    memcpy(header_slot, log, sizeof(header_slot));
    CHECK(appliance_boot_log_parse_header(header_slot, &header) ==
              APPLIANCE_BOOT_LOG_OK && header.next_sequence == 0U &&
              header.generation == 0U && header.written == 0U &&
              header.record_capacity == APPLIANCE_BOOT_LOG_RECORD_CAPACITY,
          "a fresh extent is self-describing: its header parses and declares "
          "an empty log");
}

static void test_flush_writes_records(void)
{
    struct block_device device;
    struct appliance_boot_log_store_sink sink = store_sink();
    static unsigned char log[APPLIANCE_BOOT_LOG_FILE_BYTES];
    struct store_scan scan;
    size_t length = 0U;
    uint32_t file_size = 0U;
    static const char *const records[] = {
        "[PRODUCTION-INSTALL] boot-entry state=AVAILABLE "
        "mechanism=REMOVABLE_MEDIA_PATH",
        "[BOOT-CHAIN] verdict=ATTESTED reason=NONE",
        "[PRODUCTION-BOOT] status=APPLIANCE_PRODUCTION_BOOT_OK",
        "[APPLIANCE-BOOT] runtime=READY identity=OK storage=READY",
        "[PRODUCTION-SERVING] v=1 state=SERVING",
    };
    struct appliance_boot_log_flush_stats flushes_before;
    struct appliance_boot_log_flush_stats stats;
    uint64_t flush_count_before = 0U;
    size_t index;

    store_reset_sink();
    CHECK(appliance_boot_log_store_init(&sink) ==
              APPLIANCE_BOOT_LOG_STORE_OK,
          "the store initialises for the flush proof");
    device = store_device();
    CHECK(appliance_boot_log_store_attach_disk(&device, "boot") ==
              APPLIANCE_BOOT_LOG_STORE_OK,
          "the store attaches to the existing extent");
    CHECK(published_contains("reused=1"),
          "an extent that already has the declared size is reused");
    store_reset_sink();
    if (appliance_boot_log_store_last_flush(&flushes_before)) {
        flush_count_before = flushes_before.count;
    }
    /*
     * hw15: the reading the owner asked for - what a flush costs, and where.
     * Every record below is a flush marker, so each one is flushed on its own
     * and each flush leaves its own accounting: the measured `esp_write_us`
     * and `commit_us` around the existing calls, and `publish_us` as what the
     * flush spent outside both, which is why the record names it
     * `publish=derived` rather than pretending it was measured between two
     * reads a shared loop does not have.
     */
    for (index = 0U; index < sizeof(records) / sizeof(records[0]);
         ++index) {
        const uint64_t length_of_record = (uint64_t)strlen(records[index]);

        appliance_boot_log_store_observe(
            records[index], (size_t)length_of_record);
        CHECK(appliance_boot_log_store_last_flush(&stats) &&
                  stats.count == flush_count_before + index + 1U &&
                  stats.records == 1U &&
                  stats.text_bytes == length_of_record &&
                  stats.slot_bytes ==
                      (uint64_t)APPLIANCE_BOOT_LOG_SLOT_SIZE,
              "a marker record is flushed on its own, and the accounting "
              "names that one record and its bytes");
        CHECK(stats.esp_write_us > 0U && stats.commit_us > 0U &&
                  stats.total_us > 0U &&
                  stats.publish_us + stats.esp_write_us +
                          stats.commit_us == stats.total_us &&
                  stats.total_us >= stats.esp_write_us &&
                  stats.total_us >= stats.commit_us,
              "the flush measures its own extent write and its own commit, "
              "and the derived remainder adds up");
    }
    /*
     * The explicit flush finds nothing pending - the marker already wrote the
     * last record - and still accounts for itself: zero records, zero bytes
     * and zero measured write and commit, which is what makes an "empty"
     * flush distinguishable from a flush that did work.
     */
    appliance_boot_log_store_flush("readiness");
    CHECK(published_contains("[BOOTLOG] state=READY"),
          "a flush that wrote records publishes state=READY");
    CHECK(published_contains("flushed="),
          "the published state names how many records reached the extent");
    CHECK(appliance_boot_log_store_last_flush(&stats) &&
              stats.count == flush_count_before +
                  sizeof(records) / sizeof(records[0]) + 1U &&
              stats.records == 0U && stats.text_bytes == 0U &&
              stats.slot_bytes == 0U &&
              stats.publish_us + stats.esp_write_us +
                      stats.commit_us == stats.total_us,
          "a flush with nothing pending reports no records and still counts");
    CHECK(store_read_log(log, sizeof(log), &length, &file_size),
          "the extent reads back after the flush");
    scan = store_scan_log(log);
    CHECK(scan.run_slots == 5U && scan.torn == 0U,
          "every record the flush wrote is a valid slot");
    CHECK(scan.run_oldest == 0U && scan.run_newest == 4U,
          "the records carry the sequence numbers they were published with");
    CHECK(scan.run_generation == 0U,
          "a log that has not wrapped reports generation zero");
    {
        struct appliance_boot_log_entry entry;
        size_t slot;

        for (slot = 1U; slot <= 5U; ++slot) {
            CHECK(appliance_boot_log_parse_slot(
                      log + slot * APPLIANCE_BOOT_LOG_SLOT_SIZE,
                      &entry) == APPLIANCE_BOOT_LOG_SLOT_OK &&
                      strcmp(entry.text, records[slot - 1U]) == 0 &&
                      entry.length == strlen(records[slot - 1U]),
                  "the record in the slot is the record that was published");
        }
    }
    /*
     * The rule that makes writing into a shared ESP acceptable: after the one
     * creation, a flush overwrites slots inside the extent's own clusters and
     * does not touch the allocation table or a directory.
     */
    {
        static unsigned char fat_before[STORE_ESP_FAT_COUNT *
                                        STORE_ESP_SECTORS_PER_FAT *
                                        STORE_BLOCK_SIZE];
        static unsigned char dir_before[STORE_BLOCK_SIZE];
        static unsigned char fat_after[sizeof(fat_before)];
        static unsigned char dir_after[sizeof(dir_before)];
        const uint64_t fat_offset =
            ((uint64_t)STORE_ESP_FIRST_LBA + STORE_ESP_RESERVED) *
            STORE_BLOCK_SIZE;
        const uint64_t root_dir_offset =
            ((uint64_t)STORE_ESP_FIRST_LBA + STORE_ESP_DATA_START) *
            STORE_BLOCK_SIZE;

        CHECK(store_pread(fat_offset, fat_before, sizeof(fat_before)) &&
                  store_pread(root_dir_offset, dir_before,
                              sizeof(dir_before)),
              "the fixture reads the allocation table and the root directory");
        store_reset_sink();
        appliance_boot_log_store_observe(
            "[PRODUCTION-DNS] state=RESOLVED answer=10.0.2.3", 45U);
        /*
         * `[PRODUCTION-DNS]` is a flush marker too, so the record's own flush
         * is what the accounting beside the second flush describes.
         */
        CHECK(appliance_boot_log_store_last_flush(&stats) &&
                  stats.records == 1U &&
                  stats.text_bytes == 45U &&
                  stats.slot_bytes ==
                      (uint64_t)APPLIANCE_BOOT_LOG_SLOT_SIZE &&
                  stats.publish_us + stats.esp_write_us +
                          stats.commit_us == stats.total_us,
              "the DNS record's own flush accounts for exactly that record");
        flushes_before = stats;
        appliance_boot_log_store_flush("dns");
        CHECK(store_pread(fat_offset, fat_after, sizeof(fat_after)) &&
                  memcmp(fat_before, fat_after, sizeof(fat_before)) == 0,
              "a flush after the creation changes no byte of the file "
              "allocation table");
        CHECK(store_pread(root_dir_offset, dir_after,
                          sizeof(dir_after)) &&
                  memcmp(dir_before, dir_after, sizeof(dir_before)) == 0,
              "a flush after the creation changes no byte of a directory");
        CHECK(store_read_log(log, sizeof(log), &length, &file_size),
              "the extent still reads back after the second flush");
        CHECK(appliance_boot_log_store_last_flush(&stats) &&
                  stats.count == flushes_before.count + 1U &&
                  stats.records == 0U &&
                  stats.publish_us + stats.esp_write_us +
                          stats.commit_us == stats.total_us,
              "the flush after it finds nothing pending and still accounts "
              "for itself");
        scan = store_scan_log(log);
        CHECK(scan.run_newest == 5U && scan.run_slots == 6U,
              "the new record continues the sequence in the next slot");
    }
}

static void test_flush_reading_is_bounded(void)
{
    struct appliance_boot_log_flush_stats stats;
    char record[APPLIANCE_BOOT_LOG_FLUSH_RECORD_BYTES];
    size_t length;

    memset(&stats, 0, sizeof(stats));
    stats.records = 64U;
    stats.text_bytes = 8751U;
    stats.slot_bytes = 32768U;
    stats.publish_us = 39U;
    stats.esp_write_us = 3166905U;
    stats.commit_us = 49471U;
    stats.total_us = 3216408U;
    stats.lookup_us = 1234U;
    stats.fat_traversal_us = 1234567U;
    stats.batch_count = 1U;
    length = appliance_boot_log_store_format_flush_record(
        record, sizeof(record), &stats);
    CHECK(length == strlen("[BOOTLOG-FLUSH] records=64 text_bytes=8751 "
                           "slot_bytes=32768 esp_write_us=3166905 "
                           "commit_us=49471 total_us=3216408") &&
              strcmp(record,
                     "[BOOTLOG-FLUSH] records=64 text_bytes=8751 "
                     "slot_bytes=32768 esp_write_us=3166905 "
                     "commit_us=49471 total_us=3216408") == 0,
          "the flush record names what the flush wrote and how long it took");
    length = appliance_boot_log_store_format_flush_tail_record(
        record, sizeof(record), &stats);
    CHECK(length == strlen("[BOOTLOG-FLUSH-TAIL] publish_us=39 "
                           "publish=derived lookup_us=1234 "
                           "fat_traversal_us=1234567 batch_count=1 "
                           "unavailable=esp_blocks,clusters") &&
              strcmp(record,
                     "[BOOTLOG-FLUSH-TAIL] publish_us=39 "
                     "publish=derived lookup_us=1234 "
                     "fat_traversal_us=1234567 batch_count=1 "
                     "unavailable=esp_blocks,clusters") == 0,
          "the tail names the derived span, the probe's spans and the batches");
    /*
     * The worst case the bound exists for: every decimal field at its widest.
     * A reading that would not fit is refused (a length of zero) rather than
     * truncated - a record that carries half a field would lie about the flush.
     */
    memset(&stats, 0, sizeof(stats));
    stats.records = UINT32_MAX;
    stats.text_bytes = UINT64_MAX;
    stats.slot_bytes = UINT64_MAX;
    stats.publish_us = UINT64_MAX;
    stats.esp_write_us = UINT64_MAX;
    stats.commit_us = UINT64_MAX;
    stats.total_us = UINT64_MAX;
    stats.lookup_us = UINT64_MAX;
    stats.fat_traversal_us = UINT64_MAX;
    stats.batch_count = UINT64_MAX;
    length = appliance_boot_log_store_format_flush_record(
        record, sizeof(record), &stats);
    CHECK(length > 0U && length <= APPLIANCE_BOOT_LOG_FLUSH_RECORD_BYTES,
          "the widest flush record still fits the bound it is published in");
    length = appliance_boot_log_store_format_flush_tail_record(
        record, sizeof(record), &stats);
    CHECK(length > 0U && length <= APPLIANCE_BOOT_LOG_FLUSH_RECORD_BYTES,
          "the widest tail still fits the bound it is published in");
    CHECK(appliance_boot_log_store_format_flush_record(record, 8U, &stats) ==
              0U,
          "a record that cannot fit its buffer is refused, not truncated");
    CHECK(appliance_boot_log_store_format_flush_record(NULL,
                                                        sizeof(record),
                                                        &stats) == 0U &&
              appliance_boot_log_store_format_flush_record(record,
                                                           sizeof(record),
                                                           NULL) == 0U,
          "the formatter refuses what it cannot describe");
}

/*
 * ITEM-487: the reading `hw15` asked for, at every batch size the acceptance
 * names - 1, 2, 29 and 64 records.
 *
 * The records are ordinary ones (no flush marker), so one flush writes exactly
 * the staged set: the reading names what reached the extent, how long the
 * writing and the commit took, and - through the ESP writer's probe - how much
 * of that was the path lookup and the FAT chain walk.  The device counters say
 * the same thing one level down: today every record is its own file-range
 * write, which is what the flush has to stop doing.
 */
static void test_flush_readings_for_batch_sizes(void)
{
    static const uint32_t sizes[] = { 1U, 2U, 29U, 64U };
    struct block_device device;
    struct appliance_boot_log_store_sink sink = store_sink();
    static unsigned char log[APPLIANCE_BOOT_LOG_FILE_BYTES];
    size_t size_index;

    /*
     * The case before this one leaves the store on the install-target fixture
     * and that file open; this case measures the shared boot fixture, which the
     * earlier cases already filled with a manifest and a log, so it reopens it
     * without recreating it and never rewrites a byte of the fixture itself.
     */
    store_disk_close();
    if (!store_disk_open(STORE_IMAGE, STORE_DISK_BLOCKS, false)) {
        CHECK(false, "the shared boot fixture reopens for a batch reading");
        return;
    }
    for (size_index = 0U; size_index < sizeof(sizes) / sizeof(sizes[0]);
         ++size_index) {
        const uint32_t count = sizes[size_index];
        struct appliance_boot_log_flush_stats stats;
        struct appliance_boot_log_entry entry;
        struct store_scan scan;
        char record[APPLIANCE_BOOT_LOG_TEXT_MAX];
        char last[APPLIANCE_BOOT_LOG_TEXT_MAX];
        uint64_t text_bytes = 0U;
        uint64_t writes_before;
        uint64_t reads_before;
        size_t length = 0U;
        uint32_t file_size = 0U;
        uint32_t index;

        store_reset_sink();
        CHECK(appliance_boot_log_store_init(&sink) ==
                  APPLIANCE_BOOT_LOG_STORE_OK,
              "the store initialises for a batch reading");
        device = store_device();
        CHECK(appliance_boot_log_store_attach_disk(&device, "boot") ==
                  APPLIANCE_BOOT_LOG_STORE_OK,
              "the store attaches for a batch reading");
        store_reset_sink();
        for (index = 0U; index < count; ++index) {
            int written = snprintf(record, sizeof(record),
                                   "[TEST-BATCH] size=%u index=%u payload=%s",
                                   (unsigned)count, (unsigned)index,
                                   "0123456789012345678901234567890123456789");

            CHECK(written > 0 && (size_t)written < sizeof(record),
                  "the batch record fits the staging bound");
            appliance_boot_log_store_observe(record, (size_t)written);
            text_bytes += (uint64_t)written;
            memcpy(last, record, (size_t)written + 1U);
        }
        writes_before = disk_state.write_calls;
        reads_before = disk_state.read_calls;
        appliance_boot_log_store_flush("readiness");
        CHECK(appliance_boot_log_store_last_flush(&stats),
              "a flush leaves a reading for the record");
        CHECK(stats.records == count &&
                  stats.slot_bytes ==
                      (uint64_t)count * (uint64_t)APPLIANCE_BOOT_LOG_SLOT_SIZE,
              "the reading counts the records and the slot bytes");
        CHECK(stats.text_bytes == text_bytes,
              "the reading names the text bytes those records carry");
        CHECK(stats.esp_write_us > 0U && stats.commit_us > 0U &&
                  stats.total_us > 0U &&
                  stats.publish_us + stats.esp_write_us +
                          stats.commit_us == stats.total_us,
              "the reading measured the write and the commit");
        CHECK(stats.lookup_us > 0U && stats.fat_traversal_us > 0U,
              "the ESP probe measured the lookup and the FAT walk");
        CHECK(stats.batch_count == 1U,
              "a flush of consecutive slots is one file-range write");
        CHECK(disk_state.write_calls - writes_before ==
                  (uint64_t)count + 1U,
              "the run writes one sector per record and the commit one more");
        /*
         * The point of the batch: the path lookup and the walk of the file's
         * FAT chain are paid once for the whole run, not once per record.  One
         * lookup reads the directories it walks and one walk reads the chain up
         * to the last slot, so a run of any size stays inside a bound the old
         * one-call-per-record flush exceeded by the record count.
         */
        CHECK(disk_state.read_calls - reads_before <= 2048U,
              "the whole run pays one lookup and one chain walk");
        CHECK(disk_state.read_calls > reads_before,
              "the chain walk read the FAT while it reached the slots");
        CHECK(store_read_log(log, sizeof(log), &length, &file_size),
              "the extent reads back after the batch flush");
        scan = store_scan_log(log);
        CHECK(scan.torn == 0U && scan.run_slots >= count,
              "every record the batch flush wrote is a valid slot");
        CHECK(appliance_boot_log_parse_slot(
                  log + (size_t)(1U +
                         (scan.run_newest % APPLIANCE_BOOT_LOG_RECORD_CAPACITY)) *
                            APPLIANCE_BOOT_LOG_SLOT_SIZE,
                  &entry) == APPLIANCE_BOOT_LOG_SLOT_OK &&
                  strcmp(entry.text, last) == 0,
              "the last record of the batch is the last slot written");
    }
}

static void test_second_boot_appends(void)
{
    struct block_device device;
    struct appliance_boot_log_store_sink sink = store_sink();
    static unsigned char log[APPLIANCE_BOOT_LOG_FILE_BYTES];
    struct store_scan scan;
    size_t length = 0U;
    uint32_t file_size = 0U;
    uint32_t writes_before;

    store_reset_sink();
    CHECK(appliance_boot_log_store_init(&sink) ==
              APPLIANCE_BOOT_LOG_STORE_OK,
          "the store re-initialises the way a reboot does");
    device = store_device();
    writes_before = (uint32_t)disk_state.write_calls;
    CHECK(appliance_boot_log_store_attach_disk(&device, "boot") ==
              APPLIANCE_BOOT_LOG_STORE_OK,
          "the second boot attaches to the extent the first one wrote");
    CHECK(published_contains("reused=1"),
          "the second boot reuses the extent instead of creating it");
    store_reset_sink();
    appliance_boot_log_store_observe(
        "[BOOT-CHAIN] verdict=ATTESTED reason=NONE", 36U);
    appliance_boot_log_store_flush("boot-chain");
    CHECK(store_read_log(log, sizeof(log), &length, &file_size),
          "the extent reads back after the second boot");
    scan = store_scan_log(log);
    CHECK(scan.run_newest == 6U && scan.run_slots == 7U,
          "the second boot's record continues the sequence and the slots, so "
          "one log holds the install and the boot that followed it");
    CHECK((uint32_t)disk_state.write_calls > writes_before,
          "the second boot wrote its own record to the extent");
}

static void test_wrap_is_bounded_and_marked(void)
{
    struct block_device device;
    struct appliance_boot_log_store_sink sink = store_sink();
    static unsigned char log[APPLIANCE_BOOT_LOG_FILE_BYTES];
    struct store_scan scan;
    size_t length = 0U;
    uint32_t file_size = 0U;
    char text[96U];
    bool saw_split = false;
    size_t index;

    store_reset_sink();
    CHECK(appliance_boot_log_store_init(&sink) ==
              APPLIANCE_BOOT_LOG_STORE_OK,
          "the store initialises for the wrap proof");
    device = store_device();
    CHECK(appliance_boot_log_store_attach_disk(&device, "boot") ==
              APPLIANCE_BOOT_LOG_STORE_OK,
          "the store attaches before the wrap");
    store_reset_sink();
    /*
     * More records than the extent holds, flushed the way an appliance
     * flushes: the extent wraps, the sequence keeps counting and the
     * generation marks the new pass.
     */
    for (index = 0U;
         index < APPLIANCE_BOOT_LOG_RECORD_CAPACITY +
                     APPLIANCE_BOOT_LOG_PENDING_CAPACITY + 1U;
         ++index) {
        const int written = snprintf(text, sizeof(text),
                                     "[AHCI] state=READY record=%u",
                                     (unsigned)index);

        appliance_boot_log_store_observe(text, (size_t)written);
        if ((index % 16U) == 15U) {
            struct appliance_boot_log_flush_stats stats;

            appliance_boot_log_store_flush("trigger");
            /*
             * ITEM-487: the run of consecutive slots ends where the extent's
             * own ring wraps, so the flush that crosses the wrap is two range
             * writes - and the second one starts at slot one, which is what
             * proves the batch never spans the wrap.
             */
            if (appliance_boot_log_store_last_flush(&stats) &&
                stats.batch_count == 2U) {
                saw_split = true;
            }
        }
    }
    appliance_boot_log_store_flush("trigger");
    CHECK(saw_split,
          "a flush that crosses the extent's ring is two range writes");
    CHECK(store_read_log(log, sizeof(log), &length, &file_size),
          "the extent reads back after the wrap");
    scan = store_scan_log(log);
    CHECK(scan.torn == 0U,
          "a wrapped log is still made of valid slots only");
    CHECK(scan.run_slots == APPLIANCE_BOOT_LOG_RECORD_CAPACITY,
          "the wrapped extent holds exactly one full pass of records");
    CHECK(scan.run_newest ==
              scan.run_oldest + APPLIANCE_BOOT_LOG_RECORD_CAPACITY - 1U,
          "the run the reader reconstructs is contiguous");
    CHECK(scan.run_newest >=
              (uint64_t)(APPLIANCE_BOOT_LOG_RECORD_CAPACITY +
                         APPLIANCE_BOOT_LOG_PENDING_CAPACITY),
          "the newest record is the last one published, not an older one");
    CHECK(scan.run_generation >= 1U,
          "the wrap is marked by a new generation, so a reader can tell old "
          "records from new ones");
    {
        struct appliance_boot_log_entry entry;
        size_t slot;
        bool newest_text_found = false;

        for (slot = 1U; slot < APPLIANCE_BOOT_LOG_SLOT_COUNT; ++slot) {
            if (appliance_boot_log_parse_slot(
                    log + slot * APPLIANCE_BOOT_LOG_SLOT_SIZE,
                    &entry) != APPLIANCE_BOOT_LOG_SLOT_OK) {
                continue;
            }
            if (entry.sequence == scan.run_newest) {
                newest_text_found = strcmp(
                    entry.text, "[AHCI] state=READY record=575") == 0;
            }
        }
        CHECK(newest_text_found,
              "the newest slot carries the text of the last record published");
    }
}

static void test_a_foreign_esp_is_never_written(void)
{
    struct block_device device;
    struct appliance_boot_log_store_sink sink = store_sink();
    uint64_t writes_before;

    /* A disk whose ESP belongs to somebody else: no HFOC boot manifest. */
    store_disk_close();
    if (!store_disk_open("build/test-appliance-boot-log-foreign.img",
                         STORE_DISK_BLOCKS, true)) {
        CHECK(false, "the foreign-ESP fixture is created");
        return;
    }
    CHECK(store_write_gpt(true) && store_write_volume(),
          "the foreign-ESP fixture describes a volume");
    store_reset_sink();
    CHECK(appliance_boot_log_store_init(&sink) ==
              APPLIANCE_BOOT_LOG_STORE_OK,
          "the store initialises for the foreign-ESP proof");
    device = store_device();
    writes_before = disk_state.write_calls;
    CHECK(appliance_boot_log_store_attach_disk(&device, "boot") ==
              APPLIANCE_BOOT_LOG_STORE_NOT_OURS,
          "an ESP without our boot manifest is refused as not ours");
    CHECK(disk_state.write_calls == writes_before,
          "probing an ESP that is not ours writes not one byte to it");
    CHECK(!appliance_boot_log_store_ready(),
          "the store does not claim to be durable on a disk that is not ours");
    CHECK(store_file_present(APPLIANCE_BOOT_LOG_PATH) == false,
          "no boot log file is created on an ESP that is not ours");
    store_disk_close();
}

static void test_a_disk_without_a_gpt_is_left_alone(void)
{
    struct block_device device;
    struct appliance_boot_log_store_sink sink = store_sink();
    uint64_t writes_before;

    store_disk_close();
    if (!store_disk_open("build/test-appliance-boot-log-empty.img",
                         STORE_DISK_BLOCKS, true)) {
        CHECK(false, "the empty-disk fixture is created");
        return;
    }
    store_reset_sink();
    CHECK(appliance_boot_log_store_init(&sink) ==
              APPLIANCE_BOOT_LOG_STORE_OK,
          "the store initialises for the empty-disk proof");
    device = store_device();
    writes_before = disk_state.write_calls;
    CHECK(appliance_boot_log_store_attach_disk(&device, "boot") ==
              APPLIANCE_BOOT_LOG_STORE_NOT_OURS,
          "a disk with no GPT and no ESP is not a log target");
    CHECK(disk_state.write_calls == writes_before,
          "a disk that carries no ESP is never written");
    store_disk_close();
}

static void test_a_broken_esp_publishes_unavailable(void)
{
    struct block_device device;
    struct appliance_boot_log_store_sink sink = store_sink();
    static unsigned char log[APPLIANCE_BOOT_LOG_FILE_BYTES];
    struct store_scan scan;
    size_t length = 0U;
    uint32_t file_size = 0U;

    /* Back to the working image, with the device refusing every write. */
    store_disk_close();
    if (!store_disk_open(STORE_IMAGE, STORE_DISK_BLOCKS, false)) {
        CHECK(false, "the working image is reopened for the failure proof");
        return;
    }
    store_reset_sink();
    CHECK(appliance_boot_log_store_init(&sink) ==
              APPLIANCE_BOOT_LOG_STORE_OK,
          "the store initialises for the write-failure proof");
    device = store_device();
    CHECK(appliance_boot_log_store_attach_disk(&device, "boot") ==
              APPLIANCE_BOOT_LOG_STORE_OK,
          "the store attaches to the working image");
    store_reset_sink();
    disk_state.fail_writes = true;
    appliance_boot_log_store_observe(
        "[PRODUCTION-DHCP] state=BOUND lease=ACQUIRED address=10.0.2.15",
        60U);
    CHECK(!published_contains("[BOOTLOG] state=READY"),
          "a flush whose device refuses the write does not claim to have "
          "written anything");
    CHECK(published_contains("[BOOTLOG] state=UNAVAILABLE"),
          "a failed flush publishes state=UNAVAILABLE");
    CHECK(appliance_boot_log_store_ready(),
          "the attach itself is still the state the store reports");
    disk_state.fail_writes = false;

    /* The record was not lost: the next flush writes it. */
    store_reset_sink();
    appliance_boot_log_store_flush("retry");
    CHECK(published_contains("[BOOTLOG] state=READY"),
          "the next flush writes what the failed one left staged");
    CHECK(store_read_log(log, sizeof(log), &length, &file_size),
          "the extent reads back after the retry");
    scan = store_scan_log(log);
    CHECK(scan.valid > 0U && scan.torn == 0U,
          "the retried flush left a readable log behind");
}

static void test_no_disk_and_no_sink_are_refused(void)
{
    struct appliance_boot_log_store_sink sink = store_sink();

    CHECK(appliance_boot_log_store_init(NULL) ==
              APPLIANCE_BOOT_LOG_STORE_NULL,
          "the store refuses a null sink");
    CHECK(appliance_boot_log_store_init(&sink) ==
              APPLIANCE_BOOT_LOG_STORE_OK,
          "the store accepts a sink with a publisher");
    CHECK(appliance_boot_log_store_attach_disk(NULL, "boot") ==
              APPLIANCE_BOOT_LOG_STORE_NULL,
          "the store refuses a null disk");
    CHECK(appliance_boot_log_store_attach_extent(NULL, 0U, 0U, "install") ==
              APPLIANCE_BOOT_LOG_STORE_NULL,
          "attaching an explicit extent refuses a null disk");
    appliance_boot_log_store_dump();
    CHECK(printed_count > 0U &&
              printed_contains("BOOTLOG: state=UNAVAILABLE"),
          "the console dump answers UNAVAILABLE on the screen with nothing "
          "attached");
}

static void test_attach_extent_takes_the_target_the_install_owns(void)
{
    struct appliance_boot_log_store_sink sink = store_sink();
    struct block_device device;

    /* A fresh image, staged the way the install flow leaves it: the ESP and
     * the boot set exist, and the flow attaches the log to that extent
     * without a manifest probe because it just wrote the manifest itself. */
    store_disk_close();
    if (!store_disk_open("build/test-appliance-boot-log-installed.img",
                         STORE_DISK_BLOCKS, true)) {
        CHECK(false, "the install-target fixture is created");
        return;
    }
    CHECK(store_write_gpt(true) && store_write_volume(),
          "the install-target fixture describes its volume");
    store_reset_sink();
    CHECK(appliance_boot_log_store_init(&sink) ==
              APPLIANCE_BOOT_LOG_STORE_OK,
          "the store initialises for the install-target proof");
    device = store_device();
    CHECK(appliance_boot_log_store_attach_extent(
              &device, STORE_ESP_FIRST_LBA, STORE_ESP_BLOCKS,
              "install") == APPLIANCE_BOOT_LOG_STORE_OK,
          "the install attaches the log to the extent it just staged");
    CHECK(published_contains(
              "[BOOTLOG] state=READY path=\\EFI\\HFOC\\BOOTLOG.BIN"),
          "the install publishes where the log will be readable");
    appliance_boot_log_store_observe(
        "[PRODUCTION-INSTALL] boot-entry state=AVAILABLE mechanism="
        "REMOVABLE_MEDIA_PATH", 74U);
    appliance_boot_log_store_flush("install");
    CHECK(published_contains("flushed="),
          "the install records reach the target extent");
    store_disk_close();
}

/*
 * ITEM-517: the case the BIOS install flow found.
 *
 * `attach_extent()` on a legacy-BIOS boot has no EFI environment to construct,
 * and the volume the install staged is a plain FAT boot volume, not an ESP.
 * The store must fall back to binding that volume by device and extent instead
 * of reporting BAD_ENVIRONMENT, or the install publishes no READY record and
 * the run's log never leaves the RAM ring.  This proof binds a real FAT
 * volume with the environment refused, publishes records, flushes them and
 * reads the whole extent back through the env-free path.
 */
static void test_a_boot_without_an_efi_environment_binds_by_device(void)
{
    struct block_device device;
    struct appliance_boot_log_store_sink sink = store_sink();
    static unsigned char log[APPLIANCE_BOOT_LOG_FILE_BYTES];
    struct store_scan scan;
    static const char record[] =
        "[PRODUCTION-INSTALL] bios payload=VERIFIED mechanism=BIOS_MBR";
    size_t length = 0U;
    uint32_t file_size = 0U;

    store_disk_close();
    if (!store_disk_open("build/test-appliance-boot-log-bios.img",
                         STORE_DISK_BLOCKS, true)) {
        CHECK(false, "the no-EFI-environment fixture is created");
        return;
    }
    CHECK(store_write_gpt(true) && store_write_volume(),
          "the no-EFI-environment fixture describes a FAT volume");
    store_reset_sink();
    CHECK(appliance_boot_log_store_init(&sink) ==
              APPLIANCE_BOOT_LOG_STORE_OK,
          "the store initialises for the no-EFI-environment proof");
    device = store_device();
    store_environment_refused = true;
    CHECK(appliance_boot_log_store_attach_extent(
              &device, STORE_ESP_FIRST_LBA, STORE_ESP_BLOCKS,
              "install") == APPLIANCE_BOOT_LOG_STORE_OK,
          "a boot with no EFI environment binds the FAT volume by device and "
          "extent instead of reporting BAD_ENVIRONMENT");
    store_environment_refused = false;
    CHECK(appliance_boot_log_store_ready(),
          "the device-bound attach reports the log is durable");
    CHECK(published_contains(
              "[BOOTLOG] state=READY path=\\EFI\\HFOC\\BOOTLOG.BIN"),
          "the device-bound attach publishes where the log lives");
    CHECK(published_contains("reused=0"),
          "the device-bound attach creates the extent instead of reusing one");
    store_reset_sink();
    appliance_boot_log_store_observe(record, sizeof(record) - 1U);
    appliance_boot_log_store_flush("install");
    CHECK(published_contains("[BOOTLOG] state=READY"),
          "a flush through the device-bound attach publishes state=READY");
    CHECK(published_contains("flushed="),
          "the install record reaches the FAT volume the attach bound");
    CHECK(store_read_log_device(log, sizeof(log), &length, &file_size) &&
              length == STORE_LOG_BYTES && file_size == STORE_LOG_BYTES,
          "the whole extent the device-bound attach created reads back by "
          "device");
    scan = store_scan_log(log);
    CHECK(scan.run_slots == 1U && scan.torn == 0U &&
              scan.run_newest == 0U,
          "the record the BIOS attach flushed is a valid slot at the head of "
          "the extent");
    {
        struct appliance_boot_log_entry entry;

        CHECK(appliance_boot_log_parse_slot(
                  log + APPLIANCE_BOOT_LOG_SLOT_SIZE, &entry) ==
                  APPLIANCE_BOOT_LOG_SLOT_OK &&
                  strcmp(entry.text, record) == 0,
              "the slot the device-bound attach wrote carries the record that "
              "was published");
    }
    store_disk_close();
}

/*
 * ITEM-517: the other half of the fallback - an extent that is not a volume
 * at all must be refused with the mapped store status, never bound silently.
 */
static void test_an_extent_without_a_volume_is_refused(void)
{
    struct block_device device;
    struct appliance_boot_log_store_sink sink = store_sink();
    uint64_t writes_before;

    store_disk_close();
    if (!store_disk_open("build/test-appliance-boot-log-nonfat.img",
                         STORE_DISK_BLOCKS, true)) {
        CHECK(false, "the non-FAT fixture is created");
        return;
    }
    /* A zeroed disk: the extent carries no boot sector to bind. */
    store_reset_sink();
    CHECK(appliance_boot_log_store_init(&sink) ==
              APPLIANCE_BOOT_LOG_STORE_OK,
          "the store initialises for the non-FAT proof");
    device = store_device();
    writes_before = disk_state.write_calls;
    store_environment_refused = true;
    CHECK(appliance_boot_log_store_attach_extent(
              &device, STORE_ESP_FIRST_LBA, STORE_ESP_BLOCKS,
              "install") == APPLIANCE_BOOT_LOG_STORE_BAD_VOLUME,
          "an extent that is not a FAT volume is refused with the mapped "
          "error, not a silent success");
    store_environment_refused = false;
    CHECK(disk_state.write_calls == writes_before,
          "the refused attach writes not one byte to the extent");
    CHECK(!appliance_boot_log_store_ready(),
          "the refused attach does not claim the log is durable");
    store_disk_close();
}

/*
 * ITEM-487: a run whose write was refused retires none of its records.
 *
 * The flush once retired each record as its own write succeeded; the batch
 * retires the run only after the range write returns, so one refused write
 * leaves the whole run staged for the next flush instead of leaving the ring
 * believing in slots the extent never received.  That is the property the
 * bounded log's honesty rests on, and it is why the batch is not just a faster
 * way to write the same bytes.
 */
static void test_failed_run_retires_nothing(void)
{
    struct block_device device;
    struct appliance_boot_log_store_sink sink = store_sink();
    static unsigned char log[APPLIANCE_BOOT_LOG_FILE_BYTES];
    struct appliance_boot_log_flush_stats stats;
    struct store_scan scan;
    char record[APPLIANCE_BOOT_LOG_TEXT_MAX];
    uint64_t text_bytes = 0U;
    size_t length = 0U;
    uint32_t file_size = 0U;
    uint32_t index;

    store_disk_close();
    if (!store_disk_open(STORE_IMAGE, STORE_DISK_BLOCKS, false)) {
        CHECK(false, "the working image is reopened for the refused-run proof");
        return;
    }
    store_reset_sink();
    CHECK(appliance_boot_log_store_init(&sink) ==
              APPLIANCE_BOOT_LOG_STORE_OK,
          "the store initialises for the refused-run proof");
    device = store_device();
    CHECK(appliance_boot_log_store_attach_disk(&device, "boot") ==
              APPLIANCE_BOOT_LOG_STORE_OK,
          "the store attaches for the refused-run proof");
    store_reset_sink();
    for (index = 0U; index < 6U; ++index) {
        const int written = snprintf(record, sizeof(record),
                                     "[TEST-REFUSED] index=%u payload=%s",
                                     (unsigned)index,
                                     "0123456789012345678901234567890123456789");

        CHECK(written > 0 && (size_t)written < sizeof(record),
              "the refused-run record fits the staging bound");
        appliance_boot_log_store_observe(record, (size_t)written);
        text_bytes += (uint64_t)written;
    }
    disk_state.fail_writes = true;
    appliance_boot_log_store_flush("readiness");
    CHECK(appliance_boot_log_store_last_flush(&stats) &&
              stats.records == 0U && stats.slot_bytes == 0U &&
              stats.batch_count == 1U,
          "a run whose write was refused retires none of its records");
    CHECK(published_contains("[BOOTLOG] state=UNAVAILABLE"),
          "the refused run publishes the failure instead of a state=READY");
    disk_state.fail_writes = false;
    store_reset_sink();
    appliance_boot_log_store_flush("retry");
    CHECK(appliance_boot_log_store_last_flush(&stats) &&
              stats.records == 6U && stats.text_bytes == text_bytes &&
              stats.batch_count == 1U,
          "the next flush writes the whole run in one range write");
    CHECK(store_read_log(log, sizeof(log), &length, &file_size),
          "the extent reads back after the retried run");
    scan = store_scan_log(log);
    CHECK(scan.torn == 0U && scan.run_slots >= 6U,
          "the retried run left six valid slots and nothing torn");
}

/*
 * The records a boot publishes before the log is attached are the ones that
 * explain how the boot went: they are staged in RAM while there is no target,
 * and the attach has to carry them onto the extent's own sequence instead of
 * discarding them.  This is the case the product-flow QEMU proof found: the
 * install records reached the serial stream, the log was attached afterwards,
 * and the staged records were lost in the reset.
 */
static void test_staged_records_survive_the_attach(void)
{
    struct block_device device;
    struct appliance_boot_log_store_sink sink = store_sink();
    static unsigned char log[APPLIANCE_BOOT_LOG_FILE_BYTES];
    struct store_scan scan;
    struct appliance_boot_log_entry entry;
    size_t length = 0U;
    uint32_t file_size = 0U;
    uint64_t newest_before = 0U;
    size_t slots_before = 0U;
    size_t index;
    static const char *const records[] = {
        "[PRODUCTION-INSTALL] layout mode=whole-disk reason=no-existing-system",
        "[PRODUCTION-INSTALL] boot-entry state=AVAILABLE "
        "mechanism=REMOVABLE_MEDIA_PATH",
        "[PRODUCTION-INSTALL] esp payload=VERIFIED first_lba=2048",
    };

    store_reset_sink();
    CHECK(appliance_boot_log_store_init(&sink) ==
              APPLIANCE_BOOT_LOG_STORE_OK,
          "the store initialises for the staged-records proof");
    /* Before the attach: no target, so the records stay staged. */
    for (index = 0U; index < sizeof(records) / sizeof(records[0]);
         ++index) {
        appliance_boot_log_store_observe(
            records[index], strlen(records[index]));
    }
    appliance_boot_log_store_flush("readiness");
    CHECK(published_contains("[BOOTLOG] state=UNAVAILABLE reason=NO_TARGET"),
          "the unstaged boot says so while there is no target");
    CHECK(store_read_log(log, sizeof(log), &length, &file_size),
          "the extent reads back before the attach");
    scan = store_scan_log(log);
    newest_before = scan.run_slots > 0U ? scan.run_newest + 1U : 0U;
    slots_before = scan.run_slots;
    CHECK(scan.torn == 0U,
          "the extent is readable before the attach");

    device = store_device();
    CHECK(appliance_boot_log_store_attach_disk(&device, "boot") ==
              APPLIANCE_BOOT_LOG_STORE_OK,
          "the log attaches after the boot published its records");
    CHECK(published_contains("reused=1"),
          "the attach reused the extent the earlier boot left");
    /* The install flow flushes right after the attach; a sink that is not the
     * console does not trigger anything by itself. */
    appliance_boot_log_store_flush("install");
    CHECK(store_read_log(log, sizeof(log), &length, &file_size),
          "the extent reads back after the attach");
    scan = store_scan_log(log);
    CHECK(scan.run_slots == slots_before + 3U ||
              scan.run_slots == APPLIANCE_BOOT_LOG_RECORD_CAPACITY,
          "the extent still carries one contiguous run of records");
    CHECK(scan.run_newest == newest_before + 2U,
          "the records staged before the attach continue the extent's "
          "sequence instead of restarting it or being dropped");
    for (index = 0U; index < sizeof(records) / sizeof(records[0]);
         ++index) {
        const uint64_t sequence = newest_before + index;
        const size_t slot = (size_t)(1U +
            (sequence % APPLIANCE_BOOT_LOG_RECORD_CAPACITY));

        CHECK(appliance_boot_log_parse_slot(
                  log + slot * APPLIANCE_BOOT_LOG_SLOT_SIZE,
                  &entry) == APPLIANCE_BOOT_LOG_SLOT_OK &&
                  entry.sequence == sequence &&
                  strcmp(entry.text, records[index]) == 0,
              "the staged record is in the extent, in order, after the "
              "records the extent already held");
    }
}

/*
 * ------------------------------------------------------------ ITEM-497 -----
 *
 * Three boots in a row, on one extent.  The owner's machine came up once
 * without input and once with it and nothing could be concluded, because the
 * boot that failed had been overwritten by the boot that followed.  This is the
 * acceptance the item exists for: after three boots every boot's verdict is
 * still readable from the log, including the first one's.
 */

static void store_verdict_boot_begin(
    struct block_device *device,
    const struct appliance_boot_log_store_sink *sink)
{
    store_reset_sink();
    CHECK(appliance_boot_log_store_init(sink) ==
              APPLIANCE_BOOT_LOG_STORE_OK,
          "ITEM-497: the store re-initialises the way a reboot does");
    *device = store_device();
    CHECK(appliance_boot_log_store_attach_disk(device, "boot") ==
              APPLIANCE_BOOT_LOG_STORE_OK,
          "ITEM-497: the boot attaches to the extent the earlier boot wrote");
}

static void store_verdict_boot_publish(
    const char *keyboard,
    const char *mouse)
{
    size_t beat;

    appliance_boot_log_store_input_state(
        keyboard, mouse, 900U, 1200U, 2U, 410U);
    /* The loop heartbeat is what re-states the verdict set, so each boot has
     * to live through a few of them before the next one starts. */
    for (beat = 0U; beat < 4U; ++beat) {
        static const char heartbeat[] =
            "[PRODUCTION-LOOP] state=RUNNING second=5";

        store_sink_advance(1000U);
        appliance_boot_log_store_observe(heartbeat, sizeof(heartbeat) - 1U);
    }
}

static void test_input_verdict_survives_three_boots(void)
{
    struct block_device device;
    struct appliance_boot_log_store_sink sink = store_sink();
    static unsigned char log[APPLIANCE_BOOT_LOG_FILE_BYTES];
    struct appliance_boot_log_input_record records[3];
    size_t length = 0U;
    uint32_t file_size = 0U;
    uint64_t newest = 0U;
    size_t found;

    /* Its own image: the three boots are one machine's history, and the
     * cases before this one left the shared fixture in their own state. */
    store_disk_close();
    if (!store_disk_open("build/test-appliance-boot-log-verdict.img",
                         STORE_DISK_BLOCKS, true)) {
        CHECK(false, "the three-boot fixture is created");
        return;
    }
    CHECK(store_write_gpt(true) && store_write_volume() &&
              store_stage_manifest(),
          "the three-boot fixture describes its volume and stages ours");

    store_verdict_boot_begin(&device, &sink);
    store_verdict_boot_publish("ABSENT", "ABSENT");
    store_verdict_boot_begin(&device, &sink);
    store_verdict_boot_publish("NOT_ARMED", "ARMED");
    store_verdict_boot_begin(&device, &sink);
    store_verdict_boot_publish("ARMED", "ARMED");

    CHECK(store_read_log(log, sizeof(log), &length, &file_size),
          "ITEM-497: the extent reads back after the third boot");
    found = appliance_boot_log_scan_input_verdicts(
        log, APPLIANCE_BOOT_LOG_SLOT_COUNT, records, 3U, &newest);
    CHECK(found == 3U,
          "ITEM-497: three boots leave three verdicts in one log");
    CHECK(newest == 3U,
          "ITEM-497: the third boot numbers itself after the two it read");
    CHECK(found == 3U && records[0].boot == 3U && records[1].boot == 2U &&
              records[2].boot == 1U,
          "ITEM-497: every boot's verdict is readable, newest first");
    CHECK(found == 3U &&
              strstr(records[2].text, "boot=1 verdict=NO_DEVICE") != NULL &&
              strstr(records[1].text, "boot=2 verdict=NOT_ARMED") != NULL &&
              strstr(records[0].text, "boot=3 verdict=ARMED") != NULL,
          "ITEM-497: the first boot is the one that failed, and it still "
          "reads back with its own verdict");
    store_disk_close();
}

/*
 * ITEM-521 (F-A of item-521-wiring-audit-2026-10-12.md): the install journal
 * on the medium is a second consumer of this store, and it must see exactly
 * what the store admitted - past the dump guard, the repeated-record dedup and
 * the periodic gate - not the console funnel's repaints.  A journal wired to
 * the funnel itself fills its bounded ring with heartbeats and pushes the
 * machine announcement and the observed storage out, which is what requirement
 * 1 names.  This case is the admission policy's witness: the journal side
 * counts every record it would have staged.
 */
static size_t admitted_count;
static char admitted_last[APPLIANCE_BOOT_LOG_TEXT_MAX + 1U];

static void store_sink_admitted(const char *record, size_t length)
{
    ++admitted_count;
    if (length > APPLIANCE_BOOT_LOG_TEXT_MAX) {
        length = APPLIANCE_BOOT_LOG_TEXT_MAX;
    }
    memcpy(admitted_last, record, length);
    admitted_last[length] = '\0';
}

static void test_the_second_consumer_sees_only_admitted_records(void)
{
    struct appliance_boot_log_store_sink sink = store_sink();
    static const char machine[] =
        "[BOOT] machine cpus=1 memory=512M firmware=UEFI";
    static const char diag[] =
        "[INPUT-DIAG] keyboard reports=0 poll=idle";
    static const char report[] =
        "[STORAGE-REPORT] found=2 accepted=2 refused=0";
    static const char report2[] =
        "[STORAGE-REPORT] found=2 accepted=2 refused=1";
    static const char report3[] =
        "[STORAGE-REPORT] found=3 accepted=3 refused=1";

    sink.admitted = store_sink_admitted;
    store_reset_sink();
    admitted_count = 0U;
    admitted_last[0] = '\0';
    CHECK(appliance_boot_log_store_init(&sink) ==
              APPLIANCE_BOOT_LOG_STORE_OK,
          "ITEM-521: the store initialises with a second consumer");
    appliance_boot_log_store_observe(machine, sizeof(machine) - 1U);
    CHECK(admitted_count == 1U,
          "ITEM-521: an admitted record reaches the second consumer once");
    appliance_boot_log_store_observe(machine, sizeof(machine) - 1U);
    CHECK(admitted_count == 1U,
          "ITEM-521: an unchanged repeat is not handed to the journal");
    /*
     * The periodic family: a record that differs from the last one the family
     * published is a state change and takes a slot at once; the interval only
     * governs repeats of that same record, and it starts on the first repeat.
     */
    appliance_boot_log_store_observe(diag, sizeof(diag) - 1U);
    CHECK(admitted_count == 2U,
          "ITEM-521: a changed periodic record is admitted once");
    appliance_boot_log_store_observe(report, sizeof(report) - 1U);
    CHECK(admitted_count == 3U, "ITEM-521: a normal record is admitted");
    appliance_boot_log_store_observe(diag, sizeof(diag) - 1U);
    CHECK(admitted_count == 4U,
          "ITEM-521: the first repeat of the family starts its interval and "
          "is admitted");
    appliance_boot_log_store_observe(report2, sizeof(report2) - 1U);
    CHECK(admitted_count == 5U,
          "ITEM-521: the different normal record is admitted");
    appliance_boot_log_store_observe(diag, sizeof(diag) - 1U);
    CHECK(admitted_count == 5U,
          "ITEM-521: the same periodic record inside its interval is held "
          "back");
    appliance_boot_log_store_observe(report3, sizeof(report3) - 1U);
    CHECK(admitted_count == 6U,
          "ITEM-521: a normal record after the held one is admitted");
    /* Past the interval (10 s at HFOC_TIMER_HZ = 100) the family restates. */
    store_sink_advance(2000U);
    appliance_boot_log_store_observe(diag, sizeof(diag) - 1U);
    CHECK(admitted_count == 7U,
          "ITEM-521: past its interval the periodic record is admitted again");
    CHECK(strcmp(admitted_last, diag) == 0,
          "ITEM-521: the second consumer receives the record text itself");
}

int main(void)
{
    if (!store_disk_open(STORE_IMAGE, STORE_DISK_BLOCKS, true)) {        fprintf(stderr, "cannot create %s\n", STORE_IMAGE);
        return 1;
    }
    if (!store_write_gpt(true)) {
        fprintf(stderr, "fixture: the GPT write failed\n");
        store_disk_close();
        return 1;
    }
    if (!store_write_volume()) {
        fprintf(stderr, "fixture: the volume write failed\n");
        store_disk_close();
        return 1;
    }
    if (!store_stage_manifest()) {
        fprintf(stderr, "fixture: staging the manifest failed\n");
        store_disk_close();
        return 1;
    }

    test_no_target_is_honest();
    test_the_console_dump_is_bounded_and_does_not_feed_itself();
    test_attach_creates_the_extent();
    test_flush_writes_records();
    test_flush_reading_is_bounded();
    test_second_boot_appends();
    test_wrap_is_bounded_and_marked();
    test_a_foreign_esp_is_never_written();
    test_a_disk_without_a_gpt_is_left_alone();
    test_a_broken_esp_publishes_unavailable();
    test_failed_run_retires_nothing();
    test_staged_records_survive_the_attach();
    test_no_disk_and_no_sink_are_refused();
    test_attach_extent_takes_the_target_the_install_owns();
    test_a_boot_without_an_efi_environment_binds_by_device();
    test_an_extent_without_a_volume_is_refused();
    test_input_verdict_survives_three_boots();
    test_the_second_consumer_sees_only_admitted_records();
    /*
     * ITEM-487 last: this case writes its own records into the shared
     * fixture image on purpose (1, 2, 29 and 64 of them), so it runs after
     * every case that reads the image the earlier cases left, and the
     * reader proof that follows reads a strictly larger log.
     */
    test_flush_readings_for_batch_sizes();

    store_disk_close();
    if (failures != 0) {
        fprintf(stderr, "Appliance boot log store host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Appliance boot log store host tests: ALL OK");
    puts("boot-log fixture image: " STORE_IMAGE);
    return 0;
}

#endif
