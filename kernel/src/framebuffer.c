#include "framebuffer.h"
#include "pointer.h"
#include <stddef.h>
#include <string.h>

#include "appliance_language.h"

static struct limine_framebuffer *active_framebuffer;

/*
 * ITEM-466: the cacheable RAM shadow.  `shadow_pitch` is the same stride as
 * the surface, `shadow_bytes` its whole size, and `shadow_ready` says the
 * buffer is at least that large.  The framebuffer never owns the memory: the
 * caller binds it and keeps the lifetime.
 */
static uint8_t *shadow_memory;
static uint64_t shadow_bytes;
static uint64_t shadow_pitch;
static bool shadow_ready;

static volatile uint8_t *framebuffer_base(void) {
    return (volatile uint8_t *)(uintptr_t)active_framebuffer->address;
}

/*
 * ITEM-486 (round 24): the dirty rectangles a shadow-only paint leaves behind.
 *
 * Every pixel the console draws lands in the RAM shadow and widens one of a
 * small pool of rectangles; `framebuffer_flush()` copies exactly those
 * rectangles to video memory and clears the pool.  A pool rather than one
 * bounding box keeps a status-bar update at the top and a typed character at
 * the bottom from turning into a full-screen copy.  When the pool is full the
 * two rectangles whose union adds the least area are merged, so the cost of a
 * paint is bounded by a fixed number of row copies however many pixels it
 * touched.
 */
#define FRAMEBUFFER_DIRTY_RECT_MAX 8U
/* Pixels within this many columns/rows of a rectangle join it, so the glyphs
   of one line coalesce instead of opening a rectangle each. */
#define FRAMEBUFFER_DIRTY_SLACK 24U

struct framebuffer_dirty_rect {
    uint64_t x0;
    uint64_t y0;
    uint64_t x1; /* exclusive */
    uint64_t y1; /* exclusive */
};

static struct framebuffer_dirty_rect dirty_rects[FRAMEBUFFER_DIRTY_RECT_MAX];
static uint32_t dirty_rect_count;

/* ITEM-486: the microsecond telemetry the loop record publishes.  The clock is
   bound by the boot; the host proofs leave it NULL and read the counters. */
static framebuffer_clock_fn fb_clock;
static uint64_t fb_shadow_draw_us_total;
static uint64_t fb_dirty_flush_us_total;
static uint64_t fb_dirty_bytes_total;
static uint64_t fb_dirty_rects_total;
static uint64_t fb_flush_count_total;

/*
 * ITEM-486: time the outermost drawing primitive so a glyph is one
 * measurement, not one per lit pixel.  `framebuffer_char()` wraps its calls to
 * `framebuffer_rect()`, and the cursor wraps its `pixel()` calls, so the
 * nesting depth picks the entry point.
 */
static unsigned int fb_draw_depth;
static bool fb_draw_timed;
static uint64_t fb_draw_started;

static void fb_draw_enter(void) {
    if (fb_draw_depth == 0U) {
        fb_draw_timed = shadow_ready && fb_clock != NULL;
        if (fb_draw_timed) {
            fb_draw_started = fb_clock();
        }
    }
    ++fb_draw_depth;
}

static void fb_draw_leave(void) {
    if (fb_draw_depth == 0U) {
        return;
    }
    --fb_draw_depth;
    if (fb_draw_depth == 0U && fb_draw_timed) {
        fb_shadow_draw_us_total += fb_clock() - fb_draw_started;
        fb_draw_timed = false;
    }
}

static uint64_t dirty_union_area(const struct framebuffer_dirty_rect *left,
                                 const struct framebuffer_dirty_rect *right) {
    const uint64_t x0 = left->x0 < right->x0 ? left->x0 : right->x0;
    const uint64_t y0 = left->y0 < right->y0 ? left->y0 : right->y0;
    const uint64_t x1 = left->x1 > right->x1 ? left->x1 : right->x1;
    const uint64_t y1 = left->y1 > right->y1 ? left->y1 : right->y1;

    return (x1 - x0) * (y1 - y0);
}

static void dirty_mark(uint64_t x, uint64_t y) {
    uint32_t index;

    /* A pixel already inside a rectangle changes nothing. */
    for (index = 0U; index < dirty_rect_count; ++index) {
        const struct framebuffer_dirty_rect *rect = &dirty_rects[index];

        if (x >= rect->x0 && x < rect->x1 && y >= rect->y0 && y < rect->y1) {
            return;
        }
    }
    /* A pixel near a rectangle joins it, so one line stays one rectangle. */
    for (index = 0U; index < dirty_rect_count; ++index) {
        struct framebuffer_dirty_rect *rect = &dirty_rects[index];

        if (x + FRAMEBUFFER_DIRTY_SLACK >= rect->x0 &&
            x <= rect->x1 + FRAMEBUFFER_DIRTY_SLACK &&
            y + FRAMEBUFFER_DIRTY_SLACK >= rect->y0 &&
            y <= rect->y1 + FRAMEBUFFER_DIRTY_SLACK) {
            if (x < rect->x0) {
                rect->x0 = x;
            }
            if (y < rect->y0) {
                rect->y0 = y;
            }
            if (x + 1U > rect->x1) {
                rect->x1 = x + 1U;
            }
            if (y + 1U > rect->y1) {
                rect->y1 = y + 1U;
            }
            return;
        }
    }
    if (dirty_rect_count < FRAMEBUFFER_DIRTY_RECT_MAX) {
        dirty_rects[dirty_rect_count].x0 = x;
        dirty_rects[dirty_rect_count].y0 = y;
        dirty_rects[dirty_rect_count].x1 = x + 1U;
        dirty_rects[dirty_rect_count].y1 = y + 1U;
        ++dirty_rect_count;
        return;
    }
    /*
     * The pool is full: merge the pair whose union is smallest, which frees one
     * slot for the new pixel.  A bounding box is therefore never forced until
     * every pair has already been merged once.
     */
    {
        uint32_t best_left = 0U;
        uint32_t best_right = 1U;
        uint64_t best_area = dirty_union_area(&dirty_rects[0U],
                                              &dirty_rects[1U]);

        for (index = 0U; index < dirty_rect_count; ++index) {
            for (uint32_t other = index + 1U; other < dirty_rect_count;
                 ++other) {
                const uint64_t area =
                    dirty_union_area(&dirty_rects[index], &dirty_rects[other]);

                if (area < best_area) {
                    best_area = area;
                    best_left = index;
                    best_right = other;
                }
            }
        }
        {
            struct framebuffer_dirty_rect *merged = &dirty_rects[best_left];
            const struct framebuffer_dirty_rect *gone = &dirty_rects[best_right];

            if (gone->x0 < merged->x0) {
                merged->x0 = gone->x0;
            }
            if (gone->y0 < merged->y0) {
                merged->y0 = gone->y0;
            }
            if (gone->x1 > merged->x1) {
                merged->x1 = gone->x1;
            }
            if (gone->y1 > merged->y1) {
                merged->y1 = gone->y1;
            }
        }
        dirty_rects[best_right] = dirty_rects[--dirty_rect_count];
    }
    dirty_mark(x, y);
}

/*
 * ITEM-620: the console font is the Debian console-setup Fixed 8x16
 * picture (CyrSlav-Fixed16.psf.gz, PSF1, 256 glyphs x 16 bytes, Unicode
 * table), the face /etc/default/console-setup names on the reference
 * Debian console.  Debian maps Latin and Cyrillic homoglyphs to one
 * picture (A/А, B/В, E/Е, ...); the same rows are carried here.
 *
 * Glyph assets: the console-setup package states that console fonts are
 * public domain by nature; the 8x16 source BDF additionally carries the
 * Sony/BIZNET/Dmitry permission notice.  Both statements travel with the
 * derived rows in notices/console-font-fixed8x16.txt; this is in-tree
 * project data, not a third_party import.
 */
struct font8x16_glyph {
    uint32_t codepoint;
    uint8_t rows[16];
};

