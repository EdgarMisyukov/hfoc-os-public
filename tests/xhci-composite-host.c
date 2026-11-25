/*
 * ITEM-579 host proof: a composite HID device keeps working across a role
 * re-bind, and a halted endpoint is recovered or says it is lost.
 *
 * The 10231684 hardware run found 04D9:A09F on port 2 with a pointer interface
 * (iface 0, ep81) and a keyboard interface (iface 1, ep82), bound the pointer,
 * and then the silent-candidate retry re-bound the keyboard *by resetting the
 * port*: the pointer's endpoint was left Halted after a USB transaction error,
 * `reports=0`, and the pointer never moved.  A proof that ends at
 * xhci_probe_ports() never runs that retry, so it cannot see the defect.
 *
 * This proof drives the production initialization path end to end - the port
 * walk, the silent keyboard candidate, the pointer's binding, and the
 * xhci_silent_candidate_retry() step that xhci_keyboard_init() runs last - plus
 * the production service pass that decodes reports.  It pins:
 *
 *   * a composite device is one physical path: one slot, one USB address, one
 *     EP0 and one configuration, with per-interface interrupt endpoints;
 *   * re-binding the keyboard on the live pointer's path performs no port
 *     reset and no second Address Device, and the pointer keeps completing and
 *     decoding reports afterwards;
 *   * a separate keyboard on another port binds during the walk, and the
 *     retry never runs (the machine already has a keyboard);
 *   * a Halted endpoint is repaired with Reset Endpoint + Set TR Dequeue
 *     Pointer and the next report decodes - new TRBs and a doorbell alone are
 *     not recovery;
 *   * when that repair fails, the endpoint's readiness is cleared explicitly
 *     instead of pretending the device is alive.
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
    if (count == 0U || (size_t)count * 4096U > sizeof(fake_pool)) {
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
uint64_t timer_cycles_per_second(void) { return 0U; }
uint64_t timer_measured_tick_hz(void) { return 0U; }
uint64_t timer_real_seconds(void) { return 0U; }
void timer_delay_ms(uint32_t milliseconds) { (void)milliseconds; }
uint64_t kernel_loop_iter_us(void) { return 0U; }
uint64_t kernel_loop_iter_max_us(void) { return 0U; }
uint64_t kernel_loop_iteration_count(void) { return 0U; }
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

/* The walk's storage branch links against these; the models here never bind a
   storage device, so each seam refuses by name. */
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
    return "UNSUPPORTED_INTERFACE";
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
    return "UNSUPPORTED";
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
/* The devices the model answers as. */

static const uint8_t device_descriptor[] = {
    0x12U, 0x01U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x08U,
    0xD9U, 0x04U, 0x9FU, 0xA0U, 0x00U, 0x01U,
    0x01U, 0x02U, 0x00U, 0x01U
};

/* 45-byte report-protocol keyboard descriptor: modifiers then six keys. */
static const uint8_t keyboard_report_descriptor[] = {
    0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U,
    0x05U, 0x07U, 0x19U, 0xE0U, 0x29U, 0xE7U,
    0x15U, 0x00U, 0x25U, 0x01U, 0x75U, 0x01U, 0x95U, 0x08U, 0x81U, 0x02U,
    0x95U, 0x01U, 0x75U, 0x08U, 0x81U, 0x03U,
    0x95U, 0x06U, 0x75U, 0x08U, 0x15U, 0x00U, 0x25U, 0x65U,
    0x05U, 0x07U, 0x19U, 0x00U, 0x29U, 0x65U, 0x81U, 0x00U,
    0xC0U
};

/* 52-byte report-protocol mouse descriptor with three buttons, X, Y and the
   wheel - the layout the wheel path is proven on. */
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

/*
 * The composite configuration: interface 0 is a boot mouse (subclass 1,
 * protocol 2, ep81), interface 1 is a report-protocol keyboard (subclass 0,
 * ep82) whose report descriptor is the 45-byte one above.  Two interfaces, so
 * one physical device with two HID roles - the 10231684 device's shape.
 */
#define COMPOSITE_TOTAL 59U
static const uint8_t composite_config[COMPOSITE_TOTAL] = {
    /* configuration */
    0x09U, 0x02U, COMPOSITE_TOTAL, 0x00U, 0x02U, 0x01U, 0x00U, 0xA0U, 0x32U,
    /* interface 0: mouse, boot subclass, protocol 2 */
    0x09U, 0x04U, 0x00U, 0x00U, 0x01U, 0x03U, 0x01U, 0x02U, 0x00U,
    /* HID descriptor, report descriptor length 0 (unused: boot mouse) */
    0x09U, 0x21U, 0x11U, 0x01U, 0x00U, 0x01U, 0x22U, 0x00U, 0x00U,
    /* endpoint 0x81 interrupt 8 bytes, 10 ms */
    0x07U, 0x05U, 0x81U, 0x03U, 0x08U, 0x00U, 0x0AU,
    /* interface 1: keyboard, HID class, no boot subclass */
    0x09U, 0x04U, 0x01U, 0x00U, 0x01U, 0x03U, 0x00U, 0x00U, 0x00U,
    /* HID descriptor, report descriptor length 45 */
    0x09U, 0x21U, 0x11U, 0x01U, 0x00U, 0x01U, 0x22U, 0x2DU, 0x00U,
    /* endpoint 0x82 interrupt 16 bytes, 10 ms */
    0x07U, 0x05U, 0x82U, 0x03U, 0x10U, 0x00U, 0x0AU
};

/* A plain boot keyboard: interface 0, subclass 1, protocol 1, ep81. */
#define BOOT_KEYBOARD_TOTAL 34U
static const uint8_t boot_keyboard_config[BOOT_KEYBOARD_TOTAL] = {
    0x09U, 0x02U, BOOT_KEYBOARD_TOTAL, 0x00U, 0x01U, 0x01U, 0x00U, 0xA0U,
    0x32U,
    0x09U, 0x04U, 0x00U, 0x00U, 0x01U, 0x03U, 0x01U, 0x01U, 0x00U,
    0x09U, 0x21U, 0x11U, 0x01U, 0x00U, 0x01U, 0x22U, 0x08U, 0x00U,
    0x07U, 0x05U, 0x81U, 0x03U, 0x08U, 0x00U, 0x0AU
};

