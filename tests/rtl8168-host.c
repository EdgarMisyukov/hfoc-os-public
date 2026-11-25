#include "rtl8168.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rtl8168-phy-model.h"

#ifdef HFOC_RTL8168_HOST_TEST

#define TEST_XID_8168H 0x541U
#define TEST_XID_8168B 0x300U
/* ITEM-399: the RTL8168E the product must support, PCI 10EC:8168 rev 07. */
#define TEST_XID_8168E 0x2C8U
#define TEST_TX_CONFIG_8168H ((uint32_t)TEST_XID_8168H << 20U)
#define TEST_TX_CONFIG_8168E ((uint32_t)TEST_XID_8168E << 20U)
/* ITEM-435: a classic-family part, whose MISC register is not the gate's. */
#define TEST_TX_CONFIG_CLASSIC ((uint32_t)TEST_XID_8168B << 20U)
#define TEST_MMIO_LENGTH 512U
#define TEST_MMIO_BASE UINT64_C(0xFE000000)
#define TEST_DMA_BASE UINT64_C(0x200000)
#define TEST_BUFFER_CAPACITY 2048U
#define TEST_DESCRIPTOR_COUNT 16U
#define TEST_FRAME_LENGTH 64U

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
    uint8_t registers[TEST_MMIO_LENGTH];
    bool fail_read;
    bool fail_write;
    bool reset_never_clears;
    /* A part whose enable write is not confirmed by the read-back. */
    bool enable_never_sticks;
    bool link_up;
    uint32_t eri_mac_low;
    uint32_t eri_mac_high;
    /* Observations */
    uint32_t chip_cmd_writes;
    uint32_t reset_writes;
    uint32_t rx_config_writes;
    uint32_t rx_config_value;
    uint32_t tx_config_writes;
    uint32_t tx_config_value;
    uint32_t mar_writes;
    uint32_t mar_values[2];
    uint32_t mac_low;
    uint32_t mac_high;
    uint32_t mac_writes;
    uint8_t cfg9346_writes;
    uint8_t cfg9346_value;
    uint16_t intr_mask;
    uint16_t intr_status;
    /*
     * ITEM-645: IntrStatus is write-one-to-clear on this part, so a fixture
     * that stored the written word would claim an acknowledged event is still
     * pending.  The model clears exactly the bits the driver wrote and keeps
     * the last mask and the number of writes observable.
     */
    uint32_t intr_status_writes;
    uint16_t intr_status_ack;
    uint16_t rx_max_size;
    uint16_t cplus_cmd;
    uint32_t tx_poll_writes;
    /*
     * ITEM-435: whether the receive data-valid gate was already open at the
     * moment the driver enabled the receiver - the ordering the reference
     * driver's `rtl_hw_start_8168h_1()` guarantees and the one that decides
     * whether the gate is open for the frames the part then receives.
     */
    bool gate_open_before_enable;
    bool enable_seen;
    /* ITEM-583: the clause-22 MII register file behind PHYAR (0x60). */
    uint16_t phy_registers[32];
    struct phy_model phy_model;
    bool phy_never_completes;
    uint32_t phy_reads;
    uint32_t phy_writes;
    uint16_t phy_last_write_register;
    uint16_t phy_last_write_value;
    /*
     * ITEM-591: the PHYAR handshake's clock.
     *
     * The part requires a gap after every completion indication before the
     * next command.  A fixture that answers instantly cannot observe whether
     * the driver honoured it, so the model owns a microsecond clock: every
     * 32-bit MMIO access costs `mmio_us`, an operation completes
     * `phy_complete_us` after its command, and `delay_us()` advances the same
     * clock.  A command that arrives before `phy_last_complete_us +
     * RTL8168_PHY_ACCESS_SETTLE_US` is counted in `phy_early_commands` - the
     * model rejects it the way the hardware would misbehave, instead of the
     * proof trusting the driver's own arithmetic.
     */
    uint64_t now_us;
    uint32_t mmio_us;
    uint32_t phy_complete_us;
    /* A part that answers register 4 with fewer bits than it was given. */
    uint16_t phy_advertise_write_mask;
    /* A register whose read never completes (the read-back failure path). */
    int16_t phy_fail_register;
    uint64_t phy_busy_at_us;
    uint64_t phy_last_complete_us;
    uint64_t phy_last_poll_us;
    bool phy_last_complete_valid;
    bool phy_operation_pending;
    bool phy_complete_observed;
    bool phy_pending_read;
    bool phy_poll_seen;
    uint16_t phy_pending_register;
    /*
     * ITEM-595: Linux checks completion and then reads PHYAR a second time for
     * the payload.  The completion sample does not promise stable low16 data.
     */
    uint16_t phy_completion_sample_data;
    bool phy_completion_sample_pending;
    bool phy_fail_payload_read;
    uint16_t phy_fail_payload_page;
    uint16_t phy_fail_payload_register;
    uint32_t phy_completion_samples;
    uint32_t phy_payload_reads;
    uint32_t phy_payload_read_failures;
    uint32_t phy_early_commands;
    uint32_t phy_polls;
    uint32_t phy_poll_gap_min_us;
    uint32_t phy_delays;
    uint32_t phy_settle_delays;
    uint32_t phy_delay_us_max;
    uint32_t phy_delay_us_total;
    /*
     * ITEM-592: the cold-state part.
     *
     * The Toshiba's PHY answered everything and never saw a link partner, so
     * the proof has to be about the sequence as much as about the registers:
     * this model knows which stage the start is in, refuses an access that
     * arrives before the documented stage, refuses a generation-forbidden
     * write, keeps the EPHY and extended-register files and a PHY page, and
     * can fail one of them to drive the refusal paths.
     */
    enum fixture_stage {
        FX_NONE = 0,
        FX_RESET = 1,
        FX_EPHY = 2,
        FX_CLOCK = 3,
        FX_MAC = 4,
        FX_PHY = 5,
    } stage;
    bool evl;
    bool reset_seen;
    bool mac_enabled;
    bool phy_touched;
    uint32_t order_violations;
    uint32_t forbidden_writes;
    uint32_t bad_register_accesses;
    uint16_t phy_page;
    bool fail_ephy;
    bool fail_eri;
    uint16_t ephy_registers[32];
    uint32_t ephy_reads;
    uint32_t ephy_writes;
    uint64_t ephy_busy_at_us;
    bool ephy_pending;
    bool ephy_pending_read;
    bool ephy_complete_observed;
    uint16_t ephy_pending_register;
    uint32_t eri_registers[2048];
    uint32_t eri_writes;
    uint8_t max_tx_packet_size;
    uint16_t intr_mitigate;
};

static void store32(uint8_t *base, uint32_t offset, uint32_t value)
{
    base[offset] = (uint8_t)(value & 0xFFU);
    base[offset + 1U] = (uint8_t)((value >> 8U) & 0xFFU);
    base[offset + 2U] = (uint8_t)((value >> 16U) & 0xFFU);
    base[offset + 3U] = (uint8_t)((value >> 24U) & 0xFFU);
}

static uint32_t load32(const uint8_t *base, uint32_t offset)
{
    return (uint32_t)base[offset] |
           ((uint32_t)base[offset + 1U] << 8U) |
           ((uint32_t)base[offset + 2U] << 16U) |
           ((uint32_t)base[offset + 3U] << 24U);
}

static void store16(uint8_t *base, uint32_t offset, uint16_t value)
{
    base[offset] = (uint8_t)(value & 0xFFU);
    base[offset + 1U] = (uint8_t)((value >> 8U) & 0xFFU);
}

static uint16_t load16(const uint8_t *base, uint32_t offset)
{
    return (uint16_t)((uint16_t)base[offset] |
                      ((uint16_t)base[offset + 1U] << 8U));
}

/* ITEM-591: the PHYAR model's clock and completion state. */
#define TEST_MMIO_US 1U

static void fixture_advance(struct mmio_fixture *fixture, uint32_t micros)
{
    fixture->now_us += micros;
}

/*
 * Whether the pending operation has completed.  A part that never answers - or
 * that never answers for one register - never completes, which is how the
 * bounded wait's failure path is driven.
 */
static bool fixture_phy_completed(const struct mmio_fixture *fixture)
{
    if (!fixture->phy_operation_pending || fixture->phy_never_completes) {
        return false;
    }
    /* The read-back failure is a read that never completes; the write of the
       same register still answers. */
    if (fixture->phy_pending_read && fixture->phy_page == 0U &&
        fixture->phy_fail_register >= 0 &&
        fixture->phy_pending_register ==
            (uint16_t)fixture->phy_fail_register) {
        return false;
    }
    return fixture->now_us >= fixture->phy_busy_at_us;
}

/*
 * What PHYAR reads as.  The busy bit is the operation's own handshake: a write
 * holds it until the part is done with it, a read has no data until the part
 * presents the busy bit together with the register's value.
 */
static uint32_t fixture_phy_signal(struct mmio_fixture *fixture)
{
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
        return RTL8168_PHY_ACCESS_BUSY | selector |
               phy_model_read(&fixture->phy_model, fixture->phy_registers,
                              fixture->phy_page, fixture->phy_pending_register,
                              fixture->now_us);
    }
    return 0U;
}

/*
 * The completion indication: the moment the model first reports the condition
 * the driver is polling for.  That is the point the hardware spec's 20 us gap
 * is measured from, so it is recorded here and the next command is checked
 * against it.
 */
static void fixture_phy_note_completion(struct mmio_fixture *fixture)
{
    if (!fixture->phy_complete_observed && fixture_phy_completed(fixture)) {
        fixture->phy_complete_observed = true;
        fixture->phy_last_complete_us = fixture->now_us;
        fixture->phy_last_complete_valid = true;
    }
}