static const struct font8x16_glyph font8x16_glyphs[] = {
    { 0x0020U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U } },
    { 0x0021U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x00U, 0x08U, 0x08U, 0x00U, 0x00U } },
    { 0x0022U, { 0x00U, 0x00U, 0x22U, 0x22U, 0x22U, 0x22U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U } },
    { 0x0023U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x12U, 0x12U, 0x12U, 0x7EU, 0x24U, 0x24U, 0x7EU, 0x48U, 0x48U, 0x48U, 0x00U, 0x00U } },
    { 0x0024U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x08U, 0x3EU, 0x49U, 0x48U, 0x38U, 0x0EU, 0x09U, 0x49U, 0x3EU, 0x08U, 0x00U, 0x00U } },
    { 0x0025U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x31U, 0x4AU, 0x4AU, 0x34U, 0x08U, 0x08U, 0x16U, 0x29U, 0x29U, 0x46U, 0x00U, 0x00U } },
    { 0x0026U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x1CU, 0x22U, 0x22U, 0x14U, 0x18U, 0x29U, 0x45U, 0x42U, 0x46U, 0x39U, 0x00U, 0x00U } },
    { 0x0027U, { 0x00U, 0x00U, 0x08U, 0x08U, 0x08U, 0x08U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U } },
    { 0x0028U, { 0x00U, 0x00U, 0x00U, 0x04U, 0x08U, 0x08U, 0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x08U, 0x08U, 0x04U, 0x00U } },
    { 0x0029U, { 0x00U, 0x00U, 0x00U, 0x20U, 0x10U, 0x10U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x10U, 0x10U, 0x20U, 0x00U } },
    { 0x002AU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x08U, 0x49U, 0x2AU, 0x1CU, 0x2AU, 0x49U, 0x08U, 0x00U, 0x00U, 0x00U } },
    { 0x002BU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x08U, 0x08U, 0x08U, 0x7FU, 0x08U, 0x08U, 0x08U, 0x00U, 0x00U, 0x00U } },
    { 0x002CU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x18U, 0x08U, 0x08U, 0x10U } },
    { 0x002DU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x3CU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U } },
    { 0x002EU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x18U, 0x18U, 0x00U, 0x00U } },
    { 0x002FU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x02U, 0x02U, 0x04U, 0x08U, 0x08U, 0x10U, 0x10U, 0x20U, 0x40U, 0x40U, 0x00U, 0x00U } },
    { 0x0030U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x18U, 0x24U, 0x42U, 0x46U, 0x4AU, 0x52U, 0x62U, 0x42U, 0x24U, 0x18U, 0x00U, 0x00U } },
    { 0x0031U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x08U, 0x18U, 0x28U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x3EU, 0x00U, 0x00U } },
    { 0x0032U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x3CU, 0x42U, 0x42U, 0x02U, 0x0CU, 0x10U, 0x20U, 0x40U, 0x40U, 0x7EU, 0x00U, 0x00U } },
    { 0x0033U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x3CU, 0x42U, 0x42U, 0x02U, 0x1CU, 0x02U, 0x02U, 0x42U, 0x42U, 0x3CU, 0x00U, 0x00U } },
    { 0x0034U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x04U, 0x0CU, 0x14U, 0x24U, 0x44U, 0x44U, 0x7EU, 0x04U, 0x04U, 0x04U, 0x00U, 0x00U } },
    { 0x0035U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x7EU, 0x40U, 0x40U, 0x40U, 0x7CU, 0x02U, 0x02U, 0x02U, 0x42U, 0x3CU, 0x00U, 0x00U } },
    { 0x0036U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x1CU, 0x20U, 0x40U, 0x40U, 0x7CU, 0x42U, 0x42U, 0x42U, 0x42U, 0x3CU, 0x00U, 0x00U } },
    { 0x0037U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x7EU, 0x02U, 0x02U, 0x04U, 0x04U, 0x04U, 0x08U, 0x08U, 0x08U, 0x08U, 0x00U, 0x00U } },
    { 0x0038U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x3CU, 0x42U, 0x42U, 0x42U, 0x3CU, 0x42U, 0x42U, 0x42U, 0x42U, 0x3CU, 0x00U, 0x00U } },
    { 0x0039U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x3CU, 0x42U, 0x42U, 0x42U, 0x3EU, 0x02U, 0x02U, 0x02U, 0x04U, 0x38U, 0x00U, 0x00U } },
    { 0x003AU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x18U, 0x18U, 0x00U, 0x00U, 0x00U, 0x18U, 0x18U, 0x00U, 0x00U, 0x00U } },
    { 0x003BU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x18U, 0x18U, 0x00U, 0x00U, 0x00U, 0x18U, 0x08U, 0x08U, 0x10U, 0x00U } },
    { 0x003CU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U, 0x04U, 0x08U, 0x10U, 0x20U, 0x10U, 0x08U, 0x04U, 0x02U, 0x00U, 0x00U } },
    { 0x003DU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x7EU, 0x00U, 0x00U, 0x00U, 0x7EU, 0x00U, 0x00U, 0x00U, 0x00U } },
    { 0x003EU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x40U, 0x20U, 0x10U, 0x08U, 0x04U, 0x08U, 0x10U, 0x20U, 0x40U, 0x00U, 0x00U } },
    { 0x003FU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x3CU, 0x42U, 0x42U, 0x02U, 0x04U, 0x08U, 0x08U, 0x00U, 0x08U, 0x08U, 0x00U, 0x00U } },
    { 0x0040U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x1CU, 0x22U, 0x4AU, 0x56U, 0x52U, 0x52U, 0x52U, 0x4EU, 0x20U, 0x1EU, 0x00U, 0x00U } },
    { 0x0041U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x18U, 0x24U, 0x24U, 0x42U, 0x42U, 0x7EU, 0x42U, 0x42U, 0x42U, 0x42U, 0x00U, 0x00U } },
    { 0x0042U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x7CU, 0x42U, 0x42U, 0x42U, 0x7CU, 0x42U, 0x42U, 0x42U, 0x42U, 0x7CU, 0x00U, 0x00U } },
    { 0x0043U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x3CU, 0x42U, 0x42U, 0x40U, 0x40U, 0x40U, 0x40U, 0x42U, 0x42U, 0x3CU, 0x00U, 0x00U } },
    { 0x0044U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x78U, 0x44U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x44U, 0x78U, 0x00U, 0x00U } },
    { 0x0045U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x7EU, 0x40U, 0x40U, 0x40U, 0x7CU, 0x40U, 0x40U, 0x40U, 0x40U, 0x7EU, 0x00U, 0x00U } },
    { 0x0046U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x7EU, 0x40U, 0x40U, 0x40U, 0x7CU, 0x40U, 0x40U, 0x40U, 0x40U, 0x40U, 0x00U, 0x00U } },
    { 0x0047U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x3CU, 0x42U, 0x42U, 0x40U, 0x40U, 0x4EU, 0x42U, 0x42U, 0x46U, 0x3AU, 0x00U, 0x00U } },
    { 0x0048U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x42U, 0x42U, 0x42U, 0x7EU, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x00U, 0x00U } },
    { 0x0049U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x3EU, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x3EU, 0x00U, 0x00U } },
    { 0x004AU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x1FU, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x44U, 0x44U, 0x38U, 0x00U, 0x00U } },
    { 0x004BU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x44U, 0x48U, 0x50U, 0x60U, 0x60U, 0x50U, 0x48U, 0x44U, 0x42U, 0x00U, 0x00U } },
    { 0x004CU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x40U, 0x40U, 0x40U, 0x40U, 0x40U, 0x40U, 0x40U, 0x40U, 0x40U, 0x7EU, 0x00U, 0x00U } },
    { 0x004DU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x42U, 0x66U, 0x66U, 0x5AU, 0x5AU, 0x42U, 0x42U, 0x42U, 0x42U, 0x00U, 0x00U } },
    { 0x004EU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x62U, 0x62U, 0x52U, 0x52U, 0x4AU, 0x4AU, 0x46U, 0x46U, 0x42U, 0x00U, 0x00U } },
    { 0x004FU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x3CU, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x3CU, 0x00U, 0x00U } },
    { 0x0050U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x7CU, 0x42U, 0x42U, 0x42U, 0x7CU, 0x40U, 0x40U, 0x40U, 0x40U, 0x40U, 0x00U, 0x00U } },
    { 0x0051U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x3CU, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x5AU, 0x66U, 0x3CU, 0x03U, 0x00U } },
    { 0x0052U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x7CU, 0x42U, 0x42U, 0x42U, 0x7CU, 0x48U, 0x44U, 0x44U, 0x42U, 0x42U, 0x00U, 0x00U } },
    { 0x0053U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x3CU, 0x42U, 0x42U, 0x40U, 0x30U, 0x0CU, 0x02U, 0x42U, 0x42U, 0x3CU, 0x00U, 0x00U } },
    { 0x0054U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x7FU, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x00U, 0x00U } },
    { 0x0055U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x3CU, 0x00U, 0x00U } },
    { 0x0056U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x41U, 0x41U, 0x41U, 0x22U, 0x22U, 0x22U, 0x14U, 0x14U, 0x08U, 0x08U, 0x00U, 0x00U } },
    { 0x0057U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x42U, 0x42U, 0x42U, 0x5AU, 0x5AU, 0x66U, 0x66U, 0x42U, 0x42U, 0x00U, 0x00U } },
    { 0x0058U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x42U, 0x24U, 0x24U, 0x18U, 0x18U, 0x24U, 0x24U, 0x42U, 0x42U, 0x00U, 0x00U } },
    { 0x0059U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x41U, 0x41U, 0x22U, 0x22U, 0x14U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x00U, 0x00U } },
    { 0x005AU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x7EU, 0x02U, 0x02U, 0x04U, 0x08U, 0x10U, 0x20U, 0x40U, 0x40U, 0x7EU, 0x00U, 0x00U } },
    { 0x005BU, { 0x00U, 0x00U, 0x00U, 0x0EU, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x0EU, 0x00U } },
    { 0x005CU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x40U, 0x40U, 0x20U, 0x10U, 0x10U, 0x08U, 0x08U, 0x04U, 0x02U, 0x02U, 0x00U, 0x00U } },
    { 0x005DU, { 0x00U, 0x00U, 0x00U, 0x70U, 0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x70U, 0x00U } },
    { 0x005EU, { 0x00U, 0x00U, 0x18U, 0x24U, 0x42U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U } },
    { 0x005FU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x7FU, 0x00U } },
    { 0x0060U, { 0x00U, 0x20U, 0x10U, 0x08U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U } },
    { 0x0061U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x3CU, 0x42U, 0x02U, 0x3EU, 0x42U, 0x42U, 0x46U, 0x3AU, 0x00U, 0x00U } },
    { 0x0062U, { 0x00U, 0x00U, 0x00U, 0x40U, 0x40U, 0x40U, 0x5CU, 0x62U, 0x42U, 0x42U, 0x42U, 0x42U, 0x62U, 0x5CU, 0x00U, 0x00U } },
    { 0x0063U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x3CU, 0x42U, 0x40U, 0x40U, 0x40U, 0x40U, 0x42U, 0x3CU, 0x00U, 0x00U } },
    { 0x0064U, { 0x00U, 0x00U, 0x00U, 0x02U, 0x02U, 0x02U, 0x3AU, 0x46U, 0x42U, 0x42U, 0x42U, 0x42U, 0x46U, 0x3AU, 0x00U, 0x00U } },
    { 0x0065U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x3CU, 0x42U, 0x42U, 0x7EU, 0x40U, 0x40U, 0x42U, 0x3CU, 0x00U, 0x00U } },
    { 0x0066U, { 0x00U, 0x00U, 0x00U, 0x0CU, 0x10U, 0x10U, 0x10U, 0x7CU, 0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x00U, 0x00U } },
    { 0x0067U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U, 0x3AU, 0x44U, 0x44U, 0x44U, 0x38U, 0x20U, 0x3CU, 0x42U, 0x42U, 0x3CU } },
    { 0x0068U, { 0x00U, 0x00U, 0x00U, 0x40U, 0x40U, 0x40U, 0x5CU, 0x62U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x00U, 0x00U } },
    { 0x0069U, { 0x00U, 0x00U, 0x00U, 0x08U, 0x08U, 0x00U, 0x18U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x3EU, 0x00U, 0x00U } },
    { 0x006AU, { 0x00U, 0x00U, 0x00U, 0x04U, 0x04U, 0x00U, 0x0CU, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x48U, 0x30U } },
    { 0x006BU, { 0x00U, 0x00U, 0x00U, 0x40U, 0x40U, 0x40U, 0x44U, 0x48U, 0x50U, 0x60U, 0x50U, 0x48U, 0x44U, 0x42U, 0x00U, 0x00U } },
    { 0x006CU, { 0x00U, 0x00U, 0x00U, 0x18U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x3EU, 0x00U, 0x00U } },
    { 0x006DU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x76U, 0x49U, 0x49U, 0x49U, 0x49U, 0x49U, 0x49U, 0x49U, 0x00U, 0x00U } },
    { 0x006EU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x5CU, 0x62U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x00U, 0x00U } },
    { 0x006FU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x3CU, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x3CU, 0x00U, 0x00U } },
    { 0x0070U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x5CU, 0x62U, 0x42U, 0x42U, 0x42U, 0x42U, 0x62U, 0x5CU, 0x40U, 0x40U } },
    { 0x0071U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x3AU, 0x46U, 0x42U, 0x42U, 0x42U, 0x42U, 0x46U, 0x3AU, 0x02U, 0x02U } },
    { 0x0072U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x5CU, 0x62U, 0x42U, 0x40U, 0x40U, 0x40U, 0x40U, 0x40U, 0x00U, 0x00U } },
    { 0x0073U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x3CU, 0x42U, 0x40U, 0x30U, 0x0CU, 0x02U, 0x42U, 0x3CU, 0x00U, 0x00U } },
    { 0x0074U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x10U, 0x10U, 0x10U, 0x7CU, 0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x0CU, 0x00U, 0x00U } },
    { 0x0075U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x46U, 0x3AU, 0x00U, 0x00U } },
    { 0x0076U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x42U, 0x42U, 0x24U, 0x24U, 0x24U, 0x18U, 0x18U, 0x00U, 0x00U } },
    { 0x0077U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x41U, 0x49U, 0x49U, 0x49U, 0x49U, 0x49U, 0x49U, 0x36U, 0x00U, 0x00U } },
    { 0x0078U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x42U, 0x24U, 0x18U, 0x18U, 0x24U, 0x42U, 0x42U, 0x00U, 0x00U } },
    { 0x0079U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x26U, 0x1AU, 0x02U, 0x02U, 0x3CU } },
    { 0x007AU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x7EU, 0x02U, 0x04U, 0x08U, 0x10U, 0x20U, 0x40U, 0x7EU, 0x00U, 0x00U } },
    { 0x007BU, { 0x00U, 0x00U, 0x00U, 0x0CU, 0x10U, 0x10U, 0x08U, 0x08U, 0x10U, 0x20U, 0x10U, 0x08U, 0x08U, 0x10U, 0x10U, 0x0CU } },
    { 0x007CU, { 0x00U, 0x00U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U } },
    { 0x007DU, { 0x00U, 0x00U, 0x00U, 0x30U, 0x08U, 0x08U, 0x10U, 0x10U, 0x08U, 0x04U, 0x08U, 0x10U, 0x10U, 0x08U, 0x08U, 0x30U } },
    { 0x007EU, { 0x00U, 0x00U, 0x00U, 0x31U, 0x49U, 0x46U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U } },
    { 0x0410U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x18U, 0x24U, 0x24U, 0x42U, 0x42U, 0x7EU, 0x42U, 0x42U, 0x42U, 0x42U, 0x00U, 0x00U } },
    { 0x0411U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x7CU, 0x40U, 0x40U, 0x40U, 0x7CU, 0x42U, 0x42U, 0x42U, 0x42U, 0x7CU, 0x00U, 0x00U } },
    { 0x0412U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x7CU, 0x42U, 0x42U, 0x42U, 0x7CU, 0x42U, 0x42U, 0x42U, 0x42U, 0x7CU, 0x00U, 0x00U } },
    { 0x0413U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x7EU, 0x42U, 0x40U, 0x40U, 0x40U, 0x40U, 0x40U, 0x40U, 0x40U, 0x40U, 0x00U, 0x00U } },
    { 0x0414U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x0EU, 0x12U, 0x12U, 0x12U, 0x22U, 0x22U, 0x22U, 0x42U, 0x42U, 0xFFU, 0x81U, 0x81U } },
    { 0x0415U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x7EU, 0x40U, 0x40U, 0x40U, 0x7CU, 0x40U, 0x40U, 0x40U, 0x40U, 0x7EU, 0x00U, 0x00U } },
    { 0x0416U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x49U, 0x2AU, 0x2AU, 0x1CU, 0x1CU, 0x2AU, 0x2AU, 0x49U, 0x49U, 0x00U, 0x00U } },
    { 0x0417U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x3CU, 0x42U, 0x02U, 0x02U, 0x3CU, 0x04U, 0x02U, 0x02U, 0x42U, 0x3CU, 0x00U, 0x00U } },
    { 0x0418U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x46U, 0x46U, 0x4AU, 0x4AU, 0x52U, 0x52U, 0x62U, 0x62U, 0x42U, 0x00U, 0x00U } },
    { 0x0419U, { 0x00U, 0x24U, 0x18U, 0x00U, 0x42U, 0x46U, 0x46U, 0x4AU, 0x4AU, 0x52U, 0x52U, 0x62U, 0x62U, 0x42U, 0x00U, 0x00U } },
    { 0x041AU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x44U, 0x48U, 0x50U, 0x60U, 0x60U, 0x50U, 0x48U, 0x44U, 0x42U, 0x00U, 0x00U } },
    { 0x041BU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x1EU, 0x12U, 0x12U, 0x12U, 0x12U, 0x12U, 0x12U, 0x22U, 0x22U, 0x42U, 0x00U, 0x00U } },
    { 0x041CU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x42U, 0x66U, 0x66U, 0x5AU, 0x5AU, 0x42U, 0x42U, 0x42U, 0x42U, 0x00U, 0x00U } },
    { 0x041DU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x42U, 0x42U, 0x42U, 0x7EU, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x00U, 0x00U } },
    { 0x041EU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x3CU, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x3CU, 0x00U, 0x00U } },
    { 0x041FU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x7FU, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x22U, 0x00U, 0x00U } },
    { 0x0420U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x7CU, 0x42U, 0x42U, 0x42U, 0x7CU, 0x40U, 0x40U, 0x40U, 0x40U, 0x40U, 0x00U, 0x00U } },
    { 0x0421U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x3CU, 0x42U, 0x42U, 0x40U, 0x40U, 0x40U, 0x40U, 0x42U, 0x42U, 0x3CU, 0x00U, 0x00U } },
    { 0x0422U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x7FU, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x00U, 0x00U } },
    { 0x0423U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x41U, 0x41U, 0x22U, 0x22U, 0x14U, 0x14U, 0x08U, 0x08U, 0x10U, 0x30U, 0x00U, 0x00U } },
    { 0x0424U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x7FU, 0x08U, 0x3EU, 0x49U, 0x49U, 0x49U, 0x49U, 0x3EU, 0x08U, 0x7FU, 0x00U, 0x00U } },
    { 0x0425U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x42U, 0x24U, 0x24U, 0x18U, 0x18U, 0x24U, 0x24U, 0x42U, 0x42U, 0x00U, 0x00U } },
    { 0x0426U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x7FU, 0x01U, 0x01U } },
    { 0x0427U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x46U, 0x3AU, 0x02U, 0x02U, 0x02U, 0x00U, 0x00U } },
    { 0x0428U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x49U, 0x49U, 0x49U, 0x49U, 0x49U, 0x49U, 0x49U, 0x49U, 0x7FU, 0x00U, 0x00U } },
    { 0x0429U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x92U, 0x92U, 0x92U, 0x92U, 0x92U, 0x92U, 0x92U, 0x92U, 0x92U, 0xFFU, 0x01U, 0x01U } },
    { 0x042AU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x70U, 0x10U, 0x10U, 0x10U, 0x1EU, 0x11U, 0x11U, 0x11U, 0x11U, 0x1EU, 0x00U, 0x00U } },
    { 0x042BU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x42U, 0x42U, 0x42U, 0x72U, 0x4AU, 0x4AU, 0x4AU, 0x4AU, 0x72U, 0x00U, 0x00U } },
    { 0x042CU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x40U, 0x40U, 0x40U, 0x40U, 0x7CU, 0x42U, 0x42U, 0x42U, 0x42U, 0x7CU, 0x00U, 0x00U } },
    { 0x042DU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x38U, 0x44U, 0x02U, 0x02U, 0x3EU, 0x02U, 0x02U, 0x02U, 0x44U, 0x38U, 0x00U, 0x00U } },
    { 0x042EU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x4CU, 0x52U, 0x52U, 0x52U, 0x72U, 0x52U, 0x52U, 0x52U, 0x52U, 0x4CU, 0x00U, 0x00U } },
    { 0x042FU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x3EU, 0x42U, 0x42U, 0x42U, 0x3EU, 0x12U, 0x22U, 0x22U, 0x42U, 0x42U, 0x00U, 0x00U } },
    { 0x0430U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x3CU, 0x42U, 0x02U, 0x3EU, 0x42U, 0x42U, 0x46U, 0x3AU, 0x00U, 0x00U } },
    { 0x0431U, { 0x00U, 0x00U, 0x02U, 0x1CU, 0x20U, 0x40U, 0x7CU, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x3CU, 0x00U, 0x00U } },
    { 0x0432U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x7CU, 0x42U, 0x42U, 0x7CU, 0x42U, 0x42U, 0x42U, 0x7CU, 0x00U, 0x00U } },
    { 0x0433U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x7EU, 0x40U, 0x40U, 0x40U, 0x40U, 0x40U, 0x40U, 0x40U, 0x00U, 0x00U } },
    { 0x0434U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x1EU, 0x12U, 0x22U, 0x22U, 0x42U, 0x42U, 0x42U, 0x7FU, 0x41U, 0x00U } },
    { 0x0435U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x3CU, 0x42U, 0x42U, 0x7EU, 0x40U, 0x40U, 0x42U, 0x3CU, 0x00U, 0x00U } },
    { 0x0436U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x49U, 0x2AU, 0x1CU, 0x1CU, 0x2AU, 0x49U, 0x49U, 0x00U, 0x00U } },
    { 0x0437U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x3CU, 0x42U, 0x02U, 0x3CU, 0x04U, 0x02U, 0x42U, 0x3CU, 0x00U, 0x00U } },
    { 0x0438U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x46U, 0x46U, 0x4AU, 0x4AU, 0x52U, 0x52U, 0x62U, 0x62U, 0x00U, 0x00U } },
    { 0x0439U, { 0x00U, 0x00U, 0x24U, 0x18U, 0x00U, 0x00U, 0x46U, 0x46U, 0x4AU, 0x4AU, 0x52U, 0x52U, 0x62U, 0x62U, 0x00U, 0x00U } },
    { 0x043AU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x46U, 0x48U, 0x50U, 0x60U, 0x50U, 0x48U, 0x44U, 0x42U, 0x00U, 0x00U } },
    { 0x043BU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x1EU, 0x12U, 0x12U, 0x12U, 0x12U, 0x22U, 0x22U, 0x42U, 0x00U, 0x00U } },
    { 0x043CU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x66U, 0x66U, 0x5AU, 0x5AU, 0x42U, 0x42U, 0x42U, 0x00U, 0x00U } },
    { 0x043DU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x42U, 0x42U, 0x7EU, 0x42U, 0x42U, 0x42U, 0x42U, 0x00U, 0x00U } },
    { 0x043EU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x3CU, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x3CU, 0x00U, 0x00U } },
    { 0x043FU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x7EU, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x00U, 0x00U } },
    { 0x0440U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x5CU, 0x62U, 0x42U, 0x42U, 0x42U, 0x42U, 0x62U, 0x5CU, 0x40U, 0x40U } },
    { 0x0441U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x3CU, 0x42U, 0x40U, 0x40U, 0x40U, 0x40U, 0x42U, 0x3CU, 0x00U, 0x00U } },
    { 0x0442U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x7FU, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x08U, 0x00U, 0x00U } },
    { 0x0443U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x26U, 0x1AU, 0x02U, 0x02U, 0x3CU } },
    { 0x0444U, { 0x00U, 0x00U, 0x00U, 0x08U, 0x08U, 0x08U, 0x3EU, 0x49U, 0x49U, 0x49U, 0x49U, 0x49U, 0x49U, 0x3EU, 0x08U, 0x08U } },
    { 0x0445U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x42U, 0x24U, 0x18U, 0x18U, 0x24U, 0x42U, 0x42U, 0x00U, 0x00U } },
    { 0x0446U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x42U, 0x7FU, 0x01U, 0x01U } },
    { 0x0447U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x42U, 0x42U, 0x42U, 0x46U, 0x3AU, 0x02U, 0x02U, 0x00U, 0x00U } },
    { 0x0448U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x49U, 0x49U, 0x49U, 0x49U, 0x49U, 0x49U, 0x7FU, 0x00U, 0x00U } },
    { 0x0449U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x92U, 0x92U, 0x92U, 0x92U, 0x92U, 0x92U, 0x92U, 0xFFU, 0x01U, 0x01U } },
    { 0x044AU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x70U, 0x10U, 0x10U, 0x1EU, 0x11U, 0x11U, 0x11U, 0x1EU, 0x00U, 0x00U } },
    { 0x044BU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x42U, 0x42U, 0x42U, 0x72U, 0x4AU, 0x4AU, 0x4AU, 0x72U, 0x00U, 0x00U } },
    { 0x044CU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x40U, 0x40U, 0x40U, 0x7CU, 0x42U, 0x42U, 0x42U, 0x7CU, 0x00U, 0x00U } },
    { 0x044DU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x38U, 0x44U, 0x02U, 0x3EU, 0x02U, 0x02U, 0x44U, 0x38U, 0x00U, 0x00U } },
    { 0x044EU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x4CU, 0x52U, 0x52U, 0x72U, 0x52U, 0x52U, 0x52U, 0x4CU, 0x00U, 0x00U } },
    { 0x044FU, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x3EU, 0x42U, 0x42U, 0x42U, 0x3EU, 0x12U, 0x22U, 0x42U, 0x00U, 0x00U } },
    { 0x0401U, { 0x24U, 0x00U, 0x00U, 0x00U, 0x7EU, 0x40U, 0x40U, 0x40U, 0x7CU, 0x40U, 0x40U, 0x40U, 0x40U, 0x7EU, 0x00U, 0x00U } },
    { 0x0451U, { 0x24U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x3CU, 0x42U, 0x42U, 0x7EU, 0x40U, 0x40U, 0x42U, 0x3CU, 0x00U, 0x00U } },
    { 0x00ABU, { 0x00U, 0x00U, 0x00U, 0x18U, 0x30U, 0x60U, 0x30U, 0x18U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U } },
    { 0x00BBU, { 0x00U, 0x00U, 0x00U, 0x18U, 0x0CU, 0x06U, 0x0CU, 0x18U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U } },
    { 0x2014U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0xFFU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U } },
    { 0x2026U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x49U, 0x00U, 0x00U } },
    { 0x2116U, { 0x00U, 0x00U, 0x00U, 0x00U, 0x92U, 0x95U, 0x95U, 0xD2U, 0xD0U, 0xB7U, 0xB0U, 0x90U, 0x90U, 0x90U, 0x00U, 0x00U } },
};

