/*
 * ITEM-559/564/566 host proof: the asynchronous schedule's transitions and its
 * verdicts, executed by the production engine over a scripted register file.
 *
 * The three hardware runs this proof follows:
 *
 *   #559: `usbcmd=0x00080070 usbsts=0x0000F018` - a halted controller with HSE
 *         and every descriptor still Active; the classifier answered OTHER.
 *   #564: `result=TIMEOUT cause=ASYNC_ADVANCE` - the transfer timed out and the
 *         stop's own failure then replaced the transfer's cause.
 *   #565: `cause=ASYNC_STOPPED cleanup=NOT_RUNNING quiesced=1 stalled=0` - the
 *         engine declared the pool safe because two status bits said the
 *         schedule was not running, without any confirmation that the
 *         controller had left the queue head.
 *
 * What the proof now requires:
 *
 *   * a transfer wait does not call the schedule stopped for the start-up
 *     transition (`ASE=1, ASS=0` at frame boundaries, EHCI 1.0 section 4.8);
 *     the window is bounded and the words at the first refusal are kept;
 *   * a retired descriptor chain is not evidence that the controller released
 *     the head: reuse of the pool requires an acknowledged doorbell or a
 *     confirmed halt (HCHalted read back), and the schedule is re-armed after
 *     that halt so the walk can keep looking;
 *   * the transfer's verdict and the stop's are separate records.
 *
 * The fake below models a controller, not a register dump: writing USBCMD with
 * RS clear halts it (HCH appears, ASS falls), writing RS|ASE re-arms it (ASS
 * appears after a scripted number of frames), and the doorbell either answers,
 * is ignored, or is refused.
 */
#include "ehci_engine.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } \
} while (0)

struct fake {
    uint32_t usbcmd;
    uint32_t usbsts;
    uint32_t portsc;
    uint32_t tokens[EHCI_ENGINE_MAX_QTD];
    uint32_t token_count;
    bool linked;
    uint32_t link_calls;
    uint32_t doorbell_writes;
    uint32_t iaa_writes;
    uint32_t usbcmd_writes;
    uint32_t usbcmd_last_write;
    /* The clock: one tick per pause. */
    uint64_t ticks;
    uint64_t expires_at;
    /* Scripted controller behaviour. */
    bool complete_on_pause;   /* the chain retires on the first pause */
    uint32_t ass_delay_pauses; /* frames until ASS after RS|ASE is written */
    uint32_t ass_pending_pauses;
    uint32_t complete_after_pauses; /* the chain retires on this tick */
    uint32_t ass_falls_after_pauses; /* ASS disappears while ASE stays set */
    bool refuse_doorbell;
    uint32_t ack_after_pauses;
    uint32_t pauses_after_doorbell;
    bool ignore_halt_command; /* the controller never reports HCHalted */
    bool iaa_pre_set;
};

static uint32_t fake_read(void *context, uint32_t offset) {
    struct fake *fake = context;

    switch (offset) {
    case EHCI_OP_USBCMD:
        return fake->usbcmd;
    case EHCI_OP_USBSTS:
        return fake->usbsts;
    case EHCI_OP_PORTSC:
        return fake->portsc;
    default:
        return 0U;
    }
}

static void fake_write(void *context, uint32_t offset, uint32_t value) {
    struct fake *fake = context;

    if (offset == EHCI_OP_USBCMD) {
        ++fake->usbcmd_writes;
        fake->usbcmd_last_write = value;
        if ((value & EHCI_USBCMD_RS) == 0U) {
            /* Halted: the controller stops walking every list. */
            fake->usbcmd = value;
            if (!fake->ignore_halt_command) {
                fake->usbsts |= EHCI_USBSTS_HCH;
                fake->usbsts &= ~(EHCI_USBSTS_ASS | EHCI_USBSTS_PSS);
            }
            return;
        }
        if ((value & EHCI_USBCMD_ASE) != 0U) {
            /* Re-armed: HCH goes away and ASS follows at the next frame. */
            fake->usbsts &= ~EHCI_USBSTS_HCH;
            fake->ass_pending_pauses = fake->ass_delay_pauses;
            if (fake->ass_delay_pauses == 0U) {
                fake->usbsts |= EHCI_USBSTS_ASS;
            }
        }
        if ((value & EHCI_USBCMD_IAAD) != 0U) {
            ++fake->doorbell_writes;
            fake->pauses_after_doorbell = 0U;
            /* QEMU's shape: the bit is consumed immediately, and IAA appears
               on a later frame.  The read-back alone is therefore not the
               verdict; the bounded wait is. */
            value &= ~EHCI_USBCMD_IAAD;
            if (!fake->refuse_doorbell && fake->ack_after_pauses == 0U) {
                fake->usbsts |= EHCI_USBSTS_IAA;
            }
        }
        fake->usbcmd = value;
        return;
    }
    if (offset == EHCI_OP_USBSTS) {
        if ((value & EHCI_USBSTS_IAA) != 0U) {
            ++fake->iaa_writes;
            fake->usbsts &= ~EHCI_USBSTS_IAA;
        }
    }
}