static void fixture_phy_start(struct mmio_fixture *fixture, bool read,
                              uint16_t reg)
{
    if (fixture->phy_last_complete_valid &&
        fixture->now_us < fixture->phy_last_complete_us +
                              RTL8168_PHY_ACCESS_SETTLE_US) {
        /* A command inside the settle window: the part would be addressed
           before it is ready, which is exactly the defect this model is for. */
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

/* ITEM-592: the EPHY window's completion, the same shape as the PHY one. */
static bool fixture_ephy_completed(const struct mmio_fixture *fixture)
{
    return fixture->ephy_pending && !fixture->fail_ephy &&
           fixture->now_us >= fixture->ephy_busy_at_us;
}

static uint32_t fixture_ephy_signal(struct mmio_fixture *fixture)
{
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

static void fixture_delay_us(void *context, uint32_t microseconds)
{
    struct mmio_fixture *fixture = (struct mmio_fixture *)context;

    if (fixture == NULL) {
        return;
    }
    fixture->now_us += microseconds;
    ++fixture->phy_delays;
    fixture->phy_delay_us_total += microseconds;
    if (microseconds > fixture->phy_delay_us_max) {
        fixture->phy_delay_us_max = microseconds;
    }
    if (microseconds == RTL8168_PHY_ACCESS_SETTLE_US) {
        ++fixture->phy_settle_delays;
    }
}

/* ITEM-592: the cold-start model's stage bookkeeping. */
static void fixture_stage_reach(struct mmio_fixture *fixture,
                                enum fixture_stage stage)
{
    if (stage > fixture->stage) {
        fixture->stage = stage;
    }
}

static bool fixture_read8(void *context, uint32_t offset, uint8_t *value)
{
    struct mmio_fixture *fixture = (struct mmio_fixture *)context;

    if (fixture == NULL || value == NULL || offset >= TEST_MMIO_LENGTH ||
        fixture->fail_read) {
        return false;
    }
    *value = fixture->registers[offset];
    if (offset == RTL8168_REG_CHIP_CMD && fixture->enable_never_sticks) {
        /*
         * A device that never latches the receiver/transmitter enable: the
         * register reads back without the bits the driver programmed.
         */
        *value = (uint8_t)(*value &
                           (uint8_t)~(RTL8168_CHIP_CMD_RX_ENABLE |
                                      RTL8168_CHIP_CMD_TX_ENABLE));
    }
    return true;
}

static bool fixture_read16(void *context, uint32_t offset, uint16_t *value)
{
    struct mmio_fixture *fixture = (struct mmio_fixture *)context;

    if (fixture == NULL || value == NULL ||
        offset + 2U > TEST_MMIO_LENGTH || fixture->fail_read) {
        return false;
    }
    *value = load16(fixture->registers, offset);
    return true;
}

static bool fixture_read32(void *context, uint32_t offset, uint32_t *value)
{
    struct mmio_fixture *fixture = (struct mmio_fixture *)context;

    if (fixture == NULL || value == NULL ||
        offset + 4U > TEST_MMIO_LENGTH || fixture->fail_read) {
        return false;
    }
    fixture_advance(fixture, fixture->mmio_us);
    if (offset == RTL8168_REG_EPHY_ACCESS) {
        *value = fixture_ephy_signal(fixture);
        return true;
    }
    if (offset == RTL8168_REG_PHY_ACCESS) {
        /*
         * ITEM-591: the spacing between polls, measured while the operation is
         * still pending.  A driver that reads the busy bit in a tight loop
         * would show a gap of the MMIO cost alone, not the reference driver's
         * 25 us interval.
         */
        if (fixture->phy_operation_pending &&
            !fixture->phy_complete_observed) {
            if (fixture->phy_poll_seen &&
                fixture->now_us > fixture->phy_last_poll_us) {
                const uint32_t gap =
                    (uint32_t)(fixture->now_us - fixture->phy_last_poll_us);

                if (gap < fixture->phy_poll_gap_min_us) {
                    fixture->phy_poll_gap_min_us = gap;
                }
            }
            fixture->phy_poll_seen = true;
            fixture->phy_last_poll_us = fixture->now_us;
            ++fixture->phy_polls;
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

static bool fixture_write8(void *context, uint32_t offset, uint8_t value)
{
    struct mmio_fixture *fixture = (struct mmio_fixture *)context;

    if (fixture == NULL || offset >= TEST_MMIO_LENGTH ||
        fixture->fail_write) {
        return false;
    }
    if (offset == RTL8168_REG_CHIP_CMD) {
        ++fixture->chip_cmd_writes;
        if ((value & RTL8168_CHIP_CMD_RESET) != 0U) {
            ++fixture->reset_writes;
            fixture->reset_seen = true;
            fixture->stage = FX_RESET;
            /* A device that finishes its reset clears the bit by itself. */
            fixture->registers[offset] = fixture->reset_never_clears
                ? (uint8_t)RTL8168_CHIP_CMD_RESET
                : (uint8_t)(value & (uint8_t)~RTL8168_CHIP_CMD_RESET);
            return true;
        }
        if ((value & (uint8_t)RTL8168_CHIP_CMD_RX_ENABLE) != 0U &&
            !fixture->enable_seen) {
            fixture->enable_seen = true;
            fixture->mac_enabled = true;
            /*
             * ITEM-592: the MAC may not be enabled before the EPHY and clock
             * stages on the generation that has them.
             */
            if (fixture->evl && fixture->stage < FX_CLOCK) {
                ++fixture->order_violations;
            }
            fixture_stage_reach(fixture, FX_MAC);
            /* The part remembers whether its gate was open at that moment. */
            fixture->gate_open_before_enable =
                (load32(fixture->registers, RTL8168_REG_MISC) &
                 RTL8168_MISC_RXDV_GATED_EN) == 0U;
        }
    }
    if (offset == RTL8168_REG_MCU || offset == RTL8168_REG_DLLPR ||
        offset == RTL8168_REG_EEE_LED || offset == RTL8168_REG_CONFIG5) {
        if (fixture->evl && fixture->stage < FX_EPHY) {
            ++fixture->order_violations;
        }
        fixture_stage_reach(fixture, FX_CLOCK);
    }
    if (offset == RTL8168_REG_CFG9346) {
        ++fixture->cfg9346_writes;
        fixture->cfg9346_value = value;
    }
    if (offset == RTL8168_REG_MAX_TX_PACKET_SIZE) {
        fixture->max_tx_packet_size = value;
    }
    fixture->registers[offset] = value;
    return true;
}

static bool fixture_write16(void *context, uint32_t offset, uint16_t value)
{
    struct mmio_fixture *fixture = (struct mmio_fixture *)context;

    if (fixture == NULL || offset + 2U > TEST_MMIO_LENGTH ||
        fixture->fail_write) {
        return false;
    }
    if (offset == RTL8168_REG_INTR_MITIGATE) {
        fixture->intr_mitigate = value;
    } else if (offset == RTL8168_REG_INTR_MASK) {
        fixture->intr_mask = value;
    } else if (offset == RTL8168_REG_INTR_STATUS) {
        /*
         * ITEM-645: the part's own semantics - writing 1 to a status bit
         * clears it, writing 0 leaves it.  A plain store here would model the
         * opposite and hide a driver that never clears a latched overflow.
         */
        ++fixture->intr_status_writes;
        fixture->intr_status_ack = value;
        value = (uint16_t)(fixture->intr_status & (uint16_t)~value);
        fixture->intr_status = value;
    } else if (offset == RTL8168_REG_RX_MAX_SIZE) {
        fixture->rx_max_size = value;
    } else if (offset == RTL8168_REG_CPLUS_CMD) {
        fixture->cplus_cmd = value;
    }
    store16(fixture->registers, offset, value);
    return true;
}

static bool fixture_write32(void *context, uint32_t offset, uint32_t value)
{
    struct mmio_fixture *fixture = (struct mmio_fixture *)context;

    if (fixture == NULL || offset + 4U > TEST_MMIO_LENGTH ||
        fixture->fail_write) {
        return false;
    }
    if (offset == RTL8168_REG_PHY_ACCESS) {
        const uint16_t reg = (uint16_t)(
            (value >> RTL8168_PHY_ACCESS_REG_SHIFT) &
            RTL8168_PHY_ACCESS_REG_MASK);

        /*
         * PHY preparation follows MAC reset; advertisement follows MAC
         * enable. The page-aware model owns register interpretation.
         */
        if (fixture->stage < FX_RESET) {
            ++fixture->order_violations;
        }
        fixture->phy_touched = true;
        if (fixture->mac_enabled) {
            fixture_stage_reach(fixture, FX_PHY);
        }
        fixture_advance(fixture, fixture->mmio_us);
        if (!phy_model_accept(&fixture->phy_model, fixture->phy_page, reg,
                              (value & RTL8168_PHY_ACCESS_BUSY) != 0U,
                              (uint16_t)value)) {
            return false;
        }
        if ((value & RTL8168_PHY_ACCESS_BUSY) != 0U) {
            fixture->phy_last_write_register = reg;
            fixture->phy_last_write_value =
                (uint16_t)(value & RTL8168_PHY_ACCESS_DATA_MASK);
            ++fixture->phy_writes;
            /*
             * ITEM-591: a part may answer register 4 with fewer bits than the
             * driver asked for (a PHY without the gigabit pairs masks them
             * off).  The write still carries the driver's value; the read-back
             * is the part's answer.
             */
            phy_model_write(&fixture->phy_model, fixture->phy_registers,
                            &fixture->phy_page, reg,
                            (uint16_t)(fixture->phy_last_write_value &
                                (fixture->phy_page == 0U &&
                                 reg == RTL8168_PHY_REG_ADVERTISE
                                     ? fixture->phy_advertise_write_mask
                                     : 0xFFFFU)), fixture->now_us);
            fixture_phy_start(fixture, false, reg);
        } else {
            ++fixture->phy_reads;
            fixture_phy_start(fixture, true, reg);
        }
        store32(fixture->registers, offset, 0U);
        return true;
    }
    if (offset == RTL8168_REG_EPHY_ACCESS) {
        const uint16_t reg = (uint16_t)(
            (value >> RTL8168_EPHY_ACCESS_REG_SHIFT) &
            RTL8168_EPHY_ACCESS_REG_MASK);

        /*
         * ITEM-592: the EPHY stage belongs to the 8168evl cold start and must
         * precede the MAC enable.  The window completes after the same few
         * microseconds as the PHY window; its own timing is the driver's.
         */
        if (!fixture->evl) {
            ++fixture->forbidden_writes;
        }
        if (fixture->stage < FX_RESET ||
            (fixture->evl && (fixture->phy_model.reset_finished == 0U ||
             fixture->now_us < fixture->phy_model.reset_finished + 1000U))) {
            ++fixture->order_violations;
        }
        fixture_stage_reach(fixture, FX_EPHY);
        fixture_advance(fixture, fixture->mmio_us);
        if (fixture->fail_ephy) {
            return false;
        }
        if ((value & RTL8168_EPHY_ACCESS_WRITE) != 0U) {
            ++fixture->ephy_writes;
            fixture->ephy_registers[reg] =
                (uint16_t)(value & RTL8168_EPHY_ACCESS_DATA_MASK);
            fixture->ephy_pending = true;
            fixture->ephy_pending_read = false;
            fixture->ephy_pending_register = reg;
            fixture->ephy_complete_observed = false;
            fixture->ephy_busy_at_us =
                fixture->now_us + fixture->phy_complete_us;
        } else {
            ++fixture->ephy_reads;
            fixture->ephy_pending = true;
            fixture->ephy_pending_read = true;
            fixture->ephy_pending_register = reg;
            fixture->ephy_complete_observed = false;
            fixture->ephy_busy_at_us =
                fixture->now_us + fixture->phy_complete_us;
        }
        store32(fixture->registers, offset, 0U);
        return true;
    }
    if (offset == RTL8168_REG_ERI_ADDRESS) {
        const uint32_t address = value & RTL8168_ERIAR_ADDRESS_MASK;
        uint32_t data;

        if ((value & RTL8168_ERIAR_WRITE) != 0U) {
            /*
             * ITEM-592: the write half of the extended-register window.  The
             * clock stage is the only place the cold start writes it, and it
             * happens before the MAC is enabled.
             */
            if (fixture->fail_eri) {
                return false;
            }
            if (fixture->stage < FX_RESET) {
                ++fixture->order_violations;
            }
            if (fixture->evl && fixture->stage < FX_EPHY) {
                ++fixture->order_violations;
            }
            fixture_stage_reach(fixture, FX_CLOCK);
            ++fixture->eri_writes;
            if (address < sizeof(fixture->eri_registers) /
                              sizeof(fixture->eri_registers[0])) {
                fixture->eri_registers[address] =
                    load32(fixture->registers, RTL8168_REG_ERI_DATA);
            } else {
                ++fixture->bad_register_accesses;
            }
            store32(fixture->registers, RTL8168_REG_ERI_ADDRESS,
                    value & ~RTL8168_ERIAR_FLAG);
            return true;
        }
        /* The read handshake: the doorbell, then the data register. */
        if (address == RTL8168_ERI_MAC_LOW) {
            data = fixture->eri_mac_low;
        } else if (address == RTL8168_ERI_MAC_HIGH) {
            data = fixture->eri_mac_high;
        } else if (address < sizeof(fixture->eri_registers) /
                               sizeof(fixture->eri_registers[0])) {
            data = fixture->eri_registers[address];
        } else {
            data = 0U;
        }
        store32(fixture->registers, RTL8168_REG_ERI_DATA, data);
        store32(fixture->registers, RTL8168_REG_ERI_ADDRESS,
                value | RTL8168_ERIAR_FLAG);
        return true;
    }
    if (offset == RTL8168_REG_MISC) {
        /*
         * ITEM-592: the receive data-valid gate is a G/H/EP setting.  A write
         * that changes it on the 8168evl is the generation-boundary defect the
         * model is here to catch.
         */
        const uint32_t before =
            load32(fixture->registers, RTL8168_REG_MISC);

        if (fixture->evl &&
            ((before ^ value) & RTL8168_MISC_RXDV_GATED_EN) != 0U) {
            ++fixture->forbidden_writes;
        }
        if (fixture->evl && fixture->stage < FX_EPHY) {
            ++fixture->order_violations;
        }
        fixture_stage_reach(fixture, FX_CLOCK);
    }
    if (offset == RTL8168_REG_RX_CONFIG) {
        ++fixture->rx_config_writes;
        fixture->rx_config_value = value;
        if (fixture->evl && (value & RTL8168_RX_EARLY_OFF) != 0U) {
            ++fixture->forbidden_writes;
        }
    } else if (offset == RTL8168_REG_TX_CONFIG) {
        ++fixture->tx_config_writes;
        fixture->tx_config_value = value;
    } else if (offset == RTL8168_REG_MAR0) {
        ++fixture->mar_writes;
        fixture->mar_values[0] = value;
    } else if (offset == RTL8168_REG_MAR0 + 4U) {
        ++fixture->mar_writes;
        fixture->mar_values[1] = value;
    } else if (offset == RTL8168_REG_MAC0) {
        fixture->mac_low = value;
        ++fixture->mac_writes;
    } else if (offset == RTL8168_REG_MAC4) {
        fixture->mac_high = value;
        ++fixture->mac_writes;
    }
    store32(fixture->registers, offset, value);
    return true;
}

static struct rtl8168_mmio_io fixture_io(struct mmio_fixture *fixture)
{
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

static void fixture_init(struct mmio_fixture *fixture, uint32_t tx_config)
{
    memset(fixture, 0, sizeof(*fixture));
    phy_model_init(&fixture->phy_model);
    /* ITEM-591: the part completes an operation a few microseconds after the
       command; the driver's first poll sees it in progress. */
    fixture->mmio_us = TEST_MMIO_US;
    fixture->phy_complete_us = 3U;
    fixture->phy_advertise_write_mask = 0xFFFFU;
    fixture->phy_fail_register = -1;
    fixture->phy_fail_payload_page = 0xffffU;
    fixture->phy_fail_payload_register = 0xffffU;
    fixture->phy_poll_gap_min_us = UINT32_MAX;
    /* ITEM-592: 0x2C8/0x2C9 is the 8168evl generation, and the fixture starts
       it the way firmware leaves it: the receive gate closed, the PHY MCU in
       out-of-band mode, the SPI clock on.  The cold start has to deal with
       exactly those leftovers without touching generation-foreign bits. */
    fixture->evl = ((tx_config >> 20U) & 0x7C8U) == 0x2C8U;
    if (fixture->evl) {
        store32(fixture->registers, RTL8168_REG_MISC,
                RTL8168_MISC_RXDV_GATED_EN);
        fixture->registers[RTL8168_REG_MCU] =
            (uint8_t)RTL8168_MCU_NOW_IS_OOB;
        fixture->registers[RTL8168_REG_CONFIG5] =
            (uint8_t)RTL8168_CONFIG5_SPI_EN;
        fixture->registers[RTL8168_REG_EEE_LED] =
            (uint8_t)RTL8168_EEE_LED_MASK;
    }
    store32(fixture->registers, RTL8168_REG_TX_CONFIG, tx_config);
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
    if (fixture->link_up) {
        fixture->registers[RTL8168_REG_PHY_STATUS] =
            (uint8_t)RTL8168_PHY_STATUS_LINK;
    }
}

static void fixture_set_phy_link(struct mmio_fixture *fixture, bool up)
{
    if (up) {
        fixture->phy_registers[RTL8168_PHY_REG_BMSR] |=
            RTL8168_PHY_BMSR_LINK | RTL8168_PHY_BMSR_ANEG_COMPLETE;
        fixture->registers[RTL8168_REG_PHY_STATUS] |=
            (uint8_t)RTL8168_PHY_STATUS_LINK;
    } else {
        fixture->phy_registers[RTL8168_PHY_REG_BMSR] &=
            (uint16_t)~(RTL8168_PHY_BMSR_LINK |
                        RTL8168_PHY_BMSR_ANEG_COMPLETE);
        fixture->registers[RTL8168_REG_PHY_STATUS] &=
            (uint8_t)~RTL8168_PHY_STATUS_LINK;
    }
}

static void fixture_set_mac(
    struct mmio_fixture *fixture,
    const uint8_t mac[RTL8168_MAC_LENGTH])
{
    store32(fixture->registers, RTL8168_REG_MAC0,
            (uint32_t)mac[0] | ((uint32_t)mac[1] << 8U) |
            ((uint32_t)mac[2] << 16U) | ((uint32_t)mac[3] << 24U));
    store16(fixture->registers, RTL8168_REG_MAC4,
            (uint16_t)((uint16_t)mac[4] | ((uint16_t)mac[5] << 8U)));
}

static void fixture_set_eri_mac(
    struct mmio_fixture *fixture,
    const uint8_t mac[RTL8168_MAC_LENGTH])
{
    fixture->eri_mac_low =
        (uint32_t)mac[0] | ((uint32_t)mac[1] << 8U) |
        ((uint32_t)mac[2] << 16U) | ((uint32_t)mac[3] << 24U);
    fixture->eri_mac_high =
        (uint32_t)mac[4] | ((uint32_t)mac[5] << 8U);
}

static const uint8_t test_mac[RTL8168_MAC_LENGTH] = {
    0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U
};

static void make_header(
    struct pci_device_header *header,
    uint16_t vendor,
    uint16_t device,
    uint64_t memory_bar_base,
    uint64_t memory_bar_length)
{
    memset(header, 0, sizeof(*header));
    header->present = true;
    header->vendor_id = vendor;
    header->device_id = device;
    header->revision = 15U;
    header->base_class = RTL8168_PCI_CLASS_NETWORK;
    header->subclass = RTL8168_PCI_SUBCLASS_ETHERNET;
    header->bars[0].present = true;
    header->bars[0].io_space = true;
    header->bars[0].base = 0xE000U;
    header->bars[0].length = 256U;
    if (memory_bar_length != 0U) {
        header->bars[1].present = true;
        header->bars[1].base = memory_bar_base;
        header->bars[1].length = memory_bar_length;
    }
}

static void test_pci_identity(void)
{
    struct pci_device_header header;

    make_header(&header, RTL8168_VENDOR_ID, RTL8168_DEVICE_ID,
                TEST_MMIO_BASE, 4096U);
    CHECK(rtl8168_pci_identity_supported(&header),
          "10EC:8168 is the supported PCI function");

    make_header(&header, RTL8168_VENDOR_ID, 0x8169U,
                TEST_MMIO_BASE, 4096U);
    CHECK(!rtl8168_pci_identity_supported(&header),
          "another Realtek function is not claimed by this driver");

    make_header(&header, 0x1AF4U, 0x1041U, TEST_MMIO_BASE, 4096U);
    CHECK(!rtl8168_pci_identity_supported(&header),
          "a virtio function is not claimed by this driver");

    CHECK(!rtl8168_pci_identity_supported(NULL),
          "a missing header is refused");
}

static void test_memory_bar(void)
{
    struct pci_device_header header;
    const struct pci_bar *bar = NULL;

    make_header(&header, RTL8168_VENDOR_ID, RTL8168_DEVICE_ID,
                TEST_MMIO_BASE, 4096U);
    CHECK(rtl8168_memory_bar(&header, &bar) == RTL8168_OK &&
              bar == &header.bars[1],
          "the first memory BAR covering the register file is selected");
    CHECK(bar != NULL && !bar->io_space && bar->length >= 0x100U,
          "the selected BAR is a memory window of at least 256 bytes");

    make_header(&header, RTL8168_VENDOR_ID, RTL8168_DEVICE_ID,
                TEST_MMIO_BASE, 0x80U);
    CHECK(rtl8168_memory_bar(&header, &bar) ==
              RTL8168_MISSING_MEMORY_BAR,
          "a memory BAR too small for the register file is refused");

    make_header(&header, RTL8168_VENDOR_ID, RTL8168_DEVICE_ID, 0U, 0U);
    CHECK(rtl8168_memory_bar(&header, &bar) ==
              RTL8168_MISSING_MEMORY_BAR,
          "a function without a memory BAR is refused");
    CHECK(rtl8168_memory_bar(NULL, &bar) == RTL8168_NULL,
          "a missing header is refused by the BAR selector");
}

static void test_chip_identify(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;

    fixture_init(&fixture, TEST_TX_CONFIG_8168H);
    io = fixture_io(&fixture);
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK,
          "the chip identity is readable");
    CHECK(chip.xid == TEST_XID_8168H,
          "the XID is decoded from TxConfig bits 20..31");
    CHECK(chip.family == RTL8168_CHIP_EXTENDED,
          "the 8168H part is the extended-MAC family");
    CHECK(strcmp(rtl8168_chip_name(&chip), "RTL8168-EXTENDED") == 0,
          "the extended family is named honestly");

    fixture_init(&fixture, (uint32_t)TEST_XID_8168B << 20U);
    io = fixture_io(&fixture);
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK &&
              chip.family == RTL8168_CHIP_CLASSIC,
          "the 8168B part is the classic family");

    fixture_init(&fixture, UINT32_C(0x7FF00000));
    io = fixture_io(&fixture);
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_UNSUPPORTED_DEVICE &&
              chip.family == RTL8168_CHIP_UNKNOWN,
          "an unknown XID is refused instead of driven with guessed values");

    fixture_init(&fixture, TEST_TX_CONFIG_8168H);
    fixture.fail_read = true;
    io = fixture_io(&fixture);
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_IO_ERROR,
          "a failed register read is an I/O error");
}

/*
 * ITEM-399: the two fields the [PRODUCTION-NET] record publishes.  The record
 * used to carry only `pci=10EC:8168 rev=..`, which a dozen parts of the same
 * family answer to, so a hardware run could not state which silicon it drove.
 */
static void test_chip_identity_record(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;
    char text[48U];
    char tiny[16U];

    /*
     * The RTL8168E the product must support: PCI 10EC:8168 rev 07, which the
     * driver's table matches through its XID to the extended-MAC family.
     */
    fixture_init(&fixture, (uint32_t)TEST_XID_8168E << 20U);
    io = fixture_io(&fixture);
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK &&
              chip.xid == TEST_XID_8168E &&
              chip.family == RTL8168_CHIP_EXTENDED,
          "the RTL8168E is identified as an extended-MAC part");
    CHECK(rtl8168_chip_render_identity(&chip, text, sizeof(text)) ==
              strlen(" xid=0x2C8 family=RTL8168-EXTENDED") &&
              strcmp(text, " xid=0x2C8 family=RTL8168-EXTENDED") == 0,
          "the RTL8168E identity is published as xid and family");

    /* And the acceptance host's own part. */
    fixture_init(&fixture, TEST_TX_CONFIG_8168H);
    io = fixture_io(&fixture);
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK &&
              rtl8168_chip_render_identity(&chip, text, sizeof(text)) != 0U &&
              strcmp(text, " xid=0x541 family=RTL8168-EXTENDED") == 0,
          "the acceptance host's RTL8168H is published by its own XID");

    fixture_init(&fixture, (uint32_t)TEST_XID_8168B << 20U);
    io = fixture_io(&fixture);
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK &&
              rtl8168_chip_render_identity(&chip, text, sizeof(text)) != 0U &&
              strcmp(text, " xid=0x300 family=RTL8168-CLASSIC") == 0,
          "a classic part is published as the classic family");

    /*
     * A part outside the table still states the silicon it is: the XID the
     * driver read and the family it could not map.  TxConfig bits 20..31 are
     * masked with 0xFCF (the r8169 mac_info key), so 0x7FF is published as
     * 0x7CF - the value the table is actually matched against.
     */
    fixture_init(&fixture, UINT32_C(0x7FF00000));
    io = fixture_io(&fixture);
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_UNSUPPORTED_DEVICE &&
              rtl8168_chip_render_identity(&chip, text, sizeof(text)) != 0U &&
              strcmp(text, " xid=0x7CF family=UNKNOWN") == 0,
          "an XID outside the table is published as UNKNOWN, never guessed");

    /* A short buffer publishes nothing rather than a truncated identity. */
    fixture_init(&fixture, TEST_TX_CONFIG_8168H);
    io = fixture_io(&fixture);
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK &&
              rtl8168_chip_render_identity(&chip, tiny, sizeof(tiny)) == 0U,
          "an identity that does not fit is refused, not truncated");
    CHECK(rtl8168_chip_render_identity(NULL, text, sizeof(text)) == 0U &&
              rtl8168_chip_render_identity(&chip, NULL, sizeof(text)) == 0U &&
              rtl8168_chip_render_identity(&chip, text, 0U) == 0U,
          "a missing chip or buffer publishes no identity");
    CHECK(strcmp(rtl8168_chip_family_string(RTL8168_CHIP_EXTENDED),
                 "RTL8168-EXTENDED") == 0 &&
              strcmp(rtl8168_chip_family_string(RTL8168_CHIP_CLASSIC),
                     "RTL8168-CLASSIC") == 0 &&
              strcmp(rtl8168_chip_family_string(RTL8168_CHIP_UNKNOWN),
                     "UNKNOWN") == 0,
          "the family field names the three values the record documents");
}

