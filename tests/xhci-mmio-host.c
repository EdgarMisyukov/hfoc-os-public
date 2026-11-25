/* Exercise the real startup guard and release/diagnostic lifecycle without
   physical hardware. MMIO uses RAM; console, time, journal and DMA allocation
   are host substitutes. Rejected probes must clear regs before release. */
#include "../kernel/src/xhci.c"

#include <stdio.h>
#include <string.h>

static int failures;
static char display[4096];
static size_t display_length;

/* Hardware/time/journal sinks linked from startup's event path. None of the
   halt-refusal fixtures may reach DMA allocation or access a physical device. */
uint64_t pmm_alloc_contiguous_pages(uint32_t count) {
    (void)count;
    ++failures;
    return 0U;
}

/* ITEM-577: a session whose pool is too small gives the old pages back before
   asking for a larger one.  This proof never allocates, so the free is a
   no-op that still has to exist for the link. */
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

void console_putc(char c) {
    if (display_length + 1U < sizeof(display)) {
        display[display_length++] = c;
        display[display_length] = '\0';
    }
}

void console_write(const char *text) {
    while (*text != '\0') {
        console_putc(*text++);
    }
}

/* ITEM-550: the durable-journal funnel the failure snapshot publishes through.
   The fixture keeps what was published so the proof can read the records back
   the same way a journal reader would. */
static char command_journal[8192];
static size_t command_journal_length;

void console_publish_journal(const char *record, size_t length) {
    if (record == NULL || length == 0U) {
        return;
    }
    for (size_t index = 0U; index < length; ++index) {
        if (command_journal_length + 2U < sizeof(command_journal)) {
            command_journal[command_journal_length++] = record[index];
        }
    }
    if (command_journal_length + 1U < sizeof(command_journal)) {
        command_journal[command_journal_length++] = '\n';
    }
    command_journal[command_journal_length] = '\0';
}

/* ITEM-574: the startup path now asks the PCI layer for Intel EHCI
   companions before the Intel routing stage.  This proof has no PCI bus, so
   the scan answers "none" (the routing module then leaves the controller
   alone before it reads or writes a single configuration register) and no
   MMIO window is ever mapped. */
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

#define CHECK(condition, text) do { \
    if (!(condition)) { puts("FAIL: " text); ++failures; } \
} while (0)

_Alignas(uint32_t) static uint8_t registers[0x10000];

static void reset_fixture(void) {
    memset(&xhci_machine, 0, sizeof(xhci_machine));
    memset(xhci_sessions, 0, sizeof(xhci_sessions));
    memset(registers, 0, sizeof(registers));
    memset(display, 0, sizeof(display));
    display_length = 0U;
    memset(command_journal, 0, sizeof(command_journal));
    command_journal_length = 0U;
    xhci = &xhci_sessions[0];
    xhci->in_use = true;
    xhci->location.device = 0x14U;
    xhci->mmio_bytes = sizeof(registers);
}

static void test_intel_offset_reaches_operational_registers(void) {
    reset_fixture();
    registers[0] = 0x80U;
    *(uint32_t *)&registers[XHCI_CAP_HCSPARAMS1] = 0x01000001U;
    *(uint32_t *)&registers[0x80U + XHCI_OP_PAGESIZE] = 1U;
    xhci->regs = registers;
    /* This model leaves HCH clear: a valid capability block must get as far
       as the bounded halt wait, rather than being rejected as BAD_CAPLENGTH. */
    CHECK(!xhci_controller_start(), "a non-halting mock controller is refused");
    CHECK(xhci->status == XHCI_STATUS_HALT_TIMEOUT,
          "Intel CAPLENGTH 0x80 passes the real controller startup guard");
    xhci->regs = NULL;
}

static void test_bad_offset_cannot_write_operational_registers(void) {
    uint8_t original[sizeof(registers)];

    reset_fixture();
    registers[0] = 0xFFU;
    memcpy(original, registers, sizeof(original));
    xhci->regs = registers;
    CHECK(!xhci_controller_start(), "an unaligned capability offset is refused");
    CHECK(xhci->regs == NULL, "a rejected offset is not used by release/stop");
    xhci_session_release(xhci);
    CHECK(memcmp(original, registers, sizeof(original)) == 0,
          "failed-capability cleanup writes no operational MMIO");
}

static void test_released_probe_remains_explainable(void) {
    char transient_detail[] = "BAD_CAPLENGTH";

    reset_fixture();
    xhci->status = XHCI_STATUS_MMIO_MAP_FAILED;
    xhci->detail = transient_detail;
    xhci->caplength = 0x80U;
    xhci_session_release(xhci);
    memset(transient_detail, 'X', sizeof(transient_detail) - 1U);
    CHECK(!xhci_sessions[0].in_use, "the failed session is actually released");
    xhci_diagnostics_print_xhci();
    CHECK(strstr(display, "BAD_CAPLENGTH") != NULL,
          "the failure detail survives session cleanup and source-buffer reuse");
    CHECK(strstr(display, "caplength=0x80") != NULL,
          "the failed controller's actual capability offset is visible");
    CHECK(strstr(display, "mmio_bytes=65536") != NULL,
          "the mapped span survives failed-session cleanup");
    CHECK(strstr(display, "00:14.0") != NULL,
          "diagnostics identify the failed PCI function");
}


