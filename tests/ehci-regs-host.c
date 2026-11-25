/*
 * ITEM-566 host proof: the operational window's address, and the engine over it.
 *
 * The hardware runs kept reporting schedule states (`ASE=0`, `ASS=0`, `PSS=1`)
 * that only a wrong or stale register base could explain, and every engine proof
 * until now substituted a whole register file, which cannot see an address
 * mistake: if the adapter drops CAPLENGTH, the substituted registers still
 * answer at the offsets the engine asks for.
 *
 * This proof drives the production accessor (kernel/src/ehci_regs.c) over a fake
 * BAR0 with CAPLENGTH = 0x20 - the value the Toshiba's EHCI reports - and then
 * drives the production engine through that same accessor, so the words the
 * engine writes have to land at BAR0 + CAPLENGTH + offset and the words it reads
 * have to come from there.  The capability dword at BAR0 + 0 is left as a
 * sentinel: an adapter that forgot CAPLENGTH would read and write it, and the
 * proof would see it change.
 */
#include "ehci.h"
#include "ehci_engine.h"
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

#define FAKE_BAR_BYTES 4096U
#define FAKE_CAPLENGTH 0x20U
/* The capability dword a CAPLENGTH-less adapter would touch. */
#define FAKE_CAP_SENTINEL 0x01000020U

struct fake_controller {
    uint8_t bar[FAKE_BAR_BYTES];
    uint32_t tokens[EHCI_ENGINE_MAX_QTD];
    uint32_t token_count;
    uint64_t ticks;
    uint64_t expires_at;
    bool linked;
    uint32_t link_calls;
    uint32_t doorbell_writes;
    uint32_t cap_dword_before;
};

static uint32_t cell(const struct fake_controller *fake, uint32_t absolute) {
    uint32_t value = 0U;

    memcpy(&value, &fake->bar[absolute], sizeof(value));
    return value;
}

static void set_cell(struct fake_controller *fake, uint32_t absolute,
                     uint32_t value) {
    memcpy(&fake->bar[absolute], &value, sizeof(value));
}

/* --- the accessor, on its own ------------------------------------------- */

static void test_the_accessor_addresses_through_caplength(void) {
    struct fake_controller fake;

    memset(&fake, 0, sizeof(fake));
    set_cell(&fake, 0U, FAKE_CAP_SENTINEL);
    set_cell(&fake, 4U, 0x00000006U); /* HCSPARAMS */
    fake.cap_dword_before = cell(&fake, 0U);

    ehci_reg_write(fake.bar, FAKE_CAPLENGTH, EHCI_OP_USBCMD, 0x00080021U);
    CHECK(cell(&fake, FAKE_CAPLENGTH + EHCI_OP_USBCMD) == 0x00080021U,
          "USBCMD lands at BAR + CAPLENGTH + 0");
    CHECK(cell(&fake, 0U) == FAKE_CAP_SENTINEL,
          "the capability dword at BAR + 0 is not written");
    CHECK(ehci_reg_read(fake.bar, FAKE_CAPLENGTH, EHCI_OP_USBCMD) ==
              0x00080021U,
          "the accessor reads back the word it wrote");

    ehci_reg_write(fake.bar, FAKE_CAPLENGTH, EHCI_OP_USBSTS, 0x00008000U);
    CHECK(cell(&fake, FAKE_CAPLENGTH + EHCI_OP_USBSTS) == 0x00008000U,
          "USBSTS lands at BAR + CAPLENGTH + 4");
    CHECK(cell(&fake, 4U) == 0x00000006U,
          "HCSPARAMS is not touched");

    /* A zero CAPLENGTH is the defect this item exists for: the same write
       would land on the capability dword.  The proof pins the difference. */
    ehci_reg_write(fake.bar, 0U, EHCI_OP_USBCMD, 0x00000011U);
    CHECK(cell(&fake, 0U) == 0x00000011U,
          "without CAPLENGTH the write would hit BAR + 0, which is what the "
          "hardware record's odd words would have looked like");
    CHECK(cell(&fake, FAKE_CAPLENGTH + EHCI_OP_USBCMD) == 0x00080021U,
          "the real register keeps its value");
}