static void test_mac_read(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;
    uint8_t mac[RTL8168_MAC_LENGTH];

    fixture_init(&fixture, TEST_TX_CONFIG_8168H);
    fixture_set_eri_mac(&fixture, test_mac);
    io = fixture_io(&fixture);
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK,
          "extended chip identity for the MAC read");
    memset(mac, 0, sizeof(mac));
    CHECK(rtl8168_mac_read(&io, &chip, mac) == RTL8168_OK,
          "the extended MAC read succeeds");
    CHECK(memcmp(mac, test_mac, RTL8168_MAC_LENGTH) == 0,
          "the extended MAC matches the registers");

    /* Invalid extended MAC falls back to the IDR registers. */
    fixture_init(&fixture, TEST_TX_CONFIG_8168H);
    fixture.eri_mac_low = 0U;
    fixture.eri_mac_high = 0U;
    fixture_set_mac(&fixture, test_mac);
    io = fixture_io(&fixture);
    memset(mac, 0, sizeof(mac));
    CHECK(rtl8168_mac_read(&io, &chip, mac) == RTL8168_OK &&
              memcmp(mac, test_mac, RTL8168_MAC_LENGTH) == 0,
          "an empty extended MAC falls back to the IDR registers");

    /* Classic family reads the IDR registers directly. */
    fixture_init(&fixture, (uint32_t)TEST_XID_8168B << 20U);
    fixture_set_mac(&fixture, test_mac);
    io = fixture_io(&fixture);
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK,
          "classic chip identity for the MAC read");
    memset(mac, 0, sizeof(mac));
    CHECK(rtl8168_mac_read(&io, &chip, mac) == RTL8168_OK &&
              memcmp(mac, test_mac, RTL8168_MAC_LENGTH) == 0,
          "the classic MAC read matches the registers");

    /* A device with no station address is reported, not invented. */
    fixture_init(&fixture, TEST_TX_CONFIG_8168H);
    io = fixture_io(&fixture);
    CHECK(rtl8168_mac_read(&io, &chip, mac) == RTL8168_INVALID_MAC,
          "an unreadable station address is an honest failure");
}

static void test_mac_validation(void)
{
    static const uint8_t zero[RTL8168_MAC_LENGTH] = { 0, 0, 0, 0, 0, 0 };
    static const uint8_t multicast[RTL8168_MAC_LENGTH] = {
        0x01U, 0x00U, 0x5EU, 0x00U, 0x00U, 0x01U
    };
    static const uint8_t broadcast[RTL8168_MAC_LENGTH] = {
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU
    };

    CHECK(rtl8168_mac_valid(test_mac),
          "a unicast station address is valid");
    CHECK(!rtl8168_mac_valid(zero),
          "an all-zero station address is invalid");
    CHECK(!rtl8168_mac_valid(multicast),
          "a multicast station address is invalid");
    CHECK(!rtl8168_mac_valid(broadcast),
          "the broadcast address is not a station address");
    CHECK(!rtl8168_mac_valid(NULL),
          "a missing station address is invalid");
}

static void test_reset(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;

    fixture_init(&fixture, TEST_TX_CONFIG_8168H);
    io = fixture_io(&fixture);
    CHECK(rtl8168_reset(&io) == RTL8168_OK,
          "the device reset completes");
    CHECK(fixture.reset_writes == 1U,
          "the reset writes the command register once");
    CHECK((fixture.registers[RTL8168_REG_CHIP_CMD] &
           RTL8168_CHIP_CMD_RESET) == 0U,
          "the reset bit is clear after the reset");

    fixture_init(&fixture, TEST_TX_CONFIG_8168H);
    fixture.reset_never_clears = true;
    io = fixture_io(&fixture);
    CHECK(rtl8168_reset(&io) == RTL8168_RESET_TIMEOUT,
          "a reset that never completes times out instead of hanging");

    fixture_init(&fixture, TEST_TX_CONFIG_8168H);
    fixture.fail_write = true;
    io = fixture_io(&fixture);
    CHECK(rtl8168_reset(&io) == RTL8168_IO_ERROR,
          "a failed reset write is an I/O error");
}

static void test_interrupts_masked(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;

    fixture_init(&fixture, TEST_TX_CONFIG_8168H);
    io = fixture_io(&fixture);
    CHECK(rtl8168_interrupts_mask(&io) == RTL8168_OK,
          "the interrupt mask is programmable");
    CHECK(fixture.intr_mask == RTL8168_INTR_MASK_NONE,
          "a poll-driven driver masks every interrupt");
    CHECK(fixture.intr_status_writes == 1U &&
              fixture.intr_status_ack == RTL8168_INTR_STATUS_CLEAR &&
              fixture.intr_status == 0U,
          "pending events are acknowledged with one write-one-to-clear");
}

static void test_bring_up(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;
    struct rtl8168_bring_up configuration;

    fixture_init(&fixture, TEST_TX_CONFIG_8168H);
    io = fixture_io(&fixture);
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK,
          "chip identity before bring-up");

    memset(&configuration, 0, sizeof(configuration));
    configuration.rx_descriptor_physical = TEST_DMA_BASE;
    configuration.tx_descriptor_physical = TEST_DMA_BASE + 256U;
    configuration.rx_buffer_capacity = TEST_BUFFER_CAPACITY;

    CHECK(rtl8168_bring_up(&io, &chip, &configuration, test_mac) ==
              RTL8168_OK,
          "the bring-up sequence completes");
    CHECK((fixture.registers[RTL8168_REG_CHIP_CMD] &
           (RTL8168_CHIP_CMD_RX_ENABLE |
            RTL8168_CHIP_CMD_TX_ENABLE)) ==
              (RTL8168_CHIP_CMD_RX_ENABLE |
               RTL8168_CHIP_CMD_TX_ENABLE),
          "the receiver and transmitter are enabled");
    CHECK(fixture.cfg9346_value == RTL8168_CFG9346_LOCK,
          "the config registers are locked again after bring-up");
    CHECK(fixture.cfg9346_writes == 2U,
          "the config registers are unlocked for the setup and locked after");
    CHECK(fixture.rx_config_value ==
              (RTL8168_RX128_INT_EN | RTL8168_RX_MULTI_EN |
               RTL8168_RX_DMA_BURST | RTL8168_RX_EARLY_OFF |
               RTL8168_RX_ACCEPT_BROADCAST |
               RTL8168_RX_ACCEPT_MY_PHYSICAL),
          "the receive configuration accepts broadcast and this station only");
    CHECK((fixture.rx_config_value & RTL8168_RX_ACCEPT_MULTICAST) == 0U &&
              (fixture.rx_config_value &
               RTL8168_RX_ACCEPT_ALL_PHYSICAL) == 0U,
          "the receive filter does not accept promiscuous or hashed multicast");
    CHECK((fixture.rx_config_value & RTL8168_RX_ACCEPT_ERROR_MASK) == 0U,
          "error frames and runts are not accepted");
    CHECK(fixture.tx_config_value ==
              (RTL8168_TX_DMA_BURST | RTL8168_TX_INTER_FRAME_GAP |
               RTL8168_TX_CONFIG_AUTO_FIFO),
          "the transmit configuration carries the extended FIFO bit");
    CHECK(fixture.mar_writes == 2U &&
              fixture.mar_values[0] == 0U && fixture.mar_values[1] == 0U,
          "the multicast hash filter is empty");
    CHECK(fixture.rx_max_size == TEST_BUFFER_CAPACITY,
          "the receive maximum size covers the frame buffers");
    CHECK(fixture.mac_low ==
              ((uint32_t)test_mac[0] | ((uint32_t)test_mac[1] << 8U) |
               ((uint32_t)test_mac[2] << 16U) |
               ((uint32_t)test_mac[3] << 24U)) &&
              fixture.mac_high ==
                  ((uint32_t)test_mac[4] |
                   ((uint32_t)test_mac[5] << 8U)),
          "the station address is programmed into the IDR registers");
    CHECK(load32(fixture.registers, RTL8168_REG_RX_DESC_LOW) ==
              (uint32_t)TEST_DMA_BASE &&
              load32(fixture.registers, RTL8168_REG_TX_DESC_LOW) ==
                  (uint32_t)(TEST_DMA_BASE + 256U),
          "the descriptor start registers carry the ring addresses");

    /* Rejections. */
    memset(&configuration, 0, sizeof(configuration));
    configuration.rx_descriptor_physical = TEST_DMA_BASE;
    configuration.tx_descriptor_physical = TEST_DMA_BASE + 256U;
    configuration.rx_buffer_capacity = TEST_BUFFER_CAPACITY;
    CHECK(rtl8168_bring_up(&io, &chip, &configuration, NULL) ==
              RTL8168_NULL,
          "a missing station address is refused");

    {
        static const uint8_t zero_mac[RTL8168_MAC_LENGTH] = {
            0, 0, 0, 0, 0, 0
        };

        CHECK(rtl8168_bring_up(&io, &chip, &configuration, zero_mac) ==
                  RTL8168_INVALID_MAC,
              "an invalid station address is refused");
    }

    configuration.rx_buffer_capacity = RTL8168_DESC_LENGTH_MASK + 1U;
    CHECK(rtl8168_bring_up(&io, &chip, &configuration, test_mac) ==
              RTL8168_INVALID_LENGTH,
          "a buffer capacity the descriptor cannot carry is refused");

    configuration.rx_buffer_capacity = TEST_BUFFER_CAPACITY;
    configuration.rx_descriptor_physical =
        RTL8168_DMA_ADDRESS_LIMIT + UINT64_C(0x1000);
    CHECK(rtl8168_bring_up(&io, &chip, &configuration, test_mac) ==
              RTL8168_UNSUPPORTED_DMA_ADDRESS,
          "a descriptor ring above the 32-bit DMA window is refused");

    configuration.rx_descriptor_physical = TEST_DMA_BASE + 8U;
    CHECK(rtl8168_bring_up(&io, &chip, &configuration, test_mac) ==
              RTL8168_BAD_STATE,
          "a descriptor ring that is not 256-byte aligned is refused");

    /*
     * A mapped MMIO write cannot fail visibly, so a part that does not
     * confirm the receiver/transmitter enable must not be published ready.
     */
    fixture_init(&fixture, TEST_TX_CONFIG_8168H);
    fixture.enable_never_sticks = true;
    io = fixture_io(&fixture);
    configuration.rx_descriptor_physical = TEST_DMA_BASE;
    configuration.rx_buffer_capacity = TEST_BUFFER_CAPACITY;
    CHECK(rtl8168_bring_up(&io, &chip, &configuration, test_mac) ==
              RTL8168_ENABLE_FAILED,
          "an enable the device does not confirm is refused");
}

static void test_link_status(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    bool link_up = false;

    fixture_init(&fixture, TEST_TX_CONFIG_8168H);
    io = fixture_io(&fixture);
    CHECK(rtl8168_link_status(&io, &link_up) == RTL8168_OK && link_up,
          "the link status register reports an up link");

    fixture.registers[RTL8168_REG_PHY_STATUS] = 0U;
    CHECK(rtl8168_link_status(&io, &link_up) == RTL8168_OK && !link_up,
          "the link status register reports a down link");

    fixture.registers[RTL8168_REG_PHY_STATUS] =
        (uint8_t)(RTL8168_PHY_STATUS_LINK | RTL8168_PHY_STATUS_FULL_DUPLEX);
    CHECK(rtl8168_link_status(&io, &link_up) == RTL8168_OK && link_up,
          "the duplex bit does not disturb the link bit");

    fixture.fail_read = true;
    CHECK(rtl8168_link_status(&io, &link_up) == RTL8168_IO_ERROR &&
              !link_up,
          "a failed link read reports no link");
}

/*
 * ITEM-435: the receiver's arming state.
 *
 * `rtl8168_bring_up()` verifies the enable bits by reading them back but never
 * verified that the part accepted the receive descriptor base.  A part that
 * answers every other register while holding a ring of its own receives into
 * memory the driver does not own and leaves the driver polling descriptors the
 * device never returns; the 2026-09-17 log's `rx_ok=0 rx_err=0` with a live
 * transmit ring is that shape.  The readback has to answer true only for the
 * ring the driver programmed and only while the receiver is enabled.
 */
/*
 * ITEM-583: the owner's rev 07 part reported a down link while the cable was
 * live.  The driver had only ever read PHYstatus; it never took the PHY out of
 * a firmware power-down/isolate state or restarted auto-negotiation.  The
 * fixture is the clause-22 register file behind PHYAR, so the proof is the
 * exact handshake and the exact advertisement, not a claim about QEMU.
 */
static void test_phy_mii(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_phy_state state;
    uint16_t value = 0U;
    uint16_t bmcr;

    fixture_init(&fixture, TEST_TX_CONFIG_8168E);
    io = fixture_io(&fixture);
    CHECK(rtl8168_phy_read(&io, RTL8168_PHY_REG_PHYID1, &value) ==
              RTL8168_OK && value == 0x001CU,
          "the PHY id high word is read through PHYAR");
    CHECK(rtl8168_phy_read(&io, RTL8168_PHY_REG_PHYID2, &value) ==
              RTL8168_OK && value == 0xC912U,
          "the PHY id low word is read through PHYAR");
    CHECK(rtl8168_phy_write(&io, RTL8168_PHY_REG_BMCR, 0x1140U) ==
              RTL8168_OK &&
              fixture.phy_registers[RTL8168_PHY_REG_BMCR] == 0x1140U &&
              fixture.phy_last_write_register == RTL8168_PHY_REG_BMCR,
          "a PHY write reaches the addressed register with its data");

    /* A firmware power-down/isolate/loopback state is cleared and ANEG starts. */
    fixture.phy_registers[RTL8168_PHY_REG_BMCR] =
        RTL8168_PHY_BMCR_POWER_DOWN | RTL8168_PHY_BMCR_ISOLATE |
        RTL8168_PHY_BMCR_LOOPBACK;
    fixture_set_phy_link(&fixture, false);
    memset(&state, 0, sizeof(state));
    CHECK(rtl8168_phy_configure(&io, &state) == RTL8168_OK,
          "the PHY is configured instead of only being read");
    bmcr = state.bmcr;
    CHECK((bmcr & RTL8168_PHY_BMCR_POWER_DOWN) == 0U &&
              (bmcr & RTL8168_PHY_BMCR_ISOLATE) == 0U &&
              (bmcr & RTL8168_PHY_BMCR_LOOPBACK) == 0U,
          "power-down, isolate and loopback are cleared");
    CHECK((bmcr & RTL8168_PHY_BMCR_ANEG_ENABLE) != 0U &&
              (bmcr & RTL8168_PHY_BMCR_RESTART_ANEG) != 0U,
          "auto-negotiation is enabled and restarted");
    CHECK(fixture.phy_registers[RTL8168_PHY_REG_ADVERTISE] ==
              RTL8168_PHY_ADVERTISE_DEFAULT,
          "the standard 10/100/1000 advertisement is programmed");
    CHECK(state.valid && !state.link_up && !state.autoneg_complete &&
              state.phy_id1 == 0x001CU && state.phy_id2 == 0xC912U &&
              state.configures == 1U,
          "the configured state names the PHY and its link");

    /* A link that comes up after boot is visible on the next refresh. */
    fixture_set_phy_link(&fixture, true);
    CHECK(rtl8168_phy_refresh(&io, &state) == RTL8168_OK &&
              state.link_up && state.autoneg_complete &&
              state.refreshes == 1U,
          "a late link is read by the refresh, not cached at boot");
    fixture_set_phy_link(&fixture, false);
    CHECK(rtl8168_phy_refresh(&io, &state) == RTL8168_OK && !state.link_up,
          "a dropped link is published by the same refresh");

    CHECK(rtl8168_phy_read(&io, 0x20U, &value) == RTL8168_BAD_STATE,
          "a register outside the five-bit selector is refused");
    CHECK(rtl8168_phy_write(&io, 0x20U, 0U) == RTL8168_BAD_STATE,
          "a write outside the five-bit selector is refused");
    CHECK(rtl8168_phy_read(NULL, RTL8168_PHY_REG_BMCR, &value) ==
              RTL8168_NULL &&
              rtl8168_phy_write(NULL, RTL8168_PHY_REG_BMCR, 0U) ==
                  RTL8168_NULL,
          "a missing MMIO seam is refused");

    fixture.phy_never_completes = true;
    CHECK(rtl8168_phy_read(&io, RTL8168_PHY_REG_BMCR, &value) ==
              RTL8168_IO_ERROR,
          "a PHY that never completes a read is bounded, not spun on");

    {
        struct rtl8168_chip part;

        memset(&part, 0, sizeof(part));
        part.xid = TEST_XID_8168E;
        CHECK(rtl8168_phy_mii_supported(&part),
              "the 8168E PHY is reached through PHYAR");
        part.xid = TEST_XID_8168H;
        CHECK(!rtl8168_phy_mii_supported(&part),
              "the 8168H OCP PHY is not claimed through PHYAR");
        CHECK(!rtl8168_phy_mii_supported(NULL),
              "a missing chip does not claim an MII path");
    }
}

