#ifndef HFOC_INSTALLER_UI_H
#define HFOC_INSTALLER_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "appliance_language.h"
#include "display.h"

/*
 * ITEM-488: the installer's UI foundation.
 *
 * This item does not install anything.  What it establishes is the shape the
 * installer will grow into, and the rule that shape obeys: the UI is a model
 * plus one renderer, and the renderer draws **through the display
 * abstraction** - never through the firmware's framebuffer, never through a GPU
 * driver.  The model therefore renders identically on a machine with a GOP
 * framebuffer and on a machine whose only output is the text console and the
 * serial stream, which is what makes "install on a machine we have no driver
 * for" a supported case rather than a failure.
 *
 * Every capability the installer has not built yet is `NOT_IMPLEMENTED`, in the
 * UI and in the records.  Nothing here reports a success it did not perform:
 * storage discovery, partitioning and the install executor are the next items,
 * and until they exist this screen says so.
 */

#define INSTALLER_UI_ROWS 4U
#define INSTALLER_UI_VALUE_BYTES 96U
#define INSTALLER_UI_LINE_BYTES 128U
/* Title, blank, four rows, the display row's details, footer: a bounded screen. */
#define INSTALLER_UI_LINES_MAX 16U

enum installer_ui_row_id {
    INSTALLER_UI_ROW_DISPLAY = 0,
    INSTALLER_UI_ROW_STORAGE = 1,
    INSTALLER_UI_ROW_NETWORK = 2,
    INSTALLER_UI_ROW_CONTINUE = 3,
};

enum installer_ui_row_state {
    INSTALLER_UI_STATE_AVAILABLE = 0,
    INSTALLER_UI_STATE_DEGRADED = 1,
    INSTALLER_UI_STATE_UNAVAILABLE = 2,
    /* Explicitly not built in this item - never a fake pass. */
    INSTALLER_UI_STATE_NOT_IMPLEMENTED = 3,
};

struct installer_ui_row {
    enum installer_ui_row_id id;
    const char *label;
    char value[INSTALLER_UI_VALUE_BYTES];
    enum installer_ui_row_state state;
};

struct installer_ui_model {
    const char *title;
    const char *footer;
    struct installer_ui_row rows[INSTALLER_UI_ROWS];
    size_t row_count;
    size_t selected;
    /*
     * ITEM-509 increment 14: a key the catalog cannot answer in the requested
     * language latches this flag while the model is built, and the renderer
     * answers with the named `INSTALLER TRANSLATION MISSING` line instead of
     * half a screen in two languages.
     */
    bool missing_translation;
};

/*
 * The foundation screen: the display's honest capability reading, and the
 * capabilities the following items own, marked `NOT_IMPLEMENTED`.  The screen's
 * prose comes from the one string catalog in the caller's language; a key the
 * catalog cannot answer latches `missing_translation` on the model.
 */
void installer_ui_foundation(struct installer_ui_model *model,
                             enum appliance_language language);
/* Move the selection by `delta` rows, stopping at the ends. */
bool installer_ui_move(struct installer_ui_model *model, int delta);
/* Enter on the selected row; true when the row asked to continue. */
bool installer_ui_activate(struct installer_ui_model *model);
const char *installer_ui_state_name(enum installer_ui_row_state state);
/* What a key does, so a caller can name it in a record. */
const char *installer_ui_key_name(char key);
/*
 * The screen as bounded text lines: one renderer for both backends, so the
 * framebuffer paints exactly the lines the console prints.  Returns the number
 * of lines written, or 0 when the model or the buffer cannot carry a screen.
 */
size_t installer_ui_lines(const struct installer_ui_model *model,
                          enum appliance_language language,
                          char lines[INSTALLER_UI_LINES_MAX][INSTALLER_UI_LINE_BYTES]);
/*
 * The selected row as one record line, bounded:
 *   [INSTALLER-UI] selected=<label> state=<state>
 */
size_t installer_ui_record(const struct installer_ui_model *model,
                           char *buffer,
                           size_t size);

#endif
