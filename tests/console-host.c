#include "console.h"
#include "appliance_admin_credentials_prompt.h"
#include "console_credentials_command.h"
#include "console_gate_command.h"
#include "console_help_command.h"
#include "console_language_command.h"
#include "appliance_identity.h"
#include "framebuffer.h"
#include "hfoc_build.h"
#include "ipv4_config.h"
#include "heap.h"
#include "keyboard.h"
#include "local_menu.h"
#include "pmm.h"
#include "usb_hid.h"
#include "vmm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static char serial_capture[65536];
static size_t serial_length;
/*
 * ITEM-440: whether each captured character was written inside a
 * serial_sequence_begin()/serial_sequence_end() pair.  The sequence helpers
 * are stubs here, so the proof makes them observable: the depth is counted,
 * every character records the depth it was written at, and a marker asserted
 * with serial_contains_atomic() is one the operator would have read whole.
 */
static unsigned char serial_capture_sequenced[65536];
static unsigned int serial_sequence_depth;
#ifdef HFOC_CONSOLE_HOST_TEST
uint32_t console_test_scrollback_line_count(void);
size_t console_test_scrollback_line_length(uint32_t offset);
bool console_test_pager_active(void);
uint32_t console_test_pager_top(void);
uint32_t console_test_pager_bottom(void);
#endif

static int failures;

const char *appliance_host_seed_status_string(
    enum appliance_host_seed_status status) {
    switch (status) {
    case APPLIANCE_HOST_SEED_OK:
        return "OK";
    case APPLIANCE_HOST_SEED_WRONG_PASSWORD:
        return "WRONG_PASSWORD";
    case APPLIANCE_HOST_SEED_UNSUPPORTED:
    default:
        return "UNSUPPORTED";
    }
}

const char *appliance_production_admin_enrollment_status_string(
    enum appliance_production_admin_enrollment_status status) {
    switch (status) {
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_THROTTLED:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_THROTTLED";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNAVAILABLE:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNAVAILABLE";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_SECRET:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_SECRET";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNPROVISIONED:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNPROVISIONED";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_LEGACY_RECORD:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_LEGACY_RECORD";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_CREDENTIAL_REQUIRED:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_CREDENTIAL_REQUIRED";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_PUBLISH_FAILED:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_PUBLISH_FAILED";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_SESSION_FAILED:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_SESSION_FAILED";
    case APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_STATE:
    default:
        return "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_BAD_STATE";
    }
}

const char *
appliance_production_entropy_seed_status_string(
    enum appliance_production_entropy_seed_status status) {
    switch (status) {
    case APPLIANCE_PRODUCTION_ENTROPY_SEED_OK:
        return "OK";
    case APPLIANCE_PRODUCTION_ENTROPY_SEED_NOT_AVAILABLE:
        return "NOT_AVAILABLE";
    case APPLIANCE_PRODUCTION_ENTROPY_SEED_FAILED:
        return "FAILED";
    case APPLIANCE_PRODUCTION_ENTROPY_SEED_ALREADY_READY:
    default:
        return "ALREADY_READY";
    }
}

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } \
} while (0)

static void reset_serial(void) {
    serial_length = 0U;
    serial_capture[0] = '\0';
    serial_sequence_depth = 0U;
    memset(serial_capture_sequenced, 0, sizeof(serial_capture_sequenced));
}

/* ITEM-440: the same search as serial_contains(), but every character of the
   match must have been written with a serial sequence open - i.e. without an
   IRQ trace line able to land inside it. */
static bool serial_contains_atomic(const char *needle) {
    size_t index = 0U;

    while (serial_capture[index] != '\0') {
        size_t offset = 0U;

        while (serial_capture[index + offset] != '\0' &&
               needle[offset] != '\0' &&
               serial_capture[index + offset] == needle[offset] &&
               serial_capture_sequenced[index + offset] != 0U) {
            ++offset;
        }
        if (needle[offset] == '\0') {
            return true;
        }
        ++index;
    }
    return false;
}

static bool serial_contains(const char *needle) {
    size_t index = 0U;
    while (serial_capture[index] != '\0') {
        size_t offset = 0U;
        while (serial_capture[index + offset] != '\0' &&
               needle[offset] != '\0' &&
               serial_capture[index + offset] == needle[offset]) {
            ++offset;
        }
        if (needle[offset] == '\0') {
            return true;
        }
        ++index;
    }
    return false;
}

bool framebuffer_ready(void) {
    return true;
}

uint64_t framebuffer_width(void) {
    return 1024U;
}

uint64_t framebuffer_height(void) {
    return 768U;
}

void framebuffer_clear(uint32_t colour) {
    (void)colour;
}

void framebuffer_rect(uint64_t x, uint64_t y, uint64_t width,
                      uint64_t height, uint32_t colour) {
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)colour;
}

/*
 * ITEM-405: the framebuffer is the surface the console mirror paints, so the
 * host proof records the characters that reach it.  The capture is the order
 * the console painted them in, which is what the mirror's assertions are
 * about: what an operator would read on screen, and where in the boot it
 * appeared.
 */
static char screen_capture[65536];
static size_t screen_length;
/*
 * ITEM-648: the header is painted with the text/u64 calls rather than the
 * body's character stream, so the proof records those calls separately.  The
 * x coordinate is what distinguishes the left title from the removed right
 * status block; the text is kept so the left title can be required.
 */
static char header_capture[4096];
static size_t header_length;
static uint64_t header_text_max_x;
static uint64_t header_u64_max_x;
/*
 * ITEM-509 increment 3: which rows the paint reached.  The proofs before this
 * one could not see a wrap on the host at all - the screen capture is a stream
 * of characters with no position - so nothing could tell a line that wrapped by
 * code points from one that wrapped twice.  These counters give the paint its
 * rows back: a Cyrillic line of N code points must reach exactly as many rows
 * as a Latin line of N code points.
 */
static uint64_t screen_last_y;
static bool screen_have_y;
static unsigned int screen_row_runs;

static void screen_note_row(uint64_t y) {
    if (!screen_have_y || y != screen_last_y) {
        screen_have_y = true;
        screen_last_y = y;
        ++screen_row_runs;
    }
}

void framebuffer_char(uint64_t x, uint64_t y, char value,
                      uint32_t colour, uint32_t scale) {
    (void)x;
    (void)colour;
    (void)scale;
    screen_note_row(y);
    if (screen_length + 1U < sizeof(screen_capture)) {
        screen_capture[screen_length++] = value;
        screen_capture[screen_length] = '\0';
    }
}

/*
 * ITEM-509 increment 3: the console now paints code points.  This proof has no
 * font, so the stub carries the same contract the renderer does - a code point
 * either has a glyph or is counted as missing - with the set the console proof
 * exercises: printable ASCII, the Cyrillic letters, the Russian punctuation.
 * The painted code points are recorded in order, so a test can require one cell
 * per code point rather than one per byte.
 */
static uint64_t stub_glyph_missing_count;
static uint32_t stub_glyph_missing_last;
static uint32_t screen_codepoints[4096];
static size_t screen_codepoint_count;

static bool stub_glyph_present(uint32_t codepoint) {
    /*
     * The seven printable bytes the font never carried ($ & ^ ` { }) are
     * absent here as they are in the renderer, so the proof can see the marker
     * they now produce instead of reading a silent gap as text.
     */
    if (codepoint >= 0x20U && codepoint < 0x7FU) {
        switch ((char)codepoint) {
            case '$': case '&': case '^': case '`': case '{': case '}':
            case '~':
                return false;
            default:
                return true;
        }
    }
    if (codepoint >= 0x0410U && codepoint <= 0x044FU) {
        return true;
    }
    if (codepoint == 0x0401U || codepoint == 0x0451U) {
        return true;
    }
    switch (codepoint) {
        case 0x00ABU: case 0x00BBU: case 0x2014U: case 0x2026U:
        case 0x2116U:
            return true;
        default:
            return false;
    }
}

bool framebuffer_glyph_rows(uint32_t codepoint, uint8_t rows[16]) {
    if (rows != NULL) {
        memset(rows, 0, 16U);
    }
    return stub_glyph_present(codepoint);
}

uint64_t framebuffer_glyph_missing_count(void) {
    return stub_glyph_missing_count;
}

uint32_t framebuffer_glyph_missing_last(void) {
    return stub_glyph_missing_last;
}

bool framebuffer_char_codepoint(uint64_t x, uint64_t y, uint32_t codepoint,
                                uint32_t colour, uint64_t scale) {
    (void)x;
    (void)colour;
    (void)scale;
    screen_note_row(y);
    if (screen_codepoint_count < sizeof(screen_codepoints) /
                                     sizeof(screen_codepoints[0])) {
        screen_codepoints[screen_codepoint_count++] = codepoint;
    }
    if (!stub_glyph_present(codepoint)) {
        ++stub_glyph_missing_count;
        stub_glyph_missing_last = codepoint;
        if (screen_length + 1U < sizeof(screen_capture)) {
            screen_capture[screen_length++] = '?';
            screen_capture[screen_length] = '\0';
        }
        return false;
    }
    /* The cell the operator reads: the code point's own UTF-8 bytes. */
    if (codepoint < 0x80U) {
        if (screen_length + 1U < sizeof(screen_capture)) {
            screen_capture[screen_length++] = (char)codepoint;
            screen_capture[screen_length] = '\0';
        }
        return true;
    }
    {
        char encoded[4];
        size_t length = 0U;

        if (codepoint < 0x800U) {
            encoded[length++] = (char)(0xC0U | (codepoint >> 6));
            encoded[length++] = (char)(0x80U | (codepoint & 0x3FU));
        } else if (codepoint < 0x10000U) {
            encoded[length++] = (char)(0xE0U | (codepoint >> 12));
            encoded[length++] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
            encoded[length++] = (char)(0x80U | (codepoint & 0x3FU));
        } else {
            encoded[length++] = (char)(0xF0U | (codepoint >> 18));
            encoded[length++] = (char)(0x80U | ((codepoint >> 12) & 0x3FU));
            encoded[length++] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
            encoded[length++] = (char)(0x80U | (codepoint & 0x3FU));
        }
        for (size_t index = 0U; index < length; ++index) {
            if (screen_length + 1U < sizeof(screen_capture)) {
                screen_capture[screen_length++] = encoded[index];
                screen_capture[screen_length] = '\0';
            }
        }
    }
    return true;
}

void framebuffer_text(uint64_t x, uint64_t y, const char *text,
                      uint32_t colour, uint32_t scale) {
    (void)y;
    (void)colour;
    (void)scale;
    if (x > header_text_max_x) {
        header_text_max_x = x;
    }
    while (text != NULL && *text != '\0' &&
           header_length + 1U < sizeof(header_capture)) {
        header_capture[header_length++] = *text++;
        header_capture[header_length] = '\0';
    }
}

