#include "pci.h"
#include "pci_dma.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_PCI_HOST_TEST

#define CONFIG_REGISTER_COUNT 64U
#define TEST_BUS 1U
#define TEST_DEVICE 3U
#define TEST_FUNCTION 0U
#define TEST_NVME_VENDOR 0x8086U
#define TEST_NVME_DEVICE 0x5845U

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

struct config_fixture {
    uint32_t registers[CONFIG_REGISTER_COUNT];
    uint32_t size_masks[CONFIG_REGISTER_COUNT];
    bool probing[CONFIG_REGISTER_COUNT];
    bool fail_read;
    bool fail_write;
    size_t read_calls;
    size_t write_calls;
};

static void fixture_reset(struct config_fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
}

static bool fixture_read(
    void *context,
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint16_t offset,
    uint32_t *value)
{
    struct config_fixture *fixture =
        (struct config_fixture *)context;
    const unsigned int index = (unsigned int)offset / 4U;

    if (bus != TEST_BUS || device != TEST_DEVICE ||
        function != TEST_FUNCTION) {
        *value = UINT32_MAX;
        return true;
    }
    ++fixture->read_calls;
    if (fixture->fail_read) {
        return false;
    }
    if (fixture->probing[index]) {
        *value = fixture->size_masks[index];
    } else {
        *value = fixture->registers[index];
    }
    return true;
}

static bool fixture_write(
    void *context,
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint16_t offset,
    uint32_t value)
{
    struct config_fixture *fixture =
        (struct config_fixture *)context;
    const unsigned int index = (unsigned int)offset / 4U;

    if (bus != TEST_BUS || device != TEST_DEVICE ||
        function != TEST_FUNCTION) {
        return false;
    }
    ++fixture->write_calls;
    if (fixture->fail_write) {
        return false;
    }
    if (value == UINT32_MAX) {
        fixture->probing[index] = true;
    } else {
        fixture->registers[index] = value;
        fixture->probing[index] = false;
    }
    return true;
}

static void fixture_install_nvme(
    struct config_fixture *fixture,
    bool valid_bars)
{
    fixture_reset(fixture);
    fixture->registers[0] =
        (uint32_t)TEST_NVME_VENDOR |
        ((uint32_t)TEST_NVME_DEVICE << 16U);
    fixture->registers[2] = 0x01080201U;
    fixture->registers[3] = 0x00000000U;
    if (valid_bars) {
        fixture->registers[4] = 0xFE000000U;
        fixture->size_masks[4] = 0xFFFFF000U;

        fixture->registers[6] = 0x40000004U;
        fixture->size_masks[6] = 0xFFFF0000U;
        fixture->size_masks[7] = UINT32_MAX;
    } else {
        fixture->registers[4] = 0x00000002U;
        fixture->size_masks[4] = UINT32_MAX;
    }
}

static void fixture_install_bridge(struct config_fixture *fixture)
{
    fixture_reset(fixture);
    fixture->registers[0] = 0x43E91022U;
    fixture->registers[2] = 0x06040000U;
    fixture->registers[3] = 0x00010000U;
    /* Type-1 bus-window words would look like BAR values if misinterpreted. */
    fixture->registers[4] = 0x00030201U;
    fixture->registers[5] = 0x00F1F100U;
    fixture->registers[6] = 0xFCD0FCD0U;
    fixture->registers[7] = 0xFFFFFFFFU;
}

static struct pci_config_io fixture_io(struct config_fixture *fixture)
{
    struct pci_config_io io;

    memset(&io, 0, sizeof(io));
    io.context = fixture;
    io.read32 = fixture_read;
    io.write32 = fixture_write;
    return io;
}

static struct pci_location test_location(void)
{
    struct pci_location location;

    location.bus = TEST_BUS;
    location.device = TEST_DEVICE;
    location.function = TEST_FUNCTION;
    return location;
}

