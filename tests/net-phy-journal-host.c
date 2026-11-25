/*
 * ITEM-590 host proof: the `[NET-PHY]` record reaches the boot journal.
 *
 * The owner's Toshiba filled its 512-slot boot log and not one slot named the
 * PHY: the record was written after the `[PRODUCTION-NET]` record's terminator,
 * and the console's journal mirror ignores fragments that arrive after a record
 * ended.  The serial stream had the line; the medium did not.
 *
 * This witness links the driver's own RTL8168 register model
 * (kernel/src/rtl8168.c), the record's one composition and its emitter
 * (kernel/src/appliance_production_network_phy_record.c) and the real journal
 * seam with the real preallocated FAT12 writer.  The console funnel is modelled
 * by its documented contract - `begin` opens a record, fragments append, a
 * fragment that ends in a newline completes it, and a fragment arriving after
 * that belongs to no record - which is exactly the rule the mirror implements.
 * The finished medium is then read by the operator's own Debian reader
 * (`scripts/hfoc-install-log-read.py`), so the claim is about the extracted
 * journal, not about a buffer this proof kept.
 *
 * The three states the record must cover are driven through the production
 * path:
 *
 *   * CONFIGURED - the raw TxConfig the laptop answered, 0x2C900000, gives
 *     XID 0x2C9, the PHYAR access, a completed configuration and its
 *     registers;
 *   * REFUSED - a PHY that never answers the MII handshake, whose first error
 *     stays on the state when a later poll finally reads it;
 *   * SKIPPED - a classic part with no PHY management at all, which must name
 *     the reason and print no register it never read.
 */
#include "appliance_production_network_phy_record.h"

#include "block_device.h"
#include "diag_export.h"
#include "ehci.h"
#include "install_journal_platform.h"
#include "install_journal_core.h"
#include "install_medium.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtl8168-phy-model.h"

static unsigned checks;
static unsigned failures;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        ++checks;                                                            \
        if (!(cond)) {                                                       \
            ++failures;                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (what));          \
        }                                                                    \
    } while (0)

bool ehci_storage_medium_status(const char **state, const char **reason) {
    if (state != NULL) {
        *state = "UNCHECKED";
    }
    if (reason != NULL) {
        *reason = "NOT_PROBED";
    }
    return false;
}

bool xhci_storage_medium_status(const char **state, const char **reason) {
    if (state != NULL) {
        *state = "UNCHECKED";
    }
    if (reason != NULL) {
        *reason = "NOT_PROBED";
    }
    return false;
}

bool ehci_storage_medium_publish(void) {
    return false;
}

bool xhci_storage_medium_publish(void) {
    return false;
}

const char *ehci_failure_primary(void) {
    return "NONE";
}

const char *ehci_status_string(enum ehci_status status) {
    (void)status;
    return "NONE";
}

enum ehci_status ehci_status(void) {
    return 0;
}

const char *ehci_detail(void) {
    return "NONE";
}

const char *ehci_failure_text(void) {
    return "";
}

const char *ehci_start_evidence(void) {
    return "pci=00:1D.0 start=0x0/0x0";
}

const char *ehci_controllers_text(void) {
    return "00:1D.0=NO_DEVICE/NO_CONNECTED_PORT";
}

bool ehci_storage_fault_text(char *buffer, size_t size) {
    (void)buffer;
    (void)size;
    return false;
}

bool xhci_storage_fault_text(char *buffer, size_t size) {
    (void)buffer;
    (void)size;
    return false;
}

void ehci_diagnostics_report(void (*sink)(void *context, const char *text),
                             void *context) {
    if (sink != NULL) {
        sink(context, "[EHCI] pci=00:1D.0 state=OK ports=2 devices=0");
    }
}

void xhci_diagnostics_report(void (*sink)(void *context, const char *text),
                             void *context) {
    if (sink != NULL) {
        sink(context, "[XHCI] state=UNAVAILABLE reason=NO_DEVICE pci=00:14.0");
    }
}

static const struct appliance_production_network_phy_info *export_phy_info;

void kernel_production_status_report(void (*sink)(void *, const char *),
                                     void *context) {
    char line[APPLIANCE_PRODUCTION_NETWORK_PHY_RECORD_MAX];
    size_t (*const compose[])(
        const struct appliance_production_network_phy_info *, char *, size_t) = {
        appliance_production_network_phy_record_line,
        appliance_production_network_phy_boot_line,
        appliance_production_network_phy_gigabit_line,
        appliance_production_network_phy_sample_line,
        appliance_production_network_phy_failure_line,
        appliance_production_network_phy_page_line,
        appliance_production_network_phy_select_line,
    };
    for (size_t i = 0; i < sizeof(compose) / sizeof(compose[0]); ++i) {
        if (sink && compose[i](export_phy_info, line, sizeof(line))) {
            sink(context, line);
        }
    }
}

/* Compile the actual adapter in this host TU so the real backend-selection
 * guard is tested. Uncalled PCI/MMIO paths are discarded at link time; no
 * production test hooks or copied adapter logic are used. */
#include "../kernel/src/appliance_production_network_x86.c"
#include "rtl8168-pit-reference-model.h"

/* The actual adapter delay runs against a hardware clock model. Global
 * console calibration is deliberately absent, as at cold network init. */
static uint64_t delay_test_cycles;
static uint64_t delay_test_published_rate;
static unsigned delay_test_pit_waits;
static bool delay_test_reference_ok = true;
#define DELAY_TEST_HZ UINT64_C(3200000000)

uint64_t timer_cycles(void) {
    delay_test_cycles += 320U; /* independent 0.1 us hardware read cost */
    return delay_test_cycles;
}
uint64_t timer_cycles_per_second(void) {
    return delay_test_published_rate;
}
bool timer_delay_ms_checked(uint32_t milliseconds) {
    ++delay_test_pit_waits;
    delay_test_cycles += delay_test_reference_ok
        ? (DELAY_TEST_HZ / 1000U) * milliseconds : DELAY_TEST_HZ / 10000U;
    return delay_test_reference_ok;
}
void timer_delay_ms(uint32_t milliseconds) {
    (void)timer_delay_ms_checked(milliseconds);
}

static void test_cold_adapter_delay_is_measured(void) {
    uint64_t start, elapsed;
    memset(&apn, 0, sizeof(apn));
    delay_test_cycles = 0U;
    delay_test_published_rate = 0U;
    delay_test_pit_waits = 0U;
    apn_rtl_delay_us(NULL, 0U);
    CHECK(delay_test_cycles == 0U && delay_test_pit_waits == 0U,
          "zero PHY delay does no clock work");
    start = delay_test_cycles;
    apn_rtl_delay_us(NULL, 50000U);
    elapsed = (delay_test_cycles - start) / (DELAY_TEST_HZ / 1000000U);
    CHECK(elapsed >= 50000U && elapsed < 250000U,
          "cold production adapter really waits the requested 50 ms reset interval");
    start = delay_test_cycles;
    apn_rtl_delay_us(NULL, 50000U);
    elapsed = (delay_test_cycles - start) / (DELAY_TEST_HZ / 1000000U);
    CHECK(elapsed >= 50000U && elapsed < 50100U,
          "50 ms wait is honored independently of earlier calibration time");
    start = delay_test_cycles;
    apn_rtl_delay_us(NULL, 25U);
    elapsed = (delay_test_cycles - start) / (DELAY_TEST_HZ / 1000000U);
    CHECK(elapsed >= 25U && elapsed < 100U,
          "subsequent PHYAR poll delay remains measured in microseconds");
    start = delay_test_cycles;
    apn_rtl_delay_us(NULL, 20U);
    elapsed = (delay_test_cycles - start) / (DELAY_TEST_HZ / 1000000U);
    CHECK(elapsed >= 20U && elapsed < 100U,
          "cold PHYAR post-operation settle remains at least 20 us");
    CHECK(delay_test_pit_waits == 1U,
          "cold adapter measures once and caches that reference");
    delay_test_published_rate = DELAY_TEST_HZ / 2U;
    start = delay_test_cycles;
    apn_rtl_delay_us(NULL, 20U);
    elapsed = (delay_test_cycles - start) / (DELAY_TEST_HZ / 1000000U);
    CHECK(elapsed >= 20U && elapsed < 30U && delay_test_pit_waits == 1U,
          "unverified shared clock cannot replace the checked NIC rate");
    memset(&apn, 0, sizeof(apn));
    delay_test_published_rate = 0U;
    delay_test_reference_ok = false;
    CHECK(apn_rtl_delay_rate() == 0U &&
              apn.rtl.delay_cycles_per_second == 0U,
          "failed PIT reference with advancing TSC is not a calibrated clock");
    delay_test_reference_ok = true;
    memset(&apn, 0, sizeof(apn));
}

/* --------------------------------------------------------- driver model --- */

#define TEST_XID_8168E 0x2C8U
#define TEST_TX_CONFIG_2C9 UINT32_C(0x2C900000)
#define TEST_TX_CONFIG_CLASSIC UINT32_C(0x30000000)
#define TEST_MMIO_LENGTH 512U

struct mmio_fixture {
    uint8_t registers[TEST_MMIO_LENGTH];
    bool fail_read;
    bool fail_write;
    bool phy_never_completes;
    /* ITEM-594: distinct hardware faults, below the production driver. */
    bool phy_ignore_vendor_page;
    bool phy_ignore_restore_page;
    bool phy_timeout_vendor_read;
    bool phy_fail_vendor_poll;
    bool phy_fail_payload_read;
    uint16_t phy_fail_payload_page;
    uint16_t phy_fail_payload_register;
    uint32_t phy_payload_extra; /* opaque raw bits, distinct from poll sample */
    unsigned phy_page_verify_fault; /* 1: timeout, 2: failed completion read */
    unsigned phy_vendor_poll_attempts;
    bool link_up;
    uint16_t phy_registers[32];
    struct phy_model phy_model;
    uint32_t phy_reads;
    uint32_t phy_writes;
    /*
     * ITEM-591: the PHYAR model's clock.  The driver is a production function
     * here too, so it must honour the handshake's 20 us settle; the model
     * counts a command that arrives inside the window and answers register 4
     * with fewer bits than the driver wrote.
     */
    uint64_t now_us;
    uint32_t phy_complete_us;
    uint16_t phy_advertise_write_mask;
    uint64_t phy_busy_at_us;
    uint64_t phy_last_complete_us;
    uint64_t phy_last_poll_us;
    bool phy_last_complete_valid;
    bool phy_operation_pending;
    bool phy_complete_observed;
    bool phy_pending_read;
    bool phy_completion_sample_pending;
    bool phy_poll_seen;
    uint16_t phy_pending_register;
    uint16_t phy_completion_sample_data;
    uint32_t phy_completion_samples;
    uint32_t phy_payload_reads;
    uint32_t phy_payload_read_failures;
    uint32_t phy_early_commands;
    uint32_t phy_delays;
    /* ITEM-592: the cold-start hardware the production cold boot drives. */
    uint16_t phy_page;
    uint32_t forbidden_writes;
    uint32_t order_violations;
    uint16_t ephy_registers[32];
    uint32_t ephy_writes;
    uint64_t ephy_busy_at_us;
    bool ephy_pending;
    bool ephy_pending_read;
    uint16_t ephy_pending_register;
    uint32_t eri_registers[2048];
    uint32_t eri_writes;
    uint8_t max_tx_packet_size;
};

