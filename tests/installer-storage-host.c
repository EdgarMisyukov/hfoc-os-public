/*
 * ITEM-489: the installer's storage screen, proven on the host.
 *
 * The screen exists so that an operator - not a default - names the disk the
 * installer will later write to.  This proof pins the three rules that make
 * that safe, and pins them on the cases where getting them wrong destroys data:
 *
 *   * an inventory never selects anything by itself, not even when the machine
 *     has exactly one disk;
 *   * a disk whose identity or geometry was not read is shown and never
 *     selectable;
 *   * the screen cannot continue until an operator has chosen a selectable
 *     disk.
 */
#include "installer_storage.h"
#include "installer_storage_summary.h"
#include "appliance_storage_report.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES];

static bool screen_has(const char *needle)
{
    size_t index;

    for (index = 0U; index < INSTALLER_STORAGE_LINES_MAX; ++index) {
        if (strcmp(lines[index], needle) == 0) {
            return true;
        }
    }
    return false;
}

static bool screen_contains(const char *needle)
{
    size_t index;

    for (index = 0U; index < INSTALLER_STORAGE_LINES_MAX; ++index) {
        if (strstr(lines[index], needle) != NULL) {
            return true;
        }
    }
    return false;
}

/*
 * ITEM-509 increment 18: the shared buffer keeps the lines of earlier renders
 * past the count this one produced, so a claim about the screen this render
 * wrote - in particular "no English prose left" - must read only its lines.
 */
static bool rendered_has(size_t count, const char *needle)
{
    size_t index;

    for (index = 0U; index < count && index < INSTALLER_STORAGE_LINES_MAX;
         ++index) {
        if (strcmp(lines[index], needle) == 0) {
            return true;
        }
    }
    return false;
}

static bool rendered_contains(size_t count, const char *needle)
{
    size_t index;

    for (index = 0U; index < count && index < INSTALLER_STORAGE_LINES_MAX;
         ++index) {
        if (strstr(lines[index], needle) != NULL) {
            return true;
        }
    }
    return false;
}

static void add_disk(struct installer_discovery_report *report, uint32_t id,
                     const char *identity,
                     enum installer_disk_controller_kind controller,
                     enum installer_partition_table_kind table,
                     uint64_t gb, bool efi)
{
    struct installer_discovery_disk *disk = &report->disks[report->disk_count];

    memset(disk, 0, sizeof(*disk));
    disk->id = id;
    disk->present = true;
    disk->controller = controller;
    disk->partition_table = table;
    disk->block_size = 512U;
    disk->block_count = (uint32_t)(gb * UINT64_C(1000000000) / 512U);
    disk->has_efi_system_partition = efi;
    if (identity != NULL) {
        size_t index = 0U;

        while (identity[index] != '\0' &&
               index + 1U < sizeof(disk->identity)) {
            disk->identity[index] = identity[index];
            ++index;
        }
        disk->identity[index] = '\0';
    }
    report->disk_count += 1U;
}

static void add_owner(struct installer_discovery_report *report, uint32_t disk_id,
                      enum installer_os_kind owner)
{
    struct installer_discovery_partition *partition =
        &report->partitions[report->partition_count];

    memset(partition, 0, sizeof(*partition));
    partition->id = report->partition_count;
    partition->present = true;
    partition->disk_id = disk_id;
    partition->partition_index = 1U;
    partition->kind = INSTALLER_PARTITION_LINUX_ROOT;
    partition->owner = owner;
    report->partition_count += 1U;
}

static void test_an_empty_machine_is_honest(void)
{
    struct installer_discovery_report report;
    struct installer_storage_model model;
    char record[INSTALLER_STORAGE_RECORD_BYTES];

    memset(&report, 0, sizeof(report));
    installer_storage_inventory(&model, &report);
    CHECK(model.disk_count == 0U && model.selectable_count == 0U &&
              model.selected == INSTALLER_STORAGE_NONE &&
              !installer_storage_can_continue(&model) &&
              installer_storage_selected(&model) == NULL,
          "a machine with no disk shows no disk and chooses nothing");
    CHECK(installer_storage_lines(&model, APPLIANCE_LANGUAGE_DEFAULT, lines) > 0U &&
              screen_has("Select installation disk") &&
              screen_has("  No disk was found") &&
              screen_has("  Status: UNAVAILABLE"),
          "the empty screen says so instead of inventing a disk");
    CHECK(installer_storage_record(&model, record, sizeof(record)) > 0U &&
              strcmp(record,
                     "[INSTALLER-STORAGE] disks=0 selectable=0 selected=NONE "
                     "state=NONE table=NONE existing_os=0") == 0,
          "the record reads NONE for a machine nobody has chosen on");
    CHECK(!installer_storage_choose(&model) &&
              model.selected == INSTALLER_STORAGE_NONE,
          "choosing with no disk on the screen selects nothing");
}

static void test_one_disk_is_still_not_chosen_for_the_operator(void)
{
    struct installer_discovery_report report;
    struct installer_storage_model model;
    const struct installer_storage_disk *disk;
    char record[INSTALLER_STORAGE_RECORD_BYTES];

    memset(&report, 0, sizeof(report));
    add_disk(&report, 0U, "KINGSTON SA400S3 DEADBEE",
             INSTALLER_DISK_CONTROLLER_SATA, INSTALLER_PARTITION_TABLE_GPT,
             240U, true);
    installer_storage_inventory(&model, &report);
    CHECK(model.disk_count == 1U && model.selectable_count == 1U &&
              model.selected == INSTALLER_STORAGE_NONE &&
              !installer_storage_can_continue(&model),
          "the only disk in the machine is not chosen on the operator's behalf");
    (void)installer_storage_lines(&model, APPLIANCE_LANGUAGE_DEFAULT, lines);
    CHECK(screen_contains("KINGSTON") && screen_contains("240.0 GB") &&
              screen_contains("SATA") && screen_contains("GPT") &&
              screen_contains("State: SELECTABLE"),
          "the row names the disk by the identity the hardware reported");
    CHECK(screen_has("No disk is chosen. Select one with ENTER; nothing is "
                     "chosen for you."),
          "the screen says who chooses and that nothing is chosen yet");

    CHECK(installer_storage_choose(&model) && model.selected == 0 &&
              installer_storage_can_continue(&model),
          "an explicit choice selects the row under the cursor");
    disk = installer_storage_selected(&model);
    CHECK(disk != NULL && disk->id == 0U &&
              strcmp(disk->identity,
                     "KINGSTON SA400S3 DEADBEE") == 0 &&
              disk->bytes == UINT64_C(240000000000) &&
              disk->has_efi_system_partition,
          "the chosen disk is the one that was read, by identity and geometry");
    CHECK(installer_storage_record(&model, record, sizeof(record)) > 0U &&
              strcmp(record,
                     "[INSTALLER-STORAGE] disks=1 selectable=1 selected=0 "
                     "state=SELECTABLE table=GPT existing_os=0") == 0,
          "the record names the chosen disk, its table and whether a system "
          "already lives on it");
}

static void test_an_unreadable_disk_is_never_selectable(void)
{
    struct installer_discovery_report report;
    struct installer_storage_model model;
    char record[INSTALLER_STORAGE_RECORD_BYTES];

    memset(&report, 0, sizeof(report));
    add_disk(&report, 3U, NULL, INSTALLER_DISK_CONTROLLER_NVME,
             INSTALLER_PARTITION_TABLE_NONE, 1800U, false);
    installer_storage_inventory(&model, &report);
    CHECK(model.disk_count == 1U && model.selectable_count == 0U &&
              model.disks[0].state == INSTALLER_STORAGE_DISK_UNREADABLE,
          "a disk with no identity is shown and marked unreadable");
    (void)installer_storage_lines(&model, APPLIANCE_LANGUAGE_DEFAULT, lines);
    CHECK(screen_contains("UNIDENTIFIED DISK") &&
              screen_contains("State: UNREADABLE"),
          "the screen shows the disk and the reason it may not be chosen");
    CHECK(!installer_storage_choose(&model) &&
              model.selected == INSTALLER_STORAGE_NONE &&
              !installer_storage_can_continue(&model),
          "choosing an unreadable disk selects nothing and cannot continue");
    CHECK(installer_storage_record(&model, record, sizeof(record)) > 0U &&
              strstr(record, "selected=NONE") != NULL,
          "the record still reads NONE after the refused choice");
}

static void test_a_choice_by_id_obeys_the_same_rules(void)
{
    struct installer_discovery_report report;
    struct installer_storage_model model;
    const struct installer_storage_disk *disk;

    memset(&report, 0, sizeof(report));
    add_disk(&report, 7U, "DISK-A", INSTALLER_DISK_CONTROLLER_SATA,
             INSTALLER_PARTITION_TABLE_GPT, 240U, true);
    add_disk(&report, 9U, NULL, INSTALLER_DISK_CONTROLLER_NVME,
             INSTALLER_PARTITION_TABLE_NONE, 1800U, false);
    installer_storage_inventory(&model, &report);
    CHECK(!installer_storage_select_id(&model, 9U) &&
              model.selected == INSTALLER_STORAGE_NONE,
          "a choice by id cannot select a disk nobody could name");
    CHECK(!installer_storage_select_id(&model, 4242U) &&
              model.selected == INSTALLER_STORAGE_NONE,
          "a choice by id that names no disk selects nothing");
    CHECK(installer_storage_select_id(&model, 7U) && model.selected == 0 &&
              model.cursor == 0U,
          "a choice by id selects the row that carries it");
    disk = installer_storage_selected(&model);
    CHECK(disk != NULL && disk->id == 7U &&
              strcmp(disk->identity, "DISK-A") == 0 &&
              installer_storage_can_continue(&model),
          "the selected disk is the one the id named, by identity");
    CHECK(!installer_storage_select_id(NULL, 7U),
          "a choice without a model selects nothing");
}

static void test_navigation_stops_at_the_ends(void)
{
    struct installer_discovery_report report;
    struct installer_storage_model model;

    memset(&report, 0, sizeof(report));
    add_disk(&report, 0U, "DISK-A", INSTALLER_DISK_CONTROLLER_SATA,
             INSTALLER_PARTITION_TABLE_GPT, 240U, true);
    add_disk(&report, 1U, "DISK-B", INSTALLER_DISK_CONTROLLER_NVME,
             INSTALLER_PARTITION_TABLE_GPT, 1000U, true);
    add_disk(&report, 2U, NULL, INSTALLER_DISK_CONTROLLER_UNKNOWN,
             INSTALLER_PARTITION_TABLE_NONE, 0U, false);
    installer_storage_inventory(&model, &report);
    CHECK(model.disk_count == 3U && model.selectable_count == 2U,
          "three disks are shown and only the readable ones are selectable");
    CHECK(!installer_storage_move(&model, -1) && model.cursor == 0U,
          "the cursor stops at the first row");
    CHECK(installer_storage_move(&model, 1) && model.cursor == 1U,
          "the cursor moves to the second row");
    CHECK(installer_storage_choose(&model) && model.selected == 1 &&
              installer_storage_selected(&model)->id == 1U,
          "the second disk is chosen by identity, not by position");
    CHECK(installer_storage_move(&model, 1) && model.cursor == 2U &&
              !installer_storage_choose(&model) && model.selected == 1,
          "an unreadable row cannot replace a chosen disk");
    CHECK(!installer_storage_move(&model, 1),
          "the cursor stops at the last row");
}

