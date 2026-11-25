/*
 * ITEM-457: the console's scroll moves rows, and the screen after the move is
 * the screen before it, one row higher.
 *
 * The 2026-09-19 hardware report was "when the output reaches the bottom of
 * the screen, the refresh is very slow": framebuffer_scroll_up() copied the
 * body one pixel at a time - about two million scalar reads and writes for a
 * single scrolled text line on a 1920x1080 screen.  The move is now a row
 * copy, and this proof reads the surface back to require that it is a copy:
 * every surviving row carries exactly the pixels of the row that was below
 * it, the rows that came in from the bottom carry the fill colour, and
 * nothing above the scrolled region was painted at all.
 *
 * The real framebuffer.c is linked into this binary and pointed at a host
 * surface, so what is checked is the move the machine performs, not a copy of
 * it.  The cases below cover both alignments a 32-bpp row can have (a row
 * length that is a multiple of 8 and one that is only a multiple of 4), a
 * pitch with padding the move must not touch, the short screen whose whole
 * body is cleared, and the refusals that must not write anything.
 */
#include "framebuffer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SURFACE_WIDTH 24U
#define SURFACE_HEIGHT 12U
#define PADDED_PITCH_WORDS (SURFACE_WIDTH + 3U)
#define SURFACE_WORDS (PADDED_PITCH_WORDS * SURFACE_HEIGHT)

static uint32_t surface[SURFACE_WORDS];
/* framebuffer_init() keeps the pointer it is given, so the description has to
   outlive the call that sets it up. */
static struct limine_framebuffer framebuffer = {
    .address = surface,
    .width = SURFACE_WIDTH,
    .height = SURFACE_HEIGHT,
    .pitch = SURFACE_WIDTH * 4U,
    .bpp = 32U,
};

static int failures;
static unsigned int checks;

#define CHECK(condition, message) do { \
    ++checks; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        ++failures; \
    } \
} while (0)

static int init_surface(uint64_t width, uint64_t height, uint64_t pitch_bytes)
{
    framebuffer.address = surface;
    framebuffer.width = width;
    framebuffer.height = height;
    framebuffer.pitch = pitch_bytes;
    framebuffer.bpp = 32U;
    memset(surface, 0, sizeof(surface));
    return framebuffer_init(&framebuffer) ? 0 : 1;
}

static uint32_t pattern(uint64_t row, uint64_t column)
{
    return (uint32_t)(row * 1000U + column + 1U);
}

static void paint_pattern(uint64_t width, uint64_t height,
                          uint64_t pitch_words)
{
    for (uint64_t row = 0U; row < height; ++row) {
        for (uint64_t column = 0U; column < width; ++column) {
            surface[row * pitch_words + column] = pattern(row, column);
        }
    }
}

static bool row_matches(uint64_t row, uint64_t source_row, uint64_t width,
                        uint64_t pitch_words)
{
    for (uint64_t column = 0U; column < width; ++column) {
        if (surface[row * pitch_words + column] !=
            pattern(source_row, column)) {
            return false;
        }
    }
    return true;
}

static bool row_is(uint64_t row, uint64_t width, uint64_t pitch_words,
                   uint32_t colour)
{
    for (uint64_t column = 0U; column < width; ++column) {
        if (surface[row * pitch_words + column] != colour) {
            return false;
        }
    }
    return true;
}

/*
 * The ordinary move: the rows above `top` are not the scroll's business, the
 * rows below it are the ones that were under them, and the rows that came in
 * from the bottom are filled.  The return value is the whole write - the copy
 * and the fill - which is the number the console publishes.
 */