static void store16(uint8_t *base, uint32_t offset, uint16_t value) {
    base[offset] = (uint8_t)(value & 0xFFU);
    base[offset + 1U] = (uint8_t)((value >> 8U) & 0xFFU);
}

static void store32(uint8_t *base, uint32_t offset, uint32_t value) {
    base[offset] = (uint8_t)(value & 0xFFU);
    base[offset + 1U] = (uint8_t)((value >> 8U) & 0xFFU);
    base[offset + 2U] = (uint8_t)((value >> 16U) & 0xFFU);
    base[offset + 3U] = (uint8_t)((value >> 24U) & 0xFFU);
}

static uint16_t load16(const uint8_t *base, uint32_t offset) {
    return (uint16_t)((uint16_t)base[offset] |
                      ((uint16_t)base[offset + 1U] << 8U));
}

static uint32_t load32(const uint8_t *base, uint32_t offset) {
    return (uint32_t)base[offset] | ((uint32_t)base[offset + 1U] << 8U) |
           ((uint32_t)base[offset + 2U] << 16U) |
           ((uint32_t)base[offset + 3U] << 24U);
}

#define TEST_MMIO_US 1U

static bool fixture_phy_completed(const struct mmio_fixture *fixture) {
    if (!fixture->phy_operation_pending || fixture->phy_never_completes) {
        return false;
    }
    if (fixture->phy_page_verify_fault == 1U && fixture->phy_pending_read &&
        fixture->phy_page == 7U && fixture->phy_pending_register == 0x1fU) {
        return false;
    }
    if (fixture->phy_timeout_vendor_read && fixture->phy_pending_read &&
        fixture->phy_page == 7U && fixture->phy_pending_register == 0x18U &&
        fixture->phy_model.ext_selector == 0xacU) {
        return false;
    }
    return fixture->now_us >= fixture->phy_busy_at_us;
}

static uint32_t fixture_phy_signal(struct mmio_fixture *fixture) {
    const uint32_t selector =
        (uint32_t)fixture->phy_pending_register <<
        RTL8168_PHY_ACCESS_REG_SHIFT;

    if (!fixture->phy_operation_pending) {
        return 0U;
    }
    if (!fixture_phy_completed(fixture)) {
        return fixture->phy_pending_read ? 0U : RTL8168_PHY_ACCESS_BUSY;
    }
    if (fixture->phy_pending_read) {
        if (fixture->phy_completion_sample_pending) {
            fixture->phy_completion_sample_pending = false;
            ++fixture->phy_completion_samples;
            return RTL8168_PHY_ACCESS_BUSY | selector |
                   fixture->phy_completion_sample_data;
        }
        ++fixture->phy_payload_reads;
        return RTL8168_PHY_ACCESS_BUSY | selector | fixture->phy_payload_extra |
               phy_model_read(&fixture->phy_model, fixture->phy_registers,
                              fixture->phy_page, fixture->phy_pending_register,
                              fixture->now_us);
    }
    return 0U;
}

static void fixture_phy_note_completion(struct mmio_fixture *fixture) {
    if (!fixture->phy_complete_observed && fixture_phy_completed(fixture)) {
        fixture->phy_complete_observed = true;
        fixture->phy_last_complete_us = fixture->now_us;
        fixture->phy_last_complete_valid = true;
    }
}

static void fixture_phy_start(struct mmio_fixture *fixture, bool read,
                              uint16_t reg) {
    if (fixture->phy_last_complete_valid &&
        fixture->now_us < fixture->phy_last_complete_us +
                              RTL8168_PHY_ACCESS_SETTLE_US) {
        ++fixture->phy_early_commands;
    }
    fixture->phy_operation_pending = true;
    fixture->phy_complete_observed = false;
    fixture->phy_pending_read = read;
    fixture->phy_completion_sample_pending = read;
    fixture->phy_pending_register = reg;
    fixture->phy_busy_at_us = fixture->now_us + fixture->phy_complete_us;
    fixture->phy_poll_seen = false;
}

static bool fixture_ephy_completed(const struct mmio_fixture *fixture) {
    return fixture->ephy_pending && fixture->now_us >= fixture->ephy_busy_at_us;
}

static uint32_t fixture_ephy_signal(struct mmio_fixture *fixture) {
    if (!fixture->ephy_pending) {
        return 0U;
    }
    if (!fixture_ephy_completed(fixture)) {
        return fixture->ephy_pending_read ? 0U : RTL8168_EPHY_ACCESS_FLAG;
    }
    if (fixture->ephy_pending_read) {
        return RTL8168_EPHY_ACCESS_FLAG |
               fixture->ephy_registers[fixture->ephy_pending_register];
    }
    return 0U;
}

static void fixture_delay_us(void *context, uint32_t microseconds) {
    struct mmio_fixture *fixture = (struct mmio_fixture *)context;

    if (fixture == NULL) {
        return;
    }
    fixture->now_us += microseconds;
    ++fixture->phy_delays;
}

static bool fixture_read8(void *context, uint32_t offset, uint8_t *value) {
    struct mmio_fixture *fixture = (struct mmio_fixture *)context;

    if (fixture == NULL || value == NULL || offset >= TEST_MMIO_LENGTH ||
        fixture->fail_read) {
        return false;
    }
    *value = fixture->registers[offset];
    return true;
}

static bool fixture_read16(void *context, uint32_t offset, uint16_t *value) {
    struct mmio_fixture *fixture = (struct mmio_fixture *)context;

    if (fixture == NULL || value == NULL || offset + 2U > TEST_MMIO_LENGTH ||
        fixture->fail_read) {
        return false;
    }
    *value = load16(fixture->registers, offset);
    return true;
}

static bool fixture_read32(void *context, uint32_t offset, uint32_t *value) {
    struct mmio_fixture *fixture = (struct mmio_fixture *)context;

    if (fixture == NULL || value == NULL || offset + 4U > TEST_MMIO_LENGTH ||
        fixture->fail_read) {
        return false;
    }
    fixture->now_us += TEST_MMIO_US;
    if (offset == RTL8168_REG_EPHY_ACCESS) {
        *value = fixture_ephy_signal(fixture);
        return true;
    }
    if (offset == RTL8168_REG_PHY_ACCESS) {
        if (fixture->phy_page_verify_fault == 2U && fixture->phy_pending_read &&
            fixture->phy_page == 7U && fixture->phy_pending_register == 0x1fU &&
            ++fixture->phy_vendor_poll_attempts == 2U) {
            return false;
        }
        if (fixture->phy_fail_vendor_poll && fixture->phy_pending_read &&
            fixture->phy_page == 7U && fixture->phy_pending_register == 0x18U &&
            fixture->phy_model.ext_selector == 0xacU &&
            ++fixture->phy_vendor_poll_attempts == 2U) {
            return false;
        }
        if (fixture->phy_operation_pending &&
            !fixture->phy_complete_observed) {
            fixture->phy_poll_seen = true;
            fixture->phy_last_poll_us = fixture->now_us;
        }
        fixture_phy_note_completion(fixture);
        if (fixture->phy_fail_payload_read &&
            fixture->phy_operation_pending && fixture->phy_pending_read &&
            !fixture->phy_completion_sample_pending &&
            fixture_phy_completed(fixture) &&
            (fixture->phy_fail_payload_page == 0xffffU ||
             fixture->phy_fail_payload_page == fixture->phy_page) &&
            (fixture->phy_fail_payload_register == 0xffffU ||
             fixture->phy_fail_payload_register ==
                 fixture->phy_pending_register)) {
            ++fixture->phy_payload_read_failures;
            return false;
        }
        *value = fixture_phy_signal(fixture);
        return true;
    }
    *value = load32(fixture->registers, offset);
    return true;
}

static bool fixture_write8(void *context, uint32_t offset, uint8_t value) {
    struct mmio_fixture *fixture = (struct mmio_fixture *)context;

    if (fixture == NULL || offset >= TEST_MMIO_LENGTH ||
        fixture->fail_write) {
        return false;
    }
    if (offset == RTL8168_REG_MAX_TX_PACKET_SIZE) {
        fixture->max_tx_packet_size = value;
    }
    if (offset == RTL8168_REG_CHIP_CMD &&
        (value & RTL8168_CHIP_CMD_RESET) != 0U) {
        /* A device that finishes its reset clears the bit by itself. */
        fixture->registers[offset] =
            (uint8_t)(value & (uint8_t)~RTL8168_CHIP_CMD_RESET);
        return true;
    }
    fixture->registers[offset] = value;
    return true;
}

static bool fixture_write16(void *context, uint32_t offset, uint16_t value) {
    struct mmio_fixture *fixture = (struct mmio_fixture *)context;

    if (fixture == NULL || offset + 2U > TEST_MMIO_LENGTH ||
        fixture->fail_write) {
        return false;
    }
    store16(fixture->registers, offset, value);
    return true;
}

