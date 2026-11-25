#include "installer_storage_screen.h"

#include <stddef.h>

#include "appliance_language.h"
#include "appliance_storage_report.h"
#include "console.h"
#include "display.h"
#include "installer_storage.h"
#include "installer_storage_summary.h"
#include "keyboard.h"
#include "local_input.h"
#include "arch/x86_64/timer.h"
#include "serial.h"

/*
 * ITEM-489: the storage screen on the machine.
 *
 * One renderer, two backends, and one operator decision.  The screen lists the
 * disks the discovery report holds, by the identity the hardware reported, and
 * the operator moves with the keys the rest of the product already uses and
 * chooses with ENTER.  Nothing is chosen for them: the model starts at NONE and
 * `installer_storage_choose()` refuses a disk whose identity was not read.
 *
 * This file writes no disk: it draws, it reads keys, and it publishes who chose
 * what.  Partitioning and the install executor are the following items, and the
 * screen says so.
 */

/* The installer's palette, shared with the foundation screen. */
#define STORAGE_BACKGROUND UINT32_C(0x00101820)
#define STORAGE_TITLE_COLOUR UINT32_C(0x00F0F4F8)
#define STORAGE_TEXT_COLOUR UINT32_C(0x00C0D0E0)
#define STORAGE_SELECTED_COLOUR UINT32_C(0x00FFD070)
#define STORAGE_WARNING_COLOUR UINT32_C(0x00FF9090)
#define STORAGE_MARGIN_PIXELS 16U
#define STORAGE_LINE_PIXELS 10U
/* One tick is ten milliseconds: the timer the whole kernel uses runs at 100 Hz. */
#define STORAGE_MS_PER_TICK 10U
/*
 * ITEM-500: how long the refusal page waits for a person before the machine
 * continues its ordinary boot.  Long enough to read the reason and press a key,
 * short enough that a machine whose operator walked away is not held forever.
 */
#define INSTALLER_STORAGE_REFUSAL_WAIT_MS 300000U

static void installer_storage_serial_lines(
    char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES],
    size_t count)
{
    const uint64_t serial_flags = serial_sequence_begin();
    size_t index;

    for (index = 0U; index < count; ++index) {
        serial_write(lines[index]);
        serial_write("\n");
    }
    serial_sequence_end(serial_flags);
}

static void installer_storage_paint(
    char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES],
    size_t count)
{
    const uint32_t scale = display_scale() > 0U ? display_scale() : 1U;
    const uint64_t margin = (uint64_t)STORAGE_MARGIN_PIXELS * scale;
    uint64_t y = margin;
    size_t index;

    display_clear(STORAGE_BACKGROUND);
    for (index = 0U; index < count; ++index) {
        uint32_t colour = STORAGE_TEXT_COLOUR;

        if (index == 0U) {
            colour = STORAGE_TITLE_COLOUR;
        } else if (lines[index][0] == '>') {
            colour = STORAGE_SELECTED_COLOUR;
        } else if (lines[index][0] != ' ' && lines[index][0] != '\0') {
            colour = STORAGE_WARNING_COLOUR;
        }
        display_text(margin, y, lines[index], colour, scale);
        y += (uint64_t)STORAGE_LINE_PIXELS * scale;
    }
    display_present();
}

static void installer_storage_show(
    const struct installer_storage_model *model,
    enum appliance_language language)
{
    static char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES];
    const size_t count = installer_storage_lines(model, language, lines);

    if (display_framebuffer_usable() && count > 0U) {
        installer_storage_paint(lines, count);
    } else if (count > 0U) {
        size_t index;

        for (index = 0U; index < count; ++index) {
            console_write(lines[index]);
            console_write("\n");
        }
    }
    if (count > 0U) {
        installer_storage_serial_lines(lines, count);
    }
}

static void installer_storage_show_lines(
    char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES],
    size_t count)
{
    if (display_framebuffer_usable() && count > 0U) {
        installer_storage_paint(lines, count);
    } else if (count > 0U) {
        size_t index;

        for (index = 0U; index < count; ++index) {
            console_write(lines[index]);
            console_write("\n");
        }
    }
    if (count > 0U) {
        installer_storage_serial_lines(lines, count);
    }
}

