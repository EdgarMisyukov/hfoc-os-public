/*
 * ITEM-479 (СРОЧНО-28, round 17): the loop turn's phase attribution, proven on
 * the host.
 *
 * The owner's 2026-09-25 hardware run read one turn of `input_us=1213704`
 * (`/srv/hfoc-example/queue/hw8-log-all.txt`) inside a healthy 10 ms loop and the
 * record could not say whether the driver service, the keyboard's events or
 * the pointer's movement spent it.  These cases drive the pure arithmetic
 * `log_production_loop_phases()` uses: the dominant span is chosen by value,
 * ties keep the fixed order, the remainder wins only when it really is the
 * largest, and the stall-turn bound excludes the healthy 10 ms turn.
 */
#include "loop_budget.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(condition, message)                                          \
    do {                                                                   \
        ++checks;                                                          \
        if (!(condition)) {                                                \
            ++failures;                                                    \
            printf("FAIL: %s\n", (message));                               \
        }                                                                  \
    } while (0)

static void test_pointer_spends_the_turn(void)
{
    struct loop_budget_snapshot turn;

    memset(&turn, 0, sizeof(turn));
    turn.console_us = 0U;
    turn.xhci_us = 97U;
    turn.in_xhci_us = 97U;
    turn.input_us = 1213704U;
    turn.in_kbd_us = 25U;
    turn.in_ptr_us = 1213600U;
    turn.net_us = 400U;
    turn.work_us = 1216404U;

    CHECK(loop_budget_dominant(&turn) == LOOP_BUDGET_INPUT_POINTER,
          "the pointer's movement is named when it spends the turn");
    CHECK(strcmp(loop_budget_phase_name(loop_budget_dominant(&turn)),
                 "in_ptr") == 0,
          "the pointer's phase has a stable record name");
}

static void test_keyboard_spends_the_turn(void)
{
    struct loop_budget_snapshot turn;

    memset(&turn, 0, sizeof(turn));
    turn.in_kbd_us = 900U;
    turn.in_ptr_us = 100U;
    turn.input_us = 1000U;
    turn.work_us = 1000U;

    CHECK(loop_budget_dominant(&turn) == LOOP_BUDGET_INPUT_KEYBOARD,
          "the keyboard's decoded events are named when they spend the turn");
    CHECK(strcmp(loop_budget_phase_name(loop_budget_dominant(&turn)),
                 "in_kbd") == 0,
          "the keyboard's phase has a stable record name");
}

static void test_driver_service_spends_the_turn(void)
{
    struct loop_budget_snapshot turn;

    memset(&turn, 0, sizeof(turn));
    turn.xhci_us = 800U;
    turn.in_xhci_us = 750U;
    turn.work_us = 800U;

    /* The service sub-span is the larger part of the driver span. */
    CHECK(loop_budget_dominant(&turn) == LOOP_BUDGET_INPUT_XHCI,
          "the driver service and doorbell are named when they dominate");

    /* When the diagnostics outside the service dominate instead, name that. */
    turn.xhci_us = 800U;
    turn.in_xhci_us = 100U;
    turn.work_us = 800U;
    CHECK(loop_budget_dominant(&turn) == LOOP_BUDGET_XHCI,
          "the driver remainder is named when the diagnostics dominate");
}

static void test_console_and_other(void)
{
    struct loop_budget_snapshot turn;

    memset(&turn, 0, sizeof(turn));
    turn.console_us = 500U;
    turn.work_us = 500U;
    CHECK(loop_budget_dominant(&turn) == LOOP_BUDGET_CONSOLE,
          "the console's paint and scroll are named when they dominate");
    CHECK(strcmp(loop_budget_phase_name(loop_budget_dominant(&turn)),
                 "console") == 0,
          "the console's phase has a stable record name");

    memset(&turn, 0, sizeof(turn));
    turn.work_us = 1000U;
    CHECK(loop_budget_dominant(&turn) == LOOP_BUDGET_OTHER,
          "the remainder is named when no accounted span dominates");
    CHECK(strcmp(loop_budget_phase_name(loop_budget_dominant(&turn)),
                 "other") == 0,
          "the remainder has a stable record name");
}