/* A report-protocol pointer: interface 0, subclass 0, protocol 0, ep81. */
#define REPORT_MOUSE_TOTAL 34U
static const uint8_t report_mouse_config[REPORT_MOUSE_TOTAL] = {
    0x09U, 0x02U, REPORT_MOUSE_TOTAL, 0x00U, 0x01U, 0x01U, 0x00U, 0xA0U,
    0x32U,
    0x09U, 0x04U, 0x00U, 0x00U, 0x01U, 0x03U, 0x00U, 0x00U, 0x00U,
    0x09U, 0x21U, 0x11U, 0x01U, 0x00U, 0x01U, 0x22U,
    (uint8_t)sizeof(mouse_report_descriptor), 0x00U,
    0x07U, 0x05U, 0x81U, 0x03U, 0x08U, 0x00U, 0x0AU
};

/* ------------------------------------------------------------------------- */
/* The controller and device model. */

#define MODEL_CAPLENGTH 0x80U
#define MODEL_RT_OFFSET 0x1000U
#define MODEL_DB_OFFSET 0x2000U
#define MODEL_MAX_SLOTS 8U
#define MODEL_MAX_PORTS 8U
#define MODEL_MAX_DCI 32U
#define MODEL_COMPLETION_USB_TRANSACTION_ERROR 4U
#define MODEL_COMPLETION_TRB_ERROR 5U

struct model_device {
    bool present;
    uint16_t vid;
    uint16_t pid;
    const uint8_t *config;
    uint16_t config_length;
    /* Report descriptor per interface number (0..1), NULL when none. */
    const uint8_t *report_descriptor[2];
    uint16_t report_descriptor_length[2];
    /* The device answers one interrupt transfer right after it is armed, which
       is what proves a silent candidate's opposite: a keyboard that types. */
    bool answer_once;
};

struct model_slot {
    bool valid;
    bool stale;       /* the port was reset under it: the device answers nobody */
    uint8_t port;
    uint64_t ep0_ring;
    uint32_t ep0_pos;
    uint64_t ep_ring[MODEL_MAX_DCI];
    uint32_t ep_pos[MODEL_MAX_DCI];
    uint8_t ep_cycle[MODEL_MAX_DCI];
    bool ep_configured[MODEL_MAX_DCI];
    bool ep_halted[MODEL_MAX_DCI];
};

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
    bool addressed[MODEL_MAX_PORTS + 1U];
    uint32_t resets[MODEL_MAX_PORTS + 1U];
    uint32_t command_pos;
    uint32_t event_count;
    uint32_t doorbell_count;
    uint8_t next_slot;
    struct model_slot slots[MODEL_MAX_SLOTS + 1U];
    uint8_t port_slot[MODEL_MAX_PORTS + 1U];
    struct model_device devices[MODEL_MAX_PORTS + 1U];
    /* What the model observed. */
    uint32_t address_ok;
    uint32_t address_failures;
    uint32_t configure_endpoints;
    uint32_t reset_endpoints;
    uint32_t set_tr_dequeues;
    uint32_t disable_slots;
    bool fail_reset_endpoint;
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

static void model_dma_write(uint64_t physical, const void *source,
                            size_t bytes) {
    if (physical < FAKE_POOL_PHYS ||
        physical + bytes > FAKE_POOL_PHYS + sizeof(fake_pool)) {
        return;
    }
    memcpy(fake_pool + model_pool_physical(physical), source, bytes);
}

static uint32_t model_ring_advance(uint32_t position, uint32_t pushes,
                                   uint32_t trb_count) {
    for (uint32_t step = 0U; step < pushes; ++step) {
        position += 1U;
        if (position >= trb_count - 1U) {
            position = 0U;
        }
    }
    return position;
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
                                           uint32_t slot, uint32_t dci) {
    model_emit_event(
        trb_physical, (code << 24) | (transfer_length & 0x00FFFFFFU),
        ((uint32_t)XHCI_TRB_TRANSFER_EVENT << XHCI_TRB_TYPE_SHIFT) |
            (dci << XHCI_TRB_EP_SHIFT) | (slot << XHCI_TRB_SLOT_SHIFT));
}

static uint64_t model_dcbaa_entry(uint32_t slot) {
    uint64_t physical = ((uint64_t)model.dcbaap_high << 32) |
                        (uint64_t)model.dcbaap_low;
    uint64_t entry = 0U;

    model_dma_read(physical + (uint64_t)slot * sizeof(uint64_t), &entry,
                   sizeof(entry));
    return entry;
}

/* The endpoint state the driver reads back from the Device Context. */
static void model_set_endpoint_state(uint32_t slot, uint32_t dci,
                                     uint32_t state) {
    const uint64_t device = model_dcbaa_entry(slot);
    uint32_t dword = 0U;

    if (device == 0U || dci >= MODEL_MAX_DCI) {
        return;
    }
    model_dma_read(device + (uint64_t)dci * xhci->context_bytes, &dword,
                   sizeof(dword));
    dword = (dword & ~0x07U) | (state & 0x07U);
    model_dma_write(device + (uint64_t)dci * xhci->context_bytes, &dword,
                    sizeof(dword));
}

/* A port reset returns the device to Default and invalidates every slot that
   addressed it: the device no longer answers those addresses. */
static void model_reset_port(uint32_t port) {
    model.resets[port] += 1U;
    model.addressed[port] = false;
    model.port_slot[port] = 0U;
    model.portsc[port] |= XHCI_PORTSC_PED | XHCI_PORTSC_PRC;
    for (uint32_t slot = 1U; slot <= MODEL_MAX_SLOTS; ++slot) {
        if (!model.slots[slot].valid || model.slots[slot].port != port) {
            continue;
        }
        model.slots[slot].stale = true;
        for (uint32_t dci = 1U; dci < MODEL_MAX_DCI; ++dci) {
            if (!model.slots[slot].ep_configured[dci]) {
                continue;
            }
            model.slots[slot].ep_halted[dci] = true;
            model_set_endpoint_state(slot, dci, 2U); /* Halted */
        }
    }
}

static bool model_slot_usable(uint32_t slot, uint32_t dci) {
    if (slot == 0U || slot > MODEL_MAX_SLOTS || dci >= MODEL_MAX_DCI) {
        return false;
    }
    if (!model.slots[slot].valid || model.slots[slot].stale) {
        return false;
    }
    return model.slots[slot].ep_configured[dci] &&
           !model.slots[slot].ep_halted[dci] &&
           model.slots[slot].ep_ring[dci] != 0U;
}