#define FONT8X16_GLYPH_COUNT \
    (sizeof(font8x16_glyphs) / sizeof(font8x16_glyphs[0]))

/*
 * ITEM-509 increment 3: the cell a code point with no glyph is drawn as.
 * It is a boxed dot - unlike any letter or sign in either alphabet - so a
 * reader sees "this character was not drawn", and the console publishes
 * the record that names the code point.
 */
static const uint8_t glyph_missing_marker[16] = {
    0x00U, 0x00U, 0x7FU, 0x41U, 0x55U, 0x41U, 0x7FU, 0x00U,
    0x00U, 0x7FU, 0x41U, 0x55U, 0x41U, 0x7FU, 0x00U, 0x00U
};

static uint64_t glyph_missing_count;
static uint32_t glyph_missing_last;

static const uint8_t cursor_sprite[FRAMEBUFFER_CURSOR_HEIGHT]
                                 [FRAMEBUFFER_CURSOR_WIDTH] = {
    {1,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,2,1,1,1,1,1,1},
    {1,2,2,1,2,2,1,0,0,0,0},
    {1,2,1,0,1,2,2,1,0,0,0},
    {1,1,0,0,1,2,2,1,0,0,0},
    {1,0,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,1,2,2,1,0,0}
};

#define FRAMEBUFFER_CURSOR_WHITE 0xffffffU
#define FRAMEBUFFER_CURSOR_DARK 0x000000U

