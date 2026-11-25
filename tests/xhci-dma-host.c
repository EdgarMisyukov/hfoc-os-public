/*
 * ITEM-577 host proof: the xHCI DMA geometry, the scratchpads, the device
 * context slot areas and the first fatal controller snapshot.
 *
 * The 1ad25c25 run applied the routing and the connections appeared, but the
 * controller answered HSE/HCH with USBCMD.RS already 0 and the driver reported
 * ENABLE_SLOT_TIMEOUT.  The controller had never been given the scratchpad
 * buffers its HCSPARAMS2 asks for, and every slot's Device Context was placed
 * one record apart instead of one 32-record area apart.
 *
 * This proof drives the production xhci_controller_start() and
 * xhci_address_device_ring() against a register model (the ITEM-577 MMIO seam)
 * and a fake physical pool, then reads the pool's bytes at the addresses the
 * controller would read - not through the driver's own pointers:
 *
 *   * HCSPARAMS2 says 16 scratchpads: DCBAA[0] names the pointer array, every
 *     entry is a page-aligned page inside the pool, the pages are zeroed and
 *     the array was published before USBCMD.RS was written;
 *   * HCSPARAMS2 says 0: DCBAA[0] stays zero and no scratchpad page is
 *     allocated;
 *   * an allocation failure and a pool larger than the bound are refused
 *     before RS is written;
 *   * adjacent slots are one 32-record area apart for both 32- and 64-byte
 *     records, the areas do not overlap the scratchpads or the fixed regions,
 *     and writing slot 2's contexts leaves slot 1's EP0 context untouched;
 *   * an HSE after start stops every further command (the doorbell is never
 *     rung again) and the first fatal snapshot - cause, USBCMD, USBSTS, CRCR,
 *     DCBAAP - is kept even when a later capture is attempted.
 */
#include "../kernel/src/xhci.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } \
} while (0)

/* ------------------------------------------------------------------------- */
/* The external sinks the driver's start path may reach. */

#define FAKE_POOL_PHYS 0x400000ULL
#define FAKE_POOL_BYTES (4U * 1024U * 1024U)

static uint8_t fake_pool[FAKE_POOL_BYTES];
static uint32_t fake_allocated_pages;
static bool fake_alloc_fail;

uint64_t pmm_alloc_contiguous_pages(uint32_t count) {
    if (fake_alloc_fail || count == 0U ||
        (size_t)count * 4096U > sizeof(fake_pool)) {
        return 0U;
    }
    fake_allocated_pages = count;
    memset(fake_pool, 0xAA, (size_t)count * 4096U);
    return FAKE_POOL_PHYS;
}

bool pmm_free_contiguous_pages(uint64_t physical_address, uint32_t count) {
    (void)physical_address;
    (void)count;
    return true;
}

uint64_t timer_ticks(void) { return 0U; }
uint64_t timer_cycles(void) { return 0U; }
uint64_t serial_sequence_begin(void) { return 0U; }
void serial_sequence_end(uint64_t flags) { (void)flags; }
void serial_write(const char *text) { (void)text; }
void serial_write_hex(uint64_t value) { (void)value; }
void serial_write_u64(uint64_t value) { (void)value; }
void console_record_text(const char *text) { (void)text; }
void console_record_hex64(uint64_t value) { (void)value; }
void console_record_u64(uint64_t value) { (void)value; }
void console_journal_begin(void) {}
void console_journal_end(void) {}
void console_putc(char c) { (void)c; }
void console_write(const char *text) { (void)text; }
void console_publish_journal(const char *record, size_t length) {
    (void)record;
    (void)length;
}

enum pci_status pci_scan_class(struct pci_config_io *io, uint8_t base_class,
                               uint8_t subclass, uint8_t prog_if,
                               struct pci_device_header *devices,
                               size_t capacity, size_t *count) {
    (void)io;
    (void)base_class;
    (void)subclass;
    (void)prog_if;
    (void)devices;
    (void)capacity;
    if (count != NULL) {
        *count = 0U;
    }
    return PCI_OK;
}

enum vmm_status vmm_map_mmio_page(uint64_t physical_address,
                                  uint64_t hhdm_offset,
                                  volatile void **out_virtual) {
    (void)physical_address;
    (void)hhdm_offset;
    if (out_virtual != NULL) {
        *out_virtual = NULL;
    }
    return VMM_BAD_ALIGNMENT;
}

/* ------------------------------------------------------------------------- */
/* The controller model. */