/* --- the engine through the accessor ------------------------------------ */

static uint32_t adapter_read(void *context, uint32_t offset) {
    struct fake_controller *fake = context;

    return ehci_reg_read(fake->bar, FAKE_CAPLENGTH, offset);
}

static void adapter_write(void *context, uint32_t offset, uint32_t value) {
    struct fake_controller *fake = context;
    uint32_t status;
    uint32_t command;

    ehci_reg_write(fake->bar, FAKE_CAPLENGTH, offset, value);
    command = cell(fake, FAKE_CAPLENGTH + EHCI_OP_USBCMD);
    status = cell(fake, FAKE_CAPLENGTH + EHCI_OP_USBSTS);
    if (offset == EHCI_OP_USBCMD) {
        if ((command & EHCI_USBCMD_RS) == 0U) {
            status |= EHCI_USBSTS_HCH;
            status &= ~EHCI_USBSTS_ASS;
        } else if ((command & EHCI_USBCMD_ASE) != 0U) {
            status &= ~EHCI_USBSTS_HCH;
            /* Re-armed: the controller reports the schedule at the next
               frame.  This model reports it at once, which is the strongest
               case for the caller. */
            status |= EHCI_USBSTS_ASS;
        }
        if ((command & EHCI_USBCMD_IAAD) != 0U) {
            ++fake->doorbell_writes;
            status |= EHCI_USBSTS_IAA;
            command &= ~EHCI_USBCMD_IAAD;
            ehci_reg_write(fake->bar, FAKE_CAPLENGTH, EHCI_OP_USBCMD, command);
        }
        set_cell(fake, FAKE_CAPLENGTH + EHCI_OP_USBSTS, status);
    } else if (offset == EHCI_OP_USBSTS) {
        if ((value & EHCI_USBSTS_IAA) != 0U) {
            status &= ~EHCI_USBSTS_IAA;
            set_cell(fake, FAKE_CAPLENGTH + EHCI_OP_USBSTS, status);
        }
    }
}

static uint32_t adapter_token(void *context, uint32_t index) {
    struct fake_controller *fake = context;

    return index < fake->token_count ? fake->tokens[index] : 0U;
}

static void adapter_link(void *context, bool linked) {
    struct fake_controller *fake = context;

    fake->linked = linked;
    ++fake->link_calls;
}

static void adapter_barrier(void *context) {
    (void)context;
}

static uint64_t adapter_deadline(void *context, uint32_t microseconds) {
    struct fake_controller *fake = context;

    (void)microseconds;
    return fake->expires_at;
}

static bool adapter_expired(void *context, uint64_t deadline) {
    struct fake_controller *fake = context;

    return fake->ticks >= deadline;
}

static void adapter_pause(void *context) {
    struct fake_controller *fake = context;

    ++fake->ticks;
    if (fake->ticks == 1U) {
        for (uint32_t index = 0U; index < fake->token_count; ++index) {
            fake->tokens[index] &= ~EHCI_QTD_STATUS_ACTIVE;
        }
    }
}