static void test_pci_status_and_location(void)
{
    struct pci_location absent;
    struct pci_location invalid;

    absent.bus = 255U;
    absent.device = 31U;
    absent.function = 7U;
    CHECK(pci_location_valid(&absent),
          "maximum valid PCI location is accepted");

    invalid.device = 0U;
    invalid.function = 0U;
    invalid.bus = 0U;
    invalid.device = 32U;
    CHECK(!pci_location_valid(&invalid),
          "out-of-range PCI device is rejected");
    invalid.device = 0U;
    invalid.function = 8U;
    CHECK(!pci_location_valid(&invalid),
          "out-of-range PCI function is rejected");

    CHECK(strcmp(pci_status_string(PCI_OK), "PCI_OK") == 0,
          "PCI status string is exact");
    CHECK(strcmp(pci_status_string(PCI_OVERFLOW),
                 "PCI_OVERFLOW") == 0,
          "PCI overflow status string is exact");
}

static void test_nvme_class_scan_and_bar_decode(void)
{
    struct config_fixture fixture;
    struct pci_config_io io;
    struct pci_location location;
    struct pci_device_header single;
    struct pci_device_header found[2];
    size_t found_count;
    enum pci_status status;

    fixture_install_nvme(&fixture, true);
    io = fixture_io(&fixture);
    location = test_location();

    status = pci_device_read(&io, &location, &single);
    CHECK(status == PCI_OK && single.present &&
              single.vendor_id == TEST_NVME_VENDOR &&
              single.device_id == TEST_NVME_DEVICE &&
              single.base_class == PCI_CLASS_MASS_STORAGE &&
              single.subclass == PCI_SUBCLASS_NVME &&
              single.prog_if == PCI_PROGIF_NVME,
          "NVMe PCI header is decoded with exact class identity");
    CHECK(single.bars[0].present &&
              !single.bars[0].is_64bit &&
              single.bars[0].base == UINT64_C(0xFE000000) &&
              single.bars[0].length == UINT64_C(0x1000),
          "32-bit MMIO BAR is decoded with exact bounded range");
    CHECK(single.bars[2].present &&
              single.bars[2].is_64bit &&
              single.bars[2].base == UINT64_C(0x40000000) &&
              single.bars[2].length == UINT64_C(0x10000),
          "64-bit MMIO BAR is decoded with exact bounded range");
    CHECK(!single.bars[1].present && !single.bars[3].present &&
              !single.bars[4].present && !single.bars[5].present,
          "absent and consumed PCI BARs are not published");
    CHECK(fixture.read_calls != 0U && fixture.write_calls != 0U,
          "BAR decode performed config reads and probe writes");

    fixture_install_nvme(&fixture, true);
    found_count = 0U;
    status = pci_scan_class(
        &io, PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_NVME,
        PCI_PROGIF_NVME, found, 2U, &found_count);
    CHECK(status == PCI_OK && found_count == 1U &&
              found[0].location.bus == TEST_BUS &&
              found[0].location.device == TEST_DEVICE &&
              found[0].location.function == TEST_FUNCTION,
          "PCI class scan finds the synthetic NVMe namespace controller");

    found_count = 0U;
    status = pci_scan_class(
        &io, PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_NVME,
        0xFFU, found, 0U, &found_count);
    CHECK(status == PCI_CAPACITY,
          "PCI class scan rejects zero-capacity result");
}

static void test_invalid_bar_and_io_failure(void)
{
    struct config_fixture fixture;
    struct pci_config_io io;
    struct pci_location location;
    struct pci_device_header header;

    fixture_install_nvme(&fixture, false);
    io = fixture_io(&fixture);
    location = test_location();
    CHECK(pci_device_read(&io, &location, &header) == PCI_INVALID_BAR,
          "reserved PCI memory BAR type is rejected");

    fixture_install_nvme(&fixture, true);
    fixture.fail_read = true;
    io = fixture_io(&fixture);
    CHECK(pci_device_read(&io, &location, &header) == PCI_IO_ERROR,
          "PCI config read failure surfaces as stable I/O error");

    fixture_install_nvme(&fixture, true);
    fixture.fail_write = true;
    io = fixture_io(&fixture);
    CHECK(pci_device_read(&io, &location, &header) == PCI_IO_ERROR,
          "PCI config write failure surfaces as stable I/O error");
}

