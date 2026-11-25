#include "display.h"

#include <stddef.h>
#include <string.h>

/*
 * ITEM-488: the display abstraction, and the only place its state lives.
 *
 * The layer is deliberately a value machine: a probe copies what the firmware
 * said into `display_state`, checks it against the rules this kernel can
 * actually honour, and either binds the framebuffer or binds the text path.
 * Nothing here reads memory the firmware mapped, so the whole thing is provable
 * on a host with no firmware at all - which is what `tests/display-host.c`
 * does, case by case, including the cases that must *not* become AVAILABLE.
 */

/* The framebuffer memory model the abstraction writes: RGB. */
#define DISPLAY_MEMORY_MODEL_RGB 1U

static struct display_capabilities display_state;

void display_reset(void)
{
    memset(&display_state, 0, sizeof(display_state));
    display_state.backend = DISPLAY_BACKEND_NONE;
    display_state.status = DISPLAY_STATUS_UNAVAILABLE;
    display_state.format = DISPLAY_FORMAT_UNKNOWN;
    display_state.framebuffer_usable = false;
    display_state.text_fallback = false;
    display_state.serial_console = false;
    display_state.reason = "NOT_PROBED";
}

void display_bind_text_fallback(const char *reason, bool serial_console)
{
    display_state.backend = DISPLAY_BACKEND_TEXT;
    display_state.status = DISPLAY_STATUS_UNAVAILABLE;
    display_state.format = DISPLAY_FORMAT_UNKNOWN;
    display_state.framebuffer_usable = false;
    display_state.text_fallback = true;
    display_state.serial_console = serial_console;
    display_state.address = NULL;
    display_state.width = 0U;
    display_state.height = 0U;
    display_state.pitch = 0U;
    display_state.bpp = 0U;
    display_state.reason = reason != NULL ? reason : "NO_FRAMEBUFFER";
}

/*
 * The pixel format the firmware's masks describe, or UNKNOWN.  A format this
 * kernel does not write is not guessed at: it is refused, and the refusal is
 * what a caller reads in `capabilities()->reason`.
 */
static enum display_pixel_format display_classify_format(
    const struct display_framebuffer *framebuffer)
{
    if (framebuffer->red_mask_size != 8U ||
        framebuffer->green_mask_size != 8U ||
        framebuffer->blue_mask_size != 8U) {
        return DISPLAY_FORMAT_UNKNOWN;
    }
    if (framebuffer->red_mask_shift == 16U &&
        framebuffer->green_mask_shift == 8U &&
        framebuffer->blue_mask_shift == 0U) {
        return DISPLAY_FORMAT_BGRX8888;
    }
    if (framebuffer->red_mask_shift == 0U &&
        framebuffer->green_mask_shift == 8U &&
        framebuffer->blue_mask_shift == 16U) {
        return DISPLAY_FORMAT_RGBX8888;
    }
    return DISPLAY_FORMAT_UNKNOWN;
}

/*
 * A pitch that cannot hold one scanline, or that is not a whole number of
 * pixels, describes a framebuffer whose rows do not line up with the ones this
 * layer would write; a dimension of zero or beyond the bound describes one
 * whose address arithmetic cannot be trusted.  Both are refused rather than
 * clamped: a display that draws into the wrong place is worse than no display.
 */
static bool display_pitch_is_valid(uint64_t pitch, uint64_t width, uint16_t bpp)
{
    const uint64_t bytes_per_pixel = (uint64_t)bpp / 8U;

    if (bytes_per_pixel == 0U) {
        return false;
    }
    if (pitch % bytes_per_pixel != 0U) {
        return false;
    }
    return pitch >= width * bytes_per_pixel;
}

enum display_probe_result display_probe_framebuffer(
    const struct display_framebuffer *framebuffer)
{
    enum display_pixel_format format;

    if (framebuffer == NULL || !framebuffer->present) {
        display_bind_text_fallback("NO_FRAMEBUFFER", false);
        return DISPLAY_PROBE_NO_FRAMEBUFFER;
    }
    if (framebuffer->address == NULL) {
        display_bind_text_fallback("BAD_ADDRESS", false);
        return DISPLAY_PROBE_BAD_ADDRESS;
    }
    if (framebuffer->width == 0U || framebuffer->height == 0U ||
        framebuffer->width > DISPLAY_MAX_DIMENSION ||
        framebuffer->height > DISPLAY_MAX_DIMENSION) {
        display_bind_text_fallback("INVALID_DIMENSIONS", false);
        return DISPLAY_PROBE_INVALID_DIMENSIONS;
    }
    if (framebuffer->memory_model != DISPLAY_MEMORY_MODEL_RGB ||
        framebuffer->bpp != DISPLAY_SUPPORTED_BPP) {
        display_bind_text_fallback("UNSUPPORTED_PIXEL_FORMAT", false);
        return DISPLAY_PROBE_UNSUPPORTED_FORMAT;
    }
    format = display_classify_format(framebuffer);
    if (format == DISPLAY_FORMAT_UNKNOWN) {
        display_bind_text_fallback("UNSUPPORTED_PIXEL_FORMAT", false);
        return DISPLAY_PROBE_UNSUPPORTED_FORMAT;
    }
    if (!display_pitch_is_valid(framebuffer->pitch, framebuffer->width,
                                framebuffer->bpp)) {
        display_bind_text_fallback("INVALID_PITCH", false);
        return DISPLAY_PROBE_INVALID_PITCH;
    }
    display_state.backend = DISPLAY_BACKEND_GOP;
    display_state.status = DISPLAY_STATUS_AVAILABLE;
    display_state.format = format;
    display_state.framebuffer_usable = true;
    /*
     * A reachable framebuffer does not by itself make the text path
     * unreachable: the console still carries the records, and a run that loses
     * the framebuffer later keeps working.
     */
    display_state.text_fallback = true;
    display_state.serial_console = false;
    display_state.address = framebuffer->address;
    display_state.width = framebuffer->width;
    display_state.height = framebuffer->height;
    display_state.pitch = framebuffer->pitch;
    display_state.bpp = framebuffer->bpp;
    display_state.reason = "OK";
    return DISPLAY_PROBE_OK;
}