void framebuffer_u64(uint64_t x, uint64_t y, uint64_t value,
                     uint32_t colour, uint32_t scale) {
    char digits[32];
    size_t length = 0U;

    (void)y;
    (void)colour;
    (void)scale;
    if (x > header_u64_max_x) {
        header_u64_max_x = x;
    }
    do {
        digits[length++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U && length < sizeof(digits));
    while (length != 0U && header_length + 1U < sizeof(header_capture)) {
        header_capture[header_length++] = digits[--length];
        header_capture[header_length] = '\0';
    }
}

void framebuffer_hex64(uint64_t x, uint64_t y, uint64_t value,
                       uint32_t colour, uint32_t scale) {
    (void)x;
    (void)y;
    (void)value;
    (void)colour;
    (void)scale;
}

/*
 * ITEM-464: the console drains the write-combining framebuffer before it reads
 * a cost.  This proof has no pixels and no cost to drain, so the call is a
 * no-op that only has to exist for the link.  ITEM-480: it counts the calls, so
 * the batch proof can show the drain happens once per batch rather than once
 * per coalesced report.
 */
static unsigned int framebuffer_sync_calls;
void framebuffer_sync(void) {
    ++framebuffer_sync_calls;
}

/*
 * ITEM-466: the console asks the mode for a glyph scale.  This proof has no
 * real framebuffer.c, so the stub keeps the 2x cell the console's geometry was
 * written against (1024x768 -> 1x under the real rule would change every row
 * count in the cases below); the pure rule itself is pinned by
 * tests/framebuffer-scroll-host.c, which links the real table.
 *
 * ITEM-470: the pointer cases pin the 1:1 arithmetic explicitly by asking for
 * scale 1 around their own `console_init()`, and one case keeps the stub's 2x
 * mode to prove the console applies the scale it publishes.
 */
static uint32_t host_font_scale = 2U;

uint32_t framebuffer_font_scale(uint64_t width, uint64_t height) {
    (void)width;
    (void)height;
    return host_font_scale;
}

/* ITEM-466: the same proof has no firmware mode list, so the `available=`
   field is empty here; the real list walk is pinned by
   tests/framebuffer-scroll-host.c and the pure rule's host case below. */
uint64_t framebuffer_available_text(char *output, size_t capacity) {
    if (output != NULL && capacity != 0U) {
        output[0] = '\0';
    }
    return 0U;
}

/*
 * ITEM-457: the console publishes the bytes its own scroll wrote, and this
 * proof has no pixels - so the stub counts the calls and returns a fixed,
 * known size, which is what makes the record's `bytes=` field exactly
 * checkable.  A line that still fits must not call the stub at all.
 */
#define SCROLL_STUB_BYTES 2048U
static unsigned int framebuffer_scroll_calls;
/* ITEM-482: how many body moves started while the arrow was still painted.
   The console must hide the pointer first, or the row copy carries the sprite
   up and the saved block restores stale pixels - the ghost the owner saw. */
static unsigned int scrolls_with_visible_cursor;
static bool cursor_visible_stub;

uint64_t framebuffer_scroll_up(uint64_t top, uint64_t pixels,
                               uint32_t fill_colour) {
    (void)top;
    (void)pixels;
    (void)fill_colour;
    ++framebuffer_scroll_calls;
    if (cursor_visible_stub) {
        ++scrolls_with_visible_cursor;
    }
    return SCROLL_STUB_BYTES;
}

/*
 * ITEM-448: the software pointer is pixels, and this proof has no pixels - it
 * records the arrow's own calls instead, so the click path can be driven
 * without a framebuffer.
 */
static unsigned int cursor_shows;
static unsigned int cursor_hides;
static int32_t cursor_last_x;
static int32_t cursor_last_y;

bool framebuffer_cursor_show(uint64_t x, uint64_t y) {
    cursor_last_x = (int32_t)x;
    cursor_last_y = (int32_t)y;
    cursor_visible_stub = true;
    ++cursor_shows;
    return true;
}

void framebuffer_cursor_hide(void) {
    cursor_visible_stub = false;
    ++cursor_hides;
}

bool framebuffer_cursor_visible(void) {
    return cursor_visible_stub;
}

/* ITEM-610: the credential check presents the frame before the long work; the
   host proof has no video memory, so the flush is a no-op it counts. */
static unsigned int framebuffer_flush_calls;
void framebuffer_flush(void) {
    ++framebuffer_flush_calls;
}

void serial_putc(char value) {
    if (serial_length + 1U < sizeof(serial_capture)) {
        serial_capture[serial_length] = value;
        serial_capture_sequenced[serial_length] =
            serial_sequence_depth > 0U ? 1U : 0U;
        ++serial_length;
        serial_capture[serial_length] = '\0';
    }
}

uint64_t serial_sequence_begin(void) {
    ++serial_sequence_depth;
    return 0U;
}

void serial_sequence_end(uint64_t flags) {
    (void)flags;
    if (serial_sequence_depth > 0U) {
        --serial_sequence_depth;
    }
}

void serial_write(const char *text) {
    while (*text != '\0') {
        serial_putc(*text++);
    }
}

void serial_write_u64(uint64_t value) {
    char buffer[21];
    size_t length = 0U;
    if (value == 0U) {
        serial_putc('0');
        return;
    }
    while (value != 0U) {
        buffer[length++] = (char)('0' + value % 10U);
        value /= 10U;
    }
    while (length != 0U) {
        serial_putc(buffer[--length]);
    }
}

void serial_write_hex(uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    int shift;
    serial_putc('0');
    serial_putc('x');
    for (shift = 60; shift >= 0; shift -= 4) {
        serial_putc(digits[(value >> (unsigned int)shift) & 0x0FU]);
    }
}

/*
 * ITEM-457: the scroll measurement reads the machine's tick, and this proof
 * has no PIT - so the stub reads from a value the proof controls and advances
 * by a settable step.  A scroll reads it twice, so a step of one tick makes
 * every scroll of the case measure exactly 10 ms and the record's `ms=` and
 * `max_ms=` are numbers the proof states, not ones it tolerates.
 */
static uint64_t timer_ticks_value = 42U;
static uint64_t timer_ticks_step;

uint64_t timer_ticks(void) {
    timer_ticks_value += timer_ticks_step;
    return timer_ticks_value;
}

/*
 * ITEM-463: the cycle counter the console measures microseconds with.  This
 * proof has no TSC, so the stub advances by a value the case controls and
 * converts one cycle to one microsecond; a step of one makes every measured
 * scroll exactly 1 us, which the record's `us=`/`max_us=` state.
 */
static uint64_t timer_cycles_value = 1000U;
static uint64_t timer_cycles_step = 1U;

uint64_t timer_cycles(void) {
    timer_cycles_value += timer_cycles_step;
    return timer_cycles_value;
}

uint64_t timer_cycles_to_us(uint64_t cycles) {
    return cycles;
}

uint64_t timer_uptime_seconds(void) {
    return 7U;
}

/*
 * ITEM-472: the PIT-calibrated wall clock the pointer's once-a-second record
 * reads, and the measured IRQ tick rate the loop diagnostic publishes.  The
 * case below advances `timer_real_seconds_value` to cross a real second.
 */
static uint64_t timer_real_seconds_value = 100U;

uint64_t timer_real_seconds(void) {
    return timer_real_seconds_value;
}

uint64_t timer_measured_tick_hz(void) {
    return 0U;
}

uint8_t pic_master_mask(void) {
    return 0xffU;
}

uint8_t pic_slave_mask(void) {
    return 0xffU;
}

uint64_t keyboard_irq_count(void) {
    return 0U;
}

uint8_t keyboard_last_scancode(void) {
    return 0U;
}

/*
 * ITEM-395: the console prints the live local input path in its banner and in
 * the KBD command. The renderer belongs to kernel/src/local_input.c and the USB
 * counters to kernel/src/xhci.c; this proof does not link either, so the stubs
 * below return a fixed line and fixed counters, and what is asserted is the
 * console's own output contract (the text and the order it prints).
 */
size_t local_input_render(char *buffer, size_t size) {
    static const char rendered[] =
        "LOCAL INPUT: SOURCE=USB_HID USB=READY SPEED=HIGH VID=0x627 PID=0x1"
        " PS2=CONTROLLER_ABSENT\n";
    const size_t length = sizeof(rendered) - 1U;

    if (buffer != NULL && size != 0U) {
        const size_t copy = length < size - 1U ? length : size - 1U;

        for (size_t index = 0U; index < copy; ++index) {
            buffer[index] = rendered[index];
        }
        buffer[copy] = '\0';
    }
    return length;
}

uint32_t xhci_keyboard_report_count(void) {
    return 7U;
}

/*
 * ITEM-519 increment 3: the console host proof links console.c without the
 * xHCI driver, so the storage write proof is answered with the driver's
 * refusal verdict here.  The write path itself is the driver's and is proven
 * by `make test-usb-storage-host` and the QEMU phase B, not by this stub.
 */
bool xhci_storage_write_proof(void) {
    return false;
}

/*
 * ITEM-519 increment 5: the same host proof links console.c without the xHCI
 * driver, so the medium probe reports the driver's refusal name here.  The
 * probe itself is the driver's and is proven by the install-medium host proof
 * and the QEMU proofs, not by this stub.
 */
bool xhci_storage_medium_publish(void) {
    return false;
}

const char *xhci_storage_medium_result(void) {
    return "UNCHECKED";
}

uint64_t xhci_keyboard_transfer_count(void) {
    return 8U;
}

uint32_t xhci_keyboard_error_count(void) {
    return 0U;
}

/*
 * ITEM-436: the two numbers that name a keyboard which is bound but not
 * answering. The values are distinct from the counters above and from each
 * other, so the console assertion proves the line reads the driver's numbers
 * rather than printing constants.
 */
uint32_t xhci_keyboard_refused_count(void) {
    return 2U;
}

uint32_t xhci_keyboard_last_report_bytes(void) {
    return 4U;
}

/*
 * ITEM-438: the keys those reports carried, which the `kbd` command prints as
 * KEYS= next to the counters above. A value distinct from all of them, so the
 * console assertion proves the line reads the driver's number.
 */
uint32_t xhci_keyboard_key_count(void) {
    return 5U;
}

/*
 * ITEM-458: the two loss counters the `kbd` command now prints.  Both are
 * distinct from every counter above, so the console assertion proves the line
 * reads the driver's numbers rather than printing constants.
 */
uint32_t xhci_keyboard_dropped_count(void) {
    return 0U;
}

uint32_t xhci_keyboard_gap_ms_max(void) {
    return 30U;
}

/*
 * ITEM-473 (СРОЧНО-23): `ARM` shows and sets the mouse's armed-transfer depth.
 * This proof does not link kernel/src/xhci.c, so the stub keeps the value the
 * command last set and the test reads it back through the console's answer.
 */
static uint32_t console_arm_depth_stub = 16U;

uint32_t xhci_mouse_arm_depth(void) {
    return console_arm_depth_stub;
}

void xhci_set_mouse_arm_depth(uint32_t requested) {
    console_arm_depth_stub = requested;
}

uint32_t xhci_mouse_armed_count(void) {
    return 16U;
}

/* ITEM-461/463: the main loop's turn cost, owned by kernel.c. */
uint64_t kernel_loop_iter_us(void) {
    return 7U;
}

uint64_t kernel_loop_iter_max_us(void) {
    return 9U;
}

void interrupts_make_pf_gate_not_present(void) {
}

uint64_t vmm_hhdm_offset(void) {
    return 0U;
}

enum vmm_status vmm_map_mmio_page(uint64_t physical_address,
                                  uint64_t hhdm_offset,
                                  volatile void **out_virtual) {
    (void)physical_address;
    (void)hhdm_offset;
    (void)out_virtual;
    return VMM_OK;
}

const char *vmm_status_string(enum vmm_status status) {
    (void)status;
    return "host VMM status";
}

uint64_t pmm_total_usable_bytes(void) {
    return 0x310000ULL;
}

uint64_t pmm_free_page_count(void) {
    return 191U;
}

/* ITEM-390: the console pmm command publishes the coverage state. */
static const struct pmm_init_diagnostic console_host_pmm_diagnostic = {
    .status = PMM_INIT_STATUS_OK,
    .coverage = PMM_COVERAGE_FULL,
    .usable_bytes = 0x310000ULL,
    .tracked_bytes = 0x300000ULL,
    .dropped_pages = 0U,
    .total_pages = 768U,
    .page_cap = (uint64_t)HFOC_PMM_MAX_PAGES,
    .region_count = 1U,
    .dropped_regions = 0U,
    .bitmap_pages = 1U,
    .largest_region_pages = 768U,
    .placement_region = 0U,
    .bitmap_phys = 0x100000ULL
};

const struct pmm_init_diagnostic *pmm_init_diagnostic(void) {
    return &console_host_pmm_diagnostic;
}

const char *pmm_coverage_string(enum pmm_coverage coverage) {
    switch (coverage) {
    case PMM_COVERAGE_FULL:
        return "FULL";
    case PMM_COVERAGE_LIMITED:
        return "LIMITED";
    case PMM_COVERAGE_NONE:
    default:
        return "NONE";
    }
}

bool heap_ready(void) {
    return true;
}

size_t heap_capacity_bytes(void) {
    return HFOC_HEAP_SIZE;
}

size_t heap_used_bytes(void) {
    return 0U;
}

/*
 * ITEM-506: the console account conversation timestamps a verification and a
 * change; the host proof pins one tick so the binding sees a defined value.
 */
uint32_t hfoc_lwip_time_now(void) {
    return 1000U;
}

int strcmp(const char *left, const char *right) {
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return (unsigned char)*left - (unsigned char)*right;
}

_Noreturn void kernel_panic(const char *message) {
    fprintf(stderr, "Unexpected kernel panic in console host test: %s\n",
            message);
    __builtin_unreachable();
}

static void send_event(enum keyboard_event_type type, char character) {
    const struct keyboard_event event = { type, character, 0U };
    console_process_event(&event);
}

static void send_char(char character) {
    send_event(KEYBOARD_EVENT_CHARACTER, character);
}

static void send_text(const char *text) {
    while (text != NULL && *text != '\0') {
        send_char(*text++);
    }
}

static bool line_is(const char *expected) {
    const char *line = console_test_line();
    while (*expected != '\0') {
        if (*line != *expected) {
            return false;
        }
        ++expected;
        ++line;
    }
    return *line == '\0';
}

static void submit_command(const char *command) {
    while (*command != '\0') {
        send_char(*command++);
    }
    send_event(KEYBOARD_EVENT_ENTER, '\0');
}

static void reset_screen(void) {
    screen_length = 0U;
    screen_capture[0] = '\0';
    /* ITEM-509 increment 3: the code-point record and the rows start clean. */
    screen_codepoint_count = 0U;
    screen_last_y = 0U;
    screen_have_y = false;
    screen_row_runs = 0U;
    header_length = 0U;
    header_capture[0] = '\0';
    header_text_max_x = 0U;
    header_u64_max_x = 0U;
}

static bool screen_contains(const char *needle) {
    size_t index = 0U;
    while (screen_capture[index] != '\0') {
        size_t offset = 0U;
        while (screen_capture[index + offset] != '\0' &&
               needle[offset] != '\0' &&
               screen_capture[index + offset] == needle[offset]) {
            ++offset;
        }
        if (needle[offset] == '\0') {
            return true;
        }
        ++index;
    }
    return false;
}

static bool header_contains(const char *needle) {
    size_t index = 0U;
    while (header_capture[index] != '\0') {
        size_t offset = 0U;
        while (header_capture[index + offset] != '\0' &&
               needle[offset] != '\0' &&
               header_capture[index + offset] == needle[offset]) {
            ++offset;
        }
        if (needle[offset] == '\0') {
            return true;
        }
        ++index;
    }
    return false;
}

static size_t screen_occurrences(const char *needle) {
    size_t count = 0U;
    size_t index = 0U;
    while (screen_capture[index] != '\0') {
        size_t offset = 0U;
        while (screen_capture[index + offset] != '\0' &&
               needle[offset] != '\0' &&
               screen_capture[index + offset] == needle[offset]) {
            ++offset;
        }
        if (needle[offset] == '\0') {
            ++count;
        }
        ++index;
    }
    return count;
}

static void begin_case(void) {
    console_test_reset();
    console_reset_screen_budget();
    reset_serial();
    reset_screen();
}

/*
 * ITEM-618: the local console keeps a bounded text history and `scroll` shows
 * it page by page without blocking the main loop.  The top, bottom and the
 * return to the prompt are all exercised through the real key events.
 */
static void test_scrollback_pager(void) {
    char line[64];

    begin_case();
    console_write("BOUNDARY-A");
    console_putc('\n');
    console_write("BOUNDARY-B");
    console_putc('\n');
    CHECK(console_test_scrollback_line_count() == 2U,
          "two newline-terminated writes are two scrollback lines");
    for (unsigned int index = 0U; index < 120U; ++index) {
        snprintf(line, sizeof(line), "PAGE-LINE-%03u\n", index);
        console_write(line);
    }
    submit_command("scroll");
    CHECK(screen_contains("PAGE-LINE-119"),
          "the scroll view opens on the newest line");
    CHECK(screen_contains("UP/DOWN"), "the scroll view names its keys");
    send_event(KEYBOARD_EVENT_HOME, '\0');
    CHECK(screen_contains("PAGE-LINE-000"),
          "HOME reaches the oldest retained line");
    send_event(KEYBOARD_EVENT_END, '\0');
    CHECK(screen_contains("PAGE-LINE-119"),
          "END returns to the newest line");
    send_char('q');
    CHECK(screen_contains("hfoc>"), "Q returns to the live prompt");
}

/*
 * ITEM-621: the decoded wheel is a real UI input, not a ignored field.
 *
 * The case drives the production single-report entry point - the same one the
 * xHCI decoder and the batch consumer feed - and reads the real pager state and
 * the real painted screen.  Wheel up opens the retained history at its newest
 * page and walks towards the oldest line; wheel down walks back and returns to
 * the live prompt at the bottom.  The keyboard viewer keeps its own case above.
 */
static void test_mouse_wheel_scrollback(void) {
    struct usb_hid_mouse_event event;
    char line[64];
    unsigned int index;

    begin_case();
    /* The mouse entry point paints, so the case needs a ready framebuffer. */
    host_font_scale = 1U;
    console_init(512U, 1U);
    host_font_scale = 2U;
    for (index = 0U; index < 120U; ++index) {
        snprintf(line, sizeof(line), "WHEEL-LINE-%03u\n", index);
        console_write(line);
    }
    CHECK(!console_test_pager_active(),
          "the console starts on the live screen, not in the viewer");
    memset(&event, 0, sizeof(event));
    event.wheel = 1;
    console_process_mouse_event(&event);
    CHECK(console_test_pager_active(),
          "wheel up opens the retained history");
    CHECK(screen_contains("WHEEL-LINE-119"),
          "the opened viewer shows the newest page");
    {
        const uint32_t bottom = console_test_pager_bottom();
        const uint32_t first_top = console_test_pager_top();

        CHECK(bottom > 3U && first_top == bottom - 3U,
              "one notch moves the viewport up by the pinned row step");
    }
    for (index = 0U; index < 200U; ++index) {
        memset(&event, 0, sizeof(event));
        event.wheel = 1;
        console_process_mouse_event(&event);
    }
    CHECK(console_test_pager_top() == 0U,
          "repeated wheel up stops at the oldest retained line");
    CHECK(screen_contains("WHEEL-LINE-000"),
          "the oldest page is actually painted");
    {
        const uint32_t bottom = console_test_pager_bottom();

        for (index = 0U; index < 240U && console_test_pager_active() &&
                         console_test_pager_top() < bottom; ++index) {
            memset(&event, 0, sizeof(event));
            event.wheel = -1;
            console_process_mouse_event(&event);
        }
        CHECK(console_test_pager_active(),
              "wheel down stops on the newest page instead of overrunning it");
        CHECK(console_test_pager_top() == bottom,
              "the viewport lands exactly on the newest page");
        CHECK(screen_contains("WHEEL-LINE-119"),
              "the newest page is visible at the bottom limit");
    }
    memset(&event, 0, sizeof(event));
    event.wheel = -1;
    console_process_mouse_event(&event);
    CHECK(!console_test_pager_active(),
          "one more wheel down returns to the live prompt");
    CHECK(screen_contains("hfoc>"),
          "the live prompt is back after the wheel viewer");
}

static void test_insert_middle(void) {
    begin_case();

    send_char('a');
    send_char('c');
    send_event(KEYBOARD_EVENT_LEFT, '\0');
    send_char('b');

    CHECK(console_test_cursor() == 2U, "middle insertion cursor");
    const char *line = console_test_line();
    CHECK(line[0] == 'a' && line[1] == 'b' && line[2] == 'c' &&
              line[3] == '\0',
          "middle insertion produces abc");
}

static void test_backspace_delete(void) {
    begin_case();

    send_char('a');
    send_char('b');
    send_char('c');
    send_event(KEYBOARD_EVENT_LEFT, '\0');
    send_event(KEYBOARD_EVENT_LEFT, '\0');
    send_event(KEYBOARD_EVENT_BACKSPACE, '\0');

    CHECK(console_test_cursor() == 0U, "backspace cursor");
    const char *line = console_test_line();
    CHECK(line[0] == 'b' && line[1] == 'c' && line[2] == '\0',
          "backspace removes byte before cursor");

    send_event(KEYBOARD_EVENT_HOME, '\0');
    send_event(KEYBOARD_EVENT_DELETE, '\0');
    line = console_test_line();
    CHECK(line[0] == 'c' && line[1] == '\0',
          "delete removes byte at cursor");
    CHECK(console_test_cursor() == 0U, "delete keeps cursor position");
}

static void test_left_right_saturation(void) {
    begin_case();

    send_char('a');
    send_char('b');
    send_char('c');
    send_event(KEYBOARD_EVENT_LEFT, '\0');
    send_event(KEYBOARD_EVENT_LEFT, '\0');
    send_event(KEYBOARD_EVENT_LEFT, '\0');
    send_event(KEYBOARD_EVENT_LEFT, '\0');
    CHECK(console_test_cursor() == 0U, "Left saturates at zero");

    send_event(KEYBOARD_EVENT_RIGHT, '\0');
    send_event(KEYBOARD_EVENT_RIGHT, '\0');
    send_event(KEYBOARD_EVENT_RIGHT, '\0');
    send_event(KEYBOARD_EVENT_RIGHT, '\0');
    CHECK(console_test_cursor() == 3U, "Right saturates at length");
    CHECK(console_test_line()[0] == 'a' &&
              console_test_line()[1] == 'b' &&
              console_test_line()[2] == 'c',
          "cursor movement preserves line");
}

static void test_home_end(void) {
    begin_case();

    send_char('a');
    send_char('b');
    send_event(KEYBOARD_EVENT_HOME, '\0');
    CHECK(console_test_cursor() == 0U, "Home sets cursor to zero");
    send_event(KEYBOARD_EVENT_END, '\0');
    CHECK(console_test_cursor() == 2U, "End sets cursor to length");
}

static void test_full_buffer_ignored(void) {
    begin_case();

    /*
     * ITEM-506: the cap is the account policy's 128-character password
     * maximum, so the whole maximum fits and the 129th character is ignored.
     */
    for (unsigned int i = 0U; i < 128U; ++i) {
        send_char('x');
    }
    send_char('y');

    const char *line = console_test_line();
    CHECK(console_test_cursor() == 128U, "full buffer keeps cursor valid");
    CHECK(line[0] == 'x' && line[127] == 'x' && line[128] == '\0',
          "full buffer does not corrupt existing line");
}

static void test_enter_submits_once(void) {
    begin_case();

    send_char('a');
    send_char('b');
    send_char('c');
    send_event(KEYBOARD_EVENT_ENTER, '\0');

    CHECK(console_test_cursor() == 0U, "Enter resets cursor");
    CHECK(line_is(""), "Enter resets line");
}

static void test_empty_history_up_down_ignored(void) {
    begin_case();

    send_char('a');
    send_char('c');
    send_event(KEYBOARD_EVENT_LEFT, '\0');
    send_char('b');
    send_event(KEYBOARD_EVENT_HOME, '\0');
    send_event(KEYBOARD_EVENT_RIGHT, '\0');
    send_event(KEYBOARD_EVENT_UP, '\0');
    send_event(KEYBOARD_EVENT_DOWN, '\0');

    CHECK(line_is("abc"), "empty history leaves line unchanged");
    CHECK(console_test_cursor() == 1U, "empty history leaves cursor unchanged");
}

static void test_history_navigation_and_draft_restore(void) {
    begin_case();

    console_test_add_history("first");
    console_test_add_history("second");
    console_test_add_history("third");

    send_char('d');
    send_char('r');
    send_char('a');
    send_char('f');
    send_char('t');
    send_event(KEYBOARD_EVENT_HOME, '\0');
    send_event(KEYBOARD_EVENT_RIGHT, '\0');
    send_event(KEYBOARD_EVENT_RIGHT, '\0');

    send_event(KEYBOARD_EVENT_UP, '\0');
    CHECK(line_is("third"), "first Up loads newest history entry");
    CHECK(console_test_cursor() == 5U, "recalled entry cursor is its end");

    send_event(KEYBOARD_EVENT_UP, '\0');
    CHECK(line_is("second"), "second Up moves to older entry");
    send_event(KEYBOARD_EVENT_UP, '\0');
    CHECK(line_is("first"), "third Up moves to oldest entry");
    send_event(KEYBOARD_EVENT_UP, '\0');
    CHECK(line_is("first"), "Up saturates at oldest entry");

    send_event(KEYBOARD_EVENT_DOWN, '\0');
    CHECK(line_is("second"), "Down moves to newer entry");
    send_event(KEYBOARD_EVENT_DOWN, '\0');
    CHECK(line_is("third"), "Down moves to newest entry");
    send_event(KEYBOARD_EVENT_DOWN, '\0');
    CHECK(line_is("draft"), "Down after newest restores exact draft");
    CHECK(console_test_cursor() == 2U, "Down after newest restores exact cursor");
}

static void test_empty_enter_not_stored(void) {
    begin_case();

    submit_command("alpha");
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    send_event(KEYBOARD_EVENT_UP, '\0');

    CHECK(line_is("alpha"), "empty Enter does not replace newest history");
}

static void test_identical_newest_not_duplicated(void) {
    begin_case();

    submit_command("first");
    submit_command("second");
    submit_command("second");
    send_event(KEYBOARD_EVENT_UP, '\0');
    send_event(KEYBOARD_EVENT_UP, '\0');

    CHECK(line_is("first"), "identical newest command does not duplicate");

    send_event(KEYBOARD_EVENT_DOWN, '\0');
    CHECK(line_is("second"), "Down returns to newest after deduplication");
}

static void test_equal_older_appends(void) {
    begin_case();

    submit_command("alpha");
    submit_command("beta");
    submit_command("alpha");
    send_event(KEYBOARD_EVENT_UP, '\0');
    CHECK(line_is("alpha"), "equal older command becomes newest entry");
    send_event(KEYBOARD_EVENT_UP, '\0');
    CHECK(line_is("beta"), "equal older command appends without removing older");
    send_event(KEYBOARD_EVENT_UP, '\0');
    CHECK(line_is("alpha"), "oldest equal command remains in ring");
}

static void test_seventeen_commands_ring(void) {
    begin_case();

    static const char *const commands[17] = {
        "cmd01", "cmd02", "cmd03", "cmd04", "cmd05",
        "cmd06", "cmd07", "cmd08", "cmd09", "cmd10",
        "cmd11", "cmd12", "cmd13", "cmd14", "cmd15",
        "cmd16", "cmd17"
    };
    for (size_t index = 0U; index < 17U; ++index) {
        submit_command(commands[index]);
    }

    for (int index = 16; index >= 1; --index) {
        send_event(KEYBOARD_EVENT_UP, '\0');
        CHECK(line_is(commands[index]),
              "17 commands retain only the newest 16");
    }
    send_event(KEYBOARD_EVENT_UP, '\0');
    CHECK(line_is("cmd02"), "Up saturates at oldest retained command");
}

static void test_edited_recalled_command_stored(void) {
    begin_case();

    submit_command("old");
    send_event(KEYBOARD_EVENT_UP, '\0');
    send_event(KEYBOARD_EVENT_BACKSPACE, '\0');
    send_event(KEYBOARD_EVENT_BACKSPACE, '\0');
    send_event(KEYBOARD_EVENT_BACKSPACE, '\0');
    send_char('n');
    send_char('e');
    send_char('w');
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    send_event(KEYBOARD_EVENT_UP, '\0');

    CHECK(line_is("new"), "editing recalled command then Enter stores result");
    send_event(KEYBOARD_EVENT_UP, '\0');
    CHECK(line_is("old"), "original recalled entry is retained");
}

static void test_redraw_middle_insertion(void) {
    begin_case();

    send_char('a');
    send_char('c');
    send_event(KEYBOARD_EVENT_LEFT, '\0');
    send_char('b');

    CHECK(serial_contains("\rhfoc> ac"), "middle redraw shows ac");
    CHECK(serial_contains("\rhfoc> abc"), "middle redraw shows abc");
    CHECK(serial_contains("\b \b"), "middle redraw backs up to cursor");
}

static void test_redraw_delete_clears_stale_cell(void) {
    begin_case();

    send_char('a');
    send_char('b');
    send_char('c');
    send_event(KEYBOARD_EVENT_LEFT, '\0');
    send_event(KEYBOARD_EVENT_DELETE, '\0');

    CHECK(serial_contains("\rhfoc> ab "),
          "delete redraw clears stale rendered cell");
    CHECK(serial_contains("\b \b"),
          "delete redraw returns cursor to logical position");
    CHECK(!serial_contains("\x1b["), "redraw emits no ANSI escape");
}

static void test_appliance_command(void) {
    begin_case();

    if (appliance_identity_boot_init() != APPLIANCE_IDENTITY_OK) {
        CHECK(false, "appliance identity boot init before console command");
        return;
    }
    submit_command("appliance");

    CHECK(serial_contains("appliance v=1\n"),
          "appliance command renders canonical header");
    CHECK(serial_contains(
              "identity product=HFOC-APPLIANCE-V1 arch=x86_64 "
              "platform=q35 hostname=hfoc-appliance\n"),
          "appliance command renders runtime identity");
    CHECK(serial_contains(
              "config state=OK source=DEFAULT_MISSING "
              "hostname=hfoc-appliance\n"),
          "appliance command publishes safe-default configuration");
    CHECK(serial_contains(
              "health state=OK uptime_ticks=42\n"),
          "appliance command renders timer health");
    CHECK(serial_contains(
              "network state=UNAVAILABLE\n"),
          "appliance command reports network unavailable");
    CHECK(serial_contains(
              "services state=UNAVAILABLE\n"),
          "appliance command reports services unavailable");
    CHECK(!serial_contains("[APPLIANCE] SNAPSHOT FAILED") &&
              !serial_contains("[APPLIANCE] RENDER FAILED"),
          "appliance command completes without render failure");
}

static void test_appliance_boot_command(void) {
    begin_case();

    console_init(512U, 1U);
    reset_serial();
    submit_command("appliance-boot");

    CHECK(serial_contains("HFOC Appliance\n"),
          "appliance-boot command renders screen header");
    CHECK(serial_contains("Hostname:      hfoc-appliance\n"),
          "appliance-boot command renders hostname");
    CHECK(serial_contains("CPU:           1\n"),
          "appliance-boot command renders CPU");
    CHECK(serial_contains("Memory:        512 MiB\n"),
          "appliance-boot command renders memory");
    CHECK(serial_contains("Storage:       UNAVAILABLE\n"),
          "appliance-boot command renders unavailable storage");
    CHECK(serial_contains("Network:       UNAVAILABLE\n"),
          "appliance-boot command renders unavailable network");
    CHECK(serial_contains("[1] Status\n") &&
              serial_contains("[2] Network\n") &&
              serial_contains("[3] Diagnostics\n") &&
              serial_contains("[4] Reboot\n") &&
              serial_contains("[5] Shutdown\n") &&
              serial_contains("[6] Recovery\n"),
          "appliance-boot command renders local menu");
    CHECK(!serial_contains("[APPLIANCE-BOOT] SNAPSHOT FAILED") &&
              !serial_contains("[APPLIANCE-BOOT] RENDER FAILED"),
          "appliance-boot command completes without render failure");
}

/*
 * ITEM-518: the `storage` command answers whether or not a state volume exists.
 *
 * `appliance-boot` publishes storage only behind `boot_persistence_published`,
 * so on the machine booted from the installation medium the storage verdict was
 * unreachable without a serial capture.  This command renders the report the
 * boot published - the controllers, the disks and the reason each unusable disk
 * carries - and a machine that never probed says so instead of printing zeroes.
 */
static void test_storage_command(void) {
    struct appliance_storage_report report;

    begin_case();
    console_publish_language(APPLIANCE_LANGUAGE_EN, false);
    console_init(512U, 1U);
    reset_serial();
    submit_command("storage");
    CHECK(serial_contains("Observed storage") &&
              serial_contains("storage was not probed") &&
              serial_contains("state=NOT_PROBED"),
          "without a publication the storage command says not probed");

    appliance_storage_report_init(&report);
    report.probed = true;
    report.class_ahci = 1U;
    report.brought_up = 1U;
    report.disk_count = 1U;
    report.disks[0].present = true;
    report.disks[0].controller = "AHCI";
    memcpy(report.disks[0].identity, "KINGSTON-SA400",
           sizeof("KINGSTON-SA400"));
    report.disks[0].block_size = 512U;
    report.disks[0].block_count = UINT64_C(488397168);
    report.disks[0].reason =
        APPLIANCE_STORAGE_REPORT_UNSUPPORTED_BLOCK_SIZE;
    console_publish_storage_report(&report);
    reset_serial();
    submit_command("storage");
    CHECK(serial_contains(
              "controllers: class_ahci=1 class_nvme=0 brought_up=1") &&
              serial_contains("disks: found=1 accepted=0") &&
              serial_contains("reason=UNSUPPORTED_BLOCK_SIZE"),
          "the storage command prints what the probe saw without a state "
          "volume");
    CHECK(!serial_contains("state=NOT_PROBED"),
          "a published report replaces the not-probed answer");

    console_publish_language(APPLIANCE_LANGUAGE_RU, true);
    reset_serial();
    submit_command("storage");
    CHECK(serial_contains("Наблюдённое хранилище") &&
              serial_contains("контроллеры") &&
              serial_contains("UNSUPPORTED_BLOCK_SIZE"),
          "ITEM-518: the command's labels follow the console language");
    console_publish_language(APPLIANCE_LANGUAGE_EN, false);
    appliance_storage_report_init(&report);
    console_publish_storage_report(&report);
}

/*
 * ITEM-430: the machine offers its own menu.
 *
 * The decision is checked on the host in tests/local-menu-host.c; this is the
 * console's half - the window, what the offer paints, the one key that opens
 * it, and the promise that typing is never hijacked.  The clock is passed in,
 * so the idle window costs the test nothing.
 */
static void test_local_menu_offer(void) {
    /* The idle window, the paint, and ENTER on the empty line. */
    begin_case();
    console_publish_input_source(LOCAL_INPUT_SOURCE_PS2);
    console_publish_local_input(true);
    console_init(512U, 1U);
    console_offer_local_menu(1000U);
    CHECK(!serial_contains("[LOCAL-MENU] offered"),
          "no menu offer the moment the console is ready");
    console_offer_local_menu(1000U + LOCAL_MENU_OFFER_MS - 1U);
    CHECK(!serial_contains("[LOCAL-MENU] offered"),
          "no menu offer one millisecond before the window closes");
    console_offer_local_menu(1000U + LOCAL_MENU_OFFER_MS);
    CHECK(serial_contains(
              "[LOCAL-MENU] offered reason=IDLE entries=6 "
              "keys=s,n,d,r,p,y\n"),
          "the untouched keyboard is offered the menu after the window");
    /*
     * ITEM-455: the offer says how to open the menu *and* which key opens the
     * network editor - the one place the static address is set - because the
     * owner looked for that gesture and the offer named only ENTER.
     */
    CHECK(serial_contains("PRESS ENTER FOR THE LOCAL MENU "
                          "(N: NETWORK SETUP - SET THE ADDRESS THERE)\n"),
          "the offer says how to open the menu and names the network key");
    CHECK(serial_contains("[1] Status") &&
              serial_contains("[6] Recovery"),
          "the offer paints the menu it offers");
    /*
     * ITEM-440: and it paints it inside one serial sequence.  The characters
     * of a menu line are written one by one, so without the pair an IRQ trace
     * line can land inside it and the marker a QEMU proof waits for never
     * appears whole (observed on 2026-09-17: `  [1] Stat` + `[IOAPIC] irq0 ...`
     * + `us  (s)`).  Each entry, the heading and the footer must be atomic.
     */
    CHECK(serial_contains_atomic("[1] Status"),
          "the first menu entry is painted inside one serial sequence");
    CHECK(serial_contains_atomic("[6] Recovery"),
          "the last menu entry is painted inside one serial sequence");
    CHECK(serial_contains_atomic("UP/DOWN select, ENTER run, Q leave"),
          "the menu footer is painted inside one serial sequence");
    /*
     * ITEM-443: and so are the menu's own records.  The offer, the two lines
     * that explain it and the open/close/cancel markers travel through
     * console_write() one character at a time as well, so they are the same
     * hazard for a proof that waits for them - the keyboard_absent phase of
     * test-qemu-production-storage-boot.sh waits for exactly these lines.
     */
    CHECK(serial_contains_atomic(
              "[LOCAL-MENU] offered reason=IDLE entries=6 keys=s,n,d,r,p,y"),
          "the offer record is written inside one serial sequence");
    CHECK(serial_contains_atomic(
              "PRESS ENTER FOR THE LOCAL MENU (N: NETWORK SETUP - SET THE "
              "ADDRESS THERE)"),
          "the line that explains the offer is atomic");
    /*
     * The controls: the helper can fail - a plain write is found but is not
     * atomic - and it recognises a write that is.
     */
    reset_serial();
    serial_write("PLAIN WRITE");
    CHECK(serial_contains("PLAIN WRITE") &&
              !serial_contains_atomic("PLAIN WRITE"),
          "an unsequenced write is readable but not atomic");
    {
        const uint64_t flags = serial_sequence_begin();

        serial_write("SEQUENCED WRITE");
        serial_sequence_end(flags);
    }
    CHECK(serial_contains_atomic("SEQUENCED WRITE"),
          "a sequenced write is atomic");
    /*
     * Once per boot, and checked before any keystroke: once the operator has
     * pressed something the suppression gate would hide a second offer for a
     * different reason, and the check would pass even if the "once" rule were
     * gone.
     */
    reset_serial();
    console_offer_local_menu(1000U + LOCAL_MENU_OFFER_MS + 60000U);
    CHECK(!serial_contains("[LOCAL-MENU] offered"),
          "the menu is offered once per boot");
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    CHECK(serial_contains("[LOCAL-MENU] open\n"),
          "ENTER on the empty line opens the offered menu");
    CHECK(serial_contains_atomic("[LOCAL-MENU] open"),
          "the open marker is atomic");
    send_char('q');
    CHECK(serial_contains("[LOCAL-MENU] closed\n"),
          "the offered menu closes like the typed one");
    CHECK(serial_contains_atomic("[LOCAL-MENU] closed"),
          "the close marker is atomic");

    /* A keystroke cancels the offer, and is typed rather than swallowed. */
    begin_case();
    console_publish_input_source(LOCAL_INPUT_SOURCE_PS2);
    console_publish_local_input(true);
    console_init(512U, 1U);
    console_offer_local_menu(1000U);
    console_offer_local_menu(1000U + LOCAL_MENU_OFFER_MS);
    CHECK(serial_contains(
              "[LOCAL-MENU] offered reason=IDLE entries=6 "
              "keys=s,n,d,r,p,y\n"),
          "the offer is painted before the cancel case");
    send_char('h');
    CHECK(serial_contains("[LOCAL-MENU] offer cancelled\n"),
          "a keystroke cancels the painted offer");
    CHECK(serial_contains_atomic("[LOCAL-MENU] offer cancelled"),
          "and the cancellation marker is atomic too");
    CHECK(line_is("h"), "the key that cancelled the offer was still typed");
    send_event(KEYBOARD_EVENT_BACKSPACE, '\0');
    submit_command("help");
    CHECK(serial_contains("COMMANDS: HELP"),
          "the command typed after a cancelled offer still runs");

    /* An operator who has typed at all is never offered a menu. */
    begin_case();
    console_publish_input_source(LOCAL_INPUT_SOURCE_PS2);
    console_publish_local_input(true);
    console_init(512U, 1U);
    send_char('x');
    send_event(KEYBOARD_EVENT_BACKSPACE, '\0');
    console_offer_local_menu(1000U);
    console_offer_local_menu(1000U + LOCAL_MENU_OFFER_MS + 1000U);
    CHECK(!serial_contains("[LOCAL-MENU] offered"),
          "an operator who has typed is not offered a menu");

    /* A machine that cannot type is offered the menu at once, read-only. */
    begin_case();
    console_publish_input_source(LOCAL_INPUT_SOURCE_NONE);
    console_publish_local_input(false);
    console_init(512U, 1U);
    console_offer_local_menu(0U);
    /*
     * ITEM-449: the read-only offer's lines are atomic like the idle offer's.
     * The storage-boot proof's `keyboard_absent` phase waits for the second
     * line, and a timer record inside it made the marker unfindable.
     */
    CHECK(serial_contains_atomic(
              "[LOCAL-MENU] offered reason=NO_LOCAL_INPUT entries=6 "
              "keys=s,n,d,r,p,y"),
          "a machine with no local input is offered the menu at once");
    CHECK(serial_contains_atomic("keys=s,n,d,r,p,y"),
          "and the record publishes the keys the menu answers to");
    CHECK(serial_contains_atomic(
              "LOCAL INPUT UNAVAILABLE: THE MENU IS SHOWN FOR READING"),
          "the read-only offer says why it cannot be driven");
    CHECK(serial_contains("[1] Status"),
          "the read-only offer paints the menu entries");
}

/*
 * ITEM-509 increment 9: the console paints the menu in the published language.
 *
 * The menu model is proven in tests/local-menu-host.c in both languages; this
 * is the console's half - it hands the active language to the render, so a
 * console that published `ru` cannot keep painting the English menu.  ITEM-509
 * increment 21 finished the surface: the offer prompt and the read-only reason
 * now come from the catalog too, while the offer record itself
 * (`[LOCAL-MENU] offered ...`) stays English - it is a machine record, which the
 * item's boundary keeps as it is.
 */
static void test_local_menu_language(void) {
    begin_case();
    console_publish_input_source(LOCAL_INPUT_SOURCE_PS2);
    console_publish_local_input(true);
    console_publish_language(APPLIANCE_LANGUAGE_RU, false);
    console_init(512U, 1U);
    console_offer_local_menu(1000U);
    console_offer_local_menu(1000U + LOCAL_MENU_OFFER_MS);
    CHECK(serial_contains("ЛОКАЛЬНОЕ МЕНЮ"),
          "the painted menu speaks the published language");
    CHECK(serial_contains("[1] Статус") &&
              serial_contains("[4] Перезагрузка") &&
              serial_contains("[6] Восстановление"),
          "every painted entry is translated");
    CHECK(serial_contains_atomic("[1] Статус"),
          "a translated entry is painted inside one serial sequence");
    CHECK(serial_contains_atomic(
              "ВВЕРХ/ВНИЗ — выбор, ENTER — запуск, Q — выход"),
          "the translated footer is painted inside one serial sequence");
    CHECK(serial_contains_atomic(
              "ENTER — ЛОКАЛЬНОЕ МЕНЮ (N: НАСТРОЙКА СЕТИ — ЗАДАТЬ АДРЕС)"),
          "the offer prompt speaks the published language, atomically");
    CHECK(serial_contains("[LOCAL-MENU] offered reason=IDLE entries=6 "
                          "keys=s,n,d,r,p,y\n"),
          "the offer record stays English");
    CHECK(!serial_contains("[GLYPH-MISSING]"),
          "the Russian menu needs no missing-glyph record");

    /* The read-only offer's reason speaks the published language too. */
    begin_case();
    console_publish_language(APPLIANCE_LANGUAGE_RU, false);
    console_publish_input_source(LOCAL_INPUT_SOURCE_NONE);
    console_publish_local_input(false);
    console_init(512U, 1U);
    console_offer_local_menu(0U);
    CHECK(serial_contains_atomic(
              "ЛОКАЛЬНЫЙ ВВОД НЕДОСТУПЕН: МЕНЮ ПОКАЗАНО ДЛЯ ЧТЕНИЯ"),
          "the read-only reason speaks the published language, atomically");

    /* The default language is still English after a Russian pass. */
    begin_case();
    console_publish_language(APPLIANCE_LANGUAGE_EN, false);
    console_publish_input_source(LOCAL_INPUT_SOURCE_PS2);
    console_publish_local_input(true);
    console_init(512U, 1U);
    console_offer_local_menu(1000U);
    console_offer_local_menu(1000U + LOCAL_MENU_OFFER_MS);
    CHECK(serial_contains("LOCAL MENU") &&
              serial_contains("[1] Status"),
          "the English menu is byte-for-byte after a Russian pass");
}

/*
 * ITEM-506: the account vocabulary is accepted in the case `help` prints it
 * in, and nothing that is not the exact command is accepted as one.  The old
 * `admin-*` names no longer exist: the classifier has no branch that could
 * match them, which is what "removed, not hidden" means.
 */
static void test_command_case(void) {
    bool pending = false;

    CHECK(console_command_is("credentials", "credentials") &&
              console_command_is("CREDENTIALS", "credentials") &&
              console_command_is("Credentials", "credentials"),
          "the account command matches whatever case it is typed in");
    CHECK(!console_command_is("credential", "credentials") &&
              !console_command_is("credentials ", "credentials"),
          "a name that is not the command does not match it");
    CHECK(console_credentials_command_classify(
              "admin-provision s3cret", strlen("admin-provision s3cret"),
              &pending) == CONSOLE_CREDENTIALS_COMMAND_NONE &&
              console_credentials_command_classify(
                  "admin-login s3cret", strlen("admin-login s3cret"),
                  &pending) == CONSOLE_CREDENTIALS_COMMAND_NONE &&
              console_credentials_command_classify(
                  "admin", strlen("admin"), &pending) ==
                  CONSOLE_CREDENTIALS_COMMAND_NONE,
          "the removed admin-* names are not commands any more");
    CHECK(!console_command_is(NULL, "credentials") &&
              !console_command_is("credentials", NULL) &&
              !console_command_prefix(NULL, "credentials") &&
              !console_command_prefix("credentials", NULL),
          "a missing string never matches");
    /*
     * ITEM-506: the prefix helper still serves the live `PTR ` and `ARM `
     * inputs (and the redaction safety net), so its case folding and its
     * refusal to match without the separating space keep their own proof.
     */
    {
        char typed[] = "PTR 12";

        CHECK(console_command_prefix(typed, "ptr "),
              "a name that carries an argument matches as a prefix");
        CHECK(strcmp(typed, "PTR 12") == 0,
              "the prefix helper never rewrites the argument it examines");
    }
    CHECK(console_command_prefix("Arm 3", "arm ") &&
              !console_command_prefix("ptr12", "ptr ") &&
              !console_command_prefix("ptr", "ptr "),
          "the prefix match folds case and needs the separating space");
}

/*
 * ITEM-512: the command chain matches whatever case the operator types, not
 * only the commands that happen to call `console_command_is`.  The two-controller
 * proof found the gap: its wake sends modifier-only `sendkey shift` reports to
 * the QEMU keyboard, and when the guest's bounded queue drops the release, the
 * line arrives as `MENU`.  The proof's echo witness folds case, but the console
 * refused the upper-case line with UNKNOWN COMMAND, which is a definite
 * assertion rather than a retryable timeout.  Caps Lock produces the same line
 * for a person, so the proof and the operator want the same rule: the command
 * word folds, the argument does not (a language value is still exactly `en` or
 * `ru`), and the helper's documented premise becomes true for every command.
 */
static void test_commands_match_whatever_case(void) {
    begin_case();
    console_init(512U, 1U);
    reset_serial();
    submit_command("VERSION");
    /* ITEM-558: the answer now carries the build identity after the product
       line, so the assertion follows the line the operator reads. */
    CHECK(serial_contains("HFOC OS 1.0.0 X86-64 build=unknown version=unknown\n"),
          "VERSION answers like version");
    CHECK(!serial_contains("UNKNOWN COMMAND: VERSION"),
          "VERSION is not refused");

    begin_case();
    console_publish_input_source(LOCAL_INPUT_SOURCE_USB_HID);
    console_publish_local_input(true);
    console_init(512U, 1U);
    reset_serial();
    submit_command("KBD");
    CHECK(serial_contains("KEYBOARD IRQ: 0 LAST SCANCODE: 0\n"),
          "KBD answers like kbd");
    CHECK(!serial_contains("UNKNOWN COMMAND: KBD"),
          "KBD is not refused");

    begin_case();
    console_init(512U, 1U);
    reset_serial();
    submit_command("TiCkS");
    CHECK(serial_contains("TICKS: "),
          "mixed-case TICKS answers like ticks");
    CHECK(!serial_contains("UNKNOWN COMMAND: TiCkS"),
          "mixed-case TICKS is not refused");

    begin_case();
    console_init(512U, 1U);
    reset_serial();
    submit_command("MENU");
    CHECK(serial_contains("[LOCAL-MENU] open\n"),
          "MENU opens the local menu like menu");
    CHECK(!serial_contains("UNKNOWN COMMAND: MENU"),
          "MENU is not refused");
    send_char('q');
    CHECK(serial_contains("[LOCAL-MENU] closed\n"),
          "the upper-case menu closes like the typed one");

    /*
     * Leave a clean console for the tests that drive conversations: the menu
     * case above ends with the close key on the line, and the conversation
     * proofs type their own commands.
     */
    begin_case();
    console_init(512U, 1U);
}

/*
 * ITEM-506: the account command family owns the break-glass reset as the same
 * two-line gesture, and only the exact confirmation after the exact request
 * performs it.  `credentials` and `login` start conversations and write
 * nothing by themselves.
 */
static void test_credentials_command_classify(void) {
    bool pending = false;

    CHECK(console_credentials_command_classify(
              "credentials", strlen("credentials"), &pending) ==
              CONSOLE_CREDENTIALS_COMMAND_CHANGE &&
              !pending,
          "credentials is the change command");
    CHECK(console_credentials_command_classify(
              "LOGIN", strlen("LOGIN"), &pending) ==
              CONSOLE_CREDENTIALS_COMMAND_LOGIN &&
              !pending,
          "login is the sign-in command");
    CHECK(console_credentials_command_classify(
              "credentials reset", strlen("credentials reset"), &pending) ==
              CONSOLE_CREDENTIALS_COMMAND_RESET_ARM &&
              pending,
          "the reset request line arms the gesture");
    CHECK(console_credentials_command_classify(
              "CREDENTIALS RESET CONFIRM",
              strlen("CREDENTIALS RESET CONFIRM"), &pending) ==
              CONSOLE_CREDENTIALS_COMMAND_RESET_EXECUTE &&
              !pending,
          "the confirmation line executes and clears the arm");
    CHECK(console_credentials_command_classify(
              "credentials reset confirm", strlen("credentials reset confirm"),
              &pending) == CONSOLE_CREDENTIALS_COMMAND_RESET_UNCONFIRMED &&
              !pending,
          "a confirmation with no request is refused, not honoured");
    CHECK(console_credentials_command_classify(
              "credentials reset", strlen("credentials reset"), &pending) ==
              CONSOLE_CREDENTIALS_COMMAND_RESET_ARM &&
              pending,
          "a fresh request arms again");
    CHECK(console_credentials_command_classify("help", strlen("help"),
                                               &pending) ==
              CONSOLE_CREDENTIALS_COMMAND_NONE &&
              !pending,
          "any other command disarms a pending request");
    CHECK(console_credentials_command_classify(
              "credentials reset extra", strlen("credentials reset extra"),
              &pending) == CONSOLE_CREDENTIALS_COMMAND_NONE,
          "a line that is not the exact request is not the gesture");
    CHECK(console_credentials_command_classify(
              "credentials", strlen("credentials"), NULL) ==
              CONSOLE_CREDENTIALS_COMMAND_NONE,
          "a missing pending flag can never arm the gesture");
}

/*
 * ITEM-506: the console account conversation driven end to end.  The fixture
 * binding records what the console asked the owner to do; the production owner
 * is host-proofed in its own test, so this proof is about the console half:
 * the questions, the hidden line, the order (verify before provision), the
 * one-command vocabulary and the reset gesture.
 */
struct credentials_fixture {
    unsigned int verify_calls;
    unsigned int provision_calls;
    unsigned int rekey_calls;
    unsigned int authenticate_calls;
    unsigned int reset_calls;
    unsigned int status_calls;
    /*
     * ITEM-506: the fixture numbers its calls, so a proof can assert the order
     * (the current password is verified before anything is written) instead of
     * comparing two independent counts.
     */
    unsigned int sequence;
    unsigned int verify_sequence;
    unsigned int rekey_sequence;
    unsigned int provision_sequence;
    bool verify_ok;
    bool provision_ok;
    bool authenticate_ok;
    bool rekey_fail;
    /*
     * ITEM-506: an appliance with no verifiable account.  The verify step
     * answers UNPROVISIONED and the conversation is allowed to set the first
     * account without a current password.
     */
    bool first_set;
    /*
     * ITEM-506: a real legacy fingerprint at the head.  It is a credential the
     * appliance still serves, so the change must refuse it and name the
     * explicit reset instead of overwriting it.
     */
    bool legacy_record;
    char login[APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX + 1U];
    char rekey_current[APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX + 1U];
    char rekey_new[APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX + 1U];
    char password[APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX + 1U];
};

static void credentials_fixture_copy(
    char *destination,
    size_t capacity,
    const char *value,
    size_t length) {
    size_t index;

    if (capacity == 0U) {
        return;
    }
    for (index = 0U; index + 1U < capacity && index < length; ++index) {
        destination[index] = value[index];
    }
    destination[index] = '\0';
}

static enum appliance_production_admin_enrollment_status
credentials_fixture_verify(
    void *context,
    const char *password,
    size_t password_length,
    uint64_t now_tick) {
    struct credentials_fixture *fixture = context;

    (void)now_tick;
    ++fixture->verify_calls;
    fixture->verify_sequence = ++fixture->sequence;
    credentials_fixture_copy(fixture->password, sizeof(fixture->password),
                             password, password_length);
    if (fixture->legacy_record) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_LEGACY_RECORD;
    }
    if (fixture->first_set) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_UNPROVISIONED;
    }
    return fixture->verify_ok
        ? APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK
        : APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED;
}