/*
 * ITEM-589: the owner's Toshiba L875 reports XID 0x2C9.
 *
 * The XID is (TxConfig >> 20) & 0xFCF, so a raw TxConfig of 0x2C900000 decodes
 * to 0x2C9 - one bit above the 0x2C8 the driver's MII list named - while the
 * production family table's mask row { 0x7C8, 0x2C8 } matches it.  The chip
 * determination therefore succeeds and the part is the extended family, but
 * the old exact-XID PHY list (0x2C0/0x2C1/0x2C8) skipped PHY configuration:
 * the driver stayed on the single PHYstatus byte, published link=DOWN with a
 * live cable and never auto-negotiated.  This proof drives the raw TxConfig
 * through the production identify and then through the production PHY
 * configuration, so the skip cannot come back.
 */
static void test_phy_mii_xid_2c9(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;
    struct rtl8168_phy_state state;

    /* The raw TxConfig the hardware read: bits 20..31 carry 0x2C9. */
    fixture_init(&fixture, UINT32_C(0x2C900000));
    io = fixture_io(&fixture);
    memset(&chip, 0, sizeof(chip));
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK,
          "the production identify accepts the raw TxConfig of the 0x2C9 part");
    CHECK(chip.xid == 0x2C9U,
          "the raw TxConfig decodes to XID 0x2C9");
    CHECK(chip.family == RTL8168_CHIP_EXTENDED,
          "0x2C9 is the extended family the same mask row names");
    CHECK(rtl8168_phy_mii_supported(&chip),
          "the 0x2C9 PHY is reached through PHYAR, not skipped");
    CHECK(rtl8168_phy_access_method(&chip) ==
              RTL8168_PHY_ACCESS_MII_PHYAR,
          "the part's PHY access is the clause-22 MII window");

    memset(&state, 0, sizeof(state));
    CHECK(rtl8168_phy_configure(&io, &state) == RTL8168_OK,
          "the 0x2C9 PHY is configured, not left as the firmware left it");
    CHECK(state.valid && state.configures == 1U &&
              state.config_status == RTL8168_OK &&
              state.first_error == RTL8168_OK,
          "the configuration result is kept on the state");
    CHECK((state.bmcr & RTL8168_PHY_BMCR_POWER_DOWN) == 0U &&
              (state.bmcr & RTL8168_PHY_BMCR_ISOLATE) == 0U &&
              (state.bmcr & RTL8168_PHY_BMCR_ANEG_ENABLE) != 0U &&
              state.advertise == RTL8168_PHY_ADVERTISE_DEFAULT,
          "power-down and isolation are cleared and negotiation is armed");

    /* A PHY that refuses the window keeps the first error and still names it. */
    fixture.phy_never_completes = true;
    memset(&state, 0, sizeof(state));
    CHECK(rtl8168_phy_configure(&io, &state) == RTL8168_IO_ERROR &&
              state.config_status == RTL8168_IO_ERROR &&
              state.first_error == RTL8168_IO_ERROR,
          "a refused PHY configuration keeps its first error");

    {
        struct rtl8168_chip late;

        memset(&late, 0, sizeof(late));
        late.xid = TEST_XID_8168H;
        CHECK(!rtl8168_phy_mii_supported(&late) &&
                  rtl8168_phy_access_method(&late) ==
                      RTL8168_PHY_ACCESS_OCP,
              "the later OCP parts keep their own access method");
    }
}

/*
 * ITEM-591: the PHYAR handshake's two gaps.
 *
 * The Toshiba run of 49ac1ffc configured a PHY that answered
 * (`phyid=0x001C:0xC915`) and still published `link=DOWN`, `aneg=0`,
 * `lpa=0x0000`.  The driver issued its PHYAR commands back-to-back; the
 * reference driver waits 20 us after every completion indication before the
 * next command, because the hardware spec requires it.  The model owns a
 * microsecond clock and counts a command that arrives inside that window, so
 * the four command orders - read/read, read/write, write/read, write/write -
 * are checked as the part would see them.
 */
static void test_phyar_settle_gap(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    uint16_t value = 0U;

    fixture_init(&fixture, TEST_TX_CONFIG_8168E);
    io = fixture_io(&fixture);

    CHECK(rtl8168_phy_read(&io, RTL8168_PHY_REG_PHYID1, &value) ==
              RTL8168_OK && value == 0x001CU,
          "read 1 answers through the timed PHYAR window");
    CHECK(rtl8168_phy_read(&io, RTL8168_PHY_REG_PHYID2, &value) ==
              RTL8168_OK && value == 0xC912U,
          "read -> read: the second read is not issued early");
    CHECK(rtl8168_phy_write(&io, RTL8168_PHY_REG_BMCR, 0x1140U) ==
              RTL8168_OK &&
              fixture.phy_registers[RTL8168_PHY_REG_BMCR] == 0x1140U,
          "read -> write: the write lands in the addressed register");
    CHECK(rtl8168_phy_read(&io, RTL8168_PHY_REG_BMSR, &value) ==
              RTL8168_OK,
          "write -> read: the read after a write is not issued early");
    CHECK(rtl8168_phy_write(&io, RTL8168_PHY_REG_LPA, 0x01E1U) ==
              RTL8168_OK,
          "write -> write: the second write is not issued early");
    CHECK(fixture.phy_early_commands == 0U,
          "no PHYAR command is issued inside the 20 us settle window");
    CHECK(fixture.phy_settle_delays == 5U,
          "every completed operation waits the settle pause before the next");
    CHECK(fixture.phy_delay_us_total >=
              5U * RTL8168_PHY_ACCESS_SETTLE_US,
          "the pauses are real microseconds, not a loop count");
    CHECK(fixture.phy_poll_gap_min_us >= RTL8168_PHY_ACCESS_POLL_US,
          "the busy bit is polled at the reference driver's interval");
}

/*
 * The bounded wait: a part that answers late is waited for, and a part that
 * never answers is given up inside the reference driver's budget instead of
 * being polled in a tight loop.  A failed wait still leaves the settle gap, so
 * the command after it is not issued early either.
 */
static void test_phyar_bounded_wait(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    uint16_t value = 0U;
    uint64_t started;
    uint64_t elapsed;
    uint32_t settles;

    /* A slow part: the completion arrives after several poll intervals. */
    fixture_init(&fixture, TEST_TX_CONFIG_8168E);
    fixture.phy_complete_us = 60U;
    io = fixture_io(&fixture);
    CHECK(rtl8168_phy_read(&io, RTL8168_PHY_REG_PHYID1, &value) ==
              RTL8168_OK && value == 0x001CU,
          "a part that completes late is waited for, not refused");
    CHECK(fixture.phy_polls >= 3U,
          "the slow part was polled more than once");
    CHECK(fixture.phy_early_commands == 0U,
          "the slow completion still leaves the settle gap");

    /* A part that never answers: 20 polls of 25 us, then a bounded refusal. */
    fixture_init(&fixture, TEST_TX_CONFIG_8168E);
    fixture.phy_never_completes = true;
    io = fixture_io(&fixture);
    started = fixture.now_us;
    CHECK(rtl8168_phy_read(&io, RTL8168_PHY_REG_PHYID1, &value) ==
              RTL8168_IO_ERROR,
          "a PHY that never completes a read is refused, not spun on");
    elapsed = fixture.now_us - started;
    CHECK(elapsed >= RTL8168_PHY_ACCESS_TIMEOUT_US,
          "the wait spends the reference driver's 500 us budget");
    CHECK(elapsed < RTL8168_PHY_ACCESS_TIMEOUT_US + 200U,
          "and is bounded by it rather than waiting forever");
    CHECK(fixture.phy_polls == RTL8168_PHY_ACCESS_POLLS,
          "the budget is the reference driver's 20 polls");
    CHECK(fixture.phy_poll_gap_min_us >= RTL8168_PHY_ACCESS_POLL_US,
          "the polls are spaced by the 25 us interval, not tight");
    settles = fixture.phy_settle_delays;
    CHECK(settles == 1U,
          "a failed wait still leaves the settle gap after its indication");

    /* The same bound for a write that never clears its busy bit. */
    fixture_init(&fixture, TEST_TX_CONFIG_8168E);
    fixture.phy_never_completes = true;
    io = fixture_io(&fixture);
    CHECK(rtl8168_phy_write(&io, RTL8168_PHY_REG_BMCR, 0x1140U) ==
              RTL8168_IO_ERROR,
          "a write that never completes is bounded the same way");

    /* A bus with no microsecond pause cannot honour the handshake at all. */
    fixture_init(&fixture, TEST_TX_CONFIG_8168E);
    io = fixture_io(&fixture);
    io.delay_us = NULL;
    CHECK(rtl8168_phy_read(&io, RTL8168_PHY_REG_PHYID1, &value) ==
              RTL8168_IO_ERROR &&
              fixture.phy_reads == 0U,
          "a bus that cannot wait refuses the PHY before issuing a command");
}

/*
 * ITEM-591: the advertisement is what the part answered, not the constant.
 *
 * The hardware record printed `adv=0x03E1` from the constant the driver wrote,
 * so a PHY that masked the gigabit bits (the reason a 10/100-only link partner
 * never resolves) looked identical to one that accepted them.  The model's
 * register 4 answers with the gigabit bits masked off, which the old
 * implementation could not tell from the constant.
 */
static void test_phy_advertise_readback(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_phy_state state;

    fixture_init(&fixture, TEST_TX_CONFIG_8168E);
    /* A PHY that does not implement 10BASE-T full duplex answers with that
       bit cleared, which is a value the constant the driver wrote cannot
       explain - exactly the distinction the read-back exists for. */
    fixture.phy_advertise_write_mask = 0x01BFU;
    io = fixture_io(&fixture);
    memset(&state, 0, sizeof(state));
    CHECK(rtl8168_phy_configure(&io, &state) == RTL8168_OK,
          "the part is configured");
    CHECK(state.advertise_written == RTL8168_PHY_ADVERTISE_DEFAULT,
          "the driver programmed the standard advertisement");
    CHECK(state.advertise ==
              (uint16_t)(RTL8168_PHY_ADVERTISE_DEFAULT & 0x01BFU),
          "the advertised value is what the part answered");
    CHECK(state.advertise != RTL8168_PHY_ADVERTISE_DEFAULT,
          "the read-back is distinguishable from the constant");
    CHECK(state.advertise_valid,
          "the read-back is marked as verified");
    CHECK(state.config_status == RTL8168_OK && state.valid,
          "the configuration's own verdict is CONFIGURED");
    CHECK(fixture.phy_registers[RTL8168_PHY_REG_ADVERTISE] ==
              state.advertise,
          "the model and the recorded value agree");

    /* A later refresh keeps the live value and the value that was written. */
    fixture.phy_registers[RTL8168_PHY_REG_ADVERTISE] = 0x0061U;
    CHECK(rtl8168_phy_refresh(&io, &state) == RTL8168_OK &&
              state.advertise == 0x0061U && state.advertise_valid &&
              state.advertise_written == RTL8168_PHY_ADVERTISE_DEFAULT,
          "the refresh reads the register again and keeps the programmed value");

    /* A read-back that does not answer is named as a read that did not
       answer: the first error is kept, the configuration is still reported,
       and the link/auto-negotiation answers are untouched. */
    fixture_init(&fixture, TEST_TX_CONFIG_8168E);
    fixture.phy_fail_register = (int16_t)RTL8168_PHY_REG_ADVERTISE;
    io = fixture_io(&fixture);
    memset(&state, 0, sizeof(state));
    CHECK(rtl8168_phy_configure(&io, &state) == RTL8168_OK,
          "a PHY that stops answering only register 4 still configures");
    CHECK(!state.advertise_valid,
          "the read-back is marked as unverified");
    CHECK(state.first_error == RTL8168_IO_ERROR,
          "the first PHY error is kept");
    CHECK(state.config_status == RTL8168_OK && state.valid,
          "CONFIGURED stays the configuration's own result");
    CHECK(state.advertise == 0U,
          "an unverified advertisement is not reported as a value");
}

/*
 * ITEM-592: the cold start of the 8168evl (XID 0x2C8/0x2C9).
 *
 * The Toshiba's PHY answered everything and never saw a link partner.  The
 * reference driver's start for this generation (RTL_GIGA_MAC_VER_34) runs
 * EPHY and clock stages before the MAC is enabled and does not touch the
 * receive data-valid gate at all; the driver under test ran only the MAC and
 * cleared a G-generation bit on the way.  The model knows the documented order
 * and the documented register set, so a start that skips a stage or writes a
 * generation-foreign register is a failure here, not a hardware run later.
 */
static void fixture_evl_configuration(struct rtl8168_bring_up *configuration)
{
    memset(configuration, 0, sizeof(*configuration));
    configuration->rx_descriptor_physical = TEST_DMA_BASE;
    configuration->tx_descriptor_physical = TEST_DMA_BASE + 256U;
    configuration->rx_buffer_capacity = TEST_BUFFER_CAPACITY;
}

/* Literal semantic writes from Linux v6.12 rtl8168e_2_hw_phy_config and
 * its helpers (audit doc records exact source hashes). Page selectors are
 * observed by separate register storage; these are the resulting writes.
 * Removing delay cap, changing corner register 17 to 06, reordering EEE or
 * omitting the initiated PHY reset must fail this production-path proof. */
static const struct phy_model_write evl_expected[] = {
    {0, 0, 0x00, 0x1140}, /* resume before any vendor programming */
    {7, 0x00ac, 0x18, 0x0006},
    {3, 0, 0x09, 0xa20f},
    {5, 0x8b5b, 0x06, 0x9222},
    {5, 0x8b6d, 0x06, 0x8000},
    {5, 0x8b76, 0x06, 0x8000},
    {5, 0x8b80, 0x17, 0x0086},
    {7, 0x002d, 0x18, 0x0130},
    {0, 0, 0x14, 0x8000},
    {5, 0x8b86, 0x06, 0x0201},
    {5, 0x8b85, 0x06, 0x4012},
    {7, 0x0020, 0x15, 0x0520},
    {5, 0x8b85, 0x06, 0x6012},
    {3, 0, 0x19, 0x0201},
    {3, 0, 0x10, 0x0500},
    {5, 0x8b85, 0x01, 0x0102},
    {0, 0, 0x00, 0x9340}, /* initiated soft reset, distinct from ChipCmd */
    {0, 0, 0x04, 0x01e1},
    {0, 0, 0x09, 0x0300},
    {0, 0, 0x00, 0x1340}, /* command, not observed completion */
};

/*
 * ITEM-595: Linux r8169_mdio_read checks the completion flag and then reads
 * PHYAR again for the payload.  The Toshiba failure retained busy-selector
 * 0x801f0000 from the completion poll.  This model makes that sample stale and
 * exposes the register value only to the separately requested payload read.
 */
static void test_phyar_separate_completion_and_payload(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;
    struct rtl8168_bring_up configuration;
    struct rtl8168_phy_state phy;
    struct rtl8168_cold_boot_report report;

    fixture_init(&fixture, UINT32_C(0x2c900000));
    fixture_set_phy_link(&fixture, false);
    fixture.phy_page = 7U; /* inherited vendor page */
    fixture.phy_completion_sample_data = 0U;
    io = fixture_io(&fixture);
    fixture_evl_configuration(&configuration);
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK,
          "payload-read proof identifies the production XID 2c9");
    CHECK(rtl8168_cold_boot(&io, &chip, &configuration, test_mac, &phy,
                            &report) == RTL8168_OK,
          "cold start reads PHY data after, not from, the completion sample");
    CHECK(fixture.phy_reads != 0U &&
              fixture.phy_completion_samples == fixture.phy_reads &&
              fixture.phy_payload_reads == fixture.phy_reads,
          "every completed PHY read has one completion sample and one payload read");
    CHECK(fixture.phy_model.write_count ==
              sizeof(evl_expected) / sizeof(evl_expected[0]) &&
              fixture.phy_page == 0U,
          "separate payload reads preserve the entire EVL recipe");
}

static void test_phyar_payload_read_failure_keeps_poll_evidence(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;
    struct rtl8168_bring_up configuration;
    struct rtl8168_phy_state phy;
    struct rtl8168_cold_boot_report report;

    fixture_init(&fixture, UINT32_C(0x2c900000));
    fixture_set_phy_link(&fixture, false);
    fixture.phy_page = 7U;
    fixture.phy_fail_payload_read = true;
    fixture.phy_fail_payload_page = 7U;
    fixture.phy_fail_payload_register = RTL8168_PHY_REG_PAGE;
    io = fixture_io(&fixture);
    fixture_evl_configuration(&configuration);
    (void)rtl8168_chip_identify(&io, &chip);
    CHECK(rtl8168_cold_boot(&io, &chip, &configuration, test_mac, &phy,
                            &report) == RTL8168_IO_ERROR,
          "failed separate payload read refuses the production cold start");
    CHECK(fixture.phy_payload_read_failures == 1U &&
              phy.failure.valid && phy.failure.step == 1U &&
              phy.failure.phase == RTL8168_PHY_PHASE_PAGE &&
              phy.failure.transaction.reason ==
                  RTL8168_PHY_FAILURE_MMIO_READ &&
              phy.failure.transaction.reg == RTL8168_PHY_REG_PAGE &&
              phy.failure.transaction.last_valid &&
              phy.failure.transaction.last ==
                  (RTL8168_PHY_ACCESS_BUSY |
                   ((uint32_t)RTL8168_PHY_REG_PAGE <<
                    RTL8168_PHY_ACCESS_REG_SHIFT)) &&
              !phy.failure.transaction.observed_valid &&
              phy.failure.restore_attempted && phy.failure.restore_ok &&
              phy.failure.restore_page_valid &&
              phy.failure.restore_page == RTL8168_PHY_PAGE_STANDARD,
          "payload MMIO failure keeps completion evidence and page0 recovery");
}

/* ITEM-597: selector equality is not a valid EVL initialization precondition.
 * Removing the EVL-only zero-readback path must stop production cold start
 * before delay-cap and fail the entire independently specified recipe. */