static void test_bridge_header_skips_bar_decode(void)
{
    struct config_fixture fixture;
    struct pci_config_io io;
    struct pci_location location;
    struct pci_device_header header;
    struct pci_device_header found[1];
    size_t found_count;
    enum pci_status status;

    fixture_install_bridge(&fixture);
    io = fixture_io(&fixture);
    location = test_location();

    status = pci_device_read(&io, &location, &header);
    CHECK(status == PCI_OK && header.present &&
              header.header_type == 0x01U,
          "type-1 PCI bridge header is decoded without probing bus words");
    CHECK(!header.bars[0].present && !header.bars[1].present &&
              !header.bars[2].present && !header.bars[3].present &&
              !header.bars[4].present && !header.bars[5].present,
          "type-1 PCI bridge does not publish decoded BARs");
    CHECK(fixture.write_calls == 0U,
          "type-1 PCI bridge scan performs no BAR probe writes");

    fixture_install_bridge(&fixture);
    io = fixture_io(&fixture);
    found_count = 99U;
    status = pci_scan_class(
        &io, PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA,
        PCI_PROGIF_AHCI, found, 1U, &found_count);
    CHECK(status == PCI_OK && found_count == 0U,
          "PCI class scan tolerates type-1 bridge functions");
}

/*
 * ITEM-540: several functions on one bus.
 *
 * The single-function fixture above can only show what one device answers.  A
 * class scan walks the whole bus and the reported defect is about what it does
 * with a device it is not looking for, so this fixture answers for four
 * devices (0..3) on TEST_BUS and leaves every other location absent.
 */
#define MULTI_DEVICE_COUNT 4U

struct multi_function {
    uint32_t registers[CONFIG_REGISTER_COUNT];
    uint32_t size_masks[CONFIG_REGISTER_COUNT];
    bool probing[CONFIG_REGISTER_COUNT];
    bool present;
};

struct multi_fixture {
    struct multi_function functions[MULTI_DEVICE_COUNT];
    bool fail_read;
    bool fail_write;
    size_t read_calls;
    size_t write_calls;
    size_t read_calls_by_device[MULTI_DEVICE_COUNT];
    size_t write_calls_by_device[MULTI_DEVICE_COUNT];
};

static void multi_reset(struct multi_fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
}

static bool multi_read(
    void *context,
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint16_t offset,
    uint32_t *value)
{
    struct multi_fixture *fixture = (struct multi_fixture *)context;
    const unsigned int index = (unsigned int)offset / 4U;

    if (bus != TEST_BUS || device >= MULTI_DEVICE_COUNT ||
        function != 0U || !fixture->functions[device].present) {
        *value = UINT32_MAX;
        return true;
    }
    ++fixture->read_calls;
    ++fixture->read_calls_by_device[device];
    if (fixture->fail_read) {
        return false;
    }
    if (fixture->functions[device].probing[index]) {
        *value = fixture->functions[device].size_masks[index];
    } else {
        *value = fixture->functions[device].registers[index];
    }
    return true;
}

static bool multi_write(
    void *context,
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint16_t offset,
    uint32_t value)
{
    struct multi_fixture *fixture = (struct multi_fixture *)context;
    const unsigned int index = (unsigned int)offset / 4U;

    if (bus != TEST_BUS || device >= MULTI_DEVICE_COUNT ||
        function != 0U || !fixture->functions[device].present) {
        return false;
    }
    ++fixture->write_calls;
    ++fixture->write_calls_by_device[device];
    if (fixture->fail_write) {
        return false;
    }
    if (value == UINT32_MAX) {
        fixture->functions[device].probing[index] = true;
    } else {
        fixture->functions[device].registers[index] = value;
        fixture->functions[device].probing[index] = false;
    }
    return true;
}

static void multi_add_function(
    struct multi_fixture *fixture,
    uint8_t device,
    uint16_t vendor_id,
    uint16_t device_id,
    uint8_t base_class,
    uint8_t subclass,
    uint8_t prog_if)
{
    struct multi_function *function = &fixture->functions[device];

    memset(function, 0, sizeof(*function));
    function->present = true;
    function->registers[0] =
        (uint32_t)vendor_id | ((uint32_t)device_id << 16U);
    function->registers[2] =
        ((uint32_t)base_class << 24U) |
        ((uint32_t)subclass << 16U) |
        ((uint32_t)prog_if << 8U);
    /* registers[3] stays 0: a type-0 header, so BARs are probed. */
}

