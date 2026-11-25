#ifndef HFOC_DISPLAY_H
#define HFOC_DISPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-488: the display abstraction the installer draws through.
 *
 * The installer has to run on as wide a set of UEFI x86-64 machines as
 * possible, and it must do that **without a single GPU-specific driver**.  What
 * every one of those machines has in common is that its firmware already put a
 * framebuffer on the screen: UEFI's Graphics Output Protocol hands one over
 * before any operating system exists, and the boot protocol this kernel uses
 * passes that same framebuffer to us - address, width, height, pitch, pixel
 * format and all.  Nothing in this item knows or wants to know whether the
 * silicon behind it is AMD, Intel, NVIDIA or a virtual device in QEMU.
 *
 * This header is the boundary between that firmware framebuffer and the
 * installer:
 *
 *     UEFI GOP backend (display_firmware.c, the boot protocol's framebuffer)
 *             |
 *             v
 *     HFOC display abstraction (this file, display.c)
 *             |
 *             v
 *     installer UI (installer_ui.c)
 *
 * The installer UI never reads a GOP structure, never writes a pixel address and
 * never assumes a resolution.  It asks this layer for capabilities, draws
 * through it, and - when the framebuffer is missing, malformed or in a format
 * this kernel does not write - the same flow is carried by the text console
 * and, where one exists, the serial stream.
 *
 * Three rules shape the layer, and the third is the one that keeps it honest:
 *
 *   * **no GPU driver, no acceleration, no DRM/KMS.**  The framebuffer is
 *     memory the firmware mapped; drawing into it is a store;
 *   * **the backend is replaceable.**  A later item can add another backend
 *     behind this contract without the installer UI changing;
 *   * **fail closed.**  Finding a framebuffer is not the same as having a
 *     usable one: a null address, a zero or absurd dimension, a pitch that
 *     cannot hold a scanline, a pixel format this kernel does not write - each
 *     of those is `DEGRADED` or `UNAVAILABLE` with the text fallback bound, and
 *     `AVAILABLE` is reserved for a framebuffer that passed every check.
 */

/* The bound of the `[DISPLAY]` record: the journal carries it whole. */
#define DISPLAY_RECORD_BYTES 320U
/*
 * A framebuffer wider or taller than this is not a display this kernel writes:
 * the value bounds the arithmetic (pitch, offset, span) long before any pixel
 * is touched, so a firmware that reports nonsense cannot make the installer
 * compute an address outside the mapping it was handed.
 */
#define DISPLAY_MAX_DIMENSION 16384U
/* The one pixel format this kernel's drawing path writes. */
#define DISPLAY_SUPPORTED_BPP 32U

enum display_backend {
    DISPLAY_BACKEND_NONE = 0,
    /* The firmware's framebuffer, handed over by the boot protocol. */
    DISPLAY_BACKEND_GOP = 1,
    /* No framebuffer: the console and, when present, the serial stream. */
    DISPLAY_BACKEND_TEXT = 2,
};

enum display_status {
    DISPLAY_STATUS_UNAVAILABLE = 0,
    /* A backend exists but cannot be used; the text path carries the flow. */
    DISPLAY_STATUS_DEGRADED = 1,
    DISPLAY_STATUS_AVAILABLE = 2,
};

enum display_pixel_format {
    DISPLAY_FORMAT_UNKNOWN = 0,
    /* Byte order as the masks describe it, named the way a reader reads it. */
    DISPLAY_FORMAT_BGRX8888 = 1,
    DISPLAY_FORMAT_RGBX8888 = 2,
};

/*
 * The framebuffer as the boot protocol describes it, copied out of whatever
 * structure the firmware filled in.  This is the *only* thing the abstraction
 * accepts, so the layer above it can be proven on a host with no firmware at
 * all.
 */
struct display_framebuffer {
    bool present;
    void *address;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    /* The format, as the firmware's masks describe it. */
    uint8_t memory_model;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
};

enum display_probe_result {
    DISPLAY_PROBE_OK = 0,
    DISPLAY_PROBE_NO_FRAMEBUFFER = 1,
    DISPLAY_PROBE_BAD_ADDRESS = 2,
    DISPLAY_PROBE_INVALID_DIMENSIONS = 3,
    DISPLAY_PROBE_INVALID_PITCH = 4,
    DISPLAY_PROBE_UNSUPPORTED_FORMAT = 5,
};

struct display_capabilities {
    enum display_backend backend;
    enum display_status status;
    enum display_pixel_format format;
    /* True only when drawing may go to the framebuffer. */
    bool framebuffer_usable;
    /* True when the console carries the same flow without a framebuffer. */
    bool text_fallback;
    /* True when a serial stream is available to carry it too. */
    bool serial_console;
    void *address;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    /* Why the status is what it is, in one word a record can carry. */
    const char *reason;
};

/*
 * Probe a firmware framebuffer and bind the display to it, or to the text
 * fallback when it cannot be used.  `DEGRADED` and `UNAVAILABLE` both leave the
 * text path bound: the installer never continues into a graphical flow it
 * cannot draw, and never panics over a display it does not have.
 */
enum display_probe_result display_probe_framebuffer(
    const struct display_framebuffer *framebuffer);
/*
 * Bind the text path explicitly - the boot found no framebuffer at all, or a
 * caller knows the firmware's is not ours to use.  `serial_console` says
 * whether the serial stream can carry the same lines.
 */
void display_bind_text_fallback(const char *reason, bool serial_console);
/*
 * A backend may be registered without a probe result: the text path is the
 * default state, so a machine that never calls the probe still reports an
 * honest `UNAVAILABLE` instead of an empty capability.
 */
void display_reset(void);
const struct display_capabilities *display_capabilities(void);
/* True when drawing may go to the framebuffer. */
bool display_framebuffer_usable(void);
/* The text path always exists; this says whether a serial stream joins it. */
bool display_text_available(void);
/* 0 when no framebuffer is bound, otherwise its width/height. */
uint64_t display_width(void);
uint64_t display_height(void);
/* The glyph scale the chosen resolution wants; 0 when there is no framebuffer. */
uint32_t display_scale(void);

/*
 * Drawing, through whichever backend is bound.
 *
 * A caller never asks which one it got: on a machine with a usable framebuffer
 * these write pixels into firmware memory, and on any other machine they draw
 * nothing at all - the caller has already been told, through
 * `display_capabilities()`, that the flow is being carried as text, and the
 * installer renders that text with the same lines it would have painted.
 */
void display_clear(uint32_t colour);
void display_rect(uint64_t x, uint64_t y, uint64_t width, uint64_t height,
                  uint32_t colour);
void display_text(uint64_t x, uint64_t y, const char *text, uint32_t colour,
                  uint32_t scale);
void display_u64(uint64_t x, uint64_t y, uint64_t value, uint32_t colour,
                 uint32_t scale);
/* Push what was drawn to the screen; a text backend has nothing to push. */
void display_present(void);

/*
 * The installer's own reading of the display, as one bounded record:
 *
 *   [DISPLAY] backend=<GOP|TEXT|NONE> status=<AVAILABLE|DEGRADED|UNAVAILABLE>
 *             resolution=<WxH|NONE> pitch=<n> bpp=<n> format=<name>
 *             fallback=<TEXT|SERIAL|NONE> reason=<word>
 *
 * `resolution=NONE` and `format=UNKNOWN` are the honest readings of a machine
 * with no framebuffer, and the word that says why is always present.  Returns 0
 * when the record would not fit the bound.
 */
size_t display_format_record(char *buffer, size_t size);
const char *display_backend_name(enum display_backend backend);
const char *display_status_name(enum display_status status);
const char *display_format_name(enum display_pixel_format format);
const char *display_probe_result_name(enum display_probe_result result);

#endif
