#include "installer_storage.h"

#include "appliance_language.h"

#include <stddef.h>
#include <string.h>

/*
 * ITEM-489: the storage screen's model.
 *
 * This file reads a discovery report and produces a screen and a record.  It
 * does not write anything anywhere, and it deliberately does not include the
 * block device, ESP, installer or flow headers: the only way to partition,
 * format or install from here would be to add one of them, which is what the
 * slice pins and what the next items own.
 *
 * The two rules that matter live here, in code rather than in a comment:
 * `installer_storage_inventory()` leaves `selected` at NONE whatever it found,
 * and `installer_storage_choose()` refuses a row whose identity or geometry was
 * not read.  A default of "the only disk is the disk" is exactly the default
 * that destroys data on a machine nobody asked it about.
 */

/*
 * One decimal, integer maths: freestanding code has no floating point here.
 * ITEM-514: exported so the summary page prints a disk exactly the way the
 * storage screen prints it.
 */
void installer_storage_size_text(char *text, size_t size, uint64_t bytes)
{
    static const struct {
        uint64_t unit;
        const char *suffix;
    } units[] = {
        { UINT64_C(1000000000000), "TB" },
        { UINT64_C(1000000000), "GB" },
        { UINT64_C(1000000), "MB" },
    };
    size_t unit_index;

    if (size == 0U) {
        return;
    }
    if (bytes == 0U) {
        text[0] = '\0';
        return;
    }
    for (unit_index = 0U; unit_index < sizeof(units) / sizeof(units[0]);
         ++unit_index) {
        if (bytes >= units[unit_index].unit) {
            const uint64_t whole = bytes / units[unit_index].unit;
            const uint64_t tenth =
                (bytes % units[unit_index].unit) * 10U / units[unit_index].unit;
            size_t length = 0U;
            char digits[24U];
            size_t count = 0U;
            uint64_t value = whole;

            if (value == 0U) {
                digits[count++] = '0';
            }
            while (value != 0U && count < sizeof(digits)) {
                digits[count++] = (char)('0' + (value % 10U));
                value /= 10U;
            }
            while (count > 0U && length + 1U < size) {
                text[length++] = digits[--count];
            }
            if (length + 1U < size) {
                text[length++] = '.';
            }
            if (length + 1U < size) {
                text[length++] = (char)('0' + tenth);
            }
            {
                size_t index = 0U;
                char spacer[2U];

                spacer[0] = ' ';
                spacer[1] = '\0';
                if (length + 1U < size) {
                    text[length++] = spacer[0];
                }
                while (units[unit_index].suffix[index] != '\0' &&
                       length + 1U < size) {
                    text[length++] = units[unit_index].suffix[index++];
                }
            }
            text[length] = '\0';
            return;
        }
    }
    /* Under a megabyte: name the bytes rather than round them to zero. */
    {
        size_t length = 0U;
        char digits[24U];
        size_t count = 0U;
        uint64_t value = bytes;

        while (value != 0U && count < sizeof(digits)) {
            digits[count++] = (char)('0' + (value % 10U));
            value /= 10U;
        }
        while (count > 0U && length + 1U < size) {
            text[length++] = digits[--count];
        }
        if (length + 1U < size) {
            text[length++] = 'B';
        }
        text[length] = '\0';
    }
}

static void installer_storage_copy_text(char *text, size_t size, const char *value)
{
    size_t index = 0U;

    if (size == 0U) {
        return;
    }
    if (value == NULL) {
        value = "";
    }
    while (value[index] != '\0' && index + 1U < size) {
        text[index] = value[index];
        ++index;
    }
    text[index] = '\0';
}

const char *installer_storage_state_name(enum installer_storage_disk_state state)
{
    switch (state) {
    case INSTALLER_STORAGE_DISK_SELECTABLE:
        return "SELECTABLE";
    case INSTALLER_STORAGE_DISK_UNREADABLE:
        return "UNREADABLE";
    case INSTALLER_STORAGE_DISK_ABSENT:
    default:
        return "ABSENT";
    }
}

/*
 * The keys the screen advertises, decided here rather than in the machine half:
 * the arrow keys the input layer decodes and the `w`/`s` the footer names, so an
 * operator who reads the screen can drive it with either hand on the keyboard.
 */
static bool installer_storage_is_up_character(char character)
{
    return character == 'w' || character == 'W';
}

static bool installer_storage_is_down_character(char character)
{
    return character == 's' || character == 'S';
}

static bool installer_storage_is_leave_character(char character)
{
    return character == 'q' || character == 'Q';
}

enum installer_storage_key_action installer_storage_key_action(
    enum keyboard_event_type type,
    char character)
{
    switch (type) {
    case KEYBOARD_EVENT_UP:
        return INSTALLER_STORAGE_KEY_UP;
    case KEYBOARD_EVENT_DOWN:
        return INSTALLER_STORAGE_KEY_DOWN;
    case KEYBOARD_EVENT_ENTER:
        return INSTALLER_STORAGE_KEY_CHOOSE;
    case KEYBOARD_EVENT_CHARACTER:
        if (installer_storage_is_up_character(character)) {
            return INSTALLER_STORAGE_KEY_UP;
        }
        if (installer_storage_is_down_character(character)) {
            return INSTALLER_STORAGE_KEY_DOWN;
        }
        if (installer_storage_is_leave_character(character)) {
            return INSTALLER_STORAGE_KEY_LEAVE;
        }
        return INSTALLER_STORAGE_KEY_NONE;
    default:
        /* A key nobody defined does nothing: no screen action falls out of an
         * event this item did not decide on. */
        return INSTALLER_STORAGE_KEY_NONE;
    }
}

const char *installer_storage_key_name(enum installer_storage_key_action action)
{
    switch (action) {
    case INSTALLER_STORAGE_KEY_UP:
        return "UP";
    case INSTALLER_STORAGE_KEY_DOWN:
        return "DOWN";
    case INSTALLER_STORAGE_KEY_CHOOSE:
        return "CHOOSE";
    case INSTALLER_STORAGE_KEY_LEAVE:
        return "LEAVE";
    case INSTALLER_STORAGE_KEY_NONE:
    default:
        return "NONE";
    }
}