static uint32_t cursor_saved[FRAMEBUFFER_CURSOR_WIDTH *
                             FRAMEBUFFER_CURSOR_HEIGHT];
static uint64_t cursor_saved_x;
static uint64_t cursor_saved_y;
static uint64_t cursor_saved_width;
static uint64_t cursor_saved_height;
static bool cursor_visible;


/*
 * The ASCII run is the first 95 entries, so the hot byte path needs no scan;
 * the handful of Cyrillic and punctuation rows are found by a bounded walk of
 * a table that is small enough that the branch predictor beats a binary search.
 */
static const struct font8x16_glyph *font8x16_find(uint32_t codepoint) {
    size_t index;

    if (codepoint >= 0x20U && codepoint <= 0x7EU) {
        return &font8x16_glyphs[codepoint - 0x20U];
    }
    for (index = 95U; index < FONT8X16_GLYPH_COUNT; ++index) {
        if (font8x16_glyphs[index].codepoint == codepoint) {
            return &font8x16_glyphs[index];
        }
    }
    return NULL;
}

static void glyph_rows(char character, uint8_t rows[16]) {
    const struct font8x16_glyph *glyph =
        font8x16_find((unsigned char)character);
    unsigned int row;

    for (row = 0U; row < 16U; ++row) {
        rows[row] = glyph != NULL ? glyph->rows[row] : 0U;
    }
}

