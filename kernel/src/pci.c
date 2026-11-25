#include "pci.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool pci_config_offset_valid(uint16_t offset)
{
    return (offset % PCI_BAR_REGISTER_SIZE) == 0U &&
           offset + PCI_BAR_REGISTER_SIZE <= PCI_CONFIG_HEADER_LENGTH;
}

static enum pci_status pci_config_read32(
    struct pci_config_io *io,
    const struct pci_location *location,
    uint16_t offset,
    uint32_t *value)
{
    if (io == NULL || io->read32 == NULL ||
        location == NULL || value == NULL) {
        return PCI_NULL;
    }
    if (!pci_location_valid(location) ||
        !pci_config_offset_valid(offset)) {
        return PCI_INVALID_LOCATION;
    }
    if (!io->read32(io->context, location->bus, location->device,
                    location->function, offset, value)) {
        return PCI_IO_ERROR;
    }
    return PCI_OK;
}

static enum pci_status pci_config_write32(
    struct pci_config_io *io,
    const struct pci_location *location,
    uint16_t offset,
    uint32_t value)
{
    if (io == NULL || io->write32 == NULL ||
        location == NULL) {
        return PCI_NULL;
    }
    if (!pci_location_valid(location) ||
        !pci_config_offset_valid(offset)) {
        return PCI_INVALID_LOCATION;
    }
    if (!io->write32(io->context, location->bus, location->device,
                     location->function, offset, value)) {
        return PCI_IO_ERROR;
    }
    return PCI_OK;
}

static bool pci_bar_length_valid(uint64_t length, bool io_space)
{
    const uint64_t minimum =
        io_space ? PCI_BAR_IO_ALIGNMENT : PCI_BAR_ALIGNMENT;

    return length != 0U &&
           (length & (length - 1U)) == 0U &&
           length >= minimum;
}

static enum pci_status pci_bar_decode(
    struct pci_config_io *io,
    const struct pci_location *location,
    unsigned int bar_index,
    struct pci_bar *bar)
{
    const uint16_t offset =
        (uint16_t)(PCI_BAR_OFFSET + bar_index * PCI_BAR_REGISTER_SIZE);
    enum pci_status status;
    uint32_t raw0;
    uint32_t mask0;
    uint32_t mask1 = 0U;
    uint32_t original_raw0;
    uint32_t original_raw1 = 0U;
    uint64_t combined_mask;
    uint64_t base;
    uint64_t length;
    bool io_space;
    bool is_64bit;
    unsigned int info_mask;

    status = pci_config_read32(io, location, offset, &raw0);
    if (status != PCI_OK) {
        return status;
    }
    if (raw0 == UINT32_MAX || raw0 == 0U) {
        memset(bar, 0, sizeof(*bar));
        return PCI_OK;
    }

    io_space = (raw0 & 1U) != 0U;
    if (io_space) {
        is_64bit = false;
        /* I/O BAR: bit 1 is reserved; the base starts at bit 2. */
        if ((raw0 & 2U) != 0U) {
            return PCI_INVALID_BAR;
        }
    } else {
        const unsigned int type = (raw0 >> 1U) & 3U;

        if (type == 1U) {
            return PCI_INVALID_BAR;
        }
        is_64bit = type == 2U;
    }
    if (io_space && bar_index == 5U) {
        return PCI_INVALID_BAR;
    }

    original_raw0 = raw0;
    if (is_64bit) {
        status = pci_config_read32(
            io, location, (uint16_t)(offset + PCI_BAR_REGISTER_SIZE),
            &original_raw1);
        if (status != PCI_OK) {
            return status;
        }
    }
    status = pci_config_write32(
        io, location, offset, UINT32_MAX);
    if (status != PCI_OK) {
        return status;
    }
    if (is_64bit) {
        status = pci_config_write32(
            io, location, (uint16_t)(offset + PCI_BAR_REGISTER_SIZE),
            UINT32_MAX);
        if (status != PCI_OK) {
            return status;
        }
        status = pci_config_read32(
            io, location, (uint16_t)(offset + PCI_BAR_REGISTER_SIZE),
            &mask1);
        if (status != PCI_OK) {
            return status;
        }
    }
    status = pci_config_read32(io, location, offset, &mask0);
    if (status != PCI_OK) {
        return status;
    }