static enum appliance_production_admin_enrollment_status
credentials_fixture_provision(
    void *context,
    const char *login,
    size_t login_length,
    const char *password,
    size_t password_length,
    uint64_t now_tick) {
    struct credentials_fixture *fixture = context;

    (void)now_tick;
    ++fixture->provision_calls;
    fixture->provision_sequence = ++fixture->sequence;
    credentials_fixture_copy(fixture->login, sizeof(fixture->login), login,
                             login_length);
    credentials_fixture_copy(fixture->password, sizeof(fixture->password),
                             password, password_length);
    return fixture->provision_ok
        ? APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK
        : APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_WEAK_PASSWORD;
}

static enum appliance_production_admin_enrollment_status
credentials_fixture_authenticate(
    void *context,
    const char *login,
    size_t login_length,
    const char *password,
    size_t password_length,
    uint64_t now_tick,
    uint32_t lifetime_ticks,
    uint8_t token_out[APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH],
    uint32_t *session_id_out) {
    struct credentials_fixture *fixture = context;
    size_t index;

    (void)now_tick;
    (void)lifetime_ticks;
    ++fixture->authenticate_calls;
    credentials_fixture_copy(fixture->login, sizeof(fixture->login), login,
                             login_length);
    credentials_fixture_copy(fixture->password, sizeof(fixture->password),
                             password, password_length);
    if (!fixture->authenticate_ok) {
        return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED;
    }
    for (index = 0U; index < APPLIANCE_ADMIN_AUTH_TOKEN_LENGTH; ++index) {
        token_out[index] = (uint8_t)('a' + (index % 26U));
    }
    *session_id_out = 7U;
    return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK;
}

static bool credentials_fixture_rekey(
    void *context,
    const char *current_password,
    size_t current_password_length,
    const char *new_password,
    size_t new_password_length) {
    struct credentials_fixture *fixture = context;

    ++fixture->rekey_calls;
    fixture->rekey_sequence = ++fixture->sequence;
    credentials_fixture_copy(
        fixture->rekey_current, sizeof(fixture->rekey_current),
        current_password, current_password_length);
    credentials_fixture_copy(
        fixture->rekey_new, sizeof(fixture->rekey_new),
        new_password, new_password_length);
    return !fixture->rekey_fail;
}

static enum appliance_production_admin_enrollment_status
credentials_fixture_reset(void *context) {
    struct credentials_fixture *fixture = context;

    ++fixture->reset_calls;
    return APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK;
}

static void credentials_fixture_status(void *context) {
    struct credentials_fixture *fixture = context;

    ++fixture->status_calls;
}

static const char *credentials_fixture_status_text(
    enum appliance_production_admin_enrollment_status status) {
    return status == APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK ? "OK"
                                                              : "DENIED";
}

static struct console_admin_credentials_binding credentials_fixture_binding;

static void bind_credentials_fixture(struct credentials_fixture *fixture) {
    memset(&credentials_fixture_binding, 0,
           sizeof(credentials_fixture_binding));
    credentials_fixture_binding.context = fixture;
    credentials_fixture_binding.verify = credentials_fixture_verify;
    credentials_fixture_binding.provision = credentials_fixture_provision;
    credentials_fixture_binding.rekey_credential =
        credentials_fixture_rekey;
    credentials_fixture_binding.authenticate = credentials_fixture_authenticate;
    credentials_fixture_binding.reset = credentials_fixture_reset;
    credentials_fixture_binding.status = credentials_fixture_status;
    credentials_fixture_binding.status_text = credentials_fixture_status_text;
    console_bind_admin_credentials(&credentials_fixture_binding);
}

static void test_credentials_conversation(void) {
    static const char secret[] = "correct horse battery staple";
    static const char new_login[] = "operator";
    struct credentials_fixture fixture;

    memset(&fixture, 0, sizeof(fixture));
    fixture.verify_ok = true;
    fixture.provision_ok = true;
    fixture.authenticate_ok = true;
    bind_credentials_fixture(&fixture);

    /* The change conversation asks in order and verifies before it writes. */
    reset_serial();
    submit_command("credentials");
    CHECK(console_admin_credentials_input_active(),
          "credentials opens the conversation");
    CHECK(serial_contains("Current password:"),
          "the first question is the current password");
    CHECK(fixture.status_calls == 1U,
          "the state record is published when the conversation opens");
    submit_command(secret);
    CHECK(!serial_contains(secret) && !screen_contains(secret),
          "the typed current password is never echoed");
    CHECK(serial_contains("New login:"), "the new login is asked");
    submit_command(new_login);
    CHECK(serial_contains("New password:"), "the new password is asked");
    submit_command(secret);
    CHECK(serial_contains("Repeat new password:"),
          "the confirmation is asked");
    submit_command(secret);
    CHECK(!console_admin_credentials_input_active(),
          "the conversation closes after the pair matches");
    CHECK(fixture.verify_calls == 1U && fixture.rekey_calls == 1U &&
              fixture.provision_calls == 1U &&
              fixture.verify_sequence != 0U &&
              fixture.verify_sequence < fixture.rekey_sequence &&
              fixture.rekey_sequence < fixture.provision_sequence,
          "the password was proven, the seed re-sealed, then the account set");
    CHECK(strcmp(fixture.rekey_current, secret) == 0 &&
              strcmp(fixture.rekey_new, secret) == 0,
          "the seed rekey receives the proven and replacement passwords");
    CHECK(serial_contains("credentials changed login=operator"),
          "the record names the login that changed");
    CHECK(!serial_contains(secret) && !screen_contains(secret),
          "the password never reaches the transcript");

    /* A wrong current password writes nothing. */
    memset(&fixture, 0, sizeof(fixture));
    fixture.verify_ok = false;
    bind_credentials_fixture(&fixture);
    submit_command("credentials");
    submit_command(secret);
    submit_command(new_login);
    submit_command(secret);
    submit_command(secret);
    CHECK(fixture.verify_calls == 1U && fixture.provision_calls == 0U,
          "a wrong current password stops before the write");
    CHECK(serial_contains("current password refused: DENIED"),
          "the refusal is named");
    CHECK(!console_admin_credentials_input_active(),
          "a refused change closes the conversation");

    /* A failed seed re-seal refuses the account replacement before it writes. */
    memset(&fixture, 0, sizeof(fixture));
    fixture.verify_ok = true;
    fixture.provision_ok = true;
    fixture.rekey_fail = true;
    bind_credentials_fixture(&fixture);
    reset_serial();
    submit_command("credentials");
    submit_command(secret);
    submit_command(new_login);
    submit_command(secret);
    submit_command(secret);
    CHECK(fixture.verify_calls == 1U && fixture.rekey_calls == 1U &&
              fixture.provision_calls == 0U,
          "a failed seed rekey stops before the account write");
    CHECK(serial_contains("HOST_SEED_REKEY_FAILED"),
          "the seed rekey refusal is named");

    /* An appliance with no verifiable account has no current password to
       prove; the explicit first set still goes through, and says so. */
    memset(&fixture, 0, sizeof(fixture));
    fixture.first_set = true;
    fixture.provision_ok = true;
    bind_credentials_fixture(&fixture);
    reset_serial();
    submit_command("credentials");
    submit_command(secret);
    submit_command(new_login);
    submit_command(secret);
    submit_command(secret);
    CHECK(fixture.verify_calls == 1U && fixture.rekey_calls == 0U &&
              fixture.provision_calls == 1U,
          "an appliance with no verifiable account sets the first one");
    CHECK(serial_contains("no verifiable account"),
          "the first-set step says why the current password is not checked");
    CHECK(serial_contains("credentials changed login=operator"),
          "the first account is recorded like any other change");

    /* A real legacy record is a credential the appliance still serves, so an
       unproven change refuses it and names the explicit reset instead of
       silently overwriting the secret the operator did set. */
    memset(&fixture, 0, sizeof(fixture));
    fixture.legacy_record = true;
    bind_credentials_fixture(&fixture);
    reset_serial();
    submit_command("credentials");
    submit_command(secret);
    submit_command(new_login);
    submit_command(secret);
    submit_command(secret);
    CHECK(fixture.verify_calls == 1U && fixture.provision_calls == 0U,
          "a legacy record is not migrated by an unproven change");
    CHECK(serial_contains("credentials reset confirm"),
          "the legacy refusal names the explicit reset step");
    CHECK(!console_admin_credentials_input_active(),
          "the refused legacy change closes the conversation");

    /* An empty line is the way out at any step. */
    memset(&fixture, 0, sizeof(fixture));
    fixture.verify_ok = true;
    fixture.provision_ok = true;
    bind_credentials_fixture(&fixture);
    submit_command("credentials");
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    CHECK(!console_admin_credentials_input_active(),
          "an empty line cancels the conversation");
    CHECK(serial_contains("Credential entry cancelled."),
          "the cancellation is said out loud");
    CHECK(fixture.provision_calls == 0U, "a cancelled change writes nothing");

    /* A line that did not fit is refused, not cut to a valid password. */
    memset(&fixture, 0, sizeof(fixture));
    fixture.verify_ok = true;
    bind_credentials_fixture(&fixture);
    submit_command("credentials");
    reset_serial();
    for (size_t index = 0U;
         index < APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX + 1U; ++index) {
        send_char('a');
    }
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    CHECK(serial_contains("longer than the policy allows"),
          "an over-long line is refused as too long");
    CHECK(console_admin_credentials_input_active(),
          "the over-long refusal keeps the conversation on the question");
    /*
     * The refusal zeroes the line; the overflow flag describes the line being
     * typed, so one backspace that brings a fresh over-long line back under
     * the cap must clear it - otherwise a legal password would stay refused
     * for the rest of the conversation.
     */
    for (size_t index = 0U;
         index < APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX + 1U; ++index) {
        send_char('a');
    }
    send_event(KEYBOARD_EVENT_BACKSPACE, '\0');
    reset_serial();
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    CHECK(serial_contains("New login:"),
          "a line shortened back under the cap submits, not stays refused");
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    CHECK(!console_admin_credentials_input_active(),
          "the conversation can still be cancelled");

    /* The login conversation opens the one local session. */
    memset(&fixture, 0, sizeof(fixture));
    fixture.authenticate_ok = true;
    bind_credentials_fixture(&fixture);
    reset_serial();
    submit_command("login");
    CHECK(serial_contains("Login:"), "login asks the login");
    submit_command(new_login);
    CHECK(serial_contains("Password:"), "login asks the password");
    submit_command(secret);
    CHECK(fixture.authenticate_calls == 1U,
          "the login conversation calls the one authenticate entry");
    CHECK(serial_contains("ADMIN SESSION: Authorization: Bearer "),
          "a successful login prints the session token");
    CHECK(!serial_contains(secret) && !screen_contains(secret),
          "the password never reaches the transcript");

    /* The break-glass reset stays, under the one command family. */
    memset(&fixture, 0, sizeof(fixture));
    bind_credentials_fixture(&fixture);
    reset_serial();
    submit_command("credentials reset");
    CHECK(serial_contains("requires confirmation") &&
              fixture.reset_calls == 0U,
          "the first reset line only arms the gesture");
    submit_command("credentials reset confirm");
    CHECK(fixture.reset_calls == 1U, "the confirmation performs the reset");
    CHECK(serial_contains("reset; state=UNPROVISIONED"),
          "the reset names the state it produced");

    /* Without a binding the console refuses instead of inventing an account. */
    console_bind_admin_credentials(NULL);
    reset_serial();
    submit_command("credentials");
    CHECK(serial_contains("[ADMIN-ENROLLMENT] UNAVAILABLE"),
          "an unbound account reports UNAVAILABLE");
    CHECK(!console_admin_credentials_input_active(),
          "an unbound account opens no conversation");
}

/*
 * ITEM-610: the credential line is readable.  The login is painted exactly as
 * it is typed; every password step paints one `*` per accepted character and
 * no real password byte; backspace removes one mask cell and never the
 * question; ENTER on a password announces the synchronous check before it
 * starts.  The echo is framebuffer-only, so the serial transcript and the
 * journal still hold no credential answer.
 */
static void test_credentials_readable_feedback(void) {
    static const char secret[] = "correct horse battery staple";
    static const char login[] = "operator";
    struct credentials_fixture fixture;
    size_t index;

    begin_case();
    memset(&fixture, 0, sizeof(fixture));
    fixture.verify_ok = true;
    fixture.provision_ok = true;
    bind_credentials_fixture(&fixture);

    submit_command("credentials");
    reset_screen();
    for (index = 0U; index < strlen(secret); ++index) {
        send_char(secret[index]);
        CHECK(screen_occurrences("*") == index + 1U,
              "each accepted current-password character paints one star");
    }
    CHECK(!screen_contains(secret) && !serial_contains(secret),
          "the current password is neither painted nor serialised");
    reset_screen();
    send_event(KEYBOARD_EVENT_BACKSPACE, '\0');
    CHECK(screen_occurrences("*") == 0U,
          "backspace paints no new star");
    send_char(secret[strlen(secret) - 1U]);
    CHECK(screen_occurrences("*") == 1U,
          "the restored character paints exactly one star");
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    CHECK(serial_contains("New login: "), "the current password advances");

    reset_screen();
    for (index = 0U; index < strlen(login); ++index) {
        send_char(login[index]);
    }
    CHECK(screen_contains(login),
          "the login is visible as it is typed");
    CHECK(!serial_contains(login),
          "the login echo stays off the serial transcript");
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    CHECK(serial_contains("New password: "), "the login advances");

    reset_screen();
    for (index = 0U; index < strlen(secret); ++index) {
        send_char(secret[index]);
        CHECK(screen_occurrences("*") == index + 1U,
              "each accepted new-password character paints one star");
    }
    CHECK(!screen_contains(secret) && !serial_contains(secret),
          "the new password is neither painted nor serialised");
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    CHECK(serial_contains("Repeat new password: "),
          "the new password advances to its confirmation");

    reset_screen();
    for (index = 0U; index < strlen(secret); ++index) {
        send_char(secret[index]);
    }
    CHECK(screen_occurrences("*") == strlen(secret),
          "the confirmation paints one star per character");
    CHECK(!serial_contains("Checking..."),
          "the check line waits for ENTER");
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    CHECK(serial_contains("Checking..."),
          "ENTER on the final password announces the check");
    CHECK(fixture.verify_calls == 1U && fixture.provision_calls == 1U,
          "the announced check is the real one that runs");
    reset_screen();
    CHECK(screen_occurrences("*") == 0U,
          "the next prompt carries no stale mask");
    CHECK(!serial_contains(secret),
          "no password byte reaches the serial transcript");
}