static void test_scroll_moves_rows(void)
{
    const uint64_t width = SURFACE_WIDTH;
    const uint64_t height = SURFACE_HEIGHT;
    const uint64_t pitch_words = width;
    const uint64_t pitch_bytes = pitch_words * 4U;
    const uint64_t top = 2U;
    const uint64_t pixels = 3U;
    const uint32_t fill = 0xaabbccddU;

    CHECK(init_surface(width, height, pitch_bytes) == 0,
          "the proof points the framebuffer at its surface");
    paint_pattern(width, height, pitch_words);
    const uint64_t written =
        framebuffer_scroll_up(top, pixels, fill);

    CHECK(written == (height - top) * width * 4U,
          "the scroll returns the bytes it wrote");
    for (uint64_t row = 0U; row < top; ++row) {
        CHECK(row_matches(row, row, width, pitch_words),
              "a row above the scrolled region is untouched");
    }
    for (uint64_t row = top; row + pixels < height; ++row) {
        CHECK(row_matches(row, row + pixels, width, pitch_words),
              "a scrolled row carries the pixels of the row below it");
    }
    for (uint64_t row = height - pixels; row < height; ++row) {
        CHECK(row_is(row, width, pitch_words, fill),
              "a row that came in from the bottom carries the fill colour");
    }
}

/*
 * A body shorter than the scroll: every row from `top` down is filled, the
 * rows above it are left alone, and the caller is told how much was written.
 */
static void test_scroll_clears_short_body(void)
{
    const uint64_t width = SURFACE_WIDTH;
    const uint64_t height = SURFACE_HEIGHT;
    const uint64_t pitch_words = width;
    const uint64_t pitch_bytes = pitch_words * 4U;
    const uint64_t top = 4U;
    const uint32_t fill = 0x11223344U;

    CHECK(init_surface(width, height, pitch_bytes) == 0,
          "the proof points the framebuffer at its surface");
    paint_pattern(width, height, pitch_words);
    const uint64_t written = framebuffer_scroll_up(top, height - top, fill);

    CHECK(written == (height - top) * width * 4U,
          "clearing the body returns the bytes it wrote");
    for (uint64_t row = 0U; row < top; ++row) {
        CHECK(row_matches(row, row, width, pitch_words),
              "a row above a cleared body is untouched");
    }
    for (uint64_t row = top; row < height; ++row) {
        CHECK(row_is(row, width, pitch_words, fill),
              "a cleared row carries the fill colour");
    }
}

/*
 * A pitch wider than the visible row: the move copies the visible pixels and
 * must leave the padding between rows exactly as it found it - the padding is
 * not screen content.
 */
static void test_scroll_leaves_row_padding(void)
{
    const uint64_t width = SURFACE_WIDTH;
    const uint64_t height = SURFACE_HEIGHT;
    const uint64_t pitch_words = PADDED_PITCH_WORDS;
    const uint64_t pitch_bytes = pitch_words * 4U;
    const uint32_t fill = 0x0f0f0f0fU;
    const uint32_t padding = 0xdeadbeefU;

    CHECK(init_surface(width, height, pitch_bytes) == 0,
          "the proof points the framebuffer at a padded surface");
    paint_pattern(width, height, pitch_words);
    for (uint64_t row = 0U; row < height; ++row) {
        for (uint64_t column = width; column < pitch_words; ++column) {
            surface[row * pitch_words + column] = padding;
        }
    }
    (void)framebuffer_scroll_up(1U, 2U, fill);
    for (uint64_t row = 0U; row < height; ++row) {
        for (uint64_t column = width; column < pitch_words; ++column) {
            CHECK(surface[row * pitch_words + column] == padding,
                  "row padding is not screen content and is not written");
        }
    }
    for (uint64_t row = 1U; row + 2U < height; ++row) {
        CHECK(row_matches(row, row + 2U, width, pitch_words),
              "a padded row still carries the row below it");
    }
    CHECK(row_is(height - 1U, width, pitch_words, fill),
          "a padded surface still fills the row that came in");
}

/*
 * A row whose length is only a multiple of 4 cannot be moved with 8-byte
 * stores; the move must still be a copy.  The width here is odd, so every
 * other row is 8-byte aligned and the rest are not.
 */
static void test_scroll_four_byte_rows(void)
{
    const uint64_t width = 5U;
    const uint64_t height = 6U;
    const uint64_t pitch_words = width;
    const uint64_t pitch_bytes = pitch_words * 4U;
    const uint32_t fill = 0x00ff00ffU;

    CHECK(init_surface(width, height, pitch_bytes) == 0,
          "the proof points the framebuffer at an odd-width surface");
    paint_pattern(width, height, pitch_words);
    (void)framebuffer_scroll_up(0U, 1U, fill);
    for (uint64_t row = 0U; row + 1U < height; ++row) {
        CHECK(row_matches(row, row + 1U, width, pitch_words),
              "a 4-byte-aligned row move still copies the row");
    }
    CHECK(row_is(height - 1U, width, pitch_words, fill),
          "a 4-byte-aligned row move still fills the row that came in");
}