static uint32_t fake_token(void *context, uint32_t index) {
    struct fake *fake = context;

    return index < fake->token_count ? fake->tokens[index] : 0U;
}

static void fake_link(void *context, bool linked) {
    struct fake *fake = context;

    fake->linked = linked;
    ++fake->link_calls;
}

static void fake_barrier(void *context) {
    (void)context;
}

static uint64_t fake_deadline(void *context, uint32_t microseconds) {
    struct fake *fake = context;

    (void)microseconds;
    return fake->expires_at;
}

static bool fake_expired(void *context, uint64_t deadline) {
    struct fake *fake = context;

    return fake->ticks >= deadline;
}

static void fake_pause(void *context) {
    struct fake *fake = context;

    ++fake->ticks;
    if (fake->complete_on_pause && fake->ticks == 1U) {
        for (uint32_t index = 0U; index < fake->token_count; ++index) {
            fake->tokens[index] &= ~EHCI_QTD_STATUS_ACTIVE;
        }
    }
    if (fake->complete_after_pauses != 0U &&
        fake->ticks == fake->complete_after_pauses) {
        for (uint32_t index = 0U; index < fake->token_count; ++index) {
            fake->tokens[index] &= ~EHCI_QTD_STATUS_ACTIVE;
        }
    }
    if (fake->ass_falls_after_pauses != 0U &&
        fake->ticks == fake->ass_falls_after_pauses) {
        fake->usbsts &= ~EHCI_USBSTS_ASS;
    }
    if (fake->ass_pending_pauses != 0U) {
        --fake->ass_pending_pauses;
        if (fake->ass_pending_pauses == 0U) {
            fake->usbsts |= EHCI_USBSTS_ASS;
        }
    }
    if (fake->doorbell_writes != 0U && !fake->refuse_doorbell &&
        fake->ack_after_pauses != 0U) {
        ++fake->pauses_after_doorbell;
        if (fake->pauses_after_doorbell >= fake->ack_after_pauses) {
            fake->usbsts |= EHCI_USBSTS_IAA;
            fake->usbcmd &= ~EHCI_USBCMD_IAAD;
        }
    }
}

static struct ehci_engine_ops fake_ops(struct fake *fake) {
    struct ehci_engine_ops ops;

    memset(&ops, 0, sizeof(ops));
    ops.context = fake;
    ops.read = fake_read;
    ops.write = fake_write;
    ops.qtd_token = fake_token;
    ops.link_transfer_head = fake_link;
    ops.barrier = fake_barrier;
    ops.deadline = fake_deadline;
    ops.expired = fake_expired;
    ops.pause = fake_pause;
    ops.portsc_offset = EHCI_OP_PORTSC;
    return ops;
}

static void script_control_tokens(struct fake *fake) {
    fake->token_count = 3U;
    fake->tokens[0] = 0x00080E80U; /* SETUP 8, CERR 3, Active */
    fake->tokens[1] = 0x80080D80U; /* IN 8, DATA1, CERR 3, Active */
    fake->tokens[2] = 0x80008C80U; /* OUT status, IOC, DATA1, CERR 3, Active */
}

/* A running schedule: RS and ASE commanded, the controller reporting ASS. */
static struct fake machine(void) {
    struct fake fake;

    memset(&fake, 0, sizeof(fake));
    script_control_tokens(&fake);
    fake.usbcmd = EHCI_USBCMD_RS | EHCI_USBCMD_ASE;
    fake.usbsts = EHCI_USBSTS_ASS;
    fake.portsc = 0x00001005U;
    fake.expires_at = 5U;
    fake.ass_delay_pauses = 0U;
    return fake;
}

/* The start-up transition: ASE is set but the controller has not reported ASS
   yet.  The wait must not call that a stopped schedule. */