/* One armed interrupt TRB of the slot/endpoint: its physical address, buffer
   and length.  The consumer position is the model's own, exactly as the
   controller's dequeue would be. */
static bool model_next_interrupt_trb(uint32_t slot, uint32_t dci,
                                     uint64_t *trb_physical, uint64_t *buffer,
                                     uint32_t *length) {
    struct model_slot *record;
    uint32_t position;
    uint32_t trb[4];

    if (slot == 0U || slot > MODEL_MAX_SLOTS || dci >= MODEL_MAX_DCI) {
        return false;
    }
    record = &model.slots[slot];
    if (record->ep_ring[dci] == 0U) {
        return false;
    }
    position = record->ep_pos[dci];
    for (uint32_t guard = 0U; guard < XHCI_INTERRUPT_RING_TRBS; ++guard) {
        if (position == XHCI_INTERRUPT_RING_TRBS - 1U) {
            position = 0U;
            record->ep_cycle[dci] = (uint8_t)(record->ep_cycle[dci] ^ 1U);
            continue;
        }
        model_dma_read(record->ep_ring[dci] +
                           (uint64_t)position * XHCI_TRB_BYTES,
                       trb, sizeof(trb));
        if ((trb[3] & XHCI_TRB_TYPE_MASK) ==
            ((uint32_t)XHCI_TRB_NORMAL << XHCI_TRB_TYPE_SHIFT)) {
            *trb_physical = record->ep_ring[dci] +
                            (uint64_t)position * XHCI_TRB_BYTES;
            *buffer = ((uint64_t)trb[1] << 32) | (uint64_t)trb[0];
            *length = trb[2] & 0x1FFFFU;
            record->ep_pos[dci] = model_ring_advance(
                position, 1U, XHCI_INTERRUPT_RING_TRBS);
            return true;
        }
        position = model_ring_advance(position, 1U,
                                      XHCI_INTERRUPT_RING_TRBS);
    }
    return false;
}

/*
 * Deliver one report to a slot's interrupt endpoint, if the device answers that
 * slot at all.  A reset-under-the-device slot gets a USB transaction error and
 * halts the endpoint - the exact 10231684 failure.
 */
static bool model_deliver_report(uint32_t slot, uint32_t dci,
                                 const uint8_t *report, uint32_t length) {
    uint64_t trb_physical = 0U;
    uint64_t buffer = 0U;
    uint32_t trb_length = 0U;

    if (!model_next_interrupt_trb(slot, dci, &trb_physical, &buffer,
                                  &trb_length)) {
        return false;
    }
    if (!model_slot_usable(slot, dci)) {
        model.slots[slot].ep_halted[dci] = true;
        model_set_endpoint_state(slot, dci, 2U); /* Halted */
        model_emit_transfer_completion(trb_physical,
                                       MODEL_COMPLETION_USB_TRANSACTION_ERROR,
                                       trb_length, slot, dci);
        return true;
    }
    if (length > trb_length) {
        length = trb_length;
    }
    if (length != 0U) {
        model_dma_write(buffer, report, length);
    }
    model_emit_transfer_completion(trb_physical, XHCI_COMPLETION_SUCCESS,
                                   trb_length - length, slot, dci);
    return true;
}

/* The endpoint state test hook: mark one endpoint Halted the way a device that
   stopped answering leaves it. */
static void model_halt(uint32_t slot, uint32_t dci) {
    if (slot == 0U || slot > MODEL_MAX_SLOTS || dci >= MODEL_MAX_DCI) {
        return;
    }
    model.slots[slot].ep_halted[dci] = true;
    model_set_endpoint_state(slot, dci, 2U);
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
        model.port_slot[port] = (uint8_t)slot;
        memset(&model.slots[slot], 0, sizeof(model.slots[slot]));
        model.slots[slot].valid = true;
        model.slots[slot].port = port;
        model.slots[slot].ep0_ring = ep0_ring;
        model.slots[slot].ep0_pos = 0U;
        ++model.address_ok;
    }
    model_emit_command_completion(trb_physical, code, 0U, slot);
}

static void model_complete_configure_endpoint(const uint32_t trb[4],
                                              uint64_t trb_physical) {
    const uint64_t input = ((uint64_t)trb[1] << 32) | (uint64_t)trb[0];
    const uint32_t slot = (trb[3] >> XHCI_TRB_SLOT_SHIFT) & 0xFFU;
    uint32_t add_flags = 0U;

    (void)model_dma_read(input + 4U, &add_flags, sizeof(add_flags));
    ++model.configure_endpoints;
    if (slot != 0U && slot <= MODEL_MAX_SLOTS) {
        for (uint32_t dci = 1U; dci < MODEL_MAX_DCI; ++dci) {
            uint64_t ring = 0U;

            if ((add_flags & (1U << dci)) == 0U) {
                continue;
            }
            (void)model_dma_read(
                input + ((uint64_t)1U + dci) * xhci->context_bytes + 8U,
                &ring, sizeof(ring));
            ring &= ~(uint64_t)0x0FU;
            if (dci == 1U) {
                model.slots[slot].ep0_ring = ring;
                continue;
            }
            model.slots[slot].ep_ring[dci] = ring;
            model.slots[slot].ep_pos[dci] = 0U;
            model.slots[slot].ep_cycle[dci] = 1U;
            model.slots[slot].ep_configured[dci] = true;
            model.slots[slot].ep_halted[dci] = false;
            model_set_endpoint_state(slot, dci, 1U); /* Running */
        }
    }
    model_emit_command_completion(trb_physical, XHCI_COMPLETION_SUCCESS, 0U,
                                  slot);
}

static void model_complete_reset_endpoint(const uint32_t trb[4],
                                          uint64_t trb_physical) {
    const uint32_t slot = (trb[3] >> XHCI_TRB_SLOT_SHIFT) & 0xFFU;
    const uint32_t dci = (trb[2] >> 16U) & 0x1FU;

    ++model.reset_endpoints;
    if (model.fail_reset_endpoint || slot == 0U ||
        slot > MODEL_MAX_SLOTS || dci >= MODEL_MAX_DCI) {
        model_emit_command_completion(trb_physical,
                                      MODEL_COMPLETION_TRB_ERROR, 0U, slot);
        return;
    }
    model.slots[slot].ep_halted[dci] = false;
    model_set_endpoint_state(slot, dci, 3U); /* Stopped */
    model_emit_command_completion(trb_physical, XHCI_COMPLETION_SUCCESS, 0U,
                                  slot);
}

