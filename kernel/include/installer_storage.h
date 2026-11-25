#ifndef HFOC_INSTALLER_STORAGE_H
#define HFOC_INSTALLER_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "appliance_language.h"
#include "appliance_storage_report.h"
#include "installer_hardware_discovery.h"
#include "installer_scheme.h"
#include "installer_ui.h"
#include "keyboard.h"

/*
 * ITEM-489: the installer's storage screen - the operator names the disk.
 *
 * The discovery report already knows what the machine has: every disk the
 * storage owners probed, with its controller, its partition table, its
 * partitions and the identity the hardware reports.  What did not exist is the
 * screen that shows it and the rule that decides which disk may be chosen, and
 * that rule is the whole point of this item:
 *
 *   * a disk is chosen by **identity**, never by a device name.  There are no
 *     `/dev/sdX` or `/dev/nvmeX` names in this kernel and the installer must not
 *     invent one: the row an operator reads is the model and serial the
 *     hardware reported, with the capacity and the transport beside it;
 *   * **nothing is chosen automatically.**  The model starts with no selection -
 *     even when the machine has exactly one disk - and `can_continue()` stays
 *     false until an operator chooses.  A machine whose disk layout is guessed
 *     is a machine whose data can be destroyed by a default;
 *   * a disk whose identity or geometry could not be read is **not
 *     selectable**: it is shown, with the reason, and `choose()` refuses it.  A
 *     disk nobody can name is a disk nobody may write to;
 *   * **this item writes nothing.**  No partitioning, no formatting, no payload,
 *     no boot entry: this module reads a report and produces a screen and a
 *     record.  The planner and the executor are the following items, and the
 *     destructive confirmation belongs to them.
 */

#define INSTALLER_STORAGE_DISKS_MAX INSTALLER_DISCOVERY_DISK_CAPACITY
#define INSTALLER_STORAGE_LINES_MAX 24U
#define INSTALLER_STORAGE_LINE_BYTES INSTALLER_UI_LINE_BYTES
#define INSTALLER_STORAGE_RECORD_BYTES 320U
#define INSTALLER_STORAGE_TEXT_BYTES 48U
/*
 * ITEM-501: how many existing partitions one disk may offer the operator to
 * give up.  The discovery itself carries no more than
 * INSTALLER_DISCOVERY_PARTITION_CAPACITY rows over every disk together, so this
 * is not a second, competing bound - it is the screen's own row budget.
 */
#define INSTALLER_STORAGE_PARTITIONS_MAX 16U
/* No selection: the state every inventory starts in, and stays in until a
 * person chooses. */
#define INSTALLER_STORAGE_NONE (-1)

/*
 * What one key press means on this screen.  The decision is a pure function of
 * the decoded event, so the keys the product's own screen advertises are proven
 * on the host rather than left to the machine.
 *
 *   UP arrow or `w`/`W`    move towards the first disk
 *   DOWN arrow or `s`/`S`  move towards the last disk
 *   ENTER                  choose the disk under the cursor
 *   `q`/`Q`                leave the screen with whatever is chosen
 *
 * Anything else - including a character that only looks like one of these after
 * case folding this function does not perform - is `NONE`: a key nobody
 * defined does nothing, rather than something surprising.
 */
enum installer_storage_key_action {
    INSTALLER_STORAGE_KEY_NONE = 0,
    INSTALLER_STORAGE_KEY_UP = 1,
    INSTALLER_STORAGE_KEY_DOWN = 2,
    INSTALLER_STORAGE_KEY_CHOOSE = 3,
    INSTALLER_STORAGE_KEY_LEAVE = 4,
};

enum installer_storage_disk_state {
    /* Identity and geometry read: this disk may be chosen. */
    INSTALLER_STORAGE_DISK_SELECTABLE = 0,
    /* Present but not fully readable: shown, never selectable. */
    INSTALLER_STORAGE_DISK_UNREADABLE = 1,
    /* The probe slot the storage owners left empty. */
    INSTALLER_STORAGE_DISK_ABSENT = 2,
};

