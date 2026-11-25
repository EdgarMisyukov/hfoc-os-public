#include "pointer.h"

/*
 * ITEM-448: the pointer's arithmetic, kept away from the driver, the console
 * and the framebuffer so the host proof can drive it directly.
 */

void pointer_state_reset(struct pointer_state *state, int32_t x, int32_t y)
{
    if (state == NULL) {
        return;
    }
    *state = (struct pointer_state){0};
    state->x = x;
    state->y = y;
    state->visible = true;
}

static int32_t pointer_clamp(int32_t value, int32_t limit)
{
    if (value < 0) {
        return 0;
    }
    if (value > limit) {
        return limit;
    }
    return value;
}

bool pointer_move(struct pointer_state *state, int32_t dx, int32_t dy,
                  struct pointer_bounds bounds)
{
    int32_t next_x;
    int32_t next_y;

    if (state == NULL || bounds.width <= 0 || bounds.height <= 0) {
        if (state != NULL) {
            ++state->refused;
        }
        return false;
    }
    if (dx == 0 && dy == 0) {
        return false;
    }
    next_x = state->x + dx;
    next_y = state->y + dy;
    {
        const int32_t max_x = bounds.width - 1;
        const int32_t max_y = bounds.height - 1;
        const int32_t clamped_x = pointer_clamp(next_x, max_x);
        const int32_t clamped_y = pointer_clamp(next_y, max_y);

        if (clamped_x != next_x || clamped_y != next_y) {
            ++state->clamped;
        }
        state->x = clamped_x;
        state->y = clamped_y;
    }
    ++state->moves;
    return true;
}

/*
 * ITEM-470 (СРОЧНО-20): one report delta as the pointer applies it.
 *
 * At scale 1 this is the identity, so the small modes keep the arithmetic the
 * pointer has always had.  Above scale 1 the delta is multiplied by the glyph
 * scale, and a fast delta - one at least POINTER_ACCEL_MIN_DELTA long - is
 * given a gain that grows with the delta up to POINTER_ACCEL_MAX_GAIN, so a
 * quick flick travels farther than the same distance moved slowly.  The
 * multiplication is 64-bit and saturated, so no delta a device can report can
 * make the result wrap before the bounds clamp it.
 *
 * ITEM-472 (СРОЧНО-22): the operator's own gain multiplies the scaled delta
 * before the cap, and a gain of 1 is exactly the ITEM-471 path.  At scale 1
 * and gain 1 the arithmetic is still the raw delta, so the small modes stay
 * the identity they have always been.
 */
int32_t pointer_apply_delta(int32_t delta, uint32_t scale, uint32_t gain)
{
    int64_t magnitude;
    int64_t scaled;
    uint32_t accel = 1U;

    if (gain == 0U) {
        gain = 1U;
    }
    if (gain > POINTER_GAIN_MAX) {
        gain = POINTER_GAIN_MAX;
    }
    if (scale <= 1U && gain <= 1U) {
        return delta;
    }
    if (scale == 0U) {
        scale = 1U;
    }
    magnitude = delta < 0 ? -(int64_t)delta : (int64_t)delta;
    if (magnitude >= POINTER_ACCEL_MIN_DELTA) {
        accel = 1U + (uint32_t)(magnitude / POINTER_ACCEL_MIN_DELTA);
        if (accel > POINTER_ACCEL_MAX_GAIN) {
            accel = POINTER_ACCEL_MAX_GAIN;
        }
    }
    scaled = (int64_t)delta * (int64_t)scale * (int64_t)gain * (int64_t)accel;
    /*
     * ITEM-471: cap one report before it reaches the screen.  Without this a
     * single fast report with scale 3 and gain 3 moved the arrow by up to
     * ~1140 px, which on the owner's 3840x2160 screen was a jump straight into
     * a corner - the image never showed the intermediate positions.
     */
    if (scaled > POINTER_MAX_STEP) {
        return POINTER_MAX_STEP;
    }
    if (scaled < -POINTER_MAX_STEP) {
        return -POINTER_MAX_STEP;
    }
    return (int32_t)scaled;
}

int32_t pointer_scale_delta(int32_t delta, uint32_t scale)
{
    return pointer_apply_delta(delta, scale, 1U);
}

int32_t pointer_sprite_origin(int32_t position, int32_t extent, int32_t sprite)
{
    if (position < 0) {
        position = 0;
    }
    if (sprite >= extent) {
        /* The screen cannot hold the whole sprite; draw from the top left. */
        return 0;
    }
    if (position > extent - sprite) {
        return extent - sprite;
    }
    return position;
}

bool pointer_move_scaled(struct pointer_state *state, int32_t dx, int32_t dy,
                         struct pointer_bounds bounds, uint32_t scale)
{
    return pointer_move_gained(state, dx, dy, bounds, scale, 1U);
}

bool pointer_move_gained(struct pointer_state *state, int32_t dx, int32_t dy,
                         struct pointer_bounds bounds, uint32_t scale,
                         uint32_t gain)
{
    return pointer_move(state, pointer_apply_delta(dx, scale, gain),
                        pointer_apply_delta(dy, scale, gain), bounds);
}

bool pointer_button(struct pointer_state *state, bool down)
{
    if (state == NULL) {
        return false;
    }
    if (down == state->pressed) {
        /* A held button is not a second press and a released one is not a
           second release. */
        return false;
    }
    state->pressed = down;
    if (down) {
        ++state->presses;
    } else {
        ++state->releases;
    }
    return true;
}

int32_t pointer_hit_test(const struct pointer_target *targets, size_t count,
                         int32_t x, int32_t y)
{
    size_t index;

    if (targets == NULL) {
        return -1;
    }
    for (index = 0U; index < count; ++index) {
        const struct pointer_target *target = &targets[index];

        if (target->width <= 0 || target->height <= 0) {
            continue;
        }
        if (x >= target->x && x < target->x + target->width &&
            y >= target->y && y < target->y + target->height) {
            return target->value;
        }
    }
    return -1;
}
