#include "display.h"

#include <stddef.h>
#include <string.h>

#include "console.h"
#include "framebuffer.h"
#include "installer_ui.h"
#include "limine_min.h"
#include "serial.h"

/*
 * ITEM-488: the firmware (UEFI GOP) backend, and the installer screen drawn
 * through the abstraction.
 *
 * The firmware's framebuffer reaches this kernel the same way every other
 * firmware fact does - through the boot protocol - so this file is the only
 * place that knows what a `struct limine_framebuffer` is.  It copies the
 * description into the abstraction's own value type, lets `display.c` decide
 * whether it may be used, and then routes every drawing call either into the
 * mapped framebuffer or into the text path.  No GPU is named anywhere, because
 * none has to be: the firmware already drew the screen before this kernel
 * existed, and all the installer does is write pixels into it.
 *
 * The text path carries the same screen.  The lines come from the one renderer
 * in `installer_ui.c`, so a machine with no framebuffer, or with one this
 * kernel refuses, shows the operator exactly the flow a machine with a working
 * framebuffer shows - and the serial stream carries it too, which is what the
 * QEMU proof and a hardware run read.
 */

/* The installer's own palette: the console frame's colours. */
#define DISPLAY_BACKGROUND UINT32_C(0x00101820)
#define DISPLAY_TITLE_COLOUR UINT32_C(0x00F0F4F8)
#define DISPLAY_TEXT_COLOUR UINT32_C(0x00C0D0E0)
#define DISPLAY_SELECTED_COLOUR UINT32_C(0x00FFD070)
#define DISPLAY_MARGIN_PIXELS 16U
#define DISPLAY_LINE_PIXELS 10U

static void display_copy_limine(const struct limine_framebuffer *framebuffer,
                                struct display_framebuffer *copy)
{
    memset(copy, 0, sizeof(*copy));
    if (framebuffer == NULL) {
        return;
    }
    copy->present = true;
    copy->address = framebuffer->address;
    copy->width = framebuffer->width;
    copy->height = framebuffer->height;
    copy->pitch = framebuffer->pitch;
    copy->bpp = framebuffer->bpp;
    copy->memory_model = framebuffer->memory_model;
    copy->red_mask_size = framebuffer->red_mask_size;
    copy->red_mask_shift = framebuffer->red_mask_shift;
    copy->green_mask_size = framebuffer->green_mask_size;
    copy->green_mask_shift = framebuffer->green_mask_shift;
    copy->blue_mask_size = framebuffer->blue_mask_size;
    copy->blue_mask_shift = framebuffer->blue_mask_shift;
}

enum display_probe_result display_probe_firmware(
    const struct limine_framebuffer *framebuffer,
    const char *reason,
    bool serial_console)
{
    struct display_framebuffer copy;
    enum display_probe_result result;

    display_copy_limine(framebuffer, &copy);
    if (!copy.present) {
        display_bind_text_fallback(reason != NULL ? reason : "NO_FRAMEBUFFER",
                                   serial_console);
        return DISPLAY_PROBE_NO_FRAMEBUFFER;
    }
    result = display_probe_framebuffer(&copy);
    if (result != DISPLAY_PROBE_OK) {
        /* The abstraction bound the text path with its own reason; a caller
         * that knows more (a firmware with no GOP at all) has already said so. */
        (void)reason;
        (void)serial_console;
    }
    return result;
}

/*
 * One record, published once: the honest reading of what the machine can show.
 * It goes to the serial stream and to the durable journal like every other
 * diagnostic record, and it is written whether or not a framebuffer was found -
 * "no display" is a reading, not a silence.
 */
void display_publish(void)
{
    char record[DISPLAY_RECORD_BYTES];
    const size_t length = display_format_record(record, sizeof(record));

    if (length == 0U) {
        return;
    }
    {
        /* ITEM-440: one record, one serial sequence, so an IRQ trace cannot
         * split the line a proof is waiting for. */
        const uint64_t serial_flags = serial_sequence_begin();

        serial_write(record);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    console_publish_journal(record, length);
}

/*
 * The installer's foundation screen, drawn through the abstraction.  On a
 * machine with a usable framebuffer the lines are painted on the firmware's
 * own framebuffer; on any other machine the same lines go to the console and
 * to the serial stream, and the screen is not an error - it is the same flow.
 */
void display_show_installer_foundation(enum appliance_language language)
{
    struct installer_ui_model model;
    char lines[INSTALLER_UI_LINES_MAX][INSTALLER_UI_LINE_BYTES];
    char record[128U];
    size_t count;
    size_t index;

    installer_ui_foundation(&model, language);
    count = installer_ui_lines(&model, language, lines);
    if (display_framebuffer_usable() && count > 0U) {
        const uint32_t scale = display_scale() > 0U ? display_scale() : 1U;
        const uint64_t margin = (uint64_t)DISPLAY_MARGIN_PIXELS * scale;
        uint64_t y = margin;

        display_clear(DISPLAY_BACKGROUND);
        for (index = 0U; index < count; ++index) {
            uint32_t colour = DISPLAY_TEXT_COLOUR;

            if (index == 0U) {
                colour = DISPLAY_TITLE_COLOUR;
            } else if (lines[index][0] == '>') {
                colour = DISPLAY_SELECTED_COLOUR;
            }
            display_text(margin, y, lines[index], colour, scale);
            y += (uint64_t)DISPLAY_LINE_PIXELS * scale;
        }
        display_present();
    } else {
        for (index = 0U; index < count; ++index) {
            console_write(lines[index]);
            console_write("\n");
        }
    }
    /*
     * The screen is on the serial stream either way: the text path because the
     * stream is the display, and the framebuffer path because a proof reads the
     * stream rather than the screen.
     */
    {
        /*
         * The stream carries the screen in both cases: in text mode it is the
         * display, and on a framebuffer machine a proof - and a hardware run
         * that captures serial - reads the same lines there.
         */
        const uint64_t serial_flags = serial_sequence_begin();

        for (index = 0U; index < count; ++index) {
            serial_write(lines[index]);
            serial_write("\n");
        }
        serial_sequence_end(serial_flags);
    }
    if (installer_ui_record(&model, record, sizeof(record)) > 0U) {
        const uint64_t serial_flags = serial_sequence_begin();

        serial_write(record);
        serial_write("\n");
        serial_sequence_end(serial_flags);
        console_publish_journal(record, strlen(record));
    }
}

/*
 * The abstract drawing path.  A caller never asks which backend it got: it draws,
 * and a machine without a framebuffer simply does not paint.  The text path is
 * not a fallback of last resort - the caller has already been told, through
 * `display_capabilities()`, that the flow is being carried as text.
 */
void display_clear(uint32_t colour)
{
    if (display_framebuffer_usable()) {
        framebuffer_clear(colour);
    }
}

void display_rect(uint64_t x, uint64_t y, uint64_t width, uint64_t height,
                  uint32_t colour)
{
    if (display_framebuffer_usable()) {
        framebuffer_rect(x, y, width, height, colour);
    }
}

void display_text(uint64_t x, uint64_t y, const char *text, uint32_t colour,
                  uint32_t scale)
{
    if (display_framebuffer_usable()) {
        framebuffer_text(x, y, text, colour, scale);
    }
}

void display_u64(uint64_t x, uint64_t y, uint64_t value, uint32_t colour,
                 uint32_t scale)
{
    if (display_framebuffer_usable()) {
        framebuffer_u64(x, y, value, colour, scale);
    }
}

void display_present(void)
{
    if (display_framebuffer_usable()) {
        framebuffer_sync();
    }
}