static void test_the_startup_transition_is_not_a_stop(void) {
    struct fake fake = machine();
    const struct ehci_engine_ops ops = fake_ops(&fake);
    struct ehci_engine_result result;

    fake.usbsts = 0U;                 /* ASS not reported yet */
    fake.ass_pending_pauses = 2U;     /* it appears two frames later */
    fake.complete_after_pauses = 3U;  /* the chain retires after that */
    fake.expires_at = 1000U;

    ehci_engine_run(&ops, fake.token_count, 500000U, 1000U, false, &result);

    CHECK(result.cause == EHCI_ENGINE_CAUSE_NONE,
          "a schedule that is still starting is not named stopped");
    CHECK(result.transfer == EHCI_TRANSFER_OK,
          "the transfer completed once the schedule came up");
    CHECK(result.ass_seen,
          "ASS was observed within the start-up window");
}

/* The schedule stops while the transfer waits: the stop is named only after the
   bounded window, and the words at the first refusal are kept. */
static void test_the_schedule_stopping_mid_wait_is_named_after_the_window(void) {
    struct fake fake = machine();
    const struct ehci_engine_ops ops = fake_ops(&fake);
    struct ehci_engine_result result;

    fake.ass_delay_pauses = 0U;
    fake.ass_falls_after_pauses = 1U; /* ASS goes away, ASE stays set */
    fake.expires_at = 1000U;

    ehci_engine_run(&ops, fake.token_count, 500000U, 1000U, false, &result);

    CHECK(result.cause == EHCI_ENGINE_CAUSE_ASYNC_STOPPED,
          "a schedule that stopped is named ASYNC_STOPPED");
    CHECK(result.transfer == EHCI_TRANSFER_TIMEOUT,
          "the descriptors were still in flight");
    CHECK(result.ass_waited_us == EHCI_ENGINE_ASS_TRANSITION_US,
          "the start-up window was played out before the verdict");
    CHECK((result.usbcmd_first_refusal & EHCI_USBCMD_ASE) != 0U &&
              (result.usbsts_first_refusal & EHCI_USBSTS_ASS) == 0U,
          "the words at the first refusal show ASE set and ASS clear");
}

/* With the schedule not walking the list, the pool is only safe after the
   controller is halted and says so; the schedule is re-armed afterwards. */
static void test_a_stopped_schedule_needs_a_confirmed_halt(void) {
    struct fake fake = machine();
    const struct ehci_engine_ops ops = fake_ops(&fake);
    struct ehci_engine_result result;

    fake.usbcmd = EHCI_USBCMD_RS;
    fake.usbsts = 0U;
    fake.ass_delay_pauses = 0U;
    fake.complete_on_pause = true;

    ehci_engine_run(&ops, fake.token_count, 500000U, 1000U, false, &result);

    CHECK(result.cause == EHCI_ENGINE_CAUSE_ASYNC_STOPPED,
          "the schedule is named stopped");
    CHECK(result.cleanup == EHCI_ENGINE_CLEANUP_NOT_RUNNING,
          "no doorbell is rung on a schedule that is not walking the list");
    CHECK(result.halted_confirmed && result.quiesced && !result.stalled,
          "the pool is safe only after HCHalted was read back");
    CHECK(result.restarted && result.restart_confirmed,
          "the schedule was re-armed and ASS came back for the next transfer");
    CHECK(fake.doorbell_writes == 0U,
          "no doorbell was rung");
}

/* The same state, but the controller never reports HCHalted: nothing confirms
   that it left the head, so the pool stays out of use. */
static void test_an_unconfirmed_halt_keeps_the_pool_out_of_use(void) {
    struct fake fake = machine();
    const struct ehci_engine_ops ops = fake_ops(&fake);
    struct ehci_engine_result result;

    fake.usbcmd = EHCI_USBCMD_RS;
    fake.usbsts = 0U;
    fake.ignore_halt_command = true;

    ehci_engine_run(&ops, fake.token_count, 500000U, 1000U, false, &result);

    CHECK(result.cleanup == EHCI_ENGINE_CLEANUP_NOT_RUNNING,
          "the stop is named for what was asked");
    CHECK(!result.halted_confirmed && !result.quiesced && result.stalled,
          "without HCHalted the pool must not be reused");
}

/* A completed transfer whose doorbell is refused still needs a confirmed stop
   before the pool may be reused. */
