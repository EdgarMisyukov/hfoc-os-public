#ifndef HFOC_FRAMEBUFFER_H
#define HFOC_FRAMEBUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "limine_min.h"

bool framebuffer_init(struct limine_framebuffer *framebuffer);
bool framebuffer_ready(void);
/* ITEM-488: unbind the surface, so a refused framebuffer is never written. */
void framebuffer_release(void);
void framebuffer_clear(uint32_t colour);
void framebuffer_rect(uint64_t x, uint64_t y, uint64_t width, uint64_t height, uint32_t colour);
void framebuffer_char(uint64_t x, uint64_t y, char value, uint32_t colour, uint32_t scale);
void framebuffer_text(uint64_t x, uint64_t y, const char *text, uint32_t colour, uint32_t scale);
/*
 * ITEM-509 increment 3: the font by code point, and a cell that is never
 * silently empty.
 *
 * `framebuffer_char()` keeps the byte-at-a-time contract it had - the pinned
 * ASCII tables and every screen that draws with them are untouched.
 * `framebuffer_text()` decodes UTF-8 with the same catalog decoder (ITEM-509
 * increment 17): one code point is one cell, ASCII still goes through the byte
 * path, and a code point with no glyph draws the same visible marker and grows
 * the same counter as `framebuffer_char_codepoint()`.  The console likewise
 * decodes UTF-8 and paints code points through `framebuffer_char_codepoint()`;
 * both paths carry the item's "no silence" rule:
 *
 *  * `framebuffer_glyph_rows()` answers whether a code point has a glyph and,
 *    if it does, writes its 16 rows of 8 bits (bit 7 is the leftmost column).
 *    The space has a glyph: the empty cell a space is drawn as.  Control code
 *    points do not, but the console consumes them before it paints.
 *  * a code point with no glyph is not dropped: the cell carries a visible
 *    marker, `framebuffer_glyph_missing_count()` counts it and
 *    `framebuffer_glyph_missing_last()` names the last one, so the console can
 *    publish the record the operator needs to explain the mark on the screen.
 */
bool framebuffer_glyph_rows(uint32_t codepoint, uint8_t rows[16]);
/*
 * Paint one code point.  The answer says whether the font carried it: `false`
 * means the cell is the marker and `framebuffer_glyph_missing_count()` has just
 * grown, so the caller can publish the record without asking the font twice.
 */
bool framebuffer_char_codepoint(uint64_t x, uint64_t y, uint32_t codepoint,
                                uint32_t colour, uint64_t scale);
uint64_t framebuffer_glyph_missing_count(void);
uint32_t framebuffer_glyph_missing_last(void);
void framebuffer_u64(uint64_t x, uint64_t y, uint64_t value, uint32_t colour, uint32_t scale);
void framebuffer_hex64(uint64_t x, uint64_t y, uint64_t value, uint32_t colour, uint32_t scale);
/*
 * ITEM-457: scroll the rows in [top, height) up by `pixels`, filling the rows
 * that come in from the bottom with `fill_colour`.
 *
 * The call returns how many bytes it wrote to the framebuffer - the row copy
 * plus the fill - so the console can publish what its own scrolling cost
 * instead of guessing.  The copy is the whole row moved with the widest access
 * the row's alignment allows, not one pixel at a time; a scroll on a
 * 1920x1080 screen used to be about two million scalar reads and writes.
 */
uint64_t framebuffer_scroll_up(uint64_t top, uint64_t pixels, uint32_t fill_colour);
uint64_t framebuffer_width(void);
uint64_t framebuffer_height(void);
/* ITEM-461: pitch in bytes and bits per pixel of the selected mode. */
uint64_t framebuffer_pitch(void);
uint32_t framebuffer_bpp(void);