/*
 * The echo is bounded by the same accepted-character cap as the line: the
 * 129th character is refused and paints no star.  A backspace removes one
 * accepted cell and clears the refusal flag.
 */
static void test_credentials_mask_overflow(void) {
    struct credentials_fixture fixture;
    size_t index;

    begin_case();
    memset(&fixture, 0, sizeof(fixture));
    fixture.verify_ok = true;
    bind_credentials_fixture(&fixture);
    submit_command("credentials");
    reset_screen();
    for (index = 0U; index < APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX + 1U; ++index) {
        send_char('a');
    }
    CHECK(screen_occurrences("*") == APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX,
          "the refused character paints no star");
    reset_screen();
    send_event(KEYBOARD_EVENT_BACKSPACE, '\0');
    CHECK(screen_occurrences("*") == 0U,
          "backspace paints no new star after an overflow");
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    CHECK(serial_contains("New login: "),
          "the shortened line submits instead of staying refused");
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    CHECK(!console_admin_credentials_input_active(),
          "the overflow fixture closes its conversation");
}

struct entropy_login_fixture {
    unsigned int calls;
    struct console_entropy_login_result result;
    char login[APPLIANCE_ADMIN_CREDENTIAL_LOGIN_MAX + 1U];
    char password[APPLIANCE_ADMIN_CREDENTIAL_PASSWORD_MAX + 1U];
};

static struct console_entropy_login_result entropy_login_fixture_unlock(
    void *context,
    const char *login,
    size_t login_length,
    const char *password,
    size_t password_length,
    uint64_t now_tick) {
    struct entropy_login_fixture *fixture = context;

    (void)now_tick;
    ++fixture->calls;
    credentials_fixture_copy(
        fixture->login, sizeof(fixture->login), login, login_length);
    credentials_fixture_copy(
        fixture->password, sizeof(fixture->password), password,
        password_length);
    return fixture->result;
}

static void test_entropy_login_conversation(void) {
    static const char login[] = "operator";
    static const char password[] = "correct horse battery staple";
    struct credentials_fixture credentials;
    struct entropy_login_fixture entropy;
    struct console_entropy_login_binding binding;

    memset(&credentials, 0, sizeof(credentials));
    bind_credentials_fixture(&credentials);
    memset(&entropy, 0, sizeof(entropy));
    entropy.result.stage = CONSOLE_ENTROPY_LOGIN_STAGE_ACCOUNT;
    entropy.result.account_status =
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED;
    memset(&binding, 0, sizeof(binding));
    binding.context = &entropy;
    binding.unlock = entropy_login_fixture_unlock;
    console_bind_entropy_login(&binding);

    begin_case();
    console_publish_input_source(LOCAL_INPUT_SOURCE_PS2);
    console_publish_local_input(true);
    console_init(512U, 1U);
    console_start_entropy_unlock();
    CHECK(console_admin_credentials_input_active(),
          "the host-seed login opens the normal credential conversation");
    reset_serial();
    console_offer_local_menu(1000U);
    console_offer_local_menu(1000U + LOCAL_MENU_OFFER_MS);
    CHECK(!serial_contains("[LOCAL-MENU] offered") &&
              !screen_contains("PRESS ENTER FOR THE LOCAL MENU"),
          "the local-menu offer cannot paint over a hidden login conversation");
    submit_command(login);
    CHECK(serial_contains("Password:"),
          "the host-seed login asks for the password separately");
    submit_command(password);
    CHECK(entropy.calls == 1U && strcmp(entropy.login, login) == 0 &&
              strcmp(entropy.password, password) == 0,
          "the host-seed callback receives the entered account pair");
    CHECK(serial_contains(
              "[RNG] host unlock stage=ACCOUNT status=DENIED"),
          "an account refusal names the account stage, not the seed");
    CHECK(!serial_contains(password) && !screen_contains(password),
          "the host-seed password remains hidden");

    entropy.result.stage = CONSOLE_ENTROPY_LOGIN_STAGE_SEED;
    entropy.result.account_status =
        APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_OK;
    entropy.result.seed_status = APPLIANCE_HOST_SEED_WRONG_PASSWORD;
    reset_serial();
    console_start_entropy_unlock();
    submit_command(login);
    submit_command(password);
    CHECK(serial_contains(
              "[RNG] host unlock stage=SEED status=WRONG_PASSWORD"),
          "a seed-key refusal is reported at the seed stage");
    CHECK(!serial_contains(
              "[RNG] host unlock stage=ACCOUNT status=WRONG_PASSWORD"),
          "a seed-key refusal is not mislabeled as an account refusal");

    entropy.result.stage = CONSOLE_ENTROPY_LOGIN_STAGE_COMPLETE;
    entropy.result.seed_status = APPLIANCE_HOST_SEED_OK;
    reset_serial();
    console_start_entropy_unlock();
    submit_command(login);
    submit_command(password);
    CHECK(serial_contains("[RNG] host seed status=OK stage=COMPLETE"),
          "a completed unlock publishes the existing host-seed success record");
    console_bind_entropy_login(NULL);
    console_bind_admin_credentials(NULL);
}

static void test_local_input_banner(void) {

    /* A PS/2 keyboard keeps the ITEM-392 banner. */
    begin_case();
    console_publish_input_source(LOCAL_INPUT_SOURCE_PS2);
    console_publish_local_input(true);
    console_init(512U, 1U);
    CHECK(serial_contains("GDT IDT LEGACY PIC PIT PS2: OK\n"),
          "PS/2 banner reports a healthy controller");
    CHECK(serial_contains(
              "TYPE HELP FOR COMMANDS, MENU FOR THE LOCAL MENU\n"),
          "PS/2 banner invites a command and names the menu");
    CHECK(!serial_contains("LOCAL INPUT: DISABLED"),
          "PS/2 banner does not report disabled local input");
    CHECK(!serial_contains("USB HID KEYBOARD"),
          "PS/2 banner does not advertise a USB keyboard");

    /*
     * ITEM-395: a console driven by the USB HID keyboard says so and does not
     * claim a PS/2 controller the machine does not have.
     */
    begin_case();
    console_publish_input_source(LOCAL_INPUT_SOURCE_USB_HID);
    console_publish_local_input(true);
    console_init(512U, 1U);
    CHECK(serial_contains("GDT IDT LEGACY PIC PIT PS2: UNAVAILABLE\n"),
          "USB HID banner does not claim a PS/2 controller");
    CHECK(serial_contains("LOCAL INPUT: ENABLED (USB HID KEYBOARD)\n"),
          "USB HID banner names the live keyboard");
    CHECK(serial_contains(
              "TYPE HELP FOR COMMANDS, MENU FOR THE LOCAL MENU\n"),
          "USB HID banner invites a command and names the menu");
    CHECK(!serial_contains("PS2: OK"),
          "USB HID banner never reports PS2: OK");

    /* Neither path keeps the ITEM-392 degraded banner. */
    begin_case();
    console_publish_input_source(LOCAL_INPUT_SOURCE_NONE);
    console_publish_local_input(false);
    console_init(512U, 1U);
    CHECK(serial_contains("LOCAL INPUT: DISABLED (NO PS/2 KEYBOARD)\n"),
          "degraded banner reports disabled local input");
    CHECK(serial_contains("USE SERIAL OR THE MANAGEMENT PLANE\n"),
          "degraded banner names the paths that still reach the appliance");
    /* ITEM-430: a machine that cannot type is promised the menu instead. */
    CHECK(serial_contains("THE LOCAL MENU IS OFFERED BELOW FOR READING\n"),
          "degraded banner promises the menu a machine can only read");
    CHECK(!serial_contains("USB HID KEYBOARD"),
          "degraded banner does not advertise a keyboard");
}

static void test_kbd_command_reports_input_path(void) {
    begin_case();
    console_publish_input_source(LOCAL_INPUT_SOURCE_USB_HID);
    console_publish_local_input(true);
    console_init(512U, 1U);
    reset_serial();
    submit_command("kbd");

    CHECK(serial_contains("KEYBOARD IRQ: 0 LAST SCANCODE: 0\n"),
          "kbd command keeps the PS/2 counters");
    CHECK(serial_contains(
              "LOCAL INPUT: SOURCE=USB_HID USB=READY SPEED=HIGH VID=0x627"
              " PID=0x1 PS2=CONTROLLER_ABSENT\n"),
          "kbd command prints the live local input path");
    CHECK(serial_contains(
              "USB HID: REPORTS=7 TRANSFERS=8 ERRORS=0 REFUSED=2 LAST=4"
              " KEYS=5 DROPPED=0 GAP_MS_MAX=30\n"),
          "kbd command prints the live USB HID counters, including the "
          "completions that did not carry a whole boot report and the keys "
          "the whole ones carried, ITEM-458's dropped reports and worst "
          "service gap");
    CHECK(serial_contains("[PRODUCTION-LOOP] iter_us=7 iter_max_us=9\n"),
          "kbd command repeats the loop's own cost for the proof");
    CHECK(serial_contains("[CONSOLE] paint lines="),
          "kbd command repeats the console's paint cost for the proof");
}


/*
 * ITEM-405: the diagnostic records an operator needs on a machine with no
 * serial capture - the install records, the network records and the local
 * input records - are mirrored to the framebuffer console.  These cases hold
 * the four properties the mirror promises: it is additive (the serial stream
 * does not change), it is bounded and single-line, it paints once per record,
 * and a record published before the banner survives it.
 */
static void test_console_record_mirror_is_paint_only(void) {
    begin_case();
    console_init(1024U, 1U);
    reset_serial();
    reset_screen();

    static const char refusal[] =
        "[PRODUCTION-INSTALL] boot-entry state=UNAVAILABLE "
        "reason=PRODUCTION_INSTALLER_ESP_UNSUITABLE\n";

    console_publish_record(refusal, sizeof(refusal) - 1U);

    CHECK(screen_contains(
              "[PRODUCTION-INSTALL] boot-entry state=UNAVAILABLE "
              "reason=PRODUCTION_INSTALLER_ESP_UNSUITABLE"),
          "a mirrored record is painted on the console");
    CHECK(!serial_contains("[PRODUCTION-INSTALL]"),
          "the mirror never writes to the serial stream");
}

static void test_console_record_mirror_is_bounded_and_single_line(void) {
    char oversized[CONSOLE_RECORD_MAX + 64U];
    size_t index;
    char block[] = "[XHCI] state=UNAVAILABLE reason=NO_CONNECTED_DEVICE\n"
                   "second line that must not be painted\n";

    begin_case();
    console_init(1024U, 1U);
    reset_screen();

    for (index = 0U; index < sizeof(oversized) - 1U; ++index) {
        oversized[index] = 'A';
    }
    oversized[sizeof(oversized) - 1U] = '\0';
    console_publish_record(oversized, sizeof(oversized) - 1U);
    CHECK(screen_contains("AAA..."),
          "a record longer than the bound is cut and marked");
    CHECK(screen_occurrences("A") == CONSOLE_RECORD_MAX,
          "a mirrored record paints at most the bound");

    reset_screen();
    console_publish_record(block, sizeof(block) - 1U);
    CHECK(screen_contains("[XHCI] state=UNAVAILABLE reason=NO_CONNECTED_DEVICE"),
          "a block paints its first diagnostic line");
    CHECK(!screen_contains("second line"),
          "the rest of a block stays off the console");
}

/*
 * ITEM-561: a journal record longer than one part, carried in a single fragment
 * that ends with its own newline, is split - and returns.
 *
 * The start evidence is one line of about 430 characters: longer than
 * CONSOLE_RECORD_MAX, so the mirror has to break it into continuation parts.
 * The record was marked complete before the fragment was carried, which made
 * every `console_mirror_put` a no-op: the running worst case could never shrink,
 * the break at the offset was recorded once and the second `continue` found the
 * same offset, so the loop that owns them never advanced.  The exact record that
 * was supposed to explain the refusal hung the machine instead.
 */
static void test_a_long_record_ending_in_a_newline_is_split(void) {
    char record[CONSOLE_RECORD_MAX * 2U];
    size_t length = 0U;
    size_t spills_before;

    begin_case();
    console_init(512U, 1U);
    reset_screen();
    reset_serial();

    for (const char *name = "[EHCI-START] "; *name != '\0'; ++name) {
        record[length++] = *name;
    }
    while (length + 2U < sizeof(record)) {
        record[length++] = 'S';
    }
    record[length++] = '\n';
    record[length] = '\0';

    spills_before = console_record_spill_count();
    console_journal_begin();
    console_record_text(record);
    console_journal_end();

    /* Reaching this line at all is the property under test. */
    CHECK(console_record_spill_count() > spills_before,
          "a record longer than one part is split into continuations");
    CHECK(console_record_worst_length() <= CONSOLE_RECORD_MAX,
          "no part of that record exceeds the bound it is published under");
    CHECK(screen_occurrences("S") == 0U,
          "a journal-only record paints nothing on the console");
}

static void test_console_record_mirror_paints_once(void) {
    begin_case();
    console_init(1024U, 1U);
    reset_screen();

    static const char record[] =
        "[PRODUCTION-NET] device=READY readiness=READY backend=rtl8168\n";
    static const char changed[] =
        "[PRODUCTION-NET] device=READY readiness=DEGRADED "
        "backend=rtl8168\n";

    console_publish_record(record, sizeof(record) - 1U);
    console_publish_record(record, sizeof(record) - 1U);
    console_publish_record(record, sizeof(record) - 1U);

    CHECK(screen_occurrences("[PRODUCTION-NET] device=READY") == 1U,
          "a republished record paints once, so the screen does not scroll");
    console_publish_record(changed, sizeof(changed) - 1U);
    CHECK(screen_occurrences("[PRODUCTION-NET] device=READY") == 2U,
          "a changed record is painted as a new line");
}

static void test_console_record_mirror_survives_the_banner(void) {
    begin_case();
    reset_screen();

    /* Before console_init(): the banner clears the framebuffer afterwards. */
    static const char keyboard[] =
        "[KBD ] state=UNAVAILABLE reason=NO_KEYBOARD source=NONE "
        "ps2=CONTROLLER_ABSENT usb=NO_CONNECTED_DEVICE\n";

    console_publish_record(keyboard, sizeof(keyboard) - 1U);
    CHECK(!screen_contains("[KBD ] state="),
          "nothing is painted before the banner that would erase it");

    console_init(1024U, 1U);
    CHECK(screen_contains("HFOC OS 1.0.0 INITIALISED"),
          "the banner is painted");
    CHECK(screen_contains(
              "[KBD ] state=UNAVAILABLE reason=NO_KEYBOARD source=NONE"),
          "an early record is painted after the banner, once per boot");
}

static void test_console_record_builder_matches_serial(void) {
    begin_case();
    console_init(1024U, 1U);
    reset_screen();
    reset_serial();

    console_record_begin();
    serial_write("[XHCI] state=");
    console_record_text("[XHCI] state=");
    serial_write("UNAVAILABLE");
    console_record_text("UNAVAILABLE");
    serial_write(" ports=");
    console_record_text(" ports=");
    serial_write_u64(8U);
    console_record_u64(8U);
    serial_write(" bar=");
    console_record_text(" bar=");
    serial_write_hex(0xFE000000U);
    console_record_hex64(0xFE000000U);
    serial_write("\n");
    console_record_end();

    CHECK(serial_contains(
              "[XHCI] state=UNAVAILABLE ports=8 "
              "bar=0x00000000FE000000\n"),
          "the serial record is written exactly as before");
    CHECK(screen_contains(
              "[XHCI] state=UNAVAILABLE ports=8 "
              "bar=0x00000000FE000000"),
          "the incremental mirror paints the same record");
}

/*
 * ITEM-410: the console is the funnel the boot log observes.
 *
 * Every record the console accepts is handed to the bound log - the bounded,
 * printable line the operator read, not the block it came from - and the local
 * `BOOTLOG` command asks the log for its own staged records.  The console
 * proves two boundaries here: a refused record is never observed (the log must
 * not keep what the console would not paint), and the command works from RAM,
 * so it answers on a machine whose log could not be written at all.
 */
static char observed[16U][CONSOLE_RECORD_MAX + 1U];
static size_t observed_count;
static size_t dump_calls;

static void console_host_observe(const char *record, size_t length) {
    if (observed_count >= sizeof(observed) / sizeof(observed[0])) {
        return;
    }
    if (length > CONSOLE_RECORD_MAX) {
        length = CONSOLE_RECORD_MAX;
    }
    memcpy(observed[observed_count], record, length);
    observed[observed_count][length] = '\0';
    ++observed_count;
}

static void console_host_dump(void) {
    ++dump_calls;
    console_write("BOOTLOG: state=READY path=\\EFI\\HFOC\\BOOTLOG.BIN\n");
}

static bool observed_contains(const char *needle) {
    size_t index;

    for (index = 0U; index < observed_count; ++index) {
        if (strcmp(observed[index], needle) == 0) {
            return true;
        }
    }
    return false;
}

static void test_boot_log_binding(void) {
    const struct console_boot_log_binding binding = {
        console_host_observe,
        console_host_dump
    };
    static const char refusal[] =
        "[PRODUCTION-INSTALL] boot-entry state=UNAVAILABLE "
        "reason=PRODUCTION_INSTALLER_ESP_UNSUITABLE";
    static const char control[] = "[KBD ] state=BROKEN\x01";

    observed_count = 0U;
    dump_calls = 0U;
    console_bind_boot_log(&binding);
    console_publish_record(refusal, sizeof(refusal) - 1U);
    CHECK(observed_contains(refusal),
          "the boot log observes the record the console accepted");
    console_publish_record(control, sizeof(control) - 1U);
    CHECK(observed_count == 1U,
          "a record the console refuses is never handed to the boot log");
    reset_screen();
    submit_command("bootlog");
    CHECK(dump_calls == 1U,
          "the BOOTLOG command asks the boot log for its records");
    CHECK(screen_contains("BOOTLOG: state=READY path=\\EFI\\HFOC\\BOOTLOG.BIN"),
          "the BOOTLOG command paints what the log answered");
    console_bind_boot_log(NULL);
    reset_screen();
    submit_command("bootlog");
    CHECK(screen_contains("BOOTLOG: UNAVAILABLE"),
          "without a bound log the BOOTLOG command says so instead of "
          "pretending");
    CHECK(dump_calls == 1U,
          "an unbound console never calls a log");
}

/*
 * ITEM-460: the durable transcript of the console's own conversation.
 *
 * The owner asked that the screen stop being the only record of what a run
 * said.  The command the operator typed and the answer it produced are handed
 * to the bound log as `[CONSOLE] cmd="..."` and `[CONSOLE] out="..."` records -
 * one output record per line, so a multi-line answer survives whole - and a
 * secret-bearing command is journaled with `<redacted>` in place of its
 * argument and is never written down.
 */
static bool observed_text_contains(const char *needle) {
    size_t index;

    for (index = 0U; index < observed_count; ++index) {
        if (strstr(observed[index], needle) != NULL) {
            return true;
        }
    }
    return false;
}

static void bind_host_journal(void) {
    const struct console_boot_log_binding binding = {
        console_host_observe,
        console_host_dump
    };

    observed_count = 0U;
    console_bind_boot_log(&binding);
}

/*
 * ITEM-461: the journal-only publication and the display record.  A cost
 * record must reach the log without painting - publishing it on the screen
 * would pay the cost it measures - while the `[FB]` geometry is a normal record
 * and appears on both surfaces.
 */
static void test_console_journal_publication_is_journal_only(void) {
    static const char cost[] =
        "[CONSOLE] paint lines=64 chars=1024 us=0 max_us=0";

    begin_case();
    console_bind_boot_log(NULL);
    console_init(1024U, 1U);
    bind_host_journal();
    reset_screen();

    console_publish_journal(cost, strlen(cost));

    CHECK(observed_contains(cost),
          "a journal-only record reaches the durable log");
    CHECK(!screen_contains("[CONSOLE] paint"),
          "a journal-only record is never painted");
    console_bind_boot_log(NULL);
}

/*
 * ITEM-477 (round 15): the periodic heartbeat keeps its two readers and loses
 * its third.  The 100 Hz tick turned the once-a-second input, phase and
 * ten-second loop records into the flood the owner saw; the screen budget
 * (ITEM-466) then spent itself repainting "still running" while the operator's
 * own view scrolled away.  The journal-only fragment builder must place the
 * line in the bound log and leave the framebuffer alone, while a one-time
 * state-change record and a typed command stay on both surfaces.
 */
static void test_periodic_records_are_journal_only(void) {
    static const char state[] = "[KBD ] state=READY reason=USB_HID";

    begin_case();
    console_bind_boot_log(NULL);
    console_init(1024U, 1U);
    bind_host_journal();
    observed_count = 0U;
    reset_screen();

    /* The periodic form, exactly as the loop assembles [PRODUCTION-INPUT]. */
    console_journal_begin();
    console_record_text("[PRODUCTION-INPUT] kbd_reports=");
    console_record_u64(12U);
    console_record_text(" mouse_reports=");
    console_record_u64(3U);
    console_record_text("\n");
    console_journal_end();

    CHECK(observed_contains(
              "[PRODUCTION-INPUT] kbd_reports=12 mouse_reports=3"),
          "a periodic record reaches the durable journal");
    CHECK(!screen_contains("[PRODUCTION-INPUT]"),
          "a periodic record is never painted on the screen");

    /* A one-time state change keeps the painted mirror. */
    console_record_begin();
    console_record_text(state);
    console_record_end();
    CHECK(observed_contains(state),
          "a state-change record still reaches the durable journal");
    CHECK(screen_contains(state),
          "a state-change record still paints the screen");

    /* The operator's command keeps painting, and still reaches the journal. */
    reset_screen();
    submit_command("kbd");
    CHECK(screen_contains("KEYBOARD IRQ:"),
          "the operator's command still paints its answer");
    CHECK(observed_text_contains("USB HID: REPORTS="),
          "the operator's command still reaches the durable journal");

    console_bind_boot_log(NULL);
}

/*
 * ITEM-478 (round 16): the screen carries only the product banner, the prompt,
 * the operator's own typing and answers, the local menu and an input error.
 * The readiness facts and every service marker reach the serial stream and the
 * durable journal and never the framebuffer.  The `[CONSOLE] boot` record names
 * the boot screen's own paint count, which is exactly the banner's one line.
 */
static void test_service_lines_stay_off_the_screen(void) {
    begin_case();
    console_publish_input_source(LOCAL_INPUT_SOURCE_PS2);
    console_publish_local_input(true);
    bind_host_journal();
    console_init(1024U, 1U);

    CHECK(screen_contains("HFOC OS 1.0.0 INITIALISED"),
          "the product banner is painted");
    CHECK(!screen_contains("GDT IDT LEGACY PIC PIT PS2"),
          "the readiness banner is not painted");
    CHECK(!screen_contains("TYPE HELP FOR COMMANDS"),
          "the command hint is not painted");
    CHECK(!screen_contains("DOUBLE FAULT IST ENABLED"),
          "the interrupt readiness line is not painted");
    CHECK(serial_contains("GDT IDT LEGACY PIC PIT PS2: OK\n"),
          "the readiness banner still reaches the serial stream");
    CHECK(observed_text_contains("GDT IDT LEGACY PIC PIT PS2: OK"),
          "the readiness banner still reaches the durable journal");
    CHECK(serial_contains("[CONSOLE] boot paint lines=1 "),
          "the boot screen painted only the banner");
    CHECK(observed_text_contains("[CONSOLE] boot paint lines=1 "),
          "the boot paint count reaches the durable journal");
    CHECK(serial_contains("service_lines="),
          "the boot record names the service lines it kept off the screen");

    /* The offer marker is a service marker: serial and journal, no screen. */
    reset_screen();
    reset_serial();
    console_offer_local_menu(1000U);
    console_offer_local_menu(1000U + LOCAL_MENU_OFFER_MS);
    CHECK(serial_contains("[LOCAL-MENU] offered reason=IDLE"),
          "the menu offer still reaches the serial stream");
    CHECK(observed_text_contains("[LOCAL-MENU] offered reason=IDLE"),
          "the menu offer still reaches the durable journal");
    CHECK(!screen_contains("[LOCAL-MENU] offered"),
          "the menu offer marker is not painted");
    CHECK(screen_contains("PRESS ENTER FOR THE LOCAL MENU"),
          "the menu's own offer is still painted as the UI");

    /* The operator's command keeps painting, and still reaches the journal. */
    reset_screen();
    submit_command("kbd");
    CHECK(screen_contains("KEYBOARD IRQ:"),
          "the operator's command still paints its answer");
    console_bind_boot_log(NULL);
}

static void test_console_display_record(void) {
    static const char geometry[] =
        "[FB] width=1920 height=1080 pitch=7680 cells=120x33 bpp=32"
        " modes=3 chosen=1280x720 requested=1024x768 actual=1920x1080"
        " fallback=1 scale=2 cell_px=16x32 preferred=1024x768"
        " available_count=0 available=";
    struct framebuffer_mode_choice choice = {
        .preferred_width = 1024U,
        .preferred_height = 768U,
        .chosen_width = 1280U,
        .chosen_height = 720U,
        .available_count = 0U,
        .fallback = 1U
    };

    begin_case();
    console_bind_boot_log(NULL);
    console_init(1024U, 1U);
    bind_host_journal();
    reset_screen();

    console_publish_display(1920U, 1080U, 7680U, 32U, 3U, &choice);

    CHECK(observed_contains(geometry),
          "the display geometry reaches the durable log");
    /*
     * ITEM-478 (round 16): the geometry is a service record, so it reaches the
     * journal and the serial stream and not the screen; the `kbd` command
     * prints the same numbers when the operator asks for them.
     */
    CHECK(!screen_contains(geometry),
          "the display geometry is not painted for the operator");
    CHECK(serial_contains("available_count=0 available="),
          "the display geometry still reaches the serial stream");
    console_bind_boot_log(NULL);
}

/*
 * ITEM-464: the console mode selection rule.  The firmware's list decides: the
 * smallest 32-bpp mode at least 1024x768 wins; when none reaches the floor the
 * smallest 32-bpp mode is the answer; a list with no usable mode answers false
 * so the caller falls back to the actual mode.
 */