static void installer_storage_record_publish(
    const struct installer_storage_model *model)
{
    char record[INSTALLER_STORAGE_RECORD_BYTES];
    const size_t length = installer_storage_record(model, record, sizeof(record));

    if (length == 0U) {
        return;
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();

        serial_write(record);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    console_publish_journal(record, length);
}

void installer_storage_publish(const struct installer_discovery_report *report)
{
    struct installer_storage_model model;

    installer_storage_inventory(&model, report);
    installer_storage_record_publish(&model);
}

/*
 * Hand the caller its own copy of the chosen row.  The model is the session's
 * own local, so a pointer into it would dangle the moment the screen returns -
 * and a caller that writes a disk must not be handed a dangling target.
 */
static bool installer_storage_copy_choice(
    const struct installer_storage_model *model,
    struct installer_storage_disk *chosen)
{
    const struct installer_storage_disk *selected =
        installer_storage_selected(model);

    if (selected == NULL || chosen == NULL) {
        return false;
    }
    *chosen = *selected;
    return true;
}

/* A deadline of zero is "no deadline": the mandatory screen waits for a person. */
static bool installer_storage_expired(uint64_t deadline)
{
    return deadline != 0U && timer_ticks() >= deadline;
}

void installer_storage_publish_choice(
    const struct installer_discovery_report *report,
    uint32_t disk_id)
{
    struct installer_storage_model model;

    installer_storage_inventory(&model, report);
    /*
     * The choice is applied through the model's own fail-closed rule, so a disk
     * whose identity or geometry was not read cannot be published as chosen even
     * by a caller that asks for it by id.
     */
    if (!installer_storage_select_id(&model, disk_id)) {
        return;
    }
    installer_storage_record_publish(&model);
}

enum installer_storage_decision installer_storage_session_choose_note(
    const struct installer_discovery_report *report,
    uint32_t timeout_ms,
    struct installer_storage_disk *chosen,
    const char *note,
    enum appliance_language language)
{
    struct installer_storage_model model;
    /* Zero means no deadline; anything else is a bounded session. */
    const uint64_t deadline =
        timeout_ms == INSTALLER_STORAGE_NO_DEADLINE
            ? 0U
            : timer_ticks() + (uint64_t)(timeout_ms / STORAGE_MS_PER_TICK);

    installer_storage_inventory(&model, report);
    installer_storage_set_note(&model, note);
    installer_storage_show(&model, language);
    installer_storage_record_publish(&model);

    while (!installer_storage_expired(deadline)) {
        struct keyboard_event event;

        /*
         * The same servicing the ordinary loop performs every turn: the USB HID
         * reports are merged into the hardware-independent queue here, so a
         * machine whose only keyboard is on USB can drive this screen exactly
         * like a machine with an i8042.
         */
        local_input_service();

        /*
         * The hardware-independent input layer: the PS/2 keyboard and the USB
         * HID keyboard the xHCI driver already binds arrive as the same events,
         * so this screen answers to both without knowing either.
         *
         * What a key *means* is decided by the model's pure
         * `installer_storage_key_action()`, which is proven on the host: the
         * arrow keys and the `w`/`s` the screen's own hint names both move, and
         * a key nobody defined does nothing.
         *
         * ITEM-514: ENTER names the disk and nothing more.  The model still
         * confirms the choice - a confirmed disk is one the operator is looking
         * at - but the destructive page and the final decision belong to the
         * scheme screen and the summary page that follow, where they can say
         * what will actually happen to this disk instead of warning about an
         * install the operator has not chosen yet.
         */
        while (local_input_pop_event(&event)) {
            switch (installer_storage_key_action(event.type, event.character)) {
            case INSTALLER_STORAGE_KEY_UP:
                if (installer_storage_move(&model, -1)) {
                    installer_storage_show(&model, language);
                }
                break;
            case INSTALLER_STORAGE_KEY_DOWN:
                if (installer_storage_move(&model, 1)) {
                    installer_storage_show(&model, language);
                }
                break;
            case INSTALLER_STORAGE_KEY_CHOOSE:
                if (installer_storage_choose(&model) &&
                    installer_storage_confirm(&model) &&
                    installer_storage_copy_choice(&model, chosen)) {
                    installer_storage_record_publish(&model);
                    return INSTALLER_STORAGE_DECISION_CONFIRMED;
                }
                break;
            case INSTALLER_STORAGE_KEY_LEAVE:
                /* `q` leaves the list with nothing confirmed, whatever the
                 * cursor was on: leaving is not choosing. */
                installer_storage_record_publish(&model);
                return INSTALLER_STORAGE_DECISION_NONE;
            case INSTALLER_STORAGE_KEY_NONE:
            default:
                break;
            }
        }
        /*
         * Nothing else happens here, and that is deliberate: this is a screen,
         * not a scheduler.  The loop ends when the operator chooses, when they
         * leave, or when a bounded session expires - and a mandatory session
         * never expires, because the entry that opens it is the entry an
         * operator chose.
         */
    }
    installer_storage_record_publish(&model);
    return INSTALLER_STORAGE_DECISION_NONE;
}

enum installer_storage_decision installer_storage_session_choose(
    const struct installer_discovery_report *report,
    uint32_t timeout_ms,
    struct installer_storage_disk *chosen,
    enum appliance_language language)
{
    return installer_storage_session_choose_note(report, timeout_ms, chosen,
                                                 NULL, language);
}

/*
 * ITEM-500: the page a machine that cannot install shows the person who asked
 * it to install.  It names the refusal the records name, says that this medium
 * cannot install on this machine, and waits - so a refused operator entry is
 * something the operator read rather than a machine that seemed to do nothing
 * and booted from the medium.
 *
 * The wait is bounded: a machine whose operator walked away continues its
 * ordinary boot after `INSTALLER_STORAGE_REFUSAL_WAIT_MS` instead of hanging on
 * a page nobody is reading.  The page is painted and also published line by
 * line through the same serial path every other screen line uses, so a proof
 * reads exactly what the person read.
 *
 * ITEM-541: the lines themselves are built by the model half
 * (`installer_storage_refusal_lines()`), which is where the medium verdict,
 * the journal status and the bounded ring tail are proven on the host; this
 * half only paints them and waits.
 */
void installer_storage_refusal_show(const char *reason,
                                    const struct appliance_storage_report *observed,
                                    const struct installer_refusal_evidence *evidence,
                                    enum appliance_language language)
{
    /*
     * The paint and serial helpers take the screen's full line array, so this
     * page owns one too and fills only the lines it prints.  A shorter array
     * compiles but makes the helpers read past it.
     */
    static char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES];
    size_t index;
    const size_t count = installer_storage_refusal_lines(
        reason, observed, evidence, language, lines);
    const uint64_t deadline =
        timer_ticks() +
        (uint64_t)(INSTALLER_STORAGE_REFUSAL_WAIT_MS / STORAGE_MS_PER_TICK);

    if (display_framebuffer_usable()) {
        installer_storage_paint(lines, count);
    } else {
        for (index = 0U; index < count; ++index) {
            console_write(lines[index]);
            console_write("\n");
        }
    }
    installer_storage_serial_lines(lines, count);

    while (!installer_storage_expired(deadline)) {
        struct keyboard_event event;

        local_input_service();
        if (local_input_pop_event(&event)) {
            return;
        }
    }
}