static void test_a_machine_with_an_existing_system_says_so(void)
{
    struct installer_discovery_report report;
    struct installer_storage_model model;
    char record[INSTALLER_STORAGE_RECORD_BYTES];

    memset(&report, 0, sizeof(report));
    add_disk(&report, 0U, "SAMSUNG MZVLB512HAJQ S3Z9NX0M",
             INSTALLER_DISK_CONTROLLER_NVME, INSTALLER_PARTITION_TABLE_GPT,
             512U, true);
    add_owner(&report, 0U, INSTALLER_OS_LINUX);
    installer_storage_inventory(&model, &report);
    CHECK(model.disks[0].has_existing_os,
          "a partition with an owner is an existing system on that disk");
    CHECK(installer_storage_choose(&model) &&
              installer_storage_record(&model, record, sizeof(record)) > 0U &&
              strstr(record, "existing_os=1") != NULL,
          "the record announces the system that is already there");
}

static void test_the_model_is_bounded(void)
{
    struct installer_discovery_report report;
    struct installer_storage_model model;
    char record[INSTALLER_STORAGE_RECORD_BYTES];
    unsigned int index;
    size_t count;

    memset(&report, 0, sizeof(report));
    report.disk_count = INSTALLER_DISCOVERY_DISK_CAPACITY;
    for (index = 0U; index < INSTALLER_DISCOVERY_DISK_CAPACITY; ++index) {
        report.disks[index].present = true;
        report.disks[index].id = index;
        report.disks[index].block_size = 512U;
        report.disks[index].block_count = 1024U;
        (void)snprintf(report.disks[index].identity,
                       sizeof(report.disks[index].identity), "DISK-%u", index);
    }
    installer_storage_inventory(&model, &report);
    CHECK(model.disk_count == INSTALLER_STORAGE_DISKS_MAX &&
              model.selectable_count == INSTALLER_STORAGE_DISKS_MAX,
          "more disks than the model holds are clamped, not overflowed");
    count = installer_storage_lines(&model, APPLIANCE_LANGUAGE_DEFAULT, lines);
    CHECK(count > 0U && count <= INSTALLER_STORAGE_LINES_MAX,
          "the screen of a full machine stays inside its line bound");
    CHECK(installer_storage_record(&model, record, sizeof(record)) > 0U &&
              strlen(record) <= INSTALLER_STORAGE_RECORD_BYTES,
          "the record stays inside the bound the journal carries");
    CHECK(installer_storage_record(NULL, record, sizeof(record)) == 0U &&
              installer_storage_record(&model, record, 4U) == 0U &&
              installer_storage_record(&model, NULL, sizeof(record)) == 0U,
          "a record that cannot fit is refused, never truncated");
    CHECK(installer_storage_lines(NULL, APPLIANCE_LANGUAGE_DEFAULT, lines) == 0U,
          "a screen without a model renders nothing");
}

/*
 * ITEM-495: the operator must recognise the disk he is about to erase, and the
 * screen must say what erasing it means before a byte is written.  The name the
 * machine reported is what he recognises; the identity is what the plan writes
 * by, and the warning page names both.
 */
static void set_model(struct installer_discovery_report *report, uint32_t id,
                      const char *model)
{
    size_t index;

    for (index = 0U; index < INSTALLER_DISCOVERY_DISK_CAPACITY; ++index) {
        if (report->disks[index].present && report->disks[index].id == id) {
            (void)snprintf(report->disks[index].model,
                           sizeof(report->disks[index].model), "%s", model);
            return;
        }
    }
}

static void test_the_row_names_the_disk_the_way_the_machine_did(void)
{
    struct installer_discovery_report report;
    struct installer_storage_model model;

    memset(&report, 0, sizeof(report));
    add_disk(&report, 1U, "DEADBEEF00000001", INSTALLER_DISK_CONTROLLER_SATA,
             INSTALLER_PARTITION_TABLE_GPT, 240U, true);
    set_model(&report, 1U, "HFOC Example SATA Fixture");
    installer_storage_inventory(&model, &report);
    (void)installer_storage_lines(&model, APPLIANCE_LANGUAGE_DEFAULT, lines);
    CHECK(screen_contains("HFOC Example SATA Fixture") &&
              screen_contains("DEADBEEF00000001"),
          "the row carries the name the machine reported and the identity the "
          "plan writes by");
    CHECK(strcmp(model.disks[0].model, "HFOC Example SATA Fixture") == 0,
          "the model reaches the screen's own model");
}

static void test_a_disk_without_a_name_shows_its_identity_alone(void)
{
    struct installer_discovery_report report;
    struct installer_storage_model model;

    memset(&report, 0, sizeof(report));
    add_disk(&report, 1U, "DEADBEEF00000001", INSTALLER_DISK_CONTROLLER_SATA,
             INSTALLER_PARTITION_TABLE_GPT, 240U, false);
    installer_storage_inventory(&model, &report);
    (void)installer_storage_lines(&model, APPLIANCE_LANGUAGE_DEFAULT, lines);
    CHECK(model.disks[0].model[0] == '\0' &&
              screen_contains("DEADBEEF00000001"),
          "a disk the machine did not name is still named by its identity");
}

/*
 * ITEM-500: the caller can tell the screen why it is asking, and the screen
 * prints that reason above the disks.  The line is the caller's: a screen with
 * nothing to explain prints nothing, an empty note prints nothing, and clearing
 * the note removes the line again.  The model never invents a reason.
 */
static void test_the_screen_can_say_why_it_is_asking(void)
{
    struct installer_discovery_report report;
    struct installer_storage_model model;

    memset(&report, 0, sizeof(report));
    add_disk(&report, 1U, "DEADBEEF00000001", INSTALLER_DISK_CONTROLLER_SATA,
             INSTALLER_PARTITION_TABLE_GPT, 240U, false);
    installer_storage_inventory(&model, &report);
    CHECK(installer_storage_lines(&model, APPLIANCE_LANGUAGE_DEFAULT, lines) > 0U &&
              !screen_contains("is not on this machine"),
          "a screen with nothing to explain prints no reason");

    installer_storage_set_note(
        &model,
        "Declared disk DEADBEEF00000002 is not on this machine - choose a disk");
    CHECK(installer_storage_lines(&model, APPLIANCE_LANGUAGE_DEFAULT, lines) > 0U &&
              screen_contains(
                  "Declared disk DEADBEEF00000002 is not on this machine") &&
              screen_has("Select installation disk"),
          "the caller's reason is printed above the disks it explains");

    installer_storage_set_note(&model, "");
    CHECK(installer_storage_lines(&model, APPLIANCE_LANGUAGE_DEFAULT, lines) > 0U &&
              !screen_contains("is not on this machine"),
          "an empty reason is no reason and prints nothing");

    installer_storage_set_note(&model, NULL);
    CHECK(installer_storage_lines(&model, APPLIANCE_LANGUAGE_DEFAULT, lines) > 0U &&
              !screen_contains("is not on this machine"),
          "clearing the reason removes the line again");
}

static void test_the_warning_page_says_what_will_be_erased(void)
{
    struct installer_discovery_report report;
    struct installer_storage_model model;
    const struct installer_storage_disk *disk;

    memset(&report, 0, sizeof(report));
    add_disk(&report, 1U, "DEADBEEF00000001", INSTALLER_DISK_CONTROLLER_SATA,
             INSTALLER_PARTITION_TABLE_GPT, 240U, true);
    set_model(&report, 1U, "HFOC Example SATA Fixture");
    add_owner(&report, 1U, INSTALLER_OS_LINUX);
    installer_storage_inventory(&model, &report);
    disk = &model.disks[0];
    CHECK(installer_storage_warning_lines(disk, APPLIANCE_LANGUAGE_DEFAULT, lines) > 0U,
          "the warning page renders");
    CHECK(screen_contains("WARNING: EVERYTHING ON THIS DISK WILL BE DELETED"),
          "the page says the disk will be erased, in those words");
    CHECK(screen_contains("HFOC Example SATA Fixture") &&
              screen_contains("DEADBEEF00000001"),
          "the page names the disk by model and identity");
    CHECK(screen_contains("240.0 GB") || screen_contains("240.0 GiB") ||
              screen_contains("Capacity: "),
          "the page names the capacity the operator is about to lose");
    CHECK(screen_contains("An existing system is installed on this disk."),
          "the page says a system already lives there");
    CHECK(screen_contains("Nothing has been written yet."),
          "the page says nothing has happened yet");
    CHECK(screen_contains("ENTER") && screen_contains("Q go back"),
          "the page says which key erases and which key goes back");
}

static void test_the_warning_page_refuses_to_warn_about_nothing(void)
{
    CHECK(installer_storage_warning_lines(NULL, APPLIANCE_LANGUAGE_DEFAULT, lines) > 0U &&
              screen_contains("No disk is selected") &&
              screen_contains("Nothing will be written."),
          "a warning page without a disk refuses to describe one");
    CHECK(installer_storage_warning_lines(NULL, APPLIANCE_LANGUAGE_DEFAULT, NULL) == 0U,
          "the warning page without a buffer renders nothing");
}

static void test_confirmation_needs_a_choice_and_a_new_choice_clears_it(void)
{
    struct installer_discovery_report report;
    struct installer_storage_model model;

    memset(&report, 0, sizeof(report));
    add_disk(&report, 1U, "DISK-A", INSTALLER_DISK_CONTROLLER_SATA,
             INSTALLER_PARTITION_TABLE_GPT, 120U, false);
    add_disk(&report, 2U, "DISK-B", INSTALLER_DISK_CONTROLLER_NVME,
             INSTALLER_PARTITION_TABLE_NONE, 240U, false);
    installer_storage_inventory(&model, &report);
    CHECK(!installer_storage_confirm(&model) && !model.confirmed,
          "nothing is confirmed before a disk is chosen");
    CHECK(installer_storage_choose(&model) && model.selected == 0,
          "ENTER on the first row chooses it");
    CHECK(installer_storage_confirm(&model) && model.confirmed,
          "the warning can be accepted for the chosen disk");
    CHECK(installer_storage_move(&model, 1) && installer_storage_choose(&model) &&
              !model.confirmed,
          "choosing another disk withdraws the acceptance of the first");
    CHECK(!installer_storage_confirm(NULL) && !model.confirmed,
          "a null model cannot be confirmed and confirms nothing");
    CHECK(installer_storage_confirm(&model) && model.confirmed,
          "confirming the same disk twice is the same decision, not a new one");
}