static void test_a_refused_doorbell_needs_the_confirmed_halt(void) {
    struct fake fake = machine();
    const struct ehci_engine_ops ops = fake_ops(&fake);
    struct ehci_engine_result result;

    fake.complete_on_pause = true;
    fake.refuse_doorbell = true;
    fake.ass_delay_pauses = 0U;

    ehci_engine_run(&ops, fake.token_count, 500000U, 1000U, false, &result);

    CHECK(result.transfer == EHCI_TRANSFER_OK &&
              result.cause == EHCI_ENGINE_CAUSE_NONE,
          "the transfer itself completed");
    CHECK(result.cleanup == EHCI_ENGINE_CLEANUP_REFUSED,
          "the doorbell was not accepted");
    CHECK(result.halted_confirmed && result.restarted &&
              result.restart_confirmed && result.quiesced && !result.stalled,
          "the pool is safe because the controller confirmed the halt, not "
          "because the descriptors retired");
}

/* A refused doorbell over a live transfer without a confirmed halt keeps the
   pool out of use, and the transfer's risk stands as its own verdict. */
static void test_a_refused_doorbell_without_a_halt_keeps_the_pool(void) {
    struct fake fake = machine();
    const struct ehci_engine_ops ops = fake_ops(&fake);
    struct ehci_engine_result result;

    fake.refuse_doorbell = true;
    fake.ignore_halt_command = true;
    fake.expires_at = 3U;

    ehci_engine_run(&ops, fake.token_count, 500000U, 1000U, false, &result);

    CHECK(result.cause == EHCI_ENGINE_CAUSE_WAIT_EXPIRED &&
              result.transfer == EHCI_TRANSFER_TIMEOUT,
          "the transfer's own verdict is kept");
    CHECK(result.cleanup == EHCI_ENGINE_CLEANUP_REFUSED && !result.quiesced &&
              result.stalled,
          "an unconfirmed stop forbids reuse");
}

/* The doorbell is consumed immediately and answered several frames later
   (QEMU's shape): the read-back has already lost the bit, so only the bounded
   IAA wait can see the acknowledgement. */
static void test_a_delayed_acknowledgement_is_waited_for(void) {
    struct fake fake = machine();
    const struct ehci_engine_ops ops = fake_ops(&fake);
    struct ehci_engine_result result;

    fake.complete_on_pause = true;
    fake.ack_after_pauses = 3U;
    fake.expires_at = 1000U;

    ehci_engine_run(&ops, fake.token_count, 500000U, 1000U, false, &result);

    CHECK(result.transfer == EHCI_TRANSFER_OK &&
              result.cause == EHCI_ENGINE_CAUSE_NONE,
          "a retired witness is a completed transfer");
    CHECK((result.doorbell_readback & EHCI_USBCMD_IAAD) == 0U,
          "the immediate read-back had already lost the doorbell bit");
    CHECK(result.cleanup == EHCI_ENGINE_CLEANUP_ACKNOWLEDGED &&
              result.quiesced && !result.stalled,
          "the delayed acknowledgement was waited for and seen");
    CHECK(fake.pauses_after_doorbell >= 3U,
          "the stop's wait really waited for the acknowledgement");
}

/* An IAA left set by an earlier transfer is cleared before the doorbell, so it
   cannot be read as this stop's acknowledgement. */
static void test_a_stale_iaa_is_not_an_acknowledgement(void) {
    struct fake fake = machine();
    const struct ehci_engine_ops ops = fake_ops(&fake);
    struct ehci_engine_result result;

    fake.complete_on_pause = true;
    fake.usbsts |= EHCI_USBSTS_IAA;
    fake.iaa_pre_set = true;
    fake.ack_after_pauses = 2U;
    fake.expires_at = 1000U;

    ehci_engine_run(&ops, fake.token_count, 500000U, 1000U, false, &result);

    CHECK(result.stale_iaa,
          "the engine saw the leftover IAA and says so");
    CHECK(result.cleanup == EHCI_ENGINE_CLEANUP_ACKNOWLEDGED,
          "the acknowledgement came from the doorbell's answer");
    CHECK(fake.iaa_writes == 2U,
          "the stale bit was cleared before the doorbell and ours after it");
}

/* HSE halts the controller: the wait stops at once, the cause is HSE, and the
   stop is confirmed from the halt. */