static void test_evl_phy_zero_page_readback(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;
    struct rtl8168_bring_up configuration;
    struct rtl8168_phy_state phy;
    struct rtl8168_cold_boot_report report;
    bool matches = true;

    fixture_init(&fixture, UINT32_C(0x2c900000));
    fixture_set_phy_link(&fixture, false);
    fixture.phy_model.page_readback_zero = true;
    fixture.phy_registers[RTL8168_PHY_REG_BMCR] = 0x1140U;
    io = fixture_io(&fixture);
    fixture_evl_configuration(&configuration);
    (void)rtl8168_chip_identify(&io, &chip);
    CHECK(rtl8168_cold_boot(&io, &chip, &configuration, test_mac, &phy,
                            &report) == RTL8168_OK,
          "EVL zero selector readback does not block production cold start");
    CHECK(fixture.phy_model.write_count ==
              sizeof(evl_expected) / sizeof(evl_expected[0]),
          "EVL zero readback reaches all vendor actions, reset and advertisements");
    for (size_t i = 0; i < sizeof(evl_expected) / sizeof(evl_expected[0]); ++i) {
        const struct phy_model_write *got = &fixture.phy_model.writes[i];
        const struct phy_model_write *want = &evl_expected[i];
        if (got->page != want->page || got->selector != want->selector ||
            got->reg != want->reg || got->value != want->value) matches = false;
    }
    CHECK(matches && fixture.phy_model.pages[3][9] == 0xa20fU &&
              fixture.phy_model.extended[0xac][0x18] == 6U &&
              fixture.phy_model.parameters[0x8b85] == 0x6012U,
          "zero readback keeps real page and indirect addressing in reference order");
    CHECK(fixture.phy_page == 0U && fixture.phy_model.reset_requests == 1U &&
              fixture.phy_early_commands == 0U &&
              !phy.link_up && !phy.autoneg_complete,
          "zero readback preserves cleanup, reset, settle and honest DOWN observation");
    CHECK(phy.page_selection.zero_reads == 14U &&
              phy.page_selection.requested == 7U &&
              phy.page_selection.write.command == UINT32_C(0x801f0007) &&
              phy.page_selection.read.observed_valid &&
              phy.page_selection.read.observed == 0U &&
              phy.page_selection.read.payload == UINT32_C(0x801f0000) &&
              !phy.failure.valid && phy.first_error == RTL8168_OK,
          "first zero readback remains actual evidence rather than a cached page");
    {
        struct rtl8168_phy_page_observation saved = phy.page_selection;
        CHECK(rtl8168_phy_refresh(&io, &phy) == RTL8168_OK &&
                  memcmp(&saved, &phy.page_selection, sizeof(saved)) == 0,
              "page0 refresh cannot overwrite the first non-echo observation");
    }
    CHECK(rtl8168_phy_select_page(&io, 3U) == RTL8168_IO_ERROR &&
              fixture.phy_page == 3U && io.phy_last.observed == 0U &&
              io.phy_last.reason == RTL8168_PHY_FAILURE_PAGE_MISMATCH,
          "generic page selection retains strict zero-mismatch refusal");
    CHECK(rtl8168_phy_select_page(&io, 0U) == RTL8168_OK,
          "generic page0 recovery retains its existing contract");
}

static void test_evl_phy_reference_sequence(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;
    struct rtl8168_bring_up configuration;
    struct rtl8168_phy_state phy;
    struct rtl8168_cold_boot_report report;
    bool matches = true;

    fixture_init(&fixture, UINT32_C(0x2c900000));
    fixture_set_phy_link(&fixture, false);
    fixture.phy_registers[0] = 0x1940U;
    fixture.phy_page = 7U; /* inherited vendor page */
    io = fixture_io(&fixture);
    fixture_evl_configuration(&configuration);
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK,
          "EVL PHY reference model identifies production XID 2c9");
    CHECK(rtl8168_cold_boot(&io, &chip, &configuration, test_mac, &phy,
                           &report) == RTL8168_OK,
          "EVL PHY reference model executes the production cold start");
    CHECK(fixture.phy_model.write_count ==
              sizeof(evl_expected) / sizeof(evl_expected[0]),
          "EVL PHY cold start includes all 15 vendor actions and PHY reset");
    for (size_t i = 0; i < sizeof(evl_expected) / sizeof(evl_expected[0]); ++i) {
        const struct phy_model_write *got = &fixture.phy_model.writes[i];
        const struct phy_model_write *want = &evl_expected[i];
        if (got->page != want->page || got->selector != want->selector ||
            got->reg != want->reg || got->value != want->value) {
            fprintf(stderr, "PHY write %zu: got %04x/%04x:%02x=%04x "
                    "want %04x/%04x:%02x=%04x\n", i, got->page,
                    got->selector, got->reg, got->value, want->page,
                    want->selector, want->reg, want->value);
            matches = false;
        }
    }
    CHECK(matches, "EVL PHY writes match the independently specified order");
    CHECK(fixture.phy_model.reset_requests == 1U &&
              fixture.phy_model.reset_finished != 0U,
          "EVL PHY initiates and waits for its own reset");
    CHECK(fixture.phy_registers[4] == 0x01e1U &&
              fixture.phy_registers[9] == 0x0300U,
          "EVL advertisements survive the completed PHY reset");
    CHECK(phy.bmcr_initial_valid && phy.bmcr_initial == 0x1940U,
          "EVL keeps BMCR before resume and soft reset");
    CHECK(fixture.phy_page == 0U && fixture.phy_model.bad_pages == 0U,
          "EVL vendor sequence restores the standard page");
    CHECK(!phy.link_up && !phy.autoneg_complete,
          "EVL successful register commands do not invent a physical link");
    CHECK(fixture.phy_early_commands == 0U,
          "EVL vendor operations preserve PHYAR completion pauses");
}

static void test_evl_phy_intermediate_failures(void)
{
    /* Fail every vendor write separately, including the second EEE write to
     * 8b85 (the first matching fault is sufficient to prove early abort). */
    for (unsigned zero = 0; zero < 2; ++zero) {
    for (unsigned reading = 0; reading < 2; ++reading) {
    for (size_t i = 1; i <= 15; ++i) {
        struct mmio_fixture fixture;
        struct rtl8168_mmio_io io;
        struct rtl8168_chip chip;
        struct rtl8168_bring_up configuration;
        struct rtl8168_phy_state phy;
        struct rtl8168_cold_boot_report report;
        fixture_init(&fixture, UINT32_C(0x2c900000));
        fixture.phy_model.page_readback_zero = zero != 0U;
        fixture.phy_model.fault_enabled = true;
        fixture.phy_model.fault_write = reading == 0U;
        fixture.phy_model.fault_after_writes = (unsigned)i;
        fixture.phy_model.fault_page = evl_expected[i].page;
        fixture.phy_model.fault_selector = evl_expected[i].selector;
        fixture.phy_model.fault_reg = evl_expected[i].reg;
        io = fixture_io(&fixture);
        fixture_evl_configuration(&configuration);
        (void)rtl8168_chip_identify(&io, &chip);
        CHECK(rtl8168_cold_boot(&io, &chip, &configuration, test_mac, &phy,
                               &report) == RTL8168_IO_ERROR,
              "EVL vendor write refusal stops cold start");
        CHECK(fixture.phy_model.fault_fired &&
                  fixture.phy_model.write_count == i && fixture.phy_page == 0U &&
                  fixture.phy_model.reset_requests == 0U &&
                  !fixture.mac_enabled,
              "EVL vendor refusal restores page and skips reset/MAC/AN");
        CHECK(report.failed && report.first_failure == RTL8168_IO_ERROR &&
                  strcmp(rtl8168_stage_string(report.failed_stage),
                         "phy_tune") == 0 &&
                  phy.first_error == RTL8168_IO_ERROR,
              "EVL vendor refusal names the first failed stage");
        CHECK(phy.failure.valid && phy.failure.step == i &&
                  phy.failure.phase == (reading == 0U ? RTL8168_PHY_PHASE_WRITE
                                                      : RTL8168_PHY_PHASE_READ) &&
                  phy.failure.page == evl_expected[i].page &&
                  phy.failure.transaction.reg == evl_expected[i].reg &&
                  phy.failure.transaction.write == (reading == 0U) &&
                  phy.failure.transaction.reason == RTL8168_PHY_FAILURE_MMIO_WRITE &&
                  phy.failure.transaction.polls == 0U &&
                  !phy.failure.transaction.last_valid &&
                  phy.failure.restore_attempted && phy.failure.restore_ok,
              "every vendor read/write refusal retains its exact first operation");
        CHECK(rtl8168_phy_refresh(&io, &phy) == RTL8168_OK &&
                  phy.first_error == RTL8168_IO_ERROR &&
                  phy.config_status == RTL8168_IO_ERROR,
              "EVL later successful refresh preserves configuration refusal");
    }
    }
    }
}

static void test_evl_phy_reset_timeout_and_page_failure(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;
    struct rtl8168_bring_up configuration;
    struct rtl8168_phy_state phy;
    struct rtl8168_cold_boot_report report;
    fixture_init(&fixture, UINT32_C(0x2c900000));
    fixture.phy_model.reset_stuck = true;
    io = fixture_io(&fixture);
    fixture_evl_configuration(&configuration);
    (void)rtl8168_chip_identify(&io, &chip);
    CHECK(rtl8168_cold_boot(&io, &chip, &configuration, test_mac, &phy,
                           &report) == RTL8168_IO_ERROR &&
              fixture.phy_model.reset_requests == 1U,
          "EVL stuck initiated PHY reset refuses cold start");
    CHECK(fixture.now_us >= 600000U && fixture.now_us < 700000U &&
              !fixture.mac_enabled && phy.advertise_written == 0U,
          "EVL stuck reset wait is timed and bounded before advertisement");
    CHECK(report.failed && strcmp(rtl8168_stage_string(report.failed_stage),
                                   "phy_reset") == 0,
          "EVL PHY reset timeout is distinct from MAC reset");
    CHECK(phy.failure.valid && phy.failure.step == 0U &&
              phy.failure.phase == RTL8168_PHY_PHASE_RESET_WAIT &&
              phy.failure.transaction.reason == RTL8168_PHY_FAILURE_RESET_TIMEOUT &&
              phy.failure.transaction.reg == 0U &&
              phy.failure.transaction.observed_valid &&
              (phy.failure.transaction.observed & 0x8000U) != 0U,
          "reset timeout retains completed BMCR data without naming PHYAR timeout");

    fixture_init(&fixture, UINT32_C(0x2c900000));
    fixture.phy_model.fault_enabled = true;
    fixture.phy_model.fault_write = false; /* intermediate RMW read */
    fixture.phy_model.fault_page = 7U;
    fixture.phy_model.fault_selector = 0xacU;
    fixture.phy_model.fault_reg = 0x18U;
    fixture.phy_model.fault_restore = true;
    io = fixture_io(&fixture);
    (void)rtl8168_chip_identify(&io, &chip);
    CHECK(rtl8168_cold_boot(&io, &chip, &configuration, test_mac, &phy,
                           &report) == RTL8168_IO_ERROR &&
              fixture.phy_model.fault_fired && !phy.page_valid,
          "EVL failed RMW and failed page restoration stay unverified");
    CHECK(report.failed && strcmp(rtl8168_stage_string(report.failed_stage),
                                   "phy_tune") == 0 &&
              phy.first_error == RTL8168_IO_ERROR,
          "EVL restoration failure does not replace the original error");
}

static void test_evl_phy_control_refusals(void)
{
    static const uint8_t steps[] = {0,0,1,1,1,3,1,0,0,0};
    static const enum rtl8168_phy_phase phases[] = {
        RTL8168_PHY_PHASE_READ, RTL8168_PHY_PHASE_WRITE,
        RTL8168_PHY_PHASE_PAGE, RTL8168_PHY_PHASE_PAGE,
        RTL8168_PHY_PHASE_SELECTOR, RTL8168_PHY_PHASE_SELECTOR,
        RTL8168_PHY_PHASE_RESTORE, RTL8168_PHY_PHASE_READ,
        RTL8168_PHY_PHASE_WRITE, RTL8168_PHY_PHASE_RESET_WAIT,
    };
    static const uint16_t requested_pages[] = {0,0,7,7,7,5,0,0,0,0};
    static const uint16_t requested_selectors[] = {
        0xffff,0xffff,0xac,0xac,0xac,0x8b5b,0xffff,0xffff,0xffff,0xffff
    };
    static const struct {
        uint16_t page, selector, reg;
        bool write;
        unsigned after;
        enum rtl8168_cold_boot_stage stage;
    } faults[] = {
        {0, 0, 0, false, 0, RTL8168_STAGE_PHY_RESUME},
        {0, 0, 0, true, 0, RTL8168_STAGE_PHY_RESUME},
        {0, 0, 0x1f, true, 1, RTL8168_STAGE_PHY_TUNE},
        {7, 0, 0x1f, false, 1, RTL8168_STAGE_PHY_TUNE},
        {7, 0, 0x1e, true, 1, RTL8168_STAGE_PHY_TUNE},
        {5, 0, 5, true, 3, RTL8168_STAGE_PHY_TUNE},
        {7, 0xac, 0x1f, true, 2, RTL8168_STAGE_PHY_TUNE},
        {0, 0, 0, false, 16, RTL8168_STAGE_PHY_RESET},
        {0, 0, 0, true, 16, RTL8168_STAGE_PHY_RESET},
        {0, 0, 0, false, 17, RTL8168_STAGE_PHY_RESET},
    };
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        struct mmio_fixture fixture;
        struct rtl8168_mmio_io io;
        struct rtl8168_chip chip;
        struct rtl8168_bring_up configuration;
        struct rtl8168_phy_state phy;
        struct rtl8168_cold_boot_report report;
        fixture_init(&fixture, UINT32_C(0x2c900000));
        fixture.phy_model.fault_enabled = true;
        fixture.phy_model.fault_write = faults[i].write;
        fixture.phy_model.fault_page = faults[i].page;
        fixture.phy_model.fault_selector = faults[i].selector;
        fixture.phy_model.fault_reg = faults[i].reg;
        fixture.phy_model.fault_after_writes = faults[i].after;
        io = fixture_io(&fixture);
        fixture_evl_configuration(&configuration);
        (void)rtl8168_chip_identify(&io, &chip);
        CHECK(rtl8168_cold_boot(&io, &chip, &configuration, test_mac, &phy,
                               &report) == RTL8168_IO_ERROR &&
                  fixture.phy_model.fault_fired && report.failed &&
                  report.failed_stage == faults[i].stage,
              "EVL control/selector refusal names its actual failed stage");
        CHECK(phy.failure.valid && phy.failure.step == steps[i] &&
                  phy.failure.phase == phases[i] &&
                  phy.failure.page == requested_pages[i] &&
                  phy.failure.selector_valid == (requested_selectors[i] != 0xffffU) &&
                  (!phy.failure.selector_valid ||
                   phy.failure.selector == requested_selectors[i]) &&
                  phy.failure.transaction.reg == faults[i].reg &&
                  phy.failure.transaction.write == faults[i].write,
              "control/selector refusal retains requested addressing and substep");
        CHECK(phy.first_error == RTL8168_IO_ERROR && phy.page_valid &&
                  fixture.phy_page == 0U && !fixture.mac_enabled &&
                  report.stages[RTL8168_STAGE_ANEG] == RTL8168_STAGE_SKIPPED,
              "EVL control/selector refusal recovers page and skips AN");
    }
}

static void test_evl_aneg_command_failure(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;
    struct rtl8168_bring_up configuration;
    struct rtl8168_phy_state phy;
    struct rtl8168_cold_boot_report report;
    fixture_init(&fixture, UINT32_C(0x2c900000));
    fixture.phy_model.fault_enabled = true;
    fixture.phy_model.fault_write = true;
    fixture.phy_model.fault_after_writes = 19U;
    fixture.phy_model.fault_reg = 0U;
    io = fixture_io(&fixture);
    fixture_evl_configuration(&configuration);
    (void)rtl8168_chip_identify(&io, &chip);
    CHECK(rtl8168_cold_boot(&io, &chip, &configuration, test_mac, &phy,
                           &report) == RTL8168_OK &&
              fixture.phy_model.fault_fired && !phy.aneg_start_issued,
          "failed AN command leaves MAC available and PHY refused");
    CHECK(report.stages[RTL8168_STAGE_ANEG] == RTL8168_STAGE_FAILED &&
              report.first_failure == RTL8168_IO_ERROR,
          "attempted AN command failure is FAILED rather than SKIPPED");
}

static void test_evl_phy_other_profiles_unchanged(void)
{
    const uint16_t xids[] = {0x300, 0x281, 0x2c0, 0x2c1, 0x480, 0x481,
                            0x488, 0x4c0, 0x509, 0x5c8, 0x541, 0x54b,
                            0x54a, 0x502};
    for (size_t i = 0; i < sizeof(xids) / sizeof(xids[0]); ++i) {
        struct mmio_fixture fixture;
        struct rtl8168_mmio_io io;
        struct rtl8168_chip chip;
        struct rtl8168_bring_up configuration;
        struct rtl8168_phy_state phy;
        struct rtl8168_cold_boot_report report;
        fixture_init(&fixture, (uint32_t)xids[i] << 20U);
        io = fixture_io(&fixture);
        fixture_evl_configuration(&configuration);
        CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK &&
                  rtl8168_cold_boot(&io, &chip, &configuration, test_mac,
                                   &phy, &report) == RTL8168_OK,
              "non-EVL profile retains its production cold start");
        CHECK(fixture.phy_model.reset_requests == 0U &&
                  fixture.phy_model.extended[0xac][0x18] == 0U &&
                  fixture.phy_model.parameters[0x8b5b] == 0U,
              "non-EVL profile receives no EVL tuning or PHY reset");
    }
}