static void test_an_unreadable_row_cannot_be_confirmed(void)
{
    struct installer_discovery_report report;
    struct installer_storage_model model;

    memset(&report, 0, sizeof(report));
    add_disk(&report, 1U, NULL, INSTALLER_DISK_CONTROLLER_SATA,
             INSTALLER_PARTITION_TABLE_NONE, 120U, false);
    installer_storage_inventory(&model, &report);
    model.cursor = 0U;
    CHECK(!installer_storage_choose(&model) && !model.confirmed &&
              !installer_storage_confirm(&model),
          "a disk nobody could name cannot be chosen or confirmed");
}

static void test_the_keys_the_screen_advertises_work(void)
{
    CHECK(installer_storage_key_action(KEYBOARD_EVENT_UP, '\0') ==
                  INSTALLER_STORAGE_KEY_UP &&
              installer_storage_key_action(KEYBOARD_EVENT_DOWN, '\0') ==
                  INSTALLER_STORAGE_KEY_DOWN &&
              installer_storage_key_action(KEYBOARD_EVENT_ENTER, '\0') ==
                  INSTALLER_STORAGE_KEY_CHOOSE,
          "the arrow keys and ENTER do what the hint says");
    CHECK(installer_storage_key_action(KEYBOARD_EVENT_CHARACTER, 'w') ==
                  INSTALLER_STORAGE_KEY_UP &&
              installer_storage_key_action(KEYBOARD_EVENT_CHARACTER, 'W') ==
                  INSTALLER_STORAGE_KEY_UP &&
              installer_storage_key_action(KEYBOARD_EVENT_CHARACTER, 's') ==
                  INSTALLER_STORAGE_KEY_DOWN &&
              installer_storage_key_action(KEYBOARD_EVENT_CHARACTER, 'S') ==
                  INSTALLER_STORAGE_KEY_DOWN,
          "W and S move, in both cases, exactly as the hint prints them");
    CHECK(installer_storage_key_action(KEYBOARD_EVENT_CHARACTER, 'q') ==
                  INSTALLER_STORAGE_KEY_LEAVE &&
              installer_storage_key_action(KEYBOARD_EVENT_CHARACTER, 'Q') ==
                  INSTALLER_STORAGE_KEY_LEAVE,
          "Q leaves the screen");
    CHECK(installer_storage_key_action(KEYBOARD_EVENT_CHARACTER, 'x') ==
                  INSTALLER_STORAGE_KEY_NONE &&
              installer_storage_key_action(KEYBOARD_EVENT_CHARACTER, ' ') ==
                  INSTALLER_STORAGE_KEY_NONE &&
              installer_storage_key_action(KEYBOARD_EVENT_BACKSPACE, '\0') ==
                  INSTALLER_STORAGE_KEY_NONE &&
              installer_storage_key_action(KEYBOARD_EVENT_LEFT, '\0') ==
                  INSTALLER_STORAGE_KEY_NONE,
          "a key this screen does not define does nothing at all");
    CHECK(strcmp(installer_storage_key_name(INSTALLER_STORAGE_KEY_CHOOSE),
                 "CHOOSE") == 0 &&
              strcmp(installer_storage_key_name(INSTALLER_STORAGE_KEY_NONE),
                     "NONE") == 0,
          "the actions have one spelling each");
}

static void test_names_are_stable(void)
{
    CHECK(strcmp(installer_storage_state_name(
                     INSTALLER_STORAGE_DISK_SELECTABLE), "SELECTABLE") == 0 &&
              strcmp(installer_storage_state_name(
                         INSTALLER_STORAGE_DISK_UNREADABLE),
                     "UNREADABLE") == 0,
          "the states have one spelling each");
    CHECK(strcmp(installer_storage_controller_name(
                     INSTALLER_DISK_CONTROLLER_SATA), "SATA") == 0 &&
              strcmp(installer_storage_controller_name(
                         INSTALLER_DISK_CONTROLLER_NVME), "NVME") == 0 &&
              strcmp(installer_storage_controller_name(
                         INSTALLER_DISK_CONTROLLER_UNKNOWN), "UNKNOWN") == 0,
          "the transports the hardware reports have one spelling each");
    CHECK(strcmp(installer_storage_table_name(INSTALLER_PARTITION_TABLE_GPT),
                 "GPT") == 0 &&
              strcmp(installer_storage_table_name(
                         INSTALLER_PARTITION_TABLE_NONE), "NONE") == 0,
          "the partition tables have one spelling each");
}

static void add_partition_row(struct installer_discovery_report *report,
                              uint32_t id, uint32_t disk_id,
                              uint32_t partition_index,
                              enum installer_partition_kind kind,
                              enum installer_partition_layout layout,
                              bool efi, bool container,
                              uint64_t first_lba, uint64_t length_lba)
{
    struct installer_discovery_partition *partition =
        &report->partitions[report->partition_count];

    memset(partition, 0, sizeof(*partition));
    partition->id = id;
    partition->present = true;
    partition->disk_id = disk_id;
    partition->partition_index = partition_index;
    partition->kind = kind;
    partition->owner =
        kind == INSTALLER_PARTITION_EFI_SYSTEM
            ? INSTALLER_OS_UNKNOWN
            : (kind == INSTALLER_PARTITION_WINDOWS ? INSTALLER_OS_WINDOWS
                                                   : INSTALLER_OS_LINUX);
    partition->layout = layout;
    partition->efi_system = efi;
    partition->container = container;
    partition->first_lba = first_lba;
    partition->length_lba = length_lba;
    report->partition_count += 1U;
}

/*
 * ITEM-501: what the partition screen offers and what it refuses to offer.
 * The shared ESP and a container are not rows; a Linux LVM partition is, with
 * its layout named, and the warning page says what dies and what stays.
 */
static void test_the_partition_screen_names_what_may_be_given_up(void)
{
    struct installer_discovery_report report;
    struct installer_storage_partition_model model;
    const struct installer_storage_partition *row;
    char record[INSTALLER_STORAGE_RECORD_BYTES];

    memset(&report, 0, sizeof(report));
    add_disk(&report, 1U, "SERIAL1", INSTALLER_DISK_CONTROLLER_SATA,
             INSTALLER_PARTITION_TABLE_GPT, 100U, true);
    add_partition_row(&report, 1U, 1U, 1U, INSTALLER_PARTITION_EFI_SYSTEM,
                      INSTALLER_PARTITION_LAYOUT_PLAIN, true, false,
                      2048U, 67584U);
    add_partition_row(&report, 2U, 1U, 2U, INSTALLER_PARTITION_LINUX_ROOT,
                      INSTALLER_PARTITION_LAYOUT_LVM, false, false,
                      69632U, 131072U);
    add_partition_row(&report, 3U, 1U, 3U, INSTALLER_PARTITION_LINUX_ROOT,
                      INSTALLER_PARTITION_LAYOUT_EXTENDED, false, true,
                      200704U, 65536U);

    CHECK(installer_storage_partitions_count(&report, 1U) == 1U,
          "the ESP and the container are not offered as release targets");
    installer_storage_partitions_inventory(&model, &report, 1U);
    model.required_lba = 262144U;
    CHECK(model.count == 1U && model.selected == INSTALLER_STORAGE_NONE &&
              model.rows[0U].partition_index == 2U &&
              strcmp(model.rows[0U].layout_text, "LVM") == 0 &&
              strcmp(model.rows[0U].kind_text, "Linux") == 0 &&
              model.rows[0U].size_text[0] != '\0',
          "the row names the partition, its kind, its size and its layout");
    CHECK(installer_storage_partitions_free_lba(&report, 1U) ==
              report.disks[0U].block_count - 34U - 266240U,
          "the free-space number is the largest region the disk still has");
    CHECK(installer_storage_partition_lines(&model, APPLIANCE_LANGUAGE_DEFAULT, lines) > 0U &&
              screen_contains("Free space:") &&
              screen_contains("Needed:") &&
              screen_contains("Layout") == false &&
              screen_contains("LVM") &&
              screen_contains("Q keep every partition"),
          "the list shows the numbers and the layout before the choice");
    CHECK(installer_storage_partitions_choose(&model) &&
              !installer_storage_partitions_move(&model, 1),
          "the operator chooses the only row and the cursor stops there");
    row = installer_storage_partition_selected(&model);
    CHECK(row != NULL && row->partition_index == 2U,
          "the chosen row is the partition the warning describes");
    CHECK(installer_storage_partition_warning_lines(row, &model, APPLIANCE_LANGUAGE_DEFAULT, lines) >
              0U &&
              screen_contains("THE DATA ON THIS PARTITION WILL BE DELETED") &&
              screen_contains("Partition: 2") &&
              screen_contains("Layout: LVM") &&
              screen_contains("System: Linux") &&
              screen_contains("Nothing has been written yet"),
          "the warning names what dies and what stays");
    CHECK(installer_storage_partition_record(&model, record,
                                             sizeof(record)) > 0U &&
              strcmp(record,
                     "[INSTALLER-STORAGE] release disk=1 partitions=1 "
                     "selected=2 layout=LVM size_lba=131072 writes=0") == 0,
          "the reading names the partition and says nothing was written");
}

/*
 * ITEM-509 increment 16: the disk list reads its prose from the one catalog in
 * the caller's language.  The English list above is byte for byte what it was;
 * the Russian one comes from the catalog column and keeps the machine state
 * word (UNAVAILABLE), while a language the catalog cannot answer is the named
 * refusal, never half a screen.
 */
static void test_russian_disk_list(void)
{
    struct installer_discovery_report report;
    struct installer_storage_model model;

    memset(&report, 0, sizeof(report));
    installer_storage_inventory(&model, &report);
    CHECK(installer_storage_lines(&model, APPLIANCE_LANGUAGE_RU, lines) > 0U &&
              screen_has("Выберите диск для установки") &&
              screen_has("  Диск не найден") &&
              screen_has("  Состояние: UNAVAILABLE") &&
              screen_has(
                  "W/S или стрелки — выбор   ENTER — принять   Q — выход") &&
              screen_has(
                  "Диск не выбран. Выберите ENTER; за вас ничего не "
                  "выбирается."),
          "ITEM-509: the disk list comes from the Russian catalog column");
    CHECK(!screen_has("Select installation disk") &&
              !screen_has("  No disk was found"),
          "ITEM-509: the Russian disk list carries no English interface prose");
}

