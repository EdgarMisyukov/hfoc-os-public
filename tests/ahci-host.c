#include "ahci.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_AHCI_HOST_TEST

#define TEST_BUS 0U
#define TEST_DEVICE 31U
#define TEST_FUNCTION 2U
#define TEST_VENDOR 0x8086U
#define TEST_DEVICE_ID 0x2822U
#define TEST_MMIO_BASE UINT64_C(0xFE400000)
#define TEST_MMIO_LENGTH UINT64_C(0x1000)
#define TEST_CAPABILITIES UINT32_C(0x001F0101)
#define TEST_GHC UINT32_C(0x80000000)
#define TEST_IS 0U
#define TEST_PI 1U
#define TEST_VERSION UINT32_C(0x00010100)
#define TEST_PORT_CMD UINT32_C(0x00000001)
#define TEST_PORT_SIG UINT32_C(0x00000101)
#define TEST_PORT_SSTS UINT32_C(0x00000113)
#define TEST_COMMAND_LIST_PHYS UINT64_C(0x1000)
#define TEST_FIS_PHYS UINT64_C(0x2000)
#define TEST_COMMAND_TABLE_PHYS UINT64_C(0x3000)
#define TEST_DATA_PHYS UINT64_C(0x4000)
#define TEST_DMA_CAPACITY UINT64_C(0x6000)
#define TEST_DISK_BLOCK_COUNT 64U
#define TEST_DISK_CAPACITY (TEST_DISK_BLOCK_COUNT * 512U)
#define TEST_IDENTIFY_SECTOR_COUNT UINT64_C(64)
#define MMIO_FIXTURE_WORD_COUNT \
    (AHCI_PORT_BASE_OFFSET / 4U + \
     AHCI_MAX_PORT_COUNT * AHCI_PORT_REGISTER_SIZE / 4U)

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

struct mmio_fixture {
    uint32_t values[MMIO_FIXTURE_WORD_COUNT];
    bool fail_read;
    size_t read_calls;
    bool fail_memory_read;
    bool fail_memory_write;
    bool process_commands;
    bool mark_completion_error;
    bool never_clear_ci;
    uint64_t command_list_phys;
    unsigned char memory[(size_t)TEST_DMA_CAPACITY];
    unsigned char disk[TEST_DISK_CAPACITY];
    size_t commands_processed;
    size_t flush_commands;
    bool last_write;
    uint64_t last_lba;
    uint32_t last_block_count;
};

static void test_store_le32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value & 0xFFU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xFFU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xFFU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xFFU);
}

static void test_store_le64(unsigned char *bytes, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static uint32_t test_load_le32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static uint64_t test_load_le64(const unsigned char *bytes)
{
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << (8U * index);
    }
    return value;
}

static void fixture_init(struct mmio_fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->values[0U] = TEST_CAPABILITIES;
    fixture->values[1U] = TEST_GHC;
    fixture->values[2U] = TEST_IS;
    fixture->values[3U] = TEST_PI;
    fixture->values[4U] = TEST_VERSION;
    fixture->values[AHCI_PORT_BASE_OFFSET / 4U +
                   AHCI_PORT_CMD_OFFSET / 4U] = TEST_PORT_CMD;
    fixture->values[AHCI_PORT_BASE_OFFSET / 4U +
                   AHCI_PORT_SIG_OFFSET / 4U] = TEST_PORT_SIG;
    fixture->values[AHCI_PORT_BASE_OFFSET / 4U +
                   AHCI_PORT_SSTS_OFFSET / 4U] = TEST_PORT_SSTS;
}

static bool fixture_read(
    void *context,
    uint64_t offset,
    uint32_t *value)
{
    struct mmio_fixture *fixture = (struct mmio_fixture *)context;
    size_t index = (size_t)offset / 4U;

    ++fixture->read_calls;
    if (fixture->fail_read || (offset % 4U) != 0U ||
        index >= sizeof(fixture->values) / sizeof(fixture->values[0])) {
        return false;
    }
    *value = fixture->values[index];
    return true;
}