const char *installer_storage_controller_name(
    enum installer_disk_controller_kind controller)
{
    switch (controller) {
    case INSTALLER_DISK_CONTROLLER_VIRTIO_BLK:
        return "VIRTIO-BLK";
    case INSTALLER_DISK_CONTROLLER_SATA:
        return "SATA";
    case INSTALLER_DISK_CONTROLLER_NVME:
        return "NVME";
    case INSTALLER_DISK_CONTROLLER_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

const char *installer_storage_table_name(
    enum installer_partition_table_kind table)
{
    switch (table) {
    case INSTALLER_PARTITION_TABLE_GPT:
        return "GPT";
    case INSTALLER_PARTITION_TABLE_MBR:
        return "MBR";
    case INSTALLER_PARTITION_TABLE_NONE:
    default:
        return "NONE";
    }
}

static bool installer_storage_disk_has_owner(
    const struct installer_discovery_report *report,
    uint32_t disk_id)
{
    uint32_t index;

    for (index = 0U; index < report->partition_count &&
                     index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        const struct installer_discovery_partition *partition =
            &report->partitions[index];

        if (partition->present && partition->disk_id == disk_id &&
            partition->owner != INSTALLER_OS_UNKNOWN) {
            return true;
        }
    }
    return false;
}

void installer_storage_set_note(struct installer_storage_model *model,
                                const char *note)
{
    if (model == NULL) {
        return;
    }
    model->note = note != NULL && note[0] != '\0' ? note : NULL;
}

void installer_storage_inventory(struct installer_storage_model *model,
                                 const struct installer_discovery_report *report)
{
    size_t row = 0U;
    uint32_t index;

    if (model == NULL) {
        return;
    }
    memset(model, 0, sizeof(*model));
    /* No selection, before anything is read: a machine nobody has chosen on
     * shows exactly that. */
    model->selected = INSTALLER_STORAGE_NONE;
    model->cursor = 0U;
    if (report == NULL) {
        return;
    }
    for (index = 0U; index < report->disk_count &&
                     index < INSTALLER_DISCOVERY_DISK_CAPACITY &&
                     row < INSTALLER_STORAGE_DISKS_MAX;
         ++index) {
        const struct installer_discovery_disk *disk = &report->disks[index];
        struct installer_storage_disk *entry = &model->disks[row];
        const uint64_t bytes =
            (uint64_t)disk->block_size * (uint64_t)disk->block_count;
        const bool readable = disk->present && disk->identity[0] != '\0' &&
                              disk->block_size != 0U && disk->block_count != 0U;

        if (!disk->present) {
            continue;
        }
        entry->id = disk->id;
        entry->present = true;
        entry->state = readable ? INSTALLER_STORAGE_DISK_SELECTABLE
                                : INSTALLER_STORAGE_DISK_UNREADABLE;
        entry->controller = disk->controller;
        entry->partition_table = disk->partition_table;
        entry->bytes = bytes;
        entry->has_efi_system_partition = disk->has_efi_system_partition;
        entry->has_existing_os = installer_storage_disk_has_owner(report,
                                                                  disk->id);
        installer_storage_copy_text(entry->identity, sizeof(entry->identity),
                                    disk->identity);
        /* ITEM-495: the name the hardware reported, when it reported one. */
        installer_storage_copy_text(entry->model, sizeof(entry->model),
                                    disk->model);
        installer_storage_size_text(entry->size_text, sizeof(entry->size_text),
                                    bytes);
        installer_storage_copy_text(entry->transport_text,
                                    sizeof(entry->transport_text),
                                    installer_storage_controller_name(
                                        disk->controller));
        installer_storage_copy_text(entry->table_text,
                                    sizeof(entry->table_text),
                                    installer_storage_table_name(
                                        disk->partition_table));
        if (readable) {
            model->selectable_count += 1U;
        }
        row += 1U;
    }
    model->disk_count = row;
}

bool installer_storage_move(struct installer_storage_model *model, int delta)
{
    if (model == NULL || model->disk_count == 0U || delta == 0) {
        return false;
    }
    if (delta < 0) {
        if (model->cursor == 0U) {
            return false;
        }
        model->cursor -= 1U;
        return true;
    }
    if (model->cursor + 1U >= model->disk_count) {
        return false;
    }
    model->cursor += 1U;
    return true;
}

bool installer_storage_choose(struct installer_storage_model *model)
{
    if (model == NULL || model->cursor >= model->disk_count) {
        return false;
    }
    if (model->disks[model->cursor].state !=
        INSTALLER_STORAGE_DISK_SELECTABLE) {
        /* A disk nobody could name is a disk nobody may write to. */
        return false;
    }
    /*
     * ITEM-495: a new choice withdraws the previous acceptance.  Confirming a
     * disk and then moving to another one must not leave the first one
     * confirmed behind the operator's back.
     */
    model->confirmed = false;
    model->selected = (int32_t)model->cursor;
    return true;
}

bool installer_storage_confirm(struct installer_storage_model *model)
{
    if (!installer_storage_can_continue(model)) {
        return false;
    }
    model->confirmed = true;
    return true;
}

const struct installer_storage_disk *installer_storage_selected(
    const struct installer_storage_model *model)
{
    if (model == NULL || model->selected == INSTALLER_STORAGE_NONE ||
        (size_t)model->selected >= model->disk_count) {
        return NULL;
    }
    return &model->disks[model->selected];
}

bool installer_storage_select_id(struct installer_storage_model *model,
                                 uint32_t disk_id)
{
    size_t row;

    if (model == NULL) {
        return false;
    }
    for (row = 0U; row < model->disk_count; ++row) {
        if (model->disks[row].id != disk_id) {
            continue;
        }
        /* Same rule as the cursor path, applied to an id: only a row the
         * hardware described completely may be selected. */
        if (model->disks[row].state != INSTALLER_STORAGE_DISK_SELECTABLE) {
            return false;
        }
        model->selected = (int32_t)row;
        model->cursor = row;
        return true;
    }
    return false;
}

bool installer_storage_can_continue(const struct installer_storage_model *model)
{
    const struct installer_storage_disk *disk =
        installer_storage_selected(model);

    return disk != NULL && disk->state == INSTALLER_STORAGE_DISK_SELECTABLE;
}

/*
 * The screen must be able to show every disk the model can hold, plus the title,
 * the hint and the status line: a screen that silently drops a disk is a screen
 * an operator can be wrong about.  Worst case per disk is five lines - the row,
 * the transport and table, the state, the "chosen" line and the blank between
 * disks.
 */
_Static_assert(INSTALLER_STORAGE_LINES_MAX >=
                   2U + INSTALLER_STORAGE_DISKS_MAX * 5U,
               "the storage screen must show every disk the model can hold");

/*
 * One bounded line builder, shared by every line of the storage screens and,
 * since ITEM-514, by the scheme and summary screens in the sibling file.  The
 * type and the three calls live in the header; the definitions are here.
 */
void installer_storage_line_reset(struct installer_storage_line *line,
                                  char *text, size_t size)
{
    line->text = text;
    line->size = size;
    line->length = 0U;
    if (size != 0U) {
        text[0] = '\0';
    }
}

void installer_storage_line_append(struct installer_storage_line *line,
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

void installer_storage_line_u64(struct installer_storage_line *line,
                                uint64_t value)
{
    char digits[20U];
    size_t count = 0U;

    if (value == 0U) {
        installer_storage_line_append(line, "0");
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
        installer_storage_line_append(line, single);
    }
}

struct installer_storage_screen {
    char (*lines)[INSTALLER_STORAGE_LINE_BYTES];
    size_t count;
    char scratch[INSTALLER_STORAGE_LINE_BYTES];
};

static struct installer_storage_line installer_storage_screen_line(
    struct installer_storage_screen *screen)
{
    struct installer_storage_line line;

    if (screen->count < INSTALLER_STORAGE_LINES_MAX) {
        installer_storage_line_reset(&line, screen->lines[screen->count],
                                     INSTALLER_STORAGE_LINE_BYTES);
        screen->count += 1U;
        return line;
    }
    installer_storage_line_reset(&line, screen->scratch,
                                 sizeof(screen->scratch));
    return line;
}

size_t installer_storage_lines(
    const struct installer_storage_model *model,
    enum appliance_language language,
    char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES])
{
    struct installer_storage_screen screen;
    size_t row;
    const char *select_disk;
    const char *no_disk;
    const char *status_prefix;
    const char *unidentified_disk;
    const char *transport_prefix;
    const char *table_prefix;
    const char *state_prefix;
    const char *existing_system;
    const char *efi_partition;
    const char *confirmed_hint;
    const char *selected_hint;
    const char *move_hint;
    const char *chosen_note;
    const char *not_chosen_note;

    if (model == NULL || lines == NULL) {
        return 0U;
    }
    screen.lines = lines;
    screen.count = 0U;
    screen.scratch[0] = '\0';
    /*
     * ITEM-509 increment 16: the disk list reads its prose from the one
     * catalog; a key the catalog cannot answer is the named refusal, not half a
     * screen.  The cursor, the indents, the disk's own name and identity, the
     * size and the state words stay machine values and layout.
     */
    select_disk = appliance_language_string(
        language, "installer.storage.select_disk");
    no_disk = appliance_language_string(language, "installer.storage.no_disk");
    status_prefix = appliance_language_string(
        language, "installer.storage.status_prefix");
    unidentified_disk = appliance_language_string(
        language, "installer.storage.unidentified_disk");
    transport_prefix = appliance_language_string(
        language, "installer.storage.transport_prefix");
    table_prefix = appliance_language_string(
        language, "installer.storage.table_prefix");
    state_prefix = appliance_language_string(
        language, "installer.storage.state_prefix");
    existing_system = appliance_language_string(
        language, "installer.storage.existing_system");
    efi_partition = appliance_language_string(
        language, "installer.storage.efi_partition");
    confirmed_hint = appliance_language_string(
        language, "installer.storage.confirmed_hint");
    selected_hint = appliance_language_string(
        language, "installer.storage.selected_hint");
    move_hint = appliance_language_string(
        language, "installer.storage.move_hint");
    chosen_note = appliance_language_string(
        language, "installer.storage.chosen_note");
    not_chosen_note = appliance_language_string(
        language, "installer.storage.not_chosen_note");
    if (select_disk == NULL || no_disk == NULL || status_prefix == NULL ||
        unidentified_disk == NULL || transport_prefix == NULL ||
        table_prefix == NULL || state_prefix == NULL ||
        existing_system == NULL || efi_partition == NULL ||
        confirmed_hint == NULL || selected_hint == NULL || move_hint == NULL ||
        chosen_note == NULL || not_chosen_note == NULL) {
        struct installer_storage_line line = installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, "INSTALLER TRANSLATION MISSING");
        return screen.count;
    }

    {
        struct installer_storage_line line = installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, select_disk);
    }
    /*
     * ITEM-500: the caller's own reason, printed before the disks, so the
     * operator reads why the machine is asking instead of installing.  The
     * line is a normal screen line: mixed case, no markers, cut at the line
     * width by the same append every other line uses.
     */
    if (model->note != NULL) {
        struct installer_storage_line line = installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, model->note);
    }
    if (model->disk_count == 0U) {
        struct installer_storage_line line = installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, no_disk);
        line = installer_storage_screen_line(&screen);
        installer_storage_line_append(&line, status_prefix);
        installer_storage_line_append(&line, "UNAVAILABLE");
    }
    for (row = 0U; row < model->disk_count; ++row) {
        const struct installer_storage_disk *disk = &model->disks[row];
        struct installer_storage_line line = installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, row == model->cursor ? "> " : "  ");
        if (disk->model[0] != '\0') {
            /* ITEM-495: the machine's own name for the disk first, so an
             * operator recognises the part he is about to erase. */
            installer_storage_line_append(&line, disk->model);
            installer_storage_line_append(&line, "  ");
        }
        if (disk->identity[0] != '\0') {
            installer_storage_line_append(&line, disk->identity);
        } else {
            installer_storage_line_append(&line, unidentified_disk);
        }
        installer_storage_line_append(&line, "  ");
        installer_storage_line_append(&line, disk->size_text);
        line = installer_storage_screen_line(&screen);
        installer_storage_line_append(&line, transport_prefix);
        installer_storage_line_append(&line, disk->transport_text);
        installer_storage_line_append(&line, table_prefix);
        installer_storage_line_append(&line, disk->table_text);
        line = installer_storage_screen_line(&screen);
        installer_storage_line_append(&line, state_prefix);
        installer_storage_line_append(&line,
                                      installer_storage_state_name(disk->state));
        if (disk->has_existing_os) {
            installer_storage_line_append(&line, existing_system);
        } else if (disk->has_efi_system_partition) {
            installer_storage_line_append(&line, efi_partition);
        }
        if (row == (size_t)model->selected) {
            line = installer_storage_screen_line(&screen);
            /*
             * ITEM-495: the confirmation is on this screen now.  The old line
             * sent the operator to the console for a token; the warning page
             * that ENTER draws is what he answers.
             */
            installer_storage_line_append(
                &line, model->confirmed ? confirmed_hint : selected_hint);
        }
        line = installer_storage_screen_line(&screen);
        installer_storage_line_append(&line, "");
    }
    {
        struct installer_storage_line line = installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, move_hint);
    }
    {
        struct installer_storage_line line = installer_storage_screen_line(&screen);

        if (installer_storage_can_continue(model)) {
            installer_storage_line_append(&line, chosen_note);
        } else {
            installer_storage_line_append(&line, not_chosen_note);
        }
    }
    return screen.count;
}