    status = pci_config_write32(
        io, location, offset, original_raw0);
    if (status != PCI_OK) {
        return status;
    }
    if (is_64bit) {
        status = pci_config_write32(
            io, location, (uint16_t)(offset + PCI_BAR_REGISTER_SIZE),
            original_raw1);
        if (status != PCI_OK) {
            return status;
        }
    }

    if (is_64bit) {
        combined_mask =
            ((uint64_t)mask1 << 32U) | (uint64_t)mask0;
    } else {
        combined_mask = mask0;
    }
    info_mask = io_space ? 3U : 0xFU;
    combined_mask &= ~(uint64_t)info_mask;
    /*
     * ITEM-543: the size probe is how software learns a BAR's range, and a
     * device may fail to answer it - the Intel 7-series AHCI controller's
     * legacy I/O BAR0 reads back 0x0000FFF9 (a 16-bit decode whose length
     * 0xFFFF0008 is not a power of two).  The register cannot be used, but
     * that says nothing about the device: its ABAR at BAR5 decodes normally.
     * The BAR is reported unusable, with the reason on it, and a caller that
     * needs it still refuses by name through pci_scan_class_required().  A
     * structurally malformed register stays a hard error below.
     */
    if (combined_mask == 0U) {
        memset(bar, 0, sizeof(*bar));
        /* Keep the pair bit: the upper half must not be probed on its own. */
        bar->is_64bit = is_64bit;
        bar->decode_status = PCI_INVALID_BAR;
        return PCI_OK;
    }
    if (is_64bit) {
        length = UINT64_MAX - combined_mask + 1U;
    } else {
        length = (uint64_t)(UINT32_MAX - (uint32_t)combined_mask) + 1U;
    }
    if (!pci_bar_length_valid(length, io_space)) {
        memset(bar, 0, sizeof(*bar));
        bar->is_64bit = is_64bit;
        bar->decode_status = PCI_INVALID_BAR;
        return PCI_OK;
    }

    if (is_64bit) {
        base = ((uint64_t)original_raw1 << 32U) | original_raw0;
        base &= ~(uint64_t)info_mask;
    } else {
        base = original_raw0 & ~(uint64_t)info_mask;
    }
    /*
     * A BAR is aligned to the range it declares: 16 bytes or more for memory
     * (implied by the minimum above), one doubleword or more for I/O.
     */
    if (base == 0U || base % length != 0U) {
        return PCI_BAD_ALIGNMENT;
    }
    if (base > UINT64_MAX - length) {
        return PCI_OVERFLOW;
    }
    if (!is_64bit && !io_space &&
        base + length > UINT64_C(0x100000000)) {
        return PCI_OVERFLOW;
    }

    memset(bar, 0, sizeof(*bar));
    bar->present = true;
    bar->io_space = io_space;
    bar->prefetchable = (raw0 & (1U << 3U)) != 0U;
    bar->is_64bit = is_64bit;
    bar->base = base;
    bar->length = length;
    return PCI_OK;
}

bool pci_location_valid(const struct pci_location *location)
{
    return location != NULL &&
           location->device < PCI_MAX_DEVICE &&
           location->function < PCI_MAX_FUNCTION;
}

enum pci_status pci_device_identity(
    struct pci_config_io *io,
    const struct pci_location *location,
    struct pci_device_header *header)
{
    struct pci_device_header draft;
    enum pci_status status;
    uint32_t vendor_device;
    uint32_t revision_class;
    uint32_t header_register;

    if (io == NULL || location == NULL || header == NULL) {
        return PCI_NULL;
    }
    if (!pci_location_valid(location)) {
        return PCI_INVALID_LOCATION;
    }

    status = pci_config_read32(io, location, 0U, &vendor_device);
    if (status != PCI_OK) {
        return status;
    }
    if ((uint16_t)vendor_device == 0xFFFFU ||
        ((vendor_device >> 16U) & 0xFFFFU) == 0xFFFFU) {
        return PCI_DEVICE_ABSENT;
    }
    status = pci_config_read32(io, location, 8U, &revision_class);
    if (status != PCI_OK) {
        return status;
    }
    status = pci_config_read32(io, location, 0x0CU, &header_register);
    if (status != PCI_OK) {
        return status;
    }

