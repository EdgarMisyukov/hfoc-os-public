#include "installer_ui.h"

#include "appliance_language.h"

#include <stddef.h>
#include <string.h>

/*
 * ITEM-488: the installer UI as a model and one renderer.
 *
 * The model is a value; the renderer turns it into bounded text lines.  The
 * kernel paints those lines on the firmware framebuffer when the display
 * abstraction has one, and prints the same lines to the console and the serial
 * stream when it does not - so the installer's own code never branches on the
 * backend, which is the point of the abstraction under it.
 *
 * Nothing here claims a capability the installer does not have: storage and
 * network are `NOT_IMPLEMENTED` in this item, and the display row carries
 * whatever the display layer actually measured.
 */

#define INSTALLER_UI_VALUE_MAX (INSTALLER_UI_VALUE_BYTES - 1U)

/*
 * ITEM-509 increment 14: one lookup with the named refusal.  A key the catalog
 * cannot answer in the requested language latches `missing_translation` on the
 * model, and the renderer answers with `INSTALLER TRANSLATION MISSING` instead
 * of a half-translated screen.  The marker itself stays English: it is a
 * record-like diagnostic, not interface prose.
 */
static const char *installer_ui_text(struct installer_ui_model *model,
                                     enum appliance_language language,
                                     const char *key)
{
    const char *text = appliance_language_string(language, key);

    if (text == NULL) {
        model->missing_translation = true;
        return "";
    }
    return text;
}

static void installer_ui_set_value(struct installer_ui_row *row,
                                   const char *text)
{
    size_t index = 0U;

    if (text == NULL) {
        text = "";
    }
    while (text[index] != '\0' && index < INSTALLER_UI_VALUE_MAX) {
        row->value[index] = text[index];
        ++index;
    }
    row->value[index] = '\0';
}

static const char *installer_ui_size_text(char *buffer, size_t size,
                                          const struct display_capabilities *caps)
{
    size_t index = 0U;

    if (caps->width == 0U || caps->height == 0U) {
        const char *none = "NONE";

        while (none[index] != '\0' && index + 1U < size) {
            buffer[index] = none[index];
            ++index;
        }
        buffer[index] = '\0';
        return buffer;
    }
    {
        uint64_t values[2U];
        size_t value_index;

        values[0] = caps->width;
        values[1] = caps->height;
        for (value_index = 0U; value_index < 2U; ++value_index) {
            char digits[20U];
            size_t count = 0U;
            uint64_t value = values[value_index];

            if (value_index != 0U && index + 1U < size) {
                buffer[index++] = 'x';
            }
            if (value == 0U) {
                digits[count++] = '0';
            }
            while (value != 0U && count < sizeof(digits)) {
                digits[count++] = (char)('0' + (value % 10U));
                value /= 10U;
            }
            while (count > 0U && index + 1U < size) {
                buffer[index++] = digits[--count];
            }
        }
        buffer[index] = '\0';
    }
    return buffer;
}

const char *installer_ui_state_name(enum installer_ui_row_state state)
{
    switch (state) {
    case INSTALLER_UI_STATE_AVAILABLE:
        return "AVAILABLE";
    case INSTALLER_UI_STATE_DEGRADED:
        return "DEGRADED";
    case INSTALLER_UI_STATE_UNAVAILABLE:
        return "UNAVAILABLE";
    case INSTALLER_UI_STATE_NOT_IMPLEMENTED:
    default:
        return "NOT_IMPLEMENTED";
    }
}

const char *installer_ui_key_name(char key)
{
    switch (key) {
    case 'w':
    case 'W':
        return "UP";
    case 's':
    case 'S':
        return "DOWN";
    case '\r':
    case '\n':
        return "ENTER";
    case 'q':
    case 'Q':
        return "QUIT";
    default:
        return "OTHER";
    }
}

void installer_ui_foundation(struct installer_ui_model *model,
                             enum appliance_language language)
{
    const struct display_capabilities *caps;
    char size_text[32U];

    if (model == NULL) {
        return;
    }
    memset(model, 0, sizeof(*model));
    model->title = installer_ui_text(
        model, language, "installer.foundation.title");
    model->footer = installer_ui_text(
        model, language, "installer.foundation.footer");
    model->row_count = INSTALLER_UI_ROWS;
    caps = display_capabilities();