static void test_framebuffer_mode_choice(void) {
    struct limine_video_mode small = {.width = 640U, .height = 480U,
                                      .bpp = 32U};
    struct limine_video_mode floor = {.width = 1024U, .height = 768U,
                                      .bpp = 32U};
    struct limine_video_mode big = {.width = 3840U, .height = 2160U,
                                    .bpp = 32U};
    struct limine_video_mode deep = {.width = 1024U, .height = 768U,
                                     .bpp = 24U};
    struct limine_video_mode *mixed[] = {&big, &floor, &small, &deep};
    struct limine_video_mode *only_small[] = {&deep, &small};
    struct limine_video_mode *only_deep[] = {&deep};
    uint64_t width = 0U;
    uint64_t height = 0U;

    begin_case();
    CHECK(framebuffer_choose_mode(mixed, 4U, &width, &height),
          "a mode list with a bounded 32-bpp mode is usable");
    CHECK(width == 1024U && height == 768U,
          "the smallest mode at least as large as the floor is chosen");
    width = 0U;
    height = 0U;
    CHECK(framebuffer_choose_mode(only_small, 2U, &width, &height),
          "a list whose only 32-bpp mode is below the floor is usable");
    CHECK(width == 640U && height == 480U,
          "the smallest mode offered is chosen when none reaches the floor");
    CHECK(!framebuffer_choose_mode(only_deep, 1U, &width, &height),
          "a list with no 32-bpp mode answers false");
    CHECK(!framebuffer_choose_mode(NULL, 0U, &width, &height),
          "an absent mode list answers false");
}

/*
 * ITEM-466 (round 5): the automatic mode choice.  The firmware's preference is
 * the active mode's own entry in the list, not necessarily `modes[0]`: the
 * 2026-09-21 CI run booted a 1024x768 EDID at 1024x768 while `modes[0]` was
 * larger.  An active mode that is in the list is the choice and not a fallback;
 * one that is absent falls back to the largest mode with `fallback=1`.  A list
 * with no 32-bpp mode answers false so the caller publishes the actual mode.
 */
static void test_framebuffer_preferred_mode_choice(void) {
    struct limine_video_mode big = {.width = 3840U, .height = 2160U,
                                    .bpp = 32U};
    struct limine_video_mode floor = {.width = 1024U, .height = 768U,
                                      .bpp = 32U};
    struct limine_video_mode small = {.width = 640U, .height = 480U,
                                      .bpp = 32U};
    struct limine_video_mode deep = {.width = 1024U, .height = 768U,
                                     .bpp = 24U};
    struct limine_video_mode *mixed[] = {&big, &floor, &small, &deep};
    struct limine_video_mode *only_small[] = {&small, &deep};
    struct limine_video_mode *only_deep[] = {&deep};
    struct framebuffer_mode_choice choice;

    begin_case();
    CHECK(framebuffer_choose_preferred(mixed, 4U, 3840U, 2160U, &choice),
          "a list with a 32-bpp mode is usable");
    CHECK(choice.preferred_width == 3840U && choice.preferred_height == 2160U,
          "the active mode's own entry is the firmware's preference");
    CHECK(choice.available_count == 3U,
          "only the 32-bpp modes are counted as available");
    CHECK(choice.chosen_width == 3840U && choice.chosen_height == 2160U &&
              choice.fallback == 0U,
          "the native mode is the choice and not a fallback");
    CHECK(framebuffer_choose_preferred(mixed, 4U, 1024U, 768U, &choice),
          "the rule still answers when the list is not ordered by preference");
    CHECK(choice.preferred_width == 1024U && choice.preferred_height == 768U,
          "the active mode is found in the list, not assumed to be first");
    CHECK(choice.chosen_width == 1024U && choice.chosen_height == 768U &&
              choice.fallback == 0U,
          "an active mode that is in the list is chosen without a fallback");
    CHECK(framebuffer_choose_preferred(mixed, 4U, 1280U, 800U, &choice),
          "an active mode absent from the list is still answered");
    CHECK(choice.chosen_width == 3840U && choice.chosen_height == 2160U,
          "a refused preference falls back to the largest available mode");
    CHECK(choice.fallback == 1U,
          "a missing preference is published as fallback=1");
    CHECK(framebuffer_choose_preferred(only_small, 2U, 640U, 480U, &choice),
          "a list whose only 32-bpp mode is small is usable");
    CHECK(choice.preferred_width == 640U && choice.preferred_height == 480U,
          "the small mode is the preference when it is the only 32-bpp one");
    CHECK(choice.chosen_width == 640U && choice.chosen_height == 480U &&
              choice.fallback == 0U,
          "the only offered mode is honoured without a fallback");
    CHECK(!framebuffer_choose_preferred(only_deep, 1U, 1024U, 768U, &choice),
          "a list with no 32-bpp mode answers false");
    CHECK(!framebuffer_choose_preferred(NULL, 0U, 1024U, 768U, &choice),
          "an absent mode list answers false");
}

/*
 * ITEM-466 (round 5): the screen output budget.  Every record still reaches the
 * durable journal, but only a bounded number are painted per one-second tick
 * window; the rest are counted and published, so "the screen is quieter" is not
 * "the machine stopped talking".
 */
static void test_console_screen_budget(void) {
    char record[] = "[FB] budget 00";
    const uint64_t suppressed_before = console_render_screen_suppressed();
    unsigned int index;

    begin_case();
    console_bind_boot_log(NULL);
    console_init(1024U, 1U);
    bind_host_journal();
    observed_count = 0U;
    reset_screen();
    /* Freeze the tick so every publish lands in the same window. */
    timer_ticks_step = 0U;
    for (index = 0U; index < 16U; ++index) {
        record[12] = (char)('0' + (index / 10U) % 10U);
        record[13] = (char)('0' + index % 10U);
        console_publish_record(record, strlen(record));
    }
    CHECK(observed_text_contains("budget 00"),
          "the first record of the burst is journaled");
    CHECK(observed_text_contains("budget 15"),
          "a suppressed record still reaches the durable journal");
    CHECK(console_render_screen_suppressed() - suppressed_before >= 4U,
          "the records over the screen budget are counted as suppressed");
    CHECK(!screen_contains("[FB] budget 15"),
          "a suppressed record is not painted on the screen");
    console_bind_boot_log(NULL);
}

static void test_console_journal_command_and_answer(void) {
    bind_host_journal();

    submit_command("version");

    CHECK(observed_contains("[CONSOLE] cmd=\"version\""),
          "the typed command reaches the durable journal");
    /* ITEM-558: the version answer now names the build the kernel was compiled
       from, so the assertion follows the line the operator reads.  The host
       build has no generated identity, which the command reports honestly. */
    CHECK(observed_contains("[CONSOLE] out=\"HFOC OS 1.0.0 X86-64 build=unknown version=unknown\""),
          "the answer to the command reaches the durable journal too");
    console_bind_boot_log(NULL);
}

static void test_console_journal_multi_line_answer(void) {
    bind_host_journal();

    submit_command("kbd");

    CHECK(observed_contains("[CONSOLE] cmd=\"kbd\""),
          "the kbd command is journaled");
    CHECK(observed_text_contains("KEYBOARD IRQ:"),
          "the first line of the answer is journaled");
    CHECK(observed_text_contains("USB HID: REPORTS="),
          "the last line of a multi-line answer is journaled as its own "
          "record");
    console_bind_boot_log(NULL);
}

/*
 * ITEM-506: a typed password never reaches the durable journal, the history or
 * the saved draft.  The old `admin-provision <SECRET>` argument is gone - the
 * command takes no argument at all - so the rule to prove now is stronger: the
 * hidden conversation line is not a command, not a transcript and not a
 * journal record, and the line editor never recalls it.
 */
static void test_console_journal_hidden_input(void) {
    static const char secret[] = "correct horse battery staple";
    static const char new_login[] = "operator";
    struct credentials_fixture fixture;

    memset(&fixture, 0, sizeof(fixture));
    fixture.verify_ok = true;
    fixture.provision_ok = true;
    bind_credentials_fixture(&fixture);
    bind_host_journal();

    /*
     * The history is private, so the operator's own recall is the probe: the
     * anchor is added before the conversation, and the whole history is then
     * walked with Up.  A hidden line that had been added would be one of the
     * entries this walk recalls, so the exact secret would appear in the line
     * or on the screen.
     */
    console_test_add_history("history-anchor");

    submit_command("credentials");
    submit_command(secret);
    submit_command(new_login);
    submit_command(secret);
    submit_command(secret);

    CHECK(observed_contains("[CONSOLE] cmd=\"credentials\""),
          "the command itself is journaled");
    CHECK(!observed_text_contains(secret),
          "the typed password is never written to the durable journal");
    CHECK(!observed_text_contains("correct horse"),
          "not even the first word of the password is journaled");
    CHECK(observed_contains(
              "[ADMIN-ENROLLMENT] credentials changed login=operator"),
          "the account change reaches the durable journal");
    reset_screen();
    reset_serial();
    for (unsigned int step = 0U; step < 8U; ++step) {
        send_event(KEYBOARD_EVENT_UP, '\0');
        CHECK(!line_is(secret),
              "the hidden password never becomes a history entry");
        CHECK(!screen_contains(secret) && !serial_contains(secret),
              "recalling the newest entry never redraws the password");
    }
    /* Leave the line empty for the next case. */
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    console_bind_boot_log(NULL);
    console_bind_admin_credentials(NULL);
}

/*
 * ITEM-506: the two removed `admin-provision <secret>` / `admin-login <secret>`
 * spellings do not run any more, but the shipped manuals still teach them, so
 * an operator can put a password on the line.  The three places that would
 * keep it - the durable command record, the `UNKNOWN COMMAND` echo and the
 * input history - must strip it, exactly as they did before the names were
 * removed.  The typed echo itself is the trusted local console channel and is
 * not part of this claim.
 */
static void test_console_journal_removed_secret_command(void) {
    static const char secret[] = "hunter2";
    char command[] = "admin-provision hunter2";
    const char *cursor;

    bind_host_journal();

    /*
     * The anchor is the newest history entry before the line under test, so if
     * the removed spelling were added to the history it would become the
     * newest entry and one Up would load it.
     */
    console_test_add_history("history-anchor");
    /*
     * Type the line, then clear both captures: everything after this point is
     * exactly what the ENTER produces, not the console's own typing echo.
     */
    for (cursor = command; *cursor != '\0'; ++cursor) {
        send_char(*cursor);
    }
    reset_serial();
    reset_screen();
    send_event(KEYBOARD_EVENT_ENTER, '\0');

    CHECK(observed_contains("[CONSOLE] cmd=\"admin-provision <redacted>\""),
          "the durable command record redacts the removed secret argument");
    CHECK(!observed_text_contains(secret),
          "the removed spelling's secret never reaches the durable journal");
    CHECK(serial_contains("UNKNOWN COMMAND: admin-provision <redacted>"),
          "the refusal names the command with its argument redacted");
    CHECK(!serial_contains(secret) && !screen_contains(secret),
          "the refusal echo does not repeat the secret");

    reset_screen();
    reset_serial();
    send_event(KEYBOARD_EVENT_UP, '\0');
    CHECK(line_is("history-anchor"),
          "the removed spelling is not recallable from the history");
    CHECK(!screen_contains(secret) && !serial_contains(secret),
          "recalling the newest entry never redraws the removed spelling");
    /* Leave the line empty for the next case. */
    send_event(KEYBOARD_EVENT_ENTER, '\0');

    console_bind_boot_log(NULL);
}

static void test_console_journal_unknown_command(void) {
    bind_host_journal();

    submit_command("nosuchthing");

    CHECK(observed_contains("[CONSOLE] cmd=\"nosuchthing\""),
          "an unknown command is journaled");
    CHECK(observed_text_contains("UNKNOWN COMMAND: nosuchthing"),
          "and so is the refusal it produced");
    console_bind_boot_log(NULL);
}

static void test_console_journal_menu_and_offer(void) {
    begin_case();
    console_bind_boot_log(NULL);
    console_publish_input_source(LOCAL_INPUT_SOURCE_PS2);
    console_publish_local_input(true);
    console_init(512U, 1U);
    bind_host_journal();
    console_offer_local_menu(1000U);
    console_offer_local_menu(1000U + LOCAL_MENU_OFFER_MS);
    CHECK(observed_text_contains("[LOCAL-MENU] offered reason=IDLE"),
          "the offer the machine made is journaled");

    observed_count = 0U;
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    CHECK(observed_text_contains("[LOCAL-MENU] open"),
          "opening the offered menu is journaled");

    observed_count = 0U;
    send_char('q');
    CHECK(observed_text_contains("[LOCAL-MENU] closed"),
          "closing the menu is journaled");
    console_bind_boot_log(NULL);
}

/*
 * ITEM-447: the Network entry asks the four questions, the answer is committed
 * through the record owner the boot path binds, and a refused answer writes
 * nothing.  The stub owner is the proof's own: it records what the console
 * asked it to store.
 */
static struct network_ipv4_config wizard_committed_config;
static bool wizard_commit_called;
static bool wizard_commit_static;
static bool wizard_commit_result;

static bool wizard_commit(
    void *context,
    bool is_static,
    const struct network_ipv4_config *config) {
    (void)context;
    wizard_commit_called = true;
    wizard_commit_static = is_static;
    if (config != NULL) {
        wizard_committed_config = *config;
    }
    return wizard_commit_result;
}

/*
 * ITEM-496: the stubbed reading the questions start from.  The console never
 * invents one - what the binding refuses to render is published as
 * UNAVAILABLE - so the proof can hand it any rendering, including none.
 */
static char wizard_current_text[32];

static bool wizard_current(
    void *context,
    char *output,
    size_t capacity,
    size_t *output_length) {
    size_t length = 0U;

    (void)context;
    if (output == NULL || output_length == NULL || capacity == 0U) {
        return false;
    }
    while (wizard_current_text[length] != '\0' &&
           length + 1U < capacity) {
        output[length] = wizard_current_text[length];
        ++length;
    }
    output[length] = '\0';
    *output_length = length;
    return length != 0U;
}

static void type_line(const char *line) {
    while (*line != '\0') {
        send_char(*line++);
    }
    send_event(KEYBOARD_EVENT_ENTER, '\0');
}

static void wizard_reset(void) {
    begin_case();
    console_bind_network_config(NULL);
    wizard_commit_called = false;
    wizard_commit_static = false;
    wizard_commit_result = true;
    memset(&wizard_committed_config, 0, sizeof(wizard_committed_config));
    strcpy(wizard_current_text, "DHCP");
}

static void open_network_entry(void) {
    static const struct console_network_binding binding = {
        NULL, wizard_commit, wizard_current
    };

    console_init(512U, 1U);
    console_bind_network_config(&binding);
    type_line("menu");
    /* Status is first; one DOWN selects Network. */
    send_event(KEYBOARD_EVENT_DOWN, '\0');
    send_event(KEYBOARD_EVENT_ENTER, '\0');
}

static void test_local_network_wizard_static(void) {
    wizard_reset();
    open_network_entry();
    CHECK(serial_contains("[LOCAL-MENU] run action=network\n"),
          "the Network entry is the one the menu ran");
    CHECK(serial_contains("[LOCAL-MENU] closed\n"),
          "the menu hands the keyboard back to the questions");
    /* ITEM-496: what the interface has is read out before the questions, and
       it is one IRQ-safe write like every other marker. */
    CHECK(serial_contains_atomic(
              "[PRODUCTION-NET-SET] state=OFFERED current=DHCP\n"),
          "the current configuration is published before the questions");
    CHECK(serial_contains_atomic(
              "network mode [dhcp|static] (empty cancels): "),
          "the first question is the mode, as one IRQ-safe write");

    type_line("static");
    CHECK(serial_contains_atomic("address a.b.c.d/prefix: "),
          "the static answer asks for the address");
    type_line("10.0.2.15/24");
    CHECK(serial_contains_atomic("gateway: "),
          "the address answer asks for the gateway");
    type_line("10.0.2.2");
    CHECK(serial_contains("dns: "),
          "the gateway answer asks for the DNS");
    type_line("10.0.2.3");
    CHECK(wizard_commit_called && wizard_commit_static &&
              wizard_committed_config.prefix_length == 24U &&
              wizard_committed_config.address.octets[3] == 15U &&
              wizard_committed_config.gateway.octets[3] == 2U &&
              wizard_committed_config.dns.octets[3] == 3U,
          "the completed answer is committed through the bound owner");
    /* ITEM-496: the answer is one IRQ-safe write, it is durable, and it names
       both what was stored and what the interface still runs. */
    CHECK(serial_contains_atomic(
              "[PRODUCTION-NET-SET] mode=STATIC address=10.0.2.15/24 "
              "gateway=10.0.2.2 dns=10.0.2.3 persisted=OK applies=reboot "
              "current=DHCP\n"),
          "the committed answer is published with its address and the "
          "configuration still running");
}

static void test_local_network_wizard_dhcp(void) {
    wizard_reset();
    strcpy(wizard_current_text, "STATIC address=10.0.2.15/24");
    open_network_entry();
    type_line("dhcp");
    CHECK(wizard_commit_called && !wizard_commit_static,
          "the DHCP answer commits discovery");
    CHECK(serial_contains_atomic(
              "[PRODUCTION-NET-SET] mode=DHCP persisted=OK applies=reboot "
              "current=STATIC address=10.0.2.15/24\n"),
          "the DHCP answer publishes its record and the address the "
          "interface keeps until the reboot");
}

/*
 * ITEM-496: a binding that cannot render the current configuration is
 * published as UNAVAILABLE.  The questions still run - the operator can still
 * change the address - but the record never claims a reading nobody made.
 */
static void test_local_network_wizard_unknown_current(void) {
    wizard_reset();
    wizard_current_text[0] = '\0';
    open_network_entry();
    type_line("dhcp");
    CHECK(serial_contains_atomic(
              "[PRODUCTION-NET-SET] mode=DHCP persisted=OK applies=reboot "
              "current=UNAVAILABLE\n"),
          "an unreadable current configuration is published as UNAVAILABLE");
}

static void test_local_network_wizard_refusals(void) {
    wizard_reset();
    open_network_entry();
    type_line("ethernet");
    CHECK(!wizard_commit_called &&
              serial_contains_atomic(
                  "[PRODUCTION-NET-SET] state=REFUSED reason=MODE\n"),
          "an unknown mode is refused and nothing is committed");

    wizard_reset();
    open_network_entry();
    type_line("static");
    type_line("10.0.2.15/33");
    type_line("10.0.2.2");
    type_line("10.0.2.3");
    CHECK(!wizard_commit_called &&
              serial_contains_atomic("reason=NETWORK_IPV4_INVALID_PREFIX"),
          "an invalid prefix is refused and nothing is committed");

    wizard_reset();
    open_network_entry();
    type_line("");
    CHECK(!wizard_commit_called &&
              serial_contains_atomic("[PRODUCTION-NET-SET] state=CANCELLED\n"),
          "an empty answer cancels without committing");
}

static void test_local_network_wizard_unbound(void) {
    wizard_reset();
    console_init(512U, 1U);
    type_line("menu");
    send_event(KEYBOARD_EVENT_DOWN, '\0');
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    CHECK(!serial_contains("network mode [dhcp|static]"),
          "without a record owner the entry does not start the questions");
    /* ITEM-496: and it names the reason instead of running something else. */
    CHECK(serial_contains_atomic(
              "[PRODUCTION-NET-SET] state=UNAVAILABLE "
              "reason=NO_RECORD_OWNER\n"),
          "without a record owner the entry publishes the reason");
    CHECK(serial_contains("[LOCAL-MENU] run action=network\n"),
          "without a record owner the entry keeps its old record");
}

static void test_local_network_wizard_failed_persist(void) {
    wizard_reset();
    wizard_commit_result = false;
    open_network_entry();
    type_line("dhcp");
    CHECK(wizard_commit_called &&
              serial_contains_atomic(
                  "[PRODUCTION-NET-SET] mode=DHCP persisted=FAILED "
                  "applies=reboot current=DHCP\n"),
          "a record the owner refused says persisted=FAILED");
}


/*
 * ITEM-448: a left click runs the menu entry whose painted line it landed on.
 *
 * The keyboard path is proven above; this is the pointer's half - the console
 * starts the arrow in the middle of the screen, moves it by the report's own
 * deltas, hit-tests the rectangles the last menu paint recorded, and runs the
 * entry through the same local_menu_run() the ENTER key uses.  A click that
 * lands on no entry is published as a miss and changes nothing.
 */
static void test_pointer_click_runs_menu_entry(void) {
    struct usb_hid_mouse_event event;
    int32_t target_x = 0;
    int32_t target_y = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t pointer_x = 0;
    int32_t pointer_y = 0;

    begin_case();
    /* ITEM-470: this case proves the pointer arithmetic, so it pins the 1:1
       baseline; the console's scaled path has its own case below. */
    host_font_scale = 1U;
    console_init(512U, 1U);
    host_font_scale = 2U;
    reset_serial();
    type_line("menu");
    CHECK(serial_contains("[LOCAL-MENU] open\n"),
          "the menu is open before the click proof");
    CHECK(console_test_menu_target_count() == 6U,
          "the paint recorded one rectangle per menu entry");
    CHECK(console_test_menu_target(1U, &target_x, &target_y, &width, &height) &&
              width > 0 && height > 0,
          "the Network entry has a rectangle a click can land in");

    /* The first report places the arrow in the middle of the screen. */
    memset(&event, 0, sizeof(event));
    event.dx = (int16_t)(target_x + width / 2 - 512);
    event.dy = (int16_t)(target_y + height / 2 - 384);
    /* ITEM-463: clear the journal window just before the move, so the
       pointer's own record is the one this case reads. */
    bind_host_journal();
    console_process_mouse_event(&event);
    CHECK(console_pointer_position(&pointer_x, &pointer_y) &&
              pointer_x == target_x + width / 2 &&
              pointer_y == target_y + height / 2,
          "a movement report moves the arrow to the entry's own line");
    /* ITEM-463: the pointer's own cost record is journal-only. */
    CHECK(observed_text_contains("[POINTER] moves="),
          "the pointer publishes its own move count and cost");

    memset(&event, 0, sizeof(event));
    event.buttons = USB_HID_MOUSE_BUTTON_LEFT;
    event.pressed = USB_HID_MOUSE_BUTTON_LEFT;
    console_process_mouse_event(&event);
    memset(&event, 0, sizeof(event));
    event.released = USB_HID_MOUSE_BUTTON_LEFT;
    console_process_mouse_event(&event);
    CHECK(serial_contains("[POINTER] click entry=network action=RUN"),
          "the click names the entry under the pointer and that it ran");
    CHECK(serial_contains("[LOCAL-MENU] run action=network\n"),
          "the click ran the entry through the keyboard's own path");
    CHECK(cursor_shows > 0U && cursor_last_x == target_x + width / 2 &&
              cursor_last_y == target_y + height / 2,
          "the arrow was painted where the report put it");

    /* A second click away from every entry is a published miss. */
    reset_serial();
    memset(&event, 0, sizeof(event));
    event.dx = (int16_t)(5 - pointer_x);
    event.dy = (int16_t)(5 - pointer_y);
    console_process_mouse_event(&event);
    memset(&event, 0, sizeof(event));
    event.buttons = USB_HID_MOUSE_BUTTON_LEFT;
    event.pressed = USB_HID_MOUSE_BUTTON_LEFT;
    console_process_mouse_event(&event);
    memset(&event, 0, sizeof(event));
    event.released = USB_HID_MOUSE_BUTTON_LEFT;
    console_process_mouse_event(&event);
    CHECK(serial_contains("[POINTER] click entry=NONE action=NONE"),
          "a click outside every entry is a published miss");
    CHECK(!serial_contains("[LOCAL-MENU] run action="),
          "a miss runs nothing");
}

/*
 * ITEM-480 (round 18) / ITEM-482 (round 20): the console's batch path.  A run
 * of decoded movements arrives as one batch and draws the arrow once; the
 * write-combining drain (`framebuffer_sync`) no longer runs per batch - it runs
 * once for the whole input batch in `console_end_input_batch()`, so three
 * coalesced draws pay one drain.  A button edge is a batch of its own and is
 * applied in order without drawing or dirtying the surface.  The per-turn
 * budget itself is the loop's (`local_input`'s), and its host proof lives in
 * tests/local-input-host.c.
 */
static void test_mouse_batch_coalesces_and_drains_once(void) {
    struct usb_hid_mouse_batch batch;
    uint64_t moves_before;
    unsigned int syncs_before;

    begin_case();
    host_font_scale = 1U;
    console_init(512U, 1U);

    memset(&batch, 0, sizeof(batch));
    batch.dx = 5;
    batch.dy = 3;
    batch.events = 5U;
    moves_before = console_render_pointer_moves();
    syncs_before = framebuffer_sync_calls;
    console_process_mouse_batch(&batch);
    CHECK(console_render_pointer_moves() == moves_before + 1U,
          "a coalesced movement run draws the arrow once");
    CHECK(framebuffer_sync_calls == syncs_before,
          "a batch does not drain the framebuffer on its own");
    console_end_input_batch();
    CHECK(framebuffer_sync_calls == syncs_before + 1U,
          "the input batch drains the write-combining stores exactly once");

    moves_before = console_render_pointer_moves();
    syncs_before = framebuffer_sync_calls;
    memset(&batch, 0, sizeof(batch));
    batch.pressed = USB_HID_MOUSE_BUTTON_LEFT;
    batch.events = 1U;
    console_process_mouse_batch(&batch);
    CHECK(console_render_pointer_moves() == moves_before,
          "a button edge alone does not draw the arrow");
    console_end_input_batch();
    CHECK(framebuffer_sync_calls == syncs_before,
          "a button edge alone does not drain the framebuffer");

    reset_serial();
    memset(&batch, 0, sizeof(batch));
    batch.released = USB_HID_MOUSE_BUTTON_LEFT;
    batch.events = 1U;
    console_process_mouse_batch(&batch);
    CHECK(serial_contains("[POINTER] click entry=NONE action=NONE"),
          "the release still reaches the click path in order");

    console_process_mouse_batch(NULL);
    CHECK(console_render_pointer_moves() == moves_before,
          "a null batch changes nothing");
}

/*
 * ITEM-484 (round 22): the arrow appears on mouse readiness.
 *
 * The owner's hardware run left the screen without a cursor until the mouse
 * was moved: the console marked the pointer ready on its first batch but only
 * painted the arrow inside the movement branch, so a zero report or a bare
 * click left the surface empty.  Readiness is the controller's own state - the
 * loop asks `local_input_mouse_ready()` - and the one announcement centres the
 * arrow and paints it exactly once; a click is never the mechanism that makes
 * it appear, and a zero report neither hides nor repaints it.
 */