/*
 * ITEM-495: the page between choosing and writing.
 *
 * The operator entry used to end at the console, where a typed token was the
 * confirmation and nothing on the screen said what the token would destroy.
 * This page says it: the disk by the name and the identity the hardware
 * reported, what is on it, and the one sentence that matters - everything on it
 * goes away.  ENTER accepts what the page says, Q returns to the list, and
 * nothing else on the keyboard does anything at all.
 */
size_t installer_storage_warning_lines(
    const struct installer_storage_disk *disk,
    enum appliance_language language,
    char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES])
{
    struct installer_storage_screen screen;
    const char *warning_no_disk;
    const char *nothing_written;
    const char *warning_title;
    const char *disk_prefix;
    const char *unidentified_disk;
    const char *capacity_prefix;
    const char *transport_prefix;
    const char *table_prefix;
    const char *has_system_note;
    const char *has_efi_note;
    const char *no_system_note;
    const char *erase_note;
    const char *settings_note;
    const char *settings_detail;
    const char *not_written_yet;
    const char *erase_footer;

    if (lines == NULL) {
        return 0U;
    }
    screen.lines = lines;
    screen.count = 0U;
    screen.scratch[0] = '\0';
    /*
     * ITEM-509 increment 18: the page's prose comes from the one catalog in the
     * caller's language; a key the catalog cannot answer is the named refusal,
     * not half a page.  The disk's own name, identity, capacity, transport and
     * table stay as the machine reported them.
     */
    warning_no_disk = appliance_language_string(
        language, "installer.storage.warning_no_disk");
    nothing_written = appliance_language_string(
        language, "installer.storage.nothing_written");
    warning_title = appliance_language_string(
        language, "installer.storage.warning_title");
    disk_prefix = appliance_language_string(
        language, "installer.storage.disk_prefix");
    unidentified_disk = appliance_language_string(
        language, "installer.storage.unidentified_disk");
    capacity_prefix = appliance_language_string(
        language, "installer.storage.capacity_prefix");
    transport_prefix = appliance_language_string(
        language, "installer.storage.warning_transport_prefix");
    table_prefix = appliance_language_string(
        language, "installer.storage.table_prefix");
    has_system_note = appliance_language_string(
        language, "installer.storage.has_system_note");
    has_efi_note = appliance_language_string(
        language, "installer.storage.has_efi_note");
    no_system_note = appliance_language_string(
        language, "installer.storage.no_system_note");
    erase_note = appliance_language_string(
        language, "installer.storage.erase_note");
    settings_note = appliance_language_string(
        language, "installer.storage.settings_note");
    settings_detail = appliance_language_string(
        language, "installer.storage.settings_detail");
    not_written_yet = appliance_language_string(
        language, "installer.storage.not_written_yet");
    erase_footer = appliance_language_string(
        language, "installer.storage.erase_footer");
    if (warning_no_disk == NULL || nothing_written == NULL ||
        warning_title == NULL || disk_prefix == NULL ||
        unidentified_disk == NULL || capacity_prefix == NULL ||
        transport_prefix == NULL || table_prefix == NULL ||
        has_system_note == NULL || has_efi_note == NULL ||
        no_system_note == NULL || erase_note == NULL ||
        settings_note == NULL || settings_detail == NULL ||
        not_written_yet == NULL || erase_footer == NULL) {
        struct installer_storage_line line = installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, "INSTALLER TRANSLATION MISSING");
        return screen.count;
    }
    if (disk == NULL) {
        struct installer_storage_line line = installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, warning_no_disk);
        line = installer_storage_screen_line(&screen);
        installer_storage_line_append(&line, nothing_written);
        return screen.count;
    }
    {
        struct installer_storage_line line = installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, warning_title);
    }
    {
        struct installer_storage_line line = installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, disk_prefix);
        if (disk->model[0] != '\0') {
            installer_storage_line_append(&line, disk->model);
            installer_storage_line_append(&line, "  ");
        }
        if (disk->identity[0] != '\0') {
            installer_storage_line_append(&line, disk->identity);
        } else {
            installer_storage_line_append(&line, unidentified_disk);
        }
    }
    {
        struct installer_storage_line line = installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, capacity_prefix);
        installer_storage_line_append(&line, disk->size_text);
        installer_storage_line_append(&line, transport_prefix);
        installer_storage_line_append(&line, disk->transport_text);
        installer_storage_line_append(&line, table_prefix);
        installer_storage_line_append(&line, disk->table_text);
    }
    {
        struct installer_storage_line line = installer_storage_screen_line(&screen);

        if (disk->has_existing_os) {
            installer_storage_line_append(&line, has_system_note);
        } else if (disk->has_efi_system_partition) {
            installer_storage_line_append(&line, has_efi_note);
        } else {
            installer_storage_line_append(&line, no_system_note);
        }
    }
    {
        struct installer_storage_line line = installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, erase_note);
    }
    /*
     * ITEM-502: the erase the install performs is named here, so the operator
     * reads what is destroyed instead of discovering it after the reboot.
     */
    {
        struct installer_storage_line line = installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, settings_note);
        line = installer_storage_screen_line(&screen);
        installer_storage_line_append(&line, settings_detail);
    }
    {
        struct installer_storage_line line = installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, not_written_yet);
    }
    {
        struct installer_storage_line line = installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, erase_footer);
    }
    return screen.count;
}

size_t installer_storage_record(const struct installer_storage_model *model,
                                char *buffer,
                                size_t size)
{
    const struct installer_storage_disk *disk;
    struct installer_storage_line line;
    char record[INSTALLER_STORAGE_RECORD_BYTES + 1U];

    if (model == NULL || buffer == NULL || size == 0U) {
        return 0U;
    }
    /* Assembled in full and then refused if it does not fit the caller's
     * buffer: a record that carries half a field is worse than none. */
    installer_storage_line_reset(&line, record, sizeof(record));
    installer_storage_line_append(&line, "[INSTALLER-STORAGE] disks=");
    installer_storage_line_u64(&line, (uint64_t)model->disk_count);
    installer_storage_line_append(&line, " selectable=");
    installer_storage_line_u64(&line, (uint64_t)model->selectable_count);
    installer_storage_line_append(&line, " selected=");
    disk = installer_storage_selected(model);
    if (disk == NULL) {
        installer_storage_line_append(&line, "NONE");
        installer_storage_line_append(&line, " state=NONE table=NONE "
                                             "existing_os=0");
    } else {
        installer_storage_line_u64(&line, disk->id);
        installer_storage_line_append(&line, " state=");
        installer_storage_line_append(
            &line, installer_storage_state_name(disk->state));
        installer_storage_line_append(&line, " table=");
        installer_storage_line_append(&line, disk->table_text);
        installer_storage_line_append(&line, " existing_os=");
        installer_storage_line_append(&line, disk->has_existing_os ? "1" : "0");
    }
    if (line.length == 0U || line.length > INSTALLER_STORAGE_RECORD_BYTES ||
        line.length + 1U > size) {
        return 0U;
    }
    memcpy(buffer, record, line.length + 1U);
    return line.length;
}

/* --------------------------------------------------- ITEM-501: release -- */

const char *installer_storage_kind_name(enum installer_partition_kind kind)
{
    switch (kind) {
    case INSTALLER_PARTITION_EFI_SYSTEM:
        return "EFI system";
    case INSTALLER_PARTITION_LINUX_ROOT:
        return "Linux";
    case INSTALLER_PARTITION_WINDOWS:
        return "Windows";
    case INSTALLER_PARTITION_OTHER:
        return "other";
    case INSTALLER_PARTITION_UNKNOWN:
    default:
        return "unknown";
    }
}