static void model_complete_set_tr_dequeue(const uint32_t trb[4],
                                          uint64_t trb_physical) {
    const uint32_t slot = (trb[3] >> XHCI_TRB_SLOT_SHIFT) & 0xFFU;
    const uint32_t dci = (trb[2] >> 16U) & 0x1FU;
    uint64_t pointer = ((uint64_t)trb[1] << 32) | (uint64_t)trb[0];

    ++model.set_tr_dequeues;
    if (slot == 0U || slot > MODEL_MAX_SLOTS || dci >= MODEL_MAX_DCI) {
        model_emit_command_completion(trb_physical,
                                      MODEL_COMPLETION_TRB_ERROR, 0U, slot);
        return;
    }
    model.slots[slot].ep_ring[dci] = pointer & ~(uint64_t)0x0FU;
    model.slots[slot].ep_pos[dci] = 0U;
    model.slots[slot].ep_cycle[dci] = (uint8_t)(pointer & 1U);
    model.slots[slot].ep_configured[dci] = true;
    model.slots[slot].ep_halted[dci] = false;
    model_set_endpoint_state(slot, dci, 1U);
    model_emit_command_completion(trb_physical, XHCI_COMPLETION_SUCCESS, 0U,
                                  slot);
}

static void model_complete_command(void) {
    const uint64_t ring = FAKE_POOL_PHYS + XHCI_OFF_COMMAND_RING;
    const uint64_t trb_physical =
        ring + (uint64_t)model.command_pos * XHCI_TRB_BYTES;
    uint32_t trb[4] = {0U, 0U, 0U, 0U};
    uint32_t type;

    model_dma_read(trb_physical, trb, sizeof(trb));
    type = (trb[3] & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
    model.command_pos = model_ring_advance(model.command_pos, 1U,
                                           XHCI_RING_TRBS);
    switch (type) {
    case XHCI_TRB_ENABLE_SLOT:
        model.next_slot = (uint8_t)(model.next_slot % MODEL_MAX_SLOTS + 1U);
        model_emit_command_completion(trb_physical, XHCI_COMPLETION_SUCCESS,
                                      model.next_slot, model.next_slot);
        return;
    case XHCI_TRB_ADDRESS_DEVICE:
        model_complete_address_device(trb, trb_physical);
        return;
    case XHCI_TRB_CONFIGURE_ENDPOINT:
        model_complete_configure_endpoint(trb, trb_physical);
        return;
    case XHCI_TRB_RESET_ENDPOINT:
        model_complete_reset_endpoint(trb, trb_physical);
        return;
    case XHCI_TRB_SET_TR_DEQUEUE:
        model_complete_set_tr_dequeue(trb, trb_physical);
        return;
    case XHCI_TRB_DISABLE_SLOT: {
        const uint32_t slot = (trb[3] >> XHCI_TRB_SLOT_SHIFT) & 0xFFU;

        ++model.disable_slots;
        if (slot != 0U && slot <= MODEL_MAX_SLOTS) {
            model.slots[slot].valid = false;
            for (uint32_t port = 1U; port <= MODEL_MAX_PORTS; ++port) {
                if (model.port_slot[port] == slot) {
                    model.port_slot[port] = 0U;
                }
            }
        }
        model_emit_command_completion(trb_physical, XHCI_COMPLETION_SUCCESS,
                                      0U, slot);
        return;
    }
    default:
        model_emit_command_completion(trb_physical, XHCI_COMPLETION_SUCCESS,
                                      0U,
                                      (trb[3] >> XHCI_TRB_SLOT_SHIFT) &
                                          0xFFU);
        return;
    }
}

static void model_control_transfer(uint32_t slot) {
    struct model_slot *record;
    uint64_t ring;
    uint32_t position;
    uint32_t setup[4];
    uint32_t data[4];
    uint32_t status[4];
    uint32_t data_length;
    uint64_t data_trb_physical = 0U;
    uint64_t data_buffer = 0U;
    uint64_t status_physical;
    uint8_t request_type;
    uint8_t request;
    uint8_t type;
    uint8_t status_code = XHCI_COMPLETION_SUCCESS;
    const uint8_t *source = NULL;
    uint32_t source_length = 0U;
    struct model_device *device;

    if (slot == 0U || slot > MODEL_MAX_SLOTS) {
        return;
    }
    record = &model.slots[slot];
    if (record->ep0_ring == 0U || record->stale) {
        return;
    }
    ring = record->ep0_ring;
    position = record->ep0_pos;
    model_dma_read(ring + (uint64_t)position * XHCI_TRB_BYTES, setup,
                   sizeof(setup));
    position = model_ring_advance(position, 1U, XHCI_RING_TRBS);
    data_length = (setup[1] >> 16) & 0xFFFFU;
    if (data_length != 0U) {
        model_dma_read(ring + (uint64_t)position * XHCI_TRB_BYTES, data,
                       sizeof(data));
        data_trb_physical = ring + (uint64_t)position * XHCI_TRB_BYTES;
        data_buffer = ((uint64_t)data[1] << 32) | (uint64_t)data[0];
        position = model_ring_advance(position, 1U, XHCI_RING_TRBS);
    }
    model_dma_read(ring + (uint64_t)position * XHCI_TRB_BYTES, status,
                   sizeof(status));
    status_physical = ring + (uint64_t)position * XHCI_TRB_BYTES;
    record->ep0_pos = model_ring_advance(position, 1U, XHCI_RING_TRBS);

    request_type = (uint8_t)(setup[0] & 0xFFU);
    request = (uint8_t)(setup[0] >> 8);
    type = (uint8_t)(((setup[0] >> 16) & 0xFF00U) >> 8);
    device = &model.devices[record->port];
    if (request == USB_REQUEST_GET_DESCRIPTOR && (request_type & 0x80U) != 0U) {
        const uint8_t interface = (uint8_t)(setup[1] & 0xFFU);

        if (type == USB_DESCRIPTOR_DEVICE) {
            source = device_descriptor;
            source_length = (uint32_t)sizeof(device_descriptor);
        } else if (type == USB_DESCRIPTOR_CONFIGURATION) {
            source = device->config;
            source_length = device->config_length;
        } else if (type == USB_DESCRIPTOR_HID_REPORT &&
                   interface < 2U) {
            source = device->report_descriptor[interface];
            source_length = device->report_descriptor_length[interface];
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
                                       data_length - copied, slot, 1U);
    }
    if (request == USB_REQUEST_SET_CONFIGURATION && record->port != 0U) {
        /* The configuration is accepted; the model keeps answering. */
    }
    model_emit_transfer_completion(status_physical, status_code, 0U, slot,
                                   1U);
}

/* A device that answers once after it is armed: the keyboard's idle report. */
static void model_maybe_auto_report(uint32_t slot, uint32_t dci) {
    struct model_slot *record;
    struct model_device *device;
    uint64_t trb_physical = 0U;
    uint64_t buffer = 0U;
    uint32_t length = 0U;
    uint8_t report[8];

    if (slot == 0U || slot > MODEL_MAX_SLOTS) {
        return;
    }
    record = &model.slots[slot];
    device = &model.devices[record->port];
    if (!device->present || !device->answer_once ||
        !record->ep_configured[dci] || record->ep_halted[dci]) {
        return;
    }
    if (!model_next_interrupt_trb(slot, dci, &trb_physical, &buffer,
                                  &length)) {
        return;
    }
    device->answer_once = false;
    memset(report, 0, sizeof(report));
    if (length < sizeof(report)) {
        memset(report, 0, length);
    }
    if (length != 0U) {
        model_dma_write(buffer, report, length);
    }
    model_emit_transfer_completion(trb_physical, XHCI_COMPLETION_SUCCESS, 0U,
                                   slot, dci);
}

uint32_t xhci_host_mmio_read32(const volatile uint8_t *base, uint32_t offset) {
    const size_t address = model_address(base, offset);

    if (address == 0x00U) {
        return 0x01000080U;
    }
    if (address == XHCI_CAP_HCSPARAMS1) {
        return MODEL_MAX_SLOTS | (MODEL_MAX_PORTS << 24);
    }
    if (address == XHCI_CAP_HCSPARAMS2) {
        return 4U << 4U;
    }
    if (address == XHCI_CAP_HCCPARAMS1) {
        return 0U;
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
        return 0x1U;
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
            for (uint32_t port = 1U; port <= MODEL_MAX_PORTS; ++port) {
                model.addressed[port] = false;
                model.portsc[port] &= ~(uint32_t)XHCI_PORTSC_PED;
                model.port_slot[port] = 0U;
            }
            memset(model.slots, 0, sizeof(model.slots));
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
            model_reset_port(port);
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
            return;
        }
        model_maybe_auto_report((uint32_t)index, value & 0xFFU);
        return;
    }
}

