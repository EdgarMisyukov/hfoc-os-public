#ifndef HFOC_EFI_FAT32_ESP_H
#define HFOC_EFI_FAT32_ESP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block_device.h"
#include "efi_boot_environment.h"
#include "sha256.h"

#define EFI_FAT32_ESP_VERSION 1U
#define EFI_FAT32_ESP_SECTOR_SIZE 512U
#define EFI_FAT32_ESP_DIRECTORY_ENTRY_SIZE 32U
#define EFI_FAT32_ESP_PAYLOAD_MAX 524288U
#define EFI_FAT32_ESP_MAX_PAYLOAD_CLUSTERS 1024U
#define EFI_FAT32_ESP_CANONICAL_PATH "\\EFI\\HFOC\\BOOTX64.EFI"
/*
 * ITEM-396: the Limine configuration the installed appliance boots from, the
 * highest path length this module reports, and the bound on the loader
 * inventory it will fill.
 */
#define EFI_FAT32_ESP_CONFIG_PATH "\\EFI\\HFOC\\LIMINE.CONF"
#define EFI_FAT32_ESP_CONFIG_MAX 8192U
#define EFI_FAT32_ESP_VENDOR_LENGTH 12U
#define EFI_FAT32_ESP_LOADER_PATH_LENGTH 80U
#define EFI_FAT32_ESP_LOADER_CAPACITY 8U

/*
 * ITEM-397 installed-system boot payload set.
 *
 * Up to this header the ESP writer staged one bounded payload at one canonical
 * path (ITEM-350).  A disk that has to boot an installed system needs a set:
 * the loader the firmware finds without a boot entry, the boot configuration,
 * the sealed boot manifest and the kernel itself, each at the path its own
 * configuration references.  The set writer keeps every bound explicit:
 *
 *   * EFI_FAT32_ESP_PAYLOAD_SET_MAX entries per call;
 *   * EFI_FAT32_ESP_FILE_MAX bytes per file (one contracted image slot);
 *   * EFI_FAT32_ESP_PAYLOAD_SET_BYTES_MAX bytes for the whole set;
 *   * EFI_FAT32_ESP_PATH_MAX bytes for a canonical path, with a derived 8.3
 *     alias per element; two set entries that would resolve to the same
 *     on-disk name are refused instead of overwriting each other;
 *   * EFI_FAT32_ESP_FAT_SECTORS_MAX bounds the FAT the production formatter
 *     writes for a volume, and EFI_FAT32_ESP_FAT_SCAN_SECTORS_MAX bounds the
 *     free-cluster scan the capacity precheck performs.
 *
 * `efi_fat32_esp_payload_requirement()` is pure arithmetic over the declared
 * production geometry (32 reserved sectors, two FAT copies, one sector per
 * cluster).  The installer compares that requirement with the ESP the layout
 * declares *before* the first write, so an ESP that cannot hold the set is
 * refused instead of being filled with a partial payload that cannot boot.
 *
 * Long file names are written as VFAT LFN entries followed by the derived 8.3
 * entry, because the installed paths (`\boot\hfoc-kernel.elf`,
 * `\boot\limine\limine.conf`, `\boot\hfoc-boot.man`) do not fit the 8.3 form
 * and Limine matches a name that does not fit 8.3 case sensitively.
 */
#define EFI_FAT32_ESP_PAYLOAD_SET_VERSION 1U
#define EFI_FAT32_ESP_PAYLOAD_SET_MAX 7U
#define EFI_FAT32_ESP_PATH_MAX 64U
#define EFI_FAT32_ESP_ELEMENT_MAX 64U
/*
 * MOUSE-DIAG fix (2026-09-27): 8 MiB per file and 10 MiB per set were sized
 * when the production kernel image measured about 6.8 MiB of ELF.  It now
 * measures about 8.4 MiB (8,375,472 bytes at 98ee4ea0, 8,395,368 at 9cd7ba6e -
 * most of the growth is DWARF, which the ESP carries because the staged kernel
 * is the exact sealed artifact), so the 8 MiB per-file ceiling had 13 KiB of
 * margin and the next build crossed it: the installer then refused every
 * install with PRODUCTION_INSTALLER_SOURCE_UNAVAILABLE and the machine booted
 * the medium without installing anything.
 *
 * ITEM-627 repeated that failure at the next ceiling: the installed kernel was
 * 12,587,096 bytes, 4,184 bytes over the 12 MiB per-file bound, and the
 * production installer again refused with the same status.  The per-file bound
 * now equals the 16 MiB set bound; the 33 MiB ESP role
 * (INSTALLER_LAYOUT_CONTRACT_EFI_SYSTEM_LENGTH_LBA = 67584 sectors) is
 * FAT32-valid at that size and still leaves about half the role for the FAT,
 * the directory entries and growth.
 */
