/*
 * ITEM-567/568 host proof: the root port reset, driven through the production
 * MMIO adapter, with the controller's own words kept at every step.
 *
 * The Toshiba run of build 52012c48 printed `reset=PR_LATCHED rb=0x1005
 * rs=0x1100 rh=0x1101 rc=0x1000 rb2=0x1101 ra=0x1101 wait_us=100000 rec=1` and,
 * in the start record, `hse_usbcmd=0x00080010 hse_usbsts=0x00007010`: the
 * controller had halted with HSE, so the port reset could not complete.  This
 * proof therefore requires more than the words of the reset:
 *
 *   * every write of the sequence goes to this port's PORTSC and nowhere else -
 *     not to USBCMD (0x20), USBSTS (0x24), CONFIGFLAG (0x40) or the capability
 *     space below CAPLENGTH;
 *   * USBCMD and USBSTS are read at each point (before, after the set, at the
 *     end of the hold, after the clear, at the 2 ms bound, in the poll, at the
 *     end) and the words are kept;
 *   * a controller that has halted (HSE or HCH) is refused with its own
 *     verdict, the point where HSE first appeared is kept, and no PR write is
 *     attempted on it - Table 2-16 says a halted controller may hold PR;
 *   * PR is cleared a second time only while the controller is running.
 *
 * The fake controller answers only through ehci_reg_read()/ehci_reg_write() -
 * the production accessor, a 32-bit store - so an address that ignores
 * CAPLENGTH cannot pass.
 */
#include "ehci.h"
#include "ehci_port.h"
#include "ehci_regs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } \
} while (0)

#define FAKE_CAPLENGTH 0x20U
#define FAKE_BAR_BYTES 512U
#define FAKE_USBCMD_OFFSET (FAKE_CAPLENGTH + EHCI_OP_USBCMD)
#define FAKE_USBSTS_OFFSET (FAKE_CAPLENGTH + EHCI_OP_USBSTS)

struct fake_controller {
    uint8_t bar[FAKE_BAR_BYTES] __attribute__((aligned(8)));
    uint32_t portsc_offset;
    /* Completion model: PE appears on this read after PR was dropped. */
    uint32_t enable_after_reads;
    bool enable_never;
    uint32_t pe_reads;
    bool pe_revealed;
    /* A controller that ignores the clear keeps PR set. */
    bool latch_pr;
    /* Powering the port reveals a device behind it. */
    bool power_reveals_ccs;
    uint32_t clear_writes;
    uint32_t set_writes;
    uint32_t power_writes;
    uint32_t portsc_writes;
    uint32_t portsc_reads;
    uint32_t foreign_writes;
    uint32_t last_write_offset;
    /* Halting: at this PORTSC read (1-based) the model sets HCH (and HSE unless
       halt_without_hse) and clears RS.  0 = never. */
    uint32_t halt_at_read;
    bool halt_without_hse;
    bool halted;
    uint32_t delayed_us;
    uint32_t first_delay_us;
};

static void model_store(struct fake_controller *fake, uint32_t offset,
                        uint32_t value) {
    ehci_reg_write(fake->bar, FAKE_CAPLENGTH, offset, value);
}

static uint32_t model_load(const struct fake_controller *fake,
                           uint32_t offset) {
    return ehci_reg_read(fake->bar, FAKE_CAPLENGTH, offset);
}

static void model_halt(struct fake_controller *fake) {
    uint32_t usbcmd = model_load(fake, FAKE_USBCMD_OFFSET);
    uint32_t usbsts = model_load(fake, FAKE_USBSTS_OFFSET);

    fake->halted = true;
    model_store(fake, FAKE_USBCMD_OFFSET, usbcmd & ~EHCI_USBCMD_RS);
    if (!fake->halt_without_hse) {
        usbsts |= EHCI_USBSTS_HSE;
    }
    usbsts |= EHCI_USBSTS_HCH;
    model_store(fake, FAKE_USBSTS_OFFSET, usbsts);
}

/* The module's read: through the production accessor, then the model's own
   completion and halt rules. */