static bool fixture_write(
    void *context,
    uint64_t offset,
    uint32_t value)
{
    struct mmio_fixture *fixture = (struct mmio_fixture *)context;
    size_t index = (size_t)offset / 4U;

    (void)value;
    if ((offset % 4U) != 0U ||
        index >= sizeof(fixture->values) / sizeof(fixture->values[0])) {
        return false;
    }
    if (fixture->process_commands &&
        offset == AHCI_PORT_BASE_OFFSET +
                  AHCI_PORT_CI_OFFSET &&
        value != 0U) {
        unsigned char header[AHCI_COMMAND_LIST_ENTRY_SIZE];
        unsigned char table[
            AHCI_COMMAND_TABLE_SIZE + AHCI_PRD_ENTRY_SIZE];
        unsigned char prd[AHCI_PRD_ENTRY_SIZE];
        uint32_t opts;
        uint32_t prdtl;
        uint64_t table_address;
        uint64_t data_address = 0U;
        uint8_t command;
        uint64_t lba;
        uint32_t block_count;
        size_t byte_count = 0U;

        fixture->values[index] = value;
        ++fixture->commands_processed;
        if (fixture->command_list_phys + sizeof(header) <=
                TEST_DMA_CAPACITY) {
            memcpy(
                header,
                fixture->memory + (size_t)fixture->command_list_phys,
                sizeof(header));
        }
        opts = test_load_le32(header);
        prdtl = opts >> 16U;
        table_address = test_load_le64(header + 8U);
        if (table_address + sizeof(table) <= TEST_DMA_CAPACITY) {
            memcpy(
                table,
                fixture->memory + (size_t)table_address,
                sizeof(table));
        }
        command = table[2U];
        fixture->last_write = (opts & (UINT32_C(1) << 6U)) != 0U;
        lba = (uint64_t)table[4U] |
              ((uint64_t)table[5U] << 8U) |
              ((uint64_t)table[6U] << 16U) |
              ((uint64_t)table[8U] << 24U) |
              ((uint64_t)table[9U] << 32U) |
              ((uint64_t)table[10U] << 40U);
        block_count = (uint32_t)table[12U] |
                      ((uint32_t)table[13U] << 8U);
        fixture->last_lba = lba;
        fixture->last_block_count = block_count;
        if (prdtl != 0U &&
            table_address + AHCI_COMMAND_TABLE_SIZE +
                sizeof(prd) <= TEST_DMA_CAPACITY) {
            memcpy(
                prd,
                fixture->memory +
                    (size_t)(table_address + AHCI_COMMAND_TABLE_SIZE),
                sizeof(prd));
            data_address = test_load_le64(prd);
            byte_count =
                (size_t)((test_load_le32(prd + 12U) &
                          UINT32_C(0x003FFFFF)) + 1U);
        }

        if (command == AHCI_ATA_IDENTIFY_DEVICE) {
            if (data_address != 0U &&
                data_address + AHCI_ATA_IDENTIFY_SIZE <=
                    TEST_DMA_CAPACITY) {
                unsigned char *identify =
                    fixture->memory + (size_t)data_address;

                memset(identify, 0, AHCI_ATA_IDENTIFY_SIZE);
                test_store_le64(
                    identify + 200U, TEST_IDENTIFY_SECTOR_COUNT);
            }
        } else if (command == AHCI_ATA_READ_DMA_EXT ||
                   command == AHCI_ATA_WRITE_DMA_EXT) {
            uint64_t disk_offset =
                lba * 512U +
                ((uint64_t)block_count * 512U);

            if (disk_offset <= TEST_DISK_CAPACITY &&
                data_address + byte_count <= TEST_DMA_CAPACITY &&
                byte_count == (size_t)block_count * 512U) {
                unsigned char *data =
                    fixture->memory + (size_t)data_address;

                if (fixture->last_write) {
                    memcpy(
                        fixture->disk + (size_t)(lba * 512U),
                        data, byte_count);
                } else {
                    memcpy(
                        data,
                        fixture->disk + (size_t)(lba * 512U),
                        byte_count);
                }
            }
        } else if (command == AHCI_ATA_FLUSH_CACHE_EXT) {
            ++fixture->flush_commands;
        }

        if (fixture->mark_completion_error) {
            fixture->values[
                AHCI_PORT_BASE_OFFSET / 4U +
                AHCI_PORT_TFD_OFFSET / 4U] =
                AHCI_TFD_ERR_BIT;
        } else {
            fixture->values[
                AHCI_PORT_BASE_OFFSET / 4U +
                AHCI_PORT_TFD_OFFSET / 4U] = 0U;
        }
        if (fixture->never_clear_ci) {
            fixture->values[index] = value;
        } else {
            fixture->values[index] =
                value & ~AHCI_COMMAND_SLOT_MASK;
        }
        return true;
    }
    fixture->values[index] = value;
    return true;
}

static bool fixture_memory_read(
    void *context,
    uint64_t address,
    void *buffer,
    size_t length)
{
    struct mmio_fixture *fixture =
        (struct mmio_fixture *)context;

    if (fixture->fail_memory_read ||
        address > TEST_DMA_CAPACITY ||
        (uint64_t)length > TEST_DMA_CAPACITY - address) {
        return false;
    }
    memcpy(buffer, fixture->memory + (size_t)address, length);
    return true;
}

static bool fixture_memory_write(
    void *context,
    uint64_t address,
    const void *buffer,
    size_t length)
{
    struct mmio_fixture *fixture =
        (struct mmio_fixture *)context;

    if (fixture->fail_memory_write ||
        address > TEST_DMA_CAPACITY ||
        (uint64_t)length > TEST_DMA_CAPACITY - address) {
        return false;
    }
    memcpy(fixture->memory + (size_t)address, buffer, length);
    return true;
}

static struct ahci_dma_io fixture_dma_io(
    struct mmio_fixture *fixture)
{
    struct ahci_dma_io io;

    memset(&io, 0, sizeof(io));
    io.context = fixture;
    io.read = fixture_memory_read;
    io.write = fixture_memory_write;
    return io;
}

static struct ahci_mmio_io fixture_io(struct mmio_fixture *fixture)
{
    struct ahci_mmio_io io;

    memset(&io, 0, sizeof(io));
    io.context = fixture;
    io.read32 = fixture_read;
    io.write32 = fixture_write;
    return io;
}

static struct pci_device_header ahci_header(bool valid_bar)
{
    struct pci_device_header header;

    memset(&header, 0, sizeof(header));
    header.present = true;
    header.location.bus = TEST_BUS;
    header.location.device = TEST_DEVICE;
    header.location.function = TEST_FUNCTION;
    header.vendor_id = TEST_VENDOR;
    header.device_id = TEST_DEVICE_ID;
    header.base_class = PCI_CLASS_MASS_STORAGE;
    header.subclass = PCI_SUBCLASS_SATA;
    header.prog_if = PCI_PROGIF_AHCI;
    if (valid_bar) {
        header.bars[AHCI_MEMORY_BAR_INDEX].present = true;
        header.bars[AHCI_MEMORY_BAR_INDEX].base = TEST_MMIO_BASE;
        header.bars[AHCI_MEMORY_BAR_INDEX].length = TEST_MMIO_LENGTH;
    }
    return header;
}

