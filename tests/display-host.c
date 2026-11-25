/*
 * ITEM-488: the display abstraction, proven on the host.
 *
 * The appliance has to install on machines whose GPU it has no driver for, so
 * the installer draws on the firmware's own framebuffer or not at all.  This
 * proof is the contract of that layer, and it is deliberately built around the
 * cases that must *not* pass: a framebuffer that is absent, one whose address is
 * null, one with a zero or absurd dimension, one in a pixel format this kernel
 * does not write, one whose pitch cannot hold a scanline.  Every one of them
 * must end as DEGRADED or UNAVAILABLE with the text path bound, and AVAILABLE
 * must be reachable only by a framebuffer that passed every check.
 */
#include "display.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

/* A framebuffer shaped like the one OVMF hands over through the boot protocol. */
static struct display_framebuffer display_fixture(uint64_t width, uint64_t height,
                                                  uint16_t bpp)
{
    struct display_framebuffer framebuffer;

    memset(&framebuffer, 0, sizeof(framebuffer));
    framebuffer.present = true;
    framebuffer.address = (void *)UINT64_C(0x00000000FD000000);
    framebuffer.width = width;
    framebuffer.height = height;
    framebuffer.pitch = width * ((uint64_t)bpp / 8U);
    framebuffer.bpp = bpp;
    framebuffer.memory_model = 1U;
    framebuffer.red_mask_size = 8U;
    framebuffer.red_mask_shift = 16U;
    framebuffer.green_mask_size = 8U;
    framebuffer.green_mask_shift = 8U;
    framebuffer.blue_mask_size = 8U;
    framebuffer.blue_mask_shift = 0U;
    return framebuffer;
}

static void test_reset_is_honest(void)
{
    const struct display_capabilities *caps;

    display_reset();
    caps = display_capabilities();
    CHECK(caps != NULL && caps->backend == DISPLAY_BACKEND_NONE &&
              caps->status == DISPLAY_STATUS_UNAVAILABLE &&
              !caps->framebuffer_usable &&
              strcmp(caps->reason, "NOT_PROBED") == 0,
          "a display that was never probed reports NONE/UNAVAILABLE, not a "
          "framebuffer");
}

static void test_gop_framebuffer_is_available(void)
{
    struct display_framebuffer framebuffer = display_fixture(1920U, 1080U, 32U);
    const struct display_capabilities *caps;
    char record[DISPLAY_RECORD_BYTES];
    size_t length;

    display_reset();
    CHECK(display_probe_framebuffer(&framebuffer) == DISPLAY_PROBE_OK,
          "a firmware framebuffer with sane parameters is accepted");
    caps = display_capabilities();
    CHECK(caps->backend == DISPLAY_BACKEND_GOP &&
              caps->status == DISPLAY_STATUS_AVAILABLE &&
              caps->framebuffer_usable,
          "the accepted framebuffer is the GOP backend and it is AVAILABLE");
    CHECK(caps->width == 1920U && caps->height == 1080U &&
              caps->pitch == 7680U && caps->bpp == 32U &&
              caps->format == DISPLAY_FORMAT_BGRX8888,
          "the reading names the resolution, pitch, depth and pixel format");
    CHECK(display_width() == 1920U && display_height() == 1080U &&
              display_scale() == 2U,
          "the abstraction answers the geometry the UI lays out with");
    length = display_format_record(record, sizeof(record));
    CHECK(length == strlen("[DISPLAY] backend=GOP status=AVAILABLE "
                           "resolution=1920x1080 pitch=7680 bpp=32 "
                           "format=BGRX8888 fallback=TEXT reason=OK") &&
              strcmp(record,
                     "[DISPLAY] backend=GOP status=AVAILABLE "
                     "resolution=1920x1080 pitch=7680 bpp=32 "
                     "format=BGRX8888 fallback=TEXT reason=OK") == 0,
          "the record names the backend, the status, the geometry, the format, "
          "the fallback and the reason");
}

static void test_rgbx_framebuffer_is_available(void)
{
    struct display_framebuffer framebuffer = display_fixture(1280U, 800U, 32U);
    const struct display_capabilities *caps;

    framebuffer.red_mask_shift = 0U;
    framebuffer.blue_mask_shift = 16U;
    display_reset();
    CHECK(display_probe_framebuffer(&framebuffer) == DISPLAY_PROBE_OK,
          "the other byte order this kernel writes is accepted too");
    caps = display_capabilities();
    CHECK(caps->status == DISPLAY_STATUS_AVAILABLE &&
              caps->format == DISPLAY_FORMAT_RGBX8888 &&
              display_scale() == 2U,
          "the reading names RGBX8888 rather than guessing BGRX8888");
}