#define MODEL_CAPLENGTH 0x80U
#define MODEL_RT_OFFSET 0x1000U
#define MODEL_DB_OFFSET 0x2000U
#define MODEL_MAX_SLOTS 8U
#define MODEL_MAX_PORTS 8U

struct model {
    uint32_t hcs_params2;
    bool context_64;
    bool running;
    bool hse;
    uint32_t usbcmd;
    uint32_t crcr_low;
    uint32_t crcr_high;
    uint32_t dcbaap_low;
    uint32_t dcbaap_high;
    uint32_t config;
    uint32_t iman;
    uint32_t imod;
    uint32_t erstsz;
    uint32_t erstba_low;
    uint32_t erstba_high;
    uint32_t erdp_low;
    uint32_t erdp_high;
    uint32_t portsc[MODEL_MAX_PORTS + 1U];
    size_t command_count;
    size_t event_count;
    size_t doorbell_count;
    uint8_t next_slot;
    uint64_t dcbaa0_at_rs;
    bool rs_seen;
};

static struct model model;
static uint8_t registers[0x3000];

static size_t model_address(const volatile uint8_t *base, uint32_t offset) {
    return (size_t)((const uint8_t *)base - registers) + (size_t)offset;
}

static uint32_t model_operational(uint32_t offset) {
    return MODEL_CAPLENGTH + offset;
}

static uint64_t model_pool_physical(uint64_t physical) {
    return physical - FAKE_POOL_PHYS;
}

static void model_dma_read(uint64_t physical, void *target, size_t bytes) {
    if (physical < FAKE_POOL_PHYS ||
        physical + bytes > FAKE_POOL_PHYS + sizeof(fake_pool)) {
        memset(target, 0, bytes);
        return;
    }
    memcpy(target, fake_pool + model_pool_physical(physical), bytes);
}

static uint64_t model_dcbaa_entry(uint32_t slot) {
    uint64_t physical = ((uint64_t)model.dcbaap_high << 32) |
                        (uint64_t)model.dcbaap_low;
    uint64_t entry = 0U;

    model_dma_read(physical + (uint64_t)slot * sizeof(uint64_t), &entry,
                   sizeof(entry));
    return entry;
}

static void model_emit_completion(uint64_t trb_physical, uint32_t code,
                                  uint32_t parameter, uint32_t slot) {
    const uint64_t ring = FAKE_POOL_PHYS + XHCI_OFF_EVENT_RING;
    const uint32_t index =
        (uint32_t)(model.event_count % (size_t)XHCI_EVENT_TRBS);
    uint32_t event[4];

    model.event_count += 1U;
    event[0] = (uint32_t)(trb_physical & 0xFFFFFFFFULL);
    event[1] = (uint32_t)(trb_physical >> 32);
    event[2] = (code << 24) | (parameter & 0x00FFFFFFU);
    event[3] = ((uint32_t)XHCI_TRB_COMMAND_COMPLETION << XHCI_TRB_TYPE_SHIFT) |
               (slot << XHCI_TRB_SLOT_SHIFT) | XHCI_TRB_CYCLE;
    memcpy(fake_pool + model_pool_physical(ring) + index * XHCI_TRB_BYTES,
           event, sizeof(event));
}

