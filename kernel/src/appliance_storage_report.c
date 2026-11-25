#include "appliance_storage_report.h"

#include "appliance_language.h"

#include <stddef.h>
#include <string.h>

/*
 * ITEM-518: the bounded text of the storage evidence.
 *
 * The page this renders is read by a person standing at a machine that has no
 * serial capture, so the facts have to be on the screen: the controllers the
 * class scan matched, the disks they brought up, and why a disk that came up
 * cannot be used.  The renderer is pure - no probe, no device, no catalog
 * mutation - so the host proof drives every refusal form, and the machine's
 * own numbers come from the same model the serial record quotes.
 *
 * Nothing here writes a disk or decides anything: the reason tokens describe
 * what already happened, and `appliance_storage_report_reason_usable()` is the
 * only opinion, used to count the disks a plan may still name.
 */

/* One machine record: the aggregates and every disk, under the console bound. */
#define APPLIANCE_STORAGE_REPORT_RECORD_BYTES 512U

/*
 * One bounded text builder, two capacities: the page's lines are short, the
 * machine record is long, and both refuse to write past the buffer they were
 * given instead of cutting a field in half.
 */
struct appliance_storage_report_buffer {
    char *text;
    size_t capacity;
    size_t length;
};

static void report_buffer_begin(struct appliance_storage_report_buffer *buffer,
                                char *text,
                                size_t capacity)
{
    buffer->text = text;
    buffer->capacity = capacity;
    buffer->length = 0U;
    if (capacity > 0U) {
        text[0] = '\0';
    }
}

static void report_buffer_text(struct appliance_storage_report_buffer *buffer,
                               const char *text)
{
    if (text == NULL) {
        return;
    }
    while (*text != '\0' && buffer->length + 1U < buffer->capacity) {
        buffer->text[buffer->length++] = *text++;
    }
    buffer->text[buffer->length] = '\0';
}

static void report_buffer_u64(struct appliance_storage_report_buffer *buffer,
                              uint64_t value)
{
    char digits[20U];
    size_t count = 0U;

    do {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U);
    while (count > 0U && buffer->length + 1U < buffer->capacity) {
        buffer->text[buffer->length++] = digits[--count];
    }
    buffer->text[buffer->length] = '\0';
}

static void report_buffer_i64(struct appliance_storage_report_buffer *buffer,
                              int64_t value)
{
    if (value < 0) {
        report_buffer_text(buffer, "-");
        report_buffer_u64(buffer, (uint64_t)(-value));
        return;
    }
    report_buffer_u64(buffer, (uint64_t)value);
}

/* ITEM-540: PCI identities are read as hexadecimal, as the bus numbers them. */
static void report_buffer_hex(struct appliance_storage_report_buffer *buffer,
                              uint64_t value,
                              unsigned int min_digits)
{
    static const char digits[] = "0123456789abcdef";
    char text[16U];
    size_t count = 0U;

    do {
        text[count++] = digits[value & 0xFU];
        value >>= 4U;
    } while (value != 0U || count < min_digits);
    while (count > 0U && buffer->length + 1U < buffer->capacity) {
        buffer->text[buffer->length++] = text[--count];
    }
    buffer->text[buffer->length] = '\0';
}

static void report_buffer_identity(
    struct appliance_storage_report_buffer *buffer,
    const char *identity)
{
    size_t index = 0U;

    if (identity == NULL) {
        return;
    }
    while (identity[index] != '\0' &&
           index + 1U < APPLIANCE_STORAGE_REPORT_IDENTITY_LENGTH) {
        if (buffer->length + 1U < buffer->capacity) {
            buffer->text[buffer->length++] = identity[index];
        }
        ++index;
    }
    buffer->text[buffer->length] = '\0';
}

/*
 * The catalog answers the labels; a key the catalog cannot answer is the named
 * refusal, never half a page and never English prose on a Russian screen.
 */
static const char *report_label(enum appliance_language language,
                                const char *key,
                                bool *ok)
{
    const char *text = appliance_language_string(language, key);

    if (text == NULL || text[0] == '\0') {
        *ok = false;
        return "";
    }
    return text;
}

void appliance_storage_report_init(struct appliance_storage_report *report)
{
    if (report == NULL) {
        return;
    }
    memset(report, 0, sizeof(*report));
    report->version = APPLIANCE_STORAGE_REPORT_VERSION;
    report->probed = false;
}