static void test_disk_list_missing_translation_is_named(void)
{
    struct installer_discovery_report report;
    struct installer_storage_model model;

    memset(&report, 0, sizeof(report));
    installer_storage_inventory(&model, &report);
    CHECK(installer_storage_lines(&model, (enum appliance_language)7, lines) ==
                  1U &&
              screen_has("INSTALLER TRANSLATION MISSING"),
          "ITEM-509: an unknown language renders the named refusal, not half "
          "a disk list");
}

/*
 * ITEM-509 increment 18: the disk warning page and the two partition screens
 * read the same one catalog in the caller's language.  The English screens
 * above are byte for byte what they were; these checks read the Russian column,
 * name every fixed sentence of both screens, and insist that no English
 * interface prose is left beside it.  Machine values - the disk's own name and
 * identity, the partition index, the kind and layout names, the state words -
 * stay as they were, which is why "Linux" and "LVM" still appear below.
 */
static void test_russian_disk_warning(void)
{
    struct installer_discovery_report report;
    struct installer_storage_model model;
    const struct installer_storage_disk *disk;
    size_t count;

    memset(&report, 0, sizeof(report));
    add_disk(&report, 1U, "DEADBEEF00000001", INSTALLER_DISK_CONTROLLER_SATA,
             INSTALLER_PARTITION_TABLE_GPT, 240U, true);
    set_model(&report, 1U, "HFOC Example SATA Fixture");
    add_owner(&report, 1U, INSTALLER_OS_LINUX);
    installer_storage_inventory(&model, &report);
    disk = &model.disks[0];
    count = installer_storage_warning_lines(disk, APPLIANCE_LANGUAGE_RU, lines);
    CHECK(count > 0U &&
              rendered_has(
                  count, "ВНИМАНИЕ: ВСЁ НА ЭТОМ ДИСКЕ БУДЕТ УДАЛЕНО") &&
              rendered_contains(count, "  Диск: HFOC Example SATA Fixture") &&
              rendered_contains(count, "  Ёмкость: ") &&
              rendered_has(count, "  На этом диске уже есть система.") &&
              rendered_has(
                  count,
                  "  Всё на нём — разделы, системы, данные — будет стёрто.") &&
              rendered_has(count,
                           "  Настройки самого аппарата тоже стираются:") &&
              rendered_has(
                  count,
                  "  сетевой адрес, учётная запись администратора, "
                  "сертификаты.") &&
              rendered_has(count, "  Пока ничего не записано.") &&
              rendered_has(count,
                           "ENTER — стереть диск и установить   Q — назад"),
          "ITEM-509: the disk warning comes from the Russian catalog column");
    CHECK(!rendered_contains(count, "WARNING: EVERYTHING") &&
              !rendered_contains(count, "An existing system is installed") &&
              !rendered_contains(count, "Nothing has been written yet."),
          "ITEM-509: the Russian disk warning keeps no English prose");
}

static void test_russian_partition_screens(void)
{
    struct installer_discovery_report report;
    struct installer_storage_partition_model model;
    size_t count;

    memset(&report, 0, sizeof(report));
    add_disk(&report, 1U, "SERIAL1", INSTALLER_DISK_CONTROLLER_SATA,
             INSTALLER_PARTITION_TABLE_GPT, 100U, true);
    add_partition_row(&report, 1U, 1U, 2U, INSTALLER_PARTITION_LINUX_ROOT,
                      INSTALLER_PARTITION_LAYOUT_LVM, false, false,
                      69632U, 131072U);
    installer_storage_partitions_inventory(&model, &report, 1U);
    model.required_lba = 262144U;
    count = installer_storage_partition_lines(&model, APPLIANCE_LANGUAGE_RU,
                                              lines);
    CHECK(count > 0U &&
              rendered_has(count, "Отдать один существующий раздел") &&
              rendered_contains(count, "  Свободно: ") &&
              rendered_contains(count, " МБ   Нужно: ") &&
              rendered_contains(count, "Раздел 2") &&
              rendered_has(count, "     Система: Linux") &&
              rendered_has(
                  count,
                  "W/S или стрелки — выбор   ENTER — принять   Q — сохранить "
                  "разделы") &&
              rendered_has(
                  count,
                  "Q сохраняет все разделы и продолжает без них."),
          "ITEM-509: the partition list comes from the Russian catalog column");
    CHECK(!rendered_contains(count, "Give up one existing partition") &&
              !rendered_contains(count, "Free space") &&
              !rendered_contains(count, "keep every partition"),
          "ITEM-509: the Russian partition list keeps no English prose");
    count = installer_storage_partition_warning_lines(
        &model.rows[0], &model, APPLIANCE_LANGUAGE_RU, lines);
    CHECK(count > 0U &&
              rendered_has(
                  count,
                  "ВНИМАНИЕ: ДАННЫЕ В ЭТОМ РАЗДЕЛЕ БУДУТ УДАЛЕНЫ") &&
              rendered_contains(count, "  Раздел: 2") &&
              rendered_contains(count, "   Размер: ") &&
              rendered_contains(count, "  Разметка: LVM") &&
              rendered_contains(count, "   Система: Linux") &&
              rendered_has(count,
                           "  Этот раздел удаляется, а на его месте") &&
              rendered_has(count, "  создаются три раздела аппарата.") &&
              rendered_has(count,
                           "  Все остальные разделы, общий раздел EFI") &&
              rendered_has(
                  count,
                  "  и системы на них остаются нетронутыми.") &&
              rendered_has(count,
                           "  Настройки самого аппарата тоже стираются:") &&
              rendered_has(
                  count,
                  "  сетевой адрес, учётная запись администратора, "
                  "сертификаты.") &&
              rendered_has(count, "  Пока ничего не записано.") &&
              rendered_has(
                  count,
                  "ENTER — отдать раздел и установить   Q — назад"),
          "ITEM-509: the partition warning comes from the Russian catalog "
          "column");
    CHECK(!rendered_contains(count, "WARNING: THE DATA") &&
              !rendered_contains(count, "Every other partition") &&
              !rendered_contains(count, "Nothing has been written yet."),
          "ITEM-509: the Russian partition warning keeps no English prose");
}

static void test_warning_screens_missing_translation_is_named(void)
{
    struct installer_discovery_report report;
    struct installer_storage_model model;
    struct installer_storage_partition_model partitions;

    memset(&report, 0, sizeof(report));
    add_disk(&report, 1U, "SERIAL1", INSTALLER_DISK_CONTROLLER_SATA,
             INSTALLER_PARTITION_TABLE_GPT, 100U, true);
    add_partition_row(&report, 1U, 1U, 2U, INSTALLER_PARTITION_LINUX_ROOT,
                      INSTALLER_PARTITION_LAYOUT_LVM, false, false,
                      69632U, 131072U);
    installer_storage_inventory(&model, &report);
    installer_storage_partitions_inventory(&partitions, &report, 1U);
    partitions.required_lba = 262144U;
    CHECK(installer_storage_warning_lines(&model.disks[0],
                                          (enum appliance_language)7,
                                          lines) == 1U &&
              rendered_has(1U, "INSTALLER TRANSLATION MISSING"),
          "ITEM-509: an unknown language renders the named refusal on the disk "
          "warning, not half a page");
    CHECK(installer_storage_partition_lines(&partitions,
                                            (enum appliance_language)7,
                                            lines) == 1U &&
              rendered_has(1U, "INSTALLER TRANSLATION MISSING"),
          "ITEM-509: an unknown language renders the named refusal on the "
          "partition list, not half a screen");
    CHECK(installer_storage_partition_warning_lines(
              &partitions.rows[0], &partitions, (enum appliance_language)7,
              lines) == 1U &&
              rendered_has(1U, "INSTALLER TRANSLATION MISSING"),
          "ITEM-509: an unknown language renders the named refusal on the "
          "partition warning, not half a page");
}


/*
 * ITEM-514: the install schemes and the summary before the write.
 *
 * The scheme model is proven here the way the disk model is: four rows always
 * present, each with its stable refusal name, a default that is the
 * declaration or the disk's own suggestion, and nothing selected until an
 * operator's ENTER.  The summary is proven against the contract planner the
 * installer itself calls, so "what is shown is what is written" is checked
 * rather than promised.
 */

#define SCHEME_REQUIRED_LBA UINT64_C(262144)
#define SCHEME_MINIMUM_MEDIA_LBA UINT64_C(331810)

static void add_system_disk(struct installer_discovery_report *report,
                            uint32_t id, const char *identity, uint64_t gb)
{
    report->version = INSTALLER_HARDWARE_DISCOVERY_VERSION;
    report->read_only = true;
    report->scan_complete = true;
    add_disk(report, id, identity, INSTALLER_DISK_CONTROLLER_NVME,
             INSTALLER_PARTITION_TABLE_GPT, gb, true);
    add_partition_row(report, 1U, id, 1U, INSTALLER_PARTITION_EFI_SYSTEM,
                      INSTALLER_PARTITION_LAYOUT_PLAIN, true, false,
                      2048U, 67584U);
    add_partition_row(report, 2U, id, 2U, INSTALLER_PARTITION_LINUX_ROOT,
                      INSTALLER_PARTITION_LAYOUT_PLAIN, false, false,
                      69632U, 300000U);
}