static void test_binding(void)
{
    struct pci_device_header header;
    struct ahci_controller controller;

    memset(&controller, 0xA5, sizeof(controller));
    header = ahci_header(true);
    CHECK(ahci_controller_bind(&header, &controller) == AHCI_OK &&
              controller.present &&
              controller.location.bus == TEST_BUS &&
              controller.location.device == TEST_DEVICE &&
              controller.vendor_id == TEST_VENDOR &&
              controller.mmio_base == TEST_MMIO_BASE,
          "SATA/AHCI PCI header with memory BAR binds exact controller");

    header = ahci_header(true);
    header.base_class = 0x02U;
    CHECK(ahci_controller_bind(&header, &controller) ==
              AHCI_UNSUPPORTED_DEVICE,
          "non-storage PCI device is rejected");

    header = ahci_header(true);
    header.subclass = PCI_SUBCLASS_NVME;
    header.prog_if = PCI_PROGIF_NVME;
    CHECK(ahci_controller_bind(&header, &controller) ==
              AHCI_UNSUPPORTED_DEVICE,
          "NVMe-compatible PCI class is not mistaken for SATA/AHCI");

    header = ahci_header(true);
    header.bars[AHCI_MEMORY_BAR_INDEX].present = false;
    CHECK(ahci_controller_bind(&header, &controller) ==
              AHCI_MISSING_MEMORY_BAR,
          "SATA/AHCI device without ABAR is rejected");

    header = ahci_header(true);
    header.bars[AHCI_MEMORY_BAR_INDEX].is_64bit = true;
    CHECK(ahci_controller_bind(&header, &controller) ==
              AHCI_MISSING_MEMORY_BAR,
          "ABAR must be a 32-bit memory BAR");

    header = ahci_header(true);
    header.bars[AHCI_MEMORY_BAR_INDEX].base = 0U;
    CHECK(ahci_controller_bind(&header, &controller) == AHCI_BAD_BAR,
          "zero ABAR base is rejected");

    header = ahci_header(true);
    header.bars[AHCI_MEMORY_BAR_INDEX].length = 0x40U;
    CHECK(ahci_controller_bind(&header, &controller) == AHCI_BAD_BAR,
          "short ABAR window is rejected");

    CHECK(ahci_controller_bind(NULL, &controller) == AHCI_NULL,
          "AHCI bind rejects null PCI header");
    CHECK(ahci_controller_bind(&header, NULL) == AHCI_NULL,
          "AHCI bind rejects null controller");
}

static void test_discovery(void)
{
    struct mmio_fixture fixture;
    struct ahci_mmio_io io;
    struct pci_device_header header;
    struct ahci_controller controller;
    enum ahci_status status;

    fixture_init(&fixture);
    io = fixture_io(&fixture);
    header = ahci_header(true);
    CHECK(ahci_controller_bind(&header, &controller) == AHCI_OK,
          "discovery fixture binds valid controller");
    status = ahci_controller_discover(&io, &controller);
    CHECK(status == AHCI_OK &&
              controller.capabilities == TEST_CAPABILITIES &&
              controller.global_host_control == TEST_GHC &&
              controller.interrupt_status == TEST_IS &&
              controller.ports_implemented == TEST_PI &&
              controller.version == TEST_VERSION,
          "AHCI discovery publishes CAP, GHC, IS, PI and version");

    memset(&controller, 0, sizeof(controller));
    CHECK(ahci_controller_discover(&io, &controller) == AHCI_BAD_STATE,
          "AHCI discovery rejects unbound controller");
    CHECK(ahci_controller_discover(NULL, &controller) == AHCI_NULL,
          "AHCI discovery rejects null MMIO");

    fixture_init(&fixture);
    fixture.fail_read = true;
    io = fixture_io(&fixture);
    header = ahci_header(true);
    CHECK(ahci_controller_bind(&header, &controller) == AHCI_OK &&
              ahci_controller_discover(&io, &controller) == AHCI_IO_ERROR,
          "AHCI discovery maps MMIO read failure to stable I/O error");

    fixture_init(&fixture);
    fixture.values[4U] = UINT32_C(0x000000FF);
    io = fixture_io(&fixture);
    header = ahci_header(true);
    CHECK(ahci_controller_bind(&header, &controller) == AHCI_OK &&
              ahci_controller_discover(&io, &controller) ==
                  AHCI_UNSUPPORTED_VERSION,
          "pre-1.0 AHCI version is rejected");

    fixture_init(&fixture);
    fixture.values[3U] = 0U;
    io = fixture_io(&fixture);
    CHECK(ahci_controller_bind(&header, &controller) == AHCI_OK &&
              ahci_controller_discover(&io, &controller) == AHCI_BAD_STATE,
          "AHCI controller with no implemented ports fails closed");

    fixture_init(&fixture);
    fixture.values[0U] = UINT32_C(0x001F0000);
    fixture.values[3U] = UINT32_C(0x00000002);
    io = fixture_io(&fixture);
    header = ahci_header(true);
    CHECK(ahci_controller_bind(&header, &controller) == AHCI_OK &&
              ahci_controller_discover(&io, &controller) == AHCI_BAD_STATE,
          "AHCI discovery rejects port bits beyond implemented port count");

    CHECK(strcmp(ahci_status_string(AHCI_OK), "AHCI_OK") == 0 &&
              strcmp(ahci_status_string(AHCI_UNSUPPORTED_DEVICE),
                     "AHCI_UNSUPPORTED_DEVICE") == 0,
          "AHCI status strings are exact");
}

