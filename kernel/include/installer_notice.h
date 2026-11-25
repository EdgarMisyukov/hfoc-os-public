#ifndef HFOC_INSTALLER_NOTICE_H
#define HFOC_INSTALLER_NOTICE_H

#include <stdbool.h>
#include <stdint.h>

#include "appliance_language.h"

/*
 * ITEM-417: the installer's own screen.
 *
 * The install flow runs before the kernel initialises its console
 * (`kernel.c` starts the flow long before `console_init`), and the console
 * holds every record it is handed until it is ready.  So the one record the
 * operator most needs to see - "the install is done, the machine is about to
 * reboot, take the medium out" - was written to the serial stream and held back
 * from the screen: on the acceptance host the machine rebooted with the display
 * showing nothing about it, and the owner had to be told what had happened
 * instead of reading it.
 *
 * This module paints that message directly on the framebuffer, which Limine has
 * already handed the kernel by then, and counts the seconds down on the screen
 * the operator is looking at.
 *
 * ITEM-509 increment 19: the title and the footer come from the one catalog in
 * the caller's language.  The font carries Cyrillic since increment 3 and the
 * text path decodes it since increment 17, so a Russian banner is drawn, not
 * mangled; until the medium can declare a language (509.7c-2), the installer
 * passes `APPLIANCE_LANGUAGE_DEFAULT`.
 *
 * `installer_notice_banner` returns false when there is no framebuffer to paint
 * on - a machine whose firmware gave the kernel no display - or when the
 * catalog cannot answer a string, so a caller publishes which of the two
 * happened rather than claiming a message nobody could see.
 */

/*
 * The two sentences the banner prints, read from the catalog.  Each returns
 * NULL for a language the catalog cannot answer - an empty translation is not
 * a translation and never falls back to English.
 */
const char *installer_notice_title(enum appliance_language language);
const char *installer_notice_footer(enum appliance_language language);

bool installer_notice_banner(enum appliance_language language,
                             uint32_t seconds);

#endif