static void test_the_engine_through_the_real_adapter(void) {
    struct fake_controller fake;
    struct ehci_engine_ops ops;
    struct ehci_engine_result result;
    uint32_t usbcmd;
    uint32_t usbsts;

    memset(&fake, 0, sizeof(fake));
    set_cell(&fake, 0U, FAKE_CAP_SENTINEL);
    fake.token_count = 2U;
    fake.tokens[0] = 0x00080000U; /* SETUP, Active */
    fake.tokens[1] = 0x80008C80U; /* status, Active */
    fake.expires_at = 1000U;
    /* The controller is running and reports ASS where the accessor looks. */
    set_cell(&fake, FAKE_CAPLENGTH + EHCI_OP_USBCMD,
             EHCI_USBCMD_RS | EHCI_USBCMD_ASE);
    set_cell(&fake, FAKE_CAPLENGTH + EHCI_OP_USBSTS, EHCI_USBSTS_ASS);

    memset(&ops, 0, sizeof(ops));
    ops.context = &fake;
    ops.read = adapter_read;
    ops.write = adapter_write;
    ops.qtd_token = adapter_token;
    ops.link_transfer_head = adapter_link;
    ops.barrier = adapter_barrier;
    ops.deadline = adapter_deadline;
    ops.expired = adapter_expired;
    ops.pause = adapter_pause;

    ehci_engine_run(&ops, fake.token_count, 500000U, 2000U, false, &result);

    usbcmd = cell(&fake, FAKE_CAPLENGTH + EHCI_OP_USBCMD);
    usbsts = cell(&fake, FAKE_CAPLENGTH + EHCI_OP_USBSTS);
    CHECK(result.transfer == EHCI_TRANSFER_OK && result.cause ==
              EHCI_ENGINE_CAUSE_NONE,
          "the engine ran over the real adapter and the chain retired");
    CHECK((result.usbcmd_before_link & EHCI_USBCMD_ASE) != 0U &&
              (result.usbsts_before_link & EHCI_USBSTS_ASS) != 0U,
          "the words before the link came from the operational window");
    CHECK(result.cleanup == EHCI_ENGINE_CLEANUP_ACKNOWLEDGED && result.quiesced,
          "the doorbell written through the adapter was acknowledged");
    CHECK(fake.doorbell_writes == 1U,
          "the doorbell write reached the controller through the adapter");
    CHECK((usbcmd & EHCI_USBCMD_RS) != 0U &&
              (usbcmd & EHCI_USBCMD_ASE) != 0U,
          "the controller is left armed for the next transfer");
    CHECK((usbsts & EHCI_USBSTS_HCH) == 0U,
          "the controller was not left halted");
    CHECK(cell(&fake, 0U) == FAKE_CAP_SENTINEL,
          "no engine access touched the capability dword");
}

/* A schedule the controller never reports as running: the engine must stop it
   and confirm the halt through the same adapter before the pool is reused. */
static void test_a_stopped_schedule_is_halted_and_confirmed(void) {
    struct fake_controller fake;
    struct ehci_engine_ops ops;
    struct ehci_engine_result result;

    memset(&fake, 0, sizeof(fake));
    set_cell(&fake, 0U, FAKE_CAP_SENTINEL);
    fake.token_count = 1U;
    fake.tokens[0] = 0x80008C80U;
    fake.expires_at = 1000U;
    set_cell(&fake, FAKE_CAPLENGTH + EHCI_OP_USBCMD, EHCI_USBCMD_RS);
    set_cell(&fake, FAKE_CAPLENGTH + EHCI_OP_USBSTS, 0U);

    memset(&ops, 0, sizeof(ops));
    ops.context = &fake;
    ops.read = adapter_read;
    ops.write = adapter_write;
    ops.qtd_token = adapter_token;
    ops.link_transfer_head = adapter_link;
    ops.barrier = adapter_barrier;
    ops.deadline = adapter_deadline;
    ops.expired = adapter_expired;
    ops.pause = adapter_pause;

    ehci_engine_run(&ops, fake.token_count, 500000U, 2000U, false, &result);

    CHECK(result.cleanup == EHCI_ENGINE_CLEANUP_NOT_RUNNING,
          "no doorbell is rung when the schedule is not walking the list");
    CHECK(result.halted_confirmed && result.quiesced && !result.stalled,
          "the pool is safe only after HCHalted came back through the adapter");
    CHECK(result.restarted && result.restart_confirmed,
          "the schedule was re-armed and ASS came back");
    CHECK(cell(&fake, 0U) == FAKE_CAP_SENTINEL,
          "the halt and the re-arm did not touch the capability dword");
}

int main(void) {
    test_the_accessor_addresses_through_caplength();
    test_the_engine_through_the_real_adapter();
    test_a_stopped_schedule_is_halted_and_confirmed();
    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    printf("EHCI operational window host test: ALL OK\n");
    return 0;
}