/*
 * ITEM-466: with the RAM shadow bound, the scroll reads the shadow and only
 * writes video memory.  The proof points the surface at a pattern, paints it
 * through the API so both copies carry it, then poisons the video rows the
 * move would read from.  A scroll that read the device back would copy the
 * poison; requiring the real colours proves the source was the shadow.  The
 * shadow is checked to carry the same picture the screen does afterwards.
 */
static void test_scroll_uses_shadow_not_video(void)
{
    const uint64_t width = SURFACE_WIDTH;
    const uint64_t height = SURFACE_HEIGHT;
    const uint64_t pitch_words = width;
    const uint64_t pitch_bytes = pitch_words * 4U;
    const uint64_t top = 2U;
    const uint64_t pixels = 3U;
    const uint32_t fill = 0xffffffffU;
    const uint32_t poison = 0xdead0000U;
    static uint32_t shadow_buffer[SURFACE_WORDS];

    CHECK(init_surface(width, height, pitch_bytes) == 0,
          "the proof points the framebuffer at its surface");
    CHECK(framebuffer_shadow_required_bytes() == pitch_bytes * height,
          "the shadow's required size is the whole surface");
    CHECK(!framebuffer_shadow_init(shadow_buffer, 8U),
          "a buffer smaller than the surface is refused");
    CHECK(framebuffer_shadow_ready() == false,
          "a refused buffer leaves no shadow bound");
    CHECK(framebuffer_shadow_init(shadow_buffer, sizeof(shadow_buffer)),
          "a full-size buffer binds as the shadow");
    CHECK(framebuffer_shadow_ready(),
          "the bound shadow reports itself ready");

    for (uint64_t row = 0U; row < height; ++row) {
        framebuffer_rect(0U, row, width, 1U, (uint32_t)(0x100U + row));
    }
    /* Poison exactly the rows the move reads from. */
    for (uint64_t row = top; row < height; ++row) {
        for (uint64_t column = 0U; column < width; ++column) {
            surface[row * pitch_words + column] = poison;
        }
    }

    const uint64_t written = framebuffer_scroll_up(top, pixels, fill);

    CHECK(written == (height - top) * width * 4U,
          "the shadow scroll returns the bytes it wrote to video");
    for (uint64_t row = top; row + pixels < height; ++row) {
        CHECK(row_is(row, width, pitch_words,
                     (uint32_t)(0x100U + row + pixels)),
              "the scrolled row came from the shadow, not from the poisoned "
              "video source");
    }
    for (uint64_t row = height - pixels; row < height; ++row) {
        CHECK(row_is(row, width, pitch_words, fill),
              "the row that came in from the bottom carries the fill colour");
    }
    for (uint64_t row = top; row < height; ++row) {
        for (uint64_t column = 0U; column < width; ++column) {
            const uint32_t expected = row + pixels < height
                ? (uint32_t)(0x100U + row + pixels)
                : fill;
            CHECK(shadow_buffer[row * pitch_words + column] == expected,
                  "the shadow carries the same picture the screen does");
        }
    }
    framebuffer_shadow_disable();
    CHECK(!framebuffer_shadow_ready(),
          "disabling the shadow falls back to the read-back path");
}

/* A scroll that moves nothing must write nothing and report nothing. */
static void test_scroll_refusals(void)
{
    const uint64_t width = SURFACE_WIDTH;
    const uint64_t height = SURFACE_HEIGHT;
    const uint64_t pitch_words = width;
    const uint64_t pitch_bytes = pitch_words * 4U;

    CHECK(init_surface(width, height, pitch_bytes) == 0,
          "the proof points the framebuffer at its surface");
    paint_pattern(width, height, pitch_words);
    CHECK(framebuffer_scroll_up(0U, 0U, 0xffffffffU) == 0U,
          "a scroll of zero rows writes nothing");
    CHECK(framebuffer_scroll_up(height, 1U, 0xffffffffU) == 0U,
          "a scroll below the screen writes nothing");
    CHECK(framebuffer_scroll_up(height + 8U, 1U, 0xffffffffU) == 0U,
          "a scroll past the screen writes nothing");
    for (uint64_t row = 0U; row < height; ++row) {
        CHECK(row_matches(row, row, width, pitch_words),
              "a refused scroll left the surface exactly as it was");
    }
}