const char *installer_storage_owner_name(enum installer_os_kind owner)
{
    switch (owner) {
    case INSTALLER_OS_LINUX:
        return "Linux";
    case INSTALLER_OS_WINDOWS:
        return "Windows";
    case INSTALLER_OS_OTHER:
        return "other system";
    case INSTALLER_OS_UNKNOWN:
    default:
        return "no recognised system";
    }
}

static uint64_t installer_storage_disk_block_size(
    const struct installer_discovery_report *report,
    uint32_t disk_id)
{
    size_t index;

    if (report == NULL) {
        return 0U;
    }
    for (index = 0U; index < INSTALLER_DISCOVERY_DISK_CAPACITY; ++index) {
        if (report->disks[index].present &&
            report->disks[index].id == disk_id) {
            return report->disks[index].block_size;
        }
    }
    return 0U;
}

/*
 * A row may be given up when it is a partition rather than a table and it is
 * not the shared EFI System Partition: removing the ESP would take the other
 * system's loader with it, and a container is not a partition at all.
 */
static bool installer_storage_partition_releasable(
    const struct installer_discovery_partition *partition)
{
    return partition->present && !partition->efi_system &&
           !partition->container &&
           partition->layout != INSTALLER_PARTITION_LAYOUT_EXTENDED &&
           partition->layout != INSTALLER_PARTITION_LAYOUT_LOGICAL &&
           partition->length_lba != 0U;
}

size_t installer_storage_partitions_count(
    const struct installer_discovery_report *report,
    uint32_t disk_id)
{
    size_t count = 0U;
    size_t index;

    if (report == NULL) {
        return 0U;
    }
    for (index = 0U; index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        const struct installer_discovery_partition *partition =
            &report->partitions[index];

        if (partition->disk_id == disk_id &&
            installer_storage_partition_releasable(partition)) {
            ++count;
        }
    }
    return count;
}

/*
 * The same arithmetic the dual-boot planner uses to place the appliance's
 * roles: the appliance's partitions start at the 1 MiB the layout planner
 * aligns to, and the last 34 sectors belong to the backup GPT.  The screen
 * shows this number, so an operator reads "there are N MiB" before the planner
 * is asked - and reads the planner's refusal with the same number in mind.
 */
#define INSTALLER_STORAGE_ALIGN_LBA UINT64_C(2048)
#define INSTALLER_STORAGE_GPT_TAIL_LBA UINT64_C(34)
#define INSTALLER_STORAGE_RESERVED_LBA UINT64_C(34)

static uint64_t installer_storage_align_up(uint64_t value)
{
    return (value + INSTALLER_STORAGE_ALIGN_LBA - 1U) &
           ~(INSTALLER_STORAGE_ALIGN_LBA - 1U);
}

uint64_t installer_storage_partitions_free_lba(
    const struct installer_discovery_report *report,
    uint32_t disk_id)
{
    struct {
        uint64_t first_lba;
        uint64_t end_lba;
    } sorted[INSTALLER_DISCOVERY_PARTITION_CAPACITY];
    const struct installer_discovery_disk *disk = NULL;
    uint32_t count = 0U;
    uint64_t usable_end;
    uint64_t cursor;
    uint64_t best = 0U;
    uint32_t pass;
    size_t index;

    if (report == NULL) {
        return 0U;
    }
    for (index = 0U; index < INSTALLER_DISCOVERY_DISK_CAPACITY; ++index) {
        if (report->disks[index].present &&
            report->disks[index].id == disk_id) {
            disk = &report->disks[index];
            break;
        }
    }
    if (disk == NULL || disk->partition_table != INSTALLER_PARTITION_TABLE_GPT ||
        disk->block_count <= INSTALLER_STORAGE_RESERVED_LBA) {
        return 0U;
    }
    usable_end = disk->block_count - INSTALLER_STORAGE_RESERVED_LBA;
    for (index = 0U; index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        const struct installer_discovery_partition *partition =
            &report->partitions[index];

        if (!partition->present || partition->disk_id != disk_id ||
            partition->length_lba == 0U ||
            partition->first_lba >
                UINT64_MAX - partition->length_lba) {
            continue;
        }
        sorted[count].first_lba = partition->first_lba;
        sorted[count].end_lba =
            partition->first_lba + partition->length_lba;
        ++count;
    }
    for (pass = 0U; pass < count; ++pass) {
        uint32_t other;

        for (other = 0U; other + 1U < count; ++other) {
            if (sorted[other].first_lba > sorted[other + 1U].first_lba) {
                uint64_t swap_first = sorted[other].first_lba;
                uint64_t swap_end = sorted[other].end_lba;

                sorted[other] = sorted[other + 1U];
                sorted[other + 1U].first_lba = swap_first;
                sorted[other + 1U].end_lba = swap_end;
            }
        }
    }
    cursor = INSTALLER_STORAGE_ALIGN_LBA;
    for (index = 0U; index <= count; ++index) {
        const uint64_t occupied_start =
            index < count ? sorted[index].first_lba : usable_end;
        const uint64_t occupied_end =
            index < count ? sorted[index].end_lba : usable_end;

        if (cursor < occupied_start) {
            uint64_t candidate_start = installer_storage_align_up(cursor);
            uint64_t candidate_end =
                occupied_start < usable_end ? occupied_start : usable_end;

            if (candidate_end > candidate_start &&
                candidate_end - candidate_start > best) {
                best = candidate_end - candidate_start;
            }
        }
        if (index < count && occupied_end > cursor) {
            cursor = occupied_end;
        }
    }
    return best;
}

void installer_storage_partitions_inventory(
    struct installer_storage_partition_model *model,
    const struct installer_discovery_report *report,
    uint32_t disk_id)
{
    uint64_t block_size;
    size_t index;

    if (model == NULL) {
        return;
    }
    memset(model, 0, sizeof(*model));
    model->disk_id = disk_id;
    model->selected = INSTALLER_STORAGE_NONE;
    if (report == NULL) {
        return;
    }
    block_size = installer_storage_disk_block_size(report, disk_id);
    if (block_size == 0U) {
        block_size = 512U;
    }
    model->free_lba = installer_storage_partitions_free_lba(report, disk_id);
    for (index = 0U; index < INSTALLER_DISCOVERY_PARTITION_CAPACITY &&
                     model->count < INSTALLER_STORAGE_PARTITIONS_MAX;
         ++index) {
        const struct installer_discovery_partition *partition =
            &report->partitions[index];
        struct installer_storage_partition *row;

        if (partition->disk_id != disk_id ||
            !installer_storage_partition_releasable(partition)) {
            continue;
        }
        row = &model->rows[model->count];
        row->id = partition->id;
        row->partition_index = partition->partition_index;
        row->first_lba = partition->first_lba;
        row->length_lba = partition->length_lba;
        row->kind = partition->kind;
        row->owner = partition->owner;
        row->layout = partition->layout;
        row->efi_system = partition->efi_system;
        row->container = partition->container;
        installer_storage_size_text(row->size_text, sizeof(row->size_text),
                                    block_size * partition->length_lba);
        installer_storage_copy_text(row->kind_text, sizeof(row->kind_text),
                                    installer_storage_kind_name(
                                        partition->kind));
        installer_storage_copy_text(row->owner_text,
                                    sizeof(row->owner_text),
                                    installer_storage_owner_name(
                                        partition->owner));
        installer_storage_copy_text(row->layout_text,
                                    sizeof(row->layout_text),
                                    installer_partition_layout_name(
                                        partition->layout) != NULL
                                        ? installer_partition_layout_name(
                                              partition->layout)
                                        : "PLAIN");
        model->count += 1U;
    }
}

bool installer_storage_partitions_move(
    struct installer_storage_partition_model *model, int delta)
{
    if (model == NULL || model->count == 0U || delta == 0) {
        return false;
    }
    if (delta < 0) {
        if (model->cursor == 0U) {
            return false;
        }
        model->cursor -= 1U;
        return true;
    }
    if (model->cursor + 1U >= model->count) {
        return false;
    }
    model->cursor += 1U;
    return true;
}

bool installer_storage_partitions_choose(
    struct installer_storage_partition_model *model)
{
    if (model == NULL || model->cursor >= model->count) {
        return false;
    }
    model->selected = (int32_t)model->cursor;
    return true;
}

const struct installer_storage_partition *installer_storage_partition_selected(
    const struct installer_storage_partition_model *model)
{
    if (model == NULL || model->selected == INSTALLER_STORAGE_NONE ||
        (size_t)model->selected >= model->count) {
        return NULL;
    }
    return &model->rows[model->selected];
}