/*
 * ITEM-509 increment 3: which ASCII bytes have a glyph.  Every printable
 * ASCII byte has one; the space is the empty cell it is drawn as.  The
 * host proof walks every printable byte and fails if this predicate and
 * the painted cell ever disagree, so the two cannot drift apart silently.
 */
static bool ascii_glyph_present(unsigned char character) {
    return character >= (unsigned char)0x20U &&
           character <= (unsigned char)0x7EU;
}

/*
 * ITEM-509 increment 3: the font by code point.  `false` means the code
 * point has no glyph - the caller draws the visible marker and says so.
 */
static bool glyph_rows_codepoint(uint32_t codepoint, uint8_t rows[16]) {
    const struct font8x16_glyph *glyph = font8x16_find(codepoint);
    unsigned int row;

    for (row = 0U; row < 16U; ++row) {
        rows[row] = glyph != NULL ? glyph->rows[row] : 0U;
    }
    if (codepoint < 0x80U) {
        return ascii_glyph_present((unsigned char)codepoint);
    }
    return glyph != NULL;
}

bool framebuffer_glyph_rows(uint32_t codepoint, uint8_t rows[16]) {
    if (rows == NULL) {
        return false;
    }
    return glyph_rows_codepoint(codepoint, rows);
}

uint64_t framebuffer_glyph_missing_count(void) {
    return glyph_missing_count;
}

uint32_t framebuffer_glyph_missing_last(void) {
    return glyph_missing_last;
}

static void pixel(uint64_t x, uint64_t y, uint32_t colour) {
    if (active_framebuffer == NULL || x >= active_framebuffer->width || y >= active_framebuffer->height) {
        return;
    }
    /*
     * ITEM-486 (round 24): with the shadow bound, the pixel lands in cacheable
     * RAM alone and the rectangle it widened is what the next flush copies to
     * video.  A glyph is therefore a handful of RAM stores instead of a burst
     * of volatile write-combining stores, and the visible device write happens
     * once per batch at `framebuffer_flush()`.  Without a shadow the old direct
     * store is the only path.
     */
    if (shadow_ready) {
        ((uint32_t *)shadow_memory)[y * (shadow_pitch / 4U) + x] = colour;
        dirty_mark(x, y);
        return;
    }
    ((volatile uint32_t *)active_framebuffer->address)[
        y * (active_framebuffer->pitch / 4U) + x] = colour;
}

bool framebuffer_init(struct limine_framebuffer *framebuffer) {
    if (framebuffer == NULL || framebuffer->address == NULL || framebuffer->bpp != 32) {
        return false;
    }
    active_framebuffer = framebuffer;
    /* ITEM-509 increment 3: each bound surface starts with a clean count. */
    glyph_missing_count = 0U;
    glyph_missing_last = 0U;
    return true;
}

bool framebuffer_ready(void) {
    return active_framebuffer != NULL;
}

/*
 * ITEM-488: give the framebuffer back unbound.
 *
 * The display abstraction checks a firmware framebuffer before anything is
 * drawn into it, and a framebuffer that fails those checks must not be written
 * by the console either - a screen painted into the wrong place is worse than a
 * screen that stays as the firmware left it.  This is the one call that makes
 * "refused" mean refused; the text path takes over from here.
 */
void framebuffer_release(void) {
    active_framebuffer = NULL;
    cursor_visible = false;
}

void framebuffer_clear(uint32_t colour) {
    if (active_framebuffer != NULL) {
        /* A cleared screen has nothing left to put back: the saved block
           describes pixels that no longer exist, so the pointer forgets it
           instead of restoring them later. */
        cursor_visible = false;
        framebuffer_rect(0, 0, active_framebuffer->width, active_framebuffer->height, colour);
    }
}

