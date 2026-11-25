/*
 * ITEM-578 host proof: the report descriptor the device sent is the descriptor
 * the parser reads, and the second role gets a fresh USB address.
 *
 * The a853d322 hardware run reached the device on port 2 (04D9:A09F), read its
 * configuration and asked for its report descriptor - and refused it as
 * "not a keyboard" while the pointer record still said the device had never
 * been attempted.  Two separate faults produce exactly that record:
 *
 *   * the Data Stage TRB of a control transfer named the device's generic
 *     descriptor buffer, while the report-descriptor parser read the buffer
 *     next to it.  The transfer succeeded, the bytes went to one address, and
 *     the parser classified a different (zeroed) buffer;
 *   * the mouse role was offered the same device after the keyboard role had
 *     already addressed it.  The second Address Device runs while the device
 *     still answers on the first address, and no port reset was performed
 *     between the two attempts, so the pointer never enumerated.
 *
 * This proof drives the production paths - xhci_parse_report_layout(),
 * xhci_parse_mouse_report_layout(), xhci_attach_device() and the whole
 * xhci_probe_ports() walk - against a controller and device model, and pins:
 *
 *   * the model writes every descriptor at the address the Data Stage TRB
 *     names, and the production parser returns the layout of those bytes
 *     (keyboard and mouse, not a hand-fed buffer);
 *   * a report-descriptor transfer that fails at the wire is reported as a
 *     read failure, not as a descriptor classification verdict;
 *   * the production walk refuses the first role, performs a real USB reset,
 *     and binds the second role on the same device; without the reset the same
 *     second attempt fails at Address Device, which is the acceptance host's
 *     exact failure;
 *   * the SET_PROTOCOL fallback parses the descriptor for the role it is
 *     binding (a boot mouse that refuses SET_PROTOCOL binds through its own
 *     report descriptor instead of being classified as a keyboard).
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
/* The external sinks the driver's start and walk paths may reach. */

#define FAKE_POOL_PHYS 0x400000ULL
#define FAKE_POOL_BYTES (4U * 1024U * 1024U)

static uint8_t fake_pool[FAKE_POOL_BYTES];