static void test_the_pointer_appears_on_mouse_readiness(void) {
    struct usb_hid_mouse_event click;
    struct usb_hid_mouse_batch zero;
    struct usb_hid_mouse_batch move;
    unsigned int shows_before;
    int32_t centre_x = (int32_t)(1024U / 2U);
    int32_t centre_y = (int32_t)(768U / 2U);

    begin_case();
    host_font_scale = 1U;
    console_init(512U, 1U);

    /* A bare click before readiness must not create the arrow. */
    memset(&click, 0, sizeof(click));
    click.buttons = USB_HID_MOUSE_BUTTON_LEFT;
    click.pressed = USB_HID_MOUSE_BUTTON_LEFT;
    console_process_mouse_event(&click);
    CHECK(!cursor_visible_stub,
          "a click without movement does not create the cursor");

    /* Readiness paints the arrow once, centred. */
    shows_before = cursor_shows;
    console_pointer_ready();
    CHECK(cursor_visible_stub,
          "the cursor is visible as soon as the mouse is ready");
    CHECK(cursor_shows == shows_before + 1U,
          "readiness paints the arrow exactly once");
    CHECK(cursor_last_x == centre_x && cursor_last_y == centre_y,
          "the arrow starts in the middle of the screen");
    console_pointer_ready();
    CHECK(cursor_shows == shows_before + 1U,
          "a repeated readiness does not repaint the arrow");

    /* A zero report must not delay or hide the visible arrow. */
    memset(&zero, 0, sizeof(zero));
    console_process_mouse_batch(&zero);
    CHECK(cursor_visible_stub,
          "a zero movement report leaves the cursor visible");
    CHECK(cursor_shows == shows_before + 1U,
          "a zero movement report does not repaint the arrow");

    /* The first movement moves the already-visible arrow. */
    memset(&move, 0, sizeof(move));
    move.dx = 13;
    move.dy = -7;
    move.events = 2U;
    console_process_mouse_batch(&move);
    CHECK(cursor_visible_stub && cursor_shows == shows_before + 2U,
          "the first movement repaints the arrow in its new place");
    CHECK(cursor_last_x == centre_x + 13 && cursor_last_y == centre_y - 7,
          "the movement report moves the visible arrow by its deltas");
}

/*
 * ITEM-484 (round 22): many draws, one drain.
 *
 * The model the item fixes is that the batches draw and raise the deferred
 * drain flag, and the single `console_end_input_batch()` that closes the
 * keyboard plus the bounded mouse batches performs exactly one
 * `framebuffer_sync()`.  Draw telemetry and the barrier telemetry stay
 * separate.  This proof drives several coalesced batches and counts the
 * framebuffer drains.
 */
static void test_many_batches_close_with_one_drain(void) {
    struct usb_hid_mouse_batch batch;
    unsigned int shows_before;
    unsigned int syncs_before;
    unsigned int index;

    begin_case();
    host_font_scale = 1U;
    console_init(512U, 1U);
    console_pointer_ready();
    CHECK(cursor_visible_stub, "the arrow is up before the batch proof");

    shows_before = cursor_shows;
    syncs_before = framebuffer_sync_calls;
    for (index = 0U; index < 3U; ++index) {
        memset(&batch, 0, sizeof(batch));
        batch.dx = 4;
        batch.dy = 2;
        batch.events = 4U;
        console_process_mouse_batch(&batch);
    }
    CHECK(cursor_shows == shows_before + 3U,
          "three coalesced batches draw the arrow three times");
    CHECK(framebuffer_sync_calls == syncs_before,
          "no batch drains the write-combining stores on its own");
    console_end_input_batch();
    CHECK(framebuffer_sync_calls == syncs_before + 1U,
          "three batches close with exactly one drain");
    console_end_input_batch();
    CHECK(framebuffer_sync_calls == syncs_before + 1U,
          "a batch with nothing new to drain does not drain again");
}

/*
 * ITEM-484 (round 22): the deferred echo replays inside the input batch.
 *
 * A long typed line on a scaled screen wraps, `new_line()` opens the batched
 * scroll, and every character after the wrap is buffered rather than painted.
 * The replay used to happen at the very end of the turn, after the network, the
 * heartbeat and the diagnostics, which is the residual typing delay the owner
 * reported.  The main loop now replays it just before the input batch's single
 * drain.  This proof types until the line wraps, then requires that the echo is
 * not painted until that in-batch flush - and that the flush's replay is what
 * the batch's one drain makes visible.
 */
static bool echo_char_on_screen(char value) {
    char needle[2];

    needle[0] = value;
    needle[1] = '\0';
    return screen_contains(needle);
}

static void test_the_deferred_echo_replays_inside_the_batch(void) {
    char deferred = '\0';
    char probe = 'z';
    char candidate;
    unsigned int syncs_before;
    unsigned int after_flush;
    unsigned int index;

    begin_case();
    /* Two-pixel glyph scale means twelve-pixel cells: the typeable line wraps
       before INPUT_MAX, so a typed character can open the deferred batch. */
    host_font_scale = 2U;
    console_init(512U, 1U);
    console_begin_turn_batching();
    for (index = 0U; index < 240U; ++index) {
        console_write("FILLER LINE\n");
    }
    console_flush_deferred();

    for (candidate = '!'; candidate <= '~' && deferred == '\0'; ++candidate) {
        if (candidate == '>') {
            continue;
        }
        reset_screen();
        send_char(candidate);
        if (!echo_char_on_screen(candidate)) {
            deferred = candidate;
        }
    }
    CHECK(deferred != '\0',
          "a typed character on the wrapped line is deferred");

    reset_screen();
    reset_serial();
    syncs_before = framebuffer_sync_calls;
    send_char(probe);
    CHECK(!echo_char_on_screen(probe),
          "the deferred echo waits instead of painting at once");
    CHECK(framebuffer_sync_calls == syncs_before,
          "the buffered echo pays no drain of its own");

    console_flush_deferred();
    CHECK(echo_char_on_screen(probe),
          "the deferred echo replays inside the input batch");
    after_flush = framebuffer_sync_calls;
    console_end_input_batch();
    CHECK(framebuffer_sync_calls == after_flush + 1U,
          "the replayed echo closes with the batch's single drain");
}

/*
 * ITEM-448: the newest `[POINTER] menu targets=` record in the capture, so the
 * proof can aim at the row the console itself published rather than at a
 * number it guessed.  The boot log scrolls the menu up as it is written, and
 * the console republishes the moved geometry; the last record is the one that
 * describes the screen a click would land on.
 */
static bool last_pointer_geometry(int32_t *x, int32_t *y, int32_t *line,
                                  int32_t *width) {
    const char *found = NULL;
    const char *cursor = serial_capture;

    while ((cursor = strstr(cursor, "[POINTER] menu targets=")) != NULL) {
        found = cursor;
        ++cursor;
    }
    if (found == NULL) {
        return false;
    }
    return sscanf(found,
                  "[POINTER] menu targets=%*d x=%d y=%d line=%d width=%d",
                  x, y, line, width) == 4;
}

/*
 * ITEM-448: a record written after the menu moves every painted line up, and
 * the rectangles a click hit-tests move with it - the entry the operator sees
 * on a row is the entry the click on that row runs.  The geometry the console
 * publishes is the one a proof aims at, so it is the published row that is
 * clicked here, after the log has scrolled the menu.
 */
static void test_pointer_targets_follow_scroll(void) {
    struct usb_hid_mouse_event event;
    int32_t before_x = 0;
    int32_t before_y = 0;
    int32_t before_width = 0;
    int32_t before_height = 0;
    int32_t published_x = 0;
    int32_t published_y = 0;
    int32_t published_line = 0;
    int32_t published_width = 0;
    int32_t published_height = 0;
    int32_t pointer_x = 0;
    int32_t pointer_y = 0;
    unsigned int index;

    begin_case();
    /* ITEM-470: the scroll case also pins the 1:1 pointer arithmetic.  The
       smaller cell fits more rows, so the filler must fill the body before the
       menu opens or nothing scrolls. */
    host_font_scale = 1U;
    console_init(512U, 1U);
    host_font_scale = 2U;
    /* Fill the body so the menu lands at the bottom of the screen and the
       records written after it scroll it. */
    for (index = 0U; index < 160U; ++index) {
        console_write("FILLER LINE\n");
    }
    reset_serial();
    type_line("menu");
    CHECK(serial_contains("[LOCAL-MENU] open\n"),
          "the menu is open before the scroll proof");
    CHECK(console_test_menu_target(0U, &before_x, &before_y, &before_width,
                                   &before_height),
          "the Status entry has a rectangle before the scroll");

    /* Ten records: the body scrolls, so the painted rows move up with it. */
    for (index = 0U; index < 10U; ++index) {
        console_write("SCROLL LINE\n");
    }
    if (!console_test_menu_target(0U, &published_x, &published_y,
                                  &published_width, &published_height)) {
        CHECK(false, "the Status entry still has a rectangle after the scroll");
        return;
    }
    CHECK(published_y < before_y,
          "a scroll moves the click rectangles up with the menu");
    CHECK(published_x == before_x && published_width == before_width,
          "a scroll does not move a rectangle sideways");

    /* The published geometry is what a reader aims at: click the middle of the
       row it names, from the pointer's own starting place in the middle.  The
       record is read before the capture is cleared for the click's own
       evidence. */
    CHECK(last_pointer_geometry(&published_x, &published_y, &published_line,
                                &published_width),
          "the moved geometry was published for a reader");
    CHECK(published_line == before_height,
          "the published line height is the menu's own row height");
    reset_serial();
    memset(&event, 0, sizeof(event));
    event.dx = (int16_t)(published_x + published_width / 2 - 512);
    event.dy = (int16_t)(published_y + published_line / 2 - 384);
    console_process_mouse_event(&event);
    CHECK(console_pointer_position(&pointer_x, &pointer_y) &&
              pointer_y == published_y + published_line / 2,
          "the arrow reaches the published row");
    memset(&event, 0, sizeof(event));
    event.buttons = USB_HID_MOUSE_BUTTON_LEFT;
    event.pressed = USB_HID_MOUSE_BUTTON_LEFT;
    console_process_mouse_event(&event);
    memset(&event, 0, sizeof(event));
    event.released = USB_HID_MOUSE_BUTTON_LEFT;
    console_process_mouse_event(&event);
    CHECK(serial_contains("[POINTER] click entry=status action=RUN"),
          "the published row is the row a click runs");
    CHECK(serial_contains("[LOCAL-MENU] run action=status\n"),
          "the clicked entry runs through the keyboard's own path");
}

/*
 * ITEM-470: the console passes the mode's own glyph scale to the pointer.  The
 * stub above reports 2x, so a report of three pixels must move the arrow six -
 * the exact behaviour the owner's 4K scale-3 screen lacked when the arrow moved
 * 1:1 and crawled relative to the interface.
 */
static void test_the_console_scales_the_pointer(void) {
    struct usb_hid_mouse_event event;
    int32_t pointer_x = 0;
    int32_t pointer_y = 0;

    begin_case();
    host_font_scale = 2U;
    console_init(512U, 1U);
    memset(&event, 0, sizeof(event));
    event.dx = 3;
    event.dy = -3;
    console_process_mouse_event(&event);
    CHECK(console_pointer_position(&pointer_x, &pointer_y) &&
              pointer_x == 512 + 6 && pointer_y == 384 - 6,
          "the console moves the arrow by the report scaled to the mode");
}

/*
 * ITEM-472 (СРОЧНО-22): the operator's gain and the once-a-second record.
 *
 * The owner tunes the pointer feel with `PTR <n>` instead of another code
 * round, and the journal then carries the applied deltas and the gain so the
 * next hardware run can tell a device that sends tiny deltas from one whose
 * reports are lost.  The record is due on the honest wall clock, so a second
 * report inside the same second does not republish and the next second does.
 */
static void test_the_operator_gain_and_the_pointer_window(void) {
    struct usb_hid_mouse_event event;
    int32_t pointer_x = 0;
    int32_t pointer_y = 0;

    begin_case();
    timer_real_seconds_value = 50U;
    host_font_scale = 1U;
    console_init(512U, 1U);
    reset_serial();
    type_line("ptr 10");
    CHECK(serial_contains("[POINTER] gain=10"),
          "PTR <n> sets and names the operator's gain");

    bind_host_journal();
    memset(&event, 0, sizeof(event));
    event.dx = 3;
    console_process_mouse_event(&event);
    CHECK(console_pointer_position(&pointer_x, &pointer_y) &&
              pointer_x == 512 + 30,
          "the gain multiplies the report over the glyph scale");
    CHECK(observed_text_contains("[POINTER] moves=") &&
              observed_text_contains(" dx=30") &&
              observed_text_contains(" dmax=30") &&
              observed_text_contains(" gain=10"),
          "the first movement publishes the applied delta, dmax and the gain");

    /* A second report inside the same real second waits for the next one. */
    memset(&event, 0, sizeof(event));
    event.dx = 1;
    console_process_mouse_event(&event);
    CHECK(!observed_text_contains("dx=31"),
          "a report inside the published second does not republish the window");

    timer_real_seconds_value = 51U;
    bind_host_journal();
    console_pointer_tick();
    CHECK(observed_text_contains(" dx=1") &&
              observed_text_contains(" moves=2"),
          "the next real second publishes only what landed in its window");
}

/*
 * ITEM-473 (СРОЧНО-23): the depth handle.
 *
 * The round-11 journal showed the pointer's queue frozen at `armed=16`, and the
 * worker's diagnosis named the sixteen-TRB event ring.  `ARM` lets the owner
 * check the remaining question in one hardware run - set the ITEM-469 depth of
 * three, or leave sixteen - and the answer names the event ring's capacity
 * beside the depth so the two facts sit on one line.  The value is what the
 * driver will arm, read back through `xhci_mouse_arm_depth()`.
 */
static void test_the_arm_depth_handle(void) {
    begin_case();
    console_arm_depth_stub = 16U;
    console_init(512U, 1U);
    reset_serial();
    type_line("arm");
    CHECK(serial_contains("[INPUT-ARM] mouse_arm_depth=16"),
          "ARM shows the mouse's arm depth");
    CHECK(serial_contains("event_ring=256"),
          "ARM names the event ring's capacity");

    reset_serial();
    type_line("arm 3");
    CHECK(serial_contains("[INPUT-ARM] mouse_arm_depth=3"),
          "ARM <n> sets and names the mouse's arm depth");
    CHECK(console_arm_depth_stub == 3U,
          "ARM sets the depth the driver will arm");

    reset_serial();
    type_line("arm 99");
    CHECK(serial_contains("mouse_arm_depth=16"),
          "ARM clamps a depth above the mouse maximum");
}

/*
 * ITEM-457: the console publishes what its own scrolling cost.
 *
 * The 2026-09-19 hardware report was that output reaching the bottom of the
 * screen crawled and the keyboard answered late while it happened.  The
 * measurement now lives on the serial stream, so a proof can read it:
 *
 *   [CONSOLE] scroll rows=<total> bytes=<total> us=<average> max_us=<worst>
 *
 * This case drives the console to the bottom of its 768-pixel screen and
 * requires the three things the contract asks for: a line that still fits
 * scrolls nothing and reports nothing, a line past the bottom reports exactly
 * one scroll with exactly the bytes the framebuffer returned, and the record
 * is atomic (an IRQ record cannot split the marker a reader waits for).  The
 * second half fills many screens and requires the report to stay a summary -
 * cumulative rows that grow, bounded in number, never one record per row.
 */
static unsigned int count_reports(const char *needle) {
    unsigned int count = 0U;
    const char *cursor = serial_capture;

    while ((cursor = strstr(cursor, needle)) != NULL) {
        ++count;
        ++cursor;
    }
    return count;
}

/* The `rows=` of the newest report, or 0 when there is none. */
static unsigned long long last_report_rows(void) {
    const char *found = NULL;
    const char *cursor = serial_capture;
    unsigned long long rows = 0U;

    while ((cursor = strstr(cursor, "[CONSOLE] scroll rows=")) != NULL) {
        found = cursor;
        ++cursor;
    }
    if (found == NULL ||
        sscanf(found, "[CONSOLE] scroll rows=%llu", &rows) != 1) {
        return 0U;
    }
    return rows;
}

static void test_console_scroll_report(void) {
    unsigned int index;
    unsigned int reports_after_fill;
    uint64_t fitting_rows;

    begin_case();
    framebuffer_scroll_calls = 0U;
    timer_ticks_value = 42U;
    timer_ticks_step = 0U;
    /* One cycle per read: every scroll of this case measures exactly 1 us. */
    timer_cycles_value = 1000U;
    timer_cycles_step = 1U;
    console_init(512U, 1U);
    reset_serial();
    /* Fill the body to its last row: the console knows how many lines still
       fit, so this does not depend on how long the banner was. */
    fitting_rows = console_test_rows_until_scroll();
    CHECK(fitting_rows > 0U, "the body has room for at least one more line");
    for (index = 0U; index < (unsigned int)fitting_rows; ++index) {
        console_write("FITS\n");
    }
    CHECK(framebuffer_scroll_calls == 0U,
          "a line that still fits does not scroll the screen");
    CHECK(!serial_contains("[CONSOLE] scroll "),
          "and a screen that did not scroll publishes no report");

    console_write("PAST THE BOTTOM\n");
    CHECK(framebuffer_scroll_calls != 0U,
          "the line past the bottom scrolls the body");
    CHECK(serial_contains_atomic(
              "[CONSOLE] scroll rows=1 bytes=2048 us=1 max_us=1\n"),
          "the scroll publishes its rows, bytes and time as one atomic "
          "record");

    /* Many screens: the reports stay a summary of the whole burst. */
    reset_serial();
    for (index = 0U; index < 96U; ++index) {
        console_write("SCREENFUL\n");
    }
    CHECK(framebuffer_scroll_calls > 96U,
          "the burst scrolled the body more than once per line");
    reports_after_fill = count_reports("[CONSOLE] scroll rows=");
    CHECK(reports_after_fill != 0U,
          "a long burst still publishes its cost");
    CHECK(reports_after_fill <= 8U,
          "the report is a summary, not one record per scrolled row");
    CHECK(last_report_rows() >= 16U,
          "the report cadence publishes progress through the burst");
}

/*
 * ITEM-474 (round 13): the turn's lines move the body once.
 *
 * The round-13 hardware log measured a scroll that moved the whole 4K body
 * (`bytes=31979520 us=7198`) per scrolled line and a loop that turned 2.4
 * times a second.  This case opens the per-turn batch, writes eight overflowing
 * lines, and requires that the framebuffer was *not* touched while they were
 * written and that one call - with all eight rows in a single report - moves
 * the body when the turn flushes.  The serial echo is written as each line is
 * produced, as before, so the proof also pins that batching changes the paint,
 * not the transcript.
 */
static void test_console_scroll_batch(void) {
    unsigned int index;
    uint64_t fitting_rows;
    unsigned int calls_before;
    size_t painted_after_first = 0U;

    begin_case();
    framebuffer_scroll_calls = 0U;
    timer_cycles_value = 1000U;
    timer_cycles_step = 1U;
    console_init(512U, 1U);
    reset_serial();
    console_begin_turn_batching();
    fitting_rows = console_test_rows_until_scroll();
    for (index = 0U; index < (unsigned int)fitting_rows; ++index) {
        console_write("FITS\n");
    }
    CHECK(framebuffer_scroll_calls == 0U,
          "a batch that has not overflowed still writes no framebuffer move");

    calls_before = framebuffer_scroll_calls;
    /*
     * The first line paints its characters on the last row and then opens the
     * batch with its newline; everything after that is deferred.
     */
    console_write("BATCH LINE\n");
    painted_after_first = screen_length;
    for (index = 1U; index < 8U; ++index) {
        console_write("BATCH LINE\n");
    }
    CHECK(framebuffer_scroll_calls == calls_before,
          "the turn's overflowing lines do not move the body one by one");
    CHECK(screen_length == painted_after_first,
          "no deferred character is painted before the flush");
    CHECK(serial_contains("BATCH LINE"),
          "the batch still serializes every line as it is produced");

    console_flush_deferred();
    CHECK(screen_length > painted_after_first,
          "the flush replays the deferred lines onto the screen");
    CHECK(framebuffer_scroll_calls == calls_before + 1U,
          "the whole turn's overflow moves the body exactly once");
    CHECK(serial_contains_atomic(
              "[CONSOLE] scroll rows=8 bytes=2048 us=0 max_us=1\n"),
          "the batch reports all eight rows as one atomic record");
    CHECK(console_test_scroll_rows() == 8U,
          "the batch moved exactly the eight rows it reported");

    /*
     * A deferred line longer than the screen wraps, and every wrap is a row
     * the one body move has to cover.  The logical column keeps advancing while
     * the paint is deferred, so the wraps are counted before the flush - the
     * bug this pins would leave the replay painting past the bottom row.
     */
    reset_serial();
    calls_before = framebuffer_scroll_calls;
    for (index = 0U; index < 300U; ++index) {
        console_write("W");
    }
    console_write("\n");
    console_flush_deferred();
    CHECK(console_test_scroll_rows() > 9U,
          "a wrapped deferred line counts every row in the batch");
    CHECK(framebuffer_scroll_calls == calls_before + 1U,
          "a wrapped deferred line still moves the body once");
}


/*
 * ITEM-509 increment 4: the console's `language` command.
 *
 * The classifier is a pure function, so the whole table of spellings is pinned
 * without a console; the command itself is then driven through the real console
 * with a stub owner, because the three rules the item asks for are about what
 * the operator sees and what the record keeps:
 *
 *   * `language` shows the active language, `language ru` writes once and
 *     confirms in the new language;
 *   * a value the catalog does not name is refused with the usage line and
 *     nothing is written;
 *   * a machine whose record did not load - or whose write failed - says the
 *     setting is unavailable and keeps answering in the language it had.
 */
static unsigned int language_write_calls;
static enum appliance_language language_write_argument;
static bool language_write_result;

static bool console_language_write_stub(void *context,
                                        enum appliance_language language) {
    (void)context;
    ++language_write_calls;
    language_write_argument = language;
    return language_write_result;
}

