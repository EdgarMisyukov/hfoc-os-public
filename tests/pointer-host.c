/*
 * ITEM-448: the pointer's arithmetic, driven without a screen.
 *
 * The contract is that a mouse draws a pointer and a click runs the menu entry
 * under it, and neither half can be proved by a screenshot in CI.  What can be
 * proved is the arithmetic between the two: a movement lands where the report
 * said relative to where the pointer was, a movement past an edge stops at the
 * edge instead of wrapping or leaving the screen, a held button is one press,
 * and a click lands in exactly the rectangle the console painted - including
 * the boundary cases where a hit test is usually wrong by one.
 */
#include <stdio.h>
#include <string.h>

#include "pointer.h"
#include "arch/x86_64/timer.h"

static int failures;

#define CHECK(condition, message)                                        \
    do {                                                                 \
        if (condition) {                                                 \
            printf("ok   - %s\n", message);                              \
        } else {                                                         \
            printf("FAIL - %s\n", message);                              \
            ++failures;                                                  \
        }                                                                \
    } while (0)

static void test_movement_is_relative(void)
{
    struct pointer_state state;

    pointer_state_reset(&state, 100, 100);
    CHECK(state.visible && state.x == 100 && state.y == 100 &&
              state.moves == 0U,
          "a reset puts the pointer at its start and counts nothing");
    CHECK(pointer_move(&state, 5, -5, (struct pointer_bounds){800, 600}) &&
              state.x == 105 && state.y == 95 && state.moves == 1U,
          "a movement lands at the previous position plus the report deltas");
    CHECK(pointer_move(&state, -10, 10, (struct pointer_bounds){800, 600}) &&
              state.x == 95 && state.y == 105 && state.moves == 2U,
          "the next report moves relative to where the pointer now is");
    CHECK(!pointer_move(&state, 0, 0, (struct pointer_bounds){800, 600}) &&
              state.moves == 2U,
          "a report with no movement is not counted as one");
}

static void test_edges_clamp_and_count(void)
{
    struct pointer_state state;

    pointer_state_reset(&state, 0, 0);
    CHECK(pointer_move(&state, -1, 0, (struct pointer_bounds){800, 600}) &&
              state.x == 0 && state.y == 0 && state.clamped == 1U,
          "a movement past the left edge stops at zero and is counted");
    CHECK(pointer_move(&state, 0, -4000, (struct pointer_bounds){800, 600}) &&
              state.y == 0 && state.clamped == 2U,
          "a movement past the top edge stops at zero");
    CHECK(pointer_move(&state, 4000, 4000, (struct pointer_bounds){800, 600}) &&
              state.x == 799 && state.y == 599 && state.clamped == 3U,
          "a movement past the bottom right stops at the last pixel");
    CHECK(state.moves == 3U,
          "a clamped movement is still a movement the pointer made");
}

static void test_unusable_input_is_refused(void)
{
    struct pointer_state state;

    pointer_state_reset(&state, 10, 10);
    CHECK(!pointer_move(NULL, 1, 1, (struct pointer_bounds){800, 600}),
          "a null pointer state is refused");
    CHECK(!pointer_move(&state, 1, 1, (struct pointer_bounds){0, 600}) &&
              state.moves == 0U && state.refused == 1U,
          "a zero width screen is refused and counted");
    pointer_state_reset(NULL, 0, 0);
    CHECK(pointer_hit_test(NULL, 0U, 0, 0) == -1,
          "a null reset is a no-op");
}

static void test_button_edges(void)
{
    struct pointer_state state;

    pointer_state_reset(&state, 0, 0);
    CHECK(pointer_button(&state, true) && state.presses == 1U &&
              state.pressed,
          "a button going down is one press edge");
    CHECK(!pointer_button(&state, true) && state.presses == 1U,
          "a held button is not a second press");
    CHECK(pointer_button(&state, false) && state.releases == 1U &&
              !state.pressed,
          "a button coming up is one release edge");
    CHECK(!pointer_button(&state, false) && state.releases == 1U,
          "a button already up is not a second release");
    CHECK(!pointer_button(NULL, true), "a null button state is refused");
}