static void test_controller_activation(void)
{
    struct mmio_fixture fixture;
    struct ahci_mmio_io io;
    struct pci_device_header header;
    struct ahci_controller controller;
    enum ahci_status status;

    fixture_init(&fixture);
    fixture.values[1U] = 0U;
    io = fixture_io(&fixture);
    header = ahci_header(true);
    CHECK(ahci_controller_bind(&header, &controller) == AHCI_OK &&
              ahci_controller_discover(&io, &controller) == AHCI_OK &&
              !controller.ahci_enabled,
          "AHCI controller starts in non-AHCI state");
    status = ahci_controller_activate(&io, &controller);
    CHECK(status == AHCI_OK &&
              controller.ahci_enabled &&
              (controller.global_host_control &
               (UINT32_C(1) << AHCI_GHC_AE_BIT)) != 0U,
          "AHCI controller activation enables HBA AHCI mode");

    fixture_init(&fixture);
    fixture.values[1U] = 0U;
    io = fixture_io(&fixture);
    header = ahci_header(true);
    memset(&controller, 0, sizeof(controller));
    CHECK(ahci_controller_activate(&io, &controller) == AHCI_BAD_STATE,
          "AHCI activation rejects unbound controller");
    CHECK(ahci_controller_activate(NULL, &controller) == AHCI_NULL,
          "AHCI activation rejects null MMIO");
    CHECK(ahci_controller_activate(&io, NULL) == AHCI_NULL,
          "AHCI activation rejects null controller");

    fixture_init(&fixture);
    fixture.values[1U] = 0U;
    fixture.fail_read = true;
    io = fixture_io(&fixture);
    header = ahci_header(true);
    CHECK(ahci_controller_bind(&header, &controller) == AHCI_OK &&
              ahci_controller_activate(&io, &controller) == AHCI_IO_ERROR,
          "AHCI activation maps MMIO failure to I/O error");
}

static void test_port_scan(void)
{
    struct mmio_fixture fixture;
    struct ahci_mmio_io io;
    struct pci_device_header header;
    struct ahci_controller controller;
    struct ahci_port_map map;
    enum ahci_status status;

    fixture_init(&fixture);
    fixture.values[1U] = 0U;
    io = fixture_io(&fixture);
    header = ahci_header(true);
    CHECK(ahci_controller_bind(&header, &controller) == AHCI_OK &&
              ahci_controller_discover(&io, &controller) == AHCI_OK &&
              ahci_controller_activate(&io, &controller) == AHCI_OK,
          "port-scan fixture activates AHCI controller");
    status = ahci_controller_discover_ports(&io, &controller, &map);
    CHECK(status == AHCI_OK &&
              map.complete &&
              map.port_count == 1U &&
              map.ports[0U].present &&
              map.ports[0U].device_attached &&
              map.ports[0U].supported &&
              map.ports[0U].index == 0U &&
              map.ports[0U].signature == TEST_PORT_SIG &&
              map.ports[0U].status == TEST_PORT_SSTS &&
              map.ports[0U].command == TEST_PORT_CMD,
          "AHCI port scan publishes implemented ATA disk state");

    fixture_init(&fixture);
    io = fixture_io(&fixture);
    memset(&controller, 0, sizeof(controller));
    CHECK(ahci_controller_discover_ports(&io, &controller, &map) ==
              AHCI_BAD_STATE,
          "AHCI port scan rejects unbound controller");
    CHECK(ahci_controller_discover_ports(NULL, &controller, &map) ==
              AHCI_NULL,
          "AHCI port scan rejects null MMIO");
    CHECK(ahci_controller_discover_ports(&io, &controller, NULL) ==
              AHCI_NULL,
          "AHCI port scan rejects null map");

    fixture_init(&fixture);
    fixture.values[1U] = 0U;
    fixture.values[3U] = 0U;
    io = fixture_io(&fixture);
    header = ahci_header(true);
    CHECK(ahci_controller_bind(&header, &controller) == AHCI_OK &&
              ahci_controller_discover(&io, &controller) == AHCI_BAD_STATE,
          "AHCI controller with no implemented ports fails closed");

    fixture_init(&fixture);
    fixture.values[1U] = 0U;
    fixture.values[AHCI_PORT_BASE_OFFSET / 4U +
                   AHCI_PORT_SIG_OFFSET / 4U] = UINT32_MAX;
    fixture.values[AHCI_PORT_BASE_OFFSET / 4U +
                   AHCI_PORT_SSTS_OFFSET / 4U] = 0U;
    io = fixture_io(&fixture);
    header = ahci_header(true);
    CHECK(ahci_controller_bind(&header, &controller) == AHCI_OK &&
              ahci_controller_discover(&io, &controller) == AHCI_OK &&
              ahci_controller_activate(&io, &controller) == AHCI_OK &&
              ahci_controller_discover_ports(&io, &controller, &map) ==
                  AHCI_OK &&
              map.port_count == 1U &&
              !map.ports[0U].device_attached &&
              !map.ports[0U].supported,
          "AHCI port scan reports empty implemented port");

    fixture_init(&fixture);
    fixture.values[1U] = 0U;
    fixture.values[AHCI_PORT_BASE_OFFSET / 4U +
                   AHCI_PORT_SIG_OFFSET / 4U] =
        UINT32_C(0xEB140101);
    io = fixture_io(&fixture);
    header = ahci_header(true);
    CHECK(ahci_controller_bind(&header, &controller) == AHCI_OK &&
              ahci_controller_discover(&io, &controller) == AHCI_OK &&
              ahci_controller_activate(&io, &controller) == AHCI_OK &&
              ahci_controller_discover_ports(&io, &controller, &map) ==
                  AHCI_OK &&
              map.ports[0U].device_attached &&
              !map.ports[0U].supported,
          "AHCI port scan marks non-ATA device unsupported");

    fixture_init(&fixture);
    fixture.values[1U] = 0U;
    fixture.fail_read = true;
    io = fixture_io(&fixture);
    header = ahci_header(true);
    CHECK(ahci_controller_bind(&header, &controller) == AHCI_OK &&
              ahci_controller_discover(&io, &controller) == AHCI_IO_ERROR,
          "AHCI port fixture MMIO failure remains closed");

    CHECK(strcmp(ahci_status_string(AHCI_CAPACITY), "AHCI_CAPACITY") == 0,
          "AHCI capacity status string is exact");
}