static bool fixture_write32(void *context, uint32_t offset, uint32_t value) {
    struct mmio_fixture *fixture = (struct mmio_fixture *)context;

    if (fixture == NULL || offset + 4U > TEST_MMIO_LENGTH ||
        fixture->fail_write) {
        return false;
    }
    if (offset == RTL8168_REG_EPHY_ACCESS) {
        const uint16_t reg = (uint16_t)(
            (value >> RTL8168_EPHY_ACCESS_REG_SHIFT) &
            RTL8168_EPHY_ACCESS_REG_MASK);

        fixture->now_us += TEST_MMIO_US;
        if ((value & RTL8168_EPHY_ACCESS_WRITE) != 0U) {
            ++fixture->ephy_writes;
            fixture->ephy_registers[reg] =
                (uint16_t)(value & RTL8168_EPHY_ACCESS_DATA_MASK);
            fixture->ephy_pending = true;
            fixture->ephy_pending_read = false;
            fixture->ephy_pending_register = reg;
            fixture->ephy_busy_at_us = fixture->now_us + fixture->phy_complete_us;
        } else {
            fixture->ephy_pending = true;
            fixture->ephy_pending_read = true;
            fixture->ephy_pending_register = reg;
            fixture->ephy_busy_at_us = fixture->now_us + fixture->phy_complete_us;
        }
        store32(fixture->registers, offset, 0U);
        return true;
    }
    if (offset == RTL8168_REG_ERI_ADDRESS) {
        const uint32_t address = value & RTL8168_ERIAR_ADDRESS_MASK;

        if ((value & RTL8168_ERIAR_WRITE) != 0U) {
            ++fixture->eri_writes;
            if (address < sizeof(fixture->eri_registers) /
                              sizeof(fixture->eri_registers[0])) {
                fixture->eri_registers[address] =
                    load32(fixture->registers, RTL8168_REG_ERI_DATA);
            }
            store32(fixture->registers, RTL8168_REG_ERI_ADDRESS,
                    value & ~RTL8168_ERIAR_FLAG);
            return true;
        }
        /* The MAC address read the driver's identify path performs. */
        store32(fixture->registers, RTL8168_REG_ERI_DATA,
                address == RTL8168_ERI_MAC_LOW ? 0x12005452U : 0x00005634U);
        store32(fixture->registers, RTL8168_REG_ERI_ADDRESS,
                value | RTL8168_ERIAR_FLAG);
        return true;
    }
    if (offset == RTL8168_REG_MISC) {
        const uint32_t before = load32(fixture->registers, RTL8168_REG_MISC);

        if (((before ^ value) & RTL8168_MISC_RXDV_GATED_EN) != 0U) {
            ++fixture->forbidden_writes;
        }
        store32(fixture->registers, offset, value);
        return true;
    }
    if (offset == RTL8168_REG_RX_CONFIG) {
        if ((value & RTL8168_RX_EARLY_OFF) != 0U) {
            ++fixture->forbidden_writes;
        }
        store32(fixture->registers, offset, value);
        return true;
    }
    if (offset == RTL8168_REG_PHY_ACCESS) {
        const uint16_t reg = (uint16_t)(
            (value >> RTL8168_PHY_ACCESS_REG_SHIFT) &
            RTL8168_PHY_ACCESS_REG_MASK);

        fixture->now_us += TEST_MMIO_US;
        if (!phy_model_accept(&fixture->phy_model, fixture->phy_page, reg,
                              (value & RTL8168_PHY_ACCESS_BUSY) != 0U,
                              (uint16_t)value)) {
            return false;
        }
        if ((value & RTL8168_PHY_ACCESS_BUSY) != 0U) {
            ++fixture->phy_writes;
            if (!(reg == 0x1fU &&
                  ((fixture->phy_ignore_vendor_page && (uint16_t)value == 7U) ||
                   (fixture->phy_ignore_restore_page && (uint16_t)value == 0U &&
                    fixture->phy_model.fault_fired)))) {
            phy_model_write(&fixture->phy_model, fixture->phy_registers,
                            &fixture->phy_page, reg,
                            (uint16_t)((uint16_t)value &
                                (fixture->phy_page == 0U &&
                                 reg == RTL8168_PHY_REG_ADVERTISE
                                     ? fixture->phy_advertise_write_mask
                                     : 0xFFFFU)), fixture->now_us);
            }
            fixture_phy_start(fixture, false, reg);
        } else {
            ++fixture->phy_reads;
            fixture_phy_start(fixture, true, reg);
        }
        store32(fixture->registers, offset, 0U);
        return true;
    }
    store32(fixture->registers, offset, value);
    return true;
}

static struct rtl8168_mmio_io fixture_io(struct mmio_fixture *fixture) {
    struct rtl8168_mmio_io io;

    memset(&io, 0, sizeof(io));
    io.context = fixture;
    io.read8 = fixture_read8;
    io.read16 = fixture_read16;
    io.read32 = fixture_read32;
    io.write8 = fixture_write8;
    io.write16 = fixture_write16;
    io.write32 = fixture_write32;
    io.delay_us = fixture_delay_us;
    return io;
}

static void fixture_init(struct mmio_fixture *fixture, uint32_t tx_config) {
    memset(fixture, 0, sizeof(*fixture));
    phy_model_init(&fixture->phy_model);
    fixture->phy_complete_us = 3U;
    fixture->phy_fail_payload_page = 0xffffU;
    fixture->phy_fail_payload_register = 0xffffU;
    /* The PHY masks the 10BASE-T full duplex bit: the read-back must differ
       from what the driver wrote, or the journal assertion proves nothing. */
    fixture->phy_advertise_write_mask = 0x01BFU;
    store32(fixture->registers, RTL8168_REG_TX_CONFIG, tx_config);
    if (((tx_config >> 20U) & 0x7C8U) == 0x2C8U) {
        /* The firmware leftovers the evl cold start must deal with. */
        store32(fixture->registers, RTL8168_REG_MISC,
                RTL8168_MISC_RXDV_GATED_EN);
        fixture->registers[RTL8168_REG_MCU] =
            (uint8_t)RTL8168_MCU_NOW_IS_OOB;
        fixture->registers[RTL8168_REG_CONFIG5] =
            (uint8_t)RTL8168_CONFIG5_SPI_EN;
        fixture->registers[RTL8168_REG_EEE_LED] =
            (uint8_t)RTL8168_EEE_LED_MASK;
    }
    store16(fixture->registers, RTL8168_REG_CPLUS_CMD, 0x2000U);
    fixture->link_up = true;
    fixture->phy_registers[RTL8168_PHY_REG_BMCR] =
        RTL8168_PHY_BMCR_ANEG_ENABLE | RTL8168_PHY_BMCR_SPEED_100 |
        RTL8168_PHY_BMCR_DUPLEX;
    fixture->phy_registers[RTL8168_PHY_REG_BMSR] =
        RTL8168_PHY_BMSR_LINK | RTL8168_PHY_BMSR_ANEG_COMPLETE;
    fixture->phy_registers[RTL8168_PHY_REG_PHYID1] = 0x001CU;
    fixture->phy_registers[RTL8168_PHY_REG_PHYID2] = 0xC912U;
    fixture->phy_registers[RTL8168_PHY_REG_ADVERTISE] = 0x01E1U;
    fixture->phy_registers[RTL8168_PHY_REG_LPA] = 0x01E1U;
    fixture->registers[RTL8168_REG_PHY_STATUS] =
        (uint8_t)RTL8168_PHY_STATUS_LINK;
}

/* ---------------------------------------------- the console's journal funnel --- */

#define RECORD_MAX 440U

static char record_text[RECORD_MAX + 1U];
static size_t record_length;
static bool record_open;
static bool record_complete;
static unsigned records_published;

static char serial_text[8192];
static size_t serial_length;

void serial_write(const char *text) {
    const size_t length = text != NULL ? strlen(text) : 0U;

    if (length == 0U || serial_length + length >= sizeof(serial_text)) {
        return;
    }
    memcpy(&serial_text[serial_length], text, length);
    serial_length += length;
    serial_text[serial_length] = '\0';
}

void serial_write_u64(uint64_t value) {
    char text[24];

    snprintf(text, sizeof(text), "%llu", (unsigned long long)value);
    serial_write(text);
}

void serial_write_hex(uint64_t value) {
    char text[24];

    snprintf(text, sizeof(text), "0x%llX", (unsigned long long)value);
    serial_write(text);
}

uint64_t serial_sequence_begin(void) {
    return 0U;
}

void serial_sequence_end(uint64_t flags) {
    (void)flags;
}

void appliance_boot_log_store_observe(const char *record, size_t length) {
    (void)record;
    (void)length;
}

/*
 * The console's own rule, as the mirror implements it: a record is opened,
 * fragments are carried in order, the first terminator completes the record,
 * and every later fragment belongs to no record until the next `begin`.
 */
void console_journal_begin(void) {
    record_length = 0U;
    record_text[0] = '\0';
    record_open = true;
    record_complete = false;
}

void console_record_text(const char *text) {
    if (!record_open || text == NULL) {
        return;
    }
    for (size_t index = 0U; text[index] != '\0'; ++index) {
        if (text[index] == '\n' || text[index] == '\r') {
            record_complete = true;
            break;
        }
        if (record_complete || record_length >= RECORD_MAX) {
            break;
        }
        record_text[record_length++] = text[index];
    }
    record_text[record_length] = '\0';
}

void console_journal_end(void) {
    if (!record_open) {
        return;
    }
    record_open = false;
    if (record_length == 0U) {
        return;
    }
    ++records_published;
    /* Exactly what console_journal_record() hands over: the finished line. */
    install_journal_platform_observe(record_text, record_length);
}

/* ------------------------------------------------------------- the medium --- */

#define BPS 512U
#define SPF 9U
#define RESERVED 1U
#define NFAT 2U
#define ROOT_ENTRIES 240U
#define SPC 2U
#define TEST_SECTORS 6144U
#define PARTITION_LBA 2048U
#define ROOT_SECTORS ((ROOT_ENTRIES * 32U + BPS - 1U) / BPS)
#define ROOT_LBA (RESERVED + NFAT * SPF)
#define DATA_LBA (ROOT_LBA + ROOT_SECTORS)
#define HFOC_CLUSTER 2U
#define FILE_FIRST 3U
#define FILE_CLUSTERS 8U
#define JOURNAL_SLOTS (FILE_CLUSTERS * SPC)
#define EOC 0x0FFFU

#define IMAGE_DIR "build/test-artifacts/net-phy-journal"

struct ram_medium {
    uint8_t *bytes;
    uint64_t size;
};

static struct ram_medium medium;
static struct block_device medium_block;