#define EFI_FAT32_ESP_FILE_MAX 16777216U
#define EFI_FAT32_ESP_PAYLOAD_SET_BYTES_MAX 16777216U
#define EFI_FAT32_ESP_LFN_ENTRIES_MAX 8U
/*
 * The FAT32 volume rule is what bounds the ESP the writer can format: a
 * volume is FAT32 only when it carries at least 65525 clusters, so at one
 * sector per cluster it needs at least 65525 sectors of data plus the FAT
 * region.  Those sectors are 128 FAT entries each, so a spec-valid FAT32 ESP
 * needs at least 512 sectors per FAT copy; the declared bounds leave room for
 * the 33 MiB role the installer declares.
 */
#define EFI_FAT32_ESP_FAT_SECTORS_MAX 1024U
#define EFI_FAT32_ESP_FAT_SCAN_SECTORS_MAX 1024U
#define EFI_FAT32_ESP_MIN_FAT32_CLUSTERS 65525U
#define EFI_FAT32_ESP_RESERVED_SECTORS 32U
#define EFI_FAT32_ESP_FAT_COPIES 2U
#define EFI_FAT32_ESP_SET_SHORT_NAME_LENGTH 11U
#define EFI_FAT32_ESP_FAT_ENTRIES_PER_SECTOR 128U
#define EFI_FAT32_ESP_FAT_ENTRY_BYTES 4U

enum efi_fat32_esp_status {
    EFI_FAT32_ESP_OK = 0,
    EFI_FAT32_ESP_NULL = 1,
    EFI_FAT32_ESP_BAD_STATE = 2,
    EFI_FAT32_ESP_BAD_ENVIRONMENT = 3,
    EFI_FAT32_ESP_BAD_DEVICE = 4,
    EFI_FAT32_ESP_UNSUPPORTED_GEOMETRY = 5,
    EFI_FAT32_ESP_OUT_OF_RANGE = 6,
    EFI_FAT32_ESP_CORRUPT = 7,
    EFI_FAT32_ESP_PATH_DENIED = 8,
    EFI_FAT32_ESP_NOT_FOUND = 9,
    EFI_FAT32_ESP_CAPACITY = 10,
    EFI_FAT32_ESP_IO_ERROR = 11,
    EFI_FAT32_ESP_FLUSH_ERROR = 12,
    EFI_FAT32_ESP_PATH_CONFLICT = 13,
    EFI_FAT32_ESP_SET_TOO_LARGE = 14,
};

struct efi_fat32_esp_payload {
    const char *path;
    const unsigned char *data;
    size_t size;
};

struct efi_fat32_esp_requirement {
    uint32_t version;
    size_t payload_count;
    uint64_t payload_bytes;
    uint64_t payload_clusters;
    uint64_t directory_clusters;
    uint64_t required_clusters;
    uint64_t required_sectors;
};

/*
 * One boot loader found on an ESP: the vendor directory it lives in and the FAT
 * path to it.  Read-only discovery, used to give every system that is already
 * on the disk its chainload entry in the appliance's boot menu.
 */
struct efi_fat32_esp_loader {
    char vendor[EFI_FAT32_ESP_VENDOR_LENGTH];
    char path[EFI_FAT32_ESP_LOADER_PATH_LENGTH];
    uint32_t size;
};

struct efi_fat32_esp {
    uint32_t version;
    bool bound;
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint32_t sectors_per_fat;
    uint32_t root_cluster;
    uint32_t data_start_lba;
    uint32_t cluster_count;
    /*
     * ITEM-405: the extent the volume's own boot sector declares (FAT32 offset
     * 32, FAT16 offset 19).  It is never larger than `block_count` - the bind
     * refuses a volume whose boot sector claims more sectors than the partition
     * it lives in - and `cluster_count` is derived from it rather than from the
     * partition, so the cluster space a caller can address is the cluster space
     * the volume actually describes.
     */
    uint32_t volume_sectors;
    uint64_t first_lba;
    uint64_t block_count;
    struct block_device device;
};

/*
 * ITEM-405: bind a FAT32 volume that lives at `first_lba` inside a partition of
 * `block_count` sectors.
 *
 * The boot sector is validated against the partition *and* against itself: the
 * volume's declared total-sector count must exist, must not exceed
 * `block_count`, and the data region the FAT describes (`reserved_sectors +
 * fat_count * sectors_per_fat + cluster_count * sectors_per_cluster`) must fit
 * inside it.  A volume that does not fit its partition - the shape a stale
 * foreign filesystem leaves behind after a partition table is replaced - is
 * refused with EFI_FAT32_ESP_UNSUPPORTED_GEOMETRY instead of being treated as
 * a usable volume.  A partition larger than the volume it declares still binds,
 * and the cluster count then comes from the declared volume.
 */