static void test_the_scheme_list_names_every_scheme(void)
{
    struct installer_discovery_report report;
    struct installer_storage_scheme_model model;
    char record[INSTALLER_STORAGE_RECORD_BYTES];

    memset(&report, 0, sizeof(report));
    add_system_disk(&report, 1U, "SAMSUNG MZVLB512HAJQ S3Z9NX0M", 240U);
    report.oss[0].id = 1U;
    report.oss[0].present = true;
    report.oss[0].kind = INSTALLER_OS_LINUX;
    report.oss[0].partition_id = 2U;
    report.os_count = 1U;

    installer_storage_schemes_inventory(&model, &report, 1U,
                                        INSTALLER_STORAGE_SCHEME_NONE,
                                        SCHEME_REQUIRED_LBA,
                                        SCHEME_MINIMUM_MEDIA_LBA, false);
    CHECK(model.count == 4U &&
              model.rows[0U].scheme == INSTALLER_STORAGE_SCHEME_ALONGSIDE &&
              model.rows[1U].scheme == INSTALLER_STORAGE_SCHEME_FREE_SPACE &&
              model.rows[2U].scheme == INSTALLER_STORAGE_SCHEME_ERASE &&
              model.rows[3U].scheme == INSTALLER_STORAGE_SCHEME_MANUAL,
          "the list carries all four schemes, in one order");
    CHECK(model.default_scheme == INSTALLER_STORAGE_SCHEME_ALONGSIDE &&
              model.cursor == 0U &&
              model.selected == INSTALLER_STORAGE_SCHEME_NONE,
          "a disk with a system defaults to alongside and chooses nothing");
    CHECK(model.rows[0U].available && model.rows[1U].available &&
              model.rows[2U].available && model.rows[3U].available &&
              model.rows[0U].refusal == NULL,
          "every scheme is usable on this disk and an available row has no "
          "refusal");
    CHECK(installer_storage_scheme_lines(&model, APPLIANCE_LANGUAGE_DEFAULT,
                                          lines) > 0U &&
              screen_has("Choose the installation scheme") &&
              screen_contains("Install alongside the existing system") &&
              screen_contains("Use the free space") &&
              screen_contains("Erase the whole disk and install HFOC") &&
              screen_contains("Manual: give up one existing partition") &&
              screen_contains(
                  "No full manual partitioning, no shrink, no LVM or RAID.") &&
              screen_contains("Free space:") && screen_contains("Needed:") &&
              screen_contains("128 MB") && screen_contains("162 MB") &&
              screen_contains("W/S or arrow keys move"),
          "the list names every scheme, the space arithmetic and the honest "
          "boundaries");
    CHECK(installer_storage_schemes_record(&model, record, sizeof(record)) >
                  0U &&
              strstr(record, "declared=NONE chosen=NONE cursor=ALONGSIDE") !=
                  NULL &&
              strstr(record, "available=1") != NULL &&
              strstr(record, "writes=0") != NULL,
          "the reading names the default, the cursor and that nothing is "
          "selected yet");
    CHECK(installer_storage_schemes_choose(&model) &&
              model.selected == INSTALLER_STORAGE_SCHEME_ALONGSIDE,
          "ENTER on the default row selects the scheme the operator saw");
}

static void test_a_row_that_cannot_be_used_is_named(void)
{
    struct installer_discovery_report report;
    struct installer_storage_scheme_model model;

    memset(&report, 0, sizeof(report));
    report.version = INSTALLER_HARDWARE_DISCOVERY_VERSION;
    add_disk(&report, 1U, "SERIAL1", INSTALLER_DISK_CONTROLLER_SATA,
             INSTALLER_PARTITION_TABLE_GPT, 240U, true);
    add_partition_row(&report, 1U, 1U, 1U, INSTALLER_PARTITION_EFI_SYSTEM,
                      INSTALLER_PARTITION_LAYOUT_PLAIN, true, false,
                      2048U, 67584U);
    installer_storage_schemes_inventory(&model, &report, 1U,
                                        INSTALLER_STORAGE_SCHEME_NONE,
                                        SCHEME_REQUIRED_LBA,
                                        SCHEME_MINIMUM_MEDIA_LBA, false);
    CHECK(model.default_scheme == INSTALLER_STORAGE_SCHEME_ERASE &&
              model.cursor == 2U,
          "a disk with no system defaults to erasing the whole disk");
    CHECK(!model.rows[0U].available &&
              strcmp(model.rows[0U].refusal, "NO_EXISTING_SYSTEM") == 0,
          "alongside without a system is refused by name");
    CHECK(!model.rows[3U].available &&
              strcmp(model.rows[3U].refusal, "NO_RELEASABLE_PARTITION") == 0,
          "manual without a releasable partition is refused by name");
    model.cursor = 0U;
    CHECK(!installer_storage_schemes_choose(&model) &&
              model.selected == INSTALLER_STORAGE_SCHEME_NONE,
          "choosing an unusable row selects nothing instead of another scheme");
    CHECK(installer_storage_scheme_refusal_lines(
              &model, APPLIANCE_LANGUAGE_DEFAULT, lines) > 0U &&
              screen_contains("This scheme cannot be used") &&
              screen_contains("Install alongside the existing system") &&
              screen_contains("NO_EXISTING_SYSTEM") &&
              screen_contains("Give up a partition with Manual"),
          "the refusal page names the scheme, the reason and the way out");
}

static void test_the_declaration_is_a_default_not_a_lock(void)
{
    struct installer_discovery_report report;
    struct installer_storage_scheme_model model;

    memset(&report, 0, sizeof(report));
    add_system_disk(&report, 1U, "SERIAL1", 240U);
    /*
     * The disk is smaller than the 240 GB one but still above the clean
     * minimum: the free region behind the existing partitions is too small for
     * the preserving layout, so the preserving rows refuse by name while the
     * destructive one stays usable.
     */
    report.disks[0U].block_count = 600000U;
    installer_storage_schemes_inventory(&model, &report, 1U,
                                        INSTALLER_STORAGE_SCHEME_NONE,
                                        SCHEME_REQUIRED_LBA,
                                        SCHEME_MINIMUM_MEDIA_LBA, false);
    CHECK(!model.rows[0U].available &&
              strcmp(model.rows[0U].refusal, "NOT_ENOUGH_FREE_SPACE") == 0 &&
              !model.rows[1U].available &&
              strcmp(model.rows[1U].refusal, "NOT_ENOUGH_FREE_SPACE") == 0,
          "not enough free space is a named refusal, not a silent fallback");
    CHECK(model.rows[2U].available &&
              model.rows[2U].available_lba == 600000U,
          "the destructive row is still usable and carries the media size");

    /* The medium declared the destructive scheme: that is the default cursor,
     * and the record would say declared=ERASE. */
    installer_storage_schemes_inventory(&model, &report, 1U,
                                        INSTALLER_STORAGE_SCHEME_ERASE,
                                        SCHEME_REQUIRED_LBA,
                                        SCHEME_MINIMUM_MEDIA_LBA, false);
    CHECK(model.default_scheme == INSTALLER_STORAGE_SCHEME_ERASE &&
              model.cursor == 2U,
          "the declared scheme is the default the cursor starts on");
    /* An operator may choose otherwise, including the more destructive row. */
    model.cursor = 0U;
    CHECK(!installer_storage_schemes_choose(&model),
          "an unusable row cannot be chosen, so the default stays a "
          "proposal");
    model.cursor = 2U;
    CHECK(installer_storage_schemes_choose(&model) &&
              model.selected == INSTALLER_STORAGE_SCHEME_ERASE,
          "an unusable default does not lock the operator out of the rest");
}

static void test_a_disk_below_the_minimum_refuses_erase(void)
{
    struct installer_discovery_report report;
    struct installer_storage_scheme_model model;

    memset(&report, 0, sizeof(report));
    add_system_disk(&report, 1U, "SERIAL1", 240U);
    report.disks[0U].block_count = SCHEME_MINIMUM_MEDIA_LBA - 1U;
    installer_storage_schemes_inventory(&model, &report, 1U,
                                        INSTALLER_STORAGE_SCHEME_NONE,
                                        SCHEME_REQUIRED_LBA,
                                        SCHEME_MINIMUM_MEDIA_LBA, false);
    CHECK(!model.rows[2U].available &&
              strcmp(model.rows[2U].refusal, "MEDIA_TOO_SMALL") == 0,
          "a disk below the declared minimum refuses the clean layout by name");
    (void)installer_storage_scheme_lines(&model, APPLIANCE_LANGUAGE_DEFAULT,
                                         lines);
    CHECK(screen_contains("MEDIA_TOO_SMALL"),
          "the list carries the refusal beside the row that cannot be used");
}

static void test_an_unreadable_table_is_not_an_empty_disk(void)
{
    struct installer_discovery_report report;
    struct installer_storage_scheme_model model;
    char record[INSTALLER_STORAGE_RECORD_BYTES];

    memset(&report, 0, sizeof(report));
    add_system_disk(&report, 1U, "SERIAL1", 240U);
    installer_storage_schemes_inventory(&model, &report, 1U,
                                        INSTALLER_STORAGE_SCHEME_NONE,
                                        SCHEME_REQUIRED_LBA,
                                        SCHEME_MINIMUM_MEDIA_LBA, true);
    CHECK(model.contents_unknown,
          "the model carries the fact that the table could not be read");
    (void)installer_storage_scheme_lines(&model, APPLIANCE_LANGUAGE_DEFAULT,
                                         lines);
    CHECK(screen_contains("UNREADABLE"),
          "the list marks the disk whose contents are unknown");
    CHECK(installer_storage_schemes_record(&model, record, sizeof(record)) >
                  0U &&
              strstr(record, "unknown_contents=1") != NULL,
          "the reading carries the unknown contents so the acceptance report "
          "sees them");
}


static void test_the_erase_page_names_unknown_contents(void)
{
    struct installer_discovery_report report;
    struct installer_storage_scheme_model model;
    size_t count;

    memset(&report, 0, sizeof(report));
    report.version = INSTALLER_HARDWARE_DISCOVERY_VERSION;
    report.read_only = true;
    report.scan_complete = true;
    add_system_disk(&report, 1U, "SERIAL1", 240U);
    installer_storage_schemes_inventory(&model, &report, 1U,
                                        INSTALLER_STORAGE_SCHEME_NONE,
                                        SCHEME_REQUIRED_LBA,
                                        SCHEME_MINIMUM_MEDIA_LBA, true);
    count = installer_storage_scheme_erase_warning_lines(
        &model, APPLIANCE_LANGUAGE_DEFAULT, lines);
    CHECK(count > 0U &&
              rendered_has(count,
                           "WARNING: EVERYTHING ON THIS DISK WILL BE DELETED") &&
              rendered_contains(
                  count,
                  "The contents of this disk could not be read: what is on it "
                  "is unknown.") &&
              rendered_has(
                  count,
                  "ENTER erase this disk and install   Q go back"),
          "the erase page names the unknown contents before the footer");

    installer_storage_schemes_inventory(&model, &report, 1U,
                                        INSTALLER_STORAGE_SCHEME_NONE,
                                        SCHEME_REQUIRED_LBA,
                                        SCHEME_MINIMUM_MEDIA_LBA, false);
    count = installer_storage_scheme_erase_warning_lines(
        &model, APPLIANCE_LANGUAGE_DEFAULT, lines);
    CHECK(count > 0U &&
              !rendered_contains(count,
                                 "contents of this disk could not be read"),
          "a disk whose table was read carries no unknown-contents line");

    installer_storage_schemes_inventory(&model, &report, 1U,
                                        INSTALLER_STORAGE_SCHEME_NONE,
                                        SCHEME_REQUIRED_LBA,
                                        SCHEME_MINIMUM_MEDIA_LBA, true);
    count = installer_storage_scheme_erase_warning_lines(
        &model, APPLIANCE_LANGUAGE_RU, lines);
    CHECK(count > 0U &&
              rendered_contains(
                  count,
                  "Содержимое диска прочитать не удалось") &&
              !rendered_contains(count, "contents of this disk"),
          "the unknown-contents line comes from the Russian catalog column");
}