const char *appliance_storage_report_reason_string(
    enum appliance_storage_report_reason reason)
{
    switch (reason) {
    case APPLIANCE_STORAGE_REPORT_OK:
        return "OK";
    case APPLIANCE_STORAGE_REPORT_NO_CONTROLLER:
        return "NO_CONTROLLER";
    case APPLIANCE_STORAGE_REPORT_BRINGUP_FAILED:
        return "BRINGUP_FAILED";
    case APPLIANCE_STORAGE_REPORT_EMPTY_IDENTITY:
        return "EMPTY_IDENTITY";
    case APPLIANCE_STORAGE_REPORT_ZERO_GEOMETRY:
        return "ZERO_GEOMETRY";
    case APPLIANCE_STORAGE_REPORT_UNSUPPORTED_BLOCK_SIZE:
        return "UNSUPPORTED_BLOCK_SIZE";
    case APPLIANCE_STORAGE_REPORT_INVALID_DEVICE:
        return "INVALID_DEVICE";
    case APPLIANCE_STORAGE_REPORT_IO_ERROR:
        return "IO_ERROR";
    case APPLIANCE_STORAGE_REPORT_INVALID_PARTITION_TABLE:
        return "INVALID_PARTITION_TABLE";
    case APPLIANCE_STORAGE_REPORT_NOT_PROBED:
        return "NOT_PROBED";
    }
    return "UNKNOWN";
}

bool appliance_storage_report_reason_usable(
    enum appliance_storage_report_reason reason)
{
    /*
     * A table that did not read is not a disk that cannot be used: the erase
     * scheme exists for exactly that disk, and the model refuses to select only
     * the identity and geometry it never read.  Those two annotations are
     * `IO_ERROR` and `INVALID_PARTITION_TABLE`.
     */
    return reason == APPLIANCE_STORAGE_REPORT_OK ||
           reason == APPLIANCE_STORAGE_REPORT_IO_ERROR ||
           reason == APPLIANCE_STORAGE_REPORT_INVALID_PARTITION_TABLE;
}

static size_t report_render_disk(
    const struct appliance_storage_report *report,
    size_t disk_index,
    const char *disk_label,
    char lines[APPLIANCE_STORAGE_REPORT_LINES_MAX]
               [APPLIANCE_STORAGE_REPORT_LINE_BYTES])
{
    const struct appliance_storage_report_disk *disk =
        &report->disks[disk_index];
    struct appliance_storage_report_buffer line;
    char text[APPLIANCE_STORAGE_REPORT_LINE_BYTES];

    report_buffer_begin(&line, text, sizeof(text));
    report_buffer_text(&line, "  ");
    report_buffer_text(&line, disk_label);
    report_buffer_text(&line, " ");
    report_buffer_u64(&line, (uint64_t)(disk_index + 1U));
    report_buffer_text(&line, ": controller=");
    report_buffer_text(&line, disk->controller != NULL ? disk->controller
                                                       : "UNKNOWN");
    report_buffer_text(&line, " identity=");
    report_buffer_identity(&line, disk->identity);
    memcpy(lines[0], line.text, line.length + 1U);

    report_buffer_begin(&line, text, sizeof(text));
    report_buffer_text(&line, "    blocks=");
    report_buffer_u64(&line, disk->block_count);
    report_buffer_text(&line, " bs=");
    report_buffer_u64(&line, (uint64_t)disk->block_size);
    report_buffer_text(&line, " reason=");
    report_buffer_text(&line,
                       appliance_storage_report_reason_string(disk->reason));
    memcpy(lines[1], line.text, line.length + 1U);
    return 2U;
}