static void test_port_dma_start(void)
{
    struct mmio_fixture fixture;
    struct ahci_mmio_io io;
    struct pci_device_header header;
    struct ahci_controller controller;
    struct ahci_port_map map;
    uint32_t start_mask;
    size_t command_index;
    enum ahci_status status;

    fixture_init(&fixture);
    fixture.values[1U] = 0U;
    fixture.values[AHCI_PORT_BASE_OFFSET / 4U +
                   AHCI_PORT_CMD_OFFSET / 4U] = 0U;
    io = fixture_io(&fixture);
    header = ahci_header(true);
    CHECK(ahci_controller_bind(&header, &controller) == AHCI_OK &&
              ahci_controller_discover(&io, &controller) == AHCI_OK &&
              ahci_controller_activate(&io, &controller) == AHCI_OK &&
              ahci_controller_discover_ports(&io, &controller, &map) ==
                  AHCI_OK,
          "DMA-start fixture activates and scans AHCI port");

    status = ahci_port_dma_start(
        &io, &controller, &map, 0U,
        TEST_COMMAND_LIST_PHYS, TEST_FIS_PHYS);
    command_index = AHCI_PORT_BASE_OFFSET / 4U +
                    AHCI_PORT_CMD_OFFSET / 4U;
    start_mask =
        (UINT32_C(1) << AHCI_PORT_CMD_ST_BIT) |
        (UINT32_C(1) << AHCI_PORT_CMD_FRE_BIT);
    CHECK(status == AHCI_OK &&
              fixture.values[AHCI_PORT_BASE_OFFSET / 4U +
                             AHCI_PORT_CLB_OFFSET / 4U] ==
                  (uint32_t)TEST_COMMAND_LIST_PHYS &&
              fixture.values[AHCI_PORT_BASE_OFFSET / 4U +
                             AHCI_PORT_CLBU_OFFSET / 4U] ==
                  (uint32_t)(TEST_COMMAND_LIST_PHYS >> 32U) &&
              fixture.values[AHCI_PORT_BASE_OFFSET / 4U +
                             AHCI_PORT_FB_OFFSET / 4U] ==
                  (uint32_t)TEST_FIS_PHYS &&
              fixture.values[AHCI_PORT_BASE_OFFSET / 4U +
                             AHCI_PORT_FBU_OFFSET / 4U] ==
                  (uint32_t)(TEST_FIS_PHYS >> 32U) &&
              (fixture.values[command_index] & start_mask) == start_mask,
          "AHCI port DMA start publishes command list and FIS bases");

    fixture_init(&fixture);
    fixture.values[1U] = 0U;
    fixture.values[AHCI_PORT_BASE_OFFSET / 4U +
                   AHCI_PORT_CMD_OFFSET / 4U] = 0U;
    io = fixture_io(&fixture);
    header = ahci_header(true);
    CHECK(ahci_controller_bind(&header, &controller) == AHCI_OK &&
              ahci_controller_discover(&io, &controller) == AHCI_OK &&
              ahci_controller_activate(&io, &controller) == AHCI_OK &&
              ahci_controller_discover_ports(&io, &controller, &map) ==
                  AHCI_OK &&
              ahci_port_dma_start(
                  &io, &controller, &map, 1U,
                  TEST_COMMAND_LIST_PHYS, TEST_FIS_PHYS) == AHCI_BAD_STATE,
          "AHCI DMA start rejects port absent from scanned map");

    fixture_init(&fixture);
    io = fixture_io(&fixture);
    memset(&controller, 0, sizeof(controller));
    CHECK(ahci_port_dma_start(
              &io, &controller, &map, 0U,
              TEST_COMMAND_LIST_PHYS, TEST_FIS_PHYS) == AHCI_BAD_STATE,
          "AHCI DMA start rejects unbound controller");
    CHECK(ahci_port_dma_start(
              NULL, &controller, &map, 0U,
              TEST_COMMAND_LIST_PHYS, TEST_FIS_PHYS) == AHCI_NULL,
          "AHCI DMA start rejects null MMIO");
    CHECK(ahci_port_dma_start(
              &io, &controller, NULL, 0U,
              TEST_COMMAND_LIST_PHYS, TEST_FIS_PHYS) == AHCI_NULL,
          "AHCI DMA start rejects null port map");

    fixture_init(&fixture);
    fixture.values[1U] = 0U;
    io = fixture_io(&fixture);
    header = ahci_header(true);
    CHECK(ahci_controller_bind(&header, &controller) == AHCI_OK &&
              ahci_controller_discover(&io, &controller) == AHCI_OK &&
              ahci_controller_activate(&io, &controller) == AHCI_OK &&
              ahci_controller_discover_ports(&io, &controller, &map) ==
                  AHCI_OK &&
              ahci_port_dma_start(
                  &io, &controller, &map, 0U,
                  TEST_COMMAND_LIST_PHYS + 1U, TEST_FIS_PHYS) ==
                  AHCI_BAD_STATE,
          "AHCI DMA start rejects unaligned command list");

    fixture_init(&fixture);
    fixture.values[1U] = 0U;
    fixture.fail_read = true;
    io = fixture_io(&fixture);
    header = ahci_header(true);
    CHECK(ahci_controller_bind(&header, &controller) == AHCI_OK &&
              ahci_controller_discover(&io, &controller) == AHCI_IO_ERROR,
          "AHCI DMA-start fixture remains closed on MMIO failure");
}