static void test_absent_framebuffer_falls_back(void)
{
    const struct display_capabilities *caps;
    char record[DISPLAY_RECORD_BYTES];
    size_t length;

    display_reset();
    CHECK(display_probe_framebuffer(NULL) == DISPLAY_PROBE_NO_FRAMEBUFFER,
          "a machine with no framebuffer at all is not an error, it is a case");
    caps = display_capabilities();
    CHECK(caps->backend == DISPLAY_BACKEND_TEXT &&
              caps->status == DISPLAY_STATUS_UNAVAILABLE &&
              !caps->framebuffer_usable && caps->text_fallback,
          "no framebuffer binds the text path and never claims AVAILABLE");
    length = display_format_record(record, sizeof(record));
    CHECK(length > 0U &&
              strstr(record, "backend=TEXT") != NULL &&
              strstr(record, "status=UNAVAILABLE") != NULL &&
              strstr(record, "resolution=NONE") != NULL &&
              strstr(record, "reason=NO_FRAMEBUFFER") != NULL,
          "the record says TEXT/UNAVAILABLE/NONE and why");
}

static void test_serial_is_named_when_it_carries_the_flow(void)
{
    const struct display_capabilities *caps;
    char record[DISPLAY_RECORD_BYTES];

    display_reset();
    display_bind_text_fallback("NO_FRAMEBUFFER", true);
    caps = display_capabilities();
    CHECK(caps->text_fallback && caps->serial_console,
          "a text fallback with a serial stream reports both");
    CHECK(display_format_record(record, sizeof(record)) > 0U &&
              strstr(record, "fallback=SERIAL") != NULL,
          "the record names the serial fallback");
}

static void test_bad_address_degrades(void)
{
    struct display_framebuffer framebuffer = display_fixture(1024U, 768U, 32U);

    framebuffer.address = NULL;
    display_reset();
    CHECK(display_probe_framebuffer(&framebuffer) == DISPLAY_PROBE_BAD_ADDRESS &&
              !display_framebuffer_usable(),
          "a framebuffer with a null address is refused");
}

static void test_invalid_dimensions_degrade(void)
{
    struct display_framebuffer zero_width = display_fixture(0U, 768U, 32U);
    struct display_framebuffer zero_height = display_fixture(1024U, 0U, 32U);
    struct display_framebuffer too_wide =
        display_fixture(DISPLAY_MAX_DIMENSION + 1U, 768U, 32U);

    display_reset();
    CHECK(display_probe_framebuffer(&zero_width) ==
              DISPLAY_PROBE_INVALID_DIMENSIONS,
          "a zero width is refused");
    display_reset();
    CHECK(display_probe_framebuffer(&zero_height) ==
              DISPLAY_PROBE_INVALID_DIMENSIONS,
          "a zero height is refused");
    display_reset();
    CHECK(display_probe_framebuffer(&too_wide) ==
              DISPLAY_PROBE_INVALID_DIMENSIONS,
          "a dimension beyond the bound this kernel computes in is refused");
    CHECK(display_capabilities()->status == DISPLAY_STATUS_UNAVAILABLE &&
              display_capabilities()->text_fallback,
          "every refused geometry leaves the text path bound");
}

static void test_unsupported_formats_degrade(void)
{
    struct display_framebuffer sixteen = display_fixture(1024U, 768U, 16U);
    struct display_framebuffer unknown_masks = display_fixture(1024U, 768U, 32U);
    struct display_framebuffer not_rgb = display_fixture(1024U, 768U, 32U);

    unknown_masks.red_mask_size = 5U;
    unknown_masks.green_mask_size = 6U;
    unknown_masks.blue_mask_size = 5U;
    not_rgb.memory_model = 0U;
    display_reset();
    CHECK(display_probe_framebuffer(&sixteen) ==
              DISPLAY_PROBE_UNSUPPORTED_FORMAT,
          "a pixel format this kernel does not write is refused, not guessed");
    display_reset();
    CHECK(display_probe_framebuffer(&unknown_masks) ==
              DISPLAY_PROBE_UNSUPPORTED_FORMAT,
          "masks that name no format this kernel writes are refused");
    display_reset();
    CHECK(display_probe_framebuffer(&not_rgb) ==
              DISPLAY_PROBE_UNSUPPORTED_FORMAT,
          "a non-RGB memory model is refused");
    CHECK(strcmp(display_capabilities()->reason, "UNSUPPORTED_PIXEL_FORMAT") ==
              0,
          "the refusal names the pixel format, not the machine");
}

