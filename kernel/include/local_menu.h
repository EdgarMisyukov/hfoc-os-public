#ifndef HFOC_LOCAL_MENU_H
#define HFOC_LOCAL_MENU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "appliance_language.h"
#include "keyboard.h"

/*
 * ITEM-419: the menu the person standing at the machine drives.
 *
 * The appliance rendered a list of numbered actions since ITEM-344
 * (`[4] Reboot`, `[5] Shutdown`) and nothing could select one: the renderer had
 * no caller that executed anything, so the machine's own screen offered a menu
 * that did not respond.  This module is the missing half - the selection state
 * and the key handling - and it is pure: it renders into a caller's buffer and
 * consumes keyboard events, so the host test drives the whole menu without a
 * framebuffer, a keyboard or a machine.
 *
 * What it deliberately does not do is act: `local_menu_handle()` reports that
 * the selected entry was chosen, and the console executes it, because reboot
 * and power off have to flush the boot log and publish their own record first
 * (ITEM-418).
 */
#define LOCAL_MENU_RENDER_MAX 1024U

enum local_menu_entry {
    LOCAL_MENU_ENTRY_STATUS = 0,
    LOCAL_MENU_ENTRY_NETWORK = 1,
    LOCAL_MENU_ENTRY_DIAGNOSTICS = 2,
    LOCAL_MENU_ENTRY_REBOOT = 3,
    LOCAL_MENU_ENTRY_SHUTDOWN = 4,
    LOCAL_MENU_ENTRY_RECOVERY = 5,
    LOCAL_MENU_ENTRY_COUNT = 6,
};

/* What the console has to do about an event. */
enum local_menu_outcome {
    LOCAL_MENU_OUTCOME_NONE = 0,
    LOCAL_MENU_OUTCOME_REDRAW = 1,
    LOCAL_MENU_OUTCOME_RUN = 2,
    LOCAL_MENU_OUTCOME_CLOSE = 3,
};

struct local_menu {
    bool active;
    size_t selected;
};

/* Open the menu on its first entry and mark it active. */
void local_menu_open(struct local_menu *menu);

/* Leave the menu without running anything. */
void local_menu_close(struct local_menu *menu);

bool local_menu_active(const struct local_menu *menu);

/*
 * The entry the next ENTER runs, and the entry's word - the same vocabulary
 * `appliance_boot_console_action()` already maps, so the console keeps one
 * action table instead of two.
 */
enum local_menu_entry local_menu_selected(const struct local_menu *menu);

/*
 * ITEM-448: select the entry a click landed on.  The menu's own order decides
 * what the index means, so the console never keeps a second table; an index
 * outside the menu is refused and changes nothing, exactly as a click outside
 * every entry does.
 */
bool local_menu_select(struct local_menu *menu, size_t index);

/* How many entries the menu has, for a caller that hit-tests its lines. */
size_t local_menu_entry_count(void);

/*
 * The compile-time bound a caller sizes its own hit-test array by.  The menu
 * build fails if it ever grows past this, so a click cannot be aimed at a line
 * the console has no room to record.
 */
#define LOCAL_MENU_ENTRY_MAX 8U

/*
 * ITEM-509 increment 9: the menu speaks the language the operator chose.
 *
 * Both the label and the whole rendering take the language explicitly instead
 * of reading a global, so a caller cannot forget it and the host proof can
 * drive both languages without a machine.  The words stay in the one catalog;
 * a key the catalog does not carry is a visible refusal in the render, never a
 * silent English label.
 */
const char *local_menu_entry_label(enum local_menu_entry entry,
                                   enum appliance_language language);
const char *local_menu_entry_word(enum local_menu_entry entry);

/*
 * ITEM-455: the keys that run the entries directly, in the menu's own order,
 * written as one comma-separated list ("s,n,d,r,p,y") and NUL-terminated.
 *
 * The console's offer record publishes them, so a reader of the durable capture
 * - the owner had no `[LOCAL-MENU]` record at all, and the offer line named
 * only ENTER - can see which keys the menu answers to without opening it.  The
 * list comes from the same table the menu renders, so it cannot drift from the
 * keys the menu actually handles.  Returns the bytes written without the
 * terminator, or 0 when the buffer cannot hold the whole list; a buffer that
 * cannot is left as an empty string rather than a truncated list.
 */
#define LOCAL_MENU_KEY_LIST_MAX (LOCAL_MENU_ENTRY_MAX * 2U)
size_t local_menu_key_list(char *output, size_t capacity);

/*
 * One keyboard event.  Up and Down move the selection and wrap around it,
 * ENTER runs the selected entry, and a letter that names an entry runs that
 * entry directly - `r` for reboot, `s` for status, and so on.  Escape does not
 * exist in this kernel's key model, so the way out is `q`, which closes the
 * menu; an event the menu does not use is consumed rather than typed into the
 * line editor underneath it.
 */
enum local_menu_outcome local_menu_handle(
    struct local_menu *menu,
    const struct keyboard_event *event);

/*
 * The whole menu as text, for the console that paints it and for the host test
 * that reads it.  `language` selects the catalog values; the machine words the
 * console runs (`status`, `reboot`, ...) stay English at every language, as the
 * item's boundary requires.  Returns the length the render would have needed,
 * so a caller can tell "it fit" from "it was cut"; a catalog that cannot
 * translate the menu renders one visible refusal line instead of half a menu.
 */
size_t local_menu_render(
    const struct local_menu *menu,
    enum appliance_language language,
    char *output,
    size_t capacity);

/*
 * ITEM-430: the offer the machine makes when nobody asks for the menu.
 *
 * The menu is reachable by typing `menu`, which assumes an operator who knows
 * the word and a keyboard to type it on.  The 2026-09-16 acceptance machine
 * had a keyboard; a machine without one has no way at all to see what the
 * appliance offers, and an operator who has never read the serial protocol has
 * no way to guess the word.  The console therefore offers the menu itself,
 * once per boot: after LOCAL_MENU_OFFER_MS of an untouched keyboard, or at
 * once on a machine with no local input to touch - the case where the offer is
 * the only way the choices are ever seen.
 *
 * The decision is pure, so the host test drives the clock instead of waiting
 * for it.  It never offers a menu the operator has already reached (any
 * keystroke counts), never while a menu is open, and never twice.
 */
#define LOCAL_MENU_OFFER_MS 30000U

enum local_menu_offer_outcome {
    LOCAL_MENU_OFFER_NONE = 0,
    LOCAL_MENU_OFFER_IDLE = 1,
    LOCAL_MENU_OFFER_NO_LOCAL_INPUT = 2,
};

struct local_menu_offer {
    bool started;
    uint32_t first_ms;
    bool offered;
};

/*
 * True when this turn of the console must paint the offer, in which case the
 * offer is marked made.  `console_ready` starts the idle window, `input_seen`
 * is "the operator has pressed something at least once since boot",
 * `input_available` is "this machine has a local keyboard at all", and
 * `menu_active` is "a menu is already open".
 */
enum local_menu_offer_outcome local_menu_offer_due(
    struct local_menu_offer *offer,
    uint32_t now_ms,
    bool console_ready,
    bool input_available,
    bool input_seen,
    bool menu_active);

#endif