size_t installer_storage_partition_lines(
    const struct installer_storage_partition_model *model,
    enum appliance_language language,
    char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES])
{
    struct installer_storage_screen screen;
    size_t row;
    const char *partition_title;
    const char *free_space_prefix;
    const char *mb_needed_middle;
    const char *mb_suffix;
    const char *partition_not_enough;
    const char *partition_none;
    const char *partition_unchanged;
    const char *partition_prefix;
    const char *partition_system_prefix;
    const char *partition_move_hint;
    const char *partition_keep_note;

    if (model == NULL || lines == NULL) {
        return 0U;
    }
    screen.lines = lines;
    screen.count = 0U;
    screen.scratch[0] = '\0';
    /*
     * ITEM-509 increment 18: the list's prose comes from the one catalog in the
     * caller's language; a key the catalog cannot answer is the named refusal.
     * The cursor, the device index, the capacity and the kind and layout names
     * stay machine values.
     */
    partition_title = appliance_language_string(
        language, "installer.storage.partition_title");
    free_space_prefix = appliance_language_string(
        language, "installer.storage.free_space_prefix");
    mb_needed_middle = appliance_language_string(
        language, "installer.storage.mb_needed_middle");
    mb_suffix = appliance_language_string(
        language, "installer.storage.mb_suffix");
    partition_not_enough = appliance_language_string(
        language, "installer.storage.partition_not_enough");
    partition_none = appliance_language_string(
        language, "installer.storage.partition_none");
    partition_unchanged = appliance_language_string(
        language, "installer.storage.partition_unchanged");
    partition_prefix = appliance_language_string(
        language, "installer.storage.partition_prefix");
    partition_system_prefix = appliance_language_string(
        language, "installer.storage.partition_system_prefix");
    partition_move_hint = appliance_language_string(
        language, "installer.storage.partition_move_hint");
    partition_keep_note = appliance_language_string(
        language, "installer.storage.partition_keep_note");
    if (partition_title == NULL || free_space_prefix == NULL ||
        mb_needed_middle == NULL || mb_suffix == NULL ||
        partition_not_enough == NULL || partition_none == NULL ||
        partition_unchanged == NULL || partition_prefix == NULL ||
        partition_system_prefix == NULL || partition_move_hint == NULL ||
        partition_keep_note == NULL) {
        struct installer_storage_line line = installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, "INSTALLER TRANSLATION MISSING");
        return screen.count;
    }
    {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, partition_title);
    }
    if (model->required_lba != 0U) {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, free_space_prefix);
        installer_storage_line_u64(
            &line, model->free_lba / INSTALLER_STORAGE_ALIGN_LBA);
        installer_storage_line_append(&line, mb_needed_middle);
        installer_storage_line_u64(
            &line, model->required_lba / INSTALLER_STORAGE_ALIGN_LBA);
        installer_storage_line_append(&line, mb_suffix);
        if (model->free_lba < model->required_lba) {
            line = installer_storage_screen_line(&screen);
            installer_storage_line_append(&line, partition_not_enough);
        }
    }
    if (model->count == 0U) {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, partition_none);
        line = installer_storage_screen_line(&screen);
        installer_storage_line_append(&line, partition_unchanged);
        return screen.count;
    }
    for (row = 0U; row < model->count; ++row) {
        const struct installer_storage_partition *partition =
            &model->rows[row];
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, row == model->cursor ? "> " : "  ");
        installer_storage_line_append(&line, partition_prefix);
        installer_storage_line_u64(&line, partition->partition_index);
        installer_storage_line_append(&line, "  ");
        installer_storage_line_append(&line, partition->size_text);
        installer_storage_line_append(&line, "  ");
        installer_storage_line_append(&line, partition->kind_text);
        installer_storage_line_append(&line, "  ");
        installer_storage_line_append(&line, partition->layout_text);
        line = installer_storage_screen_line(&screen);
        installer_storage_line_append(&line, partition_system_prefix);
        installer_storage_line_append(&line, partition->owner_text);
    }
    {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, partition_move_hint);
    }
    {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, partition_keep_note);
    }
    return screen.count;
}

size_t installer_storage_partition_warning_lines(
    const struct installer_storage_partition *partition,
    const struct installer_storage_partition_model *model,
    enum appliance_language language,
    char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES])
{
    struct installer_storage_screen screen;
    const char *warning_no_partition;
    const char *nothing_written;
    const char *warning_title;
    const char *partition_prefix;
    const char *size_prefix;
    const char *type_prefix;
    const char *layout_prefix;
    const char *system_prefix;
    const char *disk_prefix;
    const char *removed_note;
    const char *created_note;
    const char *others_note;
    const char *others_detail;
    const char *settings_note;
    const char *settings_detail;
    const char *not_written_yet;
    const char *footer;

    if (lines == NULL) {
        return 0U;
    }
    screen.lines = lines;
    screen.count = 0U;
    screen.scratch[0] = '\0';
    /*
     * ITEM-509 increment 18: the page's prose comes from the one catalog in the
     * caller's language; a key the catalog cannot answer is the named refusal.
     * The device index, the capacity and the kind, owner and layout names stay
     * machine values.
     */
    warning_no_partition = appliance_language_string(
        language, "installer.storage.partition_warning_no_partition");
    nothing_written = appliance_language_string(
        language, "installer.storage.nothing_written");
    warning_title = appliance_language_string(
        language, "installer.storage.partition_warning_title");
    partition_prefix = appliance_language_string(
        language, "installer.storage.partition_field_prefix");
    size_prefix = appliance_language_string(
        language, "installer.storage.size_prefix");
    type_prefix = appliance_language_string(
        language, "installer.storage.type_prefix");
    layout_prefix = appliance_language_string(
        language, "installer.storage.layout_prefix");
    system_prefix = appliance_language_string(
        language, "installer.storage.system_prefix");
    disk_prefix = appliance_language_string(
        language, "installer.storage.disk_field_prefix");
    removed_note = appliance_language_string(
        language, "installer.storage.partition_removed_note");
    created_note = appliance_language_string(
        language, "installer.storage.partition_created_note");
    others_note = appliance_language_string(
        language, "installer.storage.partition_others_note");
    others_detail = appliance_language_string(
        language, "installer.storage.partition_others_detail");
    settings_note = appliance_language_string(
        language, "installer.storage.settings_note");
    settings_detail = appliance_language_string(
        language, "installer.storage.settings_detail");
    not_written_yet = appliance_language_string(
        language, "installer.storage.not_written_yet");
    footer = appliance_language_string(
        language, "installer.storage.partition_footer");
    if (warning_no_partition == NULL || nothing_written == NULL ||
        warning_title == NULL || partition_prefix == NULL ||
        size_prefix == NULL || type_prefix == NULL || layout_prefix == NULL ||
        system_prefix == NULL || disk_prefix == NULL || removed_note == NULL ||
        created_note == NULL || others_note == NULL ||
        others_detail == NULL || settings_note == NULL ||
        settings_detail == NULL || not_written_yet == NULL || footer == NULL) {
        struct installer_storage_line line = installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, "INSTALLER TRANSLATION MISSING");
        return screen.count;
    }
    if (partition == NULL) {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, warning_no_partition);
        line = installer_storage_screen_line(&screen);
        installer_storage_line_append(&line, nothing_written);
        return screen.count;
    }
    {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, warning_title);
    }
    {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, partition_prefix);
        installer_storage_line_u64(&line, partition->partition_index);
        installer_storage_line_append(&line, size_prefix);
        installer_storage_line_append(&line, partition->size_text);
        installer_storage_line_append(&line, type_prefix);
        installer_storage_line_append(&line, partition->kind_text);
    }
    {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, layout_prefix);
        installer_storage_line_append(&line, partition->layout_text);
        installer_storage_line_append(&line, system_prefix);
        installer_storage_line_append(&line, partition->owner_text);
        if (model != NULL && model->disk_text[0] != '\0') {
            installer_storage_line_append(&line, disk_prefix);
            installer_storage_line_append(&line, model->disk_text);
        }
    }
    {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, removed_note);
        line = installer_storage_screen_line(&screen);
        installer_storage_line_append(&line, created_note);
    }
    {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, others_note);
        line = installer_storage_screen_line(&screen);
        installer_storage_line_append(&line, others_detail);
    }
    /* ITEM-502: the same erase notice the disk page carries. */
    {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, settings_note);
        line = installer_storage_screen_line(&screen);
        installer_storage_line_append(&line, settings_detail);
    }
    {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, not_written_yet);
    }
    {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, footer);
    }
    return screen.count;
}