/*
 * ITEM-466: the mode decides the glyph scale, and the rule is pure.  The four
 * modes the owner named are pinned exactly, and the floor is required to hold
 * for every mode the rule can return.  ITEM-610 raised the readable floor to
 * 80 columns and ITEM-620 made the cell 8x16 while keeping the 30-row
 * floor, so the rule follows the Debian picture instead of the old 6x9 cell
 * without enlarging a 768-line mode to a 16x32 cell.
 */
static void test_font_scale_rule(void)
{
    CHECK(framebuffer_font_scale(1024U, 768U) == 1U,
          "1024x768 gets a 1x cell");
    CHECK(framebuffer_font_scale(1366U, 768U) == 1U,
          "1366x768 gets a 1x cell");
    CHECK(framebuffer_font_scale(1920U, 1080U) == 2U,
          "1920x1080 gets a 2x cell");
    CHECK(framebuffer_font_scale(3840U, 2160U) == 3U,
          "3840x2160 gets a 3x cell");
    CHECK(framebuffer_font_scale(640U, 480U) == 1U,
          "a mode too small for the grid still gets the 1x floor");
    CHECK(framebuffer_font_scale(0U, 0U) == 1U,
          "an absent mode never scales below 1x");
    for (uint64_t width = 320U; width <= 3840U; width += 160U) {
        for (uint64_t height = 200U; height <= 2160U; height += 120U) {
            const uint32_t scale = framebuffer_font_scale(width, height);
            const uint64_t columns = width / (8U * scale);
            const uint64_t rows = height / (16U * scale);

            CHECK(scale >= 1U && scale <= 3U,
                  "the scale stays in [1,3]");
            if (columns >= 80U && rows >= 30U) {
                /* The largest passing scale was chosen: one step up would
                   have to break the floor. */
                CHECK(scale == 3U ||
                          width / (8U * (scale + 1U)) < 80U ||
                          height / (16U * (scale + 1U)) < 30U,
                      "the chosen scale is the largest one that keeps the "
                      "grid floor");
            }
        }
    }
}

/*
 * ITEM-482 (round 20): a body move forgets the saved block.
 *
 * The pointer keeps the pixels it covered so hide() can put them back.  Once
 * the body has moved, those pixels describe a place that no longer exists, so
 * the driver must forget them instead of restoring them over the new content.
 * The console hides the pointer before the move (proven in console-host.c);
 * this case pins the framebuffer's own guarantee that a move invalidates the
 * block, and that invalidate() itself writes nothing.
 */
static void test_cursor_invalidate_forgets_the_saved_block(void)
{
    const uint64_t width = SURFACE_WIDTH;
    const uint64_t height = SURFACE_HEIGHT;
    const uint64_t pitch_words = width;
    const uint64_t pitch_bytes = pitch_words * 4U;
    uint32_t after_show[SURFACE_WORDS];

    CHECK(init_surface(width, height, pitch_bytes) == 0,
          "the cursor proof points the framebuffer at its surface");
    paint_pattern(width, height, pitch_words);
    CHECK(framebuffer_cursor_show(4U, 3U) == true,
          "the pointer paints over the surface");
    CHECK(framebuffer_cursor_visible(),
          "the pointer is visible after it is shown");
    memcpy(after_show, surface, sizeof(after_show));

    framebuffer_cursor_invalidate();
    CHECK(!framebuffer_cursor_visible(),
          "invalidate forgets the pointer");
    CHECK(memcmp(after_show, surface, sizeof(after_show)) == 0,
          "invalidate writes no pixels by itself");

    framebuffer_cursor_hide();
    CHECK(memcmp(after_show, surface, sizeof(after_show)) == 0,
          "a hide after invalidate does not restore the stale block");
}