static void test_hit_test_boundaries(void)
{
    const struct pointer_target targets[] = {
        { .x = 10, .y = 20, .width = 100, .height = 16, .value = 0 },
        { .x = 10, .y = 36, .width = 100, .height = 16, .value = 1 },
        { .x = 10, .y = 52, .width = 0, .height = 16, .value = 2 },
    };

    CHECK(pointer_hit_test(targets, 3U, 10, 20) == 0,
          "the top left corner of a rectangle is inside it");
    CHECK(pointer_hit_test(targets, 3U, 109, 35) == 0,
          "the last pixel of a rectangle is inside it");
    CHECK(pointer_hit_test(targets, 3U, 110, 35) == -1,
          "one pixel right of a rectangle is outside it");
    CHECK(pointer_hit_test(targets, 3U, 9, 20) == -1,
          "one pixel left of a rectangle is outside it");
    CHECK(pointer_hit_test(targets, 3U, 50, 36) == 1,
          "the second entry is selected by its own rectangle");
    CHECK(pointer_hit_test(targets, 3U, 50, 52) == -1,
          "a rectangle with no width is never hit");
    CHECK(pointer_hit_test(targets, 3U, 50, 0) == -1,
          "a click above every entry hits nothing");
    CHECK(pointer_hit_test(targets, 0U, 50, 20) == -1,
          "an empty target list hits nothing");
    CHECK(pointer_hit_test(NULL, 3U, 50, 20) == -1,
          "a null target list hits nothing");
}

static void test_the_menu_entries_are_separate_rectangles(void)
{
    /*
     * The console paints the six entries as six lines of one cell height, so
     * the seam between two entries belongs to exactly one of them - the whole
     * point of hit-testing rectangles rather than rounding a coordinate.
     */
    struct pointer_target entries[6];
    size_t index;

    for (index = 0U; index < 6U; ++index) {
        entries[index].x = 8;
        entries[index].y = (int32_t)(100 + 16 * index);
        entries[index].width = 240;
        entries[index].height = 16;
        entries[index].value = (int32_t)index;
    }
    CHECK(pointer_hit_test(entries, 6U, 8, 100) == 0 &&
              pointer_hit_test(entries, 6U, 8, 115) == 0 &&
              pointer_hit_test(entries, 6U, 8, 116) == 1 &&
              pointer_hit_test(entries, 6U, 8, 195) == 5 &&
              pointer_hit_test(entries, 6U, 8, 196) == -1,
          "each menu entry owns its own line and the seam belongs to one");
}

/*
 * ITEM-470: the console passes the mode's own glyph scale to the pointer, so
 * the same hand movement travels as far relative to the interface on a big
 * screen as on a small one.  Scale 1 must stay the exact arithmetic above (a
 * small mode must not become twitchy), a scaled report is the delta times the
 * scale, a fast, scaled report is carried farther than the scale alone, and a
 * scaled move past an edge is still clamped to the last pixel.
 */
static void test_the_scale_speeds_the_pointer(void)
{
    struct pointer_state state;

    CHECK(pointer_scale_delta(2, 1U) == 2 &&
              pointer_scale_delta(-2, 1U) == -2 &&
              pointer_scale_delta(16, 1U) == 16,
          "scale one is the exact movement it always was");

    pointer_state_reset(&state, 100, 100);
    CHECK(pointer_move_scaled(&state, 3, 4, (struct pointer_bounds){800, 600},
                              3U) &&
              state.x == 109 && state.y == 112 && state.moves == 1U,
          "a scaled move multiplies the report delta by the glyph scale");

    CHECK(pointer_scale_delta(8, 3U) == 48 &&
              pointer_scale_delta(-8, 3U) == -48,
          "a fast scaled report is carried farther than the glyph scale alone");

    pointer_state_reset(&state, 100, 100);
    CHECK(pointer_move_scaled(&state, 4000, 0,
                              (struct pointer_bounds){200, 200}, 3U) &&
              state.x == 199 && state.y == 100 && state.clamped == 1U,
          "a scaled move past the edge still stops at the last pixel");

    pointer_state_reset(&state, 10, 20);
    CHECK(pointer_move_scaled(&state, 5, -5, (struct pointer_bounds){800, 600},
                              1U) &&
              state.x == 15 && state.y == 15,
          "the scaled entry point keeps scale one bit for bit");
}

/*
 * ITEM-471 (СРОЧНО-21): the owner's round-9 run showed the arrow leave the
 * screen in one step and then never come back.  Two rules fix the arithmetic:
 * one report cannot move the arrow farther than POINTER_MAX_STEP, and the
 * sprite is drawn from an origin that keeps it wholly on the screen.  Scale 1
 * is still the identity, cap included, because the small modes were never the
 * problem.
 */
