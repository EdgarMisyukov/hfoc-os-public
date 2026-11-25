#include "local_menu.h"

#include "appliance_language.h"

#include <string.h>

struct local_menu_entry_text {
    enum local_menu_entry entry;
    /* The catalog key of the label; the text itself lives in the one catalog. */
    const char *label_key;
    const char *word;
    /* The key that runs this entry directly, or 0 when it has none. */
    char key;
};

static const struct local_menu_entry_text local_menu_entries[] = {
    { LOCAL_MENU_ENTRY_STATUS, "menu.local.status", "status", 's' },
    { LOCAL_MENU_ENTRY_NETWORK, "menu.local.network", "network", 'n' },
    { LOCAL_MENU_ENTRY_DIAGNOSTICS, "menu.local.diagnostics",
      "diagnostics", 'd' },
    { LOCAL_MENU_ENTRY_REBOOT, "menu.local.reboot", "reboot", 'r' },
    { LOCAL_MENU_ENTRY_SHUTDOWN, "menu.local.shutdown", "shutdown", 'p' },
    { LOCAL_MENU_ENTRY_RECOVERY, "menu.local.recovery", "recovery", 'y' },
};

#define LOCAL_MENU_ENTRY_COUNT \
    (sizeof(local_menu_entries) / sizeof(local_menu_entries[0]))

/* The console sizes its click hit-test array by this bound (ITEM-448). */
_Static_assert(LOCAL_MENU_ENTRY_COUNT <= LOCAL_MENU_ENTRY_MAX,
               "the menu grew past LOCAL_MENU_ENTRY_MAX");

static const struct local_menu_entry_text *local_menu_text(
    enum local_menu_entry entry)
{
    size_t index;

    for (index = 0U; index < LOCAL_MENU_ENTRY_COUNT; ++index) {
        if (local_menu_entries[index].entry == entry) {
            return &local_menu_entries[index];
        }
    }
    return NULL;
}

void local_menu_open(struct local_menu *menu)
{
    if (menu == NULL) {
        return;
    }
    menu->active = true;
    menu->selected = 0U;
}

void local_menu_close(struct local_menu *menu)
{
    if (menu == NULL) {
        return;
    }
    menu->active = false;
}

bool local_menu_active(const struct local_menu *menu)
{
    return menu != NULL && menu->active;
}

enum local_menu_entry local_menu_selected(const struct local_menu *menu)
{
    if (menu == NULL || menu->selected >= LOCAL_MENU_ENTRY_COUNT) {
        return LOCAL_MENU_ENTRY_STATUS;
    }
    return local_menu_entries[menu->selected].entry;
}

bool local_menu_select(struct local_menu *menu, size_t index)
{
    if (menu == NULL || index >= LOCAL_MENU_ENTRY_COUNT) {
        return false;
    }
    menu->selected = index;
    return true;
}

size_t local_menu_entry_count(void)
{
    return LOCAL_MENU_ENTRY_COUNT;
}

const char *local_menu_entry_label(enum local_menu_entry entry,
                                   enum appliance_language language)
{
    const struct local_menu_entry_text *text = local_menu_text(entry);
    const char *label;

    /*
     * Both an entry the menu does not carry and a label the catalog does not
     * carry answer with the catalog's own unknown marker, never with an
     * English label that would look translated.
     */
    if (text == NULL) {
        label = appliance_language_string(language, "menu.local.unknown");
    } else {
        label = appliance_language_string(language, text->label_key);
    }
    if (label == NULL) {
        label = appliance_language_string(
            language, "menu.local.unknown");
    }
    return label != NULL ? label : "UNKNOWN";
}

/*
 * ITEM-455: the keys the menu answers to, from the table the menu renders.
 * The list is written whole or not at all: half of it would be worse than
 * none, because a reader cannot tell a short menu from a truncated list.
 */