/* ------------------------------------------------------------------------- */
/* Fixtures. */

static void model_reset(void) {
    memset(&model, 0, sizeof(model));
    memset(registers, 0, sizeof(registers));
    memset(fake_pool, 0xAA, sizeof(fake_pool));
    for (uint32_t port = 1U; port <= MODEL_MAX_PORTS; ++port) {
        model.portsc[port] = 0x000002A0U;
    }
}

static void model_attach_device(uint32_t port, uint16_t vid, uint16_t pid,
                                const uint8_t *config, uint16_t config_length,
                                bool answer_once) {
    struct model_device *device = &model.devices[port];

    model.portsc[port] = 0x000006A1U; /* PP, CCS, full speed */
    device->present = true;
    device->vid = vid;
    device->pid = pid;
    device->config = config;
    device->config_length = config_length;
    device->answer_once = answer_once;
}

static void model_composite_device(uint32_t port) {
    model_attach_device(port, 0x04D9U, 0xA09FU, composite_config,
                        COMPOSITE_TOTAL, false);
    model.devices[port].report_descriptor[0] = NULL;
    model.devices[port].report_descriptor_length[0] = 0U;
    model.devices[port].report_descriptor[1] = keyboard_report_descriptor;
    model.devices[port].report_descriptor_length[1] =
        (uint16_t)sizeof(keyboard_report_descriptor);
}

static void model_boot_keyboard(uint32_t port) {
    model_attach_device(port, 0x046DU, 0xC31CU, boot_keyboard_config,
                        BOOT_KEYBOARD_TOTAL, true);
}

static void model_report_mouse(uint32_t port) {
    model_attach_device(port, 0x04D9U, 0xA09FU, report_mouse_config,
                        REPORT_MOUSE_TOTAL, false);
    model.devices[port].report_descriptor[0] = mouse_report_descriptor;
    model.devices[port].report_descriptor_length[0] =
        (uint16_t)sizeof(mouse_report_descriptor);
}

static void fixture(void) {
    model_reset();
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
    /* The service pass serves a machine that finished its probe; the walk sets
       this in xhci_keyboard_init(), which this proof drives step by step. */
    xhci_machine.initialized = true;
    xhci_evidence_reset();
}

static void walk(void) {
    struct xhci_scan scan;

    CHECK(xhci_controller_start(), "the controller starts for the walk");
    xhci_scan_init(&scan, 1U, XHCI_MAX_ATTACH_ATTEMPTS);
    xhci_scan_begin_controller(&scan);
    xhci_probe_ports(&scan);
}

static void service(void) {
    uint32_t guard = 0U;

    /* Drain the event ring the way the main loop does, a bounded number of
       times: an answering device re-arms on every completion. */
    while (guard < 64U) {
        xhci_keyboard_service();
        ++guard;
    }
}

struct diag_lines {
    char lines[96][512];
    size_t count;
};

static void diag_collect(void *context, const char *line) {
    struct diag_lines *out = context;

    if (out->count < sizeof(out->lines) / sizeof(out->lines[0])) {
        snprintf(out->lines[out->count], sizeof(out->lines[0]), "%s", line);
        out->count += 1U;
    }
}

static const char *diag_find(const struct diag_lines *out, const char *needle) {
    for (size_t index = 0U; index < out->count; ++index) {
        if (strstr(out->lines[index], needle) != NULL) {
            return out->lines[index];
        }
    }
    return NULL;
}

