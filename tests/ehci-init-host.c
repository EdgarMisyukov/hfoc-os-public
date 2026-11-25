/*
 * ITEM-561 host proof: the controller initialization sequence over a scripted
 * register file and clock.
 *
 * The runs before this one could not be explained because the driver's first
 * operational accesses happened before CAPLENGTH was read: with caplength zero
 * they landed in the capability space, the reset was never issued, the
 * firmware's periodic schedule stayed on and the asynchronous schedule was
 * started without a reset.  The fake below keeps the two spaces apart and
 * counts any access below the capability length, so the regression cannot
 * come back silently.
 *
 * Scenarios: a PSS that clears a few frames after PSE was written, a PSS that
 * never clears (the guard stays, and the start is never issued), PSE = 0 with
 * PSS = 1 (the two bits are different things), PSE = 1 with PSS = 0, a reset
 * bit that never clears, a controller that never reports HCHalted, a start
 * whose halt bit never clears, an asynchronous schedule that never reports,
 * a CONFIGFLAG that never sticks, and a valid start that keeps the reported
 * interrupt threshold.
 */
#include "ehci_init.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } \
} while (0)

#define CAP_LENGTH 0x20U
#define OP_BASE CAP_LENGTH
#define REG_USBCMD (OP_BASE + EHCI_OP_USBCMD)
#define REG_USBSTS (OP_BASE + EHCI_OP_USBSTS)
#define REG_CTRLDSSEGMENT (OP_BASE + EHCI_OP_CTRLDSSEGMENT)
#define REG_PERIODICLISTBASE (OP_BASE + EHCI_OP_PERIODICLISTBASE)
#define REG_ASYNCLISTADDR (OP_BASE + EHCI_OP_ASYNCLISTADDR)
#define REG_CONFIGFLAG (OP_BASE + EHCI_OP_CONFIGFLAG)
#define REG_PORTSC (OP_BASE + EHCI_OP_PORTSC)

struct fake {
    uint32_t usbcmd;
    uint32_t usbsts;
    uint32_t mmio[128];
    uint32_t caplength;
    uint32_t hcsparams;
    uint32_t hccparams;
    uint64_t ticks;
    /* Scripted controller behaviour. */
    bool halt_never;
    bool reset_never;
    int reset_ticks;
    uint32_t usbcmd_after_reset;
    uint32_t usbsts_after_reset;
    int pss_ticks;      /* < 0: never clears */
    int ass_ticks;      /* < 0: never reports */
    bool start_keeps_halt;
    bool configflag_never;
    /* Bookkeeping. */
    uint32_t cap_writes;
    uint32_t op_accesses;
    uint32_t pauses;
    bool rs_written;
    bool stop_word_written;
    uint32_t stop_word;
    int pss_clear_at;
    int reset_clear_at;
    int ass_set_at;
};

static int fake_index(uint32_t offset) {
    if (offset == REG_CONFIGFLAG) return 0;
    if (offset == REG_PERIODICLISTBASE) return 1;
    if (offset == REG_CTRLDSSEGMENT) return 2;
    if (offset == REG_ASYNCLISTADDR) return 3;
    return -1;
}

static uint32_t fake_read(void *context, uint32_t offset) {
    struct fake *fake = context;
    const int index = fake_index(offset);

    if (offset < fake->caplength) {
        if (offset == 0U) {
            return fake->caplength | (0x0100U << 16U);
        }
        if (offset == 4U) {
            return fake->hcsparams;
        }
        if (offset == 8U) {
            return fake->hccparams;
        }
        return 0U;
    }
    ++fake->op_accesses;
    if (offset == REG_USBCMD) return fake->usbcmd;
    if (offset == REG_USBSTS) return fake->usbsts;
    if (offset == REG_PORTSC) return 0x00001005U;
    if (index >= 0) return fake->mmio[index];
    return 0U;
}

static void fake_write(void *context, uint32_t offset, uint32_t value) {
    struct fake *fake = context;

    if (offset < fake->caplength) {
        ++fake->cap_writes;
        return;
    }
    ++fake->op_accesses;
    if (offset == REG_USBCMD) {
        fake->usbcmd = value;
        if (value == 0U) {
            if (!fake->halt_never) {
                fake->usbsts |= EHCI_USBSTS_HCH;
            }
            return;
        }
        if ((value & EHCI_USBCMD_HCRESET) != 0U) {
            fake->usbcmd = EHCI_USBCMD_HCRESET;
            if (!fake->reset_never) {
                fake->reset_clear_at = (int)fake->ticks + fake->reset_ticks;
            }
            return;
        }
        if ((value & EHCI_USBCMD_PSE) == 0U &&
            (value & (EHCI_USBCMD_RS | EHCI_USBCMD_ASE)) == 0U) {
            fake->stop_word_written = true;
            fake->stop_word = value;
            if (fake->pss_ticks >= 0) {
                fake->pss_clear_at = (int)fake->ticks + fake->pss_ticks;
            }
            return;
        }
        if ((value & EHCI_USBCMD_RS) != 0U) {
            fake->rs_written = true;
            if (fake->start_keeps_halt) {
                fake->usbsts |= EHCI_USBSTS_HCH;
            } else {
                fake->usbsts &= ~EHCI_USBSTS_HCH;
                if (fake->ass_ticks >= 0) {
                    fake->ass_set_at = (int)fake->ticks + fake->ass_ticks;
                }
            }
        }
        return;
    }
    if (offset == REG_USBSTS) {
        fake->usbsts &= ~value;
        return;
    }
    if (offset == REG_CONFIGFLAG) {
        if (!fake->configflag_never) {
            fake->mmio[0] = value;
        }
        return;
    }
    {
        const int index = fake_index(offset);

        if (index >= 0) {
            fake->mmio[index] = value;
        }
    }
}