enum installer_storage_decision installer_storage_session(
    const struct installer_discovery_report *report,
    uint32_t timeout_ms,
    enum appliance_language language)
{
    struct installer_storage_disk chosen;

    return installer_storage_session_choose(report, timeout_ms, &chosen,
                                            language);
}

/*
 * ITEM-501: the partition screen.
 *
 * The disk screen names the disk; this screen names what may be given up on it.
 * The operator moves over the partitions the discovery found - every row is a
 * real partition with its device index, its capacity, its kind and its volume
 * layout - and ENTER draws the warning page for the row under the cursor, the
 * page a second ENTER authorizes.  Q on the list is not a cancellation: it means
 * "keep every partition", and the caller then continues with the free-space
 * path exactly as before.  Q on the warning page returns to the list.
 *
 * Nothing here writes: the model carries the extent the planner will use, and
 * the record published at every step says `writes=0`.
 */
static void installer_storage_show_partitions(
    const struct installer_storage_partition_model *model,
    enum appliance_language language)
{
    static char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES];
    const size_t count =
        installer_storage_partition_lines(model, language, lines);

    if (display_framebuffer_usable() && count > 0U) {
        installer_storage_paint(lines, count);
    } else if (count > 0U) {
        size_t index;

        for (index = 0U; index < count; ++index) {
            console_write(lines[index]);
            console_write("\n");
        }
    }
    if (count > 0U) {
        installer_storage_serial_lines(lines, count);
    }
}