struct installer_storage_disk {
    uint32_t id;
    bool present;
    enum installer_storage_disk_state state;
    enum installer_disk_controller_kind controller;
    enum installer_partition_table_kind partition_table;
    uint64_t bytes;
    bool has_efi_system_partition;
    bool has_existing_os;
    /* The identity the hardware reported: the serial, or empty. */
    char identity[INSTALLER_DISCOVERY_IDENTITY_LENGTH];
    /*
     * ITEM-495: the name the hardware reported ("HFOC Example SATA Fixture"), or
     * empty when it reported none.  The operator recognises the disk by this;
     * the plan still writes by the identity.
     */
    char model[INSTALLER_DISCOVERY_MODEL_LENGTH];
    /* What the screen shows: capacity and transport, rendered once. */
    char size_text[INSTALLER_STORAGE_TEXT_BYTES];
    char transport_text[INSTALLER_STORAGE_TEXT_BYTES];
    char table_text[INSTALLER_STORAGE_TEXT_BYTES];
};

struct installer_storage_model {
    struct installer_storage_disk disks[INSTALLER_STORAGE_DISKS_MAX];
    /* Rows the screen shows: every present disk, readable or not. */
    size_t disk_count;
    size_t selectable_count;
    /* The row the operator is on. */
    size_t cursor;
    /* INSTALLER_STORAGE_NONE until an operator chooses a selectable disk. */
    int32_t selected;
    /*
     * ITEM-495: the operator accepted the warning for `selected`.  Choosing a
     * row is a proposal - it draws the warning - and only this flag makes it a
     * decision a caller may write a disk by.  It is cleared whenever the
     * selection changes, so a confirmed disk can never be a disk the operator
     * is no longer looking at.
     */
    bool confirmed;
    /*
     * ITEM-500: why the screen is asking, when the caller has something to
     * explain - "the disk this medium declared is not on this machine".  It is
     * the caller's own bounded line, printed above the disks; NULL (or empty)
     * when there is nothing to explain, which is the ordinary case.
     */
    const char *note;
};

/*
 * ITEM-501: one row of the partition screen - an existing partition the
 * operator may give up.  It carries the numbers the screen and the warning page
 * must name (device index, capacity, kind, owning system, volume layout), and
 * the extent the planner will use, so the row the operator reads and the extent
 * the writer removes are the same values.
 */
struct installer_storage_partition {
    uint32_t id;
    uint32_t partition_index;
    uint64_t first_lba;
    uint64_t length_lba;
    enum installer_partition_kind kind;
    enum installer_os_kind owner;
    enum installer_partition_layout layout;
    bool efi_system;
    bool container;
    char size_text[INSTALLER_STORAGE_TEXT_BYTES];
    char kind_text[INSTALLER_STORAGE_TEXT_BYTES];
    char owner_text[INSTALLER_STORAGE_TEXT_BYTES];
    char layout_text[INSTALLER_STORAGE_TEXT_BYTES];
};

struct installer_storage_partition_model {
    uint32_t disk_id;
    /*
     * The rows that may be given up, in the order the discovery reported them.
     * The shared EFI System Partition and every container are excluded: giving
     * one up would take the other system's loader with it, or remove a table
     * rather than a partition.
     */
    size_t count;
    size_t cursor;
    /* INSTALLER_STORAGE_NONE until an operator chooses a row. */
    int32_t selected;
    /*
     * ITEM-501: the numbers the free-space path is decided by - the largest
     * contiguous free region the disk still has, and what the appliance's three
     * roles need.  Both are published on the screen before the operator
     * decides, so "not enough space" is a number the person read and not a
     * refusal that appears later.
     */
    uint64_t free_lba;
    uint64_t required_lba;
    /* The disk the rows belong to, as the disk screen described it. */
    char disk_text[INSTALLER_STORAGE_TEXT_BYTES];
    struct installer_storage_partition
        rows[INSTALLER_STORAGE_PARTITIONS_MAX];
};