/*
 * ITEM-486 (round 23): the former `other_us` remainder is split into the
 * services/application state, the journal records and their disk flush, and the
 * storage/queued writes.  The owner's run read `other_us=3 108 092` beside
 * `work_us=3 108 657`; these cases prove the arithmetic names the subsystem
 * that spent such a turn instead of the remainder.
 */
static void test_remainder_names_its_parts(void)
{
    struct loop_budget_snapshot turn;

    memset(&turn, 0, sizeof(turn));
    turn.svc_us = 3100000U;
    turn.work_us = 3108657U;
    CHECK(loop_budget_dominant(&turn) == LOOP_BUDGET_SERVICES,
          "the services span is named when it spends the turn");
    CHECK(strcmp(loop_budget_phase_name(loop_budget_dominant(&turn)),
                 "svc") == 0,
          "the services phase has a stable record name");

    memset(&turn, 0, sizeof(turn));
    turn.journal_us = 3100000U;
    turn.work_us = 3108657U;
    CHECK(loop_budget_dominant(&turn) == LOOP_BUDGET_JOURNAL,
          "the journal span is named when the flush spends the turn");
    CHECK(strcmp(loop_budget_phase_name(loop_budget_dominant(&turn)),
                 "journal") == 0,
          "the journal phase has a stable record name");

    memset(&turn, 0, sizeof(turn));
    turn.store_us = 3100000U;
    turn.work_us = 3108657U;
    CHECK(loop_budget_dominant(&turn) == LOOP_BUDGET_STORAGE,
          "the storage span is named when a queued write spends the turn");
    CHECK(strcmp(loop_budget_phase_name(loop_budget_dominant(&turn)),
                 "store") == 0,
          "the storage phase has a stable record name");

    /*
     * The named parts come out of the remainder: the residual only wins when it
     * is larger than the named part that left it.
     */
    memset(&turn, 0, sizeof(turn));
    turn.svc_us = 600U;
    turn.work_us = 1000U;
    CHECK(loop_budget_dominant(&turn) == LOOP_BUDGET_SERVICES,
          "the residual is smaller than the named part that left it");
}

static void test_ties_keep_the_fixed_order(void)
{
    struct loop_budget_snapshot turn;

    memset(&turn, 0, sizeof(turn));
    turn.console_us = 500U;
    turn.in_ptr_us = 500U;
    turn.work_us = 1000U;
    CHECK(loop_budget_dominant(&turn) == LOOP_BUDGET_CONSOLE,
          "a tie between the console and the pointer keeps the console");

    memset(&turn, 0, sizeof(turn));
    turn.in_kbd_us = 500U;
    turn.in_ptr_us = 500U;
    turn.input_us = 1000U;
    turn.work_us = 1000U;
    CHECK(loop_budget_dominant(&turn) == LOOP_BUDGET_INPUT_KEYBOARD,
          "a tie between the keyboard and the pointer keeps the keyboard");
}

static void test_stall_turn_bound(void)
{
    CHECK(!loop_budget_is_long_turn(0U),
          "an empty turn is not a stall turn");
    CHECK(!loop_budget_is_long_turn(9998U),
          "the healthy ten-millisecond turn is not a stall turn");
    CHECK(!loop_budget_is_long_turn(LOOP_BUDGET_LONG_TURN_US),
          "a turn exactly at the bound is not a stall turn");
    CHECK(loop_budget_is_long_turn(LOOP_BUDGET_LONG_TURN_US + 1U),
          "a turn just past the bound is a stall turn");
    CHECK(loop_budget_is_long_turn(1216404U),
          "the owner's 1.2-second input turn is a stall turn");
    CHECK(loop_budget_is_long_turn(3159521U),
          "the boot's worst turn is a stall turn");
}