    model->rows[0].id = INSTALLER_UI_ROW_DISPLAY;
    model->rows[0].label = installer_ui_text(
        model, language, "installer.foundation.row_display");
    /*
     * The value names the backend and the geometry the abstraction measured; the
     * state is the abstraction's own status, so a machine with no framebuffer
     * reads `TEXT NONE UNKNOWN ... UNAVAILABLE` here instead of a graphics
     * claim the installer cannot honour.
     */
    {
        char value[INSTALLER_UI_VALUE_BYTES];
        const char *backend = display_backend_name(caps->backend);
        const char *format = display_format_name(caps->format);
        const char *size = installer_ui_size_text(size_text, sizeof(size_text),
                                                  caps);
        size_t length = 0U;
        const char *parts[5U];
        size_t part;

        parts[0] = backend;
        parts[1] = " ";
        parts[2] = size;
        parts[3] = " ";
        parts[4] = format;
        for (part = 0U; part < 5U; ++part) {
            size_t index = 0U;

            while (parts[part][index] != '\0' &&
                   length + 1U < sizeof(value)) {
                value[length++] = parts[part][index++];
            }
        }
        value[length] = '\0';
        installer_ui_set_value(&model->rows[0], value);
    }
    switch (caps->status) {
    case DISPLAY_STATUS_AVAILABLE:
        model->rows[0].state = INSTALLER_UI_STATE_AVAILABLE;
        break;
    case DISPLAY_STATUS_DEGRADED:
        model->rows[0].state = INSTALLER_UI_STATE_DEGRADED;
        break;
    case DISPLAY_STATUS_UNAVAILABLE:
    default:
        model->rows[0].state = INSTALLER_UI_STATE_UNAVAILABLE;
        break;
    }

    model->rows[1].id = INSTALLER_UI_ROW_STORAGE;
    model->rows[1].label = installer_ui_text(
        model, language, "installer.foundation.row_storage");
    installer_ui_set_value(
        &model->rows[1],
        installer_ui_text(model, language, "installer.foundation.storage_detail"));
    model->rows[1].state = INSTALLER_UI_STATE_NOT_IMPLEMENTED;

    model->rows[2].id = INSTALLER_UI_ROW_NETWORK;
    model->rows[2].label = installer_ui_text(
        model, language, "installer.foundation.row_network");
    installer_ui_set_value(
        &model->rows[2],
        installer_ui_text(model, language, "installer.foundation.network_detail"));
    model->rows[2].state = INSTALLER_UI_STATE_NOT_IMPLEMENTED;

    model->rows[3].id = INSTALLER_UI_ROW_CONTINUE;
    model->rows[3].label = installer_ui_text(
        model, language, "installer.foundation.row_continue");
    installer_ui_set_value(
        &model->rows[3],
        installer_ui_text(model, language, "installer.foundation.continue_detail"));
    model->rows[3].state = INSTALLER_UI_STATE_AVAILABLE;

    model->selected = 0U;
}

/*
 * ITEM-509 increment 14: the record names the row in English whatever language
 * the screen is drawn in.  A record is a machine line that the acceptance logs
 * and the QEMU markers quote, so it must not follow the operator's language.
 */
static const char *installer_ui_row_key(enum installer_ui_row_id id)
{
    switch (id) {
    case INSTALLER_UI_ROW_DISPLAY:
        return "installer.foundation.row_display";
    case INSTALLER_UI_ROW_STORAGE:
        return "installer.foundation.row_storage";
    case INSTALLER_UI_ROW_NETWORK:
        return "installer.foundation.row_network";
    case INSTALLER_UI_ROW_CONTINUE:
        return "installer.foundation.row_continue";
    default:
        return NULL;
    }
}

bool installer_ui_move(struct installer_ui_model *model, int delta)
{
    if (model == NULL || model->row_count == 0U || delta == 0) {
        return false;
    }
    if (delta < 0) {
        if (model->selected == 0U) {
            return false;
        }
        model->selected -= 1U;
        return true;
    }
    if (model->selected + 1U >= model->row_count) {
        return false;
    }
    model->selected += 1U;
    return true;
}

bool installer_ui_activate(struct installer_ui_model *model)
{
    if (model == NULL || model->selected >= model->row_count) {
        return false;
    }
    return model->rows[model->selected].id == INSTALLER_UI_ROW_CONTINUE;
}

size_t installer_ui_record(const struct installer_ui_model *model,
                           char *buffer,
                           size_t size)
{
    const struct installer_ui_row *row;
    const char *state;
    size_t length = 0U;
    size_t index;

    if (model == NULL || buffer == NULL || size == 0U ||
        model->selected >= model->row_count) {
        return 0U;
    }
    row = &model->rows[model->selected];
    state = installer_ui_state_name(row->state);
    {
        const char *parts[5U];
        const char *label = row->label;
        const char *english = appliance_language_string(
            APPLIANCE_LANGUAGE_EN, installer_ui_row_key(row->id));
        size_t part;

        if (english != NULL) {
            label = english;
        }
        parts[0] = "[INSTALLER-UI] selected=";
        parts[1] = label;
        parts[2] = " state=";
        parts[3] = state;
        parts[4] = NULL;
        for (part = 0U; parts[part] != NULL; ++part) {
            for (index = 0U; parts[part][index] != '\0'; ++index) {
                if (length + 1U >= size) {
                    return 0U;
                }
                buffer[length++] = parts[part][index];
            }
        }
    }
    buffer[length] = '\0';
    return length;
}

/* One bounded line builder, so no line can exceed the buffer it is handed. */
struct installer_ui_line {
    char *text;
    size_t size;
    size_t length;
};

static void installer_ui_line_reset(struct installer_ui_line *line, char *text,
                                    size_t size)
{
    line->text = text;
    line->size = size;
    line->length = 0U;
    if (size != 0U) {
        text[0] = '\0';
    }
}