static bool prepare_sata_fixture(
    struct mmio_fixture *fixture,
    struct ahci_mmio_io *io,
    struct ahci_controller *controller,
    struct ahci_port_map *map)
{
    struct pci_device_header header;
    enum ahci_status status;

    fixture_init(fixture);
    fixture->values[1U] = 0U;
    fixture->values[AHCI_PORT_BASE_OFFSET / 4U +
                   AHCI_PORT_CMD_OFFSET / 4U] = 0U;
    fixture->command_list_phys = TEST_COMMAND_LIST_PHYS;
    fixture->process_commands = true;

    *io = fixture_io(fixture);
    header = ahci_header(true);
    status = ahci_controller_bind(&header, controller);
    if (status != AHCI_OK) {
        return false;
    }
    status = ahci_controller_discover(io, controller);
    if (status != AHCI_OK) {
        return false;
    }
    status = ahci_controller_activate(io, controller);
    if (status != AHCI_OK) {
        return false;
    }
    status = ahci_controller_discover_ports(
        io, controller, map);
    if (status != AHCI_OK) {
        return false;
    }
    status = ahci_port_dma_start(
        io, controller, map, 0U,
        TEST_COMMAND_LIST_PHYS, TEST_FIS_PHYS);
    return status == AHCI_OK;
}