static enum block_device_status ram_read(struct block_device *device,
                                         uint64_t offset, void *buffer,
                                         size_t length) {
    struct ram_medium *ram = device->context;

    if (offset + length > ram->size) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    memcpy(buffer, &ram->bytes[offset], length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status ram_write(struct block_device *device,
                                          uint64_t offset,
                                          const void *buffer, size_t length) {
    struct ram_medium *ram = device->context;

    if (offset + length > ram->size) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    memcpy(&ram->bytes[offset], buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status ram_flush(struct block_device *device) {
    (void)device;
    return BLOCK_DEVICE_OK;
}

static void put_le16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void put_le32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8) & 0xFFU);
    p[2] = (uint8_t)((value >> 16) & 0xFFU);
    p[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static uint32_t crc32_bytes(const uint8_t *bytes, size_t length) {
    uint32_t crc = 0xFFFFFFFFU;

    for (size_t i = 0U; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1) ^
                  (0xEDB88320U & (uint32_t)(-(int32_t)(crc & 1U)));
        }
    }
    return ~crc;
}

static uint64_t cluster_offset(uint32_t cluster) {
    return (uint64_t)(DATA_LBA + (cluster - 2U) * SPC) * BPS;
}

static void fat_set(uint8_t *img, uint32_t cluster, uint16_t value) {
    for (uint32_t copy = 0U; copy < NFAT; ++copy) {
        uint64_t base = (uint64_t)(RESERVED + copy * SPF) * BPS;
        uint32_t off = cluster + cluster / 2U;
        uint16_t word = (uint16_t)((uint16_t)img[base + off] |
                                   ((uint16_t)img[base + off + 1U] << 8));

        if (cluster % 2U == 0U) {
            word = (uint16_t)((word & 0xF000U) | (value & 0x0FFFU));
        } else {
            word = (uint16_t)((word & 0x000FU) | ((value & 0x0FFFU) << 4));
        }
        put_le16(&img[base + off], word);
    }
}

static void make_entry(uint8_t *entry, const char *name8, const char *ext3,
                       uint8_t attr, uint32_t cluster, uint32_t size) {
    memset(entry, 0, 32U);
    memset(entry, ' ', 11U);
    memcpy(entry, name8, strlen(name8));
    memcpy(&entry[8], ext3, strlen(ext3));
    entry[11] = attr;
    put_le16(&entry[26], (uint16_t)cluster);
    put_le32(&entry[28], size);
}

static void header_put(uint8_t *slot, uint32_t capacity_slots) {
    memset(slot, 0, BPS);
    memcpy(slot, INSTALL_JOURNAL_MAGIC, INSTALL_JOURNAL_MAGIC_BYTES);
    put_le32(&slot[9], INSTALL_JOURNAL_VERSION);
    put_le32(&slot[13], INSTALL_JOURNAL_SLOT_BYTES);
    put_le32(&slot[17], capacity_slots);
    put_le32(&slot[21], 1U);
    put_le32(&slot[36], crc32_bytes(slot, 36U));
}

static void volume_build(uint8_t *volume) {
    volume[510] = 0x55U;
    volume[511] = 0xAAU;
    put_le16(&volume[11], BPS);
    volume[13] = SPC;
    put_le16(&volume[14], RESERVED);
    volume[16] = NFAT;
    put_le16(&volume[17], ROOT_ENTRIES);
    put_le16(&volume[19], (uint16_t)TEST_SECTORS);
    volume[21] = 0xF8U;
    put_le16(&volume[22], SPF);
    put_le16(&volume[24], 63U);
    put_le16(&volume[26], 255U);
    put_le32(&volume[28], 140U);
    volume[38] = 0x29U;
    memcpy(&volume[54], "FAT12   ", 8U);

    fat_set(volume, 0U, 0x0FF8U);
    fat_set(volume, 1U, EOC);
    fat_set(volume, HFOC_CLUSTER, EOC);
    for (uint32_t i = 0U; i < FILE_CLUSTERS; ++i) {
        fat_set(volume, FILE_FIRST + i,
                (i + 1U == FILE_CLUSTERS) ? EOC
                                          : (uint16_t)(FILE_FIRST + i + 1U));
    }
    make_entry(&volume[ROOT_LBA * BPS], "HFOC", "", 0x10U, HFOC_CLUSTER, 0U);
    {
        uint8_t *dir = &volume[cluster_offset(HFOC_CLUSTER)];

        make_entry(&dir[0], ".", "", 0x10U, HFOC_CLUSTER, 0U);
        make_entry(&dir[32], "..", "", 0x10U, 0U, 0U);
        make_entry(&dir[64], "INSTALL", "LOG", 0x20U, FILE_FIRST,
                   JOURNAL_SLOTS * BPS);
    }
    header_put(&volume[cluster_offset(FILE_FIRST)], JOURNAL_SLOTS);
}

/* A fresh machine: the seam initialises once and never rebinds a journal. */
static void machine_reset(void) {
    uint8_t *entry;
    struct install_medium_verdict verdict;

    install_journal_platform_init();
    records_published = 0U;
    record_length = 0U;
    record_open = false;
    record_complete = false;
    record_text[0] = '\0';
    serial_length = 0U;
    serial_text[0] = '\0';

    free(medium.bytes);
    memset(&medium, 0, sizeof(medium));
    medium.size = (uint64_t)(PARTITION_LBA + TEST_SECTORS) * BPS;
    medium.bytes = calloc(1U, (size_t)medium.size);
    if (medium.bytes == NULL) {
        fprintf(stderr, "cannot allocate the RAM medium\n");
        exit(2);
    }
    medium.bytes[510] = 0x55U;
    medium.bytes[511] = 0xAAU;
    entry = &medium.bytes[446];
    entry[4] = 0x01U;
    put_le32(&entry[8], PARTITION_LBA);
    put_le32(&entry[12], TEST_SECTORS);
    volume_build(&medium.bytes[(uint64_t)PARTITION_LBA * BPS]);

    memset(&medium_block, 0, sizeof(medium_block));
    medium_block.block_size = BPS;
    medium_block.block_count = medium.size / BPS;
    medium_block.context = &medium;
    medium_block.read_fn = ram_read;
    medium_block.write_fn = ram_write;
    medium_block.flush_fn = ram_flush;

    memset(&verdict, 0, sizeof(verdict));
    verdict.result = INSTALL_MEDIUM_MATCH;
    verdict.reason = "OK";
    verdict.partition_index = 1U;
    verdict.partition_type = 0x01U;
    verdict.first_lba = PARTITION_LBA;
    verdict.length_lba = TEST_SECTORS;
    if (!install_journal_platform_bind_medium(&medium_block, &verdict)) {
        fprintf(stderr, "the reserved journal does not bind\n");
        exit(2);
    }
    if (!install_journal_platform_ready()) {
        fprintf(stderr, "the bound journal is not ready\n");
        exit(2);
    }
}

/* The device record the machine publishes first, then the PHY record as the
   production call site publishes it: the device record is finished, the PHY
   record arrives after it. */
static void publish_device_record_then_phy(
    const struct appliance_production_network_phy_info *info) {
    console_journal_begin();
    console_record_text("[PRODUCTION-NET] device=PRESENT readiness=DEGRADED "
                        "backend=RTL8168 pci=10EC:8168 link=DOWN "
                        "status=LINK_NOT_READY\n");
    console_journal_end();
    CHECK(appliance_production_network_phy_record_publish(info, serial_write),
          "the PHY answer publishes a record");
    /* ITEM-592: the cold start's evidence follows as its own records. */
    appliance_production_network_phy_detail_publish(info, serial_write);
}

static unsigned extract_journal(const char *name, const char *log) {
    char image[256];
    char command[768];
    int result;

    snprintf(image, sizeof(image), "%s/%s.img", IMAGE_DIR, name);
    {
        FILE *handle = fopen(image, "wb");
        bool ok = handle != NULL &&
                  fwrite(medium.bytes, 1U, (size_t)medium.size, handle) ==
                      (size_t)medium.size;

        if (handle != NULL) {
            fclose(handle);
        }
        if (!ok) {
            printf("FAIL: cannot write %s\n", image);
            ++failures;
            return 0U;
        }
    }
    snprintf(command, sizeof(command),
             "python3 scripts/hfoc-install-log-read.py --disk '%s' >'%s' 2>&1",
             image, log);
    result = system(command);
    if (result == -1 || (result >> 8) != 0) {
        printf("FAIL: the operator's reader refused %s\n", image);
        ++failures;
        return 0U;
    }
    return 1U;
}

static bool log_contains(const char *path, const char *needle) {
    char line[1024];
    FILE *handle = fopen(path, "rb");

    if (handle == NULL) {
        return false;
    }
    while (fgets(line, sizeof(line), handle) != NULL) {
        if (strstr(line, needle) != NULL) {
            fclose(handle);
            return true;
        }
    }
    fclose(handle);
    return false;
}

static bool log_has_exact_line(const char *path, const char *line_start) {
    char line[1024];
    FILE *handle = fopen(path, "rb");

    if (handle == NULL) {
        return false;
    }
    while (fgets(line, sizeof(line), handle) != NULL) {
        /* The reader prints "<slot>  <text>": the record starts after the
           right-aligned slot number. */
        const char *text = line;

        while (*text == ' ') {
            ++text;
        }
        while (*text >= '0' && *text <= '9') {
            ++text;
        }
        while (*text == ' ') {
            ++text;
        }
        {
            const size_t length = strlen(text);

            if (length != 0U && text[length - 1U] == '\n' &&
                strncmp(text, line_start, length - 1U) == 0 &&
                strlen(line_start) == length - 1U) {
                fclose(handle);
                return true;
            }
        }
    }
    fclose(handle);
    return false;
}

/* ------------------------------------------------------------- the cases --- */

static void test_configured_phy_names_itself(void) {
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;
    struct rtl8168_phy_state phy;
    struct rtl8168_cold_boot_report boot;
    struct appliance_production_network_phy_info info;

    memset(&boot, 0, sizeof(boot));
    machine_reset();
    fixture_init(&fixture, TEST_TX_CONFIG_2C9);
    io = fixture_io(&fixture);
    memset(&chip, 0, sizeof(chip));
    memset(&phy, 0, sizeof(phy));
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK,
          "the raw 0x2C900000 TxConfig identifies the laptop's part");
    CHECK(chip.xid == 0x2C9U, "the XID is the one the Toshiba answered");
    CHECK(chip.profile == RTL8168_PROFILE_8168EVL,
          "the row is the 8168evl generation");
    phy.access = rtl8168_phy_access_method(&chip);
    CHECK(phy.access == RTL8168_PHY_ACCESS_MII_PHYAR,
          "the part's PHY is reached through PHYAR");
    /*
     * ITEM-592: the production cold start, not a bare PHY configuration: the
     * EPHY and clock stages run first, then the MAC, then the PHY stage with
     * its page, both advertisements and the auto-negotiation start.
     */
    {
        struct rtl8168_bring_up configuration;
        static const uint8_t mac[RTL8168_MAC_LENGTH] = {
            0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U
        };

        memset(&configuration, 0, sizeof(configuration));
        configuration.rx_descriptor_physical = UINT64_C(0x200000);
        configuration.tx_descriptor_physical = UINT64_C(0x200100);
        configuration.rx_buffer_capacity = 2048U;
        CHECK(rtl8168_cold_boot(&io, &chip, &configuration, mac, &phy,
                                &boot) == RTL8168_OK,
              "the 8168evl cold start completes on the production path");
    }
    CHECK(boot.stages[RTL8168_STAGE_EPHY] == RTL8168_STAGE_OK &&
              boot.stages[RTL8168_STAGE_CLOCK] == RTL8168_STAGE_OK &&
              boot.stages[RTL8168_STAGE_MAC] == RTL8168_STAGE_OK &&
              boot.stages[RTL8168_STAGE_PHY] == RTL8168_STAGE_OK,
          "every generation-specific stage ran");
    CHECK(fixture.order_violations == 0U &&
              fixture.forbidden_writes == 0U,
          "the model saw the documented order and no foreign register");
    CHECK(fixture.phy_reads != 0U &&
              fixture.phy_completion_samples == fixture.phy_reads &&
              fixture.phy_payload_reads == fixture.phy_reads,
          "cold start separates each PHY completion sample from its payload");
    CHECK(rtl8168_phy_refresh(&io, &phy) == RTL8168_OK,
          "the PHY state reads back after the configuration");
    appliance_production_network_phy_info_from_driver(&chip, &phy, &boot,
                                                      0U, &info);
    CHECK(info.available && info.state ==
                                APPLIANCE_PRODUCTION_NETWORK_PHY_STATE_CONFIGURED,
          "the answer is CONFIGURED");
    CHECK(info.first_error == NULL,
          "a PHY with no error says NONE, not the name of success");
    CHECK(info.advertise_written == RTL8168_PHY_ADVERTISE_DEFAULT &&
              info.advertise ==
                  (uint16_t)(RTL8168_PHY_ADVERTISE_DEFAULT & 0x01BFU) &&
              info.advertise_valid,
          "the answer separates the written advertisement from the read one");
    CHECK(info.advertise != RTL8168_PHY_ADVERTISE_DEFAULT,
          "the recorded advertisement is not the constant");
    CHECK(info.advertise_1000_written == RTL8168_PHY_ADVERTISE_1000 &&
              info.advertise_1000_valid &&
              info.advertise_1000 == RTL8168_PHY_ADVERTISE_1000,
          "register 9 advertises 1000BASE-T and reads back");
    CHECK(info.lpa_1000_valid && info.lpa_1000 == 0U,
          "the partner's 1000BASE-T status is read");
    CHECK(info.profile != NULL &&
              strcmp(info.profile, "RTL8168evl/8111evl") == 0,
          "the answer names the confirmed profile");
    CHECK(info.page_valid && info.page == RTL8168_PHY_PAGE_STANDARD,
          "the answer names the PHY page");
    CHECK(info.advertise != RTL8168_PHY_ADVERTISE_DEFAULT,
          "the recorded advertisement is not the constant");
    CHECK(fixture.phy_early_commands == 0U,
          "the configuration never issued a PHY command inside the settle "
          "window");

    publish_device_record_then_phy(&info);
    CHECK(records_published == 5U,
          "the device record, the PHY record and the three evidence records "
          "are five records");
    CHECK(strstr(serial_text, "[NET-PHY-FAIL]") == NULL,
          "successful PHY reads do not invent a failed transaction");
    CHECK(extract_journal("configured", IMAGE_DIR "/configured.log") == 1U,
          "the configured medium is readable by the operator's reader");
    CHECK(log_has_exact_line(
              IMAGE_DIR "/configured.log",
              "[PRODUCTION-NET] device=PRESENT readiness=DEGRADED "
              "backend=RTL8168 pci=10EC:8168 link=DOWN "
              "status=LINK_NOT_READY"),
          "the device record is its own line in the journal");
    CHECK(log_contains(IMAGE_DIR "/configured.log",
                       "  [NET-PHY] state=CONFIGURED xid=0x02C9 "
                       "access=MII_PHYAR profile=RTL8168evl/8111evl "
                       "first_error=NONE phyid=0x001C:0xC912 bmcr=0x"),
          "the journal carries the PHY record with the XID, the access, the "
          "profile and the registers");
    CHECK(log_contains(IMAGE_DIR "/configured.log", " bmsr=0x"),
          "the PHY record carries BMSR");
    CHECK(log_contains(IMAGE_DIR "/configured.log",
                       " adv=0x01A1 adv_written=0x01E1 adv_read=OK"),
          "the journal carries the advertisement the part answered, the value "
          "that was written and the read's own verdict");
    CHECK(!log_contains(IMAGE_DIR "/configured.log", " adv=0x03E1"),
          "the journal does not print the constant as if it were the "
          "advertisement");
    CHECK(log_contains(IMAGE_DIR "/configured.log",
                       "  [NET-PHY-BOOT] profile=RTL8168evl/8111evl "
                       "stages=reset:OK,phy_resume:OK,phy_tune:OK,phy_reset:OK,"
                       "ephy:OK,clock:OK,mac:OK,phy:OK,aneg_start:OK "
                       "first=NONE firmware=SKIPPED"),
          "the journal names the profile, every stage and the skipped "
          "firmware dependency");
    CHECK(log_contains(IMAGE_DIR "/configured.log",
                       "  [NET-PHY-GIG] adv1000=0x0300 "
                       "adv1000_written=0x0300 adv1000_read=OK "
                       "lpa1000=0x0000 lpa1000_read=OK lpa=0x01E1 "
                       "page=0x0000"),
          "the journal carries the 1000BASE-T advertisement, the partner's "
          "status and the PHY page with their read verdicts");
    CHECK(log_contains(IMAGE_DIR "/configured.log",
                       "  [NET-PHY-SAMPLES] bmsr=0x"),
          "the journal carries the BMSR sample line");
    CHECK(log_contains(IMAGE_DIR "/configured.log", " reads=") &&
              log_contains(IMAGE_DIR "/configured.log", " link_seen=") &&
              log_contains(IMAGE_DIR "/configured.log", " aneg_seen="),
          "the sample line counts the readings and whether link/AN were seen");
    CHECK(log_contains(IMAGE_DIR "/configured.log", " link=UP aneg=1 pdown=0"),
          "the PHY record carries the negotiated link");
    CHECK(strstr(serial_text, "[NET-PHY] state=CONFIGURED xid=0x02C9 "
                              "access=MII_PHYAR profile=RTL8168evl/8111evl "
                              "first_error=NONE") != NULL,
          "the serial copy of the record is the same text");
    CHECK(serial_length != 0U && serial_text[serial_length - 1U] == '\n',
          "the serial copy ends its own line");
    {
        char small[64];

        CHECK(appliance_production_network_phy_record_line(
                  &info, small, sizeof(small)) == 0U && small[0] == '\0',
              "a line that does not fit is refused whole, not cut");
    }
}