static uint32_t fake_mmio_read(void *context, uint32_t offset) {
    struct fake_controller *fake = context;
    uint32_t word = model_load(fake, offset);

    if (offset == fake->portsc_offset) {
        const bool reset_dropped =
            (word & EHCI_PORTSC_PR) == 0U && fake->clear_writes != 0U;

        fake->portsc_reads += 1U;
        if (fake->halt_at_read == fake->portsc_reads) {
            model_halt(fake);
            word = model_load(fake, offset);
        }
        if (reset_dropped && !fake->pe_revealed && !fake->enable_never &&
            !fake->halted) {
            fake->pe_reads += 1U;
            if (fake->pe_reads >= fake->enable_after_reads) {
                word |= EHCI_PORTSC_PE;
                model_store(fake, offset, word);
                fake->pe_revealed = true;
            }
        }
    }
    return word;
}

/* The module's write: through the production accessor, with the controller's
   side of the write-1-to-clear and PR semantics applied first. */
static void fake_mmio_write(void *context, uint32_t offset, uint32_t value) {
    struct fake_controller *fake = context;

    fake->last_write_offset = offset;
    if (offset != fake->portsc_offset) {
        fake->foreign_writes += 1U;
        model_store(fake, offset, value);
        return;
    }
    fake->portsc_writes += 1U;
    {
        uint32_t word = model_load(fake, offset);

        /* Change bits are write-1-to-clear; the write value never sets them. */
        word &= ~EHCI_PORTSC_CHANGE;
        if ((value & EHCI_PORTSC_PP) != 0U) {
            if ((word & EHCI_PORTSC_PP) == 0U) {
                fake->power_writes += 1U;
                if (fake->power_reveals_ccs) {
                    word |= EHCI_PORTSC_CCS;
                }
            }
            word |= EHCI_PORTSC_PP;
        }
        if ((value & EHCI_PORTSC_PR) != 0U) {
            word |= EHCI_PORTSC_PR;
            fake->set_writes += 1U;
        } else {
            /* The clear: a controller that latched PR ignores it. */
            fake->clear_writes += 1U;
            if (fake->latch_pr) {
                word |= EHCI_PORTSC_PR;
            } else {
                word &= ~EHCI_PORTSC_PR;
                fake->pe_reads = 0U;
                fake->pe_revealed = false;
            }
        }
        model_store(fake, offset, word);
    }
}

static void fake_mmio_delay(void *context, uint32_t microseconds) {
    struct fake_controller *fake = context;

    if (fake->first_delay_us == 0U) {
        fake->first_delay_us = microseconds;
    }
    fake->delayed_us += microseconds;
}

static struct ehci_port_ops fake_ops(struct fake_controller *fake) {
    struct ehci_port_ops ops;

    ops.context = fake;
    ops.read = fake_mmio_read;
    ops.write = fake_mmio_write;
    ops.delay_us = fake_mmio_delay;
    return ops;
}

static void fake_reset(struct fake_controller *fake, uint32_t port,
                       uint32_t portsc) {
    memset(fake, 0, sizeof(*fake));
    fake->portsc_offset = ehci_port_offset(FAKE_CAPLENGTH, port);
    fake->enable_after_reads = 1U;
    model_store(fake, fake->portsc_offset, portsc);
    /* A running controller: RS and ASE set, nothing in USBSTS. */
    model_store(fake, FAKE_USBCMD_OFFSET, EHCI_USBCMD_RS | EHCI_USBCMD_ASE);
    model_store(fake, FAKE_USBSTS_OFFSET, 0U);
}

/* (a) and (b): the set word carries PR with the port's power, and the reset is
   held for the specification's 50 ms.  (c): the clear is an explicit write with
   PR zero.  (d): the port's completion is what is awaited, and the controller's
   words are kept at every point.  (e): every write goes to PORTSC. */