/*
 * MOUSE-DIAG step 3: the accounting-mismatch rule.  It compares the existing
 * phase sum with an independent raw anchor and never corrects it; the slack is
 * the pacing period, because a turn that straddles the window boundary folds
 * its whole span into the window.
 */
static void test_accounting_mismatch(void)
{
    CHECK(!loop_budget_accounting_mismatch(4000000U, 4000000U, 10000U),
          "a phase sum equal to the raw window is not a mismatch");
    CHECK(!loop_budget_accounting_mismatch(4005000U, 4000000U, 10000U),
          "an overshoot inside the straddling turn's slack is not a mismatch");
    CHECK(loop_budget_accounting_mismatch(4010001U, 4000000U, 10000U),
          "an overshoot past the slack is a mismatch");
    CHECK(loop_budget_accounting_mismatch(2829709U, 1000000U, 38579U),
          "the hw14 journal window is a mismatch");
    CHECK(loop_budget_accounting_mismatch(2831316U, 1000000U, 38579U),
          "the hw14 work span is a mismatch against the one-second window");
    CHECK(!loop_budget_accounting_mismatch(UINT64_MAX, UINT64_MAX, 10000U),
          "a saturated raw value does not overflow into a false mismatch");
}

static void test_missing_snapshot(void)
{
    CHECK(loop_budget_dominant(NULL) == LOOP_BUDGET_OTHER,
          "a turn with no snapshot is named as the remainder");
}

/*
 * MOUSE-DIAG (owner task 2026-09-27): `iter_max_us` alone could not be told
 * from a measurement artifact, so the record that names its phase is written
 * once per new boot maximum and only past the long-turn budget.  The proof pins
 * both boundaries: an ordinary paced turn never publishes, a long turn below
 * the previous maximum never publishes, and the first long turn above it does.
 */
static void test_iter_max_publish_policy(void)
{
    CHECK(!loop_budget_iter_max_should_publish(9998U, 0U),
          "the paced ten-millisecond turn never publishes a maximum");
    CHECK(!loop_budget_iter_max_should_publish(LOOP_BUDGET_LONG_TURN_US, 0U),
          "a turn exactly at the bound is not a stall and publishes nothing");
    CHECK(loop_budget_iter_max_should_publish(LOOP_BUDGET_LONG_TURN_US + 1U, 0U),
          "the first stall turn past the bound publishes");
    CHECK(!loop_budget_iter_max_should_publish(1216404U, 3159521U),
          "a stall turn below the boot maximum publishes nothing");
    CHECK(loop_budget_iter_max_should_publish(3159521U, 1216404U),
          "a stall turn above the boot maximum publishes");
    CHECK(!loop_budget_iter_max_should_publish(3259516U, 3259516U),
          "the same maximum twice publishes once, not per turn");
}

static void test_every_phase_has_a_name(void)
{
    for (int phase = 0; phase < (int)LOOP_BUDGET_PHASE_COUNT; ++phase) {
        const char *name = loop_budget_phase_name(
            (enum loop_budget_phase)phase);

        CHECK(name != NULL && name[0] != '\0',
              "every loop phase has a non-empty record name");
    }
}

int main(void)
{
    test_pointer_spends_the_turn();
    test_keyboard_spends_the_turn();
    test_driver_service_spends_the_turn();
    test_console_and_other();
    test_remainder_names_its_parts();
    test_ties_keep_the_fixed_order();
    test_stall_turn_bound();
    test_missing_snapshot();
    test_iter_max_publish_policy();
    test_accounting_mismatch();
    test_every_phase_has_a_name();

    if (failures != 0) {
        fprintf(stderr, "Loop budget host tests: %d FAILURES\n", failures);
        return 1;
    }
    printf("Loop budget host tests: ALL OK (%d checks)\n", checks);
    return 0;
}