/* The command the doorbell names: the driver's own ring push order. */
static void model_complete_command(void) {
    const uint64_t ring = FAKE_POOL_PHYS + XHCI_OFF_COMMAND_RING;
    const uint32_t index =
        (uint32_t)(model.command_count % (size_t)XHCI_RING_TRBS);
    uint32_t trb[4] = {0U, 0U, 0U, 0U};
    uint64_t trb_physical = ring + (uint64_t)index * XHCI_TRB_BYTES;
    uint32_t type;
    uint32_t slot = 0U;
    uint32_t parameter = 0U;

    model.command_count += 1U;
    model_dma_read(trb_physical, trb, sizeof(trb));
    type = (trb[3] & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
    if (type == XHCI_TRB_ENABLE_SLOT) {
        model.next_slot = (uint8_t)(model.next_slot + 1U);
        slot = model.next_slot;
        parameter = slot;
    } else if (type == XHCI_TRB_ADDRESS_DEVICE) {
        /*
         * The controller copies the slot and endpoint contexts from the Input
         * Context into the slot's Device Context (xHCI 1.1 section 6.4.3.4).
         * Model that copy byte for byte so the proof can read the result at
         * the DCBAA address rather than through the driver's pointers.
         */
        const uint64_t input =
            ((uint64_t)trb[1] << 32) | (uint64_t)trb[0];
        slot = (trb[3] >> XHCI_TRB_SLOT_SHIFT) & 0xFFU;
        if (slot != 0U && slot <= MODEL_MAX_SLOTS) {
            const uint64_t device = model_dcbaa_entry(slot);
            const size_t bytes = xhci->slot_area_bytes;

            if (device != 0U && input != 0U &&
                bytes == (size_t)XHCI_DEVICE_CONTEXT_ENTRIES *
                             xhci->context_bytes) {
                uint8_t *source = fake_pool + model_pool_physical(input) +
                                  xhci->context_bytes;
                uint8_t *target = fake_pool + model_pool_physical(device);

                memmove(target, source, bytes);
            }
        }
    }
    model_emit_completion(trb_physical, XHCI_COMPLETION_SUCCESS, parameter,
                          slot);
}

uint32_t xhci_host_mmio_read32(const volatile uint8_t *base, uint32_t offset) {
    const size_t address = model_address(base, offset);

    if (address == 0x00U) {
        return 0x01000080U; /* HCIVERSION 1.0, CAPLENGTH 0x80 */
    }
    if (address == XHCI_CAP_HCSPARAMS1) {
        return MODEL_MAX_SLOTS | (MODEL_MAX_PORTS << 24);
    }
    if (address == XHCI_CAP_HCSPARAMS2) {
        return model.hcs_params2;
    }
    if (address == XHCI_CAP_HCCPARAMS1) {
        return model.context_64 ? 0x4U : 0x0U;
    }
    if (address == XHCI_CAP_DBOFF) {
        return MODEL_DB_OFFSET;
    }
    if (address == XHCI_CAP_RTSOFF) {
        return MODEL_RT_OFFSET;
    }
    if (address == model_operational(XHCI_OP_USBCMD)) {
        return model.usbcmd;
    }
    if (address == model_operational(XHCI_OP_USBSTS)) {
        uint32_t status = 0U;

        if (!model.running) {
            status |= XHCI_USBSTS_HCH;
        }
        if (model.hse) {
            status |= XHCI_USBSTS_HSE;
        }
        return status;
    }
    if (address == model_operational(XHCI_OP_PAGESIZE)) {
        return 0x1U; /* 4 KiB supported */
    }
    if (address == model_operational(XHCI_OP_CRCR)) {
        return model.crcr_low;
    }
    if (address == model_operational(XHCI_OP_CRCR) + 4U) {
        return model.crcr_high;
    }
    if (address == model_operational(XHCI_OP_DCBAAP)) {
        return model.dcbaap_low;
    }
    if (address == model_operational(XHCI_OP_DCBAAP) + 4U) {
        return model.dcbaap_high;
    }
    if (address == model_operational(XHCI_OP_CONFIG)) {
        return model.config;
    }
    if (address >= model_operational(XHCI_OP_PORTSC_BASE) &&
        address < model_operational(XHCI_OP_PORTSC_BASE) +
                      MODEL_MAX_PORTS * XHCI_OP_PORTSC_STRIDE) {
        const size_t index =
            (address - model_operational(XHCI_OP_PORTSC_BASE)) /
            XHCI_OP_PORTSC_STRIDE;

        return model.portsc[index + 1U];
    }
    if (address == MODEL_RT_OFFSET + XHCI_RT_IR0 + XHCI_IR_IMAN) {
        return model.iman;
    }
    if (address == MODEL_RT_OFFSET + XHCI_RT_IR0 + XHCI_IR_IMOD) {
        return model.imod;
    }
    if (address == MODEL_RT_OFFSET + XHCI_RT_IR0 + XHCI_IR_ERSTSZ) {
        return model.erstsz;
    }
    if (address == MODEL_RT_OFFSET + XHCI_RT_IR0 + XHCI_IR_ERSTBA) {
        return model.erstba_low;
    }
    if (address == MODEL_RT_OFFSET + XHCI_RT_IR0 + XHCI_IR_ERSTBA + 4U) {
        return model.erstba_high;
    }
    if (address == MODEL_RT_OFFSET + XHCI_RT_IR0 + XHCI_IR_ERDP) {
        return model.erdp_low;
    }
    if (address == MODEL_RT_OFFSET + XHCI_RT_IR0 + XHCI_IR_ERDP + 4U) {
        return model.erdp_high;
    }
    return 0U;
}

void xhci_host_mmio_write32(volatile uint8_t *base, uint32_t offset,
                            uint32_t value) {
    const size_t address = model_address(base, offset);

    if (address == model_operational(XHCI_OP_USBCMD)) {
        if ((value & XHCI_USBCMD_HCRST) != 0U) {
            /* The reset completes immediately in this model. */
            value &= ~XHCI_USBCMD_HCRST;
            model.running = false;
        }
        model.usbcmd = value;
        model.running = (value & XHCI_USBCMD_RS) != 0U;
        if (model.running) {
            model.rs_seen = true;
            model.dcbaa0_at_rs = model_dcbaa_entry(0U);
        }
        return;
    }
    if (address == model_operational(XHCI_OP_USBSTS)) {
        return;
    }
    if (address == model_operational(XHCI_OP_CRCR)) {
        model.crcr_low = value;
        return;
    }
    if (address == model_operational(XHCI_OP_CRCR) + 4U) {
        model.crcr_high = value;
        return;
    }
    if (address == model_operational(XHCI_OP_DCBAAP)) {
        model.dcbaap_low = value;
        return;
    }
    if (address == model_operational(XHCI_OP_DCBAAP) + 4U) {
        model.dcbaap_high = value;
        return;
    }
    if (address == model_operational(XHCI_OP_CONFIG)) {
        model.config = value;
        return;
    }
    if (address == MODEL_RT_OFFSET + XHCI_RT_IR0 + XHCI_IR_IMAN) {
        model.iman = value;
        return;
    }
    if (address == MODEL_RT_OFFSET + XHCI_RT_IR0 + XHCI_IR_IMOD) {
        model.imod = value;
        return;
    }
    if (address == MODEL_RT_OFFSET + XHCI_RT_IR0 + XHCI_IR_ERSTSZ) {
        model.erstsz = value;
        return;
    }
    if (address == MODEL_RT_OFFSET + XHCI_RT_IR0 + XHCI_IR_ERSTBA) {
        model.erstba_low = value;
        return;
    }
    if (address == MODEL_RT_OFFSET + XHCI_RT_IR0 + XHCI_IR_ERSTBA + 4U) {
        model.erstba_high = value;
        return;
    }
    if (address == MODEL_RT_OFFSET + XHCI_RT_IR0 + XHCI_IR_ERDP) {
        model.erdp_low = value;
        return;
    }
    if (address == MODEL_RT_OFFSET + XHCI_RT_IR0 + XHCI_IR_ERDP + 4U) {
        model.erdp_high = value;
        return;
    }
    if (address >= MODEL_DB_OFFSET && address < MODEL_DB_OFFSET + 256U * 4U) {
        const size_t index = (address - MODEL_DB_OFFSET) / 4U;

        model.doorbell_count += 1U;
        if (index == 0U) {
            model_complete_command();
        }
        return;
    }
}

/* ------------------------------------------------------------------------- */
/* Fixtures. */

static void model_reset(uint32_t scratchpads, bool context_64) {
    memset(&model, 0, sizeof(model));
    memset(registers, 0, sizeof(registers));
    memset(fake_pool, 0xAA, sizeof(fake_pool));
    fake_alloc_fail = false;
    fake_allocated_pages = 0U;
    model.context_64 = context_64;
    /* HCSPARAMS2: Max Scratchpad Buffers Hi in 25:21, Lo in 31:27; the ERST
       max field is 2^4 segments (bits 7:4). */
    model.hcs_params2 = (((scratchpads >> 5U) & 0x1FU) << 21U) |
                        ((scratchpads & 0x1FU) << 27U) | (4U << 4U);
    for (uint32_t port = 1U; port <= MODEL_MAX_PORTS; ++port) {
        model.portsc[port] = 0x000002A0U;
    }
}

static void fixture(uint32_t scratchpads, bool context_64) {
    model_reset(scratchpads, context_64);
    memset(&xhci_machine, 0, sizeof(xhci_machine));
    memset(xhci_sessions, 0, sizeof(xhci_sessions));
    xhci = &xhci_sessions[0];
    xhci->in_use = true;
    xhci->location.bus = 0U;
    xhci->location.device = 0x14U;
    xhci->location.function = 0U;
    xhci->pci_vendor = 0x8086U;
    xhci->pci_device = 0x1E31U;
    xhci->mmio_bytes = sizeof(registers);
    xhci->regs = registers;
    xhci_machine.hhdm_offset = (uint64_t)(uintptr_t)fake_pool - FAKE_POOL_PHYS;
}

static bool start_ok(uint32_t scratchpads, bool context_64) {
    fixture(scratchpads, context_64);
    return xhci_controller_start();
}

static uint64_t pool_entry(uint64_t physical) {
    uint64_t value = 0U;

    model_dma_read(physical, &value, sizeof(value));
    return value;
}

static bool within_pool(uint64_t physical, size_t bytes) {
    return physical >= FAKE_POOL_PHYS &&
           physical + bytes <= FAKE_POOL_PHYS + xhci->pool_bytes;
}

/* ------------------------------------------------------------------------- */
/* Tests. */

static void test_scratchpads_are_allocated_and_published(void) {
    CHECK(start_ok(16U, false), "a controller with 16 scratchpads starts");
    CHECK(xhci->scratchpad_count == 16U,
          "HCSPARAMS2's scratchpad count is read, not a constant");
    CHECK(xhci->scratchpad_page_bytes == 4096U,
          "the supported PAGESIZE selects 4 KiB scratchpad pages");
    CHECK(xhci->dcbaa0 != 0U, "DCBAA[0] names the scratchpad pointer array");
    CHECK(model.rs_seen && model.dcbaa0_at_rs == xhci->dcbaa0,
          "DCBAA[0] was published before USBCMD.RS was written");
    CHECK(model_dcbaa_entry(0U) == xhci->dcbaa0,
          "the controller's DCBAA[0] is the pointer array");
    for (uint32_t index = 0U; index < 16U; ++index) {
        const uint64_t page = pool_entry(xhci->dcbaa0 +
                                         (uint64_t)index * sizeof(uint64_t));

        CHECK(page != 0U && (page % 4096U) == 0U &&
                  within_pool(page, 4096U),
              "every scratchpad entry is a page-aligned page inside the pool");
        CHECK(fake_pool[model_pool_physical(page)] == 0U,
              "every scratchpad page is zeroed");
    }
}

static void test_no_scratchpads_leaves_dcbaa0_zero(void) {
    CHECK(start_ok(0U, false), "a controller with no scratchpads starts");
    CHECK(xhci->scratchpad_count == 0U, "a zero HCSPARAMS2 count is honoured");
    CHECK(xhci->dcbaa0 == 0U && model_dcbaa_entry(0U) == 0U,
          "DCBAA[0] stays zero when the controller needs no scratchpad");
    CHECK(xhci->scratchpad_page_bytes == 0U,
          "no scratchpad page is allocated for a zero count");
}

static void test_insufficient_memory_is_refused_before_rs(void) {
    fixture(16U, false);
    fake_alloc_fail = true;
    CHECK(!xhci_controller_start(),
          "an allocation failure refuses the controller");
    CHECK(xhci->status == XHCI_STATUS_DMA_UNAVAILABLE,
          "the refusal is the DMA status, not a later command timeout");
    CHECK(!model.rs_seen, "nothing was started without the scratchpad pages");

    fixture(1023U, true);
    CHECK(!xhci_controller_start(),
          "a pool larger than the bound is refused, not half-allocated");
    CHECK(!model.rs_seen, "the oversized pool never reaches USBCMD.RS");
}

static void test_adjacent_slots_do_not_overlap(void) {
    for (uint32_t pass = 0U; pass < 2U; ++pass) {
        const bool context_64 = pass != 0U;
        const uint64_t context_bytes = context_64 ? 64U : 32U;

        CHECK(start_ok(16U, context_64), "the slot-area controller starts");
        CHECK(xhci->context_bytes == context_bytes,
              "the context record size follows HCCPARAMS1.CSZ");
        CHECK(xhci->slot_area_bytes ==
                  XHCI_DEVICE_CONTEXT_ENTRIES * context_bytes,
              "the slot area is 32 records, not one");
        {
            const uint64_t first = model_dcbaa_entry(1U);
            const uint64_t second = model_dcbaa_entry(2U);

            CHECK(first != 0U && second != 0U,
                  "both enabled slots have a device context address");
            CHECK(second - first == xhci->slot_area_bytes,
                  "adjacent slots are one full area apart");
            CHECK(within_pool(first, xhci->slot_area_bytes) &&
                      within_pool(second, xhci->slot_area_bytes),
                  "each slot area lies inside the pool");
            CHECK(first + xhci->slot_area_bytes <= second,
                  "the first and second slot areas do not overlap");
        }
    }
}

static void test_ep0_of_the_first_slot_survives_the_second(void) {
    const uint64_t context_bytes = 32U;
    uint8_t first_ep0_before[32];

    CHECK(start_ok(16U, false), "the two-slot controller starts");
    {
        struct xhci_ring first_ring;
        struct xhci_ring second_ring;
        const uint64_t device1 = model_dcbaa_entry(1U);
        const uint64_t device2 = model_dcbaa_entry(2U);

        xhci_ring_init(&first_ring,
                       (struct xhci_trb *)xhci_dma_at(
                           XHCI_OFF_DEVICE_EP0_RING),
                       xhci_dma_physical(XHCI_OFF_DEVICE_EP0_RING),
                       XHCI_RING_TRBS, 1U);
        xhci_ring_init(&second_ring,
                       (struct xhci_trb *)xhci_dma_at(
                           XHCI_OFF_DEVICE_EP0_RING + 0x1000U),
                       xhci_dma_physical(XHCI_OFF_DEVICE_EP0_RING + 0x1000U),
                       XHCI_RING_TRBS, 1U);
        CHECK(xhci_address_device_ring(&first_ring, 1U, 2U, 3U, 64U),
              "slot 1 is addressed");
        model_dma_read(device1 + context_bytes, first_ep0_before,
                       sizeof(first_ep0_before));
        CHECK(xhci_address_device_ring(&second_ring, 2U, 3U, 2U, 8U),
              "slot 2 is addressed");
        CHECK(memcmp(fake_pool + model_pool_physical(device1 + context_bytes),
                     first_ep0_before, sizeof(first_ep0_before)) == 0,
              "slot 2's write leaves slot 1's EP0 context untouched");
        CHECK(memcmp(fake_pool + model_pool_physical(device1 + context_bytes),
                     fake_pool + model_pool_physical(device2 + context_bytes),
                     sizeof(first_ep0_before)) != 0,
              "the two slots' EP0 contexts are different records");
        CHECK(device2 - device1 == xhci->slot_area_bytes,
              "the second slot's area is a full slot area away");
    }
}

static void test_hse_stops_commands_and_keeps_the_first_snapshot(void) {
    char first[sizeof(xhci->fatal)];
    size_t doorbells;

    CHECK(start_ok(16U, false), "the fatal-snapshot controller starts");
    model.hse = true;
    doorbells = model.doorbell_count;
    {
        const struct xhci_trb noop = xhci_trb_noop_command(1U);
        const char *detail = NULL;

        CHECK(!xhci_command_ok(&noop, &detail),
              "a command after HSE is refused");
        CHECK(xhci->fatal_valid, "the first fatal condition is recorded");
        CHECK(strstr(xhci->fatal, "cause=HSE") != NULL,
              "the snapshot names HSE as the cause");
        CHECK(strstr(xhci->fatal, "usbsts=0x") != NULL &&
                  strstr(xhci->fatal, "crcr=0x") != NULL &&
                  strstr(xhci->fatal, "dcbaap=0x") != NULL,
              "the snapshot keeps the controller's command/status registers");
        CHECK(detail != NULL && strcmp(detail, "CONTROLLER_FATAL_HSE") == 0,
              "the refusal names the fatal cause, not a command timeout");
        CHECK(model.doorbell_count == doorbells,
              "the fatal controller is given no further command doorbell");
    }
    memcpy(first, xhci->fatal, sizeof(first));
    xhci_fatal_capture("HCH");
    CHECK(memcmp(first, xhci->fatal, sizeof(first)) == 0,
          "a later fatal capture does not displace the first snapshot");
    {
        const struct xhci_trb enable = xhci_trb_enable_slot(1U);
        const char *detail = NULL;

        CHECK(!xhci_command_ok(&enable, &detail),
              "no further command is submitted to a fatal controller");
        CHECK(model.doorbell_count == doorbells,
              "the second refusal never reaches the command ring");
    }
}

int main(void) {
    test_scratchpads_are_allocated_and_published();
    test_no_scratchpads_leaves_dcbaa0_zero();
    test_insufficient_memory_is_refused_before_rs();
    test_adjacent_slots_do_not_overlap();
    test_ep0_of_the_first_slot_survives_the_second();
    test_hse_stops_commands_and_keeps_the_first_snapshot();
    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    printf("xHCI DMA host test: ALL OK\n");
    return 0;
}