static void multi_set_bar(
    struct multi_fixture *fixture,
    uint8_t device,
    unsigned int bar_index,
    uint32_t raw,
    uint32_t size_mask)
{
    fixture->functions[device].registers[4U + bar_index] = raw;
    fixture->functions[device].size_masks[4U + bar_index] = size_mask;
}

static struct pci_config_io multi_io(struct multi_fixture *fixture)
{
    struct pci_config_io io;

    memset(&io, 0, sizeof(io));
    io.context = fixture;
    io.read32 = multi_read;
    io.write32 = multi_write;
    return io;
}

static void multi_install_ahci(struct multi_fixture *fixture, uint8_t device)
{
    multi_add_function(fixture, device, 0x8086U, 0x1E03U,
                       PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA,
                       PCI_PROGIF_AHCI);
    multi_set_bar(fixture, device, 5U, 0xFE000000U, 0xFFFFF000U);
}

static void test_io_bar_small_ranges_decode(void)
{
    struct multi_fixture fixture;
    struct pci_config_io io;
    struct pci_location location;
    struct pci_device_header header;

    location.bus = TEST_BUS;
    location.device = 1U;
    location.function = 0U;

    /*
     * A legitimate 4-byte I/O BAR: bit 0 marks I/O space, bit 1 is reserved,
     * the base occupies bits 31:2 and the size mask clears those two bits.
     * The specification puts the I/O minimum at one doubleword and aligns an
     * I/O range to its own size, not to the 16-byte memory minimum, and bit 2
     * is part of the base, not another reserved bit.
     */
    multi_reset(&fixture);
    multi_add_function(&fixture, 1U, 0x8086U, 0x1E3AU, 0x0CU, 0x05U, 0x00U);
    multi_set_bar(&fixture, 1U, 4U, 0x0000C005U, 0xFFFFFFFCU);
    io = multi_io(&fixture);
    CHECK(pci_device_read(&io, &location, &header) == PCI_OK &&
              header.bars[4].present && header.bars[4].io_space &&
              !header.bars[4].is_64bit &&
              header.bars[4].base == UINT64_C(0xC004) &&
              header.bars[4].length == UINT64_C(4),
          "4-byte I/O BAR decodes to its own size and alignment");

    /* An 8-byte I/O BAR based at 0xC008 is aligned to 8, not to 16. */
    multi_reset(&fixture);
    multi_add_function(&fixture, 1U, 0x8086U, 0x1E3AU, 0x0CU, 0x05U, 0x00U);
    multi_set_bar(&fixture, 1U, 2U, 0x0000C009U, 0xFFFFFFF8U);
    io = multi_io(&fixture);
    CHECK(pci_device_read(&io, &location, &header) == PCI_OK &&
              header.bars[2].present && header.bars[2].io_space &&
              header.bars[2].base == UINT64_C(0xC008) &&
              header.bars[2].length == UINT64_C(8),
          "8-byte I/O BAR at an 8-byte boundary decodes");

    /* Bit 1 of an I/O BAR is reserved and stays a refusal. */
    multi_reset(&fixture);
    multi_add_function(&fixture, 1U, 0x8086U, 0x1E3AU, 0x0CU, 0x05U, 0x00U);
    multi_set_bar(&fixture, 1U, 0U, 0x0000C003U, 0xFFFFFFFCU);
    io = multi_io(&fixture);
    CHECK(pci_device_read(&io, &location, &header) == PCI_INVALID_BAR,
          "I/O BAR with reserved bit 1 set is rejected");
}