/*
 * Build the partition screen's rows from the discovery report for one disk.
 * Nothing is selected afterwards.  A disk with no releasable partition yields a
 * model with `count == 0`, which the caller answers by using free space.
 */
void installer_storage_partitions_inventory(
    struct installer_storage_partition_model *model,
    const struct installer_discovery_report *report,
    uint32_t disk_id);
/*
 * ITEM-501: the largest contiguous free region of one disk, in 512-byte
 * sectors, over the discovery's own partition extents.  It is the same
 * arithmetic the dual-boot planner uses to place the appliance's roles, exposed
 * so the screen can show the number before the planner runs.  A disk the report
 * does not carry or that is not GPT yields zero.
 */
uint64_t installer_storage_partitions_free_lba(
    const struct installer_discovery_report *report,
    uint32_t disk_id);
/* Move the cursor over the rows that exist; false at the ends. */
bool installer_storage_partitions_move(
    struct installer_storage_partition_model *model, int delta);
/* The operator's explicit choice of the row under the cursor. */
bool installer_storage_partitions_choose(
    struct installer_storage_partition_model *model);
/* The chosen row, or NULL when nothing is chosen. */
const struct installer_storage_partition *installer_storage_partition_selected(
    const struct installer_storage_partition_model *model);
/* How many partitions of this disk may be given up. */
size_t installer_storage_partitions_count(
    const struct installer_discovery_report *report,
    uint32_t disk_id);
/*
 * The list screen: every releasable partition with its device index, capacity,
 * kind and volume layout, and the one line that says Q keeps them all and uses
 * free space instead.
 */
size_t installer_storage_partition_lines(
    const struct installer_storage_partition_model *model,
    enum appliance_language language,
    char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES]);
/*
 * The warning page for one partition: the page the operator's ENTER
 * authorizes.  It names the device index, the extent, the kind, the owning
 * system and the volume layout, says that this partition's data will be erased
 * while every other partition and the shared EFI System Partition stay, and
 * says that nothing has been written yet.
 */
size_t installer_storage_partition_warning_lines(
    const struct installer_storage_partition *partition,
    const struct installer_storage_partition_model *model,
    enum appliance_language language,
    char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES]);
/* The reading of the partition screen, for the record. */
size_t installer_storage_partition_record(
    const struct installer_storage_partition_model *model,
    char *buffer,
    size_t size);
/* Stable names for the two enumerations the screen prints. */
const char *installer_storage_kind_name(enum installer_partition_kind kind);
const char *installer_storage_owner_name(enum installer_os_kind owner);

/*
 * What the screen decided, as it hands the decision back.  Two outcomes and no
 * more: the caller either has a disk the operator accepted, or it has nothing
 * and writes nothing.
 */
enum installer_storage_decision {
    /* Nothing was accepted: the operator left the screen (before or after
     * choosing) or a bounded session ran out. */
    INSTALLER_STORAGE_DECISION_NONE = 0,
    /* The operator chose a disk and accepted the warning drawn for it. */
    INSTALLER_STORAGE_DECISION_CONFIRMED = 1,
};

/*
 * Build the screen's model from the discovery report.  Nothing is selected
 * afterwards: `selected` is INSTALLER_STORAGE_NONE when this returns, always.
 * A null report is an empty machine, not an error.
 */
void installer_storage_inventory(struct installer_storage_model *model,
                                 const struct installer_discovery_report *report);
/*
 * ITEM-500: set the one line the screen prints above the disks, explaining why
 * the operator is being asked.  The text is borrowed, never copied, so it must
 * outlive the screen; NULL clears it.  The line is bounded by the screen's own
 * line width, and a longer text is cut by the screen rather than overflowing.
 */
void installer_storage_set_note(struct installer_storage_model *model,
                                const char *note);
/* Move the cursor over the rows that exist; false at the ends. */
bool installer_storage_move(struct installer_storage_model *model, int delta);
/*
 * The operator's explicit choice of the row under the cursor.  False - and no
 * selection - when the row is absent or unreadable.
 */