uint64_t pmm_alloc_contiguous_pages(uint32_t count) {
    if (count == 0U ||
        (size_t)count * 4096U > sizeof(fake_pool)) {
        return 0U;
    }
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
void timer_delay_ms(uint32_t milliseconds) { (void)milliseconds; }
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

/*
 * The walk offers a port no HID role bound to USB mass storage.  This proof
 * binds its pointer first, so storage is never attempted on the model; these
 * are the seams the walk's storage branch links against, refusing by name so a
 * call that ever did happen would fail the proof visibly instead of passing.
 */
uint64_t timer_cycles_to_us(uint64_t cycles) { (void)cycles; return 0U; }

enum usb_storage_status usb_storage_parse_config(
    const void *descriptor, size_t length, struct usb_storage_config *config) {
    (void)descriptor;
    (void)length;
    (void)config;
    return USB_STORAGE_UNSUPPORTED_INTERFACE;
}

const char *usb_storage_status_string(enum usb_storage_status status) {
    (void)status;
    return "NOT_MASS_STORAGE";
}

enum usb_storage_status usb_storage_bind(
    struct usb_storage *storage, const struct usb_storage_transport *transport,
    const struct usb_storage_config *config) {
    (void)storage;
    (void)transport;
    (void)config;
    return USB_STORAGE_UNSUPPORTED_INTERFACE;
}

enum usb_storage_status usb_storage_read(
    struct usb_storage *storage, uint64_t lba, uint32_t blocks, void *buffer) {
    (void)storage;
    (void)lba;
    (void)blocks;
    (void)buffer;
    return USB_STORAGE_UNSUPPORTED_INTERFACE;
}

enum block_device_status usb_storage_block_device_bind(
    struct usb_storage *storage, struct usb_storage_block_device *backend) {
    (void)storage;
    (void)backend;
    return BLOCK_DEVICE_UNSUPPORTED;
}

const char *block_device_status_string(enum block_device_status status) {
    (void)status;
    return "NOT_READY";
}

enum block_device_status block_device_read(struct block_device *device,
                                           uint64_t offset, void *buffer,
                                           size_t length) {
    (void)device;
    (void)offset;
    (void)buffer;
    (void)length;
    return BLOCK_DEVICE_UNSUPPORTED;
}

bool install_medium_boot_identity(struct install_medium_boot_identity *out) {
    (void)out;
    return false;
}

struct install_medium_verdict install_medium_identify(
    const struct install_medium_boot_identity *boot,
    const uint8_t sector[INSTALL_MEDIUM_SECTOR_BYTES], uint64_t block_count) {
    struct install_medium_verdict verdict;

    (void)boot;
    (void)sector;
    (void)block_count;
    memset(&verdict, 0, sizeof(verdict));
    verdict.result = INSTALL_MEDIUM_NO_IDENTITY;
    verdict.reason = "NO_LOADER";
    return verdict;
}

const char *install_medium_result_string(enum install_medium_result result) {
    (void)result;
    return "NO_IDENTITY";
}

bool install_journal_platform_bind_medium(
    struct block_device *device, const struct install_medium_verdict *verdict) {
    (void)device;
    (void)verdict;
    return false;
}


/* ITEM-582: the host fixtures link xhci.c without the journal platform; the
   production probe path stays in xhci.c and these fixtures answer as a machine
   with no diagnostic medium. */
bool install_journal_platform_bind_diagnostic(
    struct block_device *device,
    const struct install_medium_verdict *boot_verdict) {
    (void)device;
    (void)boot_verdict;
    return false;
}

void install_journal_platform_diagnostic_state(bool *attempted,
                                               const char **result,
                                               const char **reason,
                                               const char **placement) {
    if (attempted != NULL) {
        *attempted = false;
    }
    if (result != NULL) {
        *result = "UNAVAILABLE";
    }
    if (reason != NULL) {
        *reason = "NOT_PROBED";
    }
    if (placement != NULL) {
        *placement = "NONE";
    }
}

bool install_journal_platform_medium_unbound(void) {
    return true;
}

/* ------------------------------------------------------------------------- */
/* The device the model answers as. */

/* 04D9:A09F: a HID interface that declares no boot protocol, so only its own
   report descriptor says whether it is a keyboard or a pointer. */
static const uint8_t device_descriptor[] = {
    0x12U, 0x01U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x08U,
    0xD9U, 0x04U, 0x9FU, 0xA0U, 0x00U, 0x01U,
    0x01U, 0x02U, 0x00U, 0x01U
};

/* 45 bytes: Generic Desktop / Keyboard, modifiers then six key bytes - the
   layout the production parser must return (modifier byte 0, keys from 2). */
static const uint8_t keyboard_report_descriptor[] = {
    0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U,
    0x05U, 0x07U, 0x19U, 0xE0U, 0x29U, 0xE7U,
    0x15U, 0x00U, 0x25U, 0x01U, 0x75U, 0x01U, 0x95U, 0x08U, 0x81U, 0x02U,
    0x95U, 0x01U, 0x75U, 0x08U, 0x81U, 0x03U,
    0x95U, 0x06U, 0x75U, 0x08U, 0x15U, 0x00U, 0x25U, 0x65U,
    0x05U, 0x07U, 0x19U, 0x00U, 0x29U, 0x65U, 0x81U, 0x00U,
    0xC0U
};

/* 52 bytes: Generic Desktop / Mouse with three buttons and relative X, Y and
   wheel bytes.  The keyboard parser must refuse it; the pointer parser must
   return it (buttons at byte 0, X at 1, Y at 2). */
static const uint8_t mouse_report_descriptor[] = {
    0x05U, 0x01U, 0x09U, 0x02U, 0xA1U, 0x01U, 0x09U, 0x01U, 0xA1U, 0x00U,
    0x05U, 0x09U, 0x19U, 0x01U, 0x29U, 0x03U, 0x15U, 0x00U, 0x25U, 0x01U,
    0x95U, 0x03U, 0x75U, 0x01U, 0x81U, 0x02U,
    0x95U, 0x01U, 0x75U, 0x05U, 0x81U, 0x03U,
    0x05U, 0x01U, 0x09U, 0x30U, 0x09U, 0x31U, 0x09U, 0x38U,
    0x15U, 0x81U, 0x25U, 0x7FU,
    0x75U, 0x08U, 0x95U, 0x03U, 0x81U, 0x06U,
    0xC0U, 0xC0U
};

/* The same pointer without the wheel usage: a boot mouse whose report
   descriptor declares only buttons, X and Y.  A three-byte boot report is the
   only wire format this device has, so the driver must keep boot protocol. */
static const uint8_t mouse_report_descriptor_no_wheel[] = {
    0x05U, 0x01U, 0x09U, 0x02U, 0xA1U, 0x01U, 0x09U, 0x01U, 0xA1U, 0x00U,
    0x05U, 0x09U, 0x19U, 0x01U, 0x29U, 0x03U, 0x15U, 0x00U, 0x25U, 0x01U,
    0x95U, 0x03U, 0x75U, 0x01U, 0x81U, 0x02U,
    0x95U, 0x01U, 0x75U, 0x05U, 0x81U, 0x03U,
    0x05U, 0x01U, 0x09U, 0x30U, 0x09U, 0x31U,
    0x15U, 0x81U, 0x25U, 0x7FU,
    0x75U, 0x08U, 0x95U, 0x02U, 0x81U, 0x06U,
    0xC0U, 0xC0U
};

/* ITEM-640: the exact 71-byte descriptor of the physical 04D9:A09F pointer,
   read read-only from the same device on Debian.  Buttons in byte 0, signed
   little-endian 16-bit X and Y at bytes 1 and 3, wheel at byte 5 and an AC Pan
   byte at 6 that the pointer decoder must ignore.  SHA-256
   aafe4959849fa3b3102be488a80dbfd228d47d733e7050297dfc977a80e56daa */
static const uint8_t toshiba_16bit_mouse_report_descriptor[] = {
    0x05U, 0x01U, 0x09U, 0x02U, 0xA1U, 0x01U, 0x09U, 0x01U,
    0xA1U, 0x00U, 0x05U, 0x09U, 0x15U, 0x00U, 0x25U, 0x01U,
    0x19U, 0x01U, 0x29U, 0x05U, 0x75U, 0x01U, 0x95U, 0x05U,
    0x81U, 0x02U, 0x95U, 0x03U, 0x81U, 0x01U, 0x05U, 0x01U,
    0x16U, 0x01U, 0x80U, 0x26U, 0xFFU, 0x7FU, 0x09U, 0x30U,
    0x09U, 0x31U, 0x75U, 0x10U, 0x95U, 0x02U, 0x81U, 0x06U,
    0x15U, 0x81U, 0x25U, 0x7FU, 0x09U, 0x38U, 0x75U, 0x08U,
    0x95U, 0x01U, 0x81U, 0x06U, 0x05U, 0x0CU, 0x0AU, 0x38U,
    0x02U, 0x95U, 0x01U, 0x81U, 0x06U, 0xC0U, 0xC0U,
};

/* 16 bytes: a consumer-control collection, i.e. a HID device that is neither a
   keyboard nor a pointer.  Both walks refuse it, and each publishes its own
   reason - the shape a combined receiver or a media keypad produces. */
static const uint8_t other_report_descriptor[] = {
    0x05U, 0x0CU, 0x09U, 0x01U, 0xA1U, 0x01U, 0x09U, 0x01U,
    0x15U, 0x00U, 0x25U, 0x01U, 0x75U, 0x01U, 0x95U, 0x01U,
    0x81U, 0x02U, 0xC0U
};

#define MODEL_CONFIG_TOTAL 34U
#define MODEL_CONFIG_SUBCLASS_OFFSET 15U
#define MODEL_CONFIG_PROTOCOL_OFFSET 16U
#define MODEL_CONFIG_REPORT_LENGTH_OFFSET 25U

/* The configuration the model answers with.  `subclass`/`protocol` shape the
   interface the walk sees; the HID descriptor publishes the report length. */
static uint8_t config_descriptor[MODEL_CONFIG_TOTAL];

static void build_config(uint8_t subclass, uint8_t protocol,
                         uint16_t report_length) {
    static const uint8_t prefix[MODEL_CONFIG_TOTAL] = {
        0x09U, 0x02U, MODEL_CONFIG_TOTAL, 0x00U, 0x01U, 0x01U, 0x00U, 0xA0U,
        0x32U,
        0x09U, 0x04U, 0x00U, 0x00U, 0x01U, 0x03U, 0x00U, 0x00U, 0x00U,
        0x09U, 0x21U, 0x11U, 0x01U, 0x00U, 0x01U, 0x22U, 0x00U, 0x00U,
        0x07U, 0x05U, 0x81U, 0x03U, 0x08U, 0x00U, 0x0AU
    };

    memcpy(config_descriptor, prefix, sizeof(prefix));
    config_descriptor[MODEL_CONFIG_SUBCLASS_OFFSET] = subclass;
    config_descriptor[MODEL_CONFIG_PROTOCOL_OFFSET] = protocol;
    config_descriptor[MODEL_CONFIG_REPORT_LENGTH_OFFSET] =
        (uint8_t)(report_length & 0xFFU);
    config_descriptor[MODEL_CONFIG_REPORT_LENGTH_OFFSET + 1U] =
        (uint8_t)(report_length >> 8);
}

/* ------------------------------------------------------------------------- */
/* The controller and device model. */

#define MODEL_CAPLENGTH 0x80U
#define MODEL_RT_OFFSET 0x1000U
#define MODEL_DB_OFFSET 0x2000U
#define MODEL_MAX_SLOTS 8U
#define MODEL_MAX_PORTS 8U
#define MODEL_PORT 2U

#define MODEL_COMPLETION_USB_TRANSACTION_ERROR 4U

struct model {
    bool running;
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
    /* USB 2.0: a device keeps the address it was given until a port reset
       returns it to the Default state. */
    bool addressed[MODEL_MAX_PORTS + 1U];
    uint32_t resets[MODEL_MAX_PORTS + 1U];
    uint32_t command_pos;
    uint32_t event_count;
    uint32_t doorbell_count;
    uint8_t next_slot;
    uint64_t slot_ring[MODEL_MAX_SLOTS + 1U];
    uint32_t slot_pos[MODEL_MAX_SLOTS + 1U];
    /* What the device answers with. */
    const uint8_t *report_descriptor;
    uint16_t report_descriptor_length;
    bool report_descriptor_fail;
    bool refuse_set_protocol;
    /* What the model observed. */
    uint32_t address_ok;
    uint32_t address_failures;
    uint32_t report_reads;
    uint64_t last_report_read_address;
    uint32_t set_protocol_requests;
    uint16_t last_set_protocol_value;
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

static bool model_dma_read(uint64_t physical, void *target, size_t bytes) {
    if (physical < FAKE_POOL_PHYS ||
        physical + bytes > FAKE_POOL_PHYS + sizeof(fake_pool)) {
        memset(target, 0, bytes);
        return false;
    }
    memcpy(target, fake_pool + model_pool_physical(physical), bytes);
    return true;
}

static void model_dma_write(uint64_t physical, const void *source,
                            size_t bytes) {
    if (physical < FAKE_POOL_PHYS ||
        physical + bytes > FAKE_POOL_PHYS + sizeof(fake_pool)) {
        return;
    }
    memcpy(fake_pool + model_pool_physical(physical), source, bytes);
}

/*
 * A ring wraps through the Link TRB at index trb_count - 1, so the position
 * after one push skips that entry - exactly what xhci_ring_push_index() does
 * to the ring the driver fills.
 */
static uint32_t model_ring_advance(uint32_t position, uint32_t pushes) {
    for (uint32_t step = 0U; step < pushes; ++step) {
        position += 1U;
        if (position >= XHCI_RING_TRBS - 1U) {
            position = 0U;
        }
    }
    return position;
}

static void model_read_trb(uint64_t ring, uint32_t position, uint32_t trb[4]) {
    model_dma_read(ring + (uint64_t)position * XHCI_TRB_BYTES, trb,
                   XHCI_TRB_BYTES);
}

static void model_emit_event(uint64_t trb_physical, uint32_t dword2,
                             uint32_t dword3) {
    const uint64_t ring = FAKE_POOL_PHYS + XHCI_OFF_EVENT_RING;
    const uint32_t index = model.event_count % XHCI_EVENT_TRBS;
    uint32_t event[4];

    model.event_count += 1U;
    event[0] = (uint32_t)(trb_physical & 0xFFFFFFFFULL);
    event[1] = (uint32_t)(trb_physical >> 32);
    event[2] = dword2;
    event[3] = dword3 | XHCI_TRB_CYCLE;
    model_dma_write(ring + (uint64_t)index * XHCI_TRB_BYTES, event,
                    sizeof(event));
}

static void model_emit_command_completion(uint64_t trb_physical,
                                          uint32_t code, uint32_t parameter,
                                          uint32_t slot) {
    model_emit_event(trb_physical,
                     (code << 24) | (parameter & 0x00FFFFFFU),
                     ((uint32_t)XHCI_TRB_COMMAND_COMPLETION
                      << XHCI_TRB_TYPE_SHIFT) |
                         (slot << XHCI_TRB_SLOT_SHIFT));
}

static void model_emit_transfer_completion(uint64_t trb_physical,
                                           uint32_t code,
                                           uint32_t transfer_length,
                                           uint32_t slot) {
    model_emit_event(
        trb_physical, (code << 24) | (transfer_length & 0x00FFFFFFU),
        ((uint32_t)XHCI_TRB_TRANSFER_EVENT << XHCI_TRB_TYPE_SHIFT) |
            (1U << XHCI_TRB_EP_SHIFT) | (slot << XHCI_TRB_SLOT_SHIFT));
}

static uint64_t model_dcbaa_entry(uint32_t slot) {
    uint64_t physical = ((uint64_t)model.dcbaap_high << 32) |
                        (uint64_t)model.dcbaap_low;
    uint64_t entry = 0U;

    model_dma_read(physical + (uint64_t)slot * sizeof(uint64_t), &entry,
                   sizeof(entry));
    return entry;
}

static void model_complete_address_device(const uint32_t trb[4],
                                          uint64_t trb_physical) {
    const uint64_t input = ((uint64_t)trb[1] << 32) | (uint64_t)trb[0];
    const uint32_t slot = (trb[3] >> XHCI_TRB_SLOT_SHIFT) & 0xFFU;
    uint32_t slot_dword1 = 0U;
    uint64_t ep0_ring = 0U;
    uint8_t port = 0U;
    uint32_t code = XHCI_COMPLETION_SUCCESS;

    (void)model_dma_read(input + xhci->context_bytes + 4U, &slot_dword1,
                         sizeof(slot_dword1));
    (void)model_dma_read(input + 2U * xhci->context_bytes + 8U, &ep0_ring,
                         sizeof(ep0_ring));
    ep0_ring &= ~(uint64_t)0x0FU;
    port = (uint8_t)((slot_dword1 >> 16U) & 0xFFU);
    /*
     * Address Device sends SET_ADDRESS to the Default Address.  A device that
     * already holds an address answers nothing on address 0, so the command
     * fails with a USB transaction error until a port reset returns it to
     * Default - the a853d322 run's second-role failure, modelled here.
     */
    if (port == 0U || port > MODEL_MAX_PORTS || model.addressed[port]) {
        ++model.address_failures;
        code = MODEL_COMPLETION_USB_TRANSACTION_ERROR;
    } else {
        const uint64_t device = model_dcbaa_entry(slot);
        const size_t bytes = xhci->slot_area_bytes;

        if (device != 0U &&
            bytes ==
                (size_t)XHCI_DEVICE_CONTEXT_ENTRIES * xhci->context_bytes) {
            uint8_t *source = fake_pool + model_pool_physical(input) +
                              xhci->context_bytes;
            uint8_t *target = fake_pool + model_pool_physical(device);

            memmove(target, source, bytes);
        }
        model.addressed[port] = true;
        model.slot_ring[slot] = ep0_ring;
        model.slot_pos[slot] = 0U;
        ++model.address_ok;
    }
    model_emit_command_completion(trb_physical, code, 0U, slot);
}

static void model_complete_command(void) {
    const uint64_t ring = FAKE_POOL_PHYS + XHCI_OFF_COMMAND_RING;
    const uint64_t trb_physical =
        ring + (uint64_t)model.command_pos * XHCI_TRB_BYTES;
    uint32_t trb[4] = {0U, 0U, 0U, 0U};
    uint32_t type;

    model_read_trb(ring, model.command_pos, trb);
    type = (trb[3] & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
    model.command_pos = model_ring_advance(model.command_pos, 1U);
    if (type == XHCI_TRB_ENABLE_SLOT) {
        model.next_slot = (uint8_t)(model.next_slot % MODEL_MAX_SLOTS + 1U);
        model_emit_command_completion(trb_physical, XHCI_COMPLETION_SUCCESS,
                                      model.next_slot, model.next_slot);
        return;
    }
    if (type == XHCI_TRB_ADDRESS_DEVICE) {
        model_complete_address_device(trb, trb_physical);
        return;
    }
    model_emit_command_completion(trb_physical, XHCI_COMPLETION_SUCCESS, 0U,
                                  (trb[3] >> XHCI_TRB_SLOT_SHIFT) & 0xFFU);
}

/*
 * One control transfer, from the ring the driver filled.  Every descriptor is
 * written at the address the Data Stage TRB itself carries - the model makes
 * no assumption about where the driver's buffer is, which is what makes the
 * parser's own read the proof.
 */
static void model_control_transfer(uint32_t slot) {
    uint64_t ring;
    uint32_t position;
    uint32_t setup[4];
    uint32_t data[4];
    uint32_t status[4];
    uint32_t data_length;
    uint64_t data_trb_physical = 0U;
    uint64_t data_buffer = 0U;
    uint64_t status_physical;
    uint8_t request;
    uint8_t request_type;
    uint8_t type;
    uint8_t status_code = XHCI_COMPLETION_SUCCESS;
    const uint8_t *source = NULL;
    uint32_t source_length = 0U;

    if (slot == 0U || slot > MODEL_MAX_SLOTS || model.slot_ring[slot] == 0U) {
        return;
    }
    ring = model.slot_ring[slot];
    position = model.slot_pos[slot];
    model_read_trb(ring, position, setup);
    position = model_ring_advance(position, 1U);
    data_length = (setup[1] >> 16) & 0xFFFFU;
    if (data_length != 0U) {
        model_read_trb(ring, position, data);
        data_trb_physical = ring + (uint64_t)position * XHCI_TRB_BYTES;
        /* The address the Data Stage TRB itself names: the model writes the
           device's answer only here, never at an address of its own. */
        data_buffer = ((uint64_t)data[1] << 32) | (uint64_t)data[0];
        position = model_ring_advance(position, 1U);
    }
    model_read_trb(ring, position, status);
    status_physical = ring + (uint64_t)position * XHCI_TRB_BYTES;
    model.slot_pos[slot] = model_ring_advance(position, 1U);

    request = (uint8_t)(setup[0] >> 8);
    request_type = (uint8_t)(setup[0] & 0xFFU);
    type = (uint8_t)(((setup[0] >> 16) & 0xFF00U) >> 8);
    if (request == USB_REQUEST_GET_DESCRIPTOR &&
        (request_type & 0x80U) != 0U) {
        if (type == USB_DESCRIPTOR_DEVICE) {
            source = device_descriptor;
            source_length = (uint32_t)sizeof(device_descriptor);
        } else if (type == USB_DESCRIPTOR_CONFIGURATION) {
            source = config_descriptor;
            source_length = MODEL_CONFIG_TOTAL;
        } else if (type == USB_DESCRIPTOR_HID_REPORT) {
            source = model.report_descriptor;
            source_length = model.report_descriptor_length;
            model.report_reads += 1U;
            model.last_report_read_address = data_buffer;
            if (model.report_descriptor_fail) {
                /* The wire fails: no byte reaches the buffer at all. */
                model_emit_transfer_completion(
                    data_trb_physical,
                    MODEL_COMPLETION_USB_TRANSACTION_ERROR, data_length, slot);
                return;
            }
        }
    }
    if (data_length != 0U && source != NULL) {
        const uint32_t copied =
            source_length < data_length ? source_length : data_length;

        if (copied != 0U) {
            model_dma_write(data_buffer, source, copied);
        }
        model_emit_transfer_completion(data_trb_physical,
                                       XHCI_COMPLETION_SUCCESS,
                                       data_length - copied, slot);
    }
    if (request == USB_HID_REQUEST_SET_PROTOCOL) {
        model.set_protocol_requests += 1U;
        model.last_set_protocol_value = (uint16_t)(setup[0] >> 16);
        if (model.refuse_set_protocol) {
            status_code = MODEL_COMPLETION_USB_TRANSACTION_ERROR;
        }
    }
    model_emit_transfer_completion(status_physical, status_code, 0U, slot);
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
        return 4U << 4U; /* no scratchpads, ERST max = 16 segments */
    }
    if (address == XHCI_CAP_HCCPARAMS1) {
        return 0U; /* 32-byte contexts, no extended capabilities */
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
        return model.running ? 0U : XHCI_USBSTS_HCH;
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
            value &= ~XHCI_USBCMD_HCRST;
            model.running = false;
            /* A host reset leaves every port disabled and every device at the
               Default Address. */
            for (uint32_t port = 1U; port <= MODEL_MAX_PORTS; ++port) {
                model.addressed[port] = false;
                model.portsc[port] &= ~(uint32_t)XHCI_PORTSC_PED;
            }
        }
        model.usbcmd = value;
        model.running = (value & XHCI_USBCMD_RS) != 0U;
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
    if (address >= model_operational(XHCI_OP_PORTSC_BASE) &&
        address < model_operational(XHCI_OP_PORTSC_BASE) +
                      MODEL_MAX_PORTS * XHCI_OP_PORTSC_STRIDE) {
        const size_t index =
            (address - model_operational(XHCI_OP_PORTSC_BASE)) /
            XHCI_OP_PORTSC_STRIDE;
        const uint32_t port = (uint32_t)index + 1U;

        model.portsc[port] &= ~(value & XHCI_PORTSC_RW1C);
        if ((value & XHCI_PORTSC_PR) != 0U) {
            /*
             * A real USB reset: the device returns to the Default state, so
             * the next Address Device answers on address 0.
             */
            model.resets[port] += 1U;
            model.addressed[port] = false;
            model.portsc[port] |= XHCI_PORTSC_PED | XHCI_PORTSC_PRC;
        }
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
            return;
        }
        if ((value & 0xFFU) == 1U) {
            model_control_transfer((uint32_t)index);
        }
        return;
    }
}

/* ------------------------------------------------------------------------- */
/* Fixtures. */

static void model_reset(uint16_t report_length) {
    memset(&model, 0, sizeof(model));
    memset(registers, 0, sizeof(registers));
    memset(fake_pool, 0xAA, sizeof(fake_pool));
    build_config(0x00U, 0x00U, report_length);
    for (uint32_t port = 1U; port <= MODEL_MAX_PORTS; ++port) {
        model.portsc[port] = 0x000002A0U;
    }
    model.portsc[MODEL_PORT] = 0x000006A1U; /* PP, CCS, full speed */
}

static void fixture(uint16_t report_length) {
    model_reset(report_length);
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
    xhci->header.location.bus = 0U;
    xhci->header.location.device = 0x14U;
    xhci->header.location.function = 0U;
    xhci->header.vendor_id = 0x8086U;
    xhci->header.device_id = 0x1E31U;
    /* The session's device reset - what a controller visit does before any port
       is probed: each role carries its own identity, report length and state. */
    for (uint32_t role = 0U; role < XHCI_HID_DEVICES; ++role) {
        struct xhci_hid_device *device = &xhci->devices[role];

        memset(device, 0, sizeof(*device));
        device->role = (uint8_t)role;
        device->report_length =
            role == XHCI_HID_ROLE_MOUSE ? USB_HID_BOOT_MOUSE_TRANSFER_LENGTH
                                        : USB_HID_BOOT_REPORT_LENGTH;
        device->layout.modifier_offset = USB_HID_REPORT_OFFSET_NONE;
        usb_hid_state_reset(&device->hid);
        usb_hid_mouse_state_reset(&device->mouse);
    }
    xhci_machine.hhdm_offset = (uint64_t)(uintptr_t)fake_pool - FAKE_POOL_PHYS;
}

/* The production start path, with the EP0 ring of `role` initialised the way
   xhci_attach_device() does before its first control transfer. */
static bool started_with_role(uint16_t report_length, uint8_t role) {
    fixture(report_length);
    if (!xhci_controller_start()) {
        return false;
    }
    xhci_ring_init(&xhci->ep0_rings[role],
                   (struct xhci_trb *)xhci_dma_at(
                       XHCI_OFF_DEVICE_EP0_RING + (uint32_t)role * 0x1000U),
                   xhci_dma_physical(XHCI_OFF_DEVICE_EP0_RING +
                                     (uint32_t)role * 0x1000U),
                   XHCI_RING_TRBS, 1U);
    /* The model learns the ring the way an Address Device completion programs
       the slot's device context: one ring per slot, starting at its first TRB. */
    model.slot_ring[1] = xhci->ep0_rings[role].phys;
    model.slot_pos[1] = 0U;
    return true;
}

static struct xhci_hid_device *role_device(uint8_t role, uint8_t slot) {
    struct xhci_hid_device *device = xhci_device(role);

    memset(device, 0, sizeof(*device));
    device->role = role;
    device->slot_id = slot;
    device->port = MODEL_PORT;
    device->speed = XHCI_SPEED_FULL;
    device->layout.modifier_offset = USB_HID_REPORT_OFFSET_NONE;
    usb_hid_state_reset(&device->hid);
    usb_hid_mouse_state_reset(&device->mouse);
    return device;
}

/* ------------------------------------------------------------------------- */
/* The report records the durable evidence renders. */

struct collected {
    char lines[64][512];
    size_t count;
};

static void collect_line(void *context, const char *line) {
    struct collected *out = context;

    if (out->count < sizeof(out->lines) / sizeof(out->lines[0])) {
        snprintf(out->lines[out->count], sizeof(out->lines[0]), "%s", line);
        out->count += 1U;
    }
}

static const char *find_line(const struct collected *out, const char *needle) {
    for (size_t index = 0U; index < out->count; ++index) {
        if (strstr(out->lines[index], needle) != NULL) {
            return out->lines[index];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Tests. */

/*
 * The Data Stage destination and the parser's source are one buffer.
 *
 * The model writes the descriptor at the address the production Data Stage TRB
 * carries.  The production parser must return the layout of those bytes: with
 * the descriptor written into a different buffer (the pre-ITEM-578 rule) the
 * parser reads zeros and refuses.
 */
static void test_report_descriptor_reaches_the_parser(void) {
    struct xhci_hid_device *device;

    CHECK(started_with_role(0U, XHCI_HID_ROLE_KEYBOARD),
          "the controller starts for the keyboard descriptor read");
    device = role_device(XHCI_HID_ROLE_KEYBOARD, 1U);
    model.report_descriptor = keyboard_report_descriptor;
    model.report_descriptor_length =
        (uint16_t)sizeof(keyboard_report_descriptor);
    CHECK(xhci_parse_report_layout(device, MODEL_PORT, 0U),
          "the keyboard parser accepts the descriptor the device sent");
    CHECK(device->layout.modifier_offset == 0U &&
              device->layout.key_offset == 2U &&
              device->layout.key_count == 6U &&
              device->layout.report_length == 8U,
          "the keyboard layout is built from the descriptor's own bytes");
    CHECK(model.last_report_read_address ==
              xhci_dma_physical(xhci_device_data_offset(
                                    XHCI_HID_ROLE_KEYBOARD) +
                                XHCI_DATA_REPORT_DESCRIPTOR),
          "the Data Stage TRB names the report-descriptor buffer");

    CHECK(started_with_role((uint16_t)sizeof(mouse_report_descriptor),
                            XHCI_HID_ROLE_MOUSE),
          "the controller starts for the pointer descriptor read");
    device = role_device(XHCI_HID_ROLE_MOUSE, 1U);
    model.report_descriptor = mouse_report_descriptor;
    model.report_descriptor_length =
        (uint16_t)sizeof(mouse_report_descriptor);
    CHECK(xhci_parse_mouse_report_layout(device, MODEL_PORT, 0U),
          "the pointer parser accepts the descriptor the device sent");
    CHECK(device->mouse_layout.buttons_offset == 0U &&
              device->mouse_layout.buttons_mask == 0x07U &&
              device->mouse_layout.dx_offset == 1U &&
              device->mouse_layout.dy_offset == 2U,
          "the pointer layout is built from the descriptor's own bytes");
    CHECK(model.last_report_read_address ==
              xhci_dma_physical(xhci_device_data_offset(
                                    XHCI_HID_ROLE_MOUSE) +
                                XHCI_DATA_REPORT_DESCRIPTOR),
          "the pointer's Data Stage TRB names the report-descriptor buffer");
    /* The same bytes must not parse as the other role: the two walks ask
       different questions of one buffer. */
    CHECK(!usb_hid_parse_report_descriptor(
              mouse_report_descriptor, sizeof(mouse_report_descriptor),
              &device->layout),
          "the pointer's descriptor is not a keyboard descriptor");
}

/*
 * A report-descriptor transfer that fails at the wire is a read failure, not a
 * classification verdict: the driver must say the read did not happen instead
 * of claiming the descriptor is not a keyboard.
 */
static void test_report_descriptor_read_failure_is_not_a_verdict(void) {
    struct xhci_hid_device *device;

    CHECK(started_with_role((uint16_t)sizeof(mouse_report_descriptor),
                            XHCI_HID_ROLE_KEYBOARD),
          "the controller starts for the failing read");
    device = role_device(XHCI_HID_ROLE_KEYBOARD, 1U);
    model.report_descriptor = mouse_report_descriptor;
    model.report_descriptor_length =
        (uint16_t)sizeof(mouse_report_descriptor);
    model.report_descriptor_fail = true;
    CHECK(!xhci_parse_report_layout(device, MODEL_PORT, 0U),
          "a failed report-descriptor transfer refuses the role");
    CHECK(xhci->status == XHCI_STATUS_ENUMERATION_FAILED,
          "the refusal is an enumeration failure, not a descriptor verdict");
    CHECK(xhci->detail != NULL &&
              strstr(xhci->detail, "REPORT_DESCRIPTOR_READ") != NULL,
          "the detail names the failed read");
    CHECK(xhci->detail == NULL ||
              strstr(xhci->detail, "NOT_KEYBOARD") == NULL,
          "a failed read is never reported as 'not a keyboard'");
    CHECK(model.report_reads == 1U,
          "the model saw exactly one report-descriptor request");
}

/*
 * The a853d322 failure and the production rule that fixes it: after the
 * keyboard role refused the device, the pointer role's Address Device fails
 * while the device still holds the first address - and the full production
 * walk resets the port first, so the pointer binds.
 */
static void test_second_role_needs_a_real_reset(void) {
    struct xhci_hid_device *keyboard;
    struct xhci_hid_device *mouse;
    struct xhci_scan scan;

    /* Phase 1: no reset - the same Address Device the laptop refused. */
    fixture((uint16_t)sizeof(mouse_report_descriptor));
    model.report_descriptor = mouse_report_descriptor;
    model.report_descriptor_length =
        (uint16_t)sizeof(mouse_report_descriptor);
    CHECK(xhci_controller_start(), "the controller starts for the re-address");
    keyboard = role_device(XHCI_HID_ROLE_KEYBOARD, 1U);
    CHECK(!xhci_attach_device(keyboard, MODEL_PORT, XHCI_SPEED_FULL),
          "the keyboard role refuses the pointer's descriptor");
    CHECK(model.resets[MODEL_PORT] == 0U,
          "the first attempt performed no port reset of its own");
    mouse = role_device(XHCI_HID_ROLE_MOUSE, 1U);
    CHECK(!xhci_attach_device(mouse, MODEL_PORT, XHCI_SPEED_FULL),
          "a second Address Device without a reset fails");
    CHECK(model.address_failures == 1U,
          "the model refused the address of a device that is still addressed");
    CHECK(xhci->status == XHCI_STATUS_ENUMERATION_FAILED &&
              xhci->detail != NULL &&
              strstr(xhci->detail, "ADDRESS_DEVICE") != NULL,
          "the refusal names the Address Device stage");

    /* Phase 2: the production walk - reset between the roles, pointer bound. */
    fixture((uint16_t)sizeof(mouse_report_descriptor));
    model.report_descriptor = mouse_report_descriptor;
    model.report_descriptor_length =
        (uint16_t)sizeof(mouse_report_descriptor);
    CHECK(xhci_controller_start(), "the controller starts for the walk");
    xhci_scan_init(&scan, 1U, XHCI_MAX_ATTACH_ATTEMPTS);
    xhci_scan_begin_controller(&scan);
    xhci_probe_ports(&scan);
    CHECK(xhci_machine.mouse_session == xhci,
          "the production walk binds the pointer on the same controller");
    CHECK(xhci->devices[XHCI_HID_ROLE_MOUSE].ready,
          "the pointer role is ready");
    CHECK(xhci->devices[XHCI_HID_ROLE_MOUSE].report_mode &&
              xhci->devices[XHCI_HID_ROLE_MOUSE].mouse_layout.buttons_mask ==
                  0x07U,
          "the bound pointer carries the descriptor's own layout");
    CHECK(model.resets[MODEL_PORT] >= 2U,
          "the walk reset the port before the second role addressed the device");
    CHECK(model.address_failures == 0U,
          "the production walk never re-addresses an addressed device");
}

/*
 * A boot mouse that refuses SET_PROTOCOL binds through the report descriptor
 * the fallback reads - parsed as the role being bound, not as a keyboard.
 */
static void test_set_protocol_fallback_parses_the_role(void) {
    struct xhci_hid_device *mouse;

    fixture((uint16_t)sizeof(mouse_report_descriptor));
    build_config(0x01U, 0x02U, (uint16_t)sizeof(mouse_report_descriptor));
    model.report_descriptor = mouse_report_descriptor;
    model.report_descriptor_length =
        (uint16_t)sizeof(mouse_report_descriptor);
    model.refuse_set_protocol = true;
    CHECK(xhci_controller_start(),
          "the controller starts for the SET_PROTOCOL fallback");
    mouse = role_device(XHCI_HID_ROLE_MOUSE, 1U);
    CHECK(xhci_attach_device(mouse, MODEL_PORT, XHCI_SPEED_FULL),
          "a boot mouse that refuses SET_PROTOCOL binds through its descriptor");
    CHECK(mouse->report_mode,
          "the fallback marked the pointer as a report-protocol device");
    CHECK(mouse->mouse_layout.buttons_mask == 0x07U,
          "the fallback parsed the pointer's own layout");
}

/*
 * The physical 04D9:A09F run: a boot mouse that sends three-byte reports has
 * no wheel on the wire.  Its report descriptor does declare the wheel, so the
 * driver must ask for report protocol once, bind that layout, and decode the
 * fourth byte as the wheel.
 */
static void test_boot_mouse_with_wheel_switches_to_report_protocol(void) {
    struct xhci_hid_device *mouse;
    struct usb_hid_mouse_event event;
    uint8_t *report;
    uint32_t slot;
    uint32_t transfer_length;

    fixture((uint16_t)sizeof(mouse_report_descriptor));
    build_config(USB_HID_SUBCLASS_BOOT, USB_HID_PROTOCOL_MOUSE,
                 (uint16_t)sizeof(mouse_report_descriptor));
    model.report_descriptor = mouse_report_descriptor;
    model.report_descriptor_length =
        (uint16_t)sizeof(mouse_report_descriptor);
    CHECK(xhci_controller_start(),
          "the controller starts for the wheel-capable boot mouse");
    mouse = role_device(XHCI_HID_ROLE_MOUSE, 1U);
    CHECK(xhci_attach_device(mouse, MODEL_PORT, XHCI_SPEED_FULL),
          "the wheel-capable boot mouse binds");
    CHECK(mouse->report_mode,
          "a boot mouse whose descriptor has a wheel is switched to report protocol");
    CHECK(mouse->mouse_layout.wheel_offset == 3U &&
              mouse->mouse_layout.report_length == 4U,
          "the pointer keeps the wheel byte of its own descriptor");
    CHECK(model.set_protocol_requests == 1U &&
              model.last_set_protocol_value == USB_HID_REPORT_PROTOCOL_VALUE,
          "the driver asks for report protocol instead of boot protocol");

    slot = mouse->slot_id;
    report = xhci_dma_at(xhci_report_buffer_offset(mouse, (uint8_t)slot));
    report[0U] = 0x01U;
    report[1U] = 0x00U;
    report[2U] = 0x02U;
    report[3U] = 0x01U;
    transfer_length = mouse->report_length - 4U;
    xhci_decode_mouse_report(mouse, (uint8_t)slot, transfer_length);
    CHECK(xhci_mouse_pop_event(&event),
          "the report-protocol pointer queues the wheel report");
    CHECK(event.dx == 0 && event.dy == 2 && event.wheel == 1,
          "the fourth byte reaches the pointer as the wheel, not a delta");
    CHECK(xhci_mouse_wheel_count() == 1U,
          "the production wheel counter records the notch");
}

/*
 * ITEM-640: the physical 04D9:A09F pointer reports signed 16-bit X and Y.  Its
 * boot-protocol three-byte report has no wheel, so the driver must parse this
 * exact descriptor, switch to report protocol and decode the 16-bit deltas and
 * the wheel through the production path.
 */
static void test_toshiba_16bit_mouse_switches_and_decodes(void) {
    struct xhci_hid_device *mouse;
    struct usb_hid_mouse_event event;
    uint8_t *report;
    uint32_t slot;
    uint32_t transfer_length;

    fixture((uint16_t)sizeof(toshiba_16bit_mouse_report_descriptor));
    build_config(USB_HID_SUBCLASS_BOOT, USB_HID_PROTOCOL_MOUSE,
                 (uint16_t)sizeof(toshiba_16bit_mouse_report_descriptor));
    model.report_descriptor = toshiba_16bit_mouse_report_descriptor;
    model.report_descriptor_length =
        (uint16_t)sizeof(toshiba_16bit_mouse_report_descriptor);
    CHECK(xhci_controller_start(),
          "the controller starts for the physical 16-bit pointer");
    mouse = role_device(XHCI_HID_ROLE_MOUSE, 1U);
    CHECK(xhci_attach_device(mouse, MODEL_PORT, XHCI_SPEED_FULL),
          "the physical 16-bit pointer binds through its descriptor");
    CHECK(mouse->report_mode,
          "the 16-bit pointer is switched to report protocol");
    CHECK(mouse->mouse_layout.report_length == 7U &&
              mouse->mouse_layout.buttons_offset == 0U &&
              mouse->mouse_layout.dx_offset == 1U &&
              mouse->mouse_layout.dy_offset == 3U &&
              mouse->mouse_layout.dx_size == 16U &&
              mouse->mouse_layout.dy_size == 16U &&
              mouse->mouse_layout.wheel_offset == 5U &&
              mouse->mouse_layout.wheel_size == 8U,
          "the parsed layout names the 16-bit axes and the wheel byte");
    CHECK(model.set_protocol_requests == 1U &&
              model.last_set_protocol_value == USB_HID_REPORT_PROTOCOL_VALUE,
          "the driver asks for report protocol once");

    slot = mouse->slot_id;
    report = xhci_dma_at(xhci_report_buffer_offset(mouse, (uint8_t)slot));
    report[0U] = 0x05U;
    report[1U] = 0x34U;
    report[2U] = 0x12U;
    report[3U] = 0xFEU;
    report[4U] = 0xFFU;
    report[5U] = 0x01U;
    report[6U] = 0x7FU;
    transfer_length = mouse->report_length - 7U;
    xhci_decode_mouse_report(mouse, (uint8_t)slot, transfer_length);
    CHECK(xhci_mouse_pop_event(&event),
          "the 16-bit report reaches the production decoder");
    CHECK(event.buttons == 0x05U && event.pressed == 0x05U &&
              event.dx == 0x1234 && event.dy == -2 && event.wheel == 1,
          "the 16-bit deltas and the wheel survive the production path");
    CHECK(mouse->last_report_bytes == 7U,
          "the diagnostic publishes the real seven-byte report");
    CHECK(xhci_mouse_wheel_count() == 1U,
          "the production wheel counter records the notch");
}

/*
 * A boot mouse whose descriptor has no wheel must keep boot protocol and its
 * three-byte report; reading the descriptor is an optional probe, not a
 * reason to stop binding.
 */
static void test_boot_mouse_without_wheel_stays_in_boot_protocol(void) {
    struct xhci_hid_device *mouse;
    struct usb_hid_mouse_event event;
    uint8_t *report;
    uint32_t slot;
    uint32_t transfer_length;

    fixture((uint16_t)sizeof(mouse_report_descriptor_no_wheel));
    build_config(USB_HID_SUBCLASS_BOOT, USB_HID_PROTOCOL_MOUSE,
                 (uint16_t)sizeof(mouse_report_descriptor_no_wheel));
    model.report_descriptor = mouse_report_descriptor_no_wheel;
    model.report_descriptor_length =
        (uint16_t)sizeof(mouse_report_descriptor_no_wheel);
    CHECK(xhci_controller_start(),
          "the controller starts for the boot-only pointer");
    mouse = role_device(XHCI_HID_ROLE_MOUSE, 1U);
    CHECK(xhci_attach_device(mouse, MODEL_PORT, XHCI_SPEED_FULL),
          "a boot mouse without a wheel usage still binds");
    CHECK(!mouse->report_mode,
          "a descriptor without a wheel keeps the pointer in boot protocol");
    CHECK(model.set_protocol_requests == 1U &&
              model.last_set_protocol_value == USB_HID_BOOT_PROTOCOL_VALUE,
          "the driver leaves a boot-only pointer on boot protocol");

    slot = mouse->slot_id;
    report = xhci_dma_at(xhci_report_buffer_offset(mouse, (uint8_t)slot));
    report[0U] = 0x00U;
    report[1U] = 0x01U;
    report[2U] = 0xFEU;
    transfer_length = mouse->report_length - USB_HID_BOOT_MOUSE_REPORT_LENGTH;
    xhci_decode_mouse_report(mouse, (uint8_t)slot, transfer_length);
    CHECK(xhci_mouse_pop_event(&event),
          "the three-byte boot report still reaches the pointer");
    CHECK(event.dx == 1 && event.dy == -2 && event.wheel == 0,
          "the boot report keeps its two deltas and has no wheel");
}

/*
 * The optional descriptor probe is not allowed to make a working boot mouse
 * unbindable: a device that fails the report-descriptor read keeps the boot
 * protocol it declared and still binds.
 */
static void test_boot_mouse_descriptor_read_failure_keeps_boot_protocol(void) {
    struct xhci_hid_device *mouse;

    fixture((uint16_t)sizeof(mouse_report_descriptor));
    build_config(USB_HID_SUBCLASS_BOOT, USB_HID_PROTOCOL_MOUSE,
                 (uint16_t)sizeof(mouse_report_descriptor));
    model.report_descriptor = mouse_report_descriptor;
    model.report_descriptor_length =
        (uint16_t)sizeof(mouse_report_descriptor);
    model.report_descriptor_fail = true;
    CHECK(xhci_controller_start(),
          "the controller starts for the failed optional descriptor read");
    mouse = role_device(XHCI_HID_ROLE_MOUSE, 1U);
    CHECK(xhci_attach_device(mouse, MODEL_PORT, XHCI_SPEED_FULL),
          "a failed optional descriptor read does not unbind the boot mouse");
    CHECK(!mouse->report_mode,
          "the failed probe leaves the pointer on its declared boot protocol");
    CHECK(model.set_protocol_requests == 1U &&
              model.last_set_protocol_value == USB_HID_BOOT_PROTOCOL_VALUE,
          "the fallback boot request is the only protocol request");
}

/*
 * The durable evidence keeps each role's own attempt and the report descriptor
 * the read returned: the keyboard's refusal and the pointer's binding are two
 * records, and the descriptor's bytes, length and read result are exported.
 */
static void test_evidence_keeps_each_role_and_the_descriptor(void) {
    const struct xhci_evidence_controller *controller;
    const struct xhci_evidence_role *keyboard;
    const struct xhci_evidence_role *mouse;
    const struct xhci_evidence_mouse *verdict;
    struct collected out;
    const char *line;
    struct xhci_scan scan;

    fixture((uint16_t)sizeof(mouse_report_descriptor));
    model.report_descriptor = mouse_report_descriptor;
    model.report_descriptor_length =
        (uint16_t)sizeof(mouse_report_descriptor);
    CHECK(xhci_controller_start(), "the controller starts for the evidence");
    xhci_scan_init(&scan, 1U, XHCI_MAX_ATTACH_ATTEMPTS);
    xhci_scan_begin_controller(&scan);
    xhci_probe_ports(&scan);
    xhci_evidence_keep_session(xhci, false);
    xhci_evidence_keep_mouse();
    controller = xhci_evidence_controller(0U);
    CHECK(controller != NULL && controller->role_count == 2U,
          "the evidence keeps one record per role");
    if (controller == NULL) {
        return;
    }
    keyboard = &controller->roles[XHCI_HID_ROLE_KEYBOARD];
    mouse = &controller->roles[XHCI_HID_ROLE_MOUSE];
    CHECK(keyboard->attempted && strcmp(keyboard->stage, "REPORT_DESCRIPTOR") == 0,
          "the keyboard's record names its own stage");
    CHECK(strcmp(keyboard->reason, "NOT_A_KEYBOARD") == 0,
          "the keyboard's reason is its own status");
    CHECK(mouse->attempted && mouse->bound,
          "the pointer's record is a separate, bound attempt");
    CHECK(strcmp(mouse->reason, "OK") == 0 && strcmp(mouse->stage, "BOUND") == 0,
          "the pointer's reason is its own binding, not the keyboard's refusal");
    CHECK(keyboard->report_descriptor_read == XHCI_EVIDENCE_RDESC_READ_OK &&
              mouse->report_descriptor_read == XHCI_EVIDENCE_RDESC_READ_OK,
          "both roles' report-descriptor reads succeeded");
    CHECK(keyboard->report_descriptor_length ==
              (uint16_t)sizeof(mouse_report_descriptor) &&
              mouse->report_descriptor_length ==
                  (uint16_t)sizeof(mouse_report_descriptor),
          "the read length is the descriptor's own length");
    CHECK(keyboard->report_descriptor_bytes[0] == 0x05U &&
              keyboard->report_descriptor_bytes[1] == 0x01U &&
              keyboard->report_descriptor_bytes[2] == 0x09U &&
              keyboard->report_descriptor_bytes[3] == 0x02U,
          "the exported bytes are the descriptor the device sent");
    CHECK(mouse->report_descriptor_parse == XHCI_EVIDENCE_RDESC_PARSE_OK &&
              keyboard->report_descriptor_parse ==
                  XHCI_EVIDENCE_RDESC_PARSE_REFUSED,
          "the classification result is kept per role");
    verdict = xhci_evidence_mouse();
    CHECK(verdict != NULL && verdict->bound && verdict->seen,
          "the pointer verdict is bound");

    memset(&out, 0, sizeof(out));
    xhci_evidence_report(collect_line, &out);
    line = find_line(&out, "[XHCI-INPUT] pci=00:14.0 role=KEYBOARD");
    CHECK(line != NULL && strstr(line, "reason=NOT_A_KEYBOARD") != NULL,
          "the keyboard record is rendered with its own reason");
    line = find_line(&out, "[XHCI-INPUT] pci=00:14.0 role=MOUSE");
    CHECK(line != NULL && strstr(line, "stage=BOUND") != NULL &&
              strstr(line, "reason=OK") != NULL,
          "the pointer record is rendered with its own binding");
    line = find_line(&out, "[XHCI-RDESC] pci=00:14.0 role=MOUSE");
    CHECK(line != NULL && strstr(line, "read=OK") != NULL &&
              strstr(line, "parse=OK") != NULL &&
              strstr(line, "length=52") != NULL &&
              strstr(line, "bytes=05010902") != NULL,
          "the report-descriptor record names its result, length and bytes");
}

/*
 * A device both walks refuse: each role keeps its own status and detail, and
 * the pointer verdict never borrows the keyboard's reason.
 */
static void test_pointer_outcome_is_not_the_keyboards(void) {
    const struct xhci_evidence_controller *controller;
    const struct xhci_evidence_role *keyboard;
    const struct xhci_evidence_role *mouse;
    const struct xhci_evidence_mouse *verdict;
    struct xhci_scan scan;

    fixture((uint16_t)sizeof(other_report_descriptor));
    model.report_descriptor = other_report_descriptor;
    model.report_descriptor_length =
        (uint16_t)sizeof(other_report_descriptor);
    CHECK(xhci_controller_start(), "the controller starts for the refusals");
    xhci_scan_init(&scan, 1U, XHCI_MAX_ATTACH_ATTEMPTS);
    xhci_scan_begin_controller(&scan);
    xhci_probe_ports(&scan);
    xhci_evidence_keep_session(xhci, false);
    xhci_evidence_keep_mouse();
    controller = xhci_evidence_controller(0U);
    CHECK(controller != NULL && controller->role_count == 2U,
          "the refused device keeps two role records");
    if (controller == NULL) {
        return;
    }
    keyboard = &controller->roles[XHCI_HID_ROLE_KEYBOARD];
    mouse = &controller->roles[XHCI_HID_ROLE_MOUSE];
    CHECK(strcmp(keyboard->reason, "NOT_A_KEYBOARD") == 0 &&
              strstr(keyboard->detail, "NOT_KEYBOARD") != NULL,
          "the keyboard refused the descriptor as a keyboard");
    CHECK(strcmp(mouse->reason, "NOT_A_MOUSE") == 0 &&
              strstr(mouse->detail, "NOT_MOUSE") != NULL,
          "the pointer refused the same descriptor as a pointer");
    CHECK(strstr(mouse->detail, "NOT_KEYBOARD") == NULL,
          "the pointer's record never carries the keyboard's reason");
    verdict = xhci_evidence_mouse();
    CHECK(verdict != NULL && verdict->seen && !verdict->bound,
          "the pointer is published as seen, not bound");
    CHECK(strstr(verdict->detail, "NOT_MOUSE") != NULL &&
              strstr(verdict->detail, "NOT_KEYBOARD") == NULL,
          "the pointer verdict names the pointer's own stage");
}

int main(void) {
    test_report_descriptor_reaches_the_parser();
    test_report_descriptor_read_failure_is_not_a_verdict();
    test_second_role_needs_a_real_reset();
    test_set_protocol_fallback_parses_the_role();
    test_boot_mouse_with_wheel_switches_to_report_protocol();
    test_toshiba_16bit_mouse_switches_and_decodes();
    test_boot_mouse_without_wheel_stays_in_boot_protocol();
    test_boot_mouse_descriptor_read_failure_keeps_boot_protocol();
    test_evidence_keeps_each_role_and_the_descriptor();
    test_pointer_outcome_is_not_the_keyboards();
    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    printf("xHCI descriptor handover host test: ALL OK\n");
    return 0;
}