static void test_the_sequence_and_its_writes(void) {
    struct fake_controller fake;
    struct ehci_port_reset_state state;
    struct ehci_port_ops ops;

    fake_reset(&fake, 1U, EHCI_PORTSC_PP | EHCI_PORTSC_CCS | EHCI_PORTSC_CSC);
    ops = fake_ops(&fake);

    CHECK(ehci_port_reset_run(&ops, FAKE_CAPLENGTH, 1U, false, &state) ==
              EHCI_PORT_RESET_OK,
          "a port that enables is a reset OK");
    CHECK(state.portsc_before ==
              (EHCI_PORTSC_PP | EHCI_PORTSC_CCS | EHCI_PORTSC_CSC),
          "the pre-reset word is the port's own");
    CHECK(state.portsc_set ==
              (EHCI_PORTSC_PP | EHCI_PORTSC_CSC | EHCI_PORTSC_PR),
          "the set word is PP|CSC|PR: power and change bits travel with the "
          "reset, and PE is written zero");
    CHECK(state.portsc_hold ==
              (EHCI_PORTSC_PP | EHCI_PORTSC_CCS | EHCI_PORTSC_PR),
          "the controller does not self-clear PR: the hold word still has it");
    CHECK(state.portsc_cleared == EHCI_PORTSC_PP,
          "the clear is an explicit write with PR zero");
    CHECK(state.portsc_cleared_readback ==
              (EHCI_PORTSC_PP | EHCI_PORTSC_CCS | EHCI_PORTSC_PE),
          "the 2 ms sample is the word read at the completion bound");
    CHECK(state.portsc_after ==
              (EHCI_PORTSC_PP | EHCI_PORTSC_CCS | EHCI_PORTSC_PE),
          "the final word is the enabled port");
    CHECK(state.waited_us == EHCI_PORT_RESET_SETTLE_US,
          "a completion visible at the 2 ms bound uses only that bound of the "
          "window");
    CHECK(!state.powered && !state.cleared_twice,
          "a powered port needs no power write and no second clear");

    /* (2) every write of the reset is this port's PORTSC, and nothing else. */
    CHECK(fake.foreign_writes == 0U,
          "no reset write landed outside this port's PORTSC");
    CHECK(fake.last_write_offset == fake.portsc_offset,
          "the last reset write is at CAPLENGTH + PORTSC + (port-1)*4");
    CHECK(fake.portsc_writes == 2U,
          "the reset writes exactly twice: the set and the clear");
    CHECK(fake.portsc_offset == FAKE_CAPLENGTH + 0x44U,
          "port 1's PORTSC is BAR+CAPLENGTH+0x44, not BAR+0x44 and not USBCMD");
    CHECK(fake.portsc_offset != FAKE_USBCMD_OFFSET &&
              fake.portsc_offset != FAKE_USBSTS_OFFSET &&
              fake.portsc_offset != (FAKE_CAPLENGTH + EHCI_OP_CONFIGFLAG),
          "the reset address is neither USBCMD, USBSTS nor CONFIGFLAG");

    /* (3) the controller's words at every point are kept. */
    CHECK(state.usbcmd[EHCI_PORT_RESET_POINT_BEFORE] ==
              (EHCI_USBCMD_RS | EHCI_USBCMD_ASE),
          "the command word before the reset is kept");
    CHECK(state.usbsts[EHCI_PORT_RESET_POINT_BEFORE] == 0U,
          "the status word before the reset is kept");
    CHECK(state.usbcmd[EHCI_PORT_RESET_POINT_AFTER] ==
              (EHCI_USBCMD_RS | EHCI_USBCMD_ASE),
          "the command word after the reset is kept");
    CHECK(state.hse_point == EHCI_PORT_RESET_POINT_NONE,
          "a running controller has no HSE point");
}

/* The completion is not instantaneous: PE appears several polls after PR is
   dropped, which the old single, immediate check could never see. */
static void test_a_delayed_completion_is_awaited(void) {
    struct fake_controller fake;
    struct ehci_port_reset_state state;
    struct ehci_port_ops ops;

    fake_reset(&fake, 1U, EHCI_PORTSC_PP | EHCI_PORTSC_CCS);
    fake.enable_after_reads = 8U;
    ops = fake_ops(&fake);

    CHECK(ehci_port_reset_run(&ops, FAKE_CAPLENGTH, 1U, false, &state) ==
              EHCI_PORT_RESET_OK,
          "a port that enables eight polls after the clear is a reset OK");
    CHECK(state.waited_us ==
              EHCI_PORT_RESET_SETTLE_US + 7U * EHCI_PORT_RESET_STEP_US,
          "the completion window records the steps after the 2 ms sample");
    CHECK(fake.clear_writes == 1U,
          "a controller that clears PR on the first write is not cleared "
          "twice");
}

/* A port that never enables is a port verdict, not a controller one. */
static void test_a_port_that_never_enables_is_named(void) {
    struct fake_controller fake;
    struct ehci_port_reset_state state;
    struct ehci_port_ops ops;

    fake_reset(&fake, 1U, EHCI_PORTSC_PP | EHCI_PORTSC_CCS);
    fake.enable_never = true;
    ops = fake_ops(&fake);

    CHECK(ehci_port_reset_run(&ops, FAKE_CAPLENGTH, 1U, false, &state) ==
              EHCI_PORT_RESET_NOT_ENABLED,
          "a port the controller never enables is NOT_ENABLED");
    CHECK((state.portsc_cleared_readback & EHCI_PORTSC_PR) == 0U,
          "PR did go to zero, which is why it cannot be the completion "
          "witness");
    CHECK(state.waited_us == EHCI_PORT_RESET_WINDOW_US,
          "the whole completion window was spent before refusing");
    CHECK(fake.clear_writes == 1U && !state.cleared_twice,
          "PR that clears on the first write is not cleared again");
}