size_t installer_storage_partition_record(
    const struct installer_storage_partition_model *model,
    char *buffer,
    size_t size)
{
    const struct installer_storage_partition *partition;
    struct installer_storage_line line;
    char record[INSTALLER_STORAGE_RECORD_BYTES + 1U];

    if (model == NULL || buffer == NULL || size == 0U) {
        return 0U;
    }
    installer_storage_line_reset(&line, record, sizeof(record));
    installer_storage_line_append(&line, "[INSTALLER-STORAGE] release disk=");
    installer_storage_line_u64(&line, model->disk_id);
    installer_storage_line_append(&line, " partitions=");
    installer_storage_line_u64(&line, (uint64_t)model->count);
    installer_storage_line_append(&line, " selected=");
    partition = installer_storage_partition_selected(model);
    if (partition == NULL) {
        installer_storage_line_append(&line, "NONE");
        installer_storage_line_append(&line, " size_lba=0 writes=0");
    } else {
        installer_storage_line_u64(&line, partition->partition_index);
        installer_storage_line_append(&line, " layout=");
        installer_storage_line_append(&line, partition->layout_text);
        installer_storage_line_append(&line, " size_lba=");
        installer_storage_line_u64(&line, partition->length_lba);
        installer_storage_line_append(&line, " writes=0");
    }
    if (line.length == 0U || line.length > INSTALLER_STORAGE_RECORD_BYTES ||
        line.length + 1U > size) {
        return 0U;
    }
    memcpy(buffer, record, line.length + 1U);
    return line.length;
}

/*
 * ITEM-514: the install schemes.
 *
 * The scheme model is a value machine like the disk and partition models: it
 * reads the report, exposes four rows with their availability and their stable
 * refusal names, and writes nothing.  The required footprint and the smallest
 * media are caller-supplied numbers because the contract that owns them lives
 * one layer up; this file still includes no layout, device or installer header.
 */

const char *installer_storage_scheme_name(enum installer_storage_scheme scheme)
{
    switch (scheme) {
    case INSTALLER_STORAGE_SCHEME_ALONGSIDE:
        return "ALONGSIDE";
    case INSTALLER_STORAGE_SCHEME_FREE_SPACE:
        return "FREE_SPACE";
    case INSTALLER_STORAGE_SCHEME_ERASE:
        return "ERASE";
    case INSTALLER_STORAGE_SCHEME_MANUAL:
        return "MANUAL";
    case INSTALLER_STORAGE_SCHEME_NONE:
    default:
        return "NONE";
    }
}

const char *installer_storage_scheme_text(enum installer_storage_scheme scheme)
{
    switch (scheme) {
    case INSTALLER_STORAGE_SCHEME_ALONGSIDE:
        return "alongside";
    case INSTALLER_STORAGE_SCHEME_FREE_SPACE:
        return "free-space";
    case INSTALLER_STORAGE_SCHEME_ERASE:
        return "erase";
    case INSTALLER_STORAGE_SCHEME_MANUAL:
        return "manual";
    case INSTALLER_STORAGE_SCHEME_NONE:
    default:
        return "none";
    }
}

static const char *installer_storage_scheme_name_key(
    enum installer_storage_scheme scheme)
{
    switch (scheme) {
    case INSTALLER_STORAGE_SCHEME_ALONGSIDE:
        return "installer.scheme.alongside";
    case INSTALLER_STORAGE_SCHEME_FREE_SPACE:
        return "installer.scheme.free_space";
    case INSTALLER_STORAGE_SCHEME_ERASE:
        return "installer.scheme.erase";
    case INSTALLER_STORAGE_SCHEME_MANUAL:
        return "installer.scheme.manual";
    default:
        return NULL;
    }
}

static const char *installer_storage_scheme_detail_key(
    enum installer_storage_scheme scheme)
{
    switch (scheme) {
    case INSTALLER_STORAGE_SCHEME_ALONGSIDE:
        return "installer.scheme.alongside_detail";
    case INSTALLER_STORAGE_SCHEME_FREE_SPACE:
        return "installer.scheme.free_space_detail";
    case INSTALLER_STORAGE_SCHEME_ERASE:
        return "installer.scheme.erase_detail";
    case INSTALLER_STORAGE_SCHEME_MANUAL:
        return "installer.scheme.manual_detail";
    default:
        return NULL;
    }
}

/*
 * "A system is here" for the scheme list: a partition that is not an EFI
 * System Partition is somebody's data, and a discovered system is a system
 * whatever its partition looked like.  It mirrors the rule the flow applies
 * when no operator chose, so the default scheme names the same disk the
 * unattended path would have preserved.
 */
static bool installer_storage_disk_has_foreign_system(
    const struct installer_discovery_report *report,
    uint32_t disk_id)
{
    size_t index;

    for (index = 0U; index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        const struct installer_discovery_partition *partition =
            &report->partitions[index];

        if (partition->present && partition->disk_id == disk_id &&
            !partition->efi_system) {
            return true;
        }
    }
    for (index = 0U; index < INSTALLER_DISCOVERY_OS_CAPACITY; ++index) {
        const struct installer_discovery_os *os = &report->oss[index];
        size_t partition_index;

        if (!os->present) {
            continue;
        }
        for (partition_index = 0U;
             partition_index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
             ++partition_index) {
            const struct installer_discovery_partition *partition =
                &report->partitions[partition_index];

            if (partition->present && partition->id == os->partition_id &&
                partition->disk_id == disk_id) {
                return true;
            }
        }
    }
    return false;
}

void installer_storage_schemes_inventory(
    struct installer_storage_scheme_model *model,
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    enum installer_storage_scheme declared,
    uint64_t required_lba,
    uint64_t minimum_media_lba,
    bool contents_unknown)
{
    struct installer_storage_model disks;
    size_t index;
    uint32_t scan;

    if (model == NULL) {
        return;
    }
    memset(model, 0, sizeof(*model));
    model->disk_id = disk_id;
    model->selected = INSTALLER_STORAGE_SCHEME_NONE;
    model->declared = declared;
    model->required_lba = required_lba;
    model->minimum_media_lba = minimum_media_lba;
    model->contents_unknown = contents_unknown;
    if (report == NULL) {
        return;
    }
    /*
     * The disk the scheme belongs to goes through the disk model's own
     * fail-closed rule: a disk whose identity or geometry was not read cannot
     * grow a scheme list here either.
     */
    installer_storage_inventory(&disks, report);
    if (!installer_storage_select_id(&disks, disk_id)) {
        return;
    }
    model->disk = *installer_storage_selected(&disks);
    model->free_lba = installer_storage_partitions_free_lba(report, disk_id);
    model->releasable_count =
        (uint32_t)installer_storage_partitions_count(report, disk_id);
    model->has_existing_system =
        installer_storage_disk_has_foreign_system(report, disk_id);
    for (scan = 0U; scan < INSTALLER_DISCOVERY_OS_CAPACITY; ++scan) {
        if (report->oss[scan].present) {
            model->existing_system_count += 1U;
        }
    }
    for (scan = 0U; scan < report->disk_count &&
                     scan < INSTALLER_DISCOVERY_DISK_CAPACITY; ++scan) {
        if (report->disks[scan].present && report->disks[scan].id == disk_id) {
            model->media_lba = report->disks[scan].block_count;
            break;
        }
    }
    /*
     * The four rows, in the order the screen lists them.  Each carries the
     * stable refusal name that says why it cannot be used; an available row
     * carries NULL, and no row is ever dropped - a scheme that disappears is a
     * scheme the operator is wrong about.
     */
    index = 0U;
    model->rows[index].scheme = INSTALLER_STORAGE_SCHEME_ALONGSIDE;
    model->rows[index].required_lba = required_lba;
    model->rows[index].available_lba = model->free_lba;
    if (!model->has_existing_system) {
        model->rows[index].refusal = "NO_EXISTING_SYSTEM";
    } else if (!model->disk.has_efi_system_partition) {
        /*
         * A preserving install reuses the system's own EFI System Partition;
         * it never creates a second one, so a disk without one cannot carry the
         * scheme - and saying so here is the named refusal, not a plan that
         * fails later.
         */
        model->rows[index].refusal = "NO_EFI_SYSTEM_PARTITION";
    } else if (required_lba != 0U && model->free_lba < required_lba) {
        model->rows[index].refusal = "NOT_ENOUGH_FREE_SPACE";
    }
    model->rows[index].available = model->rows[index].refusal == NULL;
    index += 1U;
    model->rows[index].scheme = INSTALLER_STORAGE_SCHEME_FREE_SPACE;
    model->rows[index].required_lba = required_lba;
    model->rows[index].available_lba = model->free_lba;
    if (model->free_lba == 0U) {
        model->rows[index].refusal = "NO_FREE_SPACE";
    } else if (required_lba != 0U && model->free_lba < required_lba) {
        model->rows[index].refusal = "NOT_ENOUGH_FREE_SPACE";
    } else if (!model->disk.has_efi_system_partition) {
        model->rows[index].refusal = "NO_EFI_SYSTEM_PARTITION";
    }
    model->rows[index].available = model->rows[index].refusal == NULL;
    index += 1U;
    model->rows[index].scheme = INSTALLER_STORAGE_SCHEME_ERASE;
    model->rows[index].required_lba = minimum_media_lba;
    model->rows[index].available_lba = model->media_lba;
    if (minimum_media_lba != 0U && model->media_lba < minimum_media_lba) {
        model->rows[index].refusal = "MEDIA_TOO_SMALL";
    }
    model->rows[index].available = model->rows[index].refusal == NULL;
    index += 1U;
    model->rows[index].scheme = INSTALLER_STORAGE_SCHEME_MANUAL;
    model->rows[index].required_lba = required_lba;
    model->rows[index].available_lba = 0U;
    /*
     * The manual scheme opens the ITEM-501 partition screen, which needs a GPT
     * table to rewrite and the shared EFI System Partition to keep; a disk
     * without either has nothing to offer here, and the row says so by name.
     */
    if (model->releasable_count == 0U ||
        model->disk.partition_table != INSTALLER_PARTITION_TABLE_GPT ||
        !model->disk.has_efi_system_partition) {
        model->rows[index].refusal = "NO_RELEASABLE_PARTITION";
    }
    model->rows[index].available = model->rows[index].refusal == NULL;
    index += 1U;
    model->count = index;

    /*
     * The default is the declaration when the medium made one, otherwise the
     * disk's own suggestion: preserve a system that is here, erase a disk that
     * carries none.  It is a cursor position, not a decision - `selected`
     * stays NONE and only the operator's ENTER moves it.
     */
    if (declared != INSTALLER_STORAGE_SCHEME_NONE) {
        model->default_scheme = declared;
    } else if (model->has_existing_system) {
        model->default_scheme = INSTALLER_STORAGE_SCHEME_ALONGSIDE;
    } else {
        model->default_scheme = INSTALLER_STORAGE_SCHEME_ERASE;
    }
    for (index = 0U; index < model->count; ++index) {
        if (model->rows[index].scheme == model->default_scheme) {
            model->cursor = index;
            break;
        }
    }
}