/*
 * ITEM-463/464: the bounded console mode the boot asks Limine for, and the
 * rule that decides whether the mode it got is a fallback.
 *
 * A text console gains nothing from 3840x2160 that it does not have at
 * 1024x768 - the body is the same characters - while a scroll of the 4K body
 * moves tens of megabytes and the owner sees the screen stutter.  ITEM-463
 * asked through a *global* `resolution:` line, which Limine does not apply to
 * the menu entry: the installed system still came up at 3840x2160.  ITEM-464
 * writes the request on the entry itself, in both the medium's configuration
 * and the installed configuration, and asks for the smallest standard mode
 * (1024x768).  When the firmware cannot provide it and hands back a larger
 * mode instead, `[FB] ... requested= actual= fallback=1` names that, so a slow
 * screen has a reason a reader can check rather than a guess.
 */
#define FRAMEBUFFER_REQUESTED_WIDTH 1024U
#define FRAMEBUFFER_REQUESTED_HEIGHT 768U

bool framebuffer_mode_is_bounded(uint64_t width, uint64_t height);

/*
 * ITEM-464: how many modes the firmware offered, and the one the selection
 * rule prefers.
 *
 * `framebuffer_best_mode()` walks the mode list the Limine framebuffer
 * response carries and returns the smallest mode at least as large as the
 * requested floor; when none reaches the floor it returns the smallest mode
 * offered, so a firmware with only small or only large modes still has a named
 * answer.  A 32-bpp mode is required because that is the only depth
 * framebuffer_init() accepts.  The functions answer false when the response
 * carried no list, and the caller then publishes the actual mode as the
 * choice.
 */
uint64_t framebuffer_mode_count(void);
bool framebuffer_best_mode(uint64_t *width, uint64_t *height);

/*
 * ITEM-464: wait for the write-combining framebuffer stores to reach the
 * device before a cost is read.
 *
 * The framebuffer is mapped write-combining, so the processor retires a paint
 * whose stores are still queued and a `rdtsc` around it measures the queueing,
 * not the memory traffic the operator waits for.  `sfence` orders the stores
 * and the read of the first pixel drains them, so the duration the console
 * records is the duration the frame took.  The read is volatile so it is not
 * elided.
 */
void framebuffer_sync(void);

/*
 * ITEM-466: the automatic font scale.
 *
 * A text console shows the same characters at every mode, so the mode should
 * only decide how large the glyphs are.  The scale is the largest integer in
 * [1, FRAMEBUFFER_FONT_SCALE_MAX] whose grid still keeps at least
 * FRAMEBUFFER_FONT_MIN_COLUMNS columns and FRAMEBUFFER_FONT_MIN_ROWS rows of
 * one 8x16-pixel cell; the cap keeps a 4K screen from turning into a dozen
 * giant glyphs.  ITEM-620 makes the cell the Debian Fixed 8x16 picture and
 * keeps the row floor at 30: the rule yields 1024x768 -> 1, 1280x800 -> 1,
 * 1366x768 -> 1, 1920x1080 -> 2 and 3840x2160 -> 3.  On the owner's
 * confirmed 1024x768 surface the Debian 8x16 picture is used at its native
 * size; no mode is enlarged just to fill rows.  The 80-column floor still
 * refuses a grid that would wrap protocol records.  It is pure, and the host
 * proof pins it.
 */
#define FRAMEBUFFER_FONT_CELL_WIDTH 8U
#define FRAMEBUFFER_FONT_CELL_HEIGHT 16U
#define FRAMEBUFFER_FONT_MIN_COLUMNS 80U
#define FRAMEBUFFER_FONT_MIN_ROWS 30U
#define FRAMEBUFFER_FONT_SCALE_MAX 3U
uint32_t framebuffer_font_scale(uint64_t width, uint64_t height);

/*
 * ITEM-466: the console's pixel image lives in a cacheable RAM shadow, and
 * video memory is written, never read.
 *
 * The 2026-09-21 hardware run measured `[CONSOLE] scroll ... max_us=16315`: a
 * single scrolled line read the body back out of uncached video memory, and
 * the main loop - which also polls the keyboard - waited for it, so "the
 * keyboard practically does not type".  The shadow is the console's own copy
 * of the surface in ordinary cacheable memory.  Every console write lands in
 * both; a scroll moves rows inside the shadow (a RAM copy) and then streams
 * the affected rows to video, so no video memory is ever read back.
 *
 * `framebuffer_shadow_init()` binds a caller-owned buffer (the boot passes
 * pages from the physical allocator through the HHDM; the host proof passes an
 * ordinary array) and returns false when it is smaller than the surface, in
 * which case the framebuffer keeps its read-back path rather than failing.
 */