static uint32_t diag_field(const char *line, const char *key) {
    const char *at;

    if (line == NULL || key == NULL) {
        return 0xFFFFFFFFU;
    }
    at = strstr(line, key);
    if (at == NULL) {
        return 0xFFFFFFFFU;
    }
    return (uint32_t)strtoul(at + strlen(key), NULL, 10);
}

static uint32_t mouse_reports(void) { return xhci_mouse_report_count(); }
static uint32_t mouse_moves(void) { return xhci_mouse_move_count(); }
static uint32_t mouse_presses(void) { return xhci_mouse_press_count(); }

/* ------------------------------------------------------------------------- */
/* Tests. */

/*
 * The full initialization path, including the last step: a composite device's
 * keyboard interface is a silent candidate, the pointer binds, and the retry
 * re-binds the keyboard.  The pointer must keep completing and decoding
 * reports, on the same slot the keyboard just joined.
 */
static void test_composite_retry_keeps_the_mouse(void) {
    struct usb_hid_mouse_event event;

    fixture();
    model_composite_device(2U);
    walk();

    CHECK(xhci_machine.silent_candidate.valid,
          "the composite keyboard interface is the silent candidate");
    CHECK(xhci_machine.mouse_session == xhci && xhci_machine.keyboard_session == NULL,
          "the walk binds the pointer and leaves the keyboard to the retry");
    CHECK(xhci->devices[XHCI_HID_ROLE_MOUSE].ready,
          "the pointer is bound after the walk");

    /* One report through the production service pass, before the retry. */
    {
        static const uint8_t report[4] = {0x01U, 0x05U, 0xFBU, 0x00U};
        const uint32_t slot = xhci->devices[XHCI_HID_ROLE_MOUSE].slot_id;
        const uint32_t dci = xhci->devices[XHCI_HID_ROLE_MOUSE].dci;

        CHECK(model_deliver_report(slot, dci, report, sizeof(report)),
              "the model delivered a pointer report to the armed endpoint");
        service();
        CHECK(mouse_reports() == 1U,
              "the pointer decoded the report before the retry");
        CHECK(mouse_presses() == 1U, "the button press was decoded");
        CHECK(xhci_mouse_pop_event(&event), "a pointer event was queued");
        CHECK(event.dx == 5 && event.dy == -5 && event.wheel == 0,
              "the event carries the movement the report carried");
    }

    /* The retry the walk never sees: the last step of the initialization. */
    xhci_silent_candidate_retry(&(struct xhci_scan){0}, 0U);

    CHECK(xhci_machine.keyboard_session == xhci,
          "the retry binds the keyboard in the pointer's session");
    CHECK(xhci->devices[XHCI_HID_ROLE_KEYBOARD].ready,
          "the keyboard role is bound after the retry");
    CHECK(xhci->devices[XHCI_HID_ROLE_KEYBOARD].slot_id ==
              xhci->devices[XHCI_HID_ROLE_MOUSE].slot_id,
          "both roles of the composite device share one slot");
    CHECK(xhci->devices[XHCI_HID_ROLE_KEYBOARD].dci !=
              xhci->devices[XHCI_HID_ROLE_MOUSE].dci,
          "each role keeps its own interrupt endpoint");
    CHECK(model.resets[2] == 2U,
          "the retry performed no port reset on the live pointer's path");
    CHECK(model.address_ok == 2U,
          "the retry performed no second Address Device");

    /* The pointer keeps working after the keyboard joined. */
    {
        static const uint8_t report[4] = {0x00U, 0x01U, 0x02U, 0x00U};
        const uint32_t slot = xhci->devices[XHCI_HID_ROLE_MOUSE].slot_id;
        const uint32_t dci = xhci->devices[XHCI_HID_ROLE_MOUSE].dci;

        CHECK(model_deliver_report(slot, dci, report, sizeof(report)),
              "the model delivered a second report after the retry");
        service();
        CHECK(mouse_reports() == 2U,
              "the pointer decoded a report after the keyboard re-bind");
        CHECK(mouse_moves() == 2U, "both movements were counted");
        CHECK(xhci_mouse_pop_event(&event), "the second event was queued");
        CHECK(event.dx == 1 && event.dy == 2,
              "the second event carries the second movement");
    }
}

/*
 * A separate keyboard on another port: the walk keeps searching after the
 * silent candidate and binds it, so the retry does not run and the pointer on
 * the first port is not touched.
 */
static void test_separate_keyboard_after_silent_candidate(void) {
    struct usb_hid_mouse_event event;

    fixture();
    model_composite_device(2U);
    model_boot_keyboard(3U);
    walk();

    CHECK(xhci->devices[XHCI_HID_ROLE_KEYBOARD].ready &&
              xhci->devices[XHCI_HID_ROLE_KEYBOARD].port == 3U,
          "the separate keyboard on the other port is bound");
    CHECK(xhci_machine.keyboard_session == xhci,
          "the walk keeps searching after the silent candidate");
    CHECK(xhci->devices[XHCI_HID_ROLE_MOUSE].ready, "the pointer stays bound");
    CHECK(xhci->devices[XHCI_HID_ROLE_KEYBOARD].slot_id !=
              xhci->devices[XHCI_HID_ROLE_MOUSE].slot_id,
          "the two physical devices have their own slots");

    /* The retry is skipped: the machine already has a keyboard. */
    service();
    xhci_silent_candidate_retry(&(struct xhci_scan){0}, 0U);
    CHECK(xhci_machine.keyboard_session == xhci &&
              xhci->devices[XHCI_HID_ROLE_KEYBOARD].port == 3U,
          "the retry does not replace the bound keyboard");
    CHECK(model.resets[2] == 2U,
          "the retry never resets the pointer's port");

    {
        static const uint8_t report[4] = {0x00U, 0x03U, 0x00U, 0x00U};
        const uint32_t slot = xhci->devices[XHCI_HID_ROLE_MOUSE].slot_id;
        const uint32_t dci = xhci->devices[XHCI_HID_ROLE_MOUSE].dci;

        CHECK(model_deliver_report(slot, dci, report, sizeof(report)),
              "the pointer's endpoint is still armed on its own port");
        service();
        CHECK(mouse_reports() == 1U,
              "the pointer decodes while the separate keyboard is bound");
        CHECK(xhci_mouse_pop_event(&event) && event.dx == 3,
              "the pointer event carries the movement");
    }
}

/*
 * A Halted endpoint after a USB transaction error is not repaired by arming
 * more TRBs: the endpoint is reset and its dequeue pointer re-established, and
 * the next report decodes.
 */