const struct display_capabilities *display_capabilities(void)
{
    return &display_state;
}

bool display_framebuffer_usable(void)
{
    return display_state.framebuffer_usable;
}

bool display_text_available(void)
{
    return display_state.text_fallback;
}

uint64_t display_width(void)
{
    return display_state.width;
}

uint64_t display_height(void)
{
    return display_state.height;
}

uint32_t display_scale(void)
{
    if (!display_state.framebuffer_usable) {
        return 0U;
    }
    /*
     * The glyph scale the console already chooses for a mode: one step up from
     * the 640x480 default every 1024 pixels of width, so a 1920-wide screen is
     * readable from across a room and a small one is not scaled at all.
     */
    if (display_state.width >= 3840U) {
        return 4U;
    }
    if (display_state.width >= 2560U) {
        return 3U;
    }
    if (display_state.width >= 1280U) {
        return 2U;
    }
    return 1U;
}

const char *display_backend_name(enum display_backend backend)
{
    switch (backend) {
    case DISPLAY_BACKEND_GOP:
        return "GOP";
    case DISPLAY_BACKEND_TEXT:
        return "TEXT";
    case DISPLAY_BACKEND_NONE:
    default:
        return "NONE";
    }
}

const char *display_status_name(enum display_status status)
{
    switch (status) {
    case DISPLAY_STATUS_AVAILABLE:
        return "AVAILABLE";
    case DISPLAY_STATUS_DEGRADED:
        return "DEGRADED";
    case DISPLAY_STATUS_UNAVAILABLE:
    default:
        return "UNAVAILABLE";
    }
}

const char *display_format_name(enum display_pixel_format format)
{
    switch (format) {
    case DISPLAY_FORMAT_BGRX8888:
        return "BGRX8888";
    case DISPLAY_FORMAT_RGBX8888:
        return "RGBX8888";
    case DISPLAY_FORMAT_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

const char *display_probe_result_name(enum display_probe_result result)
{
    switch (result) {
    case DISPLAY_PROBE_OK:
        return "OK";
    case DISPLAY_PROBE_NO_FRAMEBUFFER:
        return "NO_FRAMEBUFFER";
    case DISPLAY_PROBE_BAD_ADDRESS:
        return "BAD_ADDRESS";
    case DISPLAY_PROBE_INVALID_DIMENSIONS:
        return "INVALID_DIMENSIONS";
    case DISPLAY_PROBE_INVALID_PITCH:
        return "INVALID_PITCH";
    case DISPLAY_PROBE_UNSUPPORTED_FORMAT:
    default:
        return "UNSUPPORTED_PIXEL_FORMAT";
    }
}

/*
 * The record writer: bounded, and it refuses rather than truncates, so a
 * published line never carries half a number.
 */
struct display_record_writer {
    char *buffer;
    size_t size;
    size_t length;
    bool overflow;
};

static void display_record_text(struct display_record_writer *writer,
                                const char *text)
{
    size_t index = 0U;

    if (writer->overflow || text == NULL) {
        return;
    }
    while (text[index] != '\0') {
        if (writer->length + 1U >= writer->size) {
            writer->overflow = true;
            return;
        }
        writer->buffer[writer->length++] = text[index++];
    }
    writer->buffer[writer->length] = '\0';
}

static void display_record_u64(struct display_record_writer *writer,
                               uint64_t value)
{
    char digits[20U];
    size_t count = 0U;

    if (writer->overflow) {
        return;
    }
    if (value == 0U) {
        display_record_text(writer, "0");
        return;
    }
    while (value != 0U && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count > 0U) {
        char single[2U];

        single[0] = digits[--count];
        single[1] = '\0';
        display_record_text(writer, single);
    }
}

size_t display_format_record(char *buffer, size_t size)
{
    struct display_record_writer writer;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    writer.buffer = buffer;
    writer.size = size;
    writer.length = 0U;
    writer.overflow = false;
    buffer[0] = '\0';
    display_record_text(&writer, "[DISPLAY] backend=");
    display_record_text(&writer, display_backend_name(display_state.backend));
    display_record_text(&writer, " status=");
    display_record_text(&writer, display_status_name(display_state.status));
    display_record_text(&writer, " resolution=");
    if (display_state.width == 0U || display_state.height == 0U) {
        display_record_text(&writer, "NONE");
    } else {
        display_record_u64(&writer, display_state.width);
        display_record_text(&writer, "x");
        display_record_u64(&writer, display_state.height);
    }
    display_record_text(&writer, " pitch=");
    display_record_u64(&writer, display_state.pitch);
    display_record_text(&writer, " bpp=");
    display_record_u64(&writer, display_state.bpp);
    display_record_text(&writer, " format=");
    display_record_text(&writer, display_format_name(display_state.format));
    display_record_text(&writer, " fallback=");
    if (display_state.serial_console) {
        display_record_text(&writer, "SERIAL");
    } else if (display_state.text_fallback) {
        display_record_text(&writer, "TEXT");
    } else {
        display_record_text(&writer, "NONE");
    }
    display_record_text(&writer, " reason=");
    display_record_text(&writer, display_state.reason);
    if (writer.overflow || writer.length == 0U ||
        writer.length > DISPLAY_RECORD_BYTES) {
        return 0U;
    }
    return writer.length;
}