static void test_refused_probe_is_visible_without_backend(void) {
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;
    struct rtl8168_phy_state phy;
    struct rtl8168_cold_boot_report boot;
    struct appliance_production_network_phy_info info;
    struct rtl8168_bring_up configuration = {
        .rx_descriptor_physical = 0x200000U,
        .tx_descriptor_physical = 0x200100U,
        .rx_buffer_capacity = 2048U,
    };
    const uint8_t mac[6] = {0x52, 0x54, 0, 0x12, 0x34, 0x56};
    machine_reset();
    fixture_init(&fixture, TEST_TX_CONFIG_2C9);
    fixture.phy_registers[0] = 0x1940U;
    fixture.phy_model.fault_enabled = true;
    fixture.phy_model.fault_write = true;
    fixture.phy_model.fault_page = 7U;
    fixture.phy_model.fault_selector = 0xacU;
    fixture.phy_model.fault_reg = 0x18U;
    io = fixture_io(&fixture);
    (void)rtl8168_chip_identify(&io, &chip);
    CHECK(rtl8168_cold_boot(&io, &chip, &configuration, mac, &phy, &boot) ==
              RTL8168_IO_ERROR, "adapter diagnostic fixture refuses PHY tuning");
    memset(&apn, 0, sizeof(apn));
    apn.backend = APN_BACKEND_NONE;
    apn.rtl.chip_identified = true;
    apn.rtl.chip = chip;
    apn.rtl.phy = phy;
    apn.rtl.boot = boot;
    appliance_production_network_boot_phy_info(&info);
    CHECK(info.available && info.state ==
              APPLIANCE_PRODUCTION_NETWORK_PHY_STATE_REFUSED &&
              info.boot_failed && info.first_error && info.bmcr_initial_valid,
          "actual adapter exposes failed PHY probe before backend activation");
    if (info.available) {
        struct diag_export_session session;
        char command[768], path[256];
        char small[32] = "previous contents";
        CHECK(appliance_production_network_phy_failure_line(
                  &info, small, sizeof(small)) == 0U && small[0] == '\0',
              "a failed-transaction record that does not fit is refused whole");
        publish_device_record_then_phy(&info);
        export_phy_info = &info;
        CHECK(diag_export_save(&session) == DIAG_EXPORT_OK,
              "refused production probe is exportable by real diag save");
        CHECK(extract_journal("refused-probe", IMAGE_DIR "/refused-probe.log"),
              "refused adapter probe persists on the journal medium");
        snprintf(command, sizeof(command),
                 "python3 scripts/hfoc-diag-read.py --disk '%s/refused-probe.img' "
                 "--output '%s/refused-export' >'%s/refused-export.log' 2>&1",
                 IMAGE_DIR, IMAGE_DIR, IMAGE_DIR);
        CHECK(system(command) == 0, "refused probe export passes operator CRC reader");
        snprintf(path, sizeof(path), "%s/refused-export/HFOC-DIAG-%08X.txt",
                 IMAGE_DIR, session.id);
        CHECK(log_contains(path, "first=phy_tune") &&
                  log_contains(path, "error=RTL8168_IO_ERROR") &&
                  log_contains(path, "aneg_start:SKIPPED") &&
                  log_contains(path, "bmcr0=0x1940"),
              "refused adapter export keeps first failure and original snapshot");
        CHECK(log_contains(path, "[NET-PHY-FAIL] step=1 phase=WRITE op=WRITE "
                                 "page_req=0x0007 selector_req=0x00AC reg=0x0018") &&
                  log_contains(path, "reason=MMIO_WRITE_FAILED cmd=0x80180006 "
                                     "last=UNREAD polls=0") &&
                  log_contains(path, "restore=OK restore_page=0x0000"),
              "real diag save retains the exact first failed PHY transaction");
        export_phy_info = NULL;
    }
    memset(&apn, 0, sizeof(apn));
    appliance_production_network_boot_phy_info(&info);
    CHECK(!info.available, "no identified RTL probe produces no PHY evidence");
}