static void installer_ui_line_append(struct installer_ui_line *line,
                                     const char *text)
{
    size_t index = 0U;

    if (text == NULL) {
        return;
    }
    while (text[index] != '\0' && line->length + 1U < line->size) {
        line->text[line->length++] = text[index++];
    }
    line->text[line->length] = '\0';
}

/* The screen under construction: where the next line goes, and how many there are. */
struct installer_ui_screen {
    char (*lines)[INSTALLER_UI_LINE_BYTES];
    size_t count;
    char scratch[INSTALLER_UI_LINE_BYTES];
};

static struct installer_ui_line installer_ui_screen_line(
    struct installer_ui_screen *screen)
{
    struct installer_ui_line line;

    if (screen->count < INSTALLER_UI_LINES_MAX) {
        installer_ui_line_reset(&line, screen->lines[screen->count],
                                INSTALLER_UI_LINE_BYTES);
        screen->count += 1U;
        return line;
    }
    /* Past the bound the line is built and dropped, never half-written. */
    installer_ui_line_reset(&line, screen->scratch, sizeof(screen->scratch));
    return line;
}

size_t installer_ui_lines(const struct installer_ui_model *model,
                          enum appliance_language language,
                          char lines[INSTALLER_UI_LINES_MAX][INSTALLER_UI_LINE_BYTES])
{
    const struct display_capabilities *caps;
    struct installer_ui_screen screen;
    size_t row;
    char size_text[32U];
    const char *backend_prefix;
    const char *resolution_prefix;
    const char *pixel_format_prefix;
    const char *status_prefix;
    const char *fallback_prefix;

    if (model == NULL || lines == NULL) {
        return 0U;
    }
    screen.lines = lines;
    screen.count = 0U;
    screen.scratch[0] = '\0';
    backend_prefix = appliance_language_string(
        language, "installer.ui.backend_prefix");
    resolution_prefix = appliance_language_string(
        language, "installer.ui.resolution_prefix");
    pixel_format_prefix = appliance_language_string(
        language, "installer.ui.pixel_format_prefix");
    status_prefix = appliance_language_string(
        language, "installer.ui.status_prefix");
    fallback_prefix = appliance_language_string(
        language, "installer.ui.fallback_prefix");
    if (model->missing_translation || backend_prefix == NULL ||
        resolution_prefix == NULL || pixel_format_prefix == NULL ||
        status_prefix == NULL || fallback_prefix == NULL) {
        struct installer_ui_line line = installer_ui_screen_line(&screen);

        installer_ui_line_append(&line, "INSTALLER TRANSLATION MISSING");
        return screen.count;
    }
    caps = display_capabilities();

    {
        struct installer_ui_line line = installer_ui_screen_line(&screen);
        installer_ui_line_append(&line, model->title);
    }
    {
        struct installer_ui_line line = installer_ui_screen_line(&screen);
        installer_ui_line_append(&line, "");
    }
    for (row = 0U; row < model->row_count; ++row) {
        const struct installer_ui_row *entry = &model->rows[row];
        struct installer_ui_line line = installer_ui_screen_line(&screen);

        installer_ui_line_append(&line, row == model->selected ? "> " : "  ");
        installer_ui_line_append(&line, entry->label);
        if (screen.count >= INSTALLER_UI_LINES_MAX) {
            break;
        }
        {
            struct installer_ui_line blank = installer_ui_screen_line(&screen);
            installer_ui_line_append(&blank, "");
        }
        if (entry->id == INSTALLER_UI_ROW_DISPLAY) {
            struct installer_ui_line detail = installer_ui_screen_line(&screen);

            installer_ui_line_append(&detail, backend_prefix);
            installer_ui_line_append(&detail,
                                     display_backend_name(caps->backend));
            detail = installer_ui_screen_line(&screen);
            installer_ui_line_append(&detail, resolution_prefix);
            installer_ui_line_append(
                &detail, installer_ui_size_text(size_text, sizeof(size_text),
                                                caps));
            detail = installer_ui_screen_line(&screen);
            installer_ui_line_append(&detail, pixel_format_prefix);
            installer_ui_line_append(&detail,
                                     display_format_name(caps->format));
            detail = installer_ui_screen_line(&screen);
            installer_ui_line_append(&detail, status_prefix);
            installer_ui_line_append(&detail,
                                     display_status_name(caps->status));
            detail = installer_ui_screen_line(&screen);
            installer_ui_line_append(&detail, fallback_prefix);
            installer_ui_line_append(&detail,
                                     caps->serial_console
                                         ? "SERIAL"
                                         : (caps->text_fallback ? "TEXT"
                                                                : "NONE"));
        } else {
            struct installer_ui_line detail = installer_ui_screen_line(&screen);

            installer_ui_line_append(&detail, status_prefix);
            installer_ui_line_append(
                &detail, installer_ui_state_name(entry->state));
        }
    }
    {
        struct installer_ui_line line = installer_ui_screen_line(&screen);

        installer_ui_line_append(&line, "");
    }
    if (screen.count < INSTALLER_UI_LINES_MAX) {
        struct installer_ui_line line = installer_ui_screen_line(&screen);
        installer_ui_line_append(&line, model->footer);
    }
    return screen.count;
}
