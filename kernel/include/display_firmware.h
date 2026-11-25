#ifndef HFOC_DISPLAY_FIRMWARE_H
#define HFOC_DISPLAY_FIRMWARE_H

#include <stdbool.h>

#include "appliance_language.h"
#include "display.h"
#include "limine_min.h"

/*
 * ITEM-488: the firmware (UEFI GOP) backend, as the boot code sees it.
 *
 * This is the only header that names the boot protocol's framebuffer type.  The
 * installer, the console and every other caller work through `display.h`, so
 * replacing this backend later - or proving the layer above it on a host - never
 * has to know how the firmware described its screen.
 */

/*
 * Probe the framebuffer the boot protocol handed over and bind the display to
 * it, or to the text path when it is absent or unusable.  A null framebuffer is
 * the "firmware offered no GOP" case, and it is not an error: it binds the text
 * fallback and reports UNAVAILABLE.  Never panics.
 */
enum display_probe_result display_probe_firmware(
    const struct limine_framebuffer *framebuffer,
    const char *reason,
    bool serial_console);
/* Publish the `[DISPLAY]` reading: serial stream and durable journal, once. */
void display_publish(void);
/*
 * Draw the installer's foundation screen through the abstraction, or print it.
 * ITEM-509 increment 14: the screen's prose comes from the one catalog in the
 * caller's language; the caller passes `APPLIANCE_LANGUAGE_DEFAULT` until the
 * installation medium can declare a language (509.7c-2).
 */
void display_show_installer_foundation(enum appliance_language language);

#endif