static void test_the_summary_renders_the_contracted_plan(void)
{
    struct installer_discovery_report report;
    struct installer_storage_summary summary;
    char record[INSTALLER_STORAGE_RECORD_BYTES];

    memset(&report, 0, sizeof(report));
    report.firmware_mode = INSTALLER_FIRMWARE_UEFI;
    add_system_disk(&report, 1U, "SAMSUNG MZVLB512HAJQ S3Z9NX0M", 240U);
    report.oss[0].id = 1U;
    report.oss[0].present = true;
    report.oss[0].kind = INSTALLER_OS_LINUX;
    report.oss[0].partition_id = 2U;
    report.os_count = 1U;

    installer_storage_summary_build(&summary, &report, 1U,
                                    INSTALLER_STORAGE_SCHEME_NONE,
                                    INSTALLER_STORAGE_SCHEME_ALONGSIDE, 0U);
    CHECK(summary.status == INSTALLER_STORAGE_SUMMARY_OK &&
              summary.layout.mode ==
                  INSTALLER_LAYOUT_CONTRACT_MODE_DUAL_BOOT &&
              summary.layout.esp_reused && summary.layout.role_count == 4U &&
              installer_layout_contract_validate_summary(
                  &report, 1U, &summary.layout) == INSTALLER_LAYOUT_CONTRACT_OK,
          "the alongside summary is the contracted dual-boot plan the writer "
          "executes");
    CHECK(installer_storage_summary_lines(&summary, &report,
                                          APPLIANCE_LANGUAGE_DEFAULT,
                                          lines) > 0U &&
              screen_has("Confirm the installation plan") &&
              screen_contains("Install alongside the existing system") &&
              screen_contains(
                  "Will be deleted: nothing: the existing partitions stay") &&
              screen_contains("EFI system partition: reused") &&
              screen_contains("Menu entries added: HFOC appliance") &&
              screen_contains("ENTER confirm this plan") &&
              screen_contains("Declared: none") &&
              screen_contains("Chosen: alongside"),
          "the summary names the scheme, what stays, the reused ESP and the "
          "menu entry it adds");
    CHECK(installer_storage_summary_record(&summary, record, sizeof(record)) >
                  0U &&
              strstr(record, "scheme=ALONGSIDE declared=NONE status=OK") !=
                  NULL &&
              strstr(record, "deleted=none esp_reused=1") != NULL &&
              strstr(record, "writes=0") != NULL,
          "the reading says which plan was shown and that nothing was written");

    installer_storage_summary_build(&summary, &report, 1U,
                                    INSTALLER_STORAGE_SCHEME_ALONGSIDE,
                                    INSTALLER_STORAGE_SCHEME_ERASE, 0U);
    CHECK(summary.status == INSTALLER_STORAGE_SUMMARY_OK &&
              summary.layout.mode ==
                  INSTALLER_LAYOUT_CONTRACT_MODE_CLEAN_WHOLE_DISK &&
              !summary.layout.esp_reused &&
              summary.layout.role_count == 4U &&
              summary.existing_system_count == 0U,
          "the erase summary is the contracted clean plan");
    CHECK(installer_storage_summary_lines(&summary, &report,
                                          APPLIANCE_LANGUAGE_DEFAULT,
                                          lines) > 0U &&
              screen_contains("Will be deleted: everything on this disk") &&
              screen_contains("Declared: alongside") &&
              screen_contains("Chosen: erase"),
          "the destructive summary says the whole disk goes, names both "
          "sources and does not count the system it deletes as kept");
    CHECK(installer_storage_summary_record(&summary, record, sizeof(record)) >
                  0U &&
              strstr(record, "scheme=ERASE declared=ALONGSIDE status=OK") !=
                  NULL &&
              strstr(record, "deleted=whole-disk") != NULL,
          "the erase reading names the declaration the operator overrode");

    installer_storage_summary_build(&summary, &report, 1U,
                                    INSTALLER_STORAGE_SCHEME_NONE,
                                    INSTALLER_STORAGE_SCHEME_MANUAL, 2U);
    CHECK(summary.status == INSTALLER_STORAGE_SUMMARY_OK &&
              summary.layout.mode == INSTALLER_LAYOUT_CONTRACT_MODE_RELEASE &&
              summary.layout.released_index == 2U &&
              installer_layout_contract_validate_summary(
                  &report, 1U, &summary.layout) == INSTALLER_LAYOUT_CONTRACT_OK,
          "the manual summary is the contracted release plan for the partition "
          "the operator gave up");
    CHECK(installer_storage_summary_lines(&summary, &report,
                                          APPLIANCE_LANGUAGE_DEFAULT,
                                          lines) > 0U &&
              screen_contains("Will be deleted: partition 2") &&
              screen_contains("Chosen: manual"),
          "the manual summary names the partition that is removed");
    CHECK(installer_storage_summary_record(&summary, record, sizeof(record)) >
                  0U &&
              strstr(record, "deleted=released-partition") != NULL &&
              strstr(record, "released_index=2") != NULL,
          "the manual reading names the released partition");
}

/*
 * ITEM-517, requirement 3: the summary of a legacy-BIOS machine must not
 * promise an EFI System Partition.  The erase scheme renders the BIOS layout
 * (BIOS boot partition and FAT boot volume, no ESP); the preserving schemes
 * reuse a UEFI ESP and are refused by name instead of being drawn.
 */
static void test_a_bios_summary_promises_no_esp(void)
{
    struct installer_discovery_report report;
    struct installer_storage_summary summary;
    char record[INSTALLER_STORAGE_RECORD_BYTES];

    memset(&report, 0, sizeof(report));
    report.version = INSTALLER_HARDWARE_DISCOVERY_VERSION;
    report.read_only = true;
    report.scan_complete = true;
    report.firmware_mode = INSTALLER_FIRMWARE_BIOS_LEGACY;
    add_disk(&report, 1U, "TOSHIBA MQ01ABD100",
             INSTALLER_DISK_CONTROLLER_SATA,
             INSTALLER_PARTITION_TABLE_NONE, 1U, false);

    installer_storage_summary_build(&summary, &report, 1U,
                                    INSTALLER_STORAGE_SCHEME_NONE,
                                    INSTALLER_STORAGE_SCHEME_ERASE, 0U);
    CHECK(summary.status == INSTALLER_STORAGE_SUMMARY_OK &&
              summary.layout.mode ==
                  INSTALLER_LAYOUT_CONTRACT_MODE_CLEAN_WHOLE_DISK_BIOS &&
              summary.layout.role_count ==
                  INSTALLER_LAYOUT_CONTRACT_BIOS_ROLE_COUNT &&
              installer_layout_contract_validate_summary(
                  &report, 1U, &summary.layout) ==
                  INSTALLER_LAYOUT_CONTRACT_OK,
          "the BIOS erase summary is the contracted BIOS layout");
    CHECK(installer_storage_summary_lines(&summary, &report,
                                          APPLIANCE_LANGUAGE_DEFAULT,
                                          lines) > 0U &&
              screen_contains("BIOS boot partition") &&
              screen_contains("FAT boot volume") &&
              !screen_contains("EFI system partition"),
          "the BIOS summary names both BIOS roles and promises no ESP");
    CHECK(installer_storage_summary_record(&summary, record, sizeof(record)) >
                  0U &&
              strstr(record, "esp_reused=0") != NULL &&
              strstr(record, "roles=5") != NULL,
          "the BIOS reading publishes five roles and no reused ESP");

    installer_storage_summary_build(&summary, &report, 1U,
                                    INSTALLER_STORAGE_SCHEME_NONE,
                                    INSTALLER_STORAGE_SCHEME_ALONGSIDE, 0U);
    CHECK(summary.status == INSTALLER_STORAGE_SUMMARY_REFUSED &&
              summary.contract_status ==
                  INSTALLER_LAYOUT_CONTRACT_PLAN_REJECTED,
          "a BIOS preserve scheme is refused, not drawn");
    CHECK(installer_storage_summary_lines(&summary, &report,
                                          APPLIANCE_LANGUAGE_DEFAULT,
                                          lines) > 0U &&
              screen_contains("The plan cannot be built:") &&
              !screen_contains("EFI system partition"),
          "the BIOS refusal names the contract status and promises no ESP");
}

static void test_a_summary_that_cannot_be_planned_is_refused(void)
{
    struct installer_discovery_report report;
    struct installer_storage_summary summary;

    memset(&report, 0, sizeof(report));
    report.firmware_mode = INSTALLER_FIRMWARE_UEFI;
    add_system_disk(&report, 1U, "SERIAL1", 240U);
    report.oss[0].id = 1U;
    report.oss[0].present = true;
    report.oss[0].kind = INSTALLER_OS_LINUX;
    report.oss[0].partition_id = 2U;
    report.os_count = 1U;
    /* A released partition smaller than the three roles cannot plan. */
    report.partitions[1U].length_lba = 4096U;
    installer_storage_summary_build(&summary, &report, 1U,
                                    INSTALLER_STORAGE_SCHEME_NONE,
                                    INSTALLER_STORAGE_SCHEME_MANUAL, 2U);
    CHECK(summary.status == INSTALLER_STORAGE_SUMMARY_REFUSED &&
              summary.contract_status == INSTALLER_LAYOUT_CONTRACT_NO_RESERVE,
          "a released partition too small for the roles is refused by the "
          "contract, not resized");
    CHECK(installer_storage_summary_lines(&summary, &report,
                                          APPLIANCE_LANGUAGE_DEFAULT,
                                          lines) > 0U &&
              screen_contains("The plan cannot be built:") &&
              screen_contains("NO_RESERVE"),
          "the refused page names the contract refusal");
    installer_storage_summary_build(&summary, &report, 1U,
                                    INSTALLER_STORAGE_SCHEME_NONE,
                                    INSTALLER_STORAGE_SCHEME_MANUAL, 0U);
    CHECK(summary.status == INSTALLER_STORAGE_SUMMARY_INVALID,
          "manual without a released partition is invalid, not a plan");
    installer_storage_summary_build(&summary, &report, 1U,
                                    INSTALLER_STORAGE_SCHEME_NONE,
                                    INSTALLER_STORAGE_SCHEME_NONE, 0U);
    CHECK(summary.status == INSTALLER_STORAGE_SUMMARY_INVALID,
          "no scheme is not a plan");
}