enum efi_fat32_esp_status efi_fat32_esp_bind(
    const struct efi_boot_environment *environment,
    uint64_t first_lba,
    uint64_t block_count,
    struct efi_fat32_esp *esp);
/*
 * ITEM-517: the same bind without an EFI environment.  The legacy-BIOS FAT
 * boot volume is a plain FAT32 volume on a machine with no UEFI, so the BIOS
 * boot path must not construct an EFI environment to reach the writer.  The
 * validation and the resulting `struct efi_fat32_esp` are exactly the ones
 * `efi_fat32_esp_bind()` produces; only the environment checks are skipped.
 */
enum efi_fat32_esp_status efi_fat32_esp_bind_device(
    struct block_device *device,
    uint64_t first_lba,
    uint64_t block_count,
    struct efi_fat32_esp *esp);
enum efi_fat32_esp_status efi_fat32_esp_install_payload(
    struct efi_fat32_esp *esp,
    const unsigned char *payload,
    size_t payload_size);
enum efi_fat32_esp_status efi_fat32_esp_verify_payload(
    const struct efi_fat32_esp *esp);
/*
 * ITEM-396.  The Limine configuration is a second file in the same
 * `\EFI\HFOC` directory as the boot payload: it is written, verified and read
 * back through the same bounded, rollback-on-flush-failure path the payload
 * uses, and it never deletes anything else on a shared ESP.
 */
enum efi_fat32_esp_status efi_fat32_esp_install_config(
    struct efi_fat32_esp *esp,
    const unsigned char *config,
    size_t config_length);
enum efi_fat32_esp_status efi_fat32_esp_verify_config(
    const struct efi_fat32_esp *esp);
enum efi_fat32_esp_status efi_fat32_esp_read_config(
    const struct efi_fat32_esp *esp,
    unsigned char *config,
    size_t capacity,
    size_t *config_length);
/*
 * Read-only inventory of the loaders already on the ESP: the subdirectories of
 * `\EFI` and, in each of them, the first UEFI loader this module recognises.
 * At most `capacity` loaders are reported and the directory walk is bounded by
 * the cluster chain, so a corrupt or hostile volume cannot make it loop.  A
 * volume with no `\EFI` directory is an empty inventory, not an error.
 */
enum efi_fat32_esp_status efi_fat32_esp_list_loaders(
    const struct efi_fat32_esp *esp,
    struct efi_fat32_esp_loader *loaders,
    size_t capacity,
    size_t *count);
#define EFI_FAT32_ESP_FREE_SCAN_MAX_SECTORS 16384U
/*
 * Read-only count of the volume's free clusters, used to refuse an install on a
 * shared ESP that cannot hold the appliance's files *before* the first write.
 * The scan reads the first FAT copy sector by sector, so it is bounded by
 * `sectors_per_fat`; a volume whose FAT is larger than
 * EFI_FAT32_ESP_FREE_SCAN_MAX_SECTORS is refused with CAPACITY rather than
 * scanned, because a bounded installer does not walk an unbounded FAT.
 */
enum efi_fat32_esp_status efi_fat32_esp_free_clusters(
    const struct efi_fat32_esp *esp,
    uint32_t *free_clusters);
enum efi_fat32_esp_status efi_fat32_esp_read_payload(
    const struct efi_fat32_esp *esp,
    unsigned char *payload,
    size_t capacity,
    size_t *payload_size);
enum efi_fat32_esp_status efi_fat32_esp_validate_path(
    const char *path,
    size_t path_length);
enum efi_fat32_esp_status efi_fat32_esp_validate_payload_path(
    const char *path);
enum efi_fat32_esp_status efi_fat32_esp_payload_requirement(
    const struct efi_fat32_esp_payload *payloads,
    size_t payload_count,
    uint32_t sectors_per_cluster,
    struct efi_fat32_esp_requirement *requirement);
enum efi_fat32_esp_status efi_fat32_esp_install_payload_set(
    struct efi_fat32_esp *esp,
    const struct efi_fat32_esp_payload *payloads,
    size_t payload_count);
enum efi_fat32_esp_status efi_fat32_esp_verify_payload_set(
    const struct efi_fat32_esp *esp,
    const struct efi_fat32_esp_payload *payloads,
    size_t payload_count,
    size_t sealed_count,
    unsigned char sealed_digest[SHA256_DIGEST_SIZE],
    uint64_t *sealed_length);
enum efi_fat32_esp_status efi_fat32_esp_read_file(
    const struct efi_fat32_esp *esp,
    const char *path,
    unsigned char *buffer,
    size_t capacity,
    size_t *size_out);