/* These literal outcomes distinguish a bad page, an uncompleted command,
 * an MMIO read failure and a rejected command. The production change that
 * must fail this proof is dropping/overwriting the first transaction before
 * publishing it, including after a successful refresh or failed cleanup. */
static void test_first_phy_transaction_is_published(void) {
    static const struct {
        const char *operation;
        const char *result;
        const char *restore;
    } cases[] = {
        {"phase=WRITE op=WRITE page_req=0x0007 selector_req=0x00AC reg=0x0018",
         "reason=MMIO_WRITE_FAILED cmd=0x80180006 last=UNREAD polls=0",
         "restore=OK restore_page=0x0000"},
        {"phase=PAGE op=READ page_req=0x0007 selector_req=0x00AC reg=0x001F",
         "reason=PAGE_MISMATCH cmd=0x001F0000 last=0x801F0000 polls=2 "
         "expected=0x0007 observed=0x0002", "restore=OK restore_page=0x0000"},
        {"phase=READ op=READ page_req=0x0007 selector_req=0x00AC reg=0x0018",
         "reason=PHYAR_TIMEOUT cmd=0x00180000 last=0x00000000 polls=20",
         "restore=OK restore_page=0x0000"},
        {"phase=READ op=READ page_req=0x0007 selector_req=0x00AC reg=0x0018",
         "reason=MMIO_READ_FAILED cmd=0x00180000 last=0x00000000 polls=2",
         "restore=OK restore_page=0x0000"},
        {"phase=WRITE op=WRITE page_req=0x0007 selector_req=0x00AC reg=0x0018",
         "reason=MMIO_WRITE_FAILED cmd=0x80180006 last=UNREAD polls=0",
         "restore=FAILED restore_page=UNREAD"},
        {"phase=WRITE op=WRITE page_req=0x0007 selector_req=0x00AC reg=0x0018",
         "reason=MMIO_WRITE_FAILED cmd=0x80180006 last=UNREAD polls=0",
         "restore=FAILED restore_page=0x0007"},
        {"phase=PAGE op=READ page_req=0x0007 selector_req=0x00AC reg=0x001F",
         "reason=MMIO_READ_FAILED cmd=0x001F0000 last=0x801F0000 polls=2 "
         "expected=NONE observed=UNREAD", "restore=OK restore_page=0x0000"},
        {"phase=PAGE op=READ page_req=0x0007 selector_req=0x00AC reg=0x001F",
         "reason=PHYAR_TIMEOUT cmd=0x001F0000 last=0x00000000 polls=20 "
         "expected=NONE observed=UNREAD", "restore=OK restore_page=0x0000"},
        {"phase=PAGE op=READ page_req=0x0007 selector_req=0x00AC reg=0x001F",
         "reason=MMIO_READ_FAILED cmd=0x001F0000 last=0x00000000 polls=2 "
         "expected=NONE observed=UNREAD", "restore=OK restore_page=0x0000"},
        {"phase=PAGE op=READ page_req=0x0007 selector_req=0x00AC reg=0x001F",
         "reason=MMIO_WRITE_FAILED cmd=0x001F0000 last=UNREAD polls=0 "
         "expected=NONE observed=UNREAD", "restore=OK restore_page=0x0000"},
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct mmio_fixture fixture;
        struct rtl8168_mmio_io io;
        struct rtl8168_chip chip;
        struct rtl8168_phy_state phy;
        struct rtl8168_cold_boot_report boot;
        struct appliance_production_network_phy_info info;
        struct rtl8168_bring_up config = {
            .rx_descriptor_physical = 0x200000U,
            .tx_descriptor_physical = 0x200100U,
            .rx_buffer_capacity = 2048U,
        };
        const uint8_t mac[6] = {0x52,0x54,0,0x12,0x34,0x56};
        machine_reset();
        fixture_init(&fixture, TEST_TX_CONFIG_2C9);
        /* Zero is a characterized EVL response; a different nonzero page
         * is still an invalid echo and must retain the refusal evidence. */
        fixture.phy_model.nonzero_page_readback = i == 1U ? 2U : 0U;
        fixture.phy_payload_extra = i == 1U ? UINT32_C(0x00400000) : 0U;
        fixture.phy_ignore_restore_page = i == 5U;
        fixture.phy_timeout_vendor_read = i == 2U;
        fixture.phy_fail_vendor_poll = i == 3U;
        fixture.phy_fail_payload_read = i == 6U;
        fixture.phy_fail_payload_page = 7U;
        fixture.phy_fail_payload_register = RTL8168_PHY_REG_PAGE;
        fixture.phy_page_verify_fault = i == 7U ? 1U : i == 8U ? 2U : 0U;
        if (i == 9U) {
            fixture.phy_model.fault_enabled = true;
            fixture.phy_model.fault_write = false;
            fixture.phy_model.fault_page = 7U;
            fixture.phy_model.fault_reg = 0x1fU;
            fixture.phy_model.fault_after_writes = 1U;
        }
        if (i == 0U || i == 4U || i == 5U) {
            fixture.phy_model.fault_enabled = true;
            fixture.phy_model.fault_write = true;
            fixture.phy_model.fault_page = 7U;
            fixture.phy_model.fault_selector = 0xacU;
            fixture.phy_model.fault_reg = 0x18U;
            fixture.phy_model.fault_restore = i == 4U;
        }
        io = fixture_io(&fixture);
        (void)rtl8168_chip_identify(&io, &chip);
        CHECK(rtl8168_cold_boot(&io, &chip, &config, mac, &phy, &boot) ==
                  RTL8168_IO_ERROR && boot.failed_stage == RTL8168_STAGE_PHY_TUNE,
              "transaction evidence is reached by production cold-start refusal");
        appliance_production_network_phy_info_from_driver(&chip,&phy,&boot,0,&info);
        appliance_production_network_phy_detail_publish(&info, serial_write);
        CHECK(strstr(serial_text, "[NET-PHY-FAIL] step=1 ") &&
                  strstr(serial_text, cases[i].operation) &&
                  strstr(serial_text, cases[i].result) &&
                  strstr(serial_text, cases[i].restore),
              "published first PHY transaction distinguishes the failure cause");
        CHECK(fixture.phy_early_commands == 0U,
              "failure evidence preserves the PHYAR post-operation pauses");
        if (i == 1U || i >= 6U) {
            const char *pair = i == 1U ?
                "[NET-PHY-PAGE] write_cmd=0x801F0007 write_last=0x00000000 "
                "write_polls=2 read_payload=0x805F0002" :
                "[NET-PHY-PAGE] write_cmd=0x801F0007 write_last=0x00000000 "
                "write_polls=2 read_payload=UNREAD";
            CHECK(strstr(serial_text, pair) != NULL,
                  "page refusal preserves the preceding write and raw payload");
            CHECK(fixture.phy_reads == (i == 9U ? 3U : 4U) &&
                      fixture.phy_writes == 4U &&
                      fixture.phy_completion_samples == (i == 1U || i == 6U ? 4U : 3U) &&
                      fixture.phy_payload_reads == (i == 1U ? 4U : 3U) &&
                      fixture.phy_model.write_count == 1U &&
                      fixture.phy_model.reset_requests == 0U &&
                      fixture.now_us < 10000U &&
                      boot.stages[RTL8168_STAGE_MAC] == RTL8168_STAGE_SKIPPED,
                  "page evidence adds no PHY commands and never bypasses refusal");
            {
                char small[32] = "previous text", line[320];
                CHECK(appliance_production_network_phy_page_line(
                          &info, small, sizeof(small)) == 0U && small[0] == '\0' &&
                          appliance_production_network_phy_page_line(
                          &info, line, sizeof(line)) == strlen(pair) &&
                          strcmp(line, pair) == 0,
                      "page pair fits the console and refuses a truncated record");
            }
            if (i == 1U) {
                struct diag_export_session session;
                char command[768], path[256];
                publish_device_record_then_phy(&info);
                export_phy_info = &info;
                CHECK(diag_export_save(&session) == DIAG_EXPORT_OK,
                      "page mismatch is saved by production diag save");
                CHECK(extract_journal("page-pair", IMAGE_DIR "/page-pair.log"),
                      "page write and failed read reach the real journal");
                snprintf(command, sizeof(command),
                         "python3 scripts/hfoc-diag-read.py --disk '%s/page-pair.img' "
                         "--output '%s/page-pair-export' >'%s/page-pair-export.log' 2>&1",
                         IMAGE_DIR, IMAGE_DIR, IMAGE_DIR);
                CHECK(system(command) == 0,
                      "page evidence export passes the operator CRC reader");
                snprintf(path, sizeof(path),
                         "%s/page-pair-export/HFOC-DIAG-%08X.txt",
                         IMAGE_DIR, session.id);
                CHECK(log_contains(path, pair) &&
                          log_contains(path, "reason=PAGE_MISMATCH") &&
                          log_contains(path, "aneg_start:SKIPPED"),
                      "diag export retains the page pair and truthful refusal");
                export_phy_info = NULL;
            }
        } else {
            CHECK(strstr(serial_text, "[NET-PHY-PAGE]") == NULL,
                  "ordinary register refusal has no unrelated page-write evidence");
        }
        fixture.phy_ignore_vendor_page = false;
        fixture.phy_ignore_restore_page = false;
        fixture.phy_timeout_vendor_read = false;
        fixture.phy_fail_vendor_poll = false;
        fixture.phy_fail_payload_read = false;
        fixture.phy_page_verify_fault = 0U;
        fixture.phy_model.fault_restore = false;
        unsigned char original[sizeof(phy.failure)];
        memcpy(original, &phy.failure, sizeof(original));
        CHECK(rtl8168_phy_refresh(&io,&phy) == RTL8168_OK,
              "transaction fixture can later obtain a valid live snapshot");
        serial_length = 0U;
        serial_text[0] = '\0';
        appliance_production_network_phy_info_from_driver(&chip,&phy,&boot,0,&info);
        appliance_production_network_phy_detail_publish(&info,serial_write);
        CHECK(strstr(serial_text,cases[i].operation) &&
                  strstr(serial_text,cases[i].result) &&
                  strstr(serial_text,cases[i].restore),
              "later successful refresh does not overwrite the first transaction");
        if (i == 1U || i >= 6U) {
            CHECK(strstr(serial_text,
                      "[NET-PHY-PAGE] write_cmd=0x801F0007 write_last=0x00000000 "
                      "write_polls=2 read_payload=") != NULL,
                  "later refresh cannot replace the original page write with cleanup");
        }
        fixture.phy_never_completes = true;
        CHECK(rtl8168_phy_refresh(&io, &phy) == RTL8168_IO_ERROR,
              "later PHY access failure is also bounded");
        serial_length = 0U;
        serial_text[0] = '\0';
        appliance_production_network_phy_info_from_driver(&chip,&phy,&boot,0,&info);
        appliance_production_network_phy_detail_publish(&info,serial_write);
        CHECK(strstr(serial_text,cases[i].operation) &&
                  strstr(serial_text,cases[i].result) &&
                  strstr(serial_text,cases[i].restore),
              "later failed refresh retains the original transaction and cleanup");
        CHECK(memcmp(&phy.failure, original, sizeof(original)) == 0,
              "successful and failed refreshes retain all first page-pair bytes");
    }
}

