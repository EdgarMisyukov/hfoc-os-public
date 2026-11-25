#ifndef HFOC_PCI_H
#define HFOC_PCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PCI_CONFIG_HEADER_LENGTH 0x40U
#define PCI_MAX_BUS 256U
#define PCI_MAX_DEVICE 32U
#define PCI_MAX_FUNCTION 8U
#define PCI_BAR_COUNT 6U
#define PCI_BAR_OFFSET 0x10U
#define PCI_BAR_REGISTER_SIZE 4U
/* Memory BARs: the specification's 16-byte minimum and alignment. */
#define PCI_BAR_ALIGNMENT 16U
/*
 * I/O BARs: the base occupies bits 31:2 (bit 1 is reserved), so an I/O range
 * is at least one doubleword and is aligned to its own size - not to the
 * memory minimum.  ITEM-540: the common path rejected every legitimate small
 * or non-16-byte-aligned I/O BAR, which aborted a whole class scan.
 */
#define PCI_BAR_IO_ALIGNMENT 4U
#define PCI_BAR_INDEX_NONE 0xFFU
/*
 * ITEM-543: which BARs a scan must be able to decode.  A device may carry a
 * BAR its driver never touches (the Intel 7-series AHCI controller's legacy
 * I/O BAR0), so the caller names the BARs it needs.  `PCI_BAR_REQUIRED_ALL`
 * keeps the older contract of every scan entry point that does not choose.
 */
#define PCI_BAR_REQUIRED_NONE 0U
#define PCI_BAR_REQUIRED(bar_index) (1U << (bar_index))
#define PCI_BAR_REQUIRED_ALL ((1U << PCI_BAR_COUNT) - 1U)

#define PCI_CLASS_MASS_STORAGE 0x01U
#define PCI_CLASS_NETWORK 0x02U
/*
 * A class filter field set to this matches every value of that field.  It is
 * how a caller asks for "every network controller" instead of "every Ethernet
 * controller with no programmed interface", which is the difference between
 * finding a machine's NIC and finding only the NICs somebody predicted.
 */
#define PCI_FILTER_ANY 0xFFU
#define PCI_SUBCLASS_SATA 0x06U
#define PCI_PROGIF_AHCI 0x01U
#define PCI_SUBCLASS_NVME 0x08U
#define PCI_PROGIF_NVME 0x02U

enum pci_status {
    PCI_OK = 0,
    PCI_NULL = 1,
    PCI_IO_ERROR = 2,
    PCI_DEVICE_ABSENT = 3,
    PCI_INVALID_LOCATION = 4,
    PCI_BAD_HEADER = 5,
    PCI_INVALID_BAR = 6,
    PCI_UNSUPPORTED_BAR = 7,
    PCI_BAD_ALIGNMENT = 8,
    PCI_OVERFLOW = 9,
    PCI_CAPACITY = 10,
};

struct pci_location {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
};

typedef bool (*pci_config_read_fn)(
    void *context,
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint16_t offset,
    uint32_t *value);
typedef bool (*pci_config_write_fn)(
    void *context,
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint16_t offset,
    uint32_t value);

struct pci_config_io {
    void *context;
    pci_config_read_fn read32;
    pci_config_write_fn write32;
};

struct pci_bar {
    bool present;
    bool io_space;
    bool prefetchable;
    bool is_64bit;
    uint64_t base;
    uint64_t length;
    /*
     * ITEM-543: PCI_OK when the register was decoded into a usable range or
     * is not implemented at all; PCI_INVALID_BAR when the size probe did not
     * explain a range (a mask read back as zero, or a length that is not a
     * power of two or is below the minimum).  Such a BAR is unusable - it is
     * `present == false` - but it does not sink the device: a driver's own
     * BAR may still decode, and only a scan that asks for this BAR refuses.
     */
    enum pci_status decode_status;
};

struct pci_device_header {
    bool present;
    struct pci_location location;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t revision;
    uint8_t base_class;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t header_type;
    struct pci_bar bars[PCI_BAR_COUNT];
};

/*
 * ITEM-540: what a failed class scan stopped on, so the caller publishes the
 * primary error instead of a bare status.  `bar_index` is
 * PCI_BAR_INDEX_NONE when the failure was not a BAR decode; `vendor_id` and
 * `device_id` are zero when the identity read itself failed.
 */
struct pci_scan_failure {
    enum pci_status status;
    struct pci_location location;
    uint16_t vendor_id;
    uint16_t device_id;
    unsigned int bar_index;
};

bool pci_location_valid(const struct pci_location *location);
/*
 * Identity and class only: reads the vendor/device word, the revision/class
 * word and the header type, and never writes a configuration register.  A
 * class scan can therefore filter a bus without probing resources of devices
 * it is not looking for.  Returns PCI_DEVICE_ABSENT for an empty slot.
 */
enum pci_status pci_device_identity(
    struct pci_config_io *io,
    const struct pci_location *location,
    struct pci_device_header *header);
/*
 * Resource preparation for one already-identified device: decodes the BARs of
 * a type-0 header into `header->bars`.  Identity fields are left as read.
 * ITEM-543: a BAR whose size probe cannot explain a range is reported as
 * unusable on that BAR (`present == false`, `decode_status` names why) and
 * does not fail the call; a structurally malformed register (reserved bits or
 * type, I/O BAR5, a base that is not aligned to its range, an overflowing
 * range) and a failed configuration access still do.
 */
enum pci_status pci_device_resources_read(
    struct pci_config_io *io,
    const struct pci_location *location,
    struct pci_device_header *header);
enum pci_status pci_device_read(
    struct pci_config_io *io,
    const struct pci_location *location,
    struct pci_device_header *header);
enum pci_status pci_scan_class(
    struct pci_config_io *io,
    uint8_t base_class,
    uint8_t subclass,
    uint8_t prog_if,
    struct pci_device_header *devices,
    size_t capacity,
    size_t *count);
/*
 * The same scan, with the failing device recorded.  A resource error on a
 * device that does not match the filter is not the caller's failure and no
 * longer stops the search; a matching device's own resource error still does,
 * and is named here.  This entry point asks for every BAR the matched device
 * implements (`PCI_BAR_REQUIRED_ALL`).
 */
enum pci_status pci_scan_class_ex(
    struct pci_config_io *io,
    uint8_t base_class,
    uint8_t subclass,
    uint8_t prog_if,
    struct pci_device_header *devices,
    size_t capacity,
    size_t *count,
    struct pci_scan_failure *failure);
/*
 * ITEM-543: the same scan with the caller naming the BARs it needs.  A
 * matched device is returned when every required BAR decoded, even if another
 * of its BARs could not be sized - the L875 AHCI controller's legacy I/O BAR0
 * must not hide the ABAR at BAR5.  A required BAR that did not decode is the
 * named refusal of that device, with the BAR index and the decode status, so
 * the operator still reads which BAR failed instead of a bare NO_DISK.
 * `required_bars` is a mask of `PCI_BAR_REQUIRED(bar_index)`.
 */
enum pci_status pci_scan_class_required(
    struct pci_config_io *io,
    uint8_t base_class,
    uint8_t subclass,
    uint8_t prog_if,
    uint32_t required_bars,
    struct pci_device_header *devices,
    size_t capacity,
    size_t *count,
    struct pci_scan_failure *failure);
const char *pci_status_string(enum pci_status status);

#endif