void framebuffer_rect(uint64_t x, uint64_t y, uint64_t width, uint64_t height, uint32_t colour) {
    if (active_framebuffer == NULL) {
        return;
    }
    fb_draw_enter();
    for (uint64_t row = 0; row < height && y + row < active_framebuffer->height; ++row) {
        for (uint64_t column = 0; column < width && x + column < active_framebuffer->width; ++column) {
            pixel(x + column, y + row, colour);
        }
    }
    fb_draw_leave();
}

void framebuffer_char(uint64_t x, uint64_t y, char value, uint32_t colour, uint32_t scale) {
    uint8_t rows[16];
    glyph_rows(value, rows);
    fb_draw_enter();
    for (unsigned int row = 0; row < 16; ++row) {
        for (unsigned int column = 0; column < 8; ++column) {
            if ((rows[row] & (1U << (7U - column))) != 0) {
                framebuffer_rect(x + column * scale, y + row * scale, scale, scale, colour);
            }
        }
    }
    fb_draw_leave();
}

bool framebuffer_char_codepoint(uint64_t x, uint64_t y, uint32_t codepoint,
                                uint32_t colour, uint64_t scale) {
    uint8_t rows[16];
    const bool present = glyph_rows_codepoint(codepoint, rows);

    if (!present) {
        /*
         * ITEM-509 increment 3: a code point the font does not carry is never
         * drawn as nothing.  The cell gets the marker, the counter names how
         * many were seen and the last one is kept so the console can publish
         * the record that explains the mark on the screen.
         */
        ++glyph_missing_count;
        glyph_missing_last = codepoint;
        for (unsigned int row = 0; row < 16; ++row) {
            rows[row] = glyph_missing_marker[row];
        }
    }
    fb_draw_enter();
    for (unsigned int row = 0; row < 16; ++row) {
        for (unsigned int column = 0; column < 8; ++column) {
            if ((rows[row] & (1U << (7U - column))) != 0) {
                framebuffer_rect(x + column * scale, y + row * scale, scale, scale, colour);
            }
        }
    }
    fb_draw_leave();
    return present;
}

/*
 * ITEM-509 increment 17: one code point, one cell.
 *
 * The ASCII range keeps the byte path above, so every Latin glyph is the same
 * picture it always was.  A code point outside it is looked up in the
 * increment 3 tables; a code point with no glyph draws the visible marker and
 * is counted, exactly as the console path does, so a Russian screen cannot
 * disappear into blank cells.
 */
static void framebuffer_codepoint(uint64_t x, uint64_t y, uint32_t codepoint,
                                  uint32_t colour, uint32_t scale) {
    uint8_t rows[16];
    unsigned int row;
    unsigned int column;

    if (codepoint < 0x80U) {
        framebuffer_char(x, y, (char)codepoint, colour, scale);
        return;
    }
    if (!glyph_rows_codepoint(codepoint, rows)) {
        ++glyph_missing_count;
        glyph_missing_last = codepoint;
        for (row = 0U; row < 16U; ++row) {
            rows[row] = glyph_missing_marker[row];
        }
    }
    fb_draw_enter();
    for (row = 0U; row < 16U; ++row) {
        for (column = 0U; column < 8U; ++column) {
            if ((rows[row] & (1U << (7U - column))) != 0) {
                framebuffer_rect(x + column * scale, y + row * scale, scale,
                                 scale, colour);
            }
        }
    }
    fb_draw_leave();
}

/*
 * ITEM-509 increment 17: the text path decodes UTF-8.
 *
 * The installer screens, the boot console's framebuffer backend and every other
 * caller hand this function the catalog's UTF-8 text.  A decoded code point is
 * one cell - a Russian letter is not three Latin glyphs - and an invalid or
 * truncated sequence falls back to the byte it started with instead of
 * consuming the rest of the line.  Pure ASCII is byte for byte what it was.
 */
void framebuffer_text(uint64_t x, uint64_t y, const char *text, uint32_t colour, uint32_t scale) {
    uint64_t cursor = x;
    size_t length;
    size_t offset = 0U;

    if (text == NULL) {
        return;
    }
    length = strlen(text);
    while (offset < length) {
        uint32_t codepoint = 0U;
        size_t next = offset;

        if (!appliance_language_utf8_next(text, length, &next, &codepoint)) {
            codepoint = (uint32_t)(unsigned char)text[offset];
            next = offset + 1U;
        }
        framebuffer_codepoint(cursor, y, codepoint, colour, scale);
        cursor += FRAMEBUFFER_FONT_CELL_WIDTH * scale;
        offset = next;
    }
}

void framebuffer_u64(uint64_t x, uint64_t y, uint64_t value, uint32_t colour, uint32_t scale) {
    char buffer[21];
    unsigned int index = 0;
    if (value == 0) {
        framebuffer_text(x, y, "0", colour, scale);
        return;
    }
    while (value > 0) {
        buffer[index++] = (char)('0' + value % 10);
        value /= 10;
    }
    uint64_t cursor = x;
    while (index > 0) {
        framebuffer_char(cursor, y, buffer[--index], colour, scale);
        cursor += FRAMEBUFFER_FONT_CELL_WIDTH * scale;
    }
}

void framebuffer_hex64(uint64_t x, uint64_t y, uint64_t value, uint32_t colour, uint32_t scale) {
    static const char digits_hex[] = "0123456789ABCDEF";
    framebuffer_text(x, y, "0X", colour, scale);
    x += 12U * scale;
    for (int shift = 60; shift >= 0; shift -= 4) {
        framebuffer_char(x, y, digits_hex[(value >> shift) & 0x0fU], colour, scale);
        x += FRAMEBUFFER_FONT_CELL_WIDTH * scale;
    }
}

/*
 * ITEM-457: the row move and the row fill behind the scroll.
 *
 * The 2026-09-19 hardware report was "when the output reaches the bottom of
 * the screen, the refresh is very slow": framebuffer_scroll_up() moved the
 * body one *pixel* at a time, so a single scrolled text line on a 1920x1080
 * screen was about two million scalar reads and writes - and the console, the
 * keyboard poll and the HID drivers all run from the same main loop.
 *
 * Both helpers move whole rows with the widest access the row's alignment
 * allows, checking that alignment once per row instead of once per word.  The
 * framebuffer is 32 bits per pixel (framebuffer_init() refuses anything else)
 * and every row starts at a 4-byte boundary, so the 4-byte path always
 * applies and the byte tail only ever finishes a row whose length is not a
 * multiple of 4 - a shape the 32-bpp contract does not produce.
 *
 * The copy is forward and every destination row is above its source row, so a
 * row that was already written is never read again: the move needs no
 * temporary buffer and no backward pass.
 */
static void framebuffer_copy_row(volatile uint8_t *destination,
                                 const volatile uint8_t *source,
                                 uint64_t bytes) {
    uint64_t offset = 0U;

    if ((((uintptr_t)destination | (uintptr_t)source) & 7U) == 0U) {
        for (; offset + 8U <= bytes; offset += 8U) {
            *(volatile uint64_t *)(uintptr_t)(destination + offset) =
                *(const volatile uint64_t *)(uintptr_t)(source + offset);
        }
    }
    if ((((uintptr_t)(destination + offset) |
          (uintptr_t)(source + offset)) & 3U) == 0U) {
        for (; offset + 4U <= bytes; offset += 4U) {
            *(volatile uint32_t *)(uintptr_t)(destination + offset) =
                *(const volatile uint32_t *)(uintptr_t)(source + offset);
        }
    }
    for (; offset < bytes; ++offset) {
        destination[offset] = source[offset];
    }
}

static void framebuffer_fill_row(volatile uint8_t *destination, uint64_t bytes,
                                 uint32_t colour) {
    const uint64_t pattern = (uint64_t)colour | ((uint64_t)colour << 32U);
    uint64_t offset = 0U;

    if (((uintptr_t)destination & 7U) == 0U) {
        for (; offset + 8U <= bytes; offset += 8U) {
            *(volatile uint64_t *)(uintptr_t)(destination + offset) = pattern;
        }
    }
    for (; offset + 4U <= bytes; offset += 4U) {
        *(volatile uint32_t *)(uintptr_t)(destination + offset) = colour;
    }
    for (; offset < bytes; ++offset) {
        destination[offset] =
            (uint8_t)(colour >> (8U * (uint32_t)(offset & 3U)));
    }
}