static void test_negotiation_evidence_is_observation(void) {
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;
    struct rtl8168_phy_state phy;
    struct rtl8168_cold_boot_report boot;
    struct appliance_production_network_phy_info info;
    struct rtl8168_bring_up configuration = {
        .rx_descriptor_physical = 0x200000U,
        .tx_descriptor_physical = 0x200100U,
        .rx_buffer_capacity = 2048U,
    };
    const uint8_t mac[6] = {0x52, 0x54, 0, 0x12, 0x34, 0x56};
    char line[APPLIANCE_PRODUCTION_NETWORK_PHY_RECORD_MAX];
    machine_reset();
    fixture_init(&fixture, TEST_TX_CONFIG_2C9);
    fixture.link_up = false;
    fixture.phy_registers[0] = 0x1940U;
    fixture.phy_registers[1] = 0x7949U;
    io = fixture_io(&fixture);
    (void)rtl8168_chip_identify(&io, &chip);
    CHECK(rtl8168_cold_boot(&io, &chip, &configuration, mac, &phy, &boot) ==
              RTL8168_OK, "negotiation evidence uses production cold start");
    appliance_production_network_phy_info_from_driver(&chip, &phy, &boot, 0,
                                                      &info);
    CHECK(appliance_production_network_phy_boot_line(&info, line, sizeof(line)) &&
              strstr(line, "aneg_start:OK") && strstr(line, "aneg_done=NO") &&
              strstr(line, "bmcr0=0x1940") && !strstr(line, "aneg:OK"),
          "command success and incomplete negotiation are separate evidence");
    publish_device_record_then_phy(&info);
    fixture.phy_registers[1] = 0x796dU;
    CHECK(rtl8168_phy_refresh(&io, &phy) == RTL8168_OK,
          "late hardware negotiation completion is sampled");
    appliance_production_network_phy_info_from_driver(&chip, &phy, &boot, 0,
                                                      &info);
    CHECK(appliance_production_network_phy_boot_line(&info, line, sizeof(line)) &&
              strstr(line, "aneg_done=YES") && strstr(line, "bmcr0=0x1940"),
          "observed completion updates while original BMCR stays intact");
    fixture.phy_never_completes = true;
    CHECK(rtl8168_phy_refresh(&io, &phy) == RTL8168_IO_ERROR,
          "failed live read refuses current negotiation evidence");
    appliance_production_network_phy_info_from_driver(&chip, &phy, &boot, 0,
                                                      &info);
    CHECK(appliance_production_network_phy_boot_line(&info, line, sizeof(line)) &&
              strstr(line, "aneg_done=UNKNOWN") && strstr(line, "bmcr0=0x1940"),
          "failed live read does not publish stale completion");
    CHECK(appliance_production_network_phy_record_line(&info, line, sizeof(line)) &&
              strstr(line, "status_read=FAILED") &&
              strstr(line, "link=UNKNOWN aneg=UNKNOWN"),
          "failed status read is explicit in the operator record");
    publish_device_record_then_phy(&info);
    CHECK(extract_journal("negotiation", IMAGE_DIR "/negotiation.log") == 1U &&
              log_contains(IMAGE_DIR "/negotiation.log", "aneg_done=NO") &&
              log_contains(IMAGE_DIR "/negotiation.log", "aneg_done=UNKNOWN") &&
              !log_contains(IMAGE_DIR "/negotiation.log", "aneg:OK"),
          "operator extractor preserves command, observation and read validity");
    {
        struct diag_export_session session;
        char path[256];
        char command[768];
        export_phy_info = &info;
        CHECK(diag_export_save(&session) == DIAG_EXPORT_OK &&
                  session.records == 8U,
              "real diag save serializes all five production PHY records");
        CHECK(extract_journal("diag-phy", IMAGE_DIR "/diag-phy.log") == 1U,
              "exported PHY session is persisted on the model medium");
        snprintf(command, sizeof(command),
                 "python3 scripts/hfoc-diag-read.py --disk '%s/diag-phy.img' "
                 "--output '%s/export' >'%s/export.log' 2>&1",
                 IMAGE_DIR, IMAGE_DIR, IMAGE_DIR);
        CHECK(system(command) == 0,
              "operator diag reader verifies exported PHY session CRC");
        snprintf(path, sizeof(path), "%s/export/HFOC-DIAG-%08X.txt",
                 IMAGE_DIR, session.id);
        CHECK(log_contains(path, "build=host-593") &&
                  log_contains(path, "aneg_start:OK") &&
                  log_contains(path, "aneg_done=UNKNOWN") &&
                  log_contains(path, "status_read=FAILED") &&
                  log_contains(path, "bmcr0=0x1940") &&
                  log_contains(path, "[NET-PHY-FAIL] step=0 phase=DIRECT "
                                     "op=WRITE page_req=0x0000 selector_req=NONE "
                                     "reg=0x001F reason=PHYAR_TIMEOUT") &&
                  log_contains(path, "restore=SKIPPED restore_page=UNREAD"),
              "verified diag export retains original snapshots and read validity");
        export_phy_info = NULL;
    }
}

static void test_refused_phy_keeps_its_first_error(void) {
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;
    struct rtl8168_phy_state phy;
    struct appliance_production_network_phy_info info;

    machine_reset();
    fixture_init(&fixture, TEST_TX_CONFIG_2C9);
    fixture.phy_never_completes = true;
    io = fixture_io(&fixture);
    memset(&chip, 0, sizeof(chip));
    memset(&phy, 0, sizeof(phy));
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK,
          "the part is identified even though its PHY never answers");
    phy.access = rtl8168_phy_access_method(&chip);
    CHECK(rtl8168_phy_configure(&io, &phy) == RTL8168_IO_ERROR,
          "a PHY that never completes the handshake refuses the configuration");
    CHECK(phy.configure_attempted && phy.first_error == RTL8168_IO_ERROR,
          "the driver keeps the configuration attempt and its first error");
    appliance_production_network_phy_info_from_driver(&chip, &phy, NULL, 3U,
                                                      &info);
    CHECK(info.state == APPLIANCE_PRODUCTION_NETWORK_PHY_STATE_REFUSED,
          "the answer is REFUSED");
    CHECK(info.reason != NULL && strcmp(info.reason, "PHY_REFUSED") == 0,
          "the refusal names itself");

    publish_device_record_then_phy(&info);
    /* A later poll finally reads the PHY; the evidence of the refusal must
       survive it, or the next hardware run sees a healthy part and no reason
       the first configuration was lost. */
    fixture.phy_never_completes = false;
    CHECK(rtl8168_phy_refresh(&io, &phy) == RTL8168_OK,
          "a later poll can read the PHY");
    CHECK(phy.first_error == RTL8168_IO_ERROR,
          "the first error survives the later successful poll");
    appliance_production_network_phy_info_from_driver(&chip, &phy, NULL, 7U,
                                                      &info);
    CHECK(info.state == APPLIANCE_PRODUCTION_NETWORK_PHY_STATE_REFUSED &&
              info.first_error != NULL &&
              strcmp(info.first_error, "RTL8168_IO_ERROR") == 0,
          "the answer is still REFUSED with the first error named");
    publish_device_record_then_phy(&info);

    CHECK(extract_journal("refused", IMAGE_DIR "/refused.log") == 1U,
          "the refused medium is readable by the operator's reader");
    CHECK(log_contains(IMAGE_DIR "/refused.log",
                       "  [NET-PHY] state=REFUSED xid=0x02C9 "
                       "access=MII_PHYAR profile=RTL8168evl/8111evl "
                       "first_error=RTL8168_IO_ERROR reason=PHY_REFUSED"),
          "the journal carries the refusal with the first error and reason");
    CHECK(log_contains(IMAGE_DIR "/refused.log", "first_error=RTL8168_IO_ERROR "
                                                 "reason=PHY_REFUSED") &&
              !log_contains(IMAGE_DIR "/refused.log", "[NET-PHY] state=REFUSED "
                                                      "xid=0x02C9 access=MII_PHYAR "
                                                      "first_error=RTL8168_IO_ERROR "
                                                      "reason=PHY_REFUSED phyid="),
          "a refused PHY prints no register it never read");
    {
        unsigned count = 0U;
        char line[1024];
        FILE *handle = fopen(IMAGE_DIR "/refused.log", "rb");

        if (handle != NULL) {
            while (fgets(line, sizeof(line), handle) != NULL) {
                if (strstr(line, "[NET-PHY] state=REFUSED") != NULL) {
                    ++count;
                }
            }
            fclose(handle);
        }
        CHECK(count == 2U,
              "both polls' refusals reached the journal, each with its own "
              "record");
    }
}