enum efi_fat32_esp_status efi_fat32_esp_digest_file(
    const struct efi_fat32_esp *esp,
    const char *path,
    unsigned char digest[SHA256_DIGEST_SIZE],
    size_t *size_out);
/*
 * ITEM-398: probe a path and rename a file in place.
 *
 * A shared ESP can already carry the two paths the installed boot set uses: a
 * foreign Limine configuration at the loader's own first candidate
 * (`\EFI\BOOT\limine.conf`, which would silently shadow ours) and a foreign
 * fallback loader at `\EFI\BOOT\BOOTX64.EFI`.  The probe reports what is
 * there, and the rename keeps the original bytes recoverable by moving the
 * directory entry to a documented name - the data clusters, the FAT chain and
 * every byte of the file are untouched, which is what "preserved" has to mean
 * for somebody else's loader.
 */
enum efi_fat32_esp_status efi_fat32_esp_probe_file(
    const struct efi_fat32_esp *esp,
    const char *path,
    bool *present,
    uint32_t *size);
enum efi_fat32_esp_status efi_fat32_esp_rename_file(
    struct efi_fat32_esp *esp,
    const char *from_path,
    const char *to_path);
/*
 * ITEM-410: one file of ours, preallocated to a fixed extent, so the appliance
 * boot log can be written into it on every boot without the FAT ever changing
 * again.
 *
 * `create_fixed_file()` allocates the whole extent in one chain, zeroes it and
 * writes the one directory entry for `path`; a file that is already exactly
 * `size` bytes long and whose chain is already exactly `size / cluster_bytes`
 * clusters long is reported with `*reused_out = true` and not one byte of it is
 * written - which is what makes the second boot append to the log the first one
 * left instead of starting over.  `size` must be a whole number of clusters.
 *
 * `write_file_range()` overwrites bytes inside that extent: it never allocates,
 * never frees and never changes the file's length, and a range that would grow
 * the file is refused with EFI_FAT32_ESP_OUT_OF_RANGE.  `read_file_range()`
 * reads the extent back through the same chain walk and reports the file's
 * declared length, so a caller can tell a truncated file from a short one.
 */
enum efi_fat32_esp_status efi_fat32_esp_create_fixed_file(
    struct efi_fat32_esp *esp,
    const char *path,
    uint32_t size,
    uint32_t *first_cluster_out,
    bool *reused_out);
enum efi_fat32_esp_status efi_fat32_esp_write_file_range(
    struct efi_fat32_esp *esp,
    const char *path,
    uint32_t offset,
    const unsigned char *data,
    size_t length);
enum efi_fat32_esp_status efi_fat32_esp_read_file_range(
    const struct efi_fat32_esp *esp,
    const char *path,
    uint32_t offset,
    unsigned char *buffer,
    size_t capacity,
    size_t *length_out,
    uint32_t *file_size_out);
enum efi_fat32_esp_status efi_fat32_esp_flush(
    struct efi_fat32_esp *esp);
const char *efi_fat32_esp_status_string(
    enum efi_fat32_esp_status status);

/*
 * ITEM-487: what one caller's file-range work actually cost.
 *
 * `hw15` measured the durable boot log's flush at ~42 ms per 512-byte slot and
 * 2.7-3.2 s for a 64-record flush, and the shape of that number is this
 * writer's: every call resolves the path again (`lookup_us`) and walks the
 * file's FAT cluster chain from its first cluster to the requested offset
 * (`traversal_us`, one sector read per FAT entry, and the production ESP
 * declares one sector per cluster).  A caller that writes N separate ranges
 * pays the lookup and the walk N times.
 *
 * This probe is the bounded reading that separates those two spans from the
 * bytes, so "the flush is slow" becomes "the flush spends its time here":
 *
 *   * `transfers` - file-range transfers started (write and read);
 *   * `lookups` - path resolutions, one per transfer;
 *   * `fat_entries_read` - FAT entries the chain walk resolved;
 *   * `sector_reads`, `sector_writes` - device sectors touched;
 *   * `lookup_us`, `traversal_us` - the two spans above, in microseconds.
 *
 * It is a probe, not a policy: the counters are one fixed struct, nothing in
 * any path reacts to them, and a host proof that links this file without a
 * cycle counter reads zeroes instead of failing to link.
 */
struct efi_fat32_esp_io_stats {
    uint64_t transfers;
    uint64_t lookups;
    uint64_t fat_entries_read;
    uint64_t sector_reads;
    uint64_t sector_writes;
    uint64_t lookup_us;
    uint64_t traversal_us;
};

/* Start a reading: the counters then describe the work done after this call. */
void efi_fat32_esp_io_stats_reset(void);
/* Copy the current reading; false when there is nothing to read. */
bool efi_fat32_esp_io_stats_read(struct efi_fat32_esp_io_stats *stats_out);

#endif