size_t local_menu_key_list(char *output, size_t capacity) {
    size_t length = 0U;

    if (output == NULL || capacity == 0U) {
        return 0U;
    }
    output[0] = '\0';
    for (size_t index = 0U; index < LOCAL_MENU_ENTRY_COUNT; ++index) {
        const char key = local_menu_entries[index].key;

        if (key == '\0') {
            continue;
        }
        if (length + 1U + (length != 0U ? 1U : 0U) + 1U > capacity) {
            output[0] = '\0';
            return 0U;
        }
        if (length != 0U) {
            output[length++] = ',';
        }
        output[length++] = key;
        output[length] = '\0';
    }
    return length;
}

const char *local_menu_entry_word(enum local_menu_entry entry)
{
    const struct local_menu_entry_text *text = local_menu_text(entry);

    return text != NULL ? text->word : "";
}

static enum local_menu_entry local_menu_entry_for_key(char key)
{
    size_t index;

    for (index = 0U; index < LOCAL_MENU_ENTRY_COUNT; ++index) {
        if (local_menu_entries[index].key == key) {
            return local_menu_entries[index].entry;
        }
    }
    return LOCAL_MENU_ENTRY_COUNT;
}

static size_t local_menu_index_of(enum local_menu_entry entry)
{
    size_t index;

    for (index = 0U; index < LOCAL_MENU_ENTRY_COUNT; ++index) {
        if (local_menu_entries[index].entry == entry) {
            return index;
        }
    }
    return 0U;
}

static void local_menu_move(struct local_menu *menu, int direction)
{
    const size_t count = LOCAL_MENU_ENTRY_COUNT;

    if (direction < 0) {
        menu->selected = menu->selected == 0U ? count - 1U
                                              : menu->selected - 1U;
        return;
    }
    menu->selected = (menu->selected + 1U) % count;
}

enum local_menu_outcome local_menu_handle(
    struct local_menu *menu,
    const struct keyboard_event *event)
{
    if (menu == NULL || event == NULL || !menu->active) {
        return LOCAL_MENU_OUTCOME_NONE;
    }
    switch (event->type) {
    case KEYBOARD_EVENT_UP:
        local_menu_move(menu, -1);
        return LOCAL_MENU_OUTCOME_REDRAW;
    case KEYBOARD_EVENT_DOWN:
        local_menu_move(menu, 1);
        return LOCAL_MENU_OUTCOME_REDRAW;
    case KEYBOARD_EVENT_ENTER:
        return LOCAL_MENU_OUTCOME_RUN;
    case KEYBOARD_EVENT_CHARACTER: {
        const char key = event->character;

        if (key == 'q' || key == 'Q') {
            return LOCAL_MENU_OUTCOME_CLOSE;
        }
        if (key >= '1' && key <= '6') {
            menu->selected = (size_t)(key - '1');
            return LOCAL_MENU_OUTCOME_RUN;
        }
        {
            const enum local_menu_entry entry = local_menu_entry_for_key(key);

            if (entry != LOCAL_MENU_ENTRY_COUNT) {
                menu->selected = local_menu_index_of(entry);
                return LOCAL_MENU_OUTCOME_RUN;
            }
        }
        /* Every other key stays inside the menu: the line editor underneath it
           must not see a keystroke the operator aimed at the menu. */
        return LOCAL_MENU_OUTCOME_NONE;
    }
    default:
        return LOCAL_MENU_OUTCOME_NONE;
    }
}

struct local_menu_text_buffer {
    char *output;
    size_t capacity;
    size_t length;
};

static void local_menu_append(
    struct local_menu_text_buffer *buffer,
    const char *text)
{
    if (text == NULL) {
        return;
    }
    while (*text != '\0') {
        if (buffer->output != NULL && buffer->length + 1U < buffer->capacity) {
            buffer->output[buffer->length] = *text;
        }
        ++buffer->length;
        ++text;
    }
    if (buffer->output != NULL && buffer->capacity != 0U) {
        const size_t end = buffer->length < buffer->capacity
                               ? buffer->length
                               : buffer->capacity - 1U;

        buffer->output[end] = '\0';
    }
}

/*
 * ITEM-509 increment 9: the render is only as good as the catalog it reads.
 * The proof walks every key in both languages, so an incomplete catalog fails
 * the build's own suite; at run time the menu paints one named refusal line
 * instead of half a menu in the wrong language.
 */