static void test_ata_command_path(void)
{
    struct mmio_fixture fixture;
    struct ahci_mmio_io io;
    struct ahci_dma_io dma;
    struct ahci_controller controller;
    struct ahci_port_map map;
    struct ahci_ata_identity identity;
    unsigned char header[AHCI_COMMAND_LIST_ENTRY_SIZE];
    unsigned char command_table[AHCI_COMMAND_TABLE_SIZE];
    size_t command_index;
    size_t index;

    memset(&controller, 0, sizeof(controller));
    memset(&map, 0, sizeof(map));
    if (!prepare_sata_fixture(
            &fixture, &io, &controller, &map)) {
        CHECK(false, "ATA command fixture cannot bind and start SATA port");
        return;
    }
    dma = fixture_dma_io(&fixture);

    CHECK(ahci_ata_identify(
              &io, &dma, &controller, &map, 0U,
              TEST_COMMAND_LIST_PHYS,
              TEST_COMMAND_TABLE_PHYS, TEST_DATA_PHYS,
              &identity) == AHCI_OK &&
              identity.valid &&
              identity.block_size == 512U &&
              identity.sector_count == TEST_IDENTIFY_SECTOR_COUNT &&
              fixture.commands_processed == 1U,
          "AHCI IDENTIFY command publishes parsed ATA identity");

    memcpy(header, fixture.memory + (size_t)TEST_COMMAND_LIST_PHYS,
           sizeof(header));
    memcpy(command_table,
           fixture.memory + (size_t)TEST_COMMAND_TABLE_PHYS,
           sizeof(command_table));
    CHECK((header[0U] & 0x1FU) == AHCI_H2D_FIS_CFL &&
              header[2U] == 1U &&
              command_table[0U] == 0x27U &&
              command_table[1U] == 0x80U &&
              command_table[2U] == AHCI_ATA_IDENTIFY_DEVICE &&
              command_table[7U] == AHCI_ATA_DEVICE_OBS,
          "AHCI IDENTIFY command list and H2D FIS are exact");

    memset(fixture.disk + 2U * 512U, 0x3CU, 2U * 512U);
    CHECK(ahci_ata_read_blocks(
              &io, &dma, &controller, &map, 0U,
              TEST_COMMAND_LIST_PHYS,
              TEST_COMMAND_TABLE_PHYS, TEST_DATA_PHYS,
              2U, 2U) == AHCI_OK &&
              fixture.commands_processed == 2U &&
              fixture.last_lba == 2U &&
              fixture.last_block_count == 2U &&
              !fixture.last_write,
          "AHCI READ DMA EXT command completes on attached ATA disk");
    CHECK(memcmp(fixture.memory + (size_t)TEST_DATA_PHYS,
                 fixture.disk + 2U * 512U,
                 2U * 512U) == 0,
          "AHCI READ DMA EXT publishes PRD data buffer");

    memset(fixture.memory + (size_t)TEST_DATA_PHYS, 0xA5U,
           2U * 512U);
    CHECK(ahci_ata_write_blocks(
              &io, &dma, &controller, &map, 0U,
              TEST_COMMAND_LIST_PHYS,
              TEST_COMMAND_TABLE_PHYS, TEST_DATA_PHYS,
              4U, 2U) == AHCI_OK &&
              fixture.commands_processed == 3U &&
              fixture.last_write,
          "AHCI WRITE DMA EXT command completes on attached ATA disk");
    CHECK(fixture.disk[4U * 512U] == 0xA5U &&
              fixture.disk[5U * 512U - 1U] == 0xA5U,
          "AHCI WRITE DMA EXT transfers PRD data to disk");

    CHECK(ahci_ata_flush_cache(
              &io, &dma, &controller, &map, 0U,
              TEST_COMMAND_LIST_PHYS,
              TEST_COMMAND_TABLE_PHYS) == AHCI_OK &&
              fixture.flush_commands == 1U,
          "AHCI FLUSH CACHE EXT command completes");

    CHECK(ahci_ata_read_blocks(
              &io, &dma, &controller, &map, 0U,
              TEST_COMMAND_LIST_PHYS,
              TEST_COMMAND_TABLE_PHYS, TEST_DATA_PHYS,
              0U, AHCI_MAX_SECTORS_PER_TRANSFER + 1U) ==
              AHCI_BAD_RANGE,
          "AHCI ATA command rejects oversized sector transfer");
    CHECK(ahci_ata_read_blocks(
              &io, &dma, &controller, &map, 0U,
              TEST_COMMAND_LIST_PHYS + 1U,
              TEST_COMMAND_TABLE_PHYS, TEST_DATA_PHYS,
              0U, 1U) == AHCI_BAD_ALIGNMENT,
          "AHCI ATA command rejects unaligned command list");

    memset(&identity, 0, sizeof(identity));
    memset(fixture.memory + (size_t)TEST_DATA_PHYS, 0,
           AHCI_ATA_IDENTIFY_SIZE);
    test_store_le64(
        fixture.memory + (size_t)TEST_DATA_PHYS + 200U,
        TEST_IDENTIFY_SECTOR_COUNT);
    CHECK(ahci_ata_identity_parse(
              fixture.memory + (size_t)TEST_DATA_PHYS,
              &identity) == AHCI_OK &&
              identity.sector_count == TEST_IDENTIFY_SECTOR_COUNT,
          "AHCI identify parser reads LBA48 capacity");
    memset(fixture.memory + (size_t)TEST_DATA_PHYS, 0,
           AHCI_ATA_IDENTIFY_SIZE);
    CHECK(ahci_ata_identity_parse(
              fixture.memory + (size_t)TEST_DATA_PHYS,
              &identity) == AHCI_BAD_IDENTIFY,
          "AHCI identify parser rejects zero-capacity data");
    CHECK(ahci_ata_identity_parse(NULL, &identity) == AHCI_NULL &&
              ahci_ata_identity_parse(
                  fixture.memory + (size_t)TEST_DATA_PHYS,
                  NULL) == AHCI_NULL,
          "AHCI identify parser rejects null arguments");
    for (index = 0U; index < AHCI_ATA_IDENTIFY_SIZE; ++index) {
        fixture.memory[(size_t)TEST_DATA_PHYS + index] =
            (unsigned char)index;
    }
    memset(fixture.memory + (size_t)TEST_DATA_PHYS + 200U,
           0, 8U);
    test_store_le32(
        fixture.memory + (size_t)TEST_DATA_PHYS + 120U,
        UINT32_C(0x123456));
    CHECK(ahci_ata_identity_parse(
              fixture.memory + (size_t)TEST_DATA_PHYS,
              &identity) == AHCI_OK &&
              identity.sector_count == UINT64_C(0x123456),
          "AHCI identify parser falls back to LBA28 capacity");

    CHECK(strcmp(ahci_status_string(AHCI_TIMEOUT), "AHCI_TIMEOUT") == 0 &&
              strcmp(ahci_status_string(AHCI_FAILED_COMPLETION),
                     "AHCI_FAILED_COMPLETION") == 0 &&
              strcmp(ahci_status_string(AHCI_BAD_RANGE),
                     "AHCI_BAD_RANGE") == 0 &&
              strcmp(ahci_status_string(AHCI_BAD_ALIGNMENT),
                     "AHCI_BAD_ALIGNMENT") == 0 &&
              strcmp(ahci_status_string(AHCI_BAD_IDENTIFY),
                     "AHCI_BAD_IDENTIFY") == 0,
          "new AHCI status strings are exact");

    command_index = fixture.commands_processed;
    fixture.fail_memory_write = true;
    CHECK(ahci_ata_identify(
              &io, &dma, &controller, &map, 0U,
              TEST_COMMAND_LIST_PHYS,
              TEST_COMMAND_TABLE_PHYS, TEST_DATA_PHYS,
              &identity) == AHCI_IO_ERROR,
          "AHCI command path maps DMA write failure to I/O error");
    fixture.fail_memory_write = false;
    fixture.commands_processed = command_index;
}