bool installer_storage_schemes_move(struct installer_storage_scheme_model *model,
                                    int delta)
{
    if (model == NULL || model->count == 0U || delta == 0) {
        return false;
    }
    if (delta < 0) {
        if (model->cursor == 0U) {
            return false;
        }
        model->cursor -= 1U;
        return true;
    }
    if (model->cursor + 1U >= model->count) {
        return false;
    }
    model->cursor += 1U;
    return true;
}

bool installer_storage_schemes_choose(struct installer_storage_scheme_model *model)
{
    if (model == NULL || model->cursor >= model->count) {
        return false;
    }
    if (!model->rows[model->cursor].available) {
        /* The refusal is the answer; no other scheme is chosen behind it. */
        return false;
    }
    model->selected = model->rows[model->cursor].scheme;
    return true;
}

const struct installer_storage_scheme_row *installer_storage_scheme_row_at(
    const struct installer_storage_scheme_model *model, size_t index)
{
    if (model == NULL || index >= model->count) {
        return NULL;
    }
    return &model->rows[index];
}

const struct installer_storage_scheme_row *installer_storage_scheme_row_cursor(
    const struct installer_storage_scheme_model *model)
{
    if (model == NULL || model->count == 0U || model->cursor >= model->count) {
        return NULL;
    }
    return &model->rows[model->cursor];
}

const struct installer_storage_scheme_row *installer_storage_scheme_selected(
    const struct installer_storage_scheme_model *model)
{
    size_t index;

    if (model == NULL || model->selected == INSTALLER_STORAGE_SCHEME_NONE) {
        return NULL;
    }
    for (index = 0U; index < model->count; ++index) {
        if (model->rows[index].scheme == model->selected) {
            return &model->rows[index];
        }
    }
    return NULL;
}

size_t installer_storage_scheme_lines(
    const struct installer_storage_scheme_model *model,
    enum appliance_language language,
    char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES])
{
    struct installer_storage_screen screen;
    const char *title;
    const char *disk_prefix;
    const char *free_space_prefix;
    const char *mb_needed_middle;
    const char *mb_suffix;
    const char *move_hint;
    const char *unavailable_prefix;
    size_t row;

    if (model == NULL || lines == NULL || model->count == 0U) {
        return 0U;
    }
    screen.lines = lines;
    screen.count = 0U;
    screen.scratch[0] = '\0';
    title = appliance_language_string(language, "installer.scheme.title");
    disk_prefix = appliance_language_string(language,
                                            "installer.storage.disk_prefix");
    free_space_prefix = appliance_language_string(
        language, "installer.storage.free_space_prefix");
    mb_needed_middle = appliance_language_string(
        language, "installer.storage.mb_needed_middle");
    mb_suffix = appliance_language_string(language,
                                          "installer.storage.mb_suffix");
    move_hint = appliance_language_string(language, "installer.scheme.move_hint");
    unavailable_prefix = appliance_language_string(
        language, "installer.scheme.unavailable_prefix");
    if (title == NULL || disk_prefix == NULL || free_space_prefix == NULL ||
        mb_needed_middle == NULL || mb_suffix == NULL || move_hint == NULL ||
        unavailable_prefix == NULL) {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, "INSTALLER TRANSLATION MISSING");
        return screen.count;
    }
    {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, title);
    }
    {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, disk_prefix);
        if (model->disk.model[0] != '\0') {
            installer_storage_line_append(&line, model->disk.model);
            installer_storage_line_append(&line, "  ");
        }
        installer_storage_line_append(&line, model->disk.size_text);
        installer_storage_line_append(&line, "  ");
        installer_storage_line_append(&line, model->disk.table_text);
        if (model->contents_unknown) {
            installer_storage_line_append(&line, "  UNREADABLE");
        }
    }
    if (model->required_lba != 0U) {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, free_space_prefix);
        installer_storage_line_u64(
            &line, model->free_lba / INSTALLER_STORAGE_ALIGN_LBA);
        installer_storage_line_append(&line, mb_needed_middle);
        installer_storage_line_u64(
            &line, model->required_lba / INSTALLER_STORAGE_ALIGN_LBA);
        installer_storage_line_append(&line, mb_suffix);
    }
    for (row = 0U; row < model->count; ++row) {
        const struct installer_storage_scheme_row *entry = &model->rows[row];
        const char *name_key = installer_storage_scheme_name_key(entry->scheme);
        const char *detail_key =
            installer_storage_scheme_detail_key(entry->scheme);
        const char *name = name_key != NULL
                               ? appliance_language_string(language, name_key)
                               : NULL;
        const char *detail = detail_key != NULL
                                 ? appliance_language_string(language, detail_key)
                                 : NULL;

        if (name == NULL || detail == NULL) {
            struct installer_storage_line line =
                installer_storage_screen_line(&screen);

            installer_storage_line_append(&line,
                                          "INSTALLER TRANSLATION MISSING");
            return screen.count;
        }
        {
            struct installer_storage_line line =
                installer_storage_screen_line(&screen);

            installer_storage_line_append(&line,
                                          row == model->cursor ? "> " : "  ");
            installer_storage_line_append(&line, name);
            /*
             * ITEM-514: every row carries the volume it needs, so the operator
             * reads the size beside the scheme and not only in the header.
             */
            if (entry->required_lba != 0U) {
                installer_storage_line_append(&line, "   ");
                installer_storage_line_u64(
                    &line, entry->required_lba / INSTALLER_STORAGE_ALIGN_LBA);
                installer_storage_line_append(&line, mb_suffix);
            }
            if (!entry->available) {
                installer_storage_line_append(&line, unavailable_prefix);
                installer_storage_line_append(&line, entry->refusal);
            }
        }
        {
            struct installer_storage_line line =
                installer_storage_screen_line(&screen);

            installer_storage_line_append(&line, detail);
        }
    }
    {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, move_hint);
    }
    return screen.count;
}

size_t installer_storage_scheme_refusal_lines(
    const struct installer_storage_scheme_model *model,
    enum appliance_language language,
    char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES])
{
    struct installer_storage_screen screen;
    const struct installer_storage_scheme_row *row;
    const char *title;
    const char *reason_prefix;
    const char *scheme_prefix;
    const char *free_space_prefix;
    const char *mb_needed_middle;
    const char *mb_suffix;
    const char *hint;
    const char *footer;

    if (model == NULL || lines == NULL) {
        return 0U;
    }
    screen.lines = lines;
    screen.count = 0U;
    screen.scratch[0] = '\0';
    row = installer_storage_scheme_row_cursor(model);
    title = appliance_language_string(language, "installer.scheme.refusal_title");
    reason_prefix = appliance_language_string(
        language, "installer.scheme.refusal_reason");
    scheme_prefix = appliance_language_string(
        language, "installer.scheme.summary_scheme_prefix");
    free_space_prefix = appliance_language_string(
        language, "installer.storage.free_space_prefix");
    mb_needed_middle = appliance_language_string(
        language, "installer.storage.mb_needed_middle");
    mb_suffix = appliance_language_string(language,
                                          "installer.storage.mb_suffix");
    hint = appliance_language_string(language, "installer.scheme.refusal_hint");
    footer = appliance_language_string(language,
                                       "installer.scheme.refusal_footer");
    if (title == NULL || reason_prefix == NULL || scheme_prefix == NULL ||
        free_space_prefix == NULL || mb_needed_middle == NULL ||
        mb_suffix == NULL || hint == NULL || footer == NULL) {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, "INSTALLER TRANSLATION MISSING");
        return screen.count;
    }
    {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, title);
    }
    if (row != NULL) {
        const char *name_key = installer_storage_scheme_name_key(row->scheme);
        const char *name = name_key != NULL
                               ? appliance_language_string(language, name_key)
                               : NULL;

        {
            struct installer_storage_line line =
                installer_storage_screen_line(&screen);

            installer_storage_line_append(&line, scheme_prefix);
            installer_storage_line_append(&line, name != NULL ? name
                                                              : "UNKNOWN");
        }
        {
            struct installer_storage_line line =
                installer_storage_screen_line(&screen);

            installer_storage_line_append(&line, reason_prefix);
            installer_storage_line_append(
                &line, row->refusal != NULL ? row->refusal : "NONE");
        }
        if (row->required_lba != 0U || row->available_lba != 0U) {
            struct installer_storage_line line =
                installer_storage_screen_line(&screen);

            installer_storage_line_append(&line, free_space_prefix);
            installer_storage_line_u64(
                &line, row->available_lba / INSTALLER_STORAGE_ALIGN_LBA);
            installer_storage_line_append(&line, mb_needed_middle);
            installer_storage_line_u64(
                &line, row->required_lba / INSTALLER_STORAGE_ALIGN_LBA);
            installer_storage_line_append(&line, mb_suffix);
        }
    }
    {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, hint);
        line = installer_storage_screen_line(&screen);
        installer_storage_line_append(&line, footer);
    }
    return screen.count;
}