/* A running controller that keeps PR set gets exactly one second clear. */
static void test_a_running_controller_latching_pr_is_cleared_once_more(void) {
    struct fake_controller fake;
    struct ehci_port_reset_state state;
    struct ehci_port_ops ops;

    fake_reset(&fake, 1U, EHCI_PORTSC_PP | EHCI_PORTSC_CCS);
    fake.latch_pr = true;
    ops = fake_ops(&fake);

    CHECK(ehci_port_reset_run(&ops, FAKE_CAPLENGTH, 1U, false, &state) ==
              EHCI_PORT_RESET_PR_LATCHED,
          "a running controller that keeps PR set is PR_LATCHED");
    CHECK(fake.clear_writes == 2U && state.cleared_twice,
          "the latched reset is cleared exactly once more");
    CHECK(state.waited_us == EHCI_PORT_RESET_WINDOW_US,
          "the latched case spends the whole window before refusing");
    CHECK(state.hse_point == EHCI_PORT_RESET_POINT_NONE,
          "a running controller reports no HSE");
}

/* The Toshiba's state: the controller had already halted with HSE.  The reset
   must not write PR into it (Table 2-16: a halted controller may hold PR), and
   the point that carried HSE is what the record keeps. */
static void test_a_halted_controller_is_refused_before_any_write(void) {
    struct fake_controller fake;
    struct ehci_port_reset_state state;
    struct ehci_port_ops ops;

    fake_reset(&fake, 1U, EHCI_PORTSC_PP | EHCI_PORTSC_CCS);
    model_halt(&fake);
    ops = fake_ops(&fake);

    CHECK(ehci_port_reset_run(&ops, FAKE_CAPLENGTH, 1U, false, &state) ==
              EHCI_PORT_RESET_CONTROLLER_HALTED,
          "a halted controller is refused with its own verdict");
    CHECK(fake.portsc_writes == 0U,
          "no PR write is attempted on a halted controller");
    CHECK(state.hse_point == EHCI_PORT_RESET_POINT_BEFORE &&
              (state.hse_usbsts & EHCI_USBSTS_HSE) != 0U &&
              (state.hse_usbsts & EHCI_USBSTS_HCH) != 0U &&
              (state.hse_usbcmd & EHCI_USBCMD_RS) == 0U,
          "the first HSE sample and the words that carried it are kept");
    CHECK(state.usbsts[EHCI_PORT_RESET_POINT_BEFORE] ==
              (EHCI_USBSTS_HSE | EHCI_USBSTS_HCH),
          "the status word at the point is kept");
}

/* HSE can appear while the sequence is running: the point it appears at is the
   item's open question, so the proof requires the sequence to stop there and
   to keep that point. */
static void test_hse_during_the_hold_stops_the_sequence(void) {
    struct fake_controller fake;
    struct ehci_port_reset_state state;
    struct ehci_port_ops ops;

    fake_reset(&fake, 1U, EHCI_PORTSC_PP | EHCI_PORTSC_CCS);
    fake.halt_at_read = 2U; /* the hold's read */
    ops = fake_ops(&fake);

    CHECK(ehci_port_reset_run(&ops, FAKE_CAPLENGTH, 1U, false, &state) ==
              EHCI_PORT_RESET_CONTROLLER_HALTED,
          "HSE during the hold is a controller verdict");
    CHECK(state.hse_point == EHCI_PORT_RESET_POINT_HOLD,
          "the point that first carried HSE is recorded");
    CHECK(fake.portsc_writes == 1U,
          "the sequence stops after the set write and never clears a halted "
          "controller");
    CHECK(state.portsc_hold ==
              (EHCI_PORTSC_PP | EHCI_PORTSC_CCS | EHCI_PORTSC_PR),
          "the word read at the hold is still kept");
}