static void test_scan_survives_unrelated_device_resource(void)
{
    struct multi_fixture fixture;
    struct pci_config_io io;
    struct pci_device_header found[2];
    size_t found_count;
    enum pci_status status;

    /*
     * Device 1 is not a storage controller and carries a legitimate 4-byte
     * I/O BAR; device 2 is the AHCI controller with a valid MMIO BAR5.  The
     * old common path decoded every present device's BARs before filtering by
     * class and returned the first resource error, so this scan aborted at
     * device 1 before it ever reached device 2.
     */
    multi_reset(&fixture);
    multi_add_function(&fixture, 1U, 0x8086U, 0x1E3AU, 0x0CU, 0x05U, 0x00U);
    multi_set_bar(&fixture, 1U, 4U, 0x0000C005U, 0xFFFFFFFCU);
    multi_install_ahci(&fixture, 2U);
    io = multi_io(&fixture);
    found_count = 0U;
    status = pci_scan_class(&io, PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA,
                            PCI_PROGIF_AHCI, found, 2U, &found_count);
    CHECK(status == PCI_OK && found_count == 1U &&
              found[0].location.device == 2U &&
              found[0].bars[5].present &&
              found[0].bars[5].base == UINT64_C(0xFE000000),
          "an unrelated device's small I/O BAR does not abort the class scan");

    /* A malformed BAR on a device nobody asked for is the same story. */
    multi_reset(&fixture);
    multi_add_function(&fixture, 0U, 0x1234U, 0x5678U, 0x03U, 0x00U, 0x00U);
    multi_set_bar(&fixture, 0U, 0U, 0x00000002U, UINT32_MAX);
    multi_install_ahci(&fixture, 2U);
    io = multi_io(&fixture);
    found_count = 0U;
    status = pci_scan_class(&io, PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA,
                            PCI_PROGIF_AHCI, found, 2U, &found_count);
    CHECK(status == PCI_OK && found_count == 1U &&
              found[0].location.device == 2U,
          "an unrelated device's malformed BAR does not abort the scan");

    /*
     * The matched controller's own bad resource is still a refusal: the fix
     * narrows who is probed, it does not turn a broken target into a success.
     */
    multi_reset(&fixture);
    multi_add_function(&fixture, 2U, 0x8086U, 0x1E03U,
                       PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA,
                       PCI_PROGIF_AHCI);
    multi_set_bar(&fixture, 2U, 5U, 0x00000002U, UINT32_MAX);
    io = multi_io(&fixture);
    found_count = 99U;
    status = pci_scan_class(&io, PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA,
                            PCI_PROGIF_AHCI, found, 2U, &found_count);
    CHECK(status == PCI_INVALID_BAR,
          "a matched controller's malformed BAR is still a refusal");
}

static void test_scan_probes_only_matched_devices(void)
{
    struct multi_fixture fixture;
    struct pci_config_io io;
    struct pci_device_header found[1];
    size_t found_count = 0U;
    enum pci_status status;

    multi_reset(&fixture);
    multi_add_function(&fixture, 1U, 0x8086U, 0x1E3AU, 0x0CU, 0x05U, 0x00U);
    /*
     * The 8-byte I/O BAR is the case that used to write before it failed: the
     * old decoder accepted bits 2:1, probed the range and only then rejected
     * the 8-byte size, so an unmatched device was written to on every scan.
     */
    multi_set_bar(&fixture, 1U, 2U, 0x0000C009U, 0xFFFFFFF8U);
    multi_install_ahci(&fixture, 2U);
    io = multi_io(&fixture);

    status = pci_scan_class(&io, PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA,
                            PCI_PROGIF_AHCI, found, 1U, &found_count);
    CHECK(status == PCI_OK && found_count == 1U,
          "the scan still finds the one controller it was asked for");
    CHECK(fixture.write_calls_by_device[1] == 0U,
          "a scan performs no BAR probe writes on an unmatched device");
    CHECK(fixture.write_calls_by_device[2] != 0U,
          "a scan probes the matched device's resources");
}

static void test_identity_read_skips_resource_probe(void)
{
    struct multi_fixture fixture;
    struct pci_config_io io;
    struct pci_location location;
    struct pci_device_header header;

    multi_reset(&fixture);
    multi_add_function(&fixture, 1U, 0x8086U, 0x1E3AU, 0x0CU, 0x05U, 0x00U);
    multi_set_bar(&fixture, 1U, 4U, 0x0000C005U, 0xFFFFFFFCU);
    io = multi_io(&fixture);
    location.bus = TEST_BUS;
    location.device = 1U;
    location.function = 0U;

    CHECK(pci_device_identity(&io, &location, &header) == PCI_OK &&
              header.present && header.base_class == 0x0CU &&
              header.subclass == 0x05U && fixture.write_calls == 0U,
          "identity read reports the class without probing resources");

    CHECK(pci_device_resources_read(&io, &location, &header) == PCI_OK &&
              header.bars[4].present && header.bars[4].length == 4U,
          "resource preparation decodes the identified device's BARs");

    location.device = 3U;
    CHECK(pci_device_identity(&io, &location, &header) == PCI_DEVICE_ABSENT,
          "identity read reports an empty slot as absent");
}