size_t installer_storage_refusal_lines(
    const char *reason,
    const struct appliance_storage_report *observed,
    const struct installer_refusal_evidence *evidence,
    enum appliance_language language,
    char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES])
{
    struct installer_storage_screen screen;
    static char observed_lines[APPLIANCE_STORAGE_REPORT_LINES_MAX]
                              [APPLIANCE_STORAGE_REPORT_LINE_BYTES];
    const char *title;
    const char *reason_prefix;
    const char *medium;
    const char *press_key;
    const char *medium_state = "UNCHECKED";
    const char *medium_reason = "NOT_PROBED";
    const char *journal_state = "UNBOUND";
    const char *journal_reason = "NONE";
    uint32_t journal_pending = 0U;
    uint32_t journal_dropped = 0U;
    uint32_t journal_refused = 0U;
    size_t tail_count;
    size_t observed_count;
    size_t observed_budget;
    size_t index;

    if (lines == NULL) {
        return 0U;
    }
    screen.lines = lines;
    screen.count = 0U;
    screen.scratch[0] = '\0';

    tail_count = evidence != NULL ? evidence->record_count : 0U;
    if (tail_count > INSTALLER_STORAGE_REFUSAL_TAIL_MAX) {
        tail_count = INSTALLER_STORAGE_REFUSAL_TAIL_MAX;
    }
    if (evidence != NULL) {
        if (evidence->medium_state != NULL) {
            medium_state = evidence->medium_state;
        }
        if (evidence->medium_reason != NULL) {
            medium_reason = evidence->medium_reason;
        }
        if (evidence->journal_state != NULL) {
            journal_state = evidence->journal_state;
        }
        if (evidence->journal_reason != NULL) {
            journal_reason = evidence->journal_reason;
        }
        journal_pending = evidence->journal_pending;
        journal_dropped = evidence->journal_dropped;
        journal_refused = evidence->journal_refused;
    }
    /*
     * Reserve the seven fixed lines - title, refusal, medium, the two status
     * lines, the tail header and the way out - and the tail itself.  The
     * observed report takes what is left, so it is clipped rather than allowed
     * to push the way out off the page.
     */
    if (tail_count > INSTALLER_STORAGE_LINES_MAX - 7U) {
        tail_count = INSTALLER_STORAGE_LINES_MAX - 7U;
    }
    observed_budget = INSTALLER_STORAGE_LINES_MAX - 7U - tail_count;

    title = appliance_language_string(language, "installer.refusal.title");
    reason_prefix = appliance_language_string(
        language, "installer.refusal.reason_prefix");
    medium = appliance_language_string(language, "installer.refusal.medium");
    press_key = appliance_language_string(
        language, "installer.refusal.press_key");
    if (title == NULL || reason_prefix == NULL || medium == NULL ||
        press_key == NULL) {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, "INSTALLER TRANSLATION MISSING");
        return screen.count;
    }

    {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, title);
    }
    {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, reason_prefix);
        installer_storage_line_append(&line, reason != NULL ? reason : "NONE");
    }
    if (medium[0] != '\0') {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, medium);
    }
    /*
     * F-6: the section the operator did not see.  An empty report used to mean
     * a silent page; the page now says `state=NOT_PROBED` itself, so "nobody
     * looked" and "nothing was printed" cannot read the same.
     */
    observed_count = appliance_storage_report_lines(observed, language,
                                                    observed_lines);
    if (observed_count == 0U) {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line,
                                      "[STORAGE-REPORT] state=NOT_PROBED");
    } else {
        for (index = 0U;
             index < observed_count && index < observed_budget;
             ++index) {
            struct installer_storage_line line =
                installer_storage_screen_line(&screen);

            installer_storage_line_append(&line, observed_lines[index]);
        }
    }
    {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, "[INSTALL-MEDIUM] state=");
        installer_storage_line_append(&line, medium_state);
        installer_storage_line_append(&line, " reason=");
        installer_storage_line_append(&line, medium_reason);
    }
    {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, "[INSTALL-LOG] state=");
        installer_storage_line_append(&line, journal_state);
        installer_storage_line_append(&line, " reason=");
        installer_storage_line_append(&line, journal_reason);
        installer_storage_line_append(&line, " pending=");
        installer_storage_line_u64(&line, journal_pending);
        installer_storage_line_append(&line, " dropped=");
        installer_storage_line_u64(&line, journal_dropped);
        installer_storage_line_append(&line, " refused=");
        installer_storage_line_u64(&line, journal_refused);
    }
    {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, "[INSTALL-LOG-TAIL] shown=");
        installer_storage_line_u64(&line, tail_count);
    }
    /* `records` is newest first; the page reads the tail oldest first. */
    for (index = 0U; index < tail_count; ++index) {
        const char *record = evidence != NULL
                                 ? evidence->records[tail_count - 1U - index]
                                 : NULL;
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, record != NULL ? record : "");
    }
    {
        struct installer_storage_line line =
            installer_storage_screen_line(&screen);

        installer_storage_line_append(&line, press_key);
    }
    return screen.count;
}

size_t installer_storage_scheme_erase_warning_lines(
    const struct installer_storage_scheme_model *model,
    enum appliance_language language,
    char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES])
{
    size_t count;

    if (model == NULL || lines == NULL) {
        return 0U;
    }
    count = installer_storage_warning_lines(&model->disk, language, lines);
    if (model->contents_unknown && count > 0U &&
        count < INSTALLER_STORAGE_LINES_MAX) {
        const char *unknown = appliance_language_string(
            language, "installer.scheme.unknown_contents");

        if (unknown != NULL) {
            size_t index;

            /* The footer stays last: the notice goes before it. */
            for (index = count; index > count - 1U; --index) {
                installer_storage_copy_text(lines[index],
                                            INSTALLER_STORAGE_LINE_BYTES,
                                            lines[index - 1U]);
            }
            installer_storage_copy_text(lines[count - 1U],
                                        INSTALLER_STORAGE_LINE_BYTES,
                                        unknown);
            count += 1U;
        }
    }
    return count;
}

size_t installer_storage_schemes_record(
    const struct installer_storage_scheme_model *model,
    char *buffer,
    size_t size)
{
    struct installer_storage_line line;
    char record[INSTALLER_STORAGE_RECORD_BYTES + 1U];

    if (model == NULL || buffer == NULL || size == 0U) {
        return 0U;
    }
    installer_storage_line_reset(&line, record, sizeof(record));
    installer_storage_line_append(&line, "[INSTALLER-STORAGE] scheme disk=");
    installer_storage_line_u64(&line, model->disk_id);
    installer_storage_line_append(&line, " declared=");
    installer_storage_line_append(
        &line, installer_storage_scheme_name(model->declared));
    installer_storage_line_append(&line, " chosen=");
    installer_storage_line_append(
        &line, installer_storage_scheme_name(model->selected));
    installer_storage_line_append(&line, " cursor=");
    installer_storage_line_append(
        &line, model->count != 0U
                   ? installer_storage_scheme_name(model->rows[model->cursor].scheme)
                   : "NONE");
    installer_storage_line_append(&line, " available=");
    installer_storage_line_append(
        &line,
        model->count != 0U && !model->rows[model->cursor].available ? "0"
                                                                    : "1");
    installer_storage_line_append(&line, " free_lba=");
    installer_storage_line_u64(&line, model->free_lba);
    installer_storage_line_append(&line, " required_lba=");
    installer_storage_line_u64(&line, model->required_lba);
    installer_storage_line_append(&line, " unknown_contents=");
    installer_storage_line_append(&line, model->contents_unknown ? "1" : "0");
    installer_storage_line_append(&line, " writes=0");
    if (line.length == 0U || line.length > INSTALLER_STORAGE_RECORD_BYTES ||
        line.length + 1U > size) {
        return 0U;
    }
    memcpy(buffer, record, line.length + 1U);
    return line.length;
}