/*
 * ITEM-550: the command wait's failure record, through the real
 * xhci_submit_command().
 *
 * The L875 run could only report `P1_ENABLE_SLOT_TIMEOUT`.  These proofs drive
 * the driver's own wait against RAM-backed MMIO, so every required distinction
 * is exercised in the code that will run on the laptop: a completion that
 * arrives (nothing is recorded), a wait that ends with the event ring empty
 * (timeout, no event read), and a wait whose only event is a Command Completion
 * for a different TRB (unmatched, kept).  The last proof releases the session
 * and requires the record to survive that release and the xhci command to print
 * it without a controller.
 */
static struct xhci_trb command_ring_trbs[XHCI_RING_TRBS];
static struct xhci_trb event_ring_trbs[XHCI_EVENT_TRBS];

#define COMMAND_RING_PHYSICAL 0x00000000A0000000ULL
#define EVENT_RING_PHYSICAL 0x00000000B0000000ULL
#define DCBAAP_PHYSICAL 0x00000000C0000000ULL
#define ERSTBA_PHYSICAL 0x00000000D0000000ULL

static void command_fixture(void) {
    reset_fixture();
    memset(command_ring_trbs, 0, sizeof(command_ring_trbs));
    memset(event_ring_trbs, 0, sizeof(event_ring_trbs));
    xhci->regs = registers;
    xhci->caplength = 0x20U;
    xhci->rt_offset = 0x1000U;
    xhci->db_offset = 0x2000U;
    xhci->mmio_bytes = sizeof(registers);
    xhci->max_slots = 8U;
    xhci->command_ring.trbs = command_ring_trbs;
    xhci->command_ring.phys = COMMAND_RING_PHYSICAL;
    xhci->command_ring.trb_count = XHCI_RING_TRBS;
    xhci->command_ring.enqueue = 0U;
    xhci->command_ring.cycle = 1U;
    xhci->event_ring = event_ring_trbs;
    xhci->event_ring_physical = EVENT_RING_PHYSICAL;
    xhci->event_index = 0U;
    xhci->event_cycle = 1U;
    *(uint32_t *)&registers[0x20U + XHCI_OP_USBCMD] = XHCI_USBCMD_RS;
    *(uint32_t *)&registers[0x20U + XHCI_OP_CONFIG] = 8U;
    *(uint32_t *)&registers[0x20U + XHCI_OP_CRCR] =
        (uint32_t)COMMAND_RING_PHYSICAL | XHCI_CRCR_RCS;
    *(uint32_t *)&registers[0x20U + XHCI_OP_DCBAAP] = (uint32_t)DCBAAP_PHYSICAL;
    *(uint32_t *)&registers[0x1000U + XHCI_RT_IR0 + XHCI_IR_ERSTSZ] = 1U;
    *(uint32_t *)&registers[0x1000U + XHCI_RT_IR0 + XHCI_IR_ERSTBA] =
        (uint32_t)ERSTBA_PHYSICAL;
    *(uint32_t *)&registers[0x1000U + XHCI_RT_IR0 + XHCI_IR_ERDP] =
        (uint32_t)EVENT_RING_PHYSICAL | XHCI_ERDP_EHB;
}

/* One Command Completion Event TRB, exactly as the controller writes it. */
static void put_command_completion(uint32_t index, uint64_t address,
                                   uint8_t code, uint8_t slot) {
    event_ring_trbs[index].dword[0] = (uint32_t)address;
    event_ring_trbs[index].dword[1] = (uint32_t)(address >> 32);
    event_ring_trbs[index].dword[2] = (uint32_t)code << 24;
    event_ring_trbs[index].dword[3] =
        ((uint32_t)XHCI_TRB_COMMAND_COMPLETION << XHCI_TRB_TYPE_SHIFT) |
        ((uint32_t)slot << XHCI_TRB_SLOT_SHIFT) | XHCI_TRB_CYCLE;
}

static void test_command_completion_is_taken(void) {
    const struct xhci_trb enable = xhci_trb_enable_slot(1U);
    struct xhci_event completion;

    memset(&completion, 0, sizeof(completion));
    command_fixture();
    put_command_completion(0U, COMMAND_RING_PHYSICAL, XHCI_COMPLETION_SUCCESS,
                           2U);
    CHECK(xhci_submit_command(&enable, &completion),
          "the wait takes the completion that names its own TRB");
    CHECK(completion.completion_code == XHCI_COMPLETION_SUCCESS,
          "the completion carries the controller's completion code");
    CHECK(!xhci_machine.command.valid,
          "a command that completed records no failure");
    CHECK(command_journal_length == 0U,
          "a command that completed publishes no command record");
}