static void test_cold_boot_evl_sequence(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;
    struct rtl8168_bring_up configuration;
    struct rtl8168_phy_state phy;
    struct rtl8168_cold_boot_report report;

    fixture_init(&fixture, TEST_TX_CONFIG_8168E);
    fixture_set_phy_link(&fixture, false);
    io = fixture_io(&fixture);
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK &&
              chip.profile == RTL8168_PROFILE_8168EVL,
          "the 0x2C9 row is the 8168evl profile");
    CHECK(strcmp(rtl8168_profile_string(chip.profile),
                 "RTL8168evl/8111evl") == 0,
          "the profile carries the reference driver's own name");
    CHECK(rtl8168_profile_evl_cold_boot(&chip) &&
              !rtl8168_profile_rxdv_gate(&chip) &&
              !rtl8168_profile_rx_early_off(&chip) &&
              rtl8168_profile_tx_auto_fifo(&chip),
          "the generation bounds are the evl's, not the family's");
    fixture_evl_configuration(&configuration);
    memset(&phy, 0, sizeof(phy));
    memset(&report, 0, sizeof(report));
    CHECK(rtl8168_cold_boot(&io, &chip, &configuration, test_mac, &phy,
                            &report) == RTL8168_OK,
          "the cold start completes");
    CHECK(report.profile == RTL8168_PROFILE_8168EVL &&
              strcmp(report.profile_name, "RTL8168evl/8111evl") == 0,
          "the report names the confirmed profile");
    CHECK(report.stages[RTL8168_STAGE_RESET] == RTL8168_STAGE_OK &&
              report.stages[RTL8168_STAGE_EPHY] == RTL8168_STAGE_OK &&
              report.stages[RTL8168_STAGE_CLOCK] == RTL8168_STAGE_OK &&
              report.stages[RTL8168_STAGE_MAC] == RTL8168_STAGE_OK &&
              report.stages[RTL8168_STAGE_PHY] == RTL8168_STAGE_OK &&
              report.stages[RTL8168_STAGE_ANEG] == RTL8168_STAGE_OK,
          "every stage of the documented order ran and succeeded");
    CHECK(!report.failed && report.first_failure == RTL8168_OK,
          "a complete start records no failure");
    CHECK(report.firmware_skipped,
          "the vendor firmware dependency the driver cannot satisfy is named");
    CHECK(fixture.order_violations == 0U,
          "the model saw the documented order");
    CHECK(fixture.forbidden_writes == 0U &&
              fixture.bad_register_accesses == 0U,
          "no generation-foreign register and no page-mismatched PHY access");
    CHECK(fixture.ephy_writes == 4U,
          "the 8168evl EPHY table wrote its four registers");
    CHECK(fixture.ephy_registers[0x09] == 0x0080U &&
              fixture.ephy_registers[0x19] == 0x0224U &&
              fixture.ephy_registers[0x00] == 0x0004U &&
              (fixture.ephy_registers[0x0C] & 0x3DF0U) == 0U &&
              (fixture.ephy_registers[0x0C] & 0x0200U) != 0U,
          "the EPHY registers hold the documented values");
    CHECK(fixture.eri_registers[0xC0] == 0U &&
              fixture.eri_registers[0xB8] == 0U &&
              fixture.eri_registers[0xC8] == 0x00100002U &&
              fixture.eri_registers[0xE8] == 0x00100006U &&
              fixture.eri_registers[0xCC] == 0x50U &&
              fixture.eri_registers[0xD0] == 0x07FF0060U,
          "the clock stage's extended-register words are the documented ones");
    CHECK((fixture.eri_registers[0x1D0] & 0x2U) != 0U &&
              (fixture.eri_registers[0x1B0] & (0x10U | 0x3U)) ==
                  (0x10U | 0x3U) &&
              (fixture.eri_registers[0xD4] & 0x1F00U) == 0x1F00U,
          "the extended bits above 0xff are addressed correctly");
    CHECK((fixture.registers[RTL8168_REG_MCU] &
           RTL8168_MCU_NOW_IS_OOB) == 0U,
          "the PHY MCU is taken out of out-of-band mode");
    CHECK((fixture.registers[RTL8168_REG_DLLPR] &
           RTL8168_DLLPR_PFM_EN) != 0U,
          "the power-management clock is enabled");
    CHECK((load32(fixture.registers, RTL8168_REG_MISC) &
           RTL8168_MISC_PWM_EN) != 0U,
          "the PWM block is turned on");
    CHECK((fixture.registers[RTL8168_REG_CONFIG5] &
           RTL8168_CONFIG5_SPI_EN) == 0U,
          "the SPI clock is turned off");
    CHECK((load32(fixture.registers, RTL8168_REG_MISC) &
           RTL8168_MISC_RXDV_GATED_EN) != 0U,
          "the evl's receive gate bit is left exactly as the part came up");
    CHECK((fixture.rx_config_value & RTL8168_RX_EARLY_OFF) == 0U &&
              (fixture.rx_config_value & RTL8168_RX_MULTI_EN) != 0U,
          "the evl RxConfig carries RX_MULTI_EN and not RX_EARLY_OFF");
    CHECK((fixture.tx_config_value & RTL8168_TX_CONFIG_AUTO_FIFO) != 0U,
          "the evl TxConfig carries the automatic FIFO bit");
    CHECK(fixture.max_tx_packet_size == RTL8168_MAX_TX_PACKET_SIZE_EARLY,
          "the evl start programs the early transmit size");
    CHECK(fixture.intr_mitigate == 0U,
          "interrupt coalescing is disabled");
    CHECK(phy.configure_attempted && phy.config_status == RTL8168_OK &&
              phy.page_valid && phy.page == RTL8168_PHY_PAGE_STANDARD,
          "the PHY stage configured on the standard page");
    CHECK(phy.advertise_written == RTL8168_PHY_ADVERTISE_DEFAULT &&
              phy.advertise == RTL8168_PHY_ADVERTISE_DEFAULT,
          "register 4 advertises the standard 10/100 set");
    CHECK(phy.gigabit && phy.advertise_1000_valid &&
              phy.advertise_1000 == RTL8168_PHY_ADVERTISE_1000 &&
              phy.advertise_1000_written == RTL8168_PHY_ADVERTISE_1000,
          "register 9 advertises 1000BASE-T and reads back");
    CHECK(phy.lpa_1000_valid && phy.lpa_1000 == 0U,
          "the partner's 1000BASE-T status was read and is empty");
    CHECK(phy.phy_status_initial_valid && phy.bmcr_initial_valid,
          "the PHYstatus and BMCR the part came up with are kept");
    CHECK(phy.bmsr_reads >= 1U && phy.bmsr_sequence_count >= 1U &&
              !phy.link_seen && !phy.aneg_complete_seen,
          "the first BMSR sample is kept and shows no link yet");
}

/* The old, MAC-only path on this generation is the wrong order by the model. */
static void test_cold_boot_evl_rejects_a_missing_stage(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;
    struct rtl8168_bring_up configuration;

    fixture_init(&fixture, TEST_TX_CONFIG_8168E);
    io = fixture_io(&fixture);
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK,
          "the part is identified before the short start");
    fixture_evl_configuration(&configuration);
    CHECK(rtl8168_bring_up(&io, &chip, &configuration, test_mac) ==
              RTL8168_OK,
          "the MAC-only start still completes electrically");
    CHECK(fixture.order_violations == 1U,
          "the model refuses the MAC enable that skipped the ephy/clock stages");
    CHECK(fixture.ephy_writes == 0U,
          "the short start never ran the EPHY stage");
}

static void test_cold_boot_evl_refusals(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;
    struct rtl8168_bring_up configuration;
    struct rtl8168_phy_state phy;
    struct rtl8168_cold_boot_report report;

    /* A refused EPHY write stops the start at that stage. */
    fixture_init(&fixture, TEST_TX_CONFIG_8168E);
    fixture.fail_ephy = true;
    io = fixture_io(&fixture);
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK,
          "the part is identified before the refused EPHY stage");
    fixture_evl_configuration(&configuration);
    memset(&phy, 0, sizeof(phy));
    memset(&report, 0, sizeof(report));
    CHECK(rtl8168_cold_boot(&io, &chip, &configuration, test_mac, &phy,
                            &report) == RTL8168_IO_ERROR,
          "a refused EPHY access fails the cold start");
    CHECK(report.failed && report.failed_stage == RTL8168_STAGE_EPHY &&
              report.first_failure == RTL8168_IO_ERROR,
          "the refusal names the EPHY stage and keeps the first failure");
    CHECK(report.stages[RTL8168_STAGE_RESET] == RTL8168_STAGE_OK &&
              report.stages[RTL8168_STAGE_EPHY] == RTL8168_STAGE_FAILED &&
              report.stages[RTL8168_STAGE_MAC] == RTL8168_STAGE_SKIPPED &&
              report.stages[RTL8168_STAGE_PHY] == RTL8168_STAGE_SKIPPED,
          "the stages after the refusal are skipped by name");
    CHECK(!fixture.mac_enabled,
          "the MAC is never enabled after a refused stage");

    /* A refused extended-register write stops it in the clock stage. */
    fixture_init(&fixture, TEST_TX_CONFIG_8168E);
    fixture.fail_eri = true;
    io = fixture_io(&fixture);
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK,
          "the part is identified before the refused clock stage");
    memset(&phy, 0, sizeof(phy));
    memset(&report, 0, sizeof(report));
    CHECK(rtl8168_cold_boot(&io, &chip, &configuration, test_mac, &phy,
                            &report) == RTL8168_IO_ERROR,
          "a refused extended-register write fails the cold start");
    CHECK(report.failed && report.failed_stage == RTL8168_STAGE_CLOCK &&
              report.stages[RTL8168_STAGE_EPHY] == RTL8168_STAGE_OK,
          "the refusal names the clock stage after a completed EPHY stage");
    CHECK(!fixture.mac_enabled,
          "the MAC stays disabled after the refused clock stage");
}

/*
 * The page the firmware left behind: registers 0..15 are the standard set only
 * on page 0, so the start has to select it before it reads anything it means to
 * interpret - and the model counts every access made on the wrong page.
 */
static void test_cold_boot_evl_recovers_the_phy_page(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;
    struct rtl8168_bring_up configuration;
    struct rtl8168_phy_state phy;
    struct rtl8168_cold_boot_report report;

    fixture_init(&fixture, TEST_TX_CONFIG_8168E);
    fixture.phy_page = 0x0007U;
    io = fixture_io(&fixture);
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK,
          "the part is identified with a non-standard PHY page");
    fixture_evl_configuration(&configuration);
    memset(&phy, 0, sizeof(phy));
    memset(&report, 0, sizeof(report));
    CHECK(rtl8168_cold_boot(&io, &chip, &configuration, test_mac, &phy,
                            &report) == RTL8168_OK,
          "the cold start recovers a part left on another page");
    CHECK(fixture.bad_register_accesses == 0U,
          "no standard register was touched before page 0 was selected");
    CHECK(fixture.phy_page == RTL8168_PHY_PAGE_STANDARD && phy.page_valid &&
              phy.page == RTL8168_PHY_PAGE_STANDARD,
          "the part settled on page 0 and the state records it");
    CHECK(phy.advertise_valid && phy.advertise_1000_valid,
          "the read-backs are trustworthy after the recovery");
}

/* A read-back that does not answer keeps the first error and its own verdict. */
static void test_cold_boot_evl_keeps_the_first_error(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;
    struct rtl8168_bring_up configuration;
    struct rtl8168_phy_state phy;
    struct rtl8168_cold_boot_report report;

    fixture_init(&fixture, TEST_TX_CONFIG_8168E);
    fixture.phy_fail_register = (int16_t)RTL8168_PHY_REG_CTRL1000;
    io = fixture_io(&fixture);
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK,
          "the part is identified before the failed gigabit read-back");
    fixture_evl_configuration(&configuration);
    memset(&phy, 0, sizeof(phy));
    memset(&report, 0, sizeof(report));
    CHECK(rtl8168_cold_boot(&io, &chip, &configuration, test_mac, &phy,
                            &report) == RTL8168_OK,
          "a PHY that cannot be read back does not fail the whole boot");
    CHECK(report.stages[RTL8168_STAGE_PHY] == RTL8168_STAGE_OK,
          "the configuration was applied");
    CHECK(!phy.advertise_1000_valid,
          "the gigabit advertisement is marked unverified");
    CHECK(phy.first_error == RTL8168_IO_ERROR,
          "the first PHY error is kept");
    CHECK(phy.config_status == RTL8168_OK && phy.valid,
          "CONFIGURED stays the configuration's own result");
    CHECK(phy.advertise_valid,
          "the 10/100 read-back that did answer stays verified");
}

/* The BMSR samples a link that comes up late needs. */
static void test_cold_boot_evl_samples_the_bmsr(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_phy_state state;
    struct rtl8168_chip chip;
    struct rtl8168_bring_up configuration;
    struct rtl8168_cold_boot_report report;

    fixture_init(&fixture, TEST_TX_CONFIG_8168E);
    fixture_set_phy_link(&fixture, false);
    io = fixture_io(&fixture);
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK,
          "the part is identified before the sampling");
    fixture_evl_configuration(&configuration);
    memset(&state, 0, sizeof(state));
    memset(&report, 0, sizeof(report));
    CHECK(rtl8168_cold_boot(&io, &chip, &configuration, test_mac, &state,
                            &report) == RTL8168_OK,
          "the cold start completes before the link arrives");
    CHECK(!state.link_seen && !state.aneg_complete_seen &&
              state.bmsr_sequence_count == 1U,
          "the down link keeps one distinct sample so far");
    fixture_set_phy_link(&fixture, true);
    CHECK(rtl8168_phy_refresh(&io, &state) == RTL8168_OK,
          "the refresh reads the part after the link arrives");
    CHECK(state.link_seen && state.aneg_complete_seen &&
              state.link_up && state.autoneg_complete,
          "the transition is visible and remembered");
    CHECK(state.bmsr_sequence_count == 2U &&
              state.bmsr_sequence[0] != state.bmsr_sequence[1],
          "the second distinct BMSR value is kept in the sequence");
    CHECK(state.bmsr_reads >= 2U,
          "the number of readings is kept");
    CHECK(state.phy_status_latest != 0U,
          "the raw PHYstatus is kept beside the MII reading");
}

/* The other supported generations keep their own bounds. */
static void test_phy_sample_survives_later_read_failure(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;
    struct rtl8168_bring_up configuration;
    struct rtl8168_phy_state phy;
    struct rtl8168_cold_boot_report report;
    uint32_t before;
    fixture_init(&fixture, UINT32_C(0x2c900000));
    fixture.phy_fail_register = RTL8168_PHY_REG_LPA;
    io = fixture_io(&fixture);
    fixture_evl_configuration(&configuration);
    (void)rtl8168_chip_identify(&io, &chip);
    CHECK(rtl8168_cold_boot(&io, &chip, &configuration, test_mac, &phy,
                           &report) == RTL8168_OK && !phy.status_valid,
          "later partner-read failure refuses the current status snapshot");
    CHECK(phy.bmsr_reads == 1U && phy.link_seen && phy.aneg_complete_seen,
          "cold-start BMSR observation survives a later partner-read failure");
    fixture_init(&fixture, UINT32_C(0x2c900000));
    fixture_set_phy_link(&fixture, false);
    io = fixture_io(&fixture);
    (void)rtl8168_chip_identify(&io, &chip);
    (void)rtl8168_cold_boot(&io, &chip, &configuration, test_mac, &phy, &report);
    before = phy.bmsr_reads;
    fixture_set_phy_link(&fixture, true);
    fixture.phy_fail_register = RTL8168_PHY_REG_LPA;
    CHECK(rtl8168_phy_refresh(&io, &phy) == RTL8168_IO_ERROR &&
              !phy.status_valid && phy.first_error == RTL8168_IO_ERROR,
          "partial refresh is unverified and keeps its first error");
    CHECK(phy.bmsr_reads == before + 1U && phy.link_seen && phy.aneg_complete_seen,
          "refresh BMSR observation survives a later partner-read failure");
}

static void test_generation_bounds(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;

    fixture_init(&fixture, TEST_TX_CONFIG_8168H);
    io = fixture_io(&fixture);
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK &&
              chip.profile == RTL8168_PROFILE_8168H,
          "the acceptance host's 0x541 part is the 8168H profile");
    CHECK(rtl8168_profile_rxdv_gate(&chip) &&
              rtl8168_profile_rx_early_off(&chip) &&
              rtl8168_profile_rx_multi_en(&chip) &&
              rtl8168_profile_tx_auto_fifo(&chip) &&
              !rtl8168_profile_evl_cold_boot(&chip),
          "the 8168H keeps the gate, RX_EARLY_OFF and the auto FIFO");

    fixture_init(&fixture, TEST_TX_CONFIG_CLASSIC);
    io = fixture_io(&fixture);
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK &&
              chip.profile == RTL8168_PROFILE_CLASSIC,
          "an 8168B part is the classic profile");
    CHECK(!rtl8168_profile_rxdv_gate(&chip) &&
              !rtl8168_profile_rx_early_off(&chip) &&
              !rtl8168_profile_rx_multi_en(&chip) &&
              !rtl8168_profile_tx_auto_fifo(&chip),
          "a classic part claims none of the extended generation settings");

    {
        struct rtl8168_chip part;

        memset(&part, 0, sizeof(part));
        part.profile = RTL8168_PROFILE_8168E;
        CHECK(rtl8168_profile_rx_multi_en(&part) &&
                  !rtl8168_profile_tx_auto_fifo(&part) &&
                  !rtl8168_profile_rx_early_off(&part),
              "the E generation has RX_MULTI_EN but not the evl settings");
        CHECK(strcmp(rtl8168_profile_string(part.profile),
                     "RTL8168e/8111e") == 0,
              "the E profile is named");
    }
}