static void fake_barrier(void *context) {
    (void)context;
}

static uint64_t fake_deadline(void *context, uint32_t microseconds) {
    struct fake *fake = context;

    return fake->ticks + microseconds;
}

static bool fake_expired(void *context, uint64_t deadline) {
    struct fake *fake = context;

    return fake->ticks >= deadline;
}

static void fake_pause(void *context) {
    struct fake *fake = context;

    ++fake->ticks;
    ++fake->pauses;
    if (fake->reset_clear_at >= 0 && (int)fake->ticks >= fake->reset_clear_at) {
        fake->usbcmd = fake->usbcmd_after_reset;
        fake->usbsts = fake->usbsts_after_reset;
        fake->reset_clear_at = -1;
    }
    if (fake->pss_clear_at >= 0 && (int)fake->ticks >= fake->pss_clear_at) {
        fake->usbsts &= ~EHCI_USBSTS_PSS;
    }
    if (fake->ass_set_at >= 0 && (int)fake->ticks >= fake->ass_set_at) {
        fake->usbsts |= EHCI_USBSTS_ASS;
    }
}

static struct fake machine(void) {
    struct fake fake;

    memset(&fake, 0, sizeof(fake));
    fake.caplength = CAP_LENGTH;
    fake.hcsparams = 2U; /* two root ports */
    fake.hccparams = 0U; /* 32-bit addressing; the driver must not rely on it */
    fake.reset_ticks = 2;
    fake.pss_ticks = 2;
    fake.ass_ticks = 1;
    fake.reset_clear_at = -1;
    fake.pss_clear_at = -1;
    fake.ass_set_at = -1;
    fake.usbcmd = 0x00080010U; /* firmware left the periodic schedule enabled */
    fake.usbsts = 0U;
    return fake;
}

static struct ehci_init_ops fake_ops(struct fake *fake) {
    struct ehci_init_ops ops;

    memset(&ops, 0, sizeof(ops));
    ops.context = fake;
    ops.read = fake_read;
    ops.write = fake_write;
    ops.barrier = fake_barrier;
    ops.deadline = fake_deadline;
    ops.expired = fake_expired;
    ops.pause = fake_pause;
    return ops;
}

static struct ehci_init_result run(struct fake *fake) {
    const struct ehci_init_ops ops = fake_ops(fake);
    struct ehci_init_result result;

    ehci_init_run(&ops, 4096U, false, 0x00200000U, 0x00100000U, 8U, 8U, 8U,
                  &result);
    return result;
}

/* A start that clears the delayed PSS and reports both schedules. */
static void test_delayed_pss_clears(void) {
    struct fake fake = machine();
    struct ehci_init_result result;

    fake.usbcmd_after_reset = EHCI_USBCMD_PSE | 0x00080000U;
    fake.usbsts_after_reset = EHCI_USBSTS_PSS;
    result = run(&fake);

    CHECK(result.cause == EHCI_INIT_OK,
          "the start succeeds once the delayed PSS has cleared");
    CHECK(result.pse_was_set && result.pss_was_set,
          "PSE and PSS were both set after the reset and both are recorded");
    CHECK(result.pss_stopped && fake.stop_word_written,
          "the periodic schedule was stopped with a written command word");
    CHECK((result.pse_clear_word & EHCI_USBCMD_PSE) == 0U &&
              (result.pse_clear_word & EHCI_USBCMD_RS) == 0U,
          "the stop word clears the periodic enable while halted");
    CHECK(fake.ticks > 0U && fake.pauses > 0U,
          "the wait for PSS consumed frames instead of an instant check");
    CHECK(result.periodic_off && result.async_on,
          "the controller reports the periodic schedule off and the async one on");
    CHECK((result.start_word & EHCI_USBCMD_PSE) == 0U &&
              (result.start_word & EHCI_USBCMD_RS) != 0U,
          "the start word runs without the periodic schedule");
    CHECK(fake.cap_writes == 0U,
          "no operational access landed in the capability space");
}