size_t appliance_storage_report_lines(
    const struct appliance_storage_report *report,
    enum appliance_language language,
    char lines[APPLIANCE_STORAGE_REPORT_LINES_MAX]
               [APPLIANCE_STORAGE_REPORT_LINE_BYTES])
{
    struct appliance_storage_report_buffer line;
    char text[APPLIANCE_STORAGE_REPORT_LINE_BYTES];
    size_t count = 0U;
    size_t index;
    size_t class_total;
    size_t failed;
    bool labels_ok = true;
    const char *title;
    const char *controllers;
    const char *disks;
    const char *disk_label;
    const char *not_probed;
    const char *probe_failed;

    if (report == NULL || lines == NULL) {
        return 0U;
    }
    title = report_label(language, "storage.report.title", &labels_ok);
    controllers = report_label(language, "storage.report.controllers",
                               &labels_ok);
    disks = report_label(language, "storage.report.disks", &labels_ok);
    disk_label = report_label(language, "storage.report.disk", &labels_ok);
    not_probed = report_label(language, "storage.report.not_probed",
                              &labels_ok);
    probe_failed = report_label(language, "storage.report.probe_failed",
                                &labels_ok);
    if (!labels_ok) {
        report_buffer_begin(&line, text, sizeof(text));
        report_buffer_text(&line, "STORAGE TRANSLATION MISSING");
        memcpy(lines[0], line.text, line.length + 1U);
        return 1U;
    }

    report_buffer_begin(&line, text, sizeof(text));
    report_buffer_text(&line, title);
    memcpy(lines[count++], line.text, line.length + 1U);

    if (!report->probed) {
        /*
         * ITEM-540: two machines used to read the same here.  One was never
         * probed; the other was probed and the search itself failed, and the
         * operator needs that primary error - the stage, the PCI status and
         * the device - not the silence that read like "nobody looked".
         */
        if (!report->probe_attempted) {
            report_buffer_begin(&line, text, sizeof(text));
            report_buffer_text(&line, "  ");
            report_buffer_text(&line, not_probed);
            report_buffer_text(&line, " state=NOT_PROBED");
            memcpy(lines[count++], line.text, line.length + 1U);
            return count;
        }
        report_buffer_begin(&line, text, sizeof(text));
        report_buffer_text(&line, "  ");
        report_buffer_text(&line, probe_failed);
        report_buffer_text(&line, " state=PROBE_FAILED stage=");
        report_buffer_text(&line, report->probe_stage != NULL
                                      ? report->probe_stage
                                      : "NONE");
        report_buffer_text(&line, " status=");
        report_buffer_text(&line, report->probe_status != NULL
                                      ? report->probe_status
                                      : "PCI_UNKNOWN");
        memcpy(lines[count++], line.text, line.length + 1U);
        if (report->failure_known) {
            report_buffer_begin(&line, text, sizeof(text));
            report_buffer_text(&line, "  failed at bdf=");
            report_buffer_hex(&line, report->failure_bus, 2U);
            report_buffer_text(&line, ":");
            report_buffer_hex(&line, report->failure_device, 2U);
            report_buffer_text(&line, ".");
            report_buffer_hex(&line, report->failure_function, 1U);
            report_buffer_text(&line, " vendor=");
            report_buffer_hex(&line, report->failure_vendor_id, 4U);
            report_buffer_text(&line, " device=");
            report_buffer_hex(&line, report->failure_device_id, 4U);
            if (report->failure_bar != APPLIANCE_STORAGE_REPORT_BAR_NONE) {
                report_buffer_text(&line, " bar=");
                report_buffer_u64(&line, (uint64_t)report->failure_bar);
            }
            memcpy(lines[count++], line.text, line.length + 1U);
        }
        return count;
    }

    class_total = report->class_ahci + report->class_nvme;
    failed = class_total > report->brought_up
                 ? class_total - report->brought_up
                 : 0U;
    report_buffer_begin(&line, text, sizeof(text));
    report_buffer_text(&line, "  ");
    report_buffer_text(&line, controllers);
    report_buffer_text(&line, ": class_ahci=");
    report_buffer_u64(&line, (uint64_t)report->class_ahci);
    report_buffer_text(&line, " class_nvme=");
    report_buffer_u64(&line, (uint64_t)report->class_nvme);
    report_buffer_text(&line, " brought_up=");
    report_buffer_u64(&line, (uint64_t)report->brought_up);
    report_buffer_text(&line, " failed=");
    report_buffer_u64(&line, (uint64_t)failed);
    if (class_total == 0U) {
        report_buffer_text(&line, " reason=");
        report_buffer_text(
            &line,
            appliance_storage_report_reason_string(
                APPLIANCE_STORAGE_REPORT_NO_CONTROLLER));
    } else if (report->brought_up == 0U) {
        report_buffer_text(&line, " reason=");
        report_buffer_text(
            &line,
            appliance_storage_report_reason_string(
                APPLIANCE_STORAGE_REPORT_BRINGUP_FAILED));
    }
    memcpy(lines[count++], line.text, line.length + 1U);

    /*
     * The NVMe bring-up detail gets its own line: `nvme_status`, the step it
     * stopped at and the wait that step cost are what tell "the controller
     * never reported ready" from "the driver never waited long enough", and one
     * combined line would not fit the page's line budget.
     */
    if (report->class_nvme > 0U) {
        report_buffer_begin(&line, text, sizeof(text));
        report_buffer_text(&line, "  nvme: status=");
        report_buffer_text(&line,
                           report->nvme_status != 0 ? "NOT_OK" : "NVME_OK");
        report_buffer_text(&line, " nvme_status=");
        report_buffer_i64(&line, (int64_t)report->nvme_status);
        report_buffer_text(&line, " step=");
        report_buffer_text(&line,
                           report->nvme_step != NULL ? report->nvme_step
                                                     : "NONE");
        report_buffer_text(&line, " wait_ms=");
        report_buffer_u64(&line, (uint64_t)report->nvme_wait_ms);
        memcpy(lines[count++], line.text, line.length + 1U);
    }

    report_buffer_begin(&line, text, sizeof(text));
    report_buffer_text(&line, "  ");
    report_buffer_text(&line, disks);
    report_buffer_text(&line, ": found=");
    report_buffer_u64(&line, (uint64_t)report->disk_count);
    report_buffer_text(&line, " accepted=");
    report_buffer_u64(&line, (uint64_t)report->accepted);
    memcpy(lines[count++], line.text, line.length + 1U);

    for (index = 0U; index < report->disk_count &&
                     index < APPLIANCE_STORAGE_REPORT_DISK_CAPACITY;
         ++index) {
        if (!report->disks[index].present) {
            continue;
        }
        if (count + 2U > APPLIANCE_STORAGE_REPORT_LINES_MAX) {
            break;
        }
        count += report_render_disk(report, index, disk_label, lines + count);
    }
    return count;
}