bool installer_storage_choose(struct installer_storage_model *model);
/*
 * ITEM-490: choose the disk the hardware reported under this discovery id, with
 * the same fail-closed rule the cursor path applies - an absent or unreadable
 * row cannot be selected, by id either.  False when no selectable row carries
 * that id.
 */
bool installer_storage_select_id(struct installer_storage_model *model,
                                 uint32_t disk_id);
/* True only after a selectable disk was chosen by an operator. */
bool installer_storage_can_continue(const struct installer_storage_model *model);
/*
 * ITEM-495: the operator accepted the warning for the row that is chosen.
 * False - and nothing confirmed - when no selectable disk is chosen.  Any later
 * choice clears it again.
 */
bool installer_storage_confirm(struct installer_storage_model *model);
/* The chosen disk, or NULL when nothing is chosen. */
const struct installer_storage_disk *installer_storage_selected(
    const struct installer_storage_model *model);
const char *installer_storage_state_name(enum installer_storage_disk_state state);
/* The action one decoded key press has on this screen. */
enum installer_storage_key_action installer_storage_key_action(
    enum keyboard_event_type type,
    char character);
/* The action's spelling, for a record that names what a key did. */
const char *installer_storage_key_name(enum installer_storage_key_action action);
const char *installer_storage_controller_name(
    enum installer_disk_controller_kind controller);
const char *installer_storage_table_name(
    enum installer_partition_table_kind table);
/*
 * The screen as bounded text lines, through the same renderer contract the
 * foundation screen uses, so a framebuffer machine and a text machine read the
 * same thing.
 */
size_t installer_storage_lines(
    const struct installer_storage_model *model,
    enum appliance_language language,
    char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES]);
/*
 * ITEM-495: the warning page for one disk - what the operator reads before
 * anything is written, and the page his ENTER authorizes.  It names the disk
 * the hardware described (model, identity, capacity, transport, table) and says
 * in one line what will happen to it.  A null disk renders the refusal it
 * deserves rather than a warning about nothing.
 */
size_t installer_storage_warning_lines(
    const struct installer_storage_disk *disk,
    enum appliance_language language,
    char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES]);
/*
 * The reading, one bounded record:
 *
 *   [INSTALLER-STORAGE] disks=<n> selectable=<n> selected=<id|NONE>
 *                       state=<SELECTABLE|UNREADABLE|NONE> table=<GPT|MBR|NONE>
 *                       existing_os=<0|1>
 *
 * `selected=NONE` is the honest reading of a screen nobody has chosen on, and
 * it is the reading a machine that was never touched by an operator shows.
 */
size_t installer_storage_record(const struct installer_storage_model *model,
                                char *buffer,
                                size_t size);

/*
 * ITEM-514: the install schemes - the screen between the disk and the write.
 *
 * ITEM-489 named the disk and ITEM-501 gave up a partition, but the *mode* of
 * the install was still decided by the medium's declaration or by the disk the
 * discovery happened to find: the operator chose a disk and the run picked
 * between "preserve what is there" and "erase everything".  This is that
 * decision on a screen, in the shape the ordinary installers use - Debian's
 * "Partitioning method", Anaconda's "Installation Destination": a list of
 * named schemes, each with one line about what it does, the space it needs
 * beside the space the disk has, and a summary of the resulting plan before a
 * byte is written.
 *
 * Four schemes are offered, always all four, because a scheme that silently
 * disappears is a scheme the operator is wrong about:
 *
 *   ALONGSIDE  install beside the system that is already here, in free space;
 *   FREE_SPACE use the largest contiguous free region, nothing else;
 *   ERASE      erase the whole disk and install HFOC (destructive);
 *   MANUAL     give up one existing partition - the ITEM-501 screen - because
 *              HFOC has no full manual partitioning, no shrink and no LVM/RAID,
 *              and the screen says so instead of hiding the row.
 *
 * A row that cannot be used carries the stable refusal name that says why
 * (`NO_EXISTING_SYSTEM`, `NOT_ENOUGH_FREE_SPACE`, `MEDIA_TOO_SMALL`,
 * `NO_RELEASABLE_PARTITION`), and choosing it draws that refusal instead of
 * silently selecting another scheme.  Nothing is chosen by this model: the
 * caller sets the cursor to the declared or discovered default, and only an
 * operator's ENTER selects.
 */