static void test_scan_failure_names_the_device(void)
{
    struct multi_fixture fixture;
    struct pci_config_io io;
    struct pci_device_header found[1];
    struct pci_scan_failure failure;
    size_t found_count = 0U;
    enum pci_status status;

    multi_reset(&fixture);
    multi_add_function(&fixture, 2U, 0x8086U, 0x1E03U,
                       PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA,
                       PCI_PROGIF_AHCI);
    multi_set_bar(&fixture, 2U, 5U, 0x00000002U, UINT32_MAX);
    io = multi_io(&fixture);

    status = pci_scan_class_ex(&io, PCI_CLASS_MASS_STORAGE,
                               PCI_SUBCLASS_SATA, PCI_PROGIF_AHCI,
                               found, 1U, &found_count, &failure);
    CHECK(status == PCI_INVALID_BAR && failure.status == PCI_INVALID_BAR &&
              failure.location.bus == TEST_BUS &&
              failure.location.device == 2U &&
              failure.vendor_id == 0x8086U &&
              failure.device_id == 0x1E03U &&
              failure.bar_index == 5U,
          "a failed scan names the device, its identity and its BAR");
}

/*
 * ITEM-543: the L875 laptop and F-8 of 2026-10-16.
 *
 * The machine's Intel 7-series AHCI controller (8086:1e03) works: Linux sees
 * its disk and BAR5 is a normal memory ABAR.  BAR0 is a legacy I/O BAR, and
 * this controller's size probe does not explain its range - a 16-bit I/O
 * decode answers 0x0000FFF9, whose length 0xFFFF0008 is not a power of two.
 * The decoder returned PCI_INVALID_BAR for that one BAR, the class scan gave
 * up on the matched device before AHCI could look at the ABAR, and the
 * installer refused NO_DISK with `probe failed ... bar=0`.
 *
 * A BAR whose size cannot be explained is a BAR that cannot be used; it is
 * not a reason to throw away a device that has a BAR its driver can use.  The
 * three probes below are the shapes that explain no size - the L875's 16-bit
 * decode, a probe that reads back zeroes, and a read-only BAR that answers
 * with its own base.
 */
static void test_unexplained_bar_size_leaves_the_device_usable(void)
{
    static const struct {
        uint32_t raw;
        uint32_t probe;
        const char *message;
    } cases[] = {
        {
            0x00004089U,
            0x0000FFF9U,
            "the L875 legacy I/O BAR whose 16-bit probe names no range",
        },
        {
            0x00004089U,
            0x00000000U,
            "a legacy I/O BAR whose size probe reads back zero",
        },
        {
            0x00004089U,
            0x00004089U,
            "a read-only legacy I/O BAR answers with its own base",
        },
    };
    size_t case_index;

    for (case_index = 0U; case_index < sizeof(cases) / sizeof(cases[0]);
         ++case_index) {
        struct multi_fixture fixture;
        struct pci_config_io io;
        struct pci_location location;
        struct pci_device_header header;
        struct pci_device_header found[1];
        struct pci_scan_failure failure;
        size_t found_count = 0U;
        enum pci_status status;

        multi_reset(&fixture);
        multi_install_ahci(&fixture, 2U);
        multi_set_bar(&fixture, 2U, 0U, cases[case_index].raw,
                      cases[case_index].probe);
        io = multi_io(&fixture);
        location.bus = TEST_BUS;
        location.device = 2U;
        location.function = 0U;

        status = pci_device_read(&io, &location, &header);
        CHECK(status == PCI_OK && header.present &&
                  !header.bars[0].present &&
                  header.bars[0].decode_status == PCI_INVALID_BAR &&
                  header.bars[5].present &&
                  header.bars[5].base == UINT64_C(0xFE000000) &&
                  header.bars[5].length == UINT64_C(0x1000),
              cases[case_index].message);

        /*
         * The strict entry point still asks for every BAR the device
         * implements, so the unexplained BAR stays a named refusal there and
         * the old callers' contract does not change quietly.
         */
        found_count = 0U;
        status = pci_scan_class_ex(
            &io, PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA,
            PCI_PROGIF_AHCI, found, 1U, &found_count, &failure);
        CHECK(status == PCI_INVALID_BAR && failure.bar_index == 0U &&
                  failure.vendor_id == 0x8086U &&
                  failure.device_id == 0x1E03U,
              "a caller that requires every BAR still gets the named refusal");
    }

    /*
     * A 64-bit BAR whose low half explains no size keeps its pair bit: the
     * upper half is part of the same register pair and is not probed as a BAR
     * of its own, so it is not reported as a second failure that never
     * happened.
     */
    {
        struct multi_fixture fixture;
        struct pci_config_io io;
        struct pci_location location;
        struct pci_device_header header;

        multi_reset(&fixture);
        multi_install_ahci(&fixture, 2U);
        multi_set_bar(&fixture, 2U, 0U, 0xFE000004U, 0x00000000U);
        io = multi_io(&fixture);
        location.bus = TEST_BUS;
        location.device = 2U;
        location.function = 0U;

        CHECK(pci_device_read(&io, &location, &header) == PCI_OK &&
                  !header.bars[0].present && header.bars[0].is_64bit &&
                  header.bars[0].decode_status == PCI_INVALID_BAR &&
                  !header.bars[1].present &&
                  header.bars[1].decode_status == PCI_OK,
              "the upper half of an unusable 64-bit BAR is not probed alone");
    }
}