static void test_invalid_pitch_degrades(void)
{
    struct display_framebuffer short_pitch = display_fixture(1024U, 768U, 32U);
    struct display_framebuffer misaligned = display_fixture(1024U, 768U, 32U);

    short_pitch.pitch = 1024U * 4U - 4U;
    misaligned.pitch = 1024U * 4U + 2U;
    display_reset();
    CHECK(display_probe_framebuffer(&short_pitch) == DISPLAY_PROBE_INVALID_PITCH,
          "a pitch that cannot hold a scanline is refused");
    display_reset();
    CHECK(display_probe_framebuffer(&misaligned) == DISPLAY_PROBE_INVALID_PITCH,
          "a pitch that is not a whole number of pixels is refused");
    CHECK(!display_framebuffer_usable(),
          "no refused pitch leaves a usable framebuffer behind");
}

static void test_a_failed_probe_clears_the_previous_one(void)
{
    struct display_framebuffer good = display_fixture(1920U, 1080U, 32U);
    struct display_framebuffer bad = display_fixture(1920U, 1080U, 32U);

    bad.pitch = 16U;
    display_reset();
    CHECK(display_probe_framebuffer(&good) == DISPLAY_PROBE_OK &&
              display_framebuffer_usable(),
          "a good framebuffer is bound");
    CHECK(display_probe_framebuffer(&bad) == DISPLAY_PROBE_INVALID_PITCH &&
              !display_framebuffer_usable() && display_width() == 0U,
          "a later failed probe rebinds to text instead of leaving the old "
          "framebuffer in place");
}

static void test_the_record_is_bounded(void)
{
    struct display_framebuffer framebuffer =
        display_fixture(DISPLAY_MAX_DIMENSION, DISPLAY_MAX_DIMENSION, 32U);
    char record[DISPLAY_RECORD_BYTES];
    size_t length;

    framebuffer.pitch = UINT64_C(0xFFFFFFFFFFFF0000);
    display_reset();
    (void)display_probe_framebuffer(&framebuffer);
    length = display_format_record(record, sizeof(record));
    CHECK(length > 0U && length <= DISPLAY_RECORD_BYTES,
          "the widest reading still fits the bound the journal carries");
    CHECK(display_format_record(NULL, sizeof(record)) == 0U &&
              display_format_record(record, 8U) == 0U,
          "a record that cannot fit is refused, never truncated");
}

static void test_names_are_stable(void)
{
    CHECK(strcmp(display_backend_name(DISPLAY_BACKEND_GOP), "GOP") == 0 &&
              strcmp(display_backend_name(DISPLAY_BACKEND_TEXT), "TEXT") == 0 &&
              strcmp(display_backend_name(DISPLAY_BACKEND_NONE), "NONE") == 0,
          "the backend names are the words the records and the UI read");
    CHECK(strcmp(display_status_name(DISPLAY_STATUS_AVAILABLE), "AVAILABLE") ==
              0 &&
              strcmp(display_status_name(DISPLAY_STATUS_DEGRADED), "DEGRADED") ==
                  0 &&
              strcmp(display_status_name(DISPLAY_STATUS_UNAVAILABLE),
                     "UNAVAILABLE") == 0,
          "the status names are the words the installer shows");
    CHECK(strcmp(display_probe_result_name(DISPLAY_PROBE_NO_FRAMEBUFFER),
                 "NO_FRAMEBUFFER") == 0 &&
              strcmp(display_format_name(DISPLAY_FORMAT_BGRX8888),
                     "BGRX8888") == 0,
          "the probe results and formats have one spelling each");
}

int main(void)
{
    display_reset();
    test_reset_is_honest();
    test_gop_framebuffer_is_available();
    test_rgbx_framebuffer_is_available();
    test_absent_framebuffer_falls_back();
    test_serial_is_named_when_it_carries_the_flow();
    test_bad_address_degrades();
    test_invalid_dimensions_degrade();
    test_unsupported_formats_degrade();
    test_invalid_pitch_degrades();
    test_a_failed_probe_clears_the_previous_one();
    test_the_record_is_bounded();
    test_names_are_stable();

    if (failures != 0) {
        fprintf(stderr, "Display host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Display host tests: ALL OK");
    return 0;
}