    memset(&draft, 0, sizeof(draft));
    draft.present = true;
    draft.location = *location;
    draft.vendor_id = (uint16_t)vendor_device;
    draft.device_id = (uint16_t)(vendor_device >> 16U);
    draft.revision = (uint8_t)revision_class;
    draft.prog_if = (uint8_t)(revision_class >> 8U);
    draft.subclass = (uint8_t)(revision_class >> 16U);
    draft.base_class = (uint8_t)(revision_class >> 24U);
    draft.header_type = (uint8_t)(header_register >> 16U);
    *header = draft;
    return PCI_OK;
}

static enum pci_status pci_device_decode_bars(
    struct pci_config_io *io,
    const struct pci_location *location,
    struct pci_device_header *header,
    unsigned int *failed_bar)
{
    unsigned int bar_index;

    /* Only type-0 headers expose BARs at 0x10..0x24. Type-1 headers use those
       words for bridge bus windows and must not be probed as BARs. */
    if ((header->header_type & 0x7FU) != 0U) {
        return PCI_OK;
    }
    for (bar_index = 0U; bar_index < PCI_BAR_COUNT; ++bar_index) {
        enum pci_status status;

        if (bar_index > 0U && header->bars[bar_index - 1U].is_64bit) {
            continue;
        }
        status = pci_bar_decode(
            io, location, bar_index, &header->bars[bar_index]);
        if (status != PCI_OK) {
            if (failed_bar != NULL) {
                *failed_bar = bar_index;
            }
            return status;
        }
    }
    return PCI_OK;
}

enum pci_status pci_device_resources_read(
    struct pci_config_io *io,
    const struct pci_location *location,
    struct pci_device_header *header)
{
    if (io == NULL || location == NULL || header == NULL) {
        return PCI_NULL;
    }
    if (!pci_location_valid(location)) {
        return PCI_INVALID_LOCATION;
    }
    if (!header->present) {
        return PCI_BAD_HEADER;
    }
    return pci_device_decode_bars(io, location, header, NULL);
}

enum pci_status pci_device_read(
    struct pci_config_io *io,
    const struct pci_location *location,
    struct pci_device_header *header)
{
    struct pci_device_header draft;
    enum pci_status status;

    status = pci_device_identity(io, location, &draft);
    if (status != PCI_OK) {
        return status;
    }
    status = pci_device_decode_bars(io, location, &draft, NULL);
    if (status != PCI_OK) {
        return status;
    }
    *header = draft;
    return PCI_OK;
}

/*
 * ITEM-543: a decoded device satisfies a scan only when every BAR that caller
 * named decoded.  `decode_status` is PCI_OK for a BAR that was decoded and
 * for one that is not implemented at all (including the upper half of a
 * 64-bit BAR, which is never probed separately), so this asks exactly "did
 * every BAR the caller needs come out of its probe usable".
 */
static enum pci_status pci_required_bars_status(
    const struct pci_device_header *header,
    uint32_t required_bars,
    unsigned int *failed_bar)
{
    unsigned int bar_index;

    for (bar_index = 0U; bar_index < PCI_BAR_COUNT; ++bar_index) {
        if ((required_bars & PCI_BAR_REQUIRED(bar_index)) == 0U) {
            continue;
        }
        if (header->bars[bar_index].decode_status != PCI_OK) {
            if (failed_bar != NULL) {
                *failed_bar = bar_index;
            }
            return header->bars[bar_index].decode_status;
        }
    }
    return PCI_OK;
}

enum pci_status pci_scan_class_required(
    struct pci_config_io *io,
    uint8_t base_class,
    uint8_t subclass,
    uint8_t prog_if,
    uint32_t required_bars,
    struct pci_device_header *devices,
    size_t capacity,
    size_t *count,
    struct pci_scan_failure *failure)
{
    size_t found;
    unsigned int bus_index;

    if (failure != NULL) {
        memset(failure, 0, sizeof(*failure));
        failure->status = PCI_OK;
        failure->bar_index = PCI_BAR_INDEX_NONE;
    }
    if (io == NULL || devices == NULL || count == NULL) {
        return PCI_NULL;
    }
    if (capacity == 0U) {
        return PCI_CAPACITY;
    }

