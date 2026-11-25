#ifndef HFOC_APPLIANCE_STORAGE_REPORT_H
#define HFOC_APPLIANCE_STORAGE_REPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "appliance_language.h"

/*
 * ITEM-518: what the storage probe saw, said out loud.
 *
 * The installer refused a machine with `NO_DISK` and printed only the code
 * (`installer_storage_screen.c`), so the operator could not tell three
 * different findings apart: no controller matched the two scanned classes, a
 * controller matched and its disk never came up, or the disk came up and was
 * dropped (empty identity, zero geometry, unsupported block size).  The
 * evidence already existed - `appliance_production_storage_evidence()` counts
 * the controllers and the disks, the discovery statuses say what happened to
 * each disk - but it only reached the serial journal.
 *
 * This module is the bounded, allocation-free text of that evidence, and it is
 * pure: the probe fills the model, the refusal page and the console `storage`
 * command render it, and a host proof drives both forms without a machine.
 * Machine values (counts, status names, identities) stay machine text; only
 * the labels come from the one language catalog.
 */

#define APPLIANCE_STORAGE_REPORT_VERSION 1U
#define APPLIANCE_STORAGE_REPORT_LINE_BYTES 128U
#define APPLIANCE_STORAGE_REPORT_LINES_MAX 24U
#define APPLIANCE_STORAGE_REPORT_DISK_CAPACITY 8U
#define APPLIANCE_STORAGE_REPORT_IDENTITY_LENGTH 32U
#define APPLIANCE_STORAGE_REPORT_MODEL_LENGTH 41U
/* No BAR was the failure when the probe stopped before or outside a decode. */
#define APPLIANCE_STORAGE_REPORT_BAR_NONE 0xFFU

/*
 * Why a disk the probe brought up cannot be used, or `OK` when it can.  The
 * names are the stable tokens the page, the console command and the serial
 * record quote; they are not translated.
 */
enum appliance_storage_report_reason {
    APPLIANCE_STORAGE_REPORT_OK = 0,
    /* Matched controllers, no disk: the class scan found nothing at all. */
    APPLIANCE_STORAGE_REPORT_NO_CONTROLLER = 1,
    /* Controllers matched, none of them produced a disk. */
    APPLIANCE_STORAGE_REPORT_BRINGUP_FAILED = 2,
    /* The disk came up without an identity, so no plan may name it. */
    APPLIANCE_STORAGE_REPORT_EMPTY_IDENTITY = 3,
    /* The disk came up with no geometry. */
    APPLIANCE_STORAGE_REPORT_ZERO_GEOMETRY = 4,
    /* The disk's block size is not the one the installer reads. */
    APPLIANCE_STORAGE_REPORT_UNSUPPORTED_BLOCK_SIZE = 5,
    /* The block device the controller produced failed validation. */
    APPLIANCE_STORAGE_REPORT_INVALID_DEVICE = 6,
    /* The first sector did not read; the disk stays a candidate. */
    APPLIANCE_STORAGE_REPORT_IO_ERROR = 7,
    /* The first sector read but carries no usable partition table. */
    APPLIANCE_STORAGE_REPORT_INVALID_PARTITION_TABLE = 8,
    /* A disk the probe never enumerated (the discovery refused it). */
    APPLIANCE_STORAGE_REPORT_NOT_PROBED = 9,
};

struct appliance_storage_report_disk {
    bool present;
    /*
     * The controller family, as the machine words the rest of the system uses
     * ("AHCI", "NVME"); borrowed, never copied, so it must be a literal.
     */
    const char *controller;
    char identity[APPLIANCE_STORAGE_REPORT_IDENTITY_LENGTH];
    char model[APPLIANCE_STORAGE_REPORT_MODEL_LENGTH];
    uint32_t block_size;
    uint64_t block_count;
    enum appliance_storage_report_reason reason;
};

struct appliance_storage_report {
    uint32_t version;
    /* False until a probe filled the model: the report says so, never zeroes. */
    bool probed;
    /*
     * ITEM-540: `probed == false` covered two different machines - one whose
     * storage was never probed at all and one whose probe ran and failed.  A
     * probe that was entered records `probe_attempted`, the class scan that
     * stopped (`probe_stage`), the primary PCI status (`probe_status`) and,
     * when the failure was a decode, the device it belonged to.  The report
     * then says `state=PROBE_FAILED` with those facts instead of the silence
     * that read like "nobody looked".
     */
    bool probe_attempted;
    const char *probe_stage;
    const char *probe_status;
    bool failure_known;
    unsigned int failure_bus;
    unsigned int failure_device;
    unsigned int failure_function;
    unsigned int failure_bar;
    uint16_t failure_vendor_id;
    uint16_t failure_device_id;
    /* Controllers the class scan matched. */
    size_t class_ahci;
    size_t class_nvme;
    /* Disks those controllers brought up. */
    size_t brought_up;
    /* Disks a plan may name: reason OK, or a table-level annotation only. */
    size_t accepted;
    int nvme_status;
    const char *nvme_step;
    uint32_t nvme_wait_ms;
    size_t disk_count;
    struct appliance_storage_report_disk disks[
        APPLIANCE_STORAGE_REPORT_DISK_CAPACITY];
};

/* The empty report: not probed, nothing found, nothing invented. */
void appliance_storage_report_init(struct appliance_storage_report *report);
/* The reason's stable token.  Never NULL. */
const char *appliance_storage_report_reason_string(
    enum appliance_storage_report_reason reason);
/*
 * Is this reason a disk a plan may name?  OK, and the two table-level
 * annotations (a table that did not read is unknown, not rejected: the erase
 * scheme exists for exactly that disk), are usable; the rest are not.
 */
bool appliance_storage_report_reason_usable(
    enum appliance_storage_report_reason reason);
/*
 * Render the bounded operator page.  Every line fits
 * APPLIANCE_STORAGE_REPORT_LINE_BYTES including its terminator; a model that
 * does not fit the line count is truncated at the last whole line, never in
 * the middle of one.  A catalog that cannot answer the labels renders the one
 * named refusal line instead of half a page, exactly like the installer pages.
 * Returns the number of lines written (0 when `report` or the line array is
 * NULL).
 */
size_t appliance_storage_report_lines(
    const struct appliance_storage_report *report,
    enum appliance_language language,
    char lines[APPLIANCE_STORAGE_REPORT_LINES_MAX]
               [APPLIANCE_STORAGE_REPORT_LINE_BYTES]);
/*
 * One machine line for the serial journal, the same facts in one record.  The
 * per-disk part is bounded by the disk capacity.  Returns the length written
 * without the terminator, and the output is always terminated; a capacity that
 * cannot hold the whole record keeps its prefix, never a partial byte.
 */
size_t appliance_storage_report_record(
    const struct appliance_storage_report *report,
    char *output,
    size_t capacity);

#endif