static void test_hse_halts_the_wait(void) {
    struct fake fake = machine();
    const struct ehci_engine_ops ops = fake_ops(&fake);
    struct ehci_engine_result result;

    fake.usbcmd = EHCI_USBCMD_RS | EHCI_USBCMD_ASE | EHCI_USBCMD_PSE;
    fake.usbsts = EHCI_USBSTS_HSE | EHCI_USBSTS_HCH | EHCI_USBSTS_ASS;
    fake.expires_at = 1000U;
    fake.ass_delay_pauses = 0U;

    ehci_engine_run(&ops, fake.token_count, 500000U, 1000U, false, &result);

    CHECK(result.cause == EHCI_ENGINE_CAUSE_HOST_SYSTEM_ERROR &&
              strcmp(ehci_engine_cause_string(result.cause), "HSE") == 0,
          "the halted controller with HSE is named HSE");
    CHECK(result.transfer == EHCI_TRANSFER_TIMEOUT,
          "descriptors that never retired keep the classifier's timeout");
    CHECK(result.halted_confirmed && result.quiesced,
          "the halt is confirmed from HCHalted");
    CHECK((result.usbcmd_before_link & EHCI_USBCMD_PSE) != 0U,
          "the words before the link are kept");
}

/* A descriptor error with a healthy schedule keeps the classifier's verdict. */
static void test_a_descriptor_error_keeps_its_verdict(void) {
    struct fake fake = machine();
    const struct ehci_engine_ops ops = fake_ops(&fake);
    struct ehci_engine_result result;

    fake.complete_on_pause = true;
    fake.tokens[0] = EHCI_QTD_STATUS_HALTED | EHCI_QTD_STATUS_TRANSACTION |
                     0x00080000U;

    ehci_engine_run(&ops, fake.token_count, 500000U, 1000U, false, &result);

    CHECK(result.cause == EHCI_ENGINE_CAUSE_NONE,
          "a healthy schedule adds no cause of its own");
    CHECK(result.transfer == EHCI_TRANSFER_TRANSACTION,
          "the classifier's own verdict is kept, not replaced");
    CHECK(result.failed_index == 0U,
          "the failing descriptor is the one that carried the error");
    CHECK(result.cleanup == EHCI_ENGINE_CLEANUP_ACKNOWLEDGED && result.quiesced,
          "the stop of a healthy transfer is confirmed like any other");
}

/* A previous stop was never confirmed: nothing may touch the pool at all. */
static void test_stalled_engine_refuses_before_any_register(void) {
    struct fake fake = machine();
    const struct ehci_engine_ops ops = fake_ops(&fake);
    struct ehci_engine_result result;

    ehci_engine_run(&ops, fake.token_count, 500000U, 1000U, true, &result);

    CHECK(result.cause == EHCI_ENGINE_CAUSE_ENGINE_STALLED &&
              result.stalled,
          "a stalled engine is named STALLED and stays stalled");
    CHECK(result.transfer == EHCI_TRANSFER_OTHER,
          "a refused transfer has no descriptor verdict");
    CHECK(fake.link_calls == 0U && fake.doorbell_writes == 0U &&
              fake.ticks == 0U,
          "nothing was linked, rung or waited on");
}

static void test_invalid_input_is_other(void) {
    struct ehci_engine_result result;
    struct ehci_engine_ops ops;
    struct fake fake = machine();

    memset(&result, 0, sizeof(result));
    ehci_engine_run(NULL, 3U, 1000U, 1000U, false, &result);
    CHECK(result.transfer == EHCI_TRANSFER_OTHER &&
              result.cause == EHCI_ENGINE_CAUSE_NONE &&
              result.cleanup == EHCI_ENGINE_CLEANUP_NONE,
          "no transport is not a schedule failure");

    ops = fake_ops(&fake);
    ehci_engine_run(&ops, 0U, 1000U, 1000U, false, &result);
    CHECK(result.transfer == EHCI_TRANSFER_OTHER && fake.link_calls == 0U,
          "an empty descriptor list is refused before the schedule is touched");

    ops.qtd_token = NULL;
    ehci_engine_run(&ops, 3U, 1000U, 1000U, false, &result);
    CHECK(result.transfer == EHCI_TRANSFER_OTHER && fake.link_calls == 0U,
          "an incomplete transport is refused");
}

int main(void) {
    test_the_startup_transition_is_not_a_stop();
    test_the_schedule_stopping_mid_wait_is_named_after_the_window();
    test_a_stopped_schedule_needs_a_confirmed_halt();
    test_an_unconfirmed_halt_keeps_the_pool_out_of_use();
    test_a_refused_doorbell_needs_the_confirmed_halt();
    test_a_refused_doorbell_without_a_halt_keeps_the_pool();
    test_a_delayed_acknowledgement_is_waited_for();
    test_a_stale_iaa_is_not_an_acknowledgement();
    test_hse_halts_the_wait();
    test_a_descriptor_error_keeps_its_verdict();
    test_stalled_engine_refuses_before_any_register();
    test_invalid_input_is_other();

    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    printf("EHCI schedule engine host test: ALL OK\n");
    return 0;
}