static void test_halted_endpoint_is_recovered(void) {
    static const uint8_t report[4] = {0x02U, 0x04U, 0x00U, 0x00U};
    uint32_t slot;
    uint32_t dci;

    fixture();
    model_composite_device(2U);
    walk();
    CHECK(xhci->devices[XHCI_HID_ROLE_MOUSE].ready,
          "the pointer is bound for the halt proof");
    slot = xhci->devices[XHCI_HID_ROLE_MOUSE].slot_id;
    dci = xhci->devices[XHCI_HID_ROLE_MOUSE].dci;

    model_halt(slot, dci);
    CHECK(model_deliver_report(slot, dci, report, sizeof(report)),
          "the halted endpoint produced an error completion");
    service();
    CHECK(model.reset_endpoints == 1U,
          "the driver reset the halted endpoint");
    CHECK(model.set_tr_dequeues == 1U,
          "the driver re-established the dequeue pointer");
    CHECK(mouse_reports() == 0U,
          "the failed transfer was not decoded as a report");
    CHECK(xhci->devices[XHCI_HID_ROLE_MOUSE].ready,
          "a recovered endpoint keeps the pointer ready");

    CHECK(model_deliver_report(slot, dci, report, sizeof(report)),
          "the model delivered a report on the recovered endpoint");
    service();
    CHECK(mouse_reports() == 1U,
          "the next report decodes after the endpoint recovery");
}

/*
 * If the endpoint cannot be reset, the driver must take the role down instead
 * of keeping a readiness it cannot honour.
 */
static void test_failed_endpoint_reset_clears_readiness(void) {
    static const uint8_t report[4] = {0x00U, 0x00U, 0x00U, 0x00U};
    uint32_t slot;
    uint32_t dci;

    fixture();
    model_composite_device(2U);
    walk();
    CHECK(xhci->devices[XHCI_HID_ROLE_MOUSE].ready,
          "the pointer is bound for the failed-recovery proof");
    slot = xhci->devices[XHCI_HID_ROLE_MOUSE].slot_id;
    dci = xhci->devices[XHCI_HID_ROLE_MOUSE].dci;

    model.fail_reset_endpoint = true;
    model_halt(slot, dci);
    CHECK(model_deliver_report(slot, dci, report, sizeof(report)),
          "the halted endpoint produced an error completion");
    service();
    CHECK(model.reset_endpoints == 1U,
          "the driver attempted the endpoint reset");
    CHECK(!xhci->devices[XHCI_HID_ROLE_MOUSE].ready,
          "a failed reset takes the pointer out of readiness");
    CHECK(!xhci_mouse_attached(),
          "the machine sees the pointer as no longer attached");
}

/*
 * When the per-endpoint repair fails on a device that another role is using,
 * the physical reset stops and restores every role of the device - the pointer
 * must not be left on a slot that no longer answers.
 */
static void test_shared_device_repair_failure_restores_all_roles(void) {
    static const uint8_t report[4] = {0x01U, 0x07U, 0x01U, 0x00U};
    uint32_t slot;
    uint32_t dci;

    fixture();
    model_composite_device(2U);
    walk();
    xhci_silent_candidate_retry(&(struct xhci_scan){0}, 0U);
    CHECK(xhci->devices[XHCI_HID_ROLE_KEYBOARD].ready &&
              xhci->devices[XHCI_HID_ROLE_MOUSE].ready,
          "both composite roles are live for the restore proof");
    slot = xhci->devices[XHCI_HID_ROLE_MOUSE].slot_id;
    dci = xhci->devices[XHCI_HID_ROLE_MOUSE].dci;

    model.fail_reset_endpoint = true;
    model_halt(slot, dci);
    CHECK(model_deliver_report(slot, dci, report, sizeof(report)),
          "the halted endpoint produced an error completion");
    service();
    CHECK(model.reset_endpoints == 1U,
          "the per-endpoint repair was attempted first");
    CHECK(xhci->devices[XHCI_HID_ROLE_KEYBOARD].ready &&
              xhci->devices[XHCI_HID_ROLE_MOUSE].ready,
          "a physical reset restores every role of the device");
    CHECK(model.resets[2] == 3U, "the restore reset the port once");
    CHECK(model.address_ok == 3U,
          "the restore re-established the device once and the second role joined it");
    CHECK(xhci->devices[XHCI_HID_ROLE_KEYBOARD].slot_id ==
              xhci->devices[XHCI_HID_ROLE_MOUSE].slot_id,
          "both roles share the restored path's slot");

    model.fail_reset_endpoint = false;
    slot = xhci->devices[XHCI_HID_ROLE_MOUSE].slot_id;
    dci = xhci->devices[XHCI_HID_ROLE_MOUSE].dci;
    CHECK(model_deliver_report(slot, dci, report, sizeof(report)),
          "the restored pointer endpoint accepts a report");
    service();
    CHECK(mouse_reports() == 1U,
          "the pointer decodes after the device-wide restore");
}

/*
 * The report-protocol pointer path: a 4-byte report (buttons, X, Y, wheel)
 * decoded from the descriptor the device published, through the production
 * service pass and out as a pointer event.
 */
static void test_report_protocol_mouse_decodes_the_wheel(void) {
    struct usb_hid_mouse_event event;
    static const uint8_t report[4] = {0x01U, 0x00U, 0x02U, 0x01U};

    fixture();
    model_report_mouse(2U);
    walk();
    CHECK(xhci->devices[XHCI_HID_ROLE_MOUSE].ready,
          "the report-protocol pointer is bound");
    CHECK(xhci->devices[XHCI_HID_ROLE_MOUSE].report_mode,
          "the pointer was bound through its report descriptor");

    {
        const uint32_t slot = xhci->devices[XHCI_HID_ROLE_MOUSE].slot_id;
        const uint32_t dci = xhci->devices[XHCI_HID_ROLE_MOUSE].dci;

        CHECK(model_deliver_report(slot, dci, report, sizeof(report)),
              "the model delivered the report-protocol report");
        service();
    }
    CHECK(mouse_reports() == 1U, "the report-protocol report decoded");
    CHECK(xhci_mouse_pop_event(&event), "a pointer event was queued");
    CHECK(event.dx == 0 && event.dy == 2 && event.wheel == 1,
          "the event carries movement and the wheel");
}