/* A line that reaches the buffer's last byte was cut by the renderer. */
static bool rendered_within_budget(size_t count)
{
    size_t index;

    for (index = 0U; index < count && index < INSTALLER_STORAGE_LINES_MAX;
         ++index) {
        if (strlen(lines[index]) + 1U >= INSTALLER_STORAGE_LINE_BYTES) {
            return false;
        }
    }
    return true;
}

static void test_the_scheme_and_summary_speak_russian(void)
{
    struct installer_discovery_report report;
    struct installer_storage_scheme_model model;
    struct installer_storage_summary summary;
    size_t count;

    memset(&report, 0, sizeof(report));
    report.firmware_mode = INSTALLER_FIRMWARE_UEFI;
    add_system_disk(&report, 1U, "SERIAL1", 240U);
    installer_storage_schemes_inventory(&model, &report, 1U,
                                        INSTALLER_STORAGE_SCHEME_NONE,
                                        SCHEME_REQUIRED_LBA,
                                        SCHEME_MINIMUM_MEDIA_LBA, false);
    count = installer_storage_scheme_lines(&model, APPLIANCE_LANGUAGE_RU,
                                           lines);
    CHECK(count > 0U && rendered_has(count, "Выберите схему установки") &&
              rendered_contains(count, "Стереть весь диск и установить HFOC") &&
              rendered_contains(count, "Ручной разметки целиком"),
          "ITEM-514: the scheme list comes from the Russian catalog column");
    CHECK(!rendered_contains(count, "Erase the whole disk") &&
              !rendered_contains(count, "No full manual partitioning"),
          "ITEM-514: the Russian scheme list keeps no English prose");
    CHECK(rendered_within_budget(count),
          "ITEM-514: no Russian scheme line is truncated by the line budget");
    CHECK(installer_storage_scheme_lines(&model, (enum appliance_language)7,
                                         lines) == 1U &&
              rendered_has(1U, "INSTALLER TRANSLATION MISSING"),
          "an unknown language renders the named refusal, not half a screen");

    installer_storage_summary_build(&summary, &report, 1U,
                                    INSTALLER_STORAGE_SCHEME_NONE,
                                    INSTALLER_STORAGE_SCHEME_ERASE, 0U);
    count = installer_storage_summary_lines(&summary, &report,
                                            APPLIANCE_LANGUAGE_RU, lines);
    CHECK(count > 0U && rendered_has(count, "Подтвердите план установки") &&
              rendered_contains(count, "Будет удалено: всё на этом диске"),
          "ITEM-514: the summary comes from the Russian catalog column");
    CHECK(!rendered_contains(count, "Confirm the installation plan") &&
              !rendered_contains(count, "everything on this disk"),
          "ITEM-514: the Russian summary keeps no English prose");
    CHECK(rendered_within_budget(count),
          "ITEM-514: no Russian summary line is truncated by the line budget");
    CHECK(installer_storage_summary_lines(&summary, &report,
                                          (enum appliance_language)7,
                                          lines) == 1U &&
              rendered_has(1U, "INSTALLER TRANSLATION MISSING"),
          "an unknown language renders the named refusal on the summary");
}

/*
 * ITEM-518: the observed storage report - the facts the refusal page and the
 * `storage` command print so `NO_DISK` stops being a code the person at the
 * machine cannot act on.  Two refusal forms are driven here without a machine:
 * a machine whose class scan matched nothing, and a machine whose controller
 * came up but whose disk was dropped.  The page renders every line within the
 * declared budget, and a catalog that cannot answer the labels is the named
 * refusal, never half a page.
 */
static char observed_lines[APPLIANCE_STORAGE_REPORT_LINES_MAX]
                          [APPLIANCE_STORAGE_REPORT_LINE_BYTES];

static bool observed_contains(size_t count, const char *needle)
{
    size_t index;

    for (index = 0U;
         index < count && index < APPLIANCE_STORAGE_REPORT_LINES_MAX;
         ++index) {
        if (strstr(observed_lines[index], needle) != NULL) {
            return true;
        }
    }
    return false;
}

static bool observed_within_budget(size_t count)
{
    size_t index;

    for (index = 0U;
         index < count && index < APPLIANCE_STORAGE_REPORT_LINES_MAX;
         ++index) {
        if (strlen(observed_lines[index]) + 1U >=
            APPLIANCE_STORAGE_REPORT_LINE_BYTES) {
            return false;
        }
    }
    return true;
}

static void observed_add_disk(struct appliance_storage_report *report,
                              const char *controller, const char *identity,
                              uint32_t block_size, uint64_t block_count,
                              enum appliance_storage_report_reason reason)
{
    struct appliance_storage_report_disk *disk =
        &report->disks[report->disk_count];

    memset(disk, 0, sizeof(*disk));
    disk->present = true;
    disk->controller = controller;
    if (identity != NULL) {
        size_t index = 0U;

        while (identity[index] != '\0' &&
               index + 1U < sizeof(disk->identity)) {
            disk->identity[index] = identity[index];
            ++index;
        }
        disk->identity[index] = '\0';
    }
    disk->block_size = block_size;
    disk->block_count = block_count;
    disk->reason = reason;
    if (appliance_storage_report_reason_usable(reason)) {
        ++report->accepted;
    }
    report->disk_count += 1U;
}

static void test_the_observed_report_names_an_empty_machine(void)
{
    struct appliance_storage_report report;
    size_t count;

    appliance_storage_report_init(&report);
    report.probed = true;
    count = appliance_storage_report_lines(&report, APPLIANCE_LANGUAGE_EN,
                                           observed_lines);
    CHECK(count >= 3U && observed_contains(count, "Observed storage") &&
              observed_contains(
                  count,
                  "controllers: class_ahci=0 class_nvme=0 brought_up=0 "
                  "failed=0 reason=NO_CONTROLLER") &&
              observed_contains(count, "disks: found=0 accepted=0") &&
              !observed_contains(count, "disk 1:"),
          "an empty machine names the controllers it did not find");
    CHECK(observed_within_budget(count),
          "ITEM-518: no observed line is truncated by the line budget");

    count = appliance_storage_report_lines(&report, APPLIANCE_LANGUAGE_RU,
                                           observed_lines);
    CHECK(count >= 3U && observed_contains(count, "Наблюдённое хранилище") &&
              observed_contains(count, "контроллеры") &&
              observed_contains(count, "NO_CONTROLLER"),
          "ITEM-518: the page's labels come from the Russian catalog column");
}

static void test_the_observed_report_names_each_refusal_form(void)
{
    struct appliance_storage_report report;
    char record[512U];
    size_t count;

    appliance_storage_report_init(&report);
    report.probed = true;
    report.class_ahci = 2U;
    report.class_nvme = 1U;
    /* Two controllers matched, one disk came up: a bring-up failure. */
    report.brought_up = 1U;
    report.nvme_status = 1;
    report.nvme_step = "READY";
    report.nvme_wait_ms = 1000U;
    observed_add_disk(&report, "AHCI", "KINGSTON-SA400", 512U,
                      UINT64_C(488397168),
                      APPLIANCE_STORAGE_REPORT_OK);
    observed_add_disk(&report, "NVME", "", 512U, UINT64_C(1000),
                      APPLIANCE_STORAGE_REPORT_EMPTY_IDENTITY);
    observed_add_disk(&report, "NVME", "NVME-1", 4096U, UINT64_C(1000),
                      APPLIANCE_STORAGE_REPORT_UNSUPPORTED_BLOCK_SIZE);
    observed_add_disk(&report, "NVME", "NVME-2", 512U, 0U,
                      APPLIANCE_STORAGE_REPORT_ZERO_GEOMETRY);
    count = appliance_storage_report_lines(&report, APPLIANCE_LANGUAGE_EN,
                                           observed_lines);
    CHECK(observed_contains(count,
                            "controllers: class_ahci=2 class_nvme=1 "
                            "brought_up=1 failed=2") &&
              observed_contains(count,
                                "disk 1: controller=AHCI identity=KINGSTON") &&
              observed_contains(count, "reason=OK") &&
              observed_contains(count, "reason=EMPTY_IDENTITY") &&
              observed_contains(count, "reason=UNSUPPORTED_BLOCK_SIZE") &&
              observed_contains(count, "reason=ZERO_GEOMETRY") &&
              observed_contains(count, "nvme: status=NOT_OK") &&
              observed_contains(count, "step=READY wait_ms=1000"),
          "each disk and each refusal form is named by its own token");
    CHECK(report.accepted == 1U &&
              observed_contains(count, "disks: found=4 accepted=1"),
          "the accepted count is the model's rule, not a second opinion");
    CHECK(observed_within_budget(count),
          "ITEM-518: the full observed page stays within the line budget");

    CHECK(appliance_storage_report_record(&report, record, sizeof(record)) >
                  0U &&
              strstr(record, "[STORAGE-REPORT] probed=1 class_ahci=2 "
                             "class_nvme=1 brought_up=1 found=4 accepted=1") !=
                  NULL &&
              strstr(record, "d=1:AHCI/512/488397168/OK") != NULL &&
              strstr(record, "d=2:NVME/512/1000/EMPTY_IDENTITY") != NULL,
          "the one machine record carries the same facts for the journal");
}

static void test_the_observed_report_says_not_probed(void)
{
    struct appliance_storage_report report;
    size_t count;

    appliance_storage_report_init(&report);
    count = appliance_storage_report_lines(&report, APPLIANCE_LANGUAGE_EN,
                                           observed_lines);
    CHECK(count == 2U &&
              observed_contains(count, "Observed storage") &&
              observed_contains(count, "storage was not probed") &&
              observed_contains(count, "state=NOT_PROBED"),
          "a machine that was never probed says so instead of zeroes");
}