struct installer_storage_scheme_row {
    enum installer_storage_scheme scheme;
    /* False when the row cannot be used; `refusal` then names why. */
    bool available;
    /* A stable machine token, never translated; NULL when available. */
    const char *refusal;
    /* What the scheme needs and what the disk offers, in 512-byte sectors. */
    uint64_t required_lba;
    uint64_t available_lba;
};

struct installer_storage_scheme_model {
    uint32_t disk_id;
    size_t count;
    size_t cursor;
    /* INSTALLER_STORAGE_SCHEME_NONE until an operator selects a row. */
    enum installer_storage_scheme selected;
    /* The declaration or discovery default the cursor starts on. */
    enum installer_storage_scheme default_scheme;
    /* The scheme the medium declared; NONE when it declared none. */
    enum installer_storage_scheme declared;
    /*
     * ITEM-514 (R03 of the 2026-10-09 audit): the partition table could not be
     * read, so "empty disk" is not what this disk is known to be.  The erase
     * page says the contents are unknown instead of looking like a blank disk.
     */
    bool contents_unknown;
    bool has_existing_system;
    uint32_t existing_system_count;
    uint32_t releasable_count;
    uint64_t free_lba;
    uint64_t required_lba;
    uint64_t media_lba;
    uint64_t minimum_media_lba;
    /*
     * The disk the scheme belongs to, as the disk screen described it: the
     * erase warning page is the ITEM-495 disk page and reads this copy.
     */
    struct installer_storage_disk disk;
    struct installer_storage_scheme_row rows[INSTALLER_STORAGE_SCHEMES_MAX];
};

/*
 * Build the scheme list for one disk.  `declared` is the scheme the medium
 * declared (NONE when it declared none), `required_lba` the footprint the
 * preserving layout needs, `minimum_media_lba` the smallest disk the clean
 * layout fits on.  The model starts with `selected` at NONE; the cursor is the
 * default row - the declared scheme when there is one, otherwise the scheme
 * the disk itself suggests (alongside when a system is here, erase otherwise).
 */
void installer_storage_schemes_inventory(
    struct installer_storage_scheme_model *model,
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    enum installer_storage_scheme declared,
    uint64_t required_lba,
    uint64_t minimum_media_lba,
    bool contents_unknown);
/* Move the cursor over the rows that exist; false at the ends. */
bool installer_storage_schemes_move(
    struct installer_storage_scheme_model *model, int delta);
/*
 * The operator's explicit choice of the row under the cursor.  False - and no
 * selection - when the row cannot be used; the refusal is read from the row
 * and painted by the caller.
 */
bool installer_storage_schemes_choose(
    struct installer_storage_scheme_model *model);
const struct installer_storage_scheme_row *installer_storage_scheme_row_at(
    const struct installer_storage_scheme_model *model, size_t index);
/* The row under the cursor, or NULL. */
const struct installer_storage_scheme_row *installer_storage_scheme_row_cursor(
    const struct installer_storage_scheme_model *model);
/* The row an operator selected, or NULL. */
const struct installer_storage_scheme_row *installer_storage_scheme_selected(
    const struct installer_storage_scheme_model *model);
/* Stable record spelling: "ALONGSIDE", "FREE_SPACE", "ERASE", "MANUAL", "NONE". */
const char *installer_storage_scheme_name(enum installer_storage_scheme scheme);
/* The same for the reading: "alongside", "free-space", "erase", "manual", "none". */
const char *installer_storage_scheme_text(enum installer_storage_scheme scheme);
/*
 * The list screen: every scheme with its one-line description, the space
 * arithmetic (free / needed) and the honest boundary line - no full manual
 * partitioning, no shrinking, no LVM or RAID.  An unavailable row carries its
 * refusal token beside it.
 */