/*
 * ITEM-580: one export snapshot, agreed by all three record families.
 *
 * The kept evidence used to be frozen at initialization, so a pointer that had
 * since decoded reports was published as `completions=0 reports=0
 * epstate=DISABLED` beside a live `[INPUT-DIAG]`.  This walks the production
 * path - initial keep, then HID reports, then two exports - and requires the
 * same submitted/completions/reports from `[INPUT-DIAG]`, `[XHCI-INPUT]` and
 * `[XHCI-MOUSE]` in both exports, with the second attempt carrying the newer
 * values.  submitted != completions is the shape that separates the two
 * counters.
 */
static void test_export_snapshot_is_live_and_consistent(void) {
    static const uint8_t report[4] = {0x01U, 0x02U, 0xFBU, 0x01U};
    struct diag_lines first;
    struct diag_lines second;
    const char *input_diag;
    const char *evidence;
    const char *mouse_line;
    uint32_t submitted1;
    uint32_t completions1;
    uint32_t reports1;
    uint32_t slot;
    uint32_t dci;

    fixture();
    model_composite_device(2U);
    walk();
    /* The end-of-visit keep: the state the report would have been frozen at. */
    xhci_evidence_keep_session(xhci, false);
    xhci_silent_candidate_retry(&(struct xhci_scan){0}, 0U);
    CHECK(xhci->devices[XHCI_HID_ROLE_KEYBOARD].ready &&
              xhci->devices[XHCI_HID_ROLE_MOUSE].ready,
          "both roles are live for the export-snapshot proof");
    slot = xhci->devices[XHCI_HID_ROLE_MOUSE].slot_id;
    dci = xhci->devices[XHCI_HID_ROLE_MOUSE].dci;

    {
        /* Two reports through the production service pass. */
        for (uint32_t index = 0U; index < 2U; ++index) {
            CHECK(model_deliver_report(slot, dci, report, sizeof(report)),
                  "the model delivered a pointer report");
            service();
        }
    }
    CHECK(mouse_reports() == 2U, "the pointer decoded two reports");

    memset(&first, 0, sizeof(first));
    xhci_diagnostics_report(diag_collect, &first);
    input_diag = diag_find(&first, "[INPUT-DIAG] role=mouse");
    evidence = diag_find(&first, "[XHCI-INPUT] pci=00:14.0 role=MOUSE");
    mouse_line = diag_find(&first, "[XHCI-MOUSE] ");
    CHECK(input_diag != NULL && evidence != NULL && mouse_line != NULL,
          "the export carries all three input records");
    if (input_diag == NULL || evidence == NULL || mouse_line == NULL) {
        return;
    }
    CHECK(strstr(input_diag, " completed=") == NULL &&
              strstr(input_diag, " submitted=") != NULL &&
              strstr(input_diag, " completions=") != NULL,
          "the role line names submitted and completions, not completed");
    submitted1 = diag_field(input_diag, " submitted=");
    completions1 = diag_field(input_diag, " completions=");
    reports1 = diag_field(input_diag, " reports=");
    CHECK(submitted1 != 0xFFFFFFFFU && completions1 != 0xFFFFFFFFU &&
              reports1 != 0xFFFFFFFFU,
          "the live role line carries the three counters");
    CHECK(diag_field(evidence, " submitted=") == submitted1 &&
              diag_field(evidence, " completions=") == completions1 &&
              diag_field(evidence, " reports=") == reports1,
          "the kept role record agrees with the live role line");
    CHECK(diag_field(mouse_line, " submitted=") == submitted1 &&
              diag_field(mouse_line, " completions=") == completions1 &&
              diag_field(mouse_line, " reports=") == reports1,
          "the pointer record agrees with the live role line");
    CHECK(reports1 == 2U && completions1 >= 2U && submitted1 > completions1,
          "the export shows submitted greater than completions and both real");
    CHECK(strstr(evidence, "epstate=RUNNING") != NULL,
          "the kept role record names the running endpoint, not DISABLED");
    CHECK(strstr(evidence, "state=BOUND") != NULL,
          "the kept role record still says the role is bound");

    {
        /* A third report, then a second export: one command, new values. */
        CHECK(model_deliver_report(slot, dci, report, sizeof(report)),
              "the model delivered the third report");
        service();
    }
    CHECK(mouse_reports() == 3U, "the pointer decoded the third report");
    memset(&second, 0, sizeof(second));
    xhci_diagnostics_report(diag_collect, &second);
    input_diag = diag_find(&second, "[INPUT-DIAG] role=mouse");
    evidence = diag_find(&second, "[XHCI-INPUT] pci=00:14.0 role=MOUSE");
    mouse_line = diag_find(&second, "[XHCI-MOUSE] ");
    CHECK(input_diag != NULL && evidence != NULL && mouse_line != NULL,
          "the second export carries all three input records");
    if (input_diag == NULL || evidence == NULL || mouse_line == NULL) {
        return;
    }
    CHECK(diag_field(input_diag, " reports=") == 3U &&
              diag_field(input_diag, " reports=") > reports1,
          "the second export carries the newer report count");
    CHECK(diag_field(evidence, " submitted=") ==
              diag_field(input_diag, " submitted=") &&
              diag_field(evidence, " completions=") ==
                  diag_field(input_diag, " completions=") &&
              diag_field(evidence, " reports=") ==
                  diag_field(input_diag, " reports="),
          "the second kept record agrees with the second live line");
    CHECK(diag_field(mouse_line, " submitted=") ==
              diag_field(input_diag, " submitted=") &&
              diag_field(mouse_line, " completions=") ==
                  diag_field(input_diag, " completions=") &&
              diag_field(mouse_line, " reports=") ==
                  diag_field(input_diag, " reports="),
          "the second pointer record agrees with the second live line");
    CHECK(diag_field(evidence, " completions=") > completions1,
          "the kept record grew between the two exports");
}

int main(void) {
    test_composite_retry_keeps_the_mouse();
    test_separate_keyboard_after_silent_candidate();
    test_halted_endpoint_is_recovered();
    test_failed_endpoint_reset_clears_readiness();
    test_shared_device_repair_failure_restores_all_roles();
    test_export_snapshot_is_live_and_consistent();
    test_report_protocol_mouse_decodes_the_wheel();
    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    printf("xHCI composite device host test: ALL OK\n");
    return 0;
}