static void test_skipped_phy_names_its_reason(void) {
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;
    struct rtl8168_phy_state phy;
    struct appliance_production_network_phy_info info;

    machine_reset();
    fixture_init(&fixture, TEST_TX_CONFIG_CLASSIC);
    io = fixture_io(&fixture);
    memset(&chip, 0, sizeof(chip));
    memset(&phy, 0, sizeof(phy));
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK,
          "a classic part is identified");
    phy.access = rtl8168_phy_access_method(&chip);
    CHECK(phy.access == RTL8168_PHY_ACCESS_NONE,
          "a classic part has no PHY management window");
    appliance_production_network_phy_info_from_driver(&chip, &phy, NULL, 1U,
                                                      &info);
    CHECK(info.state == APPLIANCE_PRODUCTION_NETWORK_PHY_STATE_SKIPPED,
          "the answer is SKIPPED");
    CHECK(info.reason != NULL &&
              strcmp(info.reason, "NO_PHY_MANAGEMENT") == 0,
          "the skip names the missing PHY management");

    publish_device_record_then_phy(&info);
    CHECK(extract_journal("skipped", IMAGE_DIR "/skipped.log") == 1U,
          "the skipped medium is readable by the operator's reader");
    CHECK(log_contains(IMAGE_DIR "/skipped.log",
                       "  [NET-PHY] state=SKIPPED xid=0x0300 access=NONE "
                       "profile=RTL8168-classic first_error=NONE "
                       "reason=NO_PHY_MANAGEMENT"),
          "the journal carries the skipped record with its reason");
}

/* The contract the fix rests on: a fragment after a record's terminator is not
   part of that record.  Pinned here so a future emitter cannot quietly return
   to appending the PHY record to the device record. */
static void test_a_fragment_after_the_terminator_is_not_a_record(void) {
    machine_reset();
    console_journal_begin();
    console_record_text("[PRODUCTION-NET] device=PRESENT\n");
    console_record_text("[NET-PHY] state=CONFIGURED");
    console_journal_end();
    CHECK(records_published == 1U,
          "one record, not two, when the fragments follow a terminator");
    CHECK(extract_journal("contract", IMAGE_DIR "/contract.log") == 1U,
          "the contract's medium is readable");
    CHECK(!log_contains(IMAGE_DIR "/contract.log", "[NET-PHY]"),
          "the fragment after the terminator belongs to no record");
}

/* ITEM-597: real cold boot -> PHY formatter -> journal -> diag save -> CRC
 * reader. Dropping the EVL zero observation or falsely calling it verified
 * must fail this proof, even when the PHY commands themselves complete. */
static void test_evl_zero_readback_is_published(void) {
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;
    struct rtl8168_phy_state phy;
    struct rtl8168_cold_boot_report boot;
    struct appliance_production_network_phy_info info;
    struct diag_export_session session;
    struct rtl8168_bring_up config = {
        .rx_descriptor_physical = 0x200000U,
        .tx_descriptor_physical = 0x200100U,
        .rx_buffer_capacity = 2048U,
    };
    const uint8_t mac[6] = {0x52,0x54,0,0x12,0x34,0x56};
    const char *evidence = "[NET-PHY-SELECT] policy=EVL_ZERO_UNVERIFIED "
        "zero_reads=14 requested=0x0007 observed=0x0000";
    char command[768], path[256];
    machine_reset();
    fixture_init(&fixture, TEST_TX_CONFIG_2C9);
    fixture.link_up = false;
    fixture.phy_registers[1] = 0x7949U;
    fixture.phy_registers[3] = 0xc915U;
    fixture.phy_model.page_readback_zero = true;
    io = fixture_io(&fixture);
    (void)rtl8168_chip_identify(&io, &chip);
    CHECK(rtl8168_cold_boot(&io, &chip, &config, mac, &phy, &boot) == RTL8168_OK,
          "zero page readback reaches the production journal path");
    appliance_production_network_phy_info_from_driver(&chip,&phy,&boot,0,&info);
    publish_device_record_then_phy(&info);
    CHECK(strstr(serial_text, evidence) &&
              strstr(serial_text, "write_cmd=0x801F0007") &&
              strstr(serial_text, "read_payload=0x801F0000"),
          "zero selector policy retains actual requested page and raw read");
    export_phy_info = &info;
    CHECK(diag_export_save(&session) == DIAG_EXPORT_OK,
          "zero selector observation is saved by production diag save");
    CHECK(extract_journal("zero-select", IMAGE_DIR "/zero-select.log"),
          "zero selector observation reaches the real journal reader");
    snprintf(command,sizeof(command),
        "python3 scripts/hfoc-diag-read.py --disk '%s/zero-select.img' "
        "--output '%s/zero-select-export' >'%s/zero-select-export.log' 2>&1",
        IMAGE_DIR,IMAGE_DIR,IMAGE_DIR);
    CHECK(system(command) == 0,
          "zero selector diag export passes the operator CRC reader");
    snprintf(path,sizeof(path),"%s/zero-select-export/HFOC-DIAG-%08X.txt",
             IMAGE_DIR,session.id);
    CHECK(log_contains(path,evidence) && log_contains(path,"aneg_done=NO") &&
              log_contains(path,"firmware=SKIPPED") &&
              log_contains(path,"link=DOWN aneg=0"),
          "export names unverified selector and never invents physical success");
    export_phy_info = NULL;
}

/*
 * ITEM-645: the production adapter's acknowledge policy, driven through the
 * real register accessors (not the driver fixture).  The window is plain RAM,
 * so a write stores the acknowledge mask instead of applying the part's
 * write-one-to-clear; tests/rtl8168-host.c owns that model.  What this proof
 * owns is the adapter's decision: which mask it writes on a drained pass and
 * on a busy one, and that one latch is counted once.
 */
static void test_adapter_acknowledges_receive_stop(void) {
    static union {
        uint8_t bytes[APN_RTL_REGISTER_LENGTH];
        uint16_t align;
    } registers;

    memset(&apn, 0, sizeof(apn));
    memset(&registers, 0, sizeof(registers));
    apn.rtl.region = registers.bytes;
    apn.rtl.io.context = &apn;
    apn.rtl.io.read8 = apn_rtl_read8;
    apn.rtl.io.read16 = apn_rtl_read16;
    apn.rtl.io.read32 = apn_rtl_read32;
    apn.rtl.io.write8 = apn_rtl_write8;
    apn.rtl.io.write16 = apn_rtl_write16;
    apn.rtl.io.write32 = apn_rtl_write32;

    store16(registers.bytes, RTL8168_REG_INTR_STATUS, 0x00F5U);
    apn_rtl_ack_events(true);
    CHECK(load16(registers.bytes, RTL8168_REG_INTR_STATUS) == 0x00F5U,
          "ITEM-645: a drained pass writes every observed bit back");
    CHECK(apn.rtl.rx_overflow == 1U && apn.rtl.rx_overflow_acks == 1U &&
              !apn.rtl.rx_overflow_latched,
          "ITEM-645: the cleared stop event is counted once");
    store16(registers.bytes, RTL8168_REG_INTR_STATUS, 0U);

    store16(registers.bytes, RTL8168_REG_INTR_STATUS, 0x00F5U);
    apn_rtl_ack_events(false);
    CHECK(load16(registers.bytes, RTL8168_REG_INTR_STATUS) == 0x00A5U &&
              apn.rtl.rx_overflow == 2U && apn.rtl.rx_overflow_acks == 1U &&
              apn.rtl.rx_overflow_latched,
          "ITEM-645: a busy ring writes the others and keeps the stop pair");
    store16(registers.bytes, RTL8168_REG_INTR_STATUS, 0x00F5U);
    apn_rtl_ack_events(false);
    CHECK(load16(registers.bytes, RTL8168_REG_INTR_STATUS) == 0x00A5U &&
              apn.rtl.rx_overflow == 2U,
          "ITEM-645: a repeated busy pass does not count the same event twice");

    store16(registers.bytes, RTL8168_REG_INTR_STATUS, 0x00F5U);
    apn_rtl_ack_events(true);
    CHECK(load16(registers.bytes, RTL8168_REG_INTR_STATUS) == 0x00F5U &&
              apn.rtl.rx_overflow_acks == 2U && !apn.rtl.rx_overflow_latched,
          "ITEM-645: the next drained pass clears and counts the recovery");

    store16(registers.bytes, RTL8168_REG_INTR_STATUS, 0U);
    apn_rtl_ack_events(true);
    CHECK(load16(registers.bytes, RTL8168_REG_INTR_STATUS) == 0U &&
              apn.rtl.rx_overflow == 2U && apn.rtl.rx_overflow_acks == 2U,
          "ITEM-645: a quiet pass changes nothing");
    memset(&apn, 0, sizeof(apn));
}

int main(void) {
    if (system("mkdir -p " IMAGE_DIR " >/dev/null 2>&1") == -1) {
        fprintf(stderr, "cannot create %s\n", IMAGE_DIR);
        return 2;
    }
    test_phy_reference_wait_reports_completion();
    test_cold_adapter_delay_is_measured();
    test_refused_probe_is_visible_without_backend();
    test_first_phy_transaction_is_published();
    test_configured_phy_names_itself();
    test_evl_zero_readback_is_published();
    test_negotiation_evidence_is_observation();
    test_refused_phy_keeps_its_first_error();
    test_skipped_phy_names_its_reason();
    test_a_fragment_after_the_terminator_is_not_a_record();
    test_adapter_acknowledges_receive_stop();

    if (failures != 0U) {
        printf("\n%u FAILURES of %u checks\n", failures, checks);
        return 1;
    }
    printf("PHY record journal host test: %u checks ALL OK\n", checks);
    return 0;
}