static void test_language_command(void) {
    static const struct {
        const char *line;
        enum console_language_command_kind kind;
        enum appliance_language language;
        bool has_value;
    } table[] = {
        {"language", CONSOLE_LANGUAGE_COMMAND_SHOW,
         APPLIANCE_LANGUAGE_DEFAULT, false},
        {"LANGUAGE", CONSOLE_LANGUAGE_COMMAND_SHOW,
         APPLIANCE_LANGUAGE_DEFAULT, false},
        {"language en", CONSOLE_LANGUAGE_COMMAND_SET,
         APPLIANCE_LANGUAGE_EN, true},
        {"language ru", CONSOLE_LANGUAGE_COMMAND_SET,
         APPLIANCE_LANGUAGE_RU, true},
        {"language de", CONSOLE_LANGUAGE_COMMAND_REFUSE,
         APPLIANCE_LANGUAGE_DEFAULT, true},
        {"language EN", CONSOLE_LANGUAGE_COMMAND_REFUSE,
         APPLIANCE_LANGUAGE_DEFAULT, true},
        {"language  en", CONSOLE_LANGUAGE_COMMAND_REFUSE,
         APPLIANCE_LANGUAGE_DEFAULT, true},
        {"language en extra", CONSOLE_LANGUAGE_COMMAND_REFUSE,
         APPLIANCE_LANGUAGE_DEFAULT, true},
        {"language ", CONSOLE_LANGUAGE_COMMAND_REFUSE,
         APPLIANCE_LANGUAGE_DEFAULT, true},
        {"languages", CONSOLE_LANGUAGE_COMMAND_NONE,
         APPLIANCE_LANGUAGE_DEFAULT, false},
        {"lang", CONSOLE_LANGUAGE_COMMAND_NONE,
         APPLIANCE_LANGUAGE_DEFAULT, false},
        {"language1", CONSOLE_LANGUAGE_COMMAND_NONE,
         APPLIANCE_LANGUAGE_DEFAULT, false},
    };
    const struct console_language_binding binding = {
        NULL,
        console_language_write_stub
    };
    const struct console_boot_log_binding log_binding = {
        console_host_observe,
        console_host_dump
    };
    unsigned int index;

    for (index = 0U; index < sizeof(table) / sizeof(table[0]); ++index) {
        enum appliance_language language = APPLIANCE_LANGUAGE_DEFAULT;
        const char *value = NULL;
        size_t value_length = 0U;
        size_t length = strlen(table[index].line);
        enum console_language_command_kind kind =
            console_language_command_classify(table[index].line, length,
                                              &language, &value,
                                              &value_length);

        CHECK(kind == table[index].kind, "the language classifier agrees");
        CHECK(language == table[index].language,
              "the classifier names the language it read");
        CHECK((value != NULL) == table[index].has_value,
              "the classifier reports whether a value was typed");
    }

    /* `language` shows what the console is set to. */
    begin_case();
    console_init(512U, 1U);
    console_publish_language(APPLIANCE_LANGUAGE_EN, true);
    console_bind_language(&binding);
    reset_serial();
    reset_screen();
    submit_command("language");
    CHECK(screen_contains("LANGUAGE: EN"),
          "the language command shows the active language");

    /* A successful switch writes once, confirms in the new language and is
       recorded. */
    begin_case();
    console_init(512U, 1U);
    console_publish_language(APPLIANCE_LANGUAGE_EN, true);
    console_bind_language(&binding);
    observed_count = 0U;
    language_write_calls = 0U;
    language_write_argument = APPLIANCE_LANGUAGE_DEFAULT;
    language_write_result = true;
    console_bind_boot_log(&log_binding);
    reset_serial();
    reset_screen();
    submit_command("language ru");
    CHECK(language_write_calls == 1U,
          "a language switch writes the record exactly once");
    CHECK(language_write_argument == APPLIANCE_LANGUAGE_RU,
          "the record is written with the language that was typed");
    CHECK(console_active_language() == APPLIANCE_LANGUAGE_RU,
          "a written language becomes the console's own");
    CHECK(screen_contains("ЯЗЫК СОХРАНЁН"),
          "the confirmation is printed in the language just chosen");
    CHECK(observed_contains("[CONSOLE] language=ru"),
          "the switch leaves one record naming the language");

    /* The answer follows the setting: showing it again is Russian now. */
    reset_screen();
    submit_command("language");
    CHECK(screen_contains("ЯЗЫК: RU"),
          "the show answer follows the language that was chosen");

    /*
     * ITEM-509 increment 22: the console's field labels come from the same
     * catalog, so a Russian console names its fields in Russian while the
     * values beside them (numbers, ON/OFF, MIB) stay English machine text.
     */
    reset_screen();
    submit_command("uptime");
    CHECK(screen_contains("ВРЕМЯ РАБОТЫ: "),
          "the uptime label speaks the chosen language");
    CHECK(screen_contains(" SECONDS"),
          "the value beside the translated label stays English");
    CHECK(!screen_contains("UPTIME: "),
          "the English label is gone from a Russian console");
    reset_screen();
    submit_command("mem");
    CHECK(screen_contains("ДОСТУПНАЯ ПАМЯТЬ: ") && screen_contains(" MIB"),
          "the memory label speaks the chosen language");
    reset_screen();
    submit_command("nosuchthing");
    CHECK(screen_contains("НЕИЗВЕСТНАЯ КОМАНДА: nosuchthing"),
          "the unknown-command label speaks the chosen language");
    CHECK(!screen_contains("UNKNOWN COMMAND: "),
          "the English unknown-command label is gone too");
    /*
     * ITEM-509 increment 23: the sentence after the echoed name is prose too,
     * and so are the two range suffixes of PTR and ARM; the records beside them
     * (`[POINTER] gain=`, `[INPUT-ARM] ...`) stay English machine text.
     */
    CHECK(screen_contains(". НАБЕРИТЕ HELP."),
          "the unknown-command hint speaks the chosen language");
    CHECK(!screen_contains(". TYPE HELP."),
          "the English unknown-command hint is gone from a Russian console");
    reset_screen();
    submit_command("ptr");
    CHECK(serial_contains("[POINTER] gain=") &&
              serial_contains("> задаёт его)"),
          "the pointer range suffix is translated and the record stays English");
    reset_screen();
    submit_command("arm");
    CHECK(serial_contains("[INPUT-ARM] mouse_arm_depth=") &&
              serial_contains("> задаёт глубину мыши)"),
          "the arm range suffix is translated and the record stays English");

    /*
     * ITEM-509 increment 23: the honest UNAVAILABLE answer is operator prose
     * and follows the chosen language; the journal record stays English.
     */
    begin_case();
    console_init(512U, 1U);
    console_publish_language(APPLIANCE_LANGUAGE_RU, true);
    console_bind_language(&binding);
    console_bind_boot_log(NULL);
    reset_serial();
    reset_screen();
    submit_command("bootlog");
    CHECK(screen_contains("ЖУРНАЛ ЗАГРУЗКИ: НЕДОСТУПЕН"),
          "the unavailable boot log answers in the chosen language");
    CHECK(!screen_contains("BOOTLOG: UNAVAILABLE"),
          "the English unavailable answer is gone from a Russian console");

    /*
     * ITEM-509 increment 24: two sentences the host build cannot reach through
     * the keyboard - the power refusal behind the platform-power build guard
     * and the already-open account conversation - follow the chosen language;
     * the label, the action name and the `[ADMIN-ENROLLMENT]` marker stay
     * English machine text.
     */
    begin_case();
    console_init(512U, 1U);
    console_publish_language(APPLIANCE_LANGUAGE_RU, true);
    console_bind_language(&binding);
    reset_serial();
    console_test_power_refusal("REBOOT");
    CHECK(serial_contains("ПИТАНИЕ: REBOOT ОТКАЗ: эта платформа не объявляет "
                          "механизм"),
          "the power refusal speaks the chosen language");
    CHECK(!serial_contains("REFUSED: this platform declares no mechanism"),
          "the English power refusal is gone from a Russian console");

    begin_case();
    console_init(512U, 1U);
    console_publish_language(APPLIANCE_LANGUAGE_EN, true);
    console_bind_language(&binding);
    reset_serial();
    console_test_power_refusal("SHUTDOWN");
    CHECK(serial_contains(
              "POWER: SHUTDOWN REFUSED: this platform declares no mechanism"),
          "the English power refusal is byte for byte the old line");

    {
        struct credentials_fixture fixture;

        begin_case();
        memset(&fixture, 0, sizeof(fixture));
        bind_credentials_fixture(&fixture);
        console_init(512U, 1U);
        console_publish_language(APPLIANCE_LANGUAGE_RU, true);
        console_bind_language(&binding);
        reset_serial();
        submit_command("credentials");
        CHECK(console_admin_credentials_input_active(),
              "the account conversation is open before the guard is exercised");
        reset_serial();
        console_test_admin_credentials_reenter();
        CHECK(serial_contains(
                  "[ADMIN-ENROLLMENT] разговор об учётной записи уже открыт"),
              "the busy account conversation speaks the chosen language");
        CHECK(!serial_contains("a credentials conversation is already open"),
              "the English busy sentence is gone from a Russian console");
        console_bind_admin_credentials(NULL);
    }

    /*
     * ITEM-509 increment 23: the service lines are journal records, so they
     * stay English byte for byte even when the operator prose is Russian.
     */
    begin_case();
    console_publish_input_source(LOCAL_INPUT_SOURCE_PS2);
    console_publish_local_input(true);
    console_publish_language(APPLIANCE_LANGUAGE_RU, true);
    console_bind_language(&binding);
    reset_serial();
    console_init(512U, 1U);
    CHECK(serial_contains("GDT IDT LEGACY PIC PIT PS2: OK\n"),
          "the service banner stays English in a Russian console");
    CHECK(serial_contains(
              "TYPE HELP FOR COMMANDS, MENU FOR THE LOCAL MENU\n"),
          "the service invitation stays English in a Russian console");

    /* A value the catalog does not name is refused, and nothing is written. */
    begin_case();
    console_init(512U, 1U);
    console_publish_language(APPLIANCE_LANGUAGE_EN, true);
    console_bind_language(&binding);
    observed_count = 0U;
    language_write_calls = 0U;
    console_bind_boot_log(&log_binding);
    reset_serial();
    reset_screen();
    submit_command("language de");
    CHECK(language_write_calls == 0U,
          "a refused language writes nothing");
    CHECK(console_active_language() == APPLIANCE_LANGUAGE_EN,
          "a refused language leaves the console where it was");
    CHECK(screen_contains("UNKNOWN LANGUAGE: use en or ru"),
          "the refusal names the rule instead of falling back to English");
    CHECK(screen_contains("USAGE: language en|ru"),
          "the refusal carries the usage line");
    CHECK(observed_contains(
              "[CONSOLE] language=de refused reason=UNKNOWN_LANGUAGE"),
          "the refusal is recorded with the value that was typed");

    /* A machine without a usable record says so. */
    begin_case();
    console_init(512U, 1U);
    console_publish_language(APPLIANCE_LANGUAGE_EN, false);
    console_bind_language(NULL);
    observed_count = 0U;
    language_write_calls = 0U;
    console_bind_boot_log(&log_binding);
    reset_serial();
    reset_screen();
    submit_command("language ru");
    CHECK(language_write_calls == 0U,
          "an unbound console never calls a write");
    CHECK(console_active_language() == APPLIANCE_LANGUAGE_EN,
          "an unavailable setting leaves the language alone");
    CHECK(screen_contains("LANGUAGE IS NOT AVAILABLE"),
          "the console says the setting is unavailable instead of pretending");
    CHECK(observed_contains(
              "[CONSOLE] language=ru refused reason=UNAVAILABLE"),
          "the unavailable refusal is recorded");

    /* A write the owner refuses is the same answer, with its own reason. */
    begin_case();
    console_init(512U, 1U);
    console_publish_language(APPLIANCE_LANGUAGE_EN, true);
    console_bind_language(&binding);
    observed_count = 0U;
    language_write_calls = 0U;
    language_write_result = false;
    console_bind_boot_log(&log_binding);
    reset_serial();
    reset_screen();
    submit_command("language ru");
    CHECK(language_write_calls == 1U,
          "a failed switch still tried the record once");
    CHECK(console_active_language() == APPLIANCE_LANGUAGE_EN,
          "a failed write does not move the console's language");
    CHECK(screen_contains("LANGUAGE IS NOT AVAILABLE"),
          "a failed write is reported, not hidden");
    CHECK(observed_contains(
              "[CONSOLE] language=ru refused reason=WRITE_FAILED"),
          "the failed write is recorded with its reason");
    console_bind_boot_log(NULL);
    console_bind_language(NULL);
}

/*
 * ITEM-516: the console's `gate` command.
 *
 * The classifier is pure, so every spelling is pinned without a console.  The
 * command is then driven through the real console:
 *
 *   * without a proven local secret, `gate open` refuses by name and writes
 *     nothing - the dangerous direction is local-only and proof-bound;
 *   * after a successful `login`, `gate open` prints the full warning and
 *     arms the confirmation without writing;
 *   * `gate open confirm` on the next line writes `open` exactly once;
 *   * `gate close` always writes the default closed.
 */
static unsigned int gate_read_calls;
static unsigned int gate_write_calls;
static bool gate_write_argument;
static bool gate_write_result;

static bool console_gate_read_stub(void *context, bool *open_out) {
    (void)context;
    ++gate_read_calls;
    if (open_out != NULL) {
        *open_out = console_gate_is_open();
    }
    return true;
}

static bool console_gate_write_stub(void *context, bool open) {
    (void)context;
    ++gate_write_calls;
    gate_write_argument = open;
    return gate_write_result;
}

static void test_gate_command_classify(void) {
    static const struct {
        const char *line;
        enum console_gate_command_kind kind;
        bool armed_after;
    } table[] = {
        {"gate", CONSOLE_GATE_COMMAND_SHOW, false},
        {"GATE", CONSOLE_GATE_COMMAND_SHOW, false},
        {"gate open", CONSOLE_GATE_COMMAND_OPEN_REQUEST, true},
        {"GATE OPEN", CONSOLE_GATE_COMMAND_OPEN_REQUEST, true},
        {"gate close", CONSOLE_GATE_COMMAND_CLOSE, false},
        {"gate open confirm", CONSOLE_GATE_COMMAND_OPEN_UNCONFIRMED, false},
        {"gate opne", CONSOLE_GATE_COMMAND_REFUSE, false},
        {"gate open extra", CONSOLE_GATE_COMMAND_REFUSE, false},
        {"gate ", CONSOLE_GATE_COMMAND_REFUSE, false},
        {"gateway", CONSOLE_GATE_COMMAND_NONE, false},
        {"gates", CONSOLE_GATE_COMMAND_NONE, false},
    };
    unsigned int index;

    for (index = 0U; index < sizeof(table) / sizeof(table[0]); ++index) {
        bool armed = false;
        const size_t length = strlen(table[index].line);
        const enum console_gate_command_kind kind =
            console_gate_command_classify(table[index].line, length, &armed);

        CHECK(kind == table[index].kind, "the gate classifier agrees");
        CHECK(armed == table[index].armed_after,
              "the classifier reports the armed confirmation");
    }
    /* The confirmation only confirms when the request was armed. */
    {
        bool armed = false;
        const char *request = "gate open";
        const char *confirm = "gate open confirm";
        enum console_gate_command_kind kind = console_gate_command_classify(
            request, strlen(request), &armed);

        CHECK(kind == CONSOLE_GATE_COMMAND_OPEN_REQUEST && armed,
              "the request arms the confirmation");
        kind = console_gate_command_classify(confirm, strlen(confirm), &armed);
        CHECK(kind == CONSOLE_GATE_COMMAND_OPEN_CONFIRM && !armed,
              "the armed confirmation is classified as the confirmation");
        kind = console_gate_command_classify(confirm, strlen(confirm), &armed);
        CHECK(kind == CONSOLE_GATE_COMMAND_OPEN_UNCONFIRMED,
              "a second confirmation is unconfirmed");
    }
}

static void test_gate_command(void) {
    static const char login[] = "operator";
    static const char secret[] = "correct horse battery staple";
    const struct console_gate_binding binding = {
        NULL,
        console_gate_read_stub,
        console_gate_write_stub
    };
    const struct console_boot_log_binding log_binding = {
        console_host_observe,
        console_host_dump
    };
    struct credentials_fixture fixture;

    /* The state answers without any binding at all. */
    begin_case();
    console_init(512U, 1U);
    console_bind_boot_log(&log_binding);
    reset_serial();
    reset_screen();
    submit_command("gate");
    CHECK(screen_contains("MANAGEMENT GATE: CLOSED AFTER BOOT"),
          "gate shows the setting in force");
    CHECK(screen_contains("the panel waits for a local login on this machine"),
          "gate always names what the setting means");

    /* Without a proven local secret, opening is refused by name. */
    begin_case();
    console_init(512U, 1U);
    console_publish_gate(false, true);
    console_bind_gate(&binding);
    observed_count = 0U;
    gate_write_calls = 0U;
    console_bind_boot_log(&log_binding);
    reset_serial();
    reset_screen();
    submit_command("gate open");
    CHECK(gate_write_calls == 0U,
          "gate open without a local proof writes nothing");
    CHECK(!console_gate_local_secret_proven(),
          "gate open without a login proves no secret");
    CHECK(serial_contains(
              "REFUSED: run login first; opening the gate requires the "
              "administrator secret proven on this machine"),
          "the refusal names the local secret requirement");
    CHECK(observed_contains(
              "[PRODUCTION-SECURITY] gate=REFUSED "
              "reason=GATE_LOCAL_SECRET_REQUIRED"),
          "the refusal is recorded by name");
    reset_screen();
    submit_command("gate open confirm");
    CHECK(gate_write_calls == 0U,
          "an unconfirmed confirmation writes nothing either");

    /* A successful local login unlocks the dangerous direction. */
    begin_case();
    memset(&fixture, 0, sizeof(fixture));
    fixture.authenticate_ok = true;
    bind_credentials_fixture(&fixture);
    console_init(512U, 1U);
    console_publish_gate(false, true);
    console_bind_gate(&binding);
    observed_count = 0U;
    gate_write_calls = 0U;
    gate_write_result = true;
    console_bind_boot_log(&log_binding);
    reset_serial();
    reset_screen();
    submit_command("login");
    submit_command(login);
    submit_command(secret);
    CHECK(console_gate_local_secret_proven(),
          "a successful local login proves the secret");
    reset_screen();
    submit_command("gate open");
    CHECK(gate_write_calls == 0U,
          "the request still writes nothing");
    CHECK(console_gate_is_open() == false,
          "the request does not move the setting");
    CHECK(screen_contains("WARNING: you are about to let this machine answer "
                          "the panel over the network right after boot."),
          "the warning opens with what is about to happen");
    CHECK(screen_contains("The machine will NO LONGER REQUIRE PHYSICAL "
                          "PRESENCE"),
          "the warning names the loss of physical presence");
    CHECK(screen_contains("protected only by the administrator password"),
          "the warning names the password as the first protection");
    CHECK(screen_contains("After five failed sign-ins the account is locked"),
          "the warning names the failure lockout");
    CHECK(screen_contains("Audit finding R01"),
          "the warning names audit finding R01");
    CHECK(screen_contains("listens on a wildcard address"),
          "the warning says the service listens on a wildcard address");
    CHECK(serial_contains("open to the whole network"),
          "the warning says what that means on an Internet-exposed machine");
    CHECK(screen_contains("Type gate open confirm on the next line"),
          "the warning asks for the explicit confirmation");
    CHECK(observed_contains(
              "[PRODUCTION-SECURITY] gate=PENDING action=open "
              "confirmation_required"),
          "the armed request is recorded as pending");

    /* Any other command disarms it; the confirmation must follow directly. */
    reset_screen();
    submit_command("uptime");
    reset_serial();
    submit_command("gate open confirm");
    CHECK(gate_write_calls == 0U,
          "a confirmation after another command writes nothing");
    CHECK(screen_contains("REFUSED: no gate open command was requested"),
          "the unconfirmed confirmation says why");

    /* The confirmation on the next line writes open exactly once. */
    reset_serial();
    observed_count = 0U;
    submit_command("gate open");
    submit_command("gate open confirm");
    CHECK(gate_write_calls == 1U,
          "the confirmation writes the record exactly once");
    CHECK(gate_write_argument == true,
          "the confirmation writes open");
    CHECK(console_gate_is_open(),
          "the setting moves only after the write said yes");
    CHECK(observed_contains(
              "[PRODUCTION-SECURITY] gate=ENABLED source=LOCAL_SECRET"),
          "the enabled setting is recorded with its source");
    CHECK(serial_contains_atomic(
              "[PRODUCTION-SECURITY] gate=ENABLED source=LOCAL_SECRET"),
          "the enabled record is one IRQ-safe write, not a splittable line");
    CHECK(screen_contains(
              "SAVED: the panel will answer after every boot without a local "
              "login"),
          "the console confirms what was saved");

    /* The safe direction is always available. */
    gate_write_calls = 0U;
    gate_write_argument = true;
    reset_serial();
    submit_command("gate close");
    CHECK(gate_write_calls == 1U && gate_write_argument == false,
          "gate close writes the default closed without any proof");
    CHECK(!console_gate_is_open(),
          "closing moves the setting back");
    CHECK(observed_contains(
              "[PRODUCTION-SECURITY] gate=DISABLED source=CONSOLE"),
          "the closed setting is recorded with its source");
    CHECK(screen_contains("SAVED: the panel waits for a local login again"),
          "the console confirms the safe direction");

    /* A machine whose record did not load refuses both directions. */
    begin_case();
    console_init(512U, 1U);
    console_publish_gate(false, false);
    console_bind_gate(NULL);
    observed_count = 0U;
    gate_write_calls = 0U;
    console_bind_boot_log(&log_binding);
    reset_serial();
    reset_screen();
    submit_command("gate close");
    CHECK(gate_write_calls == 0U,
          "an unbound console never calls a write");
    CHECK(screen_contains("MANAGEMENT GATE IS NOT AVAILABLE"),
          "the console says the setting is unavailable instead of pretending");
    CHECK(observed_contains(
              "[PRODUCTION-SECURITY] gate=REFUSED "
              "reason=GATE_LOCAL_SECRET_REQUIRED"),
          "the unavailable answer is still a refusal record");

    /* A write the owner refuses is reported and recorded. */
    begin_case();
    memset(&fixture, 0, sizeof(fixture));
    fixture.authenticate_ok = true;
    bind_credentials_fixture(&fixture);
    console_init(512U, 1U);
    console_publish_gate(false, true);
    console_bind_gate(&binding);
    observed_count = 0U;
    gate_write_calls = 0U;
    gate_write_result = false;
    console_bind_boot_log(&log_binding);
    reset_serial();
    reset_screen();
    submit_command("gate close");
    CHECK(gate_write_calls == 1U,
          "a failed write still tried the record once");
    CHECK(screen_contains("MANAGEMENT GATE WAS NOT SAVED"),
          "a failed write is reported, not hidden");
    console_bind_boot_log(NULL);
    console_bind_gate(NULL);
    console_bind_admin_credentials(NULL);
}

/*
 * ITEM-509 increment 7: the console's help.
 *
 * The classifier is pure, so every spelling is pinned without a console.  The
 * content is then measured against the renderer's line bound, and the real
 * console is driven four ways: `help` prints the table of contents, `help
 * <topic>` prints exactly that topic, `help all` prints every topic, and an id
 * no topic carries gets the named refusal.  The Russian render goes through
 * the same console the language command switches, and the glyph record has to
 * stay absent: the help bodies are covered by the glyph proof like any other
 * Russian sentence.
 */
static void test_help_command(void) {
    static const struct {
        const char *line;
        enum console_help_command_kind kind;
        const char *topic;
    } table[] = {
        {"help", CONSOLE_HELP_COMMAND_LIST, NULL},
        {"HELP", CONSOLE_HELP_COMMAND_LIST, NULL},
        {"help all", CONSOLE_HELP_COMMAND_ALL, NULL},
        {"help ALL", CONSOLE_HELP_COMMAND_ALL, NULL},
        {"help install", CONSOLE_HELP_COMMAND_TOPIC, "install"},
        {"help INSTALL", CONSOLE_HELP_COMMAND_TOPIC, "install"},
        {"help records", CONSOLE_HELP_COMMAND_TOPIC, "records"},
        {"help unknown", CONSOLE_HELP_COMMAND_REFUSE, NULL},
        {"help ", CONSOLE_HELP_COMMAND_REFUSE, NULL},
        {"help  install", CONSOLE_HELP_COMMAND_REFUSE, NULL},
        {"help install extra", CONSOLE_HELP_COMMAND_REFUSE, NULL},
        {"helps", CONSOLE_HELP_COMMAND_NONE, NULL},
        {"help1", CONSOLE_HELP_COMMAND_NONE, NULL},
    };
    const struct console_boot_log_binding log_binding = {
        console_host_observe,
        console_host_dump
    };
    const size_t topic_count = appliance_help_topic_count();
    const size_t install =
        appliance_help_topic_find("install", strlen("install"));
    const size_t account =
        appliance_help_topic_find("account", strlen("account"));
    size_t index;
    size_t field;

    for (index = 0U; index < sizeof(table) / sizeof(table[0]); ++index) {
        size_t found = (size_t)-1;
        const size_t length = strlen(table[index].line);
        const enum console_help_command_kind kind =
            console_help_command_classify(table[index].line, length, &found);

        CHECK(kind == table[index].kind, "the help classifier agrees");
        if (table[index].topic == NULL) {
            CHECK(found == (size_t)-1,
                  "a line that names no topic leaves the index alone");
        } else {
            CHECK(found != (size_t)-1 &&
                      strcmp(appliance_help_topic_id(found),
                             table[index].topic) == 0,
                  "the classifier names the topic it read");
        }
    }
    CHECK(install != (size_t)-1, "the install topic exists");
    CHECK(account != (size_t)-1, "the account topic exists");

    /*
     * The renderer's bound is a property of the authored content: no line of
     * any topic is longer than one rendered line, in either language, so the
     * renderer never has to break a sentence and never cuts one.
     */
    for (index = 0U; index < topic_count; ++index) {
        for (field = 0U; field < (size_t)APPLIANCE_HELP_FIELD_COUNT; ++field) {
            enum appliance_language language;

            for (language = APPLIANCE_LANGUAGE_EN;
                 language < APPLIANCE_LANGUAGE_COUNT; ++language) {
                const char *text = appliance_help_topic_string(
                    index, (enum appliance_help_field)field, language);
                size_t line_start = 0U;
                size_t offset = 0U;

                CHECK(text != NULL, "every help string exists");
                if (text == NULL) {
                    continue;
                }
                for (offset = 0U;; ++offset) {
                    if (text[offset] == '\n' || text[offset] == '\0') {
                        CHECK(offset - line_start < CONSOLE_HELP_LINE_MAX,
                              "an authored help line fits the renderer");
                        line_start = offset + 1U;
                        if (text[offset] == '\0') {
                            break;
                        }
                    }
                }
            }
        }
    }

    /* `help`: the table of contents, every topic with its summary, the usage
       and the vocabulary line every operator and proof already reads. */
    begin_case();
    console_init(512U, 1U);
    console_publish_language(APPLIANCE_LANGUAGE_EN, true);
    reset_serial();
    reset_screen();
    submit_command("help");
    CHECK(screen_contains("Help"), "the table of contents is titled");
    CHECK(screen_contains("TOPICS"), "the table of contents names its list");
    CHECK(serial_contains("TYPE help <topic>, help all"),
          "the table of contents says how to ask for a topic");
    CHECK(serial_contains("COMMANDS: HELP CLEAR SCROLL VERSION"),
          "the command vocabulary line stays on the answer");
    for (index = 0U; index < topic_count; ++index) {
        char line[CONSOLE_HELP_LINE_MAX];
        const char *id = appliance_help_topic_id(index);
        const char *summary = appliance_help_topic_string(
            index, APPLIANCE_HELP_FIELD_SUMMARY, APPLIANCE_LANGUAGE_EN);
        int built;

        CHECK(id != NULL && summary != NULL,
              "the topic has a table-of-contents line");
        if (id == NULL || summary == NULL) {
            continue;
        }
        built = snprintf(line, sizeof(line), "%s: %s", id, summary);
        CHECK(built > 0 && (size_t)built < sizeof(line),
              "the table-of-contents line is built whole");
        CHECK(screen_contains(line),
              "the table of contents lists the topic");
        CHECK(serial_contains_atomic(line),
              "the table-of-contents line is one atomic write");
    }

    /* `help <topic>`: exactly that topic, in full, and no other. */
    begin_case();
    console_init(512U, 1U);
    console_publish_language(APPLIANCE_LANGUAGE_EN, true);
    reset_serial();
    reset_screen();
    submit_command("help install");
    for (field = 0U; field < (size_t)APPLIANCE_HELP_FIELD_COUNT; ++field) {
        const char *text = appliance_help_topic_string(
            install, (enum appliance_help_field)field, APPLIANCE_LANGUAGE_EN);

        CHECK(text != NULL && serial_contains(text),
              "one topic prints its whole title, summary, body and related");
    }
    CHECK(!serial_contains("NO SUCH HELP TOPIC"),
          "a known topic is not refused");
    CHECK(!serial_contains(appliance_help_topic_string(
              account, APPLIANCE_HELP_FIELD_TITLE, APPLIANCE_LANGUAGE_EN)),
          "one topic does not print another topic");

    /* `help all`: every topic, in the same order. */
    begin_case();
    console_init(512U, 1U);
    console_publish_language(APPLIANCE_LANGUAGE_EN, true);
    reset_serial();
    reset_screen();
    submit_command("help all");
    for (index = 0U; index < topic_count; ++index) {
        const char *title = appliance_help_topic_string(
            index, APPLIANCE_HELP_FIELD_TITLE, APPLIANCE_LANGUAGE_EN);

        CHECK(title != NULL && serial_contains(title),
              "help all prints every topic, not only the first");
    }

    /* An id no topic carries: the named refusal, and no topic body. */
    begin_case();
    console_init(512U, 1U);
    console_publish_language(APPLIANCE_LANGUAGE_EN, true);
    reset_serial();
    reset_screen();
    submit_command("help nonsense");
    CHECK(screen_contains("NO SUCH HELP TOPIC"),
          "an unknown topic is refused by name");
    CHECK(screen_contains("TYPE help <topic>, help all"),
          "the refusal repeats the usage");
    CHECK(!serial_contains(appliance_help_topic_string(
              install, APPLIANCE_HELP_FIELD_TITLE, APPLIANCE_LANGUAGE_EN)),
          "a refusal renders no topic");

    /* Russian: the renderer follows the language the console is set to, and
       every glyph of the help bodies is covered. */
    begin_case();
    console_init(512U, 1U);
    console_publish_language(APPLIANCE_LANGUAGE_RU, true);
    observed_count = 0U;
    console_bind_boot_log(&log_binding);
    reset_serial();
    reset_screen();
    submit_command("help account");
    for (field = 0U; field < (size_t)APPLIANCE_HELP_FIELD_COUNT; ++field) {
        const char *text = appliance_help_topic_string(
            account, (enum appliance_help_field)field, APPLIANCE_LANGUAGE_RU);

        CHECK(text != NULL && serial_contains(text),
              "the Russian topic is printed in Russian");
    }
    CHECK(!observed_contains("[GLYPH-MISSING]"),
          "no help glyph was missing");
    reset_screen();
    submit_command("help");
    CHECK(screen_contains("Справка") && screen_contains("ТЕМЫ"),
          "the table of contents follows the language as well");
    console_bind_boot_log(NULL);
}

/*
 * ITEM-509 increment 3: the console paints Russian.
 *
 * Until this increment every byte of a Russian string reached the renderer
 * separately: a two-byte code point was two cells, and both cells were empty
 * because the font had no Cyrillic glyphs - the text vanished.  The proof drives
 * the real console: one cell per code point (not per byte), the bytes on the
 * serial transcript unchanged, a Cyrillic line wrapping exactly as a Latin line
 * of the same code-point count, and a code point with no glyph drawn as a
 * visible marker and published as a record that names it.
 */