static void test_rx_armed(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;
    struct rtl8168_bring_up configuration;
    bool armed = false;
    const uint64_t ring = TEST_DMA_BASE;

    memset(&configuration, 0, sizeof(configuration));
    fixture_init(&fixture, TEST_TX_CONFIG_8168H);
    io = fixture_io(&fixture);
    configuration.rx_descriptor_physical = ring;
    configuration.tx_descriptor_physical =
        ring + RTL8168_DESCRIPTOR_ALIGNMENT;
    configuration.rx_buffer_capacity = TEST_BUFFER_CAPACITY;
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK &&
              rtl8168_bring_up(&io, &chip, &configuration, test_mac) ==
                  RTL8168_OK,
          "the fixture part comes up with the receive ring armed");

    CHECK(rtl8168_rx_armed(&io, ring, &armed) == RTL8168_OK && armed,
          "the receive ring the part accepted is reported armed");

    /* A part holding a different ring is not armed for this driver. */
    CHECK(rtl8168_rx_armed(&io, ring + 4096U, &armed) == RTL8168_OK &&
              !armed,
          "a descriptor base the part does not hold is not armed");

    /* The receiver-enable bit is half of the answer. */
    fixture.registers[RTL8168_REG_CHIP_CMD] = 0U;
    CHECK(rtl8168_rx_armed(&io, ring, &armed) == RTL8168_OK && !armed,
          "a ring with the receiver disabled is not armed");

    fixture.fail_read = true;
    CHECK(rtl8168_rx_armed(&io, ring, &armed) == RTL8168_IO_ERROR && !armed,
          "a failed readback reports no armed ring");

    CHECK(rtl8168_rx_armed(NULL, ring, &armed) == RTL8168_NULL && !armed,
          "a missing device reports no armed ring");
}

/*
 * ITEM-435: the receive data-valid gate.
 *
 * The 8168H comes out of reset with MISC bit 19 (`RXDV_GATED_EN`) set, and a
 * part whose gate stays closed answers every register, completes transmit
 * descriptors and never writes a receive descriptor: no frame, no receive
 * error, no interrupt.  That is the 2026-09-17 log's shape, so the bring-up
 * has to open the gate before it enables the receiver - and the proof starts
 * from a part whose gate is closed, which is the state the hardware run had.
 */
static void test_rxdv_gate(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_chip chip;
    struct rtl8168_bring_up configuration;

    memset(&configuration, 0, sizeof(configuration));
    fixture_init(&fixture, TEST_TX_CONFIG_8168H);
    io = fixture_io(&fixture);
    store32(fixture.registers, RTL8168_REG_MISC,
            RTL8168_MISC_RXDV_GATED_EN);
    configuration.rx_descriptor_physical = TEST_DMA_BASE;
    configuration.tx_descriptor_physical =
        TEST_DMA_BASE + RTL8168_DESCRIPTOR_ALIGNMENT;
    configuration.rx_buffer_capacity = TEST_BUFFER_CAPACITY;

    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK &&
              chip.family == RTL8168_CHIP_EXTENDED,
          "the fixture part is the extended family");
    CHECK(rtl8168_bring_up(&io, &chip, &configuration, test_mac) ==
              RTL8168_OK,
          "the part with a closed receive gate still comes up");
    CHECK((load32(fixture.registers, RTL8168_REG_MISC) &
           RTL8168_MISC_RXDV_GATED_EN) == 0U,
          "the receive data-valid gate is open after the bring-up");
    CHECK(fixture.chip_cmd_writes != 0U,
          "the gate was opened before the receiver was enabled");
    CHECK(fixture.enable_seen && fixture.gate_open_before_enable,
          "the part's gate was already open when the receiver was enabled");

    /*
     * ITEM-439: the arming answer the bind now depends on.  The bring-up
     * programmed RDSAR with the ring's physical address and enabled the
     * receiver, so the part must answer "armed" for exactly that address; a
     * different address is the negative control, and an unreadable part is not
     * an answer at all.
     */
    {
        bool armed = true;

        CHECK(rtl8168_rx_armed(&io, TEST_DMA_BASE, &armed) == RTL8168_OK,
              "the part answers whether it holds the receive ring");
        CHECK(armed,
              "the receiver holds the ring the bring-up programmed");
        CHECK(rtl8168_rx_armed(&io, TEST_DMA_BASE + 0x1000U, &armed) ==
                  RTL8168_OK &&
                  !armed,
              "a ring address the part was not given reads as not armed");
        fixture.fail_read = true;
        CHECK(rtl8168_rx_armed(&io, TEST_DMA_BASE, &armed) ==
                  RTL8168_IO_ERROR,
              "an unreadable part is an error, not an armed or disarmed answer");
        fixture.fail_read = false;
    }

    /*
     * The gate is the extended family's.  A classic part keeps its MISC
     * register untouched: the reference driver opens the gate only from the
     * 8168G MAC version on, and this driver only claims the map it proved.
     */
    fixture_init(&fixture, TEST_TX_CONFIG_CLASSIC);
    io = fixture_io(&fixture);
    store32(fixture.registers, RTL8168_REG_MISC,
            RTL8168_MISC_RXDV_GATED_EN);
    CHECK(rtl8168_chip_identify(&io, &chip) == RTL8168_OK &&
              chip.family == RTL8168_CHIP_CLASSIC,
          "the second fixture part is the classic family");
    CHECK(rtl8168_bring_up(&io, &chip, &configuration, test_mac) ==
              RTL8168_OK,
          "the classic part comes up");
    CHECK((load32(fixture.registers, RTL8168_REG_MISC) &
           RTL8168_MISC_RXDV_GATED_EN) != 0U,
          "the classic part's MISC register is left alone");
}

static void test_region_layout(void)
{
    struct rtl8168_region_layout layout;

    CHECK(rtl8168_region_calculate(&layout) == RTL8168_OK,
          "the DMA region layout is calculated");
    CHECK(layout.rx_descriptor_offset == 0U,
          "the receive descriptors start at the region base");
    CHECK(layout.tx_descriptor_offset == RTL8168_DESCRIPTOR_ALIGNMENT,
          "the transmit descriptors are 256-byte aligned");
    CHECK((layout.rx_buffer_offset % 4096U) == 0U &&
              (layout.tx_buffer_offset % 4096U) == 0U,
          "the frame buffers are page aligned");
    CHECK(layout.length <= 65536U,
          "the whole region fits the bounded DMA allocation");
    CHECK(layout.rx_buffer_offset == 4096U,
          "the receive buffers follow both descriptor rings");
    CHECK(layout.tx_buffer_offset ==
              4096U + RTL8168_RX_DESCRIPTOR_COUNT *
                          RTL8168_RX_BUFFER_STRIDE,
          "the transmit buffers follow the receive buffers");
    CHECK(layout.length ==
              layout.tx_buffer_offset +
                  RTL8168_TX_DESCRIPTOR_COUNT * RTL8168_TX_BUFFER_STRIDE,
          "the region ends after the transmit buffers");
    CHECK(rtl8168_region_calculate(NULL) == RTL8168_NULL,
          "a missing layout output is refused");
}

static void test_rx_ring(void)
{
    static struct rtl8168_descriptor descriptors[TEST_DESCRIPTOR_COUNT];
    struct rtl8168_ring ring;
    uint32_t length = 0U;
    uint32_t index;

    memset(descriptors, 0, sizeof(descriptors));
    rtl8168_ring_init(&ring, descriptors, TEST_DESCRIPTOR_COUNT);
    CHECK(ring.capacity == TEST_DESCRIPTOR_COUNT && ring.index == 0U,
          "the receive ring starts empty at index zero");

    CHECK(rtl8168_rx_post(&ring, TEST_DMA_BASE, TEST_BUFFER_CAPACITY) ==
              RTL8168_OK,
          "the receive ring is posted");
    for (index = 0U; index < TEST_DESCRIPTOR_COUNT; ++index) {
        const uint32_t opts1 = descriptors[index].opts1;

        if ((opts1 & RTL8168_DESC_OWN) == 0U ||
            (opts1 & RTL8168_DESC_LENGTH_MASK) != TEST_BUFFER_CAPACITY ||
            descriptors[index].address !=
                TEST_DMA_BASE + (uint64_t)index * TEST_BUFFER_CAPACITY) {
            break;
        }
    }
    CHECK(index == TEST_DESCRIPTOR_COUNT,
          "every posted descriptor is owned by the device with the buffer size");
    CHECK((descriptors[TEST_DESCRIPTOR_COUNT - 1U].opts1 &
           RTL8168_DESC_RING_END) != 0U &&
              (descriptors[0].opts1 & RTL8168_DESC_RING_END) == 0U,
          "only the last descriptor carries the ring-end bit");

    CHECK(rtl8168_rx_complete(&ring, TEST_BUFFER_CAPACITY, &length) ==
              RTL8168_RX_NOT_READY,
          "a descriptor still owned by the device is not a frame");

    /* The device completes a full 64-byte frame plus its 4-byte FCS. */
    descriptors[0].opts1 = RTL8168_DESC_FIRST_FRAGMENT |
                           RTL8168_DESC_LAST_FRAGMENT |
                           (TEST_FRAME_LENGTH + RTL8168_FCS_LENGTH);
    length = 0U;
    CHECK(rtl8168_rx_complete(&ring, TEST_BUFFER_CAPACITY, &length) ==
              RTL8168_OK,
          "a complete frame is decoded from the descriptor");
    CHECK(length == TEST_FRAME_LENGTH,
          "the descriptor FCS is removed from the reported frame length");

    CHECK(rtl8168_rx_repost(&ring, TEST_DMA_BASE,
                            TEST_BUFFER_CAPACITY) == RTL8168_OK,
          "the consumed descriptor is posted back");
    CHECK(ring.index == 1U,
          "the receive cursor advances after the repost");
    CHECK((descriptors[0].opts1 & RTL8168_DESC_OWN) != 0U,
          "the reposted descriptor is owned by the device again");

    /* Error and malformed frames are dropped, not fed to the stack. */
    descriptors[1].opts1 = RTL8168_DESC_FIRST_FRAGMENT |
                           RTL8168_DESC_LAST_FRAGMENT |
                           RTL8168_RX_STATUS_RECEIVE_ERROR |
                           (TEST_FRAME_LENGTH + RTL8168_FCS_LENGTH);
    CHECK(rtl8168_rx_complete(&ring, TEST_BUFFER_CAPACITY, &length) ==
              RTL8168_RX_DROPPED,
          "a receive-error frame is dropped");
    (void)rtl8168_rx_repost(&ring, TEST_DMA_BASE, TEST_BUFFER_CAPACITY);

    descriptors[ring.index % TEST_DESCRIPTOR_COUNT].opts1 =
        RTL8168_DESC_FIRST_FRAGMENT | (TEST_FRAME_LENGTH + RTL8168_FCS_LENGTH);
    CHECK(rtl8168_rx_complete(&ring, TEST_BUFFER_CAPACITY, &length) ==
              RTL8168_RX_DROPPED,
          "a frame that is not a whole frame is dropped");
    (void)rtl8168_rx_repost(&ring, TEST_DMA_BASE, TEST_BUFFER_CAPACITY);

    descriptors[ring.index % TEST_DESCRIPTOR_COUNT].opts1 =
        RTL8168_DESC_FIRST_FRAGMENT | RTL8168_DESC_LAST_FRAGMENT |
        (4U + 10U);
    CHECK(rtl8168_rx_complete(&ring, TEST_BUFFER_CAPACITY, &length) ==
              RTL8168_RX_DROPPED,
          "a runt frame is dropped");
    (void)rtl8168_rx_repost(&ring, TEST_DMA_BASE, TEST_BUFFER_CAPACITY);

    descriptors[ring.index % TEST_DESCRIPTOR_COUNT].opts1 =
        RTL8168_DESC_FIRST_FRAGMENT | RTL8168_DESC_LAST_FRAGMENT |
        (TEST_BUFFER_CAPACITY + 8U + RTL8168_FCS_LENGTH);
    CHECK(rtl8168_rx_complete(&ring, TEST_BUFFER_CAPACITY, &length) ==
              RTL8168_RX_DROPPED,
          "a length beyond the buffer is dropped");

    CHECK(rtl8168_rx_complete(NULL, TEST_BUFFER_CAPACITY, &length) ==
              RTL8168_NULL,
          "a missing ring is refused");
}

static void test_tx_ring(void)
{
    static struct rtl8168_descriptor descriptors[TEST_DESCRIPTOR_COUNT];
    struct rtl8168_ring ring;
    uint32_t reclaimed = 0U;
    uint32_t index;

    CHECK(rtl8168_tx_frame_length(0U) == 0U,
          "an empty frame has no descriptor length");
    CHECK(rtl8168_tx_frame_length(10U) == RTL8168_FRAME_MINIMUM,
          "a short frame is padded to the Ethernet minimum");
    CHECK(rtl8168_tx_frame_length(RTL8168_FRAME_MINIMUM) ==
              RTL8168_FRAME_MINIMUM,
          "a minimum-size frame keeps its length");
    CHECK(rtl8168_tx_frame_length(1514U) == 1514U,
          "a full IPv4 frame keeps its length");
    CHECK(rtl8168_tx_frame_length(RTL8168_FRAME_MAXIMUM) ==
              RTL8168_FRAME_MAXIMUM,
          "a maximum-size frame keeps its length");
    CHECK(rtl8168_tx_frame_length(RTL8168_FRAME_MAXIMUM + 1U) == 0U,
          "a frame beyond the Ethernet maximum is refused");
    CHECK(rtl8168_tx_frame_length(SIZE_MAX) == 0U,
          "an unrepresentable frame length is refused");

    memset(descriptors, 0, sizeof(descriptors));
    rtl8168_ring_init(&ring, descriptors, TEST_DESCRIPTOR_COUNT);

    CHECK(rtl8168_tx_program(&ring, TEST_DMA_BASE + 4096U,
                             RTL8168_FRAME_MINIMUM, 1536U) == RTL8168_OK,
          "a padded minimum frame is programmed");
    CHECK((descriptors[0].opts1 & RTL8168_DESC_OWN) != 0U &&
              (descriptors[0].opts1 & RTL8168_DESC_LENGTH_MASK) ==
                  RTL8168_FRAME_MINIMUM,
          "the transmit descriptor carries the real frame length");
    CHECK((descriptors[0].opts1 & (RTL8168_DESC_FIRST_FRAGMENT |
                                   RTL8168_DESC_LAST_FRAGMENT)) ==
              (RTL8168_DESC_FIRST_FRAGMENT |
               RTL8168_DESC_LAST_FRAGMENT),
          "a single-buffer frame is both the first and the last fragment");
    CHECK(descriptors[0].address == TEST_DMA_BASE + 4096U,
          "the transmit descriptor carries the frame address");
    CHECK(ring.in_flight == 1U && ring.index == 1U,
          "the transmit window records the in-flight frame");

    CHECK(rtl8168_tx_reclaim(&ring, &reclaimed) == RTL8168_RING_EMPTY &&
              reclaimed == 0U && ring.in_flight == 1U,
          "a frame the device still owns is not reclaimed");

    memset(descriptors, 0, sizeof(descriptors));
    ring.index = 0U;
    ring.in_flight = 0U;
    CHECK(rtl8168_tx_program(&ring, TEST_DMA_BASE, RTL8168_FRAME_MINIMUM,
                             1536U) == RTL8168_OK &&
              rtl8168_tx_program(&ring, TEST_DMA_BASE + 1536U,
                                 RTL8168_FRAME_MAXIMUM, 1536U) ==
                  RTL8168_OK,
          "two frames are programmed");
    /* The device completes only the first frame. */
    descriptors[0].opts1 &= ~RTL8168_DESC_OWN;
    CHECK(rtl8168_tx_reclaim(&ring, &reclaimed) == RTL8168_OK &&
              reclaimed == 1U && ring.in_flight == 1U,
          "only the completed frame is reclaimed");
    descriptors[1].opts1 &= ~RTL8168_DESC_OWN;
    CHECK(rtl8168_tx_reclaim(&ring, &reclaimed) == RTL8168_OK &&
              reclaimed == 1U && ring.in_flight == 0U,
          "the second completed frame is reclaimed");

    /* Fill the ring: the programming cursor wraps and the last slot has EOR. */
    ring.index = 0U;
    ring.in_flight = 0U;
    memset(descriptors, 0, sizeof(descriptors));
    for (index = 0U; index < TEST_DESCRIPTOR_COUNT; ++index) {
        if (rtl8168_tx_program(&ring, TEST_DMA_BASE + index * 1536U,
                               RTL8168_FRAME_MINIMUM, 1536U) !=
            RTL8168_OK) {
            break;
        }
    }
    CHECK(index == TEST_DESCRIPTOR_COUNT && ring.in_flight ==
              TEST_DESCRIPTOR_COUNT,
          "the transmit ring fills to its full in-flight window");
    CHECK((descriptors[TEST_DESCRIPTOR_COUNT - 1U].opts1 &
           RTL8168_DESC_RING_END) != 0U,
          "the last transmit slot carries the ring-end bit");
    CHECK(ring.index == 0U,
          "the transmit cursor wraps to the start of the ring");
    CHECK(rtl8168_tx_program(&ring, TEST_DMA_BASE, RTL8168_FRAME_MINIMUM,
                             1536U) == RTL8168_RING_FULL,
          "a full transmit ring refuses another frame");

    /* Length and address rules. */
    ring.in_flight = 0U;
    CHECK(rtl8168_tx_program(&ring, TEST_DMA_BASE, 10U, 1536U) ==
              RTL8168_INVALID_LENGTH,
          "a frame shorter than the Ethernet minimum is refused");
    CHECK(rtl8168_tx_program(&ring, TEST_DMA_BASE,
                             RTL8168_FRAME_MAXIMUM + 1U, 1536U) ==
              RTL8168_INVALID_LENGTH,
          "a frame beyond the Ethernet maximum is refused");
    CHECK(rtl8168_tx_program(&ring, TEST_DMA_BASE, 1514U, 512U) ==
              RTL8168_INVALID_LENGTH,
          "a frame longer than its buffer is refused");
    CHECK(rtl8168_tx_program(&ring, RTL8168_DMA_ADDRESS_LIMIT, 64U,
                             1536U) == RTL8168_UNSUPPORTED_DMA_ADDRESS,
          "a frame above the 32-bit DMA window is refused");

    /* Ring helpers. */
    CHECK(rtl8168_ring_next(&ring, TEST_DESCRIPTOR_COUNT - 1U) == 0U,
          "the ring index wraps");
    CHECK(rtl8168_ring_next(&ring, 3U) == 4U,
          "the ring index advances by one");
    rtl8168_ring_init(NULL, descriptors, TEST_DESCRIPTOR_COUNT);
    CHECK(rtl8168_tx_reclaim(NULL, &reclaimed) == RTL8168_NULL,
          "a missing transmit ring is refused");
}