uint64_t framebuffer_shadow_required_bytes(void);
bool framebuffer_shadow_init(void *memory, uint64_t bytes);
void framebuffer_shadow_disable(void);
bool framebuffer_shadow_ready(void);

/*
 * ITEM-486 (round 24): the shadow is the primary surface and the device is
 * written once per changed region.
 *
 * ITEM-466 made the RAM shadow the source a scroll reads, but every pixel was
 * still stored to both the shadow and write-combining video memory, so a glyph
 * was a burst of volatile stores.  The interactive path now draws into the
 * shadow alone, records the changed rectangle(s), and a single
 * `framebuffer_flush()` copies them to the device.  `framebuffer_sync()` calls
 * that flush before it drains, so every existing barrier keeps its meaning: the
 * pixels a burst drew are visible when the barrier returns.
 *
 * `framebuffer_set_clock()` binds the kernel's cycle counter for the
 * microsecond telemetry the loop record publishes; the host proofs leave it
 * unset and read the byte/rectangle counters, which need no clock.
 */
typedef uint64_t (*framebuffer_clock_fn)(void);
void framebuffer_set_clock(framebuffer_clock_fn clock);
void framebuffer_flush(void);
uint64_t framebuffer_shadow_draw_us(void);
uint64_t framebuffer_dirty_flush_us(void);
uint64_t framebuffer_dirty_bytes(void);
uint64_t framebuffer_dirty_rects(void);
uint64_t framebuffer_flush_count(void);

/*
 * ITEM-466 (round 5): the automatic mode choice.
 *
 * The owner's rule is "the mode is the monitor's native/preferred one, with a
 * reasonable fallback to the largest available when the preferred one is not
 * handed over".  The mode Limine actually programmed *is* the firmware's
 * preferred mode - it is the one the firmware and the panel agreed on - so the
 * preference is that mode's own entry in `modes[]`, found by width/height, not
 * `modes[0]`: the 2026-09-21 CI run proved the list is not ordered with the
 * active mode first (a 1024x768 EDID booted 1024x768 but `modes[0]` was
 * larger).  When the active mode is not in the list at all, the first 32-bpp
 * entry is named, and the largest 32-bpp mode is the fallback, with
 * `fallback=1` telling the reader the preference was not handed over.
 *
 * The rule is pure and takes the actual mode, so the host proof pins every
 * branch without a framebuffer.  `available_count` is how many 32-bpp modes
 * the firmware offered, which is what makes "there was no bounded mode" a
 * fact rather than a guess.
 */
struct framebuffer_mode_choice {
    uint64_t preferred_width;
    uint64_t preferred_height;
    uint64_t chosen_width;
    uint64_t chosen_height;
    uint64_t available_count;
    uint32_t fallback;
};

/*
 * ITEM-466: the live-response wrapper; false when the response carried no
 * 32-bpp mode at all (the caller then publishes the actual mode as the choice).
 */
bool framebuffer_describe_modes(uint64_t actual_width, uint64_t actual_height,
                                struct framebuffer_mode_choice *choice);

/*
 * ITEM-466: write the comma-separated list of 32-bpp modes the firmware
 * offered, bounded by `capacity`, and return how many were written.  The list
 * is what `[FB] available=` publishes; a list that does not fit ends with
 * `...`, so a truncated record names its own truncation.
 */
uint64_t framebuffer_available_text(char *output, size_t capacity);

/*
 * ITEM-464/466: the old "smallest bounded mode" rule, kept because slice 72
 * and the host proof pin the fallback shape it introduced.  ITEM-466 no longer
 * uses it for the boot choice; `framebuffer_choose_preferred()` above does.
 */