static void installer_storage_show_partition_warning(
    const struct installer_storage_partition *partition,
    const struct installer_storage_partition_model *model,
    enum appliance_language language)
{
    static char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES];
    const size_t count =
        installer_storage_partition_warning_lines(partition, model, language,
                                                  lines);

    if (display_framebuffer_usable() && count > 0U) {
        installer_storage_paint(lines, count);
    } else if (count > 0U) {
        size_t index;

        for (index = 0U; index < count; ++index) {
            console_write(lines[index]);
            console_write("\n");
        }
    }
    if (count > 0U) {
        installer_storage_serial_lines(lines, count);
    }
}

static void installer_storage_partition_record_publish(
    const struct installer_storage_partition_model *model)
{
    char record[INSTALLER_STORAGE_RECORD_BYTES];
    const size_t length =
        installer_storage_partition_record(model, record, sizeof(record));

    if (length == 0U) {
        return;
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();

        serial_write(record);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    console_publish_journal(record, length);
}

enum installer_storage_decision installer_storage_session_release(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    uint32_t timeout_ms,
    uint32_t *released_index,
    uint64_t required_lba,
    enum appliance_language language)
{
    struct installer_storage_partition_model model;
    bool warning_page = false;
    const uint64_t deadline =
        timeout_ms == INSTALLER_STORAGE_NO_DEADLINE
            ? 0U
            : timer_ticks() + (uint64_t)(timeout_ms / STORAGE_MS_PER_TICK);

    installer_storage_partitions_inventory(&model, report, disk_id);
    model.required_lba = required_lba;
    if (model.count == 0U) {
        /* Nothing to offer: the free-space path is the only one this disk
         * has, and the reading says so rather than inventing a row. */
        installer_storage_partition_record_publish(&model);
        return INSTALLER_STORAGE_DECISION_NONE;
    }
    installer_storage_show_partitions(&model, language);
    installer_storage_partition_record_publish(&model);

    while (!installer_storage_expired(deadline)) {
        struct keyboard_event event;

        local_input_service();
        while (local_input_pop_event(&event)) {
            switch (installer_storage_key_action(event.type, event.character)) {
            case INSTALLER_STORAGE_KEY_UP:
                if (!warning_page &&
                    installer_storage_partitions_move(&model, -1)) {
                    installer_storage_show_partitions(&model, language);
                }
                break;
            case INSTALLER_STORAGE_KEY_DOWN:
                if (!warning_page &&
                    installer_storage_partitions_move(&model, 1)) {
                    installer_storage_show_partitions(&model, language);
                }
                break;
            case INSTALLER_STORAGE_KEY_CHOOSE:
                if (warning_page) {
                    const struct installer_storage_partition *selected =
                        installer_storage_partition_selected(&model);

                    if (selected == NULL || released_index == NULL) {
                        warning_page = false;
                        installer_storage_show_partitions(&model, language);
                        break;
                    }
                    *released_index = selected->partition_index;
                    installer_storage_partition_record_publish(&model);
                    return INSTALLER_STORAGE_DECISION_CONFIRMED;
                }
                if (installer_storage_partitions_choose(&model)) {
                    installer_storage_partition_record_publish(&model);
                    warning_page = true;
                    installer_storage_show_partition_warning(
                        installer_storage_partition_selected(&model),
                        &model, language);
                }
                break;
            case INSTALLER_STORAGE_KEY_LEAVE:
                if (warning_page) {
                    /* Q on the warning is "not this one": back to the list
                     * with nothing given up. */
                    warning_page = false;
                    installer_storage_show_partitions(&model, language);
                    break;
                }
                installer_storage_partition_record_publish(&model);
                return INSTALLER_STORAGE_DECISION_NONE;
            case INSTALLER_STORAGE_KEY_NONE:
            default:
                break;
            }
        }
    }
    installer_storage_partition_record_publish(&model);
    return INSTALLER_STORAGE_DECISION_NONE;
}

/*
 * ITEM-514: the scheme screen and the summary page.
 *
 * Both are the same kind of machine half as the disk and partition screens:
 * they render a model through the one paint/serial path, read keys through the
 * hardware-independent input queue, and publish a reading.  Neither writes a
 * disk, and the only decision either returns is `CONFIRMED` or `NONE`.
 */

static void installer_storage_scheme_record_publish(
    const struct installer_storage_scheme_model *model)
{
    char record[INSTALLER_STORAGE_RECORD_BYTES];
    const size_t length =
        installer_storage_schemes_record(model, record, sizeof(record));

    if (length == 0U) {
        return;
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();

        serial_write(record);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    console_publish_journal(record, length);
}

static void installer_storage_summary_record_publish(
    const struct installer_storage_summary *summary)
{
    char record[INSTALLER_STORAGE_RECORD_BYTES];
    const size_t length =
        installer_storage_summary_record(summary, record, sizeof(record));

    if (length == 0U) {
        return;
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();

        serial_write(record);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    console_publish_journal(record, length);
}

static void installer_storage_show_scheme(
    const struct installer_storage_scheme_model *model,
    enum appliance_language language)
{
    static char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES];
    const size_t count =
        installer_storage_scheme_lines(model, language, lines);

    installer_storage_show_lines(lines, count);
}

/*
 * The page the erase scheme draws: the ITEM-495 disk warning, unchanged,
 * because that page is exactly about erasing this disk - and, when the
 * partition table could not be read, one added line that says the contents are
 * unknown, so a disk nobody could read never looks like an empty one (R03 of
 * the 2026-10-09 audit).  The line goes before the footer, which stays last.
 */
static void installer_storage_show_scheme_warning(
    const struct installer_storage_scheme_model *model,
    enum appliance_language language)
{
    static char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES];
    const size_t count = installer_storage_scheme_erase_warning_lines(
        model, language, lines);

    installer_storage_show_lines(lines, count);
}

static void installer_storage_show_scheme_refusal(
    const struct installer_storage_scheme_model *model,
    enum appliance_language language)
{
    static char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES];
    const size_t count =
        installer_storage_scheme_refusal_lines(model, language, lines);

    installer_storage_show_lines(lines, count);
}