static void test_one_report_cannot_cross_the_screen(void)
{
    CHECK(pointer_scale_delta(4000, 3U) == POINTER_MAX_STEP &&
              pointer_scale_delta(-4000, 3U) == -POINTER_MAX_STEP,
          "one fast report is capped at a single followable step");
    CHECK(pointer_scale_delta(40, 3U) == POINTER_MAX_STEP,
          "the cap applies after the glyph scale and the acceleration");
    CHECK(pointer_scale_delta(8, 3U) == 48 &&
              pointer_scale_delta(8, 1U) == 8,
          "a step inside the cap is untouched and scale one stays exact");
    CHECK(pointer_scale_delta(4000, 1U) == 4000,
          "scale one keeps the raw delta even past the cap");
}

static void test_the_arrow_stays_whole_at_the_edges(void)
{
    CHECK(pointer_sprite_origin(100, 800, 11) == 100 &&
              pointer_sprite_origin(0, 800, 11) == 0,
          "a pointer away from an edge keeps its own origin");
    CHECK(pointer_sprite_origin(799, 800, 11) == 789,
          "the arrow shifts left so its last column stays on the screen");
    CHECK(pointer_sprite_origin(599, 600, 16) == 584,
          "the arrow shifts up so its last row stays on the screen");
    CHECK(pointer_sprite_origin(900, 800, 11) == 789 &&
              pointer_sprite_origin(-5, 800, 11) == 0,
          "an out-of-range position is clamped before the fit");
    CHECK(pointer_sprite_origin(10, 8, 11) == 0,
          "a screen smaller than the sprite uses the top-left corner");
}

/*
 * ITEM-471: the loop's own two clocks, as pure arithmetic.
 *
 * `iter_us` and `loops` come from the same window, so `iter_us * loops` is the
 * window by construction.  The owner's round-9 record paired 98 turns with an
 * 80 000 us average in one 100-tick window; with the tick taken at its word
 * that is ten milliseconds a turn, and the item publishes the calibrated rate
 * beside both so the next run can decide which clock lost real time instead of
 * assuming one of them.
 */
static void test_the_loop_clocks_reconcile(void)
{
    const uint64_t window_us = 1000000U;
    const uint64_t turns = 98U;
    const uint64_t average = timer_window_average_us(window_us, turns);

    CHECK(average == 10204U && average * turns <= window_us &&
              window_us - average * turns < turns,
          "the average turn is the window divided by the turns");
    CHECK(timer_window_average_us(0U, 7U) == 0U &&
              timer_window_average_us(777U, 0U) == 0U,
          "an empty window or no turns is zero, not a division");
    CHECK(timer_calibration_rate(3000000000ULL, 10U, 100U) ==
                  30000000000ULL &&
              timer_calibration_rate(3000000000ULL, 0U, 100U) == 0U,
          "the calibrated rate is the cycles per tick times the tick rate");
}

/*
 * ITEM-472 (СРОЧНО-22): the operator's gain.  It multiplies the scaled delta,
 * gain 1 is bit-for-bit the ITEM-471 arithmetic, and the single-report cap
 * still holds however high the gain is set.
 */
static void test_the_operator_gain(void)
{
    struct pointer_state state;

    CHECK(pointer_apply_delta(3, 1U, 1U) == 3 &&
              pointer_apply_delta(-3, 1U, 1U) == -3,
          "gain one at scale one is still the exact movement");
    CHECK(pointer_apply_delta(3, 1U, 4U) == 12 &&
              pointer_apply_delta(-3, 1U, 4U) == -12,
          "the operator's gain multiplies the movement");
    CHECK(pointer_apply_delta(100, 3U, POINTER_GAIN_MAX) == POINTER_MAX_STEP,
          "even the highest gain is capped at one followable step");
    pointer_state_reset(&state, 0, 0);
    CHECK(pointer_move_gained(&state, 5, 0, (struct pointer_bounds){800, 600},
                              1U, 2U) &&
              state.x == 10 && state.moves == 1U,
          "the gained move entry point applies the gain");
}

int main(void)
{
    test_movement_is_relative();
    test_edges_clamp_and_count();
    test_unusable_input_is_refused();
    test_button_edges();
    test_hit_test_boundaries();
    test_the_menu_entries_are_separate_rectangles();
    test_the_scale_speeds_the_pointer();
    test_one_report_cannot_cross_the_screen();
    test_the_arrow_stays_whole_at_the_edges();
    test_the_loop_clocks_reconcile();
    test_the_operator_gain();

    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    printf("\nPOINTER HOST TEST: ALL OK\n");
    return 0;
}