/*
 * ITEM-486 (round 24): copy the changed rectangles to video memory and clear
 * the pool.  This is the one device write a paint batch owes; `framebuffer_sync`
 * calls it before it drains, so the existing barrier sites become the batched
 * flush the contract asks for without a new call in the console.
 */
void framebuffer_flush(void) {
    uint64_t bytes = 0U;
    uint64_t rects = 0U;
    uint64_t started = 0U;
    const bool timed = shadow_ready && fb_clock != NULL;

    if (!shadow_ready || dirty_rect_count == 0U || active_framebuffer == NULL) {
        return;
    }
    if (timed) {
        started = fb_clock();
    }
    for (uint32_t index = 0U; index < dirty_rect_count; ++index) {
        const struct framebuffer_dirty_rect *rect = &dirty_rects[index];
        const uint64_t row_bytes = (rect->x1 - rect->x0) * 4U;

        for (uint64_t y = rect->y0; y < rect->y1; ++y) {
            framebuffer_copy_row(
                framebuffer_base() + y * shadow_pitch + rect->x0 * 4U,
                shadow_memory + y * shadow_pitch + rect->x0 * 4U, row_bytes);
        }
        bytes += row_bytes * (rect->y1 - rect->y0);
        ++rects;
    }
    dirty_rect_count = 0U;
    fb_dirty_bytes_total += bytes;
    fb_dirty_rects_total += rects;
    ++fb_flush_count_total;
    if (timed) {
        fb_dirty_flush_us_total += fb_clock() - started;
    }
}

void framebuffer_set_clock(framebuffer_clock_fn clock) {
    fb_clock = clock;
}

uint64_t framebuffer_shadow_draw_us(void) {
    return fb_shadow_draw_us_total;
}

uint64_t framebuffer_dirty_flush_us(void) {
    return fb_dirty_flush_us_total;
}

uint64_t framebuffer_dirty_bytes(void) {
    return fb_dirty_bytes_total;
}

uint64_t framebuffer_dirty_rects(void) {
    return fb_dirty_rects_total;
}

uint64_t framebuffer_flush_count(void) {
    return fb_flush_count_total;
}

/* ITEM-466: the pure scale rule; see framebuffer.h for the numbers. */
uint32_t framebuffer_font_scale(uint64_t width, uint64_t height) {
    for (uint32_t scale = FRAMEBUFFER_FONT_SCALE_MAX; scale >= 1U; --scale) {
        const uint64_t columns =
            width / (FRAMEBUFFER_FONT_CELL_WIDTH * scale);
        const uint64_t rows =
            height / (FRAMEBUFFER_FONT_CELL_HEIGHT * scale);

        if (columns >= FRAMEBUFFER_FONT_MIN_COLUMNS &&
            rows >= FRAMEBUFFER_FONT_MIN_ROWS) {
            return scale;
        }
    }
    return 1U;
}

uint64_t framebuffer_shadow_required_bytes(void) {
    if (active_framebuffer == NULL) {
        return 0U;
    }
    return active_framebuffer->pitch * active_framebuffer->height;
}

bool framebuffer_shadow_init(void *memory, uint64_t bytes) {
    const uint64_t required = framebuffer_shadow_required_bytes();

    if (active_framebuffer == NULL || memory == NULL || required == 0U ||
        bytes < required) {
        return false;
    }
    shadow_memory = (uint8_t *)memory;
    shadow_bytes = bytes;
    shadow_pitch = active_framebuffer->pitch;
    shadow_ready = true;
    dirty_rect_count = 0U;
    /* ITEM-466: start from what the screen already shows, so the two copies
       are identical from the first console write after the binding. */
    for (uint64_t y = 0U; y < active_framebuffer->height; ++y) {
        framebuffer_copy_row(shadow_memory + y * shadow_pitch,
                             framebuffer_base() + y * shadow_pitch,
                             active_framebuffer->width * 4U);
    }
    return true;
}

void framebuffer_shadow_disable(void) {
    /*
     * ITEM-486: a pending shadow paint is copied out before the source is
     * dropped, so disabling the shadow never leaves the device behind the
     * picture the console believed it had drawn.
     */
    framebuffer_flush();
    shadow_memory = NULL;
    shadow_bytes = 0U;
    shadow_pitch = 0U;
    shadow_ready = false;
    dirty_rect_count = 0U;
}

bool framebuffer_shadow_ready(void) {
    return shadow_ready;
}

uint64_t framebuffer_scroll_up(uint64_t top, uint64_t pixels, uint32_t fill_colour) {
    if (active_framebuffer == NULL || top >= active_framebuffer->height || pixels == 0) {
        return 0U;
    }
    /* The rows moved: the saved block under the pointer no longer describes
       what is on the screen there, so the pointer is forgotten rather than
       restored at the wrong place.  The console hides the pointer before this
       move - so no sprite pixel is carried up by the copy - and shows it again
       after the write that scrolled. */
    framebuffer_cursor_invalidate();
    const uint64_t pitch = active_framebuffer->pitch;
    const uint64_t row_bytes = active_framebuffer->width * 4U;
    uint64_t written = 0U;

    /*
     * ITEM-466: with the RAM shadow bound, the move happens in cacheable
     * memory and video memory is only written.  The owner's 2026-09-21 run
     * measured a scroll that read 2.8 MB back out of uncached video memory
     * (max_us=16315) and starved the keyboard; the shadow is the fix.
     */
    if (shadow_ready) {
        uint8_t *shadow = shadow_memory;

        if (pixels >= active_framebuffer->height - top) {
            for (uint64_t y = top; y < active_framebuffer->height; ++y) {
                framebuffer_fill_row(shadow + y * pitch, row_bytes,
                                     fill_colour);
            }
        } else {
            for (uint64_t y = top;
                 y + pixels < active_framebuffer->height; ++y) {
                framebuffer_copy_row(shadow + y * pitch,
                                     shadow + (y + pixels) * pitch, row_bytes);
            }
            for (uint64_t y = active_framebuffer->height - pixels;
                 y < active_framebuffer->height; ++y) {
                framebuffer_fill_row(shadow + y * pitch, row_bytes,
                                     fill_colour);
            }
        }
        for (uint64_t y = top; y < active_framebuffer->height; ++y) {
            framebuffer_copy_row(framebuffer_base() + y * pitch,
                                 shadow + y * pitch, row_bytes);
            written += row_bytes;
        }
        return written;
    }

    volatile uint8_t *base = framebuffer_base();

    if (pixels >= active_framebuffer->height - top) {
        for (uint64_t y = top; y < active_framebuffer->height; ++y) {
            framebuffer_fill_row(base + y * pitch, row_bytes, fill_colour);
            written += row_bytes;
        }
        return written;
    }
    for (uint64_t y = top; y + pixels < active_framebuffer->height; ++y) {
        framebuffer_copy_row(base + y * pitch, base + (y + pixels) * pitch,
                             row_bytes);
        written += row_bytes;
    }
    for (uint64_t y = active_framebuffer->height - pixels;
         y < active_framebuffer->height; ++y) {
        framebuffer_fill_row(base + y * pitch, row_bytes, fill_colour);
        written += row_bytes;
    }
    return written;
}

uint64_t framebuffer_width(void) {
    return active_framebuffer == NULL ? 0 : active_framebuffer->width;
}

uint64_t framebuffer_height(void) {
    return active_framebuffer == NULL ? 0 : active_framebuffer->height;
}

/* ITEM-461: the rest of the mode the boot selected, so the `[FB]` record can
   name it instead of leaving "the resolution may not fit" a guess. */
uint64_t framebuffer_pitch(void) {
    return active_framebuffer == NULL ? 0 : active_framebuffer->pitch;
}

uint32_t framebuffer_bpp(void) {
    return active_framebuffer == NULL ? 0U : active_framebuffer->bpp;
}

/* ITEM-463: whether the selected mode is the bounded one the boot asked for. */
bool framebuffer_mode_is_bounded(uint64_t width, uint64_t height) {
    return width <= FRAMEBUFFER_REQUESTED_WIDTH &&
           height <= FRAMEBUFFER_REQUESTED_HEIGHT;
}

/*
 * ITEM-464: the modes the firmware offered and the one the selection rule
 * prefers.  `modes[]` is the Limine framebuffer response's own list; it is only
 * read, never modified, and `framebuffer_choose_mode()` holds the rule so the
 * host proof can exercise it without a framebuffer.
 */
uint64_t framebuffer_mode_count(void) {
    if (active_framebuffer == NULL) {
        return 0U;
    }
    return active_framebuffer->mode_count;
}