static void test_cyrillic_output_is_code_point_based(void) {
    static const char hello[] = "ПРИВЕТ";
    static const char zhe[] = "Ж";
    static const char unknown[] = "\xE4\xB8\x80"; /* U+4E00, no glyph */
    static const char truncated[] = "\xD0";       /* half of a two-byte letter */
    const struct console_boot_log_binding binding = {
        console_host_observe,
        console_host_dump
    };
    uint64_t ascii_rows;
    uint64_t cyrillic_rows;
    unsigned int index;

    begin_case();
    console_init(512U, 1U);
    reset_serial();
    reset_screen();
    console_write(hello);
    CHECK(screen_codepoint_count == 6U,
          "six Russian letters are six cells, not twelve");
    CHECK(screen_contains(hello),
          "the screen carries the Russian text the console was given");
    CHECK(serial_contains(hello),
          "the serial transcript keeps the same bytes it always did");

    /*
     * Wrapping is counted in code points: a line of 1000 Cyrillic letters must
     * reach exactly as many rows as a line of 1000 Latin letters, even though
     * it is twice as many bytes on the wire.
     */
    begin_case();
    console_init(512U, 1U);
    reset_serial();
    reset_screen();
    for (index = 0U; index < 1000U; ++index) {
        console_write("W");
    }
    console_flush_deferred();
    ascii_rows = (uint64_t)screen_row_runs;
    /* The same console state for the Russian line, so the two are comparable. */
    console_init(512U, 1U);
    reset_serial();
    reset_screen();
    for (index = 0U; index < 1000U; ++index) {
        console_write(zhe);
    }
    console_flush_deferred();
    cyrillic_rows = (uint64_t)screen_row_runs;
    CHECK(ascii_rows > 1U,
          "the Latin control line is long enough to wrap");
    CHECK(ascii_rows == cyrillic_rows,
          "a Cyrillic line wraps by code points, not by bytes");

    /* A code point the font does not carry is visible and named. */
    begin_case();
    console_init(512U, 1U);
    reset_serial();
    reset_screen();
    observed_count = 0U;
    dump_calls = 0U;
    stub_glyph_missing_count = 0U;
    stub_glyph_missing_last = 0U;
    console_bind_boot_log(&binding);
    console_write(unknown);
    console_flush_deferred();
    CHECK(stub_glyph_missing_count == 1U,
          "a code point with no glyph is counted, not dropped");
    CHECK(stub_glyph_missing_last == 0x4E00U,
          "the counter names the code point that had no glyph");
    CHECK(screen_contains("?"),
          "the cell carries a visible marker instead of silence");
    CHECK(serial_contains(unknown),
          "the serial transcript keeps the bytes of the refused code point");
    CHECK(observed_contains(
              "[GLYPH-MISSING] codepoint=U+4E00 count=1"),
          "the journal carries one record naming the code point");

    /* A truncated sequence is refused the same way, and says which byte. */
    reset_screen();
    observed_count = 0U;
    console_write(truncated);
    console_flush_deferred();
    CHECK(stub_glyph_missing_count == 2U,
          "a truncated sequence is a missing glyph, not a silent byte");
    CHECK(screen_contains("?"),
          "the truncated sequence leaves a visible marker");
    CHECK(observed_contains(
              "[GLYPH-MISSING] codepoint=U+FFFD byte=0xD0 count=2"),
          "the record names the byte that broke the sequence");
    CHECK(serial_contains(truncated),
          "the truncated byte still reaches the serial transcript");
    console_bind_boot_log(NULL);
}

/*
 * ITEM-482 (round 20): typing at the end of the line is one painted cell.
 *
 * The owner's keyboard lag came from `redraw_input_line()` on every character:
 * it writes `"\r"`, the prompt and the whole line, and each of those writes
 * drains the framebuffer, so the echo cost grew with the line's length.  The
 * fast path appends the character and hands exactly that cell to the painter,
 * so no drain is charged while the text stays put.  A backspace at the end is
 * the same one cell; the characters still reach the serial transcript in order.
 */
static void test_typing_appends_without_a_full_redraw(void) {
    unsigned int syncs_before;

    begin_case();
    host_font_scale = 1U;
    console_init(512U, 1U);
    send_char('a');
    send_char('b');
    send_char('c');
    send_event(KEYBOARD_EVENT_BACKSPACE, 0);
    syncs_before = framebuffer_sync_calls;
    send_char('d');
    CHECK(framebuffer_sync_calls == syncs_before,
          "appending a character paints one cell, not a whole-line redraw");
    syncs_before = framebuffer_sync_calls;
    send_event(KEYBOARD_EVENT_BACKSPACE, 0);
    CHECK(framebuffer_sync_calls == syncs_before,
          "erasing the last character paints one cell, not a whole-line redraw");
    CHECK(serial_contains("ab"),
          "the fast path still serializes the typed characters in order");
}

/*
 * ITEM-483 (round 20 follow-up): the fast path paints one cell only while the
 * prompt and the typed line still stand where it left them on both surfaces the
 * operator reads.  A mirrored record is another writer: `console_record_paint()`
 * re-emits the prompt and the line on the framebuffer only, so the serial
 * stream has no prompt after it.  A journal-only record (`console_journal_*`)
 * never paints at all, yet it still pushes the typed characters apart on the
 * wire - which is exactly how the 2026-09-26 load proof lost `e`/`f` of
 * `abcdef`.  Either kind of record between two keystrokes must make the next
 * keystroke do the full `redraw_input_line()`, which re-emits `hfoc> ` and the
 * whole line to both surfaces; the fast path must take over again after it.
 */
static void test_a_foreign_record_forces_the_typing_redraw(void) {
    static const char record[] = "[TEST-ITEM-483] foreign paint\n";
    unsigned int syncs_before;

    begin_case();
    host_font_scale = 1U;
    console_init(512U, 1U);
    send_char('a');
    send_char('b');
    console_publish_record(record, sizeof(record) - 1U);
    reset_serial();
    send_char('c');
    CHECK(serial_contains("hfoc> abc"),
          "the first keystroke after a painted record re-emits the prompt "
          "and the whole line");
    CHECK(line_is("abc"), "the painted record did not disturb the line");
    syncs_before = framebuffer_sync_calls;
    send_char('d');
    CHECK(framebuffer_sync_calls == syncs_before,
          "the fast path resumes for the character after the forced redraw");
    CHECK(line_is("abcd"), "the resumed fast path keeps the line whole");

    /* A journal-only record paints nothing, but it still splits the serial
       witness, so it must invalidate the fast path just the same. */
    reset_serial();
    console_journal_begin();
    console_record_text("[TEST-ITEM-483] journal only\n");
    console_journal_end();
    send_char('e');
    CHECK(serial_contains("hfoc> abcde"),
          "a journal-only record forces the prompt and the whole line back");
    CHECK(line_is("abcde"),
          "the journal-only record did not disturb the line");
}

/*
 * ITEM-482 (round 20): the pointer is hidden before the body moves.
 *
 * The owner's 2026-09-26 run showed the arrow left on the screen after the
 * console reached the bottom and refreshed: the sprite lives in the surface
 * the scroll copies, so a visible pointer is carried up as a ghost and the
 * saved block then puts stale pixels back at the old place.  This proof drives
 * a visible pointer into an overflowing write and requires that no body move
 * ever starts while the arrow is painted: the console hides it first and draws
 * it again afterwards.
 */
static void test_the_cursor_is_hidden_before_the_body_moves(void) {
    struct usb_hid_mouse_batch batch;
    uint64_t fitting_rows;
    unsigned int index;
    unsigned int shows_before;

    begin_case();
    host_font_scale = 1U;
    console_init(512U, 1U);
    scrolls_with_visible_cursor = 0U;
    framebuffer_scroll_calls = 0U;

    memset(&batch, 0, sizeof(batch));
    batch.dx = 7;
    batch.dy = 5;
    batch.events = 3U;
    console_process_mouse_batch(&batch);
    CHECK(cursor_visible_stub,
          "the pointer is painted after a movement batch");

    fitting_rows = console_test_rows_until_scroll();
    for (index = 0U; index < (unsigned int)fitting_rows; ++index) {
        console_write("FITS\n");
    }
    shows_before = cursor_shows;
    console_write("PAST THE BOTTOM\n");
    console_flush_deferred();
    CHECK(framebuffer_scroll_calls != 0U,
          "the overflowing write moved the body");
    CHECK(scrolls_with_visible_cursor == 0U,
          "the body never moved while the arrow was painted");
    CHECK(cursor_shows > shows_before,
          "the pointer is drawn again after the body moved");
}

/*
 * ITEM-474: the menu's geometry is placed while a batch may be open.  The
 * menu's own lines are flushed before the target rows are counted back from
 * the cursor, so the later flush moves the rectangles for the geometry
 * record's own row only.  Without that, a batched menu would move every entry
 * `cells` rows too high and each click would run the entry above the one the
 * geometry names.
 */
static void test_console_menu_targets_under_batch(void) {
    int32_t target_x = 0;
    int32_t target_y = 0;
    int32_t target_width = 0;
    int32_t target_height = 0;
    int32_t published_x = 0;
    int32_t published_y = 0;
    int32_t published_line = 0;
    int32_t published_width = 0;
    unsigned int index;

    begin_case();
    host_font_scale = 1U;
    console_init(512U, 1U);
    host_font_scale = 2U;
    console_begin_turn_batching();
    for (index = 0U; index < 160U; ++index) {
        console_write("FILLER LINE\n");
    }
    reset_serial();
    type_line("menu");
    console_flush_deferred();
    CHECK(console_test_menu_target(0U, &target_x, &target_y, &target_width,
                                   &target_height),
          "the menu target exists when its paint was batched");
    CHECK(last_pointer_geometry(&published_x, &published_y, &published_line,
                                &published_width),
          "the batched menu published its geometry");
    CHECK(published_y == target_y,
          "the batched menu publishes the row the target is on");
    CHECK(published_x == target_x && published_width == target_width,
          "the batched menu publishes the rectangle's column and width");
}

/*
 * ITEM-481: a durable record whose worst case crosses the bound is split into
 * a named head and `[<name>-TAIL]` continuations published in the same tick,
 * instead of being cut with "...".  The proof counts the fields back out of
 * the observed records, so "bounded" can never mean "lost".
 */
static void test_record_bound_spills_into_bounded_tails(void) {
    const struct console_boot_log_binding binding = {
        console_host_observe,
        console_host_dump
    };
    size_t index;
    size_t tails = 0U;

    begin_case();
    observed_count = 0U;
    dump_calls = 0U;
    console_bind_boot_log(&binding);

    console_journal_begin();
    console_record_text("[ITEM481]");
    for (index = 0U; index < 40U; ++index) {
        console_record_text(" f=");
        console_record_u64(123456789012345ULL);
    }
    console_record_text("\n");
    console_journal_end();

    CHECK(observed_count >= 2U,
          "an over-bound durable record is split, not cut");
    CHECK(strncmp(observed[0], "[ITEM481]", 9U) == 0,
          "the head keeps the record's own name");
    for (index = 1U; index < observed_count; ++index) {
        if (strncmp(observed[index], "[ITEM481-TAIL]", 14U) == 0) {
            ++tails;
        }
    }
    CHECK(tails + 1U == observed_count,
          "every continuation is named [<name>-TAIL]");
    for (index = 0U; index < observed_count; ++index) {
        CHECK(strlen(observed[index]) <= CONSOLE_RECORD_MAX,
              "every published journal part is inside the bound");
    }
    {
        char expected[2048];
        char joined[2048];
        size_t expected_length = 0U;
        size_t joined_length = 0U;
        static const char field[] = " f=123456789012345";

        memcpy(expected, "[ITEM481]", 9U);
        expected_length = 9U;
        for (index = 0U; index < 40U; ++index) {
            memcpy(expected + expected_length, field, sizeof(field) - 1U);
            expected_length += sizeof(field) - 1U;
        }
        for (index = 0U; index < observed_count; ++index) {
            const char *body = observed[index];

            if (index != 0U) {
                body += strlen("[ITEM481-TAIL]");
            }
            memcpy(joined + joined_length, body, strlen(body));
            joined_length += strlen(body);
        }
        CHECK(joined_length == expected_length &&
                  memcmp(joined, expected, expected_length) == 0,
              "no field is lost across the tail split");
    }
    CHECK(console_record_spill_count() >= 1U, "the split is counted");
    CHECK(console_record_worst_length() <= CONSOLE_RECORD_MAX,
          "the assembled worst case is inside the bound");
    CHECK(console_record_bound_violations() == 0U,
          "the by-construction bound never had to fire");

    /*
     * A record the caller builds itself - the console transcript, a service
     * line - goes through the same bound: it is split at the bound and every
     * byte survives in the head and its `-TAIL` continuations.
     */
    {
        char long_record[CONSOLE_RECORD_MAX + 200U];
        const size_t length = sizeof(long_record);
        size_t payload = 0U;

        for (index = 0U; index < length; ++index) {
            long_record[index] = 'Q';
        }
        memcpy(long_record, "[ITEM481LEN]", 12U);
        observed_count = 0U;
        console_publish_journal(long_record, length);
        CHECK(observed_count >= 2U,
              "a pre-built over-bound record is split too");
        CHECK(strncmp(observed[0], "[ITEM481LEN]", 12U) == 0,
              "the pre-built head keeps its name");
        CHECK(strncmp(observed[1], "[ITEM481LEN-TAIL]", 17U) == 0,
              "the pre-built continuation is named");
        for (index = 0U; index < observed_count; ++index) {
            CHECK(strlen(observed[index]) <= CONSOLE_RECORD_MAX,
                  "a pre-built part is inside the bound");
            for (const char *cursor = observed[index]; *cursor != '\0';
                 ++cursor) {
                if (*cursor == 'Q') {
                    ++payload;
                }
            }
        }
        CHECK(payload == length - 12U,
              "every byte of a pre-built record survives the split");
    }

    console_bind_boot_log(NULL);
}


/*
 * ITEM-648: the right-hand uptime block is gone from the operator screen.
 * The left title stays, a second event repaints nothing, and the `uptime`
 * command still answers with the clock's value.
 */
static void test_console_status_block_is_gone(void) {
    char expected_header[128];

    begin_case();
    console_init(512U, 1U);
    snprintf(expected_header, sizeof(expected_header), "HFOC OS %sLOCAL CONSOLE",
             hfoc_version_string());
    CHECK(strcmp(header_capture, expected_header) == 0,
          "the painted header uses the build version and keeps the subtitle");
    CHECK(!header_contains("UPTIME"),
          "the right-hand UPTIME label is not painted");
    CHECK(header_text_max_x < 700U,
          "no header text is painted in the right-hand status area");
    CHECK(header_u64_max_x < 700U,
          "no header number is painted in the right-hand status area");

    reset_screen();
    console_update_uptime(7U);
    console_update_uptime(8U);
    console_update_uptime(9U);
    CHECK(header_length == 0U,
          "a second event repaints nothing in the header");
    CHECK(screen_length == 0U,
          "a second event paints nothing on the operator screen");

    reset_screen();
    submit_command("uptime");
    CHECK(screen_contains("UPTIME: "),
          "the uptime command still prints its English label");
    CHECK(screen_contains(" SECONDS"),
          "the uptime command still prints the clock value");
}

/*
 * ITEM-648: a management address never lands inside a credential question.
 * It is held while LOGIN_NAME/LOGIN_PASSWORD or the refusal/cancel result owns
 * the line and painted exactly once at the next prompt boundary.
 */
static void test_management_notice_waits_for_the_prompt(void) {
    static const char login[] = "operator";
    static const char secret[] = "correct horse battery staple";
    static const char url[] = "https://192.0.2.55/";
    static const char url2[] = "https://192.0.2.66/";
    struct credentials_fixture fixture;

    begin_case();
    memset(&fixture, 0, sizeof(fixture));
    fixture.authenticate_ok = true;
    bind_credentials_fixture(&fixture);

    /* A publication in the middle of the login name paints nothing. */
    submit_command("login");
    CHECK(serial_contains("Login:"), "login asks the login");
    send_text("op");
    reset_screen();
    reset_serial();
    console_publish_management(url, strlen(url), "local", "verified");
    CHECK(screen_length == 0U,
          "the address is not painted inside the login question");
    CHECK(serial_length == 0U,
          "the held address is not serialised inside the question");
    CHECK(console_admin_credentials_input_active(),
          "the login question stays open while the address waits");
    send_text("erator");
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    CHECK(serial_contains("Password:"), "login asks the password");

    /* The same address during the password step also waits. */
    send_text("se");
    reset_screen();
    reset_serial();
    console_publish_management(url, strlen(url), "local", "verified");
    CHECK(screen_length == 0U,
          "the address is not painted inside the password echo");
    CHECK(serial_length == 0U,
          "the password line serialises no address");
    send_text("cret");
    reset_screen();
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    CHECK(!console_admin_credentials_input_active(),
          "a successful login closes the conversation");
    CHECK(screen_occurrences("MANAGEMENT: ") == 1U,
          "a successful login paints exactly one management notice");
    CHECK(screen_occurrences(url) == 1U,
          "the notice names the current URL exactly once");
    CHECK(!screen_contains(secret) && !serial_contains(secret),
          "the password still reaches neither the screen nor the serial");

    /* A refusal flushes the held notice after the refusal line. */
    begin_case();
    memset(&fixture, 0, sizeof(fixture));
    fixture.authenticate_ok = false;
    bind_credentials_fixture(&fixture);
    submit_command("login");
    send_text(login);
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    send_char('x');
    reset_screen();
    console_publish_management(url, strlen(url), "local", "verified");
    CHECK(screen_length == 0U, "the refusal question keeps the address off");
    reset_screen();
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    CHECK(!console_admin_credentials_input_active(),
          "a refused login closes the conversation");
    CHECK(screen_contains("login refused"),
          "the refusal is named on screen");
    CHECK(screen_occurrences("MANAGEMENT: ") == 1U,
          "the refusal boundary paints exactly one notice");
    CHECK(screen_occurrences(url) == 1U, "the refusal notice names the URL");

    /* Cancelling with an empty line also flushes exactly once. */
    begin_case();
    memset(&fixture, 0, sizeof(fixture));
    fixture.authenticate_ok = true;
    bind_credentials_fixture(&fixture);
    submit_command("login");
    reset_screen();
    console_publish_management(url, strlen(url), "local", "verified");
    CHECK(screen_length == 0U, "the cancelled question keeps the address off");
    reset_screen();
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    CHECK(!console_admin_credentials_input_active(),
          "an empty line cancels the login");
    CHECK(screen_contains("cancelled"), "the cancellation is said out loud");
    CHECK(screen_occurrences("MANAGEMENT: ") == 1U,
          "the cancellation boundary paints exactly one notice");
    CHECK(screen_occurrences(url) == 1U,
          "the cancellation notice names the URL");

    /* A late address replaces an earlier held one; only the newest prints. */
    begin_case();
    memset(&fixture, 0, sizeof(fixture));
    fixture.authenticate_ok = true;
    bind_credentials_fixture(&fixture);
    submit_command("login");
    send_text(login);
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    send_char('x');
    console_publish_management(url, strlen(url), "local", "verified");
    console_publish_management(url2, strlen(url2), "any", "self-signed");
    reset_screen();
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    CHECK(screen_occurrences("MANAGEMENT: ") == 1U,
          "repeated publications still produce one notice");
    CHECK(screen_contains(url2) && !screen_contains(url),
          "the notice carries the newest address, not the stale one");
    CHECK(screen_contains("reachability=any") &&
              screen_contains("cert=self-signed"),
          "the newest reachability and certificate are painted");

    /* RU and EN both label the notice outside the dialog. */
    begin_case();
    console_publish_language(APPLIANCE_LANGUAGE_RU, true);
    console_publish_management(url, strlen(url), "local", "verified");
    CHECK(screen_occurrences("УПРАВЛЕНИЕ: ") == 1U,
          "the Russian console names the notice in Russian");
    CHECK(screen_occurrences("MANAGEMENT: ") == 0U,
          "the Russian console does not print the English label");
    begin_case();
    console_publish_language(APPLIANCE_LANGUAGE_EN, true);
    console_publish_management(url, strlen(url), "local", "verified");
    CHECK(screen_occurrences("MANAGEMENT: ") == 1U,
          "the English console names the notice in English");

    /* An unchanged address is not repainted, and no address paints nothing. */
    begin_case();
    console_publish_management(url, strlen(url), "local", "verified");
    reset_screen();
    console_publish_management(url, strlen(url), "local", "verified");
    CHECK(screen_length == 0U,
          "an unchanged address is not repainted");
    CHECK(screen_length == 0U, "no stale notice survives without an address");
}

/*
 * ITEM-648: a notice that arrives while an ordinary command is being typed
 * still lands on its own line and does not lose the typed command.
 */
static void test_management_notice_leaves_typed_command_intact(void) {
    static const char url[] = "https://192.0.2.55/";

    begin_case();
    send_text("hel");
    console_publish_management(url, strlen(url), "local", "verified");
    CHECK(screen_occurrences("MANAGEMENT: ") == 1U,
          "an ordinary typed line gets the address on its own line");
    send_text("p");
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    CHECK(screen_contains("COMMANDS: HELP"),
          "the command typed across the notice still runs");
}

/*
 * ITEM-648: the held notice is copied whole or not at all.  A state string of
 * exactly the implementation's 64-byte limit is accepted; one byte past it
 * leaves the previously held notice intact instead of pairing a new address
 * with a truncated state.
 */
static void test_management_notice_state_strings_are_bounded(void) {
    static const char url[] = "https://192.0.2.55/";
    static const char url2[] = "https://192.0.2.66/";
    char max_state[65];
    char over_state[66];
    struct credentials_fixture fixture;

    memset(max_state, 'r', 64U);
    max_state[64] = '\0';
    memset(over_state, 'o', 65U);
    over_state[65] = '\0';

    begin_case();
    memset(&fixture, 0, sizeof(fixture));
    fixture.authenticate_ok = true;
    bind_credentials_fixture(&fixture);
    submit_command("login");
    console_publish_management(url, strlen(url), max_state, max_state);
    reset_screen();
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    CHECK(screen_occurrences(url) == 1U,
          "a state string at the limit keeps its address");
    CHECK(screen_contains(max_state),
          "a state string at the limit is copied whole");

    /* An over-long reachability refuses the whole notice. */
    begin_case();
    memset(&fixture, 0, sizeof(fixture));
    fixture.authenticate_ok = true;
    bind_credentials_fixture(&fixture);
    submit_command("login");
    console_publish_management(url, strlen(url), "local", "verified");
    console_publish_management(url2, strlen(url2), over_state, "self-signed");
    reset_screen();
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    CHECK(screen_contains(url) && !screen_contains(url2),
          "an over-long reachability leaves the held address unchanged");
    CHECK(screen_contains("reachability=local"),
          "the accepted state stays paired with the held address");

    /* An over-long certificate refuses the whole notice too. */
    begin_case();
    memset(&fixture, 0, sizeof(fixture));
    fixture.authenticate_ok = true;
    bind_credentials_fixture(&fixture);
    submit_command("login");
    console_publish_management(url, strlen(url), "local", "verified");
    console_publish_management(url2, strlen(url2), "any", over_state);
    reset_screen();
    send_event(KEYBOARD_EVENT_ENTER, '\0');
    CHECK(screen_contains(url) && !screen_contains(url2),
          "an over-long certificate leaves the held address unchanged");
    CHECK(screen_contains("cert=verified"),
          "the accepted certificate stays paired with the held address");
}

int main(void) {
    test_scrollback_pager();
    test_mouse_wheel_scrollback();
    test_insert_middle();
    test_backspace_delete();
    test_left_right_saturation();
    test_home_end();
    test_full_buffer_ignored();
    test_enter_submits_once();
    test_empty_history_up_down_ignored();
    test_history_navigation_and_draft_restore();
    test_empty_enter_not_stored();
    test_identical_newest_not_duplicated();
    test_equal_older_appends();
    test_seventeen_commands_ring();
    test_edited_recalled_command_stored();
    test_redraw_middle_insertion();
    test_redraw_delete_clears_stale_cell();
    test_appliance_command();
    test_appliance_boot_command();
    test_storage_command();
    test_local_menu_offer();
    test_local_menu_language();
    test_command_case();
    test_commands_match_whatever_case();
    test_credentials_command_classify();
    test_credentials_conversation();
    test_credentials_readable_feedback();
    test_credentials_mask_overflow();
    test_entropy_login_conversation();
    test_local_input_banner();
    test_kbd_command_reports_input_path();
    test_console_record_mirror_is_paint_only();
    test_console_record_mirror_is_bounded_and_single_line();
    test_a_long_record_ending_in_a_newline_is_split();
    test_console_record_mirror_paints_once();
    test_console_record_mirror_survives_the_banner();
    test_console_record_builder_matches_serial();
    test_boot_log_binding();
    test_console_journal_command_and_answer();
    test_console_journal_multi_line_answer();
    test_console_journal_hidden_input();
    test_console_journal_removed_secret_command();
    test_console_journal_unknown_command();
    test_console_journal_menu_and_offer();
    test_console_journal_publication_is_journal_only();
    test_periodic_records_are_journal_only();
    test_service_lines_stay_off_the_screen();
    test_console_display_record();
    test_console_screen_budget();
    test_framebuffer_mode_choice();
    test_framebuffer_preferred_mode_choice();
    test_local_network_wizard_static();
    test_local_network_wizard_dhcp();
    test_local_network_wizard_unknown_current();
    test_local_network_wizard_refusals();
    test_local_network_wizard_unbound();
    test_local_network_wizard_failed_persist();
    test_pointer_targets_follow_scroll();
    test_pointer_click_runs_menu_entry();
    test_mouse_batch_coalesces_and_drains_once();
    test_the_pointer_appears_on_mouse_readiness();
    test_many_batches_close_with_one_drain();
    test_the_deferred_echo_replays_inside_the_batch();
    test_the_console_scales_the_pointer();
    test_the_operator_gain_and_the_pointer_window();
    test_the_arm_depth_handle();
    test_console_scroll_report();
    test_console_scroll_batch();
    test_cyrillic_output_is_code_point_based();
    test_language_command();
    test_gate_command_classify();
    test_gate_command();
    test_help_command();
    test_the_cursor_is_hidden_before_the_body_moves();
    test_typing_appends_without_a_full_redraw();
    test_a_foreign_record_forces_the_typing_redraw();
    test_console_menu_targets_under_batch();
    test_record_bound_spills_into_bounded_tails();
    test_console_status_block_is_gone();
    test_management_notice_waits_for_the_prompt();
    test_management_notice_leaves_typed_command_intact();
    test_management_notice_state_strings_are_bounded();

    if (failures != 0) {
        printf("Console host tests: FAIL (%d)\n", failures);
        return 1;
    }
    printf("Console host tests: OK\n");
    return 0;
}