/* A controller that halts without HSE is a halt too, and PR stays latched. */
static void test_a_halt_without_hse_is_not_cleared_twice(void) {
    struct fake_controller fake;
    struct ehci_port_reset_state state;
    struct ehci_port_ops ops;

    fake_reset(&fake, 1U, EHCI_PORTSC_PP | EHCI_PORTSC_CCS);
    fake.latch_pr = true;
    fake.halt_without_hse = true;
    fake.halt_at_read = 3U; /* the 2 ms sample */
    ops = fake_ops(&fake);

    CHECK(ehci_port_reset_run(&ops, FAKE_CAPLENGTH, 1U, false, &state) ==
              EHCI_PORT_RESET_CONTROLLER_HALTED,
          "a halt without HSE is a controller verdict");
    CHECK(state.hse_point == EHCI_PORT_RESET_POINT_NONE,
          "no HSE point is invented for a halt that did not report it");
    CHECK(fake.clear_writes == 1U && !state.cleared_twice,
          "a halted controller is not asked a second time");
}

/* Software port power: a port that is off is powered and given its power-good
   interval before the reset, and the set word carries PP. */
static void test_a_powered_port_is_powered_before_its_reset(void) {
    struct fake_controller fake;
    struct ehci_port_reset_state state;
    struct ehci_port_ops ops;

    fake_reset(&fake, 1U, 0U);
    fake.power_reveals_ccs = true;
    ops = fake_ops(&fake);

    CHECK(ehci_port_reset_run(&ops, FAKE_CAPLENGTH, 1U, true, &state) ==
              EHCI_PORT_RESET_OK,
          "a port that only shows its device after power is reset after it");
    CHECK(state.powered && fake.power_writes == 1U,
          "the power write happened exactly once");
    CHECK(fake.first_delay_us == EHCI_PORT_POWER_GOOD_US,
          "power good is waited for, not assumed");
    CHECK(state.portsc_set == (EHCI_PORTSC_PP | EHCI_PORTSC_PR),
          "the set word keeps the port powered");
    CHECK((state.portsc_before & EHCI_PORTSC_CCS) != 0U,
          "the reset starts from the word read after power good");
}

/* Nothing on the port: no reset is a request to an empty port. */
static void test_an_empty_port_is_not_reset(void) {
    struct fake_controller fake;
    struct ehci_port_reset_state state;
    struct ehci_port_ops ops;

    fake_reset(&fake, 1U, EHCI_PORTSC_PP);
    ops = fake_ops(&fake);

    CHECK(ehci_port_reset_run(&ops, FAKE_CAPLENGTH, 1U, false, &state) ==
              EHCI_PORT_RESET_NOT_CONNECTED,
          "an empty port is refused before any write");
    CHECK(fake.portsc_writes == 0U,
          "no reset is written to a port with nothing on it");
    CHECK(state.usbcmd[EHCI_PORT_RESET_POINT_BEFORE] ==
              (EHCI_USBCMD_RS | EHCI_USBCMD_ASE),
          "the controller's state is still sampled on an empty port");
}

/*
 * The port's PORTSC is addressed through the controller's own CAPLENGTH: the
 * same word placed where CAPLENGTH was forgotten (BAR + 0x44) must not be what
 * the reset reads.
 */
static void test_the_reset_reads_through_the_controllers_caplength(void) {
    struct fake_controller fake;
    struct ehci_port_reset_state state;
    struct ehci_port_ops ops;

    fake_reset(&fake, 1U, EHCI_PORTSC_PP | EHCI_PORTSC_CCS);
    /* What a reader that dropped CAPLENGTH would fetch: a connected port. */
    model_store(&fake, 0x44U, EHCI_PORTSC_PP | EHCI_PORTSC_CCS);
    model_store(&fake, ehci_port_offset(FAKE_CAPLENGTH, 1U), 0U);
    ops = fake_ops(&fake);

    CHECK(ehci_port_reset_run(&ops, FAKE_CAPLENGTH, 1U, false, &state) ==
              EHCI_PORT_RESET_NOT_CONNECTED,
          "the reset reads the port at CAPLENGTH + PORTSC, not at BAR + "
          "PORTSC");
}

int main(void) {
    test_the_sequence_and_its_writes();
    test_a_delayed_completion_is_awaited();
    test_a_port_that_never_enables_is_named();
    test_a_running_controller_latching_pr_is_cleared_once_more();
    test_a_halted_controller_is_refused_before_any_write();
    test_hse_during_the_hold_stops_the_sequence();
    test_a_halt_without_hse_is_not_cleared_twice();
    test_a_powered_port_is_powered_before_its_reset();
    test_an_empty_port_is_not_reset();
    test_the_reset_reads_through_the_controllers_caplength();
    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    printf("EHCI root port reset host test: ALL OK\n");
    return 0;
}