size_t appliance_storage_report_record(
    const struct appliance_storage_report *report,
    char *output,
    size_t capacity)
{
    struct appliance_storage_report_buffer record;
    char text[APPLIANCE_STORAGE_REPORT_RECORD_BYTES];
    size_t index;

    if (report == NULL || output == NULL || capacity == 0U) {
        return 0U;
    }
    report_buffer_begin(&record, text, sizeof(text));
    report_buffer_text(&record, "[STORAGE-REPORT] probed=");
    report_buffer_text(&record, report->probed ? "1" : "0");
    report_buffer_text(&record, " class_ahci=");
    report_buffer_u64(&record, (uint64_t)report->class_ahci);
    report_buffer_text(&record, " class_nvme=");
    report_buffer_u64(&record, (uint64_t)report->class_nvme);
    report_buffer_text(&record, " brought_up=");
    report_buffer_u64(&record, (uint64_t)report->brought_up);
    report_buffer_text(&record, " found=");
    report_buffer_u64(&record, (uint64_t)report->disk_count);
    report_buffer_text(&record, " accepted=");
    report_buffer_u64(&record, (uint64_t)report->accepted);
    for (index = 0U; index < report->disk_count &&
                     index < APPLIANCE_STORAGE_REPORT_DISK_CAPACITY;
         ++index) {
        if (!report->disks[index].present) {
            continue;
        }
        report_buffer_text(&record, " d=");
        report_buffer_u64(&record, (uint64_t)(index + 1U));
        report_buffer_text(&record, ":");
        report_buffer_text(&record,
                           report->disks[index].controller != NULL
                               ? report->disks[index].controller
                               : "UNKNOWN");
        report_buffer_text(&record, "/");
        report_buffer_u64(&record,
                          (uint64_t)report->disks[index].block_size);
        report_buffer_text(&record, "/");
        report_buffer_u64(&record, report->disks[index].block_count);
        report_buffer_text(&record, "/");
        report_buffer_text(
            &record,
            appliance_storage_report_reason_string(
                report->disks[index].reason));
    }
    /*
     * ITEM-540: the probe's own verdict goes after the aggregates, so a
     * record written by a machine whose search failed names the stage, the
     * primary PCI status and the device it stopped on.
     */
    report_buffer_text(&record, " probe_attempted=");
    report_buffer_text(&record, report->probe_attempted ? "1" : "0");
    report_buffer_text(&record, " probe_stage=");
    report_buffer_text(&record, report->probe_stage != NULL
                                    ? report->probe_stage
                                    : "NONE");
    report_buffer_text(&record, " probe_status=");
    report_buffer_text(&record, report->probe_status != NULL
                                    ? report->probe_status
                                    : "PCI_UNKNOWN");
    if (report->failure_known) {
        report_buffer_text(&record, " failed_bdf=");
        report_buffer_hex(&record, report->failure_bus, 2U);
        report_buffer_text(&record, ":");
        report_buffer_hex(&record, report->failure_device, 2U);
        report_buffer_text(&record, ".");
        report_buffer_hex(&record, report->failure_function, 1U);
        report_buffer_text(&record, " vendor=");
        report_buffer_hex(&record, report->failure_vendor_id, 4U);
        report_buffer_text(&record, " device=");
        report_buffer_hex(&record, report->failure_device_id, 4U);
        if (report->failure_bar != APPLIANCE_STORAGE_REPORT_BAR_NONE) {
            report_buffer_text(&record, " bar=");
            report_buffer_u64(&record, (uint64_t)report->failure_bar);
        }
    }
    {
        size_t copy = record.length;

        if (copy > capacity - 1U) {
            copy = capacity - 1U;
        }
        memcpy(output, record.text, copy);
        output[copy] = '\0';
        return copy;
    }
}