/*
 * ITEM-620: the pointer's fill edge is the console background, not the old
 * `#07111f` blue.  On a black surface every painted pointer pixel must be the
 * white outline or black; any other colour is read back by name.
 */
static void test_cursor_dark_edge_is_black(void)
{
    const uint64_t pitch_bytes = SURFACE_WIDTH * 4U;
    bool only_white_or_black = true;

    CHECK(init_surface(SURFACE_WIDTH, SURFACE_HEIGHT, pitch_bytes) == 0,
          "the pointer colour proof points the framebuffer at its surface");
    memset(surface, 0, sizeof(surface));
    CHECK(framebuffer_cursor_show(0U, 0U) == true,
          "the pointer paints on the black console");
    for (uint64_t row = 0U; row < SURFACE_HEIGHT; ++row) {
        for (uint64_t column = 0U; column < FRAMEBUFFER_CURSOR_WIDTH;
             ++column) {
            const uint32_t colour = surface[row * SURFACE_WIDTH + column];

            if (colour != 0x00000000U && colour != 0x00FFFFFFU) {
                only_white_or_black = false;
            }
        }
    }
    CHECK(only_white_or_black,
          "the pointer fill edge is black, not the old dark blue");
    framebuffer_cursor_hide();
}

static void test_scroll_forgets_the_pointer(void)
{
    const uint64_t width = SURFACE_WIDTH;
    const uint64_t height = SURFACE_HEIGHT;
    const uint64_t pitch_words = width;
    const uint64_t pitch_bytes = pitch_words * 4U;

    CHECK(init_surface(width, height, pitch_bytes) == 0,
          "the scroll proof points the framebuffer at its surface");
    paint_pattern(width, height, pitch_words);
    CHECK(framebuffer_cursor_show(2U, 1U) == true,
          "the pointer paints before the scroll");
    CHECK(framebuffer_cursor_visible(),
          "the pointer is visible before the scroll");
    (void)framebuffer_scroll_up(0U, 2U, 0x00000000U);
    CHECK(!framebuffer_cursor_visible(),
          "a body move forgets the pointer");
}

/*
 * ITEM-486 (round 24): the interactive path draws into the RAM shadow and the
 * device is written by one batched flush.
 *
 * The motivating report was `kbd_draw_us` in the tens of milliseconds while
 * `kbd_sync_us` was zero: the draw itself paid a volatile store per pixel.  The
 * proof below requires that a paint changes the shadow but not the surface
 * until a flush, that one flush copies the changed rectangle, and that the
 * `framebuffer_sync()` barrier every existing call site uses performs that
 * flush - so "batched" is a property of the real module, not a promise.
 */
static void test_shadow_paint_waits_for_flush(void)
{
    const uint64_t width = SURFACE_WIDTH;
    const uint64_t height = SURFACE_HEIGHT;
    const uint64_t pitch_words = width;
    static uint32_t shadow_buffer[SURFACE_WORDS];
    uint32_t before[SURFACE_WORDS];
    uint64_t flushes_before;
    uint64_t bytes_before;

    CHECK(init_surface(width, height, pitch_words * 4U) == 0,
          "the paint proof points the framebuffer at its surface");
    CHECK(framebuffer_shadow_init(shadow_buffer, sizeof(shadow_buffer)),
          "the paint proof binds the shadow");
    memcpy(before, surface, sizeof(before));
    flushes_before = framebuffer_flush_count();
    bytes_before = framebuffer_dirty_bytes();

    framebuffer_rect(3U, 2U, 4U, 3U, 0x00ff00ffU);

    CHECK(memcmp(before, surface, sizeof(before)) == 0,
          "a shadow paint does not write the device before the flush");
    CHECK(shadow_buffer[2U * width + 3U] == 0x00ff00ffU,
          "a shadow paint lands in the RAM copy");
    CHECK(framebuffer_flush_count() == flushes_before,
          "a paint alone is not a flush");

    framebuffer_flush();

    CHECK(surface[2U * width + 3U] == 0x00ff00ffU,
          "the flush writes the changed pixel to the device");
    CHECK(surface[3U * width + 5U] == 0x00ff00ffU,
          "the flush writes the whole changed rectangle");
    CHECK(framebuffer_flush_count() == flushes_before + 1U,
          "one flush is counted as one flush");
    CHECK(framebuffer_dirty_bytes() > bytes_before,
          "the flushed bytes are counted");
    CHECK(framebuffer_dirty_rects() >= 1U,
          "the flushed rectangle is counted");
    framebuffer_shadow_disable();
}