/*
 * Wait for one key (or the session deadline) without deciding anything: the
 * refusal page and the page a plan the contract refused are read, not
 * answered, and the caller then goes back to the schemes.
 */
static void installer_storage_wait_for_key(uint64_t deadline)
{
    while (!installer_storage_expired(deadline)) {
        struct keyboard_event event;

        local_input_service();
        if (local_input_pop_event(&event)) {
            return;
        }
    }
}

enum installer_storage_decision installer_storage_session_scheme(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    enum installer_storage_scheme declared,
    uint64_t required_lba,
    uint64_t minimum_media_lba,
    bool contents_unknown,
    uint32_t timeout_ms,
    enum installer_storage_scheme *chosen,
    enum appliance_language language)
{
    struct installer_storage_scheme_model model;
    bool warning_page = false;
    const uint64_t deadline =
        timeout_ms == INSTALLER_STORAGE_NO_DEADLINE
            ? 0U
            : timer_ticks() + (uint64_t)(timeout_ms / STORAGE_MS_PER_TICK);

    installer_storage_schemes_inventory(&model, report, disk_id, declared,
                                        required_lba, minimum_media_lba,
                                        contents_unknown);
    if (model.count == 0U) {
        /* A disk no model can describe has no schemes to offer; the disk
         * screen would not have handed it over, and the caller refuses. */
        return INSTALLER_STORAGE_DECISION_NONE;
    }
    installer_storage_show_scheme(&model, language);
    installer_storage_scheme_record_publish(&model);

