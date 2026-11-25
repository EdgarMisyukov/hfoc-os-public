#ifndef HFOC_INSTALLER_HARDWARE_DISCOVERY_H
#define HFOC_INSTALLER_HARDWARE_DISCOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block_device.h"

#define INSTALLER_HARDWARE_DISCOVERY_VERSION 1U

#define INSTALLER_DISCOVERY_DISK_CAPACITY 4U
#define INSTALLER_DISCOVERY_PARTITION_CAPACITY 64U
#define INSTALLER_DISCOVERY_OS_CAPACITY 4U
#define INSTALLER_DISCOVERY_NIC_CAPACITY 4U
#define INSTALLER_DISCOVERY_PCI_CAPACITY 16U
#define INSTALLER_DISCOVERY_IDENTITY_LENGTH 32U
/*
 * ITEM-495: the name the hardware reports for the disk - the ATA model string
 * ("HFOC Example SATA Fixture") - kept beside the identity, which is the serial.
 * A machine is described by both: the operator recognises the disk by its
 * name, and the plan names it by the identity that cannot repeat.
 */
#define INSTALLER_DISCOVERY_MODEL_LENGTH 41U
#define INSTALLER_DISCOVERY_BOOT_PATH_LENGTH 80U

enum installer_firmware_mode {
    INSTALLER_FIRMWARE_UNKNOWN = 0,
    INSTALLER_FIRMWARE_UEFI = 1,
    INSTALLER_FIRMWARE_BIOS_LEGACY = 2,
};

enum installer_disk_controller_kind {
    INSTALLER_DISK_CONTROLLER_UNKNOWN = 0,
    INSTALLER_DISK_CONTROLLER_VIRTIO_BLK = 1,
    INSTALLER_DISK_CONTROLLER_SATA = 2,
    INSTALLER_DISK_CONTROLLER_NVME = 3,
};

enum installer_partition_table_kind {
    INSTALLER_PARTITION_TABLE_NONE = 0,
    INSTALLER_PARTITION_TABLE_MBR = 1,
    INSTALLER_PARTITION_TABLE_GPT = 2,
};

enum installer_partition_kind {
    INSTALLER_PARTITION_UNKNOWN = 0,
    INSTALLER_PARTITION_LINUX_ROOT = 1,
    INSTALLER_PARTITION_WINDOWS = 2,
    INSTALLER_PARTITION_EFI_SYSTEM = 3,
    INSTALLER_PARTITION_LINUX_SWAP = 4,
    INSTALLER_PARTITION_OTHER = 5,
};

enum installer_os_kind {
    INSTALLER_OS_UNKNOWN = 0,
    INSTALLER_OS_LINUX = 1,
    INSTALLER_OS_WINDOWS = 2,
    INSTALLER_OS_OTHER = 3,
};

/*
 * ITEM-501: what actually lives in a partition, on top of the plain partition
 * table.  The discovery used to describe a Fedora laptop as one large
 * Linux/unknown partition because LVM, Btrfs and the MBR extended container
 * were invisible: the operator could not tell which row was a plain partition
 * and which one was a volume group or a subvolume set.  The layout is named in
 * the model, so the record and the screen can name it too.
 */
enum installer_partition_layout {
    /* A plain partition with its own filesystem (or none). */
    INSTALLER_PARTITION_LAYOUT_PLAIN = 0,
    /* A Linux LVM physical volume; the volumes inside are not visible. */
    INSTALLER_PARTITION_LAYOUT_LVM = 1,
    /* A Btrfs filesystem; its subvolumes are not visible. */
    INSTALLER_PARTITION_LAYOUT_BTRFS = 2,
    /* The MBR extended container itself: a table, not a filesystem. */
    INSTALLER_PARTITION_LAYOUT_EXTENDED = 3,
    /* An MBR logical partition inside the extended container. */
    INSTALLER_PARTITION_LAYOUT_LOGICAL = 4,
};

const char *installer_partition_layout_name(
    enum installer_partition_layout layout);

enum installer_pci_category {
    INSTALLER_PCI_UNKNOWN = 0,
    INSTALLER_PCI_STORAGE = 1,
    INSTALLER_PCI_NETWORK = 2,
    INSTALLER_PCI_DISPLAY = 3,
    INSTALLER_PCI_OTHER = 4,
};

enum installer_discovery_status {
    INSTALLER_DISCOVERY_OK = 0,
    INSTALLER_DISCOVERY_NULL = 1,
    INSTALLER_DISCOVERY_BAD_STATE = 2,
    INSTALLER_DISCOVERY_BAD_VERSION = 3,
    INSTALLER_DISCOVERY_INVALID_ARGUMENT = 4,
    INSTALLER_DISCOVERY_ALREADY_PUBLISHED = 5,
    INSTALLER_DISCOVERY_OUT_OF_RANGE = 6,
    INSTALLER_DISCOVERY_INVALID_BLOCK_DEVICE = 7,
    INSTALLER_DISCOVERY_IO_ERROR = 8,
    INSTALLER_DISCOVERY_INVALID_PARTITION_TABLE = 9,
    INSTALLER_DISCOVERY_UNSUPPORTED_PARTITION_TABLE = 10,
    INSTALLER_DISCOVERY_TOO_MANY_PARTITIONS = 11,
    INSTALLER_DISCOVERY_AMBIGUOUS = 12,
    INSTALLER_DISCOVERY_READ_ONLY_VIOLATION = 13,
    INSTALLER_DISCOVERY_UNSUPPORTED_BLOCK_SIZE = 14,
};