/*
 * ITEM-543: the scan that names the BARs it needs.
 *
 * The storage probe is brought up through the AHCI ABAR at BAR5 and the NVMe
 * BAR at BAR0, so those are the BARs the scan must be able to decode.  A
 * device whose needed BAR decoded is a candidate even when another BAR of it
 * explains no size; a needed BAR that did not decode is the named refusal of
 * that device, with the BAR index and the decode status the operator page
 * prints; and a structurally malformed register is never tolerated, required
 * or not.
 */
static void test_required_bar_reaches_the_scan_and_names_its_own_failure(void)
{
    struct multi_fixture fixture;
    struct pci_config_io io;
    struct pci_device_header found[1];
    struct pci_scan_failure failure;
    size_t found_count = 0U;
    enum pci_status status;

    /* The L875 shape: BAR0 explains no size, the required ABAR decodes. */
    multi_reset(&fixture);
    multi_install_ahci(&fixture, 2U);
    multi_set_bar(&fixture, 2U, 0U, 0x00004089U, 0x0000FFF9U);
    io = multi_io(&fixture);
    status = pci_scan_class_required(
        &io, PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA, PCI_PROGIF_AHCI,
        PCI_BAR_REQUIRED(5U), found, 1U, &found_count, &failure);
    CHECK(status == PCI_OK && found_count == 1U &&
              found[0].location.device == 2U &&
              found[0].bars[5].present &&
              found[0].bars[5].base == UINT64_C(0xFE000000) &&
              !found[0].bars[0].present &&
              found[0].bars[0].decode_status == PCI_INVALID_BAR,
          "the L875 device reaches a scan that asks for its ABAR");

    /* The required BAR itself is refused by name, with its index. */
    multi_reset(&fixture);
    multi_install_ahci(&fixture, 2U);
    multi_set_bar(&fixture, 2U, 5U, 0xFE000000U, 0xFFFFF100U);
    io = multi_io(&fixture);
    found_count = 99U;
    status = pci_scan_class_required(
        &io, PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA, PCI_PROGIF_AHCI,
        PCI_BAR_REQUIRED(5U), found, 1U, &found_count, &failure);
    CHECK(status == PCI_INVALID_BAR && failure.status == PCI_INVALID_BAR &&
              failure.bar_index == 5U &&
              failure.vendor_id == 0x8086U &&
              failure.device_id == 0x1E03U,
          "a needed BAR that explains no size is a named refusal of that device");

    /*
     * A reserved memory type is a malformed register, not an unexplained
     * size: it stays a hard refusal even though this scan only needs BAR5.
     */
    multi_reset(&fixture);
    multi_install_ahci(&fixture, 2U);
    multi_set_bar(&fixture, 2U, 0U, 0x00000002U, UINT32_MAX);
    io = multi_io(&fixture);
    found_count = 99U;
    status = pci_scan_class_required(
        &io, PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA, PCI_PROGIF_AHCI,
        PCI_BAR_REQUIRED(5U), found, 1U, &found_count, &failure);
    CHECK(status == PCI_INVALID_BAR && failure.bar_index == 0U,
          "a malformed register is still a hard refusal on any BAR");
}

