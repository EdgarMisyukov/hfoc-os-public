#ifndef HFOC_POINTER_H
#define HFOC_POINTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-448: where the pointer is, what one movement does to it, and which
 * rectangle a click lands in.
 *
 * The module owns the arithmetic only.  The xHCI driver produces one decoded
 * mouse event at a time, the console paints the pointer and runs the menu
 * entry a click selects, and the framebuffer owns the pixels underneath the
 * pointer; none of the three is named here, so the host proof drives every
 * edge - a movement off each edge, a click on a rectangle boundary, a click
 * outside every rectangle - without a screen, a controller or a menu.
 *
 * Coordinates are pixels in the framebuffer's own space: x grows right, y
 * grows down, and a position is always inside [0, width-1] x [0, height-1]
 * once a movement has been applied.  A report whose deltas would leave the
 * screen is clamped, not dropped: the operator moved the device, and the
 * pointer has to end up at the edge rather than stay where it was.
 */

struct pointer_state {
    int32_t x;
    int32_t y;
    bool visible;
    /* The left button's own state, so only a transition is an edge. */
    bool pressed;
    /* Accepted movements (a report with both deltas zero is not a movement). */
    uint32_t moves;
    /* Button edges: one press per transition down, one release per up. */
    uint32_t presses;
    uint32_t releases;
    /* Movements that ran past an edge and were clamped back onto the screen. */
    uint32_t clamped;
    /* Events refused because the state or the bounds were unusable. */
    uint32_t refused;
};

/* The drawable area a movement is clamped to. */
struct pointer_bounds {
    int32_t width;
    int32_t height;
};

/*
 * A rectangle a click can land in, and the value the caller wants back for it
 * (the console passes the menu entry's index).  A target with a non-positive
 * width or height can never be hit.
 */
struct pointer_target {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    int32_t value;
};

void pointer_state_reset(struct pointer_state *state, int32_t x, int32_t y);

/*
 * Apply one decoded report's signed deltas.  Returns true and counts the
 * movement when the pointer moved; a report with both deltas zero returns
 * false and changes nothing, because "no movement" is not a movement.  A
 * NULL state or unusable bounds is refused and counted.
 */
bool pointer_move(struct pointer_state *state, int32_t dx, int32_t dy,
                  struct pointer_bounds bounds);

/*
 * ITEM-470 (СРОЧНО-20): the pointer's speed.
 *
 * `pointer_move()` was the whole arithmetic until the owner's 3840x2160 run.
 * The automatic glyph scale is 3 there, so the interface is three times bigger
 * and the same hand movement that felt right on a small screen crawled across
 * the big one.  The console now passes its own `console_scale` - the very
 * number the `[FB]` record publishes - and a scaled movement multiplies each
 * delta by it.  A fast report (one whose magnitude reaches
 * POINTER_ACCEL_MIN_DELTA) is carried one further step per
 * POINTER_ACCEL_MIN_DELTA, capped at POINTER_ACCEL_MAX_GAIN, so a flick
 * crosses the screen while slow aiming stays exact.  At scale 1 the fast path
 * is off and `pointer_move_scaled()` is `pointer_move()` unchanged, which is
 * what keeps the small modes from becoming twitchy.
 */
#define POINTER_ACCEL_MIN_DELTA 8
#define POINTER_ACCEL_MAX_GAIN 3U

/*
 * ITEM-471 (СРОЧНО-21): a single report may not throw the arrow across the
 * screen.  With the glyph scale of 3 and the acceleration above, one report
 * the device can legitimately send moved the pointer by more than a thousand
 * pixels, so a flick put it in a corner in one step and there was nothing to
 * see between the two positions.  The scaled, accelerated delta is capped here
 * at a step an operator can follow; moving the device slowly still accumulates
 * the small deltas, and at scale 1 the whole path stays the identity.
 */
#define POINTER_MAX_STEP 128

/*
 * ITEM-472 (СРОЧНО-22): the operator's own gain on top of the glyph scale.
 *
 * The round-9/10 runs left the feel of the pointer a guess: the scale is
 * automatic and the acceleration is fixed, so the only way to make the arrow
 * keep up with the hand was to change the code.  The console owns a gain,
 * default 1, that multiplies the scaled delta before the single-report cap;
 * `PTR <n>` sets it and `PTR` shows it, and `[POINTER] gain=` publishes it so
 * the owner can pick a number in one run and read it back from the journal.
 * The default of 1 leaves the ITEM-471 arithmetic exactly as it was.
 */
#define POINTER_GAIN_MAX 64U

/* One report delta as the pointer applies it: the glyph scale, the operator's
   gain, the soft acceleration a fast, scaled movement gets, and the
   single-report cap.  Pure and integer.  A gain of 1 is the ITEM-471 path. */
int32_t pointer_apply_delta(int32_t delta, uint32_t scale, uint32_t gain);

/* One report delta as the pointer applies it: the glyph scale, the soft
   acceleration a fast, scaled movement gets, and the single-report cap.
   Pure and integer. */
int32_t pointer_scale_delta(int32_t delta, uint32_t scale);

/* `pointer_move()`, with the console's own glyph scale applied first. */
bool pointer_move_scaled(struct pointer_state *state, int32_t dx, int32_t dy,
                         struct pointer_bounds bounds, uint32_t scale);

/* `pointer_move()`, with the console's glyph scale and the operator's gain. */
bool pointer_move_gained(struct pointer_state *state, int32_t dx, int32_t dy,
                         struct pointer_bounds bounds, uint32_t scale,
                         uint32_t gain);

/*
 * ITEM-471: the sprite's top-left corner for a pointer position.
 *
 * A position is clamped to a single screen pixel, and the arrow is drawn from
 * that pixel.  At the right and bottom edges that used to draw only the first
 * column or row of the sprite - the loop broke out as soon as a column or row
 * ran past the edge - so a pointer at the edge looked like a one-pixel line.
 * The origin is the position itself while the whole sprite fits, and the last
 * origin at which it fits otherwise.  A screen narrower or shorter than the
 * sprite has no such origin and uses zero.
 */
int32_t pointer_sprite_origin(int32_t position, int32_t extent, int32_t sprite);

/* Record a button edge.  `down` is the button state of the report; only a
   transition is a press or a release, so a held button stays one press. */
bool pointer_button(struct pointer_state *state, bool down);

/*
 * The `value` of the first target containing (x, y), or -1 when none does.
 * A click on a shared boundary belongs to the earlier target, so the menu's
 * own entry order decides, exactly as the keyboard's index order does.
 */
int32_t pointer_hit_test(const struct pointer_target *targets, size_t count,
                         int32_t x, int32_t y);

#endif