size_t installer_storage_scheme_lines(
    const struct installer_storage_scheme_model *model,
    enum appliance_language language,
    char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES]);
/*
 * The page a scheme that cannot be used draws: the scheme, the stable refusal
 * name and the numbers, so the operator reads what stopped it and what to do
 * instead of watching a key do nothing.
 */
size_t installer_storage_scheme_refusal_lines(
    const struct installer_storage_scheme_model *model,
    enum appliance_language language,
    char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES]);
/*
 * The page the destructive scheme draws: the ITEM-495 disk warning, unchanged,
 * plus one added line - before the footer - when the partition table could not
 * be read, so a disk nobody could read never looks like an empty one (R03 of
 * the 2026-10-09 audit).  It lives here, in the model, so the exact line array
 * the machine paints is proven on the host.
 */
size_t installer_storage_scheme_erase_warning_lines(
    const struct installer_storage_scheme_model *model,
    enum appliance_language language,
    char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES]);
/* The reading of the scheme screen, one bounded record, `writes=0`. */
size_t installer_storage_schemes_record(
    const struct installer_storage_scheme_model *model,
    char *buffer,
    size_t size);

/*
 * ITEM-541: the page a refused install leaves behind has to answer the two
 * questions that decide what the operator does next - did this machine
 * recognise the medium it booted from, and is the install journal bound to it
 * - because on a machine without serial that page is the only channel.  The
 * caller gathers the evidence (the medium verdict, the journal's own status,
 * the bounded tail of the journal's RAM ring); the builder is a pure function
 * of it, so every page form and the line budget are proven on the host.
 *
 * `records` is newest first and `record_count` how many entries are valid; the
 * builder paints its bounded prefix oldest first, so the page reads in the
 * order the machine lived it.
 */
#define INSTALLER_STORAGE_REFUSAL_TAIL_MAX 12U

struct installer_refusal_evidence {
    /* "MATCH"/"NO_MATCH"/"UNAVAILABLE"/"UNCHECKED" and the reason behind it. */
    const char *medium_state;
    const char *medium_reason;
    /* "READY"/"UNBOUND"/"UNAVAILABLE"/"FULL" and the first name that decided
       it ("OK" while READY). */
    const char *journal_state;
    const char *journal_reason;
    uint32_t journal_pending;
    uint32_t journal_dropped;
    uint32_t journal_refused;
    const char *records[INSTALLER_STORAGE_REFUSAL_TAIL_MAX];
    size_t record_count;
};

/*
 * Build the refusal page: the refusal, the observed storage (or the honest
 * `state=NOT_PROBED` when the report is empty - F-6), the medium verdict, the
 * journal status and counters, the bounded ring tail and the way out.  The
 * fixed lines and the tail are reserved first, so the observed section is
 * clipped rather than allowed to push the way out off the page.
 */
size_t installer_storage_refusal_lines(
    const char *reason,
    const struct appliance_storage_report *observed,
    const struct installer_refusal_evidence *evidence,
    enum appliance_language language,
    char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES]);

/*
 * ITEM-514: the one bounded line builder the storage screens share, so the
 * scheme and summary screens write the same bytes the disk and partition
 * screens do.  It never overflows and never truncates in the middle of a
 * buffer it was not given.
 */
struct installer_storage_line {
    char *text;
    size_t size;
    size_t length;
};
void installer_storage_line_reset(struct installer_storage_line *line,
                                  char *text, size_t size);
void installer_storage_line_append(struct installer_storage_line *line,
                                   const char *text);
void installer_storage_line_u64(struct installer_storage_line *line,
                                uint64_t value);
/* The one-decimal capacity text every storage screen prints ("240.0 GB"). */
void installer_storage_size_text(char *text, size_t size, uint64_t bytes);

#endif