static void test_sync_is_the_batch_flush(void)
{
    const uint64_t width = SURFACE_WIDTH;
    const uint64_t height = SURFACE_HEIGHT;
    static uint32_t shadow_buffer[SURFACE_WORDS];
    uint64_t flushes_before;

    CHECK(init_surface(width, height, width * 4U) == 0,
          "the barrier proof points the framebuffer at its surface");
    CHECK(framebuffer_shadow_init(shadow_buffer, sizeof(shadow_buffer)),
          "the barrier proof binds the shadow");
    flushes_before = framebuffer_flush_count();

    framebuffer_rect(7U, 5U, 2U, 2U, 0x0000ff00U);
    CHECK(surface[5U * width + 7U] != 0x0000ff00U,
          "the barrier's paint is still only in the shadow");

    framebuffer_sync();

    CHECK(surface[5U * width + 7U] == 0x0000ff00U,
          "the existing barrier flushes the shadow to the device");
    CHECK(framebuffer_flush_count() == flushes_before + 1U,
          "the barrier is the one batched flush");
    CHECK(memcmp(surface, shadow_buffer, sizeof(surface)) == 0,
          "after the barrier the two copies are identical");
    framebuffer_shadow_disable();
}

static void test_scattered_paints_flush_completely(void)
{
    const uint64_t width = SURFACE_WIDTH;
    const uint64_t height = SURFACE_HEIGHT;
    static uint32_t shadow_buffer[SURFACE_WORDS];
    uint64_t flush_before;

    CHECK(init_surface(width, height, width * 4U) == 0,
          "the coalescing proof points the framebuffer at its surface");
    CHECK(framebuffer_shadow_init(shadow_buffer, sizeof(shadow_buffer)),
          "the coalescing proof binds the shadow");
    flush_before = framebuffer_flush_count();

    /* More separated rectangles than the pool holds, so the merge path runs. */
    for (uint64_t index = 0U; index < 24U; ++index) {
        const uint64_t x = (index * 7U) % width;
        const uint64_t y = (index * 3U) % height;

        framebuffer_rect(x, y, 1U, 1U, (uint32_t)(0x010000U + index));
    }
    framebuffer_flush();

    CHECK(framebuffer_flush_count() == flush_before + 1U,
          "many paints still make one flush");
    CHECK(memcmp(surface, shadow_buffer, sizeof(surface)) == 0,
          "the flush copies every changed pixel, however many rectangles");
    /* A second flush with nothing dirty must write nothing. */
    flush_before = framebuffer_flush_count();
    framebuffer_flush();
    CHECK(framebuffer_flush_count() == flush_before,
          "a flush with no dirty rectangle does not run");
    framebuffer_shadow_disable();
}

static void test_disable_flushes_pending_paint(void)
{
    const uint64_t width = SURFACE_WIDTH;
    const uint64_t height = SURFACE_HEIGHT;
    static uint32_t shadow_buffer[SURFACE_WORDS];

    CHECK(init_surface(width, height, width * 4U) == 0,
          "the disable proof points the framebuffer at its surface");
    CHECK(framebuffer_shadow_init(shadow_buffer, sizeof(shadow_buffer)),
          "the disable proof binds the shadow");
    framebuffer_rect(1U, 1U, 2U, 2U, 0x00abcdefU);
    CHECK(surface[1U * width + 1U] != 0x00abcdefU,
          "the pending paint is still only in the shadow");

    framebuffer_shadow_disable();

    CHECK(!framebuffer_shadow_ready(),
          "disabling the shadow clears the binding");
    CHECK(surface[1U * width + 1U] == 0x00abcdefU,
          "disabling the shadow flushes the pending paint first");
}