struct installer_discovery_disk {
    uint32_t id;
    bool present;
    enum installer_disk_controller_kind controller;
    enum installer_partition_table_kind partition_table;
    uint32_t block_size;
    uint64_t block_count;
    bool has_efi_system_partition;
    char identity[INSTALLER_DISCOVERY_IDENTITY_LENGTH];
    /* ITEM-495: the name the hardware reported, or empty when it did not. */
    char model[INSTALLER_DISCOVERY_MODEL_LENGTH];
};

struct installer_discovery_partition {
    uint32_t id;
    bool present;
    uint32_t disk_id;
    uint32_t partition_index;
    uint64_t first_lba;
    uint64_t length_lba;
    enum installer_partition_kind kind;
    enum installer_os_kind owner;
    bool efi_system;
    /*
     * ITEM-501: the container or volume layout the partition carries, and
     * whether this row is a container (the extended partition) rather than a
     * partition a caller may use as a target.
     */
    enum installer_partition_layout layout;
    bool container;
};

struct installer_discovery_nic {
    uint32_t id;
    bool present;
    uint16_t vendor_id;
    uint16_t product_id;
    uint32_t bus_address;
    uint8_t mac_address[6U];
};

struct installer_discovery_pci {
    uint32_t id;
    bool present;
    uint16_t vendor_id;
    uint16_t product_id;
    uint32_t bus_address;
    uint16_t class_code;
    enum installer_pci_category category;
};

struct installer_discovery_os {
    uint32_t id;
    bool present;
    enum installer_os_kind kind;
    uint32_t partition_id;
    bool has_efi_entry;
    char boot_path[INSTALLER_DISCOVERY_BOOT_PATH_LENGTH];
};

struct installer_discovery_report {
    uint32_t version;
    enum installer_firmware_mode firmware_mode;
    bool cpu_present;
    uint32_t cpu_cores;
    bool ram_present;
    uint64_t ram_bytes;
    bool read_only;
    bool scan_complete;
    bool manual_selection_required;
    uint32_t disk_count;
    uint32_t partition_count;
    uint32_t os_count;
    uint32_t nic_count;
    uint32_t pci_count;
    struct installer_discovery_disk disks[INSTALLER_DISCOVERY_DISK_CAPACITY];
    struct installer_discovery_partition
        partitions[INSTALLER_DISCOVERY_PARTITION_CAPACITY];
    struct installer_discovery_os oss[INSTALLER_DISCOVERY_OS_CAPACITY];
    struct installer_discovery_nic nics[INSTALLER_DISCOVERY_NIC_CAPACITY];
    struct installer_discovery_pci pcis[INSTALLER_DISCOVERY_PCI_CAPACITY];
};

enum installer_discovery_status installer_discovery_init(
    struct installer_discovery_report *report);
enum installer_discovery_status installer_discovery_publish_firmware(
    struct installer_discovery_report *report,
    enum installer_firmware_mode mode);
enum installer_discovery_status installer_discovery_publish_cpu(
    struct installer_discovery_report *report,
    uint32_t cpu_cores);
enum installer_discovery_status installer_discovery_publish_memory(
    struct installer_discovery_report *report,
    uint64_t ram_bytes);
enum installer_discovery_status installer_discovery_publish_disk(
    struct installer_discovery_report *report,
    const struct installer_discovery_disk *disk);
enum installer_discovery_status installer_discovery_publish_partition(
    struct installer_discovery_report *report,
    const struct installer_discovery_partition *partition);
enum installer_discovery_status installer_discovery_publish_nic(
    struct installer_discovery_report *report,
    const struct installer_discovery_nic *nic);
enum installer_discovery_status installer_discovery_publish_pci(
    struct installer_discovery_report *report,
    const struct installer_discovery_pci *pci);
enum installer_discovery_status installer_discovery_publish_os(
    struct installer_discovery_report *report,
    const struct installer_discovery_os *os);
enum installer_discovery_status installer_discovery_scan_block_device(
    struct installer_discovery_report *report,
    struct block_device *device,
    uint32_t disk_id,
    enum installer_disk_controller_kind controller,
    const char *identity,
    size_t identity_length);
/*
 * ITEM-495: name the disk the way the hardware named it.  The model is not part
 * of the identity - two disks never share a serial, and a model is shared by
 * every disk of its kind - so it is recorded beside it rather than folded into
 * it.  A caller that read no name simply does not call this, and the screen
 * then shows the identity alone.  Refused - and the report unchanged - for a
 * disk this report does not hold, for text that is empty, too long, or carries
 * a byte a terminal cannot show, and after the report has been finalized.
 */
enum installer_discovery_status installer_discovery_set_disk_model(
    struct installer_discovery_report *report,
    uint32_t disk_id,
    const char *model,
    size_t model_length);
enum installer_discovery_status installer_discovery_finalize(
    struct installer_discovery_report *report);
enum installer_discovery_status installer_discovery_validate(
    const struct installer_discovery_report *report);
const char *installer_discovery_status_string(
    enum installer_discovery_status status);

#endif