bool framebuffer_best_mode(uint64_t *width, uint64_t *height) {
    if (active_framebuffer == NULL) {
        return false;
    }
    return framebuffer_choose_mode(active_framebuffer->modes,
                                   active_framebuffer->mode_count,
                                   width, height);
}

/*
 * ITEM-466 (round 5): name the firmware's preference and what the rule would
 * choose.  Limine orders `modes[]` from most to least preferred, so the first
 * 32-bpp entry is the native mode; the boot no longer asks for a resolution of
 * its own (the owner's "no hardcoded 1024x768"), and this is what makes the
 * result checkable.
 */
bool framebuffer_describe_modes(uint64_t actual_width, uint64_t actual_height,
                                struct framebuffer_mode_choice *choice) {
    if (active_framebuffer == NULL || choice == NULL) {
        return false;
    }
    return framebuffer_choose_preferred(active_framebuffer->modes,
                                        active_framebuffer->mode_count,
                                        actual_width, actual_height, choice);
}

/* ITEM-466: decimal helper for the `[FB] available=` list. */
static size_t framebuffer_append_u64(char *output, size_t capacity,
                                     size_t length, uint64_t value) {
    char digits[20];
    size_t count = 0U;

    if (value == 0U) {
        digits[count++] = '0';
    }
    while (value != 0U) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count > 0U && length + 1U < capacity) {
        output[length++] = digits[--count];
    }
    if (capacity != 0U) {
        output[length] = '\0';
    }
    return length;
}

uint64_t framebuffer_available_text(char *output, size_t capacity) {
    size_t length = 0U;
    uint64_t written = 0U;

    if (output == NULL || capacity == 0U || active_framebuffer == NULL) {
        return 0U;
    }
    output[0] = '\0';
    for (uint64_t index = 0U; index < active_framebuffer->mode_count; ++index) {
        const struct limine_video_mode *mode = active_framebuffer->modes[index];
        char entry[48];
        size_t entry_length = 0U;
        size_t needed;

        if (mode == NULL || mode->bpp != 32U) {
            continue;
        }
        /* Compose the entry, then reserve four bytes for a possible ",...". */
        if (written != 0U) {
            entry[entry_length++] = ',';
        }
        entry_length = framebuffer_append_u64(entry, sizeof(entry),
                                              entry_length, mode->width);
        entry[entry_length++] = 'x';
        entry_length = framebuffer_append_u64(entry, sizeof(entry),
                                              entry_length, mode->height);
        entry[entry_length] = '\0';
        needed = entry_length;
        if (length + needed + 4U > capacity) {
            /* The list does not fit whole: mark the truncation, bounded. */
            if (length + 4U <= capacity) {
                output[length++] = '.';
                output[length++] = '.';
                output[length++] = '.';
                output[length] = '\0';
            }
            break;
        }
        for (size_t position = 0U; position < entry_length; ++position) {
            output[length++] = entry[position];
        }
        output[length] = '\0';
        ++written;
    }
    return written;
}

/*
 * ITEM-464: drain the write-combining stores before a duration is read.
 *
 * x86 orders write-combining stores with `sfence`, but the stores may still be
 * queued in the store buffer; reading a framebuffer byte is a device read that
 * cannot pass the queued writes, so the read makes the barrier observable.  A
 * machine without a framebuffer has nothing to drain.
 */
void framebuffer_sync(void) {
    if (active_framebuffer == NULL || active_framebuffer->address == NULL) {
        return;
    }
    /*
     * ITEM-486 (round 24): the barrier is the batch boundary, so the shadow's
     * changed rectangles reach the device here and nowhere per pixel.  The
     * `sfence` and the device read then make the queued write-combining stores
     * observable, exactly as before.
     */
    framebuffer_flush();
    __asm__ volatile("sfence" : : : "memory");
    (void)*(const volatile uint32_t *)(uintptr_t)active_framebuffer->address;
}

/*
 * ITEM-448: the software pointer.
 *
 * The sprite is an arrow: 1 is the white body, 2 its dark outline, 0 is
 * transparent.  The saved block holds what was under the sprite, so hide()
 * puts the screen back exactly as it was - including the text of an entry the
 * pointer happened to cover - and show() at a new place first restores the old
 * one.  Only the pixels inside the screen are ever saved, written or restored,
 * which is what lets a pointer sit on the last row without running past the
 * framebuffer.
 */
static uint32_t pixel_read(uint64_t x, uint64_t y) {
    if (shadow_ready) {
        /* ITEM-466: the pointer's save/restore reads the RAM copy too, so the
           cursor never turns into an uncached video read either. */
        return ((const uint32_t *)shadow_memory)[
            y * (shadow_pitch / 4U) + x];
    }
    const volatile uint32_t *address =
        (const volatile uint32_t *)active_framebuffer->address;

    return address[y * (active_framebuffer->pitch / 4U) + x];
}

bool framebuffer_cursor_show(uint64_t x, uint64_t y) {
    uint64_t origin_x;
    uint64_t origin_y;

    if (active_framebuffer == NULL) {
        return false;
    }
    fb_draw_enter();
    /*
     * ITEM-471: the whole sprite stays on the screen.  The pointer's own
     * position is clamped to the last pixel, but drawing from it would cut the
     * arrow at the right and bottom edges (the old loops `break` there and
     * left a single column).  The origin moves left/up by exactly as much as
     * the sprite would overhang, so the arrow is always complete.
     */
    origin_x = (uint64_t)pointer_sprite_origin(
        (int32_t)x, (int32_t)active_framebuffer->width,
        (int32_t)FRAMEBUFFER_CURSOR_WIDTH);
    origin_y = (uint64_t)pointer_sprite_origin(
        (int32_t)y, (int32_t)active_framebuffer->height,
        (int32_t)FRAMEBUFFER_CURSOR_HEIGHT);
    framebuffer_cursor_hide();
    cursor_saved_x = origin_x;
    cursor_saved_y = origin_y;
    cursor_saved_width = 0U;
    cursor_saved_height = 0U;
    for (uint64_t row = 0U; row < FRAMEBUFFER_CURSOR_HEIGHT; ++row) {
        if (origin_y + row >= active_framebuffer->height) {
            break;
        }
        for (uint64_t column = 0U; column < FRAMEBUFFER_CURSOR_WIDTH;
             ++column) {
            if (origin_x + column >= active_framebuffer->width) {
                break;
            }
            cursor_saved[row * FRAMEBUFFER_CURSOR_WIDTH + column] =
                pixel_read(origin_x + column, origin_y + row);
            if (cursor_saved_width < column + 1U) {
                cursor_saved_width = column + 1U;
            }
        }
        if (cursor_saved_height < row + 1U) {
            cursor_saved_height = row + 1U;
        }
    }
    for (uint64_t row = 0U; row < cursor_saved_height; ++row) {
        for (uint64_t column = 0U; column < cursor_saved_width; ++column) {
            const uint8_t mark = cursor_sprite[row][column];

            if (mark == 0U) {
                continue;
            }
            pixel(origin_x + column, origin_y + row,
                  mark == 1U ? FRAMEBUFFER_CURSOR_WHITE
                             : FRAMEBUFFER_CURSOR_DARK);
        }
    }
    cursor_visible = true;
    fb_draw_leave();
    return true;
}

void framebuffer_cursor_hide(void) {
    if (!cursor_visible || active_framebuffer == NULL) {
        cursor_visible = false;
        return;
    }
    fb_draw_enter();
    for (uint64_t row = 0U; row < cursor_saved_height; ++row) {
        for (uint64_t column = 0U; column < cursor_saved_width; ++column) {
            pixel(cursor_saved_x + column, cursor_saved_y + row,
                  cursor_saved[row * FRAMEBUFFER_CURSOR_WIDTH + column]);
        }
    }
    cursor_visible = false;
    fb_draw_leave();
}

bool framebuffer_cursor_visible(void) {
    return cursor_visible;
}

/*
 * ITEM-482 (round 20): throw the saved block away without painting it.
 *
 * The screen under the pointer was rewritten (the body scrolled, or a repaint
 * covered it), so restoring the block would put back pixels that no longer
 * belong there and leave a second arrow behind.  Forgetting the block makes
 * the next hide a no-op and the next show save the surface as it now is.
 */
void framebuffer_cursor_invalidate(void) {
    cursor_visible = false;
    cursor_saved_width = 0U;
    cursor_saved_height = 0U;
}