static void test_command_timeout_is_captured_before_release(void) {
    const struct xhci_trb enable = xhci_trb_enable_slot(1U);
    uint8_t before[sizeof(registers)];

    command_fixture();
    memcpy(before, registers, sizeof(before));
    CHECK(!xhci_submit_command(&enable, NULL),
          "a wait that sees no event gives up");
    /* The submission rings the doorbell and nothing else touches MMIO: the
       snapshot is a reader, so every other register read back unchanged. */
    *(uint32_t *)&before[0x2000U] = *(uint32_t *)&registers[0x2000U];
    CHECK(memcmp(before, registers, sizeof(before)) == 0,
          "the failure snapshot writes no register and only the doorbell moved");
    CHECK(xhci_machine.command.valid,
          "the failure is kept in the machine's own record");
    CHECK(xhci_machine.command.result == XHCI_COMMAND_RESULT_TIMEOUT,
          "an accepted command that never completed is a timeout");
    CHECK(xhci_machine.command.type == XHCI_TRB_ENABLE_SLOT,
          "the record names the command type that failed");
    CHECK(xhci_machine.command.trb[3] == enable.dword[3],
          "the record carries the four dwords the command ring holds");
    CHECK(xhci_machine.command.ring_index == 0U &&
              xhci_machine.command.ring_cycle == 1U,
          "the record carries the ring slot and the producer cycle");
    CHECK(xhci_machine.command.physical == COMMAND_RING_PHYSICAL,
          "the record carries the command's physical address");
    CHECK(xhci_machine.command.wait.events == 0U &&
              xhci_machine.command.wait.unmatched == 0U,
          "a wait with no event says so instead of inventing one");
    CHECK(strstr(command_journal, "[XHCI-CMD] result=TIMEOUT") != NULL,
          "the failure reaches the durable journal at its failure point");
    CHECK(strstr(command_journal, "type=9") != NULL &&
              strstr(command_journal, "crcr=0x00000000A0000001") != NULL &&
              strstr(command_journal, "dcbaap=0x00000000C0000000") != NULL &&
              strstr(command_journal, "erstsz=0x00000001") != NULL &&
              strstr(command_journal, "erstba=0x00000000D0000000") != NULL,
          "the journal record carries the registers the cause depends on");
    CHECK(strstr(command_journal, "[XHCI-CMD-EVT]") != NULL &&
              strstr(command_journal, "event=none") != NULL &&
              strstr(command_journal, "events=0") != NULL,
          "the journal says the event ring produced nothing");

    /* Releasing the session is what the probe does next; the record has to
       outlive it and the xhci command has to print it with no controller. */
    xhci_session_release(xhci);
    memset(&xhci_sessions[0], 0, sizeof(xhci_sessions[0]));
    CHECK(!xhci_sessions[0].in_use, "the session that captured it is gone");
    memset(display, 0, sizeof(display));
    display_length = 0U;
    xhci_diagnostics_print_xhci();
    CHECK(strstr(display, "[XHCI-CMD] result=TIMEOUT") != NULL,
          "the xhci command prints the kept failure after the release");
    CHECK(strstr(display, "[XHCI-CMD-REG]") != NULL &&
              strstr(display, "[XHCI-CMD-EVT]") != NULL,
          "the xhci command prints all three captured records");
}

static void test_command_foreign_completion_is_counted(void) {
    const struct xhci_trb enable = xhci_trb_enable_slot(1U);

    command_fixture();
    put_command_completion(0U, COMMAND_RING_PHYSICAL + 0x40U, 4U, 5U);
    CHECK(!xhci_submit_command(&enable, NULL),
          "a completion for another TRB does not complete this wait");
    CHECK(xhci_machine.command.result == XHCI_COMMAND_RESULT_TIMEOUT,
          "a wait whose only event belonged to another command is a timeout");
    CHECK(xhci_machine.command.wait.events == 1U,
          "the event that was read is counted");
    CHECK(xhci_machine.command.wait.unmatched == 1U &&
              xhci_machine.command.wait.unmatched_valid,
          "the completion for another command is counted as unmatched");
    CHECK(xhci_machine.command.wait.unmatched_address ==
              COMMAND_RING_PHYSICAL + 0x40U,
          "the unmatched completion keeps the command address it named");
    CHECK(xhci_machine.command.wait.unmatched_code == 4U &&
              xhci_machine.command.wait.unmatched_slot == 5U,
          "the unmatched completion keeps its code and slot");
    CHECK(xhci_machine.command.event_valid &&
              xhci_machine.command.event[1] == 0U,
          "the record keeps the four dwords of the event it read");
    CHECK(strstr(command_journal, "unmatched=1") != NULL &&
              strstr(command_journal, "last=addr=0x00000000A0000040") != NULL,
          "the journal carries the unmatched completion the wait had to drop");
}

int main(void) {
    test_intel_offset_reaches_operational_registers();
    test_bad_offset_cannot_write_operational_registers();
    test_released_probe_remains_explainable();
    test_command_completion_is_taken();
    test_command_timeout_is_captured_before_release();
    test_command_foreign_completion_is_counted();
    if (failures != 0) {
        return 1;
    }
    puts("xHCI startup and released-probe host test: ALL OK");
    return 0;
}