#define LOCAL_MENU_TRANSLATION_REFUSAL "MENU TRANSLATION MISSING\n"

static bool local_menu_catalog_complete(enum appliance_language language)
{
    size_t index;

    if (appliance_language_string(language, "menu.local.title") == NULL ||
        appliance_language_string(language, "menu.local.hint") == NULL) {
        return false;
    }
    for (index = 0U; index < LOCAL_MENU_ENTRY_COUNT; ++index) {
        if (appliance_language_string(
                language, local_menu_entries[index].label_key) == NULL) {
            return false;
        }
    }
    return true;
}

size_t local_menu_render(
    const struct local_menu *menu,
    enum appliance_language language,
    char *output,
    size_t capacity)
{
    struct local_menu_text_buffer buffer;
    size_t index;

    buffer.output = output;
    buffer.capacity = capacity;
    buffer.length = 0U;
    if (output != NULL && capacity != 0U) {
        output[0] = '\0';
    }

    if (!local_menu_catalog_complete(language)) {
        local_menu_append(&buffer, LOCAL_MENU_TRANSLATION_REFUSAL);
        return buffer.length;
    }
    local_menu_append(&buffer, "\n");
    local_menu_append(&buffer,
                      appliance_language_string(language, "menu.local.title"));
    local_menu_append(&buffer, "\n");
    for (index = 0U; index < LOCAL_MENU_ENTRY_COUNT; ++index) {
        const bool chosen = menu != NULL && menu->active &&
                            menu->selected == index;

        local_menu_append(&buffer, chosen ? "> [" : "  [");
        {
            const char digit[2] = {
                (char)('1' + (char)index), '\0'
            };

            local_menu_append(&buffer, digit);
        }
        local_menu_append(&buffer, "] ");
        local_menu_append(
            &buffer,
            appliance_language_string(
                language, local_menu_entries[index].label_key));
        local_menu_append(&buffer, "  (");
        {
            const char key[2] = { local_menu_entries[index].key, '\0' };

            local_menu_append(&buffer, key);
        }
        local_menu_append(&buffer, ")\n");
    }
    local_menu_append(&buffer,
                      appliance_language_string(language, "menu.local.hint"));
    local_menu_append(&buffer, "\n");
    return buffer.length;
}

/*
 * ITEM-430: when the machine offers its menu instead of waiting to be asked.
 *
 * The rule has one clock and one edge: the idle window opens when the console
 * is first ready, and it closes at LOCAL_MENU_OFFER_MS unless the operator has
 * touched the keyboard, a menu is already open, or the offer has been made.  A
 * machine with no local input is offered the menu at once - waiting for a
 * keystroke that cannot come would be waiting forever - and the reason the
 * offer carries says which of the two it was, so a reader can tell an idle
 * operator from a machine that cannot type at all.
 *
 * The comparison is the signed difference the rest of the kernel uses for its
 * millisecond clock, so a clock that wraps or goes backwards cannot arm a
 * storm of offers.
 */
enum local_menu_offer_outcome local_menu_offer_due(
    struct local_menu_offer *offer,
    uint32_t now_ms,
    bool console_ready,
    bool input_available,
    bool input_seen,
    bool menu_active)
{
    if (offer == NULL || !console_ready || input_seen || menu_active) {
        return LOCAL_MENU_OFFER_NONE;
    }
    if (!offer->started) {
        offer->started = true;
        offer->first_ms = now_ms;
        if (!input_available) {
            offer->offered = true;
            return LOCAL_MENU_OFFER_NO_LOCAL_INPUT;
        }
        return LOCAL_MENU_OFFER_NONE;
    }
    if (offer->offered) {
        return LOCAL_MENU_OFFER_NONE;
    }
    if (!input_available) {
        offer->offered = true;
        return LOCAL_MENU_OFFER_NO_LOCAL_INPUT;
    }
    if ((int32_t)(now_ms - offer->first_ms) >=
        (int32_t)LOCAL_MENU_OFFER_MS) {
        offer->offered = true;
        return LOCAL_MENU_OFFER_IDLE;
    }
    return LOCAL_MENU_OFFER_NONE;
}