/*
 * ITEM-428: the transmit ring's own counters and the rewind that follows a
 * device reset.
 *
 * The 2026-09-16 hardware run's log could not tell "the machine never
 * transmitted" from "it transmitted and was never answered".  These are the
 * counts that answer it - what the driver handed over, what the device handed
 * back, and what a restart had to abandon - and the rewind is the memory side
 * of the reference driver's transmit-timeout recovery.
 */
static void test_tx_counters_and_rewind(void)
{
    static struct rtl8168_descriptor descriptors[TEST_DESCRIPTOR_COUNT];
    struct rtl8168_ring ring;
    uint32_t reclaimed = 0U;
    uint32_t abandoned = 0U;
    uint32_t index;

    memset(descriptors, 0, sizeof(descriptors));
    rtl8168_ring_init(&ring, descriptors, TEST_DESCRIPTOR_COUNT);
    CHECK(ring.submitted == 0U && ring.reclaimed == 0U &&
              ring.abandoned == 0U,
          "a fresh transmit ring counts nothing");

    for (index = 0U; index < TEST_DESCRIPTOR_COUNT; ++index) {
        CHECK(rtl8168_tx_program(&ring, TEST_DMA_BASE + index * 1536U,
                                 RTL8168_FRAME_MINIMUM, 1536U) ==
                  RTL8168_OK,
              "a frame is programmed while the ring has room");
    }
    CHECK(ring.submitted == TEST_DESCRIPTOR_COUNT &&
              ring.in_flight == TEST_DESCRIPTOR_COUNT,
          "every programmed frame is counted as submitted");
    CHECK(rtl8168_tx_program(&ring, TEST_DMA_BASE, RTL8168_FRAME_MINIMUM,
                             1536U) == RTL8168_RING_FULL &&
              ring.submitted == TEST_DESCRIPTOR_COUNT,
          "a refused frame is not counted as submitted");

    /*
     * A device that has stopped clearing OWN leaves every descriptor in
     * flight: the ring reads full and nothing is reclaimed, which is exactly
     * the state the restart has to be able to leave.
     */
    CHECK(rtl8168_tx_reclaim(&ring, &reclaimed) == RTL8168_RING_EMPTY &&
              reclaimed == 0U && ring.reclaimed == 0U &&
              ring.in_flight == TEST_DESCRIPTOR_COUNT,
          "a device that completes nothing reclaims nothing");

    /* The device completes two, then stops again. */
    descriptors[0].opts1 &= ~RTL8168_DESC_OWN;
    descriptors[1].opts1 &= ~RTL8168_DESC_OWN;
    CHECK(rtl8168_tx_reclaim(&ring, &reclaimed) == RTL8168_OK &&
              reclaimed == 2U && ring.reclaimed == 2U &&
              ring.in_flight == TEST_DESCRIPTOR_COUNT - 2U,
          "the descriptors the device completed are counted as reclaimed");

    /*
     * The caller has reset the part and rebuilds the ring: the frames still in
     * flight are abandoned, counted, and every descriptor is handed back to
     * the CPU with no address and no ownership bit.
     */
    CHECK(rtl8168_tx_rewind(&ring, &abandoned) == RTL8168_OK &&
              abandoned == TEST_DESCRIPTOR_COUNT - 2U &&
              ring.abandoned == TEST_DESCRIPTOR_COUNT - 2U,
          "a rewind counts the frames it declared lost");
    CHECK(ring.in_flight == 0U && ring.index == 0U,
          "a rewind returns the ring cursors to the start");
    for (index = 0U; index < TEST_DESCRIPTOR_COUNT; ++index) {
        CHECK(descriptors[index].opts1 == 0U &&
                  descriptors[index].address == 0U,
              "a rewound descriptor is CPU-owned again");
    }
    CHECK(ring.submitted == TEST_DESCRIPTOR_COUNT &&
              ring.reclaimed == 2U,
          "a rewind does not rewrite the submission and completion history");
    CHECK(rtl8168_tx_program(&ring, TEST_DMA_BASE, RTL8168_FRAME_MINIMUM,
                             1536U) == RTL8168_OK &&
              ring.in_flight == 1U,
          "the ring takes frames again after a rewind");
    CHECK(rtl8168_tx_rewind(NULL, &abandoned) == RTL8168_NULL,
          "a missing transmit ring is refused by the rewind");
    {
        struct rtl8168_ring empty;

        rtl8168_ring_init(&empty, NULL, TEST_DESCRIPTOR_COUNT);
        CHECK(rtl8168_tx_rewind(&empty, &abandoned) == RTL8168_NULL,
              "a ring with no descriptors is refused by the rewind");
    }
}

static void test_dma_window(void)
{
    CHECK(rtl8168_dma_address_supported(UINT64_C(0x1000)),
          "a low physical address is inside the DMA window");
    CHECK(rtl8168_dma_address_supported(RTL8168_DMA_ADDRESS_LIMIT - 1U),
          "the last address inside the DMA window is accepted");
    CHECK(!rtl8168_dma_address_supported(RTL8168_DMA_ADDRESS_LIMIT),
          "the first address above the DMA window is refused");
    CHECK(!rtl8168_dma_address_supported(0U),
          "the null address is not a DMA address");
    CHECK(rtl8168_status_string(RTL8168_OK) != NULL &&
              rtl8168_status_string(RTL8168_RX_DROPPED) != NULL &&
              rtl8168_status_string(RTL8168_RESET_TIMEOUT) != NULL &&
              rtl8168_status_string(RTL8168_ENABLE_FAILED) != NULL &&
              rtl8168_status_string(RTL8168_UNSUPPORTED_DEVICE) != NULL,
          "the driver status strings are published");
}

/* ITEM-643: the live receive snapshot is read-only and bounded. */
static void test_rx_diagnostic(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_descriptor descriptors[4];
    struct rtl8168_ring ring;
    struct rtl8168_rx_diagnostic diagnostic;

    fixture_init(&fixture, 0x2C800000U);
    io = fixture_io(&fixture);
    fixture.registers[RTL8168_REG_CHIP_CMD] =
        (uint8_t)(RTL8168_CHIP_CMD_RX_ENABLE | RTL8168_CHIP_CMD_TX_ENABLE);
    store16(fixture.registers, RTL8168_REG_INTR_STATUS, 0x1234U);
    store16(fixture.registers, RTL8168_REG_INTR_MASK, 0x0000U);
    store32(fixture.registers, RTL8168_REG_RX_CONFIG, 0x0FU);
    store32(fixture.registers, RTL8168_REG_RX_DESC_LOW, 0x00001000U);
    store32(fixture.registers, RTL8168_REG_RX_DESC_HIGH, 0x00000000U);
    store32(fixture.registers, RTL8168_REG_MISC, 0x00000000U);
    memset(descriptors, 0, sizeof(descriptors));
    descriptors[0].opts1 = RTL8168_DESC_OWN | 0x100U;
    descriptors[0].address = 0x2000U;
    descriptors[1].opts1 = 0x200U;
    descriptors[2].opts1 = RTL8168_DESC_OWN;
    descriptors[3].opts1 = RTL8168_DESC_OWN;
    rtl8168_ring_init(&ring, descriptors, 4U);
    ring.index = 2U;
    ring.in_flight = 1U;

    CHECK(rtl8168_rx_diagnostic_read(&io, &ring, &diagnostic) == RTL8168_OK,
          "rx diagnostic reads every register");
    CHECK(diagnostic.available && strcmp(diagnostic.reason, "OK") == 0,
          "rx diagnostic is available");
    CHECK(diagnostic.chip_cmd ==
              (RTL8168_CHIP_CMD_RX_ENABLE | RTL8168_CHIP_CMD_TX_ENABLE),
          "rx diagnostic carries ChipCmd");
    CHECK(diagnostic.intr_status == 0x1234U,
          "rx diagnostic reads IntrStatus without acknowledging it");
    CHECK(diagnostic.intr_mask == 0U &&
              diagnostic.rx_config == 0x0FU &&
              diagnostic.rx_desc_low == 0x1000U &&
              diagnostic.rx_desc_high == 0U &&
              diagnostic.misc == 0U,
          "rx diagnostic carries the configured registers");
    CHECK(diagnostic.rx_index == 2U && diagnostic.rx_capacity == 4U &&
              diagnostic.rx_in_flight == 1U,
          "rx diagnostic carries the software ring state");
    CHECK(diagnostic.rx_own == 3U,
          "rx diagnostic counts the descriptors the device still owns");
    CHECK(diagnostic.rx_head_opts1 == (RTL8168_DESC_OWN | 0x100U) &&
              diagnostic.rx_head_address_low == 0x2000U,
          "rx diagnostic carries the first descriptor");

    CHECK(rtl8168_rx_diagnostic_read(&io, NULL, &diagnostic) == RTL8168_OK &&
              diagnostic.available && diagnostic.rx_capacity == 0U &&
              diagnostic.rx_own == 0U,
          "a missing ring still publishes the register state");

    fixture.fail_read = true;
    CHECK(rtl8168_rx_diagnostic_read(&io, &ring, &diagnostic) ==
              RTL8168_IO_ERROR &&
              !diagnostic.available &&
              strcmp(diagnostic.reason, "READ_FAILED") == 0,
          "a failed read is published as unavailable");
    fixture.fail_read = false;

    CHECK(rtl8168_rx_diagnostic_read(NULL, &ring, &diagnostic) ==
              RTL8168_NULL &&
              !diagnostic.available &&
              strcmp(diagnostic.reason, "NULL") == 0,
          "a missing MMIO bus is refused");
}

/*
 * ITEM-645: the runtime acknowledge the poll loop was missing.
 *
 * The 2026-11-22 Toshiba capture is the stopped shape: every RX descriptor is
 * owned by the NIC and `IntrStatus` carries `RxFIFOOver|RxOverflow`.  Linux
 * clears the status bits it read each time its handler runs and its poll path
 * clears `RxOverflow` once the ring has room; a poll-driven driver that only
 * clears them once at bring-up leaves the receiver stopped forever.  The
 * model clears exactly the written bits, so a plain store cannot pass this
 * proof.
 */
static void test_intr_events_ack(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    uint16_t observed;
    uint16_t acknowledged;

    fixture_init(&fixture, TEST_TX_CONFIG_8168H);
    io = fixture_io(&fixture);
    store16(fixture.registers, RTL8168_REG_INTR_STATUS, 0x00F5U);
    fixture.intr_status = 0x00F5U;

    observed = 0U;
    acknowledged = 0U;
    CHECK(rtl8168_intr_ack_events(&io, true, &observed, &acknowledged) ==
              RTL8168_OK,
          "the runtime acknowledge reads and clears observed events");
    CHECK(observed == 0x00F5U && acknowledged == 0x00F5U,
          "a drained ring clears every observed event, including the overflow pair");
    CHECK(fixture.intr_status == 0U &&
              load16(fixture.registers, RTL8168_REG_INTR_STATUS) == 0U,
          "write-one-to-clear leaves no observed status bit set");
    CHECK(fixture.intr_status_writes == 1U &&
              fixture.intr_status_ack == 0x00F5U,
          "the clear is one write carrying exactly the observed bits");

    CHECK(rtl8168_intr_ack_events(&io, true, &observed, &acknowledged) ==
              RTL8168_OK &&
              observed == 0U && acknowledged == 0U,
          "a quiet poll pass reports nothing");
    CHECK(fixture.intr_status_writes == 1U,
          "a quiet poll pass performs no MMIO write");

    /*
     * A pass that stopped on its frame budget has not proven the ring empty:
     * clearing the stop condition there only re-arms a receiver with nowhere
     * to write, so it stays latched until the next drained pass.
     */
    store16(fixture.registers, RTL8168_REG_INTR_STATUS, 0x00F5U);
    fixture.intr_status = 0x00F5U;
    CHECK(rtl8168_intr_ack_events(&io, false, &observed, &acknowledged) ==
              RTL8168_OK,
          "a pass that did not drain the ring still acknowledges the rest");
    CHECK(observed == 0x00F5U && acknowledged == 0x00A5U,
          "the overflow pair is not cleared while the ring is still full");
    CHECK(fixture.intr_status == 0x0050U,
          "RxFIFOOver and RxOverflow stay latched on a busy ring");

    CHECK(rtl8168_intr_ack_events(&io, true, &observed, &acknowledged) ==
              RTL8168_OK &&
              observed == 0x0050U && acknowledged == 0x0050U &&
              fixture.intr_status == 0U,
          "the next drained pass clears the latched overflow and frees the receiver");

    fixture.fail_read = true;
    observed = 0xFFFFU;
    acknowledged = 0xFFFFU;
    CHECK(rtl8168_intr_ack_events(&io, true, &observed, &acknowledged) ==
              RTL8168_IO_ERROR && observed == 0U && acknowledged == 0U,
          "a failed status read is an I/O error and acknowledges nothing");
    fixture.fail_read = false;

    store16(fixture.registers, RTL8168_REG_INTR_STATUS, 0x0010U);
    fixture.intr_status = 0x0010U;
    fixture.fail_write = true;
    CHECK(rtl8168_intr_ack_events(&io, true, &observed, &acknowledged) ==
              RTL8168_IO_ERROR && observed == 0x0010U &&
              acknowledged == 0U && fixture.intr_status == 0x0010U,
          "a failed clear is an I/O error and leaves the event latched");
    fixture.fail_write = false;

    CHECK(rtl8168_intr_ack_events(NULL, true, &observed, &acknowledged) ==
              RTL8168_NULL && observed == 0U && acknowledged == 0U,
          "a missing MMIO bus is refused");
    CHECK(rtl8168_intr_ack_events(&io, true, NULL, NULL) == RTL8168_OK,
          "the acknowledgement does not require the observation outputs");
}

/* ITEM-645: the ring makes progress after the stop condition is cleared. */
static void test_rx_progress_after_overflow_ack(void)
{
    struct mmio_fixture fixture;
    struct rtl8168_mmio_io io;
    struct rtl8168_descriptor descriptors[4];
    struct rtl8168_ring ring;
    uint16_t observed;
    uint16_t acknowledged;
    uint32_t frame_length = 0U;

    fixture_init(&fixture, TEST_TX_CONFIG_8168H);
    io = fixture_io(&fixture);
    memset(descriptors, 0, sizeof(descriptors));
    for (uint32_t index = 0U; index < 4U; ++index) {
        descriptors[index].opts1 = RTL8168_DESC_OWN;
    }
    rtl8168_ring_init(&ring, descriptors, 4U);
    store16(fixture.registers, RTL8168_REG_INTR_STATUS,
            RTL8168_INTR_RX_OVERFLOW);
    fixture.intr_status = RTL8168_INTR_RX_OVERFLOW;

    CHECK(rtl8168_rx_complete(&ring, RTL8168_RX_BUFFER_CAPACITY,
                              &frame_length) == RTL8168_RX_NOT_READY,
          "the stalled ring has no completed descriptor");
    CHECK(rtl8168_intr_ack_events(&io, true, &observed, &acknowledged) ==
              RTL8168_OK && observed == RTL8168_INTR_RX_OVERFLOW &&
              acknowledged == RTL8168_INTR_RX_OVERFLOW,
          "draining the stalled shape clears the stop condition");

    descriptors[0].opts1 = RTL8168_DESC_FIRST_FRAGMENT |
                           RTL8168_DESC_LAST_FRAGMENT | (64U + 4U);
    CHECK(rtl8168_rx_complete(&ring, RTL8168_RX_BUFFER_CAPACITY,
                              &frame_length) == RTL8168_OK &&
              frame_length == 64U,
          "a descriptor completes after the overflow is cleared");
    CHECK(rtl8168_rx_repost(&ring, 0x2000U,
                            RTL8168_RX_BUFFER_CAPACITY) == RTL8168_OK &&
              ring.index == 1U &&
              (descriptors[0].opts1 & RTL8168_DESC_OWN) != 0U,
          "the consumed descriptor is handed back and the cursor advances");
}

int main(void)
{
    test_pci_identity();
    test_memory_bar();
    test_chip_identify();
    test_chip_identity_record();
    test_mac_read();
    test_mac_validation();
    test_reset();
    test_interrupts_masked();
    test_bring_up();
    test_link_status();
    test_phy_mii();
    test_phy_mii_xid_2c9();
    test_phyar_settle_gap();
    test_phyar_bounded_wait();
    test_phy_advertise_readback();
    test_phyar_separate_completion_and_payload();
    test_phyar_payload_read_failure_keeps_poll_evidence();
    test_evl_phy_reference_sequence();
    test_evl_phy_zero_page_readback();
    test_evl_phy_intermediate_failures();
    test_evl_phy_reset_timeout_and_page_failure();
    test_evl_phy_other_profiles_unchanged();
    test_evl_aneg_command_failure();
    test_evl_phy_control_refusals();
    test_cold_boot_evl_sequence();
    test_cold_boot_evl_rejects_a_missing_stage();
    test_cold_boot_evl_refusals();
    test_cold_boot_evl_recovers_the_phy_page();
    test_cold_boot_evl_keeps_the_first_error();
    test_cold_boot_evl_samples_the_bmsr();
    test_phy_sample_survives_later_read_failure();
    test_generation_bounds();
    test_rx_armed();
    test_rxdv_gate();
    test_region_layout();
    test_rx_ring();
    test_tx_ring();
    test_tx_counters_and_rewind();
    test_dma_window();
    test_rx_diagnostic();
    test_intr_events_ack();
    test_rx_progress_after_overflow_ack();

    if (failures != 0) {
        fprintf(stderr, "RTL8168 host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("RTL8168 host tests: ALL OK");
    return 0;
}

#endif