    while (!installer_storage_expired(deadline)) {
        struct keyboard_event event;

        local_input_service();
        while (local_input_pop_event(&event)) {
            switch (installer_storage_key_action(event.type, event.character)) {
            case INSTALLER_STORAGE_KEY_UP:
                if (!warning_page && installer_storage_schemes_move(&model, -1)) {
                    installer_storage_show_scheme(&model, language);
                }
                break;
            case INSTALLER_STORAGE_KEY_DOWN:
                if (!warning_page && installer_storage_schemes_move(&model, 1)) {
                    installer_storage_show_scheme(&model, language);
                }
                break;
            case INSTALLER_STORAGE_KEY_CHOOSE:
                if (warning_page) {
                    /*
                     * ENTER on the erase page is the operator accepting the
                     * destructive scheme he read; only now may it be selected.
                     */
                    if (installer_storage_schemes_choose(&model)) {
                        installer_storage_scheme_record_publish(&model);
                        if (chosen != NULL) {
                            *chosen = model.selected;
                        }
                        return INSTALLER_STORAGE_DECISION_CONFIRMED;
                    }
                    warning_page = false;
                    installer_storage_show_scheme(&model, language);
                    break;
                }
                {
                    const struct installer_storage_scheme_row *row =
                        installer_storage_scheme_row_cursor(&model);

                    if (row == NULL) {
                        break;
                    }
                    if (!row->available) {
                        /*
                         * A scheme that cannot be used draws its name, its
                         * stable refusal and the numbers - and then the list
                         * again.  Nothing is substituted behind the operator's
                         * back.
                         */
                        installer_storage_show_scheme_refusal(&model, language);
                        installer_storage_wait_for_key(deadline);
                        if (installer_storage_expired(deadline)) {
                            break;
                        }
                        installer_storage_show_scheme(&model, language);
                        break;
                    }
                    if (row->scheme == INSTALLER_STORAGE_SCHEME_ERASE) {
                        /* The destructive scheme is read before it is chosen. */
                        warning_page = true;
                        installer_storage_show_scheme_warning(&model, language);
                        break;
                    }
                    if (installer_storage_schemes_choose(&model)) {
                        installer_storage_scheme_record_publish(&model);
                        if (chosen != NULL) {
                            *chosen = model.selected;
                        }
                        return INSTALLER_STORAGE_DECISION_CONFIRMED;
                    }
                }
                break;
            case INSTALLER_STORAGE_KEY_LEAVE:
                if (warning_page) {
                    /* Q on the erase page is "not this one": back to the list
                     * with nothing chosen, exactly as on the disk screen. */
                    warning_page = false;
                    installer_storage_show_scheme(&model, language);
                    break;
                }
                installer_storage_scheme_record_publish(&model);
                return INSTALLER_STORAGE_DECISION_NONE;
            case INSTALLER_STORAGE_KEY_NONE:
            default:
                break;
            }
        }
    }
    installer_storage_scheme_record_publish(&model);
    return INSTALLER_STORAGE_DECISION_NONE;
}

enum installer_storage_decision installer_storage_session_summary(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    enum installer_storage_scheme declared,
    enum installer_storage_scheme scheme,
    uint32_t released_index,
    uint32_t timeout_ms,
    enum appliance_language language)
{
    static char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES];
    struct installer_storage_summary summary;
    const uint64_t deadline =
        timeout_ms == INSTALLER_STORAGE_NO_DEADLINE
            ? 0U
            : timer_ticks() + (uint64_t)(timeout_ms / STORAGE_MS_PER_TICK);
    size_t count;

    installer_storage_summary_build(&summary, report, disk_id, declared, scheme,
                                    released_index);
    count = installer_storage_summary_lines(&summary, report, language, lines);
    installer_storage_show_lines(lines, count);
    installer_storage_summary_record_publish(&summary);
    if (summary.status != INSTALLER_STORAGE_SUMMARY_OK) {
        /*
         * The contract refused the plan: the page names the refusal, and a key
         * (or the session's own end) goes back to the schemes.  Enter cannot
         * confirm a plan that was never built.
         */
        installer_storage_wait_for_key(deadline);
        return INSTALLER_STORAGE_DECISION_NONE;
    }

    while (!installer_storage_expired(deadline)) {
        struct keyboard_event event;

        local_input_service();
        while (local_input_pop_event(&event)) {
            switch (installer_storage_key_action(event.type, event.character)) {
            case INSTALLER_STORAGE_KEY_CHOOSE:
                installer_storage_summary_record_publish(&summary);
                return INSTALLER_STORAGE_DECISION_CONFIRMED;
            case INSTALLER_STORAGE_KEY_LEAVE:
                installer_storage_summary_record_publish(&summary);
                return INSTALLER_STORAGE_DECISION_NONE;
            case INSTALLER_STORAGE_KEY_NONE:
            default:
                /* Moving keys do nothing on the last page: the plan is one
                 * text, not a list, and the only decisions are write or go
                 * back. */
                break;
            }
        }
    }
    installer_storage_summary_record_publish(&summary);
    return INSTALLER_STORAGE_DECISION_NONE;
}