static void test_ata_failure_path(void)
{
    struct mmio_fixture fixture;
    struct ahci_mmio_io io;
    struct ahci_dma_io dma;
    struct ahci_controller controller;
    struct ahci_port_map map;
    struct ahci_ata_identity identity;

    memset(&controller, 0, sizeof(controller));
    memset(&map, 0, sizeof(map));
    if (!prepare_sata_fixture(
            &fixture, &io, &controller, &map)) {
        CHECK(false, "ATA failure fixture cannot bind and start SATA port");
        return;
    }
    dma = fixture_dma_io(&fixture);

    fixture.mark_completion_error = true;
    CHECK(ahci_ata_identify(
              &io, &dma, &controller, &map, 0U,
              TEST_COMMAND_LIST_PHYS,
              TEST_COMMAND_TABLE_PHYS, TEST_DATA_PHYS,
              &identity) == AHCI_FAILED_COMPLETION,
          "AHCI command path reports task-file completion error");

    fixture.mark_completion_error = false;
    fixture.never_clear_ci = true;
    CHECK(ahci_ata_read_blocks(
              &io, &dma, &controller, &map, 0U,
              TEST_COMMAND_LIST_PHYS,
              TEST_COMMAND_TABLE_PHYS, TEST_DATA_PHYS,
              0U, 1U) == AHCI_TIMEOUT,
          "AHCI command path times out when PxCI does not clear");

    fixture.never_clear_ci = false;
    fixture.fail_read = true;
    CHECK(ahci_ata_read_blocks(
              &io, &dma, &controller, &map, 0U,
              TEST_COMMAND_LIST_PHYS,
              TEST_COMMAND_TABLE_PHYS, TEST_DATA_PHYS,
              0U, 1U) == AHCI_IO_ERROR,
          "AHCI command path remains closed on MMIO read failure");

    fixture.fail_read = false;
    fixture.fail_memory_read = true;
    CHECK(ahci_ata_identify(
              &io, &dma, &controller, &map, 0U,
              TEST_COMMAND_LIST_PHYS,
              TEST_COMMAND_TABLE_PHYS, TEST_DATA_PHYS,
              &identity) == AHCI_IO_ERROR,
          "AHCI identify maps DMA read failure to I/O error");
}

static void test_block_device_adapter(void)
{
    struct mmio_fixture fixture;
    struct ahci_mmio_io io;
    struct ahci_dma_io dma;
    struct ahci_controller controller;
    struct ahci_port_map map;
    struct ahci_ata_identity identity;
    struct ahci_block_device backend;
    unsigned char write_buffer[2U * 512U];
    unsigned char read_buffer[2U * 512U];
    enum block_device_status block_status;

    memset(&controller, 0, sizeof(controller));
    memset(&map, 0, sizeof(map));
    if (!prepare_sata_fixture(
            &fixture, &io, &controller, &map)) {
        CHECK(false, "block-device fixture cannot bind and start SATA port");
        return;
    }
    dma = fixture_dma_io(&fixture);
    CHECK(ahci_ata_identify(
              &io, &dma, &controller, &map, 0U,
              TEST_COMMAND_LIST_PHYS,
              TEST_COMMAND_TABLE_PHYS, TEST_DATA_PHYS,
              &identity) == AHCI_OK,
          "block-device fixture obtains ATA identity");

    CHECK(ahci_block_device_bind(
              &io, &dma, &controller, &map, 0U,
              TEST_COMMAND_LIST_PHYS,
              TEST_COMMAND_TABLE_PHYS, TEST_DATA_PHYS,
              &identity, &backend) == AHCI_OK &&
              backend.device.block_size == 512U &&
              backend.device.block_count == TEST_IDENTIFY_SECTOR_COUNT &&
              backend.device.read_fn != NULL &&
              backend.device.write_fn != NULL &&
              backend.device.flush_fn != NULL,
          "AHCI block-device adapter binds parsed ATA identity");

    memset(write_buffer, 0x5AU, sizeof(write_buffer));
    block_status = block_device_write(
        &backend.device, 2U * 512U, write_buffer, sizeof(write_buffer));
    CHECK(block_status == BLOCK_DEVICE_OK &&
              fixture.disk[2U * 512U] == 0x5AU &&
              fixture.disk[3U * 512U + 1U] == 0x5AU,
          "AHCI block-device adapter writes through DMA command path");

    memset(read_buffer, 0, sizeof(read_buffer));
    block_status = block_device_read(
        &backend.device, 2U * 512U, read_buffer, sizeof(read_buffer));
    CHECK(block_status == BLOCK_DEVICE_OK &&
              memcmp(read_buffer, write_buffer,
                     sizeof(write_buffer)) == 0,
          "AHCI block-device adapter reads through DMA command path");

    block_status = block_device_flush(&backend.device);
    CHECK(block_status == BLOCK_DEVICE_OK &&
              fixture.flush_commands == 1U,
          "AHCI block-device adapter propagates device flush");

    block_status = block_device_write(
        &backend.device, 2U * 512U, write_buffer, 1U);
    CHECK(block_status == BLOCK_DEVICE_UNALIGNED,
          "AHCI block-device adapter rejects unaligned request");
    block_status = block_device_read(
        &backend.device, backend.device.block_count * 512U,
        read_buffer, sizeof(read_buffer));
    CHECK(block_status == BLOCK_DEVICE_OUT_OF_RANGE,
          "AHCI block-device adapter rejects out-of-range request");

    memset(&identity, 0, sizeof(identity));
    identity.valid = true;
    identity.block_size = 512U;
    identity.sector_count = BLOCK_DEVICE_BLOCK_COUNT_MAX + 1U;
    CHECK(ahci_block_device_bind(
              &io, &dma, &controller, &map, 0U,
              TEST_COMMAND_LIST_PHYS,
              TEST_COMMAND_TABLE_PHYS, TEST_DATA_PHYS,
              &identity, &backend) == AHCI_CAPACITY,
          "AHCI block-device bind rejects unsupported physical capacity");
}

int main(void)
{
    test_binding();
    test_discovery();
    test_controller_activation();
    test_port_scan();
    test_port_dma_start();
    test_ata_command_path();
    test_ata_failure_path();
    test_block_device_adapter();

    if (failures != 0) {
        fprintf(stderr, "AHCI host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("AHCI host tests: ALL OK");
    return 0;
}

#endif