/*
 * ITEM-486 (round 24): the software pointer keeps working over the shadow-only
 * path.  show() saves the shadow pixels and paints the arrow into the shadow;
 * hide() restores them; a flush after each step makes both the arrow and its
 * absence visible.  The final surface must be exactly the picture the arrow
 * covered, which is the contract's "no ghost cursor, no lost pixels".
 */
static void test_cursor_round_trip_over_shadow(void)
{
    const uint64_t width = SURFACE_WIDTH;
    const uint64_t height = SURFACE_HEIGHT;
    static uint32_t shadow_buffer[SURFACE_WORDS];
    uint32_t clean[SURFACE_WORDS];

    CHECK(init_surface(width, height, width * 4U) == 0,
          "the cursor-over-shadow proof points the framebuffer at its surface");
    CHECK(framebuffer_shadow_init(shadow_buffer, sizeof(shadow_buffer)),
          "the cursor-over-shadow proof binds the shadow");
    for (uint64_t row = 0U; row < height; ++row) {
        framebuffer_rect(0U, row, width, 1U, pattern(row, 0U));
    }
    framebuffer_flush();
    memcpy(clean, surface, sizeof(clean));

    CHECK(framebuffer_cursor_show(5U, 4U) == true,
          "the pointer paints over the shadow");
    framebuffer_flush();
    CHECK(memcmp(surface, shadow_buffer, sizeof(surface)) == 0,
          "the flushed device matches the shadow with the arrow drawn");
    CHECK(memcmp(surface, clean, sizeof(clean)) != 0,
          "the arrow changed the picture");

    framebuffer_cursor_hide();
    framebuffer_flush();
    CHECK(memcmp(surface, clean, sizeof(clean)) == 0,
          "hiding the pointer restores every saved pixel exactly");
    CHECK(memcmp(surface, shadow_buffer, sizeof(surface)) == 0,
          "the device and the shadow agree after the pointer is hidden");
    framebuffer_shadow_disable();
}

static uint64_t host_fake_clock_value;
static uint64_t host_fake_clock(void)
{
    host_fake_clock_value += 10U;
    return host_fake_clock_value;
}

static void test_shadow_and_flush_are_timed(void)
{
    const uint64_t width = SURFACE_WIDTH;
    const uint64_t height = SURFACE_HEIGHT;
    static uint32_t shadow_buffer[SURFACE_WORDS];
    uint64_t draw_before;
    uint64_t flush_us_before;

    CHECK(init_surface(width, height, width * 4U) == 0,
          "the telemetry proof points the framebuffer at its surface");
    CHECK(framebuffer_shadow_init(shadow_buffer, sizeof(shadow_buffer)),
          "the telemetry proof binds the shadow");
    framebuffer_set_clock(host_fake_clock);
    draw_before = framebuffer_shadow_draw_us();
    flush_us_before = framebuffer_dirty_flush_us();

    framebuffer_rect(2U, 2U, 3U, 3U, 0x00fedcbaU);
    framebuffer_flush();

    CHECK(framebuffer_shadow_draw_us() > draw_before,
          "the shadow paint time is measured");
    CHECK(framebuffer_dirty_flush_us() > flush_us_before,
          "the dirty flush time is measured");
    framebuffer_set_clock(NULL);
    framebuffer_shadow_disable();
}

int main(void)
{
    test_scroll_moves_rows();
    test_scroll_clears_short_body();
    test_scroll_leaves_row_padding();
    test_scroll_four_byte_rows();
    test_scroll_refusals();
    test_scroll_uses_shadow_not_video();
    test_cursor_invalidate_forgets_the_saved_block();
    test_cursor_dark_edge_is_black();
    test_scroll_forgets_the_pointer();
    test_font_scale_rule();
    test_shadow_paint_waits_for_flush();
    test_sync_is_the_batch_flush();
    test_scattered_paints_flush_completely();
    test_disable_flushes_pending_paint();
    test_shadow_and_flush_are_timed();
    test_cursor_round_trip_over_shadow();

    if (failures != 0) {
        fprintf(stderr, "framebuffer scroll host: %u checks, %d failures\n",
                checks, failures);
        return 1;
    }
    printf("framebuffer scroll host: %u checks, 0 failures\n", checks);
    return 0;
}