    found = 0U;
    for (bus_index = 0U; bus_index < PCI_MAX_BUS; ++bus_index) {
        struct pci_location location;
        uint8_t device;

        location.bus = (uint8_t)bus_index;
        for (device = 0U; device < PCI_MAX_DEVICE; ++device) {
            uint8_t function;

            for (function = 0U;
                 function < PCI_MAX_FUNCTION;
                 ++function) {
                struct pci_device_header header;
                enum pci_status status;

                location.device = device;
                location.function = function;
                /*
                 * ITEM-540: the identity/class word decides whether this
                 * device is the one being looked for; only a match is asked
                 * for its resources.  A resource error on anybody else is not
                 * this caller's failure and must not stop the search.
                 */
                status = pci_device_identity(io, &location, &header);
                if (status == PCI_DEVICE_ABSENT) {
                    continue;
                }
                if (status != PCI_OK) {
                    if (failure != NULL) {
                        failure->status = status;
                        failure->location = location;
                    }
                    return status;
                }
                if (header.base_class != base_class ||
                    (subclass != PCI_FILTER_ANY &&
                     header.subclass != subclass) ||
                    (prog_if != PCI_FILTER_ANY &&
                     header.prog_if != prog_if)) {
                    continue;
                }
                {
                    unsigned int failed_bar = PCI_BAR_INDEX_NONE;

                    status = pci_device_decode_bars(
                        io, &location, &header, &failed_bar);
                    if (status == PCI_OK) {
                        /*
                         * ITEM-543: a BAR that could not be sized is not a
                         * matching device's failure by itself - only a BAR
                         * this caller needs is.
                         */
                        status = pci_required_bars_status(
                            &header, required_bars, &failed_bar);
                    }
                    if (status != PCI_OK) {
                        if (failure != NULL) {
                            failure->status = status;
                            failure->location = location;
                            failure->vendor_id = header.vendor_id;
                            failure->device_id = header.device_id;
                            failure->bar_index = failed_bar;
                        }
                        return status;
                    }
                }
                if (found >= capacity) {
                    return PCI_CAPACITY;
                }
                devices[found++] = header;
            }
        }
    }

    *count = found;
    return PCI_OK;
}

enum pci_status pci_scan_class_ex(
    struct pci_config_io *io,
    uint8_t base_class,
    uint8_t subclass,
    uint8_t prog_if,
    struct pci_device_header *devices,
    size_t capacity,
    size_t *count,
    struct pci_scan_failure *failure)
{
    return pci_scan_class_required(io, base_class, subclass, prog_if,
                                   PCI_BAR_REQUIRED_ALL, devices, capacity,
                                   count, failure);
}

enum pci_status pci_scan_class(
    struct pci_config_io *io,
    uint8_t base_class,
    uint8_t subclass,
    uint8_t prog_if,
    struct pci_device_header *devices,
    size_t capacity,
    size_t *count)
{
    return pci_scan_class_ex(io, base_class, subclass, prog_if,
                             devices, capacity, count, NULL);
}

const char *pci_status_string(enum pci_status status)
{
    switch (status) {
    case PCI_OK:
        return "PCI_OK";
    case PCI_NULL:
        return "PCI_NULL";
    case PCI_IO_ERROR:
        return "PCI_IO_ERROR";
    case PCI_DEVICE_ABSENT:
        return "PCI_DEVICE_ABSENT";
    case PCI_INVALID_LOCATION:
        return "PCI_INVALID_LOCATION";
    case PCI_BAD_HEADER:
        return "PCI_BAD_HEADER";
    case PCI_INVALID_BAR:
        return "PCI_INVALID_BAR";
    case PCI_UNSUPPORTED_BAR:
        return "PCI_UNSUPPORTED_BAR";
    case PCI_BAD_ALIGNMENT:
        return "PCI_BAD_ALIGNMENT";
    case PCI_OVERFLOW:
        return "PCI_OVERFLOW";
    case PCI_CAPACITY:
        return "PCI_CAPACITY";
    default:
        return "PCI_UNKNOWN";
    }
}