static inline bool framebuffer_choose_preferred(
    struct limine_video_mode *const *modes, uint64_t mode_count,
    uint64_t actual_width, uint64_t actual_height,
    struct framebuffer_mode_choice *choice) {
    const struct limine_video_mode *preferred = NULL;
    const struct limine_video_mode *first = NULL;
    const struct limine_video_mode *largest = NULL;

    if (modes == NULL || choice == NULL) {
        return false;
    }
    choice->available_count = 0U;
    for (uint64_t index = 0U; index < mode_count; ++index) {
        const struct limine_video_mode *mode = modes[index];
        uint64_t area;

        if (mode == NULL || mode->bpp != 32U) {
            continue;
        }
        ++choice->available_count;
        if (first == NULL) {
            first = mode;
        }
        area = mode->width * mode->height;
        if (largest == NULL || area > largest->width * largest->height) {
            largest = mode;
        }
        /* The active mode's own entry is the firmware's preference. */
        if (preferred == NULL && mode->width == actual_width &&
            mode->height == actual_height) {
            preferred = mode;
        }
    }
    if (preferred == NULL) {
        /* The active mode is not in the list: name the first 32-bpp mode. */
        preferred = first;
    }
    if (preferred == NULL) {
        return false;
    }
    choice->preferred_width = preferred->width;
    choice->preferred_height = preferred->height;
    if (actual_width == preferred->width &&
        actual_height == preferred->height) {
        choice->chosen_width = preferred->width;
        choice->chosen_height = preferred->height;
        choice->fallback = 0U;
    } else if (largest != NULL) {
        choice->chosen_width = largest->width;
        choice->chosen_height = largest->height;
        choice->fallback = 1U;
    } else {
        choice->chosen_width = actual_width;
        choice->chosen_height = actual_height;
        choice->fallback = 1U;
    }
    return true;
}

static inline bool framebuffer_choose_mode(
    struct limine_video_mode *const *modes, uint64_t mode_count,
    uint64_t *width, uint64_t *height) {
    const struct limine_video_mode *preferred = NULL;
    const struct limine_video_mode *smallest = NULL;

    if (modes == NULL || width == NULL || height == NULL) {
        return false;
    }
    for (uint64_t index = 0U; index < mode_count; ++index) {
        const struct limine_video_mode *mode = modes[index];
        uint64_t area;

        if (mode == NULL || mode->bpp != 32U) {
            continue;
        }
        area = mode->width * mode->height;
        if (smallest == NULL ||
            area < smallest->width * smallest->height) {
            smallest = mode;
        }
        if (mode->width >= FRAMEBUFFER_REQUESTED_WIDTH &&
            mode->height >= FRAMEBUFFER_REQUESTED_HEIGHT &&
            (preferred == NULL ||
             area < preferred->width * preferred->height)) {
            preferred = mode;
        }
    }
    if (preferred == NULL) {
        preferred = smallest;
    }
    if (preferred == NULL) {
        return false;
    }
    *width = preferred->width;
    *height = preferred->height;
    return true;
}

/*
 * ITEM-448: the pointer the mouse draws.
 *
 * The console paints text forward-only and keeps no shadow of the screen, so a
 * pointer drawn over it has to put back exactly what it covered.  These calls
 * own that: show saves the pixels under the sprite and paints it, hide puts
 * the saved pixels back, and a second show on a moved pointer restores the old
 * place before painting the new one.  The console hides the pointer around a
 * text write and shows it again afterwards, so a record never ends up with an
 * arrow baked into it.
 *
 * The sprite is clipped at the screen edges: a pointer at the last column
 * still draws its visible part, and the saved block is exactly the part that
 * was painted, so nothing outside the screen is ever written.
 */
#define FRAMEBUFFER_CURSOR_WIDTH 11U
#define FRAMEBUFFER_CURSOR_HEIGHT 16U

bool framebuffer_cursor_show(uint64_t x, uint64_t y);
void framebuffer_cursor_hide(void);
bool framebuffer_cursor_visible(void);
/*
 * ITEM-482 (round 20): the screen under the pointer changed, so the saved
 * block no longer describes it.  Forget the block instead of restoring stale
 * pixels at the old place; the next show saves fresh pixels.  The console
 * hides the pointer before it moves the body (so nothing is left behind),
 * and a move that skipped that hide forgets the block here.
 */
void framebuffer_cursor_invalidate(void);

#endif