static void test_the_observed_report_names_a_failed_probe(void)
{
    struct appliance_storage_report report;
    char record[512U];
    size_t count;

    /*
     * ITEM-540: a probe that was entered and failed is not the same machine as
     * one nobody probed, and the page has to name the primary error - the
     * class scan that stopped, the PCI status and the device - or the operator
     * still cannot tell a broken search from a missing controller.
     */
    appliance_storage_report_init(&report);
    report.probe_attempted = true;
    report.probe_stage = "NVME_SCAN";
    report.probe_status = "PCI_INVALID_BAR";
    report.failure_known = true;
    report.failure_bus = 2U;
    report.failure_device = 0x1FU;
    report.failure_function = 1U;
    report.failure_bar = 5U;
    report.failure_vendor_id = 0x8086U;
    report.failure_device_id = 0x1E03U;

    count = appliance_storage_report_lines(&report, APPLIANCE_LANGUAGE_EN,
                                           observed_lines);
    CHECK(count == 3U &&
              observed_contains(count, "storage probe failed") &&
              observed_contains(count, "state=PROBE_FAILED stage=NVME_SCAN "
                                       "status=PCI_INVALID_BAR") &&
              observed_contains(count, "failed at bdf=02:1f.1 vendor=8086 "
                                       "device=1e03 bar=5") &&
              !observed_contains(count, "state=NOT_PROBED"),
          "a failed search names its stage, primary status and device");
    CHECK(observed_within_budget(count),
          "ITEM-540: the failed-probe lines fit the page budget");

    count = appliance_storage_report_lines(&report, APPLIANCE_LANGUAGE_RU,
                                           observed_lines);
    CHECK(observed_contains(count, "опрос хранилища завершился ошибкой"),
          "the failed-probe label is translated like the rest of the page");

    CHECK(appliance_storage_report_record(&report, record, sizeof(record)) >
                  0U &&
              strstr(record, "probe_attempted=1 probe_stage=NVME_SCAN "
                             "probe_status=PCI_INVALID_BAR") != NULL &&
              strstr(record, "failed_bdf=02:1f.1 vendor=8086 device=1e03 "
                             "bar=5") != NULL,
          "the machine record carries the primary error too");
}

static void test_the_observed_report_refuses_a_missing_translation(void)
{
    struct appliance_storage_report report;
    size_t count;

    appliance_storage_report_init(&report);
    report.probed = true;
    count = appliance_storage_report_lines(&report,
                                           (enum appliance_language)7,
                                           observed_lines);
    CHECK(count == 1U && observed_contains(1U,
                                           "STORAGE TRANSLATION MISSING"),
          "an unknown language renders the named refusal, not half a page");
}

/*
 * ITEM-541: the refusal page on a machine with no serial.  It has to answer
 * the two questions that decide what the operator does next - did the machine
 * recognise the medium it booted from, and is the journal bound to it - print
 * the bounded tail of the journal's RAM ring, and still keep the way out on
 * the page.  The builder is pure, so every form is driven here.
 */
static char refusal_lines[INSTALLER_STORAGE_LINES_MAX]
                         [INSTALLER_STORAGE_LINE_BYTES];

static bool refusal_contains(size_t count, const char *needle)
{
    size_t index;

    for (index = 0U; index < count && index < INSTALLER_STORAGE_LINES_MAX;
         ++index) {
        if (strstr(refusal_lines[index], needle) != NULL) {
            return true;
        }
    }
    return false;
}

static size_t refusal_line_index(size_t count, const char *needle)
{
    size_t index;

    for (index = 0U; index < count && index < INSTALLER_STORAGE_LINES_MAX;
         ++index) {
        if (strstr(refusal_lines[index], needle) != NULL) {
            return index;
        }
    }
    return count;
}

static bool refusal_within_budget(size_t count)
{
    size_t index;

    if (count > INSTALLER_STORAGE_LINES_MAX) {
        return false;
    }
    for (index = 0U; index < count; ++index) {
        if (strlen(refusal_lines[index]) + 1U >
            INSTALLER_STORAGE_LINE_BYTES) {
            return false;
        }
    }
    return true;
}

static void test_the_refusal_page_names_medium_journal_and_tail(void)
{
    struct appliance_storage_report report;
    struct installer_refusal_evidence evidence;
    const char *press_key = appliance_language_string(
        APPLIANCE_LANGUAGE_EN, "installer.refusal.press_key");
    size_t count;

    appliance_storage_report_init(&report);
    report.probed = true;
    memset(&evidence, 0, sizeof(evidence));
    evidence.medium_state = "UNAVAILABLE";
    evidence.medium_reason = "NO_DEVICE";
    evidence.journal_state = "UNBOUND";
    evidence.journal_reason = "NO_PLACEMENT";
    evidence.journal_pending = 2U;
    evidence.journal_dropped = 1U;
    evidence.journal_refused = 3U;
    evidence.records[0] = "[INSTALL-LOG] newest";
    evidence.records[1] = "[STORAGE-REPORT] older";
    evidence.record_count = 2U;

    count = installer_storage_refusal_lines("NO_DISK", &report, &evidence,
                                            APPLIANCE_LANGUAGE_EN,
                                            refusal_lines);
    CHECK(refusal_within_budget(count) && press_key != NULL &&
              refusal_contains(count, press_key),
          "ITEM-541: the refusal page stays in budget and keeps the way out");
    CHECK(refusal_contains(
              count,
              "[INSTALL-MEDIUM] state=UNAVAILABLE reason=NO_DEVICE"),
          "the page names the medium verdict and the reason behind it");
    CHECK(refusal_contains(
              count,
              "[INSTALL-LOG] state=UNBOUND reason=NO_PLACEMENT pending=2 "
              "dropped=1 refused=3"),
          "the page names the journal state, reason and counters");
    CHECK(refusal_contains(count, "[INSTALL-LOG-TAIL] shown=2") &&
              refusal_contains(count, "[STORAGE-REPORT] older") &&
              refusal_contains(count, "[INSTALL-LOG] newest"),
          "the page prints the bounded ring tail");
    CHECK(refusal_line_index(count, "older") <
              refusal_line_index(count, "newest"),
          "the tail reads oldest first");
    CHECK(refusal_contains(count, "NO_DISK") &&
              refusal_contains(count, "Observed storage"),
          "the refusal and the observed storage keep their lines");
}

static void test_the_refusal_page_says_not_probed_instead_of_nothing(void)
{
    struct installer_refusal_evidence evidence;
    size_t count;

    memset(&evidence, 0, sizeof(evidence));
    evidence.medium_state = "UNCHECKED";
    evidence.medium_reason = "NOT_PROBED";
    evidence.journal_state = "UNBOUND";
    evidence.journal_reason = "NONE";

    /* F-6: a report the page was never handed is named, never silent. */
    count = installer_storage_refusal_lines("NO_DISK", NULL, &evidence,
                                            APPLIANCE_LANGUAGE_EN,
                                            refusal_lines);
    CHECK(refusal_contains(count, "[STORAGE-REPORT] state=NOT_PROBED") &&
              refusal_within_budget(count),
          "ITEM-541: an empty report says state=NOT_PROBED, not nothing");

    /* No evidence at all is its own honest form. */
    count = installer_storage_refusal_lines("NO_DISK", NULL, NULL,
                                            APPLIANCE_LANGUAGE_EN,
                                            refusal_lines);
    CHECK(refusal_contains(count,
                           "[INSTALL-MEDIUM] state=UNCHECKED reason=NOT_PROBED") &&
              refusal_contains(count,
                               "[INSTALL-LOG] state=UNBOUND reason=NONE") &&
              refusal_contains(count, "[INSTALL-LOG-TAIL] shown=0"),
          "ITEM-541: missing evidence is named as unchecked, not invented");
}

static void test_the_refusal_page_bounds_the_ring_tail(void)
{
    struct installer_refusal_evidence evidence;
    static const char *records[INSTALLER_STORAGE_REFUSAL_TAIL_MAX] = {
        "record-12", "record-11", "record-10", "record-09",
        "record-08", "record-07", "record-06", "record-05",
        "record-04", "record-03", "record-02", "record-01",
    };
    const char *press_key = appliance_language_string(
        APPLIANCE_LANGUAGE_EN, "installer.refusal.press_key");
    size_t count;
    size_t index;

    memset(&evidence, 0, sizeof(evidence));
    evidence.medium_state = "MATCH";
    evidence.medium_reason = "OK";
    evidence.journal_state = "UNAVAILABLE";
    evidence.journal_reason = "NOT_RESERVED";
    evidence.journal_pending = INSTALLER_STORAGE_REFUSAL_TAIL_MAX;
    for (index = 0U; index < INSTALLER_STORAGE_REFUSAL_TAIL_MAX; ++index) {
        evidence.records[index] = records[index];
    }
    evidence.record_count = INSTALLER_STORAGE_REFUSAL_TAIL_MAX;

    count = installer_storage_refusal_lines("NO_DISK", NULL, &evidence,
                                            APPLIANCE_LANGUAGE_EN,
                                            refusal_lines);
    CHECK(refusal_within_budget(count) &&
              refusal_contains(count, "[INSTALL-LOG-TAIL] shown=12"),
          "ITEM-541: the full ring tail fits the page budget");
    CHECK(refusal_line_index(count, "record-01") <
              refusal_line_index(count, "record-12") &&
              press_key != NULL &&
              refusal_contains(count, press_key),
          "the bounded tail is complete, oldest first, and the way out stays");
}

int main(void)
{
    test_an_empty_machine_is_honest();
    test_one_disk_is_still_not_chosen_for_the_operator();
    test_an_unreadable_disk_is_never_selectable();
    test_navigation_stops_at_the_ends();
    test_a_choice_by_id_obeys_the_same_rules();
    test_a_machine_with_an_existing_system_says_so();
    test_the_model_is_bounded();
    test_the_row_names_the_disk_the_way_the_machine_did();
    test_a_disk_without_a_name_shows_its_identity_alone();
    test_the_screen_can_say_why_it_is_asking();
    test_the_warning_page_says_what_will_be_erased();
    test_the_warning_page_refuses_to_warn_about_nothing();
    test_confirmation_needs_a_choice_and_a_new_choice_clears_it();
    test_an_unreadable_row_cannot_be_confirmed();
    test_the_keys_the_screen_advertises_work();
    test_names_are_stable();
    test_the_partition_screen_names_what_may_be_given_up();
    test_russian_disk_list();
    test_disk_list_missing_translation_is_named();
    test_russian_disk_warning();
    test_russian_partition_screens();
    test_warning_screens_missing_translation_is_named();
    test_the_scheme_list_names_every_scheme();
    test_a_row_that_cannot_be_used_is_named();
    test_the_declaration_is_a_default_not_a_lock();
    test_a_disk_below_the_minimum_refuses_erase();
    test_an_unreadable_table_is_not_an_empty_disk();
    test_the_erase_page_names_unknown_contents();
    test_the_summary_renders_the_contracted_plan();
    test_a_bios_summary_promises_no_esp();
    test_a_summary_that_cannot_be_planned_is_refused();
    test_the_scheme_and_summary_speak_russian();
    test_the_observed_report_names_an_empty_machine();
    test_the_observed_report_names_each_refusal_form();
    test_the_observed_report_says_not_probed();
    test_the_observed_report_names_a_failed_probe();
    test_the_observed_report_refuses_a_missing_translation();
    test_the_refusal_page_names_medium_journal_and_tail();
    test_the_refusal_page_says_not_probed_instead_of_nothing();
    test_the_refusal_page_bounds_the_ring_tail();

    if (failures != 0) {
        fprintf(stderr, "Installer storage host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Installer storage host tests: ALL OK");
    return 0;
}