/* A PSS that never clears: the guard stays, and the schedule is never started. */
static void test_pss_never_clears_refuses(void) {
    struct fake fake = machine();
    struct ehci_init_result result;

    fake.usbcmd_after_reset = EHCI_USBCMD_PSE;
    fake.usbsts_after_reset = EHCI_USBSTS_PSS;
    fake.pss_ticks = -1;
    result = run(&fake);

    CHECK(result.cause == EHCI_INIT_PERIODIC_STOP_TIMEOUT,
          "a PSS that survives the budget is refused by name");
    CHECK(!fake.rs_written,
          "the asynchronous schedule is not started while the periodic one runs");
    CHECK(fake.ticks >= 8U,
          "the refusal used the whole stop budget, not an instant check");
}

/* PSE = 0 and PSS = 1: two different bits, and the status one is waited out. */
static void test_pss_without_pse(void) {
    struct fake fake = machine();
    struct ehci_init_result result;

    fake.usbcmd_after_reset = 0x00080000U; /* PSE clear */
    fake.usbsts_after_reset = EHCI_USBSTS_PSS;
    fake.pss_ticks = 2;
    result = run(&fake);

    CHECK(result.cause == EHCI_INIT_OK,
          "PSS alone is waited out, not treated as PSE");
    CHECK(!result.pse_was_set && result.pss_was_set,
          "the two bits are recorded separately");
    CHECK(fake.stop_word_written,
          "the stop word is still written because PSS was set");
}

/* PSE = 1 with PSS = 0: the command is cleared and there is nothing to wait. */
static void test_pse_without_pss(void) {
    struct fake fake = machine();
    struct ehci_init_result result;

    fake.usbcmd_after_reset = EHCI_USBCMD_PSE;
    fake.usbsts_after_reset = 0U;
    fake.pss_ticks = -1; /* would hang if the code waited anyway */
    result = run(&fake);

    CHECK(result.cause == EHCI_INIT_OK,
          "PSE can be cleared while PSS already reads zero");
    CHECK(result.pse_was_set && !result.pss_was_set && result.pss_stopped,
          "the bits and the outcome are recorded");
}

static void test_reset_timeout(void) {
    struct fake fake = machine();
    struct ehci_init_result result;

    fake.reset_never = true;
    result = run(&fake);

    CHECK(result.cause == EHCI_INIT_RESET_TIMEOUT,
          "a reset bit that never clears is named RESET_TIMEOUT");
    CHECK(!fake.rs_written, "the schedule is not started after a failed reset");
}

static void test_halt_failure(void) {
    struct fake fake = machine();
    struct ehci_init_result result;

    fake.halt_never = true;
    result = run(&fake);

    CHECK(result.cause == EHCI_INIT_HALT_FAILED,
          "a controller that never reports HCHalted is refused before the reset");
}

static void test_start_timeouts(void) {
    struct fake fake = machine();
    struct ehci_init_result result;

    fake.start_keeps_halt = true;
    result = run(&fake);
    CHECK(result.cause == EHCI_INIT_HCH_TIMEOUT,
          "a start that never clears HCHalted is named HCH");

    fake = machine();
    fake.ass_ticks = -1;
    result = run(&fake);
    CHECK(result.cause == EHCI_INIT_ASS_TIMEOUT,
          "an asynchronous schedule that never reports is named ASS");
}

static void test_configflag_timeout(void) {
    struct fake fake = machine();
    struct ehci_init_result result;

    fake.configflag_never = true;
    result = run(&fake);

    CHECK(result.cause == EHCI_INIT_CONFIGFLAG_TIMEOUT,
          "a CONFIGFLAG that never sticks is named");
}

static void test_invalid_caplength(void) {
    struct fake fake = machine();
    struct ehci_init_result result;

    fake.caplength = 0x10U;
    result = run(&fake);

    CHECK(result.cause == EHCI_INIT_CAPLENGTH,
          "an impossible CAPLENGTH is refused before any operational access");
    CHECK(fake.op_accesses == 0U,
          "nothing touched the operational space without a valid offset");
}

static void test_start_keeps_threshold(void) {
    struct fake fake = machine();
    struct ehci_init_result result;

    fake.usbcmd_after_reset = 0x00080000U; /* ITC = 8, no PSE */
    result = run(&fake);

    CHECK(result.cause == EHCI_INIT_OK,
          "a clean controller starts");
    CHECK((result.start_word & EHCI_USBCMD_ITC_MASK) == 0x00080000U,
          "the start word keeps the interrupt threshold the controller reports");
    CHECK(result.periodiclistbase == 0x00200000U,
          "the periodic list base is the frame list the driver built, not "
          "zero");
    CHECK(result.ctrldssegment_written == false,
          "a 32-bit controller is not given a segment register");
    CHECK(result.asynclistaddr == 0x00100000U,
          "the asynchronous list address is read back as written");
    CHECK(fake.cap_writes == 0U,
          "no write crossed into the capability space");
}

int main(void) {
    test_delayed_pss_clears();
    test_pss_never_clears_refuses();
    test_pss_without_pse();
    test_pse_without_pss();
    test_reset_timeout();
    test_halt_failure();
    test_start_timeouts();
    test_configflag_timeout();
    test_invalid_caplength();
    test_start_keeps_threshold();

    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    printf("EHCI init host test: ALL OK\n");
    return 0;
}