static void test_dma_validation(void)
{
    struct pci_dma_buffer buffer;
    struct pci_dma_buffer slice;
    enum pci_dma_status status;

    memset(&buffer, 0xA5, sizeof(buffer));
    memset(&slice, 0xA5, sizeof(slice));
    CHECK(pci_dma_buffer_publish(NULL, UINT64_C(0x1000), 512U,
                                 PCI_DMA_DIRECTION_DEVICE_WRITE) ==
              PCI_DMA_NULL,
          "DMA publish rejects null buffer");
    CHECK(pci_dma_buffer_publish(&buffer, 0U, 512U,
                                 PCI_DMA_DIRECTION_DEVICE_WRITE) ==
              PCI_DMA_INVALID_ADDRESS,
          "DMA publish rejects zero physical address");
    CHECK(pci_dma_buffer_publish(&buffer, 0x1000U, 0U,
                                 PCI_DMA_DIRECTION_DEVICE_WRITE) ==
              PCI_DMA_INVALID_LENGTH,
          "DMA publish rejects zero length");
    CHECK(pci_dma_buffer_publish(&buffer, 0x1000U, 4096U,
                                 (enum pci_dma_direction)99U) ==
              PCI_DMA_INVALID_DIRECTION,
          "DMA publish rejects invalid direction");
    CHECK(pci_dma_buffer_publish(&buffer, 0x1000U,
                                 PCI_DMA_LENGTH_MAX + 1U,
                                 PCI_DMA_DIRECTION_DEVICE_WRITE) ==
              PCI_DMA_INVALID_LENGTH,
          "DMA publish rejects over-limit length");
    CHECK(pci_dma_buffer_publish(&buffer, 0x1800U, 4096U,
                                 PCI_DMA_DIRECTION_DEVICE_WRITE) ==
              PCI_DMA_UNALIGNED,
          "DMA publish rejects unaligned physical address");
    CHECK(pci_dma_buffer_publish(
              &buffer, UINT64_C(0xFFFFFFFFFFFFF000),
              PCI_DMA_LENGTH_MAX,
              PCI_DMA_DIRECTION_DEVICE_READ) == PCI_DMA_OVERFLOW,
          "DMA publish rejects overflowing physical range");

    status = pci_dma_buffer_publish(
        &buffer, UINT64_C(0x80000000), 4096U,
        PCI_DMA_DIRECTION_BIDIRECTIONAL);
    CHECK(status == PCI_DMA_OK && buffer.published &&
              buffer.physical_base == UINT64_C(0x80000000) &&
              buffer.physical_end == UINT64_C(0x80001000) &&
              buffer.length == 4096U,
          "DMA publish creates exact validated buffer");

    status = pci_dma_buffer_slice(
        &buffer, 0U, 4096U, &slice);
    CHECK(status == PCI_DMA_OK &&
              slice.physical_base == UINT64_C(0x80000000) &&
              slice.physical_end == UINT64_C(0x80001000),
          "page-aligned DMA slice is exact");
    status = pci_dma_buffer_slice(
        &buffer, 4096U, 4096U, &slice);
    CHECK(status == PCI_DMA_OUT_OF_RANGE,
          "out-of-range DMA slice is rejected");
    status = pci_dma_buffer_slice(
        &buffer, 512U, 4096U, &slice);
    CHECK(status == PCI_DMA_UNALIGNED,
          "unaligned DMA slice is rejected");
}

int main(void)
{
    test_pci_status_and_location();
    test_nvme_class_scan_and_bar_decode();
    test_invalid_bar_and_io_failure();
    test_bridge_header_skips_bar_decode();
    test_io_bar_small_ranges_decode();
    test_scan_survives_unrelated_device_resource();
    test_scan_probes_only_matched_devices();
    test_identity_read_skips_resource_probe();
    test_scan_failure_names_the_device();
    test_unexplained_bar_size_leaves_the_device_usable();
    test_required_bar_reaches_the_scan_and_names_its_own_failure();
    test_dma_validation();

    if (failures != 0) {
        fprintf(stderr, "PCI host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("PCI host tests: ALL OK");
    return 0;
}

#endif
